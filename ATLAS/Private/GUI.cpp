#include "pch.h"
#include "../Public/GUI.h"
#include "../Public/Hotkey.h"
#include "../Public/Configuration.h"
#include "../Public/Client.h"
#include "../Public/Diagnostics.h"
#include "../Public/Icon.h"
#include "../ImGui/imgui.h"

#include <sstream>
#include <iomanip>
#include <array>
#include <atomic>
#include <deque>
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
}

void FGUI::SaveHotkey() { HotkeyPersist::Save(FGUI::HotkeyVK); }
void FGUI::LoadHotkey() { FGUI::HotkeyVK = HotkeyPersist::Load(VK_F9); }

void FGUI::SaveJoinHotkey() { HotkeyPersist::Save(FGUI::JoinHotkeyVK, "joinHotkey"); }
void FGUI::LoadJoinHotkey() { FGUI::JoinHotkeyVK = HotkeyPersist::Load(VK_F5, "joinHotkey"); }

void FGUI::SaveCommands()
{
    HotkeyPersist::SaveCommands(FGUI::Commands);
    RefreshExclusiveCommandHotkeys();
}

void FGUI::LoadCommands()
{
    FGUI::Commands = HotkeyPersist::LoadCommands();
    RefreshExclusiveCommandHotkeys();
}

void FGUI::ResetAll()
{
    FGUI::HotkeyVK = VK_F9;
    FGUI::JoinHotkeyVK = VK_F5;
    FGUI::bRebinding = false;
    FGUI::bRebindingJoin = false;
    FGUI::RebindingCommandIndex = -2;
    FGUI::PendingCommandVK = 0;
    FGUI::CommandInput[0] = '\0';
    FGUI::Commands.clear();
    RefreshExclusiveCommandHotkeys();

    HotkeyPersist::SaveAll(FGUI::HotkeyVK, FGUI::JoinHotkeyVK, {});
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

static void ExecNow(const char* cmd)
{
    if (!cmd || !*cmd)
        return;

    auto world = UWorld::GetWorld();
    if (!world || !world->OwningGameInstance)
    {
        AtlasDiagnostics::WriteLine("command-skip no-world command=%s", cmd);
        return;
    }

    auto& localPlayers = world->OwningGameInstance->LocalPlayers;
    if (localPlayers.Num() == 0 || !localPlayers[0] || !localPlayers[0]->PlayerController)
    {
        AtlasDiagnostics::WriteLine("command-skip no-player command=%s", cmd);
        return;
    }

    const int len = MultiByteToWideChar(CP_UTF8, 0, cmd, -1, nullptr, 0);
    if (len <= 1)
        return;

    std::wstring wide(static_cast<size_t>(len), L'\0');
    if (MultiByteToWideChar(CP_UTF8, 0, cmd, -1, wide.data(), len) != len)
        return;
    wide.resize(static_cast<size_t>(len - 1));

    FString command(wide.c_str());
    AtlasDiagnostics::WriteLine("command-dispatch pc=%p command=%s", localPlayers[0]->PlayerController, cmd);
    UKismetSystemLibrary::ExecuteConsoleCommand(localPlayers[0]->PlayerController, command);
    AtlasDiagnostics::WriteLine("command-return command=%s", cmd);
    command.Free();
}

static std::deque<std::string> g_CommandQueue;
static ULONGLONG g_LastCommandDispatch = 0;

static void Exec(const char* cmd)
{
    if (!cmd || !*cmd)
        return;

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
    }
    else
    {
        AtlasDiagnostics::WriteLine("command-drop queue-full command=%s", cmd);
    }
}

static void DispatchQueuedCommand()
{
    if (g_CommandQueue.empty())
        return;

    const ULONGLONG now = GetTickCount64();
    if (g_LastCommandDispatch != 0 && now - g_LastCommandDispatch < 100)
        return;

    std::string command = std::move(g_CommandQueue.front());
    g_CommandQueue.pop_front();
    g_LastCommandDispatch = now;
    ExecNow(command.c_str());
}

static void ClearQueuedCommands()
{
    if (!g_CommandQueue.empty())
        AtlasDiagnostics::WriteLine("command-clear inactive depth=%zu", g_CommandQueue.size());
    g_CommandQueue.clear();
    g_LastCommandDispatch = 0;
}

static void SpawnConsole()
{
    auto engine = UEngine::GetEngine();
    if (engine && engine->GameViewport && engine->ConsoleClass)
        engine->GameViewport->ViewportConsole = UGameplayStatics::SpawnObject(engine->ConsoleClass, engine->GameViewport);
}

