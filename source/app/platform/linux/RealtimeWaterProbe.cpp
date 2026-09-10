// Actual native module + actual GpuScene/Camera, an extracted source oracle,
// real particle TXD and pager shoreline. This TU is not in the product target.
#define GL_GLEXT_PROTOTYPES
#include "app/platform/linux/Realtime.cpp"
#include "app/platform/linux/TexSample.h"
#include "RealtimeWaterProbe.oracle.h"
#include "RealtimeWaterScanProof.h"
#include <EGL/eglext.h>
#include <rw.h>
#include <chrono>
#include <numeric>

bool RealtimeWaterProbeLegacy_Load(const char*, const char*, WaterLevelData&, char*, std::size_t);
bool RealtimeWaterProbeLegacy_BuildScene(const WaterLevelData&, const uint8_t[4], WorldShotScene&, int&, char*, std::size_t);

namespace {
constexpr int kWidth = 960, kHeight = 540;
static void Require(bool ok, const char* reason) {
    if (!ok) {
        std::fprintf(stderr, "water-probe-fail %s GL=%x EGL=%x\n", reason, glGetError(), eglGetError());
        std::exit(1);
    }
}
static uint64_t Hash(const std::vector<unsigned char>& bytes) {
    uint64_t h = 14695981039346656037ull;
    for (auto b : bytes) h = (h ^ b) * 1099511628211ull;
    return h;
}
static RealtimeWaterSample Oracle(int x, int y, float z, float big, float small, const RealtimeWaterState& state) {
    using namespace water_oracle;
    CTimer::time = state.gameMs;
    CWaterLevel::m_nWaterTimeOffset = state.waterTimeOffset;
    CWeather::Wavyness = state.wavyness;
    CWeather::SunGlare = state.sunGlare;
    CVector normal{0, 0, 1};
    RealtimeWaterSample result{};
    result.z = z;
    CWaterLevel::CalculateWavesOnlyForCoordinate(x, y, big, small, result.z, result.colorMult, result.glare, normal);
    result.normal = {normal.x, normal.y, normal.z};
    return result;
}
static void CheckOffline(const char* gameDir) {
    char error[512]{};
    for (const char* file : {"data/water.dat", "data/water1.dat"}) {
        WaterLevelData actual{}, legacy{};
        Require(WaterLevel_Load(gameDir, file, actual, error, sizeof(error)), error);
        Require(RealtimeWaterProbeLegacy_Load(gameDir, file, legacy, error, sizeof(error)), error);
        for (int hour : {0, 12, 23}) {
            TimeCycleParams params{};
            Require(TimeCycle_LoadHour(gameDir, hour, params, error, sizeof(error)), error);
            WorldShotScene a{}, b{};
            int aTris = 0, bTris = 0;
            Require(WaterLevel_BuildScene(actual, params.water, a, aTris, error, sizeof(error)), error);
            Require(RealtimeWaterProbeLegacy_BuildScene(legacy, params.water, b, bTris, error, sizeof(error)), error);
            Require(aTris == bTris && a.meshes.size() == b.meshes.size(), "offline legacy triangle counts");
            for (size_t i = 0; i < a.meshes.size(); ++i) {
                const auto& x = a.meshes[i]; const auto& y = b.meshes[i];
                Require(x.pos == y.pos && x.nrm == y.nrm && x.triImg == y.triImg && x.triCol == y.triCol && x.uv == y.uv,
                    "offline legacy mesh bytes");
            }
            std::vector<unsigned char> aPixels, bPixels;
            TexFrameStats aStats{}, bStats{};
            TexSample_RenderOrbit(a, 640, 360, 0, nullptr, aPixels, aStats);
            TexSample_RenderOrbit(b, 640, 360, 0, nullptr, bPixels, bStats);
            Require(aPixels == bPixels, "offline pixel hashes versus 4209d974");
            std::printf("water-offline file=%s hour=%d tris=%d fnv=%016llx baseline=4209d974 exact\n",
                file, hour, aTris, (unsigned long long)Hash(aPixels));
        }
    }
}
static void CheckOracle(RealtimeEnvironment& env) {
    size_t cases = 0;
    float worst = 0;
    for (uint32_t ms : {0u, 1u, 2999u, 3000u, 3499u, 3500u, 4095u, 4096u, 4999u, 5000u, 8191u, 8192u, 0xffffffffu}) {
        for (float wavyness : {.3f, .55f, 1.f}) for (uint32_t offset : {0u, 7000u}) {
            auto state = env.GetWaterState();
            state.gameMs = ms; state.waterTimeOffset = offset; state.wavyness = wavyness; state.sunGlare = .7f;
            Require(env.SetWaterState(state), "explicit weather/time");
            for (const auto& poly : env.GetWaterData().polys) for (int i = 0; i < poly.nverts; ++i) {
                const auto& v = poly.v[i];
                const auto actual = env.SampleWater(int(v.x), int(v.y), v.z, v.bigWaves, v.smallWaves);
                const auto ref = Oracle(int(v.x), int(v.y), v.z, v.bigWaves, v.smallWaves, state);
                for (float error : {actual.z-ref.z, actual.colorMult-ref.colorMult, actual.glare-ref.glare,
                     actual.normal[0]-ref.normal[0], actual.normal[1]-ref.normal[1], actual.normal[2]-ref.normal[2]})
                    worst = std::max(worst, std::abs(error));
                ++cases;
            }
        }
    }
    Require(worst < 1.e-5f, "source-extracted wave/normal/color/glare oracle");
    std::printf("water-oracle cases=%zu max-error=%.9g\n", cases, worst);
}

static void CheckSeaBedCpu(const RealtimeEnvironment& env) {
    size_t vertices = 0;
    for (const auto& ref : water_oracle::seaBedFixtures) {
        const std::array block{RealtimeWaterBlock{int16_t(ref.x),int16_t(ref.y)}};
        RealtimeSeaBedGeometry actual;
        Require(RealtimeEnvironment::BuildSeaBed(block,ref.cx,ref.cy,ref.area,actual),"seabed fixture build");
        Require(actual.size == ref.vertices.size(),"seabed source LOD/edge/area counts");
        Require(std::equal(ref.vertices.begin(),ref.vertices.end(),actual.vertices.begin()),
            "seabed independently rational source-store fixture exact XYZ/UV/order");
        vertices += actual.size;
    }
    RealtimeSeaBedGeometry capacity;
    std::array<RealtimeWaterBlock,70> maximum;
    maximum.fill({-1,6}); // storage bound, not an authored visibility list
    Require(RealtimeEnvironment::BuildSeaBed(maximum,-3250,250,0,capacity) && capacity.size==1120,"seabed capacity bound");
    const auto saved = capacity.vertices;
    const std::array<RealtimeWaterBlock,71> tooMany{};
    Require(!RealtimeEnvironment::BuildSeaBed(tooMany,0,0,0,capacity) && capacity.vertices==saved,"seabed oversized list rejected atomically");
    Require(!RealtimeEnvironment::BuildSeaBed(maximum,NAN,0,0,capacity) && capacity.vertices==saved,"seabed invalid camera atomic");
    size_t limited = 0, checks = 0;
    for (const auto& poly : env.GetWaterData().polys) {
        limited += (poly.flags & 2) != 0;
        const float z = poly.v[0].z;
        for (const auto& test : std::array<std::pair<float,bool>,5>{{
                 {std::nextafter(z-6,INFINITY),true},{std::nextafter(z-6,-INFINITY),(poly.flags&2)==0},
                 {std::nextafter(z+20,-INFINITY),true},{std::nextafter(z+20,INFINITY),false},{z,true}}}) {
            Require(RealtimeEnvironment::WaterQueryHeightAllowed(poly.flags,z,test.first)==test.second,
                "actual authored limited-depth query -6/+20 inclusive boundary");
            ++checks;
        }
    }
    // Exactly representable boundaries; fractional authored heights above use
    // adjacent floats on each side of the original unrounded x87 threshold.
    Require(env.WaterQueryHeightAllowed(3,42,36) && env.WaterQueryHeightAllowed(3,42,62)
        && !env.WaterQueryHeightAllowed(3,42,35.999996185302734375f)
        && !env.WaterQueryHeightAllowed(1,42,62.000003814697265625f)
        && env.WaterQueryHeightAllowed(1,42,-1000000),"exact limited query boundary fixture");
    Require(limited==21,"authored limited-depth metadata count");
    std::printf("seabed-cpu fixtures=%zu exact-vertices=%zu capacity=1120 limited-polys=%zu height-boundaries=%zu exact; depth-bit-is-query-not-floor\n",
        water_oracle::seaBedFixtures.size(),vertices,limited,checks);
}

static void CheckFlow(RealtimeEnvironment& env) {
    Require(env.SetWaterState({}), "flow reset snapshot");
    RealtimeWaterState state{};
    state.accumulateFlow = true; state.gameMs = 100;
    state.firstFlowUV = state.secondFlowUV = {.99999f,-.25f};
    state.currentFlow = {.5f,-.5f}; state.flowTimeStep = 1;
    Require(env.SetWaterState(state), "seed owned flow");
    state.gameMs = 120;
    Require(env.SetWaterState(state), "advance owned flow");
    // Independent constant fixture evaluated from retail 728DA7..728E8E's
    // x87 operands and float stores (including the strict >1 subtraction).
    const std::array first{0x1.a0d000p-10f,-0x1.01a36ep-2f};
    const std::array second{0x1.9e3000p-11f,-0x1.00d1b8p-2f};
    Require(env.GetWaterState().firstFlowUV==first && env.GetWaterState().secondFlowUV==second,"original flow arithmetic fixture exact");
    for (int i=0; i<8; ++i) Require(env.SetWaterState(state),"same clock repeated setter");
    Require(env.GetWaterState().firstFlowUV==first && env.GetWaterState().secondFlowUV==second,"same-clock flow is idempotent");
    state.flowTimeStep = std::numeric_limits<float>::quiet_NaN();
    Require(!env.SetWaterState(state) && env.GetWaterState().firstFlowUV==first,"invalid timestep rejected atomically");
    Require(env.SetWaterState({}),"restore absolute flow");
    state = {}; state.accumulateFlow = true; state.gameMs = 0xffffffffu;
    state.firstFlowUV = state.secondFlowUV = {1.f,-1.f}; state.flowTimeStep=1;
    Require(env.SetWaterState(state),"wrap-clock seed");
    state.gameMs = 0;
    Require(env.SetWaterState(state),"simulation clock wrap");
    Require(env.GetWaterState().firstFlowUV==state.firstFlowUV && env.GetWaterState().secondFlowUV==state.secondFlowUV,
        "source equality/negative accumulators do not wrap");
    Require(env.SetWaterState({}),"restore default flow");
    std::puts("water-flow original-arithmetic-exact owned-once-per-gameMs strict-wrap/negative/idempotent/clock-wrap-ok");
}

// Independent selector lift: compact function is still a plugin stub. Branch
// provenance is retail 724300..7247C9, statically inspected only. Canonical vertex
// ownership below runs verbatim extracted AddWaterLevelVertex, not native code.
struct FlowReference {
    struct Quad { std::array<WaterVert, 4> v; int polygon; };
    std::vector<Quad> quads;
    explicit FlowReference(const WaterLevelData& data) {
        using W = water_oracle::CWaterLevel;
        W::NumWaterVertices = 0;
        for (size_t p = 0; p < data.polys.size(); ++p) {
            const auto& poly = data.polys[p];
            bool sameX = true, sameY = true;
            for (int i = 1; i < poly.nverts; ++i) {
                sameX &= int(poly.v[i].x) == int(poly.v[0].x);
                sameY &= int(poly.v[i].y) == int(poly.v[0].y);
            }
            if (sameX || sameY) continue;
            Quad q{{}, int(p)};
            for (int i = 0; i < poly.nverts; ++i) {
                const auto& v = poly.v[i];
                const auto id = W::AddWaterLevelVertex(int(v.x), int(v.y),
                    {v.z, v.bigWaves, v.smallWaves, int8_t(int(v.flowX * 64)), int8_t(int(v.flowY * 64))});
                Require(W::NumWaterVertices < W::m_aVertices.size(), "oracle source vertex capacity");
                const auto& s = W::m_aVertices[id];
                q.v[i] = {float(s.x), float(s.y), s.rp.z, float(s.rp.flowX)/64, float(s.rp.flowY)/64, s.rp.big, s.rp.small};
            }
            if (poly.nverts != 4) continue;
            std::sort(q.v.begin(), q.v.end(), [](const auto& a, const auto& b) { return a.y == b.y ? a.x < b.x : a.y < b.y; });
            quads.push_back(q);
        }
    }
    RealtimeWaterFlowSelection Find(float x, float y, std::array<float, 2> previous = {}) const {
        RealtimeWaterFlowSelection r;
        if (!(x > -3000 && x < 3000 && y > -3000 && y < 3000)) { // 72433C..724383
            r.result = RealtimeWaterFlowSelection::Result::OutsideWorld;
            r.nearestWavyDistance = 0;
            return r;
        }
        r.desired = previous;
        float flowDistance = 1.e7f;
        for (const auto& q : quads) {
            float dx = 0, dy = 0;
            if (x < q.v[0].x) dx = q.v[0].x-x; else if (x > q.v[1].x) dx = x-q.v[1].x;
            if (y < q.v[0].y) dy = q.v[0].y-y; else if (y > q.v[2].y) dy = y-q.v[2].y;
            const float squared = float(double(dx)*dx + double(dy)*dy);
            const float distance = float(std::sqrt(double(squared))); // 72444E..724470
            if (r.nearestWavyDistance > distance &&
                (q.v[0].bigWaves || q.v[0].smallWaves || q.v[1].bigWaves || q.v[1].smallWaves ||
                 q.v[2].bigWaves || q.v[2].smallWaves || q.v[3].bigWaves || q.v[3].smallWaves)) {
                r.nearestWavyDistance = distance;
                r.nearestWavyHeight = q.v[0].z; // 724538..724543
            }
            if (!(flowDistance > distance)) continue; // 724548..724552
            flowDistance = distance;
            float d[4];
            for (int i = 0; i < 4; ++i) {
                const double vx = double(x)-q.v[i].x, vy = double(y)-q.v[i].y;
                d[i] = float(vx*vx + vy*vy); // four float stores 724599/5CE/607/623
            }
            // Literal comparison cascade 724626..724767, not native min-loop.
            const int corner = d[0]<d[1] && d[0]<d[2] && d[0]<d[3] ? 0 :
                               d[1]<d[2] && d[1]<d[3] ? 1 : d[2]<d[3] ? 2 : 3;
            r.desired = {q.v[corner].flowX, q.v[corner].flowY};
            r.corner = corner; r.polygon = q.polygon;
            r.result = RealtimeWaterFlowSelection::Result::SelectedQuad;
        }
        return r;
    }
};
static bool SameSelection(const RealtimeWaterFlowSelection& a, const RealtimeWaterFlowSelection& b) {
    return a.result == b.result && a.desired == b.desired && a.polygon == b.polygon && a.corner == b.corner &&
        a.nearestWavyDistance == b.nearestWavyDistance && a.nearestWavyHeight == b.nearestWavyHeight;
}
static void ReferenceUV(RealtimeWaterState& s) {
    // Original promoted float constants; independent of AdvanceFlowUV.
    for (int i = 0; i < 2; ++i) {
        const double travel = double(s.flowTimeStep) * 0.039999999105930328369140625 * double(s.currentFlow[i]);
        s.firstFlowUV[i] = float(double(s.firstFlowUV[i]) + travel * 0.07999999821186065673828125);
        s.secondFlowUV[i] = float(double(s.secondFlowUV[i]) + travel * 0.039999999105930328369140625);
        if (s.firstFlowUV[i] > 1) s.firstFlowUV[i] = s.firstFlowUV[i]-1;
        if (s.secondFlowUV[i] > 1) s.secondFlowUV[i] = s.secondFlowUV[i]-1;
    }
}
static bool SameFlow(const RealtimeWaterState& a, const RealtimeWaterState& b) {
    return a.currentFlow == b.currentFlow && a.firstFlowUV == b.firstFlowUV && a.secondFlowUV == b.secondFlowUV &&
        a.flowTimeStep == b.flowTimeStep && a.gameMs == b.gameMs;
}
static std::vector<WaterLevelData> FlowFixtures() {
    auto quad = [](float x, float y, float z) {
        WaterPoly p{}; p.nverts = 4; p.flags = 1;
        for (int i = 0; i < 4; ++i) p.v[i] = {x+(i%2)*10, y+(i/2)*10, z, (i+1)*.1099f, -(i+1)*.1099f, 1, .2f};
        std::swap(p.v[0], p.v[3]); // authored order differs from sorted order
        return p;
    };
    std::vector<WaterLevelData> fixtures(8);
    auto triangle = quad(0, 0, 5); triangle.nverts = 3;
    triangle.v[0] = {0, 0, 5, .015624f, -.015626f, 0, 0};
    triangle.v[1] = {10, 0, 5, .5f, -.5f, 0, 0};
    triangle.v[2] = {0, 10, 5, .75f, -.75f, 0, 0};
    fixtures[1].polys = {triangle};
    fixtures[2].polys = {quad(0, 0, 5)};
    auto hidden = quad(0, 0, 1005); hidden.flags = 0;
    for (auto& v : hidden.v) v.bigWaves = v.smallWaves = 0;
    fixtures[3].polys = {hidden, quad(0, 0, 5), quad(10, 0, 20)};
    fixtures[4].polys = {triangle, quad(0, 0, 5), quad(0, 0, 6)};
    fixtures[5].polys = {quad(-3005, -4, 55), quad(2995, -4, 77), quad(3001, 30, 88)};
    auto degenerate = quad(0, 0, 5);
    for (auto& v : degenerate.v) { v.x = 0; v.flowX = 1.9f; }
    fixtures[6].polys = {degenerate, quad(0, 0, 5)};
    auto fractional = quad(-3.75f, -2.99f, 9);
    fractional.v[0].flowX = 1.999f; fractional.v[1].flowX = 2.125f;
    fractional.v[2].flowY = -2.125f;
    fixtures[7].polys = {fractional};
    return fixtures;
}
static void CheckNearest(const WaterLevelData& real) {
    auto fixtures = FlowFixtures(); fixtures.push_back(real);
    size_t comparisons = 0, nonzero = 0;
    for (const auto& p : real.polys) for (int i = 0; i < p.nverts; ++i)
        nonzero += p.v[i].flowX != 0 || p.v[i].flowY != 0;
    Require(nonzero == 0, "actual water.dat has no authored current flow");
    for (auto& data : fixtures) {
        RealtimeWaterFlow actual; actual.Initialise(data);
        FlowReference reference(data);
        std::vector<std::array<float, 2>> points;
        for (float x : {-3001.f, -3000.f, std::nextafter(-3000.f, 0.f), -10.f, 0.f, 5.f, std::nextafter(5.f, 6.f),
                         10.f, 15.f, 20.f, std::nextafter(3000.f, 0.f), 3000.f, 3001.f})
            for (float y : {-3001.f, -3000.f, -10.f, 0.f, 5.f, std::nextafter(5.f, 6.f), 10.f, 15.f, 20.f, 3000.f, 3001.f})
                points.push_back({x,y});
        for (int x = -20; x <= 20; ++x) for (int y = -20; y <= 20; ++y) points.push_back({x*149.93f, y*149.97f});
        for (const auto& p : data.polys) {
            float x = 0, y = 0;
            for (int i = 0; i < p.nverts; ++i) {
                points.push_back({p.v[i].x, p.v[i].y});
                points.push_back({std::nextafter(p.v[i].x, INFINITY), std::nextafter(p.v[i].y, -INFINITY)});
                x += p.v[i].x; y += p.v[i].y;
            }
            points.push_back({x/p.nverts, y/p.nverts});
        }
        data.polys.clear(); // the native field owns its startup cache
        for (const auto& xy : points) {
            const auto a = actual.FindNearest(xy[0], xy[1], {.375f,-.625f});
            const auto b = reference.Find(xy[0], xy[1], {.375f,-.625f});
            Require(SameSelection(a,b), "independent nearest selector/vertex ownership differential");
            ++comparisons;
        }
    }
    // Explicit expectations keep both implementations honest at ambiguous edges.
    auto data = FlowFixtures(); RealtimeWaterFlow f;
    f.Initialise(data[2]); Require(f.FindNearest(5,5).corner == 3, "last corner wins four-way tie");
    Require(f.FindNearest(5,0).corner == 1 && f.FindNearest(0,5).corner == 2, "last corner wins two-way ties");
    f.Initialise(data[3]); const auto overlap = f.FindNearest(5,5);
    Require(overlap.polygon == 0 && overlap.nearestWavyHeight == 5, "first quad wins, invisible/interior not filtered, separate wavy height");
    f.Initialise(data[4]); Require(f.FindNearest(0,0).desired == std::array{0.f,-1.f/64}, "triangle first-owned metadata, signed truncation");
    f.Initialise(data[1]); Require(f.FindNearest(0,0,{.25f,-.25f}).desired == std::array{.25f,-.25f}, "triangles are not flow candidates");
    Require(f.FindNearest(3000,0,{.25f,-.25f}).desired == std::array{0.f,0.f}, "world boundary clears desired");
    std::printf("water-nearest comparisons=%zu exact fixtures=8 real-quads=%d authored-nonzero-flow=%zu ties/bounds/flags/triangles/dedup/clamp-ok\n",
        comparisons, real.quads, nonzero);
}

static void CheckFlowTicks(const WaterLevelData& real) {
    using W = water_oracle::CWaterLevel;
    using T = water_oracle::CTimer;
    auto fixtures = FlowFixtures(); fixtures.push_back(real);
    size_t checked = 0, totalSelections = 0;
    for (size_t fixture = 0; fixture < fixtures.size(); ++fixture) {
        RealtimeWaterState baseline{};
        for (int fps : {15, 30, 60, 144}) {
            RealtimeWaterFlow actual; actual.Initialise(fixtures[fixture]);
            FlowReference reference(fixtures[fixture]);
            RealtimeWaterFlowSelection selected{};
            RealtimeWaterState a{}, b{};
            a.gameMs = b.gameMs = 1700; // wave clock independent of flow tick clock
            a.currentFlow = b.currentFlow = {.5f,-.000001f};
            a.firstFlowUV = a.secondFlowUV = b.firstFlowUV = b.secondFlowUV = {.99999f,-.25f};
            W::m_CurrentFlow = {.5f,-.000001f}; W::m_CurrentDesiredFlow = {};
            unsigned n = 0, elapsed = 0, selections = 0;
            RealtimeWaterFlowTick last{};
            W::nearestCallback = [&] {
                selected = reference.Find(last.cameraX, last.cameraY, selected.desired);
                W::m_CurrentDesiredFlow = {selected.desired[0], selected.desired[1]};
                ++selections;
            };
            for (int presentation = 0; presentation < fps*40; ++presentation) {
                const unsigned due = unsigned((presentation+1)*30/fps);
                while (n < due) {
                    last = {}; last.frame = 0xffffffc0u+n;
                    const bool sourcePaused = n >= 128 && n < 160;
                    last.suspended = n >= 500 && n < 532;
                    last.canSeeWater = !(n >= 64 && n < 96);
                    if (!sourcePaused && !last.suspended) ++elapsed;
                    last.gameMs = 0xfffffd00u + elapsed*1000/30;
                    last.timeStep = sourcePaused ? .00001f : last.suspended ? 0 : n%17 == 0 ? 3 : n%19 == 0 ? .00001f : 50.f/30;
                    last.cameraX = n%128 < 64 ? 5 : 15; last.cameraY = n%80 < 40 ? 5 : -3;
                    if (n >= 300 && n < 400) last.cameraX = 3000;
                    if (!last.suspended && last.canSeeWater) {
                        T::m_FrameCounter = last.frame; T::step = last.timeStep;
                        W::UpdateFlow(); // verbatim extracted source incl 29/32 phase
                        b.currentFlow = {W::m_CurrentFlow.x,W::m_CurrentFlow.y};
                        b.flowTimeStep = last.timeStep; ReferenceUV(b);
                    }
                    Require(actual.Advance(last,a), "consecutive original flow simulation tick");
                    Require(SameSelection(actual.GetSelection(),selected) && SameFlow(a,b), "source UpdateFlow/UV differential exact");
                    ++n; ++checked;
                }
                if (n) {
                    const auto old = a;
                    Require(actual.Advance(last,a) && SameFlow(a,old), "extra presentation does not advance source flow");
                }
            }
            if (fps == 15) baseline = a; else Require(SameFlow(a,baseline), "15/30/60/144 presentation cadence invariant");
            const auto old = a;
            auto invalid = last; invalid.frame += 2;
            Require(!actual.Advance(invalid,a) && SameFlow(a,old), "missing source tick rejected atomically");
            invalid = last; invalid.cameraX += 1;
            Require(!actual.Advance(invalid,a) && SameFlow(a,old), "conflicting duplicate rejected");
            invalid = last; ++invalid.frame; invalid.timeStep = 3.001f;
            Require(!actual.Advance(invalid,a), "unclipped timestep rejected");
            invalid.timeStep = std::numeric_limits<float>::quiet_NaN(); Require(!actual.Advance(invalid,a), "NaN timestep rejected");
            invalid.timeStep = 1; invalid.cameraX = INFINITY; Require(!actual.Advance(invalid,a), "nonfinite camera rejected");
            invalid.cameraX = 5; a.accumulateFlow = true;
            Require(!actual.Advance(invalid,a), "explicit accumulation cannot double-step owned flow"); a.accumulateFlow = false;
            Require(SameFlow(a,old), "invalid calls preserve all flow fields");
            totalSelections += selections;
        }
    }
    W::nearestCallback = {};
    std::printf("water-flow-ticks checked=%zu selections=%zu source-exact fps=15,30,60,144 source-pause/native-suspend/area/32-phase/frame-ms-wrap/idempotence/gap-reject-ok\n",
        checked,totalSelections);
}

static void CheckFeedback(RealtimeEnvironment& env, uint32_t ms, bool distant = false, bool flowing = false,
    const RealtimeWaterState* snapshot = nullptr, bool ocean = false) {
    auto state = snapshot ? *snapshot : RealtimeWaterState{};
    if (flowing) {
        state.accumulateFlow = true; state.firstFlowUV = state.secondFlowUV = {.99999f,-.25f};
        state.currentFlow = {.5f,-.5f}; state.flowTimeStep = 1;
        Require(env.SetWaterState(state),"feedback seed source-flow fixture");
    }
    state.gameMs = ms;
    Require(env.SetWaterState(state), "feedback clock");
    state = env.GetWaterState();
    // Authored gaz_pier2 water quad [736,904]x[-1896,-1864]: all four
    // heights=0, big=.19900, small=.24100. Observe actual emitted GL vertices.
    const WaterPoly* body = nullptr;
    for (const auto& p : env.GetWaterData().polys)
        if (p.nverts == 4 && p.v[0].x == 736 && p.v[0].y == -1896) body = &p;
    Require(body != nullptr, "authored pier water body");
    for (const auto& v : body->v)
        Require(v.z == 0 && v.bigWaves == .199f && v.smallWaves == .241f, "authored body parameters");
    glViewport(0, 0, kWidth, kHeight);
    const float left = ocean ? (distant ? -3505 : -3270) : distant ? 730 : 800;
    const float bottom = ocean ? (distant ? -5 : 236) : distant ? -1900 : -1894;
    const float width = distant ? (ocean ? 510 : 180) : 40, height = distant ? (ocean ? 510 : 40) : 24;
    const float cx = ocean ? -3250 : 820, cy = ocean ? 250 : -1880;
    glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(left, left+width, bottom, bottom+height, -10, 10);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    std::vector<float> feedback(1000000);
    glFeedbackBuffer(static_cast<GLsizei>(feedback.size()), GL_3D_COLOR_TEXTURE, feedback.data());
    glRenderMode(GL_FEEDBACK);
    if (ocean) {
        const std::array blocks{RealtimeWaterBlock{-1,6}};
        env.DrawWater(cx,distant ? 850 : cy,false,blocks);
    } else env.DrawWater(cx, distant ? -1780 : cy);
    const int count = glRenderMode(GL_RENDER);
    Require(count > 0, "GL geometry feedback capacity");
    size_t checked = 0;
    float zError = 0, uvError = 0, colorError = 0;
    const int secondAlpha = int(env.GetParams().water[3] * 255 * .5f);
    const int firstAlpha = std::min(255, (secondAlpha << 8) / (256 - secondAlpha));
    const double phase1 = double(ms & 4095) * .0015339808305725455;
    const double phase2 = double(ms & 8191) * .0007669904152862728;
    const float shifts[2][2]{{float(double(state.firstFlowUV[0]) + std::sin(phase1) * double(.08f) * double(.3f)),
                              float(double(state.firstFlowUV[1]) + std::cos(phase1) * double(.08f) * double(.3f))},
                             {state.secondFlowUV[0], float(double(state.secondFlowUV[1]) + std::cos(phase2) * double(.04f) * double(.6f))}};
    for (int cursor = 0; cursor < count;) {
        Require(feedback[cursor++] == GL_POLYGON_TOKEN, "triangle feedback token");
        const int vertices = int(feedback[cursor++]);
        for (int i = 0; i < vertices; ++i, cursor += 11) {
            const float* v = &feedback[cursor];
            const float x = left + v[0] / kWidth * width, y = bottom + v[1] / kHeight * height;
            if (distant) {
                if ((std::abs(x-(ocean ? -3500 : 736)) > .001f && std::abs(x-(ocean ? -3000 : 904)) > .001f) ||
                    (std::abs(y-(ocean ? 0 : -1896)) > .001f && std::abs(y-(ocean ? 500 : -1864)) > .001f)) continue;
            } else if (x <= left+.01f || x >= left+width-.01f || y <= bottom+.01f || y >= bottom+height-.01f) continue;
            Require(std::abs(x-std::round(x/2)*2) < .001f && std::abs(y-std::round(y/2)*2) < .001f,
                "source 2m detail grid");
            const float attenuation = std::clamp((1-std::hypot(x-cx, y-cy)/48)*2, 0.f, 1.f);
            const auto ref = distant ? RealtimeWaterSample{0, .577f, 0, {0,0,1}} :
                Oracle(int(std::round(x)), int(std::round(y)), 0, (ocean ? 1 : body->v[0].bigWaves) * attenuation,
                     (ocean ? 0 : body->v[0].smallWaves) * attenuation, state);
            zError = std::max(zError, std::abs((.5f-v[2])*20 - ref.z));
            const int alpha = int(std::round(v[6]*255));
            Require(alpha == firstAlpha || alpha == secondAlpha, "source layer alpha geometry");
            const int layer = alpha == firstAlpha ? 0 : 1;
            const float scale = layer ? .04f : .08f, size = layer ? 12.5f : 25.f;
            for (int c = 0; c < 2; ++c) {
                const float coordinate = c ? y : x;
                const float delta = v[7+c] - ((distant ? coordinate/size : coordinate*scale) + shifts[layer][c]);
                uvError = std::max(uvError, std::abs(delta-std::round(delta)));
            }
            for (int c = 0; c < 3; ++c) {
                const float expected = int(int(env.GetParams().water[c]*255)*ref.colorMult)/255.f;
                colorError = std::max(colorError, std::abs(v[3+c]-expected));
            }
            ++checked;
        }
    }
    std::printf("water-geometry %s ms=%u vertices=%zu z-error=%g uv-error=%g rgba-error=%g\n",
        ocean ? (distant ? "ocean-far" : "ocean-near") : distant ? "far" : flowing ? "near-flow" : snapshot ? "near-selected-flow" : "near", ms, checked, zError, uvError, colorError);
    Require(checked >= (distant ? 12u : 100u) && zError < .0001f && uvError < .0001f && colorError < .0001f,
        "GL geometry/UV/color source oracle");
}

static void CheckUpload(RealtimeEnvironment& env, char* error, size_t errorSize) {
    GLuint buffer = 0;
    glGenBuffers(1, &buffer); glBindBuffer(GL_PIXEL_UNPACK_BUFFER, buffer);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 8); glPixelStorei(GL_UNPACK_ROW_LENGTH, 19);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 3); glPixelStorei(GL_UNPACK_SKIP_PIXELS, 2);
    glPixelTransferf(GL_RED_SCALE, .3f); glPixelTransferf(GL_RED_BIAS, .2f); glPixelTransferi(GL_MAP_COLOR, GL_TRUE);
    Require(env.Upload(error, errorSize), error);
    GLint actual = 0;
    glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &actual); Require(GLuint(actual) == buffer, "upload restores PBO");
    for (auto pair : {std::array<GLint,2>{GL_UNPACK_ALIGNMENT,8}, {GL_UNPACK_ROW_LENGTH,19},
         {GL_UNPACK_SKIP_ROWS,3}, {GL_UNPACK_SKIP_PIXELS,2}, {GL_MAP_COLOR,GL_TRUE}}) {
        glGetIntegerv(pair[0], &actual); Require(actual == pair[1], "upload preserves pixel modes");
    }
    GLfloat value = 0;
    glGetFloatv(GL_RED_SCALE, &value); Require(value == .3f, "upload restores pixel scale");
    glGetFloatv(GL_RED_BIAS, &value); Require(value == .2f, "upload restores pixel bias");
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0); glDeleteBuffers(1, &buffer);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4); glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0); glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    glPixelTransferf(GL_RED_SCALE, 1); glPixelTransferf(GL_RED_BIAS, 0); glPixelTransferi(GL_MAP_COLOR, GL_FALSE);
    // CheckPixels subsequently verifies that the GPU texture itself is exact.
    std::puts("water-upload hostile-PBO/pixel-transfer/state-ok");
}

