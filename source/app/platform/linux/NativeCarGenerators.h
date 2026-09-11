// Owned car-generator registry and source-gated native processing contract.
#pragma once

#include "app/platform/linux/NativeVehiclePool.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

struct NativeCarGeneratorRef {
    std::int32_t Value = -1;
    bool operator==(const NativeCarGeneratorRef&) const = default;
};

enum class NativeCarGeneratorSourceKind : std::uint8_t {
    Script014B,
    TextIpl,
    BinaryIpl,
};

enum class NativeCarGeneratorAssetPhase : std::uint8_t {
    StartupStaticLoaded,
    DeferredStreaming,
};

struct NativeCarGeneratorFileRecord {
    NativeScriptPosition Position;
    float AngleRadians = 0.0f;
    std::int32_t ModelId = -1;
    std::int32_t PrimaryColor = -1;
    std::int32_t SecondaryColor = -1;
    std::uint32_t Flags = 0;
    std::int32_t AlarmChance = 0;
    std::int32_t DoorLockChance = 0;
    std::int32_t MinDelay = 0;
    std::int32_t MaxDelay = 0;
    bool operator==(const NativeCarGeneratorFileRecord&) const = default;
};

struct NativeCarGeneratorProvenance {
    NativeCarGeneratorSourceKind Kind = NativeCarGeneratorSourceKind::Script014B;
    std::string Source;
    std::uint32_t Line = 0;
    std::uint32_t Record = 0;
    std::uint64_t ContainerByteOffset = 0;
    std::uint64_t ByteOffset = 0;
    std::uint32_t ByteSize = 0;
    bool operator==(const NativeCarGeneratorProvenance&) const = default;
};

// CFileLoader (0x537990) only switches on a successful Create (0x6F31A0).
// A source model-range rejection is a completed load without a registry slot;
// capacity and conversion failures are not interchangeable with that rejection.
enum class NativeCarGeneratorRegistrationStatus : std::uint8_t {
    NotAttempted,
    Registered,
    RejectedModelRange,
    CapacityExceeded,
    InvalidMetadata,
};

struct NativeCarGeneratorAssetRecord {
    NativeCarGeneratorFileRecord Authored;
    NativeCarGeneratorProvenance Provenance;
    NativeCarGeneratorAssetPhase Phase = NativeCarGeneratorAssetPhase::DeferredStreaming;
    std::int32_t RegistryIndex = -1;
    bool ModelDefinitionResolved = false;
    NativeCarGeneratorRegistrationStatus RegistrationStatus = NativeCarGeneratorRegistrationStatus::NotAttempted;
    std::uint8_t RegistrationIplId = 0;
    std::string RegistrationDetail;
};

struct NativeCarGeneratorModelDefinition {
    std::int32_t ModelId = -1;
    std::string ModelName;
    std::string TextureName;
    NativeVehicleType Type = NativeVehicleType::Unsupported;
    std::string TypeName;
    std::string HandlingName;
    std::string GameName;
    std::string AnimationGroup;
    std::string ClassName;
    std::uint32_t Frequency = 0;
    std::uint32_t Flags = 0;
    std::uint32_t ComponentRules = 0;
    std::int32_t Misc = -1;
    float WheelSizeFront = 0.7f;
    float WheelSizeRear = 0.7f;
    std::int32_t WheelUpgradeClass = -1;
    std::string Source;
    std::uint32_t Line = 0;
};

struct NativeCarGeneratorCreateRequest {
    NativeScriptRequestId Id;
    NativeScriptPosition Position;
    float AngleDegrees = 0.0f;
    std::int32_t ModelId = -1;
    std::int32_t PrimaryColor = -1;
    std::int32_t SecondaryColor = -1;
    std::int32_t ForceSpawn = 0;
    std::int32_t AlarmChance = 0;
    std::int32_t DoorLockChance = 0;
    std::int32_t MinDelay = 0;
    std::int32_t MaxDelay = 0;
    std::uint8_t IplId = 0;
    bool IgnorePopulationLimit = false;
    bool operator==(const NativeCarGeneratorCreateRequest&) const = default;
};

struct NativeCarGeneratorSwitchRequest {
    NativeScriptRequestId Id;
    NativeCarGeneratorRef Generator;
    std::int32_t Count = 0;
    bool operator==(const NativeCarGeneratorSwitchRequest&) const = default;
};

