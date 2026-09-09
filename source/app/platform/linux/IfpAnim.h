// IfpAnim: single-frame ped pose from IFP bytes (R6j, round 12).
// Loads ONE skinned DFF (+ per-model TXD) exactly like SkinPed (bind path),
// loads the ped IFP bank (`anim/ped.ifp` loose file first, then
// `anim/anim.img:ped.ifp` through the same VER2 IMG reader when the loose
// file is absent), finds one animation by name (default IDLE_stance, the
// retail idle loop from animgrp.dat `man` group), samples ONE keyframe per
// bone at fractional time T (default 0.5 = middle, no lerp/slerp between
// frames in this round), retargets the sampled local SRT onto the DFF
// HAnim hierarchy, and skins with the ANIMATED world matrices through the
// same CPU path as SkinPed.
//
// IFP spec (as reversed in `game_sa/Animation/`, read-only reference, NOT
// linked): ANP3 bank (`ANP3` + blockName[24] + numAnims) with per-anim
// (name[24] + numSeq + size + flags) and per-seq (seqName[24] +
// frameType + numFrames + boneTag) followed by keyframes:
//   type 3 = KeyFrameCompressed (Rot 4xi16/4096 + DeltaTime i16/60 = 10B)
//   type 4 = KeyFrameTransCompressed (Rot + DeltaTime + Trans 3xi16/1024)
//   = 16B, layout Rot,Delta,Trans (see AnimSequenceFrames.h: KeyFrameCompressed
//   {FixedQuat<int16,4096>, FixedFloat<int16,60>} + Trans FixedVector<int16,1024>)
// Retail mapping for skinned clumps is boneTag equality
// (RpAnimBlendClumpFindBone); the tag domain doubles as the bone-name
// domain via ConvertBoneTag2BoneName, so the round's "by NAMES" mapping is
// implemented as: effective tag (disk tag, or canonical-name lookup when
// tag == -1) compared through the shared canonical name table; every DFF
// bone without an IFP partner keeps its bind local matrix (honest
// unmappedBones counter, never a procedural pose).
//
// Retail application for skinned non-root bones (FrameUpdateCallBackSkinned):
// local rotation := anim quat (single sample, normalised), local position
// := bind local pos unless the sequence carries translation (only Root and
// a few others do), in which case := IFP translation. Root translation
// comes ONLY from IFP bytes (no manual +90deg, no procedural upright).
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "app/platform/linux/WorldShot.h"

struct IfpAnimStats {
    char model[64]; // resolved DFF base (e.g. "andre")
    char requested[64]; // --model value
    char src[160]; // e.g. "gta3.img:andre.dff"
    char txd[160]; // e.g. "gta3.img:andre.txd"
    char bank[64]; // IFP bank name (always "ped" this round)
    char bankSrc[160]; // e.g. "anim/ped.ifp" or "anim.img:ped.ifp"
    char anim[64]; // resolved animation name as stored (e.g. "IDLE_stance")
    double time = 0.5; // requested fractional time T (0..1)
    double timeAbs = 0.0; // T_abs = T * animTotalTime (seconds, IFP clock)
    double animTotal = 0.0; // max over sequences of last absolute DeltaTime
    int animsInBank = 0; // bank animation count (294 for ped.ifp)
    int seqs = 0; // sequences in the sampled animation (32 for IDLE_stance)
    int bones = 0; // DFF HAnim bone count (32 for andre)
    int mapped = 0; // DFF bones driven by an IFP sequence
    int unmapped = 0; // bones kept at bind (identity path)
    int tried = 0; // DFF fallback probes (0 on direct hit)
    int textures = 0;
    int tris = 0;
    int verts = 0; // tris*3
    int geoms = 0;
    int frames = 0; // clump frame count
    double wsum = 0.0; // mean weight sum (must stay ~=1.0)
    float rootDelta = 0.0f; // |animRootWorld - bindRootWorld|
    float bindMin[3]; // AABB of bind-skinned positions, SKIN space (bit-
    float bindMax[3]; // comparable with round-11 ped-load bbox)
    float animMin[3]; // AABB of anim-skinned positions, WORLD space (atomic
    float animMax[3]; // placement A from DFF bytes applied, as librw's
                      // skinRenderCB does; what the TGA shows, gates check)
    // Worked-example bone for the report (first mapped, prefer Pelvis).
    char boneName[40];
    int boneTag = -9999;
    float boneQ[4]; // sampled quat (x,y,z,w) from IFP bytes
    float boneT[3]; // sampled trans (IFP when hasTrans, else bind local pos)
    int boneHasTrans = 0;
    int boneFrame = 0; // sampled keyframe index within its sequence
    int boneFrames = 0; // sequence length
    float rootWorld[3]; // world pos of the tag-0 bone at the sampled time
    // Keyaudit for the interpolation round (R6k): ONE bone sampled with
    // lerp (trans) + slerp (quat) between its two bracketing IFP keys.
    // k0==k1 && alpha==0 means the sample landed exactly on a key (or the
    // sequence has a single key); a fractional alpha proves real interp.
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
    int interp = 0; // 1 when this sample used lerp+slerp between keys
};

// Loads + poses. When interp is false the sample is the legacy single key
// (first key with absTime >= T_abs, R6j behaviour, etalon-preserving); when
// true, every sequence is sampled with lerp (trans) + slerp (quat) between
// its two bracketing IFP keys (R6k walk-cycle interpolation). keyaudit and
// rootWorld are filled in both modes (alpha==0 without interp).
bool IfpAnim_Init(const char* gameDir, const char* model, const char* animName, double timeFrac,
                  WorldShotScene& scene, IfpAnimStats& stats, char* err, std::size_t errSize,
                  bool interp = false);
// Lists bank animation names (for --list-anims). Names are stored-case.
bool IfpAnim_List(const char* gameDir, std::vector<std::string>& names, char* bankSrcOut,
                  std::size_t bankSrcSize, char* err, std::size_t errSize);
// R6k sequencer (walk-cycle interpolation): K evenly spaced fractional
// times across the clip, inclusive endpoints T_i = i/(K-1) (K<=1 gives 0).
// IfpAnim_SeqTimeFrac is the schedule (pure function, no assets); IfpAnim_Seq
// poses every frame with lerp+slerp (interp=true) and returns one scene per
// frame; IfpAnim_LoopGap is the honest joint-space loop-closure metric:
// mean over mapped sequences of (local trans distance + quat angle in
// radians) between the T=0 and T=1 interpolated poses, from IFP bytes only
// (BonePos cancels for rotation-only seqs, so only IFP-carried translation
// such as Root travel contributes). Root travel itself is NOT averaged: it
// is reported separately as rootTravel (world distance of the tag-0 bone).
double IfpAnim_SeqTimeFrac(int idx, int count);
struct IfpAnimSeqFrame {
    WorldShotScene scene;
    IfpAnimStats stats;
};
bool IfpAnim_Seq(const char* gameDir, const char* model, const char* animName, int frames,
                 std::vector<IfpAnimSeqFrame>& out, char* err, std::size_t errSize);
bool IfpAnim_LoopGap(const char* gameDir, const char* animName, float* gapOut, int* mappedOut,
                     char* err, std::size_t errSize);
void IfpAnim_Shutdown();
