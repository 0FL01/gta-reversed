#include "NativeActivityLifecycle.h"

#include <algorithm>
#include <limits>

namespace {
constexpr std::array<char, 20> Name(const char* text) {
    std::array<char, 20> out{};
    for (std::size_t i = 0; i < out.size() && text[i]; ++i) out[i] = text[i];
    return out;
}
constexpr std::array<NativeActivityDefinition, std::size_t(NativeActivityFamily::Count)> kDefinitions{{
    {NativeActivityFamily::Taxi, NativeActivityCategory::Service, Name("TAXI"), -1},
    {NativeActivityFamily::Vigilante, NativeActivityCategory::Service, Name("VIGILANTE"), -1},
    {NativeActivityFamily::Paramedic, NativeActivityCategory::Service, Name("PARAMEDIC"), -1},
    {NativeActivityFamily::Firefighter, NativeActivityCategory::Service, Name("FIREFIGHTER"), -1},
    {NativeActivityFamily::Courier, NativeActivityCategory::Service, Name("COURIER"), -1},
    {NativeActivityFamily::Trucking, NativeActivityCategory::Service, Name("TRUCKING"), -1},
    {NativeActivityFamily::Valet, NativeActivityCategory::Service, Name("VALET"), 72},
    {NativeActivityFamily::Race, NativeActivityCategory::Race, Name("RACE"), -1},
    {NativeActivityFamily::DrivingSchool, NativeActivityCategory::School, Name("DRIVING_SCHOOL"), -1},
    {NativeActivityFamily::BikeSchool, NativeActivityCategory::School, Name("BIKE_SCHOOL"), -1},
    {NativeActivityFamily::BoatSchool, NativeActivityCategory::School, Name("BOAT_SCHOOL"), -1},
    {NativeActivityFamily::FlyingSchool, NativeActivityCategory::School, Name("FLYING_SCHOOL"), -1},
    {NativeActivityFamily::Pool, NativeActivityCategory::Minigame, Name("POOL_SCRIPT"), 21},
    {NativeActivityFamily::Casino, NativeActivityCategory::Minigame, Name("SLOT_MACHINE"), 4},
    {NativeActivityFamily::Arcade, NativeActivityCategory::Minigame, Name("ARCADE"), 7},
    {NativeActivityFamily::Gym, NativeActivityCategory::Other, Name("GYMBIKE"), 11},
    {NativeActivityFamily::Dance, NativeActivityCategory::Other, Name("DANCE"), 35},
}};
}

NativeActivityLifecycle::NativeActivityLifecycle() {
    auto initial = std::make_shared<NativeActivitySnapshot>();
    initial->Generation = 1;
    m_Published = std::move(initial);
}

const std::array<NativeActivityDefinition, std::size_t(NativeActivityFamily::Count)>&
NativeActivityLifecycle::Definitions() noexcept { return kDefinitions; }

NativeActivityStatus NativeActivityLifecycle::Publish(NativeActivitySnapshot next,
    NativeActivityEventKind kind, NativeActivityFamily family, std::uint32_t time,
    std::int32_t score, std::string& error) {
    if (m_NextEvent == std::numeric_limits<std::uint64_t>::max() ||
        next.Generation == std::numeric_limits<std::uint64_t>::max()) {
        error = "activity sequence exhausted";
        return NativeActivityStatus::Overflow;
    }
    next.Events.push_back({m_NextEvent + 1, kind, family, next.Attempt, time, score});
    ++next.Generation;
    next.LastTimeMs = time;
    m_Published = std::make_shared<const NativeActivitySnapshot>(std::move(next));
    ++m_NextEvent;
    error.clear();
    return NativeActivityStatus::Ok;
}

NativeActivityStatus NativeActivityLifecycle::Start(NativeActivityFamily family,
    std::uint32_t nowMs, std::string& error) {
    if (std::size_t(family) >= kDefinitions.size()) { error = "activity family is invalid"; return NativeActivityStatus::InvalidInput; }
    if (m_Published->Phase != NativeActivityPhase::Idle && m_Published->Phase != NativeActivityPhase::Cleaned) {
        error = "activity start phase is invalid"; return NativeActivityStatus::InvalidPhase;
    }
    auto next = *m_Published;
    if (next.Attempt == std::numeric_limits<std::uint32_t>::max() ||
        next.Stats[std::size_t(family)].Starts == std::numeric_limits<std::uint32_t>::max()) {
        error = "activity attempt exhausted"; return NativeActivityStatus::Overflow;
    }
    ++next.Attempt;
    ++next.Stats[std::size_t(family)].Starts;
    next.ActiveFamily = family;
    next.Phase = NativeActivityPhase::Running;
    next.StartTimeMs = nowMs;
    return Publish(std::move(next), NativeActivityEventKind::Start, family, nowMs, 0, error);
}

NativeActivityStatus NativeActivityLifecycle::Finish(bool passed, std::int32_t score,
    std::uint32_t nowMs, std::string& error) {
    if (m_Published->Phase != NativeActivityPhase::Running || nowMs < m_Published->StartTimeMs) {
        error = "activity result phase/time is invalid"; return NativeActivityStatus::InvalidPhase;
    }
    auto next = *m_Published;
    auto& stats = next.Stats[std::size_t(next.ActiveFamily)];
    auto& counter = passed ? stats.Passes : stats.Failures;
    if (counter == std::numeric_limits<std::uint32_t>::max()) { error = "activity result count exhausted"; return NativeActivityStatus::Overflow; }
    ++counter;
    if (passed) {
        stats.BestScore = std::max(stats.BestScore, score);
        const auto elapsed = nowMs - next.StartTimeMs;
        if (!stats.BestTimeMs || elapsed < stats.BestTimeMs) stats.BestTimeMs = elapsed;
    }
    next.Phase = passed ? NativeActivityPhase::Passed : NativeActivityPhase::Failed;
    return Publish(std::move(next), passed ? NativeActivityEventKind::Pass : NativeActivityEventKind::Fail,
        m_Published->ActiveFamily, nowMs, score, error);
}

NativeActivityStatus NativeActivityLifecycle::Cleanup(std::uint32_t nowMs, std::string& error) {
    if ((m_Published->Phase != NativeActivityPhase::Passed && m_Published->Phase != NativeActivityPhase::Failed) ||
        nowMs < m_Published->LastTimeMs) {
        error = "activity cleanup phase/time is invalid"; return NativeActivityStatus::InvalidPhase;
    }
    auto next = *m_Published;
    auto& count = next.Stats[std::size_t(next.ActiveFamily)].Cleanups;
    if (count == std::numeric_limits<std::uint32_t>::max()) { error = "activity cleanup count exhausted"; return NativeActivityStatus::Overflow; }
    ++count;
    const auto family = next.ActiveFamily;
    next.ActiveFamily = NativeActivityFamily::Count;
    next.Phase = NativeActivityPhase::Cleaned;
    return Publish(std::move(next), NativeActivityEventKind::Cleanup, family, nowMs, 0, error);
}
