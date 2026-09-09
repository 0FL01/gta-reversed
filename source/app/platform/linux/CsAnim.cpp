// CsAnim implementation (R6w). See CsAnim.h for the contract.
// Own librw engine handle (same NULL-platform parse-only set as
// SkinPed/IfpAnim; one shot path per process, no double init). IMG helpers
// duplicated per native-track precedent (no refactors of verified slices
// in-round).

#include "app/platform/linux/CsAnim.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <array>
#include <string>
#include <vector>
#include <map>

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

// Trim ASCII spaces/tabs/CR/LF/NUL on both ends; CS seq names carry a
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
// CS face tags (5001..5021) and CS finger extras (28..30, 38..40) have no
// canonical entry — those bones match by tag equality only, and are
// reported as "tagNNNN" when unmapped.
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

// Reverse lookup for tag==-1 sequences: trimmed case-insensitive match
// against the canonical table plus the known IFP spelling variants.
int32 BoneNameToTag(const std::string& trimmed) {
    std::string l = trimmed;
    ToLowerInPlace(l);
    if (l == "normal" || l == "root") {
        return 0;
    }
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
    if (l == "l finger" || l == "l fingers") {
        return 35;
    }
    if (l == "r finger" || l == "r fingers") {
        return 25;
    }
    if (l == "l toe0" || l == "l toe") {
        return 44;
    }
    if (l == "r toe0" || l == "r toe") {
        return 54;
    }
    if (l == "spine 1" || l == "spine1" || l == "spine 2") {
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

// --- ANPK bank model (decoded floats, absolute seconds as stored) ---

struct CsKey {
    float q[4]; // x,y,z,w, RETAIL-CONJUGATED on load, de-flipped per seq
    float t[3]; // valid iff hasT
    bool hasT = false;
    float absTime = 0.0f; // absolute scene time, seconds
};

struct CsSeq {
    char name[32] = {}; // trimmed stored-case ("root", " Pelvis"->"Pelvis")
    int32 tag = -1;
    char kind[5] = {}; // "KR00", "KRT0" or "KRTS"
    std::vector<CsKey> keys;
};

struct CsAnimData {
    char name[32] = {}; // as stored ("csplay")
    std::vector<CsSeq> seqs;
    float total = 0.0f; // max last absTime
};

static uint32 ReadU32LE(const uint8* p) {
    uint32 v = 0;
    std::memcpy(&v, p, 4);
    return v;
}
static int32 ReadI32LE(const uint8* p) {
    int32 v = 0;
    std::memcpy(&v, p, 4);
    return v;
}
static float ReadF32LE(const uint8* p) {
    float v = 0.0f;
    std::memcpy(&v, p, 4);
    return v;
}

static std::size_t RoundTo4(std::size_t n) {
    return (n + 3) & ~static_cast<std::size_t>(3);
}

// Parses ANPK exactly along LoadAnimFile_ANPK (section nesting
// ANPK/INFO + NAME + DGAN/INFO + CPAN/ANIM + KFRM, RoundTo4 on every
// payload except the raw KFRM key block). Returns false on any layout
// mismatch; never invents keys.
bool ParseAnpkBank(const std::vector<uint8>& bytes, std::string& blockNameOut,
                   std::vector<CsAnimData>& animsOut, char* err, std::size_t errSize) {
    animsOut.clear();
    blockNameOut.clear();
    if (bytes.size() < 8) {
        SetErr(err, errSize, "ANPK too small for header");
        return false;
    }
    if (std::memcmp(bytes.data(), "ANPK", 4) != 0) {
        SetErr(err, errSize, "ANPK header is not ANPK (not a cutscene bank?)");
        return false;
    }
    std::size_t pos = 8; // skip outer ANPK FourCC + size dword
    // INFO section: {numAnims u32 + blockName[size-4]}.
    if (pos + 8 > bytes.size()) {
        SetErr(err, errSize, "ANPK truncated at INFO header");
        return false;
    }
    if (std::memcmp(bytes.data() + pos, "INFO", 4) != 0) {
        SetErr(err, errSize, "ANPK INFO section missing");
        return false;
    }
    uint32 infoSize = ReadU32LE(bytes.data() + pos + 4);
    pos += 8;
    if (infoSize < 4 || infoSize > 64 || pos + RoundTo4(infoSize) > bytes.size()) {
        SetErr(err, errSize, "ANPK INFO size out of range");
        return false;
    }
    uint32 numAnims = ReadU32LE(bytes.data() + pos);
    {
        char blk[65] = {};
        std::size_t nl = infoSize - 4;
        if (nl > 64) {
            nl = 64;
        }
        std::memcpy(blk, bytes.data() + pos + 4, nl);
        blk[64] = '\0';
        blockNameOut = TrimCopy(blk, nl);
    }
    pos += RoundTo4(infoSize);
    if (numAnims == 0 || numAnims > 10000) {
        SetErr(err, errSize, "ANPK animation count out of range");
        return false;
    }
    for (uint32 a = 0; a < numAnims; ++a) {
        // NAME section.
        if (pos + 8 > bytes.size() || std::memcmp(bytes.data() + pos, "NAME", 4) != 0) {
            SetErr(err, errSize, "ANPK truncated at NAME header");
            return false;
        }
        uint32 nameSize = ReadU32LE(bytes.data() + pos + 4);
        pos += 8;
        if (nameSize == 0 || nameSize > 64 || pos + RoundTo4(nameSize) > bytes.size()) {
            SetErr(err, errSize, "ANPK NAME size out of range");
            return false;
        }
        CsAnimData anim;
        {
            char nm[65] = {};
            std::memcpy(nm, bytes.data() + pos, nameSize);
            nm[64] = '\0';
            std::string t = TrimCopy(nm, nameSize);
            (void)std::snprintf(anim.name, sizeof(anim.name), "%s", t.c_str());
        }
        pos += RoundTo4(nameSize);
        // DGAN wrapper.
        if (pos + 8 > bytes.size() || std::memcmp(bytes.data() + pos, "DGAN", 4) != 0) {
            SetErr(err, errSize, "ANPK DGAN section missing");
            return false;
        }
        uint32 dganSize = ReadU32LE(bytes.data() + pos + 4);
        pos += 8;
        if (pos + dganSize > bytes.size()) {
            SetErr(err, errSize, "ANPK DGAN size out of range");
            return false;
        }
        const std::size_t dganEnd = pos + dganSize;
        // DGAN::AnimInfo (INFO section, payload {seqCount + pad}).
        if (pos + 8 > bytes.size() || std::memcmp(bytes.data() + pos, "INFO", 4) != 0) {
            SetErr(err, errSize, "ANPK anim INFO section missing");
            return false;
        }
        uint32 aiSize = ReadU32LE(bytes.data() + pos + 4);
        pos += 8;
        if (aiSize < 4 || aiSize > 72 || pos + RoundTo4(aiSize) > bytes.size()) {
            SetErr(err, errSize, "ANPK anim INFO size out of range");
            return false;
        }
        uint32 numSeq = ReadU32LE(bytes.data() + pos);
        pos += RoundTo4(aiSize);
        if (numSeq == 0 || numSeq > 256) {
            SetErr(err, errSize, "ANPK sequence count out of range");
            return false;
        }
        anim.seqs.reserve(numSeq);
        float animTotal = 0.0f;
        for (uint32 s = 0; s < numSeq; ++s) {
            // CPAN wrapper: payload is ANIM section + optional KFRM block.
            if (pos + 8 > bytes.size() || std::memcmp(bytes.data() + pos, "CPAN", 4) != 0) {
                SetErr(err, errSize, "ANPK CPAN section missing");
                return false;
            }
            uint32 cpanSize = ReadU32LE(bytes.data() + pos + 4);
            pos += 8;
            if (pos + cpanSize > bytes.size()) {
                SetErr(err, errSize, "ANPK CPAN size out of range");
                return false;
            }
            const std::size_t cpanEnd = pos + cpanSize;
            if (pos + 8 > bytes.size() || std::memcmp(bytes.data() + pos, "ANIM", 4) != 0) {
                SetErr(err, errSize, "ANPK ANIM section missing");
                return false;
            }
            uint32 animSize = ReadU32LE(bytes.data() + pos + 4);
            pos += 8;
            // Retail Anim struct: ObjName[28] + NumFrames + Next + Prev
            // (40B) + BoneTag iff section size == 44.
            if (animSize != 40 && animSize != 44) {
                SetErr(err, errSize, "ANPK ANIM size is not 40/44");
                return false;
            }
            if (pos + RoundTo4(animSize) > bytes.size()) {
                SetErr(err, errSize, "ANPK truncated in ANIM");
                return false;
            }
            CsSeq seq;
            {
                char sn[29] = {};
                std::memcpy(sn, bytes.data() + pos, 28);
                sn[28] = '\0';
                std::string t = TrimCopy(sn, 28);
                (void)std::snprintf(seq.name, sizeof(seq.name), "%s", t.c_str());
            }
            uint32 nframes = ReadU32LE(bytes.data() + pos + 28);
            seq.tag = (animSize == 44) ? ReadI32LE(bytes.data() + pos + 40) : -1;
            pos += RoundTo4(animSize);
            if (nframes > 100000) {
                SetErr(err, errSize, "ANPK frame count out of range");
                return false;
            }
            if (nframes == 0) {
                if (pos != cpanEnd) {
                    SetErr(err, errSize, "ANPK CPAN end mismatch (empty seq)");
                    return false;
                }
                anim.seqs.push_back(std::move(seq));
                continue;
            }
            if (pos + 8 > bytes.size()) {
                SetErr(err, errSize, "ANPK truncated at KFRM header");
                return false;
            }
            char k4[5] = {};
            std::memcpy(k4, bytes.data() + pos, 4);
            uint32 kfSize = ReadU32LE(bytes.data() + pos + 4);
            pos += 8;
            bool hasT = false;
            std::size_t floatsPer = 0;
            if (std::memcmp(k4, "KR00", 4) == 0) {
                floatsPer = 5;
            } else if (std::memcmp(k4, "KRT0", 4) == 0) {
                floatsPer = 8;
                hasT = true;
            } else if (std::memcmp(k4, "KRTS", 4) == 0) {
                floatsPer = 11;
                hasT = true;
            } else {
                SetErr(err, errSize, "ANPK unknown frame type (not KR00/KRT0/KRTS)");
                return false;
            }
            (void)std::snprintf(seq.kind, sizeof(seq.kind), "%s", k4);
            if (kfSize != static_cast<uint32>(nframes * floatsPer * 4) ||
                pos + kfSize > bytes.size()) {
                SetErr(err, errSize, "ANPK KFRM size mismatch");
                return false;
            }
            seq.keys.reserve(nframes);
            const std::size_t dtOff = (floatsPer - 1) * 4;
            for (uint32 k = 0; k < nframes; ++k) {
                const uint8* p = bytes.data() + pos + static_cast<size_t>(k) * floatsPer * 4;
                CsKey key;
                // Retail conjugates every stored quat on load.
                key.q[0] = -ReadF32LE(p + 0);
                key.q[1] = -ReadF32LE(p + 4);
                key.q[2] = -ReadF32LE(p + 8);
                key.q[3] = ReadF32LE(p + 12);
                key.hasT = hasT;
                if (hasT) {
                    key.t[0] = ReadF32LE(p + 16);
                    key.t[1] = ReadF32LE(p + 20);
                    key.t[2] = ReadF32LE(p + 24);
                    // KRTS scale at p+28..40 is read and ignored (retail).
                } else {
                    key.t[0] = key.t[1] = key.t[2] = 0.0f;
                }
                key.absTime = ReadF32LE(p + dtOff);
                if (!(key.absTime >= -1.0f && key.absTime <= 1e6f)) {
                    SetErr(err, errSize, "ANPK key time out of range");
                    return false;
                }
                seq.keys.push_back(key);
            }
            pos += kfSize;
            if (pos != cpanEnd) {
                SetErr(err, errSize, "ANPK CPAN end mismatch");
                return false;
            }
            // Retail RemoveQuaternionFlips for uncompressed data: negate
            // runs that flip hemisphere so neighbours slerp the short way.
            // Same rotation, sign canonicalisation only.
            for (std::size_t k = 1; k < seq.keys.size(); ++k) {
                const float* a = seq.keys[k - 1].q;
                float* b = seq.keys[k].q;
                float dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
                if (dot < 0.0f) {
                    b[0] = -b[0];
                    b[1] = -b[1];
                    b[2] = -b[2];
                    b[3] = -b[3];
                }
            }
            if (!seq.keys.empty()) {
                float last = seq.keys.back().absTime;
                if (last > animTotal) {
                    animTotal = last;
                }
            }
            anim.seqs.push_back(std::move(seq));
        }
        if (pos != dganEnd) {
            SetErr(err, errSize, "ANPK DGAN end mismatch");
            return false;
        }
        anim.total = animTotal;
        animsOut.push_back(std::move(anim));
    }
    return true;
}

// Effective tag for mapping: disk tag, else canonical-name lookup.
int32 EffectiveTag(const CsSeq& seq) {
    if (seq.tag != -1) {
        return seq.tag;
    }
    return BoneNameToTag(seq.name);
}

// Loads raw bank bytes from anim/cuts.img (`<bank>.ifp`, case-insensitive).
// Cutscene banks ship ONLY inside cuts.img (no loose anim/*.ifp for them);
// no bytes are invented.
bool LoadBankBytes(const char* gameDir, const char* bank, std::vector<uint8>& out,
                   std::string& srcLabel, std::string& entryName, int& bankCount, char* err,
                   std::size_t errSize) {
    std::string b = bank && bank[0] ? bank : "smoke1a";
    std::string want = b + ".ifp";
    std::string wantLower = ToLowerCopy(want.c_str());
    ImgIndex idx;
    if (!BuildImgIndex("anim/cuts.img", idx)) {
        SetErr(err, errSize, "anim/cuts.img not indexed (VER2 expected)");
        return false;
    }
    bankCount = 0;
    for (const ImgEntry& e : idx.entries) {
        if (e.nameLower.size() > 4 &&
            e.nameLower.compare(e.nameLower.size() - 4, 4, ".ifp") == 0) {
            ++bankCount;
        }
    }
    for (const ImgEntry& e : idx.entries) {
        if (e.nameLower != wantLower || e.size == 0) {
            continue;
        }
        std::vector<uint8> blob;
        if (!ImgReadBytesStd(idx, wantLower, blob) || blob.empty()) {
            SetErr(err, errSize, "cuts.img bank read failed");
            return false;
        }
        out = std::move(blob);
        srcLabel = std::string("cuts.img:") + e.name;
        entryName = e.name;
        (void)gameDir;
        return true;
    }
    char msg[192];
    (void)std::snprintf(msg, sizeof(msg), "CS bank '%s.ifp' not in anim/cuts.img (%d banks)",
                         b.c_str(), bankCount);
    SetErr(err, errSize, msg);
    return false;
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

std::vector<rw::TexDictionary*> s_txds;

} // namespace

bool CsAnim_List(const char* gameDir, const char* bank, std::vector<CsAnimSeqInfo>& out,
                 char* bankSrcOut, std::size_t bankSrcSize, char* err, std::size_t errSize) {
    out.clear();
    if (!gameDir || !gameDir[0]) {
        SetErr(err, errSize, "no game dir");
        return false;
    }
    std::string game(gameDir);
    OS_SetFilePathOffset(game.c_str());
    s_gameAbs = game;
    std::vector<uint8> bankBytes;
    std::string srcLabel;
    std::string entryName;
    int bankCount = 0;
    char lerr[256] = {};
    if (!LoadBankBytes(gameDir, bank ? bank : "smoke1a", bankBytes, srcLabel, entryName, bankCount,
                       lerr, sizeof(lerr))) {
        SetErr(err, errSize, lerr);
        return false;
    }
    std::string blockName;
    std::vector<CsAnimData> anims;
    if (!ParseAnpkBank(bankBytes, blockName, anims, err, errSize)) {
        return false;
    }
    for (const auto& a : anims) {
        CsAnimSeqInfo info;
        (void)std::snprintf(info.name, sizeof(info.name), "%s", a.name);
        info.seqs = static_cast<int>(a.seqs.size());
        info.total = a.total;
        out.push_back(info);
    }
    if (bankSrcOut && bankSrcSize > 0) {
        (void)std::snprintf(bankSrcOut, bankSrcSize, "%s", srcLabel.c_str());
    }
    return true;
}

bool CsAnim_Init(const char* gameDir, const char* model, const char* bank, const char* animName,
                 double timeFrac, WorldShotScene& scene, CsAnimStats& stats, char* err,
                 std::size_t errSize) {
    stats = CsAnimStats{};
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
    std::string want = model && model[0] ? model : "cssmokevest";
    (void)std::snprintf(stats.requested, sizeof(stats.requested), "%s", want.c_str());
    std::string wantLower = want;
    ToLowerInPlace(wantLower);
    std::string bankWant = bank && bank[0] ? bank : "smoke1a";
    std::string animWant = animName && animName[0] ? animName : "csplay";
    std::string animWantLower = ToLowerCopy(animWant.c_str());
    stats.time = timeFrac;
    std::string game(gameDir);
    OS_SetFilePathOffset(game.c_str());
    s_gameAbs = game;

    ImgIndex cutsIdx;
    if (!BuildImgIndex("anim/cuts.img", cutsIdx)) {
        SetErr(err, errSize, "anim/cuts.img not indexed (VER2 expected)");
        return false;
    }
    ImgIndex csIdx;
    if (!BuildImgIndex("models/cutscene.img", csIdx)) {
        SetErr(err, errSize, "models/cutscene.img not indexed (VER2 expected)");
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

    // --- 1. CS DFF bytes (CS archives only, direct lookup, no fallback) ---
    std::vector<uint8> dffBytes;
    std::string dffFile = wantLower + ".dff";
    const ImgIndex* hitImg = nil;
    std::string storedDff;
    if (ImgReadBytesStd(csIdx, dffFile, dffBytes)) {
        hitImg = &csIdx;
        for (const ImgEntry& e : csIdx.entries) {
            if (e.nameLower == dffFile) {
                storedDff = e.name;
                break;
            }
        }
    }
    if (dffBytes.empty()) {
        char msg[256];
        (void)std::snprintf(msg, sizeof(msg), "CS model '%s' not in models/cutscene.img (CS-only; "
                                              "low-poly stand-ins forbidden)",
                             want.c_str());
        SetErr(err, errSize, msg);
        return false;
    }
    (void)std::snprintf(stats.model, sizeof(stats.model), "%s", wantLower.c_str());
    (void)std::snprintf(stats.src, sizeof(stats.src), "%s:%s", hitImg->label.c_str(),
                         storedDff.c_str());

    // --- 2. Per-model TXD ---
    std::string txdFile = wantLower + ".txd";
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
        SetErr(err, errSize, "no HAnim hierarchy for skinned CS model");
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

    std::vector<int32> boneTags(static_cast<size_t>(numBones), -1);
    std::vector<rw::Frame*> boneFrames(static_cast<size_t>(numBones), nil);
    std::vector<rw::Matrix> bindWorld(static_cast<size_t>(numBones));
    for (int i = 0; i < numBones; ++i) {
        boneTags[static_cast<size_t>(i)] = hh->nodeInfo[i].id;
        boneFrames[static_cast<size_t>(i)] = hh->nodeInfo[i].frame;
        rw::Frame* f = hh->nodeInfo[i].frame;
        if (f) {
            rw::Matrix* ltm = f->getLTM();
            bindWorld[static_cast<size_t>(i)] = ltm ? *ltm : f->matrix;
        } else {
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

    // --- 4. ANPK bank + animation lookup (case-insensitive) ---
    std::vector<uint8> bankBytes;
    std::string bankSrc;
    std::string entryName;
    int bankCount = 0;
    {
        char lerr[256] = {};
        if (!LoadBankBytes(gameDir, bankWant.c_str(), bankBytes, bankSrc, entryName, bankCount,
                           lerr, sizeof(lerr))) {
            TexSample_FreeLinked(lc);
            SetErr(err, errSize, lerr);
            return false;
        }
    }
    std::string blockName;
    std::vector<CsAnimData> bankAnims;
    if (!ParseAnpkBank(bankBytes, blockName, bankAnims, err, errSize)) {
        TexSample_FreeLinked(lc);
        return false;
    }
    (void)std::snprintf(stats.bank, sizeof(stats.bank), "%s",
                         blockName.empty() ? bankWant.c_str() : blockName.c_str());
    (void)std::snprintf(stats.bankSrc, sizeof(stats.bankSrc), "%s", bankSrc.c_str());
    stats.animsInBank = static_cast<int>(bankAnims.size());
    stats.banksInCuts = bankCount;
    const CsAnimData* anim = nil;
    for (const auto& a : bankAnims) {
        if (ToLowerCopy(a.name) == animWantLower) {
            anim = &a;
            break;
        }
    }
    if (!anim) {
        TexSample_FreeLinked(lc);
        char msg[192];
        (void)std::snprintf(msg, sizeof(msg), "CS animation '%s' not in bank '%s' (%d anims)",
                             animWant.c_str(), bankWant.c_str(), static_cast<int>(bankAnims.size()));
        SetErr(err, errSize, msg);
        return false;
    }
    (void)std::snprintf(stats.anim, sizeof(stats.anim), "%s", anim->name);
    stats.seqs = static_cast<int>(anim->seqs.size());
    stats.animTotal = anim->total;
    stats.timeAbs = timeFrac * anim->total;

    // --- 5. Sample every sequence at T_abs with lerp+slerp between its two
    // bracketing ANPK keys (the R6k-proven operators; ANPK key times are
    // absolute seconds, same bracketing semantics as the IFP path) ---
    struct Sampled {
        float q[4];
        float t[3];
        bool hasT = false;
        bool valid = false;
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
        const CsSeq& sq = anim->seqs[s];
        Sampled sm;
        if (sq.keys.empty()) {
            sampled[s] = sm;
            continue;
        }
        if (sq.keys.size() == 1) {
            const CsKey& fr = sq.keys[0];
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
            sm.k0 = sm.k1 = 0;
            sm.t0 = sm.t1 = fr.absTime;
            sm.alpha = 0.0f;
            for (int c = 0; c < 4; ++c) {
                sm.q0[c] = sm.q1[c] = nq[c];
            }
            sm.p0[0] = sm.p1[0] = fr.t[0];
            sm.p0[1] = sm.p1[1] = fr.t[1];
            sm.p0[2] = sm.p1[2] = fr.t[2];
            sm.valid = true;
            sampled[s] = sm;
            continue;
        }
        float tAbs = static_cast<float>(stats.timeAbs);
        std::size_t k0 = 0;
        std::size_t k1 = 0;
        float alpha = 0.0f;
        if (tAbs <= sq.keys.front().absTime) {
            k0 = k1 = 0;
        } else if (tAbs >= sq.keys.back().absTime) {
            k0 = k1 = sq.keys.size() - 1;
        } else {
            for (std::size_t k = 0; k + 1 < sq.keys.size(); ++k) {
                float a = sq.keys[k].absTime;
                float b = sq.keys[k + 1].absTime;
                if (a <= tAbs && tAbs <= b) {
                    k0 = k;
                    k1 = k + 1;
                    alpha = (b > a) ? (tAbs - a) / (b - a) : 0.0f;
                    break;
                }
            }
        }
        const CsKey& f0 = sq.keys[k0];
        const CsKey& f1 = sq.keys[k1];
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
        sm.k0 = static_cast<int>(k0);
        sm.k1 = static_cast<int>(k1);
        sm.t0 = f0.absTime;
        sm.t1 = f1.absTime;
        sm.alpha = (k0 == k1) ? 0.0f : alpha;
        for (int c = 0; c < 4; ++c) {
            sm.q0[c] = n0[c];
            sm.q1[c] = n1[c];
        }
        sm.p0[0] = f0.t[0];
        sm.p0[1] = f0.t[1];
        sm.p0[2] = f0.t[2];
        sm.p1[0] = f1.t[0];
        sm.p1[1] = f1.t[1];
        sm.p1[2] = f1.t[2];
        sm.valid = true;
        sampled[s] = sm;
    }
    // CS tag -> sampled index (first wins) + CS trimmed-lower name index.
    // Both keys come from cuts.img bytes only — never from ped.ifp.
    std::map<int32, std::size_t> tagToSeq;
    std::map<std::string, std::size_t> nameToSeq;
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

    // --- 6. Retarget onto DFF bones: tag equality first, then the CS
    // name domain (DFF canonical name vs CS trimmed seq names) ---
    std::vector<int> boneSeqIdx(static_cast<size_t>(numBones), -1);
    int mapped = 0;
    for (int i = 0; i < numBones; ++i) {
        int32 tag = boneTags[static_cast<size_t>(i)];
        int pick = -1;
        if (tag != -1) {
            auto it = tagToSeq.find(tag);
            if (it != tagToSeq.end()) {
                pick = static_cast<int>(it->second);
            }
        }
        if (pick < 0) {
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
        } else {
            boneSeqIdx[static_cast<size_t>(i)] = -1;
            const char* cn = BoneTagToName(tag);
            char nm[48];
            if (cn) {
                (void)std::snprintf(nm, sizeof(nm), "%s", cn);
            } else {
                (void)std::snprintf(nm, sizeof(nm), "tag%d", static_cast<int>(tag));
            }
            stats.unmappedNames.push_back(nm);
        }
    }
    stats.mapped = mapped;
    stats.unmapped = numBones - mapped;

    // --- 7. Bind skinning (bbox only) + anim locals -> anim world ---
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
    // skin-to-bone matrices. For non-translated CS bones the animated
    // local translation must be BonePos (retail
    // FrameUpdateCallBackSkinned), with ANPK translation only where the
    // sequence carries it.
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
        uint32 stk[128] = {};
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
    // local matrix (ANPK bytes when the sequence carries translation, else
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
            stats.boneK0 = sm.k0;
            stats.boneK1 = sm.k1;
            stats.boneAlpha = sm.alpha;
            stats.boneFrames = static_cast<int>(anim->seqs[static_cast<size_t>(s)].keys.size());
        }
    }

    // Keyaudit: ONE bone with its two bracketing ANPK keys and the
    // lerp+slerp value between them. Prefer the Root sequence (tag 0,
    // carries translation so both lerp and slerp are visible), else the
    // first multi-key sequence. All quats/trans are ANPK bytes (conjugated
    // per retail, normalised for quats); no synthesis.
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
                if (sampled[s].valid && anim->seqs[s].keys.size() > 1) {
                    audit = static_cast<int>(s);
                    break;
                }
            }
        }
        if (audit >= 0) {
            const Sampled& sm = sampled[static_cast<size_t>(audit)];
            const CsSeq& sq = anim->seqs[static_cast<size_t>(audit)];
            (void)std::snprintf(stats.keyBone, sizeof(stats.keyBone), "%s", sq.name);
            stats.keyTag = EffectiveTag(sq);
            stats.keyK0 = sm.k0;
            stats.keyK1 = sm.k1;
            stats.keyT0 = sm.t0;
            stats.keyT1 = sm.t1;
            stats.keyAlpha = sm.alpha;
            stats.keyTimeAbs = static_cast<float>(stats.timeAbs);
            for (int c = 0; c < 4; ++c) {
                stats.keyQ0[c] = sm.q0[c];
                stats.keyQ1[c] = sm.q1[c];
                stats.keyQI[c] = sm.q[c];
            }
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

    // Animated locals: mapped bones get (ANPK quat, ANPK-or-BonePos pos),
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
    // Final render verts = A * S * stored, exactly like librw's
    // skinRenderCB (setWorldMatrix(atomic->getFrame()->getLTM())).
    rw::Matrix atomicCur;
    if (atomicFrame && atomicFrame->getLTM()) {
        atomicCur = *atomicFrame->getLTM();
    } else {
        atomicCur.setIdentity();
    }
    rw::Matrix invAtomicCur;
    rw::Matrix::invert(&invAtomicCur, &atomicCur);

    // Animated skin matrices S_i = IB_i * (Wanim_i * invAcur), composed
    // with the model matrix: MW_i = Acur * S_i (one rigid per bone).
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
            rw::Matrix::mult(&animSkinMats[static_cast<size_t>(i)], &atomicCur, &t2);
        }
    }

    // --- 8. Flatten with ANIMATED matrices into the scene (rendered) ---
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
            rw::V3d n = { 0.0f, 0.0f, 1.0f };
            for (int k = 0; k < 4; ++k) {
                int b = idx[k];
                float w = wgt[k];
                wsum += w;
                if (w == 0.0f || b < 0 || b >= numBones) {
                    continue;
                }
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

void CsAnim_Shutdown() {
    for (rw::TexDictionary* t : s_txds) {
        if (t) {
            t->destroy();
        }
    }
    s_txds.clear();
}
