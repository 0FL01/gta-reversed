#include "app/platform/linux/NativePlayerAssets.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <stdexcept>
#include <utility>

using int32 = int32_t;
using uint32 = uint32_t;
using int64 = int64_t;
using uint64 = uint64_t;
#ifndef __stdcall
#define __stdcall
#endif
#include "oswrapper/oswrapper.h"
#include "app/platform/linux/TexSample.h"
#include <rw.h>

namespace {
void Check(bool ok, const std::string& error) {
    if (!ok) throw std::runtime_error(error);
}
uint32 U32(const uint8_t* p) {
    return uint32(p[0]) | uint32(p[1]) << 8 | uint32(p[2]) << 16 | uint32(p[3]) << 24;
}
std::string Lower(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return s;
}
bool IsStem(const std::string& s) {
    return !s.empty() && s.size() <= 19 && std::all_of(s.begin(), s.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
    });
}
struct Archive {
    void* File = nullptr;
    struct Entry { int32 Offset, Size; };
    std::map<std::string, Entry> Entries;
    ~Archive() { if (File) OS_FileClose(File); }
    void Open(const char* path) {
        Check(OS_FileOpen(FILE_DATA_AREA_DEFAULT, &File, path, FILE_ACCESS_READ) == 0 && File, path);
        const int32 size = OS_FileSize(File);
        uint8_t header[8];
        Check(size >= 8 && OS_FileRead(File, header, 8) == 0 && !std::memcmp(header, "VER2", 4), "invalid IMG header");
        const uint32 count = U32(header + 4);
        Check(count && count <= 300000 && 8ull + count * 32ull <= uint32(size), "IMG directory bounds");
        std::vector<uint8_t> directory(count * 32u);
        Check(OS_FileRead(File, directory.data(), int32(directory.size())) == 0, "IMG directory read");
        for (uint32 i = 0; i < count; ++i) {
            const auto* e = directory.data() + i * 32u;
            const uint64 offset = uint64(U32(e)) * 2048;
            const uint64 length = uint64(U32(e + 4) & 0x7fff) * 2048;
            Check(offset >= 8ull + count * 32ull && offset + length <= uint32(size), "IMG entry bounds");
            std::string name(reinterpret_cast<const char*>(e + 8), strnlen(reinterpret_cast<const char*>(e + 8), 24));
            Check(Entries.emplace(Lower(name), Entry{int32(offset), int32(length)}).second, "duplicate IMG entry");
        }
    }
    std::vector<uint8_t> Read(const std::string& name) {
        const auto it = Entries.find(name);
        Check(it != Entries.end(), "missing IMG asset: " + name);
        const auto e = it->second;
        Check(e.Size > 0, "empty IMG asset: " + name);
        OS_FileSetPosition(File, e.Offset);
        Check(OS_FileGetPosition(File) == e.Offset, "IMG seek: " + name);
        std::vector<uint8_t> bytes(e.Size);
        Check(OS_FileRead(File, bytes.data(), e.Size) == 0, "IMG read: " + name);
        return bytes;
    }
};
void InitRw() {
    if (rw::Engine::state == rw::Engine::Started) return;
    Check(rw::Engine::state == rw::Engine::Dead, "librw must be Dead or Started");
    Check(rw::Engine::init(nullptr), "librw init");
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
    if (!rw::Engine::open(nullptr)) {
        rw::Engine::term();
        Check(false, "librw open");
    }
    if (!rw::Engine::start()) {
        rw::Engine::close();
        rw::Engine::term();
        Check(false, "librw start");
    }
    rw::Texture::setLoadTextures(false);
}

