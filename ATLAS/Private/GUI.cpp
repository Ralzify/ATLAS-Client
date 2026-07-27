#include "pch.h"
#include "../Public/GUI.h"
#include "../Public/Hotkey.h"
#include "../Public/Configuration.h"
#include "../Public/Client.h"
#include "../Public/Diagnostics.h"
#include "../Public/Icon.h"
#include "../ImGui/imgui.h"
#include "../ImGui/imgui_stdlib.h"

#include <sstream>
#include <iomanip>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <deque>
#include <mutex>
#include <unordered_set>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG

#include "../ImGui/stb_image.h"

extern void* SelectEdit(void*);
extern void* SelectReset(void*);

#define ACCENT_R 0.f
#define ACCENT_G 0.635f
#define ACCENT_B 0.808f

static ImVec4 Accent(float a = 1.f) { return ImVec4(ACCENT_R, ACCENT_G, ACCENT_B, a); }
static ImVec4 AccentDk(float a = 1.f) { return ImVec4(0.f, 0.38f, 0.50f, a); }

enum class EConsoleLineKind : uint8_t
{
    Output,
    Command,
    Spacer,
    System,
    Error
};

struct FConsoleLine
{
    uint64_t Id = 0;
    EConsoleLineKind Kind = EConsoleLineKind::Output;
    std::string Time;
    std::string Text;
};

struct FConsoleUiState
{
    std::string Input;
    std::string Draft;
    std::vector<std::string> History;
    int HistoryPosition = -1;
    std::string CompletionSeed;
    std::vector<std::string> CompletionMatches;
    int CompletionPosition = -1;
    bool FocusInput = false;
    bool ScrollToBottom = false;
    bool WasAtBottom = true;
    bool WindowFocused = false;
    bool Expanded = false;
    bool ResetExpansion = false;
    float Expansion = 0.f;
    bool InitialHelpPending = false;
    bool InitialHelpRequested = false;
    uint64_t InitialHelpSession = 0;
    unsigned int InitialHelpAttempts = 0;
    ULONGLONG NextInitialHelpAttemptAt = 0;
};

static FConsoleUiState g_ConsoleUi;
static std::deque<FConsoleLine> g_ConsoleLines;
static size_t g_ConsoleLineBytes = 0;
static uint64_t g_NextConsoleLineId = 1;
static uint64_t g_ConsoleLineRevision = 0;

static std::mutex g_ConsolePendingMutex;
static std::deque<std::string> g_ConsolePending;
static size_t g_ConsolePendingBytes = 0;
static std::atomic_uint g_ConsoleDroppedMessages = 0;
static std::string g_ConsoleDrainCarry;
static size_t g_ConsoleDrainOffset = 0;
static bool g_FocusMenuWindow = false;
static bool g_MenuWindowFocused = false;
static ULONGLONG g_BindErrorUntil = 0;
static std::string g_BindError;

static constexpr size_t kMaximumConsoleLines = 2048;
static constexpr size_t kMaximumConsoleBytes = 1024 * 1024;
static constexpr size_t kMaximumPendingMessages = 256;
static constexpr size_t kMaximumPendingBytes = 512 * 1024;
static constexpr size_t kMaximumConsoleLineLength = 4096;
static constexpr size_t kMaximumConsoleHistory = 100;
static constexpr size_t kMaximumConsoleCommandLength = 2048;
static constexpr size_t kMaximumDrainLinesPerFrame = 256;
static constexpr size_t kMaximumDrainBytesPerFrame = 64 * 1024;
static constexpr unsigned int kMaximumInitialHelpAttempts = 3;
static constexpr ULONGLONG kInitialHelpRetryMs = 5000;

static bool IsUnrealConsoleHotkey(int vk)
{
    // Both characters produced by the Tilde key arrive as VK_OEM_3. F8 is
    // ATLAS's second fixed alias for the same UE-style console cycle.
    return vk == VK_OEM_3 || vk == VK_F8;
}

bool GUI_IsOverlayVisible()
{
    return FGUI::bVisible || FGUI::bConsoleVisible;
}

static bool ContainsInsensitive(const std::string& value, const char* needle)
{
    if (!needle || !*needle)
        return true;

    const size_t needleLength = strlen(needle);
    if (needleLength > value.size())
        return false;

    for (size_t start = 0; start + needleLength <= value.size(); start++)
    {
        size_t i = 0;
        for (; i < needleLength; i++)
        {
            const unsigned char left = static_cast<unsigned char>(value[start + i]);
            const unsigned char right = static_cast<unsigned char>(needle[i]);
            if (std::tolower(left) != std::tolower(right))
                break;
        }
        if (i == needleLength)
            return true;
    }

    return false;
}

static bool StartsWithInsensitive(const std::string& value, const std::string& prefix)
{
    if (prefix.size() > value.size())
        return false;

    for (size_t i = 0; i < prefix.size(); i++)
    {
        const unsigned char left = static_cast<unsigned char>(value[i]);
        const unsigned char right = static_cast<unsigned char>(prefix[i]);
        if (std::tolower(left) != std::tolower(right))
            return false;
    }

    return true;
}

static EConsoleLineKind ClassifyConsoleOutput(const std::string& text)
{
    if (StartsWithInsensitive(text, "[ATLAS]"))
        return EConsoleLineKind::System;

    static const char* kErrorWords[] = {
        "could not", "failed", "invalid", "wrong number", "error", "no player",
        "not found", "unavailable", "queue full"
    };

    for (const char* word : kErrorWords)
        if (ContainsInsensitive(text, word))
            return EConsoleLineKind::Error;

    return EConsoleLineKind::Output;
}

static void AppendConsoleLine(EConsoleLineKind kind, std::string text)
{
    const bool isSpacer = kind == EConsoleLineKind::Spacer;
    if (!isSpacer)
    {
        while (!text.empty() &&
            (text.back() == '\r' || text.back() == '\n' ||
                text.back() == '\0'))
        {
            text.pop_back();
        }

        if (text.empty())
            return;

        if (text.size() > kMaximumConsoleLineLength)
        {
            text.resize(kMaximumConsoleLineLength - 14);
            text += " ...[truncated]";
        }
    }

    SYSTEMTIME now{};
    char timestamp[16]{};
    if (!isSpacer)
    {
        GetLocalTime(&now);
        snprintf(timestamp, sizeof(timestamp), "%02u:%02u:%02u",
            now.wHour, now.wMinute, now.wSecond);
    }

    FConsoleLine line{};
    line.Id = g_NextConsoleLineId++;
    line.Kind = kind;
    line.Time = timestamp;
    line.Text = std::move(text);
    g_ConsoleLineBytes += line.Time.size() + line.Text.size();
    g_ConsoleLines.push_back(std::move(line));

    while (!g_ConsoleLines.empty() &&
        (g_ConsoleLines.size() > kMaximumConsoleLines || g_ConsoleLineBytes > kMaximumConsoleBytes))
    {
        g_ConsoleLineBytes -= g_ConsoleLines.front().Time.size() + g_ConsoleLines.front().Text.size();
        g_ConsoleLines.pop_front();
    }

    g_ConsoleLineRevision++;
}

bool GUI_QueueConsoleOutput(const wchar_t* text, int length)
{
    if (!text || length <= 0)
        return false;

    if (length > 16384)
        length = 16384;

    // Reserve access before converting. Once the bounded queue is saturated,
    // producers drop immediately instead of repeatedly allocating and doing
    // UTF conversion on the gameplay/RPC thread.
    std::unique_lock<std::mutex> lock(g_ConsolePendingMutex, std::try_to_lock);
    const size_t minimumUtf8Bytes = static_cast<size_t>(length);
    if (!lock.owns_lock() ||
        g_ConsolePending.size() >= kMaximumPendingMessages ||
        g_ConsolePendingBytes >= kMaximumPendingBytes ||
        minimumUtf8Bytes > kMaximumPendingBytes - g_ConsolePendingBytes)
    {
        g_ConsoleDroppedMessages.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    const int utf8Length = WideCharToMultiByte(CP_UTF8, 0, text, length, nullptr, 0, nullptr, nullptr);
    if (utf8Length <= 0)
        return false;

    std::string utf8(static_cast<size_t>(utf8Length), '\0');
    if (WideCharToMultiByte(CP_UTF8, 0, text, length, utf8.data(), utf8Length, nullptr, nullptr) != utf8Length)
        return false;

    if (utf8.size() > kMaximumPendingBytes - g_ConsolePendingBytes)
    {
        g_ConsoleDroppedMessages.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    g_ConsolePendingBytes += utf8.size();
    g_ConsolePending.push_back(std::move(utf8));
    return true;
}

static void DrainConsoleOutput()
{
    const unsigned int dropped = g_ConsoleDroppedMessages.exchange(0, std::memory_order_acq_rel);
    if (dropped > 0)
    {
        char message[128]{};
        snprintf(message, sizeof(message), "[ATLAS] Dropped %u output message%s to protect frame time.",
            dropped, dropped == 1 ? "" : "s");
        AppendConsoleLine(EConsoleLineKind::System, message);
    }

    size_t lineCount = 0;
    size_t byteCount = 0;
    while (lineCount < kMaximumDrainLinesPerFrame &&
        byteCount < kMaximumDrainBytesPerFrame)
    {
        if (g_ConsoleDrainOffset >= g_ConsoleDrainCarry.size())
        {
            g_ConsoleDrainCarry.clear();
            g_ConsoleDrainOffset = 0;

            std::lock_guard<std::mutex> lock(g_ConsolePendingMutex);
            if (g_ConsolePending.empty())
                break;

            g_ConsolePendingBytes -= g_ConsolePending.front().size();
            g_ConsoleDrainCarry = std::move(g_ConsolePending.front());
            g_ConsolePending.pop_front();
        }

        const size_t end = g_ConsoleDrainCarry.find('\n', g_ConsoleDrainOffset);
        const size_t lineEnd = end == std::string::npos ? g_ConsoleDrainCarry.size() : end;
        const size_t consumedBytes = lineEnd - g_ConsoleDrainOffset +
            (end == std::string::npos ? 0 : 1);

        if (lineCount > 0 && consumedBytes > kMaximumDrainBytesPerFrame - byteCount)
            break;

        std::string line = g_ConsoleDrainCarry.substr(
            g_ConsoleDrainOffset, lineEnd - g_ConsoleDrainOffset);
        AppendConsoleLine(ClassifyConsoleOutput(line), std::move(line));

        lineCount++;
        byteCount += consumedBytes;
        g_ConsoleDrainOffset = end == std::string::npos
            ? g_ConsoleDrainCarry.size()
            : end + 1;
    }
}

static void AppendConsoleCommandEcho(
    const std::string& command)
{
    // Flush output produced by the previous command before placing the next
    // command boundary. This keeps hotkey-triggered output grouped even when
    // the console is closed between key presses.
    DrainConsoleOutput();

    if (!g_ConsoleLines.empty() &&
        g_ConsoleLines.back().Kind != EConsoleLineKind::Spacer)
    {
        AppendConsoleLine(EConsoleLineKind::Spacer, {});
    }

    if (!command.empty())
    {
        AppendConsoleLine(
            EConsoleLineKind::Command,
            "> " + command);
    }

    g_ConsoleUi.ScrollToBottom = true;
}

static void ClearConsoleLogs()
{
    {
        std::lock_guard<std::mutex> lock(g_ConsolePendingMutex);
        g_ConsolePending.clear();
        g_ConsolePendingBytes = 0;
    }

    g_ConsoleDroppedMessages.store(0, std::memory_order_release);
    g_ConsoleDrainCarry.clear();
    g_ConsoleDrainOffset = 0;
    g_ConsoleLines.clear();
    g_ConsoleLineBytes = 0;
    g_ConsoleLineRevision++;
    g_ConsoleUi.ScrollToBottom = false;
    g_ConsoleUi.WasAtBottom = true;
}

static const char* VKName(int vk)
{
    static char buf[32];

    switch (vk)
    {
    case VK_LBUTTON: return "MOUSE1";
    case VK_RBUTTON: return "MOUSE2";
    case VK_MBUTTON: return "MOUSE3";
    case VK_XBUTTON1: return "MOUSE4";
    case VK_XBUTTON2: return "MOUSE5";
    case VK_F1:  return "F1";  case VK_F2:  return "F2";
    case VK_F3:  return "F3";  case VK_F4:  return "F4";
    case VK_F5:  return "F5";  case VK_F6:  return "F6";
    case VK_F7:  return "F7";  case VK_F8:  return "F8";
    case VK_F9:  return "F9";  case VK_F10: return "F10";
    case VK_F11: return "F11"; case VK_F12: return "F12";
    case VK_INSERT: return "INSERT"; case VK_DELETE: return "DELETE";
    case VK_HOME:   return "HOME";   case VK_END:    return "END";
    case VK_PRIOR:  return "PGUP";   case VK_NEXT:   return "PGDN";
    case VK_SPACE:  return "SPACE";  case VK_RETURN: return "ENTER";
    case VK_TAB:    return "TAB";    case VK_BACK:   return "BACKSPACE";
    case VK_LEFT:   return "LEFT";   case VK_RIGHT:  return "RIGHT";
    case VK_UP:     return "UP";     case VK_DOWN:   return "DOWN";
    case VK_CAPITAL: return "CAPS";  case VK_OEM_3:  return "`";
    }

    if (vk >= '0' && vk <= '9') { buf[0] = (char)vk; buf[1] = '\0'; return buf; }
    if (vk >= 'A' && vk <= 'Z') { buf[0] = (char)vk; buf[1] = '\0'; return buf; }

    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9)
    {
        snprintf(buf, sizeof(buf), "NUM%d", vk - VK_NUMPAD0);
        return buf;
    }

    // Fall back to the OS name for anything else (OEM/punctuation keys etc.).
    UINT sc = MapVirtualKeyW((UINT)vk, MAPVK_VK_TO_VSC);
    wchar_t wname[32]{};
    if (sc != 0 && GetKeyNameTextW((LONG)(sc << 16), wname, 32) > 0 &&
        WideCharToMultiByte(CP_UTF8, 0, wname, -1, buf, sizeof(buf), nullptr, nullptr) > 0)
        return buf;

    snprintf(buf, sizeof(buf), "0x%02X", vk);
    return buf;
}

static const char* BindName(int vk)
{
    return (vk > 0 && vk <= 254) ? VKName(vk) : "UNBOUND";
}

// Saved command hotkeys are actions owned by ATLAS. Keep an atomic lookup for
// WndProc so those presses do not also activate the game's binding (MOUSE5 is
// commonly bound to Jump, which can otherwise leave ability activations open).
static std::array<std::atomic_bool, 0xFF> g_ExclusiveCommandHotkeys{};

static void RefreshExclusiveCommandHotkeys()
{
    for (auto& exclusive : g_ExclusiveCommandHotkeys)
        exclusive.store(false, std::memory_order_release);

    for (const auto& command : FGUI::Commands)
    {
        if (command.VK > 0 && command.VK <= 0xFE)
        {
            g_ExclusiveCommandHotkeys[static_cast<size_t>(command.VK)].store(true, std::memory_order_release);
            AtlasDiagnostics::WriteLine("command-hotkey-exclusive vk=%d command=%s", command.VK, command.Command.c_str());
        }
    }

    for (const auto& macro : FGUI::Macros)
    {
        if (macro.VK > 0 && macro.VK <= 0xFE)
        {
            g_ExclusiveCommandHotkeys[static_cast<size_t>(macro.VK)].store(true, std::memory_order_release);
            AtlasDiagnostics::WriteLine("macro-hotkey-exclusive vk=%d macro=%s", macro.VK, macro.Name.c_str());
        }
    }
}

void FGUI::SaveHotkey() { HotkeyPersist::Save(FGUI::HotkeyVK); }
void FGUI::LoadHotkey() { FGUI::HotkeyVK = HotkeyPersist::Load(VK_F9); }

void FGUI::SaveJoinHotkey() { HotkeyPersist::Save(FGUI::JoinHotkeyVK, "joinHotkey"); }
void FGUI::LoadJoinHotkey() { FGUI::JoinHotkeyVK = HotkeyPersist::Load(VK_F5, "joinHotkey"); }

void FGUI::SaveCommands()
{
    HotkeyPersist::SaveCommands(FGUI::Commands, FGUI::Macros);
    RefreshExclusiveCommandHotkeys();
}

void FGUI::LoadCommands()
{
    FGUI::Commands = HotkeyPersist::LoadCommands();
    RefreshExclusiveCommandHotkeys();
}

void FGUI::SaveMacros()
{
    HotkeyPersist::SaveCommands(FGUI::Commands, FGUI::Macros);
    RefreshExclusiveCommandHotkeys();
}

void FGUI::LoadMacros()
{
    FGUI::Macros = HotkeyPersist::LoadMacros();
    RefreshExclusiveCommandHotkeys();
}

void FGUI::ResetAll()
{
    FGUI::HotkeyVK = VK_F9;
    FGUI::JoinHotkeyVK = VK_F5;
    FConfiguration::ConsoleMode.store(
        static_cast<int>(EConsoleMode::Atlas), std::memory_order_release);
    FGUI::bRebinding = false;
    FGUI::bRebindingJoin = false;
    FGUI::RebindingCommandIndex = -2;
    FGUI::RebindingMacroIndex = -2;
    FGUI::PendingCommandVK = 0;
    FGUI::CommandInput[0] = '\0';
    FGUI::MacroDelayInput[0] = '\0';
    FGUI::Commands.clear();
    FGUI::Macros.clear();
    FGUI::MacroDraftSteps.clear();
    RefreshExclusiveCommandHotkeys();

    HotkeyPersist::SaveAll(FGUI::HotkeyVK, FGUI::JoinHotkeyVK,
        static_cast<int>(EConsoleMode::Atlas), {}, {});
}

void GUI_LoadTextures(ID3D11Device* device)
{
    int w, h, ch;
    unsigned char* pixels = stbi_load_from_memory(Icon, (int)sizeof(Icon), &w, &h, &ch, 4);

    if (!pixels) 
        return;

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = (UINT)w;
    desc.Height = (UINT)h;
    desc.MipLevels = 0;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    desc.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;

    ID3D11Texture2D* tex = nullptr;
    if (SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &tex)))
    {
        ID3D11DeviceContext* ctx = nullptr;
        device->GetImmediateContext(&ctx);

        ctx->UpdateSubresource(tex, 0, nullptr, pixels, (UINT)(w * 4), 0);

        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = (UINT)-1;
        ID3D11ShaderResourceView* srv = nullptr;
        if (SUCCEEDED(device->CreateShaderResourceView(tex, &srvDesc, &srv)))
        {
            FGUI::LogoTexture = (ImTextureID)srv;
            ctx->GenerateMips(srv);
            FGUI::LogoW = w;
            FGUI::LogoH = h;
        }

        if (ctx) ctx->Release();
        tex->Release();
    }

    stbi_image_free(pixels);
}

