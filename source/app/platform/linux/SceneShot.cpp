// SceneShot implementation: IDE/IPL parsing + multi-IMG DFF/TXD loading.
// See SceneShot.h for the contract. This TU owns its own librw engine handle
// (mirrors WorldShot's plugin set); only one of the two shot paths runs per
// process, so there is no double Engine::init. Shared logic is deliberately
// duplicated rather than refactored out of WorldShot.cpp: the native-tracks
// change envelope for this round allows new SceneShot.* files, not refactors
// of the already-verified R5 slice.

#include "app/platform/linux/SceneShot.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <map>
#include <set>

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

#include <dirent.h>

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

// gta.dat ships DOS-style paths (DATA\MAPS\LA\LAn.IDE) while the Linux
// checkout has its own case (data/maps/LA/LAn.ide). Resolve each component
// case-insensitively so the parse follows the same file list as the game.
bool ResolveGamePath(const std::string& gameDir, const std::string& rel, std::string& out) {
    std::string norm = rel;
    for (char& c : norm) {
        if (c == '\\') {
            c = '/';
        }
    }
    std::string cur = gameDir;
    size_t pos = 0;
    while (pos < norm.size()) {
        while (pos < norm.size() && norm[pos] == '/') {
            ++pos;
        }
        if (pos >= norm.size()) {
            break;
        }
        size_t end = norm.find('/', pos);
        std::string want = norm.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        pos = end == std::string::npos ? norm.size() : end + 1;
        if (want.empty() || want == ".") {
            continue;
        }
        DIR* dir = opendir(cur.c_str());
        if (!dir) {
            return false;
        }
        bool found = false;
        std::string real;
        for (dirent* ent = readdir(dir); ent; ent = readdir(dir)) {
            if (StrCaseCmp(ent->d_name, want.c_str()) == 0) {
                real = ent->d_name;
                found = true;
                break;
            }
        }
        closedir(dir);
        if (!found) {
            return false;
        }
        cur += "/";
        cur += real;
    }
    out = cur;
    return true;
}

bool ReadResolvedFile(const std::string& absPath, std::vector<uint8>& out) {
    FILE* f = std::fopen(absPath.c_str(), "rb");
    if (!f) {
        return false;
    }
    (void)std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    (void)std::fseek(f, 0, SEEK_SET);
    if (size < 0) {
        (void)std::fclose(f);
        return false;
    }
    out.resize(static_cast<size_t>(size));
    bool ok = true;
    if (size > 0) {
        ok = std::fread(out.data(), 1, static_cast<size_t>(size), f) == static_cast<size_t>(size);
    }
    (void)std::fclose(f);
    return ok;
}

bool ReadGameText(const std::string& gameDir, const std::string& rel, std::string& out) {
    std::string abs;
    if (!ResolveGamePath(gameDir, rel, abs)) {
        return false;
    }
    std::vector<uint8> bytes;
    if (!ReadResolvedFile(abs, bytes)) {
        return false;
    }
    out.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    return true;
}

std::string Trim(const std::string& s) {
    size_t b = 0;
    while (b < s.size() && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r')) {
        ++b;
    }
    size_t e = s.size();
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r')) {
        --e;
    }
    return s.substr(b, e - b);
}

// game_sa CFileLoader::LoadLine sanitization: commas and control chars act
// as whitespace before sscanf.
void SanitizeLine(std::string& line) {
    for (char& c : line) {
        if (static_cast<unsigned char>(c) < 32 || c == ',') {
            c = ' ';
        }
    }
}

bool StartsWithNoCase(const std::string& line, const char* prefix) {
    size_t n = std::strlen(prefix);
    if (line.size() < n) {
        return false;
    }
    for (size_t i = 0; i < n; ++i) {
        char a = line[i];
        char b = prefix[i];
        if (a >= 'A' && a <= 'Z') {
            a = static_cast<char>(a + 32);
        }
        if (b >= 'A' && b <= 'Z') {
            b = static_cast<char>(b + 32);
        }
        if (a != b) {
            return false;
        }
    }
    return line.size() == n || line[n] == ' ' || line[n] == '\t';
}