struct Chunk { uint32 Type; const uint8_t* Data; size_t Size; };
std::vector<Chunk> Chunks(const uint8_t* bytes, size_t size, bool padding = false) {
    std::vector<Chunk> chunks;
    while (size) {
        if (padding && std::all_of(bytes, bytes + size, [](uint8_t b) { return b == 0; })) break;
        Check(size >= 12, "truncated RW chunk header");
        const size_t length = U32(bytes + 4);
        Check(length <= size - 12, "RW chunk bounds");
        chunks.push_back({U32(bytes), bytes + 12, length});
        bytes += length + 12;
        size -= length + 12;
    }
    return chunks;
}
// Read the selected atomic's FrameList name directly. This also works when
// IfpAnim initialized librw WITHOUT a node-name plugin: never add plugins to
// a running engine. FileLoader.cpp:379 reads successive complex-part clumps.
void CheckVariant(const Chunk& clump, const char* variant) {
    std::vector<std::string> names;
    int atomicFrame = -1, atomicCount = 0;
    for (const auto& c : Chunks(clump.Data, clump.Size)) {
        if (c.Type == rw::ID_FRAMELIST) {
            const auto frames = Chunks(c.Data, c.Size);
            Check(!frames.empty() && frames[0].Type == rw::ID_STRUCT && frames[0].Size >= 4, "DFF FrameList struct");
            const size_t count = U32(frames[0].Data);
            Check(count <= 256 && frames[0].Size == 4 + count * 56 && frames.size() == count + 1, "DFF FrameList bounds");
            names.resize(count);
            for (size_t i = 0; i < count; ++i) {
                Check(frames[i + 1].Type == rw::ID_EXTENSION, "DFF frame extension");
                for (const auto& ext : Chunks(frames[i + 1].Data, frames[i + 1].Size)) {
                    if (ext.Type == 0x253f2fe) {
                        Check(ext.Size < 64, "DFF frame name bounds");
                        names[i] = Lower(std::string(reinterpret_cast<const char*>(ext.Data), strnlen(reinterpret_cast<const char*>(ext.Data), ext.Size)));
                    }
                }
            }
        } else if (c.Type == rw::ID_ATOMIC) {
            const auto atomic = Chunks(c.Data, c.Size);
            Check(!atomic.empty() && atomic[0].Type == rw::ID_STRUCT && atomic[0].Size >= 16, "DFF atomic struct");
            atomicFrame = int(U32(atomic[0].Data));
            ++atomicCount;
        }
    }
    Check(atomicCount == 1 && atomicFrame >= 0 && size_t(atomicFrame) < names.size() && names[atomicFrame] == variant, "selected DFF clump is not single-atomic " + std::string(variant));
}
struct Parsed {
    LinkedClump Linked;
    ~Parsed() { TexSample_FreeLinked(Linked); }
    void Read(Archive& archive, const std::string& model, bool base, size_t variant = 0) {
        const auto bytes = archive.Read(model + ".dff");
        const auto chunks = Chunks(bytes.data(), bytes.size(), true);
        std::vector<Chunk> clumps;
        for (const auto& c : chunks) if (c.Type == rw::ID_CLUMP) clumps.push_back(c);
        Check(clumps.size() == (base ? 1u : 3u), "unsupported clump count: " + model);
        constexpr const char* names[]{"normal", "fat", "ripped"};
        Check(variant < 3, "invalid body variant");
        const auto& normal = clumps[base ? 0 : 2 - variant]; // retail player.img Ripped, Fat, Normal
        CheckVariant(normal, names[variant]);
        Linked = TexSample_LinkedParse(normal.Data - 12, normal.Size + 12, nullptr, nullptr, 0);
        Check(Linked.clump, "DFF parse: " + model);
    }
    rw::Atomic* Atomic() {
        rw::Atomic* result = nullptr;
        FORLIST(link, Linked.clump->atomics) {
            Check(!result, "multiple body atomics");
            result = rw::Atomic::fromClump(link);
        }
        Check(result && result->geometry, "missing body geometry");
        return result;
    }
};
rw::HAnimHierarchy* Hierarchy(rw::Atomic* atomic) {
    auto* h = rw::HAnimHierarchy::find(atomic->getFrame());
    auto* skin = rw::Skin::get(atomic->geometry);
    Check(h && skin && h->numNodes > 0 && h->numNodes <= 256 && skin->numBones == h->numNodes && skin->inverseMatrices, "invalid skin hierarchy");
    h->attach();
    return h;
}
NativePlayerMatrix Own(const rw::Matrix& m) {
    NativePlayerMatrix out{{m.right.x, m.right.y, m.right.z}, {m.up.x, m.up.y, m.up.z}, {m.at.x, m.at.y, m.at.z}, {m.pos.x, m.pos.y, m.pos.z}};
    for (const auto& v : {out.Right, out.Up, out.At, out.Pos}) for (float f : v) Check(std::isfinite(f), "nonfinite bone matrix");
    return out;
}
void LoadBones(Parsed& base, NativePlayerAssets& out) {
    auto* atomic = base.Atomic();
    auto* h = Hierarchy(atomic);
    auto* skin = rw::Skin::get(atomic->geometry);
    Check(h->numNodes == 32, "MODEL_PLAYER must have 32 bones");
    std::vector<rw::Matrix> inverse(32);
    std::vector<int> stack;
    std::map<int, int> tags;
    int parent = -1;
    for (int i = 0; i < 32; ++i) {
        const auto& node = h->nodeInfo[i];
        Check(node.frame && tags.emplace(node.id, i).second, "missing/duplicate base bone");
        if (parent >= 0) Check(node.frame->getParent() == h->nodeInfo[parent].frame, "base frame/HAnim parent mismatch");
        std::memcpy(&inverse[i], skin->inverseMatrices + i * 16, 64);
        inverse[i].flags = 0;
        NativePlayerBone bone;
        bone.Tag = node.id;
        bone.Parent = parent;
        bone.Flags = node.flags;
        bone.InverseBind = Own(inverse[i]);
        rw::Matrix world, local;
        rw::Matrix::invert(&world, &inverse[i]);
        local = world;
        if (parent >= 0) rw::Matrix::mult(&local, &world, &inverse[parent]);
        bone.BindLocal = Own(local);
        bone.FrameLocal = Own(node.frame->matrix);
        out.Bones.push_back(bone);
        if (node.flags & rw::HAnimHierarchy::PUSH) stack.push_back(parent);
        parent = i;
        if (node.flags & rw::HAnimHierarchy::POP) {
            Check(!stack.empty() || i == 31, "base HAnim stack underflow");
            parent = stack.empty() ? -1 : stack.back();
            if (!stack.empty()) stack.pop_back();
        }
    }
    Check(stack.empty(), "base HAnim stack imbalance");
}
NativePlayerImage LoadImage(Archive& archive, const NativePlayerTextureLayer& layer) {
    Check(IsStem(layer.Txd) && !layer.Texture.empty() && layer.Texture.size() < 32, "explicit TXD stem/texture required");
    const auto bytes = archive.Read(layer.Txd + ".txd");
    const auto chunks = Chunks(bytes.data(), bytes.size(), true);
    const Chunk* txd = nullptr;
    for (const auto& c : chunks) if (c.Type == rw::ID_TEXDICTIONARY) {
        Check(!txd, "multiple texture dictionaries");
        txd = &c;
    }
    Check(txd, "missing TXD chunk");
    rw::StreamMemory stream;
    stream.open(const_cast<uint8_t*>(txd->Data), uint32(txd->Size));
    auto* dict = rw::TexDictionary::streamRead(&stream);
    stream.close();
    Check(dict, "TXD parse: " + layer.Txd);
    struct Guard { rw::TexDictionary* Dict; ~Guard() { Dict->destroy(); } } guard{dict};
    TexImage decoded;
    bool found = false;
    FORLIST(link, dict->textures) {
        const auto* texture = rw::Texture::fromDict(link);
        if (layer.Texture != texture->name) continue;
        Check(!found && TexSample_Decode(texture, decoded), "duplicate/undecodable texture: " + layer.Texture);
        found = true;
    }
    Check(found && decoded.w > 0 && decoded.h > 0 && decoded.rgba.size() == size_t(decoded.w) * decoded.h * 4, "missing/invalid texture: " + layer.Texture);
    // CopyTexture (ClothesBuilder.cpp:344-345) creates a fresh texture with
    // linear filtering; RW's new-texture addressing is wrap in both axes.
    const uint32 sampler = rw::Texture::LINEAR | (rw::Texture::WRAP << 8) | (rw::Texture::WRAP << 12);
    return {layer.Texture, decoded.w, decoded.h, sampler, std::move(decoded.rgba)};
}
using Ratios = std::array<float, 3>; // Normal, Fat, Ripped
Ratios BodyRatios(const NativePlayerClothes& clothes) {
    Check(std::isfinite(clothes.FatStat) && std::isfinite(clothes.MuscleStat) &&
        clothes.FatStat >= 0 && clothes.FatStat <= 1000 && clothes.MuscleStat >= 0 && clothes.MuscleStat <= 1000, "body stats must be finite in 0..1000");
    if (!clothes.BlendBody) {
        Check(clothes.FatStat <= 200 && clothes.MuscleStat == 0, "explicit Normal preview requires fat 0..200, muscle 0");
        return {1, 0, 0};
    }
    // Owned-retail CalculateRatios 0x5bca10, independently PE-mapped; effective
    // CStats muscle is supplied explicitly here, never descriptor/global lookup.
    float fat = std::max(0.0f, clothes.FatStat - 200.0f) / 800.0f;
    float ripped = clothes.MuscleStat / 1000.0f;
    const float sum = fat + ripped;
    if (sum > 1.0f) return {0, fat / sum, ripped / sum};
    return {1.0f - sum, fat, ripped};
}
void BlendSkin(const std::array<NativePlayerVertex, 3>& source, const Ratios& ratios, NativePlayerVertex& out) {
    // Retail BlendGeometry3 0x5bd28f..7a7: first-seen indices, not largest
    // weights. Keep four; ONLY a nonzero fifth triggers retained-sum division.
    // Twelve slots safely cover all three inputs without the original scratch
    // array's implicit limit. Indices are still in the NORMAL node-ID table.
    std::array<uint8_t, 12> indices{};
    std::array<float, 12> weights{};
    size_t count = 0;
    for (size_t s = 0; s < source.size(); ++s) {
        for (size_t k = 0; k < 4; ++k) {
            const float weight = source[s].Weights[k] * ratios[s];
            if (weight == 0) continue;
            size_t dst = 0;
            while (dst < count && indices[dst] != source[s].Bones[k]) ++dst;
            if (dst == count) indices[count++] = source[s].Bones[k];
            weights[dst] += weight;
        }
    }
    std::copy_n(indices.begin(), 4, out.Bones.begin());
    std::copy_n(weights.begin(), 4, out.Weights.begin());
    if (weights[4] != 0) {
        const float sum = weights[0] + weights[1] + weights[2] + weights[3];
        Check(sum > 0, "empty retained skin influences");
        for (auto& w : out.Weights) w /= sum;
    }
}
void BlendImage(NativePlayerImage& normal, const NativePlayerImage& fat, const NativePlayerImage& ripped, const Ratios& ratios) {
    Check(normal.Width == fat.Width && normal.Height == fat.Height && normal.Width == ripped.Width && normal.Height == ripped.Height, "body texture dimensions differ");
    // Retail 0x5be4a0: x87 weighted RGB, truncation toward zero;
    // do not round intermediate byte products to float. Alpha stays normal.
    for (size_t p = 0; p < normal.RGBA.size(); p += 4) {
        for (size_t c = 0; c < 3; ++c) {
            const double rgb = double(normal.RGBA[p + c]) * ratios[0] + double(fat.RGBA[p + c]) * ratios[1] + double(ripped.RGBA[p + c]) * ratios[2];
            normal.RGBA[p + c] = uint8_t(std::clamp(rgb, 0.0, 255.0));
        }
    }
}
void OverlayImage(NativePlayerImage& image, const NativePlayerImage& overlay) {
    Check(image.Width == overlay.Width && image.Height == overlay.Height, "texture layer dimensions differ");
    // PlaceTextureOnTopOfTexture, ClothesBuilder.cpp:353 / retail 0x5be260:
    // nonzero alpha REPLACES full RGBA, including fractional alpha.
    for (size_t p = 0; p < image.RGBA.size(); p += 4) {
        if (overlay.RGBA[p + 3]) std::copy_n(overlay.RGBA.begin() + p, 4, image.RGBA.begin() + p);
    }
}
void LoadPart(Archive& archive, const std::string& model, size_t slot, bool blend, const Ratios& ratios, NativePlayerAssets& out) {
    std::array<Parsed, 3> parts;
    std::array<rw::Geometry*, 3> geometries{};
    std::array<rw::Skin*, 3> skins{};
    parts[0].Read(archive, model, false);
    auto* atomic = parts[0].Atomic();
    auto* h = Hierarchy(atomic);
    auto* g = atomic->geometry;
    const size_t variants = blend ? 3 : 1;
    for (size_t s = 0; s < variants; ++s) {
        if (s) parts[s].Read(archive, model, false, s);
        auto* a = parts[s].Atomic();
        auto* sourceHierarchy = Hierarchy(a);
        auto* geom = geometries[s] = a->geometry;
        auto* skin = skins[s] = rw::Skin::get(geom);
        Check(geom->numVertices > 0 && geom->numVertices <= 65535 && geom->numVertices == g->numVertices &&
            geom->numTriangles > 0 && geom->numTriangles == g->numTriangles && geom->triangles && geom->numMorphTargets == 1 &&
            geom->morphTargets && geom->morphTargets[0].vertices && geom->morphTargets[0].normals &&
            geom->numTexCoordSets == 1 && geom->texCoords[0] && skin->weights && skin->indices, "unsupported body geometry: " + model);
        Check(sourceHierarchy->numNodes == h->numNodes, "body variant bone count mismatch");
        for (int b = 0; b < h->numNodes; ++b) Check(sourceHierarchy->nodeInfo[b].id == h->nodeInfo[b].id, "body variant node-ID order mismatch");
    }
    std::vector<int> remap(h->numNodes, -1);
    for (int i = 0; i < h->numNodes; ++i) {
        for (size_t b = 0; b < out.Bones.size(); ++b) if (out.Bones[b].Tag == h->nodeInfo[i].id) remap[i] = int(b);
    }
    auto& range = out.Parts[slot];
    range = {model, uint32(out.Vertices.size()), uint32(g->numVertices), uint32(out.Triangles.size()), uint32(g->numTriangles)};
    for (int v = 0; v < g->numVertices; ++v) {
        std::array<NativePlayerVertex, 3> source{};
        for (size_t s = 0; s < variants; ++s) {
            const auto p = geometries[s]->morphTargets[0].vertices[v];
            const auto n = geometries[s]->morphTargets[0].normals[v];
            const auto uv = geometries[s]->texCoords[0][v];
            auto& vertex = source[s];
            vertex = {{p.x, p.y, p.z}, {n.x, n.y, n.z}, {uv.u, uv.v}, {}, {}};
            for (float f : {p.x, p.y, p.z, n.x, n.y, n.z, uv.u, uv.v}) Check(std::isfinite(f), "nonfinite body vertex");
            double sum = 0;
            for (int k = 0; k < 4; ++k) {
                const float w = skins[s]->weights[v * 4 + k];
                const int index = skins[s]->indices[v * 4 + k];
                Check(std::isfinite(w) && w >= 0, "invalid skin weight");
                if (w != 0) Check(index < h->numNodes, "weighted body bone index bounds");
                vertex.Bones[k] = uint8_t(index);
                vertex.Weights[k] = w;
                sum += w;
            }
            Check(std::abs(sum - 1) < 0.0001, "unnormalized source skin weights");
        }
        auto vertex = source[0];
        if (blend) {
            const auto weighted = [&](auto member, size_t k) {
                return float(double((source[0].*member)[k]) * ratios[0] + double((source[1].*member)[k]) * ratios[1] + double((source[2].*member)[k]) * ratios[2]);
            };
            for (size_t k = 0; k < 3; ++k) {
                vertex.Position[k] = weighted(&NativePlayerVertex::Position, k);
                vertex.Normal[k] = weighted(&NativePlayerVertex::Normal, k);
            }
            for (size_t k = 0; k < 2; ++k) vertex.UV[k] = weighted(&NativePlayerVertex::UV, k);
            // Retail 0x5bd237 calls RW normalize after the weighted sum.
            // The upstream unreversed generic helper omits this. sqrt here
            // is mathematically equivalent, not bit-identical to RW's lookup.
            const auto& n = vertex.Normal;
            const float length = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            Check(length > 0 && std::isfinite(length), "degenerate blended normal");
            for (auto& f : vertex.Normal) f /= length;
            BlendSkin(source, ratios, vertex);
        }
        for (size_t k = 0; k < 4; ++k) {
            // Retail assembly maps a missing Normal node ID to base index 0.
            const int index = vertex.Bones[k];
            vertex.Bones[k] = vertex.Weights[k] == 0 || index >= h->numNodes || remap[index] < 0 ? 0 : uint8_t(remap[index]);
        }
        out.Vertices.push_back(vertex);
    }
    for (int t = 0; t < g->numTriangles; ++t) {
        NativePlayerTriangle triangle{{}, uint32(slot)};
        for (int k = 0; k < 3; ++k) {
            const uint32 index = g->triangles[t].v[k];
            Check(index < uint32(g->numVertices), "triangle vertex bounds");
            triangle.Vertices[k] = range.FirstVertex + index;
        }
        // Source creates a single white material per body slot, including
        // head's two original materials (ClothesBuilder.cpp:565-612).
        out.Triangles.push_back(triangle);
    }
}
} // namespace