static ID3D12Resource* g_LogoTextureDX12 = nullptr;

struct LogoMipLevel
{
    UINT Width = 0;
    UINT Height = 0;
    std::vector<uint8_t> Pixels;
};

static std::vector<LogoMipLevel> BuildLogoMipChain(const unsigned char* pixels, UINT width, UINT height)
{
    std::vector<LogoMipLevel> mips;
    if (!pixels || !width || !height)
        return mips;

    LogoMipLevel base{};
    base.Width = width;
    base.Height = height;
    base.Pixels.assign(pixels, pixels + (size_t)width * height * 4);
    mips.push_back(std::move(base));

    while (mips.back().Width > 1 || mips.back().Height > 1)
    {
        const LogoMipLevel& src = mips.back();
        LogoMipLevel dst{};
        dst.Width = src.Width > 1 ? src.Width / 2 : 1;
        dst.Height = src.Height > 1 ? src.Height / 2 : 1;
        dst.Pixels.resize((size_t)dst.Width * dst.Height * 4);

        for (UINT y = 0; y < dst.Height; y++)
        {
            for (UINT x = 0; x < dst.Width; x++)
            {
                UINT r = 0, g = 0, b = 0, a = 0;
                for (UINT oy = 0; oy < 2; oy++)
                {
                    const UINT sy = min(src.Height - 1, y * 2 + oy);
                    for (UINT ox = 0; ox < 2; ox++)
                    {
                        const UINT sx = min(src.Width - 1, x * 2 + ox);
                        const uint8_t* p = &src.Pixels[((size_t)sy * src.Width + sx) * 4];
                        const UINT alpha = p[3];
                        r += p[0] * alpha;
                        g += p[1] * alpha;
                        b += p[2] * alpha;
                        a += alpha;
                    }
                }

                uint8_t* out = &dst.Pixels[((size_t)y * dst.Width + x) * 4];
                out[3] = (uint8_t)(a / 4);
                if (a > 0)
                {
                    out[0] = (uint8_t)(r / a);
                    out[1] = (uint8_t)(g / a);
                    out[2] = (uint8_t)(b / a);
                }
                else
                {
                    out[0] = out[1] = out[2] = 0;
                }
            }
        }

        mips.push_back(std::move(dst));
    }

    return mips;
}

void GUI_LoadTexturesDX12(ID3D12Device* device, ID3D12CommandQueue* commandQueue, D3D12_CPU_DESCRIPTOR_HANDLE srvCpu, D3D12_GPU_DESCRIPTOR_HANDLE srvGpu)
{
    if (!device || !commandQueue || !srvCpu.ptr || !srvGpu.ptr)
        return;

    int w, h, ch;
    unsigned char* pixels = stbi_load_from_memory(Icon, (int)sizeof(Icon), &w, &h, &ch, 4);
    if (!pixels)
        return;

    std::vector<LogoMipLevel> mips = BuildLogoMipChain(pixels, (UINT)w, (UINT)h);
    stbi_image_free(pixels);
    if (mips.empty())
        return;

    if (g_LogoTextureDX12)
    {
        g_LogoTextureDX12->Release();
        g_LogoTextureDX12 = nullptr;
    }

    D3D12_RESOURCE_DESC texDesc{};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width = (UINT64)w;
    texDesc.Height = (UINT)h;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = (UINT16)mips.size();
    texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    D3D12_HEAP_PROPERTIES defaultHeap{};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;

    HRESULT hr = device->CreateCommittedResource(
        &defaultHeap,
        D3D12_HEAP_FLAG_NONE,
        &texDesc,
        D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,
        IID_PPV_ARGS(&g_LogoTextureDX12));

    if (FAILED(hr))
    {
        return;
    }

    UINT64 uploadSize = 0;
    const UINT mipCount = (UINT)mips.size();
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(mipCount);
    std::vector<UINT> numRows(mipCount);
    std::vector<UINT64> rowSizeInBytes(mipCount);
    device->GetCopyableFootprints(&texDesc, 0, mipCount, 0, footprints.data(), numRows.data(), rowSizeInBytes.data(), &uploadSize);

    D3D12_HEAP_PROPERTIES uploadHeap{};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC uploadDesc{};
    uploadDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    uploadDesc.Width = uploadSize;
    uploadDesc.Height = 1;
    uploadDesc.DepthOrArraySize = 1;
    uploadDesc.MipLevels = 1;
    uploadDesc.SampleDesc.Count = 1;
    uploadDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ID3D12Resource* uploadBuffer = nullptr;
    hr = device->CreateCommittedResource(
        &uploadHeap,
        D3D12_HEAP_FLAG_NONE,
        &uploadDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&uploadBuffer));

    if (FAILED(hr))
    {
        g_LogoTextureDX12->Release();
        g_LogoTextureDX12 = nullptr;
        return;
    }

    uint8_t* mapped = nullptr;
    D3D12_RANGE readRange{};
    if (FAILED(uploadBuffer->Map(0, &readRange, (void**)&mapped)))
    {
        uploadBuffer->Release();
        g_LogoTextureDX12->Release();
        g_LogoTextureDX12 = nullptr;
        return;
    }

    for (UINT mip = 0; mip < mipCount; mip++)
    {
        const LogoMipLevel& level = mips[mip];
        const UINT srcPitch = level.Width * 4;
        uint8_t* dst = mapped + footprints[mip].Offset;
        for (UINT row = 0; row < level.Height; row++)
            memcpy(dst + row * footprints[mip].Footprint.RowPitch, level.Pixels.data() + (size_t)row * srcPitch, srcPitch);
    }

    uploadBuffer->Unmap(0, nullptr);

    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* commandList = nullptr;
    ID3D12Fence* fence = nullptr;
    HANDLE fenceEvent = nullptr;

    hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
    if (SUCCEEDED(hr))
        hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr, IID_PPV_ARGS(&commandList));
    if (SUCCEEDED(hr))
        hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    if (SUCCEEDED(hr))
        fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    if (FAILED(hr) || !fenceEvent)
    {
        if (fenceEvent) CloseHandle(fenceEvent);
        if (fence) fence->Release();
        if (commandList) commandList->Release();
        if (allocator) allocator->Release();
        uploadBuffer->Release();
        g_LogoTextureDX12->Release();
        g_LogoTextureDX12 = nullptr;
        return;
    }

    for (UINT mip = 0; mip < mipCount; mip++)
    {
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = uploadBuffer;
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint = footprints[mip];

        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = g_LogoTextureDX12;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = mip;

        commandList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = g_LogoTextureDX12;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    commandList->ResourceBarrier(1, &barrier);

    commandList->Close();
    ID3D12CommandList* lists[] = { commandList };
    commandQueue->ExecuteCommandLists(1, lists);
    commandQueue->Signal(fence, 1);
    if (fence->GetCompletedValue() < 1 && SUCCEEDED(fence->SetEventOnCompletion(1, fenceEvent)))
        WaitForSingleObject(fenceEvent, INFINITE);

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = mipCount;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    device->CreateShaderResourceView(g_LogoTextureDX12, &srvDesc, srvCpu);

    FGUI::LogoTexture = (ImTextureID)srvGpu.ptr;
    FGUI::LogoW = w;
    FGUI::LogoH = h;

    CloseHandle(fenceEvent);
    fence->Release();
    commandList->Release();
    allocator->Release();
    uploadBuffer->Release();
}

static void PushStyle()
{
    ImFontConfig FontConfig;
    FontConfig.FontDataOwnedByAtlas = false;
    ImGui::GetIO().Fonts->AddFontFromMemoryTTF((void*)Font, sizeof(Font), 17.f, &FontConfig);

    ImGuiStyle& s = ImGui::GetStyle();

    s.WindowRounding = 8.f;
    s.ChildRounding = 6.f;
    s.FrameRounding = 4.5f;
    s.GrabRounding = 16.f;
    s.PopupRounding = 4.f;
    s.ScrollbarRounding = 16.f;
    s.TabRounding = 4.f;
    s.WindowPadding = { 16.f, 14.f };
    s.FramePadding = { 10.f,  5.f };
    s.ItemSpacing = { 8.f,  7.f };
    s.ItemInnerSpacing = { 6.f,  4.f };
    s.IndentSpacing = 18.f;
    s.ScrollbarSize = 18.f;
    s.GrabMinSize = 14.f;
    s.WindowBorderSize = 1.f;
    s.FrameBorderSize = 0.f;

    auto C = [](float r, float g, float b, float a = 1.f) { return ImVec4(r, g, b, a); };
    ImVec4* col = s.Colors;

    col[ImGuiCol_WindowBg] = C(0.0667f, 0.0824f, 0.1216f, 1.f); // #11151f
    col[ImGuiCol_ChildBg] = C(0.066f, 0.082f, 0.122f, 1.f);
    col[ImGuiCol_PopupBg] = C(0.066f, 0.082f, 0.122f, 0.98f);
    col[ImGuiCol_Border] = C(0.137f, 0.165f, 0.243f, 1.f);
    col[ImGuiCol_BorderShadow] = C(0.f, 0.f, 0.f, 0.f);
    col[ImGuiCol_FrameBg] = C(0.094f, 0.114f, 0.165f, 1.f);
    col[ImGuiCol_FrameBgHovered] = C(0.12f, 0.145f, 0.21f, 1.f);
    col[ImGuiCol_FrameBgActive] = C(0.14f, 0.17f, 0.25f, 1.f);
    col[ImGuiCol_TitleBg] = C(0.043f, 0.051f, 0.071f, 1.f);
    col[ImGuiCol_TitleBgActive] = C(0.043f, 0.051f, 0.071f, 1.f);
    col[ImGuiCol_TitleBgCollapsed] = C(0.043f, 0.051f, 0.071f, 0.9f);
    col[ImGuiCol_ScrollbarBg] = C(0.043f, 0.051f, 0.071f, 0.f);
    col[ImGuiCol_ScrollbarGrab] = C(0.137f, 0.165f, 0.243f, 1.f);
    col[ImGuiCol_ScrollbarGrabHovered] = Accent(0.5f);
    col[ImGuiCol_ScrollbarGrabActive] = Accent(1.f);
    col[ImGuiCol_CheckMark] = Accent();
    col[ImGuiCol_SliderGrab] = Accent();
    col[ImGuiCol_SliderGrabActive] = AccentDk();
    col[ImGuiCol_Button] = C(0.094f, 0.114f, 0.165f, 1.f);
    col[ImGuiCol_ButtonHovered] = Accent(0.18f);
    col[ImGuiCol_ButtonActive] = Accent(0.32f);
    col[ImGuiCol_Header] = Accent(0.14f);
    col[ImGuiCol_HeaderHovered] = Accent(0.22f);
    col[ImGuiCol_HeaderActive] = Accent(0.32f);
    col[ImGuiCol_Separator] = C(0.137f, 0.165f, 0.243f, 1.f);
    col[ImGuiCol_SeparatorHovered] = Accent(0.4f);
    col[ImGuiCol_SeparatorActive] = Accent(1.f);
    col[ImGuiCol_ResizeGrip] = Accent(0.22f);
    col[ImGuiCol_ResizeGripHovered] = Accent(0.55f);
    col[ImGuiCol_ResizeGripActive] = Accent(0.90f);
    col[ImGuiCol_Tab] = C(0.066f, 0.082f, 0.122f, 1.f);
    col[ImGuiCol_TabHovered] = Accent(0.2f);
    col[ImGuiCol_TabActive] = C(0.094f, 0.114f, 0.165f, 1.f);
    col[ImGuiCol_TabUnfocused] = col[ImGuiCol_Tab];
    col[ImGuiCol_TabUnfocusedActive] = col[ImGuiCol_TabActive];
    col[ImGuiCol_Text] = C(0.847f, 0.878f, 0.941f, 1.f);
    col[ImGuiCol_TextDisabled] = C(0.353f, 0.388f, 0.478f, 1.f);
    col[ImGuiCol_PlotLines] = Accent();
    col[ImGuiCol_PlotLinesHovered] = Accent();
    col[ImGuiCol_PlotHistogram] = Accent();
    col[ImGuiCol_PlotHistogramHovered] = AccentDk();
    col[ImGuiCol_TableHeaderBg] = C(0.066f, 0.082f, 0.122f, 1.f);
    col[ImGuiCol_TableBorderLight] = C(0.137f, 0.165f, 0.243f, 1.f);
    col[ImGuiCol_TableBorderStrong] = C(0.137f, 0.165f, 0.243f, 1.f);
    col[ImGuiCol_TableRowBg] = C(0.f, 0.f, 0.f, 0.f);
    col[ImGuiCol_TableRowBgAlt] = C(1.f, 1.f, 1.f, 0.03f);
    col[ImGuiCol_DragDropTarget] = Accent(0.8f);
    col[ImGuiCol_NavHighlight] = Accent();
    col[ImGuiCol_NavWindowingHighlight] = C(1.f, 1.f, 1.f, 0.7f);
    col[ImGuiCol_NavWindowingDimBg] = C(0.8f, 0.8f, 0.8f, 0.2f);
    col[ImGuiCol_ModalWindowDimBg] = C(0.f, 0.f, 0.f, 0.55f);
}

static void AccentRule()
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    dl->AddLine(ImVec2(p.x, p.y + 1.f), ImVec2(p.x + w, p.y + 1.f),
        ImGui::GetColorU32(Accent(0.25f)), 1.f);
    ImGui::Dummy(ImVec2(0.f, 3.f));
}

static void SectionLabel(const char* label)
{
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, Accent(0.85f));
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
    AccentRule();
    ImGui::Spacing();
}

static bool SidebarTab(const char* label, int index, float height)
{
    ImGui::PushID(index);
    const bool active = (FGUI::ActiveTab == index);

    const ImVec2 size(ImGui::GetContentRegionAvail().x, height);
    const ImVec2 p = ImGui::GetCursorScreenPos();

    const bool pressed = ImGui::InvisibleButton("##tab", size);
    const bool hovered = ImGui::IsItemHovered();

    ImDrawList* dl = ImGui::GetWindowDrawList();

    if (active)
        dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), ImGui::GetColorU32(Accent(0.10f)));
    else if (hovered)
        dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), ImGui::GetColorU32(Accent(0.05f)));

    ImVec4 textCol = active ? Accent() : ImVec4(0.50f, 0.55f, 0.66f, 1.f);
    if (!active && hovered) textCol = ImVec4(0.78f, 0.82f, 0.90f, 1.f);

    const ImVec2 ts = ImGui::CalcTextSize(label);
    dl->AddText(ImVec2(p.x + 26.f, p.y + (size.y - ts.y) * 0.5f), ImGui::GetColorU32(textCol), label);

    if (pressed)
        FGUI::ActiveTab = index;

    ImGui::PopID();
    return pressed;
}

static void QueueCommandError(const wchar_t* message)
{
    if (!message || !*message)
        return;

    GUI_QueueConsoleOutput(message, static_cast<int>(wcslen(message)));
}


static bool IsExecutableAddress(const void* address)
{
    if (!address)
        return false;

    MEMORY_BASIC_INFORMATION region{};
    if (VirtualQuery(
            address, &region, sizeof(region)) !=
        sizeof(region))
    {
        return false;
    }

    const DWORD executableProtection =
        PAGE_EXECUTE | PAGE_EXECUTE_READ |
        PAGE_EXECUTE_READWRITE |
        PAGE_EXECUTE_WRITECOPY;
    return region.State == MEM_COMMIT &&
        (region.Protect &
            (PAGE_GUARD | PAGE_NOACCESS)) == 0 &&
        (region.Protect & executableProtection) != 0;
}

static bool IsReadableRange(
    const void* address,
    size_t length)
{
    if (!address || length == 0)
        return false;

    const uintptr_t begin =
        reinterpret_cast<uintptr_t>(address);
    if (length > SIZE_MAX - begin)
        return false;

    const uintptr_t end = begin + length;
    uintptr_t current = begin;
    while (current < end)
    {
        MEMORY_BASIC_INFORMATION region{};
        if (VirtualQuery(
                reinterpret_cast<const void*>(current),
                &region, sizeof(region)) !=
                sizeof(region) ||
            region.State != MEM_COMMIT ||
            (region.Protect &
                (PAGE_GUARD | PAGE_NOACCESS)) != 0 ||
            region.RegionSize == 0)
        {
            return false;
        }

        const uintptr_t regionBegin =
            reinterpret_cast<uintptr_t>(
                region.BaseAddress);
        if (region.RegionSize >
            SIZE_MAX - regionBegin)
        {
            return false;
        }

        const uintptr_t regionEnd =
            regionBegin + region.RegionSize;
        if (regionEnd <= current)
            return false;

        current = (std::min)(end, regionEnd);
    }
    return true;
}

