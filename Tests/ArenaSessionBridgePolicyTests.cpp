#include "../ATLAS/Public/ArenaSessionBridgePolicy.h"

#include <cassert>
#include <cwchar>

int main()
{
    using namespace AtlasArenaSessionBridgePolicy;

    // Pre-v19 Arena authored full totals even though the inventory shorthand
    // helper was already present in those clients.
    assert(SelectScoreTextFormatRoute(10.40, true, true, true) ==
        EArenaScoreTextFormatRoute::RawInteger);
    assert(SelectScoreTextFormatRoute(18.40, false, false, true) ==
        EArenaScoreTextFormatRoute::RawInteger);

    // v19.00/19.01 still need the legacy helper; later reflected formatters
    // take precedence as soon as they are available.
    assert(SelectScoreTextFormatRoute(19.00, false, false, true) ==
        EArenaScoreTextFormatRoute::LegacyShorthand);
    assert(SelectScoreTextFormatRoute(19.01, false, false, true) ==
        EArenaScoreTextFormatRoute::LegacyShorthand);
    assert(SelectScoreTextFormatRoute(19.10, true, true, true) ==
        EArenaScoreTextFormatRoute::ModernLimitedDigits);
    assert(SelectScoreTextFormatRoute(19.10, true, false, true) ==
        EArenaScoreTextFormatRoute::ModernShorthand);
    assert(SelectScoreTextFormatRoute(19.10, false, true, true) ==
        EArenaScoreTextFormatRoute::ModernLimitedDigits);
    assert(SelectScoreTextFormatRoute(19.10, false, false, true) ==
        EArenaScoreTextFormatRoute::LegacyShorthand);
    assert(SelectScoreTextFormatRoute(19.10, false, false, false) ==
        EArenaScoreTextFormatRoute::RawInteger);
    assert(ScoreStabilizationWriteTarget(false) == 1);
    assert(ScoreStabilizationWriteTarget(true) == 8);
    assert(!ShouldMaintainStableScoreText(false));
    assert(ShouldMaintainStableScoreText(true));

    assert(ShouldUseNativeVisualGrace(true, false, false));
    assert(!ShouldUseNativeVisualGrace(false, false, false));
    assert(!ShouldUseNativeVisualGrace(true, true, false));
    assert(!ShouldUseNativeVisualGrace(true, false, true));

    assert(NativeVisualFallbackDelayMilliseconds(
        EArenaPresentationEventKind::Elimination, true) ==
        kEliminationNativeVisualGraceMilliseconds);
    assert(NativeVisualFallbackDelayMilliseconds(
        EArenaPresentationEventKind::Placement, true) ==
        kNativeVisualGraceMilliseconds);
    assert(NativeVisualFallbackDelayMilliseconds(
        EArenaPresentationEventKind::Victory, true) ==
        kNativeVisualGraceMilliseconds);
    assert(NativeVisualFallbackDelayMilliseconds(
        EArenaPresentationEventKind::BusFare, true) ==
        kNativeVisualGraceMilliseconds);
    assert(NativeVisualFallbackDelayMilliseconds(
        EArenaPresentationEventKind::Elimination, false) == 0);

    // A cached compatibility HUD disables passive waiting, but an exact
    // callback from a distinct native widget still owns the same event. Keep
    // the raw capability separate from the derived passive-grace decision.
    const bool rawNativeCapability = true;
    const bool passiveGraceWithFallback = ShouldUseNativeVisualGrace(
        rawNativeCapability, false, true);
    assert(!passiveGraceWithFallback);
    assert(CanNativeVisualOwnBridgeEvent(
        rawNativeCapability, 1000, 0));
    assert(!CanNativeVisualOwnBridgeEvent(
        passiveGraceWithFallback, 1000, 0));

    assert(ShouldOpenScoreNeutralHudOpportunity(
        EArenaPresentationEventKind::BusFare, 0, false));
    assert(!ShouldOpenScoreNeutralHudOpportunity(
        EArenaPresentationEventKind::BusFare, 0, true));
    assert(!ShouldOpenScoreNeutralHudOpportunity(
        EArenaPresentationEventKind::BusFare, -50, false));
    assert(!ShouldOpenScoreNeutralHudOpportunity(
        EArenaPresentationEventKind::Elimination, 0, false));

    assert(CanNativeVisualOwnBridgeEvent(true, 1000, 0));
    assert(!CanNativeVisualOwnBridgeEvent(false, 1000, 0));
    assert(!CanNativeVisualOwnBridgeEvent(true, 0, 0));
    assert(!CanNativeVisualOwnBridgeEvent(true, 2999, 3000));
    assert(CanNativeVisualOwnBridgeEvent(true, 3000, 3000));
    assert(CanNativeVisualOwnBridgeEvent(true, 3001, 3000));

    // Preserve the native observation's clock. A bridge frame received at
    // 1,900ms for a native presentation at 1,000ms reserves through 3,000ms,
    // not 3,900ms from the later frame arrival.
    assert(ReserveVisualPresentationDeadline(0, 1000) == 3000);
    assert(ReserveVisualPresentationDeadline(3500, 1000) == 3500);
    assert(ReserveVisualPresentationDeadline(3500, 0) == 3500);
    assert(ReserveVisualPresentationDeadline(
        0, (std::numeric_limits<std::uint64_t>::max)() - 1) ==
        (std::numeric_limits<std::uint64_t>::max)());

    // Due time is deliberately absent from candidate selection. The oldest
    // active authoritative sequence remains the FIFO head until it is ready.
    std::uint64_t selectedSequence = 0;
    assert(ShouldPreferVisualSequence(
        true, false, 2, selectedSequence));
    selectedSequence = 2;
    assert(ShouldPreferVisualSequence(
        true, false, 1, selectedSequence));
    selectedSequence = 1;
    assert(!ShouldPreferVisualSequence(
        true, false, 2, selectedSequence));
    assert(!ShouldPreferVisualSequence(
        true, true, 0, selectedSequence));

    assert(ShouldAttemptTournamentStatRoute(false));
    assert(!ShouldAttemptTournamentStatRoute(true));
    assert(std::wcscmp(
        kBusFareTournamentStatName, L"MATCH_PLAYED_STAT") == 0);
    assert(std::wcscmp(
        kBusFareTournamentDisplayName, L"MatchEntryFee") == 0);
    assert(std::wcscmp(
        DirectBadgeIconTexturePath(
            EArenaPresentationEventKind::BusFare),
        L"/Game/UI/Events/Icons/"
        L"T-Icon-Bus-Fare-Flat.T-Icon-Bus-Fare-Flat") == 0);
    assert(std::wcscmp(
        DirectBadgeIconTexturePath(
            EArenaPresentationEventKind::Elimination),
        L"/Game/Athena/HUD/Art/killsico_alt.KillsIco_Alt") == 0);
    assert(std::wcscmp(
        DirectBadgeIconTexturePath(
            EArenaPresentationEventKind::Placement),
        L"/Game/UI/Foundation/Textures/Icons/Quest/"
        L"T-Icon-Trophy-32.T-Icon-Trophy-32") == 0);
    assert(DirectBadgeIconTexturePath(
        EArenaPresentationEventKind::Placement) ==
        DirectBadgeIconTexturePath(
            EArenaPresentationEventKind::Victory));
    assert(DirectBadgeIconTexturePath(
        static_cast<EArenaPresentationEventKind>(L'X')) == nullptr);
    assert(VisualDispatchDelayMilliseconds(true) ==
        kVisualPresentationSpacingMilliseconds);
    assert(VisualDispatchDelayMilliseconds(false) ==
        kVisualRetryDelayMilliseconds);

    assert(SelectHudFallbackAttemptThreshold(
        false, false, false, false) ==
        kAuthoritativeHudFallbackAttempt);
    assert(SelectHudFallbackAttemptThreshold(
        true, false, false, false) ==
        kNativeHudFallbackAttempt);
    assert(SelectHudFallbackAttemptThreshold(
        true, true, true, false) ==
        kNativeHudFallbackAttempt);
    assert(SelectHudFallbackAttemptThreshold(
        true, true, true, true) ==
        kAuthoritativeHudFallbackAttempt);
    assert(SelectHudFallbackAttemptThreshold(
        true, true, false, false) ==
        kAuthoritativeHudFallbackAttempt);

    wchar_t bootstrapText[128]{};
    const int bootstrapLength = std::swprintf(
        bootstrapText, 128,
        L"ATLAS_ARENA_BOOTSTRAP_V1:%016llX:A:%d:%010d:%+07d",
        0x12ABull, 1, 2040, -40);
    assert(bootstrapLength ==
        static_cast<int>(kArenaBootstrapMessageLength));

    FArenaBootstrap bootstrap{};
    assert(TryParseBootstrapMessageV1(
        bootstrapText,
        static_cast<std::size_t>(bootstrapLength),
        &bootstrap));
    assert(bootstrap.SessionOrdinal == 0x12ABull);
    assert(bootstrap.Active);
    assert(bootstrap.SavingEnabled);
    assert(bootstrap.StartingHype == 2040);
    assert(bootstrap.ArenaEntryFee == -40);

    wchar_t endText[64]{};
    const int endLength = std::swprintf(
        endText, 64,
        L"ATLAS_ARENA_END_V1:%016llX",
        0x12ABull);
    assert(endLength == static_cast<int>(kArenaEndMessageLength));
    FArenaEnd end{};
    assert(TryParseEndMessageV1(
        endText, static_cast<std::size_t>(endLength), &end));
    assert(end.Active);
    assert(end.SessionOrdinal == 0x12ABull);

    FArenaEnd malformedEnd{ 99, true };
    assert(!TryParseEndMessageV1(
        endText, static_cast<std::size_t>(endLength - 1),
        &malformedEnd));
    assert(!malformedEnd.Active);
    assert(malformedEnd.SessionOrdinal == 0);
    assert(!TryParseEndMessageV1(
        L"ATLAS_ARENA_END_V1:0000000000000000",
        kArenaEndMessageLength, &malformedEnd));
    assert(!TryParseEndMessageV1(
        L"ATLAS_ARENA_END_V1:00000000000012ab",
        kArenaEndMessageLength, &malformedEnd));
    assert(!TryParseEndMessageV1(
        L"ATLAS_ARENA_END_V1:00000000000012ABX",
        kArenaEndMessageLength + 1, &malformedEnd));

    FArenaSessionState state{};
    assert(ApplyBootstrap(&state, bootstrap) ==
        EBootstrapApplyResult::Initialized);
    assert(state.Active);
    assert(state.CurrentHype == 2040);
    assert(state.PreviousPlacement == 101);
    assert(CanApplyEnd(state, end));
    auto staleEnd = end;
    staleEnd.SessionOrdinal = 0x12ACull;
    assert(!CanApplyEnd(state, staleEnd));
    staleEnd = end;
    staleEnd.Active = false;
    assert(!CanApplyEnd(state, staleEnd));
    auto inactiveState = state;
    inactiveState.Active = false;
    assert(!CanApplyEnd(inactiveState, end));

    const auto parseEvent = [](
        std::uint64_t sequence,
        wchar_t kind,
        int delta,
        int resultingHype,
        int placement,
        bool nativeGrace)
    {
        wchar_t text[160]{};
        const int length = std::swprintf(
            text, 160,
            L"ATLAS_ARENA_EVENT_V1:%016llX:%016llX:%lc:%+07d:%010d:%03d:%d",
            0x12ABull,
            static_cast<unsigned long long>(sequence),
            kind, delta, resultingHype, placement,
            nativeGrace ? 1 : 0);
        assert(length == static_cast<int>(kArenaEventMessageLength));
        FArenaPresentationEvent event{};
        assert(TryParseEventMessageV1(
            text, static_cast<std::size_t>(length), &event));
        return event;
    };

    const auto busFare = parseEvent(1, L'B', -40, 2000, 0, true);
    std::int32_t previousPlacement = 0;
    auto wrongTotal = busFare;
    wrongTotal.ResultingHype = 1999;
    assert(!ApplyPresentationEvent(
        &state, wrongTotal, &previousPlacement));
    auto wrongFare = busFare;
    wrongFare.PointsDelta = -20;
    wrongFare.ResultingHype = 2020;
    assert(!ApplyPresentationEvent(
        &state, wrongFare, &previousPlacement));
    assert(ApplyPresentationEvent(
        &state, busFare, &previousPlacement));
    assert(previousPlacement == 101);
    assert(state.CurrentHype == 2000);
    auto duplicateFare = busFare;
    duplicateFare.EventSequence = 2;
    duplicateFare.ResultingHype = 1960;
    assert(!ApplyPresentationEvent(&state, duplicateFare));

    const auto elimination = parseEvent(2, L'E', 20, 2020, 0, true);
    assert(ApplyPresentationEvent(&state, elimination));
    assert(state.EliminationCount == 1);
    assert(state.CurrentHype == 2020);
    // Exact replays and older events cannot double an award or visual.
    assert(!ApplyPresentationEvent(&state, elimination));
    assert(!ApplyPresentationEvent(&state, busFare));
    assert(state.EliminationCount == 1);

    const auto placement = parseEvent(3, L'P', 60, 2080, 25, false);
    assert(ApplyPresentationEvent(
        &state, placement, &previousPlacement));
    assert(previousPlacement == 101);
    assert(state.PreviousPlacement == 25);

    auto repeatedPlacement = placement;
    repeatedPlacement.EventSequence = 4;
    repeatedPlacement.ResultingHype = 2140;
    assert(!ApplyPresentationEvent(&state, repeatedPlacement));

    const auto victory = parseEvent(4, L'V', 60, 2140, 1, true);
    assert(ApplyPresentationEvent(
        &state, victory, &previousPlacement));
    assert(previousPlacement == 25);
    assert(state.PreviousPlacement == 1);
    assert(state.CurrentHype == 2140);

    // A retried bootstrap for the same session updates flags without rolling
    // the accepted event sequence or Hype total backward.
    bootstrap.SavingEnabled = false;
    bootstrap.StartingHype = 0;
    assert(ApplyBootstrap(&state, bootstrap) ==
        EBootstrapApplyResult::Refreshed);
    assert(!state.SavingEnabled);
    assert(state.CurrentHype == 2140);
    assert(state.LastEventSequence == 4);

    // A new server-authored session resets all match-local counters. This is
    // the Save-Hype-off behavior: normal session scoring starts from zero but
    // nothing from the prior match is restored.
    bootstrap.SessionOrdinal = 0x12ACull;
    bootstrap.StartingHype = 0;
    bootstrap.ArenaEntryFee = 0;
    assert(ApplyBootstrap(&state, bootstrap) ==
        EBootstrapApplyResult::Initialized);
    assert(state.CurrentHype == 0);
    assert(state.EliminationCount == 0);
    assert(state.LastEventSequence == 0);

    // A score-neutral B frame marks the aircraft/HUD ownership boundary in
    // zero-fare sessions without changing Hype or showing a badge.
    FArenaPresentationEvent zeroFare{
        0x12ACull, 1, EArenaPresentationEventKind::BusFare,
        0, 0, 0, true };
    assert(ApplyPresentationEvent(&state, zeroFare));
    assert(state.CurrentHype == 0);
    assert(state.LastEventSequence == 1);

    FArenaPresentationEvent stale = elimination;
    assert(!ApplyPresentationEvent(&state, stale));

    FArenaSessionState flooredState{};
    FArenaBootstrap flooredBootstrap{
        0x9000ull, true, true, 10, -40 };
    assert(ApplyBootstrap(&flooredState, flooredBootstrap) ==
        EBootstrapApplyResult::Initialized);
    FArenaPresentationEvent flooredFare{
        0x9000ull, 1, EArenaPresentationEventKind::BusFare,
        -40, 0, 0, false };
    assert(ApplyPresentationEvent(&flooredState, flooredFare));
    assert(flooredState.CurrentHype == 0);

    FArenaSessionState cappedState{};
    FArenaBootstrap cappedBootstrap{
        0x9001ull, true, true,
        (std::numeric_limits<std::int32_t>::max)() - 10, 0 };
    assert(ApplyBootstrap(&cappedState, cappedBootstrap) ==
        EBootstrapApplyResult::Initialized);
    FArenaPresentationEvent cappedElimination{
        0x9001ull, 1, EArenaPresentationEventKind::Elimination,
        20, (std::numeric_limits<std::int32_t>::max)(), 0, false };
    assert(ApplyPresentationEvent(&cappedState, cappedElimination));

    // Exact-width parsing rejects truncation, trailing data, lowercase hex,
    // unknown event kinds, invalid placement shapes, and a positive fee.
    assert(!TryParseBootstrapMessageV1(
        bootstrapText,
        static_cast<std::size_t>(bootstrapLength - 1),
        &bootstrap));
    bootstrapText[bootstrapLength] = L'X';
    assert(!TryParseBootstrapMessageV1(
        bootstrapText,
        static_cast<std::size_t>(bootstrapLength + 1),
        &bootstrap));
    bootstrapText[bootstrapLength] = L'\0';
    bootstrapText[kArenaBootstrapPrefixLength + 14] = L'a';
    assert(!TryParseBootstrapMessageV1(
        bootstrapText,
        static_cast<std::size_t>(bootstrapLength),
        &bootstrap));

    wchar_t malformedEvent[160]{};
    int malformedLength = std::swprintf(
        malformedEvent, 160,
        L"ATLAS_ARENA_EVENT_V1:%016llX:%016llX:%lc:%+07d:%010d:%03d:%d",
        0x12ABull, 1ull, L'X', 20, 20, 0, 0);
    FArenaPresentationEvent malformed{};
    assert(!TryParseEventMessageV1(
        malformedEvent,
        static_cast<std::size_t>(malformedLength),
        &malformed));

    malformedLength = std::swprintf(
        malformedEvent, 160,
        L"ATLAS_ARENA_EVENT_V1:%016llX:%016llX:%lc:%+07d:%010d:%03d:%d",
        0x12ABull, 1ull, L'P', 60, 60, 0, 0);
    assert(!TryParseEventMessageV1(
        malformedEvent,
        static_cast<std::size_t>(malformedLength),
        &malformed));

    wchar_t positiveFee[128]{};
    const int positiveFeeLength = std::swprintf(
        positiveFee, 128,
        L"ATLAS_ARENA_BOOTSTRAP_V1:%016llX:A:%d:%010d:%+07d",
        0x12ABull, 1, 2040, 40);
    assert(!TryParseBootstrapMessageV1(
        positiveFee,
        static_cast<std::size_t>(positiveFeeLength),
        &bootstrap));

    return 0;
}
