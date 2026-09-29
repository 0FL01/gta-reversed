#!/usr/bin/env python3
"""Compile literal ManagePed/Vector bodies against the isolated value policy."""
import argparse
import hashlib
from pathlib import Path
import subprocess


def body(text, signature):
    start = text.index(signature)
    opening = text.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[opening:end]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--sanitized', action='store_true')
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    source = here.parents[2]
    workspace = source.parents[1]
    out = workspace / 'artifacts' / 'graphics'
    out.mkdir(parents=True, exist_ok=True)
    population = (source / 'game_sa/Population.cpp').read_text()
    vector = (source / 'game_sa/Core/Vector.cpp').read_text()
    manage = body(population, 'void CPopulation::ManagePed(')
    magnitude = body(vector, 'float CVector::Magnitude2D() const')
    digest = hashlib.sha256((manage + magnitude).encode()).hexdigest()
    scaffold = r'''
#include <algorithm>
#include <array>
using std::sqrt;
constexpr unsigned PED_TYPE_COP = 6;
constexpr int MODE_SNIPER = 7, MODE_SNIPER_RUNABOUT = 39, MODE_CAMERA = 46;
struct CVector {
    float x{}, y{}, z{};
    CVector operator-(const CVector& b) const { return {x-b.x,y-b.y,z-b.z}; }
    float Magnitude2D() const;
};
struct AttachedEntity { bool GetIsTypeVehicle() const { return true; } };
struct CPed {
    CVector Position;
    bool Player{}, Deletable{}, bInVehicle{}, Dead{}, bFadeOut{}, OnScreen{};
    bool bCullExtraFarAway{}, bDeadPedInFrontOfCar{};
    AttachedEntity* m_pAttachedTo{};
    AttachedEntity* m_VehDeadInFrontOf{};
    unsigned m_nPedType{}, m_nDeathTimeMS{}, m_nTimeTillWeNeedThisPed{};
    float m_fRemovalDistMultiplier{};
    unsigned char Alpha{};
    bool IsPlayer() const { return Player; }
    bool CanBeDeleted() const { return Deletable; }
    bool IsStateDead() const { return Dead; }
    const CVector& GetPosition() const { return Position; }
    bool GetIsOnScreen() const { return OnScreen; }
    CPed* GetRpClump() { return this; }
};
struct SourceCam {
    int m_nMode{};
    bool m_bLookingLeft{}, m_bLookingRight{}, m_bLookingBehind{};
};
struct SourceCamera {
    float m_fGenerationDistMultiplier{};
    SourceCam Cam;
    const SourceCam& GetActiveCamera() const { return Cam; }
};
static SourceCamera TheCamera;
static unsigned s_SourceTime;
static bool s_Frenzy, s_GangWar, s_Removed;
static float s_CreationMultiplier;
struct CTimer { static unsigned GetTimeInMS() { return s_SourceTime; } };
struct CDarkel { static bool FrenzyOnGoing() { return s_Frenzy; } };
struct CGangWars { static bool GangWarFightingGoingOn() { return s_GangWar; } };
struct CVisibilityPlugins { static unsigned char GetClumpAlpha(CPed* p) { return p->Alpha; } };
static bool IsPedTypeGang(unsigned type) { return type >= 7 && type <= 16; }
struct OriginalPopulation {
    static void ManagePed(CPed*, const CVector&);
    static void RemovePed(CPed*) { s_Removed = true; }
    static float PedCreationDistMultiplier() { return s_CreationMultiplier; }
};
'''
    compare = r'''
static std::size_t RunOriginalAmbientPedPolicyOracle() {
    static constexpr std::array<float, 17> positions{
        0, 1, 24.999998f, 25, 25.000002f, 29.999998f, 30, 30.000002f,
        54.499996f, 54.5f, 54.500004f, 64.99999f, 65, 65.00001f, 84.5f, 95, 200};
    static constexpr std::array<unsigned, 13> times{
        0, 7999, 8000, 8001, 14999, 15000, 15001, 29999, 30000, 30001,
        0xfffffff0u, 0xffffffffu, 100};
    static constexpr std::array<int, 5> modes{4, 7, 39, 46, 53};
    std::size_t checks = 0;
    unsigned bits = 1792;
    const auto draw = [&] { bits = bits * 1664525u + 1013904223u; return bits; };
    AttachedEntity vehicle;
    for (unsigned iteration = 0; iteration < 30'000; ++iteration) {
        NativeAmbientPedState state;
        NativeAmbientPedInput input;
        const auto flags = draw() >> 16u;
        state.Position = {positions[draw() % positions.size()], float(draw() % 5), 3};
        state.PedType = draw() % 32;
        state.Player = flags & 1;
        state.Deletable = !(flags & 2);
        state.InVehicle = flags & 4;
        state.AttachedToVehicle = flags & 8;
        state.Dead = flags & 16;
        state.FadeOut = flags & 32;
        state.ClumpAlpha = flags & 64 ? 0 : 255;
        state.CullExtraFar = flags & 128;
        state.DeadInFrontOfCar = flags & 256;
        state.HasDeadInFrontVehicle = flags & 512;
        state.DeathTimeMs = times[draw() % times.size()];
        state.NeededUntilMs = times[draw() % times.size()];
        state.RemovalDistanceMultiplier = float(draw() % 3) * 0.5f;
        input.GameMs = times[draw() % times.size()];
        input.CreationDistanceMultiplier = 1.0f + float(draw() % 3) * 0.25f;
        input.CameraGenerationMultiplier = float(draw() % 4) * 0.5f;
        input.CameraMode = modes[draw() % modes.size()];
        input.OnScreen = flags & 1024;
        input.Frenzy = flags & 2048;
        input.GangWarFighting = flags & 4096;
        input.LookingLeft = flags & 8192;
        input.LookingRight = flags & 16384;
        input.LookingBehind = flags & 32768;
        CPed original;
        original.Position = {state.Position[0],state.Position[1],state.Position[2]};
        original.m_nPedType = state.PedType;
        original.Player = state.Player;
        original.Deletable = state.Deletable;
        original.bInVehicle = state.InVehicle;
        original.m_pAttachedTo = state.AttachedToVehicle ? &vehicle : nullptr;
        original.Dead = state.Dead;
        original.bFadeOut = state.FadeOut;
        original.Alpha = state.ClumpAlpha;
        original.bCullExtraFarAway = state.CullExtraFar;
        original.bDeadPedInFrontOfCar = state.DeadInFrontOfCar;
        original.m_VehDeadInFrontOf = state.HasDeadInFrontVehicle ? &vehicle : nullptr;
        original.m_nDeathTimeMS = state.DeathTimeMs;
        original.m_nTimeTillWeNeedThisPed = state.NeededUntilMs;
        original.m_fRemovalDistMultiplier = state.RemovalDistanceMultiplier;
        original.OnScreen = input.OnScreen;
        s_SourceTime = input.GameMs;
        s_CreationMultiplier = input.CreationDistanceMultiplier;
        s_Frenzy = input.Frenzy;
        s_GangWar = input.GangWarFighting;
        s_Removed = false;
        TheCamera.m_fGenerationDistMultiplier = input.CameraGenerationMultiplier;
        TheCamera.Cam = {input.CameraMode, input.LookingLeft,input.LookingRight,input.LookingBehind};
        OriginalPopulation::ManagePed(&original, {0,0,0});
        NativeAmbientPedDecision decision;
        const auto status = NativeAmbientManagePed(state, input, decision);
        ++checks;
        if (status != NativeAmbientPedStatus::Ok || state.FadeOut != original.bFadeOut ||
            state.NeededUntilMs != original.m_nTimeTillWeNeedThisPed ||
            (decision.Action == NativeAmbientPedAction::Remove) != s_Removed) {
            std::fprintf(stderr, "source ManagePed mismatch case=%u status=%d\n", iteration, int(status));
            std::exit(1);
        }
    }
    return checks;
}
'''
    generated = scaffold + '\nfloat CVector::Magnitude2D() const\n' + magnitude
    generated += '\nvoid OriginalPopulation::ManagePed(CPed* ped, const CVector& playerPosn)\n' + manage
    generated += compare
    oracle = out / 'NativeAmbientPedPolicyOracle.inc'
    oracle.write_text(generated)
    stem = 'NativeAmbientPedPolicyProbe' + ('-sanitized' if args.sanitized else '')
    executable = out / stem
    command = ['g++', '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Wpedantic',
               '-fno-fast-math', '-ffp-contract=off',
               f'-DNATIVE_AMBIENT_PED_ORACLE="{oracle}"',
               str(here / 'NativeAmbientPedPolicy.cpp'), str(here / 'NativeAmbientPedPolicyProbe.cpp'),
               '-o', str(executable)]
    if args.sanitized:
        command[1:1] = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
    with (out / (stem + '-build.log')).open('w') as log:
        subprocess.run(command, check=True, stdout=log, stderr=subprocess.STDOUT)
    result = subprocess.run([str(executable)], text=True, capture_output=True)
    (out / (stem + '.log')).write_text(f'source-body-sha256={digest}\n' + result.stdout + result.stderr)
    print(result.stdout, end='')
    print(result.stderr, end='')
    result.check_returncode()
    print('source ManagePed/Vector bodies retained', digest, 'sanitized', args.sanitized)


if __name__ == '__main__':
    main()
