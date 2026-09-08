// Collide implementation: branchless-friendly raycast primitives.
// See Collide.h for the contract. Float only; callers validate finiteness.

#include "app/platform/linux/Collide.h"

#include <cmath>

namespace Collide {

bool RaySphere(const float* o, const float* d, const float* c, float r, float& tOut) {
    if (!(r > 0.0f) || !std::isfinite(r)) {
        return false;
    }
    const float ox = o[0] - c[0];
    const float oy = o[1] - c[1];
    const float oz = o[2] - c[2];
    // Direction is unit, so a == 1.
    const float b = ox * d[0] + oy * d[1] + oz * d[2];
    const float cc = ox * ox + oy * oy + oz * oz - r * r;
    const float disc = b * b - cc;
    if (!(disc >= 0.0f) || !std::isfinite(disc)) {
        return false;
    }
    const float t = -b - std::sqrt(disc);
    if (!(t >= 0.0f) || !std::isfinite(t)) {
        return false;
    }
    tOut = t;
    return true;
}

bool RayBox(const float* o, const float* d, const float* bmin, const float* bmax, float& tOut) {
    float tEnter = 0.0f;
    float tExit = 0.0f;
    bool have = false;
    for (int i = 0; i < 3; ++i) {
        const float origin = o[i];
        const float dir = d[i];
        const float lo = bmin[i];
        const float hi = bmax[i];
        if (!(hi >= lo) || !std::isfinite(origin) || !std::isfinite(dir)) {
            return false;
        }
        if (dir > -1e-9f && dir < 1e-9f) {
            // Parallel: origin must sit inside the slab.
            if (origin < lo || origin > hi) {
                return false;
            }
            continue;
        }
        float t0 = (lo - origin) / dir;
        float t1 = (hi - origin) / dir;
        if (t0 > t1) {
            const float tmp = t0;
            t0 = t1;
            t1 = tmp;
        }
        if (!have) {
            tEnter = t0;
            tExit = t1;
            have = true;
        } else {
            if (t0 > tEnter) {
                tEnter = t0;
            }
            if (t1 < tExit) {
                tExit = t1;
            }
        }
        if (tEnter > tExit) {
            return false;
        }
    }
    if (!have) {
        return false;
    }
    // Origin inside the box: the top cap is above the ray start; a downward
    // ground probe never starts buried, so this is not a ground hit.
    if (tEnter < 0.0f || !std::isfinite(tEnter)) {
        return false;
    }
    tOut = tEnter;
    return true;
}

bool RayTri(const float* o, const float* d, const float* a, const float* b, const float* c,
            float& tOut) {
    const float e1x = b[0] - a[0];
    const float e1y = b[1] - a[1];
    const float e1z = b[2] - a[2];
    const float e2x = c[0] - a[0];
    const float e2y = c[1] - a[1];
    const float e2z = c[2] - a[2];
    // p = d x e2
    const float px = d[1] * e2z - d[2] * e2y;
    const float py = d[2] * e2x - d[0] * e2z;
    const float pz = d[0] * e2y - d[1] * e2x;
    float det = e1x * px + e1y * py + e1z * pz;
    if (det > -1e-9f && det < 1e-9f) {
        return false; // parallel
    }
    // Double-sided: flip the determinant side instead of culling.
    const float sign = det < 0.0f ? -1.0f : 1.0f;
    det *= sign;
    const float tx = o[0] - a[0];
    const float ty = o[1] - a[1];
    const float tz = o[2] - a[2];
    const float u = (tx * px + ty * py + tz * pz) * sign;
    if (u < 0.0f || u > det) {
        return false;
    }
    // q = tvec x e1
    const float qx = (ty * e1z - tz * e1y) * sign;
    const float qy = (tz * e1x - tx * e1z) * sign;
    const float qz = (tx * e1y - ty * e1x) * sign;
    const float v = d[0] * qx + d[1] * qy + d[2] * qz;
    if (v < 0.0f || u + v > det) {
        return false;
    }
    const float t = (e2x * qx + e2y * qy + e2z * qz) / det;
    if (!(t >= 0.0f) || !std::isfinite(t)) {
        return false;
    }
    tOut = t;
    return true;
}

} // namespace Collide
