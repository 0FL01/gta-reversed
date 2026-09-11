#pragma once

#include "app/platform/linux/NativeScriptEntities.h"

#include <SDL3/SDL_events.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

enum class NativePadFeedbackStatus {
    Idle,
    Applied,
    NoDevice,
    Disabled,
    NoReplay,
    Unsupported,
    Error,
};

struct NativePadFeedbackResult {
    NativePadFeedbackStatus Status = NativePadFeedbackStatus::Idle;
    std::size_t Devices = 0;
    std::size_t RumbleDevices = 0;
    std::size_t AppliedDevices = 0;
    std::string Message;
};

struct NativePadFeedbackRumble {
    std::uint16_t LowFrequencyStrength = 0;
    std::uint16_t HighFrequencyStrength = 0;
    std::uint32_t DurationMs = 0;
    bool operator==(const NativePadFeedbackRumble&) const = default;
};

// CPad::StartShake has one uint8 strength/frequency value. SDL names its two
// uint16 values for the motors they drive, but both values are intensities.
// Preserve the one source value on both motors with an exact 8-to-16-bit map.
std::optional<NativePadFeedbackRumble> NativePadFeedback_MapShake(const NativeScriptPadShakeEvent& event);

struct NativePadFeedbackOptions {
    // Probe isolation only. Production leaves this false and queries every
    // gamepad SDL already recognizes without changing mappings/player indices.
    bool VirtualDevicesOnly = false;
    std::uint64_t (*MonotonicMilliseconds)() = nullptr;
};

struct NativePadFeedbackState {
    bool Initialized = false;
    bool OwnsGamepadSubsystem = false;
    bool Active = false;
    bool Paused = false;
    std::size_t Devices = 0;
    std::size_t RumbleDevices = 0;
    std::size_t SeenEvents = 0;
    std::uint64_t LogicalMilliseconds = 0;
    std::uint64_t DeadlineMilliseconds = 0;
    std::uint32_t RemainingMilliseconds = 0;
    NativePadFeedbackRumble Rumble;
};

// Main-thread owner. Construct after SDL video/window initialization and keep it
// on the event-loop thread. Forward GAMEPAD_ADDED/REMOVED events, call Update
// with native suspension state, then submit each ConsumePadShake result once.
// The required vibrationOn argument represents CPad's source preference gate;
// the native settings UI does not currently own that setting.
class NativePadFeedback {
public:
    explicit NativePadFeedback(NativePadFeedbackOptions options = {});
    ~NativePadFeedback();

    NativePadFeedback(const NativePadFeedback&) = delete;
    NativePadFeedback& operator=(const NativePadFeedback&) = delete;

    NativePadFeedbackResult Initialize();
    NativePadFeedbackResult HandleEvent(const SDL_Event& event);
    NativePadFeedbackResult Submit(const NativeScriptPadShakeEvent& event, bool vibrationOn);
    NativePadFeedbackResult Update(bool paused);
    NativePadFeedbackState GetState() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_Impl;
};
