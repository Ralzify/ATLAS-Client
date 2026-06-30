#include "pch.h"
#include "../Public/Client.h"
#include "../Public/Configuration.h"
#include "../Public/Finders.h"
#include "../Public/GUI.h"
#include <atomic>
#include <array>
#include <thread>
#include <vector>

#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3d12.lib")

#include "../ImGui/imgui.h"
#include "../ImGui/imgui_impl_win32.h"
#include "../ImGui/imgui_impl_dx11.h"
#include "../ImGui/imgui_impl_dx12.h"

static ID3D11Device* g_pd3dDevice = nullptr;
static ID3D11DeviceContext* g_pd3dContext = nullptr;
static ID3D11RenderTargetView* g_mainRTV = nullptr;
static HWND g_hWnd = nullptr;
static bool g_ImGuiReady = false;

typedef HRESULT(__stdcall* Present_t)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT(__stdcall* ResizeBuffers_t)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
typedef void(STDMETHODCALLTYPE* ExecuteCommandLists_t)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
static Present_t OriginalPresent = nullptr;
static ResizeBuffers_t OriginalResizeBuffers = nullptr;
static ExecuteCommandLists_t OriginalExecuteCommandLists = nullptr;

static WNDPROC OriginalWndProc = nullptr;
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);
static LRESULT CALLBACK HookedWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

static std::atomic_bool g_DXHookInstalled = false;
static std::atomic_bool g_DX12QueueHookInstalled = false;
static bool g_LoggedUnsupportedRenderer = false;
static bool g_LoggedDX12QueueWait = false;
static bool g_WasVisible = false;

enum class ERenderBackend
{
    None,
    DX11,
    DX12
};

static ERenderBackend g_RenderBackend = ERenderBackend::None;

struct DX12FrameContext
{
    ID3D12CommandAllocator* CommandAllocator = nullptr;
    ID3D12Resource* RenderTarget = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE RtvHandle{};
};

static constexpr UINT kDX12SrvDescriptorCount = 64;

static ID3D12Device* g_pd3d12Device = nullptr;
static IDXGISwapChain3* g_pSwapChain3 = nullptr;
static ID3D12CommandQueue* g_pd3d12CommandQueue = nullptr;
static ID3D12DescriptorHeap* g_pd3d12RtvHeap = nullptr;
static ID3D12DescriptorHeap* g_pd3d12SrvHeap = nullptr;
static ID3D12GraphicsCommandList* g_pd3d12CommandList = nullptr;
static ID3D12Fence* g_pd3d12Fence = nullptr;
static HANDLE g_pd3d12FenceEvent = nullptr;
static UINT64 g_pd3d12FenceValue = 0;
static UINT g_dx12RtvDescriptorSize = 0;
static UINT g_dx12SrvDescriptorSize = 0;
static std::vector<DX12FrameContext> g_dx12Frames;
static std::array<bool, kDX12SrvDescriptorCount> g_dx12SrvUsed{};

template <typename T>
static void SafeRelease(T*& ptr)
{
    if (ptr)
    {
        ptr->Release();
        ptr = nullptr;
    }
}

static HWND CreateDummyWindow()
{
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"ATLAS_DXGI_DUMMY";

    RegisterClassExW(&wc);

    return CreateWindowExW(
        0,
        wc.lpszClassName,
        L"ATLAS",
        WS_OVERLAPPEDWINDOW,
        0,
        0,
        2,
        2,
        nullptr,
        nullptr,
        wc.hInstance,
        nullptr);
}

static void ReleaseMainRenderTarget()
{
    if (g_pd3dContext)
        g_pd3dContext->OMSetRenderTargets(0, nullptr, nullptr);

    if (g_mainRTV)
    {
        g_mainRTV->Release();
        g_mainRTV = nullptr;
    }
}