#ifdef NATIVE_PLAYER_ASSETS_PROBE
// Probe-only entry into the actual retained-influence arithmetic; no runtime API.
NativePlayerVertex NativePlayerAssets_ProbeBlendSkin(const std::array<NativePlayerVertex, 3>& source, const std::array<float, 3>& ratios) {
    NativePlayerVertex result{};
    BlendSkin(source, ratios, result);
    return result;
}
NativePlayerImage NativePlayerAssets_ProbeBlendImage(NativePlayerImage normal, const NativePlayerImage& fat, const NativePlayerImage& ripped, const NativePlayerImage& overlay, const std::array<float, 3>& ratios) {
    BlendImage(normal, fat, ripped, ratios);
    OverlayImage(normal, overlay);
    return normal;
}
#endif

NativePlayerClothes NativePlayerClothes_Startup() {
    // Static owned-retail/SCM facts: 087B at file 59838,59857,59883,59915;
    // 070D at 59948; stats at 56061/56070. Model slot 2 remains zero (hands
    // fallback). Other texture/model keys are zero, so no tattoos/accessories.
    // PreprocessClothesDesc and ConstructTextures remain unreversed upstream;
    // this bounded schema follows retail-clothes-static.py's PE-mapped proof,
    // NOT Compact VA assumptions and NOT a claim of native SCM reachability.
    NativePlayerClothes clothes;
    clothes.Models = {"vest", "head", "", "jeans", "sneaker"};
    clothes.Textures[0] = {{"player_torso", "torso", "torso_fat", "torso_ripped"}, {"vest", "vest"}};
    clothes.Textures[1] = {{"player_face", "face"}}; // face_fat absent: copy, no blend
    clothes.Textures[2] = {{"player_legs", "legs", "legs_fat", "legs_ripped"}, {"jeansdenim", "legs"}};
    clothes.Textures[3] = {{"sneakerbincblk", "sneakerbincblk"}};
    clothes.FatStat = 200;
    clothes.MuscleStat = 50;
    clothes.BlendBody = true;
    return clothes;
}

