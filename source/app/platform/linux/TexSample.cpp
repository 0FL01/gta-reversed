// TexSample: TXD texel decoding + UV sampling for the Linux native track.
// Round 5 (R6c): proves TXD bytes reach triangles. This TU owns librw
// texture/raster linkage (NULL-platform parse-only), the DXT1/DXT3 + raw
// 32-bit decoders, and the CPU rasterizer; no GL here. Texels come only
// from TXD raster bytes; procedural textures are forbidden.

#include "app/platform/linux/TexSample.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <map>

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

// --- Production: linkage, decode, CPU rasterizer (R6c) ---

LinkedClump TexSample_LinkedParse(const uint8_t* bytes, std::size_t size,
                                  rw::TexDictionary* primary,
                                  rw::TexDictionary* const* fallbacks, std::size_t nFallbacks) {
    LinkedClump lc;
    if (!bytes || size < 12) {
        return lc;
    }
    lc.scratch = rw::TexDictionary::create();
    rw::TexDictionary* prev = rw::TexDictionary::getCurrent();
    rw::TexDictionary::setCurrent(lc.scratch);
    rw::Texture::setLoadTextures(false);
    rw::Texture::setCreateDummies(true);
    {
        rw::StreamMemory stream;
        stream.open(const_cast<uint8*>(bytes), static_cast<uint32>(size));
        if (rw::findChunk(&stream, rw::ID_CLUMP, nil, nil)) {
            lc.clump = rw::Clump::streamRead(&stream);
        }
        stream.close();
    }
    rw::Texture::setCreateDummies(false);
    rw::TexDictionary::setCurrent(prev);
    if (!lc.clump) {
        return lc;
    }
    // Resolve every dummy (wanted name) against primary, then fallbacks.
    FORLIST(link, lc.clump->atomics) {
        rw::Atomic* atomic = rw::Atomic::fromClump(link);
        rw::Geometry* geo = atomic ? atomic->geometry : nil;
        if (!geo) {
            continue;
        }
        for (int m = 0; m < geo->matList.numMaterials; ++m) {
            rw::Material* mat = geo->matList.materials[m];
            if (!mat || !mat->texture) {
                continue; // untextured by design (no TEXTURE chunk)
            }
            rw::Texture* dum = mat->texture;
            if (lc.resolved.find(dum) != lc.resolved.end()) {
                continue;
            }
            LinkedClump::Resolved r;
            r.filter = dum->filterAddressing; // DFF sampler state survives below
            std::memcpy(r.name, dum->name, sizeof(r.name));
            r.name[sizeof(r.name) - 1] = '\0';
            rw::Texture* found = primary ? primary->find(dum->name) : nil;
            for (std::size_t f = 0; found == nil && f < nFallbacks; ++f) {
                if (fallbacks[f]) {
                    found = fallbacks[f]->find(dum->name);
                }
            }
            if (found && found->raster && found->raster->width > 0) {
                r.real = found;
            } else {
                r.real = nil; // wanted but missing: grey fallback, never invented
            }
            lc.resolved[dum] = r;
        }
    }
    return lc;
}

void TexSample_FreeLinked(LinkedClump& lc) {
    if (lc.clump) {
        lc.clump->destroy();
        lc.clump = nil;
    }
    lc.resolved.clear();
    if (lc.scratch) {
        lc.scratch->destroy();
        lc.scratch = nil;
    }
}

namespace {

// --- DXT decoders (spec: S3TC block layout, little-endian) ---

inline void Unpack565(uint16_t c, uint8_t& r, uint8_t& g, uint8_t& b) {
    r = static_cast<uint8_t>(((c >> 11) & 31) * 255 / 31);
    g = static_cast<uint8_t>(((c >> 5) & 63) * 255 / 63);
    b = static_cast<uint8_t>((c & 31) * 255 / 31);
}

inline uint16_t ReadU16LE(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

inline uint32_t ReadU32LE(const uint8_t* p) {
    return static_cast<uint32_t>(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24));
}

// Decodes one DXT1 color block at `blk` into 4x4 RGBA `tile` (64 bytes).
// Returns the "transparent" flag (c0 <= c1 three-color mode).
bool DecodeDxt1Block(const uint8_t* blk, uint8_t* tile) {
    uint16_t c0 = ReadU16LE(blk);
    uint16_t c1 = ReadU16LE(blk + 2);
    uint8_t r0, g0, b0, r1, g1, b1;
    Unpack565(c0, r0, g0, b0);
    Unpack565(c1, r1, g1, b1);
    uint8_t col[4][4];
    col[0][0] = r0;
    col[0][1] = g0;
    col[0][2] = b0;
    col[0][3] = 255;
    col[1][0] = r1;
    col[1][1] = g1;
    col[1][2] = b1;
    col[1][3] = 255;
    bool transparent = false;
    if (c0 > c1) {
        for (int k = 0; k < 3; ++k) {
            col[2][k] = static_cast<uint8_t>((2 * col[0][k] + col[1][k]) / 3);
            col[3][k] = static_cast<uint8_t>((col[0][k] + 2 * col[1][k]) / 3);
        }
        col[2][3] = col[3][3] = 255;
    } else {
        for (int k = 0; k < 3; ++k) {
            col[2][k] = static_cast<uint8_t>((col[0][k] + col[1][k]) / 2);
        }
        col[2][3] = 255;
        col[3][0] = col[3][1] = col[3][2] = 0;
        col[3][3] = 0;
        transparent = true;
    }
    uint32_t idx = ReadU32LE(blk + 4);
    for (int py = 0; py < 4; ++py) {
        for (int px = 0; px < 4; ++px) {
            int code = (idx >> (2 * (4 * py + px))) & 3;
            for (int k = 0; k < 4; ++k) {
                tile[(py * 4 + px) * 4 + k] = col[code][k];
            }
        }
    }
    return transparent;
}

void BlitTile(uint8_t* dst, int dstW, int dstH, int bx, int by, const uint8_t* tile) {
    for (int py = 0; py < 4; ++py) {
        for (int px = 0; px < 4; ++px) {
            int x = bx * 4 + px;
            int y = by * 4 + py;
            if (x < dstW && y < dstH) {
                for (int k = 0; k < 4; ++k) {
                    dst[(y * dstW + x) * 4 + k] = tile[(py * 4 + px) * 4 + k];
                }
            }
        }
    }
}

bool DecodeDxt1(const uint8_t* data, int w, int h, std::vector<uint8_t>& rgba) {
    rgba.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
    int blocksX = (w + 3) / 4;
    int blocksY = (h + 3) / 4;
    uint8_t tile[64];
    for (int by = 0; by < blocksY; ++by) {
        for (int bx = 0; bx < blocksX; ++bx) {
            DecodeDxt1Block(data + static_cast<size_t>(by * blocksX + bx) * 8, tile);
            BlitTile(rgba.data(), w, h, bx, by, tile);
        }
    }
    return true;
}

bool DecodeDxt3(const uint8_t* data, int w, int h, std::vector<uint8_t>& rgba) {
    rgba.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
    int blocksX = (w + 3) / 4;
    int blocksY = (h + 3) / 4;
    uint8_t tile[64];
    for (int by = 0; by < blocksY; ++by) {
        for (int bx = 0; bx < blocksX; ++bx) {
            const uint8_t* blk = data + static_cast<size_t>(by * blocksX + bx) * 16;
            uint8_t alpha[16];
            for (int i = 0; i < 8; ++i) {
                alpha[2 * i] = static_cast<uint8_t>((blk[i] & 0xF) * 17);
                alpha[2 * i + 1] = static_cast<uint8_t>((blk[i] >> 4) * 17);
            }
            DecodeDxt1Block(blk + 8, tile);
            for (int i = 0; i < 16; ++i) {
                tile[i * 4 + 3] = alpha[i];
            }
            BlitTile(rgba.data(), w, h, bx, by, tile);
        }
    }
    return true;
}

} // namespace

bool TexSample_Decode(const rw::Texture* tex, TexImage& out) {
    out = TexImage{};
    if (!tex || !tex->raster) {
        return false;
    }
    rw::Raster* ras = tex->raster;
    if (ras->platform != rw::PLATFORM_D3D8 && ras->platform != rw::PLATFORM_D3D9) {
        return false; // only SA on-disk native rasters; nothing else invented
    }
    rw::d3d::D3dRaster* nat = GETD3DRASTEREXT(ras);
    if (!nat || !nat->texture) {
        return false;
    }
    uint8_t* locked = ras->lock(0, rw::Raster::LOCKREAD);
    if (!locked) {
        return false;
    }
    int w = ras->width;
    int h = ras->height;
    int stride = ras->stride;
    bool ok = false;
    if (w > 0 && h > 0 && w <= 4096 && h <= 4096) {
        if (nat->customFormat) {
            uint32_t fmt = nat->format;
            if (fmt == rw::d3d::D3DFMT_DXT1) {
                ok = DecodeDxt1(locked, w, h, out.rgba);
            } else if (fmt == rw::d3d::D3DFMT_DXT3) {
                ok = DecodeDxt3(locked, w, h, out.rgba);
            } else {
                ok = false; // DXT2/4/5 or unknown: refuse, count fallback
            }
        } else {
            uint32_t fmt = nat->format;
            // Raw 32-bit LE texels: bytes are B,G,R,A (A8R8G8B8) or B,G,R,X.
            if ((fmt == rw::d3d::D3DFMT_A8R8G8B8 || fmt == rw::d3d::D3DFMT_X8R8G8B8) &&
                stride >= w * 4) {
                out.rgba.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
                for (int y = 0; y < h; ++y) {
                    const uint8_t* src = locked + static_cast<size_t>(y) * stride;
                    uint8_t* dst = out.rgba.data() + static_cast<size_t>(y) * w * 4;
                    for (int x = 0; x < w; ++x) {
                        dst[x * 4 + 0] = src[x * 4 + 2];
                        dst[x * 4 + 1] = src[x * 4 + 1];
                        dst[x * 4 + 2] = src[x * 4 + 0];
                        dst[x * 4 + 3] =
                            (fmt == rw::d3d::D3DFMT_A8R8G8B8) ? src[x * 4 + 3] : 255;
                    }
                }
                ok = true;
            } else {
                ok = false; // 16-bit/paletted raw: not in the SA corpus as
                            // uncompressed (survey: only 32-bit raw); refuse
            }
        }
    }
    ras->unlock(0);
    if (!ok) {
        out = TexImage{};
        return false;
    }
    (void)std::snprintf(out.name, sizeof(out.name), "%s", tex->name);
    out.w = w;
    out.h = h;
    return true;
}

