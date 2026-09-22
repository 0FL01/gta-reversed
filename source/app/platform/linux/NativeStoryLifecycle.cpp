#include "NativeStoryLifecycle.h"

#include <limits>

namespace {
bool AddWouldOverflow(std::uint32_t a, std::uint32_t b) {
    return b > std::numeric_limits<std::uint32_t>::max() - a;
}
}

NativeStoryStatus NativeStoryLifecycle::OwnerFailure(const NativeScriptServiceResult& result,
    const char* operation, std::string& error) {
    error = std::string(operation) + ": " + result.Message;
    return NativeStoryStatus::OwnerError;
}

NativeStoryStatus NativeStoryLifecycle::Publish(NativeStorySnapshot next,
    NativeStoryEventKind kind, std::uint32_t nowMs, std::string& error) {
    if (m_NextEvent == std::numeric_limits<std::uint64_t>::max() ||
        next.Generation == std::numeric_limits<std::uint64_t>::max()) {
        error = "story lifecycle sequence exhausted";
        return NativeStoryStatus::Overflow;
    }
    next.Events.push_back({m_NextEvent + 1, kind, nowMs, next.Attempt});
    ++next.Generation;
    next.TimeMs = nowMs;
    auto published = std::make_shared<const NativeStorySnapshot>(std::move(next));
    ++m_NextEvent;
    m_Published = std::move(published);
    error.clear();
    return NativeStoryStatus::Ok;
}

NativeStoryStatus NativeStoryLifecycle::Initialize(const char* gameDir, std::int32_t mission,
    const std::array<char, 8>& cutscene, std::int32_t audioEvent, std::string& error) {
    if (!gameDir || !*gameDir || mission < 0 || audioEvent < 42000 || audioEvent >= 43922) {
        error = "story lifecycle initialization is invalid";
        return NativeStoryStatus::InvalidInput;
    }
    NativeCutscene cutsceneOwner;
    NativeMissionAudio audioOwner;
    if (!cutsceneOwner.Initialize(gameDir, error) || !audioOwner.LoadBeforeWorker(gameDir, error))
        return NativeStoryStatus::OwnerError;
    auto initial = std::make_shared<NativeStorySnapshot>();
    initial->Generation = 1;
    initial->Mission = mission;
    initial->Phase = NativeStoryPhase::Available;
    m_Cutscene = std::move(cutsceneOwner);
    m_Audio = std::move(audioOwner);
    m_CutsceneName = cutscene;
    m_AudioEvent = audioEvent;
    m_Published = std::move(initial);
    m_NextEvent = 0;
    m_Loaded = true;
    error.clear();
    return NativeStoryStatus::Ok;
}

NativeStoryStatus NativeStoryLifecycle::Start(std::uint32_t nowMs, std::string& error) {
    if (!m_Loaded) { error = "story lifecycle is not loaded"; return NativeStoryStatus::NotLoaded; }
    if (m_Published->Phase != NativeStoryPhase::Available) {
        error = "story start phase is invalid"; return NativeStoryStatus::InvalidPhase;
    }
    auto next = *m_Published;
    if (next.Attempt == std::numeric_limits<std::uint32_t>::max()) { error = "story attempt exhausted"; return NativeStoryStatus::Overflow; }
    ++next.Attempt;
    next.Phase = NativeStoryPhase::Running;
    return Publish(std::move(next), NativeStoryEventKind::Start, nowMs, error);
}

NativeStoryStatus NativeStoryLifecycle::StartCutscene(std::uint32_t nowMs, std::string& error) {
    if (!m_Loaded) { error = "story lifecycle is not loaded"; return NativeStoryStatus::NotLoaded; }
    if (m_Published->Phase != NativeStoryPhase::Running) { error = "cutscene start phase is invalid"; return NativeStoryStatus::InvalidPhase; }
    auto loaded = m_Cutscene.Load(m_CutsceneName);
    if (loaded.Status != NativeScriptServiceStatus::Ready) return OwnerFailure(loaded, "cutscene load", error);
    m_Cutscene.AdvanceTime(nowMs);
    auto started = m_Cutscene.Start();
    if (started.Status != NativeScriptServiceStatus::Ready) return OwnerFailure(started, "cutscene start", error);
    auto next = *m_Published;
    next.Phase = NativeStoryPhase::Cutscene;
    next.CutsceneLoaded = next.CutsceneStarted = true;
    return Publish(std::move(next), NativeStoryEventKind::CutsceneStart, nowMs, error);
}

NativeStoryStatus NativeStoryLifecycle::ResolveCutscene(std::uint32_t nowMs, bool skipped, std::string& error) {
    if (!m_Loaded || m_Published->Phase != NativeStoryPhase::Cutscene) {
        error = "cutscene resolution phase is invalid"; return m_Loaded ? NativeStoryStatus::InvalidPhase : NativeStoryStatus::NotLoaded;
    }
    m_Cutscene.AdvanceTime(nowMs);
    if (!skipped && !m_Cutscene.Finished()) { error = "source cutscene is not finished"; return NativeStoryStatus::InvalidPhase; }
    const auto unloaded = m_Cutscene.Unload();
    if (unloaded.Status != NativeScriptServiceStatus::Ready) return OwnerFailure(unloaded, "cutscene unload", error);
    auto next = *m_Published;
    next.Phase = NativeStoryPhase::Running;
    next.CutsceneLoaded = next.CutsceneStarted = false;
    if (skipped) ++next.Skips;
    return Publish(std::move(next), skipped ? NativeStoryEventKind::CutsceneSkip : NativeStoryEventKind::CutsceneFinish, nowMs, error);
}

