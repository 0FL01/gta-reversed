// StreamPager implementation: IDE/IPL parsing + IMG DFF/TXD loading +
// grid pager with hysteresis eviction. Parsing helpers mirror SceneShot.cpp
// (same DAT order, same IMG VER2 layout, same librw plugin set, same
// CMatrix::SetRotate basis); the pager layer on top is new for R6b.

#include "app/platform/linux/StreamPager.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <algorithm>
#include <utility>
#include <optional>
#include <cassert>

#include "app/platform/linux/TexSample.h"

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

// librw umbrella header (NULL-platform parse-only use, no GL in this TU).
#include <rw.h>

namespace {

// Pager tuning: grid cell ~ SA streaming block, window radius R with
// hysteresis H (evict past R+H so border cells don't thrash).
constexpr float kCellSize = 300.0f;
constexpr float kHysteresis = 100.0f;
constexpr size_t kMaxTxdLineage = 64;
StreamPagerOptions s_options;
NativeCollisionPopulation s_collisionPopulation;
std::map<std::string, std::string> s_txdParents;
bool s_txdParentCatalogValid = true;

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

void CollectDatLists(const std::string& text, std::vector<std::string>& ide, std::vector<std::string>& ipl) {
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
        if (kind[1] == 'D') {
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
                  std::set<std::string>& animModels, std::map<int, std::string>& modelIds) {
    int mode = 0; // 0 none, 1 static, 2 anim, 3 TXD parents
    bool timeModel = false;
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
        std::string head = Trim(low);
        if (head == "end") {
            mode = 0;
            continue;
        }
        if (head == "objs" || head == "tobj") {
            mode = 1;
            timeModel = head == "tobj";
            continue;
        }
        if (head == "anim") {
            mode = 2;
            timeModel = false;
            continue;
        }
        if (head == "txdp") {
            mode = 3;
            continue;
        }
        if (mode == 0) {
            continue;
        }
        std::string line = raw;
        SanitizeLine(line);
        if (mode == 3) {
            char child[64]{}, parent[64]{}, extra[2]{};
            if (std::sscanf(line.c_str(), "%63s %63s %1s", child, parent, extra) != 2) {
                s_txdParentCatalogValid = false;
            } else {
                std::string childKey = child;
                std::string parentKey = parent;
                ToLowerInPlace(childKey);
                ToLowerInPlace(parentKey);
                // LoadTexDictionaryParent assigns the slot each time: later
                // declarations override earlier ones in source DAT order.
                s_txdParents[childKey] = parentKey;
            }
            continue;
        }
        int id = -1;
        char model[64] = {};
        char txd[64] = {};
        if (std::sscanf(line.c_str(), "%d %63s %63s", &id, model, txd) != 3 || id < 0) {
            continue;
        }
        std::string key = model;
        ToLowerInPlace(key);
        modelIds[id] = model;
        s_collisionPopulation.Models[id] = {key, timeModel};
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
    int modelId = -1;
    uint32 flags = 0;
};

// Layout from tBinaryIplFile (0x4c header) and CFileObjectInstance
// (0x28 record). Decode explicitly, never cast unaligned asset bytes.
static uint32 ReadU32(const uint8* p) {
    return uint32(p[0]) | (uint32(p[1]) << 8) | (uint32(p[2]) << 16) | (uint32(p[3]) << 24);
}

static bool ParseBinaryIpl(const std::vector<uint8>& bytes, const std::map<int, std::string>& modelIds,
                           std::vector<IplInst>& out, char* err, size_t errSize) {
    if (bytes.size() < 0x4c || std::memcmp(bytes.data(), "bnry", 4) != 0) {
        SetErr(err, errSize, "invalid binary IPL header");
        return false;
    }
    const uint32 count = ReadU32(bytes.data() + 4);
    const uint32 offset = ReadU32(bytes.data() + 28);
    if (count && (offset < 0x4c || offset > bytes.size() || count > (bytes.size() - offset) / 40)) {
        SetErr(err, errSize, "binary IPL instances out of bounds");
        return false;
    }
    for (uint32 i = 0; i < count; ++i) {
        const uint8* p = bytes.data() + offset + static_cast<size_t>(i) * 40;
        auto model = modelIds.find(static_cast<int32>(ReadU32(p + 28)));
        if (model == modelIds.end()) {
            SetErr(err, errSize, "binary IPL references unknown IDE model ID");
            return false;
        }
        IplInst inst;
        inst.model = model->second;
        inst.modelId = static_cast<int32>(ReadU32(p + 28));
        inst.flags = ReadU32(p + 32);
        // Upper instance-type bits are streaming/tunnel flags, NOT interior.
        inst.interior = static_cast<int>(ReadU32(p + 32) & 0xff);
        inst.lod = static_cast<int32>(ReadU32(p + 36));
        for (int j = 0; j < 7; ++j) {
            const uint32 bits = ReadU32(p + j * 4);
            float value;
            std::memcpy(&value, &bits, sizeof(value));
            if (!std::isfinite(value)) {
                SetErr(err, errSize, "nonfinite binary IPL transform");
                return false;
            }
            if (j < 3) {
                inst.pos[j] = value;
            } else {
                inst.quat[j - 3] = value;
            }
        }
        out.push_back(std::move(inst));
    }
    return true;
}

void ParseIplText(const std::string& text, std::vector<IplInst>& out, bool sourceInstanceType) {
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
        if (sourceInstanceType) {
            // FileLoader.cpp:1092-1110 reads m_nInstanceType, not m_nAreaCode.
            // FileObjectInstance.h:19-27 gives area its low eight bits. Retain
            // the entire authored word, including currently unassigned bits.
            inst.flags = static_cast<uint32>(inst.interior);
            inst.interior = static_cast<int>(inst.flags & 0xff);
        }
        inst.model = model;
        inst.modelId = id;
        inst.quat[0] = qx;
        inst.quat[1] = qy;
        inst.quat[2] = qz;
        inst.quat[3] = qw;
        out.push_back(inst);
    }
}

struct ImgEntry {
    std::string nameLower;
    std::string memberName;
    uint32 off = 0;
    uint32 size = 0;
};

struct ImgIndex {
    std::string absPath;
    std::string archiveName;
    std::string identityArchive;
    std::vector<ImgEntry> entries;
};

bool BuildImgIndex(const std::string& absPath, const std::string& identityArchive, ImgIndex& idx) {
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
    idx.identityArchive = identityArchive;
    const auto slash = absPath.find_last_of('/');
    idx.archiveName = absPath.substr(slash == std::string::npos ? 0 : slash + 1);
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
        e.memberName = name;
        e.nameLower = e.memberName;
        ToLowerInPlace(e.nameLower);
        e.off = off;
        e.size = size & 0x7FFFu;
        idx.entries.push_back(e);
    }
    (void)std::fclose(f);
    return true;
}

bool ImgReadBytes(const ImgIndex& idx, const std::string& wantLower, std::vector<uint8>& out,
                  std::string* sourceMember = nullptr) {
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
        if (ok && sourceMember) {
            *sourceMember = e.memberName;
        }
        return ok && !out.empty();
    }
    return false;
}

bool s_rwInit = false;

