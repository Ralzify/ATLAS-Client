#include "pch.h"

#include "../Public/LoadoutTelemetry.h"
#include "../Public/FortPlayerControllerAthena.h"
#include "../Public/Diagnostics.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <limits>

namespace AtlasLoadoutTelemetry
{
namespace
{
	constexpr char kHelloPrefix[] = "mgloadout:v1:h:";
	constexpr wchar_t kAckPrefix[] = L"mgloadout:v1:a:";
	constexpr char kReportPrefix[] = "mgloadout:v1:r:";
	constexpr size_t kPrefixCharacters = 15;
	constexpr size_t kSessionCharacters = 16;
	constexpr size_t kSequenceCharacters = 8;
	constexpr size_t kSlotCount = 5;
	constexpr size_t kHelloCharacters =
		kPrefixCharacters + kSessionCharacters;
	constexpr size_t kAckCharacters = kHelloCharacters;
	constexpr size_t kGuidCharacters = 32;
	constexpr size_t kReportedQuickbarCapacity = 10;
	constexpr size_t kReportCharacters =
		kPrefixCharacters + kSessionCharacters + 1 +
		kSequenceCharacters + kSlotCount * (1 + kGuidCharacters);

	// PrimaryQuickBarSlotItemGuids is a fixed 0xa4-byte POD read on the
	// releases that expose it (including FN 30). Poll that source quickly so a
	// client-side drag is visible to the server without waiting close to a
	// second. Older fallback sources still use the conservative cadence below
	// because they may walk the inventory or call quickbar getters.
	// The telemetry pump itself is frame-quantized (roughly 33 ms on the
	// common FN 30 client path). A 50 ms gate therefore only sampled every
	// other pump and made the required two-sample stability check take up to
	// four frames. Keep two independent samples, but permit one per pump.
	constexpr ULONGLONG kReportedProbeIntervalMs = 30;
	constexpr ULONGLONG kFallbackProbeIntervalMs = 200;
	constexpr ULONGLONG kDebounceMs = 30;
	// Keep the sender no faster than Magnesium's controller-bound report
	// limiter. The receiver deliberately has additional jitter margin because
	// this unacknowledged transport cannot observe a rate-limited packet.
	constexpr ULONGLONG kMinimumSendIntervalMs = 100;
	// ServerCheat is a best-effort, unacknowledged report transport. Send one
	// bounded confirmation copy after a changed map so a transient bridge-lock
	// collision or a coalesced network frame cannot hide the swap until the
	// ordinary heartbeat. The global minimum-send gate still caps traffic at
	// 10 Hz, and a stable loadout incurs only this one extra packet.
	constexpr ULONGLONG kChangedReportConfirmationMs = 200;
	constexpr ULONGLONG kHeartbeatIntervalMs = 1000;
	constexpr ULONGLONG kSchemaRetryMs = 2000;
	constexpr ULONGLONG kResolveDiagnosticIntervalMs = 5000;
	// A server can deliberately drop a hello while its non-blocking bridge lock
	// is busy. Keep negotiation bounded, but leave more than one second between
	// wire attempts so an unsupported server still sees only a tiny fixed burst.
	constexpr ULONGLONG kHelloRetryIntervalMs = 1500;
	constexpr unsigned int kMaximumHelloAttempts = 3;
	constexpr unsigned int kMaximumGuardedFaults = 3;
	constexpr int32 kMaximumQuickbarSlots = 64;
	constexpr int32 kMaximumSlotItems = 16;
	constexpr int32 kMaximumInventoryEntries = 512;

	constexpr uint64 kCpfParm = 0x80;
	constexpr uint64 kCpfOutParm = 0x100;
	constexpr uint64 kCpfReturnParm = 0x400;
	constexpr uint64 kCastEnum = 0x4;
	constexpr uint64 kCastStruct = 0x10;
	constexpr uint64 kCastClass = 0x20;
	constexpr uint32 kInvalidOffset = UINT32_MAX;

	static_assert(sizeof(void*) == 8);
	static_assert(kHelloCharacters == 31);
	static_assert(kReportCharacters == 205);
	static_assert(kHelloRetryIntervalMs > 1000);
	static_assert(kMaximumHelloAttempts == 3);
	static_assert(
		kChangedReportConfirmationMs >= kMinimumSendIntervalMs);

	struct FBridgeGuid
	{
		uint32 A = 0;
		uint32 B = 0;
		uint32 C = 0;
		uint32 D = 0;
	};

	struct FRawArray
	{
		void* Data = nullptr;
		int32 Num = 0;
		int32 Max = 0;
	};

	static_assert(sizeof(FBridgeGuid) == 16);
	static_assert(sizeof(FRawArray) == sizeof(TArray<uint8>));

	struct FPropertyLayout
	{
		uint32 Offset = kInvalidOffset;
		uint32 ElementSize = 0;

		bool IsValid() const
		{
			return Offset != kInvalidOffset && ElementSize != 0;
		}
	};

	struct FControllerLayout
	{
		const UClass* Class = nullptr;
		FPropertyLayout WorldInventory{};
		FPropertyLayout ClientQuickBars{};
		FPropertyLayout QuickBars{};
		FPropertyLayout ReportedQuickbar{};
		bool Valid = false;
	};

	struct FInventoryLayout
	{
		const UClass* Class = nullptr;
		const UClass* ExpectedClass = nullptr;
		const UStruct* ItemListStruct = nullptr;
		const UStruct* ItemEntryStruct = nullptr;
		FPropertyLayout Inventory{};
		FPropertyLayout ReplicatedEntries{};
		FPropertyLayout ItemGuid{};
		FPropertyLayout ItemDefinition{};
		int32 EntrySize = 0;
		bool Valid = false;
	};

	struct FQuickbarLayout
	{
		const UClass* Class = nullptr;
		const UClass* ExpectedClass = nullptr;
		const UStruct* QuickbarStruct = nullptr;
		const UStruct* SlotStruct = nullptr;
		FPropertyLayout Owner{};
		FPropertyLayout PrimaryQuickbar{};
		FPropertyLayout Slots{};
		FPropertyLayout Items{};
		int32 SlotSize = 0;
		bool Valid = false;
	};

	struct FWorldItemLayout
	{
		const UClass* Class = nullptr;
		const UClass* ExpectedClass = nullptr;
		FPropertyLayout ItemEntry{};
		bool Valid = false;
	};

	struct FFunctionSchema
	{
		UFunction* Function = nullptr;
		uint32 ParamsSize = 0;
		uint32 MessageOffset = kInvalidOffset;
	};

	struct FGetterSchema
	{
		UFunction* Function = nullptr;
		uint32 ParamsSize = 0;
		uint32 QuickbarTypeOffset = kInvalidOffset;
		uint32 QuickbarTypeSize = 0;
		uint32 SlotOffset = kInvalidOffset;
		uint32 ReturnOffset = kInvalidOffset;
	};

	struct FSlotSnapshot
	{
		std::array<FBridgeGuid, kSlotCount> Slots{};
	};

	// Client quickbar models from the first post-7.40 implementation through
	// current releases expose this small POD snapshot.  Reading it avoids a
	// complete inventory/object walk on the normal telemetry path.  The server
	// still maps every reported GUID through its authoritative inventory before
	// accepting the snapshot or enabling an edit.
	struct FReportedQuickbarGuidState
	{
		FBridgeGuid EquippedItemGuids[kReportedQuickbarCapacity]{};
		int32 NumEnabledSlots = 0;
	};
	static_assert(sizeof(FReportedQuickbarGuidState) == 0xa4);

	struct FInventoryGuids
	{
		struct FEntry
		{
			FBridgeGuid Guid{};
			UObject* Definition = nullptr;
		};

		std::array<FEntry, kMaximumInventoryEntries> Values{};
		int32 Count = 0;
	};

	struct FLayoutRetry
	{
		const UClass* Class = nullptr;
		ULONGLONG NextAttemptAt = 0;
	};

	template <typename TObject>
	struct FResidentLookup
	{
		const TObject* Value = nullptr;
		ULONGLONG NextAttemptAt = 0;
	};

	enum class EResidentClass : size_t
	{
		FortPlayerControllerAthena,
		FortInventory,
		FortQuickBars,
		FortWorldItem,
		FortWeaponRangedItemDefinition,
		FortGadgetItemDefinition,
		FortConsumableItemDefinition,
		Count
	};

	constexpr const wchar_t* kResidentClassPaths[] = {
		L"/Script/FortniteGame.FortPlayerControllerAthena",
		L"/Script/FortniteGame.FortInventory",
		L"/Script/FortniteGame.FortQuickBars",
		L"/Script/FortniteGame.FortWorldItem",
		L"/Script/FortniteGame.FortWeaponRangedItemDefinition",
		L"/Script/FortniteGame.FortGadgetItemDefinition",
		L"/Script/FortniteGame.FortConsumableItemDefinition"
	};
	static_assert(
		sizeof(kResidentClassPaths) / sizeof(kResidentClassPaths[0]) ==
			static_cast<size_t>(EResidentClass::Count));

	enum class EResidentStruct : size_t
	{
		FortItemList,
		FortItemEntry,
		QuickBar,
		QuickBarSlot,
		Count
	};

