/* Our own Vulkan renderer: Vulkan itself (VULKAN.md). The instance and device through SDL3's Vulkan
   support (its loader, its surface), memory in blocks with ranges handed out, vertex and index
   buffers as ranges of large buffers, frames in flight each with a command pool, a fence and a ring
   (constants, drawing from memory, staging), objects destroyed once the work using them is done,
   the bindless texture and sampler set, samplers by Direct3D state, the swapchain.
   ETM_VK_VALIDATE=1: Khronos validation layer on, its messages in the log. */
#define _GNU_SOURCE
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <stdlib.h>
#include <string.h>
#include "vk.h"
#include "options.h"
#include "win.h"

#define VK_DEF_FN(n) __attribute__((visibility("hidden"))) PFN_vk##n vk##n;
VK_GLOBAL_FNS(VK_DEF_FN)
VK_INSTANCE_FNS(VK_DEF_FN)
VK_DEVICE_FNS(VK_DEF_FN)
#undef VK_DEF_FN
static PFN_vkGetInstanceProcAddr gipa;
static PFN_vkCreateDebugUtilsMessengerEXT vkCreateDebugUtilsMessengerEXT_;
static VkDebugUtilsMessengerEXT messenger;

VkR R;

#define CHECK(x) do { VkResult _r = (x); if (_r != VK_SUCCESS) { LOG(LOG_ALWAYS, "vulkan: %s failed (%d)", #x, _r); return 0; } } while (0)

static VKAPI_ATTR VkBool32 VKAPI_CALL debug_cb(VkDebugUtilsMessageSeverityFlagBitsEXT sev, VkDebugUtilsMessageTypeFlagsEXT type,
                                               const VkDebugUtilsMessengerCallbackDataEXT *d, void *user)
{
    (void)type; (void)user;
    static int n;
    if (n++ < 400)
        LOG(LOG_ALWAYS, "vulkan %s: %s", sev >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT ? "ERROR" : "warning",
            d->pMessage);
    return VK_FALSE;
}

/* ------------------------------------------------------------------ instance and device */
static int has_ext(const VkExtensionProperties *e, uint32_t n, const char *name)
{
    for (uint32_t i = 0; i < n; i++)
        if (!strcmp(e[i].extensionName, name)) return 1;
    return 0;
}

static int make_instance(void)
{
    if (!SDL_Vulkan_LoadLibrary(NULL)) {
        LOG(LOG_ALWAYS, "vulkan: SDL cannot load the Vulkan loader: %s", SDL_GetError());
        return 0;
    }
    gipa = (PFN_vkGetInstanceProcAddr)SDL_Vulkan_GetVkGetInstanceProcAddr();
    if (!gipa) return 0;
#define VK_LOAD_G(n) vk##n = (PFN_vk##n)gipa(NULL, "vk" #n);
    VK_GLOBAL_FNS(VK_LOAD_G)
#undef VK_LOAD_G
    Uint32 nsdl = 0;
    const char *const *sdl_ext = SDL_Vulkan_GetInstanceExtensions(&nsdl);
    const char *exts[16];
    uint32_t next = 0;
    for (Uint32 i = 0; i < nsdl && next < 12; i++) exts[next++] = sdl_ext[i];
    const char *layers[1];
    uint32_t nlayers = 0;
    const char *v = getenv("ETM_VK_VALIDATE");
    if (v && *v && *v != '0') {
        uint32_t n = 0;
        vkEnumerateInstanceLayerProperties(&n, NULL);
        VkLayerProperties *lp = calloc(n ? n : 1, sizeof *lp);
        vkEnumerateInstanceLayerProperties(&n, lp);
        for (uint32_t i = 0; i < n; i++)
            if (!strcmp(lp[i].layerName, "VK_LAYER_KHRONOS_validation")) {
                layers[nlayers++] = "VK_LAYER_KHRONOS_validation";
                exts[next++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
                R.validation = 1;
            }
        free(lp);
        LOG(LOG_ALWAYS, "vulkan: validation layer %s", R.validation ? "on" : "asked for but not installed");
    }
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "Enter the Matrix", 1, "etm-native", 1, VK_API_VERSION_1_3};
    VkInstanceCreateInfo ci = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, nlayers, layers, next, exts};
    CHECK(vkCreateInstance(&ci, NULL, &R.instance));
#define VK_LOAD_I(n) vk##n = (PFN_vk##n)gipa(R.instance, "vk" #n);
    VK_INSTANCE_FNS(VK_LOAD_I)
#undef VK_LOAD_I
    if (R.validation) {
        vkCreateDebugUtilsMessengerEXT_ = (PFN_vkCreateDebugUtilsMessengerEXT)gipa(R.instance, "vkCreateDebugUtilsMessengerEXT");
        VkDebugUtilsMessengerCreateInfoEXT mi = {VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT, NULL, 0,
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
            VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT, debug_cb, NULL};
        if (vkCreateDebugUtilsMessengerEXT_) vkCreateDebugUtilsMessengerEXT_(R.instance, &mi, NULL, &messenger);
    }
    return 1;
}

static int pick_device(void)
{
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(R.instance, &n, NULL);
    if (!n) { LOG(LOG_ALWAYS, "vulkan: no device"); return 0; }
    VkPhysicalDevice *pd = calloc(n, sizeof *pd);
    vkEnumeratePhysicalDevices(R.instance, &n, pd);
    int best = -1, best_score = -1;
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(pd[i], &p);
        if (p.apiVersion < VK_API_VERSION_1_3) continue;
        int score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3 :
                    p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2 :
                    p.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU ? 0 : 1;
        const char *want = getenv("ETM_VK_DEVICE");
        if (want && strstr(p.deviceName, want)) score = 10;
        if (score > best_score) best = (int)i, best_score = score;
    }
    if (best < 0) { free(pd); LOG(LOG_ALWAYS, "vulkan: no Vulkan 1.3 device"); return 0; }
    R.phys = pd[best];
    free(pd);
    VkPhysicalDeviceVulkan11Properties p11 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES};
    VkPhysicalDeviceVulkan12Properties p12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES, &p11};
    VkPhysicalDeviceProperties2 p2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &p12};
    vkGetPhysicalDeviceProperties2(R.phys, &p2);
    R.props = p2.properties;
    memcpy(R.uuid, p11.deviceUUID, 16);
    R.driver_id = p12.driverID;
    /* (DXVK's rule: float controls only with independent denormal modes, 32-bit RTE and flush-to-zero;
       float_controls2 besides only with signed zero/inf/NaN preserve: make_device) */
    R.fcon = p12.denormBehaviorIndependence != VK_SHADER_FLOAT_CONTROLS_INDEPENDENCE_NONE &&
             p12.shaderRoundingModeRTEFloat32 && p12.shaderDenormFlushToZeroFloat32;
    R.fc2 = R.fcon && p12.shaderSignedZeroInfNanPreserveFloat32;
    vkGetPhysicalDeviceMemoryProperties(R.phys, &R.memprops);
    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(R.phys, &nq, NULL);
    VkQueueFamilyProperties *qp = calloc(nq, sizeof *qp);
    vkGetPhysicalDeviceQueueFamilyProperties(R.phys, &nq, qp);
    R.qfamily = ~0u;
    for (uint32_t i = 0; i < nq; i++)
        if (qp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { R.qfamily = i; break; }
    free(qp);
    if (R.qfamily == ~0u) return 0;
    LOG(LOG_ALWAYS, "vulkan: %s (driver %s, Vulkan %u.%u.%u)", R.props.deviceName, p12.driverInfo,
        VK_API_VERSION_MAJOR(R.props.apiVersion), VK_API_VERSION_MINOR(R.props.apiVersion), VK_API_VERSION_PATCH(R.props.apiVersion));
    return 1;
}