bool RwInitEngine() {
    if (s_rwInit) {
        return true;
    }
    if (rw::Engine::state != rw::Engine::Dead) {
        rw::Texture::setLoadTextures(false);
        s_rwInit = true;
        return true;
    }
    if (!rw::Engine::init(nil)) {
        return false;
    }
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

struct CachedModel {
    std::vector<float> pos;
    std::vector<float> nrm;
    std::vector<float> uv; // 6 floats per triangle
    std::vector<int> triImg; // local image index: >=0, -1 flat, -2 missing
    std::vector<float> triCol; // 3 floats per triangle
    std::vector<WorldShotImage> images; // decoded TXD texels for this model
    int tris = 0;
    std::vector<uint8> dayColors, nightColors;
    std::vector<WorldShotSurface> surfaces;
    std::string sourceModelName;
    std::string sourceTxdName;
    std::string sourceArchiveName;
};

enum class TxdLineageValidity {
    Complete,
    MalformedCatalog,
    Cycle,
    TooDeep,
    UnavailableDictionary,
};

struct TxdNameChain {
    std::vector<std::string> names;
    TxdLineageValidity validity{TxdLineageValidity::Complete};
};

struct TxdLookupChain {
    std::vector<rw::TexDictionary*> dictionaries;
    std::vector<NativeAssetIdentity::ArchiveMember> lineage;
    TxdLineageValidity validity{TxdLineageValidity::Complete};
};

static TxdNameChain BuildTxdNameChain(std::string child) {
    TxdNameChain chain;
    ToLowerInPlace(child);
    if (child.empty() || !s_txdParentCatalogValid) {
        chain.validity = TxdLineageValidity::MalformedCatalog;
        return chain;
    }
    std::set<std::string> visited;
    for (size_t depth = 0; depth < kMaxTxdLineage; ++depth) {
        if (!visited.insert(child).second) {
            chain.validity = TxdLineageValidity::Cycle;
            return chain;
        }
        chain.names.push_back(child);
        const auto parent = s_txdParents.find(child);
        if (parent == s_txdParents.end()) {
            return chain;
        }
        child = parent->second;
    }
    chain.validity = TxdLineageValidity::TooDeep;
    return chain;
}

static int FindTextureOwner(const TxdLookupChain& chain, const char* name, const rw::Texture* texture) {
    for (size_t i = 0; i < chain.dictionaries.size(); ++i) {
        if (const auto* found = chain.dictionaries[i]->find(name)) {
            return !texture || found == texture ? static_cast<int>(i) : -1;
        }
    }
    return -1;
}

// librw skips Rockstar's 0x253F2F9 plugin. Read only that bounded extension,
// without registering process-global plugins after another parser starts RW.
// Original layout: uint32 present, then numVertices RGBA (NightColors).
static bool ReadNightColors(const std::vector<uint8>& bytes, rw::Clump* clump,
                            std::map<const rw::Geometry*, std::vector<uint8>>& out,
                            std::map<const rw::Geometry*, int>& geometryOrdinals) {
    struct Chunk { uint32 type; size_t begin, end; };
    auto children = [&](size_t begin, size_t end, std::vector<Chunk>& chunks) {
        if (end > bytes.size()) {
            return false;
        }
        while (begin < end) {
            if (end - begin < 12) {
                return false;
            }
            const auto size = ReadU32(bytes.data() + begin + 4);
            if (size > end - begin - 12) {
                return false;
            }
            chunks.push_back({ReadU32(bytes.data() + begin), begin + 12, begin + 12 + size});
            begin += 12 + size;
        }
        return true;
    };
    if (bytes.size() < 12 || ReadU32(bytes.data()) != 0x10) {
        return false;
    }
    std::vector<Chunk> root;
    if (!children(12, 12ull + ReadU32(bytes.data() + 4), root)) {
        return false;
    }
    std::vector<std::vector<uint8>> colors;
    std::vector<uint32> atomicGeometry;
    for (const auto& chunk : root) {
        if (chunk.type == 0x1A) {
            std::vector<Chunk> geometries;
            if (!children(chunk.begin, chunk.end, geometries)) {
                return false;
            }
            for (const auto& geometry : geometries) {
                if (geometry.type != 0xF) {
                    continue;
                }
                colors.emplace_back();
                std::vector<Chunk> parts;
                if (!children(geometry.begin, geometry.end, parts)) {
                    return false;
                }
                for (const auto& part : parts) {
                    if (part.type != 3) {
                        continue;
                    }
                    std::vector<Chunk> plugins;
                    if (!children(part.begin, part.end, plugins)) {
                        return false;
                    }
                    for (const auto& plugin : plugins) {
                        if (plugin.type == 0x253F2F9) {
                            if (plugin.end - plugin.begin < 4) {
                                return false;
                            }
                            if (ReadU32(bytes.data() + plugin.begin)) {
                                colors.back().assign(bytes.begin() + plugin.begin + 4, bytes.begin() + plugin.end);
                            }
                        }
                    }
                }
            }
        } else if (chunk.type == 0x14) {
            std::vector<Chunk> parts;
            if (!children(chunk.begin, chunk.end, parts) || parts.empty() ||
                parts[0].type != 1 || parts[0].end - parts[0].begin < 16) {
                return false;
            }
            atomicGeometry.push_back(ReadU32(bytes.data() + parts[0].begin + 4));
        }
    }
    // Clump::streamRead/addAtomic appends atomics in file order.
    size_t index = 0;
    FORLIST(link, clump->atomics) {
        const auto* geo = rw::Atomic::fromClump(link)->geometry;
        if (index >= atomicGeometry.size() || atomicGeometry[index] >= colors.size()) {
            return false;
        }
        const auto geometryOrdinal = atomicGeometry[index++];
        const auto& night = colors[geometryOrdinal];
        if (!night.empty() && (!geo || night.size() != static_cast<size_t>(geo->numVertices) * 4)) {
            return false;
        }
        if (geo) {
            geometryOrdinals[geo] = static_cast<int>(geometryOrdinal);
        }
        out[geo] = night;
    }
    return index == atomicGeometry.size();
}

bool FlattenClumpStatic(rw::Clump* clump, const LinkedClump& lc, CachedModel& out,
                        const std::map<const rw::Geometry*, std::vector<uint8>>& nightColors,
                        const std::map<const rw::Geometry*, int>& geometryOrdinals,
                        const TxdLookupChain* sourceChain) {
    out.pos.clear();
    out.nrm.clear();
    out.uv.clear();
    out.triImg.clear();
    out.triCol.clear();
    out.images.clear();
    out.dayColors.clear();
    out.nightColors.clear();
    out.surfaces.clear();
    out.tris = 0;
    std::map<const rw::Texture*, int> imgCache;
    std::map<NativeAssetIdentity::Texture, int> streamedImgCache;
    std::map<std::pair<const rw::Geometry*, int>, int> materialSlots;
    FORLIST(link, clump->atomics) {
        rw::Atomic* atomic = rw::Atomic::fromClump(link);
        rw::Geometry* geo = atomic ? atomic->geometry : nil;
        if (!geo || geo->numTriangles <= 0 || geo->numVertices <= 0) {
            continue;
        }
        if (!geo->triangles || !geo->morphTargets || !geo->morphTargets[0].vertices) {
            continue;
        }
        if (rw::Skin::get(geo)) {
            return false;
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
        size_t base = out.pos.size();
        size_t triBase = out.triImg.size();
        out.pos.resize(base + static_cast<size_t>(geo->numTriangles) * 9);
        out.nrm.resize(base + static_cast<size_t>(geo->numTriangles) * 9);
        out.uv.resize(triBase * 6 + static_cast<size_t>(geo->numTriangles) * 6);
        size_t w = base;
        size_t wuv = triBase * 6;
        int kept = 0;
        std::vector<int> keptImg;
        std::vector<float> keptCol;
        keptImg.reserve(static_cast<size_t>(geo->numTriangles));
        keptCol.reserve(static_cast<size_t>(geo->numTriangles) * 3);
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
                        if (s_options.includeStreamed) {
                            const int owner = sourceChain ?
                                FindTextureOwner(*sourceChain, rit->second.name, real) : -1;
                            if (owner < 0) {
                                imgIdx = -2;
                            } else {
                                NativeAssetIdentity::Texture identity;
                                identity.lineage = sourceChain->lineage;
                                identity.owner = sourceChain->lineage[static_cast<size_t>(owner)];
                                identity.name.assign(real->name, strnlen(real->name, sizeof(real->name)));
                                identity.filter = rit->second.filter;
                                auto cit = streamedImgCache.find(identity);
                                if (cit != streamedImgCache.end()) {
                                    imgIdx = cit->second;
                                } else {
                                    TexImage decoded;
                                    if (TexSample_Decode(real, decoded)) {
                                        decoded.filter = rit->second.filter;
                                        decoded.sourceIdentity = identity;
                                        decoded.hasSourceIdentity = true;
                                        imgIdx = static_cast<int>(out.images.size());
                                        out.images.push_back(std::move(decoded));
                                        streamedImgCache[std::move(identity)] = imgIdx;
                                    } else {
                                        imgIdx = -2;
                                    }
                                }
                            }
                        } else {
                            auto cit = imgCache.find(real);
                            if (cit != imgCache.end()) {
                                imgIdx = cit->second;
                            } else {
                                TexImage decoded;
                                if (TexSample_Decode(real, decoded)) {
                                    decoded.filter = rit->second.filter;
                                    imgIdx = static_cast<int>(out.images.size());
                                    out.images.push_back(std::move(decoded));
                                    imgCache[real] = imgIdx;
                                } else {
                                    imgIdx = -2;
                                }
                            }
                        }
                    } else if (s_options.includeStreamed && sourceChain && rit != lc.resolved.end()) {
                        // Only absence from every dictionary in a complete authored
                        // chain is source-null. An existing undecodable texture is not.
                        imgIdx = FindTextureOwner(*sourceChain, rit->second.name, nullptr) < 0 ? -1 : -2;
                    } else {
                        imgIdx = -2;
                    }
                }
            }
            const rw::V3d* p[3] = { &objVerts[tri.v[0]], &objVerts[tri.v[1]], &objVerts[tri.v[2]] };
            if (s_options.includeStreamed) {
                WorldShotSurface surface;
                const auto slot = materialSlots.try_emplace(
                    std::make_pair(geo, static_cast<int>(tri.matId)), static_cast<int>(materialSlots.size()));
                surface.sourceMaterial = slot.first->second;
                if (const auto geometry = geometryOrdinals.find(geo); geometry != geometryOrdinals.end()) {
                    surface.sourceGeometry = geometry->second;
                }
                surface.sourceTriangle = t;
                const bool lit = geo->flags & rw::Geometry::LIGHT;
                surface.ambient = lit && mat ? mat->surfaceProps.ambient : 0.0f;
                surface.diffuse = lit && norms && mat ? mat->surfaceProps.diffuse : 0.0f;
                if (mat && (geo->flags & rw::Geometry::MODULATE)) {
                    surface.color = {matCol[0], matCol[1], matCol[2], mat->color.alpha / 255.0f};
                }
                out.surfaces.push_back(surface);
                const auto night = nightColors.find(geo);
                for (int k = 0; k < 3; ++k) {
                    const rw::RGBA day = geo->colors ? geo->colors[tri.v[k]] :
                        lit ? rw::RGBA{0, 0, 0, 255} : rw::RGBA{255, 255, 255, 255};
                    const uint8 rgba[]{day.red, day.green, day.blue, day.alpha};
                    out.dayColors.insert(out.dayColors.end(), rgba, rgba + 4);
                    const uint8* nc = night != nightColors.end() && !night->second.empty() ?
                        &night->second[static_cast<size_t>(tri.v[k]) * 4] : rgba;
                    out.nightColors.insert(out.nightColors.end(), nc, nc + 4);
                }
            }
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
                if (uvs) {
                    out.uv[wuv] = uvs[tri.v[k]].u;
                    out.uv[wuv + 1] = uvs[tri.v[k]].v;
                } else {
                    out.uv[wuv] = 0.0f;
                    out.uv[wuv + 1] = 0.0f;
                }
                w += 3;
                wuv += 2;
            }
            keptImg.push_back(imgIdx);
            keptCol.push_back(matCol[0]);
            keptCol.push_back(matCol[1]);
            keptCol.push_back(matCol[2]);
            ++kept;
        }
        out.pos.resize(base + static_cast<size_t>(kept) * 9);
        out.nrm.resize(base + static_cast<size_t>(kept) * 9);
        out.uv.resize(triBase * 6 + static_cast<size_t>(kept) * 6);
        for (int i = 0; i < kept; ++i) {
            out.triImg.push_back(keptImg[static_cast<size_t>(i)]);
            out.triCol.push_back(keptCol[static_cast<size_t>(i) * 3]);
            out.triCol.push_back(keptCol[static_cast<size_t>(i) * 3 + 1]);
            out.triCol.push_back(keptCol[static_cast<size_t>(i) * 3 + 2]);
        }
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

