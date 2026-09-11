#include "app/platform/linux/NativePadFeedback.h"

#include <SDL3/SDL.h>

#include <cstdio>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
int g_Failures;
std::uint64_t g_Now;

struct RumbleCapture {
    bool Succeed = true;
    std::vector<std::pair<Uint16, Uint16>> Calls;
};

std::uint64_t FakeClock() {
    return g_Now;
}

bool SDLCALL CaptureRumble(void* userdata, Uint16 low, Uint16 high) {
    auto& capture = *static_cast<RumbleCapture*>(userdata);
    capture.Calls.emplace_back(low, high);
    if (!capture.Succeed) {
        return SDL_SetError("intentional virtual rumble failure");
    }
    return true;
}

void Check(bool ok, const char* text) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", text);
    g_Failures += !ok;
}

void Require(bool ok, const std::string& error) {
    if (!ok) {
        throw std::runtime_error(error);
    }
}

SDL_JoystickID AttachVirtual(const char* name, RumbleCapture* capture, bool hasRumble) {
    SDL_VirtualJoystickDesc description;
    SDL_INIT_INTERFACE(&description);
    description.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    description.naxes = SDL_GAMEPAD_AXIS_COUNT;
    description.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    description.name = name;
    description.userdata = capture;
    description.Rumble = hasRumble ? CaptureRumble : nullptr;
    const auto id = SDL_AttachVirtualJoystick(&description);
    Require(id != 0, std::string{"attach virtual gamepad: "} + SDL_GetError());
    return id;
}

SDL_Event WaitDeviceEvent(SDL_EventType type, SDL_JoystickID id) {
    const auto deadline = SDL_GetTicksNS() + 100'000'000;
    do {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == type && event.gdevice.which == id) {
                return event;
            }
        }
        SDL_Delay(1);
    } while (SDL_GetTicksNS() < deadline);
    throw std::runtime_error("SDL did not publish the virtual gamepad hotplug event");
}

NativeScriptPadShakeEvent Shake(std::int32_t pickup, std::uint32_t frame) {
    return {{pickup}, frame, 120, 100, 0};
}
} // namespace

