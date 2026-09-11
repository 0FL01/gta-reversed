// Actual unmodified main.scm -> persistent host -> production HUD, surfaceless GL.
#define GL_GLEXT_PROTOTYPES 1
#include "app/platform/linux/RealtimeScriptHost.h"
#include "app/platform/linux/RealtimeHud.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <algorithm>
#include <cstdio>
#include <limits>
#include <stdexcept>

namespace {
void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
struct ProbeGl {
    EGLDisplay Display = EGL_NO_DISPLAY;
    EGLSurface Surface = EGL_NO_SURFACE;
    EGLContext Context = EGL_NO_CONTEXT;
    ProbeGl() {
        const auto getDisplay = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
        Require(bool(getDisplay), "surfaceless EGL entry");
        Display = getDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
        Require(Display != EGL_NO_DISPLAY && eglInitialize(Display, nullptr, nullptr) && eglBindAPI(EGL_OPENGL_API), "EGL init");
        const EGLint attributes[]{EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_BIT,
            EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_NONE};
        EGLConfig config{}; EGLint count{};
        Require(eglChooseConfig(Display, attributes, &config, 1, &count) && count, "EGL config");
        const EGLint dimensions[]{EGL_WIDTH,640,EGL_HEIGHT,448,EGL_NONE};
        Surface = eglCreatePbufferSurface(Display, config, dimensions);
        Context = eglCreateContext(Display, config, EGL_NO_CONTEXT, nullptr);
        Require(Surface != EGL_NO_SURFACE && Context != EGL_NO_CONTEXT &&
            eglMakeCurrent(Display, Surface, Surface, Context), "EGL context");
        std::printf("real-gl renderer=%s\n", glGetString(GL_RENDERER));
    }
    ~ProbeGl() {
        eglMakeCurrent(Display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroyContext(Display, Context); eglDestroySurface(Display, Surface); eglTerminate(Display);
    }
};
std::vector<unsigned char> Draw(const RealtimeHud& hud, const RealtimeHudState& state) {
    glViewport(0,0,640,448); glClearColor(.1f,.12f,.15f,1); glClear(GL_COLOR_BUFFER_BIT);
    hud.Draw({.clock=false}, state, 640,448);
    std::vector<unsigned char> pixels(640*448*4);
    glReadPixels(0,0,640,448,GL_RGBA,GL_UNSIGNED_BYTE,pixels.data());
    Require(glGetError() == GL_NO_ERROR, "actual HUD draw/readback");
    return pixels;
}
void Transactions(RealtimeScriptHost& host, RealtimeHud& hud, NativeScriptEntities& small, const NativeScriptRadarBlip& actual) {
    // Labelled direct service fixtures after the exact real SCM boundary.
    NativeScriptCoordinateBlipRequest r{{99100,1,1}, actual.Position, actual.Sprite};
    const auto before = host.Entities().Revision();
    host.SetRadarSpriteReady({});
    Require(host.CreateCoordinateBlip(r).Result.Status == NativeScriptServiceStatus::Unsupported &&
        host.Entities().Revision() == before && !host.Entities().OwnsRequest(r.Id), "missing consumer is atomic");
    host.SetRadarSpriteReady([](int) -> bool { throw std::runtime_error("labelled callback failure"); });
    Require(host.CreateCoordinateBlip(r).Result.Status == NativeScriptServiceStatus::Error &&
        host.Entities().Revision() == before && !host.Entities().OwnsRequest(r.Id), "throwing consumer is atomic");
    host.SetRadarSpriteReady([&hud](int sprite) { return hud.IsRadarSpriteUploaded(sprite); });
    const auto made = host.CreateCoordinateBlip(r);
    Require(made.Result.Status == NativeScriptServiceStatus::Ready, made.Result.Message);
    const auto revision = host.Entities().Revision();
    host.SetRadarSpriteReady({});
    Require(host.CreateCoordinateBlip(r).Reference.Value == made.Reference.Value && host.Entities().Revision() == revision,
        "exact replay does not depend on later GPU state");
    auto changed = r; ++changed.Sprite;
    NativeScriptCarGeneratorRequest generator;
    generator.Id = r.Id;
    Require(host.CreateCoordinateBlip(changed).Result.Status == NativeScriptServiceStatus::Error &&
        host.CreateContactBlip({r.Id,r.Position,r.Sprite}).Result.Status == NativeScriptServiceStatus::Error &&
        host.SetCameraBehindPlayer({r.Id}).Status == NativeScriptServiceStatus::Error &&
        host.SetEntryExitFlag({r.Id}).Status == NativeScriptServiceStatus::Error &&
        host.CreateCarGenerator(generator).Result.Status == NativeScriptServiceStatus::Error &&
        host.Entities().Revision() == revision, "global journal identity rejects aliases and cross-service reuse");
    for (int display : {0,1,2,3}) {
        Require(host.SetBlipDisplay({{99101,std::uint64_t(display+1),1},made.Reference,display}).Status == NativeScriptServiceStatus::Ready,
            "source BLIP_COORD has no Draw3dMarkers branch even for BOTH/MARKERONLY");
    }
    Require(host.Entities().RemoveBlip(made.Reference) &&
        host.CreateCoordinateBlip(r).Result.Status == NativeScriptServiceStatus::Error, "removed generation cannot replay");
    host.SetRadarSpriteReady([&hud](int sprite) { return hud.IsRadarSpriteUploaded(sprite); });
    r.Id = {99102,1,1};
    const auto replacement = host.CreateCoordinateBlip(r);
    Require(replacement.Result.Status == NativeScriptServiceStatus::Ready && replacement.Reference.Value != made.Reference.Value &&
        (std::uint32_t(replacement.Reference.Value)&0xffff) == (std::uint32_t(made.Reference.Value)&0xffff), "slot reuse changes generation");
    const auto old = host.Entities().Revision();
    r.Id = {99103,1,1}; r.Position.Z = -100;
    Require(host.CreateCoordinateBlip(r).Result.Status == NativeScriptServiceStatus::Unsupported && host.Entities().Revision() == old,
        "source ground sentinel explicitly requires the unported XY world query");
    r.Position.Z = std::numeric_limits<float>::quiet_NaN();
    Require(host.CreateCoordinateBlip(r).Result.Status == NativeScriptServiceStatus::Error && host.Entities().Revision() == old,
        "nonfinite operands leave pool/journal unchanged");
    r.Position = actual.Position; r.Sprite = 0;
    Require(host.CreateCoordinateBlip(r).Result.Status == NativeScriptServiceStatus::Unsupported && host.Entities().Revision() == old,
        "non-sprite height/color renderer requirement remains explicit");
    r.Position = actual.Position; r.Sprite = actual.Sprite; r.Id = {99300,1,1};
    const auto ready = [&hud](int sprite) { return hud.IsRadarSpriteUploaded(sprite); };
    const auto one = small.CreateCoordinateBlip(r,ready);
    Require(one.Result.Status == NativeScriptServiceStatus::Ready,"one-slot real-GPU service fixture");
    const auto smallRevision = small.Revision();
    auto another = r; ++another.Id.Instruction;
    Require(small.CreateCoordinateBlip(another,ready).Result.Status == NativeScriptServiceStatus::Error &&
        small.Revision() == smallRevision && !small.OwnsRequest(another.Id) &&
        small.CreateCoordinateBlip(r,{}).Reference.Value == one.Reference.Value,
        "pool exhaustion preserves journal, generation, live drawable and exact replay");
    Require(small.SetBlipDisplay({another.Id,{one.Reference.Value ^ 65536},2}).Status == NativeScriptServiceStatus::Error &&
        small.SetBlipDisplay({another.Id,one.Reference,4}).Status == NativeScriptServiceStatus::Error &&
        small.Revision() == smallRevision && !small.OwnsRequest(another.Id), "stale display and invalid flag are atomic");
    for (const auto& event : host.Events()) {
        another.Id = event.Id;
        Require(host.CreateCoordinateBlip(another).Result.Status == NativeScriptServiceStatus::Error,
            "all committed player/world/generator IDs exclude coordinate create");
    }
}
}

int main(int argc, char** argv) try {
    Require(argc == 2 || (argc == 3 && std::string_view(argv[2]) == "--cpu"), "game directory [--cpu] required");
    const bool cpu = argc == 3;
    std::setvbuf(stdout,nullptr,_IONBF,0);
    RealtimeGameplay gameplay; RealtimeScriptHost host(gameplay);
    E2ELoadInfo info; char message[512]{}; std::string error;
    Require(StreamPager_Init(argv[1],info,message,sizeof(message),{true,300,1200}),message);
    auto collision = NativeCollisionContext::LoadBeforeWorker(argv[1],900,error);
    Require(bool(collision) && host.InitializeBeforeWorker(argv[1],error,collision),error);
    RealtimeHud hud;
    NativeScriptEntities small(0,1);
    if (!cpu) Require(hud.Load(argv[1],message,sizeof(message)) && small.LoadBeforeWorker(argv[1],error),std::string(message)+error);
    Require(host.RunPass(1000).Status == NativeScriptStatus::Waiting && host.State().Commands == 53,"actual main53 first yield");
    Require(host.PrepareInitialGarageWorldBeforeWorker(error) && host.AdvanceTime(0,error),error);
    host.SealStartup(); // all RW/IO preparation is complete before runtime passes
    if (cpu) {
        const auto terminal = host.RunPass(5000);
        Require(terminal.Status == NativeScriptStatus::Unsupported && terminal.IP == 205876 && terminal.Opcode == 0x0570 &&
            host.Session().Threads()[1].Commands == 539,"unchanged CPU-negative539");
        std::printf("NativeCoordinateBlipProbe CPU PASS main=53 mission=539 terminal=0570@205876 GL-ready=0\n");
        StreamPager_Shutdown(); return 0;
    }
    {
        ProbeGl gl;
        Require(hud.Upload(message,sizeof(message)),message);
        host.SetRadarSpriteReady([&hud](int sprite) { return hud.IsRadarSpriteUploaded(sprite); });
        NativeScriptResult terminal; std::size_t creates = 0;
        NativeScriptRadarBlip first;
        for (unsigned i = 0; i < 10000; ++i) {
            const auto before = host.Entities().Revision();
            terminal = host.RunPass(1);
            const auto& thread = host.Session().Threads()[1];
            if (terminal.Status != NativeScriptStatus::BudgetYield) {
                Require(host.Entities().Revision() == before,"terminal instruction has no entity mutation");
                break;
            }
            if (thread.LastOpcode != 0x04CE) continue;
            const auto* blip = host.Entities().ResolveBlip({thread.LastOutputWrite.Value});
            Require(blip && blip->Kind == NativeScriptBlipKind::Coordinate && !blip->Contact &&
                blip->ShortRange && blip->Display == 3 && blip->Colour == 8 && blip->Bright &&
                hud.IsRadarSpriteUploaded(blip->Sprite), "actual04CE output owns typed source defaults and real GPU sprite");
            if (!creates) first = *blip;
            ++creates;
            std::printf("actual04CE commands=%llu ip=%u next=%u output=%u ref=%d xyz=%.9g,%.9g,%.9g sprite=%d kind=Coordinate display=%d\n",
                (unsigned long long)thread.Commands,thread.LastInstructionIP,thread.IP,thread.LastOutputWrite.Variable,
                blip->Reference.Value,blip->Position.X,blip->Position.Y,blip->Position.Z,blip->Sprite,blip->Display);
        }
        std::printf("strict-terminal status=%d main=%llu mission=%llu ip=%u opcode=%04X coordinates=%zu message=%s\n",
            int(terminal.Status),(unsigned long long)host.State().Commands,(unsigned long long)host.Session().Threads()[1].Commands,
            terminal.IP,terminal.Opcode,creates,terminal.Message.c_str());
        const auto& stopped = host.Session().Threads()[1];
        std::printf("preceding-instruction opcode=%04X ip=%u write-sequence=%llu variable=%u\n",
            stopped.LastOpcode, stopped.LastInstructionIP,
            (unsigned long long)stopped.LastOutputWrite.Sequence, stopped.LastOutputWrite.Variable);
        Require(terminal.Status == NativeScriptStatus::Unsupported,"strict actual next frontier");
        if (!creates) {
            Require(terminal.Opcode == 0x04CE && terminal.IP == 212086 && host.Session().Threads()[1].Commands == 1207,
                "current exact GPU readiness barrier");
            std::printf("NativeCoordinateBlipProbe GPU BARRIER 04CE@212086 mission=1207 missing actual HUD sprite; no full-boot claim\n");
        } else {
            Require(creates == 9 && host.Session().Threads()[1].Commands == 1234 && terminal.IP == 212669 && terminal.Opcode == 0x0814,
                "measured original mission0 frontier: nine coordinate sprites, restart registrations, then stunt jumps");
            Require(first.Position == NativeScriptPosition{2067.4f,-1831.2f,13.5f} && first.Sprite == 63 && first.Reference.Value == 65571,
                "first actual04CE operands and source allocation order");
            Require(NativeScriptRadarVisible(first,.5f,true,0,true) && !NativeScriptRadarVisible(first,1.01f,false,0,true) &&
                !NativeScriptRadarVisible(first,.5f,false,1,true) && !NativeScriptRadarVisible(first,.5f,false,0,false),
                "source coordinate mission/distance/zoom/interior conditions");
            auto contact = first; contact.Kind = NativeScriptBlipKind::Contact;
            Require(!NativeScriptRadarVisible(contact,.5f,true,0,true),"typed kind is authoritative over legacy Contact boolean");
            auto school = first; school.Sprite = 36;
            Require(NativeScriptRadarVisible(school,.5f,true,0,false),"source school interior whitelist");
            // Labelled HUD camera/proximity presentation fixture: original owned
            // blip positions remain untouched; use the exact Entities.Blips span.
            RealtimeHudState state;
            state.playerX = first.Position.X - 50; state.playerY = first.Position.Y;
            state.scriptBlips = host.Entities().Blips(); state.playerOnMission = true;
            const auto visible = Draw(hud,state);
            const auto* owned = host.Entities().ResolveBlip(first.Reference);
            Require(owned,"actual first coordinate still owned");
            const auto oldDisplay = owned->Display;
            Require(host.SetBlipDisplay({{99200,1,1},first.Reference,0}).Status == NativeScriptServiceStatus::Ready,"hide first actual coordinate");
            const auto hidden = Draw(hud,state);
            Require(visible != hidden,"actual coordinate remains visible during mission through real HUD/vector");
            Require(host.SetBlipDisplay({{99200,2,1},first.Reference,oldDisplay}).Status == NativeScriptServiceStatus::Ready &&
                Draw(hud,state) == visible,"source display restoration reproduces framebuffer");
            Transactions(host,hud,small,first);
            std::printf("NativeCoordinateBlipProbe GL PASS coordinates=%zu mission=%llu terminal=%04X@%u fullBoot=0\n",
                creates,(unsigned long long)host.Session().Threads()[1].Commands,terminal.Opcode,terminal.IP);
        }
        host.SetRadarSpriteReady({}); hud.ReleaseGpu();
    }
    StreamPager_Shutdown(); return 0;
} catch (const std::exception& e) {
    std::fprintf(stderr,"NativeCoordinateBlipProbe FAIL %s\n",e.what()); return 2;
}
