#include "pch.h"
#include "../Public/Client.h"
#include "../Public/Configuration.h"
#include "../Public/Diagnostics.h"
#include "../Public/Finders.h"
#include "../Public/GUI.h"
#include <atomic>
#include <array>
#include <mutex>
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
static std::atomic_bool g_ImGuiReady = false;
static std::atomic<IDXGISwapChain*> g_PrimarySwapChain = nullptr;

typedef HRESULT(__stdcall* Present_t)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT(__stdcall* ResizeBuffers_t)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
typedef void(STDMETHODCALLTYPE* ExecuteCommandLists_t)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
static Present_t OriginalPresent = nullptr;
static ResizeBuffers_t OriginalResizeBuffers = nullptr;
static ExecuteCommandLists_t OriginalExecuteCommandLists = nullptr;

static WNDPROC OriginalWndProc = nullptr;
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);
static LRESULT CALLBACK HookedWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
static std::mutex g_RenderMutex;
static std::recursive_mutex g_ImGuiMutex;

enum class EInputOwner : uint8_t
{
    Unknown,
    Gameplay,
    Overlay
};

static std::array<std::atomic_uint8_t, 0xFF> g_LegacyInputOwners{};
static std::array<std::atomic_uint8_t, 0xFF> g_RawInputOwners{};
static std::array<std::atomic_bool, 0xFF> g_ImGuiLegacyDown{};
static std::atomic_uint g_SuppressedConsoleScanCode = 0;
static std::atomic_bool g_TranslatedF8Down = false;

static std::atomic_bool g_DXHookInstalled = false;
static std::atomic_bool g_DX12QueueHookInstalled = false;
static bool g_LoggedUnsupportedRenderer = false;
static bool g_LoggedDX12QueueWait = false;
static bool g_WasVisible = false;
static RECT g_PreOverlayCursorClip{};
static bool g_HasPreOverlayCursorClip = false;
static bool g_ShouldRestoreGameplayMouse = false;
// Defaults to false so a process cannot accept hotkeys before Main has
// classified it. The -nullrhi host keeps its server-side CheatManager setup,
// but it must never consume or dispatch GUI input.
static std::atomic_bool g_InteractiveInputEnabled = false;

namespace
{
    constexpr wchar_t TransportMappingName[] =
        L"Local\\Magnesium.Transport.v1";
    constexpr DWORD TransportMagic = 0x4D475450u;
    constexpr DWORD TransportSchema = 1u;
    constexpr DWORD TransportCommitted = 1u;
    constexpr DWORD TransportModeGenericLegacy = 0u;
    constexpr DWORD TransportModeIris = 1u;

    struct FTransportManifest
    {
        volatile LONG Sequence;
        DWORD Magic;
        DWORD Schema;
        DWORD StructSize;
        DWORD PublisherPid;
        DWORD FortniteVersionHundredths;
        DWORD ServerPort;
        DWORD Committed;
        DWORD Mode;
    };

    static_assert(sizeof(FTransportManifest) == 36);
    static_assert(offsetof(FTransportManifest, Sequence) == 0);
    static_assert(offsetof(FTransportManifest, Magic) == 4);
    static_assert(offsetof(FTransportManifest, Mode) == 32);

    struct FIrisPatchSite
    {
        uintptr_t DisplacementAddress = 0;
        uint32_t OriginalDisplacement = 0;
    };

    std::mutex IrisPolicyMutex;
    std::vector<FIrisPatchSite> IrisPatchSites;
    uint32_t* IrisCVar = nullptr;
    bool bIrisPatchSitesCaptured = false;
    bool bHeadlessHostProcess = false;

    DWORD GetFortniteVersionHundredths()
    {
        return static_cast<DWORD>(
            VersionInfo.FortniteVersion * 100.0 + 0.5);
    }

    bool CaptureIrisPatchSites(uintptr_t IrisBool)
    {
        if (bIrisPatchSitesCaptured)
            return !IrisPatchSites.empty();
        if (!IrisBool)
            return false;

        const auto SizeOfImage =
            Memcury::PE::GetNTHeaders()->OptionalHeader.SizeOfImage;
        const auto ScanBytes = reinterpret_cast<std::uint8_t*>(
            Memcury::PE::GetModuleBase());
        if (!ScanBytes || SizeOfImage < 7)
            return false;

        for (size_t Index = 0; Index + 7 <= SizeOfImage; ++Index)
        {
            const bool bCmpImmediate = ScanBytes[Index] == 0x83;
            const bool bCmpRegister = ScanBytes[Index] == 0x39;
            if (!bCmpImmediate && !bCmpRegister)
                continue;

            const auto Target = Memcury::PE::Address(&ScanBytes[Index])
                .RelativeOffset(2, bCmpImmediate ? 1u : 0u)
                .GetAs<void*>();
            if (Target != reinterpret_cast<void*>(IrisBool))
                continue;

            const auto DisplacementAddress = reinterpret_cast<uintptr_t>(
                &ScanBytes[Index + 2]);
            const auto Existing = std::find_if(
                IrisPatchSites.begin(),
                IrisPatchSites.end(),
                [DisplacementAddress](const FIrisPatchSite& Site)
                {
                    return Site.DisplacementAddress == DisplacementAddress;
                });
            if (Existing == IrisPatchSites.end())
            {
                IrisPatchSites.push_back({
                    DisplacementAddress,
                    *reinterpret_cast<uint32_t*>(DisplacementAddress)
                });
            }
        }

        bIrisPatchSitesCaptured = true;
        AtlasDiagnostics::WriteLine(
            "replication-policy iris-sites=%zu cvar=%p",
            IrisPatchSites.size(),
            reinterpret_cast<void*>(IrisBool));
        return !IrisPatchSites.empty();
    }

