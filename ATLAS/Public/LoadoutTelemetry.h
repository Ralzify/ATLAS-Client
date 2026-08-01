#pragma once

#include <cstdint>

namespace SDK
{
	class UWorld;
}

class AFortPlayerControllerAthena;

// Optional, negotiated exact-slot telemetry for Magnesium.  ATLAS never
// imports or links against the gameserver: an unmodified/unsupported server
// simply never acknowledges the hello and this component remains inert.
namespace AtlasLoadoutTelemetry
{
	void SetEnabled(bool enabled) noexcept;
	bool WantsClientMessageHook() noexcept;

	// Called from ATLAS's existing ClientMessage native thunk. Returns true
	// only for the exact acknowledgement belonging to the current session; the
	// caller suppresses that one internal marker and forwards every other line.
	bool HandleClientMessage(const wchar_t* text, int length) noexcept;

	// Must only be called by ATLAS's existing client game-thread pump.
	void GameThreadTick(
		SDK::UWorld* world,
		AFortPlayerControllerAthena* controller,
		bool clientMessageHookReady) noexcept;
}