static int make_device(void)
{
    VkPhysicalDeviceVulkan13Features f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &f13};
    VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f12};
    vkGetPhysicalDeviceFeatures2(R.phys, &f2);
    const char *missing = !f13.dynamicRendering ? "dynamicRendering" : !f13.synchronization2 ? "synchronization2" :
        !f13.shaderDemoteToHelperInvocation ? "shaderDemoteToHelperInvocation" : !f12.runtimeDescriptorArray ? "runtimeDescriptorArray" :
        !f12.descriptorBindingPartiallyBound ? "descriptorBindingPartiallyBound" :
        !f12.descriptorBindingSampledImageUpdateAfterBind ? "descriptorBindingSampledImageUpdateAfterBind" :
        !f12.descriptorBindingUpdateUnusedWhilePending ? "descriptorBindingUpdateUnusedWhilePending" :
        !f2.features.textureCompressionBC ? "textureCompressionBC" : NULL;
    if (missing) { LOG(LOG_ALWAYS, "vulkan: the device lacks %s", missing); return 0; }
    VkPhysicalDeviceVulkan13Features w13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    w13.dynamicRendering = w13.synchronization2 = w13.shaderDemoteToHelperInvocation = VK_TRUE;
    w13.maintenance4 = f13.maintenance4;
    VkPhysicalDeviceVulkan12Features w12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &w13};
    w12.runtimeDescriptorArray = w12.descriptorBindingPartiallyBound = w12.descriptorBindingSampledImageUpdateAfterBind =
        w12.descriptorBindingUpdateUnusedWhilePending = w12.descriptorIndexing = VK_TRUE;
    w12.shaderSampledImageArrayNonUniformIndexing = f12.shaderSampledImageArrayNonUniformIndexing;
    w12.hostQueryReset = f12.hostQueryReset;
    R.ttimes = f12.hostQueryReset;
    VkPhysicalDeviceFeatures2 w2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &w12};
    w2.features.samplerAnisotropy = f2.features.samplerAnisotropy;
    w2.features.textureCompressionBC = VK_TRUE;
    w2.features.fillModeNonSolid = f2.features.fillModeNonSolid;
    w2.features.independentBlend = f2.features.independentBlend;
    w2.features.depthBiasClamp = f2.features.depthBiasClamp;
    w2.features.depthClamp = f2.features.depthClamp;
    w2.features.shaderClipDistance = f2.features.shaderClipDistance;
    uint32_t ne = 0;
    vkEnumerateDeviceExtensionProperties(R.phys, NULL, &ne, NULL);
    VkExtensionProperties *ep = calloc(ne ? ne : 1, sizeof *ep);
    vkEnumerateDeviceExtensionProperties(R.phys, NULL, &ne, ep);
    const char *exts[8];
    uint32_t next = 0;
    if (has_ext(ep, ne, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) exts[next++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    /* (float_controls2: the shaders' default fast-math mode as DXVK's, vk_d3d9.c module_of) */
    VkPhysicalDeviceShaderFloatControls2FeaturesKHR fc2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT_CONTROLS_2_FEATURES_KHR};
    if (R.fc2 && has_ext(ep, ne, VK_KHR_SHADER_FLOAT_CONTROLS_2_EXTENSION_NAME)) {
        VkPhysicalDeviceFeatures2 q = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &fc2};
        vkGetPhysicalDeviceFeatures2(R.phys, &q);
        R.fc2 = fc2.shaderFloatControls2;
        if (R.fc2) {
            exts[next++] = VK_KHR_SHADER_FLOAT_CONTROLS_2_EXTENSION_NAME;
            fc2.pNext = w12.pNext;
            w12.pNext = &fc2;
        }
    } else R.fc2 = 0;
    if (has_ext(ep, ne, VK_EXT_SAMPLE_LOCATIONS_EXTENSION_NAME)) {
        exts[next++] = VK_EXT_SAMPLE_LOCATIONS_EXTENSION_NAME;
        VkPhysicalDeviceSampleLocationsPropertiesEXT sl = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLE_LOCATIONS_PROPERTIES_EXT};
        VkPhysicalDeviceProperties2 p2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &sl};
        vkGetPhysicalDeviceProperties2(R.phys, &p2);
        R.sample_locations = 1;
        R.sample_location_counts = sl.sampleLocationSampleCounts;
        R.variable_sample_locations = sl.variableSampleLocations;
    }
    free(ep);
    float prio = 1;
    VkDeviceQueueCreateInfo qi = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, R.qfamily, 1, &prio};
    VkDeviceCreateInfo di = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &w2, 0, 1, &qi, 0, NULL, next, exts, NULL};
    CHECK(vkCreateDevice(R.phys, &di, NULL, &R.dev));
#define VK_LOAD_D(n) vk##n = (PFN_vk##n)vkGetDeviceProcAddr(R.dev, "vk" #n);
    VK_DEVICE_FNS(VK_LOAD_D)
#undef VK_LOAD_D
    vkGetDeviceQueue(R.dev, R.qfamily, 0, &R.queue);
    LOG(LOG_ALWAYS, "vulkan: the shaders' float controls (as DXVK's): round to nearest even and denormals flushed %s, "
        "float_controls2's default mode %s", R.fcon ? "yes" : "no", R.fc2 ? "yes" : "no");
    return 1;
}

/* ------------------------------------------------------------------ memory: blocks and ranges */
#define BLOCK_SIZE (64ull << 20)
struct VkBlock {
    VkBlock *next;
    VkDeviceMemory mem;
    VkDeviceSize size;
    uint32_t type;
    uint8_t *map;
    struct { VkDeviceSize off, size; } *free;
    int nfree, capfree;
    int dedicated;
};
static VkBlock *blocks;

