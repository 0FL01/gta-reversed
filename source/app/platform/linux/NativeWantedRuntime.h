#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

enum class NativeWantedStatus : std::uint8_t {
    Ok, InvalidInput, CapacityExceeded, Duplicate, Overflow,
};
struct NativeWantedState {
    std::uint32_t Chaos = 0;
    std::uint8_t Level = 0;
    std::uint8_t ChanceOnRoadblock = 0;
    std::uint8_t MaximumCops = 0;
    std::uint8_t MaximumCopCars = 0;
    std::uint64_t LastDecreaseMs = 0;
    std::array<std::uint64_t, 10> PursuitCops{};
    std::uint8_t PursuitCount = 0;
    std::uint64_t Revision = 0;
    bool operator==(const NativeWantedState&) const = default;
};
struct NativeWantedEvent {
    std::uint64_t Sequence = 0;
    NativeWantedState State;
    const char* Kind = nullptr;
};

class NativeWantedRuntime {
public:
    NativeWantedStatus RegisterOffense(std::uint32_t chaos, std::uint64_t nowMs, std::string& error);
    NativeWantedStatus JoinPursuit(std::uint64_t copIdentity, std::string& error);
    NativeWantedStatus EscapeTick(std::uint64_t nowMs, bool policePresent,
        bool elusiveZone, bool elusiveVehicle, std::string& error);
    bool ShouldCreateRoadblock(std::uint8_t randomPercent) const noexcept;
    const NativeWantedState& State() const noexcept { return m_State; }
    const std::vector<NativeWantedEvent>& Events() const noexcept { return m_Events; }

private:
    void UpdateLevel(std::uint64_t nowMs);
    void Record(const char* kind);
    NativeWantedState m_State;
    std::vector<NativeWantedEvent> m_Events;
    std::uint64_t m_Sequence = 0;
};