    bool ApplyIrisPolicyLocked(bool bUseIris)
    {
        if (bHeadlessHostProcess || VersionInfo.EngineVersion < 5.3)
            return VersionInfo.EngineVersion < 5.3;

        if (!IrisCVar)
            IrisCVar = FindCVar<uint32_t>(
                L"net.Iris.UseIrisReplication");
        if (!IrisCVar)
        {
            AtlasDiagnostics::WriteLine(
                "replication-policy failed mode=%s cvar=%p sites=%zu",
                bUseIris ? "Iris" : "GenericLegacy",
                static_cast<void*>(IrisCVar),
                IrisPatchSites.size());
            return false;
        }

        // Some UE5 cohorts honor the cvar directly and contain no forceable
        // compare sites. Preserve that supported path; on 27.11 the captured
        // sites are additionally restored for Generic or forced for Iris.
        CaptureIrisPatchSites(
            reinterpret_cast<uintptr_t>(IrisCVar));

        if (bUseIris)
        {
            *IrisCVar = 1u;
            for (const auto& Site : IrisPatchSites)
                Utils::Patch<uint32_t>(
                    Site.DisplacementAddress, 0u);
        }
        else
        {
            for (const auto& Site : IrisPatchSites)
            {
                Utils::Patch<uint32_t>(
                    Site.DisplacementAddress,
                    Site.OriginalDisplacement);
            }
            *IrisCVar = 0u;
        }

        FlushInstructionCache(
            GetCurrentProcess(), nullptr, 0);
        FConfiguration::bEnableIris = bUseIris;
        AtlasDiagnostics::WriteLine(
            "replication-policy applied mode=%s sites=%zu",
            bUseIris ? "Iris" : "GenericLegacy",
            IrisPatchSites.size());
        return true;
    }

    bool IsPublisherAlive(DWORD PublisherPid)
    {
        if (!PublisherPid || PublisherPid == GetCurrentProcessId())
            return false;

        HANDLE Process = OpenProcess(
            SYNCHRONIZE, FALSE, PublisherPid);
        if (!Process)
            return false;
        const DWORD WaitResult = WaitForSingleObject(Process, 0);
        CloseHandle(Process);
        return WaitResult == WAIT_TIMEOUT;
    }

    bool ReadLocalTransportManifest(
        DWORD ExpectedPort,
        bool& bOutUseIris,
        wchar_t* Error,
        size_t ErrorCapacity)
    {
        HANDLE Mapping = OpenFileMappingW(
            FILE_MAP_READ, FALSE, TransportMappingName);
        if (!Mapping)
        {
            _snwprintf_s(
                Error, ErrorCapacity, _TRUNCATE,
                L"Local server transport is not ready. Start the Magnesium match before joining.");
            return false;
        }

        auto Shared = static_cast<const FTransportManifest*>(
            MapViewOfFile(
                Mapping, FILE_MAP_READ, 0, 0,
                sizeof(FTransportManifest)));
        if (!Shared)
        {
            CloseHandle(Mapping);
            _snwprintf_s(
                Error, ErrorCapacity, _TRUNCATE,
                L"Could not read the local server transport policy.");
            return false;
        }

        FTransportManifest Snapshot{};
        bool bStable = false;
        for (int Attempt = 0; Attempt < 8; ++Attempt)
        {
            const LONG SequenceBefore = Shared->Sequence;
            if (!SequenceBefore || (SequenceBefore & 1))
            {
                SwitchToThread();
                continue;
            }

            Snapshot.Magic = Shared->Magic;
            Snapshot.Schema = Shared->Schema;
            Snapshot.StructSize = Shared->StructSize;
            Snapshot.PublisherPid = Shared->PublisherPid;
            Snapshot.FortniteVersionHundredths =
                Shared->FortniteVersionHundredths;
            Snapshot.ServerPort = Shared->ServerPort;
            Snapshot.Committed = Shared->Committed;
            Snapshot.Mode = Shared->Mode;
            MemoryBarrier();

            const LONG SequenceAfter = Shared->Sequence;
            if (SequenceBefore == SequenceAfter &&
                !(SequenceAfter & 1))
            {
                Snapshot.Sequence = SequenceAfter;
                bStable = true;
                break;
            }
        }

        UnmapViewOfFile(Shared);
        CloseHandle(Mapping);

        const bool bValid = bStable &&
            Snapshot.Magic == TransportMagic &&
            Snapshot.Schema == TransportSchema &&
            Snapshot.StructSize == sizeof(FTransportManifest) &&
            Snapshot.FortniteVersionHundredths == 2711u &&
            Snapshot.Committed == TransportCommitted &&
            (Snapshot.Mode == TransportModeGenericLegacy ||
                Snapshot.Mode == TransportModeIris) &&
            IsPublisherAlive(Snapshot.PublisherPid);
        if (!bValid)
        {
            _snwprintf_s(
                Error, ErrorCapacity, _TRUNCATE,
                L"The local server transport policy is invalid or stale. Restart the Magnesium host.");
            return false;
        }

        if (ExpectedPort != Snapshot.ServerPort)
        {
            _snwprintf_s(
                Error, ErrorCapacity, _TRUNCATE,
                L"The local server is using port %lu. Join 127.0.0.1:%lu instead.",
                Snapshot.ServerPort,
                Snapshot.ServerPort);
            return false;
        }

        bOutUseIris = Snapshot.Mode == TransportModeIris;
        AtlasDiagnostics::WriteLine(
            "replication-policy manifest pid=%lu version=%lu port=%lu mode=%s",
            Snapshot.PublisherPid,
            Snapshot.FortniteVersionHundredths,
            Snapshot.ServerPort,
            bOutUseIris ? "Iris" : "GenericLegacy");
        return true;
    }

