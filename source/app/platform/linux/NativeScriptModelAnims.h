#pragma once

#include "app/platform/linux/NativeScriptSession.h"

#include <array>
#include <cstddef>
#include <span>

struct NativeScriptModelAnimBinding {
    std::int32_t ModelId = -1;
    std::array<char, 16> IfpName{};
    bool operator==(const NativeScriptModelAnimBinding&) const = default;
};

class NativeScriptModelAnims {
public:
    static constexpr std::size_t Capacity = 8;
    static constexpr bool RuntimeApplication = false;

    NativeScriptServiceResult Add(const NativeScriptModelAnimRequest& request);
    std::span<const NativeScriptModelAnimBinding> Entries() const { return {m_Entries.data(), m_Count}; }

private:
    std::array<NativeScriptModelAnimBinding, Capacity> m_Entries{};
    std::size_t m_Count = 0;
};
