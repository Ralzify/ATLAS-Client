#include "../ATLAS/Public/ArenaHudOwnershipPolicy.h"

#include <cassert>

int main()
{
    using namespace AtlasArenaHudOwnershipPolicy;

    const int object = 0;
    const FObjectIdentity first{ &object, 42, 1001 };
    const FObjectIdentity same{ &object, 42, 1001 };
    const FObjectIdentity recycledAddress{ &object, 77, 1002 };
    const FObjectIdentity recycledSlot{ &object, 42, 1002 };

    assert(IsSameObjectInstance(first, same));
    assert(!IsSameObjectInstance(first, recycledAddress));
    assert(!IsSameObjectInstance(first, recycledSlot));
    assert(!IsSameObjectInstance(first, {}));

    assert(CanAdoptVisibleUnmatchedAlert(true, true));
    assert(!CanAdoptVisibleUnmatchedAlert(true, false));
    assert(!CanAdoptVisibleUnmatchedAlert(false, true));

    assert(CanAdoptObservedNativeAlert(
        true, true, true, true, false, false, true));
    assert(CanAdoptObservedNativeAlert(
        true, true, false, false, true, false, true));
    assert(CanAdoptObservedNativeAlert(
        true, true, true, true, false, true, false));
    assert(!CanAdoptObservedNativeAlert(
        false, true, true, true, false, false, true));
    assert(!CanAdoptObservedNativeAlert(
        true, false, true, true, false, false, true));
    assert(!CanAdoptObservedNativeAlert(
        true, true, true, false, true, false, true));
    assert(!CanAdoptObservedNativeAlert(
        true, true, false, false, false, false, true));
    assert(!CanAdoptObservedNativeAlert(
        true, true, true, true, false, false, false));

    assert(CanAdoptLegacyOwnerMatchedNativeAlert(
        true, true, true, true, true));
    assert(!CanAdoptLegacyOwnerMatchedNativeAlert(
        false, true, true, true, true));
    assert(!CanAdoptLegacyOwnerMatchedNativeAlert(
        true, false, true, true, true));
    assert(!CanAdoptLegacyOwnerMatchedNativeAlert(
        true, true, false, true, true));
    assert(!CanAdoptLegacyOwnerMatchedNativeAlert(
        true, true, true, false, true));
    assert(!CanAdoptLegacyOwnerMatchedNativeAlert(
        true, true, true, true, false));

    const FVisibleAlertPreference oldNative{
        EVisibleAlertSource::Native, false, 100 };
    const FVisibleAlertPreference newNative{
        EVisibleAlertSource::Native, false, 200 };
    const FVisibleAlertPreference cachedNative{
        EVisibleAlertSource::Native, true, 300 };
    const FVisibleAlertPreference fallback{
        EVisibleAlertSource::OwnedFallback, true, 50 };
    assert(PreferVisibleAlert(oldNative, newNative));
    assert(!PreferVisibleAlert(newNative, oldNative));
    assert(PreferVisibleAlert(oldNative, cachedNative));
    assert(!PreferVisibleAlert(cachedNative, oldNative));
    assert(PreferVisibleAlert(cachedNative, oldNative, true));
    assert(!PreferVisibleAlert(oldNative, cachedNative, true));
    assert(PreferVisibleAlert(oldNative, fallback));
    assert(!PreferVisibleAlert(fallback, oldNative));

    assert(ShouldScanVisibleAlerts(
        true, EVisibleAlertSource::OwnedFallback, true, 100, 1000));
    assert(ShouldScanVisibleAlerts(
        false, EVisibleAlertSource::Native, false, 100, 1000));
    assert(!ShouldScanVisibleAlerts(
        true, EVisibleAlertSource::OwnedFallback, false, 999, 1000));
    assert(ShouldScanVisibleAlerts(
        true, EVisibleAlertSource::OwnedFallback, false, 1000, 1000));
    assert(!ShouldScanVisibleAlerts(
        true, EVisibleAlertSource::Native, false, 5000, 1000));

    assert(CanCreateOwnedFallbackForSession(false, 0, 0));
    assert(CanCreateOwnedFallbackForSession(true, 42, 0));
    assert(!CanCreateOwnedFallbackForSession(true, 42, 42));
    assert(CanCreateOwnedFallbackForSession(true, 43, 42));
    assert(!CanCreateOwnedFallbackForSession(true, 42, 0, 42));
    assert(CanCreateOwnedFallbackForSession(true, 43, 0, 42));

    // One native ownership lifetime spans the temporary 10.x HUD gaps from
    // spawn island to aircraft and aircraft to gameplay. A new match ordinal
    // remains eligible for compatibility fallback if it has no native owner.
    const std::uint64_t nativeOwnedSession = 42;
    assert(!CanCreateOwnedFallbackForSession(
        true, 42, 0, nativeOwnedSession));
    assert(!CanCreateOwnedFallbackForSession(
        true, 42, 0, nativeOwnedSession));
    assert(CanCreateOwnedFallbackForSession(
        true, 43, 0, nativeOwnedSession));

    assert(ShouldReserveLegacyNativeHudOwnership(true, true, true));
    assert(!ShouldReserveLegacyNativeHudOwnership(false, true, true));
    assert(!ShouldReserveLegacyNativeHudOwnership(true, false, true));
    assert(!ShouldReserveLegacyNativeHudOwnership(true, true, false));

    assert(ShouldCreateOwnedFallbackNow(true, true, true));
    assert(!ShouldCreateOwnedFallbackNow(true, false, true));
    assert(!ShouldCreateOwnedFallbackNow(true, true, false));
    assert(!ShouldCreateOwnedFallbackNow(false, true, true));
    assert(!ShouldCreateOwnedFallbackNow(false, false, false));

    assert(!CanRetireOwnedFallbackForNative(
        false, true, 1000, 1000));
    assert(!CanRetireOwnedFallbackForNative(
        true, false, 1000, 1000));
    assert(!CanRetireOwnedFallbackForNative(
        true, true, 1000, 999));
    assert(CanRetireOwnedFallbackForNative(
        true, true, 1000, 1000));

    assert(ShouldSuppressOwnedFallbackConsumer(true, false));
    assert(!ShouldSuppressOwnedFallbackConsumer(true, true));
    assert(!ShouldSuppressOwnedFallbackConsumer(false, false));
    assert(!ShouldSuppressOwnedFallbackConsumer(false, true));

    assert(CanDispatchStateChangingTournamentStatFallback(true, false));
    assert(!CanDispatchStateChangingTournamentStatFallback(true, true));
    assert(!CanDispatchStateChangingTournamentStatFallback(false, false));
    assert(!CanDispatchStateChangingTournamentStatFallback(false, true));

    assert(!CanFinalizeOwnedFallbackRetirement(
        42, 42, true, 3000, 4000));
    assert(!CanFinalizeOwnedFallbackRetirement(
        42, 42, false, 3000, 2999));
    assert(CanFinalizeOwnedFallbackRetirement(
        42, 42, false, 3000, 3000));
    assert(!CanFinalizeOwnedFallbackRetirement(
        42, 41, false, 3000, 4000));
    assert(!CanFinalizeOwnedFallbackRetirement(
        0, 0, false, 0, 0));

    return 0;
}
