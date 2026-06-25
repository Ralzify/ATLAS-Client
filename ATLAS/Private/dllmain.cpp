#include "pch.h"
#include "../Public/Client.h"
#include "../Public/Configuration.h"
#include "../Public/Finders.h"
#include "../Public/GUI.h"
#include <thread>

#include <d3d11.h>
#pragma comment(lib, "d3d11.lib")

#include "../ImGui/imgui.h"
#include "../ImGui/imgui_impl_win32.h"
#include "../ImGui/imgui_impl_dx11.h"

static ID3D11Device* g_pd3dDevice = nullptr;
static ID3D11DeviceContext* g_pd3dContext = nullptr;
static ID3D11RenderTargetView* g_mainRTV = nullptr;
static HWND g_hWnd = nullptr;
static bool g_ImGuiReady = false;

typedef HRESULT(__stdcall* Present_t)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT(__stdcall* ResizeBuffers_t)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
static Present_t OriginalPresent = nullptr;
static ResizeBuffers_t OriginalResizeBuffers = nullptr;

static WNDPROC OriginalWndProc = nullptr;
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

static bool g_WasVisible = false;

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
        if (SUCCEEDED(pSwapChain->GetDevice(__uuidof(ID3D11Device), (void**)&g_pd3dDevice)))
        {
            g_pd3dDevice->GetImmediateContext(&g_pd3dContext);

            DXGI_SWAP_CHAIN_DESC sd{};
            pSwapChain->GetDesc(&sd);
            g_hWnd = sd.OutputWindow;

            CreateMainRenderTarget(pSwapChain);

            OriginalWndProc = (WNDPROC)SetWindowLongPtrW(g_hWnd, GWLP_WNDPROC, (LONG_PTR)HookedWndProc);

            IMGUI_CHECKVERSION();
            ImGui::CreateContext();

            ImGuiIO& io = ImGui::GetIO();
            io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;

            ImGui_ImplWin32_Init(g_hWnd);
            ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dContext);

            GUI_Init();
            GUI_LoadTextures(g_pd3dDevice);

            g_ImGuiReady = true;
        }
    }

    if (g_ImGuiReady)
    {
        if (!g_mainRTV)
            CreateMainRenderTarget(pSwapChain);

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
                SetForegroundWindow(g_hWnd);
            }

            g_WasVisible = FGUI::bVisible;
        }

        ImGuiIO& io = ImGui::GetIO();
        io.MouseDrawCursor = FGUI::bVisible;

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

    return OriginalPresent(pSwapChain, SyncInterval, Flags);
}

static HRESULT __stdcall HookedResizeBuffers(IDXGISwapChain* pSwapChain, UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags)
{
    ReleaseMainRenderTarget();

    HRESULT hr = OriginalResizeBuffers(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags);

    if (SUCCEEDED(hr) && g_ImGuiReady)
        CreateMainRenderTarget(pSwapChain);

    return hr;
}

static bool GetSwapChainAddresses(Present_t* presentAddr, ResizeBuffers_t* resizeBuffersAddr)
{
    HWND dummyHwnd = FindWindowW(L"UnrealWindow", nullptr);

    if (!dummyHwnd) 
        dummyHwnd = GetForegroundWindow();

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
        return false;

    void** vtable = *reinterpret_cast<void***>(tmpChain);
    if (presentAddr)
        *presentAddr = reinterpret_cast<Present_t>(vtable[8]);

    if (resizeBuffersAddr)
        *resizeBuffersAddr = reinterpret_cast<ResizeBuffers_t>(vtable[13]);

    tmpChain->Release();
    tmpContext->Release();
    tmpDevice->Release();

    return true;
}

static void InstallDXHook()
{
    Present_t presentAddr = nullptr;
    ResizeBuffers_t resizeBuffersAddr = nullptr;

    if (!GetSwapChainAddresses(&presentAddr, &resizeBuffersAddr) || !presentAddr || !resizeBuffersAddr)
        return;

    MH_STATUS initStatus = MH_Initialize();
    if (initStatus != MH_OK && initStatus != MH_ERROR_ALREADY_INITIALIZED)
        return;

    MH_CreateHook(reinterpret_cast<void*>(presentAddr), reinterpret_cast<void*>(HookedPresent), reinterpret_cast<void**>(&OriginalPresent));
    MH_CreateHook(reinterpret_cast<void*>(resizeBuffersAddr), reinterpret_cast<void*>(HookedResizeBuffers), reinterpret_cast<void**>(&OriginalResizeBuffers));
    MH_EnableHook(reinterpret_cast<void*>(presentAddr));
    MH_EnableHook(reinterpret_cast<void*>(resizeBuffersAddr));
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
    InstallDXHook();

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
