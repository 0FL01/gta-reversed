// CPU-only source model retention. Call the loader at startup or on the sole
// parser worker. This API neither starts a worker nor creates a vehicle.
#pragma once

#include "app/platform/linux/NativeCarGenerators.h"
#include "app/platform/linux/NativeCollisionAssets.h"
#include "app/platform/linux/WorldShot.h"
#include "app/platform/linux/CarPose.h"

#include <memory>
#include <span>

using NativeGeneratedVehicleMatrix = std::array<float, 12>; // right/up/at/position

struct NativeGeneratedVehicleFrame {
    std::string Name; // exact DFF NodeName, NOT a guessed game hierarchy ID
    std::int32_t Parent = -1;
    std::uint32_t Flags{};
    NativeGeneratedVehicleMatrix LocalBind{}, ModelBind{};
    NativeGeneratedVehicleMatrix LocalPose{}, ModelPose{};
};

enum class NativeGeneratedVehicleComponentUse : std::uint8_t {
    PristineBody, WheelInstance, DamagedAlternative, FarLod, NonRendering, AlphaSuppressed,
};

struct NativeGeneratedVehicleComponent {
    std::uint32_t Atomic{}, Geometry{}, SourceFrame{}, BindFrame{}, Flags{};
    std::uint32_t RuntimeFlags{}, GeometryFlags{}, RuntimeGeometryFlags{};
    std::optional<std::uint8_t> MaterialAlpha;
    NativeGeneratedVehicleComponentUse Use{};
    std::int32_t Mesh = -1; // alternatives retain provenance but have no near mesh
};

struct NativeGeneratedVehiclePaint {
    // Parallel to Scene.meshes[*].surfaces. RGB marker slot 0..3, -1 ordinary.
    // Scene.triCol and surface.color retain authored RGB; no first/random scheme.
    std::vector<std::int8_t> Slots;
};

struct NativeGeneratedVehiclePacket {
    NativeCarGeneratorModelDefinition Definition;
    WorldShotScene Scene{}; // model-local presentation soup, owned decoded RGBA
    enum class PoseContract { ParkedBind400, FreshParkedAbandoned476 };
    PoseContract Pose = PoseContract::ParkedBind400;
    // 476 is pristine/closed, engine off, gear/phase/roll/steer zero, first
    // PreRender BEFORE control/collision. These owned inputs can replay CarPose.
    std::vector<VehicleAtomicOverride> AtomicOverrides;
    std::vector<VehicleFrameOverride> FrameOverrides;
    std::vector<NativeGeneratedVehicleFrame> Frames;
    std::vector<NativeGeneratedVehicleComponent> Components;
    std::vector<NativeGeneratedVehiclePaint> Paint;
    std::shared_ptr<const NativeCollisionModel> Collision; // catalog or DFF-plugin ownership
    bool EmbeddedCollision = false; // source 0x253f2fa; authored name retained
    std::string DffSource, ModelTxdSource, CommonTxdSource;
    // TexSample's upstream common-before-model + remap/#emap lookup was used.
    // Image names are the resolved dictionary names, not generated textures.
    std::uint64_t DffFingerprint{};
    std::uint32_t DffBytes{};
};

using NativeGeneratedVehicleAsset = std::shared_ptr<const NativeGeneratedVehiclePacket>;

enum class NativeGeneratedVehicleAssetStatus : std::uint8_t { Ready, Unsupported, Error };
struct NativeGeneratedVehicleAssetResult {
    NativeGeneratedVehicleAssetStatus Status = NativeGeneratedVehicleAssetStatus::Error;
    std::string Detail;
    explicit operator bool() const { return Status == NativeGeneratedVehicleAssetStatus::Ready; }
};

// Audited: actual 400/landstal bind and 476/rustler fresh parked S0 only.
// 461/pcj600 and 430/predator have different wheel/component contracts.
// Other models are explicitly Unsupported; no fallback/model-ID substitution.
// Output is published only on Ready. The caller owns RW engine lifetime and
// serializes all parser use (including CarPose); no RW pointer escapes.
// Copying Asset retains model/COL data; resetting the last Asset releases CPU
// retention. This does not acquire a source streaming reference or pool slot.
NativeGeneratedVehicleAssetResult NativeGeneratedVehicleAssets_Load(
    const char* gameDir, const NativeCarGeneratorModelDefinition& definition,
    const NativeCollisionAssets& catalog, NativeGeneratedVehicleAsset& out);

// Bounded, RW-free hierarchy metadata parser, useful for unsupported-model
// inspection too. Does not imply render support. Atomic output on failure.
bool NativeGeneratedVehicleAssets_ReadFrames(std::span<const std::uint8_t> dff,
    std::vector<NativeGeneratedVehicleFrame>& out, std::string& error);
