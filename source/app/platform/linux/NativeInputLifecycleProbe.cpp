#include "app/platform/linux/NativeInputLifecycle.h"

#include <cstdlib>
#include <iostream>

namespace {
unsigned s_Checks;
void Check(bool condition) { ++s_Checks; if (!condition) std::abort(); }
}

int main() {
    NativeInputLifecycle input;
    std::string error;
    NativeInputDeviceRef keyboard, mouse, gamepad;
    Check(input.Connect(NativeInputDeviceKind::Keyboard, 1, false, keyboard, error) == NativeInputLifecycleStatus::Ok);
    Check(input.Connect(NativeInputDeviceKind::Mouse, 2, false, mouse, error) == NativeInputLifecycleStatus::Ok);
    Check(input.BindSourceDefaults(error) == NativeInputLifecycleStatus::Ok && input.State().Bindings == 34);

    Check(input.SubmitDigital(keyboard, 'W', true, error) == NativeInputLifecycleStatus::Ok &&
        input.SubmitDigital(keyboard, 'F', true, error) == NativeInputLifecycleStatus::Ok &&
        input.SubmitDigital(mouse, 1, true, error) == NativeInputLifecycleStatus::Ok &&
        input.SubmitAxis(mouse, 0, 0.5f, error) == NativeInputLifecycleStatus::Ok);
    NativeInputLifecycleSnapshot first;
    Check(input.Sample(1, 10, first, error) == NativeInputLifecycleStatus::Ok &&
        first.Pad.Sample.MoveY == -128 && first.Pad.Pressed == 4 &&
        first.Actions[std::size_t(NativeInputAction::Fire)] == 1.0f &&
        first.Actions[std::size_t(NativeInputAction::LookX)] == 0.5f);

    Check(input.Connect(NativeInputDeviceKind::Gamepad, 77, true, gamepad, error) == NativeInputLifecycleStatus::Ok &&
        input.SubmitAxis(gamepad, 0, 0.75f, error) == NativeInputLifecycleStatus::Ok &&
        input.SubmitAxis(gamepad, 1, -0.5f, error) == NativeInputLifecycleStatus::Ok &&
        input.SubmitDigital(gamepad, 3, true, error) == NativeInputLifecycleStatus::Ok);
    NativeInputLifecycleSnapshot second;
    Check(input.Sample(2, 20, second, error) == NativeInputLifecycleStatus::Ok &&
        second.Pad.Sample.MoveX == 96 && second.Pad.Sample.MoveY == -128 &&
        second.Pad.Down == 5 && second.Pad.Pressed == 1);
    NativeInputLifecycleSnapshot feedback;
    Check(input.RequestFeedback(gamepad, {25700, 25700, 120}, feedback, error) ==
        NativeInputLifecycleStatus::Ok && feedback.FeedbackSequence == 1 &&
        feedback.FeedbackDevice == gamepad);

    const auto disconnected = gamepad;
    Check(input.Disconnect(gamepad, error) == NativeInputLifecycleStatus::Ok &&
        input.SubmitDigital(disconnected, 3, false, error) == NativeInputLifecycleStatus::StaleDevice);
    const auto retained = input.State();
    Check(input.RequestFeedback(mouse, {1, 1, 1}, feedback, error) ==
        NativeInputLifecycleStatus::Unsupported && input.State() == retained);
    Check(input.Connect(NativeInputDeviceKind::Gamepad, 77, true, gamepad, error) == NativeInputLifecycleStatus::Ok &&
        gamepad != disconnected);
    NativeInputLifecycleSnapshot reconnected;
    Check(input.Sample(3, 30, reconnected, error) == NativeInputLifecycleStatus::Ok &&
        reconnected.Pad.Sample.MoveX == 0 && reconnected.Pad.Down == 4 &&
        reconnected.ConnectedDevices == 3);
    Check(input.SubmitDigital(keyboard, 'W', false, error) == NativeInputLifecycleStatus::Ok &&
        input.SubmitDigital(keyboard, 'F', false, error) == NativeInputLifecycleStatus::Ok &&
        input.SubmitDigital(mouse, NativeInputControls::MouseLeft, false, error) == NativeInputLifecycleStatus::Ok);
    NativeInputLifecycleSnapshot released;
    Check(input.Sample(4, 40, released, error) == NativeInputLifecycleStatus::Ok &&
        released.Pad.Down == 0 && released.Pad.Released == 4);
    Check(input.SubmitDigital(keyboard, NativeInputControls::KeyLeft, true, error) == NativeInputLifecycleStatus::Ok &&
        input.SubmitDigital(keyboard, NativeInputControls::KeyEnter, true, error) == NativeInputLifecycleStatus::Ok &&
        input.SubmitAxis(mouse, 2, -1.0f, error) == NativeInputLifecycleStatus::Ok);
    NativeInputLifecycleSnapshot keyboardMouse;
    Check(input.Sample(5, 50, keyboardMouse, error) == NativeInputLifecycleStatus::Ok &&
        keyboardMouse.Pad.Pressed == 4 &&
        keyboardMouse.Actions[std::size_t(NativeInputAction::LookX)] == 1.0f &&
        keyboardMouse.Actions[std::size_t(NativeInputAction::CycleWeapon)] == -1.0f);
    const auto beforeBadAxis = input.State();
    Check(input.SubmitAxis(gamepad, 0, 2.0f, error) == NativeInputLifecycleStatus::InvalidInput &&
        input.State() == beforeBadAxis);

    std::cout << "native-input-lifecycle-ok checks=" << s_Checks
              << " devices=keyboard,mouse,gamepad hotplug=disconnect,reconnect"
              << " bindings=34 pad=source feedback=25700/120\n";
}
