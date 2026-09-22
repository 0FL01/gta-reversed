#include "app/platform/linux/NativeInputLifecycle.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace {
constexpr std::uint16_t kKeyW = 'W';
constexpr std::uint16_t kKeyA = 'A';
constexpr std::uint16_t kKeyS = 'S';
constexpr std::uint16_t kKeyD = 'D';
constexpr std::uint16_t kKeyF = 'F';
constexpr std::uint16_t kKeySpace = 32;
constexpr std::uint16_t kGamepadFire = 1;
constexpr std::uint16_t kGamepadSprint = 2;
constexpr std::uint16_t kGamepadJump = 3;
constexpr std::uint16_t kGamepadEnter = 4;

float ClampAction(float value) {
    return std::clamp(value, -1.0f, 1.0f);
}
}

NativeInputDeviceRef NativeInputLifecycle::Reference(std::size_t slot,
    std::uint16_t generation) noexcept {
    return {std::uint32_t((slot + 1u) << 16u) | generation};
}

NativeInputLifecycle::Device* NativeInputLifecycle::Resolve(NativeInputDeviceRef ref) noexcept {
    const auto slot = std::size_t(ref.Value >> 16u);
    if (!slot || slot > m_Devices.size()) return nullptr;
    auto& device = m_Devices[slot - 1u];
    return device.Connected && device.Generation == std::uint16_t(ref.Value) ? &device : nullptr;
}

const NativeInputLifecycle::Device* NativeInputLifecycle::Resolve(NativeInputDeviceRef ref) const noexcept {
    return const_cast<NativeInputLifecycle*>(this)->Resolve(ref);
}

bool NativeInputLifecycle::AdvanceRevision(std::string& error) noexcept {
    if (m_State.Revision == std::numeric_limits<std::uint64_t>::max()) {
        error = "input lifecycle revision exhausted";
        return false;
    }
    ++m_State.Revision;
    error.clear();
    return true;
}

NativeInputLifecycleStatus NativeInputLifecycle::Connect(NativeInputDeviceKind kind,
    std::uint32_t hardwareId, bool feedback, NativeInputDeviceRef& out, std::string& error) {
    if (!hardwareId || std::uint8_t(kind) > std::uint8_t(NativeInputDeviceKind::Gamepad) ||
        (feedback && kind != NativeInputDeviceKind::Gamepad)) {
        error = "input device connection rejected";
        return NativeInputLifecycleStatus::InvalidInput;
    }
    for (std::size_t i = 0; i < m_Devices.size(); ++i) {
        const auto& device = m_Devices[i];
        if (device.Connected && device.Kind == kind && device.HardwareId == hardwareId) {
            out = Reference(i, device.Generation);
            error.clear();
            return NativeInputLifecycleStatus::Ok;
        }
    }
    const auto found = std::ranges::find(m_Devices, false, &Device::Connected);
    if (found == m_Devices.end()) {
        error = "input device capacity exceeded";
        return NativeInputLifecycleStatus::CapacityExceeded;
    }
    const auto slot = std::size_t(found - m_Devices.begin());
    auto generation = std::uint16_t(found->Generation + 1u);
    if (!generation) generation = 1;
    *found = {};
    found->Generation = generation;
    found->Connected = true;
    found->Feedback = feedback;
    found->Kind = kind;
    found->HardwareId = hardwareId;
    if (!AdvanceRevision(error)) { *found = {}; return NativeInputLifecycleStatus::CapacityExceeded; }
    ++m_State.ConnectedDevices;
    out = Reference(slot, generation);
    return NativeInputLifecycleStatus::Ok;
}

NativeInputLifecycleStatus NativeInputLifecycle::Disconnect(NativeInputDeviceRef ref,
    std::string& error) {
    auto* device = Resolve(ref);
    if (!device) { error = "input device reference is stale"; return NativeInputLifecycleStatus::StaleDevice; }
    device->Connected = false;
    device->Digital = {};
    device->Axes = {};
    --m_State.ConnectedDevices;
    if (!AdvanceRevision(error)) return NativeInputLifecycleStatus::CapacityExceeded;
    return NativeInputLifecycleStatus::Ok;
}