static void CheckState(RealtimeEnvironment& env) {
    auto snapshot = [] {
        std::vector<float> values;
        for (GLenum name : {GL_CURRENT_PROGRAM, GL_ACTIVE_TEXTURE, GL_MATRIX_MODE, GL_DEPTH_TEST, GL_DEPTH_FUNC,
             GL_DEPTH_WRITEMASK, GL_ALPHA_TEST, GL_ALPHA_TEST_FUNC, GL_ALPHA_TEST_REF, GL_BLEND, GL_BLEND_SRC_RGB,
             GL_BLEND_DST_RGB, GL_BLEND_EQUATION_RGB, GL_BLEND_EQUATION_ALPHA, GL_FOG, GL_FOG_MODE, GL_FOG_START,
             GL_FOG_END, GL_FOG_COORDINATE_SOURCE, GL_CULL_FACE, GL_LIGHTING, GL_SHADE_MODEL}) {
            GLfloat v = 0; glGetFloatv(name, &v); values.push_back(v);
        }
        auto array = [&](GLenum name, size_t n) {
            const auto start = values.size(); values.resize(start+n); glGetFloatv(name, &values[start]);
        };
        array(GL_CURRENT_COLOR, 4); array(GL_CURRENT_NORMAL, 3); array(GL_FOG_COLOR, 4);
        array(GL_MODELVIEW_MATRIX, 16); array(GL_PROJECTION_MATRIX, 16);
        GLint active = 0, units = 0; glGetIntegerv(GL_ACTIVE_TEXTURE, &active); glGetIntegerv(GL_MAX_TEXTURE_UNITS, &units);
        for (int i = 0; i < units; ++i) {
            glActiveTexture(GL_TEXTURE0+i);
            array(GL_TEXTURE_MATRIX, 16);
            for (GLenum name : {GL_TEXTURE_2D, GL_TEXTURE_BINDING_2D, GL_TEXTURE_GEN_S, GL_TEXTURE_GEN_T}) array(name, 1);
        }
        glActiveTexture(active);
        return values;
    };
    env.BeginWorld(); // restoration includes the existing world shader
    glEnable(GL_ALPHA_TEST); glAlphaFunc(GL_GREATER, .4f);
    glBlendEquationSeparate(GL_FUNC_REVERSE_SUBTRACT, GL_FUNC_SUBTRACT);
    glActiveTexture(GL_TEXTURE1); glEnable(GL_TEXTURE_2D);
    glMatrixMode(GL_TEXTURE); glPushMatrix(); glTranslatef(.3f, .4f, 0);
    const auto before = snapshot();
    env.DrawWater(820, -1880);
    Require(before == snapshot(), "water GL state/program/texture-matrix restoration");
    RealtimeSeaBedGeometry bed;
    const std::array blocks{RealtimeWaterBlock{0,6}};
    Require(RealtimeEnvironment::BuildSeaBed(blocks,-2990,250,0,bed),"state seabed build");
    env.DrawSeaBed(bed);
    Require(before == snapshot(),"seabed GL state/program/texture-matrix restoration");
    glPopMatrix();
    env.EndWorld();
    glMatrixMode(GL_MODELVIEW); glActiveTexture(GL_TEXTURE0);
    Require(glGetError() == GL_NO_ERROR, "water state GL errors");
    std::printf("water-state restored-values=%zu seabed-state-restored=%zu\n", before.size(),before.size());
}

