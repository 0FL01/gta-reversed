#include "NativeProgressionRuntime.h"

#include <bit>
#include <cmath>
#include <limits>

namespace {
constexpr std::array<std::uint8_t, 8> Magic{'M', 'A', 'D', 'S', 'A', 'P', 'R', 'G'};
constexpr std::uint32_t Version = 1;

std::uint64_t Hash(std::span<const std::uint8_t> bytes) {
    std::uint64_t value = 1469598103934665603ULL;
    for (const auto byte : bytes) value = (value ^ byte) * 1099511628211ULL;
    return value;
}

struct Writer {
    std::vector<std::uint8_t> Bytes;
    void U8(std::uint8_t value) { Bytes.push_back(value); }
    void U16(std::uint16_t value) { U8(std::uint8_t(value)); U8(std::uint8_t(value >> 8)); }
    void U32(std::uint32_t value) {
        for (unsigned i = 0; i < 4; ++i) U8(std::uint8_t(value >> (i * 8)));
    }
    void U64(std::uint64_t value) {
        for (unsigned i = 0; i < 8; ++i) U8(std::uint8_t(value >> (i * 8)));
    }
    void I32(std::int32_t value) { U32(std::bit_cast<std::uint32_t>(value)); }
};

struct Reader {
    std::span<const std::uint8_t> Bytes;
    std::size_t At = 0;
    bool U8(std::uint8_t& value) {
        if (At >= Bytes.size()) return false;
        value = Bytes[At++];
        return true;
    }
    bool U16(std::uint16_t& value) {
        std::uint8_t a, b;
        if (!U8(a) || !U8(b)) return false;
        value = std::uint16_t(a) | std::uint16_t(b) << 8;
        return true;
    }
    bool U32(std::uint32_t& value) {
        value = 0;
        for (unsigned i = 0; i < 4; ++i) {
            std::uint8_t byte;
            if (!U8(byte)) return false;
            value |= std::uint32_t(byte) << (i * 8);
        }
        return true;
    }
    bool U64(std::uint64_t& value) {
        value = 0;
        for (unsigned i = 0; i < 8; ++i) {
            std::uint8_t byte;
            if (!U8(byte)) return false;
            value |= std::uint64_t(byte) << (i * 8);
        }
        return true;
    }
    bool I32(std::int32_t& value) {
        std::uint32_t bits;
        if (!U32(bits)) return false;
        value = std::bit_cast<std::int32_t>(bits);
        return true;
    }
};

NativeProgressionStatus Owner(NativeInteractionStatus status, std::string& error) {
    if (status == NativeInteractionStatus::Ok) return NativeProgressionStatus::Ok;
    if (status == NativeInteractionStatus::NotFound) return NativeProgressionStatus::NotFound;
    if (status == NativeInteractionStatus::Overflow) return NativeProgressionStatus::Overflow;
    if (error.empty()) error = "interaction owner rejected progression operation";
    return NativeProgressionStatus::OwnerError;
}
}

NativeProgressionStatus NativeProgressionRuntime::SetMoney(std::int32_t value, std::string& error) {
    const auto status = Owner(m_Interactions.SetMoney(value, error), error);
    if (status == NativeProgressionStatus::Ok) ++m_Revision;
    return status;
}

NativeProgressionStatus NativeProgressionRuntime::RegisterPurchase(std::string_view name,
    NativeInteractionKind kind, std::uint32_t price, bool owned, std::string& error) {
    const auto status = Owner(m_Interactions.Register(name, kind, price, owned, error), error);
    if (status == NativeProgressionStatus::Ok) ++m_Revision;
    return status;
}

NativeProgressionStatus NativeProgressionRuntime::Buy(std::string_view name, std::string& error) {
    const auto status = Owner(m_Interactions.Buy(name, error), error);
    if (status == NativeProgressionStatus::Ok) ++m_Revision;
    return status;
}

NativeProgressionStatus NativeProgressionRuntime::Use(std::string_view name, std::string& error) {
    const auto status = Owner(m_Interactions.Use(name, error), error);
    if (status == NativeProgressionStatus::Ok) ++m_Revision;
    return status;
}

NativeProgressionStatus NativeProgressionRuntime::SetIntegerStat(std::uint16_t id,
    std::int32_t value, std::string& error) {
    if (id < 120 || id >= 343) { error = "integer stat id is invalid"; return NativeProgressionStatus::InvalidInput; }
    m_IntegerStats[id - 120] = value;
    ++m_Revision;
    error.clear();
    return NativeProgressionStatus::Ok;
}

