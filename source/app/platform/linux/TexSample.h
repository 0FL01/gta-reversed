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
    long foggedPixels = 0; // geometry pixels written with fog factor > 0
    float uvMin[2] = { 0.0f, 0.0f };
    float uvMax[2] = { 0.0f, 0.0f };
    bool haveUV = false;
    // First-evidence sample: first texture name + first fetched texel/pixel.
    char firstTex[32] = {};
    int firstTexel[4] = { 0, 0, 0, 0 };
    int firstPixel[3] = { 0, 0, 0 };
    bool haveFirst = false;
};

// Time-of-day lighting from timecyc.dat (R6m). amb/sun are 0..1 (Amb/Dir
// bytes / 255); skyTop/skyBot are raw bytes for the background gradient.
// The light direction is NOT part of this struct: it stays at the legacy
// fixed vector (sunDir=fixed), and there is no specular term (spec=off).
// R6s: optional distance fog from the same timecyc row. When fog is false
// (default) the TC path is bit-for-bit the R6m look. When true, every
// geometry pixel blends toward fogColor by
// factor=clamp((dist-FogSt)/(FarClp-FogSt),0,1) with fogColor=SkyBot and
// dist = view-space depth (w_clip = 1/den from the rasterizer's own
// perspective denominator, the same data the z-buffer test uses).
struct TexTimeEnv {
    float amb[3] = { 0.0f, 0.0f, 0.0f };
    float sun[3] = { 0.0f, 0.0f, 0.0f };
    uint8_t skyTop[3] = { 0, 0, 0 };
    uint8_t skyBot[3] = { 0, 0, 0 };
    bool fog = false;
    float farClp = 0.0f; // FarClp from timecyc bytes (tokens[27])
    float fogSt = 0.0f; // FogSt from timecyc bytes (tokens[28])
    uint8_t fogColor[3] = { 0, 0, 0 }; // SkyBot of the same row
};

// Software rasterizer over a WorldShotScene (triangle soup + per-tri UV,
// texture indices and material colors). Orbit = fit-sphere frustum +
// Z-turntable (shot/scene modes); Path = fixed 60deg frustum along
// eye->target (e2e mode). Output is bottom-up RGBA (glReadPixels layout)
// so the existing TGA writer applies unchanged. Deterministic: fixed
// traversal order, float math, no threads.
//
// The Render* variants with a null env keep the legacy look bit-for-bit
// (solid clear-color background, shade = 0.32+0.68*NdotL). RenderOrbitTC
// with a timecyc env paints the vertical SkyTop->SkyBot gradient first and
// shades texel*(ambient+sun*NdotL) per channel instead.
void TexSample_RenderOrbit(const WorldShotScene& scene, int width, int height, float angleDeg,
                           const float* eyeOverrideOrNull, std::vector<uint8_t>& outRGBA,
                           TexFrameStats& stats);
void TexSample_RenderOrbitTC(const WorldShotScene& scene, int width, int height, float angleDeg,
                             const float* eyeOverrideOrNull, const TexTimeEnv& env,
                             std::vector<uint8_t>& outRGBA, TexFrameStats& stats);
void TexSample_RenderPath(const WorldShotScene& scene, int width, int height, const float eye[3],
                          const float target[3], std::vector<uint8_t>& outRGBA,
                          TexFrameStats& stats);

// Duo render (R6t, round 22): ONE shared-depth CPU frame over a merged
// car+ped scene. The first `carMeshes` meshes belong to the car (actor 0),
// the rest to the ped (actor 1). Pixels come from a single z-buffer pass
// with the legacy look (no timecyc env, no turntable, fixed 60deg frustum
// along eye->target, exactly the RenderPath camera). Coverage of EVERY
// valid fragment (inside-test + den>eps, before the depth test, even on
// alpha-cutout) sets that actor's coverage bit; the depth winner sets the
// per-pixel owner. carPixels/pedPixels count depth winners in the final
// image; overlap counts pixels where BOTH actors projected (both coverage
// bits set) regardless of who won the shared z-test. Rendered in fixed
// mesh order (car first, then ped), float math, no threads: deterministic.
// Legacy Render* paths are untouched bit-for-bit.
struct TexDuoStats {
    long carPixels = 0; // depth winners owned by the car meshes
    long pedPixels = 0; // depth winners owned by the ped meshes
    long overlap = 0; // pixels where both actors projected (shared-z proof)
};
void TexSample_RenderDuo(const WorldShotScene& scene, int carMeshes, int width, int height,
                          const float eye[3], const float target[3],
                          std::vector<uint8_t>& outRGBA, TexFrameStats& stats,
                          TexDuoStats& duo);

// Round 42 (R6an): Duo render with a timecyc env (same scene-as---fog path).
// Identical to TexSample_RenderDuo except the background is the vertical
// SkyBot->SkyTop gradient, per-channel light is ambient+sun*NdotL, and every
// geometry pixel blends toward fogColor=SkyBot by
// factor=clamp((dist-FogSt)/(FarClp-FogSt),0,1) with dist = view-space depth
// (w_clip = 1/den, the same denominator the z-buffer test uses) — the exact
// ShadeTri math of the scene --fog path (foggedPixels counts geometry pixels
// written with factor > 0). Coverage/owner/depth rules are untouched.
// TexSample_RenderDuo above keeps the legacy look bit-for-bit (null env).
void TexSample_RenderDuoTC(const WorldShotScene& scene, int carMeshes, int width, int height,
                           const float eye[3], const float target[3], const TexTimeEnv& env,
                           std::vector<uint8_t>& outRGBA, TexFrameStats& stats,
                           TexDuoStats& duo);

// Crowd render (R6u, round 23): ONE shared-depth CPU frame over a merged
// 3-ped scene, generalising RenderDuo from 2 actors to N=3 by composition
// (not stitching). Meshes [0,meshEnd0) belong to actor 0, meshes
// [meshEnd0,meshEnd1) to actor 1, the rest to actor 2. Same legacy look as
// the Duo path (no timecyc env, no turntable, fixed 60deg frustum along
// eye->target, same light/background/wrap/alpha/depth rules, fixed mesh
// order, float math, no threads: deterministic); the ONLY difference is
// per-actor coverage bits for three actors + the depth winner. Coverage of
// EVERY valid fragment (inside-test + den>eps, before the depth test, even
// on alpha-cutout) sets that actor's coverage bit; the depth winner sets
// the per-pixel owner. pix[i] counts depth winners owned by actor i;
// overlap12 counts pixels where >=2 actors projected; overlapAll counts
// pixels where all three projected (shared-z proof). Legacy Render* and
// RenderDuo paths are untouched bit-for-bit.
struct TexCrowdStats {
    long pix[3] = { 0, 0, 0 }; // depth winners per actor
    long overlap12 = 0; // pixels where >=2 actors projected
    long overlapAll = 0; // pixels where all 3 actors projected
};
void TexSample_RenderCrowd(const WorldShotScene& scene, int meshEnd0, int meshEnd1, int width,
                           int height, const float eye[3], const float target[3],
                           std::vector<uint8_t>& outRGBA, TexFrameStats& stats,
                           TexCrowdStats& crowd);
