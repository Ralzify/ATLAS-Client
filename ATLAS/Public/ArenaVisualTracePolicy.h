#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>

namespace AtlasArenaVisualTracePolicy
{
    inline constexpr std::uint64_t CpfParm = 0x80;
    inline constexpr std::uint64_t CpfOutParm = 0x100;
    inline constexpr std::uint64_t CpfReturnParm = 0x400;
    inline constexpr std::uint64_t CpfReferenceParm = 0x8000000;

    struct FParameterSchema
    {
        const char* Name = nullptr;
        std::uint32_t Offset = 0;
        std::uint32_t ElementSize = 0;
        std::uint64_t PropertyFlags = 0;
    };

    enum class ETournamentStatPayloadSchema : std::uint8_t
    {
        Unsupported,
        CompactNameWithoutDelta,
        LegacyFullName,
        LegacyCompactName,
        ModernCompactNameWithDelta
    };

    struct FTournamentStatFieldSchema
    {
        const char* Name = nullptr;
        std::uint32_t Offset = 0;
        std::uint32_t ElementSize = 0;
    };

    inline bool IsPlainInput(
        const FParameterSchema& Parameter) noexcept
    {
        return (Parameter.PropertyFlags & CpfParm) != 0 &&
            (Parameter.PropertyFlags &
                (CpfOutParm | CpfReturnParm | CpfReferenceParm)) == 0;
    }

    inline bool IsStructInput(
        const FParameterSchema& Parameter) noexcept
    {
        if ((Parameter.PropertyFlags & CpfParm) == 0 ||
            (Parameter.PropertyFlags & CpfReturnParm) != 0)
        {
            return false;
        }

        const bool IsOut =
            (Parameter.PropertyFlags & CpfOutParm) != 0;
        const bool IsReference =
            (Parameter.PropertyFlags & CpfReferenceParm) != 0;
        return !IsOut || IsReference;
    }

    inline bool IsPlacementRpcSchema(
        std::uint32_t ParameterSize,
        const FParameterSchema* Parameters,
        std::size_t ParameterCount) noexcept
    {
        if (ParameterSize != sizeof(std::int32_t) * 2 ||
            !Parameters || ParameterCount != 2)
        {
            return false;
        }

        const auto HasParameter = [&](const char* Name,
                                      std::uint32_t Offset) noexcept
        {
            for (std::size_t Index = 0;
                 Index < ParameterCount; ++Index)
            {
                const auto& Parameter = Parameters[Index];
                if (Parameter.Name &&
                    std::strcmp(Parameter.Name, Name) == 0 &&
                    Parameter.Offset == Offset &&
                    Parameter.ElementSize == sizeof(std::int32_t) &&
                    IsPlainInput(Parameter))
                {
                    return true;
                }
            }
            return false;
        };

        return HasParameter("Placement", 0) &&
            HasParameter("PointsEarned", sizeof(std::int32_t));
    }

    inline bool IsTeamPlacementConsumerSchema(
        std::uint32_t ParameterSize,
        const FParameterSchema* Parameters,
        std::size_t ParameterCount) noexcept
    {
        if (ParameterSize != sizeof(std::int32_t) ||
            !Parameters || ParameterCount != 1)
        {
            return false;
        }
        const auto& Parameter = Parameters[0];
        return Parameter.Name &&
            std::strcmp(Parameter.Name, "NewTeamPlacement") == 0 &&
            Parameter.Offset == 0 &&
            Parameter.ElementSize == sizeof(std::int32_t) &&
            IsPlainInput(Parameter);
    }

    inline bool IsTournamentStatRpcSchema(
        std::uint32_t ParameterSize,
        const FParameterSchema* Parameters,
        std::size_t ParameterCount) noexcept
    {
        if ((ParameterSize != 0x18 && ParameterSize != 0x20) ||
            !Parameters ||
            ParameterCount != 1)
        {
            return false;
        }

        const auto& Parameter = Parameters[0];
        const bool KnownName = Parameter.Name &&
            (std::strcmp(Parameter.Name, "TournamentStatInfo") == 0 ||
             std::strcmp(Parameter.Name, "StatInfo") == 0);
        return KnownName && Parameter.Offset == 0 &&
            Parameter.ElementSize == ParameterSize &&
            IsStructInput(Parameter);
    }

    inline bool IsZeroParameterSchema(
        std::uint32_t ParameterSize,
        std::size_t ParameterCount) noexcept
    {
        return ParameterSize == 0 && ParameterCount == 0;
    }

