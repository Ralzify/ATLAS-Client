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
	static inline const char* ConsoleVersion = "v2.1.0";

	static inline std::atomic_int ConsoleMode =
		static_cast<int>(EConsoleMode::Atlas);
	static inline std::atomic_bool bSaveTogglePreferences = true;
	static inline std::atomic_bool bSlashKeyEnabled = true;
	static inline std::atomic_bool bSprintByDefault = true;
	static inline std::atomic_bool bForceRespawns = true;
	static inline bool bEnableIris = true;
	static inline std::atomic_bool bEOREnabled = true;
	static inline std::atomic_bool bROREnabled = true;
	static inline std::atomic_bool bMobileBuilds = false;
	static inline std::atomic_bool bGameplayFog = true;
	static inline std::atomic_bool bDebugCamera = false;
	static inline std::atomic_bool bTargetInfo = false;
	static inline std::atomic_bool bDisablePreEdits = true;
	static inline int RespawnTime = 3;
	static inline int RespawnHeight = 20000;
	static inline std::atomic_int FOV = 80;
	static inline std::atomic_int MipMapLODBias = 0;
};
