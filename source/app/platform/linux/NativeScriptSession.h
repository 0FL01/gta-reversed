// Bounded, owned native SCM startup interpreter. No original-address dispatcher.
// Supported contract: main first WAIT + mission-0 policy/numeric initialization
// through locked-property/contact-radar and IPL ENEX flags; unknowns fault.
// No result means "game booted"; hosts choose an explicit observation boundary.
#pragma once

#include "app/platform/linux/NativeScriptCorpus.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

enum class NativeScriptServiceStatus { Ready, Pending, Unsupported, Error };

struct NativeScriptServiceResult {
    NativeScriptServiceStatus Status = NativeScriptServiceStatus::Unsupported;
    std::string Message;
};

struct NativeScriptRequestId {
    std::uint64_t Session = 0;
    std::uint64_t Instruction = 0;
    std::uint32_t IP = 0;
    bool operator==(const NativeScriptRequestId&) const = default;
};

struct NativeScriptPosition {
    float X = 0, Y = 0, Z = 0;
    bool operator==(const NativeScriptPosition&) const = default;
};

struct NativeScriptCollisionRequest {
    NativeScriptRequestId Id;
    float X = 0, Y = 0;
};

struct NativeScriptSceneRequest {
    NativeScriptRequestId Id;
    NativeScriptPosition Position;
};
struct NativeScriptDirectionalSceneRequest {
    NativeScriptRequestId Id;
    NativeScriptPosition Position;
    float Direction = 0;
};
struct NativeScriptClearAreaRequest {
    NativeScriptRequestId Id;
    NativeScriptPosition Position;
    float Radius = 0;
    bool IncludeProjectiles = false;
};

struct NativeScriptPlayerRequest {
    NativeScriptRequestId Id;
    std::int32_t PlayerIndex = 0;
    // Authored SCM position, NOT a ground-ray ceiling. The host implements the
    // MODEL_PLAYER/base offset/mission ownership/on-foot task contract of 0053.
    NativeScriptPosition Position;
};

