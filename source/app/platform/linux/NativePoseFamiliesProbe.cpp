#include "app/platform/linux/NativePoseFamilies.h"

#include <bit>
#include <cstdio>
#include <cstdlib>

namespace {
int g_Checks = 0;
void Check(bool value, const char* message) {
    ++g_Checks;
    if (!value) { std::fprintf(stderr, "pose-families-fail: %s\n", message); std::exit(1); }
}

std::uint64_t Fingerprint(const WorldShotScene& scene) {
    std::uint64_t hash = 1469598103934665603ull;
    auto add = [&](std::uint32_t value) {
        for (unsigned i = 0; i < 4; ++i) { hash ^= (value >> (i * 8)) & 0xFFu; hash *= 1099511628211ull; }
    };
    add(static_cast<std::uint32_t>(scene.meshes.size()));
    for (const auto& mesh : scene.meshes) {
        add(static_cast<std::uint32_t>(mesh.tris));
        for (const auto value : mesh.pos) add(std::bit_cast<std::uint32_t>(value));
        for (const auto value : mesh.nrm) add(std::bit_cast<std::uint32_t>(value));
    }
    return hash;
}
}

int main(int argc, char** argv) {
    const char* gameDir = argc > 1 ? argv[1] : "/game";
    NativePoseFamilies poses;
    std::string error;
    Check(poses.CapturePed(gameDir, "andre", "IDLE_stance", 0.5, error), error.c_str());
    const auto idle = poses.LastFrame();
    Check(idle && idle->Family == NativePoseFamily::Ped && idle->PedStats.bones == 32 &&
        idle->PedStats.mapped == 32 && idle->Scene.stats.triangles > 0, "source ped idle pose");
    const auto idleHash = Fingerprint(idle->Scene);
    Check(poses.CapturePed(gameDir, "andre", "JUMP_glide", 0.5, error), error.c_str());
    const auto jump = poses.LastFrame();
    Check(jump->Generation == idle->Generation + 1 && jump->PedStats.mapped == 26 &&
        Fingerprint(jump->Scene) != idleHash, "source ped family transition");
    const auto heldJump = jump;
    Check(poses.CapturePedTransition(gameDir, "andre", "IDLE_stance", "WALK_civi", 5, error), error.c_str());
    const auto transition = poses.LastTransition();
    Check(transition && transition->Frames.size() == 5 && transition->Alphas.front() == 0.0 &&
        transition->Alphas.back() == 1.0 && transition->MorphMonotonic > 0.9,
        "source five-frame skeletal blend");
    Check(Fingerprint(transition->Frames.front()) != Fingerprint(transition->Frames.back()),
        "transition endpoints differ");
    Check(poses.CaptureCutscene(gameDir, "cssmokevest", "smoke1a", "csplay", 0.5, error),
        error.c_str());
    const auto cutscene = poses.LastFrame();
    Check(cutscene && cutscene->Family == NativePoseFamily::Cutscene &&
        cutscene->CutsceneStats.bones == 61 && cutscene->CutsceneStats.mapped == 56 &&
        cutscene->Scene.stats.triangles == 2704, "source cutscene pose");
    Check(heldJump->Family == NativePoseFamily::Ped && Fingerprint(heldJump->Scene) == Fingerprint(jump->Scene),
        "immutable prior pose survives family switch");
    const auto retained = poses.LastFrame();
    Check(!poses.CapturePed(gameDir, "andre", "missing", 0.5, error) && poses.LastFrame() == retained,
        "missing clip rejection retains publication");
    Check(!NativePoseFamilies::PresentationFeedback, "no presentation feedback");
    std::printf("native-pose-families-ok checks=%d ped=%s jump-mapped=%d blend=%zu morph=%.6f cutscene=%s/%s bones=%d mapped=%d immutable=1 feedback=0\n",
        g_Checks, idle->Clip.c_str(), jump->PedStats.mapped, transition->Frames.size(),
        transition->MorphMonotonic, cutscene->Model.c_str(), cutscene->Clip.c_str(), cutscene->CutsceneStats.bones,
        cutscene->CutsceneStats.mapped);
}