struct NativeCarGeneratorState {
    std::int16_t ModelId = -1;
    std::int8_t PrimaryColor = -1;
    std::int8_t SecondaryColor = -1;
    std::array<std::int16_t, 3> CompressedPosition{}; // source scale: 8 units per world unit
    std::int8_t Angle = 0; // source scale: 256 units per turn
    std::uint8_t AlarmChance = 0;
    std::uint8_t DoorLockChance = 0;
    bool WaitUntilFarFromPlayer = false;
    bool HighPriority = false;
    bool ActiveFlag = false; // preserved source bit; current processing does not consume it
    bool PlayerHasAlreadyOwnedCar = false;
    bool IgnorePopulationLimit = false;
    std::uint16_t MinDelay = 0;
    std::uint16_t MaxDelay = 0;
    std::uint32_t NextGenerationTime = 0;
    NativeVehicleRef Vehicle;
    std::uint16_t GenerateCount = 0;
    std::uint8_t IplId = 0;
    bool Used = false;
    NativeCarGeneratorProvenance Provenance;

    NativeScriptPosition Position() const;
    float HeadingRadians() const;
};

enum class NativeCarGeneratorEventKind : std::uint8_t {
    Create014B,
    Switch014C,
};

struct NativeCarGeneratorEvent {
    std::uint64_t Sequence = 0;
    NativeCarGeneratorEventKind Kind = NativeCarGeneratorEventKind::Create014B;
    NativeScriptRequestId Id;
    NativeCarGeneratorRef Generator;
    NativeCarGeneratorCreateRequest Create;
    std::int32_t Count = 0;
    NativeScriptServiceResult Result;
};

enum class NativeCarGeneratorVisibility : std::uint8_t {
    Unknown,
    HiddenOrOccluded,
    VisibleUnoccluded,
};

enum class NativeCarGeneratorBlockage : std::uint8_t {
    Unknown,
    Clear,
    Blocked,
};

enum class NativeCarGeneratorGround : std::uint8_t {
    Unknown,
    Miss,
    Hit,
};

struct NativeCarGeneratorObservation {
    NativeCarGeneratorRef Generator;
    NativeCarGeneratorVisibility Visibility = NativeCarGeneratorVisibility::Unknown;
    NativeCarGeneratorBlockage Blockage = NativeCarGeneratorBlockage::Unknown;
    NativeCarGeneratorGround Ground = NativeCarGeneratorGround::Unknown;
    float GroundZ = 0.0f;
};

struct NativeCarGeneratorModelRuntime {
    std::int32_t ModelId = -1;
    NativeVehicleType Type = NativeVehicleType::Unsupported;
    bool KeepInMemoryRequested = false;
    bool Loaded = false;
};

enum class NativeCarGeneratorRandomCompatibility : std::uint8_t {
    SharedSourceRand15Required,
};

enum class NativeCarGeneratorPlacement : std::uint8_t {
    VerticalLine,
    Direct,
    DirectGroundLookup,
};

struct NativeCarGeneratorConsumerRequest {
    NativeCarGeneratorRef Generator;
    NativeScriptPosition Position;
    NativeScriptPosition Camera;
    std::int32_t ModelId = -1;
    std::array<std::int32_t, 2> ModelsToRequest{-1, -1};
    std::uint8_t ModelRequestCount = 0;
    NativeVehicleType VehicleType = NativeVehicleType::Unsupported;
    NativeCarGeneratorPlacement Placement = NativeCarGeneratorPlacement::VerticalLine;
    float GroundZ = 0.0f;
    std::int8_t PrimaryColor = -1;
    std::int8_t SecondaryColor = -1;
    std::uint8_t AlarmChance = 0;
    std::uint8_t DoorLockChance = 0;
    bool HighPriority = false;
    bool PlayerAlreadyOwned = false;
    NativeCarGeneratorRandomCompatibility Random = NativeCarGeneratorRandomCompatibility::SharedSourceRand15Required;
    // CarGenerator.cpp consumes the shared CRT rand15 stream only after real
    // construction/world insertion: alarm first, then door lock. No seed is
    // invented or owned by this registry.
    std::uint8_t PostSpawnRand15Calls = 2;
};

enum class NativeCarGeneratorRequirement : std::uint8_t {
    None,
    ModelDefinition,
    BoatVisibilityAndOcclusion,
    CollisionBlockage,
    ModelKeepInMemory,
    RandomPopulationSelection,
    GroundCollision,
    VehiclePool,
    VehicleProducerBinding,
    VehiclePoolCapacity,
    VehicleConstructionAndWorldInsertion,
    PlayerVehicleTransition,
};

