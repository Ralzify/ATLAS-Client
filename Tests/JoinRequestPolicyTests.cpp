#include "../ATLAS/Public/JoinRequestPolicy.h"

#include <cassert>

int main()
{
    using namespace AtlasJoinRequestPolicy;

    // Canonical frontend worlds remain eligible even if the optional driver
    // field could not be resolved on that engine generation.
    assert(ResolveFrontendActive({
        true, true, true, ENetDriverState::Unknown }));

    // A valid lobby can temporarily use a non-canonical world name. The
    // absence of a gameplay net driver is the cross-version capability that
    // keeps the quick join key working there.
    assert(ResolveFrontendActive({
        true, true, false, ENetDriverState::Absent }));

    // A live driver is a hard gameplay/travel boundary, including the short
    // interval where the outgoing world may still be named Frontend.
    assert(!ResolveFrontendActive({
        true, true, false, ENetDriverState::Present }));
    assert(!ResolveFrontendActive({
        true, true, true, ENetDriverState::Present }));

    // Unknown capabilities fail closed unless the canonical world name is
    // positively known. Missing world/controller state is never frontend.
    assert(!ResolveFrontendActive({
        true, true, false, ENetDriverState::Unknown }));
    assert(!ResolveFrontendActive({
        false, true, true, ENetDriverState::Absent }));
    assert(!ResolveFrontendActive({
        true, false, true, ENetDriverState::Absent }));

    assert(ShouldDispatchJoin(
        EJoinRequestSource::QuickHotkey, true));
    assert(!ShouldDispatchJoin(
        EJoinRequestSource::QuickHotkey, false));

    assert(ShouldDispatchJoin(
        EJoinRequestSource::OverlayButton, true));
    assert(ShouldDispatchJoin(
        EJoinRequestSource::OverlayButton, false));

    assert(!ShouldDispatchJoin(
        static_cast<EJoinRequestSource>(0xFF), true));
    return 0;
}
