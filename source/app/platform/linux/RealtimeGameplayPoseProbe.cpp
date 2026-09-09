// Isolated EGL/GL actor screenshot: actual DFF triangles/TXD, no CPU bitmap upload.
#include "app/platform/linux/IfpAnim.h"
#include "app/platform/linux/RealtimeGameplayPoseAudit.h"
#include "app/platform/linux/RealtimeGameplay.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

static void Check(bool ok,const char* text) {
    if (!ok) { std::fprintf(stderr,"pose-probe FAIL %s\n",text); std::exit(1); }
}
int main(int argc,char** argv) {
    const char* game=argc>1 ? argv[1]:"/game";
    const char* output=argc>2 ? argv[2]:"/workspace/artifacts/graphics/RealtimeGameplayPose.tga";
    const char* anim=argc>3 ? argv[3]:"WALK_civi";
    WorldShotScene scene{}; IfpAnimStats stats{}; char error[512]={};
    RealtimeGameplayPoseAudit total{};
    for (const char* clip:{"IDLE_stance","WALK_civi","run_player","JUMP_glide"}) {
        for (int frame=0;frame<=32;++frame) {
            RealtimeGameplayPoseAudit audit{};
            Check(RealtimeGameplay_AuditPose(game,clip,frame/32.0,scene,stats,audit,error,sizeof(error)),error);
            if (frame==0) std::printf("pose-source clip=%s bones=%d mapped=%d vertices=%d parentMismatch=%d quatError=%.9f jointError=%.9f vertexError=%.9f normalError=%.9f\n",clip,
                audit.Bones,stats.mapped,audit.Vertices,audit.ParentMismatches,audit.MaxQuaternionError,audit.MaxJointError,audit.MaxVertexError,audit.MaxNormalError);
            // ped.ifp JUMP_glide contains 26 sequences; absent tracks retain
            // their DFF bind locals. The other three clips contain all 32.
            const int expectedSequences=std::string(clip)=="JUMP_glide" ? 26:32;
            Check(audit.Bones==32 && stats.seqs==expectedSequences && stats.mapped==expectedSequences && audit.Vertices>1000,
                "real DFF hierarchy and every available IFP tag mapped");
            Check(audit.ParentMismatches==0 && audit.InvalidInfluences==0,"hierarchy parent stack and skin index domain");
            Check(audit.MaxQuaternionError<0.00001f,"IFP quaternion convention equals librw HAnim application");
            Check(audit.MaxJointError<0.0001f && audit.MaxVertexError<0.0001f,"bind joints and weighted vertices equal sequential librw skin/world transforms");
            Check(audit.MaxNormalError<0.0001f,"weighted normals equal sequential librw transforms");
            total.Vertices+=audit.Vertices;
            total.MaxJointError=std::max(total.MaxJointError,audit.MaxJointError);
            total.MaxVertexError=std::max(total.MaxVertexError,audit.MaxVertexError);
            total.MaxNormalError=std::max(total.MaxNormalError,audit.MaxNormalError);
            total.MaxQuaternionError=std::max(total.MaxQuaternionError,audit.MaxQuaternionError);
            total.MaxLegacyJointError=std::max(total.MaxLegacyJointError,audit.MaxLegacyJointError);
        }
    }
    Check(total.MaxLegacyJointError>0.5f,"negative control rejects original matrix-order defect");
    std::printf("pose-audit PASS frames=132 vertices=%d maxJoint=%.9f maxVertex=%.9f maxNormal=%.9f maxQuaternion=%.9f legacyJoint=%.6f\n",
        total.Vertices,total.MaxJointError,total.MaxVertexError,total.MaxNormalError,total.MaxQuaternionError,total.MaxLegacyJointError);
    IfpAnimBlendResult blend{};
    Check(IfpAnim_Blend(game,"andre","IDLE_stance","WALK_civi",5,blend,error,sizeof(error)),error);
    Check(IfpAnim_Init(game,"andre","IDLE_stance",0.5,scene,stats,error,sizeof(error),false),error);
    Check(scene.meshes.size()==blend.frames.front().scene.meshes.size(),"blend source endpoint topology");
    for (size_t i=0;i<scene.meshes.size();++i) {
        Check(scene.meshes[i].pos==blend.frames.front().scene.meshes[i].pos && scene.meshes[i].nrm==blend.frames.front().scene.meshes[i].nrm,
            "blend frame zero equals current direct IFP reference, not a stale screenshot hash");
    }
    std::printf("pose-blend PASS exactEndpoint=1 morphMono=%.6f\n",blend.morphMono);
    Check(IfpAnim_Init(game,"andre",anim,0.375,scene,stats,error,sizeof(error),true),error);
    float center[3]={(scene.bboxMin[0]+scene.bboxMax[0])*0.5f,(scene.bboxMin[1]+scene.bboxMax[1])*0.5f,(scene.bboxMin[2]+scene.bboxMax[2])*0.5f};
    if (argc>4 && std::string(argv[4])=="runtime") {
        WorldShotScene ground{}; ground.meshes.emplace_back();
        ground.meshes[0].pos={-100,-100,0,100,-100,0,100,100,0,-100,-100,0,100,100,0,-100,100,0}; ground.meshes[0].tris=2;
        RealtimeGameplayWorld world; RealtimeGameplay gameplay; std::string message;
        Check(world.Rebuild(ground,message),message.c_str());
        Check(gameplay.Initialize(game,message),message.c_str());
        Check(gameplay.Spawn(world,0,0,10,1.5707963f,message),message.c_str());
        for (int i=0;i<45;++i) gameplay.Tick(1.0/60.0,{.Forward=1},world);
        scene=gameplay.Actors();
        WorldShotScene direct{},end{}; IfpAnimStats beginStats{},endStats{},directStats{};
        Check(IfpAnim_Init(game,"andre","WALK_civi",0,direct,beginStats,error,sizeof(error),true),error);
        Check(IfpAnim_Init(game,"andre","WALK_civi",1,end,endStats,error,sizeof(error),true),error);
        const double stride=std::hypot(endStats.rootWorld[0]-beginStats.rootWorld[0],endStats.rootWorld[1]-beginStats.rootWorld[1]);
        const double cycles=gameplay.State().WalkDistance/stride;
        Check(IfpAnim_Init(game,"andre","WALK_civi",cycles-std::floor(cycles),direct,directStats,error,sizeof(error),true),error);
        float cacheError=0;
        for (size_t i=0;i<direct.meshes.size();++i) {
            const auto& a=direct.meshes[i].pos; const auto& b=scene.meshes[i].pos;
            Check(a.size()==b.size(),"cached/direct posed topology");
            for (size_t v=0;v<a.size();v+=3) {
                cacheError=std::max({cacheError,
                    std::abs(a[v]-directStats.rootWorld[0]+gameplay.State().Ped.X-b[v]),
                    std::abs(a[v+1]-directStats.rootWorld[1]+gameplay.State().Ped.Y-b[v+1]),
                    std::abs(a[v+2]-beginStats.animMin[2]+gameplay.State().Ped.Z-b[v+2])});
            }
        }
        Check(cacheError<0.025f,"cached interpolation remains close to direct skeletal pose");
        center[0]=gameplay.State().Ped.X; center[1]=gameplay.State().Ped.Y; center[2]=gameplay.State().Ped.Z+0.9f;
        std::printf("pose-runtime ticks=%llu clip=%s distance=%.6f meshes=%zu cacheVsSkeleton=%.9f\n",static_cast<unsigned long long>(gameplay.State().Ticks),gameplay.State().Animation,gameplay.State().WalkDistance,scene.meshes.size(),cacheError);
    }
    auto displayFn=reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
    Check(displayFn!=nullptr,"EGL entrypoint");
    EGLDisplay display=displayFn(EGL_PLATFORM_SURFACELESS_MESA,EGL_DEFAULT_DISPLAY,nullptr);
    Check(eglInitialize(display,nullptr,nullptr),"EGL initialize");
    const EGLint attrs[]={EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_BIT,EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_DEPTH_SIZE,24,EGL_NONE};
    EGLConfig config{}; EGLint count=0;
    Check(eglChooseConfig(display,attrs,&config,1,&count) && count>0,"EGL config");
    const int width=960,height=720;
    const EGLint surfaceAttrs[]={EGL_WIDTH,width,EGL_HEIGHT,height,EGL_NONE};
    EGLSurface surface=eglCreatePbufferSurface(display,config,surfaceAttrs);
    Check(eglBindAPI(EGL_OPENGL_API),"OpenGL API");
    EGLContext context=eglCreateContext(display,config,EGL_NO_CONTEXT,nullptr);
    Check(eglMakeCurrent(display,surface,surface,context),"EGL current");
    std::printf("pose-gl %s anim=%s bbox=(%.3f %.3f %.3f)-(%.3f %.3f %.3f)\n",glGetString(GL_RENDERER),anim,
        scene.bboxMin[0],scene.bboxMin[1],scene.bboxMin[2],scene.bboxMax[0],scene.bboxMax[1],scene.bboxMax[2]);
    glViewport(0,0,width,height); glClearColor(0.25f,0.3f,0.36f,1); glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST); glEnable(GL_NORMALIZE); glDisable(GL_CULL_FACE);
    glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(-1.7,1.7,-1.275,1.275,0.1,30);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity(); glTranslatef(0,0,-8); glRotatef(-80,1,0,0); glRotatef(20,0,0,1);
    glTranslatef(-center[0],-center[1],-center[2]);
    std::vector<GLuint> textures(scene.images.size()); glGenTextures(static_cast<GLsizei>(textures.size()),textures.data());
    for (size_t i=0;i<textures.size();++i) {
        const auto& image=scene.images[i]; glBindTexture(GL_TEXTURE_2D,textures[i]);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR); glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
        glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,image.w,image.h,0,GL_RGBA,GL_UNSIGNED_BYTE,image.rgba.data());
    }
    for (const auto& m:scene.meshes) {
        for (int t=0;t<m.tris;++t) {
            int image=m.triImg[t];
            if (image>=0) { glEnable(GL_TEXTURE_2D); glBindTexture(GL_TEXTURE_2D,textures[image]); } else glDisable(GL_TEXTURE_2D);
            glBegin(GL_TRIANGLES);
            for (int k=0;k<3;++k) {
                size_t v=static_cast<size_t>(t)*9+k*3;
                const float shade=0.65f+0.35f*std::max(0.0f,m.nrm[v]*0.3f-m.nrm[v+1]*0.5f+m.nrm[v+2]*0.8f);
                glColor3f(m.triCol[t*3]*shade,m.triCol[t*3+1]*shade,m.triCol[t*3+2]*shade);
                glNormal3fv(&m.nrm[v]); glTexCoord2fv(&m.uv[t*6+k*2]); glVertex3fv(&m.pos[v]);
            }
            glEnd();
        }
    }
    std::vector<unsigned char> pixels(static_cast<size_t>(width)*height*3);
    glReadPixels(0,0,width,height,GL_BGR,GL_UNSIGNED_BYTE,pixels.data());
    Check(glGetError()==GL_NO_ERROR,"GL render");
    FILE* file=std::fopen(output,"wb"); Check(file!=nullptr,"screenshot open");
    unsigned char header[18]={}; header[2]=2; header[12]=width&255; header[13]=width>>8; header[14]=height&255; header[15]=height>>8; header[16]=24;
    Check(std::fwrite(header,1,18,file)==18 && std::fwrite(pixels.data(),1,pixels.size(),file)==pixels.size(),"screenshot write"); std::fclose(file);
    std::printf("pose-screenshot %s tris=%d textures=%zu\n",output,scene.stats.triangles,scene.images.size());
    glDeleteTextures(static_cast<GLsizei>(textures.size()),textures.data());
    eglMakeCurrent(display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT); eglDestroyContext(display,context); eglDestroySurface(display,surface); eglTerminate(display);
    IfpAnim_Shutdown();
}
