#pragma once

#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <limits>

namespace AtlasArenaSessionBridgePolicy
{
    enum class EArenaScoreTextFormatRoute : std::uint8_t
    {
        None,
        ModernShorthand,
        ModernLimitedDigits,
        LegacyShorthand,
        RawInteger
    };

    enum class EArenaPresentationEventKind : wchar_t
    {
        BusFare = L'B',
        Elimination = L'E',
        Placement = L'P',
        Victory = L'V'
    };

    struct FArenaBootstrap
    {
        std::uint64_t SessionOrdinal = 0;
        bool Active = false;
        bool SavingEnabled = false;
        std::int32_t StartingHype = 0;
        std::int32_t ArenaEntryFee = 0;
    };

    struct FArenaPresentationEvent
    {
        std::uint64_t SessionOrdinal = 0;
        std::uint64_t EventSequence = 0;
        EArenaPresentationEventKind Kind =
            EArenaPresentationEventKind::BusFare;
        std::int32_t PointsDelta = 0;
        std::int32_t ResultingHype = 0;
        std::int32_t Placement = 0;
        bool NativeGraceExpected = false;
    };

    struct FArenaEnd
    {
        std::uint64_t SessionOrdinal = 0;
        bool Active = false;
    };

    struct FArenaSessionState
    {
        std::uint64_t SessionOrdinal = 0;
        std::uint64_t LastEventSequence = 0;
        std::int32_t CurrentHype = 0;
        std::int32_t EliminationCount = 0;
        std::int32_t PreviousPlacement = 101;
        std::int32_t ArenaEntryFee = 0;
        bool Active = false;
        bool SavingEnabled = false;
        bool BusFareApplied = false;
    };

    enum class EBootstrapApplyResult : std::uint8_t
    {
        Rejected,
        Initialized,
        Refreshed
    };

    constexpr wchar_t kArenaBootstrapPrefix[] =
        L"ATLAS_ARENA_BOOTSTRAP_V1:";
    constexpr wchar_t kArenaEventPrefix[] =
        L"ATLAS_ARENA_EVENT_V1:";
    constexpr wchar_t kArenaEndPrefix[] =
        L"ATLAS_ARENA_END_V1:";
    constexpr std::size_t kArenaBootstrapPrefixLength =
        (sizeof(kArenaBootstrapPrefix) / sizeof(wchar_t)) - 1;
    constexpr std::size_t kArenaEventPrefixLength =
        (sizeof(kArenaEventPrefix) / sizeof(wchar_t)) - 1;
    constexpr std::size_t kArenaEndPrefixLength =
        (sizeof(kArenaEndPrefix) / sizeof(wchar_t)) - 1;
    constexpr std::size_t kArenaBootstrapMessageLength =
        kArenaBootstrapPrefixLength + 16 + 3 + 1 + 1 + 10 + 1 + 7;
    constexpr std::size_t kArenaEventMessageLength =
        kArenaEventPrefixLength + 16 + 1 + 16 + 1 + 1 + 1 + 7 + 1 +
        10 + 1 + 3 + 1 + 1;
    constexpr std::size_t kArenaEndMessageLength =
        kArenaEndPrefixLength + 16;
    constexpr std::uint64_t kNativeVisualGraceMilliseconds = 1000;
    constexpr std::uint64_t
        kEliminationNativeVisualGraceMilliseconds = 250;
    constexpr std::uint64_t kNativeVisualDedupeMilliseconds = 2000;
    constexpr std::uint64_t kVisualPresentationSpacingMilliseconds = 2000;
    constexpr std::uint64_t kVisualRetryDelayMilliseconds = 250;
    constexpr unsigned kAuthoritativeHudFallbackAttempt = 4;
    constexpr unsigned kNativeHudFallbackAttempt = 12;
    constexpr unsigned kNativeScoreStabilizationWrites = 1;
    constexpr unsigned kOwnedFallbackScoreStabilizationWrites = 8;
    constexpr wchar_t kBusFareTournamentStatName[] =
        L"MATCH_PLAYED_STAT";
    constexpr wchar_t kBusFareTournamentDisplayName[] =
        L"MatchEntryFee";
    constexpr wchar_t kBusFareIconTexturePath[] =
        L"/Game/UI/Events/Icons/"
        L"T-Icon-Bus-Fare-Flat.T-Icon-Bus-Fare-Flat";
    constexpr wchar_t kEliminationIconTexturePath[] =
        L"/Game/Athena/HUD/Art/killsico_alt.KillsIco_Alt";
    constexpr wchar_t kPlacementIconTexturePath[] =
        L"/Game/UI/Foundation/Textures/Icons/Quest/"
        L"T-Icon-Trophy-32.T-Icon-Trophy-32";

