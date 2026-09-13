#include "app/platform/linux/NativeScriptObjects.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace {
bool Finite(const NativeScriptPosition& position) {
    return std::isfinite(position.X) && std::isfinite(position.Y) && std::isfinite(position.Z);
}

std::size_t NameLength(const std::array<char, 24>& name) {
    const auto end = std::find(name.begin(), name.end(), '\0');
    return std::size_t(end - name.begin());
}
}

NativeScriptReferenceResult<NativeScriptObjectRef> NativeScriptObjects::Create(
    const NativeScriptObjectRequest& request, const NativeScriptObjectSource& source, bool noOffset) {
    NativeScriptReferenceResult<NativeScriptObjectRef> result;
    if (!Finite(request.Position) || source.ModelId < 0 || NameLength(source.Name) == 0) {
        result.Result = {NativeScriptServiceStatus::Error, "object identity, position or source collision is invalid"};
        return result;
    }
    if (request.Position.Z <= -100.0f) {
        result.Result = {NativeScriptServiceStatus::Unsupported, "object no-offset ground fallback has no source ground owner"};
        return result;
    }
    const auto slot = std::find_if(m_Slots.begin(), m_Slots.end(), [](const Slot& candidate) {
        return !candidate.Object.has_value();
    });
    if (slot == m_Slots.end()) {
        result.Result = {NativeScriptServiceStatus::Error, "source object pool is full"};
        return result;
    }
    if (slot->Generation == std::numeric_limits<std::uint8_t>::max()) {
        result.Result = {NativeScriptServiceStatus::Error, "source object generation exhausted"};
        return result;
    }
    auto& mutableSlot = const_cast<Slot&>(*slot);
    ++mutableSlot.Generation;
    const auto index = std::size_t(&mutableSlot - m_Slots.data());
    NativeScriptObject object;
    object.Reference = {static_cast<std::int32_t>((std::uint32_t(mutableSlot.Generation) << 16) | index)};
    object.ModelOperand = request.ModelId;
    object.ModelId = source.ModelId;
    object.Name = source.Name;
    object.Position = request.Position;
    if (!noOffset) {
        if (!source.Collision || source.Collision->Empty ||
            (source.Collision->Spheres.empty() && source.Collision->Boxes.empty() && source.Collision->Faces.empty()) ||
            !std::isfinite(source.Collision->Min[2])) {
            result.Result = {NativeScriptServiceStatus::Error, "source object COL base is nonfinite"};
            return result;
        }
        object.Position.Z -= source.Collision->Min[2];
    }
    object.Collision = source.Collision;
    mutableSlot.Object = std::move(object);
    ++m_Live;
    if (m_Revision == std::numeric_limits<std::uint64_t>::max()) {
        mutableSlot.Object.reset();
        --m_Live;
        result.Result = {NativeScriptServiceStatus::Error, "source object revision exhausted"};
        return result;
    }
    ++m_Revision;
    result.Reference = mutableSlot.Object->Reference;
    result.Result = {NativeScriptServiceStatus::Ready, {}};
    return result;
}

NativeScriptObjectStatus NativeScriptObjects::Remove(NativeScriptObjectRef reference, std::string& error) {
    if (reference.Value < 0) {
        error = "invalid source object reference";
        return NativeScriptObjectStatus::InvalidInput;
    }
    const auto value = std::uint32_t(reference.Value);
    const auto index = std::size_t(value & 0xffffu);
    const auto generation = std::uint8_t(value >> 16);
    if (index >= m_Slots.size() || !m_Slots[index].Object || m_Slots[index].Generation != generation) {
        error = "stale source object reference";
        return NativeScriptObjectStatus::InvalidInput;
    }
    m_Slots[index].Object.reset();
    --m_Live;
    ++m_Revision;
    error.clear();
    return NativeScriptObjectStatus::Ok;
}

NativeScriptObjectStatus NativeScriptObjects::SetHeading(NativeScriptObjectRef reference, float degrees, std::string& error) {
    if (!std::isfinite(degrees)) {
        error = "nonfinite source object heading";
        return NativeScriptObjectStatus::InvalidInput;
    }
    const auto value = std::uint32_t(reference.Value);
    const auto index = reference.Value < 0 ? Capacity : std::size_t(value & 0xffffu);
    const auto generation = std::uint8_t(value >> 16);
    if (index >= m_Slots.size() || !m_Slots[index].Object || m_Slots[index].Generation != generation) {
        error = "stale source object reference";
        return NativeScriptObjectStatus::InvalidInput;
    }
    if (degrees < 0) degrees += 360.0f;
    else if (degrees > 360.0f) degrees -= 360.0f;
    if (!std::isfinite(degrees)) {
        error = "source object heading overflow";
        return NativeScriptObjectStatus::Overflow;
    }
    m_Slots[index].Object->HeadingDegrees = degrees;
    ++m_Revision;
    error.clear();
    return NativeScriptObjectStatus::Ok;
}