NativeInputLifecycleStatus NativeInputLifecycle::Bind(const NativeInputBinding& binding,
    std::string& error) {
    if (std::uint8_t(binding.Device) > std::uint8_t(NativeInputDeviceKind::Gamepad) ||
        std::uint8_t(binding.ControlKind) > std::uint8_t(NativeInputControlKind::Axis) ||
        std::size_t(binding.Action) >= std::size_t(NativeInputAction::Count) ||
        !std::isfinite(binding.Scale) || binding.Scale == 0.0f ||
        (binding.ControlKind == NativeInputControlKind::Digital && binding.Control >= 512) ||
        (binding.ControlKind == NativeInputControlKind::Axis && binding.Control >= 16)) {
        error = "input binding rejected";
        return NativeInputLifecycleStatus::InvalidInput;
    }
    if (std::ranges::find(m_Bindings.begin(), m_Bindings.begin() + m_BindingCount, binding) !=
        m_Bindings.begin() + m_BindingCount) {
        error.clear();
        return NativeInputLifecycleStatus::Ok;
    }
    if (m_BindingCount == m_Bindings.size()) {
        error = "input binding capacity exceeded";
        return NativeInputLifecycleStatus::CapacityExceeded;
    }
    m_Bindings[m_BindingCount++] = binding;
    m_State.Bindings = m_BindingCount;
    if (!AdvanceRevision(error)) return NativeInputLifecycleStatus::CapacityExceeded;
    return NativeInputLifecycleStatus::Ok;
}

NativeInputLifecycleStatus NativeInputLifecycle::BindSourceDefaults(std::string& error) {
    constexpr std::array defaults{
        NativeInputBinding{NativeInputDeviceKind::Keyboard, NativeInputControlKind::Digital, kKeyW, NativeInputAction::MoveForward, 1},
        NativeInputBinding{NativeInputDeviceKind::Keyboard, NativeInputControlKind::Digital, kKeyS, NativeInputAction::MoveBack, 1},
        NativeInputBinding{NativeInputDeviceKind::Keyboard, NativeInputControlKind::Digital, kKeyA, NativeInputAction::MoveLeft, 1},
        NativeInputBinding{NativeInputDeviceKind::Keyboard, NativeInputControlKind::Digital, kKeyD, NativeInputAction::MoveRight, 1},
        NativeInputBinding{NativeInputDeviceKind::Keyboard, NativeInputControlKind::Digital, NativeInputControls::KeyShift, NativeInputAction::Jump, 1},
        NativeInputBinding{NativeInputDeviceKind::Keyboard, NativeInputControlKind::Digital, kKeySpace, NativeInputAction::Sprint, 1},
        NativeInputBinding{NativeInputDeviceKind::Keyboard, NativeInputControlKind::Digital, kKeyF, NativeInputAction::EnterExit, 1},
        NativeInputBinding{NativeInputDeviceKind::Keyboard, NativeInputControlKind::Digital, NativeInputControls::KeyControl, NativeInputAction::Fire, 1},
        NativeInputBinding{NativeInputDeviceKind::Keyboard, NativeInputControlKind::Digital, NativeInputControls::KeyEscape, NativeInputAction::Pause, 1},
        NativeInputBinding{NativeInputDeviceKind::Keyboard, NativeInputControlKind::Digital, NativeInputControls::KeyLeft, NativeInputAction::LookX, 1},
        NativeInputBinding{NativeInputDeviceKind::Keyboard, NativeInputControlKind::Digital, NativeInputControls::KeyRight, NativeInputAction::LookX, -1},
        NativeInputBinding{NativeInputDeviceKind::Keyboard, NativeInputControlKind::Digital, NativeInputControls::KeyUp, NativeInputAction::LookY, 1},
        NativeInputBinding{NativeInputDeviceKind::Keyboard, NativeInputControlKind::Digital, NativeInputControls::KeyDown, NativeInputAction::LookY, -1},
        NativeInputBinding{NativeInputDeviceKind::Keyboard, NativeInputControlKind::Digital, NativeInputControls::KeyEnter, NativeInputAction::EnterExit, 1},
        NativeInputBinding{NativeInputDeviceKind::Keyboard, NativeInputControlKind::Digital, kKeyW, NativeInputAction::VehicleAccelerate, 1},
        NativeInputBinding{NativeInputDeviceKind::Keyboard, NativeInputControlKind::Digital, kKeyS, NativeInputAction::VehicleBrake, 1},
        NativeInputBinding{NativeInputDeviceKind::Keyboard, NativeInputControlKind::Digital, kKeySpace, NativeInputAction::Handbrake, 1},
        NativeInputBinding{NativeInputDeviceKind::Mouse, NativeInputControlKind::Digital, NativeInputControls::MouseLeft, NativeInputAction::Fire, 1},
        NativeInputBinding{NativeInputDeviceKind::Mouse, NativeInputControlKind::Digital, NativeInputControls::MouseRight, NativeInputAction::Target, 1},
        NativeInputBinding{NativeInputDeviceKind::Mouse, NativeInputControlKind::Digital, NativeInputControls::MouseMiddle, NativeInputAction::LookBehind, 1},
        NativeInputBinding{NativeInputDeviceKind::Mouse, NativeInputControlKind::Axis, 0, NativeInputAction::LookX, 1},
        NativeInputBinding{NativeInputDeviceKind::Mouse, NativeInputControlKind::Axis, 1, NativeInputAction::LookY, 1},
        NativeInputBinding{NativeInputDeviceKind::Mouse, NativeInputControlKind::Axis, 2, NativeInputAction::CycleWeapon, 1},
        NativeInputBinding{NativeInputDeviceKind::Gamepad, NativeInputControlKind::Axis, 0, NativeInputAction::MoveX, 1},
        NativeInputBinding{NativeInputDeviceKind::Gamepad, NativeInputControlKind::Axis, 1, NativeInputAction::MoveY, 1},
        NativeInputBinding{NativeInputDeviceKind::Gamepad, NativeInputControlKind::Axis, 2, NativeInputAction::LookX, 1},
        NativeInputBinding{NativeInputDeviceKind::Gamepad, NativeInputControlKind::Axis, 3, NativeInputAction::LookY, 1},
        NativeInputBinding{NativeInputDeviceKind::Gamepad, NativeInputControlKind::Digital, kGamepadFire, NativeInputAction::Fire, 1},
        NativeInputBinding{NativeInputDeviceKind::Gamepad, NativeInputControlKind::Digital, kGamepadSprint, NativeInputAction::Sprint, 1},
        NativeInputBinding{NativeInputDeviceKind::Gamepad, NativeInputControlKind::Digital, kGamepadJump, NativeInputAction::Jump, 1},
        NativeInputBinding{NativeInputDeviceKind::Gamepad, NativeInputControlKind::Digital, kGamepadEnter, NativeInputAction::EnterExit, 1},
        NativeInputBinding{NativeInputDeviceKind::Gamepad, NativeInputControlKind::Digital, kGamepadSprint, NativeInputAction::VehicleAccelerate, 1},
        NativeInputBinding{NativeInputDeviceKind::Gamepad, NativeInputControlKind::Digital, kGamepadJump, NativeInputAction::VehicleBrake, 1},
        NativeInputBinding{NativeInputDeviceKind::Gamepad, NativeInputControlKind::Digital, 8, NativeInputAction::Handbrake, 1},
    };
    for (const auto& binding : defaults) {
        const auto status = Bind(binding, error);
        if (status != NativeInputLifecycleStatus::Ok) return status;
    }
    return NativeInputLifecycleStatus::Ok;
}

