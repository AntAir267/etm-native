/* OUR OWN VULKAN RENDERER (option renderer = vulkan; VULKAN.md). The Direct3D 9 the game uses,
   implemented in C as host COM objects with Direct3D's vtables, so everything that called DXVK's
   objects (the generated wrappers, d3d9.c, d3d9_msaa.c, the shots) calls ours unchanged. vk_core.c:
   Vulkan itself (instance, device, memory, frames, uploads, pipelines, samplers, the bindless set,
   the swapchain); vk_d3d9.c: the Direct3D objects and the device's state, draws and passes, with
   Direct3D's fixed function (runtime/shaders/ff.vert, ff.frag). From larush-native (2026-10-29). */
#pragma once
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
/* (DXVK's headers in C give vtables without the object pointer: put it back, so each method here
   is checked against Direct3D's own declaration, IDirect3DDevice9Vtbl and the others) */
#include <windows.h>
#undef THIS_
#define THIS_ void *This,
#undef THIS
#define THIS void *This
#include <d3d9.h>
#undef LOG
#include <pthread.h>
#include <stdint.h>
#include "runtime.h"
#include "vk_abi.h"

/* ------------------------------------------------------------------ Vulkan's functions */
#define VK_GLOBAL_FNS(X) X(CreateInstance) X(EnumerateInstanceExtensionProperties) X(EnumerateInstanceLayerProperties)
#define VK_INSTANCE_FNS(X) X(DestroyInstance) X(EnumeratePhysicalDevices) X(GetPhysicalDeviceProperties) \
    X(GetPhysicalDeviceProperties2) X(GetPhysicalDeviceFeatures2) X(GetPhysicalDeviceQueueFamilyProperties) \
    X(GetPhysicalDeviceMemoryProperties) X(GetPhysicalDeviceFormatProperties) X(CreateDevice) X(GetDeviceProcAddr) \
    X(EnumerateDeviceExtensionProperties) X(GetPhysicalDeviceSurfaceSupportKHR) X(GetPhysicalDeviceSurfaceCapabilitiesKHR) \
    X(GetPhysicalDeviceSurfaceFormatsKHR) X(GetPhysicalDeviceSurfacePresentModesKHR) X(DestroySurfaceKHR) \
    X(GetPhysicalDeviceImageFormatProperties)
#define VK_DEVICE_FNS(X) X(DestroyDevice) X(GetDeviceQueue) X(DeviceWaitIdle) X(QueueSubmit2) X(QueueWaitIdle) \
    X(CreateCommandPool) X(DestroyCommandPool) X(AllocateCommandBuffers) X(ResetCommandPool) X(BeginCommandBuffer) \
    X(EndCommandBuffer) X(CreateFence) X(DestroyFence) X(WaitForFences) X(ResetFences) X(GetFenceStatus) \
    X(CreateSemaphore) X(DestroySemaphore) X(AllocateMemory) X(FreeMemory) X(MapMemory) X(UnmapMemory) \
    X(CreateBuffer) X(DestroyBuffer) X(GetBufferMemoryRequirements) X(BindBufferMemory) X(CreateImage) \
    X(DestroyImage) X(GetImageMemoryRequirements) X(BindImageMemory) X(CreateImageView) X(DestroyImageView) \
    X(CreateSampler) X(DestroySampler) X(CreateShaderModule) X(DestroyShaderModule) X(CreatePipelineLayout) \
    X(DestroyPipelineLayout) X(CreateDescriptorSetLayout) X(DestroyDescriptorSetLayout) X(CreateDescriptorPool) \
    X(DestroyDescriptorPool) X(AllocateDescriptorSets) X(UpdateDescriptorSets) X(CreateGraphicsPipelines) \
    X(DestroyPipeline) X(CreatePipelineCache) X(DestroyPipelineCache) X(GetPipelineCacheData) \
    X(CmdBeginRendering) X(CmdEndRendering) X(CmdBindPipeline) X(CmdBindDescriptorSets) X(CmdPushConstants) \
    X(CmdBindVertexBuffers2) X(CmdBindIndexBuffer) X(CmdDraw) X(CmdDrawIndexed) X(CmdSetViewport) X(CmdSetScissor) \
    X(CmdSetCullMode) X(CmdSetFrontFace) X(CmdSetPrimitiveTopology) X(CmdSetDepthTestEnable) \
    X(CmdSetDepthWriteEnable) X(CmdSetDepthCompareOp) X(CmdSetDepthBiasEnable) X(CmdSetDepthBias) \
    X(CmdSetStencilTestEnable) X(CmdSetStencilOp) X(CmdSetStencilCompareMask) X(CmdSetStencilWriteMask) \
    X(CmdSetStencilReference) X(CmdSetBlendConstants) X(CmdPipelineBarrier2) X(CmdCopyBufferToImage) \
    X(CmdCopyImageToBuffer) X(CmdCopyImage) X(CmdBlitImage) X(CmdResolveImage) X(CmdClearAttachments) \
    X(CmdClearColorImage) X(CmdClearDepthStencilImage) X(CmdCopyBuffer) X(CreateSwapchainKHR) \
    X(DestroySwapchainKHR) X(GetSwapchainImagesKHR) X(AcquireNextImageKHR) X(QueuePresentKHR) \
    X(CreateQueryPool) X(DestroyQueryPool) X(ResetQueryPool) X(GetQueryPoolResults) X(CmdWriteTimestamp2)