	constexpr const wchar_t* kResidentStructPaths[] = {
		L"/Script/FortniteGame.FortItemList",
		L"/Script/FortniteGame.FortItemEntry",
		L"/Script/FortniteGame.QuickBar",
		L"/Script/FortniteGame.QuickBarSlot"
	};
	static_assert(
		sizeof(kResidentStructPaths) / sizeof(kResidentStructPaths[0]) ==
			static_cast<size_t>(EResidentStruct::Count));
	constexpr wchar_t kResidentItemTypeEnumPath[] =
		L"/Script/FortniteGame.EFortItemType";

	struct FItemTypeLayoutCacheEntry
	{
		const UClass* Class = nullptr;
		FPropertyLayout ItemType{};
	};

	constexpr size_t kItemTypeLayoutCacheSize = 16;
	constexpr size_t kPrimaryDefinitionClassCount = 3;
	constexpr size_t kPrimaryItemTypeCount = 6;

	enum class ESnapshotSource : uint8
	{
		None,
		ReportedQuickbar,
		ClientQuickBars,
		QuickBars,
		Getters
	};

	enum class EDefinitionKind : uint8
	{
		Unknown,
		Harvest,
		Primary
	};

	const char* SnapshotSourceName(ESnapshotSource source)
	{
		switch (source)
		{
		case ESnapshotSource::ReportedQuickbar:
			return "reported-property";
		case ESnapshotSource::ClientQuickBars:
			return "client-quickbars";
		case ESnapshotSource::QuickBars:
			return "quickbars";
		case ESnapshotSource::Getters:
			return "getters";
		default:
			return "none";
		}
	}

	struct FState
	{
		unsigned int FaultCount = 0;
		uint64 Session = 0;
		uint32 Sequence = 0;
		uint64 WorldIdentity = 0;
		uint64 ControllerIdentity = 0;
		ULONGLONG NextProbeAt = 0;
		ULONGLONG NextSchemaAttemptAt = 0;
		ULONGLONG NextHelloAt = 0;
		ULONGLONG CandidateSince = 0;
		ULONGLONG LastSendAt = 0;
		ULONGLONG NextChangedReportConfirmationAt = 0;
		ULONGLONG NextResolveDiagnosticAt = 0;
		unsigned int HelloAttempts = 0;
		bool AckLogged = false;
		bool CandidateValid = false;
		bool LastSentValid = false;
		bool ChangedReportConfirmationPending = false;
		bool HasSentNonzero = false;
		FSlotSnapshot Candidate{};
		FSlotSnapshot LastSent{};
		ESnapshotSource LastSource = ESnapshotSource::None;
		const UClass* ServerCheatClass = nullptr;
		FFunctionSchema ServerCheat{};
		FControllerLayout Controller{};
		FInventoryLayout Inventory{};
		FQuickbarLayout Quickbars{};
		FWorldItemLayout WorldItem{};
		const UClass* GetterClass = nullptr;
		ULONGLONG NextGetterSchemaAttemptAt = 0;
		FGetterSchema ItemGetter{};
		FGetterSchema CountGetter{};
		FLayoutRetry ControllerLayoutRetry{};
		FLayoutRetry InventoryLayoutRetry{};
		FLayoutRetry QuickbarLayoutRetry{};
		FLayoutRetry WorldItemLayoutRetry{};
		std::array<FItemTypeLayoutCacheEntry,
			kItemTypeLayoutCacheSize> ItemTypeLayouts{};
		size_t NextItemTypeLayoutReplacement = 0;
		const UEnum* ItemTypeEnum = nullptr;
		ULONGLONG NextItemTypeEnumAttemptAt = 0;
		int64 HarvestItemType = -1;
		std::array<int64, kPrimaryItemTypeCount> PrimaryItemTypes{};
		bool PrimaryDefinitionClassesResolved = false;
		ULONGLONG NextPrimaryDefinitionClassAttemptAt = 0;
		std::array<const UClass*,
			kPrimaryDefinitionClassCount> PrimaryDefinitionClasses{};
	};

	FState GState{};
	std::atomic_bool GEnabled = false;
	std::atomic_bool GPermanentlyInert = false;
	std::atomic_uint64_t GPublishedSession = 0;
	std::atomic_uint64_t GAcknowledgedSession = 0;
	std::array<FResidentLookup<UClass>,
		static_cast<size_t>(EResidentClass::Count)> GResidentClasses{};
	std::array<FResidentLookup<UStruct>,
		static_cast<size_t>(EResidentStruct::Count)> GResidentStructs{};
	FResidentLookup<UEnum> GResidentItemTypeEnum{};

	bool IsLiveObject(const UObject* object);

	template <typename TObject>
	const TObject* FindResidentObjectOnce(
		FResidentLookup<TObject>& lookup,
		const wchar_t* exactPath,
		uint64 expectedCastFlag)
	{
		if (lookup.Value && IsLiveObject(lookup.Value))
			return lookup.Value;

		const ULONGLONG now = GetTickCount64();
		if (now < lookup.NextAttemptAt)
			return nullptr;

		lookup.Value = nullptr;
		lookup.NextAttemptAt = now + kSchemaRetryMs;
		// StaticFindObject is the SDK's resident-only engine lookup. It does
		// not load a package or walk TUObjectArray in this module. Reflection
		// types can become resident after the local controller first appears,
		// so cache only a success and retry a miss at a small bounded cadence.
		if (exactPath && SDK::Offsets::StaticFindObject)
		{
			const UObject* candidate =
				SDK::StaticFindObject(exactPath, nullptr);
			if (IsLiveObject(candidate) && candidate->Class &&
				(candidate->Class->GetCastFlags() & expectedCastFlag) != 0)
			{
				lookup.Value =
					reinterpret_cast<const TObject*>(candidate);
				lookup.NextAttemptAt = 0;
			}
		}
		return lookup.Value;
	}

	const UClass* FindResidentClassOnce(EResidentClass id)
	{
		auto& lookup = GResidentClasses[static_cast<size_t>(id)];
		return FindResidentObjectOnce(
			lookup,
			kResidentClassPaths[static_cast<size_t>(id)],
			kCastClass);
	}

	const UStruct* FindResidentStructOnce(EResidentStruct id)
	{
		auto& lookup = GResidentStructs[static_cast<size_t>(id)];
		return FindResidentObjectOnce(
			lookup,
			kResidentStructPaths[static_cast<size_t>(id)],
			kCastStruct);
	}

	const UEnum* FindResidentItemTypeEnumOnce()
	{
		return FindResidentObjectOnce(
			GResidentItemTypeEnum,
			kResidentItemTypeEnumPath,
			kCastEnum);
	}

	bool IsReadableProtection(DWORD protection)
	{
		const DWORD access = protection & 0xff;
		return access == PAGE_READONLY ||
			access == PAGE_READWRITE ||
			access == PAGE_WRITECOPY ||
			access == PAGE_EXECUTE_READ ||
			access == PAGE_EXECUTE_READWRITE ||
			access == PAGE_EXECUTE_WRITECOPY;
	}