// UE4's Kismet ExecuteConsoleCommand ultimately invokes the virtual
// APlayerController::ConsoleCommand method with bWriteToLog=true, which throws
// away the FString it builds. Resolve that exact virtual slot from Kismet's
// native implementation and call it with bWriteToLog=false so the normal UE
// response can be copied into the ATLAS console without intercepting an
// uncertain UConsole vtable entry.
using PlayerConsoleCommandNative =
    FString*(*)(UObject*, FString*, const FString*, bool);

static std::atomic_size_t g_PlayerConsoleCommandIndex = SIZE_MAX;
static ULONGLONG g_NextPlayerConsoleCommandResolveAt = 0;

static bool GetBoundedFunctionRange(
    uintptr_t address,
    size_t maximumSize,
    uintptr_t* functionStart,
    uintptr_t* functionEnd)
{
    if (!address || !functionStart || !functionEnd ||
        !IsExecutableAddress(
            reinterpret_cast<void*>(address)))
    {
        return false;
    }

    DWORD64 imageBase = 0;
    const PRUNTIME_FUNCTION runtimeFunction =
        RtlLookupFunctionEntry(
            static_cast<DWORD64>(address),
            &imageBase, nullptr);
    if (!runtimeFunction || !imageBase)
        return false;

    const uintptr_t start =
        static_cast<uintptr_t>(
            imageBase + runtimeFunction->BeginAddress);
    const uintptr_t end =
        static_cast<uintptr_t>(
            imageBase + runtimeFunction->EndAddress);
    if (address < start || address >= end ||
        end <= start ||
        static_cast<size_t>(end - start) >
            maximumSize)
    {
        return false;
    }

    *functionStart = start;
    *functionEnd = end;
    return true;
}