// --- Pager state (deterministic containers only: map/set/vector) ---

struct PagerInst {
    std::string model;
    std::string key; // lowercased model
    std::string txd; // IDE txd (may be empty)
    int modelId = -1;
    float pos[3] = {};
    float quat[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    int order = 0; // DAT order (stable tiebreak)
};

using Cell = std::pair<int, int>;

int CellOf(float v) {
    return static_cast<int>(std::floor(v / kCellSize));
}

// Min distance from point to the axis-aligned cell rect.
float CellRectDist(int cx, int cy, float x, float y) {
    float x0 = static_cast<float>(cx) * kCellSize;
    float x1 = x0 + kCellSize;
    float y0 = static_cast<float>(cy) * kCellSize;
    float y1 = y0 + kCellSize;
    float dx = 0.0f;
    if (x < x0) {
        dx = x0 - x;
    } else if (x > x1) {
        dx = x - x1;
    }
    float dy = 0.0f;
    if (y < y0) {
        dy = y0 - y;
    } else if (y > y1) {
        dy = y - y1;
    }
    return std::sqrt(dx * dx + dy * dy);
}

bool s_init = false;
std::vector<PagerInst> s_insts;
std::map<Cell, std::vector<int>> s_grid; // cell -> IPL-ordered instance rows
std::map<std::string, IdeEntry> s_ide;
std::set<std::string> s_animModels; // lowercased anim (Clump) keys, retained for selected validation
std::vector<ImgIndex> s_imgs;
std::map<std::string, CachedModel> s_cache; // resident DFF models
std::set<std::string> s_failed; // known-bad models (missing/skinned/anim)
std::map<std::string, rw::TexDictionary*> s_txds; // resident TXDs (nil = miss)
std::map<std::string, NativeAssetIdentity::ArchiveMember> s_txdSources;
std::vector<rw::TexDictionary*> s_txdOrder; // non-nil, for shutdown/evict
rw::TexDictionary* s_empty = nil;
std::set<Cell> s_active;
int s_sectorsLoaded = 0;
int s_sectorsEvicted = 0;
int s_modelsPeak = 0;
int s_trisPeak = 0;
int s_texResident = 0;

// Single optional owned LOD supplement (P1-A04). No hardcoded model IDs:
// the bridge supplies the catalog-validated child/parent pair. Parent is
// LOD-filtered out of s_insts; its authored placement/model/TXD/order comes
// from s_collisionPopulation + s_ide, never an invented transform. Disabled
// by default; Shutdown/Init reset to disabled.
struct LodSupplementConfig {
    NativePlacementIdentity child;
    NativePlacementIdentity parent;
    int childPopIdx = -1;
    int parentPopIdx = -1;
    int childRow = -1; // index into s_insts for the selectable child
    std::string childKey; // lowercased model
    std::string parentKey; // lowercased model
    std::string parentTxd; // IDE TXD name (original case, chain lowercases)
    int parentModelId = -1;
    int parentOrder = 0; // global order = parentPopIdx + 1
    float parentPos[3] = {};
    float parentQuat[4] = {0.0f, 0.0f, 0.0f, 1.0f}; // already bound (conjugated when streamed)
};
std::optional<LodSupplementConfig> s_supplement;

bool FindInImgs(const std::string& wantLower, std::vector<uint8>& out,
                std::string* sourceArchive = nullptr,
                NativeAssetIdentity::ArchiveMember* sourceIdentity = nullptr) {
    for (const ImgIndex& idx : s_imgs) {
        std::string sourceMember;
        if (ImgReadBytes(idx, wantLower, out, sourceIdentity ? &sourceMember : nullptr)) {
            if (sourceArchive) {
                *sourceArchive = idx.archiveName;
            }
            if (sourceIdentity) {
                *sourceIdentity = {idx.identityArchive, std::move(sourceMember)};
            }
            return true;
        }
    }
    return false;
}

static TxdLookupChain BuildTxdLookupChain(const std::string& child) {
    const auto names = BuildTxdNameChain(child);
    TxdLookupChain chain;
    chain.validity = names.validity;
    if (chain.validity != TxdLineageValidity::Complete) {
        return chain;
    }
    for (const auto& name : names.names) {
        const auto txd = s_txds.find(name);
        const auto source = s_txdSources.find(name);
        if (txd == s_txds.end() || !txd->second || source == s_txdSources.end()) {
            chain.dictionaries.clear();
            chain.lineage.clear();
            chain.validity = TxdLineageValidity::UnavailableDictionary;
            return chain;
        }
        chain.dictionaries.push_back(txd->second);
        chain.lineage.push_back(source->second);
    }
    return chain;
}

// Shared placed-mesh emission for the normal window and the LOD supplement
// parent: same source IDs/UV/material/lineage path, one-to-one rendered
// identity. Caller owns cap/selection/validity, counting and ordering.
static void EmitPlacedMesh(const CachedModel& cached, const float* worldPos, const float* right,
                           const float* fwd, const float* up, int sourceModelId,
                           uint32_t sourcePlacementId, const NativePlacementIdentity& renderedIdentity,
                           int colorIndex, WorldShotScene& scene, std::map<std::string, int>& globalImg,
                           std::map<NativeAssetIdentity::Texture, int>& streamedGlobalImg, bool& haveBox,
                           std::vector<NativePlacementIdentity>* rendered) {
    WorldShotMesh mesh;
    MeshColor(colorIndex, mesh.color);
    mesh.tris = cached.tris;
    mesh.sourceModelId = sourceModelId;
    mesh.sourceModelName = cached.sourceModelName;
    mesh.sourceTxdName = cached.sourceTxdName;
    mesh.sourceArchiveName = cached.sourceArchiveName;
    mesh.sourcePlacementId = sourcePlacementId;
    size_t count = cached.pos.size();
    mesh.pos.resize(count);
    mesh.nrm.resize(count);
    mesh.uv.resize(cached.uv.size());
    mesh.triImg.resize(cached.triImg.size());
    mesh.triCol.resize(cached.triCol.size());
    if (s_options.includeStreamed) {
        mesh.dayColors = cached.dayColors;
        mesh.nightColors = cached.nightColors;
        mesh.surfaces = cached.surfaces;
    }
    for (size_t ti = 0; ti < cached.triImg.size(); ++ti) {
        int local = cached.triImg[ti];
        if (local >= 0 && local < static_cast<int>(cached.images.size())) {
            const WorldShotImage& src = cached.images[local];
            if (s_options.includeStreamed) {
                if (!src.hasSourceIdentity) {
                    mesh.triImg[ti] = -2;
                } else if (auto git = streamedGlobalImg.find(src.sourceIdentity);
                           git != streamedGlobalImg.end()) {
                    mesh.triImg[ti] = git->second;
                } else {
                    int gi = static_cast<int>(scene.images.size());
                    scene.images.push_back(src);
                    streamedGlobalImg[src.sourceIdentity] = gi;
                    mesh.triImg[ti] = gi;
                }
            } else {
                auto git = globalImg.find(src.name);
                if (git != globalImg.end()) {
                    mesh.triImg[ti] = git->second;
                } else {
                    int gi = static_cast<int>(scene.images.size());
                    scene.images.push_back(src);
                    globalImg[src.name] = gi;
                    mesh.triImg[ti] = gi;
                }
            }
        } else {
            mesh.triImg[ti] = local;
        }
        mesh.triCol[ti * 3] = cached.triCol[ti * 3];
        mesh.triCol[ti * 3 + 1] = cached.triCol[ti * 3 + 1];
        mesh.triCol[ti * 3 + 2] = cached.triCol[ti * 3 + 2];
    }
    for (size_t i = 0; i < count; i += 3) {
        float lx = cached.pos[i];
        float ly = cached.pos[i + 1];
        float lz = cached.pos[i + 2];
        float wx = worldPos[0] + right[0] * lx + fwd[0] * ly + up[0] * lz;
        float wy = worldPos[1] + right[1] * lx + fwd[1] * ly + up[1] * lz;
        float wz = worldPos[2] + right[2] * lx + fwd[2] * ly + up[2] * lz;
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
        {
            size_t vi2 = (i / 3) * 2;
            if (vi2 + 1 < cached.uv.size()) {
                mesh.uv[vi2] = cached.uv[vi2];
                mesh.uv[vi2 + 1] = cached.uv[vi2 + 1];
            }
        }
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
    if (rendered) {
        rendered->push_back(renderedIdentity);
    }
}

// Shared wanted-resource stages for the legacy window (stages 4-6) and the
// P1-A07 selected residency. No new loader/catalog authority: both paths use
// the same TXD-lineage/DFF-cache/eviction implementation. Legacy mode
// preserves missing/skinned skip behavior exactly; selected mode is explicit
// fail-closed (strictSelected) with the specific identity in every message.
static void PagerPageInTxds(const std::map<std::string, int>& wantTxd) {
    for (const auto& kv : wantTxd) {
        if (s_txds.find(kv.first) != s_txds.end()) {
            continue;
        }
        std::vector<uint8> txdBytes;
        NativeAssetIdentity::ArchiveMember sourceIdentity;
        if (FindInImgs(kv.first + ".txd", txdBytes, nullptr, &sourceIdentity)) {
            rw::TexDictionary* txd = ParseTxd(txdBytes);
            if (txd && (s_options.includeStreamed || txd->count() > 0)) {
                s_txds[kv.first] = txd;
                s_txdSources[kv.first] = std::move(sourceIdentity);
                s_txdOrder.push_back(txd);
            } else {
                if (txd) {
                    txd->destroy();
                }
                s_txds[kv.first] = nil;
                s_txdSources.erase(kv.first);
            }
        } else {
            s_txds[kv.first] = nil;
            s_txdSources.erase(kv.first);
        }
    }
}

static bool PagerPageInModels(const std::map<std::string, int>& wantModel, bool strictSelected,
                              const std::map<std::string, std::string>* identityForModel,
                              char* err, std::size_t errSize) {
    auto identityOf = [&](const std::string& key) -> std::string {
        if (identityForModel) {
            const auto found = identityForModel->find(key);
            if (found != identityForModel->end() && !found->second.empty()) return found->second;
        }
        return key;
    };
    for (const auto& kv : wantModel) {
        if (s_cache.find(kv.first) != s_cache.end()) {
            continue;
        }
        if (!strictSelected) {
            if (s_failed.find(kv.first) != s_failed.end()) {
                continue;
            }
        } else if (s_failed.find(kv.first) != s_failed.end()) {
            SetErr(err, errSize,
                   ("failed selected model " + kv.first + " identity " + identityOf(kv.first)).c_str());
            return false;
        }
        // Resolve the TXD the IDE row names (R5 rule) before streaming.
        std::string txdName;
        auto iit = s_ide.find(kv.first);
        if (iit != s_ide.end()) {
            txdName = iit->second.txd;
        } else if (strictSelected) {
            SetErr(err, errSize,
                   ("missing IDE for selected identity " + identityOf(kv.first)).c_str());
            return false;
        }
        rw::TexDictionary* primary = nil;
        std::vector<rw::TexDictionary*> fb;
        TxdLookupChain sourceChain;
        if (s_options.includeStreamed) {
            sourceChain = BuildTxdLookupChain(txdName);
            if (sourceChain.validity != TxdLineageValidity::Complete || sourceChain.dictionaries.empty()) {
                if (!strictSelected) {
                    const auto message = std::string{"unavailable authored TXD lineage for model "} + kv.first +
                        " (" + txdName + ")";
                    SetErr(err, errSize, message.c_str());
                    return false;
                }
                SetErr(err, errSize,
                       ("unavailable authored TXD lineage for model " + kv.first + " (" + txdName +
                        ") identity " + identityOf(kv.first))
                           .c_str());
                return false;
            }
            if (sourceChain.validity == TxdLineageValidity::Complete &&
                !sourceChain.dictionaries.empty()) {
                primary = sourceChain.dictionaries.front();
                fb.assign(sourceChain.dictionaries.begin() + 1, sourceChain.dictionaries.end());
            }
        } else {
            if (!txdName.empty()) {
                std::string tk = txdName;
                ToLowerInPlace(tk);
                auto tit = s_txds.find(tk);
                if (tit != s_txds.end()) {
                    primary = tit->second;
                }
            }
            for (rw::TexDictionary* txd : s_txdOrder) {
                if (txd && txd != primary) {
                    fb.push_back(txd);
                }
            }
        }
        std::vector<uint8> dffBytes;
        std::string sourceArchive;
        if (!FindInImgs(kv.first + ".dff", dffBytes, &sourceArchive)) {
            s_failed.insert(kv.first);
            if (!strictSelected) {
                continue;
            }
            SetErr(err, errSize, ("missing DFF for selected identity " + identityOf(kv.first)).c_str());
            return false;
        }
        LinkedClump lc = TexSample_LinkedParse(dffBytes.data(), dffBytes.size(), primary,
                                               fb.empty() ? nullptr : fb.data(), fb.size());
        if (!lc.clump) {
            TexSample_FreeLinked(lc);
            s_failed.insert(kv.first);
            if (!strictSelected) {
                continue;
            }
            SetErr(err, errSize, ("failed DFF for selected identity " + identityOf(kv.first)).c_str());
            return false;
        }
        CachedModel cached;
        cached.sourceModelName = iit != s_ide.end() ? iit->second.model : kv.first;
        cached.sourceTxdName = txdName;
        cached.sourceArchiveName = sourceArchive;
        std::map<const rw::Geometry*, std::vector<uint8>> nightColors;
        std::map<const rw::Geometry*, int> geometryOrdinals;
        bool ok = !s_options.includeStreamed || ReadNightColors(dffBytes, lc.clump, nightColors, geometryOrdinals);
        const auto* identityChain = s_options.includeStreamed &&
                sourceChain.validity == TxdLineageValidity::Complete
            ? &sourceChain
            : nullptr;
        ok = ok && FlattenClumpStatic(lc.clump, lc, cached, nightColors, geometryOrdinals, identityChain);
        TexSample_FreeLinked(lc);
        if (!ok) {
            s_failed.insert(kv.first); // skinned or GPU-only: honestly skipped in legacy
            if (!strictSelected) {
                continue;
            }
            SetErr(err, errSize, ("skinned/empty DFF for selected identity " + identityOf(kv.first)).c_str());
            return false;
        }
        if (strictSelected && (cached.tris <= 0 || cached.pos.empty())) {
            s_failed.insert(kv.first);
            SetErr(err, errSize, ("empty DFF for selected identity " + identityOf(kv.first)).c_str());
            return false;
        }
        s_cache[kv.first] = std::move(cached);
    }
    return true;
}

static void PagerEvictUnreferenced(const std::map<std::string, int>& wantModel,
                                   const std::map<std::string, int>& wantTxd) {
    {
        std::vector<std::string> drop;
        for (const auto& kv : s_cache) {
            if (wantModel.find(kv.first) == wantModel.end()) {
                drop.push_back(kv.first);
            }
        }
        for (const std::string& k : drop) {
            s_cache.erase(k);
        }
    }
    {
        std::vector<std::string> drop;
        for (const auto& kv : s_txds) {
            if (!kv.second) {
                continue; // miss markers are free
            }
            if (wantTxd.find(kv.first) == wantTxd.end()) {
                drop.push_back(kv.first);
            }
        }
        for (const std::string& k : drop) {
            rw::TexDictionary* txd = s_txds[k];
            if (txd) {
                txd->destroy();
            }
            s_txds.erase(k);
            s_txdSources.erase(k);
            for (auto it = s_txdOrder.begin(); it != s_txdOrder.end(); ++it) {
                if (*it == txd) {
                    s_txdOrder.erase(it);
                    break;
                }
            }
        }
    }
    s_texResident = 0;
    for (const auto& kv : s_txds) {
        if (kv.second) {
            ++s_texResident;
        }
    }
}

} // namespace

bool StreamPager_Init(const char* gameDir, E2ELoadInfo& info, char* err, std::size_t errSize,
                      const StreamPagerOptions& options) {
    assert(std::isfinite(options.radius) && options.radius > 0 && options.maxInstances > 0);
    s_options = options;
    s_collisionPopulation = {};
    s_supplement.reset();
    s_txdParents.clear();
    s_txdParentCatalogValid = true;
    s_collisionPopulation.IncludesStreamed = options.includeStreamed;
    info = E2ELoadInfo{};
    if (!gameDir || !gameDir[0]) {
        SetErr(err, errSize, "no game dir");
        return false;
    }
    std::string game(gameDir);
    OS_SetFilePathOffset(game.c_str());

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
        idePaths.insert(options.includeStreamed ? idePaths.begin() : idePaths.end(), ide2.begin(), ide2.end());
        iplPaths.insert(iplPaths.end(), ipl2.begin(), ipl2.end());
    }
    if (idePaths.empty() || iplPaths.empty()) {
        SetErr(err, errSize, "no IDE/IPL entries in data/*.dat");
        return false;
    }

