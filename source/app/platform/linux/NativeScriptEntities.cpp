#include "app/platform/linux/NativeScriptEntities.h"
#include "app/platform/linux/TexSample.h"
#include "app/platform/linux/GxtText.h"
#include "app/platform/linux/MenuShot.h"
#include "app/platform/linux/NativeCollisionAssets.h"
#include <cassert>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <span>
#include <stdexcept>
#include <type_traits>

using int32 = std::int32_t;
using uint32 = std::uint32_t;
using int64 = std::int64_t;
using uint64 = std::uint64_t;
#ifndef __stdcall
#define __stdcall
#endif
#include "oswrapper/oswrapper.h"
#include <rw.h>

namespace {
static_assert(std::is_nothrow_move_assignable_v<NativeScriptPickup>);
static_assert(std::is_nothrow_copy_assignable_v<NativeScriptRadarBlip>);
static_assert(std::is_nothrow_move_assignable_v<WorldShotScene>);
static_assert(std::is_nothrow_move_assignable_v<NativeScriptHelpPresentation>);
static_assert(std::is_nothrow_move_assignable_v<NativePlayerActivitySnapshot>);
void Require(bool ok, const std::string& error) { if (!ok) throw std::runtime_error(error); }
NativeScriptServiceResult Ready() { return {NativeScriptServiceStatus::Ready, {}}; }
NativeScriptServiceResult Error(const char* error) { return {NativeScriptServiceStatus::Error, error}; }
NativeScriptServiceResult Unsupported(const char* error) { return {NativeScriptServiceStatus::Unsupported, error}; }
uint32 Word(const std::uint8_t* p) { return uint32(p[0]) | uint32(p[1]) << 8 | uint32(p[2]) << 16 | uint32(p[3]) << 24; }
struct RwChunkView { uint32 Type = 0; std::span<const std::uint8_t> Data; };
std::vector<RwChunkView> RwChunks(std::span<const std::uint8_t> bytes) {
    std::vector<RwChunkView> chunks;
    for (std::size_t offset = 0; offset + 12 <= bytes.size();) {
        const auto size = std::size_t(Word(bytes.data() + offset + 4));
        Require(size <= bytes.size() - offset - 12, "DFF chunk bounds");
        chunks.push_back({Word(bytes.data() + offset), bytes.subspan(offset + 12, size)});
        offset += 12 + size;
    }
    return chunks;
}
std::shared_ptr<const NativeCollisionModel> EmbeddedCollision(
    const std::vector<std::uint8_t>& dff, const std::string& model) {
    const auto top = RwChunks(dff);
    Require(!top.empty() && top.front().Type == 0x10, "DFF clump chunk");
    std::shared_ptr<const NativeCollisionModel> result;
    for (const auto& extension : RwChunks(top.front().Data)) if (extension.Type == 3) {
        for (const auto& plugin : RwChunks(extension.Data)) if (plugin.Type == 0x253f2fa) {
            Require(!result, "multiple embedded vehicle COL chunks");
            auto collision = std::make_shared<NativeCollisionModel>();
            std::string error;
            Require(NativeCollisionAssets::Parse(plugin.Data, "gta3.img:" + model + ".dff:0x253f2fa",
                *collision, error), error);
            Require(collision->Name == model + "_col", "embedded COL/model identity mismatch");
            result = std::move(collision);
        }
    }
    return result;
}
bool Finite(NativeScriptPosition p) { return std::isfinite(p.X) && std::isfinite(p.Y) && std::isfinite(p.Z); }
template<typename T> bool ValidEnum(T value, T last) {
    return static_cast<std::underlying_type_t<T>>(value) <= static_cast<std::underlying_type_t<T>>(last);
}
bool ValidActivity(const NativePlayerActivitySnapshot& activity) {
    if (!ValidEnum(activity.Authority, NativePlayerActivityAuthority::SourceBacked) ||
        !ValidEnum(activity.PedState, NativePlayerPedState::Unsupported) ||
        activity.ActiveWeaponSlot >= activity.WeaponSlots.size()) return false;
    for (const auto& chain : activity.PrimaryTasks) for (const auto task : chain)
        if (!ValidEnum(task, NativePlayerTaskType::Unsupported)) return false;
    for (const auto& chain : activity.SecondaryTasks) for (const auto task : chain)
        if (!ValidEnum(task, NativePlayerTaskType::Unsupported)) return false;
    for (const auto event : activity.TypedEvents)
        if (!ValidEnum(event, NativePlayerEventType::Unsupported)) return false;
    for (const auto& weapon : activity.WeaponSlots) {
        if (!ValidEnum(weapon.Type, NativePlayerWeaponType::Unsupported) ||
            !ValidEnum(weapon.State, NativePlayerWeaponState::Unsupported)) return false;
    }
    return true;
}
bool SameActivity(const NativePlayerActivitySnapshot& a, const NativePlayerActivitySnapshot& b) {
    if (a.Authority != b.Authority || a.PrimaryTasks != b.PrimaryTasks || a.SecondaryTasks != b.SecondaryTasks ||
        a.TypedEvents != b.TypedEvents || a.PedState != b.PedState || a.InAir != b.InAir ||
        a.Landing != b.Landing || a.Alive != b.Alive || a.GamePlaying != b.GamePlaying ||
        a.CoopGame != b.CoopGame || a.ActiveWeaponSlot != b.ActiveWeaponSlot) return false;
    for (std::size_t i = 0; i < a.WeaponSlots.size(); ++i) {
        const auto& x = a.WeaponSlots[i]; const auto& y = b.WeaponSlots[i];
        if (x.Type != y.Type || x.State != y.State || x.AmmoInClip != y.AmmoInClip || x.TotalAmmo != y.TotalAmmo) return false;
    }
    return true;
}
struct File {
    void* Handle = nullptr;
    explicit File(const char* path) {
        Require(OS_FileOpen(FILE_DATA_AREA_DEFAULT, &Handle, path, FILE_ACCESS_READ) == 0 && Handle, path);
    }
    ~File() { OS_FileClose(Handle); }
    std::vector<std::uint8_t> Read(int32 offset, int32 size) {
        Require(offset >= 0 && size >= 0 && int64(offset) + size <= OS_FileSize(Handle), "asset read bounds");
        OS_FileSetPosition(Handle, offset);
        Require(OS_FileGetPosition(Handle) == offset, "asset seek");
        std::vector<std::uint8_t> bytes(size);
        Require(OS_FileRead(Handle, bytes.data(), size) == 0, "asset read");
        return bytes;
    }
};
std::vector<std::uint8_t> ReadFile(const char* path) {
    File file(path); return file.Read(0, OS_FileSize(file.Handle));
}
std::vector<std::uint8_t> ReadEntry(const std::string& name) {
    File file("models/gta3.img");
    auto header = file.Read(0, 8);
    Require(!std::memcmp(header.data(), "VER2", 4), "property IMG header");
    const auto count = Word(header.data() + 4);
    Require(count && count <= 300000, "property IMG directory count");
    const auto directory = file.Read(8, int32(count * 32));
    for (uint32 i = 0; i < count; ++i) {
        const auto* e = directory.data() + i * 32;
        if (name != std::string(reinterpret_cast<const char*>(e + 8), strnlen(reinterpret_cast<const char*>(e + 8), 24))) continue;
        const uint64 offset = uint64(Word(e)) * 2048, size = uint64(Word(e + 4) & 0x7fff) * 2048;
        Require(offset >= 8ull + count * 32ull && size && offset + size <= uint64(OS_FileSize(file.Handle)), "property IMG entry bounds");
        return file.Read(int32(offset), int32(size));
    }
    throw std::runtime_error("missing property asset: " + name);
}
// This slice borrows the running engine. No global init/term and no plugins
// added after startup; all dictionaries/clumps are locally owned and destroyed.
struct RwScope {
    rw::TexDictionary* Previous;
    RwScope(): Previous(nullptr) {
        Require(rw::Engine::state == rw::Engine::Started, "property parser needs exclusive started engine");
        Previous = rw::TexDictionary::getCurrent();
    }
    ~RwScope() { rw::TexDictionary::setCurrent(Previous); }
};
struct Dictionary {
    rw::TexDictionary* Value = nullptr;
    explicit Dictionary(std::vector<std::uint8_t>& bytes) {
        rw::StreamMemory stream; stream.open(bytes.data(), uint32(bytes.size()));
        if (rw::findChunk(&stream, rw::ID_TEXDICTIONARY, nullptr, nullptr)) Value = rw::TexDictionary::streamRead(&stream);
        stream.close(); Require(Value, "property TXD parse");
    }
    ~Dictionary() { Value->destroy(); }
};
WorldShotScene ReadStaticModel(const std::string& model, const std::string& txd, const NativeScriptStaticModelOptions& options = {});
WorldShotScene ReadModel(NativeScriptPropertyGeometry& property, int modelId, std::string_view expectedName = {}) {
    auto ide = ReadFile("data/maps/generic/dynamic.ide");
    ide.push_back(0);
    std::string model, txd;
    const char* line = reinterpret_cast<const char*>(ide.data());
    while (*line) {
        int id = -1; char dff[64]{}, dict[64]{};
        if (std::sscanf(line, "%d, %63[^,], %63[^,]", &id, dff, dict) == 3 && id == modelId) {
            model = dff; txd = dict; break;
        }
        const auto* next = std::strchr(line, '\n'); if (!next) break; line = next + 1;
    }
    const auto stem = [](const std::string& s) { return !s.empty() && s.size() <= 19 && std::all_of(s.begin(), s.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; }); };
    Require(stem(model) && stem(txd), "missing/invalid property IDE binding");
    Require(expectedName.empty() || model == expectedName, "source pickup model-name/IDE binding mismatch");
    // Verified generic dynamic-model library; resolve its chunk by BOTH source
    // name and IDE ID. No collision-context ownership or global COL loader.
    const auto col = ReadEntry("dynamic.col");
    bool found = false;
    for (std::size_t offset = 0; offset + 32 <= col.size();) {
        const auto* chunk = col.data() + offset;
        const bool signature = !std::memcmp(chunk, "COLL", 4) || !std::memcmp(chunk, "COL2", 4) ||
            !std::memcmp(chunk, "COL3", 4) || !std::memcmp(chunk, "COL4", 4);
        if (!signature && offset && col.size() - offset < 2048 && std::memcmp(chunk, "COL", 3)) break;
        Require(signature, "invalid property COL library framing");
        const std::size_t size = std::size_t(Word(chunk + 4)) + 8;
        Require(size >= 32 && size <= col.size() - offset, "property COL chunk bounds");
        const std::string name(reinterpret_cast<const char*>(chunk + 8), strnlen(reinterpret_cast<const char*>(chunk + 8), 22));
        if (name == model) {
            Require(!found, "ambiguous property COL binding");
            NativeCollisionModel parsed; std::string error;
            Require(NativeCollisionAssets::Parse(std::span(col).subspan(offset, size), "models/gta3.img:dynamic.col", parsed, error), error);
            Require(parsed.HeaderId == modelId, "property COL/IDE model ID mismatch");
            property.ColMin = parsed.Min; property.ColMax = parsed.Max;
            property.ColLibrary = parsed.Library; property.ColHeaderId = parsed.HeaderId;
            property.Scale = NativeScriptPropertyScale(parsed.Min, parsed.Max); found = true;
        }
        offset += size;
    }
    Require(found, "missing property COL bounds");
    return ReadStaticModel(model, txd);
}
WorldShotScene ReadStaticModel(const std::string& model, const std::string& txd, const NativeScriptStaticModelOptions& options) {
    auto textureBytes = ReadEntry(txd + ".txd");
    Dictionary dictionary(textureBytes);
    std::vector<std::uint8_t> sharedBytes;
    std::unique_ptr<Dictionary> shared;
    if (options.VehicleShared) {
        sharedBytes = ReadFile("models/generic/vehicle.txd");
        shared = std::make_unique<Dictionary>(sharedBytes);
    }
    auto modelBytes = ReadEntry(model + ".dff");
    const auto collision = options.CollisionOut ? EmbeddedCollision(modelBytes, model) : nullptr;
    struct Linked {
        LinkedClump Value;
        ~Linked() { TexSample_FreeLinked(Value); }
    } linked{TexSample_LinkedParse(modelBytes.data(), modelBytes.size(), dictionary.Value, nullptr, 0,
        shared ? shared->Value : nullptr)};
    Require(linked.Value.clump, "locked property DFF parse");
    WorldShotScene scene{};
    if (options.ResidencyOnly) {
        FORLIST(link, linked.Value.clump->atomics) {
            const auto* atomic = rw::Atomic::fromClump(link);
            Require(atomic && atomic->geometry, "resident model atomic geometry");
            ++scene.stats.atomics;
            scene.stats.triangles += atomic->geometry->numTriangles;
        }
        Require(scene.stats.atomics && scene.stats.triangles, "empty resident model clump");
        std::snprintf(scene.stats.dffName, sizeof(scene.stats.dffName), "%s.dff", model.c_str());
        std::snprintf(scene.stats.txdName, sizeof(scene.stats.txdName), "%s.txd", txd.c_str());
        if (options.CollisionOut) *options.CollisionOut = collision;
        return scene;
    }
    std::map<std::pair<const rw::Texture*, uint32>, int> images;
    FORLIST(link, linked.Value.clump->atomics) {
        auto* atomic = rw::Atomic::fromClump(link);
        auto* geometry = atomic->geometry;
        Require(geometry && geometry->numVertices > 0 && geometry->numTriangles > 0 && geometry->triangles &&
            geometry->morphTargets && geometry->morphTargets[0].vertices && !rw::Skin::get(geometry), "property static geometry");
        const auto count = geometry->numVertices;
        std::vector<rw::V3d> positions(count), normals(count);
        auto* frame = atomic->getFrame(); Require(frame, "property atomic frame");
        if (options.ResetFrame) std::copy_n(geometry->morphTargets[0].vertices, count, positions.begin());
        else rw::V3d::transformPoints(positions.data(), geometry->morphTargets[0].vertices, count, frame->getLTM());
        auto* sourceNormals = geometry->morphTargets[0].normals;
        if (sourceNormals) {
            if (options.ResetFrame) std::copy_n(sourceNormals, count, normals.begin());
            else rw::V3d::transformVectors(normals.data(), sourceNormals, count, frame->getLTM());
        }
        WorldShotMesh mesh{}; mesh.tris = geometry->numTriangles;
        std::fill(std::begin(mesh.color), std::end(mesh.color), 1.0f);
        for (int t = 0; t < mesh.tris; ++t) {
            const auto& tri = geometry->triangles[t];
            Require(tri.matId < geometry->matList.numMaterials && tri.v[0] < count && tri.v[1] < count && tri.v[2] < count, "property triangle bounds");
            const auto* mat = geometry->matList.materials[tri.matId]; Require(mat, "property material");
            int image = -1;
            if (mat->texture) {
                const auto it = linked.Value.resolved.find(mat->texture);
                const bool resolved = it != linked.Value.resolved.end() && it->second.real && geometry->texCoords[0];
                Require(resolved || !options.RequireTexture, "unresolved property texture/UV");
                if (resolved) {
                    const auto key = std::make_pair(it->second.real, it->second.filter);
                    auto found = images.find(key);
                    if (found == images.end()) {
                        WorldShotImage decoded{}; Require(TexSample_Decode(key.first, decoded) && !decoded.rgba.empty(), "property texel decode");
                        decoded.filter = key.second;
                        image = int(scene.images.size()); scene.images.push_back(std::move(decoded)); images.emplace(key, image);
                    } else image = found->second;
                }
            }
            mesh.triImg.push_back(image);
            mesh.triCol.insert(mesh.triCol.end(), {mat->color.red / 255.0f, mat->color.green / 255.0f, mat->color.blue / 255.0f});
            WorldShotSurface surface;
            surface.color = {mat->color.red / 255.0f, mat->color.green / 255.0f, mat->color.blue / 255.0f, mat->color.alpha / 255.0f};
            if (options.FirstMaterialColor && tri.matId == 0) {
                surface.color = *options.FirstMaterialColor;
                std::copy_n(surface.color.begin(), 3, mesh.triCol.end()-3);
            }
            surface.ambient = mat->surfaceProps.ambient; surface.diffuse = mat->surfaceProps.diffuse;
            mesh.surfaces.push_back(surface);
            const auto a = positions[tri.v[0]], b = positions[tri.v[1]], c = positions[tri.v[2]];
            const rw::V3d face{(b.y-a.y)*(c.z-a.z)-(b.z-a.z)*(c.y-a.y), (b.z-a.z)*(c.x-a.x)-(b.x-a.x)*(c.z-a.z), (b.x-a.x)*(c.y-a.y)-(b.y-a.y)*(c.x-a.x)};
            for (auto v : tri.v) {
                const auto p = positions[v]; Require(Finite({p.x, p.y, p.z}), "nonfinite property vertex");
                auto n = sourceNormals ? normals[v] : face;
                const auto length = std::sqrt(n.x*n.x+n.y*n.y+n.z*n.z);
                Require(std::isfinite(length) && length > 0, "invalid property normal");
                mesh.pos.insert(mesh.pos.end(), {p.x, p.y, p.z}); mesh.nrm.insert(mesh.nrm.end(), {n.x/length, n.y/length, n.z/length});
                const auto uv = geometry->texCoords[0] ? geometry->texCoords[0][v] : rw::TexCoords{};
                mesh.uv.insert(mesh.uv.end(), {uv.u, uv.v});
                const auto color = geometry->colors ? geometry->colors[v] : rw::RGBA{255, 255, 255, 255};
                mesh.dayColors.insert(mesh.dayColors.end(), {color.red, color.green, color.blue, color.alpha});
            }
        }
        scene.stats.triangles += mesh.tris; scene.stats.vertices += count; ++scene.stats.atomics;
        scene.meshes.push_back(std::move(mesh));
        if (options.FirstAtomicOnly) break;
    }
    Require(scene.stats.triangles && (!options.RequireTexture || !scene.images.empty()), "empty/untextured locked property model");
    std::snprintf(scene.stats.dffName, sizeof(scene.stats.dffName), "%s.dff", model.c_str());
    std::snprintf(scene.stats.txdName, sizeof(scene.stats.txdName), "%s.txd", txd.c_str());
    scene.stats.textures = int(scene.images.size());
    if (options.CollisionOut) *options.CollisionOut = collision;
    return scene;
}
int32 NextRef(int32 old, std::size_t slot) {
    const uint32 generation = old == -1 ? 1u : (uint32(old) >> 16) + 1;
    // Retire exhausted slots instead of allowing an old lifetime to alias.
    Require(generation < 0xffff, "entity generation exhausted");
    return std::bit_cast<int32>((generation << 16) | uint32(slot));
}
void Bounds(WorldShotScene& scene) {
    std::fill(std::begin(scene.bboxMin), std::end(scene.bboxMin), std::numeric_limits<float>::max());
    std::fill(std::begin(scene.bboxMax), std::end(scene.bboxMax), std::numeric_limits<float>::lowest());
    for (const auto& mesh : scene.meshes) for (std::size_t v = 0; v < mesh.pos.size(); ++v) {
        scene.bboxMin[v % 3] = std::min(scene.bboxMin[v % 3], mesh.pos[v]);
        scene.bboxMax[v % 3] = std::max(scene.bboxMax[v % 3], mesh.pos[v]);
    }
    if (scene.meshes.empty()) { std::fill(std::begin(scene.bboxMin), std::end(scene.bboxMin), 0); std::fill(std::begin(scene.bboxMax), std::end(scene.bboxMax), 0); }
}
// Default native keyboard binding: mode0 CollectPickupJustDown uses L1,
// mapped to PED_ANSWER_PHONE (TAB). Unknown formatting is a real barrier.
std::string PropertyText(std::string text, std::int32_t price) {
    const auto replace = [&](std::string_view token, const std::string& value) {
        for (auto pos = text.find(token); pos != std::string::npos; pos = text.find(token, pos + value.size())) text.replace(pos, token.size(), value);
    };
    replace("~k~~PED_ANSWER_PHONE~", "TAB");
    replace("~1~", std::to_string(price));
    Require(!text.empty() && text.size() < 400 && std::all_of(text.begin(), text.end(), [](unsigned char c) {
        return c == '\n' || (c >= 32 && c < 128 && c != '~'); }), "unsupported property GXT/control format");
    return text;
}
}

