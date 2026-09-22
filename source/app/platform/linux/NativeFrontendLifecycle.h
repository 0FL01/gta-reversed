#pragma once

#include "NativeSourceCamera.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

enum class NativeFrontendScreen : std::int8_t {
    None = -1,
    DisplaySettings = 4,
    Map = 5,
    Options = 33,
    MainMenu = 34,
    PauseMenu = 41,
};
enum class NativeFrontendStatus : std::uint8_t {
    Ok,
    NotInitialized,
    InvalidState,
    InvalidInput,
    CameraError,
    Overflow,
};
enum class NativeFrontendEventKind : std::uint8_t {
    Initialize,
    StartGame,
    Pause,
    OpenMap,
    PanMap,
    ZoomMap,
    Back,
    OpenOptions,
    OpenDisplay,
    SetDisplay,
    Resume,
};

struct NativeFrontendDisplaySettings {
    std::int32_t Brightness = 256;
    float DrawDistance = 1.2f;
    bool FrameLimiter = true, Hud = true, SavePhotos = true, MipMapping = true;
    std::uint8_t Antialiasing = 1;
    bool Widescreen = false, MapLegend = false;
    std::uint8_t RadarMode = 0;
    bool LocationBlips = true, ContactBlips = true, MissionBlips = true, OtherBlips = true;
    bool GangAreaBlips = true, Subtitles = true;
    bool operator==(const NativeFrontendDisplaySettings&) const = default;
};
struct NativeFrontendMapState {
    float CenterX{}, CenterY{}, Zoom = 1.0f;
    bool operator==(const NativeFrontendMapState&) const = default;
};
struct NativeFrontendEvent {
    std::uint64_t Sequence{}; NativeFrontendEventKind Kind{};
    NativeFrontendScreen From=NativeFrontendScreen::None, To=NativeFrontendScreen::None;
    std::uint32_t TimeMs{}; std::uint64_t InputSequence{};
    bool operator==(const NativeFrontendEvent&) const = default;
};
struct NativeFrontendSnapshot {
    std::uint64_t Epoch{}, Generation{}; std::uint32_t TimeMs{};
    NativeFrontendScreen Screen=NativeFrontendScreen::None;
    bool FrontendActive{}, InGame{}, Paused{};
    NativeFrontendDisplaySettings Display; NativeFrontendMapState Map;
    NativeSourceCameraSnapshot Camera; std::vector<NativeFrontendEvent> Events;
    bool PresentationFeedback=false;
    bool operator==(const NativeFrontendSnapshot&) const = default;
};

// Value-only CMenuManager screen/settings lifecycle composed with the existing
// source camera transition owner. It never samples presentation node state and
// does not manufacture the still-unowned CCam eye/collision solver.
class NativeFrontendLifecycle {
public:
    NativeFrontendStatus Initialize(std::uint64_t epoch, std::uint64_t pedIdentity,
        std::uint32_t nowMs);
    NativeFrontendStatus StartGame(std::uint32_t nowMs, float playerX, float playerY,
        std::uint64_t inputSequence = 0);
    NativeFrontendStatus Pause(std::uint32_t nowMs, std::uint64_t inputSequence = 0);
    NativeFrontendStatus OpenMap(std::uint32_t nowMs, std::uint64_t inputSequence = 0);
    NativeFrontendStatus PanMap(std::uint32_t nowMs, float x, float y,
        std::uint64_t inputSequence = 0);
    NativeFrontendStatus ZoomMap(std::uint32_t nowMs, float zoom,
        std::uint64_t inputSequence = 0);
    NativeFrontendStatus Back(std::uint32_t nowMs, std::uint64_t inputSequence = 0);
    NativeFrontendStatus OpenOptions(std::uint32_t nowMs, std::uint64_t inputSequence = 0);
    NativeFrontendStatus OpenDisplay(std::uint32_t nowMs, std::uint64_t inputSequence = 0);
    NativeFrontendStatus SetDisplay(std::uint32_t nowMs,
        const NativeFrontendDisplaySettings&, std::uint64_t inputSequence = 0);
    NativeFrontendStatus Resume(std::uint32_t nowMs, std::uint64_t inputSequence = 0);
    NativeFrontendStatus SetCameraDirectlyBehind(std::uint32_t nowMs,
        std::array<float, 3> pedForward);
    std::shared_ptr<const NativeFrontendSnapshot> LastCommitted() const noexcept { return m_Published; }
    const NativeSourceCamera& Camera() const noexcept { return m_Camera; }
private:
    NativeFrontendStatus Transition(std::uint32_t, NativeFrontendEventKind, NativeFrontendScreen, std::uint64_t);
    NativeFrontendStatus Republish(NativeFrontendSnapshot&&);
    NativeSourceCamera m_Camera;
    std::shared_ptr<const NativeFrontendSnapshot> m_Published;
    std::vector<NativeFrontendEvent> m_Events;
    std::uint64_t m_NextEvent = 1, m_PedIdentity{};
};