    s_ide.clear();
    s_animModels.clear();
    std::set<std::string> animModels;
    std::map<int, std::string> modelIds;
    int ideFiles = 0;
    for (const std::string& rel : idePaths) {
        std::string text;
        if (!ReadGameText(game, rel, text)) {
            if (options.includeStreamed) {
                s_txdParentCatalogValid = false;
            }
            continue;
        }
        ParseIdeText(text, s_ide, animModels, modelIds);
        ++ideFiles;
    }
    s_animModels = animModels;
    if (s_ide.empty()) {
        SetErr(err, errSize, "no IDE model entries parsed");
        return false;
    }

    std::vector<IplInst> all;
    auto exportInstances = [&](size_t begin, const std::string& source, bool binary) {
        for (size_t i = begin; i < all.size(); ++i) {
            const auto& inst = all[i];
            NativeCollisionPlacement p;
            p.Model = inst.model; ToLowerInPlace(p.Model);
            p.ModelId = inst.modelId; p.Interior = inst.interior; p.Lod = inst.lod;
            p.Flags = inst.flags; p.Ipl = source; p.Binary = binary;
            p.Record = static_cast<uint32>(i - begin);
            std::copy_n(inst.pos, 3, p.Position.begin());
            std::copy_n(inst.quat, 4, p.Quaternion.begin());
            s_collisionPopulation.Instances.push_back(std::move(p));
        }
    };
    int iplFiles = 0;
    for (const std::string& rel : iplPaths) {
        std::string text;
        if (!ReadGameText(game, rel, text)) {
            continue;
        }
        size_t before = all.size();
        ParseIplText(text, all, options.includeStreamed);
        exportInstances(before, rel, false);
        if (all.size() > before) {
            ++iplFiles;
        }
    }
    if (all.empty()) {
        SetErr(err, errSize, "no IPL inst entries parsed");
        return false;
    }

