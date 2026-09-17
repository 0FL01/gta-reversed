// Owned property/save-token slice. Startup parsers are exclusive; frame APIs use
// only owned triangles/RGBA/GXT. No game_sa pool or original-address linkage.
#pragma once
#include "app/platform/linux/NativePlayerActivity.h"
#include "app/platform/linux/NativeScriptSession.h"
#include "app/platform/linux/WorldShot.h"
#include <string_view>
#include <optional>
#include <functional>

struct NativeScriptPropertyGeometry {
    std::array<float, 3> ColMin{}, ColMax{};
    std::string ColLibrary;
    std::uint16_t ColHeaderId = 0;
    float Scale = 1;
};
// Pure source transform; COL bounds, never DFF/render bounds, determine scale.
float NativeScriptPropertyScale(std::array<float, 3> minimum, std::array<float, 3> maximum);
WorldShotScene NativeScriptPropertyActor(const WorldShotScene& bind, NativeScriptPosition position,
    float scale, std::uint32_t gameMs);

// CFont FONT_SUBTITLES (font1, no Pricedown remapping), help wrap at 230
// from x=34, scale=(0.52,1.1). Shared layout supplies the timed line count.
std::vector<std::string> NativeScriptHelpLines(std::string_view text, std::span<const int, 208> widths);
struct NativeScriptHelpView {
    std::string_view Text;
    std::uint8_t Alpha = 0; // source font/background alpha, 0..200
};
// Only CHud::SetHelpMessage(text,false,false,false), used by locked pickups.
// Retail static RE provenance/assertions: NativeScriptEntitiesSourceProbe.py.
class NativeScriptHelpPresentation {
public:
    void Show(std::string_view text, std::uint32_t lines, bool quick = false);
    void Clear();
    bool Displayed() const { return m_State != 0; } // CHud::HelpMessageDisplayed
    // One update per native UNPAUSED frame. Duplicate stamps are idempotent;
    // backwards/ambiguous (>INT32_MAX elapsed) stamps fail without mutation.
    bool AdvanceTime(std::uint32_t gameMs, std::string& error);
    NativeScriptHelpView View() const { return {m_State || m_NewMessage ? m_Text : std::string_view{}, m_Alpha}; }
    std::uint32_t LifetimeMs() const { return m_Lifetime; }
private:
    std::string m_Text;
    std::uint32_t m_LastTime = 0, m_Lifetime = 0;
    std::uint64_t m_Timer = 0;
    std::int64_t m_FadeTimer = 0;
    std::uint8_t m_State = 0, m_Alpha = 0;
    bool m_HasTime = false, m_NewMessage = false, m_Quick = false;
};

struct NativeScriptPickup {
    NativeScriptPickupRef Reference;
    NativeScriptPosition AuthoredPosition, Position; // CPickup stores int16 / 8
    std::array<char, 8> Text{};
    std::string Message;
    WorldShotScene Actor; // actual prepared DFF; absolute phase rebuilt from bind
    bool Active = false, HelpMessageDisplayed = false;
    int Model = 1272, Type = 17; // eModelID / ePickupType; sale=1273/18, save=1277/3
    std::int32_t Ammo = 0;
    std::int32_t Price = 0; // CPickup::m_nAmmo bits; signed comparison in Update
    std::uint16_t CostValue = 0; // CObject::m_wCostValue = uint32(price)/5
    std::uint32_t MessageLines = 0;
    std::string Label;
    // Ordinary pickup source scheduler state, not property visibility policy.
    bool Visible = false, ObjectPresent = false;
    std::uint32_t RegenerationTime = 0; // type3 has no timeout or regeneration
};

enum class NativeScriptPickupRequirementKind {
    None,
    PlayerTaskEligibility, // legacy missing snapshot, or unknown CanPlayerStartMission inputs
    PlayerActivityAuthority,
    PlayerPickupDesire,
};
struct NativeScriptPickupRequirement {
    NativeScriptPickupRequirementKind Kind = NativeScriptPickupRequirementKind::None;
    NativeScriptPickupRef Pickup;
    NativeScriptPosition Position;
    std::uint32_t FrameCounter = 0;
    int Model = -1, Type = 0;
    std::uint64_t ActivityOwnerRevision = 0;
    NativeMissionStartDecision MissionStart;
};

struct NativeScriptPickupReferenceRequest {
    NativeScriptRequestId Id;
    NativeScriptPickupRef Pickup;
};
struct NativeScriptPickupCollectedResult {
    NativeScriptServiceResult Result;
    bool Collected = false;
};
struct NativeScriptPadShakeEvent {
    NativeScriptPickupRef Pickup;
    std::uint32_t FrameCounter = 0;
    std::int16_t TimeMs = 120;
    std::uint8_t Frequency = 100;
    std::uint32_t Arg2 = 0;
};

