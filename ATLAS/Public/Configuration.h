#pragma once

#include "../../SDK/Engine.h"
#include <atomic>

enum class EConsoleMode : int
{
	Atlas = 0,
	Unreal = 1
};

struct FConfiguration
{
	static inline const char* ConsoleVersion = "v1.2.10";

	static inline std::atomic_int ConsoleMode =
		static_cast<int>(EConsoleMode::Atlas);
	static inline std::atomic_bool bSprintByDefault = false;
	static inline bool bForceRespawns = true;
	static inline bool bEnableIris = true;
	static inline bool bEOREnabled = false;
	static inline bool bROREnabled = false;
	static inline bool bPotatoGraphics = false;
	static inline std::atomic_bool bDisablePreEdits = false;
	static inline int RespawnTime = 3;
	static inline int RespawnHeight = 20000;
	static inline int FOV = 80;
	static inline int LODBias = 0;
};
