#include "app/platform/linux/NativeScriptSession.h"
#include "app/platform/linux/NativeScriptEntities.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstring>
#include <exception>
#include <limits>
#include <numbers>
#include <utility>

using int8 = std::int8_t;
using int16 = std::int16_t;
using int32 = std::int32_t;
using int64 = std::int64_t;
using uint8 = std::uint8_t;
using uint16 = std::uint16_t;
using uint32 = std::uint32_t;
using uint64 = std::uint64_t;
#ifndef __stdcall
#define __stdcall
#endif
#include "oswrapper/oswrapper.h"

namespace {
constexpr uint32 MainCapacity = 200000;
constexpr uint32 ExternalBase = 300000;
constexpr uint32 ExternalStride = 65536;
std::atomic<uint64> s_NextSession{1};

bool EqualNameNoCase(const std::array<char, 8>& left, const std::array<char, 8>& right) {
    for (std::size_t i = 0; i < left.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(left[i])) !=
            std::tolower(static_cast<unsigned char>(right[i]))) return false;
        if (!left[i] && !right[i]) return true;
    }
    return true;
}

// Explicit little-endian reads avoid alignment, aliasing and host-endian UB.
struct Reader {
    std::span<const uint8> Bytes;
    std::size_t Pos = 0;
    bool Read(unsigned count, uint32& value) {
        if (count > 4 || Pos > Bytes.size() || count > Bytes.size() - Pos) return false;
        value = 0;
        for (unsigned i = 0; i < count; ++i) value |= uint32(Bytes[Pos++]) << (8 * i);
        return true;
    }
};

uint32 Word(std::span<const uint8> bytes, std::size_t pos, unsigned count = 4) {
    uint32 value = 0;
    Reader{bytes, pos}.Read(count, value); // callers first validate complete chunks
    return value;
}

using O = NativeScriptOperandType;

bool ValidStat(int32 id) { return (id >= 0 && id < 82) || (id >= 120 && id < 343); }
bool FitsInt(float value) {
    return std::isfinite(value) && double(value) >= std::numeric_limits<int32>::min()
        && double(value) <= std::numeric_limits<int32>::max();
}

char LowerAscii(char value) {
    return value >= 'A' && value <= 'Z' ? char(value + ('a' - 'A')) : value;
}

bool ValidStreamedName(std::span<const char, 20> name) {
    const auto end = std::find(name.begin(), name.end(), '\0');
    if (end == name.begin() || end == name.end()) return false;
    return std::all_of(name.begin(), end, [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '_';
    });
}

std::string StreamedMemberName(const NativeScriptStreamedDefinition& definition) {
    std::string result;
    for (const char c : definition.Name) {
        if (!c) break;
        result.push_back(LowerAscii(c));
    }
    result += ".scm";
    return result;
}
} // namespace

NativeScriptPickupCollectedResult NativeScriptServices::HasPickupBeenCollected(const NativeScriptPickupReferenceRequest&) { return {}; }
NativeScriptServiceResult NativeScriptServices::RemoveScriptPickup(const NativeScriptPickupReferenceRequest&) { return {}; }

int32 NativeScriptSession::Instruction::Int(unsigned i) const { return std::bit_cast<int32>(Values[i]); }
float NativeScriptSession::Instruction::Float(unsigned i) const { return std::bit_cast<float>(Values[i]); }

bool NativeScriptSession::LoadMain(const char* gameDir, std::string& error) {
    if (!gameDir || !*gameDir || m_InService || m_Pending) {
        error = "invalid directory or outstanding script service";
        return false;
    }
    const std::string path = std::string(gameDir) + "/data/script/main.scm";
    // OS wrapper uses a 2048-byte path buffer. Never allow silent truncation.
    if (path.size() >= 2048) { error = "SCM path too long"; return false; }
    void* file = nullptr;
    if (OS_FileOpen(FILE_DATA_AREA_DEFAULT, &file, path.c_str(), FILE_ACCESS_READ) || !file) {
        error = "cannot open main.scm";
        return false;
    }
    struct Close { void* File; ~Close() { OS_FileClose(File); } } close{file};
    const int32 size = OS_FileSize(file);
    if (size < 8) { error = "main.scm is empty/truncated"; return false; }
    std::vector<uint8> prefix(static_cast<std::size_t>(size));
    if (OS_FileRead(file, prefix.data(), int32(prefix.size())) != 0) {
        error = "short main.scm read";
        return false;
    }
    return LoadMainBytes(prefix, uint64(size), error);
}

bool NativeScriptSession::LoadMainBytes(std::span<const uint8> prefix, uint64 fileBytes, std::string& error) {
    if (m_InService || m_Pending) { error = "outstanding script service"; return false; }
    auto reject = [&](const char* message) { error = message; return false; };
    if (prefix.size() != fileBytes || fileBytes > std::numeric_limits<int32>::max()) return reject("complete SCM payload required");
    NativeScriptMetadata metadata;
    std::array<uint32, 6> headers{}, targets{};
    uint32 pos = 0;
    constexpr uint8 indices[] = {115, 0, 1, 2, 3, 4};
    for (unsigned i = 0; i < headers.size(); ++i) {
        if (pos > prefix.size() || prefix.size() - pos < 8) return reject("truncated SCM header");
        if (Word(prefix, pos, 2) != 2 || prefix[pos + 2] != 1 || prefix[pos + 7] != indices[i])
            return reject("invalid SCM header GOTO/index");
        const uint32 next = Word(prefix, pos + 3);
        if (next < pos + 8 || next > prefix.size()) return reject("SCM chunk extent out of bounds");
        const uint32 length = next - pos;
        headers[i] = pos;
        targets[i] = next;
        switch (i) {
        case 0:
            metadata.GlobalBytes = length - 8;
            if (!metadata.GlobalBytes || metadata.GlobalBytes % 4 || next > 65536)
                return reject("invalid SCM global space");
            break;
        case 1:
            if (length < 12 || uint64(Word(prefix, pos + 8)) * 24 + 12 != length)
                return reject("invalid SCM used-object table");
            if (Word(prefix, pos + 8) >= 395) return reject("SCM used-object table exceeds source capacity");
            metadata.UsedObjects.resize(Word(prefix, pos + 8));
            for (std::size_t j = 0; j < metadata.UsedObjects.size(); ++j)
                std::copy_n(prefix.begin() + pos + 12 + j * 24, 24, metadata.UsedObjects[j].begin());
            break;
        case 2: {
            if (length < 24) return reject("truncated mission metadata");
            metadata.MainSize = Word(prefix, pos + 8);
            metadata.LargestMission = Word(prefix, pos + 12);
            metadata.MissionLocals = Word(prefix, pos + 20);
            const uint32 count = Word(prefix, pos + 16, 2);
            if (24 + count * 4 != length || Word(prefix, pos + 18, 2) != 0)
                return reject("invalid/unsupported exclusive mission table");
            if (metadata.MainSize > prefix.size() || metadata.MainSize > MainCapacity || metadata.LargestMission > 69000 || metadata.MissionLocals > 1024)
                return reject("SCM main/mission capacity exceeded");
            for (uint32 j = 0; j < count; ++j) {
                const uint32 offset = Word(prefix, pos + 24 + j * 4);
                if (offset < metadata.MainSize || offset >= fileBytes) return reject("mission file offset out of bounds");
                metadata.MissionOffsets.push_back(offset);
            }
            break;
        }
        case 3:
            if (length < 16) return reject("truncated streamed metadata");
            metadata.LargestStreamed = Word(prefix, pos + 8);
            metadata.StreamedScripts = Word(prefix, pos + 12);
            if (uint64(metadata.StreamedScripts) * 28 + 16 != length) return reject("invalid streamed table");
            if (metadata.StreamedScripts > 82 || metadata.LargestStreamed >= ExternalStride)
                return reject("streamed script source capacity exceeded");
            metadata.StreamedDefinitions.resize(metadata.StreamedScripts);
            for (uint32 j = 0; j < metadata.StreamedScripts; ++j) {
                const auto at = pos + 16 + j * 28;
                auto& definition = metadata.StreamedDefinitions[j];
                std::copy_n(prefix.begin() + at, definition.Name.size(), definition.Name.begin());
                definition.FileOffset = Word(prefix, at + 20);
                definition.Size = Word(prefix, at + 24);
                if (!ValidStreamedName(definition.Name) || !definition.Size ||
                    definition.Size > metadata.LargestStreamed)
                    return reject("streamed script exceeds declared maximum");
                for (uint32 previous = 0; previous < j; ++previous) {
                    if (StreamedMemberName(metadata.StreamedDefinitions[previous]) == StreamedMemberName(definition))
                        return reject("duplicate streamed script name");
                }
            }
            break;
        case 4:
            if (length != 12) return reject("invalid SCM chunk 3");
            break;
        case 5:
            if (length != 16 || Word(prefix, pos + 8) != metadata.GlobalBytes)
                return reject("inconsistent SCM globals metadata");
            metadata.Build = Word(prefix, pos + 12);
            break;
        }
        pos = next;
    }
    metadata.CodeStart = pos;
    if (uint64(pos) + 2 > metadata.MainSize) return reject("missing main executable region");
    for (std::size_t i = 0; i < metadata.MissionOffsets.size(); ++i) {
        const uint64 start = metadata.MissionOffsets[i];
        const uint64 end = i + 1 < metadata.MissionOffsets.size() ? metadata.MissionOffsets[i + 1] : fileBytes;
        if (end <= start || end - start < 2 || end - start > metadata.LargestMission)
            return reject("invalid mission extent/order/capacity");
    }
    std::vector<uint8> memory(prefix.begin(), prefix.begin() + metadata.MainSize);
    std::vector<uint8> payload(prefix.begin(), prefix.end());
    std::vector<NativeScriptThreadState> threads;
    threads.reserve(96); // source script pool capacity; references survive launch
    threads.emplace_back();
    threads[0].Active = true;
    std::vector<std::vector<uint8>> streamedPayloads(metadata.StreamedScripts);
    std::vector<NativeScriptStreamedState> streamedStates(metadata.StreamedScripts);
    for (std::size_t i = 0; i < streamedStates.size(); ++i)
        streamedStates[i].Definition = metadata.StreamedDefinitions[i];
    m_Memory = std::move(memory);
    m_Payload = std::move(payload);
    m_Mission.clear();
    m_StreamedPayloads = std::move(streamedPayloads);
    m_StreamedStates = std::move(streamedStates);
    m_Threads = std::move(threads);
    m_Active = {0};
    m_Active.reserve(96);
    m_Idle.clear();
    m_Idle.reserve(96);
    m_Pass.clear();
    m_PassCursor = 0;
    m_Metadata = std::move(metadata);
    m_Headers = headers;
    m_HeaderTargets = targets;
    m_State = {};
    static_cast<NativeScriptThreadState&>(m_State) = m_Threads[0];
    m_SessionId = s_NextSession.fetch_add(1);
    m_Loaded = true;
    m_Pending = m_Faulted = false;
    m_CommandSequence = 0;
    m_Fault = {};
    m_PendingInstruction.reset();
    error.clear();
    return true;
}

bool NativeScriptSession::LoadStreamedScript(const char* gameDir, uint16 scriptIndex, std::string& error) {
    return LoadStreamedScriptInternal(gameDir, scriptIndex, false, error);
}

bool NativeScriptSession::FulfillPendingStreamedScript(const char* gameDir, std::string& error) {
    if (!m_Pending || !m_PendingInstruction || m_PendingInstruction->Opcode != 0x08A9 ||
        m_PendingInstruction->Int(0) < 0 || m_PendingInstruction->Int(0) > std::numeric_limits<uint16>::max()) {
        error = "no exact pending streamed-script request";
        return false;
    }
    return LoadStreamedScriptInternal(gameDir, uint16(m_PendingInstruction->Int(0)), true, error);
}

bool NativeScriptSession::LoadStreamedScriptInternal(const char* gameDir, uint16 scriptIndex,
    bool pendingFulfillment, std::string& error) {
    const bool exactPending = pendingFulfillment && m_Pending && m_PendingInstruction &&
        m_PendingInstruction->Opcode == 0x08A9 && m_PendingInstruction->Int(0) == scriptIndex && !m_Pass.empty();
    if (!m_Loaded || !gameDir || !*gameDir || m_InService ||
        (pendingFulfillment ? !exactPending : m_Pending || !m_Pass.empty()) ||
        scriptIndex >= m_StreamedStates.size()) {
        error = "invalid streamed-script load";
        return false;
    }
    if (m_StreamedStates[scriptIndex].Loaded || m_StreamedStates[scriptIndex].Users) {
        error = "streamed script already loaded/in use";
        return false;
    }
    const std::string path = std::string(gameDir) + "/data/script/script.img";
    if (path.size() >= 2048) { error = "streamed IMG path too long"; return false; }
    void* file = nullptr;
    if (OS_FileOpen(FILE_DATA_AREA_DEFAULT, &file, path.c_str(), FILE_ACCESS_READ) || !file) {
        error = "cannot open script.img";
        return false;
    }
    struct Close { void* File; ~Close() { OS_FileClose(File); } } close{file};
    const int32 fileSize = OS_FileSize(file);
    std::array<uint8, 8> header{};
    if (fileSize < 8 || OS_FileRead(file, header.data(), int32(header.size())) != 0 ||
        std::memcmp(header.data(), "VER2", 4) != 0) {
        error = "invalid streamed IMG header";
        return false;
    }
    const uint32 count = Word(header, 4);
    const uint64 directoryEnd = 8ull + uint64(count) * 32;
    if (!count || count > 300000 || count != m_StreamedStates.size() || directoryEnd > uint32(fileSize)) {
        error = "invalid streamed IMG directory";
        return false;
    }
    std::vector<bool> matched(m_StreamedStates.size());
    uint64 memberOffset = 0, memberExtent = 0;
    for (uint32 i = 0; i < count; ++i) {
        std::array<uint8, 32> entry{};
        if (OS_FileRead(file, entry.data(), int32(entry.size())) != 0) {
            error = "short streamed IMG directory";
            return false;
        }
        const auto zero = std::find(entry.begin() + 8, entry.end(), uint8(0));
        if (zero == entry.end()) { error = "unterminated streamed IMG name"; return false; }
        std::string name;
        for (auto at = entry.begin() + 8; at != zero; ++at) name.push_back(LowerAscii(char(*at)));
        const uint64 offset = uint64(Word(entry, 0)) * 2048;
        const uint64 extent = uint64(Word(entry, 4)) * 2048;
        if (offset < directoryEnd || !extent || offset + extent > uint32(fileSize)) {
            error = "streamed IMG member out of bounds";
            return false;
        }
        bool known = false;
        for (std::size_t definitionIndex = 0; definitionIndex < m_StreamedStates.size(); ++definitionIndex) {
            if (name != StreamedMemberName(m_StreamedStates[definitionIndex].Definition)) continue;
            if (matched[definitionIndex] || m_StreamedStates[definitionIndex].Definition.Size > extent) {
                error = "ambiguous/truncated streamed IMG member";
                return false;
            }
            matched[definitionIndex] = true;
            known = true;
            if (definitionIndex == scriptIndex) {
                memberOffset = offset;
                memberExtent = extent;
            }
            break;
        }
        if (!known) { error = "streamed metadata/archive name mismatch"; return false; }
    }
    const uint32 size = m_StreamedStates[scriptIndex].Definition.Size;
    if (!std::ranges::all_of(matched, [](bool value) { return value; }) ||
        !memberExtent || size > memberExtent || memberOffset > std::numeric_limits<int32>::max()) {
        error = "missing/truncated streamed IMG member";
        return false;
    }
    std::vector<uint8> bytes(size);
    OS_FileSetPosition(file, int32(memberOffset));
    if (OS_FileGetPosition(file) != int32(memberOffset) || OS_FileRead(file, bytes.data(), int32(bytes.size())) != 0) {
        error = "short streamed script read";
        return false;
    }
    return LoadStreamedScriptBytesInternal(scriptIndex, bytes, pendingFulfillment, error);
}

bool NativeScriptSession::LoadStreamedScriptBytes(uint16 scriptIndex, std::span<const uint8> bytes, std::string& error) {
    return LoadStreamedScriptBytesInternal(scriptIndex, bytes, false, error);
}

