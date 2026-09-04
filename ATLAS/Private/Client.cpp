#include "pch.h"
#include "../Public/BuildingSMActor.h"
#include "../Public/FortPlaylistAthena.h"
#include "../Public/Utils.h"
#include "../Public/FortPlayerControllerAthena.h"
#include "../Public/Client.h"
#include "../Public/Configuration.h"
#include "../Public/Diagnostics.h"
#include "../Public/GUI.h"
#include "../Public/HotfixReload.h"
#include "../Public/JoinRequestPolicy.h"
#include "../Public/LoadoutTelemetry.h"
#include "../Public/ArenaHudOwnershipPolicy.h"
#include "../Public/ArenaNativeVisualCorrelationPolicy.h"
#include "../Public/ArenaSessionBridgePolicy.h"
#include "../Public/ArenaVisualTracePolicy.h"
#include "../Public/PlaylistHudPatchPolicy.h"
#include "../Public/TournamentRefreshPolicy.h"

#include <array>
#include <atomic>

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
static std::atomic_bool g_FrontendActive = false;

using SpecialEventScriptActivateNative = void(*)(AActor*, int32, float);
using SpecialEventPhaseActivateNative = void(*)(AActor*, float);
static SpecialEventScriptActivateNative g_DurianScriptActivateOriginal = nullptr;
static SpecialEventPhaseActivateNative g_DurianPhaseActivateOriginal = nullptr;
static std::atomic_int g_DurianPendingPhaseIndex = -1;
static std::atomic_int g_DurianObservedScriptPhaseIndex = -1;
static std::atomic_int g_DurianSuppressedPhaseActorIndex = -1;
static std::atomic<UWorld*> g_DurianPendingPhaseWorld = nullptr;
static std::atomic_bool g_DurianPhaseSyncInstalled = false;
static thread_local int g_DurianScriptActivationDepth = 0;
static thread_local bool g_DurianSuppressPhaseActorActivation = false;
static UWorld* g_DurianPhaseSyncWorld = nullptr;
static int32 g_DurianLastAppliedPhaseIndex = -1;

struct FFrontendTournamentRefreshState
{
	UWorld* World = nullptr;
	AFortPlayerControllerAthena* Controller = nullptr;
	ULONGLONG NextAttemptAt = 0;
	unsigned Attempts = 0;
	UEAllocatedString ObservedSelection;
	uint64 ObservedSelectionRevision = 0;
	uint64 SelectionBurstGeneration = 0;
	uint64 ReboundSelectionBurstGeneration = 0;
	bool HasObservedSelection = false;
	bool SelectionTriggeredBurst = false;
};

static FFrontendTournamentRefreshState g_FrontendTournamentRefresh;

static bool TryGetLastSelectedSelectionIdentity(
	UEAllocatedString* selectionIdentity);
static bool TryRebindFrontendSelectedActivity(
	AFortPlayerControllerAthena* playerController);

using ArenaVisualTraceNative = void(*)(UObject*, FFrame&, void*);
static std::atomic<ArenaVisualTraceNative>
	g_FrontendPlaylistChangedBaseOriginal = nullptr;
static std::atomic<ArenaVisualTraceNative>
	g_FrontendPlaylistChangedDerivedOriginal = nullptr;
static std::atomic<ArenaVisualTraceNative>
	g_FrontendModeSetSelectionOriginal = nullptr;
static std::atomic<ArenaVisualTraceNative>
	g_FrontendActivityConfirmedOriginal = nullptr;
static std::atomic_bool g_FrontendPlaylistChangedBaseInstalled = false;
static std::atomic_bool g_FrontendPlaylistChangedDerivedInstalled = false;
static std::atomic_bool g_FrontendModeSetSelectionInstalled = false;
static std::atomic_bool g_FrontendActivityConfirmedInstalled = false;
static std::atomic_uint64_t g_FrontendSelectionRevision = 0;
static std::atomic_uint64_t g_LastFrontendSelectionSignalAt = 0;
static ULONGLONG g_NextFrontendSelectionHookResolveAt = 0;
static thread_local bool g_FrontendTournamentRefreshDispatchActive = false;
static thread_local unsigned g_FrontendSelectionRebindDepth = 0;
static std::atomic<DWORD> g_ClientGameThreadId = 0;
static std::atomic<ArenaVisualTraceNative>
	g_ArenaPlacementRpcOriginal = nullptr;
static std::atomic<ArenaVisualTraceNative>
	g_ArenaTournamentStatRpcOriginal = nullptr;
static std::atomic<ArenaVisualTraceNative>
	g_ArenaTournamentStatConsumerOriginal = nullptr;
static std::atomic<ArenaVisualTraceNative>
	g_ArenaPointScoredConsumerOriginal = nullptr;
static std::atomic<ArenaVisualTraceNative>
	g_ArenaTeamPlacementConsumerOriginal = nullptr;
static std::atomic<UFunction*> g_ArenaPlacementRpcFunction = nullptr;
static std::atomic<UFunction*>
	g_ArenaTournamentStatRpcFunction = nullptr;
static std::atomic<UFunction*>
	g_ArenaTournamentStatConsumerFunction = nullptr;
static std::atomic<UFunction*>
	g_ArenaTeamPlacementConsumerFunction = nullptr;
static std::atomic_bool g_ArenaPlacementRpcTraceInstalled = false;
static std::atomic_bool
	g_ArenaTournamentStatRpcTraceInstalled = false;
static std::atomic_bool
	g_ArenaTournamentStatConsumerTraceInstalled = false;
static std::atomic_bool
	g_ArenaPointScoredConsumerTraceInstalled = false;
static std::atomic_bool
	g_ArenaTeamPlacementConsumerTraceInstalled = false;
static std::atomic_uint64_t g_ArenaTournamentStatConsumerCalls = 0;
static std::atomic_uint64_t g_ArenaPointScoredConsumerCalls = 0;
static std::atomic_uint64_t g_ArenaTeamPlacementConsumerCalls = 0;
static std::atomic<UObject*> g_ArenaLiveScoringAlert = nullptr;
// Consumer hooks can observe Fortnite's authored scoring widget before a
// nested UI-extension child is discoverable through IsInViewport. Preserve
// that exact context for the next game-thread HUD pass instead of reducing the
// observation to a Boolean rescan request.
static std::atomic<UObject*> g_ArenaObservedNativeScoringAlert = nullptr;
static std::atomic<UWorld*> g_ArenaObservedNativeScoringAlertWorld = nullptr;
static std::atomic_uint64_t
	g_ArenaObservedNativeScoringAlertSessionOrdinal = 0;
// Hooks run independently of HUD repair bookkeeping. Mirror only the pointer
// of the exact fallback identity currently owned by ATLAS so its callbacks can
// never be mistaken for evidence of a second native widget.
static std::atomic<UObject*> g_ArenaOwnedScoringAlertForObserver = nullptr;
static std::atomic<UObject*> g_ArenaBusFareIconTexture = nullptr;
static std::atomic<UObject*> g_ArenaEliminationIconTexture = nullptr;
static std::atomic<UObject*> g_ArenaPlacementIconTexture = nullptr;
static std::atomic_bool g_ArenaDirectBadgeIconPreloadAttempted = false;
static std::atomic_bool g_ArenaVisibleAlertReauditRequested = false;
static ULONGLONG g_NextArenaVisualTraceResolveAt = 0;

enum class EArenaVisualTraceSchema
{
	Placement,
	TeamPlacement,
	TournamentStat,
	ZeroParameters
};

struct FPendingArenaVisualFallback
{
	static constexpr std::size_t MaximumStatNameCharacters = 128;
	EArenaVisualTraceSchema Schema =
		EArenaVisualTraceSchema::ZeroParameters;
	alignas(16) std::array<uint8, 0x20> Parameters{};
	std::array<wchar_t, MaximumStatNameCharacters> OwnedStatName{};
	uint32 ParameterSize = 0;
	int32 PreviousPlacement = 0;
	int32 Placement = 0;
	int32 PointsEarned = 0;
	ULONGLONG DueAt = 0;
	uint64 Sequence = 0;
	unsigned RetryCount = 0;
	bool AcceptDedicatedRpcPresentation = false;
	bool Active = false;
	bool Dispatching = false;
	bool CancelAfterDispatch = false;
};

static std::array<FPendingArenaVisualFallback, 32>
	g_PendingArenaVisualFallbacks{};
static uint64 g_NextArenaVisualFallbackSequence = 1;
static ULONGLONG g_NextArenaVisualFallbackDispatchAt = 0;
static thread_local unsigned g_ArenaRpcOriginalDispatchDepth = 0;
static thread_local unsigned g_ArenaTypedConsumerDispatchDepth = 0;
static thread_local bool g_ArenaVisualFallbackDispatchActive = false;
// Consumer hooks are installed from the concrete ArenaScoringHUD CDO before
// a live widget exists. Ignore callbacks caused by ATLAS constructing its own
// compatibility widget until its exact identity has been recorded.
static thread_local bool g_ArenaOwnedFallbackConstructionActive = false;
static constexpr ULONGLONG g_ArenaPlacementFallbackGraceMs = 1000;
static constexpr ULONGLONG g_ArenaPlacementPresentationDedupeMs = 5000;
static int32 g_LastArenaPresentedPlacement = 0;
static ULONGLONG g_LastArenaPresentedPlacementAt = 0;
static int32 g_LastArenaPresentedRpcPlacement = 0;
static int32 g_LastArenaPresentedRpcPoints = 0;
static ULONGLONG g_LastArenaPresentedRpcAt = 0;
static bool g_LastArenaRpcPresented = false;

struct FArenaSessionBridgeRuntimeState
{
	UWorld* World = nullptr;
	AtlasArenaSessionBridgePolicy::FArenaSessionState Session{};
	UObject* LastScoreAlert = nullptr;
	int32 LastAppliedHype = -1;
	int32 CachedScoreTextHype = -1;
	AtlasArenaSessionBridgePolicy::EArenaScoreTextFormatRoute
		CachedScoreTextRoute = AtlasArenaSessionBridgePolicy::
			EArenaScoreTextFormatRoute::None;
	ULONGLONG NextScoreFormatCapabilityProbeAt = 0;
	ULONGLONG NextScoreRepairAt = 0;
	std::uint64_t TerminalSessionOrdinal = 0;
	std::uint64_t OwnedFallbackSuppressedSessionOrdinal = 0;
	std::uint64_t NativeHudOwnerSessionOrdinal = 0;
	bool OwnedFallbackOpportunityReached = false;
	ULONGLONG OwnedFallbackRetireAt = 0;
	ULONGLONG TerminalVisualDrainDeadline = 0;
	unsigned ScoreRepairAttempts = 0;
	unsigned ScoreRepairSuccesses = 0;
	bool ScoreRepairPending = false;
};

struct FReflectedTextOwnerSlot
{
	alignas(8) std::array<uint8, 0x18> Storage{};
	uint32 ValueSize = 0;
};

// The SDK intentionally does not guess at UE4/UE5 FText destructor ABIs.
// Keep two process-lifetime owners and recycle them through the engine's own
// reflected FText return assignment (native shorthand or Conv_StringToText)
// so each replacement releases the prior value. This bounds ownership instead
// of leaking a new FText per event.
static FReflectedTextOwnerSlot g_ArenaScoreTextOwner;
static FReflectedTextOwnerSlot g_ArenaBadgeTextOwner;

struct FPendingArenaBridgeVisual
{
	UWorld* World = nullptr;
	AFortPlayerControllerAthena* Controller = nullptr;
	std::uint64_t SessionOrdinal = 0;
	std::uint64_t EventSequence = 0;
	AtlasArenaSessionBridgePolicy::EArenaPresentationEventKind Kind =
		AtlasArenaSessionBridgePolicy::EArenaPresentationEventKind::BusFare;
	int32 PointsDelta = 0;
	int32 ResultingHype = 0;
	int32 PreviousPlacement = 0;
	int32 Placement = 0;
	int32 AbsoluteStatValue = 0;
	ULONGLONG ReceivedAt = 0;
	ULONGLONG DueAt = 0;
	unsigned RetryCount = 0;
	bool NativeCapabilityExpected = false;
	bool NativeGraceExpected = false;
	bool TournamentRouteAttempted = false;
	bool Active = false;
	bool Dispatching = false;
	bool CancelAfterDispatch = false;
};

static FArenaSessionBridgeRuntimeState g_ArenaSessionBridge;
static std::array<FPendingArenaBridgeVisual, 32>
	g_PendingArenaBridgeVisuals{};
static ULONGLONG g_NextArenaBridgeVisualDispatchAt = 0;
static constexpr ULONGLONG g_ArenaBridgeNativeGraceMs =
	AtlasArenaSessionBridgePolicy::kNativeVisualGraceMilliseconds;
static constexpr ULONGLONG g_ArenaBridgeNativeDedupeMs =
	AtlasArenaSessionBridgePolicy::kNativeVisualDedupeMilliseconds;
static ULONGLONG g_LastArenaEliminationPresentedAt = 0;
static int32 g_LastArenaEliminationStatValue = 0;
static ULONGLONG g_LastArenaBusFarePresentedAt = 0;
static std::array<
	AtlasArenaNativeVisualCorrelationPolicy::
		FPlacementPointsRpcObservation,
	32> g_ArenaPlacementRpcObservations{};
static std::uint64_t g_NextArenaPlacementRpcObservationSequence = 1;
static std::array<
	AtlasArenaNativeVisualCorrelationPolicy::
		FNativePointScoredObservation,
	32> g_ArenaNativePointScoredObservations{};
static std::uint64_t g_NextArenaNativePointScoredObservationSequence = 1;
static UWorld* g_ArenaNativeVisualCorrelationWorld = nullptr;
static AFortPlayerControllerAthena*
	g_ArenaNativeVisualCorrelationController = nullptr;
static constexpr ULONGLONG g_ArenaNativeVisualCorrelationMaximumAgeMs =
	2000;
static constexpr ULONGLONG
	g_ArenaNativePointScoredCorrelationMaximumAgeMs =
		g_ArenaBridgeNativeGraceMs;

static void ProcessDurianPhaseSync(UWorld* world);
static bool HandleDurianFinalPhaseCue(
	const wchar_t* text, int textLength);
static bool IsFrontendWorld(const UWorld* world);
static const UClass* GetLiveArenaScoringHudClass();
static bool IsVerifiedVisibleArenaScoringAlert(UObject* alert);
static bool IsExactOwnedArenaScoringAlert(const UObject* alert);
static bool IsVerifiedNativeOwnedArenaScoringAlert(UObject* alert);
static bool IsVerifiedClientOwnedArenaScoringAlert(UObject* alert);
static bool IsUsableArenaHudOwnerAlert(
	UObject* alert,
	AFortPlayerControllerAthena* playerController);
static bool IsTrustedArenaNativeScoringConsumer(UObject* context);
static void ResetArenaObservedNativeCandidate();
static void MarkArenaSessionScoreRepairPending();
static UObject* FindLiveArenaScoringAlert(
	AFortPlayerControllerAthena* playerController,
	bool preferNewestNative = false);
static void RequestArenaScoringHudRefresh();
static bool HandleArenaSessionBridgeMessage(
	const wchar_t* text, int textLength);
static bool HandleArenaPlacementBridgeMessage(
	const wchar_t* text, int textLength);

static bool SupportsArenaRuntime()
{
	return AtlasPlaylistHudPatchPolicy::ToVersionHundredths(
		VersionInfo.FortniteVersion) >= 820;
}

struct FClientMessageView
{
	const wchar_t* Data;
	int32_t NumElements;
	int32_t MaxElements;
};

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
	if (!message || !view)
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
		view->MaxElements <= 65536;
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
	// ClientMessage is a hot path during travel and gameplay. Keep capture
	// active in Atlas mode even while its tiny bar is hidden so background
	// output (notably outputge) remains available when the console is reopened.
	// Original-console mode remains fully isolated from Atlas capture.
	const bool shouldCapture =
		FConfiguration::ConsoleMode.load(std::memory_order_acquire) ==
			static_cast<int>(EConsoleMode::Atlas);
	const bool shouldInspectTelemetry =
		AtlasLoadoutTelemetry::WantsClientMessageHook();
	const bool shouldInspectDurianCue =
		fabs(VersionInfo.FortniteVersion - 27.11) < 0.001 &&
		g_DurianPhaseSyncInstalled.load(std::memory_order_acquire);
	const bool shouldInspectArenaBridge = SupportsArenaRuntime();

	const UFunction* function =
		(shouldCapture || shouldInspectTelemetry ||
		 shouldInspectDurianCue || shouldInspectArenaBridge)
			? g_ClientMessageFunction.load(std::memory_order_acquire)
			: nullptr;
	const uint32_t textOffset =
		(shouldCapture || shouldInspectTelemetry ||
		 shouldInspectDurianCue || shouldInspectArenaBridge)
			? g_ClientMessageTextOffset.load(std::memory_order_acquire)
			: UINT32_MAX;
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
			if (copied && shouldInspectArenaBridge &&
				HandleArenaSessionBridgeMessage(
				messageCopy, length))
			{
				return;
			}
			if (copied && shouldInspectArenaBridge &&
				HandleArenaPlacementBridgeMessage(
				messageCopy, length))
			{
				return;
			}
			if (copied && HandleDurianFinalPhaseCue(
				messageCopy, length))
			{
				return;
			}
			// Bridge messages are private transport. Suppress only exact,
			// fixed-width markers; malformed and ordinary ClientMessage calls
			// still flow to the console and the game's original handler.
			if (copied && AtlasLoadoutTelemetry::HandleClientMessage(
				messageCopy, length))
			{
				return;
			}
			if (copied && shouldCapture && length > 0)
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

static UObject* ReadStrongObjectPropertyUnsafe(
	UObject* owner,
	const char* propertyName)
{
	if (!IsLiveUObject(owner) || !propertyName || !*propertyName ||
		!owner->Class)
	{
		return nullptr;
	}

	const UField* namedProperty = owner->GetProperty(propertyName);
	// These fields are reflected strong UObject pointers. Accepting
	// WeakObjectProperty here would reinterpret an FWeakObjectPtr as a raw
	// UObject*, which is unsafe on every engine generation.
	constexpr uint64 kObjectProperty = 0x10000;
	const UField* objectProperty = owner->GetProperty(
		propertyName, kObjectProperty);
	if (!namedProperty || namedProperty != objectProperty)
		return nullptr;

	const uint32 offset = GetFromOffset<uint32>(
		namedProperty, Offsets::Offset_Internal);
	const uint32 elementSize = GetFromOffset<uint32>(
		namedProperty, Offsets::ElementSize);
	const int32 ownerSize = owner->Class->GetPropertiesSize();
	if (elementSize != sizeof(UObject*) || ownerSize <= 0 ||
		offset > static_cast<uint32>(ownerSize) ||
		sizeof(UObject*) >
			static_cast<uint32>(ownerSize) - offset)
	{
		return nullptr;
	}

	UObject* value = nullptr;
	memcpy(&value,
		reinterpret_cast<const uint8*>(owner) + offset,
		sizeof(value));
	return IsLiveUObject(value) ? value : nullptr;
}

static UObject* ReadStrongObjectProperty(
	UObject* owner,
	const char* propertyName)
{
	__try
	{
		return ReadStrongObjectPropertyUnsafe(owner, propertyName);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return nullptr;
	}
}

struct FDynamicTextParameterLayout
{
	uint32 FunctionSize = 0;
	uint32 ValueOffset = 0;
	uint32 ValueSize = 0;
};

static bool ResolveDynamicTextParameterUnsafe(
	UFunction* function,
	const char* parameterName,
	bool returnParameter,
	FDynamicTextParameterLayout* layout)
{
	if (layout)
		*layout = {};
	if (!IsLiveUObject(function) || !parameterName || !layout)
		return false;
	// EClassCastFlags::TextProperty. 0x8000 is the generic Property flag and
	// does not prove that the reflected payload has FText ownership/layout.
	constexpr uint64 kTextProperty = 0x40000000ULL;
	const UField* namedProperty = function->GetProperty(parameterName);
	const UField* textProperty = function->GetProperty(
		parameterName, kTextProperty);
	if (!namedProperty || namedProperty != textProperty)
		return false;

	const auto reflected = function->GetParamsNamed();
	if (reflected.Size == 0 || reflected.Size > 0x80)
		return false;

	for (const auto& parameter : reflected.NameOffsetMap)
	{
		if (parameter.Name != parameterName ||
			(parameter.ElementSize != 0x10 &&
			 parameter.ElementSize != 0x18) ||
			parameter.Offset > reflected.Size ||
			parameter.ElementSize >
				reflected.Size - parameter.Offset)
		{
			continue;
		}

		const bool isReturn =
			(parameter.PropertyFlags &
				AtlasArenaVisualTracePolicy::CpfReturnParm) != 0;
		if (isReturn != returnParameter)
			continue;
		if (!returnParameter &&
			!AtlasArenaVisualTracePolicy::IsStructInput({
				parameter.Name.c_str(), parameter.Offset,
				parameter.ElementSize, parameter.PropertyFlags }))
		{
			continue;
		}

		layout->FunctionSize = reflected.Size;
		layout->ValueOffset = parameter.Offset;
		layout->ValueSize = parameter.ElementSize;
		return true;
	}
	return false;
}

static bool BuildReflectedTextValueUnsafe(
	const wchar_t* text,
	FReflectedTextOwnerSlot* owner)
{
	if (!text || !owner)
		return false;

	UObject* textLibrary = const_cast<UObject*>(
		DefaultObjImpl("KismetTextLibrary"));
	UFunction* convert = textLibrary
		? textLibrary->GetFunction("Conv_StringToText")
		: nullptr;
	if (!IsLiveUObject(convert))
		return false;

	const auto conversionSchema = convert->GetParamsNamed();
	uint32 stringOffset = UINT32_MAX;
	FDynamicTextParameterLayout returnLayout{};
	if (!ResolveDynamicTextParameterUnsafe(
			convert, "ReturnValue", true, &returnLayout) ||
		returnLayout.ValueSize > owner->Storage.size() ||
		conversionSchema.Size == 0 ||
		conversionSchema.Size > 0x80)
	{
		return false;
	}
	const UField* namedStringProperty =
		convert->GetProperty("InString");
	const UField* stringProperty =
		convert->GetProperty("InString", 0x4000);
	if (!namedStringProperty ||
		namedStringProperty != stringProperty)
	{
		return false;
	}

	for (const auto& parameter : conversionSchema.NameOffsetMap)
	{
		if (parameter.Name == "InString" &&
			parameter.ElementSize == sizeof(FString) &&
			(parameter.PropertyFlags &
				AtlasArenaVisualTracePolicy::CpfParm) != 0 &&
			parameter.Offset <= conversionSchema.Size &&
			parameter.ElementSize <=
				conversionSchema.Size - parameter.Offset)
		{
			stringOffset = parameter.Offset;
			break;
		}
	}
	if (stringOffset == UINT32_MAX)
		return false;

	alignas(16) std::array<uint8, 0x80> conversionParameters{};
	FString source(text);
	if (!source.Data)
		return false;
	memcpy(conversionParameters.data() + stringOffset,
		&source, sizeof(source));
	if (owner->ValueSize != 0)
	{
		if (owner->ValueSize != returnLayout.ValueSize)
		{
			source.Free();
			return false;
		}
		// Transfer the old owner into ReturnValue. The reflected native thunk's
		// normal FText assignment releases it before storing the new value.
		memcpy(conversionParameters.data() + returnLayout.ValueOffset,
			owner->Storage.data(), owner->ValueSize);
		owner->Storage.fill(0);
		owner->ValueSize = 0;
	}
	textLibrary->ProcessEvent(convert, conversionParameters.data());
	source.Free();
	memcpy(owner->Storage.data(),
		conversionParameters.data() + returnLayout.ValueOffset,
		returnLayout.ValueSize);
	memset(conversionParameters.data() + returnLayout.ValueOffset,
		0, returnLayout.ValueSize);
	owner->ValueSize = returnLayout.ValueSize;
	return true;
}

static bool SetReflectedTextValueUnsafe(
	UObject* textBlock,
	const std::array<uint8, 0x18>& storage,
	uint32 valueSize)
{
	if (!IsLiveUObject(textBlock) || !valueSize ||
		valueSize > storage.size())
	{
		return false;
	}

	UFunction* setText = textBlock->GetFunction("SetText");
	FDynamicTextParameterLayout setLayout{};
	if (!ResolveDynamicTextParameterUnsafe(
			setText, "InText", false, &setLayout) ||
		setLayout.ValueSize != valueSize)
	{
		return false;
	}

	alignas(16) std::array<uint8, 0x80> setParameters{};
	memcpy(setParameters.data() + setLayout.ValueOffset,
		storage.data(), valueSize);
	textBlock->ProcessEvent(setText, setParameters.data());
	return true;
}

static bool TrySetReflectedTextUnsafe(
	UObject* textBlock,
	const wchar_t* text)
{
	return BuildReflectedTextValueUnsafe(
			text, &g_ArenaBadgeTextOwner) &&
		SetReflectedTextValueUnsafe(
			textBlock, g_ArenaBadgeTextOwner.Storage,
			g_ArenaBadgeTextOwner.ValueSize);
}

static bool TrySetReflectedText(
	UObject* textBlock,
	const wchar_t* text)
{
	__try
	{
		return TrySetReflectedTextUnsafe(textBlock, text);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return false;
	}
}

static bool NotifyArenaScoreTextSetUnsafe(UObject* pointCounter)
{
	if (!IsLiveUObject(pointCounter))
		return false;
	UFunction* onPointTextSet = pointCounter->GetFunction(
		"OnPointTextSet");
	if (!IsLiveUObject(onPointTextSet))
		return false;
	const UField* namedProperty =
		onPointTextSet->GetProperty("bHasScore");
	const UField* boolProperty =
		onPointTextSet->GetProperty("bHasScore", 0x20000);
	const auto schema = onPointTextSet->GetParamsNamed();
	if (!namedProperty || namedProperty != boolProperty ||
		schema.Size != 1 || schema.NameOffsetMap.size() != 1 ||
		schema.NameOffsetMap[0].Name != "bHasScore" ||
		schema.NameOffsetMap[0].Offset != 0 ||
		schema.NameOffsetMap[0].ElementSize != 1 ||
		!AtlasArenaVisualTracePolicy::IsPlainInput({
			schema.NameOffsetMap[0].Name.c_str(),
			schema.NameOffsetMap[0].Offset,
			schema.NameOffsetMap[0].ElementSize,
			schema.NameOffsetMap[0].PropertyFlags }))
	{
		return false;
	}
	const uint8 fieldMask = boolProperty->GetFieldMask();
	if (!fieldMask)
		return false;
	uint8 hasScore = fieldMask;
	pointCounter->ProcessEvent(onPointTextSet, &hasScore);
	return true;
}

static bool NotifyArenaScoreTextSet(UObject* pointCounter)
{
	__try
	{
		return NotifyArenaScoreTextSetUnsafe(pointCounter);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return false;
	}
}

struct FArenaShorthandTextFunctionLayout
{
	uint32 FunctionSize = 0;
	uint32 QuantityOffset = 0;
	uint32 MinimumFractionalDigitsOffset = 0;
	uint32 MaximumFractionalDigitsOffset = 0;
	uint32 MaximumDigitsOffset = 0;
	uint32 RoundDownOffset = 0;
	uint32 ReturnOffset = 0;
	uint32 ReturnSize = 0;
	uint8 RoundDownMask = 0;
};

static bool ResolveArenaPlainScalarInputUnsafe(
	UFunction* function,
	const UFunction::ParamsNamed& schema,
	const char* parameterName,
	uint64 propertyCastFlag,
	uint32 expectedSize,
	uint32* outputOffset,
	uint8* outputBoolMask = nullptr)
{
	if (outputOffset)
		*outputOffset = 0;
	if (outputBoolMask)
		*outputBoolMask = 0;
	if (!IsLiveUObject(function) || !parameterName ||
		!outputOffset || !expectedSize)
	{
		return false;
	}

	const UField* namedProperty = function->GetProperty(parameterName);
	const UField* typedProperty = function->GetProperty(
		parameterName, propertyCastFlag);
	if (!namedProperty || namedProperty != typedProperty)
		return false;

	const UFunction::ParamNamed* match = nullptr;
	for (const auto& parameter : schema.NameOffsetMap)
	{
		if (parameter.Name != parameterName)
			continue;
		if (match || parameter.ElementSize != expectedSize ||
			parameter.Offset > schema.Size ||
			parameter.ElementSize > schema.Size - parameter.Offset ||
			!AtlasArenaVisualTracePolicy::IsPlainInput({
				parameter.Name.c_str(), parameter.Offset,
				parameter.ElementSize, parameter.PropertyFlags }))
		{
			return false;
		}
		match = &parameter;
	}
	if (!match)
		return false;

	if (outputBoolMask)
	{
		const uint8 mask = typedProperty->GetFieldMask();
		if (!mask)
			return false;
		*outputBoolMask = mask;
	}
	*outputOffset = match->Offset;
	return true;
}

static bool ResolveArenaShorthandTextFunctionUnsafe(
	UFunction* function,
	AtlasArenaSessionBridgePolicy::EArenaScoreTextFormatRoute route,
	FArenaShorthandTextFunctionLayout* output)
{
	using ERoute = AtlasArenaSessionBridgePolicy::
		EArenaScoreTextFormatRoute;
	if (output)
		*output = {};
	if (!IsLiveUObject(function) || !output)
		return false;

	const auto schema = function->GetParamsNamed();
	if (schema.Size == 0 || schema.Size > 0x80)
		return false;

	FDynamicTextParameterLayout returnLayout{};
	if (!ResolveDynamicTextParameterUnsafe(
			function, "ReturnValue", true, &returnLayout) ||
		returnLayout.FunctionSize != schema.Size)
	{
		return false;
	}

	constexpr uint64 kIntProperty = 0x80;
	constexpr uint64 kBoolProperty = 0x20000;
	if (!ResolveArenaPlainScalarInputUnsafe(
			function, schema, route == ERoute::ModernLimitedDigits
				? "Score"
				: "Quantity",
			kIntProperty, sizeof(int32), &output->QuantityOffset))
	{
		return false;
	}

	std::size_t expectedParameterCount = 0;
	if (route == ERoute::ModernLimitedDigits)
	{
		expectedParameterCount = 3;
		if (!ResolveArenaPlainScalarInputUnsafe(
				function, schema, "MaxDigits", kIntProperty,
				sizeof(int32), &output->MaximumDigitsOffset))
		{
			return false;
		}
	}
	else if (route == ERoute::ModernShorthand ||
		route == ERoute::LegacyShorthand)
	{
		expectedParameterCount = route == ERoute::ModernShorthand ? 5 : 4;
		const char* minimumName = route == ERoute::LegacyShorthand
			? "MiniumFractionalDigits"
			: "MinimumFractionalDigits";
		if (!ResolveArenaPlainScalarInputUnsafe(
				function, schema, minimumName, kIntProperty,
				sizeof(int32),
				&output->MinimumFractionalDigitsOffset) ||
			!ResolveArenaPlainScalarInputUnsafe(
				function, schema, "MaximumFractionalDigits",
				kIntProperty, sizeof(int32),
				&output->MaximumFractionalDigitsOffset))
		{
			return false;
		}
		if (route == ERoute::ModernShorthand &&
			!ResolveArenaPlainScalarInputUnsafe(
				function, schema, "bShouldRoundDown",
				kBoolProperty, sizeof(uint8),
				&output->RoundDownOffset,
				&output->RoundDownMask))
		{
			return false;
		}
	}
	else
	{
		return false;
	}

	if (schema.NameOffsetMap.size() != expectedParameterCount)
		return false;
	output->FunctionSize = schema.Size;
	output->ReturnOffset = returnLayout.ValueOffset;
	output->ReturnSize = returnLayout.ValueSize;
	return true;
}

static bool InvokeArenaShorthandTextFunctionUnsafe(
	UObject* formatter,
	UFunction* function,
	const FArenaShorthandTextFunctionLayout& layout,
	AtlasArenaSessionBridgePolicy::EArenaScoreTextFormatRoute route,
	int32 hype,
	FReflectedTextOwnerSlot* owner)
{
	using ERoute = AtlasArenaSessionBridgePolicy::
		EArenaScoreTextFormatRoute;
	if (!IsLiveUObject(formatter) || !IsLiveUObject(function) ||
		!owner || layout.FunctionSize == 0 ||
		layout.FunctionSize > 0x80 || layout.ReturnSize == 0 ||
		layout.ReturnSize > owner->Storage.size())
	{
		return false;
	}

	alignas(16) std::array<uint8, 0x80> parameters{};
	memcpy(parameters.data() + layout.QuantityOffset,
		&hype, sizeof(hype));
	if (route == ERoute::ModernLimitedDigits)
	{
		const int32 maximumDigits = 3;
		memcpy(parameters.data() + layout.MaximumDigitsOffset,
			&maximumDigits, sizeof(maximumDigits));
	}
	else
	{
		const int32 minimumFractionalDigits = 0;
		const int32 maximumFractionalDigits = 2;
		memcpy(parameters.data() +
			layout.MinimumFractionalDigitsOffset,
			&minimumFractionalDigits,
			sizeof(minimumFractionalDigits));
		memcpy(parameters.data() +
			layout.MaximumFractionalDigitsOffset,
			&maximumFractionalDigits,
			sizeof(maximumFractionalDigits));
		if (route == ERoute::ModernShorthand)
		{
			parameters[layout.RoundDownOffset] &=
				static_cast<uint8>(~layout.RoundDownMask);
		}
	}

	if (owner->ValueSize != 0)
	{
		if (owner->ValueSize != layout.ReturnSize)
			return false;
		// Give the native return assignment the previous FText owner so the
		// engine releases it before returning the new shorthand text.
		memcpy(parameters.data() + layout.ReturnOffset,
			owner->Storage.data(), owner->ValueSize);
		owner->Storage.fill(0);
		owner->ValueSize = 0;
	}
	formatter->ProcessEvent(function, parameters.data());
	memcpy(owner->Storage.data(),
		parameters.data() + layout.ReturnOffset,
		layout.ReturnSize);
	memset(parameters.data() + layout.ReturnOffset,
		0, layout.ReturnSize);
	owner->ValueSize = layout.ReturnSize;
	return true;
}

static bool TryBuildArenaNativeShorthandTextUnsafe(
	int32 hype,
	FReflectedTextOwnerSlot* owner,
	AtlasArenaSessionBridgePolicy::EArenaScoreTextFormatRoute* usedRoute)
{
	using namespace AtlasArenaSessionBridgePolicy;
	if (usedRoute)
		*usedRoute = EArenaScoreTextFormatRoute::None;
	if (!owner || !usedRoute || hype < 0)
		return false;

	UObject* modernFormatter = const_cast<UObject*>(
		DefaultObjImpl("FortUIBlueprintFunctionLibrary"));
	UObject* legacyFormatter = const_cast<UObject*>(
		DefaultObjImpl("FortInventoryContext"));
	UFunction* modernShorthand = modernFormatter
		? modernFormatter->GetFunction("GetShorthandQuantity")
		: nullptr;
	UFunction* modernLimitedDigits = modernFormatter
		? modernFormatter->GetFunction(
			"GetShorthandQuantityWithLimitedDigits")
		: nullptr;
	UFunction* legacyShorthand = legacyFormatter
		? legacyFormatter->GetFunction("GetShorthandItemStackCount")
		: nullptr;

	FArenaShorthandTextFunctionLayout modernLayout{};
	FArenaShorthandTextFunctionLayout limitedLayout{};
	FArenaShorthandTextFunctionLayout legacyLayout{};
	const bool hasModernShorthand =
		ResolveArenaShorthandTextFunctionUnsafe(
			modernShorthand,
			EArenaScoreTextFormatRoute::ModernShorthand,
			&modernLayout);
	const bool hasModernLimitedDigits =
		ResolveArenaShorthandTextFunctionUnsafe(
			modernLimitedDigits,
			EArenaScoreTextFormatRoute::ModernLimitedDigits,
			&limitedLayout);
	const bool hasLegacyShorthand =
		ResolveArenaShorthandTextFunctionUnsafe(
			legacyShorthand,
			EArenaScoreTextFormatRoute::LegacyShorthand,
			&legacyLayout);

	const auto route = SelectScoreTextFormatRoute(
		VersionInfo.FortniteVersion,
		hasModernShorthand,
		hasModernLimitedDigits,
		hasLegacyShorthand);
	UObject* formatter = nullptr;
	UFunction* function = nullptr;
	const FArenaShorthandTextFunctionLayout* layout = nullptr;
	switch (route)
	{
	case EArenaScoreTextFormatRoute::ModernShorthand:
		formatter = modernFormatter;
		function = modernShorthand;
		layout = &modernLayout;
		break;
	case EArenaScoreTextFormatRoute::ModernLimitedDigits:
		formatter = modernFormatter;
		function = modernLimitedDigits;
		layout = &limitedLayout;
		break;
	case EArenaScoreTextFormatRoute::LegacyShorthand:
		formatter = legacyFormatter;
		function = legacyShorthand;
		layout = &legacyLayout;
		break;
	default:
		return false;
	}

	if (!InvokeArenaShorthandTextFunctionUnsafe(
			formatter, function, *layout, route, hype, owner))
	{
		return false;
	}
	*usedRoute = route;
	return true;
}

static const char* ArenaScoreTextFormatRouteName(
	AtlasArenaSessionBridgePolicy::EArenaScoreTextFormatRoute route)
{
	using ERoute = AtlasArenaSessionBridgePolicy::
		EArenaScoreTextFormatRoute;
	switch (route)
	{
	case ERoute::ModernShorthand:
		return "modern-shorthand";
	case ERoute::ModernLimitedDigits:
		return "modern-limited-digits";
	case ERoute::LegacyShorthand:
		return "legacy-shorthand";
	case ERoute::RawInteger:
		return "raw-integer";
	default:
		return "none";
	}
}

static bool TrySetArenaScoreTextUnsafe(
	UObject* scoringAlert,
	int32 hype)
{
	if (!IsLiveUObject(scoringAlert) || hype < 0)
		return false;

	UObject* pointCounter = ReadStrongObjectProperty(
		scoringAlert, "HUDPointCounter");
	UObject* scoreValue = ReadStrongObjectProperty(
		pointCounter, "Text_ScoreValue");
	if (!pointCounter || !scoreValue)
		return false;

	if (g_ArenaSessionBridge.CachedScoreTextHype != hype ||
		!g_ArenaScoreTextOwner.ValueSize ||
		g_ArenaSessionBridge.CachedScoreTextRoute ==
			AtlasArenaSessionBridgePolicy::
				EArenaScoreTextFormatRoute::None ||
		(g_ArenaSessionBridge.CachedScoreTextRoute ==
			AtlasArenaSessionBridgePolicy::
				EArenaScoreTextFormatRoute::RawInteger &&
		 GetTickCount64() >=
			g_ArenaSessionBridge.NextScoreFormatCapabilityProbeAt))
	{
		using ERoute = AtlasArenaSessionBridgePolicy::
			EArenaScoreTextFormatRoute;
		ERoute route = ERoute::None;
		if (!TryBuildArenaNativeShorthandTextUnsafe(
				hype, &g_ArenaScoreTextOwner, &route))
		{
			wchar_t scoreText[32]{};
			if (swprintf_s(
					scoreText, _countof(scoreText), L"%d", hype) <= 0 ||
				!BuildReflectedTextValueUnsafe(
					scoreText, &g_ArenaScoreTextOwner))
			{
				return false;
			}
			route = ERoute::RawInteger;
		}
		g_ArenaSessionBridge.CachedScoreTextHype = hype;
		g_ArenaSessionBridge.CachedScoreTextRoute = route;
		g_ArenaSessionBridge.NextScoreFormatCapabilityProbeAt =
			route == ERoute::RawInteger
				? GetTickCount64() + 2000
				: 0;
		AtlasDiagnostics::WriteLine(
			"arena-session score-format hype=%d route=%s",
			hype, ArenaScoreTextFormatRouteName(route));
	}
	if (!SetReflectedTextValueUnsafe(
			scoreValue,
			g_ArenaScoreTextOwner.Storage,
			g_ArenaScoreTextOwner.ValueSize))
	{
		return false;
	}

	NotifyArenaScoreTextSet(pointCounter);
	return true;
}

static bool TrySetArenaScoreText(
	UObject* scoringAlert,
	int32 hype)
{
	__try
	{
		return TrySetArenaScoreTextUnsafe(scoringAlert, hype);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return false;
	}
}

static bool MatchesArenaVisualTraceSchemaUnsafe(
	UFunction* function,
	EArenaVisualTraceSchema expectedSchema)
{
	if (!IsLiveUObject(function))
		return false;

	const auto reflected = function->GetParamsNamed();
	if (reflected.NameOffsetMap.size() > 4)
		return false;

	std::array<
		AtlasArenaVisualTracePolicy::FParameterSchema,
		4> parameters{};
	for (std::size_t index = 0;
		 index < reflected.NameOffsetMap.size(); ++index)
	{
		const auto& source = reflected.NameOffsetMap[index];
		parameters[index] = {
			source.Name.c_str(),
			source.Offset,
			source.ElementSize,
			source.PropertyFlags
		};
	}

	using namespace AtlasArenaVisualTracePolicy;
	switch (expectedSchema)
	{
	case EArenaVisualTraceSchema::Placement:
		return IsPlacementRpcSchema(
			reflected.Size, parameters.data(),
			reflected.NameOffsetMap.size());

	case EArenaVisualTraceSchema::TeamPlacement:
		return IsTeamPlacementConsumerSchema(
			reflected.Size, parameters.data(),
			reflected.NameOffsetMap.size());

	case EArenaVisualTraceSchema::TournamentStat:
		return IsTournamentStatRpcSchema(
			reflected.Size, parameters.data(),
			reflected.NameOffsetMap.size());

	case EArenaVisualTraceSchema::ZeroParameters:
		return IsZeroParameterSchema(
			reflected.Size,
			reflected.NameOffsetMap.size());

	default:
		return false;
	}
}

static bool MatchesArenaVisualTraceSchema(
	UFunction* function,
	EArenaVisualTraceSchema expectedSchema)
{
	__try
	{
		return MatchesArenaVisualTraceSchemaUnsafe(
			function, expectedSchema);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return false;
	}
}

static bool IsDirectArenaVisualTraceFrame(
	const FFrame* stack,
	const UFunction* function)
{
	__try
	{
		return stack && function && stack->Node == function &&
			stack->Code == nullptr && stack->Locals;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return false;
	}
}

static bool TryDispatchArenaConsumerFallbackUnsafe(
	const char* functionName,
	EArenaVisualTraceSchema schema,
	void* parameters,
	uint32 parameterSize)
{
	UObject* alert = g_ArenaLiveScoringAlert.load(
		std::memory_order_acquire);
	if (!IsLiveUObject(alert) || !functionName)
		return false;

	UFunction* function = alert->GetFunction(functionName);
	if (!MatchesArenaVisualTraceSchema(function, schema))
		return false;
	const auto reflected = function->GetParamsNamed();
	if (reflected.Size != parameterSize)
		return false;
	alert->ProcessEvent(function, parameters);
	return true;
}

static bool TryDispatchArenaConsumerFallback(
	const char* functionName,
	EArenaVisualTraceSchema schema,
	void* parameters,
	uint32 parameterSize)
{
	__try
	{
		return TryDispatchArenaConsumerFallbackUnsafe(
			functionName, schema, parameters, parameterSize);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return false;
	}
}

static bool ReadArenaTournamentStatNameUnsafe(
	const void* parameters,
	uint32 parameterSize,
	const wchar_t** outName,
	std::size_t* outCharacterCount)
{
	if (outName)
		*outName = nullptr;
	if (outCharacterCount)
		*outCharacterCount = 0;
	if (!parameters || parameterSize < sizeof(FString) ||
		!outName || !outCharacterCount)
	{
		return false;
	}

	const auto* statName =
		reinterpret_cast<const FString*>(parameters);
	if (!statName->Data || statName->NumElements <= 1 ||
		statName->NumElements > static_cast<int32>(
			FPendingArenaVisualFallback::MaximumStatNameCharacters) ||
		statName->MaxElements < statName->NumElements ||
		statName->Data[statName->NumElements - 1] != L'\0')
	{
		return false;
	}

	*outName = statName->Data;
	*outCharacterCount = static_cast<std::size_t>(
		statName->NumElements);
	return true;
}

static bool ReadArenaTournamentStatName(
	const void* parameters,
	uint32 parameterSize,
	const wchar_t** outName,
	std::size_t* outCharacterCount)
{
	__try
	{
		return ReadArenaTournamentStatNameUnsafe(
			parameters, parameterSize,
			outName, outCharacterCount);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		if (outName)
			*outName = nullptr;
		if (outCharacterCount)
			*outCharacterCount = 0;
		return false;
	}
}

static bool IsArenaEliminationTournamentStat(
	const void* parameters,
	uint32 parameterSize)
{
	const wchar_t* statName = nullptr;
	std::size_t characterCount = 0;
	if (!ReadArenaTournamentStatName(
			parameters, parameterSize,
			&statName, &characterCount))
	{
		return false;
	}
	__try
	{
		return AtlasArenaVisualTracePolicy::
			IsEliminationTournamentStat(
				statName, characterCount);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return false;
	}
}

static bool CopyOwnedArenaTournamentStatParameters(
	FPendingArenaVisualFallback& destination,
	const void* parameters,
	uint32 parameterSize)
{
	const wchar_t* statName = nullptr;
	std::size_t characterCount = 0;
	if (!ReadArenaTournamentStatName(
			parameters, parameterSize,
			&statName, &characterCount) ||
		parameterSize > destination.Parameters.size() ||
		characterCount > destination.OwnedStatName.size())
	{
		return false;
	}

	__try
	{
		memcpy(destination.Parameters.data(),
			parameters, parameterSize);
		memcpy(destination.OwnedStatName.data(),
			statName, characterCount * sizeof(wchar_t));

		struct FOwnedStringLayout
		{
			wchar_t* Data;
			int32 NumElements;
			int32 MaxElements;
		};
		static_assert(sizeof(FOwnedStringLayout) == sizeof(FString));
		const FOwnedStringLayout ownedName{
			destination.OwnedStatName.data(),
			static_cast<int32>(characterCount),
			static_cast<int32>(characterCount)
		};
		memcpy(destination.Parameters.data(),
			&ownedName, sizeof(ownedName));
		return true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return false;
	}
}

struct FArenaTournamentStatRuntimeLayout
{
	uint32 ParameterSize = 0;
	uint32 DisplayNameSize = 0;
	uint32 ValueOffset = 0;
	uint8 DeltaMask = 0;
	bool HasDeltaFlag = false;
};

enum class EArenaPlacementStatTransitionResult
{
	Unavailable,
	Presented,
	Silent,
	PrimePresented,
	MultiplePresentations
};

static bool ReadArenaTournamentStatFieldUnsafe(
	const UStruct* owner,
	const char* name,
	uint64 castFlags,
	AtlasArenaVisualTracePolicy::FTournamentStatFieldSchema* output)
{
	if (output)
		*output = {};
	if (!owner || !name || !*name || !castFlags || !output ||
		Offsets::ElementSize < sizeof(int32))
	{
		return false;
	}

	const UField* namedProperty = owner->GetProperty(name);
	const UField* typedProperty = owner->GetProperty(name, castFlags);
	if (!namedProperty || namedProperty != typedProperty)
		return false;

	const uint32 offset = GetFromOffset<uint32>(
		namedProperty, Offsets::Offset_Internal);
	const uint32 elementSize = GetFromOffset<uint32>(
		namedProperty, Offsets::ElementSize);
	const int32 arrayDimension = GetFromOffset<int32>(
		namedProperty, Offsets::ElementSize - sizeof(int32));
	if (arrayDimension != 1)
		return false;

	output->Name = name;
	output->Offset = offset;
	output->ElementSize = elementSize;
	return true;
}

static const UStruct* ResolveArenaTournamentStatInfoStructUnsafe()
{
	const UStruct* statInfo = nullptr;
	if (Offsets::StaticFindObject)
	{
		statInfo = reinterpret_cast<const UStruct*>(
			StaticFindObject(
				L"/Script/FortniteGame.FortTournamentStatInfo",
				nullptr));
	}
	if (!statInfo)
		statInfo = SDK::FindStruct("FortTournamentStatInfo");
	if (!statInfo)
		statInfo = SDK::FindStruct("FFortTournamentStatInfo");
	return statInfo;
}

static bool ArenaStructPropertyReferencesUnsafe(
	const UStruct* owner,
	const char* name,
	const UStruct* expectedStruct)
{
	if (!owner || !name || !*name || !expectedStruct)
		return false;
	constexpr uint64 kStructProperty = 0x100000;
	const UField* namedProperty = owner->GetProperty(name);
	const UField* structProperty = owner->GetProperty(
		name, kStructProperty);
	if (!namedProperty || namedProperty != structProperty)
		return false;

	// FStructProperty::Struct moved between the UProperty and FProperty
	// layouts. Search only the bounded reflected-property metadata and require
	// exactly one reference to the canonical UScriptStruct instead of choosing
	// a version-specific raw offset.
	constexpr std::size_t kMetadataBytes = 0x100;
	std::size_t matches = 0;
	for (std::size_t offset = 0;
		offset + sizeof(const UStruct*) <= kMetadataBytes;
		offset += alignof(const UStruct*))
	{
		if (GetFromOffset<const UStruct*>(
				structProperty, static_cast<uint32>(offset)) ==
			expectedStruct)
		{
			++matches;
		}
	}
	return matches == 1;
}

static bool ResolveArenaTournamentStatRuntimeLayoutUnsafe(
	UFunction* function,
	FArenaTournamentStatRuntimeLayout* output)
{
	if (output)
		*output = {};
	if (!function || !output ||
		!MatchesArenaVisualTraceSchema(
			function, EArenaVisualTraceSchema::TournamentStat))
	{
		return false;
	}

	const UStruct* statInfo =
		ResolveArenaTournamentStatInfoStructUnsafe();
	if (!statInfo)
		return false;
	const int32 statInfoSize = statInfo->GetPropertiesSize();
	const auto functionSchema = function->GetParamsNamed();
	if (statInfoSize <= 0 ||
		functionSchema.Size != static_cast<uint32>(statInfoSize) ||
		functionSchema.NameOffsetMap.size() != 1 ||
		!ArenaStructPropertyReferencesUnsafe(
			function,
			functionSchema.NameOffsetMap[0].Name.c_str(),
			statInfo))
	{
		return false;
	}

	constexpr uint64 kStrProperty = 0x4000;
	constexpr uint64 kNameProperty = 0x2000;
	constexpr uint64 kIntProperty = 0x80;
	constexpr uint64 kBoolProperty = 0x20000;
	std::array<
		AtlasArenaVisualTracePolicy::FTournamentStatFieldSchema,
		4> fields{};
	std::size_t fieldCount = 0;
	uint8 deltaMask = 0;
	if (!ReadArenaTournamentStatFieldUnsafe(
			statInfo, "StatName", kStrProperty,
			&fields[fieldCount++]) ||
		!ReadArenaTournamentStatFieldUnsafe(
			statInfo, "StatDisplayName", kNameProperty,
			&fields[fieldCount++]) ||
		!ReadArenaTournamentStatFieldUnsafe(
			statInfo, "StatValue", kIntProperty,
			&fields[fieldCount++]))
	{
		return false;
	}
	if (statInfo->GetProperty("bIsDeltaCount"))
	{
		if (!ReadArenaTournamentStatFieldUnsafe(
				statInfo, "bIsDeltaCount", kBoolProperty,
				&fields[fieldCount++]))
		{
			return false;
		}
		const UField* deltaProperty = statInfo->GetProperty(
			"bIsDeltaCount", kBoolProperty);
		deltaMask = deltaProperty
			? deltaProperty->GetFieldMask()
			: 0;
		if (!deltaMask)
			return false;
	}

	using namespace AtlasArenaVisualTracePolicy;
	const auto schema = ResolveTournamentStatPayloadSchema(
		functionSchema.Size, fields.data(), fieldCount);
	const uint32 displayNameSize =
		TournamentStatDisplayNameSize(schema);
	const uint32 valueOffset = TournamentStatValueOffset(schema);
	if (!displayNameSize || !valueOffset ||
		valueOffset + sizeof(int32) > functionSchema.Size)
	{
		return false;
	}

	output->ParameterSize = functionSchema.Size;
	output->DisplayNameSize = displayNameSize;
	output->ValueOffset = valueOffset;
	output->HasDeltaFlag = TournamentStatHasDeltaFlag(schema);
	output->DeltaMask = output->HasDeltaFlag ? deltaMask : 0;
	return true;
}

static bool ResolveArenaTournamentStatRuntimeLayout(
	UFunction* function,
	FArenaTournamentStatRuntimeLayout* output)
{
	__try
	{
		return ResolveArenaTournamentStatRuntimeLayoutUnsafe(
			function, output);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		if (output)
			*output = {};
		return false;
	}
}

static bool ReadArenaPlacementTournamentStatValueUnsafe(
	UFunction* function,
	const void* parameters,
	uint32 parameterSize,
	int32* placement)
{
	if (placement)
		*placement = 0;
	if (!function || !parameters || !placement)
		return false;

	const wchar_t* statName = nullptr;
	std::size_t characterCount = 0;
	if (!ReadArenaTournamentStatNameUnsafe(
			parameters, parameterSize,
			&statName, &characterCount) ||
		!AtlasArenaVisualTracePolicy::IsPlacementTournamentStat(
			statName, characterCount))
	{
		return false;
	}

	FArenaTournamentStatRuntimeLayout layout{};
	if (!ResolveArenaTournamentStatRuntimeLayoutUnsafe(
			function, &layout) ||
		layout.ParameterSize != parameterSize)
	{
		return false;
	}
	memcpy(placement,
		static_cast<const uint8*>(parameters) + layout.ValueOffset,
		sizeof(*placement));
	return *placement > 0 && *placement <= 100;
}

static bool ReadArenaPlacementTournamentStatValue(
	UFunction* function,
	const void* parameters,
	uint32 parameterSize,
	int32* placement)
{
	__try
	{
		return ReadArenaPlacementTournamentStatValueUnsafe(
			function, parameters, parameterSize, placement);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		if (placement)
			*placement = 0;
		return false;
	}
}

enum class EObservedArenaTournamentStat
{
	Unknown,
	MatchPlayed,
	Eliminations,
	Placement
};

static bool ReadArenaTournamentStatValueUnsafe(
	UFunction* function,
	const void* parameters,
	uint32 parameterSize,
	EObservedArenaTournamentStat* kind,
	int32* value)
{
	if (kind)
		*kind = EObservedArenaTournamentStat::Unknown;
	if (value)
		*value = 0;
	if (!function || !parameters || !kind || !value)
		return false;

	const wchar_t* statName = nullptr;
	std::size_t characterCount = 0;
	if (!ReadArenaTournamentStatNameUnsafe(
			parameters, parameterSize,
			&statName, &characterCount))
	{
		return false;
	}

	using namespace AtlasArenaVisualTracePolicy;
	if (IsMatchPlayedTournamentStat(statName, characterCount))
		*kind = EObservedArenaTournamentStat::MatchPlayed;
	else if (IsEliminationTournamentStat(statName, characterCount))
		*kind = EObservedArenaTournamentStat::Eliminations;
	else if (IsPlacementTournamentStat(statName, characterCount))
		*kind = EObservedArenaTournamentStat::Placement;
	else
		return false;

	FArenaTournamentStatRuntimeLayout layout{};
	if (!ResolveArenaTournamentStatRuntimeLayoutUnsafe(
			function, &layout) ||
		layout.ParameterSize != parameterSize)
	{
		*kind = EObservedArenaTournamentStat::Unknown;
		return false;
	}
	memcpy(value,
		static_cast<const uint8*>(parameters) + layout.ValueOffset,
		sizeof(*value));
	return true;
}

static bool ReadArenaTournamentStatValue(
	UFunction* function,
	const void* parameters,
	uint32 parameterSize,
	EObservedArenaTournamentStat* kind,
	int32* value)
{
	__try
	{
		return ReadArenaTournamentStatValueUnsafe(
			function, parameters, parameterSize, kind, value);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		if (kind)
			*kind = EObservedArenaTournamentStat::Unknown;
		if (value)
			*value = 0;
		return false;
	}
}

static bool DispatchArenaPlacementTournamentStatValueUnsafe(
	UObject* alert,
	UFunction* function,
	const FArenaTournamentStatRuntimeLayout& layout,
	int32 placement)
{
	if (!IsLiveUObject(alert) || !IsLiveUObject(function) ||
		placement <= 0 || placement > 101 ||
		layout.ParameterSize == 0 || layout.ParameterSize > 0x20 ||
		!Offsets::Realloc || !Offsets::FNameConstructor)
	{
		return false;
	}

	std::array<uint8, 0x20> parameters{};
	FString statName(L"PLACEMENT_STAT_INDEX");
	FName displayName(L"Placement");
	if (!statName.Data || !displayName.IsValid())
	{
		statName.Free();
		return false;
	}
	memcpy(parameters.data(), &statName, sizeof(statName));
	memcpy(parameters.data() + 0x10,
		&displayName, layout.DisplayNameSize);
	if (layout.HasDeltaFlag)
		parameters[0x14] = 0;
	memcpy(parameters.data() + layout.ValueOffset,
		&placement, sizeof(placement));

	alert->ProcessEvent(function, parameters.data());
	statName.Free();
	return true;
}

enum class EArenaBridgeStatDispatchResult
{
	Unavailable,
	Silent,
	Presented
};

static EArenaBridgeStatDispatchResult
	DispatchArenaBridgeTournamentStatUnsafe(
		AtlasArenaSessionBridgePolicy::EArenaPresentationEventKind kind,
		int32 absoluteStatValue)
{
	using EKind =
		AtlasArenaSessionBridgePolicy::EArenaPresentationEventKind;
	if ((kind != EKind::BusFare && kind != EKind::Elimination) ||
		absoluteStatValue < 0)
	{
		return EArenaBridgeStatDispatchResult::Unavailable;
	}

	UObject* alert = g_ArenaLiveScoringAlert.load(
		std::memory_order_acquire);
	// Tournament-stat updates mutate the widget's internal stat map. They are a
	// fallback-only route: replaying them into Fortnite's authored widget after
	// its TournamentStats already processed the event can drift the native
	// persistent Hype total. Native widgets use the direct badge/correlation
	// paths and always receive the authoritative bridge total via score repair.
	UObject* observedNative = g_ArenaObservedNativeScoringAlert.load(
		std::memory_order_acquire);
	UObject* ownedFallback = g_ArenaOwnedScoringAlertForObserver.load(
		std::memory_order_acquire);
	const bool nativeTakeoverPending = observedNative &&
		observedNative != ownedFallback;
	if (!AtlasArenaHudOwnershipPolicy::
			CanDispatchStateChangingTournamentStatFallback(
				IsVerifiedClientOwnedArenaScoringAlert(alert),
				nativeTakeoverPending))
		return EArenaBridgeStatDispatchResult::Unavailable;
	UFunction* function = alert->GetFunction("OnTournamentStatUpdate");
	FArenaTournamentStatRuntimeLayout layout{};
	if (!ResolveArenaTournamentStatRuntimeLayoutUnsafe(
			function, &layout) ||
		layout.ParameterSize == 0 || layout.ParameterSize > 0x20)
	{
		return EArenaBridgeStatDispatchResult::Unavailable;
	}

	const wchar_t* statNameText = kind == EKind::BusFare
		? AtlasArenaSessionBridgePolicy::kBusFareTournamentStatName
		: L"TEAM_ELIMS_STAT_INDEX";
	const wchar_t* displayNameText = kind == EKind::BusFare
		? AtlasArenaSessionBridgePolicy::kBusFareTournamentDisplayName
		: L"Eliminations";
	FString statName(statNameText);
	FName displayName(displayNameText);
	if (!statName.Data || !displayName.IsValid())
	{
		statName.Free();
		return EArenaBridgeStatDispatchResult::Unavailable;
	}

	alignas(16) std::array<uint8, 0x20> parameters{};
	memcpy(parameters.data(), &statName, sizeof(statName));
	memcpy(parameters.data() + 0x10,
		&displayName, layout.DisplayNameSize);
	const bool useDelta = layout.HasDeltaFlag;
	if (useDelta)
		parameters[0x14] = layout.DeltaMask;
	const int32 value = useDelta
		? (kind == EKind::BusFare ? 1 : 1)
		: absoluteStatValue;
	memcpy(parameters.data() + layout.ValueOffset,
		&value, sizeof(value));

	const uint64 pointCallsBefore =
		g_ArenaPointScoredConsumerCalls.load(
			std::memory_order_acquire);
	g_ArenaVisualFallbackDispatchActive = true;
	alert->ProcessEvent(function, parameters.data());
	g_ArenaVisualFallbackDispatchActive = false;
	statName.Free();
	const uint64 pointCallsAfter =
		g_ArenaPointScoredConsumerCalls.load(
			std::memory_order_acquire);
	return pointCallsAfter > pointCallsBefore
		? EArenaBridgeStatDispatchResult::Presented
		: EArenaBridgeStatDispatchResult::Silent;
}

static EArenaBridgeStatDispatchResult
	DispatchArenaBridgeTournamentStat(
		AtlasArenaSessionBridgePolicy::EArenaPresentationEventKind kind,
		int32 absoluteStatValue)
{
	__try
	{
		return DispatchArenaBridgeTournamentStatUnsafe(
			kind, absoluteStatValue);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		g_ArenaVisualFallbackDispatchActive = false;
		return EArenaBridgeStatDispatchResult::Unavailable;
	}
}

struct FArenaSetBrushFromTextureLayout
{
	uint32 ParameterSize = 0;
	uint32 TextureOffset = 0;
	uint32 MatchSizeOffset = 0;
	uint8 MatchSizeMask = 0;
};

static bool ResolveArenaSetBrushFromTextureLayoutUnsafe(
	UFunction* function,
	FArenaSetBrushFromTextureLayout* layout)
{
	if (layout)
		*layout = {};
	if (!IsLiveUObject(function) || !layout)
		return false;

	constexpr uint64 kObjectProperty = 0x10000;
	constexpr uint64 kBoolProperty = 0x20000;
	const UField* namedTexture = function->GetProperty("Texture");
	const UField* typedTexture = function->GetProperty(
		"Texture", kObjectProperty);
	const UField* namedMatchSize = function->GetProperty("bMatchSize");
	const UField* typedMatchSize = function->GetProperty(
		"bMatchSize", kBoolProperty);
	if (!namedTexture || namedTexture != typedTexture ||
		!namedMatchSize || namedMatchSize != typedMatchSize)
	{
		return false;
	}

	const auto schema = function->GetParamsNamed();
	if (schema.NameOffsetMap.size() != 2)
	{
		return false;
	}
	std::array<AtlasArenaVisualTracePolicy::FParameterSchema, 2>
		parameters{};
	for (std::size_t index = 0;
		 index < schema.NameOffsetMap.size(); ++index)
	{
		const auto& source = schema.NameOffsetMap[index];
		parameters[index] = {
			source.Name.c_str(), source.Offset,
			source.ElementSize, source.PropertyFlags };
	}
	if (!AtlasArenaVisualTracePolicy::IsSetBrushFromTextureSchema(
			schema.Size, parameters.data(), parameters.size()))
	{
		return false;
	}

	bool foundTexture = false;
	bool foundMatchSize = false;
	for (const auto& parameter : schema.NameOffsetMap)
	{
		if (parameter.Name == "Texture")
		{
			layout->TextureOffset = parameter.Offset;
			foundTexture = true;
		}
		else if (parameter.Name == "bMatchSize")
		{
			layout->MatchSizeOffset = parameter.Offset;
			foundMatchSize = true;
		}
	}
	const uint32 reflectedTextureOffset = GetFromOffset<uint32>(
		namedTexture, Offsets::Offset_Internal);
	const uint32 reflectedTextureSize = GetFromOffset<uint32>(
		namedTexture, Offsets::ElementSize);
	const uint32 reflectedMatchSizeOffset = GetFromOffset<uint32>(
		namedMatchSize, Offsets::Offset_Internal);
	const uint32 reflectedMatchSizeSize = GetFromOffset<uint32>(
		namedMatchSize, Offsets::ElementSize);
	const uint8 matchSizeMask = namedMatchSize->GetFieldMask();
	if (!foundTexture || !foundMatchSize || !matchSizeMask ||
		layout->TextureOffset != reflectedTextureOffset ||
		layout->MatchSizeOffset != reflectedMatchSizeOffset ||
		reflectedTextureSize != sizeof(UObject*) ||
		reflectedMatchSizeSize != sizeof(uint8))
	{
		*layout = {};
		return false;
	}

	layout->ParameterSize = schema.Size;
	layout->MatchSizeMask = matchSizeMask;
	return true;
}

static std::atomic<UObject*>* ArenaDirectBadgeIconCache(
	AtlasArenaSessionBridgePolicy::EArenaPresentationEventKind kind)
{
	using EKind =
		AtlasArenaSessionBridgePolicy::EArenaPresentationEventKind;
	switch (kind)
	{
	case EKind::BusFare:
		return &g_ArenaBusFareIconTexture;
	case EKind::Elimination:
		return &g_ArenaEliminationIconTexture;
	case EKind::Placement:
	case EKind::Victory:
		return &g_ArenaPlacementIconTexture;
	default:
		return nullptr;
	}
}

static const UClass* ResolveTexture2DClassForArenaIcons()
{
	if (!Offsets::StaticFindObject)
		return nullptr;
	__try
	{
		const UClass* classClass = UClass::StaticClass();
		if (!IsLiveUObject(classClass))
			return nullptr;
		const UClass* textureClass = static_cast<const UClass*>(
			SDK::StaticFindObject(
				L"/Script/Engine.Texture2D", classClass));
		if (!IsLiveUObject(textureClass) ||
			!textureClass->IsA(classClass))
		{
			return nullptr;
		}
		return textureClass;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return nullptr;
	}
}

static UObject* LoadArenaDirectBadgeIconTexture(
	const UClass* textureClass,
	const wchar_t* path)
{
	if (!IsLiveUObject(textureClass) || !path || !*path ||
		!Offsets::StaticFindObject)
	{
		return nullptr;
	}
	__try
	{
		const UObject* texture = SDK::StaticFindObject(
			path, textureClass);
		if (!texture && Offsets::StaticLoadObject)
			texture = SDK::StaticLoadObject(path, textureClass);
		if (!IsLiveUObject(texture) || !texture->IsA(textureClass))
			return nullptr;
		texture->AddToRoot();
		return const_cast<UObject*>(texture);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return nullptr;
	}
}

static bool HasPendingArenaBridgeVisual()
{
	for (const auto& pending : g_PendingArenaBridgeVisuals)
	{
		if (pending.Active || pending.Dispatching)
			return true;
	}
	return false;
}

static void MaintainArenaDirectBadgeIconPreload()
{
	if (g_ArenaDirectBadgeIconPreloadAttempted.load(
			std::memory_order_acquire) ||
		HasPendingArenaBridgeVisual() ||
		!IsLiveUObject(g_ArenaLiveScoringAlert.load(
			std::memory_order_acquire)) ||
		!Offsets::StaticFindObject)
	{
		return;
	}

	bool expected = false;
	if (!g_ArenaDirectBadgeIconPreloadAttempted.compare_exchange_strong(
			expected, true, std::memory_order_acq_rel))
	{
		return;
	}
	const UClass* textureClass = ResolveTexture2DClassForArenaIcons();
	if (!textureClass)
	{
		AtlasDiagnostics::WriteLine(
			"arena-session direct-badge-icons preload-unavailable "
			"reason=texture-class");
		return;
	}

	using EKind =
		AtlasArenaSessionBridgePolicy::EArenaPresentationEventKind;
	struct FIconLoad
	{
		EKind Kind;
		std::atomic<UObject*>* Cache;
	};
	const std::array<FIconLoad, 3> icons{{
		{ EKind::BusFare, &g_ArenaBusFareIconTexture },
		{ EKind::Elimination, &g_ArenaEliminationIconTexture },
		{ EKind::Placement, &g_ArenaPlacementIconTexture }
	}};
	std::array<UObject*, 3> textures{};
	unsigned loaded = 0;
	for (std::size_t index = 0; index < icons.size(); ++index)
	{
		textures[index] = LoadArenaDirectBadgeIconTexture(
			textureClass,
			AtlasArenaSessionBridgePolicy::
				DirectBadgeIconTexturePath(icons[index].Kind));
		if (textures[index])
			++loaded;
	}
	// Enable the authored override only as a complete set. Otherwise a later
	// direct badge whose texture is absent could inherit the previous badge's
	// icon (for example, an elimination retaining the bus-fare icon).
	if (loaded == icons.size())
	{
		for (std::size_t index = 0; index < icons.size(); ++index)
		{
			icons[index].Cache->store(
				textures[index], std::memory_order_release);
		}
	}
	AtlasDiagnostics::WriteLine(
		"arena-session direct-badge-icons preloaded=%u total=%zu "
		"enabled=%d",
		loaded, icons.size(), loaded == icons.size() ? 1 : 0);
}

static bool TrySetArenaDirectBadgeIconUnsafe(
	UObject* alert,
	AtlasArenaSessionBridgePolicy::EArenaPresentationEventKind kind)
{
	std::atomic<UObject*>* cache = ArenaDirectBadgeIconCache(kind);
	UObject* texture = cache
		? cache->load(std::memory_order_acquire)
		: nullptr;
	if (!IsLiveUObject(alert) || !IsLiveUObject(texture) ||
		!AtlasArenaSessionBridgePolicy::
			DirectBadgeIconTexturePath(kind))
	{
		return false;
	}

	UObject* icon = ReadStrongObjectProperty(alert, "Icon_Scoring");
	if (!icon)
		return false;
	UFunction* setBrush = icon->GetFunction("SetBrushFromTexture");
	FArenaSetBrushFromTextureLayout layout{};
	if (!ResolveArenaSetBrushFromTextureLayoutUnsafe(
			setBrush, &layout))
	{
		return false;
	}

	alignas(16) std::array<uint8, 0x20> parameters{};
	memcpy(parameters.data() + layout.TextureOffset,
		&texture, sizeof(texture));
	// Keep the authored widget dimensions. The zeroed bool parameter is the
	// reflected bMatchSize=false value; the validated mask proves this is an
	// actual BoolProperty rather than an unrelated one-byte input.
	parameters[layout.MatchSizeOffset] &=
		static_cast<uint8>(~layout.MatchSizeMask);
	icon->ProcessEvent(setBrush, parameters.data());
	return true;
}

static bool TrySetArenaDirectBadgeIcon(
	UObject* alert,
	AtlasArenaSessionBridgePolicy::EArenaPresentationEventKind kind)
{
	__try
	{
		return TrySetArenaDirectBadgeIconUnsafe(alert, kind);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return false;
	}
}

static bool TryDispatchDirectArenaBadgeUnsafe(
	AtlasArenaSessionBridgePolicy::EArenaPresentationEventKind kind,
	int32 absoluteStatValue,
	int32 pointsDelta)
{
	using EKind =
		AtlasArenaSessionBridgePolicy::EArenaPresentationEventKind;
	UObject* alert = g_ArenaLiveScoringAlert.load(
		std::memory_order_acquire);
	if (!IsLiveUObject(alert) || pointsDelta == 0)
	{
		return false;
	}

	UObject* description = ReadStrongObjectProperty(
		alert, "Text_ScoreDesc");
	UObject* pointCount = ReadStrongObjectProperty(
		alert, "Text_PointCount");
	if (!description || !pointCount)
		return false;
	UFunction* onPointScored = alert->GetFunction("OnPointScored");
	if (!MatchesArenaVisualTraceSchema(
			onPointScored, EArenaVisualTraceSchema::ZeroParameters))
	{
		return false;
	}

	wchar_t descriptionText[64]{};
	wchar_t pointText[64]{};
	if (kind == EKind::BusFare)
	{
		wcscpy_s(descriptionText, L"BUS FARE");
	}
	else if (kind == EKind::Elimination)
	{
		if (swprintf_s(
		descriptionText, _countof(descriptionText),
		absoluteStatValue == 1
			? L"%d ELIMINATION"
			: L"%d ELIMINATIONS",
		absoluteStatValue) <= 0)
		{
			return false;
		}
	}
	else if (kind == EKind::Victory)
	{
		wcscpy_s(descriptionText, L"VICTORY ROYALE");
	}
	else if (kind == EKind::Placement)
	{
		if (swprintf_s(
			descriptionText, _countof(descriptionText),
			L"TOP %d", absoluteStatValue) <= 0)
		{
			return false;
		}
	}
	else
	{
		return false;
	}
	if (swprintf_s(
		pointText, _countof(pointText),
		L"%+d HYPE", pointsDelta) <= 0 ||
		!TrySetReflectedText(description, descriptionText) ||
		!TrySetReflectedText(pointCount, pointText))
	{
		return false;
	}
	// Icon setup is deliberately fail-open: assets are preloaded outside the
	// award dispatch path, and a missing texture or incompatible UImage schema
	// must never delay the text-only direct badge.
	TrySetArenaDirectBadgeIcon(alert, kind);

	g_ArenaVisualFallbackDispatchActive = true;
	alert->ProcessEvent(onPointScored, nullptr);
	g_ArenaVisualFallbackDispatchActive = false;
	// The authored animation can rewrite the score text after the bridge has
	// already stabilized it. Re-arm the authoritative score repair in the same
	// game-thread tick so the badge never leaves an older native total behind.
	MarkArenaSessionScoreRepairPending();
	return true;
}

static bool TryDispatchDirectArenaBadge(
	AtlasArenaSessionBridgePolicy::EArenaPresentationEventKind kind,
	int32 absoluteStatValue,
	int32 pointsDelta)
{
	__try
	{
		return TryDispatchDirectArenaBadgeUnsafe(
			kind, absoluteStatValue, pointsDelta);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		g_ArenaVisualFallbackDispatchActive = false;
		return false;
	}
}

static EArenaPlacementStatTransitionResult
	TryDispatchArenaPlacementTournamentStatTransitionUnsafe(
		int32 previousPlacement,
		int32 placement)
{
	if (previousPlacement <= placement || placement <= 0 ||
		previousPlacement > 101)
	{
		return EArenaPlacementStatTransitionResult::Unavailable;
	}

	UObject* alert = g_ArenaLiveScoringAlert.load(
		std::memory_order_acquire);
	UObject* observedNative = g_ArenaObservedNativeScoringAlert.load(
		std::memory_order_acquire);
	UObject* ownedFallback = g_ArenaOwnedScoringAlertForObserver.load(
		std::memory_order_acquire);
	const bool nativeTakeoverPending = observedNative &&
		observedNative != ownedFallback;
	if (!AtlasArenaHudOwnershipPolicy::
			CanDispatchStateChangingTournamentStatFallback(
				IsVerifiedClientOwnedArenaScoringAlert(alert),
				nativeTakeoverPending))
		return EArenaPlacementStatTransitionResult::Unavailable;
	UFunction* function = alert->GetFunction(
		"OnTournamentStatUpdate");
	FArenaTournamentStatRuntimeLayout layout{};
	if (!ResolveArenaTournamentStatRuntimeLayoutUnsafe(
			function, &layout))
	{
		return EArenaPlacementStatTransitionResult::Unavailable;
	}

	const uint64 pointCallsBefore =
		g_ArenaPointScoredConsumerCalls.load(
			std::memory_order_acquire);
	g_ArenaVisualFallbackDispatchActive = true;
	const bool primed = DispatchArenaPlacementTournamentStatValueUnsafe(
		alert, function, layout, previousPlacement);
	const uint64 pointCallsAfterPrime =
		g_ArenaPointScoredConsumerCalls.load(
			std::memory_order_acquire);
	if (!primed)
	{
		g_ArenaVisualFallbackDispatchActive = false;
		return EArenaPlacementStatTransitionResult::Unavailable;
	}
	if (pointCallsAfterPrime != pointCallsBefore)
	{
		g_ArenaVisualFallbackDispatchActive = false;
		return EArenaPlacementStatTransitionResult::PrimePresented;
	}

	const bool targeted = DispatchArenaPlacementTournamentStatValueUnsafe(
		alert, function, layout, placement);
	g_ArenaVisualFallbackDispatchActive = false;
	if (!targeted)
		return EArenaPlacementStatTransitionResult::Unavailable;
	const uint64 pointCallsAfterTarget =
		g_ArenaPointScoredConsumerCalls.load(
			std::memory_order_acquire);
	if (AtlasArenaVisualTracePolicy::
		DidPlacementStatTransitionPresentExactlyOnce(
			pointCallsBefore,
			pointCallsAfterPrime,
			pointCallsAfterTarget))
	{
		return EArenaPlacementStatTransitionResult::Presented;
	}
	return pointCallsAfterTarget == pointCallsBefore
		? EArenaPlacementStatTransitionResult::Silent
		: EArenaPlacementStatTransitionResult::MultiplePresentations;
}

static EArenaPlacementStatTransitionResult
	TryDispatchArenaPlacementTournamentStatTransition(
		int32 previousPlacement,
		int32 placement)
{
	__try
	{
		return TryDispatchArenaPlacementTournamentStatTransitionUnsafe(
			previousPlacement, placement);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		g_ArenaVisualFallbackDispatchActive = false;
		return EArenaPlacementStatTransitionResult::Unavailable;
	}
}

static bool TryDispatchArenaTournamentStatPresentation(
	void* parameters,
	uint32 parameterSize)
{
	g_ArenaVisualFallbackDispatchActive = true;
	const bool updated = TryDispatchArenaConsumerFallback(
		"OnTournamentStatUpdate",
		EArenaVisualTraceSchema::TournamentStat,
		parameters, parameterSize);
	g_ArenaVisualFallbackDispatchActive = false;
	return updated;
}

static bool TryDispatchArenaPointScoredPresentation()
{
	g_ArenaVisualFallbackDispatchActive = true;
	const bool updated = TryDispatchArenaConsumerFallback(
		"OnPointScored",
		EArenaVisualTraceSchema::ZeroParameters,
		nullptr, 0);
	g_ArenaVisualFallbackDispatchActive = false;
	return updated;
}

static bool TryDispatchArenaTeamPlacementPresentation(
	int32 placement,
	bool* pointScored)
{
	if (pointScored)
		*pointScored = false;
	if (placement <= 0)
		return false;

	const uint64 pointCallsBefore =
		g_ArenaPointScoredConsumerCalls.load(
			std::memory_order_acquire);
	g_ArenaVisualFallbackDispatchActive = true;
	const bool updated = TryDispatchArenaConsumerFallback(
		"OnTeamPlacementChanged",
		EArenaVisualTraceSchema::TeamPlacement,
		&placement, sizeof(placement));
	g_ArenaVisualFallbackDispatchActive = false;
	if (pointScored)
	{
		*pointScored =
			g_ArenaPointScoredConsumerCalls.load(
				std::memory_order_acquire) > pointCallsBefore;
	}
	return updated;
}

static bool WasArenaPlacementRecentlyPresented(
	int32 placement, ULONGLONG now)
{
	return placement > 0 &&
		g_LastArenaPresentedPlacement == placement &&
		now >= g_LastArenaPresentedPlacementAt &&
		now - g_LastArenaPresentedPlacementAt <=
			g_ArenaPlacementPresentationDedupeMs;
}

static void MarkArenaPlacementPresented(int32 placement)
{
	if (placement <= 0)
		return;
	g_LastArenaPresentedPlacement = placement;
	g_LastArenaPresentedPlacementAt = GetTickCount64();
}

static bool WasArenaPlacementRpcRecentlyPresented(
	int32 placement, int32 pointsEarned, ULONGLONG now)
{
	return AtlasArenaVisualTracePolicy::
			DoesPlacementBridgeMatchPresentedRpc(
				g_LastArenaPresentedRpcPlacement,
				g_LastArenaPresentedRpcPoints,
				g_LastArenaRpcPresented,
				placement, pointsEarned) &&
		now >= g_LastArenaPresentedRpcAt &&
		now - g_LastArenaPresentedRpcAt <=
			g_ArenaPlacementPresentationDedupeMs;
}

static void BeginArenaPlacementRpcObservation(
	int32 placement, int32 pointsEarned)
{
	g_LastArenaPresentedRpcPlacement = placement;
	g_LastArenaPresentedRpcPoints = pointsEarned;
	g_LastArenaPresentedRpcAt = GetTickCount64();
	g_LastArenaRpcPresented = false;
}

static void MarkArenaPlacementRpcPresented(
	int32 placement, int32 pointsEarned)
{
	if (placement <= 0 || pointsEarned <= 0)
		return;
	g_LastArenaPresentedRpcPlacement = placement;
	g_LastArenaPresentedRpcPoints = pointsEarned;
	g_LastArenaPresentedRpcAt = GetTickCount64();
	g_LastArenaRpcPresented = true;
}

static void ResetArenaNativeVisualCorrelations(
	UWorld* world = nullptr,
	AFortPlayerControllerAthena* controller = nullptr)
{
	for (auto& observation : g_ArenaPlacementRpcObservations)
		observation = {};
	for (auto& observation : g_ArenaNativePointScoredObservations)
		observation = {};
	g_NextArenaPlacementRpcObservationSequence = 1;
	g_NextArenaNativePointScoredObservationSequence = 1;
	g_ArenaNativeVisualCorrelationWorld = world;
	g_ArenaNativeVisualCorrelationController = controller;
}

static void MaintainArenaNativeVisualCorrelationScope(
	UWorld* world,
	AFortPlayerControllerAthena* controller)
{
	if (world == g_ArenaNativeVisualCorrelationWorld &&
		controller == g_ArenaNativeVisualCorrelationController)
	{
		return;
	}
	ResetArenaNativeVisualCorrelations(world, controller);
}

static void MarkCorrelatedArenaNativeVisualPresented(
	AtlasArenaSessionBridgePolicy::EArenaPresentationEventKind kind,
	int32 absoluteStatValue,
	int32 placement,
	ULONGLONG now)
{
	using EKind =
		AtlasArenaSessionBridgePolicy::EArenaPresentationEventKind;
	if (kind == EKind::BusFare)
	{
		g_LastArenaBusFarePresentedAt = now;
	}
	else if (kind == EKind::Elimination)
	{
		g_LastArenaEliminationStatValue = absoluteStatValue;
		g_LastArenaEliminationPresentedAt = now;
	}
	else if (kind == EKind::Placement || kind == EKind::Victory)
	{
		MarkArenaPlacementPresented(placement);
	}
}

static void ResetPendingArenaVisualFallbacks()
{
	for (auto& pending : g_PendingArenaVisualFallbacks)
	{
		// ProcessEvent consumes the parameter storage synchronously. If a
		// refresh is requested reentrantly from that call, defer clearing this
		// one slot until dispatch returns so its owned FString cannot be
		// invalidated underneath the consumer.
		if (pending.Dispatching)
			pending.CancelAfterDispatch = true;
		else
			pending = {};
	}
	g_NextArenaVisualFallbackDispatchAt = 0;
}

static void ScheduleArenaVisualFallback(
	EArenaVisualTraceSchema schema,
	const void* parameters,
	uint32 parameterSize,
	ULONGLONG delayMilliseconds = 0,
	int32 previousPlacement = 0,
	int32 placement = 0,
	int32 pointsEarned = 0,
	bool acceptDedicatedRpcPresentation = false)
{
	if (schema != EArenaVisualTraceSchema::TournamentStat &&
		schema != EArenaVisualTraceSchema::ZeroParameters &&
		schema != EArenaVisualTraceSchema::TeamPlacement)
	{
		return;
	}
	if (schema == EArenaVisualTraceSchema::TeamPlacement)
	{
		for (auto& pending : g_PendingArenaVisualFallbacks)
		{
			if (pending.Active &&
				pending.Schema == schema &&
				pending.PreviousPlacement == previousPlacement &&
				pending.Placement == placement &&
				pending.PointsEarned == pointsEarned)
			{
				// The dedicated RPC and private bridge can both report the same
				// milestone. Keep one ordered repair and its earliest deadline.
				const ULONGLONG dueAt =
					GetTickCount64() + delayMilliseconds;
				if (dueAt < pending.DueAt)
					pending.DueAt = dueAt;
				pending.AcceptDedicatedRpcPresentation =
					pending.AcceptDedicatedRpcPresentation ||
					acceptDedicatedRpcPresentation;
				return;
			}
		}
	}
	if ((schema == EArenaVisualTraceSchema::TournamentStat &&
		 (parameterSize == 0 || parameterSize > 0x20 || !parameters)) ||
		(schema == EArenaVisualTraceSchema::ZeroParameters &&
		 (parameterSize != 0 || parameters)) ||
		(schema == EArenaVisualTraceSchema::TeamPlacement &&
		 (parameterSize != 0 || parameters || placement <= 0 ||
		  pointsEarned <= 0)))
	{
		return;
	}

	FPendingArenaVisualFallback* slot = nullptr;
	for (auto& pending : g_PendingArenaVisualFallbacks)
	{
		if (!pending.Active)
		{
			slot = &pending;
			break;
		}
	}
	if (!slot)
	{
		for (auto& pending : g_PendingArenaVisualFallbacks)
		{
			if (pending.Dispatching)
				continue;
			if (!slot || pending.Sequence < slot->Sequence)
				slot = &pending;
		}
		if (!slot)
		{
			AtlasDiagnostics::WriteLine(
				"arena-visual-fallback queue-overflow dropped-incoming");
			return;
		}
		AtlasDiagnostics::WriteLine(
			"arena-visual-fallback queue-overflow replaced=%llu",
			static_cast<unsigned long long>(slot->Sequence));
	}

	*slot = {};
	slot->Schema = schema;
	slot->ParameterSize = parameterSize;
	slot->PreviousPlacement = previousPlacement;
	slot->Placement = placement;
	slot->PointsEarned = pointsEarned;
	slot->AcceptDedicatedRpcPresentation =
		acceptDedicatedRpcPresentation;
	if (schema == EArenaVisualTraceSchema::TournamentStat &&
		!CopyOwnedArenaTournamentStatParameters(
			*slot, parameters, parameterSize))
	{
		AtlasDiagnostics::WriteLine(
			"arena-visual-fallback rejected unsafe-stat-payload "
			"bytes=%u",
			parameterSize);
		return;
	}
	// Maintenance runs after HUD repair in the same client tick. An event that
	// arrived just before the widget was created can therefore be presented
	// immediately, while later retries remain bounded and game-thread-only.
	slot->DueAt = GetTickCount64() + delayMilliseconds;
	slot->Sequence = g_NextArenaVisualFallbackSequence++;
	slot->Active = true;
}

static void ResetPendingArenaBridgeVisuals()
{
	for (auto& pending : g_PendingArenaBridgeVisuals)
	{
		if (pending.Dispatching)
			pending.CancelAfterDispatch = true;
		else
			pending = {};
	}
	g_NextArenaBridgeVisualDispatchAt = 0;
}

static void BeginArenaTerminalVisualDrain(
	std::uint64_t sessionOrdinal,
	ULONGLONG now)
{
	if (!sessionOrdinal)
		return;
	g_ArenaSessionBridge.TerminalVisualDrainDeadline = now + 4000;
	FPendingArenaBridgeVisual* finalTail = nullptr;
	for (auto& pending : g_PendingArenaBridgeVisuals)
	{
		if (!pending.Active || pending.SessionOrdinal != sessionOrdinal)
			continue;
		const bool candidateFinal =
			pending.Kind == AtlasArenaSessionBridgePolicy::
				EArenaPresentationEventKind::Victory ||
			pending.Kind == AtlasArenaSessionBridgePolicy::
				EArenaPresentationEventKind::Placement;
		if (candidateFinal &&
			(!finalTail || pending.EventSequence >
				finalTail->EventSequence))
		{
			finalTail = &pending;
		}
	}

	for (auto& pending : g_PendingArenaBridgeVisuals)
	{
		if (!pending.Active || pending.SessionOrdinal != sessionOrdinal ||
			&pending == finalTail)
		{
			continue;
		}
		if (pending.Dispatching)
			pending.CancelAfterDispatch = true;
		else
			pending = {};
	}
	if (finalTail && !finalTail->Dispatching)
	{
		finalTail->DueAt = (std::max)(
			now, g_NextArenaBridgeVisualDispatchAt);
		finalTail->RetryCount = 0;
	}
}

static bool HasPendingArenaBridgeVisualForSession(
	std::uint64_t sessionOrdinal)
{
	if (sessionOrdinal == 0)
		return false;
	for (const auto& pending : g_PendingArenaBridgeVisuals)
	{
		if (pending.Active &&
			pending.SessionOrdinal == sessionOrdinal)
		{
			return true;
		}
	}
	return false;
}

static bool HasDueArenaBridgeVisualForSession(
	std::uint64_t sessionOrdinal,
	ULONGLONG now)
{
	if (sessionOrdinal == 0 || now < g_NextArenaBridgeVisualDispatchAt)
		return false;
	for (const auto& pending : g_PendingArenaBridgeVisuals)
	{
		if (pending.Active && !pending.Dispatching &&
			pending.SessionOrdinal == sessionOrdinal &&
			now >= pending.DueAt)
		{
			return true;
		}
	}
	return false;
}

static void ReserveArenaBridgePresentationSpacing(
	ULONGLONG presentedAt)
{
	g_NextArenaBridgeVisualDispatchAt =
		AtlasArenaSessionBridgePolicy::
			ReserveVisualPresentationDeadline(
				g_NextArenaBridgeVisualDispatchAt,
				presentedAt);
}

static ULONGLONG GetArenaBridgeNativePresentationAt(
	AtlasArenaSessionBridgePolicy::EArenaPresentationEventKind kind,
	int32 absoluteStatValue,
	int32 placement)
{
	using EKind =
		AtlasArenaSessionBridgePolicy::EArenaPresentationEventKind;
	if (kind == EKind::BusFare)
		return g_LastArenaBusFarePresentedAt;
	if (kind == EKind::Elimination)
	{
		return absoluteStatValue > 0 &&
			g_LastArenaEliminationStatValue == absoluteStatValue
			? g_LastArenaEliminationPresentedAt
			: 0;
	}
	if ((kind == EKind::Placement || kind == EKind::Victory) &&
		placement > 0 &&
		g_LastArenaPresentedPlacement == placement)
	{
		return g_LastArenaPresentedPlacementAt;
	}
	return 0;
}

static bool WasArenaBridgeNativeRecentlyPresented(
	AtlasArenaSessionBridgePolicy::EArenaPresentationEventKind kind,
	int32 absoluteStatValue,
	int32 placement,
	ULONGLONG now)
{
	using EKind =
		AtlasArenaSessionBridgePolicy::EArenaPresentationEventKind;
	const ULONGLONG presentedAt = GetArenaBridgeNativePresentationAt(
		kind, absoluteStatValue, placement);
	if (!presentedAt || now < presentedAt)
		return false;
	if (kind == EKind::Placement || kind == EKind::Victory)
	{
		// The dedicated placement-points RPC is ambiguous on legacy builds:
		// 10.x also uses it as an elimination presentation. Only an exact
		// authoritative E/P/V correlation may let that RPC suppress a bridge
		// fallback. Typed placement consumers remain safe to deduplicate here.
		return now - presentedAt <=
			g_ArenaPlacementPresentationDedupeMs;
	}
	return now - presentedAt <= g_ArenaBridgeNativeDedupeMs;
}

static AtlasArenaNativeVisualCorrelationPolicy::ECorrelationDecision
	ClaimArenaPlacementRpcObservationForEvent(
		const AtlasArenaSessionBridgePolicy::FArenaPresentationEvent& event,
		UWorld* world,
		AFortPlayerControllerAthena* controller,
		ULONGLONG now,
		ULONGLONG* observedAt = nullptr)
{
	using namespace AtlasArenaNativeVisualCorrelationPolicy;
	if (observedAt)
		*observedAt = 0;
	if (!world || !controller || !event.NativeGraceExpected)
		return ECorrelationDecision::NoMatch;

	MaintainArenaNativeVisualCorrelationScope(world, controller);
	FAuthoritativeArenaVisualEvent candidate{};
	candidate.EventSequence = event.EventSequence;
	candidate.SessionOrdinal = event.SessionOrdinal;
	candidate.WorldIdentity =
		reinterpret_cast<std::uintptr_t>(world);
	candidate.ControllerIdentity =
		reinterpret_cast<std::uintptr_t>(controller);
	candidate.Kind = event.Kind;
	candidate.Placement = event.Placement;
	candidate.PointsDelta = event.PointsDelta;
	candidate.ReceivedAtMilliseconds = now;
	candidate.NativeGraceExpected = event.NativeGraceExpected;
	candidate.Active = true;

	ECorrelationDecision decision = ECorrelationDecision::NoMatch;
	const std::size_t index = SelectOldestCompatibleObservation(
		g_ArenaPlacementRpcObservations.data(),
		g_ArenaPlacementRpcObservations.size(),
		candidate, now,
		g_ArenaNativeVisualCorrelationMaximumAgeMs,
		&decision);
	if (index == NoCorrelation)
		return ECorrelationDecision::NoMatch;
	if (observedAt)
	{
		*observedAt = static_cast<ULONGLONG>(
			g_ArenaPlacementRpcObservations[index].
				ObservedAtMilliseconds);
	}
	g_ArenaPlacementRpcObservations[index].Active = false;
	return decision;
}

static UObject* GetArenaNativePointScoredCorrelationWidget(
	UWorld* world)
{
	if (!world)
		return nullptr;
	UObject* owned = g_ArenaOwnedScoringAlertForObserver.load(
		std::memory_order_acquire);
	UObject* active = g_ArenaLiveScoringAlert.load(
		std::memory_order_acquire);
	if (active && active != owned &&
		IsVerifiedNativeOwnedArenaScoringAlert(active))
	{
		return active;
	}

	UObject* observed = g_ArenaObservedNativeScoringAlert.load(
		std::memory_order_acquire);
	if (observed && observed != owned &&
		g_ArenaObservedNativeScoringAlertWorld.load(
			std::memory_order_acquire) == world &&
		g_ArenaObservedNativeScoringAlertSessionOrdinal.load(
			std::memory_order_acquire) ==
			g_ArenaSessionBridge.Session.SessionOrdinal &&
		IsTrustedArenaNativeScoringConsumer(observed))
	{
		return observed;
	}
	return nullptr;
}

static void AdvanceArenaNativePointScoredEventBarrier(
	std::uint64_t sessionOrdinal,
	UWorld* world,
	AFortPlayerControllerAthena* controller,
	UObject* nativeWidget,
	ULONGLONG receivedAt)
{
	using namespace AtlasArenaNativeVisualCorrelationPolicy;
	if (!sessionOrdinal || !world || !controller || !nativeWidget ||
		!receivedAt)
	{
		return;
	}

	FNativePointScoredEvent barrier{};
	barrier.SessionOrdinal = sessionOrdinal;
	barrier.WorldIdentity = reinterpret_cast<std::uintptr_t>(world);
	barrier.ControllerIdentity =
		reinterpret_cast<std::uintptr_t>(controller);
	barrier.NativeWidgetIdentity =
		reinterpret_cast<std::uintptr_t>(nativeWidget);
	barrier.ReceivedAtMilliseconds = receivedAt;
	for (auto& observation : g_ArenaNativePointScoredObservations)
	{
		if (IsNativePointScoredObservationBehindEventBarrier(
				observation, barrier))
		{
			observation.Active = false;
		}
	}
}

static bool ClaimArenaNativePointScoredObservationForEvent(
	const AtlasArenaSessionBridgePolicy::FArenaPresentationEvent& event,
	UWorld* world,
	AFortPlayerControllerAthena* controller,
	ULONGLONG now,
	ULONGLONG* observedAt = nullptr)
{
	using namespace AtlasArenaNativeVisualCorrelationPolicy;
	if (observedAt)
		*observedAt = 0;
	if (!world || !controller || !event.NativeGraceExpected)
		return false;

	UObject* nativeWidget =
		GetArenaNativePointScoredCorrelationWidget(world);
	if (!nativeWidget)
		return false;
	MaintainArenaNativeVisualCorrelationScope(world, controller);

	FNativePointScoredEvent candidate{};
	candidate.EventSequence = event.EventSequence;
	candidate.SessionOrdinal = event.SessionOrdinal;
	candidate.WorldIdentity = reinterpret_cast<std::uintptr_t>(world);
	candidate.ControllerIdentity =
		reinterpret_cast<std::uintptr_t>(controller);
	candidate.NativeWidgetIdentity =
		reinterpret_cast<std::uintptr_t>(nativeWidget);
	candidate.Kind = event.Kind;
	candidate.PointsDelta = event.PointsDelta;
	candidate.ReceivedAtMilliseconds = now;
	candidate.NativeGraceExpected = event.NativeGraceExpected;
	candidate.Active = true;

	const std::size_t index = SelectNewestNativePointScoredObservation(
		g_ArenaNativePointScoredObservations.data(),
		g_ArenaNativePointScoredObservations.size(),
		candidate, now,
		g_ArenaNativePointScoredCorrelationMaximumAgeMs);
	if (index == NoCorrelation)
		return false;
	if (observedAt)
	{
		*observedAt = static_cast<ULONGLONG>(
			g_ArenaNativePointScoredObservations[index].
				ObservedAtMilliseconds);
	}
	// The server invokes native gameplay first and sends this ordered event
	// afterward. Retire every older callback in the exact same scope so a
	// duplicate/internal animation callback cannot be recycled by a later kill.
	AdvanceArenaNativePointScoredEventBarrier(
		event.SessionOrdinal, world, controller, nativeWidget, now);
	return true;
}

static void ScheduleArenaBridgeVisual(
	const AtlasArenaSessionBridgePolicy::FArenaPresentationEvent& event,
	int32 previousPlacement,
	int32 absoluteStatValue)
{
	if (event.SessionOrdinal == 0 || event.EventSequence == 0)
	{
		return;
	}

	const ULONGLONG now = GetTickCount64();
	UWorld* world = UWorld::GetWorld();
	AFortPlayerControllerAthena* controller =
		GetLocalFortPlayerController();
	if (event.PointsDelta == 0)
	{
		if (event.Kind == AtlasArenaSessionBridgePolicy::
				EArenaPresentationEventKind::BusFare)
		{
			// B:0 is a lifecycle/control frame, not a badge. It still advances
			// the native callback barrier so match-entry activity can never be
			// reused as evidence for the first elimination.
			AdvanceArenaNativePointScoredEventBarrier(
				event.SessionOrdinal, world, controller,
				GetArenaNativePointScoredCorrelationWidget(world), now);
		}
		return;
	}
	UObject* activeAlert = g_ArenaLiveScoringAlert.load(
		std::memory_order_acquire);
	const bool verifiedClientOwnedFallback =
		IsVerifiedClientOwnedArenaScoringAlert(activeAlert);
	// Exact native observations are stronger than the cached HUD owner. A
	// native callback can reveal the authored widget while the compatibility
	// widget is still cached for the few milliseconds before game-thread
	// adoption. Keep correlation eligible from the server capability bit; only
	// passive waiting is disabled by an already-owned fallback.
	const bool exactNativeCorrelationExpected =
		event.NativeGraceExpected;
	const bool initialNativeGrace =
		AtlasArenaSessionBridgePolicy::ShouldUseNativeVisualGrace(
			event.NativeGraceExpected,
			false,
			verifiedClientOwnedFallback);
	auto correlationEvent = event;
	correlationEvent.NativeGraceExpected =
		exactNativeCorrelationExpected;
	ULONGLONG correlatedNativePresentedAt = 0;
	const auto correlation = ClaimArenaPlacementRpcObservationForEvent(
		correlationEvent, world, controller, now,
		&correlatedNativePresentedAt);
	using ECorrelationDecision =
		AtlasArenaNativeVisualCorrelationPolicy::ECorrelationDecision;
	bool nativeWasObservedSilent = correlation ==
		ECorrelationDecision::DispatchFallbackNativeSilent;
	if (correlation ==
		ECorrelationDecision::SuppressFallbackNativePresented)
	{
		if (AtlasArenaSessionBridgePolicy::
				CanNativeVisualOwnBridgeEvent(
					exactNativeCorrelationExpected,
					correlatedNativePresentedAt,
					g_NextArenaBridgeVisualDispatchAt))
		{
			MarkCorrelatedArenaNativeVisualPresented(
				event.Kind, absoluteStatValue, event.Placement,
				correlatedNativePresentedAt);
			ReserveArenaBridgePresentationSpacing(
				correlatedNativePresentedAt);
			AtlasDiagnostics::WriteLine(
				"arena-session visual-correlated-native session=%llu "
				"sequence=%llu kind=%lc",
				static_cast<unsigned long long>(event.SessionOrdinal),
				static_cast<unsigned long long>(event.EventSequence),
				static_cast<wchar_t>(event.Kind));
			return;
		}
		// The native call occurred inside another badge's protected window.
		// Its stat mutation is real, but the bridge must still own the visual.
		nativeWasObservedSilent = true;
	}
	if (correlation == ECorrelationDecision::NoMatch)
	{
		ULONGLONG genericNativePresentedAt = 0;
		const bool genericNativeClaimed =
			ClaimArenaNativePointScoredObservationForEvent(
				correlationEvent, world, controller, now,
				&genericNativePresentedAt);
		if (genericNativeClaimed &&
			AtlasArenaSessionBridgePolicy::CanNativeVisualOwnBridgeEvent(
				exactNativeCorrelationExpected,
				genericNativePresentedAt,
				g_NextArenaBridgeVisualDispatchAt))
		{
			MarkCorrelatedArenaNativeVisualPresented(
				event.Kind, absoluteStatValue, event.Placement,
				genericNativePresentedAt);
			ReserveArenaBridgePresentationSpacing(
				genericNativePresentedAt);
			AtlasDiagnostics::WriteLine(
				"arena-session visual-correlated-native-point-scored "
				"session=%llu sequence=%llu kind=%lc age-ms=%llu",
				static_cast<unsigned long long>(event.SessionOrdinal),
				static_cast<unsigned long long>(event.EventSequence),
				static_cast<wchar_t>(event.Kind),
				static_cast<unsigned long long>(
					now - genericNativePresentedAt));
			return;
		}
		if (genericNativeClaimed)
			nativeWasObservedSilent = true;
	}
	const bool effectiveNativeGrace =
		AtlasArenaSessionBridgePolicy::ShouldUseNativeVisualGrace(
			event.NativeGraceExpected,
			nativeWasObservedSilent,
			verifiedClientOwnedFallback);
	const ULONGLONG nativePresentedAt =
		GetArenaBridgeNativePresentationAt(
			event.Kind, absoluteStatValue, event.Placement);
	if (!nativeWasObservedSilent &&
		WasArenaBridgeNativeRecentlyPresented(
			event.Kind, absoluteStatValue, event.Placement, now) &&
		AtlasArenaSessionBridgePolicy::CanNativeVisualOwnBridgeEvent(
			exactNativeCorrelationExpected,
			nativePresentedAt,
			g_NextArenaBridgeVisualDispatchAt))
	{
		ReserveArenaBridgePresentationSpacing(
			nativePresentedAt);
		AtlasDiagnostics::WriteLine(
			"arena-session visual-owned-by-native session=%llu "
			"sequence=%llu kind=%lc",
			static_cast<unsigned long long>(event.SessionOrdinal),
			static_cast<unsigned long long>(event.EventSequence),
			static_cast<wchar_t>(event.Kind));
		return;
	}

	for (const auto& pending : g_PendingArenaBridgeVisuals)
	{
		if (pending.Active &&
			pending.SessionOrdinal == event.SessionOrdinal &&
			pending.EventSequence == event.EventSequence)
		{
			return;
		}
	}

	FPendingArenaBridgeVisual* slot = nullptr;
	for (auto& pending : g_PendingArenaBridgeVisuals)
	{
		if (!pending.Active)
		{
			slot = &pending;
			break;
		}
	}
	if (!slot)
	{
		for (auto& pending : g_PendingArenaBridgeVisuals)
		{
			if (!pending.Dispatching &&
				(!slot || pending.EventSequence < slot->EventSequence))
			{
				slot = &pending;
			}
		}
		if (!slot)
		{
			AtlasDiagnostics::WriteLine(
				"arena-session visual-queue-overflow dropped-incoming");
			return;
		}
	}

	*slot = {};
	slot->World = world;
	slot->Controller = controller;
	slot->SessionOrdinal = event.SessionOrdinal;
	slot->EventSequence = event.EventSequence;
	slot->Kind = event.Kind;
	slot->PointsDelta = event.PointsDelta;
	slot->ResultingHype = event.ResultingHype;
	slot->PreviousPlacement = previousPlacement;
	slot->Placement = event.Placement;
	slot->AbsoluteStatValue = absoluteStatValue;
	slot->ReceivedAt = now;
	slot->NativeCapabilityExpected = event.NativeGraceExpected;
	slot->NativeGraceExpected = effectiveNativeGrace;
	slot->DueAt = now +
		AtlasArenaSessionBridgePolicy::
			NativeVisualFallbackDelayMilliseconds(
				slot->Kind, slot->NativeGraceExpected);
	slot->Active = true;
}

static void ResetArenaSessionBridgeForWorld(UWorld* world)
{
	if (g_ArenaSessionBridge.World == world)
		return;
	ResetPendingArenaBridgeVisuals();
	ResetArenaNativeVisualCorrelations(world, nullptr);
	g_ArenaSessionBridge = {};
	g_ArenaSessionBridge.World = world;
	g_LastArenaEliminationPresentedAt = 0;
	g_LastArenaEliminationStatValue = 0;
	g_LastArenaBusFarePresentedAt = 0;
}

static void MarkArenaSessionScoreRepairPending()
{
	g_ArenaSessionBridge.ScoreRepairPending = true;
	g_ArenaSessionBridge.ScoreRepairAttempts = 0;
	g_ArenaSessionBridge.ScoreRepairSuccesses = 0;
	g_ArenaSessionBridge.NextScoreRepairAt = 0;
}

static void MarkArenaNativeHudOwnerForActiveSession()
{
	auto& bridge = g_ArenaSessionBridge;
	if (!bridge.Session.Active || bridge.Session.SessionOrdinal == 0)
		return;
	bridge.NativeHudOwnerSessionOrdinal =
		bridge.Session.SessionOrdinal;
	bridge.OwnedFallbackOpportunityReached = false;
}

static bool HandleArenaSessionBridgeMessage(
	const wchar_t* text, int textLength)
{
	if (!text || textLength <= 0)
		return false;

	using namespace AtlasArenaSessionBridgePolicy;
	FArenaBootstrap bootstrap{};
	if (TryParseBootstrapMessageV1(
			text, static_cast<std::size_t>(textLength),
			&bootstrap))
	{
		UWorld* world = UWorld::GetWorld();
		if (!world || IsFrontendWorld(world))
		{
			// A valid private frame is always swallowed, even if it arrived on a
			// travel boundary where no gameplay world can safely own it.
			return true;
		}

		ResetArenaSessionBridgeForWorld(world);
		const std::uint64_t previousSession =
			g_ArenaSessionBridge.Session.SessionOrdinal;
		const auto applied = ApplyBootstrap(
			&g_ArenaSessionBridge.Session, bootstrap);
		if (applied == EBootstrapApplyResult::Rejected)
			return true;
		if (previousSession != bootstrap.SessionOrdinal)
		{
			ResetArenaObservedNativeCandidate();
			g_ArenaSessionBridge.TerminalSessionOrdinal = 0;
			g_ArenaSessionBridge.OwnedFallbackSuppressedSessionOrdinal = 0;
			g_ArenaSessionBridge.NativeHudOwnerSessionOrdinal = 0;
			g_ArenaSessionBridge.OwnedFallbackOpportunityReached = false;
			g_ArenaSessionBridge.OwnedFallbackRetireAt = 0;
			g_ArenaSessionBridge.TerminalVisualDrainDeadline = 0;
			ResetPendingArenaBridgeVisuals();
			ResetPendingArenaVisualFallbacks();
			ResetArenaNativeVisualCorrelations(
				world, GetLocalFortPlayerController());
			g_LastArenaEliminationPresentedAt = 0;
			g_LastArenaEliminationStatValue = 0;
			g_LastArenaBusFarePresentedAt = 0;
			g_LastArenaPresentedPlacement = 0;
			g_LastArenaPresentedPlacementAt = 0;
			g_LastArenaPresentedRpcPlacement = 0;
			g_LastArenaPresentedRpcPoints = 0;
			g_LastArenaPresentedRpcAt = 0;
			g_LastArenaRpcPresented = false;
			RequestArenaScoringHudRefresh();
		}
		MarkArenaSessionScoreRepairPending();
		AtlasDiagnostics::WriteLine(
			"arena-session bootstrap session=%llu saving=%d "
			"starting-hype=%d fee=%d result=%s",
			static_cast<unsigned long long>(
				bootstrap.SessionOrdinal),
			bootstrap.SavingEnabled ? 1 : 0,
			bootstrap.StartingHype,
			bootstrap.ArenaEntryFee,
			applied == EBootstrapApplyResult::Initialized
				? "initialized"
				: "refreshed");
		return true;
	}

	FArenaEnd end{};
	if (TryParseEndMessageV1(
			text, static_cast<std::size_t>(textLength), &end))
	{
		UWorld* world = UWorld::GetWorld();
		if (!world || IsFrontendWorld(world) ||
			g_ArenaSessionBridge.World != world ||
			!CanApplyEnd(g_ArenaSessionBridge.Session, end))
		{
			// Valid private lifecycle frames never escape to Fortnite's chat
			// presentation, including stale frames observed during travel.
			return true;
		}

		if (g_ArenaSessionBridge.TerminalSessionOrdinal !=
			end.SessionOrdinal)
		{
			g_ArenaSessionBridge.TerminalSessionOrdinal =
				end.SessionOrdinal;
			g_ArenaSessionBridge.OwnedFallbackRetireAt = 0;
		}
		BeginArenaTerminalVisualDrain(
			end.SessionOrdinal, GetTickCount64());
		AtlasDiagnostics::WriteLine(
			"arena-session end-accepted session=%llu",
			static_cast<unsigned long long>(end.SessionOrdinal));
		return true;
	}

	FArenaPresentationEvent event{};
	if (!TryParseEventMessageV1(
			text, static_cast<std::size_t>(textLength), &event))
	{
		return false;
	}

	UWorld* world = UWorld::GetWorld();
	if (!world || IsFrontendWorld(world) ||
		g_ArenaSessionBridge.World != world)
	{
		return true;
	}

	int32 previousPlacement = 0;
	if (!ApplyPresentationEvent(
			&g_ArenaSessionBridge.Session,
			event, &previousPlacement))
	{
		AtlasDiagnostics::WriteLine(
			"arena-session event-rejected session=%llu sequence=%llu",
			static_cast<unsigned long long>(event.SessionOrdinal),
			static_cast<unsigned long long>(event.EventSequence));
		return true;
	}

	const int32 absoluteStatValue =
		event.Kind == EArenaPresentationEventKind::Elimination
			? g_ArenaSessionBridge.Session.EliminationCount
			: (event.Placement > 0 ? event.Placement : 1);
	if (event.Kind == EArenaPresentationEventKind::BusFare)
	{
		// Only score-neutral B:0 needs a one-shot HUD construction opportunity.
		// A nonzero fare already owns a pending visual when native presentation
		// is unavailable, so latching that event would permit a retired fallback
		// to reappear later without a new visual.
		g_ArenaSessionBridge.OwnedFallbackOpportunityReached =
			ShouldOpenScoreNeutralHudOpportunity(
				event.Kind, event.PointsDelta,
				event.NativeGraceExpected);
	}
	if (event.Kind == EArenaPresentationEventKind::Victory)
	{
		if (g_ArenaSessionBridge.TerminalSessionOrdinal !=
			event.SessionOrdinal)
		{
			g_ArenaSessionBridge.TerminalSessionOrdinal =
				event.SessionOrdinal;
			g_ArenaSessionBridge.OwnedFallbackRetireAt = 0;
		}
	}
	ScheduleArenaBridgeVisual(
		event, previousPlacement, absoluteStatValue);
	if (event.Kind == EArenaPresentationEventKind::Victory)
	{
		BeginArenaTerminalVisualDrain(
			event.SessionOrdinal, GetTickCount64());
	}
	MarkArenaSessionScoreRepairPending();
	AtlasDiagnostics::WriteLine(
		"arena-session event-accepted session=%llu sequence=%llu "
		"kind=%lc delta=%d total=%d placement=%d native-grace=%d",
		static_cast<unsigned long long>(event.SessionOrdinal),
		static_cast<unsigned long long>(event.EventSequence),
		static_cast<wchar_t>(event.Kind),
		event.PointsDelta, event.ResultingHype,
		event.Placement,
		event.NativeGraceExpected ? 1 : 0);
	return true;
}

static bool HandleArenaPlacementBridgeMessage(
	const wchar_t* text, int textLength)
{
	if (!text || textLength <= 0)
		return false;

	int32 placement = 0;
	int32 previousPlacement = 0;
	int32 pointsEarned = 0;
	bool dedicatedRpcExpected = false;
	const bool versionTwo =
		AtlasArenaVisualTracePolicy::TryParsePlacementBridgeMessageV2(
			text, static_cast<std::size_t>(textLength),
			&previousPlacement, &placement,
			&pointsEarned, &dedicatedRpcExpected);
	if (!versionTwo &&
		!AtlasArenaVisualTracePolicy::TryParsePlacementBridgeMessage(
			text, static_cast<std::size_t>(textLength),
			&placement, &pointsEarned, &dedicatedRpcExpected))
	{
		return false;
	}

	const ULONGLONG now = GetTickCount64();
	const bool nativeAlreadyPresented =
		WasArenaPlacementRecentlyPresented(placement, now);
	const bool dedicatedRpcAlreadyPresented =
		dedicatedRpcExpected &&
		WasArenaPlacementRpcRecentlyPresented(
			placement, pointsEarned, now);
	if (!nativeAlreadyPresented && !dedicatedRpcAlreadyPresented)
	{
		ScheduleArenaVisualFallback(
			EArenaVisualTraceSchema::TeamPlacement,
			nullptr, 0,
			g_ArenaPlacementFallbackGraceMs,
			previousPlacement,
			placement,
			pointsEarned,
			dedicatedRpcExpected);
	}
	static std::atomic_uint32_t logged = 0;
	if (logged.fetch_add(1, std::memory_order_relaxed) < 128)
	{
		AtlasDiagnostics::WriteLine(
			"arena-visual-bridge version=%d previous=%d "
			"placement=%d points=%d dedicated-rpc=%d result=%s",
			versionTwo ? 2 : 1,
			previousPlacement, placement, pointsEarned,
			dedicatedRpcExpected ? 1 : 0,
			nativeAlreadyPresented
				? "native-already-presented"
				: (dedicatedRpcAlreadyPresented
					? "rpc-already-presented"
					: "scheduled"));
	}
	return true;
}

static void ConsumeOldestPendingArenaVisualFallback(
	EArenaVisualTraceSchema schema)
{
	FPendingArenaVisualFallback* oldest = nullptr;
	for (auto& pending : g_PendingArenaVisualFallbacks)
	{
		if (!pending.Active || pending.Schema != schema)
			continue;
		if (!oldest || pending.Sequence < oldest->Sequence)
			oldest = &pending;
	}
	if (oldest)
		oldest->Active = false;
}

static void ConsumePendingArenaTeamPlacementFallback(
	int32 placement)
{
	if (placement <= 0)
		return;

	FPendingArenaVisualFallback* oldest = nullptr;
	for (auto& pending : g_PendingArenaVisualFallbacks)
	{
		if (!pending.Active ||
			pending.Schema != EArenaVisualTraceSchema::TeamPlacement ||
			pending.Placement != placement)
		{
			continue;
		}
		if (!oldest || pending.Sequence < oldest->Sequence)
			oldest = &pending;
	}
	if (oldest)
		oldest->Active = false;
}

static void ConsumePendingArenaBridgeVisual(
	AtlasArenaSessionBridgePolicy::EArenaPresentationEventKind kind,
	int32 placement = 0,
	int32 pointsDelta = 0,
	ULONGLONG presentedAt = 0)
{
	if (!presentedAt)
		presentedAt = GetTickCount64();
	FPendingArenaBridgeVisual* oldest = nullptr;
	for (auto& pending : g_PendingArenaBridgeVisuals)
	{
		if (!pending.Active || pending.Kind != kind ||
			!AtlasArenaSessionBridgePolicy::
				CanNativeVisualOwnBridgeEvent(
					pending.NativeCapabilityExpected,
					presentedAt,
					g_NextArenaBridgeVisualDispatchAt) ||
			(placement > 0 && pending.Placement != placement) ||
			(pointsDelta != 0 &&
			 pending.PointsDelta != pointsDelta))
		{
			continue;
		}
		if (!oldest || pending.EventSequence < oldest->EventSequence)
			oldest = &pending;
	}
	if (!oldest)
		return;
	ReserveArenaBridgePresentationSpacing(presentedAt);
	if (oldest->Dispatching)
		oldest->CancelAfterDispatch = true;
	else
		*oldest = {};
}

static void RecordArenaNativePointScoredObservation(UObject* context)
{
	using namespace AtlasArenaNativeVisualCorrelationPolicy;
	UWorld* world = UWorld::GetWorld();
	AFortPlayerControllerAthena* controller =
		GetLocalFortPlayerController();
	UObject* owned = g_ArenaOwnedScoringAlertForObserver.load(
		std::memory_order_acquire);
	if (!world || !controller || !context || context == owned ||
		world != g_ArenaSessionBridge.World ||
		!g_ArenaSessionBridge.Session.Active ||
		!IsTrustedArenaNativeScoringConsumer(context))
	{
		return;
	}

	MaintainArenaNativeVisualCorrelationScope(world, controller);
	FNativePointScoredObservation* slot = nullptr;
	for (auto& observation : g_ArenaNativePointScoredObservations)
	{
		if (!observation.Active)
		{
			slot = &observation;
			break;
		}
	}
	if (!slot)
	{
		for (auto& observation : g_ArenaNativePointScoredObservations)
		{
			if (!slot || observation.ObservationSequence <
					slot->ObservationSequence)
			{
				slot = &observation;
			}
		}
	}
	if (!slot)
		return;

	*slot = {};
	slot->ObservationSequence =
		g_NextArenaNativePointScoredObservationSequence++;
	slot->SessionOrdinal =
		g_ArenaSessionBridge.Session.SessionOrdinal;
	slot->WorldIdentity = reinterpret_cast<std::uintptr_t>(world);
	slot->ControllerIdentity =
		reinterpret_cast<std::uintptr_t>(controller);
	slot->NativeWidgetIdentity =
		reinterpret_cast<std::uintptr_t>(context);
	const ULONGLONG now = GetTickCount64();
	slot->ObservedAtMilliseconds = now;
	slot->Active = true;

	AtlasDiagnostics::WriteLine(
		"arena-visual-trace native-point-scored-observed "
		"session=%llu observation=%llu widget=%p",
		static_cast<unsigned long long>(slot->SessionOrdinal),
		static_cast<unsigned long long>(slot->ObservationSequence),
		context);

	// Reliable native/bridge ordering is normally callback then frame, but a
	// version may defer its UI consumer until just after the private frame is
	// decoded. Claim the oldest pending B/E event in that same exact scope so
	// the one-second fallback cannot replay an already presented native badge.
	FPendingArenaBridgeVisual* matched = nullptr;
	for (auto& pending : g_PendingArenaBridgeVisuals)
	{
		if (!pending.Active || pending.Dispatching)
			continue;
		FNativePointScoredEvent event{};
		event.EventSequence = pending.EventSequence;
		event.SessionOrdinal = pending.SessionOrdinal;
		event.WorldIdentity = reinterpret_cast<std::uintptr_t>(
			pending.World);
		event.ControllerIdentity = reinterpret_cast<std::uintptr_t>(
			pending.Controller);
		event.NativeWidgetIdentity =
			reinterpret_cast<std::uintptr_t>(context);
		event.Kind = pending.Kind;
		event.PointsDelta = pending.PointsDelta;
		event.ReceivedAtMilliseconds = pending.ReceivedAt;
		// An exact callback from a distinct native widget can supersede a cached
		// compatibility widget. Preserve the raw server capability for this
		// correlation; NativeGraceExpected only controls passive waiting.
		event.NativeGraceExpected = pending.NativeCapabilityExpected;
		event.Active = true;
		if (!IsNativePointScoredReverseCorrelationMatch(
				*slot, event, now,
				g_ArenaNativePointScoredCorrelationMaximumAgeMs))
		{
			continue;
		}
		if (!matched || pending.EventSequence < matched->EventSequence)
			matched = &pending;
	}
	if (!matched)
		return;

	const std::uint64_t matchedSession = matched->SessionOrdinal;
	const std::uint64_t matchedSequence = matched->EventSequence;
	const auto matchedKind = matched->Kind;
	AdvanceArenaNativePointScoredEventBarrier(
		matchedSession, world, controller, context, now);
	if (AtlasArenaSessionBridgePolicy::CanNativeVisualOwnBridgeEvent(
			matched->NativeCapabilityExpected,
			now, g_NextArenaBridgeVisualDispatchAt))
	{
		MarkCorrelatedArenaNativeVisualPresented(
			matchedKind, matched->AbsoluteStatValue,
			matched->Placement, now);
		ReserveArenaBridgePresentationSpacing(now);
		*matched = {};
		AtlasDiagnostics::WriteLine(
			"arena-session visual-correlated-native-point-scored-reverse "
			"session=%llu sequence=%llu kind=%lc",
			static_cast<unsigned long long>(matchedSession),
			static_cast<unsigned long long>(matchedSequence),
			static_cast<wchar_t>(matchedKind));
		return;
	}

	// The callback landed inside another badge's protected window. It remains
	// an ordering barrier, but this event still needs a bridge-owned visual as
	// soon as the presentation spacing permits.
	matched->NativeGraceExpected = false;
	matched->DueAt = now;
	matched->RetryCount = 0;
	AtlasDiagnostics::WriteLine(
		"arena-session visual-correlated-native-point-scored-reverse-silent "
		"session=%llu sequence=%llu kind=%lc",
		static_cast<unsigned long long>(matchedSession),
		static_cast<unsigned long long>(matchedSequence),
		static_cast<wchar_t>(matchedKind));
}

static void RecordArenaPlacementRpcObservation(
	int32 placement,
	int32 pointsEarned,
	bool pointScoredPresented,
	bool teamPlacementConsumerAdvanced)
{
	using namespace AtlasArenaNativeVisualCorrelationPolicy;
	UWorld* world = UWorld::GetWorld();
	AFortPlayerControllerAthena* controller =
		GetLocalFortPlayerController();
	if (!world || !controller ||
		world != g_ArenaSessionBridge.World ||
		!g_ArenaSessionBridge.Session.Active ||
		placement <= 0 || placement > 100 || pointsEarned <= 0)
	{
		return;
	}

	MaintainArenaNativeVisualCorrelationScope(world, controller);
	FPlacementPointsRpcObservation* slot = nullptr;
	for (auto& observation : g_ArenaPlacementRpcObservations)
	{
		if (!observation.Active)
		{
			slot = &observation;
			break;
		}
	}
	if (!slot)
	{
		for (auto& observation : g_ArenaPlacementRpcObservations)
		{
			if (!slot || observation.ObservationSequence <
					slot->ObservationSequence)
			{
				slot = &observation;
			}
		}
	}
	if (!slot)
		return;

	const ULONGLONG now = GetTickCount64();
	const bool pointScoredOwnedVisual = pointScoredPresented &&
		AtlasArenaSessionBridgePolicy::CanNativeVisualOwnBridgeEvent(
			true, now, g_NextArenaBridgeVisualDispatchAt);
	*slot = {};
	slot->ObservationSequence =
		g_NextArenaPlacementRpcObservationSequence++;
	slot->SessionOrdinal =
		g_ArenaSessionBridge.Session.SessionOrdinal;
	slot->WorldIdentity = reinterpret_cast<std::uintptr_t>(world);
	slot->ControllerIdentity =
		reinterpret_cast<std::uintptr_t>(controller);
	slot->Placement = placement;
	slot->PointsEarned = pointsEarned;
	slot->ObservedAtMilliseconds = now;
	slot->PointScoredPresented = pointScoredOwnedVisual;
	slot->TeamPlacementConsumerAdvanced =
		teamPlacementConsumerAdvanced;
	slot->Active = true;

	FPendingArenaBridgeVisual* matched = nullptr;
	ECorrelationDecision decision = ECorrelationDecision::NoMatch;
	for (auto& pending : g_PendingArenaBridgeVisuals)
	{
		if (!pending.Active || pending.Dispatching)
			continue;
		FAuthoritativeArenaVisualEvent event{};
		event.EventSequence = pending.EventSequence;
		event.SessionOrdinal = pending.SessionOrdinal;
		event.WorldIdentity = reinterpret_cast<std::uintptr_t>(
			pending.World);
		event.ControllerIdentity = reinterpret_cast<std::uintptr_t>(
			pending.Controller);
		event.Kind = pending.Kind;
		event.Placement = pending.Placement;
		event.PointsDelta = pending.PointsDelta;
		event.ReceivedAtMilliseconds = pending.ReceivedAt;
		event.NativeGraceExpected = pending.NativeCapabilityExpected;
		event.Active = true;
		const auto candidateDecision = EvaluateCorrelation(
			*slot, event, now,
			g_ArenaNativeVisualCorrelationMaximumAgeMs);
		if (candidateDecision == ECorrelationDecision::NoMatch)
			continue;
		if (!matched || pending.EventSequence <
				matched->EventSequence)
		{
			matched = &pending;
			decision = candidateDecision;
		}
	}

	if (!matched)
		return;
	slot->Active = false;
	if (decision ==
			ECorrelationDecision::SuppressFallbackNativePresented &&
		AtlasArenaSessionBridgePolicy::CanNativeVisualOwnBridgeEvent(
			matched->NativeCapabilityExpected,
			now,
			g_NextArenaBridgeVisualDispatchAt))
	{
		MarkCorrelatedArenaNativeVisualPresented(
			matched->Kind, matched->AbsoluteStatValue,
			matched->Placement, now);
		ReserveArenaBridgePresentationSpacing(now);
		AtlasDiagnostics::WriteLine(
			"arena-session visual-correlated-native-reverse "
			"session=%llu sequence=%llu kind=%lc",
			static_cast<unsigned long long>(matched->SessionOrdinal),
			static_cast<unsigned long long>(matched->EventSequence),
			static_cast<wchar_t>(matched->Kind));
		*matched = {};
		return;
	}

	// The native RPC ran but its active scoring widget did not present, or it
	// ran inside another badge's reserved presentation window. It is an
	// ordered barrier, so bypass the remaining grace period rather than waiting
	// or allowing a later same-valued RPC to claim this event.
	matched->NativeGraceExpected = false;
	matched->DueAt = now;
	matched->RetryCount = 0;
	AtlasDiagnostics::WriteLine(
		"arena-session visual-correlated-silent-reverse session=%llu "
		"sequence=%llu kind=%lc",
		static_cast<unsigned long long>(matched->SessionOrdinal),
		static_cast<unsigned long long>(matched->EventSequence),
		static_cast<wchar_t>(matched->Kind));
}

static void MaintainPendingArenaVisualFallbacks(ULONGLONG now)
{
	if (now < g_NextArenaVisualFallbackDispatchAt)
		return;

	FPendingArenaVisualFallback* next = nullptr;
	for (auto& pending : g_PendingArenaVisualFallbacks)
	{
		if (!pending.Active || pending.Dispatching)
			continue;
		if (!next || pending.Sequence < next->Sequence)
			next = &pending;
	}
	if (!next || now < next->DueAt)
		return;

	auto& pending = *next;
	const uint64 sequence = pending.Sequence;
	pending.Dispatching = true;
	const char* consumerName = nullptr;
	bool dispatched = false;
	if (pending.Schema == EArenaVisualTraceSchema::TournamentStat)
	{
		consumerName = "OnTournamentStatUpdate";
		dispatched = TryDispatchArenaTournamentStatPresentation(
			pending.Parameters.data(), pending.ParameterSize);
	}
	else if (pending.Schema == EArenaVisualTraceSchema::ZeroParameters)
	{
		consumerName = "OnPointScored";
		dispatched = TryDispatchArenaPointScoredPresentation();
	}
	else if (pending.Schema == EArenaVisualTraceSchema::TeamPlacement)
	{
		// Transitional servers can emit both the legacy placement marker and
		// the authoritative session event. If the latter already presented this
		// exact milestone, consume the legacy fallback without replaying it.
		if (WasArenaPlacementRecentlyPresented(
				pending.Placement, now))
		{
			consumerName = "already-presented";
			dispatched = true;
		}
		else
		{
			const auto transition = pending.PreviousPlacement > 0
				? TryDispatchArenaPlacementTournamentStatTransition(
					pending.PreviousPlacement, pending.Placement)
				: EArenaPlacementStatTransitionResult::Unavailable;
			if (transition !=
				EArenaPlacementStatTransitionResult::Unavailable)
			{
				consumerName = "OnTournamentStatUpdate(prime-target)";
				// A valid typed transition is a one-shot operation. Retrying it
				// would rewind the widget's old-value map and could duplicate a
				// presentation that completed synchronously.
				dispatched = true;
				if (transition ==
					EArenaPlacementStatTransitionResult::Presented)
				{
					MarkArenaPlacementPresented(pending.Placement);
				}
				AtlasDiagnostics::WriteLine(
					"arena-visual-placement-transition previous=%d "
						"placement=%d points=%d result=%s",
					pending.PreviousPlacement,
					pending.Placement,
					pending.PointsEarned,
					transition ==
						EArenaPlacementStatTransitionResult::Presented
							? "presented"
							: (transition ==
								EArenaPlacementStatTransitionResult::Silent
									? "silent"
									: (transition ==
										EArenaPlacementStatTransitionResult::
											PrimePresented
											? "prime-presented"
											: "multiple-presentations")));
			}
			else
			{
				consumerName = "OnTeamPlacementChanged";
				bool pointScored = false;
				dispatched = TryDispatchArenaTeamPlacementPresentation(
					pending.Placement, &pointScored);
				if (dispatched && pointScored)
					MarkArenaPlacementPresented(pending.Placement);
			}
		}
	}

	// A reentrant world/HUD reset may have cancelled this entry. Its storage
	// was deliberately retained through ProcessEvent and can now be cleared.
	if (pending.Sequence != sequence)
		return;
	pending.Dispatching = false;
	if (pending.CancelAfterDispatch)
	{
		pending = {};
		return;
	}
	g_NextArenaVisualFallbackDispatchAt = now + 250;

	if (!dispatched && pending.RetryCount < 120)
	{
		++pending.RetryCount;
		pending.DueAt = now + 250;
	}
	else
	{
		pending.Active = false;
		static std::atomic_uint32_t logged = 0;
		if (logged.fetch_add(
				1, std::memory_order_relaxed) < 128)
		{
			AtlasDiagnostics::WriteLine(
				"arena-visual-fallback delayed-consumer=%s "
				"sequence=%llu result=%s retries=%u",
				consumerName ? consumerName : "unsupported",
				static_cast<unsigned long long>(
					pending.Sequence),
				dispatched ? "dispatched" : "unavailable",
				pending.RetryCount);
		}
	}
}

static void MaintainPendingArenaBridgeVisuals(ULONGLONG now)
{
	const std::uint64_t terminalSession =
		g_ArenaSessionBridge.TerminalSessionOrdinal;
	const ULONGLONG terminalDeadline =
		g_ArenaSessionBridge.TerminalVisualDrainDeadline;
	if (terminalSession && terminalDeadline && now >= terminalDeadline)
	{
		std::size_t dropped = 0;
		for (auto& pending : g_PendingArenaBridgeVisuals)
		{
			if (!pending.Active ||
				pending.SessionOrdinal != terminalSession)
			{
				continue;
			}
			if (pending.Dispatching)
				pending.CancelAfterDispatch = true;
			else
				pending = {};
			++dropped;
		}
		g_ArenaSessionBridge.TerminalVisualDrainDeadline = 0;
		g_NextArenaBridgeVisualDispatchAt = (std::min)(
			g_NextArenaBridgeVisualDispatchAt, now);
		AtlasDiagnostics::WriteLine(
			"arena-session terminal-visual-drain-expired "
			"session=%llu dropped=%zu",
			static_cast<unsigned long long>(terminalSession), dropped);
	}
	if (now < g_NextArenaBridgeVisualDispatchAt)
		return;

	FPendingArenaBridgeVisual* next = nullptr;
	for (auto& pending : g_PendingArenaBridgeVisuals)
	{
		if (AtlasArenaSessionBridgePolicy::
			ShouldPreferVisualSequence(
				pending.Active,
				pending.Dispatching,
				pending.EventSequence,
				next ? next->EventSequence : 0))
		{
			next = &pending;
		}
	}
	if (!next || now < next->DueAt)
		return;

	auto& pending = *next;
	if (g_ArenaSessionBridge.Session.SessionOrdinal !=
			pending.SessionOrdinal ||
		g_ArenaSessionBridge.World != UWorld::GetWorld() ||
		pending.World != UWorld::GetWorld() ||
		pending.Controller != GetLocalFortPlayerController() ||
		!g_ArenaSessionBridge.Session.Active)
	{
		pending = {};
		return;
	}

	const ULONGLONG nativePresentedAt =
		GetArenaBridgeNativePresentationAt(
			pending.Kind, pending.AbsoluteStatValue,
			pending.Placement);
	if (WasArenaBridgeNativeRecentlyPresented(
			pending.Kind, pending.AbsoluteStatValue,
			pending.Placement, now) &&
		AtlasArenaSessionBridgePolicy::CanNativeVisualOwnBridgeEvent(
			pending.NativeGraceExpected,
			nativePresentedAt,
			g_NextArenaBridgeVisualDispatchAt))
	{
		ReserveArenaBridgePresentationSpacing(
			nativePresentedAt);
		AtlasDiagnostics::WriteLine(
			"arena-session visual-consumed-by-native "
			"session=%llu sequence=%llu kind=%lc",
			static_cast<unsigned long long>(pending.SessionOrdinal),
			static_cast<unsigned long long>(pending.EventSequence),
			static_cast<wchar_t>(pending.Kind));
		pending = {};
		return;
	}

	using EKind =
		AtlasArenaSessionBridgePolicy::EArenaPresentationEventKind;
	const std::uint64_t sequence = pending.EventSequence;
	pending.Dispatching = true;
	bool dispatched = false;
	const char* route = "unavailable";
	if (pending.Kind == EKind::BusFare ||
		pending.Kind == EKind::Elimination)
	{
		// The authoritative bridge already supplies exact text, delta, and
		// resulting Hype. Prefer the presentation-only badge path so the client
		// cannot replay TournamentStats that Fortnite may have applied natively.
		dispatched = TryDispatchDirectArenaBadge(
			pending.Kind, pending.AbsoluteStatValue,
			pending.PointsDelta);
		route = dispatched ? "direct-badge" : "consumer-unavailable";
		auto result = EArenaBridgeStatDispatchResult::Unavailable;
		if (!dispatched && !pending.NativeCapabilityExpected &&
			AtlasArenaSessionBridgePolicy::ShouldAttemptTournamentStatRoute(
				pending.TournamentRouteAttempted))
		{
			result = DispatchArenaBridgeTournamentStat(
				pending.Kind, pending.AbsoluteStatValue);
			// A silent call can still advance the widget's internal stat map.
			// Never replay that state-changing route on a visual retry.
			if (result != EArenaBridgeStatDispatchResult::Unavailable)
				pending.TournamentRouteAttempted = true;
		}
		if (result == EArenaBridgeStatDispatchResult::Presented)
		{
			dispatched = true;
			route = "tournament-stat";
		}
		else if (!dispatched &&
			result != EArenaBridgeStatDispatchResult::Unavailable)
		{
			route = result == EArenaBridgeStatDispatchResult::Silent
				? "tournament-stat-silent"
				: "consumer-unavailable";
		}
	}
	else
	{
		// The ordered server event already supplies the exact milestone and
		// award. Once the native grace window expires, drive the alert directly
		// instead of priming a version-specific placement map (which can emit a
		// stale previous-placement badge on some builds).
		dispatched = TryDispatchDirectArenaBadge(
			pending.Kind, pending.Placement,
			pending.PointsDelta);
		route = dispatched ? "direct-badge" : "consumer-unavailable";
		if (dispatched)
			MarkArenaPlacementPresented(pending.Placement);
	}

	if (pending.EventSequence != sequence)
		return;
	pending.Dispatching = false;
	if (pending.CancelAfterDispatch)
	{
		pending = {};
		return;
	}
	if (dispatched)
	{
		ReserveArenaBridgePresentationSpacing(now);
	}
	else
	{
		g_NextArenaBridgeVisualDispatchAt = now +
			AtlasArenaSessionBridgePolicy::
				kVisualRetryDelayMilliseconds;
	}
	if (!dispatched && pending.RetryCount < 120)
	{
		++pending.RetryCount;
		pending.DueAt = now +
			AtlasArenaSessionBridgePolicy::
				kVisualRetryDelayMilliseconds;
		return;
	}

	AtlasDiagnostics::WriteLine(
		"arena-session visual-dispatch session=%llu sequence=%llu "
		"kind=%lc route=%s retries=%u",
		static_cast<unsigned long long>(pending.SessionOrdinal),
		static_cast<unsigned long long>(pending.EventSequence),
		static_cast<wchar_t>(pending.Kind),
		dispatched ? route : "exhausted",
		pending.RetryCount);
	pending = {};
}

static void MaintainArenaSessionBridgeScore(
	UWorld* world,
	ULONGLONG now)
{
	auto& bridge = g_ArenaSessionBridge;
	if (!world || bridge.World != world ||
		!bridge.Session.Active || IsFrontendWorld(world))
	{
		bridge.LastScoreAlert = nullptr;
		bridge.LastAppliedHype = -1;
		bridge.ScoreRepairPending = false;
		bridge.ScoreRepairAttempts = 0;
		bridge.ScoreRepairSuccesses = 0;
		bridge.NextScoreRepairAt = 0;
		return;
	}

	UObject* alert = g_ArenaLiveScoringAlert.load(
		std::memory_order_acquire);
	const bool ownedFallbackSuppressed =
		!AtlasArenaHudOwnershipPolicy::CanCreateOwnedFallbackForSession(
			true,
			bridge.Session.SessionOrdinal,
			bridge.OwnedFallbackSuppressedSessionOrdinal,
			bridge.NativeHudOwnerSessionOrdinal);
	if (!IsLiveUObject(alert) && ownedFallbackSuppressed)
	{
		bridge.LastScoreAlert = nullptr;
		bridge.LastAppliedHype = -1;
		bridge.ScoreRepairPending = false;
		bridge.ScoreRepairAttempts = 0;
		bridge.ScoreRepairSuccesses = 0;
		bridge.NextScoreRepairAt = 0;
		return;
	}
	if (alert != bridge.LastScoreAlert)
	{
		bridge.LastScoreAlert = IsLiveUObject(alert) ? alert : nullptr;
		bridge.LastAppliedHype = -1;
		bridge.ScoreRepairPending = true;
		bridge.ScoreRepairAttempts = 0;
		bridge.ScoreRepairSuccesses = 0;
		bridge.NextScoreRepairAt = 0;
	}
	constexpr unsigned kMaximumAttempts = 120;
	constexpr ULONGLONG kSlowRetryMilliseconds = 2000;
	constexpr ULONGLONG kMaintenanceWriteMilliseconds = 2000;
	const bool exactOwnedFallback =
		IsExactOwnedArenaScoringAlert(alert);
	const unsigned successfulStabilizationWrites =
		AtlasArenaSessionBridgePolicy::ScoreStabilizationWriteTarget(
			exactOwnedFallback);
	const bool maintainStableScoreText =
		AtlasArenaSessionBridgePolicy::ShouldMaintainStableScoreText(
			exactOwnedFallback);
	bool maintenanceWrite = false;
	if (!bridge.ScoreRepairPending)
	{
		if (bridge.LastAppliedHype != bridge.Session.CurrentHype)
		{
			MarkArenaSessionScoreRepairPending();
		}
		else if (maintainStableScoreText &&
			bridge.NextScoreRepairAt != 0 &&
			now >= bridge.NextScoreRepairAt)
		{
			// Only ATLAS's compatibility widget lacks a native callback that can
			// re-arm repair after its asynchronous "=" placeholder refresh.
			bridge.ScoreRepairPending = true;
			bridge.ScoreRepairSuccesses =
				successfulStabilizationWrites - 1;
			maintenanceWrite = true;
		}
	}
	if (!bridge.ScoreRepairPending || now < bridge.NextScoreRepairAt)
		return;

	const bool applied = IsLiveUObject(alert) &&
		TrySetArenaScoreText(alert, bridge.Session.CurrentHype);
	if (bridge.ScoreRepairAttempts < UINT_MAX)
		++bridge.ScoreRepairAttempts;
	if (applied)
	{
		++bridge.ScoreRepairSuccesses;
		bridge.LastAppliedHype = bridge.Session.CurrentHype;
		bridge.ScoreRepairPending =
			bridge.ScoreRepairSuccesses <
				successfulStabilizationWrites;
	}
	else
	{
		// HUDPointCounter/Text_ScoreValue is populated asynchronously on some
		// authored widgets. Stay recoverable without hammering reflection once
		// the bounded fast window has elapsed.
		bridge.ScoreRepairPending = true;
	}
	bridge.NextScoreRepairAt = bridge.ScoreRepairPending
		? now + (bridge.ScoreRepairAttempts < kMaximumAttempts
			? 250
			: kSlowRetryMilliseconds)
		: (maintainStableScoreText
			? now + kMaintenanceWriteMilliseconds
			: 0);
	if (bridge.ScoreRepairAttempts == 1 ||
		(!bridge.ScoreRepairPending && !maintenanceWrite) ||
		(!applied && bridge.ScoreRepairAttempts == kMaximumAttempts))
	{
		AtlasDiagnostics::WriteLine(
			"arena-session score-repair session=%llu hype=%d "
			"applied=%d attempts=%u complete=%d alert=%p owner=%s",
			static_cast<unsigned long long>(
				bridge.Session.SessionOrdinal),
			bridge.Session.CurrentHype,
			applied ? 1 : 0,
			bridge.ScoreRepairAttempts,
			bridge.ScoreRepairPending ? 0 : 1,
			alert,
			exactOwnedFallback ? "fallback" : "native");
	}
}

static uint32 GetArenaTournamentStatParameterSizeUnsafe(
	const UFunction* function,
	const FFrame* stack)
{
	if (!function || !stack || !stack->Locals)
		return 0;
	const auto schema = function->GetParamsNamed();
	if (schema.Size == 0 || schema.Size > 0x20)
		return 0;
	return schema.Size;
}

static uint32 GetArenaTournamentStatParameterSize(
	const UFunction* function,
	const FFrame* stack)
{
	__try
	{
		return GetArenaTournamentStatParameterSizeUnsafe(
			function, stack);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return 0;
	}
}

static void TraceArenaPlacementRpc(
	UObject* context, FFrame& stack, void* result)
{
	const UFunction* function =
		g_ArenaPlacementRpcFunction.load(
			std::memory_order_acquire);
	const bool directFrame =
		IsDirectArenaVisualTraceFrame(&stack, function);
	AFortPlayerControllerAthena* localController =
		GetLocalFortPlayerController();
	const bool localDirectFrame = directFrame && localController &&
		context == localController;
	int32 placement = 0;
	int32 pointsEarned = 0;
	bool payloadValid = false;
	if (localDirectFrame)
	{
		__try
		{
			memcpy(&placement, stack.Locals, sizeof(placement));
			memcpy(&pointsEarned,
				stack.Locals + sizeof(placement),
				sizeof(pointsEarned));
			payloadValid = true;
			static std::atomic_uint32_t logged = 0;
			if (logged.fetch_add(
					1, std::memory_order_relaxed) < 128)
			{
				AtlasDiagnostics::WriteLine(
					"arena-visual-trace rpc=placement "
					"placement=%d points=%d context=%p",
					placement, pointsEarned, context);
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			AtlasDiagnostics::WriteLine(
				"arena-visual-trace rpc=placement "
				"payload-read-faulted context=%p",
				context);
		}
	}
	const uint64 pointConsumerCallsBefore =
		g_ArenaPointScoredConsumerCalls.load(
			std::memory_order_acquire);
	const uint64 teamPlacementCallsBefore =
		g_ArenaTeamPlacementConsumerCalls.load(
			std::memory_order_acquire);
	const ULONGLONG nextAllowedPresentationAt =
		g_NextArenaBridgeVisualDispatchAt;
	if (payloadValid && localDirectFrame)
	{
		// Maintain the legacy placement-bridge marker separately. Universal
		// E/P/V ownership is resolved by the scoped FIFO observation below.
		BeginArenaPlacementRpcObservation(placement, pointsEarned);
	}

	bool originalDispatched = false;
	if (const ArenaVisualTraceNative original =
		g_ArenaPlacementRpcOriginal.load(
			std::memory_order_acquire))
	{
		originalDispatched = true;
		++g_ArenaRpcOriginalDispatchDepth;
		original(context, stack, result);
		--g_ArenaRpcOriginalDispatchDepth;
	}

	const uint64 pointConsumerCallsAfter =
		g_ArenaPointScoredConsumerCalls.load(
			std::memory_order_acquire);
	const uint64 teamPlacementCallsAfter =
		g_ArenaTeamPlacementConsumerCalls.load(
			std::memory_order_acquire);
	const bool observedPointConsumerAdvance =
		pointConsumerCallsAfter > pointConsumerCallsBefore;
	const bool pointConsumerObserverInstalled =
		g_ArenaPointScoredConsumerTraceInstalled.load(
			std::memory_order_acquire);
	UObject* visibleArenaAlert =
		g_ArenaLiveScoringAlert.load(std::memory_order_acquire);
	// A zero counter delta proves silence only when the consumer observer is
	// actually installed. Legacy 10.x can present this RPC through its exact
	// visible Arena widget without exposing a hookable OnPointScored function.
	if (localDirectFrame && !pointConsumerObserverInstalled &&
		originalDispatched &&
		!IsVerifiedNativeOwnedArenaScoringAlert(visibleArenaAlert))
	{
		visibleArenaAlert = FindLiveArenaScoringAlert(
			localController);
		if (visibleArenaAlert)
		{
			g_ArenaLiveScoringAlert.store(
				visibleArenaAlert, std::memory_order_release);
		}
	}
	const bool visualConsumerAdvanced =
		AtlasArenaNativeVisualCorrelationPolicy::
			ShouldTreatNativeRpcAsPresented(
				pointConsumerObserverInstalled,
				observedPointConsumerAdvance,
				originalDispatched,
				IsVerifiedNativeOwnedArenaScoringAlert(
					visibleArenaAlert));
	const bool teamPlacementConsumerAdvanced =
		teamPlacementCallsAfter > teamPlacementCallsBefore;
	const ULONGLONG nativePresentedAt = GetTickCount64();
	const bool nativeVisualCanOwn =
		AtlasArenaSessionBridgePolicy::CanNativeVisualOwnBridgeEvent(
			true, nativePresentedAt, nextAllowedPresentationAt);
	if (payloadValid && localDirectFrame &&
		visualConsumerAdvanced && nativeVisualCanOwn)
	{
		// Retain this only for the legacy placement bridge. The RPC is not
		// classified as a placement or elimination until the ordered Arena
		// event supplies that identity.
		MarkArenaPlacementRpcPresented(placement, pointsEarned);
	}
	if (payloadValid && localDirectFrame &&
		teamPlacementConsumerAdvanced && visualConsumerAdvanced &&
		nativeVisualCanOwn)
	{
		// Unlike OnPointScored, this payload-preserving consumer is specific
		// to a real team-placement transition. Let it own the milestone even
		// when a native RPC called it synchronously.
		MarkArenaPlacementPresented(placement);
		ConsumePendingArenaTeamPlacementFallback(placement);
		ConsumePendingArenaBridgeVisual(
			placement == 1
				? AtlasArenaSessionBridgePolicy::
					EArenaPresentationEventKind::Victory
				: AtlasArenaSessionBridgePolicy::
					EArenaPresentationEventKind::Placement,
			placement, pointsEarned, nativePresentedAt);
	}
	else if (payloadValid && localDirectFrame)
	{
		// On 10.x a point-only invocation may be an elimination. On later
		// builds the same shape may be a placement. Preserve the observation
		// and let the exact server event correlate it in FIFO order.
		RecordArenaPlacementRpcObservation(
			placement, pointsEarned,
			visualConsumerAdvanced && nativeVisualCanOwn,
			teamPlacementConsumerAdvanced);
	}
}

static void TraceArenaTournamentStatRpc(
	UObject* context, FFrame& stack, void* result)
{
	const UFunction* function =
		g_ArenaTournamentStatRpcFunction.load(
			std::memory_order_acquire);
	const bool directFrame =
		IsDirectArenaVisualTraceFrame(&stack, function);
	AFortPlayerControllerAthena* localController =
		GetLocalFortPlayerController();
	const bool localDirectFrame = directFrame && localController &&
		context == localController->PlayerState;
	const uint32 parameterSize = directFrame
		? GetArenaTournamentStatParameterSize(
			function, &stack)
		: 0;
	EObservedArenaTournamentStat observedKind =
		EObservedArenaTournamentStat::Unknown;
	int32 observedValue = 0;
	const bool observedStat = localDirectFrame && parameterSize > 0 &&
		ReadArenaTournamentStatValue(
			const_cast<UFunction*>(function),
			stack.Locals, parameterSize,
			&observedKind, &observedValue);
	int32 placement = 0;
	const bool placementStat = observedStat && observedKind ==
		EObservedArenaTournamentStat::Placement &&
		ReadArenaPlacementTournamentStatValue(
			const_cast<UFunction*>(function),
			stack.Locals, parameterSize, &placement);
	const bool eliminationStat = observedStat && observedKind ==
		EObservedArenaTournamentStat::Eliminations;
	const ULONGLONG nextAllowedPresentationAt =
		g_NextArenaBridgeVisualDispatchAt;

	static std::atomic_uint32_t logged = 0;
	if (logged.fetch_add(1, std::memory_order_relaxed) < 128)
	{
		AtlasDiagnostics::WriteLine(
			"arena-visual-trace rpc=tournament-stat context=%p "
			"direct-frame=%d bytes=%u elimination=%d",
			context,
			directFrame ? 1 : 0,
			parameterSize,
			eliminationStat ? 1 : 0);
	}

	bool originalDispatched = false;
	if (const ArenaVisualTraceNative original =
		g_ArenaTournamentStatRpcOriginal.load(
			std::memory_order_acquire))
	{
		originalDispatched = true;
		++g_ArenaRpcOriginalDispatchDepth;
		original(context, stack, result);
		--g_ArenaRpcOriginalDispatchDepth;
	}

	const bool completeObserverInstalled =
		g_ArenaTournamentStatConsumerTraceInstalled.load(
			std::memory_order_acquire) &&
		g_ArenaPointScoredConsumerTraceInstalled.load(
			std::memory_order_acquire);
	UObject* visibleArenaAlert =
		g_ArenaLiveScoringAlert.load(std::memory_order_acquire);
	if (localDirectFrame && observedStat && originalDispatched &&
		!completeObserverInstalled &&
		!IsVerifiedNativeOwnedArenaScoringAlert(visibleArenaAlert))
	{
		visibleArenaAlert = FindLiveArenaScoringAlert(
			localController);
		if (visibleArenaAlert)
		{
			g_ArenaLiveScoringAlert.store(
				visibleArenaAlert, std::memory_order_release);
		}
	}
	if (localDirectFrame && observedStat && originalDispatched &&
		!completeObserverInstalled &&
		IsVerifiedNativeOwnedArenaScoringAlert(visibleArenaAlert))
	{
		// Some generations expose the tournament client RPC but not hookable
		// native alert consumers. In that state a returned RPC on the exact
		// visible Arena widget is native-owned; a zero observer count is unknown,
		// not proof that the visual was silent.
		using EKind = AtlasArenaSessionBridgePolicy::
			EArenaPresentationEventKind;
		const ULONGLONG now = GetTickCount64();
		const bool nativeVisualCanOwn =
			AtlasArenaSessionBridgePolicy::CanNativeVisualOwnBridgeEvent(
				true, now, nextAllowedPresentationAt);
		const bool positiveAward =
			AtlasArenaNativeVisualCorrelationPolicy::
				IsPositiveNativeTournamentAward(observedValue);
		if (observedKind == EObservedArenaTournamentStat::MatchPlayed &&
			positiveAward && nativeVisualCanOwn)
		{
			g_LastArenaBusFarePresentedAt = now;
			ConsumePendingArenaBridgeVisual(EKind::BusFare, 0, 0, now);
		}
		else if (observedKind ==
			EObservedArenaTournamentStat::Eliminations &&
			positiveAward)
		{
			// The reflected value may be a delta (normally 1) or an absolute
			// count depending on generation. The ordered local invocation itself
			// is one credited elimination, matching the session bridge counter.
			if (g_LastArenaEliminationStatValue < INT32_MAX)
				++g_LastArenaEliminationStatValue;
			if (nativeVisualCanOwn)
			{
				g_LastArenaEliminationPresentedAt = now;
				ConsumePendingArenaBridgeVisual(
					EKind::Elimination, 0, 0, now);
			}
		}
		else if (placementStat && placement > 0 &&
			nativeVisualCanOwn)
		{
			MarkArenaPlacementPresented(placement);
			ConsumePendingArenaTeamPlacementFallback(placement);
			ConsumePendingArenaBridgeVisual(
				placement == 1 ? EKind::Victory : EKind::Placement,
				placement, 0, now);
		}
	}
}

static void RequestArenaVisibleAlertReaudit(UObject* context)
{
	// A native Arena alert can finish constructing after the manual fallback
	// has already become the cached live widget. A nested UI-extension widget
	// can consume authored events while IsInViewport still reports false, so
	// preserve the exact consumer context for bounded game-thread validation.
	if (g_ArenaOwnedFallbackConstructionActive)
		return;
	UObject* activeAlert = g_ArenaLiveScoringAlert.load(
		std::memory_order_acquire);
	UObject* ownedAlert = g_ArenaOwnedScoringAlertForObserver.load(
		std::memory_order_acquire);
	if (context && context != activeAlert && context != ownedAlert &&
		IsTrustedArenaNativeScoringConsumer(context))
	{
		// A later authored widget may replace an earlier native instance during
		// the spawn-to-aircraft HUD transition. The exact trusted callback is
		// stronger evidence than the old cached pointer, so let the game-thread
		// audit rebind instead of waiting for the stale object to be destroyed.
		g_ArenaObservedNativeScoringAlert.store(
			context, std::memory_order_release);
		g_ArenaObservedNativeScoringAlertWorld.store(
			UWorld::GetWorld(), std::memory_order_release);
		g_ArenaObservedNativeScoringAlertSessionOrdinal.store(
			g_ArenaSessionBridge.Session.SessionOrdinal,
			std::memory_order_release);
		g_ArenaVisibleAlertReauditRequested.store(
			true, std::memory_order_release);
	}
}

static void TraceArenaTournamentStatConsumer(
	UObject* context, FFrame& stack, void* result)
{
	UFunction* function =
		g_ArenaTournamentStatConsumerFunction.load(
			std::memory_order_acquire);
	const bool directFrame =
		IsDirectArenaVisualTraceFrame(&stack, function);
	const uint32 parameterSize = directFrame
		? GetArenaTournamentStatParameterSize(function, &stack)
		: 0;
	int32 placement = 0;
	const bool placementStat = parameterSize > 0 &&
		ReadArenaPlacementTournamentStatValue(
			function, stack.Locals, parameterSize, &placement);
	EObservedArenaTournamentStat observedKind =
		EObservedArenaTournamentStat::Unknown;
	int32 observedValue = 0;
	const bool observedStat = parameterSize > 0 &&
		ReadArenaTournamentStatValue(
			function, stack.Locals, parameterSize,
			&observedKind, &observedValue);
	UObject* activeAlert = g_ArenaLiveScoringAlert.load(
		std::memory_order_acquire);
	const bool activeConsumer = context && context == activeAlert;
	if (!activeConsumer)
		RequestArenaVisibleAlertReaudit(context);
	UObject* observedNative = g_ArenaObservedNativeScoringAlert.load(
		std::memory_order_acquire);
	UObject* ownedFallback = g_ArenaOwnedScoringAlertForObserver.load(
		std::memory_order_acquire);
	const bool observedNativeConsumer = context &&
		context == observedNative && context != ownedFallback;
	const bool suppressOwnedFallbackConsumer =
		AtlasArenaHudOwnershipPolicy::
			ShouldSuppressOwnedFallbackConsumer(
				context == ownedFallback,
				g_ArenaVisualFallbackDispatchActive);
	const bool presentationConsumer = observedNativeConsumer ||
		(activeConsumer && !suppressOwnedFallbackConsumer);
	const uint64 pointCallsBefore =
		g_ArenaPointScoredConsumerCalls.load(
			std::memory_order_acquire);
	const ULONGLONG nextAllowedPresentationAt =
		g_NextArenaBridgeVisualDispatchAt;
	if (presentationConsumer)
	{
		g_ArenaTournamentStatConsumerCalls.fetch_add(
			1, std::memory_order_acq_rel);
		if (g_ArenaRpcOriginalDispatchDepth == 0 &&
			!g_ArenaVisualFallbackDispatchActive)
		{
			ConsumeOldestPendingArenaVisualFallback(
				EArenaVisualTraceSchema::TournamentStat);
		}
	}
	static std::atomic_uint32_t logged = 0;
	if (logged.fetch_add(1, std::memory_order_relaxed) < 128)
	{
		AtlasDiagnostics::WriteLine(
			"arena-visual-trace consumer=tournament-stat "
			"context=%p active=%d",
			context, presentationConsumer ? 1 : 0);
	}

	if (const ArenaVisualTraceNative original =
		g_ArenaTournamentStatConsumerOriginal.load(
			std::memory_order_acquire))
	{
		if (!suppressOwnedFallbackConsumer)
		{
			++g_ArenaTypedConsumerDispatchDepth;
			original(context, stack, result);
			--g_ArenaTypedConsumerDispatchDepth;
		}
	}
	if (presentationConsumer && context != ownedFallback &&
		!g_ArenaVisualFallbackDispatchActive)
	{
		MarkArenaSessionScoreRepairPending();
	}

	const uint64 pointCallsAfter =
		g_ArenaPointScoredConsumerCalls.load(
			std::memory_order_acquire);
	const bool pointScored = pointCallsAfter > pointCallsBefore;
	if (presentationConsumer && observedStat && pointScored &&
		!g_ArenaVisualFallbackDispatchActive)
	{
		using EKind =
			AtlasArenaSessionBridgePolicy::EArenaPresentationEventKind;
		const ULONGLONG now = GetTickCount64();
		const bool nativeVisualCanOwn =
			AtlasArenaSessionBridgePolicy::CanNativeVisualOwnBridgeEvent(
				true, now, nextAllowedPresentationAt);
		const bool positiveAward =
			AtlasArenaNativeVisualCorrelationPolicy::
				IsPositiveNativeTournamentAward(observedValue);
		if (observedKind ==
			EObservedArenaTournamentStat::MatchPlayed &&
			positiveAward && nativeVisualCanOwn)
		{
			g_LastArenaBusFarePresentedAt = now;
			ConsumePendingArenaBridgeVisual(EKind::BusFare, 0, 0, now);
		}
		else if (observedKind ==
			EObservedArenaTournamentStat::Eliminations &&
			positiveAward)
		{
			if (g_LastArenaEliminationStatValue < INT32_MAX)
				++g_LastArenaEliminationStatValue;
			if (nativeVisualCanOwn)
			{
				g_LastArenaEliminationPresentedAt = now;
				ConsumePendingArenaBridgeVisual(
					EKind::Elimination, 0, 0, now);
			}
		}
		else if (observedKind ==
			EObservedArenaTournamentStat::Placement && placementStat &&
			placement > 0 && nativeVisualCanOwn)
		{
			// Only the payload-bearing placement stat can own this marker. An
			// adjacent elimination OnPointScored remains deliberately unrelated.
			MarkArenaPlacementPresented(placement);
			ConsumePendingArenaTeamPlacementFallback(placement);
			ConsumePendingArenaBridgeVisual(
				placement == 1 ? EKind::Victory : EKind::Placement,
				placement, 0, now);
		}
	}

	static std::atomic_uint32_t placementLogged = 0;
	if (placementStat &&
		placementLogged.fetch_add(
			1, std::memory_order_relaxed) < 128)
	{
		AtlasDiagnostics::WriteLine(
			"arena-visual-trace consumer=tournament-stat-placement "
			"placement=%d active=%d point-scored=%d fallback=%d",
			placement,
			presentationConsumer ? 1 : 0,
			pointScored ? 1 : 0,
			g_ArenaVisualFallbackDispatchActive ? 1 : 0);
	}
}

static void TraceArenaPointScoredConsumer(
	UObject* context, FFrame& stack, void* result)
{
	UObject* activeAlert = g_ArenaLiveScoringAlert.load(
		std::memory_order_acquire);
	const bool activeConsumer = context && context == activeAlert;
	if (!activeConsumer)
		RequestArenaVisibleAlertReaudit(context);
	UObject* observedNative = g_ArenaObservedNativeScoringAlert.load(
		std::memory_order_acquire);
	UObject* ownedFallback = g_ArenaOwnedScoringAlertForObserver.load(
		std::memory_order_acquire);
	const bool observedNativeConsumer = context &&
		context == observedNative && context != ownedFallback;
	const bool suppressOwnedFallbackConsumer =
		AtlasArenaHudOwnershipPolicy::
			ShouldSuppressOwnedFallbackConsumer(
				context == ownedFallback,
				g_ArenaVisualFallbackDispatchActive);
	const bool presentationConsumer = observedNativeConsumer ||
		(activeConsumer && !suppressOwnedFallbackConsumer);
	const bool typedNativeDispatchActive =
		g_ArenaRpcOriginalDispatchDepth != 0 ||
		g_ArenaTypedConsumerDispatchDepth != 0;
	if (presentationConsumer)
	{
		g_ArenaPointScoredConsumerCalls.fetch_add(
			1, std::memory_order_acq_rel);
		if (g_ArenaRpcOriginalDispatchDepth == 0 &&
			!g_ArenaVisualFallbackDispatchActive)
		{
			ConsumeOldestPendingArenaVisualFallback(
				EArenaVisualTraceSchema::ZeroParameters);
		}
	}
	static std::atomic_uint32_t logged = 0;
	if (logged.fetch_add(1, std::memory_order_relaxed) < 128)
	{
		AtlasDiagnostics::WriteLine(
			"arena-visual-trace consumer=point-scored "
			"context=%p active=%d",
			context, presentationConsumer ? 1 : 0);
	}

	bool originalDispatched = false;
	if (const ArenaVisualTraceNative original =
		g_ArenaPointScoredConsumerOriginal.load(
			std::memory_order_acquire))
	{
		if (!suppressOwnedFallbackConsumer)
		{
			original(context, stack, result);
			originalDispatched = true;
		}
	}
	if (AtlasArenaNativeVisualCorrelationPolicy::
			ShouldRecordNativePointScoredObservation(
				presentationConsumer,
				context && context != ownedFallback,
				originalDispatched,
				g_ArenaVisualFallbackDispatchActive,
				typedNativeDispatchActive,
				g_ArenaSessionBridge.Session.Active))
	{
		RecordArenaNativePointScoredObservation(context);
	}
	if (presentationConsumer && context != ownedFallback &&
		!g_ArenaVisualFallbackDispatchActive)
	{
		MarkArenaSessionScoreRepairPending();
	}
}

static void TraceArenaTeamPlacementConsumer(
	UObject* context, FFrame& stack, void* result)
{
	const UFunction* function =
		g_ArenaTeamPlacementConsumerFunction.load(
			std::memory_order_acquire);
	const bool directFrame =
		IsDirectArenaVisualTraceFrame(&stack, function);
	int32 placement = 0;
	bool payloadValid = false;
	if (directFrame)
	{
		__try
		{
			memcpy(&placement, stack.Locals, sizeof(placement));
			payloadValid = placement > 0;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			payloadValid = false;
		}
	}

	UObject* activeAlert = g_ArenaLiveScoringAlert.load(
		std::memory_order_acquire);
	const bool activeConsumer = context && context == activeAlert;
	if (!activeConsumer)
		RequestArenaVisibleAlertReaudit(context);
	UObject* observedNative = g_ArenaObservedNativeScoringAlert.load(
		std::memory_order_acquire);
	UObject* ownedFallback = g_ArenaOwnedScoringAlertForObserver.load(
		std::memory_order_acquire);
	const bool observedNativeConsumer = context &&
		context == observedNative && context != ownedFallback;
	const bool suppressOwnedFallbackConsumer =
		AtlasArenaHudOwnershipPolicy::
			ShouldSuppressOwnedFallbackConsumer(
				context == ownedFallback,
				g_ArenaVisualFallbackDispatchActive);
	const bool presentationConsumer = observedNativeConsumer ||
		(activeConsumer && !suppressOwnedFallbackConsumer);
	const uint64 pointCallsBefore =
		g_ArenaPointScoredConsumerCalls.load(
			std::memory_order_acquire);
	const ULONGLONG nextAllowedPresentationAt =
		g_NextArenaBridgeVisualDispatchAt;
	if (presentationConsumer)
	{
		g_ArenaTeamPlacementConsumerCalls.fetch_add(
			1, std::memory_order_acq_rel);
	}

	if (const ArenaVisualTraceNative original =
		g_ArenaTeamPlacementConsumerOriginal.load(
			std::memory_order_acquire))
	{
		if (!suppressOwnedFallbackConsumer)
		{
			++g_ArenaTypedConsumerDispatchDepth;
			original(context, stack, result);
			--g_ArenaTypedConsumerDispatchDepth;
		}
	}
	if (presentationConsumer && context != ownedFallback &&
		!g_ArenaVisualFallbackDispatchActive)
	{
		MarkArenaSessionScoreRepairPending();
	}

	const uint64 pointCallsAfter =
		g_ArenaPointScoredConsumerCalls.load(
			std::memory_order_acquire);
	const ULONGLONG nativePresentedAt = GetTickCount64();
	const bool nativeVisualCanOwn =
		AtlasArenaSessionBridgePolicy::CanNativeVisualOwnBridgeEvent(
			true, nativePresentedAt, nextAllowedPresentationAt);
	if (presentationConsumer && payloadValid &&
		pointCallsAfter > pointCallsBefore &&
		g_ArenaRpcOriginalDispatchDepth == 0 &&
		!g_ArenaVisualFallbackDispatchActive && nativeVisualCanOwn)
	{
		// A direct native team-placement callback owns this exact milestone.
		// An unrelated zero-parameter elimination notification cannot consume
		// the payload-keyed repair.
		MarkArenaPlacementPresented(placement);
		ConsumePendingArenaTeamPlacementFallback(placement);
		ConsumePendingArenaBridgeVisual(
			placement == 1
				? AtlasArenaSessionBridgePolicy::
					EArenaPresentationEventKind::Victory
				: AtlasArenaSessionBridgePolicy::
					EArenaPresentationEventKind::Placement,
			placement, 0, nativePresentedAt);
	}

	static std::atomic_uint32_t logged = 0;
	if (logged.fetch_add(1, std::memory_order_relaxed) < 128)
	{
		AtlasDiagnostics::WriteLine(
			"arena-visual-trace consumer=team-placement "
			"placement=%d direct-frame=%d active=%d point-scored=%d",
			placement,
			directFrame ? 1 : 0,
			activeConsumer ? 1 : 0,
			pointCallsAfter > pointCallsBefore ? 1 : 0);
	}
}

static bool TryInstallArenaVisualTrace(
	UFunction* function,
	EArenaVisualTraceSchema schema,
	ArenaVisualTraceNative detour,
	std::atomic<ArenaVisualTraceNative>& originalStorage,
	std::atomic_bool& installedStorage,
	std::atomic<UFunction*>* functionStorage,
	const char* label)
{
	if (installedStorage.load(std::memory_order_acquire))
		return true;
	if (!function || !detour || !label ||
		!MatchesArenaVisualTraceSchema(function, schema))
	{
		return false;
	}

	__try
	{
		void*& nativeFunction = function->GetNativeFunc();
		auto nativeSlot =
			reinterpret_cast<PVOID volatile*>(&nativeFunction);
		void* observed = InterlockedCompareExchangePointer(
			nativeSlot, nullptr, nullptr);
		if (!observed || observed ==
			reinterpret_cast<void*>(detour))
		{
			return false;
		}

		originalStorage.store(
			reinterpret_cast<ArenaVisualTraceNative>(observed),
			std::memory_order_release);
		if (functionStorage)
		{
			functionStorage->store(
				function, std::memory_order_release);
		}

		void* previous = InterlockedCompareExchangePointer(
			nativeSlot,
			reinterpret_cast<void*>(detour), observed);
		if (previous != observed)
		{
			originalStorage.store(
				nullptr, std::memory_order_release);
			if (functionStorage)
			{
				functionStorage->store(
					nullptr, std::memory_order_release);
			}
			return false;
		}

		installedStorage.store(true, std::memory_order_release);
		AtlasDiagnostics::WriteLine(
			"arena-visual-trace installed target=%s "
			"function=%p original=%p",
			label, function, observed);
		return true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		originalStorage.store(nullptr, std::memory_order_release);
		if (functionStorage)
			functionStorage->store(nullptr, std::memory_order_release);
		return false;
	}
}

static UFunction* FindArenaVisualTraceFunction(
	const wchar_t* path)
{
	if (!path || !SDK::Offsets::StaticFindObject)
		return nullptr;
	__try
	{
		return const_cast<UFunction*>(
			FindObject<UFunction>(path));
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return nullptr;
	}
}

static void TryInstallArenaVisualConsumerTraces(UObject* consumerSource)
{
	if (!consumerSource)
		return;

	if (!g_ArenaTournamentStatConsumerTraceInstalled.load(
			std::memory_order_acquire))
	{
		// Resolve from the exact live widget or ArenaScoringHUD_C default object.
		// GetFunction may return an inherited implementation on releases without
		// a Blueprint override; the detour still accepts only a trusted local
		// scoring-alert context in the active Arena session.
		UFunction* consumer =
			consumerSource->GetFunction("OnTournamentStatUpdate");
		TryInstallArenaVisualTrace(
			consumer,
			EArenaVisualTraceSchema::TournamentStat,
			TraceArenaTournamentStatConsumer,
			g_ArenaTournamentStatConsumerOriginal,
			g_ArenaTournamentStatConsumerTraceInstalled,
			&g_ArenaTournamentStatConsumerFunction,
			"tournament-stat-consumer");
	}

	if (!g_ArenaPointScoredConsumerTraceInstalled.load(
			std::memory_order_acquire))
	{
		UFunction* consumer = consumerSource->GetFunction("OnPointScored");
		TryInstallArenaVisualTrace(
			consumer,
			EArenaVisualTraceSchema::ZeroParameters,
			TraceArenaPointScoredConsumer,
			g_ArenaPointScoredConsumerOriginal,
			g_ArenaPointScoredConsumerTraceInstalled,
			nullptr,
			"point-scored-consumer");
	}

	if (!g_ArenaTeamPlacementConsumerTraceInstalled.load(
			std::memory_order_acquire))
	{
		UFunction* consumer =
			consumerSource->GetFunction("OnTeamPlacementChanged");
		TryInstallArenaVisualTrace(
			consumer,
			EArenaVisualTraceSchema::TeamPlacement,
			TraceArenaTeamPlacementConsumer,
			g_ArenaTeamPlacementConsumerOriginal,
			g_ArenaTeamPlacementConsumerTraceInstalled,
			&g_ArenaTeamPlacementConsumerFunction,
			"team-placement-consumer");
	}
}

static void MaintainArenaConcreteVisualTracePreHud()
{
	if (!g_ArenaSessionBridge.Session.Active ||
		g_ArenaSessionBridge.World != UWorld::GetWorld())
	{
		return;
	}
	if (g_ArenaTournamentStatConsumerTraceInstalled.load(
			std::memory_order_acquire) &&
		g_ArenaPointScoredConsumerTraceInstalled.load(
			std::memory_order_acquire) &&
		g_ArenaTeamPlacementConsumerTraceInstalled.load(
			std::memory_order_acquire))
	{
		return;
	}

	// Hook concrete Blueprint overrides before HUD repair can construct ATLAS's
	// fallback. That guarantees a native widget born during the same tick is
	// observable and can take ownership without briefly producing two totals.
	const UClass* arenaClass = GetLiveArenaScoringHudClass();
	UObject* arenaDefault = IsLiveUObject(arenaClass)
		? const_cast<UClass*>(arenaClass)->GetDefaultObj()
		: nullptr;
	if (IsLiveUObject(arenaDefault))
		TryInstallArenaVisualConsumerTraces(arenaDefault);
}

static void MaintainArenaVisualTrace(
	AFortPlayerControllerAthena* playerController,
	ULONGLONG now)
{
	if (!playerController || now < g_NextArenaVisualTraceResolveAt)
		return;
	g_NextArenaVisualTraceResolveAt = now + 1000;
	UObject* liveScoringAlert =
		g_ArenaLiveScoringAlert.load(std::memory_order_acquire);
	if (!IsUsableArenaHudOwnerAlert(
			liveScoringAlert, playerController))
		liveScoringAlert = nullptr;
	UObject* consumerSource = liveScoringAlert;
	if (!consumerSource && g_ArenaSessionBridge.Session.Active &&
		g_ArenaSessionBridge.World == UWorld::GetWorld())
	{
		// Resolve the exact Blueprint class rather than the generic scoring base.
		// This installs the concrete override before the bus notification can
		// construct/reveal its first live instance, closing the observation gap
		// that previously caused a second widget and a replayed bus fare.
		const UClass* arenaClass = GetLiveArenaScoringHudClass();
		UObject* arenaDefault = IsLiveUObject(arenaClass)
			? const_cast<UClass*>(arenaClass)->GetDefaultObj()
			: nullptr;
		if (IsLiveUObject(arenaDefault))
			consumerSource = arenaDefault;
	}

	if (!g_ArenaPlacementRpcTraceInstalled.load(
			std::memory_order_acquire))
	{
		TryInstallArenaVisualTrace(
			playerController->GetFunction(
				"ClientReportTournamentPlacementPointsScored"),
			EArenaVisualTraceSchema::Placement,
			TraceArenaPlacementRpc,
			g_ArenaPlacementRpcOriginal,
			g_ArenaPlacementRpcTraceInstalled,
			&g_ArenaPlacementRpcFunction,
			"placement-rpc");
	}

	if (!g_ArenaTournamentStatRpcTraceInstalled.load(
			std::memory_order_acquire))
	{
		UFunction* tournamentStatFunction =
			playerController->PlayerState
				? playerController->PlayerState->GetFunction(
					"ClientReportTournamentStatUpdate")
				: nullptr;
		if (!tournamentStatFunction)
		{
			// PlayerState can arrive after the RPC UFunction has already been
			// registered. Resolve the owning class globally so tracing does not
			// depend on one live instance being available during this tick.
			tournamentStatFunction = FindArenaVisualTraceFunction(
				L"/Script/FortniteGame.FortPlayerStateAthena.ClientReportTournamentStatUpdate");
		}

		TryInstallArenaVisualTrace(
			tournamentStatFunction,
			EArenaVisualTraceSchema::TournamentStat,
			TraceArenaTournamentStatRpc,
			g_ArenaTournamentStatRpcOriginal,
			g_ArenaTournamentStatRpcTraceInstalled,
			&g_ArenaTournamentStatRpcFunction,
			"tournament-stat-rpc");
	}

	TryInstallArenaVisualConsumerTraces(consumerSource);
}

static bool TryReadObjectName(
	const UObject* object, FName* name)
{
	if (!object || !name)
		return false;

	__try
	{
		*name = object->Name;
		return true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return false;
	}
}

static bool IsFrontendWorld(const UWorld* world)
{
	if (!IsLiveUObject(world))
		return false;

	FName name{};
	if (!TryReadObjectName(world, &name))
		return false;

	return AtlasTournamentRefreshPolicy::IsFrontendWorldName(
		name.ToString());
}

static AtlasJoinRequestPolicy::ENetDriverState
ResolveJoinNetDriverState(UWorld* world)
{
	using AtlasJoinRequestPolicy::ENetDriverState;
	if (!IsLiveUObject(world))
		return ENetDriverState::Unknown;

	__try
	{
		if (!world->HasNetDriver())
			return ENetDriverState::Unknown;

		// Any non-null world net driver is treated as gameplay/travel. It need
		// not survive a UObject liveness test for the hotkey to fail closed.
		return world->NetDriver
			? ENetDriverState::Present
			: ENetDriverState::Absent;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return ENetDriverState::Unknown;
	}
}

static bool ResolveJoinFrontendActive(
	UWorld* world,
	AFortPlayerControllerAthena* playerController,
	AtlasJoinRequestPolicy::ENetDriverState* netDriverState)
{
	const auto resolvedNetDriver = ResolveJoinNetDriverState(world);
	if (netDriverState)
		*netDriverState = resolvedNetDriver;

	return AtlasJoinRequestPolicy::ResolveFrontendActive({
		IsLiveUObject(world),
		IsLiveUObject(playerController),
		IsFrontendWorld(world),
		resolvedNetDriver });
}

static bool MatchesFrontendPlaylistChangedSchemaUnsafe(
	UFunction* function)
{
	if (!IsLiveUObject(function))
		return false;

	const auto parameters = function->GetParamsNamed();
	if (parameters.NameOffsetMap.size() != 2 ||
		parameters.Size == 0 || parameters.Size > 0x40)
	{
		return false;
	}

	bool hasLobbyType = false;
	bool hasPlaylistData = false;
	for (const auto& parameter : parameters.NameOffsetMap)
	{
		if (parameter.Offset > parameters.Size ||
			parameter.ElementSize > parameters.Size - parameter.Offset)
		{
			return false;
		}

		if (parameter.Name == "LobbyType")
		{
			hasLobbyType = parameter.ElementSize > 0 &&
				parameter.ElementSize <= sizeof(int32);
		}
		else if (parameter.Name == "PlaylistData")
		{
			hasPlaylistData =
				parameter.ElementSize == sizeof(UObject*);
		}
		else
		{
			return false;
		}
	}

	return hasLobbyType && hasPlaylistData;
}

static bool MatchesFrontendPlaylistChangedSchema(
	UFunction* function)
{
	__try
	{
		return MatchesFrontendPlaylistChangedSchemaUnsafe(function);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return false;
	}
}

static void SignalFrontendPlaylistSelection()
{
	if (g_FrontendTournamentRefreshDispatchActive ||
		VersionInfo.FortniteVersion < 19.0 ||
		!IsFrontendWorld(UWorld::GetWorld()))
	{
		return;
	}

	// One user selection can notify several lobby widgets. Collapse that fanout
	// into one edge while still preserving selections made later in the session.
	const ULONGLONG now = GetTickCount64();
	uint64 previous = g_LastFrontendSelectionSignalAt.load(
		std::memory_order_acquire);
	for (;;)
	{
		if (now >= previous && now - previous < 250)
			return;
		if (g_LastFrontendSelectionSignalAt.compare_exchange_weak(
				previous, now, std::memory_order_acq_rel))
		{
			break;
		}
	}

	const uint64 revision =
		g_FrontendSelectionRevision.fetch_add(
			1, std::memory_order_acq_rel) + 1;
	AtlasDiagnostics::WriteLine(
		"tournament-refresh selection-signal revision=%llu",
		static_cast<unsigned long long>(revision));
}

static void FrontendPlaylistChangedBaseHook(
	UObject* context, FFrame& stack, void* result)
{
	if (const ArenaVisualTraceNative original =
		g_FrontendPlaylistChangedBaseOriginal.load(
			std::memory_order_acquire))
	{
		original(context, stack, result);
	}
	SignalFrontendPlaylistSelection();
}

static void FrontendPlaylistChangedDerivedHook(
	UObject* context, FFrame& stack, void* result)
{
	if (const ArenaVisualTraceNative original =
		g_FrontendPlaylistChangedDerivedOriginal.load(
			std::memory_order_acquire))
	{
		original(context, stack, result);
	}
	SignalFrontendPlaylistSelection();
}

static void FrontendModeSetSelectionHook(
	UObject* context, FFrame& stack, void* result)
{
	if (const ArenaVisualTraceNative original =
		g_FrontendModeSetSelectionOriginal.load(
			std::memory_order_acquire))
	{
		original(context, stack, result);
	}
	SignalFrontendPlaylistSelection();
}

static void FrontendActivityConfirmedHook(
	UObject* context, FFrame& stack, void* result)
{
	if (const ArenaVisualTraceNative original =
		g_FrontendActivityConfirmedOriginal.load(
			std::memory_order_acquire))
	{
		original(context, stack, result);
	}
	SignalFrontendPlaylistSelection();
}

static bool TryInstallFrontendPlaylistChangedHook(
	UFunction* function,
	ArenaVisualTraceNative detour,
	std::atomic<ArenaVisualTraceNative>& originalStorage,
	std::atomic_bool& installedStorage,
	const char* label)
{
	if (installedStorage.load(std::memory_order_acquire))
		return true;
	if (!function || !detour || !label ||
		!MatchesFrontendPlaylistChangedSchema(function))
	{
		return false;
	}

	__try
	{
		void*& nativeFunction = function->GetNativeFunc();
		auto nativeSlot =
			reinterpret_cast<PVOID volatile*>(&nativeFunction);
		void* observed = InterlockedCompareExchangePointer(
			nativeSlot, nullptr, nullptr);
		if (!observed || observed == reinterpret_cast<void*>(detour))
			return false;

		originalStorage.store(
			reinterpret_cast<ArenaVisualTraceNative>(observed),
			std::memory_order_release);
		void* previous = InterlockedCompareExchangePointer(
			nativeSlot, reinterpret_cast<void*>(detour), observed);
		if (previous != observed)
		{
			originalStorage.store(nullptr, std::memory_order_release);
			return false;
		}

		installedStorage.store(true, std::memory_order_release);
		AtlasDiagnostics::WriteLine(
			"tournament-refresh selection-hook-installed "
			"target=%s function=%p original=%p",
			label, function, observed);
		return true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		originalStorage.store(nullptr, std::memory_order_release);
		return false;
	}
}

static UFunction* FindLiveFrontendPlaylistChangedFunctionUnsafe(
	const UClass* lobbyBaseClass,
	UFunction* baseFunction)
{
	if (!lobbyBaseClass)
		return nullptr;

	// Live frontend widgets are allocated near the end of GUObjectArray. Scan
	// backwards and stop at the first derived implementation; this runs only
	// while resolving the one-time hook, not as a permanent per-frame scan.
	constexpr int maximumObjectsToInspect = 65536;
	const int objectCount = TUObjectArray::Num();
	const int firstIndex = (std::max)(0,
		objectCount - maximumObjectsToInspect);
	for (int index = objectCount - 1; index >= firstIndex; --index)
	{
		const FUObjectItem* item = TUObjectArray::GetItemByIndex(index);
		UObject* object = item ? item->Object : nullptr;
		if (!object || (item->Flags & 0x20) != 0 ||
			object->IsDefaultObject() || !object->IsA(lobbyBaseClass))
		{
			continue;
		}

		UFunction* function = object->GetFunction("OnPlaylistChanged");
		if (function)
			return function;
	}
	return nullptr;
}

static void MaintainFrontendPlaylistSelectionHooks(ULONGLONG now)
{
	if (VersionInfo.FortniteVersion >= 19.0 &&
		VersionInfo.FortniteVersion < 27.0)
	{
		// These classes were introduced with the newer Discovery browser. The
		// cross-version lobby callback below owns the selection edge before 27.
		g_FrontendModeSetSelectionInstalled.store(
			true, std::memory_order_release);
		g_FrontendActivityConfirmedInstalled.store(
			true, std::memory_order_release);
	}

	if (VersionInfo.FortniteVersion < 19.0 ||
		now < g_NextFrontendSelectionHookResolveAt ||
		(g_FrontendPlaylistChangedBaseInstalled.load(
			std::memory_order_acquire) &&
		 g_FrontendPlaylistChangedDerivedInstalled.load(
			std::memory_order_acquire) &&
		 g_FrontendModeSetSelectionInstalled.load(
			std::memory_order_acquire) &&
		 g_FrontendActivityConfirmedInstalled.load(
			std::memory_order_acquire)))
	{
		return;
	}
	g_NextFrontendSelectionHookResolveAt = now + 1000;

	// 27.11+ exposes exact zero-parameter Discovery commit events. They are
	// the cleanest child-activity edge because BR remains the parent playlist.
	if (!g_FrontendModeSetSelectionInstalled.load(
			std::memory_order_acquire))
	{
		TryInstallArenaVisualTrace(
			FindArenaVisualTraceFunction(
				L"/Script/DiscoveryBrowserUI.FortActivityModeSetSelectionModal.SaveSelectionAndClose"),
			EArenaVisualTraceSchema::ZeroParameters,
			FrontendModeSetSelectionHook,
			g_FrontendModeSetSelectionOriginal,
			g_FrontendModeSetSelectionInstalled,
			nullptr,
			"discovery-mode-set-selection");
	}
	if (!g_FrontendActivityConfirmedInstalled.load(
			std::memory_order_acquire))
	{
		TryInstallArenaVisualTrace(
			FindArenaVisualTraceFunction(
				L"/Script/DiscoveryBrowserUI.FortActivityTileDetailsDisplay.BroadcastOnActivityConfirmed"),
			EArenaVisualTraceSchema::ZeroParameters,
			FrontendActivityConfirmedHook,
			g_FrontendActivityConfirmedOriginal,
			g_FrontendActivityConfirmedInstalled,
			nullptr,
			"discovery-activity-confirmed");
	}

	UFunction* baseFunction = FindArenaVisualTraceFunction(
		L"/Script/FortniteUI.AthenaLobbyBase.OnPlaylistChanged");
	if (!g_FrontendPlaylistChangedBaseInstalled.load(
			std::memory_order_acquire))
	{
		TryInstallFrontendPlaylistChangedHook(
			baseFunction,
			FrontendPlaylistChangedBaseHook,
			g_FrontendPlaylistChangedBaseOriginal,
			g_FrontendPlaylistChangedBaseInstalled,
			"AthenaLobbyBase");
	}

	if (!g_FrontendPlaylistChangedDerivedInstalled.load(
			std::memory_order_acquire))
	{
		__try
		{
			UFunction* derivedFunction =
				FindLiveFrontendPlaylistChangedFunctionUnsafe(
					FindClass("AthenaLobbyBase"), baseFunction);
			if (derivedFunction == baseFunction && baseFunction &&
				g_FrontendPlaylistChangedBaseInstalled.load(
					std::memory_order_acquire))
			{
				// The live object inherited the exact base UFunction, so the
				// installed base hook already observes its dispatch.
				g_FrontendPlaylistChangedDerivedInstalled.store(
					true, std::memory_order_release);
			}
			else if (derivedFunction)
			{
				TryInstallFrontendPlaylistChangedHook(
					derivedFunction,
					FrontendPlaylistChangedDerivedHook,
					g_FrontendPlaylistChangedDerivedOriginal,
					g_FrontendPlaylistChangedDerivedInstalled,
					"live-derived-lobby");
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
		}
	}
}

static bool TryRefreshTournamentManager(UFortCheatManager* manager)
{
	if (!IsLiveUObject(manager))
		return false;

	__try
	{
		UFunction* function = manager->GetFunction(
			"RefreshTournamentManager");
		if (!IsLiveUObject(function))
			return false;

		// This API is a zero-parameter native exec function in supported Arena
		// clients. Reject a changed schema instead of guessing a parameter layout.
		const int32 parameterSize = function->GetPropertiesSize();
		if (parameterSize != 0)
		{
			AtlasDiagnostics::WriteLine(
				"tournament-refresh skipped schema-size=%d",
				parameterSize);
			return false;
		}

		g_FrontendTournamentRefreshDispatchActive = true;
		manager->ProcessEvent(function, nullptr);
		g_FrontendTournamentRefreshDispatchActive = false;
		return true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		g_FrontendTournamentRefreshDispatchActive = false;
		return false;
	}
}

static void MaintainFrontendTournamentRefresh(
	UWorld* world,
	AFortPlayerControllerAthena* controller,
	UFortCheatManager* manager,
	ULONGLONG now)
{
	using namespace AtlasTournamentRefreshPolicy;

	if (!world || !controller || !IsFrontendWorld(world))
	{
		g_FrontendTournamentRefresh = {};
		return;
	}

	MaintainFrontendPlaylistSelectionHooks(now);

	auto& state = g_FrontendTournamentRefresh;
	if (state.World != world || state.Controller != controller)
	{
		state.World = world;
		state.Controller = controller;
		state.NextAttemptAt = now + InitialDelayMs;
		state.Attempts = 0;
		state.SelectionTriggeredBurst = false;
		state.SelectionBurstGeneration = 0;
		state.ReboundSelectionBurstGeneration = 0;
		state.ObservedSelection.clear();
		state.ObservedSelectionRevision =
			g_FrontendSelectionRevision.load(
				std::memory_order_acquire);
		state.HasObservedSelection =
			TryGetLastSelectedSelectionIdentity(
				&state.ObservedSelection);
		AtlasDiagnostics::WriteLine(
			"tournament-refresh scheduled world=%p pc=%p "
			"delay-ms=%llu selection=%s revision=%llu",
			world, controller,
			static_cast<unsigned long long>(InitialDelayMs),
			state.HasObservedSelection
				? state.ObservedSelection.c_str()
				: "<unavailable>",
			static_cast<unsigned long long>(
				state.ObservedSelectionRevision));
	}

	bool selectionRearmed = false;
	const uint64 selectionRevision =
		g_FrontendSelectionRevision.load(std::memory_order_acquire);
	if (ShouldRearmForSelectionRevision(
			state.ObservedSelectionRevision, selectionRevision))
	{
		const uint64 previousRevision =
			state.ObservedSelectionRevision;
		state.ObservedSelectionRevision = selectionRevision;
		state.NextAttemptAt = now + SelectionDelayMs;
		state.Attempts = 0;
		state.SelectionTriggeredBurst = true;
		if (++state.SelectionBurstGeneration == 0)
			++state.SelectionBurstGeneration;
		selectionRearmed = true;
		AtlasDiagnostics::WriteLine(
			"tournament-refresh selection-rearmed "
			"source=callback previous-revision=%llu "
			"current-revision=%llu delay-ms=%llu",
			static_cast<unsigned long long>(previousRevision),
			static_cast<unsigned long long>(selectionRevision),
			static_cast<unsigned long long>(SelectionDelayMs));
	}

	UEAllocatedString selectedIdentity;
	if (TryGetLastSelectedSelectionIdentity(&selectedIdentity))
	{
		if (!state.HasObservedSelection)
		{
			state.ObservedSelection = selectedIdentity;
			state.HasObservedSelection = true;
		}
		else if (ShouldRearmForSelectionChange(
				state.ObservedSelection, selectedIdentity))
		{
			const UEAllocatedString previous = state.ObservedSelection;
			state.ObservedSelection = selectedIdentity;
			if (!selectionRearmed)
			{
				state.NextAttemptAt = now + SelectionDelayMs;
				state.Attempts = 0;
				state.SelectionTriggeredBurst = true;
				if (++state.SelectionBurstGeneration == 0)
					++state.SelectionBurstGeneration;
				AtlasDiagnostics::WriteLine(
					"tournament-refresh selection-rearmed "
					"source=identity previous=%s current=%s "
					"delay-ms=%llu",
					previous.c_str(), selectedIdentity.c_str(),
					static_cast<unsigned long long>(
						SelectionDelayMs));
			}
		}
	}

	const unsigned maxAttempts = state.SelectionTriggeredBurst
		? SelectionMaxAttempts
		: MaxAttempts;
	if (state.Attempts >= maxAttempts ||
		now < state.NextAttemptAt)
	{
		return;
	}

	const unsigned attempt = ++state.Attempts;
	const bool dispatched = TryRefreshTournamentManager(manager);
	AtlasDiagnostics::WriteLine(
		"tournament-refresh attempt=%u dispatched=%d world=%p pc=%p manager=%p",
		attempt, dispatched ? 1 : 0, world, controller, manager);

	if (ShouldDispatchSelectionRebind(
			state.SelectionTriggeredBurst,
			state.SelectionBurstGeneration,
			state.ReboundSelectionBurstGeneration))
	{
		const bool rebound = TryRebindFrontendSelectedActivity(controller);
		if (rebound)
		{
			state.ReboundSelectionBurstGeneration =
				state.SelectionBurstGeneration;
		}
		AtlasDiagnostics::WriteLine(
			"tournament-refresh selection-rebind "
			"generation=%llu dispatched=%d attempt=%u",
			static_cast<unsigned long long>(
				state.SelectionBurstGeneration),
			rebound ? 1 : 0,
			attempt);
	}

	const ULONGLONG retryDelay = state.SelectionTriggeredBurst
		? SelectionRetryDelayMs
		: RetryDelayMs;
	state.NextAttemptAt = state.Attempts < maxAttempts
		? now + retryDelay
		: 0;
}

static bool HandleDurianFinalPhaseCue(
	const wchar_t* text, int textLength)
{
	static constexpr wchar_t cue[] = L"ATLAS_DURIAN_PHASE:5";
	static constexpr int cueLength =
		static_cast<int>(_countof(cue) - 1);
	if (fabs(VersionInfo.FortniteVersion - 27.11) >= 0.001 ||
		!g_DurianPhaseSyncInstalled.load(std::memory_order_acquire) ||
		!text || textLength != cueLength ||
		wmemcmp(text, cue, static_cast<size_t>(cueLength)) != 0)
	{
		return false;
	}

	auto world = UWorld::GetWorld();
	if (world)
	{
		g_DurianPendingPhaseWorld.store(
			world, std::memory_order_release);
		g_DurianPendingPhaseIndex.store(
			5, std::memory_order_release);
		AtlasDiagnostics::WriteLine(
			"durian-phase-sync early-cue phase=5 world=%p",
			world);
		ProcessDurianPhaseSync(world);
	}
	else
	{
		AtlasDiagnostics::WriteLine(
			"durian-phase-sync early-cue ignored no-world");
	}

	return true;
}

static bool IsDurianSpecialEventScript(const AActor* script)
{
	if (!script || !script->Class)
		return false;

	const auto className = script->Class->Name.ToString();
	return className == "BP_Durian_SpecialEventScript_C";
}

static int32 ResolveDurianPhaseIndex(const AActor* phaseActor)
{
	if (!phaseActor || !phaseActor->Class)
		return -1;

	const auto className = phaseActor->Class->Name.ToString();
	static constexpr char prefix[] = "BP_Durian_Phase";
	const size_t prefixPosition = className.find(prefix);
	if (prefixPosition == decltype(className)::npos)
		return -1;

	const size_t numberPosition =
		prefixPosition + sizeof(prefix) - 1;
	if (numberPosition >= className.size())
		return -1;

	const int32 phaseNumber =
		static_cast<int32>(className[numberPosition] - '0');
	return phaseNumber >= 1 && phaseNumber <= 6
		? phaseNumber - 1
		: -1;
}

static void HookedDurianScriptActivate(
	AActor* script,
	int32 phaseIndex,
	float sequenceTimeOffset)
{
	if (IsDurianSpecialEventScript(script))
	{
		g_DurianObservedScriptPhaseIndex.store(
			phaseIndex, std::memory_order_release);
	}

	++g_DurianScriptActivationDepth;
	if (g_DurianScriptActivateOriginal)
	{
		g_DurianScriptActivateOriginal(
			script, phaseIndex, sequenceTimeOffset);
	}
	--g_DurianScriptActivationDepth;
}

static void HookedDurianPhaseActivate(
	AActor* phaseActor,
	float sequenceTimeOffset)
{
	const int32 phaseIndex = ResolveDurianPhaseIndex(phaseActor);
	if (phaseIndex >= 0)
	{
		int32 suppressedPhase = phaseIndex;
		if (g_DurianSuppressedPhaseActorIndex.compare_exchange_strong(
				suppressedPhase, -1, std::memory_order_acq_rel))
		{
			AtlasDiagnostics::WriteLine(
				"durian-phase-sync suppressed duplicate phase=%d actor=%p",
				phaseIndex, phaseActor);
			return;
		}
	}

	if (g_DurianSuppressPhaseActorActivation)
		return;

	if (g_DurianPhaseActivateOriginal)
	{
		g_DurianPhaseActivateOriginal(
			phaseActor, sequenceTimeOffset);
	}

	if (g_DurianScriptActivationDepth > 0)
		return;

	if (phaseIndex < 0 ||
		g_DurianObservedScriptPhaseIndex.load(
			std::memory_order_acquire) == phaseIndex)
	{
		return;
	}

	g_DurianPendingPhaseWorld.store(
		UWorld::GetWorld(), std::memory_order_release);
	g_DurianPendingPhaseIndex.store(
		phaseIndex, std::memory_order_release);
	AtlasDiagnostics::WriteLine(
		"durian-phase-sync queued phase=%d actor=%p offset=%.3f",
		phaseIndex, phaseActor, sequenceTimeOffset);
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

static bool IsPlacedBuildingEditCandidate(AActor* candidate)
{
	return candidate &&
		IsLiveUObject(candidate) &&
		candidate->IsA<ABuildingSMActor>() &&
		!candidate->IsA<ABuildingPlayerPrimitivePreview>();
}

static AActor* FindHighlightedPlacedBuilding(
	AFortPlayerControllerAthena* playerController)
{
	if (playerController->HasHighlightedPrimaryBuilding())
	{
		auto highlighted =
			playerController->HighlightedPrimaryBuilding.Get();
		if (IsPlacedBuildingEditCandidate(highlighted))
			return highlighted;
	}

	if (playerController->HasHighlightedPrimaryBuildings())
	{
		auto& highlighted =
			playerController->HighlightedPrimaryBuildings;
		for (int i = 0; i < highlighted.Num(); i++)
		{
			if (IsPlacedBuildingEditCandidate(highlighted[i]))
				return highlighted[i];
		}
	}

	return nullptr;
}

void PerformBuildingEditInteraction(AFortPlayerControllerAthena* _this)
{
	if (FConfiguration::bDisablePreEdits.load(
			std::memory_order_acquire) &&
		_this && _this->HasTargetedBuilding())
	{
		auto target = _this->TargetedBuilding;
		if (target &&
			target->IsA<ABuildingPlayerPrimitivePreview>())
		{
			// TargetedBuilding can lag one update behind the highlight when
			// switching away immediately after placement. Redirect that
			// transient preview target to the real highlighted piece and let
			// the original function enforce team/ownership permissions.
			auto placedTarget =
				FindHighlightedPlacedBuilding(_this);
			if (!placedTarget)
			{
				AtlasDiagnostics::WriteLine(
					"pre-edit blocked preview=%p", target);
				return;
			}

			_this->TargetedBuilding = placedTarget;
			AtlasDiagnostics::WriteLine(
				"pre-edit redirected preview=%p placed=%p",
				target, placedTarget);
		}
	}

	PerformBuildingEditInteractionOG(_this);
}

static std::atomic_bool g_ClientUObjectInitializationPending = true;
static bool g_ViewportConsoleInitializationComplete = false;
static bool g_PlaylistInitializationComplete = false;
static const UClass* g_ClientPlaylistClass = nullptr;
static int g_PlaylistScanIndex = 0;
static int g_PlaylistMatchCount = 0;
static bool g_PlaylistExtensionsInitialized = false;
static AtlasPlaylistHudPatchPolicy::FUiExtensionLayout
	g_PlaylistUiExtensionLayout{};
static TArray<FUIExtension> g_ArenaExtensions;
static TArray<FUIExtension> g_ShowdownExtensions;
static std::atomic_bool g_ClientUObjectInitializationRunning = false;
static ULONGLONG g_NextViewportConsoleAttemptAt = 0;
static ULONGLONG g_NextPlaylistClassResolveAt = 0;
static ULONGLONG g_NextPlaylistScanRetryAt = 0;
static UWorld* g_CompetitivePlaylistPatchWorld = nullptr;
static std::array<const UObject*,
    AtlasPlaylistHudPatchPolicy::CompetitivePlaylists.size()>
    g_CompetitivePlaylistObjects{};
static std::size_t g_CompetitivePlaylistProbeIndex = 0;
static ULONGLONG g_NextCompetitivePlaylistProbeAt = 0;
static ULONGLONG g_NextActivePlaylistMaintenanceAt = 0;
static ULONGLONG g_NextPlaylistOnRepResolveAt = 0;
static std::atomic<ArenaVisualTraceNative>
	g_CurrentPlaylistInfoOnRepOriginal = nullptr;
static std::atomic<ArenaVisualTraceNative>
	g_CurrentPlaylistDataOnRepOriginal = nullptr;
static std::atomic_bool g_CurrentPlaylistInfoOnRepInstalled = false;
static std::atomic_bool g_CurrentPlaylistDataOnRepInstalled = false;

static const UClass* LoadArenaScoringHudClass();
static const UClass* LoadShowdownScoringHudClass();
static bool IsWidgetInViewport(
	UObject* widget,
	bool* supported);
static bool IsWidgetVisibleThroughHudHierarchy(UObject* widget);
static bool RemoveArenaScoringHudFromViewport(UObject* widget);

struct FArenaScoringHudRepairState
{
	static constexpr std::size_t MaximumRejectedAlerts = 16;
	UWorld* World = nullptr;
	UObject* AthenaHud = nullptr;
	UObject* ScoringAlert = nullptr;
	UObject* OwnedScoringAlert = nullptr;
	AtlasArenaHudOwnershipPolicy::FObjectIdentity
		OwnedScoringAlertIdentity{};
	AtlasArenaHudOwnershipPolicy::FObjectIdentity
		ObservedNativeScoringAlertIdentity{};
	std::uint64_t ObservedNativeSessionOrdinal = 0;
	std::array<
		AtlasArenaHudOwnershipPolicy::FObjectIdentity,
		MaximumRejectedAlerts> RejectedScoringAlerts{};
	std::size_t RejectedAlertCursor = 0;
	const UClass* ArenaScoringHudClass = nullptr;
	ULONGLONG NextAttemptAt = 0;
	ULONGLONG NextObjectScanAt = 0;
	unsigned Attempts = 0;
	bool DuplicateAuditRequested = true;
	bool UsesHudOwnerSource = false;
	bool InitialUpdateAttempted = false;
	unsigned DirectCreateAttempts = 0;
	ULONGLONG NextDirectCreateAt = 0;
	bool ExhaustionLogged = false;
	bool OwnedFallbackNativeRetirementPending = false;
	ULONGLONG NativeHandoffRetireAt = 0;
	ULONGLONG NativeHandoffRetryAt = 0;
};

static FArenaScoringHudRepairState g_ArenaScoringHudRepair;

struct FPendingArenaOwnedScoringAlertRemoval
{
	UObject* Widget = nullptr;
	AtlasArenaHudOwnershipPolicy::FObjectIdentity Identity{};
	ULONGLONG RetryAt = 0;
	bool Active = false;
};

static std::array<FPendingArenaOwnedScoringAlertRemoval, 8>
	g_PendingArenaOwnedScoringAlertRemovals{};

static AtlasArenaHudOwnershipPolicy::FObjectIdentity
CaptureArenaScoringAlertIdentity(const UObject* alert);
static bool IsExactObservedNativeArenaScoringAlert(const UObject* alert);

static bool IsVerifiedNativeOwnedArenaScoringAlert(UObject* alert)
{
	// A directly-created fallback is visible, but legacy native RPCs are not
	// guaranteed to discover it through their HUD ownership chain. Only an
	// adopted/native widget can prove presentation when no consumer observer
	// exists; the manual widget must remain driven by the bridge.
	return !IsExactOwnedArenaScoringAlert(alert) &&
		IsVerifiedVisibleArenaScoringAlert(alert);
}

static bool IsVerifiedClientOwnedArenaScoringAlert(UObject* alert)
{
	return alert && IsExactOwnedArenaScoringAlert(alert) &&
		!g_ArenaScoringHudRepair.DuplicateAuditRequested &&
		IsVerifiedVisibleArenaScoringAlert(alert);
}

static bool IsUsableArenaScoringAlertIdentity(
	const AtlasArenaHudOwnershipPolicy::FObjectIdentity& identity)
{
	return AtlasArenaHudOwnershipPolicy::IsSameObjectInstance(
		identity, identity);
}

static bool MatchesArenaScoringAlertIdentity(
	const UObject* alert,
	const AtlasArenaHudOwnershipPolicy::FObjectIdentity& expected)
{
	if (!alert || alert != expected.Address ||
		!IsUsableArenaScoringAlertIdentity(expected))
	{
		return false;
	}
	__try
	{
		// Resolve from the stored slot and serial without dereferencing Address.
		// A destroyed UObject pointer can fault before FWeakObjectPtr gets a
		// chance to compare its recycled slot.
		const FUObjectItem* item =
			TUObjectArray::GetItemByIndex(expected.ObjectIndex);
		return item && item->Object == expected.Address &&
			item->SerialNumber == expected.SerialNumber;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return false;
	}
}

static bool IsExactOwnedArenaScoringAlert(const UObject* alert)
{
	return alert == g_ArenaScoringHudRepair.OwnedScoringAlert &&
		MatchesArenaScoringAlertIdentity(
			alert,
			g_ArenaScoringHudRepair.OwnedScoringAlertIdentity);
}

static bool IsExactObservedNativeArenaScoringAlert(
	const UObject* alert)
{
	return alert &&
		g_ArenaScoringHudRepair.ObservedNativeSessionOrdinal ==
			g_ArenaSessionBridge.Session.SessionOrdinal &&
		MatchesArenaScoringAlertIdentity(
			alert,
			g_ArenaScoringHudRepair.
				ObservedNativeScoringAlertIdentity);
}

static void ClearOwnedArenaScoringAlertTracking()
{
	g_ArenaOwnedScoringAlertForObserver.store(
		nullptr, std::memory_order_release);
	g_ArenaScoringHudRepair.OwnedScoringAlert = nullptr;
	g_ArenaScoringHudRepair.OwnedScoringAlertIdentity = {};
	g_ArenaScoringHudRepair.OwnedFallbackNativeRetirementPending = false;
	g_ArenaScoringHudRepair.NativeHandoffRetireAt = 0;
	g_ArenaScoringHudRepair.NativeHandoffRetryAt = 0;
}

static void BeginArenaNativeHandoffRetirement()
{
	if (g_ArenaScoringHudRepair.OwnedFallbackNativeRetirementPending)
		return;
	g_ArenaScoringHudRepair.OwnedFallbackNativeRetirementPending = true;
	// Once a validated native widget is consuming Arena events it owns both
	// presentation and HUD scaling. Retire ATLAS's exact fallback on this HUD
	// pass; keeping it through another animation window exposes two totals and
	// leaves the raw viewport widget at a different scale.
	g_ArenaScoringHudRepair.NativeHandoffRetireAt = GetTickCount64();
	g_ArenaScoringHudRepair.NativeHandoffRetryAt = 0;
}

static void ResetArenaObservedNativeCandidate()
{
	g_ArenaObservedNativeScoringAlert.store(
		nullptr, std::memory_order_release);
	g_ArenaObservedNativeScoringAlertWorld.store(
		nullptr, std::memory_order_release);
	g_ArenaObservedNativeScoringAlertSessionOrdinal.store(
		0, std::memory_order_release);
	g_ArenaScoringHudRepair.ObservedNativeScoringAlertIdentity = {};
	g_ArenaScoringHudRepair.ObservedNativeSessionOrdinal = 0;
}

static AtlasArenaHudOwnershipPolicy::FObjectIdentity
CaptureArenaScoringAlertIdentity(const UObject* alert)
{
	if (!alert)
		return {};
	__try
	{
		// FWeakObjectPtr assigns a serial when an older build has not created
		// one yet. Retaining the serial prevents a later widget allocated at
		// the same address/object slot from inheriting a stale rejection.
		const FWeakObjectPtr identity(alert);
		return {
			alert,
			identity.ObjectIndex,
			identity.ObjectSerialNumber
		};
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return {};
	}
}

static bool IsRejectedArenaScoringAlert(const UObject* alert)
{
	if (!alert)
		return false;
	const auto identity = CaptureArenaScoringAlertIdentity(alert);
	for (const auto& rejected :
		g_ArenaScoringHudRepair.RejectedScoringAlerts)
	{
		if (AtlasArenaHudOwnershipPolicy::IsSameObjectInstance(
				rejected, identity))
			return true;
	}
	return false;
}

static void RejectArenaScoringAlert(UObject* alert)
{
	if (!alert || IsRejectedArenaScoringAlert(alert))
		return;
	const auto identity = CaptureArenaScoringAlertIdentity(alert);
	if (!identity.Address || identity.SerialNumber == 0)
		return;
	auto& state = g_ArenaScoringHudRepair;
	state.RejectedScoringAlerts[
		state.RejectedAlertCursor %
			state.RejectedScoringAlerts.size()] = identity;
	++state.RejectedAlertCursor;
}

static void ApplyLegacySprintByDefault(
	AFortPlayerControllerAthena* playerController)
{
	if (VersionInfo.FortniteVersion >= 5.00 ||
		!playerController ||
		!playerController->HasbWantsToSprint())
	{
		return;
	}

	const bool enabled =
		FConfiguration::bSprintByDefault.load(
			std::memory_order_acquire);
	static bool wasEnabled = false;

	// bWantsToSprint is the pre-5.00 controller's local sprint intent.
	// Reassert it while enabled because the normal input/reload/build paths
	// are allowed to clear it. The game's own movement code still decides
	// whether the pawn can sprint in its current state.
	if (enabled)
		playerController->bWantsToSprint = true;
	else if (wasEnabled)
		playerController->bWantsToSprint = false;

	wasEnabled = enabled;
}

namespace
{
	constexpr uint64 kFirstPersonParameterFlag = 0x80;
	constexpr uint64 kFirstPersonReturnFlag = 0x400;
	constexpr size_t kMaximumFirstPersonParameterBytes = 0x20;
	constexpr ULONGLONG kFirstPersonRetryMilliseconds = 500;
	constexpr size_t kMaximumFirstPersonPrimitiveComponents = 128;
	constexpr size_t kMaximumFirstPersonComponentParameterBytes = 0x40;
	constexpr size_t kMaximumFirstPersonReflectedValueBytes = 8;

	struct FFirstPersonBoolSetter
	{
		UFunction* Function = nullptr;
		uint32 ParameterSize = 0;
		uint32 ValueOffset = 0;
		uint8 ValueMask = 0;
	};

	struct FFirstPersonBoolFieldLayout
	{
		uint32 Offset = 0;
		uint8 Mask = 0;
		bool IsValid = false;
	};

	struct FFirstPersonValueFieldLayout
	{
		uint32 Offset = 0;
		uint32 Size = 0;
		bool IsValid = false;
	};

	enum class EFirstPersonPawnHideMethod : uint8
	{
		None,
		PrimitiveOwnerNoSee
	};

	struct FFirstPersonPrimitiveVisibilityState
	{
		UObject* Component = nullptr;
		FWeakObjectPtr ComponentIdentity{};
		FFirstPersonBoolSetter SetOwnerNoSee{};
		FFirstPersonBoolFieldLayout OwnerNoSeeLayout{};
		bool OriginalOwnerNoSee = false;
		bool DesiredOwnerNoSee = true;
	};

	struct FFirstPersonAnimationUpdateState
	{
		UObject* Component = nullptr;
		FWeakObjectPtr ComponentIdentity{};
		FFirstPersonValueFieldLayout VisibilityTickOption{};
		std::array<uint8, kMaximumFirstPersonReflectedValueBytes>
			OriginalVisibilityTickOption{};
		FFirstPersonBoolFieldLayout UpdateRateOptimization{};
		bool OriginalUpdateRateOptimization = false;
		bool HasUpdateRateOptimization = false;
	};

	struct FFirstPersonRuntimeState
	{
		UWorld* World = nullptr;
		AFortPlayerControllerAthena* Controller = nullptr;
		FWeakObjectPtr ControllerIdentity{};
		AFortPlayerPawnAthena* Pawn = nullptr;
		FWeakObjectPtr PawnIdentity{};
		UObject* BodyMesh = nullptr;
		FWeakObjectPtr BodyMeshIdentity{};
		EFirstPersonPawnHideMethod HideMethod =
			EFirstPersonPawnHideMethod::None;
		std::array<FFirstPersonPrimitiveVisibilityState,
			kMaximumFirstPersonPrimitiveComponents> PrimitiveVisibility{};
		size_t PrimitiveVisibilityCount = 0;
		std::array<FFirstPersonAnimationUpdateState,
			kMaximumFirstPersonPrimitiveComponents> AnimationUpdates{};
		size_t AnimationUpdateCount = 0;
		ULONGLONG NextRetryAt = 0;
	};

	static FFirstPersonRuntimeState g_FirstPersonRuntime;
	static ULONGLONG g_FirstPersonNextActivationAt = 0;

	static bool MatchesFirstPersonObjectIdentity(
		const UObject* object,
		const FWeakObjectPtr& identity)
	{
		if (!object)
			return false;
		__try
		{
			return identity.Get() == object;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	static UObject* InvokeFirstPersonObjectGetterUnsafe(
		UObject* target,
		const char* functionName)
	{
		if (!IsLiveUObject(target) || !functionName || !*functionName)
			return nullptr;
		UFunction* function = target->GetFunction(functionName);
		if (!IsLiveUObject(function))
			return nullptr;

		const auto parameters = function->GetParamsNamed();
		if (parameters.Size == 0 ||
			parameters.Size > kMaximumFirstPersonParameterBytes ||
			parameters.NameOffsetMap.size() != 1)
		{
			return nullptr;
		}

		const auto& result = parameters.NameOffsetMap[0];
		const bool isParameter =
			(result.PropertyFlags & kFirstPersonParameterFlag) != 0;
		const bool isReturn =
			(result.PropertyFlags & kFirstPersonReturnFlag) != 0;
		const UField* namedProperty = function->GetProperty(
			result.Name.c_str());
		const UField* objectProperty = function->GetProperty(
			result.Name.c_str(), 0x10000);
		if (!isParameter || !isReturn ||
			!namedProperty || namedProperty != objectProperty ||
			result.ElementSize != sizeof(UObject*) ||
			result.Offset > parameters.Size ||
			sizeof(UObject*) > parameters.Size - result.Offset)
		{
			return nullptr;
		}

		std::array<uint8, kMaximumFirstPersonParameterBytes>
			storage{};
		target->ProcessEvent(function, storage.data());
		UObject* value = nullptr;
		memcpy(&value, storage.data() + result.Offset, sizeof(value));
		return IsLiveUObject(value) ? value : nullptr;
	}

	static UObject* InvokeFirstPersonObjectGetter(
		UObject* target,
		const char* functionName)
	{
		__try
		{
			return InvokeFirstPersonObjectGetterUnsafe(
				target, functionName);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return nullptr;
		}
	}

	static UObject* InvokeFirstPersonPartMeshGetterUnsafe(
		AFortPlayerPawnAthena* pawn,
		uint8 partType)
	{
		if (!IsLiveUObject(pawn))
			return nullptr;

		UFunction* function = nullptr;
		for (const char* functionName : {
			"GetSkeletalMeshForPartType",
			"GetPartSkeletalMeshComponent" })
		{
			function = pawn->GetFunction(functionName);
			if (IsLiveUObject(function))
				break;
		}
		if (!IsLiveUObject(function))
			return nullptr;

		const auto parameters = function->GetParamsNamed();
		if (parameters.Size == 0 ||
			parameters.Size > kMaximumFirstPersonParameterBytes)
		{
			return nullptr;
		}

		uint32 partTypeOffset = UINT32_MAX;
		uint32 returnOffset = UINT32_MAX;
		unsigned inputMatches = 0;
		unsigned returnMatches = 0;
		for (const auto& parameter : parameters.NameOffsetMap)
		{
			const bool isParameter =
				(parameter.PropertyFlags &
					kFirstPersonParameterFlag) != 0;
			const bool isReturn =
				(parameter.PropertyFlags &
					kFirstPersonReturnFlag) != 0;
			if (!isParameter)
				continue;

			const bool isPartType =
				parameter.Name == "PartType" ||
				parameter.Name == "CharacterPartType" ||
				parameter.Name == "Part";
			if (!isReturn && isPartType &&
				parameter.ElementSize == sizeof(uint8) &&
				parameter.Offset < parameters.Size)
			{
				partTypeOffset = parameter.Offset;
				++inputMatches;
			}
			else if (isReturn &&
				parameter.Name == "ReturnValue" &&
				parameter.ElementSize == sizeof(UObject*) &&
				parameter.Offset <= parameters.Size &&
				sizeof(UObject*) <=
					parameters.Size - parameter.Offset)
			{
				returnOffset = parameter.Offset;
				++returnMatches;
			}
			else
			{
				return nullptr;
			}
		}
		if (inputMatches != 1 || returnMatches != 1 ||
			partTypeOffset == UINT32_MAX ||
			returnOffset == UINT32_MAX)
		{
			return nullptr;
		}

		alignas(16) std::array<uint8,
			kMaximumFirstPersonParameterBytes> storage{};
		storage[partTypeOffset] = partType;
		pawn->ProcessEvent(function, storage.data());

		UObject* component = nullptr;
		memcpy(&component, storage.data() + returnOffset,
			sizeof(component));
		if (!IsLiveUObject(component))
			return nullptr;

		const UClass* skeletalMeshComponentClass =
			FindClass("SkeletalMeshComponent");
		if (IsLiveUObject(skeletalMeshComponentClass) &&
			!component->IsA(skeletalMeshComponentClass))
		{
			return nullptr;
		}
		return component;
	}

	static UObject* ResolveFirstPersonBodyMeshComponent(
		AFortPlayerPawnAthena* pawn)
	{
		__try
		{
			// EFortCustomPartType::Body has remained slot 1 from the legacy
			// modular-character builds through the current first-person mode.
			UObject* component = InvokeFirstPersonPartMeshGetterUnsafe(
				pawn, 1);
			if (IsLiveUObject(component))
				return component;

			// Very old/custom branches may omit the part getter. Preserve their
			// primary character mesh so the camera still has rendered arms and a
			// full-rate socket source for the separately owned weapon actor.
			for (const char* propertyName : {
				"Mesh", "CharacterMesh", "SkeletalMesh",
				"MeshComponent" })
			{
				component = ReadStrongObjectProperty(
					pawn, propertyName);
				if (IsLiveUObject(component))
					return component;
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return nullptr;
		}
		return nullptr;
	}

	static AFortPlayerPawnAthena* ResolveFirstPersonPawn(
		AFortPlayerControllerAthena* playerController)
	{
		if (!IsLiveUObject(playerController))
			return nullptr;

		for (const char* functionName : {
			"GetPlayerPawn", "GetPawn", "K2_GetPawn" })
		{
			UObject* candidate = InvokeFirstPersonObjectGetter(
				playerController, functionName);
			if (IsLiveUObject(candidate))
			{
				return reinterpret_cast<AFortPlayerPawnAthena*>(
					candidate);
			}
		}

		// Property availability moved between Controller, PlayerController and
		// FortPlayerController across supported builds. Resolve by reflection
		// first so one missing generated field cannot suppress the feature.
		for (const char* propertyName : {
			"MyFortPawn", "Pawn", "AcknowledgedPawn", "Character" })
		{
			UObject* candidate = ReadStrongObjectProperty(
				playerController, propertyName);
			if (IsLiveUObject(candidate))
			{
				return reinterpret_cast<AFortPlayerPawnAthena*>(
					candidate);
			}
		}

		AFortPlayerPawnAthena* pawn = nullptr;
		__try
		{
			if (playerController->HasMyFortPawn())
				pawn = playerController->MyFortPawn;
			if (!IsLiveUObject(pawn) && playerController->HasPawn())
				pawn = playerController->Pawn;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return nullptr;
		}
		return IsLiveUObject(pawn) ? pawn : nullptr;
	}

	static bool ResolveFirstPersonBoolParameter(
		UFunction* function,
		bool returnParameter,
		uint32* functionSize,
		uint32* valueOffset,
		uint8* valueMask)
	{
		if (!IsLiveUObject(function) || !functionSize ||
			!valueOffset || !valueMask)
		{
			return false;
		}

		const auto parameters = function->GetParamsNamed();
		if (parameters.Size == 0 ||
			parameters.Size > kMaximumFirstPersonParameterBytes)
		{
			return false;
		}

		unsigned matches = 0;
		for (const auto& parameter : parameters.NameOffsetMap)
		{
			const bool isParameter =
				(parameter.PropertyFlags &
					kFirstPersonParameterFlag) != 0;
			const bool isReturn =
				(parameter.PropertyFlags &
					kFirstPersonReturnFlag) != 0;
			if (!isParameter || isReturn != returnParameter)
				continue;
			if (parameter.ElementSize != sizeof(bool) ||
				parameter.Offset >= parameters.Size)
			{
				return false;
			}

			const UField* property = function->GetProperty(
				parameter.Name.c_str(), 0x20000);
			const uint8 fieldMask = property
				? property->GetFieldMask()
				: 0;
			if (fieldMask == 0)
				return false;

			*valueOffset = parameter.Offset;
			*valueMask = fieldMask;
			++matches;
		}

		if (matches != 1)
			return false;
		*functionSize = parameters.Size;
		return true;
	}

	static bool ResolveFirstPersonBoolSetter(
		UObject* target,
		const char* functionName,
		FFirstPersonBoolSetter* setter)
	{
		if (setter)
			*setter = {};
		if (!IsLiveUObject(target) || !functionName || !*functionName ||
			!setter)
		{
			return false;
		}

		FFirstPersonBoolSetter resolved{};
		resolved.Function = target->GetFunction(functionName);
		if (!ResolveFirstPersonBoolParameter(
				resolved.Function, false,
				&resolved.ParameterSize,
				&resolved.ValueOffset,
				&resolved.ValueMask))
		{
			return false;
		}

		*setter = resolved;
		return true;
	}

	static bool InvokeFirstPersonBoolSetter(
		UObject* target,
		const FFirstPersonBoolSetter& setter,
		bool value)
	{
		if (!IsLiveUObject(target) ||
			!IsLiveUObject(setter.Function) ||
			setter.ParameterSize == 0 ||
			setter.ParameterSize > kMaximumFirstPersonParameterBytes ||
			setter.ValueOffset >= setter.ParameterSize ||
			setter.ValueMask == 0)
		{
			return false;
		}

		alignas(16) std::array<uint8,
			kMaximumFirstPersonParameterBytes> parameters{};
		parameters[setter.ValueOffset] =
			value ? setter.ValueMask : 0;
		target->ProcessEvent(setter.Function, parameters.data());
		return true;
	}

	static bool ResolveFirstPersonBoolFieldLayoutUnsafe(
		UObject* object,
		const char* propertyName,
		FFirstPersonBoolFieldLayout* layout)
	{
		if (layout)
			*layout = {};
		if (!IsLiveUObject(object) || !object->Class ||
			!propertyName || !*propertyName || !layout)
		{
			return false;
		}

		const UField* property = object->GetProperty(
			propertyName, 0x20000);
		if (!property)
			return false;
		const uint32 offset = GetFromOffset<uint32>(
			property, Offsets::Offset_Internal);
		const uint8 mask = property->GetFieldMask();
		const int32 ownerSize = object->Class->GetPropertiesSize();
		if (mask == 0 || ownerSize <= 0 ||
			offset >= static_cast<uint32>(ownerSize))
		{
			return false;
		}

		layout->Offset = offset;
		layout->Mask = mask;
		layout->IsValid = true;
		return true;
	}

	static bool ResolveFirstPersonBoolFieldLayout(
		UObject* object,
		const char* propertyName,
		FFirstPersonBoolFieldLayout* layout)
	{
		__try
		{
			return ResolveFirstPersonBoolFieldLayoutUnsafe(
				object, propertyName, layout);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			if (layout)
				*layout = {};
			return false;
		}
	}

	static bool ReadFirstPersonBoolFieldUnsafe(
		const UObject* object,
		const FFirstPersonBoolFieldLayout& layout,
		bool* value)
	{
		if (!IsLiveUObject(object) || !layout.IsValid ||
			layout.Mask == 0 || !value)
		{
			return false;
		}
		*value =
			(GetFromOffset<uint8>(object, layout.Offset) &
				layout.Mask) != 0;
		return true;
	}

	static bool ReadFirstPersonBoolField(
		const UObject* object,
		const FFirstPersonBoolFieldLayout& layout,
		bool* value)
	{
		__try
		{
			return ReadFirstPersonBoolFieldUnsafe(
				object, layout, value);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	static bool WriteFirstPersonBoolFieldUnsafe(
		UObject* object,
		const FFirstPersonBoolFieldLayout& layout,
		bool value)
	{
		if (!IsLiveUObject(object) || !layout.IsValid ||
			layout.Mask == 0)
		{
			return false;
		}

		auto& storage = GetFromOffset<uint8>(object, layout.Offset);
		if (value)
			storage |= layout.Mask;
		else
			storage &= static_cast<uint8>(~layout.Mask);
		return (((storage & layout.Mask) != 0) == value);
	}

	static bool WriteFirstPersonBoolField(
		UObject* object,
		const FFirstPersonBoolFieldLayout& layout,
		bool value)
	{
		__try
		{
			return WriteFirstPersonBoolFieldUnsafe(
				object, layout, value);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	static bool ResolveFirstPersonValueFieldLayoutUnsafe(
		UObject* object,
		const char* propertyName,
		FFirstPersonValueFieldLayout* layout)
	{
		if (layout)
			*layout = {};
		if (!IsLiveUObject(object) || !object->Class ||
			!propertyName || !*propertyName || !layout ||
			!Offsets::Offset_Internal || !Offsets::ElementSize)
		{
			return false;
		}

		const UField* property = object->GetProperty(propertyName);
		if (!property)
			return false;
		const uint32 offset = GetFromOffset<uint32>(
			property, Offsets::Offset_Internal);
		const uint32 size = GetFromOffset<uint32>(
			property, Offsets::ElementSize);
		const int32 ownerSize = object->Class->GetPropertiesSize();
		if (size == 0 ||
			size > kMaximumFirstPersonReflectedValueBytes ||
			ownerSize <= 0 ||
			offset > static_cast<uint32>(ownerSize) ||
			size > static_cast<uint32>(ownerSize) - offset)
		{
			return false;
		}

		layout->Offset = offset;
		layout->Size = size;
		layout->IsValid = true;
		return true;
	}

	static bool ResolveFirstPersonValueFieldLayout(
		UObject* object,
		const char* propertyName,
		FFirstPersonValueFieldLayout* layout)
	{
		__try
		{
			return ResolveFirstPersonValueFieldLayoutUnsafe(
				object, propertyName, layout);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			if (layout)
				*layout = {};
			return false;
		}
	}

	static bool ReadFirstPersonValueFieldUnsafe(
		const UObject* object,
		const FFirstPersonValueFieldLayout& layout,
		std::array<uint8,
			kMaximumFirstPersonReflectedValueBytes>* value)
	{
		if (value)
			value->fill(0);
		if (!IsLiveUObject(object) || !layout.IsValid ||
			layout.Size == 0 ||
			layout.Size > kMaximumFirstPersonReflectedValueBytes ||
			!value)
		{
			return false;
		}

		memcpy(value->data(),
			&GetFromOffset<const uint8>(object, layout.Offset),
			layout.Size);
		return true;
	}

	static bool ReadFirstPersonValueField(
		const UObject* object,
		const FFirstPersonValueFieldLayout& layout,
		std::array<uint8,
			kMaximumFirstPersonReflectedValueBytes>* value)
	{
		__try
		{
			return ReadFirstPersonValueFieldUnsafe(
				object, layout, value);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			if (value)
				value->fill(0);
			return false;
		}
	}

	static bool WriteFirstPersonValueFieldUnsafe(
		UObject* object,
		const FFirstPersonValueFieldLayout& layout,
		const std::array<uint8,
			kMaximumFirstPersonReflectedValueBytes>& value)
	{
		if (!IsLiveUObject(object) || !layout.IsValid ||
			layout.Size == 0 ||
			layout.Size > kMaximumFirstPersonReflectedValueBytes)
		{
			return false;
		}

		memcpy(&GetFromOffset<uint8>(object, layout.Offset),
			value.data(), layout.Size);
		return true;
	}

	static bool WriteFirstPersonValueField(
		UObject* object,
		const FFirstPersonValueFieldLayout& layout,
		const std::array<uint8,
			kMaximumFirstPersonReflectedValueBytes>& value)
	{
		__try
		{
			return WriteFirstPersonValueFieldUnsafe(
				object, layout, value);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	static bool IsFirstPersonValueZero(
		const FFirstPersonValueFieldLayout& layout,
		const std::array<uint8,
			kMaximumFirstPersonReflectedValueBytes>& value)
	{
		if (!layout.IsValid || layout.Size == 0 ||
			layout.Size > value.size())
		{
			return false;
		}
		for (uint32 index = 0; index < layout.Size; ++index)
		{
			if (value[index] != 0)
				return false;
		}
		return true;
	}

	static bool GatherFirstPersonPawnPrimitiveComponentsUnsafe(
		AFortPlayerPawnAthena* pawn,
		std::array<UObject*,
			kMaximumFirstPersonPrimitiveComponents>* components,
		size_t* componentCount)
	{
		if (componentCount)
			*componentCount = 0;
		if (!IsLiveUObject(pawn) || !components || !componentCount)
			return false;

		const UClass* primitiveComponentClass =
			FindClass("PrimitiveComponent");
		UFunction* function = nullptr;
		for (const char* functionName : {
			"GetComponentsByClass", "K2_GetComponentsByClass" })
		{
			function = pawn->GetFunction(functionName);
			if (IsLiveUObject(function))
				break;
		}
		if (!IsLiveUObject(primitiveComponentClass) ||
			!IsLiveUObject(function))
		{
			return false;
		}

		const auto reflected = function->GetParamsNamed();
		if (reflected.Size == 0 || reflected.Size >
			kMaximumFirstPersonComponentParameterBytes)
		{
			return false;
		}

		uint32 classOffset = UINT32_MAX;
		uint32 returnOffset = UINT32_MAX;
		unsigned inputMatches = 0;
		unsigned returnMatches = 0;
		for (const auto& parameter : reflected.NameOffsetMap)
		{
			const bool isParameter =
				(parameter.PropertyFlags &
					kFirstPersonParameterFlag) != 0;
			const bool isReturn =
				(parameter.PropertyFlags &
					kFirstPersonReturnFlag) != 0;
			if (!isParameter)
				continue;
			if (!isReturn && parameter.Name == "ComponentClass" &&
				parameter.ElementSize == sizeof(UClass*) &&
				parameter.Offset <= reflected.Size &&
				sizeof(UClass*) <= reflected.Size - parameter.Offset)
			{
				classOffset = parameter.Offset;
				++inputMatches;
			}
			else if (isReturn && parameter.Name == "ReturnValue" &&
				parameter.ElementSize == sizeof(TArray<UObject*>) &&
				parameter.Offset <= reflected.Size &&
				sizeof(TArray<UObject*>) <=
					reflected.Size - parameter.Offset)
			{
				returnOffset = parameter.Offset;
				++returnMatches;
			}
		}
		if (inputMatches != 1 || returnMatches != 1 ||
			classOffset == UINT32_MAX || returnOffset == UINT32_MAX)
		{
			return false;
		}

		alignas(16) std::array<uint8,
			kMaximumFirstPersonComponentParameterBytes> parameters{};
		memcpy(parameters.data() + classOffset,
			&primitiveComponentClass, sizeof(primitiveComponentClass));
		pawn->ProcessEvent(function, parameters.data());

		TArray<UObject*> returned{};
		memcpy(&returned, parameters.data() + returnOffset,
			sizeof(returned));
		if (returned.NumElements < 0 ||
			returned.MaxElements < returned.NumElements ||
			returned.MaxElements > 4096 ||
			(returned.NumElements > 0 && !returned.Data))
		{
			return false;
		}

		for (int32 index = 0; index < returned.NumElements &&
			*componentCount < components->size(); ++index)
		{
			UObject* component = returned.Data[index];
			if (!IsLiveUObject(component) ||
				!component->IsA(primitiveComponentClass))
			{
				continue;
			}
			(*components)[(*componentCount)++] = component;
		}
		returned.Free();
		return *componentCount > 0;
	}

	static bool GatherFirstPersonPawnPrimitiveComponents(
		AFortPlayerPawnAthena* pawn,
		std::array<UObject*,
			kMaximumFirstPersonPrimitiveComponents>* components,
		size_t* componentCount)
	{
		__try
		{
			return GatherFirstPersonPawnPrimitiveComponentsUnsafe(
				pawn, components, componentCount);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			if (componentCount)
				*componentCount = 0;
			return false;
		}
	}

	static bool TrackFirstPersonAnimationUpdate(
		FFirstPersonRuntimeState& state,
		UObject* component)
	{
		if (!IsLiveUObject(component))
			return false;

		for (size_t index = 0;
			index < state.AnimationUpdateCount; ++index)
		{
			const auto& tracked = state.AnimationUpdates[index];
			if (tracked.Component == component &&
				MatchesFirstPersonObjectIdentity(
					component, tracked.ComponentIdentity))
			{
				return true;
			}
		}
		if (state.AnimationUpdateCount >= state.AnimationUpdates.size())
			return false;

		// UE renamed MeshComponentUpdateFlag to
		// VisibilityBasedAnimTickOption. Both enum layouts define
		// AlwaysTickPoseAndRefreshBones as zero, so the same reflected write
		// keeps hand sockets moving while the body is owner-hidden across the
		// legacy and modern Fortnite branches.
		FFirstPersonValueFieldLayout visibilityTickOption{};
		for (const char* propertyName : {
			"VisibilityBasedAnimTickOption",
			"MeshComponentUpdateFlag" })
		{
			if (ResolveFirstPersonValueFieldLayout(
					component, propertyName, &visibilityTickOption))
			{
				break;
			}
		}
		if (!visibilityTickOption.IsValid)
			return false;

		std::array<uint8, kMaximumFirstPersonReflectedValueBytes>
			originalVisibilityTickOption{};
		if (!ReadFirstPersonValueField(
				component, visibilityTickOption,
				&originalVisibilityTickOption))
		{
			return false;
		}

		std::array<uint8, kMaximumFirstPersonReflectedValueBytes>
			alwaysTickPoseAndRefreshBones{};
		if (!WriteFirstPersonValueField(
				component, visibilityTickOption,
				alwaysTickPoseAndRefreshBones))
		{
			return false;
		}
		std::array<uint8, kMaximumFirstPersonReflectedValueBytes>
			observedVisibilityTickOption{};
		if (!ReadFirstPersonValueField(
				component, visibilityTickOption,
				&observedVisibilityTickOption) ||
			!IsFirstPersonValueZero(
				visibilityTickOption,
				observedVisibilityTickOption))
		{
			WriteFirstPersonValueField(
				component, visibilityTickOption,
				originalVisibilityTickOption);
			return false;
		}

		auto& tracked = state.AnimationUpdates[
			state.AnimationUpdateCount++];
		tracked.Component = component;
		tracked.ComponentIdentity = FWeakObjectPtr(component);
		tracked.VisibilityTickOption = visibilityTickOption;
		tracked.OriginalVisibilityTickOption =
			originalVisibilityTickOption;

		// Hidden skeletal meshes can also be throttled by update-rate
		// optimization even when their visibility policy permits ticking.
		// Disable it locally for full-rate held-item socket animation, while
		// snapshotting the exact original bit for restoration.
		FFirstPersonBoolFieldLayout updateRateOptimization{};
		bool originalUpdateRateOptimization = false;
		if (ResolveFirstPersonBoolFieldLayout(
				component, "bEnableUpdateRateOptimizations",
				&updateRateOptimization) &&
			ReadFirstPersonBoolField(
				component, updateRateOptimization,
				&originalUpdateRateOptimization) &&
			WriteFirstPersonBoolField(
				component, updateRateOptimization, false))
		{
			tracked.UpdateRateOptimization = updateRateOptimization;
			tracked.OriginalUpdateRateOptimization =
				originalUpdateRateOptimization;
			tracked.HasUpdateRateOptimization = true;
		}

		return true;
	}

	static void TrackFirstPersonPawnAnimationFallbacks(
		FFirstPersonRuntimeState& state,
		AFortPlayerPawnAthena* pawn)
	{
		if (!IsLiveUObject(pawn))
			return;

		// GetComponentsByClass exists on supported builds, but these strong
		// reflected mesh fields keep the animation fix available if a custom
		// branch strips that Blueprint function.
		for (const char* propertyName : {
			"Mesh", "CharacterMesh", "SkeletalMesh",
			"MeshComponent" })
		{
			UObject* component = ReadStrongObjectProperty(
				pawn, propertyName);
			if (IsLiveUObject(component))
			{
				TrackFirstPersonAnimationUpdate(state, component);
			}
		}
	}

	static void ClearFirstPersonPawnHideState(
		FFirstPersonRuntimeState& state)
	{
		state.Pawn = nullptr;
		state.PawnIdentity = {};
		state.BodyMesh = nullptr;
		state.BodyMeshIdentity = {};
		state.HideMethod = EFirstPersonPawnHideMethod::None;
		state.PrimitiveVisibility = {};
		state.PrimitiveVisibilityCount = 0;
		state.AnimationUpdates = {};
		state.AnimationUpdateCount = 0;
	}

	static void RestoreFirstPersonPawnVisibility(
		FFirstPersonRuntimeState& state)
	{
		// Restore every successfully snapshotted primitive even when a later
		// step selected a different fallback. Component application is bounded
		// but intentionally transactional at teardown.
		for (size_t index = 0;
			index < state.PrimitiveVisibilityCount; ++index)
		{
			auto& componentState =
				state.PrimitiveVisibility[index];
			if (!MatchesFirstPersonObjectIdentity(
					componentState.Component,
					componentState.ComponentIdentity))
			{
				continue;
			}
			InvokeFirstPersonBoolSetter(
				componentState.Component,
				componentState.SetOwnerNoSee,
				componentState.OriginalOwnerNoSee);
		}
		for (size_t index = 0;
			index < state.AnimationUpdateCount; ++index)
		{
			auto& animationState = state.AnimationUpdates[index];
			if (!MatchesFirstPersonObjectIdentity(
					animationState.Component,
					animationState.ComponentIdentity))
			{
				continue;
			}

			WriteFirstPersonValueField(
				animationState.Component,
				animationState.VisibilityTickOption,
				animationState.OriginalVisibilityTickOption);
			if (animationState.HasUpdateRateOptimization)
			{
				WriteFirstPersonBoolField(
					animationState.Component,
					animationState.UpdateRateOptimization,
					animationState.
						OriginalUpdateRateOptimization);
			}
		}
		ClearFirstPersonPawnHideState(state);
	}

	static bool TrackFirstPersonPawnPrimitiveComponent(
		FFirstPersonRuntimeState& state,
		UObject* component,
		bool desiredOwnerNoSee)
	{
		if (!IsLiveUObject(component))
			return false;

		for (size_t index = 0;
			index < state.PrimitiveVisibilityCount; ++index)
		{
			auto& tracked = state.PrimitiveVisibility[index];
			if (tracked.Component == component &&
				MatchesFirstPersonObjectIdentity(
					component, tracked.ComponentIdentity))
			{
				if (tracked.DesiredOwnerNoSee != desiredOwnerNoSee)
				{
					const bool previousDesiredOwnerNoSee =
						tracked.DesiredOwnerNoSee;
					if (!InvokeFirstPersonBoolSetter(
							component, tracked.SetOwnerNoSee,
							desiredOwnerNoSee))
					{
						return false;
					}
					bool observedOwnerNoSee = false;
					if (!ReadFirstPersonBoolField(
							component, tracked.OwnerNoSeeLayout,
							&observedOwnerNoSee) ||
						observedOwnerNoSee != desiredOwnerNoSee)
					{
						InvokeFirstPersonBoolSetter(
							component, tracked.SetOwnerNoSee,
							previousDesiredOwnerNoSee);
						return false;
					}
					tracked.DesiredOwnerNoSee = desiredOwnerNoSee;
				}
				return true;
			}
		}
		if (state.PrimitiveVisibilityCount >=
			state.PrimitiveVisibility.size())
		{
			return false;
		}

		FFirstPersonBoolSetter setOwnerNoSee{};
		FFirstPersonBoolFieldLayout ownerNoSeeLayout{};
		bool originalOwnerNoSee = false;
		if (!ResolveFirstPersonBoolSetter(
				component, "SetOwnerNoSee", &setOwnerNoSee) ||
			!ResolveFirstPersonBoolFieldLayout(
				component, "bOwnerNoSee", &ownerNoSeeLayout) ||
			!ReadFirstPersonBoolField(
				component, ownerNoSeeLayout, &originalOwnerNoSee))
		{
			return false;
		}

		const bool changedOwnerNoSee =
			originalOwnerNoSee != desiredOwnerNoSee;
		if (changedOwnerNoSee &&
			!InvokeFirstPersonBoolSetter(
				component, setOwnerNoSee, desiredOwnerNoSee))
		{
			return false;
		}
		bool ownerNoSee = false;
		if (!ReadFirstPersonBoolField(
				component, ownerNoSeeLayout, &ownerNoSee) ||
			ownerNoSee != desiredOwnerNoSee)
		{
			// Do not leave a component changed if post-write validation fails
			// before it can be added to the restoration snapshot.
			if (changedOwnerNoSee)
			{
				InvokeFirstPersonBoolSetter(
					component, setOwnerNoSee,
					originalOwnerNoSee);
			}
			return false;
		}

		auto& tracked = state.PrimitiveVisibility[
			state.PrimitiveVisibilityCount++];
		tracked.Component = component;
		tracked.ComponentIdentity = FWeakObjectPtr(component);
		tracked.SetOwnerNoSee = setOwnerNoSee;
		tracked.OwnerNoSeeLayout = ownerNoSeeLayout;
		tracked.OriginalOwnerNoSee = originalOwnerNoSee;
		tracked.DesiredOwnerNoSee = desiredOwnerNoSee;
		return true;
	}

	static bool ApplyFirstPersonPrimitiveVisibility(
		FFirstPersonRuntimeState& state,
		AFortPlayerPawnAthena* pawn)
	{
		UObject* bodyMesh = ResolveFirstPersonBodyMeshComponent(pawn);
		if (!IsLiveUObject(bodyMesh))
			return false;

		std::array<UObject*, 5> preservedMeshes{};
		size_t preservedMeshCount = 0;
		auto preserveMesh = [&](UObject* component)
		{
			if (!IsLiveUObject(component))
				return;
			for (size_t index = 0; index < preservedMeshCount; ++index)
			{
				if (preservedMeshes[index] == component)
					return;
			}
			if (preservedMeshCount < preservedMeshes.size())
				preservedMeshes[preservedMeshCount++] = component;
		};
		preserveMesh(bodyMesh);
		// The inherited character mesh can remain the animation/socket source
		// even when a modular Body component supplies the rendered geometry.
		// Keeping both rendered avoids returning the weapon to a hidden-pawn
		// update path; mesh components without geometry remain visually inert.
		for (const char* propertyName : {
			"Mesh", "CharacterMesh", "SkeletalMesh",
			"MeshComponent" })
		{
			preserveMesh(ReadStrongObjectProperty(pawn, propertyName));
		}

		std::array<UObject*, kMaximumFirstPersonPrimitiveComponents>
			components{};
		size_t componentCount = 0;
		GatherFirstPersonPawnPrimitiveComponents(
			pawn, &components, &componentCount);

		bool bodyPreserved = false;
		// Reserve restoration entries for every preserved mesh before walking
		// the bounded component list, so a cosmetic-heavy pawn cannot consume
		// the snapshot capacity before its Body/animation source is secured.
		for (size_t index = 0; index < preservedMeshCount; ++index)
		{
			TrackFirstPersonAnimationUpdate(
				state, preservedMeshes[index]);
			const bool preserved =
				TrackFirstPersonPawnPrimitiveComponent(
					state, preservedMeshes[index], false);
			if (preservedMeshes[index] == bodyMesh && preserved)
				bodyPreserved = true;
		}
		for (size_t index = 0; index < componentCount; ++index)
		{
			bool preserve = false;
			for (size_t preservedIndex = 0;
				preservedIndex < preservedMeshCount; ++preservedIndex)
			{
				if (components[index] == preservedMeshes[preservedIndex])
				{
					preserve = true;
					break;
				}
			}
			TrackFirstPersonAnimationUpdate(
				state, components[index]);
			if (TrackFirstPersonPawnPrimitiveComponent(
					state, components[index], !preserve) &&
				components[index] == bodyMesh && preserve)
			{
				bodyPreserved = true;
			}
		}

		if (bodyPreserved)
		{
			state.BodyMesh = bodyMesh;
			state.BodyMeshIdentity = FWeakObjectPtr(bodyMesh);
		}
		return bodyPreserved;
	}

	static bool ApplyFirstPersonPawnVisibility(
		FFirstPersonRuntimeState& state,
		AFortPlayerControllerAthena* playerController,
		AFortPlayerPawnAthena* pawn)
	{
		if (!IsLiveUObject(playerController) || !IsLiveUObject(pawn))
			return false;

		const bool applied =
			ApplyFirstPersonPrimitiveVisibility(state, pawn);
		TrackFirstPersonPawnAnimationFallbacks(state, pawn);

		if (!applied)
		{
			AtlasDiagnostics::WriteLine(
				"first-person body-mesh-wait pc=%p pawn=%p",
				playerController, pawn);
			return false;
		}

		state.Pawn = pawn;
		state.PawnIdentity = FWeakObjectPtr(pawn);
		state.HideMethod =
			EFirstPersonPawnHideMethod::PrimitiveOwnerNoSee;
		AtlasDiagnostics::WriteLine(
			"first-person body-mesh pc=%p pawn=%p body=%p "
			"components=%zu animations=%zu",
			playerController, pawn,
			state.BodyMesh,
			state.PrimitiveVisibilityCount,
			state.AnimationUpdateCount);
		return true;
	}

	static void ClearFirstPersonRuntimeState()
	{
		g_FirstPersonRuntime = {};
	}

	static bool RestoreFirstPersonRuntimeState(
		UWorld* currentWorld)
	{
		auto& state = g_FirstPersonRuntime;
		if (!state.World)
			return true;

		AFortPlayerControllerAthena* restoreController =
			state.Controller;
		const bool controllerAlive =
			MatchesFirstPersonObjectIdentity(
				restoreController, state.ControllerIdentity);
		const bool canRetry = state.World == currentWorld &&
			controllerAlive;

		bool cameraRestored = !controllerAlive;
		if (controllerAlive && IsLiveUObject(currentWorld))
		{
			cameraRestored = GUI_ExecuteInternalConsoleCommand(
				currentWorld, restoreController, "camera default");
		}

		AFortPlayerPawnAthena* restoredPawn = state.Pawn;
		RestoreFirstPersonPawnVisibility(state);

		if (!cameraRestored && canRetry)
			return false;

		AtlasDiagnostics::WriteLine(
			"first-person restored pc=%p pawn=%p camera=%d",
			restoreController, restoredPawn,
			cameraRestored ? 1 : 0);
		ClearFirstPersonRuntimeState();
		return true;
	}

	static bool ActivateFirstPersonRuntimeState(
		UWorld* world,
		AFortPlayerControllerAthena* playerController,
		AFortPlayerPawnAthena* pawn,
		ULONGLONG now)
	{
		if (!GUI_ExecuteInternalConsoleCommand(
				world, playerController, "camera first"))
		{
			AtlasDiagnostics::WriteLine(
				"first-person unavailable reason=camera-command pc=%p",
				playerController);
			return false;
		}

		FFirstPersonRuntimeState state{};
		state.World = world;
		state.Controller = playerController;
		state.ControllerIdentity =
			FWeakObjectPtr(playerController);
		state.NextRetryAt = now +
			kFirstPersonRetryMilliseconds;
		g_FirstPersonRuntime = state;
		if (IsLiveUObject(pawn))
		{
			ApplyFirstPersonPawnVisibility(
				g_FirstPersonRuntime, playerController, pawn);
		}

		AtlasDiagnostics::WriteLine(
			"first-person applied route=body-mesh pc=%p pawn=%p hide=%u",
			playerController, pawn,
			static_cast<unsigned>(
				g_FirstPersonRuntime.HideMethod));
		return true;
	}

	static void MaintainFirstPersonRuntimeState(
		UWorld* world,
		AFortPlayerControllerAthena* playerController,
		bool frontendActive,
		ULONGLONG now)
	{
		const DWORD gameThreadId =
			g_ClientGameThreadId.load(std::memory_order_acquire);
		const bool onClientGameThread = gameThreadId != 0 &&
			gameThreadId == GetCurrentThreadId();
		AFortPlayerPawnAthena* pawn = onClientGameThread
			? ResolveFirstPersonPawn(playerController)
			: nullptr;
		const bool desired = FConfiguration::bFirstPerson.load(
			std::memory_order_acquire);
		const bool eligible = desired && onClientGameThread &&
			!frontendActive && IsLiveUObject(world) &&
			IsLiveUObject(playerController);

		static int observedDesired = -1;
		static bool observedEligible = false;
		static bool observedPawnAvailable = false;
		static UWorld* observedWorld = nullptr;
		static AFortPlayerControllerAthena* observedController = nullptr;
		const bool pawnAvailable = IsLiveUObject(pawn);
		if (observedDesired != (desired ? 1 : 0) ||
			observedEligible != eligible ||
			observedPawnAvailable != pawnAvailable ||
			observedWorld != world ||
			observedController != playerController)
		{
			AtlasDiagnostics::WriteLine(
				"first-person state desired=%d eligible=%d thread=%d frontend=%d world=%p pc=%p pawn=%p",
				desired ? 1 : 0, eligible ? 1 : 0,
				onClientGameThread ? 1 : 0,
				frontendActive ? 1 : 0,
				world, playerController, pawn);
			observedDesired = desired ? 1 : 0;
			observedEligible = eligible;
			observedPawnAvailable = pawnAvailable;
			observedWorld = world;
			observedController = playerController;
		}

		auto& state = g_FirstPersonRuntime;
		if (state.World)
		{
			const bool sameSession = eligible &&
				state.World == world &&
				state.Controller == playerController &&
				MatchesFirstPersonObjectIdentity(
					playerController,
					state.ControllerIdentity);
			if (!sameSession)
			{
				if (!RestoreFirstPersonRuntimeState(world))
				{
					return;
				}
			}
		}

		if (!eligible)
			return;

		if (!g_FirstPersonRuntime.World)
		{
			if (now < g_FirstPersonNextActivationAt)
				return;
			if (!ActivateFirstPersonRuntimeState(
					world, playerController, pawn, now))
			{
				g_FirstPersonNextActivationAt = now +
					kFirstPersonRetryMilliseconds;
			}
			else
			{
				g_FirstPersonNextActivationAt = 0;
			}
			return;
		}

		auto& active = g_FirstPersonRuntime;
		const bool trackedPawnAlive =
			MatchesFirstPersonObjectIdentity(
				active.Pawn, active.PawnIdentity);
		if (active.Pawn &&
			(!trackedPawnAlive ||
			 (pawnAvailable && active.Pawn != pawn)))
		{
			RestoreFirstPersonPawnVisibility(active);
		}

		if (!active.Pawn && pawnAvailable &&
			now >= active.NextRetryAt)
		{
			if (!ApplyFirstPersonPawnVisibility(
					active, playerController, pawn))
			{
				active.NextRetryAt = now +
					kFirstPersonRetryMilliseconds;
			}
			else
			{
				active.NextRetryAt = now +
					kFirstPersonRetryMilliseconds;
			}
		}
		if (active.HideMethod == EFirstPersonPawnHideMethod::
			PrimitiveOwnerNoSee)
		{
			for (size_t index = 0;
				index < active.PrimitiveVisibilityCount; ++index)
			{
				auto& componentState =
					active.PrimitiveVisibility[index];
				if (!MatchesFirstPersonObjectIdentity(
						componentState.Component,
						componentState.ComponentIdentity))
				{
					continue;
				}
				bool ownerNoSee = false;
				if (ReadFirstPersonBoolField(
						componentState.Component,
						componentState.OwnerNoSeeLayout,
						&ownerNoSee) &&
					ownerNoSee != componentState.DesiredOwnerNoSee)
				{
					InvokeFirstPersonBoolSetter(
						componentState.Component,
						componentState.SetOwnerNoSee,
						componentState.DesiredOwnerNoSee);
				}
			}

			// Cosmetic components can be created shortly after possession.
			// Discover them at a bounded cadence, preserving the Body/animation
			// source while hiding other pawn-owned components. Attached weapon
			// actors are never returned.
			if (now >= active.NextRetryAt &&
				MatchesFirstPersonObjectIdentity(
					active.Pawn, active.PawnIdentity))
			{
				ApplyFirstPersonPrimitiveVisibility(
					active, active.Pawn);
				active.NextRetryAt = now +
					kFirstPersonRetryMilliseconds;
			}
		}
		for (size_t index = 0;
			index < active.AnimationUpdateCount; ++index)
		{
			auto& animationState = active.AnimationUpdates[index];
			if (!MatchesFirstPersonObjectIdentity(
					animationState.Component,
					animationState.ComponentIdentity))
			{
				continue;
			}

			std::array<uint8,
				kMaximumFirstPersonReflectedValueBytes> observed{};
			if (ReadFirstPersonValueField(
					animationState.Component,
					animationState.VisibilityTickOption,
					&observed) &&
				!IsFirstPersonValueZero(
					animationState.VisibilityTickOption, observed))
			{
				std::array<uint8,
					kMaximumFirstPersonReflectedValueBytes>
					alwaysTickPoseAndRefreshBones{};
				WriteFirstPersonValueField(
					animationState.Component,
					animationState.VisibilityTickOption,
					alwaysTickPoseAndRefreshBones);
			}

			if (animationState.HasUpdateRateOptimization)
			{
				bool updateRateOptimization = false;
				if (ReadFirstPersonBoolField(
						animationState.Component,
						animationState.UpdateRateOptimization,
						&updateRateOptimization) &&
					updateRateOptimization)
				{
					WriteFirstPersonBoolField(
						animationState.Component,
						animationState.UpdateRateOptimization,
						false);
				}
			}
		}
	}
}

static bool ResolvePlaylistUiExtensionLayoutUnsafe()
{
	using namespace AtlasPlaylistHudPatchPolicy;
	if (g_PlaylistUiExtensionLayout.ElementSize != 0)
		return true;

	// FN 10.x exposes this as PlaylistUIExtension; later SDKs shortened the
	// reflected name to UIExtension. Resolve both exact names and keep all
	// size/field validation below version-neutral.
	const UStruct* extensionType = nullptr;
	for (const char* typeName : PlaylistUiExtensionTypeNames)
	{
		extensionType = SDK::FindStruct(typeName);
		if (extensionType)
			break;
	}
	if (!extensionType)
		return false;

	const int32 reflectedSize = extensionType->GetPropertiesSize();
	const uint32 slotOffset = extensionType->GetOffset("Slot");
	const uint32 widgetClassOffset =
		extensionType->GetOffset("WidgetClass");
	if (reflectedSize <= 0)
		return false;

	const FUiExtensionLayout candidate{
		static_cast<std::size_t>(reflectedSize),
		static_cast<std::size_t>(slotOffset),
		static_cast<std::size_t>(widgetClassOffset)};
	if (!IsSafeUiExtensionLayout(
			candidate, sizeof(FUIExtension),
			sizeof(FWeakObjectPtr)))
	{
		AtlasDiagnostics::WriteLine(
			"competitive-hud reflected-layout-rejected "
			"size=%d slot=%u widget-class=%u storage=%zu",
			reflectedSize, slotOffset, widgetClassOffset,
			sizeof(FUIExtension));
		return false;
	}

	g_PlaylistUiExtensionLayout = candidate;
	AtlasDiagnostics::WriteLine(
		"competitive-hud reflected-layout-ready "
		"size=%zu slot=%zu widget-class=%zu",
		candidate.ElementSize, candidate.SlotOffset,
		candidate.WidgetClassOffset);
	return true;
}

static bool ResolvePlaylistUiExtensionLayout()
{
	__try
	{
		return ResolvePlaylistUiExtensionLayoutUnsafe();
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return false;
	}
}

static int32 GetPlaylistUiExtensionSize()
{
	return g_PlaylistUiExtensionLayout.ElementSize > 0 &&
		g_PlaylistUiExtensionLayout.ElementSize <=
			static_cast<std::size_t>(INT32_MAX)
		? static_cast<int32>(
			g_PlaylistUiExtensionLayout.ElementSize)
		: 0;
}

static bool ReadPlaylistUiExtensionWeakObject(
	const FUIExtension& extension,
	FWeakObjectPtr& weakObject)
{
	if (!AtlasPlaylistHudPatchPolicy::IsSafeUiExtensionLayout(
			g_PlaylistUiExtensionLayout, sizeof(FUIExtension),
			sizeof(FWeakObjectPtr)))
	{
		return false;
	}

	const auto* bytes = reinterpret_cast<const uint8*>(&extension);
	memcpy(&weakObject,
		bytes + g_PlaylistUiExtensionLayout.WidgetClassOffset,
		sizeof(weakObject));
	return true;
}

static uint8 ReadPlaylistUiExtensionSlot(
	const FUIExtension& extension)
{
	const auto* bytes = reinterpret_cast<const uint8*>(&extension);
	return bytes[g_PlaylistUiExtensionLayout.SlotOffset];
}

static void WritePlaylistUiExtensionSlot(
	FUIExtension& extension,
	uint8 slot)
{
	auto* bytes = reinterpret_cast<uint8*>(&extension);
	bytes[g_PlaylistUiExtensionLayout.SlotOffset] = slot;
}

static bool BuildResolvedPlaylistUiExtension(
	FUIExtension& extension,
	uint8 slot,
	const UClass* widgetClass,
	const wchar_t* packagePath,
	const wchar_t* assetName,
	const wchar_t* legacyObjectPath)
{
	if (!widgetClass || !packagePath || !assetName ||
		!legacyObjectPath || !ResolvePlaylistUiExtensionLayout())
		return false;

	memset(&extension, 0, sizeof(extension));
	WritePlaylistUiExtensionSlot(extension, slot);
	const FWeakObjectPtr widgetIdentity(widgetClass);
	auto* bytes = reinterpret_cast<uint8*>(&extension);
	auto* widgetBytes = bytes +
		g_PlaylistUiExtensionLayout.WidgetClassOffset;
	memcpy(widgetBytes,
		&widgetIdentity, sizeof(widgetIdentity));

	const std::size_t widgetStorageSize =
		g_PlaylistUiExtensionLayout.ElementSize -
		g_PlaylistUiExtensionLayout.WidgetClassOffset;
	if (VersionInfo.FortniteVersion >= 23)
	{
		// UE5 stores FTopLevelAssetPath as compact package/asset name ids.
		// Write only the runtime four-byte ids: copying the SDK's eight-byte
		// compatibility FName here would overlap the adjacent field.
		const auto pathLayout = AtlasPlaylistHudPatchPolicy::
			ResolveTopLevelSoftClassPathLayout(
				VersionInfo.EngineVersion >= 5.3);
		const std::size_t packageOffset =
			pathLayout.PackageNameOffset;
		const std::size_t assetOffset =
			pathLayout.AssetNameOffset;
		const std::size_t subPathOffset =
			pathLayout.SubPathOffset;
		if (packageOffset + sizeof(int32) > widgetStorageSize ||
			assetOffset + sizeof(int32) > widgetStorageSize ||
			subPathOffset + sizeof(FString) > widgetStorageSize)
		{
			return false;
		}

		const FName packageName(packagePath);
		const FName classAssetName(assetName);
		memcpy(widgetBytes + packageOffset,
			&packageName.ComparisonIndex, sizeof(int32));
		memcpy(widgetBytes + assetOffset,
			&classAssetName.ComparisonIndex, sizeof(int32));
	}
	else
	{
		const std::size_t objectIdOffset =
			offsetof(FSoftObjectPtr, ObjectID);
		if (objectIdOffset + sizeof(FName) > widgetStorageSize)
			return false;
		const FName fullObjectPath(legacyObjectPath);
		memcpy(widgetBytes + objectIdOffset,
			&fullObjectPath, sizeof(fullObjectPath));
	}

	FWeakObjectPtr roundTrip;
	return ReadPlaylistUiExtensionWeakObject(
			extension, roundTrip) &&
		roundTrip.ObjectIndex == widgetIdentity.ObjectIndex &&
		roundTrip.ObjectSerialNumber ==
			widgetIdentity.ObjectSerialNumber;
}

static bool AddResolvedPlaylistUiExtension(
	TArray<FUIExtension>& extensions,
	uint8 slot,
	const UClass* widgetClass,
	const wchar_t* packagePath,
	const wchar_t* assetName,
	const wchar_t* legacyObjectPath)
{
	if (extensions.Num() == 1 && extensions.GetData())
		return true;
	if (extensions.Num() != 0)
		return false;

	FUIExtension extension{};
	if (!BuildResolvedPlaylistUiExtension(
			extension, slot, widgetClass,
			packagePath, assetName, legacyObjectPath))
	{
		return false;
	}
	const int32 elementSize = GetPlaylistUiExtensionSize();
	if (elementSize <= 0)
		return false;
	extensions.Add(extension, elementSize);
	return true;
}

static void InitializePlaylistExtensions()
{
	if (VersionInfo.FortniteVersion < 10 ||
		!ResolvePlaylistUiExtensionLayout())
	{
		return;
	}

	auto PrimarySlot = uint8_t(
		EPlaylistUIExtensionSlot::StaticEnum()
			? EPlaylistUIExtensionSlot::GetPrimary()
			: EUIExtensionSlot::GetPrimary());

	if (g_ArenaExtensions.Num() == 0)
	{
		const UClass* arenaClass = LoadArenaScoringHudClass();
		if (arenaClass && AddResolvedPlaylistUiExtension(
				g_ArenaExtensions, PrimarySlot, arenaClass,
				L"/Game/UI/Competitive/Arena/ArenaScoringHUD",
				L"ArenaScoringHUD_C",
				L"/Game/UI/Competitive/Arena/"
				L"ArenaScoringHUD.ArenaScoringHUD_C"))
		{
			g_ArenaScoringHudRepair.ArenaScoringHudClass =
				arenaClass;
		}
	}
	if (g_ShowdownExtensions.Num() == 0)
	{
		AddResolvedPlaylistUiExtension(
			g_ShowdownExtensions, PrimarySlot,
			LoadShowdownScoringHudClass(),
			L"/Game/UI/Frontend/Showdown/ShowdownScoringHUD",
			L"ShowdownScoringHUD_C",
			L"/Game/UI/Frontend/Showdown/"
			L"ShowdownScoringHUD.ShowdownScoringHUD_C");
	}

	g_PlaylistExtensionsInitialized =
		g_ArenaExtensions.Num() == 1 &&
		g_ArenaExtensions.GetData();
}

static bool IsWritableMemoryRange(
	const void* address, std::size_t size)
{
	if (size == 0)
		return true;
	if (!address)
		return false;

	const uintptr_t begin = reinterpret_cast<uintptr_t>(address);
	if (begin > UINTPTR_MAX - size)
		return false;
	const uintptr_t end = begin + size;
	uintptr_t cursor = begin;

	while (cursor < end)
	{
		MEMORY_BASIC_INFORMATION region{};
		if (VirtualQuery(reinterpret_cast<const void*>(cursor),
				&region, sizeof(region)) != sizeof(region) ||
			region.State != MEM_COMMIT ||
			(region.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
		{
			return false;
		}

		const DWORD protection = region.Protect & 0xFF;
		if (protection != PAGE_READWRITE &&
			protection != PAGE_WRITECOPY &&
			protection != PAGE_EXECUTE_READWRITE &&
			protection != PAGE_EXECUTE_WRITECOPY)
		{
			return false;
		}

		const uintptr_t regionBegin =
			reinterpret_cast<uintptr_t>(region.BaseAddress);
		if (regionBegin > UINTPTR_MAX - region.RegionSize)
			return false;
		const uintptr_t regionEnd = regionBegin + region.RegionSize;
		if (regionEnd <= cursor)
			return false;
		cursor = (std::min)(regionEnd, end);
	}

	return true;
}

static bool IsPlaylistUiExtensionArrayShapeValid(
	const TArray<FUIExtension>& extensions,
	int32 elementSize)
{
	constexpr int32 MaximumReasonableExtensions = 128;
	if (elementSize <= 0 ||
		extensions.Num() < 0 ||
		extensions.Max() < extensions.Num() ||
		extensions.Max() > MaximumReasonableExtensions)
	{
		return false;
	}

	if (extensions.Max() == 0)
		return extensions.GetData() == nullptr;
	if (!extensions.GetData())
		return false;

	const std::size_t allocatedBytes =
		static_cast<std::size_t>(extensions.Max()) *
		static_cast<std::size_t>(elementSize);
	return IsWritableMemoryRange(
		extensions.GetData(), allocatedBytes);
}

enum class ECompetitiveWidgetClassification : uint8
{
	Match,
	NonMatch,
	Unknown
};

static const UClass* ResolvePlaylistUiExtensionWidgetClassUnsafe(
	const FUIExtension& extension)
{
	FWeakObjectPtr weakClass;
	if (!ReadPlaylistUiExtensionWeakObject(extension, weakClass))
		return nullptr;

	// Classification runs while playlist assets and HUD owners are being
	// refreshed. Only inspect an already-resolved weak class here; synchronously
	// loading every unrelated UI extension can hitch or construct side effects.
	const UObject* resolved = weakClass.Get();
	if (!IsLiveUObject(resolved) ||
		!resolved->IsA(UClass::StaticClass()))
	{
		return nullptr;
	}
	return static_cast<const UClass*>(resolved);
}

static const UClass* ResolvePlaylistUiExtensionWidgetClass(
	const FUIExtension& extension)
{
	__try
	{
		return ResolvePlaylistUiExtensionWidgetClassUnsafe(extension);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return nullptr;
	}
}

static bool IsSameOrDerivedWidgetClass(
	const UClass* candidate,
	const UClass* expected)
{
	if (!candidate || !expected)
		return false;
	if (candidate == expected)
		return true;
	__try
	{
		const UObject* defaultObject = candidate->GetDefaultObj();
		return IsLiveUObject(defaultObject) &&
			defaultObject->IsA(expected);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return false;
	}
}

static ECompetitiveWidgetClassification ClassifyCompetitiveWidget(
	const FUIExtension& extension,
	const FUIExtension& expected)
{
	FWeakObjectPtr extensionIdentity;
	FWeakObjectPtr expectedIdentity;
	const bool weakIdentityMatches =
		ReadPlaylistUiExtensionWeakObject(
			extension, extensionIdentity) &&
		ReadPlaylistUiExtensionWeakObject(
			expected, expectedIdentity) &&
		extensionIdentity.ObjectIndex >= 0 &&
		extensionIdentity.ObjectIndex ==
			expectedIdentity.ObjectIndex &&
		extensionIdentity.ObjectSerialNumber != 0 &&
			extensionIdentity.ObjectSerialNumber ==
			expectedIdentity.ObjectSerialNumber;
	if (weakIdentityMatches)
		return ECompetitiveWidgetClassification::Match;

	// Authored UE4 playlist entries commonly retain only their soft object
	// path until the widget class is first loaded. Comparing just WeakPtr would
	// append a second ArenaScoringHUD entry in that state. Match the validated
	// reflected path representation as a stable fallback across UE4 and UE5.
	auto readPathIdentity = [](const FUIExtension& candidate,
		AtlasPlaylistHudPatchPolicy::FSoftClassPathIdentity* identity)
	{
		using namespace AtlasPlaylistHudPatchPolicy;
		if (identity)
			*identity = {};
		if (!identity || !IsSafeUiExtensionLayout(
				g_PlaylistUiExtensionLayout, sizeof(FUIExtension),
				sizeof(FWeakObjectPtr)))
		{
			return false;
		}

		const std::size_t widgetStorageSize =
			g_PlaylistUiExtensionLayout.ElementSize -
			g_PlaylistUiExtensionLayout.WidgetClassOffset;
		const auto* widgetBytes =
			reinterpret_cast<const uint8*>(&candidate) +
			g_PlaylistUiExtensionLayout.WidgetClassOffset;
		if (VersionInfo.FortniteVersion >= 23.00)
		{
			const auto pathLayout = ResolveTopLevelSoftClassPathLayout(
				VersionInfo.EngineVersion >= 5.3);
			if (pathLayout.PackageNameOffset + sizeof(int32) >
					widgetStorageSize ||
				pathLayout.AssetNameOffset + sizeof(int32) >
					widgetStorageSize)
			{
				return false;
			}
			int32 packageNameId = 0;
			int32 assetNameId = 0;
			memcpy(&packageNameId,
				widgetBytes + pathLayout.PackageNameOffset,
				sizeof(packageNameId));
			memcpy(&assetNameId,
				widgetBytes + pathLayout.AssetNameOffset,
				sizeof(assetNameId));
			*identity = MakeTopLevelSoftClassPathIdentity(
				packageNameId, assetNameId);
		}
		else
		{
			const std::size_t objectIdOffset =
				offsetof(FSoftObjectPtr, ObjectID);
			if (objectIdOffset + sizeof(FName) > widgetStorageSize)
				return false;
			FName pathName(0, 0);
			pathName.Number = 0;
			memcpy(&pathName, widgetBytes + objectIdOffset,
				sizeof(pathName));
			*identity = MakeLegacySoftClassPathIdentity(
				pathName.ComparisonIndex, pathName.Number);
		}
		return IsValidSoftClassPathIdentity(*identity);
	};

	AtlasPlaylistHudPatchPolicy::FSoftClassPathIdentity extensionPath{};
	AtlasPlaylistHudPatchPolicy::FSoftClassPathIdentity expectedPath{};
	const bool extensionPathValid =
		readPathIdentity(extension, &extensionPath);
	const bool expectedPathValid =
		readPathIdentity(expected, &expectedPath);
	if (extensionPathValid && expectedPathValid &&
		AtlasPlaylistHudPatchPolicy::IsSameSoftClassPathIdentity(
			extensionPath, expectedPath))
	{
		return ECompetitiveWidgetClassification::Match;
	}

	const UClass* expectedClass =
		ResolvePlaylistUiExtensionWidgetClass(expected);
	const UClass* extensionClass =
		ResolvePlaylistUiExtensionWidgetClass(extension);
	if (expectedClass && extensionClass)
	{
		return IsSameOrDerivedWidgetClass(
				extensionClass, expectedClass)
			? ECompetitiveWidgetClassification::Match
			: ECompetitiveWidgetClassification::NonMatch;
	}

	// A different unloaded soft path can still be an authored derived/alias
	// class. Without a loaded class hierarchy, absence is not proven; fail
	// closed so legacy playlists never receive a second scoring widget.
	return ECompetitiveWidgetClassification::Unknown;
}

static const UClass* GetLiveArenaScoringHudClass()
{
	const UClass* arenaClass =
		g_ArenaScoringHudRepair.ArenaScoringHudClass;
	if (!IsLiveUObject(arenaClass))
	{
		arenaClass = LoadArenaScoringHudClass();
		g_ArenaScoringHudRepair.ArenaScoringHudClass = arenaClass;
	}
	return IsLiveUObject(arenaClass) ? arenaClass : nullptr;
}

static bool IsVerifiedVisibleArenaScoringAlertUnsafe(
	UObject* alert)
{
	const UClass* arenaClass = GetLiveArenaScoringHudClass();
	const UClass* scoringAlertClass = FindClass("FortHUDScoringAlert");
	const bool exactObservedNative =
		IsExactObservedNativeArenaScoringAlert(alert);
	const bool isArenaAlert = IsLiveUObject(arenaClass) &&
		IsLiveUObject(alert) && alert->IsA(arenaClass);
	const bool isObservedScoringAlert = exactObservedNative &&
		IsLiveUObject(scoringAlertClass) && alert->IsA(scoringAlertClass);
	if (!IsLiveUObject(alert) ||
		(!isArenaAlert && !isObservedScoringAlert) ||
		IsRejectedArenaScoringAlert(alert))
	{
		return false;
	}

	bool viewportQuerySupported = false;
	const bool inViewport = IsWidgetInViewport(
		alert, &viewportQuerySupported);
	if (viewportQuerySupported && inViewport)
		return true;
	if (IsWidgetVisibleThroughHudHierarchy(alert))
		return true;
	// The exact session-scoped callback identity is itself positive evidence
	// that a nested authored widget is consuming Arena presentations. Several
	// legacy UI-extension hosts expose neither a useful parent nor IsRendered.
	return isObservedScoringAlert;
}

static bool IsVerifiedVisibleArenaScoringAlert(
	UObject* alert)
{
	__try
	{
		return IsVerifiedVisibleArenaScoringAlertUnsafe(alert);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return false;
	}
}

static bool HasVerifiedHudOwnerArenaSource()
{
	return g_ArenaScoringHudRepair.UsesHudOwnerSource &&
		IsVerifiedVisibleArenaScoringAlert(
			g_ArenaScoringHudRepair.ScoringAlert);
}

static bool NormalizeCompetitiveUiExtensionArrayUnsafe(
	TArray<FUIExtension>& extensions,
	const FUIExtension& expected,
	bool arena,
	const char* source,
	const void* owner)
{
	const int32 elementSize = GetPlaylistUiExtensionSize();
	if (!IsPlaylistUiExtensionArrayShapeValid(
			extensions, elementSize))
	{
		static std::atomic_uint32_t invalidLayouts = 0;
		if (invalidLayouts.fetch_add(
				1, std::memory_order_relaxed) < 16)
		{
			AtlasDiagnostics::WriteLine(
				"competitive-hud array-rejected source=%s "
				"owner=%p num=%d max=%d element-size=%d",
				source ? source : "unknown", owner,
				extensions.Num(), extensions.Max(), elementSize);
		}
		return false;
	}

	constexpr std::size_t MaximumReasonableExtensions = 128;
	std::array<ECompetitiveWidgetClassification,
		MaximumReasonableExtensions> classifications{};
	int32 firstExpectedIndex = -1;
	std::size_t expectedCount = 0;
	std::size_t unknownCount = 0;
	for (int32 index = 0; index < extensions.Num(); ++index)
	{
		const auto classification = ClassifyCompetitiveWidget(
			extensions.Get(index, elementSize), expected);
		classifications[static_cast<std::size_t>(index)] =
			classification;
		if (classification ==
			ECompetitiveWidgetClassification::Match)
		{
			if (firstExpectedIndex < 0)
				firstExpectedIndex = index;
			expectedCount++;
		}
		else if (classification ==
			ECompetitiveWidgetClassification::Unknown)
		{
			unknownCount++;
		}
	}

	const auto plan =
		AtlasPlaylistHudPatchPolicy::PlanExtensionNormalization(
			static_cast<std::size_t>(extensions.Num()),
			expectedCount, unknownCount);
	bool changed = false;
	for (int32 index = extensions.Num() - 1;
		 index >= 0 && plan.DuplicateCount > 0; --index)
	{
		if (index != firstExpectedIndex &&
			classifications[static_cast<std::size_t>(index)] ==
				ECompetitiveWidgetClassification::Match)
		{
			extensions.Remove(index, elementSize);
			changed = true;
		}
	}

	if (firstExpectedIndex >= 0)
	{
		auto& retained = extensions.Get(
			firstExpectedIndex, elementSize);
		const uint8 expectedSlot =
			ReadPlaylistUiExtensionSlot(expected);
		if (ReadPlaylistUiExtensionSlot(retained) != expectedSlot)
		{
			WritePlaylistUiExtensionSlot(
				retained, expectedSlot);
			changed = true;
		}
	}
	else if (plan.AppendExpected)
	{
		extensions.Add(expected, elementSize);
		changed = true;
	}

	if (changed)
	{
		AtlasDiagnostics::WriteLine(
			"competitive-hud extensions-normalized source=%s "
			"owner=%p arena=%d before=%zu matches=%zu unknown=%zu "
			"after=%d",
			source ? source : "unknown", owner,
			arena ? 1 : 0,
			plan.FinalCount - (plan.AppendExpected ? 1u : 0u) +
				plan.DuplicateCount,
			expectedCount, unknownCount, extensions.Num());
	}

	return changed;
}

static bool RemoveCompetitiveUiExtensionFromArrayUnsafe(
	TArray<FUIExtension>& extensions,
	const FUIExtension& expected,
	const char* source,
	const void* owner)
{
	const int32 elementSize = GetPlaylistUiExtensionSize();
	if (!IsPlaylistUiExtensionArrayShapeValid(
			extensions, elementSize))
	{
		return false;
	}

	const int32 before = extensions.Num();
	for (int32 index = extensions.Num() - 1;
		 index >= 0; --index)
	{
		if (ClassifyCompetitiveWidget(
				extensions.Get(index, elementSize), expected) ==
			ECompetitiveWidgetClassification::Match)
		{
			extensions.Remove(index, elementSize);
		}
	}

	if (extensions.Num() == before)
		return false;

	AtlasDiagnostics::WriteLine(
		"competitive-hud playlist-arena-source-removed "
		"source=%s owner=%p before=%d after=%d",
		source ? source : "unknown", owner,
		before, extensions.Num());
	return true;
}

static bool RemoveArenaScoringHudFromPlaylistUnsafe(
	UFortPlaylistAthena* playlist,
	const char* source)
{
	if (!playlist || !playlist->HasUIExtensions() ||
		!g_PlaylistExtensionsInitialized ||
		g_ArenaExtensions.Num() != 1 ||
		!g_ArenaExtensions.GetData())
	{
		return false;
	}

	return RemoveCompetitiveUiExtensionFromArrayUnsafe(
		playlist->UIExtensions,
		g_ArenaExtensions.Get(0),
		source,
		playlist);
}

static bool RemoveArenaScoringHudFromPlaylist(
	UFortPlaylistAthena* playlist,
	const char* source)
{
	__try
	{
		return RemoveArenaScoringHudFromPlaylistUnsafe(
			playlist, source);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		AtlasDiagnostics::WriteLine(
			"competitive-hud playlist-arena-removal-faulted "
			"source=%s playlist=%p",
			source ? source : "unknown", playlist);
		return false;
	}
}

static bool NormalizeCompetitivePlaylistUiExtensionsUnsafe(
	UFortPlaylistAthena* playlist,
	bool arena,
	const char* source)
{
	if (!playlist || !playlist->HasUIExtensions() ||
		!g_PlaylistExtensionsInitialized)
	{
		return false;
	}

	auto& expectedArray = arena
		? g_ArenaExtensions
		: g_ShowdownExtensions;
	if (expectedArray.Num() != 1 || !expectedArray.GetData())
		return false;

	return NormalizeCompetitiveUiExtensionArrayUnsafe(
		playlist->UIExtensions,
		expectedArray.Get(0),
		arena,
		source,
		playlist);
}

static bool NormalizeCompetitivePlaylistUiExtensions(
	UFortPlaylistAthena* playlist,
	bool arena,
	const char* source)
{
	__try
	{
		return NormalizeCompetitivePlaylistUiExtensionsUnsafe(
			playlist, arena, source);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		AtlasDiagnostics::WriteLine(
			"competitive-hud normalization-faulted source=%s "
			"playlist=%p",
			source ? source : "unknown", playlist);
		return false;
	}
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
		AtlasPlaylistHudPatchPolicy::ECompetitiveHud hud{};
		if (Playlist->HasUIExtensions() &&
			AtlasPlaylistHudPatchPolicy::
				TryResolveCompetitivePlaylistObjectName(Name, &hud))
		{
			const bool arena = hud == AtlasPlaylistHudPatchPolicy::
				ECompetitiveHud::Arena;
			if (arena && HasVerifiedHudOwnerArenaSource())
			{
				// ArenaTournamentUIExtensions is the single in-match source.
				// Keeping ArenaScoringHUD in both arrays creates duplicate
				// Primary-slot widgets on clients that aggregate them.
				RemoveArenaScoringHudFromPlaylist(
					Playlist, "loaded-object");
			}
			else
			{
				NormalizeCompetitivePlaylistUiExtensions(
					Playlist, arena, "loaded-object");
			}
		}
	}
}

static const UObject* FindLoadedCompetitivePlaylist(
	const wchar_t* ObjectPath)
{
	if (!ObjectPath || !g_ClientPlaylistClass ||
		!SDK::Offsets::StaticFindObject)
	{
		return nullptr;
	}

	__try
	{
		return StaticFindObject(
			ObjectPath, g_ClientPlaylistClass);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return nullptr;
	}
}

static bool IsReflectedOffsetInBounds(
	const UStruct* ownerType,
	uint32 offset,
	std::size_t valueSize)
{
	if (!ownerType || offset == UINT32_MAX)
		return false;

	const int32 propertiesSize = ownerType->GetPropertiesSize();
	return propertiesSize > 0 &&
		offset <= static_cast<uint32>(propertiesSize) &&
		valueSize <= static_cast<std::size_t>(propertiesSize) - offset;
}

static const UObject* ReadReflectedObjectPropertyUnsafe(
	const UObject* owner,
	const char* propertyName)
{
	if (!owner || !owner->Class || !propertyName)
		return nullptr;

	uint32 offset = owner->GetOffset(propertyName, 0x10000);
	if (!IsReflectedOffsetInBounds(
			owner->Class, offset, sizeof(UObject*)))
	{
		return nullptr;
	}

	return *reinterpret_cast<UObject* const*>(
		reinterpret_cast<const uint8*>(owner) + offset);
}

struct FReflectedStringLayout
{
	const wchar_t* Data;
	int32 NumElements;
	int32 MaxElements;
};

static bool TryCopyReflectedAsciiStringUnsafe(
	const void* address,
	UEAllocatedString* value)
{
	if (value)
		value->clear();
	if (!address || !value)
		return false;

	FReflectedStringLayout source{};
	memcpy(&source, address, sizeof(source));
	if (!source.Data || source.NumElements <= 1 ||
		source.NumElements > 256 ||
		source.MaxElements < source.NumElements ||
		source.Data[source.NumElements - 1] != L'\0')
	{
		return false;
	}

	value->reserve(static_cast<std::size_t>(source.NumElements - 1));
	for (int32 index = 0; index < source.NumElements - 1; ++index)
	{
		const wchar_t character = source.Data[index];
		if (character <= 0 || character > 0x7f)
		{
			value->clear();
			return false;
		}
		value->push_back(static_cast<char>(character));
	}
	return !value->empty();
}

static bool TryGetLastSelectedSelectionIdentityUnsafe(
	UEAllocatedString* selectionIdentity)
{
	if (selectionIdentity)
		selectionIdentity->clear();
	if (!selectionIdentity)
		return false;

	const UEngine* engine = UEngine::GetEngine();
	const UObject* settings = ReadReflectedObjectPropertyUnsafe(
		engine, "GameUserSettings");
	const UStruct* playlistDataType =
		SDK::FindStruct("PlaylistData");
	if (!settings || !settings->Class || !playlistDataType)
		return false;

	uint32 selectedOffset = settings->GetOffset(
		"LastSelectedPlaylist", 0x100000);
	if (selectedOffset == UINT32_MAX)
		selectedOffset = settings->GetOffset(
			"LastSelectedPlaylist");
	const int32 playlistDataSize =
		playlistDataType->GetPropertiesSize();
	if (playlistDataSize <= 0 ||
		!IsReflectedOffsetInBounds(
			settings->Class, selectedOffset,
			static_cast<std::size_t>(playlistDataSize)))
	{
		return false;
	}

	uint32 nameOffset = playlistDataType->GetOffset(
		"PlaylistName", 0x2000);
	if (nameOffset == UINT32_MAX)
		nameOffset = playlistDataType->GetOffset("PlaylistName");
	if (!IsReflectedOffsetInBounds(
			playlistDataType, nameOffset, sizeof(FName)))
	{
		return false;
	}

	const auto* selectedData =
		reinterpret_cast<const uint8*>(settings) + selectedOffset;

	// Newer FPlaylistData revisions carry the selected Discovery child in
	// LinkId.Mnemonic even while PlaylistName remains Playlist_DefaultSolo.
	// Resolve the nested structs entirely by reflected names and bounds; never
	// assume a version-specific offset or reinterpret an older RegionId field.
	const UField* linkProperty = playlistDataType->GetProperty(
		"LinkId", 0x100000);
	if (linkProperty)
	{
		const UStruct* linkIdType = SDK::FindStruct("OnlineLinkId");
		if (!linkIdType)
			linkIdType = SDK::FindStruct("FOnlineLinkId");
		if (!linkIdType)
			return false;

		const uint32 linkOffset = GetFromOffset<uint32>(
			linkProperty, Offsets::Offset_Internal);
		const int32 linkSize = linkIdType->GetPropertiesSize();
		if (linkSize <= 0 || !IsReflectedOffsetInBounds(
				playlistDataType, linkOffset,
				static_cast<std::size_t>(linkSize)))
		{
			return false;
		}

		const UField* mnemonicProperty = linkIdType->GetProperty(
			"Mnemonic", 0x4000);
		if (!mnemonicProperty)
			return false;
		const uint32 mnemonicOffset = GetFromOffset<uint32>(
			mnemonicProperty, Offsets::Offset_Internal);
		if (!IsReflectedOffsetInBounds(
				linkIdType, mnemonicOffset,
				sizeof(FReflectedStringLayout)))
		{
			return false;
		}

		UEAllocatedString linkMnemonic;
		if (!TryCopyReflectedAsciiStringUnsafe(
				selectedData + linkOffset + mnemonicOffset,
				&linkMnemonic))
		{
			return false;
		}

		const std::string_view resolved =
			AtlasTournamentRefreshPolicy::ResolveSelectionIdentity(
				true, linkMnemonic, {});
		if (resolved.empty())
			return false;
		*selectionIdentity = linkMnemonic;
		return true;
	}

	// Legacy PlaylistData has no LinkId field. Its PlaylistName is the actual
	// selected mode and remains the safe compatibility signal.
	const auto* name = reinterpret_cast<const FName*>(
		selectedData + nameOffset);
	const UEAllocatedString playlistName = name->ToString();
	const std::string_view resolved =
		AtlasTournamentRefreshPolicy::ResolveSelectionIdentity(
			false, {}, playlistName);
	if (resolved.empty())
		return false;
	*selectionIdentity = playlistName;
	return true;
}

static bool TryGetLastSelectedSelectionIdentity(
	UEAllocatedString* selectionIdentity)
{
	__try
	{
		return TryGetLastSelectedSelectionIdentityUnsafe(
			selectionIdentity);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		if (selectionIdentity)
			selectionIdentity->clear();
		return false;
	}
}

static std::array<const UObject*, 3>
ResolveActivePlaylistCandidatesUnsafe(const UObject* gameState)
{
	std::array<const UObject*, 3> candidates{};
	if (!gameState || !gameState->Class)
		return candidates;

	std::size_t candidateCount = 0;
	auto addUnique = [&](const UObject* candidate)
	{
		if (!candidate || candidateCount >= candidates.size())
			return;
		for (std::size_t index = 0;
			 index < candidateCount; ++index)
		{
			if (candidates[index] == candidate)
				return;
		}
		candidates[candidateCount++] = candidate;
	};

	const UStruct* playlistInfoType =
		SDK::FindStruct("PlaylistPropertyArray");
	const UObject* overridePlaylist = nullptr;
	const UObject* basePlaylist = nullptr;
	uint32 playlistInfoOffset = gameState->GetOffset(
		"CurrentPlaylistInfo", 0x100000);
	if (playlistInfoOffset == UINT32_MAX)
	{
		// Old reflection builds do not consistently expose StructProperty cast
		// flags. Name lookup is still bounded against both reflected sizes.
		playlistInfoOffset = gameState->GetOffset(
			"CurrentPlaylistInfo");
	}

	if (playlistInfoType &&
		IsReflectedOffsetInBounds(
			gameState->Class, playlistInfoOffset,
			static_cast<std::size_t>(
				playlistInfoType->GetPropertiesSize())))
	{
		const auto* playlistInfo =
			reinterpret_cast<const uint8*>(gameState) +
			playlistInfoOffset;
		const uint32 overrideOffset =
			playlistInfoType->GetOffset(
				"OverridePlaylist", 0x10000);
		if (IsReflectedOffsetInBounds(
				playlistInfoType, overrideOffset,
				sizeof(UObject*)))
		{
			overridePlaylist = *reinterpret_cast<UObject* const*>(
				playlistInfo + overrideOffset);
		}

		const uint32 baseOffset = playlistInfoType->GetOffset(
			"BasePlaylist", 0x10000);
		if (IsReflectedOffsetInBounds(
				playlistInfoType, baseOffset,
				sizeof(UObject*)))
		{
			basePlaylist = *reinterpret_cast<UObject* const*>(
				playlistInfo + baseOffset);
		}
	}

	// Before UE 4.24 the replicated BasePlaylist is the published value.
	// Later builds publish OverridePlaylist first. Mirror the gameserver's
	// authority order so a stale frontend value cannot win on either branch.
	if (VersionInfo.EngineVersion < 4.24)
	{
		addUnique(basePlaylist);
		addUnique(overridePlaylist);
	}
	else
	{
		addUnique(overridePlaylist);
		addUnique(basePlaylist);
	}

	addUnique(ReadReflectedObjectPropertyUnsafe(
		gameState, "CurrentPlaylistData"));
	return candidates;
}

static const UObject* ResolveAuthoritativeActivePlaylistUnsafe(
	const UObject* gameState)
{
	// CurrentPlaylistInfo is ordered by the engine-version-specific publication
	// contract above, and legacy CurrentPlaylistData is used only after it.
	// Treating these as an unordered OR lets a stale frontend/base Arena value
	// activate Arena HUD repair inside a non-Arena server match.
	const auto candidates =
		ResolveActivePlaylistCandidatesUnsafe(gameState);
	for (const UObject* candidate : candidates)
	{
		if (IsLiveUObject(candidate) &&
			candidate->IsA(g_ClientPlaylistClass))
		{
			return candidate;
		}
	}
	return nullptr;
}

static bool NormalizeActiveCompetitivePlaylistUnsafe(
	const UObject* gameState,
	const char* source)
{
	if (!IsLiveUObject(gameState) || !g_ClientPlaylistClass)
		return false;

	const UObject* candidate =
		ResolveAuthoritativeActivePlaylistUnsafe(gameState);
	if (!candidate)
		return false;

	const auto name = candidate->Name.ToString();
	AtlasPlaylistHudPatchPolicy::ECompetitiveHud hud{};
	if (!AtlasPlaylistHudPatchPolicy::
			TryResolveCompetitivePlaylistObjectName(name, &hud))
	{
		return false;
	}

	auto* playlist = static_cast<UFortPlaylistAthena*>(
		const_cast<UObject*>(candidate));
	const bool arena = hud ==
		AtlasPlaylistHudPatchPolicy::ECompetitiveHud::Arena;
	if (arena && HasVerifiedHudOwnerArenaSource())
	{
		return RemoveArenaScoringHudFromPlaylist(
			playlist, source);
	}
	return NormalizeCompetitivePlaylistUiExtensions(
		playlist, arena, source);
}

static bool NormalizeActiveCompetitivePlaylist(
	const UObject* gameState,
	const char* source)
{
	__try
	{
		return NormalizeActiveCompetitivePlaylistUnsafe(
			gameState, source);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		AtlasDiagnostics::WriteLine(
			"competitive-hud active-playlist-faulted "
			"source=%s game-state=%p",
			source ? source : "unknown", gameState);
		return false;
	}
}

static bool IsActiveArenaPlaylistUnsafe(const UObject* gameState)
{
	if (!IsLiveUObject(gameState) || !g_ClientPlaylistClass)
		return false;

	const UObject* candidate =
		ResolveAuthoritativeActivePlaylistUnsafe(gameState);
	return candidate &&
		AtlasPlaylistHudPatchPolicy::IsArenaPlaylistObjectName(
			candidate->Name.ToString());
}

static bool IsActiveArenaPlaylist(const UObject* gameState)
{
	__try
	{
		return IsActiveArenaPlaylistUnsafe(gameState);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return false;
	}
}

static UObject* GetWidgetOwningPlayerUnsafe(
	UObject* widget,
	bool* supported)
{
	if (supported)
		*supported = false;
	if (!IsLiveUObject(widget))
		return nullptr;

	UFunction* function = widget->GetFunction("GetOwningPlayer");
	if (!function)
		return nullptr;
	const auto schema = function->GetParamsNamed();
	if (schema.Size != sizeof(UObject*) ||
		schema.NameOffsetMap.size() != 1)
	{
		return nullptr;
	}
	const auto& result = schema.NameOffsetMap[0];
	if (result.Name != "ReturnValue" || result.Offset != 0 ||
		result.ElementSize != sizeof(UObject*) ||
		(result.PropertyFlags &
			AtlasArenaVisualTracePolicy::CpfReturnParm) == 0)
	{
		return nullptr;
	}

	if (supported)
		*supported = true;
	UObject* owningPlayer = nullptr;
	widget->ProcessEvent(function, &owningPlayer);
	return owningPlayer;
}

static UObject* GetWidgetOwningPlayer(
	UObject* widget,
	bool* supported)
{
	__try
	{
		return GetWidgetOwningPlayerUnsafe(widget, supported);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		if (supported)
			*supported = false;
		return nullptr;
	}
}

static UObject* GetWidgetOwningLocalPlayerUnsafe(
	UObject* widget,
	bool* supported)
{
	if (supported)
		*supported = false;
	if (!IsLiveUObject(widget))
		return nullptr;

	UFunction* function = widget->GetFunction("GetOwningLocalPlayer");
	if (!function)
		return nullptr;
	const auto schema = function->GetParamsNamed();
	if (schema.Size != sizeof(UObject*) ||
		schema.NameOffsetMap.size() != 1)
	{
		return nullptr;
	}
	const auto& result = schema.NameOffsetMap[0];
	if (result.Name != "ReturnValue" || result.Offset != 0 ||
		result.ElementSize != sizeof(UObject*) ||
		(result.PropertyFlags &
			AtlasArenaVisualTracePolicy::CpfReturnParm) == 0)
	{
		return nullptr;
	}
	if (supported)
		*supported = true;
	UObject* owningLocalPlayer = nullptr;
	widget->ProcessEvent(function, &owningLocalPlayer);
	return owningLocalPlayer;
}

static UObject* GetWidgetOwningLocalPlayer(
	UObject* widget,
	bool* supported)
{
	__try
	{
		return GetWidgetOwningLocalPlayerUnsafe(widget, supported);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		if (supported)
			*supported = false;
		return nullptr;
	}
}

static bool BelongsToLocalPlayer(
	UObject* object,
	AFortPlayerControllerAthena* playerController,
	bool requireOwningPlayer = false)
{
	if (!playerController)
		return false;
	bool owningPlayerSupported = false;
	UObject* owningPlayer = GetWidgetOwningPlayer(
		object, &owningPlayerSupported);
	if (owningPlayerSupported)
		return owningPlayer == playerController;
	return !requireOwningPlayer;
}

static bool IsWidgetInViewportUnsafe(
	UObject* widget,
	bool* supported)
{
	if (supported)
		*supported = false;
	if (!IsLiveUObject(widget))
		return false;

	UFunction* function = widget->GetFunction("IsInViewport");
	if (!function)
		return false;
	const auto schema = function->GetParamsNamed();
	if (schema.Size == 0 || schema.Size > 8 ||
		schema.NameOffsetMap.size() != 1)
	{
		return false;
	}
	const auto& returnValue = schema.NameOffsetMap[0];
	if (returnValue.Name != "ReturnValue" ||
		returnValue.ElementSize == 0 ||
		returnValue.Offset > schema.Size ||
		returnValue.ElementSize >
			schema.Size - returnValue.Offset ||
		(returnValue.PropertyFlags &
			AtlasArenaVisualTracePolicy::CpfReturnParm) == 0)
	{
		return false;
	}

	if (supported)
		*supported = true;
	std::array<uint8, 8> parameters{};
	widget->ProcessEvent(function, parameters.data());
	for (uint32 byte = 0;
		 byte < returnValue.ElementSize; ++byte)
	{
		if (parameters[returnValue.Offset + byte] != 0)
			return true;
	}
	return false;
}

static bool IsWidgetInViewport(
	UObject* widget,
	bool* supported)
{
	__try
	{
		return IsWidgetInViewportUnsafe(widget, supported);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		if (supported)
			*supported = false;
		return false;
	}
}

static bool ReadZeroParameterWidgetBoolUnsafe(
	UObject* widget,
	const char* functionName,
	bool* supported)
{
	if (supported)
		*supported = false;
	if (!IsLiveUObject(widget) || !functionName)
		return false;
	UFunction* function = widget->GetFunction(functionName);
	if (!function)
		return false;
	const auto schema = function->GetParamsNamed();
	if (schema.Size == 0 || schema.Size > 8 ||
		schema.NameOffsetMap.size() != 1)
	{
		return false;
	}
	const auto& returnValue = schema.NameOffsetMap[0];
	if (returnValue.Name != "ReturnValue" ||
		returnValue.ElementSize == 0 ||
		returnValue.Offset > schema.Size ||
		returnValue.ElementSize > schema.Size - returnValue.Offset ||
		(returnValue.PropertyFlags &
			AtlasArenaVisualTracePolicy::CpfReturnParm) == 0)
	{
		return false;
	}
	if (supported)
		*supported = true;
	std::array<uint8, 8> parameters{};
	widget->ProcessEvent(function, parameters.data());
	for (uint32 byte = 0; byte < returnValue.ElementSize; ++byte)
	{
		if (parameters[returnValue.Offset + byte] != 0)
			return true;
	}
	return false;
}

static bool ReadZeroParameterWidgetBool(
	UObject* widget,
	const char* functionName,
	bool* supported)
{
	__try
	{
		return ReadZeroParameterWidgetBoolUnsafe(
			widget, functionName, supported);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		if (supported)
			*supported = false;
		return false;
	}
}

static UObject* GetWidgetParentUnsafe(
	UObject* widget,
	bool* supported)
{
	if (supported)
		*supported = false;
	if (!IsLiveUObject(widget))
		return nullptr;
	UFunction* function = widget->GetFunction("GetParent");
	if (!function)
		return nullptr;
	const auto schema = function->GetParamsNamed();
	if (schema.Size != sizeof(UObject*) ||
		schema.NameOffsetMap.size() != 1)
	{
		return nullptr;
	}
	const auto& result = schema.NameOffsetMap[0];
	if (result.Name != "ReturnValue" || result.Offset != 0 ||
		result.ElementSize != sizeof(UObject*) ||
		(result.PropertyFlags &
			AtlasArenaVisualTracePolicy::CpfReturnParm) == 0)
	{
		return nullptr;
	}
	if (supported)
		*supported = true;
	UObject* parent = nullptr;
	widget->ProcessEvent(function, &parent);
	return parent;
}

static UObject* GetWidgetParent(UObject* widget, bool* supported)
{
	__try
	{
		return GetWidgetParentUnsafe(widget, supported);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		if (supported)
			*supported = false;
		return nullptr;
	}
}

static bool IsWidgetVisibleThroughHudHierarchy(UObject* widget)
{
	UObject* current = widget;
	for (unsigned depth = 0;
		 depth < 16 && IsLiveUObject(current); ++depth)
	{
		bool renderedSupported = false;
		if (ReadZeroParameterWidgetBool(
				current, "IsRendered", &renderedSupported))
		{
			return true;
		}

		bool visibleSupported = false;
		const bool visible = ReadZeroParameterWidgetBool(
			current, "IsVisible", &visibleSupported);
		if (visibleSupported && !visible)
			return false;

		bool viewportQuerySupported = false;
		if (IsWidgetInViewport(current, &viewportQuerySupported) &&
			viewportQuerySupported)
		{
			return true;
		}

		bool parentSupported = false;
		UObject* parent = GetWidgetParent(current, &parentSupported);
		if (!parentSupported || !IsLiveUObject(parent) ||
			parent == current)
		{
			return false;
		}
		current = parent;
	}
	return false;
}

static UObject* FindLiveFrontendMatchmakingWidgetUnsafe(
	AFortPlayerControllerAthena* playerController)
{
	const UClass* widgetClass =
		FindClass("FortAthenaMatchmakingWidgetLegacy");
	if (!widgetClass || !playerController)
		return nullptr;

	// Frontend widgets are allocated near the end of GUObjectArray. This scan
	// happens only on a bounded selection-refresh attempt, never every frame.
	constexpr int maximumObjectsToInspect = 65536;
	const int objectCount = TUObjectArray::Num();
	const int firstIndex = (std::max)(
		0, objectCount - maximumObjectsToInspect);
	for (int index = objectCount - 1; index >= firstIndex; --index)
	{
		const FUObjectItem* item = TUObjectArray::GetItemByIndex(index);
		UObject* object = item ? item->Object : nullptr;
		if (!object || (item->Flags & 0x20) != 0 ||
			object->IsDefaultObject() || !object->IsA(widgetClass) ||
			!BelongsToLocalPlayer(object, playerController, true))
		{
			continue;
		}

		UFunction* function = object->GetFunction("OnIslandChanged");
		if (MatchesArenaVisualTraceSchema(
				function, EArenaVisualTraceSchema::ZeroParameters))
		{
			return object;
		}
	}
	return nullptr;
}

static bool TryRebindFrontendSelectedActivity(
	AFortPlayerControllerAthena* playerController)
{
	const DWORD gameThreadId = g_ClientGameThreadId.load(
		std::memory_order_acquire);
	if (!playerController || gameThreadId == 0 ||
		GetCurrentThreadId() != gameThreadId ||
		g_FrontendSelectionRebindDepth != 0 ||
		g_FrontendTournamentRefreshDispatchActive ||
		!IsFrontendWorld(UWorld::GetWorld()))
	{
		return false;
	}

	__try
	{
		UObject* widget = FindLiveFrontendMatchmakingWidgetUnsafe(
			playerController);
		if (!IsLiveUObject(widget))
			return false;

		UFunction* function = widget->GetFunction("OnIslandChanged");
		if (!MatchesArenaVisualTraceSchema(
				function, EArenaVisualTraceSchema::ZeroParameters))
		{
			return false;
		}

		++g_FrontendSelectionRebindDepth;
		g_FrontendTournamentRefreshDispatchActive = true;
		widget->ProcessEvent(function, nullptr);
		g_FrontendTournamentRefreshDispatchActive = false;
		--g_FrontendSelectionRebindDepth;
		AtlasDiagnostics::WriteLine(
			"tournament-refresh selection-rebind-dispatched "
			"widget=%p function=%p",
			widget, function);
		return true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		g_FrontendTournamentRefreshDispatchActive = false;
		g_FrontendSelectionRebindDepth = 0;
		return false;
	}
}

static bool HasExactlyOneLocalPlayer()
{
	__try
	{
		UWorld* world = UWorld::GetWorld();
		return world && world->OwningGameInstance &&
			world->OwningGameInstance->LocalPlayers.Num() == 1;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return false;
	}
}

static UObject* GetOnlyLocalPlayer()
{
	__try
	{
		UWorld* world = UWorld::GetWorld();
		if (!world || !world->OwningGameInstance ||
			world->OwningGameInstance->LocalPlayers.Num() != 1)
		{
			return nullptr;
		}
		return world->OwningGameInstance->LocalPlayers[0];
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return nullptr;
	}
}

static bool IsLegacyOwnerMatchedNativeArenaScoringAlert(
	UObject* alert,
	AFortPlayerControllerAthena* playerController)
{
	if (!AtlasPlaylistHudPatchPolicy::
			UsesLegacyPlaylistOwnedArenaHud(
				VersionInfo.FortniteVersion) ||
		!playerController || !IsLiveUObject(alert) ||
		IsExactOwnedArenaScoringAlert(alert))
	{
		return false;
	}

	const UClass* arenaClass = GetLiveArenaScoringHudClass();
	if (!IsLiveUObject(arenaClass) || alert->IsDefaultObject() ||
		!alert->IsA(arenaClass) || IsRejectedArenaScoringAlert(alert))
	{
		return false;
	}

	bool owningLocalPlayerSupported = false;
	UObject* owningLocalPlayer = GetWidgetOwningLocalPlayer(
		alert, &owningLocalPlayerSupported);
	bool owningPlayerSupported = false;
	UObject* owningPlayer = GetWidgetOwningPlayer(
		alert, &owningPlayerSupported);
	UObject* expectedLocalPlayer = GetOnlyLocalPlayer();
	const bool hasUsableLocalPlayerQuery =
		owningLocalPlayerSupported && owningLocalPlayer;
	const bool hasUsableControllerQuery =
		owningPlayerSupported && owningPlayer;
	const bool ownershipQuerySupported =
		hasUsableLocalPlayerQuery || hasUsableControllerQuery;
	const bool ownershipMatches = hasUsableLocalPlayerQuery
		? owningLocalPlayer == expectedLocalPlayer
		: (hasUsableControllerQuery &&
			owningPlayer == playerController);
	return AtlasArenaHudOwnershipPolicy::
		CanAdoptLegacyOwnerMatchedNativeAlert(
			true,
			true,
			true,
			ownershipQuerySupported,
			ownershipMatches);
}

static bool IsUsableArenaHudOwnerAlert(
	UObject* alert,
	AFortPlayerControllerAthena* playerController)
{
	return IsVerifiedVisibleArenaScoringAlert(alert) ||
		IsLegacyOwnerMatchedNativeArenaScoringAlert(
			alert, playerController);
}

static bool IsTrustedArenaNativeScoringConsumerUnsafe(UObject* context)
{
	const DWORD gameThreadId = g_ClientGameThreadId.load(
		std::memory_order_acquire);
	UWorld* world = UWorld::GetWorld();
	AFortPlayerControllerAthena* controller =
		GetLocalFortPlayerController();
	const bool serverArenaSession = world &&
		g_ArenaSessionBridge.World == world &&
		g_ArenaSessionBridge.Session.Active;
	if (!gameThreadId || GetCurrentThreadId() != gameThreadId ||
		!world || IsFrontendWorld(world) || !controller ||
		(!serverArenaSession &&
		 !IsActiveArenaPlaylist(world->GameState)) ||
		!IsLiveUObject(context) || context->IsDefaultObject() ||
		context == g_ArenaOwnedScoringAlertForObserver.load(
			std::memory_order_acquire) ||
		IsRejectedArenaScoringAlert(context))
	{
		return false;
	}

	const UClass* scoringAlertClass = FindClass("FortHUDScoringAlert");
	if (!IsLiveUObject(scoringAlertClass) ||
		!context->IsA(scoringAlertClass))
	{
		return false;
	}

	bool owningLocalPlayerSupported = false;
	UObject* owningLocalPlayer = GetWidgetOwningLocalPlayer(
		context, &owningLocalPlayerSupported);
	bool owningPlayerSupported = false;
	UObject* owningPlayer = GetWidgetOwningPlayer(
		context, &owningPlayerSupported);
	UObject* expectedLocalPlayer = GetOnlyLocalPlayer();
	const bool hasUsableLocalPlayerQuery =
		owningLocalPlayerSupported && owningLocalPlayer;
	const bool hasUsableControllerQuery =
		owningPlayerSupported && owningPlayer;
	const bool ownershipQuerySupported =
		hasUsableLocalPlayerQuery || hasUsableControllerQuery;
	const bool ownershipMatches = hasUsableLocalPlayerQuery
		? owningLocalPlayer == expectedLocalPlayer
		: (hasUsableControllerQuery && owningPlayer == controller);
	return AtlasArenaHudOwnershipPolicy::CanAdoptObservedNativeAlert(
		true,
		true,
		ownershipQuerySupported,
		ownershipMatches,
		HasExactlyOneLocalPlayer(),
		false,
		true);
}

static bool IsTrustedArenaNativeScoringConsumer(UObject* context)
{
	__try
	{
		return IsTrustedArenaNativeScoringConsumerUnsafe(context);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return false;
	}
}

static bool RemoveArenaScoringHudFromViewportUnsafe(UObject* widget)
{
	if (!IsLiveUObject(widget))
		return false;
	UFunction* remove = widget->GetFunction("RemoveFromParent");
	if (!MatchesArenaVisualTraceSchema(
			remove,
			EArenaVisualTraceSchema::ZeroParameters))
	{
		return false;
	}
	widget->ProcessEvent(remove, nullptr);
	return true;
}

static bool RemoveArenaScoringHudFromViewport(UObject* widget)
{
	__try
	{
		return RemoveArenaScoringHudFromViewportUnsafe(widget);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return false;
	}
}

static UObject* FindLiveArenaScoringAlertUnsafe(
	AFortPlayerControllerAthena* playerController,
	bool preferNewestNative)
{
	const UClass* arenaScoringHudClass =
		GetLiveArenaScoringHudClass();
	if (!playerController || !arenaScoringHudClass)
		return nullptr;

	struct FVisibleCandidate
	{
		UObject* Object = nullptr;
		AtlasArenaHudOwnershipPolicy::FVisibleAlertPreference
			Preference{};
	};
	constexpr std::size_t MaximumVisibleCandidates = 32;
	std::array<FVisibleCandidate,
		MaximumVisibleCandidates> candidates{};
	std::size_t candidateCount = 0;
	const bool hasExactlyOneLocalPlayer =
		HasExactlyOneLocalPlayer();
	UObject* expectedLocalPlayer = hasExactlyOneLocalPlayer
		? GetOnlyLocalPlayer()
		: nullptr;

	for (int index = 0; index < TUObjectArray::Num(); ++index)
	{
		const FUObjectItem* item =
			TUObjectArray::GetItemByIndex(index);
		UObject* object = item ? item->Object : nullptr;
		if (!object || (item->Flags & 0x20) != 0 ||
			object->IsDefaultObject())
		{
			continue;
		}
		if (!object->IsA(arenaScoringHudClass) ||
			IsRejectedArenaScoringAlert(object))
		{
			continue;
		}
		bool viewportQuerySupported = false;
		const bool inViewport = IsWidgetInViewport(
			object, &viewportQuerySupported);
		const bool visibleThroughHudHierarchy =
			IsWidgetVisibleThroughHudHierarchy(object);
		const bool visiblyAttached =
			(viewportQuerySupported && inViewport) ||
			visibleThroughHudHierarchy;
		const bool legacyOwnerMatchedNative =
			!visiblyAttached &&
			IsLegacyOwnerMatchedNativeArenaScoringAlert(
				object, playerController);
		if (!visiblyAttached && !legacyOwnerMatchedNative)
			continue;

		bool owningLocalPlayerSupported = false;
		UObject* owningLocalPlayer = GetWidgetOwningLocalPlayer(
			object, &owningLocalPlayerSupported);
		bool owningPlayerSupported = false;
		UObject* owningPlayer = GetWidgetOwningPlayer(
			object, &owningPlayerSupported);
		const bool hasUsableLocalPlayerQuery =
			owningLocalPlayerSupported && owningLocalPlayer;
		const bool hasUsableControllerQuery =
			owningPlayerSupported && owningPlayer;
		const bool ownershipQuerySupported =
			hasUsableLocalPlayerQuery || hasUsableControllerQuery;
		const bool ownershipMatches =
			(hasUsableLocalPlayerQuery &&
			 owningLocalPlayer == expectedLocalPlayer) ||
			(hasUsableControllerQuery &&
			 owningPlayer == playerController);
		if (ownershipQuerySupported && !ownershipMatches &&
			!AtlasArenaHudOwnershipPolicy::
				CanAdoptVisibleUnmatchedAlert(
					hasExactlyOneLocalPlayer, visiblyAttached))
		{
			continue;
		}

		if (candidateCount >= candidates.size())
			continue;
		auto& candidate = candidates[candidateCount++];
		candidate.Object = object;
		candidate.Preference.Source =
			IsExactOwnedArenaScoringAlert(object)
			? AtlasArenaHudOwnershipPolicy::
				EVisibleAlertSource::OwnedFallback
			: AtlasArenaHudOwnershipPolicy::
				EVisibleAlertSource::Native;
		candidate.Preference.IsCached = object ==
			g_ArenaScoringHudRepair.ScoringAlert;
		candidate.Preference.ObjectIndex = index;
		if (legacyOwnerMatchedNative)
		{
			static std::atomic_uint32_t latentOwnerLogs = 0;
			if (latentOwnerLogs.fetch_add(
					1, std::memory_order_relaxed) < 32)
			{
				AtlasDiagnostics::WriteLine(
					"arena-hud adopted-legacy-owner-matched-native "
					"alert=%p owner=%p index=%d",
					object,
					hasUsableLocalPlayerQuery
						? owningLocalPlayer : owningPlayer,
					index);
			}
		}

		if (!owningPlayerSupported || !owningPlayer)
		{
			static std::atomic_uint32_t adoptedLogs = 0;
			if (adoptedLogs.fetch_add(
					1, std::memory_order_relaxed) < 32)
			{
				AtlasDiagnostics::WriteLine(
					"arena-hud adopted-visible-alert alert=%p "
					"owner=%p owner-supported=0",
					object, owningPlayer);
			}
		}
	}

	if (candidateCount == 0)
		return nullptr;
	std::size_t selectedIndex = 0;
	for (std::size_t index = 1;
		 index < candidateCount; ++index)
	{
		if (AtlasArenaHudOwnershipPolicy::PreferVisibleAlert(
				candidates[index].Preference,
				candidates[selectedIndex].Preference,
				preferNewestNative))
		{
			selectedIndex = index;
		}
	}

	UObject* selected = candidates[selectedIndex].Object;
	const bool selectedNative =
		candidates[selectedIndex].Preference.Source ==
			AtlasArenaHudOwnershipPolicy::EVisibleAlertSource::Native;
	const bool fallbackRetirementPending = selectedNative &&
		IsExactOwnedArenaScoringAlert(
			g_ArenaScoringHudRepair.OwnedScoringAlert);
	if (fallbackRetirementPending)
	{
		// Selection changes immediately so every score write and new badge uses
		// the HUD-scaled native widget. Removal is deferred to the exact-identity
		// retirement pass so a badge already animating is not clipped.
		BeginArenaNativeHandoffRetirement();
	}
	if (selectedNative && g_ArenaSessionBridge.Session.Active)
	{
		// A visible authored widget is the current session's HUD owner even when
		// it temporarily retains the travel controller. Consume the score-neutral
		// one-shot opportunity so it cannot create a second widget later.
		MarkArenaNativeHudOwnerForActiveSession();
	}
	if (candidateCount > 1)
	{
		static std::atomic_uint32_t arbitrationLogs = 0;
		if (arbitrationLogs.fetch_add(
				1, std::memory_order_relaxed) < 32)
		{
			AtlasDiagnostics::WriteLine(
				"arena-hud duplicate-arbitrated visible=%zu "
				"fallback-pending=%d selected=%p source=%s",
				candidateCount,
				fallbackRetirementPending ? 1 : 0,
				selected,
				candidates[selectedIndex].Preference.Source ==
					AtlasArenaHudOwnershipPolicy::
						EVisibleAlertSource::Native
					? "native"
					: "fallback");
		}
	}
	return selected;
}

static UObject* FindLiveArenaScoringAlert(
	AFortPlayerControllerAthena* playerController,
	bool preferNewestNative)
{
	__try
	{
		return FindLiveArenaScoringAlertUnsafe(
			playerController, preferNewestNative);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return nullptr;
	}
}

static bool TryAdoptObservedNativeArenaScoringAlert(
	UWorld* world,
	AFortPlayerControllerAthena* playerController)
{
	UObject* observed = g_ArenaObservedNativeScoringAlert.load(
		std::memory_order_acquire);
	const std::uint64_t observedSession =
		g_ArenaObservedNativeScoringAlertSessionOrdinal.load(
			std::memory_order_acquire);
	if (!observed ||
		g_ArenaObservedNativeScoringAlertWorld.load(
			std::memory_order_acquire) != world ||
		observedSession !=
			g_ArenaSessionBridge.Session.SessionOrdinal)
	{
		return false;
	}
	if (observed == g_ArenaScoringHudRepair.ScoringAlert &&
		IsExactObservedNativeArenaScoringAlert(observed) &&
		IsVerifiedVisibleArenaScoringAlert(observed))
	{
		MarkArenaNativeHudOwnerForActiveSession();
		return true;
	}

	const UClass* scoringAlertClass = FindClass("FortHUDScoringAlert");
	const bool isScoringAlert = IsLiveUObject(observed) &&
		IsLiveUObject(scoringAlertClass) &&
		!observed->IsDefaultObject() &&
		observed->IsA(scoringAlertClass) &&
		!IsRejectedArenaScoringAlert(observed);
	bool owningPlayerSupported = false;
	UObject* owningPlayer = GetWidgetOwningPlayer(
		observed, &owningPlayerSupported);
	bool owningLocalPlayerSupported = false;
	UObject* owningLocalPlayer = GetWidgetOwningLocalPlayer(
		observed, &owningLocalPlayerSupported);
	UObject* expectedLocalPlayer = GetOnlyLocalPlayer();
	const bool hasUsableLocalPlayerQuery =
		owningLocalPlayerSupported && owningLocalPlayer;
	const bool hasUsableControllerQuery =
		owningPlayerSupported && owningPlayer;
	const bool ownershipQuerySupported =
		hasUsableLocalPlayerQuery || hasUsableControllerQuery;
	const bool ownershipMatches = hasUsableLocalPlayerQuery
		? owningLocalPlayer == expectedLocalPlayer
		: (hasUsableControllerQuery && owningPlayer == playerController);
	bool viewportQuerySupported = false;
	const bool directlyInViewport = IsWidgetInViewport(
		observed, &viewportQuerySupported);
	const bool hierarchyVisible =
		IsWidgetVisibleThroughHudHierarchy(observed);
	// Reaching any hooked scoring consumer is stronger attachment evidence
	// than IsInViewport on a nested UI-extension child. The remaining policy
	// checks still require the scoring base and the local player.
	const bool callbackProvedHierarchy = true;
	if (!AtlasArenaHudOwnershipPolicy::CanAdoptObservedNativeAlert(
			!IsExactOwnedArenaScoringAlert(observed),
			isScoringAlert,
			ownershipQuerySupported,
			ownershipMatches,
			HasExactlyOneLocalPlayer(),
			viewportQuerySupported && directlyInViewport,
			hierarchyVisible || callbackProvedHierarchy))
	{
		return false;
	}

	const auto identity = CaptureArenaScoringAlertIdentity(observed);
	if (!IsUsableArenaScoringAlertIdentity(identity))
		return false;
	g_ArenaScoringHudRepair.ObservedNativeScoringAlertIdentity = identity;
	g_ArenaScoringHudRepair.ObservedNativeSessionOrdinal = observedSession;
	g_ArenaScoringHudRepair.ScoringAlert = observed;
	// A consumer callback proves the native widget, not whether 10.x playlist
	// UIExtensions or a newer AthenaHUD owner created it. Preserve provenance;
	// only the verified UpdateUIExtensions branch may claim HUD ownership.
	g_ArenaScoringHudRepair.DuplicateAuditRequested = false;
	g_ArenaScoringHudRepair.NextObjectScanAt = 0;
	g_ArenaScoringHudRepair.NextAttemptAt = 0;
	MarkArenaNativeHudOwnerForActiveSession();
	if (IsExactOwnedArenaScoringAlert(
			g_ArenaScoringHudRepair.OwnedScoringAlert))
	{
		BeginArenaNativeHandoffRetirement();
	}
	g_ArenaLiveScoringAlert.store(
		observed, std::memory_order_release);
	MarkArenaSessionScoreRepairPending();
	const bool sessionActive = g_ArenaSessionBridge.Session.Active;
	const bool scoreApplied = !sessionActive || TrySetArenaScoreText(
		observed, g_ArenaSessionBridge.Session.CurrentHype);
	if (sessionActive && scoreApplied)
	{
		// Adoption already performed the authoritative native-widget write.
		// Account for it so the next HUD tick does not repeat SetText and
		// OnPointTextSet solely because the alert identity changed.
		g_ArenaSessionBridge.LastScoreAlert = observed;
		g_ArenaSessionBridge.LastAppliedHype =
			g_ArenaSessionBridge.Session.CurrentHype;
		g_ArenaSessionBridge.ScoreRepairAttempts = 1;
		g_ArenaSessionBridge.ScoreRepairSuccesses =
			AtlasArenaSessionBridgePolicy::
				ScoreStabilizationWriteTarget(false);
		g_ArenaSessionBridge.ScoreRepairPending = false;
		g_ArenaSessionBridge.NextScoreRepairAt = 0;
	}
	AtlasDiagnostics::WriteLine(
		"arena-hud observed-native-selected alert=%p owner=%p "
		"owner-supported=%d direct=%d hierarchy=%d score-applied=%d",
		observed,
		hasUsableLocalPlayerQuery ? owningLocalPlayer : owningPlayer,
		ownershipQuerySupported ? 1 : 0,
		viewportQuerySupported && directlyInViewport ? 1 : 0,
		hierarchyVisible ? 1 : 0,
		scoreApplied ? 1 : 0);
	return true;
}

static UObject* FindLivePlaylistUiExtensionOwnerUnsafe(
	AFortPlayerControllerAthena* playerController)
{
	for (int index = TUObjectArray::Num() - 1;
		 index >= 0; --index)
	{
		const FUObjectItem* item =
			TUObjectArray::GetItemByIndex(index);
		UObject* object = item ? item->Object : nullptr;
		if (!object || (item->Flags & 0x20) != 0 ||
			object->IsDefaultObject())
		{
			continue;
		}

		// The concrete loader class changed across Fortnite generations. Its
		// reflected capabilities are the stable contract, so do not exclude a
		// valid UE5 owner just because its class name is new.
		if (!object->GetFunction("UpdateUIExtensions") ||
			object->GetOffset(
				"ArenaTournamentUIExtensions") == UINT32_MAX ||
			!BelongsToLocalPlayer(object, playerController))
		{
			continue;
		}
		return object;
	}
	return nullptr;
}

static UObject* FindLivePlaylistUiExtensionOwner(
	AFortPlayerControllerAthena* playerController)
{
	__try
	{
		return FindLivePlaylistUiExtensionOwnerUnsafe(
			playerController);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return nullptr;
	}
}

static bool NormalizeArenaTournamentHudExtensionsUnsafe(
	UObject* owner,
	const char* source)
{
	if (!IsLiveUObject(owner) || !owner->Class ||
		!g_PlaylistExtensionsInitialized ||
		g_ArenaExtensions.Num() != 1 ||
		!g_ArenaExtensions.GetData())
	{
		return false;
	}

	const uint32 offset = owner->GetOffset(
		"ArenaTournamentUIExtensions");
	if (!IsReflectedOffsetInBounds(
			owner->Class, offset,
			sizeof(TArray<FUIExtension>)))
	{
		return false;
	}

	auto& extensions =
		*reinterpret_cast<TArray<FUIExtension>*>(
			reinterpret_cast<uint8*>(owner) + offset);
	return NormalizeCompetitiveUiExtensionArrayUnsafe(
		extensions,
		g_ArenaExtensions.Get(0),
		true,
		source,
		owner);
}

static bool NormalizeArenaTournamentHudExtensions(
	UObject* owner,
	const char* source)
{
	__try
	{
		return NormalizeArenaTournamentHudExtensionsUnsafe(
			owner, source);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		AtlasDiagnostics::WriteLine(
			"arena-hud extension-normalization-faulted "
			"source=%s owner=%p",
			source ? source : "unknown", owner);
		return false;
	}
}

static bool HasValidArenaTournamentHudExtensionArrayUnsafe(
	UObject* owner)
{
	if (!IsLiveUObject(owner) || !owner->Class ||
		!g_PlaylistExtensionsInitialized)
	{
		return false;
	}
	const uint32 offset = owner->GetOffset(
		"ArenaTournamentUIExtensions");
	if (!IsReflectedOffsetInBounds(
		owner->Class, offset,
		sizeof(TArray<FUIExtension>)))
	{
		return false;
	}
	const auto& extensions =
		*reinterpret_cast<const TArray<FUIExtension>*>(
			reinterpret_cast<const uint8*>(owner) + offset);
	return IsPlaylistUiExtensionArrayShapeValid(
		extensions, GetPlaylistUiExtensionSize());
}

static bool HasValidArenaTournamentHudExtensionArray(
	UObject* owner)
{
	__try
	{
		return HasValidArenaTournamentHudExtensionArrayUnsafe(owner);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return false;
	}
}

static bool IsObjectInputParameter(
	const UFunction::ParamNamed& parameter,
	const char* name,
	uint32 maximumSize)
{
	using namespace AtlasArenaVisualTracePolicy;
	return name && parameter.Name == name &&
		parameter.ElementSize == sizeof(UObject*) &&
		parameter.Offset <= maximumSize &&
		parameter.ElementSize <= maximumSize - parameter.Offset &&
		(parameter.PropertyFlags & CpfParm) != 0 &&
		(parameter.PropertyFlags &
			(CpfOutParm | CpfReturnParm)) == 0;
}

static bool UpdatePlaylistUiExtensionsUnsafe(
	UObject* owner,
	UObject* playerState)
{
	if (!IsLiveUObject(owner) || !IsLiveUObject(playerState))
		return false;

	UFunction* function = owner->GetFunction(
		"UpdateUIExtensions");
	if (!function)
		return false;
	const auto schema = function->GetParamsNamed();
	if (schema.Size == 0 || schema.Size > 0x40 ||
		schema.NameOffsetMap.size() > 16)
	{
		return false;
	}

	const UFunction::ParamNamed* clientState = nullptr;
	const UFunction::ParamNamed* minigame = nullptr;
	for (const auto& parameter : schema.NameOffsetMap)
	{
		if (IsObjectInputParameter(
				parameter, "ClientState", schema.Size))
		{
			clientState = &parameter;
		}
		else if (IsObjectInputParameter(
				parameter, "Minigame", schema.Size))
		{
			minigame = &parameter;
		}
	}
	if (!clientState || !minigame)
		return false;

	std::array<uint8, 0x40> parameters{};
	memcpy(parameters.data() + clientState->Offset,
		&playerState, sizeof(playerState));
	UObject* noMinigame = nullptr;
	memcpy(parameters.data() + minigame->Offset,
		&noMinigame, sizeof(noMinigame));
	owner->ProcessEvent(function, parameters.data());
	return true;
}

static bool HasValidUpdatePlaylistUiExtensionsUnsafe(
	UObject* owner)
{
	if (!IsLiveUObject(owner))
		return false;
	UFunction* function = owner->GetFunction(
		"UpdateUIExtensions");
	if (!function)
		return false;
	const auto schema = function->GetParamsNamed();
	if (schema.Size == 0 || schema.Size > 0x40 ||
		schema.NameOffsetMap.size() > 16)
	{
		return false;
	}

	bool hasClientState = false;
	bool hasMinigame = false;
	for (const auto& parameter : schema.NameOffsetMap)
	{
		hasClientState = hasClientState ||
			IsObjectInputParameter(
				parameter, "ClientState", schema.Size);
		hasMinigame = hasMinigame ||
			IsObjectInputParameter(
				parameter, "Minigame", schema.Size);
	}
	return hasClientState && hasMinigame;
}

static bool HasValidUpdatePlaylistUiExtensions(
	UObject* owner)
{
	__try
	{
		return HasValidUpdatePlaylistUiExtensionsUnsafe(owner);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return false;
	}
}

static bool TryUpdatePlaylistUiExtensions(
	UObject* owner,
	UObject* playerState)
{
	__try
	{
		return UpdatePlaylistUiExtensionsUnsafe(
			owner, playerState);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return false;
	}
}

static const UClass* LoadArenaScoringHudClass()
{
	__try
	{
		return FindObject<UClass>(
			L"/Game/UI/Competitive/Arena/"
			L"ArenaScoringHUD.ArenaScoringHUD_C");
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return nullptr;
	}
}

static const UClass* LoadShowdownScoringHudClass()
{
	__try
	{
		return FindObject<UClass>(
			L"/Game/UI/Frontend/Showdown/"
			L"ShowdownScoringHUD.ShowdownScoringHUD_C");
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return nullptr;
	}
}

static bool MatchesAddToViewportSchemaUnsafe(
	UFunction* function)
{
	using namespace AtlasArenaVisualTracePolicy;
	if (!function)
		return false;
	const auto schema = function->GetParamsNamed();
	if (schema.Size < sizeof(int32) || schema.Size > 0x40 ||
		schema.NameOffsetMap.size() > 16)
	{
		return false;
	}

	for (const auto& parameter : schema.NameOffsetMap)
	{
		if (parameter.Name == "ZOrder" &&
			parameter.ElementSize == sizeof(int32) &&
			parameter.Offset <= schema.Size &&
			parameter.ElementSize <=
				schema.Size - parameter.Offset &&
			(parameter.PropertyFlags & CpfParm) != 0 &&
			(parameter.PropertyFlags &
				(CpfOutParm | CpfReturnParm)) == 0)
		{
			return true;
		}
	}
	return false;
}

static bool AddArenaScoringHudToViewportUnsafe(
	UObject* widget,
	UFunction* function)
{
	if (!widget || !function)
		return false;
	const auto schema = function->GetParamsNamed();
	if (!MatchesAddToViewportSchemaUnsafe(function))
		return false;
	const UFunction::ParamNamed* zOrderParameter = nullptr;
	for (const auto& parameter : schema.NameOffsetMap)
	{
		if (parameter.Name == "ZOrder" &&
			parameter.ElementSize == sizeof(int32))
		{
			zOrderParameter = &parameter;
			break;
		}
	}
	if (!zOrderParameter)
		return false;

	std::array<uint8, 0x40> parameters{};
	int32 zOrder = 10;
	memcpy(parameters.data() + zOrderParameter->Offset,
		&zOrder, sizeof(zOrder));
	widget->ProcessEvent(function, parameters.data());
	return true;
}

static bool TryAddArenaScoringHudToViewport(
	UObject* widget,
	UFunction* function)
{
	if (!widget || !function)
		return false;
	__try
	{
		return AddArenaScoringHudToViewportUnsafe(
			widget, function);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return false;
	}
}

static UObject* CreateArenaScoringHudUnsafe(
	UWorld* world,
	AFortPlayerControllerAthena* playerController,
	const UClass* widgetClass)
{
	if (!IsLiveUObject(world) || !IsLiveUObject(playerController) ||
		!IsLiveUObject(widgetClass))
	{
		return nullptr;
	}
	const UClass* scoringAlertClass =
		FindClass("FortHUDScoringAlert");
	UObject* widgetDefault = const_cast<UObject*>(
		widgetClass->GetDefaultObj());
	UFunction* validatedAddToViewport = widgetDefault
		? widgetDefault->GetFunction("AddToViewport")
		: nullptr;
	if (!scoringAlertClass || !widgetDefault ||
		!widgetDefault->IsA(scoringAlertClass) ||
		!MatchesAddToViewportSchemaUnsafe(
			validatedAddToViewport))
	{
		return nullptr;
	}

	const UClass* libraryClass =
		FindClass("WidgetBlueprintLibrary");
	UObject* library = libraryClass
		? const_cast<UObject*>(libraryClass->GetDefaultObj())
		: nullptr;
	UFunction* create = library
		? library->GetFunction("Create")
		: nullptr;
	if (!library || !create)
		return nullptr;

	const auto schema = create->GetParamsNamed();
	if (schema.Size == 0 || schema.Size > 0x50 ||
		schema.NameOffsetMap.size() > 20)
	{
		return nullptr;
	}

	const UFunction::ParamNamed* worldContext = nullptr;
	const UFunction::ParamNamed* widgetType = nullptr;
	const UFunction::ParamNamed* owningPlayer = nullptr;
	const UFunction::ParamNamed* returnValue = nullptr;
	for (const auto& parameter : schema.NameOffsetMap)
	{
		if (IsObjectInputParameter(
				parameter, "WorldContextObject", schema.Size))
		{
			worldContext = &parameter;
		}
		else if (IsObjectInputParameter(
				parameter, "WidgetType", schema.Size))
		{
			widgetType = &parameter;
		}
		else if (IsObjectInputParameter(
				parameter, "OwningPlayer", schema.Size))
		{
			owningPlayer = &parameter;
		}
		else if (parameter.Name == "ReturnValue" &&
			parameter.ElementSize == sizeof(UObject*) &&
			parameter.Offset <= schema.Size &&
			parameter.ElementSize <=
				schema.Size - parameter.Offset &&
			(parameter.PropertyFlags &
				AtlasArenaVisualTracePolicy::CpfReturnParm) != 0)
		{
			returnValue = &parameter;
		}
	}
	if (!worldContext || !widgetType ||
		!owningPlayer || !returnValue)
	{
		return nullptr;
	}

	std::array<uint8, 0x50> parameters{};
	UObject* worldObject = world;
	UObject* widgetTypeObject =
		const_cast<UClass*>(widgetClass);
	UObject* owningPlayerObject = playerController;
	memcpy(parameters.data() + worldContext->Offset,
		&worldObject, sizeof(worldObject));
	memcpy(parameters.data() + widgetType->Offset,
		&widgetTypeObject, sizeof(widgetTypeObject));
	memcpy(parameters.data() + owningPlayer->Offset,
		&owningPlayerObject, sizeof(owningPlayerObject));
	library->ProcessEvent(create, parameters.data());

	UObject* widget = nullptr;
	memcpy(&widget,
		parameters.data() + returnValue->Offset,
		sizeof(widget));
	if (!IsLiveUObject(widget) ||
		!widget->IsA(widgetClass) ||
		!BelongsToLocalPlayer(
			widget, playerController, true))
	{
		RejectArenaScoringAlert(widget);
		return nullptr;
	}

	UFunction* addToViewport = widget->GetFunction(
		"AddToViewport");
	if (!MatchesAddToViewportSchemaUnsafe(addToViewport) ||
		!TryAddArenaScoringHudToViewport(
			widget, addToViewport))
	{
		RejectArenaScoringAlert(widget);
		return nullptr;
	}

	// ProcessEvent returning only proves that AddToViewport was invoked. A
	// transiently unready viewport can accept the call without displaying the
	// widget; never let that invisible object suppress later repair attempts.
	bool viewportQuerySupported = false;
	const bool inViewport = IsWidgetInViewport(
		widget, &viewportQuerySupported);
	if (!viewportQuerySupported || !inViewport)
	{
		UFunction* remove = widget->GetFunction("RemoveFromParent");
		if (MatchesArenaVisualTraceSchema(
				remove, EArenaVisualTraceSchema::ZeroParameters))
		{
			widget->ProcessEvent(remove, nullptr);
		}
		RejectArenaScoringAlert(widget);
		return nullptr;
	}
	return widget;
}

static UObject* TryCreateArenaScoringHud(
	UWorld* world,
	AFortPlayerControllerAthena* playerController,
	const UClass* widgetClass)
{
	__try
	{
		return CreateArenaScoringHudUnsafe(
			world, playerController, widgetClass);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return nullptr;
	}
}

static bool RemoveOwnedArenaScoringHud()
{
	UObject* widget = g_ArenaScoringHudRepair.OwnedScoringAlert;
	if (!widget)
		return true;
	if (!IsExactOwnedArenaScoringAlert(widget))
	{
		// The address or UObject slot was recycled. Drop stale bookkeeping but
		// never call RemoveFromParent on an object ATLAS can no longer prove it
		// created.
		ClearOwnedArenaScoringAlertTracking();
		return true;
	}
	const bool removalInvoked =
		RemoveArenaScoringHudFromViewport(widget);
	bool viewportQuerySupported = false;
	const bool stillVisible = IsWidgetInViewport(
		widget, &viewportQuerySupported);
	return removalInvoked && viewportQuerySupported && !stillVisible;
}

static void QueueArenaOwnedScoringAlertRemoval(
	UObject* widget,
	const AtlasArenaHudOwnershipPolicy::FObjectIdentity& identity,
	ULONGLONG now)
{
	if (!widget || !IsUsableArenaScoringAlertIdentity(identity))
		return;
	FPendingArenaOwnedScoringAlertRemoval* available = nullptr;
	for (auto& pending : g_PendingArenaOwnedScoringAlertRemovals)
	{
		if (pending.Active &&
			AtlasArenaHudOwnershipPolicy::IsSameObjectInstance(
				pending.Identity, identity))
		{
			pending.RetryAt = (std::min)(pending.RetryAt, now + 250);
			return;
		}
		if (!pending.Active && !available)
			available = &pending;
	}
	if (!available)
	{
		AtlasDiagnostics::WriteLine(
			"arena-hud owned-removal-queue-full widget=%p", widget);
		return;
	}
	*available = {};
	available->Widget = widget;
	available->Identity = identity;
	available->RetryAt = now + 250;
	available->Active = true;
}

static void MaintainPendingArenaOwnedScoringAlertRemovals(
	ULONGLONG now)
{
	for (auto& pending : g_PendingArenaOwnedScoringAlertRemovals)
	{
		if (!pending.Active)
			continue;
		if (!MatchesArenaScoringAlertIdentity(
				pending.Widget, pending.Identity))
		{
			pending = {};
			continue;
		}
		if (now < pending.RetryAt)
			continue;

		const bool removalInvoked =
			RemoveArenaScoringHudFromViewport(pending.Widget);
		bool viewportQuerySupported = false;
		const bool stillVisible = IsWidgetInViewport(
			pending.Widget, &viewportQuerySupported);
		if (removalInvoked && viewportQuerySupported && !stillVisible)
		{
			AtlasDiagnostics::WriteLine(
				"arena-hud queued-owned-fallback-retired widget=%p",
				pending.Widget);
			pending = {};
			continue;
		}
		pending.RetryAt = now + 250;
	}
}

static void MaintainArenaNativeHandoffRetirement(ULONGLONG now)
{
	if (!g_ArenaScoringHudRepair.OwnedFallbackNativeRetirementPending)
		return;
	UObject* native = g_ArenaScoringHudRepair.ScoringAlert;
	UObject* owned = g_ArenaScoringHudRepair.OwnedScoringAlert;
	const bool exactOwned = IsExactOwnedArenaScoringAlert(owned);
	if (!owned)
	{
		ClearOwnedArenaScoringAlertTracking();
		return;
	}
	if (!exactOwned)
	{
		AtlasDiagnostics::WriteLine(
			"arena-hud native-handoff-owned-identity-stale widget=%p",
			owned);
		ClearOwnedArenaScoringAlertTracking();
		return;
	}
	const bool nativeSelected =
		IsVerifiedNativeOwnedArenaScoringAlert(native) ||
		IsLegacyOwnerMatchedNativeArenaScoringAlert(
			native, GetLocalFortPlayerController());
	const ULONGLONG retirementDeadline = (std::max)(
		g_ArenaScoringHudRepair.NativeHandoffRetireAt,
		g_ArenaScoringHudRepair.NativeHandoffRetryAt);
	if (!AtlasArenaHudOwnershipPolicy::
			CanRetireOwnedFallbackForNative(
				nativeSelected,
				exactOwned,
				retirementDeadline,
				now))
	{
		return;
	}

	const bool removalInvoked =
		RemoveArenaScoringHudFromViewport(owned);
	bool viewportQuerySupported = false;
	const bool stillVisible = IsWidgetInViewport(
		owned, &viewportQuerySupported);
	if (!removalInvoked || !viewportQuerySupported || stillVisible)
	{
		g_ArenaScoringHudRepair.NativeHandoffRetryAt = now + 250;
		return;
	}

	RejectArenaScoringAlert(owned);
	if (g_ArenaScoringHudRepair.ScoringAlert == owned)
		g_ArenaScoringHudRepair.ScoringAlert = native;
	if (g_ArenaLiveScoringAlert.load(std::memory_order_acquire) == owned)
	{
		g_ArenaLiveScoringAlert.store(
			native, std::memory_order_release);
	}
	ClearOwnedArenaScoringAlertTracking();
	AtlasDiagnostics::WriteLine(
		"arena-hud native-handoff-fallback-retired native=%p fallback=%p",
		native, owned);
}

static void MaintainArenaOwnedFallbackLifecycle(ULONGLONG now)
{
	auto& bridge = g_ArenaSessionBridge;
	const std::uint64_t sessionOrdinal =
		bridge.Session.SessionOrdinal;
	if (!bridge.Session.Active || sessionOrdinal == 0)
	{
		return;
	}

	// Native observations and direct bridge dispatches both reserve this
	// deadline for the authored badge animation. Do not add a second hold after
	// the queue drains; that would leave the fallback over Match Stats.
	const ULONGLONG retireAt = (std::max)(
		g_NextArenaBridgeVisualDispatchAt,
		bridge.OwnedFallbackRetireAt);
	if (!AtlasArenaHudOwnershipPolicy::
			CanFinalizeOwnedFallbackRetirement(
				sessionOrdinal,
				bridge.TerminalSessionOrdinal,
				HasPendingArenaBridgeVisualForSession(sessionOrdinal),
				retireAt,
				now))
	{
		return;
	}

	// Only suppress creation after every terminal badge has had a chance to
	// obtain an alert and finish presenting. Terminal receipt alone must not
	// prevent a late fallback from being created for the final visual.
	bridge.OwnedFallbackSuppressedSessionOrdinal = sessionOrdinal;
	UObject* owned = g_ArenaScoringHudRepair.OwnedScoringAlert;
	if (!owned)
	{
		bridge.OwnedFallbackRetireAt = 0;
		return;
	}
	if (!IsExactOwnedArenaScoringAlert(owned))
	{
		// Never remove a recycled UObject that merely inherited the old address.
		ClearOwnedArenaScoringAlertTracking();
		bridge.OwnedFallbackRetireAt = 0;
		return;
	}

	const bool removalInvoked =
		RemoveArenaScoringHudFromViewport(owned);
	bool viewportQuerySupported = false;
	const bool stillVisible = IsWidgetInViewport(
		owned, &viewportQuerySupported);
	if (!removalInvoked || !viewportQuerySupported || stillVisible)
	{
		// RemoveFromParent can race a widget animation/construction pass. Retry
		// without dropping the only strong identity we can safely remove.
		bridge.OwnedFallbackRetireAt = now + 250;
		static std::atomic_uint32_t retryLogs = 0;
		if (retryLogs.fetch_add(1, std::memory_order_relaxed) < 16)
		{
			AtlasDiagnostics::WriteLine(
				"arena-hud fallback-retirement-retry session=%llu "
				"widget=%p invoked=%d viewport-supported=%d visible=%d",
				static_cast<unsigned long long>(sessionOrdinal),
				owned,
				removalInvoked ? 1 : 0,
				viewportQuerySupported ? 1 : 0,
				stillVisible ? 1 : 0);
		}
		return;
	}

	RejectArenaScoringAlert(owned);
	if (g_ArenaScoringHudRepair.ScoringAlert == owned)
		g_ArenaScoringHudRepair.ScoringAlert = nullptr;
	if (g_ArenaLiveScoringAlert.load(std::memory_order_acquire) == owned)
	{
		g_ArenaLiveScoringAlert.store(
			nullptr, std::memory_order_release);
	}
	ClearOwnedArenaScoringAlertTracking();
	g_ArenaScoringHudRepair.DuplicateAuditRequested = true;
	g_ArenaScoringHudRepair.NextObjectScanAt = 0;
	g_ArenaScoringHudRepair.NextAttemptAt = 0;
	bridge.OwnedFallbackRetireAt = 0;
	AtlasDiagnostics::WriteLine(
		"arena-hud fallback-retired session=%llu widget=%p",
		static_cast<unsigned long long>(sessionOrdinal), owned);
}

static void ResetArenaScoringHudRepair(UWorld* world)
{
	ResetArenaSessionBridgeForWorld(world);
	g_ArenaVisibleAlertReauditRequested.store(
		false, std::memory_order_release);
	ResetArenaObservedNativeCandidate();
	UObject* owned = g_ArenaScoringHudRepair.OwnedScoringAlert;
	const auto ownedIdentity =
		g_ArenaScoringHudRepair.OwnedScoringAlertIdentity;
	if (!RemoveOwnedArenaScoringHud() &&
		MatchesArenaScoringAlertIdentity(owned, ownedIdentity))
	{
		QueueArenaOwnedScoringAlertRemoval(
			owned, ownedIdentity, GetTickCount64());
	}
	ClearOwnedArenaScoringAlertTracking();
	ResetPendingArenaVisualFallbacks();
	g_LastArenaPresentedPlacement = 0;
	g_LastArenaPresentedPlacementAt = 0;
	g_LastArenaPresentedRpcPlacement = 0;
	g_LastArenaPresentedRpcPoints = 0;
	g_LastArenaPresentedRpcAt = 0;
	g_LastArenaRpcPresented = false;
	g_ArenaLiveScoringAlert.store(
		nullptr, std::memory_order_release);
	// Rejections only arbitrate duplicate/removed widgets inside one world.
	// Carrying raw object history across travel can hide the next match's
	// authored Arena HUD when Unreal recycles a UObject slot or address.
	g_ArenaScoringHudRepair = {};
	g_ArenaScoringHudRepair.World = world;
}

static void RequestArenaScoringHudRefresh()
{
	UObject* preservedVisibleAlert = nullptr;
	UObject* owned = g_ArenaScoringHudRepair.OwnedScoringAlert;
	if (owned)
	{
		if (IsExactOwnedArenaScoringAlert(owned) &&
			IsVerifiedVisibleArenaScoringAlert(owned))
		{
			preservedVisibleAlert = owned;
			// A manual fallback is not proof that the newer HUD-owner array is
			// active, so keep the playlist extension as the native source.
			g_ArenaScoringHudRepair.UsesHudOwnerSource = false;
		}
		else
		{
			if (RemoveOwnedArenaScoringHud())
			{
				if (IsExactOwnedArenaScoringAlert(owned))
					RejectArenaScoringAlert(owned);
				ClearOwnedArenaScoringAlertTracking();
			}
		}
	}
	if (!preservedVisibleAlert &&
		IsVerifiedVisibleArenaScoringAlert(
			g_ArenaScoringHudRepair.ScoringAlert))
	{
		preservedVisibleAlert =
			g_ArenaScoringHudRepair.ScoringAlert;
	}
	if (!preservedVisibleAlert)
		g_ArenaScoringHudRepair.UsesHudOwnerSource = false;
	g_ArenaScoringHudRepair.AthenaHud = nullptr;
	g_ArenaScoringHudRepair.ScoringAlert = preservedVisibleAlert;
	g_ArenaScoringHudRepair.NextAttemptAt = 0;
	g_ArenaScoringHudRepair.NextObjectScanAt = 0;
	g_ArenaScoringHudRepair.Attempts = 0;
	g_ArenaScoringHudRepair.DuplicateAuditRequested = true;
	g_ArenaScoringHudRepair.InitialUpdateAttempted = false;
	g_ArenaScoringHudRepair.DirectCreateAttempts = 0;
	g_ArenaScoringHudRepair.NextDirectCreateAt = 0;
	g_ArenaScoringHudRepair.ExhaustionLogged = false;
	g_ArenaLiveScoringAlert.store(
		preservedVisibleAlert, std::memory_order_release);
}

static void MaintainArenaScoringHud(
	UWorld* world,
	AFortPlayerControllerAthena* playerController,
	ULONGLONG now)
{
	using namespace AtlasPlaylistHudPatchPolicy;
	if (world != g_ArenaScoringHudRepair.World)
	{
		ResetArenaScoringHudRepair(world);
		AtlasDiagnostics::WriteLine(
			"arena-hud world-changed world=%p", world);
	}

	if (!world || IsFrontendWorld(world) || !playerController ||
		!IsLiveUObject(playerController->PlayerState))
	{
		return;
	}
	if (g_ArenaScoringHudRepair.OwnedScoringAlert &&
		!IsExactOwnedArenaScoringAlert(
			g_ArenaScoringHudRepair.OwnedScoringAlert))
	{
		ClearOwnedArenaScoringAlertTracking();
	}
	const bool serverArenaSession =
		g_ArenaSessionBridge.World == world &&
		g_ArenaSessionBridge.Session.Active;
	const bool activeArenaPlaylist =
		IsActiveArenaPlaylist(world->GameState);
	if (AtlasArenaHudOwnershipPolicy::
			ShouldReserveLegacyNativeHudOwnership(
				UsesLegacyPlaylistOwnedArenaHud(
					VersionInfo.FortniteVersion),
				serverArenaSession,
				activeArenaPlaylist))
	{
		// FN 10.x publishes the Arena extension before its nested widget becomes
		// discoverable. Treat that authored source as the session owner now so a
		// bus event cannot create ATLAS's differently-scaled second total.
		MarkArenaNativeHudOwnerForActiveSession();
	}
	const bool arenaHudRequired =
		activeArenaPlaylist || serverArenaSession;
	if (g_ArenaVisibleAlertReauditRequested.exchange(
			false, std::memory_order_acq_rel))
	{
		g_ArenaScoringHudRepair.DuplicateAuditRequested = true;
		g_ArenaScoringHudRepair.NextObjectScanAt = 0;
		g_ArenaScoringHudRepair.NextAttemptAt = 0;
	}
	if (arenaHudRequired)
	{
		TryAdoptObservedNativeArenaScoringAlert(
			world, playerController);
	}
	MaintainArenaNativeHandoffRetirement(now);
	MaintainArenaOwnedFallbackLifecycle(now);

	UObject* cachedAlert =
		g_ArenaScoringHudRepair.ScoringAlert;
	if (now < g_ArenaScoringHudRepair.NextAttemptAt)
	{
		if (IsUsableArenaHudOwnerAlert(
				cachedAlert, playerController))
		{
			g_ArenaLiveScoringAlert.store(
				cachedAlert, std::memory_order_release);
		}
		else
		{
			g_ArenaLiveScoringAlert.store(
				nullptr, std::memory_order_release);
		}
		return;
	}
	g_ArenaScoringHudRepair.NextAttemptAt = now +
		(g_ArenaScoringHudRepair.ExhaustionLogged ? 1000 : 500);

	if (!arenaHudRequired)
	{
		ResetPendingArenaVisualFallbacks();
		UObject* owned = g_ArenaScoringHudRepair.OwnedScoringAlert;
		if (owned)
		{
			if (RemoveOwnedArenaScoringHud())
			{
				if (IsExactOwnedArenaScoringAlert(owned))
					RejectArenaScoringAlert(owned);
				ClearOwnedArenaScoringAlertTracking();
			}
		}
		ResetArenaObservedNativeCandidate();
		g_ArenaScoringHudRepair.AthenaHud = nullptr;
		g_ArenaScoringHudRepair.ScoringAlert = nullptr;
		g_ArenaScoringHudRepair.NextObjectScanAt = 0;
		g_ArenaScoringHudRepair.Attempts = 0;
		g_ArenaScoringHudRepair.DuplicateAuditRequested = true;
		g_ArenaScoringHudRepair.UsesHudOwnerSource = false;
		g_ArenaScoringHudRepair.InitialUpdateAttempted = false;
		g_ArenaScoringHudRepair.DirectCreateAttempts = 0;
		g_ArenaScoringHudRepair.NextDirectCreateAt = 0;
		g_ArenaScoringHudRepair.ExhaustionLogged = false;
		g_ArenaLiveScoringAlert.store(
			nullptr, std::memory_order_release);
		return;
	}

	const bool cachedAlertVisible =
		IsUsableArenaHudOwnerAlert(
			cachedAlert, playerController);
	const auto cachedAlertSource =
		IsExactOwnedArenaScoringAlert(cachedAlert)
		? AtlasArenaHudOwnershipPolicy::
			EVisibleAlertSource::OwnedFallback
		: AtlasArenaHudOwnershipPolicy::EVisibleAlertSource::Native;
	const bool scanVisibleWidgets =
		AtlasArenaHudOwnershipPolicy::ShouldScanVisibleAlerts(
			cachedAlertVisible,
			cachedAlertSource,
			g_ArenaScoringHudRepair.DuplicateAuditRequested,
			now,
			g_ArenaScoringHudRepair.NextObjectScanAt);
	if (cachedAlertVisible && !scanVisibleWidgets)
	{
		g_ArenaLiveScoringAlert.store(
			cachedAlert, std::memory_order_release);
		return;
	}

	if (scanVisibleWidgets)
	{
		g_ArenaScoringHudRepair.DuplicateAuditRequested = false;
		// A global UObject walk is reserved for repair/refresh and a bounded
		// retry while no widget is cached. Never repeat it every HUD tick.
		g_ArenaScoringHudRepair.NextObjectScanAt = now +
			AtlasArenaHudOwnershipPolicy::
				kOwnedFallbackNativeProbeMilliseconds;
		if (UObject* alert = FindLiveArenaScoringAlert(
				playerController))
		{
			if (alert != g_ArenaScoringHudRepair.ScoringAlert)
			{
				g_ArenaScoringHudRepair.ScoringAlert = alert;
				AtlasDiagnostics::WriteLine(
					"arena-hud scoring-alert-ready alert=%p "
					"attempts=%u",
					alert, g_ArenaScoringHudRepair.Attempts);
			}
			g_ArenaLiveScoringAlert.store(
				alert, std::memory_order_release);
			return;
		}
	}
	if (cachedAlertVisible)
	{
		g_ArenaLiveScoringAlert.store(
			cachedAlert, std::memory_order_release);
		return;
	}
	g_ArenaLiveScoringAlert.store(
		nullptr, std::memory_order_release);
	const bool supportsUiExtensionPatch =
		SupportsUiExtensionPatch(VersionInfo.FortniteVersion);
	if (supportsUiExtensionPatch)
	{
		InitializePlaylistExtensions();
	}
	// 8.20-9.x use an authored legacy HUD path instead of FUIExtension. The
	// exact visible widget is adopted above; if that path is absent, continue
	// to the validated direct-widget fallback without touching an unknown
	// playlist layout.

	UObject* owner = g_ArenaScoringHudRepair.AthenaHud;
	if (!IsLiveUObject(owner) ||
		!BelongsToLocalPlayer(owner, playerController))
	{
		owner = FindLivePlaylistUiExtensionOwner(
			playerController);
	}
	g_ArenaScoringHudRepair.AthenaHud = owner;
	bool instanceChanged = false;
	bool defaultChanged = false;
	// 10.x owns ArenaScoringHUD through the playlist's UIExtensions array.
	// Its loader also exposes similarly named owner fields, but handing off to
	// that path can remove the only authored source before a widget exists.
	const bool allowHudOwnerSource =
		VersionInfo.FortniteVersion >= 11.00;
	const bool ownerSchemaValid = allowHudOwnerSource && owner &&
		HasValidUpdatePlaylistUiExtensions(owner) &&
		HasValidArenaTournamentHudExtensionArray(owner);
	if (ownerSchemaValid &&
		(!g_ArenaScoringHudRepair.InitialUpdateAttempted ||
		 g_ArenaScoringHudRepair.UsesHudOwnerSource))
	{
		instanceChanged = NormalizeArenaTournamentHudExtensions(
			owner, "live-hud");
		UObject* defaultOwner = owner->Class
			? const_cast<UObject*>(owner->Class->GetDefaultObj())
			: nullptr;
		if (defaultOwner && defaultOwner != owner)
		{
			defaultChanged = NormalizeArenaTournamentHudExtensions(
				defaultOwner, "hud-default");
		}
	}
	else if (!ownerSchemaValid &&
		g_ArenaScoringHudRepair.UsesHudOwnerSource)
	{
		g_ArenaScoringHudRepair.UsesHudOwnerSource = false;
		NormalizeActiveCompetitivePlaylist(
			world->GameState, "hud-owner-invalid");
	}

	if (!g_ArenaScoringHudRepair.ArenaScoringHudClass &&
		g_ArenaScoringHudRepair.Attempts >= 1)
	{
		g_ArenaScoringHudRepair.ArenaScoringHudClass =
			LoadArenaScoringHudClass();
	}

	bool updated = false;
	if (ownerSchemaValid &&
		!g_ArenaScoringHudRepair.InitialUpdateAttempted)
	{
		// Keep the playlist extension alive during the handoff. The owner array
		// becomes authoritative only after its exact ArenaScoringHUD_C is
		// verified in the viewport.
		g_ArenaScoringHudRepair.UsesHudOwnerSource = false;
		updated = TryUpdatePlaylistUiExtensions(
			owner, playerController->PlayerState);
		g_ArenaScoringHudRepair.InitialUpdateAttempted = true;
		if (!updated)
		{
			// A validated reflection layout can still fail at runtime. Put the
			// playlist source back immediately instead of leaving this version
			// without either supported HUD path.
			g_ArenaScoringHudRepair.UsesHudOwnerSource = false;
			NormalizeActiveCompetitivePlaylist(
				world->GameState, "hud-owner-update-failed");
		}
	}
	if (g_ArenaScoringHudRepair.Attempts < UINT_MAX)
		++g_ArenaScoringHudRepair.Attempts;

	const unsigned refreshAttempt =
		g_ArenaScoringHudRepair.Attempts;
	if (refreshAttempt <= 16 ||
		(refreshAttempt & (refreshAttempt - 1)) == 0 ||
		instanceChanged || defaultChanged || updated)
	{
		AtlasDiagnostics::WriteLine(
			"arena-hud refresh-attempt attempt=%u owner=%p "
			"owner-valid=%d instance-changed=%d default-changed=%d "
			"class=%p owner-source=%d updated=%d",
			refreshAttempt, owner,
			ownerSchemaValid ? 1 : 0,
			instanceChanged ? 1 : 0,
			defaultChanged ? 1 : 0,
			g_ArenaScoringHudRepair.ArenaScoringHudClass,
			g_ArenaScoringHudRepair.UsesHudOwnerSource ? 1 : 0,
			updated ? 1 : 0);
	}

	// UpdateUIExtensions may create the native widget synchronously. Observe
	// that result before considering the manual fallback so one scoring event
	// can never be bound to two local alert widgets.
	if (updated)
	{
		if (UObject* alert = FindLiveArenaScoringAlert(
				playerController, true))
		{
			g_ArenaScoringHudRepair.ScoringAlert = alert;
			g_ArenaScoringHudRepair.UsesHudOwnerSource = true;
			NormalizeActiveCompetitivePlaylist(
				world->GameState, "hud-owner-verified");
			g_ArenaLiveScoringAlert.store(
				alert, std::memory_order_release);
			AtlasDiagnostics::WriteLine(
				"arena-hud scoring-alert-ready alert=%p "
				"attempts=%u source=ui-extension-update",
				alert, g_ArenaScoringHudRepair.Attempts);
			return;
		}
		g_ArenaScoringHudRepair.UsesHudOwnerSource = false;
		NormalizeActiveCompetitivePlaylist(
			world->GameState, "hud-owner-no-visible-widget");
	}

	const unsigned directCreateThreshold =
		AtlasArenaSessionBridgePolicy::
			SelectHudFallbackAttemptThreshold(
				allowHudOwnerSource,
				serverArenaSession,
				ownerSchemaValid,
				g_ArenaScoringHudRepair.InitialUpdateAttempted);
	const bool ownedFallbackAllowed =
		AtlasArenaHudOwnershipPolicy::CanCreateOwnedFallbackForSession(
			serverArenaSession,
			g_ArenaSessionBridge.Session.SessionOrdinal,
			g_ArenaSessionBridge.OwnedFallbackSuppressedSessionOrdinal,
			g_ArenaSessionBridge.NativeHudOwnerSessionOrdinal);
	const bool ownedFallbackNeededNow =
		AtlasArenaHudOwnershipPolicy::ShouldCreateOwnedFallbackNow(
			ownedFallbackAllowed,
			serverArenaSession,
			HasDueArenaBridgeVisualForSession(
				g_ArenaSessionBridge.Session.SessionOrdinal, now) ||
			g_ArenaSessionBridge.OwnedFallbackOpportunityReached);
	if (ownedFallbackNeededNow &&
		g_ArenaScoringHudRepair.Attempts >= directCreateThreshold &&
		!IsExactOwnedArenaScoringAlert(
			g_ArenaScoringHudRepair.OwnedScoringAlert) &&
		now >= g_ArenaScoringHudRepair.NextDirectCreateAt &&
		g_ArenaScoringHudRepair.ArenaScoringHudClass)
	{
		if (g_ArenaScoringHudRepair.DirectCreateAttempts < UINT_MAX)
			++g_ArenaScoringHudRepair.DirectCreateAttempts;
		g_ArenaOwnedFallbackConstructionActive = true;
		UObject* widget = TryCreateArenaScoringHud(
			world, playerController,
			g_ArenaScoringHudRepair.ArenaScoringHudClass);
		g_ArenaOwnedFallbackConstructionActive = false;
		if (widget)
		{
			const auto identity =
				CaptureArenaScoringAlertIdentity(widget);
			if (!IsUsableArenaScoringAlertIdentity(identity))
			{
				// Do not retain a fallback that cannot later be proven as the
				// exact object ATLAS created.
				RemoveArenaScoringHudFromViewport(widget);
				RejectArenaScoringAlert(widget);
			}
			else
			{
				g_ArenaScoringHudRepair.OwnedScoringAlert = widget;
				g_ArenaScoringHudRepair.OwnedScoringAlertIdentity = identity;
				g_ArenaOwnedScoringAlertForObserver.store(
					widget, std::memory_order_release);
				g_ArenaScoringHudRepair.ScoringAlert = widget;
				g_ArenaLiveScoringAlert.store(
					widget, std::memory_order_release);
				g_ArenaScoringHudRepair.DuplicateAuditRequested = true;
				g_ArenaScoringHudRepair.NextObjectScanAt = 0;
				g_ArenaSessionBridge.OwnedFallbackOpportunityReached = false;
				AtlasDiagnostics::WriteLine(
					"arena-hud direct-widget-fallback-created "
					"widget=%p attempt=%u",
					widget, g_ArenaScoringHudRepair.Attempts);
				return;
			}
		}
		const unsigned backoffStep = (std::min)(
			g_ArenaScoringHudRepair.DirectCreateAttempts, 5u);
		g_ArenaScoringHudRepair.NextDirectCreateAt = now +
			(std::min)(5000ULL, 250ULL << backoffStep);
		const unsigned createAttempt =
			g_ArenaScoringHudRepair.DirectCreateAttempts;
		if (createAttempt <= 8 || (createAttempt % 16) == 0)
		{
			AtlasDiagnostics::WriteLine(
				"arena-hud direct-widget-fallback-failed attempt=%u "
				"create-attempt=%u retry-ms=%llu",
				g_ArenaScoringHudRepair.Attempts,
				createAttempt,
				static_cast<unsigned long long>(
					g_ArenaScoringHudRepair.NextDirectCreateAt - now));
		}
	}

	if (g_ArenaScoringHudRepair.Attempts >= 16 &&
		!g_ArenaScoringHudRepair.ExhaustionLogged)
	{
		g_ArenaScoringHudRepair.ExhaustionLogged = true;
		AtlasDiagnostics::WriteLine(
			"arena-hud entering-slow-retry owner=%p class=%p",
			owner,
			g_ArenaScoringHudRepair.ArenaScoringHudClass);
	}
}

static void CurrentPlaylistInfoOnRepHook(
	UObject* context, FFrame& stack, void* result)
{
	// FN 10.x constructs playlist UI extensions during this one-shot OnRep.
	// Normalize first so the original sees exactly one ArenaScoringHUD source;
	// the post-pass still repairs any hotfix rewrite made inside the original.
	const bool changedBefore = NormalizeActiveCompetitivePlaylist(
		context, "onrep-current-playlist-info-pre");
	if (const ArenaVisualTraceNative original =
		g_CurrentPlaylistInfoOnRepOriginal.load(
			std::memory_order_acquire))
	{
		original(context, stack, result);
	}
	RequestArenaScoringHudRefresh();
	const bool changedAfter = NormalizeActiveCompetitivePlaylist(
		context, "onrep-current-playlist-info-post");
	static std::atomic_uint32_t logged = 0;
	if (logged.fetch_add(1, std::memory_order_relaxed) < 32)
	{
		AtlasDiagnostics::WriteLine(
			"competitive-hud onrep-complete "
			"property=CurrentPlaylistInfo game-state=%p "
			"changed-before=%d changed-after=%d",
			context,
			changedBefore ? 1 : 0,
			changedAfter ? 1 : 0);
	}
}

static void CurrentPlaylistDataOnRepHook(
	UObject* context, FFrame& stack, void* result)
{
	const bool changedBefore = NormalizeActiveCompetitivePlaylist(
		context, "onrep-current-playlist-data-pre");
	if (const ArenaVisualTraceNative original =
		g_CurrentPlaylistDataOnRepOriginal.load(
			std::memory_order_acquire))
	{
		original(context, stack, result);
	}
	RequestArenaScoringHudRefresh();
	const bool changedAfter = NormalizeActiveCompetitivePlaylist(
		context, "onrep-current-playlist-data-post");
	static std::atomic_uint32_t logged = 0;
	if (logged.fetch_add(1, std::memory_order_relaxed) < 32)
	{
		AtlasDiagnostics::WriteLine(
			"competitive-hud onrep-complete "
			"property=CurrentPlaylistData game-state=%p "
			"changed-before=%d changed-after=%d",
			context,
			changedBefore ? 1 : 0,
			changedAfter ? 1 : 0);
	}
}

static bool TryInstallPlaylistOnRepHook(
	UFunction* function,
	ArenaVisualTraceNative detour,
	std::atomic<ArenaVisualTraceNative>& originalStorage,
	std::atomic_bool& installedStorage,
	const char* label)
{
	if (installedStorage.load(std::memory_order_acquire))
		return true;
	if (!function || !detour || !label ||
		!MatchesArenaVisualTraceSchema(
			function, EArenaVisualTraceSchema::ZeroParameters))
	{
		return false;
	}

	__try
	{
		void*& nativeFunction = function->GetNativeFunc();
		auto nativeSlot =
			reinterpret_cast<PVOID volatile*>(&nativeFunction);
		void* observed = InterlockedCompareExchangePointer(
			nativeSlot, nullptr, nullptr);
		if (!observed || observed ==
			reinterpret_cast<void*>(detour))
		{
			return false;
		}

		originalStorage.store(
			reinterpret_cast<ArenaVisualTraceNative>(observed),
			std::memory_order_release);
		void* previous = InterlockedCompareExchangePointer(
			nativeSlot, reinterpret_cast<void*>(detour), observed);
		if (previous != observed)
		{
			originalStorage.store(nullptr,
				std::memory_order_release);
			return false;
		}

		installedStorage.store(true, std::memory_order_release);
		AtlasDiagnostics::WriteLine(
			"competitive-hud onrep-hook-installed target=%s "
			"function=%p original=%p",
			label, function, observed);
		return true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		originalStorage.store(nullptr, std::memory_order_release);
		return false;
	}
}

static void MaintainPlaylistOnRepHooks(ULONGLONG now)
{
	if (now < g_NextPlaylistOnRepResolveAt)
		return;
	g_NextPlaylistOnRepResolveAt = now + 1000;

	if (!g_CurrentPlaylistInfoOnRepInstalled.load(
			std::memory_order_acquire))
	{
		TryInstallPlaylistOnRepHook(
			FindArenaVisualTraceFunction(
				L"/Script/FortniteGame.FortGameStateAthena.OnRep_CurrentPlaylistInfo"),
			CurrentPlaylistInfoOnRepHook,
			g_CurrentPlaylistInfoOnRepOriginal,
			g_CurrentPlaylistInfoOnRepInstalled,
			"CurrentPlaylistInfo");
	}

	if (!g_CurrentPlaylistDataOnRepInstalled.load(
			std::memory_order_acquire))
	{
		TryInstallPlaylistOnRepHook(
			FindArenaVisualTraceFunction(
				L"/Script/FortniteGame.FortGameStateAthena.OnRep_CurrentPlaylistData"),
			CurrentPlaylistDataOnRepHook,
			g_CurrentPlaylistDataOnRepOriginal,
			g_CurrentPlaylistDataOnRepInstalled,
			"CurrentPlaylistData");
	}
}

static void MaintainCompetitivePlaylistExtensions(UWorld* World)
{
	using namespace AtlasPlaylistHudPatchPolicy;

	if (!World || !SupportsUiExtensionPatch(
			VersionInfo.FortniteVersion))
	{
		return;
	}

	const ULONGLONG Now = GetTickCount64();
	if (World != g_CompetitivePlaylistPatchWorld)
	{
		g_CompetitivePlaylistPatchWorld = World;
		g_CompetitivePlaylistObjects.fill(nullptr);
		g_CompetitivePlaylistProbeIndex = 0;
		g_NextCompetitivePlaylistProbeAt = 0;
		g_NextActivePlaylistMaintenanceAt = 0;
		AtlasDiagnostics::WriteLine(
			"competitive-hud world-changed world=%p",
			World);
	}

	if (!g_ClientPlaylistClass)
	{
		g_ClientPlaylistClass = FindClass("FortPlaylistAthena");
		if (!g_ClientPlaylistClass)
		{
			g_NextCompetitivePlaylistProbeAt = Now + 1000;
			return;
		}
	}

	InitializePlaylistExtensions();
	if (!g_PlaylistExtensionsInitialized)
		return;

	MaintainPlaylistOnRepHooks(Now);
	if (Now >= g_NextActivePlaylistMaintenanceAt)
	{
		g_NextActivePlaylistMaintenanceAt = Now + 250;
		__try
		{
			NormalizeActiveCompetitivePlaylist(
				World->GameState, "active-playlist-maintenance");
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
		}
	}

	if (Now < g_NextCompetitivePlaylistProbeAt)
		return;

	// Probe two canonical paths per game-thread pass. A complete pass takes
	// at most six frames, while absent playlists stay cheap hash lookups. This
	// catches playlists that modern clients load lazily after the original
	// one-time TUObjectArray scan has already completed.
	constexpr std::size_t PathsPerTick = 2;
	for (std::size_t Count = 0; Count < PathsPerTick; ++Count)
	{
		const std::size_t Index =
			g_CompetitivePlaylistProbeIndex;
		const auto& Candidate = CompetitivePlaylists[Index];
		const UObject* Object =
			g_CompetitivePlaylistObjects[Index];
		if (!IsLiveUObject(Object) ||
			!Object->IsA(g_ClientPlaylistClass))
		{
			Object = FindLoadedCompetitivePlaylist(
				Candidate.ObjectPath);
			g_CompetitivePlaylistObjects[Index] = Object;
		}

		if (IsLiveUObject(Object) &&
			Object->IsA(g_ClientPlaylistClass))
		{
			ApplyClientPlaylistConfiguration(Object);
		}

		g_CompetitivePlaylistProbeIndex =
			(Index + 1) % CompetitivePlaylists.size();
	}

	// Revalidate all known paths once a second. This also repairs a playlist
	// if a late hotfix replaces its local UIExtensions array after travel.
	g_NextCompetitivePlaylistProbeAt =
		g_CompetitivePlaylistProbeIndex == 0
			? Now + 1000
			: Now;
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
				// Playlist defaults live for the process lifetime. Keep this
				// one-time compatibility scan far below a frame-sized burst;
				// FN30 can have more than half a million UObjects here.
				constexpr int kPlaylistObjectsPerTick = 128;
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

static AActor* FindDurianSpecialEventScript()
{
	static const UClass* scriptClass = nullptr;
	if (!IsLiveUObject(scriptClass))
	{
		scriptClass = static_cast<const UClass*>(StaticFindObject(
			L"/Durian/Gameplay/BP_Durian_SpecialEventScript.BP_Durian_SpecialEventScript_C",
			UClass::StaticClass()));
	}
	if (!IsLiveUObject(scriptClass))
		return nullptr;

	TArray<AActor*> scripts;
	Utils::GetAll(scriptClass, scripts);
	AActor* result = nullptr;
	for (int32 index = 0; index < scripts.Num(); ++index)
	{
		AActor* script = scripts[index];
		if (IsLiveUObject(script) &&
			IsDurianSpecialEventScript(script))
		{
			result = script;
			break;
		}
	}
	scripts.Free();
	return result;
}

static bool InvokeDurianPhaseOnRep(
	AActor* script,
	uint32 phaseOffset,
	UFunction* onRep,
	int32 phaseIndex,
	int32* oldPhaseIndex)
{
	__try
	{
		auto phaseAddress = reinterpret_cast<int32*>(
			reinterpret_cast<uint8*>(script) + phaseOffset);
		*oldPhaseIndex = *phaseAddress;
		*phaseAddress = phaseIndex;
		g_DurianSuppressedPhaseActorIndex.store(
			phaseIndex, std::memory_order_release);
		g_DurianSuppressPhaseActorActivation = true;
		script->ProcessEvent(onRep, nullptr);
		g_DurianSuppressPhaseActorActivation = false;
		return true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		g_DurianSuppressPhaseActorActivation = false;
		int32 suppressedPhase = phaseIndex;
		g_DurianSuppressedPhaseActorIndex.compare_exchange_strong(
			suppressedPhase, -1, std::memory_order_acq_rel);
		return false;
	}
}

static void ResetDurianPhaseSyncForWorld(UWorld* world)
{
	g_DurianPhaseSyncWorld = world;
	g_DurianObservedScriptPhaseIndex.store(
		-1, std::memory_order_release);
	g_DurianSuppressedPhaseActorIndex.store(
		-1, std::memory_order_release);
	g_DurianLastAppliedPhaseIndex = -1;

	if (g_DurianPendingPhaseWorld.load(
			std::memory_order_acquire) != world)
	{
		g_DurianPendingPhaseIndex.store(
			-1, std::memory_order_release);
		g_DurianPendingPhaseWorld.store(
			nullptr, std::memory_order_release);
	}
}

static void ProcessDurianPhaseSync(UWorld* world)
{
	if (!g_DurianPhaseSyncInstalled.load(
			std::memory_order_acquire) || !world)
	{
		return;
	}

	if (world != g_DurianPhaseSyncWorld)
		ResetDurianPhaseSyncForWorld(world);

	const int32 phaseIndex = g_DurianPendingPhaseIndex.load(
		std::memory_order_acquire);
	if (phaseIndex < 0)
		return;

	if (g_DurianPendingPhaseWorld.load(
			std::memory_order_acquire) != world)
	{
		return;
	}

	if (phaseIndex == g_DurianLastAppliedPhaseIndex ||
		phaseIndex == g_DurianObservedScriptPhaseIndex.load(
			std::memory_order_acquire))
	{
		int32 expectedPhase = phaseIndex;
		g_DurianPendingPhaseIndex.compare_exchange_strong(
			expectedPhase, -1, std::memory_order_acq_rel);
		return;
	}

	AActor* script = FindDurianSpecialEventScript();
	if (!script)
		return;

	const uint32 phaseOffset = script->GetOffset(
		"ReplicatedActivePhaseIndex");
	UFunction* onRep = script->GetFunction(
		"OnRep_ReplicatedActivePhaseIndex");
	if (phaseOffset == UINT32_MAX || !IsLiveUObject(onRep))
		return;

	int32 oldPhaseIndex = -1;
	if (!InvokeDurianPhaseOnRep(
			script, phaseOffset, onRep,
			phaseIndex, &oldPhaseIndex))
	{
		AtlasDiagnostics::WriteLine(
			"durian-phase-sync apply-failed phase=%d script=%p",
			phaseIndex, script);
		return;
	}

	g_DurianObservedScriptPhaseIndex.store(
		phaseIndex, std::memory_order_release);
	g_DurianLastAppliedPhaseIndex = phaseIndex;
	int32 expectedPhase = phaseIndex;
	g_DurianPendingPhaseIndex.compare_exchange_strong(
		expectedPhase, -1, std::memory_order_acq_rel);
	AtlasDiagnostics::WriteLine(
		"durian-phase-sync applied phase=%d old=%d script=%p",
		phaseIndex, oldPhaseIndex, script);
}

static UWorld* ClientGameThreadTick()
{
	InitializeClientUObjects();

	static UWorld* observedSessionWorld = nullptr;
	static AFortPlayerControllerAthena*
		observedSessionController = nullptr;
	static AFortPlayerControllerAthena* observedController = nullptr;
	static UFortCheatManager* observedManager = nullptr;
	static ULONGLONG nextCaptureResolveAt = 0;

	auto world = UWorld::GetWorld();
	const bool supportsArenaRuntime = SupportsArenaRuntime();
	if (supportsArenaRuntime)
	{
		MaintainPendingArenaOwnedScoringAlertRemovals(GetTickCount64());
		// Reset source ownership before touching a new world's playlist. Otherwise
		// a verified HUD-owner source from the previous world can strip the only
		// authored playlist extension during the first maintenance pass.
		if (world != g_ArenaScoringHudRepair.World)
		{
			ResetArenaScoringHudRepair(world);
			AtlasDiagnostics::WriteLine(
				"arena-hud world-changed world=%p", world);
		}
	}
	MaintainCompetitivePlaylistExtensions(world);
	auto playerController = GetLocalFortPlayerController();
	AtlasJoinRequestPolicy::ENetDriverState joinNetDriverState =
		AtlasJoinRequestPolicy::ENetDriverState::Unknown;
	const bool frontendActive = ResolveJoinFrontendActive(
		world, playerController, &joinNetDriverState);
	const bool previousFrontendActive = g_FrontendActive.exchange(
		frontendActive, std::memory_order_acq_rel);
	if (frontendActive != previousFrontendActive)
	{
		AtlasDiagnostics::WriteLine(
			"join-hotkey frontend-active=%d canonical-name=%d "
			"net-driver=%u world=%p pc=%p",
			frontendActive ? 1 : 0,
			IsFrontendWorld(world) ? 1 : 0,
			static_cast<unsigned>(joinNetDriverState),
			world, playerController);
	}
	if (supportsArenaRuntime)
	{
		MaintainArenaNativeVisualCorrelationScope(
			world, playerController);
	}
	if (world != observedSessionWorld)
	{
		observedSessionWorld = world;
		observedSessionController = nullptr;
		g_ServerCommandListReceived.store(
			false, std::memory_order_release);
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

	ProcessDurianPhaseSync(world);

	const ULONGLONG now = GetTickCount64();
	MaintainFirstPersonRuntimeState(
		world, playerController, frontendActive, now);
	if (supportsArenaRuntime)
	{
		MaintainArenaConcreteVisualTracePreHud();
		MaintainArenaScoringHud(world, playerController, now);
		// Resolve and root the tiny authored badge textures while the Arena HUD is
		// idle. If an authoritative visual is already queued, skip loading so its
		// delivery is never held behind synchronous asset work.
		MaintainArenaDirectBadgeIconPreload();
		// Retain the post-maintenance pass so legacy builds that lack a discoverable
		// concrete CDO can still supply functions from the exact live widget.
		MaintainArenaVisualTrace(playerController, now);
		MaintainPendingArenaVisualFallbacks(now);
		MaintainPendingArenaBridgeVisuals(now);
		MaintainArenaSessionBridgeScore(world, now);
	}
	if (playerController &&
		!g_ClientMessageCaptureInstalled.load(std::memory_order_acquire) &&
		now >= nextCaptureResolveAt)
	{
		TryInstallClientMessageCapture(playerController);
		nextCaptureResolveAt = now + 1000;
	}

	AtlasLoadoutTelemetry::GameThreadTick(
		world,
		playerController,
		g_ClientMessageCaptureInstalled.load(std::memory_order_acquire));

	if (playerController)
	{
		ApplyLegacySprintByDefault(playerController);

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

		if (supportsArenaRuntime)
		{
			MaintainFrontendTournamentRefresh(
				world, playerController, cheatManager, now);
		}
		else
		{
			g_FrontendTournamentRefresh = {};
		}
	}
	else if (observedController)
	{
		AtlasDiagnostics::WriteLine("controller-unavailable previous-pc=%p manager=%p", observedController, observedManager);
		observedController = nullptr;
		observedManager = nullptr;
		g_FrontendTournamentRefresh = {};
	}

	return world;
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
	static std::atomic_bool loggedThreadMigration = false;
	static ULONGLONG nextClientTickAt = 0;

	const DWORD currentThreadId = GetCurrentThreadId();
	DWORD expectedThreadId = 0;
	if (g_ClientGameThreadId.compare_exchange_strong(
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
		UWorld* world = ClientGameThreadTick();
		AtlasHotfixReload::GameThreadTick(world);
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

static bool InstallDurianPhaseSyncHook()
{
	if (fabs(VersionInfo.FortniteVersion - 27.11) >= 0.001)
		return true;

	if (g_DurianPhaseSyncInstalled.load(
			std::memory_order_acquire))
	{
		return true;
	}

	const MH_STATUS initializeStatus = MH_Initialize();
	if (initializeStatus != MH_OK &&
		initializeStatus != MH_ERROR_ALREADY_INITIALIZED)
	{
		AtlasDiagnostics::WriteLine(
			"durian-phase-sync install-failed initialize-status=%d",
			static_cast<int>(initializeStatus));
		return false;
	}

	const uint64 scriptTarget = FindActivatePhase();
	if (IsExecutableHookTarget(scriptTarget))
	{
		const MH_STATUS createStatus = MH_CreateHook(
			reinterpret_cast<LPVOID>(scriptTarget),
			reinterpret_cast<LPVOID>(HookedDurianScriptActivate),
			reinterpret_cast<LPVOID*>(
				&g_DurianScriptActivateOriginal));
		if (createStatus == MH_OK &&
			g_DurianScriptActivateOriginal)
		{
			const MH_STATUS enableStatus = MH_EnableHook(
				reinterpret_cast<LPVOID>(scriptTarget));
			if (enableStatus != MH_OK &&
				enableStatus != MH_ERROR_ENABLED)
			{
				MH_RemoveHook(reinterpret_cast<LPVOID>(scriptTarget));
				g_DurianScriptActivateOriginal = nullptr;
				AtlasDiagnostics::WriteLine(
					"durian-script-observer install-failed "
					"enable-status=%d target=%p",
					static_cast<int>(enableStatus),
					reinterpret_cast<void*>(scriptTarget));
			}
			else
			{
				AtlasDiagnostics::WriteLine(
					"durian-script-observer installed target=%p",
					reinterpret_cast<void*>(scriptTarget));
			}
		}
		else
		{
			g_DurianScriptActivateOriginal = nullptr;
			AtlasDiagnostics::WriteLine(
				"durian-script-observer install-failed "
				"create-status=%d target=%p",
				static_cast<int>(createStatus),
				reinterpret_cast<void*>(scriptTarget));
		}
	}
	else
	{
		AtlasDiagnostics::WriteLine(
			"durian-script-observer unavailable target=%p",
			reinterpret_cast<void*>(scriptTarget));
	}

	const uint64 phaseTarget = FindSpecialEventPhaseActivate();
	if (!IsExecutableHookTarget(phaseTarget))
	{
		AtlasDiagnostics::WriteLine(
			"durian-phase-sync install-failed invalid-target=%p",
			reinterpret_cast<void*>(phaseTarget));
		return false;
	}

	const MH_STATUS createStatus = MH_CreateHook(
		reinterpret_cast<LPVOID>(phaseTarget),
		reinterpret_cast<LPVOID>(HookedDurianPhaseActivate),
		reinterpret_cast<LPVOID*>(&g_DurianPhaseActivateOriginal));
	if (createStatus != MH_OK || !g_DurianPhaseActivateOriginal)
	{
		g_DurianPhaseActivateOriginal = nullptr;
		AtlasDiagnostics::WriteLine(
			"durian-phase-sync install-failed "
			"create-status=%d target=%p",
			static_cast<int>(createStatus),
			reinterpret_cast<void*>(phaseTarget));
		return false;
	}

	const MH_STATUS enableStatus = MH_EnableHook(
		reinterpret_cast<LPVOID>(phaseTarget));
	if (enableStatus != MH_OK &&
		enableStatus != MH_ERROR_ENABLED)
	{
		MH_RemoveHook(reinterpret_cast<LPVOID>(phaseTarget));
		g_DurianPhaseActivateOriginal = nullptr;
		AtlasDiagnostics::WriteLine(
			"durian-phase-sync install-failed "
			"enable-status=%d target=%p",
			static_cast<int>(enableStatus),
			reinterpret_cast<void*>(phaseTarget));
		return false;
	}

	g_DurianPhaseSyncInstalled.store(
		true, std::memory_order_release);
	AtlasDiagnostics::WriteLine(
		"durian-phase-sync installed target=%p",
		reinterpret_cast<void*>(phaseTarget));
	return true;
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
	g_FrontendActive.store(false, std::memory_order_release);

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
	AtlasLoadoutTelemetry::SetEnabled(interactiveClient);
	if (interactiveClient)
	{
		// Fail closed on an unsupported client build. Running UObject
		// initialization or cheat-manager maintenance from a Win32 worker
		// recreates the world-travel/GC race this dispatcher removes.
		if (InstallGameThreadPump())
			InstallDurianPhaseSyncHook();
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

bool Client::IsFrontendActive()
{
	return g_FrontendActive.load(std::memory_order_acquire);
}
