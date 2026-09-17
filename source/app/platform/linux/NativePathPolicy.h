#pragma once
#include "app/platform/linux/NativeScriptSession.h"
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

struct NativePathPolicyEntry {
    NativePathPolicyKind Kind{};
    std::array<float, 3> Min{}, Max{};
    bool operator==(const NativePathPolicyEntry&) const = default;
};
class NativePathPolicy {
public:
    static constexpr bool PathConsumer = false;
    NativeScriptServiceResult Add(const NativeScriptPathPolicyRequest&);
    std::span<const NativePathPolicyEntry> Entries() const { return m_Entries; }
    std::uint64_t Revision() const { return m_Revision; }
private:
    std::vector<NativePathPolicyEntry> m_Entries;
    std::uint64_t m_Revision = 0;
};