void CollectDatLists(const std::string& text, std::vector<std::string>& ide,
                     std::vector<std::string>& ipl) {
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t end = text.find('\n', pos);
        std::string line = Trim(text.substr(pos, end == std::string::npos ? std::string::npos : end - pos));
        pos = end == std::string::npos ? text.size() + 1 : end + 1;
        if (line.empty() || line[0] == '#') {
            continue;
        }
        const char* kind = nullptr;
        if (StartsWithNoCase(line, "IDE")) {
            kind = "IDE";
        } else if (StartsWithNoCase(line, "IPL")) {
            kind = "IPL";
        }
        if (!kind) {
            continue;
        }
        std::string after = Trim(line.substr(3));
        if (after.empty()) {
            continue;
        }
        size_t sp = after.find_first_of(" \t");
        std::string path = after.substr(0, sp);
        if (path.empty()) {
            continue;
        }
        if (kind[0] == 'E') {
            ide.push_back(path);
        } else {
            ipl.push_back(path);
        }
    }
}

struct IdeEntry {
    std::string model;
    std::string txd;
};

void ParseIdeText(const std::string& text, std::map<std::string, IdeEntry>& out,
                  std::set<std::string>& animModels) {
    // Sections of interest: objs/tobj = static map objects (id, model, txd,
    // ...). anim = animated (excluded from the static scene). Everything else
    // (cars/peds/weap/path/2dfx/txdp/...) is not world geometry.
    int mode = 0; // 0 none, 1 static, 2 anim
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t end = text.find('\n', pos);
        std::string raw = Trim(text.substr(pos, end == std::string::npos ? std::string::npos : end - pos));
        pos = end == std::string::npos ? text.size() + 1 : end + 1;
        if (raw.empty() || raw[0] == '#') {
            continue;
        }
        std::string low = raw;
        ToLowerInPlace(low);
        // Section headers are bare words (possibly with trailing spaces).
        std::string head = Trim(low);
        if (head == "end") {
            mode = 0;
            continue;
        }
        if (head == "objs" || head == "tobj") {
            mode = 1;
            continue;
        }
        if (head == "anim") {
            mode = 2;
            continue;
        }
        if (mode == 0) {
            continue;
        }
        std::string line = raw;
        SanitizeLine(line);
        int id = -1;
        char model[64] = {};
        char txd[64] = {};
        // First three fields are shared by objs/tobj/anim row flavors.
        if (std::sscanf(line.c_str(), "%d %63s %63s", &id, model, txd) != 3 || id < 0) {
            continue;
        }
        std::string key = model;
        ToLowerInPlace(key);
        if (mode == 2) {
            animModels.insert(key);
            continue;
        }
        if (out.find(key) == out.end()) {
            out[key] = IdeEntry{ model, txd };
        }
    }
}

struct IplInst {
    std::string model;
    int interior = 0;
    float pos[3] = {};
    float quat[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    int lod = -1;
};

// IPL inst rows (game_sa CFileLoader::LoadObjectInstance): id, model,
// interior, pos(3), quaternion(4: qx qy qz qw), lod index.
void ParseIplText(const std::string& text, std::vector<IplInst>& out) {
    bool inInst = false;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t end = text.find('\n', pos);
        std::string raw = Trim(text.substr(pos, end == std::string::npos ? std::string::npos : end - pos));
        pos = end == std::string::npos ? text.size() + 1 : end + 1;
        if (raw.empty() || raw[0] == '#') {
            continue;
        }
        std::string low = Trim(raw);
        ToLowerInPlace(low);
        if (low == "inst") {
            inInst = true;
            continue;
        }
        if (low == "end") {
            inInst = false;
            continue;
        }
        if (!inInst) {
            continue;
        }
        std::string line = raw;
        SanitizeLine(line);
        int id = -1;
        char model[64] = {};
        IplInst inst;
        float qx = 0, qy = 0, qz = 0, qw = 1;
        int n = std::sscanf(line.c_str(), "%d %63s %d %f %f %f %f %f %f %f %d", &id, model,
                            &inst.interior, &inst.pos[0], &inst.pos[1], &inst.pos[2], &qx, &qy, &qz,
                            &qw, &inst.lod);
        if (n != 11 || id < 0) {
            continue;
        }
        inst.model = model;
        inst.quat[0] = qx;
        inst.quat[1] = qy;
        inst.quat[2] = qz;
        inst.quat[3] = qw;
        out.push_back(inst);
    }
}

