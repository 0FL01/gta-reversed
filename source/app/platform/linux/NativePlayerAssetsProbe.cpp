// Standalone numerical CJ asset diagnostic. No original payload is written.
// Run before any engine or streaming worker; this process owns NULL librw.
// Startup schema follows independently PE-mapped owned-retail static evidence
// and main.scm's 087B/070D identifiers/stats. It does not execute the SCM VM.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

using int32 = int32_t;
using uint32 = uint32_t;
using int64 = int64_t;
using uint64 = uint64_t;
#define __stdcall
#include "oswrapper/oswrapper.h"
#include "app/platform/linux/TexSample.h"
#include "app/platform/linux/NativePlayerAssets.h"
// Probe-only access to the existing bank decoder/sampler, like the neighboring
// VehicleMaterialProbe's CarPose inclusion. No IfpAnim API/implementation edit,
// no call to its model-loading (andre fallback) path, no duplicate IFP parser.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "app/platform/linux/IfpAnim.cpp" // also includes librw's unguarded rw.h
#pragma GCC diagnostic pop

NativePlayerVertex NativePlayerAssets_ProbeBlendSkin(const std::array<NativePlayerVertex, 3>&, const std::array<float, 3>&);
NativePlayerImage NativePlayerAssets_ProbeBlendImage(NativePlayerImage, const NativePlayerImage&, const NativePlayerImage&, const NativePlayerImage&, const std::array<float, 3>&);

