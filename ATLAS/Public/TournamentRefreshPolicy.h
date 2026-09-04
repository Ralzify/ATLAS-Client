#pragma once

#include <cstdint>
#include <string_view>

namespace AtlasTournamentRefreshPolicy
{
    // The stock frontend first downloads the tournament profile and only
    // resolves its Arena event/window several seconds later. A 17.30 runtime
    // trace resolved the selection about 14.6 seconds after client injection;
    // earlier refreshes raced that selection and left the lobby at zero.
    // Refresh after the authored state has settled, then retry at a low
    // cadence in case profile/CMS delivery was slower on that launch. Three
    // bounded asynchronous refreshes cover selection settling as late as
    // 35 seconds without turning this into a per-frame or unbounded poll.
    inline constexpr std::uint64_t InitialDelayMs = 15000;
    inline constexpr std::uint64_t RetryDelayMs = 10000;
    inline constexpr unsigned MaxAttempts = 3;

    // The startup burst cannot cover a playlist chosen later from Discover.
    // Re-arm a shorter bounded burst when a Discover selection commits. The
    // stock 27.11 flow took about 2.8 seconds from selection to a successful
    // tournament-profile load, so firing at 750 ms only raced that work. Start
    // after the observed load window, then retain bounded retries for slower
    // machines/releases. Tournament refreshes can update event/window fields
    // without recursively scheduling more work.
    inline constexpr std::uint64_t SelectionDelayMs = 3500;
    inline constexpr std::uint64_t SelectionRetryDelayMs = 3000;
    inline constexpr unsigned SelectionMaxAttempts = 5;

    inline bool IsMeaningfulPlaylistName(std::string_view Name)
    {
        return !Name.empty() && Name != "None";
    }

    // Modern Discover selections can leave PlaylistName pointing at the
    // parent Battle Royale playlist while LastSelectedPlaylist.LinkId stores
    // the selected child activity (for example Arena). Prefer that link when
    // the reflected field exists. An empty modern LinkId is deliberately not
    // replaced by PlaylistName: doing so would oscillate between the parent
    // and child identities while Discover is still committing its selection.
    inline std::string_view ResolveSelectionIdentity(
        bool HasLinkMnemonicField,
        std::string_view LinkMnemonic,
        std::string_view PlaylistName)
    {
        if (HasLinkMnemonicField)
        {
            return IsMeaningfulPlaylistName(LinkMnemonic)
                ? LinkMnemonic
                : std::string_view{};
        }

        return IsMeaningfulPlaylistName(PlaylistName)
            ? PlaylistName
            : std::string_view{};
    }

    inline bool ShouldRearmForSelectionChange(
        std::string_view Previous,
        std::string_view Current)
    {
        return IsMeaningfulPlaylistName(Current) && Previous != Current;
    }

    inline bool ShouldRearmForSelectionRevision(
        std::uint64_t Previous,
        std::uint64_t Current)
    {
        return Current != 0 && Previous != Current;
    }

    inline bool ShouldDispatchSelectionRebind(
        bool SelectionTriggeredBurst,
        std::uint64_t SelectionBurstGeneration,
        std::uint64_t ReboundSelectionBurstGeneration)
    {
        return SelectionTriggeredBurst &&
            SelectionBurstGeneration != 0 &&
            SelectionBurstGeneration != ReboundSelectionBurstGeneration;
    }

    inline bool ShouldRearmForPlaylistChange(
        std::string_view Previous,
        std::string_view Current)
    {
        return ShouldRearmForSelectionChange(Previous, Current);
    }

    inline bool IsFrontendWorldName(std::string_view Name)
    {
        constexpr std::string_view BaseName = "Frontend";
        const auto HasCaseInsensitiveBase = [Name, BaseName]() noexcept
        {
            if (Name.size() < BaseName.size())
                return false;

            for (std::size_t Index = 0; Index < BaseName.size(); ++Index)
            {
                char Character = Name[Index];
                if (Character >= 'A' && Character <= 'Z')
                    Character = static_cast<char>(Character - 'A' + 'a');

                char Expected = BaseName[Index];
                if (Expected >= 'A' && Expected <= 'Z')
                    Expected = static_cast<char>(Expected - 'A' + 'a');

                if (Character != Expected)
                    return false;
            }
            return true;
        };

        if (Name.size() == BaseName.size() && HasCaseInsensitiveBase())
            return true;

        if (!HasCaseInsensitiveBase() ||
            Name.size() <= BaseName.size() + 1 ||
            Name[BaseName.size()] != '_')
        {
            return false;
        }

        // Unreal can append a numeric instance suffix to a world name. Do not
        // accept content sub-levels such as Frontend_BG_Main: this refresh is
        // deliberately restricted to the actual frontend world.
        for (const char Character : Name.substr(BaseName.size() + 1))
        {
            if (Character < '0' || Character > '9')
                return false;
        }
        return true;
    }
}
