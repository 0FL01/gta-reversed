// Collide: vertical-ray ground queries over SA collision bytes (R6g).
// Pure math, no IO, no game_sa/ linkage. The caller (ColLoad) owns the
// parsed COL models and instance transforms; here are only the raycast
// primitives: ray vs sphere / axis-aligned box / triangle, all in the
// model's local frame with a unit direction. Every hit returns the ray
// parameter t (>= 0); world height is recovered by the caller as
// z = rayTop - t (the ray always points straight down).
#pragma once

namespace Collide {

// Ray vs sphere: |o + t*d - c|^2 = r^2. Nearest t >= 0, false on miss.
bool RaySphere(const float* o, const float* d, const float* c, float r, float& tOut);

// Ray vs axis-aligned box (slab test). Entry t >= 0, false on miss or when
// the origin starts inside (the top cap would sit above the ray origin,
// which cannot happen for a downward ground probe).
bool RayBox(const float* o, const float* d, const float* bmin, const float* bmax, float& tOut);

// Ray vs triangle (Moller-Trumbore, double-sided: ground is hit from above
// but backfaces still count as solid). t >= 0, false on miss/parallel.
bool RayTri(const float* o, const float* d, const float* a, const float* b, const float* c,
            float& tOut);

} // namespace Collide
