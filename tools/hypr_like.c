// Replicates the process state aquamarine/Hyprland is in when the Vulkan copier gets created:
// DRM master on both cards, EGL device-platform displays (with EGL_KHR_display_reference) and
// GLES contexts on both GPUs, GBM devices on both, then dlopen(libvulkan) + instance + device on
// NVIDIA, logging every step with a watchdog so a hang prints where it stopped.
#define _GNU_SOURCE
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <gbm.h>
#include <xf86drm.h>
#include <drm_fourcc.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <time.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

static const char* g_step = "start";
static void watchdog(int) { fprintf(stderr, "\n*** WATCHDOG: hung at step '%s'\n", g_step); _exit(3); }
static void step(const char* s) { g_step = s; struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); fprintf(stderr, "[%6.3f] %s\n", t.tv_sec % 1000 + t.tv_nsec / 1e9, s); }

static PFNEGLQUERYDEVICESEXTPROC pQueryDevices; static PFNEGLQUERYDEVICESTRINGEXTPROC pQueryDeviceString; static PFNEGLGETPLATFORMDISPLAYEXTPROC pGetPlatformDisplay;

static EGLDisplay egl_on(const char* cardPath, EGLContext* ctxOut, int trackRefs) {
    EGLDeviceEXT devs[16]; EGLint n = 0; pQueryDevices(16, devs, &n);
    EGLDeviceEXT dev = EGL_NO_DEVICE_EXT;
    for (int i = 0; i < n; i++) { const char* f = pQueryDeviceString(devs[i], EGL_DRM_DEVICE_FILE_EXT); if (f && !strcmp(f, cardPath)) { dev = devs[i]; break; } }
    if (dev == EGL_NO_DEVICE_EXT) { fprintf(stderr, "no EGL device for %s\n", cardPath); return EGL_NO_DISPLAY; }
    EGLint attrs[] = {EGL_TRACK_REFERENCES_KHR, EGL_TRUE, EGL_NONE};
    EGLDisplay d = pGetPlatformDisplay(EGL_PLATFORM_DEVICE_EXT, dev, trackRefs ? attrs : NULL);
    EGLint maj, min;
    if (d == EGL_NO_DISPLAY || !eglInitialize(d, &maj, &min)) {
        fprintf(stderr, "    eglInitialize with EGL_TRACK_REFERENCES failed (0x%x), retrying without\n", eglGetError());
        d = pGetPlatformDisplay(EGL_PLATFORM_DEVICE_EXT, dev, NULL);
        if (d == EGL_NO_DISPLAY || !eglInitialize(d, &maj, &min)) { fprintf(stderr, "eglInitialize failed for %s (0x%x)\n", cardPath, eglGetError()); return EGL_NO_DISPLAY; }
    }
    eglBindAPI(EGL_OPENGL_ES_API);
    EGLint cattr[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 2, EGL_NONE};
    EGLContext c = eglCreateContext(d, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, cattr);
    if (c == EGL_NO_CONTEXT) { EGLint c2[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE}; c = eglCreateContext(d, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, c2); }
    eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, c);
    fprintf(stderr, "    %s: %s / %s\n", cardPath, eglQueryString(d, EGL_VENDOR), glGetString(GL_RENDERER));
    *ctxOut = c;
    return d;
}

int main(int argc, char** argv) {
    signal(SIGALRM, watchdog); alarm(25);
    int wantMaster = argc > 1 && strstr(argv[1], "master");
    int keepCurrent = argc > 1 && strstr(argv[1], "current");

    pQueryDevices = (void*)eglGetProcAddress("eglQueryDevicesEXT"); pQueryDeviceString = (void*)eglGetProcAddress("eglQueryDeviceStringEXT"); pGetPlatformDisplay = (void*)eglGetProcAddress("eglGetPlatformDisplayEXT");

    step("open card1 (nvidia) + card2 (intel)");
    int nv = open("/dev/dri/card1", O_RDWR | O_CLOEXEC), in = open("/dev/dri/card2", O_RDWR | O_CLOEXEC);
    if (nv < 0 || in < 0) { perror("open"); return 1; }
    drmSetClientCap(nv, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1); drmSetClientCap(nv, DRM_CLIENT_CAP_ATOMIC, 1);
    drmSetClientCap(in, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1); drmSetClientCap(in, DRM_CLIENT_CAP_ATOMIC, 1);
    if (wantMaster) {
        step("drmSetMaster on both");
        fprintf(stderr, "    nvidia master: %s, intel master: %s\n", drmSetMaster(nv) == 0 ? "OK" : strerror(errno), drmSetMaster(in) == 0 ? "OK" : strerror(errno));
        fprintf(stderr, "    isMaster nvidia=%d intel=%d\n", drmIsMaster(nv), drmIsMaster(in));
    }

    step("gbm devices on both card fds (like aquamarine's allocators)");
    struct gbm_device* nvg = gbm_create_device(nv); struct gbm_device* ing = gbm_create_device(in);
    if (!nvg || !ing) { fprintf(stderr, "gbm fail\n"); return 1; }

    step("EGL device-platform display + context on nvidia");
    EGLContext nctx; EGLDisplay nd = egl_on("/dev/dri/card1", &nctx, 1);
    step("EGL device-platform display + context on intel");
    EGLContext ictx; EGLDisplay id = egl_on("/dev/dri/card2", &ictx, 1);
    if (nd == EGL_NO_DISPLAY || id == EGL_NO_DISPLAY) return 1;

    step("allocate an nvidia gbm bo (RENDERING) and an intel linear scanout bo");
    struct gbm_bo* nbo = gbm_bo_create(nvg, 2560, 1600, DRM_FORMAT_XRGB8888, GBM_BO_USE_RENDERING);
    uint64_t lin = DRM_FORMAT_MOD_LINEAR;
    struct gbm_bo* ibo = gbm_bo_create_with_modifiers2(ing, 2560, 1600, DRM_FORMAT_XRGB8888, &lin, 1, GBM_BO_USE_RENDERING | GBM_BO_USE_SCANOUT);
    fprintf(stderr, "    nbo=%p ibo=%p\n", (void*)nbo, (void*)ibo);

    if (keepCurrent) { step("keeping nvidia EGL context current on this thread"); eglMakeCurrent(nd, EGL_NO_SURFACE, EGL_NO_SURFACE, nctx); }
    else { step("releasing EGL context on this thread"); eglMakeCurrent(nd, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT); }

    step("dlopen libvulkan.so.1");
    void* h = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!h) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(h, "vkGetInstanceProcAddr");
    PFN_vkCreateInstance vkCreateInstance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");

    step("vkCreateInstance");
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "aquamarine", .apiVersion = VK_API_VERSION_1_1};
    VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    VkInstance inst; VkResult r = vkCreateInstance(&ici, NULL, &inst);
    fprintf(stderr, "    -> %d\n", r); if (r) return 1;
