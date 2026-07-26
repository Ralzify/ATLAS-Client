#include "pch.h"
#include "../Public/FortPlaylistAthena.h"
#include "../Public/Utils.h"
#include "../Public/FortPlayerControllerAthena.h"
#include "../Public/Client.h"
#include "../Public/Configuration.h"
#include "../Public/Diagnostics.h"
#include "../Public/GUI.h"

#include <atomic>
#include <limits>

inline void* (*SelectResetOG)(void*) = nullptr;
inline void* (*SelectEditOG)(void*) = nullptr;
inline void (*PerformBuildingEditInteractionOG)(void*) = nullptr;
inline void (*CompleteBuildingEditInteraction)(void*) = nullptr;

using ClientMessageNative = void(*)(UObject*, FFrame&, void*);
static std::atomic<ClientMessageNative> g_ClientMessageOriginal = nullptr;
static std::atomic<UFunction*> g_ClientMessageFunction = nullptr;
static std::atomic_bool g_ClientMessageCaptureInstalled = false;
static std::atomic_bool g_ClientMessageCaptureEnabled = true;
static std::atomic_bool g_ServerCommandListReceived = false;
static std::atomic_uint64_t g_ConsoleSessionGeneration = 0;
static std::atomic_uint32_t g_ClientMessageTextOffset = UINT32_MAX;

struct FClientMessageView
{
	const wchar_t* Data;
	int32_t NumElements;
	int32_t MaxElements;
};

