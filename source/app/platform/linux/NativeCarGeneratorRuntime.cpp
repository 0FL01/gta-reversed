#include "app/platform/linux/NativeCarGeneratorRuntime.h"
#include "app/platform/linux/NativeScriptSession.h"
#include "app/platform/linux/RealtimeStreaming.h"

#include <algorithm>
#include <atomic>
#include <cmath>

namespace {
std::atomic<std::uint64_t> s_NextOwner{0};

NativeScriptServiceResult Error(std::string message) {
    return {NativeScriptServiceStatus::Error, std::move(message)};
}
NativeScriptServiceResult Unsupported(std::string message) {
    return {NativeScriptServiceStatus::Unsupported, std::move(message)};
}
bool Finite(RealtimeVec3 v) {
    return std::isfinite(v.X) && std::isfinite(v.Y) && std::isfinite(v.Z);
}
std::array<float, 4> Transform(const std::array<float, 16>& matrix, std::array<float, 4> point) {
    std::array<float, 4> result{};
    for (std::size_t row = 0; row < 4; ++row)
        for (std::size_t column = 0; column < 4; ++column)
            result[row] += matrix[column * 4 + row] * point[column];
    return result;
}
bool CameraMatches(const RealtimeHudPriceView& view, const RealtimeGameplayCamera& camera) {
    const auto finite = [](float v) { return std::isfinite(v); };
    if (!std::ranges::all_of(view.ModelView, finite) || !std::ranges::all_of(view.Projection, finite) ||
        !std::isfinite(view.NearClip) || !std::isfinite(view.FarClip) || !std::isfinite(view.Fov) ||
        view.NearClip <= 0 || view.FarClip <= view.NearClip || view.Fov <= 0 || view.Fov >= 180) return false;
    const auto eye = Transform(view.ModelView, {camera.Position.X, camera.Position.Y, camera.Position.Z, 1});
    const auto target = Transform(view.ModelView, {camera.Target.X, camera.Target.Y, camera.Target.Z, 1});
    return std::abs(eye[0]) < .01f && std::abs(eye[1]) < .01f && std::abs(eye[2]) < .01f &&
        std::abs(eye[3] - 1) < .001f && std::abs(target[0]) < .01f && std::abs(target[1]) < .01f && target[2] < 0 &&
        view.Projection[0] > 0 && view.Projection[5] > 0 && view.Projection[11] == -1 && view.Projection[15] == 0;
}
} // namespace

NativeCarGeneratorRuntime::NativeCarGeneratorRuntime(NativeCarGenerators& registry,
    const RealtimeGameplay& gameplay, const NativeVehiclePool& vehicles, const NativeScriptState& script)
    : m_Registry(registry), m_Gameplay(gameplay), m_Vehicles(vehicles), m_Script(script),
      m_Thread(std::this_thread::get_id()), m_Owner(++s_NextOwner), m_Previous(gameplay.State()) {
}

NativeCarGeneratorQueryWitness NativeCarGeneratorRuntime::Observe(const realtime_streaming::CpuWorld& world,
    NativeCarGeneratorRef generator, NativeScriptPosition position, const RealtimeGameplayCamera& camera,
    const std::optional<RealtimeHudPriceView>& projection) {
    NativeCarGeneratorQueryWitness witness;
    witness.Observation.Generator = generator;
    if (!world.SourceCollision || !world.Error.empty() || !Finite({position.X, position.Y, position.Z}) ||
        !Finite(camera.Position)) return witness;
    if (projection && CameraMatches(*projection, camera)) {
        const auto clip = Transform(projection->Projection,
            Transform(projection->ModelView, {position.X, position.Y, position.Z, 1}));
        if (std::ranges::all_of(clip, [](float v) { return std::isfinite(v); })) {
            witness.FrustumTested = true;
            witness.InFrustum = clip[3] > 0 && std::abs(clip[0]) <= clip[3] &&
                std::abs(clip[1]) <= clip[3] && std::abs(clip[2]) <= clip[3];
            if (!witness.InFrustum) witness.Observation.Visibility = NativeCarGeneratorVisibility::HiddenOrOccluded;
        }
    }
    const float top = position.Z > -100 ? position.Z + 1 : 1000;
    witness.VerticalRayTested = true;
    witness.VerticalRayHit = world.QueryWorld().Raycast({position.X, position.Y, top}, {position.X, position.Y, -1000},
        witness.VerticalHit, &witness.VerticalSource);
    witness.CameraRayTested = true;
    witness.CameraRayHit = world.QueryWorld().Raycast(camera.Position, {position.X, position.Y, position.Z},
        witness.CameraHit, &witness.CameraSource);
    // CheckForBlockage selects vehicles+peds, NOT buildings/objects. The source
    // candidate model's COL and source broadphase ordering are unavailable here.
    // Ground also remains Unknown: the static BVH has no building-only mask or
    // complete-window proof. Ray misses never become invented clear ground.
    return witness;
}

