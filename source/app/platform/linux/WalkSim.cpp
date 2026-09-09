// WalkSim implementation: distance-bound composition (R6o).
// Owns no engine itself; reuses the verified slice entry points. Engine
// sharing: StreamPager brings the librw engine up first, IfpAnim reuses it
// through its tolerant RwInitEngine (identical plugin set).

#include "app/platform/linux/WalkSim.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "app/platform/linux/ColLoad.h"
#include "app/platform/linux/Collide.h"

namespace {

constexpr double kPi = 3.14159265358979323846;

double WrapPi(double a) {
    while (a > kPi) {
        a -= 2.0 * kPi;
    }
    while (a < -kPi) {
        a += 2.0 * kPi;
    }
    return a;
}

} // namespace

bool WalkSim_Clip(const char* gameDir, const char* model, const char* anim, WalkClip& out,
                  char* err, std::size_t errSize) {
    out = WalkClip{};
    if (!gameDir || !gameDir[0]) {
        if (err && errSize) {
            (void)std::snprintf(err, errSize, "no game dir");
        }
        return false;
    }
    const char* wantModel = (model && model[0]) ? model : "andre";
    const char* wantAnim = (anim && anim[0]) ? anim : "WALK_civi";
    std::vector<IfpAnimSeqFrame> seq;
    char sErr[512] = {};
    if (!IfpAnim_Seq(gameDir, wantModel, wantAnim, 2, seq, sErr, sizeof(sErr))) {
        if (err && errSize) {
            (void)std::snprintf(err, errSize, "%s", sErr[0] ? sErr : "clip seq failed");
        }
        return false;
    }
    if (seq.size() != 2) {
        if (err && errSize) {
            (void)std::snprintf(err, errSize, "clip seq short got=%d",
                                static_cast<int>(seq.size()));
        }
        return false;
    }
    const IfpAnimStats& s0 = seq[0].stats;
    const IfpAnimStats& s1 = seq[1].stats;
    double dx = static_cast<double>(s1.rootWorld[0]) - s0.rootWorld[0];
    double dy = static_cast<double>(s1.rootWorld[1]) - s0.rootWorld[1];
    double dz = static_cast<double>(s1.rootWorld[2]) - s0.rootWorld[2];
    double stride = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (!(stride > 1e-6) || !std::isfinite(stride)) {
        if (err && errSize) {
            (void)std::snprintf(err, errSize, "bad strideLen=%.6f", stride);
        }
        return false;
    }
    if (!(s0.animTotal > 1e-6) || !std::isfinite(s0.animTotal)) {
        if (err && errSize) {
            (void)std::snprintf(err, errSize, "bad clip total=%.6f", s0.animTotal);
        }
        return false;
    }
    (void)std::snprintf(out.model, sizeof(out.model), "%s", s0.model);
    (void)std::snprintf(out.anim, sizeof(out.anim), "%s", s0.anim);
    (void)std::snprintf(out.src, sizeof(out.src), "%s", s0.src);
    (void)std::snprintf(out.bankSrc, sizeof(out.bankSrc), "%s", s0.bankSrc);
    out.total = s0.animTotal;
    out.strideLen = stride;
    out.root0[0] = s0.rootWorld[0];
    out.root0[1] = s0.rootWorld[1];
    out.root0[2] = s0.rootWorld[2];
    out.root1[0] = s1.rootWorld[0];
    out.root1[1] = s1.rootWorld[1];
    out.root1[2] = s1.rootWorld[2];
    out.footMinZ = s0.animMin[2];
    out.footMaxZ = s0.animMax[2];
    out.bones = s0.bones;
    out.mapped = s0.mapped;
    out.wsum = s0.wsum;
    return true;
}