// Script handles include the host pool/group generation. Never substitute a
// player index, pointer, or unversioned slot for these opaque script references.
struct NativeScriptGroupRef { std::int32_t Value = -1; };
struct NativeScriptPedRef { std::int32_t Value = -1; };
struct NativeScriptPickupRef { std::int32_t Value = -1; };
struct NativeScriptBlipRef { std::int32_t Value = -1; };
struct NativeScriptObjectRef {
    std::int32_t Value = -1;
    bool operator==(const NativeScriptObjectRef&) const = default;
};
// Source generator slot references: zero is valid; -1 is allocation failure.
struct NativeScriptCarGeneratorRef { std::int32_t Value = -1; };
struct NativeScriptVehicleRef { std::int32_t Value = -1; };
template<typename Ref> struct NativeScriptReferenceResult {
    NativeScriptServiceResult Result;
    Ref Reference;
};
using NativeScriptVehicleResult = NativeScriptReferenceResult<NativeScriptVehicleRef>;
struct NativeScriptVehicleStatsResult {
    NativeScriptServiceResult Result;
    std::array<std::int32_t, 3> Times{};
    std::array<float, 3> Distances{};
};
struct NativeScriptCarModelResult {
    NativeScriptServiceResult Result;
    std::int32_t ModelId = -1, VehicleClass = -1;
};
struct NativeScriptCarGeneratorRequest {
    NativeScriptRequestId Id;
    NativeScriptPosition Position;
    float AngleDegrees = 0;
    std::int32_t ModelId = 0, PrimaryColor = 0, SecondaryColor = 0;
    std::int32_t ForceSpawn = 0, AlarmChance = 0, DoorLockChance = 0;
    std::int32_t MinDelay = 0, MaxDelay = 0;
};
struct NativeScriptVehicleCreateRequest {
    NativeScriptRequestId Id;
    std::int32_t ModelId = 0;
    NativeScriptPosition Position;
};
struct NativeScriptVehicleHeadingRequest {
    NativeScriptRequestId Id;
    NativeScriptVehicleRef Vehicle;
    float Degrees = 0.0f;
};
struct NativeScriptVehicleStateRequest {
    NativeScriptRequestId Id;
    NativeScriptVehicleRef Vehicle;
    std::int32_t Value = 0;
};
struct NativeScriptPedVehicleRequest {
    NativeScriptRequestId Id;
    NativeScriptPedRef Ped;
    NativeScriptVehicleRef Vehicle;
    std::int32_t Seat = -1;
};
struct NativeScriptCreatePedInVehicleRequest {
    NativeScriptRequestId Id;
    NativeScriptVehicleRef Vehicle;
    std::int32_t PedType = 0;
    std::int32_t ModelId = 0;
    std::int32_t Seat = -1; // -1 is the source driver command; nonnegative is passenger index.
};
struct NativeScriptPedCreateRequest {
    NativeScriptRequestId Id;
    std::int32_t PedType = 0;
    std::int32_t ModelId = 0;
    NativeScriptPosition Position;
};
struct NativeScriptTrainCreateRequest {
    NativeScriptRequestId Id;
    std::int32_t Type = 0;
    NativeScriptPosition Position;
    bool Clockwise = false;
};
struct NativeScriptTrainSpeedRequest {
    NativeScriptRequestId Id;
    NativeScriptVehicleRef Train;
    float Speed = 0.0f;
};
struct NativeScriptCarDriveTaskRequest {
    NativeScriptRequestId Id;
    NativeScriptPedRef Ped;
    NativeScriptVehicleRef Vehicle;
    NativeScriptPosition Target;
    float Speed = 0.0f;
    std::int32_t DriveStyle = 0, ModelId = 0, DrivingStyle = 0;
};
struct NativeScriptGoStraightTaskRequest {
    NativeScriptRequestId Id;
    NativeScriptPedRef Ped;
    NativeScriptPosition Target;
    std::int32_t MoveState = 0;
    std::int32_t TimeMs = 0;
};
struct NativeScriptSkipCutsceneRequest {
    NativeScriptRequestId Id;
    std::int32_t Target = 0;
};
struct NativeScriptCameraCommandRequest {
    NativeScriptRequestId Id;
    std::uint16_t Opcode = 0;
    std::array<float, 6> Floats{};
    std::array<std::int32_t, 2> Integers{};
};
struct NativeScriptFixedCameraRequest {
    NativeScriptRequestId Id;
    NativeScriptPosition Position;
    NativeScriptPosition Offset;
};
struct NativeScriptPointCameraRequest {
    NativeScriptRequestId Id;
    NativeScriptPosition Position;
    std::int32_t SwitchType = 0;
};
struct NativeScriptCarGeneratorPlateRequest {
    NativeScriptCarGeneratorRequest Generator;
    std::array<char, 8> PlateText{};
};
struct NativeScriptCarGeneratorSwitchRequest {
    NativeScriptRequestId Id;
    NativeScriptCarGeneratorRef Generator;
    std::int32_t Count = 0;
};
struct NativeScriptCarGeneratorOwnedRequest {
    NativeScriptRequestId Id;
    NativeScriptCarGeneratorRef Generator;
    bool Owned = false;
};
struct NativeScriptPlayerLookupRequest {
    NativeScriptRequestId Id;
    std::int32_t PlayerIndex = 0;
};
struct NativeScriptScoreRequest {
    NativeScriptRequestId Id;
    std::int32_t PlayerIndex = 0;
    std::int32_t Amount = 0;
};
struct NativeScriptCameraRequest { NativeScriptRequestId Id; };
struct NativeScriptHeadingRequest {
    NativeScriptRequestId Id;
    NativeScriptPedRef Ped;
    float Radians = 0; // source FixAngleDegrees (one +/-360 adjustment), then radians
};
struct NativeScriptLockedPropertyRequest {
    NativeScriptRequestId Id;
    NativeScriptPosition Position;
    std::array<char, 8> Text{};
};
struct NativeScriptForSalePropertyRequest {
    NativeScriptRequestId Id;
    NativeScriptPosition Position;
    std::int32_t Price = 0;
    std::array<char, 8> Text{};
};
struct NativeScriptPickupRequest {
    NativeScriptRequestId Id;
    std::int32_t Model = 0, Type = 0;
    NativeScriptPosition Position;
    // Negative model operands index the immutable SCM used-object table.
    // Positive IDs have an empty name. The host resolves names against IDE.
    std::array<char, 24> UsedObjectName{};
};
struct NativeScriptPickupAmmoRequest {
    NativeScriptRequestId Id;
    std::int32_t Model = 0, Type = 0, Ammo = 0;
    NativeScriptPosition Position;
};
struct NativeScriptObjectRequest {
    NativeScriptRequestId Id;
    std::int32_t ModelId = -1; // source operand; negative values index UsedObjects
    NativeScriptPosition Position;
    std::array<char, 24> UsedObjectName{};
};
struct NativeScriptObjectHeadingRequest {
    NativeScriptRequestId Id;
    NativeScriptObjectRef Object;
    float Degrees = 0;
};
struct NativeScriptObjectCleanupRequest {
    NativeScriptRequestId Id;
    NativeScriptObjectRef Object;
};
struct NativeScriptObjectDamageRequest {
    NativeScriptRequestId Id;
    NativeScriptObjectRef Object;
    std::int32_t Effect = 0;
};
struct NativeScriptObjectFreezeRequest {
    NativeScriptRequestId Id;
    NativeScriptObjectRef Object;
    bool Frozen = false;
};
struct NativeScriptObjectDynamicRequest {
    NativeScriptRequestId Id;
    NativeScriptObjectRef Object;
    bool Dynamic = false;
};
struct NativeScriptObjectVelocityRequest {
    NativeScriptRequestId Id;
    NativeScriptObjectRef Object;
    NativeScriptPosition Velocity;
};
struct NativeScriptObjectProofRequest {
    NativeScriptRequestId Id;
    NativeScriptObjectRef Object;
    std::uint8_t Proofs = 0; // source five boolean proof parameters, bits 0..4
};
struct NativeScriptObjectRotateRequest {
    NativeScriptRequestId Id;
    NativeScriptObjectRef Object;
    NativeScriptPosition Rotation;
    bool Relative = false;
};
struct NativeScriptObjectAreaRequest {
    NativeScriptRequestId Id;
    NativeScriptObjectRef Object;
    std::int32_t Area = 0;
};
struct NativeScriptObjectLodRequest {
    NativeScriptRequestId Id;
    NativeScriptObjectRef Child, Parent;
};
struct NativeScriptObjectCoordinatesRequest {
    NativeScriptRequestId Id;
    NativeScriptObjectRef Object;
    NativeScriptPosition Offset;
};
struct NativeScriptObjectCoordinatesResult {
    NativeScriptServiceResult Result;
    NativeScriptPosition Position;
};
struct NativeScriptObjectHeadingResult {
    NativeScriptServiceResult Result;
    float Degrees = 0;
};
struct NativeScriptBooleanResult {
    NativeScriptServiceResult Result;
    bool Value = false;
};
struct NativeScriptIntegerResult {
    NativeScriptServiceResult Result;
    std::int32_t Value = 0;
};
struct NativeScriptPositionResult {
    NativeScriptServiceResult Result;
    NativeScriptPosition Value;
};
struct NativeScriptStringResult {
    NativeScriptServiceResult Result;
    std::array<char, 16> Value{};
};
struct NativeScriptPedQueryRequest {
    NativeScriptRequestId Id;
    NativeScriptPedRef Ped;
};
struct NativeScriptPedWeaponRequest {
    NativeScriptRequestId Id;
    NativeScriptPedRef Ped;
    std::int32_t Weapon = 0;
};
struct NativeScriptPedStateRequest {
    NativeScriptRequestId Id;
    NativeScriptPedRef Ped;
    bool Value = false;
};
struct NativeScriptModelRequest {
    NativeScriptRequestId Id;
    std::int32_t Model = 0;
    std::array<char, 24> UsedObjectName{};
};
struct NativeScriptSpecialModelRequest {
    NativeScriptRequestId Id;
    std::int32_t Slot = 0;
    std::array<char, 8> Name{};
};
struct NativeScriptCarRecordingRequest {
    NativeScriptRequestId Id;
    std::int32_t Recording = 0;
};
struct NativeScriptBeatTrackRequest {
    NativeScriptRequestId Id;
    std::int32_t Track = 0;
};
struct NativeScriptMissionAudioRequest {
    NativeScriptRequestId Id;
    std::int32_t Slot = 0;
    std::int32_t AudioId = 0;
};
struct NativeScriptBooleanRequest {
    NativeScriptRequestId Id;
    bool Value = false;
};
struct NativeScriptDensityRequest {
    NativeScriptRequestId Id;
    float Multiplier = 1.0f;
    bool Cars = false;
};
struct NativeScriptWeatherRequest {
    NativeScriptRequestId Id;
    std::int32_t Weather = 0;
};
struct NativeScriptFadeColourRequest {
    NativeScriptRequestId Id;
    std::int32_t Red = 0, Green = 0, Blue = 0;
};
struct NativeScriptAreaRequest {
    NativeScriptRequestId Id;
    std::int32_t Area = 0;
};
struct NativeScriptPlayerControlRequest {
    NativeScriptRequestId Id;
    std::int32_t PlayerIndex = 0;
    bool Enabled = false;
};
struct NativeScriptPedHealthRequest {
    NativeScriptRequestId Id;
    NativeScriptPedRef Ped;
    std::int32_t Health = 0;
};
struct NativeScriptClothesRequest {
    NativeScriptRequestId Id;
    std::int32_t PlayerIndex = 0;
    std::array<char, 16> Texture{};
    std::array<char, 16> Model{};
    std::int32_t BodyPart = 0;
};
struct NativeScriptMissionTextRequest {
    NativeScriptRequestId Id;
    std::array<char, 8> Name{};
};
struct NativeScriptTextureDictionaryRequest {
    NativeScriptRequestId Id;
    std::array<char, 16> Name{};
};
struct NativeScriptSpriteRequest {
    NativeScriptRequestId Id;
    std::int32_t Slot = -1;
    std::array<char, 16> Name{};
};
struct NativeScriptTextCommandsRequest {
    NativeScriptRequestId Id;
    bool Enabled = false;
};
struct NativeScriptTextStyleRequest {
    NativeScriptRequestId Id;
    std::uint16_t Opcode = 0;
    std::array<float, 2> Floats{};
    std::array<std::int32_t, 5> Integers{};
};
struct NativeScriptTextDisplayRequest {
    NativeScriptRequestId Id;
    float X = 0.0f;
    float Y = 0.0f;
    std::array<char, 8> Key{};
};
struct NativeScriptPrintRequest {
    NativeScriptRequestId Id;
    std::array<char, 8> Key{};
    std::int32_t TimeMs = 0, Flag = 0;
};
struct NativeScriptCutsceneRequest {
    NativeScriptRequestId Id;
    std::array<char, 8> Name{};
};
struct NativeScriptStreamedRequest {
    NativeScriptRequestId Id;
    std::int32_t ScriptIndex = -1;
};
enum class NativeScriptPlayerStateQueryKind : std::uint8_t {
    InTrain, InFlyingVehicle, InBoat, ControlEnabled, InVehicleModel, InAnyVehicle, CanStartMission
};
struct NativeScriptPlayerStateQueryRequest {
    NativeScriptRequestId Id;
    std::int32_t Reference = -1;
    NativeScriptPlayerStateQueryKind Kind{};
    std::int32_t ModelId = -1;
};
struct NativeScriptLocateCharRequest {
    NativeScriptRequestId Id;
    NativeScriptPedRef Ped;
    NativeScriptPosition Center{};
    NativeScriptPosition Radius{};
    bool OnFoot = false;
    bool Highlight = false;
    bool TwoDimensional = false;
    bool Stopped = false;
};
struct NativeScriptLocateCharObjectRequest {
    NativeScriptRequestId Id;
    NativeScriptPedRef Ped;
    NativeScriptObjectRef Object;
    float RadiusX = 0.0f, RadiusY = 0.0f;
    bool Highlight = false;
};
struct NativeScriptContactBlipRequest {
    NativeScriptRequestId Id;
    NativeScriptPosition Position;
    std::int32_t Sprite = 0;
    // Raw VM requests leave this false. The realtime host sets it only after
    // its registered radar consumer reports the numeric sprite ready now.
    bool RadarSpriteReady = false;
    bool AddSphere = false;
};
// Original04CE: BLIP_COORD, SHORT_RANGE, BOTH; not a contact-point alias.
// Readiness belongs to the host's registered live renderer, never this DTO.
struct NativeScriptCoordinateBlipRequest {
    NativeScriptRequestId Id;
    NativeScriptPosition Position;
    std::int32_t Sprite = 0;
};
struct NativeScriptBlipDisplayRequest {
    NativeScriptRequestId Id;
    NativeScriptBlipRef Blip;
    std::int32_t Display = 0;
};
struct NativeScriptBlipReferenceRequest {
    NativeScriptRequestId Id;
    NativeScriptBlipRef Blip;
};
struct NativeScriptUserMarkerRef { std::int32_t Value = -1; };
struct NativeScriptUserMarkerRequest {
    NativeScriptRequestId Id;
    NativeScriptPosition Position;
    std::int32_t Colour = 0;
};
struct NativeScriptUserMarkerReferenceRequest {
    NativeScriptRequestId Id;
    NativeScriptUserMarkerRef Marker;
};
// 09B4: XY, search range, low-16-bit flag mask, integer boolean (nonzero).
// No Z, output handle or compare update. Host owns nearest lookup + word write.
struct NativeScriptEntryExitFlagRequest {
    NativeScriptRequestId Id;
    float X = 0, Y = 0, Radius = 0;
    std::int32_t Mask = 0, State = 0;
};
struct NativeScriptEntryExitSwitchRequest {
    NativeScriptRequestId Id;
    std::array<char, 8> Name{};
    bool Enabled = false;
};
struct NativeScriptGarageRequest {
    NativeScriptRequestId Id;
    std::array<char, 8> Name{};
};
struct NativeScriptGarageTypeRequest {
    NativeScriptRequestId Id;
    std::array<char, 8> Name{};
    std::int32_t Type = 0;
};

