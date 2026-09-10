#pragma once
#include "app/platform/linux/NativeGarages.h"
#include "app/platform/linux/RealtimeGameplay.h"

namespace realtime_streaming { struct CpuWorld; }
struct NativeGaragesRuntimeInput {
    std::uint64_t Frame = 0; // unpaused source frame counter, once per update
    bool Replay = false, Coop = false;
};
enum class NativeGaragesRuntimeBarrier { None, GarageUpdate, GarageCamera };
struct NativeGaragesRuntimeFrame {
    std::uint64_t Revision = 0, Generation = 0;
    NativeGarageView View;
    NativeGarageCamera Camera;
    // Real existing follow camera, never a fabricated garage-fixed camera.
    RealtimeGameplayCamera BaselineCamera;
    NativeGaragesRuntimeBarrier Barrier = NativeGaragesRuntimeBarrier::None;
    NativeGarageRequirement Requirement = NativeGarageRequirement::None;
    std::optional<NativeGarageRef> Garage;
    std::size_t UnsupportedUpdates = 0;
};

class NativeGaragesRuntime {
public:
    NativeGaragesRuntime(NativeGarages& garages,const RealtimeGameplay& gameplay) : m_Garages(garages),m_Gameplay(gameplay) {}
    // Pass the ACTIVE committed CpuWorld, after its GPU upload/scene swap, not a
    // worker result still uploading. Parent owns that commit and both lifetimes.
    // Validates the coupled source-COL/placement generation before touching the
    // registry. Tick consumes actual gameplay state; no caller eligibility bool.
    NativeScriptServiceResult Tick(const realtime_streaming::CpuWorld& published, NativeGaragesRuntimeInput input);
    // Validation failures leave this unchanged. Required source camera/type/
    // maintenance work latches Unsupported; later frames cannot skip that work.
    const NativeGaragesRuntimeFrame& Frame() const { return m_Frame; }
    // Pure adapter exposed for input fixtures; production Tick calls this with
    // its bound live gameplay owner. Vehicle render bounds are never substituted.
    static NativeScriptServiceResult MakeView(const RealtimeGameplayState&, const RealtimeGameplayCamera&,
        const NativeGarages&, NativeGaragesRuntimeInput, NativeGarageView& out);
private:
    NativeGarages& m_Garages;
    const RealtimeGameplay& m_Gameplay;
    NativeGaragesRuntimeFrame m_Frame;
    std::shared_ptr<const NativeCollisionSnapshot> m_PublishedCollision;
    std::optional<NativeScriptServiceResult> m_Fault;
};
