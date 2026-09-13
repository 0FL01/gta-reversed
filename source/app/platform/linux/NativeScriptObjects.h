// Owned mission-object registry for the source CREATE_OBJECT_NO_OFFSET slice.
// It carries source identity and lifetime, but does not pretend to render or
// simulate objects before those owners are ported.
#pragma once

#include "app/platform/linux/NativeCollisionAssets.h"
#include "app/platform/linux/NativeScriptSession.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

struct NativeScriptObjectSource {
    std::int32_t ModelId = -1;
    std::array<char, 24> Name{};
    std::shared_ptr<const NativeCollisionModel> Collision;
};

struct NativeScriptObject {
    NativeScriptObjectRef Reference;
    std::int32_t ModelOperand = -1, ModelId = -1;
    std::array<char, 24> Name{};
    NativeScriptPosition Position;
    std::shared_ptr<const NativeCollisionModel> Collision;
    float HeadingDegrees = 0;
    std::uint8_t CollisionDamageEffect = 0;
    std::uint8_t Proofs = 0;
    NativeScriptPosition Rotation;
    NativeScriptPosition Velocity;
    bool RelativeRotation = false;
    bool Static = true, Mission = true, InWorld = true;
    std::int32_t Area = 0;
    NativeScriptObjectRef LodParent;
    bool operator==(const NativeScriptObject&) const = default;
};

enum class NativeScriptObjectStatus : std::uint8_t { Ok, InvalidInput, Unsupported, Full, Overflow };

class NativeScriptObjects {
public:
    static constexpr std::size_t Capacity = 350; // source CObjectPool capacity

    NativeScriptReferenceResult<NativeScriptObjectRef> Create(
        const NativeScriptObjectRequest&, const NativeScriptObjectSource&, bool noOffset = true);
    NativeScriptObjectStatus SetHeading(NativeScriptObjectRef, float degrees, std::string& error);
    NativeScriptObjectStatus MarkNoLongerNeeded(NativeScriptObjectRef, std::string& error);
    NativeScriptObjectStatus SetCollisionDamageEffect(NativeScriptObjectRef, std::int32_t effect, std::string& error);
    NativeScriptObjectStatus SetStatic(NativeScriptObjectRef, bool frozen, std::string& error);
    NativeScriptObjectStatus SetVelocity(NativeScriptObjectRef, NativeScriptPosition, std::string& error);
    NativeScriptObjectStatus SetProofs(NativeScriptObjectRef, std::uint8_t proofs, std::string& error);
    NativeScriptObjectStatus SetRotation(NativeScriptObjectRef, NativeScriptPosition, bool relative, std::string& error);
    NativeScriptObjectStatus SetArea(NativeScriptObjectRef, std::int32_t area, std::string& error);
    NativeScriptObjectStatus ConnectLods(NativeScriptObjectRef child, NativeScriptObjectRef parent, std::string& error);
    NativeScriptObjectStatus GetOffsetInWorld(NativeScriptObjectRef, NativeScriptPosition offset,
        NativeScriptPosition& output, std::string& error) const;
    NativeScriptObjectStatus Remove(NativeScriptObjectRef, std::string& error);
    const NativeScriptObject* Resolve(NativeScriptObjectRef) const noexcept;
    std::size_t LiveCount() const noexcept { return m_Live; }
    std::uint64_t Revision() const noexcept { return m_Revision; }

private:
    struct Slot {
        std::optional<NativeScriptObject> Object;
        std::uint8_t Generation = 0;
    };
    std::array<Slot, Capacity> m_Slots{};
    std::size_t m_Live = 0;
    std::uint64_t m_Revision = 0;
};