// Explicit input to CPickups::Update's sale slice. Money is a READ of the
// live CPlayerInfo-equivalent, never a credit or a requested purchase amount.
struct NativeScriptPropertyInput {
    std::uint32_t FrameCounter = 0;
    std::int32_t Money = 0;
    bool OnMission = false, CollectJustDown = false, Targeting = false;
    bool ControlsDisabled = false, Busy = false, Replay = false;
    bool Cutscene = false, Widescreen = false, HelpBlocked = false;
    bool CutsceneLoaded = false, Coop = false; // ordinary GiveUsAPickUpObject / mission gate
    bool operator==(const NativeScriptPropertyInput&) const = default;
};
enum class NativeScriptPropertyInteractionStatus { None, OnMission, InsufficientFunds, ScriptPurchaseRequired };
struct NativeScriptPropertyInteraction {
    NativeScriptPropertyInteractionStatus Status = NativeScriptPropertyInteractionStatus::None;
    NativeScriptPickupRef Pickup;
    std::int32_t Price = 0, Balance = 0;
    std::uint32_t FrameCounter = 0;
};
// Pickup.cpp:585 never debits/removes/returns isRemoved for type18. A funded
// press requires subsequent script purchase logic; it is NOT a collected event.
NativeScriptPropertyInteractionStatus NativeScriptPropertyCollect(std::int32_t price,
    const NativeScriptPropertyInput& input, std::uint8_t collectBuffer);
struct NativeScriptPropertyLabel {
    NativeScriptPickupRef Pickup;
    NativeScriptPosition Position; // source object position + Z 0.7, project in parent
    std::uint32_t Price = 0; // 5*uint16(uint32(ammo)/5), not unquantized script price
    std::string Text;
    std::array<std::uint8_t, 3> Color{255, 100, 100}; // source category47 RGB
    std::uint8_t Alpha = 0;
};

// CRadar BLIP_CONTACT_POINT versus BLIP_COORD. Draw3dMarkers has no COORD case.
enum class NativeScriptBlipKind { Contact, Coordinate };
struct NativeScriptRadarBlip {
    NativeScriptBlipRef Reference;
    NativeScriptPosition Position;
    int Sprite = 32, Display = 3; // propertyR, source BOTH at creation
    bool Active = false, ShortRange = true, Contact = true;
    NativeScriptBlipKind Kind = NativeScriptBlipKind::Contact;
    // SetCoordBlip defaults; Position includes authored height. Sprite rendering
    // uses white/255; these trace properties apply to source non-sprite markers.
    std::uint32_t Colour = 8; // BLIP_COLOUR_DESTINATION (input color5 is unused)
    float SphereRadius = 1.0f;
    bool DrawSphere = false;
    std::uint16_t Size = 1;
    bool Bright = true, Friendly = false, Fade = false;
};

// Live radar (not frontend full map): CRadar::DrawCoordBlip, DisplayThisBlip
// and HasThisBlipBeenRevealed; default category toggles. Real distance BEFORE
// rim clamp. Exterior means CanSeeOutSideFromCurrArea AND player area==0.
bool NativeScriptRadarVisible(const NativeScriptRadarBlip& blip, float distance,
    bool playerOnMission, unsigned radarZoom, bool exterior);
// Shared startup loader used by the host AND the real HUD.Load. Engine must
// already be started; restores current dictionary and retains no RW pointers.
bool NativeScriptEntities_LoadRadar(const char* gameDir, WorldShotImage& image, std::string& error);
bool NativeScriptEntities_LoadRadar(const char* gameDir, WorldShotImage& image, std::string& error, int sprite);
// Exclusive startup static DFF/TXD preloader. Returns owned geometry/texels only.
struct NativeScriptStaticModelOptions {
    bool ResetFrame = false, FirstAtomicOnly = false;
    std::optional<std::array<float, 4>> FirstMaterialColor;
    bool RequireTexture = true;
    bool VehicleShared = false;
    bool ResidencyOnly = false;
};
bool NativeScriptEntities_LoadStaticModel(const char* gameDir, const std::string& model,
    const std::string& txd, WorldShotScene& scene, std::string& error, const NativeScriptStaticModelOptions& options = {});

