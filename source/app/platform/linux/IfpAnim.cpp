// IfpAnim implementation (R6j). See IfpAnim.h for the contract.
// Own librw engine handle (same NULL-platform parse-only set as SkinPed;
// one shot path per process, no double init). IMG helpers duplicated per
// native-track precedent (no refactors of verified slices in-round).

#include "app/platform/linux/IfpAnim.h"
#ifdef REALTIME_GAMEPLAY_POSE_AUDIT
#include "app/platform/linux/RealtimeGameplayPoseAudit.h"
#endif

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <array>
#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <set>

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

#include <rw.h>

namespace {

#ifdef REALTIME_GAMEPLAY_POSE_AUDIT
static RealtimeGameplayPoseAudit* s_RealtimePoseAudit = nullptr;
#endif

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

std::string ToLowerCopy(const char* s) {
    std::string o = s ? s : "";
    ToLowerInPlace(o);
    return o;
}

// Trim ASCII spaces/tabs/CR/LF/NUL on both ends; IFP seq names carry a
// leading space (" Pelvis") while DFF canonical names do not.
std::string TrimCopy(const char* s, std::size_t n) {
    std::string t;
    t.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        char c = s[i];
        if (c == '\0') {
            break;
        }
        t.push_back(c);
    }
    std::size_t b = 0;
    while (b < t.size() && (t[b] == ' ' || t[b] == '\t' || t[b] == '\r' || t[b] == '\n')) {
        ++b;
    }
    std::size_t e = t.size();
    while (e > b && (t[e - 1] == ' ' || t[e - 1] == '\t' || t[e - 1] == '\r' || t[e - 1] == '\n')) {
        --e;
    }
    return t.substr(b, e - b);
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
    std::string name;
    std::string nameLower;
    uint32 off = 0;
    uint32 size = 0;
};

struct ImgIndex {
    std::string rel;
    std::string label;
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
        en.size &= 0x7FFFu;
        idx.entries.push_back(en);
    }
    return true;
}

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
    // Tolerant across slices in one process (DuoShot composes CarPose +
    // IfpAnim, which register the identical plugin set): when another
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

// Canonical bone names: verbatim copy of the retail table
// (ConvertBoneTag2BoneName, RpAnimBlend.cpp) for the name-domain mapping.
// Tags are the shared key; names are logged as proof.
const char* BoneTagToName(int32 tag) {
    switch (tag) {
    case 301: return "R Breast";
    case 302: return "L Breast";
    case 201: return "Belly";
    case 0: return "Root";
    case 1: return "Pelvis";
    case 2: return "Spine";
    case 3: return "Spine1";
    case 4: return "Neck";
    case 5: return "Head";
    case 6: return "L Brow";
    case 7: return "R Brow";
    case 8: return "Jaw";
    case 21: return "Bip01 R Clavicle";
    case 22: return "R UpperArm";
    case 23: return "R Forearm";
    case 24: return "R Hand";
    case 25: return "R Fingers";
    case 26: return "R Finger01";
    case 31: return "Bip01 L Clavicle";
    case 32: return "L UpperArm";
    case 33: return "L Forearm";
    case 34: return "L Hand";
    case 35: return "L Fingers";
    case 36: return "L Finger01";
    case 41: return "L Thigh";
    case 42: return "L Calf";
    case 43: return "L Foot";
    case 44: return "L Toe";
    case 51: return "R Thigh";
    case 52: return "R Calf";
    case 53: return "R Foot";
    case 54: return "R Toe";
    default: return nullptr;
    }
}

// Reverse lookup for tag==-1 sequences (CAR-style, name-only): trimmed
// case-insensitive match against the canonical table plus the known IFP
// spelling variants ("L Finger" vs "L Fingers", "L Toe0" vs "L Toe",
// "Normal" vs "Root").
int32 BoneNameToTag(const std::string& trimmed) {
    std::string l = trimmed;
    ToLowerInPlace(l);
    if (l == "normal" || l == "root") {
        return 0;
    }
    // Direct canonical hit.
    for (int32 tag : {0, 1, 2, 3, 4, 5, 6, 7, 8, 21, 22, 23, 24, 25, 26, 31, 32, 33, 34, 35, 36,
                      41, 42, 43, 44, 51, 52, 53, 54, 201, 301, 302}) {
        const char* cn = BoneTagToName(tag);
        if (!cn) {
            continue;
        }
        std::string cl = cn;
        ToLowerInPlace(cl);
        if (l == cl) {
            return tag;
        }
    }
    // Known variants.
    if (l == "l finger") {
        return 35;
    }
    if (l == "r finger") {
        return 25;
    }
    if (l == "l fingers") {
        return 35;
    }
    if (l == "r fingers") {
        return 25;
    }
    if (l == "l toe0" || l == "l toe") {
        return 44;
    }
    if (l == "r toe0" || l == "r toe") {
        return 54;
    }
    if (l == "spine 1" || l == "spine1") {
        return 3;
    }
    if (l == "spine 2") {
        return 3;
    }
    if (l == "bip01 l clavicle") {
        return 31;
    }
    if (l == "bip01 r clavicle") {
        return 21;
    }
    return -1;
}

// --- IFP bank model (decoded floats, absolute times as stored) ---

} // namespace

// Named implementation types also back the owned public bank's opaque Impl.
namespace IfpAnimDetail {
struct IfpFrame {
    float q[4]; // x,y,z,w
    float t[3]; // valid iff hasT
    bool hasT = false;
    float absTime = 0.0f;
};

struct IfpSeq {
    char name[32] = {}; // trimmed stored-case ("Pelvis", "Normal")
    int32 tag = -1;
    int ftype = 0;
    std::vector<IfpFrame> frames;
};

struct IfpAnimData {
    char name[32] = {}; // as stored ("IDLE_stance")
    std::vector<IfpSeq> seqs;
    float total = 0.0f; // max last absTime
};
} // namespace IfpAnimDetail

namespace {
using IfpAnimDetail::IfpFrame;
using IfpAnimDetail::IfpSeq;
using IfpAnimDetail::IfpAnimData;

static uint16 ReadU16LE(const uint8* p) {
    return static_cast<uint16>(p[0] | (static_cast<uint16>(p[1]) << 8));
}
static int16 ReadI16LE(const uint8* p) {
    return static_cast<int16>(p[0] | (static_cast<uint16>(p[1]) << 8));
}
static int32 ReadI32LE(const uint8* p) {
    int32 v = 0;
    std::memcpy(&v, p, 4);
    return v;
}
static uint32 ReadU32LE(const uint8* p) {
    uint32 v = 0;
    std::memcpy(&v, p, 4);
    return v;
}
static float ReadF32LE(const uint8* p) {
    float v = 0.0f;
    std::memcpy(&v, p, 4);
    return v;
}

// Parses ANP3 (and ANP2 as uncompressed-only). Returns false on layout
// mismatch; never invents frames.
bool ParseIfpBank(const std::vector<uint8>& bytes, std::string& bankNameOut,
                  std::vector<IfpAnimData>& animsOut, char* err, std::size_t errSize) {
    animsOut.clear();
    bankNameOut.clear();
    if (bytes.size() < 8) {
        SetErr(err, errSize, "IFP too small for header");
        return false;
    }
    char fourcc[5] = {};
    std::memcpy(fourcc, bytes.data(), 4);
    bool isAnp3 = std::memcmp(bytes.data(), "ANP3", 4) == 0;
    bool isAnp2 = std::memcmp(bytes.data(), "ANP2", 4) == 0;
    if (!isAnp3 && !isAnp2) {
        SetErr(err, errSize, "IFP header is not ANP3/ANP2");
        return false;
    }
    std::size_t pos = 8; // skip FourCC + size/unknown dword
    if (pos + 24 + 4 > bytes.size()) {
        SetErr(err, errSize, "IFP truncated at block header");
        return false;
    }
    {
        char blk[25] = {};
        std::memcpy(blk, bytes.data() + pos, 24);
        blk[24] = '\0';
        bankNameOut = TrimCopy(blk, 24);
    }
    pos += 24;
    uint32 numAnims = ReadU32LE(bytes.data() + pos);
    pos += 4;
    if (numAnims == 0 || numAnims > 10000) {
        SetErr(err, errSize, "IFP animation count out of range");
        return false;
    }
    for (uint32 a = 0; a < numAnims; ++a) {
        if (pos + 24 + 4 > bytes.size()) {
            SetErr(err, errSize, "IFP truncated at anim header");
            return false;
        }
        IfpAnimData anim;
        {
            char nm[25] = {};
            std::memcpy(nm, bytes.data() + pos, 24);
            nm[24] = '\0';
            std::string t = TrimCopy(nm, 24);
            (void)std::snprintf(anim.name, sizeof(anim.name), "%s", t.c_str());
        }
        pos += 24;
        uint32 numSeq = ReadU32LE(bytes.data() + pos);
        pos += 4;
        if (numSeq == 0 || numSeq > 128) {
            SetErr(err, errSize, "IFP sequence count out of range");
            return false;
        }
        if (isAnp3) {
            if (pos + 8 > bytes.size()) {
                SetErr(err, errSize, "IFP truncated at ANP3 size/flags");
                return false;
            }
            // size + flags (flags&1 = compressed); frameType still decides
            // the per-sequence decoder below so mixed banks stay honest.
            pos += 8;
        }
        anim.seqs.reserve(numSeq);
        float animTotal = 0.0f;
        for (uint32 s = 0; s < numSeq; ++s) {
            if (pos + 24 + 4 + 4 + 4 > bytes.size()) {
                SetErr(err, errSize, "IFP truncated at seq header");
                return false;
            }
            IfpSeq seq;
            {
                char sn[25] = {};
                std::memcpy(sn, bytes.data() + pos, 24);
                sn[24] = '\0';
                std::string t = TrimCopy(sn, 24);
                (void)std::snprintf(seq.name, sizeof(seq.name), "%s", t.c_str());
            }
            pos += 24;
            uint32 ftype = ReadU32LE(bytes.data() + pos);
            pos += 4;
            uint32 nframes = ReadU32LE(bytes.data() + pos);
            pos += 4;
            int32 btag = ReadI32LE(bytes.data() + pos);
            pos += 4;
            seq.tag = btag;
            seq.ftype = static_cast<int>(ftype);
            if (nframes > 100000) {
                SetErr(err, errSize, "IFP frame count out of range");
                return false;
            }
            std::size_t kfSize = 0;
            if (ftype == 1) {
                kfSize = 20;
            } else if (ftype == 2) {
                kfSize = 32;
            } else if (ftype == 3) {
                kfSize = 10;
            } else if (ftype == 4) {
                kfSize = 16;
            } else {
                SetErr(err, errSize, "IFP unknown frame type (not 1..4)");
                return false;
            }
            if (pos + kfSize * nframes > bytes.size()) {
                SetErr(err, errSize, "IFP truncated in keyframes");
                return false;
            }
            seq.frames.reserve(nframes);
            for (uint32 k = 0; k < nframes; ++k) {
                const uint8* p = bytes.data() + pos + static_cast<size_t>(k) * kfSize;
                IfpFrame fr;
                if (ftype == 3) {
                    int16 x = ReadI16LE(p + 0);
                    int16 y = ReadI16LE(p + 2);
                    int16 z = ReadI16LE(p + 4);
                    int16 w = ReadI16LE(p + 6);
                    int16 dt = ReadI16LE(p + 8);
                    fr.q[0] = static_cast<float>(x) / 4096.0f;
                    fr.q[1] = static_cast<float>(y) / 4096.0f;
                    fr.q[2] = static_cast<float>(z) / 4096.0f;
                    fr.q[3] = static_cast<float>(w) / 4096.0f;
                    fr.hasT = false;
                    fr.t[0] = fr.t[1] = fr.t[2] = 0.0f;
                    fr.absTime = static_cast<float>(dt) / 60.0f;
                } else if (ftype == 4) {
                    int16 x = ReadI16LE(p + 0);
                    int16 y = ReadI16LE(p + 2);
                    int16 z = ReadI16LE(p + 4);
                    int16 w = ReadI16LE(p + 6);
                    int16 dt = ReadI16LE(p + 8);
                    int16 tx = ReadI16LE(p + 10);
                    int16 ty = ReadI16LE(p + 12);
                    int16 tz = ReadI16LE(p + 14);
                    fr.q[0] = static_cast<float>(x) / 4096.0f;
                    fr.q[1] = static_cast<float>(y) / 4096.0f;
                    fr.q[2] = static_cast<float>(z) / 4096.0f;
                    fr.q[3] = static_cast<float>(w) / 4096.0f;
                    fr.hasT = true;
                    fr.t[0] = static_cast<float>(tx) / 1024.0f;
                    fr.t[1] = static_cast<float>(ty) / 1024.0f;
                    fr.t[2] = static_cast<float>(tz) / 1024.0f;
                    fr.absTime = static_cast<float>(dt) / 60.0f;
                } else if (ftype == 1) {
                    fr.q[0] = ReadF32LE(p + 0);
                    fr.q[1] = ReadF32LE(p + 4);
                    fr.q[2] = ReadF32LE(p + 8);
                    fr.q[3] = ReadF32LE(p + 12);
                    fr.absTime = ReadF32LE(p + 16);
                    fr.hasT = false;
                    fr.t[0] = fr.t[1] = fr.t[2] = 0.0f;
                } else { // ftype 2
                    fr.q[0] = ReadF32LE(p + 0);
                    fr.q[1] = ReadF32LE(p + 4);
                    fr.q[2] = ReadF32LE(p + 8);
                    fr.q[3] = ReadF32LE(p + 12);
                    fr.absTime = ReadF32LE(p + 16);
                    fr.hasT = true;
                    fr.t[0] = ReadF32LE(p + 20);
                    fr.t[1] = ReadF32LE(p + 24);
                    fr.t[2] = ReadF32LE(p + 28);
                }
                seq.frames.push_back(fr);
            }
            pos += kfSize * nframes;
            if (!seq.frames.empty()) {
                float last = seq.frames.back().absTime;
                if (last > animTotal) {
                    animTotal = last;
                }
            }
            anim.seqs.push_back(std::move(seq));
        }
        anim.total = animTotal;
        animsOut.push_back(std::move(anim));
    }
    return true;
}

// Effective tag for mapping: disk tag, else canonical-name lookup.
int32 EffectiveTag(const IfpSeq& seq) {
    if (seq.tag != -1) {
        return seq.tag;
    }
    return BoneNameToTag(seq.name);
}

// Loads raw bank bytes: loose `anim/ped.ifp` first (retail path
// CAnimManager::LoadAnimFiles opens ANIM\PED.IFP), then the IMG-packed
// `anim/anim.img:ped.ifp` through the VER2 reader. srcLabel always names
// the winning source; no bytes are invented.
bool LoadBankBytes(const char* gameDir, const char* bank, std::vector<uint8>& out, std::string& srcLabel,
                   char* err, std::size_t errSize) {
    std::string b = bank && bank[0] ? bank : "ped";
    std::string looseRel = std::string("anim/") + b + ".ifp";
    std::vector<uint8> loose;
    if (ReadWholeFileOS(looseRel.c_str(), loose) && loose.size() >= 8) {
        out = std::move(loose);
        srcLabel = looseRel;
        return true;
    }
    ImgIndex idx;
    if (!BuildImgIndex("anim/anim.img", idx)) {
        SetErr(err, errSize, "no loose anim/ped.ifp and anim/anim.img not indexed");
        return false;
    }
    std::string want = b + ".ifp";
    ToLowerInPlace(want);
    std::vector<uint8> packed;
    if (!ImgReadBytesStd(idx, want, packed) || packed.empty()) {
        char msg[192];
        (void)std::snprintf(msg, sizeof(msg), "IFP bank '%s.ifp' in neither anim/%s.ifp nor anim/anim.img",
                             b.c_str(), b.c_str());
        SetErr(err, errSize, msg);
        return false;
    }
    out = std::move(packed);
    srcLabel = std::string("anim.img:") + b + ".ifp";
    (void)gameDir;
    return true;
}

// quat (x,y,z,w) -> librw local matrix with pos; formula is retail
// CQuaternion::Get (imag-doubled products), same convention as the game.
void QuatPosToMatrix(const float q[4], const float pos[3], rw::Matrix& out) {
    float x = q[0];
    float y = q[1];
    float z = q[2];
    float w = q[3];
    float len = std::sqrt(x * x + y * y + z * z + w * w);
    if (len > 1e-9f) {
        x /= len;
        y /= len;
        z /= len;
        w /= len;
    } else {
        x = y = z = 0.0f;
        w = 1.0f;
    }
    float x2 = x + x;
    float y2 = y + y;
    float z2 = z + z;
    float x2x = x2 * x;
    float y2x = y2 * x;
    float z2x = z2 * x;
    float y2y = y2 * y;
    float z2y = z2 * y;
    float z2z = z2 * z;
    float x2r = x2 * w;
    float y2r = y2 * w;
    float z2r = z2 * w;
    out.right.x = 1.0f - (z2z + y2y);
    out.right.y = z2r + y2x;
    out.right.z = z2x - y2r;
    out.up.x = y2x - z2r;
    out.up.y = 1.0f - (z2z + x2x);
    out.up.z = x2r + z2y;
    out.at.x = y2r + z2x;
    out.at.y = z2y - x2r;
    out.at.z = 1.0f - (y2y + x2x);
    out.pos.x = pos[0];
    out.pos.y = pos[1];
    out.pos.z = pos[2];
    out.flags = 0;
    out.pad1 = out.pad2 = out.pad3 = 0;
}

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

// --- R6k interpolation helpers (lerp trans + slerp quat, IFP bytes only) ---
void NormQuat4(float q[4]) {
    float l = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (l > 1e-9f) {
        q[0] /= l;
        q[1] /= l;
        q[2] /= l;
        q[3] /= l;
    } else {
        q[0] = q[1] = q[2] = 0.0f;
        q[3] = 1.0f;
    }
}

void NormQuatCopy(const float in[4], float out[4]) {
    out[0] = in[0];
    out[1] = in[1];
    out[2] = in[2];
    out[3] = in[3];
    NormQuat4(out);
}

// Shortest-path slerp between unit quats; falls back to nlerp for tiny
// angles (|dot| > 0.9995) to avoid division by ~0. Deterministic.
void SlerpQuat(const float a[4], const float b[4], float t, float out[4]) {
    float dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
    float bx = b[0];
    float by = b[1];
    float bz = b[2];
    float bw = b[3];
    if (dot < 0.0f) {
        dot = -dot;
        bx = -bx;
        by = -by;
        bz = -bz;
        bw = -bw;
    }
    if (dot > 0.9995f) {
        out[0] = a[0] + t * (bx - a[0]);
        out[1] = a[1] + t * (by - a[1]);
        out[2] = a[2] + t * (bz - a[2]);
        out[3] = a[3] + t * (bw - a[3]);
        NormQuat4(out);
        return;
    }
    float theta = std::acos(dot > 1.0f ? 1.0f : dot);
    float s = std::sin(theta);
    float wa = std::sin((1.0f - t) * theta) / s;
    float wb = std::sin(t * theta) / s;
    out[0] = wa * a[0] + wb * bx;
    out[1] = wa * a[1] + wb * by;
    out[2] = wa * a[2] + wb * bz;
    out[3] = wa * a[3] + wb * bw;
}

float QuatAngle(const float a[4], const float b[4]) {
    float dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
    if (dot < 0.0f) {
        dot = -dot;
    }
    if (dot > 1.0f) {
        dot = 1.0f;
    }
    return 2.0f * std::acos(dot);
}

std::vector<rw::TexDictionary*> s_txds;

} // namespace

