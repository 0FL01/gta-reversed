// Bounded CJ constructor: explicit previews and the source-backed startup outfit.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

// Models: torso, head, hands, legs, shoes (ClothesBuilder.cpp:504).
// Textures: torso, head, legs, feet; hands share torso (ibid:565).
// Empty model keys use ONLY the source's torso/head/hands/legs/feet defaults.
// Each stem is a player.img entry without .txd/.dff. Layer order is explicit.
struct NativePlayerTextureLayer {
    std::string Txd;
    std::string Texture;
    // Optional body variants in the SAME TXD; both required when blending.
    // Blend RGB before overlay, retaining Texture's alpha. Empty = copy only.
    std::string FatTexture{};
    std::string RippedTexture{};
};
struct NativePlayerClothes {
    std::array<std::string, 5> Models;
    std::array<std::vector<NativePlayerTextureLayer>, 4> Textures;
    float FatStat = 0.0f;
    float MuscleStat = 0.0f; // effective CStats muscle, not a hidden global
    bool BlendBody = false; // opt in to Normal/Fat/Ripped geometry; preserves preview API
};

// Static owned-retail dataflow + main.scm 087B/070D evidence, not VM execution
// or scheduler reachability. Fat=200, effective muscle=50. Texture keys 4..17
// and model keys 5..9 are zero; hands use the source fallback. clothes.dat has
// no applicable startup rule (non-cutscene). See ignored retail-clothes-static
// and scm-startup-clothes research logs for independently mapped retail facts.
NativePlayerClothes NativePlayerClothes_Startup();

// RW row-vector affine convention, explicitly WITHOUT RW flags/padding.
// p' = p.x*Right + p.y*Up + p.z*At + Pos. Assembled atomic is identity.
struct NativePlayerMatrix {
    std::array<float, 3> Right, Up, At, Pos;
};
struct NativePlayerBone {
    int Tag = -1;
    int Parent = -1; // index in Bones, never a bone tag
    uint32_t Flags = 0; // HAnim PUSH=2, POP=1
    NativePlayerMatrix InverseBind; // gta3.img:player.dff, NOT part DFFs
    NativePlayerMatrix BindLocal; // inverse(InverseBind) relative to parent
    NativePlayerMatrix FrameLocal; // original base frame, for pose diagnostics
};
struct NativePlayerVertex {
    std::array<float, 3> Position, Normal;
    std::array<float, 2> UV; // authored/blended, including negative wrapped V
    std::array<uint8_t, 4> Bones; // base hierarchy array indices, NOT tags
    std::array<float, 4> Weights;
};
struct NativePlayerTriangle {
    std::array<uint32_t, 3> Vertices;
    uint32_t Material;
};
struct NativePlayerImage {
    std::string Name;
    int Width = 0, Height = 0;
    uint32_t FilterAddressing = 0;
    std::vector<uint8_t> RGBA;
};
struct NativePlayerMaterial {
    std::array<uint8_t, 4> RGBA{255, 255, 255, 255};
    uint32_t Image = 0;
};
struct NativePlayerPart {
    std::string Model;
    uint32_t FirstVertex = 0, VertexCount = 0;
    uint32_t FirstTriangle = 0, TriangleCount = 0;
};
struct NativePlayerAssets {
    std::vector<NativePlayerBone> Bones;
    std::vector<NativePlayerVertex> Vertices;
    std::vector<NativePlayerTriangle> Triangles;
    std::array<NativePlayerMaterial, 5> Materials;
    std::array<NativePlayerImage, 4> Images;
    std::array<NativePlayerPart, 5> Parts;
};

// Startup-only, caller-exclusive librw/OS_File path-offset access (no worker
// parsing concurrently). Uses the caller's OS_SetFilePathOffset game root.
// Reuses a Started engine with the native Skin/HAnim/texture plugins; if Dead,
// starts the usual native NULL parse engine and leaves it available to caller.
// All output is owned; no librw pointers, callbacks, or archive handles escape.
// Five body slots and torso/vest, legs/jeans, feet/sneaker alternatives.
// BlendBody accepts stats 0..1000; otherwise Normal (fat 0..200, muscle 0).
// No general clothes.dat rules, tattoos, rescaling or cutscene support.
// Missing/unsupported assets fail, preserving out. Error is cleared on success.
bool NativePlayerAssets_Load(const NativePlayerClothes& clothes, NativePlayerAssets& out, std::string& error);
