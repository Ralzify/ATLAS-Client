#include "pch.h"
#include "../Public/GUI.h"
#include "../Public/Hotkey.h"
#include "../Public/Configuration.h"
#include "../Public/Client.h"
#include "../ImGui/imgui.h"

extern void* SelectEdit(void*);
extern void* SelectReset(void*);

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

static void PushATLASStyle()
{
    ImGuiStyle& s = ImGui::GetStyle();

    // Geometry
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
    s.ScrollbarSize = 10.f;
    s.GrabMinSize = 10.f;
    s.WindowBorderSize = 1.f;
    s.FrameBorderSize = 0.f;

    // ── Palette ──────────────────────────────────────────────────────────────
    //   bg-deep   #0B0D12   base window fill
    //   bg-panel  #111520   child / header panels
    //   bg-frame  #181D2A   input/frame backgrounds
    //   accent    #3DF5C8   primary interactive cyan-mint
    //   accent-dk #1A8F73   darker accent (hover/active)
    //   text      #D8E0F0   primary text
    //   text-dim  #5A637A   disabled / muted text
    //   border    #232A3E   subtle separator

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
    col[ImGuiCol_ScrollbarGrabHovered] = C(0.239f, 0.96f, 0.784f, 0.5f);
    col[ImGuiCol_ScrollbarGrabActive] = C(0.239f, 0.96f, 0.784f, 1.f);

    col[ImGuiCol_CheckMark] = C(0.239f, 0.96f, 0.784f, 1.f);
    col[ImGuiCol_SliderGrab] = C(0.239f, 0.96f, 0.784f, 1.f);
    col[ImGuiCol_SliderGrabActive] = C(0.102f, 0.561f, 0.451f, 1.f);

    col[ImGuiCol_Button] = C(0.094f, 0.114f, 0.165f, 1.f);
    col[ImGuiCol_ButtonHovered] = C(0.239f, 0.96f, 0.784f, 0.18f);
    col[ImGuiCol_ButtonActive] = C(0.239f, 0.96f, 0.784f, 0.32f);

    col[ImGuiCol_Header] = C(0.239f, 0.96f, 0.784f, 0.14f);
    col[ImGuiCol_HeaderHovered] = C(0.239f, 0.96f, 0.784f, 0.22f);
    col[ImGuiCol_HeaderActive] = C(0.239f, 0.96f, 0.784f, 0.32f);

    col[ImGuiCol_Separator] = C(0.137f, 0.165f, 0.243f, 1.f);
    col[ImGuiCol_SeparatorHovered] = C(0.239f, 0.96f, 0.784f, 0.4f);
    col[ImGuiCol_SeparatorActive] = C(0.239f, 0.96f, 0.784f, 1.f);

    col[ImGuiCol_ResizeGrip] = C(0.f, 0.f, 0.f, 0.f);
    col[ImGuiCol_ResizeGripHovered] = C(0.f, 0.f, 0.f, 0.f);
    col[ImGuiCol_ResizeGripActive] = C(0.f, 0.f, 0.f, 0.f);

    col[ImGuiCol_Tab] = C(0.066f, 0.082f, 0.122f, 1.f);
    col[ImGuiCol_TabHovered] = C(0.239f, 0.96f, 0.784f, 0.2f);
    col[ImGuiCol_TabActive] = C(0.094f, 0.114f, 0.165f, 1.f);
    col[ImGuiCol_TabUnfocused] = col[ImGuiCol_Tab];
    col[ImGuiCol_TabUnfocusedActive] = col[ImGuiCol_TabActive];

    col[ImGuiCol_Text] = C(0.847f, 0.878f, 0.941f, 1.f);
    col[ImGuiCol_TextDisabled] = C(0.353f, 0.388f, 0.478f, 1.f);

    col[ImGuiCol_PlotLines] = C(0.239f, 0.96f, 0.784f, 1.f);
    col[ImGuiCol_PlotLinesHovered] = C(0.239f, 0.96f, 0.784f, 1.f);
    col[ImGuiCol_PlotHistogram] = C(0.239f, 0.96f, 0.784f, 1.f);
    col[ImGuiCol_PlotHistogramHovered] = C(0.102f, 0.561f, 0.451f, 1.f);

    col[ImGuiCol_TableHeaderBg] = C(0.066f, 0.082f, 0.122f, 1.f);
    col[ImGuiCol_TableBorderLight] = C(0.137f, 0.165f, 0.243f, 1.f);
    col[ImGuiCol_TableBorderStrong] = C(0.137f, 0.165f, 0.243f, 1.f);
    col[ImGuiCol_TableRowBg] = C(0.f, 0.f, 0.f, 0.f);
    col[ImGuiCol_TableRowBgAlt] = C(1.f, 1.f, 1.f, 0.03f);

    col[ImGuiCol_DragDropTarget] = C(0.239f, 0.96f, 0.784f, 0.8f);
    col[ImGuiCol_NavHighlight] = C(0.239f, 0.96f, 0.784f, 1.f);
    col[ImGuiCol_NavWindowingHighlight] = C(1.f, 1.f, 1.f, 0.7f);
    col[ImGuiCol_NavWindowingDimBg] = C(0.8f, 0.8f, 0.8f, 0.2f);
    col[ImGuiCol_ModalWindowDimBg] = C(0.0f, 0.0f, 0.0f, 0.55f);
}