struct ImgEntry {
    std::string nameLower;
    uint32 off = 0; // 2048-byte sectors
    uint32 size = 0; // sectors (streaming flag bits masked out)
};

struct ImgIndex {
    std::string absPath;
    std::vector<ImgEntry> entries;
};

bool BuildImgIndex(const std::string& absPath, ImgIndex& idx) {
    FILE* f = std::fopen(absPath.c_str(), "rb");
    if (!f) {
        return false;
    }
    char magic[4] = {};
    uint32 count = 0;
    bool ok = std::fread(magic, 1, 4, f) == 4 && std::fread(&count, 4, 1, f) == 1;
    if (!ok || std::memcmp(magic, "VER2", 4) != 0 || count == 0 || count > 300000) {
        (void)std::fclose(f);
        return false;
    }
    idx.absPath = absPath;
    idx.entries.reserve(count);
    for (uint32 i = 0; i < count; ++i) {
        uint32 off = 0;
        uint32 size = 0;
        char name[24] = {};
        if (std::fread(&off, 4, 1, f) != 1 || std::fread(&size, 4, 1, f) != 1 ||
            std::fread(name, 1, 24, f) != 24) {
            (void)std::fclose(f);
            return false;
        }
        name[23] = '\0';
        ImgEntry e;
        e.nameLower = name;
        ToLowerInPlace(e.nameLower);
        e.off = off;
        e.size = size & 0x7FFFu; // high bits carry streaming flags, not size
        idx.entries.push_back(e);
    }
    (void)std::fclose(f);
    return true;
}

