#include "NativePedAssets.h"
#include "NativePedModelMetadata.h"
#include "NativePedSkinSetup.h"
#include "NativePedHitCollision.h"
#include "NativePedHierarchy.h"
#include "RealtimeStreaming.h"

#include <bit>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <stdexcept>
#include <thread>

namespace {
int g_Checks = 0;
void Check(bool value, const char* message) {
    ++g_Checks;
    if (!value) { std::fprintf(stderr, "ped-assets-fail: %s\n", message); std::exit(1); }
}
void Bits(float value) { std::printf(" %u", std::bit_cast<std::uint32_t>(value)); }
void Matrix(const NativePlayerMatrix& value) {
    for (const auto& row : {value.Right, value.Up, value.At, value.Pos}) for (float v : row) Bits(v);
}
void Rows(const NativePedAssets& packet) {
    std::printf("PED_ASSET %d %s %s %zu %zu\n", packet.Model.ModelId, packet.Model.Name.c_str(),
        packet.Model.TxdName.c_str(), packet.Geometries.size(), packet.Images.size());
    std::printf("CLUMP_ROOT %d %u", packet.Model.ModelId, packet.ClumpRootLocalFlags);
    Matrix(packet.ClumpRootLocal); std::puts("");
    for (std::size_t g = 0; g < packet.Geometries.size(); ++g) {
        const auto& mesh = packet.Geometries[g];
        std::printf("GEOM %d %zu %u %zu %zu %zu %zu %u\n", packet.Model.ModelId, g, mesh.Flags,
            mesh.Bones.size(), mesh.Vertices.size(), mesh.Triangles.size(), mesh.Materials.size(), unsigned(mesh.AtomicFlags));
        std::printf("GEOM_SETUP %d %zu %u %u\n", packet.Model.ModelId, g,
            std::bit_cast<std::uint32_t>(mesh.MorphRadius), mesh.HierarchyFlags);
        for (std::size_t b = 0; b < mesh.Bones.size(); ++b) {
            const auto& bone = mesh.Bones[b];
            std::printf("BONE %d %zu %zu %d %d %u", packet.Model.ModelId, g, b, bone.Tag, bone.Parent, bone.Flags);
            Matrix(bone.Local); Matrix(mesh.InverseBind[b]); std::puts("");
            std::printf("INVERSE_FLAGS %d %zu %zu %u\n", packet.Model.ModelId, g, b, mesh.InverseBindFlags[b]);
        }
        for (std::size_t m = 0; m < mesh.Materials.size(); ++m) {
            const auto& material = mesh.Materials[m];
            std::printf("MAT %d %zu %zu %d", packet.Model.ModelId, g, m, material.Image);
            for (float f : material.Surface.color) Bits(f);
            Bits(material.Surface.ambient); Bits(material.Specular); Bits(material.Surface.diffuse);
            std::puts("");
        }
        for (std::size_t v = 0; v < mesh.Vertices.size(); ++v) {
            const auto& vertex = mesh.Vertices[v];
            std::printf("VERT %d %zu %zu", packet.Model.ModelId, g, v);
            for (float f : vertex.Position) Bits(f);
            for (float f : vertex.Normal) Bits(f);
            for (float f : vertex.UV) Bits(f);
            for (auto c : vertex.Color) std::printf(" %u", unsigned(c));
            for (auto b : vertex.Bones) std::printf(" %u", unsigned(b));
            for (float f : vertex.Weights) Bits(f);
            std::puts("");
        }
        for (std::size_t t = 0; t < mesh.Triangles.size(); ++t) {
            const auto& tri = mesh.Triangles[t];
            std::printf("TRI %d %zu %zu %u %u %u %u\n", packet.Model.ModelId, g, t,
                tri.Vertices[0], tri.Vertices[1], tri.Vertices[2], tri.Material);
        }
    }
    for (std::size_t i = 0; i < packet.Images.size(); ++i) {
        const auto& image = packet.Images[i];
        std::printf("IMG %d %zu %s %d %d %zu\n", packet.Model.ModelId, i, image.name, image.w, image.h, image.rgba.size());
    }
}
void HitFixture(const NativePedAssets& packet) {
    std::vector<NativePedHitBone> bones;
    for (const auto& bone : packet.Geometries.front().Bones)
        bones.push_back({bone.Tag, true, {bone.World, bone.WorldMatrixFlags}});
    NativePedHitCollision hit;
    // Explicit frame-LTM fixture, NOT a claim that source hierarchy Update has
    // executed or that these matrices are a current live animation array.
    NativePedHitCollisionInput input{true, true, {packet.ClumpRootLocal, packet.ClumpRootLocalFlags}, bones};
    Check(NativeConstructPedHitCollision(input, hit) == NativePedHitCollisionStatus::Constructed &&
        hit.Spheres.size() == 12 && hit.Spheres[0].Material == 62, "owned frame fixture feeds hit COL, not Loaded");
    NativePedHitCollisionState pose;
    pose.Known = true;
    Check(NativeAnimatePedHitCollision(input, NativePedHitCollisionSpace::Local, pose) == NativePedHitCollisionStatus::Constructed &&
        pose.Value == hit, "local first use returns construction without animated bounds");
    Check(NativeAnimatePedHitCollision(input, NativePedHitCollisionSpace::Local, pose) == NativePedHitCollisionStatus::Updated &&
        pose.Value.Spheres == hit.Spheres, "local fixture updates bounds without changing node radii/materials");
    pose.Present = false;
    Check(NativeAnimatePedHitCollision(input, NativePedHitCollisionSpace::World, pose) == NativePedHitCollisionStatus::Updated &&
        pose.Present && pose.Value.BoundRadius == 1.5F, "world first use constructs then updates current pose");
    input.HierarchyKnown = false;
    const auto previous = hit;
    Check(NativeConstructPedHitCollision(input, hit) == NativePedHitCollisionStatus::UnknownHierarchy && hit == previous,
        "parsed frames do not implicitly authorize source hierarchy matrices");
    const auto previousPose = pose;
    Check(NativeAnimatePedHitCollision(input, NativePedHitCollisionSpace::World, pose) == NativePedHitCollisionStatus::UnknownHierarchy &&
        pose == previousPose, "pose fixture cannot publish without a current hierarchy observation");
}
void HierarchyFixture(const NativePedAssets& packet) {
    const auto& mesh = packet.Geometries.front();
    Check(mesh.InverseBindFlags.size() == mesh.Bones.size(), "authored skin matrix flags retained with inverse matrices");
    std::vector<NativePedBindBone> bindBones;
    for (std::size_t i = 0; i < mesh.Bones.size(); ++i)
        bindBones.push_back({mesh.Bones[i].Flags, true, {mesh.InverseBind[i], mesh.InverseBindFlags[i]}});
    std::vector<std::array<float, 3>> restPositions;
    NativePedBindPositionInput bindInput{true, true, bindBones};
    Check(NativePlanPedBindPositions(bindInput, restPositions) == NativePedHierarchyStatus::Planned &&
        restPositions.size() == mesh.Bones.size() && restPositions.front() == std::array<float, 3>{},
        "real authored skin/node data produces source blend-frame rest translations, not a live keyframe");
    std::vector<std::int32_t> tags;
    for (const auto& bone : mesh.Bones) tags.push_back(bone.Tag);
    std::vector<NativePedBlendFrameBinding> bindings;
    NativePedBlendInitInput blendInput{bindInput, true, 28, tags}; // Explicit registered-interpolator fixture.
    Check(NativePlanPedBlendInitialization(blendInput, bindings) == NativePedHierarchyStatus::Planned &&
        bindings.size() == mesh.Bones.size() && bindings.front().Flags == 8 &&
        bindings.back().Tag == mesh.Bones.back().Tag && bindings.back().KeyFrameByteOffset == (bindings.size() - 1) * 28,
        "real source skin rest data binds frame indices without initializing a quaternion or live pose");
    const auto previousBindings = bindings;
    blendInput.InterpolatorKnown = false;
    Check(NativePlanPedBlendInitialization(blendInput, bindings) == NativePedHierarchyStatus::UnknownInterpolator &&
        bindings == previousBindings, "asset presence cannot invent an interpolator binding");
    NativePedInterpolationFrame unknownBlendFrame;
    NativePedHitMatrix untouched;
    untouched.Value.Pos[0] = 777;
    Check(NativeApplyPedBlendFrame(unknownBlendFrame, untouched) == NativePedHierarchyStatus::UnknownAppliedPose &&
        untouched.Value.Pos[0] == 777, "frame-data initialization does not imply GTA interpolation callback execution");
    const auto previousRest = restPositions;
    bindBones.back().InverseKnown = false;
    Check(NativePlanPedBindPositions(bindInput, restPositions) == NativePedHierarchyStatus::UnknownInverseBind &&
        restPositions == previousRest, "missing inverse data cannot default a source bone position");
    std::vector<NativePedHierarchyNode> nodes;
    for (const auto& bone : packet.Geometries.front().Bones) {
        // Authored local frames stand in for the interpolation callback ONLY
        // in this explicit bind fixture. The parser never asserts AppliedKnown.
        nodes.push_back({bone.Tag, bone.Flags, true, {bone.Local, 0}, true, true, 0});
    }
    NativePedHierarchyInput input;
    input.HierarchyKnown = input.ParentKnown = input.RootFrameKnown = true;
    input.HasParent = input.HasRootFrame = true;
    input.ParentWorld = {packet.ClumpRootLocal, packet.ClumpRootLocalFlags};
    input.Flags = 0x3000;
    input.Nodes = nodes;
    NativePedHierarchyPlan plan;
    Check(NativePlanPedHierarchyUpdate(input, plan) == NativePedHierarchyStatus::Planned &&
        plan.Nodes.size() == nodes.size() && plan.EnqueueRootDirty,
        "actual node order feeds explicit callback fixture with frame/dirty effects still planned");
    std::vector<NativePedHitBone> matrices;
    for (const auto& node : plan.Nodes) {
        Check(node.Matrix && node.Modelling && node.Ltm && !node.UpdateObjects,
            "simple source hierarchy plan contains current and attached frame writes");
        matrices.push_back({node.Tag, true, *node.Matrix});
    }
    NativePedHitCollision shape;
    Check(NativeConstructPedHitCollision({true, true,
        {packet.ClumpRootLocal, packet.ClumpRootLocalFlags}, matrices}, shape) ==
        NativePedHitCollisionStatus::Constructed, "planned callback matrix fixture feeds source hit COL");
    const auto prior = plan;
    nodes.back().AppliedKnown = false;
    Check(NativePlanPedHierarchyUpdate(input, plan) == NativePedHierarchyStatus::UnknownAppliedPose && plan == prior,
        "real DFF data alone cannot authorize interpolation callback results");
}
std::optional<realtime_streaming::PedAssetCompletion> Take(realtime_streaming::Worker& worker, std::uint64_t ticket) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (std::chrono::steady_clock::now() < deadline) {
        if (auto result = worker.TakePedAsset(ticket)) return result;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return {};
}
}
int main(int argc, char** argv) {
    Check(argc == 2 || (argc == 3 && (std::string_view(argv[2]) == "--rows" || std::string_view(argv[2]) == "--catalog")),
        "usage GAME_DIR [--rows|--catalog]");
    NativeWorldEntityInfo namespaceInfo;
    NativePedModelMetadata metadata;
    std::string error;
    Check(namespaceInfo.LoadBeforeWorker(argv[1], {}, error), error.c_str());
    Check(metadata.LoadBeforeWorker(argv[1], namespaceInfo, error), error.c_str());
    E2ELoadInfo info{};
    char parserError[512]{};
    Check(StreamPager_Init(argv[1], info, parserError, sizeof(parserError)), parserError);
    std::vector<std::shared_ptr<const NativePedAssets>> retained;
    {
        realtime_streaming::Worker worker(false, {}, {}, 1, {}, {}, {},
            [](const realtime_streaming::PedAssetRequest& request, NativePedAssets& out, std::string& error) {
                return NativePedAssets_Load(request.GameDir.c_str(), request.Model, out, error);
            });
        std::uint64_t ticket = 1;
        for (int id : {7, 105, 280}) {
            const auto* model = metadata.Find(id);
            Check(model, "actual qualified ped model");
            Check(worker.RequestPedAsset({ticket, argv[1], model->Source}), "real ped request admitted");
            const auto completed = Take(worker, ticket);
            Check(completed && completed->Packet && completed->Error.empty(),
                completed ? completed->Error.c_str() : "ped timeout");
            retained.push_back(completed->Packet);
            HitFixture(*completed->Packet);
            HierarchyFixture(*completed->Packet);
            const auto& first = completed->Packet->Geometries.front();
            NativePedSkinSetupPlan skin;
            Check(NativePlanPedSkinSetup({true, false, first.MorphRadius, first.HierarchyFlags, first.Vertices}, skin) ==
                NativePedSkinSetupStatus::Planned && skin.Weights.size() == first.Vertices.size(),
                "actual packet feeds explicit simple-hierarchy fixture, not Loaded");
            Check(!worker.TakePedAsset(ticket), "single consumption");
            NativeCivilianLoadedPed slot; slot.Model = 777;
            Check(metadata.QualifyCivilianSlot(id, false, false, 0, slot) == NativePedMetadataStatus::UnknownStreaming &&
                slot.Model == 777, "parser completion does not invent source Loaded/refcount");
            if (argc == 3 && std::string_view(argv[2]) == "--rows") Rows(*completed->Packet);
            ++ticket;
        }
        auto missing = metadata.Find(7)->Source; missing.Name = "missing_owned_ped";
        Check(worker.RequestPedAsset({ticket, argv[1], missing}), "missing asset request admitted");
        const auto failed = Take(worker, ticket);
        Check(failed && !failed->Packet && !failed->Error.empty(), "missing DFF has no fallback packet");
        if (argc == 3 && std::string_view(argv[2]) == "--catalog") {
            unsigned parsed = 0, missing = 0;
            for (const auto& [id, model] : metadata.Models()) {
                if (id == 0) continue; // The separate, source modular CJ constructor owns model0.
                ++ticket;
                Check(worker.RequestPedAsset({ticket, argv[1], model.Source}), "catalog request admission");
                const auto result = Take(worker, ticket);
                Check(result.has_value(), "catalog completion");
                Check(bool(result->Packet) == result->Error.empty(), "catalog completion is packet or error");
                if (result->Packet) {
                    ++parsed;
                    HitFixture(*result->Packet);
                    HierarchyFixture(*result->Packet);
                    const auto& first = result->Packet->Geometries.front();
                    NativePedSkinSetupPlan skin;
                    Check(NativePlanPedSkinSetup({true, false, first.MorphRadius, first.HierarchyFlags, first.Vertices}, skin) ==
                        NativePedSkinSetupStatus::Planned && skin.Weights.size() == first.Vertices.size(),
                        "catalog source-normalization plan retains parser authored weights");
                } else ++missing;
                std::printf("PED_CATALOG %d %s %s %d %s\n", id, model.Source.Name.c_str(), model.Source.TxdName.c_str(),
                    result->Packet ? 1 : 0, result->Error.c_str());
            }
            std::printf("native-ped-assets-catalog-ok declarations=%u parsed=%u unavailable=%u modular-player=separate\n",
                parsed + missing, parsed, missing);
        }
        worker.Stop({}, {});
        Check(!worker.RequestPedAsset({99, argv[1], metadata.Find(7)->Source}), "stopped admission rejected");
        Check(!worker.TakePedAsset(ticket), "stopped completion cannot publish moved-from packet");
    }
    NativePedAssets output = *retained.front();
    const auto original = output.Geometries.front().Vertices.front().Position;
    auto invalid = metadata.Find(7)->Source;
    invalid.TxdName = "../male01";
    Check(!NativePedAssets_Load(argv[1], invalid, output, error) && output.Model.ModelId == 7 &&
        output.Geometries.front().Vertices.front().Position == original, "invalid name preserves owned output");
    invalid = metadata.Find(7)->Source; invalid.TxdName = "fam1";
    Check(!NativePedAssets_Load(argv[1], invalid, output, error) && output.Model.TxdName == retained.front()->Model.TxdName,
        "wrong declared TXD does not infer model-stem texture");
    invalid = metadata.Find(7)->Source; invalid.ModelId = 0;
    Check(!NativePedAssets_Load(argv[1], invalid, output, error) && output.Model.ModelId == 7,
        "modular player is not silently substituted with an ordinary ped");
    std::promise<void> entered, release;
    const auto released = release.get_future().share();
    {
        realtime_streaming::Worker barrier(false, {}, {}, 1, {}, {}, {},
            [&](const realtime_streaming::PedAssetRequest&, NativePedAssets&, std::string&) -> bool {
                entered.set_value(); released.wait(); throw std::runtime_error("intentional ped parser exception");
            });
        const realtime_streaming::PedAssetRequest request{101, argv[1], metadata.Find(7)->Source};
        Check(barrier.RequestPedAsset(request), "barrier admission");
        Check(entered.get_future().wait_for(std::chrono::seconds(5)) == std::future_status::ready, "parser barrier");
        auto other = request; other.Ticket = 102;
        Check(!barrier.RequestPedAsset(other) && barrier.RequestPedAsset(request), "in-flight ticket protection");
        other = request; other.Model = metadata.Find(105)->Source;
        Check(!barrier.RequestPedAsset(other), "same ticket cannot switch the declared ped identity");
        release.set_value();
        const auto failed = Take(barrier, 101);
        Check(failed && !failed->Packet && failed->Error.find("intentional ped parser exception") != std::string::npos &&
            !barrier.TakePedAsset(102), "exception remains exact-ticket error completion");
        barrier.Stop({}, {});
    }
    {
        realtime_streaming::Worker empty(false, {}, {}, 1, {}, {}, {},
            [](const realtime_streaming::PedAssetRequest&, NativePedAssets&, std::string&) { return true; });
        Check(empty.RequestPedAsset({201, argv[1], metadata.Find(7)->Source}), "empty parser admission");
        const auto failed = Take(empty, 201);
        Check(failed && !failed->Packet && !failed->Error.empty(), "empty success is an error, not parser-ready");
        empty.Stop({}, {});
    }
    StreamPager_Shutdown();
    for (const auto& packet : retained) {
        Check(!packet->Geometries.empty() && !packet->Images.empty(), "owned data survives RW shutdown");
        for (const auto& mesh : packet->Geometries) Check(!mesh.Bones.empty() && mesh.InverseBind.size() == mesh.Bones.size() &&
            !mesh.Vertices.empty() && !mesh.Triangles.empty(), "retained skeleton/mesh payload");
    }
    std::printf("native-ped-assets-ok checks=%d models=3 worker=sole skeleton=owned textures=declared "
        "loaded-state=unowned actor-birth=unowned census=incomplete\n", g_Checks);
}
