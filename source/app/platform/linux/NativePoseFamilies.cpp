#include "app/platform/linux/NativePoseFamilies.h"

#include <cmath>
#include <limits>

namespace {
bool ValidText(const char* value) {
    return value && *value;
}
}

bool NativePoseFamilies::CapturePed(const char* gameDir, const char* model, const char* clip,
    double fraction, std::string& error) {
    if (!ValidText(gameDir) || !ValidText(model) || !ValidText(clip) ||
        !std::isfinite(fraction) || fraction < 0.0 || fraction > 1.0 ||
        m_Generation == std::numeric_limits<std::uint64_t>::max()) {
        error = "invalid ped pose request";
        return false;
    }
    NativePoseFamilyFrame next;
    char message[512]{};
    if (!IfpAnim_Init(gameDir, model, clip, fraction, next.Scene, next.PedStats,
        message, sizeof(message), true)) {
        error = message;
        IfpAnim_Shutdown();
        return false;
    }
    IfpAnim_Shutdown();
    next.Generation = m_Generation + 1;
    next.Family = NativePoseFamily::Ped;
    next.Model = next.PedStats.model;
    next.Bank = next.PedStats.bank;
    next.Clip = next.PedStats.anim;
    next.Fraction = fraction;
    auto published = std::make_shared<const NativePoseFamilyFrame>(std::move(next));
    ++m_Generation;
    m_Frame = std::move(published);
    error.clear();
    return true;
}

bool NativePoseFamilies::CaptureCutscene(const char* gameDir, const char* model,
    const char* bank, const char* clip, double fraction, std::string& error) {
    if (!ValidText(gameDir) || !ValidText(model) || !ValidText(bank) || !ValidText(clip) ||
        !std::isfinite(fraction) || fraction < 0.0 || fraction > 1.0 ||
        m_Generation == std::numeric_limits<std::uint64_t>::max()) {
        error = "invalid cutscene pose request";
        return false;
    }
    NativePoseFamilyFrame next;
    char message[512]{};
    if (!CsAnim_Init(gameDir, model, bank, clip, fraction, next.Scene, next.CutsceneStats,
        message, sizeof(message))) {
        error = message;
        CsAnim_Shutdown();
        return false;
    }
    CsAnim_Shutdown();
    next.Generation = m_Generation + 1;
    next.Family = NativePoseFamily::Cutscene;
    next.Model = next.CutsceneStats.model;
    next.Bank = next.CutsceneStats.bank;
    next.Clip = next.CutsceneStats.anim;
    next.Fraction = fraction;
    auto published = std::make_shared<const NativePoseFamilyFrame>(std::move(next));
    ++m_Generation;
    m_Frame = std::move(published);
    error.clear();
    return true;
}

bool NativePoseFamilies::CapturePedTransition(const char* gameDir, const char* model,
    const char* fromClip, const char* toClip, int frames, std::string& error) {
    if (!ValidText(gameDir) || !ValidText(model) || !ValidText(fromClip) ||
        !ValidText(toClip) || frames < 2 || frames > 64 ||
        m_Generation == std::numeric_limits<std::uint64_t>::max()) {
        error = "invalid ped pose transition request";
        return false;
    }
    IfpAnimBlendResult source;
    char message[512]{};
    if (!IfpAnim_Blend(gameDir, model, fromClip, toClip, frames, source,
        message, sizeof(message))) {
        error = message;
        IfpAnim_Shutdown();
        return false;
    }
    IfpAnim_Shutdown();
    NativePoseFamilyTransition next;
    next.Generation = m_Generation + 1;
    next.Model = model;
    next.FromClip = source.fromAnim;
    next.ToClip = source.toAnim;
    next.Alphas = std::move(source.alphas);
    next.MorphMonotonic = source.morphMono;
    next.Frames.reserve(source.frames.size());
    for (auto& frame : source.frames) next.Frames.push_back(std::move(frame.scene));
    auto published = std::make_shared<const NativePoseFamilyTransition>(std::move(next));
    ++m_Generation;
    m_Transition = std::move(published);
    error.clear();
    return true;
}
