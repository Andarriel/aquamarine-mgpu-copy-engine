// Feasibility test: can NVIDIA's Vulkan import an Intel (i915) linear dma-buf as a
// TRANSFER_DST image and copy into it GPU-side (no CPU copy)?
// Also: can it import an NVIDIA GBM (tiled) buffer as TRANSFER_SRC via its modifier?
#define _GNU_SOURCE
#include <vulkan/vulkan.h>
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
#include <sys/poll.h>

#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { fprintf(stderr, "%s:%d %s -> %d\n", __FILE__, __LINE__, #x, r_); exit(1); } } while (0)

static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000.0 + t.tv_nsec / 1e6; }
static double cpu_ms(void) { struct timespec t; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t); return t.tv_sec * 1000.0 + t.tv_nsec / 1e6; }

static VkInstance inst;
static VkPhysicalDevice phys;
static VkDevice dev;
static VkQueue queue, gqueue;
static uint32_t qfam, gfam;
static VkCommandPool pool, gpool;
static PFN_vkGetMemoryFdPropertiesKHR pGetMemoryFdProps;
static PFN_vkGetSemaphoreFdKHR pGetSemaphoreFd;
static PFN_vkImportSemaphoreFdKHR pImportSemaphoreFd;
static PFN_vkGetFenceFdKHR pGetFenceFd;

static int has_ext(VkExtensionProperties* e, uint32_t n, const char* name) {
    for (uint32_t i = 0; i < n; i++) if (!strcmp(e[i].extensionName, name)) return 1;
    return 0;
}

static uint32_t find_mem_type(uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if (bits & (1u << i)) return i;
    return UINT32_MAX;
}

static void print_mem_type(uint32_t idx) {
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    if (idx >= mp.memoryTypeCount) { printf("memtype invalid"); return; }
    VkMemoryPropertyFlags f = mp.memoryTypes[idx].propertyFlags;
    printf("memtype %u flags=0x%x [%s%s%s%s] heap %u (%.0f MB)", idx, f,
           f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT ? "DEVICE_LOCAL " : "", f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT ? "HOST_VISIBLE " : "",
           f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT ? "HOST_COHERENT " : "", f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT ? "HOST_CACHED " : "",
           mp.memoryTypes[idx].heapIndex, mp.memoryHeaps[mp.memoryTypes[idx].heapIndex].size / 1048576.0);
}

// Query whether the device supports importing a dma-buf with this modifier for the given usage.
static int query_modifier_support(uint64_t modifier, VkImageUsageFlags usage, const char* what) {
    VkPhysicalDeviceImageDrmFormatModifierInfoEXT modInfo = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT,
                                                             .drmFormatModifier = modifier, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VkPhysicalDeviceExternalImageFormatInfo extInfo = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO, .pNext = &modInfo,
                                                       .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    VkPhysicalDeviceImageFormatInfo2 info = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2, .pNext = &extInfo,
                                             .format = VK_FORMAT_B8G8R8A8_UNORM, .type = VK_IMAGE_TYPE_2D, .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
                                             .usage = usage, .flags = 0};
    VkExternalImageFormatProperties extProps = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
    VkImageFormatProperties2 props = {.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, .pNext = &extProps};
    VkResult r = vkGetPhysicalDeviceImageFormatProperties2(phys, &info, &props);
    VkExternalMemoryFeatureFlags f = extProps.externalMemoryProperties.externalMemoryFeatures;
    printf("  [%s] modifier 0x%llx usage 0x%x: %s, features=0x%x [%s%s%s] max %ux%u\n", what, (unsigned long long)modifier, usage,
           r == VK_SUCCESS ? "SUPPORTED" : "UNSUPPORTED", f, f & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT ? "IMPORTABLE " : "",
           f & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT ? "EXPORTABLE " : "", f & VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT ? "DEDICATED_ONLY " : "",
           props.imageFormatProperties.maxExtent.width, props.imageFormatProperties.maxExtent.height);
    return r == VK_SUCCESS && (f & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT);
}

