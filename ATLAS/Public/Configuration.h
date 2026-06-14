#pragma once

#include "../../SDK/Engine.h"

struct FConfiguration
{
	static inline bool bForceRespawns = false;
	static inline bool bEnableIris = false;
	static inline bool bEOREnabled = false;
	static inline bool bROREnabled = false;
	static inline bool bDisablePreEdits = false;
	static inline int RespawnTime = 3;
	static inline int RespawnHeight = 20000;
	static inline int FOV = 80;
};