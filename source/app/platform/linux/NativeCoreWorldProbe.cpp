// P2-A04 isolated world probe: existing NativeWorldGround publication matrix
// over an explicitly synthetic isolated population bound to real 3991 COL
// bytes. Links ONLY sa_core (POSIX adapter); no StreamPager, GL, SDL, Godot,
// RW or new parser/epoch/counter API. Uses bool/error branches so Release
// (NDEBUG) still executes every call; no assert side effects. No asset
// copies, no files written. Whole-world ground is NOT claimed: this
// synthetic single-building world is isolated, not the original world.
#include "NativeCollisionAssets.h"
#include "NativeSourceGround.h"
#include "NativeWorldEntityInfo.h"
#include "NativeWorldGround.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

namespace {

using Status = NativeSourceGroundStatus;
using Reason = NativeSourceGroundReason;
using V = NativeCollisionVector;

size_t s_Checks = 0;

void Check(bool ok, const char* message) {
    ++s_Checks;
    if (!ok) {
        std::fprintf(stderr, "sa-core-world FAIL: %s\n", message);
        std::exit(1);
    }
}

void FailWith(const char* message, const std::string& error) {
    std::fprintf(stderr, "sa-core-world FAIL: %s: %s\n", message, error.c_str());
    std::exit(1);
}

V WorldPoint(const NativeSourceGroundTransform& t, V p) {
    V out{};
    for (size_t j = 0; j < 3; ++j) {
        out[j] = t.Basis[0][j] * p[0] + t.Basis[1][j] * p[1] + t.Basis[2][j] * p[2] + t.Position[j];
    }
    return out;
}

bool FiniteV(V v) {
    for (auto f : v) {
        if (!std::isfinite(f)) {
            return false;
        }
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2 || !argv[1] || !argv[1][0]) {
        std::fprintf(stderr, "usage: sa_core_world_probe <owned-game-dir>\n");
        return 1;
    }
    const char* gameDir = argv[1];
    std::string error;

    // Pre-load: before any successful Load every query is Unsupported.
    {
        NativeCollisionAssets fresh;
        const auto ready = fresh.LookupModel("GSFreeway7_LAn");
        Check(ready.Status == NativeCollisionModelStatus::Unsupported && !ready.Model &&
                  !ready.Error.empty() && !ready.TimeShared,
              "isolated synthetic pre-load GSFreeway7_LAn must be Unsupported");
        const auto empty = fresh.LookupModel("");
        Check(empty.Status == NativeCollisionModelStatus::Unsupported && !empty.Model,
              "isolated synthetic pre-load empty must be Unsupported");
        const std::string nul("bad\0name", 8);
        const auto bad = fresh.LookupModel(nul);
        Check(bad.Status == NativeCollisionModelStatus::Unsupported && !bad.Model,
              "isolated synthetic pre-load NUL must be Unsupported");
        const auto parent = fresh.LookupModel("LODGSFreeway7_LAn");
        Check(parent.Status == NativeCollisionModelStatus::Unsupported,
              "isolated synthetic pre-load parent must be Unsupported");
        std::puts("PRELOAD isolated synthetic Unsupported pass (no files)");
    }

    // Owned synthetic isolated population: single real-model placement.
    // IncludesStreamed certifies this isolated fixture's own completeness,
    // never whole-world retail parity.
    NativeCollisionPopulation synthPop;
    synthPop.Models[3991] = {"gsfreeway7_lan", false};
    synthPop.IncludesStreamed = true;
    NativeCollisionPlacement placement;
    placement.ModelId = 3991;
    placement.Model = "gsfreeway7_lan";
    placement.Record = 0;
    placement.Binary = true;
    placement.Ipl = "fixture_stream0.ipl";
    placement.Position = {10.0f, 10.0f, 0.0f};
    placement.Quaternion = {0.0f, 0.0f, 0.0f, 1.0f};
    placement.Interior = 0;
    placement.Flags = 0;
    placement.Lod = -1;
    synthPop.Instances.push_back(placement);

    // Real COL load via the existing owned API (POSIX adapter only).
    NativeCollisionAssets assets;
    if (!assets.Load(gameDir, synthPop, error)) {
        FailWith("isolated synthetic real COL Load", error);
    }
    ++s_Checks;
    std::puts("LOAD isolated synthetic real COL catalog via existing API");

    // Actual catalog gates: real 3991 Ready, parent KnownAbsent, invalid Unsupported.
    std::shared_ptr<const NativeCollisionModel> realModel;
    {
        const auto got = assets.LookupModel("GSFreeway7_LAn");
        Check(got.Status == NativeCollisionModelStatus::Ready, "isolated synthetic GSFreeway7_LAn must be Ready");
        Check(got.Model && got.Model->Name == "gsfreeway7_lan", "isolated synthetic Ready identity");
        Check(got.Model->HeaderId == 3991, "isolated synthetic header 3991");
        Check(got.Model->ValidatedHeaderId, "isolated synthetic validated header");
        Check(got.Model->Faces.size() == 122U, "isolated synthetic 122 faces");
        Check(got.Model->Unsupported.empty() && !got.Model->Empty, "isolated synthetic supported non-empty");
        Check(got.Error.empty(), "isolated synthetic Ready no error");
        Check(!got.Model->SourceChunk.empty(), "isolated synthetic complete COL bytes present");
        const auto varied = assets.LookupModel("GSFREEWAY7_LAN");
        Check(varied.Status == NativeCollisionModelStatus::Ready && varied.Model == got.Model,
              "isolated synthetic lookup lowercases");
        const auto parent = assets.LookupModel("LODGSFreeway7_LAn");
        Check(parent.Status == NativeCollisionModelStatus::KnownAbsent && !parent.Model &&
                  parent.Error.empty() && !parent.TimeShared,
              "isolated synthetic parent LODGSFreeway7_LAn must be KnownAbsent");
        const auto parentLower = assets.LookupModel("lodgsfreeway7_lan");
        Check(parentLower.Status == NativeCollisionModelStatus::KnownAbsent && !parentLower.Model,
              "isolated synthetic parent lower KnownAbsent");
        const auto empty = assets.LookupModel("");
        Check(empty.Status == NativeCollisionModelStatus::Unsupported && !empty.Model,
              "isolated synthetic empty stays Unsupported");
        const std::string nul("bad\0name", 8);
        const auto bad = assets.LookupModel(nul);
        Check(bad.Status == NativeCollisionModelStatus::Unsupported && !bad.Model,
              "isolated synthetic NUL stays Unsupported");
        realModel = got.Model;
        std::printf("LOOKUP isolated synthetic realCOL Ready model=%s header=%u faces=%zu validated=%d\n",
                    realModel->Name.c_str(), unsigned(realModel->HeaderId), realModel->Faces.size(),
                    int(realModel->ValidatedHeaderId));
    }

    // Synthetic IDE/Object.dat text via existing LoadSources (fixture pattern).
    NativeWorldEntityInfo info;
    {
        const std::vector<NativeWorldEntitySourceText> ide{
            {"fixture.ide", "objs\n3991 gsfreeway7_lan txd 180 0\nend\n"}};
        const NativeWorldEntitySourceText objects{"Object.dat", "*\n"};
        if (!info.LoadSources(synthPop, ide, objects, error)) {
            FailWith("isolated synthetic LoadSources", error);
        }
        ++s_Checks;
        const auto md = info.Query(placement);
        Check(md.Status == NativeWorldInfoStatus::Ready, "isolated synthetic metadata Ready");
        Check(md.Model && md.Placement, "isolated synthetic metadata pointers");
        std::puts("METADATA isolated synthetic fixture LoadSources pass (Building, not whole world)");
    }

    // Derive a valid query point from real model face geometry. Source math
    // unchanged; no invented height oracle.
    NativeSourceGroundTransform groundTransform;
    Check(NativeWorldGround::SourceTransform(placement, groundTransform),
          "isolated synthetic source transform");
    V stored{};
    bool haveStored = false;
    {
        Check(!realModel->Faces.empty(), "isolated synthetic real faces for query");
        for (const auto& face : realModel->Faces) {
            V verts[3];
            bool okVerts = true;
            for (size_t j = 0; j < 3; ++j) {
                if (face.Vertices[j] >= realModel->Vertices.size()) {
                    okVerts = false;
                    break;
                }
                verts[j] = realModel->Vertices[face.Vertices[j]];
                if (!FiniteV(verts[j])) {
                    okVerts = false;
                    break;
                }
            }
            if (!okVerts) {
                continue;
            }
            V centroid{};
            for (size_t k = 0; k < 3; ++k) {
                centroid[k] = (verts[0][k] + verts[1][k] + verts[2][k]) / 3.0f;
            }
            if (!FiniteV(centroid)) {
                continue;
            }
            const V world = WorldPoint(groundTransform, centroid);
            if (!FiniteV(world)) {
                continue;
            }
            const auto request = NativeSourceGroundRequest::Generator(world);
            const auto direct = NativeSourceGround::ProcessVerticalLine(*realModel, groundTransform, request);
            if (direct.Status == Status::Unsupported) {
                // Real-model domain dictates an explicit typed Unsupported;
                // never silently accept it as a Hit. Report and stop: the
                // prepared coverage below must then expect that same type.
                std::fprintf(stderr, "sa-core-world FAIL: isolated synthetic direct Unsupported reason=%d\n",
                             int(direct.Reason));
                std::exit(1);
            }
            if (direct.Status == Status::Hit && std::isfinite(direct.Point[2])) {
                stored = world;
                haveStored = true;
                break;
            }
        }
        Check(haveStored, "isolated synthetic realCOL hittable face centroid required");
        Check(FiniteV(stored), "isolated synthetic stored finite");
        std::printf("DERIVED isolated synthetic query=%.6f,%.6f,%.6f from real 122-face geometry\n", stored[0],
                    stored[1], stored[2]);
    }

    const float queryX = stored[0];
    const float queryY = stored[1];
    Check(std::isfinite(queryX) && std::isfinite(queryY) && queryX > -3000.0f && queryX < 3000.0f &&
              queryY > -3000.0f && queryY < 3000.0f,
          "isolated synthetic query inside source domain");

    // Owned SourceCollision snapshot via the existing memory-only API. Full
    // real COL model, not a borrowed triangle or generated geometry.
    auto bindings = std::make_shared<NativeCollisionSnapshot>();
    {
        NativeCollisionSnapshot out;
        if (!assets.Snapshot(synthPop, queryX, queryY, 500.0f, out, error)) {
            FailWith("isolated synthetic Snapshot", error);
        }
        ++s_Checks;
        Check(out.Instances.size() == 1U, "isolated synthetic single snapshot instance");
        Check(out.Instances[0].Model.get() == realModel.get(),
              "isolated synthetic snapshot shares real COL owner");
        Check(out.Instances[0].Model->Faces.size() == 122U, "isolated synthetic snapshot 122 faces retained");
        Check(out.Instances[0].Model->SourceChunk == realModel->SourceChunk,
              "isolated synthetic complete COL bytes retained");
        *bindings = std::move(out);
        std::puts("SNAPSHOT isolated synthetic SourceCollision owner + 122-face bytes pass");
    }

    // Prepare isolated world, metadata revision 7.
    std::shared_ptr<const NativeWorldGround> world;
    {
        std::string prepareError;
        world = NativeWorldGround::PreparePopulation(synthPop, info, *bindings, prepareError, 7);
        if (!world) {
            FailWith("isolated synthetic PreparePopulation revision 7", prepareError);
        }
        ++s_Checks;
        Check(world->PopulationCount() == 1U, "isolated synthetic single prepared entity");
        std::puts("PREPARE isolated synthetic revision=7 pass");
    }

    // Commit/publish/query matrix with the existing methods only.
    NativeWorldGroundPublication current;
    {
        const NativeWorldGroundCommit commit0{10, 7, queryX, queryY, 500.0f, bindings, 1.0f};
        std::string pubError;
        if (!world->Publish(commit0, queryX, queryY, current, pubError)) {
            FailWith("isolated synthetic initial Publish", pubError);
        }
        ++s_Checks;
        Check(current.Snapshot && current.SourceCollision == bindings,
              "isolated synthetic initial publication owns committed snapshot");
        Check(current.Snapshot->Targets.size() == 1U, "isolated synthetic single candidate");
        Check(current.Snapshot->Targets[0].Model.get() == realModel.get(),
              "isolated synthetic target retains real COL owner");
        Check(current.Snapshot->Targets[0].Model->Faces.size() == 122U,
              "isolated synthetic target 122 faces retained");
        const auto hit = NativeWorldGround::Query(current, stored, 10, 7);
        Check(hit.Status == Status::Hit, "isolated synthetic current query must real Hit");
        Check(FiniteV(hit.Point) && FiniteV(hit.Normal) && std::isfinite(hit.Fraction),
              "isolated synthetic Hit finite");
        Check(hit.WorldGeneration == 10ULL && hit.MetadataRevision == 7ULL, "isolated synthetic Hit stamps");
        std::puts("PUBLISH isolated synthetic initial Hit pass (no whole-world claim)");

        const auto oldPub = current;
        const auto oldSnap = current.Snapshot;
        const auto oldSource = current.SourceCollision;
        const size_t oldTargets = current.Snapshot->Targets.size();
        const auto oldModel = current.Snapshot->Targets[0].Model;
        Check(oldModel.get() == realModel.get(), "isolated synthetic old owner before matrix");

        // Generation rollback retains everything.
        {
            auto stale = commit0;
            --stale.WorldGeneration;
            std::string err;
            const bool ok = world->Publish(stale, queryX, queryY, current, err);
            Check(!ok && !err.empty(), "isolated synthetic rollback must fail with reason");
            Check(current.Snapshot == oldSnap && current.SourceCollision == oldSource,
                  "isolated synthetic rollback retains Snapshot/SourceCollision ptr");
            Check(current.Snapshot->Targets.size() == oldTargets &&
                      current.Snapshot->Targets[0].Model == oldModel,
                  "isolated synthetic rollback retains target list");
            Check(current.SourceCollision->Instances[0].Model.get() == realModel.get(),
                  "isolated synthetic rollback retains real COL owner");
            std::puts("ROLLBACK isolated synthetic atomic pass");
        }

        // Metadata mismatch (newer than prepared 7) retains everything.
        {
            auto stale = commit0;
            ++stale.MetadataRevision;
            std::string err;
            const bool ok = world->Publish(stale, queryX, queryY, current, err);
            Check(!ok && !err.empty(), "isolated synthetic metadata mismatch must fail");
            Check(current.Snapshot == oldSnap && current.SourceCollision == oldSource,
                  "isolated synthetic mismatch retains ptrs");
            Check(current.Snapshot->Targets.size() == oldTargets, "isolated synthetic mismatch retains targets");
            std::puts("METADATA-MISMATCH isolated synthetic atomic pass");
        }

        // Metadata older retains everything.
        {
            auto stale = commit0;
            stale.MetadataRevision = 6;
            std::string err;
            const bool ok = world->Publish(stale, queryX, queryY, current, err);
            Check(!ok && !err.empty(), "isolated synthetic metadata older must fail");
            Check(current.Snapshot == oldSnap && current.SourceCollision == oldSource,
                  "isolated synthetic older retains ptrs");
            std::puts("METADATA-OLDER isolated synthetic atomic pass");
        }

        // Same generation, different SourceCollision owner (copied snapshot):
        // entire previous Snapshot/SourceCollision/target list retained.
        auto copied = std::make_shared<NativeCollisionSnapshot>(*bindings);
        Check(copied.get() != bindings.get(), "isolated synthetic copied snapshot distinct owner");
        Check(copied->Instances[0].Model.get() == realModel.get(),
              "isolated synthetic copy shares real COL owner bytes");
        {
            auto stale = commit0;
            stale.SourceCollision = copied;
            std::string err;
            const bool ok = world->Publish(stale, queryX, queryY, current, err);
            Check(!ok && !err.empty(), "isolated synthetic same-gen changed owner must fail");
            Check(current.Snapshot == oldSnap && current.SourceCollision == oldSource,
                  "isolated synthetic owner-change retains ptrs");
            Check(current.Snapshot->Targets.size() == oldTargets &&
                      current.Snapshot->Targets[0].Model.get() == realModel.get(),
                  "isolated synthetic owner-change retains 122-face target list");
            std::puts("SAME-GEN-OWNER isolated synthetic rejection pass");
        }

        // Next generation valid owner adoption works.
        {
            auto next = commit0;
            ++next.WorldGeneration;
            next.SourceCollision = copied;
            std::string err;
            if (!world->Publish(next, queryX, queryY, current, err)) {
                FailWith("isolated synthetic next-gen adoption", err);
            }
            ++s_Checks;
            Check(current.Snapshot != oldSnap, "isolated synthetic adoption replaces Snapshot");
            Check(current.SourceCollision == copied, "isolated synthetic adoption adopts new owner");
            Check(current.SourceCollision->Instances[0].Model.get() == realModel.get(),
                  "isolated synthetic adoption retains real COL owner");
            Check(current.Snapshot->Targets.size() == 1U &&
                      current.Snapshot->Targets[0].Model->Faces.size() == 122U,
                  "isolated synthetic adoption retains 122 faces");
            const auto hitNew = NativeWorldGround::Query(current, stored, 11, 7);
            Check(hitNew.Status == Status::Hit, "isolated synthetic adopted query Hit");
            // Old immutable publication still queryable with its own generation.
            const auto hitOld = NativeWorldGround::Query(oldPub, stored, 10, 7);
            Check(hitOld.Status == Status::Hit, "isolated synthetic old publication still Hit");
            // Wrong generations report StaleWorld with the published generation.
            const auto staleNew = NativeWorldGround::Query(current, stored, 10, 7);
            Check(staleNew.Reason == Reason::StaleWorld && staleNew.WorldGeneration == 11ULL &&
                      staleNew.MetadataRevision == 7ULL,
                  "isolated synthetic new wrong-gen StaleWorld with published 11/7");
            const auto staleOld = NativeWorldGround::Query(oldPub, stored, 11, 7);
            Check(staleOld.Reason == Reason::StaleWorld && staleOld.WorldGeneration == 10ULL,
                  "isolated synthetic old wrong-gen StaleWorld with published 10");
            std::puts("ADOPTION isolated synthetic next-gen + StaleWorld + old-queryable pass");
        }

        const auto adoptedSnap = current.Snapshot;
        const auto adoptedSource = current.SourceCollision;

        // Invalid commits retain current (generation 12 avoids stale masking).
        {
            auto bad = commit0;
            bad.WorldGeneration = 12;
            bad.SourceCollision = copied;
            bad.Radius = 0.0f;
            std::string err;
            Check(!world->Publish(bad, queryX, queryY, current, err) && !err.empty(),
                  "isolated synthetic invalid radius must fail");
            Check(current.Snapshot == adoptedSnap && current.SourceCollision == adoptedSource,
                  "isolated synthetic invalid radius retains");
        }
        {
            auto bad = commit0;
            bad.WorldGeneration = 12;
            bad.SourceCollision = copied;
            bad.LodDistanceMultiplier = 0.0f;
            std::string err;
            Check(!world->Publish(bad, queryX, queryY, current, err) && !err.empty(),
                  "isolated synthetic invalid multiplier must fail");
            Check(current.Snapshot == adoptedSnap, "isolated synthetic invalid multiplier retains");
        }
        {
            auto bad = commit0;
            bad.WorldGeneration = 12;
            bad.SourceCollision = {};
            std::string err;
            Check(!world->Publish(bad, queryX, queryY, current, err) && !err.empty(),
                  "isolated synthetic null SourceCollision must fail");
            Check(current.Snapshot == adoptedSnap, "isolated synthetic null owner retains");
        }
        {
            // Committed-geometry coverage: far sector outside the committed square.
            auto far = commit0;
            far.WorldGeneration = 12;
            far.SourceCollision = copied;
            std::string err;
            Check(!world->Publish(far, 2500.0f, 2500.0f, current, err) && !err.empty(),
                  "isolated synthetic far commit must fail");
            Check(current.Snapshot == adoptedSnap && current.SourceCollision == adoptedSource,
                  "isolated synthetic far retains");
            std::puts("INVALID isolated synthetic commits retain pass");
        }

        // Whole-world is still not bypassed: far stored is outside coverage.
        {
            const V farStored{2500.0f, 2500.0f, 10.0f};
            const auto far = NativeWorldGround::Query(current, farStored, 11, 7);
            Check(far.Status == Status::Unsupported && far.Reason == Reason::OutsideCoverage,
                  "isolated synthetic far must be OutsideCoverage, not whole-world Hit");
            Check(far.WorldGeneration == 11ULL, "isolated synthetic far stamps published generation");
            std::puts("COVERAGE isolated synthetic OutsideCoverage pass (no whole-world claim)");
        }
    }

    // Duplicate identity and incomplete-population gates reuse the exact
    // fixture matrix shape on this isolated synthetic population.
    {
        NativeCollisionPopulation dup = synthPop;
        dup.Instances.push_back(placement);
        NativeWorldEntityInfo dupInfo;
        const std::vector<NativeWorldEntitySourceText> ide{
            {"fixture.ide", "objs\n3991 gsfreeway7_lan txd 180 0\nend\n"}};
        const NativeWorldEntitySourceText objects{"Object.dat", "*\n"};
        // Duplicate IPL source/record identity must be rejected at import.
        Check(!dupInfo.LoadSources(dup, ide, objects, error) && !error.empty(),
              "isolated synthetic duplicate identity rejected");
        std::puts("DUPLICATE isolated synthetic rejection pass");
    }
    {
        NativeCollisionPopulation incomplete = synthPop;
        incomplete.IncludesStreamed = false;
        NativeWorldEntityInfo partInfo;
        const std::vector<NativeWorldEntitySourceText> ide{
            {"fixture.ide", "objs\n3991 gsfreeway7_lan txd 180 0\nend\n"}};
        const NativeWorldEntitySourceText objects{"Object.dat", "*\n"};
        if (!partInfo.LoadSources(incomplete, ide, objects, error)) {
            FailWith("isolated synthetic incomplete LoadSources", error);
        }
        ++s_Checks;
        std::string prepareError;
        auto partial = NativeWorldGround::PreparePopulation(incomplete, partInfo, *bindings, prepareError, 7);
        Check(bool(partial), "isolated synthetic incomplete prepares (completeness checked at publish)");
        const NativeWorldGroundCommit commit{10, 7, queryX, queryY, 500.0f, bindings, 1.0f};
        const auto pub = partial->Publish(commit, queryX, queryY);
        const auto q = NativeWorldGround::Query(pub, stored, 10, 7);
        Check(q.Status == Status::Unsupported && q.Reason == Reason::UnknownCoverage,
              "isolated synthetic filtered population cannot assert completeness");
        std::puts("INCOMPLETE isolated synthetic UnknownCoverage pass");
    }

    std::printf("sa-core-world-ok checks=%zu isolated synthetic metadata+realCOL 3991/122 rev7 mult1.0\n", s_Checks);
    return 0;
}
