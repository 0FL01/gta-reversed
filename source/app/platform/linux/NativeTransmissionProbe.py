#!/usr/bin/python3
"""Compile the native port against an oracle extracted from the upstream sources.

docker exec mad-sa-graphics-build python3 /workspace/gta-reversed/source/app/platform/linux/NativeTransmissionProbe.py
Only address-bound scratch locals are replaced; original arithmetic/branches are
compiled verbatim. Generated source/binary/logs live in artifacts, never assets.
"""
import pathlib
import re
import subprocess

workspace = pathlib.Path('/workspace')
source = workspace / 'gta-reversed/source'
output = workspace / 'artifacts/graphics'
output.mkdir(parents=True, exist_ok=True)


def function(path, signature):
    text = path.read_text()
    begin = text.index(signature)
    brace = text.index('{', begin)
    depth = 1
    end = brace + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    body = text[begin:end]
    # The original static scratch storage is irrelevant to this single-thread
    # numerical oracle. All such locals are assigned before use in the source.
    return re.sub(r'static auto& (\w+) = StaticRef<(.+?)>\(0x[0-9A-Fa-f]+\);', r'\2 \1{};', body)


transmission = source / 'game_sa/cTransmission.cpp'
handling = source / 'game_sa/cHandlingDataMgr.cpp'
header = r'''
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
namespace oracle {
using uint8 = unsigned char;
using uint32 = unsigned;
template<class T> T sq(T x) { return x*x; }
struct CTimer { static inline float Step=1; static float GetTimeStep() { return Step; } };
enum { VEHICLE_HANDLING_1G_BOOST=1, VEHICLE_HANDLING_2G_BOOST=2,
       CHEAT_HANDLING_NONE=0, CHEAT_HANDLING_PERFECT=1, CHEAT_HANDLING_NITROS=2,
       VT_RCBANDIT=1, VT_BIKE=162, VT_FREEWAY=175 };
constexpr float TRANSMISSION_AI_CHEAT_MULT=1.2f, TRANSMISSION_NITROS_MULT=2,
    TRANSMISSION_AI_CHEAT_INERTIA_MULT=0.75f, TRANSMISSION_NITROS_INERTIA_MULT=0.5f,
    TRANSMISSION_SMOOTHER_FRAC=0.85f, TRANSMISSION_FREE_ACCELERATION=0.1f,
    ACCEL_CONST=1.f/(50.f*50.f), VELOCITY_CONST=0.277778f/50.f;
struct tTransmissionGear { float MaxVelocity{},ChangeUpVelocity{},ChangeDownVelocity{}; };
struct cTransmission {
    std::array<tTransmissionGear,6> m_aGears{};
    uint8 m_nNumberOfGears{},m_nDriveType{};
    uint32 m_handlingFlags{};
    float m_EngineAcceleration{},m_EngineInertia{},m_MaxVelocity{},m_MaxFlatVelocity{},m_MaxReverseVelocity{},m_Velocity{};
    void InitGearRatios();
    float CalculateDriveAcceleration(const float&,uint8&,float&,float&,float*,float*,uint8,uint8);
};
struct tHandlingData {
    cTransmission Transmission;
    float m_fBrakeDeceleration=6.2f,m_fMass=1700,m_fMassRecpr{},m_fBuoyancyConstant{},m_fCollisionDamageMultiplier=1,m_fDragMult{};
    int m_nPercentSubmerged=85,m_nVehicleId=0;
    bool m_bUseMaxspLimit=false;
    cTransmission& GetTransmission() { return Transmission; }
};
struct cHandlingDataMgr { void ConvertDataToGameUnits(tHandlingData*); };
'''
header += function(transmission, 'void cTransmission::InitGearRatios()') + '\n'
header += function(transmission, 'float cTransmission::CalculateDriveAcceleration(') + '\n'
header += function(handling, 'void cHandlingDataMgr::ConvertDataToGameUnits(') + '\n}\n'
# Ensure the oracle's preconversion below still matches the real loader.
assert 'm_transmissionData.m_EngineAcceleration *= 0.4f;' in handling.read_text()
assert re.search(r'#define\s+APP_MAX_FPS\s+30\b', (source / 'app/app.h').read_text())
(output / 'NativeTransmissionOracle.h').write_text(header)
binary = output / 'NativeTransmissionProbe'
subprocess.run(['c++', '-std=c++20', '-O2', '-Wall', '-Wextra', '-Wno-unused-parameter',
                '-Wno-parentheses', '-I', str(source), '-I', str(output),
                str(source / 'app/platform/linux/NativeTransmissionProbe.cpp'), '-o', str(binary)], check=True)
result = subprocess.run([str(binary), '/game/data/handling.cfg'], text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
(output / 'NativeTransmissionProbe.log').write_text(result.stdout)
print(result.stdout, end='')
result.check_returncode()