bool NativeScriptSession::LoadStreamedScriptBytesInternal(uint16 scriptIndex, std::span<const uint8> bytes,
    bool pendingFulfillment, std::string& error) {
    const bool exactPending = pendingFulfillment && m_Pending && m_PendingInstruction &&
        m_PendingInstruction->Opcode == 0x08A9 && m_PendingInstruction->Int(0) == scriptIndex && !m_Pass.empty();
    if (!m_Loaded || m_InService || (pendingFulfillment ? !exactPending : m_Pending || !m_Pass.empty()) ||
        scriptIndex >= m_StreamedStates.size()) {
        error = "invalid streamed-script payload load";
        return false;
    }
    auto& state = m_StreamedStates[scriptIndex];
    if (state.Loaded || state.Users || bytes.size() != state.Definition.Size || bytes.empty() ||
        state.Generation == std::numeric_limits<uint64>::max()) {
        error = "streamed-script payload identity mismatch";
        return false;
    }
    std::vector<uint8> candidate(bytes.begin(), bytes.end());
    m_StreamedPayloads[scriptIndex] = std::move(candidate);
    ++state.Generation;
    state.Loaded = true;
    error.clear();
    return true;
}

bool NativeScriptSession::UnloadStreamedScript(uint16 scriptIndex, std::string& error) {
    if (!m_Loaded || m_InService || m_Pending || !m_Pass.empty() || scriptIndex >= m_StreamedStates.size()) {
        error = "invalid streamed-script unload";
        return false;
    }
    auto& state = m_StreamedStates[scriptIndex];
    if (!state.Loaded || state.Users) { error = "streamed script not loaded or still in use"; return false; }
    std::vector<uint8>{}.swap(m_StreamedPayloads[scriptIndex]);
    state.Loaded = false;
    error.clear();
    return true;
}

bool NativeScriptSession::IsGlobal(uint16 offset) const {
    // Globals are byte offsets, not cell indices. Header and code are immutable.
    return offset >= 8 && uint32(offset) + 4 <= 8 + m_Metadata.GlobalBytes;
}

bool NativeScriptSession::ReadGlobal(uint16 offset, int32& value) const {
    if (!m_Loaded || !IsGlobal(offset)) return false;
    value = std::bit_cast<int32>(Word(m_Memory, offset));
    return true;
}

bool NativeScriptSession::InspectInstruction(std::size_t threadIndex, NativeScriptInstructionForm& out, std::string& error) const {
    if (!m_Loaded || m_InService || threadIndex >= m_Threads.size()) {
        error = "invalid script inspection";
        return false;
    }
    Instruction instruction;
    if (!Decode(threadIndex, m_Threads[threadIndex].IP, instruction, error)) return false;
    const auto* schema = NativeScriptLookupSchema(instruction.Opcode);
    if (!schema) {
        error = "unclassified instruction schema";
        return false;
    }
    const auto& thread = m_Threads[threadIndex];
    NativeScriptInstructionForm candidate;
    candidate.Session = m_SessionId;
    candidate.Sequence = m_CommandSequence + 1;
    candidate.ThreadGeneration = thread.Generation;
    candidate.ThreadIndex = std::uint32_t(threadIndex);
    candidate.IP = thread.IP;
    candidate.NextIP = instruction.Next;
    candidate.BaseIP = thread.BaseIP;
    candidate.LocalCount = std::uint32_t(thread.Locals.size());
    candidate.MissionIndex = thread.MissionIndex;
    candidate.StreamedIndex = thread.StreamedIndex;
    candidate.StreamedGeneration = thread.StreamedGeneration;
    candidate.Opcode = instruction.Opcode;
    candidate.RawOpcode = std::uint16_t(instruction.Opcode | (instruction.Negated ? 0x8000 : 0));
    candidate.OperandTypes = instruction.OperandTypes;
    candidate.OperandTags = instruction.RawTags;
    candidate.ArrayCounts = instruction.ArrayCounts;
    candidate.ArrayFlags = instruction.ArrayFlags;
    candidate.OperandCount = instruction.OperandCount;
    candidate.FixedOperandCount = instruction.FixedOperandCount;
    candidate.VariadicArguments = instruction.VariadicArguments;
    candidate.Semantics = schema->Semantics;
    candidate.Negated = instruction.Negated;
    candidate.UsesMissionCleanup = thread.UsesMissionCleanup;
    candidate.ExclusiveMission = thread.ThisMustBeTheOnlyMissionRunning;
    candidate.External = thread.IsExternal;
    candidate.ThreadForm = thread.IsExternal ? NativeScriptThreadForm::Streamed :
        thread.ThisMustBeTheOnlyMissionRunning ? NativeScriptThreadForm::Mission : NativeScriptThreadForm::Main;
    out = candidate;
    error.clear();
    return true;
}

bool NativeScriptSession::ScriptStorage(std::size_t threadIndex, uint32 ip, std::span<const uint8>& bytes,
    uint32& base, std::string& error) const {
    const auto& thread = m_Threads[threadIndex];
    if (ip < MainCapacity) {
        bytes = m_Memory;
        base = 0;
        return true;
    }
    if (thread.ThisMustBeTheOnlyMissionRunning && ip >= MainCapacity &&
        uint64(ip) - MainCapacity < m_Mission.size()) {
        bytes = m_Mission;
        base = MainCapacity;
        return true;
    }
    if (thread.IsExternal && thread.StreamedIndex >= 0 &&
        std::size_t(thread.StreamedIndex) < m_StreamedStates.size()) {
        const auto index = std::size_t(thread.StreamedIndex);
        const auto& streamed = m_StreamedStates[index];
        if (streamed.Loaded && streamed.Generation == thread.StreamedGeneration &&
            ip >= thread.BaseIP && uint64(ip) - thread.BaseIP < m_StreamedPayloads[index].size()) {
            bytes = m_StreamedPayloads[index];
            base = thread.BaseIP;
            return true;
        }
    }
    error = "instruction pointer has no owned script storage";
    return false;
}

bool NativeScriptSession::Decode(std::size_t thread, uint32 ip, Instruction& d, std::string& error,
    bool validateRuntimeValues) const {
    const auto& state = m_Threads[thread];
    std::span<const uint8> bytes;
    uint32 base = 0;
    if (!ScriptStorage(thread, ip, bytes, base, error)) return false;
    Reader reader{bytes, ip - base};
    uint32 opcode = 0;
    if (!reader.Read(2, opcode)) { error = "truncated opcode"; return false; }
    d.Opcode = uint16(opcode & 0x7FFF);
    d.Negated = (opcode & 0x8000) != 0;
    const auto* signature = NativeScriptLookupSchema(d.Opcode);
    if (!signature) { error = "unsupported opcode (including NOT forms)"; return false; }
    d.FixedOperandCount = signature->OperandCount;
    d.VariadicArguments = signature->VariadicArguments;
    auto decodeOperand = [&](unsigned i, O type, uint32 tag, bool variadic) {
        uint32 bits = 0;
        d.RawTags[i] = uint8(tag);
        d.OperandTypes[i] = type;
        if (type == O::DebugString128) {
            if (variadic) { error = "invalid fixed debug string"; return false; }
            for (unsigned n = 0; n < 128; ++n) {
                if (!reader.Read(1, bits)) { error = "truncated fixed debug string"; return false; }
            }
            return true;
        }
        if (type == O::IgnoredString) {
            d.Tags[i] = uint8(tag);
            if (tag == 9) {
                for (unsigned n = 0; n < 8; ++n) if (!reader.Read(1, bits)) { error = "truncated ignored string"; return false; }
                return true;
            }
            if (tag == 14) {
                if (!reader.Read(1, bits) || !bits) { error = "invalid ignored Pascal string"; return false; }
                const auto length = bits;
                for (uint32 n = 0; n < length; ++n) if (!reader.Read(1, bits)) { error = "truncated ignored Pascal string"; return false; }
                return true;
            }
            if (tag == 15) {
                for (unsigned n = 0; n < 16; ++n) if (!reader.Read(1, bits)) { error = "truncated ignored long string"; return false; }
                return true;
            }
            if (tag == 10 || tag == 11 || tag == 16 || tag == 17) {
                if (!reader.Read(2, bits)) { error = "truncated ignored string variable"; return false; }
                return true;
            }
            if (tag == 12 || tag == 13 || tag == 18 || tag == 19) {
                uint32 indexVar = 0, count = 0, flags = 0;
                if (!reader.Read(2, bits) || !reader.Read(2, indexVar) || !reader.Read(1, count) ||
                    !reader.Read(1, flags)) {
                    error = "truncated ignored string array";
                    return false;
                }
                d.ArrayCounts[i] = uint8(count);
                d.ArrayFlags[i] = uint8(flags);
                return true;
            }
            error = "unsupported ignored string operand";
            return false;
        }
        if (type == O::StringOutput) {
            if (variadic || (tag != 10 && tag != 11 && tag != 12 && tag != 13 &&
                tag != 16 && tag != 17 && tag != 18 && tag != 19) || !reader.Read(2, bits)) {
                error = "invalid string output operand";
                return false;
            }
            const uint32 length = tag >= 16 ? 16 : 8;
                const bool array = tag == 12 || tag == 13 || tag == 18 || tag == 19;
            const bool global = tag == 10 || tag == 12 || tag == 16 || tag == 18;
                if (array) {
                uint32 indexVar = 0, count = 0, flags = 0;
                if (!reader.Read(2, indexVar) || !reader.Read(1, count) || !reader.Read(1, flags) ||
                    !count || (flags & 0x7F) != (length == 16 ? 3u : 2u)) {
                    error = "invalid string output array";
                    return false;
                }
                    const bool globalIndex = (flags & 0x80) != 0;
                    d.ArrayCounts[i] = uint8(count);
                    d.ArrayFlags[i] = uint8(flags);
                    if ((globalIndex && !IsGlobal(uint16(indexVar))) || (!globalIndex && indexVar >= state.Locals.size())) {
                        error = "string output array index variable out of bounds";
                        return false;
                    }
                    if (!validateRuntimeValues) {
                        d.Tags[i] = uint8(tag);
                        d.Values[i] = bits;
                        d.OutputGlobal = global;
                        return true;
                    }
                    const int32 index = std::bit_cast<int32>(globalIndex ? Word(m_Memory, indexVar) : state.Locals[indexVar]);
                if (index < 0 || uint32(index) >= count) { error = "string output array index out of bounds"; return false; }
                const uint64 address = uint64(bits) + uint64(index) * (global ? length : length / 4);
                if (address > std::numeric_limits<uint16>::max()) { error = "string output array address overflow"; return false; }
                bits = uint32(address);
            }
            if ((global && (bits < 8 || bits + length > 8 + m_Metadata.GlobalBytes)) ||
                (!global && bits + length / 4 > state.Locals.size())) {
                error = "string output out of bounds";
                return false;
            }
            d.Tags[i] = uint8(tag);
            d.Values[i] = bits;
            d.OutputGlobal = global;
            return true;
        }
        if (type == O::DebugString) {
            if (tag != 14 || variadic || !reader.Read(1, bits) || !bits) { error = "invalid debug string operand"; return false; }
            const uint32 length = bits;
            for (uint32 n = 0; n < length; ++n) {
                if (!reader.Read(1, bits)) { error = "truncated debug string"; return false; }
            }
            d.Tags[i] = uint8(tag);
            return true;
        }
        if (type == O::String) {
            if (variadic) { error = "unsupported variadic string operand"; return false; }
            d.Tags[i] = uint8(tag);
            if (tag == 10 || tag == 11 || tag == 16 || tag == 17) {
                if (!reader.Read(2, bits)) { error = "truncated string variable"; return false; }
                const uint32 length = tag >= 16 ? 16 : 8;
                if (!validateRuntimeValues) return true;
                if (tag == 10 || tag == 16) {
                    if (bits < 8 || bits + length > 8 + m_Metadata.GlobalBytes) {
                        error = "global string variable out of bounds";
                        return false;
                    }
                    std::copy_n(reinterpret_cast<const char*>(m_Memory.data() + bits), length, d.LongText.data());
                } else {
                    const uint32 cells = length / 4;
                    if (bits + cells > state.Locals.size()) { error = "local string variable out of bounds"; return false; }
                    std::memcpy(d.LongText.data(), state.Locals.data() + bits, length);
                }
                std::copy_n(d.LongText.data(), d.Text.size(), d.Text.data());
                d.Strings[i] = d.LongText;
                return true;
            }
            if (tag == 12 || tag == 13 || tag == 18 || tag == 19) {
                uint32 indexVar = 0, count = 0, flags = 0;
                if (!reader.Read(2, bits) || !reader.Read(2, indexVar) || !reader.Read(1, count) ||
                    !reader.Read(1, flags)) {
                    error = "truncated string array";
                    return false;
                }
                d.ArrayCounts[i] = uint8(count);
                d.ArrayFlags[i] = uint8(flags);
                const uint32 length = tag >= 18 ? 16 : 8;
                if (!count || (flags & 0x7F) != (length == 16 ? 3u : 2u)) {
                    error = "invalid string array metadata";
                    return false;
                }
                if (!validateRuntimeValues) return true;
                const bool global = tag == 12 || tag == 18;
                const bool globalIndex = (flags & 0x80) != 0;
                int32 index = 0;
                if (globalIndex) {
                    if (!IsGlobal(uint16(indexVar))) { error = "global string array index out of bounds"; return false; }
                    index = int32(Word(m_Memory, indexVar));
                } else {
                    if (indexVar >= state.Locals.size()) { error = "local string array index out of bounds"; return false; }
                    index = int32(state.Locals[indexVar]);
                }
                if (index < 0 || uint32(index) >= count) { error = "string array index outside declared count"; return false; }
                const uint64 address = uint64(bits) + uint64(index) * (global ? length : length / 4);
                if (global) {
                    if (address < 8 || address + length > 8 + m_Metadata.GlobalBytes) {
                        error = "global string array value out of bounds";
                        return false;
                    }
                    std::copy_n(reinterpret_cast<const char*>(m_Memory.data() + address), length, d.LongText.data());
                } else {
                    if (address + length / 4 > state.Locals.size()) {
                        error = "local string array value out of bounds";
                        return false;
                    }
                    std::memcpy(d.LongText.data(), state.Locals.data() + address, length);
                }
                std::copy_n(d.LongText.data(), d.Text.size(), d.Text.data());
                d.Strings[i] = d.LongText;
                return true;
            }
            if (tag == 15) {
                for (unsigned n = 0; n < 16; ++n) {
                    if (!reader.Read(1, bits)) { error = "truncated static long string"; return false; }
                    if (validateRuntimeValues) d.LongText[n] = char(bits);
                }
                if (validateRuntimeValues) {
                    std::copy_n(d.LongText.data(), d.Text.size(), d.Text.data());
                    d.Strings[i] = d.LongText;
                }
                return true;
            }
            if (tag != 9 && tag != 14) { error = "unsupported string operand type"; return false; }
            uint32 length = d.Text.size();
            if (tag == 14) {
                if (!reader.Read(1, bits)) { error = "truncated long-string length"; return false; }
                if (!bits || (validateRuntimeValues && bits > d.LongText.size())) { error = "unsupported long-string length"; return false; }
                length = bits;
            }
            for (uint32 n = 0; n < length; ++n) {
                if (!reader.Read(1, bits)) { error = tag == 9 ? "truncated short string" : "truncated long string"; return false; }
                if (n < d.LongText.size()) d.LongText[n] = char(bits);
            }
            std::copy_n(d.LongText.data(), d.Text.size(), d.Text.data());
            d.Strings[i] = d.LongText;
            return true;
        }
        if (tag == 2 || tag == 3 || tag == 7 || tag == 8) {
            const bool array = tag == 7 || tag == 8;
            if (!reader.Read(2, bits)) { error = "truncated variable operand"; return false; }
            if (array) {
                // RunningScript::{ReadArrayInformation,CollectParameters,
                // StoreParameters}: global base is bytes, local base is cells;
                // the index variable is independent of the array's own bank.
                // GetAtIPFromArray<T> supplies the source typed/count checks.
                uint32 indexVar = 0, count = 0, flags = 0;
                if (!reader.Read(2, indexVar) || !reader.Read(1, count) || !reader.Read(1, flags)) {
                    error = "truncated array operand"; return false;
                }
                const bool global = tag == 7, globalIndex = (flags & 0x80) != 0;
                d.ArrayCounts[i] = uint8(count);
                d.ArrayFlags[i] = uint8(flags);
                if (variadic) {
                    if ((flags & 0x7F) > 1) { error = "invalid variadic array element type"; return false; }
                }
                const bool floating = type == O::Float || type == O::FloatOutput || type == O::InOutFloat;
                if ((!variadic && (flags & 0x7F) != (floating ? 1u : 0u)) || !count) {
                    error = "invalid numeric array type/count"; return false;
                }
                if ((global && !IsGlobal(uint16(bits))) || (!global && bits >= state.Locals.size()) ||
                    (globalIndex && !IsGlobal(uint16(indexVar))) || (!globalIndex && indexVar >= state.Locals.size())) {
                    error = "array base/index variable out of bounds"; return false;
                }
                if (!validateRuntimeValues) {
                    tag = global ? 2 : 3;
                    return true;
                }
                const int32 index = std::bit_cast<int32>(globalIndex ? Word(m_Memory, indexVar) : state.Locals[indexVar]);
                // Check signed index BEFORE address arithmetic/narrowing. An
                // in-buffer address is insufficient if outside the declared array.
                if (index < 0 || uint32(index) >= count) { error = "array index out of declared bounds"; return false; }
                const int64 address = int64(bits) + int64(index) * (global ? 4 : 1);
                if (address > std::numeric_limits<uint16>::max()) { error = "array address overflow"; return false; }
                bits = uint32(address);
                tag = global ? 2 : 3;
            }
            if ((tag == 2 && !IsGlobal(uint16(bits))) || (tag == 3 && bits >= state.Locals.size())) {
                error = "script variable out of bounds";
                return false;
            }
            d.Tags[i] = uint8(tag);
            const bool output = type == O::Output || type == O::FloatOutput || type == O::InOutInteger || type == O::InOutFloat;
            const bool floating = type == O::Float || type == O::FloatOutput || type == O::InOutFloat;
            if (output) {
                d.Values[i] = bits;
                d.OutputGlobal = tag == 2;
                if (type == O::InOutInteger || type == O::InOutFloat) {
                    d.OutputValue = tag == 2 ? Word(m_Memory, bits) : state.Locals[bits];
                    if (validateRuntimeValues && floating && !std::isfinite(std::bit_cast<float>(d.OutputValue))) {
                        error = "nonfinite arithmetic destination"; return false;
                    }
                }
                return true;
            }
            bits = tag == 2 ? Word(m_Memory, bits) : state.Locals[bits];
        } else if ((type == O::Integer || type == O::Argument) && (tag == 1 || tag == 4 || tag == 5)) {
            if (!reader.Read(tag == 1 ? 4 : tag == 4 ? 1 : 2, bits)) { error = "truncated integer"; return false; }
            if (tag == 4) bits = uint32(int32(std::bit_cast<int8>(uint8(bits))));
            if (tag == 5) bits = uint32(int32(std::bit_cast<int16>(uint16(bits))));
        } else if ((type == O::Float || type == O::Argument) && tag == 6) {
            if (!reader.Read(4, bits)) { error = "truncated float"; return false; }
        } else {
            error = "wrong/unsupported typed operand";
            return false;
        }
        d.Tags[i] = uint8(tag);
        d.Values[i] = bits;
        if (validateRuntimeValues && (type == O::Float || (type == O::Argument && tag == 6)) &&
            !std::isfinite(d.Float(i))) { error = "nonfinite float operand"; return false; }
        return true;
    };
    for (unsigned i = 0; i < signature->OperandCount; ++i) {
        uint32 tag = 0;
        if (signature->Operands[i] != O::DebugString128 && !reader.Read(1, tag)) {
            error = "truncated operand tag";
            return false;
        }
        if (!decodeOperand(i, signature->Operands[i], tag, false)) return false;
        ++d.OperandCount;
    }
    if (signature->VariadicArguments) {
        for (unsigned arguments = 0;; ++arguments) {
            uint32 tag = 0;
            if (!reader.Read(1, tag)) { error = "missing variadic argument terminator"; return false; }
            if (!tag) break;
            if (arguments >= 32 || d.OperandCount >= NativeScriptMaxOperands) {
                error = "new script argument capacity exceeded";
                return false;
            }
            if (!decodeOperand(d.OperandCount, O::Argument, tag, true)) return false;
            ++d.OperandCount;
        }
    }
    d.Next = base + uint32(reader.Pos);
    return true;
}

