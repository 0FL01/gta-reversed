// CarPose implementation: steerable/spinnable car wheels (R6l).
// See CarPose.h for the contract. Own librw engine handle (same NULL
// parse-only plugin set as the other slices); one shot path per process, so
// no double Engine::init. IMG-index helpers are duplicated from SkinPed per
// the native-track precedent (no refactors of verified slices in a round).

#include "app/platform/linux/CarPose.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <array>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <memory>

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

void ToLowerInPlace(std::string& s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c + 32);
        }
    }
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
    void* file = nullptr;
    if (OS_FileOpen(FILE_DATA_AREA_DEFAULT, &file, imgRel, FILE_ACCESS_READ) != 0 || !file) {
        return false;
    }
    const int32 fileSize = OS_FileSize(file);
    std::array<uint8, 8> head{};
    OS_FileSetPosition(file, 0);
    if (fileSize < 8 || OS_FileRead(file, head.data(), 8) != 0 ||
        std::memcmp(head.data(), "VER2", 4) != 0) {
        OS_FileClose(file);
        return false;
    }
    uint32 count = 0;
    std::memcpy(&count, head.data() + 4, 4);
    if (count == 0 || count > 300000 || count > static_cast<uint32>(fileSize - 8) / 32u) {
        OS_FileClose(file);
        return false;
    }
    // Only the VER2 directory is needed; never load the archive body here.
    const int32 dirSize = static_cast<int32>(count * 32u);
    std::vector<uint8> directory(static_cast<size_t>(dirSize));
    const bool ok = OS_FileRead(file, directory.data(), dirSize) == 0;
    OS_FileClose(file);
    if (!ok) {
        return false;
    }
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
        const uint8* e = directory.data() + static_cast<size_t>(i) * 32;
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
    // Tolerant across slices in one process (DriveSim composes CarPose +
    // StreamPager, which register the identical plugin set): when another
    // slice already brought the engine up, reuse it instead of failing
    // (check first to avoid the librw RWERROR print on double init).
    if (rw::Engine::state != rw::Engine::Dead) {
        rw::Texture::setLoadTextures(false);
        s_rwInit = true;
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

static rw::TexDictionary* LoadVehicleTxd() {
    void* file = nullptr;
    if (OS_FileOpen(FILE_DATA_AREA_DEFAULT, &file, "models/generic/vehicle.txd", FILE_ACCESS_READ) != 0 || !file) {
        return nil;
    }
    const int32 size = OS_FileSize(file);
    std::vector<uint8> bytes(size > 0 ? static_cast<size_t>(size) : 0);
    const bool ok = size >= 12 && OS_FileRead(file, bytes.data(), size) == 0;
    OS_FileClose(file);
    return ok ? ParseTxd(bytes) : nil;
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

// GPU-only paint metadata. First authored car/car4 scheme is deterministic;
// legacy triCol stays exactly as stored in the DFF for offline fixtures.
static bool LoadPaint(const std::string& model, std::array<rw::RGBA, 4>& paint,
                      std::array<int, 4>& indices) {
    indices.fill(-1);
    void* file = nullptr;
    if (OS_FileOpen(FILE_DATA_AREA_DEFAULT, &file, "data/carcols.dat", FILE_ACCESS_READ) != 0 || !file) {
        return false;
    }
    const int32 size = OS_FileSize(file);
    std::string text(size > 0 ? static_cast<size_t>(size) : 0, '\0');
    const bool ok = size > 0 && OS_FileRead(file, text.data(), size) == 0;
    OS_FileClose(file);
    if (!ok) {
        return false;
    }
    std::vector<rw::RGBA> table;
    int section = 0, count = 0;
    for (size_t pos = 0; pos < text.size();) {
        const size_t end = text.find('\n', pos);
        std::string line = text.substr(pos, end == std::string::npos ? end : end - pos);
        pos = end == std::string::npos ? text.size() : end + 1;
        line.resize(line.find('#') == std::string::npos ? line.size() : line.find('#'));
        for (char& c : line) {
            if (c == ',' || c == '\r' || c == '\t') {
                c = ' ';
            }
        }
        char name[64]{};
        if (std::sscanf(line.c_str(), "%63s", name) != 1) {
            continue;
        }
        if (!std::strcmp(name, "col")) { section = 1; continue; }
        if (!std::strcmp(name, "car")) { section = 2; continue; }
        if (!std::strcmp(name, "car4")) { section = 4; continue; }
        if (!std::strcmp(name, "end")) { section = 0; continue; }
        if (section == 1) {
            int r, g, b;
            // CVehicleModelInfo::LoadVehicleColours FIX_BUGS handles the
            // original palette row 98 typo "77.93,96" the same way.
            if (std::sscanf(line.c_str(), "%d %d %d", &r, &g, &b) != 3 &&
                std::sscanf(line.c_str(), "%d.%d %d", &r, &g, &b) != 3) {
                return false;
            }
            if (r < 0 || r > 255 || g < 0 || g > 255 || b < 0 || b > 255) {
                return false;
            }
            table.push_back({static_cast<uint8>(r), static_cast<uint8>(g), static_cast<uint8>(b), 255});
        } else if ((section == 2 || section == 4) && model == name) {
            const int parsed = std::sscanf(line.c_str(), "%63s %d %d %d %d", name,
                &indices[0], &indices[1], &indices[2], &indices[3]);
            if (parsed < section + 1) {
                return false;
            }
            count = section;
            if (count == 2) {
                indices[2] = indices[3] = -1;
            }
        }
    }
    for (int i = 0; i < count; ++i) {
        if (indices[i] < 0 || indices[i] >= static_cast<int>(table.size())) {
            return false;
        }
        paint[i] = table[indices[i]];
    }
    return count != 0;
}

// --- Raw DFF framelist walk (librw keeps no frame names) ---
// Retail car DFFs name every frame through the NodeName extension
// (0x253F2FE) inside each frame's 0x03 extension chunk: verified byte-level
// on landstal (52 frames), elegy (48), zr350 (39) — wheel dummies are
// `wheel_{lf,rf,lb,rb}_dummy`, the stored wheel mesh frame is `wheel`.
struct RawFrame {
    float pos[3];
    int parent = -2;
    std::string name;
};

bool ParseFrameNames(const std::vector<uint8>& dff, std::vector<RawFrame>& out, char* err,
                     std::size_t errSize) {
    out.clear();
    auto fail = [&](const char* m) {
        SetErr(err, errSize, m);
        return false;
    };
    if (dff.size() < 24) {
        return fail("DFF too small for clump header");
    }
    auto rd32 = [&](size_t p, uint32& v) {
        if (p + 4 > dff.size()) {
            return false;
        }
        std::memcpy(&v, dff.data() + p, 4);
        return true;
    };
    uint32 type = 0, size = 0, ver = 0;
    if (!rd32(0, type) || !rd32(4, size) || !rd32(8, ver)) {
        return fail("DFF truncated at clump header");
    }
    if (type != 0x10) {
        return fail("DFF root is not a clump (type != 0x10)");
    }
    size_t pos = 12;
    // Skip the clump struct chunk (0x01).
    uint32 st = 0, ss = 0;
    if (!rd32(pos, st) || !rd32(pos + 4, ss)) {
        return fail("DFF truncated at clump struct");
    }
    if (st != 0x01) {
        return fail("DFF clump struct missing");
    }
    pos += 12 + ss;
    // Find the framelist chunk (0x0E) among the clump children.
    bool found = false;
    size_t flStart = 0, flSize = 0;
    while (pos + 12 <= dff.size()) {
        uint32 t = 0, s = 0;
        if (!rd32(pos, t) || !rd32(pos + 4, s)) {
            return fail("DFF truncated walking clump children");
        }
        if (t == 0x0E) {
            found = true;
            flStart = pos + 12;
            flSize = s;
            break;
        }
        if (s > dff.size()) {
            return fail("DFF child size out of range");
        }
        pos += 12 + s;
    }
    if (!found || flStart + flSize > dff.size()) {
        return fail("DFF framelist (0x0E) not found");
    }
    size_t p = flStart;
    uint32 fst = 0, fss = 0;
    if (!rd32(p, fst) || !rd32(p + 4, fss)) {
        return fail("DFF truncated at framelist struct");
    }
    if (fst != 0x01 || fss < 4) {
        return fail("DFF framelist struct missing");
    }
    uint32 num = 0;
    if (!rd32(p + 12, num)) {
        return fail("DFF truncated at frame count");
    }
    if (num == 0 || num > 10000) {
        return fail("DFF frame count out of range");
    }
    p += 12 + 4;
    if (p + static_cast<size_t>(num) * 56 > dff.size()) {
        return fail("DFF truncated in frame array");
    }
    out.resize(num);
    for (uint32 i = 0; i < num; ++i) {
        float v[12] = {};
        std::memcpy(v, dff.data() + p, 48);
        int32 par = 0;
        std::memcpy(&par, dff.data() + p + 48, 4);
        out[i].pos[0] = v[9];
        out[i].pos[1] = v[10];
        out[i].pos[2] = v[11];
        out[i].parent = par;
        p += 56;
    }
    // Per-frame extension chunks (0x03), each carrying one 0x253F2FE name.
    for (uint32 i = 0; i < num; ++i) {
        uint32 et = 0, es = 0;
        if (!rd32(p, et) || !rd32(p + 4, es)) {
            return fail("DFF truncated at frame extension");
        }
        if (et != 0x03 || p + 12 + es > dff.size()) {
            return fail("DFF frame extension malformed");
        }
        size_t ep = p + 12;
        // Nested NodeName chunk: type 0x253F2FE, size ns, then ns name bytes.
        uint32 nt = 0, ns = 0;
        if (es >= 12 && rd32(ep, nt) && rd32(ep + 4, ns) && nt == 0x253F2FE &&
            ep + 12 + ns <= p + 12 + es && ns > 0 && ns <= 64) {
            const char* bytes = reinterpret_cast<const char*>(dff.data() + ep + 12);
            size_t len = 0;
            while (len < ns && bytes[len] != '\0') {
                ++len;
            }
            out[i].name.assign(bytes, len);
        }
        p += 12 + es;
    }
    return true;
}

// Wheel-dummy classification from the stored name (case-insensitive,
// optional `_dummy` suffix accepted so all retail car DFFs match).
// Returns 0 = not a wheel dummy, 1 = front (steered), 2 = rear.
int ClassifyWheel(const std::string& lower) {
    std::string n = lower;
    const std::string suf = "_dummy";
    if (n.size() > suf.size() && n.compare(n.size() - suf.size(), suf.size(), suf) == 0) {
        n = n.substr(0, n.size() - suf.size());
    }
    if (n == "wheel_lf" || n == "wheel_rf") {
        return 1;
    }
    if (n == "wheel_lb" || n == "wheel_rb") {
        return 2;
    }
    return 0;
}

// Pair raw framelist indices to librw frames by hierarchy shape. librw
// prepends children (streamAppendFrames defaults to 0), so the next-chain
// runs in reverse file order; raw children ascending pair against the
// reversed chain. A local-position check guards every pair (DFF bytes only).
bool PairFrames(const std::vector<RawFrame>& raw, int rawIdx, rw::Frame* frame,
                std::vector<rw::Frame*>& byRaw, char* err, std::size_t errSize) {
    if (!frame) {
        SetErr(err, errSize, "librw hierarchy ended early while pairing frames");
        return false;
    }
    {
        float dx = frame->matrix.pos.x - raw[static_cast<size_t>(rawIdx)].pos[0];
        float dy = frame->matrix.pos.y - raw[static_cast<size_t>(rawIdx)].pos[1];
        float dz = frame->matrix.pos.z - raw[static_cast<size_t>(rawIdx)].pos[2];
        if (dx * dx + dy * dy + dz * dz > 1e-6f) {
            char msg[192];
            (void)std::snprintf(msg, sizeof(msg),
                                 "frame pairing mismatch at raw %d ('%s'): DFF/librw positions differ",
                                 rawIdx, raw[static_cast<size_t>(rawIdx)].name.c_str());
            SetErr(err, errSize, msg);
            return false;
        }
    }
    byRaw[static_cast<size_t>(rawIdx)] = frame;
    std::vector<int> rawKids;
    for (size_t i = 0; i < raw.size(); ++i) {
        if (raw[i].parent == rawIdx) {
            rawKids.push_back(static_cast<int>(i));
        }
    }
    std::vector<rw::Frame*> libKids;
    for (rw::Frame* c = frame->child; c; c = c->next) {
        libKids.push_back(c);
    }
    if (rawKids.size() != libKids.size()) {
        char msg[192];
        (void)std::snprintf(msg, sizeof(msg),
                             "frame pairing child-count mismatch at raw %d ('%s'): DFF=%d librw=%d",
                             rawIdx, raw[static_cast<size_t>(rawIdx)].name.c_str(),
                             static_cast<int>(rawKids.size()), static_cast<int>(libKids.size()));
        SetErr(err, errSize, msg);
        return false;
    }
    // Reverse the prepend-ordered chain back to file order.
    for (size_t k = 0; k < rawKids.size(); ++k) {
        rw::Frame* kid = libKids[libKids.size() - 1 - k];
        if (!PairFrames(raw, rawKids[k], kid, byRaw, err, errSize)) {
            return false;
        }
    }
    return true;
}

void LtmTo12(const rw::Matrix& m, float* v) {
    v[0] = m.right.x;
    v[1] = m.right.y;
    v[2] = m.right.z;
    v[3] = m.up.x;
    v[4] = m.up.y;
    v[5] = m.up.z;
    v[6] = m.at.x;
    v[7] = m.at.y;
    v[8] = m.at.z;
    v[9] = m.pos.x;
    v[10] = m.pos.y;
    v[11] = m.pos.z;
}

std::vector<rw::TexDictionary*> s_txds;

} // namespace

bool CarPose_Init(const char* gameDir, const char* model, double steerDeg, double spinDeg,
                  WorldShotScene& scene, CarPoseStats& stats, CarPoseAudit& audit, char* err,
                  std::size_t errSize, CarPoseTextures textures, CarPoseComponents components) {
    stats = CarPoseStats{};
    audit = CarPoseAudit{};
    scene.meshes.clear();
    scene.images.clear();
    if (!gameDir || !gameDir[0]) {
        SetErr(err, errSize, "no game dir");
        return false;
    }
    if (!std::isfinite(steerDeg) || !std::isfinite(spinDeg)) {
        SetErr(err, errSize, "non-finite steer/spin angle");
        return false;
    }
    stats.steerDeg = steerDeg;
    stats.spinDeg = spinDeg;
    std::string want = model && model[0] ? model : "landstal";
    (void)std::snprintf(stats.requested, sizeof(stats.requested), "%s", want.c_str());
    std::string wantLower = want;
    ToLowerInPlace(wantLower);
    std::string game(gameDir);
    OS_SetFilePathOffset(game.c_str());
    s_gameAbs = game;

    static const char* kImgs[] = { "models/gta3.img", "models/gta_int.img" };
    std::vector<ImgIndex> imgs;
    for (const char* rel : kImgs) {
        ImgIndex idx;
        if (BuildImgIndex(rel, idx)) {
            imgs.push_back(std::move(idx));
        }
    }
    if (imgs.empty()) {
        SetErr(err, errSize, "no IMG archive indexed (models/gta3.img)");
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

    // --- 1. Direct lookup: <model>.dff (cars live in gta3.img). ---
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
        char msg[256];
        (void)std::snprintf(msg, sizeof(msg), "car DFF '%s' not found in gta3/gta_int.img",
                             dffFile.c_str());
        SetErr(err, errSize, msg);
        return false;
    }
    {
        bool named = false;
        for (const ImgEntry& e : hitImg->entries) {
            if (e.nameLower == dffFile) {
                (void)std::snprintf(stats.src, sizeof(stats.src), "%s:%s", hitImg->label.c_str(),
                                     e.name.c_str());
                named = true;
                break;
            }
        }
        if (!named) {
            (void)std::snprintf(stats.src, sizeof(stats.src), "%s:%s", hitImg->label.c_str(),
                                 dffFile.c_str());
        }
    }
    (void)std::snprintf(stats.model, sizeof(stats.model), "%s", resolved.c_str());

    // --- 2. Per-model TXD: <base>.txd next to the DFF. ---
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

    // --- 3. Parse with honest material linkage. ---
    // The common dictionary lives through linking and RGBA flattening, including
    // every error path. Scene images own their texels, never these RW pointers.
    const auto destroyTxd = [](rw::TexDictionary* txd) { if (txd) txd->destroy(); };
    std::unique_ptr<rw::TexDictionary, decltype(destroyTxd)> shared(nil, destroyTxd);
    if (textures == CarPoseTextures::RealtimeVehicle) {
        shared.reset(LoadVehicleTxd());
        if (!shared || shared->count() == 0) {
            SetErr(err, errSize, "cannot load models/generic/vehicle.txd for realtime vehicle");
            return false;
        }
        stats.sharedTextures = shared->count();
        stats.textures += stats.sharedTextures;
    }
    rw::TexDictionary* primary = dicts.empty() ? nil : dicts[0];
    LinkedClump lc = TexSample_LinkedParse(dffBytes.data(), dffBytes.size(), primary, nil, 0, shared.get());
    if (!lc.clump) {
        TexSample_FreeLinked(lc);
        SetErr(err, errSize, "DFF parse produced no clump (not a RenderWare clump?)");
        return false;
    }
    rw::Frame* root = lc.clump->getFrame();
    if (!root || root->getParent()) {
        TexSample_FreeLinked(lc);
        SetErr(err, errSize, "clump root frame missing");
        return false;
    }
    stats.frames = root->count();

    // --- 4. Raw frame names + pairing to librw frames. ---
    std::vector<RawFrame> raw;
    if (!ParseFrameNames(dffBytes, raw, err, errSize)) {
        TexSample_FreeLinked(lc);
        return false;
    }
    if (static_cast<int>(raw.size()) != stats.frames) {
        TexSample_FreeLinked(lc);
        char msg[192];
        (void)std::snprintf(msg, sizeof(msg),
                             "framelist count %d != librw frame count %d",
                             static_cast<int>(raw.size()), stats.frames);
        SetErr(err, errSize, msg);
        return false;
    }
    std::vector<rw::Frame*> byRaw(raw.size(), nil);
    if (!PairFrames(raw, 0, root, byRaw, err, errSize)) {
        TexSample_FreeLinked(lc);
        return false;
    }

    const bool pristine = components.geometry == CarPoseGeometry::PristineNear ||
        (components.geometry == CarPoseGeometry::FromTextureMode && textures == CarPoseTextures::RealtimeVehicle);
    std::map<rw::Frame*, const RawFrame*> frameInfo;
    for (size_t i = 0; i < raw.size(); ++i) {
        frameInfo[byRaw[i]] = &raw[i];
    }
    std::vector<rw::Atomic*> extras;
    rw::Frame* extraParent = root;
    if (pristine) {
        // CVehicleModelInfo::PreprocessHierarchy removes the first object of
        // each descriptor extra frame; CreateInstance clones only the chosen
        // slots. Names come from the NodeName plugin, not material/mesh guesses.
        // CVisibilityPlugins' ATOMIC_OK/DAMAGED flags are runtime flags (its
        // constructor initializes them to zero); they are not stored DFF flags.
        for (size_t i = 0; i < raw.size(); ++i) {
            if (raw[i].name == "chassis_dummy") extraParent = byRaw[i];
        }
        for (const char* name : {"extra1", "extra2", "extra3", "extra4", "extra5", "extra6"}) {
            for (size_t i = 0; i < raw.size(); ++i) {
                if (raw[i].name != name) continue;
                FORLIST(link, byRaw[i]->objectList) {
                    auto* object = rw::ObjectWithFrame::fromFrame(link);
                    if (object->object.type == rw::Atomic::ID) extras.push_back(reinterpret_cast<rw::Atomic*>(object));
                    break; // GetFirstObject, as upstream
                }
                break;
            }
        }
        stats.extrasAvailable = static_cast<int>(extras.size());
        for (int extra : components.extras) {
            if (extra < -1 || extra >= stats.extrasAvailable) {
                TexSample_FreeLinked(lc);
                SetErr(err, errSize, "forced vehicle extra index outside available DFF components");
                return false;
            }
        }
    }
    const auto visible = [&](rw::Atomic* atomic) {
        if (!pristine) return true;
        const auto& name = frameInfo.at(atomic->getFrame())->name;
        // HideDamagedAtomicCB (0x4C7720): strstr, case-sensitive, on the
        // atomic's own frame. _ok tags the intact atomic, it does not force
        // rpATOMICRENDER back on. Respect the serialized RenderWare flags too.
        if (name.find("_dam") != std::string::npos) {
            ++stats.damagedAtomicsSkipped;
            return false;
        }
        if (!(atomic->getFlags() & rw::Atomic::RENDER)) {
            ++stats.nonRenderAtomicsSkipped;
            return false;
        }
        // SetAtomicRendererCB assigns _vlo the really-low-detail callback;
        // RenderVehicleReallyLowDetailCB and HiDetailCB are distance-exclusive.
        // This actor cache is the near-detail representation, not both LODs.
        if (name.find("_vlo") != std::string::npos) {
            ++stats.lodAtomicsSkipped;
            return false;
        }
        return true;
    };

    // --- 5. Wheel dummies by stored name; body audit frame ("chassis"). ---
    struct Wheel {
        int raw = -1;
        rw::Frame* frame = nil;
        bool front = false;
        std::string name;
    };
    std::vector<Wheel> wheels;
    for (size_t i = 0; i < raw.size(); ++i) {
        std::string low = raw[i].name;
        ToLowerInPlace(low);
        int cls = ClassifyWheel(low);
        if (cls != 0) {
            Wheel w;
            w.raw = static_cast<int>(i);
            w.frame = byRaw[i];
            w.front = cls == 1;
            w.name = raw[i].name;
            wheels.push_back(w);
        }
    }
    if (wheels.empty()) {
        TexSample_FreeLinked(lc);
        char msg[192];
        (void)std::snprintf(msg, sizeof(msg),
                             "no wheel dummy frames (wheel_{lf,rf,lb,rb}[_dummy]) in '%.127s'", stats.src);
        SetErr(err, errSize, msg);
        return false;
    }
    std::set<rw::Frame*> wheelSet;
    for (const Wheel& w : wheels) {
        wheelSet.insert(w.frame);
    }
    int fronts = 0;
    for (const Wheel& w : wheels) {
        fronts += w.front ? 1 : 0;
    }
    stats.wheels = static_cast<int>(wheels.size());
    stats.fronts = fronts;
    for (size_t i = 0; i < wheels.size() && i < 4; ++i) {
        (void)std::snprintf(stats.wheelNames[i], sizeof(stats.wheelNames[i]), "%s",
                             wheels[i].name.c_str());
    }
    // Body audit frame: the stored `chassis` node when present, else root.
    rw::Frame* auditBody = root;
    std::string auditBodyName = raw[0].name;
    for (size_t i = 0; i < raw.size(); ++i) {
        std::string low = raw[i].name;
        ToLowerInPlace(low);
        if (low == "chassis") {
            auditBody = byRaw[i];
            auditBodyName = raw[i].name;
            break;
        }
    }

    // --- 6. Wheel kit: atomics living in a wheel-dummy subtree (the stored
    // `wheel` mesh the game clones onto every wheel frame). Everything else
    // is body and must stay byte-identical through the pose. ---
    struct KitGeom {
        rw::Geometry* geo = nil;
        rw::Frame* frame = nil; // its stored atomic frame
        rw::Frame* owner = nil; // wheel dummy above it
    };
    std::vector<KitGeom> kit;
    std::vector<rw::Atomic*> bodyAtomics;
    FORLIST(link, lc.clump->atomics) {
        rw::Atomic* atomic = rw::Atomic::fromClump(link);
        if (pristine) {
            bool extra = false;
            for (auto* candidate : extras) extra |= candidate == atomic;
            if (extra || !visible(atomic)) continue;
        }
        rw::Geometry* geo = atomic ? atomic->geometry : nil;
        if (!geo || geo->numTriangles <= 0 || geo->numVertices <= 0) {
            continue;
        }
        if (!geo->triangles || !geo->morphTargets || !geo->morphTargets[0].vertices) {
            continue; // native-only geometry: nothing CPU-readable to draw
        }
        if (rw::Skin::get(geo)) {
            continue; // cars are unskinned; never skin here
        }
        rw::Frame* af = atomic->getFrame();
        rw::Frame* owner = nil;
        for (rw::Frame* f = af; f; f = f->getParent()) {
            if (wheelSet.count(f)) {
                owner = f;
                break;
            }
        }
        if (owner) {
            KitGeom k;
            k.geo = geo;
            k.frame = af;
            k.owner = owner;
            kit.push_back(k);
        } else {
            bodyAtomics.push_back(atomic);
        }
    }
    if (pristine) {
        for (int extra : components.extras) {
            if (extra < 0 || !visible(extras[extra])) continue;
            bodyAtomics.push_back(extras[extra]);
            ++stats.extrasSelected;
        }
    }
    if (kit.empty()) {
        TexSample_FreeLinked(lc);
        SetErr(err, errSize, "wheel dummies carry no geometry (no stored wheel mesh to pose)");
        return false;
    }
    if (bodyAtomics.empty()) {
        TexSample_FreeLinked(lc);
        SetErr(err, errSize, "no body geometry outside the wheel subtrees");
        return false;
    }

    // --- 7. Bind snapshots, then the pose: extra local matrices over the
    // DFF transforms, angles only from argv (librw takes degrees). ---
    auto snapLtm = [](rw::Frame* f, float* v) {
        rw::Matrix* ltm = f ? f->getLTM() : nil;
        if (ltm) {
            LtmTo12(*ltm, v);
        } else {
            for (int i = 0; i < 12; ++i) {
                v[i] = 0.0f;
            }
        }
    };
    float auditBefore[12], bodyBefore[12];
    snapLtm(wheels[0].frame, auditBefore);
    snapLtm(auditBody, bodyBefore);
    // Wheel-relative matrices R_k = inv(ownerBind) * wheelFrameBind, so each
    // kit mesh instances onto any dummy as M = dummyPosed * R_k (the retail
    // RpAtomicClone step, computed from DFF bytes, not invented).
    std::vector<rw::Matrix> kitRel(kit.size());
    for (size_t k = 0; k < kit.size(); ++k) {
        rw::Matrix* ownerLtm = kit[k].owner->getLTM();
        rw::Matrix* wheelLtm = kit[k].frame->getLTM();
        if (!ownerLtm || !wheelLtm) {
            TexSample_FreeLinked(lc);
            SetErr(err, errSize, "missing bind LTM for wheel-relative matrix");
            return false;
        }
        rw::Matrix invOwner;
        rw::Matrix::invert(&invOwner, ownerLtm);
        rw::Matrix::mult(&kitRel[k], &invOwner, wheelLtm);
    }
    const rw::V3d zAxis = rw::makeV3d(0.0f, 0.0f, 1.0f); // steer yaw (up)
    const rw::V3d xAxis = rw::makeV3d(1.0f, 0.0f, 0.0f); // spin roll (axle)
    // PRECONCAT so each rotation pivots about the dummy's own center (the
    // DFF local is a pure translation): v * (Rx * Rz * T) spins the mesh
    // about its axle first, then yaws the whole assembly, then translates.
    // POSTCONCAT would orbit the wheel around the car root instead (caught
    // by the audit: the dummy position must not move).
    for (const Wheel& w : wheels) {
        if (w.front && steerDeg != 0.0) {
            w.frame->rotate(&zAxis, static_cast<float>(steerDeg), rw::COMBINEPRECONCAT);
        }
        if (spinDeg != 0.0) {
            w.frame->rotate(&xAxis, static_cast<float>(spinDeg), rw::COMBINEPRECONCAT);
        }
    }
    float auditAfter[12], bodyAfter[12];
    snapLtm(wheels[0].frame, auditAfter);
    snapLtm(auditBody, bodyAfter);
    audit.bodySame = std::memcmp(bodyBefore, bodyAfter, sizeof(bodyBefore)) == 0 ? 1 : 0;
    stats.chassisSame = audit.bodySame;
    (void)std::snprintf(audit.wheel, sizeof(audit.wheel), "%s", wheels[0].name.c_str());
    (void)std::snprintf(audit.body, sizeof(audit.body), "%s", auditBodyName.c_str());
    for (int i = 0; i < 12; ++i) {
        audit.before[i] = auditBefore[i];
        audit.after[i] = auditAfter[i];
    }

    // --- 8. Flatten: body through untouched LTMs, kit instanced per dummy. ---
    int meshIndex = 0;
    bool first = true;
    int totalTris = 0;
    int bodyTris = 0;
    std::array<rw::RGBA, 4> paint{};
    std::array<int, 4> paintIndices{};
    const bool hasPaint = LoadPaint(wantLower, paint, paintIndices);
    std::map<const rw::Texture*, int> imgCache;
    auto emitTris = [&](rw::Geometry* geo, const rw::Matrix& m, int& meshTris, bool& ok) {
        ok = true;
        const int numVerts = geo->numVertices;
        rw::V3d* verts = geo->morphTargets[0].vertices;
        rw::V3d* norms = (geo->flags & rw::Geometry::NORMALS) ? geo->morphTargets[0].normals : nil;
        rw::TexCoords* uvs = geo->texCoords[0];
        std::vector<rw::V3d> wv(static_cast<size_t>(numVerts));
        std::vector<rw::V3d> wn(norms ? static_cast<size_t>(numVerts) : 0);
        rw::V3d::transformPoints(wv.data(), verts, numVerts, &m);
        if (norms) {
            rw::V3d::transformVectors(wn.data(), norms, numVerts, &m);
        }
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
            const rw::V3d* pp[3] = { &wv[tri.v[0]], &wv[tri.v[1]], &wv[tri.v[2]] };
            WorldShotSurface surface;
            const bool lit = geo->flags & rw::Geometry::LIGHT;
            surface.ambient = lit && mat ? mat->surfaceProps.ambient : 0.0f;
            surface.diffuse = lit && norms && mat ? mat->surfaceProps.diffuse : 0.0f;
            if (mat) {
                auto color = mat->color;
                // CVehicleModelInfo::SetEditableMaterialsCB, RGB markers.
                const uint32 rgb = uint32(color.red) | (uint32(color.green) << 8) | (uint32(color.blue) << 16);
                const int slot = rgb == 0x00FF3C ? 0 : rgb == 0xAF00FF ? 1 :
                                 rgb == 0xFFFF00 ? 2 : rgb == 0xFF00FF ? 3 : -1;
                if (slot >= 0 && hasPaint && paintIndices[slot] >= 0) {
                    color.red = paint[slot].red;
                    color.green = paint[slot].green;
                    color.blue = paint[slot].blue;
                    surface.vehicleColorIndex = paintIndices[slot];
                }
                surface.color = {color.red / 255.0f, color.green / 255.0f,
                                 color.blue / 255.0f, color.alpha / 255.0f};
            }
            mesh.surfaces.push_back(surface);
            float face[3];
            {
                float a[3] = { pp[0]->x, pp[0]->y, pp[0]->z };
                float b[3] = { pp[1]->x, pp[1]->y, pp[1]->z };
                float c[3] = { pp[2]->x, pp[2]->y, pp[2]->z };
                CrossSub(a, b, c, face);
            }
            for (int kk = 0; kk < 3; ++kk) {
                const rw::RGBA day = geo->colors ? geo->colors[tri.v[kk]] :
                    lit ? rw::RGBA{0, 0, 0, 255} : rw::RGBA{255, 255, 255, 255};
                const uint8 rgba[]{day.red, day.green, day.blue, day.alpha};
                mesh.dayColors.insert(mesh.dayColors.end(), rgba, rgba + 4);
                mesh.pos.push_back(pp[kk]->x);
                mesh.pos.push_back(pp[kk]->y);
                mesh.pos.push_back(pp[kk]->z);
                if (norms) {
                    const rw::V3d& nn = wn[tri.v[kk]];
                    float len = std::sqrt(nn.x * nn.x + nn.y * nn.y + nn.z * nn.z);
                    if (len > 1e-9f) {
                        mesh.nrm.push_back(nn.x / len);
                        mesh.nrm.push_back(nn.y / len);
                        mesh.nrm.push_back(nn.z / len);
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
                    mesh.uv.push_back(uvs[tri.v[kk]].u);
                    mesh.uv.push_back(uvs[tri.v[kk]].v);
                } else {
                    mesh.uv.push_back(0.0f);
                    mesh.uv.push_back(0.0f);
                }
                if (first) {
                    scene.bboxMin[0] = scene.bboxMax[0] = pp[kk]->x;
                    scene.bboxMin[1] = scene.bboxMax[1] = pp[kk]->y;
                    scene.bboxMin[2] = scene.bboxMax[2] = pp[kk]->z;
                    first = false;
                } else {
                    if (pp[kk]->x < scene.bboxMin[0]) {
                        scene.bboxMin[0] = pp[kk]->x;
                    }
                    if (pp[kk]->y < scene.bboxMin[1]) {
                        scene.bboxMin[1] = pp[kk]->y;
                    }
                    if (pp[kk]->z < scene.bboxMin[2]) {
                        scene.bboxMin[2] = pp[kk]->z;
                    }
                    if (pp[kk]->x > scene.bboxMax[0]) {
                        scene.bboxMax[0] = pp[kk]->x;
                    }
                    if (pp[kk]->y > scene.bboxMax[1]) {
                        scene.bboxMax[1] = pp[kk]->y;
                    }
                    if (pp[kk]->z > scene.bboxMax[2]) {
                        scene.bboxMax[2] = pp[kk]->z;
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
            ok = false;
            return;
        }
        meshTris = mesh.tris;
        scene.meshes.push_back(std::move(mesh));
        ++meshIndex;
    };

    int geoms = 0;
    for (rw::Atomic* atomic : bodyAtomics) {
        rw::Geometry* geo = atomic->geometry;
        rw::Frame* af = atomic->getFrame();
        rw::Matrix* ltm = af ? af->getLTM() : nil;
        rw::Matrix m;
        if (ltm) {
            m = *ltm;
        } else {
            m.setIdentity();
        }
        if (pristine) {
            for (auto* extra : extras) {
                if (extra == atomic) {
                    // CreateInstance copies the extra's LOCAL matrix onto a
                    // new frame under CAR_CHASSIS, not the original parent LTM.
                    rw::Matrix::mult(&m, &af->matrix, extraParent->getLTM());
                    break;
                }
            }
        }
        int got = 0;
        bool ok = false;
        emitTris(geo, m, got, ok);
        if (ok) {
            totalTris += got;
            bodyTris += got;
            ++geoms;
        }
    }
    int kitTris = 0;
    for (const Wheel& w : wheels) {
        rw::Matrix* dummyLtm = w.frame->getLTM();
        if (!dummyLtm) {
            TexSample_FreeLinked(lc);
            SetErr(err, errSize, "missing posed wheel-dummy LTM");
            return false;
        }
        for (size_t k = 0; k < kit.size(); ++k) {
            rw::Matrix m;
            rw::Matrix::mult(&m, &kitRel[k], dummyLtm);
            int got = 0;
            bool ok = false;
            emitTris(kit[k].geo, m, got, ok);
            if (ok) {
                totalTris += got;
                if (w.raw == wheels[0].raw) {
                    kitTris += got;
                }
            }
        }
    }
    TexSample_FreeLinked(lc);
    if (totalTris <= 0 || bodyTris <= 0) {
        char msg[192];
        (void)std::snprintf(msg, sizeof(msg), "no car triangles flattened from '%.127s'",
                             stats.src);
        SetErr(err, errSize, msg);
        return false;
    }
    stats.tris = totalTris;
    stats.verts = totalTris * 3;
    stats.bodyTris = bodyTris;
    stats.wheelTris = kitTris;
    stats.geoms = geoms;
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

void CarPose_Shutdown() {
    for (rw::TexDictionary* t : s_txds) {
        if (t) {
            t->destroy();
        }
    }
    s_txds.clear();
}

bool CarPose_Measure(const char* gameDir, const char* model, CarPoseMeasure& out, char* err,
                     std::size_t errSize) {
    out = CarPoseMeasure{};
    if (!gameDir || !gameDir[0]) {
        SetErr(err, errSize, "no game dir");
        return false;
    }
    std::string want = model && model[0] ? model : "landstal";
    std::string wantLower = want;
    ToLowerInPlace(wantLower);
    std::string game(gameDir);
    OS_SetFilePathOffset(game.c_str());
    s_gameAbs = game;

    static const char* kImgs[] = { "models/gta3.img", "models/gta_int.img" };
    std::vector<ImgIndex> imgs;
    for (const char* rel : kImgs) {
        ImgIndex idx;
        if (BuildImgIndex(rel, idx)) {
            imgs.push_back(std::move(idx));
        }
    }
    if (imgs.empty()) {
        SetErr(err, errSize, "no IMG archive indexed (models/gta3.img)");
        return false;
    }
    if (!RwInitEngine()) {
        SetErr(err, errSize, "librw Engine::init failed");
        return false;
    }
    std::vector<uint8> dffBytes;
    std::string dffFile = wantLower + ".dff";
    const ImgIndex* hitImg = nil;
    for (const ImgIndex& idx : imgs) {
        if (ImgReadBytesStd(idx, dffFile, dffBytes)) {
            hitImg = &idx;
            break;
        }
    }
    if (dffBytes.empty()) {
        char msg[256];
        (void)std::snprintf(msg, sizeof(msg), "car DFF '%s' not found in gta3/gta_int.img",
                             dffFile.c_str());
        SetErr(err, errSize, msg);
        return false;
    }
    {
        bool named = false;
        for (const ImgEntry& e : hitImg->entries) {
            if (e.nameLower == dffFile) {
                (void)std::snprintf(out.src, sizeof(out.src), "%s:%s", hitImg->label.c_str(),
                                     e.name.c_str());
                named = true;
                break;
            }
        }
        if (!named) {
            (void)std::snprintf(out.src, sizeof(out.src), "%s:%s", hitImg->label.c_str(),
                                 dffFile.c_str());
        }
    }
    (void)std::snprintf(out.model, sizeof(out.model), "%s", wantLower.c_str());

    LinkedClump lc = TexSample_LinkedParse(dffBytes.data(), dffBytes.size(), nil, nil, 0);
    if (!lc.clump) {
        TexSample_FreeLinked(lc);
        SetErr(err, errSize, "DFF parse produced no clump (not a RenderWare clump?)");
        return false;
    }
    rw::Frame* root = lc.clump->getFrame();
    if (!root || root->getParent()) {
        TexSample_FreeLinked(lc);
        SetErr(err, errSize, "clump root frame missing");
        return false;
    }
    std::vector<RawFrame> raw;
    if (!ParseFrameNames(dffBytes, raw, err, errSize)) {
        TexSample_FreeLinked(lc);
        return false;
    }
    if (static_cast<int>(raw.size()) != root->count()) {
        TexSample_FreeLinked(lc);
        SetErr(err, errSize, "framelist count != librw frame count");
        return false;
    }
    std::vector<rw::Frame*> byRaw(raw.size(), nil);
    if (!PairFrames(raw, 0, root, byRaw, err, errSize)) {
        TexSample_FreeLinked(lc);
        return false;
    }
    struct Wheel {
        int raw = -1;
        rw::Frame* frame = nil;
        bool front = false;
        std::string name;
    };
    std::vector<Wheel> wheels;
    for (size_t i = 0; i < raw.size(); ++i) {
        std::string low = raw[i].name;
        ToLowerInPlace(low);
        int cls = ClassifyWheel(low);
        if (cls != 0) {
            Wheel w;
            w.raw = static_cast<int>(i);
            w.frame = byRaw[i];
            w.front = cls == 1;
            w.name = raw[i].name;
            wheels.push_back(w);
        }
    }
    if (wheels.empty()) {
        TexSample_FreeLinked(lc);
        SetErr(err, errSize, "no wheel dummy frames in DFF");
        return false;
    }
    std::set<rw::Frame*> wheelSet;
    for (const Wheel& w : wheels) {
        wheelSet.insert(w.frame);
    }
    // Wheelbase from dummy Y positions (DFF bytes, car +Y forward).
    double frontSum = 0.0, rearSum = 0.0;
    int frontN = 0, rearN = 0;
    for (const Wheel& w : wheels) {
        double y = raw[static_cast<size_t>(w.raw)].pos[1];
        if (w.front) {
            frontSum += y;
            ++frontN;
        } else {
            rearSum += y;
            ++rearN;
        }
    }
    if (frontN == 0 || rearN == 0) {
        TexSample_FreeLinked(lc);
        SetErr(err, errSize, "need front and rear wheel dummies for wheelbase");
        return false;
    }
    out.frontY = frontSum / frontN;
    out.rearY = rearSum / rearN;
    out.wheelbase = out.frontY - out.rearY;
    if (!(out.wheelbase > 0.1 && out.wheelbase < 20.0)) {
        TexSample_FreeLinked(lc);
        SetErr(err, errSize, "wheelbase out of range");
        return false;
    }
    // Kit geoms: atomics inside a wheel-dummy subtree (same rule as Init).
    struct KitGeom {
        rw::Geometry* geo = nil;
        rw::Frame* frame = nil;
        rw::Frame* owner = nil;
    };
    std::vector<KitGeom> kit;
    FORLIST(link, lc.clump->atomics) {
        rw::Atomic* atomic = rw::Atomic::fromClump(link);
        rw::Geometry* geo = atomic ? atomic->geometry : nil;
        if (!geo || geo->numTriangles <= 0 || geo->numVertices <= 0) {
            continue;
        }
        if (!geo->triangles || !geo->morphTargets || !geo->morphTargets[0].vertices) {
            continue;
        }
        if (rw::Skin::get(geo)) {
            continue;
        }
        rw::Frame* af = atomic->getFrame();
        rw::Frame* owner = nil;
        for (rw::Frame* f = af; f; f = f->getParent()) {
            if (wheelSet.count(f)) {
                owner = f;
                break;
            }
        }
        if (owner) {
            KitGeom k;
            k.geo = geo;
            k.frame = af;
            k.owner = owner;
            kit.push_back(k);
        }
    }
    if (kit.empty()) {
        TexSample_FreeLinked(lc);
        SetErr(err, errSize, "wheel dummies carry no geometry");
        return false;
    }
    // Relative matrices R_k = inv(ownerBind) * wheelFrameBind (DFF bytes).
    std::vector<rw::Matrix> kitRel(kit.size());
    for (size_t k = 0; k < kit.size(); ++k) {
        rw::Matrix* ownerLtm = kit[k].owner->getLTM();
        rw::Matrix* wheelLtm = kit[k].frame->getLTM();
        if (!ownerLtm || !wheelLtm) {
            TexSample_FreeLinked(lc);
            SetErr(err, errSize, "missing bind LTM for wheel-relative matrix");
            return false;
        }
        rw::Matrix invOwner;
        rw::Matrix::invert(&invOwner, ownerLtm);
        rw::Matrix::mult(&kitRel[k], &invOwner, wheelLtm);
    }
    // Per-wheel car-space bounds in bind pose (dummyBind * R_k).
    double minZAll = 0.0;
    bool haveZ = false;
    double firstMinY = 0.0, firstMaxY = 0.0, firstMinZ = 0.0, firstMaxZ = 0.0;
    bool haveFirst = false;
    for (size_t wi = 0; wi < wheels.size(); ++wi) {
        rw::Matrix* dummyLtm = wheels[wi].frame->getLTM();
        if (!dummyLtm) {
            TexSample_FreeLinked(lc);
            SetErr(err, errSize, "missing wheel-dummy bind LTM");
            return false;
        }
        for (size_t k = 0; k < kit.size(); ++k) {
            rw::Matrix m;
            rw::Matrix::mult(&m, &kitRel[k], dummyLtm);
            rw::Geometry* geo = kit[k].geo;
            int nv = geo->numVertices;
            rw::V3d* verts = geo->morphTargets[0].vertices;
            std::vector<rw::V3d> wv(static_cast<size_t>(nv));
            rw::V3d::transformPoints(wv.data(), verts, nv, &m);
            for (int vi = 0; vi < nv; ++vi) {
                double y = wv[static_cast<size_t>(vi)].y;
                double z = wv[static_cast<size_t>(vi)].z;
                if (!haveZ) {
                    minZAll = z;
                    haveZ = true;
                } else if (z < minZAll) {
                    minZAll = z;
                }
                if (wi == 0 && k == 0) {
                    if (!haveFirst) {
                        firstMinY = firstMaxY = y;
                        firstMinZ = firstMaxZ = z;
                        haveFirst = true;
                    } else {
                        if (y < firstMinY) {
                            firstMinY = y;
                        }
                        if (y > firstMaxY) {
                            firstMaxY = y;
                        }
                        if (z < firstMinZ) {
                            firstMinZ = z;
                        }
                        if (z > firstMaxZ) {
                            firstMaxZ = z;
                        }
                    }
                }
            }
        }
        // For wheels beyond the first, still extend the first-wheel R only
        // from wheel 0 (identical clone); minZAll spans all wheels.
    }
    TexSample_FreeLinked(lc);
    if (!haveZ || !haveFirst) {
        SetErr(err, errSize, "no wheel vertices measured");
        return false;
    }
    // Wheel spins about the X axle, so the rolling circle lies in the YZ
    // plane: radius = max(Y extent, Z extent) / 2 (DFF bytes only).
    double extY = firstMaxY - firstMinY;
    double extZ = firstMaxZ - firstMinZ;
    if (!(extY > 0.05 && extY < 5.0 && extZ > 0.05 && extZ < 5.0)) {
        SetErr(err, errSize, "wheel extents out of range");
        return false;
    }
    out.wheelR = (extY > extZ ? extY : extZ) * 0.5;
    out.clearance = -minZAll;
    if (!(out.wheelR > 0.05 && out.wheelR < 2.0 && out.clearance > 0.0 && out.clearance < 5.0)) {
        SetErr(err, errSize, "wheelR/clearance out of range");
        return false;
    }
    out.wheels = static_cast<int>(wheels.size());
    return true;
}