    s_imgs.clear();
    static const char* kImgRels[] = { "models/gta3.img", "models/gta_int.img", "models/player.img" };
    for (const char* rel : kImgRels) {
        std::string abs;
        if (!ResolveGamePath(game, rel, abs)) {
            continue;
        }
        ImgIndex idx;
        if (BuildImgIndex(abs, rel, idx)) {
            s_imgs.push_back(std::move(idx));
        }
    }
    if (s_imgs.empty()) {
        SetErr(err, errSize, "no IMG archive indexed (models/*.img)");
        return false;
    }

    if (options.includeStreamed) {
        const size_t textCount = all.size();
        std::set<std::string> loaded;
        for (const auto& img : s_imgs) {
            for (const auto& entry : img.entries) {
                if (!entry.nameLower.ends_with(".ipl") || !loaded.insert(entry.nameLower).second) {
                    continue;
                }
                std::vector<uint8> bytes;
                const auto before = all.size();
                if (!ImgReadBytes(img, entry.nameLower, bytes) ||
                    !ParseBinaryIpl(bytes, modelIds, all, err, errSize)) {
                    std::printf("pager-binary-fail file=%s\n", entry.nameLower.c_str());
                    return false;
                }
                ++info.binaryIplFiles;
                exportInstances(before, img.absPath + ":" + entry.nameLower, true);
            }
        }
        info.binaryInstances = static_cast<int>(all.size() - textCount);
    }

    if (!RwInitEngine()) {
        SetErr(err, errSize, "librw Engine::init failed");
        return false;
    }
    s_cache.clear();
    s_failed.clear();
    s_txds.clear();
    s_txdSources.clear();
    s_txdOrder.clear();
    s_active.clear();
    s_sectorsLoaded = 0;
    s_sectorsEvicted = 0;
    s_modelsPeak = 0;
    s_trisPeak = 0;
    s_texResident = 0;
    s_empty = rw::TexDictionary::create();
    if (s_empty) {
        rw::TexDictionary::setCurrent(s_empty);
    }

    // Filtered static index in DAT order (deterministic).
    s_insts.clear();
    s_grid.clear();
    int order = 0;
    for (const IplInst& inst : all) {
        ++order;
        if (inst.interior != 0) {
            continue;
        }
        if (StartsWithLod(inst.model)) {
            continue;
        }
        std::string key = inst.model;
        ToLowerInPlace(key);
        if (animModels.find(key) != animModels.end()) {
            continue;
        }
        PagerInst p;
        p.model = inst.model;
        p.key = key;
        p.modelId = inst.modelId;
        auto it = s_ide.find(key);
        p.txd = it != s_ide.end() ? it->second.txd : std::string();
        p.pos[0] = inst.pos[0];
        p.pos[1] = inst.pos[1];
        p.pos[2] = inst.pos[2];
        p.quat[0] = inst.quat[0];
        p.quat[1] = inst.quat[1];
        p.quat[2] = inst.quat[2];
        p.quat[3] = inst.quat[3];
        if (options.includeStreamed) {
            // IPL stores the inverse rotation. CFileLoader::LoadObjectInstance
            // conjugates it before CMatrix::SetRotate (or negates Z heading).
            // Keep historical offline fixture transforms unchanged.
            p.quat[0] = -p.quat[0];
            p.quat[1] = -p.quat[1];
            p.quat[2] = -p.quat[2];
        }
        p.order = order;
        int row = static_cast<int>(s_insts.size());
        s_insts.push_back(p);
        Cell c{ CellOf(p.pos[0]), CellOf(p.pos[1]) };
        s_grid[c].push_back(row);
    }
    if (s_insts.empty()) {
        SetErr(err, errSize, "no static outdoor instances indexed");
        return false;
    }
    info.iplTotal = static_cast<int>(all.size());
    info.iplKept = static_cast<int>(s_insts.size());
    info.ideModels = static_cast<int>(s_ide.size());
    info.ideFiles = ideFiles;
    info.iplFiles = iplFiles;
    s_init = true;
    return true;
}

