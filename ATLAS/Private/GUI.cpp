#include "pch.h"
#include "../Public/GUI.h"
#include "../Public/Hotkey.h"
#include "../Public/Configuration.h"
#include "../Public/Client.h"
#include "../Public/Icon.h"
#include "../ImGui/imgui.h"

#include <sstream>
#include <iomanip>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG

#include "../ImGui/stb_image.h"

extern void* SelectEdit(void*);
extern void* SelectReset(void*);

#define ACCENT_R 0.f
#define ACCENT_G 0.635f
#define ACCENT_B 0.808f

static ImVec4 Accent(float a = 1.f) { return ImVec4(ACCENT_R, ACCENT_G, ACCENT_B, a); }
static ImVec4 AccentDk(float a = 1.f) { return ImVec4(0.f, 0.38f, 0.50f, a); }  // darker for active

static const char* VKName(int vk)
{
    static char buf[32];

    switch (vk)
    {
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

void FGUI::SaveHotkey() { HotkeyPersist::Save(FGUI::HotkeyVK); }
void FGUI::LoadHotkey() { FGUI::HotkeyVK = HotkeyPersist::Load(VK_F9); }

void FGUI::SaveJoinHotkey() { HotkeyPersist::Save(FGUI::JoinHotkeyVK, "joinHotkey"); }
void FGUI::LoadJoinHotkey() { FGUI::JoinHotkeyVK = HotkeyPersist::Load(VK_F5, "joinHotkey"); }

void GUI_LoadTextures(ID3D11Device* device)
{
    int w, h, ch;
    unsigned char* pixels = stbi_load_from_memory(Icon, (int)sizeof(Icon), &w, &h, &ch, 4);

    if (!pixels) 
        return;

    // Full mip chain + GPU-generated mips so the high-res (1024x1024) source
    // downscales smoothly to the small on-screen size instead of aliasing into a
    // pixelated mess (a plain single-level texture has no mips to filter against).
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = (UINT)w;
    desc.Height = (UINT)h;
    desc.MipLevels = 0; // 0 => allocate a full mip chain
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

        // upload the full-resolution image into mip 0, then build the smaller mips
        ctx->UpdateSubresource(tex, 0, nullptr, pixels, (UINT)(w * 4), 0);

        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = (UINT)-1; // use all available mips
        if (SUCCEEDED(device->CreateShaderResourceView(tex, &srvDesc, &FGUI::LogoTexture)))
        {
            ctx->GenerateMips(FGUI::LogoTexture);
            FGUI::LogoW = w;
            FGUI::LogoH = h;
        }

        if (ctx) ctx->Release();
        tex->Release();
    }

    stbi_image_free(pixels);
}

static void PushStyle()
{
    ImFontConfig FontConfig;
    FontConfig.FontDataOwnedByAtlas = false;
    ImGui::GetIO().Fonts->AddFontFromMemoryTTF((void*)Font, sizeof(Font), 17.f, &FontConfig);

    ImGuiStyle& s = ImGui::GetStyle();

    s.WindowRounding = 8.f;
    s.ChildRounding = 6.f;
    s.FrameRounding = 4.f;
    s.GrabRounding = 4.f;
    s.PopupRounding = 4.f;
    s.ScrollbarRounding = 4.f;
    s.TabRounding = 4.f;
    s.WindowPadding = { 16.f, 14.f };
    s.FramePadding = { 10.f,  5.f };
    s.ItemSpacing = { 8.f,  7.f };
    s.ItemInnerSpacing = { 6.f,  4.f };
    s.IndentSpacing = 18.f;
    s.ScrollbarSize = 4.f;
    s.GrabMinSize = 10.f;
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

// Thin accent rule inset to the content padding. Unlike ImGui::Separator(), which
// spans the whole window width edge-to-edge, this honours the left/right margins.
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

// One full-width vertical tab in the left sidebar. Returns true on click.
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

static void Exec(const char* cmd)
{
    if (!UWorld::GetWorld() || !UWorld::GetWorld()->OwningGameInstance) return;
    auto& lp = UWorld::GetWorld()->OwningGameInstance->LocalPlayers;
    if (lp.Num() == 0) return;
    auto pc = lp[0]->PlayerController;
    if (!pc) return;

    int len = MultiByteToWideChar(CP_UTF8, 0, cmd, -1, nullptr, 0);
    std::wstring ws(len - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, cmd, -1, ws.data(), len);
    UKismetSystemLibrary::ExecuteConsoleCommand(pc, FString(ws.c_str()));
}

static void ExecEngine(const char* cmd)
{
    auto World = UWorld::GetWorld();
    if (!World) return;

    int len = MultiByteToWideChar(CP_UTF8, 0, cmd, -1, nullptr, 0);
    std::wstring ws(len - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, cmd, -1, ws.data(), len);
    UKismetSystemLibrary::ExecuteConsoleCommand(World, FString(ws.c_str()), nullptr);
}

static void SpawnConsole()
{
    auto Engine = UEngine::GetEngine();

    if (Engine && Engine->GameViewport && Engine->ConsoleClass)
        Engine->GameViewport->ViewportConsole = UGameplayStatics::SpawnObject(Engine->ConsoleClass, Engine->GameViewport);
}

static void DestroyConsole()
{
    auto Engine = UEngine::GetEngine();

    if (!Engine || !Engine->GameViewport || !Engine->GameViewport->ViewportConsole)
        return;

    Engine->GameViewport->ViewportConsole->ObjectFlags |= 0x4;
    Engine->GameViewport->ViewportConsole = nullptr;
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
    FGUI::LoadHotkey();
    FGUI::LoadJoinHotkey();
    PushStyle();
}

// Keys we never bind to: mouse buttons (vk < 0x08), the bare modifiers, and ESC
// (reserved to cancel a rebind).
static bool IsBindableVK(int vk)
{
    switch (vk)
    {
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

// Returns the first bindable key pressed this frame, or 0 if none.
static int PollBindKey()
{
    for (int vk = 0x08; vk <= 0xFE; vk++)
        if (IsBindableVK(vk) && (GetAsyncKeyState(vk) & 1))
            return vk;
    return 0;
}

void GUI_HandleInput()
{
    // ESC cancels any pending rebind without changing the bound key.
    if ((FGUI::bRebinding || FGUI::bRebindingJoin) && (GetAsyncKeyState(VK_ESCAPE) & 1))
    {
        FGUI::bRebinding = false;
        FGUI::bRebindingJoin = false;
        return;
    }

    if (FGUI::bRebinding)
    {
        if (int vk = PollBindKey())
        {
            FGUI::HotkeyVK = vk;
            FGUI::bRebinding = false;
            FGUI::SaveHotkey();
        }
        return;
    }

    if (FGUI::bRebindingJoin)
    {
        if (int vk = PollBindKey())
        {
            FGUI::JoinHotkeyVK = vk;
            FGUI::bRebindingJoin = false;
            FGUI::SaveJoinHotkey();
        }
        return;
    }

    if (GetAsyncKeyState(FGUI::HotkeyVK) & 1)
        FGUI::bVisible = !FGUI::bVisible;

    if (GetAsyncKeyState(FGUI::JoinHotkeyVK) & 1)
        JoinSelectedHost();
}

void GUI_Render()
{
    ImGuiIO& io = ImGui::GetIO();

    // Fade animation: ramp toward 1 while visible, toward 0 while hidden.
    // Driving the whole window (and the backdrop) off this gives a smooth fade in/out.
    static float s_Fade = 0.f;
    const float target = FGUI::bVisible ? 1.f : 0.f;
    const float fadeStep = (io.DeltaTime > 0.f ? io.DeltaTime : 1.f / 60.f) / 0.15f; // ~0.15s
    if (s_Fade < target) { s_Fade += fadeStep; if (s_Fade > target) s_Fade = target; }
    else if (s_Fade > target) { s_Fade -= fadeStep; if (s_Fade < target) s_Fade = target; }

    if (s_Fade <= 0.f)
        return;

    const float fade = s_Fade * s_Fade * (3.f - 2.f * s_Fade); // smoothstep easing

    // Full-screen black backdrop behind the menu (50% dim), fading in with it.
    ImGui::GetBackgroundDrawList()->AddRectFilled(
        ImVec2(0.f, 0.f), io.DisplaySize, IM_COL32(0, 0, 0, (int)(fade * 0.50f * 255.f)));

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

    // ---- Top bar: logo + branding (left), hotkey badge + version (right) ----
    {
        // top bar fill (#0e1118), rounded to match the window's top corners
        ImGui::GetWindowDrawList()->AddRectFilled(
            wp, ImVec2(wp.x + W, wp.y + TopBarH),
            ImGui::GetColorU32(ImVec4(0.0549f, 0.0667f, 0.0941f, 1.f)),
            ImGui::GetStyle().WindowRounding, ImDrawFlags_RoundCornersTop);

        const float LogoSize = 30.f;
        const float PadL = 14.f;

        ImGui::SetCursorPos(ImVec2(PadL, (TopBarH - LogoSize) * 0.5f));
        if (FGUI::LogoTexture)
            ImGui::Image((ImTextureID)FGUI::LogoTexture, ImVec2(LogoSize, LogoSize));
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

        // version sits inline right after the title
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

    // Panel divider lines (drawn on top so the child backgrounds don't cover them).
    {
        ImDrawList* fdl = ImGui::GetForegroundDrawList();
        const ImU32 line = IM_COL32(35, 42, 62, (int)(fade * 255.f));
        fdl->AddLine(ImVec2(wp.x, wp.y + TopBarH), ImVec2(wp.x + W, wp.y + TopBarH), line, 1.f);
        fdl->AddLine(ImVec2(wp.x + SidebarW, wp.y + TopBarH), ImVec2(wp.x + SidebarW, wp.y + H), line, 1.f);
    }

    // ---- Sidebar: vertical tabs + animated accent indicator ----
    static const char* kTabs[] = { "Main", "Config" };
    const int kTabCount = (int)(sizeof(kTabs) / sizeof(kTabs[0]));
    const float TabH = 40.f;
    const float TabsTop = 12.f;

    ImGui::SetCursorPos(ImVec2(0.f, TopBarH));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0549f, 0.0667f, 0.0941f, 1.f)); // #0e1118
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.f, 0.f));
    ImGui::BeginChild("##sidebar", ImVec2(SidebarW, H - TopBarH), false, ImGuiWindowFlags_NoScrollbar);
    {
        const ImVec2 sbPos = ImGui::GetWindowPos();

        // Place each tab at an exact Y so they are contiguous (no item-spacing gaps),
        // which keeps the indicator below lined up with the tab it points at.
        for (int i = 0; i < kTabCount; i++)
        {
            ImGui::SetCursorPos(ImVec2(0.f, TabsTop + i * TabH));
            SidebarTab(kTabs[i], i, TabH);
        }

        // Accent indicator that slides to the active tab. Animated in sidebar-local
        // space (then offset by sbPos) so it stays glued to the tab while the window
        // is being dragged instead of lagging/wiggling behind it.
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

    // ---- Content panel: the active tab's controls. The child is physically inset
    // from the divider and the window's right/bottom edges so nothing hugs the edges,
    // and it's transparent so the inset margin blends into the window background.
    // (No per-tab animation; the sidebar indicator is the only tab-switch motion.) ----
    const float ContentPadX = 28.f;
    const float ContentPadTop = 12.f;
    ImGui::SetCursorPos(ImVec2(SidebarW + ContentPadX, TopBarH + ContentPadTop));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.f, 0.f, 0.f, 0.f));
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, fade);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.f, 0.f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.f, 10.f));
    ImGui::BeginChild("##content",
        ImVec2((W - SidebarW) - ContentPadX * 2.f, (H - TopBarH) - ContentPadTop - 12.f), false);

    const float CW = ImGui::GetContentRegionAvail().x;

    switch (FGUI::ActiveTab)
    {
    case 0: // Main (Host + Editing + Respawn + Console)
    {
        SectionLabel("Host");

    const char* hostTypeItems[] = { "Local Host", "Remote Host" };
    ImGui::PushItemWidth(CW);
    // The content child runs with zero window padding; the dropdown popup would
    // inherit that and look cramped, so give it its own padding + roomier rows.
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

        if (VersionInfo.FortniteVersion < 24.30)
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
        {
            ImGui::Checkbox("Disable Pre-Edits", &FConfiguration::bDisablePreEdits);
        }
        }

        SectionLabel("Respawn");

        ImGui::Checkbox("Respawns Enabled", &FConfiguration::bForceRespawns);

    if (FConfiguration::bForceRespawns)
    {
        //ImGui::Indent(16.f);

        bool RespawnTimeEnabled = (FConfiguration::RespawnTime > 0);
        bool RespawnHeightEnabled = (FConfiguration::RespawnHeight > 0);

        // Align both rows' sliders to a shared column past the widest checkbox label,
        // and give them an identical width that fills the remaining space (minus room
        // for the unit label) so they stay equal and fit at any window width.
        const float RowStartX = ImGui::GetCursorPosX();
        const float RowAvail = ImGui::GetContentRegionAvail().x;
        const float LabelW = ImGui::CalcTextSize("Custom Respawn Height").x; // widest label
        const float SliderX = RowStartX + ImGui::GetFrameHeight()
            + ImGui::GetStyle().ItemInnerSpacing.x + LabelW + 16.f;
        float SliderW = (RowStartX + RowAvail) - SliderX - 42.f; // 42px reserved for unit
        if (SliderW < 60.f) SliderW = 60.f;

        if (ImGui::Checkbox("Custom Respawn Time", &RespawnTimeEnabled))
            FConfiguration::RespawnTime = RespawnTimeEnabled ? 3 : 0;

        if (RespawnTimeEnabled)
        {
            ImGui::SameLine();
            ImGui::SetCursorPosX(SliderX);
            ImGui::PushItemWidth(SliderW);
            ImGui::SliderInt("##rtime", &FConfiguration::RespawnTime, 1, 30);
            ImGui::PopItemWidth();
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.353f, 0.388f, 0.478f, 1.f));
            ImGui::Text("sec");
            ImGui::PopStyleColor();
        }

        if (ImGui::Checkbox("Custom Respawn Height", &RespawnHeightEnabled))
            FConfiguration::RespawnHeight = RespawnHeightEnabled ? 20000 : 0;

        if (RespawnHeightEnabled)
        {
            ImGui::SameLine();
            ImGui::SetCursorPosX(SliderX);
            ImGui::PushItemWidth(SliderW);
            ImGui::SliderInt("##rheight", &FConfiguration::RespawnHeight, 1000, 50000);
            ImGui::PopItemWidth();
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.353f, 0.388f, 0.478f, 1.f));
            ImGui::Text("ue");
            ImGui::PopStyleColor();
        }

        //ImGui::Unindent(16.f);
        }

        SectionLabel("Console");

        if (ImGui::Checkbox("Console Enabled", &FConfiguration::bConsoleEnabled))
    {
        if (FConfiguration::bConsoleEnabled)
            SpawnConsole();
        else
            DestroyConsole();
    }

    /*
	// LOD bias
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.847f, 0.878f, 0.941f, 1.f));
        ImGui::Text("Potato Graphics");
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::PushItemWidth(180.f);

        if (ImGui::SliderInt("##LODBias", &FConfiguration::LODBias, -3, 7))
        {
            std::ostringstream ss;
            ss.imbue(std::locale::classic());
            ss << "r.MipMapLODBias " << std::fixed << std::setprecision(1) << FConfiguration::LODBias;
            std::string Command = ss.str();

            auto Engine = UEngine::GetEngine();
            if (Engine && Engine->GameViewport && Engine->GameViewport->ViewportConsole)
            {
                int len = MultiByteToWideChar(CP_UTF8, 0, Command.c_str(), -1, nullptr, 0);
                std::wstring ws(len - 1, L'\0');
                MultiByteToWideChar(CP_UTF8, 0, Command.c_str(), -1, ws.data(), len);

                ((UConsole*)Engine->GameViewport->ViewportConsole)->ConsoleCommand(FString(ws.c_str()));
            }
        }

        ImGui::PopItemWidth();
    } */

    // fov
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.847f, 0.878f, 0.941f, 1.f));
        ImGui::Text("FOV");
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::PushItemWidth(180.f);

        if (ImGui::SliderInt("##fov", &FConfiguration::FOV, 1, 175))
        {
            char cmd[32];
            snprintf(cmd, sizeof(cmd), "fov %d", FConfiguration::FOV);
            Exec(cmd);
        }

        ImGui::PopItemWidth();
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.353f, 0.388f, 0.478f, 1.f));
        ImGui::Text("%d\xc2\xb0", FConfiguration::FOV);
        ImGui::PopStyleColor();
    }

	if (ImGui::Button("Reset FOV", ImVec2(100.f, 0.f)))
    {
        FConfiguration::FOV = 80;
        Exec("fov");
    }

    /* {
        const char* resItems[] = { "1920x1080", "1720x1080", "1280x720" };
        const char* resCmds[] = { "setres 1920x1080", "setres 1720x1080", "setres 1280x720" };
        ImGui::Text("Resolution");
        ImGui::SameLine();
        ImGui::PushItemWidth(140.f);
        if (ImGui::BeginCombo("##res", resItems[FGUI::Resolution]))
        {
            for (int i = 0; i < 3; i++)
            {
                bool sel = (FGUI::Resolution == i);
                if (ImGui::Selectable(resItems[i], sel))
                {
                    FGUI::Resolution = i;
                    Exec(resCmds[i]);
                }
                if (sel) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::PopItemWidth();
    } */

        break;
    }
    case 1: // Config
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
        }
    }

    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.353f, 0.388f, 0.478f, 1.f));
    ImGui::TextWrapped("Hotkey is saved automatically and will persist across sessions.");
    ImGui::PopStyleColor();

        ImGui::Dummy(ImVec2(0.f, 8.f));
        break;
    }
    }

    ImGui::EndChild();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor();
    ImGui::End();

    ImGui::PopStyleVar(); // ImGuiStyleVar_Alpha
}