    bool IsOpenCommand(
        const char* Command,
        bool& bOutLoopback,
        DWORD& OutPort)
    {
        bOutLoopback = false;
        OutPort = 7777u;
        if (!Command)
            return false;

        while (*Command && std::isspace(
            static_cast<unsigned char>(*Command)))
        {
            ++Command;
        }
        if (_strnicmp(Command, "open", 4) != 0 ||
            !std::isspace(static_cast<unsigned char>(Command[4])))
        {
            return false;
        }

        Command += 4;
        while (*Command && std::isspace(
            static_cast<unsigned char>(*Command)))
        {
            ++Command;
        }
        if (!*Command)
            return false;

        const char* End = Command;
        while (*End && !std::isspace(
            static_cast<unsigned char>(*End)) && *End != '?')
        {
            ++End;
        }
        std::string Endpoint(Command, End);
        if (Endpoint.empty())
            return false;

        std::string Host = Endpoint;
        const auto Colon = Endpoint.rfind(':');
        if (Colon != std::string::npos &&
            Endpoint.find(':') == Colon)
        {
            const std::string PortText = Endpoint.substr(Colon + 1);
            char* ParseEnd = nullptr;
            const unsigned long ParsedPort = std::strtoul(
                PortText.c_str(), &ParseEnd, 10);
            if (ParseEnd && *ParseEnd == '\0' &&
                ParsedPort > 0 && ParsedPort <= 65535)
            {
                OutPort = static_cast<DWORD>(ParsedPort);
                Host.resize(Colon);
            }
        }

        bOutLoopback = _stricmp(Host.c_str(), "127.0.0.1") == 0 ||
            _stricmp(Host.c_str(), "localhost") == 0 ||
            Host == "[::1]" || Host == "::1";
        return true;
    }
}

bool ATLAS_PrepareReplicationForOpen(
    const char* Command,
    bool bRemoteDurianLegacy,
    wchar_t* Error,
    size_t ErrorCapacity)
{
    bool bLoopback = false;
    DWORD Port = 7777u;
    if (!IsOpenCommand(Command, bLoopback, Port))
        return true;
    if (VersionInfo.EngineVersion < 5.3)
        return true;

    bool bUseIris = FConfiguration::bEnableIris;
    if (GetFortniteVersionHundredths() == 2711u)
    {
        if (bLoopback)
        {
            if (!ReadLocalTransportManifest(
                    Port, bUseIris, Error, ErrorCapacity))
            {
                return false;
            }
        }
        else
        {
            bUseIris = !bRemoteDurianLegacy;
        }
    }

    std::lock_guard<std::mutex> Lock(IrisPolicyMutex);
    if (ApplyIrisPolicyLocked(bUseIris))
        return true;

    _snwprintf_s(
        Error, ErrorCapacity, _TRUNCATE,
        L"Could not configure the client replication transport. Restart ATLAS and try again.");
    return false;
}

static UINT GetReleaseMouseCaptureMessage()
{
    static const UINT message =
        RegisterWindowMessageW(
            L"ATLAS.ReleaseMouseCapture");
    return message;
}

static bool GameWindowHasMouseCapture(HWND window)
{
    if (!window)
        return false;

    const DWORD windowThreadId =
        GetWindowThreadProcessId(window, nullptr);
    GUITHREADINFO info{};
    info.cbSize = sizeof(info);
    return windowThreadId != 0 &&
        GetGUIThreadInfo(windowThreadId, &info) != FALSE &&
        info.hwndCapture == window;
}

static bool PinAtlasModule()
{
    HMODULE module = nullptr;
    return GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(&g_InteractiveInputEnabled),
        &module) != FALSE;
}

static bool ContainsInsensitive(const wchar_t* text, const wchar_t* token)
{
    if (!text || !token || !*token)
        return false;

    const size_t tokenLength = wcslen(token);
    for (const wchar_t* current = text; *current; current++)
        if (_wcsnicmp(current, token, tokenLength) == 0)
            return true;

    return false;
}

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
    if (!g_InteractiveInputEnabled.load(std::memory_order_acquire))
    {
        GUI_HandleInput(false);
        return;
    }

    // Each injected process owns a different game window. Gate actions at
    // consumption time so a press queued just before an Alt+Tab cannot run in
    // a background instance.
    const bool windowActive = g_hWnd && GetForegroundWindow() == g_hWnd;
    GUI_HandleInput(windowActive);

    const bool overlayVisible = GUI_IsOverlayVisible();
    if (overlayVisible != g_WasVisible)
    {
        if (overlayVisible)
        {
            GUI_CancelGameplayMouseRestore();
            g_HasPreOverlayCursorClip =
                windowActive &&
                GetClipCursor(&g_PreOverlayCursorClip) != FALSE;
            RECT virtualDesktop{
                GetSystemMetrics(SM_XVIRTUALSCREEN),
                GetSystemMetrics(SM_YVIRTUALSCREEN),
                GetSystemMetrics(SM_XVIRTUALSCREEN) +
                    GetSystemMetrics(SM_CXVIRTUALSCREEN),
                GetSystemMetrics(SM_YVIRTUALSCREEN) +
                    GetSystemMetrics(SM_CYVIRTUALSCREEN)
            };
            const bool cursorWasConfined =
                g_HasPreOverlayCursorClip &&
                !EqualRect(
                    &g_PreOverlayCursorClip,
                    &virtualDesktop);
            CURSORINFO cursorInfo{};
            cursorInfo.cbSize = sizeof(cursorInfo);
            const bool cursorWasHidden =
                GetCursorInfo(&cursorInfo) != FALSE &&
                (cursorInfo.flags & CURSOR_SHOWING) == 0;
            g_ShouldRestoreGameplayMouse =
                windowActive &&
                (cursorWasConfined ||
                    cursorWasHidden ||
                    GameWindowHasMouseCapture(g_hWnd));
            ClipCursor(nullptr);
            const UINT releaseCaptureMessage =
                GetReleaseMouseCaptureMessage();
            if (g_hWnd && releaseCaptureMessage != 0)
            {
                // Mouse capture belongs to the HWND thread, which may differ
                // from Present's render thread. Release it in HookedWndProc.
                PostMessageW(
                    g_hWnd,
                    releaseCaptureMessage,
                    0,
                    0);
            }
            ShowCursor(TRUE);
        }
        else
        {
            ShowCursor(FALSE);
            if (windowActive && g_hWnd)
            {
                if (g_HasPreOverlayCursorClip)
                {
                    // Opening ATLAS temporarily removes Unreal's gameplay
                    // confinement. Restore the exact pre-overlay rectangle so
                    // the hardware cursor cannot escape before the game-thread
                    // input-mode restore runs.
                    ClipCursor(&g_PreOverlayCursorClip);
                }
                if (g_ShouldRestoreGameplayMouse)
                    GUI_RequestGameplayMouseRestore(g_hWnd);
            }
            g_HasPreOverlayCursorClip = false;
            g_ShouldRestoreGameplayMouse = false;
        }

        g_WasVisible = overlayVisible;
    }

    {
        std::lock_guard<std::recursive_mutex> lock(g_ImGuiMutex);
        ImGui::GetIO().MouseDrawCursor = overlayVisible;
    }
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
    g_ImGuiReady.store(true, std::memory_order_release);
    return true;
}

