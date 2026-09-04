#include "pch.h"

#include "../Public/HotfixReload.h"
#include "../Public/AssetHotfixBaselineSampler.h"
#include "../Public/AssetHotfixRevisionPlanner.h"
#include "../Public/Diagnostics.h"

#include <atomic>
#include <bit>
#include <charconv>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <unordered_set>

namespace AtlasHotfixReload
{
namespace
{
	class FExclusiveSrwLockGuard final
	{
	public:
		explicit FExclusiveSrwLockGuard(SRWLOCK& lock) noexcept
			: Lock(&lock)
		{
			AcquireSRWLockExclusive(Lock);
		}

		~FExclusiveSrwLockGuard() noexcept
		{
			ReleaseSRWLockExclusive(Lock);
		}

		FExclusiveSrwLockGuard(const FExclusiveSrwLockGuard&) = delete;
		FExclusiveSrwLockGuard& operator=(
			const FExclusiveSrwLockGuard&) = delete;

	private:
		PSRWLOCK Lock = nullptr;
	};

	enum class EManagerScanKind : uint8
	{
		None,
		Full,
		Appended
	};

	enum class EFStringPayloadReadVerdict : uint8
	{
		Invalid,
		Oversize,
		Success
	};

	enum class EAssetPatchDecision : uint8
	{
		None,
		OutsideAddedRefresh,
		FirstObservedPayload,
		ManagerTransition,
		PayloadChanged,
		PayloadUnchanged,
		AcceptedPayloadRecovery,
		NativeUnchanged,
		LateNativeUnchanged,
		DefaultGameUntracked,
		LateDefaultGameUntracked,
		CorrelationRace
	};

	using FHotfixIniFile = bool(__fastcall*)(
		void* manager,
		const FString* fileName,
		const FString* iniData);
	using FPatchAssetsFromIniFiles = void(__fastcall*)(void* manager);

	constexpr ULONGLONG kDiscoveryDelayMs = 5000;
	constexpr ULONGLONG kStartupGraceMs = 8000;
	constexpr ULONGLONG kRefreshIntervalMs = 1000;
	constexpr ULONGLONG kNoCallbackSettleMs = 3000;
	constexpr ULONGLONG kLateCallbackGuardMs = 250;
	constexpr ULONGLONG kAssetPatchPromotionWindowMs = 1000;
	constexpr ULONGLONG kClassCycleRetryMs = 15000;
	constexpr ULONGLONG kManagerScanRetryMs = 3000;
	constexpr ULONGLONG kManagerScanMaxRetryMs = 30000;
	constexpr ULONGLONG kManagerObjectGrowthCheckMs = 500;
	constexpr ULONGLONG kInvocationRetryMs = 5000;
	constexpr ULONGLONG kWorldTransitionDelayMs = 1000;
	constexpr ULONGLONG kUnavailableLogIntervalMs = 60000;
	constexpr DWORD kMaximumDefaultGameBytes = 8 * 1024 * 1024;
	constexpr int kClassObjectsPerTick = 1024;
	constexpr int kManagerObjectsPerTick = 512;
	constexpr size_t kMaximumCurveBaselineSamplesPerRevision = 64;
	constexpr size_t kMaximumCurveBaselineSamplesWithoutClock = 4;
	constexpr LONGLONG kCurveBaselineSamplingBudgetMs = 8;
	constexpr int32 kDestroyedObjectFlags = 0x01800000;
	constexpr unsigned kMaximumInvocationFaults = 3;

	constexpr const char* kManagerClassDiagnosticNames[] =
	{
		"OnlineHotfixManager",
		"FortOnlineHotfixManager",
		"FortHotfixManager"
	};
	constexpr const wchar_t* kManagerClassNames[] =
	{
		L"OnlineHotfixManager",
		L"FortOnlineHotfixManager",
		L"FortHotfixManager"
	};
	static_assert(
		_countof(kManagerClassDiagnosticNames) ==
		_countof(kManagerClassNames));
	static_assert(kClassObjectsPerTick > 0 &&
		kClassObjectsPerTick <= 1024);
	static_assert(kManagerObjectsPerTick > 0 &&
		kManagerObjectsPerTick <= 1024);

	struct FAcceptedPayloadState
	{
		bool Known = false;
		uint64 ManagerIdentity = 0;
		uint64 ManagerGeneration = 0;
		uint64 WorldGeneration = 0;
		uint64 Serial = 0;
		std::string Bytes;
		std::string FileName;
		FHotfixIniFile IniOriginal = nullptr;
	};

	struct FAcceptedPayloadSnapshot
	{
		uint64 ManagerIdentity = 0;
		uint64 ManagerGeneration = 0;
		uint64 WorldGeneration = 0;
		uint64 Serial = 0;
		std::string Bytes;
		std::string FileName;
		FHotfixIniFile IniOriginal = nullptr;
	};

	enum class ESyntheticPatchPhase : uint8
	{
		None,
		Delta,
		Restore,
		DirectNative
	};

	struct FExpectedExactPatchState
	{
		bool Active = false;
		void* Manager = nullptr;
		unsigned SuppressedPasses = 0;
	};

	struct FSyntheticPatchState
	{
		ESyntheticPatchPhase Phase = ESyntheticPatchPhase::None;
		void* Manager = nullptr;
		bool NativePassObserved = false;
		unsigned SuppressedPasses = 0;
	};

	struct FNativeAnchorRoots
	{
		const wchar_t* WideText = nullptr;
		const char* NarrowText = nullptr;
		size_t WideBytes = 0;
		size_t NarrowBytes = 0;
		uint64 WidePrefix = 0;
		uint64 NarrowPrefix = 0;
		std::unordered_set<void*> Roots;
		bool Overflowed = false;
	};

	SRWLOCK GHookInstallMutex = SRWLOCK_INIT;
	SRWLOCK GAcceptedPayloadMutex = SRWLOCK_INIT;
	SRWLOCK GAssetPatchGateMutex = SRWLOCK_INIT;

	std::atomic_bool GHooksInstalled = false;
	std::atomic_bool GPatchAssetsHookInstalled = false;
	std::atomic_bool GIniFileHookValidated = false;
	std::atomic<DWORD> GGameThreadId = 0;
	std::atomic_bool GLoggedThreadMismatch = false;
	std::atomic_uint64_t GCurrentManagerIdentity = 0;
	std::atomic_uint64_t GManagerLifecycleGeneration = 1;
	std::atomic_uint64_t GWorldGeneration = 0;
	std::atomic_uint64_t GIniHookValidatedManagerIdentity = 0;
	std::atomic_uint64_t GIniHookRejectedManagerIdentity = 0;
	std::atomic_uint64_t GRefreshSequence = 0;
	std::atomic_uint64_t GPendingRefresh = 0;
	std::atomic_uint64_t GPendingRefreshManagerIdentity = 0;
	std::atomic_uint64_t GPendingRefreshManagerGeneration = 0;
	std::atomic_uint64_t GPendingRefreshWorldGeneration = 0;
	std::atomic<ULONGLONG> GPendingRefreshDeadline = 0;
	std::atomic_uint64_t GPromotedRefresh = 0;
	std::atomic_uint64_t GPromotedRefreshManagerIdentity = 0;
	std::atomic_uint64_t GPromotedPayloadSerial = 0;
	std::atomic<ULONGLONG> GPromotedDeadline = 0;
	std::atomic_uint64_t GObservedDefaultGameRefresh = 0;
	std::atomic_uint64_t GCompletedObservedDefaultGameRefresh = 0;
	std::atomic_uint32_t GActiveIniDispatches = 0;

	bool GHookInstallAttempted = false;
	bool GBaseIniHookEnabled = false;
	bool GBasePatchAssetsHookEnabled = false;
	bool GLoggedManagerIniHookFailure = false;
	bool GLoggedPeriodicGateUnavailable = false;
	bool GClassNamesInitialized = false;
	std::atomic_bool GInvocationActive = false;
	std::atomic_bool GPlannerTransactionActive = false;
	std::atomic_bool GPlannerForceFull = true;
	bool GLoggedFirstRefresh = false;
	bool GRefreshRecoveryPending = false;
	std::atomic_bool GRefreshAwaitingCompletion = false;
	bool GObservedWorldInitialized = false;
	std::atomic_bool GDisabledForWorld = false;
	ULONGLONG GStartedAt = 0;
	ULONGLONG GNextRefreshAt = 0;
	ULONGLONG GRefreshCompletionDeadlineAt = 0;
	std::atomic_uint64_t GNoCallbackRecycleRefresh = 0;
	std::atomic_uint64_t GNoCallbackRecycleManagerIdentity = 0;
	std::atomic_uint64_t GNoCallbackRecycleManagerGeneration = 0;
	std::atomic_uint64_t GNoCallbackRecycleWorldGeneration = 0;
	ULONGLONG GNextClassResolveAt = 0;
	ULONGLONG GNextManagerScanAt = 0;
	ULONGLONG GNextManagerFullScanAt = 0;
	ULONGLONG GNextManagerGrowthCheckAt = 0;
	ULONGLONG GNextUnavailableLogAt = 0;
	std::atomic_uint64_t GRefreshSerial = 0;
	unsigned GInvocationFaultCount = 0;
	uint64 GInvocationFaultManagerIdentity = 0;
	int GClassScanIndex = -1;
	int GManagerScanIndex = -1;
	int GManagerScanFloorIndex = 0;
	int GManagerScanSnapshotObjectCount = 0;
	int GLastCompletedManagerScanObjectCount = 0;
	unsigned GManagerScanMissCount = 0;
	EManagerScanKind GManagerScanKind = EManagerScanKind::None;
	FName GClassMetaName{};
	FName GManagerClassNames[_countof(kManagerClassNames)]{};
	const UClass* GDiscoveredManagerClasses[
		_countof(kManagerClassNames)]{};
	const UClass* GManagerClass = nullptr;
	UObject* GManager = nullptr;
	UFunction* GStartHotfixProcess = nullptr;
	const UWorld* GObservedWorld = nullptr;
	uint64 GObservedWorldIdentity = 0;
	FAcceptedPayloadState GAcceptedPayload{};

	FHotfixIniFile GBaseIniOriginal = nullptr;
	void* GBaseIniTarget = nullptr;
	FHotfixIniFile GManagerIniOriginal = nullptr;
	void* GManagerIniTarget = nullptr;
	FPatchAssetsFromIniFiles GPatchAssetsOriginal = nullptr;
	void* GPatchAssetsTarget = nullptr;
	const char* GPatchAssetsResolutionDiagnostic = "not-attempted";
	thread_local uint32 GIniDispatchDepth = 0;
	thread_local uint64 GStartHotfixInvocationSequence = 0;
	thread_local FExpectedExactPatchState GExpectedExactPatch{};
	thread_local FSyntheticPatchState GSyntheticPatch{};

	AtlasAssetHotfixRevisionPlanner::FBaselineRestorationMap
		GBaselineRestorations;
	std::set<std::string> GTaintedBaselineIdentities;
	std::set<std::string> GKnownResolvableIdentities;
	std::set<std::string> GKnownUnresolvableIdentities;

	bool GAppliedPayloadIdentityReady = false;
	uint64 GAppliedPayloadManagerIdentity = 0;
	uint64 GAppliedPayloadSerial = 0;
	uint64 GAssetPatchAllowedCount = 0;
	uint64 GAssetPatchSkippedCount = 0;
	EAssetPatchDecision GLastLoggedAssetPatchDecision =
		EAssetPatchDecision::None;
	uint64 GLastLoggedAssetPatchPayloadSerial = 0;

	uint64 AdvanceLifecycleGeneration(
		std::atomic_uint64_t& generation) noexcept
	{
		uint64 next = generation.fetch_add(
			1, std::memory_order_acq_rel) + 1;
		if (!next)
		{
			next = generation.fetch_add(
				1, std::memory_order_acq_rel) + 1;
		}
		return next;
	}

