#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

enum class NativeRadioMode : std::uint8_t { Stopped, Playing, Interrupted };
enum class NativeRadioStatus : std::uint8_t {
    Ok,
    InvalidInput,
    InvalidState,
    Overflow,
    EnvelopeRejected,
};

struct NativeRadioStationDefinition {
    std::uint8_t Station = 0;
    std::uint8_t MusicCount = 0;
    std::array<std::int32_t, 31> Music{};
};

struct NativeRadioState {
    std::uint64_t Revision = 0;
    NativeRadioMode Mode = NativeRadioMode::Stopped;
    std::int32_t Station = -1;
    std::array<std::int32_t, 5> Queue{-1, -1, -1, -1, -1};
    std::int32_t CurrentTrack = -1;
    std::int32_t PreviousTrack = -1;
    std::uint32_t PlayTimeMs = 0;
    std::int32_t InterruptEvent = -1;
    std::int32_t SavedStation = -1;
    std::int32_t SavedTrack = -1;
    std::uint32_t SavedPlayTimeMs = 0;
    std::array<std::uint32_t, 12> ListenTimeMs{};
    std::array<std::array<std::int32_t, 20>, 12> MusicHistory{};
    bool operator==(const NativeRadioState&) const = default;
};

class NativeRadioRuntime {
public:
    NativeRadioRuntime();

    NativeRadioStatus Start(std::int32_t station, std::uint32_t draw, std::string& error);
    NativeRadioStatus Advance(std::uint32_t elapsedMs, std::uint32_t trackLengthMs, std::string& error);
    NativeRadioStatus Interrupt(std::int32_t event, std::string& error);
    NativeRadioStatus Resume(std::string& error);
    NativeRadioStatus Retune(std::int32_t station, std::uint32_t draw, std::string& error);
    NativeRadioStatus Stop(std::string& error);
    bool Save(std::vector<std::uint8_t>& out, std::string& error) const;
    NativeRadioStatus Restore(std::span<const std::uint8_t> bytes, std::string& error);

    const NativeRadioState& State() const noexcept { return m_State; }
    static const std::array<NativeRadioStationDefinition, 12>& Stations() noexcept;

private:
    bool AdvanceRevision(NativeRadioState& candidate, std::string& error) const;
    static void FillQueue(NativeRadioState& state, std::int32_t station, std::uint32_t draw);

    NativeRadioState m_State;
};