    inline const wchar_t* DirectBadgeIconTexturePath(
        EArenaPresentationEventKind Kind) noexcept
    {
        switch (Kind)
        {
        case EArenaPresentationEventKind::BusFare:
            return kBusFareIconTexturePath;
        case EArenaPresentationEventKind::Elimination:
            return kEliminationIconTexturePath;
        case EArenaPresentationEventKind::Placement:
        case EArenaPresentationEventKind::Victory:
            return kPlacementIconTexturePath;
        default:
            return nullptr;
        }
    }

    inline EArenaScoreTextFormatRoute SelectScoreTextFormatRoute(
        double FortniteVersion,
        bool HasModernShorthand,
        bool HasModernLimitedDigits,
        bool HasLegacyShorthand) noexcept
    {
        // Epic kept Arena totals as full integers through Chapter 2. Compact
        // quantity text became the authored Arena presentation in v19.00.
        // The legacy formatter remains the only reflected route in 19.00 and
        // 19.01, so gate by presentation era before selecting capabilities.
        if (!(FortniteVersion >= 19.00))
            return EArenaScoreTextFormatRoute::RawInteger;
        if (HasModernLimitedDigits)
            return EArenaScoreTextFormatRoute::ModernLimitedDigits;
        if (HasModernShorthand)
            return EArenaScoreTextFormatRoute::ModernShorthand;
        if (HasLegacyShorthand)
            return EArenaScoreTextFormatRoute::LegacyShorthand;
        return EArenaScoreTextFormatRoute::RawInteger;
    }

    inline unsigned ScoreStabilizationWriteTarget(
        bool ExactClientOwnedFallback) noexcept
    {
        // Native widgets own their authored formatting and re-arm a repair
        // after any scoring animation. One authoritative write is sufficient.
        // The compatibility widget can populate its point counter over several
        // ticks, so retain the bounded stabilization window only for it.
        return ExactClientOwnedFallback
            ? kOwnedFallbackScoreStabilizationWrites
            : kNativeScoreStabilizationWrites;
    }

    inline bool ShouldMaintainStableScoreText(
        bool ExactClientOwnedFallback) noexcept
    {
        // A native widget will notify the bridge whenever its Blueprint writes
        // the field. Polling it forever creates a competing text owner. The
        // ATLAS-created compatibility widget has no such native notification,
        // so it alone keeps the low-frequency placeholder repair.
        return ExactClientOwnedFallback;
    }

    inline bool ShouldUseNativeVisualGrace(
        bool NativeGraceExpected,
        bool NativeWasObservedSilent,
        bool VerifiedClientOwnedFallbackActive) noexcept
    {
        return NativeGraceExpected &&
            !NativeWasObservedSilent &&
            !VerifiedClientOwnedFallbackActive;
    }

