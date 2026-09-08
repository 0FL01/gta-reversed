// ColLoad implementation: COL discovery + chunk parsing + IPL binding.
// See ColLoad.h for the contract. Layout reference (reimplemented here,
// standalone): game_sa/Collision/ColHelpers.h (FileHeader/FileInfo,
// V1/V2/V3/V4 headers, TSphere/TBox/TFace/CompressedVector int16/128) and
// game_sa/FileLoader.cpp (LoadCollisionModel/Ver2/Ver3/Ver4: offsets are
// relative to just after the 8-byte fourcc+size, emptiness/face-group flag
// bits 2/8, shadow bit 16 in V3+). No game_sa/ headers are included.

#include "app/platform/linux/ColLoad.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "app/platform/linux/Collide.h"

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

namespace {

std::string s_game; // game dir for OS_File offset restore

void SetErr(char* err, std::size_t errSize, const char* msg) {
    if (!err || errSize == 0) {
        return;
    }
    (void)std::snprintf(err, errSize, "%s", msg ? msg : "unknown");
}

uint32 ReadU32LE(const uint8* p) {
    return static_cast<uint32>(p[0]) | (static_cast<uint32>(p[1]) << 8) |
           (static_cast<uint32>(p[2]) << 16) | (static_cast<uint32>(p[3]) << 24);
}

uint16 ReadU16LE(const uint8* p) {
    return static_cast<uint16>(p[0] | (p[1] << 8));
}

int16 ReadI16LE(const uint8* p) {
    return static_cast<int16>(p[0] | (p[1] << 8));
}

float ReadF32LE(const uint8* p) {
    uint32 u = ReadU32LE(p);
    float f = 0.0f;
    std::memcpy(&f, &u, sizeof(f));
    return f;
}

bool Finite3(float x, float y, float z) {
    return std::isfinite(x) && std::isfinite(y) && std::isfinite(z);
}

void ToLowerInPlace(std::string& s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c + 32);
        }
    }
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

// Same DOS-path/case-insensitive resolution as SceneShot (game.dat ships
// backslashes; the checkout case differs): duplicated, not refactored, so
// the verified R5/R6a slice stays untouched.
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

constexpr int kReadChunkBytes = 1 << 20;

// Reads exactly `size` bytes from absolute position `pos`. OS_File* uses
// int32 sizes, so large ranges are split (same pattern as SfxDecode).
bool ReadRange(void* file, uint32 pos, uint8* dst, size_t size) {
    uint64 remaining = size;
    uint64 cursor = pos;
    while (remaining > 0) {
        // World .col blobs and IMG entries sit below 2 GiB.
        OS_FileSetPosition(file, static_cast<int32>(cursor));
        const size_t want = remaining > kReadChunkBytes ? kReadChunkBytes : remaining;
        if (OS_FileRead(file, dst, static_cast<int32>(want)) != 0) {
            return false;
        }
        dst += want;
        cursor += want;
        remaining -= want;
    }
    return true;
}

// Absolute-path read through OS_File*: the offset is cleared so BuildPath
// passes the path through, then restored to the game dir. Used for every
// DAT-listed asset (DOS backslashes + Windows case never open directly on
// the Linux checkout); the bytes still travel only through OS_File*.
bool ReadWholeFileAbs(const std::string& absPath, std::vector<uint8>& out) {
    OS_SetFilePathOffset("");
    void* file = nullptr;
    bool opened =
        OS_FileOpen(FILE_DATA_AREA_DEFAULT, &file, absPath.c_str(), FILE_ACCESS_READ) == 0 && file;
    bool ok = false;
    out.clear();
    if (opened) {
        const int32 size = OS_FileSize(file);
        ok = size >= 0;
        if (ok && size > 0) {
            out.resize(static_cast<size_t>(size));
            ok = ReadRange(file, 0, out.data(), out.size());
        }
        OS_FileClose(file);
    }
    OS_SetFilePathOffset(s_game.c_str());
    return ok;
}

