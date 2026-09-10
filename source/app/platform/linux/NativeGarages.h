#pragma once
#include "app/platform/linux/NativeCollisionAssets.h"
#include "app/platform/linux/NativeScriptSession.h"
#include <optional>

struct NativeGarageRef {
    std::uint64_t Owner = 0;
    std::size_t Index = 0;
    bool operator==(const NativeGarageRef&) const = default;
};
struct NativeGarageMatrix {
    NativeCollisionVector Position{};
    std::array<NativeCollisionVector, 3> Basis{{{1,0,0},{0,1,0},{0,0,1}}};
    bool operator==(const NativeGarageMatrix&) const = default;
};
struct NativeGarageEntry {
    std::array<char, 8> Name{}; // AddOne copies seven bytes; owned eighth stays NUL
    std::string Ipl;
    std::uint32_t Line = 0, Record = 0;
    NativeCollisionVector Origin{};
    std::array<float, 2> DirectionA{}, DirectionB{};
    float Width = 0, Height = 0, Top = 0;
    std::array<float, 4> Rect{}; // minX, maxX, minY, maxY; all four corners
    std::uint8_t Type = 0, OriginalType = 0, Flags = 0, DoorState = 0;
    float DoorPosition = 0;
    std::uint32_t TimeToOpen = 0;
};
struct NativeGarageDoor {
    NativeCollisionPlacement Placement; // exact IPL + record + binary + model ID
    std::shared_ptr<const NativeCollisionModel> Collision;
    std::optional<NativeGarageRef> Garage; // original lookup may genuinely find none
    // SourcePose / CollisionEnabled are source-requested object state, NOT an
    // assertion that rendering or immutable world collision has applied it.
    // Replace the original placement by identity when wiring dynamic doors.
    NativeGarageMatrix Authored, SourcePose;
    bool CollisionEnabled = true;
    bool RequiresDynamicPublication = false;
};
struct NativeGarageEntityBounds {
    // Caller supplies the ACTUAL entity matrix and source COL (ped/vehicle).
    // No invented capsule, render bounds, or precomputed eligibility boolean.
    NativeGarageMatrix Matrix;
    std::shared_ptr<const NativeCollisionModel> Collision;
    int ModelId = -1, VehicleSubType = -1; // eVehicleType; source vehicle offset 0x594, BMX=10
};
struct NativeGarageView {
    NativeGarageEntityBounds Player;
    std::optional<NativeGarageEntityBounds> Vehicle;
    NativeCollisionVector Camera{};
    bool Replay = false, Coop = false;
    std::uint64_t Frame = 0;
    // Only the runtime adapter supplies this from the committed scene/COL pair.
    std::shared_ptr<const NativePlacementOverrides> PublishedOverrides;
};
struct NativeGaragePolicy {
    // CGarages::Init. A required respray wanted-state write remains Unsupported,
    // so these cannot drift away from the actual unmodified player policy.
    std::int32_t LastGaragePlayerWasIn = -1;
    bool NoResprays = false;
};
enum class NativeGarageRequirement {
    None, RestoreAndOpen, DoorMotion, DoorObstruction, VehicleCapacity, SourceTypeUpdate, TidyUp,
    WantedPolicy, ImpoundVehicles
};
struct NativeGarageUpdate {
    NativeGarageRef Garage;
    NativeScriptServiceStatus Status = NativeScriptServiceStatus::Ready;
    NativeGarageRequirement Requirement = NativeGarageRequirement::None;
    std::uint8_t FromState = 0, RequestedState = 0;
    bool InactiveClosed = false;
    std::vector<std::size_t> Doors; // indices in Doors(), not new drawables
};
struct NativeGarageCamera {
    bool Apply = false, Outside = false;
    std::optional<NativeGarageRef> Previous, Garage, AvoidFirstPerson;
};
struct NativeGarageFrame {
    NativeGarageCamera Camera;
    std::vector<NativeGarageUpdate> Updates;
    std::optional<NativeGarageRef> Maintenance;
    bool TidyClose = false;
};