bool StreamPager_Update(float camX, float camY, float camZ, WorldShotScene& scene, E2EPagerFrame& frame,
                        char* err, std::size_t errSize,
                        const std::shared_ptr<const NativePlacementOverrides>& overrides,
                        std::vector<NativePlacementIdentity>* rendered) {
    const float kRadius = s_options.radius;
    const int kMaxInstances = s_options.maxInstances;
    (void)camZ; // window is x/y based (verticality comes free with instances)
    frame = E2EPagerFrame{};
    scene.meshes.clear();
    if (rendered) rendered->clear();
    if (!s_init) {
        SetErr(err, errSize, "pager not initialized");
        return false;
    }

    // The source population and static grid stay authored. Replaced rows leave
    // that grid for this build and are culled at their requested world position.
    std::map<int, const NativePlacementOverride*> replacements;
    if (overrides) {
        for (size_t row = 0; row < s_insts.size(); ++row) {
            const auto& source = s_collisionPopulation.Instances[s_insts[row].order - 1];
            if (const auto* replacement = overrides->Find(source)) replacements.emplace(static_cast<int>(row), replacement);
        }
    }
    const auto position = [&](int row) -> const float* {
        const auto it = replacements.find(row);
        return it == replacements.end() ? s_insts[row].pos : it->second->Position.data();
    };

    // --- 1. Wanted cells (rect intersects the R disc). ---
    std::set<Cell> wanted;
    int ix0 = CellOf(camX - kRadius);
    int ix1 = CellOf(camX + kRadius);
    int iy0 = CellOf(camY - kRadius);
    int iy1 = CellOf(camY + kRadius);
    for (int cx = ix0; cx <= ix1; ++cx) {
        for (int cy = iy0; cy <= iy1; ++cy) {
            if (CellRectDist(cx, cy, camX, camY) <= kRadius) {
                wanted.insert(Cell{ cx, cy });
            }
        }
    }
    // Hysteresis: keep resident cells until they leave R+H.
    std::set<Cell> next = wanted;
    for (const Cell& c : s_active) {
        if (next.find(c) == next.end() && CellRectDist(c.first, c.second, camX, camY) <= kRadius + kHysteresis) {
            next.insert(c);
        }
    }
    int loaded = 0;
    for (const Cell& c : next) {
        if (s_active.find(c) == s_active.end()) {
            ++loaded;
        }
    }
    int evicted = 0;
    // Collect evicted cells with crossing distance (deterministic order).
    std::vector<Cell> evList;
    for (const Cell& c : s_active) {
        if (next.find(c) == next.end()) {
            evList.push_back(c);
        }
    }
    std::sort(evList.begin(), evList.end());
    evicted = static_cast<int>(evList.size());
    s_active = next;
    s_sectorsLoaded += loaded;
    s_sectorsEvicted += evicted;

    // --- 2. Candidate instances within R (exact per-instance cull). ---
    struct Cand {
        float d;
        int order;
        int row;
    };
    std::vector<Cand> cands;
    for (const Cell& c : next) {
        auto git = s_grid.find(c);
        if (git == s_grid.end()) {
            continue;
        }
        for (int row : git->second) {
            if (replacements.contains(row)) continue;
            const PagerInst& p = s_insts[static_cast<size_t>(row)];
            float dx = p.pos[0] - camX;
            float dy = p.pos[1] - camY;
            float d = std::sqrt(dx * dx + dy * dy);
            if (d <= kRadius) {
                cands.push_back(Cand{ d, p.order, row });
            }
        }
    }
    for (const auto& [row, replacement] : replacements) {
        const float dx = replacement->Position[0] - camX, dy = replacement->Position[1] - camY;
        const float d = std::sqrt(dx * dx + dy * dy);
        if (d <= kRadius) cands.push_back(Cand{d, s_insts[row].order, row});
    }
    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) {
        if (a.d != b.d) {
            return a.d < b.d;
        }
        return a.order < b.order;
    });
    if (cands.size() > static_cast<size_t>(kMaxInstances)) {
        cands.resize(static_cast<size_t>(kMaxInstances));
    }
    if (cands.size() < 8) {
        // Sparse window (water/void edge): widen deterministically so the
        // frame still shows real world instead of background.
        cands.clear();
        const float wide = 750.0f;
        for (size_t row = 0; row < s_insts.size(); ++row) {
            const PagerInst& p = s_insts[row];
            const auto* pos = position(static_cast<int>(row));
            float dx = pos[0] - camX;
            float dy = pos[1] - camY;
            float d = std::sqrt(dx * dx + dy * dy);
            if (d <= wide) {
                cands.push_back(Cand{ d, p.order, static_cast<int>(row) });
            }
        }
        std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) {
            if (a.d != b.d) {
                return a.d < b.d;
            }
            return a.order < b.order;
        });
        if (cands.size() > static_cast<size_t>(kMaxInstances)) {
            cands.resize(static_cast<size_t>(kMaxInstances));
        }
        frame.fallback = 1;
    }
    if (cands.empty()) {
        SetErr(err, errSize, "no instances near camera (void)");
        return false;
    }

    // --- 2b. Optional single-chain LOD supplement selection + cap reserve. ---
    // Default disabled: no-supplement behavior below is bit-identical. When the
    // configured child is in the final candidate window, one slot is reserved
    // for its real parent (same position, LOD-filtered, authored transform).
    // The child is preserved at the cap edge by evicting the farthest other
    // candidate; cap 1 with a selected child rejects the whole update.
    bool lodPairNeeded = false;
    const LodSupplementConfig* lodSup = nullptr;
    if (s_supplement.has_value()) {
        lodSup = &(*s_supplement);
        for (const Cand& c : cands) {
            if (c.row == lodSup->childRow) {
                lodPairNeeded = true;
                break;
            }
        }
        if (lodPairNeeded) {
            if (kMaxInstances <= 1) {
                SetErr(err, errSize,
                       "LOD supplement pair needs 2 slots but cap is 1 (child selected, update rejected)");
                return false;
            }
            if (cands.size() >= static_cast<size_t>(kMaxInstances)) {
                int removeIdx = -1;
                for (int i = static_cast<int>(cands.size()) - 1; i >= 0; --i) {
                    if (cands[static_cast<size_t>(i)].row != lodSup->childRow) {
                        removeIdx = i;
                        break;
                    }
                }
                if (removeIdx < 0) {
                    SetErr(err, errSize,
                           "LOD supplement cannot reserve pair slot (child only, cap overflow)");
                    return false;
                }
                cands.erase(cands.begin() + removeIdx);
            }
        }
    }

    // --- 3. Reference sets for this window. ---
    std::map<std::string, int> wantModel; // key -> instance count
    std::map<std::string, int> wantTxd; // lower txd -> user count
    for (const Cand& c : cands) {
        const PagerInst& p = s_insts[static_cast<size_t>(c.row)];
        if (s_failed.find(p.key) != s_failed.end()) {
            continue;
        }
        wantModel[p.key]++;
        if (!p.txd.empty()) {
            if (s_options.includeStreamed) {
                for (const auto& txd : BuildTxdNameChain(p.txd).names) {
                    wantTxd[txd]++;
                }
            } else {
                std::string tk = p.txd;
                ToLowerInPlace(tk);
                wantTxd[tk]++;
            }
        }
    }
    // Parent is wanted only when its child is selected; otherwise it evicts
    // via the normal unreferenced path below. Same stage-5 lineage path.
    if (lodPairNeeded && lodSup) {
        wantModel[lodSup->parentKey]++;
        if (!lodSup->parentTxd.empty()) {
            if (s_options.includeStreamed) {
                for (const auto& txd : BuildTxdNameChain(lodSup->parentTxd).names) {
                    wantTxd[txd]++;
                }
            } else {
                std::string tk = lodSup->parentTxd;
                ToLowerInPlace(tk);
                wantTxd[tk]++;
            }
        }
    }

    // --- 4-6. Shared wanted-resource stages (TXD/DFF/eviction). Legacy mode
    // preserves missing/skinned skip behavior exactly.
    PagerPageInTxds(wantTxd);
    if (!PagerPageInModels(wantModel, false, nullptr, err, errSize)) {
        return false;
    }
    PagerEvictUnreferenced(wantModel, wantTxd);

    // Supplement pair must be fully loadable before publishing any scene:
    // never continue with a half pair. Other candidates keep honest skipping.
    if (lodPairNeeded && lodSup) {
        const auto childIt = s_cache.find(lodSup->childKey);
        const auto parentIt = s_cache.find(lodSup->parentKey);
        const bool childOk = childIt != s_cache.end() && childIt->second.tris > 0 &&
                             !childIt->second.pos.empty();
        const bool parentOk = parentIt != s_cache.end() && parentIt->second.tris > 0 &&
                              !parentIt->second.pos.empty();
        if (!childOk || !parentOk) {
            if (!childOk && !parentOk) {
                SetErr(err, errSize,
                       "LOD supplement child and parent geometry missing/failed (pair rejected)");
            } else if (!childOk) {
                SetErr(err, errSize, "LOD supplement child geometry missing/failed (pair rejected)");
            } else {
                SetErr(err, errSize, "LOD supplement parent geometry missing/failed (pair rejected)");
            }
            return false;
        }
    }

    // --- 7. Build the world-space scene in candidate order. ---
    bool haveBox = false;
    int placed = 0;
    int tris = 0;
    std::set<std::string> usedModels;
    std::map<std::string, int> globalImg; // texture name -> scene image
    std::map<NativeAssetIdentity::Texture, int> streamedGlobalImg;
    scene.images.clear();
    for (const Cand& c : cands) {
        const PagerInst& p = s_insts[static_cast<size_t>(c.row)];
        auto cit = s_cache.find(p.key);
        if (cit == s_cache.end()) {
            // Supplement pair never skips half: the pre-build check above
            // already rejected a missing child/parent. Other candidates keep
            // the honest skip.
            if (lodPairNeeded && lodSup && c.row == lodSup->childRow) {
                SetErr(err, errSize, "LOD supplement child geometry missing/failed (pair rejected)");
                return false;
            }
            continue; // failed/skinned/evicted-race: skip honestly
        }
        const CachedModel& cached = cit->second;
        float right[3];
        float fwd[3];
        float up[3];
        QuatToBasis(p.quat, right, fwd, up);
        if (const auto it = replacements.find(c.row); it != replacements.end()) {
            std::copy_n(it->second->Basis[0].begin(), 3, right);
            std::copy_n(it->second->Basis[1].begin(), 3, fwd);
            std::copy_n(it->second->Basis[2].begin(), 3, up);
        }
        const auto* pos = position(c.row);
        EmitPlacedMesh(cached, pos, right, fwd, up, p.modelId, static_cast<uint32_t>(p.order),
                       NativePlacementIdentity::From(s_collisionPopulation.Instances[p.order - 1]), placed,
                       scene, globalImg, streamedGlobalImg, haveBox, rendered);
        usedModels.insert(p.key);
        ++placed;
        tris += cached.tris;
    }
    // Real parent DFF via the same stage-5 lineage path and stage-7 helper,
    // with authored placement/model/TXD/order (never an invented transform).
    if (lodPairNeeded && lodSup) {
        auto parentCache = s_cache.find(lodSup->parentKey);
        if (parentCache == s_cache.end() || parentCache->second.tris <= 0 ||
            parentCache->second.pos.empty()) {
            SetErr(err, errSize, "LOD supplement parent geometry missing/failed (pair rejected)");
            return false;
        }
        const CachedModel& parentCached = parentCache->second;
        float right[3];
        float fwd[3];
        float up[3];
        QuatToBasis(lodSup->parentQuat, right, fwd, up);
        EmitPlacedMesh(parentCached, lodSup->parentPos, right, fwd, up, lodSup->parentModelId,
                       static_cast<uint32_t>(lodSup->parentOrder), lodSup->parent, placed, scene, globalImg,
                       streamedGlobalImg, haveBox, rendered);
        usedModels.insert(lodSup->parentKey);
        ++placed;
        tris += parentCached.tris;
    }
    if (placed == 0) {
        SetErr(err, errSize, "window has no loadable models (all failed)");
        return false;
    }
    if (static_cast<int>(s_cache.size()) > s_modelsPeak) {
        s_modelsPeak = static_cast<int>(s_cache.size());
    }
    if (tris > s_trisPeak) {
        s_trisPeak = tris;
    }
    (void)std::snprintf(scene.stats.dffName, sizeof(scene.stats.dffName), "pager:%d", placed);
    (void)std::snprintf(scene.stats.txdName, sizeof(scene.stats.txdName), "multi:%d", s_texResident);
    scene.stats.atomics = placed;
    scene.stats.triangles = tris;
    scene.stats.vertices = tris * 3;
    scene.stats.textures = 0;
    scene.stats.firstTexture[0] = '\0';
    scene.stats.firstTexW = 0;
    scene.stats.firstTexH = 0;

    frame.instances = placed;
    frame.modelsUnique = static_cast<int>(usedModels.size());
    frame.tris = tris;
    frame.verts = tris * 3;
    frame.activeCells = static_cast<int>(next.size());
    frame.loadedCells = loaded;
    frame.evictedCells = evicted;
    frame.cacheModels = static_cast<int>(s_cache.size());
    frame.texDicts = s_texResident;
    int shown = 0;
    for (const Cell& c : evList) {
        if (shown >= 16) {
            break;
        }
        frame.evictedCX[shown] = c.first;
        frame.evictedCY[shown] = c.second;
        frame.evictedDist[shown] = static_cast<int>(CellRectDist(c.first, c.second, camX, camY));
        ++shown;
    }
    frame.evictedShown = shown;
    return true;
}

