#include "app/platform/linux/NativeScriptSession.h"
#include "app/platform/linux/NativeScriptEntities.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
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
std::atomic<uint64> s_NextSession{1};

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

enum class Operand { Integer, Float, String, Output, FloatOutput, InOutInteger, InOutFloat };
struct Signature {
    uint16 Opcode;
    std::array<Operand, 13> Types{};
    unsigned Count = 0;
};
using O = Operand;
// SA default schema 1.65, sannybuilder/library commit
// 53ed1c2561bf6ca70dc16afca5d8f3a406066158, sa/sa.json.
// Fixed typed contracts, NOT keyword dispatch or arity inference. Behavioral
// references: game_sa/Scripts/{RunningScript,TheScripts,Commands/*}.cpp.
constexpr Signature Signatures[] = {
    {0x0000, {}, 0},
    {0x0001, {O::Integer}, 1}, {0x0002, {O::Integer}, 1},
    {0x0004, {O::Output, O::Integer}, 2}, {0x0005, {O::FloatOutput, O::Float}, 2},
    {0x0006, {O::Output, O::Integer}, 2}, {0x0007, {O::FloatOutput, O::Float}, 2},
    {0x0086, {O::FloatOutput, O::Float}, 2},
    // BasicCommands.cpp::{Add,Sub,Mult,Div}InPlace<T>. The opcode selects
    // global/local destination and int/float; addresses remain byte/cell based.
    {0x0008, {O::InOutInteger, O::Integer}, 2}, {0x0009, {O::InOutFloat, O::Float}, 2},
    {0x000A, {O::InOutInteger, O::Integer}, 2}, {0x000B, {O::InOutFloat, O::Float}, 2},
    {0x000C, {O::InOutInteger, O::Integer}, 2}, {0x000D, {O::InOutFloat, O::Float}, 2},
    {0x000E, {O::InOutInteger, O::Integer}, 2}, {0x000F, {O::InOutFloat, O::Float}, 2},
    {0x0010, {O::InOutInteger, O::Integer}, 2}, {0x0011, {O::InOutFloat, O::Float}, 2},
    {0x0012, {O::InOutInteger, O::Integer}, 2}, {0x0013, {O::InOutFloat, O::Float}, 2},
    {0x0014, {O::InOutInteger, O::Integer}, 2}, {0x0015, {O::InOutFloat, O::Float}, 2},
    {0x0016, {O::InOutInteger, O::Integer}, 2}, {0x0017, {O::InOutFloat, O::Float}, 2},
    {0x001A, {O::Integer, O::Integer}, 2},
    {0x004D, {O::Integer}, 1}, {0x004E, {}, 0}, {0x00D6, {O::Integer}, 1},
    {0x03A4, {O::String}, 1}, {0x016A, {O::Integer, O::Integer}, 2},
    {0x042C, {O::Integer}, 1}, {0x030D, {O::Integer}, 1},
    {0x0997, {O::Integer}, 1}, {0x01F0, {O::Integer}, 1},
    {0x0111, {O::Integer}, 1}, {0x00C0, {O::Integer, O::Integer}, 2},
    {0x04E4, {O::Float, O::Float}, 2},
    {0x03CB, {O::Float, O::Float, O::Float}, 3},
    {0x062A, {O::Integer, O::Float}, 2},
    {0x0629, {O::Integer, O::Integer}, 2},
    {0x0053, {O::Integer, O::Float, O::Float, O::Float, O::Output}, 5},
    {0x06CF, {O::Integer}, 1},
    {0x0746, {O::Integer, O::Integer, O::Integer}, 3},
    {0x07AF, {O::Integer, O::Output}, 2}, {0x01F5, {O::Integer, O::Output}, 2},
    {0x0373, {}, 0}, {0x0173, {O::Integer, O::Float}, 2},
    {0x0417, {O::Integer}, 1},
    {0x06C8, {O::Integer}, 1},
    {0x0517, {O::Float, O::Float, O::Float, O::String, O::Output}, 5},
    {0x0518, {O::Float, O::Float, O::Float, O::Integer, O::String, O::Output}, 6},
    {0x0570, {O::Float, O::Float, O::Float, O::Integer, O::Output}, 5},
    {0x04CE, {O::Float, O::Float, O::Float, O::Integer, O::Output}, 5},
    {0x018B, {O::Integer, O::Integer}, 2},
    {0x09B4, {O::Float, O::Float, O::Float, O::Integer, O::Integer}, 5},
    {0x02B9, {O::String}, 1},
    {0x0213, {O::Integer, O::Integer, O::Float, O::Float, O::Float, O::Output}, 6},
    {0x0214, {O::Integer}, 1}, {0x0215, {O::Integer}, 1},
    {0x014B, {O::Float, O::Float, O::Float, O::Float, O::Integer, O::Integer, O::Integer,
              O::Integer, O::Integer, O::Integer, O::Integer, O::Integer, O::Output}, 13},
    {0x014C, {O::Integer, O::Integer}, 2},
};