static uintptr_t FollowSimpleJump(uintptr_t address)
{
    for (unsigned int depth = 0; depth < 2; depth++)
    {
        if (!IsExecutableAddress(
                reinterpret_cast<void*>(address)))
        {
            return 0;
        }

        __try
        {
            const auto bytes =
                reinterpret_cast<const uint8_t*>(address);
            if (bytes[0] == 0xE9)
            {
                const int32_t relative =
                    *reinterpret_cast<const int32_t*>(
                        address + 1);
                address = address + 5 + relative;
                continue;
            }

            if (bytes[0] == 0xFF &&
                bytes[1] == 0x25)
            {
                const int32_t relative =
                    *reinterpret_cast<const int32_t*>(
                        address + 2);
                address =
                    *reinterpret_cast<const uintptr_t*>(
                        address + 6 + relative);
                continue;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }

        break;
    }
    return address;
}

static bool DecodeRegisterMove(
    const uint8_t* bytes,
    unsigned int* destination,
    unsigned int* source)
{
    if (!bytes || !destination || !source)
        return false;

    const uint8_t rex = bytes[0];
    if ((rex & 0xF8) != 0x48)
        return false;

    const uint8_t opcode = bytes[1];
    const uint8_t modRm = bytes[2];
    if ((opcode != 0x8B && opcode != 0x89) ||
        (modRm & 0xC0) != 0xC0)
    {
        return false;
    }

    const unsigned int reg =
        ((modRm >> 3) & 7u) |
        ((rex & 0x04) ? 8u : 0u);
    const unsigned int rm =
        (modRm & 7u) |
        ((rex & 0x01) ? 8u : 0u);
    if (opcode == 0x8B)
    {
        *destination = reg;
        *source = rm;
    }
    else
    {
        *destination = rm;
        *source = reg;
    }
    return true;
}

static bool LooksLikePlayerConsoleCommand(
    uintptr_t target,
    uint32_t playerOffset)
{
    target = FollowSimpleJump(target);
    if (!target || playerOffset == UINT32_MAX)
        return false;

    uintptr_t functionStart = 0;
    uintptr_t functionEnd = 0;
    if (!GetBoundedFunctionRange(
            target, 0x100,
            &functionStart, &functionEnd))
    {
        return false;
    }

    // A vtable entry must point at the beginning of the resolved runtime
    // function (after following a short import/hot-patch jump). Rejecting
    // interior addresses prevents an unrelated byte sequence from being
    // treated as a complete ConsoleCommand wrapper.
    if (target != functionStart)
        return false;

    uintptr_t savesResultAt = 0;
    uintptr_t loadsPlayerAt = 0;
    uintptr_t testsPlayerAt = 0;
    uintptr_t callsPlayerAt = 0;
    uintptr_t restoresResultAt = 0;
    uintptr_t returnsAt = 0;
    int savedResultRegister = -1;

    __try
    {
        for (uintptr_t cursor = functionStart;
            cursor < functionEnd; cursor++)
        {
            const auto bytes =
                reinterpret_cast<const uint8_t*>(cursor);

            unsigned int destination = 0;
            unsigned int source = 0;
            if (cursor + 3 <= functionEnd &&
                DecodeRegisterMove(
                    bytes, &destination, &source))
            {
                if (!savesResultAt &&
                    cursor - functionStart <= 32 &&
                    source == 2 &&
                    (destination == 3 ||
                        destination == 5 ||
                        destination == 6 ||
                        destination == 7 ||
                        destination >= 12))
                {
                    savesResultAt = cursor;
                    savedResultRegister =
                        static_cast<int>(destination);
                }

                if (callsPlayerAt &&
                    !restoresResultAt &&
                    destination == 0 &&
                    savedResultRegister >= 0 &&
                    source ==
                        static_cast<unsigned int>(
                            savedResultRegister))
                {
                    restoresResultAt = cursor;
                }
            }

            if (!loadsPlayerAt &&
                cursor - functionStart <= 32 &&
                ((cursor + 7 <= functionEnd &&
                    bytes[0] == 0x48 &&
                    bytes[1] == 0x8B &&
                    bytes[2] == 0x89 &&
                    *reinterpret_cast<const uint32_t*>(
                        cursor + 3) == playerOffset) ||
                    (cursor + 4 <= functionEnd &&
                        bytes[0] == 0x48 &&
                        bytes[1] == 0x8B &&
                        bytes[2] == 0x49 &&
                        playerOffset <= 0x7F &&
                        bytes[3] ==
                            static_cast<uint8_t>(
                                playerOffset))))
            {
                loadsPlayerAt = cursor;
            }

            if (!testsPlayerAt &&
                loadsPlayerAt &&
                savesResultAt &&
                cursor > (std::max)(
                    loadsPlayerAt, savesResultAt) &&
                cursor - (std::max)(
                    loadsPlayerAt, savesResultAt) <= 12 &&
                cursor + 3 <= functionEnd &&
                bytes[0] == 0x48 &&
                bytes[1] == 0x85 &&
                bytes[2] == 0xC9)
            {
                testsPlayerAt = cursor;
            }

            if (!callsPlayerAt &&
                testsPlayerAt &&
                cursor > testsPlayerAt &&
                cursor - testsPlayerAt <= 16 &&
                bytes[0] == 0xE8 &&
                cursor + 5 <= functionEnd)
            {
                callsPlayerAt = cursor;
            }

            if (!returnsAt &&
                restoresResultAt &&
                cursor > restoresResultAt &&
                cursor - restoresResultAt <= 16 &&
                bytes[0] == 0xC3)
            {
                returnsAt = cursor;
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }

    return savesResultAt &&
        loadsPlayerAt &&
        ((savesResultAt > loadsPlayerAt
                ? savesResultAt - loadsPlayerAt
                : loadsPlayerAt - savesResultAt) <= 16) &&
        testsPlayerAt &&
        callsPlayerAt &&
        restoresResultAt &&
        returnsAt;
}

static uintptr_t FindResultBufferWrite(
    uintptr_t start,
    uintptr_t end)
{
    for (uintptr_t cursor = start;
        cursor + 5 <= end; cursor++)
    {
        const auto bytes =
            reinterpret_cast<const uint8_t*>(cursor);
        if (bytes[0] == 0x48 &&
            bytes[1] == 0x8D &&
            bytes[2] == 0x54 &&
            bytes[3] == 0x24)
        {
            return cursor;
        }

        if (cursor + 8 <= end &&
            bytes[0] == 0x48 &&
            bytes[1] == 0x8D &&
            bytes[2] == 0x94 &&
            bytes[3] == 0x24)
        {
            return cursor;
        }
    }
    return 0;
}

static uintptr_t FindR9One(
    uintptr_t start,
    uintptr_t end)
{
    for (uintptr_t cursor = start;
        cursor + 3 <= end; cursor++)
    {
        const auto bytes =
            reinterpret_cast<const uint8_t*>(cursor);
        if (bytes[0] == 0x41 &&
            bytes[1] == 0xB1 &&
            bytes[2] == 0x01)
        {
            return cursor;
        }

        if (cursor + 6 <= end &&
            bytes[0] == 0x41 &&
            bytes[1] == 0xB9 &&
            *reinterpret_cast<const uint32_t*>(
                cursor + 2) == 1)
        {
            return cursor;
        }
    }
    return 0;
}

static uintptr_t FindR8Write(
    uintptr_t start,
    uintptr_t end)
{
    for (uintptr_t cursor = start;
        cursor + 3 <= end; cursor++)
    {
        const auto bytes =
            reinterpret_cast<const uint8_t*>(cursor);
        const uint8_t rex = bytes[0];
        if ((rex & 0xF8) == 0x48 &&
            (rex & 0x04) != 0 &&
            (bytes[1] == 0x8B ||
                bytes[1] == 0x8D) &&
            (bytes[2] & 0x38) == 0)
        {
            return cursor;
        }
    }
    return 0;
}

static bool DecodeVtableLoad(
    const uint8_t* bytes,
    unsigned int* vtableRegister,
    unsigned int* controllerRegister)
{
    if (!bytes || !vtableRegister ||
        !controllerRegister)
        return false;

    const uint8_t rex = bytes[0];
    const uint8_t modRm = bytes[2];
    if ((rex & 0xF8) != 0x48 ||
        bytes[1] != 0x8B ||
        (modRm & 0xC0) != 0 ||
        (modRm & 7u) == 4u ||
        (modRm & 7u) == 5u)
    {
        return false;
    }

    *vtableRegister =
        ((modRm >> 3) & 7u) |
        ((rex & 0x04) ? 8u : 0u);
    *controllerRegister =
        (modRm & 7u) |
        ((rex & 0x01) ? 8u : 0u);
    return true;
}

static bool DecodeVirtualCall(
    uintptr_t address,
    uintptr_t end,
    unsigned int* vtableRegister,
    uint32_t* byteOffset,
    size_t* instructionLength)
{
    if (!address || !vtableRegister ||
        !byteOffset || !instructionLength ||
        address >= end)
    {
        return false;
    }

    const auto start =
        reinterpret_cast<const uint8_t*>(address);
    size_t prefixLength = 0;
    uint8_t rex = 0;
    if ((start[0] & 0xF0) == 0x40)
    {
        rex = start[0];
        prefixLength = 1;
    }

    if (address + prefixLength + 6 > end)
        return false;

    const auto bytes = start + prefixLength;
    const uint8_t modRm = bytes[1];
    if (bytes[0] != 0xFF ||
        (modRm & 0xC0) != 0x80 ||
        ((modRm >> 3) & 7u) != 2u ||
        (rex & 0x06) != 0)
    {
        return false;
    }

    const unsigned int rm = modRm & 7u;
    size_t displacementOffset = 2;
    unsigned int baseRegister = rm;
    if (rm == 4u)
    {
        if (address + prefixLength + 7 > end)
            return false;

        const uint8_t sib = bytes[2];
        // A vtable call only needs a base register. Reject indexed SIB forms
        // so an unrelated array-style indirect call cannot qualify.
        if (((sib >> 3) & 7u) != 4u ||
            (rex & 0x02) != 0)
        {
            return false;
        }
        baseRegister = sib & 7u;
        displacementOffset = 3;
    }

    baseRegister |=
        (rex & 0x01) ? 8u : 0u;
    *vtableRegister = baseRegister;
    *byteOffset =
        *reinterpret_cast<const uint32_t*>(
            bytes + displacementOffset);
    *instructionLength =
        prefixLength + displacementOffset + 4;
    return true;
}

static bool MatchesPlayerConsoleCallSite(
    uintptr_t start,
    uintptr_t call,
    unsigned int callVtableRegister)
{
    // Known optimized UE4 builds emit this dataflow in order:
    //   vtable load -> result buffer -> true -> command -> this -> call.
    // Requiring both the order and short instruction gaps avoids combining
    // unrelated bytes from the surrounding implementation.
    for (uintptr_t vtableLoad = start;
        vtableLoad + 3 <= call; vtableLoad++)
    {
        unsigned int vtableRegister = 0;
        unsigned int controllerRegister = 0;
        if (!DecodeVtableLoad(
                reinterpret_cast<const uint8_t*>(
                    vtableLoad),
                &vtableRegister,
                &controllerRegister))
        {
            continue;
        }
        if (vtableRegister != callVtableRegister)
            continue;

        const uintptr_t resultBuffer =
            FindResultBufferWrite(
                vtableLoad + 3,
                (std::min)(call, vtableLoad + 15));
        if (!resultBuffer)
            continue;

        const uintptr_t r9One =
            FindR9One(
                resultBuffer + 3,
                (std::min)(call, resultBuffer + 15));
        if (!r9One)
            continue;

        const uintptr_t r8Write =
            FindR8Write(
                r9One + 3,
                (std::min)(call, r9One + 15));
        if (!r8Write)
            continue;

        for (uintptr_t restoresRcx = r8Write + 3;
            restoresRcx + 3 <= call &&
                restoresRcx < r8Write + 15;
            restoresRcx++)
        {
            unsigned int destination = 0;
            unsigned int source = 0;
            if (DecodeRegisterMove(
                    reinterpret_cast<const uint8_t*>(
                        restoresRcx),
                    &destination, &source) &&
                destination == 1 &&
                source == controllerRegister &&
                call - restoresRcx <= 8)
            {
                return true;
            }
        }
    }
    return false;
}

static bool FindPlayerConsoleCommandIndex(
    uintptr_t implementation,
    UObject* controller,
    size_t* resolvedIndex)
{
    if (!controller || !controller->Vft ||
        !resolvedIndex)
    {
        return false;
    }

    uintptr_t functionStart = 0;
    uintptr_t functionEnd = 0;
    implementation = FollowSimpleJump(implementation);
    if (!implementation ||
        !GetBoundedFunctionRange(
            implementation, 0x1000,
            &functionStart, &functionEnd))
    {
        return false;
    }

    const uint32_t playerOffset =
        controller->GetOffset("Player");
    if (playerOffset == UINT32_MAX)
        return false;

    size_t uniqueIndex = SIZE_MAX;
    __try
    {
        for (uintptr_t call = functionStart;
            call < functionEnd; call++)
        {
            unsigned int callVtableRegister = 0;
            uint32_t byteOffset = 0;
            size_t callLength = 0;
            if (!DecodeVirtualCall(
                    call, functionEnd,
                    &callVtableRegister,
                    &byteOffset,
                    &callLength))
            {
                continue;
            }

            if (callLength < 6 ||
                (byteOffset & 7u) != 0)
            {
                continue;
            }

            const size_t index =
                static_cast<size_t>(byteOffset / 8u);
            if (index >= 768)
                continue;

            const uintptr_t windowStart =
                call > functionStart + 48
                    ? call - 48
                    : functionStart;
            if (!MatchesPlayerConsoleCallSite(
                    windowStart, call,
                    callVtableRegister))
            {
                continue;
            }

            const uintptr_t target =
                reinterpret_cast<uintptr_t>(
                    controller->Vft[index]);
            if (!LooksLikePlayerConsoleCommand(
                    target, playerOffset))
            {
                continue;
            }

            if (uniqueIndex != SIZE_MAX &&
                uniqueIndex != index)
            {
                return false;
            }
            uniqueIndex = index;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }

    if (uniqueIndex == SIZE_MAX)
        return false;

    *resolvedIndex = uniqueIndex;
    return true;
}

static bool TryResolvePlayerConsoleCommand(
    UObject* controller)
{
    if (g_PlayerConsoleCommandIndex.load(
            std::memory_order_acquire) != SIZE_MAX)
    {
        return true;
    }

    const ULONGLONG now = GetTickCount64();
    if (now < g_NextPlayerConsoleCommandResolveAt)
        return false;
    g_NextPlayerConsoleCommandResolveAt = now + 1000;

    auto library =
        UKismetSystemLibrary::GetDefaultObj();
    UFunction* function =
        library
            ? library->GetFunction(
                "ExecuteConsoleCommand")
            : nullptr;
    const uintptr_t nativeWrapper =
        function
            ? reinterpret_cast<uintptr_t>(
                function->GetNativeFunc())
            : 0;

    uintptr_t wrapperStart = 0;
    uintptr_t wrapperEnd = 0;
    if (!nativeWrapper ||
        !GetBoundedFunctionRange(
            nativeWrapper, 0x1000,
            &wrapperStart, &wrapperEnd))
    {
        return false;
    }

    size_t resolvedIndex = SIZE_MAX;
    bool sawSetNotZero = false;
    __try
    {
        for (uintptr_t cursor = wrapperStart;
            cursor < wrapperEnd; cursor++)
        {
            const auto bytes =
                reinterpret_cast<const uint8_t*>(cursor);
            if (cursor + 2 <= wrapperEnd &&
                bytes[0] == 0x0F &&
                bytes[1] == 0x95)
            {
                sawSetNotZero = true;
                continue;
            }

            if (!sawSetNotZero ||
                cursor + 5 > wrapperEnd ||
                (bytes[0] != 0xE8 &&
                    bytes[0] != 0xE9))
            {
                continue;
            }

            const int32_t relative =
                *reinterpret_cast<const int32_t*>(
                    cursor + 1);
            const uintptr_t candidate =
                cursor + 5 + relative;
            if (FindPlayerConsoleCommandIndex(
                    candidate, controller,
                    &resolvedIndex))
            {
                break;
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }

    if (resolvedIndex == SIZE_MAX)
    {
        AtlasDiagnostics::WriteLine(
            "console-return-path resolve-failed "
            "wrapper=%p",
            reinterpret_cast<void*>(nativeWrapper));
        return false;
    }

    g_PlayerConsoleCommandIndex.store(
        resolvedIndex, std::memory_order_release);
    AtlasDiagnostics::WriteLine(
        "console-return-path resolved index=%zu "
        "target=%p",
        resolvedIndex,
        controller->Vft[resolvedIndex]);
    return true;
}

static PlayerConsoleCommandNative ReadPlayerConsoleCommand(
    UObject* controller,
    size_t index)
{
    __try
    {
        return controller && controller->Vft
            ? reinterpret_cast<PlayerConsoleCommandNative>(
                controller->Vft[index])
            : nullptr;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return nullptr;
    }
}

static bool InvokePlayerConsoleCommand(
    PlayerConsoleCommandNative native,
    UObject* controller,
    FString* output,
    const FString* command)
{
    __try
    {
        native(controller, output, command, false);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

static bool FreeReturnedConsoleOutput(
    FString* output)
{
    __try
    {
        output->Free();
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

static bool QueueReturnedConsoleOutput(
    FString* output,
    int length)
{
    __try
    {
        if (length > 0 &&
            output->Data[length - 1] == L'\0')
        {
            length--;
        }

        if (length > 0)
        {
            GUI_QueueConsoleOutput(
                output->Data, length);
        }
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

static bool ExecuteConsoleCommandWithReturnedOutput(
    UObject* controller,
    const FString& command)
{
    // UE5's Kismet path can route CVars through IConsoleManager before it ever
    // reaches PlayerController, so preserve Kismet dispatch there. UE4 routes
    // this same call directly through PlayerController and can safely expose
    // the otherwise-discarded FString response.
    if (VersionInfo.EngineVersion >= 5.0 ||
        !TryResolvePlayerConsoleCommand(controller))
    {
        return false;
    }

    const size_t index =
        g_PlayerConsoleCommandIndex.load(
            std::memory_order_acquire);
    if (index == SIZE_MAX || index >= 768)
        return false;

    const PlayerConsoleCommandNative native =
        ReadPlayerConsoleCommand(controller, index);
    if (!IsExecutableAddress(
            reinterpret_cast<void*>(native)))
    {
        return false;
    }

    FString output{};
    if (!InvokePlayerConsoleCommand(
            native, controller, &output, &command))
    {
        g_PlayerConsoleCommandIndex.store(
            SIZE_MAX, std::memory_order_release);
        AtlasDiagnostics::WriteLine(
            "console-return-path invoke-failed target=%p",
            reinterpret_cast<void*>(native));
        QueueCommandError(
            L"Command error: Unreal console dispatch failed.");
        // The native call may already have performed part or all of the
        // command before faulting. Treat it as consumed so the Kismet fallback
        // cannot execute a state-changing command twice.
        return true;
    }

    const bool emptyOutput =
        !output.Data &&
        output.NumElements == 0 &&
        output.MaxElements == 0;
    const bool allocatedOutput =
        output.Data &&
        output.NumElements >= 0 &&
        output.MaxElements >= output.NumElements &&
        output.MaxElements > 0 &&
        output.MaxElements <= 65536 &&
        IsReadableRange(
            output.Data,
            static_cast<size_t>(
                (std::max)(output.NumElements, 1)) *
                sizeof(wchar_t));
    if (!emptyOutput && !allocatedOutput)
    {
        g_PlayerConsoleCommandIndex.store(
            SIZE_MAX, std::memory_order_release);
        AtlasDiagnostics::WriteLine(
            "console-return-path invalid-result "
            "data=%p num=%d max=%d",
            output.Data,
            output.NumElements,
            output.MaxElements);
        QueueCommandError(
            L"Command ran, but Unreal returned invalid console output.");
        return true;
    }

    if (allocatedOutput &&
        output.NumElements > 0)
    {
        const int length =
            (std::min)(output.NumElements, 16384);
        if (!QueueReturnedConsoleOutput(
                &output, length))
        {
            AtlasDiagnostics::WriteLine(
                "console-return-path copy-failed "
                "data=%p num=%d max=%d",
                output.Data,
                output.NumElements,
                output.MaxElements);
        }
    }

    if (allocatedOutput &&
        !FreeReturnedConsoleOutput(&output))
    {
        AtlasDiagnostics::WriteLine(
            "console-return-path free-failed");
    }
    return true;
}

static bool ExecNow(const char* cmd, bool showConsoleErrors = true)
{
    if (!cmd || !*cmd)
        return false;

    auto world = UWorld::GetWorld();
    if (!world || !world->OwningGameInstance)
    {
        AtlasDiagnostics::WriteLine("command-skip no-world command=%s", cmd);
        if (showConsoleErrors)
            QueueCommandError(L"Command error: no active Unreal world.");
        return false;
    }

    auto& localPlayers = world->OwningGameInstance->LocalPlayers;
    if (localPlayers.Num() == 0 || !localPlayers[0] || !localPlayers[0]->PlayerController)
    {
        AtlasDiagnostics::WriteLine("command-skip no-player command=%s", cmd);
        if (showConsoleErrors)
            QueueCommandError(L"Command error: no local player controller.");
        return false;
    }

    const int len = MultiByteToWideChar(CP_UTF8, 0, cmd, -1, nullptr, 0);
    if (len <= 1)
    {
        if (showConsoleErrors)
            QueueCommandError(L"Command error: invalid UTF-8 input.");
        return false;
    }

    std::wstring wide(static_cast<size_t>(len), L'\0');
    if (MultiByteToWideChar(CP_UTF8, 0, cmd, -1, wide.data(), len) != len)
    {
        if (showConsoleErrors)
            QueueCommandError(L"Command error: input conversion failed.");
        return false;
    }
    wide.resize(static_cast<size_t>(len - 1));

    UObject* activeController =
        localPlayers[0]->PlayerController;
    FString command(wide.c_str());
    AtlasDiagnostics::WriteLine(
        "command-dispatch world=%p pc=%p command=%s",
        world, activeController, cmd);

    const bool usedReturnedOutput =
        ExecuteConsoleCommandWithReturnedOutput(
            activeController, command);
    if (!usedReturnedOutput)
    {
        UKismetSystemLibrary::ExecuteConsoleCommand(
            world, command, activeController);
    }

    AtlasDiagnostics::WriteLine(
        "command-return path=%s command=%s",
        usedReturnedOutput ? "player-result" : "kismet",
        cmd);
    command.Free();
    return true;
}

struct FGameThreadCommand
{
    std::string Command;
    bool ShowConsoleErrors = true;
};

// Present owns the UI scheduler, while Unreal command execution must happen
// on the game thread. A one-item mailbox provides strict backpressure: rapid
// binds may fill the regular bounded queue, but they can never re-enter
// ProcessEvent or build an unbounded game-thread backlog.
static std::mutex g_GameThreadCommandMutex;
static std::deque<FGameThreadCommand> g_GameThreadCommands;
static std::atomic_bool g_GameThreadDispatcherReady = false;
static std::atomic_bool g_GameThreadCommandExecuting = false;
static std::atomic_uint64_t g_GameplayMouseRestoreGeneration = 0;
static std::atomic_uint64_t g_GameplayMouseRestoreHandledGeneration = 0;
static std::atomic<HWND> g_GameplayMouseRestoreWindow = nullptr;
static std::atomic<ULONGLONG> g_GameThreadCommandReadyAfter = 0;
static constexpr size_t kMaximumGameThreadCommands = 1;
static constexpr ULONGLONG kGameThreadCommandSettleMs = 100;

void GUI_RequestGameplayMouseRestore(HWND gameWindow)
{
    if (!gameWindow)
        return;

    g_GameplayMouseRestoreWindow.store(
        gameWindow, std::memory_order_release);
    g_GameplayMouseRestoreGeneration.fetch_add(
        1, std::memory_order_acq_rel);
}

void GUI_CancelGameplayMouseRestore()
{
    g_GameplayMouseRestoreHandledGeneration.store(
        g_GameplayMouseRestoreGeneration.load(
            std::memory_order_acquire),
        std::memory_order_release);
}

void GUI_SetGameThreadDispatcherReady(bool ready)
{
    g_GameThreadDispatcherReady.store(ready, std::memory_order_release);
    if (ready)
        return;

    g_GameThreadCommandReadyAfter.store(
        0, std::memory_order_release);
    std::unique_lock<std::mutex> lock(
        g_GameThreadCommandMutex, std::try_to_lock);
    if (lock.owns_lock())
        g_GameThreadCommands.clear();
}

static bool HandOffGameThreadCommand(
    const std::string& command,
    bool showConsoleErrors = true)
{
    if (command.empty() ||
        !g_GameThreadDispatcherReady.load(std::memory_order_acquire) ||
        g_GameThreadCommandExecuting.load(std::memory_order_acquire) ||
        GetTickCount64() <
            g_GameThreadCommandReadyAfter.load(
                std::memory_order_acquire))
    {
        return false;
    }

    std::unique_lock<std::mutex> lock(
        g_GameThreadCommandMutex, std::try_to_lock);
    if (!lock.owns_lock() ||
        g_GameThreadCommandExecuting.load(std::memory_order_acquire) ||
        g_GameThreadCommands.size() >= kMaximumGameThreadCommands)
    {
        return false;
    }

    g_GameThreadCommands.push_back(
        FGameThreadCommand{ command, showConsoleErrors });
    AtlasDiagnostics::WriteLine(
        "command-handoff depth=%zu command=%s",
        g_GameThreadCommands.size(), command.c_str());
    return true;
}

static bool RestoreGameplayMouseNow()
{
    auto world = UWorld::GetWorld();
    if (!world || !world->OwningGameInstance)
        return false;

    auto& localPlayers = world->OwningGameInstance->LocalPlayers;
    if (localPlayers.Num() == 0 || !localPlayers[0] ||
        !localPlayers[0]->PlayerController)
    {
        return false;
    }

    // Present detects the overlay transition, but reflected Unreal calls must
    // stay on this game-thread pump. SetInputMode_GameOnly reapplies Slate's
    // viewport focus, high-precision movement, capture, and lock policy.
    static UObject* widgetLibrary = nullptr;
    static UFunction* setInputModeGameOnly = nullptr;
    static ULONGLONG nextResolveAttemptAt = 0;
    if (!widgetLibrary || !setInputModeGameOnly)
    {
        const ULONGLONG now = GetTickCount64();
        if (now < nextResolveAttemptAt)
            return false;
        nextResolveAttemptAt = now + 1000;

        if (!widgetLibrary)
        {
            const UClass* widgetLibraryClass =
                FindClass("WidgetBlueprintLibrary");
            if (!widgetLibraryClass)
                return false;

            widgetLibrary = widgetLibraryClass->GetDefaultObj();
        }

        if (widgetLibrary)
        {
            setInputModeGameOnly =
                widgetLibrary->GetFunction(
                    "SetInputMode_GameOnly");
        }
    }
    if (!widgetLibrary || !setInputModeGameOnly)
        return false;

    // UE5 adds bFlushInput after the player-controller parameter. The
    // reflective caller ignores this extra argument on older one-parameter
    // versions, keeping the restore compatible across supported builds.
    widgetLibrary->Call<void>(
        setInputModeGameOnly,
        localPlayers[0]->PlayerController,
        false);
    return true;
}

static void PumpGameplayMouseRestore()
{
    const uint64_t mouseRestoreGeneration =
        g_GameplayMouseRestoreGeneration.load(
            std::memory_order_acquire);
    const uint64_t handledMouseRestoreGeneration =
        g_GameplayMouseRestoreHandledGeneration.load(
            std::memory_order_acquire);
    const HWND mouseRestoreWindow =
        g_GameplayMouseRestoreWindow.load(
            std::memory_order_acquire);
    if (mouseRestoreGeneration !=
            handledMouseRestoreGeneration &&
        mouseRestoreWindow &&
        GetForegroundWindow() == mouseRestoreWindow &&
        !GUI_IsOverlayVisible() &&
        RestoreGameplayMouseNow())
    {
        // Do not erase a newer close request that arrived while ProcessEvent
        // was applying the input mode. Also leave this request pending if the
        // overlay reopened or focus changed during that call.
        if (GetForegroundWindow() == mouseRestoreWindow &&
            !GUI_IsOverlayVisible())
        {
            uint64_t handledGeneration =
                g_GameplayMouseRestoreHandledGeneration.load(
                    std::memory_order_acquire);
            while (handledGeneration <
                    mouseRestoreGeneration &&
                !g_GameplayMouseRestoreHandledGeneration
                    .compare_exchange_weak(
                        handledGeneration,
                        mouseRestoreGeneration,
                        std::memory_order_acq_rel,
                        std::memory_order_acquire))
            {
            }
        }
    }
}

void GUI_PumpGameThreadCommands()
{
    bool expected = false;
    if (!g_GameThreadCommandExecuting.compare_exchange_strong(
        expected, true, std::memory_order_acq_rel,
        std::memory_order_acquire))
    {
        return;
    }

    FGameThreadCommand pending{};
    {
        std::unique_lock<std::mutex> lock(
            g_GameThreadCommandMutex, std::try_to_lock);
        if (!lock.owns_lock() || g_GameThreadCommands.empty())
        {
            g_GameThreadCommandExecuting.store(
                false, std::memory_order_release);
            PumpGameplayMouseRestore();
            return;
        }

        pending = std::move(g_GameThreadCommands.front());
        g_GameThreadCommands.pop_front();
    }

    // Do not hold the mailbox, ImGui, or render mutex across ProcessEvent.
    ExecNow(
        pending.Command.c_str(),
        pending.ShowConsoleErrors);
    g_GameThreadCommandReadyAfter.store(
        GetTickCount64() + kGameThreadCommandSettleMs,
        std::memory_order_release);
    g_GameThreadCommandExecuting.store(
        false, std::memory_order_release);

    // A command such as ToggleDebugCamera can synchronously replace the local
    // player's active controller. Resolve and restore input after dispatch so
    // GameOnly mode is applied to the controller that now owns gameplay.
    PumpGameplayMouseRestore();
}

static std::deque<std::string> g_CommandQueue;
static ULONGLONG g_LastCommandDispatch = 0;
static bool g_LastScheduledCommandWasMacro = false;
static constexpr ULONGLONG kMinimumCommandIntervalMs = 100;
static constexpr size_t kMaximumQueuedMacroRuns = 8;

struct QueuedMacroRun
{
    std::string Name;
    std::vector<HotkeyPersist::MacroStep> Steps;
    size_t NextStep = 0;
    ULONGLONG NextDispatchAt = 0;
};

static std::deque<QueuedMacroRun> g_MacroRunQueue;

static bool Exec(const char* cmd)
{
    if (!cmd || !*cmd)
        return false;

    if (!g_GameThreadDispatcherReady.load(std::memory_order_acquire))
    {
        AtlasDiagnostics::WriteLine(
            "command-drop dispatcher-unavailable command=%s", cmd);
        return false;
    }

    // Slider updates only need the newest FOV value; allowing old values to
    // accumulate would delay later commands unnecessarily.
    if (strncmp(cmd, "fov", 3) == 0 && (cmd[3] == '\0' || cmd[3] == ' '))
    {
        for (auto it = g_CommandQueue.begin(); it != g_CommandQueue.end();)
        {
            if (it->compare(0, 3, "fov") == 0 && (it->size() == 3 || (*it)[3] == ' '))
                it = g_CommandQueue.erase(it);
            else
                ++it;
        }
    }

    if (g_CommandQueue.size() < 32)
    {
        g_CommandQueue.emplace_back(cmd);
        AtlasDiagnostics::WriteLine("command-enqueue depth=%zu command=%s", g_CommandQueue.size(), cmd);
        return true;
    }

    AtlasDiagnostics::WriteLine("command-drop queue-full command=%s", cmd);
    return false;
}

static void QueueMacro(const HotkeyPersist::CommandMacro& macro)
{
    if (macro.Steps.empty())
        return;

    if (g_MacroRunQueue.size() >= kMaximumQueuedMacroRuns)
    {
        AtlasDiagnostics::WriteLine("macro-drop queue-full macro=%s", macro.Name.c_str());
        return;
    }

    QueuedMacroRun run{};
    run.Name = macro.Name;
    run.Steps.reserve(macro.Steps.size());

    for (const auto& step : macro.Steps)
    {
        if (step.Command.empty())
            continue;

        HotkeyPersist::MacroStep copy = step;
        copy.DelayMs = HotkeyPersist::SanitizeMacroDelayMs(copy.DelayMs);
        run.Steps.push_back(std::move(copy));
    }

    if (run.Steps.empty())
        return;

    g_MacroRunQueue.push_back(std::move(run));
    AtlasDiagnostics::WriteLine("macro-enqueue depth=%zu macro=%s steps=%zu", g_MacroRunQueue.size(),
        macro.Name.c_str(), macro.Steps.size());
}

static void RefreshInitialHelpSession(bool armIfVisible)
{
    const uint64_t session = Client::GetConsoleSessionGeneration();
    if (session != g_ConsoleUi.InitialHelpSession)
    {
        g_ConsoleUi.InitialHelpSession = session;
        g_ConsoleUi.InitialHelpRequested = false;
        g_ConsoleUi.InitialHelpPending = false;
        g_ConsoleUi.InitialHelpAttempts = 0;
        g_ConsoleUi.NextInitialHelpAttemptAt = 0;
    }

    if (armIfVisible && session != 0 &&
        !Client::HasReceivedServerCommandList() &&
        !g_ConsoleUi.InitialHelpRequested)
    {
        g_ConsoleUi.InitialHelpRequested = true;
        g_ConsoleUi.InitialHelpPending = true;
        g_ConsoleUi.InitialHelpAttempts = 0;
        g_ConsoleUi.NextInitialHelpAttemptAt = 0;
    }
}

static void DispatchQueuedCommand()
{
    RefreshInitialHelpSession(
        FGUI::bConsoleVisible.load(std::memory_order_acquire));

    const ULONGLONG now = GetTickCount64();
    if (g_LastCommandDispatch != 0 && now - g_LastCommandDispatch < kMinimumCommandIntervalMs)
        return;

    if (g_ConsoleUi.InitialHelpPending &&
        Client::HasReceivedServerCommandList())
    {
        g_ConsoleUi.InitialHelpPending = false;
    }

    if (g_ConsoleUi.InitialHelpPending &&
        g_ConsoleUi.InitialHelpAttempts >=
            kMaximumInitialHelpAttempts)
    {
        g_ConsoleUi.InitialHelpPending = false;
    }

    while (!g_MacroRunQueue.empty() &&
        g_MacroRunQueue.front().NextStep >=
            g_MacroRunQueue.front().Steps.size())
    {
        g_MacroRunQueue.pop_front();
    }

    const bool macroDue =
        !g_MacroRunQueue.empty() &&
        (g_MacroRunQueue.front().NextDispatchAt == 0 ||
            now >= g_MacroRunQueue.front().NextDispatchAt);

    auto dispatchRegular = [&]() -> bool
    {
        if (g_CommandQueue.empty())
            return false;

        const std::string& command = g_CommandQueue.front();
        if (!HandOffGameThreadCommand(command))
            return false;

        g_CommandQueue.pop_front();
        g_LastCommandDispatch = now;
        g_LastScheduledCommandWasMacro = false;
        return true;
    };

    auto dispatchMacro = [&]() -> bool
    {
        if (!macroDue || g_MacroRunQueue.empty())
            return false;

        auto& run = g_MacroRunQueue.front();
        const HotkeyPersist::MacroStep step =
            run.Steps[run.NextStep];
        if (!HandOffGameThreadCommand(step.Command))
            return false;

        run.NextStep++;
        const bool completed =
            run.NextStep >= run.Steps.size();
        const std::string macroName = run.Name;
        const size_t stepNumber = run.NextStep;
        if (completed)
        {
            g_MacroRunQueue.pop_front();
        }
        else
        {
            run.NextDispatchAt =
                now + static_cast<ULONGLONG>(
                    HotkeyPersist::SanitizeMacroDelayMs(
                        step.DelayMs));
        }

        g_LastCommandDispatch = now;
        g_LastScheduledCommandWasMacro = true;
        AtlasDiagnostics::WriteLine(
            "macro-dispatch macro=%s step=%zu command=%s",
            macroName.c_str(), stepNumber,
            step.Command.c_str());
        return true;
    };

    // Alternate when both queues are ready. A delayed macro no longer blocks
    // ordinary commands for up to 60 seconds, and a macro burst cannot starve
    // direct console or saved-bind commands.
    if (!g_CommandQueue.empty() &&
        (!macroDue || g_LastScheduledCommandWasMacro))
    {
        if (dispatchRegular())
            return;
        return;
    }

    if (macroDue)
    {
        if (dispatchMacro())
            return;
        return;
    }

    if (!g_CommandQueue.empty())
    {
        if (dispatchRegular())
            return;
        return;
    }

    // Help discovery is intentionally lowest priority and is requested only
    // once per world/controller session. It cannot repeatedly flood the
    // server merely because the console was reopened before detection latched.
    if (g_ConsoleUi.InitialHelpPending &&
        Client::IsConsoleCaptureReady() &&
        now >= g_ConsoleUi.NextInitialHelpAttemptAt &&
        HandOffGameThreadCommand("cheat", false))
    {
        g_ConsoleUi.InitialHelpAttempts++;
        g_ConsoleUi.NextInitialHelpAttemptAt =
            now + kInitialHelpRetryMs;
        g_LastCommandDispatch = now;
        g_LastScheduledCommandWasMacro = false;
        AtlasDiagnostics::WriteLine(
            "console-help-dispatch command=cheat");
    }
}

static void ClearQueuedCommands()
{
    if (!g_CommandQueue.empty())
        AtlasDiagnostics::WriteLine("command-clear inactive depth=%zu", g_CommandQueue.size());
    if (!g_MacroRunQueue.empty())
        AtlasDiagnostics::WriteLine("macro-clear inactive depth=%zu", g_MacroRunQueue.size());
    g_CommandQueue.clear();
    g_MacroRunQueue.clear();
    g_LastCommandDispatch = 0;
    g_LastScheduledCommandWasMacro = false;

    std::unique_lock<std::mutex> lock(
        g_GameThreadCommandMutex, std::try_to_lock);
    if (lock.owns_lock())
        g_GameThreadCommands.clear();
}

static void JoinSelectedHost()
{
    if (FGUI::HostType == 0)
    {
        Exec("open 127.0.0.1");
    }
    else
    {
        if (FGUI::RemoteIP[0] == '\0')
            return;

        std::string cmd = "open ";
        cmd += FGUI::RemoteIP;
        Exec(cmd.c_str());
    }
}

void GUI_Init()
{
    AtlasDiagnostics::BeginSession(FConfiguration::ConsoleVersion);
    FGUI::LoadHotkey();
    FGUI::LoadJoinHotkey();
    FGUI::LoadCommands();
    FGUI::LoadMacros();

    const int consoleMode = HotkeyPersist::LoadConsoleMode();
    FConfiguration::ConsoleMode.store(consoleMode, std::memory_order_release);

    const bool hadLegacyConsoleHotkey =
        HotkeyPersist::HasLegacyConsoleHotkey();
    const int legacyConsoleHotkey =
        hadLegacyConsoleHotkey
            ? HotkeyPersist::LoadLegacyConsoleHotkey()
            : 0;
    bool migratedSettings = hadLegacyConsoleHotkey;
    int menuHotkey = FGUI::HotkeyVK.load(std::memory_order_acquire);
    int joinHotkey = FGUI::JoinHotkeyVK.load(std::memory_order_acquire);

    if (IsUnrealConsoleHotkey(menuHotkey))
    {
        static const int menuFallbacks[] = { VK_F9, VK_F10, VK_F11, VK_F12 };
        for (int fallback : menuFallbacks)
        {
            if (fallback != joinHotkey && !IsUnrealConsoleHotkey(fallback))
            {
                menuHotkey = fallback;
                FGUI::HotkeyVK.store(menuHotkey, std::memory_order_release);
                migratedSettings = true;
                break;
            }
        }
    }

    // v1.1 temporarily moved Join from F5 to F6 to make room for a separate
    // console bind. The enhanced console now uses F8 and Unreal's native
    // ` / ~ key, so migrate that generated setting back to F5.
    if (legacyConsoleHotkey == VK_F5 && joinHotkey == VK_F6)
    {
        joinHotkey = VK_F5;
        FGUI::JoinHotkeyVK.store(joinHotkey, std::memory_order_release);
    }

    if (joinHotkey == menuHotkey || IsUnrealConsoleHotkey(joinHotkey))
    {
        static const int fallbacks[] = {
            VK_F5, VK_F6, VK_F7, VK_F10, VK_F11, VK_F12
        };
        for (int fallback : fallbacks)
        {
            if (fallback != menuHotkey &&
                !IsUnrealConsoleHotkey(fallback))
            {
                joinHotkey = fallback;
                FGUI::JoinHotkeyVK.store(joinHotkey, std::memory_order_release);
                migratedSettings = true;
                break;
            }
        }
    }

    size_t unboundActionCount = 0;
    auto conflictsWithOverlay = [menuHotkey, joinHotkey](int vk)
    {
        return vk > 0 &&
            (vk == menuHotkey || vk == joinHotkey ||
                IsUnrealConsoleHotkey(vk));
    };
    for (auto& command : FGUI::Commands)
    {
        if (conflictsWithOverlay(command.VK))
        {
            command.VK = 0;
            unboundActionCount++;
        }
    }
    for (auto& macro : FGUI::Macros)
    {
        if (conflictsWithOverlay(macro.VK))
        {
            macro.VK = 0;
            unboundActionCount++;
        }
    }
    migratedSettings = migratedSettings || unboundActionCount > 0;

    if (migratedSettings)
    {
        HotkeyPersist::SaveAll(menuHotkey, joinHotkey, consoleMode,
            FGUI::Commands, FGUI::Macros);
        RefreshExclusiveCommandHotkeys();
    }

    if (unboundActionCount > 0)
    {
        AtlasDiagnostics::WriteLine(
            "settings-migration unbound-console-conflicts=%zu",
            unboundActionCount);
    }
    PushStyle();
}

static bool IsBindableVK(int vk)
{
    switch (vk)
    {
    case VK_CANCEL:
    case VK_ESCAPE:
    case VK_SHIFT: case VK_CONTROL: case VK_MENU:
    case VK_LSHIFT: case VK_RSHIFT:
    case VK_LCONTROL: case VK_RCONTROL:
    case VK_LMENU: case VK_RMENU:
    case VK_LWIN: case VK_RWIN:
        return false;
    }
    return true;
}

using InputPressCounts = std::array<unsigned int, 0xFF>;
static std::array<std::atomic_uint, 0xFF> g_InputPressCounts{};

static void QueueKeyPress(int vk)
{
    if (vk <= 0 || vk > 0xFE)
        return;

    // A small cap prevents an unattended window from accumulating an
    // unbounded number of actions while still preserving rapid key presses.
    auto& count = g_InputPressCounts[static_cast<size_t>(vk)];
    unsigned int current = count.load(std::memory_order_relaxed);
    while (current < 8 &&
        !count.compare_exchange_weak(current, current + 1,
            std::memory_order_release, std::memory_order_relaxed))
    {
    }
}

static void ClearInputQueue()
{
    for (auto& count : g_InputPressCounts)
        count.store(0, std::memory_order_release);
}

static int InputMessageVK(UINT message, WPARAM wParam)
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

static bool IsExclusiveCommandHotkey(int vk)
{
    return vk > 0 && vk <= 0xFE &&
        g_ExclusiveCommandHotkeys[static_cast<size_t>(vk)].load(std::memory_order_acquire);
}

static bool IsAtlasConsoleSelected()
{
    return FConfiguration::ConsoleMode.load(std::memory_order_acquire) ==
        static_cast<int>(EConsoleMode::Atlas);
}

static bool IsAtlasOwnedHotkey(int vk)
{
    return vk > 0 && vk <= 0xFE &&
        (vk == FGUI::HotkeyVK || vk == FGUI::JoinHotkeyVK ||
            vk == VK_F8 ||
            (IsAtlasConsoleSelected() && vk == VK_OEM_3) ||
            IsExclusiveCommandHotkey(vk));
}

bool GUI_IsOwnedHotkey(int vk)
{
    return IsAtlasOwnedHotkey(vk);
}

bool GUI_ShouldConsumeInputMessage(UINT message, WPARAM wParam)
{
    return IsAtlasOwnedHotkey(InputMessageVK(message, wParam));
}

void GUI_QueueInputMessage(UINT message, WPARAM wParam, LPARAM lParam)
{
    int vk = InputMessageVK(message, wParam);

    switch (message)
    {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        // Ignore keyboard auto-repeat. Each physical down transition should
        // run a bind once.
        if ((static_cast<ULONG_PTR>(lParam) & (1ull << 30)) == 0)
            vk = static_cast<int>(wParam);
        else
            vk = 0;
        break;
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_XBUTTONDOWN:
        break;
    case WM_KILLFOCUS:
    case WM_CANCELMODE:
        ClearInputQueue();
        return;
    case WM_ACTIVATE:
        if (LOWORD(wParam) == WA_INACTIVE)
            ClearInputQueue();
        return;
    case WM_ACTIVATEAPP:
        if (!wParam)
            ClearInputQueue();
        return;
    default:
        return;
    }

    QueueKeyPress(vk);
}

static InputPressCounts TakeInputPresses()
{
    InputPressCounts presses{};
    for (size_t i = 0; i < presses.size(); i++)
        presses[i] = g_InputPressCounts[i].exchange(0, std::memory_order_acq_rel);
    return presses;
}

static unsigned int TakePressCount(InputPressCounts& presses, int vk)
{
    if (vk <= 0 || vk > 0xFE)
        return 0;

    unsigned int count = presses[static_cast<size_t>(vk)];
    presses[static_cast<size_t>(vk)] = 0;
    return count;
}

static void BeginBindCapture()
{
    // Discard the click/key that activated the bind button. A subsequent
    // window-message transition will be the key the user intended to bind.
    ClearInputQueue();
}

static void EndBindCapture()
{
    ClearInputQueue();
}

static void CancelBindCapture()
{
    FGUI::bRebinding = false;
    FGUI::bRebindingJoin = false;
    FGUI::RebindingCommandIndex = -2;
    FGUI::RebindingMacroIndex = -2;
    FGUI::PendingCommandVK = 0;
    EndBindCapture();
}

static void CloseMenuSurface()
{
    FGUI::bVisible.store(false, std::memory_order_release);
    g_MenuWindowFocused = false;
    CancelBindCapture();
    if (FGUI::bConsoleVisible.load(std::memory_order_acquire))
        g_ConsoleUi.FocusInput = true;
}

static void CloseConsoleSurface()
{
    FGUI::bConsoleVisible.store(false, std::memory_order_release);
    g_ConsoleUi.WindowFocused = false;
    g_ConsoleUi.HistoryPosition = -1;
    g_ConsoleUi.Draft.clear();
    g_ConsoleUi.CompletionMatches.clear();
    g_ConsoleUi.CompletionPosition = -1;
    if (FGUI::bVisible.load(std::memory_order_acquire))
        g_FocusMenuWindow = true;
}

static bool DismissConsoleHistory()
{
    if (g_ConsoleUi.HistoryPosition < 0)
        return false;

    g_ConsoleUi.HistoryPosition = -1;
    g_ConsoleUi.Draft.clear();
    g_ConsoleUi.FocusInput = true;
    return true;
}

static int PollBindKey(const InputPressCounts& presses)
{
    for (int vk = 0x01; vk <= 0xFE; vk++)
        if (IsBindableVK(vk) && presses[static_cast<size_t>(vk)] > 0)
            return vk;

    return 0;
}

static void SetBindError(const char* message)
{
    g_BindError = message ? message : "That key is already in use.";
    g_BindErrorUntil = GetTickCount64() + 3500;
    AtlasDiagnostics::WriteLine("bind-rejected reason=%s", g_BindError.c_str());
}

static bool IsReservedOverlayKey(int vk, int except)
{
    return (except != 0 && vk == FGUI::HotkeyVK) ||
        (except != 1 && vk == FGUI::JoinHotkeyVK) ||
        IsUnrealConsoleHotkey(vk);
}

static bool IsSavedActionHotkey(int vk)
{
    for (const auto& command : FGUI::Commands)
        if (command.VK == vk)
            return true;
    for (const auto& macro : FGUI::Macros)
        if (macro.VK == vk)
            return true;
    return false;
}

void GUI_HandleInput(bool windowActive)
{
    InputPressCounts presses = TakeInputPresses();
    if (!windowActive)
    {
        ClearQueuedCommands();
        return;
    }

    // Never execute a burst of accumulated ProcessEvent calls in one Present.
    // One command per 100 ms keeps rapid binds responsive without re-entering
    // cheat/inventory state changes before the previous command has settled.
    DispatchQueuedCommand();

    const bool wasOverlayVisible = GUI_IsOverlayVisible();

    if ((FGUI::bRebinding || FGUI::bRebindingJoin ||
        FGUI::RebindingCommandIndex != -2 || FGUI::RebindingMacroIndex != -2) &&
        TakePressCount(presses, VK_ESCAPE) > 0)
    {
        FGUI::bRebinding = false;
        FGUI::bRebindingJoin = false;
        FGUI::RebindingCommandIndex = -2;
        FGUI::RebindingMacroIndex = -2;
        EndBindCapture();
        return;
    }

    if (FGUI::bRebinding)
    {
        if (int vk = PollBindKey(presses))
        {
            if (IsReservedOverlayKey(vk, 0) || IsSavedActionHotkey(vk))
                SetBindError("That key is already assigned to another ATLAS action.");
            else
            {
                FGUI::HotkeyVK = vk;
                FGUI::SaveHotkey();
            }
            FGUI::bRebinding = false;
            FGUI::RebindingMacroIndex = -2;
            EndBindCapture();
        }
        return;
    }

    if (FGUI::bRebindingJoin)
    {
        if (int vk = PollBindKey(presses))
        {
            if (IsReservedOverlayKey(vk, 1) || IsSavedActionHotkey(vk))
                SetBindError("That key is already assigned to another ATLAS action.");
            else
            {
                FGUI::JoinHotkeyVK = vk;
                FGUI::SaveJoinHotkey();
            }
            FGUI::bRebindingJoin = false;
            FGUI::RebindingMacroIndex = -2;
            EndBindCapture();
        }
        return;
    }

    if (FGUI::RebindingCommandIndex != -2)
    {
        if (int vk = PollBindKey(presses))
        {
            if (IsReservedOverlayKey(vk, -1))
            {
                SetBindError("Menu, Join, and Unreal console keys are reserved.");
            }
            else if (FGUI::RebindingCommandIndex == -1)
            {
                FGUI::PendingCommandVK = vk;
            }
            else if (FGUI::RebindingCommandIndex >= 0 && FGUI::RebindingCommandIndex < (int)FGUI::Commands.size())
            {
                FGUI::Commands[FGUI::RebindingCommandIndex].VK = vk;
                FGUI::SaveCommands();
            }

            FGUI::RebindingCommandIndex = -2;
            EndBindCapture();
        }
        return;
    }

    if (FGUI::RebindingMacroIndex != -2)
    {
        if (int vk = PollBindKey(presses))
        {
            if (IsReservedOverlayKey(vk, -1))
            {
                SetBindError("Menu, Join, and Unreal console keys are reserved.");
            }
            else if (FGUI::RebindingMacroIndex >= 0 && FGUI::RebindingMacroIndex < (int)FGUI::Macros.size())
            {
                FGUI::Macros[FGUI::RebindingMacroIndex].VK = vk;
                FGUI::SaveMacros();
            }

            FGUI::RebindingMacroIndex = -2;
            EndBindCapture();
        }
        return;
    }

    const unsigned int menuHotkeyPresses = TakePressCount(presses, FGUI::HotkeyVK);
    if ((menuHotkeyPresses & 1u) != 0)
    {
        if (FGUI::bVisible.load(std::memory_order_acquire))
        {
            CloseMenuSurface();
        }
        else
        {
            FGUI::bVisible.store(true, std::memory_order_release);
            g_FocusMenuWindow = true;
        }
    }

    const unsigned int consoleHotkeyPresses =
        TakePressCount(presses, VK_OEM_3) +
        TakePressCount(presses, VK_F8);
    if (IsAtlasConsoleSelected())
    {
        const unsigned int advances = consoleHotkeyPresses % 3u;
        for (unsigned int advance = 0; advance < advances; advance++)
        {
            if (!FGUI::bConsoleVisible.load(std::memory_order_acquire))
            {
                g_ConsoleUi.Expanded = false;
                g_ConsoleUi.ResetExpansion = true;
                FGUI::bConsoleVisible.store(true, std::memory_order_release);
                RefreshInitialHelpSession(true);
            }
            else if (!g_ConsoleUi.Expanded)
            {
                g_ConsoleUi.Expanded = true;
            }
            else
            {
                CloseConsoleSurface();
            }

            g_ConsoleUi.FocusInput = true;
            g_ConsoleUi.ScrollToBottom = true;
        }
    }

    if (FGUI::bConsoleVisible.load(std::memory_order_acquire) &&
        (!FGUI::bVisible.load(std::memory_order_acquire) ||
            g_ConsoleUi.WindowFocused || !g_MenuWindowFocused))
    {
        unsigned int escapePresses =
            TakePressCount(presses, VK_ESCAPE);
        if (escapePresses > 0 &&
            DismissConsoleHistory())
        {
            escapePresses--;
        }

        if (escapePresses > 0)
            CloseConsoleSurface();
    }

    if (!wasOverlayVisible && !GUI_IsOverlayVisible() &&
        TakePressCount(presses, FGUI::JoinHotkeyVK) > 0)
    {
        JoinSelectedHost();
    }

    // Do not execute presses made while the overlay was visible, including
    // other keys received in the same batch as the close hotkey.
    if (!wasOverlayVisible && !GUI_IsOverlayVisible())
    {
        for (const auto& command : FGUI::Commands)
        {
            const unsigned int pressCount = TakePressCount(presses, command.VK);
            for (unsigned int i = 0; i < pressCount; i++)
            {
                if (Exec(command.Command.c_str()))
                    AppendConsoleCommandEcho(
                        command.Command);
            }
        }

        for (const auto& macro : FGUI::Macros)
        {
            const unsigned int pressCount = TakePressCount(presses, macro.VK);
            for (unsigned int i = 0; i < pressCount; i++)
                QueueMacro(macro);
        }
    }
}

static std::string TrimCommand(const std::string& value)
{
    size_t start = 0;
    while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start])))
        start++;

    size_t end = value.size();
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1])))
        end--;

    return value.substr(start, end - start);
}

static std::string NormalizeConsoleCommand(const std::string& entered)
{
    if (entered.empty() || entered[0] != '/')
        return TrimCommand(entered);

    std::string shorthand = TrimCommand(entered.substr(1));
    if (shorthand.empty())
        return std::string();

    if (StartsWithInsensitive(shorthand, "cheat") &&
        (shorthand.size() == 5 || std::isspace(static_cast<unsigned char>(shorthand[5]))))
        return shorthand;

    return "cheat " + shorthand;
}

static std::string LowercaseKey(const std::string& value)
{
    std::string lowered = value;
    for (char& ch : lowered)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return lowered;
}

static std::string ToConsoleCandidate(const std::string& command)
{
    const std::string trimmed = TrimCommand(command);
    if (!StartsWithInsensitive(trimmed, "cheat"))
        return trimmed;

    size_t position = 5;
    if (position < trimmed.size() && !std::isspace(static_cast<unsigned char>(trimmed[position])))
        return trimmed;
    while (position < trimmed.size() && std::isspace(static_cast<unsigned char>(trimmed[position])))
        position++;

    return "/" + trimmed.substr(position);
}

static void AddCompletionCandidate(std::vector<std::string>& candidates,
    std::unordered_set<std::string>& seen, const std::string& candidate, const std::string& prefix)
{
    size_t start = 0;
    while (start < candidate.size() &&
        std::isspace(static_cast<unsigned char>(candidate[start])))
    {
        start++;
    }

    const std::string completion = candidate.substr(start);
    if (TrimCommand(completion).empty() || !StartsWithInsensitive(completion, prefix))
        return;

    const std::string key = LowercaseKey(completion);
    if (seen.insert(key).second)
        candidates.push_back(completion);
}

static std::vector<std::string> BuildCompletionCandidates(const std::string& prefix)
{
    std::vector<std::string> candidates;
    std::unordered_set<std::string> seen;

    for (auto it = g_ConsoleUi.History.rbegin(); it != g_ConsoleUi.History.rend(); ++it)
        AddCompletionCandidate(candidates, seen, *it, prefix);

    for (const auto& command : FGUI::Commands)
        AddCompletionCandidate(candidates, seen, ToConsoleCandidate(command.Command), prefix);

    for (const auto& macro : FGUI::Macros)
        for (const auto& step : macro.Steps)
            AddCompletionCandidate(candidates, seen, ToConsoleCandidate(step.Command), prefix);

    static const char* kCommonCommands[] = {
        "/give ", "/suicide", "/startaircraft", "/outputge", "/applyge ",
        "/dumpge", "/god", "/fly", "fov ", "open "
    };
    for (const char* command : kCommonCommands)
        AddCompletionCandidate(candidates, seen, command, prefix);

    return candidates;
}

static void ReplaceConsoleInput(ImGuiInputTextCallbackData* data, const std::string& value)
{
    data->DeleteChars(0, data->BufTextLen);
    data->InsertChars(0, value.c_str());
    data->CursorPos = data->SelectionStart = data->SelectionEnd = data->BufTextLen;
}

static int ConsoleInputCallback(ImGuiInputTextCallbackData* data)
{
    auto& state = *static_cast<FConsoleUiState*>(data->UserData);

    if (data->EventFlag == ImGuiInputTextFlags_CallbackHistory)
    {
        state.CompletionMatches.clear();
        state.CompletionPosition = -1;

        if (data->EventKey == ImGuiKey_UpArrow)
        {
            if (state.History.empty())
                return 0;

            if (state.HistoryPosition < 0)
            {
                state.Draft.assign(data->Buf, static_cast<size_t>(data->BufTextLen));
                state.HistoryPosition = static_cast<int>(state.History.size()) - 1;
            }
            else
            {
                const int historyCount =
                    static_cast<int>(state.History.size());
                state.HistoryPosition =
                    (state.HistoryPosition - 1 + historyCount) %
                    historyCount;
            }

            ReplaceConsoleInput(data, state.History[static_cast<size_t>(state.HistoryPosition)]);
        }
        else if (data->EventKey == ImGuiKey_DownArrow &&
            state.HistoryPosition >= 0 &&
            !state.History.empty())
        {
            state.HistoryPosition =
                (state.HistoryPosition + 1) %
                static_cast<int>(state.History.size());
            ReplaceConsoleInput(
                data,
                state.History[
                    static_cast<size_t>(
                        state.HistoryPosition)]);
        }
    }
    else if (data->EventFlag == ImGuiInputTextFlags_CallbackCompletion)
    {
        state.HistoryPosition = -1;
        state.Draft.clear();
        const std::string current(data->Buf, static_cast<size_t>(data->BufTextLen));
        if (!state.CompletionMatches.empty() && state.CompletionPosition >= 0 &&
            current == state.CompletionMatches[static_cast<size_t>(state.CompletionPosition)])
        {
            state.CompletionPosition =
                (state.CompletionPosition + 1) % static_cast<int>(state.CompletionMatches.size());
        }
        else
        {
            state.CompletionSeed = current;
            state.CompletionMatches = BuildCompletionCandidates(current);
            state.CompletionPosition = state.CompletionMatches.empty() ? -1 : 0;
        }

        if (state.CompletionPosition >= 0)
            ReplaceConsoleInput(data, state.CompletionMatches[static_cast<size_t>(state.CompletionPosition)]);
    }
    else if (data->EventFlag == ImGuiInputTextFlags_CallbackEdit)
    {
        if (data->BufTextLen > static_cast<int>(kMaximumConsoleCommandLength))
        {
            int validLength = static_cast<int>(kMaximumConsoleCommandLength);
            while (validLength > 0 &&
                (static_cast<unsigned char>(data->Buf[validLength]) & 0xC0) == 0x80)
            {
                validLength--;
            }
            data->DeleteChars(validLength, data->BufTextLen - validLength);
        }

        state.HistoryPosition = -1;
        state.Draft.clear();
        state.CompletionMatches.clear();
        state.CompletionPosition = -1;
    }

    return 0;
}

static void RememberConsoleCommand(const std::string& command)
{
    const std::string key = LowercaseKey(command);
    for (auto it = g_ConsoleUi.History.begin(); it != g_ConsoleUi.History.end();)
    {
        if (LowercaseKey(*it) == key)
            it = g_ConsoleUi.History.erase(it);
        else
            ++it;
    }

    g_ConsoleUi.History.push_back(command);
    if (g_ConsoleUi.History.size() > kMaximumConsoleHistory)
        g_ConsoleUi.History.erase(g_ConsoleUi.History.begin());
}

static bool SubmitConsoleCommand()
{
    const std::string raw = g_ConsoleUi.Input;
    const std::string entered = TrimCommand(raw);
    if (entered.empty())
        return false;

    if (raw.size() > kMaximumConsoleCommandLength)
    {
        AppendConsoleLine(EConsoleLineKind::Error, "[ATLAS] Command is too long (maximum 2048 bytes).");
        g_ConsoleUi.FocusInput = true;
        return false;
    }

    const std::string command = NormalizeConsoleCommand(raw);
    if (command.empty())
    {
        AppendConsoleLine(EConsoleLineKind::Error, "[ATLAS] Enter a command after '/'.");
        g_ConsoleUi.FocusInput = true;
        return false;
    }

    if (!Exec(command.c_str()))
    {
        AppendConsoleLine(
            EConsoleLineKind::Error,
            g_GameThreadDispatcherReady.load(
                std::memory_order_acquire)
                ? "[ATLAS] Command queue is full; command was not sent."
                : "[ATLAS] Unreal game-thread command dispatcher is unavailable.");
        g_ConsoleUi.FocusInput = true;
        return false;
    }

    RememberConsoleCommand(raw);
    AppendConsoleCommandEcho(raw);

    g_ConsoleUi.Input.clear();
    g_ConsoleUi.Draft.clear();
    g_ConsoleUi.HistoryPosition = -1;
    g_ConsoleUi.CompletionMatches.clear();
    g_ConsoleUi.CompletionPosition = -1;
    g_ConsoleUi.FocusInput = true;
    g_ConsoleUi.ScrollToBottom = true;
    return true;
}

static ImVec4 ConsoleLineColor(EConsoleLineKind)
{
    return ImVec4(1.f, 1.f, 1.f, 1.f);
}

static void DrawConsoleHistoryPopup(
    const ImVec2& inputMin, const ImVec2& inputMax)
{
    const int selected = g_ConsoleUi.HistoryPosition;
    const int historyCount =
        static_cast<int>(g_ConsoleUi.History.size());
    if (selected < 0 || selected >= historyCount)
        return;

    static constexpr int kMaximumVisibleHistory = 5;
    const int visibleCount =
        (std::min)(kMaximumVisibleHistory, historyCount);
    const int maximumFirst =
        historyCount - visibleCount;
    const int first =
        (std::max)(
            0,
            (std::min)(
                maximumFirst,
                selected - visibleCount / 2));
    const int last = first + visibleCount - 1;
    const float rowHeight =
        std::ceil(ImGui::GetTextLineHeight() + 5.f);
    const float popupPadding = 3.f;
    float popupWidth = 96.f;
    for (int index = first; index <= last; index++)
    {
        const std::string label =
            "> " + g_ConsoleUi.History[static_cast<size_t>(index)];
        popupWidth =
            (std::max)(
                popupWidth,
                ImGui::CalcTextSize(label.c_str()).x + 14.f);
    }
    popupWidth =
        std::ceil(
            (std::min)(
                popupWidth,
                inputMax.x - inputMin.x));

    const float popupHeight =
        rowHeight * static_cast<float>(visibleCount) +
        popupPadding * 2.f;
    const ImVec2 popupMax(
        std::round(inputMin.x) + popupWidth,
        std::round(inputMin.y - 2.f));
    const ImVec2 popupMin(
        std::round(inputMin.x),
        popupMax.y - popupHeight);

    ImDrawList* draw = ImGui::GetForegroundDrawList();
    draw->AddRectFilled(
        popupMin,
        popupMax,
        ImGui::GetColorU32(
            ImVec4(0.025f, 0.025f, 0.025f, 0.94f)));
    draw->PushClipRect(popupMin, popupMax, true);
    for (int index = first; index <= last; index++)
    {
        const float rowTop =
            popupMin.y + popupPadding +
            rowHeight * static_cast<float>(index - first);
        if (index == selected)
        {
            draw->AddRectFilled(
                ImVec2(popupMin.x + 2.f, rowTop),
                ImVec2(popupMax.x - 2.f, rowTop + rowHeight),
                ImGui::GetColorU32(
                    ImVec4(0.24f, 0.24f, 0.24f, 0.72f)));
        }

        const std::string label =
            "> " + g_ConsoleUi.History[static_cast<size_t>(index)];
        draw->AddText(
            ImVec2(popupMin.x + 7.f, rowTop + 2.f),
            ImGui::GetColorU32(
                ImVec4(1.f, 1.f, 1.f, 1.f)),
            label.c_str());
    }
    draw->PopClipRect();
}

static bool RenderConsoleCommandInput(
    float rightPadding, float frameOpacity)
{
    const float clearButtonWidth = 62.f;
    const float controlSpacing = 6.f;
    ImGui::SetNextItemWidth((std::max)(
        120.f,
        ImGui::GetContentRegionAvail().x -
            rightPadding - clearButtonWidth - controlSpacing));
    if (g_ConsoleUi.FocusInput &&
        FGUI::bConsoleVisible.load(std::memory_order_acquire))
    {
        ImGui::SetKeyboardFocusHere();
        g_ConsoleUi.FocusInput = false;
    }

    const ImGuiInputTextFlags inputFlags =
        ImGuiInputTextFlags_EnterReturnsTrue |
        ImGuiInputTextFlags_CallbackHistory |
        ImGuiInputTextFlags_CallbackCompletion |
        ImGuiInputTextFlags_CallbackEdit;

    const ImVec2 promptSize = ImGui::CalcTextSize(">");
    const float promptLeftPadding = 8.f;
    const float promptGap = 7.f;
    const float frameAlpha =
        (std::max)(0.f, (std::min)(1.f, frameOpacity));
    const ImVec2 framePadding = ImGui::GetStyle().FramePadding;
    ImGui::PushStyleVar(
        ImGuiStyleVar_FramePadding,
        ImVec2(
            promptLeftPadding + promptSize.x + promptGap,
            framePadding.y));
    ImGui::PushStyleVar(
        ImGuiStyleVar_FrameBorderSize, 0.f);
    ImGui::PushStyleColor(
        ImGuiCol_Text, ImVec4(1.f, 1.f, 1.f, 1.f));
    ImGui::PushStyleColor(
        ImGuiCol_TextSelectedBg,
        ImVec4(0.42f, 0.42f, 0.42f, 0.55f));
    ImGui::PushStyleColor(
        ImGuiCol_NavHighlight,
        ImVec4(0.58f, 0.58f, 0.60f, 0.72f));
    ImGui::PushStyleColor(
        ImGuiCol_FrameBg, ImVec4(0.f, 0.f, 0.f, frameAlpha));
    ImGui::PushStyleColor(
        ImGuiCol_FrameBgHovered, ImVec4(0.f, 0.f, 0.f, frameAlpha));
    ImGui::PushStyleColor(
        ImGuiCol_FrameBgActive, ImVec4(0.f, 0.f, 0.f, frameAlpha));
    const bool submitted = ImGui::InputText(
        "##console_input", &g_ConsoleUi.Input,
        inputFlags, ConsoleInputCallback, &g_ConsoleUi);
    const ImVec2 inputMin = ImGui::GetItemRectMin();
    const ImVec2 inputMax = ImGui::GetItemRectMax();
    ImGui::PopStyleColor(6);
    ImGui::PopStyleVar(2);

    const float promptY =
        std::round(
            inputMin.y +
            (std::max)(
                0.f,
                (inputMax.y - inputMin.y - promptSize.y) * 0.5f));
    ImGui::GetWindowDrawList()->AddText(
        ImVec2(
            std::round(inputMin.x + promptLeftPadding),
            promptY),
        ImGui::GetColorU32(
            ImVec4(1.f, 1.f, 1.f, 1.f)),
        ">");
    DrawConsoleHistoryPopup(inputMin, inputMax);

    ImGui::SameLine(0.f, controlSpacing);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.f);
    ImGui::PushStyleColor(
        ImGuiCol_Text, ImVec4(1.f, 1.f, 1.f, 1.f));
    ImGui::PushStyleColor(
        ImGuiCol_Button, ImVec4(0.f, 0.f, 0.f, frameAlpha));
    ImGui::PushStyleColor(
        ImGuiCol_ButtonHovered,
        ImVec4(0.18f, 0.18f, 0.18f, frameAlpha));
    ImGui::PushStyleColor(
        ImGuiCol_ButtonActive,
        ImVec4(0.27f, 0.27f, 0.27f, frameAlpha));
    const bool clearLogs = ImGui::Button(
        "Clear##console_logs",
        ImVec2(clearButtonWidth, inputMax.y - inputMin.y));
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar();
    if (clearLogs)
    {
        ClearConsoleLogs();
        g_ConsoleUi.FocusInput = true;
    }

    return submitted && SubmitConsoleCommand();
}

static void RenderConsoleWindow(float fade)
{
    if (fade <= 0.f)
        return;

    ImGuiIO& io = ImGui::GetIO();
    const bool expanded = g_ConsoleUi.Expanded;
    const float consoleWidth = (std::max)(1.f, io.DisplaySize.x);
    const float screenBottom =
        std::round((std::max)(1.f, io.DisplaySize.y));
    const float compactHeight =
        (std::min)(screenBottom, 40.f);
    const float expansion =
        g_ConsoleUi.Expansion * g_ConsoleUi.Expansion *
        (3.f - 2.f * g_ConsoleUi.Expansion);
    const float desiredConsoleHeight =
        compactHeight +
        (screenBottom - compactHeight) *
            expansion;
    const float consoleY =
        std::round(
            (std::max)(
                0.f,
                screenBottom - desiredConsoleHeight));
    const float consoleHeight =
        (std::max)(1.f, screenBottom - consoleY);
    const bool showExpandedContent =
        expanded && consoleHeight >= 220.f;

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoResize;
    if (!FGUI::bConsoleVisible)
        flags |= ImGuiWindowFlags_NoInputs;

    ImGui::SetNextWindowPos(ImVec2(0.f, consoleY), ImGuiCond_Always);
    ImGui::SetNextWindowSize(
        ImVec2(consoleWidth, consoleHeight), ImGuiCond_Always);
    if (g_ConsoleUi.FocusInput && FGUI::bConsoleVisible)
        ImGui::SetNextWindowFocus();

    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, fade);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.f, 0.f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
    // The expanded console is a dimmed game view with output drawn directly
    // over it. Keep this interaction window invisible; only the command input
    // retains its own black frame.
    ImGui::PushStyleColor(
        ImGuiCol_WindowBg,
        ImVec4(0.f, 0.f, 0.f, 0.f));
    ImGui::Begin("##atlas_unreal_console", nullptr, flags);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);
    g_ConsoleUi.WindowFocused =
        FGUI::bConsoleVisible.load(std::memory_order_acquire) &&
        ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);

    const float width = ImGui::GetWindowWidth();
    const ImVec2 windowPos = ImGui::GetWindowPos();
    const float horizontalPadding = 8.f;
    const float inputScreenY =
        screenBottom -
        std::round(ImGui::GetFrameHeight()) - 7.f;
    const float inputY =
        (std::max)(
            2.f,
            inputScreenY - windowPos.y);
    const float inputFrameOpacity =
        0.90f + 0.10f * g_ConsoleUi.Expansion;

    if (!showExpandedContent)
    {
        ImGui::SetCursorPos(ImVec2(horizontalPadding, inputY));
        if (RenderConsoleCommandInput(
                horizontalPadding, inputFrameOpacity) &&
            !expanded)
            CloseConsoleSurface();
    }
    else
    {
        ImGui::SetCursorPos(ImVec2(horizontalPadding, 7.f));
        const float logHeight = (std::max)(
            80.f,
            inputY - ImGui::GetCursorPosY() - 4.f);
        ImGui::PushStyleColor(
            ImGuiCol_ChildBg,
            ImVec4(0.f, 0.f, 0.f, 0.f));
        ImGui::PushStyleColor(
            ImGuiCol_ScrollbarBg,
            ImVec4(0.035f, 0.035f, 0.035f, 0.58f));
        ImGui::PushStyleColor(
            ImGuiCol_ScrollbarGrab,
            ImVec4(0.38f, 0.38f, 0.40f, 0.88f));
        ImGui::PushStyleColor(
            ImGuiCol_ScrollbarGrabHovered,
            ImVec4(0.52f, 0.52f, 0.54f, 0.96f));
        ImGui::PushStyleColor(
            ImGuiCol_ScrollbarGrabActive,
            ImVec4(0.66f, 0.66f, 0.68f, 1.f));
        ImGui::PushStyleVar(
            ImGuiStyleVar_ChildBorderSize, 0.f);
        ImGui::PushStyleVar(
            ImGuiStyleVar_ChildRounding, 0.f);
        ImGui::PushStyleVar(
            ImGuiStyleVar_ScrollbarSize, 11.f);
        ImGui::PushStyleVar(
            ImGuiStyleVar_ScrollbarRounding, 1.f);
        ImGui::BeginChild(
            "##console_log",
            ImVec2(width - horizontalPadding * 2.f, logHeight),
            false,
            ImGuiWindowFlags_HorizontalScrollbar);

        static uint64_t lastRenderedRevision = 0;
        const bool hasNewLines =
            lastRenderedRevision != g_ConsoleLineRevision;
        const bool shouldScroll =
            g_ConsoleUi.ScrollToBottom ||
            (hasNewLines && g_ConsoleUi.WasAtBottom);
        const bool wasAtBottom =
            ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 2.f;

        if (!g_ConsoleLines.empty())
        {
            const float lineHeight =
                ImGui::GetTextLineHeightWithSpacing();
            const float lineContentHeight =
                lineHeight *
                static_cast<float>(g_ConsoleLines.size());
            const float bottomAlignment =
                (std::max)(
                    0.f,
                    ImGui::GetContentRegionAvail().y -
                        lineContentHeight);
            if (bottomAlignment > 0.f)
            {
                ImGui::SetCursorPosY(
                    ImGui::GetCursorPosY() +
                    bottomAlignment);
            }

            ImGuiListClipper clipper;
            clipper.Begin(
                static_cast<int>(g_ConsoleLines.size()),
                lineHeight);
            if (shouldScroll)
            {
                clipper.IncludeItemByIndex(
                    static_cast<int>(g_ConsoleLines.size()) - 1);
            }

            while (clipper.Step())
            {
                for (int row = clipper.DisplayStart;
                    row < clipper.DisplayEnd; row++)
                {
                    const FConsoleLine& line =
                        g_ConsoleLines[static_cast<size_t>(row)];
                    ImGui::PushID(static_cast<int>(line.Id));
                    if (line.Kind == EConsoleLineKind::Spacer)
                    {
                        ImGui::Dummy(ImVec2(
                            0.f, ImGui::GetTextLineHeight()));
                    }
                    else
                    {
                        ImGui::PushStyleColor(
                            ImGuiCol_Text,
                            ImVec4(0.46f, 0.46f, 0.48f, 1.f));
                        ImGui::TextUnformatted(line.Time.c_str());
                        ImGui::PopStyleColor();
                        ImGui::SameLine();
                        ImGui::PushStyleColor(
                            ImGuiCol_Text,
                            ConsoleLineColor(line.Kind));
                        ImGui::TextUnformatted(line.Text.c_str());
                        ImGui::PopStyleColor();
                    }
                    ImGui::PopID();

                    if (shouldScroll &&
                        row == static_cast<int>(
                            g_ConsoleLines.size()) - 1)
                    {
                        ImGui::SetScrollHereY(1.f);
                    }
                }
            }
        }

        g_ConsoleUi.WasAtBottom = shouldScroll || wasAtBottom;
        g_ConsoleUi.ScrollToBottom = false;
        lastRenderedRevision = g_ConsoleLineRevision;
        ImGui::EndChild();
        ImGui::PopStyleVar(4);
        ImGui::PopStyleColor(5);

        ImGui::SetCursorPos(ImVec2(horizontalPadding, inputY));
        (void)RenderConsoleCommandInput(
            horizontalPadding, inputFrameOpacity);
    }

    ImGui::End();
    ImGui::PopStyleVar();
}

