#include "NativeInteractionRuntime.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

namespace {
constexpr std::array<std::uint8_t, 8> kMagic{'M','A','D','S','A','I','N','T'};
void U32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) out.push_back(std::uint8_t(value >> (i * 8)));
}
void U64(std::vector<std::uint8_t>& out, std::uint64_t value) {
    for (unsigned i = 0; i < 8; ++i) out.push_back(std::uint8_t(value >> (i * 8)));
}
std::uint32_t Read32(std::span<const std::uint8_t> bytes, std::size_t offset) {
    return std::uint32_t(bytes[offset]) | std::uint32_t(bytes[offset+1]) << 8 |
        std::uint32_t(bytes[offset+2]) << 16 | std::uint32_t(bytes[offset+3]) << 24;
}
std::uint64_t Read64(std::span<const std::uint8_t> bytes, std::size_t offset) {
    std::uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) value |= std::uint64_t(bytes[offset+i]) << (i*8);
    return value;
}
std::uint64_t Hash(std::span<const std::uint8_t> bytes) {
    std::uint64_t value = 1469598103934665603ull;
    for (const auto byte : bytes) value = (value ^ byte) * 1099511628211ull;
    return value;
}
std::string_view Name(const std::array<char, 16>& name) {
    const auto end = std::find(name.begin(), name.end(), '\0');
    return {name.data(), std::size_t(end - name.begin())};
}
}

NativeInteractionStatus NativeInteractionRuntime::SetMoney(std::int32_t value, std::string& error) {
    if (value < 0) { error = "interaction money is invalid"; return NativeInteractionStatus::InvalidInput; }
    m_Money = value;
    ++m_Revision;
    error.clear();
    return NativeInteractionStatus::Ok;
}

const NativeInteractionRecord* NativeInteractionRuntime::Find(std::string_view name) const noexcept {
    for (std::size_t i = 0; i < m_Count; ++i) if (Name(m_Records[i].Name) == name) return &m_Records[i];
    return nullptr;
}
NativeInteractionRecord* NativeInteractionRuntime::FindMutable(std::string_view name) noexcept {
    return const_cast<NativeInteractionRecord*>(std::as_const(*this).Find(name));
}

NativeInteractionStatus NativeInteractionRuntime::Register(std::string_view name,
    NativeInteractionKind kind, std::uint32_t price, bool owned, std::string& error) {
    if (name.empty() || name.size() >= 16 || kind > NativeInteractionKind::Shop ||
        (kind == NativeInteractionKind::Garage && price)) {
        error = "interaction registration is invalid";
        return NativeInteractionStatus::InvalidInput;
    }
    if (Find(name)) { error = "interaction name is duplicated"; return NativeInteractionStatus::Duplicate; }
    if (m_Count == m_Records.size()) { error = "interaction capacity exceeded"; return NativeInteractionStatus::Overflow; }
    auto& record = m_Records[m_Count++];
    record = {};
    std::copy(name.begin(), name.end(), record.Name.begin());
    record.Kind = kind;
    record.Price = price;
    record.Owned = owned || kind != NativeInteractionKind::Property;
    ++m_Revision;
    error.clear();
    return NativeInteractionStatus::Ok;
}

NativeInteractionStatus NativeInteractionRuntime::Buy(std::string_view name, std::string& error) {
    auto* record = FindMutable(name);
    if (!record) { error = "interaction is not registered"; return NativeInteractionStatus::NotFound; }
    if (record->Kind == NativeInteractionKind::Garage) { error = "garage cannot be purchased"; return NativeInteractionStatus::Unsupported; }
    if (record->Kind == NativeInteractionKind::Property && record->Owned) { error = "property is already owned"; return NativeInteractionStatus::Duplicate; }
    if (std::uint32_t(m_Money) < record->Price) { error = "interaction funds are insufficient"; return NativeInteractionStatus::InsufficientFunds; }
    m_Money -= std::int32_t(record->Price);
    if (record->Kind == NativeInteractionKind::Property) record->Owned = true;
    else ++record->Purchases;
    ++m_Revision;
    error.clear();
    return NativeInteractionStatus::Ok;
}

