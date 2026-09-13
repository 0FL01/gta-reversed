// Source CStuntJumpManager registration owner. Registration is intentionally
// separate from runtime detection/reward/camera/reset and persistence coverage.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

struct NativeStuntJumpVector {
    float X = 0, Y = 0, Z = 0;
    bool operator==(const NativeStuntJumpVector&) const = default;
};

struct NativeStuntJumpBox {
    NativeStuntJumpVector Min, Max;
    bool operator==(const NativeStuntJumpBox&) const = default;
};

struct NativeStuntJump {
    NativeStuntJumpBox Start, End;
    NativeStuntJumpVector Camera;
    std::int32_t Reward = 0;
    bool Done = false, Found = false;
    bool operator==(const NativeStuntJump&) const = default;
};

enum class NativeStuntJumpStatus : std::uint8_t { Ok, InvalidInput, Full, Overflow };

struct NativeStuntJumpCoverage {
    bool Registration = true;
    bool RuntimeUpdate = false;
    bool Reward = false;
    bool Reset = false;
    bool SaveLoad = false;
    bool operator==(const NativeStuntJumpCoverage&) const = default;
};

class NativeStuntJumps {
public:
    static constexpr std::size_t Capacity = 256; // CStuntJumpManager source pool
    static constexpr NativeStuntJumpCoverage Coverage{};

    // SCM0814 supplies centers and axis half-sizes. The source handler builds
    // two axis-aligned boxes, then AddOne creates done=false/found=false.
    // Failure is allocation-free and retains the complete registry.
    NativeStuntJumpStatus Add(NativeStuntJumpVector startCenter, NativeStuntJumpVector startHalfSize,
        NativeStuntJumpVector endCenter, NativeStuntJumpVector endHalfSize,
        NativeStuntJumpVector camera, std::int32_t reward, std::size_t& index, std::string& error);

    std::span<const NativeStuntJump> Entries() const noexcept { return {m_Entries.data(), m_Count}; }
    std::uint64_t Revision() const noexcept { return m_Revision; }

private:
    std::array<NativeStuntJump, Capacity> m_Entries{};
    std::size_t m_Count = 0;
    std::uint64_t m_Revision = 0;
};
