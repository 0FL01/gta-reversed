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
#include <vector>

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
    float Health = 1000.0f;
    std::uint64_t DamageRevision = 0;
    bool UsesCollision = true;
    bool Visible = true;
    bool RenderDamaged = false;
    bool Broken = false;
    bool operator==(const NativeScriptObject&) const = default;
};

struct NativeScriptObjectSnapshot {
    std::uint64_t Epoch = 0;
    std::uint64_t Revision = 0;
    std::vector<NativeScriptObject> Objects;
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
    NativeScriptObjectStatus ApplyDamage(NativeScriptObjectRef, float damage,
        float collisionDamageMultiplier, std::string& error);
    NativeScriptObjectStatus Reload(std::uint64_t nextEpoch, std::string& error);
    const NativeScriptObject* Resolve(NativeScriptObjectRef) const noexcept;
    std::shared_ptr<const NativeScriptObjectSnapshot> Publish() const;
    std::size_t LiveCount() const noexcept { return m_Live; }
    std::uint64_t Revision() const noexcept { return m_Revision; }

private:
    struct Slot {
        std::optional<NativeScriptObject> Object;
        std::uint8_t Generation = 0;
    };
    std::array<Slot, Capacity> m_Slots{};
    std::size_t m_Live = 0;
    std::uint64_t m_Epoch = 1;
    std::uint64_t m_Revision = 0;
};