uint32 NativeScriptSession::Target(std::size_t thread, int32 target) const {
    return target < 0 ? uint32(uint64(m_Threads[thread].BaseIP) + uint64(-int64(target))) : uint32(target);
}

bool NativeScriptSession::IsTarget(std::size_t thread, int32 label) const {
    m_TargetError.clear();
    // RunningScript::UpdatePC interprets every nonnegative label in the shared
    // main ScriptSpace. Mission/streamed-local labels are negative offsets from
    // BaseIP, even though this owner represents their addresses numerically.
    if (label >= 0 && uint32(label) >= MainCapacity) return false;
    if (label < 0 && !m_Threads[thread].BaseIP) return false;
    if (label < 0 && uint64(m_Threads[thread].BaseIP) + uint64(-int64(label)) > std::numeric_limits<uint32>::max()) return false;
    const uint32 target = Target(thread, label);
    std::span<const uint8> bytes;
    uint32 base = 0;
    std::string storageError;
    if (!ScriptStorage(thread, target, bytes, base, storageError)) { m_TargetError = storageError; return false; }
    if (!base && std::find(m_Headers.begin(), m_Headers.end(), target) != m_Headers.end()) return true;
    if (!base && (target < m_Metadata.CodeStart || target >= m_Metadata.MainSize)) return false;
    // Prove an instruction boundary by typed decoding, never searching bytes.
    // This bounded slice rejects forward labels beyond an unknown instruction.
    uint32 pos = base ? base : m_Metadata.CodeStart;
    while (pos < target) {
        Instruction d;
        std::string error;
        if (!Decode(thread, pos, d, error, false)) {
            m_TargetError = "scan@" + std::to_string(pos) + " target=" + std::to_string(target) + ": " + error;
            return false;
        }
        pos = d.Next;
    }
    return pos == target;
}

NativeScriptResult NativeScriptSession::Fail(std::size_t thread, NativeScriptStatus status, uint16 opcode, std::string message) {
    m_Faulted = true;
    m_Pending = false;
    m_PendingInstruction.reset();
    m_Fault = {status, m_Threads[thread].IP, opcode, 0, std::move(message), thread};
    return m_Fault;
}

NativeScriptResult NativeScriptSession::Step(NativeScriptServices& services) {
    if (!m_Loaded || m_InService) return {NativeScriptStatus::Error, m_State.IP, 0, 0, "session not loaded or reentrant call"};
    if (!m_Pass.empty()) return {NativeScriptStatus::Error, m_State.IP, 0, 0, "scheduler pass outstanding"};
    auto result = StepThread(services, 0);
    static_cast<NativeScriptThreadState&>(m_State) = m_Threads[0];
    return result;
}

