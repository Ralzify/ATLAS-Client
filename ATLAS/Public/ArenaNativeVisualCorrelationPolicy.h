#pragma once

#include "ArenaSessionBridgePolicy.h"

#include <cstddef>
#include <cstdint>
#include <limits>

namespace AtlasArenaNativeVisualCorrelationPolicy
{
    using EArenaEventKind =
        AtlasArenaSessionBridgePolicy::EArenaPresentationEventKind;

    inline constexpr std::size_t NoCorrelation =
        (std::numeric_limits<std::size_t>::max)();

    enum class ECorrelationDecision : std::uint8_t
    {
        NoMatch,
        SuppressFallbackNativePresented,
        DispatchFallbackNativeSilent
    };

    inline constexpr bool ShouldTreatNativeRpcAsPresented(
        bool ObserverInstalled,
        bool ObserverAdvanced,
        bool OriginalDispatched,
        bool ExactNativeOwnedArenaWidgetVisible) noexcept
    {
        // A hooked observer gives definitive evidence either way. Without a
        // hook, a returned native RPC can only be trusted when the exact Arena
        // widget is visibly present; a zero counter is otherwise unknowable.
        return ObserverInstalled
            ? ObserverAdvanced
            : OriginalDispatched && ExactNativeOwnedArenaWidgetVisible;
    }

    inline constexpr bool IsPositiveNativeTournamentAward(
        std::int32_t ObservedValue) noexcept
    {
        // Zero-valued initialization/catch-up notifications do not award
        // Hype and must not consume a pending authoritative visual event.
        return ObservedValue > 0;
    }

    // ClientReportTournamentPlacementPointsScored is not semantically a
    // placement-only RPC on every Fortnite generation. Older clients can use
    // the same reflected contract to present an elimination award. Keep the
    // observation unclassified until its ordered private Arena event arrives.
    struct FPlacementPointsRpcObservation
    {
        std::uint64_t ObservationSequence = 0;
        std::uint64_t SessionOrdinal = 0;
        std::uintptr_t WorldIdentity = 0;
        std::uintptr_t ControllerIdentity = 0;
        std::int32_t Placement = 0;
        std::int32_t PointsEarned = 0;
        std::uint64_t ObservedAtMilliseconds = 0;
        bool PointScoredPresented = false;
        bool TeamPlacementConsumerAdvanced = false;
        bool Active = false;
    };

    struct FAuthoritativeArenaVisualEvent
    {
        std::uint64_t EventSequence = 0;
        std::uint64_t SessionOrdinal = 0;
        std::uintptr_t WorldIdentity = 0;
        std::uintptr_t ControllerIdentity = 0;
        EArenaEventKind Kind = EArenaEventKind::BusFare;
        std::int32_t Placement = 0;
        std::int32_t PointsDelta = 0;
        std::uint64_t ReceivedAtMilliseconds = 0;
        bool NativeGraceExpected = false;
        bool Active = false;
    };

    // Some generations invoke the scoring alert's zero-parameter
    // OnPointScored callback before Magnesium's ordered private Arena frame
    // reaches the client. Unlike the placement RPC, that callback carries no
    // award identity. Correlate it only to the immediately following bus-fare
    // or elimination event, and bind it to the exact native widget that fired.
    struct FNativePointScoredObservation
    {
        std::uint64_t ObservationSequence = 0;
        std::uint64_t SessionOrdinal = 0;
        std::uintptr_t WorldIdentity = 0;
        std::uintptr_t ControllerIdentity = 0;
        std::uintptr_t NativeWidgetIdentity = 0;
        std::uint64_t ObservedAtMilliseconds = 0;
        bool Active = false;
    };

    struct FNativePointScoredEvent
    {
        std::uint64_t EventSequence = 0;
        std::uint64_t SessionOrdinal = 0;
        std::uintptr_t WorldIdentity = 0;
        std::uintptr_t ControllerIdentity = 0;
        std::uintptr_t NativeWidgetIdentity = 0;
        EArenaEventKind Kind = EArenaEventKind::BusFare;
        std::int32_t PointsDelta = 0;
        std::uint64_t ReceivedAtMilliseconds = 0;
        bool NativeGraceExpected = false;
        bool Active = false;
    };