struct IfpAnimPlayerBank::Impl {
    std::string Name, Source;
    std::vector<IfpAnimData> Anims;
};

IfpAnimPlayerBank::IfpAnimPlayerBank() = default;
IfpAnimPlayerBank::~IfpAnimPlayerBank() = default;
bool IfpAnimPlayerBank::Load(const char* gameDir, const char* bank, char* err, std::size_t errSize) {
    if (!gameDir || !*gameDir || !bank || !*bank) {
        SetErr(err, errSize, "player bank requires game directory and bank name"); return false;
    }
    OS_SetFilePathOffset(gameDir);
    s_gameAbs = gameDir;
    auto next = std::make_unique<Impl>();
    std::vector<uint8> bytes;
    if (!LoadBankBytes(gameDir, bank, bytes, next->Source, err, errSize) ||
        !ParseIfpBank(bytes, next->Name, next->Anims, err, errSize)) return false;
    m_Impl = std::move(next);
    SetErr(err, errSize, ""); return true;
}

namespace {
rw::Matrix PlayerMatrix(const NativePlayerMatrix& m) {
    rw::Matrix out{};
    out.right = {m.Right[0], m.Right[1], m.Right[2]};
    out.up = {m.Up[0], m.Up[1], m.Up[2]};
    out.at = {m.At[0], m.At[1], m.At[2]};
    out.pos = {m.Pos[0], m.Pos[1], m.Pos[2]};
    return out;
}
NativePlayerMatrix PlayerOwnedMatrix(const rw::Matrix& m) {
    return {{m.right.x,m.right.y,m.right.z}, {m.up.x,m.up.y,m.up.z},
            {m.at.x,m.at.y,m.at.z}, {m.pos.x,m.pos.y,m.pos.z}};
}
bool PlayerAssetsValid(const NativePlayerAssets& a) {
    if (a.Bones.size()!=32 || a.Vertices.empty() || a.Triangles.empty()) return false;
    std::set<int> tags;
    std::vector<int> stack;
    int parent=-1;
    for (size_t i=0;i<a.Bones.size();++i) {
        const auto& b=a.Bones[i];
        if (b.Parent!=parent || b.Parent>=static_cast<int>(i) || !tags.insert(b.Tag).second) return false;
        for (const auto& m:{b.InverseBind,b.BindLocal})
            for (const auto& v:{m.Right,m.Up,m.At,m.Pos})
                for (float f:v) if (!std::isfinite(f)) return false;
        if (b.Flags & 2) stack.push_back(parent);
        parent=static_cast<int>(i);
        if (b.Flags & 1) {
            if (stack.empty() && i+1!=a.Bones.size()) return false;
            parent=stack.empty() ? -1:stack.back();
            if (!stack.empty()) stack.pop_back();
        }
    }
    if (!stack.empty()) return false;
    for (const auto& v:a.Vertices) {
        for (float f:v.Position) if (!std::isfinite(f)) return false;
        for (float f:v.Normal) if (!std::isfinite(f)) return false;
        for (float f:v.UV) if (!std::isfinite(f)) return false;
        float sum=0;
        for (size_t k=0;k<4;++k) {
            if (!std::isfinite(v.Weights[k]) || v.Weights[k]<0 ||
                (v.Weights[k]>0 && v.Bones[k]>=a.Bones.size())) return false;
            sum+=v.Weights[k];
        }
        if (std::abs(sum-1)>0.001f) return false;
    }
    size_t triangles=0,vertices=0;
    for (size_t slot=0;slot<a.Parts.size();++slot) {
        const auto& p=a.Parts[slot];
        if (p.FirstTriangle!=triangles || p.FirstVertex!=vertices || !p.TriangleCount || !p.VertexCount ||
            triangles+p.TriangleCount>a.Triangles.size() || vertices+p.VertexCount>a.Vertices.size()) return false;
        for (size_t t=triangles;t<triangles+p.TriangleCount;++t) {
            if (a.Triangles[t].Material!=slot) return false;
            for (auto v:a.Triangles[t].Vertices) if (v<vertices || v>=vertices+p.VertexCount) return false;
        }
        triangles+=p.TriangleCount; vertices+=p.VertexCount;
    }
    if (triangles!=a.Triangles.size() || vertices!=a.Vertices.size()) return false;
    for (const auto& m:a.Materials) if (m.Image>=a.Images.size()) return false;
    for (const auto& image:a.Images)
        if (image.Width<=0 || image.Height<=0 || image.RGBA.size()!=size_t(image.Width)*size_t(image.Height)*4) return false;
    return true;
}
}

bool IfpAnim_InitPlayer(const NativePlayerAssets& assets, const IfpAnimPlayerBank& bank,
                       const char* animName, double timeFrac, WorldShotScene& scene,
                       IfpAnimStats& stats, char* err, std::size_t errSize,
                       bool exportImages, IfpAnimPlayerAudit* audit) {
    if (!PlayerAssetsValid(assets) || !std::isfinite(timeFrac)) {
        SetErr(err,errSize,"invalid player asset references/hierarchy/weights or pose time"); return false;
    }
    const auto* data=bank.Data();
    const IfpAnimData* anim=nullptr;
    if (animName && *animName) {
        if (data) for (const auto& a:data->Anims)
            if (ToLowerCopy(a.name)==ToLowerCopy(animName)) { anim=&a; break; }
        if (!anim) { SetErr(err,errSize,"player IFP clip absent from loaded bank"); return false; }
    }
    IfpAnimStats st{};
    st.time=std::clamp(timeFrac,0.0,1.0); st.animTotal=anim ? anim->total:0;
    st.timeAbs=st.time*st.animTotal; st.interp=1;
    st.bones=32; st.geoms=5; st.textures=4;
    st.seqs=anim ? static_cast<int>(anim->seqs.size()):0;
    st.animsInBank=data ? static_cast<int>(data->Anims.size()):0;
    std::snprintf(st.model,sizeof(st.model),"cj-explicit-normal");
    std::snprintf(st.requested,sizeof(st.requested),"explicit NativePlayerClothes");
    std::snprintf(st.src,sizeof(st.src),"gta3.img:player.dff + player.img:five-parts");
    std::snprintf(st.txd,sizeof(st.txd),"player.img:explicit-composed-images");
    std::snprintf(st.anim,sizeof(st.anim),"%s",anim ? anim->name:"bind");
    if (data) {
        std::snprintf(st.bank,sizeof(st.bank),"%s",data->Name.c_str());
        std::snprintf(st.bankSrc,sizeof(st.bankSrc),"%s",data->Source.c_str());
    }
    std::vector<rw::Matrix> locals(32),worlds(32),skins(32),bindWorlds(32),bindSkins(32);
    for (size_t i=0;i<assets.Bones.size();++i) {
        const auto& bone=assets.Bones[i];
        locals[i]=PlayerMatrix(bone.BindLocal);
        bindWorlds[i]=locals[i];
        if (bone.Parent>=0) rw::Matrix::mult(&bindWorlds[i],&locals[i],&bindWorlds[bone.Parent]);
        const auto inverse=PlayerMatrix(bone.InverseBind);
        rw::Matrix::mult(&bindSkins[i],&inverse,&bindWorlds[i]);
        const IfpSeq* seq=nullptr;
        if (anim) for (const auto& s:anim->seqs)
            if (EffectiveTag(s)==bone.Tag && !s.frames.empty()) { seq=&s; break; }
        if (seq) {
            ++st.mapped;
            const float time=static_cast<float>(st.timeAbs);
            size_t k0=0,k1=0;
            if (time>=seq->frames.back().absTime) k0=k1=seq->frames.size()-1;
            else if (time>seq->frames.front().absTime) {
                for (size_t k=0;k+1<seq->frames.size();++k)
                    if (seq->frames[k].absTime<=time && time<=seq->frames[k+1].absTime) { k0=k; k1=k+1; break; }
            }
            const auto& a=seq->frames[k0]; const auto& b=seq->frames[k1];
            const float alpha=b.absTime>a.absTime ? (time-a.absTime)/(b.absTime-a.absTime):0;
            float q0[4],q1[4],q[4],t[3];
            NormQuatCopy(a.q,q0); NormQuatCopy(b.q,q1);
            SlerpQuat(q0,q1,alpha,q); NormQuat4(q);
            for (int c=0;c<3;++c) t[c]=(a.hasT || b.hasT) ? a.t[c]+alpha*(b.t[c]-a.t[c]):bone.BindLocal.Pos[c];
            QuatPosToMatrix(q,t,locals[i]);
        }
        worlds[i]=locals[i];
        if (bone.Parent>=0) rw::Matrix::mult(&worlds[i],&locals[i],&worlds[bone.Parent]);
        // Row-vector skinRenderCB order S*A: inverseBind * animatedWorld.
        // The assembled CJ atomic A is identity (asset constructor contract).
        rw::Matrix::mult(&skins[i],&inverse,&worlds[i]);
        if (bone.Tag==0) {
            st.rootWorld[0]=worlds[i].pos.x; st.rootWorld[1]=worlds[i].pos.y; st.rootWorld[2]=worlds[i].pos.z;
            st.rootDelta=std::hypot(worlds[i].pos.x-bindWorlds[i].pos.x,
                worlds[i].pos.y-bindWorlds[i].pos.y,worlds[i].pos.z-bindWorlds[i].pos.z);
        }
    }
    st.unmapped=st.bones-st.mapped;
    WorldShotScene out{};
    for (int c=0;c<3;++c) { st.bindMin[c]=st.animMin[c]=INFINITY; st.bindMax[c]=st.animMax[c]=-INFINITY; }
    std::vector<rw::V3d> positions(assets.Vertices.size()),normals(assets.Vertices.size());
    for (size_t v=0;v<assets.Vertices.size();++v) {
        const auto& source=assets.Vertices[v];
        const rw::V3d p{source.Position[0],source.Position[1],source.Position[2]};
        const rw::V3d n{source.Normal[0],source.Normal[1],source.Normal[2]};
        rw::V3d pos{},normal{},bind{};
        for (size_t k=0;k<4;++k) {
            const float weight=source.Weights[k]; if (weight==0) continue;
            const auto bone=source.Bones[k]; rw::V3d a,b,c;
            rw::V3d::transformPoints(&a,&p,1,&skins[bone]);
            rw::V3d::transformVectors(&b,&n,1,&skins[bone]);
            rw::V3d::transformPoints(&c,&p,1,&bindSkins[bone]);
            pos.x+=a.x*weight; pos.y+=a.y*weight; pos.z+=a.z*weight;
            normal.x+=b.x*weight; normal.y+=b.y*weight; normal.z+=b.z*weight;
            bind.x+=c.x*weight; bind.y+=c.y*weight; bind.z+=c.z*weight;
            st.wsum+=weight;
        }
        const float length=std::hypot(normal.x,normal.y,normal.z);
        if (length>1e-9f) { normal.x/=length; normal.y/=length; normal.z/=length; }
        positions[v]=pos; normals[v]=normal;
        const float bp[]{bind.x,bind.y,bind.z},ap[]{pos.x,pos.y,pos.z};
        for (int c=0;c<3;++c) {
            st.bindMin[c]=std::min(st.bindMin[c],bp[c]); st.bindMax[c]=std::max(st.bindMax[c],bp[c]);
            st.animMin[c]=std::min(st.animMin[c],ap[c]); st.animMax[c]=std::max(st.animMax[c],ap[c]);
        }
    }
    st.wsum/=assets.Vertices.size();
    for (size_t slot=0;slot<assets.Parts.size();++slot) {
        auto& mesh=out.meshes.emplace_back(); const auto& part=assets.Parts[slot];
        mesh.tris=static_cast<int>(part.TriangleCount);
        const auto& material=assets.Materials[slot];
        WorldShotSurface surface;
        for (int c=0;c<4;++c) surface.color[c]=material.RGBA[c]/255.0f;
        for (int c=0;c<3;++c) mesh.color[c]=surface.color[c];
        for (size_t i=part.FirstTriangle;i<size_t(part.FirstTriangle)+part.TriangleCount;++i) {
            const auto& tri=assets.Triangles[i];
            mesh.triImg.push_back(static_cast<int>(material.Image)); mesh.surfaces.push_back(surface);
            mesh.triCol.insert(mesh.triCol.end(),{mesh.color[0],mesh.color[1],mesh.color[2]});
            for (auto v:tri.Vertices) {
                const auto p=positions[v],n=normals[v];
                mesh.pos.insert(mesh.pos.end(),{p.x,p.y,p.z}); mesh.nrm.insert(mesh.nrm.end(),{n.x,n.y,n.z});
                mesh.uv.insert(mesh.uv.end(),assets.Vertices[v].UV.begin(),assets.Vertices[v].UV.end());
                // The assembled lit CJ geometry has no authored prelight.
                mesh.dayColors.insert(mesh.dayColors.end(),{0,0,0,255});
            }
        }
    }
    if (exportImages) for (const auto& image:assets.Images) {
        auto& dst=out.images.emplace_back();
        std::snprintf(dst.name,sizeof(dst.name),"%s",image.Name.c_str());
        dst.w=image.Width; dst.h=image.Height; dst.filter=image.FilterAddressing; dst.rgba=image.RGBA;
    }
    st.tris=static_cast<int>(assets.Triangles.size()); st.verts=st.tris*3;
    out.stats.atomics=5; out.stats.triangles=st.tris; out.stats.vertices=st.verts; out.stats.textures=4;
    for (int c=0;c<3;++c) { out.bboxMin[c]=st.animMin[c]; out.bboxMax[c]=st.animMax[c]; }
    if (audit) {
        audit->Locals.clear(); audit->Worlds.clear();
        for (size_t i=0;i<32;++i) { audit->Locals.push_back(PlayerOwnedMatrix(locals[i])); audit->Worlds.push_back(PlayerOwnedMatrix(worlds[i])); }
    }
    scene=std::move(out); stats=st; SetErr(err,errSize,""); return true;
}

