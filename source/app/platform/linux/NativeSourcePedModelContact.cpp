#include "NativeSourcePedModelContact.h"
#include <algorithm>
#include <limits>

namespace {
using Vec = NativeCollisionVector;
using Matrix = NativeSourceGroundTransform;
using Status = NativeSourcePedModelStatus;
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
            const auto high = F(s.Center[i] + s.Radius);
            const auto low = F(s.Center[i] - s.Radius);
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
}

NativeSourcePedModelStatus NativeSourceProcessPedModel(const NativeSourcePedCollisionShape& ped,
    const Matrix& pedTransform, const NativeCollisionModel& other, const Matrix& otherTransform,
    NativeSourcePedModelContacts& out) {
    if (!Valid(pedTransform) || !Valid(otherTransform) || !Bounds(ped.Min, ped.Max) ||
        !Valid(NativeSourceContactSphere{ped.BoundCenter, ped.BoundRadius, {}}) || ped.LineCount > 2 ||
        !std::ranges::all_of(ped.Spheres, [](const auto& s) { return Valid(s); }) ||
        !Finite(ped.LineStart) || !Finite(ped.LineEnd) || !Finite(ped.HeadLineStart) || !Finite(ped.HeadLineEnd)) return Status::InvalidInput;
    for (auto f : out.LineFractions) if (!std::isfinite(f) || f < 0 || f > 1) return Status::InvalidInput;
    if (!other.Unsupported.empty() || other.Version < 1 || other.Version > 4 || other.Spheres.size() > 128 ||
        other.Boxes.size() > 65535 || other.Faces.size() > 65535 || !Bounds(other.Min, other.Max)) return Status::Unsupported;
    for (const auto& s : other.Spheres) if (!Valid(NativeSourceContactSphere{s.Center, s.Radius, {}})) return Status::InvalidInput;
    for (const auto& b : other.Boxes) if (!Bounds(b.Min, b.Max)) return Status::InvalidInput;
    for (const auto& group : other.FaceGroups) {
        if (!Bounds(group.Min, group.Max) || group.First > group.Last || group.Last >= other.Faces.size()) return Status::InvalidInput;
    }
    if (!other.FaceGroups.empty() && (other.Version == 1 || !(other.Flags & 8))) return Status::Unsupported;
    NativeSourcePedModelContacts next = out;
    next.SphereCount = next.CandidateSpheres = next.CandidateBoxes = next.CandidateTriangles = 0;
    next.LineHits = {};
    auto finish = [&] { out = next; return Status::Ok; };
    if (!ped.QueryEnabled || other.Empty) return finish();
    Math math;
    const auto aToB = math.Multiply(math.Invert(otherTransform), pedTransform);
    const auto bToA = math.Multiply(math.Invert(pedTransform), otherTransform);
    const NativeSourceContactSphere bound{math.Point(aToB, ped.BoundCenter), ped.BoundRadius, {}};
    const bool overlaps = math.SphereBox(bound, other.Min, other.Max);
    if (!math.Ok) return Status::Overflow;
    if (!overlaps) return finish();

    std::array<NativeSourceContactSphere, 3> spheresA = ped.Spheres;
    std::array<std::size_t, 3> indicesA{};
    std::array<std::size_t, 128> indicesB{};
    std::array<std::size_t, 64> boxesB{};
    std::size_t countA = 0;
    for (std::size_t i = 0; i < spheresA.size(); ++i) {
        spheresA[i].Center = math.Point(aToB, spheresA[i].Center);
        if (math.SphereBox(spheresA[i], other.Min, other.Max)) indicesA[countA++] = i;
    }
    for (std::size_t i = 0; i < other.Spheres.size(); ++i) {
        const auto& s = other.Spheres[i];
        if (math.SphereBox({math.Point(bToA, s.Center), s.Radius, {}}, ped.Min, ped.Max)) indicesB[next.CandidateSpheres++] = i;
    }
    if (!math.Ok) return Status::Overflow;
    if (!countA && !ped.LineCount && !next.CandidateSpheres) return finish();
    for (std::size_t i = 0; i < other.Boxes.size(); ++i) {
        if (math.SphereBox(bound, other.Boxes[i].Min, other.Boxes[i].Max)) {
            boxesB[next.CandidateBoxes++] = i;
            if (next.CandidateBoxes == boxesB.size()) break; // Source MAX_BOXES truncation.
        }
    }
    if (!math.Ok) return Status::Overflow;
    // Binding is source fixed-point conversion, not a second asset reader.
    std::vector<NativeSourceContactTriangle> triangles;
    triangles.reserve(other.Faces.size());
    for (const auto& face : other.Faces) {
        NativeSourceContactTriangle triangle;
        triangle.Material = face.Surface.Material;
        triangle.Lighting = face.Surface.Light;
        for (std::size_t i = 0; i < 3; ++i) {
            if (face.Vertices[i] >= other.Vertices.size()) return Status::InvalidInput;
            const auto& v = other.Vertices[face.Vertices[i]];
            for (std::size_t axis = 0; axis < 3; ++axis) {
                const float scaled = v[axis] * 128.0f;
                if (!std::isfinite(scaled) || scaled < -32768 || scaled >= 32768) return Status::Unsupported;
                if (other.Version != 1 && std::trunc(scaled) != scaled) return Status::Unsupported;
                triangle.Vertices[i][axis] = float(static_cast<std::int16_t>(scaled)) / 128.0f;
            }
        }
        NativeSourceGroundPlane plane;
        if (!NativeSourceGround::CalculatePlane(triangle.Vertices[0], triangle.Vertices[1], triangle.Vertices[2], plane)) return Status::Unsupported;
        triangles.push_back(triangle);
    }
    std::array<std::size_t, 600> trianglesB{};
    auto candidate = [&](std::size_t i) {
        const auto status = NativeSourceTestSphereTriangle(bound, triangles[i]);
        if (!Accepted(status)) return Failure(status);
        if (status == ContactStatus::Hit) {
            // Original grouped loop can overrun at a following group. Do not reproduce memory corruption.
            if (next.CandidateTriangles == trianglesB.size()) return Status::Unsupported;
            trianglesB[next.CandidateTriangles++] = i;
        }
        return Status::Ok;
    };
    if (other.Flags & 8) {
        for (const auto& group : other.FaceGroups) {
            if (math.SphereBox(bound, group.Min, group.Max)) {
                for (std::size_t i = group.First; i <= group.Last; ++i) {
                    const auto status = candidate(i);
                    if (status != Status::Ok) return status;
                    if (next.CandidateTriangles == trianglesB.size()) break;
                }
            }
        }
    } else {
        for (std::size_t i = 0; i < triangles.size() && next.CandidateTriangles < trianglesB.size(); ++i) {
            const auto status = candidate(i);
            if (status != Status::Ok) return status;
        }
    }
    if (!math.Ok) return Status::Overflow;
    if (!next.CandidateSpheres && !next.CandidateBoxes && !next.CandidateTriangles) return finish();
    next.Spheres[0].Depth = -1;
    for (std::size_t a = 0; a < countA; ++a) {
        const auto& sphere = spheresA[indicesA[a]];
        float minimum = 1e24f;
        bool advance = false;
        for (std::size_t i = 0; i < next.CandidateSpheres; ++i) {
            const auto& b = other.Spheres[indicesB[i]];
            const auto status = NativeSourceSphereSphere(sphere, {b.Center, b.Radius, Surface(b.Surface, other.Version)}, minimum, next.Spheres[next.SphereCount]);
            if (!Accepted(status)) return Failure(status);
            advance |= status == ContactStatus::Hit;
        }
        auto emit = [&](ContactStatus status) {
            if (status != ContactStatus::Hit) return;
            if (ped.ReturnAllContacts && sphere.Surface.Piece <= 2 && next.SphereCount < 31) {
                advance = false;
                minimum = 1e24f;
                next.Spheres[++next.SphereCount].Depth = -1;
            } else advance = true;
        };
        for (std::size_t i = 0; i < next.CandidateBoxes; ++i) {
            const auto& b = other.Boxes[boxesB[i]];
            const auto status = NativeSourceSphereBox(sphere, {b.Min, b.Max, Surface(b.Surface, other.Version)}, minimum, next.Spheres[next.SphereCount]);
            if (!Accepted(status)) return Failure(status);
            emit(status);
        }
        for (std::size_t i = 0; i < next.CandidateTriangles; ++i) {
            const auto status = NativeSourceSphereTriangle(sphere, triangles[trianglesB[i]], minimum, next.Spheres[next.SphereCount]);
            if (!Accepted(status)) return Failure(status);
            emit(status);
        }
        if (advance) {
            if (next.SphereCount >= 31) break;
            next.Spheres[++next.SphereCount].Depth = -1;
        }
    }
    for (std::size_t i = 0; i < next.SphereCount; ++i) {
        next.Spheres[i].Point = math.Point(otherTransform, next.Spheres[i].Point);
        next.Spheres[i].Normal = math.Vector(otherTransform, next.Spheres[i].Normal);
    }
    for (std::size_t line = 0; line < ped.LineCount; ++line) {
        const auto start = math.Point(aToB, line == 0 ? ped.LineStart : ped.HeadLineStart);
        const auto end = math.Point(aToB, line == 0 ? ped.LineEnd : ped.HeadLineEnd);
        if (!math.Ok) return Status::Overflow;
        auto record = [&](ContactStatus status) { next.LineHits[line] |= status == ContactStatus::Hit; };
        for (std::size_t i = 0; i < next.CandidateSpheres; ++i) {
            const auto& b = other.Spheres[indicesB[i]];
            const auto status = NativeSourceLineSphere(start, end, {b.Center, b.Radius, Surface(b.Surface, other.Version)}, next.LineFractions[line], next.Lines[line]);
            if (!Accepted(status)) return Failure(status);
            record(status);
        }
        for (std::size_t i = 0; i < next.CandidateBoxes; ++i) {
            const auto& b = other.Boxes[boxesB[i]];
            const auto status = NativeSourceLineBox(start, end, {b.Min, b.Max, Surface(b.Surface, other.Version)}, next.LineFractions[line], next.Lines[line]);
            if (!Accepted(status)) return Failure(status);
            record(status);
        }
        for (std::size_t i = 0; i < next.CandidateTriangles; ++i) {
            const auto status = NativeSourceLineTriangle(start, end, triangles[trianglesB[i]], next.LineFractions[line], next.Lines[line]);
            if (!Accepted(status)) return Failure(status);
            record(status);
        }
        if (next.LineHits[line]) {
            next.Lines[line].Point = math.Point(otherTransform, next.Lines[line].Point);
            next.Lines[line].Normal = math.Vector(otherTransform, next.Lines[line].Normal);
        }
    }
    if (!math.Ok) return Status::Overflow;
    return finish();
}