NativeStoryStatus NativeStoryLifecycle::RegisterResources(std::uint32_t peds, std::uint32_t vehicles,
    std::uint32_t trains, std::uint32_t objects, std::string& error) {
    if (!m_Loaded || m_Published->Phase != NativeStoryPhase::Running) {
        error = "story resource phase is invalid"; return m_Loaded ? NativeStoryStatus::InvalidPhase : NativeStoryStatus::NotLoaded;
    }
    auto next = *m_Published;
    if (AddWouldOverflow(next.Peds, peds) || AddWouldOverflow(next.Vehicles, vehicles) ||
        AddWouldOverflow(next.Trains, trains) || AddWouldOverflow(next.Objects, objects)) {
        error = "story resource count overflow"; return NativeStoryStatus::Overflow;
    }
    next.Peds += peds; next.Vehicles += vehicles; next.Trains += trains; next.Objects += objects;
    ++next.Generation;
    m_Published = std::make_shared<const NativeStorySnapshot>(std::move(next));
    error.clear();
    return NativeStoryStatus::Ok;
}

NativeStoryStatus NativeStoryLifecycle::StartAudio(std::uint32_t nowMs, std::string& error) {
    if (!m_Loaded || m_Published->Phase != NativeStoryPhase::Running) {
        error = "story audio phase is invalid"; return m_Loaded ? NativeStoryStatus::InvalidPhase : NativeStoryStatus::NotLoaded;
    }
    auto requested = m_Audio.Request(1, m_AudioEvent);
    if (requested.Status != NativeScriptServiceStatus::Ready) return OwnerFailure(requested, "mission audio load", error);
    auto played = m_Audio.Play(1, nowMs);
    if (played.Status != NativeScriptServiceStatus::Ready) return OwnerFailure(played, "mission audio play", error);
    auto next = *m_Published;
    next.AudioLoaded = true;
    next.AudioFinished = false;
    return Publish(std::move(next), NativeStoryEventKind::AudioStart, nowMs, error);
}

NativeStoryStatus NativeStoryLifecycle::Advance(std::uint32_t nowMs, std::string& error) {
    if (!m_Loaded || nowMs < m_Published->TimeMs) { error = "story time is invalid"; return m_Loaded ? NativeStoryStatus::InvalidInput : NativeStoryStatus::NotLoaded; }
    m_Cutscene.AdvanceTime(nowMs);
    m_Audio.Advance(nowMs);
    auto next = *m_Published;
    next.TimeMs = nowMs;
    if (next.AudioLoaded) next.AudioFinished = m_Audio.Finished(1);
    ++next.Generation;
    m_Published = std::make_shared<const NativeStorySnapshot>(std::move(next));
    error.clear();
    return NativeStoryStatus::Ok;
}

NativeStoryStatus NativeStoryLifecycle::Fail(std::uint32_t nowMs, std::string& error) {
    if (!m_Loaded || (m_Published->Phase != NativeStoryPhase::Running && m_Published->Phase != NativeStoryPhase::Cutscene)) {
        error = "story failure phase is invalid"; return m_Loaded ? NativeStoryStatus::InvalidPhase : NativeStoryStatus::NotLoaded;
    }
    if (m_Cutscene.Loaded()) m_Cutscene.Unload();
    m_Audio.Clear(1);
    auto next = *m_Published;
    next.Phase = NativeStoryPhase::Failed;
    next.CutsceneLoaded = next.CutsceneStarted = next.AudioLoaded = next.AudioFinished = false;
    ++next.Failures;
    return Publish(std::move(next), NativeStoryEventKind::Fail, nowMs, error);
}

NativeStoryStatus NativeStoryLifecycle::Retry(std::uint32_t nowMs, std::string& error) {
    if (!m_Loaded || m_Published->Phase != NativeStoryPhase::RetryAvailable) {
        error = "story retry phase is invalid"; return m_Loaded ? NativeStoryStatus::InvalidPhase : NativeStoryStatus::NotLoaded;
    }
    auto next = *m_Published;
    ++next.Retries;
    ++next.Attempt;
    next.Phase = NativeStoryPhase::Running;
    return Publish(std::move(next), NativeStoryEventKind::Retry, nowMs, error);
}

NativeStoryStatus NativeStoryLifecycle::Complete(std::uint32_t nowMs, std::string& error) {
    if (!m_Loaded || m_Published->Phase != NativeStoryPhase::Running ||
        (m_Published->AudioLoaded && !m_Published->AudioFinished)) {
        error = "story completion phase is invalid"; return m_Loaded ? NativeStoryStatus::InvalidPhase : NativeStoryStatus::NotLoaded;
    }
    auto next = *m_Published;
    next.Phase = NativeStoryPhase::Completed;
    ++next.Completions;
    return Publish(std::move(next), NativeStoryEventKind::Complete, nowMs, error);
}

NativeStoryStatus NativeStoryLifecycle::Cleanup(std::uint32_t nowMs, std::string& error) {
    if (!m_Loaded || (m_Published->Phase != NativeStoryPhase::Failed && m_Published->Phase != NativeStoryPhase::Completed)) {
        error = "story cleanup phase is invalid"; return m_Loaded ? NativeStoryStatus::InvalidPhase : NativeStoryStatus::NotLoaded;
    }
    m_Audio.Clear(1);
    if (m_Cutscene.Loaded()) m_Cutscene.Unload();
    auto next = *m_Published;
    const bool retry = next.Phase == NativeStoryPhase::Failed;
    next.Phase = retry ? NativeStoryPhase::RetryAvailable : NativeStoryPhase::Cleaned;
    next.Peds = next.Vehicles = next.Trains = next.Objects = 0;
    next.CutsceneLoaded = next.CutsceneStarted = next.AudioLoaded = next.AudioFinished = false;
    ++next.Cleanups;
    return Publish(std::move(next), NativeStoryEventKind::Cleanup, nowMs, error);
}
