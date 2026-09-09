// SkinPed implementation: single skinned character in bind pose (R6i).
// See SkinPed.h for the contract. This TU owns its own librw engine handle
// (same NULL-platform parse-only plugin set as WorldShot/SceneShot); only
// one shot path runs per process, so there is no double Engine::init.
// IMG-index helpers are deliberately duplicated from SceneShot.cpp per the
// native-track precedent (no refactors of verified slices inside a round).

#include "app/platform/linux/SkinPed.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <map>

#include "app/platform/linux/TexSample.h"

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

// librw umbrella header (same NULL-platform parse-only use as WorldShot).
#include <rw.h>

namespace {

void SetErr(char* err, std::size_t errSize, const char* msg) {
    if (!err || errSize == 0) {
        return;
    }
    (void)std::snprintf(err, errSize, "%s", msg ? msg : "unknown");
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

void ToLowerInPlace(std::string& s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c + 32);
        }
    }
}

bool ReadWholeFileOS(const char* path, std::vector<uint8>& out) {
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

struct ImgEntry {
    std::string name; // as stored
    std::string nameLower;
    uint32 off = 0; // 2048-byte sectors
    uint32 size = 0; // sectors (streaming flag bits masked out)
};

struct ImgIndex {
    std::string rel; // e.g. "models/gta3.img" (stdio path suffix)
    std::string label; // e.g. "gta3.img" (log name)
    std::vector<ImgEntry> entries;
};

bool BuildImgIndex(const char* imgRel, ImgIndex& idx) {
    std::vector<uint8> head;
    if (!ReadWholeFileOS(imgRel, head) || head.size() < 8) {
        return false;
    }
    if (std::memcmp(head.data(), "VER2", 4) != 0) {
        return false;
    }
    uint32 count = 0;
    std::memcpy(&count, head.data() + 4, 4);
    if (count == 0 || count > 300000 || head.size() < 8 + static_cast<size_t>(count) * 32) {
        return false;
    }
    // The full IMG may exceed int32; entries are read through OS_File with a
    // 64-bit-safe local reader instead (fopen/fseeko, game dir absolute).
    idx.rel = imgRel;
    idx.label = imgRel;
    {
        size_t slash = idx.label.rfind('/');
        if (slash != std::string::npos) {
            idx.label = idx.label.substr(slash + 1);
        }
    }
    idx.entries.reserve(count);
    for (uint32 i = 0; i < count; ++i) {
        const uint8* e = head.data() + 8 + static_cast<size_t>(i) * 32;
        ImgEntry en;
        std::memcpy(&en.off, e, 4);
        std::memcpy(&en.size, e + 4, 4);
        char nm[24] = {};
        std::memcpy(nm, e + 8, 24);
        nm[23] = '\0';
        en.name = nm;
        en.nameLower = nm;
        ToLowerInPlace(en.nameLower);
        en.size &= 0x7FFFu; // high bits carry streaming flags, not size
        idx.entries.push_back(en);
    }
    return true;
}

// Absolute game path for stdio reads (IMG bodies can exceed int32, which
// OS_FileSetPosition cannot address; the header above stays on OS_File*).
std::string s_gameAbs;

bool ImgReadBytesStd(const ImgIndex& idx, const std::string& wantLower, std::vector<uint8>& out) {
    for (const ImgEntry& e : idx.entries) {
        if (e.nameLower != wantLower || e.size == 0) {
            continue;
        }
        std::string abs = s_gameAbs + "/" + idx.rel;
        FILE* f = std::fopen(abs.c_str(), "rb");
        if (!f) {
            return false;
        }
        bool ok = fseeko(f, static_cast<off_t>(e.off) * 2048, SEEK_SET) == 0;
        out.resize(static_cast<size_t>(e.size) * 2048u);
        if (ok && !out.empty()) {
            ok = std::fread(out.data(), 1, out.size(), f) == out.size();
        }
        (void)std::fclose(f);
        return ok && !out.empty();
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
    // Same stream-plugin set as WorldShot (librw clumpview): SA-era DFF/TXD
    // chunks parse while unknown Rockstar extensions are skipped by the core.
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
    if (!rw::Engine::open(nil) || !rw::Engine::start()) {
        return false;
    }
    rw::Texture::setLoadTextures(false);
    s_rwInit = true;
    return true;
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

void MeshColor(int index, float* rgb) {
    static const float kPalette[][3] = {
        { 0.78f, 0.74f, 0.68f }, { 0.85f, 0.70f, 0.48f }, { 0.55f, 0.60f, 0.62f },
        { 0.72f, 0.30f, 0.24f }, { 0.35f, 0.52f, 0.70f }, { 0.45f, 0.62f, 0.38f },
        { 0.88f, 0.86f, 0.80f }, { 0.60f, 0.45f, 0.62f },
    };
    const int count = 8;
    int pick = index % count;
    if (pick < 0) {
        pick = 0;
    }
    rgb[0] = kPalette[pick][0];
    rgb[1] = kPalette[pick][1];
    rgb[2] = kPalette[pick][2];
}

// Evidence vertex (index 0 of the first skinned geometry): stored + skinned.
SkinPedVert s_vert{};
bool s_haveVert = false;

// Bind-pose skinning of one skinned atomic into the scene (appends meshes).
// Returns the skinned triangle count (0 = unusable). Updates wsumAcc with
// the per-vertex weight sums and boneDevAcc with |S_i - I| samples.
int FlattenSkinned(rw::Clump* clump, rw::Atomic* atomic, const LinkedClump& lc, WorldShotScene& scene,
                   int& meshIndex, bool& first, double& wsumAcc, long& wsumVerts, double& boneDevAcc,
                   long& boneDevSamples, int& bonesOut, int& attachedOut, int geomSerial) {
    rw::Geometry* geo = atomic ? atomic->geometry : nil;
    if (!geo || geo->numTriangles <= 0 || geo->numVertices <= 0) {
        return 0;
    }
    if (!geo->triangles || !geo->morphTargets || !geo->morphTargets[0].vertices) {
        return 0; // native-only geometry: nothing CPU-readable to draw
    }
    rw::Skin* skin = rw::Skin::get(geo);
    if (!skin || skin->numBones <= 0 || !skin->indices || !skin->weights ||
        !skin->inverseMatrices) {
        return 0;
    }
    const int numVerts = geo->numVertices;
    const int numBones = skin->numBones;
    rw::V3d* verts = geo->morphTargets[0].vertices;
    rw::V3d* norms = (geo->flags & rw::Geometry::NORMALS) ? geo->morphTargets[0].normals : nil;
    rw::TexCoords* uvs = geo->texCoords[0];

    // Bone hierarchy: the atomic's own (SA peds) or the clump-wide one.
    // attach() links hierarchy nodes to frames by bone ID; frame LTMs as
    // streamed are the bind pose (anim=bind: no HAnim interpolation).
    rw::HAnimHierarchy* hier = rw::Skin::getHierarchy(atomic);
    if (!hier && clump) {
        hier = rw::HAnimHierarchy::find(clump->getFrame());
    }
    bool useHier = hier && hier->numNodes == numBones && hier->nodeInfo;
    int attached = 0;
    if (useHier) {
        hier->attach();
        for (int i = 0; i < numBones; ++i) {
            if (hier->nodeInfo[i].frame) {
                ++attached;
            }
        }
        if (attached == 0) {
            useHier = false; // unattached: fall back to atomic-frame bind below
        }
    }
    attachedOut = useHier ? attached : 0;

    rw::Frame* atomicFrame = atomic->getFrame();
    rw::Matrix* atomicLTM = atomicFrame ? atomicFrame->getLTM() : nil;
    rw::Matrix atomicMat;
    if (atomicLTM) {
        atomicMat = *atomicLTM;
    } else {
        atomicMat.setIdentity();
    }
    rw::Matrix invAtomic;
    rw::Matrix::invert(&invAtomic, &atomicMat);

    // Skin matrices S_i = IB_i * (W_i * inv(A)) — librw gl3skin non-local
    // branch with W_i = bind-pose bone world LTM. (SA ped hierarchies carry
    // flags=0x0; a LOCALSPACEMATRICES file would resolve against the
    // hierarchy parent instead — no such file met in player/gta3 IMGs, and
    // binddev below would expose the mismatch honestly.)
    std::vector<rw::Matrix> skinMats(static_cast<size_t>(numBones));
    for (int i = 0; i < numBones; ++i) {
        rw::Matrix ib;
        std::memcpy(&ib, skin->inverseMatrices + static_cast<size_t>(i) * 16, 64);
        ib.flags = 0;
        const rw::Matrix* w = &atomicMat;
        if (useHier && hier->nodeInfo[i].frame) {
            w = hier->nodeInfo[i].frame->getLTM();
        }
        rw::Matrix tmp;
        rw::Matrix::mult(&tmp, w, &invAtomic);
        rw::Matrix::mult(&skinMats[static_cast<size_t>(i)], &ib, &tmp);
        // Bind-pose proof: S_i must be ~identity (12 live floats).
        const rw::Matrix& s = skinMats[static_cast<size_t>(i)];
        double dev = 0.0;
        dev += std::fabs(s.right.x - 1.0) + std::fabs(s.right.y) + std::fabs(s.right.z);
        dev += std::fabs(s.up.x) + std::fabs(s.up.y - 1.0) + std::fabs(s.up.z);
        dev += std::fabs(s.at.x) + std::fabs(s.at.y) + std::fabs(s.at.z - 1.0);
        dev += std::fabs(s.pos.x) + std::fabs(s.pos.y) + std::fabs(s.pos.z);
        boneDevAcc += dev / 12.0;
        ++boneDevSamples;
    }
    bonesOut = numBones;

    // Per-vertex bind skinning through librw's own point/vector transforms.
    std::vector<rw::V3d> skinnedPos(static_cast<size_t>(numVerts));
    std::vector<rw::V3d> skinnedNrm(norms ? static_cast<size_t>(numVerts) : 0);
    for (int v = 0; v < numVerts; ++v) {
        const uint8* idx = skin->indices + static_cast<size_t>(v) * 4;
        const float* wgt = skin->weights + static_cast<size_t>(v) * 4;
        double wsum = 0.0;
        rw::V3d p = { 0.0f, 0.0f, 0.0f };
        rw::V3d n = { 0.0f, 0.0f, 1.0f };
        for (int k = 0; k < 4; ++k) {
            int b = idx[k];
            float w = wgt[k];
            wsum += w;
            if (w == 0.0f || b < 0 || b >= numBones) {
                continue;
            }
            rw::V3d tp;
            rw::V3d::transformPoints(&tp, &verts[v], 1, &skinMats[static_cast<size_t>(b)]);
            p.x += w * tp.x;
            p.y += w * tp.y;
            p.z += w * tp.z;
            if (norms) {
                rw::V3d tn;
                rw::V3d::transformVectors(&tn, &norms[v], 1, &skinMats[static_cast<size_t>(b)]);
                n.x += w * tn.x;
                n.y += w * tn.y;
                n.z += w * tn.z;
            }
        }
        wsumAcc += wsum;
        ++wsumVerts;
        skinnedPos[static_cast<size_t>(v)] = p;
        if (norms) {
            float len = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
            if (len > 1e-9f) {
                n.x /= len;
                n.y /= len;
                n.z /= len;
            } else {
                n.x = 0.0f;
                n.y = 0.0f;
                n.z = 1.0f;
            }
            skinnedNrm[static_cast<size_t>(v)] = n;
        }
        if (geomSerial == 0 && v == 0 && !s_haveVert) {
            s_vert.stored[0] = verts[v].x;
            s_vert.stored[1] = verts[v].y;
            s_vert.stored[2] = verts[v].z;
            s_vert.skinned[0] = p.x;
            s_vert.skinned[1] = p.y;
            s_vert.skinned[2] = p.z;
            for (int k = 0; k < 4; ++k) {
                s_vert.bones[k] = idx[k];
                s_vert.weights[k] = wgt[k];
            }
            s_haveVert = true;
        }
    }

    std::map<const rw::Texture*, int> imgCache;
    WorldShotMesh mesh;
    MeshColor(meshIndex, mesh.color);
    mesh.tris = 0;
    mesh.pos.reserve(static_cast<size_t>(geo->numTriangles) * 9);
    mesh.nrm.reserve(static_cast<size_t>(geo->numTriangles) * 9);
    mesh.uv.reserve(static_cast<size_t>(geo->numTriangles) * 6);
    mesh.triImg.reserve(static_cast<size_t>(geo->numTriangles));
    mesh.triCol.reserve(static_cast<size_t>(geo->numTriangles) * 3);
    for (int t = 0; t < geo->numTriangles; ++t) {
        const rw::Triangle& tri = geo->triangles[t];
        if (tri.v[0] >= numVerts || tri.v[1] >= numVerts || tri.v[2] >= numVerts) {
            continue;
        }
        int imgIdx = -1;
        float matCol[3] = { 1.0f, 1.0f, 1.0f };
        rw::Material* mat =
            (tri.matId < geo->matList.numMaterials) ? geo->matList.materials[tri.matId] : nil;
        if (mat) {
            matCol[0] = mat->color.red / 255.0f;
            matCol[1] = mat->color.green / 255.0f;
            matCol[2] = mat->color.blue / 255.0f;
            if (mat->texture) {
                auto rit = lc.resolved.find(mat->texture);
                if (rit != lc.resolved.end() && rit->second.real) {
                    const rw::Texture* real = rit->second.real;
                    auto cit = imgCache.find(real);
                    if (cit != imgCache.end()) {
                        imgIdx = cit->second;
                    } else {
                        TexImage decoded;
                        if (TexSample_Decode(real, decoded)) {
                            decoded.filter = rit->second.filter;
                            imgIdx = static_cast<int>(scene.images.size());
                            scene.images.push_back(std::move(decoded));
                            imgCache[real] = imgIdx;
                        } else {
                            imgIdx = -2;
                        }
                    }
                } else {
                    imgIdx = -2;
                }
            }
        }
        const rw::V3d* p[3] = {
            &skinnedPos[tri.v[0]],
            &skinnedPos[tri.v[1]],
            &skinnedPos[tri.v[2]],
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
                const rw::V3d& n = skinnedNrm[tri.v[k]];
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
            if (uvs) {
                mesh.uv.push_back(uvs[tri.v[k]].u);
                mesh.uv.push_back(uvs[tri.v[k]].v);
            } else {
                mesh.uv.push_back(0.0f);
                mesh.uv.push_back(0.0f);
            }
            if (first) {
                scene.bboxMin[0] = scene.bboxMax[0] = p[k]->x;
                scene.bboxMin[1] = scene.bboxMax[1] = p[k]->y;
                scene.bboxMin[2] = scene.bboxMax[2] = p[k]->z;
                first = false;
            } else {
                if (p[k]->x < scene.bboxMin[0]) {
                    scene.bboxMin[0] = p[k]->x;
                }
                if (p[k]->y < scene.bboxMin[1]) {
                    scene.bboxMin[1] = p[k]->y;
                }
                if (p[k]->z < scene.bboxMin[2]) {
                    scene.bboxMin[2] = p[k]->z;
                }
                if (p[k]->x > scene.bboxMax[0]) {
                    scene.bboxMax[0] = p[k]->x;
                }
                if (p[k]->y > scene.bboxMax[1]) {
                    scene.bboxMax[1] = p[k]->y;
                }
                if (p[k]->z > scene.bboxMax[2]) {
                    scene.bboxMax[2] = p[k]->z;
                }
            }
        }
        mesh.triImg.push_back(imgIdx);
        mesh.triCol.push_back(matCol[0]);
        mesh.triCol.push_back(matCol[1]);
        mesh.triCol.push_back(matCol[2]);
        ++mesh.tris;
    }
    if (mesh.tris <= 0) {
        return 0;
    }
    int got = mesh.tris;
    scene.meshes.push_back(std::move(mesh));
    ++meshIndex;
    return got;
}

// Static (unskinned) atomic in the same clump: object-space LTM path, same
// as SceneShot's static flatten (accessories co-streamed with the ped).
int FlattenStatic(rw::Atomic* atomic, const LinkedClump& lc, WorldShotScene& scene, int& meshIndex,
                  bool& first) {
    rw::Geometry* geo = atomic ? atomic->geometry : nil;
    if (!geo || geo->numTriangles <= 0 || geo->numVertices <= 0) {
        return 0;
    }
    if (!geo->triangles || !geo->morphTargets || !geo->morphTargets[0].vertices) {
        return 0;
    }
    if (rw::Skin::get(geo)) {
        return 0; // handled by the skin path, never double-counted
    }
    const int numVerts = geo->numVertices;
    rw::V3d* verts = geo->morphTargets[0].vertices;
    rw::V3d* norms = (geo->flags & rw::Geometry::NORMALS) ? geo->morphTargets[0].normals : nil;
    rw::TexCoords* uvs = geo->texCoords[0];
    std::vector<rw::V3d> objVerts(static_cast<size_t>(numVerts));
    std::vector<rw::V3d> objNorms(norms ? static_cast<size_t>(numVerts) : 0);
    rw::Frame* frame = atomic->getFrame();
    rw::Matrix* ltm = frame ? frame->getLTM() : nil;
    if (ltm) {
        rw::V3d::transformPoints(objVerts.data(), verts, numVerts, ltm);
        if (norms) {
            rw::V3d::transformVectors(objNorms.data(), norms, numVerts, ltm);
        }
    } else {
        for (int i = 0; i < numVerts; ++i) {
            objVerts[static_cast<size_t>(i)] = verts[i];
            if (norms) {
                objNorms[static_cast<size_t>(i)] = norms[i];
            }
        }
    }
    std::map<const rw::Texture*, int> imgCache;
    WorldShotMesh mesh;
    MeshColor(meshIndex, mesh.color);
    mesh.tris = 0;
    for (int t = 0; t < geo->numTriangles; ++t) {
        const rw::Triangle& tri = geo->triangles[t];
        if (tri.v[0] >= numVerts || tri.v[1] >= numVerts || tri.v[2] >= numVerts) {
            continue;
        }
        int imgIdx = -1;
        float matCol[3] = { 1.0f, 1.0f, 1.0f };
        rw::Material* mat =
            (tri.matId < geo->matList.numMaterials) ? geo->matList.materials[tri.matId] : nil;
        if (mat) {
            matCol[0] = mat->color.red / 255.0f;
            matCol[1] = mat->color.green / 255.0f;
            matCol[2] = mat->color.blue / 255.0f;
            if (mat->texture) {
                auto rit = lc.resolved.find(mat->texture);
                if (rit != lc.resolved.end() && rit->second.real) {
                    const rw::Texture* real = rit->second.real;
                    auto cit = imgCache.find(real);
                    if (cit != imgCache.end()) {
                        imgIdx = cit->second;
                    } else {
                        TexImage decoded;
                        if (TexSample_Decode(real, decoded)) {
                            decoded.filter = rit->second.filter;
                            imgIdx = static_cast<int>(scene.images.size());
                            scene.images.push_back(std::move(decoded));
                            imgCache[real] = imgIdx;
                        } else {
                            imgIdx = -2;
                        }
                    }
                } else {
                    imgIdx = -2;
                }
            }
        }
        const rw::V3d* p[3] = { &objVerts[tri.v[0]], &objVerts[tri.v[1]], &objVerts[tri.v[2]] };
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
                const rw::V3d& n = objNorms[tri.v[k]];
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
            if (uvs) {
                mesh.uv.push_back(uvs[tri.v[k]].u);
                mesh.uv.push_back(uvs[tri.v[k]].v);
            } else {
                mesh.uv.push_back(0.0f);
                mesh.uv.push_back(0.0f);
            }
            if (first) {
                scene.bboxMin[0] = scene.bboxMax[0] = p[k]->x;
                scene.bboxMin[1] = scene.bboxMax[1] = p[k]->y;
                scene.bboxMin[2] = scene.bboxMax[2] = p[k]->z;
                first = false;
            } else {
                if (p[k]->x < scene.bboxMin[0]) {
                    scene.bboxMin[0] = p[k]->x;
                }
                if (p[k]->y < scene.bboxMin[1]) {
                    scene.bboxMin[1] = p[k]->y;
                }
                if (p[k]->z < scene.bboxMin[2]) {
                    scene.bboxMin[2] = p[k]->z;
                }
                if (p[k]->x > scene.bboxMax[0]) {
                    scene.bboxMax[0] = p[k]->x;
                }
                if (p[k]->y > scene.bboxMax[1]) {
                    scene.bboxMax[1] = p[k]->y;
                }
                if (p[k]->z > scene.bboxMax[2]) {
                    scene.bboxMax[2] = p[k]->z;
                }
            }
        }
        mesh.triImg.push_back(imgIdx);
        mesh.triCol.push_back(matCol[0]);
        mesh.triCol.push_back(matCol[1]);
        mesh.triCol.push_back(matCol[2]);
        ++mesh.tris;
    }
    if (mesh.tris <= 0) {
        return 0;
    }
    int got = mesh.tris;
    scene.meshes.push_back(std::move(mesh));
    ++meshIndex;
    return got;
}

// True when the parsed clump carries at least one skinned geometry.
bool ClumpHasSkin(rw::Clump* clump) {
    if (!clump) {
        return false;
    }
    FORLIST(link, clump->atomics) {
        rw::Atomic* atomic = rw::Atomic::fromClump(link);
        rw::Geometry* geo = atomic ? atomic->geometry : nil;
        if (geo && rw::Skin::get(geo)) {
            return true;
        }
    }
    return false;
}

std::vector<rw::TexDictionary*> s_txds;

} // namespace

bool SkinPed_Init(const char* gameDir, const char* model, WorldShotScene& scene, SkinPedStats& stats,
                  char* err, std::size_t errSize) {
    stats = SkinPedStats{};
    scene.meshes.clear();
    scene.images.clear();
    s_haveVert = false;
    if (!gameDir || !gameDir[0]) {
        SetErr(err, errSize, "no game dir");
        return false;
    }
    std::string want = model && model[0] ? model : "cj";
    (void)std::snprintf(stats.requested, sizeof(stats.requested), "%s", want.c_str());
    std::string wantLower = want;
    ToLowerInPlace(wantLower);
    std::string game(gameDir);
    OS_SetFilePathOffset(game.c_str());
    s_gameAbs = game;

    static const char* kImgs[] = { "models/player.img", "models/gta3.img", "models/gta_int.img" };
    std::vector<ImgIndex> imgs;
    for (const char* rel : kImgs) {
        ImgIndex idx;
        if (BuildImgIndex(rel, idx)) {
            imgs.push_back(std::move(idx));
        }
    }
    if (imgs.empty()) {
        SetErr(err, errSize, "no IMG archive indexed (models/*.img)");
        return false;
    }
    if (!RwInitEngine()) {
        SetErr(err, errSize, "librw Engine::init failed");
        return false;
    }
    for (rw::TexDictionary* t : s_txds) {
        if (t) {
            t->destroy();
        }
    }
    s_txds.clear();

    // --- 1. Direct lookup: <model>.dff in player.img, then gta3/gta_int. ---
    std::vector<uint8> dffBytes;
    std::string resolved = wantLower;
    std::string dffFile = wantLower + ".dff";
    const ImgIndex* hitImg = nil;
    for (const ImgIndex& idx : imgs) {
        if (ImgReadBytesStd(idx, dffFile, dffBytes)) {
            hitImg = &idx;
            break;
        }
    }
    if (dffBytes.empty()) {
        // --- 2. Fallback: first skinned DFF in gta3.img archive order. ---
        // cj.dff does not ship (CJ is torso/head/legs parts in player.img);
        // the round spec names this contingency explicitly. Only
        // character-sized entries (<=128 sectors = 256KiB; peds are ~40-50)
        // are parsed so the scan stays a seconds-long bootstrap, not a full
        // 900MiB reparse; the cap is logged, the pick is archive-ordered and
        // deterministic.
        const ImgIndex* gta3 = nil;
        for (const ImgIndex& idx : imgs) {
            if (idx.rel == "models/gta3.img") {
                gta3 = &idx;
                break;
            }
        }
        if (!gta3) {
            SetErr(err, errSize, "gta3.img not indexed for ped fallback scan");
            return false;
        }
        int tried = 0;
        std::string pickLocal;
        for (const ImgEntry& e : gta3->entries) {
            if (e.nameLower.size() < 5 ||
                e.nameLower.compare(e.nameLower.size() - 4, 4, ".dff") != 0) {
                continue;
            }
            if (e.size == 0 || e.size > 128) {
                continue;
            }
            std::vector<uint8> cand;
            if (!ImgReadBytesStd(*gta3, e.nameLower, cand)) {
                continue;
            }
            ++tried;
            LinkedClump lc = TexSample_LinkedParse(cand.data(), cand.size(), nil, nil, 0);
            bool skinned = ClumpHasSkin(lc.clump);
            TexSample_FreeLinked(lc);
            if (skinned) {
                dffBytes = std::move(cand);
                hitImg = gta3;
                resolved = e.nameLower.substr(0, e.nameLower.size() - 4);
                dffFile = e.nameLower;
                pickLocal = e.name;
                break;
            }
        }
        stats.tried = tried;
        if (dffBytes.empty()) {
            char msg[256];
            (void)std::snprintf(msg, sizeof(msg),
                                "ped fallback scan found no skinned DFF in gta3.img (tried=%d)",
                                tried);
            SetErr(err, errSize, msg);
            return false;
        }
        (void)std::snprintf(stats.src, sizeof(stats.src), "%s:%s", hitImg->label.c_str(),
                            pickLocal.c_str());
    } else {
        // Exact entry casing for the log.
        for (const ImgEntry& e : hitImg->entries) {
            if (e.nameLower == dffFile) {
                (void)std::snprintf(stats.src, sizeof(stats.src), "%s:%s", hitImg->label.c_str(),
                                    e.name.c_str());
                break;
            }
        }
    }
    (void)std::snprintf(stats.model, sizeof(stats.model), "%s", resolved.c_str());

    // --- 3. Per-model TXD: <base>.txd next to the DFF, then generics. ---
    std::string txdFile = resolved + ".txd";
    std::vector<rw::TexDictionary*> dicts;
    {
        std::vector<uint8> txdBytes;
        if (hitImg && ImgReadBytesStd(*hitImg, txdFile, txdBytes)) {
            rw::TexDictionary* txd = ParseTxd(txdBytes);
            if (txd && txd->count() > 0) {
                dicts.push_back(txd);
                s_txds.push_back(txd);
                stats.textures = txd->count();
                (void)std::snprintf(stats.txd, sizeof(stats.txd), "%s:%s", hitImg->label.c_str(),
                                    txdFile.c_str());
            } else {
                if (txd) {
                    txd->destroy();
                }
            }
        }
        if (dicts.empty()) {
            (void)std::snprintf(stats.txd, sizeof(stats.txd), "%s", "none");
        }
    }

    // --- 4. Parse with honest material linkage + bind-pose skinning. ---
    rw::TexDictionary* primary = dicts.empty() ? nil : dicts[0];
    LinkedClump lc = TexSample_LinkedParse(dffBytes.data(), dffBytes.size(), primary, nil, 0);
    if (!lc.clump) {
        TexSample_FreeLinked(lc);
        SetErr(err, errSize, "DFF parse produced no clump (not a RenderWare clump?)");
        return false;
    }
    if (!ClumpHasSkin(lc.clump)) {
        TexSample_FreeLinked(lc);
        char msg[192];
        (void)std::snprintf(msg, sizeof(msg), "DFF '%s' has no skinned geometry", stats.src);
        SetErr(err, errSize, msg);
        return false;
    }
    rw::Frame* root = lc.clump->getFrame();
    stats.frames = root ? root->count() : 0;

    int meshIndex = 0;
    bool first = true;
    int totalTris = 0;
    int skinGeoms = 0;
    int bones = 0;
    int attached = 0;
    double wsumAcc = 0.0;
    long wsumVerts = 0;
    double boneDevAcc = 0.0;
    long boneDevSamples = 0;
    int serial = 0;
    FORLIST(link, lc.clump->atomics) {
        rw::Atomic* atomic = rw::Atomic::fromClump(link);
        rw::Geometry* geo = atomic ? atomic->geometry : nil;
        if (!geo) {
            continue;
        }
        if (rw::Skin::get(geo)) {
            int b = 0;
            int a = 0;
            int got = FlattenSkinned(lc.clump, atomic, lc, scene, meshIndex, first, wsumAcc,
                                     wsumVerts, boneDevAcc, boneDevSamples, b, a, serial++);
            if (got > 0) {
                totalTris += got;
                ++skinGeoms;
                bones += b;
                attached += a;
            }
        } else {
            int got = FlattenStatic(atomic, lc, scene, meshIndex, first);
            if (got > 0) {
                totalTris += got;
            }
        }
    }
    TexSample_FreeLinked(lc);
    if (totalTris <= 0 || skinGeoms <= 0) {
        char msg[192];
        (void)std::snprintf(msg, sizeof(msg), "no skinned triangles flattened from '%.127s'",
                            stats.src);
        SetErr(err, errSize, msg);
        return false;
    }
    stats.tris = totalTris;
    stats.verts = totalTris * 3;
    stats.bones = bones;
    stats.geoms = skinGeoms;
    stats.attached = attached;
    stats.wsum = wsumVerts > 0 ? wsumAcc / wsumVerts : 0.0;
    stats.binddev = boneDevSamples > 0 ? boneDevAcc / boneDevSamples : -1.0;
    (void)std::snprintf(scene.stats.dffName, sizeof(scene.stats.dffName), "%.127s", stats.src);
    (void)std::snprintf(scene.stats.txdName, sizeof(scene.stats.txdName), "%.127s", stats.txd);
    scene.stats.atomics = meshIndex;
    scene.stats.triangles = totalTris;
    scene.stats.vertices = totalTris * 3;
    scene.stats.textures = stats.textures;
    scene.stats.firstTexture[0] = '\0';
    scene.stats.firstTexW = 0;
    scene.stats.firstTexH = 0;
    if (!scene.images.empty()) {
        (void)std::snprintf(scene.stats.firstTexture, sizeof(scene.stats.firstTexture), "%s",
                            scene.images[0].name);
        scene.stats.firstTexW = scene.images[0].w;
        scene.stats.firstTexH = scene.images[0].h;
    }
    return true;
}

bool SkinPed_SampleVert(SkinPedVert& out) {
    if (!s_haveVert) {
        return false;
    }
    out = s_vert;
    return true;
}

// R6v cutscene close-up (round 24). Same bind-pose path as SkinPed_Init,
// CS archives only. See SkinPed.h for the contract.
bool SkinPed_InitCs(const char* gameDir, const char* model, WorldShotScene& scene, SkinPedStats& stats,
                    char* err, std::size_t errSize) {
    stats = SkinPedStats{};
    scene.meshes.clear();
    scene.images.clear();
    s_haveVert = false;
    if (!gameDir || !gameDir[0]) {
        SetErr(err, errSize, "no game dir");
        return false;
    }
    std::string want = model ? model : "auto";
    if (want.empty()) {
        want = "auto";
    }
    const bool autoScan = (StrCaseCmp(want.c_str(), "auto") == 0);
    (void)std::snprintf(stats.requested, sizeof(stats.requested), "%s", want.c_str());
    std::string wantLower = want;
    ToLowerInPlace(wantLower);
    std::string game(gameDir);
    OS_SetFilePathOffset(game.c_str());
    s_gameAbs = game;

    // CS archives only: cuts.img first (round spec order), then the
    // archive that actually ships the CS DFFs. A low-poly gta3/player
    // name can never resolve here by design (anti-stand-in gate).
    static const char* kCsImgs[] = { "anim/cuts.img", "models/cutscene.img" };
    std::vector<ImgIndex> imgs;
    for (const char* rel : kCsImgs) {
        ImgIndex idx;
        if (BuildImgIndex(rel, idx)) {
            imgs.push_back(std::move(idx));
        }
    }
    if (imgs.empty()) {
        SetErr(err, errSize, "no CS IMG archive indexed (anim/cuts.img, models/cutscene.img)");
        return false;
    }
    const ImgIndex* cuts = nil;
    const ImgIndex* cs = nil;
    for (const ImgIndex& idx : imgs) {
        if (idx.rel == "anim/cuts.img") {
            cuts = &idx;
        } else if (idx.rel == "models/cutscene.img") {
            cs = &idx;
        }
    }
    // Archive composition accounting (spec-premise check, logged verbatim).
    if (cuts) {
        stats.cutsEntries = static_cast<int>(cuts->entries.size());
        for (const ImgEntry& e : cuts->entries) {
            if (e.nameLower.size() >= 5 &&
                e.nameLower.compare(e.nameLower.size() - 4, 4, ".dff") == 0) {
                ++stats.cutsDffs;
            }
        }
    }
    if (cs) {
        stats.csEntries = static_cast<int>(cs->entries.size());
        for (const ImgEntry& e : cs->entries) {
            if (e.nameLower.size() >= 5 &&
                e.nameLower.compare(e.nameLower.size() - 4, 4, ".dff") == 0) {
                ++stats.csDffs;
            }
        }
    }
    if (!RwInitEngine()) {
        SetErr(err, errSize, "librw Engine::init failed");
        return false;
    }
    for (rw::TexDictionary* t : s_txds) {
        if (t) {
            t->destroy();
        }
    }
    s_txds.clear();

    std::vector<uint8> dffBytes;
    std::string resolved;
    std::string dffFile;
    const ImgIndex* hitImg = nil;
    std::string pickLocal;
    if (!autoScan) {
        // --- Direct hit: <model>.dff in cuts.img, then cutscene.img. ---
        dffFile = wantLower + ".dff";
        for (const ImgIndex& idx : imgs) {
            if (ImgReadBytesStd(idx, dffFile, dffBytes)) {
                hitImg = &idx;
                resolved = wantLower;
                break;
            }
        }
        if (dffBytes.empty()) {
            char msg[256];
            (void)std::snprintf(msg, sizeof(msg),
                                "CS model '%s' not in cuts.img/cutscene.img (CS-only; "
                                "low-poly gta3/player stand-ins rejected)",
                                want.c_str());
            SetErr(err, errSize, msg);
            return false;
        }
        for (const ImgEntry& e : hitImg->entries) {
            if (e.nameLower == dffFile) {
                pickLocal = e.name;
                break;
            }
        }
        (void)std::snprintf(stats.csArchive, sizeof(stats.csArchive), "%s",
                            hitImg->label.c_str());
    } else {
        // --- Auto scan: full archive-order pass over cutscene.img. ---
        // cuts.img is still indexed and counted above (cutsDffs==0 is the
        // honest finding), but there is nothing to parse inside it. First
        // skinned DFF with bones>=10 and flattened verts>8000 wins; every
        // earlier skinned candidate is below the hi-poly bar by
        // first-match construction. No size cap: the whole 26MiB archive
        // parses in seconds, so the max below is a proven global max.
        if (!cs) {
            SetErr(err, errSize, "models/cutscene.img not indexed for CS scan");
            return false;
        }
        int tried = 0;
        int skinned = 0;
        int skippedLo = 0;
        int passed = 0;
        int pickIdx = -1;
        int maxVerts = 0;
        std::string maxName;
        std::string firstPick;
        std::string firstPickLocal;
        for (size_t ei = 0; ei < cs->entries.size(); ++ei) {
            const ImgEntry& e = cs->entries[ei];
            if (e.nameLower.size() < 5 ||
                e.nameLower.compare(e.nameLower.size() - 4, 4, ".dff") != 0) {
                continue;
            }
            if (e.size == 0) {
                continue;
            }
            std::vector<uint8> cand;
            if (!ImgReadBytesStd(*cs, e.nameLower, cand)) {
                continue;
            }
            ++tried;
            LinkedClump lc = TexSample_LinkedParse(cand.data(), cand.size(), nil, nil, 0);
            int skinTris = 0;
            int skinBones = 0;
            if (lc.clump) {
                FORLIST(link, lc.clump->atomics) {
                    rw::Atomic* atomic = rw::Atomic::fromClump(link);
                    rw::Geometry* geo = atomic ? atomic->geometry : nil;
                    rw::Skin* sk = geo ? rw::Skin::get(geo) : nil;
                    if (sk && sk->numBones > 0 && sk->indices && sk->weights &&
                        sk->inverseMatrices && geo->numTriangles > 0) {
                        skinTris += geo->numTriangles;
                        if (sk->numBones > skinBones) {
                            skinBones = sk->numBones;
                        }
                    }
                }
            }
            const bool isSkinned = skinTris > 0 && skinBones > 0;
            TexSample_FreeLinked(lc);
            if (!isSkinned) {
                continue;
            }
            ++skinned;
            const int flatVerts = skinTris * 3;
            if (flatVerts > maxVerts) {
                maxVerts = flatVerts;
                maxName = e.nameLower.substr(0, e.nameLower.size() - 4);
            }
            if (skinBones >= 10 && flatVerts > 8000) {
                ++passed;
                if (pickIdx < 0) {
                    pickIdx = static_cast<int>(ei);
                    dffBytes = std::move(cand);
                    hitImg = cs;
                    resolved = e.nameLower.substr(0, e.nameLower.size() - 4);
                    dffFile = e.nameLower;
                    firstPick = resolved;
                    firstPickLocal = e.name;
                }
            } else {
                ++skippedLo;
            }
        }
        stats.csTried = tried;
        stats.csSkinned = skinned;
        stats.csSkippedLo = skippedLo;
        stats.csPassed = passed;
        stats.csIndex = pickIdx;
        stats.csMaxVerts = maxVerts;
        (void)std::snprintf(stats.csMaxModel, sizeof(stats.csMaxModel), "%s",
                            maxName.c_str());
        (void)std::snprintf(stats.csArchive, sizeof(stats.csArchive), "%s",
                            cs ? cs->label.c_str() : "cutscene.img");
        if (dffBytes.empty()) {
            char msg[256];
            (void)std::snprintf(msg, sizeof(msg),
                                "CS scan found no skinned DFF with verts>8000 in cutscene.img "
                                "(tried=%d skinned=%d max=%s verts=%d)",
                                tried, skinned, maxName.c_str(), maxVerts);
            SetErr(err, errSize, msg);
            return false;
        }
        pickLocal = firstPickLocal;
    }
    (void)std::snprintf(stats.model, sizeof(stats.model), "%s", resolved.c_str());
    (void)std::snprintf(stats.src, sizeof(stats.src), "%s:%s", hitImg->label.c_str(),
                        pickLocal.c_str());

    // --- Per-model TXD: <base>.txd next to the DFF in the same archive. ---
    std::string txdFile = resolved + ".txd";
    std::vector<rw::TexDictionary*> dicts;
    {
        std::vector<uint8> txdBytes;
        if (hitImg && ImgReadBytesStd(*hitImg, txdFile, txdBytes)) {
            rw::TexDictionary* txd = ParseTxd(txdBytes);
            if (txd && txd->count() > 0) {
                dicts.push_back(txd);
                s_txds.push_back(txd);
                stats.textures = txd->count();
                (void)std::snprintf(stats.txd, sizeof(stats.txd), "%s:%s", hitImg->label.c_str(),
                                    txdFile.c_str());
            } else {
                if (txd) {
                    txd->destroy();
                }
            }
        }
        if (dicts.empty()) {
            (void)std::snprintf(stats.txd, sizeof(stats.txd), "%s", "none");
        }
    }

    // --- Parse with honest material linkage + bind-pose skinning. ---
    // Same tail as SkinPed_Init (file pose, anim=bind, no HAnim sampling).
    rw::TexDictionary* primary = dicts.empty() ? nil : dicts[0];
    LinkedClump lc = TexSample_LinkedParse(dffBytes.data(), dffBytes.size(), primary, nil, 0);
    if (!lc.clump) {
        TexSample_FreeLinked(lc);
        SetErr(err, errSize, "DFF parse produced no clump (not a RenderWare clump?)");
        return false;
    }
    if (!ClumpHasSkin(lc.clump)) {
        TexSample_FreeLinked(lc);
        char msg[192];
        (void)std::snprintf(msg, sizeof(msg), "DFF '%s' has no skinned geometry", stats.src);
        SetErr(err, errSize, msg);
        return false;
    }
    rw::Frame* root = lc.clump->getFrame();
    stats.frames = root ? root->count() : 0;

    int meshIndex = 0;
    bool first = true;
    int totalTris = 0;
    int skinGeoms = 0;
    int bones = 0;
    int attached = 0;
    double wsumAcc = 0.0;
    long wsumVerts = 0;
    double boneDevAcc = 0.0;
    long boneDevSamples = 0;
    int serial = 0;
    FORLIST(link, lc.clump->atomics) {
        rw::Atomic* atomic = rw::Atomic::fromClump(link);
        rw::Geometry* geo = atomic ? atomic->geometry : nil;
        if (!geo) {
            continue;
        }
        if (rw::Skin::get(geo)) {
            int b = 0;
            int a = 0;
            int got = FlattenSkinned(lc.clump, atomic, lc, scene, meshIndex, first, wsumAcc,
                                     wsumVerts, boneDevAcc, boneDevSamples, b, a, serial++);
            if (got > 0) {
                totalTris += got;
                ++skinGeoms;
                bones += b;
                attached += a;
            }
        } else {
            int got = FlattenStatic(atomic, lc, scene, meshIndex, first);
            if (got > 0) {
                totalTris += got;
            }
        }
    }
    TexSample_FreeLinked(lc);
    if (totalTris <= 0 || skinGeoms <= 0) {
        char msg[192];
        (void)std::snprintf(msg, sizeof(msg), "no skinned triangles flattened from '%.127s'",
                            stats.src);
        SetErr(err, errSize, msg);
        return false;
    }
    stats.tris = totalTris;
    stats.verts = totalTris * 3;
    stats.bones = bones;
    stats.geoms = skinGeoms;
    stats.attached = attached;
    stats.wsum = wsumVerts > 0 ? wsumAcc / wsumVerts : 0.0;
    stats.binddev = boneDevSamples > 0 ? boneDevAcc / boneDevSamples : -1.0;
    (void)std::snprintf(scene.stats.dffName, sizeof(scene.stats.dffName), "%.127s", stats.src);
    (void)std::snprintf(scene.stats.txdName, sizeof(scene.stats.txdName), "%.127s", stats.txd);
    scene.stats.atomics = meshIndex;
    scene.stats.triangles = totalTris;
    scene.stats.vertices = totalTris * 3;
    scene.stats.textures = stats.textures;
    scene.stats.firstTexture[0] = '\0';
    scene.stats.firstTexW = 0;
    scene.stats.firstTexH = 0;
    if (!scene.images.empty()) {
        (void)std::snprintf(scene.stats.firstTexture, sizeof(scene.stats.firstTexture), "%s",
                            scene.images[0].name);
        scene.stats.firstTexW = scene.images[0].w;
        scene.stats.firstTexH = scene.images[0].h;
    }
    return true;
}

void SkinPed_Shutdown() {
    for (rw::TexDictionary* t : s_txds) {
        if (t) {
            t->destroy();
        }
    }
    s_txds.clear();
}
