#pragma once
#include "app/platform/linux/NativeScriptSession.h"
#include <array>
#include <cstdint>
#include <span>
#include <string>

enum class NativeExternalTriggerKind : std::uint8_t { PedModel, ObjectModel, CodeUse, AttractorCodeUse };
struct NativeExternalScriptTrigger {
    NativeExternalTriggerKind Kind{};
    std::uint8_t ScriptIndex = 0;
    std::int32_t ModelId = -1, Priority = 0, Type = 0;
    float Radius = 0;
    std::array<char, 24> ModelName{};
    bool operator==(const NativeExternalScriptTrigger&) const = default;
};
class NativeExternalScriptTriggers {
public:
    static constexpr std::size_t Capacity = 70;
    static constexpr bool RuntimeActivation = false;
    NativeScriptServiceResult Add(const NativeScriptExternalTriggerRequest&,
        std::span<const NativeScriptStreamedState> scripts);
    NativeScriptServiceResult AddCodeUse(const NativeScriptCodeBrainRequest&,
        std::span<const NativeScriptStreamedState> scripts);
    std::span<const NativeExternalScriptTrigger> Entries() const { return {m_Entries.data(), m_Count}; }
private:
    std::array<NativeExternalScriptTrigger, Capacity> m_Entries{};
    std::size_t m_Count = 0;
};