class NativeGarages {
public:
    NativeGarages() = default;
    NativeGarages(const NativeGarages&) = delete;
    NativeGarages& operator=(const NativeGarages&) = delete;
    bool LoadBeforeWorker(const char* gameDir, const NativeCollisionContext& collision, std::string& error);
    void SealStartup() { m_Sealed = true; }
    const std::vector<NativeGarageEntry>& Entries() const { return m_Entries; }
    const std::vector<NativeGarageDoor>& Doors() const { return m_Doors; }
    // FileLoader::LoadPedObject binds this source-constructed shared shape to
    // MODEL_PLAYER; it is NOT a ped1 entry in the malformed peds.col archive.
    std::shared_ptr<const NativeCollisionModel> Ped1Collision() const { return m_Ped1Collision; }
    const NativeGarageFrame& Frame() const { return m_Frame; }
    const NativeGaragePolicy& Policy() const { return m_Policy; }
    std::uint64_t Revision() const { return m_Revision; } // script writes, not common per-frame collision flags
    std::size_t RejectedRows() const { return m_RejectedRows; }
    std::size_t IplFiles() const { return m_IplFiles; }
    std::optional<NativeGarageRef> Find(std::span<const char> name) const;
    const NativeGarageEntry* Resolve(NativeGarageRef ref) const;
    // Transactional publication; no assets/RW/scene reload and no door movement.
    // True means valid frame publication, not that every garage step is Ready.
    // Caller applies Camera then routes each Unsupported requirement explicitly.
    bool Tick(const NativeGarageView& view, std::string& error);
    // Pure source predicates, also usable with independent record/hull fixtures.
    static bool Contains(const NativeGarageEntry&, NativeCollisionVector point, float margin);
    static bool EntirelyInside(const NativeGarageEntry&, const NativeGarageEntityBounds&, float margin);
    static bool EntirelyOutside(const NativeGarageEntry&, const NativeGarageEntityBounds&, float margin);
    static float DistanceSquared(const NativeGarageEntry&, NativeCollisionVector point);
    static std::optional<std::size_t> FindForDoor(std::span<const NativeGarageEntry>, NativeCollisionVector boundCenter);
    static NativeGarageRequirement Transition(const NativeGarageEntry&, const NativeGarageView&,
        const NativeGaragePolicy& policy = {}, std::size_t index = 50);
    static std::uint8_t UpdateCollisionFlags(const NativeGarageEntry&);
    static NativeGarageMatrix DoorPose(const NativeGarageEntry&, const NativeGarageDoor&);
    static bool DoorPublished(const NativeGarageEntry&, const NativeGarageDoor&, const NativePlacementOverrides*, std::uint8_t sourceFlags);
private:
    NativeGarages& operator=(NativeGarages&&) noexcept = default; // transactional startup publication only
    friend class RealtimeScriptHost;
    NativeScriptServiceResult Deactivate(std::span<const char> name);
    std::vector<NativeGarageEntry> m_Entries;
    std::vector<NativeGarageDoor> m_Doors;
    std::shared_ptr<const NativeCollisionModel> m_Ped1Collision;
    // CGarages::Init clears every CStoredCar::m_wModelIndex (20 x 4).
    // No restore/store service is ported: any required restoration is explicit,
    // even while these genuine initialized slots are empty. Not a fake count.
    std::array<std::array<std::uint16_t, 4>, 20> m_StoredModels{};
    NativeGarageFrame m_Frame;
    NativeGaragePolicy m_Policy;
    std::uint64_t m_Owner = 0, m_Revision = 0;
    std::optional<std::uint64_t> m_LastFrame;
    std::uint32_t m_MaintenanceIndex = 0;
    std::size_t m_RejectedRows = 0, m_IplFiles = 0;
    bool m_Sealed = false;
};
