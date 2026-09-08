// WorldShot implementation: IMG/loose-file loading + librw DFF/TXD parsing.
// See WorldShot.h for the contract. This TU owns librw: engine init, plugin
// registration (mirrors librw's clumpview attachPlugins), stream parsing.

#include "app/platform/linux/WorldShot.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>

// --- Basic RE types (mirrors `source/Base.h`, standalone-safe) ---
// Must precede `oswrapper.h`, same as in MainLinux.cpp.
using int8 = int8_t;
using int16 = int16_t;
using int32 = int32_t;
using int64 = int64_t;
using uint8 = uint8_t;
using uint16 = uint16_t;
using uint32 = uint32_t;
using uint64 = uint64_t;

#ifndef __stdcall
#define __stdcall
#endif

#include "oswrapper/oswrapper.h"

// librw umbrella header (pulls glad via the GL3 backend header; keep GL out).
#include <rw.h>

namespace {

void SetErr(char* err, std::size_t errSize, const char* msg) {
    if (!err || errSize == 0) {
        return;
    }
    (void)std::snprintf(err, errSize, "%s", msg ? msg : "unknown");
}

bool ReadWholeFile(const char* path, std::vector<uint8>& out) {
    void* file = nullptr;
    if (OS_FileOpen(FILE_DATA_AREA_DEFAULT, &file, path, FILE_ACCESS_READ) != 0 || !file) {
        return false;
    }
    int32 size = OS_FileSize(file);
    if (size < 0) {
        OS_FileClose(file);
        return false;
    }
    out.resize(static_cast<size_t>(size));
    bool ok = true;
    if (size > 0) {
        ok = OS_FileRead(file, out.data(), size) == 0;
    }
    OS_FileClose(file);
    return ok;
}

int StrCaseCmp(const char* a, const char* b) {
    while (*a && *b) {
        char ca = *a >= 'A' && *a <= 'Z' ? static_cast<char>(*a + 32) : *a;
        char cb = *b >= 'A' && *b <= 'Z' ? static_cast<char>(*b + 32) : *b;
        if (ca != cb) {
            return ca < cb ? -1 : 1;
        }
        ++a;
        ++b;
    }
    return *a == *b ? 0 : (*a ? 1 : -1);
}

// GTA:SA IMG archive, version VER2 (no .dir sidecar): header "VER2",
// uint32 entry count, then 32-byte entries: uint32 offset (2048-byte
// sectors), uint32 size (sectors), char name[24].
bool ImgReadEntry(const char* imgPath, const char* wantName, std::vector<uint8>& out) {
    void* file = nullptr;
    if (OS_FileOpen(FILE_DATA_AREA_DEFAULT, &file, imgPath, FILE_ACCESS_READ) != 0 || !file) {
        return false;
    }
    char magic[4] = {};
    uint32 count = 0;
    bool ok = OS_FileRead(file, magic, 4) == 0 && OS_FileRead(file, &count, 4) == 0;
    if (!ok || std::memcmp(magic, "VER2", 4) != 0 || count == 0 || count > 300000) {
        OS_FileClose(file);
        return false;
    }
    uint32 foundOff = 0;
    uint32 foundSize = 0;
    for (uint32 i = 0; i < count; ++i) {
        uint32 off = 0;
        uint32 size = 0;
        char name[24] = {};
        if (OS_FileRead(file, &off, 4) != 0 || OS_FileRead(file, &size, 4) != 0 ||
            OS_FileRead(file, name, 24) != 0) {
            OS_FileClose(file);
            return false;
        }
        name[23] = '\0';
        if (StrCaseCmp(name, wantName) == 0) {
            foundOff = off;
            foundSize = size & 0x7FFFu; // high bits carry streaming flags, not size
            break;
        }
    }
    if (foundSize == 0) {
        OS_FileClose(file);
        return false;
    }
    // Offsets are int32-based in OS_FileSetPosition; gta3.img (~900MB) fits.
    OS_FileSetPosition(file, static_cast<int32>(foundOff * 2048u));
    out.resize(static_cast<size_t>(foundSize) * 2048u);
    ok = OS_FileRead(file, out.data(), static_cast<int32>(out.size())) == 0;
    OS_FileClose(file);
    return ok;
}

bool LoadAssetBytes(const char* imgPath, const char* entryName, const char* loosePath,
                    std::vector<uint8>& out) {
    if (imgPath && entryName && ImgReadEntry(imgPath, entryName, out) && !out.empty()) {
        return true;
    }
    if (loosePath && ReadWholeFile(loosePath, out) && !out.empty()) {
        return true;
    }
    return false;
}

bool s_rwInit = false;

bool RwInitEngine() {
    if (s_rwInit) {
        return true;
    }
    if (!rw::Engine::init(nil)) {
        return false;
    }
    // Same stream-plugin set as librw's clumpview: lets SA-era DFF/TXD
    // chunks (skin, MatFX, HAnim, UVAnim, mesh, native data) parse while
    // unknown Rockstar extensions (2dfx & co) are skipped by the core.
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
    // Material::streamRead touches texture globals (TEXTUREGLOBAL derefs
    // `engine`), so the engine must be opened/started even though nothing
    // is ever rendered through librw. NULL platform: no window, headless.
    if (!rw::Engine::open(nil) || !rw::Engine::start()) {
        return false;
    }
    // Never probe the disk for missing textures: link only against the TXD
    // we load explicitly below; unknown names stay nil (material survives).
    rw::Texture::setLoadTextures(false);
    s_rwInit = true;
    return true;
}

rw::Clump* ParseClump(const std::vector<uint8>& bytes) {
    if (bytes.size() < 12) {
        return nil;
    }
    rw::StreamMemory stream;
    // StreamMemory::open does not copy; bytes must outlive the parse.
    stream.open(const_cast<uint8*>(bytes.data()), static_cast<uint32>(bytes.size()));
    if (!rw::findChunk(&stream, rw::ID_CLUMP, nil, nil)) {
        stream.close();
        return nil;
    }
    rw::Clump* clump = rw::Clump::streamRead(&stream);
    stream.close();
    return clump;
}

rw::TexDictionary* ParseTxd(const std::vector<uint8>& bytes) {
    if (bytes.size() < 12) {
        return nil;
    }
    rw::StreamMemory stream;
    stream.open(const_cast<uint8*>(bytes.data()), static_cast<uint32>(bytes.size()));
    if (!rw::findChunk(&stream, rw::ID_TEXDICTIONARY, nil, nil)) {
        stream.close();
        return nil;
    }
    rw::TexDictionary* txd = rw::TexDictionary::streamRead(&stream);
    stream.close();
    return txd;
}

void CrossSub(const float* a, const float* b, const float* c, float* n) {
    float ux = b[0] - a[0];
    float uy = b[1] - a[1];
    float uz = b[2] - a[2];
    float vx = c[0] - a[0];
    float vy = c[1] - a[1];
    float vz = c[2] - a[2];
    n[0] = uy * vz - uz * vy;
    n[1] = uz * vx - ux * vz;
    n[2] = ux * vy - uy * vx;
    float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (len > 1e-9f) {
        n[0] /= len;
        n[1] /= len;
        n[2] /= len;
    } else {
        n[0] = 0.0f;
        n[1] = 0.0f;
        n[2] = 1.0f;
    }
}

// Muted SA-ish palette so meshes are distinguishable without textures.
void MeshColor(int index, float* rgb) {
    static const float kPalette[][3] = {
        { 0.78f, 0.74f, 0.68f }, // concrete
        { 0.85f, 0.70f, 0.48f }, // sand
        { 0.55f, 0.60f, 0.62f }, // asphalt grey
        { 0.72f, 0.30f, 0.24f }, // brick red
        { 0.35f, 0.52f, 0.70f }, // painted blue
        { 0.45f, 0.62f, 0.38f }, // scrub green
        { 0.88f, 0.86f, 0.80f }, // off-white
        { 0.60f, 0.45f, 0.62f }, // dusk purple
    };
    const int count = 8;
    const float* pick = kPalette[index % count < 0 ? 0 : index % count];
    rgb[0] = pick[0];
    rgb[1] = pick[1];
    rgb[2] = pick[2];
}

// Flattens every atomic of the clump into world-space triangles.
// Returns the triangle count (0 = unusable for render).
int FlattenClump(rw::Clump* clump, WorldShotScene& scene) {
    int meshIndex = 0;
    int totalTris = 0;
    int totalVerts = 0;
    scene.meshes.clear();
    bool first = true;
    {
        FORLIST(link, clump->atomics) {
            rw::Atomic* atomic = rw::Atomic::fromClump(link);
            rw::Geometry* geo = atomic ? atomic->geometry : nil;
            if (!geo || geo->numTriangles <= 0 || geo->numVertices <= 0) {
                continue;
            }
            if (!geo->triangles || !geo->morphTargets || !geo->morphTargets[0].vertices) {
                continue; // native-only geometry: nothing CPU-readable to draw
            }
            const int numVerts = geo->numVertices;
            rw::V3d* verts = geo->morphTargets[0].vertices;
            rw::V3d* norms = (geo->flags & rw::Geometry::NORMALS) ? geo->morphTargets[0].normals : nil;
            // To world space through the atomic frame (static props: ~identity).
            std::vector<rw::V3d> worldVerts(static_cast<size_t>(numVerts));
            std::vector<rw::V3d> worldNorms(norms ? static_cast<size_t>(numVerts) : 0);
            rw::Frame* frame = atomic->getFrame();
            rw::Matrix* ltm = frame ? frame->getLTM() : nil;
            if (ltm) {
                rw::V3d::transformPoints(worldVerts.data(), verts, numVerts, ltm);
                if (norms) {
                    rw::V3d::transformVectors(worldNorms.data(), norms, numVerts, ltm);
                }
            } else {
                for (int i = 0; i < numVerts; ++i) {
                    worldVerts[static_cast<size_t>(i)] = verts[i];
                    if (norms) {
                        worldNorms[static_cast<size_t>(i)] = norms[i];
                    }
                }
            }
            WorldShotMesh mesh;
            MeshColor(meshIndex, mesh.color);
            mesh.tris = 0;
            mesh.pos.reserve(static_cast<size_t>(geo->numTriangles) * 9);
            mesh.nrm.reserve(static_cast<size_t>(geo->numTriangles) * 9);
            for (int t = 0; t < geo->numTriangles; ++t) {
                const rw::Triangle& tri = geo->triangles[t];
                if (tri.v[0] >= numVerts || tri.v[1] >= numVerts || tri.v[2] >= numVerts) {
                    continue;
                }
                const rw::V3d* p[3] = {
                    &worldVerts[tri.v[0]],
                    &worldVerts[tri.v[1]],
                    &worldVerts[tri.v[2]],
                };
                float face[3];
                {
                    float a[3] = { p[0]->x, p[0]->y, p[0]->z };
                    float b[3] = { p[1]->x, p[1]->y, p[1]->z };
                    float c[3] = { p[2]->x, p[2]->y, p[2]->z };
                    CrossSub(a, b, c, face);
                }
                for (int k = 0; k < 3; ++k) {
                    mesh.pos.push_back(p[k]->x);
                    mesh.pos.push_back(p[k]->y);
                    mesh.pos.push_back(p[k]->z);
                    if (norms) {
                        const rw::V3d& n = worldNorms[tri.v[k]];
                        float len = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
                        if (len > 1e-9f) {
                            mesh.nrm.push_back(n.x / len);
                            mesh.nrm.push_back(n.y / len);
                            mesh.nrm.push_back(n.z / len);
                        } else {
                            mesh.nrm.push_back(face[0]);
                            mesh.nrm.push_back(face[1]);
                            mesh.nrm.push_back(face[2]);
                        }
                    } else {
                        mesh.nrm.push_back(face[0]);
                        mesh.nrm.push_back(face[1]);
                        mesh.nrm.push_back(face[2]);
                    }
                    if (first) {
                        scene.bboxMin[0] = scene.bboxMax[0] = p[k]->x;
                        scene.bboxMin[1] = scene.bboxMax[1] = p[k]->y;
                        scene.bboxMin[2] = scene.bboxMax[2] = p[k]->z;
                        first = false;
                    } else {
                        if (p[k]->x < scene.bboxMin[0]) scene.bboxMin[0] = p[k]->x;
                        if (p[k]->y < scene.bboxMin[1]) scene.bboxMin[1] = p[k]->y;
                        if (p[k]->z < scene.bboxMin[2]) scene.bboxMin[2] = p[k]->z;
                        if (p[k]->x > scene.bboxMax[0]) scene.bboxMax[0] = p[k]->x;
                        if (p[k]->y > scene.bboxMax[1]) scene.bboxMax[1] = p[k]->y;
                        if (p[k]->z > scene.bboxMax[2]) scene.bboxMax[2] = p[k]->z;
                    }
                }
                ++mesh.tris;
            }
            if (mesh.tris > 0) {
                totalTris += mesh.tris;
                totalVerts += mesh.tris * 3;
                scene.meshes.push_back(std::move(mesh));
                ++meshIndex;
            }
        }
    }
    scene.stats.atomics = meshIndex;
    scene.stats.triangles = totalTris;
    scene.stats.vertices = totalVerts;
    return totalTris;
}

struct DffCandidate {
    const char* img;
    const char* entry;
    const char* loose;
    const char* label;
};

struct TxdCandidate {
    const char* img;
    const char* entry;
    const char* loose;
    const char* label;
};

rw::Clump* s_clump = nil;
rw::TexDictionary* s_txd = nil;

} // namespace