bool ReadGameRel(const std::string& rel, std::vector<uint8>& out) {
    std::string abs;
    if (!ResolveGamePath(s_game, rel, abs)) {
        return false;
    }
    return ReadWholeFileAbs(abs, out);
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

// game_sa CFileLoader::LoadLine: commas and control chars are whitespace.
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

void CollectIplLists(const std::string& text, std::vector<std::string>& ipl) {
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t end = text.find('\n', pos);
        std::string line = Trim(text.substr(pos, end == std::string::npos ? std::string::npos : end - pos));
        pos = end == std::string::npos ? text.size() + 1 : end + 1;
        if (line.empty() || line[0] == '#') {
            continue;
        }
        if (!StartsWithNoCase(line, "IPL")) {
            continue;
        }
        std::string after = Trim(line.substr(3));
        if (after.empty()) {
            continue;
        }
        size_t sp = after.find_first_of(" \t");
        std::string path = after.substr(0, sp);
        if (!path.empty()) {
            ipl.push_back(path);
        }
    }
}

struct IplInst {
    std::string model; // original case
    std::string lower; // lowercase key
    int interior = 0;
    float pos[3] = {};
    float quat[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
};

// IPL inst rows (game_sa CFileLoader::LoadObjectInstance): id, model,
// interior, pos(3), quaternion(4: qx qy qz qw), lod index. Same rule as
// SceneShot/StreamPager: interior==0, no LOD* shells.
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
        int lod = -1;
        int n = std::sscanf(line.c_str(), "%d %63s %d %f %f %f %f %f %f %f %d", &id, model,
                            &inst.interior, &inst.pos[0], &inst.pos[1], &inst.pos[2], &qx, &qy,
                            &qz, &qw, &lod);
        if (n != 11 || id < 0 || inst.interior != 0) {
            continue;
        }
        if (!Finite3(inst.pos[0], inst.pos[1], inst.pos[2]) ||
            !std::isfinite(qx) || !std::isfinite(qy) || !std::isfinite(qz) ||
            !std::isfinite(qw)) {
            continue;
        }
        inst.model = model;
        inst.lower = model;
        ToLowerInPlace(inst.lower);
        if (inst.lower.size() >= 3 && inst.lower[0] == 'l' && inst.lower[1] == 'o' &&
            inst.lower[2] == 'd') {
            continue;
        }
        inst.quat[0] = qx;
        inst.quat[1] = qy;
        inst.quat[2] = qz;
        inst.quat[3] = qw;
        out.push_back(inst);
    }
}

// game_sa CMatrix::SetRotate(quat): basis vectors of the instance rotation.
// Same formula as SceneShot::QuatToBasis.
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

struct ColSphere {
    float c[3];
    float r = 0.0f;
};

struct ColBox {
    float mn[3];
    float mx[3];
};

struct ColModel {
    std::string name; // original case from the chunk header
    float bmin[3] = {};
    float bmax[3] = {};
    std::vector<ColSphere> spheres;
    std::vector<ColBox> boxes;
    std::vector<float> verts; // xyz triplets, model space
    std::vector<uint32> tris; // index triplets into verts
};

struct BoundInst {
    int modelIdx = -1;
    std::string name; // IPL model name (original case) for near= reporting
    float pos[3] = {};
    float row[3][3] = {}; // world = pos + row[0]*lx + row[1]*ly + row[2]*lz
    float wmin[3] = {};
    float wmax[3] = {};
};

std::map<std::string, ColModel> s_models; // lower name -> COL (ordered)
std::vector<const ColModel*> s_byIdx; // ordered mirror of s_models
std::vector<BoundInst> s_insts; // IPL DAT order
ColLoadStats s_stats;

