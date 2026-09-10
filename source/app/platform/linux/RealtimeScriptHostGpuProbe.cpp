// Same renderer and HUD as production; surfaceless readback, no asset dumps.
#ifndef GL_GLEXT_PROTOTYPES
#define GL_GLEXT_PROTOTYPES 1
#endif
#include "app/platform/linux/Realtime.cpp"
#include <EGL/eglext.h>
#include <stdexcept>

namespace {
std::unique_ptr<RealtimeHud> s_ProbeHud;
void CheckGpu(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "script-entities GPU FAIL %s\n", message); std::exit(2); }
}
void ProbeMarkerCamera() {
    Camera camera; camera.x = camera.y = camera.z = camera.yaw = camera.pitch = 0;
    // Independent 60-degree projection boundaries, in this camera's +X view.
    const auto visible = [&](float depth, float side, float up, float radius = 0) {
        return camera.SphereVisible({depth,-side,up},radius,640,448,1000);
    };
    CheckGpu(!visible(0.099f,0,0) && visible(0.101f,0,0) && visible(999.99f,0,0) && !visible(1000.01f,0,0),
        "actual camera near=0.1/far=1000 point clipping");
    CheckGpu(visible(0.05f,0,0,0.06f) && !visible(0.05f,0,0,0.04f) && visible(1000.5f,0,0,0.6f) && !visible(1000.5f,0,0,0.4f),
        "actual camera sphere/near/far-plane intersection");
    CheckGpu(visible(10,0,5.77f) && !visible(10,0,5.78f) && visible(10,8.24f,0) && !visible(10,8.25f,0) &&
        !visible(10,0,6.5f), "actual 60-degree frustum excludes old 70-degree-only point");
    CheckGpu(visible(10,0,6.9f,1) && !visible(10,0,6.94f,1) && visible(10,9.5f,0,1) && !visible(10,9.56f,0,1),
        "actual camera normalized sloping-plane sphere radius");
    camera.yaw = 1.57079632679f; camera.pitch = 0.4f;
    CheckGpu(camera.SphereVisible({0,10*std::cos(camera.pitch),10*std::sin(camera.pitch)},0,640,448,1000) &&
        !camera.SphereVisible({0,-10*std::cos(camera.pitch),-10*std::sin(camera.pitch)},0,640,448,1000),
        "actual frustum follows camera yaw and pitch");
    std::printf("enex camera PASS fov=60 near=0.1 far=1000 sphere-planes=normalized rotated=1\n");
}

