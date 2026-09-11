// Reuse the isolated read-only OS boundary and the complete old Landstal gate.
#define main LandstalProbeMain
#include "app/platform/linux/NativeGeneratedVehicleAssetsProbe.cpp"
#undef main
#include <bit>
#include <cmath>
#include "app/platform/linux/TexSample.h"

void NativeRustlerGpuProbe(const char*, const WorldShotScene&, const WorldShotScene&, const char*);

namespace {
using Bytes = std::span<const uint8_t>;
uint32 Word(Bytes b, size_t p = 0) {
    Require(p + 4 <= b.size(), "oracle word bounds");
    uint32 value; std::memcpy(&value, b.data() + p, 4); return value;
}
float Float(Bytes b, size_t p) { return std::bit_cast<float>(Word(b, p)); }
uint16_t Half(Bytes b, size_t p) {
    Require(p + 2 <= b.size(), "oracle half bounds");
    return uint16_t(b[p]) | uint16_t(b[p + 1]) << 8;
}
struct Chunk { uint32 Type; Bytes Data; };
std::vector<Chunk> Walk(Bytes b) {
    std::vector<Chunk> out;
    while (!b.empty()) {
        const auto size = Word(b, 4);
        Require(b.size() >= size + 12ull, "oracle chunk bounds");
        out.push_back({Word(b), b.subspan(12, size)});
        b = b.subspan(12 + size);
    }
    return out;
}
Bytes Part(Bytes b, uint32 type) {
    for (const auto& c : Walk(b)) if (c.Type == type) return c.Data;
    throw std::runtime_error("oracle missing chunk");
}
std::vector<uint8_t> RawDff(const char* game) {
    FILE* file = std::fopen((std::string(game) + "/models/gta3.img").c_str(), "rb");
    Require(file, "oracle archive");
    std::array<uint8_t, 8> header{};
    Require(std::fread(header.data(), 1, 8, file) == 8 && !std::memcmp(header.data(), "VER2", 4), "oracle IMG");
    std::vector<uint8_t> directory(Word(header, 4) * 32ull);
    Require(std::fread(directory.data(), 1, directory.size(), file) == directory.size(), "oracle directory");
    std::vector<uint8_t> result;
    for (size_t p = 0; p < directory.size(); p += 32) {
        if (std::strncmp(reinterpret_cast<const char*>(directory.data() + p + 8), "rustler.dff", 24)) continue;
        Require(result.empty(), "oracle unique DFF");
        std::fseek(file, long(Word(directory, p)) * 2048, SEEK_SET);
        result.resize((Word(directory, p + 4) & 0x7fff) * 2048ull);
        Require(std::fread(result.data(), 1, result.size(), file) == result.size(), "oracle DFF read");
    }
    std::fclose(file);
    Require(!result.empty(), "oracle DFF found");
    result.resize(12ull + Word(result, 4));
    return result;
}
void Near(float a, float b, const char* message, float tolerance = 1e-6f) {
    Require(std::isfinite(a) && std::abs(a - b) <= tolerance, message);
}
void RawOracle(const NativeGeneratedVehiclePacket& packet, Bytes dff, bool measures = true) {
    const auto clump = dff.subspan(12);
    const auto frameList = Walk(Part(clump, 14));
    const auto rawFrames = frameList.front().Data;
    Require(Word(rawFrames) == 27 && packet.Frames.size() == 27, "oracle frame count");
    Require(packet.FrameOverrides.size() == 8, "eight complete S0 overrides");
    std::array<bool, 27> targets{};
    for (const auto& value : packet.FrameOverrides) {
        Require(value.Frame < targets.size() && !targets[value.Frame], "unique S0 override target");
        targets[value.Frame] = true;
    }
    for (size_t i = 0; i < 27; ++i) {
        const auto& frame = packet.Frames[i];
        Require(frame.Parent == std::bit_cast<int32>(Word(rawFrames, 4 + i * 56 + 48)), "raw parent");
        Require(frame.Flags == Word(rawFrames, 4 + i * 56 + 52), "raw frame flags");
        for (size_t j = 0; j < 12; ++j) Require(frame.LocalBind[j] == Float(rawFrames, 4 + i * 56 + j * 4), "raw full bind matrix");
        const auto name = Part(frameList[i + 1].Data, 0x253f2fe);
        Require(frame.Name == std::string(reinterpret_cast<const char*>(name.data()), strnlen(reinterpret_cast<const char*>(name.data()), name.size())), "raw frame name");
        auto local = frame.LocalBind;
        for (const auto& value : packet.FrameOverrides) if (value.Frame == i) local = value.LocalMatrix;
        Require(frame.LocalPose == local, "complete local replacement pose");
        for (size_t axis = 0; axis < 4; ++axis) for (size_t c = 0; c < 3; ++c) {
            double posed = local[axis * 3 + c], bind = frame.LocalBind[axis * 3 + c];
            if (frame.Parent >= 0) {
                const auto& parent = packet.Frames[frame.Parent];
                posed = axis == 3 ? parent.ModelPose[9 + c] : 0;
                bind = axis == 3 ? parent.ModelBind[9 + c] : 0;
                for (size_t k = 0; k < 3; ++k) {
                    posed += double(local[axis * 3 + k]) * parent.ModelPose[k * 3 + c];
                    bind += double(frame.LocalBind[axis * 3 + k]) * parent.ModelBind[k * 3 + c];
                }
            }
            Near(frame.ModelPose[axis * 3 + c], float(posed), "complete hierarchy posed matrix");
            Near(frame.ModelBind[axis * 3 + c], float(bind), "complete hierarchy bind matrix");
        }
    }
    const auto col = Part(Part(clump, 3), 0x253f2fa);
    Require(packet.EmbeddedCollision && packet.Collision->Name == "rustler_col", "authored COL name");
    Require(packet.Collision->SourceChunk.size() == col.size() && std::equal(col.begin(), col.end(), packet.Collision->SourceChunk.begin()), "exact source COL bytes retained");
    const auto& collision = *packet.Collision;
    Require(collision.Version == 3 && Word(col, 4) + 8ull == col.size() && collision.HeaderId == Half(col, 30) &&
        collision.Flags == Word(col, 80) && collision.Spheres.size() == Half(col, 72) && collision.Boxes.size() == Half(col, 74) &&
        collision.Faces.size() == Half(col, 76), "independent COL header/counts");
    Near(collision.BoundRadius, Float(col, 68), "raw COL radius");
    for (size_t c = 0; c < 3; ++c) {
        Near(collision.Min[c], Float(col, 32 + c * 4), "raw COL min");
        Near(collision.Max[c], Float(col, 44 + c * 4), "raw COL max");
        Near(collision.BoundCenter[c], Float(col, 56 + c * 4), "raw COL center");
    }
    for (size_t i = 0; i < collision.Spheres.size(); ++i) {
        const auto p = 4ull + Word(col, 84) + i * 20;
        const auto& sphere = collision.Spheres[i];
        for (size_t c = 0; c < 3; ++c) Near(sphere.Center[c], Float(col, p + c * 4), "raw COL sphere center");
        Near(sphere.Radius, Float(col, p + 12), "raw COL sphere radius");
        Require(sphere.Surface.Material == col[p + 16] && sphere.Surface.Flags == col[p + 17] &&
            sphere.Surface.Brightness == col[p + 18] && sphere.Surface.Light == col[p + 19], "raw COL sphere surface");
    }
    for (size_t i = 0; i < collision.Faces.size(); ++i) {
        const auto p = 4ull + Word(col, 100) + i * 8;
        const auto& face = collision.Faces[i];
        for (size_t c = 0; c < 3; ++c) Require(face.Vertices[c] == Half(col, p + c * 2), "raw COL face indices");
        Require(face.Surface.Material == col[p + 6] && face.Surface.Light == col[p + 7], "raw COL face surface");
    }
    for (size_t i = 0; i < collision.Vertices.size(); ++i) for (size_t c = 0; c < 3; ++c)
        Near(collision.Vertices[i][c], std::bit_cast<int16_t>(Half(col, 4ull + Word(col, 96) + i * 6 + c * 2)) / 128.0f, "raw compressed COL vertices");
    std::vector<Bytes> geometries, atomics;
    for (const auto& c : Walk(Part(clump, 26))) if (c.Type == 15) geometries.push_back(c.Data);
    for (const auto& c : Walk(clump)) if (c.Type == 20) atomics.push_back(Part(c.Data, 1));
    Require(atomics.size() == 12 && geometries.size() == 12, "raw atomic/geometry count");
    const uint32 order[]{0,1,2,3,4,5,6,8,10,7,7,7,7};
    const uint32 bindings[]{1,2,3,4,5,6,7,8,14,12,13,21,23};
    size_t visited = 0, alpha128 = 0, static255 = 0;
    for (const auto& component : packet.Components) {
        const auto source = atomics.at(component.Atomic);
        Require(component.SourceFrame == Word(source) && component.Geometry == Word(source, 4) &&
            component.Flags == Word(source, 8) && component.RuntimeFlags == component.Flags, "raw atomic flags and provenance");
        if (component.Atomic == 9) {
            Require(component.Mesh == -1 && component.Use == NativeGeneratedVehicleComponentUse::AlphaSuppressed &&
                component.MaterialAlpha == 0 && component.Flags == 5 && component.RuntimeGeometryFlags == (component.GeometryFlags | 0x40), "moving alpha suppression retains render bit");
        }
        if (component.Mesh < 0) continue;
        const size_t mi = size_t(component.Mesh);
        Require(mi == visited++ && component.Atomic == order[mi] && component.BindFrame == bindings[mi], "independent mesh order and source collapse");
        const auto& mesh = packet.Scene.meshes[mi];
        const auto geometry = geometries.at(component.Geometry);
        const auto data = Part(geometry, 1);
        const auto format = Word(data), tris = Word(data, 4), verts = Word(data, 8);
        Require(mesh.tris == int(tris), "raw triangle count");
        size_t p = 16;
        const auto colors = p;
        if (format & 8) p += verts * 4ull;
        uint32 uvSets = (format >> 16) & 255;
        if (!uvSets) uvSets = (format & 128) ? 2 : (format & 4) ? 1 : 0;
        const auto uvs = p;
        p += uvSets * verts * 8ull;
        const auto triangles = p;
        p += tris * 8ull;
        Require(Word(data, p + 16) == 1, "raw positions present");
        const bool normals = Word(data, p + 20) != 0;
        p += 24;
        Require(p + verts * 12ull * (normals ? 2 : 1) <= data.size(), "raw vertex bounds");
        std::vector<Bytes> materials;
        for (const auto& c : Walk(Part(geometry, 8))) if (c.Type == 7) materials.push_back(c.Data);
        const auto& matrix = packet.Frames[bindings[mi]].ModelPose;
        const auto half = [&](size_t offset) { return uint32(data[offset]) | uint32(data[offset + 1]) << 8; };
        for (size_t t = 0; t < tris; ++t) {
            const uint32 indices[]{half(triangles + t * 8 + 2), half(triangles + t * 8), half(triangles + t * 8 + 6)};
            const auto mat = Part(materials.at(half(triangles + t * 8 + 4)), 1);
            const auto expectedAlpha = component.Atomic == 1 ? 255 : mat[7];
            Near(mesh.surfaces[t].color[3], expectedAlpha / 255.0f, "raw material alpha + source override");
            Near(mesh.surfaces[t].ambient, Float(mat, 16), "raw material ambient");
            Near(mesh.surfaces[t].diffuse, Float(mat, 24), "raw material diffuse");
            if (Word(mat, 12)) {
                const auto texture = Part(materials.at(half(triangles + t * 8 + 4)), 6);
                const auto name = Part(texture, 2);
                const std::string sourceName(reinterpret_cast<const char*>(name.data()), strnlen(reinterpret_cast<const char*>(name.data()), name.size()));
                Require(mesh.triImg[t] >= 0, "raw textured material must resolve");
                const auto& image = packet.Scene.images.at(mesh.triImg[t]);
                Require(sourceName == image.name || (sourceName == "#emap" && std::string(image.name) == "vehicleenvmap128") ||
                    (sourceName == "remap" && std::string(image.name).starts_with("rustler")), "raw resolved texture name");
            }
            alpha128 += expectedAlpha == 128;
            static255 += component.Atomic == 1 && expectedAlpha == 255;
            for (size_t c = 0; c < 3; ++c) {
                Near(mesh.triCol[t * 3 + c], mat[4 + c] / 255.0f, "raw authored RGB");
                Near(mesh.surfaces[t].color[c], mat[4 + c] / 255.0f, "no random paint");
            }
            for (size_t v = 0; v < 3; ++v) {
                Require(indices[v] < verts, "raw index bounds");
                for (size_t c = 0; c < 3; ++c) {
                    double expected = matrix[9 + c];
                    for (size_t k = 0; k < 3; ++k) expected += double(Float(data, p + indices[v] * 12 + k * 4)) * matrix[k * 3 + c];
                    Near(mesh.pos[t * 9 + v * 3 + c], float(expected), "every posed source vertex", 3e-6f);
                }
                if (normals) {
                    // The independently checked S0 bases have orthogonal axes.
                    // Inverse transpose is axis / squared axis length, not the
                    // position transform under the source nonuniform scaling.
                    std::array<double, 3> transformed{};
                    for (size_t k = 0; k < 3; ++k) {
                        double square = 0;
                        for (size_t c = 0; c < 3; ++c) square += double(matrix[k*3+c]) * matrix[k*3+c];
                        const double n = Float(data, p + verts * 12ull + indices[v] * 12 + k * 4);
                        for (size_t c = 0; c < 3; ++c) transformed[c] += n * matrix[k*3+c] / square;
                    }
                    double square = 0;
                    for (double n : transformed) square += n*n;
                    if (square > 1e-18) for (size_t c = 0; c < 3; ++c)
                        Near(mesh.nrm[t*9+v*3+c], float(transformed[c] / std::sqrt(square)), "every inverse-transpose source normal", 3e-6f);
                }
                if (uvSets) for (size_t c = 0; c < 2; ++c)
                    Near(mesh.uv[t * 6 + v * 2 + c], Float(data, uvs + indices[v] * 8 + c * 4), "raw UV order");
                if (format & 8) for (size_t c = 0; c < 4; ++c)
                    Require(mesh.dayColors[t * 12 + v * 4 + c] == data[colors + indices[v] * 4 + c], "raw prelight/alpha");
            }
        }
    }
    Require(visited == 13 && static255 == 84 && alpha128 > 0, "complete near/material proof");
    if (!measures) return;
    std::printf("rustler-oracle meshes=%zu triangles=%d static255=%zu alpha128=%zu colBytes=%zu colFaces=%zu colSpheres=%zu colBoxes=%zu\n",
        visited, packet.Scene.stats.triangles, static255, alpha128, col.size(), packet.Collision->Faces.size(), packet.Collision->Spheres.size(), packet.Collision->Boxes.size());
    for (const auto& override : packet.FrameOverrides) {
        std::printf("pose %u", override.Frame);
        for (float f : override.LocalMatrix) std::printf(" %.9g", f);
        std::puts("");
    }
    for (const auto& image : packet.Scene.images) std::printf("rustler-texture name=%s size=%dx%d bytes=%zu\n", image.name, image.w, image.h, image.rgba.size());
}
}

