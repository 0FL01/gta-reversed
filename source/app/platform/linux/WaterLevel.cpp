// WaterLevel implementation: water.dat parse + flat WaterRGBA scene build.
// See WaterLevel.h for the contract and the game-code references.

#include "app/platform/linux/WaterLevel.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

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

namespace {

void SetErr(char* err, std::size_t errSize, const char* msg) {
    if (!err || errSize == 0) {
        return;
    }
    (void)std::snprintf(err, errSize, "%s", msg ? msg : "water error");
}

std::string TrimRight(const std::string& s) {
    size_t e = s.size();
    while (e > 0 && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r')) {
        --e;
    }
    return s.substr(0, e);
}

// Parses one water data line: 3-4 vertices of 7 floats each
// (x y z flowX flowY bigWaves smallWaves, the game's ReadNextVertex order)
// plus an optional trailing flag. Mirrors the game's stream logic: a
// vertex either parses fully or the scan stops before it; fewer than 3
// vertices means the line is skipped. hasFlag reports whether a trailing
// flag token was present (water.dat: always; water1.dat: never).
bool ParseWaterLine(const char* line, WaterPoly& poly, bool& hasFlag) {
    poly = WaterPoly{};
    hasFlag = false;
    const char* p = line;
    float verts[4][7];
    int nverts = 0;
    for (; nverts < 4; ++nverts) {
        float v[7] = {};
        const char* q = p;
        bool ok = true;
        for (int k = 0; k < 7; ++k) {
            while (*q == ' ' || *q == '\t') {
                ++q;
            }
            if (*q == '\0') {
                ok = false;
                break;
            }
            char* end = nullptr;
            const float f = std::strtof(q, &end);
            if (end == q || !std::isfinite(f)) {
                ok = false;
                break;
            }
            v[k] = f;
            q = end;
        }
        if (!ok) {
            break; // stop before this vertex, like the game's seek-back
        }
        for (int k = 0; k < 7; ++k) {
            verts[nverts][k] = v[k];
        }
        p = q;
    }
    if (nverts < 3) {
        return false;
    }
    uint32_t flags = 0; // absent flag reads as 0, like `liness >> flags`
    {
        const char* q = p;
        while (*q == ' ' || *q == '\t') {
            ++q;
        }
        if (*q != '\0') {
            char* end = nullptr;
            const unsigned long f = std::strtoul(q, &end, 10);
            if (end != q) {
                flags = static_cast<uint32_t>(f);
                hasFlag = true;
            }
        }
    }
    poly.nverts = nverts;
    poly.flags = flags;
    poly.hasFlag = hasFlag;
    for (int i = 0; i < nverts; ++i) {
        poly.v[i].x = verts[i][0];
        poly.v[i].y = verts[i][1];
        poly.v[i].z = verts[i][2];
    }
    return true;
}

} // namespace

