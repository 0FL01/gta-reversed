// Actual HUD-ready SCM continuation plus controller-owned save-token collection.
// Generated SCM below contains only probe instructions, never copied asset bytes.
#define GL_GLEXT_PROTOTYPES 1
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>

#include "app/platform/linux/RealtimeHud.h"
#include "app/platform/linux/RealtimeScriptHost.h"
#include "app/platform/linux/StreamPager.h"

#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Bytes = std::vector<std::uint8_t>;
int s_Failures;

void Check(bool ok, const char* label) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", label);
    s_Failures += !ok;
}

void Require(bool ok, const std::string& error) {
    if (!ok) throw std::runtime_error(error);
}

void Put(Bytes& bytes, std::uint32_t value, unsigned count) {
    for (unsigned i = 0; i < count; ++i) bytes.push_back(std::uint8_t(value >> (8 * i)));
}

void Patch(Bytes& bytes, std::size_t offset, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes.at(offset + i) = std::uint8_t(value >> (8 * i));
}

void Op(Bytes& bytes, std::uint16_t opcode) { Put(bytes, opcode, 2); }
void I32(Bytes& bytes, std::int32_t value) { bytes.push_back(1); Put(bytes, std::uint32_t(value), 4); }

Bytes Fixture(const Bytes& code) {
    Bytes bytes;
    const auto chunk = [&](std::uint8_t index, const Bytes& payload) {
        const auto next = std::uint32_t(bytes.size() + payload.size() + 8);
        Op(bytes, 2); I32(bytes, std::int32_t(next)); bytes.push_back(index);
        bytes.insert(bytes.end(), payload.begin(), payload.end());
    };
    chunk(115, Bytes(16));
    chunk(0, Bytes(4));
    Bytes info(16); Patch(info, 0, 104 + std::uint32_t(code.size()));
    chunk(1, info);
    chunk(2, Bytes(8));
    chunk(3, Bytes(4));
    Bytes extra(8); Patch(extra, 0, 16); chunk(4, extra);
    Require(bytes.size() == 104, "generated pickup SCM header size");
    bytes.insert(bytes.end(), code.begin(), code.end());
    return bytes;
}

void LoadFixture(NativeScriptSession& session, NativeScriptServices& services, const Bytes& code) {
    const auto fixture = Fixture(code);
    std::string error;
    Require(session.LoadMainBytes(fixture, fixture.size(), error), error);
    const auto headers = session.Run(services, 6);
    Require(headers.Status == NativeScriptStatus::BudgetYield && headers.Executed == 6 && session.State().IP == 104,
        "generated pickup SCM header traversal");
}

bool Query(RealtimeScriptHost& host, NativeScriptPickupRef ref) {
    Bytes code; Op(code, 0x0214); I32(code, ref.Value);
    NativeScriptSession session; LoadFixture(session, host, code);
    const auto result = session.Step(host);
    Require(result.Status == NativeScriptStatus::Advanced && result.Executed == 1 && result.Opcode == 0x0214,
        result.Message);
    return session.State().Condition;
}

void Remove(RealtimeScriptHost& host, NativeScriptPickupRef ref) {
    Bytes code; Op(code, 0x0215); I32(code, ref.Value);
    NativeScriptSession session; LoadFixture(session, host, code);
    const auto result = session.Step(host);
    Require(result.Status == NativeScriptStatus::Advanced && result.Executed == 1 && result.Opcode == 0x0215,
        result.Message);
}

struct EglContext {
    EGLDisplay Display = EGL_NO_DISPLAY;
    EGLSurface Surface = EGL_NO_SURFACE;
    EGLContext Context = EGL_NO_CONTEXT;