static void CheckPixels(RealtimeEnvironment& env) {
    RealtimeWaterState state{};
    state.gameMs = 1700;
    Require(env.SetWaterState(state), "pixel clock");
    const auto ref = Oracle(820, -1880, 0, .199f, .241f, state);
    const auto& params = env.GetParams();
    const auto& image = env.GetWaterImage();
    // Independent GL_LINEAR + GL_REPEAT oracle against the decoded real TXD,
    // evaluated at an exact 2m-grid vertex in a one-pixel orthographic view.
    auto texel = [&](float u, float v, int channel) {
        const float x = (u-std::floor(u))*image.w-.5f, y = (v-std::floor(v))*image.h-.5f;
        const int ix = int(std::floor(x)), iy = int(std::floor(y));
        auto byte = [&](int px, int py) {
            px = (px%image.w+image.w)%image.w; py = (py%image.h+image.h)%image.h;
            return image.rgba[(py*image.w+px)*4+channel]/255.f;
        };
        return std::lerp(std::lerp(byte(ix,iy), byte(ix+1,iy), x-ix),
                         std::lerp(byte(ix,iy+1), byte(ix+1,iy+1), x-ix), y-iy);
    };
    const float firstPhase = float(state.gameMs & 4095);
    const float secondPhase = float(state.gameMs & 8191);
    const float shifts[2][2]{{float(std::sin(firstPhase*.0015339808305725455)*double(.08f)*double(.3f)),
                              float(std::cos(firstPhase*.0015339808305725455)*double(.08f)*double(.3f))},
                             {0, float(std::cos(secondPhase*.0007669904152862728)*double(.04f)*double(.6f))}};
    const int a2 = int(params.water[3]*255*.5f), a1 = std::min(255, (a2<<8)/(256-a2));
    glViewport(0, 0, 1, 1);
    glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(819,821,-1881,-1879,0,3000);
    glMatrixMode(GL_MODELVIEW);
    for (float depth : {20.f, (params.fogStart+params.farClip)*.5f, params.farClip+2}) {
        glLoadIdentity(); glTranslatef(0, 0, -depth);
        glDepthMask(GL_TRUE); glClearDepth(1); glClearColor(.2f,.3f,.4f,.5f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        std::array<unsigned char,4> pixel{}, initial{};
        glReadPixels(0,0,1,1,GL_RGBA,GL_UNSIGNED_BYTE,initial.data());
        env.DrawWater(820,-1880);
        glReadPixels(0,0,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel.data());
        std::array<float,4> expected{};
        for (int c=0;c<4;++c) expected[c]=initial[c]/255.f;
        const float fog = std::clamp((params.farClip-(depth-ref.z))/(params.farClip-params.fogStart),0.f,1.f);
        for (int layer=0;layer<2;++layer) {
            const float scale = layer ? .04f : .08f;
            const float u=820*scale+shifts[layer][0], v=-1880*scale+shifts[layer][1];
            const float alpha=(layer ? a2:a1)/255.f*texel(u,v,3);
            for (int c=0;c<4;++c) {
                float value=alpha;
                if (c<3) value=std::lerp(params.skyBottom[c], int(int(params.water[c]*255)*ref.colorMult)/255.f*texel(u,v,c),fog);
                expected[c]=std::round((value*alpha+expected[c]*(1-alpha))*255)/255;
            }
        }
        for (int c=0;c<4;++c) Require(std::abs(pixel[c]-expected[c]*255)<=2, "water TXD/alpha/fog pixel oracle");
        float z=0; glReadPixels(0,0,1,1,GL_DEPTH_COMPONENT,GL_FLOAT,&z);
        Require(z==1, "water does not write depth");
        glClearDepth(0); glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        env.DrawWater(820,-1880);
        glReadPixels(0,0,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel.data());
        Require(pixel==initial, "water respects world depth occlusion");
    }
    glClearDepth(1);
    std::puts("water-pixels real-TXD/2-layer-alpha/fog/depth-ok");
}

static std::vector<unsigned char> Pixels() {
    std::vector<unsigned char> pixels(kWidth*kHeight*4);
    glReadPixels(0, 0, kWidth, kHeight, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    return pixels;
}
static void Capture(const char* directory, const char* name, const std::vector<unsigned char>& pixels) {
    // Own application framebuffer only; no game asset is written or copied.
    const std::string path = std::string(directory) + "/RealtimeWaterProbe-" + name + ".ppm";
    FILE* f = std::fopen(path.c_str(), "wb");
    Require(f != nullptr, "capture output");
    std::fprintf(f, "P6\n%d %d\n255\n", kWidth, kHeight);
    for (int y = kHeight-1; y >= 0; --y) for (int x = 0; x < kWidth; ++x)
        Require(std::fwrite(&pixels[(y*kWidth+x)*4], 1, 3, f) == 3, "capture write");
    Require(std::fclose(f) == 0, "capture close");
}

static void CheckSeaBedGpu(RealtimeEnvironment& env, const char* output) {
    RealtimeWaterState sourceStartup{}; sourceStartup.gameMs=1700;
    Require(env.SetWaterState(sourceStartup),"seabed captures use actual zero-flow startup, not preceding residual-flow fixture");
    size_t checked = 0;
    float worst = 0;
    glViewport(0,0,kWidth,kHeight);
    // Actual immediate-mode output versus exact-rational source fixtures,
    // including source winding/indices, static color, and near/far UV geometry.
    for (const auto& ref : water_oracle::seaBedFixtures) {
        RealtimeSeaBedGeometry bed;
        const std::array blocks{RealtimeWaterBlock{int16_t(ref.x),int16_t(ref.y)}};
        Require(env.BuildSeaBed(blocks,ref.cx,ref.cy,ref.area,bed),"seabed feedback build");
        const float left=ref.x*500-3001, bottom=ref.y*500-3001;
        glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(left,left+502,bottom,bottom+502,-200,200);
        glMatrixMode(GL_MODELVIEW); glLoadIdentity();
        std::array<float,2048> feedback{};
        glFeedbackBuffer(feedback.size(),GL_3D_COLOR_TEXTURE,feedback.data()); glRenderMode(GL_FEEDBACK);
        env.DrawSeaBed(bed);
        const int count=glRenderMode(GL_RENDER);
        Require(count>=0,"seabed feedback capacity");
        size_t offset=0, triangle=0;
        constexpr int indices[]{0,1,2,3,1,2};
        while (offset<size_t(count)) {
            Require(feedback[offset++]==GL_POLYGON_TOKEN && feedback[offset++]==3,"seabed triangle topology");
            for (int i=0;i<3;++i) {
                const auto& v=ref.vertices.at((triangle/2)*4+indices[(triangle%2)*3+i]);
                const float expected[]{(v.x-left)/502*kWidth,(v.y-bottom)/502*kHeight,.675f,
                    80.f/255,80.f/255,80.f/255,1,v.u,v.v,0,1};
                for (int field=0;field<11;++field) {
                    const float error=std::abs(feedback[offset++]-expected[field]);
                    worst=std::max(worst,error);
                    Require(error<.001f,"seabed actual GL source geometry/color/UV oracle");
                }
                ++checked;
            }
            ++triangle;
        }
        Require(triangle==ref.vertices.size()/2,"seabed actual GL source triangle count");
    }
    // One real texel footprint, with source color (not water/timecyc RGB).
    const auto& image=env.GetSeaBedImage();
    size_t alphaZero=0,alphaPartial=0;
    for (size_t i=3;i<image.rgba.size();i+=4) {
        alphaZero+=image.rgba[i]==0; alphaPartial+=image.rgba[i]>0&&image.rgba[i]<255;
    }
    std::printf("seabed-asset name=%s size=%dx%d rgba-fnv=%016llx alpha-zero=%zu partial=%zu\n",
        image.name,image.w,image.h,(unsigned long long)Hash(image.rgba),alphaZero,alphaPartial);
    Require(std::strcmp(image.name,"seabd32")==0,"real seabed texture dictionary name");
    Require(alphaZero==0 && alphaPartial==0,"installed seabed texture is opaque");
    const std::array blocks{RealtimeWaterBlock{0,6}};
    RealtimeSeaBedGeometry bed;
    Require(env.BuildSeaBed(blocks,-2990,123.5f,0,bed),"seabed pixel build");
    const float u=.16f,v=1.976f;
    const auto texel = [&](int channel) {
        const float tx=(u-std::floor(u))*image.w-.5f,ty=(v-std::floor(v))*image.h-.5f;
        const int ix=int(std::floor(tx)),iy=int(std::floor(ty));
        const auto at = [&](int x,int y) {
            return image.rgba[(((y+image.h)%image.h)*image.w+(x+image.w)%image.w)*4+channel]/255.f;
        };
        return std::lerp(std::lerp(at(ix,iy),at(ix+1,iy),tx-ix),std::lerp(at(ix,iy+1),at(ix+1,iy+1),tx-ix),ty-iy);
    };
    glViewport(0,0,1,1);
    glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(-2991,-2989,122.5,124.5,0,3000);
    glMatrixMode(GL_MODELVIEW);
    const auto& params=env.GetParams();
    int pixelWorst=0;
    for (float eyeDepth : {20.f,70.f,(params.fogStart+params.farClip)*.5f,params.farClip+2}) {
        glLoadIdentity(); glTranslatef(0,0,70-eyeDepth);
        glDepthMask(GL_TRUE); glClearDepth(1); glClearColor(.2f,.3f,.4f,.5f);
        glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
        std::array<unsigned char,4> initial{},pixel{};
        glReadPixels(0,0,1,1,GL_RGBA,GL_UNSIGNED_BYTE,initial.data());
        glDepthMask(GL_FALSE); // caller state must not disable the source floor depth writes
        env.DrawSeaBed(bed);
        glReadPixels(0,0,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel.data());
        const float fog=std::clamp((params.farClip-eyeDepth)/(params.farClip-params.fogStart),0.f,1.f);
        for (int c=0;c<4;++c) {
            const int expected=c==3 ? 255 : int(std::round(std::lerp(params.skyBottom[c],80.f/255*texel(c),fog)*255));
            pixelWorst=std::max(pixelWorst,std::abs(int(pixel[c])-expected));
            Require(std::abs(int(pixel[c])-expected)<=2,"seabed real TXD/constant-RGBA/fog pixel oracle");
        }
        GLfloat depth=0; glReadPixels(0,0,1,1,GL_DEPTH_COMPONENT,GL_FLOAT,&depth);
        Require(std::abs(depth-eyeDepth/3000)<2.e-7f,"seabed writes original -70 plane depth");
        GLboolean mask=GL_TRUE; glGetBooleanv(GL_DEPTH_WRITEMASK,&mask); Require(!mask,"seabed restores depth write mask");
        glDepthMask(GL_TRUE); glClearDepth(0); glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
        env.DrawSeaBed(bed);
        glReadPixels(0,0,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel.data());
        Require(pixel==initial,"seabed respects opaque world depth occlusion");
    }
    glClearDepth(1); glDepthMask(GL_TRUE);
    std::printf("seabed-GL fixture-vertices=%zu max-feedback-error=%g max-pixel-byte-error=%d TXD/alpha/fog/depth/occlusion-ok\n",checked,worst,pixelWorst);
    // Actual water.dat at the west ocean boundary; no fabricated beach mesh.
    // Only render views where the source floor exists. The source floor does
    // not move at the water surface boundary; underwater post-FX are separate.
    for (int view=0;view<4;++view) {
        Camera camera;
        camera.x=-2990; camera.y=123.5f; camera.yaw=0; camera.pitch=-1.4f;
        camera.z=view==0?4: view==1?100: view==2?-2:-71;
        if (view==3) camera.pitch=1.4f; // source cullNONE: underside is visible
        const auto render = [&](bool seaBed,int area) {
            camera.Apply(kWidth,kHeight,params.farClip);
            glDepthMask(GL_TRUE); glClearColor(.2f,.3f,.4f,1); glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
            env.DrawSky(camera.x,camera.y,camera.z);
            if (seaBed) Require(env.DrawSeaBed(camera.x,camera.y,area),"seabed actual perspective/frustum/area");
            env.DrawWater(camera.x,camera.y);
            glFinish(); return Pixels();
        };
        const auto baseline=render(false,0), actual=render(true,0), repeated=render(true,0), hidden=render(true,1), area5=render(true,5);
        Require(actual==repeated && hidden==baseline && area5==actual,"seabed stable presentation and source area gate");
        // Independent visibility coverage oracle: submit a generous complete
        // 9x9 block grid (one block per call avoids the source list's 70 cap),
        // letting GL clip it. It must reproduce the frustum-scanned floor.
        // This tests actual pixels, not a second copy of the hull scanner.
        camera.Apply(kWidth,kHeight,params.farClip);
        glDepthMask(GL_TRUE); glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
        env.DrawSky(camera.x,camera.y,camera.z);
        for (int bx=-4;bx<=4;++bx) for (int by=2;by<=10;++by) {
            const std::array candidate{RealtimeWaterBlock{int16_t(bx),int16_t(by)}};
            RealtimeSeaBedGeometry full;
            Require(env.BuildSeaBed(candidate,camera.x,camera.y,0,full),"independent full-grid visibility oracle");
            env.DrawSeaBed(full);
        }
        env.DrawWater(camera.x,camera.y);
        Require(actual==Pixels(),"frustum-scanned seabed pixels equal independent unculled block coverage");
        size_t changed=0;
        for (size_t i=0;i<actual.size();i+=4) changed+=!std::equal(actual.begin()+i,actual.begin()+i+3,baseline.begin()+i);
        Require(changed>100,"real ocean boundary seabed pass changes pixels");
        auto state=env.GetWaterState(); state.gameMs+=1700; Require(env.SetWaterState(state),"seabed independent clock");
        // Compare the bed-only pass at fixed view to avoid surface-wave changes.
        camera.Apply(kWidth,kHeight,params.farClip); glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
        Require(env.DrawSeaBed(camera.x,camera.y),"bed only time A"); const auto a=Pixels();
        state.gameMs+=1700; Require(env.SetWaterState(state),"seabed independent second clock");
        glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT); Require(env.DrawSeaBed(camera.x,camera.y),"bed only time B");
        Require(a==Pixels(),"seabed remains static across real game-time changes");
        const char* names[]{"seabed-close","seabed-far","seabed-underwater","seabed-underside"};
        Capture(output,names[view],actual);
        std::printf("seabed-boundary %s camera-z=%g changed=%zu fnv=%016llx same-clock/area0/area5/hidden-area/static-floor-ok\n",
            names[view],camera.z,changed,(unsigned long long)Hash(actual));
    }
    Require(glGetError()==GL_NO_ERROR,"seabed GL final error");
}
static void CheckTriangles(RealtimeEnvironment& env, const char* output) {
    int bodies = 0;
    for (const auto& body : env.GetWaterData().polys) {
        if (body.nverts != 3) continue;
        ++bodies;
        Require(body.flags == 1, "six authored exterior/visible triangle flags");
        const auto& a = body.v[0]; const auto& b = body.v[1]; const auto& c = body.v[2];
        const double determinant = (b.y-c.y)*(a.x-c.x)+(c.x-b.x)*(a.y-c.y);
        auto barycentric = [&](double x, double y) {
            const double u = ((b.y-c.y)*(x-c.x)+(c.x-b.x)*(y-c.y))/determinant;
            const double v = ((c.y-a.y)*(x-c.x)+(a.x-c.x)*(y-c.y))/determinant;
            return std::array{u, v, 1-u-v};
        };
        const float cx = (a.x+b.x+c.x)/3, cy = (a.y+b.y+c.y)/3;
        const float minX = std::floor((cx-48)/2)*2, maxX = std::ceil((cx+48)/2)*2;
        const float minY = std::floor((cy-48)/2)*2, maxY = std::ceil((cy+48)/2)*2;
        const float left = std::min({a.x,b.x,c.x})-2, bottom = std::min({a.y,b.y,c.y})-2;
        const float width = std::max({a.x,b.x,c.x})-left+2, height = std::max({a.y,b.y,c.y})-bottom+2;
        // Independent convex clipping determines the exact area that MUST have
        // 2m detail cells. It does not use the renderer's recursive split code.
        std::vector<std::array<double,2>> polygon{{a.x,a.y},{b.x,b.y},{c.x,c.y}};
        for (int plane = 0; plane < 4; ++plane) {
            const int axis = plane/2;
            const double boundary = axis ? (plane%2 ? maxY : minY) : (plane%2 ? maxX : minX);
            const auto before = polygon; polygon.clear();
            auto inside = [&](const auto& p) { return plane%2 ? p[axis] <= boundary : p[axis] >= boundary; };
            for (size_t i = 0; i < before.size(); ++i) {
                const auto p = before[i], q = before[(i+1)%before.size()];
                if (inside(p)) polygon.push_back(p);
                if (inside(p) != inside(q)) {
                    const double t = (boundary-p[axis])/(q[axis]-p[axis]);
                    polygon.push_back({p[0]+(q[0]-p[0])*t,p[1]+(q[1]-p[1])*t});
                }
            }
        }
        double detailArea = 0;
        for (size_t i = 0; i < polygon.size(); ++i) {
            const auto p = polygon[i], q = polygon[(i+1)%polygon.size()];
            detailArea += p[0]*q[1]-q[0]*p[1];
        }
        detailArea = std::abs(detailArea)/2;
        glViewport(0, 0, kWidth, kHeight);
        glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(left,left+width,bottom,bottom+height,-10,10);
        glMatrixMode(GL_MODELVIEW); glLoadIdentity();
        std::vector<unsigned char> previous;
        for (uint32_t ms : {0u,1700u}) {
            RealtimeWaterState state{}; state.gameMs = ms;
            Require(env.SetWaterState(state), "triangle simulation clock");
            std::vector<float> feedback(2000000);
            glFeedbackBuffer(static_cast<GLsizei>(feedback.size()), GL_3D_COLOR_TEXTURE, feedback.data());
            glRenderMode(GL_FEEDBACK); env.DrawWater(cx,cy);
            const int count = glRenderMode(GL_RENDER);
            Require(count > 0, "triangle GL feedback capacity");
            double totalArea = 0, nearArea = 0;
            size_t triangles = 0, nearTriangles = 0, vertices = 0;
            float zError = 0, uvError = 0;
            const int secondAlpha = int(env.GetParams().water[3]*255*.5f);
            const int firstAlpha = std::min(255,(secondAlpha<<8)/(256-secondAlpha));
            for (int cursor = 0; cursor < count;) {
                Require(feedback[cursor++] == GL_POLYGON_TOKEN, "triangle polygon feedback");
                const int n = int(feedback[cursor++]);
                const auto* points = &feedback[cursor]; cursor += n*11;
                double x = 0, y = 0;
                for (int i = 0; i < n; ++i) { x += left+points[i*11]/kWidth*width; y += bottom+points[i*11+1]/kHeight*height; }
                x /= n; y /= n;
                const auto center = barycentric(x,y);
                if (*std::min_element(center.begin(),center.end()) < .00001) continue;
                Require(n == 3, "authored triangle has no clipped internal geometry");
                const bool near = x>minX && x<maxX && y>minY && y<maxY;
                double area = 0;
                for (int i = 0; i < 3; ++i) {
                    const float* p = points+i*11; const float* q = points+((i+1)%3)*11;
                    const double px = left+p[0]/kWidth*width, py = bottom+p[1]/kHeight*height;
                    const double qx = left+q[0]/kWidth*width, qy = bottom+q[1]/kHeight*height;
                    area += px*qy-qx*py;
                    const auto weights = barycentric(px,py);
                    Require(*std::min_element(weights.begin(),weights.end()) > -.00001, "no geometry crosses authored hypotenuse");
                    const int ix = int(std::round(px)), iy = int(std::round(py));
                    Require(std::abs(px-ix)<.001 && std::abs(py-iy)<.001, "source integer water grid");
                    if (near) Require(ix%2==0 && iy%2==0, "source triangle signed 2m steps");
                    // An independent affine plane from authored vertices checks
                    // every split's parameter routing, including the variable-
                    // amplitude first triangle. Waves/normals come from the
                    // verbatim extracted 6E6EF0 oracle, not SampleWater.
                    const auto w = barycentric(ix,iy);
                    auto interpolate = [&](float WaterVert::* field) { return float(w[0]*(a.*field)+w[1]*(b.*field)+w[2]*(c.*field)); };
                    const float radius = std::sqrt((ix-cx)*(ix-cx)+(iy-cy)*(iy-cy))/48;
                    const float attenuation = near ? std::clamp((1-radius)*2,0.f,1.f) : 0;
                    const auto ref = Oracle(ix,iy,interpolate(&WaterVert::z),interpolate(&WaterVert::bigWaves)*attenuation,
                        interpolate(&WaterVert::smallWaves)*attenuation,state);
                    zError = std::max(zError,std::abs((.5f-p[2])*20-ref.z));
                    const int alpha = int(std::round(p[6]*255));
                    Require(alpha==firstAlpha || alpha==secondAlpha,"triangle source two-layer alpha");
                    const int layer = alpha==firstAlpha ? 0 : 1;
                    const double phase = double(ms & (layer ? 8191 : 4095))*(layer ? .0007669904152862728 : .0015339808305725455);
                    const float shift[2]{layer ? 0 : float(std::sin(phase)*double(.08f)*double(.3f)),
                        float(std::cos(phase)*double(layer ? .04f : .08f)*double(layer ? .6f : .3f))};
                    for (int axis = 0; axis < 2; ++axis) {
                        const float coordinate = float(axis ? iy : ix);
                        const float expected = (near ? coordinate*(layer ? .04f : .08f) : coordinate/(layer ? 12.5f : 25.f))+shift[axis];
                        const float delta = p[7+axis]-expected;
                        uvError = std::max(uvError,std::abs(delta-std::round(delta)));
                    }
                    // The 60m body fits wholly in detail with n=30: no rectangle
                    // children. Retail 7232A6 fixes its RGB multiplier to .577.
                    if (b.x-a.x == 60) for (int ch = 0; ch < 3; ++ch)
                        Require(std::abs(p[3+ch]-int(int(env.GetParams().water[ch]*255)*.577f)/255.f)<.00001f,
                            "source high-detail triangle color override");
                    ++vertices;
                }
                area = std::abs(area)/2;
                totalArea += area; if (near) { nearArea += area; ++nearTriangles; Require(std::abs(area-2)<.002,"source detail triangle cell area=2"); }
                ++triangles;
            }
            std::printf("water-triangle body=%d ms=%u triangles=%zu detail=%zu vertices=%zu area=%.5f/%.5f detail-area=%.5f/%.5f z-error=%g uv-error=%g\n",
                bodies,ms,triangles,nearTriangles,vertices,totalArea/2,std::abs(determinant)/2,nearArea/2,detailArea,zError,uvError);
            Require(std::abs(totalArea-std::abs(determinant))<.5 && std::abs(nearArea/2-detailArea)<.5 && nearTriangles>100,
                "authored area and camera detail boundary coverage");
            if (b.x-a.x == 60) Require(triangles==1800,"source n*n triangle tessellation, two layers");
            Require(zError<.00002f && uvError<.0002f,"all six triangle geometry/source-wave/UV oracle");
            glEnable(GL_STENCIL_TEST); glStencilMask(255); glClearStencil(0);
            glStencilFunc(GL_ALWAYS,0,255); glStencilOp(GL_KEEP,GL_KEEP,GL_INCR);
            glDepthMask(GL_TRUE); glClearColor(0,0,0,1);
            glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT|GL_STENCIL_BUFFER_BIT);
            env.DrawWater(cx,cy);
            std::vector<unsigned char> stencil(kWidth*kHeight);
            glReadPixels(0,0,kWidth,kHeight,GL_STENCIL_INDEX,GL_UNSIGNED_BYTE,stencil.data());
            size_t covered = 0;
            for (int py = 0; py < kHeight; ++py) for (int px = 0; px < kWidth; ++px) {
                const auto w = barycentric(left+(px+.5)/kWidth*width,bottom+(py+.5)/kHeight*height);
                if (*std::min_element(w.begin(),w.end())<.015) continue;
                Require(stencil[py*kWidth+px]==2,"no triangle/rectangle split holes or double-coverage seams"); ++covered;
            }
            Require(covered>10000,"triangle interior stencil coverage");
            glDisable(GL_STENCIL_TEST);
            auto pixels = Pixels();
            if (!previous.empty()) Require(pixels!=previous,"triangle texture/time phase changes");
            previous = pixels;
            glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT); env.DrawWater(cx,cy);
            Require(Pixels()==pixels,"same-clock triangle render deterministic");
            char name[64]; std::snprintf(name,sizeof(name),"triangle-%d-%u",bodies,ms); Capture(output,name,pixels);
            std::printf("water-triangle-pixels body=%d ms=%u covered=%zu fnv=%016llx seams=0\n",bodies,ms,covered,(unsigned long long)Hash(pixels));
        }
    }
    Require(bodies==6,"all six authored triangles exercised");
}