bool WaterLevel_Load(const char* gameDir, const char* waterFile, WaterLevelData& out,
                       char* err, std::size_t errSize) {
    out = WaterLevelData{};
    if (!gameDir || !gameDir[0]) {
        SetErr(err, errSize, "no game dir");
        return false;
    }
    // Only the two shipped water files are allowed (no path traversal:
    // the value is matched exactly, never concatenated from user input).
    const bool isWater1 = waterFile && std::strcmp(waterFile, "data/water1.dat") == 0;
    const bool isWater = waterFile && std::strcmp(waterFile, "data/water.dat") == 0;
    if (!isWater && !isWater1) {
        SetErr(err, errSize, "bad water file (want data/water.dat|data/water1.dat)");
        return false;
    }
    OS_SetFilePathOffset(gameDir);
    void* file = nullptr;
    if (OS_FileOpen(FILE_DATA_AREA_DEFAULT, &file, waterFile, FILE_ACCESS_READ) != 0 ||
        !file) {
        char msg[128] = {};
        (void)std::snprintf(msg, sizeof(msg), "cannot open %s", waterFile);
        SetErr(err, errSize, msg);
        return false;
    }
    int32 size = OS_FileSize(file);
    if (size <= 0) {
        OS_FileClose(file);
        char msg[128] = {};
        (void)std::snprintf(msg, sizeof(msg), "empty %s", waterFile);
        SetErr(err, errSize, msg);
        return false;
    }
    std::vector<char> buf(static_cast<size_t>(size));
    int32 rc = OS_FileRead(file, buf.data(), size);
    OS_FileClose(file);
    if (rc != 0) {
        char msg[128] = {};
        (void)std::snprintf(msg, sizeof(msg), "cannot read %s", waterFile);
        SetErr(err, errSize, msg);
        return false;
    }
    std::vector<std::string> lines;
    {
        size_t pos = 0;
        while (pos < buf.size()) {
            size_t end = pos;
            while (end < buf.size() && buf[end] != '\n') {
                ++end;
            }
            lines.emplace_back(buf.data() + pos, end - pos);
            pos = end < buf.size() ? end + 1 : buf.size();
        }
    }
    if (lines.empty() || TrimRight(lines[0]) != "processed") {
        char msg[128] = {};
        (void)std::snprintf(msg, sizeof(msg), "%s missing 'processed' header", waterFile);
        SetErr(err, errSize, msg);
        return false;
    }
    (void)std::snprintf(out.file, sizeof(out.file), "%s", waterFile);
    bool haveBox = false;
    for (size_t i = 1; i < lines.size(); ++i) {
        const std::string ln = TrimRight(lines[i]);
        if (ln.empty()) {
            continue;
        }
        WaterPoly poly{};
        bool hasFlag = false;
        if (!ParseWaterLine(ln.c_str(), poly, hasFlag)) {
            ++out.skipped;
            continue;
        }
        if (!hasFlag) {
            // The shipped water.dat always carries the flag (307/307), so
            // this default never fires there; water1.dat omits the column
            // file-wide and its rows count as visible water (see header).
            ++out.noflag;
            if (isWater1) {
                poly.flags = 1u;
            }
        }
        ++out.rows;
        if (poly.nverts == 4) {
            ++out.quads;
        } else {
            ++out.tris;
        }
        if (!poly.Visible()) {
            ++out.invis;
        }
        for (int k = 0; k < poly.nverts; ++k) {
            const float p[3] = { poly.v[k].x, poly.v[k].y, poly.v[k].z };
            if (!haveBox) {
                out.bboxMin[0] = out.bboxMax[0] = p[0];
                out.bboxMin[1] = out.bboxMax[1] = p[1];
                out.bboxMin[2] = out.bboxMax[2] = p[2];
                haveBox = true;
            } else {
                for (int c = 0; c < 3; ++c) {
                    if (p[c] < out.bboxMin[c]) {
                        out.bboxMin[c] = p[c];
                    }
                    if (p[c] > out.bboxMax[c]) {
                        out.bboxMax[c] = p[c];
                    }
                }
            }
        }
        out.polys.push_back(poly);
    }
    if (out.rows == 0) {
        char msg[128] = {};
        (void)std::snprintf(msg, sizeof(msg), "%s has no water rows", waterFile);
        SetErr(err, errSize, msg);
        return false;
    }
    return true;
}

