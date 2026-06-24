#pragma once

#include "../../SDK/Engine.h"

struct FConfiguration
{
	static inline const char* ConsoleVersion = "v1.0.0";

	static inline bool bConsoleEnabled = true;
	static inline bool bForceRespawns = true;
	static inline bool bEnableIris = true;
	static inline bool bEOREnabled = false;
	static inline bool bROREnabled = false;
	static inline bool bPotatoGraphics = false;
	static inline bool bDisablePreEdits = false;
	static inline int RespawnTime = 3;
	static inline int RespawnHeight = 20000;
	static inline int FOV = 80;
	static inline int LODBias = 0;
};