static int mem_type(uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags avoid)
{
    for (uint32_t i = 0; i < R.memprops.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (R.memprops.memoryTypes[i].propertyFlags & want) == want &&
            !(R.memprops.memoryTypes[i].propertyFlags & avoid))
            return (int)i;
    for (uint32_t i = 0; i < R.memprops.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (R.memprops.memoryTypes[i].propertyFlags & want) == want)
            return (int)i;
    return -1;
}

static int block_take(VkBlock *b, VkDeviceSize size, VkDeviceSize align, VkDeviceSize *off)
{
    for (int i = 0; i < b->nfree; i++) {
        VkDeviceSize o = (b->free[i].off + align - 1) & ~(align - 1);
        VkDeviceSize end = b->free[i].off + b->free[i].size;
        if (o + size > end) continue;
        VkDeviceSize head = o - b->free[i].off, tail = end - (o + size);
        if (head && tail) {
            if (b->nfree == b->capfree) {
                b->capfree *= 2;
                b->free = realloc(b->free, b->capfree * sizeof *b->free);
            }
            memmove(&b->free[i + 2], &b->free[i + 1], (b->nfree - i - 1) * sizeof *b->free);
            b->nfree++;
            b->free[i].size = head;
            b->free[i + 1].off = o + size;
            b->free[i + 1].size = tail;
        } else if (head) {
            b->free[i].size = head;
        } else if (tail) {
            b->free[i].off = o + size;
            b->free[i].size = tail;
        } else {
            memmove(&b->free[i], &b->free[i + 1], (b->nfree - i - 1) * sizeof *b->free);
            b->nfree--;
        }
        *off = o;
        return 1;
    }
    return 0;
}

static void block_give(VkBlock *b, VkDeviceSize off, VkDeviceSize size)
{
    int i = 0;
    while (i < b->nfree && b->free[i].off < off) i++;
    if (b->nfree == b->capfree) {
        b->capfree *= 2;
        b->free = realloc(b->free, b->capfree * sizeof *b->free);
    }
    memmove(&b->free[i + 1], &b->free[i], (b->nfree - i) * sizeof *b->free);
    b->free[i].off = off;
    b->free[i].size = size;
    b->nfree++;
    if (i + 1 < b->nfree && b->free[i].off + b->free[i].size == b->free[i + 1].off) {
        b->free[i].size += b->free[i + 1].size;
        memmove(&b->free[i + 1], &b->free[i + 2], (b->nfree - i - 2) * sizeof *b->free);
        b->nfree--;
    }
    if (i > 0 && b->free[i - 1].off + b->free[i - 1].size == b->free[i].off) {
        b->free[i - 1].size += b->free[i].size;
        memmove(&b->free[i], &b->free[i + 1], (b->nfree - i - 1) * sizeof *b->free);
        b->nfree--;
    }
}