static void RenderDX11(IDXGISwapChain* pSwapChain)
{
    if (!g_mainRTV)
        CreateMainRenderTarget(pSwapChain);

    HandleOverlayInput();

    {
        std::lock_guard<std::recursive_mutex> lock(g_ImGuiMutex);
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
    g_ImGuiReady.store(true, std::memory_order_release);
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

    {
        std::lock_guard<std::recursive_mutex> lock(g_ImGuiMutex);
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
    }

    if (SUCCEEDED(g_pd3d12CommandList->Close()))
    {
        ID3D12CommandList* commandLists[] = { g_pd3d12CommandList };
        g_pd3d12CommandQueue->ExecuteCommandLists(1, commandLists);
        WaitForDX12Queue();
    }
}

static int LegacyMessageVK(UINT message, WPARAM wParam)
{
    switch (message)
    {
    case WM_KEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYDOWN:
    case WM_SYSKEYUP:
        return static_cast<int>(wParam);
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:
        return VK_LBUTTON;
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_RBUTTONDBLCLK:
        return VK_RBUTTON;
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_MBUTTONDBLCLK:
        return VK_MBUTTON;
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
    case WM_XBUTTONDBLCLK:
        return GET_XBUTTON_WPARAM(wParam) == XBUTTON1 ? VK_XBUTTON1 : VK_XBUTTON2;
    default:
        return 0;
    }
}

static bool IsLegacyDownMessage(UINT message)
{
    return message == WM_KEYDOWN || message == WM_SYSKEYDOWN ||
        message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN ||
        message == WM_MBUTTONDOWN || message == WM_XBUTTONDOWN ||
        message == WM_LBUTTONDBLCLK || message == WM_RBUTTONDBLCLK ||
        message == WM_MBUTTONDBLCLK || message == WM_XBUTTONDBLCLK;
}

static bool IsLegacyReleaseMessage(UINT message)
{
    return message == WM_KEYUP || message == WM_SYSKEYUP ||
        message == WM_LBUTTONUP || message == WM_RBUTTONUP ||
        message == WM_MBUTTONUP || message == WM_XBUTTONUP;
}

static void MarkInputDown(
    std::array<std::atomic_uint8_t, 0xFF>& owners,
    int vk,
    EInputOwner owner)
{
    if (vk <= 0 || vk > 0xFE)
        return;

    auto& inputOwner = owners[static_cast<size_t>(vk)];
    if (owner == EInputOwner::Gameplay)
    {
        inputOwner.store(
            static_cast<uint8_t>(EInputOwner::Gameplay),
            std::memory_order_release);
        return;
    }

    uint8_t expected = static_cast<uint8_t>(EInputOwner::Unknown);
    inputOwner.compare_exchange_strong(
        expected,
        static_cast<uint8_t>(EInputOwner::Overlay),
        std::memory_order_release,
        std::memory_order_relaxed);
}

static EInputOwner GetInputOwner(
    const std::array<std::atomic_uint8_t, 0xFF>& owners,
    int vk)
{
    if (vk <= 0 || vk > 0xFE)
        return EInputOwner::Unknown;

    return static_cast<EInputOwner>(
        owners[static_cast<size_t>(vk)].load(std::memory_order_acquire));
}

static EInputOwner TakeInputRelease(
    std::array<std::atomic_uint8_t, 0xFF>& owners,
    int vk)
{
    if (vk <= 0 || vk > 0xFE)
        return EInputOwner::Unknown;

    return static_cast<EInputOwner>(
        owners[static_cast<size_t>(vk)].exchange(
            static_cast<uint8_t>(EInputOwner::Unknown),
            std::memory_order_acq_rel));
}

static void SetInputOwner(
    std::array<std::atomic_uint8_t, 0xFF>& owners,
    int vk,
    EInputOwner owner)
{
    if (vk <= 0 || vk > 0xFE)
        return;

    owners[static_cast<size_t>(vk)].store(
        static_cast<uint8_t>(owner),
        std::memory_order_release);
}

static void ReconcileReleasedInputOwners()
{
    auto reconcile = [](std::array<std::atomic_uint8_t, 0xFF>& owners)
    {
        for (int vk = 1; vk <= 0xFE; vk++)
        {
            auto& owner = owners[static_cast<size_t>(vk)];
            if (owner.load(std::memory_order_acquire) !=
                    static_cast<uint8_t>(EInputOwner::Unknown) &&
                (GetAsyncKeyState(vk) & 0x8000) == 0)
            {
                owner.store(
                    static_cast<uint8_t>(EInputOwner::Unknown),
                    std::memory_order_release);
            }
        }
    };

    reconcile(g_LegacyInputOwners);
    reconcile(g_RawInputOwners);
    if ((GetAsyncKeyState(VK_F8) & 0x8000) == 0)
    {
        g_TranslatedF8Down.store(
            false, std::memory_order_release);
    }
}

static void ClearImGuiMouseOwnership(HWND window)
{
    constexpr int mouseButtons[] = {
        VK_LBUTTON, VK_RBUTTON, VK_MBUTTON,
        VK_XBUTTON1, VK_XBUTTON2
    };
    for (int vk : mouseButtons)
    {
        g_ImGuiLegacyDown[static_cast<size_t>(vk)].store(
            false, std::memory_order_release);
    }

    // The Win32 backend keeps its own mouse-button bitmask in addition to
    // ImGuiIO. Replaying releases clears that private state and releases
    // capture without touching gameplay during normal hidden input.
    ImGui_ImplWin32_WndProcHandler(window, WM_LBUTTONUP, 0, 0);
    ImGui_ImplWin32_WndProcHandler(window, WM_RBUTTONUP, 0, 0);
    ImGui_ImplWin32_WndProcHandler(window, WM_MBUTTONUP, 0, 0);
    ImGui_ImplWin32_WndProcHandler(
        window, WM_XBUTTONUP, MAKEWPARAM(0, XBUTTON1), 0);
    ImGui_ImplWin32_WndProcHandler(
        window, WM_XBUTTONUP, MAKEWPARAM(0, XBUTTON2), 0);
}

static void ClearImGuiInputOwnership(HWND window)
{
    for (auto& down : g_ImGuiLegacyDown)
        down.store(false, std::memory_order_release);

    ImGui::GetIO().AddFocusEvent(false);
    ClearImGuiMouseOwnership(window);
}

enum class ERawInputDestination : uint8_t
{
    Gameplay,
    Overlay
};

struct FRawInputTransition
{
    int VK = 0;
    bool Down = false;
    bool AtlasOwned = false;
    EInputOwner PriorOwner = EInputOwner::Unknown;
};

static ERawInputDestination RouteRawInput(
    LPARAM lParam,
    bool overlayVisible,
    bool allowAtlasHotkeys)
{
    RAWINPUT raw{};
    UINT rawSize = sizeof(raw);
    if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, &raw,
        &rawSize, sizeof(RAWINPUTHEADER)) == static_cast<UINT>(-1))
    {
        // The packet may contain the release for a gameplay-owned down.
        // When it cannot be inspected, forwarding both halves is safer than
        // swallowing an unknown release and leaving an action held.
        return ERawInputDestination::Gameplay;
    }

    std::array<FRawInputTransition, 10> transitions{};
    size_t transitionCount = 0;
    auto append = [
        allowAtlasHotkeys,
        &transitions,
        &transitionCount](int vk, bool down)
    {
        if (vk <= 0 || vk > 0xFE ||
            transitionCount >= transitions.size())
        {
            return;
        }

        FRawInputTransition& transition =
            transitions[transitionCount++];
        transition.VK = vk;
        transition.Down = down;
        transition.AtlasOwned =
            allowAtlasHotkeys && GUI_IsOwnedHotkey(vk);
        transition.PriorOwner =
            GetInputOwner(g_RawInputOwners, vk);
    };

    if (raw.header.dwType == RIM_TYPEKEYBOARD)
    {
        append(static_cast<int>(raw.data.keyboard.VKey),
            (raw.data.keyboard.Flags & RI_KEY_BREAK) == 0);
    }
    else if (raw.header.dwType == RIM_TYPEMOUSE)
    {
        const USHORT flags = raw.data.mouse.usButtonFlags;
        if (flags & RI_MOUSE_BUTTON_1_DOWN) append(VK_LBUTTON, true);
        if (flags & RI_MOUSE_BUTTON_1_UP) append(VK_LBUTTON, false);
        if (flags & RI_MOUSE_BUTTON_2_DOWN) append(VK_RBUTTON, true);
        if (flags & RI_MOUSE_BUTTON_2_UP) append(VK_RBUTTON, false);
        if (flags & RI_MOUSE_BUTTON_3_DOWN) append(VK_MBUTTON, true);
        if (flags & RI_MOUSE_BUTTON_3_UP) append(VK_MBUTTON, false);
        if (flags & RI_MOUSE_BUTTON_4_DOWN) append(VK_XBUTTON1, true);
        if (flags & RI_MOUSE_BUTTON_4_UP) append(VK_XBUTTON1, false);
        if (flags & RI_MOUSE_BUTTON_5_DOWN) append(VK_XBUTTON2, true);
        if (flags & RI_MOUSE_BUTTON_5_UP) append(VK_XBUTTON2, false);
    }

    bool hasGameplayTransition = transitionCount == 0 &&
        !overlayVisible;
    bool hasOverlayTransition = transitionCount == 0 &&
        overlayVisible;
    for (size_t index = 0;
        index < transitionCount;
        index++)
    {
        const FRawInputTransition& transition =
            transitions[index];
        bool gameplayTransition = false;
        if (transition.Down)
        {
            // Repeats and additional downs from an already-owned physical
            // press must follow its original destination. Reclassifying a
            // leaked gameplay down as overlay-owned would swallow its up.
            if (transition.PriorOwner == EInputOwner::Gameplay)
                gameplayTransition = true;
            else if (transition.PriorOwner == EInputOwner::Overlay)
                gameplayTransition = false;
            else
                gameplayTransition =
                    !overlayVisible &&
                    !transition.AtlasOwned;
        }
        else if (overlayVisible)
        {
            // A release whose down reached gameplay must still reach it.
            // Unknown releases are also harmless to forward and are safer
            // than leaving a game action held after focus changes.
            gameplayTransition =
                transition.PriorOwner != EInputOwner::Overlay;
        }
        else
        {
            gameplayTransition =
                transition.PriorOwner == EInputOwner::Gameplay ||
                (transition.PriorOwner == EInputOwner::Unknown &&
                    !transition.AtlasOwned);
        }

        hasGameplayTransition =
            hasGameplayTransition || gameplayTransition;
        hasOverlayTransition =
            hasOverlayTransition || !gameplayTransition;
    }

    // RAWMOUSE packets are indivisible. If any transition must reach
    // gameplay, forward the complete packet and mark every down as gameplay
    // owned so its later release is forwarded too. This prevents a mixed
    // LMB-up/MOUSE5-down packet from leaking only half of the MOUSE5 pair.
    const ERawInputDestination destination =
        hasGameplayTransition
            ? ERawInputDestination::Gameplay
            : ERawInputDestination::Overlay;

    for (size_t index = 0;
        index < transitionCount;
        index++)
    {
        const FRawInputTransition& transition =
            transitions[index];
        if (transition.Down)
        {
            SetInputOwner(
                g_RawInputOwners,
                transition.VK,
                destination == ERawInputDestination::Gameplay
                    ? EInputOwner::Gameplay
                    : EInputOwner::Overlay);
        }
        else
        {
            TakeInputRelease(
                g_RawInputOwners,
                transition.VK);
        }
    }

    if (hasGameplayTransition && hasOverlayTransition)
    {
        static std::atomic_bool loggedMixedPacket = false;
        if (!loggedMixedPacket.exchange(
            true, std::memory_order_acq_rel))
        {
            AtlasDiagnostics::WriteLine(
                "raw-input mixed transitions=%zu route=%s",
                transitionCount,
                destination == ERawInputDestination::Gameplay
                    ? "gameplay"
                    : "overlay");
        }
    }

    return destination;
}

