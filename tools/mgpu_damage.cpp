// Correctness test for the damage-limited path of CVulkanCopier.
//
// The dangerous bug in partial copies is stale content: a destination buffer that was not written
// for a few frames must still receive the damage of those frames when its turn comes. This test
// reproduces exactly that: one NVIDIA source buffer that always holds the full correct frame, and
// N Intel destination buffers used round-robin, like the real scanout swapchain. Each frame paints
// one small rectangle, passes only that rectangle as damage, and then verifies the *entire*
// destination buffer against a CPU-side reference image.
//
// Run: ./mgpu_damage [frames] [destinations] [--verbose]
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
#include <cerrno>
#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <random>
#include <vector>

using namespace Aquamarine;
using namespace Hyprutils::Memory;
using namespace Hyprutils::Math;
#define SP CSharedPointer

static const int W = 2560, H = 1600;
static bool      verbose = false;

class CTestBuffer : public IBuffer {
  public:
    CTestBuffer(gbm_bo* bo_) : bo(bo_) {
        attrs.success    = true;
        attrs.size       = {(double)gbm_bo_get_width(bo), (double)gbm_bo_get_height(bo)};
        attrs.format     = gbm_bo_get_format(bo);
        attrs.modifier   = gbm_bo_get_modifier(bo);
        attrs.planes     = 1;
        attrs.strides[0] = gbm_bo_get_stride(bo);
        attrs.offsets[0] = gbm_bo_get_offset(bo, 0);
        attrs.fds[0]     = gbm_bo_get_fd(bo);
        size             = attrs.size;
    }
    ~CTestBuffer() override {
        attachments.clear();
        close(attrs.fds[0]);
        gbm_bo_destroy(bo);
    }
    eBufferCapability caps() override { return BUFFER_CAPABILITY_NONE; }
    eBufferType       type() override { return BUFFER_TYPE_DMABUF; }
    void              update(const CRegion&) override {}
    bool              isSynchronous() override { return false; }
    bool              good() override { return true; }
    SDMABUFAttrs      dmabuf() override { return attrs; }
    gbm_bo*           bo;
    SDMABUFAttrs      attrs;
};

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IOLBF, 0); // keep output if we die mid-run
    const int FRAMES = argc > 1 ? atoi(argv[1]) : 90;
    const int NDST   = argc > 2 ? atoi(argv[2]) : 3;
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--verbose"))
            verbose = true;

    SBackendOptions opts;
    opts.logFunction = [](eBackendLogLevel lvl, std::string msg) {
        if (verbose)
            printf("  [aq %d] %s\n", (int)lvl, msg.c_str());
    };
    SBackendImplementationOptions headless;
    headless.backendType        = AQ_BACKEND_HEADLESS;
    headless.backendRequestMode = AQ_BACKEND_REQUEST_IF_AVAILABLE;
    auto backend                = CBackend::create({headless}, opts);

    int  nvFd = open("/dev/dri/card1", O_RDWR | O_CLOEXEC), inFd = open("/dev/dri/card2", O_RDWR | O_CLOEXEC);
    auto copier = CVulkanCopier::attempt(backend, nvFd);
    if (!copier) {
        fprintf(stderr, "copier creation failed\n");
        return 1;
    }

    auto        srcMods = copier->importableModifiers(DRM_FORMAT_XRGB8888, true);
    gbm_device* nvGbm   = gbm_create_device(nvFd);
    gbm_bo*     nvBo    = gbm_bo_create_with_modifiers2(nvGbm, W, H, DRM_FORMAT_XRGB8888, srcMods.data(), srcMods.size(), GBM_BO_USE_RENDERING);
    if (!nvBo) {
        perror("nvidia bo");
        return 1;
    }
    auto src = makeShared<CTestBuffer>(nvBo);

    // ---- NVIDIA GL onto the source buffer
    auto pGetPlatformDisplay = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    auto pCreateImage        = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    auto pRBStorage          = (PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC)eglGetProcAddress("glEGLImageTargetRenderbufferStorageOES");
    auto pCreateSync         = (PFNEGLCREATESYNCKHRPROC)eglGetProcAddress("eglCreateSyncKHR");
    auto pDestroySync        = (PFNEGLDESTROYSYNCKHRPROC)eglGetProcAddress("eglDestroySyncKHR");
    auto pDestroyImage       = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
    auto pDupFence           = (PFNEGLDUPNATIVEFENCEFDANDROIDPROC)eglGetProcAddress("eglDupNativeFenceFDANDROID");

    EGLDisplay dpy = pGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, nvGbm, nullptr);
    EGLint     maj, min;
    eglInitialize(dpy, &maj, &min);
    eglBindAPI(EGL_OPENGL_ES_API);
    EGLint     cattr[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    EGLContext ctx     = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, cattr);
    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx);

    const auto& A         = src->attrs;
    EGLint      iattrs[]  = {EGL_WIDTH,
                             W,
                             EGL_HEIGHT,
                             H,
                             EGL_LINUX_DRM_FOURCC_EXT,
                             (EGLint)A.format,
                             EGL_DMA_BUF_PLANE0_FD_EXT,
                             A.fds[0],
                             EGL_DMA_BUF_PLANE0_OFFSET_EXT,
                             (EGLint)A.offsets[0],
                             EGL_DMA_BUF_PLANE0_PITCH_EXT,
                             (EGLint)A.strides[0],
                             EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT,
                             (EGLint)(A.modifier & 0xffffffff),
                             EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT,
                             (EGLint)(A.modifier >> 32),
                             EGL_NONE};
    EGLImageKHR img       = pCreateImage(dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, iattrs);
    GLuint      rbo, fbo;
    glGenRenderbuffers(1, &rbo);
    glBindRenderbuffer(GL_RENDERBUFFER, rbo);
    pRBStorage(GL_RENDERBUFFER, img);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rbo);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "fbo incomplete\n");
        return 1;
    }
    glViewport(0, 0, W, H);

    // a plain solid-colour full-screen triangle; scissor limits it to the damaged rect.
    // MGPU_TEST_DRAW=1 uses it instead of a scissored glClear (NVIDIA may implement the latter as
    // a fast clear that another engine reading the buffer does not see).
    auto compile = [](GLenum t, const char* src) {
        GLuint sh = glCreateShader(t); glShaderSource(sh, 1, &src, nullptr); glCompileShader(sh);
        GLint ok = 0; glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
        if (!ok) { char log[512]; glGetShaderInfoLog(sh, 512, nullptr, log); fprintf(stderr, "shader: %s\n", log); exit(1); }
        return sh;
    };
    GLuint prog = glCreateProgram();
    glAttachShader(prog, compile(GL_VERTEX_SHADER, "attribute vec2 p; void main(){ gl_Position = vec4(p, 0.0, 1.0); }"));
    glAttachShader(prog, compile(GL_FRAGMENT_SHADER, "precision mediump float; uniform vec4 c; void main(){ gl_FragColor = c; }"));
    glBindAttribLocation(prog, 0, "p");
    glLinkProgram(prog);
    glUseProgram(prog);
    const GLint       U_COLOUR = glGetUniformLocation(prog, "c");
    static const float TRI[]   = {-1, -1, 3, -1, -1, 3};
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, TRI);
    glDisable(GL_BLEND);
    const bool USE_DRAW = getenv("MGPU_TEST_DRAW");

    // ---- Intel destinations, like the real scanout swapchain
    gbm_device*                inGbm  = gbm_create_device(inFd);
    uint64_t                   linear = DRM_FORMAT_MOD_LINEAR;
    std::vector<SP<CTestBuffer>> dst;
    for (int i = 0; i < NDST; i++) {
        gbm_bo* b = gbm_bo_create_with_modifiers2(inGbm, W, H, DRM_FORMAT_XRGB8888, &linear, 1, GBM_BO_USE_RENDERING | GBM_BO_USE_SCANOUT);
        if (!b) {
            perror("intel bo");
            return 1;
        }
        dst.emplace_back(makeShared<CTestBuffer>(b));
    }
    // ---- Intel GL, used to read the destinations back.
    // A CPU mapping is not a reliable view here: the NVIDIA copy engine writes this system-memory
    // buffer over PCIe and those writes become visible to the CPU lazily, so gbm_bo_map shows
    // scattered stale lines. Intel's GPU reads it the same way its display engine does.
    EGLDisplay iDpy = pGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, inGbm, nullptr);
    if (!eglInitialize(iDpy, &maj, &min)) { fprintf(stderr, "intel eglInitialize failed\n"); return 1; }
    eglBindAPI(EGL_OPENGL_ES_API);
    EGLContext iCtx = eglCreateContext(iDpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, cattr);
    if (iCtx == EGL_NO_CONTEXT) { fprintf(stderr, "intel eglCreateContext failed\n"); return 1; }
    std::vector<GLuint> dstFbo(NDST);
    eglMakeCurrent(iDpy, EGL_NO_SURFACE, EGL_NO_SURFACE, iCtx);
    for (int i = 0; i < NDST; i++) {
        const auto& DA = dst[i]->attrs;
        EGLint      at[] = {EGL_WIDTH, W, EGL_HEIGHT, H, EGL_LINUX_DRM_FOURCC_EXT, (EGLint)DA.format,
                            EGL_DMA_BUF_PLANE0_FD_EXT, DA.fds[0], EGL_DMA_BUF_PLANE0_OFFSET_EXT, (EGLint)DA.offsets[0],
                            EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)DA.strides[0],
                            EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, (EGLint)(DA.modifier & 0xffffffff),
                            EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, (EGLint)(DA.modifier >> 32), EGL_NONE};
        EGLImageKHR di = pCreateImage(iDpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, at);
        if (di == EGL_NO_IMAGE_KHR) { fprintf(stderr, "intel import of destination %d failed 0x%x\n", i, eglGetError()); return 1; }
        GLuint rb; glGenRenderbuffers(1, &rb); glBindRenderbuffer(GL_RENDERBUFFER, rb);
        pRBStorage(GL_RENDERBUFFER, di);
        glGenFramebuffers(1, &dstFbo[i]); glBindFramebuffer(GL_FRAMEBUFFER, dstFbo[i]);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) { fprintf(stderr, "intel fbo %d incomplete\n", i); return 1; }
        // Touch the buffer from the Intel side first, exactly like aquamarine's GBM allocator does
        // for scanout buffers (CGBMBuffer clears them through the DRM renderer). Without this the
        // pages are never faulted in by their owner and the first foreign writes can go missing.
        if (!getenv("MGPU_TEST_NO_PREPARE")) {
            glClearColor(1.f, 0.f, 1.f, 1.f);
            glClear(GL_COLOR_BUFFER_BIT);
        }
    }
    if (!getenv("MGPU_TEST_NO_PREPARE"))
        glFinish();
    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx);
    std::vector<uint8_t> readback((size_t)W * H * 4);

    printf("source modifier 0x%llx, %d linear destinations, %d frames\n", (unsigned long long)src->attrs.modifier, NDST, FRAMES);

    // CPU reference of what the full frame should look like
    // a distinctive baseline: if a CPU readback shows freshly-allocated (zeroed) memory instead
    // of what we copied, every pixel mismatches and it cannot be mistaken for a small copy bug
    constexpr uint32_t    BASE = 0x00203040;
    std::vector<uint32_t> expected((size_t)W * H, BASE);
    std::mt19937          rng(1234);

    // paint the whole thing once so the first frames have a defined baseline
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0x20 / 255.f, 0x30 / 255.f, 0x40 / 255.f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);

    // Warm-up: one full copy into every destination, exactly like the compositor's first frames
    // (aquamarine forces full copies into a freshly imported buffer). The very first copy into a
    // brand-new import has been seen to leave scattered pixels untouched; the second never does.
    glFinish(); // the baseline clear must be in the buffer before we copy it anywhere
    for (int i = 0; i < NDST * 2; i++) {
        auto w = copier->copy(src, dst[i % NDST], -1, nullptr);
        if (w.success && w.syncFD >= 0) {
            pollfd pf = {.fd = w.syncFD, .events = POLLIN};
            poll(&pf, 1, 2000);
        }
    }

    if (const char* ms = getenv("MGPU_TEST_SETTLE")) {
        printf("settling for %s ms before the first copy\n", ms);
        usleep(atoi(ms) * 1000);
    }

    int      bad = 0, skipped = 0;
    uint64_t totalCopied = 0;

    for (int f = 0; f < FRAMES; f++) {
        // a rectangle in *buffer* coordinates (row 0 = first row in memory), like Hyprland's damage
        int x = rng() % (W - 64), y = rng() % (H - 64);
        int w = 16 + rng() % 400, h = 16 + rng() % 300;
        w = std::min(w, W - x);
        h = std::min(h, H - y);
        // every 17th frame: no damage at all (destination should be left alone or already correct)
        const bool NO_DAMAGE = (f % 17) == 16;
        // every 11th frame: many small rects, to exercise the bounding-box fallback
        const bool MANY = (f % 11) == 10;

        CRegion damage;
        uint8_t r = rng() % 256, g = rng() % 256, b = rng() % 256;
        uint32_t colour = 0xff000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b; // XRGB in memory

        if (!NO_DAMAGE) {
            glEnable(GL_SCISSOR_TEST);
            glClearColor(r / 255.f, g / 255.f, b / 255.f, 1.f);
            auto paint = [&](int px, int py, int pw, int ph) {
                glScissor(px, py, pw, ph); // EGLImage row 0 == GL y 0 for these buffers
                if (USE_DRAW) {
                    glUniform4f(U_COLOUR, r / 255.f, g / 255.f, b / 255.f, 1.f);
                    glDrawArrays(GL_TRIANGLES, 0, 3);
                } else
                    glClear(GL_COLOR_BUFFER_BIT);
                damage.add(px, py, pw, ph);
                for (int yy = py; yy < py + ph; yy++)
                    for (int xx = px; xx < px + pw; xx++)
                        expected[(size_t)yy * W + xx] = colour;
            };
            if (MANY) {
                for (int k = 0; k < 40; k++) {
                    int sx = rng() % (W - 40), sy = rng() % (H - 40);
                    paint(sx, sy, 8 + rng() % 32, 8 + rng() % 32);
                }
            } else
                paint(x, y, w, h);
            glDisable(GL_SCISSOR_TEST);
        }

        // MGPU_TEST_GLFINISH=1 replaces the explicit fence with a CPU-side glFinish, to tell a
        // broken fence wait apart from a broken copy.
        static const bool GLFINISH = getenv("MGPU_TEST_GLFINISH");
        EGLSyncKHR        sync     = nullptr;
        int               fenceFd  = -1;
        if (GLFINISH)
            glFinish();
        else {
            sync = pCreateSync(dpy, EGL_SYNC_NATIVE_FENCE_ANDROID, nullptr);
            glFlush();
            fenceFd = pDupFence(dpy, sync);
        }

        // MGPU_TEST_PROBE: read the source back with GL and report where the paint actually is
        if (getenv("MGPU_TEST_PROBE") && !NO_DAMAGE && !MANY) {
            std::vector<uint8_t> px((size_t)W * H * 4);
            glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
            int mnx = W, mny = H, mxx = -1, mxy = -1;
            for (int yy = 0; yy < H; yy++)
                for (int xx = 0; xx < W; xx++) {
                    const uint8_t* q = &px[((size_t)yy * W + xx) * 4];
                    if (q[0] == r && q[1] == g && q[2] == b) {
                        mnx = std::min(mnx, xx); mxx = std::max(mxx, xx);
                        mny = std::min(mny, yy); mxy = std::max(mxy, yy);
                    }
                }
            printf("  frame %2d PROBE: asked to paint (%d,%d) %dx%d; glReadPixels finds the colour in GL rows (%d,%d)-(%d,%d)\n", f, x, y, w, h, mnx, mny, mxx, mxy);
        }

        auto& D   = dst[f % NDST];
        auto  res = copier->copy(src, D, fenceFd, &damage);
        if (!res.success) {
            fprintf(stderr, "frame %d: copy failed\n", f);
            return 1;
        }
        if (res.syncFD < 0)
            skipped++;
        else {
            pollfd pfd = {.fd = res.syncFD, .events = POLLIN};
            if (poll(&pfd, 1, 2000) <= 0) {
                fprintf(stderr, "frame %d: fence never signalled\n", f);
                return 1;
            }
        }

        // is the SOURCE itself what we think it is? (NVIDIA's own view of the buffer)
        size_t srcBad = 0;
        if (getenv("MGPU_TEST_SRC")) {
            std::vector<uint8_t> sp((size_t)W * H * 4);
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, sp.data());
            for (size_t i = 0; i < (size_t)W * H; i++) {
                const uint32_t GOT = ((uint32_t)sp[i * 4] << 16) | ((uint32_t)sp[i * 4 + 1] << 8) | sp[i * 4 + 2];
                if (GOT != (expected[i] & 0xffffff))
                    srcBad++;
            }
            if (srcBad)
                printf("  frame %2d: SOURCE differs from expectation in %zu pixels (GL's own view)\n", f, srcBad);
        }

        // verify the WHOLE destination with Intel's GPU, not just the damaged part
        eglMakeCurrent(iDpy, EGL_NO_SURFACE, EGL_NO_SURFACE, iCtx);
        glBindFramebuffer(GL_FRAMEBUFFER, dstFbo[f % NDST]);
        glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, readback.data());
        eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx);

        size_t   mismatches = 0, flippedMismatches = 0;
        int      firstX = -1, firstY = -1;
        int      mnX = W, mnY = H, mxX = -1, mxY = -1;
        uint32_t gotSample = 0, wantSample = 0;
        for (int yy = 0; yy < H; yy++) {
            for (int xx = 0; xx < W; xx++) {
                const uint8_t* q   = &readback[((size_t)yy * W + xx) * 4];
                const uint32_t GOT = ((uint32_t)q[0] << 16) | ((uint32_t)q[1] << 8) | q[2];
                if (GOT != (expected[(size_t)yy * W + xx] & 0xffffff)) {
                    if (!mismatches) { firstX = xx; firstY = yy; gotSample = GOT; wantSample = expected[(size_t)yy * W + xx]; }
                    mnX = std::min(mnX, xx); mxX = std::max(mxX, xx);
                    mnY = std::min(mnY, yy); mxY = std::max(mxY, yy);
                    mismatches++;
                }
                if (GOT != (expected[(size_t)(H - 1 - yy) * W + xx] & 0xffffff))
                    flippedMismatches++;
            }
        }
        // Re-read through a *fresh* Intel import after a pause. If the holes disappear, the
        // destination memory was fine and the reader was showing stale cached lines.
        if (mismatches) {
            usleep(150000);
            const auto& DA   = dst[f % NDST]->attrs;
            EGLint      at[] = {EGL_WIDTH, W, EGL_HEIGHT, H, EGL_LINUX_DRM_FOURCC_EXT, (EGLint)DA.format,
                                EGL_DMA_BUF_PLANE0_FD_EXT, DA.fds[0], EGL_DMA_BUF_PLANE0_OFFSET_EXT, (EGLint)DA.offsets[0],
                                EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)DA.strides[0],
                                EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, (EGLint)(DA.modifier & 0xffffffff),
                                EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, (EGLint)(DA.modifier >> 32), EGL_NONE};
            eglMakeCurrent(iDpy, EGL_NO_SURFACE, EGL_NO_SURFACE, iCtx);
            EGLImageKHR di2 = pCreateImage(iDpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, at);
            GLuint      rb2, fb2;
            glGenRenderbuffers(1, &rb2); glBindRenderbuffer(GL_RENDERBUFFER, rb2); pRBStorage(GL_RENDERBUFFER, di2);
            glGenFramebuffers(1, &fb2); glBindFramebuffer(GL_FRAMEBUFFER, fb2);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb2);
            glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, readback.data());
            glDeleteFramebuffers(1, &fb2); glDeleteRenderbuffers(1, &rb2); pDestroyImage(iDpy, di2);
            eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx);
            size_t after = 0;
            for (int yy = 0; yy < H; yy++)
                for (int xx = 0; xx < W; xx++) {
                    const uint8_t* q   = &readback[((size_t)yy * W + xx) * 4];
                    const uint32_t GOT = ((uint32_t)q[0] << 16) | ((uint32_t)q[1] << 8) | q[2];
                    if (GOT != (expected[(size_t)yy * W + xx] & 0xffffff))
                        after++;
                }
            printf("  frame %2d: %zu mismatches -> %zu after 150ms and a fresh import\n", f, mismatches, after);
            if (after && getenv("MGPU_TEST_RECOPY")) {
                // repeat the copy: if the holes fill in, the first copy read a stale source
                auto r2 = copier->copy(src, D, -1, nullptr);
                if (r2.success && r2.syncFD >= 0) {
                    pollfd pf2 = {.fd = r2.syncFD, .events = POLLIN};
                    poll(&pf2, 1, 2000);
                }
                eglMakeCurrent(iDpy, EGL_NO_SURFACE, EGL_NO_SURFACE, iCtx);
                glBindFramebuffer(GL_FRAMEBUFFER, dstFbo[f % NDST]);
                glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, readback.data());
                eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx);
                size_t after2 = 0;
                for (size_t i = 0; i < (size_t)W * H; i++) {
                    const uint32_t GOT = ((uint32_t)readback[i * 4] << 16) | ((uint32_t)readback[i * 4 + 1] << 8) | readback[i * 4 + 2];
                    if (GOT != (expected[i] & 0xffffff))
                        after2++;
                }
                printf("       after an immediate second FULL copy: %zu mismatches\n", after2);
                after = after2;
            }
            if (after && getenv("MGPU_TEST_PATTERN")) {
                // where are the holes? alignment tells us whether this is a tiling/layout problem
                int  ax16 = 0, ay8 = 0, n = 0;
                int  runMinX = W, runMaxX = -1;
                printf("       first holes:");
                for (int yy = 0; yy < H && n < 12; yy++)
                    for (int xx = 0; xx < W && n < 12; xx++) {
                        const uint8_t* q   = &readback[((size_t)yy * W + xx) * 4];
                        const uint32_t GOT = ((uint32_t)q[0] << 16) | ((uint32_t)q[1] << 8) | q[2];
                        if (GOT != (expected[(size_t)yy * W + xx] & 0xffffff)) {
                            printf(" (%d,%d)", xx, yy);
                            if (xx % 16 == 0) ax16++;
                            if (yy % 8 == 0) ay8++;
                            runMinX = std::min(runMinX, xx); runMaxX = std::max(runMaxX, xx);
                            n++;
                        }
                    }
                printf("\n       of the first %d: %d start on a 16px boundary, %d on an 8-row boundary; x range %d..%d\n", n, ax16, ay8, runMinX, runMaxX);
                // how many whole rows are affected, and how wide is each affected run?
                int rowsAffected = 0;
                for (int yy = 0; yy < H; yy++) {
                    bool anyBad = false;
                    for (int xx = 0; xx < W; xx++) {
                        const uint8_t* q   = &readback[((size_t)yy * W + xx) * 4];
                        const uint32_t GOT = ((uint32_t)q[0] << 16) | ((uint32_t)q[1] << 8) | q[2];
                        if (GOT != (expected[(size_t)yy * W + xx] & 0xffffff)) { anyBad = true; break; }
                    }
                    if (anyBad) rowsAffected++;
                }
                printf("       %d of %d rows contain at least one hole\n", rowsAffected, H);
            }
            mismatches = after;
        }

        if (mismatches) {
            printf("  frame %2d: %zu mismatches, bad box (%d,%d)-(%d,%d), got 0x%06x want 0x%06x (flipped compare: %zu)\n", f, mismatches, mnX, mnY, mxX, mxY, gotSample,
                   wantSample & 0xffffff, flippedMismatches);
            if (!damage.empty()) {
                auto ext = damage.getExtents();
                printf("       this frame damaged (%d,%d) %dx%d in %zu rect(s)\n", (int)ext.x, (int)ext.y, (int)ext.w, (int)ext.h, damage.getRects().size());
            }
        }

        if (mismatches) {
            bad++;
            printf("  frame %2d (dst %zu): %zu mismatching pixels, first at (%d,%d)%s%s\n", f, (size_t)(f % NDST), mismatches, firstX, firstY,
                   NO_DAMAGE ? " [no-damage frame]" : "", MANY ? " [many rects]" : "");
            if (bad > 5) {
                printf("too many bad frames, stopping\n");
                break;
            }
        } else if (verbose)
            printf("  frame %2d (dst %zu): ok%s%s\n", f, (size_t)(f % NDST), NO_DAMAGE ? " [no-damage]" : "", MANY ? " [many rects]" : "");

        if (fenceFd >= 0)
            close(fenceFd);
        if (sync)
            pDestroySync(dpy, sync);
        (void)totalCopied;
    }

    printf("\nresult: %d/%d frames correct (%d frames needed no copy at all)\n", FRAMES - bad, FRAMES, skipped);
    dst.clear();
    src.reset();
    copier.reset();
    return bad ? 2 : 0;
}