int vkm_alloc(const VkMemoryRequirements *req, VkMemoryPropertyFlags want, VkMemoryPropertyFlags avoid, VkAlloc *out)
{
    int t = mem_type(req->memoryTypeBits, want, avoid);
    if (t < 0 && (want & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
        t = mem_type(req->memoryTypeBits, want & ~VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, avoid);
    if (t < 0) return 0;
    VkDeviceSize align = req->alignment < R.props.limits.bufferImageGranularity ? R.props.limits.bufferImageGranularity : req->alignment;
    int host = (R.memprops.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0;
    pthread_mutex_lock(&R.lock);
    VkBlock *b;
    VkDeviceSize off = 0;
    int dedicated = req->size > BLOCK_SIZE / 2;
    for (b = dedicated ? NULL : blocks; b; b = b->next)
        if (!b->dedicated && b->type == (uint32_t)t && block_take(b, req->size, align, &off)) break;
    if (!b) {
        VkDeviceSize size = dedicated ? req->size : BLOCK_SIZE;
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, size, (uint32_t)t};
        VkDeviceMemory mem;
        if (vkAllocateMemory(R.dev, &ai, NULL, &mem) != VK_SUCCESS) {
            pthread_mutex_unlock(&R.lock);
            LOG(LOG_ALWAYS, "vulkan: out of memory (%llu bytes, type %d)", (unsigned long long)size, t);
            return 0;
        }
        b = calloc(1, sizeof *b);
        b->mem = mem;
        b->size = size;
        b->type = (uint32_t)t;
        b->dedicated = dedicated;
        b->capfree = 16;
        b->free = malloc(b->capfree * sizeof *b->free);
        b->free[0].off = 0;
        b->free[0].size = size;
        b->nfree = 1;
        if (host) vkMapMemory(R.dev, mem, 0, VK_WHOLE_SIZE, 0, (void **)&b->map);
        b->next = blocks;
        blocks = b;
        block_take(b, req->size, align, &off);
    }
    pthread_mutex_unlock(&R.lock);
    out->blk = b;
    out->mem = b->mem;
    out->off = off;
    out->size = req->size;
    out->map = b->map ? b->map + off : NULL;
    return 1;
}

void vkm_free(VkAlloc *a)
{
    if (!a->blk) return;
    pthread_mutex_lock(&R.lock);
    block_give(a->blk, a->off, a->size);
    pthread_mutex_unlock(&R.lock);
    a->blk = NULL;
}

/* ------------------------------------------------------------------ vertex and index buffer arenas */
#define VKB_ARENA (32ull << 20)
typedef struct Arena { struct Arena *next; VkBuffer buf; VkAlloc mem; VkBlock fake; } Arena;
static Arena *arenas;

static VkMemoryPropertyFlags buffer_mem_flags(void)
{
    /* host-visible memory the GPU reads directly: on the Radeon 890M all of it is the device's own;
       on a discrete card device-local host-visible memory (resizable BAR) when it is large, else system
       memory [guess: the T7's RTX 4070 has resizable BAR on; not checked] */
    return VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
}

int vkb_sub(VkDeviceSize size, VkSub *out)
{
    size = (size + 255) & ~255ull;
    if (size > VKB_ARENA) {
        LOG(LOG_ALWAYS, "vulkan: a buffer of %llu bytes is larger than an arena", (unsigned long long)size);
        return 0;
    }
    pthread_mutex_lock(&R.lock);
    Arena *a;
    VkDeviceSize off = 0;
    for (a = arenas; a; a = a->next)
        if (block_take(&a->fake, size, 256, &off)) break;
    pthread_mutex_unlock(&R.lock);
    if (!a) {
        a = calloc(1, sizeof *a);
        VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, VKB_ARENA,
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
            VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
        if (vkCreateBuffer(R.dev, &bi, NULL, &a->buf) != VK_SUCCESS) { free(a); return 0; }
        VkMemoryRequirements mr;
        vkGetBufferMemoryRequirements(R.dev, a->buf, &mr);
        VkMemoryPropertyFlags f = buffer_mem_flags();
        if (!vkm_alloc(&mr, f | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, &a->mem) && !vkm_alloc(&mr, f, 0, &a->mem)) {
            vkDestroyBuffer(R.dev, a->buf, NULL);
            free(a);
            return 0;
        }
        vkBindBufferMemory(R.dev, a->buf, a->mem.mem, a->mem.off);
        a->fake.size = VKB_ARENA;
        a->fake.capfree = 16;
        a->fake.free = malloc(16 * sizeof *a->fake.free);
        a->fake.free[0].off = 0;
        a->fake.free[0].size = VKB_ARENA;
        a->fake.nfree = 1;
        pthread_mutex_lock(&R.lock);
        block_take(&a->fake, size, 256, &off);
        a->next = arenas;
        arenas = a;
        pthread_mutex_unlock(&R.lock);
    }
    out->buf = a->buf;
    out->off = off;
    out->size = size;
    out->map = a->mem.map + off;
    out->arena = a;
    return 1;
}

void vkb_sub_free(VkSub *s)
{
    if (!s->arena) return;
    pthread_mutex_lock(&R.lock);
    block_give(&((Arena *)s->arena)->fake, s->off, s->size);
    pthread_mutex_unlock(&R.lock);
    s->arena = NULL;
}

/* ------------------------------------------------------------------ the frame's ring */
#define RING_SIZE (32ull << 20)
#define RING_SLACK 4096             /* (a constant buffer's descriptor range past any offset) */
typedef struct VkChunk { struct VkChunk *next; VkBuffer buf; VkAlloc mem; VkDeviceSize size, used; VkDescriptorSet cset; int keep; } VkChunk;
static VkDescriptorPool cpool;

static VkChunk *chunk_new(VkDeviceSize size)
{
    VkChunk *c = calloc(1, sizeof *c);
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, size,
        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
    if (vkCreateBuffer(R.dev, &bi, NULL, &c->buf) != VK_SUCCESS) { free(c); return NULL; }
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(R.dev, c->buf, &mr);
    if (!vkm_alloc(&mr, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0, &c->mem)) {
        vkDestroyBuffer(R.dev, c->buf, NULL);
        free(c);
        return NULL;
    }
    vkBindBufferMemory(R.dev, c->buf, c->mem.mem, c->mem.off);
    c->size = size;
    VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, cpool, 1, &R.set1_layout};
    if (vkAllocateDescriptorSets(R.dev, &ai, &c->cset) == VK_SUCCESS) {
        /* (the vertex shader's chunks each see 256 registers from its offset: relative addressing reads
           them all through chunk 0; RING_SLACK keeps that range inside the buffer) */
        VkDescriptorBufferInfo bv[VK_VS_CHUNKS], bp = {c->buf, 0, (VK_PS_EXTRA + VK_PS_CONSTS) * 16};
        for (int k = 0; k < VK_VS_CHUNKS; k++) bv[k] = (VkDescriptorBufferInfo){c->buf, 0, VK_VS_CONSTS * 16};
        VkWriteDescriptorSet w[2] = {
            {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, c->cset, 0, 0, VK_VS_CHUNKS, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, NULL, bv, NULL},
            {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, c->cset, 1, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, NULL, &bp, NULL}};
        vkUpdateDescriptorSets(R.dev, 2, w, 0, NULL);
    }
    return c;
}

int vk_ring(VkDeviceSize size, VkDeviceSize align, VkRing *out)
{
    VkFrame *f = &R.frames[R.fi];
    VkChunk *c = f->chunks;
    VkDeviceSize a = align < R.props.limits.minUniformBufferOffsetAlignment ? R.props.limits.minUniformBufferOffsetAlignment : align;
    VkDeviceSize off = c ? (c->used + a - 1) & ~(a - 1) : 0;
    if (!c || off + size + RING_SLACK > c->size) {
        VkDeviceSize sz = size + RING_SLACK > RING_SIZE ? size + RING_SLACK : RING_SIZE;
        VkChunk *n = chunk_new(sz);
        if (!n) return 0;
        n->next = f->chunks;
        f->chunks = n;
        c = n;
        off = 0;
    }
    c->used = off + size;
    out->buf = c->buf;
    out->off = off;
    out->map = c->mem.map + off;
    out->cset = c->cset;
    return 1;
}

static void ring_reset(VkFrame *f)
{
    /* the first chunk made stays (reused); overflow ones go */
    VkChunk *keep = NULL, *c = f->chunks;
    while (c) {
        VkChunk *n = c->next;
        if (!n) { keep = c; keep->used = 0; break; }
        vkDestroyBuffer(R.dev, c->buf, NULL);
        vkm_free(&c->mem);
        /* (its descriptor set stays allocated in the pool; the pool is large: [guess] enough) */
        free(c);
        c = n;
    }
    f->chunks = keep;
}

/* ------------------------------------------------------------------ destroyed later */
static void later_add(VkLater *l)
{
    VkFrame *f = &R.frames[R.fi];
    pthread_mutex_lock(&R.lock);
    l->next = f->later;
    f->later = l;
    pthread_mutex_unlock(&R.lock);
}

void vk_later(int kind, void *a, void *b)
{
    VkLater *l = calloc(1, sizeof *l);
    l->kind = kind;
    l->a = a;
    l->b = b;
    later_add(l);
}

void vk_later_alloc(VkAlloc *al)
{
    VkLater *l = calloc(1, sizeof *l);
    l->kind = LATER_ALLOC;
    l->al = *al;
    later_add(l);
}

void vk_later_sub(VkSub *s)
{
    VkLater *l = calloc(1, sizeof *l);
    l->kind = LATER_SUB;
    l->sub = *s;
    later_add(l);
}

static int *free_slots;
static int nfree_slots;

static void later_run(VkFrame *f)
{
    pthread_mutex_lock(&R.lock);
    VkLater *l = f->later;
    f->later = NULL;
    pthread_mutex_unlock(&R.lock);
    while (l) {
        VkLater *n = l->next;
        switch (l->kind) {
        case LATER_IMAGE: vkDestroyImage(R.dev, (VkImage)l->a, NULL); break;
        case LATER_VIEW: vkDestroyImageView(R.dev, (VkImageView)l->a, NULL); break;
        case LATER_BUFFER: vkDestroyBuffer(R.dev, (VkBuffer)l->a, NULL); break;
        case LATER_ALLOC: vkm_free(&l->al); break;
        case LATER_SUB: vkb_sub_free(&l->sub); break;
        case LATER_FREE: free(l->a); break;
        case LATER_PIPELINE: vkDestroyPipeline(R.dev, (VkPipeline)l->a, NULL); break;
        case LATER_TEXID:
            pthread_mutex_lock(&R.lock);
            free_slots[nfree_slots++] = (int)(intptr_t)l->a;
            pthread_mutex_unlock(&R.lock);
            break;
        }
        free(l);
        l = n;
    }
}

/* ------------------------------------------------------------------ frames and submission */
static int frames_init(void)
{
    for (int i = 0; i < VK_FRAMES; i++) {
        VkFrame *f = &R.frames[i];
        VkCommandPoolCreateInfo pi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_TRANSIENT_BIT, R.qfamily};
        CHECK(vkCreateCommandPool(R.dev, &pi, NULL, &f->pool));
        VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, f->pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 16};
        CHECK(vkAllocateCommandBuffers(R.dev, &ai, f->cbs));
        f->ncb = 16;
        VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, NULL, 0};
        CHECK(vkCreateFence(R.dev, &fi, NULL, &f->fence));
        VkSemaphoreCreateInfo si = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, NULL, 0};
        CHECK(vkCreateSemaphore(R.dev, &si, NULL, &f->acquired));
    }
    return 1;
}