static void CheckSelectedFlowGpu(RealtimeEnvironment& env, const char* output) {
    RealtimeWaterState zero{}; zero.gameMs = 1700;
    Require(env.SetWaterState(zero), "owned selector zero startup");
    RealtimeWaterFlowTick tick{};
    tick.cameraX = 820; tick.cameraY = -1880; tick.timeStep = 50.f/30;
    for (uint32_t i = 0; i < 600; ++i) {
        tick.frame = i; tick.gameMs = i*1000/30;
        Require(env.AdvanceWaterFlow(tick), "actual zero-flow source tick");
    }
    Require(env.GetWaterState().currentFlow == zero.currentFlow && env.GetWaterState().firstFlowUV == zero.firstFlowUV &&
        env.GetWaterState().secondFlowUV == zero.secondFlowUV, "real asset startup never invents flow");
    Require(env.GetWaterFlowSelection().result == RealtimeWaterFlowSelection::Result::SelectedQuad &&
        env.GetWaterFlowSelection().desired == zero.currentFlow, "owned nearest source body selected");
    // Nonzero residual CURRENT is a labeled state fixture, not fabricated asset
    // flow. Actual water.dat selects desired=(0,0); extracted UpdateFlow decays
    // this snapshot while RenderWater advances UV at a fixed wave clock.
    auto expected = zero; expected.currentFlow = {.5f,-.5f};
    Require(env.SetWaterState(expected), "seed explicit residual-current fixture");
    auto render = [&] {
        glViewport(0,0,kWidth,kHeight);
        glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(800,840,-1894,-1870,1,3000);
        glMatrixMode(GL_MODELVIEW); glLoadIdentity(); glTranslatef(0,0,-20);
        glDepthMask(GL_TRUE); glClearColor(0,0,0,1); glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
        env.DrawWater(820,-1880); glFinish();
        Require(glGetError() == GL_NO_ERROR,"selected-flow real GL errors");
        return Pixels();
    };
    const auto before = render();
    const auto wave = env.SampleWater(820,-1880,0,.199f,.241f);
    using W = water_oracle::CWaterLevel;
    W::m_CurrentFlow = {.5f,-.5f}; W::m_CurrentDesiredFlow = {};
    FlowReference reference(env.GetWaterData());
    W::nearestCallback = [&] {
        const auto r = reference.Find(tick.cameraX,tick.cameraY);
        Require(r.desired == std::array{0.f,0.f}, "real body desired flow is zero");
        W::m_CurrentDesiredFlow = {r.desired[0],r.desired[1]};
    };
    for (uint32_t i = 600; i < 720; ++i) {
        tick.frame = i; tick.gameMs = i*1000/30;
        water_oracle::CTimer::m_FrameCounter = tick.frame; water_oracle::CTimer::step = tick.timeStep;
        W::UpdateFlow(); expected.currentFlow = {W::m_CurrentFlow.x,W::m_CurrentFlow.y};
        expected.flowTimeStep = tick.timeStep; ReferenceUV(expected);
        Require(env.AdvanceWaterFlow(tick) && SameFlow(env.GetWaterState(),expected), "owned source selection/smoothing/UV GPU input exact");
    }
    W::nearestCallback = {};
    Require(env.GetWaterState().gameMs == 1700 && env.SampleWater(820,-1880,0,.199f,.241f).z == wave.z,
        "fixed wave clock/geometry while flow UV advances");
    const auto after = render();
    for (int i = 0; i < 8; ++i) Require(env.AdvanceWaterFlow(tick), "same source tick duplicate before GL");
    Require(render() == after,"selected flow same-tick GL deterministic");
    size_t changed = 0;
    for (size_t p = 0; p < before.size(); p += 4)
        changed += !std::equal(before.begin()+p,before.begin()+p+3,after.begin()+p);
    Require(changed > 10000,"real water body pixels respond to source-derived flow UV at fixed gameMs");
    Capture(output,"selected-flow-before",before); Capture(output,"selected-flow-after",after);
    std::printf("water-selected-flow-GPU zero-startup-ticks=600 residual-fixture-ticks=120 gameMs=1700 changed=%zu current=(%a,%a) UV1=(%a,%a) UV2=(%a,%a) fnv=%016llx/%016llx\n",
        changed, expected.currentFlow[0], expected.currentFlow[1], expected.firstFlowUV[0], expected.firstFlowUV[1],
        expected.secondFlowUV[0], expected.secondFlowUV[1], (unsigned long long)Hash(before), (unsigned long long)Hash(after));
    CheckFeedback(env,1700,false,false,&expected);
    Require(env.SetWaterState({}),"restore after selected flow fixture");
}

