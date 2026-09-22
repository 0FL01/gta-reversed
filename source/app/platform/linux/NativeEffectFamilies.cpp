#include "NativeEffectFamilies.h"

#include <algorithm>

namespace {
float Clamp(float value) { return std::clamp(value, 0.0f, 1.0f); }
}

NativeEffectFamilies::NativeEffectFamilies() {
    m_States = {{
        {NativeEffectFamily::Particle, {0.25f, 0.20f, 0.15f, 0.50f}, {}, 1.0f, true, false},
        {NativeEffectFamily::Shadow, {0.0f, 0.0f, 0.0f, 128.0f / 255.0f}, {}, 1.0f, false, false},
        {NativeEffectFamily::Reflection, {0.10f, 0.10f, 0.10f, 0.50f}, {0.20f, 0.40f, 0.60f, 1.0f}, 0.5f, false, true},
        {NativeEffectFamily::Post, {0.20f, 0.20f, 0.20f, 0.50f}, {0.40f, 0.40f, 0.40f, 0.25f}, 1.0f, true, false},
    }};
}

const NativeEffectState& NativeEffectFamilies::State(NativeEffectFamily family) const noexcept {
    return m_States[static_cast<std::size_t>(family)];
}

NativeEffectColour NativeEffectFamilies::Composite(const NativeEffectState& state,
    NativeEffectColour destination) noexcept {
    auto out = destination;
    switch (state.Family) {
    case NativeEffectFamily::Particle:
        out.Red = Clamp(destination.Red + state.Primary.Red * state.Primary.Alpha);
        out.Green = Clamp(destination.Green + state.Primary.Green * state.Primary.Alpha);
        out.Blue = Clamp(destination.Blue + state.Primary.Blue * state.Primary.Alpha);
        break;
    case NativeEffectFamily::Shadow:
        out.Red = destination.Red * (1.0f - state.Primary.Alpha);
        out.Green = destination.Green * (1.0f - state.Primary.Alpha);
        out.Blue = destination.Blue * (1.0f - state.Primary.Alpha);
        break;
    case NativeEffectFamily::Reflection:
        out.Red = Clamp(state.Primary.Red * state.Primary.Alpha + state.Secondary.Red * state.Coefficient + destination.Red * (1.0f - state.Primary.Alpha));
        out.Green = Clamp(state.Primary.Green * state.Primary.Alpha + state.Secondary.Green * state.Coefficient + destination.Green * (1.0f - state.Primary.Alpha));
        out.Blue = Clamp(state.Primary.Blue * state.Primary.Alpha + state.Secondary.Blue * state.Coefficient + destination.Blue * (1.0f - state.Primary.Alpha));
        break;
    case NativeEffectFamily::Post:
        out.Red = Clamp(destination.Red + state.Primary.Red * state.Primary.Alpha + state.Secondary.Red * state.Secondary.Alpha);
        out.Green = Clamp(destination.Green + state.Primary.Green * state.Primary.Alpha + state.Secondary.Green * state.Secondary.Alpha);
        out.Blue = Clamp(destination.Blue + state.Primary.Blue * state.Primary.Alpha + state.Secondary.Blue * state.Secondary.Alpha);
        break;
    }
    out.Alpha = 1.0f;
    return out;
}