VkCommandBuffer vk_cmd(void)
{
    if (R.cb) return R.cb;
    VkFrame *f = &R.frames[R.fi];
    if (f->cb_used >= f->ncb) fatal("vulkan: more than %d submissions in a frame", f->ncb);
    R.cb = f->cbs[f->cb_used++];
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};
    vkBeginCommandBuffer(R.cb, &bi);
    vkCmdBindDescriptorSets(R.cb, VK_PIPELINE_BIND_POINT_GRAPHICS, R.layout, 0, 1, &R.set0, 0, NULL);
    return R.cb;
}

static VkSemaphore submit_wait_sem, submit_signal_sem;

void vk_submit(int wait)
{
    VkFrame *f = &R.frames[R.fi];
    vk_cmd();                                  /* (a submission even when nothing was recorded: the fence) */
    vkEndCommandBuffer(R.cb);
    VkCommandBufferSubmitInfo cbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, NULL, R.cb, 0};
    VkSemaphoreSubmitInfo wsi = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, NULL, submit_wait_sem, 0,
                                 VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0};
    VkSemaphoreSubmitInfo ssi = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, NULL, submit_signal_sem, 0,
                                 VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0};
    VkSubmitInfo2 si = {VK_STRUCTURE_TYPE_SUBMIT_INFO_2, NULL, 0, submit_wait_sem ? 1 : 0, &wsi, 1, &cbi,
                        submit_signal_sem ? 1 : 0, &ssi};
    VkResult r = vkQueueSubmit2(R.queue, 1, &si, f->fence);
    if (r != VK_SUCCESS) fatal("vulkan: submit failed (%d)", r);
    R.cb = NULL;
    R.serial++;
    f->serial = R.serial;
    submit_wait_sem = submit_signal_sem = VK_NULL_HANDLE;
    if (wait) {
        vkWaitForFences(R.dev, 1, &f->fence, VK_TRUE, UINT64_MAX);
        vkResetFences(R.dev, 1, &f->fence);
        R.done = R.serial;
    }
}

/* the frame's last submission has gone (vk_submit without waiting): move to the next frame slot,
   waiting for its previous work */
void vk_frame_end(void)
{
    R.fi = (R.fi + 1) % VK_FRAMES;
    VkFrame *f = &R.frames[R.fi];
    if (f->serial > R.done) {
        vkWaitForFences(R.dev, 1, &f->fence, VK_TRUE, UINT64_MAX);
        R.done = f->serial;
    }
    vkResetFences(R.dev, 1, &f->fence);
    vkResetCommandPool(R.dev, f->pool, 0);
    f->cb_used = 0;
    ring_reset(f);
    later_run(f);
}

/* ------------------------------------------------------------------ barriers */
void vk_barrier_all(VkCommandBuffer cb)
{
    VkMemoryBarrier2 mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, NULL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, NULL, 0, 1, &mb, 0, NULL, 0, NULL};
    vkCmdPipelineBarrier2(cb, &di);
}

void vk_image_layout(VkCommandBuffer cb, VkImage img, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to,
                     uint32_t layers)
{
    VkImageMemoryBarrier2 ib = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, NULL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
        from, to, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, img, {aspect, 0, VK_REMAINING_MIP_LEVELS, 0, layers}};
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, NULL, 0, 0, NULL, 0, NULL, 1, &ib};
    vkCmdPipelineBarrier2(cb, &di);
}

/* ------------------------------------------------------------------ the sets */
static int sets_init(void)
{
    uint32_t ntex = VK_MAX_TEXTURES, nsmp = VK_MAX_SAMPLERS;
    VkDescriptorSetLayoutBinding b0[5];
    VkDescriptorBindingFlags bf[5];
    for (int i = 0; i < 5; i++) {
        b0[i] = (VkDescriptorSetLayoutBinding){(uint32_t)i, i == 4 ? VK_DESCRIPTOR_TYPE_SAMPLER : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                                               i == 4 ? nsmp : ntex, VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
        bf[i] = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
                VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT;
    }
    VkDescriptorSetLayoutBindingFlagsCreateInfo bfi = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO, NULL, 5, bf};
    VkDescriptorSetLayoutCreateInfo li = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, &bfi,
        VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT, 5, b0};
    CHECK(vkCreateDescriptorSetLayout(R.dev, &li, NULL, &R.set0_layout));
    VkDescriptorPoolSize ps0[2] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 4 * ntex}, {VK_DESCRIPTOR_TYPE_SAMPLER, nsmp}};
    VkDescriptorPoolCreateInfo pi0 = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL,
        VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT, 1, 2, ps0};
    CHECK(vkCreateDescriptorPool(R.dev, &pi0, NULL, &R.pool0));
    VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, R.pool0, 1, &R.set0_layout};
    CHECK(vkAllocateDescriptorSets(R.dev, &ai, &R.set0));
    if (R.props.limits.maxDescriptorSetUniformBuffersDynamic < VK_VS_CHUNKS + 1 ||
        R.props.limits.maxPerStageDescriptorUniformBuffers < VK_VS_CHUNKS) {
        LOG(LOG_ALWAYS, "vulkan: the device has too few dynamic uniform buffers (%u)", R.props.limits.maxDescriptorSetUniformBuffersDynamic);
        return 0;
    }
    VkDescriptorSetLayoutBinding b1[2] = {
        {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, VK_VS_CHUNKS, VK_SHADER_STAGE_VERTEX_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL}};
    VkDescriptorSetLayoutCreateInfo li1 = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 2, b1};
    CHECK(vkCreateDescriptorSetLayout(R.dev, &li1, NULL, &R.set1_layout));
    VkDescriptorPoolSize ps1 = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, (VK_VS_CHUNKS + 1) * 1024};
    VkDescriptorPoolCreateInfo pi1 = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 1024, 1, &ps1};
    CHECK(vkCreateDescriptorPool(R.dev, &pi1, NULL, &cpool));
    VkDescriptorSetLayout sl[2] = {R.set0_layout, R.set1_layout};
    VkPushConstantRange pr = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, 4 * VK_MAX_SAMPLER_SLOTS};
    VkPipelineLayoutCreateInfo pli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 2, sl, 1, &pr};
    CHECK(vkCreatePipelineLayout(R.dev, &pli, NULL, &R.layout));
    free_slots = malloc(VK_MAX_TEXTURES * sizeof *free_slots);
    for (int i = VK_MAX_TEXTURES - 1; i >= 1; i--) free_slots[nfree_slots++] = i;
    return 1;
}