static void DestroyConsole()
{
    auto engine = UEngine::GetEngine();
    if (!engine || !engine->GameViewport || !engine->GameViewport->ViewportConsole)
        return;

    engine->GameViewport->ViewportConsole->ObjectFlags |= 0x4;
    engine->GameViewport->ViewportConsole = nullptr;
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

bool GUI_ShouldConsumeInputMessage(UINT message, WPARAM wParam)
{
    return IsExclusiveCommandHotkey(InputMessageVK(message, wParam));
}

bool GUI_ShouldConsumeRawInput(LPARAM lParam)
{
    RAWINPUT raw{};
    UINT rawSize = sizeof(raw);
    if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, &raw,
        &rawSize, sizeof(RAWINPUTHEADER)) == static_cast<UINT>(-1))
        return false;

    if (raw.header.dwType == RIM_TYPEKEYBOARD)
        return IsExclusiveCommandHotkey(static_cast<int>(raw.data.keyboard.VKey));

    if (raw.header.dwType != RIM_TYPEMOUSE)
        return false;

    const USHORT flags = raw.data.mouse.usButtonFlags;
    return ((flags & (RI_MOUSE_BUTTON_1_DOWN | RI_MOUSE_BUTTON_1_UP)) && IsExclusiveCommandHotkey(VK_LBUTTON)) ||
        ((flags & (RI_MOUSE_BUTTON_2_DOWN | RI_MOUSE_BUTTON_2_UP)) && IsExclusiveCommandHotkey(VK_RBUTTON)) ||
        ((flags & (RI_MOUSE_BUTTON_3_DOWN | RI_MOUSE_BUTTON_3_UP)) && IsExclusiveCommandHotkey(VK_MBUTTON)) ||
        ((flags & (RI_MOUSE_BUTTON_4_DOWN | RI_MOUSE_BUTTON_4_UP)) && IsExclusiveCommandHotkey(VK_XBUTTON1)) ||
        ((flags & (RI_MOUSE_BUTTON_5_DOWN | RI_MOUSE_BUTTON_5_UP)) && IsExclusiveCommandHotkey(VK_XBUTTON2));
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

