#include "app/platform/linux/NativeRadioRuntime.h"

#include <algorithm>
#include <cstring>
#include <limits>

using int32 = std::int32_t;
#include "game_sa/Audio/Config/RadioStreamsPC.h"

namespace {
constexpr std::array<std::uint8_t, 8> kMagic{'M','A','D','S','A','R','A','D'};

void U32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) out.push_back(std::uint8_t(value >> (i * 8)));
}

void U64(std::vector<std::uint8_t>& out, std::uint64_t value) {
    for (unsigned i = 0; i < 8; ++i) out.push_back(std::uint8_t(value >> (i * 8)));
}

bool Read32(std::span<const std::uint8_t> bytes, std::size_t& at, std::uint32_t& value) {
    if (at + 4 > bytes.size()) return false;
    value = 0;
    for (unsigned i = 0; i < 4; ++i) value |= std::uint32_t(bytes[at++]) << (i * 8);
    return true;
}

bool Read64(std::span<const std::uint8_t> bytes, std::size_t& at, std::uint64_t& value) {
    if (at + 8 > bytes.size()) return false;
    value = 0;
    for (unsigned i = 0; i < 8; ++i) value |= std::uint64_t(bytes[at++]) << (i * 8);
    return true;
}

std::uint64_t Hash(std::span<const std::uint8_t> bytes) {
    std::uint64_t hash = 1469598103934665603ull;
    for (const auto value : bytes) { hash ^= value; hash *= 1099511628211ull; }
    return hash;
}
}

NativeRadioRuntime::NativeRadioRuntime() {
    for (auto& station : m_State.MusicHistory) station.fill(-1);
}

const std::array<NativeRadioStationDefinition, 12>& NativeRadioRuntime::Stations() noexcept {
    static const auto stations = [] {
        std::array<NativeRadioStationDefinition, 12> out{};
        for (std::size_t station = 0; station < out.size(); ++station) {
            out[station].Station = std::uint8_t(station);
            out[station].MusicCount = std::uint8_t(gRadioNumMusicTracksPerStation[station]);
            std::copy_n(gRadioMusicTracks[station], out[station].Music.size(), out[station].Music.begin());
        }
        return out;
    }();
    return stations;
}

bool NativeRadioRuntime::AdvanceRevision(NativeRadioState& candidate, std::string& error) const {
    if (candidate.Revision == std::numeric_limits<std::uint64_t>::max()) {
        error = "radio revision exhausted";
        return false;
    }
    ++candidate.Revision;
    error.clear();
    return true;
}

void NativeRadioRuntime::FillQueue(NativeRadioState& state, std::int32_t station, std::uint32_t draw) {
    const auto& definition = Stations()[std::size_t(station)];
    for (std::size_t i = 0; i < state.Queue.size(); ++i)
        state.Queue[i] = definition.Music[(draw + i) % definition.MusicCount];
    state.CurrentTrack = state.Queue[0];
    state.PreviousTrack = -1;
    state.PlayTimeMs = 0;
}

NativeRadioStatus NativeRadioRuntime::Start(std::int32_t station, std::uint32_t draw,
    std::string& error) {
    if (station < 0 || std::size_t(station) >= Stations().size()) {
        error = "radio station rejected";
        return NativeRadioStatus::InvalidInput;
    }
    auto candidate = m_State;
    candidate.Mode = NativeRadioMode::Playing;
    candidate.Station = station;
    candidate.InterruptEvent = -1;
    FillQueue(candidate, station, draw);
    if (!AdvanceRevision(candidate, error)) return NativeRadioStatus::Overflow;
    m_State = candidate;
    return NativeRadioStatus::Ok;
}