#define GI(n) PFN_##n n = (PFN_##n)gipa(inst, #n)
    GI(vkEnumeratePhysicalDevices); GI(vkGetPhysicalDeviceProperties2); GI(vkEnumerateDeviceExtensionProperties); GI(vkCreateDevice); GI(vkGetPhysicalDeviceQueueFamilyProperties); GI(vkGetDeviceProcAddr);

    step("vkEnumeratePhysicalDevices");
    uint32_t n = 0; vkEnumeratePhysicalDevices(inst, &n, NULL); VkPhysicalDevice pds[8]; vkEnumeratePhysicalDevices(inst, &n, pds);
    VkPhysicalDevice nvpd = NULL;
    for (uint32_t i = 0; i < n; i++) {
        step("vkGetPhysicalDeviceProperties2 (drm props)");
        VkPhysicalDeviceDrmPropertiesEXT drm = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT};
        VkPhysicalDeviceProperties2 p = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &drm};
        vkGetPhysicalDeviceProperties2(pds[i], &p);
        fprintf(stderr, "    %s render %ld:%ld\n", p.properties.deviceName, (long)drm.renderMajor, (long)drm.renderMinor);
        if (p.properties.vendorID == 0x10de) nvpd = pds[i];
    }
    step("vkEnumerateDeviceExtensionProperties (nvidia)");
    uint32_t ne = 0; vkEnumerateDeviceExtensionProperties(nvpd, NULL, &ne, NULL);
    step("vkGetPhysicalDeviceQueueFamilyProperties");
    uint32_t nq = 0; vkGetPhysicalDeviceQueueFamilyProperties(nvpd, &nq, NULL); VkQueueFamilyProperties qp[16]; vkGetPhysicalDeviceQueueFamilyProperties(nvpd, &nq, qp);
    uint32_t fam = 0; for (uint32_t i = 0; i < nq; i++) if ((qp[i].queueFlags & VK_QUEUE_TRANSFER_BIT) && !(qp[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT))) { fam = i; break; }
    step("vkCreateDevice (transfer queue family)");
    const char* exts[] = {"VK_KHR_external_memory_fd", "VK_EXT_external_memory_dma_buf", "VK_EXT_image_drm_format_modifier", "VK_KHR_external_semaphore_fd", "VK_EXT_queue_family_foreign"};
    float prio = 1; VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = fam, .queueCount = 1, .pQueuePriorities = &prio};
    VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci, .enabledExtensionCount = 5, .ppEnabledExtensionNames = exts};
    VkDevice dev; r = vkCreateDevice(nvpd, &dci, NULL, &dev);
    fprintf(stderr, "    -> %d (family %u)\n", r, fam);
    step("vkGetDeviceQueue + command pool");
    PFN_vkGetDeviceQueue vkGetDeviceQueue = (PFN_vkGetDeviceQueue)vkGetDeviceProcAddr(dev, "vkGetDeviceQueue"); VkQueue q; vkGetDeviceQueue(dev, fam, 0, &q);
    PFN_vkCreateCommandPool vkCreateCommandPool = (PFN_vkCreateCommandPool)vkGetDeviceProcAddr(dev, "vkCreateCommandPool");
    VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = fam}; VkCommandPool pool; r = vkCreateCommandPool(dev, &pci, NULL, &pool);
    fprintf(stderr, "    -> %d\n", r);
    step("done: no hang in this configuration");
    return 0;
}