static LRESULT CALLBACK HookedWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    const UINT releaseCaptureMessage =
        GetReleaseMouseCaptureMessage();
    if (releaseCaptureMessage != 0 &&
        msg == releaseCaptureMessage)
    {
        if (hWnd == g_hWnd && GUI_IsOverlayVisible())
            ReleaseCapture();
        return 0;
    }

    const bool interactiveInput = g_InteractiveInputEnabled.load(std::memory_order_acquire);
    const bool imguiReady = g_ImGuiReady.load(std::memory_order_acquire);
    const bool losingCapture =
        msg == WM_CAPTURECHANGED &&
        reinterpret_cast<HWND>(lParam) != hWnd;
    const bool focusLifecycleMessage = msg == WM_SETFOCUS || msg == WM_KILLFOCUS ||
        msg == WM_CANCELMODE || msg == WM_ACTIVATE ||
        msg == WM_ACTIVATEAPP || losingCapture;
    const bool losingFocus = msg == WM_KILLFOCUS || msg == WM_CANCELMODE ||
        (msg == WM_ACTIVATE && LOWORD(wParam) == WA_INACTIVE) ||
        (msg == WM_ACTIVATEAPP && !wParam);
    const bool gainingFocus = msg == WM_SETFOCUS ||
        (msg == WM_ACTIVATE && LOWORD(wParam) != WA_INACTIVE) ||
        (msg == WM_ACTIVATEAPP && wParam);
    const bool foregroundGameWindow = hWnd == g_hWnd && GetForegroundWindow() == hWnd;
    const bool atlasWindow = imguiReady && interactiveInput && hWnd == g_hWnd;

    // TranslateMessage emits a separate WM_CHAR/WM_DEADCHAR after the native
    // console keydown. Consume that companion message too: otherwise it can
    // leak a backtick into Unreal while opening ATLAS, or into the command
    // field while closing it.
    const bool atlasConsoleKey =
        atlasWindow && foregroundGameWindow &&
        GUI_IsOwnedHotkey(VK_OEM_3);
    if (atlasConsoleKey &&
        (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) &&
        static_cast<int>(wParam) == VK_OEM_3)
    {
        const unsigned int scanCode =
            static_cast<unsigned int>((static_cast<ULONG_PTR>(lParam) >> 16) & 0xFFu);
        g_SuppressedConsoleScanCode.store(
            scanCode + 1u, std::memory_order_release);
    }
    else if (atlasWindow &&
        (msg == WM_CHAR || msg == WM_SYSCHAR ||
            msg == WM_DEADCHAR || msg == WM_SYSDEADCHAR))
    {
        unsigned int pendingScanCode =
            g_SuppressedConsoleScanCode.load(std::memory_order_acquire);
        const unsigned int characterScanCode =
            static_cast<unsigned int>((static_cast<ULONG_PTR>(lParam) >> 16) & 0xFFu) + 1u;
        if (pendingScanCode != 0 &&
            pendingScanCode == characterScanCode &&
            g_SuppressedConsoleScanCode.compare_exchange_strong(
                pendingScanCode, 0u,
                std::memory_order_acq_rel, std::memory_order_acquire))
        {
            return 0;
        }
    }
    else if (losingFocus ||
        ((msg == WM_KEYUP || msg == WM_SYSKEYUP) &&
            static_cast<int>(wParam) == VK_OEM_3))
    {
        g_SuppressedConsoleScanCode.store(0u, std::memory_order_release);
    }

    if (atlasWindow && (foregroundGameWindow || focusLifecycleMessage))
        GUI_QueueInputMessage(msg, wParam, lParam);
    if (atlasWindow &&
        (gainingFocus || msg == WM_CANCELMODE))
        ReconcileReleasedInputOwners();

    const bool overlayVisible = GUI_IsOverlayVisible();
    const int legacyVK = LegacyMessageVK(msg, wParam);
    const bool legacyDown = IsLegacyDownMessage(msg);
    const bool legacyRelease = IsLegacyReleaseMessage(msg);
    const EInputOwner legacyReleaseOwner =
        atlasWindow && legacyRelease
            ? TakeInputRelease(g_LegacyInputOwners, legacyVK)
            : EInputOwner::Unknown;
    const bool imguiOwnedRelease =
        atlasWindow && legacyRelease && legacyVK > 0 && legacyVK <= 0xFE &&
        g_ImGuiLegacyDown[static_cast<size_t>(legacyVK)].exchange(
            false, std::memory_order_acq_rel);

    // Raw input carries an ephemeral HRAWINPUT handle, so inspect it here.
    // Legacy ImGui input stays on the owning window thread and is serialized
    // against NewFrame/render by a short recursive lock.
    if (atlasWindow && msg == WM_INPUT)
    {
        const ERawInputDestination destination =
            RouteRawInput(
                lParam,
                overlayVisible,
                !overlayVisible && foregroundGameWindow);
        if (destination == ERawInputDestination::Gameplay)
            return CallWindowProcW(
                OriginalWndProc, hWnd, msg, wParam, lParam);
        return DefWindowProcW(hWnd, msg, wParam, lParam);
    }

    // Original-console F8 is translated to Unreal's OEM3 key. Latch the
    // translation for the complete physical press so a mode/overlay change
    // between down and up cannot send OEM3-down followed by F8-up.
    if (atlasWindow && legacyVK == VK_F8)
    {
        const bool translatedRelease =
            legacyRelease &&
            g_TranslatedF8Down.exchange(
                false, std::memory_order_acq_rel);
        const bool translatedDown =
            legacyDown && !overlayVisible &&
            foregroundGameWindow &&
            FConfiguration::ConsoleMode.load(
                std::memory_order_acquire) ==
                    static_cast<int>(EConsoleMode::Unreal);
        if (translatedRelease || translatedDown)
        {
            if (translatedDown)
            {
                const bool repeat =
                    (static_cast<ULONG_PTR>(lParam) &
                        (1ull << 30)) != 0;
                if (repeat ||
                    g_TranslatedF8Down.exchange(
                        true, std::memory_order_acq_rel))
                {
                    return 0;
                }

                MarkInputDown(
                    g_LegacyInputOwners,
                    VK_F8,
                    EInputOwner::Gameplay);
            }

            ULONG_PTR translatedLParam =
                static_cast<ULONG_PTR>(lParam);
            translatedLParam &= ~(0xFFull << 16);
            translatedLParam |=
                (static_cast<ULONG_PTR>(
                    MapVirtualKeyW(
                        VK_OEM_3,
                        MAPVK_VK_TO_VSC)) &
                    0xFFull) << 16;
            return CallWindowProcW(
                OriginalWndProc,
                hWnd,
                msg,
                static_cast<WPARAM>(VK_OEM_3),
                static_cast<LPARAM>(
                    translatedLParam));
        }
    }

    if (atlasWindow && overlayVisible && legacyDown &&
        legacyVK > 0 && legacyVK <= 0xFE)
    {
        g_ImGuiLegacyDown[static_cast<size_t>(legacyVK)].store(
            true, std::memory_order_release);
    }

    if (atlasWindow && msg != WM_INPUT &&
        (overlayVisible || imguiOwnedRelease || focusLifecycleMessage))
    {
        std::lock_guard<std::recursive_mutex> lock(g_ImGuiMutex);
        ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam);
        if (losingFocus)
            ClearImGuiInputOwnership(hWnd);
        else if (losingCapture)
            ClearImGuiMouseOwnership(hWnd);
    }

    if (atlasWindow && overlayVisible)
    {
        if (legacyDown)
            MarkInputDown(g_LegacyInputOwners, legacyVK, EInputOwner::Overlay);

        if (legacyRelease)
        {
            // A release whose down reached Unreal must still reach Unreal so
            // opening the overlay cannot leave gameplay input held. Releases
            // for UI/overlay downs are consumed to avoid orphan actions.
            if (legacyReleaseOwner == EInputOwner::Overlay ||
                (legacyReleaseOwner == EInputOwner::Unknown &&
                    GUI_IsOwnedHotkey(legacyVK)))
                return 0;
            return CallWindowProcW(OriginalWndProc, hWnd, msg, wParam, lParam);
        }

        if (legacyDown ||
            msg == WM_MOUSEWHEEL || msg == WM_MOUSEHWHEEL ||
            msg == WM_MOUSEMOVE || msg == WM_CHAR || msg == WM_SYSCHAR ||
            msg == WM_DEADCHAR || msg == WM_SYSDEADCHAR ||
            msg == WM_UNICHAR || msg == WM_CONTEXTMENU)
        {
            return 0;
        }
    }

    if (atlasWindow && !overlayVisible)
    {
        if (legacyDown &&
            GetInputOwner(g_LegacyInputOwners, legacyVK) ==
                EInputOwner::Overlay)
        {
            // A held key/button that began in the overlay remains owned by it
            // until release, even if that same press closed the overlay.
            return 0;
        }

        if (legacyRelease)
        {
            if (legacyReleaseOwner == EInputOwner::Overlay ||
                (legacyReleaseOwner == EInputOwner::Unknown &&
                    GUI_IsOwnedHotkey(legacyVK)))
                return 0;
            return CallWindowProcW(OriginalWndProc, hWnd, msg, wParam, lParam);
        }

        if (!foregroundGameWindow)
        {
            if (legacyDown)
                MarkInputDown(g_LegacyInputOwners, legacyVK, EInputOwner::Gameplay);
            return CallWindowProcW(OriginalWndProc, hWnd, msg, wParam, lParam);
        }

        // A saved command bind belongs exclusively to ATLAS. Forwarding the
        // same MOUSE5/key press into Fortnite can activate a gameplay ability
        // on every command and eventually saturate its uint8 ActiveCount.
        if (GUI_ShouldConsumeInputMessage(msg, wParam))
        {
            if (legacyDown)
                MarkInputDown(g_LegacyInputOwners, legacyVK, EInputOwner::Overlay);
            return 0;
        }

        if (legacyDown)
            MarkInputDown(g_LegacyInputOwners, legacyVK, EInputOwner::Gameplay);
    }

    return CallWindowProcW(OriginalWndProc, hWnd, msg, wParam, lParam);
}

