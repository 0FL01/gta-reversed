#include "app/platform/linux/NativeScriptPortableSave.h"

#include "app/platform/linux/NativeScriptSchema.h"
#include "app/platform/linux/NativeScriptSession.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>

namespace {
constexpr std::array<std::uint8_t, 8> kMagic{'M', 'A', 'D', 'S', 'A', 'P', 'S', 'V'};
constexpr std::uint32_t kEndian = 0x01020304;
constexpr std::uint32_t kHeaderBytes = 80;
constexpr std::uint32_t kOwnerScriptSession = 0x314D4353; // "SCM1" in LE bytes
constexpr std::uint32_t kBodyVersion = 1;
constexpr std::size_t kMaxEnvelopeBytes = 16 * 1024 * 1024;
constexpr std::uint64_t kFnvOffset = 14695981039346656037ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;
static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559,
    "portable script envelope requires IEEE-754 binary32");

class Writer {
public:
    void U8(std::uint8_t value) { Bytes.push_back(value); }
    void U16(std::uint16_t value) { Put(value, 2); }
    void U32(std::uint32_t value) { Put(value, 4); }
    void U64(std::uint64_t value) { Put(value, 8); }
    void I32(std::int32_t value) { U32(std::bit_cast<std::uint32_t>(value)); }
    void Float(float value) { U32(std::bit_cast<std::uint32_t>(value)); }
    void Bool(bool value) { U8(value ? 1 : 0); }
    void Raw(std::span<const std::uint8_t> values) { Bytes.insert(Bytes.end(), values.begin(), values.end()); }
    void SetU32(std::size_t at, std::uint32_t value) {
        for (unsigned i = 0; i < 4; ++i) Bytes.at(at + i) = std::uint8_t(value >> (i * 8));
    }
    void SetU64(std::size_t at, std::uint64_t value) {
        for (unsigned i = 0; i < 8; ++i) Bytes.at(at + i) = std::uint8_t(value >> (i * 8));
    }

    std::vector<std::uint8_t> Bytes;

private:
    void Put(std::uint64_t value, unsigned count) {
        for (unsigned i = 0; i < count; ++i) Bytes.push_back(std::uint8_t(value >> (i * 8)));
    }
};

class Reader {
public:
    explicit Reader(std::span<const std::uint8_t> bytes) : m_Bytes(bytes) {}
    bool U8(std::uint8_t& value) { std::uint64_t bits = 0; if (!Get(bits, 1)) return false; value = std::uint8_t(bits); return true; }
    bool U16(std::uint16_t& value) { std::uint64_t bits = 0; if (!Get(bits, 2)) return false; value = std::uint16_t(bits); return true; }
    bool U32(std::uint32_t& value) { std::uint64_t bits = 0; if (!Get(bits, 4)) return false; value = std::uint32_t(bits); return true; }
    bool U64(std::uint64_t& value) { return Get(value, 8); }
    bool I32(std::int32_t& value) { std::uint32_t bits = 0; if (!U32(bits)) return false; value = std::bit_cast<std::int32_t>(bits); return true; }
    bool Float(float& value) { std::uint32_t bits = 0; if (!U32(bits)) return false; value = std::bit_cast<float>(bits); return true; }
    bool Bool(bool& value) { std::uint8_t raw = 0; if (!U8(raw) || raw > 1) return false; value = raw != 0; return true; }
    bool Raw(std::span<std::uint8_t> values) {
        if (values.size() > m_Bytes.size() - m_Pos) return false;
        std::copy_n(m_Bytes.begin() + m_Pos, values.size(), values.begin());
        m_Pos += values.size();
        return true;
    }
    bool Done() const { return m_Pos == m_Bytes.size(); }

private:
    bool Get(std::uint64_t& value, unsigned count) {
        if (count > m_Bytes.size() - m_Pos) return false;
        value = 0;
        for (unsigned i = 0; i < count; ++i) value |= std::uint64_t(m_Bytes[m_Pos++]) << (i * 8);
        return true;
    }

    std::span<const std::uint8_t> m_Bytes;
    std::size_t m_Pos = 0;
};

std::uint64_t Hash(std::span<const std::uint8_t> bytes, std::uint64_t value = kFnvOffset) {
    for (const auto byte : bytes) { value ^= byte; value *= kFnvPrime; }
    return value;
}

void HashWord(std::uint64_t& hash, std::uint64_t value, unsigned count) {
    for (unsigned i = 0; i < count; ++i) {
        hash ^= std::uint8_t(value >> (i * 8));
        hash *= kFnvPrime;
    }
}