const Signature* FindSignature(uint16 opcode) {
    for (const auto& signature : Signatures) if (signature.Opcode == opcode) return &signature;
    return nullptr;
}

bool ValidStat(int32 id) { return (id >= 0 && id < 82) || (id >= 120 && id < 343); }
bool FitsInt(float value) {
    return std::isfinite(value) && double(value) >= std::numeric_limits<int32>::min()
        && double(value) <= std::numeric_limits<int32>::max();
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
            for (uint32 j = 0; j < metadata.StreamedScripts; ++j) {
                if (Word(prefix, pos + 16 + j * 28 + 24) > metadata.LargestStreamed)
                    return reject("streamed script exceeds declared maximum");
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
    m_Memory = std::move(memory);
    m_Payload = std::move(payload);
    m_Mission.clear();
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

bool NativeScriptSession::IsGlobal(uint16 offset) const {
    // Globals are byte offsets, not cell indices. Header and code are immutable.
    return offset >= 8 && uint32(offset) + 4 <= 8 + m_Metadata.GlobalBytes;
}

bool NativeScriptSession::ReadGlobal(uint16 offset, int32& value) const {
    if (!m_Loaded || !IsGlobal(offset)) return false;
    value = std::bit_cast<int32>(Word(m_Memory, offset));
    return true;
}

bool NativeScriptSession::Decode(std::size_t thread, uint32 ip, Instruction& d, std::string& error) const {
    const auto& state = m_Threads[thread];
    const uint32 base = ip >= MainCapacity ? MainCapacity : 0;
    if (base && !state.ThisMustBeTheOnlyMissionRunning) { error = "mission IP without ownership"; return false; }
    Reader reader{base ? std::span<const uint8>(m_Mission) : std::span<const uint8>(m_Memory), ip - base};
    uint32 opcode = 0;
    if (!reader.Read(2, opcode)) { error = "truncated opcode"; return false; }
    d.Opcode = uint16(opcode);
    d.Negated = (opcode & 0x8000) != 0;
    if (d.Negated && ((opcode & 0x7FFF) == 0x001A || (opcode & 0x7FFF) == 0x0214)) d.Opcode &= 0x7FFF;
    const auto* signature = FindSignature(d.Opcode);
    if (!signature) { error = "unsupported opcode (including NOT forms)"; return false; }
    for (unsigned i = 0; i < signature->Count; ++i) {
        uint32 tag = 0, bits = 0;
        if (!reader.Read(1, tag)) { error = "truncated operand tag"; return false; }
        const auto type = signature->Types[i];
        const bool output = type == O::Output || type == O::FloatOutput || type == O::InOutInteger || type == O::InOutFloat;
        const bool floating = type == O::Float || type == O::FloatOutput || type == O::InOutFloat;
        if (type == O::String) {
            if (tag != 9) { error = "unsupported string operand type"; return false; }
            d.Tags[i] = uint8(tag);
            for (auto& c : d.Text) {
                if (!reader.Read(1, bits)) { error = "truncated short string"; return false; }
                c = char(bits);
            }
            continue;
        }
        if (tag == 2 || tag == 3 || tag == 7 || tag == 8) {
            if (!reader.Read(2, bits)) { error = "truncated variable operand"; return false; }
            if (tag == 7 || tag == 8) {
                // RunningScript::{ReadArrayInformation,CollectParameters,
                // StoreParameters}: global base is bytes, local base is cells;
                // the index variable is independent of the array's own bank.
                // GetAtIPFromArray<T> supplies the source typed/count checks.
                uint32 indexVar = 0, count = 0, flags = 0;
                if (!reader.Read(2, indexVar) || !reader.Read(1, count) || !reader.Read(1, flags)) {
                    error = "truncated array operand"; return false;
                }
                const bool global = tag == 7, globalIndex = (flags & 0x80) != 0;
                if ((flags & 0x7F) != (floating ? 1u : 0u) || !count) {
                    error = "invalid numeric array type/count"; return false;
                }
                if ((global && !IsGlobal(uint16(bits))) || (!global && bits >= state.Locals.size()) ||
                    (globalIndex && !IsGlobal(uint16(indexVar))) || (!globalIndex && indexVar >= state.Locals.size())) {
                    error = "array base/index variable out of bounds"; return false;
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
            if (output) {
                d.Values[i] = bits;
                d.OutputGlobal = tag == 2;
                if (type == O::InOutInteger || type == O::InOutFloat) {
                    d.OutputValue = tag == 2 ? Word(m_Memory, bits) : state.Locals[bits];
                    if (floating && !std::isfinite(std::bit_cast<float>(d.OutputValue))) {
                        error = "nonfinite arithmetic destination"; return false;
                    }
                }
                continue;
            }
            bits = tag == 2 ? Word(m_Memory, bits) : state.Locals[bits];
        } else if (type == O::Integer && (tag == 1 || tag == 4 || tag == 5)) {
            if (!reader.Read(tag == 1 ? 4 : tag == 4 ? 1 : 2, bits)) { error = "truncated integer"; return false; }
            if (tag == 4) bits = uint32(int32(std::bit_cast<int8>(uint8(bits))));
            if (tag == 5) bits = uint32(int32(std::bit_cast<int16>(uint16(bits))));
        } else if (type == O::Float && tag == 6) {
            if (!reader.Read(4, bits)) { error = "truncated float"; return false; }
        } else {
            error = "wrong/unsupported typed operand";
            return false;
        }
        d.Tags[i] = uint8(tag);
        d.Values[i] = bits;
        if (type == O::Float && !std::isfinite(d.Float(i))) { error = "nonfinite float operand"; return false; }
    }
    d.Next = base + uint32(reader.Pos);
    return true;
}

uint32 NativeScriptSession::Target(std::size_t thread, int32 target) const {
    return target < 0 ? uint32(uint64(m_Threads[thread].BaseIP) + uint64(-int64(target))) : uint32(target);
}

bool NativeScriptSession::IsTarget(std::size_t thread, int32 label) const {
    if (label < 0 && (!m_Threads[thread].BaseIP || uint64(-int64(label)) >= m_Mission.size())) return false;
    const uint32 target = Target(thread, label);
    const bool mission = target >= MainCapacity;
    if (mission && (!m_Threads[thread].ThisMustBeTheOnlyMissionRunning || target - MainCapacity >= m_Mission.size())) return false;
    if (!mission && std::find(m_Headers.begin(), m_Headers.end(), target) != m_Headers.end()) return true;
    if (!mission && (target < m_Metadata.CodeStart || target >= m_Metadata.MainSize)) return false;
    // Prove an instruction boundary by typed decoding, never searching bytes.
    // This bounded slice rejects forward labels beyond an unknown instruction.
    uint32 pos = mission ? MainCapacity : m_Metadata.CodeStart;
    while (pos < target) {
        Instruction d;
        std::string error;
        if (!Decode(thread, pos, d, error)) return false;
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
        return Fail(thread, FindSignature(d.Opcode) ? NativeScriptStatus::Error : NativeScriptStatus::Unsupported, uint16(d.Opcode | (d.Negated ? 0x8000 : 0)), error);
    }
    const int32 a = d.Int(0), b = d.Int(1);
    const auto rawOpcode = uint16(d.Opcode | (d.Negated ? 0x8000 : 0));
    auto invalid = [&](const char* message) { return Fail(thread, NativeScriptStatus::Error, rawOpcode, message); };
    std::vector<uint8> mission;
    std::optional<NativeScriptThreadState> newThread;
    uint32 arithmeticResult = 0;
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
    switch (d.Opcode) {
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
        if (!IsTarget(thread, a)) return invalid("GOTO target is not a supported instruction boundary");
        for (unsigned i = 0; i < m_Headers.size(); ++i) {
            if (state.IP == m_Headers[i] && uint32(a) != m_HeaderTargets[i]) return invalid("modified SCM header target");
        }
        break;
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
    default: break;
    }

    int32 reference = -1;
    bool pickupCollected = false;
    if (d.Opcode == 0x04E4 || d.Opcode == 0x03CB || d.Opcode == 0x0053 || d.Opcode == 0x07AF || d.Opcode == 0x01F5 || d.Opcode == 0x0373 || d.Opcode == 0x0173 || d.Opcode == 0x0517 || d.Opcode == 0x0518 || d.Opcode == 0x0570 || d.Opcode == 0x04CE || d.Opcode == 0x018B || d.Opcode == 0x09B4 || d.Opcode == 0x02B9 || d.Opcode == 0x0213 || d.Opcode == 0x0214 || d.Opcode == 0x0215 || d.Opcode == 0x014B || d.Opcode == 0x014C) {
        const NativeScriptRequestId id{m_SessionId, m_CommandSequence + 1, state.IP};
        NativeScriptServiceResult result;
        // All operands/output bounds have been checked before ANY host call.
        struct Guard { bool& Flag; Guard(bool& flag): Flag(flag) { Flag = true; } ~Guard() { Flag = false; } } guard{m_InService};
        try {
            if (d.Opcode == 0x014B) {
                // Pinned schema: model -1 selects a random local-popcycle car,
                // NOT an SCM used-object index. Host owns source constructor
                // narrowing/model checks, including a successful -1 result.
                auto created = services.CreateCarGenerator({id,
                    {d.Float(0), d.Float(1), d.Float(2)}, d.Float(3),
                    d.Int(4), d.Int(5), d.Int(6), d.Int(7), d.Int(8), d.Int(9), d.Int(10), d.Int(11)});
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
            if (d.Opcode == 0x0214) {
                auto queried = services.HasPickupBeenCollected({id, {a}});
                result = std::move(queried.Result); pickupCollected = queried.Collected;
            }
            if (d.Opcode == 0x0215) result = services.RemoveScriptPickup({id, {a}});
            if (d.Opcode == 0x09B4) result = services.SetEntryExitFlag({id, d.Float(0), d.Float(1), d.Float(2), d.Int(3), d.Int(4)});
            if (d.Opcode == 0x02B9) result = services.DeactivateGarage({id, d.Text});
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
        if ((d.Opcode == 0x07AF || d.Opcode == 0x01F5 || d.Opcode == 0x0517 || d.Opcode == 0x0518 || d.Opcode == 0x0570 || d.Opcode == 0x04CE) && reference == -1) return invalid("Ready lookup returned invalid script reference");
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
    auto updateCondition = [&](bool condition) {
        condition = condition != d.Negated;
        if (!state.AndOrState) state.Condition = condition;
        else if (state.AndOrState < 21) state.Condition &= condition;
        else state.Condition |= condition;
        if (state.AndOrState == 1 || state.AndOrState == 21) state.AndOrState = 0;
        else if (state.AndOrState) --state.AndOrState;
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
    case 0x0004: case 0x0005: case 0x0006: case 0x0007: case 0x0086: write(0, d.Values[1]); break;
    case 0x0008: case 0x0009: case 0x000A: case 0x000B:
    case 0x000C: case 0x000D: case 0x000E: case 0x000F:
    case 0x0010: case 0x0011: case 0x0012: case 0x0013:
    case 0x0014: case 0x0015: case 0x0016: case 0x0017:
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
    case 0x001A: updateCondition(a > b); break;
    case 0x0214: updateCondition(pickupCollected); break;
    case 0x004D: if (!state.Condition) d.Next = Target(thread, a); break;
    case 0x004E:
        // Owned VM lifecycle only. ShutdownThisScript's original-address world
        // cleanup is outside this slice; no mission world allocations supported.
        state.Active = false;
        if (state.ThisMustBeTheOnlyMissionRunning) m_State.AlreadyRunningMission = false;
        std::erase(m_Active, thread);
        m_Idle.push_back(thread); // AddScriptToList(idle): head insertion
        break;
    case 0x03A4: state.Name = d.Text; break;
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
    case 0x0518: case 0x0213: write(5, uint32(reference)); break;
    case 0x014B: write(12, uint32(reference)); break;
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
        const auto slot = m_Idle.empty() ? m_Threads.size() : m_Idle.back();
        if (m_Idle.empty()) m_Threads.push_back(std::move(*newThread));
        else { m_Idle.pop_back(); m_Threads[slot] = std::move(*newThread); }
        m_Active.insert(m_Active.begin(), slot);
        m_State.AlreadyRunningMission = true;
        break;
    }
    case 0x06CF: break; // genuine DISPLAY_TIMER_BARS NOP: UnusedCommands.cpp
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
