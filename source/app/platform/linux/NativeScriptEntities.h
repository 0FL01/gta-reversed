// Owned locked-property slice. Startup parsers are exclusive; frame APIs use
// only owned triangles/RGBA/GXT. No game_sa pool or original-address linkage.
#pragma once
#include "app/platform/linux/NativeScriptSession.h"
#include "app/platform/linux/WorldShot.h"
#include <string_view>
#include <optional>

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
    void Show(std::string_view text, std::uint32_t lines);
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
    bool m_HasTime = false, m_NewMessage = false;
};

struct NativeScriptPickup {
    NativeScriptPickupRef Reference;
    NativeScriptPosition AuthoredPosition, Position; // CPickup stores int16 / 8
    std::array<char, 8> Text{};
    std::string Message;
    WorldShotScene Actor; // actual prepared DFF; absolute phase rebuilt from bind
    bool Active = false, HelpMessageDisplayed = false;
    static constexpr int Model = 1272, Type = 17; // eModelID / ePickupType
};

struct NativeScriptRadarBlip {
    NativeScriptBlipRef Reference;
    NativeScriptPosition Position;
    int Sprite = 32, Display = 3; // propertyR, source BOTH at creation
    bool Active = false, ShortRange = true, Contact = true;
};

// For exterior live radar (not frontend full map): CRadar::DrawCoordBlip,
// DisplayThisBlip and HasThisBlipBeenRevealed. Real distance BEFORE rim clamp.
bool NativeScriptRadarVisible(const NativeScriptRadarBlip& blip, float distance,
    bool playerOnMission, unsigned radarZoom, bool exterior);
// Shared startup loader used by the host AND the real HUD.Load. Engine must
// already be started; restores current dictionary and retains no RW pointers.
bool NativeScriptEntities_LoadRadar(const char* gameDir, WorldShotImage& image, std::string& error);

class NativeScriptEntities {
public:
    explicit NativeScriptEntities(std::size_t pickupCapacity = 620, std::size_t blipCapacity = 175);
    bool LoadBeforeWorker(const char* gameDir, std::string& error);
    NativeScriptReferenceResult<NativeScriptPickupRef> CreateLockedProperty(const NativeScriptLockedPropertyRequest&);
    NativeScriptReferenceResult<NativeScriptBlipRef> CreateContactBlip(const NativeScriptContactBlipRequest&);
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
    std::span<const NativeScriptPickup> Pickups() const { return m_Pickups; }
    std::span<const NativeScriptRadarBlip> Blips() const { return m_Blips; }
    const WorldShotScene& Actors() const { return m_Actors; } // camera-visible actual actors
    const WorldShotScene& PreparedModel() const { return m_Model; }
    const NativeScriptPropertyGeometry& PropertyGeometry() const { return m_PropertyGeometry; }
    const WorldShotImage& RadarImage() const { return m_Radar; }
    const std::string& HelpMessage() const { return m_HelpMessage; }
    std::uint64_t HelpRevision() const { return m_HelpRevision; }
    bool AdvanceTime(std::uint32_t gameMs, std::string& error);
    NativeScriptHelpView HelpPresentation() const { return m_Help.View(); }
    std::uint32_t HelpLifetimeMs() const { return m_Help.LifetimeMs(); }
    std::uint64_t Revision() const { return m_Revision; }
    bool OwnsRequest(NativeScriptRequestId id) const { return FindEvent(id) != nullptr; }
private:
    struct Event {
        NativeScriptRequestId Id;
        std::uint16_t Opcode = 0;
        NativeScriptPosition Position;
        std::array<char, 8> Text{};
        std::int32_t Argument = 0, Reference = -1;
    };
    const Event* FindEvent(NativeScriptRequestId id) const;
    std::vector<NativeScriptPickup> m_Pickups;
    std::vector<NativeScriptRadarBlip> m_Blips;
    std::vector<Event> m_Events;
    WorldShotScene m_Model{}, m_Actors{};
    NativeScriptPropertyGeometry m_PropertyGeometry;
    WorldShotImage m_Radar{};
    std::array<std::string, 3> m_Messages;
    std::array<std::uint32_t, 3> m_MessageLines{};
    NativeScriptHelpPresentation m_Help;
    std::string m_HelpMessage;
    std::uint64_t m_HelpRevision = 0, m_Revision = 0;
    struct FrameInput {
        NativeScriptPosition Ped, Camera;
        bool Alive = false, InVehicle = false;
        bool operator==(const FrameInput&) const = default;
    };
    std::optional<FrameInput> m_Frame, m_PublishedFrame;
    std::uint64_t m_PresentationRevision = 0;
    std::uint32_t m_GameMs = 0;
    bool m_HasTime = false;
    bool m_Loaded = false;
};