namespace {
void Require(bool ok, const char* message) {
    if (!ok) {
        std::fprintf(stderr, "player-assets FAIL %s\n", message);
        std::exit(1);
    }
}
uint32 U32(const uint8_t* p) {
    return uint32(p[0]) | uint32(p[1]) << 8 | uint32(p[2]) << 16 | uint32(p[3]) << 24;
}
struct Archive {
    void* file = nullptr;
    struct Entry { int32 offset, size; };
    std::map<std::string, Entry> entries;
    explicit Archive(const char* path) {
        Require(OS_FileOpen(FILE_DATA_AREA_DEFAULT, &file, path, FILE_ACCESS_READ) == 0, path);
        const int32 size = OS_FileSize(file);
        Require(size >= 8, "IMG fits positive OS_File int32 range");
        uint8_t header[8];
        Require(OS_FileRead(file, header, 8) == 0 && !std::memcmp(header, "VER2", 4), "IMG header");
        const uint32 count = U32(header + 4);
        Require(count && count <= 300000 && 8ull + count * 32ull <= uint32(size), "IMG directory bounds");
        for (uint32 i = 0; i < count; ++i) {
            uint8_t entry[32];
            Require(OS_FileRead(file, entry, 32) == 0, "IMG directory read");
            const uint64_t offset = uint64_t(U32(entry)) * 2048;
            const uint64_t length = uint64_t(U32(entry + 4) & 0x7fff) * 2048;
            Require(offset + length <= uint32(size), "IMG entry bounds");
            std::string name(reinterpret_cast<char*>(entry + 8), strnlen(reinterpret_cast<char*>(entry + 8), 24));
            for (char& c : name) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
            Require(entries.emplace(name, Entry{int32(offset), int32(length)}).second, "unique IMG entry");
        }
        std::printf("archive path=%s entries=%zu bytes=%d\n", path, entries.size(), size);
    }
    ~Archive() { OS_FileClose(file); }
    std::vector<uint8_t> Read(const std::string& name) {
        const auto it = entries.find(name);
        Require(it != entries.end(), name.c_str());
        const auto e = it->second;
        Require(e.size > 0, "nonempty IMG entry");
        OS_FileSetPosition(file, e.offset);
        Require(OS_FileGetPosition(file) == e.offset, "IMG seek");
        std::vector<uint8_t> bytes(e.size);
        Require(OS_FileRead(file, bytes.data(), e.size) == 0, "IMG payload read");
        return bytes;
    }
};

int s_nameOffset;
void* InitName(void* object, int32 offset, int32 size) {
    std::memset(static_cast<char*>(object) + offset, 0, size);
    return object;
}
rw::Stream* ReadName(rw::Stream* stream, int32 length, void* object, int32 offset, int32 size) {
    Require(length >= 0 && length < size, "frame name length");
    stream->read8(static_cast<char*>(object) + offset, length);
    return stream;
}
const char* Name(rw::Frame* frame) { return reinterpret_cast<char*>(frame) + s_nameOffset; }
void InitRw() {
    Require(rw::Engine::init(nullptr), "RW init");
    rw::ps2::registerPDSPlugin(40);
    rw::ps2::registerPluginPDSPipes();
    rw::registerMeshPlugin();
    rw::registerNativeDataPlugin();
    rw::registerAtomicRightsPlugin();
    rw::registerMaterialRightsPlugin();
    rw::xbox::registerVertexFormatPlugin();
    rw::registerSkinPlugin();
    rw::registerUserDataPlugin();
    rw::registerHAnimPlugin();
    rw::registerMatFXPlugin();
    rw::registerUVAnimPlugin();
    rw::ps2::registerADCPlugin();
    s_nameOffset = rw::Frame::registerPlugin(64, 0x253f2fe, InitName, nullptr, nullptr);
    Require(s_nameOffset >= 0, "frame name plugin");
    rw::Frame::registerPluginStream(0x253f2fe, ReadName, nullptr, nullptr);
    Require(rw::Engine::open(nullptr) && rw::Engine::start(), "RW start");
    rw::Texture::setLoadTextures(false);
}
void Frames(rw::Frame* f, int& count) {
    ++count;
    for (auto* c = f->child; c; c = c->next) Frames(c, count);
}
std::set<int> s_playerTags;
std::map<int, std::array<float, 16>> s_playerInverse;
std::map<std::string, TexImage> s_overlayInputs;
void InspectClump(const uint8_t* bytes, size_t size, const char* name, int clumpIndex) {
    auto linked = TexSample_LinkedParse(bytes, size, nullptr, nullptr, 0);
    Require(linked.clump, "DFF parse");
    auto* clump = linked.clump;
    int frames = 0;
    Frames(clump->getFrame(), frames);
    std::printf("dff name=%s clump=%d frames=%d\n", name, clumpIndex, frames);
    FORLIST(link, clump->atomics) {
        auto* a = rw::Atomic::fromClump(link);
        auto* g = a->geometry;
        auto* skin = rw::Skin::get(g);
        auto* hierarchy = rw::HAnimHierarchy::find(a->getFrame());
        if (!hierarchy) hierarchy = rw::HAnimHierarchy::find(clump->getFrame());
        Require(skin && hierarchy && skin->numBones == hierarchy->numNodes, "skinned part hierarchy");
        int attached = 0, missing = 0, remapped = 0;
        if (hierarchy) {
            hierarchy->attach();
            for (int b = 0; b < hierarchy->numNodes; ++b) {
                attached += hierarchy->nodeInfo[b].frame != nullptr;
                const int tag = hierarchy->nodeInfo[b].id;
                if (!std::strcmp(name, "player.dff")) s_playerTags.insert(tag);
                else missing += !s_playerTags.contains(tag);
                if (!std::strcmp(name, "player.dff") && skin && b < skin->numBones) {
                    std::copy_n(skin->inverseMatrices + b * 16, 16, s_playerInverse[tag].begin());
                }
                remapped += tag != b;
            }
        }
        double wsum = 0;
        double weightError = 0;
        int invalid = 0, usedMissing = 0;
        float inverseDelta = 0;
        std::set<int> usedTags;
        if (skin) for (int v = 0; v < g->numVertices; ++v) {
            double vertexWeight = 0;
            for (int k = 0; k < 4; ++k) {
                const float w = skin->weights[v * 4 + k];
                const int b = skin->indices[v * 4 + k];
                Require(std::isfinite(w) && w >= 0, "finite nonnegative weight");
                wsum += w;
                vertexWeight += w;
                if (w != 0) {
                    invalid += b >= skin->numBones;
                    if (hierarchy && b < hierarchy->numNodes) usedTags.insert(hierarchy->nodeInfo[b].id);
                }
            }
            weightError = std::max(weightError, std::abs(vertexWeight - 1));
        }
        for (int tag : usedTags) usedMissing += !s_playerTags.contains(tag);
        if (hierarchy && skin) for (int b = 0; b < hierarchy->numNodes && b < skin->numBones; ++b) {
            const int tag = hierarchy->nodeInfo[b].id;
            if (!usedTags.contains(tag) || !s_playerInverse.contains(tag)) continue;
            for (int k = 0; k < 16; ++k) {
                if (k % 4 == 3) continue; // RW matrix flags/padding are not coefficients.
                inverseDelta = std::max(inverseDelta, std::abs(skin->inverseMatrices[b * 16 + k] - s_playerInverse.at(tag)[k]));
            }
        }
        std::printf(" atomic frame=%s verts=%d tris=%d mats=%d uvsets=%d skinBones=%d hierarchy=%d attached=%d tagsMissingFromPlayer=%d indexNotTag=%d usedTags=%zu wsum=%.9f invalid=%d\n",
            Name(a->getFrame()), g->numVertices, g->numTriangles, g->matList.numMaterials, g->numTexCoordSets,
            skin ? skin->numBones : 0, hierarchy ? hierarchy->numNodes : 0, attached, missing, remapped, usedTags.size(),
            g->numVertices ? wsum / g->numVertices : 0, invalid);
        std::printf("  usedTagsMissingFromPlayer=%d usedInverseBindMaxDelta=%.9g maxVertexWeightError=%.9g\n", usedMissing, inverseDelta, weightError);
        Require(invalid == 0 && usedMissing == 0 && weightError < 0.0001, "weighted bone coverage and normalization");
        if (g->numTexCoordSets) {
            float u0 = INFINITY, v0 = INFINITY, u1 = -INFINITY, v1 = -INFINITY;
            for (int v = 0; v < g->numVertices; ++v) {
                const auto uv = g->texCoords[0][v];
                Require(std::isfinite(uv.u) && std::isfinite(uv.v), "finite UVs");
                u0 = std::min(u0, uv.u); u1 = std::max(u1, uv.u);
                v0 = std::min(v0, uv.v); v1 = std::max(v1, uv.v);
            }
            std::printf("  uvBounds=%.9g,%.9g,%.9g,%.9g\n", u0, v0, u1, v1);
        }
        if (hierarchy) {
            std::printf("  tags=");
            for (int b = 0; b < hierarchy->numNodes; ++b) std::printf("%s%d", b ? "," : "", hierarchy->nodeInfo[b].id);
            std::printf("\n");
        }
        for (int m = 0; m < g->matList.numMaterials; ++m) {
            auto* mat = g->matList.materials[m];
            std::printf("  material index=%d rgba=%u,%u,%u,%u texture=%s\n", m, mat->color.red, mat->color.green,
                mat->color.blue, mat->color.alpha, mat->texture ? mat->texture->name : "none");
        }
    }
    TexSample_FreeLinked(linked);
}
void InspectDff(Archive& archive, const char* name) {
    auto bytes = archive.Read(name);
    size_t offset = 0;
    int clumps = 0;
    // FileLoader.cpp:379-405: complex player parts contain successive clumps.
    // TexSample_LinkedParse alone reads only the first (Ripped, not Normal).
    while (offset + 12 <= bytes.size()) {
        const uint32 type = U32(bytes.data() + offset);
        if (type == 0) break; // IMG sector padding
        const size_t length = size_t(U32(bytes.data() + offset + 4)) + 12;
        Require(length <= bytes.size() - offset, "DFF top-level chunk bounds");
        if (type == rw::ID_CLUMP) InspectClump(bytes.data() + offset, length, name, clumps++);
        offset += length;
    }
    Require(clumps > 0, "DFF has clumps");
    std::printf("dff-summary name=%s clumps=%d\n", name, clumps);
}
void InspectTxd(Archive& archive, const char* name) {
    auto bytes = archive.Read(name);
    rw::StreamMemory stream;
    stream.open(bytes.data(), uint32(bytes.size()));
    Require(rw::findChunk(&stream, rw::ID_TEXDICTIONARY, nullptr, nullptr), "TXD chunk");
    auto* dict = rw::TexDictionary::streamRead(&stream);
    stream.close();
    Require(dict, "TXD parse");
    FORLIST(link, dict->textures) {
        const auto* tex = rw::Texture::fromDict(link);
        TexImage image;
        Require(TexSample_Decode(tex, image), "TXD decode");
        int zero = 0, opaque = 0, fractional = 0;
        for (size_t i = 3; i < image.rgba.size(); i += 4) {
            zero += image.rgba[i] == 0;
            opaque += image.rgba[i] == 255;
            fractional += image.rgba[i] > 0 && image.rgba[i] < 255;
        }
        std::printf("txd name=%s texture=%s width=%d height=%d alphaZero=%d alphaOpaque=%d alphaFractional=%d\n",
            name, tex->name, image.w, image.h, zero, opaque, fractional);
        if (!std::strcmp(name, "torso.txd") || !std::strcmp(name, "vest.txd")) {
            Require(s_overlayInputs.emplace(name, std::move(image)).second, "one overlay input texture");
        }
    }
    dict->destroy();
}
void InspectOverlay() {
    auto result = s_overlayInputs.at("torso.txd");
    const auto& overlay = s_overlayInputs.at("vest.txd");
    Require(result.w == overlay.w && result.h == overlay.h && result.rgba.size() == overlay.rgba.size(), "overlay dimensions");
    int replaced = 0, changed = 0, transparent = 0;
    // Exact PlaceTextureOnTopOfTexture rule (ClothesBuilder.cpp:353).
    // This validates the primitive, not ConstructTextures' choice/order.
    for (size_t i = 0; i < result.rgba.size(); i += 4) {
        if (overlay.rgba[i + 3] != 0) {
            ++replaced;
            changed += !std::equal(overlay.rgba.begin() + i, overlay.rgba.begin() + i + 4, result.rgba.begin() + i);
            std::copy_n(overlay.rgba.begin() + i, 4, result.rgba.begin() + i);
        }
        transparent += result.rgba[i + 3] == 0;
    }
    std::printf("overlay-primitive base=torso.txd overlay=vest.txd replaced=%d changed=%d resultTransparent=%d outfitSelectionAsserted=0\n", replaced, changed, transparent);
}
NativePlayerClothes BodyDescriptor() {
    NativePlayerClothes d;
    d.Textures[0] = {{"torso", "torso"}};
    d.Textures[1] = {{"head", "head8bit"}};
    d.Textures[2] = {{"legs", "legs"}};
    d.Textures[3] = {{"feet", "feet8bit"}};
    return d;
}
rw::Matrix Matrix(const NativePlayerMatrix& m) {
    rw::Matrix out;
    out.setIdentity();
    out.right = {m.Right[0], m.Right[1], m.Right[2]};
    out.up = {m.Up[0], m.Up[1], m.Up[2]};
    out.at = {m.At[0], m.At[1], m.At[2]};
    out.pos = {m.Pos[0], m.Pos[1], m.Pos[2]};
    out.flags = 0;
    return out;
}
void ValidateAssembly(const NativePlayerAssets& a, const char* label, size_t vertices, size_t triangles) {
    Require(a.Bones.size() == 32 && a.Vertices.size() == vertices && a.Triangles.size() == triangles, "assembled counts");
    std::set<int> used;
    std::vector<rw::Matrix> world(32), skin(32);
    double matrixError = 0, pointError = 0, weightError = 0;
    for (size_t i = 0; i < 32; ++i) {
        const auto& b = a.Bones[i];
        Require(b.Parent < int(i) && b.Parent >= -1, "owned parent ordering");
        auto local = Matrix(b.BindLocal);
        world[i] = local;
        if (b.Parent >= 0) rw::Matrix::mult(&world[i], &local, &world[b.Parent]);
        auto inverse = Matrix(b.InverseBind);
        rw::Matrix::mult(&skin[i], &inverse, &world[i]);
        const rw::V3d points[]{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
        for (const auto& p : points) {
            rw::V3d result;
            rw::V3d::transformPoints(&result, &p, 1, &skin[i]);
            matrixError = std::max(matrixError, double(std::max({std::abs(result.x - p.x), std::abs(result.y - p.y), std::abs(result.z - p.z)})));
        }
        if (!s_playerInverse.empty()) {
            const auto& expected = s_playerInverse.at(b.Tag);
            for (int k = 0; k < 3; ++k) {
                Require(b.InverseBind.Right[k] == expected[k] && b.InverseBind.Up[k] == expected[4 + k] &&
                    b.InverseBind.At[k] == expected[8 + k] && b.InverseBind.Pos[k] == expected[12 + k], "exact BASE inverse bind coefficients");
            }
        }
    }
    float minV = INFINITY;
    for (const auto& v : a.Vertices) {
        const rw::V3d input{v.Position[0], v.Position[1], v.Position[2]};
        rw::V3d result{0, 0, 0};
        double sum = 0;
        for (int k = 0; k < 4; ++k) {
            Require(v.Bones[k] < 32, "owned skin indices");
            sum += v.Weights[k];
            if (!v.Weights[k]) continue;
            used.insert(a.Bones[v.Bones[k]].Tag);
            rw::V3d p;
            rw::V3d::transformPoints(&p, &input, 1, &skin[v.Bones[k]]);
            result.x += p.x * v.Weights[k]; result.y += p.y * v.Weights[k]; result.z += p.z * v.Weights[k];
        }
        weightError = std::max(weightError, std::abs(sum - 1));
        pointError = std::max(pointError, double(std::max({std::abs(result.x - input.x), std::abs(result.y - input.y), std::abs(result.z - input.z)})));
        minV = std::min(minV, v.UV[1]);
    }
    Require(matrixError < 0.0001 && pointError < 0.0001 && weightError < 0.0001, "real base bind reconstruction and skinned identity");
    constexpr uint32 slots[]{0, 1, 0, 2, 3};
    for (size_t i = 0; i < a.Parts.size(); ++i) {
        const auto& p = a.Parts[i];
        Require(a.Materials[i].Image == slots[i] && a.Materials[i].RGBA == std::array<uint8_t, 4>{255, 255, 255, 255}, "source white body-slot materials");
        for (size_t t = p.FirstTriangle; t < p.FirstTriangle + p.TriangleCount; ++t) {
            Require(a.Triangles[t].Material == i, "triangle body-slot material");
            for (auto v : a.Triangles[t].Vertices) Require(v >= p.FirstVertex && v < p.FirstVertex + p.VertexCount, "merged triangle range");
        }
    }
    std::printf("assembly name=%s vertices=%zu triangles=%zu bones=%zu weightedTags=%zu weightedMissing=0 materials=5 images=4 bindMatrixError=%.9g bindPointError=%.9g weightError=%.9g minV=%.9g\n",
        label, a.Vertices.size(), a.Triangles.size(), a.Bones.size(), used.size(), matrixError, pointError, weightError, minV);
    for (const auto& image : a.Images) std::printf(" assembled-image slot=%s size=%dx%d rgbaBytes=%zu filter=0x%x\n", image.Name.c_str(), image.Width, image.Height, image.RGBA.size(), image.FilterAddressing);
}
NativePlayerAssets TestAssembly() {
    const auto descriptor = BodyDescriptor();
    NativePlayerAssets body;
    std::string error;
    Require(NativePlayerAssets_Load(descriptor, body, error), error.c_str());
    ValidateAssembly(body, "canonical-explicit-body", 1745, 2366);
    auto outfit = descriptor;
    outfit.Models = {"vest", "head", "hands", "jeans", "sneaker"};
    outfit.Textures[0].push_back({"vest", "vest"});
    outfit.Textures[2] = {{"jeansdenim", "legs"}};
    outfit.Textures[3] = {{"sneakerbincblk", "sneakerbincblk"}};
    NativePlayerAssets clothed;
    Require(NativePlayerAssets_Load(outfit, clothed, error), error.c_str());
    ValidateAssembly(clothed, "explicit-vest-jeans-sneaker", 1721, 2332);
    // Obtain independent overlay pixels (constructor is not the oracle).
    if (s_overlayInputs.empty()) {
        Archive player("models/player.img");
        InspectTxd(player, "torso.txd");
        InspectTxd(player, "vest.txd");
    }
    const auto& overlay = s_overlayInputs.at("vest.txd").rgba;
    const auto& original = s_overlayInputs.at("torso.txd").rgba;
    Require(body.Images[0].RGBA == original, "base texture exact decoded RGBA");
    size_t changed = 0;
    for (size_t p = 0; p < overlay.size(); p += 4) {
        const auto& expected = overlay[p + 3] ? overlay : original;
        Require(std::equal(expected.begin() + p, expected.begin() + p + 4, clothed.Images[0].RGBA.begin() + p), "exact source RGBA alpha-overwrite composition");
        changed += !std::equal(original.begin() + p, original.begin() + p + 4, clothed.Images[0].RGBA.begin() + p);
    }
    Require(changed == 31625, "measured torso/vest overlay effect");
    // Late failure after base/texture parsing must preserve output storage
    // and content; also validate invalid descriptor and absent assets.
    const auto* storage = clothed.Vertices.data();
    const auto* pixels = clothed.Images[0].RGBA.data();
    for (int test = 0; test < 6; ++test) {
        auto bad = outfit;
        if (test == 0) bad.MuscleStat = 1;
        if (test == 1) bad.FatStat = 201;
        if (test == 2) bad.Models[4] = "andre";
        if (test == 3) bad.Textures[3][0].Txd = "missing_asset";
        if (test == 4) bad.Textures[3][0].Texture = "missing_texture";
        if (test == 5) bad.Textures[3].push_back({"head", "head8bit"});
        Require(!NativePlayerAssets_Load(bad, clothed, error) && !error.empty(), "unsupported/missing/bad asset must fail");
        Require(storage == clothed.Vertices.data() && pixels == clothed.Images[0].RGBA.data() && clothed.Vertices.size() == 1721 && clothed.Triangles.size() == 2332, "atomic failure preserves output");
        std::printf("assembly-reject test=%d error=%s preserved=1\n", test, error.c_str());
    }
    std::printf("assembly-overlay changedPixels=%zu exactRGBA=1 engineReuse=1 newGameOutfitAsserted=0\n", changed);
    return clothed;
}

void TestRetainedInfluences() {
    std::array<NativePlayerVertex, 3> inputs{};
    inputs[0].Bones = {8, 3, 5, 9};
    inputs[0].Weights = {.1f, .2f, .3f, .4f};
    inputs[1].Bones = {3, 7, 2, 1};
    inputs[1].Weights = {.1f, .2f, .3f, .4f};
    inputs[2].Bones = {7, 8, 6, 4};
    inputs[2].Weights = {.1f, .2f, .3f, .4f};
    auto result = NativePlayerAssets_ProbeBlendSkin(inputs, {.5f, 0, .5f});
    Require(result.Bones == std::array<uint8_t, 4>{8, 3, 5, 9}, "retained influences first-seen, no sorting");
    // Ripped's second influence merges into Normal's first even though the
    // fifth index was already seen. Discarded weights do not dilute retained4.
    const std::array<float, 4> expected{.15f / .6f, .1f / .6f, .15f / .6f, .2f / .6f};
    for (size_t k = 0; k < 4; ++k) Require(std::abs(result.Weights[k] - expected[k]) < 1e-7, "fifth-nonzero retained sum division");
    result = NativePlayerAssets_ProbeBlendSkin(inputs, {0, .5f, .5f});
    Require(result.Bones == std::array<uint8_t, 4>{3, 7, 2, 1}, "zero Normal skipped, Fat precedes Ripped");
    inputs[0].Bones = {2, 2, 3, 255};
    inputs[0].Weights = {.2f, .3f, .49996f, 0};
    result = NativePlayerAssets_ProbeBlendSkin(inputs, {1, 0, 0});
    Require(result.Bones == std::array<uint8_t, 4>{2, 3, 0, 0} && result.Weights == std::array<float, 4>{.5f, .49996f, 0, 0}, "duplicate merging and no renormalization without fifth");
    inputs = {};
    result = NativePlayerAssets_ProbeBlendSkin(inputs, {.95f, 0, .05f});
    Require(result.Weights == std::array<float, 4>{} && result.Bones == std::array<uint8_t, 4>{}, "exact zero contributions skipped");
    std::puts("blend-skin-edge-fixtures firstSeen=1 zeroRatio=1 duplicateMerge=1 retained4=1 fifthOnlyRenormalize=1");

    NativePlayerImage normal{"", 3, 1, 0, {100, 101, 102, 17, 10, 20, 30, 40, 30, 40, 50, 60}};
    NativePlayerImage fat = normal, ripped = normal;
    fat.RGBA[3] = 255;
    ripped.RGBA[3] = 0;
    const NativePlayerImage overlay{"", 3, 1, 0, {255, 255, 255, 0, 1, 2, 3, 1, 4, 5, 6, 128}};
    const auto image = NativePlayerAssets_ProbeBlendImage(normal, fat, ripped, overlay, {.95f, 0, .05f});
    // Stored .95f + .05f is just BELOW one in x87 precision: truncation
    // yields 99/100/101 even when both source RGB triplets are identical.
    Require(image.RGBA == std::vector<uint8_t>{99, 100, 101, 17, 1, 2, 3, 1, 4, 5, 6, 128}, "RGB wide truncation, retained base alpha, fractional full RGBA replacement");
    std::puts("blend-texture-edge-fixtures wideTruncation=1 baseAlpha=1 transparentSkip=1 fractionalRGBAReplacement=1");
}

struct BodyInput {
    std::vector<NativePlayerVertex> Vertices;
    std::vector<std::array<uint32_t, 3>> Triangles;
    std::vector<int> Tags;
};
std::array<BodyInput, 3> ReadBodyInputs(Archive& archive, const std::string& model) {
    const auto bytes = archive.Read(model + ".dff");
    std::array<BodyInput, 3> result;
    size_t offset = 0, count = 0;
    while (offset + 12 <= bytes.size() && U32(bytes.data() + offset)) {
        const size_t length = 12 + U32(bytes.data() + offset + 4);
        Require(length <= bytes.size() - offset, "independent variant chunk bounds");
        if (U32(bytes.data() + offset) == rw::ID_CLUMP) {
            Require(count < 3, "exactly three variant clumps");
            // Independent stream order: Ripped, Fat, Normal.
            auto& input = result[2 - count++];
            auto linked = TexSample_LinkedParse(bytes.data() + offset, length, nullptr, nullptr, 0);
            Require(linked.clump, "independent variant parse");
            FORLIST(link, linked.clump->atomics) {
                auto* atomic = rw::Atomic::fromClump(link);
                auto* g = atomic->geometry;
                auto* skin = rw::Skin::get(g);
                auto* hierarchy = rw::HAnimHierarchy::find(atomic->getFrame());
                Require(input.Vertices.empty() && skin && hierarchy, "independent single skinned atomic");
                for (int b = 0; b < hierarchy->numNodes; ++b) input.Tags.push_back(hierarchy->nodeInfo[b].id);
                for (int v = 0; v < g->numVertices; ++v) {
                    const auto p = g->morphTargets[0].vertices[v], n = g->morphTargets[0].normals[v];
                    const auto uv = g->texCoords[0][v];
                    NativePlayerVertex vertex{{p.x, p.y, p.z}, {n.x, n.y, n.z}, {uv.u, uv.v}, {}, {}};
                    std::copy_n(skin->weights + v * 4, 4, vertex.Weights.begin());
                    std::copy_n(skin->indices + v * 4, 4, vertex.Bones.begin());
                    input.Vertices.push_back(vertex);
                }
                for (int t = 0; t < g->numTriangles; ++t) input.Triangles.push_back({g->triangles[t].v[0], g->triangles[t].v[1], g->triangles[t].v[2]});
            }
            TexSample_FreeLinked(linked);
        }
        offset += length;
    }
    Require(count == 3, "all Normal/Fat/Ripped input clumps read");
    return result;
}

void ValidateBodyBlend(Archive& player, const NativePlayerAssets& output, const std::array<float, 3>& ratios) {
    double maxPosition = 0, maxNormal = 0, maxUV = 0, maxWeight = 0, maxUnitError = 0;
    size_t vertices = 0, truncated = 0, changed = 0;
    for (const auto& part : output.Parts) {
        const auto input = ReadBodyInputs(player, part.Model);
        for (const auto& variant : input) Require(variant.Vertices.size() == part.VertexCount && variant.Tags == input[0].Tags, "independent variant correspondence");
        for (size_t v = 0; v < part.VertexCount; ++v) {
            const auto& actual = output.Vertices[part.FirstVertex + v];
            double normal[3]{}, position[3]{}, uv[2]{};
            std::map<uint8_t, float> weightByIndex;
            std::vector<uint8_t> seen;
            for (size_t s = 0; s < 3; ++s) {
                const auto& source = input[s].Vertices[v];
                for (size_t k = 0; k < 3; ++k) {
                    position[k] += double(source.Position[k]) * ratios[s];
                    normal[k] += double(source.Normal[k]) * ratios[s];
                }
                for (size_t k = 0; k < 2; ++k) uv[k] += double(source.UV[k]) * ratios[s];
                for (size_t k = 0; k < 4; ++k) {
                    const float contribution = source.Weights[k] * ratios[s];
                    if (contribution == 0) continue;
                    const uint8_t index = source.Bones[k];
                    if (!weightByIndex.contains(index)) seen.push_back(index);
                    weightByIndex[index] += contribution;
                }
            }
            const double length = std::sqrt(normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2]);
            double actualLength = 0;
            for (size_t k = 0; k < 3; ++k) {
                maxPosition = std::max(maxPosition, std::abs(actual.Position[k] - position[k]));
                maxNormal = std::max(maxNormal, std::abs(actual.Normal[k] - normal[k] / length));
                actualLength += double(actual.Normal[k]) * actual.Normal[k];
            }
            maxUnitError = std::max(maxUnitError, std::abs(std::sqrt(actualLength) - 1));
            for (size_t k = 0; k < 2; ++k) maxUV = std::max(maxUV, std::abs(actual.UV[k] - uv[k]));
            float retained = 0;
            for (size_t k = 0; k < std::min(size_t(4), seen.size()); ++k) retained += weightByIndex.at(seen[k]);
            truncated += seen.size() > 4;
            for (size_t k = 0; k < 4; ++k) {
                float expected = 0;
                int bone = 0;
                if (k < seen.size()) {
                    expected = weightByIndex.at(seen[k]);
                    if (seen.size() > 4) expected /= retained;
                    const int tag = input[0].Tags.at(seen[k]);
                    const auto it = std::find_if(output.Bones.begin(), output.Bones.end(), [&](const auto& b) { return b.Tag == tag; });
                    if (it != output.Bones.end()) bone = int(it - output.Bones.begin());
                }
                Require(actual.Bones[k] == bone, "every blended influence remapped via Normal node IDs");
                maxWeight = std::max(maxWeight, double(std::abs(actual.Weights[k] - expected)));
            }
            changed += actual.Position != input[0].Vertices[v].Position;
            ++vertices;
        }
        for (size_t t = 0; t < part.TriangleCount; ++t) for (size_t k = 0; k < 3; ++k) {
            Require(output.Triangles[part.FirstTriangle + t].Vertices[k] == part.FirstVertex + input[0].Triangles[t][k], "exact Normal topology with assembly offsets");
        }
    }
    Require(vertices == output.Vertices.size() && changed > 0 && maxPosition < 2e-7 && maxNormal < 2e-7 && maxUV < 2e-7 && maxWeight < 2e-7 && maxUnitError < 2e-7, "all-vertex independent three-clump numerical blend");
    std::printf("body-blend ratios=%.9g,%.9g,%.9g vertices=%zu changed=%zu retained4Vertices=%zu maxPosition=%.9g maxNormal=%.9g maxUV=%.9g maxWeight=%.9g normalUnitError=%.9g\n", ratios[0], ratios[1], ratios[2], vertices, changed, truncated, maxPosition, maxNormal, maxUV, maxWeight, maxUnitError);
}

std::map<std::string, TexImage> ReadTextureInputs(Archive& player, const char* name) {
    auto bytes = player.Read(std::string(name) + ".txd");
    rw::StreamMemory stream;
    stream.open(bytes.data(), uint32(bytes.size()));
    Require(rw::findChunk(&stream, rw::ID_TEXDICTIONARY, nullptr, nullptr), "independent body TXD chunk");
    auto* dict = rw::TexDictionary::streamRead(&stream);
    stream.close();
    Require(dict, "independent body TXD parse");
    std::map<std::string, TexImage> images;
    FORLIST(link, dict->textures) {
        const auto* texture = rw::Texture::fromDict(link);
        Require(TexSample_Decode(texture, images[texture->name]), "independent body TXD decode");
    }
    dict->destroy();
    return images;
}

void ValidateStartupTextures(Archive& player, const NativePlayerAssets& output, bool withOverlay = true) {
    for (size_t slot : {size_t(0), size_t(2)}) {
        const bool torso = slot == 0;
        const std::string name = torso ? "torso" : "legs";
        const auto body = ReadTextureInputs(player, torso ? "player_torso" : "player_legs");
        const auto garment = ReadTextureInputs(player, torso ? "vest" : "jeansdenim");
        Require(body.size() == 3 && garment.size() == 1, "actual body variants and first garment texture");
        const auto& normal = body.at(name);
        const auto& fat = body.at(name + "_fat").rgba;
        const auto& ripped = body.at(name + "_ripped").rgba;
        const auto& overlay = garment.begin()->second.rgba;
        const auto& actual = output.Images[slot];
        Require(actual.Width == normal.w && actual.Height == normal.h && actual.RGBA.size() == normal.rgba.size() && overlay.size() == normal.rgba.size(), "startup image dimensions");
        size_t retained = 0, replaced = 0, fractional = 0, changed = 0;
        for (size_t p = 0; p < actual.RGBA.size(); p += 4) {
            for (size_t c = 0; c < 4; ++c) {
                uint8_t expected = normal.rgba[p + c];
                if (c != 3) {
                    // Independent wide arithmetic, truncating ONLY at byte
                    // conversion. float ratios are the source's stored inputs.
                    const long double value = normal.rgba[p + c] * (long double).95f + fat[p + c] * (long double)0.0f + ripped[p + c] * (long double).05f;
                    expected = uint8_t(value);
                }
                if (withOverlay && overlay[p + 3]) expected = overlay[p + c];
                Require(actual.RGBA[p + c] == expected, "exact startup RGB truncation, base alpha and full RGBA overlay");
            }
            retained += !withOverlay || overlay[p + 3] == 0;
            replaced += withOverlay && overlay[p + 3] != 0;
            fractional += withOverlay && overlay[p + 3] > 0 && overlay[p + 3] < 255;
            changed += !std::equal(actual.RGBA.begin() + p, actual.RGBA.begin() + p + 4, normal.rgba.begin() + p);
        }
        std::printf("startup-texture slot=%zu overlay=%d exactBytes=%zu retainedBaseAlpha=%zu replacedRGBA=%zu fractionalOverlay=%zu changed=%zu\n", slot, withOverlay, actual.RGBA.size(), retained, replaced, fractional, changed);
    }
    const auto face = ReadTextureInputs(player, "player_face");
    Require(face.size() == 1 && face.contains("face") && !face.contains("face_fat"), "player_face has face, no face_fat");
    Require(output.Images[1].RGBA == face.at("face").rgba, "startup head exact player_face:face copy");
    const auto feet = ReadTextureInputs(player, "sneakerbincblk");
    Require(feet.size() == 1 && output.Images[3].RGBA == feet.begin()->second.rgba, "startup feet exact first sneaker texture copy");
    std::puts("startup-texture head=player_face:face faceFatAbsent=1 feet=sneakerbincblk exactRGBA=1");
}

NativePlayerAssets TestStartup() {
    TestRetainedInfluences();
    auto descriptor = NativePlayerClothes_Startup();
    Require(descriptor.FatStat == 200 && descriptor.MuscleStat == 50 && descriptor.BlendBody && descriptor.Models == std::array<std::string, 5>{"vest", "head", "", "jeans", "sneaker"}, "source startup descriptor stats and model slots");
    NativePlayerAssets startup;
    std::string error;
    Require(NativePlayerAssets_Load(descriptor, startup, error), error.c_str());
    ValidateAssembly(startup, "source-startup-200-50", 1721, 2332);
    Archive player("models/player.img");
    ValidateBodyBlend(player, startup, {.95f, 0, .05f});
    ValidateStartupTextures(player, startup);
    const auto* storage = startup.Vertices.data();
    const auto* pixels = startup.Images[0].RGBA.data();
    const auto saved = startup;
    for (int test = 0; test < 5; ++test) {
        auto bad = descriptor;
        if (test == 0) bad.MuscleStat = NAN;
        if (test == 1) bad.FatStat = 1001;
        if (test == 2) bad.Textures[0][0].FatTexture = "missing_fat"; // zero ratio still requires actual source
        if (test == 3) bad.Textures[2][0].RippedTexture.clear();
        if (test == 4) bad.Textures[3][0].Texture = "missing_texture";
        Require(!NativePlayerAssets_Load(bad, startup, error) && !error.empty(), "bad startup must fail");
        Require(storage == startup.Vertices.data() && pixels == startup.Images[0].RGBA.data(), "startup failure preserves storage");
        for (size_t i = 0; i < startup.Images.size(); ++i) Require(startup.Images[i].RGBA == saved.Images[i].RGBA, "startup failure preserves pixels");
        for (size_t i = 0; i < startup.Vertices.size(); ++i) {
            const auto& a = startup.Vertices[i]; const auto& b = saved.Vertices[i];
            Require(a.Position == b.Position && a.Normal == b.Normal && a.UV == b.UV && a.Bones == b.Bones && a.Weights == b.Weights, "startup failure preserves vertex content");
        }
        std::printf("startup-reject test=%d preserved=1 error=%s\n", test, error.c_str());
    }
    NativePlayerAssets mixed;
    // Jeans are fully opaque. Check both body blends BEFORE overlay as well,
    // otherwise a broken legs RGB operator would be hidden by the garment.
    auto uncovered = descriptor;
    uncovered.Textures[0].pop_back();
    uncovered.Textures[2].pop_back();
    Require(NativePlayerAssets_Load(uncovered, mixed, error), error.c_str());
    ValidateStartupTextures(player, mixed, false);
    descriptor.FatStat = 360;
    descriptor.MuscleStat = 200;
    Require(NativePlayerAssets_Load(descriptor, mixed, error), error.c_str());
    ValidateBodyBlend(player, mixed, {.6f, .2f, .2f});
    descriptor.FatStat = descriptor.MuscleStat = 1000;
    Require(NativePlayerAssets_Load(descriptor, mixed, error), error.c_str());
    ValidateBodyBlend(player, mixed, {0, .5f, .5f});
    return startup;
}
void TestAnimations(const NativePlayerAssets& a) {
    std::vector<uint8> bytes;
    Require(ReadWholeFileOS("anim/ped.ifp", bytes), "read real ped.ifp via OS_File");
    std::string bank;
    std::vector<IfpAnimData> anims;
    char error[256]{};
    Require(ParseIfpBank(bytes, bank, anims, error, sizeof(error)), error);
    for (const char* name : {"idle_stance", "walk_civi"}) {
        const auto it = std::find_if(anims.begin(), anims.end(), [&](const auto& anim) { return ToLowerCopy(anim.name) == name; });
        Require(it != anims.end(), name);
        std::vector<rw::V3d> first;
        double motion = 0;
        for (double time : {0.25, 0.75}) {
            std::vector<BlendSampled> samples;
            SampleLegacyAnim(*it, it->total * time, samples);
            std::vector<rw::Matrix> world(32), skin(32);
            int mapped = 0;
            for (size_t b = 0; b < a.Bones.size(); ++b) {
                const auto& bone = a.Bones[b];
                rw::Matrix local = Matrix(bone.BindLocal);
                for (size_t s = 0; s < it->seqs.size(); ++s) {
                    const auto& sequence = it->seqs[s];
                    const int tag = sequence.tag >= 0 ? sequence.tag : BoneNameToTag(sequence.name);
                    if (tag != bone.Tag || !samples[s].valid) continue;
                    const auto& sample = samples[s];
                    // IfpAnim's retail BonePos comes from BASE inverse binds,
                    // not part bind matrices or arbitrary frame translations.
                    QuatPosToMatrix(sample.q, sample.hasT ? sample.t : bone.BindLocal.Pos.data(), local);
                    ++mapped;
                    break;
                }
                world[b] = local;
                if (bone.Parent >= 0) rw::Matrix::mult(&world[b], &local, &world[bone.Parent]);
                const auto inverse = Matrix(bone.InverseBind);
                rw::Matrix::mult(&skin[b], &inverse, &world[b]); // assembled atomic identity
            }
            Require(mapped == 32, "all CJ base bones mapped to real idle/walk sequences");
            std::array<float, 3> low{INFINITY, INFINITY, INFINITY}, high{-INFINITY, -INFINITY, -INFINITY};
            for (size_t v = 0; v < a.Vertices.size(); ++v) {
                const auto& vertex = a.Vertices[v];
                rw::V3d input{vertex.Position[0], vertex.Position[1], vertex.Position[2]}, output{0, 0, 0};
                for (int k = 0; k < 4; ++k) {
                    rw::V3d point;
                    rw::V3d::transformPoints(&point, &input, 1, &skin[vertex.Bones[k]]);
                    output.x += point.x * vertex.Weights[k]; output.y += point.y * vertex.Weights[k]; output.z += point.z * vertex.Weights[k];
                }
                const float coords[]{output.x, output.y, output.z};
                for (int k = 0; k < 3; ++k) {
                    Require(std::isfinite(coords[k]), "finite CJ animated vertex");
                    low[k] = std::min(low[k], coords[k]); high[k] = std::max(high[k], coords[k]);
                }
                if (time == 0.25) first.push_back(output);
                else motion += std::sqrt(std::pow(double(output.x - first[v].x), 2) + std::pow(double(output.y - first[v].y), 2) + std::pow(double(output.z - first[v].z), 2));
            }
            Require(high[2] - low[2] > 1 && high[2] - low[2] < 3, "CJ animated body height");
            std::printf("assembly-ifp bank=%s clip=%s sample=legacy-key time=%.2f mapped=%d vertices=%zu bbox=%.6g,%.6g,%.6g:%.6g,%.6g,%.6g\n", bank.c_str(), it->name, time, mapped, a.Vertices.size(), low[0], low[1], low[2], high[0], high[1], high[2]);
        }
        Require(motion > 0, "real idle/walk keys move CJ mesh");
        std::printf("assembly-ifp-motion clip=%s meanVertexDelta=%.9g\n", it->name, motion / a.Vertices.size());
    }
}
}
int main(int argc, char** argv) {
    OS_SetFilePathOffset(argc > 1 ? argv[1] : "/game");
    const bool constructorOnly = argc > 2 && !std::strcmp(argv[2], "--constructor-only");
    if (!constructorOnly) {
        InitRw();
        Archive gta("models/gta3.img"), player("models/player.img");
        InspectDff(gta, "player.dff");
        for (const char* name : {"torso.dff", "head.dff", "hands.dff", "legs.dff", "feet.dff", "vest.dff", "jeans.dff", "sneaker.dff"}) {
            if (player.entries.contains(name)) InspectDff(player, name);
            else std::printf("missing player.img:%s\n", name);
        }
        for (const auto& [name, e] : player.entries) {
            if (name == "player.txd" || name.starts_with("torso") || name.starts_with("head") || name.starts_with("vest") ||
                name.starts_with("jeansdenim") || name.starts_with("sneakerbinc") || name.starts_with("legs") || name.starts_with("feet")) {
                if (name.ends_with(".txd")) InspectTxd(player, name.c_str());
            }
        }
        if (gta.entries.contains("player.txd")) InspectTxd(gta, "player.txd");
        InspectOverlay();
    }
    TestAssembly();
    const auto owned = TestStartup();
    TestAnimations(owned);
    rw::Engine::stop();
    rw::Engine::close();
    rw::Engine::term();
    ValidateAssembly(owned, "owned-after-RW-term", 1721, 2332);
    std::puts("player-assets assembly-ok (explicit Normal + source-backed startup; SCM VM not executed)");
}