bool ImgReadBytes(const ImgIndex& idx, const std::string& wantLower, std::vector<uint8>& out) {
    for (const ImgEntry& e : idx.entries) {
        if (e.nameLower != wantLower || e.size == 0) {
            continue;
        }
        FILE* f = std::fopen(idx.absPath.c_str(), "rb");
        if (!f) {
            return false;
        }
        bool ok = fseeko(f, static_cast<off_t>(e.off) * 2048, SEEK_SET) == 0;
        out.resize(static_cast<size_t>(e.size) * 2048u);
        if (ok && e.size > 0) {
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

rw::Clump* ParseClump(const std::vector<uint8>& bytes) {
    if (bytes.size() < 12) {
        return nil;
    }
    rw::StreamMemory stream;
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

// Muted SA-ish palette so instances stay distinguishable without textures.
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

struct CachedModel {
    // Object-space flat triangle soup (LTM applied, instance pending).
    std::vector<float> pos;
    std::vector<float> nrm;
    int tris = 0;
};

// Flattens every atomic of the clump to object space. Returns false for
// skinned geometry (not a static world object) or zero triangles.
bool FlattenClumpStatic(rw::Clump* clump, CachedModel& out) {
    out.pos.clear();
    out.nrm.clear();
    out.tris = 0;
    FORLIST(link, clump->atomics) {
        rw::Atomic* atomic = rw::Atomic::fromClump(link);
        rw::Geometry* geo = atomic ? atomic->geometry : nil;
        if (!geo || geo->numTriangles <= 0 || geo->numVertices <= 0) {
            continue;
        }
        if (!geo->triangles || !geo->morphTargets || !geo->morphTargets[0].vertices) {
            continue; // native-only geometry: nothing CPU-readable to draw
        }
        if (rw::Skin::get(geo)) {
            return false; // skinned: animated, not static world geometry
        }
        const int numVerts = geo->numVertices;
        rw::V3d* verts = geo->morphTargets[0].vertices;
        rw::V3d* norms = (geo->flags & rw::Geometry::NORMALS) ? geo->morphTargets[0].normals : nil;
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
        size_t base = out.pos.size();
        out.pos.resize(base + static_cast<size_t>(geo->numTriangles) * 9);
        out.nrm.resize(base + static_cast<size_t>(geo->numTriangles) * 9);
        size_t w = base;
        int kept = 0;
        for (int t = 0; t < geo->numTriangles; ++t) {
            const rw::Triangle& tri = geo->triangles[t];
            if (tri.v[0] >= numVerts || tri.v[1] >= numVerts || tri.v[2] >= numVerts) {
                continue;
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
                out.pos[w] = p[k]->x;
                out.pos[w + 1] = p[k]->y;
                out.pos[w + 2] = p[k]->z;
                if (norms) {
                    const rw::V3d& n = objNorms[tri.v[k]];
                    float len = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
                    if (len > 1e-9f) {
                        out.nrm[w] = n.x / len;
                        out.nrm[w + 1] = n.y / len;
                        out.nrm[w + 2] = n.z / len;
                    } else {
                        out.nrm[w] = face[0];
                        out.nrm[w + 1] = face[1];
                        out.nrm[w + 2] = face[2];
                    }
                } else {
                    out.nrm[w] = face[0];
                    out.nrm[w + 1] = face[1];
                    out.nrm[w + 2] = face[2];
                }
                w += 3;
            }
            ++kept;
        }
        out.pos.resize(base + static_cast<size_t>(kept) * 9);
        out.nrm.resize(base + static_cast<size_t>(kept) * 9);
        out.tris += kept;
    }
    return out.tris > 0;
}

// game_sa CMatrix::SetRotate(quat): basis vectors of the instance rotation.
void QuatToBasis(const float* q, float* right, float* fwd, float* up) {
    float qx = q[0];
    float qy = q[1];
    float qz = q[2];
    float qw = q[3];
    float len = std::sqrt(qx * qx + qy * qy + qz * qz + qw * qw);
    if (len > 1e-9f) {
        qx /= len;
        qy /= len;
        qz /= len;
        qw /= len;
    } else {
        qx = qy = qz = 0.0f;
        qw = 1.0f;
    }
    float x2 = qx + qx;
    float y2 = qy + qy;
    float z2 = qz + qz;
    float x2x = x2 * qx;
    float y2x = y2 * qx;
    float z2x = z2 * qx;
    float y2y = y2 * qy;
    float z2y = z2 * qy;
    float z2z = z2 * qz;
    float x2r = x2 * qw;
    float y2r = y2 * qw;
    float z2r = z2 * qw;
    right[0] = 1.0f - (z2z + y2y);
    right[1] = z2r + y2x;
    right[2] = z2x - y2r;
    fwd[0] = y2x - z2r;
    fwd[1] = 1.0f - (z2z + x2x);
    fwd[2] = x2r + z2y;
    up[0] = y2r + z2x;
    up[1] = z2y - x2r;
    up[2] = 1.0f - (y2y + x2x);
}

bool StartsWithLod(const std::string& model) {
    return model.size() >= 3 && (model[0] == 'l' || model[0] == 'L') &&
           (model[1] == 'o' || model[1] == 'O') && (model[2] == 'd' || model[2] == 'D');
}

std::map<std::string, rw::TexDictionary*> s_txds;
std::vector<rw::TexDictionary*> s_txdOrder;

} // namespace

bool SceneShot_Init(const char* gameDir, WorldShotScene& scene, SceneShotStats& stats, char* err,
                    std::size_t errSize) {
    stats = SceneShotStats{};
    scene.meshes.clear();
    if (!gameDir || !gameDir[0]) {
        SetErr(err, errSize, "no game dir");
        return false;
    }
    std::string game(gameDir);
    OS_SetFilePathOffset(game.c_str());

    // --- 1. DAT file lists (same files R3 counts: ide=54 ipl=52). ---
    std::string gtaDat;
    if (!ReadGameText(game, "data/gta.dat", gtaDat)) {
        SetErr(err, errSize, "cannot read data/gta.dat");
        return false;
    }
    std::string defaultDat;
    if (!ReadGameText(game, "data/default.dat", defaultDat)) {
        SetErr(err, errSize, "cannot read data/default.dat");
        return false;
    }
    std::vector<std::string> idePaths;
    std::vector<std::string> iplPaths;
    CollectDatLists(gtaDat, idePaths, iplPaths);
    {
        std::vector<std::string> ide2;
        std::vector<std::string> ipl2;
        CollectDatLists(defaultDat, ide2, ipl2);
        idePaths.insert(idePaths.end(), ide2.begin(), ide2.end());
        iplPaths.insert(iplPaths.end(), ipl2.begin(), ipl2.end());
    }
    if (idePaths.empty() || iplPaths.empty()) {
        SetErr(err, errSize, "no IDE/IPL entries in data/*.dat");
        return false;
    }

    // --- 2. IDE dictionary: model -> txd (+ anim exclusion set). ---
    std::map<std::string, IdeEntry> ide;
    std::set<std::string> animModels;
    int ideFiles = 0;
    for (const std::string& rel : idePaths) {
        std::string text;
        if (!ReadGameText(game, rel, text)) {
            continue;
        }
        ParseIdeText(text, ide, animModels);
        ++ideFiles;
    }
    if (ide.empty()) {
        SetErr(err, errSize, "no IDE model entries parsed");
        return false;
    }

    // --- 3. IPL instances in DAT order (deterministic). ---
    std::vector<IplInst> insts;
    int iplFiles = 0;
    for (const std::string& rel : iplPaths) {
        std::string text;
        if (!ReadGameText(game, rel, text)) {
            continue;
        }
        size_t before = insts.size();
        ParseIplText(text, insts);
        if (insts.size() > before) {
            ++iplFiles;
        }
        if (insts.size() > 20000) {
            break; // whole map would do; a bounded prefix is enough for K models
        }
    }
    if (insts.empty()) {
        SetErr(err, errSize, "no IPL inst entries parsed");
        return false;
    }

    // --- 4. IMG indices (world DFF/TXD search across archives). ---
    std::vector<ImgIndex> imgs;
    static const char* kImgRels[] = { "models/gta3.img", "models/gta_int.img", "models/player.img" };
    for (const char* rel : kImgRels) {
        std::string abs;
        if (!ResolveGamePath(game, rel, abs)) {
            continue;
        }
        ImgIndex idx;
        if (BuildImgIndex(abs, idx)) {
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
    s_txds.clear();
    s_txdOrder.clear();

    auto findInImgs = [&](const std::string& wantLower, std::vector<uint8>& out) -> bool {
        for (const ImgIndex& idx : imgs) {
            if (ImgReadBytes(idx, wantLower, out)) {
                return true;
            }
        }
        return false;
    };

    // Current-dictionary bootstrap so textured materials parse to nil safely
    // even before the first per-model TXD resolves (same as WorldShot).
    rw::TexDictionary* empty = rw::TexDictionary::create();
    if (empty) {
        rw::TexDictionary::setCurrent(empty);
    }

    // --- 5. Greedy scene fill over IPL order. ---
    const int kMaxModels = 16;
    const int kWantModels = 10;
    const int kWantTris = 24000;
    const size_t kScanCap = 600;
    std::map<std::string, CachedModel> modelCache; // lower name -> mesh (ordered)
    std::set<std::string> failedModels;
    std::set<std::string> usedModels;
    std::string listAcc;
    bool haveBox = false;
    size_t scanned = 0;

    for (const IplInst& inst : insts) {
        if (scanned++ >= kScanCap) {
            break;
        }
        if (stats.models >= kMaxModels ||
            (stats.models >= kWantModels && stats.tris >= kWantTris)) {
            break;
        }
        if (inst.interior != 0) {
            continue; // keep one coherent outdoor scene
        }
        if (StartsWithLod(inst.model)) {
            continue; // low-res LOD shells only
        }
        std::string key = inst.model;
        ToLowerInPlace(key);
        if (usedModels.find(key) != usedModels.end() || failedModels.find(key) != failedModels.end()) {
            continue; // cache: repeated models load once
        }
        if (animModels.find(key) != animModels.end()) {
            failedModels.insert(key);
            continue; // animated, not static
        }

        auto ideIt = ide.find(key);
        std::string txdName = ideIt != ide.end() ? ideIt->second.txd : std::string();

        // Per-model TXD first (R5 rule): DFF materials resolve against it.
        if (!txdName.empty()) {
            std::string txdKey = txdName;
            ToLowerInPlace(txdKey);
            if (s_txds.find(txdKey) == s_txds.end()) {
                std::vector<uint8> txdBytes;
                if (findInImgs(txdKey + ".txd", txdBytes)) {
                    rw::TexDictionary* txd = ParseTxd(txdBytes);
                    if (txd && txd->count() > 0) {
                        s_txds[txdKey] = txd;
                        s_txdOrder.push_back(txd);
                        ++stats.texDicts;
                        stats.textures += txd->count();
                    } else {
                        if (txd) {
                            txd->destroy();
                        }
                        ++stats.missTex;
                        s_txds[txdKey] = nil;
                    }
                } else {
                    ++stats.missTex;
                    s_txds[txdKey] = nil;
                }
            }
            rw::TexDictionary* cur = s_txds[txdKey];
            if (cur) {
                rw::TexDictionary::setCurrent(cur);
            }
        } else {
            ++stats.missTex;
        }

        auto cacheIt = modelCache.find(key);
        if (cacheIt == modelCache.end()) {
            std::vector<uint8> dffBytes;
            if (!findInImgs(key + ".dff", dffBytes)) {
                ++stats.missDff;
                failedModels.insert(key);
                continue;
            }
            rw::Clump* clump = ParseClump(dffBytes);
            if (!clump) {
                ++stats.missDff;
                failedModels.insert(key);
                continue;
            }
            CachedModel cached;
            bool ok = FlattenClumpStatic(clump, cached);
            clump->destroy();
            if (!ok) {
                ++stats.skippedSkin; // skinned or GPU-only: honestly skipped
                failedModels.insert(key);
                continue;
            }
            modelCache[key] = std::move(cached);
            cacheIt = modelCache.find(key);
        }
        const CachedModel& cached = cacheIt->second;

        // Instance transform: CMatrix::SetRotate(quat) basis + IPL position.
        float right[3];
        float fwd[3];
        float up[3];
        QuatToBasis(inst.quat, right, fwd, up);
        WorldShotMesh mesh;
        MeshColor(stats.models, mesh.color);
        mesh.tris = cached.tris;
        size_t count = cached.pos.size();
        mesh.pos.resize(count);
        mesh.nrm.resize(count);
        for (size_t i = 0; i < count; i += 3) {
            float lx = cached.pos[i];
            float ly = cached.pos[i + 1];
            float lz = cached.pos[i + 2];
            float wx = inst.pos[0] + right[0] * lx + fwd[0] * ly + up[0] * lz;
            float wy = inst.pos[1] + right[1] * lx + fwd[1] * ly + up[1] * lz;
            float wz = inst.pos[2] + right[2] * lx + fwd[2] * ly + up[2] * lz;
            mesh.pos[i] = wx;
            mesh.pos[i + 1] = wy;
            mesh.pos[i + 2] = wz;
            float nx = cached.nrm[i];
            float ny = cached.nrm[i + 1];
            float nz = cached.nrm[i + 2];
            float rx = right[0] * nx + fwd[0] * ny + up[0] * nz;
            float ry = right[1] * nx + fwd[1] * ny + up[1] * nz;
            float rz = right[2] * nx + fwd[2] * ny + up[2] * nz;
            float len = std::sqrt(rx * rx + ry * ry + rz * rz);
            if (len > 1e-9f) {
                rx /= len;
                ry /= len;
                rz /= len;
            }
            mesh.nrm[i] = rx;
            mesh.nrm[i + 1] = ry;
            mesh.nrm[i + 2] = rz;
            if (!haveBox) {
                scene.bboxMin[0] = scene.bboxMax[0] = wx;
                scene.bboxMin[1] = scene.bboxMax[1] = wy;
                scene.bboxMin[2] = scene.bboxMax[2] = wz;
                haveBox = true;
            } else {
                if (wx < scene.bboxMin[0]) {
                    scene.bboxMin[0] = wx;
                }
                if (wy < scene.bboxMin[1]) {
                    scene.bboxMin[1] = wy;
                }
                if (wz < scene.bboxMin[2]) {
                    scene.bboxMin[2] = wz;
                }
                if (wx > scene.bboxMax[0]) {
                    scene.bboxMax[0] = wx;
                }
                if (wy > scene.bboxMax[1]) {
                    scene.bboxMax[1] = wy;
                }
                if (wz > scene.bboxMax[2]) {
                    scene.bboxMax[2] = wz;
                }
            }
        }
        scene.meshes.push_back(std::move(mesh));
        usedModels.insert(key);
        ++stats.models;
        stats.tris += cached.tris;
        stats.verts += cached.tris * 3;

        char cell[160];
        (void)std::snprintf(cell, sizeof(cell), "%s@%.2f,%.2f,%.2f;", inst.model.c_str(), inst.pos[0],
                            inst.pos[1], inst.pos[2]);
        if (listAcc.size() + std::strlen(cell) < sizeof(stats.list) - 1) {
            listAcc += cell;
        }
    }

    if (empty) {
        empty->destroy();
    }
    if (stats.models < 8 || stats.tris <= 20000) {
        char msg[256];
        (void)std::snprintf(msg, sizeof(msg),
                            "scene too small: models=%d tris=%d (need >=8, >20000; ide=%d ipl=%d files=%d/%d)",
                            stats.models, stats.tris, (int)ide.size(), (int)insts.size(), ideFiles, iplFiles);
        SetErr(err, errSize, msg);
        return false;
    }
    (void)std::snprintf(stats.list, sizeof(stats.list), "%s", listAcc.c_str());
    (void)std::snprintf(scene.stats.dffName, sizeof(scene.stats.dffName), "scene:%d", stats.models);
    (void)std::snprintf(scene.stats.txdName, sizeof(scene.stats.txdName), "multi:%d", stats.texDicts);
    scene.stats.atomics = stats.models;
    scene.stats.triangles = stats.tris;
    scene.stats.vertices = stats.verts;
    scene.stats.textures = stats.textures;
    scene.stats.firstTexture[0] = '\0';
    scene.stats.firstTexW = 0;
    scene.stats.firstTexH = 0;
    return true;
}

void SceneShot_Shutdown() {
    for (rw::TexDictionary* txd : s_txdOrder) {
        if (txd) {
            txd->destroy();
        }
    }
    s_txdOrder.clear();
    s_txds.clear();
}