namespace {

// --- CPU rasterizer (R6c): own clip/project/scan, perspective-correct UV ---

struct ClipVert {
    float x, y, z, w; // clip space
    float u, v, s; // texcoords + lambert shade (carried linearly, like GL)
};

struct RasterTri {
    // Screen-space (pixels, y-up) + 1/w-weighted attributes.
    float sx[3], sy[3];
    float o[3]; // 1/w
    float uo[3], vo[3], so[3], zo[3]; // u/w, v/w, shade/w, z/w
};

inline void Normalize3f(float* v) {
    float len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (len > 1e-9f) {
        v[0] /= len;
        v[1] /= len;
        v[2] /= len;
    }
}

// Row-major 4x4: out = m * p.
inline void XformPoint(const float* m, const float* p, float* out) {
    out[0] = m[0] * p[0] + m[1] * p[1] + m[2] * p[2] + m[3];
    out[1] = m[4] * p[0] + m[5] * p[1] + m[6] * p[2] + m[7];
    out[2] = m[8] * p[0] + m[9] * p[1] + m[10] * p[2] + m[11];
    out[3] = m[12] * p[0] + m[13] * p[1] + m[14] * p[2] + m[15];
}

inline void Mul44(const float* a, const float* b, float* out) {
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            out[r * 4 + c] =
                a[r * 4 + 0] * b[c] + a[r * 4 + 1] * b[4 + c] + a[r * 4 + 2] * b[8 + c] +
                a[r * 4 + 3] * b[12 + c];
        }
    }
}

// Column-major-lookAt port (same math as MainLinux::LookAtMatrix), row-major.
void BuildView(const float* eye, const float* center, float* m) {
    float f[3] = { center[0] - eye[0], center[1] - eye[1], center[2] - eye[2] };
    Normalize3f(f);
    float up[3] = { 0.0f, 0.0f, 1.0f };
    float s[3] = {
        f[1] * up[2] - f[2] * up[1],
        f[2] * up[0] - f[0] * up[2],
        f[0] * up[1] - f[1] * up[0],
    };
    Normalize3f(s);
    float u[3] = {
        s[1] * f[2] - s[2] * f[1],
        s[2] * f[0] - s[0] * f[2],
        s[0] * f[1] - s[1] * f[0],
    };
    m[0] = s[0];
    m[1] = s[1];
    m[2] = s[2];
    m[3] = -(s[0] * eye[0] + s[1] * eye[1] + s[2] * eye[2]);
    m[4] = u[0];
    m[5] = u[1];
    m[6] = u[2];
    m[7] = -(u[0] * eye[0] + u[1] * eye[1] + u[2] * eye[2]);
    m[8] = -f[0];
    m[9] = -f[1];
    m[10] = -f[2];
    m[11] = (f[0] * eye[0] + f[1] * eye[1] + f[2] * eye[2]);
    m[12] = 0.0f;
    m[13] = 0.0f;
    m[14] = 0.0f;
    m[15] = 1.0f;
}

// glFrustum(l,r,b,t,n,f) port, row-major.
void BuildFrustum(float l, float r, float b, float t, float n, float f, float* m) {
    m[0] = 2.0f * n / (r - l);
    m[1] = 0.0f;
    m[2] = (r + l) / (r - l);
    m[3] = 0.0f;
    m[4] = 0.0f;
    m[5] = 2.0f * n / (t - b);
    m[6] = (t + b) / (t - b);
    m[7] = 0.0f;
    m[8] = 0.0f;
    m[9] = 0.0f;
    m[10] = -(f + n) / (f - n);
    m[11] = -2.0f * f * n / (f - n);
    m[12] = 0.0f;
    m[13] = 0.0f;
    m[14] = -1.0f;
    m[15] = 0.0f;
}

inline float PlaneDist(int plane, const ClipVert& v) {
    switch (plane) {
        case 0: return v.x + v.w; // left
        case 1: return v.w - v.x; // right
        case 2: return v.y + v.w; // bottom
        case 3: return v.w - v.y; // top
        case 4: return v.z + v.w; // near
        default: return v.w - v.z; // far
    }
}

ClipVert LerpVert(const ClipVert& a, const ClipVert& b, float t) {
    ClipVert o;
    o.x = a.x + (b.x - a.x) * t;
    o.y = a.y + (b.y - a.y) * t;
    o.z = a.z + (b.z - a.z) * t;
    o.w = a.w + (b.w - a.w) * t;
    o.u = a.u + (b.u - a.u) * t;
    o.v = a.v + (b.v - a.v) * t;
    o.s = a.s + (b.s - a.s) * t;
    return o;
}

// Sutherland-Hodgman against the 6 clip half-spaces. Returns vertex count.
int ClipTriangle(const ClipVert in[3], ClipVert out[16]) {
    ClipVert bufA[16], bufB[16];
    bufA[0] = in[0];
    bufA[1] = in[1];
    bufA[2] = in[2];
    int n = 3;
    for (int plane = 0; plane < 6; ++plane) {
        if (n == 0) {
            return 0;
        }
        int m = 0;
        for (int i = 0; i < n; ++i) {
            const ClipVert& cur = bufA[i];
            const ClipVert& nxt = bufA[(i + 1) % n];
            float dc = PlaneDist(plane, cur);
            float dn = PlaneDist(plane, nxt);
            bool inC = dc >= 0.0f;
            bool inN = dn >= 0.0f;
            if (inC) {
                bufB[m++] = cur;
            }
            if (inC != inN) {
                float t = dc / (dc - dn);
                if (t < 0.0f) {
                    t = 0.0f;
                } else if (t > 1.0f) {
                    t = 1.0f;
                }
                bufB[m++] = LerpVert(cur, nxt, t);
            }
        }
        n = m;
        for (int i = 0; i < n; ++i) {
            bufA[i] = bufB[i];
        }
    }
    for (int i = 0; i < n; ++i) {
        out[i] = bufA[i];
    }
    return n;
}

inline int WrapAxis(float t, int size, uint32_t mode, bool& /*borderHit*/) {
    if (size <= 0) {
        return 0;
    }
    if (mode == 1 || mode == 0) { // WRAP (0 = RW default, treat as wrap)
        float f = t - std::floor(t);
        int i = static_cast<int>(f * size);
        if (i < 0) {
            i = 0;
        } else if (i >= size) {
            i = size - 1;
        }
        return i;
    }
    if (mode == 2) { // MIRROR
        float f = t * 0.5f;
        f = f - std::floor(f); // [0,1) over a 2-texel period
        float g = f * 2.0f; // [0,2)
        if (g >= 1.0f) {
            g = 2.0f - g;
        }
        int i = static_cast<int>(g * size);
        if (i < 0) {
            i = 0;
        } else if (i >= size) {
            i = size - 1;
        }
        return i;
    }
    // CLAMP (3) and BORDER (4 -> clamped edge; counted by caller if needed).
    float c = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    int i = static_cast<int>(c * size);
    if (i < 0) {
        i = 0;
    } else if (i >= size) {
        i = size - 1;
    }
    return i;
}

struct RasterCtx {
    int w = 0;
    int h = 0;
    uint8_t* px = nullptr; // bottom-up RGBA
    float* zbuf = nullptr;
    const WorldShotScene* scene = nullptr;
    TexFrameStats* st = nullptr;
    const TexTimeEnv* env = nullptr; // null = legacy look (bit-for-bit)
};

