#pragma once

#include "NativeCutscene.h"
#include "NativeMissionAudio.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

enum class NativeStoryPhase : std::uint8_t {
    Available,
    Running,
    Cutscene,
    Failed,
    RetryAvailable,
    Completed,
    Cleaned,
};
enum class NativeStoryStatus : std::uint8_t { Ok, NotLoaded, InvalidInput, InvalidPhase, OwnerError, Overflow };
enum class NativeStoryEventKind : std::uint8_t {
    Start, CutsceneStart, CutsceneSkip, CutsceneFinish, Fail, Cleanup, Retry,
    AudioStart, Complete,
};

struct NativeStoryEvent {
    std::uint64_t Sequence = 0;
    NativeStoryEventKind Kind = NativeStoryEventKind::Start;
    std::uint32_t TimeMs = 0;
    std::uint32_t Attempt = 0;
    bool operator==(const NativeStoryEvent&) const = default;
};

struct NativeStorySnapshot {
    std::uint64_t Generation = 0;
    std::int32_t Mission = -1;
    NativeStoryPhase Phase = NativeStoryPhase::Available;
    std::uint32_t Attempt = 0, TimeMs = 0, Failures = 0, Retries = 0, Skips = 0, Completions = 0, Cleanups = 0;
    std::uint32_t Peds = 0, Vehicles = 0, Trains = 0, Objects = 0;
    bool CutsceneLoaded = false, CutsceneStarted = false, AudioLoaded = false, AudioFinished = false;
    std::vector<NativeStoryEvent> Events;
    bool operator==(const NativeStorySnapshot&) const = default;
};

// Representative source story owner. It composes the existing real cutscene
// archive and mission-audio metadata owners, while presentation remains external.
class NativeStoryLifecycle {
public:
    NativeStoryStatus Initialize(const char* gameDir, std::int32_t mission,
        const std::array<char, 8>& cutscene, std::int32_t audioEvent, std::string& error);
    NativeStoryStatus Start(std::uint32_t nowMs, std::string& error);
    NativeStoryStatus StartCutscene(std::uint32_t nowMs, std::string& error);
    NativeStoryStatus ResolveCutscene(std::uint32_t nowMs, bool skipped, std::string& error);
    NativeStoryStatus RegisterResources(std::uint32_t peds, std::uint32_t vehicles,
        std::uint32_t trains, std::uint32_t objects, std::string& error);
    NativeStoryStatus StartAudio(std::uint32_t nowMs, std::string& error);
    NativeStoryStatus Advance(std::uint32_t nowMs, std::string& error);
    NativeStoryStatus Fail(std::uint32_t nowMs, std::string& error);
    NativeStoryStatus Retry(std::uint32_t nowMs, std::string& error);
    NativeStoryStatus Complete(std::uint32_t nowMs, std::string& error);
    NativeStoryStatus Cleanup(std::uint32_t nowMs, std::string& error);
    const std::shared_ptr<const NativeStorySnapshot>& LastCommitted() const noexcept { return m_Published; }
    static constexpr bool PresentationFeedback = false;

private:
    NativeStoryStatus Publish(NativeStorySnapshot next, NativeStoryEventKind, std::uint32_t, std::string&);
    NativeStoryStatus OwnerFailure(const NativeScriptServiceResult&, const char*, std::string&);

    NativeCutscene m_Cutscene;
    NativeMissionAudio m_Audio;
    std::array<char, 8> m_CutsceneName{};
    std::int32_t m_AudioEvent = -1;
    std::shared_ptr<const NativeStorySnapshot> m_Published;
    std::uint64_t m_NextEvent = 0;
    bool m_Loaded = false;
};
