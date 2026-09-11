// Owned source-index vehicle authority for the native host.
#pragma once

#include "app/platform/linux/NativeGarages.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

struct NativeVehicleRef {
    std::int32_t Value = -1;
    bool operator==(const NativeVehicleRef&) const = default;
};

enum class NativeVehicleCreatedBy : std::uint8_t {
    Unsupported = 0,
    Random = 1,
    Mission = 2,
    Parked = 3,
    Permanent = 4,
};

enum class NativeVehicleType : std::uint8_t {
    Automobile = 0,
    MonsterTruck = 1,
    Quad = 2,
    Helicopter = 3,
    Plane = 4,
    Boat = 5,
    Train = 6,
    FakeHelicopter = 7,
    FakePlane = 8,
    Bike = 9,
    Bmx = 10,
    Trailer = 11,
    Unsupported = 0xff,
};

enum class NativeVehicleStatus : std::uint8_t {
    Player = 0,
    PlayerPlayback = 1,
    Simple = 2,
    Physics = 3,
    Abandoned = 4,
    Wrecked = 5,
    TrainMoving = 6,
    TrainNotMoving = 7,
    Helicopter = 8,
    Plane = 9,
    RemoteControlled = 10,
    PlayerDisabled = 11,
    Unsupported = 0xff,
};

enum class NativeVehicleProducer : std::uint8_t {
    NativeScm,
    NativeGameplayController,
    CarGenerator,
    RandomTraffic,
    SetPiece,
    Helicopter,
    Train,
    SaveLoad,
    Count,
};

constexpr std::size_t NativeVehicleProducerCount = static_cast<std::size_t>(NativeVehicleProducer::Count);
constexpr std::size_t NativeVehiclePoolCapacity = 110;

struct NativeVehicleProducerExtent {
    std::array<bool, NativeVehicleProducerCount> Owned{};
    bool NativeHostComplete = false;
    // The native bridge cannot infer occupancy of missing original producers.
    bool SourceParityComplete = false;
};

struct NativeVehicleState {
    std::int32_t ModelId = -1;
    NativeVehicleType Type = NativeVehicleType::Unsupported;
    std::int32_t SubType = -1;
    NativeVehicleStatus Status = NativeVehicleStatus::Unsupported;
    NativeVehicleCreatedBy CreatedBy = NativeVehicleCreatedBy::Unsupported;
    bool MissionCleanupRegistered = false;
    bool ScriptLocked = false;
    bool InWorld = false;
    NativeGarageMatrix Matrix;
    // Required before InWorld=true. Far TidyUpGarage deliberately does not read it.
    std::shared_ptr<const NativeCollisionModel> Collision;
    bool operator==(const NativeVehicleState&) const = default;
};

struct NativeVehicleCreateRequest {
    NativeVehicleProducer Producer = NativeVehicleProducer::Count;
    std::optional<NativeScriptRequestId> ScriptRequest;
    std::int32_t ProducerIndex = -1;
    NativeVehicleState State;
};

struct NativeVehicleRecord {
    NativeVehicleRef Reference;
    NativeVehicleProducer Producer = NativeVehicleProducer::Count;
    std::optional<NativeScriptRequestId> ScriptRequest;
    std::int32_t ProducerIndex = -1;
    NativeVehicleState State;
    bool operator==(const NativeVehicleRecord&) const = default;
};

enum class NativeVehicleEventKind : std::uint8_t {
    Allocated,
    Updated,
    Released,
};

struct NativeVehicleEvent {
    std::uint64_t Sequence = 0;
    std::uint64_t Revision = 0;
    NativeVehicleEventKind Kind = NativeVehicleEventKind::Allocated;
    NativeVehicleRecord Record;
};

struct NativeVehicleCensus {
    std::size_t Alive = 0;
    std::size_t CreatedEvents = 0;
    std::size_t UpdatedEvents = 0;
    std::size_t ReleasedEvents = 0;
    std::uint64_t Revision = 0;
    NativeVehicleProducerExtent Producers;
};

class NativeVehiclePoolSnapshot {
public:
    std::uint64_t Owner() const { return m_Owner; }
    std::uint64_t Frame() const { return m_Frame; }
    std::uint64_t Generation() const { return m_Generation; }
    std::uint64_t Revision() const { return m_Census.Revision; }
    const NativeVehicleCensus& Census() const { return m_Census; }
    const NativeVehicleRecord* AtSlot(std::size_t slot) const {
        return slot < m_Slots.size() && m_Slots[slot] ? &*m_Slots[slot] : nullptr;
    }
    std::span<const NativeVehicleEvent> Events() const { return m_Events; }

private:
    friend class NativeVehiclePool;
    NativeVehiclePoolSnapshot() = default;

    std::uint64_t m_Owner = 0;
    std::uint64_t m_Frame = 0;
    std::uint64_t m_Generation = 0;
    NativeVehicleCensus m_Census;
    std::array<std::optional<NativeVehicleRecord>, NativeVehiclePoolCapacity> m_Slots;
    std::vector<NativeVehicleEvent> m_Events;
};

class NativeVehiclePool {
public:
    static constexpr std::size_t Capacity = NativeVehiclePoolCapacity;

    NativeVehiclePool();
    NativeVehiclePool(const NativeVehiclePool&) = delete;
    NativeVehiclePool& operator=(const NativeVehiclePool&) = delete;

    // Startup contract: bind every producer present in this native mode, then
    // seal. Unbound producers cannot allocate and unsealed snapshots are not a
    // complete empty-pool proof.
    bool BindProducer(NativeVehicleProducer producer, std::string& error);
    bool SealProducerExtent(std::string& error);
    NativeScriptReferenceResult<NativeVehicleRef> Allocate(const NativeVehicleCreateRequest& request);
    bool Update(NativeVehicleRef reference, const NativeVehicleState& state, std::string& error);
    // The producer calls Release only after its real world/render/reference
    // destruction consumers have completed. Garage planning never calls it.
    bool Release(NativeVehicleRef reference, std::string& error);

    const NativeVehicleRecord* Resolve(NativeVehicleRef reference) const;
    const NativeVehicleRecord* AtSlot(std::size_t slot) const;
    NativeVehicleCensus Census() const;
    std::span<const NativeVehicleEvent> Events() const { return m_Events; }
    const NativeVehicleProducerExtent& ProducerExtent() const { return m_Producers; }
    std::shared_ptr<const NativeVehiclePoolSnapshot> Publish(std::uint64_t frame, std::string& error);

    std::uint64_t Owner() const { return m_Owner; }
    std::uint64_t Revision() const { return m_Revision; }
    std::uint64_t PublicationGeneration() const { return m_PublicationGeneration; }

private:
    std::array<std::optional<NativeVehicleRecord>, Capacity> m_Slots;
    std::array<std::uint8_t, Capacity> m_SlotGenerations{};
    std::vector<NativeVehicleEvent> m_Events;
    NativeVehicleProducerExtent m_Producers;
    std::optional<std::uint64_t> m_LastPublishedFrame;
    std::uint64_t m_Owner = 0;
    std::uint64_t m_Revision = 0;
    std::uint64_t m_PublicationGeneration = 0;
    bool m_ProducerExtentSealed = false;
};
