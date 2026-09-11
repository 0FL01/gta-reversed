// SDL owns the window and EGL context on Wayland. Real DFF triangles/TXD
// textures are rasterized by GL, not by TexSample's offline CPU renderer.
#include "app/platform/linux/Realtime.h"
#include "app/platform/linux/StreamPager.h"
#include "app/platform/linux/RealtimeEnvironment.h"
#include "app/platform/linux/RealtimeClouds.h"
#include "app/platform/linux/RealtimeGameplay.h"
#include "app/platform/linux/RealtimeHud.h"
#include "app/platform/linux/RealtimeStreaming.h"
#include "app/platform/linux/NativePlayerAssets.h"
#include "app/platform/linux/RealtimeScriptHost.h"
#include "app/platform/linux/NativeGaragesRuntime.h"
#include "app/platform/linux/NativeCarGeneratorRuntime.h"
#include "app/platform/linux/NativePadFeedback.h"

#ifndef GL_GLEXT_PROTOTYPES
#define GL_GLEXT_PROTOTYPES
#endif
#include <SDL3/SDL.h>
#include <EGL/egl.h>
#include <GL/gl.h>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>
#include <string>

namespace {
struct Window {
    SDL_Window* window = nullptr;
    SDL_GLContext context = nullptr;

    ~Window() {
        if (context) {
            SDL_GL_DestroyContext(context);
        }
        if (window) {
            SDL_DestroyWindow(window);
        }
        SDL_Quit();
    }
};

struct Pager {
    ~Pager() { StreamPager_Shutdown(); }
};

static GLint WrapMode(uint32_t mode) {
    if (mode == 2) {
        return GL_MIRRORED_REPEAT;
    }
    return mode == 3 || mode == 4 ? GL_CLAMP_TO_EDGE : GL_REPEAT;
}

// Compatibility GL is already a native-track dependency. Static geometry uses
// bounded lists, preserving precisely the dynamic Draw vertex/material path.
struct GpuScene {
    std::vector<GLuint> textures;
    std::vector<GLuint> lists;
    size_t uploadImage = 0, uploadMesh = 0;
    int uploadRow = -1, uploadTriangle = 0;
    bool complete = false;
    const std::thread::id owner = std::this_thread::get_id();

    GpuScene() = default;
    GpuScene(const GpuScene&) = delete;
    GpuScene& operator=(const GpuScene&) = delete;
    ~GpuScene() { Clear(); }

    void Clear() {
        assert(owner == std::this_thread::get_id());
        glDeleteTextures(static_cast<GLsizei>(textures.size()), textures.data());
        textures.clear();
        for (auto chunk : lists) {
            glDeleteLists(chunk, 1);
        }
        lists.clear();
        uploadImage = uploadMesh = 0;
        uploadRow = -1;
        uploadTriangle = 0;
        complete = false;
    }