std::uint64_t SchemaFingerprint() {
    std::uint64_t hash = kFnvOffset;
    const auto add = [&](std::string_view text) {
        hash = Hash({reinterpret_cast<const std::uint8_t*>(text.data()), text.size()}, hash);
        HashWord(hash, 0, 1);
    };
    add(NativeScriptSchemaRevision());
    add(NativeScriptSchemaSha256());
    add(NativeScriptSchemaVersion());
    for (const auto& schema : NativeScriptSchemaEntries()) {
        HashWord(hash, schema.Opcode, 2);
        HashWord(hash, schema.OperandCount, 1);
        HashWord(hash, std::uint8_t(schema.Semantics), 1);
        HashWord(hash, schema.VariadicArguments, 1);
        for (unsigned i = 0; i < schema.OperandCount; ++i) HashWord(hash, std::uint8_t(schema.Operands[i]), 1);
    }
    return hash;
}

std::uint64_t DefinitionFingerprint(const NativeScriptStreamedDefinition& definition) {
    std::uint64_t hash = Hash({reinterpret_cast<const std::uint8_t*>(definition.Name.data()), definition.Name.size()});
    HashWord(hash, definition.FileOffset, 4);
    HashWord(hash, definition.Size, 4);
    return hash;
}

void WriteEvent(Writer& writer, const NativeScriptWriteEvent& event) {
    writer.U64(event.Sequence); writer.U32(event.IP); writer.U16(event.Variable);
    writer.Bool(event.Global); writer.I32(event.Value);
}

bool ReadEvent(Reader& reader, NativeScriptWriteEvent& event) {
    return reader.U64(event.Sequence) && reader.U32(event.IP) && reader.U16(event.Variable) &&
        reader.Bool(event.Global) && reader.I32(event.Value);
}

void WriteThread(Writer& writer, const NativeScriptThreadState& thread) {
    writer.U32(thread.IP); writer.U32(thread.TimeMs); writer.U32(thread.WakeTimeMs);
    writer.Raw({reinterpret_cast<const std::uint8_t*>(thread.Name.data()), thread.Name.size()});
    writer.U32(std::uint32_t(thread.Locals.size()));
    for (const auto value : thread.Locals) writer.U32(value);
    writer.Bool(thread.DeathArrestCheckEnabled); writer.Bool(thread.Active); writer.Bool(thread.Waiting);
    writer.Bool(thread.UsesMissionCleanup); writer.Bool(thread.ThisMustBeTheOnlyMissionRunning);
    writer.Bool(thread.IsExternal); writer.U32(thread.BaseIP); writer.I32(thread.MissionIndex);
    writer.I32(thread.StreamedIndex); writer.U64(thread.StreamedGeneration);
    for (const auto value : thread.ReturnStack) writer.U32(value);
    writer.U8(thread.StackDepth); writer.Bool(thread.Condition); writer.U8(thread.AndOrState);
    writer.U64(thread.Commands); writer.U64(thread.Generation); WriteEvent(writer, thread.LastOutputWrite);
    writer.U32(thread.LastInstructionIP); writer.U16(thread.LastOpcode);
}

bool ReadThread(Reader& reader, NativeScriptThreadState& thread) {
    std::array<std::uint8_t, 8> name{};
    std::uint32_t locals = 0;
    if (!reader.U32(thread.IP) || !reader.U32(thread.TimeMs) || !reader.U32(thread.WakeTimeMs) ||
        !reader.Raw(name) || !reader.U32(locals) || (locals != 34 && locals != 1024)) return false;
    for (std::size_t i = 0; i < name.size(); ++i) thread.Name[i] = char(name[i]);
    thread.Locals.resize(locals);
    for (auto& value : thread.Locals) if (!reader.U32(value)) return false;
    if (!reader.Bool(thread.DeathArrestCheckEnabled) || !reader.Bool(thread.Active) ||
        !reader.Bool(thread.Waiting) || !reader.Bool(thread.UsesMissionCleanup) ||
        !reader.Bool(thread.ThisMustBeTheOnlyMissionRunning) || !reader.Bool(thread.IsExternal) ||
        !reader.U32(thread.BaseIP) || !reader.I32(thread.MissionIndex) ||
        !reader.I32(thread.StreamedIndex) || !reader.U64(thread.StreamedGeneration)) return false;
    for (auto& value : thread.ReturnStack) if (!reader.U32(value)) return false;
    return reader.U8(thread.StackDepth) && reader.Bool(thread.Condition) && reader.U8(thread.AndOrState) &&
        reader.U64(thread.Commands) && reader.U64(thread.Generation) && ReadEvent(reader, thread.LastOutputWrite) &&
        reader.U32(thread.LastInstructionIP) && reader.U16(thread.LastOpcode);
}