static bool CreateMainRenderTarget(IDXGISwapChain* pSwapChain)
{
    if (!g_pd3dDevice || !pSwapChain)
        return false;

    ID3D11Texture2D* pBackBuffer = nullptr;
    HRESULT hr = pSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&pBackBuffer);

    if (FAILED(hr) || !pBackBuffer)
        return false;

    hr = g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRTV);
    pBackBuffer->Release();

    return SUCCEEDED(hr);
}

static void InstallWndProc(HWND hWnd)
{
    if (!hWnd || OriginalWndProc)
        return;

    g_hWnd = hWnd;
    OriginalWndProc = (WNDPROC)SetWindowLongPtrW(g_hWnd, GWLP_WNDPROC, (LONG_PTR)HookedWndProc);
}

static void InitImGuiBase(HWND hWnd)
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;

    ImGui_ImplWin32_Init(hWnd);
    GUI_Init();
}

static void HandleOverlayInput()
{
    GUI_HandleInput();

    if (FGUI::bVisible != g_WasVisible)
    {
        if (FGUI::bVisible)
        {
            ClipCursor(nullptr);
            ReleaseCapture();
            ShowCursor(TRUE);
        }
        else
        {
            ShowCursor(FALSE);
            if (g_hWnd)
                SetForegroundWindow(g_hWnd);
        }

        g_WasVisible = FGUI::bVisible;
    }

    ImGui::GetIO().MouseDrawCursor = FGUI::bVisible;
}

static bool InitDX11(IDXGISwapChain* pSwapChain)
{
    if (FAILED(pSwapChain->GetDevice(__uuidof(ID3D11Device), (void**)&g_pd3dDevice)))
        return false;

    g_pd3dDevice->GetImmediateContext(&g_pd3dContext);

    DXGI_SWAP_CHAIN_DESC sd{};
    pSwapChain->GetDesc(&sd);
    InstallWndProc(sd.OutputWindow);

    CreateMainRenderTarget(pSwapChain);
    InitImGuiBase(sd.OutputWindow);

    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dContext);
    GUI_LoadTextures(g_pd3dDevice);

    g_RenderBackend = ERenderBackend::DX11;
    g_ImGuiReady = true;
    return true;
}

static void RenderDX11(IDXGISwapChain* pSwapChain)
{
    if (!g_mainRTV)
        CreateMainRenderTarget(pSwapChain);

    HandleOverlayInput();

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    GUI_Render();

    ImGui::EndFrame();
    ImGui::Render();

    if (g_mainRTV)
    {
        g_pd3dContext->OMSetRenderTargets(1, &g_mainRTV, nullptr);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    }
}

static void WaitForDX12Queue()
{
    if (!g_pd3d12CommandQueue || !g_pd3d12Fence || !g_pd3d12FenceEvent)
        return;

    const UINT64 fenceValue = ++g_pd3d12FenceValue;
    if (FAILED(g_pd3d12CommandQueue->Signal(g_pd3d12Fence, fenceValue)))
        return;

    if (g_pd3d12Fence->GetCompletedValue() < fenceValue)
    {
        if (SUCCEEDED(g_pd3d12Fence->SetEventOnCompletion(fenceValue, g_pd3d12FenceEvent)))
            WaitForSingleObject(g_pd3d12FenceEvent, INFINITE);
    }
}

static void ReleaseDX12RenderTargets()
{
    WaitForDX12Queue();

    SafeRelease(g_pd3d12CommandList);
    for (DX12FrameContext& frame : g_dx12Frames)
    {
        SafeRelease(frame.RenderTarget);
        SafeRelease(frame.CommandAllocator);
        frame.RtvHandle = {};
    }
    g_dx12Frames.clear();
    SafeRelease(g_pd3d12RtvHeap);
}

static bool CreateDX12SrvHeap()
{
    if (g_pd3d12SrvHeap)
        return true;

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.NumDescriptors = kDX12SrvDescriptorCount;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

    if (FAILED(g_pd3d12Device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&g_pd3d12SrvHeap))))
        return false;

    g_dx12SrvDescriptorSize = g_pd3d12Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    g_dx12SrvUsed.fill(false);
    return true;
}