NativeInputLifecycleStatus NativeInputLifecycle::SubmitDigital(NativeInputDeviceRef ref,
    std::uint16_t control, bool down, std::string& error) {
    auto* device = Resolve(ref);
    if (!device) { error = "input digital device reference is stale"; return NativeInputLifecycleStatus::StaleDevice; }
    if (control >= device->Digital.size()) { error = "input digital control rejected"; return NativeInputLifecycleStatus::InvalidInput; }
    device->Digital[control] = down;
    if (!AdvanceRevision(error)) return NativeInputLifecycleStatus::CapacityExceeded;
    return NativeInputLifecycleStatus::Ok;
}

NativeInputLifecycleStatus NativeInputLifecycle::SubmitAxis(NativeInputDeviceRef ref,
    std::uint16_t control, float value, std::string& error) {
    auto* device = Resolve(ref);
    if (!device) { error = "input axis device reference is stale"; return NativeInputLifecycleStatus::StaleDevice; }
    if (control >= device->Axes.size() || !std::isfinite(value) || value < -1.0f || value > 1.0f) {
        error = "input axis control rejected";
        return NativeInputLifecycleStatus::InvalidInput;
    }
    device->Axes[control] = value;
    if (!AdvanceRevision(error)) return NativeInputLifecycleStatus::CapacityExceeded;
    return NativeInputLifecycleStatus::Ok;
}

