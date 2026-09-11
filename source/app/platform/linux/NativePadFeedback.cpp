#include "app/platform/linux/NativePadFeedback.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cassert>
#include <limits>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {
std::uint64_t MonotonicMilliseconds() {
    return SDL_GetTicksNS() / 1'000'000;
}

struct SeenShake {
    std::int16_t TimeMs = 0;
    std::uint8_t Frequency = 0;
    std::uint32_t Arg2 = 0;
    bool operator==(const SeenShake&) const = default;
};

std::uint64_t ShakeIdentity(const NativeScriptPadShakeEvent& event) {
    return std::uint64_t(std::uint32_t(event.Pickup.Value)) << 32 | event.FrameCounter;
}
} // namespace

std::optional<NativePadFeedbackRumble> NativePadFeedback_MapShake(const NativeScriptPadShakeEvent& event) {
    if (event.Pickup.Value < 0 || event.TimeMs <= 0 || !event.Frequency) {
        return {};
    }
    const auto strength = std::uint16_t(std::uint16_t(event.Frequency) * 257u);
    return NativePadFeedbackRumble{strength, strength, std::uint32_t(event.TimeMs)};
}

struct NativePadFeedback::Impl {
    struct Device {
        SDL_JoystickID Id = 0;
        SDL_Gamepad* Gamepad = nullptr;
        bool Rumble = false;
    };

    explicit Impl(NativePadFeedbackOptions options) : Options(options) {
        if (!Options.MonotonicMilliseconds) {
            Options.MonotonicMilliseconds = MonotonicMilliseconds;
        }
    }

    ~Impl() {
        Shutdown();
    }

    NativePadFeedbackResult Result(NativePadFeedbackStatus status, std::string message = {}, std::size_t applied = 0) const {
        return {status, Devices.size(), RumbleDeviceCount(), applied, std::move(message)};
    }

    std::size_t RumbleDeviceCount() const {
        return std::ranges::count(Devices, true, &Device::Rumble);
    }

    bool OwnerThread() const {
        return Initialized && Owner == std::this_thread::get_id();
    }

    Device* Target() {
        const auto it = std::ranges::find(Devices, true, &Device::Rumble);
        return it == Devices.end() ? nullptr : &*it;
    }

    const Device* Target() const {
        const auto it = std::ranges::find(Devices, true, &Device::Rumble);
        return it == Devices.end() ? nullptr : &*it;
    }

    std::uint32_t Remaining() const {
        if (!Active || LogicalMilliseconds >= DeadlineMilliseconds) {
            return 0;
        }
        return std::uint32_t(DeadlineMilliseconds - LogicalMilliseconds);
    }

    bool AdvanceClock(bool advanceInterval, std::string& error) {
        const auto now = Options.MonotonicMilliseconds();
        if (now < LastRawMilliseconds) {
            error = "pad feedback monotonic clock moved backwards";
            return false;
        }
        const auto delta = now - LastRawMilliseconds;
        if (advanceInterval && delta > std::numeric_limits<std::uint64_t>::max() - LogicalMilliseconds) {
            error = "pad feedback monotonic clock overflow";
            return false;
        }
        LastRawMilliseconds = now;
        if (advanceInterval) {
            LogicalMilliseconds += delta;
        }
        return true;
    }

    bool Start(Device& device, const NativePadFeedbackRumble& rumble, std::uint32_t duration, std::string& error) {
        SDL_ClearError();
        if (!SDL_RumbleGamepad(device.Gamepad, rumble.LowFrequencyStrength, rumble.HighFrequencyStrength, duration)) {
            error = "SDL gamepad rumble failed";
            if (*SDL_GetError()) {
                error += ": ";
                error += SDL_GetError();
            }
            return false;
        }
        OutputTarget = device.Id;
        return true;
    }

    bool Stop(std::string& error) {
        if (!OutputTarget) {
            return true;
        }
        const auto it = std::ranges::find(Devices, OutputTarget, &Device::Id);
        if (it == Devices.end()) {
            OutputTarget = 0;
            return true;
        }
        SDL_ClearError();
        if (!SDL_RumbleGamepad(it->Gamepad, 0, 0, 0)) {
            error = "SDL gamepad rumble stop failed";
            if (*SDL_GetError()) {
                error += ": ";
                error += SDL_GetError();
            }
            return false;
        }
        OutputTarget = 0;
        return true;
    }