enum class NativeRestartKind { Hospital, Police };
struct NativeScriptRestartRequest {
    NativeScriptRequestId Id;
    NativeRestartKind Kind = NativeRestartKind::Hospital;
    NativeScriptPosition Position;
    float HeadingDegrees = 0; // source stores raw heading, without FixAngle
    std::int32_t WhenToUse = 0; // compared against STAT_CITY_UNLOCKED, not a zone ID
    bool operator==(const NativeScriptRestartRequest&) const = default;
};

struct NativeScriptStuntJumpRequest {
    NativeScriptRequestId Id;
    NativeScriptPosition StartCenter, StartHalfSize;
    NativeScriptPosition EndCenter, EndHalfSize;
    NativeScriptPosition Camera;
    std::int32_t Reward = 0;
};
struct NativeScriptSetPieceRequest {
    NativeScriptRequestId Id;
    std::int32_t Type = 0;
    std::array<float, 12> Coordinates{};
};
struct NativeScriptZonePopulationRequest {
    NativeScriptRequestId Id;
    std::array<char,8> Name{};
    std::int32_t Value = 0;
};
struct NativeScriptZoneGangRequest {
    NativeScriptRequestId Id;
    std::array<char,8> Name{};
    std::int32_t Gang = 0, Strength = 0;
};
enum class NativePathPolicyKind : std::uint8_t { VehicleOn,VehicleOff,VehicleOriginal,PedOn,PedOff,PedOriginal };
struct NativeScriptPathPolicyRequest {
    NativeScriptRequestId Id;
    NativePathPolicyKind Kind;
    std::array<float,6> Coordinates{};
};
struct NativeScriptExternalTriggerRequest {
    NativeScriptRequestId Id;
    std::int32_t ScriptIndex=0,ModelId=-1,Priority=0,Type=0;
    float Radius=0;
    std::array<char,24> ModelName{};
    bool ObjectModel=false;
};
struct NativeScriptCodeBrainRequest {
    NativeScriptRequestId Id;
    std::int32_t ScriptIndex = 0;
    std::array<char, 8> Name{};
    bool Attractor = false;
};
struct NativeScriptModelAnimRequest {
    NativeScriptRequestId Id;
    std::int32_t ModelId = -1;
    std::array<char, 8> IfpName{};
};
struct NativeScriptIplRequest {
    NativeScriptRequestId Id;
    std::array<char, 16> Name{};
    bool Requested = true;
};
struct NativeScriptWorldObjectVisibilityRequest {
    NativeScriptRequestId Id;
    NativeScriptPosition Position{};
    float Radius = 0.0f;
    std::int32_t ModelId = -1;
    std::array<char, 24> ModelName{};
    bool Visible = true;
};

// Defined by NativeScriptEntities, which owns the source pickup collection ring.
// Keep this VM interface non-owning rather than duplicating the shared types.
struct NativeScriptPickupReferenceRequest;
struct NativeScriptPickupCollectedResult;

