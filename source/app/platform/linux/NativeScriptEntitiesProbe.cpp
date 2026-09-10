// CPU-only production presentation and wrapping checks; link with GC sections
// so this gate needs neither RW startup nor any asset/EXE reads.
#include "app/platform/linux/NativeScriptEntities.h"
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace {
void Check(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "help FAIL %s\n", message); std::exit(2); }
}
}
int main() {
    NativeScriptHelpPresentation help;
    std::string error;
    Check(help.AdvanceTime(1000, error), "clock epoch");
    help.Show("first", 2);
    Check(help.LifetimeMs() == 5000, "source line-dependent duration");
    Check(help.AdvanceTime(1000, error) && help.View().Alpha == 0, "same timestamp does not consume new message");
    Check(help.AdvanceTime(1001, error) && help.View().Alpha == 0, "source first positive frame prints zero alpha");
    Check(help.AdvanceTime(1017, error) && help.View().Alpha == 200, "source holding alpha");
    Check(help.AdvanceTime(6017, error) && help.View().Alpha == 200, "source timer comparison precedes increment");
    Check(help.AdvanceTime(6018, error) && help.View().Alpha == 200, "expiry enters fade with holding alpha this frame");
    Check(help.AdvanceTime(6118, error) && help.View().Alpha == 80, "600 minus twice 100ms, times 200/1000");
    Check(!help.AdvanceTime(6100, error) && help.View().Alpha == 80 && help.View().Text == "first", "stale time preserves text/fade");
    Check(help.AdvanceTime(6118, error) && help.View().Alpha == 80, "duplicate time preserves fade");
    Check(help.AdvanceTime(6318, error) && !help.View().Alpha, "300ms fade reaches zero");
    Check(help.AdvanceTime(6319, error) && help.View().Text.empty(), "negative fade retires presentation");
    Check(help.AdvanceTime(10000, error) && help.View().Text.empty(), "expired help never reappears");
    help.Show("replacement A", 1);
    Check(help.AdvanceTime(10001, error) && help.AdvanceTime(10017, error) && help.View().Alpha == 200, "new message after expiry");
    help.Show("replacement B", 3);
    Check(!help.View().Alpha && help.View().Text == "replacement B" && help.LifetimeMs() == 6000, "nonpermanent SetHelpMessage replaces and resets");
    Check(help.AdvanceTime(10018, error) && !help.View().Alpha && help.AdvanceTime(10034, error) && help.View().Alpha == 200,
        "replacement gets its own presentation lifetime");
    NativeScriptHelpPresentation wrap;
    Check(wrap.AdvanceTime(0xfffffff0, error), "wrap epoch"); wrap.Show("wrap", 1);
    Check(wrap.AdvanceTime(0xfffffff1, error) && wrap.AdvanceTime(0x10, error) && wrap.View().Alpha == 200, "uint32 game clock rollover accepted");
    Check(!wrap.AdvanceTime(0xf, error) && wrap.View().Alpha == 200, "backwards stamp after wrap rejected");
    std::array<int, 208> metrics; metrics.fill(20);
    const auto lines = NativeScriptHelpLines("one two three four five six", metrics);
    Check(lines.size() == 2 && lines[0] == "one two three four " && lines[1] == "five six", "word boundaries shared by duration and HUD layout");
    const auto near = [](float a, float b) { return std::abs(a-b) < 0.00005f; };
    for (std::size_t axis = 0; axis < 3; ++axis) {
        std::array<float, 3> extent{0.1f, 0.1f, 0.1f}; extent[axis] = 0.6f;
        Check(near(NativeScriptPropertyScale({}, extent), 1.6f), "largest extent on each asymmetric axis; 60-percent scale interpolation");
    }
    Check(near(NativeScriptPropertyScale({10,-4,100}, {10.2f,-3.6f,100.3f}), 2.2f), "COL extent, not absolute coordinates or render bounds");
    Check(NativeScriptPropertyScale({}, {2,3,4}) == 1, "source never shrinks large pickups");
    Check(NativeScriptPropertyScale({}, {1.2f,0.4f,0.2f}) == 1, "source scale clamp at unit ratio");
    for (const auto bounds : {std::array<float,3>{0,0,0}, {-1,1,1}, {INFINITY,1,1}, {std::numeric_limits<float>::quiet_NaN(),1,1}}) {
        bool rejected = false;
        try { (void)NativeScriptPropertyScale({}, bounds); } catch (const std::runtime_error&) { rejected = true; }
        Check(rejected, "invalid COL bounds fail instead of publishing a guessed scale");
    }
    WorldShotScene bind;
    auto& mesh = bind.meshes.emplace_back(); mesh.tris = 1;
    mesh.pos = {2,3,5, 3,3,5, 2,4,6}; // asymmetric off-origin, sloping triangle
    const float diagonal = std::sqrt(0.5f);
    mesh.nrm = {0,-diagonal,diagonal, 0,-diagonal,diagonal, 0,-diagonal,diagonal};
    const auto bindPositions = mesh.pos, bindNormals = mesh.nrm;
    const NativeScriptPosition origin{100,-50,12};
    const auto zero = NativeScriptPropertyActor(bind, origin, 1.6f, 0);
    Check(near(zero.meshes[0].pos[0],103.2f) && near(zero.meshes[0].pos[1],-45.2f) && near(zero.meshes[0].pos[2],20),
        "source phase zero replaces allocation heading; scale about model origin, no COL recentering");
    const auto quarter = NativeScriptPropertyActor(bind, origin, 1.6f, 512);
    Check(near(quarter.meshes[0].pos[0], float(100+1.6*(2*std::cos(1.565000057220459)-3*std::sin(1.565000057220459)))) &&
        near(quarter.meshes[0].pos[1], float(-50+1.6*(2*std::sin(1.565000057220459)+3*std::cos(1.565000057220459)))),
        "source 512ms angle is 1.565000057, not a guessed pi/2 rotation");
    for (const auto time : {0u,512u,1024u,2047u,2048u,2560u,65536u,0xffffffffu}) {
        const auto actor = NativeScriptPropertyActor(bind, origin, 1.6f, time);
        const auto repeated = NativeScriptPropertyActor(bind, origin, 1.6f, time & 2047);
        const auto& a = actor.meshes[0];
        Check(a.pos == repeated.meshes[0].pos && a.nrm == repeated.meshes[0].nrm, "source absolute phase mask wraps without accumulation");
        std::array<float,3> edgeA{}, edgeB{}, cross{};
        for (std::size_t j=0; j<3; ++j) { edgeA[j]=a.pos[j+3]-a.pos[j]; edgeB[j]=a.pos[j+6]-a.pos[j]; }
        cross={edgeA[1]*edgeB[2]-edgeA[2]*edgeB[1],edgeA[2]*edgeB[0]-edgeA[0]*edgeB[2],edgeA[0]*edgeB[1]-edgeA[1]*edgeB[0]};
        float length=0, normalLength=0, edgeLength=0;
        for (std::size_t j=0;j<3;++j) { length+=cross[j]*cross[j]; normalLength+=a.nrm[j]*a.nrm[j]; edgeLength+=edgeA[j]*edgeA[j]; }
        Check(near(normalLength,1) && near(std::sqrt(edgeLength),1.6f), "measured normal length and geometry scale");
        for (std::size_t j=0;j<3;++j) Check(near(cross[j]/std::sqrt(length),a.nrm[j]), "normal agrees with transformed triangle winding");
    }
    Check(bind.meshes[0].pos == bindPositions && bind.meshes[0].nrm == bindNormals, "immutable owned bind geometry retained");
    std::puts("NativeScriptEntitiesProbe PASS help-lifetime/fade/expiry/replacement/stale/duplicate/wrap/word-layout property-COL-scale/asymmetric-bounds/source-phase/wrap/measured-normals/bind-preservation");
}