    inline bool IsSetBrushFromTextureSchema(
        std::uint32_t ParameterSize,
        const FParameterSchema* Parameters,
        std::size_t ParameterCount) noexcept
    {
        if (ParameterSize != 0x10 || !Parameters ||
            ParameterCount != 2)
        {
            return false;
        }

        bool HasTexture = false;
        bool HasMatchSize = false;
        for (std::size_t Index = 0; Index < ParameterCount; ++Index)
        {
            const auto& Parameter = Parameters[Index];
            if (!Parameter.Name || !IsPlainInput(Parameter))
                return false;
            if (std::strcmp(Parameter.Name, "Texture") == 0 &&
                Parameter.Offset == 0 &&
                Parameter.ElementSize == sizeof(void*))
            {
                HasTexture = true;
            }
            else if (std::strcmp(
                         Parameter.Name, "bMatchSize") == 0 &&
                     Parameter.Offset == sizeof(void*) &&
                     Parameter.ElementSize == sizeof(std::uint8_t))
            {
                HasMatchSize = true;
            }
            else
            {
                return false;
            }
        }
        return HasTexture && HasMatchSize;
    }

    inline ETournamentStatPayloadSchema ResolveTournamentStatPayloadSchema(
        std::uint32_t ParameterSize,
        const FTournamentStatFieldSchema* Fields,
        std::size_t FieldCount) noexcept
    {
        constexpr std::uint32_t CompactSize = 0x18;
        constexpr std::uint32_t ExtendedSize = 0x20;
        if (!Fields || (FieldCount != 3 && FieldCount != 4) ||
            (ParameterSize != CompactSize && ParameterSize != ExtendedSize))
        {
            return ETournamentStatPayloadSchema::Unsupported;
        }

        const auto HasField = [&](const char* Name,
                                  std::uint32_t Offset,
                                  std::uint32_t ElementSize) noexcept
        {
            for (std::size_t Index = 0; Index < FieldCount; ++Index)
            {
                const auto& Field = Fields[Index];
                if (Field.Name && std::strcmp(Field.Name, Name) == 0 &&
                    Field.Offset == Offset &&
                    Field.ElementSize == ElementSize)
                {
                    return true;
                }
            }
            return false;
        };

        if (!HasField("StatName", 0x0, 0x10))
            return ETournamentStatPayloadSchema::Unsupported;

        if (ParameterSize == CompactSize && FieldCount == 3 &&
            HasField("StatDisplayName", 0x10, 0x4) &&
            HasField("StatValue", 0x14, 0x4))
        {
            return ETournamentStatPayloadSchema::CompactNameWithoutDelta;
        }
        if (ParameterSize != ExtendedSize ||
            !HasField("StatValue", 0x18, 0x4))
        {
            return ETournamentStatPayloadSchema::Unsupported;
        }
        if (FieldCount == 3 &&
            HasField("StatDisplayName", 0x10, 0x8))
        {
            return ETournamentStatPayloadSchema::LegacyFullName;
        }
        if (FieldCount == 3 &&
            HasField("StatDisplayName", 0x10, 0x4))
        {
            return ETournamentStatPayloadSchema::LegacyCompactName;
        }
        if (FieldCount == 4 &&
            HasField("StatDisplayName", 0x10, 0x4) &&
            HasField("bIsDeltaCount", 0x14, 0x1))
        {
            return ETournamentStatPayloadSchema::ModernCompactNameWithDelta;
        }
        return ETournamentStatPayloadSchema::Unsupported;
    }

    inline std::uint32_t TournamentStatDisplayNameSize(
        ETournamentStatPayloadSchema Schema) noexcept
    {
        return Schema == ETournamentStatPayloadSchema::LegacyFullName
            ? 8u
            : (Schema == ETournamentStatPayloadSchema::
                    CompactNameWithoutDelta ||
               Schema == ETournamentStatPayloadSchema::LegacyCompactName ||
               Schema == ETournamentStatPayloadSchema::
                    ModernCompactNameWithDelta
                ? 4u
                : 0u);
    }

    inline std::uint32_t TournamentStatValueOffset(
        ETournamentStatPayloadSchema Schema) noexcept
    {
        return Schema == ETournamentStatPayloadSchema::
                CompactNameWithoutDelta
            ? 0x14u
            : (Schema == ETournamentStatPayloadSchema::LegacyFullName ||
               Schema == ETournamentStatPayloadSchema::LegacyCompactName ||
               Schema == ETournamentStatPayloadSchema::
                    ModernCompactNameWithDelta
                ? 0x18u
                : 0u);
    }

