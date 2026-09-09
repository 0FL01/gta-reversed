// CsAnim: cutscene-actor pose from ANPK bytes in anim/cuts.img (R6w, round 25).
// Loads ONE hi-poly CS DFF (+ per-model TXD) from models/cutscene.img
// (CS-only direct lookup, no low-poly substitution), loads ONE ANPK bank
// (`anim/cuts.img:<bank>.ifp`, default smoke1a — the actor bank matching
// cssmokevest) through the same VER2 IMG reader, finds one CS animation by
// name (default csplay — the first full-rig 61-sequence animation of the
// bank), samples every bone sequence at fractional time T (default 0.5)
// with lerp (trans) + slerp (quat) between its two bracketing ANPK keys,
// retargets onto the CS HAnim hierarchy, and skins with the ANIMATED world
// matrices through the same CPU path as IfpAnim/SkinPed.
//
// ANPK spec (as reversed in `game_sa/Animation/AnimManager.cpp`,
// LoadAnimFile_ANPK, read-only reference, NOT linked): outer ANPK section,
// then INFO {numAnims + blockName}, then per animation NAME + DGAN { INFO
// {seqCount} + CPAN { ANIM {ObjName[28] + numFrames + next + prev [+boneTag
// iff section size == 44]} + KFRM KR00/KRT0/KRTS } * seqCount }. Frame data
// is float32: KR00 = Rot(quat x,y,z,w) + DeltaTime (5 floats, 20B); KRT0 =
// Rot + Trans(x,y,z) + DeltaTime (8 floats, 32B); KRTS = Rot + Trans +
// Scale(ignored, retail) + DeltaTime (11 floats, 44B). DeltaTime is ABSOLUTE
// scene time in seconds (cutscene key 0 at 0.0, last root key of
// smoke1a:csplay at 29.0). Retail conjugates every stored quat on load
// (CQuaternion::Conjugated) and de-flips uncompressed sequences
// (RemoveQuaternionFlips); both are pure functions of the file bytes and
// are reproduced here so the pose matches the in-game one.
//
// Retail mapping for skinned clumps is boneTag equality
// (RpAnimBlendClumpFindBone) with the name domain as the human-readable
// proof (ConvertBoneTag2BoneName); the round's "mapping from CS names"
// rule is honoured as: every match originates from a CS sequence (CS bone
// name + CS tag from cuts.img bytes — never from ped.ifp), matched either
// by tag equality or by trimmed case-insensitive name equality against
// the canonical table; every DFF bone without a CS partner keeps its bind
// local matrix (honest unmapped list, never a procedural pose).
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "app/platform/linux/WorldShot.h"

struct CsAnimSeqInfo {
    char name[64]; // stored-case animation name (e.g. "csplay")
    int seqs = 0; // sequence count (61 for full-rig CS anims)
    float total = 0.0f; // max last absolute key time (seconds)
};

struct CsAnimStats {
    char model[64]; // resolved DFF base (e.g. "cssmokevest")
    char requested[64]; // --model value
    char src[160]; // e.g. "cutscene.img:cssmokevest.dff"
    char txd[160]; // e.g. "cutscene.img:cssmokevest.txd"
    char bank[64]; // ANPK block name as stored (e.g. "SMOKE1a")
    char bankSrc[160]; // e.g. "cuts.img:smoke1a.ifp"
    char anim[64]; // resolved animation name as stored (e.g. "csplay")
    double time = 0.5; // requested fractional time T (0..1)
    double timeAbs = 0.0; // T_abs = T * animTotal (seconds, ANPK clock)
    double animTotal = 0.0; // max over sequences of last absolute key time
    int animsInBank = 0; // bank animation count (3 for smoke1a.ifp)
    int banksInCuts = 0; // .ifp banks inside anim/cuts.img (148)
    int seqs = 0; // sequences in the sampled animation (61)
    int bones = 0; // DFF HAnim bone count (61 for cssmokevest)
    int mapped = 0; // DFF bones driven by a CS sequence
    int unmapped = 0; // bones kept at bind (identity path)
    int textures = 0;
    int tris = 0;
    int verts = 0; // tris*3 (8112 for cssmokevest)
    int geoms = 0;
    int frames = 0; // clump frame count
    double wsum = 0.0; // mean weight sum (must stay ~=1.0)
    float rootDelta = 0.0f; // |animRootWorld - bindRootWorld|
    float bindMin[3];
    float bindMax[3]; // AABB of bind-skinned positions, SKIN space
    float animMin[3];
    float animMax[3]; // AABB of anim-skinned positions, WORLD space
    // Worked-example bone for the report (first mapped, prefer Pelvis).
    char boneName[40];
    int boneTag = -9999;
    float boneQ[4]; // sampled quat (x,y,z,w) from ANPK bytes (conjugated)
    float boneT[3]; // sampled trans (ANPK when hasTrans, else BonePos)
    int boneHasTrans = 0;
    int boneK0 = 0; // lower bracketing key index within its sequence
    int boneK1 = 0; // upper bracketing key index
    float boneAlpha = 0.0f; // slerp/lerp fraction between k0 and k1
    int boneFrames = 0; // sequence length
    float rootWorld[3]; // world pos of the tag-0 bone at the sampled time
    // Keyaudit: ONE bone sampled with lerp (trans) + slerp (quat) between
    // its two bracketing ANPK keys. Prefer Root (carries translation).
    // A fractional alpha proves real interpolation of file keys.
    char keyBone[40];
    int keyTag = -9999;
    int keyK0 = -1;
    int keyK1 = -1;
    float keyT0 = 0.0f;
    float keyT1 = 0.0f;
    float keyAlpha = 0.0f;
    float keyTimeAbs = 0.0f;
    float keyQ0[4];
    float keyQ1[4];
    float keyQI[4]; // interpolated quat (slerp, normalised)
    float keyP0[3];
    float keyP1[3];
    float keyPI[3]; // interpolated trans (lerp)
    int keyHasT = 0;
    // Unmapped DFF-bone names (canonical name or "tagNNNN"), for the
    // `unmapped=...` log field. Count == unmapped.
    std::vector<std::string> unmappedNames;
};

// Loads + poses. All SRT come from cuts.img ANPK bytes; bones mapped in
// no CS sequence keep bind.
bool CsAnim_Init(const char* gameDir, const char* model, const char* bank, const char* animName,
                 double timeFrac, WorldShotScene& scene, CsAnimStats& stats, char* err,
                 std::size_t errSize);
// Lists CS animation names (+ seq counts + totals) of one cuts.img bank.
bool CsAnim_List(const char* gameDir, const char* bank, std::vector<CsAnimSeqInfo>& out,
                 char* bankSrcOut, std::size_t bankSrcSize, char* err, std::size_t errSize);
void CsAnim_Shutdown();