    inline std::uint64_t NativeVisualFallbackDelayMilliseconds(
        EArenaPresentationEventKind Kind,
        bool NativeGraceExpected) noexcept
    {
        if (!NativeGraceExpected)
            return 0;
        // Native elimination callbacks are synchronous or arrive within the
        // first few client ticks in every observed build. Keep a short reverse
        // correlation window without making a silent modern path feel laggy.
        // Placement and victory retain the wider grace because their authored
        // RPC/animation path can legitimately settle later.
        return Kind == EArenaPresentationEventKind::Elimination
            ? kEliminationNativeVisualGraceMilliseconds
            : kNativeVisualGraceMilliseconds;
    }

    inline bool ShouldOpenScoreNeutralHudOpportunity(
        EArenaPresentationEventKind Kind,
        std::int32_t PointsDelta,
        bool NativeGraceExpected) noexcept
    {
        // Nonzero events already remain queued until a presentation owner is
        // available. Only B:0 needs a separate one-shot opportunity to create
        // the total widget when the server proved that no native route exists.
        return Kind == EArenaPresentationEventKind::BusFare &&
            PointsDelta == 0 && !NativeGraceExpected;
    }

    inline bool CanNativeVisualOwnBridgeEvent(
        bool EffectiveNativeGrace,
        std::uint64_t PresentedAt,
        std::uint64_t NextAllowedPresentationAt) noexcept
    {
        // OnPointScored proves that the native path ran, but a call made while
        // another Arena badge owns the widget's presentation window can be
        // visually dropped. Keep the authoritative bridge fallback in that
        // case. A client-owned fallback also never delegates ownership back
        // to a native timestamp merely because the server supplied a hint.
        return EffectiveNativeGrace && PresentedAt != 0 &&
            (NextAllowedPresentationAt == 0 ||
             PresentedAt >= NextAllowedPresentationAt);
    }

    inline std::uint64_t ReserveVisualPresentationDeadline(
        std::uint64_t CurrentDeadline,
        std::uint64_t PresentedAt) noexcept
    {
        if (PresentedAt == 0)
            return CurrentDeadline;
        const auto Maximum =
            (std::numeric_limits<std::uint64_t>::max)();
        const std::uint64_t Candidate = PresentedAt >
                Maximum - kVisualPresentationSpacingMilliseconds
            ? Maximum
            : PresentedAt + kVisualPresentationSpacingMilliseconds;
        return CurrentDeadline > Candidate
            ? CurrentDeadline
            : Candidate;
    }

    inline bool ShouldPreferVisualSequence(
        bool CandidateActive,
        bool CandidateDispatching,
        std::uint64_t CandidateSequence,
        std::uint64_t SelectedSequence) noexcept
    {
        return CandidateActive && !CandidateDispatching &&
            CandidateSequence != 0 &&
            (SelectedSequence == 0 ||
             CandidateSequence < SelectedSequence);
    }

    inline bool ShouldAttemptTournamentStatRoute(
        bool TournamentRouteAttempted) noexcept
    {
        return !TournamentRouteAttempted;
    }

    inline std::uint64_t VisualDispatchDelayMilliseconds(
        bool Presented) noexcept
    {
        return Presented
            ? kVisualPresentationSpacingMilliseconds
            : kVisualRetryDelayMilliseconds;
    }

    inline unsigned SelectHudFallbackAttemptThreshold(
        bool AllowsHudOwnerSource,
        bool AuthoritativeServerSession,
        bool OwnerSchemaValid,
        bool InitialOwnerUpdateAttempted) noexcept
    {
        const bool AuthoritativeFallbackReady =
            AuthoritativeServerSession &&
            (!OwnerSchemaValid || InitialOwnerUpdateAttempted);
        return !AllowsHudOwnerSource || AuthoritativeFallbackReady
            ? kAuthoritativeHudFallbackAttempt
            : kNativeHudFallbackAttempt;
    }