	bool IsReadableRange(const void* address, size_t size)
	{
		if (!address || size == 0)
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
				&region,
				sizeof(region)) != sizeof(region))
			{
				return false;
			}
			if (region.State != MEM_COMMIT ||
				(region.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0 ||
				!IsReadableProtection(region.Protect))
			{
				return false;
			}

			const uintptr_t regionBegin =
				reinterpret_cast<uintptr_t>(region.BaseAddress);
			if (regionBegin >
				(std::numeric_limits<uintptr_t>::max)() -
				region.RegionSize)
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

	template <typename T>
	bool TryCopyValue(const void* address, T& out)
	{
		out = {};
		if (!IsReadableRange(address, sizeof(T)))
			return false;
		memcpy(&out, address, sizeof(T));
		return true;
	}

	const uint8* AddOffset(const void* base, uint32 offset)
	{
		const uintptr_t value = reinterpret_cast<uintptr_t>(base);
		if (!base || value >
			(std::numeric_limits<uintptr_t>::max)() - offset)
		{
			return nullptr;
		}
		return reinterpret_cast<const uint8*>(value + offset);
	}

	bool IsLiveObject(const UObject* object)
	{
		if (!object || !IsReadableRange(object, sizeof(UObject)))
			return false;

		const int32 index = object->Index;
		if (index < 0 || index >= TUObjectArray::Num())
			return false;
		const FUObjectItem* item = TUObjectArray::GetItemByIndex(index);
		if (!item || !IsReadableRange(item, sizeof(FUObjectItem)) ||
			item->Object != object || (item->Flags & 0x20) != 0)
		{
			return false;
		}
		return object->Class &&
			IsReadableRange(object->Class, sizeof(UClass));
	}

	uint64 GetObjectIdentity(const UObject* object)
	{
		if (!IsLiveObject(object))
			return 0;
		const FUObjectItem* item =
			TUObjectArray::GetItemByIndex(object->Index);
		if (!item)
			return 0;
		return
			(static_cast<uint64>(
				static_cast<uint32>(object->Index)) << 32) |
			static_cast<uint32>(item->SerialNumber);
	}

	bool TryResolveProperty(
		const UStruct* owner,
		const char* name,
		FPropertyLayout& out)
	{
		out = {};
		if (!owner || !name || !Offsets::Offset_Internal ||
			!Offsets::ElementSize || !Offsets::PropertiesSize)
		{
			return false;
		}

		const UField* property = owner->GetProperty(name);
		if (!property)
			return false;
		const size_t metadataSize = (std::max)(
			static_cast<size_t>(Offsets::Offset_Internal) +
				sizeof(uint32),
			static_cast<size_t>(Offsets::ElementSize) +
				sizeof(uint32));
		if (metadataSize > 0x400 ||
			!IsReadableRange(property, metadataSize))
		{
			return false;
		}

		const uint32 offset = GetFromOffset<uint32>(
			property, Offsets::Offset_Internal);
		const uint32 elementSize = GetFromOffset<uint32>(
			property, Offsets::ElementSize);
		const int32 ownerSize = owner->GetPropertiesSize();
		if (offset == kInvalidOffset || elementSize == 0 ||
			offset > 0x20000 || elementSize > 0x10000 ||
			ownerSize <= 0 || ownerSize > 0x40000 ||
			offset > static_cast<uint32>(ownerSize) ||
			elementSize > static_cast<uint32>(ownerSize) - offset)
		{
			return false;
		}

		out.Offset = offset;
		out.ElementSize = elementSize;
		return true;
	}

	bool TryReadArray(
		const void* address,
		size_t elementSize,
		int32 maximumCount,
		FRawArray& out)
	{
		out = {};
		if (!elementSize || elementSize > 0x4000 ||
			maximumCount < 0 || !TryCopyValue(address, out) ||
			out.Num < 0 || out.Max < out.Num ||
			out.Max > maximumCount)
		{
			out = {};
			return false;
		}
		if (out.Num == 0)
			return true;
		const size_t count = static_cast<size_t>(out.Num);
		if (!out.Data || count >
			(std::numeric_limits<size_t>::max)() / elementSize ||
			!IsReadableRange(out.Data, count * elementSize))
		{
			out = {};
			return false;
		}
		return true;
	}

	bool IsZero(const FBridgeGuid& guid)
	{
		return guid.A == 0 && guid.B == 0 &&
			guid.C == 0 && guid.D == 0;
	}

	bool GuidEquals(const FBridgeGuid& left, const FBridgeGuid& right)
	{
		return left.A == right.A && left.B == right.B &&
			left.C == right.C && left.D == right.D;
	}

	bool SnapshotEquals(
		const FSlotSnapshot& left,
		const FSlotSnapshot& right)
	{
		for (size_t index = 0; index < kSlotCount; ++index)
		{
			if (!GuidEquals(left.Slots[index], right.Slots[index]))
				return false;
		}
		return true;
	}

	bool SnapshotHasAnyGuid(const FSlotSnapshot& snapshot)
	{
		for (const FBridgeGuid& guid : snapshot.Slots)
		{
			if (!IsZero(guid))
				return true;
		}
		return false;
	}

	bool HasUniqueGuids(const FSlotSnapshot& snapshot)
	{
		for (size_t left = 0; left < kSlotCount; ++left)
		{
			if (IsZero(snapshot.Slots[left]))
				continue;
			for (size_t right = left + 1; right < kSlotCount; ++right)
			{
				if (GuidEquals(
					snapshot.Slots[left], snapshot.Slots[right]))
				{
					return false;
				}
			}
		}
		return true;
	}

	void ClearReflectionCaches()
	{
		GState.Controller = {};
		GState.Inventory = {};
		GState.Quickbars = {};
		GState.WorldItem = {};
		GState.ControllerLayoutRetry = {};
		GState.InventoryLayoutRetry = {};
		GState.QuickbarLayoutRetry = {};
		GState.WorldItemLayoutRetry = {};
		GState.GetterClass = nullptr;
		GState.NextGetterSchemaAttemptAt = 0;
		GState.ItemGetter = {};
		GState.CountGetter = {};
		GState.ItemTypeLayouts = {};
		GState.NextItemTypeLayoutReplacement = 0;
		GState.ItemTypeEnum = nullptr;
		GState.NextItemTypeEnumAttemptAt = 0;
		GState.HarvestItemType = -1;
		GState.PrimaryItemTypes.fill(-1);
		GState.PrimaryDefinitionClassesResolved = false;
		GState.NextPrimaryDefinitionClassAttemptAt = 0;
		GState.PrimaryDefinitionClasses = {};
	}

	void RegisterGuardedFault(const char* stage)
	{
		if (GPermanentlyInert.load(std::memory_order_acquire))
			return;

		GState.CandidateValid = false;
		GState.CandidateSince = 0;
		GState.NextProbeAt = GetTickCount64() + 1000;
		++GState.FaultCount;
		AtlasDiagnostics::WriteLine(
			"loadout-telemetry guarded-fault stage=%s count=%u",
			stage ? stage : "unknown", GState.FaultCount);
		if (GState.FaultCount < kMaximumGuardedFaults)
			return;

		GPermanentlyInert.store(true, std::memory_order_release);
		GPublishedSession.store(0, std::memory_order_release);
		GAcknowledgedSession.store(0, std::memory_order_release);
		AtlasDiagnostics::WriteLine(
			"loadout-telemetry permanently-inert after guarded faults");
	}

	uint64 Mix64(uint64 value)
	{
		value += 0x9e3779b97f4a7c15ULL;
		value = (value ^ (value >> 30)) *
			0xbf58476d1ce4e5b9ULL;
		value = (value ^ (value >> 27)) *
			0x94d049bb133111ebULL;
		return value ^ (value >> 31);
	}

	uint64 CreateSession(uint64 worldIdentity, uint64 controllerIdentity)
	{
		LARGE_INTEGER counter{};
		QueryPerformanceCounter(&counter);
		uint64 seed = static_cast<uint64>(counter.QuadPart) ^
			(static_cast<uint64>(GetCurrentProcessId()) << 32) ^
			GetTickCount64() ^ worldIdentity ^
			(controllerIdentity << 1) ^
			reinterpret_cast<uintptr_t>(&GState) ^
			Memcury::PE::GetModuleBase();
		const uint64 session = Mix64(seed);
		return session ? session : 1;
	}

	void ResetForController(
		uint64 worldIdentity,
		uint64 controllerIdentity)
	{
		GState.WorldIdentity = worldIdentity;
		GState.ControllerIdentity = controllerIdentity;
		GState.Sequence = 0;
		GState.NextSchemaAttemptAt = 0;
		GState.NextHelloAt = 0;
		GState.HelloAttempts = 0;
		GState.AckLogged = false;
		GState.CandidateSince = 0;
		GState.LastSendAt = 0;
		GState.NextChangedReportConfirmationAt = 0;
		GState.NextResolveDiagnosticAt = 0;
		GState.CandidateValid = false;
		GState.LastSentValid = false;
		GState.ChangedReportConfirmationPending = false;
		GState.HasSentNonzero = false;
		GState.LastSource = ESnapshotSource::None;
		GState.ServerCheatClass = nullptr;
		GState.ServerCheat = {};
		ClearReflectionCaches();

		GAcknowledgedSession.store(0, std::memory_order_release);
		if (worldIdentity && controllerIdentity)
		{
			GState.Session =
				CreateSession(worldIdentity, controllerIdentity);
			GPublishedSession.store(
				GState.Session, std::memory_order_release);
			AtlasDiagnostics::WriteLine(
				"loadout-telemetry session-started session=%016llx",
				static_cast<unsigned long long>(GState.Session));
		}
		else
		{
			GState.Session = 0;
			GPublishedSession.store(0, std::memory_order_release);
		}
	}

	bool CanAttemptLayout(
		const FLayoutRetry& retry,
		const UClass* candidate)
	{
		return retry.Class != candidate ||
			GetTickCount64() >= retry.NextAttemptAt;
	}

	void FailLayoutClosed(
		FLayoutRetry& retry,
		const UClass* failedClass)
	{
		retry.Class = failedClass;
		// A class can be present before all reflected fields and dependent
		// structs are resident.  Fail this probe closed, then retry at the same
		// bounded schema cadence instead of disabling the source for the match.
		retry.NextAttemptAt = GetTickCount64() + kSchemaRetryMs;
	}

	bool ResolveControllerLayout(
		AFortPlayerControllerAthena* controller)
	{
		if (!controller || !controller->Class)
			return false;
		if (GState.Controller.Valid &&
			GState.Controller.Class == controller->Class)
		{
			const bool complete =
				GState.Controller.WorldInventory.IsValid() &&
				(GState.Controller.ReportedQuickbar.IsValid() ||
				 GState.Controller.ClientQuickBars.IsValid() ||
				 GState.Controller.QuickBars.IsValid());
			if (complete || !CanAttemptLayout(
					GState.ControllerLayoutRetry, controller->Class))
			{
				return true;
			}
		}
		if (!CanAttemptLayout(
			GState.ControllerLayoutRetry, controller->Class))
		{
			return false;
		}

		FControllerLayout layout{};
		layout.Class = controller->Class;
		TryResolveProperty(
			controller->Class,
			"WorldInventory",
			layout.WorldInventory);
		TryResolveProperty(
			controller->Class,
			"ClientQuickBars",
			layout.ClientQuickBars);
		TryResolveProperty(
			controller->Class,
			"QuickBars",
			layout.QuickBars);
		TryResolveProperty(
			controller->Class,
			"PrimaryQuickBarSlotItemGuids",
			layout.ReportedQuickbar);
		// The compact reported state does not require WorldInventory.  Keep a
		// valid controller layout even when that heavier fallback is not ready.
		layout.Valid = true;
		GState.Controller = layout;
		const bool complete = layout.WorldInventory.IsValid() &&
			(layout.ReportedQuickbar.IsValid() ||
			 layout.ClientQuickBars.IsValid() ||
			 layout.QuickBars.IsValid());
		if (complete)
		{
			GState.ControllerLayoutRetry = {};
		}
		else
		{
			GState.ControllerLayoutRetry.Class = controller->Class;
			GState.ControllerLayoutRetry.NextAttemptAt =
				GetTickCount64() + kSchemaRetryMs;
		}
		return true;
	}

	bool ResolveInventoryLayout(UObject* inventory)
	{
		if (!inventory || !inventory->Class)
			return false;
		if (GState.Inventory.Valid &&
			GState.Inventory.Class == inventory->Class)
		{
			return true;
		}
		if (!CanAttemptLayout(
			GState.InventoryLayoutRetry, inventory->Class))
		{
			return false;
		}

		FInventoryLayout layout{};
		layout.Class = inventory->Class;
		layout.ExpectedClass = FindResidentClassOnce(
			EResidentClass::FortInventory);
		layout.ItemListStruct = FindResidentStructOnce(
			EResidentStruct::FortItemList);
		layout.ItemEntryStruct = FindResidentStructOnce(
			EResidentStruct::FortItemEntry);
		if (!layout.ExpectedClass || !layout.ItemListStruct ||
			!layout.ItemEntryStruct ||
			!inventory->IsA(layout.ExpectedClass) ||
			!TryResolveProperty(
				inventory->Class, "Inventory", layout.Inventory) ||
			!TryResolveProperty(
				layout.ItemListStruct,
				"ReplicatedEntries",
				layout.ReplicatedEntries) ||
			layout.ReplicatedEntries.ElementSize < sizeof(FRawArray) ||
			!TryResolveProperty(
				layout.ItemEntryStruct,
				"ItemGuid",
				layout.ItemGuid) ||
			layout.ItemGuid.ElementSize != sizeof(FBridgeGuid))
		{
			FailLayoutClosed(
				GState.InventoryLayoutRetry, inventory->Class);
			return false;
		}

		TryResolveProperty(
			layout.ItemEntryStruct,
			"ItemDefinition",
			layout.ItemDefinition);
		layout.EntrySize = layout.ItemEntryStruct->GetPropertiesSize();
		layout.Valid = layout.EntrySize >= 0x20 &&
			layout.EntrySize <= 0x2000 &&
			layout.ItemGuid.Offset <=
				static_cast<uint32>(layout.EntrySize) &&
			layout.ItemGuid.ElementSize <=
				static_cast<uint32>(layout.EntrySize) -
				layout.ItemGuid.Offset;
		if (layout.ItemDefinition.IsValid() &&
			(layout.ItemDefinition.ElementSize != sizeof(void*) ||
			 layout.ItemDefinition.Offset >
				static_cast<uint32>(layout.EntrySize) ||
			 layout.ItemDefinition.ElementSize >
				static_cast<uint32>(layout.EntrySize) -
					layout.ItemDefinition.Offset))
		{
			layout.ItemDefinition = {};
		}
		if (!layout.Valid)
		{
			FailLayoutClosed(
				GState.InventoryLayoutRetry, inventory->Class);
			return false;
		}
		GState.Inventory = layout;
		GState.InventoryLayoutRetry = {};
		return true;
	}

	bool BuildInventoryGuids(
		AFortPlayerControllerAthena* controller,
		FInventoryGuids& out)
	{
		out = {};
		if (!ResolveControllerLayout(controller) ||
			!GState.Controller.WorldInventory.IsValid() ||
			GState.Controller.WorldInventory.ElementSize < sizeof(void*))
			return false;

		UObject* inventory = nullptr;
		const uint8* inventoryAddress = AddOffset(
			controller, GState.Controller.WorldInventory.Offset);
		if (!TryCopyValue(inventoryAddress, inventory) ||
			!IsLiveObject(inventory) ||
			!ResolveInventoryLayout(inventory))
		{
			return false;
		}

		const uint8* listAddress = AddOffset(
			inventory, GState.Inventory.Inventory.Offset);
		if (!listAddress)
			return false;
		const uint8* entriesAddress = AddOffset(
			listAddress, GState.Inventory.ReplicatedEntries.Offset);
		FRawArray entries{};
		if (!TryReadArray(
			entriesAddress,
			static_cast<size_t>(GState.Inventory.EntrySize),
			kMaximumInventoryEntries,
			entries))
		{
			return false;
		}

		for (int32 index = 0; index < entries.Num; ++index)
		{
			const uint8* entry =
				reinterpret_cast<const uint8*>(entries.Data) +
				static_cast<size_t>(index) *
				static_cast<size_t>(GState.Inventory.EntrySize);
			FInventoryGuids::FEntry copied{};
			// TryReadArray validated this complete replicated-entry range once.
			// TickUnsafe is SEH-guarded, so copying from that validated range
			// avoids hundreds of redundant VirtualQuery calls per probe.
			memcpy(
				&copied.Guid,
				entry + GState.Inventory.ItemGuid.Offset,
				sizeof(copied.Guid));
			if (GState.Inventory.ItemDefinition.IsValid())
			{
				memcpy(
					&copied.Definition,
					entry + GState.Inventory.ItemDefinition.Offset,
					sizeof(copied.Definition));
			}
			if (!IsZero(copied.Guid))
			{
				out.Values[static_cast<size_t>(out.Count++)] = copied;
			}
		}
		return true;
	}

	bool InventoryContains(
		const FInventoryGuids& inventory,
		const FBridgeGuid& guid)
	{
		if (IsZero(guid))
			return true;
		for (int32 index = 0; index < inventory.Count; ++index)
		{
			if (GuidEquals(
				inventory.Values[static_cast<size_t>(index)].Guid, guid))
			{
				return true;
			}
		}
		return false;
	}

	bool TryGetInventoryDefinition(
		const FInventoryGuids& inventory,
		const FBridgeGuid& guid,
		UObject*& outDefinition)
	{
		outDefinition = nullptr;
		if (IsZero(guid))
			return false;

		bool found = false;
		for (int32 index = 0; index < inventory.Count; ++index)
		{
			const auto& entry =
				inventory.Values[static_cast<size_t>(index)];
			if (!GuidEquals(entry.Guid, guid))
				continue;
			if (!entry.Definition || !IsLiveObject(entry.Definition))
				return false;
			if (found && outDefinition != entry.Definition)
				return false;
			outDefinition = entry.Definition;
			found = true;
		}
		return found;
	}

	bool ValidateSnapshot(
		const FSlotSnapshot& snapshot,
		const FInventoryGuids& inventory)
	{
		if (!HasUniqueGuids(snapshot))
			return false;
		for (const FBridgeGuid& guid : snapshot.Slots)
		{
			if (!InventoryContains(inventory, guid))
				return false;
		}
		return true;
	}

	bool TryReadReportedQuickbarProperty(
		AFortPlayerControllerAthena* controller,
		FSlotSnapshot& out,
		bool& initialEmptyProven)
	{
		out = {};
		initialEmptyProven = false;
		if (!ResolveControllerLayout(controller))
			return false;

		const FPropertyLayout& property =
			GState.Controller.ReportedQuickbar;
		if (!property.IsValid() ||
			property.ElementSize != sizeof(FReportedQuickbarGuidState))
		{
			return false;
		}

		FReportedQuickbarGuidState state{};
		if (!TryCopyValue(
			AddOffset(controller, property.Offset), state) ||
			state.NumEnabledSlots < static_cast<int32>(kSlotCount) ||
			state.NumEnabledSlots >
				static_cast<int32>(kReportedQuickbarCapacity))
		{
			return false;
		}

		int32 rawBase = -1;
		if (state.NumEnabledSlots >=
			static_cast<int32>(kSlotCount + 1))
		{
			// Stock quickbar models reserve raw cell zero for the harvesting
			// tool and expose the five combat cells at 1..5.
			rawBase = 1;
		}
		else
		{
			// Five-cell forks can be compact (0..4), while a few stock-derived
			// implementations retain 1..5 indexing but report a count of five.
			// Accept only a shape whose edge cell disambiguates the window; the
			// inventory/object fallbacks below handle every other case.
			const bool hasRawZero =
				!IsZero(state.EquippedItemGuids[0]);
			const bool hasRawFive =
				!IsZero(state.EquippedItemGuids[kSlotCount]);
			if (hasRawZero == hasRawFive)
				return false;
			rawBase = hasRawFive ? 1 : 0;
		}

		if (rawBase < 0 ||
			rawBase + static_cast<int32>(kSlotCount) >
				static_cast<int32>(kReportedQuickbarCapacity))
		{
			return false;
		}
		for (size_t index = 0; index < kSlotCount; ++index)
		{
			out.Slots[index] = state.EquippedItemGuids[
				rawBase + static_cast<int32>(index)];
		}
		if (!HasUniqueGuids(out))
		{
			out = {};
			return false;
		}

		// A nonzero reserved cell proves that an empty combat map is a real,
		// initialized loadout rather than an allocated-but-unfilled model.  The
		// reserved GUID is deliberately not sent; Magnesium validates only the
		// five combat GUIDs against its authoritative inventory.
		initialEmptyProven = rawBase == 1 &&
			!IsZero(state.EquippedItemGuids[0]);
		if (initialEmptyProven && !SnapshotHasAnyGuid(out))
		{
			FInventoryGuids inventory{};
			initialEmptyProven =
				BuildInventoryGuids(controller, inventory) &&
				InventoryContains(
					inventory, state.EquippedItemGuids[0]);
		}
		return true;
	}

	bool ResolveQuickbarLayout(UObject* quickbars)
	{
		if (!quickbars || !quickbars->Class)
			return false;
		if (GState.Quickbars.Valid &&
			GState.Quickbars.Class == quickbars->Class)
		{
			return true;
		}
		if (!CanAttemptLayout(
			GState.QuickbarLayoutRetry, quickbars->Class))
		{
			return false;
		}

		FQuickbarLayout layout{};
		layout.Class = quickbars->Class;
		layout.ExpectedClass = FindResidentClassOnce(
			EResidentClass::FortQuickBars);
		layout.QuickbarStruct = FindResidentStructOnce(
			EResidentStruct::QuickBar);
		layout.SlotStruct = FindResidentStructOnce(
			EResidentStruct::QuickBarSlot);
		if (!layout.ExpectedClass || !layout.QuickbarStruct ||
			!layout.SlotStruct ||
			!quickbars->IsA(layout.ExpectedClass) ||
			!TryResolveProperty(
				quickbars->Class,
				"PrimaryQuickBar",
				layout.PrimaryQuickbar) ||
			!TryResolveProperty(
				layout.QuickbarStruct, "Slots", layout.Slots) ||
			layout.Slots.ElementSize < sizeof(FRawArray) ||
			!TryResolveProperty(
				layout.SlotStruct, "Items", layout.Items) ||
			layout.Items.ElementSize < sizeof(FRawArray))
		{
			FailLayoutClosed(
				GState.QuickbarLayoutRetry, quickbars->Class);
			return false;
		}

		TryResolveProperty(quickbars->Class, "Owner", layout.Owner);
		layout.SlotSize = layout.SlotStruct->GetPropertiesSize();
		layout.Valid = layout.SlotSize >=
			static_cast<int32>(sizeof(FRawArray)) &&
			layout.SlotSize <= 0x1000 &&
			layout.Items.Offset <= static_cast<uint32>(layout.SlotSize) &&
			layout.Items.ElementSize <=
				static_cast<uint32>(layout.SlotSize) - layout.Items.Offset;
		if (!layout.Valid)
		{
			FailLayoutClosed(
				GState.QuickbarLayoutRetry, quickbars->Class);
			return false;
		}
		GState.Quickbars = layout;
		GState.QuickbarLayoutRetry = {};
		return true;
	}

	EDefinitionKind ClassifyDefinition(UObject* definition);

	bool TryReadQuickbarSlotGuid(
		const FRawArray& slots,
		int32 rawIndex,
		FBridgeGuid& out)
	{
		out = {};
		if (rawIndex < 0 || rawIndex >= slots.Num)
			return false;
		const uintptr_t slot =
			reinterpret_cast<uintptr_t>(slots.Data) +
			static_cast<size_t>(rawIndex) *
				static_cast<size_t>(GState.Quickbars.SlotSize);
		FRawArray items{};
		if (!TryReadArray(
			reinterpret_cast<const void*>(
				slot + GState.Quickbars.Items.Offset),
			sizeof(FBridgeGuid),
			kMaximumSlotItems,
			items) ||
			items.Num > 1)
		{
			return false;
		}
		return items.Num == 0 || TryCopyValue(items.Data, out);
	}

	bool TryReadQuickbarProperty(
		AFortPlayerControllerAthena* controller,
		const FPropertyLayout& property,
		const FInventoryGuids& inventory,
		FSlotSnapshot& out,
		bool& initialEmptyProven)
	{
		out = {};
		initialEmptyProven = false;
		if (!property.IsValid() ||
			property.ElementSize < sizeof(void*))
		{
			return false;
		}

		UObject* quickbars = nullptr;
		if (!TryCopyValue(
			AddOffset(controller, property.Offset), quickbars) ||
			!IsLiveObject(quickbars) ||
			!ResolveQuickbarLayout(quickbars))
		{
			return false;
		}

		if (GState.Quickbars.Owner.IsValid() &&
			GState.Quickbars.Owner.ElementSize >= sizeof(void*))
		{
			UObject* owner = nullptr;
			if (!TryCopyValue(
				AddOffset(quickbars, GState.Quickbars.Owner.Offset),
				owner) ||
				(owner && owner != controller))
			{
				return false;
			}
		}

		const uint8* primary = AddOffset(
			quickbars, GState.Quickbars.PrimaryQuickbar.Offset);
		const uint8* slotsAddress = AddOffset(
			primary, GState.Quickbars.Slots.Offset);
		FRawArray slots{};
		if (!TryReadArray(
			slotsAddress,
			static_cast<size_t>(GState.Quickbars.SlotSize),
			kMaximumQuickbarSlots,
			slots) ||
			slots.Num < static_cast<int32>(kSlotCount))
		{
			return false;
		}

		int32 rawBase = 0;
		if (slots.Num >= static_cast<int32>(kSlotCount + 1))
		{
			// Stock layouts with six or more cells reserve cell zero and keep
			// combat slots at 1..5.  Do not make the entire exact map depend on
			// item-type enums/classes, which differ substantially by season.
			rawBase = 1;
			FBridgeGuid reservedGuid{};
			if (TryReadQuickbarSlotGuid(slots, 0, reservedGuid) &&
				!IsZero(reservedGuid) &&
				InventoryContains(inventory, reservedGuid))
			{
				initialEmptyProven = true;
			}
		}
		if (rawBase + static_cast<int32>(kSlotCount) > slots.Num)
			return false;

		for (size_t index = 0; index < kSlotCount; ++index)
		{
			if (!TryReadQuickbarSlotGuid(
				slots,
				rawBase + static_cast<int32>(index),
				out.Slots[index]))
			{
				return false;
			}
		}
		return HasUniqueGuids(out);
	}

	bool IsInputParameter(const UFunction::ParamNamed& parameter)
	{
		return (parameter.PropertyFlags & kCpfParm) != 0 &&
			(parameter.PropertyFlags &
				(kCpfOutParm | kCpfReturnParm)) == 0;
	}

	bool BuildGetterSchema(
		UFunction* function,
		bool hasSlot,
		uint32 returnSize,
		FGetterSchema& out)
	{
		out = {};
		if (!function || !IsLiveObject(function))
			return false;
		const auto parameters = function->GetParamsNamed();
		if (parameters.Size == 0 || parameters.Size > 0x100)
			return false;

		int parameterCount = 0;
		bool hasQuickbarType = false;
		bool hasSlotIndex = !hasSlot;
		bool hasReturn = false;
		for (const auto& parameter : parameters.NameOffsetMap)
		{
			if ((parameter.PropertyFlags & kCpfParm) == 0)
				continue;
			++parameterCount;
			if (parameter.Offset >= parameters.Size ||
				parameter.ElementSize >
					parameters.Size - parameter.Offset)
			{
				return false;
			}

			if ((parameter.Name == "QuickBarType" ||
				 parameter.Name == "QuickbarType") &&
				IsInputParameter(parameter) &&
				(parameter.ElementSize == sizeof(uint8) ||
				 parameter.ElementSize == sizeof(int32)))
			{
				out.QuickbarTypeOffset = parameter.Offset;
				out.QuickbarTypeSize = parameter.ElementSize;
				hasQuickbarType = true;
			}
			else if (hasSlot && parameter.Name == "SlotIndex" &&
				IsInputParameter(parameter) &&
				parameter.ElementSize == sizeof(int32))
			{
				out.SlotOffset = parameter.Offset;
				hasSlotIndex = true;
			}
			else if (parameter.Name == "ReturnValue" &&
				(parameter.PropertyFlags & kCpfReturnParm) != 0 &&
				parameter.ElementSize == returnSize)
			{
				out.ReturnOffset = parameter.Offset;
				hasReturn = true;
			}
		}

		if (parameterCount != (hasSlot ? 3 : 2) ||
			!hasQuickbarType || !hasSlotIndex || !hasReturn)
		{
			return false;
		}
		out.Function = function;
		out.ParamsSize = parameters.Size;
		return true;
	}

	bool ResolveGetterSchemas(
		AFortPlayerControllerAthena* controller,
		ULONGLONG now)
	{
		if (GState.GetterClass != controller->Class)
		{
			GState.GetterClass = controller->Class;
			GState.NextGetterSchemaAttemptAt = 0;
			GState.ItemGetter = {};
			GState.CountGetter = {};
		}
		if (GState.ItemGetter.Function && GState.CountGetter.Function)
			return true;
		if (now < GState.NextGetterSchemaAttemptAt)
			return false;
		GState.NextGetterSchemaAttemptAt = now + kSchemaRetryMs;

		FGetterSchema item{};
		FGetterSchema count{};
		if (!BuildGetterSchema(
			controller->GetFunction("GetItemInQuickbarSlot"),
			true,
			sizeof(UObject*),
			item) ||
			!BuildGetterSchema(
				controller->GetFunction("GetNumQuickbarSlots"),
				false,
				sizeof(int32),
				count))
		{
			return false;
		}
		GState.ItemGetter = item;
		GState.CountGetter = count;
		return true;
	}

	bool InvokeGetter(
		AFortPlayerControllerAthena* controller,
		const FGetterSchema& schema,
		int32 rawSlot,
		void* outReturn,
		size_t returnSize)
	{
		if (!controller || !schema.Function ||
			!IsLiveObject(schema.Function) ||
			schema.ParamsSize == 0 || schema.ParamsSize > 0x100 ||
			!outReturn || !returnSize)
		{
			return false;
		}

		alignas(16) uint8 parameters[0x100]{};
		const uint32 primary = 0;
		memcpy(
			parameters + schema.QuickbarTypeOffset,
			&primary,
			schema.QuickbarTypeSize);
		if (schema.SlotOffset != kInvalidOffset)
		{
			memcpy(
				parameters + schema.SlotOffset,
				&rawSlot,
				sizeof(rawSlot));
		}
		controller->ProcessEvent(schema.Function, parameters);
		memcpy(
			outReturn,
			parameters + schema.ReturnOffset,
			returnSize);
		return true;
	}

	bool ResolveWorldItemLayout(UObject* item)
	{
		if (!item || !item->Class)
			return false;
		if (GState.WorldItem.Valid &&
			GState.WorldItem.Class == item->Class)
		{
			return true;
		}
		if (!CanAttemptLayout(
			GState.WorldItemLayoutRetry, item->Class))
		{
			return false;
		}

		FWorldItemLayout layout{};
		layout.Class = item->Class;
		layout.ExpectedClass = FindResidentClassOnce(
			EResidentClass::FortWorldItem);
		layout.Valid = layout.ExpectedClass &&
			item->IsA(layout.ExpectedClass) &&
			TryResolveProperty(
				item->Class, "ItemEntry", layout.ItemEntry) &&
			GState.Inventory.Valid &&
			layout.ItemEntry.ElementSize >=
				static_cast<uint32>(GState.Inventory.EntrySize);
		if (!layout.Valid)
		{
			FailLayoutClosed(
				GState.WorldItemLayoutRetry, item->Class);
			return false;
		}
		GState.WorldItem = layout;
		GState.WorldItemLayoutRetry = {};
		return true;
	}

	bool TryGetWorldItemGuid(
		UObject* item,
		FBridgeGuid& outGuid,
		UObject** outDefinition = nullptr)
	{
		outGuid = {};
		if (outDefinition)
			*outDefinition = nullptr;
		if (!IsLiveObject(item) || !ResolveWorldItemLayout(item))
			return false;

		const uint8* entry = AddOffset(
			item, GState.WorldItem.ItemEntry.Offset);
		if (!entry || !TryCopyValue(
			AddOffset(entry, GState.Inventory.ItemGuid.Offset),
			outGuid))
		{
			return false;
		}
		if (outDefinition)
		{
			if (!GState.Inventory.ItemDefinition.IsValid() ||
				GState.Inventory.ItemDefinition.ElementSize < sizeof(void*) ||
				!TryCopyValue(
					AddOffset(
						entry,
						GState.Inventory.ItemDefinition.Offset),
					*outDefinition))
			{
				return false;
			}
		}
		return true;
	}

	void RefreshDefinitionClassificationLookups()
	{
		const ULONGLONG now = GetTickCount64();
		if (!GState.ItemTypeEnum &&
			now >= GState.NextItemTypeEnumAttemptAt)
		{
			GState.ItemTypeEnum = FindResidentItemTypeEnumOnce();
			GState.NextItemTypeEnumAttemptAt =
				now + kSchemaRetryMs;
			if (GState.ItemTypeEnum)
			{
				GState.HarvestItemType =
					GState.ItemTypeEnum->GetValue("WeaponHarvest");
				static constexpr const char* primaryTypes[] = {
					"WeaponRanged",
					"WeaponMelee",
					"Consumable",
					"Gadget",
					"AthenaGadget",
					"Weapon"
				};
				static_assert(
					sizeof(primaryTypes) / sizeof(primaryTypes[0]) ==
						kPrimaryItemTypeCount);
				for (size_t index = 0;
					 index < kPrimaryItemTypeCount;
					 ++index)
				{
					GState.PrimaryItemTypes[index] =
						GState.ItemTypeEnum->GetValue(primaryTypes[index]);
				}
			}
		}

		if (!GState.PrimaryDefinitionClassesResolved &&
			now >= GState.NextPrimaryDefinitionClassAttemptAt)
		{
			GState.NextPrimaryDefinitionClassAttemptAt =
				now + kSchemaRetryMs;
			// These derived classes are safe primary fallbacks. The broad
			// FortWeaponItemDefinition base is intentionally excluded because
			// harvesting tools derive from it on several builds.
			static constexpr EResidentClass primaryClassIds[] = {
				EResidentClass::FortWeaponRangedItemDefinition,
				EResidentClass::FortGadgetItemDefinition,
				EResidentClass::FortConsumableItemDefinition
			};
			static_assert(
				sizeof(primaryClassIds) / sizeof(primaryClassIds[0]) ==
					kPrimaryDefinitionClassCount);
			for (size_t index = 0;
				 index < kPrimaryDefinitionClassCount;
				 ++index)
			{
				if (!GState.PrimaryDefinitionClasses[index])
				{
					GState.PrimaryDefinitionClasses[index] =
						FindResidentClassOnce(primaryClassIds[index]);
				}
			}
			GState.PrimaryDefinitionClassesResolved = true;
			for (const UClass* resolved :
				GState.PrimaryDefinitionClasses)
			{
				if (!resolved)
				{
					GState.PrimaryDefinitionClassesResolved = false;
					break;
				}
			}
		}
	}

	bool ResolveDefinitionItemTypeLayout(
		UObject* definition,
		FPropertyLayout& out)
	{
		out = {};
		if (!definition || !definition->Class)
			return false;
		for (const auto& cached : GState.ItemTypeLayouts)
		{
			if (cached.Class == definition->Class &&
				cached.ItemType.IsValid())
			{
				out = cached.ItemType;
				return true;
			}
		}

		FPropertyLayout resolved{};
		if (!TryResolveProperty(
			definition->Class, "ItemType", resolved) ||
			(resolved.ElementSize != sizeof(uint8) &&
			 resolved.ElementSize != sizeof(int32)))
		{
			// Failed layouts are not cached, so later probes can recover after
			// reflection metadata finishes initializing.
			return false;
		}

		size_t destination = GState.ItemTypeLayouts.size();
		for (size_t index = 0;
			 index < GState.ItemTypeLayouts.size();
			 ++index)
		{
			if (!GState.ItemTypeLayouts[index].Class)
			{
				destination = index;
				break;
			}
		}
		if (destination == GState.ItemTypeLayouts.size())
		{
			destination = GState.NextItemTypeLayoutReplacement++ %
				GState.ItemTypeLayouts.size();
		}
		GState.ItemTypeLayouts[destination] = {
			definition->Class, resolved };
		out = resolved;
		return true;
	}

	EDefinitionKind ClassifyDefinition(UObject* definition)
	{
		if (!IsLiveObject(definition))
			return EDefinitionKind::Unknown;
		RefreshDefinitionClassificationLookups();

		FPropertyLayout itemType{};
		if (GState.ItemTypeEnum &&
			ResolveDefinitionItemTypeLayout(definition, itemType))
		{
			uint32 rawType = 0;
			const uint8* address = AddOffset(definition, itemType.Offset);
			if (itemType.ElementSize == sizeof(uint8))
			{
				uint8 byteType = 0;
				if (!TryCopyValue(address, byteType))
					return EDefinitionKind::Unknown;
				rawType = byteType;
			}
			else if (!TryCopyValue(address, rawType))
			{
				return EDefinitionKind::Unknown;
			}

			if (GState.HarvestItemType >= 0 &&
				static_cast<int64>(rawType) == GState.HarvestItemType)
			{
				return EDefinitionKind::Harvest;
			}
			for (const int64 primary : GState.PrimaryItemTypes)
			{
				if (primary >= 0 &&
					static_cast<int64>(rawType) == primary)
				{
					return EDefinitionKind::Primary;
				}
			}
		}

		for (const UClass* expected :
			GState.PrimaryDefinitionClasses)
		{
			if (expected && definition->IsA(expected))
				return EDefinitionKind::Primary;
		}
		return EDefinitionKind::Unknown;
	}

	bool TryReadGetterSlots(
		AFortPlayerControllerAthena* controller,
		const FInventoryGuids& inventory,
		FSlotSnapshot& out,
		bool& initialEmptyProven,
		ULONGLONG now)
	{
		out = {};
		initialEmptyProven = false;
		if (!ResolveGetterSchemas(controller, now))
			return false;

		int32 rawCount = 0;
		if (!InvokeGetter(
			controller,
			GState.CountGetter,
			0,
			&rawCount,
			sizeof(rawCount)) ||
			rawCount < static_cast<int32>(kSlotCount) ||
			rawCount > kMaximumQuickbarSlots)
		{
			return false;
		}

		// Six-or-more is the canonical reserved-cell layout.  A few APIs return
		// five even though their valid raw combat indices remain 1..5, so prefer
		// that stock convention and use slot-zero classification only to prove a
		// compact compatibility layout when metadata is available.
		int32 rawBase = 1;
		UObject* rawZero = nullptr;
		const bool readRawZero = InvokeGetter(
			controller,
			GState.ItemGetter,
			0,
			&rawZero,
			sizeof(rawZero));
		if (readRawZero && rawZero)
		{
			FBridgeGuid rawZeroGuid{};
			UObject* definition = nullptr;
			if (TryGetWorldItemGuid(
					rawZero, rawZeroGuid, &definition) &&
				InventoryContains(inventory, rawZeroGuid))
			{
				const EDefinitionKind kind =
					ClassifyDefinition(definition);
				if (rawCount == static_cast<int32>(kSlotCount) &&
					kind == EDefinitionKind::Primary)
				{
					rawBase = 0;
				}
				else
				{
					// A live reserved item proves the model is initialized even
					// when every combat getter currently returns null.
					initialEmptyProven = rawBase == 1;
				}
			}
		}

		for (size_t index = 0; index < kSlotCount; ++index)
		{
			UObject* returned = nullptr;
			if (!InvokeGetter(
				controller,
				GState.ItemGetter,
				rawBase + static_cast<int32>(index),
				&returned,
				sizeof(returned)))
			{
				return false;
			}
			if (!returned)
				continue;
			if (!TryGetWorldItemGuid(returned, out.Slots[index]) ||
				!InventoryContains(inventory, out.Slots[index]))
			{
				return false;
			}
		}
		return ValidateSnapshot(out, inventory);
	}

	bool ResolveSnapshot(
		AFortPlayerControllerAthena* controller,
		FSlotSnapshot& out,
		ESnapshotSource& source,
		bool& initialEmptyProven,
		ULONGLONG now)
	{
		out = {};
		source = ESnapshotSource::None;
		initialEmptyProven = false;

		// This exact fixed snapshot is both the cheapest and the least
		// version-sensitive source.  It does not need a UObject quickbar or a
		// replicated inventory walk; the receiving server performs the final
		// GUID-to-inventory validation before trusting it.
		if (TryReadReportedQuickbarProperty(
			controller, out, initialEmptyProven))
		{
			source = ESnapshotSource::ReportedQuickbar;
			return true;
		}

		FInventoryGuids inventory{};
		if (!BuildInventoryGuids(controller, inventory))
			return false;
		bool sourceEmptyProven = false;

		if (GState.Controller.ClientQuickBars.IsValid() &&
			TryReadQuickbarProperty(
				controller,
				GState.Controller.ClientQuickBars,
				inventory,
				out,
				sourceEmptyProven) &&
			ValidateSnapshot(out, inventory))
		{
			source = ESnapshotSource::ClientQuickBars;
			initialEmptyProven = sourceEmptyProven;
			return true;
		}
		sourceEmptyProven = false;
		if (GState.Controller.QuickBars.IsValid() &&
			TryReadQuickbarProperty(
				controller,
				GState.Controller.QuickBars,
				inventory,
				out,
				sourceEmptyProven) &&
			ValidateSnapshot(out, inventory))
		{
			source = ESnapshotSource::QuickBars;
			initialEmptyProven = sourceEmptyProven;
			return true;
		}
		sourceEmptyProven = false;
		if (TryReadGetterSlots(
			controller,
			inventory,
			out,
			sourceEmptyProven,
			now))
		{
			source = ESnapshotSource::Getters;
			initialEmptyProven = sourceEmptyProven;
			return true;
		}
		out = {};
		initialEmptyProven = false;
		return false;
	}

	bool BuildServerCheatSchema(
		AFortPlayerControllerAthena* controller,
		FFunctionSchema& out)
	{
		out = {};
		UFunction* function = controller
			? controller->GetFunction("ServerCheat")
			: nullptr;
		if (!function || !IsLiveObject(function))
			return false;

		const auto parameters = function->GetParamsNamed();
		if (parameters.Size != sizeof(FString))
			return false;
		int parameterCount = 0;
		uint32 messageOffset = kInvalidOffset;
		for (const auto& parameter : parameters.NameOffsetMap)
		{
			if ((parameter.PropertyFlags & kCpfParm) == 0)
				continue;
			++parameterCount;
			if (!IsInputParameter(parameter) ||
				parameter.Offset >= parameters.Size ||
				parameter.ElementSize >
					parameters.Size - parameter.Offset)
			{
				return false;
			}
			if (parameter.Name == "Msg" && parameter.Offset == 0 &&
				parameter.ElementSize == sizeof(FString))
			{
				messageOffset = parameter.Offset;
			}
		}
		if (parameterCount != 1 || messageOffset == kInvalidOffset)
			return false;

		out.Function = function;
		out.ParamsSize = parameters.Size;
		out.MessageOffset = messageOffset;
		return true;
	}

	void AppendHex(char*& destination, uint64 value, size_t digits)
	{
		static constexpr char hex[] = "0123456789abcdef";
		for (size_t index = 0; index < digits; ++index)
		{
			const size_t shift = (digits - index - 1) * 4;
			*destination++ = hex[(value >> shift) & 0xf];
		}
	}

	void AppendGuid(char*& destination, const FBridgeGuid& guid)
	{
		AppendHex(destination, guid.A, 8);
		AppendHex(destination, guid.B, 8);
		AppendHex(destination, guid.C, 8);
		AppendHex(destination, guid.D, 8);
	}

	bool FormatHello(std::array<char, kHelloCharacters + 1>& out)
	{
		char* write = out.data();
		memcpy(write, kHelloPrefix, kPrefixCharacters);
		write += kPrefixCharacters;
		AppendHex(write, GState.Session, kSessionCharacters);
		*write = '\0';
		return static_cast<size_t>(write - out.data()) ==
			kHelloCharacters;
	}

	bool FormatReport(
		const FSlotSnapshot& snapshot,
		uint32 sequence,
		std::array<char, kReportCharacters + 1>& out)
	{
		char* write = out.data();
		memcpy(write, kReportPrefix, kPrefixCharacters);
		write += kPrefixCharacters;
		AppendHex(write, GState.Session, kSessionCharacters);
		*write++ = ':';
		AppendHex(write, sequence, kSequenceCharacters);
		for (const FBridgeGuid& guid : snapshot.Slots)
		{
			*write++ = ':';
			AppendGuid(write, guid);
		}
		*write = '\0';
		return static_cast<size_t>(write - out.data()) ==
			kReportCharacters;
	}

	bool SendAscii(
		AFortPlayerControllerAthena* controller,
		const char* text,
		size_t length)
	{
		if (!controller || !text || length == 0 || length > 255 ||
			!GState.ServerCheat.Function ||
			!IsLiveObject(GState.ServerCheat.Function) ||
			GState.ServerCheat.ParamsSize != sizeof(FString) ||
			GState.ServerCheat.MessageOffset != 0)
		{
			return false;
		}

		std::array<wchar_t, 256> wide{};
		for (size_t index = 0; index < length; ++index)
		{
			const unsigned char character =
				static_cast<unsigned char>(text[index]);
			if (character < 0x20 || character > 0x7e)
				return false;
			wide[index] = static_cast<wchar_t>(character);
		}

		FString message;
		message.Data = wide.data();
		message.NumElements = static_cast<int32>(length + 1);
		message.MaxElements = static_cast<int32>(length + 1);
		alignas(16) uint8 parameters[0x100]{};
		memcpy(
			parameters + GState.ServerCheat.MessageOffset,
			&message,
			sizeof(message));
		controller->ProcessEvent(
			GState.ServerCheat.Function, parameters);
		return true;
	}

	bool SendHello(AFortPlayerControllerAthena* controller)
	{
		std::array<char, kHelloCharacters + 1> hello{};
		return FormatHello(hello) &&
			SendAscii(controller, hello.data(), kHelloCharacters);
	}

	bool SendReport(
		AFortPlayerControllerAthena* controller,
		const FSlotSnapshot& snapshot)
	{
		const uint32 nextSequence = GState.Sequence + 1;
		if (nextSequence == 0)
			return false;
		std::array<char, kReportCharacters + 1> report{};
		if (!FormatReport(snapshot, nextSequence, report) ||
			!SendAscii(controller, report.data(), kReportCharacters))
		{
			return false;
		}
		GState.Sequence = nextSequence;
		return true;
	}

	void TickUnsafe(
		UWorld* world,
		AFortPlayerControllerAthena* controller,
		bool clientMessageHookReady)
	{
		if (!GEnabled.load(std::memory_order_acquire) ||
			GPermanentlyInert.load(std::memory_order_acquire))
		{
			return;
		}

		// This SDK does not expose the encrypted-property offset decoder used
		// by FN 32.x. Reading guessed offsets would be worse than losing this
		// optional feature, so those builds deliberately remain inert.
		if (VersionInfo.FortniteVersion >= 32.0)
		{
			GPermanentlyInert.store(true, std::memory_order_release);
			GPublishedSession.store(0, std::memory_order_release);
			GAcknowledgedSession.store(0, std::memory_order_release);
			AtlasDiagnostics::WriteLine(
				"loadout-telemetry inert unsupported-encrypted-layout version=%.2f",
				VersionInfo.FortniteVersion);
			return;
		}

		const ULONGLONG now = GetTickCount64();
		if (now < GState.NextProbeAt)
			return;
		GState.NextProbeAt = now +
			(GState.LastSource == ESnapshotSource::ReportedQuickbar
				? kReportedProbeIntervalMs
				: kFallbackProbeIntervalMs);

		const uint64 worldIdentity = GetObjectIdentity(world);
		const uint64 controllerIdentity = GetObjectIdentity(controller);
		if (!worldIdentity || !controllerIdentity ||
			!controller || !controller->Class)
		{
			// LocalPlayers can disappear briefly during possession/travel. Keep
			// the lifetime key so that such a transient cannot cause a second
			// hello to the same controller/world pair. A genuinely new pair is
			// detected by its object index+serial identity below.
			GState.CandidateValid = false;
			GState.CandidateSince = 0;
			return;
		}
		const UClass* controllerClass = FindResidentClassOnce(
			EResidentClass::FortPlayerControllerAthena);
		if (!controllerClass || !controller->IsA(controllerClass))
			return;

		if (worldIdentity != GState.WorldIdentity ||
			controllerIdentity != GState.ControllerIdentity)
		{
			ResetForController(worldIdentity, controllerIdentity);
		}

		if ((!GState.ServerCheat.Function ||
			 GState.ServerCheatClass != controller->Class) &&
			now >= GState.NextSchemaAttemptAt)
		{
			GState.NextSchemaAttemptAt = now + kSchemaRetryMs;
			FFunctionSchema schema{};
			if (BuildServerCheatSchema(controller, schema))
			{
				GState.ServerCheat = schema;
				GState.ServerCheatClass = controller->Class;
			}
		}
		if (!GState.ServerCheat.Function || !clientMessageHookReady)
			return;

		const bool acknowledged =
			GAcknowledgedSession.load(std::memory_order_acquire) ==
			GState.Session;
		if (!acknowledged)
		{
			if (GState.HelloAttempts < kMaximumHelloAttempts &&
				now >= GState.NextHelloAt)
			{
				++GState.HelloAttempts;
				GState.NextHelloAt =
					GState.HelloAttempts < kMaximumHelloAttempts
						? now + kHelloRetryIntervalMs
						: (std::numeric_limits<ULONGLONG>::max)();
				if (!SendHello(controller))
				{
					// Count failed local dispatches too. A stale function must not
					// turn negotiation into an unbounded retry loop for this lifetime.
					GState.ServerCheat = {};
					GState.ServerCheatClass = nullptr;
					GState.NextSchemaAttemptAt = now + kSchemaRetryMs;
				}
			}
			return;
		}
		if (!GState.AckLogged)
		{
			GState.AckLogged = true;
			AtlasDiagnostics::WriteLine(
				"loadout-telemetry negotiated session=%016llx",
				static_cast<unsigned long long>(GState.Session));
		}

		FSlotSnapshot snapshot{};
		ESnapshotSource source = ESnapshotSource::None;
		bool initialEmptyProven = false;
		if (!ResolveSnapshot(
			controller,
			snapshot,
			source,
			initialEmptyProven,
			now))
		{
			GState.CandidateValid = false;
			GState.CandidateSince = 0;
			if (now >= GState.NextResolveDiagnosticAt)
			{
				GState.NextResolveDiagnosticAt =
					now + kResolveDiagnosticIntervalMs;
				AtlasDiagnostics::WriteLine(
					"loadout-telemetry unresolved reported=%u client=%u legacy=%u inventory=%u",
					GState.Controller.ReportedQuickbar.IsValid() ? 1u : 0u,
					GState.Controller.ClientQuickBars.IsValid() ? 1u : 0u,
					GState.Controller.QuickBars.IsValid() ? 1u : 0u,
					GState.Controller.WorldInventory.IsValid() ? 1u : 0u);
			}
			return;
		}
		const bool hasAnyGuid = SnapshotHasAnyGuid(snapshot);
		if (!hasAnyGuid && !GState.HasSentNonzero &&
			!GState.LastSentValid &&
			!initialEmptyProven)
		{
			// An unproven empty first read commonly means replication is not
			// ready yet.  A canonical model with a live reserved harvesting cell
			// proves that its five empty combat cells are legitimate and must be
			// reported so the server can make them editable immediately.
			GState.CandidateValid = false;
			GState.CandidateSince = 0;
			return;
		}

		if (!GState.CandidateValid ||
			!SnapshotEquals(GState.Candidate, snapshot))
		{
			GState.Candidate = snapshot;
			GState.CandidateValid = true;
			GState.CandidateSince = now;
			GState.LastSource = source;
			return;
		}
		if (now - GState.CandidateSince < kDebounceMs ||
			now - GState.LastSendAt < kMinimumSendIntervalMs)
		{
			return;
		}

		const bool changed = !GState.LastSentValid ||
			!SnapshotEquals(GState.LastSent, snapshot);
		const bool changedReportConfirmationDue =
			GState.LastSentValid &&
			GState.ChangedReportConfirmationPending &&
			SnapshotEquals(GState.LastSent, snapshot) &&
			now >= GState.NextChangedReportConfirmationAt;
		const bool heartbeatDue = GState.LastSentValid &&
			now - GState.LastSendAt >= kHeartbeatIntervalMs;
		if (!changed && !changedReportConfirmationDue && !heartbeatDue)
			return;

		const bool firstReport = !GState.LastSentValid;
		if (SendReport(controller, snapshot))
		{
			GState.LastSent = snapshot;
			GState.LastSentValid = true;
			GState.HasSentNonzero =
				GState.HasSentNonzero || hasAnyGuid;
			GState.LastSendAt = now;
			if (changed)
			{
				GState.ChangedReportConfirmationPending = true;
				GState.NextChangedReportConfirmationAt =
					now + kChangedReportConfirmationMs;
			}
			else if (changedReportConfirmationDue)
			{
				GState.ChangedReportConfirmationPending = false;
				GState.NextChangedReportConfirmationAt = 0;
			}
			if (firstReport)
			{
				// Deliberately omit names, URLs, GUIDs, and inventory contents.
				AtlasDiagnostics::WriteLine(
					"loadout-telemetry reporting source=%s empty=%u proven=%u",
					SnapshotSourceName(source),
					hasAnyGuid ? 0u : 1u,
					initialEmptyProven ? 1u : 0u);
			}
		}
	}

	void TickGuarded(
		UWorld* world,
		AFortPlayerControllerAthena* controller,
		bool clientMessageHookReady)
	{
		__try
		{
			TickUnsafe(world, controller, clientMessageHookReady);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			RegisterGuardedFault("tick");
		}
	}

	int LowerHexValue(wchar_t character)
	{
		if (character >= L'0' && character <= L'9')
			return character - L'0';
		if (character >= L'a' && character <= L'f')
			return 10 + character - L'a';
		return -1;
	}

	bool TryParseExactAck(
		const wchar_t* text,
		int length,
		uint64& outSession)
	{
		outSession = 0;
		if (!text || length != static_cast<int>(kAckCharacters) ||
			wmemcmp(text, kAckPrefix, kPrefixCharacters) != 0)
		{
			return false;
		}
		uint64 session = 0;
		for (size_t index = 0; index < kSessionCharacters; ++index)
		{
			const int digit = LowerHexValue(
				text[kPrefixCharacters + index]);
			if (digit < 0)
				return false;
			session = (session << 4) | static_cast<uint64>(digit);
		}
		if (!session)
			return false;
		outSession = session;
		return true;
	}
}

void SetEnabled(bool enabled) noexcept
{
	GEnabled.store(enabled, std::memory_order_release);
	if (!enabled)
	{
		GPublishedSession.store(0, std::memory_order_release);
		GAcknowledgedSession.store(0, std::memory_order_release);
	}
}

bool WantsClientMessageHook() noexcept
{
	return GEnabled.load(std::memory_order_acquire) &&
		!GPermanentlyInert.load(std::memory_order_acquire);
}

bool HandleClientMessage(const wchar_t* text, int length) noexcept
{
	try
	{
		if (!WantsClientMessageHook())
			return false;
		uint64 acknowledged = 0;
		if (!TryParseExactAck(text, length, acknowledged))
			return false;
		const uint64 current =
			GPublishedSession.load(std::memory_order_acquire);
		if (!current || acknowledged != current)
			return false;
		GAcknowledgedSession.store(
			acknowledged, std::memory_order_release);
		return true;
	}
	catch (...)
	{
		return false;
	}
}

void GameThreadTick(
	UWorld* world,
	AFortPlayerControllerAthena* controller,
	bool clientMessageHookReady) noexcept
{
	if (!GEnabled.load(std::memory_order_acquire) ||
		GPermanentlyInert.load(std::memory_order_acquire))
	{
		return;
	}
	try
	{
		TickGuarded(world, controller, clientMessageHookReady);
	}
	catch (...)
	{
		RegisterGuardedFault("cpp-tick");
	}
}
}