void ShadeTri(const RasterTri& t, int imgIdx, const float* matCol, RasterCtx& ctx) {
    const WorldShotImage* img = imgIdx >= 0 ? &ctx.scene->images[imgIdx] : nullptr;
    uint32_t filter = img ? img->filter : 0;
    uint32_t uMode = (filter >> 8) & 0xF;
    uint32_t vMode = (filter >> 12) & 0xF;
    // Screen bbox, clamped.
    float minX = t.sx[0], maxX = t.sx[0], minY = t.sy[0], maxY = t.sy[0];
    for (int k = 1; k < 3; ++k) {
        if (t.sx[k] < minX) {
            minX = t.sx[k];
        }
        if (t.sx[k] > maxX) {
            maxX = t.sx[k];
        }
        if (t.sy[k] < minY) {
            minY = t.sy[k];
        }
        if (t.sy[k] > maxY) {
            maxY = t.sy[k];
        }
    }
    int x0 = static_cast<int>(std::floor(minX));
    int x1 = static_cast<int>(std::ceil(maxX));
    int y0 = static_cast<int>(std::floor(minY));
    int y1 = static_cast<int>(std::ceil(maxY));
    if (x0 < 0) {
        x0 = 0;
    }
    if (y0 < 0) {
        y0 = 0;
    }
    if (x1 > ctx.w) {
        x1 = ctx.w;
    }
    if (y1 > ctx.h) {
        y1 = ctx.h;
    }
    if (x0 >= x1 || y0 >= y1) {
        return;
    }
    float area =
        (t.sx[1] - t.sx[0]) * (t.sy[2] - t.sy[0]) - (t.sx[2] - t.sx[0]) * (t.sy[1] - t.sy[0]);
    if (area == 0.0f) {
        return;
    }
    bool gotPixel = false;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            float px = static_cast<float>(x) + 0.5f;
            float py = static_cast<float>(y) + 0.5f;
            float e0 = (t.sx[1] - t.sx[0]) * (py - t.sy[0]) - (t.sy[1] - t.sy[0]) * (px - t.sx[0]);
            float e1 = (t.sx[2] - t.sx[1]) * (py - t.sy[1]) - (t.sy[2] - t.sy[1]) * (px - t.sx[1]);
            float e2 = (t.sx[0] - t.sx[2]) * (py - t.sy[2]) - (t.sy[0] - t.sy[2]) * (px - t.sx[2]);
            // Inside test consistent for both windings (no culling, like GL).
            if (!((e0 >= 0.0f && e1 >= 0.0f && e2 >= 0.0f) ||
                  (e0 <= 0.0f && e1 <= 0.0f && e2 <= 0.0f))) {
                continue;
            }
            float l0 = e1 / area;
            float l1 = e2 / area;
            float l2 = e0 / area;
            float den = l0 * t.o[0] + l1 * t.o[1] + l2 * t.o[2];
            if (den <= 1e-12f) {
                continue;
            }
            float z = l0 * t.zo[0] + l1 * t.zo[1] + l2 * t.zo[2]; // == ndc z:
            // zo stores z/w per vertex; screen-space lerp of z/w IS the
            // fragment's ndc depth (dividing by den again would give z_clip).
            // R6s fog: view-space depth is w_clip = 1/den (the same
            // perspective denominator the z-buffer test uses; the frustum
            // gives w_clip = -z_view). factor=clamp((dist-FogSt)/
            // (FarClp-FogSt),0,1), blended toward fogColor=SkyBot.
            const bool doFog =
                ctx.env && ctx.env->fog && (ctx.env->farClp > ctx.env->fogSt);
            float fogF = 0.0f;
            if (doFog) {
                const float dist = 1.0f / den;
                float f = (dist - ctx.env->fogSt) / (ctx.env->farClp - ctx.env->fogSt);
                if (f < 0.0f) {
                    f = 0.0f;
                } else if (f > 1.0f) {
                    f = 1.0f;
                }
                fogF = f;
            }
            float shade = (l0 * t.so[0] + l1 * t.so[1] + l2 * t.so[2]) / den;
            // Legacy: `shade` is the interpolated 0.32+0.68*NdotL grey
            // factor. Timecyc: the `s` slot carries interpolated NdotL and
            // the per-channel light is ambient+sun*NdotL from timecyc.dat.
            float li[3] = { shade, shade, shade };
            if (ctx.env) {
                li[0] = ctx.env->amb[0] + ctx.env->sun[0] * shade;
                li[1] = ctx.env->amb[1] + ctx.env->sun[1] * shade;
                li[2] = ctx.env->amb[2] + ctx.env->sun[2] * shade;
            }
            float r, g, b;
            if (img) {
                float u = (l0 * t.uo[0] + l1 * t.uo[1] + l2 * t.uo[2]) / den;
                float v = (l0 * t.vo[0] + l1 * t.vo[1] + l2 * t.vo[2]) / den;
                if (!ctx.st->haveUV) {
                    ctx.st->uvMin[0] = ctx.st->uvMax[0] = u;
                    ctx.st->uvMin[1] = ctx.st->uvMax[1] = v;
                    ctx.st->haveUV = true;
                } else {
                    if (u < ctx.st->uvMin[0]) {
                        ctx.st->uvMin[0] = u;
                    }
                    if (u > ctx.st->uvMax[0]) {
                        ctx.st->uvMax[0] = u;
                    }
                    if (v < ctx.st->uvMin[1]) {
                        ctx.st->uvMin[1] = v;
                    }
                    if (v > ctx.st->uvMax[1]) {
                        ctx.st->uvMax[1] = v;
                    }
                }
                bool bh = false;
                int ix = WrapAxis(u, img->w, uMode, bh);
                int iy = WrapAxis(v, img->h, vMode, bh);
                const uint8_t* tx = img->rgba.data() + (static_cast<size_t>(iy) * img->w + ix) * 4;
                ++ctx.st->texelFetch;
                if (!ctx.st->haveFirst) {
                    (void)std::snprintf(ctx.st->firstTex, sizeof(ctx.st->firstTex), "%s",
                                        img->name);
                    ctx.st->firstTexel[0] = tx[0];
                    ctx.st->firstTexel[1] = tx[1];
                    ctx.st->firstTexel[2] = tx[2];
                    ctx.st->firstTexel[3] = tx[3];
                    ctx.st->haveFirst = true; // pixel filled below on write
                }
                if (tx[3] < 128) {
                    continue; // cutout alpha test: no depth write, like discard
                }
                float zslot = ctx.zbuf[static_cast<size_t>(y) * ctx.w + x];
                if (!(z < zslot)) {
                    continue;
                }
                r = li[0] * matCol[0] * (tx[0] / 255.0f);
                g = li[1] * matCol[1] * (tx[1] / 255.0f);
                b = li[2] * matCol[2] * (tx[2] / 255.0f);
                if (doFog) {
                    const float fr = ctx.env->fogColor[0] / 255.0f;
                    const float fg = ctx.env->fogColor[1] / 255.0f;
                    const float fb = ctx.env->fogColor[2] / 255.0f;
                    r = r + (fr - r) * fogF;
                    g = g + (fg - g) * fogF;
                    b = b + (fb - b) * fogF;
                    if (fogF > 0.0f) {
                        ++ctx.st->foggedPixels;
                    }
                }
                ctx.zbuf[static_cast<size_t>(y) * ctx.w + x] = z;
                ++ctx.st->texPixels;
                if (ctx.st->haveFirst && ctx.st->firstPixel[0] == 0 &&
                    ctx.st->firstPixel[1] == 0 && ctx.st->firstPixel[2] == 0 &&
                    (r + g + b) > 0.0f) {
                    // first written textured pixel (guard against black texel)
                }
                uint8_t* dst = ctx.px + (static_cast<size_t>(y) * ctx.w + x) * 4;
                int ri = static_cast<int>(r * 255.0f + 0.5f);
                int gi = static_cast<int>(g * 255.0f + 0.5f);
                int bi = static_cast<int>(b * 255.0f + 0.5f);
                dst[0] = static_cast<uint8_t>(ri < 0 ? 0 : (ri > 255 ? 255 : ri));
                dst[1] = static_cast<uint8_t>(gi < 0 ? 0 : (gi > 255 ? 255 : gi));
                dst[2] = static_cast<uint8_t>(bi < 0 ? 0 : (bi > 255 ? 255 : bi));
                dst[3] = 255;
                if (ctx.st->haveFirst && ctx.st->firstPixel[0] == 0 &&
                    ctx.st->firstPixel[1] == 0 && ctx.st->firstPixel[2] == 0) {
                    ctx.st->firstPixel[0] = dst[0];
                    ctx.st->firstPixel[1] = dst[1];
                    ctx.st->firstPixel[2] = dst[2];
                }
                gotPixel = true;
            } else if (imgIdx == -2) {
                float zslot = ctx.zbuf[static_cast<size_t>(y) * ctx.w + x];
                if (!(z < zslot)) {
                    continue;
                }
                r = li[0] * 0.5f;
                g = li[1] * 0.5f;
                b = li[2] * 0.5f;
                if (doFog) {
                    const float fr = ctx.env->fogColor[0] / 255.0f;
                    const float fg = ctx.env->fogColor[1] / 255.0f;
                    const float fb = ctx.env->fogColor[2] / 255.0f;
                    r = r + (fr - r) * fogF;
                    g = g + (fg - g) * fogF;
                    b = b + (fb - b) * fogF;
                    if (fogF > 0.0f) {
                        ++ctx.st->foggedPixels;
                    }
                }
                ctx.zbuf[static_cast<size_t>(y) * ctx.w + x] = z;
                ++ctx.st->fallbackPixels;
                uint8_t* dst = ctx.px + (static_cast<size_t>(y) * ctx.w + x) * 4;
                int ri = static_cast<int>(r * 255.0f + 0.5f);
                int gi = static_cast<int>(g * 255.0f + 0.5f);
                int bi = static_cast<int>(b * 255.0f + 0.5f);
                dst[0] = static_cast<uint8_t>(ri < 0 ? 0 : (ri > 255 ? 255 : ri));
                dst[1] = static_cast<uint8_t>(gi < 0 ? 0 : (gi > 255 ? 255 : gi));
                dst[2] = static_cast<uint8_t>(bi < 0 ? 0 : (bi > 255 ? 255 : bi));
                dst[3] = 255;
                gotPixel = true;
            } else {
                float zslot = ctx.zbuf[static_cast<size_t>(y) * ctx.w + x];
                if (!(z < zslot)) {
                    continue;
                }
                r = li[0] * matCol[0];
                g = li[1] * matCol[1];
                b = li[2] * matCol[2];
                if (doFog) {
                    const float fr = ctx.env->fogColor[0] / 255.0f;
                    const float fg = ctx.env->fogColor[1] / 255.0f;
                    const float fb = ctx.env->fogColor[2] / 255.0f;
                    r = r + (fr - r) * fogF;
                    g = g + (fg - g) * fogF;
                    b = b + (fb - b) * fogF;
                    if (fogF > 0.0f) {
                        ++ctx.st->foggedPixels;
                    }
                }
                ctx.zbuf[static_cast<size_t>(y) * ctx.w + x] = z;
                ++ctx.st->flatPixels;
                uint8_t* dst = ctx.px + (static_cast<size_t>(y) * ctx.w + x) * 4;
                int ri = static_cast<int>(r * 255.0f + 0.5f);
                int gi = static_cast<int>(g * 255.0f + 0.5f);
                int bi = static_cast<int>(b * 255.0f + 0.5f);
                dst[0] = static_cast<uint8_t>(ri < 0 ? 0 : (ri > 255 ? 255 : ri));
                dst[1] = static_cast<uint8_t>(gi < 0 ? 0 : (gi > 255 ? 255 : gi));
                dst[2] = static_cast<uint8_t>(bi < 0 ? 0 : (bi > 255 ? 255 : bi));
                dst[3] = 255;
                gotPixel = true;
            }
        }
    }
    (void)gotPixel;
}