static int PollBindKey(const InputPressCounts& presses)
{
    for (int vk = 0x01; vk <= 0xFE; vk++)
        if (IsBindableVK(vk) && presses[static_cast<size_t>(vk)] > 0)
            return vk;

    return 0;
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

    const bool wasVisible = FGUI::bVisible;

    if ((FGUI::bRebinding || FGUI::bRebindingJoin || FGUI::RebindingCommandIndex != -2) &&
        TakePressCount(presses, VK_ESCAPE) > 0)
    {
        FGUI::bRebinding = false;
        FGUI::bRebindingJoin = false;
        FGUI::RebindingCommandIndex = -2;
        EndBindCapture();
        return;
    }

    if (FGUI::bRebinding)
    {
        if (int vk = PollBindKey(presses))
        {
            FGUI::HotkeyVK = vk;
            FGUI::bRebinding = false;
            FGUI::SaveHotkey();
            EndBindCapture();
        }
        return;
    }

    if (FGUI::bRebindingJoin)
    {
        if (int vk = PollBindKey(presses))
        {
            FGUI::JoinHotkeyVK = vk;
            FGUI::bRebindingJoin = false;
            FGUI::SaveJoinHotkey();
            EndBindCapture();
        }
        return;
    }

    if (FGUI::RebindingCommandIndex != -2)
    {
        if (int vk = PollBindKey(presses))
        {
            if (FGUI::RebindingCommandIndex == -1)
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

    const unsigned int hotkeyPresses = TakePressCount(presses, FGUI::HotkeyVK);
    if ((hotkeyPresses & 1u) != 0)
        FGUI::bVisible = !FGUI::bVisible;

    if (TakePressCount(presses, FGUI::JoinHotkeyVK) > 0)
        JoinSelectedHost();

    // Do not execute presses made while the overlay was visible, including
    // other keys received in the same batch as the close hotkey.
    if (!wasVisible && !FGUI::bVisible)
    {
        for (const auto& command : FGUI::Commands)
        {
            const unsigned int pressCount = TakePressCount(presses, command.VK);
            for (unsigned int i = 0; i < pressCount; i++)
                Exec(command.Command.c_str());
        }
    }
}

void GUI_Render()
{
    ImGuiIO& io = ImGui::GetIO();

    static float s_Fade = 0.f;
    const float target = FGUI::bVisible ? 1.f : 0.f;
    const float fadeStep = (io.DeltaTime > 0.f ? io.DeltaTime : 1.f / 60.f) / 0.15f;
    if (s_Fade < target) { s_Fade += fadeStep; if (s_Fade > target) s_Fade = target; }
    else if (s_Fade > target) { s_Fade -= fadeStep; if (s_Fade < target) s_Fade = target; }

    if (s_Fade <= 0.f)
        return;

    const float fade = s_Fade * s_Fade * (3.f - 2.f * s_Fade);

    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0.f, 0.f), io.DisplaySize, IM_COL32(0, 0, 0, (int)(fade * 0.50f * 255.f)));

    ImGuiWindowFlags wflags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoTitleBar;

    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Once, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(580.f, 410.f), ImGuiCond_Once);
    ImGui::SetNextWindowSizeConstraints(ImVec2(500.f, 320.f), ImVec2(10000.f, 10000.f));

    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, fade);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.f, 0.f));
    bool open = true;
    ImGui::Begin("##atlas_main", &open, wflags);
    ImGui::PopStyleVar();

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
        ImGui::Text("| Console");
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
            FGUI::bVisible = false;
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
        ImDrawList* fdl = ImGui::GetForegroundDrawList();
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

    SectionLabel("Console");
    if (ImGui::Checkbox("Console Enabled", &FConfiguration::bConsoleEnabled))
    {
        if (FConfiguration::bConsoleEnabled)
            SpawnConsole();
        else
            DestroyConsole();
    }

        break;
    }
    case 1: // Commands
    {
        SectionLabel("Commands");

        ImGui::PushItemWidth(CW);
        ImGui::InputTextWithHint("##commandinput", "UE Console Command", FGUI::CommandInput, sizeof(FGUI::CommandInput));
        ImGui::PopItemWidth();

        ImGui::Spacing();

        char pendingBindLabel[64];
        if (FGUI::RebindingCommandIndex == -1)
            snprintf(pendingBindLabel, sizeof(pendingBindLabel), "Press any key...  (Esc to cancel)");
        else
            snprintf(pendingBindLabel, sizeof(pendingBindLabel), "Bind Key    [%s]", BindName(FGUI::PendingCommandVK));

        const float AddButtonW = 110.f;
        const float BindButtonW = CW - AddButtonW - ImGui::GetStyle().ItemSpacing.x;
        if (ImGui::Button(pendingBindLabel, ImVec2(BindButtonW > 140.f ? BindButtonW : 140.f, 0.f)))
        {
            FGUI::RebindingCommandIndex = -1;
            FGUI::bRebinding = false;
            FGUI::bRebindingJoin = false;
            BeginBindCapture();
        }

        ImGui::SameLine();

        if (ImGui::Button("Save Command", ImVec2(AddButtonW, 0.f)))
        {
            const char* start = FGUI::CommandInput;
            while (*start == ' ' || *start == '\t')
                start++;

            if (*start)
            {
                HotkeyPersist::CommandBind command{};
                command.Command = start;
                command.VK = (FGUI::PendingCommandVK > 0 && FGUI::PendingCommandVK <= 254) ? FGUI::PendingCommandVK : 0;
                command.DefaultVK = command.VK;
                FGUI::Commands.push_back(command);
                FGUI::CommandInput[0] = '\0';
                FGUI::PendingCommandVK = 0;
                FGUI::RebindingCommandIndex = -2;
                FGUI::SaveCommands();
            }
        }

        ImGui::Spacing();
        SectionLabel("Saved");

        if (FGUI::Commands.empty())
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
                    Exec(command.Command.c_str());
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
        }

        break;
    }
    case 2: // Config
    {
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
        snprintf(btnLabel, sizeof(btnLabel), "Rebind GUI Key    [%s]", VKName(FGUI::HotkeyVK));
        if (ImGui::Button(btnLabel, ImVec2(CW, 0.f)))
        {
            FGUI::bRebinding = true;
            FGUI::bRebindingJoin = false;
            FGUI::RebindingCommandIndex = -2;
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
            BeginBindCapture();
        }
    }

    if (ImGui::Button("Reset All", ImVec2(CW, 0.f)))
    {
        EndBindCapture();
        FGUI::ResetAll();
    }

    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.353f, 0.388f, 0.478f, 1.f));
    ImGui::TextWrapped("Hotkeys and command binds are saved automatically. Reset All restores default hotkeys and deletes saved commands.");
    ImGui::PopStyleColor();

        ImGui::Dummy(ImVec2(0.f, 8.f));
        break;
    }
    }

    ImGui::EndChild();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor();
    ImGui::End();

    ImGui::PopStyleVar();
}