static void CheckShore(RealtimeEnvironment& env, const WorldShotScene& scene, const GpuScene& gpu, const char* output) {
    for (bool close : {true, false}) {
        Camera camera;
        camera.x = close ? 820 : 746; camera.y = close ? -1920 : -1986;
        camera.z = close ? 4 : 55;
        camera.yaw = close ? 1.570796327f : std::atan2(120.f, 90.f);
        camera.pitch = close ? -.12f : -std::atan2(55.f, 150.f);
        auto render = [&](uint32_t ms, bool water, bool seaBed = true) {
            auto state = RealtimeWaterState{}; state.gameMs = ms;
            Require(env.SetWaterState(state), "render explicit time");
            camera.Apply(kWidth, kHeight, env.GetParams().farClip);
            glDepthMask(GL_TRUE); glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            env.DrawSky(camera.x, camera.y, camera.z);
            env.BeginWorld(); gpu.Draw(scene); env.EndWorld();
            if (water) {
                if (seaBed) Require(env.DrawSeaBed(camera.x,camera.y),"shore seabed source pass");
                env.DrawWater(camera.x, camera.y);
            }
            glFinish();
            Require(glGetError() == GL_NO_ERROR, "shore GL errors");
            return Pixels();
        };
        const auto baseline = render(0, false), first = render(0, true), second = render(1700, true), repeat = render(1700, true);
        Require(second==render(1700,true,false),"pier shore unchanged: no source seabed beneath interior blocks");
        Require(second == repeat, "game-clock deterministic render");
        size_t coverage = 0, changed = 0;
        for (size_t p = 0; p < first.size(); p += 4) {
            if (!std::equal(first.begin()+p, first.begin()+p+3, baseline.begin()+p)) ++coverage;
            if (!std::equal(first.begin()+p, first.begin()+p+3, second.begin()+p)) ++changed;
        }
        Require(coverage > 10000 && changed > 1000, "shore water coverage and actual phase changes");
        std::printf("water-shore %s world-tris=%d pixels=%zu time-changed=%zu fnv0=%016llx fnv1700=%016llx\n",
            close ? "close" : "far", scene.stats.triangles, coverage, changed,
            (unsigned long long)Hash(first), (unsigned long long)Hash(second));
        Capture(output, close ? "close-0" : "far-0", first);
        Capture(output, close ? "close-1700" : "far-1700", second);
    }
}
static void CheckPerformance(RealtimeEnvironment& env, WorldShotScene& scene) {
    constexpr int width=1280,height=720, modes=5;
    const char* names[]{"before-authored", "after-convenience", "bed-only", "ocean-only", "after-replayed"};
    using Clock=std::chrono::steady_clock;
    const auto ms=[](auto a,auto b) { return std::chrono::duration<double,std::milli>(b-a).count(); };
    RealtimeWaterState state{}; state.gameMs=1700;
    Require(env.SetWaterState(state),"performance frozen source clock");
    for (int view=0;view<3;++view) {
        const char* label=view==0 ? "cj" : view==1 ? "water" : "boundary";
        Camera camera;
        if (view==0) {
            // Production idle follow-camera: unoccluded 4.6m arm, .27 pitch,
            // source-COL ground from the supplied production log. No actor
            // animation or streaming work enters this environment-only A/B.
            camera.x=1600-4.6f*std::cos(-1.43f)*std::cos(.27f);
            camera.y=-1700-4.6f*std::sin(-1.43f)*std::cos(.27f);
            camera.z=27.303447723f+1.15f+4.6f*std::sin(.27f);
            camera.yaw=-1.43f; camera.pitch=-.27f;
        } else {
            camera.x=view==1 ? 820 : -2990; camera.y=view==1 ? -1880 : 123.5f;
            camera.z=view==1 ? 6 : 20; // production free-camera yaw/pitch defaults
            char error[512]{}; E2EPagerFrame frame{};
            scene={};
            Require(StreamPager_Update(camera.x,camera.y,camera.z,scene,frame,error,sizeof(error)),error);
        }
        GpuScene gpu; Require(gpu.Upload(scene),"performance actual production GpuScene upload");
        camera.Apply(width,height,std::max(1600.f,env.GetParams().farClip));
        std::array<RealtimeWaterBlock,70> blocks{}; size_t count=0;
        Require(env.ScanOutsideWaterBlocks(camera.x,camera.y,blocks,count),"performance actual block scan");
        const auto span=std::span{blocks.data(),count};
        RealtimeSeaBedGeometry bed;
        Require(env.BuildSeaBed(span,camera.x,camera.y,0,bed),"performance source geometry");
        GLfloat projection[16]; glGetFloatv(GL_PROJECTION_MATRIX,projection);
        std::printf("water-perf-view %s drawable=%dx%d camera=%.6f,%.6f,%.6f yaw=%.6f pitch=%.6f world-tris=%d textures=%zu timecycFar=%g projectionFar=%.6f blocks=%zu seabed-vertices=%zu seabed-tris=%zu list=",
            label,width,height,camera.x,camera.y,camera.z,camera.yaw,camera.pitch,scene.stats.triangles,scene.images.size(),
            env.GetParams().farClip,double(projection[14])/(double(projection[10])+1),count,bed.size,bed.size/2);
        for (auto b:span) std::printf("(%d,%d)",b.x,b.y);
        std::puts("");
        const auto pass=[&](int mode) {
            if (mode==1 || mode==2) Require(env.DrawSeaBed(camera.x,camera.y),"performance convenience bed");
            else if (mode==4) env.DrawSeaBed(bed);
            if (mode==1 || mode==3) env.DrawWater(camera.x,camera.y,false);
            else env.DrawWater(camera.x,camera.y,false,mode==4 ? span : std::span<const RealtimeWaterBlock>{});
        };
        const auto world=[&] {
            camera.Apply(width,height,std::max(1600.f,env.GetParams().farClip));
            glDepthMask(GL_TRUE); glClearColor(0,0,0,1); glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
            env.DrawSky(camera.x,camera.y,camera.z);
            env.BeginWorld(); gpu.Render(); env.EndWorld(); // production immutable display-list chunks
        };
        const auto pixels=[&] {
            std::vector<unsigned char> p(width*height*4);
            glReadPixels(0,0,width,height,GL_RGBA,GL_UNSIGNED_BYTE,p.data()); return p;
        };
        const auto depths=[&] {
            std::vector<float> d(width*height); glReadPixels(0,0,width,height,GL_DEPTH_COMPONENT,GL_FLOAT,d.data()); return d;
        };
        std::array<std::vector<unsigned char>,modes> images;
        std::vector<float> afterDepth;
        GLuint query; glGenQueries(1,&query);
        for (int mode=0;mode<modes;++mode) {
            world(); pass(mode); glFinish(); images[mode]=pixels();
            if (mode==1) afterDepth=depths();
            if (mode==4) Require(afterDepth==depths(),"replayed/convenience source depth exact");
            world(); pass(mode); glFinish(); Require(images[mode]==pixels(),"performance frozen frame repeat exact");
            glBeginQuery(GL_PRIMITIVES_GENERATED,query); pass(mode); glEndQuery(GL_PRIMITIVES_GENERATED);
            GLuint primitives=0; glGetQueryObjectuiv(query,GL_QUERY_RESULT,&primitives);
            size_t changed=0;
            for (size_t i=0;i<images[mode].size();i+=4)
                changed+=!std::equal(images[mode].begin()+i,images[mode].begin()+i+3,images[0].begin()+i);
            std::printf("water-perf-output %s mode=%s input-primitives=%u changed-vs-before=%zu rgba-fnv=%016llx\n",
                label,names[mode],primitives,changed,(unsigned long long)Hash(images[mode]));
        }
        Require(images[1]==images[4],"replayed/convenience source pixels exact");
        glDeleteQueries(1,&query); glGenQueries(1,&query); // query objects retain their first target
        Require(glGetError()==GL_NO_ERROR,"performance geometry/capture GL errors");
        // Microbenchmarks exclude pending GPU work; separately quantify the
        // two matrix reads, entire scanner, and CPU geometry construction.
        glFinish(); size_t checksum=0;
        for (int operation=0;operation<3;++operation) {
            const auto start=Clock::now();
            for (int i=0;i<20000;++i) {
                if (operation==0) { GLfloat p[16],m[16]; glGetFloatv(GL_PROJECTION_MATRIX,p); glGetFloatv(GL_MODELVIEW_MATRIX,m); }
                else if (operation==1) { size_t n=0; Require(env.ScanOutsideWaterBlocks(camera.x,camera.y,blocks,n),"timed scan"); checksum+=n; }
                else { RealtimeSeaBedGeometry g; Require(env.BuildSeaBed(span,camera.x,camera.y,0,g),"timed build"); checksum+=g.size; }
            }
            std::printf("water-perf-micro %s operation=%s us=%.6f checksum=%zu\n",label,
                operation==0 ? "two-matrix-queries" : operation==1 ? "scan-including-queries" : "CPU-build",ms(start,Clock::now())*1000/20000,checksum);
        }
        for (int i=0;i<100;++i) { world(); pass(i%modes); glFinish(); }
        struct Timing { double wall=0,submit=0,gpu=0; };
        std::array<std::vector<double>,modes> rounds;
        // Eight warmed interleaved rounds, reversing/rotating mode order.
        // Every frame is freshly drawn, clock and all scene/camera state fixed.
        for (int round=0;round<8;++round) for (int slot=0;slot<modes;++slot) {
            const int mode=(round+(round%2 ? modes-1-slot : slot))%modes;
            Timing sum;
            for (int i=0;i<16;++i) {
                glFinish(); const auto start=Clock::now();
                glBeginQuery(GL_TIME_ELAPSED,query);
                world(); const auto beforePass=Clock::now(); pass(mode); const auto submitted=Clock::now();
                glEndQuery(GL_TIME_ELAPSED); glFinish(); const auto end=Clock::now();
                GLuint64 elapsed=0; glGetQueryObjectui64v(query,GL_QUERY_RESULT,&elapsed);
                sum.wall+=ms(start,end); sum.submit+=ms(beforePass,submitted); sum.gpu+=double(elapsed)/1.e6;
            }
            rounds[mode].push_back(sum.wall/16);
            std::printf("water-perf-round %s round=%d mode=%s frame-ms=%.6f pass-submit-ms=%.6f gpu-ms=%.6f\n",
                label,round,names[mode],sum.wall/16,sum.submit/16,sum.gpu/16);
        }
        for (int mode=0;mode<modes;++mode) {
            auto ordered=rounds[mode]; std::sort(ordered.begin(),ordered.end());
            std::printf("water-perf-summary %s mode=%s frames=128 mean-ms=%.6f median-round-ms=%.6f min-round-ms=%.6f max-round-ms=%.6f\n",
                label,names[mode],std::accumulate(ordered.begin(),ordered.end(),0.)/ordered.size(),
                (ordered[3]+ordered[4])*.5,ordered.front(),ordered.back());
        }
        // Isolate the pass from queued production world work. glGet* may wait
        // for preceding driver work; that wait is not CPU water-generation
        // cost. Finish the identical world before starting each pass timer.
        // Interleave modes every frame here to control short-term load drift.
        for (int round=0;round<8;++round) {
            std::array<Timing,modes> totals{};
            for (int frame=0;frame<8;++frame) for (int slot=0;slot<modes;++slot) {
                const int mode=(round+(frame%2 ? modes-1-slot : slot))%modes;
                world(); glFinish();
                const auto start=Clock::now(); glBeginQuery(GL_TIME_ELAPSED,query);
                pass(mode); const auto submitted=Clock::now();
                glEndQuery(GL_TIME_ELAPSED); glFinish(); const auto end=Clock::now();
                GLuint64 elapsed=0; glGetQueryObjectui64v(query,GL_QUERY_RESULT,&elapsed);
                totals[mode].wall+=ms(start,end); totals[mode].submit+=ms(start,submitted); totals[mode].gpu+=double(elapsed)/1.e6;
            }
            for (int mode=0;mode<modes;++mode) std::printf(
                "water-perf-isolated %s round=%d mode=%s wall-ms=%.6f submit-ms=%.6f gpu-ms=%.6f\n",
                label,round,names[mode],totals[mode].wall/8,totals[mode].submit/8,totals[mode].gpu/8);
        }
        glDeleteQueries(1,&query);
        Require(glGetError()==GL_NO_ERROR,"performance GL errors");
    }
    std::puts("water-perf-ok same-context/production-world/1280x720/frozen-clock/8-interleaved-rounds/finish/GPU-timer");
}

