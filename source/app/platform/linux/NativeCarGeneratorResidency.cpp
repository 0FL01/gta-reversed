#include "app/platform/linux/NativeCarGeneratorResidency.h"

#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>

namespace {
void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

std::string Normalize(std::string_view source) {
    std::string result(source);
    for (auto& c : result) {
        if (c == '\\') c = '/';
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    }
    return result;
}

bool Contains(std::span<const NativeCarGeneratorResidentSource> sources, std::uint8_t id) {
    return std::ranges::any_of(sources, [=](const auto& source) { return source.IplId == id; });
}

void ValidateSource(const NativeCarGenerators& registry, const NativeCarGeneratorResidentSource& source) {
    std::size_t accepted = 0, rejected = 0;
    for (const auto& asset : registry.AssetRecords()) {
        if (asset.Provenance.Source != source.AssetSource) continue;
        Require(asset.RegistrationIplId == source.IplId, "active binary asset registration IPL-ID mismatch");
        if (asset.RegistrationStatus == NativeCarGeneratorRegistrationStatus::RejectedModelRange) {
            Require(asset.RegistryIndex == -1 && !NativeCarGenerators::IsSourceModelInRange(asset.Authored.ModelId),
                "source-rejected binary asset has allocation or accepted model");
            ++rejected;
        } else {
            const auto* entry = registry.Resolve({asset.RegistryIndex});
            Require(asset.RegistrationStatus == NativeCarGeneratorRegistrationStatus::Registered &&
                NativeCarGenerators::IsSourceModelInRange(asset.Authored.ModelId) && entry &&
                entry->IplId == source.IplId && entry->Provenance == asset.Provenance,
                "active binary asset allocation/provenance mismatch");
            ++accepted;
        }
    }
    Require(accepted == source.AllocatableRecords() && rejected == source.RejectedRecords,
        "incomplete staged IPL activation or changed source rejection census; prior registry retained");
}

} // namespace

bool NativeCarGeneratorResidency::CanonicalKey(std::string_view source, std::string& key, std::string& error) {
    const auto normalized = Normalize(source);
    const auto colon = normalized.rfind(':');
    if (colon == std::string::npos || normalized.find('\0') != std::string::npos) {
        error = "binary IPL source requires archive:entry";
        return false;
    }
    const auto slash = normalized.rfind('/', colon);
    const auto archive = normalized.substr(slash == std::string::npos ? 0 : slash + 1,
        colon - (slash == std::string::npos ? 0 : slash + 1));
    const auto entry = normalized.substr(colon + 1);
    if (archive.size() <= 4 || !archive.ends_with(".img") || archive.find(':') != std::string::npos ||
        entry.size() <= 4 || !entry.ends_with(".ipl") || entry.find_first_of("/:") != std::string::npos) {
        error = "invalid binary IPL archive basename or entry";
        return false;
    }
    key = archive + ':' + entry;
    error.clear();
    return true;
}

