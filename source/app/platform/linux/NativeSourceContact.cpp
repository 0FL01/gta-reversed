#include "NativeSourceContact.h"
#include "NativeSourceGround.h"
#include <algorithm>
#include <cmath>

namespace {
using Vector = NativeCollisionVector;
using Status = NativeSourceContactStatus;
bool Finite(const Vector& v) {
    return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
}
bool Valid(const NativeSourceContactSphere& s) {
    return Finite(s.Center) && std::isfinite(s.Radius) && s.Radius >= 0;
}
// Detect non-representable intermediate arithmetic before a comparison can
// turn overflow into a false Miss. Operations/order remain source float.
struct Math {
    bool Valid = true;
    float Value(float v) { Valid = Valid && std::isfinite(v); return v; }
    Vector Sub(const Vector& a, const Vector& b) {
        return {Value(a[0] - b[0]), Value(a[1] - b[1]), Value(a[2] - b[2])};
    }
    Vector Add(const Vector& a, const Vector& b) {
        return {Value(a[0] + b[0]), Value(a[1] + b[1]), Value(a[2] + b[2])};
    }
    Vector Scale(const Vector& a, float f) { return {Value(a[0] * f), Value(a[1] * f), Value(a[2] * f)}; }
    float Dot(const Vector& a, const Vector& b) {
        const float x = Value(a[0] * b[0]), y = Value(a[1] * b[1]), z = Value(a[2] * b[2]);
        return Value(Value(z + y) + x); // CVector::Dot -> DotProduct, NOT SquaredMagnitude order
    }
    float Squared(const Vector& a) {
        const float x = Value(a[0] * a[0]), y = Value(a[1] * a[1]), z = Value(a[2] * a[2]);
        return Value(Value(x + y) + z);
    }
    float Determinant(float a, float b, float c, float d) {
        const float first = Value(a * b), second = Value(c * d);
        return Value(first - second);
    }
    Vector Cross(const Vector& a, const Vector& b) {
        return {Determinant(a[1], b[2], a[2], b[1]), Determinant(a[2], b[0], a[0], b[2]), Determinant(a[0], b[1], a[1], b[0])};
    }
};
// Same expression/branch order as Collision.cpp::ClosestPtPointTriangle.
// Original helper credits Christer Ericson, Real-Time Collision Detection,
// Morgan Kaufmann, copyright 2005 Elsevier Inc.
Vector Closest(Math& m, const std::array<Vector, 3>& vertices, const Vector& p) {
    const auto& a = vertices[0]; const auto& b = vertices[1]; const auto& c = vertices[2];
    const auto ab = m.Sub(b, a), ac = m.Sub(c, a), ap = m.Sub(p, a);
    const float d1 = m.Dot(ab, ap), d2 = m.Dot(ac, ap);
    if (d1 <= 0 && d2 <= 0) return a;
    const auto bp = m.Sub(p, b);
    const float d3 = m.Dot(ab, bp), d4 = m.Dot(ac, bp);
    if (d3 >= 0 && d4 <= d3) return b;
    const float vc = m.Determinant(d1, d4, d3, d2);
    if (vc <= 0 && d1 >= 0 && d3 <= 0) return m.Add(a, m.Scale(ab, m.Value(d1 / m.Value(d1 - d3))));
    const auto cp = m.Sub(p, c);
    const float d5 = m.Dot(ab, cp), d6 = m.Dot(ac, cp);
    if (d6 >= 0 && d5 <= d6) return c;
    const float vb = m.Determinant(d5, d2, d1, d6);
    if (vb <= 0 && d2 >= 0 && d6 <= 0) return m.Add(a, m.Scale(ac, m.Value(d2 / m.Value(d2 - d6))));
    const float va = m.Determinant(d3, d6, d5, d4);
    const float d43 = m.Value(d4 - d3), d56 = m.Value(d5 - d6);
    if (va <= 0 && d43 >= 0 && d56 >= 0) return m.Add(b, m.Scale(m.Sub(c, b), m.Value(d43 / m.Value(d43 + d56))));
    const float denominator = m.Value(m.Value(va + vb) + vc);
    const float v = m.Value(vb / denominator), w = m.Value(vc / denominator);
    return m.Add(m.Add(a, m.Scale(ab, v)), m.Scale(ac, w));
}
}

