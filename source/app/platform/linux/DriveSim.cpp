// DriveSim implementation: kinematic composition (R6n).
// Owns no engine itself; reuses the verified slice entry points. Engine
// sharing: StreamPager brings the librw engine up first, CarPose reuses it
// through its tolerant RwInitEngine (identical plugin set).

#include "app/platform/linux/DriveSim.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "app/platform/linux/CarPose.h"
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

bool DriveSim_Measure(const char* gameDir, const char* model, DriveMeasure& out, char* err,
                      std::size_t errSize) {
    out = DriveMeasure{};
    CarPoseMeasure m{};
    if (!CarPose_Measure(gameDir, model, m, err, errSize)) {
        return false;
    }
    (void)std::snprintf(out.model, sizeof(out.model), "%s", m.model);
    (void)std::snprintf(out.src, sizeof(out.src), "%s", m.src);
    out.wheelR = m.wheelR;
    out.wheelbase = m.wheelbase;
    out.clearance = m.clearance;
    out.frontY = m.frontY;
    out.rearY = m.rearY;
    out.wheels = m.wheels;
    return true;
}

bool DriveSim_InitWorld(const char* gameDir, E2ELoadInfo& load, char* err, std::size_t errSize) {
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

void DriveSim_ShutdownWorld() {
    StreamPager_Shutdown();
    ColLoad_Shutdown();
    CarPose_Shutdown();
}

bool DriveSim_Ground(double x, double y, double& hOut, char* modelOut, std::size_t modelSize,
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

bool DriveSim_Car(const char* gameDir, const char* model, double steerDeg, double spinDeg,
                  WorldShotScene& carScene, char* err, std::size_t errSize) {
    carScene.meshes.clear();
    carScene.images.clear();
    CarPoseStats st{};
    CarPoseAudit au{};
    return CarPose_Init(gameDir, model, steerDeg, spinDeg, carScene, st, au, err, errSize);
}

bool DriveSim_Page(double x, double y, double z, WorldShotScene& worldScene, E2EPagerFrame& pf,
                   char* err, std::size_t errSize) {
    return StreamPager_Update(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z),
                              worldScene, pf, err, errSize);
}

bool DriveSim_Sample(const std::vector<std::pair<double, double>>& ctrl, int waypoints,
                     double wheelbase, double wheelR, std::vector<DriveWaypoint>& out, char* err,
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
    if (!(wheelbase > 0.0 && wheelR > 0.0)) {
        return fail("bad wheelbase/wheelR");
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
    // Segment yaws (P-1 values).
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
    out.reserve(static_cast<size_t>(waypoints));
    if (static_cast<int>(p) == waypoints) {
        for (int i = 0; i < waypoints; ++i) {
            DriveWaypoint w;
            w.x = ctrl[static_cast<size_t>(i)].first;
            w.y = ctrl[static_cast<size_t>(i)].second;
            w.dist = cum[static_cast<size_t>(i)];
            double yaw = (i < static_cast<int>(segYaw.size())) ? segYaw[static_cast<size_t>(i)]
                                                               : segYaw.back();
            w.yawPath = yaw;
            w.yawBody = yaw - kPi * 0.5; // local +Y forward -> path dir
            double steer = 0.0;
            if (i > 0) {
                double dyaw = WrapPi(w.yawPath - out[static_cast<size_t>(i - 1)].yawPath);
                double ds = w.dist - out[static_cast<size_t>(i - 1)].dist;
                if (ds > 1e-9) {
                    steer = std::atan(wheelbase * dyaw / ds);
                }
            }
            w.steerDeg = steer * 180.0 / kPi;
            w.spinRad = w.dist / wheelR;
            w.spinDeg = w.spinRad * 180.0 / kPi;
            out.push_back(w);
        }
        return true;
    }
    // Uniform arc-length resample to W points.
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
        DriveWaypoint w;
        w.x = ctrl[seg].first + (ctrl[seg + 1].first - ctrl[seg].first) * t;
        w.y = ctrl[seg].second + (ctrl[seg + 1].second - ctrl[seg].second) * t;
        w.dist = s;
        double yaw = segYaw[seg];
        w.yawPath = yaw;
        w.yawBody = yaw - kPi * 0.5;
        double steer = 0.0;
        if (j > 0) {
            double dyaw = WrapPi(w.yawPath - out[static_cast<size_t>(j - 1)].yawPath);
            double ds = w.dist - out[static_cast<size_t>(j - 1)].dist;
            if (ds > 1e-9) {
                steer = std::atan(wheelbase * dyaw / ds);
            }
        }
        w.steerDeg = steer * 180.0 / kPi;
        w.spinRad = w.dist / wheelR;
        w.spinDeg = w.spinRad * 180.0 / kPi;
        out.push_back(w);
    }
    return true;
}

double DriveSim_MaxTurnDeg(const std::vector<std::pair<double, double>>& ctrl) {
    if (ctrl.size() < 3) {
        return 0.0;
    }
    double mx = 0.0;
    for (size_t i = 1; i + 1 < ctrl.size(); ++i) {
        double ax = ctrl[i].first - ctrl[i - 1].first;
        double ay = ctrl[i].second - ctrl[i - 1].second;
        double bx = ctrl[i + 1].first - ctrl[i].first;
        double by = ctrl[i + 1].second - ctrl[i].second;
        double la = std::sqrt(ax * ax + ay * ay);
        double lb = std::sqrt(bx * bx + by * by);
        if (la < 1e-9 || lb < 1e-9) {
            continue;
        }
        double ya = std::atan2(ay, ax);
        double yb = std::atan2(by, bx);
        double d = std::fabs(WrapPi(yb - ya)) * 180.0 / kPi;
        if (d > mx) {
            mx = d;
        }
    }
    return mx;
}

void DriveSim_Merge(const WorldShotScene& world, const WorldShotScene& car, double carX,
                    double carY, double carZ, double yawBodyRad, WorldShotScene& out) {
    out.meshes.clear();
    out.images.clear();
    out.images.reserve(world.images.size() + car.images.size());
    for (const auto& im : world.images) {
        out.images.push_back(im);
    }
    const int imgOff = static_cast<int>(world.images.size());
    for (const auto& im : car.images) {
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
    // World first (deterministic pager order preserved).
    for (const auto& m : world.meshes) {
        out.meshes.push_back(m);
        size_t n = m.pos.size() / 3;
        for (size_t i = 0; i < n; ++i) {
            grow(m.pos[i * 3], m.pos[i * 3 + 1], m.pos[i * 3 + 2]);
        }
    }
    const float c = static_cast<float>(std::cos(yawBodyRad));
    const float s = static_cast<float>(std::sin(yawBodyRad));
    for (const auto& m : car.meshes) {
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
            float xw = static_cast<float>(carX) + xl * c - yl * s;
            float yw = static_cast<float>(carY) + xl * s + yl * c;
            float zw = static_cast<float>(carZ) + zl;
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
    (void)std::snprintf(out.stats.dffName, sizeof(out.stats.dffName), "drive:%d+%d",
                         static_cast<int>(world.meshes.size()), static_cast<int>(car.meshes.size()));
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

void DriveSim_Chase(double carX, double carY, double carZ, double yawPathRad, double camD,
                    double camH, float eye[3], float target[3]) {
    const float fx = static_cast<float>(std::cos(yawPathRad));
    const float fy = static_cast<float>(std::sin(yawPathRad));
    eye[0] = static_cast<float>(carX) - fx * static_cast<float>(camD);
    eye[1] = static_cast<float>(carY) - fy * static_cast<float>(camD);
    eye[2] = static_cast<float>(carZ) + static_cast<float>(camH);
    target[0] = static_cast<float>(carX);
    target[1] = static_cast<float>(carY);
    target[2] = static_cast<float>(carZ) + 0.3f;
}
