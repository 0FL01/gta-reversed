// Bounded, owned native SCM startup interpreter. No original-address dispatcher.
// Supported contract: main first WAIT + mission-0 policy/numeric initialization
// through locked-property/contact-radar and IPL ENEX flags; unknowns fault.
// No result means "game booted"; hosts choose an explicit observation boundary.
#pragma once

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
template<typename Ref> struct NativeScriptReferenceResult {
    NativeScriptServiceResult Result;
    Ref Reference;
};
struct NativeScriptPlayerLookupRequest {
    NativeScriptRequestId Id;
    std::int32_t PlayerIndex = 0;
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
struct NativeScriptContactBlipRequest {
    NativeScriptRequestId Id;
    NativeScriptPosition Position;
    std::int32_t Sprite = 0;
};
struct NativeScriptBlipDisplayRequest {
    NativeScriptRequestId Id;
    NativeScriptBlipRef Blip;
    std::int32_t Display = 0;
};
// 09B4: XY, search range, low-16-bit flag mask, integer boolean (nonzero).
// No Z, output handle or compare update. Host owns nearest lookup + word write.
struct NativeScriptEntryExitFlagRequest {
    NativeScriptRequestId Id;
    float X = 0, Y = 0, Radius = 0;
    std::int32_t Mask = 0, State = 0;
};
struct NativeScriptGarageRequest {
    NativeScriptRequestId Id;
    std::array<char, 8> Name{};
};

class NativeScriptServices {
public:
    virtual ~NativeScriptServices() = default;
    // Pending calls are polled with the SAME ID and request. Implementations
    // must be idempotent per ID, commit external effects only on Ready, and
    // own cancellation of outstanding work on session destruction. No method
    // may reenter/mutate this session. Unsupported/Error are terminal VM faults.
    virtual NativeScriptServiceResult RequestCollision(const NativeScriptCollisionRequest&) = 0;
    virtual NativeScriptServiceResult LoadScene(const NativeScriptSceneRequest&) = 0;
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
    virtual NativeScriptReferenceResult<NativeScriptBlipRef> CreateContactBlip(const NativeScriptContactBlipRequest&) { return {}; }
    virtual NativeScriptServiceResult SetBlipDisplay(const NativeScriptBlipDisplayRequest&) { return {}; }
    virtual NativeScriptServiceResult SetEntryExitFlag(const NativeScriptEntryExitFlagRequest&) { return {}; }
    virtual NativeScriptServiceResult DeactivateGarage(const NativeScriptGarageRequest&) { return {}; }
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
    std::int32_t MissionIndex = -1;
    bool Condition = false;
    std::uint8_t AndOrState = 0;
    std::uint64_t Commands = 0;
    std::uint64_t Generation = 1; // owned thread-slot lifetime, incremented on idle reuse
    NativeScriptWriteEvent LastOutputWrite;
    std::uint32_t LastInstructionIP = 0;
    std::uint16_t LastOpcode = 0;
    bool operator==(const NativeScriptThreadState&) const = default;
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
    // CTheScripts::Init zeroes this byte offset. DECLARE_MISSION_FLAG is still
    // a strict unsupported instruction; thread presence is NOT mission status.
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
    std::span<const NativeScriptThreadState> Threads() const { return m_Threads; }
    bool SeedRelationships(const std::array<std::array<std::uint32_t, 5>, 32>& relationships);
    // Caller supplies monotonic, pause-aware GAME milliseconds, not wall time.
    // Advances wake/local timers and source fade state. Clock is the 00C0 setter
    // snapshot (Revision lets the host reset its clock); calendar progression
    // belongs to the host. UINT32 wrap is explicitly unsupported in this slice.
    bool AdvanceTime(std::uint32_t nowMs, std::string& error);
    const NativeScriptState& State() const { return m_State; } // main thread + shared VM state
    const NativeScriptMetadata& Metadata() const { return m_Metadata; }
    bool ReadGlobal(std::uint16_t byteOffset, std::int32_t& value) const;
    bool Loaded() const { return m_Loaded; }

private:
    struct Instruction {
        std::uint16_t Opcode = 0;
        std::uint32_t Next = 0;
        std::array<std::uint32_t, 6> Values{};
        std::uint32_t OutputValue = 0; // decoded old cell for checked in-place arithmetic
        std::array<char, 8> Text{};
        bool OutputGlobal = true;
        bool Negated = false;
        std::int32_t Int(unsigned i) const;
        float Float(unsigned i) const;
    };
    bool Decode(std::size_t thread, std::uint32_t ip, Instruction& instruction, std::string& error) const;
    bool IsGlobal(std::uint16_t byteOffset) const;
    bool IsTarget(std::size_t thread, std::int32_t target) const;
    std::uint32_t Target(std::size_t thread, std::int32_t target) const;
    NativeScriptResult StepThread(NativeScriptServices& services, std::size_t thread);
    NativeScriptResult Fail(std::size_t thread, NativeScriptStatus status, std::uint16_t opcode, std::string message);

    NativeScriptState m_State;
    NativeScriptMetadata m_Metadata;
    std::vector<std::uint8_t> m_Memory;
    std::vector<std::uint8_t> m_Payload, m_Mission;
    std::vector<NativeScriptThreadState> m_Threads;
    std::vector<std::size_t> m_Active, m_Idle, m_Pass;
    std::size_t m_PassCursor = 0;
    std::array<std::uint32_t, 6> m_Headers{};
    std::array<std::uint32_t, 6> m_HeaderTargets{};
    std::uint64_t m_SessionId = 0, m_CommandSequence = 0;
    bool m_Loaded = false, m_Pending = false, m_InService = false;
    bool m_Faulted = false;
    NativeScriptResult m_Fault;
    std::optional<Instruction> m_PendingInstruction;
    std::size_t m_PendingThread = 0;
};
