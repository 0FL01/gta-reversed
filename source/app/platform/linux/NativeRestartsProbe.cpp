// Actual unmodified SCM registrations + source-selector oracles; no lifecycle claim.
#define GL_GLEXT_PROTOTYPES 1
#include "app/platform/linux/RealtimeScriptHost.h"
#include "app/platform/linux/RealtimeHud.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <cmath>
#include <cstdio>
#include <limits>
#include <numbers>
#include <stdexcept>

namespace {
using Kind = NativeRestartKind;
using Status = NativeRestartStatus;
using Service = NativeScriptServiceStatus;
void Require(bool ok, const std::string& why) { if (!ok) throw std::runtime_error(why); }
struct Gl {
    EGLDisplay Display = EGL_NO_DISPLAY;
    EGLSurface Surface = EGL_NO_SURFACE;
    EGLContext Context = EGL_NO_CONTEXT;
    Gl() {
        const auto get = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
        Require(bool(get),"surfaceless EGL entry");
        Display = get(EGL_PLATFORM_SURFACELESS_MESA,EGL_DEFAULT_DISPLAY,nullptr);
        Require(Display != EGL_NO_DISPLAY && eglInitialize(Display,nullptr,nullptr) && eglBindAPI(EGL_OPENGL_API),"EGL init");
        const EGLint attributes[]{EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_BIT,EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_NONE};
        EGLConfig config{}; EGLint count{};
        Require(eglChooseConfig(Display,attributes,&config,1,&count) && count,"EGL config");
        const EGLint dimensions[]{EGL_WIDTH,640,EGL_HEIGHT,448,EGL_NONE};
        Surface = eglCreatePbufferSurface(Display,config,dimensions); Context = eglCreateContext(Display,config,EGL_NO_CONTEXT,nullptr);
        Require(Surface != EGL_NO_SURFACE && Context != EGL_NO_CONTEXT && eglMakeCurrent(Display,Surface,Surface,Context),"EGL current");
        std::printf("real-gl renderer=%s\n",glGetString(GL_RENDERER));
    }
    ~Gl() { eglMakeCurrent(Display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT); eglDestroyContext(Display,Context); eglDestroySurface(Display,Surface); eglTerminate(Display); }
};
void Fixtures() {
    NativeRestarts r; std::string error;
    Require(r.Points(Kind::Hospital).empty() && r.Points(Kind::Police).empty() && r.Policy().FadeAfterDeath && r.Policy().FadeAfterArrest &&
        !r.Policy().OverrideNext && !r.Policy().MissionBase && r.Policy().Hospital.Radius == 0 && r.Policy().Police.Radius == 0,"source initial state");
    NativeRestartQuery q{Kind::Hospital,{90,0,0},0,{},0};
    Require(r.Query(q).Status == Status::Unsupported,"no fabricated initial map authority");
    // Labelled source fixtures: overlapping boxes prove FIRST order, truncation,
    // inclusive bounds, and THEMAP fallback even beyond its own bounding box.
    const std::array<NativeRestartMapZone,2> zones{{{{0.9f,-10,-10},{100.9f,10,10},1},{{0,-10,-10},{110,10,10},2}}};
    Require(r.InitializeMapZones(zones,error),error); r.SealStartup();
    Require(!r.InitializeMapZones(zones,error) && r.LevelAt({0,0,0}) == 1 && r.LevelAt({100,0,0}) == 1 &&
        r.LevelAt({101,0,0}) == 2 && r.LevelAt({5000,0,0}) == 0,"owned source map rules and seal");
    Require(r.Query(q).Status == Status::Unsupported,"empty source selection is typed unsupported");
    for (auto kind : {Kind::Hospital,Kind::Police}) {
        Require(r.Add({{},kind,{101,0,0},-450,0}).Status == Service::Ready &&
            r.Add({{},kind,{40,0,0},725,0}).Status == Service::Ready &&
            r.Add({{},kind,{90,0,0},180,1}).Status == Service::Ready,"fixture owned registration");
        q.Kind = kind; q.CityUnlocked = 0;
        auto s = r.Query(q);
        Require(s.Status == Status::RestartRequired && s.Required->RegistryIndex == 1 && s.Required->Target.HeadingDegrees == 725,
            "source level mismatch 11*6 loses to 50 and locked closest excluded");
        const auto ordinaryRevision = r.Revision();
        Require(r.ConsumeSelection(*s.Required) && r.Revision() == ordinaryRevision,
            "ordinary selection consumes no source flag or lifecycle acknowledgement");
        q.CityUnlocked = 1; s = r.Query(q);
        Require(s.Required && s.Required->RegistryIndex == 2 && s.Required->ResurrectionPosition == NativeScriptPosition{90,0,1} &&
            s.Required->HeadingRadians == std::numbers::pi_v<float> && s.Required->DestinationArea == 0 && s.Required->Effects == 63,"unlock uses same registry; concrete lifecycle effects");
        q.Area.reset(); Require(r.Query(q).Status == Status::Unsupported,"unknown area unsupported");
        q.Area = 1; Require(r.Query(q).Status == Status::Unsupported,"interior needs outside-world conversion");
        q.OutsideWorldPosition = q.Position; Require(r.Query(q).Required->RegistryIndex == 2,"explicit ENEX authority");
        q.Area = 0; q.OutsideWorldPosition = NativeScriptPosition{40,0,0};
        Require(r.Query(q).Required->RegistryIndex == 1,"linked ENEX conversion takes precedence even with exterior area code");
        q.Area = 0; q.OutsideWorldPosition.reset();
        auto policy = r.Policy(); auto& extra = kind == Kind::Hospital ? policy.Hospital : policy.Police;
        extra = {{{93,4,999},-45,999},5};
        Require(r.Configure(policy).Status == Service::Ready && r.Query(q).Required->Origin == NativeRestartOrigin::Registry,"extra radius boundary is strict XY");
        extra.Radius = std::nextafter(5.0f,6.0f); r.Configure(policy);
        Require(r.Query(q).Required->Origin == NativeRestartOrigin::Extra && r.Query(q).Required->Target.Position.Z == 999,"extra ignores Z and unlock");
        policy.OverrideNext = NativeRestartPoint{{-1,-2,-3},-720,999}; policy.MissionBase = NativeScriptPosition{90,0,0}; r.Configure(policy);
        q.CityUnlocked.reset(); q.Area.reset(); s = r.Query(q);
        Require(s.Required && s.Required->Origin == NativeRestartOrigin::Override && s.Required->ConsumeOverride && !s.Required->ConsumeMissionBase,
            "override beats mission and needs no level/area authority");
        Require(r.ConsumeSelection(*s.Required) && !r.ConsumeSelection(*s.Required) && r.Policy().MissionBase && !r.Policy().OverrideNext,"source override one shot, stale acceptance rejected");
        s = r.Query(q); Require(s.Required && s.Required->Origin == NativeRestartOrigin::Extra && s.Required->ConsumeMissionBase,"mission base drives extra query");
        Require(r.ConsumeSelection(*s.Required) && !r.Policy().MissionBase,"source mission base one shot");
        r.Configure({}); q.Area = 0; q.CityUnlocked = 0;
        for (unsigned i = 3; i < NativeRestarts::Capacity; ++i) Require(r.Add({{},kind,{40,0,0},float(i),0}).Status == Service::Ready,"capacity fill");
        const auto revision = r.Revision();
        Require(r.Add({{},kind,{1,2,3},0,0}).Status == Service::Error && r.Revision() == revision && r.Query(q).Required->RegistryIndex == 1,"capacity independent and tie first wins");
        Require(r.Add({{},kind,{0,0,-100},0,0}).Status == Service::Unsupported && r.Revision() == revision,"ground sentinel atomic");
        Require(r.Add({{},kind,{0,0,0},std::numeric_limits<float>::quiet_NaN(),0}).Status == Service::Error && r.Revision() == revision,"nonfinite atomic");
    }
    NativeRestarts three; Require(three.InitializeMapZones({},error),error);
    three.Add({{},Kind::Hospital,{0,0,50},0,0}); three.Add({{},Kind::Hospital,{10,0,0},0,0});
    Require(three.Query({Kind::Hospital,{0,0,0},0,{},0}).Required->RegistryIndex == 1,"distance uses Z, not nearby XY list");
    three.Add({{},Kind::Police,{100000,0,0},0,0});
    Require(three.Query({Kind::Police,{0,0,0},0,{},0}).Status == Status::Unsupported,"source police distance ceiling retained");
    std::printf("labelled-source-fixtures PASS initial/map/area/unlock/3D/6x/ties/override/mission/extra/capacity/atomic\n");
}
void ActualQueries(RealtimeScriptHost& host) {
    const auto revision = host.Restarts().Revision();
    std::size_t selected = 0;
    for (auto kind : {Kind::Hospital,Kind::Police}) {
        const auto points = host.Restarts().Points(kind);
        for (std::size_t index = 0; index < points.size(); ++index) {
            const auto& p = points[index];
            NativeRestartQuery q{kind,p.Position,0,{},999}; // host overwrites spoofed stat
            auto s = host.QueryRestart(q);
            if (float(p.WhenToUse) <= float(host.State().IntStats[61])) {
                Require(s.Required && s.Required->Origin == NativeRestartOrigin::Registry && s.Required->RegistryIndex == index && s.Required->Target == p,"actual SCM registry selects itself when unlocked");
                Require(s.Required->Effects == 63 && s.Required->Target.HeadingDegrees == p.HeadingDegrees,"actual selection retains raw heading and reset work");
                ++selected;
            } else Require(!s.Required || s.Required->RegistryIndex != index,"host owns unlock stat; caller cannot unlock restart");
        }
    }
    Require(selected && host.Restarts().Revision() == revision,"actual selections consume owned records without IO/mutation");
    for (const auto& event : host.Events()) {
        if (event.Opcode != 0x016C && event.Opcode != 0x016D) continue;
        NativeScriptRestartRequest request{event.Id,event.Opcode == 0x016C ? Kind::Hospital : Kind::Police,
            {event.Arguments[0],event.Arguments[1],event.Arguments[2]},event.Arguments[3],event.Index};
        const auto events = host.Events().size();
        Require(host.AddRestart(request).Status == Service::Ready && host.Restarts().Revision() == revision && host.Events().size() == events,"exact journal replay no duplicate");
        ++request.WhenToUse;
        Require(host.AddRestart(request).Status == Service::Error && host.SetCameraBehindPlayer({event.Id}).Status == Service::Error &&
            host.CreateCoordinateBlip({event.Id,request.Position,63}).Result.Status == Service::Error && host.Restarts().Revision() == revision,"restart IDs exclude aliases and other services");
    }
    NativeScriptRestartRequest bad{{99991,1,1},Kind::Hospital,{0,0,-100},0,0};
    auto count = host.Events().size();
    Require(host.AddRestart(bad).Status == Service::Unsupported && host.Events().size() == count && host.Restarts().Revision() == revision,"failed host request has no journal/effect");
    bad.Id = host.Events().front().Id; bad.Position.Z = 1;
    Require(host.AddRestart(bad).Status == Service::Error && host.Events().size() == count,"world ID excludes restart");
    for (const auto& blip : host.Entities().Blips()) {
        if (!blip.Active) continue;
        // Actual source blip journal IDs are exercised by VM in the same host;
        // explicit source service ID fixture verifies the reverse exclusion.
        NativeScriptCoordinateBlipRequest c{{99992,1,1},blip.Position,63};
        Require(host.CreateCoordinateBlip(c).Result.Status == Service::Ready,"labelled real HUD coordinate service");
        bad.Id = c.Id; Require(host.AddRestart(bad).Status == Service::Error && host.Restarts().Revision() == revision,"entity ID excludes restart");
        break;
    }
    std::printf("actual-owned-selection PASS selected=%zu zones=%zu cityUnlocked=%d registryRevision=%llu lifecycleCompleted=0\n",selected,host.Restarts().MapZones().size(),host.State().IntStats[61],(unsigned long long)revision);
}
}
int main(int argc, char** argv) try {
    Require(argc == 2 || (argc == 3 && std::string_view(argv[2]) == "--cpu"),"game directory [--cpu] required");
    std::setvbuf(stdout,nullptr,_IONBF,0); Fixtures(); const bool cpu = argc == 3;
    RealtimeGameplay gameplay; RealtimeScriptHost host(gameplay); RealtimeHud hud;
    E2ELoadInfo info; char message[512]{}; std::string error;
    Require(StreamPager_Init(argv[1],info,message,sizeof(message),{true,300,1200}),message);
    auto collision = NativeCollisionContext::LoadBeforeWorker(argv[1],900,error);
    Require(bool(collision) && host.InitializeBeforeWorker(argv[1],error,collision),error);
    if (!cpu) Require(hud.Load(argv[1],message,sizeof(message)),message);
    Require(host.RunPass(1000).Status == NativeScriptStatus::Waiting && host.State().Commands == 53,"actual main53");
    Require(host.PrepareInitialGarageWorldBeforeWorker(error) && host.AdvanceTime(0,error),error); host.SealStartup();
    if (cpu) {
        const auto terminal = host.RunPass(5000);
        Require(terminal.Status == NativeScriptStatus::Unsupported && terminal.IP == 205876 && terminal.Opcode == 0x0570 && host.Session().Threads()[1].Commands == 539,"CPU-negative539 retained");
        std::printf("NativeRestartsProbe CPU PASS main=53 mission=539 terminal=0570@205876\n");
    } else {
        Gl gl; Require(hud.Upload(message,sizeof(message)),message);
        host.SetRadarSpriteReady([&hud](int sprite) { return hud.IsRadarSpriteUploaded(sprite); });
        NativeScriptResult terminal; std::size_t registrations = 0;
        for (unsigned i = 0; i < 10000; ++i) {
            terminal = host.RunPass(1); const auto& t = host.Session().Threads()[1];
            if (terminal.Status != NativeScriptStatus::BudgetYield) break;
            if (t.LastOpcode != 0x016C && t.LastOpcode != 0x016D) continue;
            const auto kind = t.LastOpcode == 0x016C ? Kind::Hospital : Kind::Police;
            const auto& p = host.Restarts().Points(kind).back(); ++registrations;
            Require(host.Events().back().Opcode == t.LastOpcode && host.Events().back().Id.IP == t.LastInstructionIP,"actual VM service journal IP");
            std::printf("actual%04X commands=%llu ip=%u next=%u xyz=%.9g,%.9g,%.9g heading=%.9g when=%d count=%zu\n",t.LastOpcode,(unsigned long long)t.Commands,t.LastInstructionIP,t.IP,p.Position.X,p.Position.Y,p.Position.Z,p.HeadingDegrees,p.WhenToUse,host.Restarts().Points(kind).size());
        }
        const auto stopped = host.Session().Threads()[1]; const auto revision = host.Restarts().Revision(); const auto events = host.Events().size();
        std::printf("strict-terminal status=%d main=%llu mission=%llu ip=%u opcode=%04X hospitals=%zu police=%zu message=%s\n",int(terminal.Status),(unsigned long long)host.State().Commands,(unsigned long long)stopped.Commands,terminal.IP,terminal.Opcode,host.Restarts().Points(Kind::Hospital).size(),host.Restarts().Points(Kind::Police).size(),terminal.Message.c_str());
        std::printf("preceding-instruction opcode=%04X ip=%u write-sequence=%llu variable=%u value=%d\n",stopped.LastOpcode,stopped.LastInstructionIP,(unsigned long long)stopped.LastOutputWrite.Sequence,stopped.LastOutputWrite.Variable,stopped.LastOutputWrite.Value);
        Require(registrations == 15 && host.Restarts().Points(Kind::Hospital).size() == 8 && host.Restarts().Points(Kind::Police).size() == 7 &&
            terminal.Status == NativeScriptStatus::Unsupported && terminal.Opcode == 0x0814 && terminal.IP == 212669 && stopped.Commands == 1234 &&
            stopped.LastOpcode == 0x016D && stopped.LastInstructionIP == 212645 && stopped.LastOutputWrite.Sequence == 1128 &&
            stopped.LastOutputWrite.Variable == 10624 && stopped.LastOutputWrite.Value == 0,"measured exact source restart frontier; next ADD_STUNT_JUMP stays strict");
        Require(host.RunPass(1000).IP == terminal.IP && host.Session().Threads()[1] == stopped && host.Restarts().Revision() == revision && host.Events().size() == events,"terminal repeat fully atomic");
        ActualQueries(host);
        std::printf("NativeRestartsProbe GL PASS registrations=%zu fullBoot=0 deathArrestLifecycle=0\n",registrations);
        host.SetRadarSpriteReady({}); hud.ReleaseGpu();
    }
    StreamPager_Shutdown(); return 0;
} catch (const std::exception& e) { std::fprintf(stderr,"NativeRestartsProbe FAIL %s\n",e.what()); return 2; }