NativeScriptResult NativeScriptSession::StepThread(NativeScriptServices& services, std::size_t thread) {
    auto& state = m_Threads[thread];
    if (m_Faulted) return m_Fault;
    if (!state.Active || (state.Waiting && state.TimeMs < state.WakeTimeMs)) return {NativeScriptStatus::Waiting, state.IP, 0, 0, {}, thread};
    if (m_Pending && m_PendingThread != thread) return {NativeScriptStatus::Error, state.IP, 0, 0, "another thread owns pending service", thread};
    Instruction d;
    std::string error;
    if (m_PendingInstruction) {
        d = *m_PendingInstruction; // timer-backed operands must not change on poll
    } else if (!Decode(thread, state.IP, d, error)) {
        return Fail(thread, NativeScriptLookupSchema(d.Opcode) ? NativeScriptStatus::Error : NativeScriptStatus::Unsupported, uint16(d.Opcode | (d.Negated ? 0x8000 : 0)), error);
    }
    const int32 a = d.Int(0), b = d.Int(1);
    const auto rawOpcode = uint16(d.Opcode | (d.Negated ? 0x8000 : 0));
    const auto* schema = NativeScriptLookupSchema(d.Opcode);
    if (!schema || schema->Semantics != NativeScriptSemanticCoverage::Implemented)
        return Fail(thread, NativeScriptStatus::Unsupported, rawOpcode, "opcode form classified; semantics unsupported");
    auto invalid = [&](std::string message) { return Fail(thread, NativeScriptStatus::Error, rawOpcode, std::move(message)); };
    const auto conditionOpcode=[](std::uint16_t opcode){switch(opcode){
    case 0x0038:case 0x0039:case 0x003A:case 0x003B:case 0x003C:case 0x04A3:case 0x04A4:case 0x07D6:
    case 0x0018:case 0x0019:case 0x001A:case 0x001B:case 0x001C:case 0x001D:case 0x001E:case 0x001F:
    case 0x0028:case 0x0029:case 0x002A:case 0x002B:case 0x002C:case 0x002F:
    case 0x0020:case 0x0021:case 0x0022:case 0x0023:case 0x0024:case 0x0025:case 0x0030:case 0x0043:
    case 0x0846:case 0x016B:case 0x08B4:case 0x08B5:case 0x08B6:case 0x0256:case 0x056A:case 0x02E9:
    case 0x06B9:case 0x0118:case 0x0112:case 0x0445:case 0x00A3:case 0x00EC:case 0x00FE:case 0x00FF:
    case 0x03CA:case 0x075C:case 0x0965:case 0x03B0:case 0x0491:case 0x023D:case 0x0248:case 0x07C1:case 0x0A0F:case 0x09C8:case 0x08AB:case 0x0471:
    case 0x09AE:case 0x04C8:case 0x04A7:case 0x09E7:case 0x00DD:case 0x00DF:case 0x03EE:case 0x0214:case 0x0424:case 0x01F3:case 0x0103:case 0x0119:case 0x060E:case 0x03D0:case 0x03D2:return true;
    default:return false;}};
    if(d.Negated&&!conditionOpcode(d.Opcode))return invalid("NOT prefix requires a condition opcode");
    std::vector<uint8> mission;
    std::optional<NativeScriptThreadState> newThread;
    std::optional<NativeScriptPosition> objectCoordinates;
    std::optional<float> objectHeading;
    std::optional<uint32> switchTarget;
    bool switchActivate = false, switchClear = false;
    int32 switchValue = 0, switchRemaining = 0, switchDefault = 0;
    bool switchHasDefault = false;
    int32 streamedLaunch = -1;
    uint32 arithmeticResult = 0;
    uint32 statResult = 0;
    uint32 streamedUsersResult = 0;
    bool streamedLoadedResult = false;
    uint32 progressResult = 0;
    if (d.Opcode == 0x0652) {
        if (a < 120 || std::size_t(a - 120) >= m_State.IntStats.size()) {
            return invalid("integer stat ID outside source storage");
        }
        statResult = uint32(m_State.IntStats[std::size_t(a - 120)]);
    }
    if (d.Opcode == 0x0926) {
        if (a < 0 || std::size_t(a) >= m_StreamedStates.size()) {
            return invalid("streamed-script user query index out of bounds");
        }
        streamedUsersResult = m_StreamedStates[std::size_t(a)].Users;
    }
    if (d.Opcode == 0x08AB) {
        if (a < 0 || std::size_t(a) >= m_StreamedStates.size()) {
            return invalid("streamed-script loaded query index out of bounds");
        }
        streamedLoadedResult = m_StreamedStates[std::size_t(a)].Loaded;
    }
    if (d.Opcode == 0x058C) {
        const float made = m_State.FloatStats[0], total = m_State.FloatStats[1];
        const float percentage = total > 0.0f ? made / total * 100.0f : 0.0f;
        if (!std::isfinite(made) || !std::isfinite(total) || !std::isfinite(percentage)) {
            return invalid("progress percentage is nonfinite");
        }
        progressResult = std::bit_cast<uint32>(percentage);
    }
    if (d.Opcode == 0x008C) {
        if (d.Tags[0] != 2 || d.RawTags[1] != 2 || !FitsInt(d.Float(1))) {
            return invalid("008C requires global int/float variables and finite int32 conversion");
        }
        arithmeticResult = uint32(int32(d.Float(1)));
    }
    if (d.Opcode == 0x008D) {
        if (d.Tags[0] != 2 || d.RawTags[1] != 2) return invalid("008D requires global float/int variables");
        const float converted = float(b);
        if (!std::isfinite(converted)) return invalid("008D conversion is nonfinite");
        arithmeticResult = std::bit_cast<uint32>(converted);
    }
    if (d.Opcode >= 0x0008 && d.Opcode <= 0x0017) {
        if (d.OutputGlobal != ((d.Opcode & 2) == 0)) return invalid("arithmetic variable bank mismatch");
        const unsigned operation = (d.Opcode - 0x0008) / 4;
        if (d.Opcode & 1) {
            const float lhs = std::bit_cast<float>(d.OutputValue), rhs = d.Float(1);
            if (operation == 3 && rhs == 0) return invalid("float division by zero");
            const float value = operation == 0 ? lhs + rhs : operation == 1 ? lhs - rhs : operation == 2 ? lhs * rhs : lhs / rhs;
            if (!std::isfinite(value)) return invalid("float arithmetic overflow");
            arithmeticResult = std::bit_cast<uint32>(value);
        } else {
            // Wider signed intermediates: no native signed-overflow/INT_MIN/-1 UB.
            // This bounded VM faults rather than relying on x86 wrapping/traps.
            const int64 lhs = std::bit_cast<int32>(d.OutputValue), rhs = b;
            if (operation == 3 && !rhs) return invalid("integer division by zero");
            const int64 value = operation == 0 ? lhs + rhs : operation == 1 ? lhs - rhs : operation == 2 ? lhs * rhs : lhs / rhs;
            if (value < std::numeric_limits<int32>::min() || value > std::numeric_limits<int32>::max()) return invalid("integer arithmetic overflow");
            arithmeticResult = uint32(int32(value));
        }
    }
    const bool variableArithmetic =
        (d.Opcode >= 0x0058 && d.Opcode <= 0x0065) || d.Opcode == 0x0068 || d.Opcode == 0x006A ||
        d.Opcode == 0x006B || d.Opcode == 0x006D || d.Opcode == 0x006F || d.Opcode == 0x0072 || d.Opcode == 0x0073;
    if (variableArithmetic) {
        const bool floating = d.Opcode == 0x0059 || d.Opcode == 0x005B || d.Opcode == 0x005D ||
            d.Opcode == 0x005F || d.Opcode == 0x0061 || d.Opcode == 0x0063 || d.Opcode == 0x0065 ||
            d.Opcode == 0x006B || d.Opcode == 0x006D || d.Opcode == 0x006F || d.Opcode == 0x0073;
        const bool localDestination = d.Opcode == 0x005A || d.Opcode == 0x005B || d.Opcode == 0x005C ||
            d.Opcode == 0x005D || d.Opcode == 0x0062 || d.Opcode == 0x0063 || d.Opcode == 0x0064 ||
            d.Opcode == 0x0065 || d.Opcode == 0x006A || d.Opcode == 0x006B || d.Opcode == 0x006F ||
            d.Opcode == 0x0072 || d.Opcode == 0x0073;
        const bool localSource = d.Opcode == 0x005A || d.Opcode == 0x005B || d.Opcode == 0x005E ||
            d.Opcode == 0x005F || d.Opcode == 0x0062 || d.Opcode == 0x0063 || d.Opcode == 0x006A ||
            d.Opcode == 0x006B || d.Opcode == 0x006D || d.Opcode == 0x0072 || d.Opcode == 0x0073;
        if (d.Tags[0] != (localDestination ? 3 : 2) || d.Tags[1] != (localSource ? 3 : 2)) {
            return invalid("variable arithmetic bank mismatch");
        }
        const bool subtract = d.Opcode >= 0x0060 && d.Opcode <= 0x0065;
        const bool multiply = d.Opcode == 0x0068 || d.Opcode == 0x006A || d.Opcode == 0x006B ||
            d.Opcode == 0x006D || d.Opcode == 0x006F;
        const bool divide = d.Opcode == 0x0072 || d.Opcode == 0x0073;
        if (floating) {
            const float lhs = std::bit_cast<float>(d.OutputValue), rhs = d.Float(1);
            if (divide && rhs == 0.0f) return invalid("float division by zero");
            const float value = subtract ? lhs - rhs : multiply ? lhs * rhs : divide ? lhs / rhs : lhs + rhs;
            if (!std::isfinite(value)) return invalid("float arithmetic overflow");
            arithmeticResult = std::bit_cast<uint32>(value);
        } else {
            const int64 lhs = std::bit_cast<int32>(d.OutputValue), rhs = b;
            if (divide && !rhs) return invalid("integer division by zero");
            const int64 value = subtract ? lhs - rhs : multiply ? lhs * rhs : divide ? lhs / rhs : lhs + rhs;
            if (value < std::numeric_limits<int32>::min() || value > std::numeric_limits<int32>::max()) {
                return invalid("integer arithmetic overflow");
            }
            arithmeticResult = uint32(int32(value));
        }
    }
    if (d.Opcode == 0x08BA || d.Opcode == 0x08BB || d.Opcode == 0x08BC ||
        d.Opcode == 0x08C0 || d.Opcode == 0x08C1 || d.Opcode == 0x08C2) {
        if (d.Tags[0] != 2 || b < 0 || b >= 32) return invalid("invalid global bit mutation");
        const bool constant = d.Opcode == 0x08BA || d.Opcode == 0x08C0;
        const bool global = d.Opcode == 0x08BB || d.Opcode == 0x08C1;
        if ((constant && d.RawTags[1] != 1 && d.RawTags[1] != 4 && d.RawTags[1] != 5) ||
            (global && d.RawTags[1] != 2) || (!constant && !global && d.RawTags[1] != 3)) {
            return invalid("global bit index bank mismatch");
        }
        const uint32 mask = uint32(1) << unsigned(b);
        const uint32 old = d.OutputValue;
        arithmeticResult = d.Opcode >= 0x08C0 ? old & ~mask : old | mask;
    }
    if (d.Opcode == 0x08B4 || d.Opcode == 0x08B5 || d.Opcode == 0x08B6) {
        const bool validIndex = d.Opcode == 0x08B4
            ? d.RawTags[1] == 1 || d.RawTags[1] == 4 || d.RawTags[1] == 5
            : d.RawTags[1] == (d.Opcode == 0x08B5 ? 2 : 3);
        if (d.RawTags[0] != 2 || !validIndex || b < 0 || b >= 32) {
            return invalid("global bit query bank/index mismatch");
        }
    }
    switch (d.Opcode) {
    case 0x0317:
        if (m_State.IntStats[146 - 120] == std::numeric_limits<int32>::max())
            return invalid("mission-attempt stat overflow");
        break;
    case 0x0001:
    case 0x016A:
        if (a < 0 || uint64(state.TimeMs) + uint32(a) > std::numeric_limits<uint32>::max())
            return invalid("negative/overflowing duration");
        if (d.Opcode == 0x016A && b != 0 && b != 1) return invalid("invalid fade direction");
        break;
    case 0x004D:
        if (state.Condition) break; // source doesn't evaluate PC for an untaken branch
        [[fallthrough]];
    case 0x0002:
        if (!IsTarget(thread, a)) return invalid("GOTO target is not a supported instruction boundary: " + m_TargetError);
        for (unsigned i = 0; i < m_Headers.size(); ++i) {
            if (state.IP == m_Headers[i] && uint32(a) != m_HeaderTargets[i]) return invalid("modified SCM header target");
        }
        break;
    case 0x004F:
    case 0x00D7:
        if (a < 0 || uint32(a) >= MainCapacity || !IsTarget(thread, a))
            return invalid("new main-script target is not an instruction boundary: " + m_TargetError);
        if (m_Threads.size() >= 96 && m_Idle.empty()) return invalid("script thread pool exhausted");
        newThread.emplace();
        newThread->IP = uint32(a);
        newThread->TimeMs = state.TimeMs;
        newThread->Active = true;
        if (!m_Idle.empty()) newThread->Generation = m_Threads[m_Idle.back()].Generation + 1;
        if (d.Opcode == 0x004F) {
            for (unsigned i = d.FixedOperandCount; i < d.OperandCount; ++i)
                newThread->Locals[i - d.FixedOperandCount] = d.Values[i];
        }
        break;
    case 0x0050:
        if (state.StackDepth >= state.ReturnStack.size()) return invalid("script return stack overflow");
        if (!IsTarget(thread, a)) return invalid("GOSUB target is not a supported instruction boundary: " + m_TargetError);
        break;
    case 0x0051:
        if (!state.StackDepth) return invalid("script return stack underflow");
        break;
    case 0x0871: {
        const int32 cases = d.Int(1), hasDefault = d.Int(2);
        if (cases < 0 || cases > 75 || (hasDefault != 0 && hasDefault != 1)) {
            return invalid("unsupported switch-start case count/default");
        }
        const int32 inThisInstruction = std::min(cases, 7);
        for (int32 i = 0; i < inThisInstruction; ++i) {
            if (a == d.Int(4 + unsigned(i) * 2)) {
                const int32 label = d.Int(5 + unsigned(i) * 2);
                if (!IsTarget(thread, label)) return invalid("switch case target is not an instruction boundary: " + m_TargetError);
                switchTarget = Target(thread, label);
                break;
            }
        }
        if (!switchTarget && cases > inThisInstruction) {
            switchActivate = true;
            switchValue = a;
            switchRemaining = cases - inThisInstruction;
            switchDefault = d.Int(3);
            switchHasDefault = hasDefault != 0;
        } else if (!switchTarget && hasDefault) {
            const int32 label = d.Int(3);
            if (!IsTarget(thread, label)) return invalid("switch default target is not an instruction boundary: " + m_TargetError);
            switchTarget = Target(thread, label);
        }
        if (switchTarget || cases <= inThisInstruction) switchClear = true;
        break;
    }
    case 0x0872: {
        if (!state.SwitchActive || state.SwitchRemaining <= 0)
            return invalid("switch continuation has no active switch");
        const int32 inThisInstruction = std::min(state.SwitchRemaining, 9);
        for (int32 i = 0; i < inThisInstruction; ++i) {
            if (state.SwitchValue == d.Int(unsigned(i) * 2)) {
                const int32 label = d.Int(1 + unsigned(i) * 2);
                if (!IsTarget(thread, label)) return invalid("switch continuation target is not an instruction boundary: " + m_TargetError);
                switchTarget = Target(thread, label);
                break;
            }
        }
        switchRemaining = state.SwitchRemaining - inThisInstruction;
        if (switchTarget) switchClear = true;
        else if (switchRemaining == 0) {
            if (state.SwitchHasDefault) {
                if (!IsTarget(thread, state.SwitchDefault)) return invalid("switch default target is not an instruction boundary: " + m_TargetError);
                switchTarget = Target(thread, state.SwitchDefault);
            }
            switchClear = true;
        }
        break;
    }
    case 0x042C: case 0x030D: case 0x0997:
        if (a < 0) return invalid("negative startup total");
        break;
    case 0x01F0:
        if (a < 0 || a > 6) return invalid("wanted limit out of range");
        break;
    case 0x0111:
        if (a != 0 && a != 1) return invalid("invalid death/arrest Boolean");
        break;
    case 0x00C0:
        if (a < 0 || a >= 24 || b < 0 || b >= 60) return invalid("unsupported non-normalized clock input");
        break;
    case 0x0629: case 0x062A:
        if (!ValidStat(a)) return invalid("invalid stat ID");
        if (d.Opcode == 0x062A && a >= 120 && !FitsInt(d.Float(1))) return invalid("stat integer conversion overflow");
        if (d.Opcode == 0x0629 && a >= 120 && !FitsInt(float(b))) return invalid("stat int/float roundtrip overflow");
        break;
    case 0x0053: case 0x07AF: case 0x01F5:
        if (a < 0 || a > 1) return invalid("invalid player index");
        break;
    case 0x0004: case 0x0005: case 0x0006: case 0x0007:
        if (d.OutputGlobal != (d.Opcode <= 0x0005)) return invalid("assignment variable bank mismatch");
        break;
    case 0x0084:
        if (d.Tags[0] != 2 || d.RawTags[1] != 2) return invalid("0084 requires global source and destination");
        break;
    case 0x0085:
        if (d.Tags[0] != 3 || d.RawTags[1] != 3) return invalid("0085 requires local source and destination");
        break;
    case 0x008A:
        if (d.Tags[0] != 2 || d.Tags[1] != 3) return invalid("008A requires global destination and local source");
        break;
    case 0x008B:
        if (d.Tags[0] != 3 || d.Tags[1] != 2) return invalid("008B requires local destination and global source");
        break;
    case 0x0086:
        // The pinned SA schema and BasicCommands::AssignTo<float,float> both
        // require global float variables. Global arrays retain the same bank.
        if (d.Tags[0] != 2 || d.Tags[1] != 2) return invalid("0086 global float variable bank mismatch");
        break;
    case 0x00D6:
        if (a < 0 || (a > 7 && (a < 21 || a > 27))) return invalid("invalid IF condition count");
        break;
    case 0x0746:
        if (a < 0 || a >= 5 || b < 0 || b >= 32 || d.Int(2) < 0 || d.Int(2) >= 32) return invalid("invalid relationship category/ped type");
        break;
    case 0x0417: {
        if (a < 0 || std::size_t(a) >= m_Metadata.MissionOffsets.size()) return invalid("mission index out of bounds");
        if (m_State.AlreadyRunningMission) return invalid("mission storage already owned by active mission");
        if (m_Threads.size() >= 96 && m_Idle.empty()) return invalid("script thread pool exhausted");
        const auto start = m_Metadata.MissionOffsets[a];
        const auto end = std::size_t(a) + 1 < m_Metadata.MissionOffsets.size() ? m_Metadata.MissionOffsets[a + 1] : uint32(m_Payload.size());
        mission.assign(m_Payload.begin() + start, m_Payload.begin() + end);
        newThread.emplace();
        newThread->IP = newThread->BaseIP = MainCapacity;
        newThread->TimeMs = state.TimeMs;
        newThread->Active = newThread->UsesMissionCleanup = newThread->ThisMustBeTheOnlyMissionRunning = true;
        newThread->MissionIndex = a;
        if (!m_Idle.empty()) newThread->Generation = m_Threads[m_Idle.back()].Generation + 1;
        newThread->Locals.assign(1024, 0); // WipeLocalVariableMemoryForMissionScript
        break;
    }
    case 0x0913: {
        if (a < 0 || std::size_t(a) >= m_StreamedStates.size()) return invalid("streamed script ID out of bounds");
        auto& streamed = m_StreamedStates[std::size_t(a)];
        if (!streamed.Loaded || m_StreamedPayloads[std::size_t(a)].empty()) return invalid("streamed script is not loaded");
        if (streamed.Users >= std::numeric_limits<uint8>::max()) return invalid("streamed script user count overflow");
        if (m_Threads.size() >= 96 && m_Idle.empty()) return invalid("script thread pool exhausted");
        newThread.emplace();
        newThread->BaseIP = ExternalBase + uint32(a) * ExternalStride;
        newThread->IP = newThread->BaseIP;
        newThread->TimeMs = state.TimeMs;
        newThread->Active = newThread->IsExternal = true;
        newThread->StreamedIndex = a;
        newThread->StreamedGeneration = streamed.Generation;
        if (!m_Idle.empty()) newThread->Generation = m_Threads[m_Idle.back()].Generation + 1;
        for (unsigned i = d.FixedOperandCount; i < d.OperandCount; ++i)
            newThread->Locals[i - d.FixedOperandCount] = d.Values[i];
        streamedLaunch = a;
        break;
    }
    case 0x004E:
        if (state.IsExternal && (state.StreamedIndex < 0 ||
            std::size_t(state.StreamedIndex) >= m_StreamedStates.size() ||
            !m_StreamedStates[std::size_t(state.StreamedIndex)].Loaded ||
            m_StreamedStates[std::size_t(state.StreamedIndex)].Generation != state.StreamedGeneration ||
            !m_StreamedStates[std::size_t(state.StreamedIndex)].Users))
            return invalid("external thread has no matching streamed-script user");
        break;
    default: break;
    }

    int32 reference = -1;
    bool pickupCollected = false;
    bool booleanResult = false;
    int32 integerResult = 0, integerResult2 = 0;
    std::array<int32, 3> statsTimes{};
    std::array<float, 3> statsDistances{};
    std::array<char, 16> stringResult{};
    if (d.Opcode == 0x03D2 || d.Opcode == 0x03D1 || d.Opcode == 0x03D5 || d.Opcode == 0x009A || d.Opcode == 0x0A09 || d.Opcode == 0x03D0 || d.Opcode == 0x03CF || d.Opcode == 0x0296 || d.Opcode == 0x009B || d.Opcode == 0x00A6 || d.Opcode == 0x05D3 || d.Opcode == 0x0622 || d.Opcode == 0x00BE || d.Opcode == 0x040D || d.Opcode == 0x02EB || d.Opcode == 0x0925 || d.Opcode == 0x092F || d.Opcode == 0x0930 || d.Opcode == 0x0936 || d.Opcode == 0x0920 || d.Opcode == 0x099C || d.Opcode == 0x015A || d.Opcode == 0x00BC || d.Opcode == 0x0707 || d.Opcode == 0x0701 || d.Opcode == 0x0955 || d.Opcode == 0x05D1 || d.Opcode == 0x0560 || d.Opcode == 0x01C2 || d.Opcode == 0x01C3 || d.Opcode == 0x09B2 || d.Opcode == 0x06D8 || d.Opcode == 0x06DC || d.Opcode == 0x06DD || d.Opcode == 0x06D9 || d.Opcode == 0x0954 || d.Opcode == 0x099A || d.Opcode == 0x05EB || d.Opcode == 0x060E || d.Opcode == 0x0119 || d.Opcode == 0x02A3 || d.Opcode == 0x0103 || d.Opcode == 0x01F3 || d.Opcode == 0x0109 || d.Opcode == 0x04FC || d.Opcode == 0x03C0 || d.Opcode == 0x015F || d.Opcode == 0x0160 || d.Opcode == 0x0430 || d.Opcode == 0x0129 || d.Opcode == 0x01C8 || d.Opcode == 0x067F || d.Opcode == 0x00A5 || d.Opcode == 0x0175 || d.Opcode == 0x06D7 || d.Opcode == 0x01EB || d.Opcode == 0x03DE || d.Opcode == 0x09C8 || d.Opcode == 0x0952 || d.Opcode == 0x0953 || d.Opcode == 0x094B || d.Opcode == 0x02E4 || d.Opcode == 0x02E7 || d.Opcode == 0x02E9 || d.Opcode == 0x02EA || d.Opcode == 0x056A || d.Opcode == 0x06B9 || d.Opcode == 0x033E || d.Opcode == 0x060D || d.Opcode == 0x033F || d.Opcode == 0x0340 || d.Opcode == 0x0341 || d.Opcode == 0x0342 || d.Opcode == 0x0343 || d.Opcode == 0x0344 || d.Opcode == 0x0345 || d.Opcode == 0x0348 || d.Opcode == 0x0395 || d.Opcode == 0x0A0B || d.Opcode == 0x0A0F || d.Opcode == 0x03E6 || d.Opcode == 0x08E1 || d.Opcode == 0x0349 || d.Opcode == 0x03E0 || d.Opcode == 0x00A3 || d.Opcode == 0x090F || d.Opcode == 0x0491 || d.Opcode == 0x023C || d.Opcode == 0x023D || d.Opcode == 0x0247 || d.Opcode == 0x0248 || d.Opcode == 0x0249 || d.Opcode == 0x07C0 || d.Opcode == 0x07C1 || d.Opcode == 0x08A9 || d.Opcode == 0x03B0 || d.Opcode == 0x0842 || d.Opcode == 0x09FB || d.Opcode == 0x07D0 || d.Opcode == 0x077E || d.Opcode == 0x08F8 || d.Opcode == 0x0118 || d.Opcode == 0x03F0 || d.Opcode == 0x054C || d.Opcode == 0x0112 || d.Opcode == 0x01B7 || d.Opcode == 0x01B4 || d.Opcode == 0x04BB || d.Opcode == 0x0169 || d.Opcode == 0x0793 || d.Opcode == 0x070D || d.Opcode == 0x087B || d.Opcode == 0x0471 || d.Opcode == 0x03CA || d.Opcode == 0x00EC || d.Opcode == 0x00FE || d.Opcode == 0x00FF || d.Opcode == 0x09E8 || d.Opcode == 0x0445 || d.Opcode == 0x09AE ||
        d.Opcode == 0x04C8 || d.Opcode == 0x04A7 || d.Opcode == 0x09E7 || d.Opcode == 0x00DD ||
        d.Opcode == 0x00DF || d.Opcode == 0x03EE) {
        const NativeScriptRequestId id{m_SessionId, m_CommandSequence + 1, state.IP};
        NativeScriptServiceResult result;
        struct Guard { bool& Flag; Guard(bool& flag): Flag(flag) { Flag = true; } ~Guard() { Flag = false; } } guard{m_InService};
        try {
            if (d.Opcode == 0x03D2) {
                auto queried = services.HasMissionAudioFinished(id, a);
                result = std::move(queried.Result);
                booleanResult = queried.Value;
            } else if (d.Opcode == 0x03D1) {
                result = services.PlayMissionAudio(id, a);
            } else if (d.Opcode == 0x03D5) {
                NativeScriptMissionTextRequest request{id, {}};
                std::copy_n(d.Strings[0].begin(), request.Name.size(), request.Name.begin());
                result = services.ClearText(request);
            } else if (d.Opcode == 0x009A) {
                auto created = services.CreatePed({id, a, b,
                    {d.Float(2), d.Float(3), d.Float(4)}});
                result = std::move(created.Result);
                reference = created.Reference.Value;
            } else if (d.Opcode == 0x0A09) {
                if (b != 0 && b != 1) return invalid("script speech switch requires a Boolean");
                result = services.SetPedSpeechDisabled({id, {a}, b != 0});
            } else if (d.Opcode == 0x03D0) {
                auto queried = services.HasMissionAudioLoaded(id, a);
                result = std::move(queried.Result);
                booleanResult = queried.Value;
            } else if (d.Opcode == 0x03CF) {
                result = services.LoadMissionAudio({id, a, b});
            } else if (d.Opcode == 0x0296) {
                result = services.UnloadSpecialCharacter({id, a, {}});
            } else if (d.Opcode == 0x009B) {
                result = services.DeletePed({id, {a}});
            } else if (d.Opcode == 0x00A6) {
                result = services.DeleteVehicle({id, {a}, 0});
            } else if (d.Opcode == 0x05D3) {
                result = services.AssignGoStraightTask({id, {a},
                    {d.Float(1), d.Float(2), d.Float(3)}, d.Int(4), d.Int(5)});
            } else if (d.Opcode == 0x0622) {
                result = services.TaskLeaveVehicleImmediately({id, {a}, {b}, -1});
            } else if (d.Opcode == 0x00BE) {
                result = services.ClearPrints(id);
            } else if (d.Opcode == 0x040D) {
                result = services.ClearMissionAudio(id, a);
            } else if (d.Opcode == 0x02EB || d.Opcode == 0x0925 || d.Opcode == 0x092F || d.Opcode == 0x0930 ||
                d.Opcode == 0x0936 || d.Opcode == 0x0920 || d.Opcode == 0x099C || d.Opcode == 0x015A) {
                NativeScriptCameraCommandRequest request{id, d.Opcode};
                if (d.Opcode == 0x0936 || d.Opcode == 0x0920) {
                    for (unsigned i = 0; i < 6; ++i) request.Floats[i] = d.Float(i);
                    request.Integers = {d.Int(6), d.Int(7)};
                } else if (d.Opcode == 0x099C) {
                    request.Integers[0] = a;
                    request.Floats[0] = d.Float(1);
                    request.Floats[1] = d.Float(2);
                } else if (d.Opcode == 0x092F || d.Opcode == 0x0930) request.Integers[0] = a;
                result = services.ApplyCameraCommand(request);
            } else if (d.Opcode == 0x00BC) {
                NativeScriptPrintRequest request{id, {}, b, d.Int(2)};
                std::copy_n(d.Strings[0].begin(), request.Key.size(), request.Key.begin());
                result = services.PrintNow(request);
            } else if (d.Opcode == 0x0707) {
                result = services.BeginSkippableCutscene({id, a});
            } else if (d.Opcode == 0x0701) {
                result = services.EndSkippableCutscene(id);
            } else if (d.Opcode == 0x0955) {
                result = services.StopBeatTrack(id);
            } else if (d.Opcode == 0x05D1) {
                result = services.AssignCarDriveTask({id, {a}, {b},
                    {d.Float(2), d.Float(3), d.Float(4)}, d.Float(5),
                    d.Int(6), d.Int(7), d.Int(8)});
            } else if (d.Opcode == 0x0560) {
                auto created = services.CreateRandomDriver({id, {a}, 0});
                result = std::move(created.Result);
                reference = created.Reference.Value;
            } else if (d.Opcode == 0x01C2) {
                result = services.MarkPedNoLongerNeeded({id, {a}});
            } else if (d.Opcode == 0x01C3) {
                result = services.MarkVehicleNoLongerNeeded({id, {a}, 0});
            } else if (d.Opcode == 0x09B2) {
                if (a != 0 && a != 1) return invalid("random car model query requires a Boolean");
                auto queried = services.GetRandomResidentCarModel(id, a != 0);
                result = std::move(queried.Result);
                integerResult = queried.ModelId;
                integerResult2 = queried.VehicleClass;
            } else if (d.Opcode == 0x06D8) {
                auto created = services.CreateMissionTrain({id, a,
                    {d.Float(1), d.Float(2), d.Float(3)}, d.Int(4) != 0});
                result = std::move(created.Result);
                reference = created.Reference.Value;
            } else if (d.Opcode == 0x06DC || d.Opcode == 0x06DD) {
                result = services.SetTrainSpeed({id, {a}, d.Float(1)}, d.Opcode == 0x06DD);
            } else if (d.Opcode == 0x06D9) {
                result = services.DeleteMissionTrains(id);
            } else if (d.Opcode == 0x0954) {
                result = services.PlayBeatTrack(id);
            } else if (d.Opcode == 0x05EB) {
                result = services.StartVehiclePlayback({id, {a}, b});
            } else if (d.Opcode == 0x099A) {
                if (b != 0 && b != 1) return invalid("vehicle collision switch requires a Boolean");
                result = services.SetVehicleCollision({id, {a}, b});
            } else if (d.Opcode == 0x060E) {
                auto queried = services.IsVehiclePlaybackActive({id, {a}, 0});
                result = std::move(queried.Result);
                booleanResult = queried.Value;
            } else if (d.Opcode == 0x0119) {
                auto queried = services.IsVehicleDead({id, {a}, 0});
                result = std::move(queried.Result);
                booleanResult = queried.Value;
            } else if (d.Opcode == 0x02A3) {
                if (a != 0 && a != 1) return invalid("widescreen switch requires a Boolean");
                result = services.SetWidescreen({id, a != 0});
            } else if (d.Opcode == 0x0103) {
                auto queried = services.LocateChar({id, {a},
                    {d.Float(1), d.Float(2), d.Float(3)},
                    {d.Float(4), d.Float(5), d.Float(6)}, false, d.Int(7) != 0, false, true});
                result = std::move(queried.Result);
                booleanResult = queried.Value;
            } else if (d.Opcode == 0x01F3) {
                auto queried = services.IsVehicleInAirProper({id, {a}, 0});
                result = std::move(queried.Result);
                booleanResult = queried.Value;
            } else if (d.Opcode == 0x0109) {
                result = services.AddScore({id, a, b});
            } else if (d.Opcode == 0x04FC) {
                auto queried = services.GetWheelieStats({id, {a}, 0});
                result = std::move(queried.Result);
                statsTimes = queried.Times;
                statsDistances = queried.Distances;
            } else if (d.Opcode == 0x03C0) {
                auto queried = services.GetPedVehicleNoSave({id, {a}});
                result = std::move(queried.Result);
                reference = queried.Reference.Value;
            } else if (d.Opcode == 0x015F) {
                result = services.SetFixedCameraPosition({id,
                    {d.Float(0), d.Float(1), d.Float(2)},
                    {d.Float(3), d.Float(4), d.Float(5)}});
            } else if (d.Opcode == 0x0160) {
                result = services.PointCameraAtPoint({id,
                    {d.Float(0), d.Float(1), d.Float(2)}, d.Int(3)});
            } else if (d.Opcode == 0x0430) {
                result = services.WarpPedIntoVehiclePassenger({id, {a}, {b}, d.Int(2)});
            } else if (d.Opcode == 0x0129 || d.Opcode == 0x01C8) {
                auto created = services.CreatePedInsideVehicle({id, {a}, b, d.Int(2),
                    d.Opcode == 0x01C8 ? d.Int(3) : -1});
                result = std::move(created.Result);
                reference = created.Reference.Value;
            } else if (d.Opcode == 0x067F) {
                if (b < 0 || b > 2) return invalid("vehicle lights require a source override state");
                result = services.SetVehicleLights({id, {a}, b});
            } else if (d.Opcode == 0x00A5) {
                auto created = services.CreateVehicle({id, a, {d.Float(1), d.Float(2), d.Float(3)}});
                result = std::move(created.Result);
                reference = created.Reference.Value;
            } else if (d.Opcode == 0x0175) {
                result = services.SetVehicleHeading({id, {a}, d.Float(1)});
            } else if (d.Opcode == 0x06D7) {
                if (a != 0 && a != 1) return invalid("random-train switch requires a Boolean");
                result = services.SetRandomTrains({id, a != 0});
            } else if (d.Opcode == 0x01EB || d.Opcode == 0x03DE) {
                result = services.SetDensityMultiplier({id, d.Float(0), d.Opcode == 0x01EB});
            } else if (d.Opcode == 0x09C8) {
                auto queried = services.AreSubtitlesEnabled(id);
                result = std::move(queried.Result);
                booleanResult = queried.Value;
            } else if (d.Opcode == 0x0952) {
                result = services.PreloadBeatTrack({id, a});
            } else if (d.Opcode == 0x0953) {
                auto queried = services.GetBeatTrackStatus(id);
                result = std::move(queried.Result);
                integerResult = queried.Value;
            } else if (d.Opcode == 0x094B) {
                auto queried = services.GetCharEntryExitName({id, {a}});
                result = std::move(queried.Result);
                stringResult = queried.Value;
            } else if (d.Opcode == 0x02E4) {
                result = services.LoadCutscene({id, d.Text});
            } else if (d.Opcode == 0x02E7) {
                result = services.StartCutscene(id);
            } else if (d.Opcode == 0x06B9) {
                auto queried = services.HasCutsceneLoaded(id);
                result = std::move(queried.Result);
                booleanResult = queried.Value;
            } else if (d.Opcode == 0x02E9) {
                auto queried = services.HasCutsceneFinished(id);
                result = std::move(queried.Result);
                booleanResult = queried.Value;
            } else if (d.Opcode == 0x02EA) {
                result = services.ClearCutscene(id);
            } else if (d.Opcode == 0x056A) {
                auto queried = services.WasCutsceneSkipped(id);
                result = std::move(queried.Result);
                booleanResult = queried.Value;
            } else if (d.Opcode == 0x033E) {
                NativeScriptTextDisplayRequest request{id, d.Float(0), d.Float(1)};
                std::copy_n(d.Strings[2].begin(), request.Key.size(), request.Key.begin());
                result = services.DisplayText(request);
            } else if (d.Opcode == 0x060D || d.Opcode == 0x033F || d.Opcode == 0x0340 || d.Opcode == 0x0341 || d.Opcode == 0x0342 ||
                d.Opcode == 0x0343 || d.Opcode == 0x0344 || d.Opcode == 0x0345 || d.Opcode == 0x0348) {
                result = services.SetTextStyle({id, d.Opcode, {d.Float(0), d.Float(1)},
                    {a, b, d.Int(2), d.Int(3), d.Int(4)}});
            } else if (d.Opcode == 0x0A0F) {
                auto queried = services.HasLanguageChanged(id);
                result = std::move(queried.Result);
                booleanResult = queried.Value;
            } else if (d.Opcode == 0x0A0B) {
                result=services.LoadSceneInDirection({id,{d.Float(0),d.Float(1),d.Float(2)},d.Float(3)});
            } else if (d.Opcode == 0x0395) {
                result=services.ClearArea({id,{d.Float(0),d.Float(1),d.Float(2)},d.Float(3),d.Int(4)!=0});
            } else if (d.Opcode == 0x03E6) {
                result = services.ClearHelp(id);
            } else if (d.Opcode == 0x08E1) {
                auto queried = services.GetNumberTagsTagged(id);
                result = std::move(queried.Result);
                integerResult = queried.Value;
            } else if (d.Opcode == 0x0349) {
                result = services.SetTextFont(id, a);
            } else if (d.Opcode == 0x03E0) {
                result = services.SetTextDrawBeforeFade(id, a != 0);
            } else if (d.Opcode == 0x00A3) {
                const float minX=d.Float(1), minY=d.Float(2), maxX=d.Float(3), maxY=d.Float(4);
                auto queried = services.LocateChar({id,{a},{(minX+maxX)*0.5f,(minY+maxY)*0.5f,0},
                    {(maxX-minX)*0.5f,(maxY-minY)*0.5f,0},false,d.Int(5)!=0,true});
                result = std::move(queried.Result);
                booleanResult = queried.Value;
            } else if (d.Opcode == 0x090F) {
                result = services.MarkStreamedScriptNoLongerNeeded({id, a});
            } else if (d.Opcode == 0x0491) {
                auto queried = services.HasCharGotWeapon({id, {a}, b});
                result = std::move(queried.Result);
                booleanResult = queried.Value;
            } else if(d.Opcode==0x023C){result=services.LoadSpecialCharacter({id,a,d.Text});
            } else if(d.Opcode==0x023D){auto queried=services.HasSpecialCharacterLoaded({id,a,{}});result=std::move(queried.Result);booleanResult=queried.Value;
            } else if(d.Opcode==0x07C0){result=services.RequestCarRecording({id,a});
            } else if(d.Opcode==0x07C1){auto queried=services.HasCarRecordingLoaded({id,a});result=std::move(queried.Result);booleanResult=queried.Value;
            } else if (d.Opcode == 0x0247 || d.Opcode == 0x0248 || d.Opcode == 0x0249) {
                NativeScriptModelRequest request{id, a, {}};
                if (a < 0) {
                    const auto index = uint64(-int64(a));
                    if (index >= m_Metadata.UsedObjects.size()) return invalid("model request used-object index out of bounds");
                    request.UsedObjectName = m_Metadata.UsedObjects[index];
                }
                if (d.Opcode == 0x0247) result = services.RequestModel(request);
                else if(d.Opcode==0x0249)result=services.MarkModelNoLongerNeeded(request);
                else { auto queried=services.HasModelLoaded(request); result=std::move(queried.Result); booleanResult=queried.Value; }
            } else if (d.Opcode == 0x08A9) {
                result = services.StreamScript({id, a});
            } else if (d.Opcode == 0x03B0) {
                auto queried = services.IsGarageOpen({id, d.Text});
                result = std::move(queried.Result);
                booleanResult = queried.Value;
            } else if (d.Opcode == 0x0842) {
                auto queried = services.GetCityPlayerIsIn({id, a});
                result = std::move(queried.Result);
                integerResult = queried.Value;
            } else if (d.Opcode == 0x09FB) {
                auto queried = services.GetCurrentLanguage(id);
                result = std::move(queried.Result);
                integerResult = queried.Value;
            } else if (d.Opcode == 0x07D0) {
                auto queried = services.GetCurrentDayOfWeek(id);
                result = std::move(queried.Result);
                integerResult = queried.Value;
            } else if (d.Opcode == 0x077E) {
                auto queried = services.GetAreaVisible(id);
                result = std::move(queried.Result);
                integerResult = queried.Value;
            } else if (d.Opcode == 0x08F8) {
                result = services.SetUpdateStatsVisible(id, a != 0);
            } else if (d.Opcode == 0x0118) {
                auto queried = services.IsCharDead({id, {a}});
                result = std::move(queried.Result);
                booleanResult = queried.Value;
            } else if (d.Opcode == 0x03F0) {
                result = services.UseTextCommands({id, a != 0});
            } else if (d.Opcode == 0x054C) {
                result = services.LoadMissionText({id, d.Text});
            } else if (d.Opcode == 0x01B7) {
                result = services.ReleaseWeather(id);
            } else if (d.Opcode == 0x01B4) {
                result = services.SetPlayerControl({id, a, b != 0});
            } else if (d.Opcode == 0x04BB) {
                result = services.SetAreaVisible({id, a});
            } else if (d.Opcode == 0x0169) {
                result = services.SetFadeColour({id, a, b, d.Int(2)});
            } else if (d.Opcode == 0x0793) {
                result = services.StoreClothesState(id);
            } else if (d.Opcode == 0x070D) {
                result = services.BuildPlayerModel({id, a});
            } else if (d.Opcode == 0x087B) {
                result = services.GivePlayerClothes({id, a, d.Strings[1], d.Strings[2], d.Int(3)});
            } else if (d.Opcode == 0x0471) {
                auto queried = services.LocateCharObject2D({id,{a},{b},d.Float(2),d.Float(3),d.Int(4) != 0});
                result = std::move(queried.Result);
                booleanResult = queried.Value;
            } else if (d.Opcode == 0x03CA) {
                auto queried = services.DoesObjectExist({id,{a}});
                result = std::move(queried.Result);
                booleanResult = queried.Value;
            } else if (d.Opcode == 0x00EC || d.Opcode == 0x00FE || d.Opcode == 0x00FF) {
                const bool twoDimensional = d.Opcode == 0x00EC;
                auto queried = services.LocateChar({id,{a},
                    {d.Float(1),d.Float(2),twoDimensional ? 0.0f : d.Float(3)},
                    {d.Float(twoDimensional ? 3 : 4),d.Float(twoDimensional ? 4 : 5),twoDimensional ? 0.0f : d.Float(6)},
                    d.Opcode == 0x00FF,d.Int(twoDimensional ? 5 : 7) != 0,twoDimensional});
                result = std::move(queried.Result);
                booleanResult = queried.Value;
            } else if (d.Opcode == 0x09E8) {
                auto queried = services.GetCharAreaVisible({id, {a}});
                result = std::move(queried.Result);
                integerResult = queried.Value;
            } else if (d.Opcode == 0x0112) {
                auto queried = services.HasDeathArrestBeenExecuted(id);
                result = std::move(queried.Result);
                booleanResult = queried.Value;
            } else if (d.Opcode == 0x0445) {
                auto queried = services.AreCarCheatsActivated(id);
                result = std::move(queried.Result);
                booleanResult = queried.Value;
            } else {
                const auto kind = d.Opcode == 0x09AE ? NativeScriptPlayerStateQueryKind::InTrain :
                    d.Opcode == 0x04C8 ? NativeScriptPlayerStateQueryKind::InFlyingVehicle :
                    d.Opcode == 0x04A7 ? NativeScriptPlayerStateQueryKind::InBoat :
                    d.Opcode == 0x00DD ? NativeScriptPlayerStateQueryKind::InVehicleModel :
                    d.Opcode == 0x00DF ? NativeScriptPlayerStateQueryKind::InAnyVehicle :
                    d.Opcode == 0x03EE ? NativeScriptPlayerStateQueryKind::CanStartMission :
                    NativeScriptPlayerStateQueryKind::ControlEnabled;
                auto queried = services.QueryPlayerState({id, a, kind, d.Opcode == 0x00DD ? b : -1});
                result = std::move(queried.Result);
                booleanResult = queried.Value;
            }
        } catch (const std::exception& exception) {
            return Fail(thread, NativeScriptStatus::Error, rawOpcode, "service exception: " + std::string(exception.what()));
        } catch (...) {
            return invalid("unknown service exception");
        }
        switch (result.Status) {
        case NativeScriptServiceStatus::Pending:
            m_Pending = true;
            m_PendingInstruction = d;
            m_PendingThread = thread;
            return {NativeScriptStatus::Pending, state.IP, rawOpcode, 0, std::move(result.Message), thread};
        case NativeScriptServiceStatus::Unsupported:
            return Fail(thread, NativeScriptStatus::Unsupported, rawOpcode, "service unsupported: " + result.Message);
        case NativeScriptServiceStatus::Error:
            return Fail(thread, NativeScriptStatus::Error, rawOpcode, "service failed: " + result.Message);
        case NativeScriptServiceStatus::Ready: break;
        default: return invalid("invalid service result");
        }
    }
    if (d.Opcode == 0x041D || d.Opcode == 0x0391) {
        const NativeScriptRequestId id{m_SessionId, m_CommandSequence + 1, state.IP};
        NativeScriptServiceResult result;
        struct Guard { bool& Flag; Guard(bool& flag): Flag(flag) { Flag = true; } ~Guard() { Flag = false; } } guard{m_InService};
        try {
            if (d.Opcode == 0x0391) {
                result = services.RemoveTextureDictionary(id);
            } else {
                NativeScriptCameraCommandRequest request{id, d.Opcode};
                request.Floats[0] = d.Float(0);
                result = services.ApplyCameraCommand(request);
            }
        } catch (const std::exception& exception) {
            return Fail(thread, NativeScriptStatus::Error, rawOpcode, "service exception: " + std::string(exception.what()));
        } catch (...) {
            return invalid("unknown service exception");
        }
        if (result.Status == NativeScriptServiceStatus::Pending) {
            m_Pending = true; m_PendingInstruction = d; m_PendingThread = thread;
            return {NativeScriptStatus::Pending, state.IP, rawOpcode, 0, std::move(result.Message), thread};
        }
        if (result.Status == NativeScriptServiceStatus::Unsupported)
            return Fail(thread, NativeScriptStatus::Unsupported, rawOpcode, "service unsupported: " + result.Message);
        if (result.Status == NativeScriptServiceStatus::Error)
            return Fail(thread, NativeScriptStatus::Error, rawOpcode, "service failed: " + result.Message);
        if (result.Status != NativeScriptServiceStatus::Ready) return invalid("invalid service result");
    }
    if (d.Opcode == 0x02A8) {
        const NativeScriptRequestId id{m_SessionId, m_CommandSequence + 1, state.IP};
        NativeScriptServiceResult result;
        struct Guard { bool& Flag; Guard(bool& flag): Flag(flag) { Flag = true; } ~Guard() { Flag = false; } } guard{m_InService};
        try {
            auto created = services.CreateCoordinateBlip({id,
                {d.Float(0), d.Float(1), d.Float(2)}, d.Int(3)});
            result = std::move(created.Result);
            reference = created.Reference.Value;
        } catch (const std::exception& exception) {
            return Fail(thread, NativeScriptStatus::Error, rawOpcode, "service exception: " + std::string(exception.what()));
        } catch (...) {
            return invalid("unknown service exception");
        }
        if (result.Status == NativeScriptServiceStatus::Pending) {
            m_Pending = true; m_PendingInstruction = d; m_PendingThread = thread;
            return {NativeScriptStatus::Pending, state.IP, rawOpcode, 0, std::move(result.Message), thread};
        }
        if (result.Status == NativeScriptServiceStatus::Unsupported)
            return Fail(thread, NativeScriptStatus::Unsupported, rawOpcode, "service unsupported: " + result.Message);
        if (result.Status == NativeScriptServiceStatus::Error)
            return Fail(thread, NativeScriptStatus::Error, rawOpcode, "service failed: " + result.Message);
        if (result.Status != NativeScriptServiceStatus::Ready) return invalid("invalid service result");
    }
    if (d.Opcode == 0x0A40 || d.Opcode == 0x0A41) {
        const NativeScriptRequestId id{m_SessionId, m_CommandSequence + 1, state.IP};
        NativeScriptServiceResult result;
        struct Guard { bool& Flag; Guard(bool& flag): Flag(flag) { Flag = true; } ~Guard() { Flag = false; } } guard{m_InService};
        try {
            if (d.Opcode == 0x0A40) {
                auto created = services.CreateUserMarker({id,
                    {d.Float(0), d.Float(1), d.Float(2)}, d.Int(3)});
                result = std::move(created.Result);
                reference = created.Reference.Value;
            } else result = services.RemoveUserMarker({id, {a}});
        } catch (const std::exception& exception) {
            return Fail(thread, NativeScriptStatus::Error, rawOpcode, "service exception: " + std::string(exception.what()));
        } catch (...) {
            return invalid("unknown service exception");
        }
        if (result.Status == NativeScriptServiceStatus::Pending) {
            m_Pending = true; m_PendingInstruction = d; m_PendingThread = thread;
            return {NativeScriptStatus::Pending, state.IP, rawOpcode, 0, std::move(result.Message), thread};
        }
        if (result.Status == NativeScriptServiceStatus::Unsupported)
            return Fail(thread, NativeScriptStatus::Unsupported, rawOpcode, "service unsupported: " + result.Message);
        if (result.Status == NativeScriptServiceStatus::Error)
            return Fail(thread, NativeScriptStatus::Error, rawOpcode, "service failed: " + result.Message);
        if (result.Status != NativeScriptServiceStatus::Ready) return invalid("invalid service result");
    }
    if (d.Opcode == 0x0164 || d.Opcode == 0x0223 || d.Opcode == 0x0330 || d.Opcode == 0x048F ||
        d.Opcode == 0x075C || d.Opcode == 0x0965 || d.Opcode == 0x09F5 || d.Opcode == 0x00A0) {
        const NativeScriptRequestId id{m_SessionId, m_CommandSequence + 1, state.IP};
        NativeScriptServiceResult result;
        struct Guard { bool& Flag; Guard(bool& flag): Flag(flag) { Flag = true; } ~Guard() { Flag = false; } } guard{m_InService};
        try {
            if (d.Opcode == 0x0164) result = services.RemoveBlip({id, {a}});
            else if (d.Opcode == 0x075C) {
                auto queried = services.DoesBlipExist({id, {a}});
                result = std::move(queried.Result);
                booleanResult = queried.Value;
            }
            else if (d.Opcode == 0x0965) {
                auto queried = services.IsPedSwimming({id, {a}});
                result = std::move(queried.Result);
                booleanResult = queried.Value;
            }
            else if (d.Opcode == 0x0223) result = services.SetPedHealth({id, {a}, b});
            else if (d.Opcode == 0x048F) result = services.RemoveAllPedWeapons({id, {a}});
            else if (d.Opcode == 0x00A0) {
                auto queried = services.GetPedCoordinates({id, {a}});
                result = std::move(queried.Result);
                if (result.Status == NativeScriptServiceStatus::Ready) objectCoordinates = queried.Value;
            }
            else if (d.Opcode == 0x0330) {
                if (b != 0 && b != 1) return invalid("never-tired switch requires a Boolean");
                result = services.SetPlayerNeverTired({id, a, b != 0});
            } else {
                if (a != 0 && a != 1) return invalid("speech suppression requires a Boolean");
                result = services.ShutAllCharsUp({id, a != 0});
            }
        } catch (const std::exception& exception) {
            return Fail(thread, NativeScriptStatus::Error, rawOpcode, "service exception: " + std::string(exception.what()));
        } catch (...) {
            return invalid("unknown service exception");
        }
        if (result.Status == NativeScriptServiceStatus::Pending) {
            m_Pending = true; m_PendingInstruction = d; m_PendingThread = thread;
            return {NativeScriptStatus::Pending, state.IP, rawOpcode, 0, std::move(result.Message), thread};
        }
        if (result.Status == NativeScriptServiceStatus::Unsupported)
            return Fail(thread, NativeScriptStatus::Unsupported, rawOpcode, "service unsupported: " + result.Message);
        if (result.Status == NativeScriptServiceStatus::Error)
            return Fail(thread, NativeScriptStatus::Error, rawOpcode, "service failed: " + result.Message);
        if (result.Status != NativeScriptServiceStatus::Ready) return invalid("invalid service result");
    }
    if (d.Opcode == 0x01B6 || d.Opcode == 0x0256 || d.Opcode == 0x09BA || d.Opcode == 0x0363 || d.Opcode == 0x0776 || d.Opcode == 0x0777 || d.Opcode == 0x07D3 || d.Opcode == 0x0884 || d.Opcode == 0x08E8 || d.Opcode == 0x0928 || d.Opcode == 0x0929 || d.Opcode == 0x02A7 || d.Opcode == 0x04E4 || d.Opcode == 0x03CB || d.Opcode == 0x0053 || d.Opcode == 0x07AF || d.Opcode == 0x01F5 || d.Opcode == 0x0373 || d.Opcode == 0x0173 || d.Opcode == 0x0517 || d.Opcode == 0x0518 || d.Opcode == 0x0570 || d.Opcode == 0x04CE || d.Opcode == 0x018B || d.Opcode == 0x09B4 || d.Opcode == 0x02B9 || d.Opcode == 0x02FA || d.Opcode == 0x016C || d.Opcode == 0x016D || d.Opcode == 0x0814 || d.Opcode == 0x029B || d.Opcode == 0x0107 || d.Opcode == 0x0177 || d.Opcode == 0x01C7 || d.Opcode == 0x07F7 || d.Opcode == 0x0550 || d.Opcode == 0x0392 || d.Opcode == 0x09CA || d.Opcode == 0x034D || d.Opcode == 0x0566 || d.Opcode == 0x01BB || d.Opcode == 0x0176 || d.Opcode == 0x0827 || d.Opcode == 0x0381 || d.Opcode == 0x0400 || d.Opcode == 0x0453 || d.Opcode == 0x0A17 || d.Opcode == 0x09E2 || d.Opcode == 0x07FB || d.Opcode == 0x04F8 || d.Opcode == 0x08CA || d.Opcode == 0x0767 || d.Opcode == 0x0874 || d.Opcode == 0x076A || d.Opcode == 0x076C || d.Opcode == 0x09B7 || d.Opcode == 0x0958 || d.Opcode == 0x0959 || d.Opcode == 0x095A || d.Opcode == 0x032B || d.Opcode == 0x01E7 || d.Opcode == 0x01E8 || d.Opcode == 0x091D || d.Opcode == 0x091E || d.Opcode == 0x022A || d.Opcode == 0x022B || d.Opcode == 0x0213 || d.Opcode == 0x0214 || d.Opcode == 0x0215 || d.Opcode == 0x014B || d.Opcode == 0x014C) {
        const NativeScriptRequestId id{m_SessionId, m_CommandSequence + 1, state.IP};
        NativeScriptServiceResult result;
        // All operands/output bounds have been checked before ANY host call.
        struct Guard { bool& Flag; Guard(bool& flag): Flag(flag) { Flag = true; } ~Guard() { Flag = false; } } guard{m_InService};
        try {
            if (d.Opcode == 0x01B6) result = services.ForceWeatherNow({id, a});
            if (d.Opcode == 0x014B) {
                // Pinned schema: model -1 selects a random local-popcycle car,
                // NOT an SCM used-object index. Host owns source constructor
                // narrowing/model checks, including a successful -1 result.
                auto created = services.CreateCarGenerator({id,
                    {d.Float(0), d.Float(1), d.Float(2)}, d.Float(3),
                    d.Int(4), d.Int(5), d.Int(6), d.Int(7), d.Int(8), d.Int(9), d.Int(10), d.Int(11)});
                result = std::move(created.Result); reference = created.Reference.Value;
            }
            if (d.Opcode == 0x09E2) {
                auto created = services.CreateCarGeneratorWithPlate({{id,
                    {d.Float(0), d.Float(1), d.Float(2)}, d.Float(3),
                    d.Int(4), d.Int(5), d.Int(6), d.Int(7), d.Int(8), d.Int(9), d.Int(10), d.Int(11)}, d.Text});
                result = std::move(created.Result); reference = created.Reference.Value;
            }
            if (d.Opcode == 0x014C) {
                // VehicleCommands::SwitchCarGenerator takes int32 count: zero
                // switches off; nonzero switches on, then count<=100 is assigned
                // to uint16 m_nGenerateCount. Negative counts must reach host.
                result = services.SwitchCarGenerator({id, {a}, b});
            }
            if (d.Opcode == 0x0213) {
                NativeScriptPickupRequest request{id, a, b, {d.Float(2), d.Float(3), d.Float(4)}};
                if (a < 0) {
                    const auto index = uint64(-int64(a));
                    if (index >= m_Metadata.UsedObjects.size()) return invalid("pickup used-object index out of bounds");
                    request.UsedObjectName = m_Metadata.UsedObjects[index];
                }
                auto created = services.CreatePickup(request);
                result = std::move(created.Result); reference = created.Reference.Value;
            }
            if (d.Opcode == 0x0958 || d.Opcode == 0x0959 || d.Opcode == 0x095A) {
                auto created = services.CreatePickup({id,d.Opcode==0x0958 ? 1253 : d.Opcode==0x0959 ? 954 : 953,3,{d.Float(0),d.Float(1),d.Float(2)}});
                result=std::move(created.Result); reference=created.Reference.Value;
            }
            if(d.Opcode==0x032B){auto created=services.CreatePickupWithAmmo({id,d.Int(0),d.Int(1),d.Int(2),{d.Float(3),d.Float(4),d.Float(5)}});result=std::move(created.Result);reference=created.Reference.Value;}
            if(d.Opcode==0x01E7||d.Opcode==0x01E8||d.Opcode==0x091D||d.Opcode==0x091E||d.Opcode==0x022A||d.Opcode==0x022B){
                const auto kind=d.Opcode==0x01E7?NativePathPolicyKind::VehicleOn:d.Opcode==0x01E8?NativePathPolicyKind::VehicleOff:
                    d.Opcode==0x091D?NativePathPolicyKind::VehicleOriginal:d.Opcode==0x091E?NativePathPolicyKind::PedOriginal:
                    d.Opcode==0x022A?NativePathPolicyKind::PedOn:NativePathPolicyKind::PedOff;
                result=services.AddPathPolicy({id,kind,{d.Float(0),d.Float(1),d.Float(2),d.Float(3),d.Float(4),d.Float(5)}});
            }
            if(d.Opcode==0x0928||d.Opcode==0x0929){NativeScriptExternalTriggerRequest request{id,d.Int(0),d.Int(1),d.Int(2),d.Opcode==0x0929?d.Int(4):0,d.Opcode==0x0929?d.Float(3):0,{},{d.Opcode==0x0929}};if(request.ModelId<0){const auto index=uint64(-int64(request.ModelId));if(index>=m_Metadata.UsedObjects.size())return invalid("external trigger used-object index out of bounds");request.ModelName=m_Metadata.UsedObjects[index];}result=services.AddExternalScriptTrigger(request);}
            if (d.Opcode == 0x07D3 || d.Opcode == 0x0884) result = services.AddCodeScriptBrain({id, d.Int(0), d.Text, d.Opcode == 0x0884});
            if (d.Opcode == 0x08E8) result = services.AttachAnimsToModel({id, d.Int(0), d.Text});
            if (d.Opcode == 0x0776 || d.Opcode == 0x0777)
                result = services.SetIplRequested({id, d.Strings[0], d.Opcode == 0x0776});
            if (d.Opcode == 0x0363) {
                NativeScriptWorldObjectVisibilityRequest request{id,{d.Float(0),d.Float(1),d.Float(2)},d.Float(3),d.Int(4),{},d.Int(5) != 0};
                if (request.ModelId < 0) {
                    const auto index = uint64(-int64(request.ModelId));
                    if (index >= m_Metadata.UsedObjects.size()) return invalid("world-object visibility used-object index out of bounds");
                    request.ModelName = m_Metadata.UsedObjects[index];
                }
                result = services.SetClosestObjectVisibility(request);
            }
            if (d.Opcode == 0x09BA) result = services.SetZoneNamesVisible(id, a != 0);
            if (d.Opcode == 0x0256) {
                auto queried = services.IsPlayerPlaying({id, a});
                result = std::move(queried.Result);
                booleanResult = queried.Value;
            }
            if (d.Opcode == 0x0214) {
                auto queried = services.HasPickupBeenCollected({id, {a}});
                result = std::move(queried.Result); pickupCollected = queried.Collected;
            }
            if (d.Opcode == 0x0215) result = services.RemoveScriptPickup({id, {a}});
            if (d.Opcode == 0x09B4) result = services.SetEntryExitFlag({id, d.Float(0), d.Float(1), d.Float(2), d.Int(3), d.Int(4)});
            if (d.Opcode == 0x07FB) result = services.SwitchEntryExit({id, d.Text, a != 0});
            if (d.Opcode == 0x04F8) result = services.AddSetPiece({id,a,{d.Float(1),d.Float(2),d.Float(3),d.Float(4),d.Float(5),d.Float(6),d.Float(7),d.Float(8),d.Float(9),d.Float(10),d.Float(11),d.Float(12)}});
            if (d.Opcode == 0x08CA) result = services.InitZonePopulationSettings(id);
            if (d.Opcode == 0x0767) result = services.SetZonePopulationType({id,d.Text,a});
            if (d.Opcode == 0x0874) result = services.SetZonePopulationRaces({id,d.Text,a});
            if (d.Opcode == 0x076A) result = services.SetZoneDealerStrength({id,d.Text,a});
            if (d.Opcode == 0x076C) result = services.SetZoneGangStrength({id,d.Text,a,b});
            if (d.Opcode == 0x09B7) result = services.SetZoneNoCops({id,d.Text,a});
            if (d.Opcode == 0x02B9) result = services.DeactivateGarage({id, d.Text});
            if (d.Opcode == 0x02FA) result = services.ChangeGarageType({id, d.Text, d.Int(1)});
            if (d.Opcode == 0x016C || d.Opcode == 0x016D) result = services.AddRestart({id,
                d.Opcode == 0x016C ? NativeRestartKind::Hospital : NativeRestartKind::Police,
                {d.Float(0), d.Float(1), d.Float(2)}, d.Float(3), d.Int(4)});
            if (d.Opcode == 0x0814) result = services.AddStuntJump({id,
                {d.Float(0), d.Float(1), d.Float(2)}, {d.Float(3), d.Float(4), d.Float(5)},
                {d.Float(6), d.Float(7), d.Float(8)}, {d.Float(9), d.Float(10), d.Float(11)},
                {d.Float(12), d.Float(13), d.Float(14)}, d.Int(15)});
            if (d.Opcode == 0x029B || d.Opcode == 0x0107) {
                NativeScriptObjectRequest request{id, d.Int(0), {d.Float(1), d.Float(2), d.Float(3)}};
                if (request.ModelId < 0) {
                    const auto index = uint64(-int64(request.ModelId));
                    if (index >= m_Metadata.UsedObjects.size()) return invalid("object used-object index out of bounds");
                    request.UsedObjectName = m_Metadata.UsedObjects[index];
                }
                auto created = d.Opcode == 0x029B ? services.CreateObjectNoOffset(request) : services.CreateObject(request);
                result = std::move(created.Result); reference = created.Reference.Value;
            }
            if (d.Opcode == 0x0177) result = services.SetObjectHeading({id, {a}, d.Float(1)});
            if (d.Opcode == 0x01C7) result = services.MarkObjectNoLongerNeeded({id, {a}});
            if (d.Opcode == 0x07F7) result = services.SetObjectCollisionDamageEffect({id, {a}, b});
            if (d.Opcode == 0x0550) result = services.FreezeObjectPosition({id, {a}, b != 0});
            if (d.Opcode == 0x0392) result = services.SetObjectDynamic({id, {a}, b != 0});
            if (d.Opcode == 0x09CA) {
                const auto proofs = std::uint8_t((d.Int(1) != 0) | ((d.Int(2) != 0) << 1) |
                    ((d.Int(3) != 0) << 2) | ((d.Int(4) != 0) << 3) | ((d.Int(5) != 0) << 4));
                result = services.SetObjectProofs({id, {a}, proofs});
            }
            if (d.Opcode == 0x034D) result = services.RotateObject({id, {a}, {d.Float(1), d.Float(2), 0}, d.Int(3) != 0});
            if (d.Opcode == 0x0566) result = services.SetObjectAreaVisible({id, {a}, b});
            if (d.Opcode == 0x01BB) {
                auto coordinates = services.GetObjectCoordinates({id, {a}, {}});
                result = std::move(coordinates.Result);
                if (result.Status == NativeScriptServiceStatus::Ready) objectCoordinates = coordinates.Position;
            }
            if (d.Opcode == 0x0176) {
                auto heading = services.GetObjectHeading({id, {a}, {}});
                result = std::move(heading.Result);
                if (result.Status == NativeScriptServiceStatus::Ready) objectHeading = heading.Degrees;
            }
            if (d.Opcode == 0x0827) result = services.ConnectObjectLods({id, {a}, {b}});
            if (d.Opcode == 0x0381) result = services.SetObjectVelocity({id, {a}, {d.Float(1), d.Float(2), d.Float(3)}});
            if (d.Opcode == 0x0400) {
                auto coordinates = services.GetObjectOffsetInWorld({id, {a}, {d.Float(1), d.Float(2), d.Float(3)}});
                result = std::move(coordinates.Result);
                if (result.Status == NativeScriptServiceStatus::Ready) objectCoordinates = coordinates.Position;
            }
            if (d.Opcode == 0x0453) result = services.SetObjectRotation({id, {a}, {d.Float(1), d.Float(2), d.Float(3)}, false});
            if (d.Opcode == 0x0A17) result = services.SetCarGeneratorOwned({id, {a}, b != 0});
            if (d.Opcode == 0x0517) {
                auto created = services.CreateLockedProperty({id, {d.Float(0), d.Float(1), d.Float(2)}, d.Text});
                result = std::move(created.Result); reference = created.Reference.Value;
            }
            if (d.Opcode == 0x0518) {
                auto created = services.CreateForSaleProperty({id, {d.Float(0), d.Float(1), d.Float(2)}, d.Int(3), d.Text});
                result = std::move(created.Result); reference = created.Reference.Value;
            }
            if (d.Opcode == 0x0570) {
                auto created = services.CreateContactBlip({id, {d.Float(0), d.Float(1), d.Float(2)}, d.Int(3)});
                result = std::move(created.Result); reference = created.Reference.Value;
            }
            if (d.Opcode == 0x02A7) {
                auto created = services.CreateContactBlip({id, {d.Float(0), d.Float(1), d.Float(2)}, d.Int(3), false, true});
                result = std::move(created.Result); reference = created.Reference.Value;
            }
            if (d.Opcode == 0x04CE) {
                auto created = services.CreateCoordinateBlip({id, {d.Float(0), d.Float(1), d.Float(2)}, d.Int(3)});
                result = std::move(created.Result); reference = created.Reference.Value;
            }
            if (d.Opcode == 0x018B) result = services.SetBlipDisplay({id, {a}, b});
            if (d.Opcode == 0x04E4) result = services.RequestCollision({id, d.Float(0), d.Float(1)});
            if (d.Opcode == 0x03CB) result = services.LoadScene({id, {d.Float(0), d.Float(1), d.Float(2)}});
            if (d.Opcode == 0x0053) result = services.CreatePlayer({id, a, {d.Float(1), d.Float(2), d.Float(3)}});
            if (d.Opcode == 0x07AF) {
                auto lookup = services.GetPlayerGroup({id, a});
                result = std::move(lookup.Result);
                reference = lookup.Reference.Value;
            }
            if (d.Opcode == 0x01F5) {
                auto lookup = services.GetPlayerChar({id, a});
                result = std::move(lookup.Result);
                reference = lookup.Reference.Value;
            }
            if (d.Opcode == 0x0373) result = services.SetCameraBehindPlayer({id});
            if (d.Opcode == 0x0173) {
                float degrees = d.Float(1);
                if (degrees < 0) degrees += 360.0f;
                else if (degrees > 360.0f) degrees -= 360.0f;
                result = services.SetCharHeading({id, {a}, degrees * (std::numbers::pi_v<float> / 180.0f)});
            }
        } catch (const std::exception& exception) {
            return Fail(thread, NativeScriptStatus::Error, d.Opcode, "service exception: " + std::string(exception.what()));
        } catch (...) {
            return invalid("unknown service exception");
        }
        switch (result.Status) {
        case NativeScriptServiceStatus::Pending:
            m_Pending = true;
            m_PendingInstruction = d;
            m_PendingThread = thread;
            return {NativeScriptStatus::Pending, state.IP, rawOpcode, 0, std::move(result.Message), thread};
        case NativeScriptServiceStatus::Unsupported:
            return Fail(thread, NativeScriptStatus::Unsupported, rawOpcode, "service unsupported: " + result.Message);
        case NativeScriptServiceStatus::Error:
            return Fail(thread, NativeScriptStatus::Error, rawOpcode, "service failed: " + result.Message);
        case NativeScriptServiceStatus::Ready: break;
        default: return invalid("invalid service result");
        }
        // Group generations occupy the high 16 bits, including the sign bit.
        // The host validates liveness; only the invalid sentinel is VM-invalid.
        if ((d.Opcode == 0x07AF || d.Opcode == 0x01F5 || d.Opcode == 0x0517 || d.Opcode == 0x0518 || d.Opcode == 0x0570 || d.Opcode == 0x04CE || d.Opcode == 0x029B || d.Opcode == 0x0107) && reference == -1) return invalid("Ready lookup returned invalid script reference");
    }

    // Commit point: nothing above changes script-visible state or global bytes.
    auto setStat = [&](int32 id, float value, bool notify) {
        if (id < 82) m_State.FloatStats[std::size_t(id)] = value;
        else m_State.IntStats[std::size_t(id - 120)] = int32(value);
        ++m_State.StatWrites;
        if (notify) ++m_State.UnprocessedStatNotifications;
    };
    auto write = [&](unsigned operand, uint32 value) {
        const auto offset = uint16(d.Values[operand]);
        if (d.OutputGlobal) {
            for (unsigned i = 0; i < 4; ++i) m_Memory[offset + i] = uint8(value >> (i * 8));
        } else state.Locals[offset] = value;
        state.LastOutputWrite = {state.LastOutputWrite.Sequence + 1, state.IP, offset, d.OutputGlobal, std::bit_cast<int32>(value)};
    };
    auto writeText = [&](unsigned operand) {
        const auto offset = uint16(d.Values[operand]);
        const auto length = d.RawTags[operand] >= 16 ? 16u : 8u;
        if (d.OutputGlobal) std::copy_n(reinterpret_cast<const uint8*>(d.LongText.data()), length, m_Memory.data() + offset);
        else std::memcpy(state.Locals.data() + offset, d.LongText.data(), length);
        state.LastOutputWrite = {state.LastOutputWrite.Sequence + 1, state.IP, offset, d.OutputGlobal, 0};
    };
    auto updateCondition = [&](bool condition) {
        condition = condition != d.Negated;
        if (!state.AndOrState) state.Condition = condition;
        else if (state.AndOrState < 21) state.Condition &= condition;
        else state.Condition |= condition;
        if (state.AndOrState == 1 || state.AndOrState == 21) state.AndOrState = 0;
        else if (state.AndOrState) --state.AndOrState;
    };
    auto launchPrepared = [&]() {
        const auto slot = m_Idle.empty() ? m_Threads.size() : m_Idle.back();
        if (m_Idle.empty()) m_Threads.push_back(std::move(*newThread));
        else { m_Idle.pop_back(); m_Threads[slot] = std::move(*newThread); }
        m_Active.insert(m_Active.begin(), slot);
    };
    state.Waiting = false;
    m_Pending = false;
    m_PendingInstruction.reset();
    switch (d.Opcode) {
    case 0x0001:
        state.WakeTimeMs = state.TimeMs + uint32(a);
        state.Waiting = true;
        break;
    case 0x0002: d.Next = Target(thread, a); break;
    case 0x004F: case 0x00D7: launchPrepared(); break;
    case 0x0050:
        state.ReturnStack[state.StackDepth++] = d.Next;
        d.Next = Target(thread, a);
        break;
    case 0x0051:
        d.Next = state.ReturnStack[--state.StackDepth];
        state.ReturnStack[state.StackDepth] = 0;
        break;
    case 0x0871:
        if (switchActivate) {
            state.SwitchValue = switchValue;
            state.SwitchRemaining = switchRemaining;
            state.SwitchDefault = switchDefault;
            state.SwitchHasDefault = switchHasDefault;
            state.SwitchActive = true;
        }
        if (switchClear) state.SwitchActive = false;
        if (switchTarget) d.Next = *switchTarget;
        break;
    case 0x0872:
        state.SwitchRemaining = switchRemaining;
        if (switchClear) state.SwitchActive = false;
        if (switchTarget) d.Next = *switchTarget;
        break;
    case 0x01BD: write(0, state.TimeMs); break;
    case 0x0004: case 0x0005: case 0x0006: case 0x0007: case 0x0084: case 0x0085: case 0x0086: case 0x008A: case 0x008B: write(0, d.Values[1]); break;
    case 0x08E1: write(0, uint32(integerResult)); break;
    case 0x0842: write(1, uint32(integerResult)); break;
    case 0x09FB:
    case 0x07D0:
    case 0x077E: write(0, uint32(integerResult)); break;
    case 0x0953: write(0, uint32(integerResult)); break;
    case 0x00A5: write(4, uint32(reference)); break;
    case 0x00A0:
        if (!objectCoordinates) return invalid("ped coordinate service returned no value");
        write(1, std::bit_cast<std::uint32_t>(objectCoordinates->X));
        write(2, std::bit_cast<std::uint32_t>(objectCoordinates->Y));
        write(3, std::bit_cast<std::uint32_t>(objectCoordinates->Z));
        break;
    case 0x009A: write(5, uint32(reference)); break;
    case 0x0129: write(3, uint32(reference)); break;
    case 0x01C8: write(4, uint32(reference)); break;
    case 0x06D8: write(5, uint32(reference)); break;
    case 0x09B2: write(1, uint32(integerResult)); write(2, uint32(integerResult2)); break;
    case 0x0560: write(1, uint32(reference)); break;
    case 0x03C0: write(1, uint32(reference)); break;
    case 0x04FC:
        write(1, uint32(statsTimes[0])); write(2, std::bit_cast<uint32>(statsDistances[0]));
        write(3, uint32(statsTimes[1])); write(4, std::bit_cast<uint32>(statsDistances[1]));
        write(5, uint32(statsTimes[2])); write(6, std::bit_cast<uint32>(statsDistances[2]));
        break;
    case 0x008C: case 0x008D: write(0, arithmeticResult); break;
    case 0x0652: write(1, statResult); break;
    case 0x0926: write(1, streamedUsersResult); break;
    case 0x058C: write(0, progressResult); break;
    case 0x09E8: write(1, uint32(integerResult)); break;
    case 0x05A9: case 0x05AA: case 0x06D1: writeText(0); break;
    case 0x094B:
        d.LongText = stringResult;
        writeText(1);
        break;
    case 0x04AE:
        if(d.Tags[0]!=2)return invalid("04AE requires global destination");
        write(0,d.Values[1]); break;
    case 0x04AF:
        if(d.Tags[0]!=3)return invalid("04AF requires local destination");
        write(0,d.Values[1]); break;
    case 0x0008: case 0x0009: case 0x000A: case 0x000B:
    case 0x000C: case 0x000D: case 0x000E: case 0x000F:
    case 0x0010: case 0x0011: case 0x0012: case 0x0013:
    case 0x0014: case 0x0015: case 0x0016: case 0x0017:
        write(0, arithmeticResult); break;
    case 0x0058: case 0x0059: case 0x005A: case 0x005B: case 0x005C: case 0x005D: case 0x005E: case 0x005F:
    case 0x0060: case 0x0061: case 0x0062: case 0x0063: case 0x0064: case 0x0065:
    case 0x0068: case 0x006A: case 0x006B: case 0x006D: case 0x006F: case 0x0072: case 0x0073:
        write(0, arithmeticResult); break;
    case 0x08BA: case 0x08BB: case 0x08BC: case 0x08C0: case 0x08C1: case 0x08C2:
        write(0, arithmeticResult); break;
    case 0x06C8:
        // Original owned-retail static RE: dispatcher 0x49CCE5 subtracts 1700,
        // table 0x49EDC4[06C8-1700] -> 0x49DC8B. CollectParameters(1),
        // cmp ScriptParams[0],0; setne; store byte 0xBFFC14; return OR_CONTINUE.
        // Same flag consumed by retail LaRiotsActiveHere at 0x4449DE and saved
        // at SimpleVariables offset 0xE0 (restore 0x5EDB36). No EXE runtime IO.
        m_State.LaRiotsEnabled = a != 0;
        ++m_State.LaRiotsRevision;
        break;
    case 0x00D6:
        state.AndOrState = a ? uint8(a + 1) : 0;
        state.Condition = a > 0 && a < 21;
        break;
    case 0x0038: case 0x0039: case 0x003A: case 0x003B: case 0x003C:
    case 0x04A3: case 0x04A4: case 0x07D6: updateCondition(a == b); break;
    case 0x0018: case 0x0019: case 0x001A: case 0x001B: case 0x001C: case 0x001D:
    case 0x001E: case 0x001F: updateCondition(a > b); break;
    case 0x0028: case 0x0029: case 0x002A: case 0x002B: case 0x002C: case 0x002F:
        updateCondition(a >= b); break;
    case 0x0020: case 0x0021: case 0x0022: case 0x0023: case 0x0024: case 0x0025:
        updateCondition(d.Float(0) > d.Float(1)); break;
    case 0x0030: updateCondition(d.Float(0) >= d.Float(1)); break;
    case 0x0043: updateCondition(d.Float(0) == d.Float(1)); break;
    case 0x0846: updateCondition(std::ranges::all_of(d.Strings[0], [](char value) { return value == 0; })); break;
    case 0x016B: updateCondition(m_State.Fade.Fading); break;
    case 0x08B4: case 0x08B5: case 0x08B6: updateCondition((uint32(a) & (uint32(1) << unsigned(b))) != 0); break;
    case 0x0256: updateCondition(booleanResult); break;
    case 0x0424: updateCondition(true); break;
    case 0x01F3: updateCondition(booleanResult); break;
    case 0x0103: updateCondition(booleanResult); break;
    case 0x0119: case 0x060E: case 0x03D0: case 0x03D2: updateCondition(booleanResult); break;
    case 0x056A:
    case 0x02E9:
    case 0x06B9:
    case 0x0118:
    case 0x0112:
    case 0x0445: updateCondition(booleanResult); break;
    case 0x00A3: case 0x00EC: case 0x00FE: case 0x00FF: updateCondition(booleanResult); break;
    case 0x03CA: case 0x075C: case 0x0965: updateCondition(booleanResult); break;
    case 0x03B0: updateCondition(booleanResult); break;
    case 0x0491: updateCondition(booleanResult); break;
    case 0x0248: updateCondition(booleanResult); break;
    case 0x023D: updateCondition(booleanResult); break;
    case 0x07C1: updateCondition(booleanResult); break;
    case 0x0A0F: updateCondition(booleanResult); break;
    case 0x09C8: updateCondition(booleanResult); break;
    case 0x08AB: updateCondition(streamedLoadedResult); break;
    case 0x0471: updateCondition(booleanResult); break;
    case 0x09AE: case 0x04C8: case 0x04A7: case 0x09E7: case 0x00DD: case 0x00DF: case 0x03EE:
        updateCondition(booleanResult); break;
    case 0x0214: updateCondition(pickupCollected); break;
    case 0x004D: if (!state.Condition) d.Next = Target(thread, a); break;
    case 0x004E:
        // Owned VM lifecycle only. ShutdownThisScript's original-address world
        // cleanup is outside this slice; no mission world allocations supported.
        state.Active = false;
        if (state.ThisMustBeTheOnlyMissionRunning) m_State.AlreadyRunningMission = false;
        if (state.IsExternal) --m_StreamedStates[std::size_t(state.StreamedIndex)].Users;
        std::erase(m_Active, thread);
        m_Idle.push_back(thread); // AddScriptToList(idle): head insertion
        break;
    case 0x03A4: state.Name = d.Text; break;
    case 0x00D8:
        if (m_State.OnAMissionFlag && IsGlobal(m_State.OnAMissionFlag)) {
            const auto offset = m_State.OnAMissionFlag;
            for (unsigned i = 0; i < 4; ++i) m_Memory[offset + i] = 0;
        }
        state.UsesMissionCleanup = false;
        break;
    case 0x0459: {
        const auto active = m_Active;
        for (const auto victim : active) {
            auto& candidate = m_Threads[victim];
            if (!candidate.Active || !EqualNameNoCase(candidate.Name, d.Text)) continue;
            candidate.Active = false;
            if (candidate.ThisMustBeTheOnlyMissionRunning) m_State.AlreadyRunningMission = false;
            if (candidate.IsExternal && candidate.StreamedIndex >= 0 &&
                std::size_t(candidate.StreamedIndex) < m_StreamedStates.size() &&
                m_StreamedStates[std::size_t(candidate.StreamedIndex)].Users)
                --m_StreamedStates[std::size_t(candidate.StreamedIndex)].Users;
            std::erase(m_Active, victim);
            m_Idle.push_back(victim);
        }
        break;
    }
    case 0x016A: {
        auto& fade = m_State.Fade;
        fade.DurationSeconds = float(a) / 1000.0f;
        fade.Direction = uint8(b);
        fade.Fading = fade.MusicFading = true;
        fade.StartMs = fade.MusicStartMs = state.TimeMs;
        fade.MusicDuration = std::min(std::max(fade.DurationSeconds * 0.3f, 0.3f), fade.DurationSeconds);
        fade.MusicWait = b == 0 ? fade.DurationSeconds - fade.MusicDuration : 0;
        if (b == 0) fade.MusicDuration = std::max(fade.MusicDuration - 0.1f, 0.0f);
        ++fade.Revision;
        break;
    }
    // Dedicated totals use documented opcode contracts. Their complete original
    // internal notification/call sequence is NOT established by the schema.
    case 0x042C: m_State.IntStats[148 - 120] = a; ++m_State.StatWrites; break;
    case 0x0317: ++m_State.IntStats[146 - 120]; ++m_State.StatWrites; break;
    case 0x030D: setStat(1, float(a), false); break;
    case 0x0997: m_State.IntStats[228 - 120] = a; ++m_State.StatWrites; break;
    case 0x01F0: {
        constexpr int32 chaos[] = {0, 115, 365, 875, 1800, 3500, 6900};
        m_State.MaximumWantedLevel = a;
        m_State.MaximumChaosLevel = chaos[a];
        break;
    }
    case 0x0111: state.DeathArrestCheckEnabled = a != 0; break;
    case 0x00C0:
        m_State.Clock.Hours = uint8(a);
        m_State.Clock.Minutes = uint8(b);
        m_State.Clock.Seconds = 0;
        m_State.Clock.LastTickMs = state.TimeMs;
        ++m_State.Clock.Revision;
        break;
    case 0x062A: setStat(a, d.Float(1), true); break;
    case 0x0629:
        // Original SetStat<int32> converts to float before CStats::SetStatValue.
        setStat(a, float(b), true);
        break;
    case 0x0053: write(4, uint32(a)); break;
    case 0x0517: case 0x0570: case 0x04CE: write(4, uint32(reference)); break;
    case 0x029B: case 0x0107: write(4, uint32(reference)); break;
    case 0x0958: case 0x0959: case 0x095A: write(3,uint32(reference)); break;
    case 0x032B: write(6,uint32(reference)); break;
    case 0x01BB:
        if (!objectCoordinates) return invalid("object coordinate service returned no value");
        write(1, std::bit_cast<std::uint32_t>(objectCoordinates->X));
        write(2, std::bit_cast<std::uint32_t>(objectCoordinates->Y));
        write(3, std::bit_cast<std::uint32_t>(objectCoordinates->Z));
        break;
    case 0x0176:
        if (!objectHeading) return invalid("object heading service returned no value");
        write(1, std::bit_cast<std::uint32_t>(*objectHeading));
        break;
    case 0x0400:
        if (!objectCoordinates) return invalid("object world offset service returned no value");
        write(4, std::bit_cast<std::uint32_t>(objectCoordinates->X));
        write(5, std::bit_cast<std::uint32_t>(objectCoordinates->Y));
        write(6, std::bit_cast<std::uint32_t>(objectCoordinates->Z));
        break;
    case 0x0518: case 0x0213: write(5, uint32(reference)); break;
    case 0x02A7: case 0x02A8: write(4,uint32(reference)); break;
    case 0x0A40: write(4, uint32(reference)); break;
    case 0x014B: write(12, uint32(reference)); break;
    case 0x09E2: write(13, uint32(reference)); break;
    case 0x07AF: case 0x01F5: write(1, uint32(reference)); break;
    case 0x0746: {
        auto& categories = m_State.Relationships[b];
        const uint32 bit = uint32(1) << d.Int(2);
        // Acquaintance.cpp::SetAsAcquaintance preserves an existing membership.
        if (!(categories[a] & bit)) {
            for (auto& category : categories) category &= ~bit;
            categories[a] |= bit;
        }
        ++m_State.RelationshipRevision;
        break;
    }
    case 0x0417: {
        m_Mission = std::move(mission);
        launchPrepared();
        m_State.AlreadyRunningMission = true;
        break;
    }
    case 0x0911: break; // genuine REGISTER_STREAMED_SCRIPT NOP
    case 0x0914: break; // genuine streamed-script registration companion NOP
    case 0x0913:
        launchPrepared();
        ++m_StreamedStates[std::size_t(streamedLaunch)].Users;
        break;
    case 0x06CF: break; // genuine DISPLAY_TIMER_BARS NOP: UnusedCommands.cpp
    case 0x0662: break; // source write-debug command is a retail NOP
    case 0x0180:
        if (d.Tags[0] != 2 || d.Values[0] > std::numeric_limits<std::uint16_t>::max())
            return invalid("mission flag declaration requires a global byte offset");
        m_State.OnAMissionFlag = std::uint16_t(d.Values[0]);
        break;
    default: break; // service commands committed their effects on Ready
    }
    state.LastInstructionIP = state.IP;
    state.LastOpcode = rawOpcode;
    state.IP = d.Next;
    ++state.Commands;
    ++m_CommandSequence;
    return {state.Waiting || !state.Active ? NativeScriptStatus::Waiting : NativeScriptStatus::Advanced, state.IP, state.LastOpcode, 1, {}, thread};
}

