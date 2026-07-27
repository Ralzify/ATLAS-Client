#pragma once

#include "../../pch.h"
#include "../Public/FortCheatManager.h"

class AFortPlayerStateAthena : public AActor
{
public:
    UCLASS_COMMON_MEMBERS(AFortPlayerStateAthena);

    DEFINE_PROP(SquadId, uint8);
    DEFINE_PROP(TeamIndex, uint8);
    DEFINE_PROP(PawnDeathLocation, FVector);
    DEFINE_PROP(Kills, int32);
    DEFINE_PROP(KillScore, int32);
    DEFINE_PROP(TeamKillScore, int32);
    DEFINE_PROP(Place, int32);
    DEFINE_PROP(SeasonLevelUIDisplay, int32);
    DEFINE_PROP(CharacterParts, const UObject**);
    DEFINE_PROP(HeroType, const UObject*);
    DEFINE_BITFIELD_PROP(bIsABot);
    DEFINE_BITFIELD_PROP(bIsSpectator);
    DEFINE_PROP(WorldPlayerId, int16);
    DEFINE_PROP(TeamMemberState, uint8);
    DEFINE_PROP(ReplicatedTeamMemberState, uint8);
};

class AFortPlayerPawnAthena : public AActor
{
public:
    UCLASS_COMMON_MEMBERS(AFortPlayerPawnAthena);

    DEFINE_PROP(CurrentWeapon, AActor*);
    DEFINE_PROP(PreviousWeapon, AActor*);
    DEFINE_PROP(Controller, AActor*);
    DEFINE_PROP(IncomingPickups, TArray<AActor*>);
    DEFINE_BITFIELD_PROP(bMovingEmote);
    DEFINE_PROP(EmoteWalkSpeed, float);
    DEFINE_BITFIELD_PROP(bMovingEmoteForwardOnly);
    DEFINE_BITFIELD_PROP(bMovingEmoteFollowingOnly);
    DEFINE_PROP(LastFallDistance, float);
    DEFINE_BITFIELD_PROP(bIsInAnyStorm);
    DEFINE_BITFIELD_PROP(bIsInsideSafeZone);
    DEFINE_PROP(AIControllerClass, TSubclassOf<AActor>);
    DEFINE_PROP(PlayerState, AActor*);
    DEFINE_PROP(BaseEyeHeight, float);
    DEFINE_PROP(OnHeldObjectPickedUp, TMulticastInlineDelegate<void(AActor*)>);
    DEFINE_PROP(OnHeldObjectDropped, TMulticastInlineDelegate<void(AActor*)>);
    DEFINE_PROP(OnEnteredAircraft, TMulticastInlineDelegate<void()>);
    DEFINE_PROP(PickupSpeedMultiplier, float);
    DEFINE_PROP(HeldObject, TWeakObjectPtr<AActor>);
    DEFINE_PROP(RepActiveMovementModeExtension, void*);
    DEFINE_BITFIELD_PROP(bIsPlayingEmote);
    DEFINE_PROP(CurrentWeaponList, TArray<AActor*>);
    DEFINE_PROP(bShouldDropItemsOnDeath, bool);
    DEFINE_PROP(MoveSoundStimulusBroadcastInterval, uint16_t);
    DEFINE_PROP(LastReplicatedEmoteExecuted, UObject*);
    DEFINE_PROP(Mesh, UActorComponent*);
    DEFINE_BITFIELD_PROP(bIsDBNO);
    DEFINE_BITFIELD_PROP(bIsSkydiving);
    DEFINE_BITFIELD_PROP(bIsSkydivingFromBus);
    DEFINE_PROP(RegisteredMovementModeExtentionLogic, TMap<uint32, UObject*>);
    DEFINE_PROP(VehicleInputComponent, UObject*);
};

class ABuildingPlayerPrimitivePreview : public AActor
{
public:
    UCLASS_COMMON_MEMBERS(ABuildingPlayerPrimitivePreview);
};

class AFortPlayerControllerAthena : public AActor
{
public:
    UCLASS_COMMON_MEMBERS(AFortPlayerControllerAthena);

    DEFINE_BITFIELD_PROP(bBuildFree);
    DEFINE_BITFIELD_PROP(bInfiniteAmmo);
    DEFINE_BITFIELD_PROP(bWantsToSprint);
    DEFINE_PROP(CheatManager, UFortCheatManager*);
    DEFINE_PROP(CheatClass, TSubclassOf<UObject>);
    DEFINE_PROP(TargetedBuilding, AActor*);
    DEFINE_PROP(HighlightedPrimaryBuilding, TWeakObjectPtr<AActor>);
    DEFINE_PROP(HighlightedPrimaryBuildings, TArray<AActor*>);
    DEFINE_PROP(Pawn, AFortPlayerPawnAthena*);
    DEFINE_PROP(MyFortPawn, AFortPlayerPawnAthena*);
    DEFINE_PROP(PlayerState, AFortPlayerStateAthena*);
};