class NativeScriptServices {
public:
    virtual ~NativeScriptServices() = default;
    // Pending calls are polled with the SAME ID and request. Implementations
    // must be idempotent per ID, commit external effects only on Ready, and
    // own cancellation of outstanding work on session destruction. No method
    // may reenter/mutate this session. Unsupported/Error are terminal VM faults.
    virtual NativeScriptServiceResult RequestCollision(const NativeScriptCollisionRequest&) = 0;
    virtual NativeScriptServiceResult LoadScene(const NativeScriptSceneRequest&) = 0;
    virtual NativeScriptServiceResult LoadSceneInDirection(const NativeScriptDirectionalSceneRequest&) { return {}; }
    virtual NativeScriptServiceResult ClearArea(const NativeScriptClearAreaRequest&) { return {}; }
    virtual NativeScriptServiceResult CreatePlayer(const NativeScriptPlayerRequest&) = 0;
    virtual NativeScriptReferenceResult<NativeScriptGroupRef> GetPlayerGroup(const NativeScriptPlayerLookupRequest&) { return {}; }
    virtual NativeScriptReferenceResult<NativeScriptPedRef> GetPlayerChar(const NativeScriptPlayerLookupRequest&) { return {}; }
    // Source camera follow-ped/cam-on-a-string operation, evaluated at call time
    // BEFORE a subsequent heading change. Host owns all actual camera state.
    virtual NativeScriptServiceResult SetCameraBehindPlayer(const NativeScriptCameraRequest&) { return {}; }
    // Source no-op if in vehicle; otherwise aiming/current rotation, entity
    // heading AND RW matrix. A Ready result certifies those actual host effects.
    virtual NativeScriptServiceResult SetCharHeading(const NativeScriptHeadingRequest&) { return {}; }
    virtual NativeScriptReferenceResult<NativeScriptPickupRef> CreateLockedProperty(const NativeScriptLockedPropertyRequest&) { return {}; }
    virtual NativeScriptReferenceResult<NativeScriptPickupRef> CreateForSaleProperty(const NativeScriptForSalePropertyRequest&) { return {}; }
    // Original ordinary creation can complete with -1 when the source pool
    // has no free/reclaimable slot. This is not an allocated pickup reference.
    virtual NativeScriptReferenceResult<NativeScriptPickupRef> CreatePickup(const NativeScriptPickupRequest&) { return {}; }
    virtual NativeScriptReferenceResult<NativeScriptPickupRef> CreatePickupWithAmmo(const NativeScriptPickupAmmoRequest&) { return {}; }
    virtual NativeScriptReferenceResult<NativeScriptObjectRef> CreateObjectNoOffset(const NativeScriptObjectRequest&) { return {}; }
    virtual NativeScriptReferenceResult<NativeScriptObjectRef> CreateObject(const NativeScriptObjectRequest&) { return {}; }
    virtual NativeScriptServiceResult SetObjectHeading(const NativeScriptObjectHeadingRequest&) { return {}; }
    virtual NativeScriptServiceResult MarkObjectNoLongerNeeded(const NativeScriptObjectCleanupRequest&) { return {}; }
    virtual NativeScriptServiceResult SetObjectCollisionDamageEffect(const NativeScriptObjectDamageRequest&) { return {}; }
    virtual NativeScriptServiceResult FreezeObjectPosition(const NativeScriptObjectFreezeRequest&) { return {}; }
    virtual NativeScriptServiceResult SetObjectDynamic(const NativeScriptObjectDynamicRequest&) { return {}; }
    virtual NativeScriptServiceResult SetObjectVelocity(const NativeScriptObjectVelocityRequest&) { return {}; }
    virtual NativeScriptServiceResult SetObjectProofs(const NativeScriptObjectProofRequest&) { return {}; }
    virtual NativeScriptServiceResult RotateObject(const NativeScriptObjectRotateRequest&) { return {}; }
    virtual NativeScriptServiceResult SetObjectRotation(const NativeScriptObjectRotateRequest&) { return {}; }
    virtual NativeScriptServiceResult SetObjectAreaVisible(const NativeScriptObjectAreaRequest&) { return {}; }
    virtual NativeScriptServiceResult ConnectObjectLods(const NativeScriptObjectLodRequest&) { return {}; }
    virtual NativeScriptObjectCoordinatesResult GetObjectCoordinates(const NativeScriptObjectCoordinatesRequest&) { return {}; }
    virtual NativeScriptObjectCoordinatesResult GetObjectOffsetInWorld(const NativeScriptObjectCoordinatesRequest&) { return {}; }
    virtual NativeScriptObjectHeadingResult GetObjectHeading(const NativeScriptObjectCoordinatesRequest&) { return {}; }
    virtual NativeScriptReferenceResult<NativeScriptBlipRef> CreateContactBlip(const NativeScriptContactBlipRequest&) { return {}; }
    virtual NativeScriptReferenceResult<NativeScriptBlipRef> CreateCoordinateBlip(const NativeScriptCoordinateBlipRequest&) { return {}; }
    virtual NativeScriptServiceResult SetBlipDisplay(const NativeScriptBlipDisplayRequest&) { return {}; }
    virtual NativeScriptServiceResult RemoveBlip(const NativeScriptBlipReferenceRequest&) { return {}; }
    virtual NativeScriptBooleanResult DoesBlipExist(const NativeScriptBlipReferenceRequest&) { return {}; }
    virtual NativeScriptReferenceResult<NativeScriptUserMarkerRef> CreateUserMarker(const NativeScriptUserMarkerRequest&) { return {}; }
    virtual NativeScriptServiceResult RemoveUserMarker(const NativeScriptUserMarkerReferenceRequest&) { return {}; }
    virtual NativeScriptServiceResult SetEntryExitFlag(const NativeScriptEntryExitFlagRequest&) { return {}; }
    virtual NativeScriptServiceResult SwitchEntryExit(const NativeScriptEntryExitSwitchRequest&) { return {}; }
    virtual NativeScriptServiceResult DeactivateGarage(const NativeScriptGarageRequest&) { return {}; }
    virtual NativeScriptServiceResult ChangeGarageType(const NativeScriptGarageTypeRequest&) { return {}; }
    virtual NativeScriptServiceResult AddRestart(const NativeScriptRestartRequest&) { return {}; }
    virtual NativeScriptServiceResult AddStuntJump(const NativeScriptStuntJumpRequest&) { return {}; }
    virtual NativeScriptServiceResult AddSetPiece(const NativeScriptSetPieceRequest&) { return {}; }
    virtual NativeScriptServiceResult InitZonePopulationSettings(const NativeScriptRequestId&) { return {}; }
    virtual NativeScriptServiceResult SetZonePopulationType(const NativeScriptZonePopulationRequest&) { return {}; }
    virtual NativeScriptServiceResult SetZonePopulationRaces(const NativeScriptZonePopulationRequest&) { return {}; }
    virtual NativeScriptServiceResult SetZoneDealerStrength(const NativeScriptZonePopulationRequest&) { return {}; }
    virtual NativeScriptServiceResult SetZoneGangStrength(const NativeScriptZoneGangRequest&) { return {}; }
    virtual NativeScriptServiceResult SetZoneNoCops(const NativeScriptZonePopulationRequest&) { return {}; }
    virtual NativeScriptServiceResult AddPathPolicy(const NativeScriptPathPolicyRequest&) { return {}; }
    virtual NativeScriptServiceResult AddExternalScriptTrigger(const NativeScriptExternalTriggerRequest&) { return {}; }
    virtual NativeScriptServiceResult AddCodeScriptBrain(const NativeScriptCodeBrainRequest&) { return {}; }
    virtual NativeScriptServiceResult AttachAnimsToModel(const NativeScriptModelAnimRequest&) { return {}; }
    virtual NativeScriptServiceResult SetIplRequested(const NativeScriptIplRequest&) { return {}; }
    virtual NativeScriptServiceResult SetClosestObjectVisibility(const NativeScriptWorldObjectVisibilityRequest&) { return {}; }
    virtual NativeScriptServiceResult SetZoneNamesVisible(const NativeScriptRequestId&, bool) { return {}; }
    virtual NativeScriptBooleanResult IsPlayerPlaying(const NativeScriptPlayerLookupRequest&) { return {}; }
    virtual NativeScriptIntegerResult GetCharAreaVisible(const NativeScriptPedQueryRequest&) { return {}; }
    virtual NativeScriptIntegerResult GetAreaVisible(const NativeScriptRequestId&) { return {}; }
    virtual NativeScriptIntegerResult GetCurrentDayOfWeek(const NativeScriptRequestId&) { return {}; }
    virtual NativeScriptIntegerResult GetCurrentLanguage(const NativeScriptRequestId&) { return {}; }
    virtual NativeScriptBooleanResult HasLanguageChanged(const NativeScriptRequestId&) { return {}; }
    virtual NativeScriptIntegerResult GetCityPlayerIsIn(const NativeScriptPlayerLookupRequest&) { return {}; }
    virtual NativeScriptIntegerResult GetNumberTagsTagged(const NativeScriptRequestId&) { return {}; }
    virtual NativeScriptBooleanResult AreCarCheatsActivated(const NativeScriptRequestId&) { return {}; }
    virtual NativeScriptBooleanResult HasDeathArrestBeenExecuted(const NativeScriptRequestId&) { return {}; }
    virtual NativeScriptBooleanResult IsCharDead(const NativeScriptPedQueryRequest&) { return {}; }
    virtual NativeScriptServiceResult SetPedSpeechDisabled(const NativeScriptPedStateRequest&) { return {}; }
    virtual NativeScriptBooleanResult IsGarageOpen(const NativeScriptGarageRequest&) { return {}; }
    virtual NativeScriptBooleanResult HasCharGotWeapon(const NativeScriptPedWeaponRequest&) { return {}; }
    virtual NativeScriptServiceResult RequestModel(const NativeScriptModelRequest&) { return {}; }
    virtual NativeScriptBooleanResult HasModelLoaded(const NativeScriptModelRequest&) { return {}; }
    virtual NativeScriptServiceResult MarkModelNoLongerNeeded(const NativeScriptModelRequest&) { return {}; }
    virtual NativeScriptServiceResult LoadSpecialCharacter(const NativeScriptSpecialModelRequest&) { return {}; }
    virtual NativeScriptBooleanResult HasSpecialCharacterLoaded(const NativeScriptSpecialModelRequest&) { return {}; }
    virtual NativeScriptServiceResult UnloadSpecialCharacter(const NativeScriptSpecialModelRequest&) { return {}; }
    virtual NativeScriptServiceResult RequestCarRecording(const NativeScriptCarRecordingRequest&) { return {}; }
    virtual NativeScriptBooleanResult HasCarRecordingLoaded(const NativeScriptCarRecordingRequest&) { return {}; }
    virtual NativeScriptServiceResult PreloadBeatTrack(const NativeScriptBeatTrackRequest&) { return {}; }
    virtual NativeScriptServiceResult PlayBeatTrack(const NativeScriptRequestId&) { return {}; }
    virtual NativeScriptServiceResult StopBeatTrack(const NativeScriptRequestId&) { return {}; }
    virtual NativeScriptServiceResult BeginSkippableCutscene(const NativeScriptSkipCutsceneRequest&) { return {}; }
    virtual NativeScriptServiceResult EndSkippableCutscene(const NativeScriptRequestId&) { return {}; }
    virtual NativeScriptServiceResult ApplyCameraCommand(const NativeScriptCameraCommandRequest&) { return {}; }
    virtual NativeScriptServiceResult ClearPrints(const NativeScriptRequestId&) { return {}; }
    virtual NativeScriptServiceResult ClearMissionAudio(const NativeScriptRequestId&, std::int32_t) { return {}; }
    virtual NativeScriptServiceResult LoadMissionAudio(const NativeScriptMissionAudioRequest&) { return {}; }
    virtual NativeScriptBooleanResult HasMissionAudioLoaded(const NativeScriptRequestId&, std::int32_t) { return {}; }
    virtual NativeScriptServiceResult PlayMissionAudio(const NativeScriptRequestId&, std::int32_t) { return {}; }
    virtual NativeScriptBooleanResult HasMissionAudioFinished(const NativeScriptRequestId&, std::int32_t) { return {}; }
    virtual NativeScriptIntegerResult GetBeatTrackStatus(const NativeScriptRequestId&) { return {}; }
    virtual NativeScriptBooleanResult AreSubtitlesEnabled(const NativeScriptRequestId&) { return {}; }
    virtual NativeScriptServiceResult SetDensityMultiplier(const NativeScriptDensityRequest&) { return {}; }
    virtual NativeScriptServiceResult SetRandomTrains(const NativeScriptBooleanRequest&) { return {}; }
    virtual NativeScriptReferenceResult<NativeScriptVehicleRef> CreateVehicle(const NativeScriptVehicleCreateRequest&) { return {}; }
    virtual NativeScriptServiceResult SetVehicleHeading(const NativeScriptVehicleHeadingRequest&) { return {}; }
    virtual NativeScriptServiceResult SetVehicleLights(const NativeScriptVehicleStateRequest&) { return {}; }
    virtual NativeScriptServiceResult SetVehicleCollision(const NativeScriptVehicleStateRequest&) { return {}; }
    virtual NativeScriptServiceResult AddScore(const NativeScriptScoreRequest&) { return {}; }
    virtual NativeScriptServiceResult WarpPedIntoVehiclePassenger(const NativeScriptPedVehicleRequest&) { return {}; }
    virtual NativeScriptServiceResult TaskLeaveVehicleImmediately(const NativeScriptPedVehicleRequest&) { return {}; }
    virtual NativeScriptReferenceResult<NativeScriptPedRef> CreatePedInsideVehicle(const NativeScriptCreatePedInVehicleRequest&) { return {}; }
    virtual NativeScriptReferenceResult<NativeScriptPedRef> CreatePed(const NativeScriptPedCreateRequest&) { return {}; }
    virtual NativeScriptServiceResult SetFixedCameraPosition(const NativeScriptFixedCameraRequest&) { return {}; }
    virtual NativeScriptServiceResult PointCameraAtPoint(const NativeScriptPointCameraRequest&) { return {}; }
    virtual NativeScriptServiceResult SetWidescreen(const NativeScriptBooleanRequest&) { return {}; }
    virtual NativeScriptVehicleResult GetPedVehicleNoSave(const NativeScriptPedQueryRequest&) { return {}; }
    virtual NativeScriptVehicleStatsResult GetWheelieStats(const NativeScriptVehicleStateRequest&) { return {}; }
    virtual NativeScriptBooleanResult IsVehicleInAirProper(const NativeScriptVehicleStateRequest&) { return {}; }
    virtual NativeScriptBooleanResult IsVehicleDead(const NativeScriptVehicleStateRequest&) { return {}; }
    virtual NativeScriptBooleanResult IsVehiclePlaybackActive(const NativeScriptVehicleStateRequest&) { return {}; }
    virtual NativeScriptServiceResult StartVehiclePlayback(const NativeScriptVehicleStateRequest&) { return {}; }
    virtual NativeScriptReferenceResult<NativeScriptVehicleRef> CreateMissionTrain(const NativeScriptTrainCreateRequest&) { return {}; }
    virtual NativeScriptServiceResult SetTrainSpeed(const NativeScriptTrainSpeedRequest&, bool) { return {}; }
    virtual NativeScriptServiceResult DeleteMissionTrains(const NativeScriptRequestId&) { return {}; }
    virtual NativeScriptCarModelResult GetRandomResidentCarModel(const NativeScriptRequestId&, bool) { return {}; }
    virtual NativeScriptServiceResult MarkPedNoLongerNeeded(const NativeScriptPedQueryRequest&) { return {}; }
    virtual NativeScriptServiceResult MarkVehicleNoLongerNeeded(const NativeScriptVehicleStateRequest&) { return {}; }
    virtual NativeScriptServiceResult DeletePed(const NativeScriptPedQueryRequest&) { return {}; }
    virtual NativeScriptServiceResult DeleteVehicle(const NativeScriptVehicleStateRequest&) { return {}; }
    virtual NativeScriptReferenceResult<NativeScriptPedRef> CreateRandomDriver(const NativeScriptVehicleStateRequest&) { return {}; }
    virtual NativeScriptServiceResult AssignCarDriveTask(const NativeScriptCarDriveTaskRequest&) { return {}; }
    virtual NativeScriptServiceResult AssignGoStraightTask(const NativeScriptGoStraightTaskRequest&) { return {}; }
    virtual NativeScriptBooleanResult QueryPlayerState(const NativeScriptPlayerStateQueryRequest&) { return {}; }
    virtual NativeScriptServiceResult ForceWeatherNow(const NativeScriptWeatherRequest&) { return {}; }
    virtual NativeScriptServiceResult ReleaseWeather(const NativeScriptRequestId&) { return {}; }
    virtual NativeScriptServiceResult GivePlayerClothes(const NativeScriptClothesRequest&) { return {}; }
    virtual NativeScriptServiceResult BuildPlayerModel(const NativeScriptPlayerLookupRequest&) { return {}; }
    virtual NativeScriptServiceResult StoreClothesState(const NativeScriptRequestId&) { return {}; }
    virtual NativeScriptServiceResult SetFadeColour(const NativeScriptFadeColourRequest&) { return {}; }
    virtual NativeScriptServiceResult SetAreaVisible(const NativeScriptAreaRequest&) { return {}; }
    virtual NativeScriptServiceResult SetPlayerControl(const NativeScriptPlayerControlRequest&) { return {}; }
    virtual NativeScriptServiceResult SetPedHealth(const NativeScriptPedHealthRequest&) { return {}; }
    virtual NativeScriptServiceResult RemoveAllPedWeapons(const NativeScriptPedQueryRequest&) { return {}; }
    virtual NativeScriptBooleanResult IsPedSwimming(const NativeScriptPedQueryRequest&) { return {}; }
    virtual NativeScriptServiceResult SetPlayerNeverTired(const NativeScriptPlayerControlRequest&) { return {}; }
    virtual NativeScriptServiceResult ShutAllCharsUp(const NativeScriptBooleanRequest&) { return {}; }
    virtual NativeScriptPositionResult GetPedCoordinates(const NativeScriptPedQueryRequest&) { return {}; }
    virtual NativeScriptServiceResult RemoveTextureDictionary(const NativeScriptRequestId&) { return {}; }
    virtual NativeScriptServiceResult LoadTextureDictionary(const NativeScriptTextureDictionaryRequest&) { return {}; }
    virtual NativeScriptServiceResult LoadSprite(const NativeScriptSpriteRequest&) { return {}; }
    virtual NativeScriptServiceResult LoadMissionText(const NativeScriptMissionTextRequest&) { return {}; }
    virtual NativeScriptServiceResult ClearText(const NativeScriptMissionTextRequest&) { return {}; }
    virtual NativeScriptServiceResult UseTextCommands(const NativeScriptTextCommandsRequest&) { return {}; }
    virtual NativeScriptServiceResult SetTextDrawBeforeFade(const NativeScriptRequestId&, bool) { return {}; }
    virtual NativeScriptServiceResult SetTextFont(const NativeScriptRequestId&, std::int32_t) { return {}; }
    virtual NativeScriptServiceResult SetTextStyle(const NativeScriptTextStyleRequest&) { return {}; }
    virtual NativeScriptServiceResult DisplayText(const NativeScriptTextDisplayRequest&) { return {}; }
    virtual NativeScriptServiceResult PrintNow(const NativeScriptPrintRequest&) { return {}; }
    virtual NativeScriptServiceResult LoadCutscene(const NativeScriptCutsceneRequest&) { return {}; }
    virtual NativeScriptServiceResult StartCutscene(const NativeScriptRequestId&) { return {}; }
    virtual NativeScriptServiceResult ClearCutscene(const NativeScriptRequestId&) { return {}; }
    virtual NativeScriptBooleanResult HasCutsceneLoaded(const NativeScriptRequestId&) { return {}; }
    virtual NativeScriptBooleanResult HasCutsceneFinished(const NativeScriptRequestId&) { return {}; }
    virtual NativeScriptBooleanResult WasCutsceneSkipped(const NativeScriptRequestId&) { return {}; }
    virtual NativeScriptStringResult GetCharEntryExitName(const NativeScriptPedQueryRequest&) { return {}; }
    virtual NativeScriptServiceResult SetUpdateStatsVisible(const NativeScriptRequestId&, bool) { return {}; }
    virtual NativeScriptServiceResult ClearHelp(const NativeScriptRequestId&) { return {}; }
    virtual NativeScriptServiceResult StreamScript(const NativeScriptStreamedRequest&) { return {}; }
    virtual NativeScriptServiceResult MarkStreamedScriptNoLongerNeeded(const NativeScriptStreamedRequest&) { return {}; }
    virtual NativeScriptBooleanResult LocateChar(const NativeScriptLocateCharRequest&) { return {}; }
    virtual NativeScriptBooleanResult DoesObjectExist(const NativeScriptObjectCleanupRequest&) { return {}; }
    virtual NativeScriptBooleanResult LocateCharObject2D(const NativeScriptLocateCharObjectRequest&) { return {}; }
    virtual NativeScriptReferenceResult<NativeScriptCarGeneratorRef> CreateCarGenerator(const NativeScriptCarGeneratorRequest&) { return {}; }
    virtual NativeScriptReferenceResult<NativeScriptCarGeneratorRef> CreateCarGeneratorWithPlate(const NativeScriptCarGeneratorPlateRequest&) { return {}; }
    virtual NativeScriptServiceResult SwitchCarGenerator(const NativeScriptCarGeneratorSwitchRequest&) { return {}; }
    virtual NativeScriptServiceResult SetCarGeneratorOwned(const NativeScriptCarGeneratorOwnedRequest&) { return {}; }
    virtual NativeScriptPickupCollectedResult HasPickupBeenCollected(const NativeScriptPickupReferenceRequest&);
    virtual NativeScriptServiceResult RemoveScriptPickup(const NativeScriptPickupReferenceRequest&);
};