NativeScriptObjectStatus NativeScriptObjects::MarkNoLongerNeeded(NativeScriptObjectRef reference, std::string& error) {
    const auto value = std::uint32_t(reference.Value);
    const auto index = reference.Value < 0 ? Capacity : std::size_t(value & 0xffffu);
    const auto generation = std::uint8_t(value >> 16);
    if (index >= m_Slots.size() || !m_Slots[index].Object || m_Slots[index].Generation != generation) {
        error = "stale source object reference";
        return NativeScriptObjectStatus::InvalidInput;
    }
    m_Slots[index].Object->Mission = false;
    ++m_Revision;
    error.clear();
    return NativeScriptObjectStatus::Ok;
}

NativeScriptObjectStatus NativeScriptObjects::SetCollisionDamageEffect(
    NativeScriptObjectRef reference, std::int32_t effect, std::string& error) {
    const auto value = std::uint32_t(reference.Value);
    const auto index = reference.Value < 0 ? Capacity : std::size_t(value & 0xffffu);
    const auto generation = std::uint8_t(value >> 16);
    if (index >= m_Slots.size() || !m_Slots[index].Object || m_Slots[index].Generation != generation) {
        error = "stale source object reference";
        return NativeScriptObjectStatus::InvalidInput;
    }
    if (effect < 0 || effect > 255) {
        error = "source object collision damage effect is out of byte range";
        return NativeScriptObjectStatus::InvalidInput;
    }
    m_Slots[index].Object->CollisionDamageEffect = std::uint8_t(effect);
    ++m_Revision;
    error.clear();
    return NativeScriptObjectStatus::Ok;
}

NativeScriptObjectStatus NativeScriptObjects::SetStatic(NativeScriptObjectRef reference, bool frozen, std::string& error) {
    const auto value = std::uint32_t(reference.Value);
    const auto index = reference.Value < 0 ? Capacity : std::size_t(value & 0xffffu);
    const auto generation = std::uint8_t(value >> 16);
    if (index >= m_Slots.size() || !m_Slots[index].Object || m_Slots[index].Generation != generation) {
        error = "stale source object reference";
        return NativeScriptObjectStatus::InvalidInput;
    }
    m_Slots[index].Object->Static = frozen;
    ++m_Revision;
    error.clear();
    return NativeScriptObjectStatus::Ok;
}

NativeScriptObjectStatus NativeScriptObjects::SetVelocity(
    NativeScriptObjectRef reference, NativeScriptPosition velocity, std::string& error) {
    const auto value = std::uint32_t(reference.Value);
    const auto index = reference.Value < 0 ? Capacity : std::size_t(value & 0xffffu);
    const auto generation = std::uint8_t(value >> 16);
    if (index >= m_Slots.size() || !m_Slots[index].Object || m_Slots[index].Generation != generation) {
        error = "stale source object reference";
        return NativeScriptObjectStatus::InvalidInput;
    }
    if (!Finite(velocity)) {
        error = "nonfinite source object velocity";
        return NativeScriptObjectStatus::InvalidInput;
    }
    m_Slots[index].Object->Velocity = velocity;
    ++m_Revision;
    error.clear();
    return NativeScriptObjectStatus::Ok;
}

NativeScriptObjectStatus NativeScriptObjects::SetProofs(NativeScriptObjectRef reference, std::uint8_t proofs, std::string& error) {
    const auto value = std::uint32_t(reference.Value);
    const auto index = reference.Value < 0 ? Capacity : std::size_t(value & 0xffffu);
    const auto generation = std::uint8_t(value >> 16);
    if (index >= m_Slots.size() || !m_Slots[index].Object || m_Slots[index].Generation != generation) {
        error = "stale source object reference";
        return NativeScriptObjectStatus::InvalidInput;
    }
    m_Slots[index].Object->Proofs = proofs;
    ++m_Revision;
    error.clear();
    return NativeScriptObjectStatus::Ok;
}