void WriteExtendedState(Writer& writer, const NativeScriptState& state) {
    for (const auto value : state.FloatStats) writer.Float(value);
    for (const auto value : state.IntStats) writer.I32(value);
    writer.I32(state.MaximumWantedLevel); writer.I32(state.MaximumChaosLevel);
    writer.U8(state.Clock.Hours); writer.U8(state.Clock.Minutes); writer.U16(state.Clock.Seconds);
    writer.U32(state.Clock.LastTickMs); writer.U64(state.Clock.Revision);
    writer.Float(state.Fade.DurationSeconds); writer.Float(state.Fade.Alpha); writer.U8(state.Fade.Direction);
    writer.Bool(state.Fade.Fading); writer.Bool(state.Fade.MusicFading); writer.Bool(state.Fade.MusicFadedOut);
    writer.Float(state.Fade.MusicDuration); writer.Float(state.Fade.MusicWait); writer.Float(state.Fade.EffectsScale);
    writer.U32(state.Fade.StartMs); writer.U32(state.Fade.MusicStartMs); writer.U64(state.Fade.Revision);
    writer.U64(state.StatWrites);
    for (const auto& categories : state.Relationships) for (const auto value : categories) writer.U32(value);
    writer.U64(state.RelationshipRevision); writer.Bool(state.AlreadyRunningMission);
    writer.U16(state.OnAMissionFlag); writer.Bool(state.LaRiotsEnabled); writer.U64(state.LaRiotsRevision);
    writer.U64(state.UnprocessedStatNotifications); writer.Bool(state.StatNotificationsImplemented);
}

bool ReadExtendedState(Reader& reader, NativeScriptState& state) {
    for (auto& value : state.FloatStats) if (!reader.Float(value)) return false;
    for (auto& value : state.IntStats) if (!reader.I32(value)) return false;
    if (!reader.I32(state.MaximumWantedLevel) || !reader.I32(state.MaximumChaosLevel) ||
        !reader.U8(state.Clock.Hours) || !reader.U8(state.Clock.Minutes) ||
        !reader.U16(state.Clock.Seconds) || !reader.U32(state.Clock.LastTickMs) ||
        !reader.U64(state.Clock.Revision) || !reader.Float(state.Fade.DurationSeconds) ||
        !reader.Float(state.Fade.Alpha) || !reader.U8(state.Fade.Direction) ||
        !reader.Bool(state.Fade.Fading) || !reader.Bool(state.Fade.MusicFading) ||
        !reader.Bool(state.Fade.MusicFadedOut) || !reader.Float(state.Fade.MusicDuration) ||
        !reader.Float(state.Fade.MusicWait) || !reader.Float(state.Fade.EffectsScale) ||
        !reader.U32(state.Fade.StartMs) || !reader.U32(state.Fade.MusicStartMs) ||
        !reader.U64(state.Fade.Revision) || !reader.U64(state.StatWrites)) return false;
    for (auto& categories : state.Relationships) for (auto& value : categories) if (!reader.U32(value)) return false;
    return reader.U64(state.RelationshipRevision) && reader.Bool(state.AlreadyRunningMission) &&
        reader.U16(state.OnAMissionFlag) && reader.Bool(state.LaRiotsEnabled) &&
        reader.U64(state.LaRiotsRevision) && reader.U64(state.UnprocessedStatNotifications) &&
        reader.Bool(state.StatNotificationsImplemented);
}