enum class NativeScriptStatus { Advanced, BudgetYield, Waiting, Pending, Unsupported, Error };

struct NativeScriptResult {
    NativeScriptStatus Status = NativeScriptStatus::Error;
    std::uint32_t IP = 0;
    std::uint16_t Opcode = 0;
    std::size_t Executed = 0; // committed instructions in this call, NOT attempts
    std::string Message;
    std::size_t ThreadIndex = 0; // slot in Threads(), including faults; idle slots may be reused
};

struct NativeScriptClock {
    std::uint8_t Hours = 0, Minutes = 0;
    std::uint16_t Seconds = 0;
    std::uint32_t LastTickMs = 0;
    std::uint64_t Revision = 0;
    bool operator==(const NativeScriptClock&) const = default;
};

struct NativeScriptFade {
    float DurationSeconds = 0, Alpha = 0;
    std::uint8_t Direction = 0; // source eFadeFlag: 0 raises overlay alpha
    bool Fading = false, MusicFading = false, MusicFadedOut = false;
    float MusicDuration = 0, MusicWait = 0, EffectsScale = 1;
    std::uint32_t StartMs = 0, MusicStartMs = 0;
    std::uint64_t Revision = 0;
    bool operator==(const NativeScriptFade&) const = default;
};

struct NativeScriptWriteEvent {
    std::uint64_t Sequence = 0;
    std::uint32_t IP = 0;
    std::uint16_t Variable = 0;
    bool Global = true;
    std::int32_t Value = 0;
    bool operator==(const NativeScriptWriteEvent&) const = default;
};