bool WalkSim_InitWorld(const char* gameDir, E2ELoadInfo& load, char* err, std::size_t errSize) {
    load = E2ELoadInfo{};
    if (!gameDir || !gameDir[0]) {
        if (err && errSize) {
            (void)std::snprintf(err, errSize, "no game dir");
        }
        return false;
    }
    ColLoadStats cst{};
    if (!ColLoad_Init(gameDir, cst, err, errSize)) {
        return false;
    }
    // Keep COL resident; pager init must not tear it down (separate state).
    if (!StreamPager_Init(gameDir, load, err, errSize)) {
        ColLoad_Shutdown();
        return false;
    }
    return true;
}

void WalkSim_ShutdownWorld() {
    StreamPager_Shutdown();
    ColLoad_Shutdown();
    IfpAnim_Shutdown();
}

bool WalkSim_Ground(double x, double y, double& hOut, char* modelOut, std::size_t modelSize,
                    char* primOut, std::size_t primSize) {
    ColProbeHit hit{};
    ColLoad_Probe(x, y, hit);
    hOut = hit.h;
    if (modelOut && modelSize) {
        (void)std::snprintf(modelOut, modelSize, "%s", hit.model);
    }
    if (primOut && primSize) {
        (void)std::snprintf(primOut, primSize, "%s", hit.prim);
    }
    return std::strcmp(hit.prim, "none") != 0;
}

bool WalkSim_Ped(const char* gameDir, const char* model, const char* anim, double phase,
                 WorldShotScene& pedScene, IfpAnimStats& pedStats, char* err,
                 std::size_t errSize) {
    pedScene.meshes.clear();
    pedScene.images.clear();
    if (!(phase >= 0.0 && phase < 1.0)) {
        // phase==1.0 wraps to 0.0 (mod-1 domain); clamp honestly, no invent.
        if (phase == 1.0) {
            phase = 0.0;
        } else {
            if (err && errSize) {
                (void)std::snprintf(err, errSize, "bad phase %.6f (want [0,1))", phase);
            }
            return false;
        }
    }
    // Existing IFP interpolator (lerp trans + slerp quat between the two
    // bracketing IFP keys at this distance phase). No wall-clock anywhere.
    return IfpAnim_Init(gameDir, model, anim, phase, pedScene, pedStats, err, errSize,
                        true /*interp=lerp+slerp*/);
}

bool WalkSim_Page(double x, double y, double z, WorldShotScene& worldScene, E2EPagerFrame& pf,
                  char* err, std::size_t errSize) {
    return StreamPager_Update(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z),
                              worldScene, pf, err, errSize);
}