    bool UploadTextures(const WorldShotScene& scene) {
        Clear();
        textures.resize(scene.images.size());
        glGenTextures(static_cast<GLsizei>(textures.size()), textures.data());
        for (size_t i = 0; i < scene.images.size(); ++i) {
            const auto& image = scene.images[i];
            if (image.w <= 0 || image.h <= 0 ||
                image.rgba.size() != static_cast<size_t>(image.w) * image.h * 4) {
                std::printf("play-fail invalid texture %s\n", image.name);
                return false;
            }
            glBindTexture(GL_TEXTURE_2D, textures[i]);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, WrapMode((image.filter >> 8) & 15));
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, WrapMode((image.filter >> 12) & 15));
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, image.w, image.h, 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, image.rgba.data());
        }
        return glGetError() == GL_NO_ERROR;
    }

    bool Upload(const WorldShotScene& scene) {
        Clear();
        while (!complete) {
            if (!UploadStep(scene, realtime_streaming::Milliseconds() + 4.0)) {
                return false;
            }
        }
        return true;
    }

    // Every GL operation is on the context thread. A deadline is a soft budget:
    // a driver allocation can overrun it. No glBegin/glNewList spans frames.
    bool UploadStep(const WorldShotScene& scene, double deadline) {
        assert(owner == std::this_thread::get_id());
        if (textures.empty()) {
            textures.resize(scene.images.size());
            lists.reserve((scene.stats.triangles + 1023) / 1024);
        }
        do {
            if (uploadImage < scene.images.size()) {
                const auto& image = scene.images[uploadImage];
                if (image.w <= 0 || image.h <= 0 ||
                    image.rgba.size() != static_cast<size_t>(image.w) * image.h * 4) {
                    std::printf("play-fail invalid texture %s\n", image.name);
                    return false;
                }
                auto& texture = textures[uploadImage];
                if (uploadRow < 0) {
                    glGenTextures(1, &texture);
                    glBindTexture(GL_TEXTURE_2D, texture);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, WrapMode((image.filter >> 8) & 15));
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, WrapMode((image.filter >> 12) & 15));
                    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, image.w, image.h, 0,
                                 GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
                    uploadRow = 0;
                } else {
                    glBindTexture(GL_TEXTURE_2D, texture);
                    const int rows = std::min(image.h - uploadRow, std::max(1, 65536 / (image.w * 4)));
                    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, uploadRow, image.w, rows,
                        GL_RGBA, GL_UNSIGNED_BYTE, image.rgba.data() + static_cast<size_t>(uploadRow) * image.w * 4);
                    uploadRow += rows;
                    if (uploadRow == image.h) {
                        ++uploadImage;
                        uploadRow = -1;
                    }
                }
            } else if (uploadMesh < scene.meshes.size()) {
                const auto chunk = glGenLists(1);
                if (!chunk) {
                    std::printf("play-fail GL display list allocation\n");
                    return false;
                }
                lists.push_back(chunk);
                glNewList(chunk, GL_COMPILE);
                Draw(scene, uploadMesh, uploadTriangle, 1024);
                glEndList();
                int remaining = 1024;
                while (uploadMesh < scene.meshes.size()) {
                    const int count = std::min(remaining, scene.meshes[uploadMesh].tris - uploadTriangle);
                    uploadTriangle += count;
                    remaining -= count;
                    if (uploadTriangle == scene.meshes[uploadMesh].tris) {
                        ++uploadMesh;
                        uploadTriangle = 0;
                    } else {
                        break;
                    }
                }
            } else {
                complete = true;
            }
            const auto error = glGetError();
            if (error != GL_NO_ERROR) {
                std::printf("play-fail staged upload GL=0x%x\n", error);
                return false;
            }
        } while (!complete && realtime_streaming::Milliseconds() < deadline);
        return true;
    }

    bool RetireStep(double deadline) {
        assert(owner == std::this_thread::get_id());
        do {
            if (!lists.empty()) {
                glDeleteLists(lists.back(), 1);
                lists.pop_back();
            } else if (!textures.empty()) {
                glDeleteTextures(1, &textures.back());
                textures.pop_back();
            } else {
                return true;
            }
        } while (realtime_streaming::Milliseconds() < deadline);
        return lists.empty() && textures.empty();
    }

    void Render() const {
        assert(owner == std::this_thread::get_id() && complete);
        for (auto chunk : lists) {
            glCallList(chunk);
        }
    }

    enum class ActorPass { All, Opaque, Alpha };

    // CRenderer::RenderOneNonRoad renders the opaque vehicle first, then
    // CVisibilityPlugins::RenderAlphaAtomics; CustomCarEnvMapPipeline enables
    // source-alpha blending per material. The native flattened scene has no
    // visibility-plugin component flags: use stable eye-depth triangle order,
    // not the original atomic distance/dot-product heuristic. This is a bounded
    // transparency slice, not that heuristic or the env/specular pipeline.
    void DrawActors(const WorldShotScene& scene, ActorPass pass = ActorPass::All) const {
        assert(owner == std::this_thread::get_id());
        struct AlphaTriangle { size_t mesh; int triangle; float depth; };
        std::vector<AlphaTriangle> alpha;
        GLfloat view[16];
        glGetFloatv(GL_MODELVIEW_MATRIX, view);
        glPushAttrib(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_ENABLE_BIT);
        glEnable(GL_DEPTH_TEST);
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
        for (size_t m = 0; m < scene.meshes.size(); ++m) {
            const auto& mesh = scene.meshes[m];
            const bool hasSurfaces = mesh.surfaces.size() == static_cast<size_t>(mesh.tris);
            int begin = 0;
            for (int t = 0; t <= mesh.tris; ++t) {
                const bool blend = t < mesh.tris && hasSurfaces && mesh.surfaces[t].vehicleAlpha;
                if (t == mesh.tris || blend) {
                    if (pass != ActorPass::Alpha && t > begin) Draw(scene, m, begin, t - begin);
                    begin = t + 1;
                }
                if (!blend || pass == ActorPass::Opaque) continue;
                float depth = 0.0f;
                for (int k = 0; k < 3; ++k) {
                    const auto* p = &mesh.pos[t * 9 + k * 3];
                    depth -= view[2] * p[0] + view[6] * p[1] + view[10] * p[2] + view[14];
                }
                alpha.push_back({m, t, depth / 3.0f});
            }
        }
        std::stable_sort(alpha.begin(), alpha.end(), [](const auto& a, const auto& b) { return a.depth > b.depth; });
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        // RenderAlphaAtomics does not disable ZWRITE. Keep testing/writing depth
        // as upstream, drawing far-to-near. Reject zero-alpha texels so holes
        // never occlude later geometry; do not inherit the world's cutout ref.
        glEnable(GL_ALPHA_TEST);
        glAlphaFunc(GL_GREATER, 0.0f);
        for (const auto& triangle : alpha) Draw(scene, triangle.mesh, triangle.triangle, 1);
        glPopAttrib();
    }

    // C3dMarker::Render cone pass: bright material, tested depth, no depth writes.
    void DrawMarkers(const WorldShotScene& scene) const {
        GLint program = 0;
        glGetIntegerv(GL_CURRENT_PROGRAM, &program);
        const bool colorSum = glIsEnabled(GL_COLOR_SUM);
        glPushAttrib(GL_ALL_ATTRIB_BITS);
        glUseProgram(0);
        glActiveTexture(GL_TEXTURE0);
        glDisable(GL_LIGHTING);
        glDisable(GL_COLOR_SUM);
        glEnable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);
        glFrontFace(GL_CCW);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glEnable(GL_ALPHA_TEST);
        glAlphaFunc(GL_GREATER, 0.0f);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
        Draw(scene);
        glPopAttrib();
        if (colorSum) glEnable(GL_COLOR_SUM); else glDisable(GL_COLOR_SUM);
        glUseProgram(program);
    }

    // Dynamic actors retain textures; only posed vertices change each tick.
    void Draw(const WorldShotScene& scene, size_t firstMesh = 0, int firstTriangle = 0,
              int remaining = std::numeric_limits<int>::max()) const {
        assert(owner == std::this_thread::get_id());
        for (size_t m = firstMesh; m < scene.meshes.size() && remaining > 0; ++m) {
            const auto& mesh = scene.meshes[m];
            const int begin = m == firstMesh ? firstTriangle : 0;
            const int end = begin + std::min(mesh.tris - begin, remaining);
            remaining -= end - begin;
            assert(mesh.pos.size() == static_cast<size_t>(mesh.tris) * 9);
            assert(mesh.nrm.size() == mesh.pos.size());
            const bool hasUV = mesh.uv.size() == static_cast<size_t>(mesh.tris) * 6;
            const bool hasImages = mesh.triImg.size() == static_cast<size_t>(mesh.tris);
            const bool hasColors = mesh.triCol.size() == static_cast<size_t>(mesh.tris) * 3;
            const bool hasSurfaces = mesh.surfaces.size() == static_cast<size_t>(mesh.tris);
            const bool hasDay = mesh.dayColors.size() == static_cast<size_t>(mesh.tris) * 12;
            const bool hasNight = mesh.nightColors.size() == mesh.dayColors.size() && hasDay;
            int previous = -3;
            for (int t = begin; t < end; ++t) {
                const int image = hasImages && hasUV ? mesh.triImg[t] : -1;
                assert(image < static_cast<int>(textures.size()));
                if (image != previous) {
                    if (t != begin) {
                        glEnd();
                    }
                    if (image >= 0) {
                        glEnable(GL_TEXTURE_2D);
                        glBindTexture(GL_TEXTURE_2D, textures[image]);
                    } else {
                        glDisable(GL_TEXTURE_2D);
                    }
                    glBegin(GL_TRIANGLES);
                    previous = image;
                }
                const float* color = hasColors ? &mesh.triCol[t * 3] : mesh.color;
                const auto* surface = hasSurfaces ? &mesh.surfaces[t] : nullptr;
                glMultiTexCoord2f(GL_TEXTURE3, surface ? surface->ambient : 1.0f,
                                  surface ? surface->diffuse : 1.0f);
                for (int k = 0; k < 3; ++k) {
                    const size_t v = static_cast<size_t>(t) * 3 + k;
                    const float* n = &mesh.nrm[v * 3];
                    glNormal3fv(n);
                    const uint8_t black[]{0, 0, 0, 255};
                    const auto* day = hasDay ? &mesh.dayColors[v * 4] : black;
                    const auto* night = hasNight ? &mesh.nightColors[v * 4] : day;
                    glMultiTexCoord4f(GL_TEXTURE1, day[0] / 255.0f, day[1] / 255.0f,
                                      day[2] / 255.0f, day[3] / 255.0f);
                    glMultiTexCoord4f(GL_TEXTURE2, night[0] / 255.0f, night[1] / 255.0f,
                                      night[2] / 255.0f, night[3] / 255.0f);
                    // Preserve the authored material (including car paint)
                    // even when its texture is absent. The diagnostic grey
                    // belongs only to legacy scenes without material metadata.
                    if (surface) {
                        glColor4fv(surface->color.data());
                    } else if (image == -2) {
                        glColor3f(0.5f, 0.5f, 0.5f);
                    } else {
                        glColor3f(color[0], color[1], color[2]);
                    }
                    if (hasUV) {
                        glTexCoord2fv(&mesh.uv[v * 2]);
                    }
                    glVertex3fv(&mesh.pos[v * 3]);
                }
            }
            if (end > begin) {
                glEnd();
            }
        }
        glDisable(GL_TEXTURE_2D);
    }
};

struct ResidentWorld {
    std::unique_ptr<realtime_streaming::CpuWorld> cpu;
    GpuScene gpu;
    const RealtimeGameplayWorld& Collision() const {
        return cpu->QueryWorld();
    }
};

struct LiveWorld {
    std::unique_ptr<ResidentWorld> active = std::make_unique<ResidentWorld>();
    std::unique_ptr<ResidentWorld> pending, retiring;
    std::shared_ptr<const NativeCollisionContext> collisionContext;
    std::unique_ptr<realtime_streaming::Worker> worker;
    RealtimeScriptHost* scriptHost = nullptr; // outlives this world; main-thread only
    double uploadMs = 0;