bool NativeCarGeneratorResidency::Initialize(const NativeCarGenerators& registry,
    const NativeCollisionPopulation& catalog, std::string& error) try {
    Require(!m_Registry, "residency catalog already initialized");
    Require(registry.Census().StartupOrderProven && catalog.IncludesStreamed,
        "residency requires loaded registry and complete streamed source catalog");
    std::map<std::string, std::string> sources;
    for (const auto& placement : catalog.Instances) {
        if (!placement.Binary) continue;
        std::string key;
        if (!CanonicalKey(placement.Ipl, key, error)) return false;
        const auto normalized = Normalize(placement.Ipl);
        const auto [it, inserted] = sources.emplace(key, normalized);
        Require(inserted || it->second == normalized, "source catalog archive-basename collision");
    }
    Require(!sources.empty() && sources.size() <= std::numeric_limits<std::uint8_t>::max(),
        "native IPL ID capacity exceeded or source catalog empty");
    Require(sources.size() == registry.Census().BinaryIplFiles,
        "source placement catalog does not cover the complete registry binary IPL catalog");
    std::vector<NativeCarGeneratorResidentSource> prepared;
    for (const auto& [key, raw] : sources) {
        (void)raw;
        prepared.push_back({key, {}, std::uint8_t(prepared.size() + 1), 0});
    }
    for (const auto& asset : registry.AssetRecords()) {
        if (asset.Provenance.Kind != NativeCarGeneratorSourceKind::BinaryIpl) continue;
        Require(asset.RegistryIndex == -1 && asset.RegistrationStatus == NativeCarGeneratorRegistrationStatus::NotAttempted,
            "initialize residency before binary activation");
        std::string key;
        if (!CanonicalKey(asset.Provenance.Source, key, error)) return false;
        auto it = std::ranges::find(prepared, key, &NativeCarGeneratorResidentSource::Key);
        Require(it != prepared.end(), "generator source absent from full source catalog");
        Require(it->AssetSource.empty() || it->AssetSource == asset.Provenance.Source,
            "generator catalog canonical source collision");
        it->AssetSource = asset.Provenance.Source;
        ++it->Records;
        it->RejectedRecords += !NativeCarGenerators::IsSourceModelInRange(asset.Authored.ModelId);
    }
    for (const auto& entry : registry.Entries()) {
        Require(!entry.Used || entry.IplId == 0, "native IPL IDs already occupied before residency initialization");
    }
    m_Catalog.swap(prepared);
    m_Registry = &registry;
    error.clear();
    return true;
} catch (const std::exception& exception) {
    error = exception.what();
    return false;
}