typedef struct { VkImage img; VkDeviceMemory mem; } vkimg;

// Import a dma-buf as a VkImage with an explicit modifier + layout.
static int import_dmabuf(int fd, uint32_t w, uint32_t h, uint64_t modifier, uint32_t stride, uint32_t offset, VkImageUsageFlags usage, vkimg* out) {
    VkSubresourceLayout layout = {.offset = offset, .rowPitch = stride, .size = 0, .arrayPitch = 0, .depthPitch = 0};
    VkImageDrmFormatModifierExplicitCreateInfoEXT modCI = {.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
                                                           .drmFormatModifier = modifier, .drmFormatModifierPlaneCount = 1, .pPlaneLayouts = &layout};
    VkExternalMemoryImageCreateInfo extCI = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, .pNext = &modCI,
                                             .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .pNext = &extCI, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_B8G8R8A8_UNORM,
                            .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT, .usage = usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VkResult r = vkCreateImage(dev, &ci, NULL, &out->img);
    if (r != VK_SUCCESS) { printf("  vkCreateImage failed: %d\n", r); return -1; }

    VkMemoryFdPropertiesKHR fdProps = {.sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
    r = pGetMemoryFdProps(dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd, &fdProps);
    if (r != VK_SUCCESS) { printf("  vkGetMemoryFdPropertiesKHR failed: %d\n", r); return -1; }

    VkMemoryDedicatedRequirements dedReq = {.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
    VkMemoryRequirements2 req = {.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, .pNext = &dedReq};
    VkImageMemoryRequirementsInfo2 reqInfo = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2, .image = out->img};
    vkGetImageMemoryRequirements2(dev, &reqInfo, &req);

    uint32_t bits = fdProps.memoryTypeBits & req.memoryRequirements.memoryTypeBits;
    uint32_t mt = find_mem_type(bits, 0);
    printf("  fd memoryTypeBits=0x%x image bits=0x%x -> ", fdProps.memoryTypeBits, req.memoryRequirements.memoryTypeBits);
    print_mem_type(mt);
    printf("; req size %llu, dedicated pref=%d req=%d\n", (unsigned long long)req.memoryRequirements.size, dedReq.prefersDedicatedAllocation, dedReq.requiresDedicatedAllocation);
    if (mt == UINT32_MAX) return -1;

    int dupfd = fcntl(fd, F_DUPFD_CLOEXEC, 0);
    VkMemoryDedicatedAllocateInfo ded = {.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, .image = out->img};
    VkImportMemoryFdInfoKHR imp = {.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR, .pNext = &ded, .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, .fd = dupfd};
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &imp, .allocationSize = req.memoryRequirements.size, .memoryTypeIndex = mt};
    r = vkAllocateMemory(dev, &ai, NULL, &out->mem);
    if (r != VK_SUCCESS) { printf("  vkAllocateMemory(import) failed: %d\n", r); close(dupfd); return -1; }
    r = vkBindImageMemory(dev, out->img, out->mem, 0);
    if (r != VK_SUCCESS) { printf("  vkBindImageMemory failed: %d\n", r); return -1; }
    printf("  import OK\n");
    return 0;
}

static VkCommandBuffer begin_cmd_on(VkCommandPool p);
static VkCommandBuffer begin_cmd(void) { return begin_cmd_on(pool); }
static VkCommandBuffer begin_cmd_on(VkCommandPool p) {
    VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = p, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VkCommandBuffer cb; CHECK(vkAllocateCommandBuffers(dev, &ai, &cb));
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    CHECK(vkBeginCommandBuffer(cb, &bi));
    return cb;
}

static void barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to, uint32_t srcQ, uint32_t dstQ) {
    VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT,
                              .dstAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT, .oldLayout = from, .newLayout = to,
                              .srcQueueFamilyIndex = srcQ, .dstQueueFamilyIndex = dstQ, .image = img,
                              .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}