// Submits one world-space triangle with UV/shade to clip + scan.
// With a null env the `s` slot carries the legacy 0.32+0.68*NdotL factor;
// with a timecyc env it carries raw max(NdotL,0) and ShadeTri applies the
// per-channel ambient+sun light instead.
void SubmitTri(const float* mvp, const float p[3][3], const float nrm[3][3], const float uv[3][2],
               int imgIdx, const float* matCol, const float* light, const TexTimeEnv* env,
               int width, int height, RasterCtx& ctx) {
    ClipVert cv[3];
    for (int k = 0; k < 3; ++k) {
        float clip[4];
        XformPoint(mvp, p[k], clip);
        cv[k].x = clip[0];
        cv[k].y = clip[1];
        cv[k].z = clip[2];
        cv[k].w = clip[3];
        cv[k].u = uv[k][0];
        cv[k].v = uv[k][1];
        float d = nrm[k][0] * light[0] + nrm[k][1] * light[1] + nrm[k][2] * light[2];
        if (env) {
            cv[k].s = d > 0.0f ? d : 0.0f;
        } else {
            cv[k].s = 0.32f + 0.68f * (d > 0.0f ? d : 0.0f);
        }
    }
    ClipVert poly[16];
    int m = ClipTriangle(cv, poly);
    if (m < 3) {
        return;
    }
    ++ctx.st->tris;
    if (imgIdx >= 0) {
        ++ctx.st->sampledTri;
    } else if (imgIdx == -2) {
        ++ctx.st->fallbackTri;
    } else {
        ++ctx.st->flatTri;
    }
    for (int i = 1; i + 1 < m; ++i) {
        const ClipVert* q[3] = { &poly[0], &poly[i], &poly[i + 1] };
        RasterTri t;
        bool bad = false;
        for (int k = 0; k < 3; ++k) {
            if (q[k]->w <= 1e-9f) {
                bad = true;
                break;
            }
            float inv = 1.0f / q[k]->w;
            t.sx[k] = (q[k]->x * inv * 0.5f + 0.5f) * width;
            t.sy[k] = (q[k]->y * inv * 0.5f + 0.5f) * height;
            t.o[k] = inv;
            t.uo[k] = q[k]->u * inv;
            t.vo[k] = q[k]->v * inv;
            t.so[k] = q[k]->s * inv;
            t.zo[k] = q[k]->z * inv;
        }
        if (bad) {
            continue;
        }
        ShadeTri(t, imgIdx, matCol, ctx);
    }
}

void RenderScene(const WorldShotScene& scene, int width, int height, const float* mvp,
                 bool spin, float angleRad, const float* center, const TexTimeEnv* env,
                 std::vector<uint8_t>& outRGBA, TexFrameStats& stats) {
    outRGBA.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
    if (env) {
        // Timecyc sky: vertical SkyBot->SkyTop gradient, painted per pixel
        // BEFORE geometry (bottom-up pixels: row 0 = frame bottom = SkyBot).
        for (int y = 0; y < height; ++y) {
            const float f =
                height <= 1 ? 0.0f : static_cast<float>(y) / static_cast<float>(height - 1);
            int ch[3];
            for (int c = 0; c < 3; ++c) {
                const float v = static_cast<float>(env->skyBot[c]) +
                                (static_cast<float>(env->skyTop[c]) -
                                 static_cast<float>(env->skyBot[c])) *
                                    f;
                int iv = static_cast<int>(v + 0.5f);
                ch[c] = iv < 0 ? 0 : (iv > 255 ? 255 : iv);
            }
            uint8_t* row = outRGBA.data() + static_cast<size_t>(y) * width * 4;
            for (int x = 0; x < width; ++x) {
                row[x * 4 + 0] = static_cast<uint8_t>(ch[0]);
                row[x * 4 + 1] = static_cast<uint8_t>(ch[1]);
                row[x * 4 + 2] = static_cast<uint8_t>(ch[2]);
                row[x * 4 + 3] = 255;
            }
        }
    } else {
        // Background matches the old GL clear color (0.05,0.07,0.12).
        for (size_t i = 0; i < outRGBA.size(); i += 4) {
            outRGBA[i] = 13;
            outRGBA[i + 1] = 18;
            outRGBA[i + 2] = 31;
            outRGBA[i + 3] = 255;
        }
    }
    std::vector<float> zbuf(static_cast<size_t>(width) * static_cast<size_t>(height), 1.0f);
    RasterCtx ctx;
    ctx.w = width;
    ctx.h = height;
    ctx.px = outRGBA.data();
    ctx.zbuf = zbuf.data();
    ctx.scene = &scene;
    ctx.st = &stats;
    ctx.env = env;
    stats = TexFrameStats{};
    ctx.st = &stats;
    float light[3] = { 0.45f, -0.55f, 0.70f };
    Normalize3f(light);
    float ca = std::cos(angleRad);
    float sa = std::sin(angleRad);
    for (const WorldShotMesh& mesh : scene.meshes) {
        size_t vcount = mesh.pos.size() / 3;
        if (vcount % 3 != 0 || mesh.tris <= 0) {
            continue;
        }
        bool hasUV = mesh.uv.size() == mesh.pos.size() / 3 * 2;
        bool hasImg = mesh.triImg.size() == static_cast<size_t>(mesh.tris);
        bool hasCol = mesh.triCol.size() == static_cast<size_t>(mesh.tris) * 3;
        for (int t = 0; t < mesh.tris; ++t) {
            float p[3][3], n[3][3], uv[3][2];
            for (int k = 0; k < 3; ++k) {
                size_t vi = static_cast<size_t>(t) * 3 + k;
                float x = mesh.pos[vi * 3];
                float y = mesh.pos[vi * 3 + 1];
                float z = mesh.pos[vi * 3 + 2];
                float nx = mesh.nrm[vi * 3];
                float ny = mesh.nrm[vi * 3 + 1];
                float nz = mesh.nrm[vi * 3 + 2];
                if (spin) {
                    float dx = x - center[0];
                    float dy = y - center[1];
                    x = center[0] + ca * dx - sa * dy;
                    y = center[1] + sa * dx + ca * dy;
                    float wx = ca * nx - sa * ny;
                    float wy = sa * nx + ca * ny;
                    nx = wx;
                    ny = wy;
                }
                p[k][0] = x;
                p[k][1] = y;
                p[k][2] = z;
                n[k][0] = nx;
                n[k][1] = ny;
                n[k][2] = nz;
                if (hasUV) {
                    uv[k][0] = mesh.uv[vi * 2];
                    uv[k][1] = mesh.uv[vi * 2 + 1];
                } else {
                    uv[k][0] = 0.0f;
                    uv[k][1] = 0.0f;
                }
            }
            int imgIdx = hasImg ? mesh.triImg[t] : -1;
            if (imgIdx >= 0 &&
                (imgIdx >= static_cast<int>(scene.images.size()) ||
                 scene.images[imgIdx].rgba.empty())) {
                imgIdx = -2; // decoded table lost it: honest fallback, never fake
            }
            float matCol[3] = { mesh.color[0], mesh.color[1], mesh.color[2] };
            if (hasCol) {
                matCol[0] = mesh.triCol[t * 3];
                matCol[1] = mesh.triCol[t * 3 + 1];
                matCol[2] = mesh.triCol[t * 3 + 2];
            }
            SubmitTri(mvp, p, n, uv, imgIdx, matCol, light, env, width, height, ctx);
        }
    }
}

} // namespace

