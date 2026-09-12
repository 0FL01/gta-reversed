#pragma once

#include "NativeSourcePedResponse.h"
#include "NativeSourceGround.h"

#include <string>
#include <vector>

enum class NativeSourcePedWorldStatus {
    Ok, NotLoaded, PedNotFound, DuplicatePed, InvalidInput, Unsupported,
    Overflow, SurfaceUnavailable, OutsideCoverage, ControlRequired, ControlOutstanding,
};

struct NativeSourcePedWorldPed {
    std::uint64_t Identity{};
    NativeSourcePhysicalState Physical;
    NativeSourcePedContactState Contact;
    std::array<NativeCollisionVector, 3> Basis{{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
    bool HasPlayerData{};
    bool StandingOnEntity{};
    float AirResistance = 1.0f / 175.0f;
    bool ControlPrepared{}; // owner sequencing bit; callers cannot pre-seed it
    bool operator==(const NativeSourcePedWorldPed&) const = default;
};

struct NativeSourcePedWorldStep {
    NativeSourcePedCollisionStepPlan Plan;
    NativeCollisionVector Start{}, End{};
    float MovingDistance{};
    std::size_t CollisionChecks{}, StaticQueries{}, DynamicQueries{};
    std::size_t Supports{}, Contacts{}, Applied{}, Friction{}, Reports{};
    bool Blocked{}, SafePosition{};
    bool operator==(const NativeSourcePedWorldStep&) const = default;
};

// One source-normal-sector collision owner. Load requires the existing world
// producer's complete, qualified and ordered candidate snapshot; no class,
// membership, ignored-entity or transform defaults are invented here.
class NativeSourcePedWorld {
public:
    bool Load(const NativeSourceGroundSnapshot&, std::string& error);
    NativeSourcePedWorldStatus AddPed(const NativeSourcePedWorldPed&);
    NativeSourcePedWorldStatus RemovePed(std::uint64_t identity);
    NativeSourcePedWorldStatus BeginControl(std::uint64_t identity, float timeStep);
    // Requires one successful BeginControl and consumes it only on a completed
    // collision step. Typed query failures retain the prepared state for retry.
    NativeSourcePedWorldStatus StepCollision(std::uint64_t identity, float timeStep,
        const NativeSourceSurfaces&, NativeSourcePedWorldStep& out);
    NativeSourcePedWorldStatus Ped(std::uint64_t identity, NativeSourcePedWorldPed& out) const;
    std::size_t StaticCount() const noexcept { return m_Statics.size(); }
    std::size_t PedCount() const noexcept { return m_Peds.size(); }
    std::uint64_t WorldGeneration() const noexcept { return m_WorldGeneration; }

private:
    struct StaticTarget {
        NativePlacementIdentity Identity;
        std::shared_ptr<const NativeCollisionModel> Model;
        NativeSourceGroundTransform Transform;
        std::uint64_t Ordinal{};
    };
    bool m_Loaded{};
    std::uint64_t m_WorldGeneration{}, m_MetadataRevision{};
    std::array<float, 2> m_MinXY{}, m_MaxXY{};
    std::vector<StaticTarget> m_Statics;
    std::vector<NativeSourcePedWorldPed> m_Peds;
};
