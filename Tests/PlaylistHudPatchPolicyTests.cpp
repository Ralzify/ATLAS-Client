#include "../ATLAS/Public/PlaylistHudPatchPolicy.h"

#include <cassert>

int main()
{
    using namespace AtlasPlaylistHudPatchPolicy;

    static_assert(PlaylistUiExtensionTypeNames.size() == 2);
    assert(std::string_view(PlaylistUiExtensionTypeNames[0]) ==
        "PlaylistUIExtension");
    assert(std::string_view(PlaylistUiExtensionTypeNames[1]) ==
        "UIExtension");

    assert(!SupportsUiExtensionPatch(9.40));
    assert(SupportsUiExtensionPatch(10.00));
    assert(SupportsUiExtensionPatch(17.30));
    assert(SupportsUiExtensionPatch(24.20));
    assert(SupportsUiExtensionPatch(30.40));
    assert(!SupportsUiExtensionPatch(31.00));
    assert(!UsesLegacyPlaylistOwnedArenaHud(9.99));
    assert(UsesLegacyPlaylistOwnedArenaHud(10.00));
    assert(UsesLegacyPlaylistOwnedArenaHud(10.40));
    assert(!UsesLegacyPlaylistOwnedArenaHud(11.00));

    assert(CompetitivePlaylists.size() == 12);
    for (std::size_t Index = 0; Index < CompetitivePlaylists.size(); ++Index)
    {
        const auto& Playlist = CompetitivePlaylists[Index];
        assert(Playlist.ObjectPath != nullptr);
        assert(Playlist.ObjectPath[0] == L'/');
        if (Index < 8)
            assert(Playlist.Hud == ECompetitiveHud::Arena);
        else
            assert(Playlist.Hud == ECompetitiveHud::Tournament);
    }

    assert(IsArenaPlaylistObjectName("Playlist_ShowdownAlt_Solo"));
    assert(IsArenaPlaylistObjectName(
        "Playlist_ShowdownAlt_NoBuildBR_Squads"));
    assert(!IsArenaPlaylistObjectName("Playlist_Showdown_Solo"));
    assert(!IsArenaPlaylistObjectName("Playlist_DefaultSolo"));

    {
        ECompetitiveHud Hud = ECompetitiveHud::Tournament;
        assert(TryResolveCompetitivePlaylistObjectName(
            "Playlist_ShowdownAlt_Solo", &Hud));
        assert(Hud == ECompetitiveHud::Arena);
        assert(TryResolveCompetitivePlaylistObjectName(
            "Playlist_Showdown_Squads", &Hud));
        assert(Hud == ECompetitiveHud::Tournament);
        assert(!TryResolveCompetitivePlaylistObjectName(
            "Playlist_ShowdownAlt_Solo_C", &Hud));
        assert(!TryResolveCompetitivePlaylistObjectName(
            "Playlist_ShowdownAlt_Solo_Experimental", &Hud));
        assert(!TryResolveCompetitivePlaylistObjectName(
            "Playlist_DefaultSolo", &Hud));
        assert(!TryResolveCompetitivePlaylistObjectName(
            "Playlist_ShowdownAlt_Solo", nullptr));
    }

    assert(IsSafeUiExtensionLayout(
        {0x30, 0x0, 0x8}, 0x30, 0x8));
    assert(IsSafeUiExtensionLayout(
        {0x28, 0x0, 0x8}, 0x30, 0x8));
    assert(IsSafeUiExtensionLayout(
        {0x20, 0x4, 0x8}, 0x30, 0x8));
    assert(!IsSafeUiExtensionLayout(
        {0x38, 0x0, 0x8}, 0x30, 0x8));
    assert(!IsSafeUiExtensionLayout(
        {0x20, 0x8, 0x8}, 0x30, 0x8));
    assert(!IsSafeUiExtensionLayout(
        {0x20, 0x0, 0x1c}, 0x30, 0x8));

    {
        // Fortnite 23.00 through 24.20 use UE5's top-level asset path while
        // retaining the pre-5.3 field placement.
        const auto Layout =
            ResolveTopLevelSoftClassPathLayout(false);
        assert(Layout.PackageNameOffset == 0x10);
        assert(Layout.AssetNameOffset == 0x14);
        assert(Layout.SubPathOffset == 0x18);
    }
    {
        const auto Layout =
            ResolveTopLevelSoftClassPathLayout(true);
        assert(Layout.PackageNameOffset == 0x08);
        assert(Layout.AssetNameOffset == 0x0c);
        assert(Layout.SubPathOffset == 0x10);
    }

    {
        const auto Authored =
            MakeLegacySoftClassPathIdentity(4312, 0);
        const auto Resolved =
            MakeLegacySoftClassPathIdentity(4312, 0);
        assert(IsValidSoftClassPathIdentity(Authored));
        assert(IsSameSoftClassPathIdentity(Authored, Resolved));
        assert(!IsSameSoftClassPathIdentity(
            Authored,
            MakeLegacySoftClassPathIdentity(4313, 0)));
        assert(!IsSameSoftClassPathIdentity(
            Authored,
            MakeLegacySoftClassPathIdentity(4312, 1)));
    }
    {
        const auto Authored =
            MakeTopLevelSoftClassPathIdentity(812, 913);
        const auto Resolved =
            MakeTopLevelSoftClassPathIdentity(812, 913);
        assert(IsValidSoftClassPathIdentity(Authored));
        assert(IsSameSoftClassPathIdentity(Authored, Resolved));
        assert(!IsSameSoftClassPathIdentity(
            Authored,
            MakeTopLevelSoftClassPathIdentity(814, 913)));
        assert(!IsSameSoftClassPathIdentity(
            Authored,
            MakeTopLevelSoftClassPathIdentity(812, 914)));
    }
    {
        const auto Legacy =
            MakeLegacySoftClassPathIdentity(812, 913);
        const auto Modern =
            MakeTopLevelSoftClassPathIdentity(812, 913);
        assert(!IsSameSoftClassPathIdentity(Legacy, Modern));

        const FSoftClassPathIdentity Invalid{};
        assert(!IsValidSoftClassPathIdentity(Invalid));
        assert(!IsSameSoftClassPathIdentity(Invalid, Invalid));
        assert(!IsValidSoftClassPathIdentity(
            MakeLegacySoftClassPathIdentity(0, 0)));
        assert(!IsValidSoftClassPathIdentity(
            MakeLegacySoftClassPathIdentity(42, -1)));
        assert(!IsValidSoftClassPathIdentity(
            MakeTopLevelSoftClassPathIdentity(42, 0)));
        assert(!IsValidSoftClassPathIdentity(
            MakeTopLevelSoftClassPathIdentity(0, 42)));
    }

    {
        const auto Plan = PlanExtensionNormalization(4, 0);
        assert(Plan.AppendExpected);
        assert(Plan.DuplicateCount == 0);
        assert(Plan.FinalCount == 5);
    }
    {
        // A 10.40 authored soft class that cannot yet be resolved must not be
        // interpreted as proof that ArenaScoringHUD is absent.
        const auto Plan = PlanExtensionNormalization(3, 0, 1);
        assert(!Plan.AppendExpected);
        assert(Plan.DuplicateCount == 0);
        assert(Plan.FinalCount == 3);
    }
    {
        const auto Plan = PlanExtensionNormalization(4, 1);
        assert(!Plan.AppendExpected);
        assert(Plan.DuplicateCount == 0);
        assert(Plan.FinalCount == 4);
    }
    {
        const auto Plan = PlanExtensionNormalization(4, 3);
        assert(!Plan.AppendExpected);
        assert(Plan.DuplicateCount == 2);
        // One expected extension and the one non-target extension remain.
        assert(Plan.FinalCount == 2);
    }
    {
        // Invalid reflected counts are clamped by the pure planner. Runtime
        // array validation still rejects them before memory is touched.
        const auto Plan = PlanExtensionNormalization(2, 9);
        assert(!Plan.AppendExpected);
        assert(Plan.DuplicateCount == 1);
        assert(Plan.FinalCount == 1);
    }
    return 0;
}
