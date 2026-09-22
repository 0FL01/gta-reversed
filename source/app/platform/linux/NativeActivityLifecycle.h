#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

enum class NativeActivityCategory : std::uint8_t { Service, Race, School, Minigame, Other };
enum class NativeActivityFamily : std::uint8_t {
    Taxi, Vigilante, Paramedic, Firefighter, Courier, Trucking, Valet,
    Race, DrivingSchool, BikeSchool, BoatSchool, FlyingSchool,
    Pool, Casino, Arcade, Gym, Dance, Count,
};
enum class NativeActivityPhase : std::uint8_t { Idle, Running, Passed, Failed, Cleaned };
enum class NativeActivityStatus : std::uint8_t { Ok, InvalidInput, InvalidPhase, Overflow };
enum class NativeActivityEventKind : std::uint8_t { Start, Pass, Fail, Cleanup };

struct NativeActivityDefinition {
    NativeActivityFamily Family = NativeActivityFamily::Taxi;
    NativeActivityCategory Category = NativeActivityCategory::Service;
    std::array<char, 20> Name{};
    std::int32_t StreamedScript = -1;
    bool operator==(const NativeActivityDefinition&) const = default;
};
struct NativeActivityStats {
    std::uint32_t Starts = 0, Passes = 0, Failures = 0, Cleanups = 0;
    std::int32_t BestScore = 0;
    std::uint32_t BestTimeMs = 0;
    bool operator==(const NativeActivityStats&) const = default;
};
struct NativeActivityEvent {
    std::uint64_t Sequence = 0;
    NativeActivityEventKind Kind = NativeActivityEventKind::Start;
    NativeActivityFamily Family = NativeActivityFamily::Taxi;
    std::uint32_t Attempt = 0, TimeMs = 0;
    std::int32_t Score = 0;
    bool operator==(const NativeActivityEvent&) const = default;
};
struct NativeActivitySnapshot {
    std::uint64_t Generation = 0;
    NativeActivityPhase Phase = NativeActivityPhase::Idle;
    NativeActivityFamily ActiveFamily = NativeActivityFamily::Count;
    std::uint32_t Attempt = 0, StartTimeMs = 0, LastTimeMs = 0;
    std::array<NativeActivityStats, std::size_t(NativeActivityFamily::Count)> Stats{};
    std::vector<NativeActivityEvent> Events;
    bool operator==(const NativeActivitySnapshot&) const = default;
};

// Lifecycle owner for the discovered activity families. Individual script/task
// rules produce the result; this owner guarantees start/result/cleanup ordering.
class NativeActivityLifecycle {
public:
    NativeActivityLifecycle();
    NativeActivityStatus Start(NativeActivityFamily, std::uint32_t nowMs, std::string& error);
    NativeActivityStatus Finish(bool passed, std::int32_t score, std::uint32_t nowMs, std::string& error);
    NativeActivityStatus Cleanup(std::uint32_t nowMs, std::string& error);
    const std::shared_ptr<const NativeActivitySnapshot>& LastCommitted() const noexcept { return m_Published; }
    static const std::array<NativeActivityDefinition, std::size_t(NativeActivityFamily::Count)>& Definitions() noexcept;
    static constexpr bool GameplayRulesComplete = false;

private:
    NativeActivityStatus Publish(NativeActivitySnapshot, NativeActivityEventKind,
        NativeActivityFamily, std::uint32_t, std::int32_t, std::string&);
    std::shared_ptr<const NativeActivitySnapshot> m_Published;
    std::uint64_t m_NextEvent = 0;
};