    inline bool TournamentStatHasDeltaFlag(
        ETournamentStatPayloadSchema Schema) noexcept
    {
        return Schema == ETournamentStatPayloadSchema::
            ModernCompactNameWithDelta;
    }

    inline bool DidPlacementStatTransitionPresentExactlyOnce(
        std::uint64_t PointCallsBefore,
        std::uint64_t PointCallsAfterPrime,
        std::uint64_t PointCallsAfterTarget) noexcept
    {
        return PointCallsAfterPrime == PointCallsBefore &&
            PointCallsAfterTarget == PointCallsBefore + 1;
    }

    inline bool IsEliminationTournamentStat(
        const wchar_t* StatName,
        std::size_t CharacterCountIncludingNull) noexcept
    {
        constexpr wchar_t EliminationStat[] =
            L"TEAM_ELIMS_STAT_INDEX";
        constexpr std::size_t ExpectedCount =
            sizeof(EliminationStat) / sizeof(EliminationStat[0]);
        return StatName &&
            CharacterCountIncludingNull == ExpectedCount &&
            std::wmemcmp(
                StatName, EliminationStat, ExpectedCount) == 0;
    }

    inline bool IsPlacementTournamentStat(
        const wchar_t* StatName,
        std::size_t CharacterCountIncludingNull) noexcept
    {
        constexpr wchar_t PlacementStat[] =
            L"PLACEMENT_STAT_INDEX";
        constexpr std::size_t ExpectedCount =
            sizeof(PlacementStat) / sizeof(PlacementStat[0]);
        return StatName &&
            CharacterCountIncludingNull == ExpectedCount &&
            std::wmemcmp(
                StatName, PlacementStat, ExpectedCount) == 0;
    }

    inline bool IsMatchPlayedTournamentStat(
        const wchar_t* StatName,
        std::size_t CharacterCountIncludingNull) noexcept
    {
        constexpr wchar_t MatchPlayedStat[] =
            L"MATCH_PLAYED_STAT";
        constexpr std::size_t ExpectedCount =
            sizeof(MatchPlayedStat) / sizeof(MatchPlayedStat[0]);
        return StatName &&
            CharacterCountIncludingNull == ExpectedCount &&
            std::wmemcmp(
                StatName, MatchPlayedStat, ExpectedCount) == 0;
    }

    inline bool TryParsePlacementBridgeMessage(
        const wchar_t* Text,
        std::size_t CharacterCount,
        std::int32_t* Placement,
        std::int32_t* PointsEarned,
        bool* DedicatedRpcExpected) noexcept
    {
        if (Placement)
            *Placement = 0;
        if (PointsEarned)
            *PointsEarned = 0;
        if (DedicatedRpcExpected)
            *DedicatedRpcExpected = false;
        constexpr wchar_t Prefix[] =
            L"ATLAS_ARENA_PLACEMENT_V1:";
        constexpr std::size_t PrefixLength =
            (sizeof(Prefix) / sizeof(Prefix[0])) - 1;
        constexpr std::size_t PlacementDigits = 3;
        constexpr std::size_t PointsDigits = 6;
        constexpr std::size_t SeparatorCharacters = 2;
        constexpr std::size_t FlagCharacters = 1;
        if (!Text || !Placement || !PointsEarned ||
            !DedicatedRpcExpected ||
            CharacterCount != PrefixLength + PlacementDigits +
                SeparatorCharacters + PointsDigits + FlagCharacters ||
            std::wmemcmp(Text, Prefix, PrefixLength) != 0)
        {
            return false;
        }

        std::int32_t Value = 0;
        for (std::size_t Index = 0;
             Index < PlacementDigits; ++Index)
        {
            const wchar_t Character = Text[PrefixLength + Index];
            if (Character < L'0' || Character > L'9')
                return false;
            Value = Value * 10 + (Character - L'0');
        }
        if (Value <= 0 || Value > 100)
            return false;

        const std::size_t SeparatorOffset =
            PrefixLength + PlacementDigits;
        if (Text[SeparatorOffset] != L':')
            return false;
        std::int32_t PointsValue = 0;
        for (std::size_t Index = 0;
             Index < PointsDigits; ++Index)
        {
            const wchar_t Character =
                Text[SeparatorOffset + 1 + Index];
            if (Character < L'0' || Character > L'9')
                return false;
            PointsValue = PointsValue * 10 + (Character - L'0');
        }
        if (PointsValue <= 0 || PointsValue > 999999)
            return false;
        const std::size_t FlagSeparatorOffset =
            SeparatorOffset + 1 + PointsDigits;
        if (Text[FlagSeparatorOffset] != L':' ||
            (Text[FlagSeparatorOffset + 1] != L'0' &&
             Text[FlagSeparatorOffset + 1] != L'1'))
        {
            return false;
        }
        *Placement = Value;
        *PointsEarned = PointsValue;
        *DedicatedRpcExpected =
            Text[FlagSeparatorOffset + 1] == L'1';
        return true;
    }

