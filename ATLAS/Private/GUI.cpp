#include "pch.h"
#include "../Public/GUI.h"
#include "../Public/Hotkey.h"
#include "../Public/Configuration.h"
#include "../Public/Client.h"
#include "../Public/Icon.h"
#include "../ImGui/imgui.h"

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
    default:        return "???";
    }
}

void FGUI::SaveHotkey() { HotkeyPersist::Save(FGUI::HotkeyVK); }
void FGUI::LoadHotkey() { FGUI::HotkeyVK = HotkeyPersist::Load(VK_F9); }

void GUI_LoadTextures(ID3D11Device* device)
{
    int w, h, ch;
    unsigned char* pixels = stbi_load_from_memory(Icon, (int)sizeof(Icon), &w, &h, &ch, 4);

    if (!pixels) 
        return;

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = (UINT)w;
    desc.Height = (UINT)h;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA initData{};
    initData.pSysMem = pixels;
    initData.SysMemPitch = (UINT)(w * 4);

    ID3D11Texture2D* tex = nullptr;
    if (SUCCEEDED(device->CreateTexture2D(&desc, &initData, &tex)))
    {
        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(tex, &srvDesc, &FGUI::LogoTexture);
        tex->Release();
        FGUI::LogoW = w;
        FGUI::LogoH = h;
    }

    stbi_image_free(pixels);
}

static void PushStyle()
{
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

    col[ImGuiCol_WindowBg] = C(0.043f, 0.051f, 0.071f, 0.97f);
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
    col[ImGuiCol_ResizeGrip] = C(0.f, 0.f, 0.f, 0.f);
    col[ImGuiCol_ResizeGripHovered] = C(0.f, 0.f, 0.f, 0.f);
    col[ImGuiCol_ResizeGripActive] = C(0.f, 0.f, 0.f, 0.f);
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

static void SectionLabel(const char* label)
{
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, Accent(0.85f));
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
    ImGui::PushStyleColor(ImGuiCol_Separator, Accent(0.25f));
    ImGui::Separator();
    ImGui::PopStyleColor();
    ImGui::Spacing();
}

static bool ATCheckbox(const char* label, bool* v)
{
    return ImGui::Checkbox(label, v);
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

    Engine->GameViewport->ViewportConsole->ObjectFlags |= 0x4; // RF_BeginDestroyed — marks it for GC
    Engine->GameViewport->ViewportConsole = nullptr;
}

void GUI_Init()
{
    FGUI::LoadHotkey();
    PushStyle();
}

void GUI_HandleInput()
{
    if (FGUI::bRebinding)
    {
        static const int candidates[] = {
            VK_F1, VK_F2, VK_F3, VK_F4, VK_F5,  VK_F6,
            VK_F7, VK_F8, VK_F9, VK_F10, VK_F11, VK_F12,
            VK_INSERT, VK_DELETE, VK_HOME, VK_END, VK_PRIOR, VK_NEXT
        };

        for (int vk : candidates)
        {
            if (GetAsyncKeyState(vk) & 1)
            {
                FGUI::HotkeyVK = vk;
                FGUI::bRebinding = false;
                FGUI::SaveHotkey();
                break;
            }
        }

        return;
    }

    if (GetAsyncKeyState(FGUI::HotkeyVK) & 1)
        FGUI::bVisible = !FGUI::bVisible;
}