void StreamPager_Counters(int& sectorsLoaded, int& sectorsEvicted, int& modelsPeak, int& trisPeak) {
    sectorsLoaded = s_sectorsLoaded;
    sectorsEvicted = s_sectorsEvicted;
    modelsPeak = s_modelsPeak;
    trisPeak = s_trisPeak;
}

void StreamPager_Shutdown() {
    s_supplement.reset();
    s_collisionPopulation = {};
    s_txdParents.clear();
    s_txdParentCatalogValid = true;
    for (rw::TexDictionary* txd : s_txdOrder) {
        if (txd) {
            txd->destroy();
        }
    }
    s_txdOrder.clear();
    s_txds.clear();
    s_txdSources.clear();
    s_cache.clear();
    s_failed.clear();
    s_insts.clear();
    s_grid.clear();
    s_ide.clear();
    s_animModels.clear();
    s_imgs.clear();
    s_active.clear();
    if (s_empty) {
        s_empty->destroy();
        s_empty = nil;
    }
    s_init = false;
}

bool StreamPager_CollisionPopulation(NativeCollisionPopulation& out, std::string& error) {
    if (!s_init) { error = "collision population requires initialized pager"; return false; }
    if (!s_collisionPopulation.IncludesStreamed) {
        error = "source collision requires pager includeStreamed=true"; return false;
    }
    out = s_collisionPopulation;
    error.clear(); return true;
}

bool StreamPager_ConfigureLodSupplement(const NativePlacementIdentity& child,
                                        const NativePlacementIdentity& parent, std::string& error) {
    if (!s_init) {
        error = "LOD supplement requires initialized pager";
        return false;
    }
    if (!s_options.includeStreamed) {
        error = "LOD supplement requires pager includeStreamed=true";
        return false;
    }
    if (child == parent) {
        error = "LOD supplement child and parent must differ";
        return false;
    }
    int childPop = -1;
    int parentPop = -1;
    int childMatches = 0;
    int parentMatches = 0;
    for (size_t i = 0; i < s_collisionPopulation.Instances.size(); ++i) {
        const auto& p = s_collisionPopulation.Instances[i];
        if (child.Matches(p)) {
            ++childMatches;
            if (childPop < 0) {
                childPop = static_cast<int>(i);
            }
        }
        if (parent.Matches(p)) {
            ++parentMatches;
            if (parentPop < 0) {
                parentPop = static_cast<int>(i);
            }
        }
    }
    if (childMatches != 1 || childPop < 0) {
        error = "LOD supplement unknown/non-unique child identity";
        return false;
    }
    if (parentMatches != 1 || parentPop < 0) {
        error = "LOD supplement unknown/non-unique parent identity";
        return false;
    }
    const NativeCollisionPlacement& childPl = s_collisionPopulation.Instances[static_cast<size_t>(childPop)];
    const NativeCollisionPlacement& parentPl = s_collisionPopulation.Instances[static_cast<size_t>(parentPop)];
    if (childPl.Binary || parentPl.Binary) {
        error = "LOD supplement requires text placements";
        return false;
    }
    if (childPl.Ipl != parentPl.Ipl) {
        error = "LOD supplement child/parent IPL mismatch";
        return false;
    }
    if (childPl.Lod != static_cast<int>(parentPl.Record)) {
        error = "LOD supplement child Lod does not bind parent record";
        return false;
    }
    if (parentPl.Lod != -1) {
        error = "LOD supplement parent must be LOD root (Lod==-1)";
        return false;
    }
    if (StartsWithLod(childPl.Model)) {
        error = "LOD supplement child must not be LOD model";
        return false;
    }
    if (!StartsWithLod(parentPl.Model)) {
        error = "LOD supplement parent must be LOD model";
        return false;
    }
    int childRow = -1;
    bool parentInRender = false;
    for (size_t r = 0; r < s_insts.size(); ++r) {
        if (s_insts[r].order == childPop + 1) {
            if (childRow >= 0) {
                error = "LOD supplement duplicate child render row";
                return false;
            }
            childRow = static_cast<int>(r);
        }
        if (s_insts[r].order == parentPop + 1) {
            parentInRender = true;
        }
    }
    if (childRow < 0) {
        error = "LOD supplement child not in static render population";
        return false;
    }
    if (parentInRender) {
        error = "LOD supplement parent must be LOD-filtered (not in render population)";
        return false;
    }
    std::string childKey = childPl.Model;
    std::string parentKey = parentPl.Model;
    ToLowerInPlace(childKey);
    ToLowerInPlace(parentKey);
    const auto childIde = s_ide.find(childKey);
    if (childIde == s_ide.end()) {
        error = "LOD supplement child model missing from IDE";
        return false;
    }
    const auto parentIde = s_ide.find(parentKey);
    if (parentIde == s_ide.end()) {
        error = "LOD supplement parent model missing from IDE";
        return false;
    }
    for (float v : parentPl.Position) {
        if (!std::isfinite(v)) {
            error = "LOD supplement parent nonfinite position";
            return false;
        }
    }
    for (float v : parentPl.Quaternion) {
        if (!std::isfinite(v)) {
            error = "LOD supplement parent nonfinite rotation";
            return false;
        }
    }
    LodSupplementConfig cfg;
    cfg.child = child;
    cfg.parent = parent;
    cfg.childPopIdx = childPop;
    cfg.parentPopIdx = parentPop;
    cfg.childRow = childRow;
    cfg.childKey = std::move(childKey);
    cfg.parentKey = std::move(parentKey);
    cfg.parentTxd = parentIde->second.txd;
    cfg.parentModelId = parentPl.ModelId;
    cfg.parentOrder = parentPop + 1;
    cfg.parentPos[0] = parentPl.Position[0];
    cfg.parentPos[1] = parentPl.Position[1];
    cfg.parentPos[2] = parentPl.Position[2];
    float qx = parentPl.Quaternion[0];
    float qy = parentPl.Quaternion[1];
    float qz = parentPl.Quaternion[2];
    float qw = parentPl.Quaternion[3];
    if (s_options.includeStreamed) {
        qx = -qx;
        qy = -qy;
        qz = -qz;
    }
    cfg.parentQuat[0] = qx;
    cfg.parentQuat[1] = qy;
    cfg.parentQuat[2] = qz;
    cfg.parentQuat[3] = qw;
    s_supplement = std::move(cfg);
    error.clear();
    return true;
}