bool IfpAnim_List(const char* gameDir, std::vector<std::string>& names, char* bankSrcOut,
                  std::size_t bankSrcSize, char* err, std::size_t errSize) {
    names.clear();
    if (!gameDir || !gameDir[0]) {
        SetErr(err, errSize, "no game dir");
        return false;
    }
    std::string game(gameDir);
    OS_SetFilePathOffset(game.c_str());
    s_gameAbs = game;
    std::vector<uint8> bankBytes;
    std::string srcLabel;
    char lerr[256] = {};
    if (!LoadBankBytes(gameDir, "ped", bankBytes, srcLabel, lerr, sizeof(lerr))) {
        SetErr(err, errSize, lerr);
        return false;
    }
    std::string bankName;
    std::vector<IfpAnimData> anims;
    if (!ParseIfpBank(bankBytes, bankName, anims, err, errSize)) {
        return false;
    }
    for (const auto& a : anims) {
        names.push_back(a.name);
    }
    if (bankSrcOut && bankSrcSize > 0) {
        (void)std::snprintf(bankSrcOut, bankSrcSize, "%s", srcLabel.c_str());
    }
    return true;
}

bool IfpAnim_Init(const char* gameDir, const char* model, const char* animName, double timeFrac,
                  WorldShotScene& scene, IfpAnimStats& stats, char* err, std::size_t errSize,
                  bool interp) {
    stats = IfpAnimStats{};
    scene.meshes.clear();
    scene.images.clear();
    if (!gameDir || !gameDir[0]) {
        SetErr(err, errSize, "no game dir");
        return false;
    }
    if (!(timeFrac >= 0.0 && timeFrac <= 1.0)) {
        SetErr(err, errSize, "bad --time (want 0..1 fraction)");
        return false;
    }
    std::string want = model && model[0] ? model : "andre";
    (void)std::snprintf(stats.requested, sizeof(stats.requested), "%s", want.c_str());
    std::string wantLower = want;
    ToLowerInPlace(wantLower);
    std::string animWant = animName && animName[0] ? animName : "IDLE_stance";
    std::string animWantLower = ToLowerCopy(animWant.c_str());
    stats.time = timeFrac;
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

    // --- 1. DFF bytes (direct, then SkinPed-identical gta3 fallback) ---
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
                                 "ped fallback scan found no skinned DFF in gta3.img (tried=%d)", tried);
            SetErr(err, errSize, msg);
            return false;
        }
        (void)std::snprintf(stats.src, sizeof(stats.src), "%s:%s", hitImg->label.c_str(),
                             pickLocal.c_str());
    } else {
        for (const ImgEntry& e : hitImg->entries) {
            if (e.nameLower == dffFile) {
                (void)std::snprintf(stats.src, sizeof(stats.src), "%s:%s", hitImg->label.c_str(),
                                     e.name.c_str());
                break;
            }
        }
    }
    (void)std::snprintf(stats.model, sizeof(stats.model), "%s", resolved.c_str());

    // --- 2. Per-model TXD ---
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

    // --- 3. Parse clump + hierarchy (bind pose as streamed) ---
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
    rw::HAnimHierarchy* hh = nil;
    {
        // Atomic hierarchy first (SA peds), else clump-wide.
        FORLIST(link, lc.clump->atomics) {
            rw::Atomic* at = rw::Atomic::fromClump(link);
            rw::HAnimHierarchy* h = rw::Skin::getHierarchy(at);
            if (h) {
                hh = h;
                break;
            }
        }
        if (!hh && root) {
            hh = rw::HAnimHierarchy::find(root);
        }
    }
    if (!hh || hh->numNodes <= 0 || !hh->nodeInfo) {
        TexSample_FreeLinked(lc);
        SetErr(err, errSize, "no HAnim hierarchy for skinned ped");
        return false;
    }
    const int numBones = hh->numNodes;
    hh->attach();
    int attached = 0;
    for (int i = 0; i < numBones; ++i) {
        if (hh->nodeInfo[i].frame) {
            ++attached;
        }
    }
    if (attached == 0) {
        TexSample_FreeLinked(lc);
        SetErr(err, errSize, "HAnim hierarchy attached to no frames");
        return false;
    }
    stats.bones = numBones;

    // Snapshots: tags, frames, bind locals + bind world LTMs.
    std::vector<int32> boneTags(static_cast<size_t>(numBones), -1);
    std::vector<rw::Frame*> boneFrames(static_cast<size_t>(numBones), nil);
    std::vector<rw::Matrix> bindLocals(static_cast<size_t>(numBones));
    std::vector<rw::Matrix> bindWorld(static_cast<size_t>(numBones));
    for (int i = 0; i < numBones; ++i) {
        boneTags[static_cast<size_t>(i)] = hh->nodeInfo[i].id;
        boneFrames[static_cast<size_t>(i)] = hh->nodeInfo[i].frame;
        rw::Frame* f = hh->nodeInfo[i].frame;
        if (f) {
            bindLocals[static_cast<size_t>(i)] = f->matrix;
            rw::Matrix* ltm = f->getLTM();
            bindWorld[static_cast<size_t>(i)] = ltm ? *ltm : f->matrix;
        } else {
            bindLocals[static_cast<size_t>(i)].setIdentity();
            bindWorld[static_cast<size_t>(i)].setIdentity();
        }
    }

    // Atomic frame for S_i = IB_i * (W_i * inv(A)).
    rw::Atomic* firstAtomic = nil;
    FORLIST(link, lc.clump->atomics) {
        firstAtomic = rw::Atomic::fromClump(link);
        break;
    }
    rw::Frame* atomicFrame = firstAtomic ? firstAtomic->getFrame() : nil;
    rw::Matrix atomicMat;
    if (atomicFrame && atomicFrame->getLTM()) {
        atomicMat = *atomicFrame->getLTM();
    } else {
        atomicMat.setIdentity();
    }
    rw::Matrix invAtomic;
    rw::Matrix::invert(&invAtomic, &atomicMat);

    // --- 4. IFP bank + animation lookup (case-insensitive) ---
    std::vector<uint8> bankBytes;
    std::string bankSrc;
    {
        char lerr[256] = {};
        if (!LoadBankBytes(gameDir, "ped", bankBytes, bankSrc, lerr, sizeof(lerr))) {
            TexSample_FreeLinked(lc);
            SetErr(err, errSize, lerr);
            return false;
        }
    }
    std::string bankName;
    std::vector<IfpAnimData> bank;
    if (!ParseIfpBank(bankBytes, bankName, bank, err, errSize)) {
        TexSample_FreeLinked(lc);
        return false;
    }
    (void)std::snprintf(stats.bank, sizeof(stats.bank), "%s", bankName.empty() ? "ped" : bankName.c_str());
    (void)std::snprintf(stats.bankSrc, sizeof(stats.bankSrc), "%s", bankSrc.c_str());
    stats.animsInBank = static_cast<int>(bank.size());
    const IfpAnimData* anim = nil;
    for (const auto& a : bank) {
        if (ToLowerCopy(a.name) == animWantLower) {
            anim = &a;
            break;
        }
    }
    if (!anim) {
        TexSample_FreeLinked(lc);
        char msg[192];
        (void)std::snprintf(msg, sizeof(msg), "animation '%s' not in ped bank (%d anims)",
                             animWant.c_str(), static_cast<int>(bank.size()));
        SetErr(err, errSize, msg);
        return false;
    }
    (void)std::snprintf(stats.anim, sizeof(stats.anim), "%s", anim->name);
    stats.seqs = static_cast<int>(anim->seqs.size());
    stats.animTotal = anim->total;
    stats.timeAbs = timeFrac * anim->total;

    // --- 5. Sample per sequence at T_abs: legacy single key (interp=false,
    // R6j etalon) or lerp+slerp between bracketing IFP keys (interp=true) ---
    struct Sampled {
        float q[4];
        float t[3];
        bool hasT = false;
        int frame = 0; // lower bracket k0 (legacy: picked key)
        int frames = 0;
        bool valid = false;
        // Bracketing keys from IFP bytes (keyaudit source).
        int k0 = -1;
        int k1 = -1;
        float t0 = 0.0f;
        float t1 = 0.0f;
        float alpha = 0.0f;
        float q0[4];
        float q1[4];
        float p0[3];
        float p1[3];
    };
    std::vector<Sampled> sampled(anim->seqs.size());
    for (std::size_t s = 0; s < anim->seqs.size(); ++s) {
        const IfpSeq& sq = anim->seqs[s];
        Sampled sm;
        sm.frames = static_cast<int>(sq.frames.size());
        if (sq.frames.empty()) {
            sampled[s] = sm;
            continue;
        }
        if (sq.frames.size() == 1) {
            const IfpFrame& fr = sq.frames[0];
            float nq[4] = { fr.q[0], fr.q[1], fr.q[2], fr.q[3] };
            NormQuat4(nq);
            sm.q[0] = nq[0];
            sm.q[1] = nq[1];
            sm.q[2] = nq[2];
            sm.q[3] = nq[3];
            sm.hasT = fr.hasT;
            sm.t[0] = fr.t[0];
            sm.t[1] = fr.t[1];
            sm.t[2] = fr.t[2];
            sm.frame = 0;
            sm.k0 = sm.k1 = 0;
            sm.t0 = sm.t1 = fr.absTime;
            sm.alpha = 0.0f;
            sm.q0[0] = sm.q1[0] = nq[0];
            sm.q0[1] = sm.q1[1] = nq[1];
            sm.q0[2] = sm.q1[2] = nq[2];
            sm.q0[3] = sm.q1[3] = nq[3];
            sm.p0[0] = sm.p1[0] = fr.t[0];
            sm.p0[1] = sm.p1[1] = fr.t[1];
            sm.p0[2] = sm.p1[2] = fr.t[2];
            sm.valid = true;
            sampled[s] = sm;
            continue;
        }
        float tAbs = static_cast<float>(stats.timeAbs);
        if (!interp) {
            std::size_t pick = sq.frames.size() - 1;
            for (std::size_t k = 0; k < sq.frames.size(); ++k) {
                if (sq.frames[k].absTime >= tAbs) {
                    pick = k;
                    break;
                }
            }
            const IfpFrame& fr = sq.frames[pick];
            float nq[4] = { fr.q[0], fr.q[1], fr.q[2], fr.q[3] };
            NormQuat4(nq);
            sm.q[0] = nq[0];
            sm.q[1] = nq[1];
            sm.q[2] = nq[2];
            sm.q[3] = nq[3];
            sm.hasT = fr.hasT;
            sm.t[0] = fr.t[0];
            sm.t[1] = fr.t[1];
            sm.t[2] = fr.t[2];
            sm.frame = static_cast<int>(pick);
            sm.k0 = sm.k1 = static_cast<int>(pick);
            sm.t0 = sm.t1 = fr.absTime;
            sm.alpha = 0.0f;
            sm.q0[0] = sm.q1[0] = nq[0];
            sm.q0[1] = sm.q1[1] = nq[1];
            sm.q0[2] = sm.q1[2] = nq[2];
            sm.q0[3] = sm.q1[3] = nq[3];
            sm.p0[0] = sm.p1[0] = fr.t[0];
            sm.p0[1] = sm.p1[1] = fr.t[1];
            sm.p0[2] = sm.p1[2] = fr.t[2];
            sm.valid = true;
            sampled[s] = sm;
            continue;
        }
        // interp=true: bracket T_abs between neighbouring IFP keys.
        std::size_t k0 = 0;
        std::size_t k1 = 0;
        float alpha = 0.0f;
        if (tAbs <= sq.frames.front().absTime) {
            k0 = k1 = 0;
            alpha = 0.0f;
        } else if (tAbs >= sq.frames.back().absTime) {
            k0 = k1 = sq.frames.size() - 1;
            alpha = 0.0f;
        } else {
            k0 = 0;
            k1 = sq.frames.size() - 1;
            for (std::size_t k = 0; k + 1 < sq.frames.size(); ++k) {
                float a = sq.frames[k].absTime;
                float b = sq.frames[k + 1].absTime;
                if (a <= tAbs && tAbs <= b) {
                    k0 = k;
                    k1 = k + 1;
                    sm.t0 = a;
                    sm.t1 = b;
                    if (b > a) {
                        alpha = (tAbs - a) / (b - a);
                    } else {
                        alpha = 0.0f;
                    }
                    break;
                }
            }
        }
        const IfpFrame& f0 = sq.frames[k0];
        const IfpFrame& f1 = sq.frames[k1];
        float n0[4] = { f0.q[0], f0.q[1], f0.q[2], f0.q[3] };
        float n1[4] = { f1.q[0], f1.q[1], f1.q[2], f1.q[3] };
        NormQuat4(n0);
        NormQuat4(n1);
        float qi[4];
        if (k0 == k1 || alpha == 0.0f) {
            qi[0] = n0[0];
            qi[1] = n0[1];
            qi[2] = n0[2];
            qi[3] = n0[3];
        } else {
            SlerpQuat(n0, n1, alpha, qi);
        }
        NormQuat4(qi);
        sm.q[0] = qi[0];
        sm.q[1] = qi[1];
        sm.q[2] = qi[2];
        sm.q[3] = qi[3];
        sm.hasT = f0.hasT || f1.hasT;
        if (sm.hasT) {
            sm.t[0] = f0.t[0] + alpha * (f1.t[0] - f0.t[0]);
            sm.t[1] = f0.t[1] + alpha * (f1.t[1] - f0.t[1]);
            sm.t[2] = f0.t[2] + alpha * (f1.t[2] - f0.t[2]);
        } else {
            sm.t[0] = sm.t[1] = sm.t[2] = 0.0f;
        }
        sm.frame = static_cast<int>(k0);
        sm.k0 = static_cast<int>(k0);
        sm.k1 = static_cast<int>(k1);
        sm.t0 = f0.absTime;
        sm.t1 = f1.absTime;
        sm.alpha = (k0 == k1) ? 0.0f : alpha;
        sm.q0[0] = n0[0];
        sm.q0[1] = n0[1];
        sm.q0[2] = n0[2];
        sm.q0[3] = n0[3];
        sm.q1[0] = n1[0];
        sm.q1[1] = n1[1];
        sm.q1[2] = n1[2];
        sm.q1[3] = n1[3];
        sm.p0[0] = f0.t[0];
        sm.p0[1] = f0.t[1];
        sm.p0[2] = f0.t[2];
        sm.p1[0] = f1.t[0];
        sm.p1[1] = f1.t[1];
        sm.p1[2] = f1.t[2];
        sm.valid = true;
        sampled[s] = sm;
    }
    // IFP tag -> sampled index (first wins; bank has unique tags).
    std::map<int32, std::size_t> tagToSeq;
    std::map<std::string, std::size_t> nameToSeq; // trimmed-lower fallback
    for (std::size_t s = 0; s < anim->seqs.size(); ++s) {
        int32 et = EffectiveTag(anim->seqs[s]);
        if (et != -1 && tagToSeq.find(et) == tagToSeq.end()) {
            tagToSeq[et] = s;
        }
        std::string nl = ToLowerCopy(anim->seqs[s].name);
        if (nameToSeq.find(nl) == nameToSeq.end()) {
            nameToSeq[nl] = s;
        }
    }

    // --- 6. Retarget onto DFF bones (name-domain via canonical table) ---
    std::vector<int> boneSeqIdx(static_cast<size_t>(numBones), -1);
    int mapped = 0;
    for (int i = 0; i < numBones; ++i) {
        int32 tag = boneTags[static_cast<size_t>(i)];
        auto it = tagToSeq.find(tag);
        int pick = -1;
        if (it != tagToSeq.end()) {
            pick = static_cast<int>(it->second);
        } else {
            // Name fallback: DFF canonical name vs IFP trimmed names.
            const char* cn = BoneTagToName(tag);
            if (cn) {
                std::string cl = ToLowerCopy(cn);
                auto jt = nameToSeq.find(cl);
                if (jt != nameToSeq.end()) {
                    pick = static_cast<int>(jt->second);
                }
            }
        }
        boneSeqIdx[static_cast<size_t>(i)] = pick;
        if (pick >= 0 && sampled[static_cast<size_t>(pick)].valid) {
            ++mapped;
        }
    }
    stats.mapped = mapped;
    stats.unmapped = numBones - mapped;

    // --- 7. Bind skinning (bbox only) + anim locals -> anim world ---
    // Collect skinned geometries first (same selection as SkinPed).
    struct SkinnedGeom {
        rw::Geometry* geo = nil;
        rw::Atomic* atomic = nil;
    };
    std::vector<SkinnedGeom> geoms;
    FORLIST(link, lc.clump->atomics) {
        rw::Atomic* at = rw::Atomic::fromClump(link);
        rw::Geometry* geo = at ? at->geometry : nil;
        if (!geo || geo->numTriangles <= 0 || geo->numVertices <= 0) {
            continue;
        }
        if (!geo->triangles || !geo->morphTargets || !geo->morphTargets[0].vertices) {
            continue;
        }
        rw::Skin* skin = rw::Skin::get(geo);
        if (!skin || skin->numBones <= 0 || !skin->indices || !skin->weights ||
            !skin->inverseMatrices) {
            continue;
        }
        // Require the skin bone count to match the hierarchy (andre: 32).
        if (skin->numBones != numBones) {
            continue;
        }
        geoms.push_back({ geo, at });
    }
    if (geoms.empty()) {
        TexSample_FreeLinked(lc);
        SetErr(err, errSize, "no skinned geometry matching the hierarchy");
        return false;
    }
    stats.geoms = static_cast<int>(geoms.size());

    // Retail bind positions (SkinGetBonePositionsToTable, RpAnimBlend.cpp):
    // BonePos[i] is the HAnim-local bind translation derived from the
    // skin-to-bone matrices (NOT the frame local pos). For non-translated
    // IFP bones the animated local translation must be BonePos (retail
    // FrameUpdateCallBackSkinned: t = lerp(BonePos, nextT, 0) = BonePos),
    // with IFP translation only where the sequence carries it.
    std::vector<rw::V3d> bonePos(static_cast<size_t>(numBones), { 0.0f, 0.0f, 0.0f });
    {
        rw::Skin* skin0 = rw::Skin::get(geoms[0].geo);
        std::vector<rw::Matrix> ib(static_cast<size_t>(numBones));
        for (int i = 0; i < numBones; ++i) {
            std::memcpy(&ib[static_cast<size_t>(i)],
                        skin0->inverseMatrices + static_cast<size_t>(i) * 16, 64);
            ib[static_cast<size_t>(i)].flags = 0;
        }
        bonePos[0].x = bonePos[0].y = bonePos[0].z = 0.0f;
        uint32 stk[64] = {};
        uint32* stkPtr = stk;
        uint32 curr = 0;
        for (int i = 1; i < numBones; ++i) {
            rw::Matrix invB;
            rw::Matrix::invert(&invB, &ib[static_cast<size_t>(i)]);
            rw::V3d out;
            rw::V3d::transformPoints(&out, &invB.pos, 1, &ib[curr]);
            bonePos[static_cast<size_t>(i)] = out;
            int fl = hh->nodeInfo[i].flags;
            if (fl & 2) { // PUSH (rpHANIMPUSHPARENTMATRIX = 0x02)
                *++stkPtr = curr;
            }
            curr = (fl & 1) ? *stkPtr-- : static_cast<uint32>(i); // POP = 0x01
        }
    }

    // Worked-example bone for the report: prefer Pelvis (tag 1), else first
    // mapped. The logged t is the exact translation put into the animated
    // local matrix (IFP bytes when the sequence carries translation, else
    // the retail BonePos from the skin matrices).
    {
        int ex = -1;
        for (int i = 0; i < numBones; ++i) {
            if (boneTags[static_cast<size_t>(i)] == 1 && boneSeqIdx[static_cast<size_t>(i)] >= 0) {
                ex = i;
                break;
            }
        }
        if (ex < 0) {
            for (int i = 0; i < numBones; ++i) {
                if (boneSeqIdx[static_cast<size_t>(i)] >= 0) {
                    ex = i;
                    break;
                }
            }
        }
        if (ex >= 0) {
            int s = boneSeqIdx[static_cast<size_t>(ex)];
            const Sampled& sm = sampled[static_cast<size_t>(s)];
            const char* cn = BoneTagToName(boneTags[static_cast<size_t>(ex)]);
            (void)std::snprintf(stats.boneName, sizeof(stats.boneName), "%s",
                                 cn ? cn : anim->seqs[static_cast<size_t>(s)].name);
            stats.boneTag = boneTags[static_cast<size_t>(ex)];
            stats.boneQ[0] = sm.q[0];
            stats.boneQ[1] = sm.q[1];
            stats.boneQ[2] = sm.q[2];
            stats.boneQ[3] = sm.q[3];
            if (sm.hasT) {
                stats.boneT[0] = sm.t[0];
                stats.boneT[1] = sm.t[1];
                stats.boneT[2] = sm.t[2];
            } else {
                stats.boneT[0] = bonePos[static_cast<size_t>(ex)].x;
                stats.boneT[1] = bonePos[static_cast<size_t>(ex)].y;
                stats.boneT[2] = bonePos[static_cast<size_t>(ex)].z;
            }
            stats.boneHasTrans = sm.hasT ? 1 : 0;
            stats.boneFrame = sm.frame;
            stats.boneFrames = sm.frames;
        }
    }
    stats.interp = interp ? 1 : 0;

    // Keyaudit (R6k proof of real interpolation): ONE bone with its two
    // bracketing IFP keys and the lerp+slerp value between them. Prefer the
    // Root sequence (tag 0, carries translation so both lerp and slerp are
    // visible), else the first multi-key sequence, else the first valid one.
    // All quats/trans are IFP bytes (normalised for quats); no synthesis.
    {
        int audit = -1;
        for (std::size_t s = 0; s < anim->seqs.size(); ++s) {
            if (EffectiveTag(anim->seqs[s]) == 0 && sampled[s].valid) {
                audit = static_cast<int>(s);
                break;
            }
        }
        if (audit < 0) {
            for (std::size_t s = 0; s < anim->seqs.size(); ++s) {
                if (sampled[s].valid && sampled[s].frames > 1) {
                    audit = static_cast<int>(s);
                    break;
                }
            }
        }
        if (audit < 0) {
            for (std::size_t s = 0; s < anim->seqs.size(); ++s) {
                if (sampled[s].valid) {
                    audit = static_cast<int>(s);
                    break;
                }
            }
        }
        if (audit >= 0) {
            const Sampled& sm = sampled[static_cast<size_t>(audit)];
            const IfpSeq& sq = anim->seqs[static_cast<size_t>(audit)];
            (void)std::snprintf(stats.keyBone, sizeof(stats.keyBone), "%s", sq.name);
            stats.keyTag = EffectiveTag(sq);
            stats.keyK0 = sm.k0;
            stats.keyK1 = sm.k1;
            stats.keyT0 = sm.t0;
            stats.keyT1 = sm.t1;
            stats.keyAlpha = sm.alpha;
            stats.keyTimeAbs = static_cast<float>(stats.timeAbs);
            stats.keyQ0[0] = sm.q0[0];
            stats.keyQ0[1] = sm.q0[1];
            stats.keyQ0[2] = sm.q0[2];
            stats.keyQ0[3] = sm.q0[3];
            stats.keyQ1[0] = sm.q1[0];
            stats.keyQ1[1] = sm.q1[1];
            stats.keyQ1[2] = sm.q1[2];
            stats.keyQ1[3] = sm.q1[3];
            stats.keyQI[0] = sm.q[0];
            stats.keyQI[1] = sm.q[1];
            stats.keyQI[2] = sm.q[2];
            stats.keyQI[3] = sm.q[3];
            stats.keyP0[0] = sm.p0[0];
            stats.keyP0[1] = sm.p0[1];
            stats.keyP0[2] = sm.p0[2];
            stats.keyP1[0] = sm.p1[0];
            stats.keyP1[1] = sm.p1[1];
            stats.keyP1[2] = sm.p1[2];
            stats.keyPI[0] = sm.t[0];
            stats.keyPI[1] = sm.t[1];
            stats.keyPI[2] = sm.t[2];
            stats.keyHasT = sm.hasT ? 1 : 0;
        }
    }

    // Bind skin matrices S_i = IB_i * (Wbind_i * invA).
    std::vector<rw::Matrix> bindSkinMats(static_cast<size_t>(numBones));
    {
        rw::Skin* skin0 = rw::Skin::get(geoms[0].geo);
        for (int i = 0; i < numBones; ++i) {
            rw::Matrix ib;
            std::memcpy(&ib, skin0->inverseMatrices + static_cast<size_t>(i) * 16, 64);
            ib.flags = 0;
            rw::Matrix tmp;
            rw::Matrix::mult(&tmp, &bindWorld[static_cast<size_t>(i)], &invAtomic);
            rw::Matrix::mult(&bindSkinMats[static_cast<size_t>(i)], &ib, &tmp);
        }
    }
    // Bind AABB from skinned positions (same vertex loop, no scene fill).
    bool haveBind = false;
    {
        for (const auto& g : geoms) {
            rw::Skin* skin = rw::Skin::get(g.geo);
            const int nv = g.geo->numVertices;
            rw::V3d* verts = g.geo->morphTargets[0].vertices;
            for (int v = 0; v < nv; ++v) {
                const uint8* idx = skin->indices + static_cast<size_t>(v) * 4;
                const float* wgt = skin->weights + static_cast<size_t>(v) * 4;
                rw::V3d p = { 0.0f, 0.0f, 0.0f };
                for (int k = 0; k < 4; ++k) {
                    int b = idx[k];
                    float w = wgt[k];
                    if (w == 0.0f || b < 0 || b >= numBones) {
                        continue;
                    }
                    rw::V3d tp;
                    rw::V3d::transformPoints(&tp, &verts[v], 1,
                                             &bindSkinMats[static_cast<size_t>(b)]);
                    p.x += w * tp.x;
                    p.y += w * tp.y;
                    p.z += w * tp.z;
                }
                if (!haveBind) {
                    stats.bindMin[0] = stats.bindMax[0] = p.x;
                    stats.bindMin[1] = stats.bindMax[1] = p.y;
                    stats.bindMin[2] = stats.bindMax[2] = p.z;
                    haveBind = true;
                } else {
                    if (p.x < stats.bindMin[0]) {
                        stats.bindMin[0] = p.x;
                    }
                    if (p.y < stats.bindMin[1]) {
                        stats.bindMin[1] = p.y;
                    }
                    if (p.z < stats.bindMin[2]) {
                        stats.bindMin[2] = p.z;
                    }
                    if (p.x > stats.bindMax[0]) {
                        stats.bindMax[0] = p.x;
                    }
                    if (p.y > stats.bindMax[1]) {
                        stats.bindMax[1] = p.y;
                    }
                    if (p.z > stats.bindMax[2]) {
                        stats.bindMax[2] = p.z;
                    }
                }
            }
        }
    }

    // Animated locals: mapped bones get (IFP quat, IFP-or-BonePos pos),
    // unmapped keep the bind local matrix bit-for-bit.
    for (int i = 0; i < numBones; ++i) {
        rw::Frame* f = boneFrames[static_cast<size_t>(i)];
        if (!f) {
            continue;
        }
        int s = boneSeqIdx[static_cast<size_t>(i)];
        if (s < 0 || !sampled[static_cast<size_t>(s)].valid) {
            continue; // identity: keep bind local
        }
        const Sampled& sm = sampled[static_cast<size_t>(s)];
        float pos[3];
        if (sm.hasT) {
            pos[0] = sm.t[0];
            pos[1] = sm.t[1];
            pos[2] = sm.t[2];
        } else {
            pos[0] = bonePos[static_cast<size_t>(i)].x;
            pos[1] = bonePos[static_cast<size_t>(i)].y;
            pos[2] = bonePos[static_cast<size_t>(i)].z;
        }
        rw::Matrix lm;
        QuatPosToMatrix(sm.q, pos, lm);
        f->matrix = lm;
        f->updateObjects();
    }
    std::vector<rw::Matrix> animWorld(static_cast<size_t>(numBones));
    for (int i = 0; i < numBones; ++i) {
        rw::Frame* f = boneFrames[static_cast<size_t>(i)];
        if (!f) {
            animWorld[static_cast<size_t>(i)].setIdentity();
            continue;
        }
        rw::Matrix* ltm = f->getLTM();
        animWorld[static_cast<size_t>(i)] = ltm ? *ltm : f->matrix;
    }
    // Root delta: world pos of tag-0 bone (else hier index 0).
    {
        int ridx = 0;
        for (int i = 0; i < numBones; ++i) {
            if (boneTags[static_cast<size_t>(i)] == 0) {
                ridx = i;
                break;
            }
        }
        float dx = animWorld[static_cast<size_t>(ridx)].pos.x - bindWorld[static_cast<size_t>(ridx)].pos.x;
        float dy = animWorld[static_cast<size_t>(ridx)].pos.y - bindWorld[static_cast<size_t>(ridx)].pos.y;
        float dz = animWorld[static_cast<size_t>(ridx)].pos.z - bindWorld[static_cast<size_t>(ridx)].pos.z;
        stats.rootDelta = std::sqrt(dx * dx + dy * dy + dz * dz);
        stats.rootWorld[0] = animWorld[static_cast<size_t>(ridx)].pos.x;
        stats.rootWorld[1] = animWorld[static_cast<size_t>(ridx)].pos.y;
        stats.rootWorld[2] = animWorld[static_cast<size_t>(ridx)].pos.z;
    }

    // Model placement A (current atomic-frame LTM, DFF file bytes): the
    // atomic is attached to the Pelvis frame, which the pose above moved,
    // so A is re-read post-sync (retail RW evaluates both the skin upload
    // and the world matrix from the live frames at render time).
    // Final render verts = stored * S * A (RW row vectors), like librw's
    // skinRenderCB (setWorldMatrix(atomic->getFrame()->getLTM())).
    // A is file data (andre: cyclic X->Y, Y->Z, Z->X placement); zero
    // manual constants. The bind AABB stays in skin space on purpose so it
    // remains bit-comparable with round-11's ped-load bbox; the anim AABB
    // is in render/world space (what the TGA shows and the gates check).
    rw::Matrix atomicCur;
    if (atomicFrame && atomicFrame->getLTM()) {
        atomicCur = *atomicFrame->getLTM();
    } else {
        atomicCur.setIdentity();
    }
    rw::Matrix invAtomicCur;
    rw::Matrix::invert(&invAtomicCur, &atomicCur);

    // Animated skin matrices S_i = IB_i * (Wanim_i * invAcur), composed
    // with the model matrix: MW_i = S_i * Acur (RW row-vector convention).
    // notsa::bugfixes: gl3skin applies world AFTER skin. Prepending Acur
    // rotates/translates stored vertices into the wrong inverse-bind space,
    // tearing weighted joints (especially hands) even at exact IFP keys.
    std::vector<rw::Matrix> animSkinMats(static_cast<size_t>(numBones));
    {
        rw::Skin* skin0 = rw::Skin::get(geoms[0].geo);
        for (int i = 0; i < numBones; ++i) {
            rw::Matrix ib;
            std::memcpy(&ib, skin0->inverseMatrices + static_cast<size_t>(i) * 16, 64);
            ib.flags = 0;
            rw::Matrix t1, t2;
            rw::Matrix::mult(&t1, &animWorld[static_cast<size_t>(i)], &invAtomicCur);
            rw::Matrix::mult(&t2, &ib, &t1);
            rw::Matrix::mult(&animSkinMats[static_cast<size_t>(i)], &t2, &atomicCur);
        }
    }

    // --- 8. Flatten with ANIMATED matrices into the scene (rendered) ---