    ~LiveWorld() {
        if (worker) {
            worker->Stop(std::move(active->cpu), pending ? std::move(pending->cpu) : nullptr);
        }
        // Remaining GL handles die here, before Window/Pager. No worker holds
        // scene references now. During play retirement is incremental instead.
    }

    bool Initialize(realtime_streaming::Center center, bool collision, bool requireSource = false) {
        if (collision && requireSource && !collisionContext) {
            std::printf("play-fail gameplay requires source COL context\n");
            return false;
        }
        active->cpu = std::make_unique<realtime_streaming::CpuWorld>();
        active->cpu->Position = center;
        active->cpu->Generation = 1;
        active->cpu->Build(collision, collisionContext);
        if (!active->cpu->Error.empty()) {
            std::printf("play-fail initial world: %s\n", active->cpu->Error.c_str());
            return false;
        }
        if (collision && collisionContext) {
            float ground;
            NativeCollisionHit hit;
            if (active->Collision().Ground(center.X, center.Y, center.Z + 1, center.Z - 150, ground, &hit)) {
                std::printf("play-col-ground xy=%.6f,%.6f z=%.9f model=%s IPL=%s record=%u\n",
                    center.X, center.Y, ground, hit.Model.c_str(), hit.Ipl.c_str(), hit.Record);
            }
        }
        return active->gpu.Upload(active->cpu->Scene);
    }

    bool Initialize(const RealtimeScriptHost& host) {
        const auto& publication = host.Publication();
        assert(publication.Scene && host.World() && host.WorldRevision());
        active->cpu = std::make_unique<realtime_streaming::CpuWorld>();
        auto& cpu = *active->cpu;
        cpu.Position = {publication.Center.X, publication.Center.Y, publication.Center.Z};
        // Continue the source-residency publication sequence, not a new counter.
        cpu.Generation = host.WorldRevision();
        cpu.Frame = publication.Frame;
        cpu.Scene = *publication.Scene; // owned snapshot, no pager/RW parser or second BVH
        cpu.Overrides = publication.Overrides;
        cpu.SourceCollision = publication.SourceCollision;
        cpu.BorrowedCollision = publication.Collision;
        return active->gpu.Upload(cpu.Scene);
    }

    void Start(bool collision) {
        worker = std::make_unique<realtime_streaming::Worker>(collision, collisionContext, active->cpu->Overrides, active->cpu->Generation);
    }

    // Call once, BEFORE Tick: physics and draw see exactly the same generation.
    bool Advance(realtime_streaming::Center center, bool& published) {
        published = false;
        auto request = [&] {
            const auto loaded = active->cpu->Position;
            worker->Request(center, std::hypot(center.X - loaded.X, center.Y - loaded.Y) >= 40.0f);
        };
        request();
        const double start = realtime_streaming::Milliseconds();
        constexpr double budgetMs = 4.0;
        if (retiring) {
            if (retiring->gpu.RetireStep(start + budgetMs)) {
                retiring.reset();
                worker->Release();
            }
            return true;
        }
        if (!pending) {
            if (auto cpu = worker->TakeReady()) {
                pending = std::make_unique<ResidentWorld>();
                pending->cpu = std::move(cpu);
                uploadMs = 0;
                if (!pending->cpu->Error.empty()) {
                    std::printf("play-fail worker world: %s\n", pending->cpu->Error.c_str());
                    return false;
                }
            }
        }
        if (pending) {
            if (!pending->gpu.UploadStep(pending->cpu->Scene, start + budgetMs)) {
                return false;
            }
            uploadMs += realtime_streaming::Milliseconds() - start;
            if (pending->gpu.complete) {
                if (scriptHost) {
                    const auto residency = scriptHost->ReconcileCarGeneratorsBeforeWorldCommit(
                        pending->cpu->Generation, pending->cpu->SourceCollision);
                    if (residency.Status != NativeCarGeneratorResidencyStatus::Ready) {
                        // No removal/model-retention owner yet. Never publish
                        // a new world beside old definitions or acknowledge cleanup.
                        std::printf("play-cargen-residency-terminal status=%s generation=%llu removals=%zu message=%s\n",
                            residency.Status == NativeCarGeneratorResidencyStatus::PendingCleanup ? "Unsupported" : "Error",
                            static_cast<unsigned long long>(pending->cpu->Generation), residency.Removals.size(), residency.Detail.c_str());
                        return false;
                    }
                }
                active.swap(pending); // matching immutable soup + BVH + GL handles
                retiring = std::move(pending);
                worker->Retire(std::move(retiring->cpu));
                request(); // don't rebuild the just-published center from stale mailbox state
                published = true;
                const auto& cpu = *active->cpu;
                std::printf("play-stream generation=%llu pagerMs=%.2f bvhMs=%.2f gpuMs=%.2f ageMs=%.2f lagM=%.1f\n",
                    static_cast<unsigned long long>(cpu.Generation), cpu.PagerMs, cpu.CollisionMs, uploadMs,
                    realtime_streaming::Milliseconds() - cpu.Started,
                    std::hypot(center.X - cpu.Position.X, center.Y - cpu.Position.Y));
            }
        }
        return true;
    }
};

struct Camera {
    float x = 1600.0f, y = -1700.0f, z = 70.0f;
    float yaw = -1.43f, pitch = -0.20f;

    bool SphereVisible(NativeScriptPosition position, float radius, int width, int height, float farPlane) const {
        const float dx = position.X-x, dy = position.Y-y, dz = position.Z-z;
        const float cy = std::cos(yaw), sy = std::sin(yaw), cp = std::cos(pitch), sp = std::sin(pitch);
        const float depth = (dx*cy+dy*sy)*cp+dz*sp;
        const float side = dx*sy-dy*cy, up = -(dx*cy+dy*sy)*sp+dz*cp;
        const float vertical = std::tan(3.14159265f/6.0f), horizontal = vertical*width/height;
        return depth+radius >= 0.1f && depth-radius <= farPlane &&
            std::abs(up) <= depth*vertical+radius*std::sqrt(1+vertical*vertical) &&
            std::abs(side) <= depth*horizontal+radius*std::sqrt(1+horizontal*horizontal);
    }

    void Update(float dt, const bool* keys, bool demo) {
        yaw += (keys[SDL_SCANCODE_LEFT] - keys[SDL_SCANCODE_RIGHT]) * dt;
        pitch = std::clamp(pitch + (keys[SDL_SCANCODE_UP] - keys[SDL_SCANCODE_DOWN]) * dt,
                           -1.4f, 1.4f);
        const float speed = keys[SDL_SCANCODE_LSHIFT] ? 90.0f : 25.0f;
        const float forward = demo ? 8.0f : (keys[SDL_SCANCODE_W] - keys[SDL_SCANCODE_S]) * speed;
        const float side = (keys[SDL_SCANCODE_D] - keys[SDL_SCANCODE_A]) * speed;
        x += (std::cos(yaw) * forward + std::sin(yaw) * side) * dt;
        y += (std::sin(yaw) * forward - std::cos(yaw) * side) * dt;
        z += (keys[SDL_SCANCODE_E] - keys[SDL_SCANCODE_Q]) * speed * dt;
    }

