#pragma once

#include "app/platform/linux/NativeSourcePad.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

enum class NativeInputDeviceKind : std::uint8_t { Keyboard, Mouse, Gamepad };
enum class NativeInputControlKind : std::uint8_t { Digital, Axis };
enum class NativeInputAction : std::uint8_t {
    MoveX,
    MoveY,
    MoveForward,
    MoveBack,
    MoveLeft,
    MoveRight,
    Jump,
    Sprint,
    EnterExit,
    Fire,
    Target,
    LookX,
    LookY,
    Pause,
    VehicleAccelerate,
    VehicleBrake,
    Handbrake,
    LookBehind,
    CycleWeapon,
    Count,
};

namespace NativeInputControls {
constexpr std::uint16_t KeyShift = 0x101;
constexpr std::uint16_t KeyControl = 0x102;
constexpr std::uint16_t KeyEscape = 0x103;
constexpr std::uint16_t KeyLeft = 0x104;
constexpr std::uint16_t KeyRight = 0x105;
constexpr std::uint16_t KeyUp = 0x106;
constexpr std::uint16_t KeyDown = 0x107;
constexpr std::uint16_t KeyEnter = 0x108;
constexpr std::uint16_t MouseLeft = 1;
constexpr std::uint16_t MouseRight = 2;
constexpr std::uint16_t MouseMiddle = 3;
}
enum class NativeInputLifecycleStatus : std::uint8_t {
    Ok,
    InvalidInput,
    CapacityExceeded,
    StaleDevice,
    Unsupported,
};

struct NativeInputFeedbackRumble {
    std::uint16_t LowFrequencyStrength = 0;
    std::uint16_t HighFrequencyStrength = 0;
    std::uint32_t DurationMs = 0;
    bool operator==(const NativeInputFeedbackRumble&) const = default;
};

struct NativeInputDeviceRef {
    std::uint32_t Value = 0;
    bool operator==(const NativeInputDeviceRef&) const = default;
};

struct NativeInputBinding {
    NativeInputDeviceKind Device = NativeInputDeviceKind::Keyboard;
    NativeInputControlKind ControlKind = NativeInputControlKind::Digital;
    std::uint16_t Control = 0;
    NativeInputAction Action = NativeInputAction::MoveX;
    float Scale = 1.0f;
    bool operator==(const NativeInputBinding&) const = default;
};

struct NativeInputLifecycleSnapshot {
    std::uint64_t Revision = 0;
    std::uint64_t SampleSequence = 0;
    std::size_t ConnectedDevices = 0;
    std::size_t Bindings = 0;
    NativeSourcePadFrame Pad;
    std::array<float, std::size_t(NativeInputAction::Count)> Actions{};
    std::uint64_t FeedbackSequence = 0;
    NativeInputDeviceRef FeedbackDevice;
    NativeInputFeedbackRumble Feedback;
    bool operator==(const NativeInputLifecycleSnapshot&) const = default;
};

class NativeInputLifecycle {
public:
    static constexpr std::size_t DeviceCapacity = 8;
    static constexpr std::size_t BindingCapacity = 96;

    NativeInputLifecycleStatus Connect(NativeInputDeviceKind kind, std::uint32_t hardwareId,
        bool feedback, NativeInputDeviceRef& out, std::string& error);
    NativeInputLifecycleStatus Disconnect(NativeInputDeviceRef ref, std::string& error);
    NativeInputLifecycleStatus Bind(const NativeInputBinding& binding, std::string& error);
    NativeInputLifecycleStatus BindSourceDefaults(std::string& error);
    NativeInputLifecycleStatus SubmitDigital(NativeInputDeviceRef ref, std::uint16_t control,
        bool down, std::string& error);
    NativeInputLifecycleStatus SubmitAxis(NativeInputDeviceRef ref, std::uint16_t control,
        float value, std::string& error);
    NativeInputLifecycleStatus Sample(std::uint64_t sequence, std::uint64_t tick,
        NativeInputLifecycleSnapshot& out, std::string& error);
    NativeInputLifecycleStatus RequestFeedback(NativeInputDeviceRef ref,
        const NativeInputFeedbackRumble& rumble, NativeInputLifecycleSnapshot& out,
        std::string& error);

    const NativeInputLifecycleSnapshot& State() const noexcept { return m_State; }

private:
    struct Device {
        std::uint16_t Generation = 0;
        bool Connected = false;
        bool Feedback = false;
        NativeInputDeviceKind Kind = NativeInputDeviceKind::Keyboard;
        std::uint32_t HardwareId = 0;
        std::array<bool, 512> Digital{};
        std::array<float, 16> Axes{};
    };

    Device* Resolve(NativeInputDeviceRef) noexcept;
    const Device* Resolve(NativeInputDeviceRef) const noexcept;
    static NativeInputDeviceRef Reference(std::size_t slot, std::uint16_t generation) noexcept;
    bool AdvanceRevision(std::string& error) noexcept;

    std::array<Device, DeviceCapacity> m_Devices{};
    std::array<NativeInputBinding, BindingCapacity> m_Bindings{};
    std::size_t m_BindingCount = 0;
    NativeSourcePad m_Pad;
    NativeInputLifecycleSnapshot m_State;
};