NativeScriptResult NativeScriptSession::Run(NativeScriptServices& services, std::size_t quota) {
    if (!quota) return {NativeScriptStatus::Error, m_State.IP, 0, 0, "zero instruction quota"};
    std::size_t count = 0;
    NativeScriptResult result;
    do {
        result = Step(services);
        count += result.Executed;
        if (result.Status != NativeScriptStatus::Advanced) { result.Executed = count; return result; }
    } while (count < quota);
    result.Status = NativeScriptStatus::BudgetYield;
    result.Executed = count;
    return result;
}

NativeScriptResult NativeScriptSession::RunPass(NativeScriptServices& services, std::size_t quota) {
    return RunPass(services, quota, nullptr);
}

NativeScriptResult NativeScriptSession::RunPass(NativeScriptServices& services, std::size_t quota, NativeScriptCommitSink* sink) {
    if (!m_Loaded || m_InService || !quota) return {NativeScriptStatus::Error, m_State.IP, 0, 0, "invalid scheduler call"};
    if (m_Faulted) return m_Fault;
    if (m_Pending && m_Pass.empty()) return {NativeScriptStatus::Error, m_State.IP, 0, 0, "main observation service outstanding"};
    // Equivalent to TheScripts.cpp's next = it->m_pNext BEFORE Process().
    // No supported instruction can remove any other thread from the list.
    if (m_Pass.empty()) { m_Pass = m_Active; m_PassCursor = 0; }
    std::size_t count = 0;
    NativeScriptResult result{NativeScriptStatus::Waiting, m_State.IP, 0, 0, {}};
    while (m_PassCursor < m_Pass.size()) {
        result = StepThread(services, m_Pass[m_PassCursor]);
        static_cast<NativeScriptThreadState&>(m_State) = m_Threads[0];
        if (sink && result.Executed) {
            const auto& thread = m_Threads[result.ThreadIndex];
            m_InService = true;
            sink->OnScriptCommit({m_SessionId, m_CommandSequence, thread.LastInstructionIP},
                result.ThreadIndex, m_State, thread);
            m_InService = false;
        }
        count += result.Executed;
        result.Executed = count;
        if (result.Status == NativeScriptStatus::Pending || result.Status == NativeScriptStatus::Unsupported || result.Status == NativeScriptStatus::Error) return result;
        if (result.Status == NativeScriptStatus::Waiting) ++m_PassCursor;
        if (m_PassCursor == m_Pass.size()) break;
        if (count == quota) { result.Status = NativeScriptStatus::BudgetYield; return result; }
    }
    m_Pass.clear();
    result.Status = NativeScriptStatus::Waiting;
    return result;
}