#define VK_DECL_FN(n) extern PFN_vk##n vk##n;
VK_GLOBAL_FNS(VK_DECL_FN)
VK_INSTANCE_FNS(VK_DECL_FN)
VK_DEVICE_FNS(VK_DECL_FN)
#undef VK_DECL_FN

/* ------------------------------------------------------------------ memory */
typedef struct VkBlock VkBlock;
typedef struct {
    VkBlock *blk;
    VkDeviceMemory mem;
    VkDeviceSize off, size;
    uint8_t *map;               /* (host-visible memory: mapped) */
} VkAlloc;

int vkm_alloc(const VkMemoryRequirements *req, VkMemoryPropertyFlags want, VkMemoryPropertyFlags avoid, VkAlloc *out);
void vkm_free(VkAlloc *a);

/* a buffer for vertices or indices: a range of one of the arenas' VkBuffers (VKB_ARENA bytes each) */
typedef struct {
    VkBuffer buf;
    VkDeviceSize off, size;
    uint8_t *map;
    void *arena;
} VkSub;
int vkb_sub(VkDeviceSize size, VkSub *out);
void vkb_sub_free(VkSub *s);    /* (once the GPU is done with it: vk_later) */

/* the frame's ring: constants, drawing from memory, staging; valid until the frame's work is done */
typedef struct { VkBuffer buf; VkDeviceSize off; uint8_t *map; VkDescriptorSet cset; } VkRing;
int vk_ring(VkDeviceSize size, VkDeviceSize align, VkRing *out);

/* ------------------------------------------------------------------ the renderer's own state */
#define VK_FRAMES 2

typedef struct {
    VkCommandPool pool;
    VkCommandBuffer cbs[16];    /* (a frame may submit more than once: GetRenderTargetData waits) */
    int ncb, cb_used;
    VkFence fence;
    uint64_t serial;            /* its last submission's */
    VkSemaphore acquired;
    /* the ring (and overflow chunks, dropped when the frame comes round again) */
    struct VkChunk *chunks;
    struct VkLater *later;      /* to destroy once its work is done */
} VkFrame;

typedef struct VkLater { struct VkLater *next; int kind; void *a, *b; VkAlloc al; VkSub sub; } VkLater;
enum { LATER_IMAGE, LATER_VIEW, LATER_BUFFER, LATER_ALLOC, LATER_SUB, LATER_FREE, LATER_PIPELINE, LATER_TEXID };