static void CheckScan(RealtimeEnvironment& env, const char* output) {
    size_t cases=0, cells=0, capped=0, changed=0, pointChecks=0;
    // Old geometric scanner, retained only as an independent regression
    // comparator. It is NOT the source oracle; the labelled retail lift is.
    const auto legacy=[](const RealtimeWaterScanPoints& p) {
        std::vector<RealtimeWaterBlock> result;
        float low=p[0][1],high=low;
        for (auto v:p) { low=std::min(low,v[1]); high=std::max(high,v[1]); }
        for (int y=int(std::floor(low)); y<=int(std::floor(high)) && result.size()<70; ++y) {
            float l=1.e10f,r=-1.e10f;
            auto add=[&](double x) { l=std::min(l,float(x)); r=std::max(r,float(x)); };
            for (size_t i=0;i<p.size();++i) {
                auto a=p[i]; if (a[1]>=y && a[1]<=y+1) add(a[0]);
                for (size_t j=i+1;j<p.size();++j) if (a[1]!=p[j][1]) for (int edge:{y,y+1}) {
                    const double t=(double(edge)-a[1])/(double(p[j][1])-a[1]);
                    if (t>=0 && t<=1) add(double(a[0])+t*(double(p[j][0])-a[0]));
                }
            }
            if (l>r) continue;
            for (int x=int(std::floor(l));x<=int(std::floor(r)) && result.size()<70;++x)
                if (x<=0||x>=11||y<=0||y>=11) result.push_back({int16_t(x),int16_t(y)});
        }
        return result;
    };
    const auto check=[&](const RealtimeWaterScanPoints& points) {
        auto all=water_scan_proof::Scan(points);
        std::vector<RealtimeWaterBlock> expected;
        for (auto c:all) if (c[0]<=0 || c[0]>=11 || c[1]<=0 || c[1]>=11) {
            expected.push_back({int16_t(c[0]),int16_t(c[1])}); if (expected.size()==70) break;
        }
        std::array<RealtimeWaterBlock,70> actual{}; size_t n=0;
        Require(env.ScanWaterBlocks(points,actual,n),"source-valid pure scanner input");
        if (n!=expected.size() || !std::equal(expected.begin(),expected.end(),actual.begin())) {
            std::fprintf(stderr,"scan differential case=%zu got=%zu expected=%zu points=",cases,n,expected.size());
            for (auto p:points) std::fprintf(stderr,"{%a,%a},",p[0],p[1]);
            std::fprintf(stderr,"\nactual="); for (size_t i=0;i<n;++i) std::fprintf(stderr,"(%d,%d)",actual[i].x,actual[i].y);
            std::fprintf(stderr,"\nexpected="); for (auto c:expected) std::fprintf(stderr,"(%d,%d)",c.x,c.y);
            std::fprintf(stderr,"\n"); Require(false,"ordered source scan differential");
        }
        changed+=legacy(points)!=expected; cells+=n; capped+=n==70; ++cases;
    };
    // Hand-evaluated branch witnesses, not expectations obtained from either
    // implementation. Integer minY uses ceil(minY), so the first scanline is
    // the camera point alone; a geometric cell-strip scanner adds (-1,0).
    const RealtimeWaterScanPoints integerTip{{{-2,3},{2,3},{2,3},{-2,3},{0,0}}};
    const std::vector<water_scan_proof::Cell> tipCells{{0,0},{-1,1},{0,1},
        {-2,2},{-1,2},{0,2},{1,2},{-2,3},{-1,3},{0,3},{1,3},{2,3}};
    Require(water_scan_proof::Scan(integerTip)==tipCells,"hand source first-row/negative-floor/final-inclusive witness");
    check(integerTip);
    const RealtimeWaterScanPoints largeBox{{{-10,10},{10,10},{10,-10},{-10,-10},{0,0}}};
    std::array<RealtimeWaterBlock,70> prefix{}; size_t prefixCount=0;
    Require(env.ScanWaterBlocks(largeBox,prefix,prefixCount) && prefixCount==70,"known first-70 source prefix");
    for (size_t i=0;i<70;++i) Require(prefix[i]==RealtimeWaterBlock{int16_t(-10+int(i%21)),int16_t(-10+int(i/21))},
        "hand row-major cap witness including final (-4,-7)");
    check(largeBox);
    // Source-valid top/down frusta: rectangular far planes with the camera
    // inside, or duplicate projected far corners for exactly horizontal views.
    for (float shift:{-12.f,-1.f,-0.f,0.f,1.f,11.f,12.f}) for (float edge:{0.f,.125f,.99999994f,1.f,1.00000012f}) {
        const float x=shift+edge;
        check({{{x,2},{x+4,2},{x+4,-2},{x,-2},{x+2,0}}});
        check({{{x,1.25f},{x+4,1.25f},{x+4,1.25f},{x,1.25f},{x+2,1.125f}}});
        check({{{x,12},{x+4,12},{x+4,12},{x,12},{x+2,-12}}});
    }
    // Actual GL cameras around every world edge, cardinal/diagonal yaw,
    // vertical/horizontal/production pitch, both source and extended far.
    for (float far:{800.f,1600.f}) for (float x:{-3000.f,-2980.f,-2500.f,0.f,2500.f,2980.f,3000.f})
        for (float y:{-3000.f,-2980.f,-2500.f,0.f,2500.f,2980.f,3000.f})
            for (int yaw=0;yaw<8;++yaw) for (float pitch:{-1.570796327f,-.27f,0.f,.27f,1.570796327f}) {
                Camera camera; camera.x=x; camera.y=y; camera.z=20; camera.yaw=yaw*.785398163f; camera.pitch=pitch;
                camera.Apply(1280,720,far);
                RealtimeWaterScanPoints points;
                Require(env.CaptureWaterScanPoints(x,y,points),"capture real source-valid camera");
                GLfloat p[16],m[16]; glGetFloatv(GL_PROJECTION_MATRIX,p); glGetFloatv(GL_MODELVIEW_MATRIX,m);
                const RealtimeWaterFrustum f{float(double(p[14])/(double(p[10])+1)),{1.f/p[0],1.f/p[5]},
                    {m[0],m[4]},{m[1],m[5]},{-m[2],-m[6]},{x,y}};
                Require(points==water_scan_proof::Points(f),"actual five GL-to-RW frustum points versus literal SSE/PC24 oracle");
                pointChecks+=5;
                check(points);
                // Explicit source far (not reconstructed) remains separately
                // testable. This NEVER changes the installed GL projection.
                auto original=f; original.farClip=far;
                Require(env.BuildWaterScanPoints(original,points) && points==water_scan_proof::Points(original),
                    "explicit original RW camera tuple matches source arithmetic");
                pointChecks+=5; check(points);
            }
    Require(changed>0 && capped>0,"source scanner exercises geometric mismatch and ordered cap");
    RealtimeWaterState frozen{}; frozen.gameMs=1700;
    Require(env.SetWaterState(frozen),"scan GPU frozen source clock");
    size_t gpuViews=0,gpuChanged=0,gpuListChanges=0;
    // Production camera witnesses, followed by valid wide-aspect cameras that
    // exercise the source 70-cell cap. Resolution/far are fixed within each
    // comparison; these are correctness fixtures, not performance variants.
    for (int view=0;view<35;++view) {
        Camera camera; int height=kHeight;
        if (view==0) {
            camera.x=1600-4.6f*std::cos(-1.43f)*std::cos(.27f);
            camera.y=-1700-4.6f*std::sin(-1.43f)*std::cos(.27f);
            camera.z=27.303447723f+1.15f+4.6f*std::sin(.27f); camera.yaw=-1.43f; camera.pitch=-.27f;
        } else if (view==1) { camera.x=820; camera.y=-1880; camera.z=6; }
        else if (view==2) { camera.x=-2990; camera.y=123.5f; camera.z=20; }
        else {
            camera.x=-3000; camera.y=(view-3)/8%2 ? -3000 : 0; camera.z=20;
            camera.yaw=((view-3)%8)*.785398163f; camera.pitch=(view-3)/16 ? -.27f : 0;
            height=90;
        }
        camera.Apply(kWidth,height,1600);
        GLfloat p[16],m[16]; glGetFloatv(GL_PROJECTION_MATRIX,p); glGetFloatv(GL_MODELVIEW_MATRIX,m);
        const float far=float(double(p[14])/(double(p[10])+1));
        RealtimeWaterScanPoints oldPoints{},points{};
        for (size_t i=0;i<4;++i) {
            const float x=(i==0||i==3 ? -far : far)/p[0],y=(i<2 ? far : -far)/p[5];
            oldPoints[i]={(camera.x+x*m[0]+y*m[1]-far*m[2])/500.f+6.f,
                          (camera.y+x*m[4]+y*m[5]-far*m[6])/500.f+6.f};
        }
        oldPoints[4]={camera.x/500.f+6.f,camera.y/500.f+6.f};
        Require(env.CaptureWaterScanPoints(camera.x,camera.y,points),"scan GPU current frustum");
        check(points);
        const auto beforeBlocks=legacy(oldPoints);
        std::array<RealtimeWaterBlock,70> storage{}; size_t n=0;
        Require(env.ScanOutsideWaterBlocks(camera.x,camera.y,storage,n),"scan GPU actual ordered capture");
        const std::vector<RealtimeWaterBlock> afterBlocks(storage.begin(),storage.begin()+n);
        std::vector<RealtimeWaterBlock> oracleBlocks;
        for (auto c:water_scan_proof::Scan(points)) if (c[0]<=0||c[0]>=11||c[1]<=0||c[1]>=11) {
            oracleBlocks.push_back({int16_t(c[0]),int16_t(c[1])}); if (oracleBlocks.size()==70) break;
        }
        Require(afterBlocks==oracleBlocks,"scan GPU source ordered oracle list");
        const bool listChanged=beforeBlocks!=afterBlocks;
        gpuListChanges+=listChanged;
        if (view>=3 && !listChanged) continue;
        const auto render=[&](std::span<const RealtimeWaterBlock> blocks) {
            camera.Apply(kWidth,height,1600);
            glDepthMask(GL_TRUE); glClearDepth(1); glClearColor(.2f,.3f,.4f,1);
            glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
            env.DrawSky(camera.x,camera.y,camera.z);
            RealtimeSeaBedGeometry bed;
            Require(env.BuildSeaBed(blocks,camera.x,camera.y,0,bed),"scan GPU geometry");
            env.DrawSeaBed(bed); env.DrawWater(camera.x,camera.y,false,blocks); glFinish();
            return Pixels();
        };
        const auto before=render(beforeBlocks),after=render(afterBlocks);
        std::vector<float> depth(kWidth*kHeight),oracleDepth(depth.size());
        glReadPixels(0,0,kWidth,kHeight,GL_DEPTH_COMPONENT,GL_FLOAT,depth.data());
        Require(after==render(oracleBlocks),"corrected scan GPU pixels exact source-list replay");
        glReadPixels(0,0,kWidth,kHeight,GL_DEPTH_COMPONENT,GL_FLOAT,oracleDepth.data());
        Require(depth==oracleDepth && after==render(afterBlocks),"corrected scan depth and same-clock pixels exact");
        size_t pixelsChanged=0;
        for (size_t i=0;i<after.size();i+=4) pixelsChanged+=!std::equal(after.begin()+i,after.begin()+i+3,before.begin()+i);
        if (view<3 || (pixelsChanged && gpuChanged==0)) {
            const auto tag="scan-"+std::to_string(view);
            Capture(output,(tag+"-before").c_str(),before); Capture(output,(tag+"-after").c_str(),after);
        }
        gpuChanged+=pixelsChanged; ++gpuViews;
        std::printf("water-scan-GPU view=%d viewport=%dx%d old-blocks=%zu source-blocks=%zu list-changed=%d changed-pixels=%zu fnv=%016llx/%016llx source-RGBA/depth/same-clock-exact\n",
            view,kWidth,height,beforeBlocks.size(),afterBlocks.size(),int(listChanged),pixelsChanged,(unsigned long long)Hash(before),(unsigned long long)Hash(after));
    }
    std::printf("water-scan ordered-cases=%zu callbacks=%zu capped70=%zu differs-from-geometric=%zu exact-SSE-PC24-points=%zu retail-control-flow-exact\n",
        cases,cells,capped,changed,pointChecks);
    std::printf("water-scan-GPU views=%zu list-changes=%zu changed-pixels=%zu\n",gpuViews,gpuListChanges,gpuChanged);
}
} // namespace