NativeInteractionStatus NativeInteractionRuntime::Use(std::string_view name, std::string& error) {
    auto* record = FindMutable(name);
    if (!record) { error = "interaction is not registered"; return NativeInteractionStatus::NotFound; }
    if (!record->Owned) { error = "interaction property is not owned"; return NativeInteractionStatus::NotOwned; }
    if (record->Uses == std::numeric_limits<std::uint32_t>::max()) { error = "interaction use count exhausted"; return NativeInteractionStatus::Overflow; }
    ++record->Uses;
    ++m_Revision;
    error.clear();
    return NativeInteractionStatus::Ok;
}

NativeInteractionStatus NativeInteractionRuntime::Encode(std::vector<std::uint8_t>& out, std::string& error) const {
    std::vector<std::uint8_t> next(kMagic.begin(), kMagic.end());
    U32(next, 1);
    U32(next, std::uint32_t(m_Money));
    U32(next, std::uint32_t(m_Count));
    U64(next, m_Revision);
    for (std::size_t i = 0; i < m_Count; ++i) {
        const auto& record = m_Records[i];
        next.insert(next.end(), reinterpret_cast<const std::uint8_t*>(record.Name.data()),
            reinterpret_cast<const std::uint8_t*>(record.Name.data() + record.Name.size()));
        next.push_back(std::uint8_t(record.Kind));
        next.push_back(record.Owned ? 1 : 0);
        U32(next, record.Price);
        U32(next, record.Uses);
        U32(next, record.Purchases);
    }
    U64(next, Hash(next));
    out = std::move(next);
    error.clear();
    return NativeInteractionStatus::Ok;
}

NativeInteractionStatus NativeInteractionRuntime::Restore(std::span<const std::uint8_t> bytes, std::string& error) {
    constexpr std::size_t header = 8 + 4 + 4 + 4 + 8;
    constexpr std::size_t recordSize = 16 + 1 + 1 + 4 + 4 + 4;
    if (bytes.size() < header + 8 || !std::equal(kMagic.begin(), kMagic.end(), bytes.begin()) || Read32(bytes, 8) != 1) {
        error = "interaction save header is invalid";
        return NativeInteractionStatus::CorruptSave;
    }
    const auto count = Read32(bytes, 16);
    if (count > Capacity || bytes.size() != header + std::size_t(count) * recordSize + 8 ||
        Hash(bytes.first(bytes.size() - 8)) != Read64(bytes, bytes.size() - 8)) {
        error = "interaction save size or checksum is invalid";
        return NativeInteractionStatus::CorruptSave;
    }
    NativeInteractionRuntime next;
    const auto money = Read32(bytes, 12);
    if (money > std::uint32_t(std::numeric_limits<std::int32_t>::max())) {
        error = "interaction save money is invalid";
        return NativeInteractionStatus::CorruptSave;
    }
    next.m_Money = std::int32_t(money);
    next.m_Count = count;
    next.m_Revision = Read64(bytes, 20);
    std::size_t offset = header;
    for (std::size_t i = 0; i < count; ++i) {
        auto& record = next.m_Records[i];
        std::memcpy(record.Name.data(), bytes.data() + offset, 16); offset += 16;
        record.Kind = NativeInteractionKind(bytes[offset++]);
        record.Owned = bytes[offset++] != 0;
        record.Price = Read32(bytes, offset); offset += 4;
        record.Uses = Read32(bytes, offset); offset += 4;
        record.Purchases = Read32(bytes, offset); offset += 4;
        bool duplicate = false;
        for (std::size_t j = 0; j < i; ++j) duplicate |= Name(next.m_Records[j].Name) == Name(record.Name);
        if (Name(record.Name).empty() || record.Kind > NativeInteractionKind::Shop || duplicate) {
            error = "interaction save record is invalid";
            return NativeInteractionStatus::CorruptSave;
        }
    }
    *this = std::move(next);
    error.clear();
    return NativeInteractionStatus::Ok;
}
