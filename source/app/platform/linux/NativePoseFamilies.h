#pragma once

#include "app/platform/linux/CsAnim.h"
#include "app/platform/linux/IfpAnim.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

enum class NativePoseFamily : std::uint8_t {
    Ped,
    Cutscene,
};

struct NativePoseFamilyFrame {
    std::uint64_t Generation = 0;
    NativePoseFamily Family = NativePoseFamily::Ped;
    std::string Model;
    std::string Bank;
    std::string Clip;
    double Fraction = 0.0;
    WorldShotScene Scene;
    IfpAnimStats PedStats{};
    CsAnimStats CutsceneStats{};
};

struct NativePoseFamilyTransition {
    std::uint64_t Generation = 0;
    std::string Model;
    std::string FromClip;
    std::string ToClip;
    std::vector<double> Alphas;
    std::vector<WorldShotScene> Frames;
    double MorphMonotonic = 0.0;
};

// Owns immutable, pointer-free CPU pose publications. Each capture uses the
// existing authoritative DFF/TXD/IFP readers and releases their parser-global
// state before publishing; no renderer or Godot node can feed pose state back.
class NativePoseFamilies {
public:
    bool CapturePed(const char* gameDir, const char* model, const char* clip,
        double fraction, std::string& error);
    bool CaptureCutscene(const char* gameDir, const char* model, const char* bank,
        const char* clip, double fraction, std::string& error);
    bool CapturePedTransition(const char* gameDir, const char* model,
        const char* fromClip, const char* toClip, int frames, std::string& error);

    std::shared_ptr<const NativePoseFamilyFrame> LastFrame() const { return m_Frame; }
    std::shared_ptr<const NativePoseFamilyTransition> LastTransition() const { return m_Transition; }
    static constexpr bool PresentationFeedback = false;

private:
    std::uint64_t m_Generation = 0;
    std::shared_ptr<const NativePoseFamilyFrame> m_Frame;
    std::shared_ptr<const NativePoseFamilyTransition> m_Transition;
};
