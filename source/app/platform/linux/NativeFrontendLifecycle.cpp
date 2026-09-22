#include "NativeFrontendLifecycle.h"

#include <cmath>
#include <limits>

namespace {
bool ValidDisplay(const NativeFrontendDisplaySettings& settings) {
    return settings.Brightness >= 0 && settings.Brightness <= 384 &&
        std::isfinite(settings.DrawDistance) && settings.DrawDistance >= 0.925f &&
        settings.DrawDistance <= 1.8f && settings.Antialiasing <= 4 &&
        settings.RadarMode <= 2;
}
}

NativeFrontendStatus NativeFrontendLifecycle::Republish(NativeFrontendSnapshot&& state) {
    try {
        state.Events = m_Events;
        m_Published = std::make_shared<const NativeFrontendSnapshot>(std::move(state));
        return NativeFrontendStatus::Ok;
    } catch (...) {
        return NativeFrontendStatus::Overflow;
    }
}

NativeFrontendStatus NativeFrontendLifecycle::Initialize(std::uint64_t epoch,
    std::uint64_t pedIdentity, std::uint32_t nowMs) {
    if (!epoch || !pedIdentity) return NativeFrontendStatus::InvalidInput;
    if (m_Camera.Initialize(epoch, pedIdentity, nowMs) != NativeSourceCameraStatus::Ok)
        return NativeFrontendStatus::CameraError;

    NativeFrontendSnapshot state;
    state.Epoch = epoch;
    state.Generation = 1;
    state.TimeMs = nowMs;
    state.Screen = NativeFrontendScreen::MainMenu;
    state.FrontendActive = true;
    state.Camera = *m_Camera.LastCommitted();
    try {
        m_Events = {{m_NextEvent, NativeFrontendEventKind::Initialize,
            NativeFrontendScreen::None, NativeFrontendScreen::MainMenu, nowMs, 0}};
    } catch (...) {
        return NativeFrontendStatus::Overflow;
    }
    const auto status = Republish(std::move(state));
    if (status == NativeFrontendStatus::Ok) {
        ++m_NextEvent;
        m_PedIdentity = pedIdentity;
    }
    return status;
}

NativeFrontendStatus NativeFrontendLifecycle::Transition(std::uint32_t nowMs,
    NativeFrontendEventKind kind, NativeFrontendScreen target, std::uint64_t inputSequence) {
    if (!m_Published) return NativeFrontendStatus::NotInitialized;
    if (nowMs < m_Published->TimeMs) return NativeFrontendStatus::InvalidInput;
    if (m_Published->Generation == std::numeric_limits<std::uint64_t>::max() ||
        m_NextEvent == std::numeric_limits<std::uint64_t>::max())
        return NativeFrontendStatus::Overflow;
    if (m_Camera.Advance(nowMs) != NativeSourceCameraStatus::Ok)
        return NativeFrontendStatus::CameraError;

    auto state = *m_Published;
    try {
        m_Events.push_back({m_NextEvent, kind, state.Screen, target, nowMs, inputSequence});
    } catch (...) {
        return NativeFrontendStatus::Overflow;
    }
    state.Generation++;
    state.TimeMs = nowMs;
    state.Screen = target;
    state.Camera = *m_Camera.LastCommitted();
    const auto status = Republish(std::move(state));
    if (status == NativeFrontendStatus::Ok) ++m_NextEvent;
    else m_Events.pop_back();
    return status;
}

NativeFrontendStatus NativeFrontendLifecycle::StartGame(std::uint32_t nowMs,
    float playerX, float playerY, std::uint64_t inputSequence) {
    if (!m_Published || m_Published->Screen != NativeFrontendScreen::MainMenu ||
        !std::isfinite(playerX) || !std::isfinite(playerY))
        return NativeFrontendStatus::InvalidState;
    const auto status = Transition(nowMs, NativeFrontendEventKind::StartGame,
        NativeFrontendScreen::None, inputSequence);
    if (status != NativeFrontendStatus::Ok) return status;
    auto state = *m_Published;
    state.FrontendActive = false;
    state.InGame = true;
    state.Paused = false;
    state.Map = {playerX, playerY, 1};
    return Republish(std::move(state));
}

NativeFrontendStatus NativeFrontendLifecycle::Pause(std::uint32_t nowMs,
    std::uint64_t inputSequence) {
    if (!m_Published || !m_Published->InGame || m_Published->FrontendActive)
        return NativeFrontendStatus::InvalidState;
    const auto status = Transition(nowMs, NativeFrontendEventKind::Pause,
        NativeFrontendScreen::PauseMenu, inputSequence);
    if (status != NativeFrontendStatus::Ok) return status;
    auto state = *m_Published;
    state.FrontendActive = true;
    state.Paused = true;
    return Republish(std::move(state));
}

NativeFrontendStatus NativeFrontendLifecycle::OpenMap(std::uint32_t nowMs,
    std::uint64_t inputSequence) {
    if (!m_Published || m_Published->Screen != NativeFrontendScreen::PauseMenu)
        return NativeFrontendStatus::InvalidState;
    return Transition(nowMs, NativeFrontendEventKind::OpenMap,
        NativeFrontendScreen::Map, inputSequence);
}