NativeScriptPortableSaveStatus Header(std::span<const std::uint8_t> envelope,
    NativeScriptPortableSaveInfo& info, std::span<const std::uint8_t>& payload, std::string& error) {
    if (envelope.size() < kHeaderBytes) { error = "portable envelope header truncated"; return NativeScriptPortableSaveStatus::Truncated; }
    if (!std::equal(kMagic.begin(), kMagic.end(), envelope.begin())) {
        error = "portable envelope magic mismatch"; return NativeScriptPortableSaveStatus::InvalidEnvelope;
    }
    Reader reader(envelope.first(kHeaderBytes));
    std::array<std::uint8_t, 8> magic{};
    std::uint32_t endian = 0, headerBytes = 0, owner = 0, reserved = 0, reserved2 = 0;
    NativeScriptPortableSaveInfo candidate;
    if (!reader.Raw(magic) || !reader.U32(endian) || !reader.U16(candidate.Major) ||
        !reader.U16(candidate.Minor) || !reader.U32(headerBytes) || !reader.U32(candidate.Bytes) ||
        !reader.U32(candidate.PayloadBytes) || !reader.U32(owner) ||
        !reader.U64(candidate.SourceFingerprint) || !reader.U64(candidate.SchemaFingerprint) ||
        !reader.U64(candidate.PayloadChecksum) || !reader.U32(candidate.Globals) ||
        !reader.U32(candidate.Threads) || !reader.U32(candidate.ActiveThreads) ||
        !reader.U32(candidate.StreamedScripts) || !reader.U32(reserved) || !reader.U32(reserved2) || !reader.Done()) {
        error = "portable envelope header malformed"; return NativeScriptPortableSaveStatus::InvalidEnvelope;
    }
    if (candidate.Major != NativeScriptPortableSaveMajor || candidate.Minor != NativeScriptPortableSaveMinor) {
        error = "portable envelope version mismatch"; return NativeScriptPortableSaveStatus::UnsupportedVersion;
    }
    if (endian != kEndian || headerBytes != kHeaderBytes || owner != kOwnerScriptSession || reserved || reserved2 ||
        !candidate.SourceFingerprint || candidate.SchemaFingerprint != SchemaFingerprint() ||
        candidate.Bytes > kMaxEnvelopeBytes || candidate.PayloadBytes != candidate.Bytes - kHeaderBytes ||
        candidate.Globals > 16382 || !candidate.Threads || candidate.Threads > 96 ||
        candidate.ActiveThreads > candidate.Threads || candidate.StreamedScripts > 82) {
        error = "portable envelope header identity invalid"; return NativeScriptPortableSaveStatus::InvalidEnvelope;
    }
    if (candidate.Bytes > envelope.size()) { error = "portable envelope payload truncated"; return NativeScriptPortableSaveStatus::Truncated; }
    if (candidate.Bytes != envelope.size()) { error = "portable envelope has trailing bytes"; return NativeScriptPortableSaveStatus::InvalidEnvelope; }
    payload = envelope.subspan(kHeaderBytes);
    if (Hash(payload) != candidate.PayloadChecksum) {
        error = "portable envelope checksum mismatch"; return NativeScriptPortableSaveStatus::ChecksumMismatch;
    }
    info = candidate;
    error.clear();
    return NativeScriptPortableSaveStatus::Ok;
}
} // namespace

bool NativeScriptPortableSave::IsInstructionBoundary(const NativeScriptSession& session,
    std::size_t threadIndex, std::uint32_t target) {
    std::span<const std::uint8_t> storage;
    std::uint32_t base = 0;
    std::string error;
    if (!session.ScriptStorage(threadIndex, target, storage, base, error)) return false;
    if (!base && std::find(session.m_Headers.begin(), session.m_Headers.end(), target) != session.m_Headers.end()) return true;
    const auto first = base ? base : session.m_Metadata.CodeStart;
    if (target < first || std::uint64_t(target) >= std::uint64_t(base) + storage.size()) return false;
    auto pos = first;
    while (pos < target) {
        NativeScriptSession::Instruction instruction;
        if (!session.Decode(threadIndex, pos, instruction, error) || instruction.Next <= pos) return false;
        pos = instruction.Next;
    }
    return pos == target;
}

