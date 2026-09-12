#include "NativeSourceContact.h"
#include "NativeSourcePhysical.h"
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace {
using Status = NativeSourceContactStatus;
std::size_t s_Checks = 0;
void Check(bool value, const char* message) {
    ++s_Checks;
    if (!value) { std::fprintf(stderr, "source-contact FAIL: %s\n", message); std::exit(1); }
}
void Spheres() {
    NativeSourceContactSphere a{{1.5f, 0, 0}, 1, {1, 2, 3}}, b{{0, 0, 0}, 1, {4, 5, 6}};
    NativeSourceContactPoint point;
    float limit = 4;
    Check(NativeSourceSphereSphere(a, b, limit, point) == Status::Hit && limit == 0.25f && point.Depth == 0.5f &&
        point.Normal == NativeCollisionVector{1, 0, 0} && point.Point == NativeCollisionVector{1, 0, 0} &&
        point.SurfaceA == a.Surface && point.SurfaceB == b.Surface, "literal source sphere contact and surface provenance");
    const auto retained = point;
    Check(NativeSourceSphereSphere(a, b, limit, point) == Status::Miss && point == retained && limit == 0.25f, "equal squared distance never replaces earlier candidate");
    a.Center[0] = 2; limit = 4;
    Check(NativeSourceSphereSphere(a, b, limit, point) == Status::Miss && point == retained && limit == 4, "touching spheres are not penetration");
    a.Center = b.Center;
    Check(NativeSourceSphereSphere(a, b, limit, point) == Status::Hit && limit == 0 && point.Normal == NativeCollisionVector{1, 0, 0} && point.Depth == 2,
        "source coincident-center normal convention and unclamped penetration depth");
    a.Center = {0.5f, 0, 0}; limit = 4;
    Check(NativeSourceSphereSphere(a, b, limit, point) == Status::Hit && point.Point == a.Center && point.Depth == 1.5f && limit == 0,
        "inside B clamps only touch distance, not penetration depth");
    NativeSourcePhysicalState first, second;
    first.Mass = second.Mass = 1;
    first.IsPed = second.IsPed = first.DisableTurnForce = second.DisableTurnForce = true;
    first.PushOtherPeds = true; first.MoveSpeed = {-5, 0, 0};
    NativeSourcePhysicalContactResult response;
    Check(NativeSourceApplyPedPair(first, second, {point.Point, point.Normal, point.SurfaceA.Material, point.SurfaceB.Material}, response) == NativeSourcePhysicalStatus::Ok &&
        response.Applied && first.MoveSpeed[0] == -4 && second.MoveSpeed[0] == -4 && response.ReportCount == 2,
        "source-generated dynamic contact feeds source pair response without normal inversion");
}
void Triangles() {
    NativeSourceContactTriangle triangle{{{{0, 0, 0}, {4, 0, 0}, {0, 4, 0}}}, 7, 8};
    NativeSourceContactSphere sphere{{1, 1, 0.5f}, 1, {1, 2, 3}};
    NativeSourceContactPoint point;
    float limit = 4;
    Check(NativeSourceSphereTriangle(sphere, triangle, limit, point) == Status::Hit && limit == 0.25f && point.Depth == 0.5f &&
        point.Point == NativeCollisionVector{1, 1, 0} && point.Normal == NativeCollisionVector{0, 0, 1} && point.SurfaceB == NativeSourceContactSurface{7, 0, 8}, "triangle face contact");
    const auto retained = point;
    Check(NativeSourceSphereTriangle(sphere, triangle, limit, point) == Status::Miss && point == retained && limit == 0.25f, "triangle strict nearest ordering");
    sphere.Center[2] = 0; limit = 4;
    Check(NativeSourceSphereTriangle(sphere, triangle, limit, point) == Status::Miss && point == retained && limit == 4, "zero-distance source guard does not emit NaN normal");
    sphere.Center[2] = 1;
    Check(NativeSourceSphereTriangle(sphere, triangle, limit, point) == Status::Miss && point == retained, "tangent sphere-triangle rejected");
    sphere.Center = {1, 1, -0.5f};
    Check(NativeSourceSphereTriangle(sphere, triangle, limit, point) == Status::Hit && point.Normal == NativeCollisionVector{0, 0, -1}, "both triangle sides qualify");
    struct Row { NativeCollisionVector Center, Expected; };
    const Row regions[] = {
        {{-0.25f, -0.25f, 0.25f}, {0, 0, 0}},
        {{4.25f, -0.25f, 0.25f}, {4, 0, 0}},
        {{-0.25f, 4.25f, 0.25f}, {0, 4, 0}},
        {{1, -0.25f, 0.25f}, {1, 0, 0}},
        {{-0.25f, 1, 0.25f}, {0, 1, 0}},
        {{2.25f, 2.25f, 0.25f}, {2, 2, 0}},
    };
    for (const auto& row : regions) {
        sphere.Center = row.Center; limit = 4;
        Check(NativeSourceSphereTriangle(sphere, triangle, limit, point) == Status::Hit && point.Point == row.Expected, "source vertex/edge Voronoi branch");
    }
    const auto saved = point;
    const auto savedLimit = limit;
    triangle.Vertices[2] = triangle.Vertices[1];
    Check(NativeSourceSphereTriangle(sphere, triangle, limit, point) == Status::Unsupported && point == saved && limit == savedLimit, "degenerate geometry is not successful empty collision");
    sphere.Radius = std::numeric_limits<float>::infinity();
    Check(NativeSourceSphereTriangle(sphere, triangle, limit, point) == Status::InvalidInput && point == saved && limit == savedLimit, "nonfinite input preserves result");
    sphere.Radius = 1; sphere.Center = {std::numeric_limits<float>::max(), 0, 0};
    triangle.Vertices[2] = {0, 4, 0};
    Check(NativeSourceSphereTriangle(sphere, triangle, limit, point) == Status::Overflow && point == saved && limit == savedLimit, "intermediate overflow cannot become a false Miss");
}
void RealTriangles(const char* gameDir) {
    NativeCollisionPopulation population;
    population.Models[3991] = {"gsfreeway7_lan", false};
    population.IncludesStreamed = true; // fixture population, not retail world coverage
    NativeCollisionPlacement placement;
    placement.ModelId = 3991; placement.Model = "gsfreeway7_lan";
    placement.Ipl = "isolated-contact-fixture";
    population.Instances.push_back(placement);
    NativeCollisionAssets assets;
    std::string error;
    Check(assets.Load(gameDir, population, error), error.c_str());
    const auto lookup = assets.LookupModel("gsfreeway7_lan");
    Check(bool(lookup.Model) && lookup.Model->Faces.size() == 122, "real source COL model, unchanged reader");
    const auto& model = *lookup.Model;
    const auto& bytes = model.SourceChunk;
    auto word = [&](std::size_t offset, std::size_t count) {
        std::uint32_t value = 0;
        for (std::size_t i = 0; i < count; ++i) value |= std::uint32_t(bytes.at(offset + i)) << (i * 8);
        return value;
    };
    Check(model.Version >= 2 && (model.Flags & 8), "real contact fixture includes authored face groups");
    const auto facesOffset = std::size_t(word(100, 4)) + 4;
    const auto groupCount = word(facesOffset - 4, 4);
    Check(groupCount > 0 && model.FaceGroups.size() == groupCount, "typed group count matches owned source bytes");
    const auto groupsOffset = facesOffset - 4 - groupCount * 28;
    for (std::size_t i = 0; i < groupCount; ++i) {
        const auto p = groupsOffset + i * 28;
        const auto& group = model.FaceGroups[i];
        bool equal = group.First == word(p + 24, 2) && group.Last == word(p + 26, 2);
        for (std::size_t axis = 0; axis < 3; ++axis) {
            equal &= std::bit_cast<std::uint32_t>(group.Min[axis]) == word(p + axis * 4, 4);
            equal &= std::bit_cast<std::uint32_t>(group.Max[axis]) == word(p + 12 + axis * 4, 4);
        }
        Check(equal, "authored group order, bounds and inclusive face range are preserved exactly");
    }
    std::size_t hits = 0;
    for (const auto& face : lookup.Model->Faces) {
        NativeSourceContactTriangle triangle;
        for (std::size_t i = 0; i < 3; ++i) triangle.Vertices[i] = lookup.Model->Vertices[face.Vertices[i]];
        triangle.Material = face.Surface.Material; triangle.Lighting = face.Surface.Light;
        const auto& a = triangle.Vertices[0]; const auto& b = triangle.Vertices[1]; const auto& c = triangle.Vertices[2];
        NativeCollisionVector ab{}, ac{}, normal{};
        for (std::size_t i = 0; i < 3; ++i) { ab[i] = b[i] - a[i]; ac[i] = c[i] - a[i]; }
        normal = {ab[1]*ac[2]-ab[2]*ac[1], ab[2]*ac[0]-ab[0]*ac[2], ab[0]*ac[1]-ab[1]*ac[0]};
        const float length = std::sqrt(normal[0]*normal[0]+normal[1]*normal[1]+normal[2]*normal[2]);
        Check(length > 0, "real fixture has nondegenerate source faces");
        NativeSourceContactSphere sphere;
        sphere.Radius = 0.5f;
        for (std::size_t i = 0; i < 3; ++i) sphere.Center[i] = (a[i]+b[i]+c[i])/3 + normal[i]/length*0.25f;
        NativeSourceContactPoint point;
        float limit = 1;
        Check(NativeSourceSphereTriangle(sphere, triangle, limit, point) == Status::Hit && point.Depth > 0.24f && point.Depth < 0.26f &&
            point.SurfaceB.Material == triangle.Material && point.SurfaceB.Lighting == triangle.Lighting,
            "real COL triangle yields geometric contact with source surface provenance");
        ++hits;
    }
    const auto& face = lookup.Model->Faces.front();
    NativeSourceContactTriangle triangle;
    for (std::size_t i = 0; i < 3; ++i) triangle.Vertices[i] = lookup.Model->Vertices[face.Vertices[i]];
    const auto& a = triangle.Vertices[0]; const auto& b = triangle.Vertices[1]; const auto& c = triangle.Vertices[2];
    NativeCollisionVector ab{}, ac{}, center{}, normal{};
    for (std::size_t i = 0; i < 3; ++i) { ab[i] = b[i]-a[i]; ac[i] = c[i]-a[i]; center[i] = (a[i]+b[i]+c[i])/3; }
    normal = {ab[1]*ac[2]-ab[2]*ac[1], ab[2]*ac[0]-ab[0]*ac[2], ab[0]*ac[1]-ab[1]*ac[0]};
    const float length = std::sqrt(normal[0]*normal[0]+normal[1]*normal[1]+normal[2]*normal[2]);
    NativeCollisionVector start{}, end{};
    for (std::size_t i = 0; i < 3; ++i) { start[i] = center[i]+normal[i]/length; end[i] = center[i]-normal[i]/length; }
    NativeSourceContactPoint linePoint;
    float fraction = 1;
    Check(NativeSourceLineTriangle(start, end, triangle, fraction, linePoint) == Status::Hit && fraction > 0.49f && fraction < 0.51f,
        "real COL general line uses existing packed source plane");
    Check(NativeSourceTestSphereTriangle({center, 1, {}}, triangle) == Status::Hit, "real COL source triangle broadphase");
    std::printf("source-contact-real faces=%zu isolated-local-space no-world-coverage\n", hits);
}
void Boxes() {
    NativeSourceContactSphere sphere{{2.5f, 1, 1}, 1, {1, 2, 3}};
    NativeSourceContactBox box{{0, 0, 0}, {2, 2, 2}, {4, 5, 6}};
    NativeSourceContactPoint point;
    point.SurfaceA.Piece = 17; point.SurfaceB.Piece = 18;
    float limit = 4;
    Check(NativeSourceSphereBox(sphere, box, limit, point) == Status::Hit && point.Point == NativeCollisionVector{2, 1, 1} &&
        point.Normal == NativeCollisionVector{1, 0, 0} && point.Depth == 0.5f && limit == 0.25f &&
        point.SurfaceA == NativeSourceContactSurface{1, 17, 3} && point.SurfaceB == NativeSourceContactSurface{4, 18, 6},
        "source box face and preserved piece fields");
    const auto retained = point;
    Check(NativeSourceSphereBox(sphere, box, limit, point) == Status::Miss && point == retained && limit == 0.25f, "outside box strict nearest candidate");
    sphere.Center = {3, 1, 1}; limit = 4;
    Check(NativeSourceSphereBox(sphere, box, limit, point) == Status::Miss && point == retained && limit == 4, "box tangency rejected");
    sphere.Center = {0.25f, 1, 1}; limit = 0;
    Check(NativeSourceSphereBox(sphere, box, limit, point) == Status::Hit && point.Normal == NativeCollisionVector{-1, 0, 0} &&
        point.Point == NativeCollisionVector{1.25f, 1, 1} && point.Depth == 1.25f && limit == 0, "unique inside face overrides even zero distance, includes sphere radius");
    sphere.Center = {1, 1, 1};
    Check(NativeSourceSphereBox(sphere, box, limit, point) == Status::Hit && point.Normal == NativeCollisionVector{0, 0, -1} && point.Depth == 2 && limit == 0,
        "retail equal-face tie selects negative z at box center");
    sphere.Center = {0.25f, 0.25f, 1};
    Check(NativeSourceSphereBox(sphere, box, limit, point) == Status::Hit && point.Normal == NativeCollisionVector{0, 0, -1} && point.Depth == 2,
        "retail x=y<z tie still selects z, not the mathematically shallowest face");
    const auto inside = point;
    box.Min[0] = 3;
    Check(NativeSourceSphereBox(sphere, box, limit, point) == Status::InvalidInput && point == inside, "inverted box rejected atomically");
}
void Lines() {
    NativeSourceContactTriangle triangle{{{{0, 0, 0}, {4, 0, 0}, {0, 4, 0}}}, 7, 8};
    NativeSourceContactPoint point;
    point.Depth = 19; point.SurfaceA = {9, 10, 11};
    float fraction = 1;
    Check(NativeSourceLineTriangle({0, 1, 1}, {2, 1, -1}, triangle, fraction, point) == Status::Hit && fraction == 0.5f &&
        point.Point == NativeCollisionVector{1, 1, 0} && point.Normal == NativeCollisionVector{0, 0, -1} &&
        point.Depth == 19 && point.SurfaceA == NativeSourceContactSurface{0, 0, 11} && point.SurfaceB == NativeSourceContactSurface{7, 0, 8},
        "general slanted line preserves source plane orientation and untouched fields");
    const auto saved = point;
    Check(NativeSourceLineTriangle({0, 1, 1}, {2, 1, -1}, triangle, fraction, point) == Status::Miss && point == saved && fraction == 0.5f,
        "equal line fraction preserves earlier source candidate");
    fraction = 1;
    Check(NativeSourceLineTriangle({0, 1, 1}, {2, 1, 0}, triangle, fraction, point) == Status::Miss && point == saved && fraction == 1,
        "line endpoint at maximum is excluded");
    Check(NativeSourceLineTriangle({1, 1, 0}, {1, 1, -1}, triangle, fraction, point) == Status::Miss && point == saved,
        "source signbit plane rule rejects same-side zero origin");
    Check(NativeSourceLineTriangle({1, 1, 0}, {1, 1, 1}, triangle, fraction, point) == Status::Hit && fraction == 0,
        "source signed plane crossing may accept zero fraction in opposite direction");
    fraction = 1;
    Check(NativeSourceLineTriangle({5, 5, 1}, {5, 5, -1}, triangle, fraction, point) == Status::Miss && fraction == 1,
        "plane crossing outside triangle does not commit fraction");
    triangle.Vertices[1][0] = 4.001f;
    const auto retained = point;
    Check(NativeSourceLineTriangle({1, 1, 1}, {1, 1, -1}, triangle, fraction, point) == Status::Unsupported && point == retained,
        "unbound unquantized vertex cannot silently change source plane");
}
void SphereAndBoxLines() {
    NativeSourceContactSphere sphere{{0, 0, 0}, 1, {7, 8, 9}};
    NativeSourceContactBox box{{-1, -1, -1}, {1, 1, 1}, {7, 8, 9}};
    NativeSourceContactPoint point;
    point.Depth = 13; point.SurfaceA = {3, 4, 5}; point.SurfaceB = {6, 7, 8};
    float fraction = 1;
    Check(NativeSourceLineSphere({-2, 0, 0}, {2, 0, 0}, sphere, fraction, point) == Status::Hit && fraction == 0.25f &&
        point.Normal == NativeCollisionVector{-1, 0, 0} && point.Point == NativeCollisionVector{-1, 0, 0} && point.Depth == 13 &&
        point.SurfaceA == NativeSourceContactSurface{0, 4, 0} && point.SurfaceB == NativeSourceContactSurface{7, 7, 9}, "source line-sphere fields and entry fraction");
    const auto retained = point;
    Check(NativeSourceLineSphere({-2, 0, 0}, {2, 0, 0}, sphere, fraction, point) == Status::Miss && point == retained && fraction == 0.25f, "line sphere equal-fraction ordering");
    fraction = 1;
    Check(NativeSourceLineSphere({0, 0, 0}, {2, 0, 0}, sphere, fraction, point) == Status::Miss && point == retained, "default inside-origin profile rejected");
    Check(NativeSourceLineSphere({2, 0, 0}, {3, 0, 0}, sphere, fraction, point) == Status::Miss, "ray points away from sphere");
    Check(NativeSourceLineSphere({2, 0, 0}, {2, 0, 0}, sphere, fraction, point) == Status::Miss, "zero-length line rejected");
    Check(NativeSourceLineSphere({-2, 1, 0}, {2, 1, 0}, sphere, fraction, point) == Status::Hit && fraction == 0.5f &&
        point.Point == NativeCollisionVector{0, 1, 0}, "source line-sphere tangent is accepted, unlike sphere-shape penetration");
    for (std::size_t axis = 0; axis < 3; ++axis) {
        for (float sign : {-1.0f, 1.0f}) {
            NativeCollisionVector start{}, end{}, normal{};
            start[axis] = sign * 2; end[axis] = -sign * 2; normal[axis] = sign;
            fraction = 1;
            Check(NativeSourceLineBox(start, end, box, fraction, point) == Status::Hit && fraction == 0.25f &&
                point.Normal == normal && point.Point == normal && point.Depth == 13 && point.SurfaceA.Piece == 4 && point.SurfaceB.Piece == 7,
                "all six source box faces preserve piece/depth fields");
        }
    }
    fraction = 1;
    Check(NativeSourceLineBox({-2, 1, 0}, {2, 1, 0}, box, fraction, point) == Status::Miss && fraction == 1, "source box edge excludes exact face boundary");
    Check(NativeSourceLineBox({0, 0, 0}, {2, 0, 0}, box, fraction, point) == Status::Hit && fraction == 0.5f &&
        point.Normal == NativeCollisionVector{1, 0, 0}, "inside box line finds exit, unlike inside sphere line");
    fraction = 1;
    Check(NativeSourceLineBox({-2, 0, 0}, {-1, 0, 0}, box, fraction, point) == Status::Miss && fraction == 1, "box endpoint strict plane crossing");
}
void TriangleBroadphase() {
    NativeSourceContactTriangle triangle{{{{0, 0, 0}, {4, 0, 0}, {0, 4, 0}}}, 7, 8};
    NativeSourceContactSphere sphere{{1, 1, 1}, 1, {}};
    Check(NativeSourceTestSphereTriangle(sphere, triangle) == Status::Hit, "source broadphase accepts face tangency");
    sphere.Center[2] = 1.01f;
    Check(NativeSourceTestSphereTriangle(sphere, triangle) == Status::Miss, "source broadphase plane separation");
    sphere.Center = {-1, 0, 0};
    Check(NativeSourceTestSphereTriangle(sphere, triangle) == Status::Hit, "source broadphase accepts vertex tangency");
    sphere.Center = {-1.01f, 0, 0};
    Check(NativeSourceTestSphereTriangle(sphere, triangle) == Status::Miss, "source broadphase vertex separation");
    sphere.Center = {1, -1, 0};
    Check(NativeSourceTestSphereTriangle(sphere, triangle) == Status::Hit, "source broadphase accepts edge tangency");
    sphere.Center = {1, -1.01f, 0};
    Check(NativeSourceTestSphereTriangle(sphere, triangle) == Status::Miss, "source broadphase edge separation");
    sphere.Center = {1, 1, 0};
    Check(NativeSourceTestSphereTriangle(sphere, triangle) == Status::Hit, "source broadphase accepts sphere center on face");
}
void ZeroRadius() {
    NativeSourceContactSphere sphere{{0, 0, 0}, 0, {}};
    NativeSourceContactSphere other{{0.5f, 0, 0}, 1, {}};
    NativeSourceContactPoint point;
    float maximum = 1;
    Check(NativeSourceSphereSphere(sphere, other, maximum, point) == Status::Miss && maximum == 1, "authored zero-radius A reaches source Miss, not invalid-input rejection");
    Check(NativeSourceSphereSphere(other, sphere, maximum, point) == Status::Hit && maximum == 0.25f && point.Depth == 0.5f,
        "zero-radius B remains a source point target");
    NativeSourceContactTriangle triangle{{{{0, 0, 0}, {4, 0, 0}, {0, 4, 0}}}, 0, 0};
    sphere.Center = {1, 1, 0}; maximum = 1;
    Check(NativeSourceSphereTriangle(sphere, triangle, maximum, point) == Status::Miss && maximum == 1,
        "zero-radius triangle penetration is empty");
    Check(NativeSourceTestSphereTriangle(sphere, triangle) == Status::Hit, "source zero-radius broadphase remains inclusive");
    NativeSourceContactBox box{{0, 0, -1}, {4, 4, 1}, {}};
    sphere.Center = {0.25f, 1, 0};
    Check(NativeSourceSphereBox(sphere, box, maximum, point) == Status::Hit && maximum == 0 && point.Depth == 0.25f,
        "source inside-box branch does not discard zero-radius authored sphere");
}
}
int main(int argc, char** argv) {
    if (argc > 2) return 2;
    Spheres(); Triangles(); Boxes(); Lines(); SphereAndBoxLines(); TriangleBroadphase(); ZeroRadius();
    if (argc == 2) RealTriangles(argv[1]);
    std::printf("source-contact-ok checks=%zu upstream-model-sphere-branches no-world-coverage-claim\n", s_Checks);
}