static void SectionLabel(const char* label)
{
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.239f, 0.96f, 0.784f, 0.85f));
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
    ImGui::PushStyleColor(ImGuiCol_Separator, ImVec4(0.239f, 0.96f, 0.784f, 0.25f));
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
    if (lp.Num() == 0) 
        return;

    auto pc = lp[0]->PlayerController;

    if (!pc) 
        return;

    int len = MultiByteToWideChar(CP_UTF8, 0, cmd, -1, nullptr, 0);
    std::wstring ws(len - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, cmd, -1, ws.data(), len);
    FString fs(ws.c_str());

	UKismetSystemLibrary::ExecuteConsoleCommand(pc, fs);
}

void GUI_Init()
{
    FGUI::LoadHotkey();
    PushATLASStyle();
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

    ImGuiWindowFlags wflags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;

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
        dl->AddRectFilled(wp, ImVec2(wp.x + ws.x, wp.y + 3.f), IM_COL32(61, 245, 200, 255));
    }

    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 3.f);
    ImGui::SetCursorPosX(16.f);

    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.239f, 0.96f, 0.784f, 1.f));
    ImGui::Text("ATLAS");
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.353f, 0.388f, 0.478f, 1.f));
    ImGui::Text("| overlay");
    ImGui::PopStyleColor();

    ImGui::SameLine(ImGui::GetWindowWidth() - 80.f);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.353f, 0.388f, 0.478f, 0.8f));
    ImGui::Text("[%s]", VKName(FGUI::HotkeyVK));
    ImGui::PopStyleColor();

    ImGui::SetCursorPosX(0.f);
    ImGui::PushStyleColor(ImGuiCol_Separator, ImVec4(0.137f, 0.165f, 0.243f, 1.f));
    ImGui::Separator();
    ImGui::PopStyleColor();
    ImGui::SetCursorPosX(16.f);

    ImGui::BeginChild("##content", ImVec2(0.f, -1.f), false, ImGuiWindowFlags_NoScrollbar);

    ImGui::SetCursorPosX(0.f);

    SectionLabel("EDITING");

    ATCheckbox("Edit On Release", &FConfiguration::bEOREnabled);
    ImGui::SameLine(0.f, 16.f);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.353f, 0.388f, 0.478f, 1.f));
    ImGui::TextUnformatted("EOR");
    ImGui::PopStyleColor();

    ATCheckbox("Reset On Release", &FConfiguration::bROREnabled);
    ImGui::SameLine(0.f, 16.f);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.353f, 0.388f, 0.478f, 1.f));
    ImGui::TextUnformatted("ROR");
    ImGui::PopStyleColor();

    ATCheckbox("Disable Pre-Edits", &FConfiguration::bDisablePreEdits);

    SectionLabel("RESPAWNING");

    if (ATCheckbox("Respawns Enabled", &FConfiguration::bForceRespawns))
    {
        // Toggling respawns live requires re-running the playlist patching logic.
        // Call Client::Init() only for the playlist section, or set a flag that
        // ClientThread picks up.  For simplicity we expose a thin re-apply helper:
        // Client::ApplyRespawnSettings();   ← add this to Client.h / Client.cpp
    }

    bool RespawnTimeEnabled = (FConfiguration::RespawnTime > 0);
    bool RespawnHeightEnabled = (FConfiguration::RespawnHeight > 0);

    if (ATCheckbox("Custom Respawn Time", &RespawnTimeEnabled))
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

    if (ATCheckbox("Custom Respawn Height", &RespawnHeightEnabled))
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

    SectionLabel("CONSOLE");

    if (ATCheckbox("Console Enabled", &FGUI::bConsoleEnabled))
    {
        if (FGUI::bConsoleEnabled)
        {
            if (UEngine::GetEngine() && UEngine::GetEngine()->GameViewport)
                UEngine::GetEngine()->GameViewport->ViewportConsole = UGameplayStatics::SpawnObject(UEngine::GetEngine()->ConsoleClass, UEngine::GetEngine()->GameViewport);
        }
    }

    if (ATCheckbox("Potato Graphics", &FGUI::bPotatoGraphics))
        Exec(FGUI::bPotatoGraphics ? "r.MipMapLODBias 7" : "r.MipMapLODBias 0");

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
        ImGui::Text("%d°", FConfiguration::FOV);
        ImGui::PopStyleColor();
    }

    {
        const char* resItems[] = { "1920x1080", "1720x1080", "1280x720" };
        const char* resCmds[] = { "setres 1920x1080", "setres 1720x1080", "setres 1280x720" };

        ImGui::Text("Resolution");
        ImGui::SameLine();
        ImGui::PushItemWidth(140.f);

        if (ImGui::BeginCombo("##res", resItems[FGUI::Resolution]))
        {
            for (int i = 0; i < 3; i++)
            {
                bool selected = (FGUI::Resolution == i);

                if (ImGui::Selectable(resItems[i], selected))
                {
                    FGUI::Resolution = i;
                    Exec(resCmds[i]);
                }

                if (selected) 
                    ImGui::SetItemDefaultFocus();
            }

            ImGui::EndCombo();
        }

        ImGui::PopItemWidth();
    }

    SectionLabel("HOTKEYS");

    if (FGUI::bRebinding)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.239f, 0.96f, 0.784f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.239f, 0.96f, 0.784f, 0.18f));
        ImGui::Button("Press any key...", ImVec2(-1.f, 0.f));
        ImGui::PopStyleColor(2);
    }
    else
    {
        char btnLabel[64];
        snprintf(btnLabel, sizeof(btnLabel), "Rebind GUI Key  [%s]", VKName(FGUI::HotkeyVK));

        if (ImGui::Button(btnLabel, ImVec2(-1.f, 0.f)))
            FGUI::bRebinding = true;
    }

    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.353f, 0.388f, 0.478f, 1.f));
    ImGui::TextWrapped("Hotkey is saved automatically and will persist across sessions.");
    ImGui::PopStyleColor();

    ImGui::EndChild();
    ImGui::End();
}