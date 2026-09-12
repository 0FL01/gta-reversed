#include "NativeSourceModelContact.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace {
using Vec = NativeCollisionVector;
using Matrix = NativeSourceGroundTransform;
using Status = NativeSourceModelStatus;
using ContactStatus = NativeSourceContactStatus;

bool Finite(const Vec& v) { return std::ranges::all_of(v, [](float f) { return std::isfinite(f); }); }
bool Valid(const Matrix& m) { return Finite(m.Position) && std::ranges::all_of(m.Basis, Finite); }
bool Bounds(const Vec& min, const Vec& max) {
    return Finite(min) && Finite(max) && min[0] <= max[0] && min[1] <= max[1] && min[2] <= max[2];
}
bool Valid(const NativeSourceContactSphere& s) { return Finite(s.Center) && std::isfinite(s.Radius) && s.Radius >= 0; }
struct Math {
    bool Ok = true;
    float F(float value) { Ok &= std::isfinite(value); return value; }
    Vec Vector(const Matrix& m, const Vec& v) {
        Vec result;
        for (std::size_t i = 0; i < 3; ++i) {
            const auto x = F(m.Basis[0][i] * v[0]);
            const auto y = F(m.Basis[1][i] * v[1]);
            const auto z = F(m.Basis[2][i] * v[2]);
            result[i] = F(F(x + y) + z);
        }
        return result;
    }
    Vec Point(const Matrix& m, const Vec& v) {
        auto result = Vector(m, v);
        for (std::size_t i = 0; i < 3; ++i) result[i] = F(result[i] + m.Position[i]);
        return result;
    }
    Matrix Invert(const Matrix& in) {
        Matrix out;
        for (std::size_t i = 0; i < 3; ++i) for (std::size_t j = 0; j < 3; ++j) out.Basis[i][j] = in.Basis[j][i];
        out.Position = Vector(out, in.Position);
        for (auto& value : out.Position) value = -value;
        return out;
    }
    Matrix Multiply(const Matrix& a, const Matrix& b) {
        Matrix out;
        for (std::size_t i = 0; i < 3; ++i) out.Basis[i] = Vector(a, b.Basis[i]);
        out.Position = Point(a, b.Position);
        return out;
    }
    bool SphereBox(const NativeSourceContactSphere& s, const Vec& min, const Vec& max) {
        for (std::size_t i = 0; i < 3; ++i) {
            const auto high = F(s.Center[i] + s.Radius), low = F(s.Center[i] - s.Radius);
            if (high < min[i] || low > max[i]) return false;
        }
        return true;
    }
};
NativeSourceContactSurface Surface(const NativeCollisionSurface& s, std::uint32_t version) {
    return {s.Material, s.Flags, version == 1 ? s.Light : s.Brightness};
}
Status Failure(ContactStatus status) {
    if (status == ContactStatus::Overflow) return Status::Overflow;
    if (status == ContactStatus::InvalidInput) return Status::InvalidInput;
    return Status::Unsupported;
}
bool Accepted(ContactStatus status) { return status == ContactStatus::Hit || status == ContactStatus::Miss; }
Status Validate(const NativeCollisionModel& model) {
    if (!model.Unsupported.empty() || model.Version < 1 || model.Version > 4 || model.Spheres.size() > 128 ||
        model.Boxes.size() > 65535 || model.Faces.size() > 65535 || !Bounds(model.Min, model.Max) ||
        !Finite(model.BoundCenter) || !std::isfinite(model.BoundRadius) || model.BoundRadius < 0) return Status::Unsupported;
    for (const auto& sphere : model.Spheres)
        if (!Valid(NativeSourceContactSphere{sphere.Center, sphere.Radius, {}})) return Status::InvalidInput;
    for (const auto& box : model.Boxes) if (!Bounds(box.Min, box.Max)) return Status::InvalidInput;
    for (const auto& group : model.FaceGroups)
        if (!Bounds(group.Min, group.Max) || group.First > group.Last || group.Last >= model.Faces.size()) return Status::InvalidInput;
    if (!model.FaceGroups.empty() && (model.Version == 1 || !(model.Flags & 8))) return Status::Unsupported;
    return Status::Ok;
}
Status BindTriangles(const NativeCollisionModel& model, std::vector<NativeSourceContactTriangle>& triangles) {
    triangles.clear();
    triangles.reserve(model.Faces.size());
    for (const auto& face : model.Faces) {
        NativeSourceContactTriangle triangle;
        triangle.Material = face.Surface.Material;
        triangle.Lighting = face.Surface.Light;
        for (std::size_t i = 0; i < 3; ++i) {
            if (face.Vertices[i] >= model.Vertices.size()) return Status::InvalidInput;
            const auto& vertex = model.Vertices[face.Vertices[i]];
            for (std::size_t axis = 0; axis < 3; ++axis) {
                const float scaled = vertex[axis] * 128.0f;
                if (!std::isfinite(scaled) || scaled < -32768 || scaled >= 32768) return Status::Unsupported;
                if (model.Version != 1 && std::trunc(scaled) != scaled) return Status::Unsupported;
                triangle.Vertices[i][axis] = float(static_cast<std::int16_t>(scaled)) / 128.0f;
            }
        }
        NativeSourceGroundPlane plane;
        if (!NativeSourceGround::CalculatePlane(triangle.Vertices[0], triangle.Vertices[1], triangle.Vertices[2], plane))
            return Status::Unsupported;
        triangles.push_back(triangle);
    }
    return Status::Ok;
}
}