int main(int argc, char** argv) try {
    Require(argc == 2 || argc == 3, "usage: NativeRustlerProbe /game [capture-directory]");
    s_Game = argv[1];
    NativeCarGenerators generators;
    std::string error;
    Require(generators.LoadBeforeWorker(argv[1], 0, error), error);
    const auto* definition = generators.FindModel(476);
    Require(definition, "actual 476 definition");
    NativeGeneratedVehicleAsset asset;
    NativeCollisionAssets catalog;
    auto result = NativeGeneratedVehicleAssets_Load(argv[1], *definition, catalog, asset);
    Require(bool(result), result.Detail);
    Require(asset->Definition.ModelId == 476 && asset->Pose == NativeGeneratedVehiclePacket::PoseContract::FreshParkedAbandoned476 &&
        asset->DffSource == "gta3.img:rustler.dff" && asset->ModelTxdSource == "gta3.img:rustler.txd", "actual S0 identity");
    const auto raw = RawDff(argv[1]);
    RawOracle(*asset, raw);
    const auto original = asset;
    const auto fingerprint = Hash(*asset);
    auto* sentinel = rw::TexDictionary::create();
    rw::TexDictionary::setCurrent(sentinel);
    s_RejectCommonTxd = true;
    result = NativeGeneratedVehicleAssets_Load(argv[1], *definition, catalog, asset);
    s_RejectCommonTxd = false;
    Require(result.Status == NativeGeneratedVehicleAssetStatus::Error && asset == original && rw::TexDictionary::getCurrent() == sentinel, "476 post-parser failure atomic output/dictionary");
    s_RejectEmbeddedCol = true;
    result = NativeGeneratedVehicleAssets_Load(argv[1], *definition, catalog, asset);
    s_RejectEmbeddedCol = false;
    Require(result.Status == NativeGeneratedVehicleAssetStatus::Unsupported && asset == original && rw::TexDictionary::getCurrent() == sentinel,
        "476 corrupt authored COL rejects Ready atomically");
    auto invalid = *definition;
    invalid.WheelSizeRear = 0.6f;
    result = NativeGeneratedVehicleAssets_Load(argv[1], invalid, catalog, asset);
    Require(result.Status == NativeGeneratedVehicleAssetStatus::Unsupported && asset == original, "incomplete source pose cannot become Ready");
    result = NativeGeneratedVehicleAssets_Load(argv[1], *definition, catalog, asset);
    Require(bool(result) && Hash(*asset) == fingerprint && Hash(*original) == fingerprint && rw::TexDictionary::getCurrent() == sentinel, "476 reload/lifetime/dictionary");
    // Replay the immutable packet's generic inputs; compare raw geometry again.
    NativeGeneratedVehiclePacket replay = *asset;
    CarPoseStats stats{}; CarPoseAudit audit{}; char message[512]{};
    const auto load = [&](std::span<const VehicleAtomicOverride> atomics, std::span<const VehicleFrameOverride> frames) {
        return CarPose_Init(argv[1], "rustler", 0, 0, replay.Scene, stats, audit, message, sizeof(message),
            CarPoseTextures::RealtimeVehicle, {CarPoseGeometry::PristineNear, {-1,-1}, atomics, frames});
    };
    Require(load(asset->AtomicOverrides, asset->FrameOverrides), message);
    // CarPose applies its historical first paint scheme; packet restores authored
    // RGB. Undo only that documented legacy presentation for the source oracle.
    for (auto& mesh : replay.Scene.meshes) for (size_t t = 0; t < mesh.surfaces.size(); ++t)
        std::copy_n(mesh.triCol.begin() + t * 3, 3, mesh.surfaces[t].color.begin());
    RawOracle(replay, raw, false);
    auto badAtomic = asset->AtomicOverrides;
    badAtomic.push_back(badAtomic.front());
    Require(!load(badAtomic, asset->FrameOverrides), "duplicate atomic target rejected");
    badAtomic = asset->AtomicOverrides; badAtomic[0].SourceAtomic = 999;
    Require(!load(badAtomic, asset->FrameOverrides), "missing atomic target rejected");
    badAtomic = asset->AtomicOverrides; badAtomic[0].SourceFrame = 9;
    Require(!load(badAtomic, asset->FrameOverrides), "mismatched atomic target rejected");
    auto badFrame = asset->FrameOverrides; badFrame.push_back(badFrame.front());
    Require(!load(asset->AtomicOverrides, badFrame), "duplicate frame target rejected");
    badFrame = asset->FrameOverrides; badFrame[0].Frame = 999;
    Require(!load(asset->AtomicOverrides, badFrame), "missing frame target rejected");
    badFrame = asset->FrameOverrides; badFrame[0].LocalMatrix[0] = NAN;
    Require(!load(asset->AtomicOverrides, badFrame), "nonfinite pose rejected");
    Require(load({}, {}), message);
    Require(stats.tris == 2478 && replay.Scene.meshes.size() == 14, "old pristine Rustler default remains bind/both props");
    NativeRustlerGpuProbe(argv[1], asset->Scene, replay.Scene, argc == 3 ? argv[2] : nullptr);
    WorldShotScene offline;
    Require(CarPose_Init(argv[1], "landstal", 0, 0, offline, stats, audit, message, sizeof(message)), message);
    std::vector<uint8_t> pixels;
    TexFrameStats texStats{};
    TexSample_RenderOrbit(offline, 640, 480, 60, nullptr, pixels, texStats);
    uint64 checksum = 1469598103934665603ull;
    for (size_t p = 0; p < pixels.size(); p += 4) {
        checksum ^= uint64(pixels[p]) | uint64(pixels[p+1]) << 8 | uint64(pixels[p+2]) << 16;
        checksum *= 1099511628211ull;
    }
    Require(checksum == 5730483265208789677ull, "historical offline Landstal checksum");
    std::printf("offline-landstal checksum=%llu unchanged=1\n", static_cast<unsigned long long>(checksum));
    CarPose_Shutdown();
    rw::TexDictionary::setCurrent(nullptr); sentinel->destroy();
    std::printf("rustler-ready model=476 images=%zu components=%zu hash=%llu flags=5,5 alpha=255,0 no-spawn\n",
        asset->Scene.images.size(), asset->Components.size(), static_cast<unsigned long long>(fingerprint));
    Require(LandstalProbeMain(2, argv) == 0, "old Landstal complete gate");
    Require(Hash(*asset) == fingerprint && Hash(*original) == fingerprint, "476 owned after engine teardown");
    RawOracle(*asset, raw, false);
    RawOracle(*original, raw, false);
    std::puts("rustler-assets-ok raw-source pose alpha order COL replay failures dictionary lifetime");
    return 0;
} catch (const std::exception& e) {
    std::fprintf(stderr, "rustler-assets FAIL %s\n", e.what());
    return 1;
}