enum class NativeCarGeneratorDecision : std::uint8_t {
    HasVehicle,
    VehicleReferenceExpired,
    Disabled,
    Timer,
    VerticalRange,
    TooFar,
    WaitingUntilFar,
    AreaMismatch,
    DistanceOrPriority,
    PlayerApproaching,
    PopulationLimit,
    Blocked,
    GroundMiss,
    ConsumerRequired,
};

struct NativeCarGeneratorAction {
    NativeCarGeneratorRef Generator;
    NativeCarGeneratorDecision Decision = NativeCarGeneratorDecision::ConsumerRequired;
    NativeCarGeneratorRequirement Requirement = NativeCarGeneratorRequirement::None;
    NativeScriptServiceResult Result{NativeScriptServiceStatus::Ready, {}};
    NativeCarGeneratorConsumerRequest Request;
};

struct NativeCarGeneratorProcessInput {
    std::uint32_t TimeMs = 0;
    std::uint8_t ClockHour = 0;
    std::uint32_t NumParkedCars = 0;
    NativeScriptPosition PlayerCenter;
    NativeScriptPosition Camera;
    std::array<float, 3> PlayerSpeed{};
    float GenerationDistanceMultiplier = 1.0f;
    bool CanSeeOutside = true;
    bool PlayerInTrain = false;
    bool Cutscene = false;
    bool ReplayPlayback = false;
    const NativeVehiclePool* Vehicles = nullptr;
    std::span<const NativeCarGeneratorObservation> Observations;
    std::span<const NativeCarGeneratorModelRuntime> Models;
};

struct NativeCarGeneratorProcessFrame {
    NativeScriptServiceResult Result{NativeScriptServiceStatus::Ready, {}};
    std::uint8_t ProcessCounterBefore = 0;
    std::uint8_t ProcessCounterAfter = 0;
    std::uint8_t GenerateCloseCounterBefore = 0;
    std::uint8_t GenerateCloseCounterAfter = 0;
    std::size_t Visited = 0;
    std::vector<NativeCarGeneratorAction> Actions;
};

enum class NativeCarGeneratorStartupDisposition : std::uint8_t {
    Unloaded,
    TextIplRecordsRegistered,
    ProvenEmptyTextIplsBinaryDeferred,
};

struct NativeCarGeneratorSourceCensus {
    std::size_t Definitions = 0;
    std::size_t TextIplFiles = 0;
    std::size_t BinaryIplFiles = 0;
    std::size_t StartupAssetRecords = 0;
    std::size_t DeferredAssetRecords = 0;
    std::size_t Registered = 0;
    std::size_t RegisteredAssetRecords = 0;
    std::size_t RejectedAssetRecords = 0;
    bool StartupOrderProven = false;
    NativeCarGeneratorStartupDisposition StartupDisposition = NativeCarGeneratorStartupDisposition::Unloaded;
};

class NativeCarGenerators {
public:
    static constexpr std::size_t Capacity = 500;
    // MODEL_LANDSTAL through MODEL_VEG_PALMKB8, plus random model -1.
    // IDE availability/type is a separate processing requirement.
    static constexpr bool IsSourceModelInRange(std::int32_t model) {
        return model == -1 || (model >= 400 && model <= 630);
    }

    NativeCarGenerators() = default;
    // Owned metadata only: copying does not acquire vehicle/model ownership.
    NativeCarGenerators(const NativeCarGenerators&) = default;
    NativeCarGenerators& operator=(const NativeCarGenerators&) = delete;

    void Swap(NativeCarGenerators& other) noexcept {
        if (this == &other) return;
        NativeCarGenerators temporary;
        temporary = std::move(*this);
        *this = std::move(other);
        other = std::move(temporary);
    }

    // Mirrors CGame::Initialise: Init, DEFAULT.DAT, GTA.DAT, then scripts.
    // Text IPL rows are therefore active now. Binary IMG rows remain catalogued
    // until the streaming owner reports the matching IPL loaded.
    bool LoadBeforeWorker(const char* gameDir, std::uint32_t timeMs, std::string& error);
    void SealStartup() { m_Sealed = true; }

