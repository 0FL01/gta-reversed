#pragma once

#include "NativeCarGenerators.h"
#include "NativeCollisionAssets.h"
#include "NativeVehicleFamilies.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

enum class NativeVehicleFamilyLifecycleStatus : std::uint8_t {
    Ok,
    InvalidInput,
    StaleReference,
    CapacityExceeded,
    Occupied,
    PoolError,
    Overflow,
};

struct NativeVehicleFamilyLifecycleRecord {
    NativeVehicleRef Reference;
    std::uint64_t Epoch = 0;
    std::int32_t ModelId = -1;
    std::string ModelName;
    NativeVehicleType Family = NativeVehicleType::Unsupported;
    NativeVehicleConstructor Constructor = NativeVehicleConstructor::Automobile;
    std::shared_ptr<const NativeCollisionModel> Collision;
    float Health = 1000.0f;
    std::uint64_t Driver = 0;
    std::array<std::uint64_t, 8> Passengers{};
    std::uint8_t MaximumPassengers = 0;
    std::uint32_t CollisionContacts = 0;
    std::uint64_t DamageRevision = 0;
    bool Destroyed = false;
    bool InWorld = true;
    bool operator==(const NativeVehicleFamilyLifecycleRecord&) const = default;
};

struct NativeVehicleFamilyLifecycleSnapshot {
    std::uint64_t Epoch = 0;
    std::uint64_t Revision = 0;
    std::vector<NativeVehicleFamilyLifecycleRecord> Records;
};

// Cross-family value lifetime over the shared source vehicle pool. Collision
// response supplies a resolved damage amount; this owner commits health,
// occupants, destruction and reload cleanup without inventing class physics.
class NativeVehicleFamilyLifecycle {
public:
    NativeVehicleFamilyLifecycle();

    NativeVehicleFamilyLifecycleStatus Spawn(const NativeCarGeneratorModelDefinition& definition,
        std::shared_ptr<const NativeCollisionModel> collision, std::uint8_t maximumPassengers,
        NativeVehicleRef& out, std::string& error);
    NativeVehicleFamilyLifecycleStatus SetDriver(NativeVehicleRef, std::uint64_t identity, std::string& error);
    NativeVehicleFamilyLifecycleStatus AddPassenger(NativeVehicleRef, std::uint64_t identity,
        std::uint8_t seat, std::string& error);
    NativeVehicleFamilyLifecycleStatus ApplyResolvedCollision(NativeVehicleRef,
        std::uint32_t contacts, float damage, std::string& error);
    NativeVehicleFamilyLifecycleStatus Destroy(NativeVehicleRef, std::string& error);
    NativeVehicleFamilyLifecycleStatus Reload(std::uint64_t nextEpoch, std::string& error);

    const NativeVehicleFamilyLifecycleRecord* Resolve(NativeVehicleRef) const noexcept;
    std::shared_ptr<const NativeVehicleFamilyLifecycleSnapshot> Publish() const;
    const NativeVehiclePool& Pool() const noexcept { return m_Pool; }
    std::uint64_t Epoch() const noexcept { return m_Epoch; }
    std::uint64_t Revision() const noexcept { return m_Revision; }
    static constexpr bool ResolvesFamilyDamageFormula = false;

private:
    NativeVehicleFamilyLifecycleRecord* ResolveMutable(NativeVehicleRef) noexcept;

    NativeVehiclePool m_Pool;
    std::array<std::optional<NativeVehicleFamilyLifecycleRecord>, NativeVehiclePoolCapacity> m_Records;
    std::uint64_t m_Epoch = 1;
    std::uint64_t m_Revision = 0;
};