static void submit_wait_on(VkQueue q, VkCommandPool p, VkCommandBuffer cb);
static void submit_wait(VkCommandBuffer cb) { submit_wait_on(queue, pool, cb); }
static void submit_wait_on(VkQueue q, VkCommandPool p, VkCommandBuffer cb) {
    CHECK(vkEndCommandBuffer(cb));
    VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence f; CHECK(vkCreateFence(dev, &fci, NULL, &f));
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
    CHECK(vkQueueSubmit(q, 1, &si, f));
    CHECK(vkWaitForFences(dev, 1, &f, VK_TRUE, UINT64_MAX));
    vkDestroyFence(dev, f, NULL);
    vkFreeCommandBuffers(dev, p, 1, &cb);
}

int main(int argc, char** argv) {
    const int W = 2560, H = 1600;
    // ---- instance / device
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_2};
    VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    CHECK(vkCreateInstance(&ici, NULL, &inst));
    uint32_t n = 0; vkEnumeratePhysicalDevices(inst, &n, NULL);
    VkPhysicalDevice pds[8]; vkEnumeratePhysicalDevices(inst, &n, pds);
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(pds[i], &p);
        printf("GPU %u: %s (vendor 0x%x) driver %u.%u.%u\n", i, p.deviceName, p.vendorID, VK_VERSION_MAJOR(p.driverVersion), VK_VERSION_MINOR(p.driverVersion), VK_VERSION_PATCH(p.driverVersion));
        if (p.vendorID == 0x10de) phys = pds[i];
    }
    if (!phys) { fprintf(stderr, "no NVIDIA vulkan device\n"); return 1; }

    uint32_t ne = 0; vkEnumerateDeviceExtensionProperties(phys, NULL, &ne, NULL);
    VkExtensionProperties* exts = calloc(ne, sizeof(*exts)); vkEnumerateDeviceExtensionProperties(phys, NULL, &ne, exts);
    const char* want[] = {"VK_KHR_external_memory_fd", "VK_EXT_external_memory_dma_buf", "VK_EXT_image_drm_format_modifier", "VK_KHR_external_semaphore_fd",
                          "VK_KHR_external_fence_fd", "VK_EXT_queue_family_foreign", "VK_EXT_physical_device_drm", "VK_KHR_image_format_list", "VK_KHR_external_memory",
                          "VK_KHR_external_semaphore", "VK_KHR_external_fence", "VK_KHR_dedicated_allocation", "VK_KHR_get_memory_requirements2", "VK_EXT_external_memory_host"};
    const char* enable[32]; int nen = 0;
    for (size_t i = 0; i < sizeof(want) / sizeof(*want); i++) {
        int h = has_ext(exts, ne, want[i]);
        printf("  ext %-38s %s\n", want[i], h ? "yes" : "NO");
        if (h) enable[nen++] = want[i];
    }

    // semaphore sync_file support
    {
        VkPhysicalDeviceExternalSemaphoreInfo si = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO, .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT};
        VkExternalSemaphoreProperties sp = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES};
        vkGetPhysicalDeviceExternalSemaphoreProperties(phys, &si, &sp);
        printf("  semaphore SYNC_FD: features=0x%x [%s%s]\n", sp.externalSemaphoreFeatures, sp.externalSemaphoreFeatures & VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT ? "EXPORT " : "",
               sp.externalSemaphoreFeatures & VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT ? "IMPORT " : "");
        VkPhysicalDeviceExternalFenceInfo fi = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_FENCE_INFO, .handleType = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT};
        VkExternalFenceProperties fp = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_FENCE_PROPERTIES};
        vkGetPhysicalDeviceExternalFenceProperties(phys, &fi, &fp);
        printf("  fence SYNC_FD: features=0x%x [%s%s]\n", fp.externalFenceFeatures, fp.externalFenceFeatures & VK_EXTERNAL_FENCE_FEATURE_EXPORTABLE_BIT ? "EXPORT " : "",
               fp.externalFenceFeatures & VK_EXTERNAL_FENCE_FEATURE_IMPORTABLE_BIT ? "IMPORT " : "");
    }

    // queue family: prefer transfer-only (copy engine)
    uint32_t nq = 0; vkGetPhysicalDeviceQueueFamilyProperties(phys, &nq, NULL);
    VkQueueFamilyProperties qp[16]; vkGetPhysicalDeviceQueueFamilyProperties(phys, &nq, qp);
    qfam = UINT32_MAX;
    for (uint32_t i = 0; i < nq; i++) {
        printf("  queue family %u: flags 0x%x count %u\n", i, qp[i].queueFlags, qp[i].queueCount);
        if ((qp[i].queueFlags & VK_QUEUE_TRANSFER_BIT) && !(qp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && !(qp[i].queueFlags & VK_QUEUE_COMPUTE_BIT) && qfam == UINT32_MAX) qfam = i;
    }
    if (argc > 1 && !strcmp(argv[1], "gfx")) qfam = UINT32_MAX;
    if (qfam == UINT32_MAX) for (uint32_t i = 0; i < nq; i++) if (qp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { qfam = i; break; }
    printf("  using queue family %u\n", qfam);

    gfam = UINT32_MAX;
    for (uint32_t i = 0; i < nq; i++) if (qp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { gfam = i; break; }
    float prio = 1.f;
    VkDeviceQueueCreateInfo qcis[2] = {{.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = gfam, .queueCount = 1, .pQueuePriorities = &prio},
                                       {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = qfam, .queueCount = 1, .pQueuePriorities = &prio}};
    VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = qfam == gfam ? 1 : 2, .pQueueCreateInfos = qcis, .enabledExtensionCount = nen, .ppEnabledExtensionNames = enable};
    CHECK(vkCreateDevice(phys, &dci, NULL, &dev));
    vkGetDeviceQueue(dev, qfam, 0, &queue);
    vkGetDeviceQueue(dev, gfam, 0, &gqueue);
    { VkCommandPoolCreateInfo gp = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = gfam, .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT}; CHECK(vkCreateCommandPool(dev, &gp, NULL, &gpool)); }
    pGetMemoryFdProps = (void*)vkGetDeviceProcAddr(dev, "vkGetMemoryFdPropertiesKHR");
    pGetSemaphoreFd = (void*)vkGetDeviceProcAddr(dev, "vkGetSemaphoreFdKHR");
    pImportSemaphoreFd = (void*)vkGetDeviceProcAddr(dev, "vkImportSemaphoreFdKHR");
    pGetFenceFd = (void*)vkGetDeviceProcAddr(dev, "vkGetFenceFdKHR");
    VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = qfam, .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT};
    CHECK(vkCreateCommandPool(dev, &pci, NULL, &pool));

    // ---- Intel linear buffer (the scanout buffer Intel would display)
    int ifd = open("/dev/dri/renderD129", O_RDWR | O_CLOEXEC);
    struct gbm_device* igbm = gbm_create_device(ifd);
    uint64_t linear = DRM_FORMAT_MOD_LINEAR;
    struct gbm_bo* ibo = gbm_bo_create_with_modifiers2(igbm, W, H, DRM_FORMAT_XRGB8888, &linear, 1, GBM_BO_USE_RENDERING | GBM_BO_USE_SCANOUT);
    if (!ibo) { perror("intel gbm_bo_create"); return 1; }
    uint32_t istride = gbm_bo_get_stride(ibo);
    int idma = gbm_bo_get_fd(ibo);
    printf("\nIntel bo: stride %u modifier 0x%llx fd %d\n", istride, (unsigned long long)gbm_bo_get_modifier(ibo), idma);

    printf("\n== NVIDIA Vulkan: query import support ==\n");
    query_modifier_support(DRM_FORMAT_MOD_LINEAR, VK_IMAGE_USAGE_TRANSFER_DST_BIT, "linear as TRANSFER_DST");
    query_modifier_support(DRM_FORMAT_MOD_LINEAR, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, "linear as TRANSFER_SRC|DST");
    query_modifier_support(DRM_FORMAT_MOD_LINEAR, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, "linear as COLOR_ATTACHMENT");

    printf("\n== import Intel linear dma-buf into NVIDIA Vulkan as TRANSFER_DST ==\n");
    vkimg dst = {0};
    if (import_dmabuf(idma, W, H, DRM_FORMAT_MOD_LINEAR, istride, 0, VK_IMAGE_USAGE_TRANSFER_DST_BIT, &dst) != 0) { printf("  -> FAILED\n"); return 1; }

    // ---- NVIDIA source: (a) a plain device-local optimal image, (b) an NVIDIA GBM bo imported via modifier
    printf("\n== NVIDIA GBM buffer (what aquamarine renders into) ==\n");
    int nfd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    struct gbm_device* ngbm = gbm_create_device(nfd);
    uint64_t vkmods[64]; uint32_t nvkmods = 0;
    {
        VkDrmFormatModifierPropertiesListEXT ml = {.sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT};
        VkFormatProperties2 fp = {.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2, .pNext = &ml};
        vkGetPhysicalDeviceFormatProperties2(phys, VK_FORMAT_B8G8R8A8_UNORM, &fp);
        VkDrmFormatModifierPropertiesEXT props[64]; ml.pDrmFormatModifierProperties = props; if (ml.drmFormatModifierCount > 64) ml.drmFormatModifierCount = 64;
        vkGetPhysicalDeviceFormatProperties2(phys, VK_FORMAT_B8G8R8A8_UNORM, &fp);
        printf("  NVIDIA Vulkan B8G8R8A8 modifiers (%u):", ml.drmFormatModifierCount);
        for (uint32_t i = 0; i < ml.drmFormatModifierCount; i++) { printf(" 0x%llx(feat 0x%x)", (unsigned long long)props[i].drmFormatModifier, props[i].drmFormatModifierTilingFeatures); vkmods[nvkmods++] = props[i].drmFormatModifier; }
        printf("\n");
    }
    struct gbm_bo* nbo = NULL;
    if (argc > 1 && !strcmp(argv[1], "vkmods")) {
        nbo = gbm_bo_create_with_modifiers2(ngbm, W, H, DRM_FORMAT_XRGB8888, vkmods, nvkmods, GBM_BO_USE_RENDERING);
        printf("  gbm_bo_create_with_modifiers2(vulkan modifier list, RENDERING): %s\n", nbo ? "OK" : strerror(errno));
    }
    if (!nbo) nbo = gbm_bo_create(ngbm, W, H, DRM_FORMAT_XRGB8888, GBM_BO_USE_RENDERING);
    vkimg src = {0}; int haveSrcBo = 0;
    if (!nbo) printf("  nvidia gbm_bo_create(RENDERING) failed: %s\n", strerror(errno));
    else {
        uint64_t nmod = gbm_bo_get_modifier(nbo); uint32_t nstride = gbm_bo_get_stride(nbo); int ndma = gbm_bo_get_fd(nbo);
        printf("  nvidia bo: modifier 0x%llx stride %u offset %u fd %d\n", (unsigned long long)nmod, nstride, gbm_bo_get_offset(nbo, 0), ndma);
        if (query_modifier_support(nmod, VK_IMAGE_USAGE_TRANSFER_SRC_BIT, "nvidia bo modifier as TRANSFER_SRC")) {
            if (import_dmabuf(ndma, W, H, nmod, nstride, gbm_bo_get_offset(nbo, 0), VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, &src) == 0) haveSrcBo = 1;
        }
    }
    if (!haveSrcBo) {
        printf("  falling back to a plain optimal-tiling NVIDIA image as source\n");
        VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_B8G8R8A8_UNORM, .extent = {W, H, 1},
                                .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                                .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
        CHECK(vkCreateImage(dev, &ci, NULL, &src.img));
        VkMemoryRequirements mr; vkGetImageMemoryRequirements(dev, src.img, &mr);
        VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = find_mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
        CHECK(vkAllocateMemory(dev, &ai, NULL, &src.mem));
        CHECK(vkBindImageMemory(dev, src.img, src.mem, 0));
    }

    // ---- clear source to a known colour, copy to dst, verify on CPU via Intel mapping
    printf("\n== copy test ==\n");
    {
        VkCommandBuffer cb = begin_cmd_on(gpool);
        barrier(cb, src.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED);
        VkClearColorValue col = {.float32 = {0.2f, 0.4f, 0.6f, 1.f}}; // R G B -> memory BGRA: 99 66 33 ff -> u32 0xff336699
        VkImageSubresourceRange rng = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cb, src.img, VK_IMAGE_LAYOUT_GENERAL, &col, 1, &rng);
        submit_wait_on(gqueue, gpool, cb);
    }
    double bestWall = 1e9, sumCpu = 0; int iters = 60;
    for (int it = 0; it < iters; it++) {
        double c0 = cpu_ms(), t0 = now_ms();
        VkCommandBuffer cb = begin_cmd();
        barrier(cb, dst.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_QUEUE_FAMILY_FOREIGN_EXT, qfam);
        VkImageCopy region = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .extent = {W, H, 1}};
        vkCmdCopyImage(cb, src.img, VK_IMAGE_LAYOUT_GENERAL, dst.img, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
        barrier(cb, dst.img, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, qfam, VK_QUEUE_FAMILY_FOREIGN_EXT);
        submit_wait(cb);
        double wall = now_ms() - t0, cpu = cpu_ms() - c0;
        if (wall < bestWall) bestWall = wall;
        sumCpu += cpu;
    }
    printf("  copy 2560x1600 XRGB8888 NVIDIA->Intel sysmem: best wall %.3f ms, avg CPU %.3f ms per copy (incl. fence wait)\n", bestWall, sumCpu / iters);

    {
        uint32_t st; void* md = NULL;
        uint32_t* p = gbm_bo_map(ibo, 0, 0, W, H, GBM_BO_TRANSFER_READ, &st, &md);
        if (!p) printf("  intel map failed: %s\n", strerror(errno));
        else {
            uint32_t a = p[0], b = ((uint32_t*)((char*)p + st * (H / 2)))[W / 2], c = ((uint32_t*)((char*)p + st * (H - 1)))[W - 1];
            printf("  Intel CPU view: [0,0]=0x%08x [mid]=0x%08x [last]=0x%08x  (expected 0x??336699) -> %s\n", a, b, c,
                   ((a & 0xffffff) == 0x336699 && (b & 0xffffff) == 0x336699 && (c & 0xffffff) == 0x336699) ? "CORRECT" : "WRONG");
            gbm_bo_unmap(ibo, md);
        }
    }

    // ---- sync_file semaphore export test (needed to hand a fence to KMS / wait on Hyprland's render fence)
    printf("\n== sync_file semaphore export/import ==\n");
    if (pGetSemaphoreFd) {
        VkExportSemaphoreCreateInfo esci = {.sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO, .handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT};
        VkSemaphoreCreateInfo sci = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &esci};
        VkSemaphore sem; VkResult r = vkCreateSemaphore(dev, &sci, NULL, &sem);
        printf("  create exportable SYNC_FD semaphore: %d\n", r);
        if (r == VK_SUCCESS) {
            VkCommandBuffer cb = begin_cmd();
            VkImageCopy region = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .extent = {W, H, 1}};
            vkCmdCopyImage(cb, src.img, VK_IMAGE_LAYOUT_GENERAL, dst.img, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
            CHECK(vkEndCommandBuffer(cb));
            VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb, .signalSemaphoreCount = 1, .pSignalSemaphores = &sem};
            double t0 = now_ms();
            CHECK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
            VkSemaphoreGetFdInfoKHR gi = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR, .semaphore = sem, .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT};
            int sfd = -1; r = pGetSemaphoreFd(dev, &gi, &sfd);
            double t1 = now_ms();
            printf("  vkGetSemaphoreFdKHR: %d fd=%d (submit+export %.3f ms)\n", r, sfd, t1 - t0);
            if (sfd >= 0) {
                struct pollfd pf = {.fd = sfd, .events = POLLIN};
                int pr = poll(&pf, 1, 1000);
                printf("  sync_file signalled after %.3f ms (poll=%d)\n", now_ms() - t1, pr);
                close(sfd);
            }
            vkQueueWaitIdle(queue);
            vkFreeCommandBuffers(dev, pool, 1, &cb);

            // import test: export a sync_file from submit A, import it into a wait semaphore for submit B
            VkSemaphore semA, semB;
            CHECK(vkCreateSemaphore(dev, &sci, NULL, &semA));
            VkSemaphoreCreateInfo plain = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            CHECK(vkCreateSemaphore(dev, &plain, NULL, &semB));
            VkCommandBuffer ca = begin_cmd(); vkCmdCopyImage(ca, src.img, VK_IMAGE_LAYOUT_GENERAL, dst.img, VK_IMAGE_LAYOUT_GENERAL, 1, &region); CHECK(vkEndCommandBuffer(ca));
            VkSubmitInfo sa = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &ca, .signalSemaphoreCount = 1, .pSignalSemaphores = &semA};
            CHECK(vkQueueSubmit(queue, 1, &sa, VK_NULL_HANDLE));
            VkSemaphoreGetFdInfoKHR ga = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR, .semaphore = semA, .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT};
            int fa = -1; CHECK(pGetSemaphoreFd(dev, &ga, &fa));
            VkImportSemaphoreFdInfoKHR ii = {.sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR, .semaphore = semB, .flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT,
                                             .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT, .fd = fa};
            VkResult ir = pImportSemaphoreFd(dev, &ii);
            printf("  vkImportSemaphoreFdKHR(sync_file, TEMPORARY): %d\n", ir);
            if (ir == VK_SUCCESS) {
                VkCommandBuffer cbb = begin_cmd(); vkCmdCopyImage(cbb, src.img, VK_IMAGE_LAYOUT_GENERAL, dst.img, VK_IMAGE_LAYOUT_GENERAL, 1, &region); CHECK(vkEndCommandBuffer(cbb));
                VkPipelineStageFlags ws = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
                VkSubmitInfo sb = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1, .pWaitSemaphores = &semB, .pWaitDstStageMask = &ws, .commandBufferCount = 1, .pCommandBuffers = &cbb};
                VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; VkFence ff; CHECK(vkCreateFence(dev, &fci, NULL, &ff));
                CHECK(vkQueueSubmit(queue, 1, &sb, ff));
                VkResult wr = vkWaitForFences(dev, 1, &ff, VK_TRUE, 2000000000ull);
                printf("  submit B waiting on imported semaphore: fence wait -> %d (0=ok)\n", wr);
                vkDestroyFence(dev, ff, NULL);
                vkFreeCommandBuffers(dev, pool, 1, &cbb);
            }
            vkQueueWaitIdle(queue);
            vkFreeCommandBuffers(dev, pool, 1, &ca);
            // also: import a signalled sync_file from EGL-like source? emulate by importing an already-signalled fd
            VkSemaphore semC; CHECK(vkCreateSemaphore(dev, &plain, NULL, &semC));
            VkSemaphoreGetFdInfoKHR gc = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR, .semaphore = semA, .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT};
            VkSubmitInfo sa2 = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .signalSemaphoreCount = 1, .pSignalSemaphores = &semA};
            CHECK(vkQueueSubmit(queue, 1, &sa2, VK_NULL_HANDLE)); vkQueueWaitIdle(queue);
            int fc = -1; CHECK(pGetSemaphoreFd(dev, &gc, &fc));
            VkImportSemaphoreFdInfoKHR ic = {.sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR, .semaphore = semC, .flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT, .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT, .fd = fc};
            printf("  import of already-signalled sync_file: %d\n", pImportSemaphoreFd(dev, &ic));
        }
    }
    printf("\nDONE\n");
    return 0;
}