static void DX12SrvDescriptorAlloc(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE* outCpuHandle, D3D12_GPU_DESCRIPTOR_HANDLE* outGpuHandle)
{
    D3D12_CPU_DESCRIPTOR_HANDLE cpuStart = g_pd3d12SrvHeap->GetCPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE gpuStart = g_pd3d12SrvHeap->GetGPUDescriptorHandleForHeapStart();

    for (UINT i = 0; i < kDX12SrvDescriptorCount; i++)
    {
        if (!g_dx12SrvUsed[i])
        {
            g_dx12SrvUsed[i] = true;
            outCpuHandle->ptr = cpuStart.ptr + (SIZE_T)i * g_dx12SrvDescriptorSize;
            outGpuHandle->ptr = gpuStart.ptr + (UINT64)i * g_dx12SrvDescriptorSize;
            return;
        }
    }

    OutputDebugStringW(L"ATLAS: DX12 SRV descriptor heap exhausted.\n");
    *outCpuHandle = {};
    *outGpuHandle = {};
}

static void DX12SrvDescriptorFree(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle, D3D12_GPU_DESCRIPTOR_HANDLE)
{
    if (!g_pd3d12SrvHeap || !cpuHandle.ptr)
        return;

    D3D12_CPU_DESCRIPTOR_HANDLE cpuStart = g_pd3d12SrvHeap->GetCPUDescriptorHandleForHeapStart();
    const SIZE_T offset = cpuHandle.ptr - cpuStart.ptr;
    const UINT index = (UINT)(offset / g_dx12SrvDescriptorSize);
    if (index < kDX12SrvDescriptorCount)
        g_dx12SrvUsed[index] = false;
}

static bool CreateDX12RenderTargets(IDXGISwapChain* pSwapChain)
{
    if (!g_pd3d12Device || !pSwapChain)
        return false;

    if (!g_pSwapChain3 && FAILED(pSwapChain->QueryInterface(IID_PPV_ARGS(&g_pSwapChain3))))
        return false;

    DXGI_SWAP_CHAIN_DESC sd{};
    if (FAILED(pSwapChain->GetDesc(&sd)) || sd.BufferCount == 0)
        return false;

    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{};
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.NumDescriptors = sd.BufferCount;
    rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

    if (FAILED(g_pd3d12Device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&g_pd3d12RtvHeap))))
        return false;

    g_dx12RtvDescriptorSize = g_pd3d12Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    g_dx12Frames.resize(sd.BufferCount);

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = g_pd3d12RtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < sd.BufferCount; i++)
    {
        DX12FrameContext& frame = g_dx12Frames[i];
        frame.RtvHandle = rtvHandle;

        if (FAILED(g_pd3d12Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&frame.CommandAllocator))))
            return false;

        if (FAILED(pSwapChain->GetBuffer(i, IID_PPV_ARGS(&frame.RenderTarget))))
            return false;

        g_pd3d12Device->CreateRenderTargetView(frame.RenderTarget, nullptr, frame.RtvHandle);
        rtvHandle.ptr += g_dx12RtvDescriptorSize;
    }

    if (FAILED(g_pd3d12Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_dx12Frames[0].CommandAllocator, nullptr, IID_PPV_ARGS(&g_pd3d12CommandList))))
        return false;
    g_pd3d12CommandList->Close();

    if (!g_pd3d12Fence && FAILED(g_pd3d12Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_pd3d12Fence))))
        return false;

    if (!g_pd3d12FenceEvent)
        g_pd3d12FenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    return g_pd3d12FenceEvent != nullptr;
}