#ifdef REALTIME_GAMEPLAY_POSE_AUDIT
    if (auto* audit = s_RealtimePoseAudit) {
        audit->Bones = numBones;
        std::vector<int> parents;
        int parent = -1;
        for (int i = 0; i < numBones; ++i) {
            const auto* frame = boneFrames[i];
            if (parent >= 0 && (!frame || frame->getParent() != boneFrames[parent])) ++audit->ParentMismatches;
            if (hh->nodeInfo[i].flags & rw::HAnimHierarchy::PUSH) parents.push_back(parent);
            parent = i;
            if (hh->nodeInfo[i].flags & rw::HAnimHierarchy::POP) {
                parent = parents.empty() ? -1 : parents.back();
                if (!parents.empty()) parents.pop_back();
            }
            const int sequence = boneSeqIdx[i];
            if (frame && sequence >= 0 && sampled[sequence].valid) {
                const auto& sm = sampled[sequence];
                rw::Matrix rotation;
                rotation.rotate(rw::makeQuat(sm.q[3],sm.q[0],sm.q[1],sm.q[2]),rw::COMBINEREPLACE);
                for (const auto axis : {rw::makeV3d(1,0,0),rw::makeV3d(0,1,0),rw::makeV3d(0,0,1)}) {
                    rw::V3d a,b;
                    rw::V3d::transformVectors(&a,&axis,1,&rotation);
                    rw::V3d::transformVectors(&b,&axis,1,&frame->matrix);
                    audit->MaxQuaternionError=std::max(audit->MaxQuaternionError,std::sqrt(rw::dot(rw::sub(a,b),rw::sub(a,b))));
                }
            }
            rw::Matrix ib, bind, local, skin, legacy;
            std::memcpy(&ib,rw::Skin::get(geoms[0].geo)->inverseMatrices+static_cast<size_t>(i)*16,64);
            ib.flags=0; rw::Matrix::invert(&bind,&ib);
            rw::V3d joint;
            rw::V3d::transformPoints(&joint,&bind.pos,1,&animSkinMats[i]);
            const auto delta=rw::sub(joint,animWorld[i].pos);
            audit->MaxJointError=std::max(audit->MaxJointError,std::sqrt(rw::dot(delta,delta)));
            // Deliberately retain the old expression as a rejecting negative
            // control, NOT a rendering path or an alternate output.
            rw::Matrix::mult(&local,&animWorld[i],&invAtomicCur);
            rw::Matrix::mult(&skin,&ib,&local);
            rw::Matrix::mult(&legacy,&atomicCur,&skin);
            rw::V3d::transformPoints(&joint,&bind.pos,1,&legacy);
            const auto oldDelta=rw::sub(joint,animWorld[i].pos);
            audit->MaxLegacyJointError=std::max(audit->MaxLegacyJointError,std::sqrt(rw::dot(oldDelta,oldDelta)));
        }
    }
