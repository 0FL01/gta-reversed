// Actual native module + actual GpuScene/Camera, an extracted source oracle,
// real particle TXD and pager shoreline. This TU is not in the product target.
#define GL_GLEXT_PROTOTYPES
#include "app/platform/linux/Realtime.cpp"
#include "app/platform/linux/TexSample.h"
#include "RealtimeWaterProbe.oracle.h"
#include <EGL/eglext.h>
#include <rw.h>

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

static void CheckFeedback(RealtimeEnvironment& env, uint32_t ms, bool distant = false, bool flowing = false) {
    auto state = RealtimeWaterState{};
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
    const float left = distant ? 730 : 800, bottom = distant ? -1900 : -1894;
    const float width = distant ? 180 : 40, height = distant ? 40 : 24;
    glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(left, left+width, bottom, bottom+height, -10, 10);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    std::vector<float> feedback(1000000);
    glFeedbackBuffer(static_cast<GLsizei>(feedback.size()), GL_3D_COLOR_TEXTURE, feedback.data());
    glRenderMode(GL_FEEDBACK);
    env.DrawWater(820, distant ? -1780 : -1880);
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
                if ((std::abs(x-736) > .001f && std::abs(x-904) > .001f) ||
                    (std::abs(y+1896) > .001f && std::abs(y+1864) > .001f)) continue;
            } else if (x <= 800.01f || x >= 839.99f || y <= -1893.99f || y >= -1870.01f) continue; // clipped vertices
            Require(std::abs(x-std::round(x/2)*2) < .001f && std::abs(y-std::round(y/2)*2) < .001f,
                "source 2m detail grid");
            const float attenuation = std::clamp((1-std::hypot(x-820, y+1880)/48)*2, 0.f, 1.f);
            const auto ref = distant ? RealtimeWaterSample{0, .577f, 0, {0,0,1}} :
                Oracle(int(std::round(x)), int(std::round(y)), 0, body->v[0].bigWaves * attenuation,
                    body->v[0].smallWaves * attenuation, state);
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
        distant ? "far" : flowing ? "near-flow" : "near", ms, checked, zError, uvError, colorError);
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
    glPopMatrix();
    env.EndWorld();
    glMatrixMode(GL_MODELVIEW); glActiveTexture(GL_TEXTURE0);
    Require(glGetError() == GL_NO_ERROR, "water state GL errors");
    std::printf("water-state restored-values=%zu\n", before.size());
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

static void CheckShore(RealtimeEnvironment& env, const WorldShotScene& scene, const GpuScene& gpu, const char* output) {
    for (bool close : {true, false}) {
        Camera camera;
        camera.x = close ? 820 : 746; camera.y = close ? -1920 : -1986;
        camera.z = close ? 4 : 55;
        camera.yaw = close ? 1.570796327f : std::atan2(120.f, 90.f);
        camera.pitch = close ? -.12f : -std::atan2(55.f, 150.f);
        auto render = [&](uint32_t ms, bool water) {
            auto state = RealtimeWaterState{}; state.gameMs = ms;
            Require(env.SetWaterState(state), "render explicit time");
            camera.Apply(kWidth, kHeight, env.GetParams().farClip);
            glDepthMask(GL_TRUE); glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            env.DrawSky(camera.x, camera.y, camera.z);
            env.BeginWorld(); gpu.Draw(scene); env.EndWorld();
            if (water) env.DrawWater(camera.x, camera.y);
            glFinish();
            Require(glGetError() == GL_NO_ERROR, "shore GL errors");
            return Pixels();
        };
        const auto baseline = render(0, false), first = render(0, true), second = render(1700, true), repeat = render(1700, true);
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
} // namespace

int main(int argc, char** argv) {
    Require(argc == 3, "usage: probe game-dir artifact-dir");
    char error[512]{};
    CheckOffline(argv[1]);
    E2ELoadInfo load{};
    Pager pager;
    Require(StreamPager_Init(argv[1], load, error, sizeof(error), {true, 600, 4096}), error);
    auto* savedDictionary = rw::TexDictionary::getCurrent();
    RealtimeEnvironment env;
    Require(env.Load(argv[1], error, sizeof(error)), error);
    Require(savedDictionary == rw::TexDictionary::getCurrent(), "RW dictionary ownership preservation");
    const auto& image = env.GetWaterImage();
    std::printf("water-asset name=%s size=%dx%d rgba-fnv=%016llx rows=%d visible-tris=%d\n", image.name, image.w, image.h,
        (unsigned long long)Hash(image.rgba), env.GetWaterData().rows, env.GetWaterTriangleCount());
    Require(std::strcmp(image.name, "waterclear256") == 0 && image.w > 0 && image.h > 0, "actual particle water texture");
    Require(env.GetWaterTriangleCount() == 604, "authored visibility flags");
    CheckOracle(env);
    CheckFlow(env);
    WorldShotScene scene{};
    E2EPagerFrame frame{};
    Require(StreamPager_Update(836, -1866, 0, scene, frame, error, sizeof(error)), error);
    Require(!scene.meshes.empty() && Hash(image.rgba) != 0, "owned texture survives pager dictionary churn");
    EGLDisplay display = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
    Require(display != EGL_NO_DISPLAY && eglInitialize(display, nullptr, nullptr), "EGL initialize");
    Require(eglBindAPI(EGL_OPENGL_API), "EGL OpenGL");
    const EGLint attributes[]{EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8, EGL_NONE};
    EGLConfig config{}; EGLint count{};
    Require(eglChooseConfig(display, attributes, &config, 1, &count) && count == 1, "EGL config");
    const EGLint surfaceAttributes[]{EGL_WIDTH, kWidth, EGL_HEIGHT, kHeight, EGL_NONE};
    const auto surface = eglCreatePbufferSurface(display, config, surfaceAttributes);
    const auto context = eglCreateContext(display, config, EGL_NO_CONTEXT, nullptr);
    Require(eglMakeCurrent(display, surface, surface, context), "EGL current");
    std::printf("water-GL version=%s renderer=%s\n", glGetString(GL_VERSION), glGetString(GL_RENDERER));
    {
        CheckUpload(env, error, sizeof(error));
        GpuScene gpu;
        Require(gpu.Upload(scene), "shore world GPU upload");
        glDisable(GL_DITHER);
        CheckFeedback(env, 0);
        CheckFeedback(env, 1700);
        CheckFeedback(env, 0, true);
        CheckFeedback(env, 1700, true);
        CheckFeedback(env, 1700, false, true);
        CheckState(env);
        CheckPixels(env);
        CheckTriangles(env, argv[2]);
        CheckShore(env, scene, gpu, argv[2]);
        env.ReleaseGpu();
    }
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(display, context); eglDestroySurface(display, surface); eglTerminate(display);
    std::puts("water-probe-ok");
}