    NativePadFeedbackResult Open(SDL_JoystickID id) {
        if (!id) {
            return Result(NativePadFeedbackStatus::Error, "SDL supplied an invalid gamepad ID");
        }
        if (std::ranges::find(Devices, id, &Device::Id) != Devices.end()) {
            return Result(NativePadFeedbackStatus::Idle, "gamepad ID is already owned by feedback");
        }
        if (Options.VirtualDevicesOnly && !SDL_IsJoystickVirtual(id)) {
            return Result(NativePadFeedbackStatus::Idle);
        }

        SDL_ClearError();
        auto* gamepad = SDL_OpenGamepad(id);
        if (!gamepad) {
            auto message = std::string{"SDL could not open gamepad"};
            if (*SDL_GetError()) {
                message += ": ";
                message += SDL_GetError();
            }
            return Result(NativePadFeedbackStatus::Error, std::move(message));
        }
        const auto properties = SDL_GetGamepadProperties(gamepad);
        if (!properties) {
            auto message = std::string{"SDL could not query gamepad capabilities"};
            if (*SDL_GetError()) {
                message += ": ";
                message += SDL_GetError();
            }
            SDL_CloseGamepad(gamepad);
            return Result(NativePadFeedbackStatus::Error, std::move(message));
        }
        const bool rumble = SDL_GetBooleanProperty(properties, SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false);
        Devices.push_back({id, gamepad, rumble});
        if (!rumble) {
            return Result(NativePadFeedbackStatus::Unsupported,
                "SDL gamepad is present but reports no left/right rumble capability");
        }
        return Result(NativePadFeedbackStatus::Idle, "SDL gamepad feedback ready");
    }

    NativePadFeedbackResult Initialize() {
        if (Initialized) {
            return Result(RumbleDeviceCount() ? NativePadFeedbackStatus::Idle : Devices.empty()
                ? NativePadFeedbackStatus::NoDevice : NativePadFeedbackStatus::Unsupported,
                "pad feedback is already initialized");
        }
        if (!SDL_IsMainThread()) {
            return Result(NativePadFeedbackStatus::Error, "pad feedback must initialize on the SDL main thread");
        }
        Owner = std::this_thread::get_id();
        if (!(SDL_WasInit(SDL_INIT_GAMEPAD) & SDL_INIT_GAMEPAD)) {
            if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
                return Result(NativePadFeedbackStatus::Error,
                    std::string{"SDL gamepad subsystem init failed: "} + SDL_GetError());
            }
            OwnsGamepadSubsystem = true;
        }
        Initialized = true;
        LastRawMilliseconds = Options.MonotonicMilliseconds();