struct NativeScriptThreadState {
    std::uint32_t IP = 0, TimeMs = 0, WakeTimeMs = 0;
    std::array<char, 8> Name{'n', 'o', 'n', 'a', 'm', 'e', 0, 0}; // CRunningScript::Init
    std::vector<std::uint32_t> Locals = std::vector<std::uint32_t>(34); // mission bank: 1024 cells
    bool DeathArrestCheckEnabled = true;
    bool Active = false, Waiting = false, UsesMissionCleanup = false;
    bool ThisMustBeTheOnlyMissionRunning = false, IsExternal = false;
    std::uint32_t BaseIP = 0;
    std::int32_t MissionIndex = -1, StreamedIndex = -1;
    std::uint64_t StreamedGeneration = 0;
    std::array<std::uint32_t, 8> ReturnStack{}; // CRunningScript::MAX_STACK_DEPTH
    std::uint8_t StackDepth = 0;
    bool Condition = false;
    std::uint8_t AndOrState = 0;
    std::int32_t SwitchValue = 0, SwitchRemaining = 0, SwitchDefault = 0;
    bool SwitchHasDefault = false, SwitchActive = false;
    std::uint64_t Commands = 0;
    std::uint64_t Generation = 1; // owned thread-slot lifetime, incremented on idle reuse
    NativeScriptWriteEvent LastOutputWrite;
    std::uint32_t LastInstructionIP = 0;
    std::uint16_t LastOpcode = 0;
    bool operator==(const NativeScriptThreadState&) const = default;
};