bool ParseV1Chunk(const uint8* body, size_t bodySize, ColModel& out) {
    // V1 (COLL): TBounds(40: sphere radius-first, then box) + u32-counted
    // arrays (spheres 20B radius-first, lines skipped, boxes 28B, verts 12B
    // float, faces 16B with u32 indices). Mirrors
    // CFileLoader::LoadCollisionModel.
    if (bodySize < 40) {
        return false;
    }
    float sr = ReadF32LE(body + 0);
    float scx = ReadF32LE(body + 4);
    float scy = ReadF32LE(body + 8);
    float scz = ReadF32LE(body + 12);
    (void)sr;
    (void)scx;
    (void)scy;
    (void)scz;
    float bmin[3] = { ReadF32LE(body + 16), ReadF32LE(body + 20), ReadF32LE(body + 24) };
    float bmax[3] = { ReadF32LE(body + 28), ReadF32LE(body + 32), ReadF32LE(body + 36) };
    if (!Finite3(bmin[0], bmin[1], bmin[2]) || !Finite3(bmax[0], bmax[1], bmax[2])) {
        return false;
    }
    size_t p = 40;
    auto takeU32 = [&](uint32& v) -> bool {
        if (p + 4 > bodySize) {
            return false;
        }
        v = ReadU32LE(body + p);
        p += 4;
        return true;
    };
    uint32 nS = 0;
    if (!takeU32(nS) || nS > 100000) {
        return false;
    }
    if (p + static_cast<size_t>(nS) * 20 > bodySize) {
        return false;
    }
    for (uint32 i = 0; i < nS; ++i) {
        float r = ReadF32LE(body + p);
        float cx = ReadF32LE(body + p + 4);
        float cy = ReadF32LE(body + p + 8);
        float cz = ReadF32LE(body + p + 12);
        p += 20;
        if (!Finite3(cx, cy, cz) || !(r > 0.0f) || !std::isfinite(r)) {
            continue;
        }
        ColSphere s;
        s.c[0] = cx;
        s.c[1] = cy;
        s.c[2] = cz;
        s.r = r;
        out.spheres.push_back(s);
    }
    uint32 nL = 0;
    if (!takeU32(nL) || nL > 100000) {
        return false;
    }
    if (p + static_cast<size_t>(nL) * 24 > bodySize) {
        return false;
    }
    p += static_cast<size_t>(nL) * 24; // suspension lines: unused, skipped
    uint32 nB = 0;
    if (!takeU32(nB) || nB > 100000) {
        return false;
    }
    if (p + static_cast<size_t>(nB) * 28 > bodySize) {
        return false;
    }
    for (uint32 i = 0; i < nB; ++i) {
        float mn[3] = { ReadF32LE(body + p), ReadF32LE(body + p + 4), ReadF32LE(body + p + 8) };
        float mx[3] = { ReadF32LE(body + p + 12), ReadF32LE(body + p + 16),
                        ReadF32LE(body + p + 20) };
        p += 28;
        if (!Finite3(mn[0], mn[1], mn[2]) || !Finite3(mx[0], mx[1], mx[2])) {
            continue;
        }
        if (mx[0] < mn[0] || mx[1] < mn[1] || mx[2] < mn[2]) {
            continue;
        }
        ColBox b;
        std::memcpy(b.mn, mn, sizeof(mn));
        std::memcpy(b.mx, mx, sizeof(mx));
        out.boxes.push_back(b);
    }
    uint32 nV = 0;
    if (!takeU32(nV) || nV > 1000000) {
        return false;
    }
    if (p + static_cast<size_t>(nV) * 12 > bodySize) {
        return false;
    }
    std::vector<float> verts;
    verts.reserve(static_cast<size_t>(nV) * 3);
    for (uint32 i = 0; i < nV; ++i) {
        float x = ReadF32LE(body + p);
        float y = ReadF32LE(body + p + 4);
        float z = ReadF32LE(body + p + 8);
        p += 12;
        if (!Finite3(x, y, z)) {
            return false;
        }
        verts.push_back(x);
        verts.push_back(y);
        verts.push_back(z);
    }
    uint32 nF = 0;
    if (!takeU32(nF) || nF > 1000000) {
        return false;
    }
    if (p + static_cast<size_t>(nF) * 16 > bodySize) {
        return false;
    }
    for (uint32 i = 0; i < nF; ++i) {
        uint32 a = ReadU32LE(body + p);
        uint32 b = ReadU32LE(body + p + 4);
        uint32 c = ReadU32LE(body + p + 8);
        p += 16;
        if (a >= nV || b >= nV || c >= nV) {
            continue;
        }
        out.tris.push_back(a);
        out.tris.push_back(b);
        out.tris.push_back(c);
    }
    out.verts = std::move(verts);
    std::memcpy(out.bmin, bmin, sizeof(bmin));
    std::memcpy(out.bmax, bmax, sizeof(bmax));
    return true;
}

