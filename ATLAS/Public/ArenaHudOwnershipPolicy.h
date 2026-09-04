#pragma once

#include <cstdint>

namespace AtlasArenaHudOwnershipPolicy
{
    constexpr std::uint64_t kOwnedFallbackNativeProbeMilliseconds = 2000;

    enum class EVisibleAlertSource : std::uint8_t
    {
        Native,
        OwnedFallback
    };

    struct FVisibleAlertPreference
    {
        EVisibleAlertSource Source = EVisibleAlertSource::Native;
        bool IsCached = false;
        std::int32_t ObjectIndex = -1;
    };

    struct FObjectIdentity
    {
        const void* Address = nullptr;
        std::int32_t ObjectIndex = -1;
        std::int32_t SerialNumber = 0;
    };

    inline bool IsSameObjectInstance(
        const FObjectIdentity& Left,
        const FObjectIdentity& Right) noexcept
    {
        return Left.Address && Right.Address &&
            Left.Address == Right.Address &&
            Left.ObjectIndex == Right.ObjectIndex &&
            Left.SerialNumber != 0 &&
            Left.SerialNumber == Right.SerialNumber;
    }

    inline bool CanAdoptVisibleUnmatchedAlert(
        bool HasExactlyOneLocalPlayer,
        bool IsInViewport) noexcept
    {
        // During travel, an authored Arena widget can briefly retain the
        // temporary controller that created it. In a single-local-player
        // client, a visible alert is still the one authoritative local HUD.
        return HasExactlyOneLocalPlayer && IsInViewport;
    }

    inline bool CanAdoptObservedNativeAlert(
        bool IsDistinctFromOwnedFallback,
        bool IsScoringAlert,
        bool OwningPlayerQuerySupported,
        bool OwningPlayerMatches,
        bool HasExactlyOneLocalPlayer,
        bool IsDirectlyInViewport,
        bool IsVisibleThroughHudHierarchy) noexcept
    {
        // A UI-extension widget is normally nested below AthenaHUDBase. UE's
        // UUserWidget::IsInViewport reports only direct screen ownership on
        // several generations, so an exact native scoring-consumer callback
        // plus hierarchy visibility is the stable evidence for that path.
        // A supported, mismatching owner always fails closed; the
        // single-local-player allowance is only for builds without a usable
        // GetOwningPlayer contract.
        const bool OwnerMatches = OwningPlayerQuerySupported
            ? OwningPlayerMatches
            : HasExactlyOneLocalPlayer;
        return IsDistinctFromOwnedFallback && IsScoringAlert &&
            OwnerMatches &&
            (IsDirectlyInViewport || IsVisibleThroughHudHierarchy);
    }

    inline bool CanAdoptLegacyOwnerMatchedNativeAlert(
        bool IsLegacyPlaylistOwnerEra,
        bool IsDistinctFromOwnedFallback,
        bool IsScoringAlert,
        bool OwningPlayerQuerySupported,
        bool OwningPlayerMatches) noexcept
    {
        // Fortnite 10.x creates ArenaScoringHUD from the playlist extension
        // before its nested widget reports IsInViewport, a usable parent, or
        // IsRendered. An exact local-player owner is sufficient HUD ownership
        // evidence in that one era, but deliberately is not evidence that a
        // scoring animation was presented.
        return IsLegacyPlaylistOwnerEra &&
            IsDistinctFromOwnedFallback && IsScoringAlert &&
            OwningPlayerQuerySupported && OwningPlayerMatches;
    }

    inline bool CanRetireOwnedFallbackForNative(
        bool NativeSelected,
        bool ExactOwnedIdentityMatches,
        std::uint64_t PresentationDeadline,
        std::uint64_t NowMilliseconds) noexcept
    {
        // Select the native widget immediately for new score writes, then
        // retire only the exact ATLAS-owned UObject instance at the fixed
        // handoff deadline. Native widgets are never removable here.
        return NativeSelected && ExactOwnedIdentityMatches &&
            NowMilliseconds >= PresentationDeadline;
    }

    inline bool ShouldSuppressOwnedFallbackConsumer(
        bool ContextIsExactOwnedFallback,
        bool ExplicitClientFallbackDispatch) noexcept
    {
        // Unreal can broadcast the same native scoring update to every live
        // instance before callback order reveals the authored widget. The
        // owned fallback is driven only by ATLAS's explicit bridge dispatch;
        // suppress all other callbacks so it cannot double-present or mutate
        // TournamentStats regardless of native/owned invocation order.
        return ContextIsExactOwnedFallback &&
            !ExplicitClientFallbackDispatch;
    }

    inline bool CanDispatchStateChangingTournamentStatFallback(
        bool ExactOwnedFallbackIsActive,
        bool NativeTakeoverPending) noexcept
    {
        // OnTournamentStatUpdate mutates TournamentStats in addition to
        // presenting a badge. Once a native consumer exists, replaying it on
        // the fallback can double-apply Hype in Fortnite's local manager.
        return ExactOwnedFallbackIsActive && !NativeTakeoverPending;
    }