int main() try {
    Require(SDL_InitSubSystem(SDL_INIT_VIDEO), std::string{"SDL video init: "} + SDL_GetError());
    Check((SDL_WasInit(SDL_INIT_VIDEO) & SDL_INIT_VIDEO) != 0, "fixture owns a pre-existing SDL video reference");

    g_Now = 0;
    {
        NativePadFeedback feedback({.VirtualDevicesOnly = true, .MonotonicMilliseconds = FakeClock});
        const auto initialized = feedback.Initialize();
        const auto initialState = feedback.GetState();
        Check(initialized.Status == NativePadFeedbackStatus::NoDevice && initialState.Initialized &&
            initialState.OwnsGamepadSubsystem && initialState.Devices == 0,
            "keyboard-only startup is an explicit normal no-device outcome");
        const auto noDevice = feedback.Submit(Shake(1, 10), true);
        Check(noDevice.Status == NativePadFeedbackStatus::NoDevice && !feedback.GetState().Active,
            "collection feedback with no device is acknowledged without fake vibration");
        Check(feedback.Submit(Shake(1, 10), true).Status == NativePadFeedbackStatus::NoReplay,
            "a consumed no-device event cannot replay after later hotplug");
    }
    Check((SDL_WasInit(SDL_INIT_VIDEO) & SDL_INIT_VIDEO) != 0 &&
        !(SDL_WasInit(SDL_INIT_GAMEPAD) & SDL_INIT_GAMEPAD),
        "owned gamepad shutdown preserves the existing SDL video reference");

    Require(SDL_InitSubSystem(SDL_INIT_GAMEPAD), std::string{"SDL gamepad init: "} + SDL_GetError());
    {
        NativePadFeedback feedback({.VirtualDevicesOnly = true, .MonotonicMilliseconds = FakeClock});
        Check(feedback.Initialize().Status == NativePadFeedbackStatus::NoDevice &&
            !feedback.GetState().OwnsGamepadSubsystem,
            "backend borrows an existing gamepad subsystem reference");

        RumbleCapture supported;
        const auto supportedId = AttachVirtual("mad-sa feedback probe", &supported, true);
        const auto supportedAdded = WaitDeviceEvent(SDL_EVENT_GAMEPAD_ADDED, supportedId);
        auto added = feedback.HandleEvent(supportedAdded);
        Check((added.Status == NativePadFeedbackStatus::Idle || added.Status == NativePadFeedbackStatus::Applied) &&
            feedback.GetState().Devices == 1 && feedback.GetState().RumbleDevices == 1,
            "supported virtual gamepad hotplug opens one owned feedback handle");
        feedback.HandleEvent(supportedAdded);
        Check(feedback.GetState().Devices == 1, "repeated hotplug ID does not duplicate the owned handle");

        const auto event = Shake(7, 96);
        const auto mapped = NativePadFeedback_MapShake(event);
        Check(mapped == NativePadFeedbackRumble{25700, 25700, 120},
            "source StartShake(120,100,0) maps full-range strength equally to both SDL motors");
        const auto disabledCalls = supported.Calls.size();
        Check(feedback.Submit(Shake(6, 95), false).Status == NativePadFeedbackStatus::Disabled &&
            supported.Calls.size() == disabledCalls,
            "source vibration preference is explicit and disabled means no SDL call");

        const auto submitted = feedback.Submit(event, true);
        const auto started = feedback.GetState();
        Check(submitted.Status == NativePadFeedbackStatus::Applied && submitted.AppliedDevices == 1 &&
            !supported.Calls.empty() && supported.Calls.back() == std::pair<Uint16, Uint16>{25700, 25700} &&
            started.Active && started.DeadlineMilliseconds == 120 && started.RemainingMilliseconds == 120,
            "virtual callback proves source intensity and 120ms logical deadline");
        const auto callsBeforeReplay = supported.Calls.size();
        Check(feedback.Submit(event, true).Status == NativePadFeedbackStatus::NoReplay &&
            supported.Calls.size() == callsBeforeReplay && feedback.GetState().DeadlineMilliseconds == 120,
            "identical feedback identity is NoReplay and cannot extend the deadline");
        auto changed = event;
        changed.Frequency = 101;
        Check(feedback.Submit(changed, true).Status == NativePadFeedbackStatus::Error &&
            feedback.GetState().Rumble == *mapped && feedback.GetState().DeadlineMilliseconds == 120,
            "changed parameters under a seen identity are an error with active state preserved");

        g_Now = 50;
        const auto callsBeforePause = supported.Calls.size();
        Require(feedback.Update(true).Status == NativePadFeedbackStatus::Idle, "pause active feedback");
        Check(supported.Calls.size() == callsBeforePause + 1 && supported.Calls.back() == std::pair<Uint16, Uint16>{0, 0} &&
            feedback.GetState().Paused && feedback.GetState().RemainingMilliseconds == 120,
            "pause stops physical output and drops the native suspension-transition interval");
        g_Now = 1'050;
        Require(feedback.Update(true).Status == NativePadFeedbackStatus::Idle, "hold paused feedback");
        Check(feedback.GetState().LogicalMilliseconds == 0 && feedback.GetState().RemainingMilliseconds == 120,
            "wall-clock time while paused does not consume the monotonic logical deadline");
        Require(feedback.Update(false).Status == NativePadFeedbackStatus::Idle, "resume active feedback");
        Check(supported.Calls.back() == std::pair<Uint16, Uint16>{25700, 25700},
            "resume reapplies only the source-mapped strength for the remaining duration");
        g_Now = 1'170;
        Require(feedback.Update(false).Status == NativePadFeedbackStatus::Idle, "expire active feedback");
        Check(!feedback.GetState().Active && feedback.GetState().RemainingMilliseconds == 0 &&
            supported.Calls.back() == std::pair<Uint16, Uint16>{0, 0},
            "deadline update stops virtual rumble exactly once at source duration");
        const auto beforeBackwards = feedback.GetState();
        g_Now = 1'169;
        const auto backwards = feedback.Update(false);
        const auto afterBackwards = feedback.GetState();
        Check(backwards.Status == NativePadFeedbackStatus::Error &&
            afterBackwards.LogicalMilliseconds == beforeBackwards.LogicalMilliseconds &&
            afterBackwards.DeadlineMilliseconds == beforeBackwards.DeadlineMilliseconds &&
            afterBackwards.Active == beforeBackwards.Active,
            "backwards monotonic input is reported without mutating feedback state");
        g_Now = 1'170;

        Require(SDL_DetachVirtualJoystick(supportedId), std::string{"detach supported gamepad: "} + SDL_GetError());
        Check(feedback.HandleEvent(WaitDeviceEvent(SDL_EVENT_GAMEPAD_REMOVED, supportedId)).Status == NativePadFeedbackStatus::NoDevice &&
            feedback.GetState().Devices == 0, "hot-unplug closes only the backend-owned gamepad handle");

        RumbleCapture unsupported;
        const auto unsupportedId = AttachVirtual("mad-sa unsupported probe", &unsupported, false);
        Check(feedback.HandleEvent(WaitDeviceEvent(SDL_EVENT_GAMEPAD_ADDED, unsupportedId)).Status == NativePadFeedbackStatus::Unsupported &&
            feedback.GetState().Devices == 1 && feedback.GetState().RumbleDevices == 0,
            "an actual SDL gamepad without rumble is explicitly Unsupported");
        Check(feedback.Submit(Shake(8, 192), true).Status == NativePadFeedbackStatus::Unsupported && unsupported.Calls.empty() &&
            !feedback.GetState().Active, "unsupported gamepad never reports or simulates vibration");
        Require(SDL_DetachVirtualJoystick(unsupportedId), std::string{"detach unsupported gamepad: "} + SDL_GetError());
        feedback.HandleEvent(WaitDeviceEvent(SDL_EVENT_GAMEPAD_REMOVED, unsupportedId));

        RumbleCapture failing{.Succeed = false, .Calls = {}};
        const auto failingId = AttachVirtual("mad-sa failing probe", &failing, true);
        Require(feedback.HandleEvent(WaitDeviceEvent(SDL_EVENT_GAMEPAD_ADDED, failingId)).Status == NativePadFeedbackStatus::Idle,
            "open failing-callback gamepad");
        const auto beforeFailure = feedback.GetState();
        const auto failed = feedback.Submit(Shake(9, 288), true);
        const auto afterFailure = feedback.GetState();
        Check(failed.Status == NativePadFeedbackStatus::Error && failed.Message.find("intentional virtual rumble failure") != std::string::npos &&
            !afterFailure.Active && afterFailure.DeadlineMilliseconds == beforeFailure.DeadlineMilliseconds &&
            afterFailure.Rumble == beforeFailure.Rumble,
            "SDL rumble callback failure is reported and does not fake active state");
        const auto failedCalls = failing.Calls.size();
        Check(feedback.Submit(Shake(9, 288), true).Status == NativePadFeedbackStatus::NoReplay &&
            failing.Calls.size() == failedCalls, "failed consumed feedback is not retried as a replay");
        Require(SDL_DetachVirtualJoystick(failingId), std::string{"detach failing gamepad: "} + SDL_GetError());
        feedback.HandleEvent(WaitDeviceEvent(SDL_EVENT_GAMEPAD_REMOVED, failingId));
    }
    Check((SDL_WasInit(SDL_INIT_GAMEPAD) & SDL_INIT_GAMEPAD) != 0 &&
        (SDL_WasInit(SDL_INIT_VIDEO) & SDL_INIT_VIDEO) != 0,
        "backend destruction releases neither borrowed gamepad nor video subsystem references");

    RumbleCapture shared;
    const auto sharedId = AttachVirtual("mad-sa shared handle probe", &shared, true);
    WaitDeviceEvent(SDL_EVENT_GAMEPAD_ADDED, sharedId);
    auto* userHandle = SDL_OpenGamepad(sharedId);
    Require(userHandle, std::string{"open independent gamepad handle: "} + SDL_GetError());
    {
        NativePadFeedback feedback({.VirtualDevicesOnly = true, .MonotonicMilliseconds = FakeClock});
        Check(feedback.Initialize().Status == NativePadFeedbackStatus::Idle && feedback.GetState().Devices == 1,
            "existing virtual gamepad is discovered without changing its binding");
    }
    Check(SDL_GamepadConnected(userHandle) && SDL_GetGamepadID(userHandle) == sharedId,
        "backend closes only its own open reference and leaves the user's handle valid");
    SDL_CloseGamepad(userHandle);
    Require(SDL_DetachVirtualJoystick(sharedId), std::string{"detach shared gamepad: "} + SDL_GetError());
    WaitDeviceEvent(SDL_EVENT_GAMEPAD_REMOVED, sharedId);

    SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    std::printf("NativePadFeedbackProbe failures=%d source=CPad::StartShake virtual-only=1 movement-ported=0\n", g_Failures);
    return g_Failures ? 1 : 0;
} catch (const std::exception& error) {
    std::fprintf(stderr, "NativePadFeedbackProbe FAIL %s\n", error.what());
    return 1;
}