static bool IsDX12SwapChain(IDXGISwapChain* pSwapChain)
{
    ID3D12Device* device = nullptr;
    const bool isDX12 = SUCCEEDED(pSwapChain->GetDevice(IID_PPV_ARGS(&device)));
    SafeRelease(device);
    return isDX12;
}

static bool InitDX12(IDXGISwapChain* pSwapChain)
{
    if (!g_pd3d12CommandQueue)
    {
        if (!g_LoggedDX12QueueWait)
        {
            OutputDebugStringW(L"ATLAS: DX12 swap chain detected; waiting for command queue capture.\n");
            g_LoggedDX12QueueWait = true;
        }
        return false;
    }

    if (FAILED(pSwapChain->GetDevice(IID_PPV_ARGS(&g_pd3d12Device))))
        return false;

    DXGI_SWAP_CHAIN_DESC sd{};
    pSwapChain->GetDesc(&sd);
    InstallWndProc(sd.OutputWindow);

    if (!CreateDX12SrvHeap() || !CreateDX12RenderTargets(pSwapChain))
        return false;

    InitImGuiBase(sd.OutputWindow);

    ImGui_ImplDX12_InitInfo initInfo{};
    initInfo.Device = g_pd3d12Device;
    initInfo.CommandQueue = g_pd3d12CommandQueue;
    initInfo.NumFramesInFlight = (int)g_dx12Frames.size();
    initInfo.RTVFormat = sd.BufferDesc.Format;
    initInfo.DSVFormat = DXGI_FORMAT_UNKNOWN;
    initInfo.SrvDescriptorHeap = g_pd3d12SrvHeap;
    initInfo.SrvDescriptorAllocFn = DX12SrvDescriptorAlloc;
    initInfo.SrvDescriptorFreeFn = DX12SrvDescriptorFree;

    if (!ImGui_ImplDX12_Init(&initInfo))
        return false;

    FGUI::LogoTexture = ImTextureID_Invalid;
    D3D12_CPU_DESCRIPTOR_HANDLE logoSrvCpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE logoSrvGpu{};
    DX12SrvDescriptorAlloc(&initInfo, &logoSrvCpu, &logoSrvGpu);
    GUI_LoadTexturesDX12(g_pd3d12Device, g_pd3d12CommandQueue, logoSrvCpu, logoSrvGpu);
    if (FGUI::LogoTexture == ImTextureID_Invalid && logoSrvCpu.ptr)
        DX12SrvDescriptorFree(&initInfo, logoSrvCpu, logoSrvGpu);

    g_RenderBackend = ERenderBackend::DX12;
    g_ImGuiReady = true;
    return true;
}

static void RenderDX12(IDXGISwapChain* pSwapChain)
{
    if (!g_pSwapChain3 || !g_pd3d12CommandQueue || g_dx12Frames.empty())
        return;

    const UINT frameIndex = g_pSwapChain3->GetCurrentBackBufferIndex() % (UINT)g_dx12Frames.size();
    DX12FrameContext& frame = g_dx12Frames[frameIndex];
    if (!frame.CommandAllocator || !frame.RenderTarget || !g_pd3d12CommandList)
        return;

    HandleOverlayInput();

    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    GUI_Render();

    ImGui::EndFrame();
    ImGui::Render();

    if (FAILED(frame.CommandAllocator->Reset()))
        return;
    if (FAILED(g_pd3d12CommandList->Reset(frame.CommandAllocator, nullptr)))
        return;

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = frame.RenderTarget;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    g_pd3d12CommandList->ResourceBarrier(1, &barrier);

    g_pd3d12CommandList->OMSetRenderTargets(1, &frame.RtvHandle, FALSE, nullptr);

    ID3D12DescriptorHeap* descriptorHeaps[] = { g_pd3d12SrvHeap };
    g_pd3d12CommandList->SetDescriptorHeaps(1, descriptorHeaps);
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), g_pd3d12CommandList);

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    g_pd3d12CommandList->ResourceBarrier(1, &barrier);

    if (SUCCEEDED(g_pd3d12CommandList->Close()))
    {
        ID3D12CommandList* commandLists[] = { g_pd3d12CommandList };
        g_pd3d12CommandQueue->ExecuteCommandLists(1, commandLists);
        WaitForDX12Queue();
    }
}