static bool IsReadableRange(const void* address, size_t length)
{
	if (!address || length == 0)
		return false;

	const uintptr_t begin = reinterpret_cast<uintptr_t>(address);
	if (length > (std::numeric_limits<uintptr_t>::max)() - begin)
		return false;

	const uintptr_t end = begin + length;
	uintptr_t current = begin;
	while (current < end)
	{
		MEMORY_BASIC_INFORMATION region{};
		if (VirtualQuery(reinterpret_cast<const void*>(current), &region, sizeof(region)) != sizeof(region) ||
			region.State != MEM_COMMIT ||
			(region.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0 ||
			region.RegionSize == 0)
		{
			return false;
		}

		const uintptr_t regionBegin = reinterpret_cast<uintptr_t>(region.BaseAddress);
		if (region.RegionSize > (std::numeric_limits<uintptr_t>::max)() - regionBegin)
			return false;

		const uintptr_t regionEnd = regionBegin + region.RegionSize;
		if (regionEnd <= current)
			return false;

		current = (std::min)(end, regionEnd);
	}

	return true;
}

static bool IsDirectClientMessageFrame(const FFrame* stack, const UFunction* function)
{
	__try
	{
		return stack && stack->Node == function && stack->Code == nullptr && stack->Locals;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return false;
	}
}

static bool TryReadClientMessageView(const FString* message, FClientMessageView* view)
{
	if (!view || !IsReadableRange(message, sizeof(FString)))
		return false;

	__try
	{
		view->Data = message->Data;
		view->NumElements = message->NumElements;
		view->MaxElements = message->MaxElements;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return false;
	}

	return view->Data &&
		view->NumElements > 0 &&
		view->NumElements <= 16384 &&
		view->MaxElements >= view->NumElements &&
		view->MaxElements <= 65536 &&
		IsReadableRange(view->Data, static_cast<size_t>(view->NumElements) * sizeof(wchar_t));
}

static bool TryCopyClientMessage(const wchar_t* source, wchar_t* destination, size_t length)
{
	__try
	{
		memcpy(destination, source, length * sizeof(wchar_t));
		return true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return false;
	}
}

static bool LooksLikeServerCommandList(
	const wchar_t* text, int textLength)
{
	if (!text || textLength <= 0)
		return false;

	static constexpr wchar_t commandPrefix[] = L"cheat ";
	static constexpr int prefixLength =
		static_cast<int>(_countof(commandPrefix) - 1);
	for (int start = 0;
		start <= textLength - prefixLength;
		start++)
	{
		const bool lineStart =
			start == 0 ||
			text[start - 1] == L'\n' ||
			text[start - 1] == L'\r';
		if (lineStart &&
			wmemcmp(text + start, commandPrefix,
				static_cast<size_t>(prefixLength)) == 0)
		{
			// The game server sends its help entries as separate
			// ClientMessage calls on some versions. One well-formed command
			// line is enough to prove that discovery has started.
			return true;
		}
	}

	return false;
}

static void CaptureClientMessage(UObject* context, FFrame& stack, void* result)
{
	const UFunction* function = g_ClientMessageFunction.load(std::memory_order_acquire);
	const uint32_t textOffset = g_ClientMessageTextOffset.load(std::memory_order_acquire);
	if (function && textOffset != UINT32_MAX && IsDirectClientMessageFrame(&stack, function))
	{
		const FString* message = reinterpret_cast<const FString*>(stack.Locals + textOffset);
		FClientMessageView view{};
		if (TryReadClientMessageView(message, &view))
		{
			thread_local wchar_t messageCopy[16384];
			int length = view.NumElements;
			const bool copied = TryCopyClientMessage(
				view.Data, messageCopy, static_cast<size_t>(length));
			if (copied && length > 0 && messageCopy[length - 1] == L'\0')
			{
				length--;
			}
			if (copied && length > 0)
			{
				const bool serverCommandList =
					LooksLikeServerCommandList(
						messageCopy, length);
				if (serverCommandList)
				{
					g_ServerCommandListReceived.store(
						true, std::memory_order_release);
				}
				GUI_QueueConsoleOutput(
					messageCopy, length);
			}
		}
	}

	if (ClientMessageNative original = g_ClientMessageOriginal.load(std::memory_order_acquire))
		original(context, stack, result);
}

static bool TryInstallClientMessageCapture(UObject* playerController)
{
	if (!g_ClientMessageCaptureEnabled.load(std::memory_order_acquire))
		return true;

	if (g_ClientMessageCaptureInstalled.load(std::memory_order_acquire) || !playerController)
		return true;

	UFunction* function = playerController->GetFunction("ClientMessage");
	if (!function)
		return false;

	const auto params = function->GetParamsNamed();
	uint32_t textOffset = UINT32_MAX;
	for (const auto& param : params.NameOffsetMap)
	{
		if ((param.Name == "S" || param.Name == "Message") &&
			param.ElementSize == sizeof(FString) &&
			(param.PropertyFlags & 0x80) != 0 &&
			param.Offset <= params.Size &&
			param.ElementSize <= params.Size - param.Offset)
		{
			textOffset = param.Offset;
			break;
		}
	}

	if (textOffset == UINT32_MAX)
	{
		AtlasDiagnostics::WriteLine("console-capture-skip invalid-clientmessage-layout size=%u", params.Size);
		return false;
	}

	void*& nativeFunction = function->GetNativeFunc();
	auto nativeSlot = reinterpret_cast<PVOID volatile*>(&nativeFunction);
	void* observed = InterlockedCompareExchangePointer(nativeSlot, nullptr, nullptr);
	if (!observed || observed == reinterpret_cast<void*>(CaptureClientMessage))
		return false;

	g_ClientMessageTextOffset.store(textOffset, std::memory_order_release);
	g_ClientMessageFunction.store(function, std::memory_order_release);
	g_ClientMessageOriginal.store(reinterpret_cast<ClientMessageNative>(observed), std::memory_order_release);

	void* previous = InterlockedCompareExchangePointer(
		nativeSlot, reinterpret_cast<void*>(CaptureClientMessage), observed);
	if (previous != observed)
	{
		g_ClientMessageOriginal.store(nullptr, std::memory_order_release);
		g_ClientMessageFunction.store(nullptr, std::memory_order_release);
		g_ClientMessageTextOffset.store(UINT32_MAX, std::memory_order_release);
		AtlasDiagnostics::WriteLine("console-capture-skip hook-failed function=%p", function);
		return false;
	}

	g_ClientMessageCaptureInstalled.store(true, std::memory_order_release);
	AtlasDiagnostics::WriteLine("console-capture-installed function=%p text-offset=%u", function, textOffset);
	return true;
}

static AFortPlayerControllerAthena* GetLocalFortPlayerController()
{
	auto World = UWorld::GetWorld();
	if (!World || !World->OwningGameInstance)
		return nullptr;

	auto& LocalPlayers = World->OwningGameInstance->LocalPlayers;
	if (LocalPlayers.Num() <= 0 || !LocalPlayers[0])
		return nullptr;

	return (AFortPlayerControllerAthena*)LocalPlayers[0]->PlayerController;
}

static bool IsLiveUObject(const UObject* object)
{
	if (!object)
		return false;

	__try
	{
		const int index = object->Index;
		if (index < 0 || index >= TUObjectArray::Num())
			return false;

		const FUObjectItem* item = TUObjectArray::GetItemByIndex(index);
		return item && item->Object == object && (item->Flags & 0x20) == 0;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return false;
	}
}

static void SafeCompleteBuildingEditInteraction()
{
	if (!CompleteBuildingEditInteraction)
		return;

	auto PlayerController = GetLocalFortPlayerController();
	if (!PlayerController)
		return;

	__try
	{
		CompleteBuildingEditInteraction(PlayerController);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		CompleteBuildingEditInteraction = nullptr;
	}
}

static uintptr_t FindTextLeaTargetNear(uintptr_t Center, int Before, int After)
{
	auto TextSect = Memcury::PE::Section::GetSection(".text");

	auto TryInstruction = [&](uintptr_t Addr) -> uintptr_t
	{
		if (!TextSect.isInSection(Addr))
			return 0;

		const uint8_t Prefix = *(uint8_t*)Addr;
		const uint8_t Op = *(uint8_t*)(Addr + 1);
		const uint8_t ModRM = *(uint8_t*)(Addr + 2);

		if ((Prefix == 0x48 || Prefix == 0x4C) && Op == 0x8D && (ModRM & 0xC7) == 0x05)
		{
			auto Target = Memcury::Scanner(Addr).RelativeOffset(3).Get();
			if (TextSect.isInSection(Target))
				return Target;
		}

		return 0;
	};

	for (int i = 1; i <= Before; i++)
	{
		if (auto Target = TryInstruction(Center - i))
			return Target;
	}

	for (int i = 0; i <= After; i++)
	{
		if (auto Target = TryInstruction(Center + i))
			return Target;
	}

	return 0;
}

static uintptr_t FindCompleteBuildingEditInteraction()
{
	for (int RefIndex = 0; RefIndex < 4; RefIndex++)
	{
		auto StringRef = Memcury::Scanner::FindStringRef("CompleteBuildingEditInteraction", false, RefIndex).Get();
		if (!StringRef)
			break;

		if (auto Target = FindTextLeaTargetNear(StringRef, 2000, 256))
			return Target;
	}

	return 0;
}

void* SelectEdit(void* a1)
{
	void* result = SelectEditOG(a1);

	if (FConfiguration::bEOREnabled)
		SafeCompleteBuildingEditInteraction();

	return result;
}

void* SelectReset(void* a1)
{
	void* result = SelectResetOG(a1);

	if (FConfiguration::bROREnabled)
		SafeCompleteBuildingEditInteraction();

	return result;
}

void PerformBuildingEditInteraction(AFortPlayerControllerAthena* _this)
{
	if (FConfiguration::bDisablePreEdits && _this && _this->TargetedBuilding && _this->TargetedBuilding->IsA<ABuildingPlayerPrimitivePreview>())
		return;

	return PerformBuildingEditInteractionOG(_this);
}

static std::atomic_bool g_ClientUObjectInitializationPending = true;
static bool g_ViewportConsoleInitializationComplete = false;
static bool g_PlaylistInitializationComplete = false;
static const UClass* g_ClientPlaylistClass = nullptr;
static int g_PlaylistScanIndex = 0;
static int g_PlaylistMatchCount = 0;
static bool g_PlaylistExtensionsInitialized = false;
static TArray<FUIExtension> g_ArenaExtensions;
static TArray<FUIExtension> g_ShowdownExtensions;
static std::atomic_bool g_ClientUObjectInitializationRunning = false;
static ULONGLONG g_NextViewportConsoleAttemptAt = 0;
static ULONGLONG g_NextPlaylistClassResolveAt = 0;
static ULONGLONG g_NextPlaylistScanRetryAt = 0;

static void RearmClientPlaylistInitialization()
{
	g_PlaylistInitializationComplete = false;
	g_PlaylistScanIndex = 0;
	g_PlaylistMatchCount = 0;
	g_NextPlaylistScanRetryAt = 0;
	g_ClientUObjectInitializationPending.store(
		true, std::memory_order_release);
}

static void InitializePlaylistExtensions()
{
	if (g_PlaylistExtensionsInitialized ||
		VersionInfo.FortniteVersion < 10)
	{
		return;
	}

	auto PrimarySlot = uint8_t(
		EPlaylistUIExtensionSlot::StaticEnum()
			? EPlaylistUIExtensionSlot::GetPrimary()
			: EUIExtensionSlot::GetPrimary());

	FUIExtension ArenaUIExtension{};
	ArenaUIExtension.Slot = PrimarySlot;
	if (VersionInfo.FortniteVersion < 23)
	{
		ArenaUIExtension.WidgetClass.ObjectID.AssetPathName =
			FName(L"/Game/UI/Competitive/Arena/ArenaScoringHUD.ArenaScoringHUD_C");
	}
	else
	{
		auto& PackageName = *(FName*)(
			__int64(&ArenaUIExtension.WidgetClass) +
			(VersionInfo.EngineVersion < 5.3 ? 0xC : 0x8));
		auto& AssetName = *(FName*)(
			__int64(&ArenaUIExtension.WidgetClass) +
			(VersionInfo.EngineVersion < 5.3 ? 0x10 : 0xC));
		auto& SubPathString = *(FString*)(
			__int64(&ArenaUIExtension.WidgetClass) +
			(VersionInfo.EngineVersion < 5.3 ? 0x14 : 0x10));

		PackageName =
			FName(L"/Game/UI/Competitive/Arena/ArenaScoringHUD");
		AssetName = FName(L"ArenaScoringHUD_C");
		SubPathString = FString();
	}

	FUIExtension ShowdownUIExtension{};
	ShowdownUIExtension.Slot = PrimarySlot;
	if (VersionInfo.FortniteVersion < 23)
	{
		ShowdownUIExtension.WidgetClass.ObjectID.AssetPathName =
			FName(L"/Game/UI/Frontend/Showdown/ShowdownScoringHUD.ShowdownScoringHUD_C");
	}
	else
	{
		auto& PackageName = *(FName*)(
			__int64(&ShowdownUIExtension.WidgetClass) +
			(VersionInfo.EngineVersion < 5.3 ? 0xC : 0x8));
		auto& AssetName = *(FName*)(
			__int64(&ShowdownUIExtension.WidgetClass) +
			(VersionInfo.EngineVersion < 5.3 ? 0x10 : 0xC));
		auto& SubPathString = *(FString*)(
			__int64(&ShowdownUIExtension.WidgetClass) +
			(VersionInfo.EngineVersion < 5.3 ? 0x14 : 0x10));

		PackageName =
			FName(L"/Game/UI/Frontend/Showdown/ShowdownScoringHUD");
		AssetName = FName(L"ShowdownScoringHUD_C");
		SubPathString = FString();
	}

	g_ArenaExtensions.Add(ArenaUIExtension);
	g_ShowdownExtensions.Add(ShowdownUIExtension);
	g_PlaylistExtensionsInitialized = true;
}

static void ApplyClientPlaylistConfiguration(
	const UObject* object)
{
	// TUObjectArray exposes read-only pointers, but these playlist defaults
	// are the intentional mutation target of this initialization pass.
	auto mutableObject = const_cast<UObject*>(object);
	auto Playlist =
		static_cast<UFortPlaylistAthena*>(mutableObject);
	if (FConfiguration::bForceRespawns)
	{
		if (Playlist->HasbRespawnInAir())
			Playlist->bRespawnInAir = true;
		if (Playlist->HasRespawnHeight())
		{
			Playlist->RespawnHeight.Curve.CurveTable = nullptr;
			Playlist->RespawnHeight.Curve.RowName = FName();
			Playlist->RespawnHeight.Value =
				(float)FConfiguration::RespawnHeight;
		}
		if (Playlist->HasRespawnTime())
		{
			Playlist->RespawnTime.Curve.CurveTable = nullptr;
			Playlist->RespawnTime.Curve.RowName = FName();
			Playlist->RespawnTime.Value =
				(float)FConfiguration::RespawnTime;
		}
		Playlist->RespawnType = 1;
		if (Playlist->HasbAllowJoinInProgress())
			Playlist->bAllowJoinInProgress = true;
		if (Playlist->HasbForceRespawnLocationInsideOfVolume())
			Playlist->bForceRespawnLocationInsideOfVolume = true;
	}

	if (VersionInfo.FortniteVersion >= 10)
	{
		auto Name = mutableObject->Name.ToString();
		if (Name.contains("Showdown"))
		{
			Playlist->UIExtensions =
				Name.contains("ShowdownAlt")
					? g_ArenaExtensions
					: g_ShowdownExtensions;
		}
	}
}

static void InitializeClientUObjects()
{
	if (!g_ClientUObjectInitializationPending.load(
		std::memory_order_acquire))
		return;

	bool expected = false;
	if (!g_ClientUObjectInitializationRunning.compare_exchange_strong(
		expected, true, std::memory_order_acq_rel,
		std::memory_order_acquire))
	{
		return;
	}

	const ULONGLONG now = GetTickCount64();
	if (!g_ViewportConsoleInitializationComplete)
	{
		if (!g_ClientMessageCaptureEnabled.load(
			std::memory_order_acquire))
		{
			g_ViewportConsoleInitializationComplete = true;
		}
		else if (now >= g_NextViewportConsoleAttemptAt)
		{
			g_NextViewportConsoleAttemptAt = now + 1000;
			auto engine = UEngine::GetEngine();
			if (engine && engine->GameViewport &&
				engine->ConsoleClass)
			{
				if (!engine->GameViewport->ViewportConsole)
				{
					// Keep Unreal's console object alive for the entire
					// session. ATLAS mode suppresses only its native key at
					// the window boundary, so switching back to Original UE
					// Console is immediate.
					engine->GameViewport->ViewportConsole =
						UGameplayStatics::SpawnObject(
							engine->ConsoleClass,
							engine->GameViewport);
				}

				g_ViewportConsoleInitializationComplete =
					engine->GameViewport->ViewportConsole != nullptr;
			}
		}
	}

	if (!g_PlaylistInitializationComplete)
	{
		if (VersionInfo.FortniteVersion < 10 &&
			!FConfiguration::bForceRespawns)
		{
			g_PlaylistInitializationComplete = true;
		}
		else
		{
			if (!g_ClientPlaylistClass &&
				now >= g_NextPlaylistClassResolveAt)
			{
				g_NextPlaylistClassResolveAt = now + 1000;
				g_ClientPlaylistClass =
					FindClass("FortPlaylistAthena");
			}
			if (g_ClientPlaylistClass &&
				now >= g_NextPlaylistScanRetryAt)
			{
				InitializePlaylistExtensions();

				// The global object array can contain hundreds of thousands
				// of entries. Scan it in bounded slices so initialization
				// cannot stall one game-thread frame.
				constexpr int kPlaylistObjectsPerTick = 2048;
				const int objectCount = TUObjectArray::Num();
				const int end = (std::min)(
					g_PlaylistScanIndex +
						kPlaylistObjectsPerTick,
					objectCount);
				for (int i = g_PlaylistScanIndex;
					i < end; i++)
				{
					const FUObjectItem* item =
						TUObjectArray::GetItemByIndex(i);
					const UObject* object =
						item ? item->Object : nullptr;
					if (object && (item->Flags & 0x20) == 0 &&
						object->IsA(g_ClientPlaylistClass))
					{
						g_PlaylistMatchCount++;
						ApplyClientPlaylistConfiguration(
							object);
					}
				}

				g_PlaylistScanIndex = end;
				if (g_PlaylistScanIndex >=
					TUObjectArray::Num())
				{
					if (g_PlaylistMatchCount > 0)
					{
						g_PlaylistInitializationComplete = true;
					}
					else
					{
						// The class can exist before any playlist defaults
						// are loaded. Retry at a low cadence instead of
						// permanently completing an empty scan.
						g_PlaylistScanIndex = 0;
						g_NextPlaylistScanRetryAt =
							now + 1000;
					}
				}
			}
		}
	}

	if (g_ViewportConsoleInitializationComplete &&
		g_PlaylistInitializationComplete)
	{
		g_ClientUObjectInitializationPending.store(
			false, std::memory_order_release);
		AtlasDiagnostics::WriteLine(
			"client-uobject-initialization complete "
			"playlist-objects=%d",
			g_PlaylistScanIndex);
	}

	g_ClientUObjectInitializationRunning.store(
		false, std::memory_order_release);
}

static void ClientGameThreadTick()
{
	InitializeClientUObjects();

	static UWorld* observedSessionWorld = nullptr;
	static AFortPlayerControllerAthena*
		observedSessionController = nullptr;
	static AFortPlayerControllerAthena* observedController = nullptr;
	static UFortCheatManager* observedManager = nullptr;
	static ULONGLONG nextCaptureResolveAt = 0;

	auto world = UWorld::GetWorld();
	auto playerController = GetLocalFortPlayerController();
	if (world != observedSessionWorld)
	{
		observedSessionWorld = world;
		observedSessionController = nullptr;
		g_ServerCommandListReceived.store(
			false, std::memory_order_release);
		RearmClientPlaylistInitialization();
	}

	if (!playerController)
	{
		observedSessionController = nullptr;
	}
	else if (playerController != observedSessionController)
	{
		observedSessionController = playerController;
		g_ServerCommandListReceived.store(
			false, std::memory_order_release);
		const uint64_t session =
			g_ConsoleSessionGeneration.fetch_add(
				1, std::memory_order_acq_rel) + 1;
		AtlasDiagnostics::WriteLine(
			"console-session changed generation=%llu "
			"world=%p pc=%p",
			static_cast<unsigned long long>(session),
			world, playerController);
	}

	if (playerController)
	{
		const ULONGLONG now = GetTickCount64();
		if (g_ClientMessageCaptureEnabled.load(std::memory_order_acquire) &&
			!g_ClientMessageCaptureInstalled.load(std::memory_order_acquire) &&
			now >= nextCaptureResolveAt)
		{
			TryInstallClientMessageCapture(playerController);
			nextCaptureResolveAt = now + 1000;
		}

		auto cheatManager = playerController->CheatManager;
		if (playerController != observedController || cheatManager != observedManager)
		{
			AtlasDiagnostics::WriteLine("manager-observed pc=%p manager=%p", playerController, cheatManager);
			observedController = playerController;
			observedManager = cheatManager;
		}

		if (cheatManager && !IsLiveUObject(cheatManager))
		{
			AtlasDiagnostics::WriteLine("manager-stale pc=%p manager=%p", playerController, cheatManager);
			playerController->CheatManager = nullptr;
			cheatManager = nullptr;
			observedManager = nullptr;
		}

		if (!cheatManager)
		{
			auto cheatClass = playerController->CheatClass.Get();
			if (cheatClass)
			{
				cheatManager = (UFortCheatManager*)UGameplayStatics::SpawnObject(cheatClass, playerController);
				if (cheatManager)
				{
					// Publish the reference while the async flags still protect the
					// new object, then make it a normal GC-managed UObject.
					playerController->CheatManager = cheatManager;
					cheatManager->ObjectFlags &= ~0x1000000;
					if (auto item = TUObjectArray::GetItemByIndex(cheatManager->Index))
						item->Flags &= ~0x4000000;
					observedManager = cheatManager;
					AtlasDiagnostics::WriteLine("manager-created pc=%p manager=%p index=%d", playerController, cheatManager, cheatManager->Index);
				}
				else
				{
					AtlasDiagnostics::WriteLine("manager-create-failed pc=%p class=%p", playerController, cheatClass);
				}
			}
		}
	}
	else if (observedController)
	{
		AtlasDiagnostics::WriteLine("controller-unavailable previous-pc=%p manager=%p", observedController, observedManager);
		observedController = nullptr;
		observedManager = nullptr;
	}
}

static DWORD WINAPI ClientThread(LPVOID)
{
	while (true)
	{
		// Compatibility fallback used only when the game-thread tick hook
		// cannot be installed. Command execution remains disabled rather than
		// calling Unreal from this worker thread.
		ClientGameThreadTick();
		Sleep(33);
	}

	return 0;
}

using GetMaxTickRateNative =
	float(*)(UEngine*, float, bool);
static GetMaxTickRateNative g_GetMaxTickRateOriginal = nullptr;
static std::atomic_bool g_GameThreadPumpInstalled = false;

static float HookedGetMaxTickRate(
	UEngine* engine,
	float deltaTime,
	bool allowFrameRateSmoothing)
{
	static std::atomic<DWORD> gameThreadId = 0;
	static std::atomic_bool loggedThreadMigration = false;
	static ULONGLONG nextClientTickAt = 0;

	const DWORD currentThreadId = GetCurrentThreadId();
	DWORD expectedThreadId = 0;
	if (gameThreadId.compare_exchange_strong(
		expectedThreadId, currentThreadId,
		std::memory_order_acq_rel,
		std::memory_order_acquire))
	{
		GUI_SetGameThreadDispatcherReady(true);
		AtlasDiagnostics::WriteLine(
			"game-thread-pump active tid=%lu",
			currentThreadId);
	}
	else if (expectedThreadId != currentThreadId)
	{
		if (!loggedThreadMigration.exchange(
			true, std::memory_order_acq_rel))
		{
			AtlasDiagnostics::WriteLine(
				"game-thread-pump ignored migrated-call "
				"owner-tid=%lu call-tid=%lu",
				expectedThreadId, currentThreadId);
		}
		return g_GetMaxTickRateOriginal(
			engine, deltaTime,
			allowFrameRateSmoothing);
	}

	const float result = g_GetMaxTickRateOriginal(
		engine, deltaTime, allowFrameRateSmoothing);

	const ULONGLONG now = GetTickCount64();
	if (now >= nextClientTickAt)
	{
		nextClientTickAt = now + 33;
		ClientGameThreadTick();
	}

	GUI_PumpGameThreadCommands();
	return result;
}

static uint64 ResolveGetMaxTickRateTarget()
{
	// The generic UE5 signatures miss Fortnite 32.11 because its prologue
	// uses different REX registers. This RVA is verified for that exact build;
	// every other version must continue to use its signature result.
	if (VersionInfo.FortniteVersion == 32.11)
		return Memcury::PE::GetModuleBase() + 0x189FE98;

	return FindGetMaxTickRate();
}

static bool IsExecutableHookTarget(uint64 target)
{
	if (!target)
		return false;

	MEMORY_BASIC_INFORMATION region{};
	if (VirtualQuery(
		reinterpret_cast<void*>(target),
		&region, sizeof(region)) != sizeof(region))
	{
		return false;
	}

	const DWORD executableProtection =
		PAGE_EXECUTE | PAGE_EXECUTE_READ |
		PAGE_EXECUTE_READWRITE |
		PAGE_EXECUTE_WRITECOPY;
	return region.State == MEM_COMMIT &&
		(region.Protect &
			(PAGE_GUARD | PAGE_NOACCESS)) == 0 &&
		(region.Protect & executableProtection) != 0;
}

static bool InstallGameThreadPump()
{
	if (g_GameThreadPumpInstalled.load(
		std::memory_order_acquire))
	{
		return true;
	}

	GUI_SetGameThreadDispatcherReady(false);

	const uint64 target = ResolveGetMaxTickRateTarget();
	if (!IsExecutableHookTarget(target))
	{
		AtlasDiagnostics::WriteLine(
			"game-thread-pump install-failed invalid-target=%p",
			reinterpret_cast<void*>(target));
		return false;
	}

	const MH_STATUS initializeStatus = MH_Initialize();
	if (initializeStatus != MH_OK &&
		initializeStatus != MH_ERROR_ALREADY_INITIALIZED)
	{
		AtlasDiagnostics::WriteLine(
			"game-thread-pump install-failed initialize-status=%d",
			static_cast<int>(initializeStatus));
		return false;
	}

	const MH_STATUS createStatus = MH_CreateHook(
		reinterpret_cast<LPVOID>(target),
		reinterpret_cast<LPVOID>(HookedGetMaxTickRate),
		reinterpret_cast<LPVOID*>(&g_GetMaxTickRateOriginal));
	if (createStatus != MH_OK || !g_GetMaxTickRateOriginal)
	{
		AtlasDiagnostics::WriteLine(
			"game-thread-pump install-failed create-status=%d target=%p",
			static_cast<int>(createStatus),
			reinterpret_cast<void*>(target));
		return false;
	}

	const MH_STATUS enableStatus = MH_EnableHook(
		reinterpret_cast<LPVOID>(target));
	if (enableStatus != MH_OK &&
		enableStatus != MH_ERROR_ENABLED)
	{
		MH_RemoveHook(reinterpret_cast<LPVOID>(target));
		g_GetMaxTickRateOriginal = nullptr;
		AtlasDiagnostics::WriteLine(
			"game-thread-pump install-failed enable-status=%d target=%p",
			static_cast<int>(enableStatus),
			reinterpret_cast<void*>(target));
		return false;
	}

	g_GameThreadPumpInstalled.store(
		true, std::memory_order_release);
	AtlasDiagnostics::WriteLine(
		"game-thread-pump installed target=%p awaiting-first-tick",
		reinterpret_cast<void*>(target));
	return true;
}

void Client::Init()
{
	if (VersionInfo.EngineVersion < 4.24)
		FConfiguration::bEOREnabled = true;
	if (VersionInfo.FortniteVersion < 24.30)
		FConfiguration::bROREnabled = true;
	//if (VersionInfo.FortniteVersion < 15.20)
		//FConfiguration::bDisablePreEdits = true;

	if (VersionInfo.FortniteVersion < 24.30)
	{
		auto CompRef = Memcury::Scanner::FindStringRef(L"EditModeInputComponent0").Get();
		uintptr_t SelectEditAddr = 0, SelectResetAddr = 0, PerformBuildingEditInteractionAddr = 0;

		if (CompRef)
		{
			int Skip = 0;
			for (int i = 1; i < 2000; i++)
			{
				if (*(uint8_t*)(CompRef + i) == 0x48 && *(uint8_t*)(CompRef + i + 1) == 0x8D && *(uint8_t*)(CompRef + i + 2) == 0x05)
				{
					if (Skip == 1)
						SelectEditAddr = Memcury::Scanner(CompRef + i).RelativeOffset(3).Get();
					else if (Skip == 2)
					{
						SelectResetAddr = Memcury::Scanner(CompRef + i).RelativeOffset(3).Get();
						break;
					}

					Skip++;
				}
			}
		}
		
		auto rdataSect = Memcury::PE::Section::GetSection(".rdata");
		if (CompRef)
		{
			for (int i = 1; i < 0x5000; i++)
			{
				if ((*(uint8_t*)(CompRef - i) == 0x48 || *(uint8_t*)(CompRef - i) == 0x4C) && *(uint8_t*)(CompRef - i + 1) == 0x8D)
				{
					auto stringAddr = Memcury::Scanner(CompRef - i).RelativeOffset(3).Get();

					if (rdataSect.isInSection(stringAddr))
					{
						auto str = (char*)stringAddr;

						if (strcmp(str, "PerformBuildingEditInteraction") == 0)
						{
							for (int x = 1; x < 2000; x++)
							{
								if (*(uint8_t*)(CompRef - i - x) == 0x48 && *(uint8_t*)(CompRef - i - x + 1) == 0x8D && *(uint8_t*)(CompRef - i - x + 2) == 0x05)
								{
									PerformBuildingEditInteractionAddr = Memcury::Scanner(CompRef - i - x).RelativeOffset(3).Get();
									break;
								}
							}
							break;
						}
					}
				}
			}
		}

		if (auto CompleteBuildingEditInteractionAddr = FindCompleteBuildingEditInteraction())
			CompleteBuildingEditInteraction = (void (*)(void*))CompleteBuildingEditInteractionAddr;

		if (MH_Initialize() == MH_ERROR_ALREADY_INITIALIZED) 
		{ 
			/* already set up, skip */ 
		}

		if (VersionInfo.FortniteVersion < 11 && SelectEditAddr && CompleteBuildingEditInteraction)
			Utils::Hook(SelectEditAddr, SelectEdit, SelectEditOG);
		if (VersionInfo.FortniteVersion < 15.20 && PerformBuildingEditInteractionAddr)
			Utils::Hook(PerformBuildingEditInteractionAddr, PerformBuildingEditInteraction, PerformBuildingEditInteractionOG);
		if (VersionInfo.FortniteVersion < 24.30 && SelectResetAddr && CompleteBuildingEditInteraction)
			Utils::Hook(SelectResetAddr, SelectReset, SelectResetOG);

		MH_EnableHook(MH_ALL_HOOKS);
	}

	const bool interactiveClient =
		g_ClientMessageCaptureEnabled.load(
			std::memory_order_acquire);
	if (interactiveClient)
	{
		// Fail closed on an unsupported client build. Running UObject
		// initialization or cheat-manager maintenance from a Win32 worker
		// recreates the world-travel/GC race this dispatcher removes.
		InstallGameThreadPump();
	}
	else
	{
		// Preserve the existing headless-host maintenance path. Interactive
		// clients never use this worker fallback.
		if (HANDLE thread = CreateThread(nullptr, 0, ClientThread, nullptr, 0, nullptr))
			CloseHandle(thread);
	}
}

void Client::SetConsoleCaptureEnabled(bool enabled)
{
	g_ClientMessageCaptureEnabled.store(enabled, std::memory_order_release);
}

bool Client::IsConsoleCaptureReady()
{
	return g_ClientMessageCaptureInstalled.load(std::memory_order_acquire);
}

bool Client::HasReceivedServerCommandList()
{
	return g_ServerCommandListReceived.load(std::memory_order_acquire);
}

uint64_t Client::GetConsoleSessionGeneration()
{
	return g_ConsoleSessionGeneration.load(std::memory_order_acquire);
}
