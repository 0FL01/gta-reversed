// Native source-COL residency approximation, not original CIplStore parity.
#pragma once

#include "app/platform/linux/NativeCarGenerators.h"
#include "app/platform/linux/NativeCollisionAssets.h"

#include <memory>
#include <span>
#include <string>
#include <vector>

struct NativeCarGeneratorResidentSource {
    std::string Key; // lowercase archive basename + ':' + entry
    std::string AssetSource; // exact owned registry provenance for activation
    std::uint8_t IplId{}; // assigned once from the complete sorted catalog
    std::size_t Records{};
    std::size_t RejectedRecords{}; // source model-range rejects, frozen at Initialize
    std::size_t AllocatableRecords() const { return Records - RejectedRecords; }
    bool operator==(const NativeCarGeneratorResidentSource&) const = default;
};

struct NativeCarGeneratorRemovalObligation {
    NativeCarGeneratorRef Generator;
    NativeVehicleRef Vehicle;
    std::int32_t ModelId{-1}; // positive fixed/cached model, -1 if not selected
    std::uint8_t IplId{};
    std::string SourceKey;
    // Release this generator's model keep-in-memory ownership, if any. The
    // registry cannot infer ownership from IDE definitions or ModelId alone.
    bool ReleaseModelOwnership{true};
    bool operator==(const NativeCarGeneratorRemovalObligation&) const = default;
};

// Read-only completion authority. Implemented by the real vehicle/model owner;
// never destroys objects during reconciliation. Complete means world/render/
// reference destruction and pool Release have happened, and model ownership
// has been released (or the owner proves this generator never acquired any).
// Missing authority stays PendingCleanup even for a -1 vehicle handle: model
// ownership cannot be established from registry metadata. The helper never
// releases a live vehicle reference; TEST ONLY metadata oracles belong in probes.
class NativeCarGeneratorResidencyCleanup {
public:
    virtual ~NativeCarGeneratorResidencyCleanup() = default;
    virtual bool Complete(const NativeCarGeneratorRemovalObligation& obligation) const noexcept = 0;
};

enum class NativeCarGeneratorResidencyStatus { Ready, PendingCleanup, Error };

struct NativeCarGeneratorResidencyResult {
    NativeCarGeneratorResidencyStatus Status{NativeCarGeneratorResidencyStatus::Error};
    std::string Detail;
    std::size_t BinarySources{}, GeneratorSources{}, Records{}, AddedSources{}, RemovedSources{};
    // Desired authored records = accepted allocations + explicit source rejects.
    // These describe the attempt; consult Status before treating it as committed.
    std::size_t AcceptedRecords{}, RejectedRecords{};
    bool Replay{};
    std::vector<NativeCarGeneratorRemovalObligation> Removals;
};

class NativeCarGeneratorResidency {
public:
    NativeCarGeneratorResidency() = default;
    NativeCarGeneratorResidency(const NativeCarGeneratorResidency&) = delete;
    NativeCarGeneratorResidency& operator=(const NativeCarGeneratorResidency&) = delete;

    // Before worker startup. Uses the full source catalog, including IPLs with
    // zero cargens; rejects basename collisions and more than 255 sources.
    // Registry must outlive this owner; binary IPL lifecycle belongs to it alone.
    bool Initialize(const NativeCarGenerators& registry, const NativeCollisionPopulation& catalog,
        std::string& error);

    // Main-thread world-publication transaction ONLY: call after selecting the
    // world packet for adoption, before initial Ready / active.swap(pending).
    // Ready must be followed immediately by a non-failing world publication.
    // Pending/Error retain the previous registry and committed snapshot;
    // result.Removals identifies generator/vehicle/model/source obligations.
    // Pending removals are serviced by the real runtime owner, then retried;
    // suppress spawning from those generators while cleanup is outstanding.
    // Generation is monotonic across startup and live worlds. A retry uses the
    // exact same immutable snapshot object; changed replay is an error.
    NativeCarGeneratorResidencyResult Reconcile(NativeCarGenerators& registry,
        std::uint64_t generation, std::shared_ptr<const NativeCollisionSnapshot> sourceCollision,
        std::uint32_t timeMs, const NativeCarGeneratorResidencyCleanup* cleanup = nullptr,
        const NativeVehiclePool* vehicles = nullptr);

    std::span<const NativeCarGeneratorResidentSource> Catalog() const { return m_Catalog; }
    std::span<const NativeCarGeneratorResidentSource> Active() const { return m_Active; }
    std::uint64_t Generation() const { return m_Generation; }
    const std::shared_ptr<const NativeCollisionSnapshot>& Snapshot() const { return m_Snapshot; }
    static bool CanonicalKey(std::string_view source, std::string& key, std::string& error);

private:
    const NativeCarGenerators* m_Registry{};
    std::vector<NativeCarGeneratorResidentSource> m_Catalog, m_Active;
    std::shared_ptr<const NativeCollisionSnapshot> m_Snapshot, m_AttemptSnapshot;
    std::uint64_t m_Generation{}, m_AttemptGeneration{};
};