NativeFrontendStatus NativeFrontendLifecycle::PanMap(std::uint32_t nowMs,
    float x, float y, std::uint64_t inputSequence) {
    if (!m_Published || m_Published->Screen != NativeFrontendScreen::Map ||
        !std::isfinite(x) || !std::isfinite(y)) return NativeFrontendStatus::InvalidState;
    const auto centerX = m_Published->Map.CenterX + x / m_Published->Map.Zoom;
    const auto centerY = m_Published->Map.CenterY + y / m_Published->Map.Zoom;
    if (!std::isfinite(centerX) || !std::isfinite(centerY)) return NativeFrontendStatus::Overflow;
    const auto status = Transition(nowMs, NativeFrontendEventKind::PanMap,
        NativeFrontendScreen::Map, inputSequence);
    if (status != NativeFrontendStatus::Ok) return status;
    auto state = *m_Published;
    state.Map.CenterX = centerX;
    state.Map.CenterY = centerY;
    return Republish(std::move(state));
}

NativeFrontendStatus NativeFrontendLifecycle::ZoomMap(std::uint32_t nowMs,
    float zoom, std::uint64_t inputSequence) {
    if (!m_Published || m_Published->Screen != NativeFrontendScreen::Map ||
        !std::isfinite(zoom) || zoom < 1 || zoom > 4)
        return NativeFrontendStatus::InvalidInput;
    const auto status = Transition(nowMs, NativeFrontendEventKind::ZoomMap,
        NativeFrontendScreen::Map, inputSequence);
    if (status != NativeFrontendStatus::Ok) return status;
    auto state = *m_Published;
    state.Map.Zoom = zoom;
    return Republish(std::move(state));
}

NativeFrontendStatus NativeFrontendLifecycle::Back(std::uint32_t nowMs,
    std::uint64_t inputSequence) {
    if (!m_Published) return NativeFrontendStatus::NotInitialized;
    const auto from = m_Published->Screen;
    const auto target = from == NativeFrontendScreen::DisplaySettings
        ? NativeFrontendScreen::Options
        : (from == NativeFrontendScreen::Map || from == NativeFrontendScreen::Options)
            ? NativeFrontendScreen::PauseMenu : NativeFrontendScreen::None;
    if (target == NativeFrontendScreen::None) return NativeFrontendStatus::InvalidState;
    return Transition(nowMs, NativeFrontendEventKind::Back, target, inputSequence);
}

NativeFrontendStatus NativeFrontendLifecycle::OpenOptions(std::uint32_t nowMs,
    std::uint64_t inputSequence) {
    if (!m_Published || m_Published->Screen != NativeFrontendScreen::PauseMenu)
        return NativeFrontendStatus::InvalidState;
    return Transition(nowMs, NativeFrontendEventKind::OpenOptions,
        NativeFrontendScreen::Options, inputSequence);
}

NativeFrontendStatus NativeFrontendLifecycle::OpenDisplay(std::uint32_t nowMs,
    std::uint64_t inputSequence) {
    if (!m_Published || m_Published->Screen != NativeFrontendScreen::Options)
        return NativeFrontendStatus::InvalidState;
    return Transition(nowMs, NativeFrontendEventKind::OpenDisplay,
        NativeFrontendScreen::DisplaySettings, inputSequence);
}

NativeFrontendStatus NativeFrontendLifecycle::SetDisplay(std::uint32_t nowMs,
    const NativeFrontendDisplaySettings& display, std::uint64_t inputSequence) {
    if (!m_Published || m_Published->Screen != NativeFrontendScreen::DisplaySettings ||
        !ValidDisplay(display)) return NativeFrontendStatus::InvalidInput;
    const auto status = Transition(nowMs, NativeFrontendEventKind::SetDisplay,
        NativeFrontendScreen::DisplaySettings, inputSequence);
    if (status != NativeFrontendStatus::Ok) return status;
    auto state = *m_Published;
    state.Display = display;
    return Republish(std::move(state));
}

NativeFrontendStatus NativeFrontendLifecycle::Resume(std::uint32_t nowMs,
    std::uint64_t inputSequence) {
    if (!m_Published || m_Published->Screen != NativeFrontendScreen::PauseMenu ||
        !m_Published->InGame) return NativeFrontendStatus::InvalidState;
    const auto status = Transition(nowMs, NativeFrontendEventKind::Resume,
        NativeFrontendScreen::None, inputSequence);
    if (status != NativeFrontendStatus::Ok) return status;
    auto state = *m_Published;
    state.FrontendActive = false;
    state.Paused = false;
    return Republish(std::move(state));
}

NativeFrontendStatus NativeFrontendLifecycle::SetCameraDirectlyBehind(
    std::uint32_t nowMs, std::array<float, 3> forward) {
    if (!m_Published || !m_Published->InGame) return NativeFrontendStatus::InvalidState;
    if (m_Camera.SetDirectlyBehind(nowMs, forward) != NativeSourceCameraStatus::Ok)
        return NativeFrontendStatus::CameraError;
    auto state = *m_Published;
    state.Generation++;
    state.TimeMs = nowMs;
    state.Camera = *m_Camera.LastCommitted();
    return Republish(std::move(state));
}