NativeInputLifecycleStatus NativeInputLifecycle::Sample(std::uint64_t sequence, std::uint64_t tick,
    NativeInputLifecycleSnapshot& out, std::string& error) {
    auto candidate = m_State;
    candidate.Actions = {};
    for (std::size_t bindingIndex = 0; bindingIndex < m_BindingCount; ++bindingIndex) {
        const auto& binding = m_Bindings[bindingIndex];
        float value = 0.0f;
        for (const auto& device : m_Devices) {
            if (!device.Connected || device.Kind != binding.Device) continue;
            value += binding.ControlKind == NativeInputControlKind::Digital
                ? (device.Digital[binding.Control] ? binding.Scale : 0.0f)
                : device.Axes[binding.Control] * binding.Scale;
        }
        auto& action = candidate.Actions[std::size_t(binding.Action)];
        action = ClampAction(action + value);
    }
    const float moveX = ClampAction(candidate.Actions[std::size_t(NativeInputAction::MoveX)] +
        candidate.Actions[std::size_t(NativeInputAction::MoveRight)] -
        candidate.Actions[std::size_t(NativeInputAction::MoveLeft)]);
    const float moveY = ClampAction(candidate.Actions[std::size_t(NativeInputAction::MoveY)] +
        candidate.Actions[std::size_t(NativeInputAction::MoveBack)] -
        candidate.Actions[std::size_t(NativeInputAction::MoveForward)]);
    std::int16_t x = 0, y = 0;
    if (NativeSourcePad::QuantizeAxis(moveX, x) != NativeSourcePadStatus::Ok ||
        NativeSourcePad::QuantizeAxis(moveY, y) != NativeSourcePadStatus::Ok) {
        error = "input lifecycle pad quantization rejected";
        return NativeInputLifecycleStatus::InvalidInput;
    }
    std::uint8_t buttons = 0;
    if (candidate.Actions[std::size_t(NativeInputAction::Jump)] > 0) buttons |= 1;
    if (candidate.Actions[std::size_t(NativeInputAction::Sprint)] > 0) buttons |= 2;
    if (candidate.Actions[std::size_t(NativeInputAction::EnterExit)] > 0) buttons |= 4;
    NativeSourcePadFrame frame;
    const auto status = m_Pad.SubmitSample({sequence, tick, x, y, buttons}, frame);
    if (status != NativeSourcePadStatus::Ok && status != NativeSourcePadStatus::DuplicateIdempotent) {
        error = NativeSourcePad::StatusName(status);
        return NativeInputLifecycleStatus::InvalidInput;
    }
    candidate.Pad = frame;
    candidate.SampleSequence = sequence;
    if (status == NativeSourcePadStatus::Ok && candidate.Revision == std::numeric_limits<std::uint64_t>::max()) {
        error = "input lifecycle revision exhausted";
        return NativeInputLifecycleStatus::CapacityExceeded;
    }
    if (status == NativeSourcePadStatus::Ok) ++candidate.Revision;
    m_State = candidate;
    out = m_State;
    error.clear();
    return NativeInputLifecycleStatus::Ok;
}

NativeInputLifecycleStatus NativeInputLifecycle::RequestFeedback(NativeInputDeviceRef ref,
    const NativeInputFeedbackRumble& rumble, NativeInputLifecycleSnapshot& out,
    std::string& error) {
    const auto* device = Resolve(ref);
    if (!device) { error = "input feedback device reference is stale"; return NativeInputLifecycleStatus::StaleDevice; }
    if (device->Kind != NativeInputDeviceKind::Gamepad || !device->Feedback) {
        error = "input feedback is unsupported by device";
        return NativeInputLifecycleStatus::Unsupported;
    }
    if (!rumble.DurationMs || (!rumble.LowFrequencyStrength && !rumble.HighFrequencyStrength) ||
        m_State.FeedbackSequence == std::numeric_limits<std::uint64_t>::max()) {
        error = "input feedback request rejected";
        return NativeInputLifecycleStatus::InvalidInput;
    }
    ++m_State.FeedbackSequence;
    m_State.FeedbackDevice = ref;
    m_State.Feedback = rumble;
    if (!AdvanceRevision(error)) return NativeInputLifecycleStatus::CapacityExceeded;
    out = m_State;
    return NativeInputLifecycleStatus::Ok;
}