    inline bool ParseHex16(
        const wchar_t* Text,
        std::uint64_t* Value) noexcept
    {
        if (Value)
            *Value = 0;
        if (!Text || !Value)
            return false;

        std::uint64_t Parsed = 0;
        for (std::size_t Index = 0; Index < 16; ++Index)
        {
            const wchar_t Character = Text[Index];
            std::uint64_t Digit = 0;
            if (Character >= L'0' && Character <= L'9')
                Digit = static_cast<std::uint64_t>(Character - L'0');
            else if (Character >= L'A' && Character <= L'F')
                Digit = 10u + static_cast<std::uint64_t>(Character - L'A');
            else
                return false;
            Parsed = (Parsed << 4u) | Digit;
        }
        *Value = Parsed;
        return true;
    }

    inline bool ParseUnsignedDecimal(
        const wchar_t* Text,
        std::size_t CharacterCount,
        std::uint64_t Maximum,
        std::uint64_t* Value) noexcept
    {
        if (Value)
            *Value = 0;
        if (!Text || !CharacterCount || !Value)
            return false;

        std::uint64_t Parsed = 0;
        for (std::size_t Index = 0; Index < CharacterCount; ++Index)
        {
            const wchar_t Character = Text[Index];
            if (Character < L'0' || Character > L'9')
                return false;
            const std::uint64_t Digit =
                static_cast<std::uint64_t>(Character - L'0');
            if (Parsed > (Maximum - Digit) / 10u)
                return false;
            Parsed = Parsed * 10u + Digit;
        }
        *Value = Parsed;
        return true;
    }

    inline bool ParseSignedSeven(
        const wchar_t* Text,
        std::int32_t* Value) noexcept
    {
        if (Value)
            *Value = 0;
        if (!Text || !Value || (Text[0] != L'+' && Text[0] != L'-'))
            return false;

        std::uint64_t Magnitude = 0;
        if (!ParseUnsignedDecimal(Text + 1, 6, 999999, &Magnitude))
            return false;
        *Value = Text[0] == L'-'
            ? -static_cast<std::int32_t>(Magnitude)
            : static_cast<std::int32_t>(Magnitude);
        return true;
    }

    inline bool IsEventShapeValid(
        EArenaPresentationEventKind Kind,
        std::int32_t PointsDelta,
        std::int32_t Placement) noexcept
    {
        switch (Kind)
        {
        case EArenaPresentationEventKind::BusFare:
            return PointsDelta <= 0 && Placement == 0;
        case EArenaPresentationEventKind::Elimination:
            return PointsDelta > 0 && Placement == 0;
        case EArenaPresentationEventKind::Placement:
            return PointsDelta > 0 && Placement > 1 && Placement <= 100;
        case EArenaPresentationEventKind::Victory:
            return PointsDelta > 0 && Placement == 1;
        default:
            return false;
        }
    }

    inline bool TryParseBootstrapMessageV1(
        const wchar_t* Text,
        std::size_t CharacterCount,
        FArenaBootstrap* Bootstrap) noexcept
    {
        if (Bootstrap)
            *Bootstrap = {};
        if (!Text || !Bootstrap ||
            CharacterCount != kArenaBootstrapMessageLength ||
            std::wmemcmp(
                Text, kArenaBootstrapPrefix,
                kArenaBootstrapPrefixLength) != 0)
        {
            return false;
        }

        std::size_t Offset = kArenaBootstrapPrefixLength;
        std::uint64_t SessionOrdinal = 0;
        if (!ParseHex16(Text + Offset, &SessionOrdinal) ||
            SessionOrdinal == 0)
        {
            return false;
        }
        Offset += 16;
        if (Text[Offset++] != L':' || Text[Offset++] != L'A' ||
            Text[Offset++] != L':')
        {
            return false;
        }
        if (Text[Offset] != L'0' && Text[Offset] != L'1')
            return false;
        const bool SavingEnabled = Text[Offset++] == L'1';
        if (Text[Offset++] != L':')
            return false;

        std::uint64_t StartingHype = 0;
        if (!ParseUnsignedDecimal(
                Text + Offset, 10,
                static_cast<std::uint64_t>(
                    (std::numeric_limits<std::int32_t>::max)()),
                &StartingHype))
        {
            return false;
        }
        Offset += 10;
        if (Text[Offset++] != L':')
            return false;
        std::int32_t ArenaEntryFee = 0;
        if (!ParseSignedSeven(Text + Offset, &ArenaEntryFee) ||
            ArenaEntryFee > 0)
        {
            return false;
        }

        Bootstrap->SessionOrdinal = SessionOrdinal;
        Bootstrap->Active = true;
        Bootstrap->SavingEnabled = SavingEnabled;
        Bootstrap->StartingHype =
            static_cast<std::int32_t>(StartingHype);
        Bootstrap->ArenaEntryFee = ArenaEntryFee;
        return true;
    }

