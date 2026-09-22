#pragma once

#include "app/platform/linux/NativeLodCatalog.h"

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

struct NativePathAreaResidency {
    std::uint8_t Area{};
    std::uint32_t Nodes{}, VehicleNodes{}, PedNodes{}, CarPathLinks{}, Addresses{};
    std::uint64_t Bytes{}, Fingerprint{};
    bool operator==(const NativePathAreaResidency&) const = default;
};

class NativePathResidencyCatalog {
public:
    bool LoadBeforeWorker(const char* gameDir, std::string& error);
    bool Select(float x, float y, float radius, std::vector<NativePathAreaResidency>& out,
        std::string& error) const;
    std::span<const NativePathAreaResidency> Areas() const { return m_Areas; }

private:
    std::vector<NativePathAreaResidency> m_Areas;
};

struct NativeDynamicWorldRef {
    std::uint32_t Value{};
    bool operator==(const NativeDynamicWorldRef&) const = default;
};

struct NativeDynamicWorldEntity {
    NativeDynamicWorldRef Reference;
    std::int32_t ModelId = -1;
    NativeCollisionVector Position{};
    std::uint8_t Area{};
    bool InWorld{};
    bool operator==(const NativeDynamicWorldEntity&) const = default;
};

struct NativeWorldResidencyCandidate {
    std::uint64_t Generation{};
    int Area{};
    float X{}, Y{}, Radius{};
    NativeCatalogResidency Selection;
    std::vector<NativePlacementIdentity> Rendered;
    std::shared_ptr<const NativeCollisionSnapshot> Collision;
    std::vector<NativePathAreaResidency> Paths;
};

struct NativeWorldResidencySnapshot {
    std::uint64_t Generation{}, Revision{};
    int Area{};
    float X{}, Y{}, Radius{};
    std::vector<NativePlacementIdentity> Visible, HiddenTargets, Rendered, CollisionIdentities;
    std::vector<NativePathAreaResidency> Paths;
    std::vector<NativeDynamicWorldEntity> Dynamic;
    std::shared_ptr<const NativeCollisionSnapshot> Collision;
    bool PairedRenderCollision{}, CompletePlacementIdentity{}, PathSearchAuthority{};
    bool operator==(const NativeWorldResidencySnapshot&) const = default;
};

class NativeWorldResidency {
public:
    static bool Prepare(const NativeLodCatalog& catalog, const NativeCollisionAssets& assets,
        const NativeCollisionPopulation& population, const NativePathResidencyCatalog& paths,
        std::uint64_t generation, float x, float y, float radius, int area,
        std::span<const NativePlacementIdentity> rendered, NativeWorldResidencyCandidate& out,
        std::string& error);

    bool SpawnDynamic(std::int32_t modelId, NativeCollisionVector position, std::uint8_t area,
        NativeDynamicWorldRef& out, std::string& error);
    bool MoveDynamic(NativeDynamicWorldRef, NativeCollisionVector position, std::uint8_t area,
        std::string& error);
    bool RemoveDynamic(NativeDynamicWorldRef, std::string& error);
    bool Adopt(const NativeWorldResidencyCandidate&, std::string& error);

    const std::shared_ptr<const NativeWorldResidencySnapshot>& LastCommitted() const { return m_Published; }
    const NativeDynamicWorldEntity* Resolve(NativeDynamicWorldRef) const noexcept;
    static constexpr bool PathSearchAuthority = false;

private:
    struct DynamicSlot {
        std::uint8_t Generation{};
        bool Alive{};
        NativeDynamicWorldEntity State;
    };
    std::array<DynamicSlot, 256> m_Dynamic{};
    std::uint64_t m_Revision{};
    std::shared_ptr<const NativeWorldResidencySnapshot> m_Published;
};