// V2/V3/V4 chunk (offsets relative to just after the 8-byte fourcc+size,
// i.e. chunk base + 4; mirrors the SetColDataPtr arithmetic in
// LoadCollisionModelVer2/3/4). Returns false on any structural problem;
// the chunk is then dropped, never half-kept.
bool ParseV234Chunk(const uint8* chunk, size_t chunkSize, int version, ColModel& out) {
    // chunk[0..8): fourcc+size; chunk[8..32): name22+modelId (FileHeader).
    const size_t headSize = version == 2 ? 76 : (version == 3 ? 88 : 92);
    if (chunkSize < 32 + headSize) {
        return false;
    }
    const uint8* h = chunk + 32;
    float bmin[3] = { ReadF32LE(h + 0), ReadF32LE(h + 4), ReadF32LE(h + 8) };
    float bmax[3] = { ReadF32LE(h + 12), ReadF32LE(h + 16), ReadF32LE(h + 20) };
    float bsph[3] = { ReadF32LE(h + 24), ReadF32LE(h + 28), ReadF32LE(h + 32) };
    float bspr = ReadF32LE(h + 36);
    (void)bsph;
    (void)bspr;
    if (!Finite3(bmin[0], bmin[1], bmin[2]) || !Finite3(bmax[0], bmax[1], bmax[2])) {
        return false;
    }
    uint16 nS = ReadU16LE(h + 40);
    uint16 nB = ReadU16LE(h + 42);
    uint16 nF = ReadU16LE(h + 44);
    uint8 nL = h[46];
    uint32 flags = ReadU32LE(h + 48);
    uint32 offS = ReadU32LE(h + 52);
    uint32 offB = ReadU32LE(h + 56);
    uint32 offL = ReadU32LE(h + 60);
    uint32 offV = ReadU32LE(h + 64);
    uint32 offF = ReadU32LE(h + 68);
    (void)offL;
    if (nS > 100000 || nB > 100000 || nF > 1000000 || nL > 200) {
        return false;
    }
    if (!(flags & 2)) {
        return false; // empty: no spheres/boxes/mesh (mirrors IsEmpty)
    }
    // Offset domain: (chunk + 4 + off) must land inside the chunk.
    auto inRange = [&](uint32 off, size_t len) -> bool {
        uint64 start = static_cast<uint64>(off) + 4;
        return off != 0 && start + len <= chunkSize;
    };
    const uint8* base = chunk; // offsets count from chunk+4 => byte = base[4+off]
    if (nS > 0) {
        if (!inRange(offS, static_cast<size_t>(nS) * 20)) {
            return false;
        }
        for (uint16 i = 0; i < nS; ++i) {
            const uint8* s = base + 4 + offS + static_cast<size_t>(i) * 20;
            float cx = ReadF32LE(s + 0);
            float cy = ReadF32LE(s + 4);
            float cz = ReadF32LE(s + 8);
            float r = ReadF32LE(s + 12);
            if (!Finite3(cx, cy, cz) || !(r > 0.0f) || !std::isfinite(r)) {
                continue;
            }
            ColSphere sp;
            sp.c[0] = cx;
            sp.c[1] = cy;
            sp.c[2] = cz;
            sp.r = r;
            out.spheres.push_back(sp);
        }
    }
    if (nB > 0) {
        if (!inRange(offB, static_cast<size_t>(nB) * 28)) {
            return false;
        }
        for (uint16 i = 0; i < nB; ++i) {
            const uint8* b = base + 4 + offB + static_cast<size_t>(i) * 28;
            float mn[3] = { ReadF32LE(b + 0), ReadF32LE(b + 4), ReadF32LE(b + 8) };
            float mx[3] = { ReadF32LE(b + 12), ReadF32LE(b + 16), ReadF32LE(b + 20) };
            if (!Finite3(mn[0], mn[1], mn[2]) || !Finite3(mx[0], mx[1], mx[2])) {
                continue;
            }
            if (mx[0] < mn[0] || mx[1] < mn[1] || mx[2] < mn[2]) {
                continue;
            }
            ColBox box;
            std::memcpy(box.mn, mn, sizeof(mn));
            std::memcpy(box.mx, mx, sizeof(mx));
            out.boxes.push_back(box);
        }
    }
    if (nF > 0) {
        if (offV == 0 || offF == 0) {
            return false;
        }
        // Face-group preface (u32 count + 28B each) sits right before the
        // faces when flags&8; verts fill [offV, facesStart).
        uint32 nfg = 0;
        if (flags & 8) {
            if (offF < 4 || static_cast<uint64>(offF) + 4 > chunkSize) {
                return false;
            }
            nfg = ReadU32LE(base + 4 + offF - 4);
            if (nfg > 100000) {
                return false;
            }
        }
        uint64 facesStart = static_cast<uint64>(offF) + 4;
        uint64 groupBytes = 4 + static_cast<uint64>(nfg) * 28;
        if ((flags & 8) && (facesStart < groupBytes || facesStart - groupBytes < 4)) {
            return false;
        }
        uint64 vertsStart = static_cast<uint64>(offV) + 4;
        uint64 vertsEnd = (flags & 8) ? facesStart - groupBytes : facesStart;
        if (vertsEnd < vertsStart || vertsEnd > chunkSize || vertsStart > chunkSize) {
            return false;
        }
        // Vert bytes are 6B CompressedVectors; the run is padded with 2
        // bytes to a 4-byte boundary when the vert count is odd (6n+2).
        // Seen in the wild: towerlan2 (133 verts + 2 pad), laroadds_05_las
        // (205 + 2); even counts (LAcityhall1 390, roads03 124) have no pad.
        uint64 vertBytes = vertsEnd - vertsStart;
        if (vertBytes % 6 == 2 && vertBytes >= 2) {
            vertBytes -= 2; // alignment pad, not a vertex
        }
        if (vertBytes % 6 != 0) {
            return false;
        }
        uint64 nV = vertBytes / 6;
        if (nV > 1000000 || nV == 0) {
            return false;
        }
        if (facesStart + static_cast<uint64>(nF) * 8 > chunkSize) {
            return false;
        }
        out.verts.reserve(static_cast<size_t>(nV) * 3);
        for (uint64 i = 0; i < nV; ++i) {
            const uint8* v = chunk + vertsStart + i * 6;
            float x = static_cast<float>(ReadI16LE(v + 0)) / 128.0f;
            float y = static_cast<float>(ReadI16LE(v + 2)) / 128.0f;
            float z = static_cast<float>(ReadI16LE(v + 4)) / 128.0f;
            if (!Finite3(x, y, z)) {
                return false;
            }
            out.verts.push_back(x);
            out.verts.push_back(y);
            out.verts.push_back(z);
        }
        for (uint16 i = 0; i < nF; ++i) {
            const uint8* f = chunk + facesStart + static_cast<size_t>(i) * 8;
            uint32 a = ReadU16LE(f + 0);
            uint32 b = ReadU16LE(f + 2);
            uint32 c = ReadU16LE(f + 4);
            if (a >= nV || b >= nV || c >= nV) {
                continue;
            }
            out.tris.push_back(a);
            out.tris.push_back(b);
            out.tris.push_back(c);
        }
    }
    std::memcpy(out.bmin, bmin, sizeof(bmin));
    std::memcpy(out.bmax, bmax, sizeof(bmax));
    return true;
}