bool NativeScriptPortableSave::ValidateGraph(const NativeScriptSession& session, std::string& error) {
    const auto reject = [&](const char* message) { error = message; return false; };
    if (!session.m_Loaded || !session.m_SessionId) return reject("portable session is not loaded");
    if (session.m_InService || session.m_Pending || !session.m_Pass.empty() ||
        session.m_PendingInstruction || session.m_Faulted)
        return reject("portable session is not at a committed scheduler boundary");
    if (session.m_Memory.size() != session.m_Metadata.MainSize || session.m_Payload.size() < session.m_Memory.size() ||
        session.m_Metadata.GlobalBytes / 4 > 16382 || session.m_Metadata.GlobalBytes % 4 ||
        session.m_Threads.empty() || session.m_Threads.size() > 96 ||
        session.m_StreamedStates.size() != session.m_Metadata.StreamedDefinitions.size() ||
        session.m_StreamedPayloads.size() != session.m_StreamedStates.size())
        return reject("portable session owner sizes are inconsistent");
    if (static_cast<const NativeScriptThreadState&>(session.m_State) != session.m_Threads[0])
        return reject("portable shared/main thread state is inconsistent");
    if (session.m_State.OnAMissionFlag && !session.IsGlobal(session.m_State.OnAMissionFlag))
        return reject("portable mission flag is not a global cell");
    for (const auto value : session.m_State.FloatStats) if (!std::isfinite(value)) return reject("portable float stat is nonfinite");
    const auto& fade = session.m_State.Fade;
    if (!std::isfinite(fade.DurationSeconds) || !std::isfinite(fade.Alpha) ||
        !std::isfinite(fade.MusicDuration) || !std::isfinite(fade.MusicWait) ||
        !std::isfinite(fade.EffectsScale) || fade.Direction > 1 ||
        session.m_State.Clock.Hours >= 24 || session.m_State.Clock.Minutes >= 60)
        return reject("portable clock/fade state is invalid");

    std::array<std::uint8_t, 96> membership{};
    for (const auto slot : session.m_Active) {
        if (slot >= session.m_Threads.size() || membership[slot] || !session.m_Threads[slot].Active)
            return reject("portable active thread list is invalid");
        membership[slot] = 1;
    }
    for (const auto slot : session.m_Idle) {
        if (slot >= session.m_Threads.size() || membership[slot] || session.m_Threads[slot].Active)
            return reject("portable idle thread list is invalid");
        membership[slot] = 2;
    }
    if (std::find(membership.begin(), membership.begin() + session.m_Threads.size(), std::uint8_t(0)) !=
        membership.begin() + session.m_Threads.size())
        return reject("portable thread slot is not owned by active or idle list");

    std::array<std::uint16_t, 82> streamedUsers{};
    std::size_t missions = 0;
    std::int32_t missionIndex = -1;
    for (std::size_t i = 0; i < session.m_Threads.size(); ++i) {
        const auto& thread = session.m_Threads[i];
        if (!thread.Generation || thread.StackDepth > thread.ReturnStack.size() || thread.AndOrState > 28 ||
            thread.TimeMs != session.m_State.TimeMs || thread.LastOutputWrite.Sequence > thread.Commands)
            return reject("portable thread scalar state is invalid");
        for (std::size_t stack = thread.StackDepth; stack < thread.ReturnStack.size(); ++stack)
            if (thread.ReturnStack[stack]) return reject("portable return stack has noncanonical tail");
        if (!thread.Active) continue;
        if (!IsInstructionBoundary(session, i, thread.IP)) return reject("portable active IP is not an instruction boundary");
        for (std::size_t stack = 0; stack < thread.StackDepth; ++stack)
            if (!IsInstructionBoundary(session, i, thread.ReturnStack[stack]))
                return reject("portable return IP is not an instruction boundary");
        if (thread.ThisMustBeTheOnlyMissionRunning) {
            if (thread.IsExternal || !thread.UsesMissionCleanup || thread.BaseIP != 200000 ||
                thread.MissionIndex < 0 || std::size_t(thread.MissionIndex) >= session.m_Metadata.MissionOffsets.size() ||
                thread.StreamedIndex != -1 || thread.Locals.size() != 1024 || ++missions > 1)
                return reject("portable mission thread identity is invalid");
            missionIndex = thread.MissionIndex;
        } else if (thread.IsExternal) {
            if (thread.MissionIndex != -1 || thread.StreamedIndex < 0 ||
                std::size_t(thread.StreamedIndex) >= session.m_StreamedStates.size() ||
                thread.BaseIP != 300000 + std::uint32_t(thread.StreamedIndex) * 65536 || thread.Locals.size() != 34)
                return reject("portable streamed thread identity is invalid");
            const auto index = std::size_t(thread.StreamedIndex);
            if (!session.m_StreamedStates[index].Loaded ||
                session.m_StreamedStates[index].Generation != thread.StreamedGeneration ||
                ++streamedUsers[index] > std::numeric_limits<std::uint8_t>::max())
                return reject("portable streamed thread owner is stale");
        } else if (thread.BaseIP || thread.MissionIndex != -1 || thread.StreamedIndex != -1 ||
            thread.StreamedGeneration || thread.Locals.size() != 34) {
            return reject("portable main thread identity is invalid");
        }
    }
    if (session.m_State.AlreadyRunningMission != (missions == 1))
        return reject("portable mission ownership flag is inconsistent");
    if (missions == 1) {
        const auto start = session.m_Metadata.MissionOffsets[std::size_t(missionIndex)];
        const auto end = std::size_t(missionIndex) + 1 < session.m_Metadata.MissionOffsets.size() ?
            session.m_Metadata.MissionOffsets[std::size_t(missionIndex) + 1] : std::uint32_t(session.m_Payload.size());
        if (session.m_Mission.size() != end - start ||
            !std::equal(session.m_Mission.begin(), session.m_Mission.end(), session.m_Payload.begin() + start))
            return reject("portable mission payload owner is inconsistent");
    }
    for (std::size_t i = 0; i < session.m_StreamedStates.size(); ++i) {
        const auto& state = session.m_StreamedStates[i];
        if (state.Definition != session.m_Metadata.StreamedDefinitions[i] || state.Users != streamedUsers[i] ||
            state.Loaded != !session.m_StreamedPayloads[i].empty() || (state.Loaded && !state.Generation) ||
            (state.Loaded && session.m_StreamedPayloads[i].size() != state.Definition.Size))
            return reject("portable streamed registry is inconsistent");
    }
    error.clear();
    return true;
}

