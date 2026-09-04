#pragma once

#include <cstdint>

namespace AtlasJoinRequestPolicy
{
    enum class ENetDriverState : std::uint8_t
    {
        Unknown,
        Absent,
        Present
    };

    struct FFrontendContext
    {
        bool HasWorld = false;
        bool HasLocalPlayerController = false;
        bool HasCanonicalFrontendWorldName = false;
        ENetDriverState NetDriver = ENetDriverState::Unknown;
    };

    // Some releases briefly expose the lobby through a non-canonical world
    // name. A local controller in a world with a resolved, absent gameplay
    // net driver is still a frontend-capable context. Conversely, a live net
    // driver is a hard gameplay/travel boundary and must close the hotkey
    // path even if the old Frontend world name has not changed yet.
    inline bool ResolveFrontendActive(
        const FFrontendContext& Context) noexcept
    {
        if (!Context.HasWorld || !Context.HasLocalPlayerController ||
            Context.NetDriver == ENetDriverState::Present)
        {
            return false;
        }

        return Context.HasCanonicalFrontendWorldName ||
            Context.NetDriver == ENetDriverState::Absent;
    }

    enum class EJoinRequestSource : std::uint8_t
    {
        QuickHotkey,
        OverlayButton
    };

    inline bool ShouldDispatchJoin(
        EJoinRequestSource Source,
        bool FrontendActive) noexcept
    {
        switch (Source)
        {
        case EJoinRequestSource::QuickHotkey:
            return FrontendActive;

        case EJoinRequestSource::OverlayButton:
            return true;

        default:
            return false;
        }
    }
}