const NativeCarGeneratorRuntimeDemand* NativeCarGeneratorRuntime::ResolveDemand(NativeCarGeneratorDemandId id) const {
    const auto found = std::ranges::find_if(m_Frame.Demands, [&](const auto& demand) { return demand.Id == id; });
    return found == m_Frame.Demands.end() ? nullptr : &*found;
}

NativeScriptServiceResult NativeCarGeneratorRuntime::Tick(const realtime_streaming::CpuWorld& world,
    NativeCarGeneratorRuntimeInput input, std::shared_ptr<const NativeVehiclePoolSnapshot> vehicles) {
    if (std::this_thread::get_id() != m_Thread) return Error("car-generator runtime requires its main-thread owner");
    // A repeat cannot re-run Process (including its timer/wait/quarter effects),
    // consume RNG, replace demand provenance or pretend an absent consumer ran.
    if (!m_Frame.Demands.empty()) return m_Frame.Process.Result;
    if (m_Frame.Revision && input.Frame <= m_Frame.Frame) return Error("duplicate/stale car-generator frame");
    const auto& player = m_Gameplay.State();
    const auto& activity = m_Gameplay.Activity();
    const auto& camera = m_Gameplay.Camera();
    if (!player.Ready || !player.MissionCreated || !player.PlayerOnFootTask ||
        activity.Authority != NativePlayerActivityAuthority::SourceBacked || !activity.Revision)
        return Unsupported("car-generator runtime requires the actual source-backed new-game player activity");
    if (input.Area != 0 || player.InVehicle || player.CarPresent || activity.CoopGame || !activity.GamePlaying)
        return Unsupported("car-generator runtime needs the unported area/player-vehicle/coop/game-state consumer outside exterior on-foot new-game extent");
    if (!Finite(player.PedRoot) || !Finite(camera.Position) || !Finite(camera.Target) ||
        !std::isfinite(player.SimulatedSeconds) || !std::isfinite(input.GenerationDistanceMultiplier) ||
        input.GenerationDistanceMultiplier <= 0 || m_Script.Clock.Hours >= 24)
        return Error("invalid owned car-generator player/camera/clock input");
    if (input.Camera && !CameraMatches(*input.Camera, camera)) return Error("car-generator projection is not the live gameplay world camera");
    if (!world.Error.empty() || world.Scene.meshes.empty() || !world.SourceCollision ||
        world.SourceCollision->Overrides != world.Overrides || world.SourceCollision->Instances.empty() ||
        (!world.QueryWorld().TriangleCount() && !world.QueryWorld().SphereCount() && !world.QueryWorld().BoxCount()))
        return Error("car-generator runtime requires the committed coupled render/source-COL query world");
    if (m_Collision && (world.Generation < m_Frame.WorldGeneration ||
        (world.Generation == m_Frame.WorldGeneration && world.SourceCollision != m_Collision)))
        return Error("stale/reused car-generator world generation");
    if (!vehicles || vehicles->Owner() != m_Vehicles.Owner() || vehicles->Frame() != input.Frame ||
        !vehicles->Generation() || vehicles->Generation() != m_Vehicles.PublicationGeneration() ||
        vehicles->Revision() != m_Vehicles.Revision() || !vehicles->Census().Producers.NativeHostComplete ||
        vehicles->Census().Producers.SourceParityComplete)
        return Error("car-generator runtime requires the current authoritative native vehicle publication");
    if (!vehicles->Census().Producers.Owned[std::size_t(NativeVehicleProducer::CarGenerator)])
        return Unsupported("bind native CarGenerator producer before pool seal/publication");
    if (player.Ticks < m_Previous.Ticks || player.SimulatedSeconds < m_Previous.SimulatedSeconds ||
        (m_Frame.Revision && (activity.Revision < m_Frame.ActivityRevision || m_Script.TimeMs < m_Frame.GameMs)))
        return Error("car-generator player/time owner reset requires a new runtime");

    NativeCarGeneratorRuntimeFrame next;
    next.MotionIntervalSeconds = player.SimulatedSeconds - m_Previous.SimulatedSeconds;
    const std::array<float, 3> displacement{player.PedRoot.X - m_Previous.PedRoot.X,
        player.PedRoot.Y - m_Previous.PedRoot.Y, player.PedRoot.Z - m_Previous.PedRoot.Z};
    if (next.MotionIntervalSeconds > 0) {
        for (std::size_t i = 0; i < displacement.size(); ++i)
            next.MeasuredPlayerSpeed[i] = float(displacement[i] / next.MotionIntervalSeconds / 50.0);
    } else if (std::ranges::any_of(displacement, [](float v) { return v != 0; })) {
        return Error("player root changed without a simulated motion interval");
    } else if (player.Ticks) {
        if (!m_Frame.Revision) return Unsupported("prime car-generator motion at player construction or supply a subsequent real gameplay interval");
        next.MeasuredPlayerSpeed = m_Frame.MeasuredPlayerSpeed;
    }
    next.Revision = m_Frame.Revision + 1; next.Frame = input.Frame;
    next.WorldGeneration = world.Generation; next.ActivityRevision = activity.Revision;
    next.VehicleOwner = vehicles->Owner(); next.VehicleGeneration = vehicles->Generation();
    next.VehicleRevision = vehicles->Revision(); next.FreeVehicleSlots = NativeVehiclePool::Capacity - vehicles->Census().Alive;
    next.GameMs = m_Script.TimeMs; next.ClockHour = m_Script.Clock.Hours; next.Area = input.Area;
    next.PlayerCenter = {player.PedRoot.X, player.PedRoot.Y, player.PedRoot.Z};
    next.Camera = {camera.Position.X, camera.Position.Y, camera.Position.Z};
    for (std::size_t slot = 0; slot < NativeVehiclePool::Capacity; ++slot) {
        const auto* vehicle = vehicles->AtSlot(slot);
        if (vehicle && vehicle->State.CreatedBy == NativeVehicleCreatedBy::Parked) ++next.ParkedCars;
    }
    std::vector<NativeCarGeneratorObservation> observations;
    const auto quarter = (m_Registry.ProcessCounter() + 1) % 4;
    for (std::size_t slot = quarter; slot < NativeCarGenerators::Capacity; slot += 4) {
        const NativeCarGeneratorRef ref{std::int32_t(slot)};
        if (const auto* generator = m_Registry.Resolve(ref)) {
            next.Queries.push_back(Observe(world, ref, generator->Position(), camera, input.Camera));
            observations.push_back(next.Queries.back().Observation);
        }
    }
    NativeCarGeneratorProcessInput process;
    process.TimeMs = next.GameMs; process.ClockHour = next.ClockHour; process.NumParkedCars = next.ParkedCars;
    process.PlayerCenter = next.PlayerCenter; process.Camera = next.Camera; process.PlayerSpeed = next.MeasuredPlayerSpeed;
    process.GenerationDistanceMultiplier = input.GenerationDistanceMultiplier;
    process.CanSeeOutside = true; // validated exterior-only owner above
    process.Vehicles = &m_Vehicles; process.Observations = observations;
    next.Process = m_Registry.Process(process);
    next.RegistryRevision = m_Registry.Revision();
    for (const auto& action : next.Process.Actions) {
        if (action.Result.Status == NativeScriptServiceStatus::Ready) continue;
        NativeCarGeneratorRuntimeDemand demand;
        demand.Id = {m_Owner, ++m_NextDemand}; demand.Frame = next.Frame;
        demand.WorldGeneration = next.WorldGeneration; demand.RegistryRevision = next.RegistryRevision;
        demand.VehicleOwner = next.VehicleOwner; demand.VehicleGeneration = next.VehicleGeneration;
        demand.VehicleRevision = next.VehicleRevision; demand.ActivityRevision = next.ActivityRevision;
        demand.GameMs = next.GameMs; demand.Action = action;
        demand.GeneratorState = *m_Registry.Resolve(action.Generator);
        demand.Collision = world.SourceCollision; demand.Vehicles = vehicles;
        next.Demands.push_back(std::move(demand));
    }
    m_Previous = player; m_Collision = world.SourceCollision; m_Frame = std::move(next);
    return m_Frame.Process.Result;
}