bool StreamPager_UpdateSelected(const std::vector<NativePlacementIdentity>& visible,
                                const std::vector<NativePlacementIdentity>& hiddenTargets,
                                WorldShotScene& scene, E2EPagerFrame& frame, char* err,
                                std::size_t errSize,
                                std::vector<NativePlacementIdentity>* rendered) {
    // Selected residency: exact catalog-backed window, no radius/cell/fallback/
    // truncation/LOD-prefix/interior filtering. Reuses the shared TXD-lineage,
    // DFF-cache, EmitPlacedMesh and eviction stages; strict whole-candidate
    // failure with the specific identity, never silent skip. Old Update,
    // non-streamed, cap and supplement semantics are untouched (this path only
    // reads s_supplement for the all-or-neither/duplicate guard).
    frame = E2EPagerFrame{};
    scene.meshes.clear();
    scene.images.clear();
    if (rendered) rendered->clear();
    if (!s_init) { SetErr(err, errSize, "pager not initialized"); return false; }
    if (!s_options.includeStreamed) {
        SetErr(err, errSize, "selected residency requires pager includeStreamed=true");
        return false;
    }
    if (visible.empty() && hiddenTargets.empty()) {
        SetErr(err, errSize, "no selected instances (empty residency)");
        return false;
    }

    auto formatIdentity = [](const NativePlacementIdentity& id) {
        std::string out = id.Ipl + ":" + std::to_string(id.Record) + " " + id.Model + " (" +
            std::to_string(id.ModelId) + ")";
        out += id.Binary ? " binary" : " text";
        return out;
    };
    auto failSelected = [&](const std::string& message) {
        SetErr(err, errSize, message.c_str());
        scene.meshes.clear();
        scene.images.clear();
        if (rendered) rendered->clear();
        frame = E2EPagerFrame{};
        return false;
    };

    // Ordered selected identities: visible then hidden. Rendered and scene
    // emission follow this exact order one-to-one.
    std::vector<NativePlacementIdentity> ordered;
    ordered.reserve(visible.size() + hiddenTargets.size());
    for (const auto& id : visible) ordered.push_back(id);
    for (const auto& id : hiddenTargets) ordered.push_back(id);

    // Duplicate input identities (within or across lists) fail whole.
    for (size_t i = 0; i < ordered.size(); ++i) {
        for (size_t j = 0; j < i; ++j) {
            if (ordered[i] == ordered[j]) {
                return failSelected("duplicate selected identity " + formatIdentity(ordered[i]));
            }
        }
    }

    // Unique 1:1 match against the full population.
    std::vector<int> popIdx;
    popIdx.reserve(ordered.size());
    for (const auto& id : ordered) {
        int found = -1;
        int matches = 0;
        for (size_t p = 0; p < s_collisionPopulation.Instances.size(); ++p) {
            if (id.Matches(s_collisionPopulation.Instances[p])) {
                ++matches;
                if (found < 0) found = static_cast<int>(p);
            }
        }
        if (matches != 1 || found < 0) {
            return failSelected(matches == 0 ? "unknown selected identity " + formatIdentity(id)
                                             : "non-unique selected identity " + formatIdentity(id));
        }
        popIdx.push_back(found);
    }
    // Distinct population rows (input duplicates already rejected, but guard
    // against aliased population rows).
    for (size_t i = 0; i < popIdx.size(); ++i) {
        for (size_t j = 0; j < i; ++j) {
            if (popIdx[i] == popIdx[j]) {
                return failSelected("duplicate selected identity " + formatIdentity(ordered[i]));
            }
        }
    }

    // Static validation: anim/Clump and missing IDE fail whole (no skip).
    // TimeAtomic is admitted like Atomic; interior/LOD-prefix are NOT skipped.
    for (size_t k = 0; k < ordered.size(); ++k) {
        const auto& pl = s_collisionPopulation.Instances[static_cast<size_t>(popIdx[k])];
        std::string key = pl.Model;
        ToLowerInPlace(key);
        if (s_animModels.find(key) != s_animModels.end()) {
            return failSelected("anim selected identity " + formatIdentity(ordered[k]));
        }
        if (s_ide.find(key) == s_ide.end()) {
            return failSelected("missing IDE for selected identity " + formatIdentity(ordered[k]));
        }
    }

    // A04 supplement guard: when its child is selected, the pair stays
    // all-or-neither without duplicate emission. The selected hidden list is
    // already expected to carry the parent; never emit a second copy here.
    if (s_supplement.has_value()) {
        const auto& sup = *s_supplement;
        bool childSelected = false;
        for (const auto& id : ordered) {
            if (id == sup.child) { childSelected = true; break; }
        }
        if (childSelected) {
            bool parentSelected = false;
            for (const auto& id : ordered) {
                if (id == sup.parent) { parentSelected = true; break; }
            }
            if (!parentSelected) {
                return failSelected("LOD supplement pair incomplete for selected child " +
                    formatIdentity(sup.child) + " missing parent " + formatIdentity(sup.parent));
            }
        }
    }

    // Exact want sets from BOTH lists (visible + hidden drive caches/eviction).
    std::map<std::string, int> wantModel;
    std::map<std::string, int> wantTxd;
    for (size_t k = 0; k < ordered.size(); ++k) {
        const auto& pl = s_collisionPopulation.Instances[static_cast<size_t>(popIdx[k])];
        std::string key = pl.Model;
        ToLowerInPlace(key);
        wantModel[key]++;
        auto iit = s_ide.find(key);
        if (iit == s_ide.end()) {
            return failSelected("missing IDE for selected identity " + formatIdentity(ordered[k]));
        }
        if (!iit->second.txd.empty()) {
            for (const auto& txd : BuildTxdNameChain(iit->second.txd).names) wantTxd[txd]++;
        }
    }

    // Shared wanted-resource stages (same implementation as legacy stages 4-6).
    // Selected mode is explicit fail-closed: any missing/failed/skinned/empty
    // model or incomplete TXD lineage fails the whole candidate with identity.
    // Failed selected output stays cleared with raw source geometry unchanged.
    PagerPageInTxds(wantTxd);
    {
        std::map<std::string, std::string> identityForModel;
        for (size_t k = 0; k < ordered.size(); ++k) {
            const auto& pl = s_collisionPopulation.Instances[static_cast<size_t>(popIdx[k])];
            std::string ck = pl.Model;
            ToLowerInPlace(ck);
            if (identityForModel.find(ck) == identityForModel.end()) {
                identityForModel[ck] = formatIdentity(ordered[k]);
            }
        }
        if (!PagerPageInModels(wantModel, true, &identityForModel, err, errSize)) {
            scene.meshes.clear();
            scene.images.clear();
            if (rendered) rendered->clear();
            frame = E2EPagerFrame{};
            return false;
        }
    }
    PagerEvictUnreferenced(wantModel, wantTxd);

    // Supplement pair must still be fully loadable when its child is selected
    // (all-or-neither); selected paging above already guarantees both sides or
    // fails whole, so this is only a defensive no-half guard.
    if (s_supplement.has_value()) {
        const auto& sup = *s_supplement;
        bool childSelected = false;
        for (const auto& id : ordered) {
            if (id == sup.child) { childSelected = true; break; }
        }
        if (childSelected) {
            const auto childIt = s_cache.find(sup.childKey);
            const auto parentIt = s_cache.find(sup.parentKey);
            const bool childOk = childIt != s_cache.end() && childIt->second.tris > 0 &&
                !childIt->second.pos.empty();
            const bool parentOk = parentIt != s_cache.end() && parentIt->second.tris > 0 &&
                !parentIt->second.pos.empty();
            if (!childOk || !parentOk) {
                return failSelected("LOD supplement pair not loadable for selected child " +
                    formatIdentity(sup.child));
            }
        }
    }

    // Shared stage: build the world-space scene in exact visible-then-hidden
    // order via EmitPlacedMesh (one-to-one with rendered).
    bool haveBox = false;
    int placed = 0;
    int tris = 0;
    std::set<std::string> usedModels;
    std::map<std::string, int> globalImg;
    std::map<NativeAssetIdentity::Texture, int> streamedGlobalImg;
    scene.images.clear();
    // ordered/popIdx are already visible-then-hidden; visible/hidden boundary
    // is preserved for the one-to-one rendered contract.
    for (size_t k = 0; k < ordered.size(); ++k) {
        const auto& identity = ordered[k];
        const auto& pl = s_collisionPopulation.Instances[static_cast<size_t>(popIdx[k])];
        std::string key = pl.Model;
        ToLowerInPlace(key);
        auto cit = s_cache.find(key);
        if (cit == s_cache.end()) {
            return failSelected("selected model geometry missing " + formatIdentity(identity));
        }
        const CachedModel& cached = cit->second;
        if (cached.tris <= 0 || cached.pos.empty()) {
            return failSelected("empty DFF for selected identity " + formatIdentity(identity));
        }
        float q[4] = {pl.Quaternion[0], pl.Quaternion[1], pl.Quaternion[2], pl.Quaternion[3]};
        // Streamed source stores the inverse rotation; conjugate once at binding
        // (same convention as Init for includeStreamed).
        q[0] = -q[0];
        q[1] = -q[1];
        q[2] = -q[2];
        float right[3], fwd[3], up[3];
        QuatToBasis(q, right, fwd, up);
        const float worldPos[3] = {pl.Position[0], pl.Position[1], pl.Position[2]};
        EmitPlacedMesh(cached, worldPos, right, fwd, up, pl.ModelId,
                       static_cast<uint32_t>(static_cast<size_t>(popIdx[k]) + 1), identity, placed,
                       scene, globalImg, streamedGlobalImg, haveBox, rendered);
        usedModels.insert(key);
        ++placed;
        tris += cached.tris;
    }
    if (placed == 0) {
        return failSelected("selected window has no loadable models (all failed)");
    }
    if (static_cast<int>(s_cache.size()) > s_modelsPeak) s_modelsPeak = static_cast<int>(s_cache.size());
    if (tris > s_trisPeak) s_trisPeak = tris;
    (void)std::snprintf(scene.stats.dffName, sizeof(scene.stats.dffName), "pager:%d", placed);
    (void)std::snprintf(scene.stats.txdName, sizeof(scene.stats.txdName), "multi:%d", s_texResident);
    scene.stats.atomics = placed;
    scene.stats.triangles = tris;
    scene.stats.vertices = tris * 3;
    scene.stats.textures = 0;
    scene.stats.firstTexture[0] = '\0';
    scene.stats.firstTexW = 0;
    scene.stats.firstTexH = 0;

    frame.instances = placed;
    frame.modelsUnique = static_cast<int>(usedModels.size());
    frame.tris = tris;
    frame.verts = tris * 3;
    frame.activeCells = static_cast<int>(s_active.size());
    frame.loadedCells = 0;
    frame.evictedCells = 0;
    frame.cacheModels = static_cast<int>(s_cache.size());
    frame.texDicts = s_texResident;
    frame.fallback = 0;
    frame.evictedShown = 0;
    return true;
}