    inline bool PreferVisibleAlert(
        const FVisibleAlertPreference& Candidate,
        const FVisibleAlertPreference& Current,
        bool PreferNewestNative = false) noexcept
    {
        // The oldest native widget is the authored HUD source on legacy
        // clients that construct the same extension twice. A cached appended
        // widget must not displace it. Any native source still beats a
        // directly-created fallback.
        const auto Priority = [](const FVisibleAlertPreference& Value)
        {
            if (Value.Source == EVisibleAlertSource::Native)
                return 0;
            return Value.IsCached ? 1 : 2;
        };
        const int CandidatePriority = Priority(Candidate);
        const int CurrentPriority = Priority(Current);
        if (CandidatePriority != CurrentPriority)
            return CandidatePriority < CurrentPriority;
        if (PreferNewestNative &&
            Candidate.Source == EVisibleAlertSource::Native)
        {
            if (Candidate.ObjectIndex < 0)
                return false;
            return Current.ObjectIndex < 0 ||
                Candidate.ObjectIndex > Current.ObjectIndex;
        }
        if (Candidate.ObjectIndex < 0)
            return false;
        return Current.ObjectIndex < 0 ||
            Candidate.ObjectIndex < Current.ObjectIndex;
    }

    inline bool ShouldScanVisibleAlerts(
        bool CachedAlertVisible,
        EVisibleAlertSource CachedSource,
        bool ExplicitAuditRequested,
        std::uint64_t NowMilliseconds,
        std::uint64_t NextProbeAtMilliseconds) noexcept
    {
        // A client-created alert is provisional. The native playlist/HUD path
        // can finish constructing later, so periodically re-audit only while
        // the fallback owns the viewport. Once a native alert is selected,
        // the normal cached fast path remains allocation- and scan-free.
        return ExplicitAuditRequested || !CachedAlertVisible ||
            (CachedSource == EVisibleAlertSource::OwnedFallback &&
             NowMilliseconds >= NextProbeAtMilliseconds);
    }

    inline bool CanCreateOwnedFallbackForSession(
        bool AuthoritativeServerSession,
        std::uint64_t ActiveSessionOrdinal,
        std::uint64_t SuppressedSessionOrdinal,
        std::uint64_t NativeOwnedSessionOrdinal = 0) noexcept
    {
        // A native widget may be replaced between spawn island, aircraft, and
        // gameplay. Once it has owned this session, never fill that temporary
        // replacement gap with a raw AddToViewport fallback: the fallback
        // ignores HUD scale and will overlap the next authored instance.
        if (AuthoritativeServerSession && ActiveSessionOrdinal != 0 &&
            ActiveSessionOrdinal == NativeOwnedSessionOrdinal)
        {
            return false;
        }

        // Playlist-only repair has no authoritative match lifetime. A bridge
        // session that has completed must not recreate its manual overlay;
        // the next ordinal automatically becomes eligible again.
        return !AuthoritativeServerSession || ActiveSessionOrdinal == 0 ||
            ActiveSessionOrdinal != SuppressedSessionOrdinal;
    }

    inline bool ShouldReserveLegacyNativeHudOwnership(
        bool IsLegacyPlaylistOwnerEra,
        bool AuthoritativeServerSession,
        bool ActiveArenaPlaylist) noexcept
    {
        // In 10.x the authoritative Arena playlist itself owns the HUD
        // extension. Reserve that native lifetime as soon as the private
        // session arrives, before the aircraft's late widget construction can
        // leave room for a raw viewport fallback.
        return IsLegacyPlaylistOwnerEra &&
            AuthoritativeServerSession && ActiveArenaPlaylist;
    }

    inline bool ShouldCreateOwnedFallbackNow(
        bool SessionAllowsFallback,
        bool AuthoritativeServerSession,
        bool AuthoritativeFallbackOpportunityReached) noexcept
    {
        // A playlist marks the authored Arena HUD as eligible, but it is not
        // authority to add a second viewport widget. Legacy playlist
        // extensions (notably 10.x) can be visible long before IsInViewport or
        // hierarchy inspection can prove them. Wait until an ordered server
        // visual is actually due, or a score-neutral bus-fare opportunity has
        // proved that no native route exists, before creating ATLAS's
        // compatibility widget. This also preserves the fallback for forced
        // Arena sessions whose client playlist was not selected in frontend.
        return SessionAllowsFallback && AuthoritativeServerSession &&
            AuthoritativeFallbackOpportunityReached;
    }

    inline bool CanFinalizeOwnedFallbackRetirement(
        std::uint64_t ActiveSessionOrdinal,
        std::uint64_t TerminalSessionOrdinal,
        bool HasPendingVisual,
        std::uint64_t PresentationOrRetryDeadline,
        std::uint64_t NowMilliseconds) noexcept
    {
        // Terminal receipt does not suppress creation by itself. A late final
        // visual may still need to create the fallback; suppression and
        // removal become atomic only after its queue and presentation window
        // have both completed.
        return ActiveSessionOrdinal != 0 &&
            ActiveSessionOrdinal == TerminalSessionOrdinal &&
            !HasPendingVisual &&
            NowMilliseconds >= PresentationOrRetryDeadline;
    }

}
