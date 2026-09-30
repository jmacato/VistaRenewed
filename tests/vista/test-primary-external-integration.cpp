/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <epoxy/gl.h>
#include <epoxy/egl.h>
extern "C" {
#include "test-primary-external-integration.h"
}
/* Existing allocation, synchronization, caller-state and pixel oracle bodies;
 * only their helper entry points are replaced with the production C bridge. */
#include "helper-oracle.inc"

static void cacheReplacement(Candidate& a) {
    GLuint textures[2]{}, fb=0;
    glGenTextures(2,textures);glGenFramebuffers(1,&fb);
    QemuEGLExternalCopyError e{};
    auto *copy=bridge_new(textures[0],80,60,&e);
    for(int i=0;i<2;i++) {
        int w=80+i*20,h=60+i*10;
        glBindTexture(GL_TEXTURE_2D,textures[i]);
        glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,w,h,0,GL_RGBA,GL_UNSIGNED_BYTE,nullptr);
        glBindFramebuffer(GL_FRAMEBUFFER,fb);
        glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,textures[i],0);
        if(glCheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE)throw Stop("cache replacement FBO");
        glDisable(GL_SCISSOR_TEST);glDisable(GL_BLEND);glDisable(GL_FRAMEBUFFER_SRGB);
        glClearColor(1,0,1,1);glClear(GL_COLOR_BUFFER_BIT);
        if(i)bridge_retarget(copy,textures[i],w,h);
        restoreSource(a);
        copyCheck(bridge_run(copy,EGL_NO_IMAGE_KHR,800,600,0,0,0,0,w,h,false,false,&e),"cache_replacement_copy",e);
        poisonSource(a);
        std::vector<Pixel> actual(size_t(w)*h);
        glReadPixels(0,0,w,h,GL_RGBA,GL_UNSIGNED_BYTE,actual.data());
        uint64_t errors=0;for(int y=0;y<h;y++)for(int x=0;x<w;x++)errors+=actual[size_t(y)*w+x]!=expected(a.o,x,y,false);
        event("cache_replacement_pixels",errors?"failed":"passed",number("pixels",actual.size())+","+number("errors",errors));
        if(errors||glGetError()!=GL_NO_ERROR)throw Stop("cache replacement oracle");
    }
    copyCheck(bridge_destroy(&copy,&e),"cache_replacement_destroy",e);
    glDeleteFramebuffers(1,&fb);glDeleteTextures(2,textures);
}