bool WorldShot_Init(const char* gameDir, WorldShotScene& scene, char* err, std::size_t errSize) {
    if (!gameDir || !gameDir[0]) {
        SetErr(err, errSize, "no game dir");
        return false;
    }
    OS_SetFilePathOffset(gameDir);
    if (!RwInitEngine()) {
        SetErr(err, errSize, "librw Engine::init failed");
        return false;
    }

    s_clump = nil;
    s_txd = nil;

    // TXD first: DFF materials resolve texture names against the current
    // dictionary during Clump::streamRead.
    static const TxdCandidate kTxds[] = {
        { "models/gta3.img", "weemap.txd", nullptr, "gta3.img:weemap.txd" },
        { nullptr, nullptr, "models/generic/vehicle.txd", "models/generic/vehicle.txd" },
        { nullptr, nullptr, "models/hud.txd", "models/hud.txd" },
    };
    (void)std::snprintf(scene.stats.txdName, sizeof(scene.stats.txdName), "%s", "none");
    scene.stats.textures = 0;
    scene.stats.firstTexture[0] = '\0';
    scene.stats.firstTexW = 0;
    scene.stats.firstTexH = 0;
    for (const TxdCandidate& cand : kTxds) {
        std::vector<uint8> bytes;
        if (!LoadAssetBytes(cand.img, cand.entry, cand.loose, bytes)) {
            continue;
        }
        rw::TexDictionary* txd = ParseTxd(bytes);
        if (!txd || txd->count() <= 0) {
            if (txd) {
                txd->destroy();
            }
            continue;
        }
        s_txd = txd;
        rw::TexDictionary::setCurrent(txd);
        (void)std::snprintf(scene.stats.txdName, sizeof(scene.stats.txdName), "%s", cand.label);
        scene.stats.textures = txd->count();
        {
            FORLIST(link, txd->textures) {
                rw::Texture* tex = rw::Texture::fromDict(link);
                if (!tex) {
                    continue;
                }
                (void)std::snprintf(
                    scene.stats.firstTexture, sizeof(scene.stats.firstTexture), "%s", tex->name
                );
                scene.stats.firstTexW = tex->raster ? tex->raster->width : 0;
                scene.stats.firstTexH = tex->raster ? tex->raster->height : 0;
                break;
            }
        }
        break;
    }
    if (!s_txd) {
        // TXD is best-effort stats, but the stream reader still needs a
        // current dictionary so textured materials parse to nil safely.
        s_txd = rw::TexDictionary::create();
        if (s_txd) {
            rw::TexDictionary::setCurrent(s_txd);
        }
    }

    static const DffCandidate kDffs[] = {
        { "models/gta3.img", "fbiranch.dff", nullptr, "gta3.img:fbiranch.dff" },
        { "models/gta3.img", "cityhall_sfs.dff", nullptr, "gta3.img:cityhall_sfs.dff" },
        { "models/gta3.img", "hanger.dff", nullptr, "gta3.img:hanger.dff" },
        { "models/gta3.img", "bridge_1.dff", nullptr, "gta3.img:bridge_1.dff" },
        { "models/gta3.img", "helipad.dff", nullptr, "gta3.img:helipad.dff" },
        { nullptr, nullptr, "models/generic/arrow.DFF", "models/generic/arrow.DFF" },
        { nullptr, nullptr, "models/grass/grass0_1.dff", "models/grass/grass0_1.dff" },
    };
    char dffLabel[128] = {};
    int bestTris = 0;
    // Try every candidate; keep the richest one so the frame shows real
    // 3D volume (flat decals like the helipad rasterize to a thin strip).
    for (const DffCandidate& cand : kDffs) {
        std::vector<uint8> bytes;
        if (!LoadAssetBytes(cand.img, cand.entry, cand.loose, bytes)) {
            continue;
        }
        rw::Clump* clump = ParseClump(bytes);
        if (!clump) {
            continue;
        }
        WorldShotScene probe;
        int tris = FlattenClump(clump, probe);
        if (tris <= 0) {
            clump->destroy();
            continue;
        }
        if (!s_clump || tris > bestTris) {
            if (s_clump) {
                s_clump->destroy();
            }
            s_clump = clump;
            scene.meshes = std::move(probe.meshes);
            scene.bboxMin[0] = probe.bboxMin[0];
            scene.bboxMin[1] = probe.bboxMin[1];
            scene.bboxMin[2] = probe.bboxMin[2];
            scene.bboxMax[0] = probe.bboxMax[0];
            scene.bboxMax[1] = probe.bboxMax[1];
            scene.bboxMax[2] = probe.bboxMax[2];
            scene.stats.atomics = probe.stats.atomics;
            scene.stats.triangles = probe.stats.triangles;
            scene.stats.vertices = probe.stats.vertices;
            (void)std::snprintf(dffLabel, sizeof(dffLabel), "%s", cand.label);
            bestTris = tris;
        } else {
            clump->destroy();
        }
    }
    if (!s_clump) {
        SetErr(err, errSize, "no DFF candidate parsed (gta3.img fbiranch/cityhall/hanger/bridge/helipad, generic/arrow, grass)");
        return false;
    }
    (void)std::snprintf(scene.stats.dffName, sizeof(scene.stats.dffName), "%s", dffLabel);
    // TXD stays best-effort: the proof is rasterized DFF geometry; texture
    // stats above report what actually loaded (possibly none).
    return true;
}

void WorldShot_Shutdown() {
    if (s_clump) {
        s_clump->destroy();
        s_clump = nil;
    }
    if (s_txd) {
        s_txd->destroy();
        s_txd = nil;
    }
}