NativeSourceContactStatus NativeSourceSphereSphere(const NativeSourceContactSphere& a,
    const NativeSourceContactSphere& b, float& maximum, NativeSourceContactPoint& out) {
    if (!Valid(a) || !Valid(b) || !std::isfinite(maximum) || maximum < 0) return Status::InvalidInput;
    Math m;
    const auto difference = m.Sub(a.Center, b.Center);
    const float squared = m.Squared(difference);
    const float sum = m.Value(a.Radius + b.Radius), radiiSquared = m.Value(sum * sum);
    if (!m.Valid) return Status::Overflow;
    if (squared >= radiiSquared) return Status::Miss;
    const float distance = std::sqrt(squared);
    const float unclamped = m.Value(distance - b.Radius), touch = std::max(unclamped, 0.0f);
    const float touchSquared = m.Value(touch * touch);
    if (!m.Valid) return Status::Overflow;
    if (touchSquared >= maximum || touch >= a.Radius) return Status::Miss;
    // CVector::NormaliseAndMag only replaces x on squared-length underflow;
    // y/z retain their values (exact coincident centers give (1,0,0)).
    Vector normal = difference;
    normal[0] = 1;
    if (squared > 0) {
        const double reciprocal = 1.0 / std::sqrt(double(squared));
        for (std::size_t i = 0; i < 3; ++i) normal[i] = m.Value(float(double(difference[i]) * reciprocal));
    }
    NativeSourceContactPoint candidate;
    candidate.Normal = normal;
    candidate.Point = m.Sub(a.Center, m.Scale(normal, touch));
    candidate.Depth = m.Value(a.Radius - unclamped);
    candidate.SurfaceA = a.Surface; candidate.SurfaceB = b.Surface;
    if (!m.Valid) return Status::Overflow;
    out = candidate; maximum = touchSquared;
    return Status::Hit;
}
NativeSourceContactStatus NativeSourceSphereTriangle(const NativeSourceContactSphere& sphere,
    const NativeSourceContactTriangle& triangle, float& maximum, NativeSourceContactPoint& out) {
    if (!Valid(sphere) || !std::isfinite(maximum) || maximum < 0) return Status::InvalidInput;
    for (const auto& v : triangle.Vertices) if (!Finite(v)) return Status::InvalidInput;
    Math m;
    const auto area = m.Cross(m.Sub(triangle.Vertices[1], triangle.Vertices[0]), m.Sub(triangle.Vertices[2], triangle.Vertices[0]));
    if (!m.Valid) return Status::Overflow;
    if (area == Vector{}) return Status::Unsupported;
    const auto point = Closest(m, triangle.Vertices, sphere.Center);
    const auto difference = m.Sub(sphere.Center, point);
    const float squared = m.Squared(difference), radiusSquared = m.Value(sphere.Radius * sphere.Radius);
    if (!m.Valid) return Status::Overflow;
    if (squared >= maximum || squared >= radiusSquared || squared <= 0) return Status::Miss;
    const float distance = std::sqrt(squared);
    NativeSourceContactPoint candidate;
    for (std::size_t i = 0; i < 3; ++i) candidate.Normal[i] = m.Value(difference[i] / distance);
    candidate.Point = point; candidate.Depth = m.Value(sphere.Radius - distance);
    candidate.SurfaceA = sphere.Surface;
    candidate.SurfaceB = {triangle.Material, 0, triangle.Lighting};
    if (!m.Valid) return Status::Overflow;
    out = candidate; maximum = squared;
    return Status::Hit;
}
NativeSourceContactStatus NativeSourceSphereBox(const NativeSourceContactSphere& sphere,
    const NativeSourceContactBox& box, float& maximum, NativeSourceContactPoint& out) {
    if (!Valid(sphere) || !Finite(box.Min) || !Finite(box.Max) || !std::isfinite(maximum) || maximum < 0) return Status::InvalidInput;
    for (std::size_t i = 0; i < 3; ++i) if (box.Min[i] > box.Max[i]) return Status::InvalidInput;
    Math m;
    bool inside = true;
    Vector closest{}, delta{}, faceDistance{};
    for (std::size_t i = 0; i < 3; ++i) {
        const float upper = m.Value(sphere.Center[i] + sphere.Radius), lower = m.Value(sphere.Center[i] - sphere.Radius);
        if (!m.Valid) return Status::Overflow;
        if (upper < box.Min[i] || lower > box.Max[i]) return Status::Miss;
        closest[i] = std::clamp(sphere.Center[i], box.Min[i], box.Max[i]);
        inside = inside && sphere.Center[i] >= box.Min[i] && sphere.Center[i] <= box.Max[i];
    }
    auto candidate = out; // source leaves both piece fields alone
    candidate.SurfaceA.Material = sphere.Surface.Material; candidate.SurfaceA.Lighting = sphere.Surface.Lighting;
    candidate.SurfaceB.Material = box.Surface.Material; candidate.SurfaceB.Lighting = box.Surface.Lighting;
    float nextMaximum = 0;
    if (inside) {
        for (std::size_t i = 0; i < 3; ++i) {
            const float center = m.Value(m.Value(box.Min[i] + box.Max[i]) * 0.5f);
            delta[i] = m.Value(sphere.Center[i] - center);
            faceDistance[i] = delta[i] <= 0 ? m.Value(sphere.Center[i] - box.Min[i]) : m.Value(box.Max[i] - sphere.Center[i]);
        }
        if (!m.Valid) return Status::Overflow;
        // Retail helper 0x410850: x only if y>x AND z>x (0x410925,
        // 0x41092e); y only if x>y AND y<z (0x410965, 0x41096e).
        // Every tie falls through to z, even when x=y<z. The upstream
        // implementation matches these branches; its tie comment does not.
        const std::size_t axis = faceDistance[0] < faceDistance[1] && faceDistance[0] < faceDistance[2] ? 0 :
            faceDistance[1] < faceDistance[0] && faceDistance[1] < faceDistance[2] ? 1 : 2;
        candidate.Normal = {};
        candidate.Normal[axis] = delta[axis] <= 0 ? -1.0f : 1.0f;
        candidate.Point = m.Sub(sphere.Center, m.Scale(candidate.Normal, sphere.Radius));
        candidate.Depth = m.Value(faceDistance[axis] + sphere.Radius);
    } else {
        const auto difference = m.Sub(sphere.Center, closest);
        const float squared = m.Squared(difference);
        if (!m.Valid) return Status::Overflow;
        if (squared >= maximum) return Status::Miss;
        const float distance = std::sqrt(squared);
        if (distance >= sphere.Radius) return Status::Miss;
        if (distance == 0) return Status::Unsupported; // subnormal underflow cannot produce NaNs
        for (std::size_t i = 0; i < 3; ++i) candidate.Normal[i] = m.Value(difference[i] / distance);
        candidate.Point = closest;
        candidate.Depth = m.Value(sphere.Radius - distance);
        nextMaximum = squared;
    }
    if (!m.Valid) return Status::Overflow;
    out = candidate; maximum = nextMaximum;
    return Status::Hit;
}
NativeSourceContactStatus NativeSourceLineTriangle(Vector start, Vector end,
    const NativeSourceContactTriangle& triangle, float& maximum, NativeSourceContactPoint& out) {
    if (!Finite(start) || !Finite(end) || !std::isfinite(maximum) || maximum < 0 || maximum > 1) return Status::InvalidInput;
    for (const auto& vertex : triangle.Vertices) {
        if (!Finite(vertex)) return Status::InvalidInput;
        for (float component : vertex) {
            const float packed = component * 128.0f;
            if (!std::isfinite(packed) || packed < -32768 || packed > 32767 || std::trunc(packed) != packed) return Status::Unsupported;
        }
    }
    auto a = triangle.Vertices[0], b = triangle.Vertices[1], c = triangle.Vertices[2];
    Math m;
    if (m.Cross(m.Sub(c, a), m.Sub(b, a)) == Vector{}) return Status::Unsupported;
    NativeSourceGroundPlane plane;
    if (!m.Valid) return Status::Overflow;
    if (!NativeSourceGround::CalculatePlane(a, b, c, plane)) return Status::Unsupported;
    Vector normal;
    for (std::size_t i = 0; i < 3; ++i) normal[i] = float(plane.Normal[i]) / 4096.0f;
    const float offset = float(plane.Distance) / 128.0f;
    const float origin = m.Value(m.Dot(start, normal) - offset), destination = m.Value(m.Dot(end, normal) - offset);
    if (!m.Valid) return Status::Overflow;
    if (std::signbit(origin) == std::signbit(destination)) return Status::Miss;
    const float magnitude = -m.Dot(m.Sub(end, start), normal);
    if (!m.Valid) return Status::Overflow;
    if (magnitude == 0) return Status::Unsupported;
    const float fraction = m.Value(origin / magnitude);
    if (!m.Valid) return Status::Overflow;
    if (fraction >= maximum) return Status::Miss;
    const auto point = m.Add(m.Scale(end, fraction), m.Scale(start, m.Value(1.0f - fraction)));
    const auto orientation = static_cast<unsigned>(plane.Direction);
    const auto axis = orientation / 2, u = (axis + 1) % 3, v = (axis + 2) % 3;
    if (orientation % 2 == 0) std::swap(b, c);
    const auto edge = [&](Vector x, Vector y, Vector relative) {
        return m.Determinant(m.Value(y[u] - x[u]), relative[v], m.Value(y[v] - x[v]), relative[u]);
    };
    const auto relative = m.Sub(point, a);
    const float ab = edge(a, b, relative), ac = edge(a, c, relative), bc = edge(b, c, m.Sub(point, b));
    if (!m.Valid) return Status::Overflow;
    if (!(ab >= 0 && ac <= 0 && bc >= 0)) return Status::Miss;
    auto candidate = out;
    candidate.Point = point; candidate.Normal = normal;
    candidate.SurfaceA.Material = candidate.SurfaceA.Piece = 0;
    candidate.SurfaceB = {triangle.Material, 0, triangle.Lighting};
    out = candidate; maximum = fraction;
    return Status::Hit;
}
NativeSourceContactStatus NativeSourceLineSphere(Vector start, Vector end,
    const NativeSourceContactSphere& sphere, float& maximum, NativeSourceContactPoint& out) {
    if (!Finite(start) || !Finite(end) || !Valid(sphere) || !std::isfinite(maximum) || maximum < 0 || maximum > 1) return Status::InvalidInput;
    Math m;
    const auto d = m.Sub(end, start), relative = m.Sub(start, sphere.Center);
    const float relativeSquared = m.Dot(relative, relative), radiusSquared = m.Value(sphere.Radius * sphere.Radius);
    const float c = m.Value(relativeSquared - radiusSquared), a = m.Dot(d, d);
    if (!m.Valid) return Status::Overflow;
    if (c <= 0) return Status::Miss;
    const float b = m.Dot(relative, d);
    if (!m.Valid) return Status::Overflow;
    if (b > 0) return Status::Miss;
    const float discriminant = m.Determinant(b, b, a, c);
    if (!m.Valid) return Status::Overflow;
    if (discriminant < 0 || a <= 0) return Status::Miss;
    const float fraction = m.Value(m.Value(-b - std::sqrt(discriminant)) / a);
    if (!m.Valid) return Status::Overflow;
    if (fraction > 1 || fraction >= maximum) return Status::Miss;
    auto candidate = out;
    candidate.Point = m.Add(start, m.Scale(d, fraction));
    candidate.Normal = m.Sub(candidate.Point, sphere.Center);
    const float squared = m.Squared(candidate.Normal);
    if (!m.Valid) return Status::Overflow;
    if (squared <= 0) candidate.Normal[0] = 1;
    else {
        const double reciprocal = 1.0 / std::sqrt(double(squared));
        for (auto& component : candidate.Normal) component = m.Value(float(double(component) * reciprocal));
    }
    candidate.SurfaceA.Material = candidate.SurfaceA.Lighting = 0;
    candidate.SurfaceB.Material = sphere.Surface.Material; candidate.SurfaceB.Lighting = sphere.Surface.Lighting;
    if (!m.Valid) return Status::Overflow;
    out = candidate; maximum = fraction;
    return Status::Hit;
}
NativeSourceContactStatus NativeSourceLineBox(Vector start, Vector end,
    const NativeSourceContactBox& box, float& maximum, NativeSourceContactPoint& out) {
    if (!Finite(start) || !Finite(end) || !Finite(box.Min) || !Finite(box.Max) ||
        !std::isfinite(maximum) || maximum < 0 || maximum > 1) return Status::InvalidInput;
    for (std::size_t i = 0; i < 3; ++i) if (box.Min[i] > box.Max[i]) return Status::InvalidInput;
    Math m;
    float nearest = 1;
    auto candidate = out;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        const std::size_t u = axis == 0 ? 1 : 0, v = axis == 2 ? 1 : 2;
        for (const bool upper : {false, true}) {
            const float face = upper ? box.Max[axis] : box.Min[axis];
            const float origin = upper ? m.Value(start[axis] - face) : m.Value(face - start[axis]);
            const float destination = upper ? m.Value(end[axis] - face) : m.Value(face - end[axis]);
            const float crossing = m.Value(origin * destination);
            if (!m.Valid) return Status::Overflow;
            if (crossing >= 0) continue;
            const float denominator = upper ? m.Value(start[axis] - end[axis]) : m.Value(end[axis] - start[axis]);
            const float fraction = m.Value(origin / denominator);
            const float first = m.Value(start[u] + m.Value(m.Value(end[u] - start[u]) * fraction));
            if (!m.Valid) return Status::Overflow;
            if (!(first > box.Min[u] && first < box.Max[u])) continue;
            const float second = m.Value(start[v] + m.Value(m.Value(end[v] - start[v]) * fraction));
            if (!m.Valid) return Status::Overflow;
            if (!(second > box.Min[v] && second < box.Max[v]) || fraction >= nearest) continue;
            nearest = fraction;
            candidate.Point[axis] = face; candidate.Point[u] = first; candidate.Point[v] = second;
            candidate.Normal = {}; candidate.Normal[axis] = upper ? 1.0f : -1.0f;
        }
    }
    if (nearest >= maximum) return Status::Miss;
    candidate.SurfaceA.Material = candidate.SurfaceA.Lighting = 0;
    candidate.SurfaceB.Material = box.Surface.Material; candidate.SurfaceB.Lighting = box.Surface.Lighting;
    out = candidate; maximum = nearest;
    return Status::Hit;
}
NativeSourceContactStatus NativeSourceTestSphereTriangle(const NativeSourceContactSphere& sphere,
    const NativeSourceContactTriangle& triangle) {
    if (!Valid(sphere)) return Status::InvalidInput;
    for (const auto& vertex : triangle.Vertices) {
        if (!Finite(vertex)) return Status::InvalidInput;
        for (float component : vertex) {
            const float packed = component * 128.0f;
            if (!std::isfinite(packed) || packed < -32768 || packed > 32767 || std::trunc(packed) != packed) return Status::Unsupported;
        }
    }
    Math m;
    const auto& vertices = triangle.Vertices;
    const auto cross = m.Cross(m.Sub(vertices[2], vertices[0]), m.Sub(vertices[1], vertices[0]));
    if (!m.Valid) return Status::Overflow;
    if (cross == Vector{}) return Status::Unsupported;
    NativeSourceGroundPlane plane;
    if (!NativeSourceGround::CalculatePlane(vertices[0], vertices[1], vertices[2], plane)) return Status::Unsupported;
    Vector normal;
    for (std::size_t i = 0; i < 3; ++i) normal[i] = float(plane.Normal[i]) / 4096;
    const auto a = m.Sub(vertices[0], sphere.Center), b = m.Sub(vertices[1], sphere.Center), c = m.Sub(vertices[2], sphere.Center);
    const float rr = m.Value(sphere.Radius * sphere.Radius);
    const bool s1 = std::abs(m.Dot(a, normal)) > sphere.Radius;
    const float aa = m.Dot(a, a), ab = m.Dot(a, b), ac = m.Dot(a, c);
    const float bb = m.Dot(b, b), bc = m.Dot(b, c), cc = m.Dot(c, c);
    const bool s2 = (aa > rr) & (ab > aa) & (ac > aa);
    const bool s3 = (bb > rr) & (ab > bb) & (bc > bb);
    const bool s4 = (cc > rr) & (ac > cc) & (bc > cc);
    const auto edgeAB = m.Sub(b, a), edgeBC = m.Sub(c, b), edgeCA = m.Sub(a, c);
    const float d1 = m.Value(ab - aa), d2 = m.Value(bc - bb), d3 = m.Value(ac - cc);
    const float e1 = m.Dot(edgeAB, edgeAB), e2 = m.Dot(edgeBC, edgeBC), e3 = m.Dot(edgeCA, edgeCA);
    const auto q1 = m.Sub(m.Scale(a, e1), m.Scale(edgeAB, d1));
    const auto q2 = m.Sub(m.Scale(b, e2), m.Scale(edgeBC, d2));
    const auto q3 = m.Sub(m.Scale(c, e3), m.Scale(edgeCA, d3));
    const auto qc = m.Sub(m.Scale(c, e1), q1), qa = m.Sub(m.Scale(a, e2), q2), qb = m.Sub(m.Scale(b, e3), q3);
    const auto separated = [&](const Vector& q, const Vector& other, float e) {
        const float squared = m.Dot(q, q), bound = m.Value(m.Value(rr * e) * e), projection = m.Dot(q, other);
        return (squared > bound) & (projection > 0);
    };
    const bool s5 = separated(q1, qc, e1);
    const bool s6 = separated(q2, qa, e2);
    const bool s7 = separated(q3, qb, e3);
    if (!m.Valid) return Status::Overflow;
    return (s1 | s2 | s3 | s4 | s5 | s6 | s7) ? Status::Miss : Status::Hit;
}