namespace {
// Shared orbit camera; env == null keeps the legacy look bit-for-bit.
void RenderOrbitImpl(const WorldShotScene& scene, int width, int height, float angleDeg,
                     const float* eyeOverrideOrNull, const TexTimeEnv* env,
                     std::vector<uint8_t>& outRGBA, TexFrameStats& stats);
} // namespace

void TexSample_RenderOrbit(const WorldShotScene& scene, int width, int height, float angleDeg,
                           const float* eyeOverrideOrNull, std::vector<uint8_t>& outRGBA,
                           TexFrameStats& stats) {
    RenderOrbitImpl(scene, width, height, angleDeg, eyeOverrideOrNull, nullptr, outRGBA, stats);
}

void TexSample_RenderOrbitTC(const WorldShotScene& scene, int width, int height, float angleDeg,
                             const float* eyeOverrideOrNull, const TexTimeEnv& env,
                             std::vector<uint8_t>& outRGBA, TexFrameStats& stats) {
    RenderOrbitImpl(scene, width, height, angleDeg, eyeOverrideOrNull, &env, outRGBA, stats);
}

namespace {

void RenderOrbitImpl(const WorldShotScene& scene, int width, int height, float angleDeg,
                     const float* eyeOverrideOrNull, const TexTimeEnv* env,
                     std::vector<uint8_t>& outRGBA, TexFrameStats& stats) {
    float center[3] = {
        0.5f * (scene.bboxMin[0] + scene.bboxMax[0]),
        0.5f * (scene.bboxMin[1] + scene.bboxMax[1]),
        0.5f * (scene.bboxMin[2] + scene.bboxMax[2]),
    };
    float dx = scene.bboxMax[0] - scene.bboxMin[0];
    float dy = scene.bboxMax[1] - scene.bboxMin[1];
    float dz = scene.bboxMax[2] - scene.bboxMin[2];
    float radius = 0.5f * std::sqrt(dx * dx + dy * dy + dz * dz);
    if (!(radius > 0.5f)) {
        radius = 0.5f;
    }
    float dist = radius * 2.35f + 0.5f;
    float nearPlane = dist - radius * 1.8f;
    if (nearPlane < 0.1f) {
        nearPlane = 0.1f;
    }
    float farPlane = dist + radius * 6.0f;
    float aspect = static_cast<float>(width) / static_cast<float>(height);
    float halfH = nearPlane * 0.5f;
    float proj[16];
    BuildFrustum(-halfH * aspect, halfH * aspect, -halfH, halfH, nearPlane, farPlane, proj);
    float dir[3] = { 0.55f, -0.75f, 0.50f };
    Normalize3f(dir);
    float eye[3] = {
        center[0] + dir[0] * dist,
        center[1] + dir[1] * dist,
        center[2] + dir[2] * dist,
    };
    if (eyeOverrideOrNull) {
        eye[0] = eyeOverrideOrNull[0];
        eye[1] = eyeOverrideOrNull[1];
        eye[2] = eyeOverrideOrNull[2];
    }
    float view[16];
    BuildView(eye, center, view);
    float mvp[16];
    Mul44(proj, view, mvp);
    RenderScene(scene, width, height, mvp, true, angleDeg * 3.14159265f / 180.0f, center, env,
                outRGBA, stats);
}

} // namespace

void TexSample_RenderPath(const WorldShotScene& scene, int width, int height, const float eye[3],
                          const float target[3], std::vector<uint8_t>& outRGBA,
                          TexFrameStats& stats) {
    float aspect = static_cast<float>(width) / static_cast<float>(height);
    const float nearPlane = 1.0f;
    const float farPlane = 6000.0f;
    const float halfH = nearPlane * 0.57735027f; // tan(30deg)
    float proj[16];
    BuildFrustum(-halfH * aspect, halfH * aspect, -halfH, halfH, nearPlane, farPlane, proj);
    float view[16];
    BuildView(eye, target, view);
    float mvp[16];
    Mul44(proj, view, mvp);
    float origin[3] = { 0.0f, 0.0f, 0.0f };
    RenderScene(scene, width, height, mvp, false, 0.0f, origin, nullptr, outRGBA, stats);
}

// --- Duo render (R6t): shared-depth car+ped frame with actor accounting ---
// Duplicates the legacy shading math exactly (same light, same background,
// same wrap/alpha/depth rules); the ONLY additions are the per-pixel
// coverage bits + winner id. Legacy ShadeTri/SubmitTri/RenderScene above
// are untouched.

namespace {

struct DuoCtx {
    RasterCtx base;
    uint8_t* carCov = nullptr; // per-pixel coverage: this actor projected here
    uint8_t* pedCov = nullptr;
    int8_t* win = nullptr; // per-pixel depth winner: -1 bg, 0 car, 1 ped
};

void ShadeTriDuo(const RasterTri& t, int imgIdx, const float* matCol, DuoCtx& dctx, int actor) {
    RasterCtx& ctx = dctx.base;
    const WorldShotImage* img = imgIdx >= 0 ? &ctx.scene->images[imgIdx] : nullptr;
    uint32_t filter = img ? img->filter : 0;
    uint32_t uMode = (filter >> 8) & 0xF;
    uint32_t vMode = (filter >> 12) & 0xF;
    float minX = t.sx[0], maxX = t.sx[0], minY = t.sy[0], maxY = t.sy[0];
    for (int k = 1; k < 3; ++k) {
        if (t.sx[k] < minX) {
            minX = t.sx[k];
        }
        if (t.sx[k] > maxX) {
            maxX = t.sx[k];
        }
        if (t.sy[k] < minY) {
            minY = t.sy[k];
        }
        if (t.sy[k] > maxY) {
            maxY = t.sy[k];
        }
    }
    int x0 = static_cast<int>(std::floor(minX));
    int x1 = static_cast<int>(std::ceil(maxX));
    int y0 = static_cast<int>(std::floor(minY));
    int y1 = static_cast<int>(std::ceil(maxY));
    if (x0 < 0) {
        x0 = 0;
    }
    if (y0 < 0) {
        y0 = 0;
    }
    if (x1 > ctx.w) {
        x1 = ctx.w;
    }
    if (y1 > ctx.h) {
        y1 = ctx.h;
    }
    if (x0 >= x1 || y0 >= y1) {
        return;
    }
    float area =
        (t.sx[1] - t.sx[0]) * (t.sy[2] - t.sy[0]) - (t.sx[2] - t.sx[0]) * (t.sy[1] - t.sy[0]);
    if (area == 0.0f) {
        return;
    }
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            float px = static_cast<float>(x) + 0.5f;
            float py = static_cast<float>(y) + 0.5f;
            float e0 = (t.sx[1] - t.sx[0]) * (py - t.sy[0]) - (t.sy[1] - t.sy[0]) * (px - t.sx[0]);
            float e1 = (t.sx[2] - t.sx[1]) * (py - t.sy[1]) - (t.sy[2] - t.sy[1]) * (px - t.sx[1]);
            float e2 = (t.sx[0] - t.sx[2]) * (py - t.sy[2]) - (t.sy[0] - t.sy[2]) * (px - t.sx[2]);
            if (!((e0 >= 0.0f && e1 >= 0.0f && e2 >= 0.0f) ||
                  (e0 <= 0.0f && e1 <= 0.0f && e2 <= 0.0f))) {
                continue;
            }
            float l0 = e1 / area;
            float l1 = e2 / area;
            float l2 = e0 / area;
            float den = l0 * t.o[0] + l1 * t.o[1] + l2 * t.o[2];
            if (den <= 1e-12f) {
                continue;
            }
            const size_t pix = static_cast<size_t>(y) * ctx.w + x;
            // Projected: valid fragment for this actor, before the depth
            // test (even on alpha-cutout below). This is the "both
            // projected" half of the overlap proof; the shared zbuf below
            // decides the visible winner.
            if (actor == 0) {
                dctx.carCov[pix] = 1;
            } else {
                dctx.pedCov[pix] = 1;
            }
            float z = l0 * t.zo[0] + l1 * t.zo[1] + l2 * t.zo[2];
            float shade = (l0 * t.so[0] + l1 * t.so[1] + l2 * t.so[2]) / den;
            float li[3] = { shade, shade, shade };
            float r, g, b;
            if (img) {
                float u = (l0 * t.uo[0] + l1 * t.uo[1] + l2 * t.uo[2]) / den;
                float v = (l0 * t.vo[0] + l1 * t.vo[1] + l2 * t.vo[2]) / den;
                if (!ctx.st->haveUV) {
                    ctx.st->uvMin[0] = ctx.st->uvMax[0] = u;
                    ctx.st->uvMin[1] = ctx.st->uvMax[1] = v;
                    ctx.st->haveUV = true;
                } else {
                    if (u < ctx.st->uvMin[0]) {
                        ctx.st->uvMin[0] = u;
                    }
                    if (u > ctx.st->uvMax[0]) {
                        ctx.st->uvMax[0] = u;
                    }
                    if (v < ctx.st->uvMin[1]) {
                        ctx.st->uvMin[1] = v;
                    }
                    if (v > ctx.st->uvMax[1]) {
                        ctx.st->uvMax[1] = v;
                    }
                }
                bool bh = false;
                int ix = WrapAxis(u, img->w, uMode, bh);
                int iy = WrapAxis(v, img->h, vMode, bh);
                const uint8_t* tx = img->rgba.data() + (static_cast<size_t>(iy) * img->w + ix) * 4;
                ++ctx.st->texelFetch;
                if (!ctx.st->haveFirst) {
                    (void)std::snprintf(ctx.st->firstTex, sizeof(ctx.st->firstTex), "%s",
                                        img->name);
                    ctx.st->firstTexel[0] = tx[0];
                    ctx.st->firstTexel[1] = tx[1];
                    ctx.st->firstTexel[2] = tx[2];
                    ctx.st->firstTexel[3] = tx[3];
                    ctx.st->haveFirst = true;
                }
                if (tx[3] < 128) {
                    continue;
                }
                if (!(z < ctx.zbuf[pix])) {
                    continue;
                }
                r = li[0] * matCol[0] * (tx[0] / 255.0f);
                g = li[1] * matCol[1] * (tx[1] / 255.0f);
                b = li[2] * matCol[2] * (tx[2] / 255.0f);
                ctx.zbuf[pix] = z;
                ++ctx.st->texPixels;
                uint8_t* dst = ctx.px + pix * 4;
                int ri = static_cast<int>(r * 255.0f + 0.5f);
                int gi = static_cast<int>(g * 255.0f + 0.5f);
                int bi = static_cast<int>(b * 255.0f + 0.5f);
                dst[0] = static_cast<uint8_t>(ri < 0 ? 0 : (ri > 255 ? 255 : ri));
                dst[1] = static_cast<uint8_t>(gi < 0 ? 0 : (gi > 255 ? 255 : gi));
                dst[2] = static_cast<uint8_t>(bi < 0 ? 0 : (bi > 255 ? 255 : bi));
                dst[3] = 255;
                if (ctx.st->haveFirst && ctx.st->firstPixel[0] == 0 &&
                    ctx.st->firstPixel[1] == 0 && ctx.st->firstPixel[2] == 0) {
                    ctx.st->firstPixel[0] = dst[0];
                    ctx.st->firstPixel[1] = dst[1];
                    ctx.st->firstPixel[2] = dst[2];
                }
                dctx.win[pix] = static_cast<int8_t>(actor);
            } else if (imgIdx == -2) {
                if (!(z < ctx.zbuf[pix])) {
                    continue;
                }
                r = li[0] * 0.5f;
                g = li[1] * 0.5f;
                b = li[2] * 0.5f;
                ctx.zbuf[pix] = z;
                ++ctx.st->fallbackPixels;
                uint8_t* dst = ctx.px + pix * 4;
                int ri = static_cast<int>(r * 255.0f + 0.5f);
                int gi = static_cast<int>(g * 255.0f + 0.5f);
                int bi = static_cast<int>(b * 255.0f + 0.5f);
                dst[0] = static_cast<uint8_t>(ri < 0 ? 0 : (ri > 255 ? 255 : ri));
                dst[1] = static_cast<uint8_t>(gi < 0 ? 0 : (gi > 255 ? 255 : gi));
                dst[2] = static_cast<uint8_t>(bi < 0 ? 0 : (bi > 255 ? 255 : bi));
                dst[3] = 255;
                dctx.win[pix] = static_cast<int8_t>(actor);
            } else {
                if (!(z < ctx.zbuf[pix])) {
                    continue;
                }
                r = li[0] * matCol[0];
                g = li[1] * matCol[1];
                b = li[2] * matCol[2];
                ctx.zbuf[pix] = z;
                ++ctx.st->flatPixels;
                uint8_t* dst = ctx.px + pix * 4;
                int ri = static_cast<int>(r * 255.0f + 0.5f);
                int gi = static_cast<int>(g * 255.0f + 0.5f);
                int bi = static_cast<int>(b * 255.0f + 0.5f);
                dst[0] = static_cast<uint8_t>(ri < 0 ? 0 : (ri > 255 ? 255 : ri));
                dst[1] = static_cast<uint8_t>(gi < 0 ? 0 : (gi > 255 ? 255 : gi));
                dst[2] = static_cast<uint8_t>(bi < 0 ? 0 : (bi > 255 ? 255 : bi));
                dst[3] = 255;
                dctx.win[pix] = static_cast<int8_t>(actor);
            }
        }
    }
}

