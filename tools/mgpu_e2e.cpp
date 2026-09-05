// End-to-end test of aquamarine's CVulkanCopier with real buffers:
//   NVIDIA GL renders into an NVIDIA GBM buffer (modifier chosen like the patched allocator does),
//   exports an EGL native fence, the copier waits on it and copies into an Intel linear buffer,
//   and we verify the pixels through a CPU mapping of the Intel buffer.
#define AQUAMARINE_HAS_VULKAN
#include <aquamarine/backend/Backend.hpp>
#include <aquamarine/buffer/Buffer.hpp>
#include "backend/drm/VulkanCopy.hpp"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <gbm.h>
#include <xf86drm.h>
#include <drm_fourcc.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <vector>

using namespace Aquamarine;
using namespace Hyprutils::Memory;

static double now_ms() { timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000.0 + t.tv_nsec / 1e6; }
static double cpu_ms() { timespec t; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t); return t.tv_sec * 1000.0 + t.tv_nsec / 1e6; }

class CTestBuffer : public IBuffer {
  public:
    CTestBuffer(gbm_bo* bo_) : bo(bo_) {
        attrs.success     = true;
        attrs.size        = {(double)gbm_bo_get_width(bo), (double)gbm_bo_get_height(bo)};
        attrs.format      = gbm_bo_get_format(bo);
        attrs.modifier    = gbm_bo_get_modifier(bo);
        attrs.planes      = 1;
        attrs.strides[0]  = gbm_bo_get_stride(bo);
        attrs.offsets[0]  = gbm_bo_get_offset(bo, 0);
        attrs.fds[0]      = gbm_bo_get_fd(bo);
        size              = attrs.size;
    }
    ~CTestBuffer() override { attachments.clear(); close(attrs.fds[0]); gbm_bo_destroy(bo); }
    eBufferCapability caps() override { return BUFFER_CAPABILITY_NONE; }
    eBufferType       type() override { return BUFFER_TYPE_DMABUF; }
    void              update(const Hyprutils::Math::CRegion&) override {}
    bool              isSynchronous() override { return false; }
    bool              good() override { return true; }
    SDMABUFAttrs      dmabuf() override { return attrs; }
    gbm_bo*           bo;
    SDMABUFAttrs      attrs;
};

static GLuint compileShader(GLenum type, const char* src) {
    GLuint s = glCreateShader(type); glShaderSource(s, 1, &src, nullptr); glCompileShader(s);
    GLint ok; glGetShaderiv(s, GL_COMPILE_STATUS, &ok); if (!ok) { char log[512]; glGetShaderInfoLog(s, 512, nullptr, log); fprintf(stderr, "shader: %s\n", log); exit(1); }
    return s;
}

