// Native, address-free port of the ordinary automobile transmission path.
// Source: tHandlingData::InitFromData (0x5BD830 loader),
// cHandlingDataMgr::ConvertDataToGameUnits (0x6F5080), and
// cTransmission::{InitGearRatios,CalculateDriveAcceleration} (0x6D0460/0x6D05E0).
// No cheats, bikes, aircraft or RCBANDIT special handling. No asset IO.
#pragma once

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>

struct NativeTransmission {
    struct Gear { float MaxVelocity=0,ChangeUpVelocity=0,ChangeDownVelocity=0; };
    struct State {
        std::uint8_t CurrentGear=1;
        float InertiaRatio=0,InertiaSmoother=0; // CAutomobile ctor initializes both to zero
    };
    struct Config {
        float MaxVelocityKmh=0,EngineAcceleration=0,EngineInertia=0,Drag=0;
        std::uint8_t NumberOfGears=0;
        char DriveType='4';
        std::uint32_t HandlingFlags=0;
    };
    // Original storage has six slots INCLUDING reverse: supported forward gears 1..5.
    std::array<Gear,6> Gears{};
    Config File{};
    float EngineAcceleration=0,MaxVelocity=0,MaxFlatVelocity=0,MaxReverseVelocity=0;
    static constexpr float UnitsPerSecond=50.0f;
    // source/app/app.h APP_MAX_FPS; reproduce the default frame-limited cadence
    // for the source's per-call inertia smoother, even with an uncapped renderer.
    static constexpr int UpdateRate=30;
    static constexpr double SecondsPerUpdate=1.0/UpdateRate;
    static constexpr float TimeStep=UnitsPerSecond/UpdateRate;

    void Initialize(Config config) {
        assert(config.NumberOfGears>=1 && config.NumberOfGears<Gears.size());
        assert(config.MaxVelocityKmh>0 && config.EngineAcceleration>0 && config.EngineInertia>0 && config.Drag>=0);
        assert(config.DriveType=='4' || config.DriveType=='F' || config.DriveType=='R');
        File=config;
        EngineAcceleration=config.EngineAcceleration*0.4f; // InitFromData, BEFORE conversion
        EngineAcceleration*=1.0f/(50.0f*50.0f);
        MaxVelocity=config.MaxVelocityKmh*(0.277778f/50.0f);
        const float limit=EngineAcceleration/6.0f;
        float velocity=MaxVelocity;
        while (velocity>0) {
            velocity-=0.01f;
            if (config.Drag>=0.01f) {
                if (velocity*velocity*(config.Drag/2.0f)/1000.0f<=limit) break;
            } else {
                const float reciprocal=1.0f/(velocity*velocity*config.Drag+1.0f);
                if ((reciprocal-1.0f)*-velocity<=limit) break;
            }
        }
        if (config.HandlingFlags&0x1000000u) { // VEHICLE_HANDLING_USE_MAXSP_LIMIT
            MaxFlatVelocity=velocity/1.2f;
            MaxReverseVelocity=std::min(-MaxFlatVelocity/4.0f,-0.2f);
        } else {
            MaxVelocity=velocity*1.2f;
            MaxFlatVelocity=velocity;
            MaxReverseVelocity=std::min(-velocity*0.3f,-0.2f);
        }
        EngineAcceleration/=static_cast<float>(DrivenWheels());
        Gears.fill({});
        const float half=0.5f*MaxVelocity/config.NumberOfGears;
        const float range=MaxVelocity-half;
        for (std::uint8_t i=1;i<=config.NumberOfGears;++i) {
            auto& gear=Gears[i]; const auto& previous=Gears[i-1];
            gear.MaxVelocity=static_cast<float>(i)*range/config.NumberOfGears+half;
            const float difference=gear.MaxVelocity-previous.MaxVelocity;
            if (i==config.NumberOfGears) gear.ChangeUpVelocity=MaxVelocity;
            else {
                Gears[i+1].ChangeDownVelocity=0.42f*difference+previous.MaxVelocity;
                gear.ChangeUpVelocity=0.6667f*difference+previous.MaxVelocity;
            }
        }
        Gears[0]={MaxReverseVelocity,-0.01f,MaxReverseVelocity};
        Gears[1].ChangeDownVelocity=-0.01f;
    }