bool NativePlayerAssets_Load(const NativePlayerClothes& clothes, NativePlayerAssets& out, std::string& error) {
    try {
        const auto ratios = BodyRatios(clothes);
        constexpr const char* defaults[]{"torso", "head", "hands", "legs", "feet"};
        constexpr const char* alternatives[]{"vest", "head", "hands", "jeans", "sneaker"};
        constexpr uint32 textureSlots[]{0, 1, 0, 2, 3};
        constexpr const char* textureNames[]{"torso", "head", "legs", "feet"};
        std::array<std::string, 5> models;
        for (size_t i = 0; i < models.size(); ++i) {
            models[i] = clothes.Models[i].empty() ? defaults[i] : clothes.Models[i];
            Check(models[i] == defaults[i] || models[i] == alternatives[i], "unsupported model in slot " + std::to_string(i));
        }
        for (const auto& layers : clothes.Textures) Check(!layers.empty(), "all four texture slots require explicit layers");
        InitRw();
        Archive gta, player;
        gta.Open("models/gta3.img");
        player.Open("models/player.img");
        NativePlayerAssets result;
        {
            Parsed base;
            base.Read(gta, "player", true);
            LoadBones(base, result);
        }
        for (size_t i = 0; i < result.Images.size(); ++i) {
            auto& image = result.Images[i];
            for (const auto& layer : clothes.Textures[i]) {
                auto next = LoadImage(player, layer);
                Check(layer.FatTexture.empty() == layer.RippedTexture.empty(), "both body texture variants required");
                if (!layer.FatTexture.empty()) {
                    Check(clothes.BlendBody && image.RGBA.empty(), "body texture blend must be first layer with BlendBody enabled");
                    const auto fat = LoadImage(player, {layer.Txd, layer.FatTexture});
                    const auto ripped = LoadImage(player, {layer.Txd, layer.RippedTexture});
                    BlendImage(next, fat, ripped, ratios);
                }
                if (image.RGBA.empty()) image = std::move(next);
                else OverlayImage(image, next);
            }
            image.Name = textureNames[i];
        }
        for (size_t i = 0; i < models.size(); ++i) {
            result.Materials[i].Image = textureSlots[i];
            LoadPart(player, models[i], i, clothes.BlendBody, ratios, result);
        }
        out = std::move(result); // commit only after every asset passed
        error.clear();
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}