    void Apply(int width, int height, float farPlane) const {
        glViewport(0, 0, width, height);
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        const double top = 0.1 * std::tan(3.14159265 / 6.0);
        const double right = top * width / height;
        glFrustum(-right, right, -top, top, 0.1, farPlane);
        const float cy = std::cos(yaw), sy = std::sin(yaw);
        const float cp = std::cos(pitch), sp = std::sin(pitch);
        // Z-up look-at, column-major OpenGL view matrix.
        const float view[] = {
            sy, -cy * sp, -cy * cp, 0,
            -cy, -sy * sp, -sy * cp, 0,
            0, cp, -sp, 0,
            0, 0, 0, 1
        };
        glMatrixMode(GL_MODELVIEW);
        glLoadMatrixf(view);
        glTranslatef(-x, -y, -z);
    }
};

static bool ScriptFault(const RealtimeScriptHost& host, const NativeScriptResult& result) {
    if (result.Status != NativeScriptStatus::Unsupported && result.Status != NativeScriptStatus::Error) {
        return false;
    }
    const auto threads = host.Session().Threads();
    const auto generation = result.ThreadIndex < threads.size() ? threads[result.ThreadIndex].Generation : 0;
    std::printf("play-script-terminal status=%s thread=%zu generation=%llu ip=%u opcode=%04X executed=%zu message=%s\n",
        result.Status == NativeScriptStatus::Unsupported ? "Unsupported" : "Error", result.ThreadIndex,
        static_cast<unsigned long long>(generation), result.IP, result.Opcode, result.Executed, result.Message.c_str());
    std::fflush(stdout);
    return true;
}

static void DrawScriptFade(const NativeScriptFade& fade) {
    if (fade.Alpha <= 0) {
        return;
    }
    glPushAttrib(GL_ENABLE_BIT | GL_COLOR_BUFFER_BIT | GL_CURRENT_BIT | GL_DEPTH_BUFFER_BIT);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_ALPHA_TEST);
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_LIGHTING);
    glDisable(GL_FOG);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();
    glColor4f(0, 0, 0, std::clamp(fade.Alpha / 255.0f, 0.0f, 1.0f));
    glBegin(GL_QUADS);
    glVertex2f(-1, -1); glVertex2f(1, -1); glVertex2f(1, 1); glVertex2f(-1, 1);
    glEnd();
    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glPopAttrib();
}
} // namespace