NativeProgressionStatus NativeProgressionRuntime::SetFloatStat(std::uint16_t id,
    float value, std::string& error) {
    if (id >= FloatStats || !std::isfinite(value)) { error = "float stat is invalid"; return NativeProgressionStatus::InvalidInput; }
    m_FloatStats[id] = value;
    ++m_Revision;
    error.clear();
    return NativeProgressionStatus::Ok;
}

NativeProgressionStatus NativeProgressionRuntime::GrantReward(std::uint8_t id,
    std::int32_t value, std::string& error) {
    if (id >= RewardCount) { error = "reward id is invalid"; return NativeProgressionStatus::InvalidInput; }
    m_Rewards[id] = value;
    ++m_Revision;
    error.clear();
    return NativeProgressionStatus::Ok;
}

NativeProgressionStatus NativeProgressionRuntime::SetUnlock(std::uint8_t id,
    bool value, std::string& error) {
    if (id >= UnlockCount) { error = "unlock id is invalid"; return NativeProgressionStatus::InvalidInput; }
    m_Unlocks[id] = value;
    ++m_Revision;
    error.clear();
    return NativeProgressionStatus::Ok;
}

NativeProgressionInterior* NativeProgressionRuntime::InteriorMutable(std::uint16_t id) noexcept {
    for (std::size_t i = 0; i < m_InteriorCount; ++i) if (m_Interiors[i].Id == id) return &m_Interiors[i];
    return nullptr;
}
const NativeProgressionInterior* NativeProgressionRuntime::Interior(std::uint16_t id) const noexcept {
    return const_cast<NativeProgressionRuntime*>(this)->InteriorMutable(id);
}

NativeProgressionStatus NativeProgressionRuntime::RegisterInterior(std::uint16_t id,
    bool unlocked, std::string& error) {
    if (Interior(id) || m_InteriorCount == InteriorCapacity) {
        error = "interior duplicate/capacity";
        return NativeProgressionStatus::InvalidInput;
    }
    m_Interiors[m_InteriorCount++] = {id, unlocked, 0};
    ++m_Revision;
    error.clear();
    return NativeProgressionStatus::Ok;
}

NativeProgressionStatus NativeProgressionRuntime::UnlockInterior(std::uint16_t id, std::string& error) {
    auto* interior = InteriorMutable(id);
    if (!interior) { error = "interior is not registered"; return NativeProgressionStatus::NotFound; }
    interior->Unlocked = true;
    ++m_Revision;
    error.clear();
    return NativeProgressionStatus::Ok;
}

NativeProgressionStatus NativeProgressionRuntime::EnterInterior(std::uint16_t id, std::string& error) {
    auto* interior = InteriorMutable(id);
    if (!interior) { error = "interior is not registered"; return NativeProgressionStatus::NotFound; }
    if (!interior->Unlocked) { error = "interior is locked"; return NativeProgressionStatus::Locked; }
    if (interior->Visits == std::numeric_limits<std::uint32_t>::max()) {
        error = "interior visit count exhausted";
        return NativeProgressionStatus::Overflow;
    }
    ++interior->Visits;
    ++m_Revision;
    error.clear();
    return NativeProgressionStatus::Ok;
}

std::int32_t NativeProgressionRuntime::IntegerStat(std::uint16_t id) const noexcept {
    return id >= 120 && id < 343 ? m_IntegerStats[id - 120] : 0;
}
float NativeProgressionRuntime::FloatStat(std::uint16_t id) const noexcept { return id < FloatStats ? m_FloatStats[id] : 0.0f; }
std::int32_t NativeProgressionRuntime::Reward(std::uint8_t id) const noexcept { return id < RewardCount ? m_Rewards[id] : 0; }
bool NativeProgressionRuntime::Unlocked(std::uint8_t id) const noexcept { return id < UnlockCount && m_Unlocks[id]; }

