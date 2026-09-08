// TexSample: TXD texel decoding + UV sampling for the Linux native track.
// Round 5 (R6c): proves TXD bytes reach triangles. This TU owns librw
// texture/raster introspection (NULL-platform parse-only) and the CPU
// rasterizer; no GL here. Texels come only from TXD raster bytes (raw
// A8R8G8B8/X8R8G8B8 + DXT1/DXT3); procedural textures are forbidden.
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "app/platform/linux/WorldShot.h"

namespace rw {
struct Clump;
struct TexDictionary;
struct Texture;
} // namespace rw

// One decoded texture: RGBA8 bytes straight from a TXD raster.
using TexImage = WorldShotImage;

// DFF parse with honest material linkage. Materials resolve against
// `primary` first, then `fallbacks` in order (both must outlive the
// returned clump; typically the TXD caches in SceneShot/StreamPager).
// Unresolvable names keep a dummy texture (raster 0x0) so the flatten
// step can tell "wanted but missing" (grey fallback) apart from
// "untextured by design" (nil texture). The scratch dict owns the
// dummies and must be destroyed together with the clump via FreeLinked.
struct LinkedClump {
    rw::Clump* clump = nullptr;
    rw::TexDictionary* scratch = nullptr; // owns dummy textures
    // Per dummy texture: resolution against the TXD dicts (key = dummy).
    struct Resolved {
        rw::Texture* real = nullptr; // nil when wanted but missing
        uint32_t filter = 0; // DFF material filterAddressing (wrap modes)
        char name[32] = {};
    };
    std::map<const rw::Texture*, Resolved> resolved;
};

LinkedClump TexSample_LinkedParse(const uint8_t* bytes, std::size_t size,
                                  rw::TexDictionary* primary,
                                  rw::TexDictionary* const* fallbacks, std::size_t nFallbacks);
void TexSample_FreeLinked(LinkedClump& lc);

// Decodes a REAL (raster-backed) TXD texture to RGBA8. Returns false for
// nil/empty/unsupported rasters (caller counts grey fallback instead of
// inventing texels).
bool TexSample_Decode(const rw::Texture* tex, TexImage& out);

// Per-frame sampling counters (all derived from real fetches, no estimates).
struct TexFrameStats {
    int tris = 0; // triangles submitted to the rasterizer
    int sampledTri = 0; // with a decoded texture, survived clip with area
    int fallbackTri = 0; // wanted a texture but none decoded (grey)
    int flatTri = 0; // untextured by design (material color)
    long texelFetch = 0; // real texel reads (one per textured pixel)
    long texPixels = 0; // textured pixels written (after alpha test)
    long fallbackPixels = 0;
    long flatPixels = 0;
    float uvMin[2] = { 0.0f, 0.0f };
    float uvMax[2] = { 0.0f, 0.0f };
    bool haveUV = false;
    // First-evidence sample: first texture name + first fetched texel/pixel.
    char firstTex[32] = {};
    int firstTexel[4] = { 0, 0, 0, 0 };
    int firstPixel[3] = { 0, 0, 0 };
    bool haveFirst = false;
};

// Software rasterizer over a WorldShotScene (triangle soup + per-tri UV,
// texture indices and material colors). Orbit = fit-sphere frustum +
// Z-turntable (shot/scene modes); Path = fixed 60deg frustum along
// eye->target (e2e mode). Output is bottom-up RGBA (glReadPixels layout)
// so the existing TGA writer applies unchanged. Deterministic: fixed
// traversal order, float math, no threads.
void TexSample_RenderOrbit(const WorldShotScene& scene, int width, int height, float angleDeg,
                           const float* eyeOverrideOrNull, std::vector<uint8_t>& outRGBA,
                           TexFrameStats& stats);
void TexSample_RenderPath(const WorldShotScene& scene, int width, int height, const float eye[3],
                          const float target[3], std::vector<uint8_t>& outRGBA,
                          TexFrameStats& stats);