int Realtime_Run(int argc, char** argv, const char* gameDir) {
    double seconds = 0;
    bool demo = false;
    bool demoCurb = false;
    bool freecam = false;
    bool playerCj = false;
    bool newGame = false, explicitCamera = false, explicitHour = false;
    bool freezeTime = false;
    float hour = 12.0f;
    const char* weather = "EXTRASUNNY_LA";
    Camera camera;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--seconds") == 0) {
            char* end = nullptr;
            if (++i == argc) {
                std::printf("play-fail --seconds needs a positive number\n");
                return 1;
            }
            seconds = std::strtod(argv[i], &end);
            if (end == argv[i] || *end || !std::isfinite(seconds) || seconds <= 0) {
                std::printf("play-fail --seconds needs a positive number\n");
                return 1;
            }
        } else if (std::strcmp(argv[i], "--demo") == 0) {
            demo = true;
        } else if (std::strcmp(argv[i], "--demo-curb") == 0) {
            demoCurb = true;
        } else if (std::strcmp(argv[i], "--freecam") == 0) {
            freecam = true;
        } else if (std::strcmp(argv[i], "--player-cj") == 0) {
            playerCj = true;
        } else if (std::strcmp(argv[i], "--new-game") == 0) {
            newGame = true;
        } else if (std::strcmp(argv[i], "--freeze-time") == 0) {
            freezeTime = true;
        } else if (std::strcmp(argv[i], "--hour") == 0) {
            explicitHour = true;
            char* end = nullptr;
            if (++i == argc) {
                std::printf("play-fail --hour needs a number in [0,24)\n");
                return 1;
            }
            hour = std::strtof(argv[i], &end);
            if (end == argv[i] || *end || !std::isfinite(hour) || hour < 0 || hour >= 24) {
                std::printf("play-fail invalid --hour\n");
                return 1;
            }
        } else if (std::strcmp(argv[i], "--weather") == 0) {
            if (++i == argc) {
                std::printf("play-fail --weather needs a timecycle section\n");
                return 1;
            }
            weather = argv[i];
        } else if (std::strcmp(argv[i], "--cam") == 0) {
            explicitCamera = true;
            int used = 0;
            if (++i == argc || std::sscanf(argv[i], "%f,%f,%f%n", &camera.x, &camera.y, &camera.z, &used) != 3 ||
                argv[i][used] || !std::isfinite(camera.x) || !std::isfinite(camera.y) || !std::isfinite(camera.z) ||
                std::abs(camera.x) > 6000 || std::abs(camera.y) > 6000 || std::abs(camera.z) > 2000) {
                std::printf("play-fail --cam needs finite map coordinates x,y,z\n");
                return 1;
            }
        }
    }
    if (newGame && (demo || demoCurb || playerCj || freecam || explicitCamera || explicitHour || freezeTime)) {
        std::printf("play-fail --new-game conflicts with --demo/--demo-curb/--player-cj/--freecam/--cam/--hour/--freeze-time\n");
        return 1;
    }
    if (playerCj && freecam) {
        std::printf("play-fail --player-cj requires gameplay without --freecam\n");
        return 1;
    }
    if (demoCurb) {
        if (freecam || demo) {
            std::printf("play-fail --demo-curb requires gameplay without --demo/--freecam\n");
            return 1;
        }
        // Measured real 17 cm curb, not generated terrain or a pose fixture.
        camera.x = 1540.0f;
        camera.y = -1736.0f;
        camera.z = 20.0f;
        camera.yaw = -1.57079637f;
    }
    Window window;
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::printf("play-fail SDL init: %s\n", SDL_GetError());
        return 1;
    }
    // OpenGL 2.1 compatibility supports the native track's fixed pipeline.
    if (!SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2) ||
        !SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1) ||
        !SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1) ||
        !SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24)) {
        std::printf("play-fail GL attributes: %s\n", SDL_GetError());
        return 1;
    }
    window.window = SDL_CreateWindow("mad-sa | OpenGL gameplay | WASD / arrows / Space / F / Tab / Esc",
        1280, 720, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!window.window || !(window.context = SDL_GL_CreateContext(window.window)) ||
        !SDL_GL_MakeCurrent(window.window, window.context)) {
        std::printf("play-fail GL window/context: %s\n", SDL_GetError());
        return 1;
    }
    if (!SDL_GL_SetSwapInterval(1)) {
        std::printf("play-note vsync unavailable: %s (60 Hz timer remains enabled)\n", SDL_GetError());
    }
    std::printf("play-video driver=%s EGL=%s GL=%s renderer=%s\n", SDL_GetCurrentVideoDriver(),
        eglGetCurrentContext() != EGL_NO_CONTEXT ? "yes" : "no",
        glGetString(GL_VERSION), glGetString(GL_RENDERER));
    std::printf("play-controls WASD move/drive; arrows look; Shift sprint; Space jump/handbrake; F enter/exit; "
                "Tab free camera; Esc quit; demo=%d freecam=%d\n", demo, freecam);
    std::fflush(stdout);

    Pager pager;
    E2ELoadInfo load{};
    char error[512] = {};
    const StreamPagerOptions options{ .includeStreamed = true, .radius = 900.0f, .maxInstances = 4096 };
    if (!StreamPager_Init(gameDir, load, error, sizeof(error), options)) {
        std::printf("play-fail pager init: %s\n", error);
        return 1;
    }
    std::printf("play-load textAndBinary=%d outdoor=%d binaryFiles=%d binaryInstances=%d radius=%.0f cap=%d\n",
        load.iplTotal, load.iplKept, load.binaryIplFiles, load.binaryInstances, options.radius, options.maxInstances);
    RealtimeGameplay gameplay;
    std::string gameplayError;
    RealtimeScriptHost scriptHost(gameplay);
    if (newGame && !scriptHost.SeedSourceRngAfterRwInit(gameplayError)) {
        std::printf("play-fail source RNG: %s\n", gameplayError.c_str());
        return 1;
    }
    std::shared_ptr<const NativeCollisionContext> collisionContext;
    if (!freecam) {
        const auto started = realtime_streaming::Milliseconds();
        collisionContext = NativeCollisionContext::LoadBeforeWorker(gameDir, options.radius, gameplayError);
        if (!collisionContext) {
            std::printf("play-fail source COL init: %s\n", gameplayError.c_str());
            return 1;
        }
        const auto& stats = collisionContext->Assets.Stats();
        std::printf("play-col-load ms=%.2f models=%zu triangles=%zu spheres=%zu boxes=%zu empty=%zu unsupported=%zu IPL=%zu radius=%.0f\n",
            realtime_streaming::Milliseconds() - started, stats.Models, stats.Triangles, stats.Spheres, stats.Boxes,
            stats.Empty, stats.Unsupported, collisionContext->Population.Instances.size(), collisionContext->Radius);
    }
    NativeGaragesRuntime garageRuntime(scriptHost.Garages(), gameplay, scriptHost.Vehicles());
    constexpr std::size_t scriptQuota = 256; // one bounded scheduler pass per presented frame
    if (newGame) {
        if (!scriptHost.InitializeBeforeWorker(gameDir, gameplayError, collisionContext)) {
            std::printf("play-fail script init: %s\n", gameplayError.c_str());
            return 1;
        }
        const auto result = scriptHost.RunPass(scriptQuota);
        if (ScriptFault(scriptHost, result)) {
            return 1;
        }
        if (result.Status != NativeScriptStatus::Waiting || !gameplay.State().Ready || !scriptHost.World()) {
            std::printf("play-fail script startup has no presentable world/player at first yield\n");
            return 1;
        }
        // Prepare the first common garage collision update without committing
        // its flags yet. Upload this paired world before the real update below.
        if (!scriptHost.PrepareInitialGarageWorldBeforeWorker(gameplayError)) {
            std::printf("play-fail initial garage world: %s\n", gameplayError.c_str());
            return 1;
        }
        // Process the source zero-duration fade before the first presentation;
        // this is a timer update at t=0, not another scheduler pass.
        if (!scriptHost.AdvanceTime(0, gameplayError)) {
            std::printf("play-fail script clock: %s\n", gameplayError.c_str());
            return 1;
        }
        const auto& clock = scriptHost.State().Clock;
        hour = clock.Hours + clock.Minutes / 60.0f + clock.Seconds / 3600.0f;
        const auto& view = gameplay.Camera();
        camera = {view.Position.X, view.Position.Y, view.Position.Z, view.Yaw, view.Pitch};
        std::printf("play-script-startup commands=%zu ip=%u worlds=%llu clock=%02u:%02u fade=%.0f model=MODEL_PLAYER\n",
            result.Executed, scriptHost.State().IP, static_cast<unsigned long long>(scriptHost.WorldRevision()),
            unsigned(clock.Hours), unsigned(clock.Minutes), scriptHost.State().Fade.Alpha);
    }
    GpuScene actorGpu, scriptGpu, entryGpu;
    std::unique_ptr<NativeCarGeneratorRuntime> carGenerators;
    if (newGame) {
        carGenerators = std::make_unique<NativeCarGeneratorRuntime>(scriptHost.CarGenerators(), gameplay,
            scriptHost.Vehicles(), scriptHost.State());
        const auto rng = scriptHost.InspectSourceRng();
        assert(rng.Value);
        std::printf("play-cargens sources=%zu definitions=%zu generation=%llu seed=%u draws=%llu spawned=0\n",
            scriptHost.CarGeneratorResidency().Active().size(), scriptHost.CarGenerators().Census().Registered,
            static_cast<unsigned long long>(scriptHost.CarGeneratorResidency().Generation()), rng.Value->Seed,
            static_cast<unsigned long long>(rng.Value->DrawCount));
        WorldShotScene scriptImages;
        scriptImages.images = scriptHost.Entities().PreparedImages();
        if (!scriptGpu.UploadTextures(scriptImages) || !entryGpu.UploadTextures(scriptHost.EntryExits().PreparedModel())) {
            std::printf("play-fail script entity textures\n");
            return 1;
        }
    }
    RealtimeEnvironment environment;
    if (!environment.Load(gameDir, error, sizeof(error), hour, weather) ||
        !environment.Upload(error, sizeof(error))) {
        std::printf("play-fail environment: %s\n", error);
        return 1;
    }
    std::printf("play-environment hour=%.2f weather=%s waterTris=%d clock=%s\n", hour, weather,
                environment.GetWaterTriangleCount(), freezeTime ? "frozen" : "one-minute-per-second");
    const bool gameplayEnabled = !freecam;
    LiveWorld world;
    world.collisionContext = collisionContext;
    world.scriptHost = newGame ? &scriptHost : nullptr;
    int updates = 0;
    auto reportWorld = [&]() {
        const auto& cpu = *world.active->cpu;
        ++updates;
        std::printf("play-scene update=%d instances=%d tris=%d textures=%zu cam=%.1f,%.1f,%.1f\n",
            updates, cpu.Frame.instances, cpu.Frame.tris, cpu.Scene.images.size(),
            cpu.Position.X, cpu.Position.Y, cpu.Position.Z);
        std::fflush(stdout);
    };
    if (!(newGame ? world.Initialize(scriptHost) : world.Initialize({camera.x, camera.y, camera.z}, gameplayEnabled, true))) {
        return 1;
    }
    if (demoCurb) {
        float low{}, high{};
        if (!world.active->Collision().Ground(1540, -1736, 20, 5, low) ||
            !world.active->Collision().Ground(1540, -1740.013672f, 20, 5, high)) {
            std::printf("play-fail demo curb source COL ground missing\n");
            return 1;
        }
        std::printf("play-col-curb low=%.9f high=%.9f rise=%.9f source=COL\n", low, high, high-low);
    }
    reportWorld();
    if (gameplayEnabled) {
        const auto initStart = SDL_GetTicksNS();
        // Source-backed startup outfit; this preview does not execute the SCM
        // branch that assigns it or imply that new-game boot is complete.
        const auto clothes = NativePlayerClothes_Startup();
        if ((!newGame && (!gameplay.Initialize(gameDir, gameplayError, playerCj ? &clothes : nullptr) ||
            !gameplay.Spawn(world.active->Collision(), camera.x, camera.y, camera.z, camera.yaw, gameplayError))) ||
            !actorGpu.UploadTextures(gameplay.Actors())) {
            std::printf("play-fail gameplay: %s\n", gameplayError.c_str());
            return 1;
        }
        std::printf("play-gameplay-init seconds=%.3f collisionTris=%zu actorTris=%d textures=%zu\n",
            static_cast<double>(SDL_GetTicksNS() - initStart) / 1e9, world.active->Collision().TriangleCount(),
            gameplay.Actors().stats.triangles, gameplay.Actors().images.size());
        std::printf("play-player model=%s outfit=%s\n", (newGame || playerCj) ? "player" : "andre",
            newGame ? "base-SCM-before-clothes" : playerCj ? "startup-fat200-muscle50-preview" : "model-txd");
    }
    RealtimeHud hud;
    if (!hud.Load(gameDir, error, sizeof(error)) || !hud.Upload(error, sizeof(error))) {
        std::printf("play-fail HUD: %s\n", error);
        return 1;
    }
    std::printf("play-hud radar=144-tiles clock=game-time player=%s\n", gameplayEnabled ? "gameplay" : "hidden-freecam");
    RealtimeClouds clouds;
    if (!clouds.Load(gameDir, error, sizeof(error)) || !clouds.Upload(error, sizeof(error))) {
        std::printf("play-fail low clouds: %s\n", error);
        return 1;
    }
    NativePadFeedback padFeedback;
    const auto reportPad = [](const NativePadFeedbackResult& result) {
        if (result.Status == NativePadFeedbackStatus::Error || result.Status == NativePadFeedbackStatus::Unsupported)
            std::printf("play-pad-feedback status=%d message=%s\n", static_cast<int>(result.Status), result.Message.c_str());
    };
    reportPad(padFeedback.Initialize());
    if (newGame) {
        scriptHost.SetRadarSpriteReady([&hud](std::int32_t sprite) { return hud.IsRadarSpriteUploaded(sprite); });
        scriptHost.SealStartup(); // no live LOAD_SCENE callback yet: explicitly Unsupported
    }
    world.Start(gameplayEnabled); // final startup parser has returned; transfer exclusive pager ownership
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_ALPHA_TEST);
    glAlphaFunc(GL_GREATER, 0.5f); // TXD cutouts (foliage/fences), no opaque rectangles.
    glDisable(GL_CULL_FACE);
    glClearColor(0.38f, 0.55f, 0.72f, 1.0f);

    const Uint64 start = SDL_GetTicksNS();
    Uint64 previous = start, report = start;
    uint64_t frames = 0, reportFrames = 0;
    double reportMaxFrameMs = 0, reportMaxStreamMs = 0;
    bool running = true;
    bool demoJumped = false;
    bool demoEntered = false;
    double demoNextExitAttempt = 9.0;
    Uint64 gameNs = 0;
    Uint64 waterTicks = 0, waterPreviousNs = 0;
    float waterPreviousX = camera.x, waterPreviousY = camera.y;
    bool paused = false;
    while (running) {
        const Uint64 now = SDL_GetTicksNS();
        if (seconds > 0 && static_cast<double>(now - start) / 1e9 >= seconds) {
            break;
        }
        const Uint64 deltaNs = now - previous;
        const float dt = std::min(static_cast<float>(deltaNs) / 1e9f, 0.1f);
        previous = now;
        SDL_Event event{};
        RealtimeGameplayInput input{};
        bool collectJustDown = false;
        while (SDL_PollEvent(&event)) {
            reportPad(padFeedback.HandleEvent(event));
            if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED ||
                (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE)) {
                running = false;
            }
            if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) {
                input.Jump |= event.key.key == SDLK_SPACE;
                input.Interact |= event.key.key == SDLK_F;
                collectJustDown |= newGame && event.key.key == SDLK_TAB;
                if (event.key.key == SDLK_TAB && gameplayEnabled && !newGame) {
                    freecam = !freecam;
                }
            }
        }
        if (!running) {
            break;
        }
        int width = 0, height = 0;
        if (!SDL_GetWindowSizeInPixels(window.window, &width, &height)) {
            std::printf("play-fail window size: %s\n", SDL_GetError());
            return 1;
        }
        if (width <= 0 || height <= 0 || (SDL_GetWindowFlags(window.window) & SDL_WINDOW_MINIMIZED)) {
            reportPad(padFeedback.Update(true));
            paused = true;
            SDL_Delay(50);
            continue;
        }
        if (frames && !paused) {
            gameNs += deltaNs;
        }
        if (newGame && frames) {
            if (gameNs / 1'000'000 > std::numeric_limits<std::uint32_t>::max() ||
                !scriptHost.AdvanceTime(static_cast<std::uint32_t>(gameNs / 1'000'000), gameplayError)) {
                std::printf("play-fail script clock: %s\n", gameplayError.c_str());
                return 1;
            }
            if (ScriptFault(scriptHost, scriptHost.RunPass(scriptQuota))) {
                return 1; // no physics, presentation, retry, or main-thread resumption after fault
            }
        }
        paused = false;
        reportPad(padFeedback.Update(false));
        const bool* keys = SDL_GetKeyboardState(nullptr);
        if (freecam) {
            camera.Update(dt, keys, demo);
        }
        const double streamStart = realtime_streaming::Milliseconds();
        bool published = false;
        if (!world.Advance({camera.x, camera.y, camera.z}, published)) {
            return 1;
        }
        reportMaxStreamMs = std::max(reportMaxStreamMs, realtime_streaming::Milliseconds() - streamStart);
        if (published) {
            reportWorld();
        }
        if (gameplayEnabled && (!newGame || frames)) {
            if (!freecam) {
                input.Forward = keys[SDL_SCANCODE_W] - keys[SDL_SCANCODE_S];
                input.Side = keys[SDL_SCANCODE_D] - keys[SDL_SCANCODE_A];
                input.Sprint = keys[SDL_SCANCODE_LSHIFT];
                input.Brake = keys[SDL_SCANCODE_LCTRL];
                input.Handbrake = keys[SDL_SCANCODE_SPACE];
                input.LookYaw = (keys[SDL_SCANCODE_LEFT] - keys[SDL_SCANCODE_RIGHT]) * dt;
                input.LookPitch = (keys[SDL_SCANCODE_UP] - keys[SDL_SCANCODE_DOWN]) * dt;
                if (demo) {
                    // Use the interactive input/state path, never teleports
                    // or forced vehicle ownership to make a demo appear green.
                    input = {};
                    const double time = gameplay.State().SimulatedSeconds;
                    if (!demoJumped && time >= 0.5) {
                        input.Jump = demoJumped = true;
                    }
                    if (!demoEntered && time >= 2.0) {
                        input.Interact = demoEntered = true;
                    }
                    const auto& state = gameplay.State();
                    input.Forward = (time >= 2.2 && time < 6.0) ||
                        (time >= 10.0 && state.Exits > 0 && !state.InVehicle) ? 1.0f : 0.0f;
                    // Brake until stopped, not until an assumed stopping time.
                    // Retry a rejected exit via input; never bypass speed or
                    // collision checks, and never resume driving after failure.
                    input.Brake = time >= 6.0 && state.InVehicle;
                    if (state.InVehicle && time >= demoNextExitAttempt && std::abs(state.Speed) <= 0.8f) {
                        input.Interact = true;
                        demoNextExitAttempt = time + 0.5;
                    }
                }
                if (demoCurb) {
                    input = {};
                    const double time = gameplay.State().SimulatedSeconds;
                    if (time >= 0.5 && time < 2.5) {
                        input.Forward = 1.0f;
                    } else if (time >= 2.5 && time < 4.5) {
                        input.Forward = -1.0f;
                    } else if (time >= 5.0 && time < 6.4) {
                        input.Forward = time < 5.7 ? 1.0f : -1.0f;
                        input.Sprint = true;
                    }
                }
            } else {
                input = {};
            }
            gameplay.Tick(dt, input, world.active->Collision());
            if (!freecam) {
                const auto& view = gameplay.Camera();
                camera.x = view.Position.X;
                camera.y = view.Position.Y;
                camera.z = view.Position.Z;
                camera.yaw = view.Yaw;
                camera.pitch = view.Pitch;
            }
        }
        if (newGame) {
            const auto vehicleSnapshot = scriptHost.PublishVehicles(frames, gameplayError);
            if (!vehicleSnapshot) {
                std::printf("play-vehicle-terminal status=Unsupported message=%s\n", gameplayError.c_str());
                return 1;
            }
            camera.Apply(width, height, std::max(1600.0f, environment.GetParams().farClip));
            NativeCarGeneratorRuntimeInput generatorInput;
            generatorInput.Frame = frames;
            generatorInput.Camera = RealtimeHud::CapturePriceView(.1f, std::max(1600.0f, environment.GetParams().farClip), 60);
            const auto generatorResult = carGenerators->Tick(*world.active->cpu, generatorInput, vehicleSnapshot);
            if (generatorResult.Status != NativeScriptServiceStatus::Ready) {
                std::printf("play-cargen-terminal status=%s frame=%llu demands=%zu spawned=0 message=%s\n",
                    generatorResult.Status == NativeScriptServiceStatus::Error ? "Error" : "Unsupported",
                    static_cast<unsigned long long>(frames), carGenerators->Frame().Demands.size(), generatorResult.Message.c_str());
                return 1; // retained demand has no fulfillment owner yet; never fake a spawn
            }
            const auto garageResult = garageRuntime.Tick(*world.active->cpu, {frames}, vehicleSnapshot);
            if (garageResult.Status != NativeScriptServiceStatus::Ready) {
                std::printf("play-garage-terminal status=%s frame=%llu message=%s\n",
                    garageResult.Status == NativeScriptServiceStatus::Unsupported ? "Unsupported" : "Error",
                    static_cast<unsigned long long>(frames), garageResult.Message.c_str());
                return 1;
            }
            auto& entities = scriptHost.Entities();
            NativeScriptPropertyInput propertyInput;
            propertyInput.FrameCounter = static_cast<std::uint32_t>(frames);
            propertyInput.CollectJustDown = collectJustDown;
            if (!scriptHost.TickPlayerEntities({camera.x, camera.y, camera.z}, propertyInput, gameplayError) ||
                !entities.AdvanceTime(static_cast<std::uint32_t>(gameNs / 1'000'000), gameplayError)) {
                const auto& requirement = entities.PickupRequirement();
                if (requirement.Kind != NativeScriptPickupRequirementKind::None) {
                    std::printf("play-pickup-terminal status=Unsupported frame=%u model=%d type=%d collection-completed=0 message=%s\n",
                        requirement.FrameCounter, requirement.Model, requirement.Type, gameplayError.c_str());
                    return 1;
                }
                std::printf("play-fail script entity clock: %s\n", gameplayError.c_str());
                return 1;
            }
            while (const auto shake = entities.ConsumePadShake()) {
                // MenuManager constructor defaults vibration on; no settings UI yet.
                const auto feedback = padFeedback.Submit(*shake, true);
                reportPad(feedback);
                std::printf("play-pickup-feedback frame=%u status=%d devices=%zu applied=%zu\n",
                    shake->FrameCounter, static_cast<int>(feedback.Status), feedback.Devices, feedback.AppliedDevices);
            }
            // Each unpaused frame is published once here. Type18 does not debit
            // cash or remove the pickup: the remaining purchase belongs to SCM.
            if (entities.Interaction().Status == NativeScriptPropertyInteractionStatus::ScriptPurchaseRequired) {
                std::printf("play-property-terminal status=Unsupported frame=%u purchase-completed=0\n",
                    entities.Interaction().FrameCounter);
                return 1;
            }
        }
        camera.Apply(width, height, std::max(1600.0f, environment.GetParams().farClip));
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        if (newGame) {
            const auto& clock = scriptHost.State().Clock;
            // Source clock setter is a snapshot. Calendar pacing belongs here;
            // one game minute per unpaused second, reset at each SCM setter.
            hour = std::fmod(clock.Hours + clock.Minutes / 60.0f + clock.Seconds / 3600.0f +
                (gameNs / 1'000'000 - clock.LastTickMs) / 60000.0f, 24.0f);
            environment.SetHour(hour);
        } else if (!freezeTime) {
            hour = std::fmod(hour + dt / 60.0f, 24.0f);
            environment.SetHour(hour);
        }
        // Original capped simulation cadence, independent of SDL presentation.
        // Catch up every tick; linearly sample the observed camera trajectory
        // between presentation snapshots. Native minimization adds no game time.
        const Uint64 targetWaterTicks = gameNs / 1'000'000'000 * 30 + gameNs % 1'000'000'000 * 30 / 1'000'000'000;
        while (waterTicks < targetWaterTicks) {
            ++waterTicks;
            const Uint64 tickNs = waterTicks / 30 * 1'000'000'000 + waterTicks % 30 * 1'000'000'000 / 30;
            const float fraction = gameNs > waterPreviousNs
                ? std::clamp(static_cast<float>(tickNs - waterPreviousNs) / static_cast<float>(gameNs - waterPreviousNs), 0.0f, 1.0f) : 1.0f;
            RealtimeWaterFlowTick tick;
            tick.frame = static_cast<std::uint32_t>(waterTicks);
            tick.gameMs = static_cast<std::uint32_t>(tickNs / 1'000'000);
            tick.timeStep = 50.0f / 30.0f;
            tick.cameraX = waterPreviousX + (camera.x - waterPreviousX) * fraction;
            tick.cameraY = waterPreviousY + (camera.y - waterPreviousY) * fraction;
            if (!environment.AdvanceWaterFlow(tick)) {
                std::printf("play-fail water flow frame=%u\n", tick.frame);
                return 1;
            }
        }
        waterPreviousNs = gameNs;
        waterPreviousX = camera.x;
        waterPreviousY = camera.y;
        auto waterState = environment.GetWaterState();
        waterState.gameMs = static_cast<std::uint32_t>(gameNs / 1'000'000);
        if (!environment.SetWaterState(waterState)) {
            std::printf("play-fail water clock\n");
            return 1;
        }
        if (newGame) {
            const auto& state = gameplay.State();
            NativeEntryExitView view;
            view.Player = {state.PedRoot.X, state.PedRoot.Y, state.PedRoot.Z};
            view.Camera = {camera.x, camera.y, camera.z};
            view.Forward = {std::cos(camera.yaw)*std::cos(camera.pitch),
                std::sin(camera.yaw)*std::cos(camera.pitch), std::sin(camera.pitch)};
            view.Hour = static_cast<std::uint8_t>(hour);
            view.Vehicle = state.InVehicle ? NativeEntryExitVehicle::Automobile : NativeEntryExitVehicle::OnFoot;
            // Current native startup has no interior/mission-start task consumer.
            // Never advertise those transitions as successfully executed.
            view.CanStartMission = state.Ready;
            view.SphereVisible = [&](NativeScriptPosition point, float radius) {
                return camera.SphereVisible(point, radius, width, height, std::max(1600.0f, environment.GetParams().farClip));
            };
            auto& entries = scriptHost.EntryExits();
            if (!entries.Tick(view, waterState.gameMs, gameplayError)) {
                std::printf("play-fail entry markers: %s\n", gameplayError.c_str());
                return 1;
            }
            const auto activation = entries.Activation(view);
            if (activation.Status == NativeEntryExitActivationStatus::TransitionRequired) {
                std::printf("play-entry-terminal status=Unsupported entry=%zu destination=%zu area=%u transition-completed=0\n",
                    activation.Transition.Entry, activation.Transition.Destination, unsigned(activation.Transition.Area));
                return 1;
            }
        }
        environment.DrawSky(camera.x, camera.y, camera.z);
        RealtimeLowCloudParams lowCloudParams;
        if (!environment.GetFixedWeatherLowCloudParams(camera.z, lowCloudParams)) {
            std::printf("play-cloud-terminal status=Unsupported message=fixed-weather metadata unavailable\n");
            return 1;
        }
        const auto cloudCamera = RealtimeClouds::CaptureCamera({camera.x, camera.y, camera.z}, width, height,
            0.1f, std::max(1600.0f, environment.GetParams().farClip), 70.0f, 0.0f);
        RealtimeCloudState cloudState;
        // The native camera projection remains 60 degrees. Source sprite sizing
        // uses CDraw::FOV=70 independently; roll is genuinely zero in this camera.
        cloudState.HasExteriorVisibility = true;
        cloudState.CanSeeOutside = true; // area 0; interior transitions are still terminal
        cloudState.HasWeather = cloudState.HasClock = cloudState.HasLowCloudColours = true;
        cloudState.Foggyness = lowCloudParams.foggyness;
        cloudState.CloudCoverage = lowCloudParams.cloudCoverage;
        cloudState.ExtraSunnyness = lowCloudParams.extraSunnyness;
        cloudState.Wind = lowCloudParams.wind;
        cloudState.Hour = lowCloudParams.hour;
        cloudState.GameMs = lowCloudParams.gameMs;
        cloudState.LowCloudColours = lowCloudParams.colours;
        const auto cloudResult = clouds.Draw(cloudCamera, cloudState);
        if (cloudResult == RealtimeCloudResult::Unsupported) {
            std::printf("play-cloud-terminal status=Unsupported message=camera or cloud render state\n");
            return 1;
        }
        if (!frames) {
            std::printf("play-clouds layer=low projectionFov=60 spriteFov=70 rgb=%u,%u,%u fog=%.2f coverage=%.2f extra=%.2f status=%d\n",
                unsigned(lowCloudParams.colours[0]), unsigned(lowCloudParams.colours[1]), unsigned(lowCloudParams.colours[2]),
                lowCloudParams.foggyness, lowCloudParams.cloudCoverage, lowCloudParams.extraSunnyness, static_cast<int>(cloudResult));
        }
        environment.BeginWorld();
        world.active->gpu.Render();
        environment.EndWorld();
        if (gameplayEnabled) {
            environment.BeginObjects();
            actorGpu.DrawActors(gameplay.Actors(), GpuScene::ActorPass::Opaque);
            if (newGame) {
                scriptGpu.DrawActors(scriptHost.Entities().Actors(), GpuScene::ActorPass::Opaque);
            }
            environment.EndWorld();
        }
        // Current runtime is exterior area 0. Interior area routing remains an
        // explicit transition dependency, never an invented seabed floor.
        if (!environment.DrawSeaBed(camera.x, camera.y, 0)) {
            std::printf("play-fail seabed camera\n");
            return 1;
        }
        environment.DrawWater(camera.x, camera.y, false);
        if (gameplayEnabled) {
            environment.BeginObjects();
            actorGpu.DrawActors(gameplay.Actors(), GpuScene::ActorPass::Alpha);
            if (newGame) {
                scriptGpu.DrawActors(scriptHost.Entities().Actors(), GpuScene::ActorPass::Alpha);
            }
            environment.EndWorld();
        }
        if (newGame) {
            entryGpu.DrawMarkers(scriptHost.EntryExits().Actors());
            hud.DrawPropertyPrices(scriptHost.Entities().PriceLabels(), width, height,
                0.1f, environment.GetParams().farClip, 60.0f);
        }
        RealtimeHudView hudView;
        hudView.cameraYaw = camera.yaw;
        hudView.radar = gameplayEnabled;
        RealtimeHudState hudState;
        hudState.hour = static_cast<int>(hour);
        hudState.minute = static_cast<int>(hour * 60.0f) % 60;
        if (gameplayEnabled) {
            const auto& state = gameplay.State();
            const auto& position = state.InVehicle ? state.Car : state.Ped;
            hudState.playerX = position.X;
            hudState.playerY = position.Y;
            hudState.playerYaw = state.InVehicle ? state.CarHeading : state.PedHeading;
        }
        if (newGame) {
            const auto& entities = scriptHost.Entities();
            hudState.scriptBlips = entities.Blips();
            const auto help = entities.HelpPresentation();
            hudState.helpText = help.Text;
            hudState.helpAlpha = help.Alpha;
        }
        hud.Draw(hudView, hudState, width, height);
        if (newGame) {
            DrawScriptFade(scriptHost.State().Fade);
        }
        const auto glError = glGetError();
        if (glError != GL_NO_ERROR || !SDL_GL_SwapWindow(window.window)) {
            std::printf("play-fail present GL=0x%x SDL=%s\n", glError, SDL_GetError());
            return 1;
        }
        ++frames;
        // Always pace: swap interval may be ignored/overridden by the driver
        // or overlay. No busy-spin and no batch-waypoint pseudo-FPS.
        const Uint64 elapsed = SDL_GetTicksNS() - now;
        reportMaxFrameMs = std::max(reportMaxFrameMs, static_cast<double>(elapsed) / 1e6);
        constexpr Uint64 frameNs = 1'000'000'000 / 60;
        if (elapsed < frameNs) {
            SDL_DelayNS(frameNs - elapsed);
        }
        if (now - report >= 1'000'000'000) {
            std::printf("play-frame swaps=%llu fps=%.1f drawable=%dx%d cam=%.1f,%.1f,%.1f maxWorkMs=%.2f maxStreamMs=%.2f\n",
                static_cast<unsigned long long>(frames),
                static_cast<double>(frames - reportFrames) * 1e9 / (now - report),
                width, height, camera.x, camera.y, camera.z, reportMaxFrameMs, reportMaxStreamMs);
            std::fflush(stdout);
            if (gameplayEnabled) {
                const auto& state = gameplay.State();
                std::printf("play-state ticks=%llu mode=%s grounded=%d ped=%.2f,%.2f,%.2f car=%.2f,%.2f,%.2f "
                    "speed=%.2f gear=%d walk=%.2f drive=%.2f jumps=%llu landings=%llu entries=%llu exits=%llu anim=%s "
                    "blocked=%llu phase=%.4f moveBlend=%.3f runBlend=%.3f airBlend=%.3f\n",
                    static_cast<unsigned long long>(state.Ticks), state.InVehicle ? "car" : "foot", state.Grounded,
                    state.Ped.X, state.Ped.Y, state.Ped.Z, state.Car.X, state.Car.Y, state.Car.Z,
                    state.Speed, state.Gear, state.WalkDistance, state.DriveDistance,
                    static_cast<unsigned long long>(state.Jumps), static_cast<unsigned long long>(state.Landings),
                    static_cast<unsigned long long>(state.Entries), static_cast<unsigned long long>(state.Exits), state.Animation,
                    static_cast<unsigned long long>(state.BlockedSteps), state.LocomotionPhase,
                    state.LocomotionBlend, state.RunBlend, state.AirBlend);
                std::fflush(stdout);
            }
            report = now;
            reportFrames = frames;
            reportMaxFrameMs = reportMaxStreamMs = 0;
        }
    }
    if (newGame) {
        std::printf("play-script-stopped swaps=%llu startup-incomplete=1\n", static_cast<unsigned long long>(frames));
        return 1;
    }
    std::printf("play-ok swaps=%llu seconds=%.3f sceneUpdates=%d\n",
        static_cast<unsigned long long>(frames), static_cast<double>(SDL_GetTicksNS() - start) / 1e9, updates);
    return 0;
}
