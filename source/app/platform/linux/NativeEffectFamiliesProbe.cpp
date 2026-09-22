#include "NativeEffectFamilies.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace {
int g_Checks = 0;
void Check(bool value, const char* message) {
    ++g_Checks;
    if (!value) { std::fprintf(stderr, "effect-families-fail: %s\n", message); std::exit(1); }
}
bool Near(float a, float b) { return std::abs(a - b) <= 1.0e-6f; }
}

int main() {
    NativeEffectFamilies effects;
    const auto& states = effects.States();
    Check(states.size() == NativeEffectFamilies::FamilyCount, "complete family inventory");
    for (std::size_t i = 0; i < states.size(); ++i)
        Check(static_cast<std::size_t>(states[i].Family) == i, "stable family order");
    const auto particle = NativeEffectFamilies::Composite(states[0], {0, 0, 0, 1});
    Check(Near(particle.Red, 0.125f) && Near(particle.Green, 0.10f) && Near(particle.Blue, 0.075f), "additive particle pixel");
    const auto shadow = NativeEffectFamilies::Composite(states[1], {1, 1, 1, 1});
    Check(Near(shadow.Red, 127.0f / 255.0f) && Near(shadow.Green, shadow.Red), "alpha shadow pixel");
    const auto reflection = NativeEffectFamilies::Composite(states[2], {0, 0, 0, 1});
    Check(Near(reflection.Red, 0.15f) && Near(reflection.Green, 0.25f) && Near(reflection.Blue, 0.35f), "MatFX reflection pixel");
    const auto post = NativeEffectFamilies::Composite(states[3], {0.16f, 0.16f, 0.16f, 1});
    Check(Near(post.Red, 0.36f) && Near(post.Green, 0.36f) && Near(post.Blue, 0.36f), "PC post pixel");
    Check(states[0].Additive && !states[0].DepthWrite && !states[1].Additive && states[2].DepthWrite && states[3].Additive, "blend/depth policy");
    Check(!NativeEffectFamilies::PresentationFeedback, "no feedback");
    std::printf("native-effect-families-ok checks=%d families=particle,shadow,reflection,post pixels=additive,alpha,env,pc-filter feedback=0\n", g_Checks);
}