        int count = 0;
        SDL_ClearError();
        auto* ids = SDL_GetGamepads(&count);
        if (!ids) {
            const auto message = *SDL_GetError()
                ? std::string{"SDL gamepad enumeration failed: "} + SDL_GetError()
                : std::string{"no SDL gamepad present; keyboard-only is a normal outcome"};
            return Result(*SDL_GetError() ? NativePadFeedbackStatus::Error : NativePadFeedbackStatus::NoDevice, message);
        }
        NativePadFeedbackResult failure;
        for (const auto id : std::span{ids, std::size_t(count)}) {
            const auto result = Open(id);
            if (result.Status == NativePadFeedbackStatus::Error) {
                failure = result;
                break;
            }
        }
        SDL_free(ids);
        if (failure.Status == NativePadFeedbackStatus::Error) {
            return failure;
        }
        if (Devices.empty()) {
            return Result(NativePadFeedbackStatus::NoDevice,
                "no SDL gamepad present; keyboard-only is a normal outcome");
        }
        if (!RumbleDeviceCount()) {
            return Result(NativePadFeedbackStatus::Unsupported,
                "SDL gamepad is present but reports no left/right rumble capability");
        }
        return Result(NativePadFeedbackStatus::Idle, "SDL gamepad feedback ready");
    }

    NativePadFeedbackResult HandleEvent(const SDL_Event& event) {
        if (!OwnerThread()) {
            return Result(NativePadFeedbackStatus::Error, "pad feedback event handling requires its owner thread");
        }
        if (event.type == SDL_EVENT_GAMEPAD_ADDED) {
            const auto beforeTarget = Target();
            const auto result = Open(event.gdevice.which);
            if (result.Status == NativePadFeedbackStatus::Error || !Active || Paused || beforeTarget || !Target()) {
                return result;
            }
            std::string error;
            if (!Start(*Target(), CurrentRumble, Remaining(), error)) {
                return Result(NativePadFeedbackStatus::Error, std::move(error));
            }
            return Result(NativePadFeedbackStatus::Applied, "active feedback resumed on hotplug", 1);
        }
        if (event.type != SDL_EVENT_GAMEPAD_REMOVED) {
            return Result(NativePadFeedbackStatus::Idle);
        }

        const auto it = std::ranges::find(Devices, event.gdevice.which, &Device::Id);
        if (it == Devices.end()) {
            return Result(Devices.empty() ? NativePadFeedbackStatus::NoDevice : NativePadFeedbackStatus::Idle);
        }
        if (OutputTarget == it->Id) {
            OutputTarget = 0;
        }
        SDL_CloseGamepad(it->Gamepad);
        Devices.erase(it);
        if (Active && !Paused && Target() && Remaining()) {
            std::string error;
            if (!Start(*Target(), CurrentRumble, Remaining(), error)) {
                return Result(NativePadFeedbackStatus::Error, std::move(error));
            }
            return Result(NativePadFeedbackStatus::Applied, "active feedback moved after hot-unplug", 1);
        }
        if (Devices.empty()) {
            return Result(NativePadFeedbackStatus::NoDevice,
                "no SDL gamepad present; keyboard-only is a normal outcome");
        }
        if (!RumbleDeviceCount()) {
            return Result(NativePadFeedbackStatus::Unsupported,
                "SDL gamepad is present but reports no left/right rumble capability");
        }
        return Result(NativePadFeedbackStatus::Idle);
    }

    NativePadFeedbackResult Submit(const NativeScriptPadShakeEvent& event, bool vibrationOn) {
        if (!OwnerThread()) {
            return Result(NativePadFeedbackStatus::Error, "pad feedback submit requires its owner thread");
        }
        const auto rumble = NativePadFeedback_MapShake(event);
        if (!rumble) {
            return Result(NativePadFeedbackStatus::Error, "invalid native pad-shake event");
        }
        std::string error;
        if (!AdvanceClock(!Paused, error)) {
            return Result(NativePadFeedbackStatus::Error, std::move(error));
        }

        const auto identity = ShakeIdentity(event);
        const SeenShake values{event.TimeMs, event.Frequency, event.Arg2};
        if (const auto seen = Seen.find(identity); seen != Seen.end()) {
            if (seen->second == values) {
                return Result(NativePadFeedbackStatus::NoReplay, "pad-shake event was already submitted");
            }
            return Result(NativePadFeedbackStatus::Error,
                "pad-shake event identity was replayed with different parameters");
        }
        Seen.emplace(identity, values);
        if (!vibrationOn) {
            return Result(NativePadFeedbackStatus::Disabled,
                "source vibration preference is disabled; feedback was not played");
        }
        if (Devices.empty()) {
            return Result(NativePadFeedbackStatus::NoDevice,
                "no SDL gamepad present; collection remains successful without feedback");
        }
        auto* target = Target();
        if (!target) {
            return Result(NativePadFeedbackStatus::Unsupported,
                "SDL gamepad is present but reports no left/right rumble capability");
        }
        if (LogicalMilliseconds > std::numeric_limits<std::uint64_t>::max() - rumble->DurationMs) {
            return Result(NativePadFeedbackStatus::Error, "pad feedback deadline overflow");
        }
        if (!Paused && !Start(*target, *rumble, rumble->DurationMs, error)) {
            return Result(NativePadFeedbackStatus::Error, std::move(error));
        }
        CurrentRumble = *rumble;
        Active = true;
        DeadlineMilliseconds = LogicalMilliseconds + rumble->DurationMs;
        return Result(NativePadFeedbackStatus::Applied,
            Paused ? "pad feedback accepted while native time is paused" : "pad feedback applied", Paused ? 0 : 1);
    }

    NativePadFeedbackResult Update(bool paused) {
        if (!OwnerThread()) {
            return Result(NativePadFeedbackStatus::Error, "pad feedback update requires its owner thread");
        }
        std::string error;
        // Native suspension drops the interval entering pause and the interval
        // returning from it, just like Realtime's unpaused game clock.
        if (!AdvanceClock(!Paused && !paused, error)) {
            return Result(NativePadFeedbackStatus::Error, std::move(error));
        }
        if (Active && LogicalMilliseconds >= DeadlineMilliseconds) {
            if (!Stop(error)) {
                return Result(NativePadFeedbackStatus::Error, std::move(error));
            }
            Active = false;
            CurrentRumble = {};
            DeadlineMilliseconds = LogicalMilliseconds;
        }
        if (paused != Paused && Active) {
            if (paused) {
                if (!Stop(error)) {
                    return Result(NativePadFeedbackStatus::Error, std::move(error));
                }
            } else if (auto* target = Target(); target && Remaining()) {
                if (!Start(*target, CurrentRumble, Remaining(), error)) {
                    return Result(NativePadFeedbackStatus::Error, std::move(error));
                }
            }
        }
        Paused = paused;
        if (Active && !Paused && !OutputTarget && Target() && Remaining()) {
            if (!Start(*Target(), CurrentRumble, Remaining(), error)) {
                return Result(NativePadFeedbackStatus::Error, std::move(error));
            }
        }
        return Result(NativePadFeedbackStatus::Idle);
    }

    NativePadFeedbackState GetState() const {
        return {
            .Initialized = Initialized,
            .OwnsGamepadSubsystem = OwnsGamepadSubsystem,
            .Active = Active,
            .Paused = Paused,
            .Devices = Devices.size(),
            .RumbleDevices = RumbleDeviceCount(),
            .SeenEvents = Seen.size(),
            .LogicalMilliseconds = LogicalMilliseconds,
            .DeadlineMilliseconds = DeadlineMilliseconds,
            .RemainingMilliseconds = Remaining(),
            .Rumble = CurrentRumble,
        };
    }

    void Shutdown() {
        if (!Initialized) {
            return;
        }
        assert(Owner == std::this_thread::get_id());
        if (OutputTarget) {
            const auto it = std::ranges::find(Devices, OutputTarget, &Device::Id);
            if (it != Devices.end()) {
                SDL_RumbleGamepad(it->Gamepad, 0, 0, 0);
            }
        }
        for (const auto& device : Devices) {
            SDL_CloseGamepad(device.Gamepad);
        }
        Devices.clear();
        if (OwnsGamepadSubsystem) {
            SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
        }
        Initialized = false;
    }

    NativePadFeedbackOptions Options;
    std::thread::id Owner;
    std::vector<Device> Devices;
    std::unordered_map<std::uint64_t, SeenShake> Seen;
    NativePadFeedbackRumble CurrentRumble;
    SDL_JoystickID OutputTarget = 0;
    std::uint64_t LastRawMilliseconds = 0;
    std::uint64_t LogicalMilliseconds = 0;
    std::uint64_t DeadlineMilliseconds = 0;
    bool Initialized = false;
    bool OwnsGamepadSubsystem = false;
    bool Active = false;
    bool Paused = false;
};

NativePadFeedback::NativePadFeedback(NativePadFeedbackOptions options) : m_Impl(std::make_unique<Impl>(options)) {
}

NativePadFeedback::~NativePadFeedback() = default;

NativePadFeedbackResult NativePadFeedback::Initialize() {
    return m_Impl->Initialize();
}

NativePadFeedbackResult NativePadFeedback::HandleEvent(const SDL_Event& event) {
    return m_Impl->HandleEvent(event);
}

NativePadFeedbackResult NativePadFeedback::Submit(const NativeScriptPadShakeEvent& event, bool vibrationOn) {
    return m_Impl->Submit(event, vibrationOn);
}

NativePadFeedbackResult NativePadFeedback::Update(bool paused) {
    return m_Impl->Update(paused);
}

NativePadFeedbackState NativePadFeedback::GetState() const {
    return m_Impl->GetState();
}
