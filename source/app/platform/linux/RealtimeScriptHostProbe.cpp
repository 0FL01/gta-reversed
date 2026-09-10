// Real main.scm and actual resident world/base MODEL_PLAYER integration probe.
// No fabricated services, game data writes, GL context, or outfit initialization.
#include "app/platform/linux/RealtimeScriptHost.h"
#include "app/platform/linux/IfpAnim.h"
#include "app/platform/linux/ColLoad.h"
#include <cmath>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <numbers>
#include <rw.h>

void RealtimeScriptHostGpuPrepare(const char* dir);
void RealtimeScriptHostGpuProbe(NativeScriptEntities& entities);

namespace {
int s_Failures = 0;
void Check(bool ok, const char* label) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", label);
    if (!ok) ++s_Failures;
}
void Require(bool ok, const std::string& error) {
    if (!ok) { std::fprintf(stderr, "host probe setup: %s\n", error.c_str()); std::exit(2); }
}
bool Near(float a, float b, float epsilon = 0.0001f) { return std::abs(a - b) < epsilon; }
}
int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const char* dir = argc > 1 ? argv[1] : "/game";
    char err[512]{}; std::string error;
    ColLoadStats col{}; ColProbeHit colHit{};
    Require(ColLoad_Init(dir, col, err, sizeof(err)), err);
    ColLoad_Probe(2488.562255859375, -1666.864501953125, colHit);
    std::printf("source-COL independent text-IPL binding models=%d instances=%d hit=%s primitive=%s z=%.7f (not gameplay BVH)\n",
        col.models, col.instances, colHit.model, colHit.prim, colHit.h);
    ColLoad_Shutdown();
    E2ELoadInfo info{};
    Require(StreamPager_Init(dir, info, err, sizeof(err), {.includeStreamed=true, .radius=300, .maxInstances=1200}), err);
    {
        RealtimeGameplay game;
        RealtimeScriptHost host(game);
        Require(host.InitializeBeforeWorker(dir, error), error);
        const auto* pagerDict = rw::TexDictionary::getCurrent();
        NativeScriptEntities small(1, 1);
        Require(small.LoadBeforeWorker(dir, error), error);
        Check(rw::TexDictionary::getCurrent() == pagerDict && rw::Engine::state == rw::Engine::Started,
            "property preload preserves pager dictionary and borrowed engine lifetime");
        Check(small.PreparedModel().stats.triangles > 0 && !small.PreparedModel().images.empty() &&
            std::string(small.PreparedModel().stats.dffName) == "property_locked.dff" &&
            std::string(small.PreparedModel().stats.txdName) == "icons4.txd", "model1272 IDE binding owns actual DFF triangles and icons4 RGBA");
        const auto& propertyGeometry = small.PropertyGeometry();
        Check(propertyGeometry.ColLibrary == "models/gta3.img:dynamic.col" && propertyGeometry.ColHeaderId == 1272 && Near(propertyGeometry.Scale, 2.2f),
            "actual model1272 source COL identity and 2.2 normalization, independent of rendered bounds");
        std::printf("property-source-col min=%.9f,%.9f,%.9f max=%.9f,%.9f,%.9f scale=%.9f\n",
            propertyGeometry.ColMin[0],propertyGeometry.ColMin[1],propertyGeometry.ColMin[2],
            propertyGeometry.ColMax[0],propertyGeometry.ColMax[1],propertyGeometry.ColMax[2],propertyGeometry.Scale);
        RealtimeScriptHostGpuPrepare(dir); // all HUD parsing before SealStartup
        const auto& model = game.PlayerModelStats();
        std::printf("base model=%s source=%s tries=%d verts=%d tris=%d bones=%d mapped=%d weight=%.8f minZ=%.8f rootZ=%.8f\n",
            model.model, model.src, model.tried, model.verts, model.tris, model.bones, model.mapped,
            model.wsum, model.animMin[2], model.rootWorld[2]);
        Check(std::string(model.src) == "gta3.img:player.dff" && !model.tried && model.verts == 6 &&
            model.tris == 2 && model.bones == 32, "direct base MODEL_PLAYER before clothes opcodes");
        const auto localPose = game.Actors().meshes.front().pos;
        const auto result = host.RunPass(1000);
        std::printf("firstpass status=%d executed=%zu commands=%llu ip=%u fault=%04X message=%s\n",
            int(result.Status), result.Executed, (unsigned long long)host.State().Commands, result.IP, result.Opcode, result.Message.c_str());
        Require(result.Status == NativeScriptStatus::Waiting, result.Message);
        Check(result.Executed == 53 && host.State().Commands == 53 && host.State().IP == 56369,
            "real scheduler first pass commits 53 commands into live host");
        Check(info.binaryInstances > 0 && host.World() && host.World()->TriangleCount() > 0 && host.WorldRevision() == 2,
            "collision request and load-scene both publish actual streamed world BVH");
        const auto& s = game.State();
        float ground = 0;
        const auto scene = host.Publication().Center;
        Check(host.World()->Ground(s.Ped.X, s.Ped.Y, scene.Z + 1, scene.Z - 150, ground), "actual ground hit at authored SCM XY");
        Check(Near(ground, 12.34375f) && host.Publication().Collision.get() == host.World(),
            "source COL ground replaces render ground; publication owns the current collision world");
        std::printf("world instances=%d binary=%d triangles=%zu center=%.7f,%.7f,%.7f ground=%.7f source=%s\n",
            host.Publication().Frame.instances, info.binaryInstances, host.World()->TriangleCount(), scene.X, scene.Y, scene.Z, ground,
            RealtimeScriptHost::CollisionSource);
        std::printf("player feet=%.7f,%.7f,%.7f root=%.7f,%.7f,%.7f current=%.7f aim=%.7f cameraYaw=%.7f actors=%d car=%d\n",
            s.Ped.X, s.Ped.Y, s.Ped.Z, s.PedRoot.X, s.PedRoot.Y, s.PedRoot.Z, s.PedCurrentRotation,
            s.PedAimingRotation, game.Camera().Yaw, game.Actors().stats.triangles, s.CarPresent);
        Check(s.Ready && s.MissionCreated && s.PlayerOnFootTask && !s.InVehicle && !s.CarPresent &&
            game.Actors().stats.triangles == 2, "mission-owned on-foot live player, no preview car published");
        Check(Near(s.Ped.Z, 12.8757f) && Near(scene.Z, 13.3757f) && Near(s.PedRoot.Z, s.Ped.Z + 1),
            "authored feet preserved; source ped1 bbox root offset is 1, scene Z is independently +0.5");
        const float rad = 262 * std::numbers::pi_v<float> / 180;
        Check(Near(s.PedCurrentRotation, rad) && Near(s.PedAimingRotation, rad) &&
            Near(s.PedHeading, rad + std::numbers::pi_v<float>/2), "262 degrees updates source current/aim and +Y entity transform");
        const auto& events = host.Events();
        const std::array<std::uint16_t, 7> expected{0x04E4,0x03CB,0x0053,0x07AF,0x01F5,0x0373,0x0173};
        bool order = events.size() == expected.size();
        for (std::size_t i=0; i<events.size(); ++i) {
            if (i<expected.size()) order &= events[i].Opcode == expected[i];
            std::printf("host-event opcode=%04X ip=%u ref=%d heading=%.7f cameraYaw=%.7f\n",
                events[i].Opcode, events[i].Id.IP, events[i].Reference, events[i].Player.PedCurrentRotation, events[i].Camera.Yaw);
        }
        Check(order, "real host service order exactly collision/scene/player/group/ped/camera/heading");
        Require(order, "cannot inspect unexpected host event order");
        Check(Near(events[5].Player.PedCurrentRotation, 0) && Near(events[5].Camera.Yaw, std::numbers::pi_v<float>/2) &&
            Near(game.Camera().Yaw, events[5].Camera.Yaw) && game.Camera().ScriptDirectlyBehind &&
            Near(game.Camera().ScriptPedOrientation, std::numbers::pi_v<float>/2), "camera-behind captures setup +Y before subsequent heading change");
        bool pose = true;
        const auto& positions = game.Actors().meshes.front().pos;
        for (std::size_t i=0; i<positions.size(); i+=3) {
            pose &= Near(positions[i], s.Ped.X + std::cos(rad)*localPose[i] - std::sin(rad)*localPose[i+1], 0.0005f) &&
                Near(positions[i+1], s.Ped.Y + std::sin(rad)*localPose[i] + std::cos(rad)*localPose[i+1], 0.0005f) &&
                Near(positions[i+2], s.Ped.Z + localPose[i+2], 0.0005f);
        }
        Check(pose, "all actual player vertices follow authored origin and source heading matrix");
        const NativeScriptGroupRef group{events[3].Reference};
        const NativeScriptPedRef ped{events[4].Reference};
        const auto* membership = host.ResolveGroup(group);
        Check(ped.Value == 1 && group.Value == 65536 && host.ResolvePed(ped) == &game && membership &&
            membership->Leader.Value == ped.Value && membership->MissionGroup && !membership->Followers,
            "source-encoded versioned ped/group refs resolve to actual actor and mission group leader");
        Check(!host.ResolvePed({ped.Value ^ 1}) && !host.ResolvePed({ped.Value | 0x80}) &&
            !host.ResolvePed({ped.Value | 0x100}) && !host.ResolveGroup({group.Value ^ 0x10000}) &&
            !host.ResolveGroup({group.Value | 1}), "stale generations, empty bit and out-of-pool slots rejected");
        const auto oldCamera = game.Camera();
        Check(host.SetCameraBehindPlayer({events[5].Id}).Status == NativeScriptServiceStatus::Ready &&
            Near(game.Camera().Yaw, oldCamera.Yaw) && host.Events().size() == 7,
            "replayed camera request does not recapture post-heading orientation");
        Check(host.State().RelationshipRevision == 22 && host.State().StatWrites == 9 && host.State().Clock.Revision &&
            host.State().Fade.Revision && host.State().UnprocessedStatNotifications == 6, "owned relationships, stats, clock and fade exposed without presentation claims");
        Check(host.AdvanceTime(0, error) && host.State().Clock.Hours == 8 && host.State().Clock.Minutes == 0 &&
            host.State().Fade.Direction == 0 && host.State().Fade.DurationSeconds == 0 && host.State().Fade.Alpha == 255,
            "source zero-duration fade processes to black at game time zero, clock 08:00");
        const auto mainBefore = static_cast<const NativeScriptThreadState&>(host.State());
        host.SealStartup();
        const auto prefix = host.RunPass(117);
        Check(prefix.Status == NativeScriptStatus::BudgetYield && prefix.Executed == 117 && prefix.ThreadIndex == 1 &&
            prefix.IP == 200868 && host.Session().Threads()[1].Commands == 117 &&
            host.Session().Threads()[1].LastOutputWrite.Sequence == 115 &&
            host.Session().Threads()[1].LastOutputWrite.Variable == 6360 &&
            host.State().LaRiotsRevision == 1 && !host.State().LaRiotsEnabled,
            "real mission0 commits 117 policy/numeric instructions through source write at 6360");
        Check(static_cast<const NativeScriptThreadState&>(host.State()) == mainBefore && host.Events().size() == 7,
            "mission0 prefix does not resume main or fabricate entity services");
        const auto beforeFault = host.State();
        const auto fault = host.RunPass(1000);
        Check(fault.Status == NativeScriptStatus::Unsupported && fault.ThreadIndex == 1 && fault.IP == 201006 &&
            fault.Opcode == 0x09B4 && fault.Executed == 9 && host.State() == beforeFault &&
            host.Session().Threads()[1].Commands == 126 && host.Session().Threads()[1].LastOutputWrite.Sequence == 121,
            "real mission0 creates three source property/radar groups then faults at 09B4 before main resumes");
        const auto missionBeforeFault = host.Session().Threads()[1];
        const auto repeated = host.RunPass(1000);
        Check(repeated.Status == NativeScriptStatus::Unsupported && repeated.Opcode == 0x09B4 && repeated.IP == 201006 &&
            repeated.Executed == 0 && host.State() == beforeFault && host.Session().Threads()[1] == missionBeforeFault,
            "unsupported mission remains terminal with unchanged main and mission states");
        auto& entities = host.Entities();
        Check(entities.Revision() == 9, "exactly three actual pickup allocations, three blips and three display writes");
        const std::array<std::uint16_t, 3> blipVariables{264, 2108, 220};
        for (std::size_t i = 0; i < 3; ++i) {
            std::int32_t pickupRef = -1, blipRef = -1, x = 0, y = 0, z = 0;
            Check(host.Session().ReadGlobal(6740 + 4*i, pickupRef) && host.Session().ReadGlobal(blipVariables[i], blipRef), "real SCM output handles available");
            const auto* pickup = entities.ResolvePickup({pickupRef}); const auto* blip = entities.ResolveBlip({blipRef});
            Require(pickup && blip, "real generation-bearing property/radar allocations");
            Check(host.Session().ReadGlobal(6096 + 4*i, x) && host.Session().ReadGlobal(6224 + 4*i, y) && host.Session().ReadGlobal(6352 + 4*i, z), "source-authored property globals available");
            const NativeScriptPosition position{std::bit_cast<float>(x), std::bit_cast<float>(y), std::bit_cast<float>(z)};
            Check(pickupRef == int(65536 + i) && blipRef == int(65536 + i) && pickup->AuthoredPosition == position &&
                blip->Position == position && blip->Sprite == 32 && blip->Display == 2 && blip->ShortRange && blip->Contact &&
                pickup->Actor.stats.triangles == entities.PreparedModel().stats.triangles && pickup->Message.size() > 0,
                "source coordinates and GXT mapped into live asset-backed pickup and radar drawable");
            std::printf("property-service slot=%zu pickup=%d blip=%d authored=%.7f,%.7f,%.7f compressed=%.7f,%.7f,%.7f tris=%d sprite=%d display=%d\n",
                i, pickupRef, blipRef, position.X, position.Y, position.Z, pickup->Position.X, pickup->Position.Y, pickup->Position.Z,
                pickup->Actor.stats.triangles, blip->Sprite, blip->Display);
        }
        const auto& property = entities.Pickups()[0];
        auto nearProperty = property.Position;
        std::uint32_t propertyTime = 0;
        const auto publishProperty = [&] { Require(entities.AdvanceTime(propertyTime++, error), error); };
        entities.Tick(nearProperty, nearProperty, false, false);
        publishProperty();
        entities.Tick(nearProperty, nearProperty, true, true);
        publishProperty();
        Check(!entities.HelpRevision(), "dead/in-vehicle proximity cannot trigger locked property help");
        nearProperty.X += 2;
        entities.Tick(nearProperty, nearProperty, true, false);
        publishProperty();
        Check(!entities.HelpRevision(), "outside source XY pickup threshold does not trigger help");
        entities.Tick(property.Position, property.Position, true, false);
        publishProperty();
        Check(entities.HelpRevision() == 1 && entities.HelpMessage() == property.Message && property.HelpMessageDisplayed &&
            entities.ResolvePickup(property.Reference) && entities.Actors().stats.triangles == property.Actor.stats.triangles,
            "living on-foot proximity shows real GXT once and keeps actual visible locked mesh alive");
        entities.Tick(property.Position, property.Position, true, false);
        publishProperty();
        Check(entities.HelpRevision() == 1, "locked pickup neither pays, collects, removes nor repeats help");
        RealtimeScriptHostGpuProbe(entities);
        const auto oldVertices = entities.Actors().meshes[0].pos;
        const auto oldNormals = entities.Actors().meshes[0].nrm;
        const auto oldHelpRevision = entities.HelpRevision();
        const auto& secondProperty = entities.Pickups()[1];
        entities.Tick(secondProperty.Position, secondProperty.Position, true, false);
        Check(entities.Actors().meshes[0].pos == oldVertices && !secondProperty.HelpMessageDisplayed,
            "Tick stages camera/proximity without publishing old-phase geometry or help");
        Check(!entities.AdvanceTime(6000, error) && entities.Actors().meshes[0].pos == oldVertices &&
            entities.Actors().meshes[0].nrm == oldNormals && entities.HelpRevision() == oldHelpRevision &&
            !secondProperty.HelpMessageDisplayed && entities.HelpPresentation().Text.empty(),
            "stale time rejects entire staged mesh/help/latch publication");
        Require(entities.AdvanceTime(10240, error), error); // exact phase zero
        const auto& posed = secondProperty.Actor.meshes[0];
        const auto& source = entities.PreparedModel().meshes[0];
        const auto scale = entities.PropertyGeometry().Scale;
        Check(Near(posed.pos[0],source.pos[0]*scale+secondProperty.Position.X) && Near(posed.pos[1],source.pos[1]*scale+secondProperty.Position.Y) &&
            entities.Actors().meshes[0].pos == posed.pos && secondProperty.HelpMessageDisplayed && entities.HelpRevision() == oldHelpRevision+1,
            "accepted current-phase actual geometry and source help publish together without one-frame latency");
        const auto* publishedVertices = entities.Actors().meshes[0].pos.data();
        Require(entities.AdvanceTime(10240, error), error);
        Check(entities.Actors().meshes[0].pos.data() == publishedVertices, "duplicate time/input does not republish or accumulate geometry");
        auto farCamera = secondProperty.Position; farCamera.Z += 10; farCamera.X += 1000;
        entities.Tick(secondProperty.Position, farCamera, true, false); Require(entities.AdvanceTime(10240, error), error);
        Check(entities.Actors().meshes.empty(), "same-clock camera change publishes source visibility without advancing phase");
        entities.Tick(secondProperty.Position, secondProperty.Position, true, false); Require(entities.AdvanceTime(10752, error), error);
        const auto oneTurnVertices = entities.Actors().meshes[0].pos, oneTurnNormals = entities.Actors().meshes[0].nrm;
        Require(entities.AdvanceTime(12800, error), error);
        Check(entities.Actors().meshes[0].pos == oneTurnVertices && entities.Actors().meshes[0].nrm == oneTurnNormals,
            "actual prepared model repeats exactly after 2048ms without cumulative transform error");
        Require(entities.AdvanceTime(0x7fffffffu, error) && entities.AdvanceTime(0xfffffff0u, error) && entities.AdvanceTime(16, error), error);
        const auto wrappedVertices = entities.Actors().meshes[0].pos;
        const auto directWrap = NativeScriptPropertyActor(entities.PreparedModel(),secondProperty.Position,scale,16);
        Check(wrappedVertices == directWrap.meshes[0].pos && !entities.AdvanceTime(15,error) && entities.Actors().meshes[0].pos == wrappedVertices,
            "uint32 clock rollover uses source phase and subsequent stale time preserves actual vertices");
        const NativeScriptLockedPropertyRequest tinyRequest{{800, 1, 1}, property.AuthoredPosition, property.Text};
        const auto tiny = small.CreateLockedProperty(tinyRequest);
        Require(tiny.Result.Status == NativeScriptServiceStatus::Ready, tiny.Result.Message);
        Check(small.CreateLockedProperty(tinyRequest).Reference.Value == tiny.Reference.Value && small.Revision() == 1, "actual property replay does not duplicate actor allocation");
        auto changed = tinyRequest; ++changed.Id.Instruction;
        Check(small.CreateLockedProperty(changed).Result.Status == NativeScriptServiceStatus::Error && small.Revision() == 1 &&
            small.ResolvePickup(tiny.Reference)->Actor.stats.triangles > 0, "pickup capacity failure preserves old generation and actual geometry");
        auto mismatched = tinyRequest; mismatched.Text[0] = 'X';
        Check(small.CreateLockedProperty(mismatched).Result.Status == NativeScriptServiceStatus::Error && small.Revision() == 1, "same ID with changed GXT rejected atomically");
        Check(small.RemovePickup(tiny.Reference) && !small.ResolvePickup(tiny.Reference), "released actual pickup lifetime invalidates old ref");
        const auto newer = small.CreateLockedProperty(changed);
        Check(newer.Result.Status == NativeScriptServiceStatus::Ready && newer.Reference.Value == tiny.Reference.Value + 65536 &&
            !small.ResolvePickup(tiny.Reference) && small.ResolvePickup(newer.Reference)->Actor.stats.triangles > 0 &&
            small.CreateLockedProperty(tinyRequest).Result.Status == NativeScriptServiceStatus::Error,
            "reused slot publishes new generation WITH actual geometry; old service replay cannot alias it");
        const NativeScriptContactBlipRequest tinyBlipRequest{{800, 3, 3}, property.AuthoredPosition, 32};
        const auto tinyBlip = small.CreateContactBlip(tinyBlipRequest);
        Require(tinyBlip.Result.Status == NativeScriptServiceStatus::Ready, tinyBlip.Result.Message);
        const auto oldRevision = small.Revision();
        auto nextBlip = tinyBlipRequest; ++nextBlip.Id.Instruction;
        Check(small.CreateContactBlip(nextBlip).Result.Status == NativeScriptServiceStatus::Error && small.Revision() == oldRevision &&
            small.CreateContactBlip(tinyBlipRequest).Reference.Value == tinyBlip.Reference.Value, "blip capacity/replay preserve actual drawable state");
        Check(small.SetBlipDisplay({{800, 9, 9}, {tinyBlip.Reference.Value ^ 65536}, 0}).Status == NativeScriptServiceStatus::Error &&
            small.ResolveBlip(tinyBlip.Reference)->Display == 3 && small.Revision() == oldRevision, "stale display reference cannot mutate live radar");
        Check(small.RemoveBlip(tinyBlip.Reference) && !small.ResolveBlip(tinyBlip.Reference), "actual radar release invalidates generation");
        const auto newBlip = small.CreateContactBlip(nextBlip);
        Check(newBlip.Result.Status == NativeScriptServiceStatus::Ready && newBlip.Reference.Value == tinyBlip.Reference.Value + 65536 &&
            small.ResolveBlip(newBlip.Reference) && small.CreateContactBlip(tinyBlipRequest).Result.Status == NativeScriptServiceStatus::Error,
            "reallocated radar drawable carries new generation; no arbitrary zero ref");
        const auto revision = host.WorldRevision();
        const auto sealed = host.RequestCollision({{999, 999, 999}, scene.X, scene.Y});
        Check(sealed.Status == NativeScriptServiceStatus::Unsupported && host.WorldRevision() == revision,
            "sealed startup refuses parser entry without live worker loader");
        Check(host.LoadScene({{999, 999, 1000}, scene}).Status == NativeScriptServiceStatus::Unsupported &&
            host.WorldRevision() == revision, "new sealed LOAD_SCENE explicitly unsupported without worker callback");
        // A real already-resident publication is a valid cache hit after worker
        // startup. Callback only transfers CPU ownership: no parser invocation.
        const auto resident = host.Publication();
        unsigned callbacks = 0;
        host.SetLiveWorldLoader([&](const NativeScriptSceneRequest& request, RealtimeScriptWorldPublication& output) {
            ++callbacks;
            if (request.Position != resident.Center) return NativeScriptServiceResult{NativeScriptServiceStatus::Error, "not resident"};
            output = resident;
            return NativeScriptServiceResult{NativeScriptServiceStatus::Ready, {}};
        }, [](const NativeScriptRequestId&) {});
        const NativeScriptSceneRequest liveRequest{{999, 1000, 1000}, resident.Center};
        Check(host.LoadScene(liveRequest).Status == NativeScriptServiceStatus::Ready && callbacks == 1 &&
            host.WorldRevision() == revision + 1 && host.Publication().Scene == resident.Scene,
            "sealed live-world callback publishes actual resident source triangles without parser entry");
        Check(resident.Collision && resident.Collision->Ground(scene.X, scene.Y, 20, 5, ground) && Near(ground, 12.34375f) &&
            host.Publication().Collision.get() == host.World(), "old publication collision lifetime survives host replacement");
        Check(host.LoadScene(liveRequest).Status == NativeScriptServiceStatus::Ready && callbacks == 1 &&
            host.WorldRevision() == revision + 1, "ready world request replay neither reloads nor republishes");
        bool workerReady = false;
        unsigned polls = 0;
        host.SetLiveWorldLoader([&](const NativeScriptSceneRequest&, RealtimeScriptWorldPublication& publication) {
            ++polls; publication = resident; // even a populated Pending result must not publish
            return NativeScriptServiceResult{workerReady ? NativeScriptServiceStatus::Ready : NativeScriptServiceStatus::Pending, {}};
        }, [](NativeScriptRequestId) {});
        const auto stableRevision = host.WorldRevision(), entityRevision = entities.Revision();
        const NativeScriptSceneRequest deferred{{999, 8, 8}, resident.Center};
        Check(host.LoadScene(deferred).Status == NativeScriptServiceStatus::Pending &&
            host.LoadScene(deferred).Status == NativeScriptServiceStatus::Pending && polls == 2 && host.WorldRevision() == stableRevision &&
            host.Publication().Scene == resident.Scene, "repeated real-host Pending never publishes callback payload or duplicates world effect");
        auto moved = deferred; ++moved.Position.Z;
        Check(host.LoadScene(moved).Status == NativeScriptServiceStatus::Error && polls == 2 && host.WorldRevision() == stableRevision,
            "pending service ID cannot change source position between polls");
        Check(host.CreateLockedProperty({deferred.Id, property.AuthoredPosition, property.Text}).Result.Status == NativeScriptServiceStatus::Error &&
            entities.Revision() == entityRevision, "pending world identity cannot allocate a property actor");
        workerReady = true;
        Check(host.LoadScene(deferred).Status == NativeScriptServiceStatus::Ready && host.LoadScene(deferred).Status == NativeScriptServiceStatus::Ready &&
            polls == 3 && host.WorldRevision() == stableRevision + 1, "Ready publishes once and replay skips completed worker request");
        Check(host.CreateLockedProperty({deferred.Id, property.AuthoredPosition, property.Text}).Result.Status == NativeScriptServiceStatus::Error &&
            entities.Revision() == entityRevision, "completed world identity cannot be reused by entity service");
        const NativeScriptRequestId firstPropertyId{host.Events().front().Id.Session, 171, 200868};
        Check(entities.OwnsRequest(firstPropertyId) && host.SetCameraBehindPlayer({firstPropertyId}).Status == NativeScriptServiceStatus::Error,
            "actual property service identity cannot be reused by camera service");
        host.SetLiveWorldLoader([&](const NativeScriptSceneRequest&, RealtimeScriptWorldPublication& publication) {
            publication = resident; publication.Center.Z += 1;
            return NativeScriptServiceResult{NativeScriptServiceStatus::Ready, {}};
        }, [](NativeScriptRequestId) {});
        Check(host.LoadScene({{999, 9, 9}, resident.Center}).Status == NativeScriptServiceStatus::Error && host.WorldRevision() == stableRevision + 1 &&
            host.Publication().Scene == resident.Scene, "invalid Ready world publication cannot replace resident state");
        game.Tick(1.0/60, {}, *host.World());
        Check(game.State().Ticks == 1 && !game.State().CarPresent && game.Actors().stats.triangles == 2,
            "persistent entity/pose keeps ticking without startup preview vehicle");
        std::printf("host-probe failures=%d firstpass=53 mission-prefix=126 terminal=09B4@201006 mission0-complete=0\n", s_Failures);
    }
    StreamPager_Shutdown();
    return s_Failures ? 1 : 0;
}