    inline constexpr bool ShouldRecordNativePointScoredObservation(
        bool PresentationConsumer,
        bool DistinctNativeWidget,
        bool OriginalDispatched,
        bool ExplicitBridgeDispatchActive,
        bool TypedNativeDispatchActive,
        bool AuthoritativeSessionActive) noexcept
    {
        // Explicit ATLAS badges also call OnPointScored. Typed native routes
        // already have payload-aware correlation and must not create a second,
        // ambiguous observation for the same presentation.
        return PresentationConsumer && DistinctNativeWidget &&
            OriginalDispatched && !ExplicitBridgeDispatchActive &&
            !TypedNativeDispatchActive && AuthoritativeSessionActive;
    }

    inline constexpr bool IsNativePointScoredEventSupported(
        const FNativePointScoredEvent& Event) noexcept
    {
        if (!Event.Active || !Event.NativeGraceExpected ||
            Event.EventSequence == 0 || Event.SessionOrdinal == 0 ||
            Event.WorldIdentity == 0 || Event.ControllerIdentity == 0 ||
            Event.NativeWidgetIdentity == 0)
        {
            return false;
        }

        if (Event.Kind == EArenaEventKind::BusFare)
            return Event.PointsDelta < 0;
        if (Event.Kind == EArenaEventKind::Elimination)
            return Event.PointsDelta > 0;
        return false;
    }

    inline bool IsNativePointScoredCorrelationMatch(
        const FNativePointScoredObservation& Observation,
        const FNativePointScoredEvent& Event,
        std::uint64_t Now,
        std::uint64_t MaximumBackwardAgeMilliseconds) noexcept
    {
        if (!Observation.Active || Observation.ObservationSequence == 0 ||
            !IsNativePointScoredEventSupported(Event) ||
            Observation.SessionOrdinal == 0 ||
            Observation.WorldIdentity == 0 ||
            Observation.ControllerIdentity == 0 ||
            Observation.NativeWidgetIdentity == 0 ||
            Observation.SessionOrdinal != Event.SessionOrdinal ||
            Observation.WorldIdentity != Event.WorldIdentity ||
            Observation.ControllerIdentity != Event.ControllerIdentity ||
            Observation.NativeWidgetIdentity != Event.NativeWidgetIdentity ||
            Observation.ObservedAtMilliseconds == 0 ||
            Event.ReceivedAtMilliseconds == 0 ||
            Observation.ObservedAtMilliseconds >
                Event.ReceivedAtMilliseconds ||
            Now < Event.ReceivedAtMilliseconds)
        {
            return false;
        }

        const std::uint64_t BackwardAge =
            Event.ReceivedAtMilliseconds -
            Observation.ObservedAtMilliseconds;
        return BackwardAge <= MaximumBackwardAgeMilliseconds &&
            Now - Observation.ObservedAtMilliseconds <=
                MaximumBackwardAgeMilliseconds;
    }

    inline bool IsNativePointScoredReverseCorrelationMatch(
        const FNativePointScoredObservation& Observation,
        const FNativePointScoredEvent& Event,
        std::uint64_t Now,
        std::uint64_t MaximumForwardAgeMilliseconds) noexcept
    {
        if (!Observation.Active || Observation.ObservationSequence == 0 ||
            !IsNativePointScoredEventSupported(Event) ||
            Observation.SessionOrdinal == 0 ||
            Observation.WorldIdentity == 0 ||
            Observation.ControllerIdentity == 0 ||
            Observation.NativeWidgetIdentity == 0 ||
            Observation.SessionOrdinal != Event.SessionOrdinal ||
            Observation.WorldIdentity != Event.WorldIdentity ||
            Observation.ControllerIdentity != Event.ControllerIdentity ||
            Observation.NativeWidgetIdentity != Event.NativeWidgetIdentity ||
            Observation.ObservedAtMilliseconds == 0 ||
            Event.ReceivedAtMilliseconds == 0 ||
            Observation.ObservedAtMilliseconds <
                Event.ReceivedAtMilliseconds ||
            Now < Observation.ObservedAtMilliseconds)
        {
            return false;
        }

        const std::uint64_t ForwardAge =
            Observation.ObservedAtMilliseconds -
            Event.ReceivedAtMilliseconds;
        return ForwardAge <= MaximumForwardAgeMilliseconds &&
            Now - Event.ReceivedAtMilliseconds <=
                MaximumForwardAgeMilliseconds;
    }