    NativeScriptReferenceResult<NativeCarGeneratorRef> Create(const NativeCarGeneratorCreateRequest& request,
        std::uint32_t timeMs);
    NativeScriptServiceResult Switch(const NativeCarGeneratorSwitchRequest& request, std::uint32_t timeMs);
    bool ActivateStreamedIpl(std::string_view source, std::uint8_t iplId, std::uint32_t timeMs, std::string& error);
    std::size_t RemoveIpl(std::uint8_t iplId);
    void ActivateGenerateEvenIfPlayerIsClose(std::uint8_t frames = 20) { m_GenerateCloseCounter = frames; }
    NativeCarGeneratorProcessFrame Process(const NativeCarGeneratorProcessInput& input);

    // Call only after the parent has completed actual model construction,
    // placement, world insertion, alpha setup and rand15 alarm/lock effects.
    bool AttachSpawnedVehicle(NativeCarGeneratorRef generator, NativeVehicleRef vehicle,
        std::int32_t actualModelId, std::int8_t primaryColor, std::int8_t secondaryColor,
        std::uint32_t timeMs, const NativeVehiclePool& pool, std::string& error);
    // Commit the result of the parent's source zone/AppropriateLoadedCars RNG
    // consumer. The registry enforces the original boat/length filters.
    bool CommitRandomPopulationSelection(NativeCarGeneratorRef generator, std::int32_t actualModelId,
        NativeVehicleType type, float sourceBoundLength, std::string& error);
    // Call only after the parent has applied source extended-removal-range zero.
    bool CommitPlayerVehicleTransition(NativeCarGeneratorRef generator, const NativeVehiclePool& pool,
        std::string& error);

    const NativeCarGeneratorState* Resolve(NativeCarGeneratorRef reference) const;
    NativeCarGeneratorState* Resolve(NativeCarGeneratorRef reference);
    const NativeCarGeneratorModelDefinition* FindModel(std::int32_t modelId) const;
    std::span<const NativeCarGeneratorState, Capacity> Entries() const { return m_Entries; }
    std::span<const NativeCarGeneratorAssetRecord> AssetRecords() const { return m_Assets; }
    std::span<const NativeCarGeneratorModelDefinition> ModelDefinitions() const { return m_Models; }
    std::span<const NativeCarGeneratorEvent> Events() const { return m_Events; }
    NativeCarGeneratorSourceCensus Census() const;
    std::uint8_t ProcessCounter() const { return m_ProcessCounter; }
    std::uint8_t GenerateCloseCounter() const { return m_GenerateCloseCounter; }
    std::uint64_t Revision() const { return m_Revision; }

    static bool ParseTextRecord(std::string_view row, NativeCarGeneratorFileRecord& out, std::string& error);
    static bool ParseBinaryIpl(std::span<const std::uint8_t> bytes, std::string_view source,
        std::uint64_t containerByteOffset, std::vector<NativeCarGeneratorAssetRecord>& out, std::string& error);

private:
    NativeCarGenerators& operator=(NativeCarGenerators&&) noexcept = default;
    NativeScriptReferenceResult<NativeCarGeneratorRef> CreateInternal(const NativeCarGeneratorCreateRequest& request,
        std::uint32_t timeMs, const NativeCarGeneratorProvenance& provenance, bool journal,
        NativeCarGeneratorRegistrationStatus* registration = nullptr);
    NativeScriptServiceResult RegisterAsset(NativeCarGeneratorAssetRecord& asset, std::uint8_t iplId,
        std::uint32_t timeMs);
    const NativeCarGeneratorEvent* FindEvent(NativeScriptRequestId id) const;

    std::array<NativeCarGeneratorState, Capacity> m_Entries{};
    std::vector<NativeCarGeneratorAssetRecord> m_Assets;
    std::vector<NativeCarGeneratorModelDefinition> m_Models;
    std::vector<NativeCarGeneratorEvent> m_Events;
    std::size_t m_Registered = 0;
    std::size_t m_TextIplFiles = 0;
    std::size_t m_BinaryIplFiles = 0;
    std::uint8_t m_ProcessCounter = 0;
    std::uint8_t m_GenerateCloseCounter = 0;
    std::uint64_t m_Revision = 0;
    bool m_SourceLoaded = false;
    bool m_StartupOrderProven = false;
    NativeCarGeneratorStartupDisposition m_StartupDisposition = NativeCarGeneratorStartupDisposition::Unloaded;
    bool m_Sealed = false;
};