void ProbeMarkerPass(GpuScene& gpu, const WorldShotScene& bind, const WorldShotScene& actors, const Camera& camera) {
    constexpr int width = 640, height = 448;
    const auto pixels = [] {
        std::vector<std::uint8_t> data(width*height*4); glReadPixels(0,0,width,height,GL_RGBA,GL_UNSIGNED_BYTE,data.data()); return data;
    };
    const auto depth = [] {
        std::vector<float> data(width*height); glReadPixels(0,0,width,height,GL_DEPTH_COMPONENT,GL_FLOAT,data.data()); return data;
    };
    const auto changed = [](const auto& a, const auto& b) {
        std::size_t count = 0; for (std::size_t i=0;i<a.size();i+=4) count += !std::equal(a.begin()+i,a.begin()+i+4,b.begin()+i); return count;
    };
    const auto reset = [&] {
        glUseProgram(0); glDisable(GL_LIGHTING); glDisable(GL_COLOR_SUM); glDisable(GL_FOG); glDisable(GL_SCISSOR_TEST);
        for (int unit=0;unit<4;++unit) { glActiveTexture(GL_TEXTURE0+unit); glDisable(GL_TEXTURE_2D); }
        glActiveTexture(GL_TEXTURE0); glDisable(GL_BLEND); glDisable(GL_ALPHA_TEST); glDisable(GL_CULL_FACE);
        glEnable(GL_DEPTH_TEST); glDepthFunc(GL_LESS); glDepthMask(GL_TRUE); glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
        glPolygonMode(GL_FRONT_AND_BACK,GL_FILL); glClearDepth(1); glClearColor(0.1f,0.12f,0.15f,1);
        glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT); camera.Apply(width,height,1000);
    };
    std::size_t ccwNormals=0,cwNormals=0,zeroArea=0,orthogonalNormals=0,zeroTexels=0;
    double signedVolume=0;
    for (const auto& mesh:bind.meshes) for (int t=0;t<mesh.tris;++t) {
        const auto* p=mesh.pos.data()+t*9; const auto* n=mesh.nrm.data()+t*9;
        const double ux=p[3]-p[0],uy=p[4]-p[1],uz=p[5]-p[2],vx=p[6]-p[0],vy=p[7]-p[1],vz=p[8]-p[2];
        const double nx=uy*vz-uz*vy,ny=uz*vx-ux*vz,nz=ux*vy-uy*vx;
        const double dot=nx*(n[0]+n[3]+n[6])+ny*(n[1]+n[4]+n[7])+nz*(n[2]+n[5]+n[8]);
        signedVolume+=(p[0]*nx+p[1]*ny+p[2]*nz)/6;
        if (nx*nx+ny*ny+nz*nz<=1e-16) ++zeroArea;
        else if (dot>1e-8) ++ccwNormals; else if (dot < -1e-8) ++cwNormals; else ++orthogonalNormals;
    }
    for (const auto& image:bind.images) for (std::size_t i=3;i<image.rgba.size();i+=4) zeroTexels += image.rgba[i]==0;
    reset(); const auto clear=pixels(); const auto untouchedDepth=depth(); gpu.DrawMarkers(actors); const auto production=pixels();
    CheckGpu(depth()==untouchedDepth,"DrawMarkers preserves every depth-buffer sample with visible actual DFF");
    const auto reference = [&](GLenum front) {
        reset(); glEnable(GL_CULL_FACE); glCullFace(GL_BACK); glFrontFace(front); glDepthMask(GL_FALSE);
        glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA); glEnable(GL_ALPHA_TEST); glAlphaFunc(GL_GREATER,0);
        glTexEnvi(GL_TEXTURE_ENV,GL_TEXTURE_ENV_MODE,GL_MODULATE);
        gpu.Draw(actors); return pixels(); // independent state oracle deliberately uses bare Draw
    };
    const auto ccw=reference(GL_CCW),cw=reference(GL_CW);
    std::printf("enex winding actual-DFF triangles=%d normalsCCW=%zu normalsCW=%zu zeroArea=%zu orthogonalNormals=%zu signedVolume=%.9f CCWpixels=%zu CWpixels=%zu windingEffect=%zu zeroAlphaTexels=%zu\n",
        bind.stats.triangles,ccwNormals,cwNormals,zeroArea,orthogonalNormals,signedVolume,changed(clear,ccw),changed(clear,cw),changed(ccw,cw),zeroTexels);
    CheckGpu(ccwNormals>0 && !cwNormals && production==ccw && changed(ccw,cw)>30,
        "actual DFF winding agrees with source normals and dedicated CCW backface-cull pass");

    // Snapshot current attributes as well as render switches: Draw emits normals,
    // colors and four texture coordinates even though it does not change ZWRITE.
    const auto snapshot = [] {
        std::vector<GLint> ints; std::vector<GLfloat> floats;
        for (auto p : {GL_CURRENT_PROGRAM,GL_ACTIVE_TEXTURE,GL_MATRIX_MODE,GL_DEPTH_WRITEMASK,GL_DEPTH_FUNC,GL_CULL_FACE_MODE,GL_FRONT_FACE,
            GL_BLEND_SRC_RGB,GL_BLEND_DST_RGB,GL_BLEND_SRC_ALPHA,GL_BLEND_DST_ALPHA,GL_ALPHA_TEST_FUNC}) {
            GLint v{}; glGetIntegerv(p,&v); ints.push_back(v);
        }
        for (auto p : {GL_DEPTH_TEST,GL_CULL_FACE,GL_BLEND,GL_ALPHA_TEST,GL_LIGHTING,GL_COLOR_SUM,GL_FOG,GL_SCISSOR_TEST}) ints.push_back(glIsEnabled(p));
        GLint viewport[4],scissor[4],polygon[2]; glGetIntegerv(GL_VIEWPORT,viewport); glGetIntegerv(GL_SCISSOR_BOX,scissor); glGetIntegerv(GL_POLYGON_MODE,polygon);
        ints.insert(ints.end(),viewport,viewport+4); ints.insert(ints.end(),scissor,scissor+4); ints.insert(ints.end(),polygon,polygon+2);
        GLfloat value[16]{};
        for (auto p : {GL_MODELVIEW_MATRIX,GL_PROJECTION_MATRIX}) { glGetFloatv(p,value); floats.insert(floats.end(),value,value+16); }
        for (auto p : {GL_CURRENT_COLOR,GL_CURRENT_SECONDARY_COLOR}) { glGetFloatv(p,value); floats.insert(floats.end(),value,value+4); }
        glGetFloatv(GL_CURRENT_NORMAL,value); floats.insert(floats.end(),value,value+3);
        glGetFloatv(GL_ALPHA_TEST_REF,value); floats.push_back(value[0]);
        GLint active{}; glGetIntegerv(GL_ACTIVE_TEXTURE,&active);
        for (int unit=0;unit<4;++unit) {
            glActiveTexture(GL_TEXTURE0+unit); ints.push_back(glIsEnabled(GL_TEXTURE_2D));
            GLint binding{},environment{}; glGetIntegerv(GL_TEXTURE_BINDING_2D,&binding); glGetTexEnviv(GL_TEXTURE_ENV,GL_TEXTURE_ENV_MODE,&environment);
            ints.push_back(binding); ints.push_back(environment);
            glGetFloatv(GL_CURRENT_TEXTURE_COORDS,value); floats.insert(floats.end(),value,value+4);
            glGetFloatv(GL_TEXTURE_MATRIX,value); floats.insert(floats.end(),value,value+16);
        }
        glActiveTexture(active); return std::pair(ints,floats);
    };
    const auto shader = [](GLenum type,const char* text) {
        const auto object=glCreateShader(type); glShaderSource(object,1,&text,nullptr); glCompileShader(object); GLint ok{};
        glGetShaderiv(object,GL_COMPILE_STATUS,&ok); CheckGpu(ok,"hostile caller shader compile"); return object;
    };
    const auto vertex=shader(GL_VERTEX_SHADER,"#version 120\nvoid main(){gl_Position=ftransform();}");
    const auto fragment=shader(GL_FRAGMENT_SHADER,"#version 120\nvoid main(){gl_FragColor=vec4(0,1,1,1);}");
    const auto program=glCreateProgram(); glAttachShader(program,vertex); glAttachShader(program,fragment); glLinkProgram(program);
    GLint linked{}; glGetProgramiv(program,GL_LINK_STATUS,&linked); CheckGpu(linked,"hostile caller shader link");
    reset(); glUseProgram(program); glEnable(GL_LIGHTING); glEnable(GL_LIGHT0); glEnable(GL_COLOR_SUM); glSecondaryColor3f(1,0,1);
    const GLfloat dark[]{0,0,0,1}; glLightModelfv(GL_LIGHT_MODEL_AMBIENT,dark);
    glDisable(GL_DEPTH_TEST); glDepthMask(GL_TRUE); glEnable(GL_CULL_FACE); glCullFace(GL_FRONT); glFrontFace(GL_CW);
    glDisable(GL_BLEND); glBlendFuncSeparate(GL_ONE,GL_ZERO,GL_ZERO,GL_ONE); glEnable(GL_ALPHA_TEST); glAlphaFunc(GL_NEVER,0.75f);
    glColor4f(0.2f,0.3f,0.4f,0.5f); glNormal3f(0.2f,0.4f,0.6f);
    glActiveTexture(GL_TEXTURE1); glMatrixMode(GL_TEXTURE);
    const auto caller=snapshot(); gpu.DrawMarkers(actors); const auto bright=pixels(); const auto restored=snapshot();
    std::printf("enex marker-state callerRestored=%d fullbrightEqual=%d depthUnchanged=%d\n",caller==restored,bright==production,depth()==untouchedDepth);
    CheckGpu(caller==restored && bright==production && depth()==untouchedDepth && glGetError()==GL_NO_ERROR,
        "DrawMarkers fullbright material ignores caller shader/light/color-sum and restores caller GL state");
    glUseProgram(0); glDeleteProgram(program); glDeleteShader(vertex); glDeleteShader(fragment); glDisable(GL_LIGHT0);

    // Real foreground geometry writes the left half of the depth buffer. The
    // caller then disables depth testing: DrawMarkers must enable it internally.
    reset(); glMatrixMode(GL_MODELVIEW); glPushMatrix(); glLoadIdentity(); glColor4f(0.3f,0.4f,0.5f,1);
    glBegin(GL_QUADS); glVertex3f(-100,-100,-2); glVertex3f(0,-100,-2); glVertex3f(0,100,-2); glVertex3f(-100,100,-2); glEnd(); glPopMatrix();
    const auto foreground=pixels(); const auto foregroundDepth=depth(); glDisable(GL_DEPTH_TEST); gpu.DrawMarkers(actors); const auto occluded=pixels();
    std::size_t blocked=0,leaked=0,visible=0;
    for (std::size_t i=0;i<foregroundDepth.size();++i) {
        const bool differs=!std::equal(foreground.begin()+i*4,foreground.begin()+i*4+4,occluded.begin()+i*4);
        if (foregroundDepth[i]<1) { ++blocked; leaked+=differs; } else visible+=differs;
    }
    CheckGpu(blocked>100 && leaked==0 && visible>100 && depth()==foregroundDepth && !glIsEnabled(GL_DEPTH_TEST),
        "foreground object occludes tested markers, visible remainder writes no depth, caller disabled test restored");
    reset(); auto transparent=actors; for (auto& mesh:transparent.meshes) for (auto& material:mesh.surfaces) material.color[3]=0;
    GLuint query{}; glGenQueries(1,&query); glBeginQuery(GL_SAMPLES_PASSED,query); gpu.DrawMarkers(transparent); glEndQuery(GL_SAMPLES_PASSED);
    GLuint samples{}; glGetQueryObjectuiv(query,GL_QUERY_RESULT,&samples); glDeleteQueries(1,&query);
    CheckGpu(samples==0 && pixels()==clear && depth()==untouchedDepth,"zero-alpha marker fragments rejected before samples/depth/color publication");
    std::printf("enex marker-pass PASS depthTest=on depthWrite=off cull=back front=CCW fullbright=1 callerGL=preserved foregroundPixels=%zu leaks=%zu visible=%zu zeroAlphaSamples=%u\n",
        blocked,leaked,visible,samples);
    reset();
}
}
void RealtimeScriptHostGpuPrepare(const char* dir) {
    s_ProbeHud = std::make_unique<RealtimeHud>();
    char error[512]{};
    CheckGpu(s_ProbeHud->Load(dir, error, sizeof(error)), error);
}
void RealtimeScriptPickupGpuProbe(RealtimeScriptHost& host, NativeScriptPickupRef ref) {
    const auto getDisplay = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
    CheckGpu(getDisplay,"pickup surfaceless EGL entry");
    const auto display=getDisplay(EGL_PLATFORM_SURFACELESS_MESA,EGL_DEFAULT_DISPLAY,nullptr);
    CheckGpu(display!=EGL_NO_DISPLAY && eglInitialize(display,nullptr,nullptr) && eglBindAPI(EGL_OPENGL_API),"pickup EGL init");
    const EGLint attributes[]{EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_BIT,
        EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_DEPTH_SIZE,24,EGL_NONE};
    EGLConfig config{}; EGLint count{};
    CheckGpu(eglChooseConfig(display,attributes,&config,1,&count) && count,"pickup EGL config");
    constexpr int width=640,height=448;
    const EGLint dimensions[]{EGL_WIDTH,width,EGL_HEIGHT,height,EGL_NONE};
    const auto surface=eglCreatePbufferSurface(display,config,dimensions);
    const auto context=eglCreateContext(display,config,EGL_NO_CONTEXT,nullptr);
    CheckGpu(surface!=EGL_NO_SURFACE && context!=EGL_NO_CONTEXT && eglMakeCurrent(display,surface,surface,context),"pickup EGL context");
    {
        auto& entities=host.Entities(); const auto* pickup=entities.ResolvePickup(ref);
        CheckGpu(pickup && pickup->Type==3 && pickup->Model==1277,"GPU consumes actual SCM-created save reference");
        const auto p=pickup->Position;
        Camera camera; camera.x=p.X+3; camera.y=p.Y-4; camera.z=p.Z+2;
        camera.yaw=std::atan2(4.0f,-3.0f); camera.pitch=std::atan2(-2.0f,5.0f);
        NativeScriptPropertyInput input; input.FrameCounter=4; std::string error;
        CheckGpu(host.TickProperties({camera.x,camera.y,camera.z},true,input,error) && entities.AdvanceTime(512,error),error.c_str());
        const auto& bind=entities.PreparedSaveModel(); const auto& geometry=entities.SaveGeometry();
        const auto offset=int(entities.PreparedModel().images.size()+entities.PreparedForSaleModel().images.size());
        const auto remap=[&](WorldShotScene scene) {
            for(auto& mesh:scene.meshes) for(auto& image:mesh.triImg) if(image>=0) image+=offset;
            return scene;
        };
        const auto actor=remap(pickup->Actor);
        bool published=true;
        for (const auto& mesh:actor.meshes) published &= std::ranges::any_of(entities.Actors().meshes,[&](const auto& m) { return m.pos==mesh.pos && m.triImg==mesh.triImg; });
        CheckGpu(published,"same source-phase save geometry/image indices present in production aggregate");
        GpuScene gpu; WorldShotScene images; images.images=entities.PreparedImages();
        CheckGpu(gpu.UploadTextures(images),"complete immutable startup image publication");
        const auto draw=[&](const WorldShotScene& scene) {
            glEnable(GL_DEPTH_TEST); glDepthMask(GL_TRUE); glClearColor(.1f,.12f,.15f,1);
            glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT); camera.Apply(width,height,1000); gpu.DrawActors(scene);
            std::vector<std::uint8_t> pixels(width*height*4); glReadPixels(0,0,width,height,GL_RGBA,GL_UNSIGNED_BYTE,pixels.data());
            CheckGpu(glGetError()==GL_NO_ERROR,"pickup production GPU readback"); return pixels;
        };
        const auto difference=[](const auto& a,const auto& b) { std::size_t n=0; for(std::size_t i=0;i<a.size();i+=4) n+=!std::equal(a.begin()+i,a.begin()+i+3,b.begin()+i); return n; };
        const auto clear=draw({}), rendered=draw(actor);
        const auto coverage=difference(clear,rendered);
        CheckGpu(coverage>100,"actual pickupsave triangles reach production GPU pixels");
        auto flat=actor; for(auto& mesh:flat.meshes) std::fill(mesh.triImg.begin(),mesh.triImg.end(),-1);
        const auto textureEffect=difference(rendered,draw(flat)); CheckGpu(textureEffect>30,"actual icons4 save texels affect pixels");
        auto white=actor; for(auto& mesh:white.meshes) for(auto& material:mesh.surfaces) material.color={1,1,1,1};
        const auto materialEffect=difference(rendered,draw(white));
        CheckGpu(materialEffect>30,"actual save material colors affect production pixels");
        const auto locked=NativeScriptPropertyActor(entities.PreparedModel(),p,entities.PropertyGeometry().Scale,512);
        const auto lockedDelta=difference(rendered,draw(locked)); CheckGpu(lockedDelta>100,"save token is visibly different from locked-property alias");
        const auto scaleEffect=difference(rendered,draw(remap(NativeScriptPropertyActor(bind,p,1,512))));
        CheckGpu(scaleEffect>100,"actual save COL normalization affects GPU coverage");
        // Independent scalar oracle: source largest COL extent, 60% fraction,
        // stored float angle and basis. No call to the product pose helper.
        const double extent=std::max({geometry.ColMax[0]-geometry.ColMin[0],geometry.ColMax[1]-geometry.ColMin[1],geometry.ColMax[2]-geometry.ColMin[2]});
        const float scale=float(1.0+double(.6f)*(double(float(std::max(1.0,double(1.2f)/extent)))-1.0));
        const double angle=1.565000057220459;
        const float c=float(std::cos(angle)),s=float(std::sin(angle));
        bool pose=true,normal=true,materials=true;
        for(std::size_t m=0;m<bind.meshes.size();++m) {
            const auto& b=bind.meshes[m]; const auto& a=pickup->Actor.meshes[m];
            for(std::size_t i=0;i<b.pos.size();i+=3) {
                pose &= std::abs(a.pos[i]-(p.X+scale*c*b.pos[i]-scale*s*b.pos[i+1]))<.0003f &&
                    std::abs(a.pos[i+1]-(p.Y+scale*s*b.pos[i]+scale*c*b.pos[i+1]))<.0003f &&
                    std::abs(a.pos[i+2]-(p.Z+scale*b.pos[i+2]))<.0001f;
                normal &= std::abs(a.nrm[i]-(c*b.nrm[i]-s*b.nrm[i+1]))<.0001f &&
                    std::abs(a.nrm[i+1]-(s*b.nrm[i]+c*b.nrm[i+1]))<.0001f;
            }
            for(std::size_t i=0;i<b.surfaces.size();++i) materials &= a.surfaces[i].color==b.surfaces[i].color;
        }
        CheckGpu(pose && normal && materials && scale==geometry.Scale,"independent source pose/normals/materials/COL fraction for every actual save vertex");
        CheckGpu(entities.AdvanceTime(1024,error),error.c_str());
        const auto rotated=remap(pickup->Actor); const auto rotationEffect=difference(rendered,draw(rotated));
        CheckGpu(rotationEffect>100,"save source time rotation updates actual GPU pixels");
        std::printf("save GPU PASS model=1277 type=3 tris=%d images=%zu coverage=%zu textureEffect=%zu materialEffect=%zu lockedDelta=%zu scale=%.9f scaleEffect=%zu rotationEffect=%zu collection=UNSUPPORTED-task-authority\n",
            bind.stats.triangles,bind.images.size(),coverage,textureEffect,materialEffect,lockedDelta,scale,scaleEffect,rotationEffect);
    }
    CheckGpu(eglMakeCurrent(display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT),"pickup release context");
    eglDestroyContext(display,context); eglDestroySurface(display,surface); eglTerminate(display);
}
void RealtimeScriptHostGpuProbe(NativeScriptEntities& entities, RealtimeScriptHost& host) {
    const auto getDisplay = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
    CheckGpu(getDisplay, "surfaceless EGL entry point");
    const auto display = getDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
    CheckGpu(display != EGL_NO_DISPLAY && eglInitialize(display, nullptr, nullptr) && eglBindAPI(EGL_OPENGL_API), "EGL initialize");
    const EGLint attributes[]{EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_DEPTH_SIZE, 24, EGL_NONE};
    EGLConfig config{}; EGLint count{};
    CheckGpu(eglChooseConfig(display, attributes, &config, 1, &count) && count, "EGL config");
    constexpr int width = 640, height = 448;
    const EGLint size[]{EGL_WIDTH, width, EGL_HEIGHT, height, EGL_NONE};
    const auto surface = eglCreatePbufferSurface(display, config, size);
    const auto context = eglCreateContext(display, config, EGL_NO_CONTEXT, nullptr);
    CheckGpu(surface != EGL_NO_SURFACE && context != EGL_NO_CONTEXT && eglMakeCurrent(display, surface, surface, context), "EGL context");
    std::printf("script-entities GPU renderer=%s\n", glGetString(GL_RENDERER));
    const auto readback = [] {
        std::vector<uint8_t> pixels(width * height * 4);
        glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        CheckGpu(glGetError() == GL_NO_ERROR, "production readback GL errors"); return pixels;
    };
    const auto difference = [](const auto& a, const auto& b) {
        std::size_t changed = 0;
        for (std::size_t i = 0; i < a.size(); i += 4) changed += a[i] != b[i] || a[i+1] != b[i+1] || a[i+2] != b[i+2];
        return changed;
    };
    const auto& pickup = entities.Pickups()[0];
    const auto p = pickup.Position;
    {
        ProbeMarkerCamera();
        auto& enex = host.EntryExits(); const auto& entrance = enex.Entries()[46];
        const auto center = entrance.Center;
        GpuScene gpu; CheckGpu(gpu.UploadTextures(enex.PreparedModel()), "prepared actual diamond_3/DIAMOND images");
        Camera camera; camera.x = center.X+3; camera.y = center.Y-5; camera.z = center.Z+3;
        camera.yaw = std::atan2(5.0f,-3.0f); camera.pitch = std::atan2(-2.0f,std::sqrt(34.0f));
        NativeEntryExitView view;
        view.Player = center; view.Camera = {camera.x,camera.y,camera.z}; view.Hour = 8; view.Area = 0; view.CanStartMission = true;
        view.Forward = {std::cos(camera.yaw)*std::cos(camera.pitch),std::sin(camera.yaw)*std::cos(camera.pitch),std::sin(camera.pitch)};
        view.SphereVisible = [&](NativeScriptPosition q, float radius) {
            return camera.SphereVisible(q,radius,width,height,1000);
        };
        std::string error; std::uint32_t frame = 0;
        const auto draw = [&] {
            CheckGpu(enex.Tick(view,frame++,error),error.c_str());
            glEnable(GL_DEPTH_TEST); glClearColor(0.1f,0.12f,0.15f,1); glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
            camera.Apply(width,height,1000); gpu.DrawMarkers(enex.Actors()); return readback();
        };
        CheckGpu(enex.Activation(view).Status == NativeEntryExitActivationStatus::None,"real 09B4-disabled entry cannot activate");
        const auto disabled = draw();
        CheckGpu(enex.VisibleEntries().empty(),"real 09B4-disabled entry has no drawable");
        NativeScriptEntryExitFlagRequest request{{901,1,1},center.X,center.Y,1,0x4000,1};
        CheckGpu(host.SetEntryExitFlag(request).Status == NativeScriptServiceStatus::Ready,"real host ENEX enable");
        const auto revision = enex.Revision();
        CheckGpu(host.SetEntryExitFlag(request).Status == NativeScriptServiceStatus::Ready && enex.Revision() == revision,"host ENEX replay no second effect");
        const auto transition = enex.Activation(view);
        CheckGpu(transition.Status == NativeEntryExitActivationStatus::TransitionRequired && transition.Transition.Entry == 46 &&
            transition.Transition.Destination == 370 && transition.Transition.Exit == enex.Entries()[370].Exit &&
            transition.Transition.Area == 8,"enabled same registry emits real typed transition, no completed teleport");
        const auto visible = draw(); const auto coverage = difference(disabled,visible);
        CheckGpu(coverage > 100 && enex.VisibleEntries().size() == 1 && enex.VisibleEntries()[0] == 46,"actual ENEX diamond reaches production GpuScene pixels");
        const auto firstVertices = enex.Actors().meshes[0].pos;
        CheckGpu(enex.Tick(view,frame-1,error) && enex.Actors().meshes[0].pos == firstVertices,"duplicate frame does not shrink first-use marker a second time");
        const auto bind = enex.PreparedModel().meshes[0].pos;
        CheckGpu(std::abs(firstVertices[0]-(center.X+bind[0]*2))<0.0001f && std::abs(firstVertices[1]-(center.Y+bind[1]*2))<0.0001f &&
            std::abs(firstVertices[2]-(center.Z+1+std::sin(0.25f*0.01745329252f*0.3f)+bind[2]*2))<0.0001f,
            "actual first source pose: identity frame, size2, authored ENEX Z+1, source new-cone bob");
        auto flat = enex.Actors(); for (auto& mesh : flat.meshes) std::fill(mesh.triImg.begin(),mesh.triImg.end(),-1);
        glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT); gpu.DrawMarkers(flat);
        const auto textureEffect = difference(visible,readback());
        CheckGpu(textureEffect > 30,"actual DIAMOND texels affect ENEX pixels");
        frame = 512; const auto bobbed = draw(); const auto poseEffect = difference(visible,bobbed);
        CheckGpu(poseEffect > 50,"source marker scale/bob geometry affects actual pixels");
        ProbeMarkerPass(gpu,enex.PreparedModel(),enex.Actors(),camera);
        const auto acceptedVertices = enex.Actors().meshes[0].pos;
        CheckGpu(!enex.Tick(view,511,error) && enex.Actors().meshes[0].pos==acceptedVertices,"stale frame cannot publish partial ENEX actors");
        const auto frustum=view.SphereVisible;
        view.SphereVisible=[](NativeScriptPosition,float)->bool { throw std::runtime_error("probe camera exception"); };
        CheckGpu(!enex.Tick(view,frame,error) && enex.Actors().meshes[0].pos==acceptedVertices,"camera failure rolls actor/phase publication back");
        view.SphereVisible=[](NativeScriptPosition,float) { return false; }; CheckGpu(draw()==disabled,"sphere visibility gates actual marker pixels"); view.SphereVisible=frustum;
        view.Area=1; CheckGpu(draw()==disabled,"linked entry area gates actual marker pixels"); view.Area=0;
        view.TransitionState=1; CheckGpu(draw()==disabled && enex.Activation(view).Status==NativeEntryExitActivationStatus::None,"transition state gates both consumers"); view.TransitionState=0;
        const auto cameraPosition=view.Camera;
        const auto positionCamera = [&](NativeScriptPosition q) { view.Camera=q; camera.x=q.X; camera.y=q.Y; camera.z=q.Z; };
        positionCamera({cameraPosition.X,cameraPosition.Y,center.Z+40}); CheckGpu(draw()==disabled,"camera 3D distance cutoff excludes 40 units"); positionCamera(cameraPosition);
        positionCamera({center.X,center.Y,center.Z+1}); CheckGpu(draw()==disabled,"PlaceMarkerCone near-camera 1.6-unit exclusion"); positionCamera(cameraPosition);
        for (auto* flag : {&view.Cutscene,&view.Coop,&view.Replay,&view.Disabled}) {
            *flag=true; CheckGpu(draw()==disabled && enex.Activation(view).Status==NativeEntryExitActivationStatus::None,"source global gate removes both consumers"); *flag=false;
        }
        view.ControlsDisabled = true; CheckGpu(draw() == disabled && enex.Activation(view).Status == NativeEntryExitActivationStatus::None,"global controls gate suppresses marker and activation");
        view.ControlsDisabled = false;
        request.Id.Instruction++; request.State = 0;
        CheckGpu(host.SetEntryExitFlag(request).Status == NativeScriptServiceStatus::Ready && draw() == disabled &&
            enex.Activation(view).Status == NativeEntryExitActivationStatus::None,"09B4 disable removes actual GPU drawable and eligibility");
        std::printf("enex GPU PASS model=%d tris=%d coverage=%zu textureEffect=%zu poseEffect=%zu activation=TransitionRequired destination=370 completed=0\n",
            NativeEntryExits::MarkerModel,enex.PreparedModel().stats.triangles,coverage,textureEffect,poseEffect);
    }
    {
        GpuScene gpu;
        CheckGpu(gpu.UploadTextures(entities.PreparedModel()), "actual prepared pickup textures");
        Camera camera;
        camera.x = p.X + 3; camera.y = p.Y - 4; camera.z = p.Z + 2;
        camera.yaw = std::atan2(p.Y - camera.y, p.X - camera.x);
        camera.pitch = std::atan2(p.Z - camera.z, 5.0f);
        glEnable(GL_DEPTH_TEST); glClearColor(0.1f, 0.12f, 0.15f, 1);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT); camera.Apply(width, height, 1000);
        const auto clear = readback();
        gpu.Draw(entities.Actors());
        const auto initial = readback();
        const auto coverage = difference(clear, initial);
        CheckGpu(coverage > 100, "actual GpuScene property mesh visible");
        std::string frameError;
        CheckGpu(entities.AdvanceTime(512, frameError), "source rotation time advance");
        CheckGpu(entities.Actors().meshes[0].pos == pickup.Actor.meshes[0].pos, "current phase reaches visible production scene without latency");
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT); gpu.Draw(entities.Actors());
        const auto textured = readback();
        const auto rotationEffect = difference(initial, textured);
        CheckGpu(rotationEffect > 50, "source time-driven matrix changes actual GPU pixels");
        const auto unscaled = NativeScriptPropertyActor(entities.PreparedModel(), p, 1, 512);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT); gpu.Draw(unscaled);
        const auto scaleEffect = difference(textured, readback());
        CheckGpu(scaleEffect > 100, "source COL normalization changes actual GPU geometry coverage");
        auto flat = pickup.Actor;
        for (auto& mesh : flat.meshes) std::fill(mesh.triImg.begin(), mesh.triImg.end(), -1);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT); gpu.Draw(flat);
        const auto textureEffect = difference(textured, readback());
        CheckGpu(textureEffect > 30, "prepared icons4 texels affect actual runtime pixels");
        std::printf("script-entities GPU PASS model=%s tris=%d coverage=%zu textureEffect=%zu rotationEffect=%zu colScale=%.9f scaleEffect=%zu\n",
            pickup.Actor.stats.dffName, pickup.Actor.stats.triangles, coverage, textureEffect, rotationEffect, entities.PropertyGeometry().Scale, scaleEffect);
    }
    {
        const auto& sale = entities.Pickups()[3];
        CheckGpu(sale.Active && sale.Type==18 && sale.Model==1273,"real SCM sale instance, not locked alias");
        GpuScene gpu; WorldShotScene prepared; prepared.images=entities.PreparedImages();
        CheckGpu(gpu.UploadTextures(prepared),"combined locked+sale owned image indices uploaded before draw");
        const auto q=sale.Position;
        Camera camera; camera.x=q.X+3; camera.y=q.Y-4; camera.z=q.Z+2;
        camera.yaw=std::atan2(-camera.y+q.Y,-camera.x+q.X); camera.pitch=std::atan2(-2.0f,5.0f);
        std::string error;
        entities.Tick(q,{camera.x,camera.y,camera.z},true,false);
        CheckGpu(entities.AdvanceTime(512,error),error.c_str());
        const auto draw=[&](const WorldShotScene& scene) {
            glEnable(GL_DEPTH_TEST); glDepthMask(GL_TRUE); glClearColor(.1f,.12f,.15f,1);
            glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT); camera.Apply(width,height,1000);
            gpu.DrawActors(scene); return readback();
        };
        const auto clear=draw({}), visible=draw(entities.Actors());
        const auto coverage=difference(clear,visible);
        CheckGpu(coverage>100 && entities.Actors().stats.triangles==sale.Actor.stats.triangles,"actual type18 mesh reaches production DrawActors pixels");
        const auto& bind=entities.PreparedForSaleModel(); const auto& geometry=entities.ForSaleGeometry();
        const float extent=std::max({geometry.ColMax[0]-geometry.ColMin[0],geometry.ColMax[1]-geometry.ColMin[1],geometry.ColMax[2]-geometry.ColMin[2]});
        const float ratio=std::max(1.0f,float(1.2f/extent));
        const float scale=float(1.0+double(.6f)*double(ratio-1.0f));
        const double angle=1.565000057220459;
        bool pose=true,normal=true,material=true;
        for (std::size_t m=0;m<bind.meshes.size();++m) {
            const auto& source=bind.meshes[m]; const auto& actor=sale.Actor.meshes[m];
            for (std::size_t i=0;i<source.pos.size();i+=3) {
                pose &= std::abs(actor.pos[i]-float(q.X+scale*(source.pos[i]*std::cos(angle)-source.pos[i+1]*std::sin(angle))))<.0002f &&
                    std::abs(actor.pos[i+1]-float(q.Y+scale*(source.pos[i]*std::sin(angle)+source.pos[i+1]*std::cos(angle))))<.0002f &&
                    std::abs(actor.pos[i+2]-(q.Z+scale*source.pos[i+2]))<.00002f;
                normal &= std::abs(actor.nrm[i]-float(source.nrm[i]*std::cos(angle)-source.nrm[i+1]*std::sin(angle)))<.00001f &&
                    std::abs(actor.nrm[i+1]-float(source.nrm[i]*std::sin(angle)+source.nrm[i+1]*std::cos(angle)))<.00001f;
            }
            for (std::size_t i=0;i<source.surfaces.size();++i) material &= source.surfaces[i].color==actor.surfaces[i].color;
        }
        CheckGpu(pose && normal && material && geometry.ColHeaderId==1273 && std::abs(geometry.Scale-scale)<.00001f,
            "all actual sale vertices/normals/materials follow independent source 60-percent COL normalization and phase");
        auto flat=entities.Actors(); for(auto& mesh:flat.meshes) std::fill(mesh.triImg.begin(),mesh.triImg.end(),-1);
        const auto textureEffect=difference(visible,draw(flat));
        CheckGpu(textureEffect>30,"actual icons3 texels affect sale pixels via combined image remap");
        auto white=entities.Actors(); for(auto& mesh:white.meshes) for(auto& surface:mesh.surfaces) surface.color={1,1,1,1};
        const auto materialEffect=difference(visible,draw(white));
        const auto locked=NativeScriptPropertyActor(entities.PreparedModel(),q,entities.PropertyGeometry().Scale,512);
        const auto differentModel=difference(visible,draw(locked));
        CheckGpu(differentModel>100,"actual sale drawable differs from locked model at identical position/time");
        auto unscaled=NativeScriptPropertyActor(bind,q,1,512);
        for(auto& mesh:unscaled.meshes)for(auto& image:mesh.triImg)if(image>=0)image+=int(entities.PreparedModel().images.size());
        const auto scaleEffect=difference(visible,draw(unscaled));
        CheckGpu(scaleEffect>100,"source sale COL scale changes actual GPU coverage");
        CheckGpu(entities.PriceLabels().size()==1 && entities.PriceLabels()[0].Price==30000 && entities.PriceLabels()[0].Text=="$30000" &&
            entities.PriceLabels()[0].Alpha==163 && entities.PriceLabels()[0].Position.Z==q.Z+.7f,
            "same visible sale exports source cost/14-unit distance alpha and Z+0.7 price-label candidate");
        std::printf("sale GPU PASS model=%s txd=%s type=%d tris=%d coverage=%zu textureEffect=%zu materialEffect=%zu lockedDelta=%zu sourceCOL=%d scale=%.9f scaleEffect=%zu labelAlpha=%u price=%u labelProjectionParent=required\n",
            bind.stats.dffName,bind.stats.txdName,sale.Type,bind.stats.triangles,coverage,textureEffect,materialEffect,differentModel,
            geometry.ColHeaderId,geometry.Scale,scaleEffect,entities.PriceLabels()[0].Alpha,entities.PriceLabels()[0].Price);
        entities.Tick(p,p,true,false); CheckGpu(entities.AdvanceTime(512,error),error.c_str());
    }
    {
        char error[256]{}; CheckGpu(s_ProbeHud && s_ProbeHud->Upload(error, sizeof(error)), error);
        RealtimeHudView view; view.clock = false;
        RealtimeHudState state;
        const auto& blip = entities.Blips()[0];
        state.playerX = blip.Position.X - 70; state.playerY = blip.Position.Y;
        const auto draw = [&] { glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT); s_ProbeHud->Draw(view, state, width, height); return readback(); };
        const auto baseline = draw();
        state.scriptBlips = entities.Blips();
        const auto visible = draw();
        const auto spritePixels = difference(baseline, visible);
        CheckGpu(spritePixels > 50 && spritePixels <= 16 * 16, "real propertyR atlas stamp, not fabricated radar primitives");
        state.playerOnMission = true; CheckGpu(draw() == baseline, "contact filter uses source on-mission condition"); state.playerOnMission = false;
        state.radarZoom = 1; CheckGpu(draw() == baseline, "source short range zoom filter"); state.radarZoom = 0;
        state.exterior = false; CheckGpu(draw() == baseline, "source exterior filter"); state.exterior = true;
        CheckGpu(entities.SetBlipDisplay({{900, 1, 1}, blip.Reference, 0}).Status == NativeScriptServiceStatus::Ready, "actual display service");
        CheckGpu(draw() == baseline, "018B hides actual HUD drawable");
        CheckGpu(entities.SetBlipDisplay({{900, 2, 2}, blip.Reference, 2}).Status == NativeScriptServiceStatus::Ready, "actual display restore");
        CheckGpu(draw() == visible, "018B restores actual HUD drawable");
        state.playerX = blip.Position.X - 181;
        state.scriptBlips = {}; const auto farBaseline = draw();
        state.scriptBlips = entities.Blips().first(1); CheckGpu(draw() == farBaseline, "short range sprite outside real distance 1 suppressed");
        std::string helpError;
        CheckGpu(entities.AdvanceTime(1000, helpError) && entities.AdvanceTime(1016, helpError) && entities.AdvanceTime(1032, helpError), "real entity presentation clock");
        const auto applyHelp = [&] {
            const auto help = entities.HelpPresentation(); state.helpText = help.Text; state.helpAlpha = help.Alpha;
        };
        applyHelp(); CheckGpu(state.helpText == pickup.Message && state.helpAlpha == 200, "source proximity latch feeds owned timed presentation");
        state.scriptBlips = {}; state.helpAlpha = 0;
        const auto noHelp = draw();
        applyHelp();
        const auto fullHelp = draw();
        const auto helpPixels = difference(noHelp, fullHelp);
        std::size_t helpInk = 0;
        for (std::size_t i = 0; i < fullHelp.size(); i += 4) helpInk += fullHelp[i] > noHelp[i] && fullHelp[i+1] > noHelp[i+1];
        CheckGpu(helpInk > 100, "owned GXT help uses actual font1 glyph ink, not merely a dark box");
        const auto expire = 1033 + entities.HelpLifetimeMs();
        CheckGpu(entities.AdvanceTime(expire - 1, helpError) && entities.AdvanceTime(expire, helpError) && entities.AdvanceTime(expire + 100, helpError), "real entity fade elapsed");
        applyHelp(); CheckGpu(state.helpAlpha == 80, "source entity alpha at 100ms into fade");
        const auto fadedHelp = draw();
        CheckGpu(difference(fullHelp, fadedHelp) > 100 && difference(noHelp, fadedHelp) > 100, "presentation alpha changes actual font and box pixels");
        CheckGpu(entities.AdvanceTime(expire + 301, helpError), "real entity help expiry");
        applyHelp(); CheckGpu(state.helpText.empty() && !state.helpAlpha && draw() == noHelp, "expired zero alpha draws neither font nor box");
        entities.Tick(p, p, true, false);
        CheckGpu(entities.AdvanceTime(expire + 302, helpError), "source latch reentry frame publication");
        CheckGpu(entities.HelpRevision() == 1 && entities.HelpPresentation().Text.empty(), "expiry does not reset source once-only pickup latch");
        // Actual GXT/font consumer, generated proximity/input fixtures. Only the
        // denial uses source owned player cash; funded input is labelled TEST.
        const auto& sale=entities.Pickups()[3];
        NativeScriptPropertyInput saleInput; saleInput.Targeting=true;
        const auto saleFrame=[&](std::uint32_t now) {
            entities.Tick(sale.Position,sale.Position,true,false,saleInput);
            CheckGpu(entities.AdvanceTime(now,helpError),helpError.c_str()); applyHelp();
        };
        const auto saleTime=expire+320;
        saleFrame(saleTime); saleFrame(saleTime+16); saleFrame(saleTime+32);
        CheckGpu(state.helpText==sale.Message && state.helpAlpha==200 && sale.Message.find("TAB")!=std::string::npos,
            "source sale control-expanded GXT reaches actual timed HUD presentation");
        const auto salePrompt=draw(); const auto promptPixels=difference(noHelp,salePrompt);
        CheckGpu(promptPixels>100,"actual sale prompt produces real font1 glyph pixels");
        saleInput.FrameCounter=6; saleInput.Targeting=false; saleInput.CollectJustDown=true; saleInput.Money=host.PlayerInfo().Money;
        saleFrame(saleTime+48); saleFrame(saleTime+64); saleFrame(saleTime+80);
        CheckGpu(entities.Interaction().Status==NativeScriptPropertyInteractionStatus::InsufficientFunds && state.helpAlpha==200 && state.helpText!=sale.Message,
            "source real zero balance yields actual quick denial GXT HUD pixels");
        const auto denialPixels=difference(salePrompt,draw()); CheckGpu(denialPixels>100,"denial is distinct actual localized glyph content");
        saleInput.FrameCounter=12; saleInput.Money=sale.Price; saleFrame(saleTime+96); // TEST sufficient balance only
        CheckGpu(entities.Interaction().Status==NativeScriptPropertyInteractionStatus::ScriptPurchaseRequired && state.helpText.empty() &&
            !state.helpAlpha && draw()==noHelp && host.PlayerInfo().Money==0 && entities.ResolvePickup(sale.Reference),
            "funded TEST input clears actual help pixels; source type18 leaves pickup alive and host cash unchanged");
        entities.Tick(p,p,true,false); CheckGpu(entities.AdvanceTime(saleTime+112,helpError),helpError.c_str());
        std::printf("sale HUD GPU PASS promptPixels=%zu denialDelta=%zu actualBalance=%d fundedInput=TEST sourcePurchase=required collected=0\n",
            promptPixels,denialPixels,host.PlayerInfo().Money);
        // Hostile caller state around the real help path, including texture unit
        // and both matrix stacks; no caller-owned framebuffer/depth writes.
        glActiveTexture(GL_TEXTURE1); glEnable(GL_TEXTURE_2D);
        glMatrixMode(GL_PROJECTION); glLoadIdentity(); glTranslatef(3, 4, 5);
        glMatrixMode(GL_MODELVIEW); glLoadIdentity(); glScalef(2, 3, 4);
        glViewport(7, 9, 113, 127); glEnable(GL_DEPTH_TEST); glDepthMask(GL_FALSE);
        glDisable(GL_BLEND); glEnable(GL_SCISSOR_TEST); glScissor(2, 3, 4, 5);
        const auto snapshot = [] {
            std::array<GLint, 12> ints{};
            glGetIntegerv(GL_ACTIVE_TEXTURE, &ints[0]); glGetIntegerv(GL_MATRIX_MODE, &ints[1]);
            glGetIntegerv(GL_VIEWPORT, &ints[2]); glGetIntegerv(GL_SCISSOR_BOX, &ints[6]);
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &ints[10]); glGetIntegerv(GL_CURRENT_PROGRAM, &ints[11]);
            std::array<GLfloat, 32> matrices{};
            glGetFloatv(GL_PROJECTION_MATRIX, matrices.data()); glGetFloatv(GL_MODELVIEW_MATRIX, matrices.data() + 16);
            GLboolean depthWrite{}; glGetBooleanv(GL_DEPTH_WRITEMASK, &depthWrite);
            return std::tuple(ints, matrices, depthWrite, glIsEnabled(GL_BLEND), glIsEnabled(GL_DEPTH_TEST), glIsEnabled(GL_SCISSOR_TEST), glIsEnabled(GL_TEXTURE_2D));
        };
        const auto caller = snapshot(); state.helpText = pickup.Message; state.helpAlpha = 80;
        s_ProbeHud->Draw(view, state, width, height);
        CheckGpu(snapshot() == caller && glGetError() == GL_NO_ERROR, "timed help restores hostile caller GL state");
        std::printf("script-entities GPU PASS sprite=%d atlas=%s changed=%zu helpPixels=%zu helpInk=%zu fade=200,80,0 callerGL=preserved filtered=mission,zoom,exterior,display,range\n",
            blip.Sprite, entities.RadarImage().name, spritePixels, helpPixels, helpInk);
        s_ProbeHud.reset();
    }
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(display, context); eglDestroySurface(display, surface); eglTerminate(display);
}