#endif
    int meshIndex = 0;
    bool first = true;
    int totalTris = 0;
    double wsumAcc = 0.0;
    long wsumVerts = 0;
    int serial = 0;
    (void)serial;
    for (const auto& g : geoms) {
        rw::Geometry* geo = g.geo;
        const int numVerts = geo->numVertices;
        rw::Skin* skin = rw::Skin::get(geo);
        rw::V3d* verts = geo->morphTargets[0].vertices;
        rw::V3d* norms = (geo->flags & rw::Geometry::NORMALS) ? geo->morphTargets[0].normals : nil;
        rw::TexCoords* uvs = geo->texCoords[0];
        std::vector<rw::V3d> skinnedPos(static_cast<size_t>(numVerts));
        std::vector<rw::V3d> skinnedNrm(norms ? static_cast<size_t>(numVerts) : 0);
        for (int v = 0; v < numVerts; ++v) {
            const uint8* idx = skin->indices + static_cast<size_t>(v) * 4;
            const float* wgt = skin->weights + static_cast<size_t>(v) * 4;
            double wsum = 0.0;
            rw::V3d p = { 0.0f, 0.0f, 0.0f };
            // notsa::bugfixes: weighted normal accumulation starts at zero,
            // like gl3skin; adding a world +Z vector biases every normal.
            rw::V3d n = { 0.0f, 0.0f, 0.0f };
#ifdef REALTIME_GAMEPLAY_POSE_AUDIT
            rw::V3d referencePos = {}, referenceNormal = {};
#endif
            for (int k = 0; k < 4; ++k) {
                int b = idx[k];
                float w = wgt[k];
                wsum += w;
                if (w == 0.0f || b < 0 || b >= numBones) {
#ifdef REALTIME_GAMEPLAY_POSE_AUDIT
                    if (s_RealtimePoseAudit && w != 0.0f) ++s_RealtimePoseAudit->InvalidInfluences;
#endif
                    continue;
                }
#ifdef REALTIME_GAMEPLAY_POSE_AUDIT
                if (s_RealtimePoseAudit) {
                    rw::Matrix ib;
                    std::memcpy(&ib,skin->inverseMatrices+static_cast<size_t>(b)*16,64); ib.flags=0;
                    rw::V3d local, world;
                    rw::V3d::transformPoints(&local,&verts[v],1,&ib);
                    rw::V3d::transformPoints(&world,&local,1,&animWorld[b]);
                    referencePos=rw::add(referencePos,rw::scale(world,w));
                    if (norms) {
                        rw::V3d::transformVectors(&local,&norms[v],1,&ib);
                        rw::V3d::transformVectors(&world,&local,1,&animWorld[b]);
                        referenceNormal=rw::add(referenceNormal,rw::scale(world,w));
                    }
                }
#endif
                rw::V3d tp;
                rw::V3d::transformPoints(&tp, &verts[v], 1, &animSkinMats[static_cast<size_t>(b)]);
                p.x += w * tp.x;
                p.y += w * tp.y;
                p.z += w * tp.z;
                if (norms) {
                    rw::V3d tn;
                    rw::V3d::transformVectors(&tn, &norms[v], 1, &animSkinMats[static_cast<size_t>(b)]);
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
#ifdef REALTIME_GAMEPLAY_POSE_AUDIT
            if (auto* audit=s_RealtimePoseAudit) {
                ++audit->Vertices;
                auto delta=rw::sub(p,referencePos);
                audit->MaxVertexError=std::max(audit->MaxVertexError,std::sqrt(rw::dot(delta,delta)));
                if (norms && rw::dot(referenceNormal,referenceNormal)>1e-12f) {
                    referenceNormal=rw::scale(referenceNormal,1.0f/std::sqrt(rw::dot(referenceNormal,referenceNormal)));
                    delta=rw::sub(n,referenceNormal);
                    audit->MaxNormalError=std::max(audit->MaxNormalError,std::sqrt(rw::dot(delta,delta)));
                }
            }
#endif
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
            continue;
        }
        totalTris += mesh.tris;
        scene.meshes.push_back(std::move(mesh));
        ++meshIndex;
    }
    TexSample_FreeLinked(lc);
    if (totalTris <= 0) {
        char msg[192];
        (void)std::snprintf(msg, sizeof(msg), "no animated triangles flattened from '%.127s'",
                             stats.src);
        SetErr(err, errSize, msg);
        return false;
    }
    stats.tris = totalTris;
    stats.verts = totalTris * 3;
    stats.wsum = wsumVerts > 0 ? wsumAcc / wsumVerts : 0.0;
    stats.animMin[0] = scene.bboxMin[0];
    stats.animMin[1] = scene.bboxMin[1];
    stats.animMin[2] = scene.bboxMin[2];
    stats.animMax[0] = scene.bboxMax[0];
    stats.animMax[1] = scene.bboxMax[1];
    stats.animMax[2] = scene.bboxMax[2];
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

void IfpAnim_Shutdown() {
    for (rw::TexDictionary* t : s_txds) {
        if (t) {
            t->destroy();
        }
    }
    s_txds.clear();
}

double IfpAnim_SeqTimeFrac(int idx, int count) {
    if (count <= 1) {
        return 0.0;
    }
    if (idx < 0) {
        idx = 0;
    }
    if (idx >= count) {
        idx = count - 1;
    }
    return static_cast<double>(idx) / static_cast<double>(count - 1);
}

bool IfpAnim_Seq(const char* gameDir, const char* model, const char* animName, int frames,
                 std::vector<IfpAnimSeqFrame>& out, char* err, std::size_t errSize) {
    out.clear();
    if (!gameDir || !gameDir[0]) {
        SetErr(err, errSize, "no game dir");
        return false;
    }
    if (frames <= 0 || frames > 64) {
        SetErr(err, errSize, "bad --frames (want 1..64)");
        return false;
    }
    out.reserve(static_cast<size_t>(frames));
    for (int i = 0; i < frames; ++i) {
        double tf = IfpAnim_SeqTimeFrac(i, frames);
        IfpAnimSeqFrame fr;
        char lerr[256] = {};
        if (!IfpAnim_Init(gameDir, model, animName, tf, fr.scene, fr.stats, lerr, sizeof(lerr),
                           true /*interp=lerp+slerp*/)) {
            SetErr(err, errSize, lerr[0] ? lerr : "seq frame init failed");
            out.clear();
            return false;
        }
        out.push_back(std::move(fr));
    }
    return true;
}

// Joint-space loop gap from IFP bytes only: samples the named animation at
// T=0 and T=1 with the same lerp+slerp bracketing as IfpAnim_Init(interp)
// and averages per-sequence (trans distance in metres + quat angle in
// radians). Rotation-only sequences contribute angle only (their BonePos
// cancels); translation-carrying sequences (Root) contribute both. Pure
// function of the bank bytes; deterministic.
bool IfpAnim_LoopGap(const char* gameDir, const char* animName, float* gapOut, int* mappedOut,
                     char* err, std::size_t errSize) {
    if (!gameDir || !gameDir[0]) {
        SetErr(err, errSize, "no game dir");
        return false;
    }
    if (!gapOut) {
        SetErr(err, errSize, "no gap output");
        return false;
    }
    std::string game(gameDir);
    OS_SetFilePathOffset(game.c_str());
    s_gameAbs = game;
    std::vector<uint8> bankBytes;
    std::string srcLabel;
    {
        char lerr[256] = {};
        if (!LoadBankBytes(gameDir, "ped", bankBytes, srcLabel, lerr, sizeof(lerr))) {
            SetErr(err, errSize, lerr);
            return false;
        }
    }
    std::string bankName;
    std::vector<IfpAnimData> bank;
    if (!ParseIfpBank(bankBytes, bankName, bank, err, errSize)) {
        return false;
    }
    std::string want = animName && animName[0] ? animName : "WALK_civi";
    std::string wantLower = ToLowerCopy(want.c_str());
    const IfpAnimData* anim = nil;
    for (const auto& a : bank) {
        if (ToLowerCopy(a.name) == wantLower) {
            anim = &a;
            break;
        }
    }
    if (!anim) {
        char msg[192];
        (void)std::snprintf(msg, sizeof(msg), "animation '%s' not in ped bank (%d anims)", want.c_str(),
                             static_cast<int>(bank.size()));
        SetErr(err, errSize, msg);
        return false;
    }
    auto sampleAt = [&](double tAbs, std::vector<std::array<float, 4>>& qs,
                        std::vector<std::array<float, 3>>& ps, std::vector<char>& hasT) {
        qs.resize(anim->seqs.size());
        ps.resize(anim->seqs.size());
        hasT.resize(anim->seqs.size());
        for (std::size_t s = 0; s < anim->seqs.size(); ++s) {
            const IfpSeq& sq = anim->seqs[s];
            float tq = static_cast<float>(tAbs);
            if (sq.frames.empty()) {
                qs[s] = { 0.0f, 0.0f, 0.0f, 1.0f };
                ps[s] = { 0.0f, 0.0f, 0.0f };
                hasT[s] = 0;
                continue;
            }
            if (sq.frames.size() == 1) {
                float nq[4] = { sq.frames[0].q[0], sq.frames[0].q[1], sq.frames[0].q[2],
                                sq.frames[0].q[3] };
                NormQuat4(nq);
                qs[s] = { nq[0], nq[1], nq[2], nq[3] };
                ps[s] = { sq.frames[0].t[0], sq.frames[0].t[1], sq.frames[0].t[2] };
                hasT[s] = sq.frames[0].hasT ? 1 : 0;
                continue;
            }
            std::size_t k0 = 0;
            std::size_t k1 = 0;
            float alpha = 0.0f;
            if (tq <= sq.frames.front().absTime) {
                k0 = k1 = 0;
            } else if (tq >= sq.frames.back().absTime) {
                k0 = k1 = sq.frames.size() - 1;
            } else {
                for (std::size_t k = 0; k + 1 < sq.frames.size(); ++k) {
                    float a = sq.frames[k].absTime;
                    float b = sq.frames[k + 1].absTime;
                    if (a <= tq && tq <= b) {
                        k0 = k;
                        k1 = k + 1;
                        alpha = (b > a) ? (tq - a) / (b - a) : 0.0f;
                        break;
                    }
                }
            }
            const IfpFrame& f0 = sq.frames[k0];
            const IfpFrame& f1 = sq.frames[k1];
            float n0[4] = { f0.q[0], f0.q[1], f0.q[2], f0.q[3] };
            float n1[4] = { f1.q[0], f1.q[1], f1.q[2], f1.q[3] };
            NormQuat4(n0);
            NormQuat4(n1);
            float qi[4];
            if (k0 == k1 || alpha == 0.0f) {
                qi[0] = n0[0];
                qi[1] = n0[1];
                qi[2] = n0[2];
                qi[3] = n0[3];
            } else {
                SlerpQuat(n0, n1, alpha, qi);
            }
            NormQuat4(qi);
            qs[s] = { qi[0], qi[1], qi[2], qi[3] };
            bool ht = f0.hasT || f1.hasT;
            hasT[s] = ht ? 1 : 0;
            if (ht) {
                ps[s] = { f0.t[0] + alpha * (f1.t[0] - f0.t[0]),
                          f0.t[1] + alpha * (f1.t[1] - f0.t[1]),
                          f0.t[2] + alpha * (f1.t[2] - f0.t[2]) };
            } else {
                ps[s] = { 0.0f, 0.0f, 0.0f };
            }
        }
    };
    std::vector<std::array<float, 4>> q0;
    std::vector<std::array<float, 3>> p0;
    std::vector<char> h0;
    std::vector<std::array<float, 4>> q1;
    std::vector<std::array<float, 3>> p1;
    std::vector<char> h1;
    sampleAt(0.0, q0, p0, h0);
    sampleAt(anim->total, q1, p1, h1);
    double acc = 0.0;
    int n = 0;
    for (std::size_t s = 0; s < anim->seqs.size(); ++s) {
        if (anim->seqs[s].frames.empty()) {
            continue;
        }
        float a[4] = { q0[s][0], q0[s][1], q0[s][2], q0[s][3] };
        float b[4] = { q1[s][0], q1[s][1], q1[s][2], q1[s][3] };
        float ang = QuatAngle(a, b);
        double td = 0.0;
        if (h0[s] || h1[s]) {
            double dx = static_cast<double>(p1[s][0]) - p0[s][0];
            double dy = static_cast<double>(p1[s][1]) - p0[s][1];
            double dz = static_cast<double>(p1[s][2]) - p0[s][2];
            td = std::sqrt(dx * dx + dy * dy + dz * dz);
        }
        acc += td + static_cast<double>(ang);
        ++n;
    }
    if (n <= 0) {
        SetErr(err, errSize, "no sequences for loop gap");
        return false;
    }
    *gapOut = static_cast<float>(acc / n);
    if (mappedOut) {
        *mappedOut = n;
    }
    return true;
}

// --- R6q blender (round 19) helpers. All verbatim mirrors of the Init path
// above (same functions, same float order) so endpoint frames stay
// bit-identical to --shot-anim. ---

struct BlendGeom {
    rw::Geometry* geo = nil;
    rw::Atomic* atomic = nil;
};

// Rotation part (right/up/at as rows, the QuatPosToMatrix convention) ->
// quat (x,y,z,w). Standard Shepperd extraction, then normalised. Round-trips
// QuatPosToMatrix up to sign; only feeds interior slerp endpoints of
// partially-mapped bones and the morph/audit full poses (DFF bind bytes,
// never synthesised angles).
void MatToQuat(const rw::Matrix& m, float q[4]) {
    float m00 = m.right.x;
    float m01 = m.right.y;
    float m02 = m.right.z;
    float m10 = m.up.x;
    float m11 = m.up.y;
    float m12 = m.up.z;
    float m20 = m.at.x;
    float m21 = m.at.y;
    float m22 = m.at.z;
    float tr = m00 + m11 + m22;
    if (tr > 0.0f) {
        float s = std::sqrt(tr + 1.0f) * 2.0f;
        q[3] = 0.25f * s;
        q[0] = (m21 - m12) / s;
        q[1] = (m02 - m20) / s;
        q[2] = (m10 - m01) / s;
    } else if (m00 > m11 && m00 > m22) {
        float s = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
        q[3] = (m21 - m12) / s;
        q[0] = 0.25f * s;
        q[1] = (m01 + m10) / s;
        q[2] = (m02 + m20) / s;
    } else if (m11 > m22) {
        float s = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
        q[3] = (m02 - m20) / s;
        q[0] = (m01 + m10) / s;
        q[1] = 0.25f * s;
        q[2] = (m12 + m21) / s;
    } else {
        float s = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
        q[3] = (m10 - m01) / s;
        q[0] = (m02 + m20) / s;
        q[1] = (m12 + m21) / s;
        q[2] = 0.25f * s;
    }
    NormQuat4(q);
}

struct BlendSampled {
    float q[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    float t[3] = { 0.0f, 0.0f, 0.0f };
    bool hasT = false;
    bool valid = false;
    int frame = 0;
    int frames = 0;
};

// Legacy single-key sampling (R6j etalon behaviour verbatim): first key with
// absTime >= tAbs, quats normalised, no lerp/slerp.
void SampleLegacyAnim(const IfpAnimData& anim, double tAbs, std::vector<BlendSampled>& out) {
    out.clear();
    out.resize(anim.seqs.size());
    float tAbsF = static_cast<float>(tAbs);
    (void)tAbsF;
    for (std::size_t s = 0; s < anim.seqs.size(); ++s) {
        const IfpSeq& sq = anim.seqs[s];
        BlendSampled sm;
        sm.frames = static_cast<int>(sq.frames.size());
        if (sq.frames.empty()) {
            out[s] = sm;
            continue;
        }
        if (sq.frames.size() == 1) {
            const IfpFrame& fr = sq.frames[0];
            float nq[4] = { fr.q[0], fr.q[1], fr.q[2], fr.q[3] };
            NormQuat4(nq);
            sm.q[0] = nq[0];
            sm.q[1] = nq[1];
            sm.q[2] = nq[2];
            sm.q[3] = nq[3];
            sm.hasT = fr.hasT;
            sm.t[0] = fr.t[0];
            sm.t[1] = fr.t[1];
            sm.t[2] = fr.t[2];
            sm.frame = 0;
            sm.valid = true;
            out[s] = sm;
            continue;
        }
        float tA = static_cast<float>(tAbs);
        std::size_t pick = sq.frames.size() - 1;
        for (std::size_t k = 0; k < sq.frames.size(); ++k) {
            if (sq.frames[k].absTime >= tA) {
                pick = k;
                break;
            }
        }
        const IfpFrame& fr = sq.frames[pick];
        float nq[4] = { fr.q[0], fr.q[1], fr.q[2], fr.q[3] };
        NormQuat4(nq);
        sm.q[0] = nq[0];
        sm.q[1] = nq[1];
        sm.q[2] = nq[2];
        sm.q[3] = nq[3];
        sm.hasT = fr.hasT;
        sm.t[0] = fr.t[0];
        sm.t[1] = fr.t[1];
        sm.t[2] = fr.t[2];
        sm.frame = static_cast<int>(pick);
        sm.valid = true;
        out[s] = sm;
    }
}

struct BlendFlatOut {
    int tris = 0;
    int verts = 0;
    double wsum = 0.0;
};

// Flatten with the given per-bone skin matrices (verbatim copy of the Init
// render flatten: same mesh order, colors, texture resolution, bbox).
void FlattenWithMats(const std::vector<BlendGeom>& geoms, const std::vector<rw::Matrix>& skinMats,
                     int numBones, LinkedClump& lc, WorldShotScene& scene, const char* srcLabel,
                     const char* txdLabel, int textures, BlendFlatOut& fo) {
    scene.meshes.clear();
    scene.images.clear();
    int meshIndex = 0;
    bool first = true;
    int totalTris = 0;
    double wsumAcc = 0.0;
    long wsumVerts = 0;
    for (const auto& g : geoms) {
        rw::Geometry* geo = g.geo;
        const int numVerts = geo->numVertices;
        rw::Skin* skin = rw::Skin::get(geo);
        rw::V3d* verts = geo->morphTargets[0].vertices;
        rw::V3d* norms = (geo->flags & rw::Geometry::NORMALS) ? geo->morphTargets[0].normals : nil;
        rw::TexCoords* uvs = geo->texCoords[0];
        std::vector<rw::V3d> skinnedPos(static_cast<size_t>(numVerts));
        std::vector<rw::V3d> skinnedNrm(norms ? static_cast<size_t>(numVerts) : 0);
        for (int v = 0; v < numVerts; ++v) {
            const uint8* idx = skin->indices + static_cast<size_t>(v) * 4;
            const float* wgt = skin->weights + static_cast<size_t>(v) * 4;
            double wsum = 0.0;
            rw::V3d p = { 0.0f, 0.0f, 0.0f };
            rw::V3d n = { 0.0f, 0.0f, 0.0f }; // same zero-based weighted normal as Init
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
            continue;
        }
        totalTris += mesh.tris;
        scene.meshes.push_back(std::move(mesh));
        ++meshIndex;
    }
    fo.tris = totalTris;
    fo.verts = totalTris * 3;
    fo.wsum = wsumVerts > 0 ? wsumAcc / wsumVerts : 0.0;
    (void)std::snprintf(scene.stats.dffName, sizeof(scene.stats.dffName), "%.127s", srcLabel);
    (void)std::snprintf(scene.stats.txdName, sizeof(scene.stats.txdName), "%.127s", txdLabel);
    scene.stats.atomics = meshIndex;
    scene.stats.triangles = totalTris;
    scene.stats.vertices = totalTris * 3;
    scene.stats.textures = textures;
    scene.stats.firstTexture[0] = '\0';
    scene.stats.firstTexW = 0;
    scene.stats.firstTexH = 0;
    if (!scene.images.empty()) {
        (void)std::snprintf(scene.stats.firstTexture, sizeof(scene.stats.firstTexture), "%s",
                             scene.images[0].name);
        scene.stats.firstTexW = scene.images[0].w;
        scene.stats.firstTexH = scene.images[0].h;
    }
}

bool IfpAnim_Blend(const char* gameDir, const char* model, const char* fromAnim, const char* toAnim,
                   int frames, IfpAnimBlendResult& out, char* err, std::size_t errSize) {
    out = IfpAnimBlendResult{};
    if (!gameDir || !gameDir[0]) {
        SetErr(err, errSize, "no game dir");
        return false;
    }
    if (frames < 2 || frames > 64) {
        SetErr(err, errSize, "bad --frames (want 2..64 for blend)");
        return false;
    }
    std::string want = model && model[0] ? model : "andre";
    std::string wantLower = want;
    ToLowerInPlace(wantLower);
    std::string fromWant = fromAnim && fromAnim[0] ? fromAnim : "IDLE_stance";
    std::string toWant = toAnim && toAnim[0] ? toAnim : "WALK_civi";
    std::string fromLower = ToLowerCopy(fromWant.c_str());
    std::string toLower = ToLowerCopy(toWant.c_str());
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

    // --- 1. DFF bytes (SkinPed-identical order: direct, then gta3 fallback) ---
    std::vector<uint8> dffBytes;
    std::string resolved = wantLower;
    std::string dffFile = wantLower + ".dff";
    const ImgIndex* hitImg = nil;
    int tried = 0;
    for (const ImgIndex& idx : imgs) {
        if (ImgReadBytesStd(idx, dffFile, dffBytes)) {
            hitImg = &idx;
            break;
        }
    }
    std::string srcLabel;
    if (dffBytes.empty()) {
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
        if (dffBytes.empty()) {
            char msg[256];
            (void)std::snprintf(msg, sizeof(msg),
                                 "ped fallback scan found no skinned DFF in gta3.img (tried=%d)", tried);
            SetErr(err, errSize, msg);
            return false;
        }
        char sl[192];
        (void)std::snprintf(sl, sizeof(sl), "%s:%s", hitImg->label.c_str(), pickLocal.c_str());
        srcLabel = sl;
    } else {
        for (const ImgEntry& e : hitImg->entries) {
            if (e.nameLower == dffFile) {
                char sl[192];
                (void)std::snprintf(sl, sizeof(sl), "%s:%s", hitImg->label.c_str(), e.name.c_str());
                srcLabel = sl;
                break;
            }
        }
    }

    // --- 2. Per-model TXD ---
    std::string txdFile = resolved + ".txd";
    std::string txdLabel = "none";
    int nTextures = 0;
    rw::TexDictionary* primary = nil;
    {
        std::vector<uint8> txdBytes;
        if (hitImg && ImgReadBytesStd(*hitImg, txdFile, txdBytes)) {
            rw::TexDictionary* txd = ParseTxd(txdBytes);
            if (txd && txd->count() > 0) {
                primary = txd;
                s_txds.push_back(txd);
                nTextures = txd->count();
                char tl[192];
                (void)std::snprintf(tl, sizeof(tl), "%s:%s", hitImg->label.c_str(), txdFile.c_str());
                txdLabel = tl;
            } else {
                if (txd) {
                    txd->destroy();
                }
            }
        }
    }

    // --- 3. Clump + hierarchy (bind pose as streamed) ---
    LinkedClump lc = TexSample_LinkedParse(dffBytes.data(), dffBytes.size(), primary, nil, 0);
    if (!lc.clump) {
        TexSample_FreeLinked(lc);
        SetErr(err, errSize, "DFF parse produced no clump (not a RenderWare clump?)");
        return false;
    }
    if (!ClumpHasSkin(lc.clump)) {
        TexSample_FreeLinked(lc);
        char msg[192];
        (void)std::snprintf(msg, sizeof(msg), "DFF '%s' has no skinned geometry", srcLabel.c_str());
        SetErr(err, errSize, msg);
        return false;
    }
    rw::Frame* root = lc.clump->getFrame();
    int nFrames = root ? root->count() : 0;
    rw::HAnimHierarchy* hh = nil;
    {
        FORLIST(link, lc.clump->atomics) {
            rw::Atomic* at = rw::Atomic::fromClump(link);
            rw::HAnimHierarchy* h = rw::Skin::getHierarchy(at);
            if (h) {
                hh = h;
                break;
            }
        }
        if (!hh && root) {
            hh = rw::HAnimHierarchy::find(root);
        }
    }
    if (!hh || hh->numNodes <= 0 || !hh->nodeInfo) {
        TexSample_FreeLinked(lc);
        SetErr(err, errSize, "no HAnim hierarchy for skinned ped");
        return false;
    }
    const int numBones = hh->numNodes;
    hh->attach();
    int attached = 0;
    for (int i = 0; i < numBones; ++i) {
        if (hh->nodeInfo[i].frame) {
            ++attached;
        }
    }
    if (attached == 0) {
        TexSample_FreeLinked(lc);
        SetErr(err, errSize, "HAnim hierarchy attached to no frames");
        return false;
    }

    std::vector<int32> boneTags(static_cast<size_t>(numBones), -1);
    std::vector<rw::Frame*> boneFrames(static_cast<size_t>(numBones), nil);
    std::vector<rw::Matrix> bindLocals(static_cast<size_t>(numBones));
    std::vector<rw::Matrix> bindWorld(static_cast<size_t>(numBones));
    for (int i = 0; i < numBones; ++i) {
        boneTags[static_cast<size_t>(i)] = hh->nodeInfo[i].id;
        boneFrames[static_cast<size_t>(i)] = hh->nodeInfo[i].frame;
        rw::Frame* f = hh->nodeInfo[i].frame;
        if (f) {
            bindLocals[static_cast<size_t>(i)] = f->matrix;
            rw::Matrix* ltm = f->getLTM();
            bindWorld[static_cast<size_t>(i)] = ltm ? *ltm : f->matrix;
        } else {
            bindLocals[static_cast<size_t>(i)].setIdentity();
            bindWorld[static_cast<size_t>(i)].setIdentity();
        }
    }

    rw::Atomic* firstAtomic = nil;
    FORLIST(link, lc.clump->atomics) {
        firstAtomic = rw::Atomic::fromClump(link);
        break;
    }
    rw::Frame* atomicFrame = firstAtomic ? firstAtomic->getFrame() : nil;
    rw::Matrix atomicMat;
    if (atomicFrame && atomicFrame->getLTM()) {
        atomicMat = *atomicFrame->getLTM();
    } else {
        atomicMat.setIdentity();
    }
    rw::Matrix invAtomic;
    rw::Matrix::invert(&invAtomic, &atomicMat);
    (void)invAtomic;

    // --- 4. IFP bank + both animation lookups ---
    std::vector<uint8> bankBytes;
    std::string bankSrc;
    {
        char lerr[256] = {};
        if (!LoadBankBytes(gameDir, "ped", bankBytes, bankSrc, lerr, sizeof(lerr))) {
            TexSample_FreeLinked(lc);
            SetErr(err, errSize, lerr);
            return false;
        }
    }
    std::string bankName;
    std::vector<IfpAnimData> bank;
    if (!ParseIfpBank(bankBytes, bankName, bank, err, errSize)) {
        TexSample_FreeLinked(lc);
        return false;
    }
    if (bankName.empty()) {
        bankName = "ped";
    }
    const IfpAnimData* animA = nil;
    const IfpAnimData* animB = nil;
    for (const auto& a : bank) {
        if (!animA && ToLowerCopy(a.name) == fromLower) {
            animA = &a;
        }
        if (!animB && ToLowerCopy(a.name) == toLower) {
            animB = &a;
        }
    }
    if (!animA || !animB) {
        TexSample_FreeLinked(lc);
        char msg[256];
        (void)std::snprintf(msg, sizeof(msg), "blend anim missing from=%s(%s) to=%s(%s) bank=%d",
                             fromWant.c_str(), animA ? "ok" : "MISSING", toWant.c_str(),
                             animB ? "ok" : "MISSING", static_cast<int>(bank.size()));
        SetErr(err, errSize, msg);
        return false;
    }
    (void)std::snprintf(out.fromAnim, sizeof(out.fromAnim), "%s", animA->name);
    (void)std::snprintf(out.toAnim, sizeof(out.toAnim), "%s", animB->name);

    // Pose A = --shot-anim path: legacy sample of fromAnim at T=0.5.
    // Pose B = first frame of toAnim: legacy sample at T=0.0.
    std::vector<BlendSampled> sampA;
    std::vector<BlendSampled> sampB;
    SampleLegacyAnim(*animA, 0.5 * animA->total, sampA);
    SampleLegacyAnim(*animB, 0.0, sampB);

    // Tag -> seq maps per animation (first wins).
    std::map<int32, std::size_t> tagToSeqA;
    std::map<std::string, std::size_t> nameToSeqA;
    for (std::size_t s = 0; s < animA->seqs.size(); ++s) {
        int32 et = EffectiveTag(animA->seqs[s]);
        if (et != -1 && tagToSeqA.find(et) == tagToSeqA.end()) {
            tagToSeqA[et] = s;
        }
        std::string nl = ToLowerCopy(animA->seqs[s].name);
        if (nameToSeqA.find(nl) == nameToSeqA.end()) {
            nameToSeqA[nl] = s;
        }
    }
    std::map<int32, std::size_t> tagToSeqB;
    std::map<std::string, std::size_t> nameToSeqB;
    for (std::size_t s = 0; s < animB->seqs.size(); ++s) {
        int32 et = EffectiveTag(animB->seqs[s]);
        if (et != -1 && tagToSeqB.find(et) == tagToSeqB.end()) {
            tagToSeqB[et] = s;
        }
        std::string nl = ToLowerCopy(animB->seqs[s].name);
        if (nameToSeqB.find(nl) == nameToSeqB.end()) {
            nameToSeqB[nl] = s;
        }
    }
    auto pickSeq = [](int32 tag, const std::map<int32, std::size_t>& tm,
                      const std::map<std::string, std::size_t>& nm) -> int {
        auto it = tm.find(tag);
        if (it != tm.end()) {
            return static_cast<int>(it->second);
        }
        const char* cn = BoneTagToName(tag);
        if (cn) {
            std::string cl = ToLowerCopy(cn);
            auto jt = nm.find(cl);
            if (jt != nm.end()) {
                return static_cast<int>(jt->second);
            }
        }
        return -1;
    };
    std::vector<int> mapA(static_cast<size_t>(numBones), -1);
    std::vector<int> mapB(static_cast<size_t>(numBones), -1);
    for (int i = 0; i < numBones; ++i) {
        mapA[static_cast<size_t>(i)] = pickSeq(boneTags[static_cast<size_t>(i)], tagToSeqA, nameToSeqA);
        mapB[static_cast<size_t>(i)] = pickSeq(boneTags[static_cast<size_t>(i)], tagToSeqB, nameToSeqB);
        if (mapA[static_cast<size_t>(i)] >= 0 &&
            !sampA[static_cast<size_t>(mapA[static_cast<size_t>(i)])].valid) {
            mapA[static_cast<size_t>(i)] = -1;
        }
        if (mapB[static_cast<size_t>(i)] >= 0 &&
            !sampB[static_cast<size_t>(mapB[static_cast<size_t>(i)])].valid) {
            mapB[static_cast<size_t>(i)] = -1;
        }
    }

    // Retail BonePos (verbatim from Init): HAnim-local bind translations from
    // the skin inverse-bind matrices; animated local translation for
    // rotation-only IFP bones.
    std::vector<rw::V3d> bonePos(static_cast<size_t>(numBones), { 0.0f, 0.0f, 0.0f });
    std::vector<BlendGeom> geoms;
    FORLIST(link, lc.clump->atomics) {
        rw::Atomic* at = rw::Atomic::fromClump(link);
        rw::Geometry* geo = at ? at->geometry : nil;
        if (!geo || geo->numTriangles <= 0 || geo->numVertices <= 0) {
            continue;
        }
        if (!geo->triangles || !geo->morphTargets || !geo->morphTargets[0].vertices) {
            continue;
        }
        rw::Skin* skin = rw::Skin::get(geo);
        if (!skin || skin->numBones <= 0 || !skin->indices || !skin->weights ||
            !skin->inverseMatrices) {
            continue;
        }
        if (skin->numBones != numBones) {
            continue;
        }
        geoms.push_back({ geo, at });
    }
    if (geoms.empty()) {
        TexSample_FreeLinked(lc);
        SetErr(err, errSize, "no skinned geometry matching the hierarchy");
        return false;
    }
    {
        rw::Skin* skin0 = rw::Skin::get(geoms[0].geo);
        std::vector<rw::Matrix> ib(static_cast<size_t>(numBones));
        for (int i = 0; i < numBones; ++i) {
            std::memcpy(&ib[static_cast<size_t>(i)],
                        skin0->inverseMatrices + static_cast<size_t>(i) * 16, 64);
            ib[static_cast<size_t>(i)].flags = 0;
        }
        bonePos[0].x = bonePos[0].y = bonePos[0].z = 0.0f;
        uint32 stk[64] = {};
        uint32* stkPtr = stk;
        uint32 curr = 0;
        for (int i = 1; i < numBones; ++i) {
            rw::Matrix invB;
            rw::Matrix::invert(&invB, &ib[static_cast<size_t>(i)]);
            rw::V3d outp;
            rw::V3d::transformPoints(&outp, &invB.pos, 1, &ib[curr]);
            bonePos[static_cast<size_t>(i)] = outp;
            int fl = hh->nodeInfo[i].flags;
            if (fl & 2) {
                *++stkPtr = curr;
            }
            curr = (fl & 1) ? *stkPtr-- : static_cast<uint32>(i);
        }
    }

    // Full endpoint poses per bone (IFP sample where mapped, else bind).
    struct FullQ {
        float q[4];
        float t[3];
        bool mapped = false;
    };
    std::vector<FullQ> fullA(static_cast<size_t>(numBones));
    std::vector<FullQ> fullB(static_cast<size_t>(numBones));
    std::vector<int> isMappedEither(static_cast<size_t>(numBones), 0);
    for (int i = 0; i < numBones; ++i) {
        size_t bi = static_cast<size_t>(i);
        float bq[4];
        MatToQuat(bindLocals[bi], bq);
        int sA = mapA[bi];
        if (sA >= 0) {
            const BlendSampled& sm = sampA[static_cast<size_t>(sA)];
            fullA[bi].q[0] = sm.q[0];
            fullA[bi].q[1] = sm.q[1];
            fullA[bi].q[2] = sm.q[2];
            fullA[bi].q[3] = sm.q[3];
            if (sm.hasT) {
                fullA[bi].t[0] = sm.t[0];
                fullA[bi].t[1] = sm.t[1];
                fullA[bi].t[2] = sm.t[2];
            } else {
                fullA[bi].t[0] = bonePos[bi].x;
                fullA[bi].t[1] = bonePos[bi].y;
                fullA[bi].t[2] = bonePos[bi].z;
            }
            fullA[bi].mapped = true;
        } else {
            fullA[bi].q[0] = bq[0];
            fullA[bi].q[1] = bq[1];
            fullA[bi].q[2] = bq[2];
            fullA[bi].q[3] = bq[3];
            fullA[bi].t[0] = bindLocals[bi].pos.x;
            fullA[bi].t[1] = bindLocals[bi].pos.y;
            fullA[bi].t[2] = bindLocals[bi].pos.z;
        }
        int sB = mapB[bi];
        if (sB >= 0) {
            const BlendSampled& sm = sampB[static_cast<size_t>(sB)];
            fullB[bi].q[0] = sm.q[0];
            fullB[bi].q[1] = sm.q[1];
            fullB[bi].q[2] = sm.q[2];
            fullB[bi].q[3] = sm.q[3];
            if (sm.hasT) {
                fullB[bi].t[0] = sm.t[0];
                fullB[bi].t[1] = sm.t[1];
                fullB[bi].t[2] = sm.t[2];
            } else {
                fullB[bi].t[0] = bonePos[bi].x;
                fullB[bi].t[1] = bonePos[bi].y;
                fullB[bi].t[2] = bonePos[bi].z;
            }
            fullB[bi].mapped = true;
        } else {
            fullB[bi].q[0] = bq[0];
            fullB[bi].q[1] = bq[1];
            fullB[bi].q[2] = bq[2];
            fullB[bi].q[3] = bq[3];
            fullB[bi].t[0] = bindLocals[bi].pos.x;
            fullB[bi].t[1] = bindLocals[bi].pos.y;
            fullB[bi].t[2] = bindLocals[bi].pos.z;
        }
        isMappedEither[bi] = (fullA[bi].mapped || fullB[bi].mapped) ? 1 : 0;
    }
    int mappedEither = 0;
    for (int v : isMappedEither) {
        mappedEither += v;
    }

    // Bind skin matrices + bind AABB (verbatim from Init, once).
    std::vector<rw::Matrix> bindSkinMats(static_cast<size_t>(numBones));
    {
        rw::Skin* skin0 = rw::Skin::get(geoms[0].geo);
        for (int i = 0; i < numBones; ++i) {
            rw::Matrix ib;
            std::memcpy(&ib, skin0->inverseMatrices + static_cast<size_t>(i) * 16, 64);
            ib.flags = 0;
            rw::Matrix tmp;
            rw::Matrix::mult(&tmp, &bindWorld[static_cast<size_t>(i)], &invAtomic);
            rw::Matrix::mult(&bindSkinMats[static_cast<size_t>(i)], &ib, &tmp);
        }
    }
    float bindMin[3] = { 0.0f, 0.0f, 0.0f };
    float bindMax[3] = { 0.0f, 0.0f, 0.0f };
    {
        bool haveBind = false;
        for (const auto& g : geoms) {
            rw::Skin* skin = rw::Skin::get(g.geo);
            const int nv = g.geo->numVertices;
            rw::V3d* verts = g.geo->morphTargets[0].vertices;
            for (int v = 0; v < nv; ++v) {
                const uint8* idx = skin->indices + static_cast<size_t>(v) * 4;
                const float* wgt = skin->weights + static_cast<size_t>(v) * 4;
                rw::V3d p = { 0.0f, 0.0f, 0.0f };
                for (int k = 0; k < 4; ++k) {
                    int b = idx[k];
                    float w = wgt[k];
                    if (w == 0.0f || b < 0 || b >= numBones) {
                        continue;
                    }
                    rw::V3d tp;
                    rw::V3d::transformPoints(&tp, &verts[v], 1,
                                             &bindSkinMats[static_cast<size_t>(b)]);
                    p.x += w * tp.x;
                    p.y += w * tp.y;
                    p.z += w * tp.z;
                }
                if (!haveBind) {
                    bindMin[0] = bindMax[0] = p.x;
                    bindMin[1] = bindMax[1] = p.y;
                    bindMin[2] = bindMax[2] = p.z;
                    haveBind = true;
                } else {
                    if (p.x < bindMin[0]) {
                        bindMin[0] = p.x;
                    }
                    if (p.y < bindMin[1]) {
                        bindMin[1] = p.y;
                    }
                    if (p.z < bindMin[2]) {
                        bindMin[2] = p.z;
                    }
                    if (p.x > bindMax[0]) {
                        bindMax[0] = p.x;
                    }
                    if (p.y > bindMax[1]) {
                        bindMax[1] = p.y;
                    }
                    if (p.z > bindMax[2]) {
                        bindMax[2] = p.z;
                    }
                }
            }
        }
    }

    // Root bone index (tag 0, else hierarchy index 0).
    int ridx = 0;
    for (int i = 0; i < numBones; ++i) {
        if (boneTags[static_cast<size_t>(i)] == 0) {
            ridx = i;
            break;
        }
    }

    // --- 5. Per-alpha frames: locals -> world -> skin -> flatten ---
    out.alphas.reserve(static_cast<size_t>(frames));
    out.frames.reserve(static_cast<size_t>(frames));
    // Blended quats per frame per bone (for the morph metric).
    std::vector<std::vector<std::array<float, 4>>> qTrack(
        static_cast<size_t>(frames), std::vector<std::array<float, 4>>(static_cast<size_t>(numBones)));
    for (int fi = 0; fi < frames; ++fi) {
        double alphaD = (frames <= 1) ? 0.0 : static_cast<double>(fi) / (frames - 1);
        float alpha = static_cast<float>(alphaD);
        out.alphas.push_back(alphaD);
        bool isA = (fi == 0);
        bool isB = (fi == frames - 1);
        for (int i = 0; i < numBones; ++i) {
            size_t bi = static_cast<size_t>(i);
            rw::Frame* f = boneFrames[bi];
            if (!f) {
                qTrack[static_cast<size_t>(fi)][bi] = { fullB[bi].q[0], fullB[bi].q[1],
                                                        fullB[bi].q[2], fullB[bi].q[3] };
                continue;
            }
            if (isA) {
                // Endpoint A, Init-exact: mapped -> (IFP quat, IFP-or-BonePos
                // trans), unmapped -> bind matrix restored bit-for-bit.
                if (mapA[bi] >= 0) {
                    float pos[3] = { fullA[bi].t[0], fullA[bi].t[1], fullA[bi].t[2] };
                    rw::Matrix lm;
                    QuatPosToMatrix(fullA[bi].q, pos, lm);
                    f->matrix = lm;
                } else {
                    f->matrix = bindLocals[bi];
                }
                qTrack[static_cast<size_t>(fi)][bi] = { fullA[bi].q[0], fullA[bi].q[1],
                                                        fullA[bi].q[2], fullA[bi].q[3] };
                f->updateObjects();
                continue;
            }
            if (isB) {
                if (mapB[bi] >= 0) {
                    float pos[3] = { fullB[bi].t[0], fullB[bi].t[1], fullB[bi].t[2] };
                    rw::Matrix lm;
                    QuatPosToMatrix(fullB[bi].q, pos, lm);
                    f->matrix = lm;
                } else {
                    f->matrix = bindLocals[bi];
                }
                qTrack[static_cast<size_t>(fi)][bi] = { fullB[bi].q[0], fullB[bi].q[1],
                                                        fullB[bi].q[2], fullB[bi].q[3] };
                f->updateObjects();
                continue;
            }
            // Interior: the R6k operators between the two IFP endpoint poses.
            if (!isMappedEither[bi]) {
                f->matrix = bindLocals[bi];
                qTrack[static_cast<size_t>(fi)][bi] = { fullB[bi].q[0], fullB[bi].q[1],
                                                        fullB[bi].q[2], fullB[bi].q[3] };
                f->updateObjects();
                continue;
            }
            float qi[4];
            SlerpQuat(fullA[bi].q, fullB[bi].q, alpha, qi);
            NormQuat4(qi);
            float ti[3] = {
                fullA[bi].t[0] + alpha * (fullB[bi].t[0] - fullA[bi].t[0]),
                fullA[bi].t[1] + alpha * (fullB[bi].t[1] - fullA[bi].t[1]),
                fullA[bi].t[2] + alpha * (fullB[bi].t[2] - fullA[bi].t[2]),
            };
            rw::Matrix lm;
            QuatPosToMatrix(qi, ti, lm);
            f->matrix = lm;
            f->updateObjects();
            qTrack[static_cast<size_t>(fi)][bi] = { qi[0], qi[1], qi[2], qi[3] };
        }
        std::vector<rw::Matrix> animWorld(static_cast<size_t>(numBones));
        for (int i = 0; i < numBones; ++i) {
            rw::Frame* f = boneFrames[static_cast<size_t>(i)];
            if (!f) {
                animWorld[static_cast<size_t>(i)].setIdentity();
                continue;
            }
            rw::Matrix* ltm = f->getLTM();
            animWorld[static_cast<size_t>(i)] = ltm ? *ltm : f->matrix;
        }
        rw::Matrix atomicCur;
        if (atomicFrame && atomicFrame->getLTM()) {
            atomicCur = *atomicFrame->getLTM();
        } else {
            atomicCur.setIdentity();
        }
        rw::Matrix invAtomicCur;
        rw::Matrix::invert(&invAtomicCur, &atomicCur);
        std::vector<rw::Matrix> skinMats(static_cast<size_t>(numBones));
        {
            rw::Skin* skin0 = rw::Skin::get(geoms[0].geo);
            for (int i = 0; i < numBones; ++i) {
                rw::Matrix ib;
                std::memcpy(&ib, skin0->inverseMatrices + static_cast<size_t>(i) * 16, 64);
                ib.flags = 0;
                rw::Matrix t1, t2;
                rw::Matrix::mult(&t1, &animWorld[static_cast<size_t>(i)], &invAtomicCur);
                rw::Matrix::mult(&t2, &ib, &t1);
                rw::Matrix::mult(&skinMats[static_cast<size_t>(i)], &t2, &atomicCur);
            }
        }
        IfpAnimBlendFrame fr;
        BlendFlatOut fo;
        FlattenWithMats(geoms, skinMats, numBones, lc, fr.scene, srcLabel.c_str(), txdLabel.c_str(),
                        nTextures, fo);
        if (fo.tris <= 0) {
            TexSample_FreeLinked(lc);
            char msg[192];
            (void)std::snprintf(msg, sizeof(msg), "blend frame %d flattened no triangles", fi);
            SetErr(err, errSize, msg);
            out = IfpAnimBlendResult{};
            return false;
        }
        IfpAnimStats& st = fr.stats;
        (void)std::snprintf(st.model, sizeof(st.model), "%s", resolved.c_str());
        (void)std::snprintf(st.requested, sizeof(st.requested), "%s", want.c_str());
        (void)std::snprintf(st.src, sizeof(st.src), "%s", srcLabel.c_str());
        (void)std::snprintf(st.txd, sizeof(st.txd), "%s", txdLabel.c_str());
        (void)std::snprintf(st.bank, sizeof(st.bank), "%s", bankName.c_str());
        (void)std::snprintf(st.bankSrc, sizeof(st.bankSrc), "%s", bankSrc.c_str());
        (void)std::snprintf(st.anim, sizeof(st.anim), "%.31s>%.31s", animA->name, animB->name);
        st.time = alphaD;
        st.timeAbs = alphaD;
        st.animTotal = animA->total;
        st.animsInBank = static_cast<int>(bank.size());
        st.seqs = static_cast<int>(animA->seqs.size() + animB->seqs.size());
        st.bones = numBones;
        st.mapped = mappedEither;
        st.unmapped = numBones - mappedEither;
        st.tried = tried;
        st.textures = nTextures;
        st.tris = fo.tris;
        st.verts = fo.verts;
        st.geoms = static_cast<int>(geoms.size());
        st.frames = nFrames;
        st.wsum = fo.wsum;
        {
            float dx = animWorld[static_cast<size_t>(ridx)].pos.x - bindWorld[static_cast<size_t>(ridx)].pos.x;
            float dy = animWorld[static_cast<size_t>(ridx)].pos.y - bindWorld[static_cast<size_t>(ridx)].pos.y;
            float dz = animWorld[static_cast<size_t>(ridx)].pos.z - bindWorld[static_cast<size_t>(ridx)].pos.z;
            st.rootDelta = std::sqrt(dx * dx + dy * dy + dz * dz);
            st.rootWorld[0] = animWorld[static_cast<size_t>(ridx)].pos.x;
            st.rootWorld[1] = animWorld[static_cast<size_t>(ridx)].pos.y;
            st.rootWorld[2] = animWorld[static_cast<size_t>(ridx)].pos.z;
        }
        st.bindMin[0] = bindMin[0];
        st.bindMin[1] = bindMin[1];
        st.bindMin[2] = bindMin[2];
        st.bindMax[0] = bindMax[0];
        st.bindMax[1] = bindMax[1];
        st.bindMax[2] = bindMax[2];
        st.animMin[0] = fr.scene.bboxMin[0];
        st.animMin[1] = fr.scene.bboxMin[1];
        st.animMin[2] = fr.scene.bboxMin[2];
        st.animMax[0] = fr.scene.bboxMax[0];
        st.animMax[1] = fr.scene.bboxMax[1];
        st.animMax[2] = fr.scene.bboxMax[2];
        st.interp = 1;
        out.frames.push_back(std::move(fr));
    }
    TexSample_FreeLinked(lc);

    // --- 6. Blendaudit bone: prefer doubly-mapped Pelvis (tag 1), else first
    // doubly-mapped, else first either-mapped, else hierarchy index 0. ---
    int audit = -1;
    for (int i = 0; i < numBones; ++i) {
        if (boneTags[static_cast<size_t>(i)] == 1 && mapA[static_cast<size_t>(i)] >= 0 &&
            mapB[static_cast<size_t>(i)] >= 0) {
            audit = i;
            break;
        }
    }
    if (audit < 0) {
        for (int i = 0; i < numBones; ++i) {
            if (mapA[static_cast<size_t>(i)] >= 0 && mapB[static_cast<size_t>(i)] >= 0) {
                audit = i;
                break;
            }
        }
    }
    if (audit < 0) {
        for (int i = 0; i < numBones; ++i) {
            if (mapA[static_cast<size_t>(i)] >= 0 || mapB[static_cast<size_t>(i)] >= 0) {
                audit = i;
                break;
            }
        }
    }
    if (audit < 0) {
        audit = 0;
    }
    {
        size_t bi = static_cast<size_t>(audit);
        const char* cn = BoneTagToName(boneTags[bi]);
        if (cn) {
            (void)std::snprintf(out.boneName, sizeof(out.boneName), "%s", cn);
        } else if (mapA[bi] >= 0) {
            (void)std::snprintf(out.boneName, sizeof(out.boneName), "%s",
                                 animA->seqs[static_cast<size_t>(mapA[bi])].name);
        } else if (mapB[bi] >= 0) {
            (void)std::snprintf(out.boneName, sizeof(out.boneName), "%s",
                                 animB->seqs[static_cast<size_t>(mapB[bi])].name);
        } else {
            (void)std::snprintf(out.boneName, sizeof(out.boneName), "bone%d", audit);
        }
        out.boneTag = boneTags[bi];
        for (int k = 0; k < 4; ++k) {
            out.qA[k] = fullA[bi].q[k];
            out.qB[k] = fullB[bi].q[k];
        }
        for (int k = 0; k < 3; ++k) {
            out.tA[k] = fullA[bi].t[k];
            out.tB[k] = fullB[bi].t[k];
        }
        SlerpQuat(fullA[bi].q, fullB[bi].q, 0.5f, out.qI);
        NormQuat4(out.qI);
        for (int k = 0; k < 3; ++k) {
            out.tI[k] = fullA[bi].t[k] + 0.5f * (fullB[bi].t[k] - fullA[bi].t[k]);
        }
    }

    // --- 7. morphMono over all hierarchy bones: angular distance of the
    // blended quat to qB must not increase frame over frame (1e-6 slack). ---
    int passed = 0;
    for (int i = 0; i < numBones; ++i) {
        size_t bi = static_cast<size_t>(i);
        float qb[4] = { fullB[bi].q[0], fullB[bi].q[1], fullB[bi].q[2], fullB[bi].q[3] };
        bool mono = true;
        float prev = 0.0f;
        for (int fi = 0; fi < frames; ++fi) {
            float qc[4] = { qTrack[static_cast<size_t>(fi)][bi][0],
                            qTrack[static_cast<size_t>(fi)][bi][1],
                            qTrack[static_cast<size_t>(fi)][bi][2],
                            qTrack[static_cast<size_t>(fi)][bi][3] };
            float d = QuatAngle(qc, qb);
            if (fi > 0 && d > prev + 1e-6f) {
                mono = false;
                break;
            }
            prev = d;
        }
        if (mono) {
            ++passed;
        }
    }
    out.morphChecked = numBones;
    out.morphPassed = passed;
    out.morphMono = numBones > 0 ? static_cast<double>(passed) / numBones : 0.0;
    return true;
}

#ifdef REALTIME_GAMEPLAY_POSE_AUDIT
bool RealtimeGameplay_AuditPose(const char* gameDir, const char* anim, double phase,
    WorldShotScene& scene, IfpAnimStats& stats, RealtimeGameplayPoseAudit& audit,
    char* error, std::size_t errorSize) {
    audit = {};
    struct Scope {
        ~Scope() { s_RealtimePoseAudit = nullptr; }
    } scope;
    s_RealtimePoseAudit = &audit;
    return IfpAnim_Init(gameDir,"andre",anim,phase,scene,stats,error,errorSize,true);
}
#endif