    inline std::size_t SelectNewestNativePointScoredObservation(
        const FNativePointScoredObservation* Observations,
        std::size_t ObservationCount,
        const FNativePointScoredEvent& Event,
        std::uint64_t Now,
        std::uint64_t MaximumBackwardAgeMilliseconds) noexcept
    {
        if (!Observations || ObservationCount == 0)
            return NoCorrelation;

        std::size_t Selected = NoCorrelation;
        std::uint64_t SelectedSequence = 0;
        for (std::size_t Index = 0; Index < ObservationCount; ++Index)
        {
            if (!IsNativePointScoredCorrelationMatch(
                    Observations[Index], Event, Now,
                    MaximumBackwardAgeMilliseconds))
            {
                continue;
            }

            const std::uint64_t Sequence =
                Observations[Index].ObservationSequence;
            // Magnesium calls the native gameplay path before emitting its
            // ordered private frame. The closest preceding native callback is
            // therefore the only one that can own this event; earlier
            // callbacks are behind the new authoritative ordering barrier.
            if (Selected == NoCorrelation || Sequence > SelectedSequence)
            {
                Selected = Index;
                SelectedSequence = Sequence;
            }
        }
        return Selected;
    }

    inline bool IsNativePointScoredObservationBehindEventBarrier(
        const FNativePointScoredObservation& Observation,
        const FNativePointScoredEvent& Event) noexcept
    {
        return Observation.Active && Observation.SessionOrdinal != 0 &&
            Observation.WorldIdentity != 0 &&
            Observation.ControllerIdentity != 0 &&
            Observation.NativeWidgetIdentity != 0 &&
            Observation.SessionOrdinal == Event.SessionOrdinal &&
            Observation.WorldIdentity == Event.WorldIdentity &&
            Observation.ControllerIdentity == Event.ControllerIdentity &&
            Observation.NativeWidgetIdentity == Event.NativeWidgetIdentity &&
            Observation.ObservedAtMilliseconds != 0 &&
            Observation.ObservedAtMilliseconds <=
                Event.ReceivedAtMilliseconds;
    }

    inline bool IsFreshAt(
        std::uint64_t Timestamp,
        std::uint64_t Now,
        std::uint64_t MaximumAgeMilliseconds) noexcept
    {
        return Timestamp != 0 && Now >= Timestamp &&
            Now - Timestamp <= MaximumAgeMilliseconds;
    }

    inline bool AreTimestampsCorrelatable(
        std::uint64_t First,
        std::uint64_t Second,
        std::uint64_t Now,
        std::uint64_t MaximumAgeMilliseconds) noexcept
    {
        if (!IsFreshAt(First, Now, MaximumAgeMilliseconds) ||
            !IsFreshAt(Second, Now, MaximumAgeMilliseconds))
        {
            return false;
        }

        const std::uint64_t Later = First > Second ? First : Second;
        const std::uint64_t Earlier = First > Second ? Second : First;
        return Later - Earlier <= MaximumAgeMilliseconds;
    }

    inline bool HasSameValidScope(
        const FPlacementPointsRpcObservation& Observation,
        const FAuthoritativeArenaVisualEvent& Event) noexcept
    {
        return Observation.SessionOrdinal != 0 &&
            Observation.WorldIdentity != 0 &&
            Observation.ControllerIdentity != 0 &&
            Observation.SessionOrdinal == Event.SessionOrdinal &&
            Observation.WorldIdentity == Event.WorldIdentity &&
            Observation.ControllerIdentity == Event.ControllerIdentity;
    }

    inline bool IsSupportedEventIdentity(
        const FAuthoritativeArenaVisualEvent& Event) noexcept
    {
        if (Event.PointsDelta <= 0)
            return false;

        switch (Event.Kind)
        {
        case EArenaEventKind::Elimination:
            return Event.Placement == 0;
        case EArenaEventKind::Placement:
            return Event.Placement > 1 && Event.Placement <= 100;
        case EArenaEventKind::Victory:
            return Event.Placement == 1;
        default:
            return false;
        }
    }