class NativeScriptEntities {
public:
    explicit NativeScriptEntities(std::size_t pickupCapacity = 620, std::size_t blipCapacity = 175);
    bool LoadBeforeWorker(const char* gameDir, std::string& error);
    NativeScriptReferenceResult<NativeScriptPickupRef> CreateLockedProperty(const NativeScriptLockedPropertyRequest&);
    NativeScriptReferenceResult<NativeScriptPickupRef> CreateForSaleProperty(const NativeScriptForSalePropertyRequest&);
    NativeScriptReferenceResult<NativeScriptPickupRef> CreatePickup(const NativeScriptPickupRequest&,
        NativeScriptPosition camera, std::uint32_t gameMs);
    NativeScriptReferenceResult<NativeScriptPickupRef> CreatePickupWithAmmo(const NativeScriptPickupAmmoRequest&,
        std::uint32_t gameMs);
    NativeScriptReferenceResult<NativeScriptBlipRef> CreateContactBlip(const NativeScriptContactBlipRequest&);
    // Exact host-owned live GPU callback; no CPU-image/property fallback. Replays
    // resolve the original generation before querying later renderer readiness.
    NativeScriptReferenceResult<NativeScriptBlipRef> CreateCoordinateBlip(const NativeScriptCoordinateBlipRequest&,
        const std::function<bool(std::int32_t)>& radarSpriteReady);
    NativeScriptServiceResult SetBlipDisplay(const NativeScriptBlipDisplayRequest&);
    const NativeScriptPickup* ResolvePickup(NativeScriptPickupRef ref) const;
    const NativeScriptRadarBlip* ResolveBlip(NativeScriptBlipRef ref) const;
    // Host lifetime/cleanup operations; no remove opcode is added to the VM.
    bool RemovePickup(NativeScriptPickupRef ref);
    bool RemoveBlip(NativeScriptBlipRef ref);
    // Ped position is source entity/root position, NOT native foot position.
    // Once-only locked help latch follows Pickup.cpp:569. Never grants money,
    // removes a locked pickup, or treats proximity as successful collection.
    // Stages inputs only. AdvanceTime atomically publishes geometry/help/latches.
    void Tick(NativeScriptPosition ped, NativeScriptPosition camera, bool alive, bool inVehicle);
    void Tick(NativeScriptPosition ped, NativeScriptPosition camera, bool alive, bool inVehicle, const NativeScriptPropertyInput& input);
    // Copies one immutable player task/event snapshot. Frame stamps are uint32
    // source counters; owner revisions do not wrap. Exact repeats are idempotent.
    bool UpdatePlayerActivity(std::uint32_t frameCounter, std::uint64_t ownerRevision,
        const NativePlayerActivitySnapshot& snapshot, std::string& error);
    // Request-ID operations for the VM adapter. The collected lookup consumes a
    // full generation reference from the source-sized ring even after removal.
    NativeScriptPickupCollectedResult HasPickupBeenCollected(const NativeScriptPickupReferenceRequest&);
    NativeScriptServiceResult RemoveScriptPickup(const NativeScriptPickupReferenceRequest&);
    // This acknowledges one owned source pad-shake event. Actual OS feedback is
    // a parent responsibility and is not claimed by this entity service.
    std::optional<NativeScriptPadShakeEvent> ConsumePadShake();
    std::span<const NativeScriptPickup> Pickups() const { return m_Pickups; }
    std::span<const NativeScriptRadarBlip> Blips() const { return m_Blips; }
    const WorldShotScene& Actors() const { return m_Actors; } // camera-visible actual actors
    const WorldShotScene& PreparedModel() const { return m_Model; }
    const WorldShotScene& PreparedForSaleModel() const { return m_ForSaleModel; }
    const WorldShotScene& PreparedSaveModel() const { return m_SaveModel; }
    const NativeScriptPropertyGeometry& SaveGeometry() const { return m_SaveGeometry; }
    // Explicit fail-closed requirement. Snapshot-aware callers can satisfy it
    // with a later frame/revision; legacy callers retain the old latch contract.
    const NativeScriptPickupRequirement& PickupRequirement() const { return m_PickupRequirement; }
    // Upload ONCE before worker startup: locked, sale, then save images. Actors'
    // triImg indices use this immutable combined table, never a late model load.
    const std::vector<WorldShotImage>& PreparedImages() const { return m_Images; }
    const NativeScriptPropertyGeometry& ForSaleGeometry() const { return m_ForSaleGeometry; }
    // Pool order candidates: parent projects near/far, then admits at most16.
    // Pickups::RenderPickUpText: pricedown, centered, proportional, scaleXY=
    // min(screenWidth/640, projectedWidthOrHeight/30), no background.
    std::span<const NativeScriptPropertyLabel> PriceLabels() const { return m_Labels; }
    // Frame result, not a queued collected event. Process once for its published
    // FrameCounter; duplicate rendering/time calls retain the same identity.
    const NativeScriptPropertyInteraction& Interaction() const { return m_Interaction; }
    std::uint8_t CollectBuffer() const { return m_CollectBuffer; }
    const NativeScriptPropertyGeometry& PropertyGeometry() const { return m_PropertyGeometry; }
    const WorldShotImage& RadarImage() const { return m_Radar; }
    const WorldShotImage* RadarImage(int sprite) const { return sprite == 32 ? &m_Radar : sprite == 31 ? &m_ForSaleRadar : nullptr; }
    const std::string& HelpMessage() const { return m_HelpMessage; }
    std::uint64_t HelpRevision() const { return m_HelpRevision; }
    bool AdvanceTime(std::uint32_t gameMs, std::string& error);
    NativeScriptHelpView HelpPresentation() const { return m_Help.View(); }
    std::uint32_t HelpLifetimeMs() const { return m_Help.LifetimeMs(); }
    NativeScriptServiceResult ClearHelp();
    std::uint64_t Revision() const { return m_Revision; }
    bool OwnsRequest(NativeScriptRequestId id) const;
private:
    struct Event {
        NativeScriptRequestId Id;
        std::uint16_t Opcode = 0;
        NativeScriptPosition Position;
        std::array<char, 8> Text{};
        std::int32_t Argument = 0, Reference = -1;
        std::int32_t Model = 0;
        std::array<char, 24> ModelName{};
        std::int32_t Ammo = 0;
    };
    enum class PickupOperationKind { HasBeenCollected, Remove };
    struct PickupOperation {
        NativeScriptRequestId Id;
        NativeScriptPickupRef Pickup;
        PickupOperationKind Kind = PickupOperationKind::HasBeenCollected;
        bool Collected = false;
    };
    struct PlayerActivityFrame {
        std::uint32_t FrameCounter = 0;
        std::uint64_t OwnerRevision = 0;
        NativePlayerActivitySnapshot Snapshot;
    };
    const Event* FindEvent(NativeScriptRequestId id) const;
    const PickupOperation* FindPickupOperation(NativeScriptRequestId id) const;
    NativeScriptReferenceResult<NativeScriptPickupRef> CreateProperty(const NativeScriptForSalePropertyRequest&, bool forSale);
    std::vector<NativeScriptPickup> m_Pickups;
    std::vector<NativeScriptRadarBlip> m_Blips;
    std::vector<Event> m_Events;
    std::vector<PickupOperation> m_PickupOperations;
    WorldShotScene m_Model{}, m_ForSaleModel{}, m_Actors{};
    WorldShotScene m_SaveModel{}, m_OysterModel{}, m_HorseshoeModel{}, m_PhotoModel{};
    NativeScriptPropertyGeometry m_SaveGeometry;
    NativeScriptPickupRequirement m_PickupRequirement;
    std::optional<PlayerActivityFrame> m_PlayerActivity;
    NativeScriptPropertyGeometry m_PropertyGeometry, m_ForSaleGeometry;
    WorldShotImage m_Radar{}, m_ForSaleRadar{};
    std::vector<WorldShotImage> m_Images;
    std::vector<NativeScriptPropertyLabel> m_Labels;
    std::array<std::string, 3> m_Messages;
    std::array<std::uint32_t, 3> m_MessageLines{};
    std::array<std::string, 3> m_SaleMessages, m_LabelMessages;
    std::array<std::string, 2> m_Denials;
    std::array<int, 208> m_HelpWidths{};
    NativeScriptHelpPresentation m_Help;
    std::string m_HelpMessage;
    std::uint64_t m_HelpRevision = 0, m_Revision = 0;
    struct FrameInput {
        NativeScriptPosition Ped, Camera;
        bool Alive = false, InVehicle = false;
        std::optional<NativeScriptPropertyInput> Property;
        bool operator==(const FrameInput&) const = default;
    };
    std::optional<FrameInput> m_Frame, m_PublishedFrame;
    std::uint64_t m_PresentationRevision = 0;
    std::uint32_t m_GameMs = 0;
    bool m_HasTime = false;
    bool m_Loaded = false;
    std::uint8_t m_CollectBuffer = 0;
    std::optional<std::uint32_t> m_CollectFrame;
    NativeScriptPropertyInteraction m_Interaction;
    std::array<NativeScriptPickupRef, 20> m_CollectedPickups{};
    std::size_t m_CollectedPickupCursor = 0;
    std::vector<NativeScriptPadShakeEvent> m_PadShakes;
    std::size_t m_PadShakeCursor = 0;
    bool m_UsesPlayerActivity = false;
};
