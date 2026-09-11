// One runtime-owned main-thread Microsoft CRT rand15 stream.
#pragma once

#include <cstdint>
#include <optional>
#include <thread>

enum class NativeSourceRngStatus : std::uint8_t {
    Ready,
    UnknownAuthority,
    WrongThread,
    Unseeded,
    AlreadySeeded,
};

struct NativeSourceRngDraw {
    NativeSourceRngStatus Status = NativeSourceRngStatus::UnknownAuthority;
    std::optional<std::uint16_t> Value;
};

// Diagnostic provenance only; there is deliberately no restore operation.
struct NativeSourceRngProvenance {
    std::uint32_t Seed = 0;
    std::uint32_t State = 0;
    std::uint64_t DrawCount = 0;
    bool operator==(const NativeSourceRngProvenance&) const = default;
};

struct NativeSourceRngInspection {
    NativeSourceRngStatus Status = NativeSourceRngStatus::UnknownAuthority;
    std::optional<NativeSourceRngProvenance> Value;
};

class NativeSourceRng;

// Borrowed, copyable authority reference: never owns or forks random state.
// The owner must outlive every reference and be constructed on the main thread.
// A default reference is explicitly UnknownAuthority. Calls check the actual
// current thread before inspecting mutable state; callers cannot supply an ID.
class NativeSourceRngRef {
public:
    NativeSourceRngRef() = default;
    [[nodiscard]] NativeSourceRngStatus Readiness() const;
    [[nodiscard]] NativeSourceRngDraw NextRand15() const;
    // Exactly local CGeneral::GetRandomNumberInRange(0, 100). One draw even
    // when the caller's alarm/lock chance is zero or 100. Alarm precedes lock,
    // after construction/world insertion and any intervening source consumers.
    [[nodiscard]] NativeSourceRngDraw NextCarGeneratorPercent() const;

private:
    friend class NativeSourceRng;
    explicit NativeSourceRngRef(NativeSourceRng& owner) : m_Owner(&owner) {}
    NativeSourceRng* m_Owner = nullptr;
};

class NativeSourceRng {
public:
    NativeSourceRng();
    NativeSourceRng(const NativeSourceRng&) = delete;
    NativeSourceRng& operator=(const NativeSourceRng&) = delete;
    NativeSourceRng(NativeSourceRng&&) = delete;
    NativeSourceRng& operator=(NativeSourceRng&&) = delete;

    // Caller passes its ONE OS_TimeMS capture immediately after native
    // RenderWare initialization, before any source RNG consumer (GameInit,
    // app_game.cpp:51-55; platform.cpp:49; WinPs.cpp:246). This class reads no
    // clock. Zero is a valid supplied seed; construction does not seed.
    [[nodiscard]] NativeSourceRngStatus SeedOnce(std::uint32_t seed);
    [[nodiscard]] NativeSourceRngStatus Readiness() const;
    [[nodiscard]] NativeSourceRngInspection Inspect() const;
    [[nodiscard]] NativeSourceRngRef Reference() { return NativeSourceRngRef{*this}; }

private:
    friend class NativeSourceRngRef;
    NativeSourceRngDraw NextRand15();

    const std::thread::id m_OwnerThread;
    NativeSourceRngProvenance m_Provenance;
    bool m_Seeded = false;
};