void SubmitTriDuo(const float* mvp, const float p[3][3], const float nrm[3][3],
                  const float uv[3][2], int imgIdx, const float* matCol, const float* light,
                  int width, int height, DuoCtx& dctx, int actor) {
    ClipVert cv[3];
    for (int k = 0; k < 3; ++k) {
        float clip[4];
        XformPoint(mvp, p[k], clip);
        cv[k].x = clip[0];
        cv[k].y = clip[1];
        cv[k].z = clip[2];
        cv[k].w = clip[3];
        cv[k].u = uv[k][0];
        cv[k].v = uv[k][1];
        float d = nrm[k][0] * light[0] + nrm[k][1] * light[1] + nrm[k][2] * light[2];
        cv[k].s = 0.32f + 0.68f * (d > 0.0f ? d : 0.0f);
    }
    ClipVert poly[16];
    int m = ClipTriangle(cv, poly);
    if (m < 3) {
        return;
    }
    RasterCtx& ctx = dctx.base;
    ++ctx.st->tris;
    if (imgIdx >= 0) {
        ++ctx.st->sampledTri;
    } else if (imgIdx == -2) {
        ++ctx.st->fallbackTri;
    } else {
        ++ctx.st->flatTri;
    }
    for (int i = 1; i + 1 < m; ++i) {
        const ClipVert* q[3] = { &poly[0], &poly[i], &poly[i + 1] };
        RasterTri t;
        bool bad = false;
        for (int k = 0; k < 3; ++k) {
            if (q[k]->w <= 1e-9f) {
                bad = true;
                break;
            }
            float inv = 1.0f / q[k]->w;
            t.sx[k] = (q[k]->x * inv * 0.5f + 0.5f) * width;
            t.sy[k] = (q[k]->y * inv * 0.5f + 0.5f) * height;
            t.o[k] = inv;
            t.uo[k] = q[k]->u * inv;
            t.vo[k] = q[k]->v * inv;
            t.so[k] = q[k]->s * inv;
            t.zo[k] = q[k]->z * inv;
        }
        if (bad) {
            continue;
        }
        ShadeTriDuo(t, imgIdx, matCol, dctx, actor);
    }
}

} // namespace

void TexSample_RenderDuo(const WorldShotScene& scene, int carMeshes, int width, int height,
                         const float eye[3], const float target[3],
                         std::vector<uint8_t>& outRGBA, TexFrameStats& stats,
                         TexDuoStats& duo) {
    duo = TexDuoStats{};
    stats = TexFrameStats{};
    outRGBA.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
    for (size_t i = 0; i < outRGBA.size(); i += 4) {
        outRGBA[i] = 13;
        outRGBA[i + 1] = 18;
        outRGBA[i + 2] = 31;
        outRGBA[i + 3] = 255;
    }
    std::vector<float> zbuf(static_cast<size_t>(width) * static_cast<size_t>(height), 1.0f);
    std::vector<uint8_t> carCov(static_cast<size_t>(width) * static_cast<size_t>(height), 0);
    std::vector<uint8_t> pedCov(static_cast<size_t>(width) * static_cast<size_t>(height), 0);
    std::vector<int8_t> win(static_cast<size_t>(width) * static_cast<size_t>(height), -1);
    DuoCtx dctx;
    dctx.base.w = width;
    dctx.base.h = height;
    dctx.base.px = outRGBA.data();
    dctx.base.zbuf = zbuf.data();
    dctx.base.scene = &scene;
    dctx.base.st = &stats;
    dctx.base.env = nullptr;
    dctx.carCov = carCov.data();
    dctx.pedCov = pedCov.data();
    dctx.win = win.data();
    float aspect = static_cast<float>(width) / static_cast<float>(height);
    const float nearPlane = 1.0f;
    const float farPlane = 6000.0f;
    const float halfH = nearPlane * 0.57735027f;
    float proj[16];
    BuildFrustum(-halfH * aspect, halfH * aspect, -halfH, halfH, nearPlane, farPlane, proj);
    float view[16];
    BuildView(eye, target, view);
    float mvp[16];
    Mul44(proj, view, mvp);
    float light[3] = { 0.45f, -0.55f, 0.70f };
    Normalize3f(light);
    if (carMeshes < 0) {
        carMeshes = 0;
    }
    if (carMeshes > static_cast<int>(scene.meshes.size())) {
        carMeshes = static_cast<int>(scene.meshes.size());
    }
    for (size_t mi = 0; mi < scene.meshes.size(); ++mi) {
        const WorldShotMesh& mesh = scene.meshes[mi];
        const int actor = (static_cast<int>(mi) < carMeshes) ? 0 : 1;
        size_t vcount = mesh.pos.size() / 3;
        if (vcount % 3 != 0 || mesh.tris <= 0) {
            continue;
        }
        bool hasUV = mesh.uv.size() == mesh.pos.size() / 3 * 2;
        bool hasImg = mesh.triImg.size() == static_cast<size_t>(mesh.tris);
        bool hasCol = mesh.triCol.size() == static_cast<size_t>(mesh.tris) * 3;
        for (int t = 0; t < mesh.tris; ++t) {
            float p[3][3], n[3][3], uv[3][2];
            for (int k = 0; k < 3; ++k) {
                size_t vi = static_cast<size_t>(t) * 3 + k;
                p[k][0] = mesh.pos[vi * 3];
                p[k][1] = mesh.pos[vi * 3 + 1];
                p[k][2] = mesh.pos[vi * 3 + 2];
                n[k][0] = mesh.nrm[vi * 3];
                n[k][1] = mesh.nrm[vi * 3 + 1];
                n[k][2] = mesh.nrm[vi * 3 + 2];
                if (hasUV) {
                    uv[k][0] = mesh.uv[vi * 2];
                    uv[k][1] = mesh.uv[vi * 2 + 1];
                } else {
                    uv[k][0] = 0.0f;
                    uv[k][1] = 0.0f;
                }
            }
            int imgIdx = hasImg ? mesh.triImg[t] : -1;
            if (imgIdx >= 0 &&
                (imgIdx >= static_cast<int>(scene.images.size()) ||
                 scene.images[imgIdx].rgba.empty())) {
                imgIdx = -2;
            }
            float matCol[3] = { mesh.color[0], mesh.color[1], mesh.color[2] };
            if (hasCol) {
                matCol[0] = mesh.triCol[t * 3];
                matCol[1] = mesh.triCol[t * 3 + 1];
                matCol[2] = mesh.triCol[t * 3 + 2];
            }
            SubmitTriDuo(mvp, p, n, uv, imgIdx, matCol, light, width, height, dctx, actor);
        }
    }
    long carPx = 0, pedPx = 0, ov = 0;
    const size_t npix = static_cast<size_t>(width) * static_cast<size_t>(height);
    for (size_t i = 0; i < npix; ++i) {
        if (win[i] == 0) {
            ++carPx;
        } else if (win[i] == 1) {
            ++pedPx;
        }
        if (carCov[i] && pedCov[i]) {
            ++ov;
        }
    }
    duo.carPixels = carPx;
    duo.pedPixels = pedPx;
    duo.overlap = ov;
}