NativeScriptPortableSaveStatus NativeScriptPortableSave::Encode(const NativeScriptSession& session,
    std::vector<std::uint8_t>& output, NativeScriptPortableSaveInfo& info, std::string& error) {
    if (!session.m_Loaded) { error = "portable session is not loaded"; return NativeScriptPortableSaveStatus::NotLoaded; }
    if (session.m_InService || session.m_Pending || !session.m_Pass.empty()) {
        error = "portable session has outstanding work"; return NativeScriptPortableSaveStatus::Busy;
    }
    if (!ValidateGraph(session, error)) return NativeScriptPortableSaveStatus::InvalidState;
    try {
        Writer writer;
        writer.Bytes.reserve(8192);
        writer.Raw(kMagic); writer.U32(kEndian); writer.U16(NativeScriptPortableSaveMajor);
        writer.U16(NativeScriptPortableSaveMinor); writer.U32(kHeaderBytes); writer.U32(0); writer.U32(0);
        writer.U32(kOwnerScriptSession); writer.U64(Hash(session.m_Payload)); writer.U64(SchemaFingerprint());
        writer.U64(0); writer.U32(session.m_Metadata.GlobalBytes / 4);
        writer.U32(std::uint32_t(session.m_Threads.size())); writer.U32(std::uint32_t(session.m_Active.size()));
        writer.U32(std::uint32_t(session.m_StreamedStates.size())); writer.U32(0); writer.U32(0);
        writer.U32(kBodyVersion); writer.U32(0); writer.U64(session.m_CommandSequence);
        WriteExtendedState(writer, session.m_State);
        for (std::uint32_t offset = 8; offset < 8 + session.m_Metadata.GlobalBytes; offset += 4) {
            const auto value = std::uint32_t(session.m_Memory[offset]) |
                (std::uint32_t(session.m_Memory[offset + 1]) << 8) |
                (std::uint32_t(session.m_Memory[offset + 2]) << 16) |
                (std::uint32_t(session.m_Memory[offset + 3]) << 24);
            writer.U32(value);
        }
        for (const auto& thread : session.m_Threads) WriteThread(writer, thread);
        for (const auto slot : session.m_Active) writer.U32(std::uint32_t(slot));
        writer.U32(std::uint32_t(session.m_Idle.size()));
        for (const auto slot : session.m_Idle) writer.U32(std::uint32_t(slot));
        for (std::size_t i = 0; i < session.m_StreamedStates.size(); ++i) {
            const auto& state = session.m_StreamedStates[i];
            writer.U64(DefinitionFingerprint(state.Definition)); writer.U64(state.Generation);
            writer.U8(state.Users); writer.Bool(state.Loaded); writer.U16(0);
            writer.U64(state.Loaded ? Hash(session.m_StreamedPayloads[i]) : 0);
        }
        if (writer.Bytes.size() > kMaxEnvelopeBytes || writer.Bytes.size() > std::numeric_limits<std::uint32_t>::max()) {
            error = "portable envelope exceeds bounded size"; return NativeScriptPortableSaveStatus::Overflow;
        }
        const auto total = std::uint32_t(writer.Bytes.size());
        const auto payloadBytes = total - kHeaderBytes;
        const auto checksum = Hash(std::span<const std::uint8_t>(writer.Bytes).subspan(kHeaderBytes));
        writer.SetU32(20, total); writer.SetU32(24, payloadBytes); writer.SetU64(48, checksum);
        NativeScriptPortableSaveInfo candidate{NativeScriptPortableSaveMajor, NativeScriptPortableSaveMinor,
            total, payloadBytes, Hash(session.m_Payload), SchemaFingerprint(), checksum,
            session.m_Metadata.GlobalBytes / 4, std::uint32_t(session.m_Threads.size()),
            std::uint32_t(session.m_Active.size()), std::uint32_t(session.m_StreamedStates.size())};
        output = std::move(writer.Bytes);
        info = candidate;
        error.clear();
        return NativeScriptPortableSaveStatus::Ok;
    } catch (const std::bad_alloc&) {
        error = "portable envelope allocation failed";
        return NativeScriptPortableSaveStatus::Overflow;
    } catch (const std::length_error&) {
        error = "portable envelope size overflow";
        return NativeScriptPortableSaveStatus::Overflow;
    }
}