// Parses every chunk of one .col blob; usable models accumulate into
// `dst` (keyed by lowercase name, first wins for deterministic dupes).
void ParseColBlob(const uint8* data, size_t size, std::map<std::string, ColModel>& dst,
                  ColLoadStats& stats) {
    size_t pos = 0;
    while (pos + 32 <= size) {
        const uint8* ch = data + pos;
        int version = 0;
        if (std::memcmp(ch, "COLL", 4) == 0) {
            version = 1;
        } else if (std::memcmp(ch, "COL2", 4) == 0) {
            version = 2;
        } else if (std::memcmp(ch, "COL3", 4) == 0) {
            version = 3;
        } else if (std::memcmp(ch, "COL4", 4) == 0) {
            version = 4;
        } else {
            break; // padding tail: no more chunks (mirrors FileLoader loop)
        }
        uint32 bodySize = ReadU32LE(ch + 4);
        // Total = FileInfo(8) + bodySize; FileHeader is 32.
        if (bodySize < 24 || static_cast<uint64>(pos) + 8 + bodySize > size) {
            break;
        }
        size_t chunkSize = 8 + bodySize;
        char nameBuf[23] = {};
        std::memcpy(nameBuf, ch + 8, 22);
        nameBuf[22] = '\0';
        std::string name(nameBuf);
        std::string lower = name;
        ToLowerInPlace(lower);
        if (!lower.empty() && dst.find(lower) == dst.end()) {
            ColModel m;
            m.name = name;
            bool ok = version == 1 ? ParseV1Chunk(ch + 32, chunkSize - 32, m)
                                   : ParseV234Chunk(ch, chunkSize, version, m);
            if (ok && (!m.spheres.empty() || !m.boxes.empty() || !m.tris.empty())) {
                stats.models += 1;
                stats.spheres += static_cast<long>(m.spheres.size());
                stats.boxes += static_cast<long>(m.boxes.size());
                stats.verts += static_cast<long>(m.verts.size() / 3);
                stats.tris += static_cast<long>(m.tris.size() / 3);
                dst[lower] = std::move(m);
            }
        }
        pos += chunkSize;
    }
}

