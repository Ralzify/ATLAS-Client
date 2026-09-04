#include "../ATLAS/Public/ArenaNativeVisualCorrelationPolicy.h"

#include <cassert>
#include <cstddef>

int main()
{
    using namespace AtlasArenaNativeVisualCorrelationPolicy;

    constexpr std::uint64_t MaximumAge = 2000;
    constexpr std::uint64_t Now = 3000;

    assert(ShouldTreatNativeRpcAsPresented(
        true, true, true, true));
    assert(!ShouldTreatNativeRpcAsPresented(
        true, false, true, true));
    assert(ShouldTreatNativeRpcAsPresented(
        false, false, true, true));
    assert(!ShouldTreatNativeRpcAsPresented(
        false, false, true, false));
    assert(!ShouldTreatNativeRpcAsPresented(
        false, false, false, true));
    assert(IsPositiveNativeTournamentAward(1));
    assert(!IsPositiveNativeTournamentAward(0));
    assert(!IsPositiveNativeTournamentAward(-1));

    FPlacementPointsRpcObservation EliminationRpc{
        10, 7, 0x1000, 0x2000,
        42, 20, 2500,
        true, false, true
    };
    FAuthoritativeArenaVisualEvent EliminationEvent{
        3, 7, 0x1000, 0x2000,
        EArenaEventKind::Elimination,
        0, 20, 2600,
        true, true
    };

    // Season-10-style point-only placement RPCs are not classified until the
    // authoritative E event arrives. A presented native award suppresses only
    // that exact bridge fallback.
    assert(EvaluateCorrelation(
        EliminationRpc, EliminationEvent, Now, MaximumAge) ==
        ECorrelationDecision::SuppressFallbackNativePresented);

    auto SilentEliminationRpc = EliminationRpc;
    SilentEliminationRpc.PointScoredPresented = false;
    assert(EvaluateCorrelation(
        SilentEliminationRpc, EliminationEvent, Now, MaximumAge) ==
        ECorrelationDecision::DispatchFallbackNativeSilent);

    // A native callback inside another badge's protected presentation window
    // is an internal stat advance, not proof that its notification was seen.
    auto SpacingBlockedEliminationRpc = EliminationRpc;
    SpacingBlockedEliminationRpc.PointScoredPresented =
        AtlasArenaSessionBridgePolicy::CanNativeVisualOwnBridgeEvent(
            true,
            SpacingBlockedEliminationRpc.ObservedAtMilliseconds,
            2600);
    assert(!SpacingBlockedEliminationRpc.PointScoredPresented);
    assert(EvaluateCorrelation(
        SpacingBlockedEliminationRpc, EliminationEvent,
        Now, MaximumAge) ==
        ECorrelationDecision::DispatchFallbackNativeSilent);

    // A verified client-owned HUD disables native ownership before
    // correlation, even if the server frame carried a native-grace hint.
    auto ClientOwnedEliminationEvent = EliminationEvent;
    ClientOwnedEliminationEvent.NativeGraceExpected =
        AtlasArenaSessionBridgePolicy::ShouldUseNativeVisualGrace(
            true, false, true);
    assert(EvaluateCorrelation(
        EliminationRpc, ClientOwnedEliminationEvent,
        Now, MaximumAge) == ECorrelationDecision::NoMatch);

    // A callback that advanced the team-placement consumer is definitively
    // placement-owned and cannot suppress an elimination.
    auto KnownPlacementRpc = EliminationRpc;
    KnownPlacementRpc.TeamPlacementConsumerAdvanced = true;
    assert(EvaluateCorrelation(
        KnownPlacementRpc, EliminationEvent, Now, MaximumAge) ==
        ECorrelationDecision::NoMatch);

    auto WrongEliminationPoints = EliminationEvent;
    WrongEliminationPoints.PointsDelta = 30;
    assert(EvaluateCorrelation(
        EliminationRpc, WrongEliminationPoints, Now, MaximumAge) ==
        ECorrelationDecision::NoMatch);

    FPlacementPointsRpcObservation PlacementRpc{
        11, 7, 0x1000, 0x2000,
        25, 60, 2700,
        true, false, true
    };
    FAuthoritativeArenaVisualEvent PlacementEvent{
        4, 7, 0x1000, 0x2000,
        EArenaEventKind::Placement,
        25, 60, 2750,
        true, true
    };
    assert(EvaluateCorrelation(
        PlacementRpc, PlacementEvent, Now, MaximumAge) ==
        ECorrelationDecision::SuppressFallbackNativePresented);

    auto WrongPlacement = PlacementEvent;
    WrongPlacement.Placement = 24;
    assert(EvaluateCorrelation(
        PlacementRpc, WrongPlacement, Now, MaximumAge) ==
        ECorrelationDecision::NoMatch);

    FPlacementPointsRpcObservation VictoryRpc{
        12, 7, 0x1000, 0x2000,
        1, 60, 2800,
        true, true, true
    };
    FAuthoritativeArenaVisualEvent VictoryEvent{
        5, 7, 0x1000, 0x2000,
        EArenaEventKind::Victory,
        1, 60, 2850,
        true, true
    };
    assert(EvaluateCorrelation(
        VictoryRpc, VictoryEvent, Now, MaximumAge) ==
        ECorrelationDecision::SuppressFallbackNativePresented);

    auto BusFareEvent = EliminationEvent;
    BusFareEvent.Kind = EArenaEventKind::BusFare;
    assert(EvaluateCorrelation(
        EliminationRpc, BusFareEvent, Now, MaximumAge) ==
        ECorrelationDecision::NoMatch);

    auto NoNativeGrace = EliminationEvent;
    NoNativeGrace.NativeGraceExpected = false;
    assert(EvaluateCorrelation(
        EliminationRpc, NoNativeGrace, Now, MaximumAge) ==
        ECorrelationDecision::NoMatch);

    auto WrongWorld = EliminationEvent;
    WrongWorld.WorldIdentity = 0x1001;
    assert(EvaluateCorrelation(
        EliminationRpc, WrongWorld, Now, MaximumAge) ==
        ECorrelationDecision::NoMatch);
    auto WrongController = EliminationEvent;
    WrongController.ControllerIdentity = 0x2001;
    assert(EvaluateCorrelation(
        EliminationRpc, WrongController, Now, MaximumAge) ==
        ECorrelationDecision::NoMatch);
    auto WrongSession = EliminationEvent;
    WrongSession.SessionOrdinal = 8;
    assert(EvaluateCorrelation(
        EliminationRpc, WrongSession, Now, MaximumAge) ==
        ECorrelationDecision::NoMatch);

    auto InvalidScope = EliminationRpc;
    InvalidScope.ControllerIdentity = 0;
    assert(EvaluateCorrelation(
        InvalidScope, EliminationEvent, Now, MaximumAge) ==
        ECorrelationDecision::NoMatch);

    auto Expired = EliminationRpc;
    Expired.ObservedAtMilliseconds = Now - MaximumAge - 1;
    assert(EvaluateCorrelation(
        Expired, EliminationEvent, Now, MaximumAge) ==
        ECorrelationDecision::NoMatch);
    auto Future = EliminationEvent;
    Future.ReceivedAtMilliseconds = Now + 1;
    assert(EvaluateCorrelation(
        EliminationRpc, Future, Now, MaximumAge) ==
        ECorrelationDecision::NoMatch);

    auto AtAgeBoundary = EliminationRpc;
    AtAgeBoundary.ObservedAtMilliseconds = Now - MaximumAge;
    auto BoundaryEvent = EliminationEvent;
    BoundaryEvent.ReceivedAtMilliseconds = Now;
    assert(EvaluateCorrelation(
        AtAgeBoundary, BoundaryEvent, Now, MaximumAge) ==
        ECorrelationDecision::SuppressFallbackNativePresented);

    // FIFO correlation disambiguates a kill and placement that have the same
    // numeric tuple. The authoritative E event is ordered first, consumes the
    // ambiguous observation, and leaves the known placement for P.
    FPlacementPointsRpcObservation SameTupleObservations[] = {
        {
            20, 7, 0x1000, 0x2000,
            5, 20, 2500,
            true, true, true
        },
        {
            10, 7, 0x1000, 0x2000,
            5, 20, 2400,
            true, false, true
        },
        {
            30, 7, 0x1000, 0x2000,
            5, 30, 2450,
            true, false, true
        },
    };
    auto SameTupleElimination = EliminationEvent;
    SameTupleElimination.PointsDelta = 20;
    ECorrelationDecision Decision = ECorrelationDecision::NoMatch;
    std::size_t Selected = SelectOldestCompatibleObservation(
        SameTupleObservations,
        sizeof(SameTupleObservations) /
            sizeof(SameTupleObservations[0]),
        SameTupleElimination, Now, MaximumAge, &Decision);
    assert(Selected == 1);
    assert(Decision ==
        ECorrelationDecision::SuppressFallbackNativePresented);
    SameTupleObservations[Selected].Active = false;

    auto SameTuplePlacement = PlacementEvent;
    SameTuplePlacement.Placement = 5;
    SameTuplePlacement.PointsDelta = 20;
    Selected = SelectOldestCompatibleObservation(
        SameTupleObservations,
        sizeof(SameTupleObservations) /
            sizeof(SameTupleObservations[0]),
        SameTuplePlacement, Now, MaximumAge, &Decision);
    assert(Selected == 0);
    assert(Decision ==
        ECorrelationDecision::SuppressFallbackNativePresented);

    // Reverse arrival is also ordered by the authoritative event sequence.
    FAuthoritativeArenaVisualEvent PendingEvents[] = {
        SameTuplePlacement,
        SameTupleElimination,
        NoNativeGrace,
    };
    PendingEvents[0].EventSequence = 9;
    PendingEvents[1].EventSequence = 8;
    PendingEvents[2].EventSequence = 1;
    Selected = SelectOldestCompatibleEvent(
        PendingEvents,
        sizeof(PendingEvents) / sizeof(PendingEvents[0]),
        EliminationRpc, Now, MaximumAge, &Decision);
    assert(Selected == 1);
    assert(Decision ==
        ECorrelationDecision::SuppressFallbackNativePresented);

    // A silent observation remains part of the ordered stream. Once an older
    // native success has been claimed by its event, the silent invocation must
    // win before a later presented invocation and force this event's fallback.
    FPlacementPointsRpcObservation SilentBarrier[] = {
        PlacementRpc,
        PlacementRpc,
        PlacementRpc,
    };
    SilentBarrier[0].ObservationSequence = 1;
    SilentBarrier[0].Active = false;
    SilentBarrier[1].ObservationSequence = 2;
    SilentBarrier[1].PointScoredPresented = false;
    SilentBarrier[2].ObservationSequence = 3;
    SilentBarrier[2].PointScoredPresented = true;
    Selected = SelectOldestCompatibleObservation(
        SilentBarrier,
        sizeof(SilentBarrier) / sizeof(SilentBarrier[0]),
        PlacementEvent, Now, MaximumAge, &Decision);
    assert(Selected == 1);
    assert(Decision ==
        ECorrelationDecision::DispatchFallbackNativeSilent);

    assert(SelectOldestCompatibleObservation(
        nullptr, 0, PlacementEvent, Now, MaximumAge) ==
        NoCorrelation);
    assert(SelectOldestCompatibleEvent(
        nullptr, 0, PlacementRpc, Now, MaximumAge) ==
        NoCorrelation);

    assert(ShouldRecordNativePointScoredObservation(
        true, true, true, false, false, true));
    assert(!ShouldRecordNativePointScoredObservation(
        true, true, true, true, false, true));
    assert(!ShouldRecordNativePointScoredObservation(
        true, true, true, false, true, true));
    assert(!ShouldRecordNativePointScoredObservation(
        true, true, false, false, false, true));
    assert(!ShouldRecordNativePointScoredObservation(
        true, false, true, false, false, true));
    assert(!ShouldRecordNativePointScoredObservation(
        true, true, true, false, false, false));

    constexpr std::uint64_t GenericMaximumAge = 1000;
    FNativePointScoredObservation GenericObservations[] = {
        { 20, 7, 0x1000, 0x2000, 0x3000, 2475, true },
        { 10, 7, 0x1000, 0x2000, 0x3000, 2400, true },
        { 30, 7, 0x1000, 0x2000, 0x3001, 2500, true },
    };
    FNativePointScoredEvent GenericElimination{
        8, 7, 0x1000, 0x2000, 0x3000,
        EArenaEventKind::Elimination, 20, 2600, true, true
    };
    Selected = SelectNewestNativePointScoredObservation(
        GenericObservations,
        sizeof(GenericObservations) / sizeof(GenericObservations[0]),
        GenericElimination, 2600, GenericMaximumAge);
    assert(Selected == 0);
    GenericObservations[Selected].Active = false;
    Selected = SelectNewestNativePointScoredObservation(
        GenericObservations,
        sizeof(GenericObservations) / sizeof(GenericObservations[0]),
        GenericElimination, 2600, GenericMaximumAge);
    assert(Selected == 1);

    // Once an event arrives, every older callback in its exact scope is
    // behind the authoritative barrier and cannot be reused by a later kill.
    assert(IsNativePointScoredObservationBehindEventBarrier(
        GenericObservations[1], GenericElimination));
    assert(!IsNativePointScoredObservationBehindEventBarrier(
        GenericObservations[2], GenericElimination));
    const auto GenericActiveObservation = GenericObservations[1];

    auto GenericBusFare = GenericElimination;
    GenericBusFare.Kind = EArenaEventKind::BusFare;
    GenericBusFare.PointsDelta = -50;
    assert(IsNativePointScoredCorrelationMatch(
        GenericActiveObservation, GenericBusFare,
        2600, GenericMaximumAge));
    auto GenericZeroFare = GenericBusFare;
    GenericZeroFare.PointsDelta = 0;
    assert(!IsNativePointScoredCorrelationMatch(
        GenericActiveObservation, GenericZeroFare,
        2600, GenericMaximumAge));

    // B:0 is not a presentation, but it is an ordering barrier. Retiring its
    // preceding same-scope callback prevents that callback from claiming the
    // first elimination; a different widget remains untouched.
    FNativePointScoredObservation ZeroFareBarrierObservations[] = {
        GenericActiveObservation,
        GenericObservations[2],
    };
    for (auto& Observation : ZeroFareBarrierObservations)
    {
        if (IsNativePointScoredObservationBehindEventBarrier(
                Observation, GenericZeroFare))
        {
            Observation.Active = false;
        }
    }
    assert(!ZeroFareBarrierObservations[0].Active);
    assert(ZeroFareBarrierObservations[1].Active);
    assert(SelectNewestNativePointScoredObservation(
        ZeroFareBarrierObservations,
        sizeof(ZeroFareBarrierObservations) /
            sizeof(ZeroFareBarrierObservations[0]),
        GenericElimination, 2600, GenericMaximumAge) == NoCorrelation);

    auto GenericPlacement = GenericElimination;
    GenericPlacement.Kind = EArenaEventKind::Placement;
    assert(!IsNativePointScoredCorrelationMatch(
        GenericActiveObservation, GenericPlacement,
        2600, GenericMaximumAge));
    auto GenericVictory = GenericElimination;
    GenericVictory.Kind = EArenaEventKind::Victory;
    assert(!IsNativePointScoredCorrelationMatch(
        GenericActiveObservation, GenericVictory,
        2600, GenericMaximumAge));

    auto WrongGenericWidget = GenericElimination;
    WrongGenericWidget.NativeWidgetIdentity = 0x3001;
    assert(!IsNativePointScoredCorrelationMatch(
        GenericActiveObservation, WrongGenericWidget,
        2600, GenericMaximumAge));
    auto WrongGenericSession = GenericElimination;
    WrongGenericSession.SessionOrdinal = 8;
    assert(!IsNativePointScoredCorrelationMatch(
        GenericActiveObservation, WrongGenericSession,
        2600, GenericMaximumAge));
    auto WrongGenericWorld = GenericElimination;
    WrongGenericWorld.WorldIdentity = 0x1001;
    assert(!IsNativePointScoredCorrelationMatch(
        GenericActiveObservation, WrongGenericWorld,
        2600, GenericMaximumAge));
    auto WrongGenericController = GenericElimination;
    WrongGenericController.ControllerIdentity = 0x2001;
    assert(!IsNativePointScoredCorrelationMatch(
        GenericActiveObservation, WrongGenericController,
        2600, GenericMaximumAge));

    auto FutureGeneric = GenericActiveObservation;
    FutureGeneric.ObservedAtMilliseconds = 2601;
    assert(!IsNativePointScoredCorrelationMatch(
        FutureGeneric, GenericElimination,
        2601, GenericMaximumAge));
    auto ExpiredGeneric = GenericActiveObservation;
    ExpiredGeneric.ObservedAtMilliseconds =
        GenericElimination.ReceivedAtMilliseconds -
        GenericMaximumAge - 1;
    assert(!IsNativePointScoredCorrelationMatch(
        ExpiredGeneric, GenericElimination,
        2600, GenericMaximumAge));
    auto AtGenericBoundary = GenericActiveObservation;
    AtGenericBoundary.ObservedAtMilliseconds =
        GenericElimination.ReceivedAtMilliseconds - GenericMaximumAge;
    assert(IsNativePointScoredCorrelationMatch(
        AtGenericBoundary, GenericElimination,
        2600, GenericMaximumAge));

    auto ReverseObservation = GenericActiveObservation;
    ReverseObservation.ObservedAtMilliseconds = 2650;
    assert(IsNativePointScoredReverseCorrelationMatch(
        ReverseObservation, GenericElimination,
        2650, GenericMaximumAge));
    assert(!IsNativePointScoredCorrelationMatch(
        ReverseObservation, GenericElimination,
        2650, GenericMaximumAge));
    auto ReverseExpired = ReverseObservation;
    ReverseExpired.ObservedAtMilliseconds =
        GenericElimination.ReceivedAtMilliseconds +
        GenericMaximumAge + 1;
    assert(!IsNativePointScoredReverseCorrelationMatch(
        ReverseExpired, GenericElimination,
        ReverseExpired.ObservedAtMilliseconds,
        GenericMaximumAge));
    assert(!IsNativePointScoredReverseCorrelationMatch(
        ReverseObservation, GenericPlacement,
        2650, GenericMaximumAge));

    return 0;
}
