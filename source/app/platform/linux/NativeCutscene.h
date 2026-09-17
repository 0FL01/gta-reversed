#pragma once

#include "app/platform/linux/NativeScriptSession.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct NativeCutscenePayload {
    std::array<char, 8> Name{};
    std::shared_ptr<const std::vector<std::uint8_t>> Ifp;
    std::shared_ptr<const std::vector<std::uint8_t>> Cut;
    std::shared_ptr<const std::vector<std::uint8_t>> Dat;
    std::array<std::uint64_t, 3> Hashes{};
    float DurationSeconds = 0.0f;
    std::uint64_t Revision = 0;
};

class NativeCutscene {
public:
    static constexpr bool RuntimePresentation = false;

    bool Initialize(const char* gameDir, std::string& error);
    NativeScriptServiceResult Load(const std::array<char, 8>& name);
    NativeScriptServiceResult Start();
    NativeScriptServiceResult Unload();
    void AdvanceTime(std::uint32_t nowMs);
    bool Loaded() const { return static_cast<bool>(m_Current); }
    bool Started() const { return m_Started; }
    bool Finished() const { return m_Started && m_LastTimeMs - m_StartTimeMs >= m_DurationMs; }
    const std::shared_ptr<const NativeCutscenePayload>& Current() const { return m_Current; }
    std::uint64_t Revision() const { return m_Revision; }

private:
    std::string m_GameDir;
    std::shared_ptr<const NativeCutscenePayload> m_Current;
    std::uint64_t m_Revision = 0;
    bool m_Started = false;
    std::uint32_t m_LastTimeMs = 0;
    std::uint32_t m_StartTimeMs = 0;
    std::uint32_t m_DurationMs = 0;
};
