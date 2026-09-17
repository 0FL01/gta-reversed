// StreamPager: R6b streaming pager over the parsed IPL world.
// Parses IDE/IPL lists from data/*.dat (same files/order as SceneShot),
// indexes filtered static instances (interior==0, no LOD*, no anim/skinned)
// in a deterministic x/y grid (cell 300m), and serves a bounded camera
// window: instances within radius R=300m, cells evicted past R+hysteresis.
// Every rendered triangle still comes from DFF bytes via librw (CachedModel);
// the pager only bounds *which* IPL records are resident. No GL here.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "app/platform/linux/WorldShot.h"
#include "app/platform/linux/NativeCollisionAssets.h"

struct E2ELoadInfo {
    int binaryIplFiles = 0;
    int binaryInstances = 0;
    int iplTotal = 0; // all parsed inst records (unfiltered)
    int iplKept = 0; // static outdoor instances entering the grid
    int ideModels = 0;
    int ideFiles = 0;
    int iplFiles = 0;
};

struct StreamPagerOptions {
    // Offline fixtures deliberately retain their original small text-IPL slice,
    // unmasked text Interior/zero Flags, and historical transform convention.
    // Runtime true preserves the authored text type word in Flags and exports
    // Interior = Flags & 255, matching the binary IPL/source loader contract.
    bool includeStreamed = false;
    float radius = 300.0f;
    int maxInstances = 80;
};

struct E2EPagerFrame {
    int instances = 0; // placed instances in the returned scene
    int modelsUnique = 0; // distinct DFF models referenced by them
    int tris = 0;
    int verts = 0;
    int activeCells = 0;
    int loadedCells = 0; // cells paged in on this update
    int evictedCells = 0; // cells paged out on this update
    int cacheModels = 0; // resident DFF models after update
    int texDicts = 0; // resident non-empty TXDs after update
    int fallback = 0; // 1 when the R-window was too sparse and widened
    int evictedShown = 0; // entries filled below (<= 16)
    int evictedCX[16];
    int evictedCY[16];
    int evictedDist[16]; // rect distance (m) that crossed R+hysteresis
};

// Loads and indexes the world relative to gameDir. False => err message.
bool StreamPager_Init(const char* gameDir, E2ELoadInfo& info, char* err, std::size_t errSize,
                      const StreamPagerOptions& options = {});
// Rebuilds `scene` (world-space soup, deterministic order) around the
// camera ground position. False => err message (never silent world-ok).
bool StreamPager_Update(float camX, float camY, float camZ, WorldShotScene& scene, E2EPagerFrame& frame,
                        char* err, std::size_t errSize,
                        const std::shared_ptr<const NativePlacementOverrides>& overrides = {},
                        std::vector<NativePlacementIdentity>* rendered = nullptr);
// Overrides replace exact source rows before culling/placement. Optional rendered
// identities correspond one-to-one with scene.meshes (owned diagnostic output).
void StreamPager_Counters(int& sectorsLoaded, int& sectorsEvicted, int& modelsPeak, int& trisPeak);
void StreamPager_Shutdown();
// COPY of the actual pre-render-filter IPL population, including binary source
// and record provenance. Requires includeStreamed=true: full text/binary Flags
// and low-byte Interior. Legacy offline mode deliberately rejects this export.
// Call under pager ownership; result survives shutdown.
bool StreamPager_CollisionPopulation(NativeCollisionPopulation& out, std::string& error);
// Read-only complete IDE identity retained by the sole pager startup parser.
bool StreamPager_KnownModelId(int modelId, std::string* modelName = nullptr);
bool StreamPager_KnownModelName(std::string_view modelName, int* modelId = nullptr);
bool StreamPager_KnownModelIdentity(int modelId, std::string& modelName, std::string& textureName);
// Optional single-chain LOD supplement (P1-A04): real paired render for one
// catalog-validated child/parent pair (no hardcoded IDs here).
// Called after Init and before the first Update. Validates exact full
// population identities, child Lod binding to the parent record, parent
// LOD provenance/filtering and includeStreamed; stores one optional owned
// configuration. Shutdown resets to disabled. Default disabled preserves the
// native/offline path exactly.
bool StreamPager_ConfigureLodSupplement(const NativePlacementIdentity& child,
                                        const NativePlacementIdentity& parent,
                                        std::string& error);
// P1-A07 catalog-backed selected residency (opt-in beside unchanged capped
// diagnostics). Matches visible+hidden identities uniquely 1:1 against the
// full population and renders exactly visible-then-hidden via the shared
// TXD-lineage/DFF-cache/EmitPlacedMesh/eviction stages (no second loader).
// Streamed only: no radius/cell fallback, count truncation, LOD-prefix or
// interior skip. Unknown/duplicate identity, anim/Clump, missing, failed,
// skinned/empty DFF or incomplete TXD chain fails the WHOLE candidate with
// the specific identity in the message; never silently skips. A configured
// A04 pair keeps all-or-neither when its child is selected, without
// duplicate emission when the parent is already the expected hidden target.
// Both lists drive exact want models/TXDs/caches/eviction; rendered (when
// provided) is exact one-to-one visible-then-hidden with scene.meshes.
// Failure publishes no half scene. Pure std, no Godot/escaped pointers.
bool StreamPager_UpdateSelected(const std::vector<NativePlacementIdentity>& visible,
                                const std::vector<NativePlacementIdentity>& hiddenTargets,
                                WorldShotScene& scene, E2EPagerFrame& frame, char* err,
                                std::size_t errSize,
                                std::vector<NativePlacementIdentity>* rendered = nullptr);