    inline bool TryParseEndMessageV1(
        const wchar_t* Text,
        std::size_t CharacterCount,
        FArenaEnd* End) noexcept
    {
        if (End)
            *End = {};
        if (!Text || !End ||
            CharacterCount != kArenaEndMessageLength ||
            std::wmemcmp(
                Text, kArenaEndPrefix,
                kArenaEndPrefixLength) != 0)
        {
            return false;
        }

        if (!ParseHex16(
                Text + kArenaEndPrefixLength,
                &End->SessionOrdinal) ||
            End->SessionOrdinal == 0)
        {
            *End = {};
            return false;
        }
        End->Active = true;
        return true;
    }

    inline bool CanApplyEnd(
        const FArenaSessionState& State,
        const FArenaEnd& End) noexcept
    {
        return State.Active && End.Active &&
            State.SessionOrdinal != 0 &&
            State.SessionOrdinal == End.SessionOrdinal;
    }

    inline bool TryParseEventMessageV1(
        const wchar_t* Text,
        std::size_t CharacterCount,
        FArenaPresentationEvent* Event) noexcept
    {
        if (Event)
            *Event = {};
        if (!Text || !Event ||
            CharacterCount != kArenaEventMessageLength ||
            std::wmemcmp(
                Text, kArenaEventPrefix,
                kArenaEventPrefixLength) != 0)
        {
            return false;
        }

        std::size_t Offset = kArenaEventPrefixLength;
        if (!ParseHex16(Text + Offset, &Event->SessionOrdinal) ||
            Event->SessionOrdinal == 0)
        {
            *Event = {};
            return false;
        }
        Offset += 16;
        if (Text[Offset++] != L':' ||
            !ParseHex16(Text + Offset, &Event->EventSequence) ||
            Event->EventSequence == 0)
        {
            *Event = {};
            return false;
        }
        Offset += 16;
        if (Text[Offset++] != L':')
        {
            *Event = {};
            return false;
        }
        Event->Kind = static_cast<EArenaPresentationEventKind>(
            Text[Offset++]);
        if (Text[Offset++] != L':' ||
            !ParseSignedSeven(Text + Offset, &Event->PointsDelta))
        {
            *Event = {};
            return false;
        }
        Offset += 7;
        if (Text[Offset++] != L':')
        {
            *Event = {};
            return false;
        }

        std::uint64_t ResultingHype = 0;
        if (!ParseUnsignedDecimal(
                Text + Offset, 10,
                static_cast<std::uint64_t>(
                    (std::numeric_limits<std::int32_t>::max)()),
                &ResultingHype))
        {
            *Event = {};
            return false;
        }
        Event->ResultingHype =
            static_cast<std::int32_t>(ResultingHype);
        Offset += 10;
        if (Text[Offset++] != L':')
        {
            *Event = {};
            return false;
        }

        std::uint64_t Placement = 0;
        if (!ParseUnsignedDecimal(Text + Offset, 3, 999, &Placement))
        {
            *Event = {};
            return false;
        }
        Event->Placement = static_cast<std::int32_t>(Placement);
        Offset += 3;
        if (Text[Offset++] != L':' ||
            (Text[Offset] != L'0' && Text[Offset] != L'1'))
        {
            *Event = {};
            return false;
        }
        Event->NativeGraceExpected = Text[Offset] == L'1';
        if (!IsEventShapeValid(
                Event->Kind, Event->PointsDelta, Event->Placement))
        {
            *Event = {};
            return false;
        }
        return true;
    }

