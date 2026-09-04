#include "../ATLAS/Public/ArenaVisualTracePolicy.h"

#include <cassert>

int main()
{
    using namespace AtlasArenaVisualTracePolicy;

    const FParameterSchema Placement[] = {
        { "Placement", 0, 4, CpfParm },
        { "PointsEarned", 4, 4, CpfParm },
    };
    assert(IsPlacementRpcSchema(8, Placement, 2));
    assert(!IsPlacementRpcSchema(12, Placement, 2));

    const FParameterSchema ReversedPlacement[] = {
        Placement[1], Placement[0],
    };
    assert(IsPlacementRpcSchema(8, ReversedPlacement, 2));

    FParameterSchema UnsafePlacement[] = {
        Placement[0], Placement[1],
    };
    UnsafePlacement[1].PropertyFlags |= CpfOutParm;
    assert(!IsPlacementRpcSchema(8, UnsafePlacement, 2));

    const FParameterSchema TeamPlacement[] = {
        { "NewTeamPlacement", 0, 4, CpfParm },
    };
    assert(IsTeamPlacementConsumerSchema(4, TeamPlacement, 1));
    assert(!IsTeamPlacementConsumerSchema(8, TeamPlacement, 1));
    assert(!IsTeamPlacementConsumerSchema(4, TeamPlacement, 0));

    FParameterSchema WrongTeamPlacementName[] = {
        { "Placement", 0, 4, CpfParm },
    };
    assert(!IsTeamPlacementConsumerSchema(
        4, WrongTeamPlacementName, 1));

    FParameterSchema WrongTeamPlacementOffset[] = {
        { "NewTeamPlacement", 4, 4, CpfParm },
    };
    assert(!IsTeamPlacementConsumerSchema(
        4, WrongTeamPlacementOffset, 1));

    FParameterSchema UnsafeTeamPlacement[] = {
        { "NewTeamPlacement", 0, 4, CpfParm | CpfOutParm },
    };
    assert(!IsTeamPlacementConsumerSchema(
        4, UnsafeTeamPlacement, 1));

    const FParameterSchema Stat[] = {
        { "TournamentStatInfo", 0, 0x20, CpfParm },
    };
    assert(IsTournamentStatRpcSchema(0x20, Stat, 1));

    const FParameterSchema ShortStatName[] = {
        { "StatInfo", 0, 0x20, CpfParm },
    };
    assert(IsTournamentStatRpcSchema(0x20, ShortStatName, 1));

    const FParameterSchema CompactStat[] = {
        { "TournamentStatInfo", 0, 0x18, CpfParm },
    };
    assert(IsTournamentStatRpcSchema(0x18, CompactStat, 1));
    assert(!IsTournamentStatRpcSchema(0x20, CompactStat, 1));

    const FParameterSchema ReferenceStat[] = {
        {
            "TournamentStatInfo", 0, 0x20,
            CpfParm | CpfOutParm | CpfReferenceParm
        },
    };
    assert(IsTournamentStatRpcSchema(0x20, ReferenceStat, 1));

    const FParameterSchema PlainOutStat[] = {
        {
            "TournamentStatInfo", 0, 0x20,
            CpfParm | CpfOutParm
        },
    };
    assert(!IsTournamentStatRpcSchema(0x20, PlainOutStat, 1));

    const FParameterSchema WrongStat[] = {
        { "TournamentStatInfo", 4, 0x20, CpfParm },
    };
    assert(!IsTournamentStatRpcSchema(0x20, WrongStat, 1));

    assert(IsZeroParameterSchema(0, 0));
    assert(!IsZeroParameterSchema(4, 0));
    assert(!IsZeroParameterSchema(0, 1));

    const FParameterSchema setBrushFromTexture[] = {
        { "Texture", 0, sizeof(void*), CpfParm },
        { "bMatchSize", sizeof(void*), sizeof(std::uint8_t), CpfParm }
    };
    assert(IsSetBrushFromTextureSchema(
        0x10, setBrushFromTexture, 2));
    assert(!IsSetBrushFromTextureSchema(
        0x08, setBrushFromTexture, 2));
    assert(!IsSetBrushFromTextureSchema(
        0x10, setBrushFromTexture, 1));
    auto wrongBrushTexture = setBrushFromTexture[0];
    wrongBrushTexture.Name = "Resource";
    const FParameterSchema wrongBrushName[] = {
        wrongBrushTexture, setBrushFromTexture[1]
    };
    assert(!IsSetBrushFromTextureSchema(
        0x10, wrongBrushName, 2));
    auto outBrushMatchSize = setBrushFromTexture[1];
    outBrushMatchSize.PropertyFlags |= CpfOutParm;
    const FParameterSchema wrongBrushFlags[] = {
        setBrushFromTexture[0], outBrushMatchSize
    };
    assert(!IsSetBrushFromTextureSchema(
        0x10, wrongBrushFlags, 2));

    const FTournamentStatFieldSchema ModernStatFields[] = {
        { "StatName", 0x0, 0x10 },
        { "StatDisplayName", 0x10, 0x4 },
        { "StatValue", 0x18, 0x4 },
        { "bIsDeltaCount", 0x14, 0x1 },
    };
    const auto ModernStat = ResolveTournamentStatPayloadSchema(
        0x20, ModernStatFields, 4);
    assert(ModernStat ==
        ETournamentStatPayloadSchema::ModernCompactNameWithDelta);
    assert(TournamentStatDisplayNameSize(ModernStat) == 4);
    assert(TournamentStatValueOffset(ModernStat) == 0x18);
    assert(TournamentStatHasDeltaFlag(ModernStat));

    const FTournamentStatFieldSchema CompactStatFields[] = {
        { "StatName", 0x0, 0x10 },
        { "StatDisplayName", 0x10, 0x4 },
        { "StatValue", 0x14, 0x4 },
    };
    const auto CompactStatPayload = ResolveTournamentStatPayloadSchema(
        0x18, CompactStatFields, 3);
    assert(CompactStatPayload ==
        ETournamentStatPayloadSchema::CompactNameWithoutDelta);
    assert(TournamentStatDisplayNameSize(CompactStatPayload) == 4);
    assert(TournamentStatValueOffset(CompactStatPayload) == 0x14);
    assert(!TournamentStatHasDeltaFlag(CompactStatPayload));

    const FTournamentStatFieldSchema LegacyStatFields[] = {
        { "StatName", 0x0, 0x10 },
        { "StatDisplayName", 0x10, 0x8 },
        { "StatValue", 0x18, 0x4 },
    };
    const auto LegacyStat = ResolveTournamentStatPayloadSchema(
        0x20, LegacyStatFields, 3);
    assert(LegacyStat ==
        ETournamentStatPayloadSchema::LegacyFullName);
    assert(TournamentStatDisplayNameSize(LegacyStat) == 8);
    assert(TournamentStatValueOffset(LegacyStat) == 0x18);

    assert(ResolveTournamentStatPayloadSchema(
        0x20, CompactStatFields, 3) ==
        ETournamentStatPayloadSchema::Unsupported);
    assert(DidPlacementStatTransitionPresentExactlyOnce(10, 10, 11));
    assert(!DidPlacementStatTransitionPresentExactlyOnce(10, 11, 12));
    assert(!DidPlacementStatTransitionPresentExactlyOnce(10, 10, 10));
    assert(!DidPlacementStatTransitionPresentExactlyOnce(10, 10, 12));

    constexpr wchar_t EliminationStat[] =
        L"TEAM_ELIMS_STAT_INDEX";
    assert(IsEliminationTournamentStat(
        EliminationStat,
        sizeof(EliminationStat) / sizeof(EliminationStat[0])));
    assert(!IsEliminationTournamentStat(
        EliminationStat,
        (sizeof(EliminationStat) / sizeof(EliminationStat[0])) - 1));
    assert(!IsEliminationTournamentStat(
        L"MATCH_PLAYED_STAT", 18));
    assert(!IsEliminationTournamentStat(nullptr, 0));
    constexpr wchar_t MatchPlayedStat[] = L"MATCH_PLAYED_STAT";
    assert(IsMatchPlayedTournamentStat(
        MatchPlayedStat,
        sizeof(MatchPlayedStat) / sizeof(MatchPlayedStat[0])));
    assert(!IsMatchPlayedTournamentStat(
        MatchPlayedStat,
        (sizeof(MatchPlayedStat) / sizeof(MatchPlayedStat[0])) - 1));
    assert(!IsMatchPlayedTournamentStat(nullptr, 0));
    constexpr wchar_t PlacementStatName[] =
        L"PLACEMENT_STAT_INDEX";
    assert(IsPlacementTournamentStat(
        PlacementStatName,
        sizeof(PlacementStatName) / sizeof(PlacementStatName[0])));
    assert(!IsPlacementTournamentStat(
        PlacementStatName,
        (sizeof(PlacementStatName) / sizeof(PlacementStatName[0])) - 1));
    assert(!IsPlacementTournamentStat(EliminationStat,
        sizeof(EliminationStat) / sizeof(EliminationStat[0])));

    std::int32_t BridgedPlacement = 0;
    std::int32_t BridgedPoints = 0;
    bool DedicatedRpcExpected = false;
    constexpr wchar_t PlacementBridge[] =
        L"ATLAS_ARENA_PLACEMENT_V1:005:000120:1";
    assert(TryParsePlacementBridgeMessage(
        PlacementBridge,
        (sizeof(PlacementBridge) / sizeof(PlacementBridge[0])) - 1,
        &BridgedPlacement, &BridgedPoints, &DedicatedRpcExpected));
    assert(BridgedPlacement == 5);
    assert(BridgedPoints == 120);
    assert(DedicatedRpcExpected);
    constexpr wchar_t VictoryBridge[] =
        L"ATLAS_ARENA_PLACEMENT_V1:001:000060:0";
    assert(TryParsePlacementBridgeMessage(
        VictoryBridge,
        (sizeof(VictoryBridge) / sizeof(VictoryBridge[0])) - 1,
        &BridgedPlacement, &BridgedPoints, &DedicatedRpcExpected));
    assert(BridgedPlacement == 1);
    assert(BridgedPoints == 60);
    assert(!DedicatedRpcExpected);
    constexpr wchar_t ZeroBridge[] =
        L"ATLAS_ARENA_PLACEMENT_V1:000:000060:0";
    assert(!TryParsePlacementBridgeMessage(
        ZeroBridge,
        (sizeof(ZeroBridge) / sizeof(ZeroBridge[0])) - 1,
        &BridgedPlacement, &BridgedPoints, &DedicatedRpcExpected));
    constexpr wchar_t OversizedBridge[] =
        L"ATLAS_ARENA_PLACEMENT_V1:101:000060:0";
    assert(!TryParsePlacementBridgeMessage(
        OversizedBridge,
        (sizeof(OversizedBridge) / sizeof(OversizedBridge[0])) - 1,
        &BridgedPlacement, &BridgedPoints, &DedicatedRpcExpected));
    constexpr wchar_t ShortBridge[] =
        L"ATLAS_ARENA_PLACEMENT_V1:05:000060:0";
    assert(!TryParsePlacementBridgeMessage(
        ShortBridge,
        (sizeof(ShortBridge) / sizeof(ShortBridge[0])) - 1,
        &BridgedPlacement, &BridgedPoints, &DedicatedRpcExpected));
    constexpr wchar_t ZeroPointsBridge[] =
        L"ATLAS_ARENA_PLACEMENT_V1:005:000000:0";
    assert(!TryParsePlacementBridgeMessage(
        ZeroPointsBridge,
        (sizeof(ZeroPointsBridge) / sizeof(ZeroPointsBridge[0])) - 1,
        &BridgedPlacement, &BridgedPoints, &DedicatedRpcExpected));
    constexpr wchar_t InvalidFlagBridge[] =
        L"ATLAS_ARENA_PLACEMENT_V1:005:000120:2";
    assert(!TryParsePlacementBridgeMessage(
        InvalidFlagBridge,
        (sizeof(InvalidFlagBridge) / sizeof(InvalidFlagBridge[0])) - 1,
        &BridgedPlacement, &BridgedPoints, &DedicatedRpcExpected));

    std::int32_t PreviousPlacement = 0;
    constexpr wchar_t PlacementBridgeV2[] =
        L"ATLAS_ARENA_PLACEMENT_V2:101:005:000120:0";
    assert(TryParsePlacementBridgeMessageV2(
        PlacementBridgeV2,
        (sizeof(PlacementBridgeV2) / sizeof(PlacementBridgeV2[0])) - 1,
        &PreviousPlacement, &BridgedPlacement, &BridgedPoints,
        &DedicatedRpcExpected));
    assert(PreviousPlacement == 101);
    assert(BridgedPlacement == 5);
    assert(BridgedPoints == 120);
    assert(!DedicatedRpcExpected);

    constexpr wchar_t VictoryBridgeV2[] =
        L"ATLAS_ARENA_PLACEMENT_V2:002:001:000060:1";
    assert(TryParsePlacementBridgeMessageV2(
        VictoryBridgeV2,
        (sizeof(VictoryBridgeV2) / sizeof(VictoryBridgeV2[0])) - 1,
        &PreviousPlacement, &BridgedPlacement, &BridgedPoints,
        &DedicatedRpcExpected));
    assert(PreviousPlacement == 2);
    assert(BridgedPlacement == 1);
    assert(BridgedPoints == 60);
    assert(DedicatedRpcExpected);

    constexpr wchar_t ReversedBridgeV2[] =
        L"ATLAS_ARENA_PLACEMENT_V2:005:006:000120:0";
    assert(!TryParsePlacementBridgeMessageV2(
        ReversedBridgeV2,
        (sizeof(ReversedBridgeV2) / sizeof(ReversedBridgeV2[0])) - 1,
        &PreviousPlacement, &BridgedPlacement, &BridgedPoints,
        &DedicatedRpcExpected));
    constexpr wchar_t EqualBridgeV2[] =
        L"ATLAS_ARENA_PLACEMENT_V2:005:005:000120:0";
    assert(!TryParsePlacementBridgeMessageV2(
        EqualBridgeV2,
        (sizeof(EqualBridgeV2) / sizeof(EqualBridgeV2[0])) - 1,
        &PreviousPlacement, &BridgedPlacement, &BridgedPoints,
        &DedicatedRpcExpected));
    constexpr wchar_t ZeroPointsBridgeV2[] =
        L"ATLAS_ARENA_PLACEMENT_V2:006:005:000000:0";
    assert(!TryParsePlacementBridgeMessageV2(
        ZeroPointsBridgeV2,
        (sizeof(ZeroPointsBridgeV2) / sizeof(ZeroPointsBridgeV2[0])) - 1,
        &PreviousPlacement, &BridgedPlacement, &BridgedPoints,
        &DedicatedRpcExpected));
    constexpr wchar_t OversizedPreviousBridgeV2[] =
        L"ATLAS_ARENA_PLACEMENT_V2:102:005:000120:0";
    assert(!TryParsePlacementBridgeMessageV2(
        OversizedPreviousBridgeV2,
        (sizeof(OversizedPreviousBridgeV2) /
            sizeof(OversizedPreviousBridgeV2[0])) - 1,
        &PreviousPlacement, &BridgedPlacement, &BridgedPoints,
        &DedicatedRpcExpected));
    constexpr wchar_t InvalidFlagBridgeV2[] =
        L"ATLAS_ARENA_PLACEMENT_V2:006:005:000120:2";
    assert(!TryParsePlacementBridgeMessageV2(
        InvalidFlagBridgeV2,
        (sizeof(InvalidFlagBridgeV2) / sizeof(InvalidFlagBridgeV2[0])) - 1,
        &PreviousPlacement, &BridgedPlacement, &BridgedPoints,
        &DedicatedRpcExpected));

    // A successful kill fallback can use the same numeric placement/points
    // tuple as the next placement award. The newer silent RPC must overwrite
    // that success before the bridge correlates against it.
    assert(DoesPlacementBridgeMatchPresentedRpc(
        5, 20, true, 5, 20));
    assert(!DoesPlacementBridgeMatchPresentedRpc(
        5, 20, false, 5, 20));
    assert(!DoesPlacementBridgeMatchPresentedRpc(
        5, 20, true, 5, 30));

    return 0;
}