static LRESULT CALLBACK HookedWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (g_ImGuiReady && FGUI::bVisible)
    {
        ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam);

        if (msg == WM_INPUT)
        {
            BYTE rawBuffer[256];
            UINT rawSize = sizeof(rawBuffer);
            GetRawInputData((HRAWINPUT)lParam, RID_INPUT, rawBuffer, &rawSize, sizeof(RAWINPUTHEADER));
            return 0;
        }

        if (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP ||
            msg == WM_RBUTTONDOWN || msg == WM_RBUTTONUP ||
            msg == WM_MBUTTONDOWN || msg == WM_MBUTTONUP ||
            msg == WM_MOUSEWHEEL || msg == WM_MOUSEMOVE ||
            msg == WM_KEYDOWN || msg == WM_KEYUP ||
            msg == WM_CHAR || msg == WM_SYSKEYDOWN ||
            msg == WM_SYSKEYUP)
        {
            return 0;
        }
    }
    return CallWindowProcW(OriginalWndProc, hWnd, msg, wParam, lParam);
}

static HRESULT __stdcall HookedPresent(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags)
{
    if (!g_ImGuiReady)
    {
        const bool dx12SwapChain = IsDX12SwapChain(pSwapChain);

        if (!InitDX11(pSwapChain) && !InitDX12(pSwapChain) && !dx12SwapChain && !g_LoggedUnsupportedRenderer)
        {
            OutputDebugStringW(L"ATLAS: Present hook landed, but the swap chain is not D3D11 or D3D12.\n");
            g_LoggedUnsupportedRenderer = true;
        }
    }

    if (g_ImGuiReady)
    {
        if (g_RenderBackend == ERenderBackend::DX11)
            RenderDX11(pSwapChain);
        else if (g_RenderBackend == ERenderBackend::DX12)
            RenderDX12(pSwapChain);
    }

    return OriginalPresent(pSwapChain, SyncInterval, Flags);
}

static HRESULT __stdcall HookedResizeBuffers(IDXGISwapChain* pSwapChain, UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags)
{
    if (g_RenderBackend == ERenderBackend::DX12)
        ReleaseDX12RenderTargets();
    else
        ReleaseMainRenderTarget();

    HRESULT hr = OriginalResizeBuffers(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags);

    if (SUCCEEDED(hr) && g_ImGuiReady)
    {
        if (g_RenderBackend == ERenderBackend::DX12)
            CreateDX12RenderTargets(pSwapChain);
        else
            CreateMainRenderTarget(pSwapChain);
    }

    return hr;
}

static void STDMETHODCALLTYPE HookedExecuteCommandLists(ID3D12CommandQueue* queue, UINT numCommandLists, ID3D12CommandList* const* commandLists)
{
    if (queue && !g_pd3d12CommandQueue)
    {
        D3D12_COMMAND_QUEUE_DESC desc = queue->GetDesc();
        if (desc.Type == D3D12_COMMAND_LIST_TYPE_DIRECT)
        {
            queue->AddRef();
            g_pd3d12CommandQueue = queue;
        }
    }

    OriginalExecuteCommandLists(queue, numCommandLists, commandLists);
}