bool WalkSim_Sample(const std::vector<std::pair<double, double>>& ctrl, int waypoints,
                    double strideLen, double clipTotal, std::vector<WalkWaypoint>& out, char* err,
                    std::size_t errSize) {
    out.clear();
    auto fail = [&](const char* m) {
        if (err && errSize) {
            (void)std::snprintf(err, errSize, "%s", m);
        }
        return false;
    };
    if (ctrl.size() < 2) {
        return fail("need >= 2 control points");
    }
    if (waypoints < 1 || waypoints > 64) {
        return fail("waypoints out of range 1..64");
    }
    if (!(strideLen > 1e-6) || !std::isfinite(strideLen)) {
        return fail("bad strideLen");
    }
    if (!(clipTotal > 1e-6) || !std::isfinite(clipTotal)) {
        return fail("bad clipTotal");
    }
    const size_t p = ctrl.size();
    std::vector<double> cum(p, 0.0);
    for (size_t i = 1; i < p; ++i) {
        double dx = ctrl[i].first - ctrl[i - 1].first;
        double dy = ctrl[i].second - ctrl[i - 1].second;
        cum[i] = cum[i - 1] + std::sqrt(dx * dx + dy * dy);
    }
    double total = cum.back();
    if (!(total > 1e-6)) {
        return fail("degenerate zero-length path");
    }
    std::vector<double> segYaw;
    segYaw.reserve(p > 0 ? p - 1 : 0);
    for (size_t i = 0; i + 1 < p; ++i) {
        double dx = ctrl[i + 1].first - ctrl[i].first;
        double dy = ctrl[i + 1].second - ctrl[i].second;
        if (dx == 0.0 && dy == 0.0) {
            return fail("duplicate control points");
        }
        segYaw.push_back(std::atan2(dy, dx));
    }
    auto fillPhase = [&](WalkWaypoint& w) {
        double cyc = w.dist / strideLen;
        double ph = cyc - std::floor(cyc);
        if (ph < 0.0) {
            ph = 0.0;
        }
        if (ph >= 1.0) {
            ph = 0.0;
        }
        w.phase = ph;
        w.timeAbs = ph * clipTotal;
        (void)WrapPi(0.0);
    };
    out.reserve(static_cast<size_t>(waypoints));
    if (waypoints == 1) {
        WalkWaypoint w;
        w.x = ctrl[0].first;
        w.y = ctrl[0].second;
        w.dist = 0.0;
        w.yawPath = segYaw.front();
        w.yawBody = w.yawPath - kPi * 0.5; // local +Y forward -> path dir
        fillPhase(w);
        out.push_back(w);
        return true;
    }
    if (static_cast<int>(p) == waypoints) {
        for (int i = 0; i < waypoints; ++i) {
            WalkWaypoint w;
            w.x = ctrl[static_cast<size_t>(i)].first;
            w.y = ctrl[static_cast<size_t>(i)].second;
            w.dist = cum[static_cast<size_t>(i)];
            double yaw = (i < static_cast<int>(segYaw.size())) ? segYaw[static_cast<size_t>(i)]
                                                               : segYaw.back();
            w.yawPath = yaw;
            w.yawBody = yaw - kPi * 0.5;
            fillPhase(w);
            out.push_back(w);
        }
        return true;
    }
    double step = total / static_cast<double>(waypoints - 1);
    size_t seg = 0;
    for (int j = 0; j < waypoints; ++j) {
        double s = (j == waypoints - 1) ? total : step * j;
        while (seg + 1 < cum.size() - 1 && cum[seg + 1] < s) {
            ++seg;
        }
        if (seg >= segYaw.size()) {
            seg = segYaw.size() - 1;
        }
        double s0 = cum[seg];
        double s1 = cum[seg + 1];
        double t = (s1 > s0) ? (s - s0) / (s1 - s0) : 0.0;
        if (t < 0.0) {
            t = 0.0;
        }
        if (t > 1.0) {
            t = 1.0;
        }
        WalkWaypoint w;
        w.x = ctrl[seg].first + (ctrl[seg + 1].first - ctrl[seg].first) * t;
        w.y = ctrl[seg].second + (ctrl[seg + 1].second - ctrl[seg].second) * t;
        w.dist = s;
        double yaw = segYaw[seg];
        w.yawPath = yaw;
        w.yawBody = yaw - kPi * 0.5;
        fillPhase(w);
        out.push_back(w);
    }
    return true;
}