typedef struct {
    int on;
    VkInstance instance;
    VkPhysicalDevice phys;
    VkDevice dev;
    VkQueue queue;
    uint32_t qfamily;
    VkPhysicalDeviceProperties props;
    VkPhysicalDeviceMemoryProperties memprops;
    uint8_t uuid[16];
    uint32_t driver_id;
    int validation;
    int fcon, fc2;              /* the shaders' float controls as DXVK uses them (vk_d3d9.c module_of): RTE and
                                   flush-to-zero for 32-bit floats; float_controls2's default fast-math mode */
    /* frames */
    VkFrame frames[VK_FRAMES];
    int fi;                     /* the frame being recorded */
    VkCommandBuffer cb;         /* its command buffer being recorded (NULL: none begun) */
    uint64_t serial;            /* submissions so far */
    uint64_t done;              /* the last one known finished */
    pthread_mutex_t lock;       /* allocators, the texture slots, later lists */
    /* the swapchain */
    VkSurfaceKHR surface;
    VkSwapchainKHR swapchain;
    VkImage sc_images[8];
    VkSemaphore sc_ready[8];    /* per image: its drawing done, for the present */
    uint32_t sc_count, sc_w, sc_h;
    VkFormat sc_format;
    VkPresentModeKHR sc_mode;
    int sc_broken;
    VkImageView sc_views[8];    /* (the back buffer's passes drawn straight into the image: vk_sc_target) */
    int sc_readable;            /* TRANSFER_SRC (shots read the frame from it) */
    int sc_idx, sc_acq;         /* the image acquired for the frame being recorded (sc_acq) */
    int vsync;
    /* binding */
    VkDescriptorSetLayout set0_layout, set1_layout;
    VkDescriptorPool pool0;
    VkDescriptorSet set0;
    VkPipelineLayout layout;
    VkPipelineCache pcache;
    VkSampler samplers[VK_MAX_SAMPLERS];
    int nsamplers;
    /* a zero buffer for vertex inputs the declaration does not give */
    VkSub zero;
    /* VK_EXT_sample_locations (Direct3D's MULTISAMPLEANTIALIAS = FALSE: every sample at the centre) */
    int sample_locations;
    VkSampleCountFlags sample_location_counts;
    int variable_sample_locations;
    /* timestamps (Direct3D's TIMESTAMP queries: d3d9_gputime.c, the passes' timers) */
    VkQueryPool tpool;
    int ttimes;                 /* the queue writes timestamps (and the host can reset them) */
} VkR;
extern VkR R;

int vk_init(void);              /* instance, device, queue, frames, sets (1 ok) */
void vk_shutdown(void);
VkCommandBuffer vk_cmd(void);   /* the command buffer being recorded (begun on first use) */
void vk_submit(int wait);       /* end and submit it (wait: until the GPU is done) */
/* the frame's swapchain image for the back buffer's passes (bw x bh, format fmt): acquired now (the
   work so far submitted first, so it does not wait for the image), 0 when it cannot be (size,
   format, none, ETM_VK_SC_DIRECT=0) */
int vk_sc_target(uint32_t bw, uint32_t bh, VkFormat fmt, VkImage *img, VkImageView *view);
void vk_frame_end(void);        /* the frame's last submit done: the next frame slot */
void vk_later(int kind, void *a, void *b);      /* destroy once the work recorded so far is done */
void vk_later_alloc(VkAlloc *al);
void vk_later_sub(VkSub *s);
int vk_swapchain(int w, int h); /* (re)make it at that size */
void vk_pipeline_cache_save(void);  /* the driver's pipeline cache data into the user folder (any thread) */

/* bindless texture slots */
int vk_tex_slot(void);
void vk_tex_write(int slot, VkImageView v2d, VkImageView vcube, VkImageView v3d, VkImageView vshadow);
void vk_tex_slot_free(int slot);

/* samplers: Direct3D's states to a slot in the bindless sampler array */
typedef struct {
    uint8_t addr[3], mag, min, mip, aniso, compare;
    uint32_t border;
    float bias, minlod, maxlod;
} VkSamplerKey;
int vk_sampler(const VkSamplerKey *k);

/* image helpers */
void vk_barrier_all(VkCommandBuffer cb);                   /* every earlier access before every later one */
void vk_image_layout(VkCommandBuffer cb, VkImage img, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to,
                     uint32_t layers);