struct NativeScriptStreamedDefinition {
    std::array<char, 20> Name{};
    std::uint32_t FileOffset = 0, Size = 0;
    bool operator==(const NativeScriptStreamedDefinition&) const = default;
};

struct NativeScriptStreamedState {
    NativeScriptStreamedDefinition Definition;
    std::uint64_t Generation = 0;
    std::uint8_t Users = 0; // CStreamedScriptInfo::m_NumberOfUsers
    bool Loaded = false;
    bool operator==(const NativeScriptStreamedState&) const = default;
};

struct NativeScriptState : NativeScriptThreadState {
    std::array<float, 82> FloatStats{};
    std::array<std::int32_t, 223> IntStats{}; // stat ID minus 120
    std::int32_t MaximumWantedLevel = 0, MaximumChaosLevel = 0;
    NativeScriptClock Clock;
    NativeScriptFade Fade;
    std::uint64_t StatWrites = 0;
    // PedType/Acquaintance: directional source ped type -> category bit masks.
    // Starts empty; host may seed ped.dat defaults before executing commands.
    std::array<std::array<std::uint32_t, 5>, 32> Relationships{};
    std::uint64_t RelationshipRevision = 0;
    bool AlreadyRunningMission = false;
    // CTheScripts::Init zeroes this byte offset; DECLARE_MISSION_FLAG stores
    // the global variable byte offset, not its current value.
    std::uint16_t OnAMissionFlag = 0;
    // Owned counterpart of Game.h::gbLARiots (Compact 0xB72958), NOT the cheat
    // or gbLARiots_NoPoliceCars. 06C8 stores (parameter != 0); no immediate effects.
    // Game.cpp initializes false; SimpleVariablesSaveStructure persists the flag.
    // Consumer boundary: a host must use this policy in LaRiotsActiveHere's
    // location/cheat checks, then population/fire/traffic/lighting/audio updates.
    // This VM owns only the persistent setter state, not those world consumers.
    bool LaRiotsEnabled = false;
    std::uint64_t LaRiotsRevision = 0; // owned write observation, including false->false
    // Explicit limitation: storage is applied, but Stats.cpp::CheckForStatsMessage
    // (original call 0x559760) and stat notification presentation are not ported.
    // Six 0629/062A startup writes request notification processing; not "shown".
    std::uint64_t UnprocessedStatNotifications = 0;
    bool StatNotificationsImplemented = false;
    bool operator==(const NativeScriptState&) const = default;
};

struct NativeScriptMetadata {
    std::uint32_t MainSize = 0, CodeStart = 0, GlobalBytes = 0;
    std::uint32_t LargestMission = 0, MissionLocals = 0;
    std::uint32_t StreamedScripts = 0, LargestStreamed = 0, Build = 0;
    std::vector<std::uint32_t> MissionOffsets;
    std::vector<std::array<char, 24>> UsedObjects;
    std::vector<NativeScriptStreamedDefinition> StreamedDefinitions;
};

