#include "NativeFrontendLifecycle.h"

#include <cstdio>
#include <cstdlib>

namespace {
int g_Checks = 0;
void Check(bool value, const char* message) {
    ++g_Checks;
    if (!value) {
        std::fprintf(stderr, "frontend-lifecycle-fail: %s\n", message);
        std::exit(1);
    }
}
}

int main() {
    NativeFrontendLifecycle owner;
    Check(owner.Initialize(7, 1001, 10) == NativeFrontendStatus::Ok, "initialize");
    const auto initial = owner.LastCommitted();
    Check(initial->Screen == NativeFrontendScreen::MainMenu && initial->FrontendActive &&
        initial->Display.Brightness == 256 && initial->Display.Hud &&
        initial->Display.RadarMode == 0, "defaults");
    Check(owner.StartGame(20, 2495, -1685, 1) == NativeFrontendStatus::Ok &&
        owner.SetCameraDirectlyBehind(21, {1, 0, 0}) == NativeFrontendStatus::Ok,
        "game camera");
    const auto game = owner.LastCommitted();
    Check(game->InGame && !game->FrontendActive && game->Camera.DirectlyBehind, "game state");
    Check(owner.Pause(30, 2) == NativeFrontendStatus::Ok &&
        owner.OpenMap(31, 3) == NativeFrontendStatus::Ok &&
        owner.PanMap(32, 64, -32, 4) == NativeFrontendStatus::Ok &&
        owner.ZoomMap(33, 2, 5) == NativeFrontendStatus::Ok, "map route");
    const auto map = owner.LastCommitted();
    Check(map->Screen == NativeFrontendScreen::Map && map->Map.CenterX == 2559 &&
        map->Map.CenterY == -1717 && map->Map.Zoom == 2, "map state");
    Check(owner.Back(34, 6) == NativeFrontendStatus::Ok &&
        owner.OpenOptions(35, 7) == NativeFrontendStatus::Ok &&
        owner.OpenDisplay(36, 8) == NativeFrontendStatus::Ok, "settings route");
    auto display = owner.LastCommitted()->Display;
    display.Brightness = 300;
    display.Hud = false;
    display.RadarMode = 1;
    display.Subtitles = false;
    Check(owner.SetDisplay(37, display, 9) == NativeFrontendStatus::Ok &&
        owner.Back(38, 10) == NativeFrontendStatus::Ok &&
        owner.Back(39, 11) == NativeFrontendStatus::Ok &&
        owner.Resume(40, 12) == NativeFrontendStatus::Ok, "resume route");
    const auto restored = owner.LastCommitted();
    Check(restored->InGame && !restored->Paused && !restored->FrontendActive &&
        restored->Display == display && restored->Map == map->Map &&
        restored->Camera.Mode == game->Camera.Mode &&
        restored->Camera.Target == game->Camera.Target && restored->Camera.DirectlyBehind,
        "restore");
    const auto before = *restored;
    Check(owner.Resume(41, 13) == NativeFrontendStatus::InvalidState &&
        *owner.LastCommitted() == before, "atomic invalid");
    Check(restored->Events.size() == 13 && !restored->PresentationFeedback &&
        owner.Camera().ResolveView() == NativeSourceCameraViewStatus::Unsupported,
        "journal boundary");
    std::printf("native-frontend-lifecycle-ok checks=%d route=frontend,game,map,settings,game events=%zu brightness=%d hud=%d radar=%u camera=%u feedback=0\n",
        g_Checks, restored->Events.size(), restored->Display.Brightness,
        restored->Display.Hud, restored->Display.RadarMode,
        unsigned(restored->Camera.Mode));
}