void GUI_Render()
{
    if (!FGUI::bVisible) 
        return;

    ImGuiWindowFlags wflags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoTitleBar;

    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Once, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(400.f, 520.f), ImGuiCond_Once);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.f, 0.f));
    bool open = true;
    ImGui::Begin("##atlas_main", &open, wflags);
    ImGui::PopStyleVar();

    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 wp = ImGui::GetWindowPos();
        ImVec2 ws = ImGui::GetWindowSize();
        //dl->AddRectFilled(wp, ImVec2(wp.x + ws.x, wp.y + 2.f), IM_COL32(11, 13, 18, 255)); // delete??
    }

    ImGui::SetCursorPosY(3.f);

    {
        const float headerH = 52.f;
        const float logoSize = 40.f;
        const float padL = 12.f;

        ImGui::SetCursorPos(ImVec2(padL, 3.f + (headerH - logoSize) * 0.5f));

        if (FGUI::LogoTexture)
            ImGui::Image((ImTextureID)FGUI::LogoTexture, ImVec2(logoSize, logoSize));
        else
            ImGui::Dummy(ImVec2(logoSize, logoSize));

        ImGui::SameLine(padL + logoSize + 8.f);
        const float titleY = 3.f + (headerH * 0.5f) - ImGui::GetTextLineHeight() * 0.5f;
        ImGui::SetCursorPosY(titleY);

        ImGui::PushStyleColor(ImGuiCol_Text, Accent());
        ImGui::Text("ATLAS");
        ImGui::PopStyleColor();
        ImGui::SameLine(0.f, 6.f);
        ImGui::SetCursorPosY(titleY);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.353f, 0.388f, 0.478f, 1.f));
        ImGui::Text("| console");
        ImGui::PopStyleColor();

        char badge[32];
        snprintf(badge, sizeof(badge), "[%s]", VKName(FGUI::HotkeyVK));
        float badgeW = ImGui::CalcTextSize(badge).x;
        ImGui::SameLine(ImGui::GetWindowWidth() - badgeW - 14.f);
        ImGui::SetCursorPosY(3.f + (headerH * 0.5f) - ImGui::GetTextLineHeight() * 0.5f);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.353f, 0.388f, 0.478f, 0.8f));
        ImGui::TextUnformatted(badge);
        ImGui::PopStyleColor();

        ImGui::SetCursorPosY(3.f + headerH);
    }

    ImGui::SetCursorPosX(0.f);
    ImGui::PushStyleColor(ImGuiCol_Separator, ImVec4(0.137f, 0.165f, 0.243f, 1.f));
    ImGui::Separator();
    ImGui::PopStyleColor();

    ImGui::BeginChild("##content", ImVec2(0.f, -1.f), false, ImGuiWindowFlags_NoScrollbar);
    ImGui::SetCursorPosX(14.f);
    ImGui::BeginGroup();

    SectionLabel("EDITING");

    ImGui::Checkbox("Edit On Release", &FConfiguration::bEOREnabled);
    ImGui::SameLine(0.f, 10.f);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.353f, 0.388f, 0.478f, 1.f));
    ImGui::TextUnformatted("EOR");
    ImGui::PopStyleColor();

    ImGui::Checkbox("Reset On Release", &FConfiguration::bROREnabled);
    ImGui::SameLine(0.f, 10.f);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.353f, 0.388f, 0.478f, 1.f));
    ImGui::TextUnformatted("ROR");
    ImGui::PopStyleColor();

    ImGui::Checkbox("Disable Pre-Edits", &FConfiguration::bDisablePreEdits);

    SectionLabel("RESPAWNING");

    ImGui::Checkbox("Respawns Enabled", &FConfiguration::bForceRespawns);

    if (FConfiguration::bForceRespawns)
    {
        //ImGui::Indent(16.f);

        bool RespawnTimeEnabled = (FConfiguration::RespawnTime > 0);
        bool RespawnHeightEnabled = (FConfiguration::RespawnHeight > 0);

        if (ImGui::Checkbox("Custom Respawn Time", &RespawnTimeEnabled))
            FConfiguration::RespawnTime = RespawnTimeEnabled ? 3 : 0;

        if (RespawnTimeEnabled)
        {
            ImGui::SameLine();
            ImGui::PushItemWidth(80.f);
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
            ImGui::PushItemWidth(100.f);
            ImGui::SliderInt("##rheight", &FConfiguration::RespawnHeight, 1000, 50000);
            ImGui::PopItemWidth();
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.353f, 0.388f, 0.478f, 1.f));
            ImGui::Text("uu");
            ImGui::PopStyleColor();
        }

        //ImGui::Unindent(16.f);
    }

    SectionLabel("CONSOLE");

    if (ImGui::Checkbox("Console Enabled", &FConfiguration::bConsoleEnabled))
    {
        if (FConfiguration::bConsoleEnabled)
            SpawnConsole();
        else
            DestroyConsole();
    }

    if (ImGui::Checkbox("Potato Graphics", &FGUI::bPotatoGraphics))
    {
        auto World = UWorld::GetWorld();

        if (World)
            UKismetSystemLibrary::ExecuteConsoleCommand(World, FString(FGUI::bPotatoGraphics ? L"r.MipMapLODBias 7" : L"r.MipMapLODBias 0"), nullptr);
    }

    // fov
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.847f, 0.878f, 0.941f, 1.f));
        ImGui::Text("FOV");
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::PushItemWidth(180.f);
        if (ImGui::SliderInt("##fov", &FConfiguration::FOV, 0, 175))
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

    SectionLabel("HOTKEYS");

    if (FGUI::bRebinding)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, Accent());
        ImGui::PushStyleColor(ImGuiCol_Button, Accent(0.18f));
        ImGui::Button("Press any key...", ImVec2(370.f, 0.f));
        ImGui::PopStyleColor(2);
    }
    else
    {
        char btnLabel[64];
        snprintf(btnLabel, sizeof(btnLabel), "Rebind GUI Key  [%s]", VKName(FGUI::HotkeyVK));
        if (ImGui::Button(btnLabel, ImVec2(370.f, 0.f)))
            FGUI::bRebinding = true;
    }

    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.353f, 0.388f, 0.478f, 1.f));
    ImGui::TextWrapped("Hotkey is saved automatically and will persist across sessions.");
    ImGui::PopStyleColor();

    ImGui::Dummy(ImVec2(0.f, 8.f));
    ImGui::EndGroup();
    ImGui::EndChild();
    ImGui::End();
}