bool WaterLevel_BuildScene(const WaterLevelData& data, const uint8_t waterRGBA[4],
                           WorldShotScene& scene, int& trisOut, char* err,
                           std::size_t errSize) {
    scene = WorldShotScene{};
    trisOut = 0;
    if (!waterRGBA) {
        SetErr(err, errSize, "no water color");
        return false;
    }
    const float shade = WaterLevel_LegacyShadeUp();
    if (!(shade > 0.0f) || !std::isfinite(shade)) {
        SetErr(err, errSize, "bad legacy shade");
        return false;
    }
    // Calibration so the legacy flat pipeline (pixel = shade*triCol)
    // reproduces the exact timecyc bytes: triCol = (W/255)/shade.
    float col[3] = {};
    for (int c = 0; c < 3; ++c) {
        col[c] = (static_cast<float>(waterRGBA[c]) / 255.0f) / shade;
    }
    WorldShotMesh mesh{};
    mesh.color[0] = col[0];
    mesh.color[1] = col[1];
    mesh.color[2] = col[2];
    bool haveBox = false;
    auto pushTri = [&](const WaterVert& a, const WaterVert& b, const WaterVert& c) {
        const WaterVert v[3] = { a, b, c };
        for (int k = 0; k < 3; ++k) {
            mesh.pos.push_back(v[k].x);
            mesh.pos.push_back(v[k].y);
            mesh.pos.push_back(v[k].z);
            mesh.nrm.push_back(0.0f);
            mesh.nrm.push_back(0.0f);
            mesh.nrm.push_back(1.0f);
        }
        mesh.triImg.push_back(-1); // untextured by design: flat water color
        mesh.triCol.push_back(col[0]);
        mesh.triCol.push_back(col[1]);
        mesh.triCol.push_back(col[2]);
        ++mesh.tris;
        for (int k = 0; k < 3; ++k) {
            const float p[3] = { v[k].x, v[k].y, v[k].z };
            if (!haveBox) {
                scene.bboxMin[0] = scene.bboxMax[0] = p[0];
                scene.bboxMin[1] = scene.bboxMax[1] = p[1];
                scene.bboxMin[2] = scene.bboxMax[2] = p[2];
                haveBox = true;
            } else {
                for (int c = 0; c < 3; ++c) {
                    if (p[c] < scene.bboxMin[c]) {
                        scene.bboxMin[c] = p[c];
                    }
                    if (p[c] > scene.bboxMax[c]) {
                        scene.bboxMax[c] = p[c];
                    }
                }
            }
        }
    };
    for (const WaterPoly& poly : data.polys) {
        if (!poly.Visible()) {
            continue; // bInvisible: the game never marks these rendered
        }
        if (poly.nverts == 4) {
            // Sort by (y,x) like CWaterLevel::DoVtxSortAndGetRange, then a
            // strip split (the raw file order is a "Z", not a fan).
            const WaterVert* s[4] = { &poly.v[0], &poly.v[1], &poly.v[2], &poly.v[3] };
            for (int a = 0; a < 4; ++a) {
                for (int b = a + 1; b < 4; ++b) {
                    const bool less = (s[b]->y < s[a]->y) ||
                                      (s[b]->y == s[a]->y && s[b]->x < s[a]->x);
                    if (less) {
                        const WaterVert* t = s[a];
                        s[a] = s[b];
                        s[b] = t;
                    }
                }
            }
            pushTri(*s[0], *s[1], *s[2]);
            pushTri(*s[1], *s[2], *s[3]);
        } else if (poly.nverts == 3) {
            pushTri(poly.v[0], poly.v[1], poly.v[2]);
        }
    }
    if (mesh.tris == 0) {
        SetErr(err, errSize, "no visible water polys");
        return false;
    }
    scene.meshes.push_back(std::move(mesh));
    (void)std::snprintf(scene.stats.dffName, sizeof(scene.stats.dffName), "%s",
                        data.file[0] ? data.file : "data/water.dat");
    (void)std::snprintf(scene.stats.txdName, sizeof(scene.stats.txdName), "%s",
                        "data/timecyc.dat:WaterRGBA");
    scene.stats.atomics = 0;
    scene.stats.triangles = trisOut = static_cast<int>(scene.meshes.back().tris);
    scene.stats.vertices = scene.stats.triangles * 3;
    scene.stats.textures = 0;
    scene.stats.firstTexW = 0;
    scene.stats.firstTexH = 0;
    scene.stats.firstTexture[0] = '\0';
    return true;
}

float WaterLevel_LegacyShadeUp() {
    // Identical float ops to TexSample RenderScene/SubmitTri (null env):
    // light = normalize(0.45,-0.55,0.70); d = n.dot(light) with n=(0,0,1);
    // shade = 0.32+0.68*max(d,0).
    float light[3] = { 0.45f, -0.55f, 0.70f };
    float len = std::sqrt(light[0] * light[0] + light[1] * light[1] + light[2] * light[2]);
    if (len > 1e-9f) {
        light[0] /= len;
        light[1] /= len;
        light[2] /= len;
    }
    float d = light[2];
    if (d < 0.0f) {
        d = 0.0f;
    }
    return 0.32f + 0.68f * d;
}

long WaterLevel_CountExact(const std::vector<uint8_t>& rgba, uint8_t r, uint8_t g,
                           uint8_t b) {
    long n = 0;
    for (size_t i = 0; i + 3 < rgba.size() + 1; i += 4) {
        if (rgba[i] == r && rgba[i + 1] == g && rgba[i + 2] == b) {
            ++n;
        }
    }
    return n;
}

void WaterLevel_Shutdown() {
}