// --- Crowd render (R6u): shared-depth 3-actor frame, Duo generalised ---
// Duplicates the Duo shading math exactly (same light, same background,
// same wrap/alpha/depth rules); the ONLY change is three per-pixel
// coverage bits + winner id in {0,1,2}. The Duo path above is untouched.

namespace {

struct CrowdCtx {
    RasterCtx base;
    uint8_t* cov[3] = { nullptr, nullptr, nullptr }; // per-actor coverage
    int8_t* win = nullptr; // per-pixel depth winner: -1 bg, 0/1/2 actor
};

void ShadeTriCrowd(const RasterTri& t, int imgIdx, const float* matCol, CrowdCtx& dctx,
                   int actor) {
    RasterCtx& ctx = dctx.base;
    const WorldShotImage* img = imgIdx >= 0 ? &ctx.scene->images[imgIdx] : nullptr;
    uint32_t filter = img ? img->filter : 0;
    uint32_t uMode = (filter >> 8) & 0xF;
    uint32_t vMode = (filter >> 12) & 0xF;
    float minX = t.sx[0], maxX = t.sx[0], minY = t.sy[0], maxY = t.sy[0];
    for (int k = 1; k < 3; ++k) {
        if (t.sx[k] < minX) {
            minX = t.sx[k];
        }
        if (t.sx[k] > maxX) {
            maxX = t.sx[k];
        }
        if (t.sy[k] < minY) {
            minY = t.sy[k];
        }
        if (t.sy[k] > maxY) {
            maxY = t.sy[k];
        }
    }
    int x0 = static_cast<int>(std::floor(minX));
    int x1 = static_cast<int>(std::ceil(maxX));
    int y0 = static_cast<int>(std::floor(minY));
    int y1 = static_cast<int>(std::ceil(maxY));
    if (x0 < 0) {
        x0 = 0;
    }
    if (y0 < 0) {
        y0 = 0;
    }
    if (x1 > ctx.w) {
        x1 = ctx.w;
    }
    if (y1 > ctx.h) {
        y1 = ctx.h;
    }
    if (x0 >= x1 || y0 >= y1) {
        return;
    }
    float area =
        (t.sx[1] - t.sx[0]) * (t.sy[2] - t.sy[0]) - (t.sx[2] - t.sx[0]) * (t.sy[1] - t.sy[0]);
    if (area == 0.0f) {
        return;
    }
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            float px = static_cast<float>(x) + 0.5f;
            float py = static_cast<float>(y) + 0.5f;
            float e0 = (t.sx[1] - t.sx[0]) * (py - t.sy[0]) - (t.sy[1] - t.sy[0]) * (px - t.sx[0]);
            float e1 = (t.sx[2] - t.sx[1]) * (py - t.sy[1]) - (t.sy[2] - t.sy[1]) * (px - t.sx[1]);
            float e2 = (t.sx[0] - t.sx[2]) * (py - t.sy[2]) - (t.sy[0] - t.sy[2]) * (px - t.sx[2]);
            if (!((e0 >= 0.0f && e1 >= 0.0f && e2 >= 0.0f) ||
                  (e0 <= 0.0f && e1 <= 0.0f && e2 <= 0.0f))) {
                continue;
            }
            float l0 = e1 / area;
            float l1 = e2 / area;
            float l2 = e0 / area;
            float den = l0 * t.o[0] + l1 * t.o[1] + l2 * t.o[2];
            if (den <= 1e-12f) {
                continue;
            }
            const size_t pix = static_cast<size_t>(y) * ctx.w + x;
            // Projected: valid fragment for this actor, before the depth
            // test (even on alpha-cutout below). Overlap counts below use
            // these bits; the shared zbuf decides the visible winner.
            if (actor >= 0 && actor < 3) {
                dctx.cov[actor][pix] = 1;
            }
            float z = l0 * t.zo[0] + l1 * t.zo[1] + l2 * t.zo[2];
            float shade = (l0 * t.so[0] + l1 * t.so[1] + l2 * t.so[2]) / den;
            float li[3] = { shade, shade, shade };
            float r, g, b;
            if (img) {
                float u = (l0 * t.uo[0] + l1 * t.uo[1] + l2 * t.uo[2]) / den;
                float v = (l0 * t.vo[0] + l1 * t.vo[1] + l2 * t.vo[2]) / den;
                if (!ctx.st->haveUV) {
                    ctx.st->uvMin[0] = ctx.st->uvMax[0] = u;
                    ctx.st->uvMin[1] = ctx.st->uvMax[1] = v;
                    ctx.st->haveUV = true;
                } else {
                    if (u < ctx.st->uvMin[0]) {
                        ctx.st->uvMin[0] = u;
                    }
                    if (u > ctx.st->uvMax[0]) {
                        ctx.st->uvMax[0] = u;
                    }
                    if (v < ctx.st->uvMin[1]) {
                        ctx.st->uvMin[1] = v;
                    }
                    if (v > ctx.st->uvMax[1]) {
                        ctx.st->uvMax[1] = v;
                    }
                }
                bool bh = false;
                int ix = WrapAxis(u, img->w, uMode, bh);
                int iy = WrapAxis(v, img->h, vMode, bh);
                const uint8_t* tx = img->rgba.data() + (static_cast<size_t>(iy) * img->w + ix) * 4;
                ++ctx.st->texelFetch;
                if (!ctx.st->haveFirst) {
                    (void)std::snprintf(ctx.st->firstTex, sizeof(ctx.st->firstTex), "%s",
                                        img->name);
                    ctx.st->firstTexel[0] = tx[0];
                    ctx.st->firstTexel[1] = tx[1];
                    ctx.st->firstTexel[2] = tx[2];
                    ctx.st->firstTexel[3] = tx[3];
                    ctx.st->haveFirst = true;
                }
                if (tx[3] < 128) {
                    continue;
                }
                if (!(z < ctx.zbuf[pix])) {
                    continue;
                }
                r = li[0] * matCol[0] * (tx[0] / 255.0f);
                g = li[1] * matCol[1] * (tx[1] / 255.0f);
                b = li[2] * matCol[2] * (tx[2] / 255.0f);
                ctx.zbuf[pix] = z;
                ++ctx.st->texPixels;
                uint8_t* dst = ctx.px + pix * 4;
                int ri = static_cast<int>(r * 255.0f + 0.5f);
                int gi = static_cast<int>(g * 255.0f + 0.5f);
                int bi = static_cast<int>(b * 255.0f + 0.5f);
                dst[0] = static_cast<uint8_t>(ri < 0 ? 0 : (ri > 255 ? 255 : ri));
                dst[1] = static_cast<uint8_t>(gi < 0 ? 0 : (gi > 255 ? 255 : gi));
                dst[2] = static_cast<uint8_t>(bi < 0 ? 0 : (bi > 255 ? 255 : bi));
                dst[3] = 255;
                if (ctx.st->haveFirst && ctx.st->firstPixel[0] == 0 &&
                    ctx.st->firstPixel[1] == 0 && ctx.st->firstPixel[2] == 0) {
                    ctx.st->firstPixel[0] = dst[0];
                    ctx.st->firstPixel[1] = dst[1];
                    ctx.st->firstPixel[2] = dst[2];
                }
                dctx.win[pix] = static_cast<int8_t>(actor);
            } else if (imgIdx == -2) {
                if (!(z < ctx.zbuf[pix])) {
                    continue;
                }
                r = li[0] * 0.5f;
                g = li[1] * 0.5f;
                b = li[2] * 0.5f;
                ctx.zbuf[pix] = z;
                ++ctx.st->fallbackPixels;
                uint8_t* dst = ctx.px + pix * 4;
                int ri = static_cast<int>(r * 255.0f + 0.5f);
                int gi = static_cast<int>(g * 255.0f + 0.5f);
                int bi = static_cast<int>(b * 255.0f + 0.5f);
                dst[0] = static_cast<uint8_t>(ri < 0 ? 0 : (ri > 255 ? 255 : ri));
                dst[1] = static_cast<uint8_t>(gi < 0 ? 0 : (gi > 255 ? 255 : gi));
                dst[2] = static_cast<uint8_t>(bi < 0 ? 0 : (bi > 255 ? 255 : bi));
                dst[3] = 255;
                dctx.win[pix] = static_cast<int8_t>(actor);
            } else {
                if (!(z < ctx.zbuf[pix])) {
                    continue;
                }
                r = li[0] * matCol[0];
                g = li[1] * matCol[1];
                b = li[2] * matCol[2];
                ctx.zbuf[pix] = z;
                ++ctx.st->flatPixels;
                uint8_t* dst = ctx.px + pix * 4;
                int ri = static_cast<int>(r * 255.0f + 0.5f);
                int gi = static_cast<int>(g * 255.0f + 0.5f);
                int bi = static_cast<int>(b * 255.0f + 0.5f);
                dst[0] = static_cast<uint8_t>(ri < 0 ? 0 : (ri > 255 ? 255 : ri));
                dst[1] = static_cast<uint8_t>(gi < 0 ? 0 : (gi > 255 ? 255 : gi));
                dst[2] = static_cast<uint8_t>(bi < 0 ? 0 : (bi > 255 ? 255 : bi));
                dst[3] = 255;
                dctx.win[pix] = static_cast<int8_t>(actor);
            }
        }
    }
}

