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
#include <stdexcept>
#include <rw.h>

void RealtimeScriptHostGpuPrepare(const char* dir);
void RealtimeScriptHostGpuProbe(NativeScriptEntities& entities, RealtimeScriptHost& host);
void NativeEntryExitsProbe(RealtimeScriptHost& host);
int NativePickupsProbe(const char* dir, std::uint64_t& commands, std::uint16_t& opcode, std::uint32_t& ip);

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
    // Separate fresh host: run the actual continuation before legacy probes
    // deliberately exercise pool releases, camera/world replacements and mocks.
    std::uint64_t pickupCommands{}; std::uint16_t pickupOpcode{}; std::uint32_t pickupIP{};
    s_Failures += NativePickupsProbe(dir, pickupCommands, pickupOpcode, pickupIP);
    {
        RealtimeGameplay game;
        RealtimeScriptHost host(game);
        Require(host.InitializeBeforeWorker(dir, error), error);
        const auto* pagerDict = rw::TexDictionary::getCurrent();
        NativeScriptEntities small(1, 1);
        Require(small.LoadBeforeWorker(dir, error), error);
        NativeScriptEntities saleFixture(1,1);
        Require(saleFixture.LoadBeforeWorker(dir,error),error);
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
        const auto properties = host.RunPass(9);
        Check(properties.Status == NativeScriptStatus::BudgetYield && properties.Executed == 9 && properties.IP == 201006 &&
            host.Session().Threads()[1].Commands == 126 && host.EntryExits().Revision() == 0, "real property group ends before first ENEX write");
        struct EnexStep { std::uint32_t IP, Next; std::uint16_t Opcode; };
        const std::array<EnexStep,6> suffix{{{201006,201024,0x09B4},{201024,201034,0x0005},{201034,201044,0x0005},
            {201044,201054,0x0005},{201054,201072,0x09B4},{201072,201080,0x0004}}};
        for (std::size_t i=0;i<suffix.size();++i) {
            const auto step=host.RunPass(1); const auto& thread=host.Session().Threads()[1]; const auto expected=suffix[i];
            Check(step.Status==NativeScriptStatus::BudgetYield && step.Executed==1 && thread.Commands==127+i &&
                thread.LastInstructionIP==expected.IP && thread.LastOpcode==expected.Opcode && thread.IP==expected.Next,
                "actual typed six-instruction suffix commits exact source IP/opcode/count");
            std::printf("enex actual-step command=%llu ip=%u opcode=%04X next=%u registryRevision=%llu\n",
                static_cast<unsigned long long>(thread.Commands),thread.LastInstructionIP,thread.LastOpcode,thread.IP,
                static_cast<unsigned long long>(host.EntryExits().Revision()));
        }
        Check(host.Session().Threads()[1].LastOutputWrite.Sequence == 125 && host.Session().Threads()[1].LastOutputWrite.Variable == 6624,
            "price assignment committed before sale creation");
        const std::array<EnexStep,3> saleSuffix{{{201080,201106,0x0518},{201106,201122,0x0570},{201122,201129,0x018B}}};
        for (std::size_t i=0;i<saleSuffix.size();++i) {
            const auto step=host.RunPass(1); const auto& thread=host.Session().Threads()[1]; const auto expected=saleSuffix[i];
            Check(step.Status==NativeScriptStatus::BudgetYield && step.Executed==1 && thread.Commands==133+i &&
                thread.LastInstructionIP==expected.IP && thread.LastOpcode==expected.Opcode && thread.IP==expected.Next,
                "actual sale/radar suffix commits exact typed source IP/opcode/count");
            std::printf("sale actual-step command=%llu ip=%u opcode=%04X next=%u entityRevision=%llu\n",
                static_cast<unsigned long long>(thread.Commands),thread.LastInstructionIP,thread.LastOpcode,thread.IP,
                static_cast<unsigned long long>(host.Entities().Revision()));
        }
        Check(host.State() == beforeFault &&
            host.Session().Threads()[1].Commands == 135 && host.Session().Threads()[1].LastOutputWrite.Sequence == 127 &&
            host.Session().Threads()[1].LastOutputWrite.IP == 201106 && host.Session().Threads()[1].LastOutputWrite.Variable == 6496 &&
            host.Session().Threads()[1].IP == 201129,
            "legacy property/ENEX regression host pauses by quota135; separate clean host proves pickup continuation");
        auto& entities = host.Entities();
        NativeEntryExitsProbe(host);
        Check(entities.Revision() == 12, "exactly four actual pickup allocations, four blips and four display writes");
        std::int32_t saleRef=-1,saleBlip=-1,price=0;
        Require(host.Session().ReadGlobal(6752,saleRef) && host.Session().ReadGlobal(6496,saleBlip) && host.Session().ReadGlobal(6624,price), "actual sale globals");
        const auto* sale=entities.ResolvePickup({saleRef}); const auto* green=entities.ResolveBlip({saleBlip});
        Require(sale && green,"actual sale outputs resolve");
        Check(saleRef==65539 && saleBlip==65539 && sale->Type==18 && sale->Model==1273 && sale->Price==price && price==30000 &&
            sale->CostValue==6000 && green->Position==sale->AuthoredPosition && green->Sprite==31 && green->Display==2 &&
            sale->Actor.stats.triangles>0 && std::string(sale->Actor.stats.dffName)=="property_fsale.dff" &&
            std::string(sale->Actor.stats.txdName)=="icons3.txd" && sale->Message.find("TAB")!=std::string::npos,
            "real0518 owns type18 model1273/icons3 price/control-GXT and green31 radar");
        Check(host.PlayerInfo().Money==0 && host.PlayerInfo().DisplayMoney==0 && host.State().OnAMissionFlag==0,
            "source new player money/display zero; mission thread does not imply declared mission flag");
        const NativeScriptForSalePropertyRequest actualSale{{events.front().Id.Session,186,201080},sale->AuthoredPosition,price,sale->Text};
        const auto saleRevision=entities.Revision();
        Check(host.CreateForSaleProperty(actualSale).Reference.Value==saleRef && entities.Revision()==saleRevision,
            "actual0518 host identity replay retains same drawable/ref once");
        auto wrongSale=actualSale; ++wrongSale.Price;
        Check(host.CreateForSaleProperty(wrongSale).Result.Status==NativeScriptServiceStatus::Error && entities.Revision()==saleRevision && sale->Price==price,
            "same0518 identity with changed price rejected atomically");
        Check(host.CreateLockedProperty({actualSale.Id,sale->AuthoredPosition,sale->Text}).Result.Status==NativeScriptServiceStatus::Error &&
            host.SetCameraBehindPlayer({actualSale.Id}).Status==NativeScriptServiceStatus::Error && entities.Revision()==saleRevision,
            "sale identity shared across locked and host world/player services");
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
        RealtimeScriptHostGpuProbe(entities, host);
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
        host.SetLiveWorldLoader([&](const NativeScriptSceneRequest& request, const NativeScriptServiceTicket&,
            RealtimeScriptWorldPublication& output) {
            ++callbacks;
            if (request.Position != resident.Center) return NativeScriptAsyncPrepareResult{NativeScriptAsyncPrepareStatus::Error, "not resident"};
            output = resident;
            return NativeScriptAsyncPrepareResult{NativeScriptAsyncPrepareStatus::Prepared, {}};
        }, [](const NativeScriptServiceTicket&) {});
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
        std::vector<NativeScriptServiceTicket> workerTickets;
        unsigned cancellations = 0;
        NativeScriptServiceTicket cancelledTicket;
        host.SetLiveWorldLoader([&](const NativeScriptSceneRequest&, const NativeScriptServiceTicket& ticket,
            RealtimeScriptWorldPublication& publication) {
            workerTickets.push_back(ticket);
            ++polls; publication = resident; // even a populated Pending result must not publish
            return NativeScriptAsyncPrepareResult{workerReady ? NativeScriptAsyncPrepareStatus::Prepared : NativeScriptAsyncPrepareStatus::Pending, {}};
        }, [&](const NativeScriptServiceTicket& ticket) { ++cancellations; cancelledTicket = ticket; });
        const auto stableRevision = host.WorldRevision(), entityRevision = entities.Revision();
        const auto transactionAttempts = host.WorldTransaction().Attempts;
        const auto transactionCommits = host.WorldTransaction().Commits;
        const NativeScriptSceneRequest deferred{{999, 8, 8}, resident.Center};
        Check(host.LoadScene(deferred).Status == NativeScriptServiceStatus::Pending &&
            host.LoadScene(deferred).Status == NativeScriptServiceStatus::Pending && polls == 2 && host.WorldRevision() == stableRevision &&
            host.Publication().Scene == resident.Scene, "repeated real-host Pending never publishes callback payload or duplicates world effect");
        auto moved = deferred; ++moved.Position.Z;
        Check(host.LoadScene(moved).Status == NativeScriptServiceStatus::Error && polls == 2 && host.WorldRevision() == stableRevision,
            "pending service ID cannot change source position between polls");
        Check(host.CreateLockedProperty({deferred.Id, property.AuthoredPosition, property.Text}).Result.Status == NativeScriptServiceStatus::Error &&
            entities.Revision() == entityRevision, "pending world identity cannot allocate a property actor");
        const auto enexRevision = host.EntryExits().Revision();
        const auto entrance = host.EntryExits().Entries()[46].Center;
        Check(host.SetEntryExitFlag({deferred.Id,entrance.X,entrance.Y,1,0x4000,1}).Status == NativeScriptServiceStatus::Error &&
            host.EntryExits().Revision() == enexRevision, "pending world identity cannot mutate ENEX registry");
        const auto garageRevision=host.Garages().Revision(); const auto garageName=host.Garages().Entries()[13].Name;
        Check(host.DeactivateGarage({deferred.Id,garageName}).Status==NativeScriptServiceStatus::Error &&
            host.Garages().Revision()==garageRevision && !(host.Garages().Entries()[13].Flags&2),
            "pending world identity cannot mutate registered garage or publish a journal effect");
        Check(host.CancelPendingWorld(deferred.Id,error) && host.CancelPendingWorld(deferred.Id,error) &&
            cancellations==1 && cancelledTicket==workerTickets[0] &&
            host.WorldTransaction().Phase==NativeScriptServiceTransactionPhase::Cancelling,
            "pending world cancel notifies one exact attempt and is idempotent");
        Check(host.LoadScene(deferred).Status==NativeScriptServiceStatus::Pending && polls==2 &&
            host.WorldRevision()==stableRevision,"cancelling world never polls or publishes fake readiness");
        auto staleTicket=cancelledTicket; ++staleTicket.Attempt;
        Check(!host.AcknowledgeWorldCancellation(staleTicket,error) &&
            host.AcknowledgeWorldCancellation(cancelledTicket,error),
            "only the cancelled attempt can acknowledge worker retirement");
        workerReady = true;
        Check(host.LoadScene(deferred).Status == NativeScriptServiceStatus::Ready && host.LoadScene(deferred).Status == NativeScriptServiceStatus::Ready &&
            polls == 3 && workerTickets[2].Owner==cancelledTicket.Owner && workerTickets[2].Attempt==cancelledTicket.Attempt+1 &&
            host.WorldRevision() == stableRevision + 1 && host.WorldTransaction().Attempts==transactionAttempts+2 &&
            host.WorldTransaction().Cancellations==1 && host.WorldTransaction().Commits==transactionCommits+1,
            "cancelled request retries with a new attempt, commits once and replays without worker readiness");
        Check(host.CreateLockedProperty({deferred.Id, property.AuthoredPosition, property.Text}).Result.Status == NativeScriptServiceStatus::Error &&
            entities.Revision() == entityRevision, "completed world identity cannot be reused by entity service");
        const NativeScriptRequestId firstPropertyId{host.Events().front().Id.Session, 171, 200868};
        Check(entities.OwnsRequest(firstPropertyId) && host.SetCameraBehindPlayer({firstPropertyId}).Status == NativeScriptServiceStatus::Error,
            "actual property service identity cannot be reused by camera service");
        unsigned discardedPrepared = 0;
        const auto transactionFailures = host.WorldTransaction().Failures;
        host.SetLiveWorldLoader([&](const NativeScriptSceneRequest&, const NativeScriptServiceTicket&,
            RealtimeScriptWorldPublication& publication) {
            publication = resident; publication.Center.Z += 1;
            return NativeScriptAsyncPrepareResult{NativeScriptAsyncPrepareStatus::Prepared, {}};
        }, [&](const NativeScriptServiceTicket&) { ++discardedPrepared; });
        Check(host.LoadScene({{999, 9, 9}, resident.Center}).Status == NativeScriptServiceStatus::Error && host.WorldRevision() == stableRevision + 1 &&
            host.Publication().Scene == resident.Scene && discardedPrepared==1 &&
            host.WorldTransaction().Phase==NativeScriptServiceTransactionPhase::Idle &&
            host.WorldTransaction().Failures==transactionFailures+1,
            "invalid Prepared world is retired as failed and never exposed as Ready");
        host.SetLiveWorldLoader([&](const NativeScriptSceneRequest&, const NativeScriptServiceTicket&,
            RealtimeScriptWorldPublication&) -> NativeScriptAsyncPrepareResult {
            throw std::runtime_error("fixture worker failure");
        }, [&](const NativeScriptServiceTicket&) { ++discardedPrepared; });
        Check(host.LoadScene({{999, 10, 10}, resident.Center}).Status==NativeScriptServiceStatus::Error &&
            host.WorldRevision()==stableRevision+1 && discardedPrepared==2 &&
            host.WorldTransaction().Phase==NativeScriptServiceTransactionPhase::Idle &&
            host.WorldTransaction().Failures==transactionFailures+2,
            "worker exception retires the exact attempt without fake publication");
        // Real assets / production consumer with explicitly generated frame and
        // balance fixtures. These are not a claim that SCM awarded player cash.
        const NativeScriptForSalePropertyRequest fixtureRequest{{902,1,1},{0,0,2},30004,sale->Text};
        const auto fixtureResult=saleFixture.CreateForSaleProperty(fixtureRequest);
        Require(fixtureResult.Result.Status==NativeScriptServiceStatus::Ready,fixtureResult.Result.Message);
        const auto* fixture=saleFixture.ResolvePickup(fixtureResult.Reference);
        Require(fixture,"source sale fixture allocation");
        const auto fixtureRevision=saleFixture.Revision();
        std::uint32_t fixtureTime=100,frame=0;
        NativeScriptPropertyInput input; input.CollectJustDown=true;
        const auto publish=[&](bool alive=true,bool car=false,NativeScriptPosition ped={0,0,2},NativeScriptPosition camera={0,0,2}) {
            input.FrameCounter=frame; saleFixture.Tick(ped,camera,alive,car,input);
            Require(saleFixture.AdvanceTime(fixtureTime++,error),error); frame+=6;
        };
        const auto none=[&] { return saleFixture.Interaction().Status==NativeScriptPropertyInteractionStatus::None; };
        publish(false); Check(none() && !saleFixture.HelpRevision(),"sale TEST-INPUT dead ped cannot prompt/collect");
        publish(true,true); Check(none() && !saleFixture.HelpRevision(),"sale in-vehicle cannot prompt/collect");
        publish(true,false,{1.35f,0,2}); Check(none() && !saleFixture.HelpRevision(),"sale XY squared >=1.8 cannot prompt/collect");
        publish(true,false,{0,0,4}); Check(none() && !saleFixture.HelpRevision(),"sale strict absZ<2 boundary");
        publish(true,false,{0,0,2},{100,0,2}); Check(none() && saleFixture.Actors().meshes.empty(),"source camera XY100 excludes sale render/collect");
        for(auto* gate : {&input.Busy,&input.Replay,&input.Cutscene,&input.Widescreen}) {
            *gate=true; publish(); Check(none() && !saleFixture.HelpRevision(),"sale global/task/target gate blocks prompt or collection"); *gate=false;
        }
        // Source targeting permits proximity help but cancels collect; verify it
        // separately below after a normal no-key prompt has become displayed.
        input.CollectJustDown=false; input.Targeting=true; publish(); input.Targeting=false;
        Check(none() && saleFixture.HelpMessage()==fixture->Message && !fixture->HelpMessageDisplayed,
            "type18 proximity displays localized control prompt without locked once-only latch");
        const auto promptRevision=saleFixture.HelpRevision();
        publish(); Check(saleFixture.HelpRevision()==promptRevision,"displayed sale help not reset by repeated proximity");
        input.CollectJustDown=true; input.Money=30003; publish();
        Check(saleFixture.Interaction().Status==NativeScriptPropertyInteractionStatus::InsufficientFunds &&
            saleFixture.Interaction().Price==30004 && saleFixture.Interaction().Balance==30003 &&
            !saleFixture.HelpMessage().empty() && saleFixture.HelpMessage()!=fixture->Message,
            "sale source insufficient cash yields localized quick denial, compares unquantized signed ammo");
        const auto denial=saleFixture.HelpMessage();
        input.OnMission=true; input.Money=30004; publish();
        Check(saleFixture.Interaction().Status==NativeScriptPropertyInteractionStatus::OnMission && saleFixture.HelpMessage()!=denial,
            "declared source mission flag takes precedence over sufficient cash with distinct GXT denial");
        input.OnMission=false; publish();
        Check(saleFixture.Interaction().Status==NativeScriptPropertyInteractionStatus::ScriptPurchaseRequired &&
            saleFixture.Interaction().Pickup.Value==fixtureResult.Reference.Value && saleFixture.HelpPresentation().Text.empty() &&
            saleFixture.ResolvePickup(fixtureResult.Reference)==fixture && fixture->Active && saleFixture.Revision()==fixtureRevision && input.Money==30004,
            "funded TEST-INPUT emits explicit script-purchase barrier, clears help, no debit/removal/collected event");
        const auto funded=saleFixture.Interaction(); const auto fundedHelpRevision=saleFixture.HelpRevision();
        Require(saleFixture.AdvanceTime(fixtureTime-1,error),error);
        Check(saleFixture.Interaction().FrameCounter==funded.FrameCounter && saleFixture.HelpRevision()==fundedHelpRevision,
            "duplicate frame/time does not repeat funded interaction or help reset");
        const auto stableVertices=saleFixture.Actors().meshes[0].pos;
        auto changedInput=input; changedInput.FrameCounter=funded.FrameCounter; changedInput.Money=0;
        saleFixture.Tick({0,0,2},{0,0,2},true,false,changedInput);
        Check(!saleFixture.AdvanceTime(fixtureTime,error) && saleFixture.Interaction().Status==funded.Status &&
            saleFixture.Actors().meshes[0].pos==stableVertices && saleFixture.HelpRevision()==fundedHelpRevision,
            "same collect frame changed cash rejects whole actors/help/buffer/interaction publication");
        const auto replayBuffer=saleFixture.CollectBuffer(); input.Replay=true; publish(); input.Replay=false;
        Check(none() && saleFixture.CollectBuffer()==replayBuffer,"replay clears current interaction and freezes source collect buffer");
        input.CollectJustDown=false; input.Targeting=true; publish();
        Check(none() && !saleFixture.CollectBuffer(),"source targeting clears previously buffered collect");
        input.Targeting=false; input.ControlsDisabled=true; input.CollectJustDown=true; publish();
        Check(none() && !saleFixture.CollectBuffer(),"disabled controls cannot create a collect key edge");
        input.ControlsDisabled=false; input.HelpBlocked=true; input.Money=0;
        const auto blockedRevision=saleFixture.HelpRevision(); publish();
        Check(saleFixture.Interaction().Status==NativeScriptPropertyInteractionStatus::InsufficientFunds && saleFixture.HelpRevision()==blockedRevision,
            "source HUD suppression does not alter cash eligibility or fabricate a help publication");
        input.HelpBlocked=false; input.CollectJustDown=true; frame=192; // slot0 visibility/update cadence starts together
        publish(); input.CollectJustDown=false;
        for(std::uint32_t offset=1;offset<=6;++offset) {
            frame=192+offset; publish();
            Check(saleFixture.CollectBuffer()==6-offset && none(),"six-frame source collect buffer decays once per frame; nonselected pool slice never purchases");
        }
        Check(saleFixture.PriceLabels().size()==1 && saleFixture.PriceLabels()[0].Price==30000 && saleFixture.PriceLabels()[0].Alpha==255,
            "source object cost is uint16(ammo/5), price label rounds to five independent of purchase comparison");
        auto badSale=fixtureRequest; badSale.Id.Instruction++; badSale.Position.Z=-100;
        Check(saleFixture.CreateForSaleProperty(badSale).Result.Status==NativeScriptServiceStatus::Unsupported && saleFixture.Revision()==fixtureRevision,
            "ground sentinel is explicit unsupported dependency without collision ownership, no invented altitude");
        badSale.Position.Z=2;
        Check(saleFixture.CreateForSaleProperty(badSale).Result.Status==NativeScriptServiceStatus::Error && saleFixture.Revision()==fixtureRevision,
            "sale pool capacity failure preserves existing live asset actor");
        Require(saleFixture.RemovePickup(fixtureResult.Reference),"fixture release");
        Check(saleFixture.PriceLabels().empty() && none(),"released sale lifetime cannot leave a stale price-label or interaction reference");
        badSale.Price=-1; const auto wrapped=saleFixture.CreateForSaleProperty(badSale);
        Require(wrapped.Result.Status==NativeScriptServiceStatus::Ready,wrapped.Result.Message);
        Check(wrapped.Reference.Value==fixtureResult.Reference.Value+65536 && saleFixture.ResolvePickup(wrapped.Reference)->CostValue==13107 &&
            !saleFixture.ResolvePickup(fixtureResult.Reference) && saleFixture.CreateForSaleProperty(fixtureRequest).Result.Status==NativeScriptServiceStatus::Error,
            "signed ammo bit pattern uses unsigned object cost then16bit truncation; versioned release/replay remains real");
        const auto root=game.State().PedRoot;
        const auto hostFixture=host.CreateForSaleProperty({{903,1,1},{root.X,root.Y,root.Z},1,sale->Text});
        Require(hostFixture.Result.Status==NativeScriptServiceStatus::Ready,hostFixture.Result.Message);
        NativeScriptPropertyInput liveInput; liveInput.FrameCounter=24; liveInput.Money=2147483647; liveInput.OnMission=true; liveInput.CollectJustDown=true;
        Require(host.TickProperties({root.X,root.Y,root.Z},true,liveInput,error) && host.Entities().AdvanceTime(32,error),error);
        Check(host.Entities().Interaction().Status==NativeScriptPropertyInteractionStatus::InsufficientFunds &&
            host.Entities().Interaction().Balance==0 && host.PlayerInfo().Money==0 && host.PlayerInfo().DisplayMoney==0,
            "live host overwrites supplied TEST cash/mission with actual owned zero player balance and undeclared SCM flag");
        Check(rw::TexDictionary::getCurrent()==pagerDict && rw::Engine::state==rw::Engine::Started,
            "sealed creation/render/proximity/collect use owned data and preserve engine context");
        game.Tick(1.0/60, {}, *host.World());
        Check(game.State().Ticks == 1 && !game.State().CarPresent && game.Actors().stats.triangles == 2,
            "persistent entity/pose keeps ticking without startup preview vehicle");
        std::printf("host-probe failures=%d firstpass=53 mission-prefix=%llu terminal=%04X@%u mission0-complete=0\n", s_Failures,
            static_cast<unsigned long long>(pickupCommands), pickupOpcode, pickupIP);
    }
    StreamPager_Shutdown();
    return s_Failures ? 1 : 0;
}