    int DrivenWheels() const { return File.DriveType=='4' ? 4:2; }

    // Returns a PER DRIVEN WHEEL velocity increment in game units, already
    // multiplied by timeStep (CTimer units, seconds*50). Never multiply by dt again.
    // Inertia suppression is intentionally skipped on the call that changes gear.
    // gearChangeCount is unused in the original implementation too.
    float DriveAcceleration(float gas,State& state,float velocity,float timeStep,bool wheelsOnGround=true) const {
        assert(state.CurrentGear<=File.NumberOfGears && timeStep>0);
        if (velocity<MaxReverseVelocity) return 0;
        bool inertia=true;
        while (velocity<=MaxVelocity) {
            const auto& gear=Gears[state.CurrentGear];
            bool accelerate=false,down=false;
            if (velocity>gear.ChangeUpVelocity) {
                if (state.CurrentGear==0 && gas<=0) accelerate=true;
                else ++state.CurrentGear;
            } else {
                if (velocity>=gear.ChangeDownVelocity || state.CurrentGear==0 || (state.CurrentGear==1 && gas>=0)) accelerate=true;
                down=true;
            }
            if (accelerate) {
                float multiplier;
                if (File.NumberOfGears==1) multiplier=1.0f;
                else if (state.CurrentGear) {
                    float number=1.0f-(static_cast<float>(state.CurrentGear)-1.0f)/(static_cast<float>(File.NumberOfGears)-1.0f);
                    number*=number;
                    multiplier=number*((File.HandlingFlags&1u) ? 5.0f:(File.HandlingFlags&2u) ? 4.0f:3.0f)+1.0f;
                } else multiplier=4.5f;
                float acceleration=multiplier*EngineAcceleration*0.4f*gas*timeStep;
                if (inertia) {
                    if (wheelsOnGround) {
                        const float change=MaxVelocity/static_cast<float>(File.NumberOfGears)*(1.0f/3.0f);
                        float numerator,denominator;
                        if (state.CurrentGear==1) {
                            numerator=change+velocity; denominator=change+Gears[1].ChangeUpVelocity;
                        } else if (state.CurrentGear) {
                            numerator=velocity-gear.ChangeDownVelocity; denominator=gear.ChangeUpVelocity-gear.ChangeDownVelocity;
                        } else {
                            numerator=change-velocity; denominator=change-Gears[0].ChangeDownVelocity;
                        }
                        const float ratio=numerator/denominator;
                        const float factor=std::clamp(1.0f-(ratio-state.InertiaRatio)*File.EngineInertia,0.1f,1.0f);
                        state.InertiaRatio=ratio;
                        state.InertiaSmoother=factor*(1.0f-0.85f)+0.85f*state.InertiaSmoother;
                        acceleration*=state.InertiaSmoother;
                    } else {
                        state.InertiaRatio=std::min(state.InertiaRatio+std::abs(gas)/File.EngineInertia*timeStep*0.1f,1.0f);
                        state.InertiaSmoother=0.1f;
                    }
                }
                float excess;
                if (gear.MaxVelocity>=0 || velocity>=gear.MaxVelocity) {
                    if (gear.MaxVelocity<=0 || velocity<=gear.MaxVelocity) return acceleration;
                    excess=velocity-gear.MaxVelocity;
                } else excess=gear.MaxVelocity-velocity;
                return acceleration*(1.0f-std::min(excess/0.05f,1.0f));
            }
            if (down) --state.CurrentGear;
            inertia=false;
        }
        return 0;
    }

    // CPhysical::ApplyAirResistance (0x544C40), flat forward motion only;
    // CVehicle::GetDefaultAirResistance supplies the drag coefficient.
    float AirResistance(float speedMs,float timeStep) const {
        const float resistance=File.Drag<=0.01f ? File.Drag:File.Drag/1000.0f/2.0f;
        return speedMs*std::pow(std::max(0.0f,1.0f-std::abs(speedMs)/UnitsPerSecond*resistance),timeStep);
    }
};