struct ImgEntry {
    std::string lower;
    uint32 off = 0; // 2048-byte sectors
    uint32 size = 0; // sectors
};

bool EndsWithCol(const std::string& lower) {
    return lower.size() > 4 && lower.compare(lower.size() - 4, 4, ".col") == 0;
}

} // namespace

bool ColLoad_Init(const char* gameDir, ColLoadStats& stats, char* err, std::size_t errSize) {
    stats = ColLoadStats{};
    s_stats = ColLoadStats{};
    s_models.clear();
    s_byIdx.clear();
    s_insts.clear();
    if (!gameDir || !gameDir[0]) {
        SetErr(err, errSize, "no game dir");
        return false;
    }
    std::string game(gameDir);
    s_game = game;
    OS_SetFilePathOffset(game.c_str());

    // --- 1. IPL instances in DAT order (same files as SceneShot). ---
    std::vector<uint8> datBytes;
    if (!ReadGameRel("data/gta.dat", datBytes)) {
        SetErr(err, errSize, "cannot read data/gta.dat");
        return false;
    }
    std::string gtaDat(reinterpret_cast<const char*>(datBytes.data()), datBytes.size());
    datBytes.clear();
    if (!ReadGameRel("data/default.dat", datBytes)) {
        SetErr(err, errSize, "cannot read data/default.dat");
        return false;
    }
    std::string defaultDat(reinterpret_cast<const char*>(datBytes.data()), datBytes.size());
    datBytes.clear();
    std::vector<std::string> iplPaths;
    CollectIplLists(gtaDat, iplPaths);
    CollectIplLists(defaultDat, iplPaths);
    if (iplPaths.empty()) {
        SetErr(err, errSize, "no IPL entries in data/*.dat");
        return false;
    }
    std::vector<IplInst> insts;
    int iplFiles = 0;
    for (const std::string& rel : iplPaths) {
        std::vector<uint8> bytes;
        if (!ReadGameRel(rel, bytes)) {
            continue;
        }
        std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        size_t before = insts.size();
        ParseIplText(text, insts);
        if (insts.size() > before) {
            ++iplFiles;
        }
        if (insts.size() > 60000) {
            break;
        }
    }
    if (insts.empty()) {
        SetErr(err, errSize, "no IPL inst entries parsed");
        return false;
    }

    // --- 2. COL blobs: loose models/coll/*.col via OS_File* ... ---
    int files = 0;
    {
        std::string collDir;
        if (ResolveGamePath(game, "models/coll", collDir)) {
            DIR* dir = opendir(collDir.c_str());
            if (dir) {
                std::vector<std::string> names;
                for (dirent* ent = readdir(dir); ent; ent = readdir(dir)) {
                    std::string n(ent->d_name);
                    std::string l = n;
                    ToLowerInPlace(l);
                    if (EndsWithCol(l)) {
                        names.push_back(n);
                    }
                }
                closedir(dir);
                // Deterministic: readdir order is filesystem-dependent.
                for (size_t i = 0; i < names.size(); ++i) {
                    for (size_t j = i + 1; j < names.size(); ++j) {
                        if (names[j] < names[i]) {
                            std::string t = names[i];
                            names[i] = names[j];
                            names[j] = t;
                        }
                    }
                }
                for (const std::string& n : names) {
                    std::string abs = collDir + "/" + n;
                    std::vector<uint8> bytes;
                    if (!ReadWholeFileAbs(abs, bytes) || bytes.empty()) {
                        continue;
                    }
                    ++files;
                    ParseColBlob(bytes.data(), bytes.size(), s_models, s_stats);
                }
            }
        }
    }

    // --- 3. ... plus every *.col entry of the world IMG archives. ---
    static const char* kImgRels[] = { "models/gta3.img", "models/gta_int.img", "models/player.img" };
    for (const char* rel : kImgRels) {
        std::string abs;
        if (!ResolveGamePath(game, rel, abs)) {
            continue;
        }
        OS_SetFilePathOffset("");
        void* img = nullptr;
        if (OS_FileOpen(FILE_DATA_AREA_DEFAULT, &img, abs.c_str(), FILE_ACCESS_READ) != 0 ||
            !img) {
            OS_SetFilePathOffset(game.c_str());
            continue;
        }
        uint8 head[8] = {};
        bool ok = ReadRange(img, 0, head, sizeof(head)) && std::memcmp(head, "VER2", 4) == 0;
        uint32 count = ok ? ReadU32LE(head + 4) : 0;
        if (!ok || count == 0 || count > 300000) {
            OS_FileClose(img);
            OS_SetFilePathOffset(game.c_str());
            continue;
        }
        std::vector<uint8> dirBytes(static_cast<size_t>(count) * 32);
        if (!ReadRange(img, 8, dirBytes.data(), dirBytes.size())) {
            OS_FileClose(img);
            OS_SetFilePathOffset(game.c_str());
            continue;
        }
        std::vector<ImgEntry> cols;
        for (uint32 i = 0; i < count; ++i) {
            const uint8* e = dirBytes.data() + static_cast<size_t>(i) * 32;
            char nm[25] = {};
            std::memcpy(nm, e + 8, 24);
            std::string name(nm);
            std::string lower = name;
            ToLowerInPlace(lower);
            if (!EndsWithCol(lower)) {
                continue;
            }
            ImgEntry en;
            en.lower = lower;
            en.off = ReadU32LE(e + 0);
            en.size = ReadU32LE(e + 4) & 0x7FFFu;
            if (en.size == 0) {
                continue;
            }
            cols.push_back(en);
        }
        dirBytes.clear();
        // IMG directory order is already deterministic (file order).
        std::vector<uint8> blob;
        for (const ImgEntry& en : cols) {
            uint64 bytes = static_cast<uint64>(en.size) * 2048u;
            if (bytes > (1u << 30)) {
                continue;
            }
            blob.resize(static_cast<size_t>(bytes));
            uint64 byteOff = static_cast<uint64>(en.off) * 2048u;
            if (byteOff > 0x7FFFFFFFu || !ReadRange(img, static_cast<uint32>(byteOff), blob.data(),
                                                   blob.size())) {
                continue;
            }
            ++files;
            ParseColBlob(blob.data(), blob.size(), s_models, s_stats);
        }
        blob.clear();
        OS_FileClose(img);
        OS_SetFilePathOffset(game.c_str());
    }
    s_stats.files = files;
    if (s_models.empty()) {
        SetErr(err, errSize, "no COL models parsed from world blobs");
        return false;
    }

    // --- 4. Bind: IPL instance -> COL model by lowercase name. ---
    // Name -> ordered index for stable BoundInst references (std::map node
    // references stay valid: no insertion happens after this point).
    std::map<std::string, int> index;
    s_byIdx.reserve(s_models.size());
    int k = 0;
    for (const auto& kv : s_models) {
        index[kv.first] = k++;
        s_byIdx.push_back(&kv.second);
    }
    for (const IplInst& inst : insts) {
        auto it = index.find(inst.lower);
        if (it == index.end()) {
            continue;
        }
        const ColModel* m = s_byIdx[static_cast<size_t>(it->second)];
        BoundInst b;
        b.modelIdx = it->second;
        b.name = inst.model;
        b.pos[0] = inst.pos[0];
        b.pos[1] = inst.pos[1];
        b.pos[2] = inst.pos[2];
        float right[3];
        float fwd[3];
        float up[3];
        QuatToBasis(inst.quat, right, fwd, up);
        b.row[0][0] = right[0];
        b.row[0][1] = fwd[0];
        b.row[0][2] = up[0];
        b.row[1][0] = right[1];
        b.row[1][1] = fwd[1];
        b.row[1][2] = up[1];
        b.row[2][0] = right[2];
        b.row[2][1] = fwd[2];
        b.row[2][2] = up[2];
        // World AABB of the model-space bound box (orthonormal rows, so the
        // extremal-corner rule is exact).
        for (int i = 0; i < 3; ++i) {
            float lo = b.pos[i];
            float hi = b.pos[i];
            for (int j = 0; j < 3; ++j) {
                float r = b.row[i][j];
                lo += r >= 0.0f ? r * m->bmin[j] : r * m->bmax[j];
                hi += r >= 0.0f ? r * m->bmax[j] : r * m->bmin[j];
            }
            b.wmin[i] = lo;
            b.wmax[i] = hi;
        }
        s_insts.push_back(b);
    }
    (void)iplFiles;
    if (s_insts.empty()) {
        SetErr(err, errSize, "no IPL instance bound to a COL model");
        return false;
    }
    s_stats.instances = static_cast<int>(s_insts.size());
    stats = s_stats;
    return true;
}