bool NativeScriptSession::SeedRelationships(const std::array<std::array<uint32, 5>, 32>& relationships) {
    if (!m_Loaded || m_InService || m_CommandSequence) return false;
    m_State.Relationships = relationships;
    return true;
}

bool NativeScriptSession::AdvanceTime(uint32 nowMs, std::string& error) {
    if (!m_Loaded || m_InService || nowMs < m_State.TimeMs) { error = "invalid/non-monotonic script time"; return false; }
    const uint32 elapsed = nowMs - m_State.TimeMs;
    const float dt = float(elapsed) / 1000.0f;
    for (auto& thread : m_Threads) {
        thread.TimeMs = nowMs;
        if (!thread.Active) continue;
        thread.Locals[32] += elapsed;
        thread.Locals[33] += elapsed;
    }
    static_cast<NativeScriptThreadState&>(m_State) = m_Threads[0];
    // Camera.cpp::ProcessFade/ProcessMusicFade, default startup camera policy.
    // Preserve the original zero-duration direction-1 behavior (no subtraction).
    auto& f = m_State.Fade;
    if (f.Fading) {
        if (f.Direction == 1) {
            if (f.DurationSeconds != 0) f.Alpha -= dt / f.DurationSeconds * 255.0f;
            if (f.Alpha <= 0) { f.Alpha = 0; f.Fading = false; }
        } else {
            if (f.Alpha >= 255) f.Fading = false;
            f.Alpha += f.DurationSeconds == 0 ? 255.0f : dt / f.DurationSeconds * 255.0f;
            f.Alpha = std::min(f.Alpha, 255.0f);
        }
    }
    if (f.MusicFading) {
        if (f.MusicWait > 0) f.MusicWait -= dt;
        else if (f.Direction == 1) {
            f.EffectsScale = f.MusicDuration > 0 ? dt / f.MusicDuration + f.EffectsScale : 1.0f;
            if (f.EffectsScale >= 1) { f.MusicFadedOut = f.MusicFading = false; f.EffectsScale = 1; }
        } else {
            if (f.EffectsScale <= 0) { f.MusicFadedOut = true; f.MusicFading = false; f.EffectsScale = 0; }
            f.EffectsScale = f.MusicDuration > 0 ? std::max(0.0f, f.EffectsScale - dt / f.MusicDuration) : 0;
        }
    }
    error.clear();
    return true;
}