    inline bool IsIdentityCompatible(
        const FPlacementPointsRpcObservation& Observation,
        const FAuthoritativeArenaVisualEvent& Event) noexcept
    {
        if (Observation.Placement <= 0 || Observation.Placement > 100 ||
            Observation.PointsEarned <= 0 ||
            Observation.PointsEarned != Event.PointsDelta ||
            !IsSupportedEventIdentity(Event))
        {
            return false;
        }

        if (Event.Kind == EArenaEventKind::Elimination)
        {
            // A team-placement callback makes the RPC definitively placement
            // owned. A point-only callback remains ambiguous and may be the
            // native elimination path used by older clients.
            return !Observation.TeamPlacementConsumerAdvanced;
        }

        // Placement and victory have an exact identity in the authoritative
        // event, so both fields must agree. This remains valid on versions
        // whose placement RPC advances only OnPointScored.
        return Observation.Placement == Event.Placement;
    }

    inline ECorrelationDecision EvaluateCorrelation(
        const FPlacementPointsRpcObservation& Observation,
        const FAuthoritativeArenaVisualEvent& Event,
        std::uint64_t Now,
        std::uint64_t MaximumAgeMilliseconds) noexcept
    {
        if (!Observation.Active || !Event.Active ||
            Observation.ObservationSequence == 0 ||
            Event.EventSequence == 0 ||
            !Event.NativeGraceExpected ||
            !HasSameValidScope(Observation, Event) ||
            !AreTimestampsCorrelatable(
                Observation.ObservedAtMilliseconds,
                Event.ReceivedAtMilliseconds,
                Now,
                MaximumAgeMilliseconds) ||
            !IsIdentityCompatible(Observation, Event))
        {
            return ECorrelationDecision::NoMatch;
        }

        return Observation.PointScoredPresented
            ? ECorrelationDecision::SuppressFallbackNativePresented
            : ECorrelationDecision::DispatchFallbackNativeSilent;
    }

    inline std::size_t SelectOldestCompatibleObservation(
        const FPlacementPointsRpcObservation* Observations,
        std::size_t ObservationCount,
        const FAuthoritativeArenaVisualEvent& Event,
        std::uint64_t Now,
        std::uint64_t MaximumAgeMilliseconds,
        ECorrelationDecision* Decision = nullptr) noexcept
    {
        if (Decision)
            *Decision = ECorrelationDecision::NoMatch;
        if (!Observations || ObservationCount == 0)
            return NoCorrelation;

        std::size_t Selected = NoCorrelation;
        std::uint64_t SelectedSequence = 0;
        ECorrelationDecision SelectedDecision =
            ECorrelationDecision::NoMatch;
        for (std::size_t Index = 0;
             Index < ObservationCount; ++Index)
        {
            const auto CandidateDecision = EvaluateCorrelation(
                Observations[Index], Event, Now,
                MaximumAgeMilliseconds);
            if (CandidateDecision == ECorrelationDecision::NoMatch)
                continue;

            const std::uint64_t Sequence =
                Observations[Index].ObservationSequence;
            if (Selected == NoCorrelation ||
                Sequence < SelectedSequence)
            {
                Selected = Index;
                SelectedSequence = Sequence;
                SelectedDecision = CandidateDecision;
            }
        }

        if (Decision)
            *Decision = SelectedDecision;
        return Selected;
    }

    inline std::size_t SelectOldestCompatibleEvent(
        const FAuthoritativeArenaVisualEvent* Events,
        std::size_t EventCount,
        const FPlacementPointsRpcObservation& Observation,
        std::uint64_t Now,
        std::uint64_t MaximumAgeMilliseconds,
        ECorrelationDecision* Decision = nullptr) noexcept
    {
        if (Decision)
            *Decision = ECorrelationDecision::NoMatch;
        if (!Events || EventCount == 0)
            return NoCorrelation;

        std::size_t Selected = NoCorrelation;
        std::uint64_t SelectedSequence = 0;
        ECorrelationDecision SelectedDecision =
            ECorrelationDecision::NoMatch;
        for (std::size_t Index = 0; Index < EventCount; ++Index)
        {
            const auto CandidateDecision = EvaluateCorrelation(
                Observation, Events[Index], Now,
                MaximumAgeMilliseconds);
            if (CandidateDecision == ECorrelationDecision::NoMatch)
                continue;

            const std::uint64_t Sequence = Events[Index].EventSequence;
            if (Selected == NoCorrelation ||
                Sequence < SelectedSequence)
            {
                Selected = Index;
                SelectedSequence = Sequence;
                SelectedDecision = CandidateDecision;
            }
        }

        if (Decision)
            *Decision = SelectedDecision;
        return Selected;
    }
}