static bool renderableExport() {
    if(!epoxy_has_egl_extension(eglGetCurrentDisplay(),"EGL_MESA_image_dma_buf_export")) {
        event("normal_renderable_path","unsupported","\"reason\":\"EGL_MESA_image_dma_buf_export absent\"");return false;
    }
    struct Storage {
        GLuint source=0,target=0,fbo=0;
        EGLImageKHR image=EGL_NO_IMAGE_KHR;
        int fd=-1; QemuEGLExternalCopy *copy=nullptr;
        ~Storage(){if(copy){QemuEGLExternalCopyError e{};if(!bridge_destroy(&copy,&e))_exit(3);}if(fd>=0)close(fd);if(image!=EGL_NO_IMAGE_KHR)eglDestroyImageKHR(eglGetCurrentDisplay(),image);glDeleteFramebuffers(1,&fbo);glDeleteTextures(1,&source);glDeleteTextures(1,&target);}
    } s;
    glGenTextures(1,&s.source);glBindTexture(GL_TEXTURE_2D,s.source);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,80,60,0,GL_RGBA,GL_UNSIGNED_BYTE,nullptr);
    glGenFramebuffers(1,&s.fbo);glBindFramebuffer(GL_FRAMEBUFFER,s.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,s.source,0);
    glDisable(GL_SCISSOR_TEST);glDisable(GL_BLEND);glDisable(GL_FRAMEBUFFER_SRGB);
    glClearColor(17/255.f,41/255.f,93/255.f,1);glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_SCISSOR_TEST);glScissor(3,5,11,7);glClearColor(201/255.f,13/255.f,71/255.f,1);glClear(GL_COLOR_BUFFER_BIT);glDisable(GL_SCISSOR_TEST);glFinish();
    EGLint attrs[]={EGL_GL_TEXTURE_LEVEL_KHR,0,EGL_IMAGE_PRESERVED_KHR,EGL_TRUE,EGL_NONE};
    s.image=eglCreateImageKHR(eglGetCurrentDisplay(),eglGetCurrentContext(),EGL_GL_TEXTURE_2D_KHR,(EGLClientBuffer)(uintptr_t)s.source,attrs);
    EGLint fourcc=0,planes=0,stride=0,offset=0;EGLuint64KHR modifier=0;
    if(s.image==EGL_NO_IMAGE_KHR||!eglExportDMABUFImageQueryMESA(eglGetCurrentDisplay(),s.image,&fourcc,&planes,&modifier)||planes!=1||!eglExportDMABUFImageMESA(eglGetCurrentDisplay(),s.image,&s.fd,&stride,&offset)) {
        event("normal_renderable_path","unsupported","\"reason\":\"desktop texture export rejected\"");return false;
    }
    bridge_source(s.fd,80,60,fourcc,stride,offset,modifier);
    bool external=true;int query=bridge_external_only(&external);
    event("normal_export_metadata","observed",number("fourcc",uint32_t(fourcc))+","+number("modifier",modifier)+","+number("stride",stride)+","+number("offset",offset)+",\"external_only\":"+(external?"true":"false"));
    if(query||external){event("normal_renderable_path","unsupported","\"reason\":\"export not advertised renderable\"");return false;}
    glGenTextures(1,&s.target);glBindTexture(GL_TEXTURE_2D,s.target);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,80,60,0,GL_RGBA,GL_UNSIGNED_BYTE,nullptr);
    glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,s.target,0);
    glClearColor(1,0,1,1);glClear(GL_COLOR_BUFFER_BIT);
    QemuEGLExternalCopyError e{};s.copy=bridge_new(s.target,80,60,&e);
    copyCheck(bridge_run(s.copy,EGL_NO_IMAGE_KHR,80,60,0,0,0,0,80,60,true,false,&e),"normal_renderable_transfer",e);
    glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,s.source,0);
    glClearColor(0,1,0,1);glClear(GL_COLOR_BUFFER_BIT);glFinish();
    glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,s.target,0);
    std::vector<Pixel> actual(80*60);glReadPixels(0,0,80,60,GL_RGBA,GL_UNSIGNED_BYTE,actual.data());
    uint64_t errors=0;for(int y=0;y<60;y++)for(int x=0;x<80;x++) {
        int sy=59-y;Pixel want=x>=3&&x<14&&sy>=5&&sy<12?Pixel{201,13,71,255}:Pixel{17,41,93,255};errors+=actual[y*80+x]!=want;
    }
    event("normal_renderable_path",errors?"failed":"passed",number("pixels",actual.size())+","+number("errors",errors)+",\"external_only\":false");
    if(errors||glGetError()!=GL_NO_ERROR)throw Stop("normal renderable pixels");
    return true;
}
int main() {
    try {
        Options o;o.width=800;o.height=600;o.tiling="drm-linear";
        Vulkan v(o);Candidate a(v,o);gpuPattern(a);a.exportFd();Desktop desktop(a);
        externalOwnership(a,true);
        bridge_source(a.fd,800,600,0x34325241,a.layout.rowPitch,a.layout.offset,0);
        bool external=false;int query=bridge_external_only(&external);
        event("real_modifier_query",!query&&external?"passed":"failed","\"external_only\":"+std::string(external?"true":"false")+",\"modifier\":0");
        if(query||!external)throw Stop("candidate not advertised external-only");
        destinationCase(a,EGL_NO_IMAGE_KHR,800,600,false);
        destinationCase(a,EGL_NO_IMAGE_KHR,800,600,true);
        destinationCase(a,EGL_NO_IMAGE_KHR,320,240,false);
        cacheReplacement(a);
        bool normal=renderableExport();
        event("result","production_transfer_bridge_pass","\"normal_renderable_path_passed\":"+std::string(normal?"true":"false")+",\"candidate_cpu_intermediate\":false,\"full_qemu_integration_proven\":false,\"application_performance_proven\":false");
        return 0;
    }catch(const std::exception& e){event("result","failed","\"reason\":"+quote(e.what()));return 2;}
}