NativeSourceModelStatus NativeSourceProcessModels(const NativeCollisionModel& a, const Matrix& transformA,
    std::span<const NativeSourceModelLine> linesA, float effectiveBoundRadiusA,
    const NativeCollisionModel& b, const Matrix& transformB, bool returnAll,
    NativeSourceModelContacts& out) {
    if (!Valid(transformA) || !Valid(transformB) || linesA.size() > 16 ||
        !std::ranges::all_of(linesA, [](const auto& line) { return Finite(line.Start) && Finite(line.End); })) return Status::InvalidInput;
    const auto validA = Validate(a); if (validA != Status::Ok) return validA;
    const auto validB = Validate(b); if (validB != Status::Ok) return validB;
    if (!std::isfinite(effectiveBoundRadiusA) || effectiveBoundRadiusA < 0) return Status::InvalidInput;
    const float boundRadiusA = effectiveBoundRadiusA == 0 ? a.BoundRadius : effectiveBoundRadiusA;
    if (boundRadiusA < a.BoundRadius) return Status::InvalidInput;
    for (std::size_t i = 0; i < linesA.size(); ++i)
        if (!std::isfinite(out.LineFractions[i]) || out.LineFractions[i] < 0 || out.LineFractions[i] > 1) return Status::InvalidInput;

    NativeSourceModelContacts next = out;
    next.SphereCount = 0; next.LineCount = linesA.size(); next.LineHits = {};
    next.CandidateSpheresA = next.CandidateSpheresB = next.CandidateBoxesA = next.CandidateBoxesB = 0;
    next.CandidateTrianglesA = next.CandidateTrianglesB = 0;
    auto finish = [&] { out = next; return Status::Ok; };
    if (a.Empty || b.Empty) return finish();

    Math math;
    const auto aToB = math.Multiply(math.Invert(transformB), transformA);
    const auto bToA = math.Multiply(math.Invert(transformA), transformB);
    const NativeSourceContactSphere boundA{math.Point(aToB, a.BoundCenter), boundRadiusA, {}};
    if (!math.Ok) return Status::Overflow;
    if (!math.SphereBox(boundA, b.Min, b.Max)) return math.Ok ? finish() : Status::Overflow;

    std::array<NativeSourceContactSphere, 128> spheresA{}, spheresB{};
    std::array<std::size_t, 128> indicesA{}, indicesB{};
    for (std::size_t i = 0; i < a.Spheres.size(); ++i) {
        const auto& source = a.Spheres[i];
        spheresA[i] = {math.Point(aToB, source.Center), source.Radius, Surface(source.Surface, a.Version)};
        if (math.SphereBox(spheresA[i], b.Min, b.Max)) indicesA[next.CandidateSpheresA++] = i;
    }
    for (std::size_t i = 0; i < b.Spheres.size(); ++i) {
        const auto& source = b.Spheres[i];
        spheresB[i] = {math.Point(bToA, source.Center), source.Radius, Surface(source.Surface, b.Version)};
        if (math.SphereBox(spheresB[i], a.Min, a.Max)) indicesB[next.CandidateSpheresB++] = i;
    }
    if (!math.Ok) return Status::Overflow;
    if (!next.CandidateSpheresA && linesA.empty() && !next.CandidateSpheresB) return finish();

    std::array<std::size_t, 64> boxesB{};
    for (std::size_t i = 0; i < b.Boxes.size(); ++i) if (math.SphereBox(boundA, b.Boxes[i].Min, b.Boxes[i].Max)) {
        boxesB[next.CandidateBoxesB++] = i;
        if (next.CandidateBoxesB == boxesB.size()) break;
    }
    std::vector<NativeSourceContactTriangle> trianglesB;
    auto bound = BindTriangles(b, trianglesB); if (bound != Status::Ok) return bound;
    std::array<std::size_t, 600> triangleIndicesB{};
    auto candidateB = [&](std::size_t i) {
        const auto status = NativeSourceTestSphereTriangle(boundA, trianglesB[i]);
        if (!Accepted(status)) return Failure(status);
        if (status == ContactStatus::Hit) {
            if (next.CandidateTrianglesB == triangleIndicesB.size()) return Status::Unsupported;
            triangleIndicesB[next.CandidateTrianglesB++] = i;
        }
        return Status::Ok;
    };
    if (b.Flags & 8) {
        for (const auto& group : b.FaceGroups) if (math.SphereBox(boundA, group.Min, group.Max))
            for (std::size_t i = group.First; i <= group.Last; ++i) {
                const auto status = candidateB(i); if (status != Status::Ok) return status;
                if (next.CandidateTrianglesB == triangleIndicesB.size()) break;
            }
    } else for (std::size_t i = 0; i < trianglesB.size() && next.CandidateTrianglesB < triangleIndicesB.size(); ++i) {
        const auto status = candidateB(i); if (status != Status::Ok) return status;
    }
    if (!math.Ok) return Status::Overflow;
    if (!next.CandidateSpheresB && !next.CandidateBoxesB && !next.CandidateTrianglesB) return finish();

    next.Spheres[0].Depth = -1;
    for (std::size_t ai = 0; ai < next.CandidateSpheresA; ++ai) {
        const auto& sphere = spheresA[indicesA[ai]];
        float minimum = 1e24f; bool advance = false;
        for (std::size_t i = 0; i < next.CandidateSpheresB; ++i) {
            const auto& original = b.Spheres[indicesB[i]];
            const auto status = NativeSourceSphereSphere(sphere,
                {original.Center, original.Radius, Surface(original.Surface, b.Version)}, minimum, next.Spheres[next.SphereCount]);
            if (!Accepted(status)) return Failure(status);
            advance |= status == ContactStatus::Hit;
        }
        auto emit = [&](ContactStatus status) {
            if (status != ContactStatus::Hit) return;
            if (returnAll && sphere.Surface.Piece <= 2 && next.SphereCount < 31) {
                advance = false; minimum = 1e24f; next.Spheres[++next.SphereCount].Depth = -1;
            } else advance = true;
        };
        for (std::size_t i = 0; i < next.CandidateBoxesB; ++i) {
            const auto& box = b.Boxes[boxesB[i]];
            const auto status = NativeSourceSphereBox(sphere,
                {box.Min, box.Max, Surface(box.Surface, b.Version)}, minimum, next.Spheres[next.SphereCount]);
            if (!Accepted(status)) return Failure(status);
            emit(status);
        }
        for (std::size_t i = 0; i < next.CandidateTrianglesB; ++i) {
            const auto status = NativeSourceSphereTriangle(sphere, trianglesB[triangleIndicesB[i]], minimum, next.Spheres[next.SphereCount]);
            if (!Accepted(status)) return Failure(status);
            emit(status);
        }
        if (advance) {
            if (next.SphereCount >= 31) break;
            next.Spheres[++next.SphereCount].Depth = -1;
        }
    }
    for (std::size_t i = 0; i < next.SphereCount; ++i) {
        next.Spheres[i].Point = math.Point(transformB, next.Spheres[i].Point);
        next.Spheres[i].Normal = math.Vector(transformB, next.Spheres[i].Normal);
    }

    for (std::size_t line = 0; line < linesA.size(); ++line) {
        const auto start = math.Point(aToB, linesA[line].Start), end = math.Point(aToB, linesA[line].End);
        if (!math.Ok) return Status::Overflow;
        auto record = [&](ContactStatus status) { next.LineHits[line] |= status == ContactStatus::Hit; };
        for (std::size_t i = 0; i < next.CandidateSpheresB; ++i) {
            const auto& sphere = b.Spheres[indicesB[i]];
            const auto status = NativeSourceLineSphere(start, end,
                {sphere.Center, sphere.Radius, Surface(sphere.Surface, b.Version)}, next.LineFractions[line], next.Lines[line]);
            if (!Accepted(status)) return Failure(status);
            record(status);
        }
        for (std::size_t i = 0; i < next.CandidateBoxesB; ++i) {
            const auto& box = b.Boxes[boxesB[i]];
            const auto status = NativeSourceLineBox(start, end,
                {box.Min, box.Max, Surface(box.Surface, b.Version)}, next.LineFractions[line], next.Lines[line]);
            if (!Accepted(status)) return Failure(status);
            record(status);
        }
        for (std::size_t i = 0; i < next.CandidateTrianglesB; ++i) {
            const auto status = NativeSourceLineTriangle(start, end, trianglesB[triangleIndicesB[i]], next.LineFractions[line], next.Lines[line]);
            if (!Accepted(status)) return Failure(status);
            record(status);
        }
        if (next.LineHits[line]) {
            next.Lines[line].Point = math.Point(transformB, next.Lines[line].Point);
            next.Lines[line].Normal = math.Vector(transformB, next.Lines[line].Normal);
        }
    }
    if (!math.Ok) return Status::Overflow;

    if (next.CandidateSpheresB && (!a.Faces.empty() || !a.Boxes.empty())) {
        const NativeSourceContactSphere boundB{math.Point(bToA, b.BoundCenter), b.BoundRadius, {}};
        std::vector<NativeSourceContactTriangle> trianglesA;
        bound = BindTriangles(a, trianglesA); if (bound != Status::Ok) return bound;
        std::array<std::size_t, 600> triangleIndicesA{};
        for (std::size_t i = 0; i < trianglesA.size(); ++i) {
            const auto status = NativeSourceTestSphereTriangle(boundB, trianglesA[i]);
            if (!Accepted(status)) return Failure(status);
            if (status == ContactStatus::Hit) {
                if (next.CandidateTrianglesA == triangleIndicesA.size()) return Status::Unsupported;
                triangleIndicesA[next.CandidateTrianglesA++] = i;
            }
        }
        std::array<std::size_t, 64> boxesA{};
        for (std::size_t i = 0; i < a.Boxes.size(); ++i) if (math.SphereBox(boundB, a.Boxes[i].Min, a.Boxes[i].Max)) {
            boxesA[next.CandidateBoxesA++] = i;
            if (next.CandidateBoxesA == boxesA.size()) break;
        }
        next.Spheres[next.SphereCount].Depth = -1;
        const auto reverseBegin = next.SphereCount;
        for (std::size_t bi = 0; bi < next.CandidateSpheresB; ++bi) {
            const auto& sphere = spheresB[indicesB[bi]];
            float minimum = 1e24f; bool advance = false;
            for (std::size_t i = 0; i < next.CandidateTrianglesA; ++i) {
                const auto status = NativeSourceSphereTriangle(sphere, trianglesA[triangleIndicesA[i]], minimum, next.Spheres[next.SphereCount]);
                if (!Accepted(status)) return Failure(status);
                advance |= status == ContactStatus::Hit;
            }
            if (advance) {
                next.Spheres[next.SphereCount].Normal = {-next.Spheres[next.SphereCount].Normal[0], -next.Spheres[next.SphereCount].Normal[1], -next.Spheres[next.SphereCount].Normal[2]};
                if (next.SphereCount >= 31) break;
                next.Spheres[++next.SphereCount].Depth = -1;
            }
            minimum = 1e24f;
            for (std::size_t i = 0; i < next.CandidateBoxesA && next.SphereCount < 31; ++i) {
                const auto& box = a.Boxes[boxesA[i]];
                auto& cp = next.Spheres[next.SphereCount];
                const auto status = NativeSourceSphereBox(sphere,
                    {box.Min, box.Max, Surface(box.Surface, a.Version)}, minimum, cp);
                if (!Accepted(status)) return Failure(status);
                if (status == ContactStatus::Hit) {
                    cp.SurfaceA = Surface(box.Surface, a.Version);
                    cp.SurfaceB = sphere.Surface;
                    cp.Normal = {-cp.Normal[0], -cp.Normal[1], -cp.Normal[2]};
                    next.Spheres[++next.SphereCount].Depth = -1;
                }
            }
        }
        for (std::size_t i = reverseBegin; i < next.SphereCount; ++i) {
            auto& cp = next.Spheres[i];
            cp.Point = math.Point(transformA, cp.Point); cp.Normal = math.Vector(transformA, cp.Normal);
            std::swap(cp.SurfaceA, cp.SurfaceB);
        }
    }
    if (!math.Ok) return Status::Overflow;
    return finish();
}