int vk_tex_slot(void)
{
    pthread_mutex_lock(&R.lock);
    int s = nfree_slots ? free_slots[--nfree_slots] : 0;
    pthread_mutex_unlock(&R.lock);
    if (!s) LOG(LOG_ALWAYS, "vulkan: out of texture slots");
    return s;
}

void vk_tex_slot_free(int slot)
{
    if (slot > 0) vk_later(LATER_TEXID, (void *)(intptr_t)slot, NULL);
}

/* the views of a texture in its slot (VK_NULL_HANDLE: that kind left as it is); layout: what the
   image is in whenever it is sampled */
static void tex_write(int slot, VkImageView v2d, VkImageView vcube, VkImageView v3d, VkImageView vshadow, VkImageLayout layout)
{
    VkDescriptorImageInfo ii[4] = {{0, v2d, layout}, {0, vcube, layout}, {0, v3d, layout}, {0, vshadow, layout}};
    VkWriteDescriptorSet w[4];
    int n = 0;
    VkImageView v[4] = {v2d, vcube, v3d, vshadow};
    for (int b = 0; b < 4; b++)
        if (v[b])
            w[n++] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, R.set0, (uint32_t)b, (uint32_t)slot, 1,
                                            VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &ii[b], NULL, NULL};
    if (n) vkUpdateDescriptorSets(R.dev, (uint32_t)n, w, 0, NULL);
}

void vk_tex_write(int slot, VkImageView v2d, VkImageView vcube, VkImageView v3d, VkImageView vshadow)
{
    tex_write(slot, v2d, vcube, v3d, vshadow, VK_IMAGE_LAYOUT_GENERAL);
}

/* ------------------------------------------------------------------ samplers */
static VkSamplerKey skeys[VK_MAX_SAMPLERS];

static VkSamplerAddressMode addr_mode(uint8_t a)
{
    switch (a) {
    case D3DTADDRESS_MIRROR: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case D3DTADDRESS_CLAMP: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    case D3DTADDRESS_BORDER: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    case D3DTADDRESS_MIRRORONCE: return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
    default: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    }
}

int vk_sampler(const VkSamplerKey *k)
{
    for (int i = 0; i < R.nsamplers; i++)
        if (!memcmp(&skeys[i], k, sizeof *k)) return i;
    if (R.nsamplers >= VK_MAX_SAMPLERS) return 0;
    VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = k->mag == D3DTEXF_POINT || k->mag == D3DTEXF_NONE ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    si.minFilter = k->min == D3DTEXF_POINT || k->min == D3DTEXF_NONE ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    si.mipmapMode = k->mip == D3DTEXF_LINEAR ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = addr_mode(k->addr[0]);
    si.addressModeV = addr_mode(k->addr[1]);
    si.addressModeW = addr_mode(k->addr[2]);
    si.mipLodBias = k->bias;
    si.anisotropyEnable = k->aniso > 1;
    si.maxAnisotropy = k->aniso > 1 ? (float)k->aniso : 1.0f;
    if (si.maxAnisotropy > R.props.limits.maxSamplerAnisotropy) si.maxAnisotropy = R.props.limits.maxSamplerAnisotropy;
    si.compareEnable = k->compare;
    si.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    si.minLod = k->minlod;
    si.maxLod = k->maxlod;
    si.borderColor = k->border == 0xffffffffu ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE :
                     k->border == 0xff000000u ? VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    VkSampler s;
    if (vkCreateSampler(R.dev, &si, NULL, &s) != VK_SUCCESS) return 0;
    int i = R.nsamplers++;
    skeys[i] = *k;
    R.samplers[i] = s;
    VkDescriptorImageInfo ii = {s, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
    VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, R.set0, VK_B_SAMPLER, (uint32_t)i, 1,
                              VK_DESCRIPTOR_TYPE_SAMPLER, &ii, NULL, NULL};
    vkUpdateDescriptorSets(R.dev, 1, &w, 0, NULL);
    return i;
}

/* ------------------------------------------------------------------ the swapchain */
int vk_swapchain(int w, int h)
{
    if (!R.surface) return 0;
    vkDeviceWaitIdle(R.dev);
    VkSurfaceCapabilitiesKHR caps;
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(R.phys, R.surface, &caps) != VK_SUCCESS) return 0;
    uint32_t nf = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(R.phys, R.surface, &nf, NULL);
    VkSurfaceFormatKHR fmts[64];
    if (nf > 64) nf = 64;
    vkGetPhysicalDeviceSurfaceFormatsKHR(R.phys, R.surface, &nf, fmts);
    VkSurfaceFormatKHR sf = fmts[0];
    for (uint32_t i = 0; i < nf; i++)
        if (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM && fmts[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) { sf = fmts[i]; break; }
    uint32_t nm = 0;
    VkPresentModeKHR modes[16];
    vkGetPhysicalDeviceSurfacePresentModesKHR(R.phys, R.surface, &nm, NULL);
    if (nm > 16) nm = 16;
    vkGetPhysicalDeviceSurfacePresentModesKHR(R.phys, R.surface, &nm, modes);
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    if (!R.vsync || hidden_window) {
        for (uint32_t i = 0; i < nm; i++) if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR) mode = modes[i];
        for (uint32_t i = 0; i < nm; i++) if (modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR) mode = modes[i];
    }
    VkExtent2D ext = caps.currentExtent;
    if (ext.width == 0xffffffffu) ext = (VkExtent2D){(uint32_t)w, (uint32_t)h};
    if (ext.width < caps.minImageExtent.width) ext.width = caps.minImageExtent.width;
    if (ext.height < caps.minImageExtent.height) ext.height = caps.minImageExtent.height;
    if (!ext.width || !ext.height) return 0;
    uint32_t count = caps.minImageCount + 1;
    if (caps.maxImageCount && count > caps.maxImageCount) count = caps.maxImageCount;
    VkSwapchainKHR old = R.swapchain;
    for (uint32_t i = 0; i < R.sc_count; i++)
        if (R.sc_views[i]) vkDestroyImageView(R.dev, R.sc_views[i], NULL), R.sc_views[i] = VK_NULL_HANDLE;
    R.sc_acq = 0;
    R.sc_readable = (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;
    VkSwapchainCreateInfoKHR ci = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, NULL, 0, R.surface, count, sf.format, sf.colorSpace,
        ext, 1, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
        (R.sc_readable ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0), VK_SHARING_MODE_EXCLUSIVE, 0, NULL,
        caps.currentTransform, VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, mode, VK_TRUE, old};
    if (!(caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR))
        ci.compositeAlpha = (VkCompositeAlphaFlagBitsKHR)(caps.supportedCompositeAlpha & -caps.supportedCompositeAlpha);
    VkResult r = vkCreateSwapchainKHR(R.dev, &ci, NULL, &R.swapchain);
    if (old) vkDestroySwapchainKHR(R.dev, old, NULL);
    if (r != VK_SUCCESS) { R.swapchain = VK_NULL_HANDLE; LOG(LOG_ALWAYS, "vulkan: swapchain failed (%d)", r); return 0; }
    for (uint32_t i = 0; i < R.sc_count; i++) vkDestroySemaphore(R.dev, R.sc_ready[i], NULL);
    R.sc_count = 8;
    vkGetSwapchainImagesKHR(R.dev, R.swapchain, &R.sc_count, R.sc_images);
    for (uint32_t i = 0; i < R.sc_count; i++) {
        VkSemaphoreCreateInfo si = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, NULL, 0};
        vkCreateSemaphore(R.dev, &si, NULL, &R.sc_ready[i]);
        VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, R.sc_images[i], VK_IMAGE_VIEW_TYPE_2D,
            sf.format, {0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        if (vkCreateImageView(R.dev, &vi, NULL, &R.sc_views[i]) != VK_SUCCESS) R.sc_views[i] = VK_NULL_HANDLE;
    }
    R.sc_w = ext.width;
    R.sc_h = ext.height;
    R.sc_format = sf.format;
    R.sc_mode = mode;
    R.sc_broken = 0;
    LOG(LOG_ALWAYS, "vulkan: swapchain %ux%u, %u images, format %d, present mode %d", ext.width, ext.height, R.sc_count, sf.format, mode);
    return 1;
}