    inline EBootstrapApplyResult ApplyBootstrap(
        FArenaSessionState* State,
        const FArenaBootstrap& Bootstrap) noexcept
    {
        if (!State || !Bootstrap.Active ||
            Bootstrap.SessionOrdinal == 0 ||
            Bootstrap.StartingHype < 0 ||
            Bootstrap.ArenaEntryFee > 0)
        {
            return EBootstrapApplyResult::Rejected;
        }

        if (!State->Active ||
            State->SessionOrdinal != Bootstrap.SessionOrdinal)
        {
            *State = {};
            State->SessionOrdinal = Bootstrap.SessionOrdinal;
            State->CurrentHype = Bootstrap.StartingHype;
            State->PreviousPlacement = 101;
            State->ArenaEntryFee = Bootstrap.ArenaEntryFee;
            State->Active = true;
            State->SavingEnabled = Bootstrap.SavingEnabled;
            return EBootstrapApplyResult::Initialized;
        }

        // A retried bootstrap must never roll back awards that were already
        // accepted for this session. Before the first event, however, a newer
        // backend lookup is allowed to refresh the starting total.
        if (State->LastEventSequence == 0)
        {
            State->CurrentHype = Bootstrap.StartingHype;
            State->ArenaEntryFee = Bootstrap.ArenaEntryFee;
        }
        State->SavingEnabled = Bootstrap.SavingEnabled;
        return EBootstrapApplyResult::Refreshed;
    }

    inline bool ApplyPresentationEvent(
        FArenaSessionState* State,
        const FArenaPresentationEvent& Event,
        std::int32_t* PreviousPlacement = nullptr) noexcept
    {
        if (PreviousPlacement)
            *PreviousPlacement = 0;
        if (!State || !State->Active ||
            State->SessionOrdinal != Event.SessionOrdinal ||
            Event.EventSequence == 0 ||
            Event.EventSequence <= State->LastEventSequence ||
            Event.ResultingHype < 0 ||
            !IsEventShapeValid(
                Event.Kind, Event.PointsDelta, Event.Placement))
        {
            return false;
        }

        const std::int64_t UnclampedHype =
            static_cast<std::int64_t>(State->CurrentHype) +
            static_cast<std::int64_t>(Event.PointsDelta);
        const std::int32_t ExpectedHype = UnclampedHype <= 0
            ? 0
            : (UnclampedHype >=
                    (std::numeric_limits<std::int32_t>::max)()
                ? (std::numeric_limits<std::int32_t>::max)()
                : static_cast<std::int32_t>(UnclampedHype));
        if (Event.ResultingHype != ExpectedHype)
            return false;

        if (Event.Kind == EArenaPresentationEventKind::BusFare &&
            (State->BusFareApplied ||
             Event.PointsDelta != State->ArenaEntryFee))
        {
            return false;
        }
        if ((Event.Kind == EArenaPresentationEventKind::Placement ||
             Event.Kind == EArenaPresentationEventKind::Victory) &&
            Event.Placement >= State->PreviousPlacement)
        {
            return false;
        }

        if (PreviousPlacement)
            *PreviousPlacement = State->PreviousPlacement;
        State->LastEventSequence = Event.EventSequence;
        State->CurrentHype = Event.ResultingHype;
        if (Event.Kind == EArenaPresentationEventKind::BusFare)
            State->BusFareApplied = true;
        if (Event.Kind == EArenaPresentationEventKind::Elimination)
            ++State->EliminationCount;
        if (Event.Kind == EArenaPresentationEventKind::Placement ||
            Event.Kind == EArenaPresentationEventKind::Victory)
        {
            State->PreviousPlacement = Event.Placement;
        }
        return true;
    }
}