void GUI_Render()
{
    ImGuiIO& io = ImGui::GetIO();
    // Keep background ClientMessage capture bounded, but do not take the
    // pending-output mutex on every Present while the console is hidden.
    // Opening either console size drains everything collected since it closed.
    if (FGUI::bConsoleVisible.load(std::memory_order_acquire))
        DrainConsoleOutput();

    static float s_MenuFade = 0.f;
    static float s_ConsoleFade = 0.f;
    const float deltaTime = io.DeltaTime > 0.f ? io.DeltaTime : 1.f / 60.f;

    if (g_ConsoleUi.ResetExpansion)
    {
        g_ConsoleUi.Expansion = 0.f;
        g_ConsoleUi.ResetExpansion = false;
    }
    const float expansionTarget =
        g_ConsoleUi.Expanded ? 1.f : 0.f;
    const float expansionStep = deltaTime / 0.16f;
    if (g_ConsoleUi.Expansion < expansionTarget)
    {
        g_ConsoleUi.Expansion =
            (std::min)(expansionTarget,
                g_ConsoleUi.Expansion + expansionStep);
    }
    else if (g_ConsoleUi.Expansion > expansionTarget)
    {
        g_ConsoleUi.Expansion =
            (std::max)(expansionTarget,
                g_ConsoleUi.Expansion - expansionStep);
    }

    auto updateFade = [deltaTime](float& value, bool visible, float duration)
    {
        const float target = visible ? 1.f : 0.f;
        const float step = deltaTime / duration;
        if (value < target) { value += step; if (value > target) value = target; }
        else if (value > target) { value -= step; if (value < target) value = target; }
    };

    updateFade(s_MenuFade, FGUI::bVisible, 0.15f);
    updateFade(s_ConsoleFade, FGUI::bConsoleVisible, 0.18f);

    const float menuFade = s_MenuFade * s_MenuFade * (3.f - 2.f * s_MenuFade);
    const float consoleFade = s_ConsoleFade * s_ConsoleFade * (3.f - 2.f * s_ConsoleFade);
    const float surfaceFade = (std::max)(menuFade, consoleFade);

    if (surfaceFade <= 0.f)
        return;

    const float backdropOpacity = (std::max)(
        menuFade * 0.50f,
        consoleFade *
            (0.50f * g_ConsoleUi.Expansion));
    if (backdropOpacity > 0.f)
    {
        ImGui::GetBackgroundDrawList()->AddRectFilled(
            ImVec2(0.f, 0.f),
            io.DisplaySize,
            IM_COL32(
                0, 0, 0,
                static_cast<int>(backdropOpacity * 255.f)));
    }

    RenderConsoleWindow(consoleFade);
    if (menuFade <= 0.f)
        return;

    const float fade = menuFade;
    ImGuiWindowFlags wflags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoTitleBar;
    if (!FGUI::bVisible)
        wflags |= ImGuiWindowFlags_NoInputs;

    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Once, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(580.f, 410.f), ImGuiCond_Once);
    ImGui::SetNextWindowSizeConstraints(ImVec2(500.f, 320.f), ImVec2(10000.f, 10000.f));
    if (g_FocusMenuWindow && FGUI::bVisible)
    {
        ImGui::SetNextWindowFocus();
        g_FocusMenuWindow = false;
    }

    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, fade);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.f, 0.f));
    bool open = true;
    ImGui::Begin("##atlas_main", &open, wflags);
    ImGui::PopStyleVar();
    g_MenuWindowFocused =
        FGUI::bVisible.load(std::memory_order_acquire) &&
        ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);

    const float W = ImGui::GetWindowWidth();
    const float H = ImGui::GetWindowHeight();
    const ImVec2 wp = ImGui::GetWindowPos();

    const float TopBarH = 46.f;
    const float SidebarW = 118.f;

    // top bar
    {
        ImGui::GetWindowDrawList()->AddRectFilled(
            wp, ImVec2(wp.x + W, wp.y + TopBarH),
            ImGui::GetColorU32(ImVec4(0.0549f, 0.0667f, 0.0941f, 1.f)),
            ImGui::GetStyle().WindowRounding, ImDrawFlags_RoundCornersTop);

        const float LogoSize = 30.f;
        const float PadL = 14.f;

        ImGui::SetCursorPos(ImVec2(PadL, (TopBarH - LogoSize) * 0.5f));
        if (FGUI::LogoTexture != ImTextureID_Invalid)
            ImGui::Image(FGUI::LogoTexture, ImVec2(LogoSize, LogoSize));
        else
            ImGui::Dummy(ImVec2(LogoSize, LogoSize));

        ImGui::SameLine(PadL + LogoSize + 8.f);
        const float TitleY = (TopBarH - ImGui::GetTextLineHeight()) * 0.5f;
        ImGui::SetCursorPosY(TitleY);
        ImGui::PushStyleColor(ImGuiCol_Text, Accent());
        ImGui::Text("ATLAS");
        ImGui::PopStyleColor();
        ImGui::SameLine(0.f, 6.f);
        ImGui::SetCursorPosY(TitleY);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.353f, 0.388f, 0.478f, 1.f));
        ImGui::Text("Console");
        ImGui::PopStyleColor();

        ImGui::SameLine(0.f, 8.f);
        ImGui::SetCursorPosY(TitleY);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.353f, 0.388f, 0.478f, 0.6f));
        ImGui::TextUnformatted(FConfiguration::ConsoleVersion);
        ImGui::PopStyleColor();

        char Badge[32];
        snprintf(Badge, sizeof(Badge), "[%s]", VKName(FGUI::HotkeyVK));
        const float BadgeW = ImGui::CalcTextSize(Badge).x;
        const float BadgeH = ImGui::GetTextLineHeight();

        // Close button pinned to the top-right; the hotkey badge sits to its left.
        const float CloseSize = 22.f;
        const float CloseX = W - CloseSize - 12.f;

        ImGui::SetCursorPos(ImVec2(CloseX - 12.f - BadgeW, (TopBarH - BadgeH) * 0.5f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.353f, 0.388f, 0.478f, 0.85f));
        ImGui::TextUnformatted(Badge);
        ImGui::PopStyleColor();

        ImGui::SetCursorPos(ImVec2(CloseX, (TopBarH - CloseSize) * 0.5f));
        if (ImGui::InvisibleButton("##closebtn", ImVec2(CloseSize, CloseSize)))
            CloseMenuSurface();
        {
            const bool hovered = ImGui::IsItemHovered();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImVec2 rmin = ImGui::GetItemRectMin();
            const ImVec2 rmax = ImGui::GetItemRectMax();
            if (hovered)
                dl->AddRectFilled(rmin, rmax, ImGui::GetColorU32(Accent(0.20f)), 4.f);
            const float pad = 6.f;
            const ImU32 xcol = ImGui::GetColorU32(hovered ? Accent() : ImVec4(0.55f, 0.60f, 0.70f, 1.f));
            dl->AddLine(ImVec2(rmin.x + pad, rmin.y + pad), ImVec2(rmax.x - pad, rmax.y - pad), xcol, 1.6f);
            dl->AddLine(ImVec2(rmax.x - pad, rmin.y + pad), ImVec2(rmin.x + pad, rmax.y - pad), xcol, 1.6f);
        }
    }

	// divider lines
    {
        ImDrawList* fdl = ImGui::GetWindowDrawList();
        const ImU32 line = IM_COL32(35, 42, 62, (int)(fade * 255.f));
        fdl->AddLine(ImVec2(wp.x, wp.y + TopBarH), ImVec2(wp.x + W, wp.y + TopBarH), line, 1.f);
        fdl->AddLine(ImVec2(wp.x + SidebarW, wp.y + TopBarH), ImVec2(wp.x + SidebarW, wp.y + H), line, 1.f);
    }

    static const char* kTabs[] = { "Main", "Commands", "Config" };
    const int kTabCount = (int)(sizeof(kTabs) / sizeof(kTabs[0]));
    const float TabH = 40.f;
    const float TabsTop = 12.f;

    ImGui::SetCursorPos(ImVec2(0.f, TopBarH));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0549f, 0.0667f, 0.0941f, 1.f)); // #0e1118
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.f, 0.f));
    ImGui::BeginChild("##sidebar", ImVec2(SidebarW, H - TopBarH), false, ImGuiWindowFlags_NoScrollbar);
    {
        const ImVec2 sbPos = ImGui::GetWindowPos();

        for (int i = 0; i < kTabCount; i++)
        {
            ImGui::SetCursorPos(ImVec2(0.f, TabsTop + i * TabH));
            SidebarTab(kTabs[i], i, TabH);
        }

        static float s_IndicatorY = -1.f;
        const float localTarget = TabsTop + FGUI::ActiveTab * TabH + TabH * 0.5f;
        if (s_IndicatorY < 0.f) s_IndicatorY = localTarget;
        float lerp = io.DeltaTime * 16.f;
        if (lerp > 1.f) lerp = 1.f;
        s_IndicatorY += (localTarget - s_IndicatorY) * lerp;
        const float indY = sbPos.y + s_IndicatorY;
        ImGui::GetWindowDrawList()->AddRectFilled(
            ImVec2(sbPos.x, indY - 9.f), ImVec2(sbPos.x + 3.f, indY + 9.f),
            ImGui::GetColorU32(Accent()), 2.f);
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();

    const float ContentPadX = 28.f;
    const float ContentPadTop = 12.f;
    const bool bCommandsTab = FGUI::ActiveTab == 1;
    const float ContentChildW = (W - SidebarW) - ContentPadX;
    ImGui::SetCursorPos(ImVec2(SidebarW + ContentPadX, TopBarH + ContentPadTop));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.f, 0.f, 0.f, 0.f));
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, fade);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.f, 0.f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.f, 10.f));
    const ImGuiWindowFlags ContentFlags = bCommandsTab ? ImGuiWindowFlags_AlwaysVerticalScrollbar : ImGuiWindowFlags_None;
    ImGui::BeginChild("##content",
        ImVec2(ContentChildW, (H - TopBarH) - ContentPadTop - 12.f), false,
        ContentFlags);

    float CW = ImGui::GetContentRegionAvail().x;
    CW -= ContentPadX;
    if (CW < 1.f)
        CW = ImGui::GetContentRegionAvail().x;

    switch (FGUI::ActiveTab)
    {
    case 0: // main tab
    {
        SectionLabel("Host");

    const char* hostTypeItems[] = { "Local Host", "Remote Host" };
    ImGui::PushItemWidth(CW);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.f, 8.f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.f, 7.f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.f, 6.f));
    ImGui::Combo("##hosttype", &FGUI::HostType, hostTypeItems, 2);
    ImGui::PopStyleVar(3);
    ImGui::PopItemWidth();

    ImGui::Spacing();

    if (FGUI::HostType == 0)
    {
        if (ImGui::Button("Join Local Host", ImVec2(CW, 0.f)))
            JoinSelectedHost();
    }
    else
    {
        ImGui::PushItemWidth(CW);
        ImGui::InputTextWithHint("##remoteip", "Enter IP to join", FGUI::RemoteIP, sizeof(FGUI::RemoteIP));
        ImGui::PopItemWidth();

        ImGui::Spacing();

        if (ImGui::Button("Join Remote Host", ImVec2(CW, 0.f)))
            JoinSelectedHost();
    }

    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.353f, 0.388f, 0.478f, 1.f));
    ImGui::TextWrapped("Pressing this key joins the currently selected host type.");
    ImGui::PopStyleColor();

    if (VersionInfo.EngineVersion < 4.24 || VersionInfo.FortniteVersion < 24.30)
    {
        SectionLabel("Editing");

        if (VersionInfo.EngineVersion < 4.24)
        {
            ImGui::Checkbox("Edit On Release", &FConfiguration::bEOREnabled);
            ImGui::SameLine(0.f, 10.f);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.353f, 0.388f, 0.478f, 1.f));
            ImGui::TextUnformatted("EOR");
            ImGui::PopStyleColor();
        }

        if (VersionInfo.FortniteVersion < 24.30)
        {
            ImGui::Checkbox("Reset On Release", &FConfiguration::bROREnabled);
            ImGui::SameLine(0.f, 10.f);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.353f, 0.388f, 0.478f, 1.f));
            ImGui::TextUnformatted("ROR");
            ImGui::PopStyleColor();
        }

        if (VersionInfo.FortniteVersion < 15.20)
            ImGui::Checkbox("Disable Pre-Edits", &FConfiguration::bDisablePreEdits);
    }

    SectionLabel("FOV");
    {
        const float ResetW = 110.f;
        const float SliderW = CW - ResetW - ImGui::GetStyle().ItemSpacing.x;
        ImGui::PushItemWidth(SliderW > 80.f ? SliderW : 80.f);

        if (ImGui::SliderInt("##fov", &FConfiguration::FOV, 1, 175))
        {
            char cmd[32];
            snprintf(cmd, sizeof(cmd), "fov %d", FConfiguration::FOV);
            Exec(cmd);
        }

        ImGui::PopItemWidth();
        ImGui::SameLine();
        if (ImGui::Button("Reset FOV", ImVec2(ResetW, 0.f)))
        {
            FConfiguration::FOV = 80;
            Exec("fov");
        }
    }

    SectionLabel("Respawn");
    ImGui::Checkbox("Respawns Enabled", &FConfiguration::bForceRespawns);

        break;
    }
    case 1: // Commands
    {
        SectionLabel("Commands");

        ImGui::PushItemWidth(CW);
        ImGui::InputTextWithHint("##commandinput", "UE Console Command", FGUI::CommandInput, sizeof(FGUI::CommandInput));
        ImGui::PopItemWidth();

        ImGui::Spacing();

        const float AddButtonW = 110.f;
        const float DelayInputW = CW - AddButtonW - ImGui::GetStyle().ItemSpacing.x;
        ImGui::PushItemWidth(DelayInputW > 140.f ? DelayInputW : 140.f);
        ImGui::InputTextWithHint("##stepdelay", "Wait before next step (ms)", FGUI::MacroDelayInput, sizeof(FGUI::MacroDelayInput), ImGuiInputTextFlags_CharsDecimal);
        ImGui::PopItemWidth();
        ImGui::SameLine();

        if (ImGui::Button("Add Step", ImVec2(AddButtonW, 0.f)))
        {
            const char* start = FGUI::CommandInput;
            while (*start == ' ' || *start == '\t')
                start++;

            if (*start)
            {
                HotkeyPersist::MacroStep step{};
                step.Command = start;
                step.DelayMs = HotkeyPersist::SanitizeMacroDelayMs(atoi(FGUI::MacroDelayInput));
                FGUI::MacroDraftSteps.push_back(std::move(step));
                FGUI::CommandInput[0] = '\0';
            }
        }

        if (!FGUI::MacroDraftSteps.empty())
        {
            ImGui::Spacing();
            ImGui::PushID("draftsteps");
            for (int i = 0; i < (int)FGUI::MacroDraftSteps.size(); i++)
            {
                auto& step = FGUI::MacroDraftSteps[i];
                ImGui::PushID(i);

                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.847f, 0.878f, 0.941f, 1.f));
                ImGui::TextWrapped("%s", step.Command.c_str());
                ImGui::PopStyleColor();
                ImGui::SameLine();

                if (ImGui::SmallButton("Remove"))
                {
                    FGUI::MacroDraftSteps.erase(FGUI::MacroDraftSteps.begin() + i);
                    ImGui::PopID();
                    break;
                }

                if (i + 1 < (int)FGUI::MacroDraftSteps.size())
                {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.353f, 0.388f, 0.478f, 1.f));
                    ImGui::Text("Wait: %d ms", step.DelayMs);
                    ImGui::PopStyleColor();
                }

                ImGui::PopID();
            }
            ImGui::PopID();
        }

        ImGui::Spacing();

        char pendingBindLabel[64];
        if (FGUI::RebindingCommandIndex == -1)
            snprintf(pendingBindLabel, sizeof(pendingBindLabel), "Press any key...  (Esc to cancel)");
        else
            snprintf(pendingBindLabel, sizeof(pendingBindLabel), "Bind Key    [%s]", BindName(FGUI::PendingCommandVK));

        const float BindButtonW = CW - AddButtonW - ImGui::GetStyle().ItemSpacing.x;
        if (ImGui::Button(pendingBindLabel, ImVec2(BindButtonW > 140.f ? BindButtonW : 140.f, 0.f)))
        {
            FGUI::RebindingCommandIndex = -1;
            FGUI::RebindingMacroIndex = -2;
            FGUI::bRebinding = false;
            FGUI::bRebindingJoin = false;
            BeginBindCapture();
        }

        ImGui::SameLine();

        if (ImGui::Button("Save", ImVec2(AddButtonW, 0.f)))
        {
            std::vector<HotkeyPersist::MacroStep> steps = FGUI::MacroDraftSteps;

            const char* start = FGUI::CommandInput;
            while (*start == ' ' || *start == '\t')
                start++;

            if (*start)
            {
                HotkeyPersist::MacroStep step{};
                step.Command = start;
                step.DelayMs = HotkeyPersist::SanitizeMacroDelayMs(atoi(FGUI::MacroDelayInput));
                steps.push_back(std::move(step));
            }

            if (!steps.empty())
            {
                const int vk = (FGUI::PendingCommandVK > 0 && FGUI::PendingCommandVK <= 254) ? FGUI::PendingCommandVK : 0;

                if (steps.size() == 1)
                {
                    HotkeyPersist::CommandBind command{};
                    command.Command = std::move(steps[0].Command);
                    command.VK = vk;
                    command.DefaultVK = vk;
                    FGUI::Commands.push_back(std::move(command));
                }
                else
                {
                    HotkeyPersist::CommandMacro macro{};
                    macro.Name = steps[0].Command;
                    macro.VK = vk;
                    macro.DefaultVK = vk;
                    macro.Steps = std::move(steps);
                    FGUI::Macros.push_back(std::move(macro));
                }

                FGUI::MacroDraftSteps.clear();
                FGUI::CommandInput[0] = '\0';
                FGUI::MacroDelayInput[0] = '\0';
                FGUI::PendingCommandVK = 0;
                FGUI::RebindingCommandIndex = -2;
                FGUI::SaveCommands();
            }
        }

        ImGui::Spacing();
        SectionLabel("Saved");

        if (FGUI::Commands.empty() && FGUI::Macros.empty())
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.353f, 0.388f, 0.478f, 1.f));
            ImGui::TextWrapped("No saved commands.");
            ImGui::PopStyleColor();
        }
        else
        {
            for (int i = 0; i < (int)FGUI::Commands.size(); i++)
            {
                auto& command = FGUI::Commands[i];
                ImGui::PushID(i);

                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.847f, 0.878f, 0.941f, 1.f));
                ImGui::TextWrapped("%s", command.Command.c_str());
                ImGui::PopStyleColor();

                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.353f, 0.388f, 0.478f, 1.f));
                ImGui::Text("Key: %s", BindName(command.VK));
                ImGui::PopStyleColor();

                const float ButtonW = (CW - ImGui::GetStyle().ItemSpacing.x * 2.f) / 3.f;
                if (ImGui::Button("Run", ImVec2(ButtonW, 0.f)))
                {
                    if (Exec(command.Command.c_str()))
                        AppendConsoleCommandEcho(
                            command.Command);
                }
                ImGui::SameLine();

                char bindLabel[64];
                if (FGUI::RebindingCommandIndex == i)
                    snprintf(bindLabel, sizeof(bindLabel), "Press key...");
                else if (command.VK > 0 && command.VK <= 254)
                    snprintf(bindLabel, sizeof(bindLabel), "Unbind");
                else
                    snprintf(bindLabel, sizeof(bindLabel), "Bind");

                if (ImGui::Button(bindLabel, ImVec2(ButtonW, 0.f)))
                {
                    if (FGUI::RebindingCommandIndex == i)
                    {
                        FGUI::RebindingCommandIndex = -2;
                        EndBindCapture();
                    }
                    else if (command.VK > 0 && command.VK <= 254)
                    {
                        command.VK = 0;
                        FGUI::RebindingCommandIndex = -2;
                        FGUI::SaveCommands();
                    }
                    else
                    {
                        FGUI::RebindingCommandIndex = i;
                        FGUI::RebindingMacroIndex = -2;
                        FGUI::bRebinding = false;
                        FGUI::bRebindingJoin = false;
                        BeginBindCapture();
                    }
                }
                ImGui::SameLine();

                if (ImGui::Button("Delete", ImVec2(ButtonW, 0.f)))
                {
                    FGUI::Commands.erase(FGUI::Commands.begin() + i);
                    FGUI::RebindingCommandIndex = -2;
                    FGUI::SaveCommands();
                    ImGui::PopID();
                    break;
                }

                ImGui::Separator();
                ImGui::PopID();
            }

            ImGui::PushID("macros");
            for (int i = 0; i < (int)FGUI::Macros.size(); i++)
            {
                auto& macro = FGUI::Macros[i];
                ImGui::PushID(i);

                for (size_t stepIndex = 0; stepIndex < macro.Steps.size(); stepIndex++)
                {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.847f, 0.878f, 0.941f, 1.f));
                    ImGui::TextWrapped("%s", macro.Steps[stepIndex].Command.c_str());
                    ImGui::PopStyleColor();

                    if (stepIndex + 1 < macro.Steps.size())
                    {
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.353f, 0.388f, 0.478f, 1.f));
                        ImGui::Text("    wait %d ms", macro.Steps[stepIndex].DelayMs);
                        ImGui::PopStyleColor();
                    }
                }

                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.353f, 0.388f, 0.478f, 1.f));
                ImGui::Text("Key: %s", BindName(macro.VK));
                ImGui::PopStyleColor();

                const float MacroButtonW = (CW - ImGui::GetStyle().ItemSpacing.x * 2.f) / 3.f;
                if (ImGui::Button("Run", ImVec2(MacroButtonW, 0.f)))
                    QueueMacro(macro);
                ImGui::SameLine();

                char macroBindLabel[64];
                if (FGUI::RebindingMacroIndex == i)
                    snprintf(macroBindLabel, sizeof(macroBindLabel), "Press key...");
                else if (macro.VK > 0 && macro.VK <= 254)
                    snprintf(macroBindLabel, sizeof(macroBindLabel), "Unbind");
                else
                    snprintf(macroBindLabel, sizeof(macroBindLabel), "Bind");

                if (ImGui::Button(macroBindLabel, ImVec2(MacroButtonW, 0.f)))
                {
                    if (FGUI::RebindingMacroIndex == i)
                    {
                        FGUI::RebindingMacroIndex = -2;
                        EndBindCapture();
                    }
                    else if (macro.VK > 0 && macro.VK <= 254)
                    {
                        macro.VK = 0;
                        FGUI::RebindingMacroIndex = -2;
                        FGUI::SaveMacros();
                    }
                    else
                    {
                        FGUI::RebindingMacroIndex = i;
                        FGUI::RebindingCommandIndex = -2;
                        FGUI::bRebinding = false;
                        FGUI::bRebindingJoin = false;
                        BeginBindCapture();
                    }
                }
                ImGui::SameLine();

                if (ImGui::Button("Delete", ImVec2(MacroButtonW, 0.f)))
                {
                    FGUI::Macros.erase(FGUI::Macros.begin() + i);
                    FGUI::RebindingMacroIndex = -2;
                    FGUI::SaveMacros();
                    ImGui::PopID();
                    break;
                }

                ImGui::Separator();
                ImGui::PopID();
            }
            ImGui::PopID();
        }

        break;
    }
    case 2: // Config
    {
    SectionLabel("Console Style");
    int consoleMode =
        FConfiguration::ConsoleMode.load(std::memory_order_acquire);
    ImGui::SetNextItemWidth(CW);
    if (ImGui::Combo("##console_mode", &consoleMode,
        "ATLAS Console\0Original UE Console\0"))
    {
        consoleMode = HotkeyPersist::SanitizeConsoleMode(consoleMode);
        FConfiguration::ConsoleMode.store(
            consoleMode, std::memory_order_release);
        HotkeyPersist::SaveConsoleMode(consoleMode);
        AtlasDiagnostics::WriteLine("console-mode changed=%s",
            consoleMode == static_cast<int>(EConsoleMode::Unreal)
                ? "original-unreal"
                : "atlas");

        if (consoleMode == static_cast<int>(EConsoleMode::Unreal))
            CloseConsoleSurface();
    }
    SectionLabel("Hotkeys");
    if (FGUI::bRebinding)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, Accent());
        ImGui::PushStyleColor(ImGuiCol_Button, Accent(0.18f));
        ImGui::Button("Press any key...  (Esc to cancel)", ImVec2(CW, 0.f));
        ImGui::PopStyleColor(2);
    }
    else
    {
        char btnLabel[64];
        snprintf(btnLabel, sizeof(btnLabel), "Rebind Menu Key      [%s]", VKName(FGUI::HotkeyVK));
        if (ImGui::Button(btnLabel, ImVec2(CW, 0.f)))
        {
            FGUI::bRebinding = true;
            FGUI::bRebindingJoin = false;
            FGUI::RebindingCommandIndex = -2;
            FGUI::RebindingMacroIndex = -2;
            BeginBindCapture();
        }
    }

    if (FGUI::bRebindingJoin)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, Accent());
        ImGui::PushStyleColor(ImGuiCol_Button, Accent(0.18f));
        ImGui::Button("Press any key...  (Esc to cancel)", ImVec2(CW, 0.f));
        ImGui::PopStyleColor(2);
    }
    else
    {
        char joinBtnLabel[64];
        snprintf(joinBtnLabel, sizeof(joinBtnLabel), "Rebind Join Key   [%s]", VKName(FGUI::JoinHotkeyVK));
        if (ImGui::Button(joinBtnLabel, ImVec2(CW, 0.f)))
        {
            FGUI::bRebindingJoin = true;
            FGUI::bRebinding = false;
            FGUI::RebindingCommandIndex = -2;
            FGUI::RebindingMacroIndex = -2;
            BeginBindCapture();
        }
    }

    if (!g_BindError.empty() && GetTickCount64() < g_BindErrorUntil)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.39f, 0.43f, 1.f));
        ImGui::TextWrapped("%s", g_BindError.c_str());
        ImGui::PopStyleColor();
    }

    if (ImGui::Button("Reset All", ImVec2(CW, 0.f)))
    {
        EndBindCapture();
        FGUI::ResetAll();
        CloseConsoleSurface();
    }

        ImGui::Dummy(ImVec2(0.f, 8.f));
        break;
    }
    }

    ImGui::EndChild();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor();
    ImGui::End();

    ImGui::PopStyleVar();

    if (!open)
        CloseMenuSurface();
}