int main(int argc, char** argv) {
    const bool performance=argc==4 && std::strcmp(argv[3],"--perf")==0;
    Require(argc == 3 || performance, "usage: probe game-dir artifact-dir [--perf]");
    char error[512]{};
    if (!performance) CheckOffline(argv[1]);
    E2ELoadInfo load{};
    Pager pager;
    Require(StreamPager_Init(argv[1], load, error, sizeof(error), {true, performance ? 900.f : 600.f, 4096}), error);
    auto* savedDictionary = rw::TexDictionary::getCurrent();
    RealtimeEnvironment env;
    Require(env.Load(argv[1], error, sizeof(error)), error);
    Require(savedDictionary == rw::TexDictionary::getCurrent(), "RW dictionary ownership preservation");
    const auto& image = env.GetWaterImage();
    std::printf("water-asset name=%s size=%dx%d rgba-fnv=%016llx rows=%d visible-tris=%d\n", image.name, image.w, image.h,
        (unsigned long long)Hash(image.rgba), env.GetWaterData().rows, env.GetWaterTriangleCount());
    Require(std::strcmp(image.name, "waterclear256") == 0 && image.w > 0 && image.h > 0, "actual particle water texture");
    Require(env.GetWaterTriangleCount() == 604, "authored visibility flags");
    if (!performance) {
        CheckSeaBedCpu(env);
        CheckOracle(env);
        CheckFlow(env);
        CheckNearest(env.GetWaterData());
        CheckFlowTicks(env.GetWaterData());
    }
    WorldShotScene scene{};
    E2EPagerFrame frame{};
    Require(StreamPager_Update(performance ? 1600 : 836, performance ? -1700 : -1866, performance ? 70 : 0, scene, frame, error, sizeof(error)), error);
    Require(!scene.meshes.empty() && Hash(image.rgba) != 0, "owned texture survives pager dictionary churn");
    EGLDisplay display = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
    Require(display != EGL_NO_DISPLAY && eglInitialize(display, nullptr, nullptr), "EGL initialize");
    Require(eglBindAPI(EGL_OPENGL_API), "EGL OpenGL");
    const EGLint attributes[]{EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8, EGL_NONE};
    EGLConfig config{}; EGLint count{};
    Require(eglChooseConfig(display, attributes, &config, 1, &count) && count == 1, "EGL config");
    const EGLint surfaceAttributes[]{EGL_WIDTH, performance ? 1280 : kWidth, EGL_HEIGHT, performance ? 720 : kHeight, EGL_NONE};
    const auto surface = eglCreatePbufferSurface(display, config, surfaceAttributes);
    const auto context = eglCreateContext(display, config, EGL_NO_CONTEXT, nullptr);
    Require(eglMakeCurrent(display, surface, surface, context), "EGL current");
    std::printf("water-GL version=%s renderer=%s\n", glGetString(GL_VERSION), glGetString(GL_RENDERER));
    {
        CheckUpload(env, error, sizeof(error));
        if (performance) CheckPerformance(env,scene);
        else {
        GpuScene gpu;
        Require(gpu.Upload(scene), "shore world GPU upload");
        glDisable(GL_DITHER);
        CheckScan(env,argv[2]);
        CheckFeedback(env, 0);
        CheckFeedback(env, 1700);
        CheckFeedback(env, 0, true);
        CheckFeedback(env, 1700, true);
        CheckFeedback(env, 1700, false, true);
        CheckState(env);
        CheckPixels(env);
        CheckFeedback(env,0,false,false,nullptr,true);
        CheckFeedback(env,1700,false,false,nullptr,true);
        CheckFeedback(env,0,true,false,nullptr,true);
        CheckFeedback(env,1700,true,false,nullptr,true);
        CheckTriangles(env, argv[2]);
        CheckSelectedFlowGpu(env, argv[2]);
        CheckSeaBedGpu(env, argv[2]);
        CheckShore(env, scene, gpu, argv[2]);
        }
        env.ReleaseGpu();
    }
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(display, context); eglDestroySurface(display, surface); eglTerminate(display);
    std::puts("water-probe-ok");
}