NativeCarGeneratorResidencyResult NativeCarGeneratorResidency::Reconcile(NativeCarGenerators& registry,
    std::uint64_t generation, std::shared_ptr<const NativeCollisionSnapshot> sourceCollision,
    std::uint32_t timeMs, const NativeCarGeneratorResidencyCleanup* cleanup, const NativeVehiclePool* vehicles) {
    NativeCarGeneratorResidencyResult result;
    try {
        Require(m_Registry == &registry, "residency registry owner mismatch or uninitialized");
        Require(bool(sourceCollision) && generation != 0, "residency requires immutable source-COL and nonzero generation");
        Require(generation >= m_AttemptGeneration, "stale world generation");
        Require(generation != m_AttemptGeneration || sourceCollision == m_AttemptSnapshot,
            "changed source-COL replay for world generation");
        // Attempts pin the immutable identity even on failure/Pending; the
        // committed snapshot and active set below change only with registry swap.
        m_AttemptGeneration = generation;
        m_AttemptSnapshot = sourceCollision;
        std::set<std::string> keys;
        for (const auto& instance : sourceCollision->Instances) {
            if (!instance.Placement.Binary) continue;
            std::string key, error;
            if (!CanonicalKey(instance.Placement.Ipl, key, error)) throw std::runtime_error(error);
            Require(std::ranges::find(m_Catalog, key, &NativeCarGeneratorResidentSource::Key) != m_Catalog.end(),
                "committed source-COL contains unknown binary source");
            keys.insert(std::move(key));
        }
        std::vector<NativeCarGeneratorResidentSource> desired;
        for (const auto& source : m_Catalog) {
            if (!source.Records || !keys.contains(source.Key)) continue;
            desired.push_back(source);
            result.Records += source.Records;
            result.AcceptedRecords += source.AllocatableRecords();
            result.RejectedRecords += source.RejectedRecords;
        }
        result.BinarySources = keys.size();
        result.GeneratorSources = desired.size();

        // Detect foreign IPL-ID use rather than removing SCM/static definitions
        // or assigning an existing ID to a different active source.
        std::size_t activeRecords = 0;
        for (const auto& source : m_Active) activeRecords += source.AllocatableRecords();
        std::size_t observedRecords = 0;
        for (std::size_t slot = 0; slot < registry.Entries().size(); ++slot) {
            const auto& entry = registry.Entries()[slot];
            if (!entry.Used || entry.IplId == 0) continue;
            const auto it = std::ranges::find(m_Active, entry.IplId, &NativeCarGeneratorResidentSource::IplId);
            Require(it != m_Active.end() && entry.Provenance.Kind == NativeCarGeneratorSourceKind::BinaryIpl &&
                entry.Provenance.Source == it->AssetSource, "foreign or colliding registry IPL ID");
            ++observedRecords;
            if (!Contains(desired, entry.IplId)) {
                Require(entry.ModelId != std::numeric_limits<std::int32_t>::min(), "invalid cached generator model");
                result.Removals.push_back({{std::int32_t(slot)}, entry.Vehicle, entry.ModelId < -1 ? -entry.ModelId : entry.ModelId,
                    entry.IplId, it->Key, true});
            }
        }
        Require(observedRecords == activeRecords, "active source registry was modified outside residency owner");
        for (const auto& asset : registry.AssetRecords()) {
            if (asset.Provenance.Kind != NativeCarGeneratorSourceKind::BinaryIpl) continue;
            const auto source = std::ranges::find(m_Active, asset.Provenance.Source, &NativeCarGeneratorResidentSource::AssetSource);
            if (source == m_Active.end()) {
                Require(asset.RegistryIndex == -1 && asset.RegistrationStatus == NativeCarGeneratorRegistrationStatus::NotAttempted &&
                    asset.RegistrationIplId == 0, "nonresident binary asset has a foreign registry allocation or registration");
            }
        }
        for (const auto& source : m_Active) ValidateSource(registry, source);
        if (generation == m_Generation) {
            Require(desired == m_Active, "changed residency replay");
            result.Status = NativeCarGeneratorResidencyStatus::Ready;
            result.Replay = true;
            return result;
        }
        std::size_t additions = 0;
        for (const auto& source : desired) {
            if (Contains(m_Active, source.IplId)) continue;
            ++result.AddedSources;
            additions += source.AllocatableRecords();
        }
        for (const auto& source : m_Active) {
            if (!Contains(desired, source.IplId)) ++result.RemovedSources;
        }
        Require(registry.Census().Registered - result.Removals.size() + additions <= NativeCarGenerators::Capacity,
            "car-generator residency capacity exceeded; prior registry retained");

        const auto revision = registry.Revision();
        auto staged = std::make_unique<NativeCarGenerators>(registry);
        for (const auto& source : m_Active) {
            if (!Contains(desired, source.IplId)) {
                Require(staged->RemoveIpl(source.IplId) == source.AllocatableRecords(), "staged removal count mismatch");
            }
        }
        for (const auto& source : desired) {
            if (Contains(m_Active, source.IplId)) continue;
            std::string error;
            if (!staged->ActivateStreamedIpl(source.AssetSource, source.IplId, timeMs, error)) {
                throw std::runtime_error(error);
            }
            // Every authored row must have an exact allocation or the expected
            // typed source rejection; capacity/conversion failures stay errors.
            const auto count = std::ranges::count_if(staged->Entries(), [&](const auto& entry) {
                return entry.Used && entry.IplId == source.IplId && entry.Provenance.Source == source.AssetSource;
            });
            Require(std::size_t(count) == source.AllocatableRecords(), "incomplete staged IPL activation; prior registry retained");
            ValidateSource(*staged, source);
        }
        for (const auto& removal : result.Removals) {
            if (!cleanup || !cleanup->Complete(removal) ||
                (removal.Vehicle.Value >= 0 && (!vehicles || vehicles->Resolve(removal.Vehicle)))) {
                result.Status = NativeCarGeneratorResidencyStatus::PendingCleanup;
                result.Detail = "real vehicle/world/reference and generator model-ownership cleanup required";
                return result;
            }
        }
        Require(registry.Revision() == revision, "registry changed during reconciliation");
        registry.Swap(*staged);
        // No allocation or other fallible work after the registry swap.
        m_Active.swap(desired);
        m_Snapshot.swap(sourceCollision);
        m_Generation = generation;
        result.Status = NativeCarGeneratorResidencyStatus::Ready;
        return result;
    } catch (const std::exception& exception) {
        result.Status = NativeCarGeneratorResidencyStatus::Error;
        result.Detail = exception.what();
        return result;
    }
}
