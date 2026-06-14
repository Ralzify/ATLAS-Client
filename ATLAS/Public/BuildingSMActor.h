#pragma once

#include "../../pch.h"

class UFortItem : public UObject
{
public:
    UCLASS_COMMON_MEMBERS(UFortItem);
};

class ABuildingSMActor : public AActor
{
public:
    UCLASS_COMMON_MEMBERS(ABuildingSMActor);

    DEFINE_BITFIELD_PROP(bPlayerPlaced);
    DEFINE_PROP(Team, uint8);
    DEFINE_PROP(TeamIndex, uint8);
    DEFINE_PROP(OwnerPersistentID, int16);
    DEFINE_BITFIELD_PROP(bDestroyed);
    DEFINE_PROP(CurrentBuildingLevel, int32);
    DEFINE_BITFIELD_PROP(bAllowResourceDrop);
    DEFINE_BITFIELD_PROP(bPersistToWorld);
    DEFINE_BITFIELD_PROP(bAutoReleaseCurieContainerOnDestroyed);
    DEFINE_PROP(BuildingReplacementType, uint8_t);
    DEFINE_PROP(ReplacementDestructionReason, uint8_t);
    DEFINE_PROP(OnReplacementDestruction, TMulticastInlineDelegate<void(uint8_t, ABuildingSMActor*)>);
    DEFINE_PROP(AttachedBuildingActors, TArray<ABuildingSMActor*>);
    DEFINE_BITFIELD_PROP(bHiddenDueToTrapPlacement);
    DEFINE_PROP(BuildingType, uint8);
    DEFINE_PROP(EditModePatternData, UObject*);
    DEFINE_BITFIELD_PROP(bIsPlayerBuildable);

    DEFINE_FUNC(SetTeam, void);
    DEFINE_FUNC(GetHealth, float);
    DEFINE_FUNC(GetMaxHealth, float);
    DEFINE_FUNC(SetMirrored, void);
    DEFINE_FUNC(InitializeKismetSpawnedBuildingActor, void);
    DEFINE_FUNC(GetHealthPercent, float);
    DEFINE_FUNC(RepairBuilding, void);
    DEFINE_FUNC(SilentDie, void);
    DEFINE_FUNC(OnRep_CurrentBuildingLevel, void);
    DEFINE_STATIC_FUNC(K2_SpawnBuildingActor, ABuildingSMActor*);
    DEFINE_FUNC(AttachBuildingActorToMe, void);
    DEFINE_FUNC(OnRep_EditingPlayer, void);
};