// Optional observation AFTER an instruction has committed, in scheduler order.
// No pending attempts, no ownership transfer. The callback must not allocate,
// throw or mutate/reenter the session. References expire when it returns.
class NativeScriptCommitSink {
public:
    virtual ~NativeScriptCommitSink() = default;
    virtual void OnScriptCommit(NativeScriptRequestId id, std::size_t threadIndex,
        const NativeScriptState& state, const NativeScriptThreadState& thread) noexcept = 0;
};

class NativeScriptSession {
public:
    NativeScriptSession() = default;
    NativeScriptSession(const NativeScriptSession&) = delete;
    NativeScriptSession& operator=(const NativeScriptSession&) = delete;
    // All file reads use OS_File*. gameDir is a host path, not a process-wide
    // working-directory change. Failed loads leave the existing session intact.
    bool LoadMain(const char* gameDir, std::string& error);
    // Same strict SCM header validation, useful for generated fixtures/owned IO.
    // Requires the complete owned file (size == fileBytes), including missions.
    // No runtime mission IO; immutable payload is retained separately from globals.
    bool LoadMainBytes(std::span<const std::uint8_t> prefix, std::uint64_t fileBytes, std::string& error);
    // Source CStreamedScripts ownership: metadata index is the script-file ID.
    // Load reads exactly the named VER2 member's declared unpadded byte count;
    // unload is legal only at zero active users. Failed operations retain the
    // previous generation and payload. No script bytes escape this owner.
    bool LoadStreamedScript(const char* gameDir, std::uint16_t scriptIndex, std::string& error);
    bool FulfillPendingStreamedScript(const char* gameDir, std::string& error);
    bool LoadStreamedScriptBytes(std::uint16_t scriptIndex, std::span<const std::uint8_t> bytes, std::string& error);
    bool UnloadStreamedScript(std::uint16_t scriptIndex, std::string& error);
    NativeScriptResult Step(NativeScriptServices& services);
    NativeScriptResult Run(NativeScriptServices& services, std::size_t quota);
    // Step/Run remain main-thread-only observation helpers. RunPass processes
    // the active list, head first, until each thread waits/terminates. It snapshots
    // next BEFORE processing; newly launched threads first run on the next pass.
    // BudgetYield/Pending retain the pass cursor; call again to continue it.
    // Mission entry/lifecycle here owns VM storage/list/flags only. Source host
    // ClearSkip(false), per-pass death/arrest checks and ShutdownThisScript world
    // cleanup are not implemented. Unknown world opcodes fault before advancing.
    NativeScriptResult RunPass(NativeScriptServices& services, std::size_t quota);
    NativeScriptResult RunPass(NativeScriptServices& services, std::size_t quota, NativeScriptCommitSink* sink);
    std::span<const NativeScriptThreadState> Threads() const { return m_Threads; }
    std::span<const NativeScriptStreamedState> StreamedScripts() const { return m_StreamedStates; }
    bool SeedRelationships(const std::array<std::array<std::uint32_t, 5>, 32>& relationships);
    // Caller supplies monotonic, pause-aware GAME milliseconds, not wall time.
    // Advances wake/local timers and source fade state. Clock is the 00C0 setter
    // snapshot (Revision lets the host reset its clock); calendar progression
    // belongs to the host. UINT32 wrap is explicitly unsupported in this slice.
    bool AdvanceTime(std::uint32_t nowMs, std::string& error);
    const NativeScriptState& State() const { return m_State; } // main thread + shared VM state
    const NativeScriptMetadata& Metadata() const { return m_Metadata; }
    bool ReadGlobal(std::uint16_t byteOffset, std::int32_t& value) const;
    // Read-only schema inspection of the current instruction in one owned
    // thread. Known-but-unimplemented forms are returned as Unsupported
    // coverage; this does not advance, execute, or turn them into NOPs.
    bool InspectInstruction(std::size_t threadIndex, NativeScriptInstructionForm& out, std::string& error) const;
    bool Loaded() const { return m_Loaded; }
    std::uint64_t SessionId() const noexcept { return m_SessionId; }
    bool PassOutstanding() const noexcept { return m_Pending || !m_Pass.empty(); }

private:
    friend class NativeScriptPortableSave;
    struct Instruction {
        std::uint16_t Opcode = 0;
        std::uint32_t Next = 0;
        std::array<std::uint32_t, NativeScriptMaxOperands> Values{};
        std::uint32_t OutputValue = 0; // decoded old cell for checked in-place arithmetic
        std::array<char, 8> Text{};
        std::array<char, 16> LongText{};
        std::array<std::array<char, 16>, NativeScriptMaxOperands> Strings{};
        std::array<NativeScriptOperandType, NativeScriptMaxOperands> OperandTypes{};
        std::array<std::uint8_t, NativeScriptMaxOperands> Tags{}; // normalized scalar/array operand bank
        std::array<std::uint8_t, NativeScriptMaxOperands> RawTags{}; // exact source operand form
        std::array<std::uint8_t, NativeScriptMaxOperands> ArrayCounts{}, ArrayFlags{};
        std::uint8_t OperandCount = 0, FixedOperandCount = 0;
        bool VariadicArguments = false;
        bool OutputGlobal = true;
        bool Negated = false;
        std::int32_t Int(unsigned i) const;
        float Float(unsigned i) const;
    };
    bool Decode(std::size_t thread, std::uint32_t ip, Instruction& instruction, std::string& error,
        bool validateRuntimeValues = true) const;
    bool ScriptStorage(std::size_t thread, std::uint32_t ip, std::span<const std::uint8_t>& bytes,
        std::uint32_t& base, std::string& error) const;
    bool LoadStreamedScriptInternal(const char* gameDir, std::uint16_t scriptIndex, bool pendingFulfillment,
        std::string& error);
    bool LoadStreamedScriptBytesInternal(std::uint16_t scriptIndex, std::span<const std::uint8_t> bytes,
        bool pendingFulfillment, std::string& error);
    bool IsGlobal(std::uint16_t byteOffset) const;
    bool IsTarget(std::size_t thread, std::int32_t target) const;
    std::uint32_t Target(std::size_t thread, std::int32_t target) const;
    NativeScriptResult StepThread(NativeScriptServices& services, std::size_t thread);
    NativeScriptResult Fail(std::size_t thread, NativeScriptStatus status, std::uint16_t opcode, std::string message);

    NativeScriptState m_State;
    NativeScriptMetadata m_Metadata;
    std::vector<std::uint8_t> m_Memory;
    std::vector<std::uint8_t> m_Payload, m_Mission;
    std::vector<std::vector<std::uint8_t>> m_StreamedPayloads;
    std::vector<NativeScriptStreamedState> m_StreamedStates;
    std::vector<NativeScriptThreadState> m_Threads;
    std::vector<std::size_t> m_Active, m_Idle, m_Pass;
    std::size_t m_PassCursor = 0;
    std::array<std::uint32_t, 6> m_Headers{};
    std::array<std::uint32_t, 6> m_HeaderTargets{};
    mutable std::string m_TargetError;
    std::uint64_t m_SessionId = 0, m_CommandSequence = 0;
    bool m_Loaded = false, m_Pending = false, m_InService = false;
    bool m_Faulted = false;
    NativeScriptResult m_Fault;
    std::optional<Instruction> m_PendingInstruction;
    std::size_t m_PendingThread = 0;
};