static bool GetSwapChainAddresses(Present_t* presentAddr, ResizeBuffers_t* resizeBuffersAddr)
{
    HWND dummyHwnd = CreateDummyWindow();
    if (!dummyHwnd)
        return false;

    D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_0;
    ID3D11Device* tmpDevice = nullptr;
    ID3D11DeviceContext* tmpContext = nullptr;
    IDXGISwapChain* tmpChain = nullptr;

    DXGI_SWAP_CHAIN_DESC scd{};
    scd.BufferCount = 1;
    scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferDesc.Width = 2;
    scd.BufferDesc.Height = 2;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = dummyHwnd;
    scd.SampleDesc.Count = 1;
    scd.Windowed = TRUE;
    scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &featureLevel, 1, D3D11_SDK_VERSION, &scd, &tmpChain, &tmpDevice, nullptr, &tmpContext);

    if (FAILED(hr))
    {
        DestroyWindow(dummyHwnd);
        return false;
    }

    void** vtable = *reinterpret_cast<void***>(tmpChain);
    if (presentAddr)
        *presentAddr = reinterpret_cast<Present_t>(vtable[8]);

    if (resizeBuffersAddr)
        *resizeBuffersAddr = reinterpret_cast<ResizeBuffers_t>(vtable[13]);

    tmpChain->Release();
    tmpContext->Release();
    tmpDevice->Release();
    DestroyWindow(dummyHwnd);

    return true;
}

static bool GetD3D12ExecuteCommandListsAddress(ExecuteCommandLists_t* executeCommandListsAddr)
{
    ID3D12Device* tmpDevice = nullptr;
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&tmpDevice))))
        return false;

    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;

    ID3D12CommandQueue* tmpQueue = nullptr;
    HRESULT hr = tmpDevice->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&tmpQueue));
    if (FAILED(hr))
    {
        tmpDevice->Release();
        return false;
    }

    void** vtable = *reinterpret_cast<void***>(tmpQueue);
    if (executeCommandListsAddr)
        *executeCommandListsAddr = reinterpret_cast<ExecuteCommandLists_t>(vtable[10]);

    tmpQueue->Release();
    tmpDevice->Release();
    return true;
}

static bool EnableHookIfAvailable(void* target)
{
    MH_STATUS status = MH_EnableHook(target);
    return status == MH_OK || status == MH_ERROR_ENABLED;
}

static bool InstallDXHook()
{
    if (g_DXHookInstalled && g_DX12QueueHookInstalled)
        return true;

    MH_STATUS initStatus = MH_Initialize();
    if (initStatus != MH_OK && initStatus != MH_ERROR_ALREADY_INITIALIZED)
        return false;

    if (!g_DXHookInstalled)
    {
        Present_t presentAddr = nullptr;
        ResizeBuffers_t resizeBuffersAddr = nullptr;

        if (!GetSwapChainAddresses(&presentAddr, &resizeBuffersAddr) || !presentAddr || !resizeBuffersAddr)
            return false;

        void* presentTarget = reinterpret_cast<void*>(presentAddr);
        void* resizeTarget = reinterpret_cast<void*>(resizeBuffersAddr);

        MH_STATUS presentStatus = MH_CreateHook(presentTarget, reinterpret_cast<void*>(HookedPresent), reinterpret_cast<void**>(&OriginalPresent));
        if (presentStatus != MH_OK && presentStatus != MH_ERROR_ALREADY_CREATED)
            return false;

        MH_STATUS resizeStatus = MH_CreateHook(resizeTarget, reinterpret_cast<void*>(HookedResizeBuffers), reinterpret_cast<void**>(&OriginalResizeBuffers));
        if (resizeStatus != MH_OK && resizeStatus != MH_ERROR_ALREADY_CREATED)
            return false;

        if (!EnableHookIfAvailable(presentTarget) || !EnableHookIfAvailable(resizeTarget))
            return false;

        g_DXHookInstalled = true;
    }

    if (!g_DX12QueueHookInstalled)
    {
        ExecuteCommandLists_t executeCommandListsAddr = nullptr;
        if (GetD3D12ExecuteCommandListsAddress(&executeCommandListsAddr) && executeCommandListsAddr)
        {
            void* executeTarget = reinterpret_cast<void*>(executeCommandListsAddr);
            MH_STATUS executeStatus = MH_CreateHook(executeTarget, reinterpret_cast<void*>(HookedExecuteCommandLists), reinterpret_cast<void**>(&OriginalExecuteCommandLists));
            if (executeStatus == MH_OK || executeStatus == MH_ERROR_ALREADY_CREATED)
                g_DX12QueueHookInstalled = EnableHookIfAvailable(executeTarget);
        }
        else
        {
            g_DX12QueueHookInstalled = true;
        }
    }

    return true;
}

