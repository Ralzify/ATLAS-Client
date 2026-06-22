#pragma once

#include "../../SDK/Engine.h"
#include "../Public/Configuration.h"
#include <string>

void GUI_Init();
void GUI_Render();
void GUI_HandleInput();
void GUI_LoadTextures(ID3D11Device* device);

struct FGUI
{
    static inline bool bVisible = false;

    static inline int HotkeyVK = VK_F9;
    static inline bool bRebinding = false;

    // static inline int Resolution = 0;

    static inline ID3D11ShaderResourceView* LogoTexture = nullptr;
    static inline int LogoW = 0, LogoH = 0;

    static inline int HostType = 0; // 0 = Local Host, 1 = Remote Host
    static inline char RemoteIP[64] = "";

    static inline int JoinHotkeyVK = VK_F5;
    static inline bool bRebindingJoin = false;

    static void SaveHotkey();
    static void LoadHotkey();

    static void SaveJoinHotkey();
    static void LoadJoinHotkey();
};