static HRESULT __stdcall HookedPresent(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags)
{
    IDXGISwapChain* knownPrimary =
        g_PrimarySwapChain.load(std::memory_order_acquire);
    if (knownPrimary && pSwapChain != knownPrimary)
        return OriginalPresent(pSwapChain, SyncInterval, Flags);

    {
        // More than one swap chain can call Present on different threads.
        // All backends share one ImGui context, and DX12 also shares a command
        // allocator/list, so keep the complete overlay frame lifecycle serial.
        std::lock_guard<std::mutex> renderLock(g_RenderMutex);

        if (!g_ImGuiReady)
        {
            const bool dx12SwapChain = IsDX12SwapChain(pSwapChain);

            if (!InitDX11(pSwapChain) && !InitDX12(pSwapChain) &&
                !dx12SwapChain && !g_LoggedUnsupportedRenderer)
            {
                OutputDebugStringW(L"ATLAS: Present hook landed, but the swap chain is not D3D11 or D3D12.\n");
                g_LoggedUnsupportedRenderer = true;
            }
        }

        if (g_ImGuiReady)
        {
            IDXGISwapChain* primarySwapChain =
                g_PrimarySwapChain.load(std::memory_order_acquire);
            if (!primarySwapChain)
            {
                g_PrimarySwapChain.store(
                    pSwapChain, std::memory_order_release);
                primarySwapChain = pSwapChain;
            }

            // ImGui and the render targets are initialized for one game swap
            // chain. Ignore secondary Presents instead of advancing the same
            // UI twice or drawing through the primary chain's target.
            if (pSwapChain == primarySwapChain)
            {
                if (g_RenderBackend == ERenderBackend::DX11)
                    RenderDX11(pSwapChain);
                else if (g_RenderBackend == ERenderBackend::DX12)
                    RenderDX12(pSwapChain);
            }
        }
    }

    return OriginalPresent(pSwapChain, SyncInterval, Flags);
}

