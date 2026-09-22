#pragma once

#include "NativeRestarts.h"

#include <cstdint>
#include <string>

struct NativeRestartActorState {
    NativeScriptPosition Position;
    float HeadingRadians = 0.0f;
    float Health = 100.0f;
    float Armour = 0.0f;
    std::uint8_t WantedLevel = 0;
    std::uint8_t Area = 0;
    std::uint64_t WorldGeneration = 1;
    std::uint64_t TaskGeneration = 1;
    bool ControlEnabled = true;
    bool CameraBehindPlayer = true;
    bool WorldCleared = false;
    bool EntryExitReset = false;
    bool SceneStreamed = false;
    bool GameplayReset = false;
    bool operator==(const NativeRestartActorState&) const = default;
};

enum class NativeRestartLifecycleStatus : std::uint8_t {
    Ok, Unsupported, InvalidInput, StaleSelection, Overflow,
};

class NativeRestartLifecycle {
public:
    explicit NativeRestartLifecycle(NativeRestarts& restarts) : m_Restarts(restarts) {}
    NativeRestartLifecycleStatus Recover(const NativeRestartQuery&,
        NativeRestartActorState& state, std::string& error);
    std::uint64_t Revision() const noexcept { return m_Revision; }

private:
    NativeRestarts& m_Restarts;
    std::uint64_t m_Revision = 0;
};