int main(int argc, char** argv) {
    const int W = 2560, H = 1600, ITERS = argc > 1 ? atoi(argv[1]) : 30, HEAVY = argc > 2 ? atoi(argv[2]) : 200;

    SBackendOptions opts;
    opts.logFunction = [](eBackendLogLevel lvl, std::string msg) { printf("  [aq %d] %s\n", (int)lvl, msg.c_str()); };
    SBackendImplementationOptions headless;
    headless.backendType        = AQ_BACKEND_HEADLESS;
    headless.backendRequestMode = AQ_BACKEND_REQUEST_IF_AVAILABLE;
    auto backend = CBackend::create({headless}, opts);
    if (!backend) { fprintf(stderr, "no backend\n"); return 1; }

    int nvFd = open("/dev/dri/card1", O_RDWR | O_CLOEXEC);
    int inFd = open("/dev/dri/card2", O_RDWR | O_CLOEXEC);
    if (nvFd < 0 || inFd < 0) { perror("open card"); return 1; }
    { drmVersionPtr v = drmGetVersion(nvFd); printf("card1 = %s\n", v->name); drmFreeVersion(v); v = drmGetVersion(inFd); printf("card2 = %s\n", v->name); drmFreeVersion(v); }

    printf("== creating copier on NVIDIA ==\n");
    auto copier = CVulkanCopier::attempt(backend, nvFd);
    if (!copier) { fprintf(stderr, "copier creation failed\n"); return 1; }
    printf("copier device: %s\n", copier->deviceName.c_str());

    auto srcMods = copier->importableModifiers(DRM_FORMAT_XRGB8888, true);
    auto dstMods = copier->importableModifiers(DRM_FORMAT_XRGB8888, false);
    printf("importable src modifiers: %zu, dst modifiers: %zu (linear ok: %d)\n", srcMods.size(), dstMods.size(),
           (int)(std::find(dstMods.begin(), dstMods.end(), DRM_FORMAT_MOD_LINEAR) != dstMods.end()));

    // ---- NVIDIA side: gbm + EGL (like Hyprland rendering into an aquamarine swapchain buffer)
    gbm_device* nvGbm = gbm_create_device(nvFd);
    gbm_bo*     nvBo  = gbm_bo_create_with_modifiers2(nvGbm, W, H, DRM_FORMAT_XRGB8888, srcMods.data(), srcMods.size(), GBM_BO_USE_RENDERING);
    if (!nvBo) { perror("nvidia gbm alloc with copier modifiers"); return 1; }
    printf("nvidia bo modifier 0x%llx stride %u\n", (unsigned long long)gbm_bo_get_modifier(nvBo), gbm_bo_get_stride(nvBo));
    auto src = makeShared<CTestBuffer>(nvBo);

    auto pGetPlatformDisplay = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    auto pCreateImage        = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    auto pRBStorage          = (PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC)eglGetProcAddress("glEGLImageTargetRenderbufferStorageOES");
    auto pCreateSync         = (PFNEGLCREATESYNCKHRPROC)eglGetProcAddress("eglCreateSyncKHR");
    auto pDestroySync        = (PFNEGLDESTROYSYNCKHRPROC)eglGetProcAddress("eglDestroySyncKHR");
    auto pDupFence           = (PFNEGLDUPNATIVEFENCEFDANDROIDPROC)eglGetProcAddress("eglDupNativeFenceFDANDROID");

    EGLDisplay dpy = pGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, nvGbm, nullptr);
    EGLint maj, min; if (!eglInitialize(dpy, &maj, &min)) { fprintf(stderr, "eglInitialize\n"); return 1; }
    eglBindAPI(EGL_OPENGL_ES_API);
    EGLint cattr[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    EGLContext ctx = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, cattr);
    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx);
    printf("GL_RENDERER: %s\n", glGetString(GL_RENDERER));

    const auto& A = src->attrs;
    EGLint iattrs[] = {EGL_WIDTH, W, EGL_HEIGHT, H, EGL_LINUX_DRM_FOURCC_EXT, (EGLint)A.format, EGL_DMA_BUF_PLANE0_FD_EXT, A.fds[0], EGL_DMA_BUF_PLANE0_OFFSET_EXT, (EGLint)A.offsets[0],
                      EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)A.strides[0], EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, (EGLint)(A.modifier & 0xffffffff),
                      EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, (EGLint)(A.modifier >> 32), EGL_NONE};
    EGLImageKHR img = pCreateImage(dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, iattrs);
    if (img == EGL_NO_IMAGE_KHR) { fprintf(stderr, "EGL import of nvidia bo failed 0x%x\n", eglGetError()); return 1; }
    GLuint rbo, fbo; glGenRenderbuffers(1, &rbo); glBindRenderbuffer(GL_RENDERBUFFER, rbo); pRBStorage(GL_RENDERBUFFER, img);
    glGenFramebuffers(1, &fbo); glBindFramebuffer(GL_FRAMEBUFFER, fbo); glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rbo);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) { fprintf(stderr, "fbo incomplete\n"); return 1; }
    glViewport(0, 0, W, H);

    // shader for heavy draws
    GLuint vs = compileShader(GL_VERTEX_SHADER, "attribute vec2 p; void main(){ gl_Position = vec4(p, 0.0, 1.0); }");
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, "precision mediump float; uniform vec4 c; void main(){ gl_FragColor = c; }");
    GLuint prog = glCreateProgram(); glAttachShader(prog, vs); glAttachShader(prog, fs); glBindAttribLocation(prog, 0, "p"); glLinkProgram(prog); glUseProgram(prog);
    GLint uc = glGetUniformLocation(prog, "c");
    const float tri[] = {-1, -1, 3, -1, -1, 3};
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, tri);
    glDisable(GL_BLEND);

    // ---- Intel side: linear scanout buffer (what the patched allocator produces for the blit target)
    gbm_device* inGbm = gbm_create_device(inFd);
    uint64_t    linear = DRM_FORMAT_MOD_LINEAR;
    gbm_bo*     inBo  = gbm_bo_create_with_modifiers2(inGbm, W, H, DRM_FORMAT_XRGB8888, &linear, 1, GBM_BO_USE_RENDERING | GBM_BO_USE_SCANOUT);
    if (!inBo) { perror("intel gbm alloc"); return 1; }
    auto dst = makeShared<CTestBuffer>(inBo);
    // and a tiled one the copier must refuse
    gbm_bo* inBoTiled = gbm_bo_create(inGbm, W, H, DRM_FORMAT_XRGB8888, GBM_BO_USE_RENDERING | GBM_BO_USE_SCANOUT);
    auto dstTiled = makeShared<CTestBuffer>(inBoTiled);
    printf("intel linear bo modifier 0x%llx, tiled bo modifier 0x%llx\n", (unsigned long long)dst->attrs.modifier, (unsigned long long)dstTiled->attrs.modifier);
    printf("canCopy(src, linear)=%d canCopy(src, tiled)=%d\n", (int)copier->canCopy(src, dst), (int)copier->canCopy(src, dstTiled));

    printf("\n== %d iterations, %d heavy fullscreen draws each ==\n", ITERS, HEAVY);
    int bad = 0; double copyCpuSum = 0, copyWallSum = 0, fenceWaitSum = 0; double minCpu = 1e9, maxCpu = 0;
    for (int i = 0; i < ITERS; i++) {
        // render: clear to a wrong colour, then draw the right colour many times (slow), so a copy
        // that doesn't wait for the fence sees the wrong colour or a torn frame.
        float r = ((i * 37) % 251) / 255.f, g = ((i * 91) % 241) / 255.f, b = ((i * 53) % 229) / 255.f;
        glClearColor(1.f - r, 1.f - g, 1.f - b, 1.f); glClear(GL_COLOR_BUFFER_BIT);
        glUniform4f(uc, r, g, b, 1.f);
        for (int k = 0; k < HEAVY; k++) glDrawArrays(GL_TRIANGLES, 0, 3);
        EGLSyncKHR sync = pCreateSync(dpy, EGL_SYNC_NATIVE_FENCE_ANDROID, nullptr);
        glFlush();
        int fenceFd = pDupFence(dpy, sync);
        if (fenceFd < 0) { fprintf(stderr, "no native fence fd\n"); return 1; }

        double c0 = cpu_ms(), t0 = now_ms();
        auto res = copier->copy(src, dst, fenceFd);
        double c1 = cpu_ms(), t1 = now_ms();
        if (!res.success) { fprintf(stderr, "copy failed at iter %d\n", i); return 1; }
        pollfd pfd = {.fd = res.syncFD, .events = POLLIN};
        int pr = poll(&pfd, 1, 2000);
        double t2 = now_ms();
        if (pr <= 0) { fprintf(stderr, "copy fence never signalled\n"); return 1; }
        copyCpuSum += c1 - c0; copyWallSum += t1 - t0; fenceWaitSum += t2 - t1; minCpu = std::min(minCpu, c1 - c0); maxCpu = std::max(maxCpu, c1 - c0);

        uint32_t st; void* md = nullptr;
        uint8_t* p = (uint8_t*)gbm_bo_map(inBo, 0, 0, W, H, GBM_BO_TRANSFER_READ, &st, &md);
        if (!p) { perror("map"); return 1; }
        auto px = [&](int x, int y) { return *(uint32_t*)(p + st * y + x * 4) & 0xffffff; };
        uint32_t expect = ((uint32_t)(r * 255 + 0.5f) << 16) | ((uint32_t)(g * 255 + 0.5f) << 8) | (uint32_t)(b * 255 + 0.5f);
        uint32_t s0 = px(0, 0), s1 = px(W / 2, H / 2), s2 = px(W - 1, H - 1), s3 = px(1234, 777);
        auto close_enough = [](uint32_t a, uint32_t b) { for (int sh = 0; sh < 24; sh += 8) if (abs((int)((a >> sh) & 0xff) - (int)((b >> sh) & 0xff)) > 1) return false; return true; };
        bool ok = close_enough(s0, expect) && close_enough(s1, expect) && close_enough(s2, expect) && close_enough(s3, expect);
        if (!ok) { bad++; printf("  iter %2d MISMATCH expect %06x got %06x %06x %06x %06x\n", i, expect, s0, s1, s2, s3); }
        gbm_bo_unmap(inBo, md);

        close(fenceFd); pDestroySync(dpy, sync);
    }
    printf("\nresult: %d/%d frames correct\n", ITERS - bad, ITERS);
    printf("copy() call: avg CPU %.3f ms (min %.3f max %.3f), avg wall %.3f ms; fence signalled %.2f ms after submit (includes render time)\n",
           copyCpuSum / ITERS, minCpu, maxCpu, copyWallSum / ITERS, fenceWaitSum / ITERS);

    // does the fallback GL path still get skipped when buffers die? destroy buffers before the copier
    dst.reset(); dstTiled.reset(); src.reset();
    copier.reset();
    printf("teardown ok\n");
    return bad ? 2 : 0;
}