	bool IsLiveObject(const UObject* object) noexcept
	{
		if (!object)
			return false;
		__try
		{
			const int index = object->Index;
			if (index < 0 || index >= TUObjectArray::Num())
				return false;
			const FUObjectItem* item =
				TUObjectArray::GetItemByIndex(index);
			return item && item->Object == object && object->Class &&
				(item->Flags & 0x20) == 0 &&
				(object->ObjectFlags & kDestroyedObjectFlags) == 0;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	uint64 GetObjectIdentity(const UObject* object) noexcept
	{
		if (!IsLiveObject(object))
			return 0;
		__try
		{
			const int index = object->Index;
			const FUObjectItem* item =
				TUObjectArray::GetItemByIndex(index);
			if (!item || item->Object != object)
				return 0;
			return
				(static_cast<uint64>(static_cast<uint32>(index)) << 32) |
				static_cast<uint32>(item->SerialNumber);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return 0;
		}
	}

	bool IsUsableManager(
		const UObject* manager,
		const UClass* managerClass) noexcept
	{
		if (!IsLiveObject(manager) || !IsLiveObject(managerClass))
			return false;
		__try
		{
			return !manager->IsDefaultObject() &&
				manager->IsA(managerClass);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	bool IsCurrentManagerLifecycle(
		uint64 managerIdentity,
		uint64 managerGeneration,
		uint64 worldGeneration) noexcept
	{
		return managerIdentity && managerGeneration && worldGeneration &&
			GCurrentManagerIdentity.load(std::memory_order_acquire) ==
				managerIdentity &&
			GManagerLifecycleGeneration.load(std::memory_order_acquire) ==
				managerGeneration &&
			GWorldGeneration.load(std::memory_order_acquire) ==
				worldGeneration;
	}

	void LowerAsciiInPlace(std::string& value) noexcept
	{
		for (char& character : value)
		{
			if (character >= 'A' && character <= 'Z')
				character = static_cast<char>(character - 'A' + 'a');
		}
	}

	bool ResizeStringNoThrow(std::string& value, size_t size) noexcept
	{
		try
		{
			value.resize(size);
			return true;
		}
		catch (...)
		{
			value.clear();
			return false;
		}
	}

	bool ReadFStringUtf8(
		const FString* value,
		std::string& out,
		size_t maximumCharacters) noexcept
	{
		out.clear();
		if (!value)
			return false;
		__try
		{
			const int32 count = value->Num();
			const int32 capacity = value->Max();
			const wchar_t* data = value->GetData();
			if (!data || count <= 1 || capacity < count ||
				static_cast<size_t>(count - 1) > maximumCharacters ||
				data[count - 1] != L'\0')
			{
				return false;
			}
			for (int32 index = 0; index < count - 1; ++index)
			{
				if (data[index] == L'\0')
					return false;
			}
			const int required = WideCharToMultiByte(
				CP_UTF8, WC_ERR_INVALID_CHARS, data, count - 1,
				nullptr, 0, nullptr, nullptr);
			if (required <= 0 ||
				static_cast<size_t>(required) > maximumCharacters * 4)
			{
				return false;
			}
			if (!ResizeStringNoThrow(
					out, static_cast<size_t>(required)))
			{
				return false;
			}
			return WideCharToMultiByte(
				CP_UTF8, WC_ERR_INVALID_CHARS, data, count - 1,
				out.data(), required, nullptr, nullptr) == required;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			out.clear();
			return false;
		}
	}

	EFStringPayloadReadVerdict ReadHotfixPayloadUtf8(
		const FString* value,
		std::string& out) noexcept
	{
		out.clear();
		if (!value)
			return EFStringPayloadReadVerdict::Invalid;
		__try
		{
			const int32 count = value->Num();
			const int32 capacity = value->Max();
			const wchar_t* data = value->GetData();
			if (!data || count <= 1 || capacity < count ||
				data[count - 1] != L'\0')
			{
				return EFStringPayloadReadVerdict::Invalid;
			}
			if (static_cast<size_t>(count - 1) >
				kMaximumDefaultGameBytes)
			{
				return EFStringPayloadReadVerdict::Oversize;
			}
			for (int32 index = 0; index < count - 1; ++index)
			{
				if (data[index] == L'\0')
					return EFStringPayloadReadVerdict::Invalid;
			}
			const int required = WideCharToMultiByte(
				CP_UTF8, WC_ERR_INVALID_CHARS, data, count - 1,
				nullptr, 0, nullptr, nullptr);
			if (required <= 0)
				return EFStringPayloadReadVerdict::Invalid;
			if (static_cast<size_t>(required) > kMaximumDefaultGameBytes)
				return EFStringPayloadReadVerdict::Oversize;
			if (!ResizeStringNoThrow(
					out, static_cast<size_t>(required)))
			{
				return EFStringPayloadReadVerdict::Invalid;
			}
			return WideCharToMultiByte(
				CP_UTF8, WC_ERR_INVALID_CHARS, data, count - 1,
				out.data(), required, nullptr, nullptr) == required
				? EFStringPayloadReadVerdict::Success
				: EFStringPayloadReadVerdict::Invalid;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			out.clear();
			return EFStringPayloadReadVerdict::Invalid;
		}
	}

	bool IsDefaultGameIni(const FString* fileNameValue) noexcept
	{
		std::string fileName;
		if (!ReadFStringUtf8(fileNameValue, fileName, 4096))
			return false;
		std::replace(fileName.begin(), fileName.end(), '\\', '/');
		LowerAsciiInPlace(fileName);
		const size_t slash = fileName.find_last_of('/');
		if (slash != std::string::npos)
			fileName.erase(0, slash + 1);
		if (fileName == "defaultgame.ini")
			return true;

		// A backend may keep the logical name DefaultGame.ini while using a
		// content-addressed download name. Accept only that explicit final form;
		// opaque atlas-<hash>.ini files are intentionally not guessed or scanned.
		constexpr char prefix[] = "defaultgame.atlas-";
		constexpr char suffix[] = ".ini";
		constexpr size_t digestLength = 64;
		constexpr size_t prefixLength = sizeof(prefix) - 1;
		constexpr size_t suffixLength = sizeof(suffix) - 1;
		if (fileName.size() !=
			prefixLength + digestLength + suffixLength ||
			fileName.compare(0, prefixLength, prefix) != 0 ||
			fileName.compare(
				prefixLength + digestLength,
				suffixLength, suffix) != 0)
		{
			return false;
		}
		for (size_t index = prefixLength;
			index < prefixLength + digestLength; ++index)
		{
			const char character = fileName[index];
			if (!((character >= '0' && character <= '9') ||
				(character >= 'a' && character <= 'f')))
			{
				return false;
			}
		}
		return true;
	}

	bool CommitAcceptedPayload(
		uint64 managerIdentity,
		uint64 managerGeneration,
		uint64 worldGeneration,
		std::string&& bytes,
		std::string&& fileName,
		FHotfixIniFile iniOriginal,
		uint64& outSerial) noexcept
	{
		outSerial = 0;
		if (!managerIdentity || !managerGeneration || !worldGeneration)
			return false;
		FExclusiveSrwLockGuard lock(GAcceptedPayloadMutex);
		if (GAcceptedPayload.Known &&
			GAcceptedPayload.ManagerIdentity == managerIdentity &&
			GAcceptedPayload.ManagerGeneration == managerGeneration &&
			GAcceptedPayload.WorldGeneration == worldGeneration &&
			GAcceptedPayload.Bytes == bytes)
		{
			GAcceptedPayload.FileName = std::move(fileName);
			GAcceptedPayload.IniOriginal = iniOriginal;
			outSerial = GAcceptedPayload.Serial;
			return false;
		}
		uint64 serial = GAcceptedPayload.Serial + 1;
		if (!serial)
			serial = 1;
		GAcceptedPayload.Known = true;
		GAcceptedPayload.ManagerIdentity = managerIdentity;
		GAcceptedPayload.ManagerGeneration = managerGeneration;
		GAcceptedPayload.WorldGeneration = worldGeneration;
		GAcceptedPayload.Serial = serial;
		GAcceptedPayload.Bytes = std::move(bytes);
		GAcceptedPayload.FileName = std::move(fileName);
		GAcceptedPayload.IniOriginal = iniOriginal;
		outSerial = serial;
		return true;
	}

	bool HasAcceptedPayload(
		uint64 managerIdentity,
		uint64 managerGeneration,
		uint64 worldGeneration) noexcept
	{
		if (!managerIdentity || !managerGeneration || !worldGeneration)
			return false;
		FExclusiveSrwLockGuard lock(GAcceptedPayloadMutex);
		return GAcceptedPayload.Known &&
			GAcceptedPayload.ManagerIdentity == managerIdentity &&
			GAcceptedPayload.ManagerGeneration == managerGeneration &&
			GAcceptedPayload.WorldGeneration == worldGeneration &&
			GAcceptedPayload.Serial && GAcceptedPayload.IniOriginal;
	}

	bool ReadAcceptedPayloadSnapshot(
		uint64 managerIdentity,
		uint64 expectedSerial,
		FAcceptedPayloadSnapshot& out) noexcept
	{
		out = {};
		if (!managerIdentity || !expectedSerial)
			return false;
		try
		{
			FExclusiveSrwLockGuard lock(GAcceptedPayloadMutex);
			if (!GAcceptedPayload.Known ||
				GAcceptedPayload.ManagerIdentity != managerIdentity ||
				GAcceptedPayload.Serial != expectedSerial ||
				!GAcceptedPayload.ManagerGeneration ||
				!GAcceptedPayload.WorldGeneration ||
				!GAcceptedPayload.IniOriginal ||
				GAcceptedPayload.FileName.empty())
			{
				return false;
			}
			out.ManagerIdentity = GAcceptedPayload.ManagerIdentity;
			out.ManagerGeneration = GAcceptedPayload.ManagerGeneration;
			out.WorldGeneration = GAcceptedPayload.WorldGeneration;
			out.Serial = GAcceptedPayload.Serial;
			out.Bytes = GAcceptedPayload.Bytes;
			out.FileName = GAcceptedPayload.FileName;
			out.IniOriginal = GAcceptedPayload.IniOriginal;
			return true;
		}
		catch (...)
		{
			out = {};
			return false;
		}
	}

	uint64 ReadAcceptedPayloadSerial(uint64 managerIdentity) noexcept
	{
		if (!managerIdentity)
			return 0;
		const uint64 managerGeneration =
			GManagerLifecycleGeneration.load(std::memory_order_acquire);
		const uint64 worldGeneration =
			GWorldGeneration.load(std::memory_order_acquire);
		FExclusiveSrwLockGuard lock(GAcceptedPayloadMutex);
		return GAcceptedPayload.Known &&
			GAcceptedPayload.ManagerIdentity == managerIdentity &&
			GAcceptedPayload.ManagerGeneration == managerGeneration &&
			GAcceptedPayload.WorldGeneration == worldGeneration
			? GAcceptedPayload.Serial
			: 0;
	}

	uint64 ObserveDefaultGameCallback(uint64 managerIdentity) noexcept
	{
		if (!managerIdentity)
			return 0;

		const uint64 pending =
			GPendingRefresh.load(std::memory_order_acquire);
		if (pending &&
			GRefreshAwaitingCompletion.load(std::memory_order_acquire) &&
			GPendingRefreshManagerIdentity.load(
				std::memory_order_relaxed) == managerIdentity &&
			IsCurrentManagerLifecycle(
				managerIdentity,
				GPendingRefreshManagerGeneration.load(
					std::memory_order_relaxed),
				GPendingRefreshWorldGeneration.load(
					std::memory_order_relaxed)))
		{
			GObservedDefaultGameRefresh.store(
				pending, std::memory_order_release);
			return pending;
		}

		const uint64 recycle =
			GNoCallbackRecycleRefresh.load(std::memory_order_acquire);
		if (recycle &&
			GNoCallbackRecycleManagerIdentity.load(
				std::memory_order_relaxed) == managerIdentity &&
			IsCurrentManagerLifecycle(
				managerIdentity,
				GNoCallbackRecycleManagerGeneration.load(
					std::memory_order_relaxed),
				GNoCallbackRecycleWorldGeneration.load(
					std::memory_order_relaxed)))
		{
			GObservedDefaultGameRefresh.store(
				recycle, std::memory_order_release);
			return recycle;
		}
		return 0;
	}

	void InvalidateAcceptedPayload() noexcept
	{
		FExclusiveSrwLockGuard lock(GAcceptedPayloadMutex);
		GAcceptedPayload = {};
	}

	bool IsBoundGameThread() noexcept
	{
		const DWORD owner = GGameThreadId.load(std::memory_order_acquire);
		return owner && owner == GetCurrentThreadId();
	}

	class FPlannerTransactionGuard final
	{
	public:
		FPlannerTransactionGuard() noexcept
		{
			bool expected = false;
			Acquired = GPlannerTransactionActive.compare_exchange_strong(
				expected, true,
				std::memory_order_acq_rel,
				std::memory_order_acquire);
		}

		~FPlannerTransactionGuard() noexcept
		{
			if (Acquired)
			{
				GPlannerTransactionActive.store(
					false, std::memory_order_release);
			}
		}

		explicit operator bool() const noexcept
		{
			return Acquired;
		}

		FPlannerTransactionGuard(
			const FPlannerTransactionGuard&) = delete;
		FPlannerTransactionGuard& operator=(
			const FPlannerTransactionGuard&) = delete;

	private:
		bool Acquired = false;
	};

	class FScopedExpectedExactPatch final
	{
	public:
		FScopedExpectedExactPatch(void* manager, bool enabled) noexcept
		{
			if (!enabled || !manager || GExpectedExactPatch.Active)
				return;
			GExpectedExactPatch = {};
			GExpectedExactPatch.Active = true;
			GExpectedExactPatch.Manager = manager;
			Active = true;
		}

		~FScopedExpectedExactPatch() noexcept
		{
			Finish();
		}

		unsigned Finish() noexcept
		{
			if (!Active)
				return Suppressed;
			Suppressed = GExpectedExactPatch.SuppressedPasses;
			GExpectedExactPatch = {};
			Active = false;
			return Suppressed;
		}

		FScopedExpectedExactPatch(
			const FScopedExpectedExactPatch&) = delete;
		FScopedExpectedExactPatch& operator=(
			const FScopedExpectedExactPatch&) = delete;

	private:
		bool Active = false;
		unsigned Suppressed = 0;
	};

	class FScopedSyntheticPatch final
	{
	public:
		FScopedSyntheticPatch(
			void* manager,
			ESyntheticPatchPhase phase) noexcept
		{
			if (!manager || phase == ESyntheticPatchPhase::None ||
				GSyntheticPatch.Phase != ESyntheticPatchPhase::None)
			{
				return;
			}
			GSyntheticPatch = {};
			GSyntheticPatch.Manager = manager;
			GSyntheticPatch.Phase = phase;
			Active = true;
		}

		~FScopedSyntheticPatch() noexcept
		{
			Finish();
		}

		bool IsActive() const noexcept
		{
			return Active;
		}

		bool Finish(unsigned& suppressedPasses) noexcept
		{
			if (!Active)
			{
				suppressedPasses = 0;
				return false;
			}
			const bool nativeObserved =
				GSyntheticPatch.NativePassObserved;
			suppressedPasses = GSyntheticPatch.SuppressedPasses;
			GSyntheticPatch = {};
			Active = false;
			return nativeObserved;
		}

		FScopedSyntheticPatch(
			const FScopedSyntheticPatch&) = delete;
		FScopedSyntheticPatch& operator=(
			const FScopedSyntheticPatch&) = delete;

	private:
		void Finish() noexcept
		{
			unsigned ignored = 0;
			Finish(ignored);
		}

		bool Active = false;
	};

	std::string_view TrimAsciiView(std::string_view value) noexcept
	{
		while (!value.empty() &&
			(value.front() == ' ' || value.front() == '\t'))
		{
			value.remove_prefix(1);
		}
		while (!value.empty() &&
			(value.back() == ' ' || value.back() == '\t' ||
			 value.back() == '\r'))
		{
			value.remove_suffix(1);
		}
		return value;
	}

	bool EqualsAsciiInsensitiveView(
		std::string_view left,
		std::string_view right) noexcept
	{
		if (left.size() != right.size())
			return false;
		for (size_t index = 0; index < left.size(); ++index)
		{
			char a = left[index];
			char b = right[index];
			if (a >= 'A' && a <= 'Z')
				a = static_cast<char>(a - 'A' + 'a');
			if (b >= 'A' && b <= 'Z')
				b = static_cast<char>(b - 'A' + 'a');
			if (a != b)
				return false;
		}
		return true;
	}

	bool TryParseSamplingDirective(
		std::string_view exactLine,
		AtlasAssetHotfixBaselineSampler::FParsedRowUpdateDirective& out,
		std::string_view* outValue = nullptr)
		noexcept
	{
		using namespace AtlasAssetHotfixBaselineSampler;
		out = {};
		if (outValue)
			*outValue = {};
		std::string_view line = TrimAsciiView(exactLine);
		constexpr std::string_view curvePrefix = "+CurveTable=";
		constexpr std::string_view dataPrefix = "+DataTable=";
		std::string_view body;
		if (line.size() >= curvePrefix.size() &&
			EqualsAsciiInsensitiveView(
				line.substr(0, curvePrefix.size()), curvePrefix))
		{
			out.Kind = EAssetKind::CurveTable;
			body = line.substr(curvePrefix.size());
		}
		else if (line.size() >= dataPrefix.size() &&
			EqualsAsciiInsensitiveView(
				line.substr(0, dataPrefix.size()), dataPrefix))
		{
			out.Kind = EAssetKind::DataTable;
			body = line.substr(dataPrefix.size());
		}
		else
		{
			return false;
		}

		std::string_view tokens[5]{};
		size_t begin = 0;
		for (size_t token = 0; token < 4; ++token)
		{
			const size_t delimiter = body.find(';', begin);
			if (delimiter == std::string_view::npos)
				return false;
			tokens[token] = TrimAsciiView(
				body.substr(begin, delimiter - begin));
			begin = delimiter + 1;
		}
		if (body.find(';', begin) != std::string_view::npos)
			return false;
		tokens[4] = TrimAsciiView(body.substr(begin));
		if (!EqualsAsciiInsensitiveView(tokens[1], "RowUpdate") ||
			tokens[0].empty() || tokens[2].empty() ||
			tokens[3].empty() || tokens[4].empty())
		{
			return false;
		}
		out.AssetPath = tokens[0];
		out.RowName = tokens[2];
		out.ColumnName = tokens[3];
		if (outValue)
			*outValue = tokens[4];
		return true;
	}

	bool TryParseFiniteFloat(
		std::string_view token,
		float& out) noexcept
	{
		token = TrimAsciiView(token);
		if (token.empty())
			return false;
		const char* begin = token.data();
		const char* end = begin + token.size();
		const auto parsed = std::from_chars(begin, end, out);
		if (parsed.ec != std::errc{} || parsed.ptr != end ||
			!std::isfinite(out))
		{
			return false;
		}
		if (out == 0.0f)
			out = 0.0f;
		return true;
	}

	bool CurveValuesEquivalent(
		std::string_view expected,
		std::string_view observed) noexcept
	{
		float expectedValue = 0.0f;
		float observedValue = 0.0f;
		return TryParseFiniteFloat(expected, expectedValue) &&
			TryParseFiniteFloat(observed, observedValue) &&
			std::bit_cast<uint32>(expectedValue) ==
				std::bit_cast<uint32>(observedValue);
	}

	bool ConvertUtf8ToWide(
		const std::string& input,
		std::wstring& out) noexcept
	{
		out.clear();
		if (input.empty() ||
			input.size() > static_cast<size_t>(
				(std::numeric_limits<int>::max)()))
		{
			return false;
		}
		const int required = MultiByteToWideChar(
			CP_UTF8, MB_ERR_INVALID_CHARS, input.data(),
			static_cast<int>(input.size()), nullptr, 0);
		if (required <= 0)
			return false;
		try
		{
			out.resize(static_cast<size_t>(required));
		}
		catch (...)
		{
			out.clear();
			return false;
		}
		if (MultiByteToWideChar(
				CP_UTF8, MB_ERR_INVALID_CHARS, input.data(),
				static_cast<int>(input.size()), out.data(), required) != required)
		{
			out.clear();
			return false;
		}
		return true;
	}

	bool InvokeSyntheticHotfixIni(
		const FAcceptedPayloadSnapshot& snapshot,
		void* manager,
		const std::string& payload,
		ESyntheticPatchPhase phase,
		bool& nativePassObserved,
		unsigned& suppressedPasses)
	{
		nativePassObserved = false;
		suppressedPasses = 0;
		if (!snapshot.IniOriginal || !manager || payload.empty())
			return false;
		std::wstring wideFileName;
		std::wstring widePayload;
		if (!ConvertUtf8ToWide(snapshot.FileName, wideFileName) ||
			!ConvertUtf8ToWide(payload, widePayload) ||
			wideFileName.size() >= static_cast<size_t>(INT32_MAX) ||
			widePayload.size() >= static_cast<size_t>(INT32_MAX))
		{
			return false;
		}

		FString fileName;
		fileName.Data = wideFileName.data();
		fileName.NumElements = static_cast<int32>(wideFileName.size() + 1);
		fileName.MaxElements = fileName.NumElements;
		FString iniData;
		iniData.Data = widePayload.data();
		iniData.NumElements = static_cast<int32>(widePayload.size() + 1);
		iniData.MaxElements = iniData.NumElements;

		FScopedSyntheticPatch scope(manager, phase);
		if (!scope.IsActive())
			return false;
		const bool accepted = snapshot.IniOriginal(
			manager, &fileName, &iniData);
		nativePassObserved = scope.Finish(suppressedPasses);
		return accepted;
	}

	bool InvokeNativePatchOnce(
		FPatchAssetsFromIniFiles patchOriginal,
		void* manager,
		unsigned& suppressedPasses) noexcept
	{
		suppressedPasses = 0;
		if (!patchOriginal || !manager)
			return false;
		FScopedSyntheticPatch scope(
			manager, ESyntheticPatchPhase::DirectNative);
		if (!scope.IsActive())
			return false;
		patchOriginal(manager);
		unsigned nestedSuppressed = 0;
		scope.Finish(nestedSuppressed);
		suppressedPasses = nestedSuppressed;
		return true;
	}

	struct FBaselineUpdateStage
	{
		std::map<std::string, std::string> Baselines;
		std::set<std::string> Taints;
		std::set<std::string> ResolvableIdentities;
		std::set<std::string> UnresolvableIdentities;
		size_t Candidates = 0;
		size_t Sampled = 0;
		size_t Behavioral = 0;
		size_t Failed = 0;
		size_t AlreadyTrusted = 0;
		size_t AlreadyTainted = 0;
		size_t SkippedOffThread = 0;
		size_t Capped = 0;
		size_t ResolutionProbed = 0;
		size_t ResolutionConfirmed = 0;
		size_t ResolutionRejected = 0;
		size_t ResolutionCapped = 0;
		AtlasAssetHotfixBaselineSampler::ESampleStatus LastFailure =
			AtlasAssetHotfixBaselineSampler::ESampleStatus::Success;
	};

	bool StageBaselineUpdates(
		const AtlasAssetHotfixRevisionPlanner::FRevisionPlan& plan,
		bool forceFull,
		FBaselineUpdateStage& out) noexcept
	{
		using namespace AtlasAssetHotfixBaselineSampler;
		out = {};
		try
		{
			size_t curveSampleAttempts = 0;
			LARGE_INTEGER sampleFrequency{};
			LARGE_INTEGER sampleStarted{};
			const bool sampleClockAvailable =
				QueryPerformanceFrequency(&sampleFrequency) &&
				sampleFrequency.QuadPart > 0 &&
				QueryPerformanceCounter(&sampleStarted);
			bool sampleBudgetExpired = false;
			for (const std::string& line :
				plan.BaselineCaptureCandidates)
			{
				++out.Candidates;
				std::string identity;
				if (!AtlasAssetHotfixRevisionPlanner::
					TryGetDirectiveIdentity(line, identity))
				{
					++out.Failed;
					out.LastFailure = ESampleStatus::InvalidDirective;
					continue;
				}
				if (GBaselineRestorations.contains(identity))
				{
					++out.AlreadyTrusted;
					continue;
				}
				if (GTaintedBaselineIdentities.contains(identity) ||
					out.Taints.contains(identity))
				{
					++out.AlreadyTainted;
					continue;
				}

				// A ForceFull/first-observed pass is not pristine: the DLL may
				// have loaded after startup already mutated these rows. Mark them
				// untrusted without touching reflection.
				if (forceFull)
				{
					out.Taints.emplace(std::move(identity));
					continue;
				}
				if (!IsBoundGameThread())
				{
					out.Taints.emplace(std::move(identity));
					++out.SkippedOffThread;
					continue;
				}

				FParsedRowUpdateDirective parsed{};
				if (!TryParseSamplingDirective(line, parsed))
				{
					out.Taints.emplace(std::move(identity));
					++out.Failed;
					out.LastFailure = ESampleStatus::InvalidDirective;
					continue;
				}

				// DataTable column exports are deliberately disabled: their
				// reflected nested-array types are not proven across every UE
				// layout and each call enumerates a full table. Never trade a
				// rollback baseline for an ABI crash or a frame hitch.
				if (parsed.Kind == EAssetKind::DataTable)
				{
					out.Taints.emplace(std::move(identity));
					++out.Failed;
					out.LastFailure = ESampleStatus::ReflectionUnavailable;
					continue;
				}
				const size_t maximumCurveSamples = sampleClockAvailable
					? kMaximumCurveBaselineSamplesPerRevision
					: kMaximumCurveBaselineSamplesWithoutClock;
				if (curveSampleAttempts >= maximumCurveSamples ||
					sampleBudgetExpired)
				{
					out.Taints.emplace(std::move(identity));
					++out.Capped;
					continue;
				}
				++curveSampleAttempts;

				FSampleResult sampled{};
				if (!TrySampleResidentBaseline(parsed, sampled))
				{
					out.Taints.emplace(std::move(identity));
					++out.Failed;
					out.LastFailure = sampled.Status;
				}
				else
				{
					const std::string restoration(
						sampled.GetRestorationDirective());
					std::string restorationIdentity;
					if (!AtlasAssetHotfixRevisionPlanner::
							TryGetDirectiveIdentity(
								restoration, restorationIdentity) ||
						restorationIdentity != identity)
					{
						out.Taints.emplace(std::move(identity));
						++out.Failed;
						out.LastFailure =
							ESampleStatus::UnsafeRestorationValue;
					}
					else
					{
						out.Baselines.emplace(identity, restoration);
						out.ResolvableIdentities.emplace(identity);
						++out.Sampled;
						if (sampled.Semantics ==
							ERestorationSemantics::BehavioralNonStructural)
						{
							++out.Behavioral;
						}
					}
				}

				if (sampleClockAvailable)
				{
					LARGE_INTEGER sampleNow{};
					if (QueryPerformanceCounter(&sampleNow))
					{
						const LONGLONG elapsed =
							sampleNow.QuadPart - sampleStarted.QuadPart;
						const LONGLONG budgetTicks =
							(sampleFrequency.QuadPart *
								kCurveBaselineSamplingBudgetMs + 999) / 1000;
						sampleBudgetExpired =
							elapsed >= budgetTicks;
					}
				}
			}

			for (const std::string& identity :
				plan.MissingBaselineIdentities)
			{
				out.Taints.emplace(identity);
			}
			return true;
		}
		catch (...)
		{
			out = {};
			return false;
		}
	}

	void StageKnownResolvableUpdates(
		const AtlasAssetHotfixRevisionPlanner::FRevisionPlan& plan,
		FBaselineUpdateStage& out,
		bool probeAllActive,
		bool ignoreCommittedEvidence) noexcept
	{
		using namespace AtlasAssetHotfixBaselineSampler;
		if (!IsBoundGameThread())
			return;
		try
		{
			LARGE_INTEGER sampleFrequency{};
			LARGE_INTEGER sampleStarted{};
			const bool sampleClockAvailable =
				QueryPerformanceFrequency(&sampleFrequency) &&
				sampleFrequency.QuadPart > 0 &&
				QueryPerformanceCounter(&sampleStarted);
			bool sampleBudgetExpired = false;
			size_t sampleAttempts = 0;
			std::set<std::string> attemptedIdentities;

			auto ProbeLine = [&](std::string_view exactLine) -> bool
			{
				FParsedRowUpdateDirective parsed{};
				std::string_view expectedValue;
				if (!TryParseSamplingDirective(
						exactLine, parsed, &expectedValue) ||
					parsed.Kind != EAssetKind::CurveTable)
				{
					return true;
				}

				std::string ownedLine(exactLine);
				std::string identity;
				if (!AtlasAssetHotfixRevisionPlanner::
						TryGetDirectiveIdentity(ownedLine, identity) ||
					out.ResolvableIdentities.contains(identity) ||
					out.UnresolvableIdentities.contains(identity) ||
					(!ignoreCommittedEvidence &&
						(GKnownResolvableIdentities.contains(identity) ||
						 GKnownUnresolvableIdentities.contains(identity))) ||
					!attemptedIdentities.emplace(identity).second)
				{
					return true;
				}

				const size_t maximumSamples = sampleClockAvailable
					? kMaximumCurveBaselineSamplesPerRevision
					: kMaximumCurveBaselineSamplesWithoutClock;
				if (sampleAttempts >= maximumSamples || sampleBudgetExpired)
				{
					++out.ResolutionCapped;
					return false;
				}
				++sampleAttempts;
				++out.ResolutionProbed;

				FSampleResult sampled{};
				bool confirmed = false;
				if (TrySampleResidentBaseline(parsed, sampled))
				{
					const std::string restoration(
						sampled.GetRestorationDirective());
					FParsedRowUpdateDirective observed{};
					std::string_view observedValue;
					std::string observedIdentity;
					confirmed = TryParseSamplingDirective(
							restoration, observed, &observedValue) &&
						AtlasAssetHotfixRevisionPlanner::
							TryGetDirectiveIdentity(
								restoration, observedIdentity) &&
						observedIdentity == identity &&
						CurveValuesEquivalent(
							expectedValue, observedValue);
				}
				if (confirmed)
				{
					out.ResolvableIdentities.emplace(identity);
					++out.ResolutionConfirmed;
				}
				else
				{
					++out.ResolutionRejected;
					// EvaluationFailed proves that the resident row/time cannot be
					// used as a retention no-op. A successful evaluation whose value
					// did not match likewise proves this directive was not applied.
					if (sampled.Status == ESampleStatus::EvaluationFailed ||
						sampled.Status == ESampleStatus::Success)
					{
						out.UnresolvableIdentities.emplace(identity);
					}
				}

				if (sampleClockAvailable)
				{
					LARGE_INTEGER sampleNow{};
					if (!QueryPerformanceCounter(&sampleNow))
					{
						sampleBudgetExpired = true;
					}
					else
					{
						const LONGLONG budgetTicks =
							(sampleFrequency.QuadPart *
								kCurveBaselineSamplingBudgetMs + 999) / 1000;
						sampleBudgetExpired =
							sampleNow.QuadPart - sampleStarted.QuadPart >=
							budgetTicks;
					}
				}
				return true;
			};

			const auto& primaryLines = probeAllActive
				? plan.ActiveDirectives
				: plan.BaselineCaptureCandidates;
			for (const std::string& line : primaryLines)
			{
				if (!ProbeLine(line))
					return;
			}

			std::string_view delta(plan.DeltaIni);
			size_t begin = 0;
			while (begin <= delta.size())
			{
				const size_t newline = delta.find('\n', begin);
				const size_t end = newline == std::string_view::npos
					? delta.size() : newline;
				if (!ProbeLine(delta.substr(begin, end - begin)))
					return;
				if (newline == std::string_view::npos)
					break;
				begin = newline + 1;
			}
		}
		catch (...)
		{
			// Resolution evidence is an optimization only. Preserve the already
			// staged baseline transaction and fail closed on retention preference.
		}
	}

	bool IsAcceptedSnapshotCurrent(
		const FAcceptedPayloadSnapshot& snapshot,
		void* manager) noexcept
	{
		return snapshot.Serial &&
			GetObjectIdentity(reinterpret_cast<UObject*>(manager)) ==
				snapshot.ManagerIdentity &&
			IsCurrentManagerLifecycle(
				snapshot.ManagerIdentity,
				snapshot.ManagerGeneration,
				snapshot.WorldGeneration) &&
			ReadAcceptedPayloadSerial(snapshot.ManagerIdentity) ==
				snapshot.Serial;
	}

	void CommitBaselineUpdates(
		FBaselineUpdateStage& staged,
		bool replaceResolutionEvidence = false) noexcept
	{
		for (auto& [identity, line] : staged.Baselines)
		{
			if (!GTaintedBaselineIdentities.contains(identity))
				GBaselineRestorations.emplace(identity, std::move(line));
		}
		for (const std::string& identity : staged.Taints)
		{
			GBaselineRestorations.erase(identity);
			GTaintedBaselineIdentities.emplace(identity);
		}
		if (replaceResolutionEvidence)
		{
			GKnownResolvableIdentities.clear();
			GKnownUnresolvableIdentities.clear();
		}
		for (const std::string& identity : staged.ResolvableIdentities)
		{
			GKnownUnresolvableIdentities.erase(identity);
			GKnownResolvableIdentities.emplace(identity);
		}
		for (const std::string& identity : staged.UnresolvableIdentities)
		{
			if (!GKnownResolvableIdentities.contains(identity) &&
				!staged.ResolvableIdentities.contains(identity))
			{
				GKnownUnresolvableIdentities.emplace(identity);
			}
		}
	}

	void LogPlannerPlan(
		const AtlasAssetHotfixRevisionPlanner::FRevisionPlan& plan,
		const FBaselineUpdateStage& capture,
		bool forceFull) noexcept
	{
		AtlasDiagnostics::WriteLine(
			"hotfix-reload asset-plan mode=%u force-full=%s "
			"full-replay=%s "
			"changed=%llu retention=%llu restored=%llu missing=%llu "
			"capture-candidates=%llu sampled=%llu behavioral=%llu "
			"failed=%llu already-trusted=%llu already-tainted=%llu "
			"off-thread=%llu capped=%llu resolve-probed=%llu "
			"resolve-confirmed=%llu resolve-rejected=%llu "
			"resolve-capped=%llu "
			"last-sample-status=%u",
			static_cast<unsigned>(plan.Mode),
			forceFull ? "yes" : "no",
			plan.FullReplayRecovery ? "yes" : "no",
			static_cast<unsigned long long>(plan.AddedOrChanged),
			static_cast<unsigned long long>(plan.RetentionReapplied),
			static_cast<unsigned long long>(plan.Restored),
			static_cast<unsigned long long>(plan.MissingBaselines),
			static_cast<unsigned long long>(capture.Candidates),
			static_cast<unsigned long long>(capture.Sampled),
			static_cast<unsigned long long>(capture.Behavioral),
			static_cast<unsigned long long>(capture.Failed),
			static_cast<unsigned long long>(capture.AlreadyTrusted),
			static_cast<unsigned long long>(capture.AlreadyTainted),
			static_cast<unsigned long long>(capture.SkippedOffThread),
			static_cast<unsigned long long>(capture.Capped),
			static_cast<unsigned long long>(capture.ResolutionProbed),
			static_cast<unsigned long long>(capture.ResolutionConfirmed),
			static_cast<unsigned long long>(capture.ResolutionRejected),
			static_cast<unsigned long long>(capture.ResolutionCapped),
			static_cast<unsigned>(capture.LastFailure));
	}

	bool TryRestoreAcceptedAssetConfig(
		const FAcceptedPayloadSnapshot& snapshot,
		void* manager,
		const std::string& restoreIni,
		unsigned& suppressedPasses) noexcept
	{
		suppressedPasses = 0;
		bool ignoredNested = false;
		unsigned firstSuppressed = 0;
		if (InvokeSyntheticHotfixIni(
				snapshot, manager, restoreIni,
				ESyntheticPatchPhase::Restore,
				ignoredNested, firstSuppressed))
		{
			suppressedPasses = firstSuppressed;
			return true;
		}

		unsigned exactSuppressed = 0;
		if (!InvokeSyntheticHotfixIni(
				snapshot, manager, snapshot.Bytes,
				ESyntheticPatchPhase::Restore,
				ignoredNested, exactSuppressed))
		{
			suppressedPasses = firstSuppressed + exactSuppressed;
			return false;
		}

		unsigned retrySuppressed = 0;
		const bool restored = InvokeSyntheticHotfixIni(
			snapshot, manager, restoreIni,
			ESyntheticPatchPhase::Restore,
			ignoredNested, retrySuppressed);
		suppressedPasses =
			firstSuppressed + exactSuppressed + retrySuppressed;
		return restored;
	}

	bool TryApplyAcceptedRevision(
		FPatchAssetsFromIniFiles patchOriginal,
		void* manager,
		const FAcceptedPayloadSnapshot& snapshot,
		bool requestedForceFull,
		bool& outCommitted) noexcept
	{
		using namespace AtlasAssetHotfixRevisionPlanner;
		outCommitted = false;
		if (!patchOriginal || !manager)
			return false;
		FPlannerTransactionGuard transaction;
		if (!transaction)
			return false;

		const bool forceFull = requestedForceFull ||
			GPlannerForceFull.load(std::memory_order_acquire);
		FRevisionPlan plan;
		if (!PrepareRevision(
				snapshot.Bytes, GBaselineRestorations,
				GKnownResolvableIdentities,
				GKnownUnresolvableIdentities,
				forceFull, plan))
		{
			GPlannerForceFull.store(true, std::memory_order_release);
			return false;
		}

		FBaselineUpdateStage capture;
		// Every full-native or full-replay plan is an untrusted observation,
		// including the transition from a committed unsafe revision back to a
		// safe one. Never sample a value as "native" immediately before it runs.
		const bool baselineStateUntrusted =
			forceFull || plan.Mode == ERevisionMode::FullNative ||
			plan.FullReplayRecovery;
		if (!StageBaselineUpdates(plan, baselineStateUntrusted, capture) ||
			!IsAcceptedSnapshotCurrent(snapshot, manager))
		{
			const bool preserveRestorationRetry = plan.Restored != 0;
			AbortRevision(plan.Token);
			GPlannerForceFull.store(true, std::memory_order_release);
			return preserveRestorationRetry;
		}
		LogPlannerPlan(plan, capture, forceFull);

		switch (plan.Mode)
		{
		case ERevisionMode::ByteIdentical:
			outCommitted = true;
			GPlannerForceFull.store(false, std::memory_order_release);
			return true;
		case ERevisionMode::NoAssetChange:
			CommitRevision(plan.Token);
			CommitBaselineUpdates(capture);
			outCommitted = true;
			GPlannerForceFull.store(false, std::memory_order_release);
			return true;
		case ERevisionMode::FullNative:
		{
			unsigned suppressedNativePasses = 0;
			if (!InvokeNativePatchOnce(
					patchOriginal, manager, suppressedNativePasses))
			{
				AbortRevision(plan.Token);
				GPlannerForceFull.store(true, std::memory_order_release);
				return false;
			}
			StageKnownResolvableUpdates(plan, capture, true, true);
			if (!IsAcceptedSnapshotCurrent(snapshot, manager))
			{
				AbortRevision(plan.Token);
				GPlannerForceFull.store(true, std::memory_order_release);
				return true;
			}
			CommitRevision(plan.Token);
			CommitBaselineUpdates(capture, true);
			outCommitted = true;
			// A successfully committed full pass establishes the exact bytes that
			// just ran, even if their AssetHotfix syntax is unsupported by the
			// delta parser. Let an identical poll take the byte fast path; the
			// committed Safe bit still forces the next changed revision through one
			// full-native transition.
			GPlannerForceFull.store(false, std::memory_order_release);
			return true;
		}
		case ERevisionMode::DeltaNative:
			break;
		default:
			AbortRevision(plan.Token);
			GPlannerForceFull.store(true, std::memory_order_release);
			return false;
		}

		bool nestedDeltaPass = false;
		unsigned suppressedDeltaPasses = 0;
		if (!InvokeSyntheticHotfixIni(
				snapshot, manager, plan.DeltaIni,
				ESyntheticPatchPhase::Delta,
				nestedDeltaPass, suppressedDeltaPasses))
		{
			unsigned recoverySuppressed = 0;
			const bool configRecovered =
				TryRestoreAcceptedAssetConfig(
					snapshot, manager, plan.RestoreIni,
					recoverySuppressed);
			AbortRevision(plan.Token);
			GPlannerForceFull.store(true, std::memory_order_release);
			AtlasDiagnostics::WriteLine(
				"hotfix-reload asset-plan-aborted "
				"reason=delta-rejected nested=%s recovered=%s "
				"restore-suppressed=%u",
				nestedDeltaPass ? "yes" : "no",
				configRecovered ? "yes" : "no",
				recoverySuppressed);
			// A recovered exact config can be forwarded once through the stock
			// pass. If recovery itself failed, suppress further mutation and let
			// the next accepted revision force a clean full-native transaction.
			return plan.Restored != 0 || !configRecovered;
		}
		if (!nestedDeltaPass)
		{
			unsigned directSuppressedPasses = 0;
			if (!InvokeNativePatchOnce(
					patchOriginal, manager, directSuppressedPasses))
			{
				unsigned recoverySuppressed = 0;
				const bool configRecovered =
					TryRestoreAcceptedAssetConfig(
						snapshot, manager, plan.RestoreIni,
						recoverySuppressed);
				AbortRevision(plan.Token);
				GPlannerForceFull.store(true, std::memory_order_release);
				return plan.Restored != 0 || !configRecovered;
			}
			suppressedDeltaPasses += directSuppressedPasses;
		}
		StageKnownResolvableUpdates(
			plan, capture,
			plan.FullReplayRecovery,
			plan.FullReplayRecovery);
		if (!IsAcceptedSnapshotCurrent(snapshot, manager))
		{
			AbortRevision(plan.Token);
			GPlannerForceFull.store(true, std::memory_order_release);
			return true;
		}

		unsigned suppressedRestorePasses = 0;
		const bool restored = TryRestoreAcceptedAssetConfig(
			snapshot, manager, plan.RestoreIni,
			suppressedRestorePasses);
		if (!restored || !IsAcceptedSnapshotCurrent(snapshot, manager))
		{
			AbortRevision(plan.Token);
			GPlannerForceFull.store(true, std::memory_order_release);
			AtlasDiagnostics::WriteLine(
				"hotfix-reload asset-plan-aborted reason=%s",
				restored ? "revision-race" : "restore-rejected");
			return true;
		}

		CommitRevision(plan.Token);
		CommitBaselineUpdates(capture, plan.FullReplayRecovery);
		outCommitted = true;
		GPlannerForceFull.store(false, std::memory_order_release);
		AtlasDiagnostics::WriteLine(
			"hotfix-reload asset-delta-applied nested=%s "
			"delta-suppressed=%u restore-suppressed=%u",
			nestedDeltaPass ? "yes" : "no",
			suppressedDeltaPasses,
			suppressedRestorePasses);
		return true;
	}

	bool SeedPlannerAfterNativePass(
		void* manager,
		uint64 payloadSerial) noexcept
	{
		using namespace AtlasAssetHotfixRevisionPlanner;
		if (!manager || !payloadSerial)
		{
			GPlannerForceFull.store(true, std::memory_order_release);
			return false;
		}
		FAcceptedPayloadSnapshot snapshot;
		if (!ReadAcceptedPayloadSnapshot(
				GetObjectIdentity(reinterpret_cast<UObject*>(manager)),
				payloadSerial, snapshot))
		{
			GPlannerForceFull.store(true, std::memory_order_release);
			return false;
		}
		FPlannerTransactionGuard transaction;
		if (!transaction)
		{
			GPlannerForceFull.store(true, std::memory_order_release);
			return false;
		}

		FRevisionPlan plan;
		if (!PrepareRevision(
				snapshot.Bytes, GBaselineRestorations,
				GKnownResolvableIdentities,
				GKnownUnresolvableIdentities,
				true, plan))
		{
			GPlannerForceFull.store(true, std::memory_order_release);
			return false;
		}
		// A stock pass cannot perform absent-row restorations. Never let fallback
		// synchronization commit this removal and erase the prior active identity;
		// keep the planner on the old revision so the next owned pass retries the
		// full-replay delta with the same trusted baseline.
		if (plan.Restored != 0)
		{
			AbortRevision(plan.Token);
			GPlannerForceFull.store(true, std::memory_order_release);
			AtlasDiagnostics::WriteLine(
				"hotfix-reload planner-seed-deferred restorations=%llu",
				static_cast<unsigned long long>(plan.Restored));
			return false;
		}
		FBaselineUpdateStage capture;
		if (!StageBaselineUpdates(plan, true, capture) ||
			!IsAcceptedSnapshotCurrent(snapshot, manager))
		{
			AbortRevision(plan.Token);
			GPlannerForceFull.store(true, std::memory_order_release);
			return false;
		}
		StageKnownResolvableUpdates(plan, capture, true, true);
		if (!IsAcceptedSnapshotCurrent(snapshot, manager))
		{
			AbortRevision(plan.Token);
			GPlannerForceFull.store(true, std::memory_order_release);
			return false;
		}
		LogPlannerPlan(plan, capture, true);
		CommitRevision(plan.Token);
		CommitBaselineUpdates(capture, true);
		// This stock pass has already applied the exact accepted payload. An
		// identical poll is therefore safe to skip. If the committed parse was
		// unsafe, planner state still makes the next changed revision FullNative.
		GPlannerForceFull.store(false, std::memory_order_release);
		return true;
	}

	bool IsExecutableAddress(const void* address) noexcept
	{
		if (!address)
			return false;
		MEMORY_BASIC_INFORMATION region{};
		if (VirtualQuery(address, &region, sizeof(region)) != sizeof(region))
			return false;
		constexpr DWORD executable = PAGE_EXECUTE |
			PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
			PAGE_EXECUTE_WRITECOPY;
		return region.State == MEM_COMMIT &&
			(region.Protect & (PAGE_GUARD | PAGE_NOACCESS)) == 0 &&
			(region.Protect & executable) != 0;
	}

	bool IsReadableMemoryRange(
		const void* address,
		size_t size) noexcept
	{
		if (!address || !size)
			return false;
		const uintptr_t begin = reinterpret_cast<uintptr_t>(address);
		if (begin > (std::numeric_limits<uintptr_t>::max)() - size)
			return false;
		const uintptr_t end = begin + size;
		uintptr_t cursor = begin;
		while (cursor < end)
		{
			MEMORY_BASIC_INFORMATION region{};
			if (VirtualQuery(
					reinterpret_cast<const void*>(cursor),
					&region, sizeof(region)) != sizeof(region) ||
				region.State != MEM_COMMIT ||
				(region.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
			{
				return false;
			}
			const uintptr_t regionBegin =
				reinterpret_cast<uintptr_t>(region.BaseAddress);
			if (regionBegin >
				(std::numeric_limits<uintptr_t>::max)() - region.RegionSize)
			{
				return false;
			}
			const uintptr_t regionEnd = regionBegin + region.RegionSize;
			if (regionEnd <= cursor)
				return false;
			cursor = (std::min)(regionEnd, end);
		}
		return true;
	}

	void* ResolveFunctionContainingReference(uintptr_t reference) noexcept
	{
		if (!reference || !IsExecutableAddress(
			reinterpret_cast<void*>(reference)))
		{
			return nullptr;
		}
		DWORD64 imageBase = 0;
		const PRUNTIME_FUNCTION runtimeFunction = RtlLookupFunctionEntry(
			static_cast<DWORD64>(reference), &imageBase, nullptr);
		if (!runtimeFunction || imageBase == 0)
			return nullptr;

		RUNTIME_FUNCTION current = *runtimeFunction;
		const DWORD64 immediateBegin = imageBase + current.BeginAddress;
		const DWORD64 immediateEnd = imageBase + current.EndAddress;
		if (reference < immediateBegin || reference >= immediateEnd ||
			immediateBegin >= immediateEnd ||
			immediateEnd - immediateBegin > 1024 * 1024)
		{
			return nullptr;
		}

		for (unsigned depth = 0; depth < 8; ++depth)
		{
			const DWORD unwindRva = current.UnwindData & ~1u;
			const auto* unwind = reinterpret_cast<const uint8*>(
				imageBase + unwindRva);
			if (!unwindRva || !IsReadableMemoryRange(unwind, 4))
				return nullptr;
			const uint8 version = unwind[0] & 0x07;
			const uint8 flags = unwind[0] >> 3;
			const uint8 codeCount = unwind[2];
			if (version != 1)
				return nullptr;
			constexpr uint8 chainedInfo = 0x04;
			if ((flags & chainedInfo) == 0)
				break;
			const size_t chainOffset = 4 +
				((static_cast<size_t>(codeCount) + 1) & ~size_t(1)) * 2;
			const auto* chained = reinterpret_cast<const RUNTIME_FUNCTION*>(
				unwind + chainOffset);
			if (!IsReadableMemoryRange(chained, sizeof(*chained)) ||
				chained->BeginAddress >= chained->EndAddress)
			{
				return nullptr;
			}
			current = *chained;
		}

		const DWORD64 begin = imageBase + current.BeginAddress;
		const DWORD64 end = imageBase + current.EndAddress;
		if (begin >= end || end - begin > 1024 * 1024 ||
			!IsExecutableAddress(reinterpret_cast<void*>(begin)))
		{
			return nullptr;
		}
		return reinterpret_cast<void*>(begin);
	}

	bool ReadNativeAnchorPrefix(
		uintptr_t target,
		uint64& prefix) noexcept
	{
		prefix = 0;
		if (!target)
			return false;
		__try
		{
			memcpy(&prefix, reinterpret_cast<const void*>(target),
				sizeof(prefix));
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	bool CollectNativeAnchorRoots(
		FNativeAnchorRoots* anchors,
		size_t anchorCount) noexcept
	{
		if (!anchors || !anchorCount)
			return false;
		constexpr size_t maximumRootsPerAnchor = 64;
		__try
		{
			const auto module = reinterpret_cast<const uint8*>(
				GetModuleHandleW(nullptr));
			if (!module)
				return false;
			const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
			if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0)
				return false;
			const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
				module + dos->e_lfanew);
			if (nt->Signature != IMAGE_NT_SIGNATURE ||
				nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
				nt->FileHeader.NumberOfSections == 0 ||
				nt->FileHeader.NumberOfSections > 96 ||
				nt->OptionalHeader.SizeOfImage < 0x1000)
			{
				return false;
			}
			const size_t imageSize = nt->OptionalHeader.SizeOfImage;
			const uintptr_t imageBegin = reinterpret_cast<uintptr_t>(module);
			const uintptr_t imageEnd = imageBegin + imageSize;
			if (imageEnd <= imageBegin)
				return false;

			for (size_t index = 0; index < anchorCount; ++index)
			{
				auto& anchor = anchors[index];
				const bool hasWide = anchor.WideText && *anchor.WideText;
				const bool hasNarrow = anchor.NarrowText && *anchor.NarrowText;
				if (!hasWide && !hasNarrow)
					return false;
				if (hasWide)
				{
					anchor.WideBytes =
						(wcslen(anchor.WideText) + 1) * sizeof(wchar_t);
					if (anchor.WideBytes < sizeof(uint64))
						return false;
					memcpy(&anchor.WidePrefix,
						anchor.WideText, sizeof(uint64));
				}
				if (hasNarrow)
				{
					anchor.NarrowBytes = strlen(anchor.NarrowText) + 1;
					if (anchor.NarrowBytes < sizeof(uint64))
						return false;
					memcpy(&anchor.NarrowPrefix,
						anchor.NarrowText, sizeof(uint64));
				}
			}

			const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
			for (WORD sectionIndex = 0;
				sectionIndex < nt->FileHeader.NumberOfSections;
				++sectionIndex, ++section)
			{
				if ((section->Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0)
					continue;
				const size_t sectionRva = section->VirtualAddress;
				const size_t sectionSize = (std::min)(
					static_cast<size_t>(section->Misc.VirtualSize),
					imageSize > sectionRva ? imageSize - sectionRva : 0);
				if (sectionSize < 7 || sectionRva >= imageSize)
					continue;
				const uint8* code = module + sectionRva;
				if (!IsReadableMemoryRange(code, sectionSize))
					continue;

				for (size_t offset = 0; offset + 7 <= sectionSize; ++offset)
				{
					const uint8 rex = code[offset];
					if ((rex & 0xf8) != 0x48 || code[offset + 1] != 0x8d ||
						(code[offset + 2] & 0xc7) != 0x05)
					{
						continue;
					}
					int32 displacement = 0;
					memcpy(&displacement, code + offset + 3,
						sizeof(displacement));
					const uintptr_t instructionEnd =
						reinterpret_cast<uintptr_t>(code + offset + 7);
					const uintptr_t target = static_cast<uintptr_t>(
						static_cast<intptr_t>(instructionEnd) + displacement);
					if (target < imageBegin || target >= imageEnd)
						continue;
					uint64 targetPrefix = 0;
					if (!ReadNativeAnchorPrefix(target, targetPrefix))
						continue;

					for (size_t anchorIndex = 0;
						anchorIndex < anchorCount; ++anchorIndex)
					{
						auto& anchor = anchors[anchorIndex];
						if (anchor.Overflowed)
							continue;
						const bool wideMatch = anchor.WideBytes &&
							targetPrefix == anchor.WidePrefix &&
							target <= imageEnd - anchor.WideBytes &&
							IsReadableMemoryRange(
								reinterpret_cast<const void*>(target),
								anchor.WideBytes) &&
							memcmp(reinterpret_cast<const void*>(target),
								anchor.WideText, anchor.WideBytes) == 0;
						const bool narrowMatch = anchor.NarrowBytes &&
							targetPrefix == anchor.NarrowPrefix &&
							target <= imageEnd - anchor.NarrowBytes &&
							IsReadableMemoryRange(
								reinterpret_cast<const void*>(target),
								anchor.NarrowBytes) &&
							memcmp(reinterpret_cast<const void*>(target),
								anchor.NarrowText, anchor.NarrowBytes) == 0;
						if (!wideMatch && !narrowMatch)
							continue;
						void* root = ResolveFunctionContainingReference(
							reinterpret_cast<uintptr_t>(code + offset));
						if (!root)
							continue;
						if (anchor.Roots.size() >= maximumRootsPerAnchor &&
							!anchor.Roots.contains(root))
						{
							anchor.Overflowed = true;
							anchor.Roots.clear();
							continue;
						}
						anchor.Roots.insert(root);
					}
				}
			}
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	void* ResolveUniqueSharedRoot(
		const FNativeAnchorRoots* const* anchors,
		size_t count) noexcept
	{
		if (!anchors || count < 2 || !anchors[0] ||
			anchors[0]->Overflowed)
		{
			return nullptr;
		}
		void* result = nullptr;
		for (void* candidate : anchors[0]->Roots)
		{
			bool shared = true;
			for (size_t index = 1; index < count; ++index)
			{
				if (!anchors[index] || anchors[index]->Overflowed ||
					!anchors[index]->Roots.contains(candidate))
				{
					shared = false;
					break;
				}
			}
			if (!shared)
				continue;
			if (result && result != candidate)
				return nullptr;
			result = candidate;
		}
		return result;
	}

	void* ResolveUniqueVotedRoot(
		const FNativeAnchorRoots* const* anchors,
		size_t count,
		size_t requiredVotes) noexcept
	{
		if (!anchors || count < 2 || requiredVotes < 2 ||
			requiredVotes > count)
		{
			return nullptr;
		}
		for (size_t index = 0; index < count; ++index)
		{
			if (!anchors[index] || anchors[index]->Overflowed)
				return nullptr;
		}
		void* result = nullptr;
		for (size_t sourceIndex = 0; sourceIndex < count; ++sourceIndex)
		{
			for (void* candidate : anchors[sourceIndex]->Roots)
			{
				bool alreadyVisited = false;
				for (size_t previous = 0; previous < sourceIndex; ++previous)
				{
					if (anchors[previous]->Roots.contains(candidate))
					{
						alreadyVisited = true;
						break;
					}
				}
				if (alreadyVisited)
					continue;
				size_t votes = 0;
				for (size_t index = 0; index < count; ++index)
				{
					if (anchors[index]->Roots.contains(candidate))
						++votes;
				}
				if (votes < requiredVotes)
					continue;
				if (result && result != candidate)
					return nullptr;
				result = candidate;
			}
		}
		return result;
	}

	const char* DescribePatchAssetsResolution(unsigned schemes) noexcept
	{
		switch (schemes)
		{
		case 0x1: return "stable";
		case 0x2: return "legacy";
		case 0x3: return "stable+legacy";
		case 0x4: return "current";
		case 0x5: return "stable+current";
		case 0x6: return "legacy+current";
		case 0x7: return "stable+legacy+current";
		default: return "no-agreement";
		}
	}

	void ResolveNativeHotfixTargets()
	{
		FNativeAnchorRoots anchors[] =
		{
			{ L"Specified per-object class %s was not found",
				"Specified per-object class %s was not found" },
			{ L"Updating config from %s took %f seconds and reloaded %d objects",
				"Updating config from %s took %f seconds and reloaded %d objects" },
			{ L"[AssetHotfix]", "[AssetHotfix]" },
			{ L"[/Script/", "[/Script/" },
			{ L"[/Game/", "[/Game/" },
			{ L"UOnlineHotfixManager::PatchAssetsFromIniFiles",
				"UOnlineHotfixManager::PatchAssetsFromIniFiles" },
			{ L"Expected a hotfix type of RowUpdate with 5 tokens or TableUpdate with 3 tokens.",
				"Expected a hotfix type of RowUpdate with 5 tokens or TableUpdate with 3 tokens." },
			{ L"Checking for assets to be patched using data from 'AssetHotfix' section in the Game .ini file",
				"Checking for assets to be patched using data from 'AssetHotfix' section in the Game .ini file" },
			{ L"Wasn't able to parse the data with semicolon separated values. Expecting 3 or 5 arguments.",
				"Wasn't able to parse the data with semicolon separated values. Expecting 3 or 5 arguments." },
			{ L"No assets were found in the 'AssetHotfix' section in the Game .ini file.  No patching needed.",
				"No assets were found in the 'AssetHotfix' section in the Game .ini file.  No patching needed." },
			{ L"Wasn't able to parse the data with semicolon separated values. Expecting 3 or 5 arguments but parsed %d.",
				"Wasn't able to parse the data with semicolon separated values. Expecting 3 or 5 arguments but parsed %d." },
			{ L"No assets were found in the 'AssetHotfix' section in the Game .ini file. No patching needed.",
				"No assets were found in the 'AssetHotfix' section in the Game .ini file. No patching needed." }
		};
		GPatchAssetsResolutionDiagnostic = "scan-failed";
		if (!CollectNativeAnchorRoots(anchors, _countof(anchors)))
			return;

		const FNativeAnchorRoots* iniPrimary[] =
			{ &anchors[0], &anchors[1] };
		GBaseIniTarget = ResolveUniqueSharedRoot(
			iniPrimary, _countof(iniPrimary));
		if (!GBaseIniTarget)
		{
			const FNativeAnchorRoots* iniFallback[] =
				{ &anchors[2], &anchors[3], &anchors[4] };
			GBaseIniTarget = ResolveUniqueSharedRoot(
				iniFallback, _countof(iniFallback));
		}

		const FNativeAnchorRoots* stablePatchAssets[] =
			{ &anchors[5], &anchors[6] };
		const FNativeAnchorRoots* legacyPatchAssets[] =
			{ &anchors[7], &anchors[8], &anchors[9] };
		const FNativeAnchorRoots* currentPatchAssets[] =
			{ &anchors[7], &anchors[10], &anchors[11] };
		void* stable = ResolveUniqueSharedRoot(
			stablePatchAssets, _countof(stablePatchAssets));
		void* legacy = ResolveUniqueVotedRoot(
			legacyPatchAssets, _countof(legacyPatchAssets), 2);
		void* current = ResolveUniqueVotedRoot(
			currentPatchAssets, _countof(currentPatchAssets), 2);
		void* resolvedPatchAssets = nullptr;
		unsigned resolutionSchemes = 0;
		bool resolutionConflict = false;
		const auto mergeResolution = [
			&resolvedPatchAssets, &resolutionSchemes,
			&resolutionConflict](void* candidate, unsigned scheme)
		{
			if (!candidate || resolutionConflict)
				return;
			if (resolvedPatchAssets && resolvedPatchAssets != candidate)
			{
				resolvedPatchAssets = nullptr;
				resolutionConflict = true;
				return;
			}
			resolvedPatchAssets = candidate;
			resolutionSchemes |= scheme;
		};
		mergeResolution(stable, 0x1);
		mergeResolution(legacy, 0x2);
		mergeResolution(current, 0x4);
		GPatchAssetsTarget = resolutionConflict
			? nullptr
			: resolvedPatchAssets;
		GPatchAssetsResolutionDiagnostic = resolutionConflict
			? "conflict"
			: DescribePatchAssetsResolution(resolutionSchemes);
		if (GPatchAssetsTarget == GBaseIniTarget)
		{
			GPatchAssetsTarget = nullptr;
			GPatchAssetsResolutionDiagnostic = "target-collision";
		}
	}

	uint64 ArmRefresh(const UObject* manager, ULONGLONG now) noexcept
	{
		if (!GPatchAssetsHookInstalled.load(std::memory_order_acquire))
			return 0;
		const uint64 managerIdentity = GetObjectIdentity(manager);
		const uint64 managerGeneration =
			GManagerLifecycleGeneration.load(std::memory_order_acquire);
		const uint64 worldGeneration =
			GWorldGeneration.load(std::memory_order_acquire);
		if (!IsCurrentManagerLifecycle(
				managerIdentity, managerGeneration, worldGeneration))
			return 0;
		uint64 serial = GRefreshSequence.fetch_add(
			1, std::memory_order_acq_rel) + 1;
		if (!serial)
		{
			serial = GRefreshSequence.fetch_add(
				1, std::memory_order_acq_rel) + 1;
			if (!serial)
				return 0;
		}
		GPendingRefreshManagerIdentity.store(
			managerIdentity, std::memory_order_relaxed);
		GPendingRefreshManagerGeneration.store(
			managerGeneration, std::memory_order_relaxed);
		GPendingRefreshWorldGeneration.store(
			worldGeneration, std::memory_order_relaxed);
		GPendingRefreshDeadline.store(
			now + kNoCallbackSettleMs, std::memory_order_relaxed);
		GPromotedRefreshManagerIdentity.store(0, std::memory_order_relaxed);
		GPromotedPayloadSerial.store(0, std::memory_order_relaxed);
		GPromotedDeadline.store(0, std::memory_order_relaxed);
		GObservedDefaultGameRefresh.store(0, std::memory_order_relaxed);
		GCompletedObservedDefaultGameRefresh.store(
			0, std::memory_order_relaxed);
		GPromotedRefresh.store(0, std::memory_order_release);
		GPendingRefresh.store(serial, std::memory_order_release);
		return serial;
	}

	bool CancelRefresh(uint64 serial) noexcept
	{
		if (!serial)
			return false;
		const uint64 requestedSerial = serial;
		if (!GPendingRefresh.compare_exchange_strong(
				serial, 0,
				std::memory_order_acq_rel,
				std::memory_order_acquire))
		{
			return false;
		}
		uint64 promoted = requestedSerial;
		GPromotedRefresh.compare_exchange_strong(
			promoted, 0,
			std::memory_order_acq_rel,
			std::memory_order_acquire);
		return true;
	}

	void PromoteRefresh(
		uint64 managerIdentity,
		uint64 payloadSerial,
		ULONGLONG now) noexcept
	{
		const uint64 refresh =
			GPendingRefresh.load(std::memory_order_acquire);
		const uint64 scopedRefresh = GStartHotfixInvocationSequence;
		if (!refresh ||
			(scopedRefresh && scopedRefresh != refresh) ||
			(!scopedRefresh &&
			 !GRefreshAwaitingCompletion.load(std::memory_order_acquire)) ||
			!managerIdentity || !payloadSerial ||
			GCurrentManagerIdentity.load(std::memory_order_acquire) !=
				managerIdentity ||
			GIniHookValidatedManagerIdentity.load(
				std::memory_order_acquire) != managerIdentity ||
			GPendingRefreshManagerIdentity.load(
				std::memory_order_relaxed) != managerIdentity ||
			ReadAcceptedPayloadSerial(managerIdentity) != payloadSerial ||
			now > GPendingRefreshDeadline.load(std::memory_order_relaxed))
		{
			return;
		}
		GPromotedRefreshManagerIdentity.store(
			managerIdentity, std::memory_order_relaxed);
		GPromotedPayloadSerial.store(
			payloadSerial, std::memory_order_relaxed);
		GPromotedDeadline.store(
			now + kAssetPatchPromotionWindowMs,
			std::memory_order_relaxed);
		GPromotedRefresh.store(refresh, std::memory_order_release);
	}

	bool ConsumeRefresh(
		uint64 managerIdentity,
		ULONGLONG now,
		uint64& outRefresh,
		uint64& outPayloadSerial) noexcept
	{
		outRefresh = 0;
		outPayloadSerial = 0;
		if (!managerIdentity)
			return false;
		uint64 refresh = GPendingRefresh.load(std::memory_order_acquire);
		if (!refresh)
			return false;
		const uint64 scopedRefresh = GStartHotfixInvocationSequence;
		if ((scopedRefresh && scopedRefresh != refresh) ||
			GPromotedRefresh.load(std::memory_order_acquire) != refresh ||
			GPendingRefreshManagerIdentity.load(
				std::memory_order_relaxed) != managerIdentity ||
			GPromotedRefreshManagerIdentity.load(
				std::memory_order_relaxed) != managerIdentity)
		{
			return false;
		}
		const uint64 payloadSerial =
			GPromotedPayloadSerial.load(std::memory_order_relaxed);
		if (!payloadSerial ||
			ReadAcceptedPayloadSerial(managerIdentity) != payloadSerial ||
			now > GPendingRefreshDeadline.load(std::memory_order_relaxed) ||
			now > GPromotedDeadline.load(std::memory_order_relaxed))
		{
			// DispatchPatchAssets owns retirement. Leaving the token intact here lets
			// it distinguish this invalid promotion from the normal no-INI unchanged
			// completion and forward the uncertain native pass once.
			return false;
		}
		const uint64 consumedRefresh = refresh;
		if (!GPendingRefresh.compare_exchange_strong(
				refresh, 0,
				std::memory_order_acq_rel,
				std::memory_order_acquire))
		{
			return false;
		}
		uint64 promoted = consumedRefresh;
		GPromotedRefresh.compare_exchange_strong(
			promoted, 0,
			std::memory_order_acq_rel,
			std::memory_order_acquire);
		outRefresh = consumedRefresh;
		outPayloadSerial = payloadSerial;
		return true;
	}

	class FScopedIniDispatch final
	{
	public:
		FScopedIniDispatch() noexcept
		{
			++GIniDispatchDepth;
			FExclusiveSrwLockGuard arbitration(GAssetPatchGateMutex);
			GActiveIniDispatches.fetch_add(1, std::memory_order_acq_rel);
		}
		~FScopedIniDispatch() noexcept
		{
			{
				FExclusiveSrwLockGuard arbitration(GAssetPatchGateMutex);
				GActiveIniDispatches.fetch_sub(1, std::memory_order_acq_rel);
			}
			--GIniDispatchDepth;
		}
		FScopedIniDispatch(const FScopedIniDispatch&) = delete;
		FScopedIniDispatch& operator=(const FScopedIniDispatch&) = delete;
	};

	class FScopedStartHotfixInvocation final
	{
	public:
		explicit FScopedStartHotfixInvocation(uint64 sequence) noexcept
			: Previous(GStartHotfixInvocationSequence)
		{
			GStartHotfixInvocationSequence = sequence;
		}
		~FScopedStartHotfixInvocation() noexcept
		{
			GStartHotfixInvocationSequence = Previous;
		}
		FScopedStartHotfixInvocation(
			const FScopedStartHotfixInvocation&) = delete;
		FScopedStartHotfixInvocation& operator=(
			const FScopedStartHotfixInvocation&) = delete;
	private:
		uint64 Previous = 0;
	};

	void DispatchPatchAssets(
		FPatchAssetsFromIniFiles original,
		void* manager);

	bool DispatchHotfixIniFile(
		FHotfixIniFile original,
		void* manager,
		const FString* fileName,
		const FString* iniData)
	{
		if (!original)
			return false;
		if (GIniDispatchDepth)
			return original(manager, fileName, iniData);
		FScopedIniDispatch dispatchScope;

		const bool defaultGame = IsDefaultGameIni(fileName);
		std::string payload;
		std::string acceptedFileName;
		const EFStringPayloadReadVerdict payloadVerdict = defaultGame
			? ReadHotfixPayloadUtf8(iniData, payload)
			: EFStringPayloadReadVerdict::Invalid;
		const bool fileNameReadable = defaultGame &&
			ReadFStringUtf8(fileName, acceptedFileName, 4096);
		const uint64 managerIdentity = defaultGame
			? GetObjectIdentity(reinterpret_cast<UObject*>(manager))
			: 0;
		const uint64 managerGeneration = defaultGame
			? GManagerLifecycleGeneration.load(std::memory_order_acquire)
			: 0;
		const uint64 worldGeneration = defaultGame
			? GWorldGeneration.load(std::memory_order_acquire)
			: 0;
		const uint64 observedRefresh = defaultGame
			? ObserveDefaultGameCallback(managerIdentity)
			: 0;
		const bool lifecycleReadyBefore = defaultGame &&
			IsCurrentManagerLifecycle(
				managerIdentity, managerGeneration, worldGeneration) &&
			GIniFileHookValidated.load(std::memory_order_acquire) &&
			GIniHookValidatedManagerIdentity.load(
				std::memory_order_acquire) == managerIdentity;
		const bool canStageExactAssetPass = lifecycleReadyBefore &&
			observedRefresh && fileNameReadable &&
			payloadVerdict == EFStringPayloadReadVerdict::Success &&
			IsBoundGameThread() && GPatchAssetsOriginal &&
			GBaseIniOriginal &&
			HasAcceptedPayload(
				managerIdentity, managerGeneration, worldGeneration);
		FScopedExpectedExactPatch expectedExactPatch(
			manager, canStageExactAssetPass);

		// Unreal decides acceptance first. Even when its answer or our bounded copy
		// is unusable, the request-scoped observation above prevents the associated
		// native asset pass from being mistaken for an unchanged-file completion.
		const bool applied = original(manager, fileName, iniData);
		const unsigned suppressedExactPasses = expectedExactPatch.Finish();
		if (!applied || !defaultGame ||
			payloadVerdict != EFStringPayloadReadVerdict::Success ||
			!fileNameReadable)
		{
			if (suppressedExactPasses)
				GPlannerForceFull.store(true, std::memory_order_release);
			// If a readable request became unusable only after Unreal accepted it,
			// preserve stock behavior for any native pass we tentatively staged.
			if (applied && suppressedExactPasses && GPatchAssetsOriginal)
				GPatchAssetsOriginal(manager);
			if (defaultGame && observedRefresh)
			{
				const char* untrackedReason = !applied
					? "native-rejected"
					: (!fileNameReadable ? "filename-unreadable" :
						(payloadVerdict == EFStringPayloadReadVerdict::Oversize
						? "payload-oversize" : "payload-unreadable"));
				AtlasDiagnostics::WriteLine(
					"hotfix-reload defaultgame-untracked "
					"reason=%s refresh=%llu callback-tid=%lu",
					untrackedReason,
					static_cast<unsigned long long>(observedRefresh),
					GetCurrentThreadId());
			}
			return applied;
		}

		if (!IsCurrentManagerLifecycle(
				managerIdentity, managerGeneration, worldGeneration) ||
			GIniHookValidatedManagerIdentity.load(
				std::memory_order_acquire) != managerIdentity)
		{
			if (suppressedExactPasses)
				GPlannerForceFull.store(true, std::memory_order_release);
			if (suppressedExactPasses && GPatchAssetsOriginal)
				GPatchAssetsOriginal(manager);
			if (observedRefresh)
			{
				AtlasDiagnostics::WriteLine(
					"hotfix-reload defaultgame-untracked "
					"reason=lifecycle-mismatch refresh=%llu "
					"callback-tid=%lu",
					static_cast<unsigned long long>(observedRefresh),
					GetCurrentThreadId());
			}
			return applied;
		}

		const ULONGLONG now = GetTickCount64();
		uint64 payloadSerial = 0;
		const size_t payloadBytes = payload.size();
		const bool payloadChanged = CommitAcceptedPayload(
			managerIdentity,
			managerGeneration,
			worldGeneration,
			std::move(payload),
			std::move(acceptedFileName),
			GBaseIniOriginal,
			payloadSerial);
		bool observedNativePassCompleted = false;
		if (observedRefresh && payloadSerial)
		{
			uint64 expectedCompletedRefresh = observedRefresh;
			observedNativePassCompleted =
				GCompletedObservedDefaultGameRefresh.compare_exchange_strong(
					expectedCompletedRefresh, 0,
					std::memory_order_acq_rel,
					std::memory_order_acquire);
		}
		if (observedNativePassCompleted)
		{
			const bool plannerSeeded =
				SeedPlannerAfterNativePass(manager, payloadSerial);
			{
				FExclusiveSrwLockGuard lock(GAssetPatchGateMutex);
				GAppliedPayloadIdentityReady = plannerSeeded;
				GAppliedPayloadManagerIdentity = plannerSeeded
					? managerIdentity : 0;
				GAppliedPayloadSerial = plannerSeeded
					? payloadSerial : 0;
			}
			if (CancelRefresh(observedRefresh))
			{
				uint64 expectedRefresh = observedRefresh;
				const bool retiredActiveRefresh =
					GRefreshSerial.compare_exchange_strong(
					expectedRefresh, 0,
					std::memory_order_acq_rel,
					std::memory_order_acquire);
				if (retiredActiveRefresh)
				{
					GRefreshAwaitingCompletion.store(
						false, std::memory_order_release);
				}
			}
		}
		else if (payloadSerial)
		{
			PromoteRefresh(managerIdentity, payloadSerial, now);
		}
		if (payloadChanged)
		{
			AtlasDiagnostics::WriteLine(
				"hotfix-reload defaultgame-accepted serial=%llu "
				"bytes=%llu refresh=%llu exact-passes-staged=%u "
				"callback-tid=%lu",
				static_cast<unsigned long long>(payloadSerial),
				static_cast<unsigned long long>(payloadBytes),
				static_cast<unsigned long long>(observedRefresh),
				suppressedExactPasses,
				GetCurrentThreadId());
		}
		if (suppressedExactPasses && GPatchAssetsOriginal)
		{
			// The exact callback is now accepted and correlated. Re-enter the
			// normal gate once, outside the temporary suppression scope, so the
			// planner can choose byte/no-asset/full/delta behavior transactionally.
			DispatchPatchAssets(GPatchAssetsOriginal, manager);
		}
		return applied;
	}

	bool BaseIniDetour(
		void* manager,
		const FString* fileName,
		const FString* iniData)
	{
		return DispatchHotfixIniFile(
			GBaseIniOriginal, manager, fileName, iniData);
	}

	bool ManagerIniDetour(
		void* manager,
		const FString* fileName,
		const FString* iniData)
	{
		return DispatchHotfixIniFile(
			GManagerIniOriginal, manager, fileName, iniData);
	}

	void DispatchPatchAssets(
		FPatchAssetsFromIniFiles original,
		void* manager)
	{
		if (!original)
			return;
		const ULONGLONG now = GetTickCount64();
		const uint64 managerIdentity = GetObjectIdentity(
			reinterpret_cast<UObject*>(manager));
		const uint64 recycleRefresh =
			GNoCallbackRecycleRefresh.load(std::memory_order_acquire);
		const uint64 recycleManagerGeneration =
			GNoCallbackRecycleManagerGeneration.load(
				std::memory_order_relaxed);
		const uint64 recycleWorldGeneration =
			GNoCallbackRecycleWorldGeneration.load(
				std::memory_order_relaxed);
		const bool lateNoCallbackPass =
			managerIdentity && recycleRefresh &&
			GNoCallbackRecycleManagerIdentity.load(
				std::memory_order_relaxed) == managerIdentity &&
			IsCurrentManagerLifecycle(
				managerIdentity,
				recycleManagerGeneration,
				recycleWorldGeneration);
		const uint64 pendingRefresh =
			GPendingRefresh.load(std::memory_order_acquire);
		const uint64 pendingManagerGeneration =
			GPendingRefreshManagerGeneration.load(std::memory_order_relaxed);
		const uint64 pendingWorldGeneration =
			GPendingRefreshWorldGeneration.load(std::memory_order_relaxed);
		const bool armedForManager =
			GRefreshAwaitingCompletion.load(std::memory_order_acquire) &&
			managerIdentity &&
			GPendingRefreshManagerIdentity.load(
				std::memory_order_acquire) == managerIdentity &&
			pendingRefresh != 0 &&
			IsCurrentManagerLifecycle(
				managerIdentity,
				pendingManagerGeneration,
				pendingWorldGeneration);
		const uint64 promotedRefreshBeforeConsume =
			GPromotedRefresh.load(std::memory_order_acquire);
		const uint64 observedDefaultGameRefresh =
			GObservedDefaultGameRefresh.load(std::memory_order_acquire);
		const bool armedTokenOwnedPass = armedForManager &&
			now <= GPendingRefreshDeadline.load(std::memory_order_relaxed);
		const bool tombstoneOwnedPass = lateNoCallbackPass &&
			!GInvocationActive.load(std::memory_order_acquire) &&
			GStartHotfixInvocationSequence == 0;
		const bool defaultGameObservedForArmedRequest =
			armedForManager && observedDefaultGameRefresh == pendingRefresh;
		const bool defaultGameObservedForTombstone =
			lateNoCallbackPass &&
			observedDefaultGameRefresh == recycleRefresh;
		const bool defaultGameObservedForRequest =
			defaultGameObservedForArmedRequest ||
			defaultGameObservedForTombstone;
		uint64 refresh = 0;
		uint64 payloadSerial = 0;
		const bool knownRefresh = ConsumeRefresh(
			managerIdentity, now, refresh, payloadSerial);
		// StartHotfixProcess still reaches PatchAssetsFromIniFiles when Unreal's
		// title-file comparison decides every returned file is unchanged. In that
		// case HotfixIniFile is intentionally never called, so there is no promoted
		// DefaultGame payload to consume. A pass owned by the current armed request
		// (or its short no-callback tombstone), in the same manager/world lifecycle,
		// is the native unchanged-completion boundary. It
		// must complete the request without walking every CurveTable/DataTable again.
		const bool noPayloadCompletionCandidate = !knownRefresh &&
			promotedRefreshBeforeConsume == 0 &&
			!defaultGameObservedForRequest &&
			GIniDispatchDepth == 0 &&
			(armedTokenOwnedPass || tombstoneOwnedPass);
		bool claimedNoPayloadCompletion = false;
		if (noPayloadCompletionCandidate)
		{
			// Arbitrate with outer INI scope entry. This makes the active-INI zero
			// observation and exact token claim one ordered decision: whichever path
			// enters first owns the pending request boundary.
			FExclusiveSrwLockGuard arbitration(GAssetPatchGateMutex);
			const bool noActiveIni =
				GActiveIniDispatches.load(std::memory_order_acquire) == 0;
			if (noActiveIni && armedTokenOwnedPass)
			{
				claimedNoPayloadCompletion = CancelRefresh(pendingRefresh);
			}
			else if (noActiveIni)
			{
				uint64 expectedRecycle = recycleRefresh;
				claimedNoPayloadCompletion =
					GNoCallbackRecycleRefresh.compare_exchange_strong(
						expectedRecycle, 0,
						std::memory_order_acq_rel,
						std::memory_order_acquire);
			}
		}
		bool claimedObservedCompletion = false;
		if (!knownRefresh && defaultGameObservedForRequest &&
			GIniDispatchDepth == 0)
		{
			FExclusiveSrwLockGuard arbitration(GAssetPatchGateMutex);
			const bool noActiveIni =
				GActiveIniDispatches.load(std::memory_order_acquire) == 0;
			if (noActiveIni && defaultGameObservedForArmedRequest)
			{
				claimedObservedCompletion = CancelRefresh(pendingRefresh);
			}
			else if (noActiveIni)
			{
				uint64 expectedRecycle = recycleRefresh;
				claimedObservedCompletion =
					GNoCallbackRecycleRefresh.compare_exchange_strong(
						expectedRecycle, 0,
						std::memory_order_acq_rel,
						std::memory_order_acquire);
			}
			if (claimedObservedCompletion)
			{
				uint64 expectedObservation = observedDefaultGameRefresh;
				GObservedDefaultGameRefresh.compare_exchange_strong(
					expectedObservation, 0,
					std::memory_order_acq_rel,
					std::memory_order_acquire);
			}
		}
		if (knownRefresh)
		{
			uint64 expectedObservation = refresh;
			GObservedDefaultGameRefresh.compare_exchange_strong(
				expectedObservation, 0,
				std::memory_order_acq_rel,
				std::memory_order_acquire);
		}
		// Suppression is permitted only after this detour owns the exact request
		// token. A failed CAS means state changed under us, so the stock pass must
		// be forwarded instead of being swallowed.
		const bool safeNoPayloadCompletion =
			noPayloadCompletionCandidate && claimedNoPayloadCompletion;

		const uint64 acceptedSerialBeforePass =
			ReadAcceptedPayloadSerial(managerIdentity);
		bool skip = false;
		const char* reason = "outside-added-refresh";
		EAssetPatchDecision decision =
			EAssetPatchDecision::OutsideAddedRefresh;
		uint64 diagnosticPayloadSerial = 0;
		bool logDecision = false;
		uint64 allowedCount = 0;
		uint64 skippedCount = 0;
		bool plannerRequestedForceFull = false;
		{
			FExclusiveSrwLockGuard lock(GAssetPatchGateMutex);
			if (knownRefresh)
			{
				diagnosticPayloadSerial = payloadSerial;
				if (!GAppliedPayloadIdentityReady)
				{
					reason = "first-observed-payload";
					decision = EAssetPatchDecision::FirstObservedPayload;
					plannerRequestedForceFull = true;
				}
				else if (GAppliedPayloadManagerIdentity != managerIdentity)
				{
					reason = "manager-transition";
					decision = EAssetPatchDecision::ManagerTransition;
					plannerRequestedForceFull = true;
				}
				else if (GAppliedPayloadSerial != payloadSerial)
				{
					reason = "payload-changed";
					decision = EAssetPatchDecision::PayloadChanged;
				}
				else
				{
					skip = true;
					reason = "payload-unchanged";
					decision = EAssetPatchDecision::PayloadUnchanged;
				}
			}
			else if (safeNoPayloadCompletion)
			{
				// An accepted callback can finish on a worker thread or just outside
				// the promotion window. If its revision has not reached the native
				// asset walker yet, this exact unchanged-completion boundary applies
				// it once instead of losing it forever.
				if (acceptedSerialBeforePass &&
					(!GAppliedPayloadIdentityReady ||
					 GAppliedPayloadManagerIdentity != managerIdentity ||
					 GAppliedPayloadSerial != acceptedSerialBeforePass))
				{
					reason = "accepted-payload-recovery";
					decision = EAssetPatchDecision::AcceptedPayloadRecovery;
					diagnosticPayloadSerial = acceptedSerialBeforePass;
					plannerRequestedForceFull =
						!GAppliedPayloadIdentityReady ||
						GAppliedPayloadManagerIdentity != managerIdentity;
				}
				else
				{
					skip = true;
					reason = armedForManager
						? "native-unchanged-no-defaultgame"
						: "late-native-unchanged-no-defaultgame";
					decision = armedForManager
						? EAssetPatchDecision::NativeUnchanged
						: EAssetPatchDecision::LateNativeUnchanged;
				}
			}
			else if (defaultGameObservedForRequest)
			{
				reason = claimedObservedCompletion
					? (defaultGameObservedForArmedRequest
						? "defaultgame-callback-untracked"
						: "late-defaultgame-callback-untracked")
					: "defaultgame-correlation-race";
				decision = claimedObservedCompletion
					? (defaultGameObservedForArmedRequest
						? EAssetPatchDecision::DefaultGameUntracked
						: EAssetPatchDecision::LateDefaultGameUntracked)
					: EAssetPatchDecision::CorrelationRace;
				diagnosticPayloadSerial = acceptedSerialBeforePass;
			}
			else if (armedForManager || lateNoCallbackPass)
			{
				reason = "asset-pass-correlation-race";
				decision = EAssetPatchDecision::CorrelationRace;
				diagnosticPayloadSerial = acceptedSerialBeforePass;
			}

			if (skip)
				++GAssetPatchSkippedCount;
			else
				++GAssetPatchAllowedCount;
			allowedCount = GAssetPatchAllowedCount;
			skippedCount = GAssetPatchSkippedCount;
			if (GLastLoggedAssetPatchDecision != decision ||
				GLastLoggedAssetPatchPayloadSerial !=
					diagnosticPayloadSerial)
			{
				GLastLoggedAssetPatchDecision = decision;
				GLastLoggedAssetPatchPayloadSerial =
					diagnosticPayloadSerial;
				logDecision = true;
			}
		}

		if (logDecision)
		{
			AtlasDiagnostics::WriteLine(
				"hotfix-reload asset-pass decision=%s reason=%s "
				"added-refresh=%s allowed=%llu skipped=%llu",
				skip ? "skip" : "allow", reason,
				(knownRefresh || safeNoPayloadCompletion ||
				 defaultGameObservedForRequest || armedForManager ||
				 lateNoCallbackPass) ? "yes" : "no",
				static_cast<unsigned long long>(allowedCount),
				static_cast<unsigned long long>(skippedCount));
		}

		if (knownRefresh)
		{
			uint64 expectedRefresh = refresh;
			const bool retiredActiveRefresh =
				GRefreshSerial.compare_exchange_strong(
				expectedRefresh, 0,
				std::memory_order_acq_rel,
				std::memory_order_acquire);
			if (retiredActiveRefresh)
			{
				GRefreshAwaitingCompletion.store(
					false, std::memory_order_release);
			}
		}
		else if (safeNoPayloadCompletion || claimedObservedCompletion)
		{
			const bool completedArmedRequest = safeNoPayloadCompletion
				? armedForManager
				: defaultGameObservedForArmedRequest;
			if (completedArmedRequest)
			{
				const uint64 completedRefresh = safeNoPayloadCompletion
					? pendingRefresh : observedDefaultGameRefresh;
				uint64 expectedRefresh = completedRefresh;
				const bool retiredActiveRefresh =
					GRefreshSerial.compare_exchange_strong(
					expectedRefresh, 0,
					std::memory_order_acq_rel,
					std::memory_order_acquire);
				if (retiredActiveRefresh)
				{
					GRefreshAwaitingCompletion.store(
						false, std::memory_order_release);
				}
			}
			GNoCallbackRecycleManagerIdentity.store(
				0, std::memory_order_relaxed);
			GNoCallbackRecycleManagerGeneration.store(
				0, std::memory_order_relaxed);
			GNoCallbackRecycleWorldGeneration.store(
				0, std::memory_order_relaxed);
		}
		else if ((armedForManager || lateNoCallbackPass) &&
			!defaultGameObservedForRequest)
		{
			bool claimedRefresh = false;
			// A no-payload candidate that lost its ownership CAS is no longer ours.
			// Forward it untouched; do not retry the claim using a stale snapshot.
			if (noPayloadCompletionCandidate)
			{
				claimedRefresh = false;
			}
			else if (armedForManager)
			{
				claimedRefresh = CancelRefresh(pendingRefresh);
			}
			else
			{
				uint64 expectedRecycle = recycleRefresh;
				claimedRefresh =
					GNoCallbackRecycleRefresh.compare_exchange_strong(
						expectedRecycle, 0,
						std::memory_order_acq_rel,
						std::memory_order_acquire);
			}
			if (claimedRefresh)
			{
				if (armedForManager)
				{
					uint64 expectedRefresh = pendingRefresh;
					const bool retiredActiveRefresh =
						GRefreshSerial.compare_exchange_strong(
						expectedRefresh, 0,
						std::memory_order_acq_rel,
						std::memory_order_acquire);
					if (retiredActiveRefresh)
					{
						GRefreshAwaitingCompletion.store(
							false, std::memory_order_release);
					}
				}
			}
		}

		if (skip)
			return;

		uint64 plannedPayloadSerial = 0;
		const bool revisionOwnedPass =
			knownRefresh ||
			decision == EAssetPatchDecision::AcceptedPayloadRecovery;
		if (knownRefresh)
			plannedPayloadSerial = payloadSerial;
		else if (decision == EAssetPatchDecision::AcceptedPayloadRecovery)
			plannedPayloadSerial = acceptedSerialBeforePass;

		bool plannerHandled = false;
		bool plannerCommitted = false;
		if (revisionOwnedPass && plannedPayloadSerial)
		{
			FAcceptedPayloadSnapshot snapshot;
			if (ReadAcceptedPayloadSnapshot(
					managerIdentity, plannedPayloadSerial, snapshot))
			{
				plannerHandled = TryApplyAcceptedRevision(
					original, manager, snapshot,
					plannerRequestedForceFull,
					plannerCommitted);
			}
		}

		bool stockNativeRan = false;
		if (!plannerHandled)
		{
			// Untracked/raced revisions retain the engine's stock behavior. A
			// correlated planner failure forwards one full pass only when no trusted
			// absent-row restoration would be lost; those plans report handled and
			// preserve the prior committed revision for the next bounded retry.
			GPlannerForceFull.store(true, std::memory_order_release);
			original(manager);
			stockNativeRan = true;
		}
		if (!knownRefresh && defaultGameObservedForRequest)
		{
			GCompletedObservedDefaultGameRefresh.store(
				observedDefaultGameRefresh,
				std::memory_order_release);
		}

		const uint64 managerIdentityAfter = GetObjectIdentity(
			reinterpret_cast<UObject*>(manager));
		const uint64 acceptedSerial =
			ReadAcceptedPayloadSerial(managerIdentityAfter);
		const bool operationCommitted =
			stockNativeRan || plannerCommitted;
		const bool stableIdentity = managerIdentityAfter &&
			managerIdentityAfter == managerIdentity && acceptedSerial &&
			operationCommitted &&
			(!plannedPayloadSerial ||
				acceptedSerial == plannedPayloadSerial);
		bool plannerSynchronized = true;
		if (stockNativeRan && revisionOwnedPass && stableIdentity)
		{
			// The native pass already completed, so synchronize the text planner
			// without performing or sampling another asset walk.
			plannerSynchronized =
				SeedPlannerAfterNativePass(manager, acceptedSerial);
		}
		const bool appliedIdentityReady =
			stableIdentity && plannerSynchronized;
		{
			FExclusiveSrwLockGuard lock(GAssetPatchGateMutex);
			GAppliedPayloadIdentityReady = appliedIdentityReady;
			GAppliedPayloadManagerIdentity = appliedIdentityReady
				? managerIdentityAfter : 0;
			GAppliedPayloadSerial = appliedIdentityReady
				? acceptedSerial : 0;
		}
	}

	void PatchAssetsDetour(void* manager)
	{
		if (manager && GExpectedExactPatch.Active &&
			GExpectedExactPatch.Manager == manager)
		{
			if (GExpectedExactPatch.SuppressedPasses !=
				(std::numeric_limits<unsigned>::max)())
			{
				++GExpectedExactPatch.SuppressedPasses;
			}
			return;
		}

		if (manager && GSyntheticPatch.Manager == manager &&
			GSyntheticPatch.Phase != ESyntheticPatchPhase::None)
		{
			if (GSyntheticPatch.Phase == ESyntheticPatchPhase::Delta &&
				!GSyntheticPatch.NativePassObserved)
			{
				GSyntheticPatch.NativePassObserved = true;
				if (GPatchAssetsOriginal)
					GPatchAssetsOriginal(manager);
				return;
			}
			if (GSyntheticPatch.SuppressedPasses !=
				(std::numeric_limits<unsigned>::max)())
			{
				++GSyntheticPatch.SuppressedPasses;
			}
			return;
		}
		DispatchPatchAssets(GPatchAssetsOriginal, manager);
	}

	bool ReadVtableEntryGuarded(
		const UObject* object,
		size_t index,
		void*& out) noexcept
	{
		out = nullptr;
		if (!object || index >= 512)
			return false;
		__try
		{
			if (!object->Vft ||
				!IsReadableMemoryRange(object->Vft + index, sizeof(void*)))
			{
				return false;
			}
			out = object->Vft[index];
			return out != nullptr;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			out = nullptr;
			return false;
		}
	}

	void EnsureManagerIniHook(
		UObject* manager,
		const UClass* baseManagerClass) noexcept
	{
		if (!GBaseIniHookEnabled || !GBaseIniTarget ||
			!manager || !baseManagerClass)
		{
			return;
		}
		const uint64 managerIdentity = GetObjectIdentity(manager);
		if (!managerIdentity)
			return;
		if (GIniHookValidatedManagerIdentity.load(
				std::memory_order_acquire) == managerIdentity &&
			GIniFileHookValidated.load(std::memory_order_acquire))
		{
			return;
		}
		if (GIniHookRejectedManagerIdentity.load(
				std::memory_order_acquire) == managerIdentity)
		{
			return;
		}

		GIniFileHookValidated.store(false, std::memory_order_release);
		GIniHookValidatedManagerIdentity.store(0, std::memory_order_release);
		GIniHookRejectedManagerIdentity.store(0, std::memory_order_release);

		UObject* baseDefault = nullptr;
		__try
		{
			baseDefault =
				const_cast<UClass*>(baseManagerClass)->GetDefaultObj();
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			baseDefault = nullptr;
		}
		if (!IsLiveObject(baseDefault) || !IsLiveObject(manager))
			return;

		size_t slot = 512;
		for (size_t index = 0; index < 512; ++index)
		{
			void* entry = nullptr;
			if (!ReadVtableEntryGuarded(baseDefault, index, entry))
				break;
			if (entry == GBaseIniTarget)
			{
				slot = index;
				break;
			}
		}
		if (slot >= 512)
		{
			if (!GLoggedManagerIniHookFailure)
			{
				GLoggedManagerIniHookFailure = true;
				AtlasDiagnostics::WriteLine(
					"hotfix-reload manager-ini-hook-unavailable "
					"reason=base-vtable-slot");
			}
			GIniHookRejectedManagerIdentity.store(
				managerIdentity, std::memory_order_release);
			return;
		}

		void* dispatchTarget = nullptr;
		if (!ReadVtableEntryGuarded(manager, slot, dispatchTarget) ||
			!IsExecutableAddress(dispatchTarget) ||
			dispatchTarget == GPatchAssetsTarget)
		{
			GIniHookRejectedManagerIdentity.store(
				managerIdentity, std::memory_order_release);
			return;
		}
		if (dispatchTarget == GBaseIniTarget)
		{
			GIniHookValidatedManagerIdentity.store(
				managerIdentity, std::memory_order_release);
			GIniFileHookValidated.store(true, std::memory_order_release);
			return;
		}
		if (GManagerIniTarget)
		{
			if (GManagerIniTarget == dispatchTarget)
			{
				GIniHookValidatedManagerIdentity.store(
					managerIdentity, std::memory_order_release);
				GIniFileHookValidated.store(true, std::memory_order_release);
				return;
			}
			if (!GLoggedManagerIniHookFailure)
			{
				GLoggedManagerIniHookFailure = true;
				AtlasDiagnostics::WriteLine(
					"hotfix-reload manager-ini-hook-unavailable "
					"reason=distinct-override");
			}
			GIniHookRejectedManagerIdentity.store(
				managerIdentity, std::memory_order_release);
			return;
		}

		const MH_STATUS createStatus = MH_CreateHook(
			dispatchTarget,
			reinterpret_cast<LPVOID>(ManagerIniDetour),
			reinterpret_cast<LPVOID*>(&GManagerIniOriginal));
		if (createStatus != MH_OK || !GManagerIniOriginal)
		{
			GManagerIniOriginal = nullptr;
			GIniHookRejectedManagerIdentity.store(
				managerIdentity, std::memory_order_release);
			if (!GLoggedManagerIniHookFailure)
			{
				GLoggedManagerIniHookFailure = true;
				AtlasDiagnostics::WriteLine(
					"hotfix-reload manager-ini-hook-unavailable "
					"reason=minhook-create status=%d",
					static_cast<int>(createStatus));
			}
			return;
		}
		const MH_STATUS enableStatus = MH_EnableHook(dispatchTarget);
		if (enableStatus != MH_OK && enableStatus != MH_ERROR_ENABLED)
		{
			MH_RemoveHook(dispatchTarget);
			GManagerIniOriginal = nullptr;
			GIniHookRejectedManagerIdentity.store(
				managerIdentity, std::memory_order_release);
			if (!GLoggedManagerIniHookFailure)
			{
				GLoggedManagerIniHookFailure = true;
				AtlasDiagnostics::WriteLine(
					"hotfix-reload manager-ini-hook-unavailable "
					"reason=minhook-enable status=%d",
					static_cast<int>(enableStatus));
			}
			return;
		}

		GManagerIniTarget = dispatchTarget;
		GIniHookValidatedManagerIdentity.store(
			managerIdentity, std::memory_order_release);
		GIniFileHookValidated.store(true, std::memory_order_release);
		AtlasDiagnostics::WriteLine(
			"hotfix-reload manager-ini-hook-installed "
			"base=%p override=%p slot=%zu",
			GBaseIniTarget, dispatchTarget, slot);
	}

	bool InitializeClassNames() noexcept
	{
		if (GClassNamesInitialized)
			return true;
		__try
		{
			GClassMetaName = FName(L"Class");
			for (size_t index = 0;
				index < _countof(kManagerClassNames); ++index)
			{
				GManagerClassNames[index] = FName(kManagerClassNames[index]);
			}
			GClassNamesInitialized = true;
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	int GetObjectCount() noexcept
	{
		__try
		{
			return TUObjectArray::Num();
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return 0;
		}
	}

	const UClass* GetManagerClassCandidate(
		int index,
		size_t* matchedNameIndex) noexcept
	{
		if (matchedNameIndex)
			*matchedNameIndex = 0;
		__try
		{
			const FUObjectItem* item =
				TUObjectArray::GetItemByIndex(index);
			const UObject* object = item ? item->Object : nullptr;
			if (!object || (item->Flags & 0x20) != 0 || !object->Class)
				return nullptr;
			for (size_t nameIndex = 0;
				nameIndex < _countof(GManagerClassNames); ++nameIndex)
			{
				if (object->Name != GManagerClassNames[nameIndex])
					continue;
				if (object->Class->Name != GClassMetaName)
					return nullptr;
				if (matchedNameIndex)
					*matchedNameIndex = nameIndex;
				return reinterpret_cast<const UClass*>(object);
			}
			return nullptr;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return nullptr;
		}
	}

	const UObject* GetManagerCandidate(
		int index,
		const UClass* managerClass) noexcept
	{
		__try
		{
			const FUObjectItem* item =
				TUObjectArray::GetItemByIndex(index);
			const UObject* object = item ? item->Object : nullptr;
			if (!object || (item->Flags & 0x20) != 0 ||
				object->IsDefaultObject() || !object->Class ||
				!object->IsA(managerClass))
			{
				return nullptr;
			}
			return object;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return nullptr;
		}
	}

	UFunction* FindStartHotfixProcess(UObject* manager) noexcept
	{
		__try
		{
			return manager
				? manager->GetFunction("StartHotfixProcess")
				: nullptr;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return nullptr;
		}
	}

	bool HasZeroParameters(const UFunction* function) noexcept
	{
		if (!function)
			return false;
		__try
		{
			return function->GetPropertiesSize() == 0;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	void ResetManagerScanBackoff() noexcept
	{
		GManagerScanMissCount = 0;
		GManagerScanFloorIndex = 0;
		GManagerScanSnapshotObjectCount = 0;
		GLastCompletedManagerScanObjectCount = 0;
		GNextManagerFullScanAt = 0;
		GNextManagerGrowthCheckAt = 0;
		GManagerScanKind = EManagerScanKind::None;
	}

	ULONGLONG ConsumeManagerScanBackoff() noexcept
	{
		const unsigned shift = GManagerScanMissCount < 4
			? GManagerScanMissCount
			: 4;
		if (GManagerScanMissCount < 5)
			++GManagerScanMissCount;
		const ULONGLONG delay = kManagerScanRetryMs << shift;
		return delay < kManagerScanMaxRetryMs
			? delay
			: kManagerScanMaxRetryMs;
	}

	void ResetAppliedPayloadIdentity() noexcept
	{
		FExclusiveSrwLockGuard lock(GAssetPatchGateMutex);
		GAppliedPayloadIdentityReady = false;
		GAppliedPayloadManagerIdentity = 0;
		GAppliedPayloadSerial = 0;
		GLastLoggedAssetPatchDecision = EAssetPatchDecision::None;
		GLastLoggedAssetPatchPayloadSerial = 0;
	}

	void ResetManager() noexcept
	{
		GPlannerForceFull.store(true, std::memory_order_release);
		const uint64 previousManagerIdentity =
			GCurrentManagerIdentity.exchange(0, std::memory_order_acq_rel);
		if (previousManagerIdentity)
			AdvanceLifecycleGeneration(GManagerLifecycleGeneration);
		const uint64 activeRefresh =
			GRefreshSerial.load(std::memory_order_acquire);
		const uint64 pending = activeRefresh
			? activeRefresh
			: GPendingRefresh.load(std::memory_order_acquire);
		CancelRefresh(pending);
		GRefreshSerial.store(0, std::memory_order_release);
		GRefreshAwaitingCompletion.store(false, std::memory_order_release);
		GObservedDefaultGameRefresh.store(0, std::memory_order_release);
		GCompletedObservedDefaultGameRefresh.store(
			0, std::memory_order_release);
		GNoCallbackRecycleRefresh.store(0, std::memory_order_release);
		GNoCallbackRecycleManagerIdentity.store(0, std::memory_order_relaxed);
		GNoCallbackRecycleManagerGeneration.store(
			0, std::memory_order_relaxed);
		GNoCallbackRecycleWorldGeneration.store(
			0, std::memory_order_relaxed);
		InvalidateAcceptedPayload();
		ResetAppliedPayloadIdentity();
		GIniHookValidatedManagerIdentity.store(0, std::memory_order_release);
		GIniHookRejectedManagerIdentity.store(0, std::memory_order_release);
		GIniFileHookValidated.store(false, std::memory_order_release);
		GManager = nullptr;
		GStartHotfixProcess = nullptr;
		GManagerScanIndex = -1;
		GManagerScanFloorIndex = 0;
		GManagerScanSnapshotObjectCount = 0;
		GManagerScanKind = EManagerScanKind::None;
	}

	void ResetManagerClass() noexcept
	{
		ResetManager();
		ResetManagerScanBackoff();
		GManagerClass = nullptr;
		GClassScanIndex = -1;
		for (auto& discoveredClass : GDiscoveredManagerClasses)
			discoveredClass = nullptr;
	}

	void LogUnavailable(ULONGLONG now, const char* reason) noexcept
	{
		if (now < GNextUnavailableLogAt)
			return;
		GNextUnavailableLogAt = now + kUnavailableLogIntervalMs;
		AtlasDiagnostics::WriteLine(
			"hotfix-reload unavailable reason=%s",
			reason ? reason : "unknown");
	}

	void ResolveManagerClass(ULONGLONG now) noexcept
	{
		if (GManagerClass && !IsLiveObject(GManagerClass))
		{
			AtlasDiagnostics::WriteLine(
				"hotfix-reload manager-class-stale class=%p",
				GManagerClass);
			ResetManagerClass();
			GNextClassResolveAt = now + kManagerScanRetryMs;
		}
		if (GManagerClass || now < GNextClassResolveAt)
			return;
		if (!InitializeClassNames())
		{
			GNextClassResolveAt = now + kManagerScanRetryMs;
			LogUnavailable(now, "class-name-initialization-failed");
			return;
		}
		const int objectCount = GetObjectCount();
		if (objectCount <= 0)
		{
			GNextClassResolveAt = now + kManagerScanRetryMs;
			LogUnavailable(now, "object-array-unavailable");
			return;
		}
		if (GClassScanIndex < 0 || GClassScanIndex >= objectCount)
			GClassScanIndex = objectCount - 1;

		const int endExclusive = (std::max)(
			-1, GClassScanIndex - kClassObjectsPerTick);
		for (int index = GClassScanIndex;
			index > endExclusive; --index)
		{
			size_t matchedNameIndex = 0;
			const UClass* candidate = GetManagerClassCandidate(
				index, &matchedNameIndex);
			if (candidate)
				GDiscoveredManagerClasses[matchedNameIndex] = candidate;
		}
		GClassScanIndex = endExclusive;
		if (GClassScanIndex >= 0)
			return;

		size_t selectedNameIndex = 0;
		for (; selectedNameIndex < _countof(GDiscoveredManagerClasses);
			++selectedNameIndex)
		{
			if (IsLiveObject(GDiscoveredManagerClasses[selectedNameIndex]))
			{
				GManagerClass =
					GDiscoveredManagerClasses[selectedNameIndex];
				break;
			}
		}
		for (auto& discoveredClass : GDiscoveredManagerClasses)
			discoveredClass = nullptr;
		if (!GManagerClass)
		{
			GNextClassResolveAt = now + kClassCycleRetryMs;
			LogUnavailable(now, "manager-class-not-found");
			return;
		}
		GManagerScanIndex = -1;
		GNextManagerScanAt = now + 1;
		GNextUnavailableLogAt = 0;
		AtlasDiagnostics::WriteLine(
			"hotfix-reload manager-class-resolved name=%s class=%p",
			kManagerClassDiagnosticNames[selectedNameIndex],
			GManagerClass);
	}

	void ResolveManager(ULONGLONG now) noexcept
	{
		if (GManager &&
			(!IsUsableManager(GManager, GManagerClass) ||
			 !IsLiveObject(GStartHotfixProcess)))
		{
			AtlasDiagnostics::WriteLine(
				"hotfix-reload manager-stale manager=%p function=%p",
				GManager, GStartHotfixProcess);
			ResetManager();
			GNextManagerScanAt = now + kManagerScanRetryMs;
		}
		if (GManager || !GManagerClass || now < GNextManagerScanAt)
			return;

		const bool fullScanDue =
			GManagerScanKind != EManagerScanKind::Full &&
			(!GNextManagerFullScanAt || now >= GNextManagerFullScanAt);
		const bool growthCheckDue =
			GManagerScanKind == EManagerScanKind::None &&
			!fullScanDue && now >= GNextManagerGrowthCheckAt;
		if (GManagerScanKind == EManagerScanKind::None &&
			!fullScanDue && !growthCheckDue)
		{
			return;
		}

		const int objectCount = GetObjectCount();
		if (objectCount <= 0)
		{
			GNextManagerScanAt = now + kManagerScanRetryMs;
			LogUnavailable(now, "object-array-unavailable");
			return;
		}
		if (fullScanDue)
		{
			GManagerScanIndex = objectCount - 1;
			GManagerScanFloorIndex = 0;
			GManagerScanSnapshotObjectCount = objectCount;
			GManagerScanKind = EManagerScanKind::Full;
			GNextManagerFullScanAt = 0;
		}
		else if (GManagerScanKind == EManagerScanKind::None)
		{
			GNextManagerGrowthCheckAt =
				now + kManagerObjectGrowthCheckMs;
			if (objectCount <= GLastCompletedManagerScanObjectCount)
				return;
			GManagerScanIndex = objectCount - 1;
			GManagerScanFloorIndex = GLastCompletedManagerScanObjectCount;
			GManagerScanSnapshotObjectCount = objectCount;
			GManagerScanKind = EManagerScanKind::Appended;
		}
		else if (GManagerScanIndex < GManagerScanFloorIndex ||
			GManagerScanIndex >= objectCount)
		{
			GManagerScanIndex = objectCount - 1;
			GManagerScanFloorIndex = 0;
			GManagerScanSnapshotObjectCount = objectCount;
			GManagerScanKind = EManagerScanKind::Full;
			GNextManagerFullScanAt = 0;
		}

		const int endExclusive = (std::max)(
			GManagerScanFloorIndex - 1,
			GManagerScanIndex - kManagerObjectsPerTick);
		for (int index = GManagerScanIndex;
			index > endExclusive; --index)
		{
			const UObject* candidate = GetManagerCandidate(
				index, GManagerClass);
			if (!candidate)
				continue;
			auto mutableCandidate = const_cast<UObject*>(candidate);
			UFunction* startHotfixProcess =
				FindStartHotfixProcess(mutableCandidate);
			if (!IsLiveObject(startHotfixProcess) ||
				!HasZeroParameters(startHotfixProcess))
			{
				LogUnavailable(now, "invalid-start-hotfix-process");
				continue;
			}
			const uint64 managerIdentity =
				GetObjectIdentity(mutableCandidate);
			if (!managerIdentity)
				continue;
			if (GInvocationFaultManagerIdentity != managerIdentity)
			{
				GInvocationFaultManagerIdentity = managerIdentity;
				GInvocationFaultCount = 0;
			}
			GManager = mutableCandidate;
			GStartHotfixProcess = startHotfixProcess;
			GCurrentManagerIdentity.store(
				managerIdentity, std::memory_order_release);
			EnsureManagerIniHook(GManager, GManagerClass);
			GManagerScanIndex = index - 1;
			ResetManagerScanBackoff();
			GNextUnavailableLogAt = 0;
			AtlasDiagnostics::WriteLine(
				"hotfix-reload manager-resolved manager=%p "
				"function=%p index=%d",
				GManager, GStartHotfixProcess, index);
			return;
		}

		GManagerScanIndex = endExclusive;
		if (GManagerScanIndex >= GManagerScanFloorIndex)
			return;
		const EManagerScanKind completedScanKind = GManagerScanKind;
		const int completedObjectCount =
			GManagerScanSnapshotObjectCount > 0
				? GManagerScanSnapshotObjectCount
				: objectCount;
		GLastCompletedManagerScanObjectCount = completedObjectCount;
		GManagerScanIndex = -1;
		GManagerScanFloorIndex = 0;
		GManagerScanSnapshotObjectCount = 0;
		GManagerScanKind = EManagerScanKind::None;
		if (completedScanKind == EManagerScanKind::Full)
		{
			GNextManagerFullScanAt =
				now + ConsumeManagerScanBackoff();
		}
		if (objectCount > completedObjectCount)
		{
			GManagerScanIndex = objectCount - 1;
			GManagerScanFloorIndex = completedObjectCount;
			GManagerScanSnapshotObjectCount = objectCount;
			GManagerScanKind = EManagerScanKind::Appended;
			GNextManagerScanAt = now + 1;
		}
		GNextManagerGrowthCheckAt = now + kManagerObjectGrowthCheckMs;
		LogUnavailable(now, "live-manager-not-found");
	}

	bool InvokeStartHotfixProcess(
		UObject* manager,
		UFunction* function) noexcept
	{
		__try
		{
			manager->ProcessEvent(function, nullptr);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	void HandleInvocationFault(
		ULONGLONG now,
		const char* reason) noexcept
	{
		const uint64 managerIdentity = GetObjectIdentity(GManager);
		if (managerIdentity &&
			GInvocationFaultManagerIdentity != managerIdentity)
		{
			GInvocationFaultManagerIdentity = managerIdentity;
			GInvocationFaultCount = 0;
		}
		if (GInvocationFaultCount < kMaximumInvocationFaults)
			++GInvocationFaultCount;
		GRefreshRecoveryPending = true;
		if (GInvocationFaultCount >= kMaximumInvocationFaults)
		{
			GDisabledForWorld.store(true, std::memory_order_release);
			ResetManager();
			AtlasDiagnostics::WriteLine(
				"hotfix-reload disabled reason=%s faults=%u",
				reason ? reason : "invocation-fault",
				GInvocationFaultCount);
			return;
		}
		const ULONGLONG retryDelay =
			kInvocationRetryMs << (GInvocationFaultCount - 1);
		ResetManager();
		ResetManagerScanBackoff();
		GNextManagerScanAt = now + retryDelay;
		GNextRefreshAt = now + retryDelay;
		AtlasDiagnostics::WriteLine(
			"hotfix-reload invocation-fault reason=%s "
			"faults=%u retry-ms=%llu",
			reason ? reason : "unknown",
			GInvocationFaultCount,
			static_cast<unsigned long long>(retryDelay));
	}

	void TickRefreshCompletion(ULONGLONG now) noexcept
	{
		if (!GRefreshAwaitingCompletion.load(std::memory_order_acquire) ||
			now < GRefreshCompletionDeadlineAt)
		{
			return;
		}
		const uint64 refresh =
			GRefreshSerial.load(std::memory_order_acquire);
		const bool acceptedPayloadAwaitingAssetPass = refresh &&
			GPromotedRefresh.load(std::memory_order_acquire) == refresh;
		if (acceptedPayloadAwaitingAssetPass)
		{
			AtlasDiagnostics::WriteLine(
				"hotfix-reload correlation-recovered "
				"reason=accepted-asset-pass-timeout refresh=%llu",
				static_cast<unsigned long long>(refresh));
		}
		// Leave a small gap before the next asynchronous check. The tombstone and
		// DefaultGame observation (when present) make a late native pass fail open;
		// a later unchanged completion also applies any still-unapplied revision.
		GNoCallbackRecycleManagerIdentity.store(
			GPendingRefreshManagerIdentity.load(std::memory_order_acquire),
			std::memory_order_relaxed);
		GNoCallbackRecycleManagerGeneration.store(
			GPendingRefreshManagerGeneration.load(std::memory_order_acquire),
			std::memory_order_relaxed);
		GNoCallbackRecycleWorldGeneration.store(
			GPendingRefreshWorldGeneration.load(std::memory_order_acquire),
			std::memory_order_relaxed);
		const ULONGLONG recycleDeadline = now + kLateCallbackGuardMs;
		GNoCallbackRecycleRefresh.store(refresh, std::memory_order_release);
		const bool retiredPending = CancelRefresh(refresh);
		GRefreshSerial.store(0, std::memory_order_release);
		GRefreshAwaitingCompletion.store(false, std::memory_order_release);
		if (!retiredPending)
		{
			uint64 expectedRecycle = refresh;
			GNoCallbackRecycleRefresh.compare_exchange_strong(
				expectedRecycle, 0,
				std::memory_order_acq_rel,
				std::memory_order_acquire);
			GNoCallbackRecycleManagerIdentity.store(
				0, std::memory_order_relaxed);
			GNoCallbackRecycleManagerGeneration.store(
				0, std::memory_order_relaxed);
			GNoCallbackRecycleWorldGeneration.store(
				0, std::memory_order_relaxed);
			return;
		}
		GNextRefreshAt = recycleDeadline;
	}

	void Tick(ULONGLONG now)
	{
		if (!GHooksInstalled.load(std::memory_order_acquire) ||
			!GPatchAssetsHookInstalled.load(std::memory_order_acquire))
		{
			if (!GLoggedPeriodicGateUnavailable)
			{
				GLoggedPeriodicGateUnavailable = true;
				AtlasDiagnostics::WriteLine(
					"hotfix-reload periodic-disabled "
					"reason=exact-once-hooks-unavailable");
			}
			GDisabledForWorld.store(true, std::memory_order_release);
			return;
		}
		if (!GStartedAt)
		{
			GStartedAt = now;
			GNextClassResolveAt = now + kDiscoveryDelayMs;
			GNextRefreshAt = now + kStartupGraceMs;
			AtlasDiagnostics::WriteLine(
				"hotfix-reload armed grace-ms=%llu check-ms=%llu "
				"no-callback-settle-ms=%llu",
				static_cast<unsigned long long>(kStartupGraceMs),
				static_cast<unsigned long long>(kRefreshIntervalMs),
				static_cast<unsigned long long>(kNoCallbackSettleMs));
			return;
		}

		TickRefreshCompletion(now);
		ResolveManagerClass(now);
		ResolveManager(now);
		if (GManager && GManagerClass)
			EnsureManagerIniHook(GManager, GManagerClass);
		if (GRefreshAwaitingCompletion.load(std::memory_order_acquire))
			return;

		const uint64 managerIdentity = GetObjectIdentity(GManager);
		if (managerIdentity &&
			GIniHookRejectedManagerIdentity.load(
				std::memory_order_acquire) == managerIdentity)
		{
			GDisabledForWorld.store(true, std::memory_order_release);
			AtlasDiagnostics::WriteLine(
				"hotfix-reload periodic-disabled "
				"reason=manager-ini-hook-rejected");
			return;
		}
		if (!managerIdentity ||
			!GIniFileHookValidated.load(std::memory_order_acquire) ||
			GIniHookValidatedManagerIdentity.load(
				std::memory_order_acquire) != managerIdentity)
		{
			return;
		}
		if (now < GNextRefreshAt)
			return;
		if (!GManager || !GStartHotfixProcess)
		{
			LogUnavailable(now, "refresh-target-unavailable");
			return;
		}
		if (!IsUsableManager(GManager, GManagerClass) ||
			!IsLiveObject(GStartHotfixProcess) ||
			FindStartHotfixProcess(GManager) != GStartHotfixProcess)
		{
			ResetManager();
			GNextManagerScanAt = now + kManagerScanRetryMs;
			LogUnavailable(now, "refresh-target-stale");
			return;
		}
		if (GInvocationActive.load(std::memory_order_acquire))
			return;

		// Retain the expired-request tombstone through every validation return.
		// Clear it only on the game thread immediately before the successor arm,
		// leaving no callback-attribution gap.
		GNoCallbackRecycleRefresh.store(0, std::memory_order_release);
		GNoCallbackRecycleManagerIdentity.store(0, std::memory_order_relaxed);
		GNoCallbackRecycleManagerGeneration.store(
			0, std::memory_order_relaxed);
		GNoCallbackRecycleWorldGeneration.store(
			0, std::memory_order_relaxed);
		const uint64 refresh = ArmRefresh(GManager, now);
		if (!refresh)
		{
			GRefreshRecoveryPending = true;
			GNextManagerScanAt = now + kManagerScanRetryMs;
			GNextRefreshAt = now + kManagerScanRetryMs;
			AtlasDiagnostics::WriteLine(
				"hotfix-reload refresh-retry-scheduled "
				"reason=arm-failed retry-ms=%llu",
				static_cast<unsigned long long>(kManagerScanRetryMs));
			return;
		}
		GRefreshSerial.store(refresh, std::memory_order_release);
		GRefreshAwaitingCompletion.store(true, std::memory_order_release);
		GRefreshCompletionDeadlineAt = now + kNoCallbackSettleMs;
		GNextRefreshAt = now + kRefreshIntervalMs;
		if (GDisabledForWorld.load(std::memory_order_acquire))
		{
			CancelRefresh(refresh);
			GRefreshSerial.store(0, std::memory_order_release);
			GRefreshAwaitingCompletion.store(
				false, std::memory_order_release);
			return;
		}

		bool started = false;
		{
			FScopedStartHotfixInvocation startScope(refresh);
			GInvocationActive.store(true, std::memory_order_release);
			started = InvokeStartHotfixProcess(
				GManager, GStartHotfixProcess);
			GInvocationActive.store(false, std::memory_order_release);
		}
		if (!started)
		{
			CancelRefresh(refresh);
			uint64 expectedRefresh = refresh;
			GRefreshSerial.compare_exchange_strong(
				expectedRefresh, 0,
				std::memory_order_acq_rel,
				std::memory_order_acquire);
			GRefreshAwaitingCompletion.store(
				false, std::memory_order_release);
			HandleInvocationFault(now, "refresh-invocation-failed");
			return;
		}
		GInvocationFaultCount = 0;
		GInvocationFaultManagerIdentity = GetObjectIdentity(GManager);
		if (!GLoggedFirstRefresh)
		{
			GLoggedFirstRefresh = true;
			GRefreshRecoveryPending = false;
			AtlasDiagnostics::WriteLine(
				"hotfix-reload first-check-started manager=%p",
				GManager);
		}
		else if (GRefreshRecoveryPending)
		{
			GRefreshRecoveryPending = false;
			AtlasDiagnostics::WriteLine(
				"hotfix-reload refresh-recovered manager=%p",
				GManager);
		}
	}

}

bool InstallEarlyHook() noexcept
{
	FExclusiveSrwLockGuard installLock(GHookInstallMutex);
	if (GHooksInstalled.load(std::memory_order_acquire))
		return true;
	if (GHookInstallAttempted)
		return false;
	GHookInstallAttempted = true;

	bool iniCreated = false;
	bool patchCreated = false;
	bool iniEnabled = false;
	try
	{
		ResolveNativeHotfixTargets();
		if (!IsExecutableAddress(GBaseIniTarget) ||
			!IsExecutableAddress(GPatchAssetsTarget) ||
			GBaseIniTarget == GPatchAssetsTarget)
		{
			AtlasDiagnostics::WriteLine(
				"hotfix-reload hooks-unavailable ini-target=%p "
				"asset-pass-target=%p asset-pass-scheme=%s",
				GBaseIniTarget,
				GPatchAssetsTarget,
				GPatchAssetsResolutionDiagnostic);
			return false;
		}

		const MH_STATUS initializeStatus = MH_Initialize();
		if (initializeStatus != MH_OK &&
			initializeStatus != MH_ERROR_ALREADY_INITIALIZED)
		{
			AtlasDiagnostics::WriteLine(
				"hotfix-reload hooks-unavailable reason=minhook-init "
				"status=%d",
				static_cast<int>(initializeStatus));
			return false;
		}

		const MH_STATUS iniCreateStatus = MH_CreateHook(
			GBaseIniTarget,
			reinterpret_cast<LPVOID>(BaseIniDetour),
			reinterpret_cast<LPVOID*>(&GBaseIniOriginal));
		iniCreated = iniCreateStatus == MH_OK && GBaseIniOriginal;
		if (!iniCreated)
		{
			GBaseIniOriginal = nullptr;
			AtlasDiagnostics::WriteLine(
				"hotfix-reload hooks-unavailable reason=ini-create "
				"status=%d",
				static_cast<int>(iniCreateStatus));
			return false;
		}

		const MH_STATUS patchCreateStatus = MH_CreateHook(
			GPatchAssetsTarget,
			reinterpret_cast<LPVOID>(PatchAssetsDetour),
			reinterpret_cast<LPVOID*>(&GPatchAssetsOriginal));
		patchCreated = patchCreateStatus == MH_OK && GPatchAssetsOriginal;
		if (!patchCreated)
		{
			MH_RemoveHook(GBaseIniTarget);
			iniCreated = false;
			GBaseIniOriginal = nullptr;
			GPatchAssetsOriginal = nullptr;
			AtlasDiagnostics::WriteLine(
				"hotfix-reload hooks-unavailable reason=asset-pass-create "
				"status=%d",
				static_cast<int>(patchCreateStatus));
			return false;
		}

		const MH_STATUS iniEnableStatus = MH_EnableHook(GBaseIniTarget);
		iniEnabled = iniEnableStatus == MH_OK ||
			iniEnableStatus == MH_ERROR_ENABLED;
		if (!iniEnabled)
		{
			MH_RemoveHook(GPatchAssetsTarget);
			MH_RemoveHook(GBaseIniTarget);
			patchCreated = false;
			iniCreated = false;
			GBaseIniOriginal = nullptr;
			GPatchAssetsOriginal = nullptr;
			AtlasDiagnostics::WriteLine(
				"hotfix-reload hooks-unavailable reason=ini-enable "
				"status=%d",
				static_cast<int>(iniEnableStatus));
			return false;
		}

		const MH_STATUS patchEnableStatus =
			MH_EnableHook(GPatchAssetsTarget);
		if (patchEnableStatus != MH_OK &&
			patchEnableStatus != MH_ERROR_ENABLED)
		{
			MH_DisableHook(GBaseIniTarget);
			MH_RemoveHook(GPatchAssetsTarget);
			MH_RemoveHook(GBaseIniTarget);
			iniEnabled = false;
			patchCreated = false;
			iniCreated = false;
			GBaseIniOriginal = nullptr;
			GPatchAssetsOriginal = nullptr;
			AtlasDiagnostics::WriteLine(
				"hotfix-reload hooks-unavailable reason=asset-pass-enable "
				"status=%d",
				static_cast<int>(patchEnableStatus));
			return false;
		}

		GBaseIniHookEnabled = true;
		GBasePatchAssetsHookEnabled = true;
		GPatchAssetsHookInstalled.store(true, std::memory_order_release);
		GHooksInstalled.store(true, std::memory_order_release);
		AtlasDiagnostics::WriteLine(
			"hotfix-reload hooks-installed ini-target=%p "
			"asset-pass-target=%p asset-pass-scheme=%s row-hook=disabled",
			GBaseIniTarget,
			GPatchAssetsTarget,
			GPatchAssetsResolutionDiagnostic);
		return true;
	}
	catch (...)
	{
		if (patchCreated && GPatchAssetsTarget)
		{
			MH_DisableHook(GPatchAssetsTarget);
			MH_RemoveHook(GPatchAssetsTarget);
		}
		if (iniCreated && GBaseIniTarget)
		{
			if (iniEnabled)
				MH_DisableHook(GBaseIniTarget);
			MH_RemoveHook(GBaseIniTarget);
		}
		GBaseIniHookEnabled = false;
		GBasePatchAssetsHookEnabled = false;
		GIniFileHookValidated.store(false, std::memory_order_release);
		GPatchAssetsHookInstalled.store(false, std::memory_order_release);
		GHooksInstalled.store(false, std::memory_order_release);
		GBaseIniOriginal = nullptr;
		GPatchAssetsOriginal = nullptr;
		AtlasDiagnostics::WriteLine(
			"hotfix-reload hooks-unavailable reason=cpp-exception");
		return false;
	}
}

void GameThreadTick(const SDK::UWorld* world) noexcept
{
	const DWORD currentThreadId = GetCurrentThreadId();
	DWORD ownerThreadId = GGameThreadId.load(std::memory_order_acquire);
	if (!ownerThreadId)
	{
		DWORD expected = 0;
		if (GGameThreadId.compare_exchange_strong(
				expected, currentThreadId,
				std::memory_order_acq_rel,
				std::memory_order_acquire))
		{
			ownerThreadId = currentThreadId;
			AtlasDiagnostics::WriteLine(
				"hotfix-reload game-thread-bound tid=%lu",
				currentThreadId);
		}
		else
		{
			ownerThreadId = expected;
		}
	}
	if (ownerThreadId != currentThreadId)
	{
		if (!GLoggedThreadMismatch.exchange(true, std::memory_order_acq_rel))
		{
			AtlasDiagnostics::WriteLine(
				"hotfix-reload ignored-non-game-thread "
				"owner-tid=%lu call-tid=%lu",
				ownerThreadId, currentThreadId);
		}
		return;
	}

	const ULONGLONG now = GetTickCount64();
	const uint64 worldIdentity = GetObjectIdentity(world);
	if (!GObservedWorldInitialized)
	{
		AdvanceLifecycleGeneration(GWorldGeneration);
		GObservedWorld = world;
		GObservedWorldIdentity = worldIdentity;
		GObservedWorldInitialized = true;
	}
	else if (world != GObservedWorld ||
		worldIdentity != GObservedWorldIdentity)
	{
		const UWorld* previousWorld = GObservedWorld;
		const uint64 previousWorldIdentity = GObservedWorldIdentity;
		AdvanceLifecycleGeneration(GWorldGeneration);
		GObservedWorld = world;
		GObservedWorldIdentity = worldIdentity;
		ResetManager();
		ResetManagerScanBackoff();
		GInvocationFaultCount = 0;
		GInvocationFaultManagerIdentity = 0;
		GDisabledForWorld.store(false, std::memory_order_release);
		GNextManagerScanAt = now + kWorldTransitionDelayMs;
		GNextRefreshAt = now + kWorldTransitionDelayMs;
		AtlasDiagnostics::WriteLine(
			"hotfix-reload world-changed old=%p old-id=%llu "
			"new=%p new-id=%llu",
			previousWorld,
			static_cast<unsigned long long>(previousWorldIdentity),
			GObservedWorld,
			static_cast<unsigned long long>(worldIdentity));
	}

	if (!worldIdentity ||
		GDisabledForWorld.load(std::memory_order_acquire))
		return;
	try
	{
		Tick(now);
	}
	catch (...)
	{
		GInvocationActive.store(false, std::memory_order_release);
		ResetManager();
		GNextManagerScanAt = now + kInvocationRetryMs;
		GNextRefreshAt = now + kInvocationRetryMs;
		LogUnavailable(now, "cpp-exception");
	}
}
}