    inline bool TryParsePlacementBridgeMessageV2(
        const wchar_t* Text,
        std::size_t CharacterCount,
        std::int32_t* PreviousPlacement,
        std::int32_t* Placement,
        std::int32_t* PointsEarned,
        bool* DedicatedRpcExpected) noexcept
    {
        if (PreviousPlacement)
            *PreviousPlacement = 0;
        if (Placement)
            *Placement = 0;
        if (PointsEarned)
            *PointsEarned = 0;
        if (DedicatedRpcExpected)
            *DedicatedRpcExpected = false;

        constexpr wchar_t Prefix[] =
            L"ATLAS_ARENA_PLACEMENT_V2:";
        constexpr std::size_t PrefixLength =
            (sizeof(Prefix) / sizeof(Prefix[0])) - 1;
        constexpr std::size_t PlacementDigits = 3;
        constexpr std::size_t PointsDigits = 6;
        constexpr std::size_t Separators = 3;
        constexpr std::size_t FlagCharacters = 1;
        constexpr std::size_t ExpectedCharacters =
            PrefixLength + PlacementDigits * 2 + PointsDigits +
            Separators + FlagCharacters;
        if (!Text || !PreviousPlacement || !Placement || !PointsEarned ||
            !DedicatedRpcExpected || CharacterCount != ExpectedCharacters ||
            std::wmemcmp(Text, Prefix, PrefixLength) != 0)
        {
            return false;
        }

        const auto ParseDigits = [&](std::size_t Offset,
                                     std::size_t Count,
                                     std::int32_t* Value) noexcept
        {
            std::int32_t Parsed = 0;
            for (std::size_t Index = 0; Index < Count; ++Index)
            {
                const wchar_t Character = Text[Offset + Index];
                if (Character < L'0' || Character > L'9')
                    return false;
                Parsed = Parsed * 10 + (Character - L'0');
            }
            *Value = Parsed;
            return true;
        };

        const std::size_t PreviousOffset = PrefixLength;
        const std::size_t TargetSeparator =
            PreviousOffset + PlacementDigits;
        const std::size_t TargetOffset = TargetSeparator + 1;
        const std::size_t PointsSeparator =
            TargetOffset + PlacementDigits;
        const std::size_t PointsOffset = PointsSeparator + 1;
        const std::size_t FlagSeparator = PointsOffset + PointsDigits;
        if (Text[TargetSeparator] != L':' ||
            Text[PointsSeparator] != L':' ||
            Text[FlagSeparator] != L':' ||
            (Text[FlagSeparator + 1] != L'0' &&
             Text[FlagSeparator + 1] != L'1') ||
            !ParseDigits(PreviousOffset, PlacementDigits,
                PreviousPlacement) ||
            !ParseDigits(TargetOffset, PlacementDigits, Placement) ||
            !ParseDigits(PointsOffset, PointsDigits, PointsEarned) ||
            *PreviousPlacement <= *Placement ||
            *PreviousPlacement > 101 || *Placement <= 0 ||
            *Placement > 100 || *PointsEarned <= 0 ||
            *PointsEarned > 999999)
        {
            *PreviousPlacement = 0;
            *Placement = 0;
            *PointsEarned = 0;
            return false;
        }

        *DedicatedRpcExpected = Text[FlagSeparator + 1] == L'1';
        return true;
    }

    inline bool DoesPlacementBridgeMatchPresentedRpc(
        std::int32_t ObservedPlacement,
        std::int32_t ObservedPointsEarned,
        bool ObservedRpcPresented,
        std::int32_t BridgedPlacement,
        std::int32_t BridgedPointsEarned) noexcept
    {
        return ObservedRpcPresented &&
            ObservedPlacement > 0 &&
            ObservedPointsEarned > 0 &&
            ObservedPlacement == BridgedPlacement &&
            ObservedPointsEarned == BridgedPointsEarned;
    }

}