NativeRadioStatus NativeRadioRuntime::Advance(std::uint32_t elapsedMs,
    std::uint32_t trackLengthMs, std::string& error) {
    if (m_State.Mode != NativeRadioMode::Playing || !trackLengthMs) {
        error = "radio advance state rejected";
        return NativeRadioStatus::InvalidState;
    }
    auto candidate = m_State;
    if (elapsedMs > std::numeric_limits<std::uint32_t>::max() - candidate.PlayTimeMs) {
        error = "radio play time overflow";
        return NativeRadioStatus::Overflow;
    }
    candidate.PlayTimeMs += elapsedMs;
    if (candidate.ListenTimeMs[std::size_t(candidate.Station)] >
        std::numeric_limits<std::uint32_t>::max() - elapsedMs) {
        error = "radio listen time overflow";
        return NativeRadioStatus::Overflow;
    }
    candidate.ListenTimeMs[std::size_t(candidate.Station)] += elapsedMs;
    while (candidate.PlayTimeMs >= trackLengthMs) {
        candidate.PlayTimeMs -= trackLengthMs;
        candidate.PreviousTrack = candidate.CurrentTrack;
        std::rotate(candidate.Queue.begin(), candidate.Queue.begin() + 1, candidate.Queue.end());
        const auto& definition = Stations()[std::size_t(candidate.Station)];
        auto previous = std::ranges::find(definition.Music, candidate.Queue[candidate.Queue.size() - 2]);
        const auto index = previous == definition.Music.end() ? 0u :
            (std::size_t(previous - definition.Music.begin()) + 1u) % definition.MusicCount;
        candidate.Queue.back() = definition.Music[index];
        candidate.CurrentTrack = candidate.Queue.front();
        auto& history = candidate.MusicHistory[std::size_t(candidate.Station)];
        std::rotate(history.rbegin(), history.rbegin() + 1, history.rend());
        history.front() = candidate.PreviousTrack;
    }
    if (!AdvanceRevision(candidate, error)) return NativeRadioStatus::Overflow;
    m_State = candidate;
    return NativeRadioStatus::Ok;
}

NativeRadioStatus NativeRadioRuntime::Interrupt(std::int32_t event, std::string& error) {
    if (m_State.Mode != NativeRadioMode::Playing || event < 0) {
        error = "radio interrupt rejected";
        return NativeRadioStatus::InvalidState;
    }
    auto candidate = m_State;
    candidate.SavedStation = candidate.Station;
    candidate.SavedTrack = candidate.CurrentTrack;
    candidate.SavedPlayTimeMs = candidate.PlayTimeMs;
    candidate.InterruptEvent = event;
    candidate.Mode = NativeRadioMode::Interrupted;
    if (!AdvanceRevision(candidate, error)) return NativeRadioStatus::Overflow;
    m_State = candidate;
    return NativeRadioStatus::Ok;
}

NativeRadioStatus NativeRadioRuntime::Resume(std::string& error) {
    if (m_State.Mode != NativeRadioMode::Interrupted || m_State.SavedStation < 0) {
        error = "radio resume rejected";
        return NativeRadioStatus::InvalidState;
    }
    auto candidate = m_State;
    candidate.Station = candidate.SavedStation;
    candidate.CurrentTrack = candidate.SavedTrack;
    candidate.PlayTimeMs = candidate.SavedPlayTimeMs;
    candidate.InterruptEvent = -1;
    candidate.Mode = NativeRadioMode::Playing;
    if (!AdvanceRevision(candidate, error)) return NativeRadioStatus::Overflow;
    m_State = candidate;
    return NativeRadioStatus::Ok;
}

NativeRadioStatus NativeRadioRuntime::Retune(std::int32_t station, std::uint32_t draw,
    std::string& error) {
    return Start(station, draw, error);
}

NativeRadioStatus NativeRadioRuntime::Stop(std::string& error) {
    auto candidate = m_State;
    candidate.Mode = NativeRadioMode::Stopped;
    candidate.Station = candidate.CurrentTrack = candidate.PreviousTrack = -1;
    candidate.Queue.fill(-1);
    candidate.PlayTimeMs = 0;
    candidate.InterruptEvent = -1;
    if (!AdvanceRevision(candidate, error)) return NativeRadioStatus::Overflow;
    m_State = candidate;
    return NativeRadioStatus::Ok;
}