    EglContext() {
        const auto getDisplay = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
        Require(getDisplay, "surfaceless EGL entry point");
        Display = getDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
        Require(Display != EGL_NO_DISPLAY && eglInitialize(Display, nullptr, nullptr) && eglBindAPI(EGL_OPENGL_API),
            "surfaceless EGL initialization");
        const EGLint attributes[]{EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
            EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
        EGLConfig config{}; EGLint count{};
        Require(eglChooseConfig(Display, attributes, &config, 1, &count) && count, "pickup script EGL config");
        const EGLint size[]{EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
        Surface = eglCreatePbufferSurface(Display, config, size);
        Context = eglCreateContext(Display, config, EGL_NO_CONTEXT, nullptr);
        Require(Surface != EGL_NO_SURFACE && Context != EGL_NO_CONTEXT &&
            eglMakeCurrent(Display, Surface, Surface, Context), "pickup script EGL context");
    }

    ~EglContext() {
        if (Display == EGL_NO_DISPLAY) return;
        eglMakeCurrent(Display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (Context != EGL_NO_CONTEXT) eglDestroyContext(Display, Context);
        if (Surface != EGL_NO_SURFACE) eglDestroySurface(Display, Surface);
        eglTerminate(Display);
    }
};
} // namespace

int main(int argc, char** argv) try {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const char* gameDir = argc > 1 ? argv[1] : "/game";
    char errorBuffer[512]{}; std::string error;
    E2ELoadInfo info{};
    Require(StreamPager_Init(gameDir, info, errorBuffer, sizeof(errorBuffer),
        {.includeStreamed=true, .radius=300, .maxInstances=1200}), errorBuffer);

    RealtimeGameplay gameplay;
    RealtimeScriptHost host(gameplay);
    NativeScriptCorpusManifest corpus;
    const auto observe = [&](std::size_t index) {
        const auto threads = host.Session().Threads();
        Require(index < threads.size() && threads[index].Active, "expected active corpus thread");
        NativeScriptInstructionForm form;
        if (!host.Session().InspectInstruction(index, form, error)) return false;
        Require(corpus.Observe(form, error), error);
        return true;
    };
    Require(host.InitializeBeforeWorker(gameDir, error), error);
    EglContext egl;
    RealtimeHud hud;
    Require(hud.Load(gameDir, errorBuffer, sizeof(errorBuffer)), errorBuffer);
    Require(hud.PreparedRadarSprite(33) && !hud.IsRadarSpriteUploaded(33),
        "actual CPU radar_race image is prepared but not a readiness surrogate");
    Require(hud.Upload(errorBuffer, sizeof(errorBuffer)), errorBuffer);
    Require(hud.IsRadarSpriteUploaded(33), "actual GL HUD sprite33 upload is ready");
    host.SetRadarSpriteReady([&](std::int32_t sprite) { return hud.IsRadarSpriteUploaded(sprite); });

    NativeScriptResult first;
    std::size_t mainCommands = 0;
    for (unsigned guard = 0; guard < 100; ++guard) {
        Require(observe(0), error);
        first = host.RunPass(1);
        mainCommands += first.Executed;
        if (first.Status != NativeScriptStatus::BudgetYield) break;
    }
    Require(first.Status == NativeScriptStatus::Waiting && mainCommands == 53 && host.State().IP == 56369,
        first.Message);
    Require(host.PrepareInitialGarageWorldBeforeWorker(error), error);
    Require(host.AdvanceTime(0, error), error);
    host.SealStartup();
    NativeScriptResult terminal;
    for (unsigned guard = 0; guard < 20000; ++guard) {
        if (!observe(1)) {
            terminal = host.RunPass(1);
            break;
        }
        terminal = host.RunPass(1);
        if (terminal.Status != NativeScriptStatus::BudgetYield) break;
    }
    const auto& mission = host.Session().Threads()[1];
    std::printf("pickup-script actual-bound status=%d executed=%zu commands=%llu ip=%u opcode=%04X last=%04X@%u "
        "hud33=%d cpuImage=%s fullboot=0\n", int(terminal.Status), terminal.Executed,
        static_cast<unsigned long long>(mission.Commands), terminal.IP, terminal.Opcode,
        mission.LastOpcode, mission.LastInstructionIP, hud.IsRadarSpriteUploaded(33),
        hud.PreparedRadarSprite(33)->name);
    const auto jumps = host.StuntJumps().Entries();
    if (!jumps.empty()) {
        const auto& firstJump = jumps.front(); const auto& lastJump = jumps.back();
        std::printf("stunt-jump-registry count=%zu revision=%llu first=%.9g,%.9g,%.9g/%.9g,%.9g,%.9g "
            "camera=%.9g,%.9g,%.9g reward=%d last-reward=%d runtime-update=%d save=%d\n",
            jumps.size(), static_cast<unsigned long long>(host.StuntJumps().Revision()),
            firstJump.Start.Min.X, firstJump.Start.Min.Y, firstJump.Start.Min.Z,
            firstJump.Start.Max.X, firstJump.Start.Max.Y, firstJump.Start.Max.Z,
            firstJump.Camera.X, firstJump.Camera.Y, firstJump.Camera.Z, firstJump.Reward,
            lastJump.Reward, NativeStuntJumps::Coverage.RuntimeUpdate, NativeStuntJumps::Coverage.SaveLoad);
    }
    NativeStuntJump expectedFirst;
    const auto sourceFloat = [](std::uint32_t bits) { return std::bit_cast<float>(bits); };
    const NativeStuntJumpVector startCenter{sourceFloat(0x44F26458), sourceFloat(0xC4F601AA), sourceFloat(0x418D51B7)};
    const NativeStuntJumpVector startHalf{sourceFloat(0x40200000), sourceFloat(0x402E978D), sourceFloat(0x404AE148)};
    const NativeStuntJumpVector endCenter{sourceFloat(0x44F8BEAC), sourceFloat(0xC4F67E71), sourceFloat(0x41E5CC98)};
    const NativeStuntJumpVector endHalf{sourceFloat(0x413224DD), sourceFloat(0x40ACBC6A), sourceFloat(0x40E75C29)};
    expectedFirst.Start = {{startCenter.X - startHalf.X, startCenter.Y - startHalf.Y, startCenter.Z - startHalf.Z},
        {startCenter.X + startHalf.X, startCenter.Y + startHalf.Y, startCenter.Z + startHalf.Z}};
    expectedFirst.End = {{endCenter.X - endHalf.X, endCenter.Y - endHalf.Y, endCenter.Z - endHalf.Z},
        {endCenter.X + endHalf.X, endCenter.Y + endHalf.Y, endCenter.Z + endHalf.Z}};
    expectedFirst.Camera = {sourceFloat(0x44F5116E), sourceFloat(0xC4F57247), sourceFloat(0x41CA954D)};
    expectedFirst.Reward = 500;
    Check(terminal.Status == NativeScriptStatus::Unsupported && terminal.Executed == 0 &&
        mission.Commands == 1305 && terminal.IP == 218276 && terminal.Opcode == 0x029B &&
        mission.LastInstructionIP == 218269 && mission.LastOpcode == 0x0004 && host.WorldRevision() == 3 &&
        jumps.size() == 70 && host.StuntJumps().Revision() == 70 && jumps.front() == expectedFirst &&
        std::ranges::all_of(jumps, [](const auto& jump) { return !jump.Done && !jump.Found; }) &&
        NativeStuntJumps::Coverage == NativeStuntJumpCoverage{true, false, false, false, false},
        "actual startup registers70 source stunt jumps then stops strictly at object creation");
    const auto stuntEvent = std::ranges::find(host.Events(), std::uint16_t(0x0814), &RealtimeScriptHostEvent::Opcode);
    Require(stuntEvent != host.Events().end(), "actual stunt-jump event journal");
    NativeScriptStuntJumpRequest replay{stuntEvent->Id,
        {startCenter.X, startCenter.Y, startCenter.Z}, {startHalf.X, startHalf.Y, startHalf.Z},
        {endCenter.X, endCenter.Y, endCenter.Z}, {endHalf.X, endHalf.Y, endHalf.Z},
        {expectedFirst.Camera.X, expectedFirst.Camera.Y, expectedFirst.Camera.Z}, expectedFirst.Reward};
    Check(host.AddStuntJump(replay).Status == NativeScriptServiceStatus::Ready &&
        host.StuntJumps().Entries().size() == 70 && host.StuntJumps().Revision() == 70,
        "stunt-jump service replay does not duplicate registration");
    ++replay.Reward;
    Check(host.AddStuntJump(replay).Status == NativeScriptServiceStatus::Error &&
        host.StuntJumps().Entries().size() == 70 && host.StuntJumps().Revision() == 70,
        "same request ID with changed registration rejects atomically");
    const auto summary = corpus.Summary();
    Check(summary.Encounters == 1359 && summary.Sites == 1359 && summary.Threads == 2 &&
        summary.Opcodes == 44 && summary.OperandForms == 49 && summary.MainSites == 53 &&
        summary.MissionSites == 1306 && summary.StreamedSites == 0 &&
        summary.ImplementedSites == 1358 && summary.UnsupportedSites == 1 &&
        corpus.Fingerprint() == 0xF2F1C258210742ADULL,
        "actual startup corpus classifies registered jumps and next strict object frontier");
    const auto& frontier = corpus.Sites().back().Form;
    Check(frontier.IP == 218276 && frontier.NextIP == 218298 && frontier.Opcode == 0x029B &&
        frontier.OperandCount == 5 && frontier.Semantics == NativeScriptSemanticCoverage::Unsupported &&
        frontier.ThreadForm == NativeScriptThreadForm::Mission &&
        frontier.OperandTags[0] == 4 && frontier.OperandTags[1] == 6 && frontier.OperandTags[2] == 6 &&
        frontier.OperandTags[3] == 6 && frontier.OperandTags[4] == 2,
        "029B complete model/position/output form remains semantic Unsupported");
    std::printf("script-corpus schema=%s sha256=%s version=%s encounters=%llu sites=%zu threads=%zu opcodes=%zu forms=%zu main=%zu mission=%zu streamed=%zu implemented=%zu unsupported=%zu frontier=%04X@%u next=%u fingerprint=%016llX nop-substitution=0\n",
        NativeScriptSchemaRevision().data(), NativeScriptSchemaSha256().data(), NativeScriptSchemaVersion().data(),
        static_cast<unsigned long long>(summary.Encounters), summary.Sites, summary.Threads, summary.Opcodes,
        summary.OperandForms, summary.MainSites, summary.MissionSites, summary.StreamedSites,
        summary.ImplementedSites, summary.UnsupportedSites, frontier.Opcode, frontier.IP, frontier.NextIP,
        static_cast<unsigned long long>(corpus.Fingerprint()));

    auto& entities = host.Entities();
    const auto player = gameplay.State().PedRoot;
    const NativeScriptPosition pickupPosition{player.X, player.Y, player.Z};
    std::uint64_t directInstruction = 1;
    const auto create = [&](std::uint64_t session) {
        NativeScriptPickupRequest request{{session, directInstruction++, 900000}, 1277, 3, pickupPosition};
        const auto result = host.CreatePickup(request);
        Require(result.Result.Status == NativeScriptServiceStatus::Ready && result.Reference.Value != -1,
            result.Result.Message);
        return std::pair{result.Reference, request.Id};
    };
    std::uint32_t frame = 0;
    const auto collect = [&](NativeScriptPickupRef ref) {
        const auto slot = std::uint32_t(ref.Value) & 0xffff;
        unsigned partition = 0;
        while (!(slot >= 620 * partition / 6 && slot < 620 * (partition + 1) / 6)) ++partition;
        if (!frame) frame = 96 + partition;
        else while (frame % 6 != partition) ++frame;
        NativeScriptPropertyInput input; input.FrameCounter = frame;
        const auto camera = gameplay.Camera().Position;
        Require(host.TickPlayerEntities({camera.X, camera.Y, camera.Z}, input, error), error);
        Require(entities.AdvanceTime(frame, error), error);
        Require(!entities.ResolvePickup(ref), "actual controller collection removes the exact save reference");
        const auto shake = entities.ConsumePadShake();
        Require(shake && shake->Pickup.Value == ref.Value && shake->FrameCounter == frame &&
            shake->TimeMs == 120 && shake->Frequency == 100 && shake->Arg2 == 0 && !entities.ConsumePadShake(),
            "actual controller collection publishes one owned source shake event");
        frame += 96;
    };

    const auto [collectedRef, collectedCreateId] = create(9100);
    const auto* save = entities.ResolvePickup(collectedRef);
    Require(save && save->Model == 1277 && save->Type == 3 && save->Actor.stats.triangles > 0 &&
        std::string(save->Actor.stats.dffName) == "pickupsave.dff", "actual save model/reference allocation");
    collect(collectedRef);
    const auto beforeQueryRevision = entities.Revision();
    Bytes operations; Op(operations, 0x0214); I32(operations, collectedRef.Value);
    Op(operations, 0x8214); I32(operations, collectedRef.Value);
    Op(operations, 0x0215); I32(operations, collectedRef.Value);
    NativeScriptSession operationSession; LoadFixture(operationSession, host, operations);
    Check(operationSession.Step(host).Status == NativeScriptStatus::Advanced && operationSession.State().Condition &&
        entities.Revision() == beforeQueryRevision + 1,
        "0214 consumes the inactive full-generation collection-ring reference once");
    Check(operationSession.Step(host).Status == NativeScriptStatus::Advanced && operationSession.State().Condition &&
        operationSession.State().LastOpcode == 0x8214 && entities.Revision() == beforeQueryRevision + 1,
        "negated repeated 0214 observes consumed false and preserves true condition");
    Check(operationSession.Step(host).Status == NativeScriptStatus::Advanced && operationSession.State().Condition &&
        entities.Revision() == beforeQueryRevision + 1,
        "0215 on current-generation inactive pickup is a source-valid no-op");

    const auto [removedRef, removedCreateId] = create(9200);
    Remove(host, removedRef);
    Check(!entities.ResolvePickup(removedRef), "0215 removes an active actual save pickup");
    const auto [reusedRef, reusedCreateId] = create(9201);
    Check((std::uint32_t(reusedRef.Value) & 0xffff) == (std::uint32_t(removedRef.Value) & 0xffff) &&
        std::uint32_t(reusedRef.Value) >> 16 == (std::uint32_t(removedRef.Value) >> 16) + 1,
        "pickup slot reuse increments the full source generation");
    Remove(host, removedRef);
    Check(entities.ResolvePickup(reusedRef), "stale-generation 0215 cannot remove the reused live pickup");
    Remove(host, reusedRef);
    Require(!entities.ResolvePickup(reusedRef), "retire stale-reference fixture before ring collection");

    std::vector<NativeScriptPickupRef> wrapped;
    for (unsigned i = 0; i < 21; ++i) {
        const auto ref = create(9300 + i).first;
        collect(ref);
        wrapped.push_back(ref);
    }
    Check(!Query(host, wrapped.front()) && Query(host, wrapped[1]),
        "actual capacity20 collection ring wraps and compares complete generation-bearing references");
    const auto [liveRef, liveCreateId] = create(9350);

    const auto eventCount = host.Events().size();
    const auto entityRevision = entities.Revision();
    const NativeScriptPickupReferenceRequest cross{{9400, 1, 1}, wrapped.front()};
    Check(host.HasPickupBeenCollected(cross).Result.Status == NativeScriptServiceStatus::Ready &&
        host.HasPickupBeenCollected(cross).Result.Status == NativeScriptServiceStatus::Ready &&
        host.RemoveScriptPickup(cross).Status == NativeScriptServiceStatus::Error,
        "pickup query request replay is idempotent and cannot change operation kind");
    Check(host.RequestCollision({cross.Id, 0, 0}).Status == NativeScriptServiceStatus::Error &&
        host.GetPlayerChar({cross.Id, 0}).Result.Status == NativeScriptServiceStatus::Error &&
        host.SetEntryExitFlag({cross.Id, 0, 0, 1, 0x4000, 1}).Status == NativeScriptServiceStatus::Error &&
        host.DeactivateGarage({cross.Id, host.Garages().Entries().at(13).Name}).Status == NativeScriptServiceStatus::Error,
        "pickup operation ID cannot cross world/player/ENEX/garage journals");
    NativeScriptPickupRequest crossedCreate{cross.Id, 1277, 3, pickupPosition};
    Check(host.CreatePickup(crossedCreate).Result.Status == NativeScriptServiceStatus::Error &&
        host.HasPickupBeenCollected({reusedCreateId, reusedRef}).Result.Status == NativeScriptServiceStatus::Error &&
        host.HasPickupBeenCollected({liveCreateId, liveRef}).Result.Status == NativeScriptServiceStatus::Error &&
        host.HasPickupBeenCollected({collectedCreateId, collectedRef}).Result.Status == NativeScriptServiceStatus::Error &&
        host.HasPickupBeenCollected({removedCreateId, removedRef}).Result.Status == NativeScriptServiceStatus::Error,
        "pickup operation IDs cannot reuse any entity creation identity");
    bool hostIdsRejected = true;
    for (const auto opcode : {std::uint16_t(0x04E4), std::uint16_t(0x0053), std::uint16_t(0x09B4), std::uint16_t(0x02B9)}) {
        const auto event = std::ranges::find_if(host.Events(), [&](const auto& candidate) { return candidate.Opcode == opcode; });
        hostIdsRejected &= event != host.Events().end() &&
            host.HasPickupBeenCollected({event->Id, liveRef}).Result.Status == NativeScriptServiceStatus::Error &&
            host.RemoveScriptPickup({event->Id, liveRef}).Status == NativeScriptServiceStatus::Error;
    }
    Check(hostIdsRejected && host.Events().size() == eventCount && entities.Revision() == entityRevision,
        "world/player/ENEX/garage IDs cannot enter pickup operations or mutate either journal");

    Check(entities.ResolvePickup(liveRef) && reusedCreateId.Session == 9201 &&
        gameplay.Activity().Authority == NativePlayerActivityAuthority::SourceBacked,
        "cross-service failures preserve reused pickup and actual controller authority");
    std::printf("native-pickup-script failures=%d actualCommands=%llu terminal=%04X@%u hud33=actual-GL "
        "collection=controller-owned ring=20 staleRef=safe os-feedback=pending-parent fullboot=0\n",
        s_Failures, static_cast<unsigned long long>(mission.Commands), terminal.Opcode, terminal.IP);
    hud.ReleaseGpu();
    StreamPager_Shutdown();
    return s_Failures ? 1 : 0;
} catch (const std::exception& exception) {
    std::fprintf(stderr, "NativePickupScriptProbe FAIL %s\n", exception.what());
    return 2;
}