static HRESULT __stdcall HookedResizeBuffers(IDXGISwapChain* pSwapChain, UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags)
{
    IDXGISwapChain* knownPrimary =
        g_PrimarySwapChain.load(std::memory_order_acquire);
    if (knownPrimary && pSwapChain != knownPrimary)
    {
        return OriginalResizeBuffers(
            pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags);
    }

    std::lock_guard<std::mutex> renderLock(g_RenderMutex);

    IDXGISwapChain* primarySwapChain =
        g_PrimarySwapChain.load(std::memory_order_acquire);
    if (primarySwapChain && pSwapChain != primarySwapChain)
    {
        return OriginalResizeBuffers(
            pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags);
    }

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

void Main()
{
    // ATLAS owns detached client/render threads and live hooks. Pinning keeps
    // every hook target valid until process exit instead of pretending a
    // mid-session FreeLibrary can safely tear those threads down.
    if (!PinAtlasModule())
    {
        OutputDebugStringW(L"ATLAS: Could not pin the module; initialization was stopped.\n");
        return;
    }

    const wchar_t* commandLine = GetCommandLineW();
    const bool headlessHost = ContainsInsensitive(commandLine, L"-nullrhi");
    bHeadlessHostProcess = headlessHost;
    g_InteractiveInputEnabled.store(!headlessHost, std::memory_order_release);
    Client::SetConsoleCaptureEnabled(!headlessHost);
    const int consoleMode = HotkeyPersist::LoadConsoleMode();
    FConfiguration::ConsoleMode.store(
        consoleMode, std::memory_order_release);
    AtlasDiagnostics::WriteLine("process-role interactive-input=%s", headlessHost ? "disabled-headless-host" : "enabled-client");
    AtlasDiagnostics::WriteLine("console-mode selected=%s",
        consoleMode == static_cast<int>(EConsoleMode::Unreal)
            ? "original-unreal"
            : "atlas");

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

    if (!headlessHost &&
        VersionInfo.EngineVersion >= 5.3 &&
        FConfiguration::bEnableIris)
    {
        UKismetSystemLibrary::ExecuteConsoleCommand(UWorld::GetWorld(), FString(L"log LogIris None"), nullptr);
        UKismetSystemLibrary::ExecuteConsoleCommand(UWorld::GetWorld(), FString(L"log LogIrisRpc None"), nullptr);
        UKismetSystemLibrary::ExecuteConsoleCommand(UWorld::GetWorld(), FString(L"log LogIrisBridge None"), nullptr);

        std::lock_guard<std::mutex> Lock(IrisPolicyMutex);
        IrisCVar = FindCVar<uint32_t>(
            L"net.Iris.UseIrisReplication");
        if (IrisCVar)
        {
            CaptureIrisPatchSites(
                reinterpret_cast<uintptr_t>(IrisCVar));
        }
        else
        {
            AtlasDiagnostics::WriteLine(
                "replication-policy prepare-failed cvar=null");
        }
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
        break;
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
    case DLL_PROCESS_DETACH:
        break;
    }
    return TRUE;
}
