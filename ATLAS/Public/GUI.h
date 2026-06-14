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

    static inline bool bPotatoGraphics = false;
    static inline int  Resolution = 0;

    static inline ID3D11ShaderResourceView* LogoTexture = nullptr;
    static inline int LogoW = 0, LogoH = 0;

    static void SaveHotkey();
    static void LoadHotkey();
};