bool NativeScriptEntities_LoadStaticModel(const char* gameDir, const std::string& model,
    const std::string& txd, WorldShotScene& scene, std::string& error, const NativeScriptStaticModelOptions& options) {
    try {
        RwScope scope; OS_SetFilePathOffset(gameDir);
        auto prepared = ReadStaticModel(model, txd, options); Bounds(prepared);
        scene = std::move(prepared); error.clear(); return true;
    } catch (const std::exception& e) { error = e.what(); return false; }
}
bool NativeScriptEntities_LoadTextureDictionary(const char* gameDir, const std::string& name,
    NativeScriptTextureDictionaryPacket& packet, std::string& error) {
    try {
        Require(!name.empty() && name.size() <= 15 && std::all_of(name.begin(), name.end(), [](unsigned char c) {
            return std::isalnum(c) || c == '_';
        }), "invalid script texture dictionary name");
        RwScope scope;
        OS_SetFilePathOffset(gameDir);
        auto bytes = ReadFile(("models/txd/" + name + ".txd").c_str());
        Dictionary dictionary(bytes);
        NativeScriptTextureDictionaryPacket candidate;
        candidate.Name = name;
        FORLIST(link, dictionary.Value->textures) {
            const auto* texture = rw::Texture::fromDict(link);
            WorldShotImage image{};
            Require(texture && TexSample_Decode(texture, image) && !image.rgba.empty(),
                "script texture decode");
            Require(std::none_of(candidate.Images.begin(), candidate.Images.end(), [&](const auto& old) {
                return !std::strcmp(old.name, image.name);
            }), "duplicate script texture name");
            candidate.Images.push_back(std::move(image));
        }
        Require(!candidate.Images.empty(), "empty script texture dictionary");
        packet = std::move(candidate);
        error.clear();
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}
bool NativeScriptEntities_LoadRadar(const char* gameDir, WorldShotImage& image, std::string& error, int sprite) {
    try {
        RwScope scope; OS_SetFilePathOffset(gameDir);
        auto bytes = ReadFile("models/hud.txd"); Dictionary dictionary(bytes);
        WorldShotImage decoded{};
        Require(sprite == 31 || sprite == 32, "unprepared property radar ID");
        Require(TexSample_Decode(dictionary.Value->find(sprite == 31 ? "radar_propertyG" : "radar_propertyR"), decoded) && !decoded.rgba.empty(), "missing property radar sprite");
        image = std::move(decoded); error.clear(); return true;
    } catch (const std::exception& e) { error = e.what(); return false; }
}
bool NativeScriptEntities_LoadRadar(const char* gameDir, WorldShotImage& image, std::string& error) {
    return NativeScriptEntities_LoadRadar(gameDir, image, error, 32);
}
bool NativeScriptRadarVisible(const NativeScriptRadarBlip& b, float distance, bool onMission, unsigned zoom, bool exterior) {
    // Live radar: HasThisBlipBeenRevealed always succeeds outside frontend map.
    // DisplayThisBlip interior whitelist, with source default category toggles.
    const bool scope = exterior || (b.Sprite >= 1 && b.Sprite <= 4) || b.Sprite == 25 ||
        b.Sprite == 36 || b.Sprite == 41 || b.Sprite == 44 || b.Sprite == 52;
    return b.Active && b.Sprite > 0 && b.Sprite < 64 && scope && !(b.Kind == NativeScriptBlipKind::Contact && onMission) &&
        (b.Display == 2 || b.Display == 3) && (!b.ShortRange || (!zoom && distance <= 1.0f));
}
NativeScriptEntities::NativeScriptEntities(std::size_t pickups, std::size_t blips): m_Pickups(pickups), m_Blips(blips) {
    Require(pickups <= 620 && blips <= 175, "native entity capacity exceeds source pools");
    for (auto& ref : m_CollectedPickups) ref.Value = 0; // CPickups::Init
}
bool NativeScriptEntities::LoadBeforeWorker(const char* gameDir, std::string& error) {
    if (m_Loaded) { error = "property assets already initialized"; return false; }
    try {
        RwScope scope; OS_SetFilePathOffset(gameDir);
        NativeScriptPropertyGeometry property, saleGeometry, saveGeometry, photoGeometry;
        auto model = ReadModel(property, 1272); Bounds(model);
        auto saleModel = ReadModel(saleGeometry, 1273); Bounds(saleModel);
        // ModelIndices.cpp MI_PICKUP_SAVEGAME binding, dynamic.ide + dynamic.col.
        // Bounded source model category, not SCM coordinates/IP/string scanning.
        auto saveModel = ReadModel(saveGeometry, 1277, "pickupsave"); Bounds(saveModel);
        auto photoModel = ReadModel(photoGeometry, 1253, "camerapickup"); Bounds(photoModel);
        NativeScriptStaticModelOptions collectible;
        collectible.RequireTexture=false;
        auto oysterModel = ReadStaticModel("cj_oyster", "shell_pick", collectible); Bounds(oysterModel);
        auto horseshoeModel = ReadStaticModel("cj_horse_shoe", "horse_shoe_pick", collectible); Bounds(horseshoeModel);
        auto images = model.images;
        images.insert(images.end(), saleModel.images.begin(), saleModel.images.end());
        images.insert(images.end(), saveModel.images.begin(), saveModel.images.end());
        images.insert(images.end(), photoModel.images.begin(), photoModel.images.end());
        images.insert(images.end(), oysterModel.images.begin(), oysterModel.images.end());
        images.insert(images.end(), horseshoeModel.images.begin(), horseshoeModel.images.end());
        WorldShotImage radar{}; Require(NativeScriptEntities_LoadRadar(gameDir, radar, error), error);
        WorldShotImage saleRadar{}; Require(NativeScriptEntities_LoadRadar(gameDir, saleRadar, error, 31), error);
        GxtTable table; char err[256]{}; Require(GxtText_Load(gameDir, "english", table, err, sizeof(err)), err);
        std::array<std::string, 3> messages;
        std::array<std::uint32_t, 3> lineCounts{};
        MenuHudFont helpFont;
        Require(MenuShot_LoadPricedownFont(gameDir, helpFont, err, sizeof(err)), err); // font1 + font-ID 1 metrics
        constexpr const char* keys[]{"PROP_3", "PROP_4", "FESZ_CA"};
        std::array<std::string, 3> saleMessages, labelMessages;
        for (std::size_t i = 0; i < messages.size(); ++i) {
            Require(GxtText_Find(table, keys[i], messages[i]), "property GXT key missing");
            const auto plain = !messages[i].empty() && messages[i].size() < 400 && std::all_of(messages[i].begin(), messages[i].end(),
                [](unsigned char c) { return c == '\n' || (c >= 32 && c < 128 && c != '~'); });
            if (plain) lineCounts[i] = std::uint32_t(NativeScriptHelpLines(messages[i], helpFont.prop).size());
            // ModifyStringLabelForControlSetting: mode0/1 changes final suffix
            // after '_' to L (including PROP_4); FESZ_CA has no such suffix.
            std::string key(keys[i]);
            if (key.size() >= 2 && key[key.size()-2] == '_') key.back() = 'L';
            Require(GxtText_Find(table, key.c_str(), saleMessages[i]), "sale control GXT key missing");
            (void)PropertyText(saleMessages[i], 0);
            labelMessages[i] = PropertyText(messages[i], 0);
        }
        std::array<std::string, 2> denials;
        Require(GxtText_Find(table, "PROP_1", denials[0]) && GxtText_Find(table, "PROP_2", denials[1]), "sale denial GXT missing");
        for (auto& denial : denials) denial = PropertyText(std::move(denial), 0);
        m_Model = std::move(model); m_Radar = std::move(radar); m_Messages = std::move(messages);
        m_ForSaleModel = std::move(saleModel); m_ForSaleGeometry = std::move(saleGeometry);
        m_SaveModel = std::move(saveModel); m_SaveGeometry = std::move(saveGeometry);
        m_PhotoModel = std::move(photoModel);
        m_OysterModel = std::move(oysterModel); m_HorseshoeModel = std::move(horseshoeModel);
        m_ForSaleRadar = std::move(saleRadar); m_Images = std::move(images);
        m_SaleMessages = std::move(saleMessages); m_LabelMessages = std::move(labelMessages);
        m_Denials = std::move(denials); std::copy_n(helpFont.prop, m_HelpWidths.size(), m_HelpWidths.begin());
        m_MessageLines = lineCounts;
        m_PropertyGeometry = std::move(property);
        m_Loaded = true; error.clear(); return true;
    } catch (const std::exception& e) { error = e.what(); return false; }
}
const NativeScriptEntities::Event* NativeScriptEntities::FindEvent(NativeScriptRequestId id) const {
    for (const auto& event : m_Events) if (event.Id == id) return &event;
    return nullptr;
}
const NativeScriptEntities::PickupOperation* NativeScriptEntities::FindPickupOperation(NativeScriptRequestId id) const {
    for (const auto& operation : m_PickupOperations) if (operation.Id == id) return &operation;
    return nullptr;
}
bool NativeScriptEntities::OwnsRequest(NativeScriptRequestId id) const {
    return FindEvent(id) || FindPickupOperation(id);
}
const NativeScriptPickup* NativeScriptEntities::ResolvePickup(NativeScriptPickupRef ref) const {
    const auto slot = uint32(ref.Value) & 0xffff;
    return slot < m_Pickups.size() && m_Pickups[slot].Active && m_Pickups[slot].Reference.Value == ref.Value ? &m_Pickups[slot] : nullptr;
}
const NativeScriptRadarBlip* NativeScriptEntities::ResolveBlip(NativeScriptBlipRef ref) const {
    const auto slot = uint32(ref.Value) & 0xffff;
    return slot < m_Blips.size() && m_Blips[slot].Active && m_Blips[slot].Reference.Value == ref.Value ? &m_Blips[slot] : nullptr;
}
NativeScriptReferenceResult<NativeScriptPickupRef> NativeScriptEntities::CreateLockedProperty(const NativeScriptLockedPropertyRequest& r) {
    return CreateProperty({r.Id, r.Position, 0, r.Text}, false);
}
NativeScriptReferenceResult<NativeScriptPickupRef> NativeScriptEntities::CreateForSaleProperty(const NativeScriptForSalePropertyRequest& r) {
    return CreateProperty(r, true);
}
NativeScriptReferenceResult<NativeScriptPickupRef> NativeScriptEntities::CreatePickup(const NativeScriptPickupRequest& r,
    NativeScriptPosition camera, std::uint32_t gameMs) {
    if (FindPickupOperation(r.Id)) return {Error("pickup request ID already owns a pickup query/removal"), {}};
    if (const auto* old = FindEvent(r.Id)) {
        if (old->Opcode != 0x0213 || old->Position != r.Position || old->Argument != r.Type || old->Model != r.Model ||
            old->ModelName != r.UsedObjectName || (old->Reference != -1 && !ResolvePickup({old->Reference}))) return {Error("mismatched/stale ordinary pickup replay"), {}};
        return {Ready(), {old->Reference}};
    }
    if (!m_Loaded) return {Unsupported("ordinary model must be prepared before worker startup"), {}};
    auto name = r.UsedObjectName;
    for (auto& c : name) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    const std::string_view model(name.data(), strnlen(name.data(), name.size()));
    if (r.Model >= 0 && std::ranges::any_of(name, [](char c) { return c != 0; })) return {Error("positive pickup model has a used-object name"), {}};
    if ((r.Model < 0 && model != "pickupsave") || r.Type <= 0 || r.Type > 22)
        return {Unsupported("ordinary pickup model/type consumer is not prepared"), {}};
    if (!Finite(r.Position) || !Finite(camera)) return {Error("nonfinite ordinary pickup position/camera"), {}};
    if (r.Position.Z <= -100) return {Unsupported("ordinary pickup ground sentinel requires owned world ground service"), {}};
    for (const auto v : {r.Position.X, r.Position.Y, r.Position.Z}) if (v * 8 < -32768 || v * 8 > 32767)
        return {Error("ordinary pickup position compression overflow"), {}};
    const auto free = std::find_if(m_Pickups.begin(), m_Pickups.end(), [](const auto& p) {
        return !p.Active && (p.Reference.Value == -1 || (uint32(p.Reference.Value) >> 16) < 0xfffe);
    });
    if (free == m_Pickups.end()) {
        // Original GenerateNewOne returns -1 when all620 slots are occupied
        // and none is money/type4/type5. These are the only populated types in
        // this slice. Journal that result too: retry cannot allocate later.
        if (m_Pickups.size() == 620 && std::ranges::all_of(m_Pickups, [](const auto& p) {
            return p.Active && (p.Type == 3 || p.Type == 17 || p.Type == 18);
        })) {
            m_Events.push_back({r.Id, 0x0213, r.Position, {}, r.Type, -1, r.Model, r.UsedObjectName});
            ++m_Revision; return {Ready(), {-1}};
        }
        return {Error("native pickup capacity/generation bound prevents source allocation"), {}};
    }
    NativeScriptPickup p;
    p.Reference = {NextRef(free->Reference.Value, std::size_t(free - m_Pickups.begin()))};
    p.AuthoredPosition = r.Position;
    p.Position = {float(int32(r.Position.X * 8)) / 8, float(int32(r.Position.Y * 8)) / 8, float(int32(r.Position.Z * 8)) / 8};
    p.Model = r.Model < 0 ? 1277 : r.Model; p.Type = r.Type; p.Active = true; p.RegenerationTime = gameMs;
    const auto dx = camera.X - p.Position.X, dy = camera.Y - p.Position.Y;
    const bool prepared=p.Model==953||p.Model==954||p.Model==1253||p.Model==1277;
    p.Visible = prepared && dx*dx + dy*dy < 10000;
    p.ObjectPresent = prepared && p.Visible && !(m_Frame && m_Frame->Property && m_Frame->Property->CutsceneLoaded);
    if(prepared)p.Actor = p.Model == 953 ? m_OysterModel : p.Model == 954 ? m_HorseshoeModel :
        p.Model == 1253 ? m_PhotoModel : m_SaveModel;
    if(prepared){
        for (auto& mesh : p.Actor.meshes) for (std::size_t i = 0; i < mesh.pos.size(); i += 3) {
            const auto x = mesh.pos[i], nx = mesh.nrm[i];
            mesh.pos[i] = mesh.pos[i+1] + p.Position.X; mesh.pos[i+1] = -x + p.Position.Y; mesh.pos[i+2] += p.Position.Z;
            mesh.nrm[i] = mesh.nrm[i+1]; mesh.nrm[i+1] = -nx;
        }
        Bounds(p.Actor);
    }
    m_Events.push_back({r.Id, 0x0213, r.Position, {}, r.Type, p.Reference.Value, r.Model, r.UsedObjectName});
    *free = std::move(p); ++m_Revision; return {Ready(), free->Reference};
}
NativeScriptReferenceResult<NativeScriptPickupRef> NativeScriptEntities::CreatePickupWithAmmo(
    const NativeScriptPickupAmmoRequest& r, std::uint32_t gameMs) {
    if (FindPickupOperation(r.Id)) return {Error("pickup request ID already owns a pickup query/removal"), {}};
    if (const auto* old=FindEvent(r.Id)) {
        if(old->Opcode!=0x032B||old->Position!=r.Position||old->Argument!=r.Type||old->Model!=r.Model||old->Ammo!=r.Ammo||
            (old->Reference!=-1&&!ResolvePickup({old->Reference}))) return {Error("mismatched/stale ammo pickup replay"),{}};
        return {Ready(),{old->Reference}};
    }
    if(!m_Loaded)return {Unsupported("pickup identities must be prepared before worker startup"),{}};
    if(r.Type<=0||r.Type>22||r.Ammo<0||!Finite(r.Position))return {Error("invalid source ammo pickup request"),{}};
    for(const auto v:{r.Position.X,r.Position.Y,r.Position.Z})if(v*8<-32768||v*8>32767)return {Error("ammo pickup position compression overflow"),{}};
    const auto free=std::find_if(m_Pickups.begin(),m_Pickups.end(),[](const auto& p){return !p.Active&&(p.Reference.Value==-1||(uint32(p.Reference.Value)>>16)<0xfffe);});
    if(free==m_Pickups.end())return {Error("pickup pool capacity exhausted"),{}};
    NativeScriptPickup p; p.Reference={NextRef(free->Reference.Value,std::size_t(free-m_Pickups.begin()))}; p.AuthoredPosition=r.Position;
    p.Position={float(int32(r.Position.X*8))/8,float(int32(r.Position.Y*8))/8,float(int32(r.Position.Z*8))/8};
    p.Model=r.Model; p.Type=r.Type; p.Ammo=r.Ammo; p.Active=true; p.RegenerationTime=gameMs;
    m_Events.push_back({r.Id,0x032B,r.Position,{},r.Type,p.Reference.Value,r.Model,{},r.Ammo});
    *free=std::move(p); ++m_Revision; return {Ready(),free->Reference};
}
NativeScriptReferenceResult<NativeScriptPickupRef> NativeScriptEntities::CreateProperty(const NativeScriptForSalePropertyRequest& r, bool forSale) {
    const std::uint16_t opcode = forSale ? 0x0518 : 0x0517;
    if (FindPickupOperation(r.Id)) return {Error("property request ID already owns a pickup query/removal"), {}};
    if (const auto* old = FindEvent(r.Id)) {
        if (old->Opcode != opcode || old->Position != r.Position || old->Text != r.Text || old->Argument != r.Price || !ResolvePickup({old->Reference})) return {Error("mismatched/stale property replay"), {}};
        return {Ready(), {old->Reference}};
    }
    if (!m_Loaded) return {Unsupported("property assets must be prepared before worker startup"), {}};
    if (!Finite(r.Position)) return {Error("nonfinite property position"), {}};
    // Original 0518 Z<=-100 requests CWorld ground +0.5. This entity service
    // has no collision authority; fail explicitly rather than invent a height.
    if (forSale && r.Position.Z <= -100) return {Unsupported("sale ground sentinel requires owned world ground service"), {}};
    for (const auto v : {r.Position.X, r.Position.Y, r.Position.Z}) if (v * 8 < -32768 || v * 8 > 32767) return {Error("property position compression overflow"), {}};
    const auto free = std::find_if(m_Pickups.begin(), m_Pickups.end(), [](const auto& p) { return !p.Active && (p.Reference.Value == -1 || (uint32(p.Reference.Value) >> 16) < 0xfffe); });
    if (free == m_Pickups.end()) return {Error("pickup pool capacity exhausted"), {}};
    NativeScriptPickup pickup;
    pickup.Reference = {NextRef(free->Reference.Value, std::size_t(free - m_Pickups.begin()))};
    pickup.AuthoredPosition = r.Position;
    pickup.Position = {float(int32(r.Position.X * 8)) / 8, float(int32(r.Position.Y * 8)) / 8, float(int32(r.Position.Z * 8)) / 8};
    pickup.Text = r.Text;
    auto text = r.Text; for (auto& c : text) if (c >= 'a' && c <= 'z') c -= 'a' - 'A';
    const std::string key(text.data(), strnlen(text.data(), text.size()));
    const auto messageIndex = key == "PROP_3" ? 0 : key == "PROP_4" ? 1 : 2;
    if (!forSale && !m_MessageLines[messageIndex]) return {Unsupported("property help requires unimplemented GXT/control-key expansion"), {}};
    pickup.Message = forSale ? PropertyText(m_SaleMessages[messageIndex], r.Price) : m_Messages[messageIndex];
    pickup.MessageLines = std::uint32_t(NativeScriptHelpLines(pickup.Message, m_HelpWidths).size());
    pickup.Label = m_LabelMessages[messageIndex];
    pickup.Price = r.Price; pickup.CostValue = std::uint16_t(uint32(r.Price) / 5u);
    pickup.Model = forSale ? 1273 : 1272; pickup.Type = forSale ? 18 : 17;
    pickup.Actor = forSale ? m_ForSaleModel : m_Model; pickup.Active = true;
    // CPickup::GiveUsAPickUpObject: position=compressed position, heading=-pi/2.
    for (auto& mesh : pickup.Actor.meshes) for (std::size_t i = 0; i < mesh.pos.size(); i += 3) {
        const auto x = mesh.pos[i], nx = mesh.nrm[i];
        mesh.pos[i] = mesh.pos[i+1] + pickup.Position.X; mesh.pos[i+1] = -x + pickup.Position.Y; mesh.pos[i+2] += pickup.Position.Z;
        mesh.nrm[i] = mesh.nrm[i+1]; mesh.nrm[i+1] = -nx;
    }
    Bounds(pickup.Actor);
    // All potentially throwing preparation precedes the commit; move assignment
    // publishes a generation and its actual geometry together, never a zero ref.
    m_Events.push_back({r.Id, opcode, r.Position, r.Text, r.Price, pickup.Reference.Value});
    *free = std::move(pickup); ++m_Revision; return {Ready(), free->Reference};
}
NativeScriptReferenceResult<NativeScriptBlipRef> NativeScriptEntities::CreateContactBlip(const NativeScriptContactBlipRequest& r) {
    if (FindPickupOperation(r.Id)) return {Error("blip request ID already owns a pickup query/removal"), {}};
    if (const auto* old = FindEvent(r.Id)) {
        if (old->Opcode != (r.AddSphere?0x02A7:0x0570) || old->Position != r.Position || old->Argument != r.Sprite || !ResolveBlip({old->Reference})) return {Error("mismatched/stale blip replay"), {}};
        return {Ready(), {old->Reference}};
    }
    if (!m_Loaded || (r.Sprite != 32 && r.Sprite != 31 && !r.RadarSpriteReady)) return {Unsupported("contact sprite is not prepared by this bounded host"), {}};
    if (!Finite(r.Position)) return {Error("nonfinite blip position"), {}};
    const auto free = std::find_if(m_Blips.begin(), m_Blips.end(), [](const auto& b) { return !b.Active && (b.Reference.Value == -1 || (uint32(b.Reference.Value) >> 16) < 0xfffe); });
    if (free == m_Blips.end()) return {Error("radar pool capacity exhausted"), {}};
    NativeScriptRadarBlip blip;
    blip.Reference = {NextRef(free->Reference.Value, std::size_t(free - m_Blips.begin()))}; blip.Position = r.Position; blip.Active = true;
    blip.Sprite = r.Sprite; blip.DrawSphere=r.AddSphere;
    m_Events.push_back({r.Id, std::uint16_t(r.AddSphere?0x02A7:0x0570), r.Position, {}, r.Sprite, blip.Reference.Value});
    *free = blip; ++m_Revision; return {Ready(), blip.Reference};
}
NativeScriptReferenceResult<NativeScriptBlipRef> NativeScriptEntities::CreateCoordinateBlip(
    const NativeScriptCoordinateBlipRequest& r, const std::function<bool(std::int32_t)>& radarSpriteReady) {
    if (FindPickupOperation(r.Id)) return {Error("coordinate blip request ID already owns a pickup operation"), {}};
    if (const auto* old = FindEvent(r.Id)) {
        if (old->Opcode != 0x04CE || old->Position != r.Position || old->Argument != r.Sprite || !ResolveBlip({old->Reference}))
            return {Error("mismatched/stale coordinate blip replay"), {}};
        return {Ready(), {old->Reference}};
    }
    if (!Finite(r.Position)) return {Error("nonfinite coordinate blip position"), {}};
    // Original04CE queries FindGroundZForCoord for the <=-100 sentinel. This
    // service must not turn a missing source world query into an authored Z.
    if (r.Position.Z <= -100.0f) return {Unsupported("coordinate blip requires source FindGroundZForCoord"), {}};
    if (r.Sprite <= 0 || r.Sprite >= 64) return {Unsupported("coordinate blip requires supported source sprite presentation"), {}};
    if (!m_Loaded || !radarSpriteReady) return {Unsupported("coordinate sprite has no registered radar consumer"), {}};
    const auto free = std::find_if(m_Blips.begin(), m_Blips.end(), [](const auto& b) {
        return !b.Active && (b.Reference.Value == -1 || (uint32(b.Reference.Value) >> 16) < 0xfffe);
    });
    if (free == m_Blips.end()) return {Error("radar pool capacity exhausted"), {}};
    try {
        if (!radarSpriteReady(r.Sprite)) return {Unsupported("coordinate sprite is not ready in the registered radar consumer"), {}};
    } catch (const std::exception& e) { return {Error(("radar sprite readiness exception: " + std::string(e.what())).c_str()), {}}; }
    catch (...) { return {Error("unknown radar sprite readiness exception"), {}}; }
    NativeScriptRadarBlip blip;
    blip.Reference = {NextRef(free->Reference.Value, std::size_t(free - m_Blips.begin()))};
    blip.Position = r.Position; blip.Sprite = r.Sprite; blip.Active = true;
    blip.Kind = NativeScriptBlipKind::Coordinate; blip.Contact = false;
    m_Events.push_back({r.Id, 0x04CE, r.Position, {}, r.Sprite, blip.Reference.Value});
    *free = blip; ++m_Revision; return {Ready(), blip.Reference};
}
NativeScriptServiceResult NativeScriptEntities::SetBlipDisplay(const NativeScriptBlipDisplayRequest& r) {
    if (FindPickupOperation(r.Id)) return Error("display request ID already owns a pickup query/removal");
    if (const auto* old = FindEvent(r.Id)) {
        if (old->Opcode != 0x018B || old->Reference != r.Blip.Value || old->Argument != r.Display || !ResolveBlip(r.Blip)) return Error("mismatched/stale display replay");
        return Ready();
    }
    if (!ResolveBlip(r.Blip)) return Error("stale or unallocated blip");
    if (r.Display < 0 || r.Display > 3) return Error("invalid radar display");
    if ((r.Display == 1 || r.Display == 3) && ResolveBlip(r.Blip)->Kind == NativeScriptBlipKind::Contact)
        return Unsupported("3D contact markers are outside this bounded radar-only service");
    m_Events.push_back({r.Id, 0x018B, {}, {}, r.Display, r.Blip.Value});
    m_Blips[uint32(r.Blip.Value) & 0xffff].Display = r.Display; ++m_Revision; return Ready();
}
bool NativeScriptEntities::RemovePickup(NativeScriptPickupRef ref) {
    if (!ResolvePickup(ref)) return false;
    auto& p = m_Pickups[uint32(ref.Value) & 0xffff];
    p.Active = false; p.Type = 0; p.Visible = false; p.ObjectPresent = false; p.Actor = {};
    m_Actors = {}; m_Labels.clear();
    if (m_Interaction.Pickup.Value == ref.Value) m_Interaction = {};
    if (m_PickupRequirement.Pickup.Value == ref.Value) m_PickupRequirement = {};
    ++m_Revision; return true;
}
bool NativeScriptEntities::RemoveBlip(NativeScriptBlipRef ref) {
    if (!ResolveBlip(ref)) return false;
    m_Blips[uint32(ref.Value) & 0xffff].Active = false; ++m_Revision; return true;
}
void NativeScriptEntities::Tick(NativeScriptPosition ped, NativeScriptPosition camera, bool alive, bool inVehicle) {
    Require(Finite(ped) && Finite(camera), "nonfinite property frame input");
    m_Frame = FrameInput{ped, camera, alive, inVehicle, {}};
}
void NativeScriptEntities::Tick(NativeScriptPosition ped, NativeScriptPosition camera, bool alive, bool inVehicle, const NativeScriptPropertyInput& input) {
    Require(Finite(ped) && Finite(camera), "nonfinite property frame input");
    m_Frame = FrameInput{ped, camera, alive, inVehicle, input};
}
bool NativeScriptEntities::UpdatePlayerActivity(std::uint32_t frameCounter, std::uint64_t ownerRevision,
    const NativePlayerActivitySnapshot& snapshot, std::string& error) try {
    if (!ValidActivity(snapshot)) { error = "invalid player activity snapshot"; return false; }
    if (m_PlayerActivity) {
        const auto elapsed = frameCounter - m_PlayerActivity->FrameCounter;
        if (elapsed > 0x7fffffff) { error = "player activity frame counter moved backwards"; return false; }
        if (ownerRevision < m_PlayerActivity->OwnerRevision) { error = "player activity owner revision moved backwards"; return false; }
        const auto same = SameActivity(snapshot, m_PlayerActivity->Snapshot);
        if (!elapsed && (ownerRevision != m_PlayerActivity->OwnerRevision || !same)) {
            error = "player activity changed within a frame"; return false;
        }
        if (ownerRevision == m_PlayerActivity->OwnerRevision && !same) {
            error = "player activity changed without an owner revision"; return false;
        }
        if (!elapsed) { error.clear(); return true; }
    }
    NativePlayerActivitySnapshot owned = snapshot;
    PlayerActivityFrame next{frameCounter, ownerRevision, std::move(owned)};
    m_PlayerActivity = std::move(next); m_UsesPlayerActivity = true;
    error.clear(); return true;
} catch (const std::exception& e) { error = e.what(); return false; }
NativeScriptPickupCollectedResult NativeScriptEntities::HasPickupBeenCollected(const NativeScriptPickupReferenceRequest& r) {
    if (FindEvent(r.Id)) return {Error("pickup query request ID already owns an entity creation/update"), false};
    if (const auto* old = FindPickupOperation(r.Id)) {
        if (old->Kind != PickupOperationKind::HasBeenCollected || old->Pickup.Value != r.Pickup.Value)
            return {Error("mismatched pickup collected-query replay"), false};
        return {Ready(), old->Collected};
    }
    const auto found = std::find_if(m_CollectedPickups.begin(), m_CollectedPickups.end(), [&](const auto ref) {
        return ref.Value == r.Pickup.Value;
    });
    const bool collected = found != m_CollectedPickups.end();
    try {
        m_PickupOperations.push_back({r.Id, r.Pickup, PickupOperationKind::HasBeenCollected, collected});
    } catch (const std::exception& e) { return {Error(e.what()), false}; }
    if (collected) { found->Value = 0; ++m_Revision; }
    return {Ready(), collected};
}
NativeScriptServiceResult NativeScriptEntities::RemoveScriptPickup(const NativeScriptPickupReferenceRequest& r) {
    if (FindEvent(r.Id)) return Error("pickup removal request ID already owns an entity creation/update");
    if (const auto* old = FindPickupOperation(r.Id)) {
        if (old->Kind != PickupOperationKind::Remove || old->Pickup.Value != r.Pickup.Value)
            return Error("mismatched pickup removal replay");
        return Ready();
    }
    try {
        m_PickupOperations.push_back({r.Id, r.Pickup, PickupOperationKind::Remove, false});
    } catch (const std::exception& e) { return Error(e.what()); }
    const auto slot = uint32(r.Pickup.Value) & 0xffff;
    if (r.Pickup.Value != -1 && slot < m_Pickups.size() && m_Pickups[slot].Reference.Value == r.Pickup.Value)
        RemovePickup(r.Pickup); // Current-generation type NONE is a source-valid no-op.
    return Ready();
}
std::optional<NativeScriptPadShakeEvent> NativeScriptEntities::ConsumePadShake() {
    if (m_PadShakeCursor == m_PadShakes.size()) return {};
    const auto event = m_PadShakes[m_PadShakeCursor++];
    if (m_PadShakeCursor == m_PadShakes.size()) { m_PadShakes.clear(); m_PadShakeCursor = 0; }
    return event;
}
NativeScriptPropertyInteractionStatus NativeScriptPropertyCollect(std::int32_t price,
    const NativeScriptPropertyInput& input, std::uint8_t buffer) {
    if (!buffer) return NativeScriptPropertyInteractionStatus::None;
    if (input.OnMission) return NativeScriptPropertyInteractionStatus::OnMission;
    if (input.Money < price) return NativeScriptPropertyInteractionStatus::InsufficientFunds;
    return NativeScriptPropertyInteractionStatus::ScriptPurchaseRequired;
}
bool NativeScriptEntities::AdvanceTime(std::uint32_t now, std::string& error) try {
    if (m_HasTime && now - m_GameMs > 0x7fffffff) { error = "property game time moved backwards or exceeded half-range"; return false; }
    if (!m_UsesPlayerActivity && m_PickupRequirement.Kind != NativeScriptPickupRequirementKind::None) {
        error = "ordinary pickup requires live CanPlayerStartMission task/event eligibility before collection"; return false;
    }
    if (m_HasTime && now == m_GameMs && m_Frame == m_PublishedFrame && m_PresentationRevision == m_Revision) {
        error.clear(); return true;
    }
    auto help = m_Help;
    auto helpMessage = m_HelpMessage;
    auto buffer = m_CollectBuffer;
    auto collectFrame = m_CollectFrame;
    auto interaction = m_Interaction;
    auto collectedPickups = m_CollectedPickups;
    auto collectedPickupCursor = m_CollectedPickupCursor;
    auto padShakes = m_PadShakes;
    bool newCollectFrame = false;
    if (m_Frame && m_Frame->Property) {
        const auto& input = *m_Frame->Property;
        if (collectFrame && input.FrameCounter - *collectFrame > 0x7fffffff) { error = "property frame counter moved backwards"; return false; }
        if (collectFrame && input.FrameCounter == *collectFrame && m_PublishedFrame && m_PublishedFrame->Property != m_Frame->Property) {
            error = "property collect inputs changed within a published frame"; return false;
        }
        newCollectFrame = !input.Replay && (!collectFrame || input.FrameCounter != *collectFrame);
        if (input.Replay) interaction = {}; // source Update returns before any collect attempt
        if (newCollectFrame) {
            collectFrame = input.FrameCounter; interaction = {};
            if (input.CollectJustDown && !input.ControlsDisabled) buffer = 6;
            else if (buffer) --buffer;
            if (input.Targeting) buffer = 0;
        }
    }
    std::uint64_t helpChanges = 0;
    std::vector<NativeScriptPropertyLabel> labels;
    std::vector<std::size_t> latches;
    std::vector<std::pair<std::size_t, WorldShotScene>> poses;
    std::vector<std::pair<std::size_t, NativeScriptPickupRef>> collections;
    struct Visibility { std::size_t Slot; bool Visible, Present; };
    std::vector<Visibility> visibility;
    WorldShotScene actors{}; actors.images = m_Images;
    for (std::size_t i = 0; i < m_Pickups.size(); ++i) {
        const auto& p = m_Pickups[i]; if (!p.Active) continue;
        const bool sale = p.Type == 18;
        const bool ordinary = p.Type == 3;
        bool collected = false;
        auto actor = NativeScriptPropertyActor(ordinary ? m_SaveModel : sale ? m_ForSaleModel : m_Model, p.Position,
            ordinary ? m_SaveGeometry.Scale : sale ? m_ForSaleGeometry.Scale : m_PropertyGeometry.Scale, now);
        if (m_Frame) {
            const auto& f = *m_Frame;
            const auto dx = f.Ped.X - p.Position.X, dy = f.Ped.Y - p.Position.Y;
            const auto cx = f.Camera.X - p.Position.X, cy = f.Camera.Y - p.Position.Y;
            bool visible = cx*cx + cy*cy < 10000 && !(sale && f.Property && (f.Property->Cutscene || f.Property->Widescreen));
            if (ordinary) {
                if (!f.Property) { error = "ordinary pickup update requires source frame/global inputs"; return false; }
                const auto& input = *f.Property;
                auto sourceVisible = p.Visible, present = p.ObjectPresent;
                if (newCollectFrame) {
                    if (i >= 620 * (input.FrameCounter % 32) / 32 && i < 620 * (input.FrameCounter % 32 + 1) / 32) {
                        sourceVisible = cx*cx + cy*cy < 10000;
                        if (!sourceVisible) present = false;
                        else if (!present && !input.CutsceneLoaded) present = true;
                    }
                    if (sourceVisible && i >= 620 * (input.FrameCounter % 6) / 6 && i < 620 * (input.FrameCounter % 6 + 1) / 6) {
                        if (!present && !input.CutsceneLoaded) present = true;
                        if (m_UsesPlayerActivity) {
                            if (!m_PlayerActivity || m_PlayerActivity->FrameCounter != input.FrameCounter) {
                                m_PickupRequirement = {NativeScriptPickupRequirementKind::PlayerActivityAuthority,
                                    p.Reference, p.Position, input.FrameCounter, p.Model, p.Type, 0,
                                    {NativeMissionStartOutcome::Unsupported, NativeMissionStartReason::ActivityUnsupported}};
                                error = "ordinary pickup requires an activity snapshot for the same source frame"; return false;
                            }
                            const auto& activity = m_PlayerActivity->Snapshot;
                            if (activity.Authority != NativePlayerActivityAuthority::SourceBacked) {
                                m_PickupRequirement = {NativeScriptPickupRequirementKind::PlayerActivityAuthority,
                                    p.Reference, p.Position, input.FrameCounter, p.Model, p.Type, m_PlayerActivity->OwnerRevision,
                                    {NativeMissionStartOutcome::Unsupported, NativeMissionStartReason::ActivityUnsupported}};
                                error = "ordinary pickup activity authority is unsupported"; return false;
                            }
                            if (!NativePlayerPickupBusy(activity) && present && !input.Cutscene && !input.Widescreen && !input.Coop) {
                                // Pickup.cpp evaluates CanPlayerStartMission before
                                // vehicle/alive/proximity and unarmed pickup desire.
                                const auto decision = NativePlayerCanStartMission(activity);
                                if (decision.Outcome == NativeMissionStartOutcome::Unsupported) {
                                    m_PickupRequirement = {NativeScriptPickupRequirementKind::PlayerTaskEligibility,
                                        p.Reference, p.Position, input.FrameCounter, p.Model, p.Type, m_PlayerActivity->OwnerRevision, decision};
                                    error = "ordinary pickup mission-start activity is unsupported"; return false;
                                }
                                if (decision.Outcome == NativeMissionStartOutcome::Allowed && f.Alive && !f.InVehicle &&
                                    dx*dx + dy*dy < 1.8f && std::abs(f.Ped.Z - p.Position.Z) < 2.0f) {
                                    if (activity.WeaponSlots[0].Type == NativePlayerWeaponType::Unsupported) {
                                        m_PickupRequirement = {NativeScriptPickupRequirementKind::PlayerPickupDesire,
                                            p.Reference, p.Position, input.FrameCounter, p.Model, p.Type, m_PlayerActivity->OwnerRevision, decision};
                                        error = "ordinary pickup unarmed weapon-slot activity is unsupported"; return false;
                                    }
                                    if (NativePlayerWantsUnarmedPickup(activity)) {
                                        // Save model 1277 takes the default unarmed
                                        // path: shake, SetRemoved, then ring publish.
                                        padShakes.push_back({p.Reference, input.FrameCounter, 120, 100, 0});
                                        collections.emplace_back(i, p.Reference);
                                        present = false; sourceVisible = false; collected = true;
                                        collectedPickups[collectedPickupCursor] = p.Reference;
                                        collectedPickupCursor = (collectedPickupCursor + 1) % collectedPickups.size();
                                    }
                                }
                            }
                        } else if (!input.Busy && present && !input.Cutscene && !input.Widescreen && !input.Coop &&
                            f.Alive && !f.InVehicle && dx*dx + dy*dy < 1.8f && std::abs(f.Ped.Z - p.Position.Z) < 2.0f) {
                            // Legacy callers have no owned task/event authority.
                            m_PickupRequirement = {NativeScriptPickupRequirementKind::PlayerTaskEligibility,
                                p.Reference, p.Position, input.FrameCounter, p.Model, p.Type, 0,
                                {NativeMissionStartOutcome::Unsupported, NativeMissionStartReason::ActivityUnsupported}};
                            error = "ordinary pickup requires live CanPlayerStartMission task/event eligibility before collection";
                            return false;
                        }
                    }
                }
                visibility.push_back({i, sourceVisible, present});
                visible = present && !input.Cutscene && !input.Widescreen && !input.Coop;
            }
            if (f.Alive && !f.InVehicle && dx*dx + dy*dy < 1.8f && std::abs(f.Ped.Z - p.Position.Z) < 2.0f) {
                if (!sale && !ordinary && !p.HelpMessageDisplayed) {
                    help.Show(p.Message, p.MessageLines);
                    helpMessage = p.Message; latches.push_back(i); ++helpChanges;
                } else if (sale && visible && newCollectFrame && !f.Property->Busy &&
                    i >= 620 * (f.Property->FrameCounter % 6) / 6 && i < 620 * (f.Property->FrameCounter % 6 + 1) / 6) {
                    const auto& input = *f.Property;
                    if (!input.HelpBlocked && !help.Displayed()) {
                        help.Show(p.Message, p.MessageLines); helpMessage = p.Message; ++helpChanges;
                    }
                    const auto status = NativeScriptPropertyCollect(p.Price, input, buffer);
                    if (status != NativeScriptPropertyInteractionStatus::None) {
                        interaction = {status, p.Reference, p.Price, input.Money, input.FrameCounter};
                        if (!input.HelpBlocked) {
                            helpMessage = status == NativeScriptPropertyInteractionStatus::ScriptPurchaseRequired ? "" :
                                m_Denials[status == NativeScriptPropertyInteractionStatus::OnMission ? 1 : 0];
                            help.Show(helpMessage, std::uint32_t(NativeScriptHelpLines(helpMessage, m_HelpWidths).size()), true); ++helpChanges;
                        }
                    }
                }
            }
            if (visible && !collected) { // CPickup::IsVisible, XY <100m
                const auto first = actors.meshes.size();
                actors.meshes.insert(actors.meshes.end(), actor.meshes.begin(), actor.meshes.end());
                if (sale) for (std::size_t m = first; m < actors.meshes.size(); ++m) for (auto& image : actors.meshes[m].triImg) if (image >= 0) image += int(m_Model.images.size());
                if (ordinary) for (std::size_t m = first; m < actors.meshes.size(); ++m) for (auto& image : actors.meshes[m].triImg) if (image >= 0) image += int(m_Model.images.size() + m_ForSaleModel.images.size());
                actors.stats.triangles += actor.stats.triangles; actors.stats.atomics += actor.stats.atomics;
                actors.stats.vertices += actor.stats.vertices;
                // DoPickUpEffects: parent admits at most16 AFTER near/far
                // screen projection; candidates retain source pool order.
                const auto distance = std::sqrt(cx*cx + cy*cy);
                if (sale && distance < 14) {
                    const auto cost = uint32(p.CostValue)*5u;
                    labels.push_back({p.Reference, {p.Position.X, p.Position.Y, p.Position.Z + 0.7f}, cost,
                        cost ? "$" + std::to_string(cost) : p.Label, {255,100,100},
                        std::uint8_t((1.0 - double(distance)/14.0)*255.0)});
                }
            }
        }
        if (!collected) poses.emplace_back(i, std::move(actor));
    }
    if (!help.AdvanceTime(now, error)) return false;
    Bounds(actors);
    // All allocation/validation precedes no-throw publication. Tick inputs,
    // once-only latches, help clock and current-phase visible meshes agree.
    for (auto& [slot, actor] : poses) m_Pickups[slot].Actor = std::move(actor);
    for (const auto& v : visibility) { m_Pickups[v.Slot].Visible = v.Visible; m_Pickups[v.Slot].ObjectPresent = v.Present; }
    for (const auto slot : latches) m_Pickups[slot].HelpMessageDisplayed = true;
    // No throwing work remains: publish source removal before the collected
    // ring and its corresponding pending feedback events.
    for (const auto& [slot, ref] : collections) {
        auto& pickup = m_Pickups[slot]; assert(pickup.Reference.Value == ref.Value);
        pickup.Active = false; pickup.Type = 0; pickup.Visible = false; pickup.ObjectPresent = false; pickup.Actor = {};
    }
    m_CollectedPickups = collectedPickups; m_CollectedPickupCursor = collectedPickupCursor;
    m_PadShakes = std::move(padShakes); m_Revision += collections.size();
    m_Help = std::move(help); m_HelpMessage = std::move(helpMessage); m_HelpRevision += helpChanges;
    m_CollectBuffer = buffer; m_CollectFrame = collectFrame; m_Interaction = interaction; m_Labels = std::move(labels);
    m_PickupRequirement = {}; m_Actors = std::move(actors); m_PublishedFrame = m_Frame;
    m_GameMs = now; m_HasTime = true; m_PresentationRevision = m_Revision;
    error.clear(); return true;
} catch (const std::exception& e) { error = e.what(); return false; }

float NativeScriptPropertyScale(std::array<float, 3> minimum, std::array<float, 3> maximum) {
    float extent = 0;
    for (std::size_t i = 0; i < 3; ++i) {
        Require(std::isfinite(minimum[i]) && std::isfinite(maximum[i]) && maximum[i] >= minimum[i], "invalid property COL bounds");
        extent = std::max(extent, maximum[i] - minimum[i]);
    }
    Require(std::isfinite(extent) && extent > 0, "degenerate property COL bounds");
    // Retail 45A15F..45A1FF: largest COL extent, MINIMUM ratio of 1,
    // then interpolate 60% from unit scale. No recentering or temporal pulse.
    const auto ratio = std::max(1.0f, float(double(1.2f) / extent));
    const auto scale = float(1.0 + (double(ratio) - 1.0) * double(0.6f));
    Require(std::isfinite(scale), "property COL scale overflow"); return scale;
}
WorldShotScene NativeScriptPropertyActor(const WorldShotScene& bind, NativeScriptPosition position, float scale, std::uint32_t gameMs) {
    Require(Finite(position) && std::isfinite(scale) && scale >= 1, "invalid property transform");
    // The source overwrites its matrix basis, replacing the allocation heading.
    // Match the float angle and float sin/cos stores before the scale multiply.
    const float angle = float(double(gameMs & 2047) * 0.003056640736758709);
    const float c = float(std::cos(double(angle))), s = float(std::sin(double(angle)));
    const float cs = c * scale, ss = s * scale;
    auto actor = bind;
    for (std::size_t m = 0; m < bind.meshes.size(); ++m) {
        const auto& source = bind.meshes[m]; auto& mesh = actor.meshes[m];
        Require(source.pos.size() % 3 == 0 && source.nrm.size() == source.pos.size(), "property bind pose vector count");
        for (std::size_t i = 0; i < source.pos.size(); i += 3) {
            const auto x = source.pos[i], y = source.pos[i+1], z = source.pos[i+2];
            mesh.pos[i] = cs*x - ss*y + position.X; mesh.pos[i+1] = ss*x + cs*y + position.Y; mesh.pos[i+2] = scale*z + position.Z;
            // Uniform positive scale: inverse-transpose followed by unit length.
            // Always use immutable bind normals, never the last published pose.
            const auto nx = c*source.nrm[i] - s*source.nrm[i+1], ny = s*source.nrm[i] + c*source.nrm[i+1], nz = source.nrm[i+2];
            const auto length = std::sqrt(nx*nx + ny*ny + nz*nz);
            Require(std::isfinite(length) && length > 0 && Finite({mesh.pos[i], mesh.pos[i+1], mesh.pos[i+2]}), "invalid transformed property geometry");
            mesh.nrm[i] = nx/length; mesh.nrm[i+1] = ny/length; mesh.nrm[i+2] = nz/length;
        }
    }
    Bounds(actor); return actor;
}

std::vector<std::string> NativeScriptHelpLines(std::string_view text, std::span<const int, 208> widths) {
    std::vector<std::string> lines(1);
    float used = 0;
    for (std::size_t i = 0; i < text.size();) {
        if (text[i] == '\n') { lines.emplace_back(); used = 0; ++i; continue; }
        const auto start = i;
        float wordWidth = 0;
        while (i < text.size() && text[i] != ' ' && text[i] != '\n') {
            const auto ch = static_cast<unsigned char>(text[i++]);
            // CFont::GetCharacterSize, plain FONT_SUBTITLES: ASCII slice only.
            assert(ch >= 32 && ch < 128);
            wordWidth += widths[ch - 32] * 0.52f;
        }
        if (used + wordWidth > 196.0f && used > 0) { lines.emplace_back(); used = 0; }
        lines.back().append(text.substr(start, i - start)); used += wordWidth;
        if (i < text.size() && text[i] == ' ') {
            lines.back().push_back(' '); used += widths[0] * 0.52f; ++i;
        }
    }
    return lines;
}
void NativeScriptHelpPresentation::Show(std::string_view text, std::uint32_t lines, bool quick) {
    assert(text.size() < 400 && (text.empty() || lines) && lines <= 400);
    std::string owned(text);
    m_Text = std::move(owned);
    m_Lifetime = (lines + 3) * 1000; // retail 597c54: GetNumberLines + 3 seconds
    // SetHelpMessage clears all three buffers and resets nonpermanent state=0,
    // including when replacing an active message (594ce8..594dd2).
    m_State = m_Alpha = 0; m_NewMessage = !m_Text.empty();
    m_Quick = quick;
}
void NativeScriptHelpPresentation::Clear() {
    *this = NativeScriptHelpPresentation{};
}

NativeScriptServiceResult NativeScriptEntities::ClearHelp() {
    if (!m_Loaded) return Unsupported("help owner is unavailable");
    m_Help.Clear();
    m_HelpMessage.clear();
    ++m_HelpRevision;
    ++m_Revision;
    return Ready();
}
bool NativeScriptHelpPresentation::AdvanceTime(std::uint32_t now, std::string& error) {
    const auto elapsed = m_HasTime ? now - m_LastTime : 0;
    if (m_HasTime && elapsed > 0x7fffffff) { error = "help game time moved backwards or exceeded half-range"; return false; }
    if (m_HasTime && !elapsed) { error.clear(); return true; }
    m_HasTime = true; m_LastTime = now;
    if (m_NewMessage) {
        m_NewMessage = false; m_State = 2; m_Timer = 0; m_FadeTimer = 0;
    }
    m_Alpha = 0;
    switch (m_State) {
    case 0: break;
    case 1:
        m_Alpha = 200; m_FadeTimer = 600;
        if (m_Timer > m_Lifetime || (m_Quick && m_Timer > 3000)) m_State = 3; // source comparison precedes timer increment
        break;
    case 2:
        m_FadeTimer += 2 * std::int64_t(elapsed);
        // Original compares to ZERO, not 1000! First positive frame switches
        // to holding but prints alpha=0; next frame is the steady alpha=200.
        if (m_FadeTimer > 0) { m_State = 1; m_FadeTimer = 0; }
        break;
    case 3:
        m_FadeTimer -= 2 * std::int64_t(elapsed);
        if (m_FadeTimer < 0) { m_State = 0; m_FadeTimer = 0; }
        else m_Alpha = std::uint8_t(m_FadeTimer * 200 / 1000);
        break;
    default: assert(false);
    }
    if (m_State) m_Timer += elapsed;
    error.clear(); return true;
}