bool NativeRadioRuntime::Save(std::vector<std::uint8_t>& out, std::string& error) const {
    std::vector<std::uint8_t> candidate(kMagic.begin(), kMagic.end());
    U32(candidate, 1);
    U64(candidate, m_State.Revision);
    candidate.push_back(std::uint8_t(m_State.Mode));
    U32(candidate, std::uint32_t(m_State.Station));
    for (const auto value : m_State.Queue) U32(candidate, std::uint32_t(value));
    U32(candidate, std::uint32_t(m_State.CurrentTrack));
    U32(candidate, std::uint32_t(m_State.PreviousTrack));
    U32(candidate, m_State.PlayTimeMs);
    U32(candidate, std::uint32_t(m_State.InterruptEvent));
    U32(candidate, std::uint32_t(m_State.SavedStation));
    U32(candidate, std::uint32_t(m_State.SavedTrack));
    U32(candidate, m_State.SavedPlayTimeMs);
    for (const auto value : m_State.ListenTimeMs) U32(candidate, value);
    for (const auto& station : m_State.MusicHistory)
        for (const auto value : station) U32(candidate, std::uint32_t(value));
    U64(candidate, Hash(candidate));
    out = std::move(candidate);
    error.clear();
    return true;
}

NativeRadioStatus NativeRadioRuntime::Restore(std::span<const std::uint8_t> bytes,
    std::string& error) {
    constexpr std::size_t expected = 8 + 4 + 8 + 1 + 4 + 5 * 4 + 2 * 4 + 4 +
        3 * 4 + 4 + 12 * 4 + 12 * 20 * 4 + 8;
    if (bytes.size() != expected || !std::equal(kMagic.begin(), kMagic.end(), bytes.begin()) ||
        Hash(bytes.first(bytes.size() - 8)) != [&] {
            std::size_t at = bytes.size() - 8; std::uint64_t value = 0; Read64(bytes, at, value); return value;
        }()) {
        error = "radio envelope identity rejected";
        return NativeRadioStatus::EnvelopeRejected;
    }
    std::size_t at = 8;
    std::uint32_t version = 0;
    std::uint64_t revision = 0;
    if (!Read32(bytes, at, version) || version != 1 || !Read64(bytes, at, revision) || at >= bytes.size()) {
        error = "radio envelope header rejected";
        return NativeRadioStatus::EnvelopeRejected;
    }
    NativeRadioState candidate;
    candidate.Revision = revision;
    candidate.Mode = NativeRadioMode(bytes[at++]);
    auto readInt = [&](std::int32_t& value) { std::uint32_t raw = 0; if (!Read32(bytes, at, raw)) return false; value = std::int32_t(raw); return true; };
    if (std::uint8_t(candidate.Mode) > std::uint8_t(NativeRadioMode::Interrupted) ||
        !readInt(candidate.Station)) return NativeRadioStatus::EnvelopeRejected;
    for (auto& value : candidate.Queue) if (!readInt(value)) return NativeRadioStatus::EnvelopeRejected;
    if (!readInt(candidate.CurrentTrack) || !readInt(candidate.PreviousTrack) ||
        !Read32(bytes, at, candidate.PlayTimeMs) || !readInt(candidate.InterruptEvent) ||
        !readInt(candidate.SavedStation) || !readInt(candidate.SavedTrack) ||
        !Read32(bytes, at, candidate.SavedPlayTimeMs)) return NativeRadioStatus::EnvelopeRejected;
    for (auto& value : candidate.ListenTimeMs) if (!Read32(bytes, at, value)) return NativeRadioStatus::EnvelopeRejected;
    for (auto& station : candidate.MusicHistory)
        for (auto& value : station) if (!readInt(value)) return NativeRadioStatus::EnvelopeRejected;
    if ((candidate.Station < -1 || candidate.Station >= 12) ||
        (candidate.SavedStation < -1 || candidate.SavedStation >= 12)) {
        error = "radio envelope state rejected";
        return NativeRadioStatus::EnvelopeRejected;
    }
    m_State = candidate;
    error.clear();
    return NativeRadioStatus::Ok;
}