static void InstallDXHookThread()
{
    while (!InstallDXHook())
        Sleep(250);
}

void ForceIris(uintptr_t IrisBool)
{
    const auto sizeOfImage = Memcury::PE::GetNTHeaders()->OptionalHeader.SizeOfImage;
    const auto scanBytes = reinterpret_cast<std::uint8_t*>(Memcury::PE::GetModuleBase());
    for (auto i = 0ul; i < sizeOfImage - 5; ++i)
    {
        if (scanBytes[i] == 0x83 || scanBytes[i] == 0x39)
        {
            if (Memcury::PE::Address(&scanBytes[i]).RelativeOffset(2, scanBytes[i] == 0x83).GetAs<void*>() == (void*)IrisBool)
                Utils::Patch<uint32_t>(__int64(&scanBytes[i]) + 2, 0x0);
        }
    }
}

void Main()
{
    std::thread(InstallDXHookThread).detach();

    SDK::Init();

    if (VersionInfo.EngineVersion >= 5.0)
    {
        auto RuntimeOptions = DefaultObjImpl("FortRuntimeOptions");

        if (RuntimeOptions)
        {
            auto offset = RuntimeOptions->GetOffset("bWaitForServerToBeInitializedBeforeTravelingFeatureEnabled");

            if (offset != -1)
                *(bool*)(__int64(RuntimeOptions) + offset) = false;
        }

        UKismetSystemLibrary::ExecuteConsoleCommand(UWorld::GetWorld(), FString(L"log LogFortUIDirector None"), nullptr);
    }

    if (VersionInfo.EngineVersion >= 5.1)
        UKismetSystemLibrary::ExecuteConsoleCommand(UWorld::GetWorld(), FString(L"net.AllowEncryption 0"), nullptr);

    if (VersionInfo.EngineVersion >= 5.3 && FConfiguration::bEnableIris)
    {
        UKismetSystemLibrary::ExecuteConsoleCommand(UWorld::GetWorld(), FString(L"log LogIris None"), nullptr);
        UKismetSystemLibrary::ExecuteConsoleCommand(UWorld::GetWorld(), FString(L"log LogIrisRpc None"), nullptr);
        UKismetSystemLibrary::ExecuteConsoleCommand(UWorld::GetWorld(), FString(L"log LogIrisBridge None"), nullptr);

        auto IrisBool = FindCVar<uint32_t>(L"net.Iris.UseIrisReplication");

        if (IrisBool) 
            *IrisBool = true;

        ForceIris(__int64(IrisBool));
    }

    if (VersionInfo.EngineVersion >= 5.4)
    {
        UKismetSystemLibrary::ExecuteConsoleCommand(UWorld::GetWorld(), FString(L"Fort.MME.TacticalSprint 0"), nullptr);
        UKismetSystemLibrary::ExecuteConsoleCommand(UWorld::GetWorld(), FString(L"Fort.MME.Hurdle 0"), nullptr);
        UKismetSystemLibrary::ExecuteConsoleCommand(UWorld::GetWorld(), FString(L"Fort.MME.Sliding 0"), nullptr);
        UKismetSystemLibrary::ExecuteConsoleCommand(UWorld::GetWorld(), FString(L"Fort.MME.Clambering 0"), nullptr);
    }

    Client::Init();
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved)
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
        std::thread(Main).detach();
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
    case DLL_PROCESS_DETACH:
        break;
    }
    return TRUE;
}
