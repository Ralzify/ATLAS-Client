#pragma once

// Periodically asks Unreal's configured OnlineHotfixManager to re-enumerate
// its title files. Accepted DefaultGame bytes gate one untouched native asset
// pass per changed revision; identical checks never walk table rows. This entry
// point must only be called from the established Unreal game-thread pump.
namespace SDK
{
	class UWorld;
}

namespace AtlasHotfixReload
{
	// Installs only the native INI observer and PatchAssets exact-once gate after
	// SDK initialization. Unsupported layouts retain stock behavior and do not
	// arm the added polling loop.
	bool InstallEarlyHook() noexcept;

	void GameThreadTick(const SDK::UWorld* world) noexcept;
}
