// Test cross-GPU dma-buf import between NVIDIA and Intel render nodes.
// Allocates GBM buffers on one device and imports them (kernel PRIME + EGL) on the other.
#define _GNU_SOURCE
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <gbm.h>
#include <xf86drm.h>
#include <drm_fourcc.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <time.h>

typedef struct {
    const char* name;
    int fd;
    struct gbm_device* gbm;
    EGLDisplay dpy;
    EGLContext ctx;
} dev_t_;

static PFNEGLGETPLATFORMDISPLAYEXTPROC pGetPlatformDisplay;
static PFNEGLCREATEIMAGEKHRPROC pCreateImage;
static PFNEGLDESTROYIMAGEKHRPROC pDestroyImage;
static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC pImageTargetTex2D;
static PFNEGLQUERYDMABUFMODIFIERSEXTPROC pQueryMods;
static PFNEGLQUERYDMABUFFORMATSEXTPROC pQueryFmts;

static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000.0 + t.tv_nsec / 1e6; }

static int open_dev(dev_t_* d, const char* path) {
    d->fd = open(path, O_RDWR | O_CLOEXEC);
    if (d->fd < 0) { perror(path); return -1; }
    drmVersionPtr v = drmGetVersion(d->fd);
    d->name = strdup(v ? v->name : "?");
    drmFreeVersion(v);
    d->gbm = gbm_create_device(d->fd);
    if (!d->gbm) { fprintf(stderr, "%s: gbm_create_device failed\n", path); return -1; }
    d->dpy = pGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, d->gbm, NULL);
    if (d->dpy == EGL_NO_DISPLAY) { fprintf(stderr, "%s: no display\n", path); return -1; }
    EGLint maj, min;
    if (!eglInitialize(d->dpy, &maj, &min)) { fprintf(stderr, "%s: eglInitialize failed 0x%x\n", path, eglGetError()); return -1; }
    eglBindAPI(EGL_OPENGL_ES_API);
    EGLint cattr[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    d->ctx = eglCreateContext(d->dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, cattr);
    if (d->ctx == EGL_NO_CONTEXT) { fprintf(stderr, "%s: eglCreateContext failed 0x%x\n", path, eglGetError()); return -1; }
    printf("[%s] %s: EGL %d.%d vendor=%s\n", path, d->name, maj, min, eglQueryString(d->dpy, EGL_VENDOR));
    eglMakeCurrent(d->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, d->ctx);
    printf("    GL_RENDERER=%s\n", glGetString(GL_RENDERER));
    const char* exts = eglQueryString(d->dpy, EGL_EXTENSIONS);
    printf("    dma_buf_import=%d import_modifiers=%d\n", !!strstr(exts, "EGL_EXT_image_dma_buf_import"), !!strstr(exts, "EGL_EXT_image_dma_buf_import_modifiers"));
    // linear XRGB8888 import support?
    if (pQueryMods) {
        EGLuint64KHR mods[64]; EGLBoolean ext[64]; EGLint n = 0;
        pQueryMods(d->dpy, DRM_FORMAT_XRGB8888, 64, mods, ext, &n);
        int hasLinear = 0;
        for (int i = 0; i < n; i++) if (mods[i] == DRM_FORMAT_MOD_LINEAR) hasLinear = ext[i] ? 2 : 1;
        printf("    modifiers:"); for (int i = 0; i < n; i++) printf(" 0x%llx%s", (unsigned long long)mods[i], ext[i] ? "(ext)" : ""); printf("\n");
        printf("    XRGB8888 import modifiers: %d, LINEAR=%s\n", n, hasLinear == 0 ? "no" : hasLinear == 1 ? "yes" : "yes(external-only)");
    }
    eglMakeCurrent(d->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    return 0;
}

static void test_import(dev_t_* src, dev_t_* dst, uint32_t flags, int useMods, int w, int h) {
    printf("\n=== alloc on %s (flags=0x%x %s) -> import on %s, %dx%d ===\n", src->name, flags, useMods ? "with LINEAR modifier" : "gbm_bo_create", dst->name, w, h);
    struct gbm_bo* bo;
    uint64_t linear = DRM_FORMAT_MOD_LINEAR;
    if (useMods)
        bo = gbm_bo_create_with_modifiers2(src->gbm, w, h, DRM_FORMAT_XRGB8888, &linear, 1, flags);
    else
        bo = gbm_bo_create(src->gbm, w, h, DRM_FORMAT_XRGB8888, flags);
    if (!bo) { printf("  gbm_bo_create FAILED: %s\n", strerror(errno)); return; }
    uint64_t mod = gbm_bo_get_modifier(bo);
    uint32_t stride = gbm_bo_get_stride(bo);
    int fd = gbm_bo_get_fd(bo);
    printf("  bo: modifier=0x%llx stride=%u fd=%d\n", (unsigned long long)mod, stride, fd);
    if (fd < 0) { gbm_bo_destroy(bo); return; }

    // kernel-level PRIME import
    uint32_t handle = 0;
    int r = drmPrimeFDToHandle(dst->fd, fd, &handle);
    printf("  drmPrimeFDToHandle on %s: %s (handle %u)\n", dst->name, r == 0 ? "OK" : strerror(errno), handle);
    if (r == 0) {
        struct drm_gem_close cl = {.handle = handle};
        drmIoctl(dst->fd, DRM_IOCTL_GEM_CLOSE, &cl);
    }

    // EGL import on dst
    eglMakeCurrent(dst->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, dst->ctx);
    for (int withMod = 1; withMod >= 0; withMod--) {
        EGLint attrs[32]; int i = 0;
        attrs[i++] = EGL_WIDTH; attrs[i++] = w;
        attrs[i++] = EGL_HEIGHT; attrs[i++] = h;
        attrs[i++] = EGL_LINUX_DRM_FOURCC_EXT; attrs[i++] = DRM_FORMAT_XRGB8888;
        attrs[i++] = EGL_DMA_BUF_PLANE0_FD_EXT; attrs[i++] = fd;
        attrs[i++] = EGL_DMA_BUF_PLANE0_OFFSET_EXT; attrs[i++] = 0;
        attrs[i++] = EGL_DMA_BUF_PLANE0_PITCH_EXT; attrs[i++] = stride;
        if (withMod) {
            uint64_t m = mod == DRM_FORMAT_MOD_INVALID ? DRM_FORMAT_MOD_LINEAR : mod;
            attrs[i++] = EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT; attrs[i++] = (EGLint)(m & 0xffffffff);
            attrs[i++] = EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT; attrs[i++] = (EGLint)(m >> 32);
        }
        attrs[i++] = EGL_NONE;
        EGLImageKHR img = pCreateImage(dst->dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, attrs);
        if (img == EGL_NO_IMAGE_KHR) {
            printf("  eglCreateImageKHR(%s) on %s: FAILED 0x%x\n", withMod ? "explicit modifier" : "no modifier", dst->name, eglGetError());
            continue;
        }
        GLuint tex; glGenTextures(1, &tex); glBindTexture(GL_TEXTURE_2D, tex);
        pImageTargetTex2D(GL_TEXTURE_2D, img);
        GLenum e = glGetError();
        {
            // can we use it as a render target (renderbuffer)?
            PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC pRB = (void*)eglGetProcAddress("glEGLImageTargetRenderbufferStorageOES");
            GLuint rb, fb; glGenRenderbuffers(1, &rb); glBindRenderbuffer(GL_RENDERBUFFER, rb);
            pRB(GL_RENDERBUFFER, img);
            GLenum e2 = glGetError();
            glGenFramebuffers(1, &fb); glBindFramebuffer(GL_FRAMEBUFFER, fb);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
            GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
            printf("  as renderbuffer: glError=0x%x fbo status=0x%x (0x8cd5=complete)\n", e2, st);
            if (st == GL_FRAMEBUFFER_COMPLETE) {
                glClearColor(1, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT); glFinish();
                printf("  cleared red via %s\n", dst->name);
            }
            glBindFramebuffer(GL_FRAMEBUFFER, 0); glDeleteFramebuffers(1, &fb); glDeleteRenderbuffers(1, &rb);
            // external texture target?
            GLuint tex2; glGenTextures(1, &tex2); glBindTexture(GL_TEXTURE_EXTERNAL_OES, tex2);
            pImageTargetTex2D(GL_TEXTURE_EXTERNAL_OES, img);
            printf("  as GL_TEXTURE_EXTERNAL_OES: glError=0x%x\n", glGetError());
            glDeleteTextures(1, &tex2);
        }
        printf("  eglCreateImageKHR(%s) on %s: OK, tex target OK=%s\n", withMod ? "explicit modifier" : "no modifier", dst->name, e == GL_NO_ERROR ? "yes" : "no");
        if (e == GL_NO_ERROR) {
            // sample it into a small FBO to prove reading works and time it
            GLuint fbo, rbo; glGenFramebuffers(1, &fbo); glGenRenderbuffers(1, &rbo);
            glBindRenderbuffer(GL_RENDERBUFFER, rbo); glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8_OES, 64, 64);
            glBindFramebuffer(GL_FRAMEBUFFER, fbo); glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rbo);
            printf("  fbo status 0x%x\n", glCheckFramebufferStatus(GL_FRAMEBUFFER));
            glDeleteFramebuffers(1, &fbo); glDeleteRenderbuffers(1, &rbo);
        }
        glDeleteTextures(1, &tex);
        pDestroyImage(dst->dpy, img);
        break;
    }
    eglMakeCurrent(dst->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    {
        uint32_t st; void* mapData = NULL;
        void* p = gbm_bo_map(bo, 0, 0, w, h, GBM_BO_TRANSFER_READ, &st, &mapData);
        if (p) { printf("  CPU map on %s: pixel[0]=0x%08x pixel[mid]=0x%08x\n", src->name, ((uint32_t*)p)[0], ((uint32_t*)((char*)p + st * (h/2)))[w/2]); gbm_bo_unmap(bo, mapData); }
        else printf("  CPU map on %s failed: %s\n", src->name, strerror(errno));
    }
    close(fd);
    gbm_bo_destroy(bo);
}