void ColLoad_Probe(double x, double y, ColProbeHit& hit) {
    constexpr float kTop = 500.0f;
    constexpr float kBottom = -50.0f;
    hit.x = x;
    hit.y = y;
    hit.h = kBottom;
    std::snprintf(hit.model, sizeof(hit.model), "-");
    std::snprintf(hit.prim, sizeof(hit.prim), "none");
    std::snprintf(hit.near, sizeof(hit.near), "-");
    hit.nearDist = 0.0;
    if (s_insts.empty() || !std::isfinite(x) || !std::isfinite(y)) {
        return;
    }
    const float fx = static_cast<float>(x);
    const float fy = static_cast<float>(y);
    float best = kBottom;
    bool haveHit = false;
    const char* bestModel = nullptr;
    const char* bestPrim = "none";
    double bestNear = 0.0;
    const char* bestNearName = "-";
    bool haveNear = false;
    for (const BoundInst& b : s_insts) {
        double dx = static_cast<double>(b.pos[0]) - x;
        double dy = static_cast<double>(b.pos[1]) - y;
        double dist = std::sqrt(dx * dx + dy * dy);
        if (!haveNear || dist < bestNear) {
            bestNear = dist;
            bestNearName = b.name.c_str();
            haveNear = true;
        }
        if (fx < b.wmin[0] || fx > b.wmax[0] || fy < b.wmin[1] || fy > b.wmax[1]) {
            continue;
        }
        const ColModel* m = (b.modelIdx >= 0 &&
                               static_cast<size_t>(b.modelIdx) < s_byIdx.size())
                                  ? s_byIdx[static_cast<size_t>(b.modelIdx)]
                                  : nullptr;
        if (!m) {
            continue;
        }
        // World ray -> model-local ray (orthonormal transpose; direction
        // stays unit, so t converts back as z = kTop - t).
        float rel[3] = { fx - b.pos[0], fy - b.pos[1], kTop - b.pos[2] };
        float lo[3] = { b.row[0][0] * rel[0] + b.row[1][0] * rel[1] + b.row[2][0] * rel[2],
                        b.row[0][1] * rel[0] + b.row[1][1] * rel[1] + b.row[2][1] * rel[2],
                        b.row[0][2] * rel[0] + b.row[1][2] * rel[1] + b.row[2][2] * rel[2] };
        float ld[3] = { -b.row[2][0], -b.row[2][1], -b.row[2][2] };
        for (const ColSphere& s : m->spheres) {
            float t = 0.0f;
            if (Collide::RaySphere(lo, ld, s.c, s.r, t)) {
                float z = kTop - t;
                if (std::isfinite(z) && z <= kTop && (!haveHit || z > best)) {
                    best = z;
                    haveHit = true;
                    bestModel = m->name.c_str();
                    bestPrim = "sphere";
                }
            }
        }
        for (const ColBox& bx : m->boxes) {
            float t = 0.0f;
            if (Collide::RayBox(lo, ld, bx.mn, bx.mx, t)) {
                float z = kTop - t;
                if (std::isfinite(z) && z <= kTop && (!haveHit || z > best)) {
                    best = z;
                    haveHit = true;
                    bestModel = m->name.c_str();
                    bestPrim = "box";
                }
            }
        }
        size_t nTri = m->tris.size() / 3;
        for (size_t ti = 0; ti < nTri; ++ti) {
            const float* a = m->verts.data() + static_cast<size_t>(m->tris[ti * 3]) * 3;
            const float* bb = m->verts.data() + static_cast<size_t>(m->tris[ti * 3 + 1]) * 3;
            const float* c = m->verts.data() + static_cast<size_t>(m->tris[ti * 3 + 2]) * 3;
            float t = 0.0f;
            if (Collide::RayTri(lo, ld, a, bb, c, t)) {
                float z = kTop - t;
                if (std::isfinite(z) && z <= kTop && (!haveHit || z > best)) {
                    best = z;
                    haveHit = true;
                    bestModel = m->name.c_str();
                    bestPrim = "tri";
                }
            }
        }
    }
    if (haveHit) {
        hit.h = best;
        std::snprintf(hit.model, sizeof(hit.model), "%s", bestModel ? bestModel : "-");
        std::snprintf(hit.prim, sizeof(hit.prim), "%s", bestPrim);
    }
    std::snprintf(hit.near, sizeof(hit.near), "%s", bestNearName);
    hit.nearDist = bestNear;
}

void ColLoad_Shutdown() {
    s_models.clear();
    s_byIdx.clear();
    s_insts.clear();
    s_stats = ColLoadStats{};
}