void WalkSim_Merge(const WorldShotScene& world, const WorldShotScene& ped, double pedX,
                   double pedY, double pedZtrans, double yawBodyRad, WorldShotScene& out) {
    out.meshes.clear();
    out.images.clear();
    out.images.reserve(world.images.size() + ped.images.size());
    for (const auto& im : world.images) {
        out.images.push_back(im);
    }
    const int imgOff = static_cast<int>(world.images.size());
    for (const auto& im : ped.images) {
        out.images.push_back(im);
    }
    bool haveBox = false;
    auto grow = [&](float x, float y, float z) {
        if (!haveBox) {
            out.bboxMin[0] = out.bboxMax[0] = x;
            out.bboxMin[1] = out.bboxMax[1] = y;
            out.bboxMin[2] = out.bboxMax[2] = z;
            haveBox = true;
        } else {
            if (x < out.bboxMin[0]) {
                out.bboxMin[0] = x;
            }
            if (y < out.bboxMin[1]) {
                out.bboxMin[1] = y;
            }
            if (z < out.bboxMin[2]) {
                out.bboxMin[2] = z;
            }
            if (x > out.bboxMax[0]) {
                out.bboxMax[0] = x;
            }
            if (y > out.bboxMax[1]) {
                out.bboxMax[1] = y;
            }
            if (z > out.bboxMax[2]) {
                out.bboxMax[2] = z;
            }
        }
    };
    for (const auto& m : world.meshes) {
        out.meshes.push_back(m);
        size_t n = m.pos.size() / 3;
        for (size_t i = 0; i < n; ++i) {
            grow(m.pos[i * 3], m.pos[i * 3 + 1], m.pos[i * 3 + 2]);
        }
    }
    const float c = static_cast<float>(std::cos(yawBodyRad));
    const float s = static_cast<float>(std::sin(yawBodyRad));
    for (const auto& m : ped.meshes) {
        WorldShotMesh t;
        t.color[0] = m.color[0];
        t.color[1] = m.color[1];
        t.color[2] = m.color[2];
        t.tris = m.tris;
        t.pos.resize(m.pos.size());
        t.nrm.resize(m.nrm.size());
        t.uv = m.uv;
        t.triImg.resize(m.triImg.size());
        t.triCol = m.triCol;
        for (size_t i = 0; i < m.triImg.size(); ++i) {
            int li = m.triImg[i];
            t.triImg[i] = (li >= 0) ? li + imgOff : li;
        }
        size_t n = m.pos.size() / 3;
        for (size_t i = 0; i < n; ++i) {
            float xl = m.pos[i * 3];
            float yl = m.pos[i * 3 + 1];
            float zl = m.pos[i * 3 + 2];
            float xw = static_cast<float>(pedX) + xl * c - yl * s;
            float yw = static_cast<float>(pedY) + xl * s + yl * c;
            float zw = static_cast<float>(pedZtrans) + zl;
            t.pos[i * 3] = xw;
            t.pos[i * 3 + 1] = yw;
            t.pos[i * 3 + 2] = zw;
            float nx = m.nrm[i * 3];
            float ny = m.nrm[i * 3 + 1];
            float nz = m.nrm[i * 3 + 2];
            t.nrm[i * 3] = nx * c - ny * s;
            t.nrm[i * 3 + 1] = nx * s + ny * c;
            t.nrm[i * 3 + 2] = nz;
            grow(xw, yw, zw);
        }
        out.meshes.push_back(std::move(t));
    }
    int tris = 0;
    for (const auto& m : out.meshes) {
        tris += m.tris;
    }
    (void)std::snprintf(out.stats.dffName, sizeof(out.stats.dffName), "walk:%d+%d",
                         static_cast<int>(world.meshes.size()), static_cast<int>(ped.meshes.size()));
    (void)std::snprintf(out.stats.txdName, sizeof(out.stats.txdName), "multi:%d",
                         static_cast<int>(out.images.size()));
    out.stats.atomics = static_cast<int>(out.meshes.size());
    out.stats.triangles = tris;
    out.stats.vertices = tris * 3;
    out.stats.textures = static_cast<int>(out.images.size());
    out.stats.firstTexture[0] = '\0';
    out.stats.firstTexW = 0;
    out.stats.firstTexH = 0;
    if (!out.images.empty()) {
        (void)std::snprintf(out.stats.firstTexture, sizeof(out.stats.firstTexture), "%s",
                             out.images[0].name);
        out.stats.firstTexW = out.images[0].w;
        out.stats.firstTexH = out.images[0].h;
    }
}

void WalkSim_Chase(double pedX, double pedY, double groundH, double yawPathRad, double camD,
                   double camH, float eye[3], float target[3]) {
    const float fx = static_cast<float>(std::cos(yawPathRad));
    const float fy = static_cast<float>(std::sin(yawPathRad));
    eye[0] = static_cast<float>(pedX) - fx * static_cast<float>(camD);
    eye[1] = static_cast<float>(pedY) - fy * static_cast<float>(camD);
    eye[2] = static_cast<float>(groundH) + static_cast<float>(camH);
    target[0] = static_cast<float>(pedX);
    target[1] = static_cast<float>(pedY);
    target[2] = static_cast<float>(groundH) + 1.0f;
}
