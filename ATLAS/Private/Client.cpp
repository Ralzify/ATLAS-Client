#include "pch.h"
#include "../Public/FortPlaylistAthena.h"
#include "../Public/Utils.h"
#include "../Public/FortPlayerControllerAthena.h"
#include "../Public/Client.h"
#include "../Public/Configuration.h"
#include "../Public/Diagnostics.h"

inline void* (*SelectResetOG)(void*) = nullptr;
inline void* (*SelectEditOG)(void*) = nullptr;
inline void (*PerformBuildingEditInteractionOG)(void*) = nullptr;
inline void (*CompleteBuildingEditInteraction)(void*) = nullptr;

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

void ClientThread()
{
	AFortPlayerControllerAthena* observedController = nullptr;
	UFortCheatManager* observedManager = nullptr;

	while (true)
	{
		auto playerController = GetLocalFortPlayerController();
		if (playerController)
		{
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

		Sleep(33);
	}
}

void Client::Init()
{
	auto engine = UEngine::GetEngine();
	if (FConfiguration::bConsoleEnabled && engine && engine->GameViewport && engine->ConsoleClass)
		engine->GameViewport->ViewportConsole = UGameplayStatics::SpawnObject(engine->ConsoleClass, engine->GameViewport);

	if (VersionInfo.EngineVersion < 4.24)
		FConfiguration::bEOREnabled = true;
	if (VersionInfo.FortniteVersion < 24.30)
		FConfiguration::bROREnabled = true;
	//if (VersionInfo.FortniteVersion < 15.20)
		//FConfiguration::bDisablePreEdits = true;

	if (VersionInfo.FortniteVersion >= 10 || FConfiguration::bForceRespawns)
	{
		auto PrimarySlot = uint8_t(EPlaylistUIExtensionSlot::StaticEnum() ? EPlaylistUIExtensionSlot::GetPrimary() : EUIExtensionSlot::GetPrimary());

		TArray<FUIExtension> ArenaExtensions, ShowdownExtensions;

		if (VersionInfo.FortniteVersion >= 10)
		{
			FUIExtension ArenaUIExtension{};
			ArenaUIExtension.Slot = PrimarySlot;
			if (VersionInfo.FortniteVersion < 23)
				ArenaUIExtension.WidgetClass.ObjectID.AssetPathName = FName(L"/Game/UI/Competitive/Arena/ArenaScoringHUD.ArenaScoringHUD_C");
			else
			{
				auto& PackageName = *(FName*)(__int64(&ArenaUIExtension.WidgetClass) + (VersionInfo.EngineVersion < 5.3 ? 0xC : 0x8));
				auto& AssetName = *(FName*)(__int64(&ArenaUIExtension.WidgetClass) + (VersionInfo.EngineVersion < 5.3 ? 0x10 : 0xC));
				auto& SubPathString = *(FString*)(__int64(&ArenaUIExtension.WidgetClass) + (VersionInfo.EngineVersion < 5.3 ? 0x14 : 0x10));

				PackageName = FName(L"/Game/UI/Competitive/Arena/ArenaScoringHUD");
				AssetName = FName(L"ArenaScoringHUD_C");
				SubPathString = FString();
			}

			FUIExtension ShowdownUIExtension{};
			ShowdownUIExtension.Slot = PrimarySlot;
			if (VersionInfo.FortniteVersion < 23)
				ShowdownUIExtension.WidgetClass.ObjectID.AssetPathName = FName(L"/Game/UI/Frontend/Showdown/ShowdownScoringHUD.ShowdownScoringHUD_C");
			else
			{
				auto& PackageName = *(FName*)(__int64(&ShowdownUIExtension.WidgetClass) + (VersionInfo.EngineVersion < 5.3 ? 0xC : 0x8));
				auto& AssetName = *(FName*)(__int64(&ShowdownUIExtension.WidgetClass) + (VersionInfo.EngineVersion < 5.3 ? 0x10 : 0xC));
				auto& SubPathString = *(FString*)(__int64(&ShowdownUIExtension.WidgetClass) + (VersionInfo.EngineVersion < 5.3 ? 0x14 : 0x10));

				PackageName = FName(L"/Game/UI/Frontend/Showdown/ShowdownScoringHUD");
				AssetName = FName(L"ShowdownScoringHUD_C");
				SubPathString = FString();
			}

			ArenaExtensions.Add(ArenaUIExtension);
			ShowdownExtensions.Add(ShowdownUIExtension);
		}

		auto PlaylistClass = FindClass("FortPlaylistAthena");

		for (int i = 0; i < TUObjectArray::Num(); i++)
		{
			auto Object = TUObjectArray::GetObjectByIndex(i);

			if (Object && Object->IsA((UClass*)PlaylistClass))
			{
				auto Playlist = (UFortPlaylistAthena*)Object;

				if (FConfiguration::bForceRespawns)
				{
					if (Playlist->HasbRespawnInAir())
						Playlist->bRespawnInAir = true;
					if (Playlist->HasRespawnHeight())
					{
						Playlist->RespawnHeight.Curve.CurveTable = nullptr;
						Playlist->RespawnHeight.Curve.RowName = FName();
						Playlist->RespawnHeight.Value = (float)FConfiguration::RespawnHeight;
					}
					if (Playlist->HasRespawnTime())
					{
						Playlist->RespawnTime.Curve.CurveTable = nullptr;
						Playlist->RespawnTime.Curve.RowName = FName();
						Playlist->RespawnTime.Value = (float)FConfiguration::RespawnTime;
					}
					Playlist->RespawnType = 1; // InfiniteRespawns
					if (Playlist->HasbAllowJoinInProgress())
						Playlist->bAllowJoinInProgress = true;
					if (Playlist->HasbForceRespawnLocationInsideOfVolume())
						Playlist->bForceRespawnLocationInsideOfVolume = true;
				}
				if (VersionInfo.FortniteVersion >= 10)
				{
					auto Name = Object->Name.ToString();
					if (Name.contains("Showdown"))
						Playlist->UIExtensions = Name.contains("ShowdownAlt") ? ArenaExtensions : ShowdownExtensions;
				}
			}
		}
	}

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

	CreateThread(nullptr, 0, (LPTHREAD_START_ROUTINE)ClientThread, nullptr, 0, nullptr);
}