/* An image of the swapchain, waiting at most ACQUIRE_NS: with vsync (FIFO) NVIDIA's Wayland driver
   never returned from an endless wait after the window went full screen and back, the game frozen
   [verified: T7 job 20261028-033217, tests/vk_player.sh's shown run: the game thread in
   vkAcquireNextImageKHR]; no image in time: that frame is not presented and the swapchain is made
   again (a frame is 4-33 ms) */
#define ACQUIRE_NS 250000000ull
static int acquire(VkSemaphore sem, uint32_t *idx)
{
    VkResult r = vkAcquireNextImageKHR(R.dev, R.swapchain, ACQUIRE_NS, sem, VK_NULL_HANDLE, idx);
    if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) return 1;
    static int said;
    if ((r == VK_TIMEOUT || r == VK_NOT_READY) && !said) {
        said = 1;
        LOG(LOG_ALWAYS, "vulkan: no swapchain image in %llu ms: the frame not presented, the swapchain made again",
            ACQUIRE_NS / 1000000);
    }
    R.sc_broken = 1;
    return 0;
}

/* THE BACK BUFFER STRAIGHT INTO THE SWAPCHAIN IMAGE (round 3, VULKAN.md 2.7). The back buffer's
   passes (the game's copy of its scene to the screen, the HUD-less screens, the menus and movies)
   draw into the frame's swapchain image instead of the back buffer when they are the same size and
   format, and Present presents it: no blit of the whole screen a frame. The image is acquired at the
   frame's first such pass; the work recorded before it goes in a submission of its own first, so
   only what draws into the image waits for it. The game's swap effect is DISCARD (the back buffer's
   contents after a Present undefined, as Direct3D 9 has them), so a frame's first pass into it
   starts from nothing (DONT_CARE) unless it clears. A shot reads the image (vk_d3d9.c). */
static void submit_part(void)
{
    if (!R.cb) return;
    vkEndCommandBuffer(R.cb);
    VkCommandBufferSubmitInfo cbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, NULL, R.cb, 0};
    VkSubmitInfo2 si = {VK_STRUCTURE_TYPE_SUBMIT_INFO_2, NULL, 0, 0, NULL, 1, &cbi, 0, NULL};
    VkResult r = vkQueueSubmit2(R.queue, 1, &si, VK_NULL_HANDLE);
    if (r != VK_SUCCESS) fatal("vulkan: submit failed (%d)", r);
    R.cb = NULL;                        /* (the serial stays: the frame's last submission's fence covers it) */
}

int vk_sc_target(uint32_t bw, uint32_t bh, VkFormat fmt, VkImage *img, VkImageView *view)
{
    static int on = -1;
    if (on < 0) on = !(getenv("ETM_VK_SC_DIRECT") && *getenv("ETM_VK_SC_DIRECT") == '0');
    if (!R.sc_acq) {
        if (!on || !R.swapchain || R.sc_broken || bw != R.sc_w || bh != R.sc_h || fmt != R.sc_format)
            return 0;
        VkFrame *f = &R.frames[R.fi];
        submit_part();
        uint32_t idx = 0;
        if (!acquire(f->acquired, &idx)) return 0;
        if (!R.sc_views[idx]) return 0;
        R.sc_idx = (int)idx;
        R.sc_acq = 1;
        static int said;
        if (!said) {
            said = 1;
            LOG(LOG_ALWAYS, "vulkan: the back buffer's passes drawn straight into the swapchain image");
        }
        submit_wait_sem = f->acquired;
        VkCommandBuffer cb = vk_cmd();
        VkImageMemoryBarrier2 ib = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, NULL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            0, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_TRANSFER_READ_BIT,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
            R.sc_images[idx], {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, NULL, 0, 0, NULL, 0, NULL, 1, &ib};
        vkCmdPipelineBarrier2(cb, &di);
    }
    *img = R.sc_images[R.sc_idx];
    *view = R.sc_views[R.sc_idx];
    return 1;
}

/* the frame's last submission with the back buffer copied to the screen (or drawn into it already:
   bb VK_NULL_HANDLE) and presented */
