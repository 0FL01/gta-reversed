#pragma once

#include "NativeInteractionRuntime.h"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

enum class NativeProgressionStatus : std::uint8_t {
    Ok, InvalidInput, NotFound, Locked, CorruptSave, OwnerError, Overflow,
};
struct NativeProgressionInterior {
    std::uint16_t Id = 0;
    bool Unlocked = false;
    std::uint32_t Visits = 0;
    bool operator==(const NativeProgressionInterior&) const = default;
};

class NativeProgressionRuntime {
public:
    static constexpr std::size_t IntegerStats = 223, FloatStats = 82;
    static constexpr std::size_t RewardCount = 64, UnlockCount = 128, InteriorCapacity = 32;

    NativeProgressionStatus SetMoney(std::int32_t, std::string& error);
    NativeProgressionStatus RegisterPurchase(std::string_view, NativeInteractionKind,
        std::uint32_t price, bool owned, std::string& error);
    NativeProgressionStatus Buy(std::string_view, std::string& error);
    NativeProgressionStatus Use(std::string_view, std::string& error);
    NativeProgressionStatus SetIntegerStat(std::uint16_t sourceId, std::int32_t, std::string& error);
    NativeProgressionStatus SetFloatStat(std::uint16_t, float, std::string& error);
    NativeProgressionStatus GrantReward(std::uint8_t, std::int32_t value, std::string& error);
    NativeProgressionStatus SetUnlock(std::uint8_t, bool, std::string& error);
    NativeProgressionStatus RegisterInterior(std::uint16_t id, bool unlocked, std::string& error);
    NativeProgressionStatus UnlockInterior(std::uint16_t id, std::string& error);
    NativeProgressionStatus EnterInterior(std::uint16_t id, std::string& error);
    NativeProgressionStatus Encode(std::vector<std::uint8_t>& out, std::string& error) const;
    NativeProgressionStatus Restore(std::span<const std::uint8_t>, std::string& error);

    const NativeInteractionRuntime& Interactions() const noexcept { return m_Interactions; }
    std::int32_t IntegerStat(std::uint16_t sourceId) const noexcept;
    float FloatStat(std::uint16_t id) const noexcept;
    std::int32_t Reward(std::uint8_t id) const noexcept;
    bool Unlocked(std::uint8_t id) const noexcept;
    const NativeProgressionInterior* Interior(std::uint16_t id) const noexcept;
    std::uint64_t Revision() const noexcept { return m_Revision; }

private:
    NativeProgressionInterior* InteriorMutable(std::uint16_t) noexcept;
    std::array<std::int32_t, IntegerStats> m_IntegerStats{};
    std::array<float, FloatStats> m_FloatStats{};
    std::array<std::int32_t, RewardCount> m_Rewards{};
    std::array<bool, UnlockCount> m_Unlocks{};
    std::array<NativeProgressionInterior, InteriorCapacity> m_Interiors{};
    std::size_t m_InteriorCount = 0;
    NativeInteractionRuntime m_Interactions;
    std::uint64_t m_Revision = 0;
};
