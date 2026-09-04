#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace AtlasPlaylistHudPatchPolicy
{
    enum class ECompetitiveHud : unsigned char
    {
        Arena,
        Tournament
    };

    struct FPlaylistPath
    {
        const wchar_t* ObjectPath;
        ECompetitiveHud Hud;
    };

    struct FExtensionNormalizationPlan
    {
        std::size_t DuplicateCount;
        std::size_t FinalCount;
        bool AppendExpected;
    };

    struct FUiExtensionLayout
    {
        std::size_t ElementSize;
        std::size_t SlotOffset;
        std::size_t WidgetClassOffset;
    };

    struct FTopLevelSoftClassPathLayout
    {
        std::size_t PackageNameOffset;
        std::size_t AssetNameOffset;
        std::size_t SubPathOffset;
    };

    enum class ESoftClassPathIdentityKind : unsigned char
    {
        Invalid,
        LegacyFName,
        TopLevelAssetPath
    };

    // A representation-independent identity for the reflected object path
    // stored inside a soft-class pointer. UE4 stores the full object path as
    // one FName (comparison index + number), while UE5's top-level asset path
    // stores separate package and asset name ids.
    struct FSoftClassPathIdentity
    {
        ESoftClassPathIdentityKind Kind =
            ESoftClassPathIdentityKind::Invalid;
        std::int32_t PrimaryNameId = 0;
        std::int32_t SecondaryNameId = 0;
    };

    // Playlist UI extensions became the competitive HUD source in FN 10.00.
    // Earlier Arena builds keep their authored, legacy HUD path and must not be
    // rewritten through the newer FUIExtension layout.
    inline constexpr int MinimumUiExtensionVersionHundredths = 1000;
    inline constexpr int MaximumUiExtensionVersionHundredthsExclusive = 3100;

    // FN 10.x used PlaylistUIExtension; later reflection exports shortened
    // the same wire-compatible struct name to UIExtension.
    inline constexpr std::array<const char*, 2>
        PlaylistUiExtensionTypeNames = {{
            "PlaylistUIExtension",
            "UIExtension"
        }};

    // These are the canonical playlists exposed by Magnesium. Static lookup is
    // intentionally used only as a loaded-object probe; the client never loads
    // all of these packages just to patch them.
    inline constexpr std::array<FPlaylistPath, 12> CompetitivePlaylists = {{
        {
            L"/Game/Athena/Playlists/Showdown/Playlist_ShowdownAlt_Solo.Playlist_ShowdownAlt_Solo",
            ECompetitiveHud::Arena
        },
        {
            L"/Game/Athena/Playlists/Showdown/Playlist_ShowdownAlt_Duos.Playlist_ShowdownAlt_Duos",
            ECompetitiveHud::Arena
        },
        {
            L"/Game/Athena/Playlists/Showdown/Playlist_ShowdownAlt_Trios.Playlist_ShowdownAlt_Trios",
            ECompetitiveHud::Arena
        },
        {
            L"/Game/Athena/Playlists/Showdown/Playlist_ShowdownAlt_Squads.Playlist_ShowdownAlt_Squads",
            ECompetitiveHud::Arena
        },
        {
            L"/Game/Athena/Playlists/NoBuildBR/Competitive/Playlist_ShowdownAlt_NoBuildBR_Solo.Playlist_ShowdownAlt_NoBuildBR_Solo",
            ECompetitiveHud::Arena
        },
        {
            L"/Game/Athena/Playlists/NoBuildBR/Competitive/Playlist_ShowdownAlt_NoBuildBR_Duos.Playlist_ShowdownAlt_NoBuildBR_Duos",
            ECompetitiveHud::Arena
        },
        {
            L"/Game/Athena/Playlists/NoBuildBR/Competitive/Playlist_ShowdownAlt_NoBuildBR_Trios.Playlist_ShowdownAlt_NoBuildBR_Trios",
            ECompetitiveHud::Arena
        },
        {
            L"/Game/Athena/Playlists/NoBuildBR/Competitive/Playlist_ShowdownAlt_NoBuildBR_Squads.Playlist_ShowdownAlt_NoBuildBR_Squads",
            ECompetitiveHud::Arena
        },
        {
            L"/Game/Athena/Playlists/Showdown/Playlist_Showdown_Solo.Playlist_Showdown_Solo",
            ECompetitiveHud::Tournament
        },
        {
            L"/Game/Athena/Playlists/Showdown/Playlist_Showdown_Duos.Playlist_Showdown_Duos",
            ECompetitiveHud::Tournament
        },
        {
            L"/Game/Athena/Playlists/Showdown/Playlist_Showdown_Trios.Playlist_Showdown_Trios",
            ECompetitiveHud::Tournament
        },
        {
            L"/Game/Athena/Playlists/Showdown/Playlist_Showdown_Squads.Playlist_Showdown_Squads",
            ECompetitiveHud::Tournament
        }
    }};

    inline int ToVersionHundredths(double Version)
    {
        return static_cast<int>(Version * 100.0 + 0.5);
    }

    inline bool SupportsUiExtensionPatch(double Version)
    {
        const int VersionHundredths = ToVersionHundredths(Version);
        return VersionHundredths >=
                MinimumUiExtensionVersionHundredths &&
            VersionHundredths <
                MaximumUiExtensionVersionHundredthsExclusive;
    }

    inline bool UsesLegacyPlaylistOwnedArenaHud(double Version)
    {
        const int VersionHundredths = ToVersionHundredths(Version);
        // In 10.x ArenaScoringHUD is authored by the active playlist's
        // UIExtensions array. Starting in 11.00 the Athena HUD owner exposes a
        // separate tournament extension array that can be refreshed directly.
        return VersionHundredths >= 1000 && VersionHundredths < 1100;
    }

    inline bool IsArenaPlaylistObjectName(std::string_view Name)
    {
        return Name.find("ShowdownAlt") != std::string_view::npos;
    }

    inline bool TryResolveCompetitivePlaylistObjectName(
        std::string_view Name,
        ECompetitiveHud* OutHud) noexcept
    {
        if (!OutHud)
            return false;

        static constexpr std::array<std::string_view, 8>
            ArenaObjectNames = {{
                "Playlist_ShowdownAlt_Solo",
                "Playlist_ShowdownAlt_Duos",
                "Playlist_ShowdownAlt_Trios",
                "Playlist_ShowdownAlt_Squads",
                "Playlist_ShowdownAlt_NoBuildBR_Solo",
                "Playlist_ShowdownAlt_NoBuildBR_Duos",
                "Playlist_ShowdownAlt_NoBuildBR_Trios",
                "Playlist_ShowdownAlt_NoBuildBR_Squads"
            }};
        static constexpr std::array<std::string_view, 4>
            TournamentObjectNames = {{
                "Playlist_Showdown_Solo",
                "Playlist_Showdown_Duos",
                "Playlist_Showdown_Trios",
                "Playlist_Showdown_Squads"
            }};

        for (const auto Candidate : ArenaObjectNames)
        {
            if (Name == Candidate)
            {
                *OutHud = ECompetitiveHud::Arena;
                return true;
            }
        }
        for (const auto Candidate : TournamentObjectNames)
        {
            if (Name == Candidate)
            {
                *OutHud = ECompetitiveHud::Tournament;
                return true;
            }
        }
        return false;
    }

    // Preserve every non-target extension. If the expected competitive HUD
    // is absent, append it; if it appears more than once, retain the first
    // authored entry and remove only the later duplicates.
    inline FExtensionNormalizationPlan PlanExtensionNormalization(
        std::size_t TotalCount,
        std::size_t ExpectedCount,
        std::size_t UnknownCount = 0)
    {
        if (ExpectedCount > TotalCount)
            ExpectedCount = TotalCount;
        if (UnknownCount > TotalCount - ExpectedCount)
            UnknownCount = TotalCount - ExpectedCount;

        const std::size_t DuplicateCount =
            ExpectedCount > 0 ? ExpectedCount - 1 : 0;
        // An unresolved authored soft class is not proof that the target is
        // absent. Defer the append until every existing entry has been
        // positively resolved; this fails closed instead of creating two
        // native scoring widgets on legacy clients.
        const bool AppendExpected =
            ExpectedCount == 0 && UnknownCount == 0;
        return {
            DuplicateCount,
            TotalCount - DuplicateCount +
                (AppendExpected ? 1u : 0u),
            AppendExpected
        };
    }

    // The soft-class representation changed between Unreal generations. The
    // reflected outer struct is the stable contract: Slot must fit, and the
    // WidgetClass field must have enough room for the weak object identity at
    // the beginning of every supported TSoftClassPtr representation.
    inline bool IsSafeUiExtensionLayout(
        const FUiExtensionLayout& Layout,
        std::size_t StorageSize,
        std::size_t WeakObjectPointerSize)
    {
        if (Layout.ElementSize == 0 ||
            Layout.ElementSize > StorageSize ||
            WeakObjectPointerSize == 0 ||
            Layout.SlotOffset >= Layout.ElementSize ||
            Layout.WidgetClassOffset > Layout.ElementSize ||
            WeakObjectPointerSize >
                Layout.ElementSize - Layout.WidgetClassOffset)
        {
            return false;
        }

        const std::size_t SlotEnd = Layout.SlotOffset + 1;
        const std::size_t WidgetEnd =
            Layout.WidgetClassOffset + WeakObjectPointerSize;
        return SlotEnd <= Layout.WidgetClassOffset ||
            WidgetEnd <= Layout.SlotOffset;
    }

    inline constexpr FTopLevelSoftClassPathLayout
        ResolveTopLevelSoftClassPathLayout(
            bool EngineAtLeast53) noexcept
    {
        return EngineAtLeast53
            ? FTopLevelSoftClassPathLayout{0x08, 0x0c, 0x10}
            : FTopLevelSoftClassPathLayout{0x10, 0x14, 0x18};
    }

    inline constexpr FSoftClassPathIdentity
        MakeLegacySoftClassPathIdentity(
            std::int32_t PathNameIndex,
            std::int32_t PathNameNumber) noexcept
    {
        return {
            ESoftClassPathIdentityKind::LegacyFName,
            PathNameIndex,
            PathNameNumber
        };
    }

    inline constexpr FSoftClassPathIdentity
        MakeTopLevelSoftClassPathIdentity(
            std::int32_t PackageNameId,
            std::int32_t AssetNameId) noexcept
    {
        return {
            ESoftClassPathIdentityKind::TopLevelAssetPath,
            PackageNameId,
            AssetNameId
        };
    }

    inline constexpr bool IsValidSoftClassPathIdentity(
        const FSoftClassPathIdentity& Identity) noexcept
    {
        if (Identity.PrimaryNameId <= 0)
            return false;

        switch (Identity.Kind)
        {
        case ESoftClassPathIdentityKind::LegacyFName:
            // FName number zero is the ordinary, unsuffixed path.
            return Identity.SecondaryNameId >= 0;
        case ESoftClassPathIdentityKind::TopLevelAssetPath:
            return Identity.SecondaryNameId > 0;
        default:
            return false;
        }
    }

    inline constexpr bool IsSameSoftClassPathIdentity(
        const FSoftClassPathIdentity& Left,
        const FSoftClassPathIdentity& Right) noexcept
    {
        return IsValidSoftClassPathIdentity(Left) &&
            IsValidSoftClassPathIdentity(Right) &&
            Left.Kind == Right.Kind &&
            Left.PrimaryNameId == Right.PrimaryNameId &&
            Left.SecondaryNameId == Right.SecondaryNameId;
    }
}
