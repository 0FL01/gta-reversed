// Real main.scm and actual resident world/base MODEL_PLAYER integration probe.
// No fabricated services, game data writes, GL context, or outfit initialization.
#include "app/platform/linux/RealtimeScriptHost.h"
#include "app/platform/linux/IfpAnim.h"
#include "app/platform/linux/ColLoad.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <numbers>

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
        const auto missionBeforeFault = host.Session().Threads()[1];
        const auto fault = host.RunPass(1000);
        Check(fault.Status == NativeScriptStatus::Unsupported && fault.ThreadIndex == 1 && fault.IP == 200868 &&
            fault.Opcode == 0x0517 && fault.Executed == 0 && host.State() == beforeFault &&
            host.Session().Threads()[1] == missionBeforeFault, "real mission0 faults at 0517 unadvanced before main resumes");
        const auto repeated = host.RunPass(1000);
        Check(repeated.Status == NativeScriptStatus::Unsupported && repeated.Opcode == 0x0517 && repeated.IP == 200868 &&
            repeated.Executed == 0 && host.State() == beforeFault && host.Session().Threads()[1] == missionBeforeFault,
            "unsupported mission remains terminal with unchanged main and mission states");
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
        Check(host.LoadScene(liveRequest).Status == NativeScriptServiceStatus::Ready && callbacks == 1 &&
            host.WorldRevision() == revision + 1, "ready world request replay neither reloads nor republishes");
        game.Tick(1.0/60, {}, *host.World());
        Check(game.State().Ticks == 1 && !game.State().CarPresent && game.Actors().stats.triangles == 2,
            "persistent entity/pose keeps ticking without startup preview vehicle");
        std::printf("host-probe failures=%d firstpass=53 mission-prefix=117 terminal=0517@200868 mission0-complete=0\n", s_Failures);
    }
    StreamPager_Shutdown();
    return s_Failures ? 1 : 0;
}
