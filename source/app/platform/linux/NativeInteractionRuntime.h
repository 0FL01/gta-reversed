#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

enum class NativeInteractionKind : std::uint8_t { Garage, Property, Shop };
struct NativeInteractionRecord {
    std::array<char, 16> Name{};
    NativeInteractionKind Kind = NativeInteractionKind::Property;
    std::uint32_t Price = 0;
    bool Owned = false;
    std::uint32_t Uses = 0;
    std::uint32_t Purchases = 0;
    bool operator==(const NativeInteractionRecord&) const = default;
};
enum class NativeInteractionStatus : std::uint8_t {
    Ok, InvalidInput, Duplicate, NotFound, NotOwned, InsufficientFunds,
    Unsupported, CorruptSave, Overflow,
};

class NativeInteractionRuntime {
public:
    static constexpr std::size_t Capacity = 64;
    NativeInteractionStatus SetMoney(std::int32_t value, std::string& error);
    NativeInteractionStatus Register(std::string_view name, NativeInteractionKind,
        std::uint32_t price, bool owned, std::string& error);
    NativeInteractionStatus Buy(std::string_view name, std::string& error);
    NativeInteractionStatus Use(std::string_view name, std::string& error);
    NativeInteractionStatus Encode(std::vector<std::uint8_t>& out, std::string& error) const;
    NativeInteractionStatus Restore(std::span<const std::uint8_t> bytes, std::string& error);
    const NativeInteractionRecord* Find(std::string_view name) const noexcept;
    std::int32_t Money() const noexcept { return m_Money; }
    std::uint64_t Revision() const noexcept { return m_Revision; }
    std::span<const NativeInteractionRecord> Records() const { return {m_Records.data(), m_Count}; }

private:
    NativeInteractionRecord* FindMutable(std::string_view) noexcept;
    std::array<NativeInteractionRecord, Capacity> m_Records{};
    std::size_t m_Count = 0;
    std::int32_t m_Money = 0;
    std::uint64_t m_Revision = 0;
};