/* ------------------------------------------------------------------ Direct3D objects */
/* the vtables are Direct3D's (host ABI: plain C calls, `this` first); refs and private data shared */
typedef struct VkPriv { struct VkPriv *next; GUID guid; void *data; DWORD size; int iunk; } VkPriv;
typedef struct {
    void **vtbl;
    volatile long refs;         /* the program's (and the other objects' private refs, see vk_d3d9.c) */
    volatile long irefs;        /* the device's own (bound), so a released bound object stays alive */
    int kind;                   /* DK_* (gen/d3d9_gen.h) */
    int zombie;                 /* released while a state slot holds it (vk_d3d9.c "bound objects") */
    VkPriv *priv;
} VkObj;

typedef struct VkTex VkTex;
typedef struct VkSurf {
    VkObj o;
    VkTex *tex;                 /* the image's owner (a texture's level, or the surface's own) */
    uint32_t level, face;
    VkImageView view;           /* as an attachment (this level and face) */
    uint8_t *shadow;            /* (locks: the bytes the program sees) */
    int locked, lock_flags;
    int standalone;             /* a render target, depth buffer, offscreen surface, the back buffer */
} VkSurf;

struct VkTex {
    VkObj o;
    VkImage image;
    VkAlloc mem;
    VkFormat vkfmt;
    D3DFORMAT fmt;
    D3DRESOURCETYPE type;       /* TEXTURE, CUBETEXTURE, or SURFACE for a standalone surface */
    DWORD usage;
    D3DPOOL pool;
    uint32_t w, h, levels, faces, samples;
    VkImageView view;           /* sampled (all levels, faces) */
    VkImageView view_depth;     /* depth formats: the compare view in texShadow (and view in tex2D) */
    int slot;                   /* bindless */
    int kind;                   /* 0 2D, 1 cube, 2 3D; 3 when sampling compares (a shadow depth format) */
    int depth, stencil;         /* its aspects */
    int has_alpha;              /* (X8R8G8B8: alpha reads 1) */
    VkSurf **surfs;             /* levels x faces */
    uint64_t used;              /* the last submission reading or writing it */
    uint8_t *sysmem;            /* SYSTEMMEM / SCRATCH: no image, the bytes only */
};

typedef struct {
    VkObj o;
    VkSub sub;
    uint8_t *shadow;            /* the bytes as the program last wrote them (host memory: our own
                                   until d3d9.c hands us the program's copy, vk_buf_host) */
    int own_shadow;
    uint32_t size;
    DWORD usage, fvf;
    D3DPOOL pool;
    D3DFORMAT fmt;              /* index buffers: INDEX16 / INDEX32 */
    uint64_t used;              /* the last submission reading it */
    uint32_t used_lo, used_hi;  /* what draws in the command buffer being recorded read */
    int lock_depth;
    uint32_t lock_off[4], lock_len[4], lock_flags[4];
} VkBuf;

typedef struct {
    VkObj o;
    D3DVERTEXELEMENT9 el[32];
    int nel;
    uint32_t id;
    uint64_t hash;              /* its elements' (pipeline keys) */
    uint32_t ffflags;           /* fixed function (vk_d3d9.c decl_classify, DXVK's Classify): DECL_* */
    uint32_t tcmask;            /* 3 bits a texture coordinate set: its components */
    uint32_t fvf;               /* made for SetFVF: the FVF (0: CreateVertexDeclaration's) */
} VkDecl;
enum { DECL_POSITIONT = 1, DECL_COLOR0 = 2, DECL_COLOR1 = 4, DECL_PSIZE = 8, DECL_FOG = 16, DECL_BLENDWEIGHT = 32,
       DECL_BLENDINDICES = 64 };

typedef struct {
    VkObj o;
    const VkShaderMeta *meta;   /* NULL: not in the table (draws with it are skipped) */
    VkShaderModule module;      /* (shared by every object of the same bytecode: vk_d3d9.c module_of) */
    uint64_t hash;
    uint32_t id;
    uint32_t defmask;           /* the vertex constant chunks its def'd constants fall in */
} VkShader;

/* vk_d3d9.c */
void *vk_create9(void);         /* IDirect3D9 */
const char *vk_format_name(D3DFORMAT f);
