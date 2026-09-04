#include "../ATLAS/Public/TournamentRefreshPolicy.h"

#include <cassert>

int main()
{
    using namespace AtlasTournamentRefreshPolicy;

    assert(IsFrontendWorldName("Frontend"));
    assert(IsFrontendWorldName("FrontEnd"));
    assert(IsFrontendWorldName("FRONTEND"));
    assert(IsFrontendWorldName("Frontend_2"));
    assert(IsFrontendWorldName("FrontEnd_2"));
    assert(IsFrontendWorldName("Frontend_2147482250"));

    assert(!IsFrontendWorldName(""));
    assert(!IsFrontendWorldName("Frontend_"));
    assert(!IsFrontendWorldName("Frontend_BG_Main"));
    assert(!IsFrontendWorldName("FrontEnd_BG_Main"));
    assert(!IsFrontendWorldName("Frontend_Lobby_Background"));
    assert(!IsFrontendWorldName("Apollo_Terrain"));

    // Keep the first refresh beyond the observed 17.30 tournament-selection
    // window. Regressing to the old 5.5/3.5 second schedule recreates the
    // startup race where both attempts complete before Arena is selected.
    assert(InitialDelayMs >= 15000);
    assert(RetryDelayMs >= 10000);
    assert(MaxAttempts == 3);

    // A Discover selection made after the startup attempts have expired must
    // refresh after the stock tournament download settles, not race it.
    assert(SelectionDelayMs >= 3000);
    assert(SelectionDelayMs <= 5000);
    assert(SelectionRetryDelayMs >= 1000);
    assert(SelectionMaxAttempts >= 3);
    assert(ShouldRearmForPlaylistChange(
        "Playlist_DefaultSolo", "Playlist_ShowdownAlt_Solo"));
    assert(ShouldRearmForPlaylistChange(
        "Playlist_ShowdownAlt_Solo", "Playlist_DefaultSolo"));
    assert(!ShouldRearmForPlaylistChange(
        "Playlist_ShowdownAlt_Solo", "Playlist_ShowdownAlt_Solo"));
    assert(!ShouldRearmForPlaylistChange(
        "Playlist_DefaultSolo", ""));
    assert(!ShouldRearmForPlaylistChange(
        "Playlist_DefaultSolo", "None"));

    // Newer Discover builds retain the BR parent playlist and put the chosen
    // child activity in FPlaylistData.LinkId.Mnemonic. That child identity is
    // therefore what must drive the refresh edge.
    assert(ResolveSelectionIdentity(
        true, "playlist_showdownalt_solo", "Playlist_DefaultSolo") ==
        "playlist_showdownalt_solo");
    assert(ShouldRearmForSelectionChange(
        "playlist_defaultsolo", "playlist_showdownalt_solo"));
    assert(!ShouldRearmForSelectionChange(
        "playlist_showdownalt_solo", "playlist_showdownalt_solo"));
    assert(ShouldRearmForSelectionRevision(7, 8));
    assert(!ShouldRearmForSelectionRevision(8, 8));
    assert(!ShouldRearmForSelectionRevision(8, 0));
    assert(ShouldDispatchSelectionRebind(true, 8, 7));
    assert(!ShouldDispatchSelectionRebind(true, 8, 8));
    assert(!ShouldDispatchSelectionRebind(false, 8, 7));
    assert(!ShouldDispatchSelectionRebind(true, 0, 0));

    // While Discover is committing a modern selection, an empty LinkId must
    // not fall back to the unchanged BR parent and create a false re-arm loop.
    assert(ResolveSelectionIdentity(
        true, "", "Playlist_DefaultSolo").empty());
    assert(ResolveSelectionIdentity(
        true, "None", "Playlist_DefaultSolo").empty());

    // Old seasons do not expose FPlaylistData.LinkId, so PlaylistName remains
    // the correct compatibility signal there.
    assert(ResolveSelectionIdentity(
        false, "", "Playlist_ShowdownAlt_Solo") ==
        "Playlist_ShowdownAlt_Solo");
    assert(ResolveSelectionIdentity(
        false, "", "None").empty());
    return 0;
}
