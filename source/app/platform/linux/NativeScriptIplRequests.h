#pragma once

#include "app/platform/linux/NativeScriptSession.h"

#include <array>
#include <cstddef>
#include <span>

struct NativeScriptIplRequestState {
    std::array<char, 8> Name{};
    bool Requested = false;
    bool operator==(const NativeScriptIplRequestState&) const = default;
};

class NativeScriptIplRequests {
public:
    static constexpr std::size_t Capacity = 256;
    static constexpr bool RuntimeStreaming = false;

    NativeScriptServiceResult Set(const NativeScriptIplRequest& request);
    std::span<const NativeScriptIplRequestState> Entries() const { return {m_Entries.data(), m_Count}; }

private:
    std::array<NativeScriptIplRequestState, Capacity> m_Entries{};
    std::size_t m_Count = 0;
};