void SubmitTriCrowd(const float* mvp, const float p[3][3], const float nrm[3][3],
                    const float uv[3][2], int imgIdx, const float* matCol, const float* light,
                    int width, int height, CrowdCtx& dctx, int actor) {
    ClipVert cv[3];
    for (int k = 0; k < 3; ++k) {
        float clip[4];
        XformPoint(mvp, p[k], clip);
        cv[k].x = clip[0];
        cv[k].y = clip[1];
        cv[k].z = clip[2];
        cv[k].w = clip[3];
        cv[k].u = uv[k][0];
        cv[k].v = uv[k][1];
        float d = nrm[k][0] * light[0] + nrm[k][1] * light[1] + nrm[k][2] * light[2];
        cv[k].s = 0.32f + 0.68f * (d > 0.0f ? d : 0.0f);
    }
    ClipVert poly[16];
    int m = ClipTriangle(cv, poly);
    if (m < 3) {
        return;
    }
    RasterCtx& ctx = dctx.base;
    ++ctx.st->tris;
    if (imgIdx >= 0) {
        ++ctx.st->sampledTri;
    } else if (imgIdx == -2) {
        ++ctx.st->fallbackTri;
    } else {
        ++ctx.st->flatTri;
    }
    for (int i = 1; i + 1 < m; ++i) {
        const ClipVert* q[3] = { &poly[0], &poly[i], &poly[i + 1] };
        RasterTri t;
        bool bad = false;
        for (int k = 0; k < 3; ++k) {
            if (q[k]->w <= 1e-9f) {
                bad = true;
                break;
            }
            float inv = 1.0f / q[k]->w;
            t.sx[k] = (q[k]->x * inv * 0.5f + 0.5f) * width;
            t.sy[k] = (q[k]->y * inv * 0.5f + 0.5f) * height;
            t.o[k] = inv;
            t.uo[k] = q[k]->u * inv;
            t.vo[k] = q[k]->v * inv;
            t.so[k] = q[k]->s * inv;
            t.zo[k] = q[k]->z * inv;
        }
        if (bad) {
            continue;
        }
        ShadeTriCrowd(t, imgIdx, matCol, dctx, actor);
    }
}

} // namespace

void TexSample_RenderCrowd(const WorldShotScene& scene, int meshEnd0, int meshEnd1, int width,
                           int height, const float eye[3], const float target[3],
                           std::vector<uint8_t>& outRGBA, TexFrameStats& stats,
                           TexCrowdStats& crowd) {
    crowd = TexCrowdStats{};
    stats = TexFrameStats{};
    outRGBA.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
    for (size_t i = 0; i < outRGBA.size(); i += 4) {
        outRGBA[i] = 13;
        outRGBA[i + 1] = 18;
        outRGBA[i + 2] = 31;
        outRGBA[i + 3] = 255;
    }
    const size_t npix = static_cast<size_t>(width) * static_cast<size_t>(height);
    std::vector<float> zbuf(npix, 1.0f);
    std::vector<uint8_t> cov0(npix, 0), cov1(npix, 0), cov2(npix, 0);
    std::vector<int8_t> win(npix, -1);
    CrowdCtx dctx;
    dctx.base.w = width;
    dctx.base.h = height;
    dctx.base.px = outRGBA.data();
    dctx.base.zbuf = zbuf.data();
    dctx.base.scene = &scene;
    dctx.base.st = &stats;
    dctx.base.env = nullptr;
    dctx.cov[0] = cov0.data();
    dctx.cov[1] = cov1.data();
    dctx.cov[2] = cov2.data();
    dctx.win = win.data();
    float aspect = static_cast<float>(width) / static_cast<float>(height);
    const float nearPlane = 1.0f;
    const float farPlane = 6000.0f;
    const float halfH = nearPlane * 0.57735027f;
    float proj[16];
    BuildFrustum(-halfH * aspect, halfH * aspect, -halfH, halfH, nearPlane, farPlane, proj);
    float view[16];
    BuildView(eye, target, view);
    float mvp[16];
    Mul44(proj, view, mvp);
    float light[3] = { 0.45f, -0.55f, 0.70f };
    Normalize3f(light);
    const int nMesh = static_cast<int>(scene.meshes.size());
    int e0 = meshEnd0 < 0 ? 0 : meshEnd0;
    int e1 = meshEnd1 < e0 ? e0 : meshEnd1;
    if (e0 > nMesh) {
        e0 = nMesh;
    }
    if (e1 > nMesh) {
        e1 = nMesh;
    }
    for (size_t mi = 0; mi < scene.meshes.size(); ++mi) {
        const WorldShotMesh& mesh = scene.meshes[mi];
        const int actor = (static_cast<int>(mi) < e0) ? 0 : ((static_cast<int>(mi) < e1) ? 1 : 2);
        size_t vcount = mesh.pos.size() / 3;
        if (vcount % 3 != 0 || mesh.tris <= 0) {
            continue;
        }
        bool hasUV = mesh.uv.size() == mesh.pos.size() / 3 * 2;
        bool hasImg = mesh.triImg.size() == static_cast<size_t>(mesh.tris);
        bool hasCol = mesh.triCol.size() == static_cast<size_t>(mesh.tris) * 3;
        for (int t = 0; t < mesh.tris; ++t) {
            float p[3][3], n[3][3], uv[3][2];
            for (int k = 0; k < 3; ++k) {
                size_t vi = static_cast<size_t>(t) * 3 + k;
                p[k][0] = mesh.pos[vi * 3];
                p[k][1] = mesh.pos[vi * 3 + 1];
                p[k][2] = mesh.pos[vi * 3 + 2];
                n[k][0] = mesh.nrm[vi * 3];
                n[k][1] = mesh.nrm[vi * 3 + 1];
                n[k][2] = mesh.nrm[vi * 3 + 2];
                if (hasUV) {
                    uv[k][0] = mesh.uv[vi * 2];
                    uv[k][1] = mesh.uv[vi * 2 + 1];
                } else {
                    uv[k][0] = 0.0f;
                    uv[k][1] = 0.0f;
                }
            }
            int imgIdx = hasImg ? mesh.triImg[t] : -1;
            if (imgIdx >= 0 &&
                (imgIdx >= static_cast<int>(scene.images.size()) ||
                 scene.images[imgIdx].rgba.empty())) {
                imgIdx = -2;
            }
            float matCol[3] = { mesh.color[0], mesh.color[1], mesh.color[2] };
            if (hasCol) {
                matCol[0] = mesh.triCol[t * 3];
                matCol[1] = mesh.triCol[t * 3 + 1];
                matCol[2] = mesh.triCol[t * 3 + 2];
            }
            SubmitTriCrowd(mvp, p, n, uv, imgIdx, matCol, light, width, height, dctx, actor);
        }
    }
    long px[3] = { 0, 0, 0 };
    long ov12 = 0, ovAll = 0;
    for (size_t i = 0; i < npix; ++i) {
        if (win[i] >= 0 && win[i] < 3) {
            ++px[win[i]];
        }
        const int bits =
            (cov0[i] ? 1 : 0) + (cov1[i] ? 1 : 0) + (cov2[i] ? 1 : 0);
        if (bits >= 2) {
            ++ov12;
        }
        if (bits == 3) {
            ++ovAll;
        }
    }
    crowd.pix[0] = px[0];
    crowd.pix[1] = px[1];
    crowd.pix[2] = px[2];
    crowd.overlap12 = ov12;
    crowd.overlapAll = ovAll;
}