NativeProgressionStatus NativeProgressionRuntime::Encode(std::vector<std::uint8_t>& out, std::string& error) const {
    std::vector<std::uint8_t> interactions;
    if (m_Interactions.Encode(interactions, error) != NativeInteractionStatus::Ok) return NativeProgressionStatus::OwnerError;
    Writer writer;
    for (const auto byte : Magic) writer.U8(byte);
    writer.U32(Version);
    writer.U64(m_Revision);
    writer.U32(std::uint32_t(interactions.size()));
    for (const auto byte : interactions) writer.U8(byte);
    for (const auto value : m_IntegerStats) writer.I32(value);
    for (const auto value : m_FloatStats) writer.U32(std::bit_cast<std::uint32_t>(value));
    for (const auto value : m_Rewards) writer.I32(value);
    for (const bool value : m_Unlocks) writer.U8(value ? 1 : 0);
    writer.U32(std::uint32_t(m_InteriorCount));
    for (std::size_t i = 0; i < m_InteriorCount; ++i) {
        writer.U16(m_Interiors[i].Id);
        writer.U8(m_Interiors[i].Unlocked ? 1 : 0);
        writer.U32(m_Interiors[i].Visits);
    }
    writer.U64(Hash(writer.Bytes));
    out = std::move(writer.Bytes);
    error.clear();
    return NativeProgressionStatus::Ok;
}

NativeProgressionStatus NativeProgressionRuntime::Restore(std::span<const std::uint8_t> bytes, std::string& error) {
    if (bytes.size() < 32) { error = "progression save size"; return NativeProgressionStatus::CorruptSave; }
    std::uint64_t expected = 0;
    for (unsigned i = 0; i < 8; ++i) expected |= std::uint64_t(bytes[bytes.size() - 8 + i]) << (i * 8);
    if (Hash(bytes.first(bytes.size() - 8)) != expected) {
        error = "progression save checksum";
        return NativeProgressionStatus::CorruptSave;
    }
    Reader reader{bytes.first(bytes.size() - 8)};
    for (const auto expectedByte : Magic) {
        std::uint8_t byte;
        if (!reader.U8(byte) || byte != expectedByte) { error = "progression save magic"; return NativeProgressionStatus::CorruptSave; }
    }
    std::uint32_t version, interactionSize, interiorCount;
    std::uint64_t revision;
    if (!reader.U32(version) || version != Version || !reader.U64(revision) ||
        !reader.U32(interactionSize) || reader.At + interactionSize > reader.Bytes.size()) {
        error = "progression save header";
        return NativeProgressionStatus::CorruptSave;
    }
    NativeProgressionRuntime candidate;
    std::string ownerError;
    if (candidate.m_Interactions.Restore(reader.Bytes.subspan(reader.At, interactionSize), ownerError) != NativeInteractionStatus::Ok) {
        error = "progression interaction payload";
        return NativeProgressionStatus::CorruptSave;
    }
    reader.At += interactionSize;
    for (auto& value : candidate.m_IntegerStats) {
        if (!reader.I32(value)) { error = "progression integer stats"; return NativeProgressionStatus::CorruptSave; }
    }
    for (auto& value : candidate.m_FloatStats) {
        std::uint32_t bits;
        if (!reader.U32(bits)) { error = "progression float stats"; return NativeProgressionStatus::CorruptSave; }
        value = std::bit_cast<float>(bits);
        if (!std::isfinite(value)) { error = "progression nonfinite stat"; return NativeProgressionStatus::CorruptSave; }
    }
    for (auto& value : candidate.m_Rewards) {
        if (!reader.I32(value)) { error = "progression rewards"; return NativeProgressionStatus::CorruptSave; }
    }
    for (auto& value : candidate.m_Unlocks) {
        std::uint8_t byte;
        if (!reader.U8(byte) || byte > 1) { error = "progression unlocks"; return NativeProgressionStatus::CorruptSave; }
        value = byte != 0;
    }
    if (!reader.U32(interiorCount) || interiorCount > InteriorCapacity) {
        error = "progression interiors";
        return NativeProgressionStatus::CorruptSave;
    }
    candidate.m_InteriorCount = interiorCount;
    for (std::size_t i = 0; i < interiorCount; ++i) {
        std::uint8_t unlocked;
        auto& interior = candidate.m_Interiors[i];
        if (!reader.U16(interior.Id) || !reader.U8(unlocked) || unlocked > 1 || !reader.U32(interior.Visits)) {
            error = "progression interior record";
            return NativeProgressionStatus::CorruptSave;
        }
        interior.Unlocked = unlocked != 0;
        for (std::size_t j = 0; j < i; ++j) {
            if (candidate.m_Interiors[j].Id == interior.Id) {
                error = "progression duplicate interior";
                return NativeProgressionStatus::CorruptSave;
            }
        }
    }
    if (reader.At != reader.Bytes.size()) { error = "progression trailing payload"; return NativeProgressionStatus::CorruptSave; }
    candidate.m_Revision = revision;
    *this = std::move(candidate);
    error.clear();
    return NativeProgressionStatus::Ok;
}