void vk_present(VkImage bb, uint32_t bw, uint32_t bh)
{
    VkFrame *f = &R.frames[R.fi];
    uint32_t idx = 0;
    int have = 0;
    if (R.sc_acq) {
        idx = (uint32_t)R.sc_idx;
        R.sc_acq = 0;
        VkCommandBuffer cb = vk_cmd();
        if (bb) {                       /* (drawn into the back buffer after all: the blit below) */
            vk_image_layout(cb, R.sc_images[idx], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1);
        } else {
            VkImageMemoryBarrier2 ib = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, NULL,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_NONE, 0,
                VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
                R.sc_images[idx], {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
            VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, NULL, 0, 0, NULL, 0, NULL, 1, &ib};
            vkCmdPipelineBarrier2(cb, &di);
            submit_signal_sem = R.sc_ready[idx];
            vk_submit(0);
            VkPresentInfoKHR pi = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, NULL, 1, &R.sc_ready[idx], 1, &R.swapchain, &idx, NULL};
            VkResult r = vkQueuePresentKHR(R.queue, &pi);
            if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) R.sc_broken = 1;
            goto end;
        }
        have = 2;
    } else if (R.swapchain && !R.sc_broken && bb) {
        have = acquire(f->acquired, &idx);
    }
    VkCommandBuffer cb = vk_cmd();
    if (have) {
        VkImage sc = R.sc_images[idx];
        if (have == 1)
            vk_image_layout(cb, sc, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1);
        VkImageBlit bl = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{0, 0, 0}, {(int32_t)bw, (int32_t)bh, 1}},
                          {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{0, 0, 0}, {(int32_t)R.sc_w, (int32_t)R.sc_h, 1}}};
        vkCmdBlitImage(cb, bb, VK_IMAGE_LAYOUT_GENERAL, sc, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl,
                       bw == R.sc_w && bh == R.sc_h ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
        vk_image_layout(cb, sc, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, 1);
        if (have == 1) submit_wait_sem = f->acquired;
        submit_signal_sem = R.sc_ready[idx];
    }
    vk_submit(0);
    if (have) {
        VkPresentInfoKHR pi = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, NULL, 1, &R.sc_ready[idx], 1, &R.swapchain, &idx, NULL};
        VkResult r = vkQueuePresentKHR(R.queue, &pi);
        if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) R.sc_broken = 1;
    }
end:
    vk_frame_end();
    if (R.sc_broken && R.surface) {
        int w = 0, h = 0;
        SDL_GetWindowSizeInPixels(host_sdl_window(), &w, &h);
        vk_swapchain(w, h);
    }
}

/* ------------------------------------------------------------------ the pipeline cache on disk
   The driver's pipeline cache data in the user folder (vk_pipeline_cache.bin), given back at the next
   start when it is this device's and driver's (its header: VkPipelineCacheHeaderVersionOne); written
   by vk_d3d9.c's saves (a thread of their own) through a temporary file. With vk_d3d9.c's list of the
   pipelines made (vk_pipelines.bin) the pipelines are made again at the start from it. */
static void cache_path(char *p, size_t n) { snprintf(p, n, "%s/vk_pipeline_cache.bin", user_dir); }

static void pipeline_cache_load(void)
{
    char path[1200];
    cache_path(path, sizeof path);
    void *data = NULL;
    size_t len = 0;
    FILE *f = fopen(path, "rb");
    if (f) {
        if (fseek(f, 0, SEEK_END) == 0) {
            long n = ftell(f);
            if (n >= 32 && n < (256l << 20) && fseek(f, 0, SEEK_SET) == 0 && (data = malloc((size_t)n)) &&
                fread(data, 1, (size_t)n, f) == (size_t)n)
                len = (size_t)n;
        }
        fclose(f);
    }
    if (len) {
        const uint8_t *h = data;
        uint32_t hsize, ver, vendor, device;
        memcpy(&hsize, h, 4), memcpy(&ver, h + 4, 4), memcpy(&vendor, h + 8, 4), memcpy(&device, h + 12, 4);
        if (hsize < 32 || ver != VK_PIPELINE_CACHE_HEADER_VERSION_ONE || vendor != R.props.vendorID ||
            device != R.props.deviceID || memcmp(h + 16, R.props.pipelineCacheUUID, VK_UUID_SIZE)) {
            LOG(LOG_ALWAYS, "vulkan: the pipeline cache on disk is another device's or driver's: not used");
            len = 0;
        }
    }
    VkPipelineCacheCreateInfo pc = {VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO, NULL, 0, len, len ? data : NULL};
    if (vkCreatePipelineCache(R.dev, &pc, NULL, &R.pcache) != VK_SUCCESS && len) {
        pc.initialDataSize = 0, pc.pInitialData = NULL;
        vkCreatePipelineCache(R.dev, &pc, NULL, &R.pcache);
        len = 0;
    }
    if (len) LOG(LOG_ALWAYS, "vulkan: pipeline cache %zu KB from the last runs", len >> 10);
    free(data);
}

void vk_pipeline_cache_save(void)
{
    size_t len = 0;
    if (vkGetPipelineCacheData(R.dev, R.pcache, &len, NULL) != VK_SUCCESS || !len) return;
    void *data = malloc(len);
    if (!data || vkGetPipelineCacheData(R.dev, R.pcache, &len, data) != VK_SUCCESS) { free(data); return; }
    char path[1200], tmp[1300];
    cache_path(path, sizeof path);
    snprintf(tmp, sizeof tmp, "%s.new", path);
    FILE *f = fopen(tmp, "wb");
    if (f) {
        int ok = fwrite(data, 1, len, f) == len;
        if (fclose(f) == 0 && ok) rename(tmp, path);
        else remove(tmp);
    }
    free(data);
}

/* ------------------------------------------------------------------ start and end */
int vk_init(void)
{
    if (R.on) return 1;
    pthread_mutex_init(&R.lock, NULL);
    if (!make_instance() || !pick_device() || !make_device() || !sets_init() || !frames_init())
        return 0;
    pipeline_cache_load();
    if (host_sdl_window()) {
        if (!SDL_Vulkan_CreateSurface(host_sdl_window(), R.instance, NULL, &R.surface)) {
            LOG(LOG_ALWAYS, "vulkan: no surface (%s): nothing is presented", SDL_GetError());
            R.surface = VK_NULL_HANDLE;
        } else {
            VkBool32 ok = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(R.phys, R.qfamily, R.surface, &ok);
            if (!ok) LOG(LOG_ALWAYS, "vulkan: the queue cannot present to the window");
        }
    }
    {   /* timestamps: one pool, a slot per Direct3D query (vk_d3d9.c) */
        uint32_t nq = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(R.phys, &nq, NULL);
        VkQueueFamilyProperties qp[16];
        if (nq > 16) nq = 16;
        vkGetPhysicalDeviceQueueFamilyProperties(R.phys, &nq, qp);
        if (R.qfamily < nq && !qp[R.qfamily].timestampValidBits) R.ttimes = 0;
        VkQueryPoolCreateInfo qi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, NULL, 0, VK_QUERY_TYPE_TIMESTAMP, 4096, 0};
        if (R.ttimes && vkCreateQueryPool(R.dev, &qi, NULL, &R.tpool) != VK_SUCCESS) R.ttimes = 0;
        if (R.ttimes) vkResetQueryPool(R.dev, R.tpool, 0, 4096);
    }
    if (!vkb_sub(256, &R.zero)) return 0;
    memset(R.zero.map, 0, 256);
    R.on = 1;
    return 1;
}

void vk_shutdown(void)
{
    if (!R.on) return;
    vkDeviceWaitIdle(R.dev);
}