NativeScriptPortableSaveStatus NativeScriptPortableSave::Inspect(std::span<const std::uint8_t> envelope,
    NativeScriptPortableSaveInfo& info, std::string& error) {
    std::span<const std::uint8_t> payload;
    NativeScriptPortableSaveInfo candidate;
    const auto status = Header(envelope, candidate, payload, error);
    if (status != NativeScriptPortableSaveStatus::Ok) return status;
    info = candidate;
    return NativeScriptPortableSaveStatus::Ok;
}

NativeScriptPortableSaveStatus NativeScriptPortableSave::Restore(NativeScriptSession& destination,
    std::span<const std::uint8_t> envelope, NativeScriptPortableSaveInfo& info, std::string& error) {
    if (!destination.m_Loaded) { error = "portable destination is not loaded"; return NativeScriptPortableSaveStatus::NotLoaded; }
    if (destination.m_InService || destination.m_Pending || !destination.m_Pass.empty()) {
        error = "portable destination has outstanding work"; return NativeScriptPortableSaveStatus::Busy;
    }
    NativeScriptPortableSaveInfo parsedInfo;
    std::span<const std::uint8_t> payload;
    auto status = Header(envelope, parsedInfo, payload, error);
    if (status != NativeScriptPortableSaveStatus::Ok) return status;
    if (parsedInfo.SourceFingerprint != Hash(destination.m_Payload) ||
        parsedInfo.Globals != destination.m_Metadata.GlobalBytes / 4 ||
        parsedInfo.StreamedScripts != destination.m_Metadata.StreamedDefinitions.size()) {
        error = "portable envelope source SCM identity mismatch";
        return NativeScriptPortableSaveStatus::SourceMismatch;
    }
    try {
        Reader reader(payload);
        std::uint32_t bodyVersion = 0, reserved = 0;
        NativeScriptSession candidate;
        candidate.m_Metadata = destination.m_Metadata;
        candidate.m_Memory = destination.m_Memory;
        candidate.m_Payload = destination.m_Payload;
        candidate.m_StreamedPayloads = destination.m_StreamedPayloads;
        candidate.m_StreamedStates.resize(parsedInfo.StreamedScripts);
        candidate.m_Headers = destination.m_Headers;
        candidate.m_HeaderTargets = destination.m_HeaderTargets;
        candidate.m_SessionId = destination.m_SessionId;
        candidate.m_Loaded = true;
        if (!reader.U32(bodyVersion) || !reader.U32(reserved) || bodyVersion != kBodyVersion || reserved ||
            !reader.U64(candidate.m_CommandSequence) || !ReadExtendedState(reader, candidate.m_State)) {
            error = "portable session body truncated or malformed";
            return NativeScriptPortableSaveStatus::InvalidEnvelope;
        }
        for (std::uint32_t i = 0; i < parsedInfo.Globals; ++i) {
            std::uint32_t value = 0;
            if (!reader.U32(value)) { error = "portable globals truncated"; return NativeScriptPortableSaveStatus::Truncated; }
            const auto offset = 8 + i * 4;
            for (unsigned byte = 0; byte < 4; ++byte) candidate.m_Memory[offset + byte] = std::uint8_t(value >> (byte * 8));
        }
        candidate.m_Threads.resize(parsedInfo.Threads);
        for (auto& thread : candidate.m_Threads) {
            if (!ReadThread(reader, thread)) { error = "portable thread state truncated or malformed"; return NativeScriptPortableSaveStatus::InvalidEnvelope; }
        }
        candidate.m_Active.resize(parsedInfo.ActiveThreads);
        for (auto& slot : candidate.m_Active) {
            std::uint32_t value = 0;
            if (!reader.U32(value)) { error = "portable active list truncated"; return NativeScriptPortableSaveStatus::Truncated; }
            slot = value;
        }
        std::uint32_t idle = 0;
        if (!reader.U32(idle) || idle > parsedInfo.Threads) {
            error = "portable idle list malformed"; return NativeScriptPortableSaveStatus::InvalidEnvelope;
        }
        candidate.m_Idle.resize(idle);
        for (auto& slot : candidate.m_Idle) {
            std::uint32_t value = 0;
            if (!reader.U32(value)) { error = "portable idle list truncated"; return NativeScriptPortableSaveStatus::Truncated; }
            slot = value;
        }
        for (std::size_t i = 0; i < candidate.m_StreamedStates.size(); ++i) {
            std::uint64_t definition = 0, generation = 0, payloadHash = 0;
            std::uint8_t users = 0;
            bool loaded = false;
            std::uint16_t streamReserved = 0;
            if (!reader.U64(definition) || !reader.U64(generation) || !reader.U8(users) ||
                !reader.Bool(loaded) || !reader.U16(streamReserved) || !reader.U64(payloadHash)) {
                error = "portable streamed registry truncated"; return NativeScriptPortableSaveStatus::Truncated;
            }
            const auto& metadata = destination.m_Metadata.StreamedDefinitions[i];
            if (streamReserved || definition != DefinitionFingerprint(metadata) ||
                (loaded && (destination.m_StreamedPayloads[i].empty() ||
                    payloadHash != Hash(destination.m_StreamedPayloads[i]))) ||
                (!loaded && payloadHash)) {
                error = "portable streamed payload identity mismatch";
                return NativeScriptPortableSaveStatus::SourceMismatch;
            }
            candidate.m_StreamedStates[i] = {metadata, generation, users, loaded};
            if (!loaded) candidate.m_StreamedPayloads[i].clear();
        }
        if (!reader.Done()) { error = "portable session body has trailing data"; return NativeScriptPortableSaveStatus::InvalidEnvelope; }
        static_cast<NativeScriptThreadState&>(candidate.m_State) = candidate.m_Threads[0];
        candidate.m_Pass.clear(); candidate.m_PassCursor = 0;
        candidate.m_Pending = candidate.m_InService = candidate.m_Faulted = false;
        candidate.m_Fault = {}; candidate.m_PendingInstruction.reset(); candidate.m_PendingThread = 0;
        candidate.m_Mission.clear();
        for (const auto& thread : candidate.m_Threads) {
            if (!thread.Active || !thread.ThisMustBeTheOnlyMissionRunning) continue;
            const auto index = std::size_t(thread.MissionIndex);
            const auto start = candidate.m_Metadata.MissionOffsets[index];
            const auto end = index + 1 < candidate.m_Metadata.MissionOffsets.size() ?
                candidate.m_Metadata.MissionOffsets[index + 1] : std::uint32_t(candidate.m_Payload.size());
            candidate.m_Mission.assign(candidate.m_Payload.begin() + start, candidate.m_Payload.begin() + end);
        }
        if (!ValidateGraph(candidate, error)) return NativeScriptPortableSaveStatus::InvalidEnvelope;

        destination.m_State = std::move(candidate.m_State);
        destination.m_Memory = std::move(candidate.m_Memory);
        destination.m_Mission = std::move(candidate.m_Mission);
        destination.m_StreamedPayloads = std::move(candidate.m_StreamedPayloads);
        destination.m_StreamedStates = std::move(candidate.m_StreamedStates);
        destination.m_Threads = std::move(candidate.m_Threads);
        destination.m_Active = std::move(candidate.m_Active);
        destination.m_Idle = std::move(candidate.m_Idle);
        destination.m_Pass.clear(); destination.m_PassCursor = 0;
        destination.m_CommandSequence = candidate.m_CommandSequence;
        destination.m_Pending = destination.m_InService = destination.m_Faulted = false;
        destination.m_Fault = {}; destination.m_PendingInstruction.reset(); destination.m_PendingThread = 0;
        info = parsedInfo;
        error.clear();
        return NativeScriptPortableSaveStatus::Ok;
    } catch (const std::bad_alloc&) {
        error = "portable restore allocation failed";
        return NativeScriptPortableSaveStatus::Overflow;
    } catch (const std::length_error&) {
        error = "portable restore size overflow";
        return NativeScriptPortableSaveStatus::Overflow;
    }
}