int main(int argc, char** argv) {
    pGetPlatformDisplay = (void*)eglGetProcAddress("eglGetPlatformDisplayEXT");
    pCreateImage = (void*)eglGetProcAddress("eglCreateImageKHR");
    pDestroyImage = (void*)eglGetProcAddress("eglDestroyImageKHR");
    pImageTargetTex2D = (void*)eglGetProcAddress("glEGLImageTargetTexture2DOES");
    pQueryMods = (void*)eglGetProcAddress("eglQueryDmaBufModifiersEXT");
    pQueryFmts = (void*)eglGetProcAddress("eglQueryDmaBufFormatsEXT");

    dev_t_ nv = {0}, intel = {0};
    if (open_dev(&nv, argc > 1 ? argv[1] : "/dev/dri/renderD128") < 0) return 1;
    if (open_dev(&intel, argc > 2 ? argv[2] : "/dev/dri/renderD129") < 0) return 1;

    int w = 2560, h = 1600;
    // What aquamarine does: multigpu -> LINEAR modifier, GBM_BO_USE_RENDERING only
    test_import(&nv, &intel, GBM_BO_USE_RENDERING, 1, w, h);
    test_import(&nv, &intel, GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR, 0, w, h);
    test_import(&nv, &intel, GBM_BO_USE_RENDERING | GBM_BO_USE_SCANOUT, 1, w, h);
    test_import(&nv, &intel, GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR | GBM_BO_USE_SCANOUT, 0, w, h);
    // reverse: Intel-allocated (system memory) -> NVIDIA import
    test_import(&intel, &nv, GBM_BO_USE_RENDERING | GBM_BO_USE_SCANOUT, 1, w, h);
    test_import(&intel, &nv, GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR | GBM_BO_USE_SCANOUT, 0, w, h);
    test_import(&intel, &nv, GBM_BO_USE_RENDERING, 1, w, h);
    return 0;
}