NativeScriptObjectStatus NativeScriptObjects::SetRotation(
    NativeScriptObjectRef reference, NativeScriptPosition rotation, bool relative, std::string& error) {
    const auto value = std::uint32_t(reference.Value);
    const auto index = reference.Value < 0 ? Capacity : std::size_t(value & 0xffffu);
    const auto generation = std::uint8_t(value >> 16);
    if (index >= m_Slots.size() || !m_Slots[index].Object || m_Slots[index].Generation != generation) {
        error = "stale source object reference";
        return NativeScriptObjectStatus::InvalidInput;
    }
    if (!std::isfinite(rotation.X) || !std::isfinite(rotation.Y) || !std::isfinite(rotation.Z)) {
        error = "nonfinite source object rotation";
        return NativeScriptObjectStatus::InvalidInput;
    }
    m_Slots[index].Object->Rotation = rotation;
    m_Slots[index].Object->RelativeRotation = relative;
    ++m_Revision;
    error.clear();
    return NativeScriptObjectStatus::Ok;
}

NativeScriptObjectStatus NativeScriptObjects::SetArea(NativeScriptObjectRef reference, std::int32_t area, std::string& error) {
    const auto value = std::uint32_t(reference.Value);
    const auto index = reference.Value < 0 ? Capacity : std::size_t(value & 0xffffu);
    const auto generation = std::uint8_t(value >> 16);
    if (index >= m_Slots.size() || !m_Slots[index].Object || m_Slots[index].Generation != generation) {
        error = "stale source object reference";
        return NativeScriptObjectStatus::InvalidInput;
    }
    if (area < 0 || area > 255) {
        error = "source object area is out of byte range";
        return NativeScriptObjectStatus::InvalidInput;
    }
    m_Slots[index].Object->Area = area;
    ++m_Revision;
    error.clear();
    return NativeScriptObjectStatus::Ok;
}

NativeScriptObjectStatus NativeScriptObjects::ConnectLods(
    NativeScriptObjectRef child, NativeScriptObjectRef parent, std::string& error) {
    const auto childValue = std::uint32_t(child.Value), parentValue = std::uint32_t(parent.Value);
    const auto childIndex = child.Value < 0 ? Capacity : std::size_t(childValue & 0xffffu);
    const auto parentIndex = parent.Value < 0 ? Capacity : std::size_t(parentValue & 0xffffu);
    if (childIndex >= m_Slots.size() || parentIndex >= m_Slots.size() || childIndex == parentIndex ||
        !m_Slots[childIndex].Object || !m_Slots[parentIndex].Object ||
        m_Slots[childIndex].Generation != std::uint8_t(childValue >> 16) ||
        m_Slots[parentIndex].Generation != std::uint8_t(parentValue >> 16)) {
        error = "stale or aliased source LOD object reference";
        return NativeScriptObjectStatus::InvalidInput;
    }
    m_Slots[childIndex].Object->LodParent = parent;
    ++m_Revision;
    error.clear();
    return NativeScriptObjectStatus::Ok;
}

NativeScriptObjectStatus NativeScriptObjects::GetOffsetInWorld(
    NativeScriptObjectRef reference, NativeScriptPosition offset, NativeScriptPosition& output,
    std::string& error) const {
    const auto value = std::uint32_t(reference.Value);
    const auto index = reference.Value < 0 ? Capacity : std::size_t(value & 0xffffu);
    const auto generation = std::uint8_t(value >> 16);
    if (index >= m_Slots.size() || !m_Slots[index].Object || m_Slots[index].Generation != generation) {
        error = "stale source object reference";
        return NativeScriptObjectStatus::InvalidInput;
    }
    if (!Finite(offset)) {
        error = "nonfinite source object offset";
        return NativeScriptObjectStatus::InvalidInput;
    }
    const auto& object = *m_Slots[index].Object;
    if (object.Rotation.X != 0 || object.Rotation.Y != 0) {
        error = "source object world offset needs an unowned 3D matrix";
        return NativeScriptObjectStatus::Unsupported;
    }
    const float radians = object.HeadingDegrees * (3.14159265358979323846f / 180.0f);
    const float sine = std::sin(radians), cosine = std::cos(radians);
    output = {object.Position.X + offset.X * cosine - offset.Y * sine,
        object.Position.Y + offset.X * sine + offset.Y * cosine,
        object.Position.Z + offset.Z};
    if (!Finite(output)) {
        error = "source object world offset overflow";
        return NativeScriptObjectStatus::Overflow;
    }
    error.clear();
    return NativeScriptObjectStatus::Ok;
}

const NativeScriptObject* NativeScriptObjects::Resolve(NativeScriptObjectRef reference) const noexcept {
    if (reference.Value < 0) return nullptr;
    const auto value = std::uint32_t(reference.Value);
    const auto index = std::size_t(value & 0xffffu);
    const auto generation = std::uint8_t(value >> 16);
    if (index >= m_Slots.size() || !m_Slots[index].Object || m_Slots[index].Generation != generation) return nullptr;
    return &*m_Slots[index].Object;
}
