#pragma once

#include "app/platform/linux/NativeScriptSession.h"

#include <array>
#include <cstdint>

struct NativeScriptClothesPart {
    std::array<char, 16> Texture{};
    std::array<char, 16> Model{};
    bool operator==(const NativeScriptClothesPart&) const = default;
};

struct NativeScriptClothesState {
    std::array<NativeScriptClothesPart, 18> Parts{};
    std::array<NativeScriptClothesPart, 18> StoredParts{};
    std::uint64_t Revision = 0;
    std::uint64_t BuildRevision = 0;
    std::uint64_t StoreRevision = 0;
    bool HasStoredState = false;
    bool operator==(const NativeScriptClothesState&) const = default;
};

class NativeScriptClothes {
public:
    static constexpr bool RuntimeApplication = false;
    NativeScriptServiceResult Give(const NativeScriptClothesRequest& request);
    NativeScriptServiceResult Build(std::int32_t playerIndex);
    NativeScriptServiceResult Store();
    const NativeScriptClothesState& State() const { return m_State; }

private:
    NativeScriptClothesState m_State;
};
