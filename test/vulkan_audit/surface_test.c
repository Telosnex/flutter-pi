// SPDX-License-Identifier: MIT
// Real Vulkan surface + compositor, fake driver/KMS edges. No GPU is touched.
#include <fcntl.h>

#include <unity.h>

static int test_fence_fcntl(int fd, int command, int value);
#define fcntl test_fence_fcntl
#include "../../src/vk_gbm_render_surface.c"
#undef fcntl
#include "compositor_ng.h"
#include "frame_scheduler.h"
#include "window.h"
#define HANDLE(type, n) ((type) (uintptr_t) (n))

static struct vk_gbm_render_surface *s;
static struct tracer *tracer;
static FlutterBackingStore stores[6];
static struct surface *repaint_snapshot;
static VkImage simulated_busy_image;
static int images_destroyed_while_busy;
static bool external_owner, import_live;
static struct fb imported;
static int deinit_calls, renderer_refs, fake_renderer, fake_drm, fake_builder;
static int bo_calls, bo_created, bo_destroyed, images_created, images_destroyed, mem_created, mem_freed;
static int fail_step, fail_bo_number, last_import_fd;
static int bo_planes, invalid_image_creates;
static uint64_t bo_modifier;
// Modifiers the fake Vulkan device can import. Empty means "only bo_modifier",
// which keeps the older tests' single-modifier device.
static uint64_t vk_modifiers[8];
static int n_vk_modifiers;
// Last list passed to gbm_bo_create_with_modifiers and what was given to Vulkan/KMS.
static uint64_t gbm_requested_modifiers[8];
static int n_gbm_requested_modifiers, gbm_modifier_calls;
static bool gbm_ignores_modifier_list;
static uint64_t image_modifier, addfb_modifier;
static VkDeviceSize layout_size, required_size, allocation_size, explicit_size;
static VkResult idle_result, export_result;
static bool export_supported, export_complete, semaphore_signaled;
static int idle_calls, submit_calls, invalid_signals, pushed, push_error, last_fence;
static bool reject_fence_only;
static bool fail_fence_wait, fail_fence_dup;
static int interrupted_fence_waits;
static short fence_revents;
static int fence_wait_calls, last_push_fence;
static int capability_failure, image_capability_queries;
static struct {
    kms_fb_release_cb_t cb;
    void *data;
    uint32_t fb;
    int fd;
} layers[16];
static struct {
    void *data;
    void (*destroy)(struct gbm_bo *, void *);
} bos[128];
static struct compositor *compositor;
static struct window *window;
static struct frame_scheduler *scheduler;
static struct fl_layer_composition *composition;
enum { FAIL_BO = 1, FAIL_IMAGE, FAIL_OFFSET, FAIL_PITCH, FAIL_FD, FAIL_PROC, FAIL_MEM_PROPS, FAIL_MEM_TYPE, FAIL_ALLOC, FAIL_BIND };

static int test_fence_fcntl(int fd, int command, int value) {
    if (fail_fence_dup) {
        errno = EMFILE;
        return -1;
    }
    return fcntl(fd, command, value);
}
struct vk_renderer *__wrap_vk_renderer_ref(struct vk_renderer *r) {
    renderer_refs++;
    return r;
}
void __wrap_vk_renderer_unref(struct vk_renderer *r) {
    (void) r;
    renderer_refs--;
}
VkPhysicalDevice __wrap_vk_renderer_get_physical_device(struct vk_renderer *r) {
    (void) r;
    return HANDLE(VkPhysicalDevice, 3);
}
void vkGetPhysicalDeviceFormatProperties2(VkPhysicalDevice physical, VkFormat format, VkFormatProperties2 *props) {
    (void) physical;
    (void) format;
    VkDrmFormatModifierPropertiesListEXT *list = props->pNext;
    if (capability_failure == 1) {
        list->drmFormatModifierCount = 0;
        return;
    }
    if (n_vk_modifiers > 0) {
        if (list->pDrmFormatModifierProperties) {
            for (int i = 0; i < n_vk_modifiers; i++) {
                list->pDrmFormatModifierProperties[i] = (VkDrmFormatModifierPropertiesEXT) {
                    .drmFormatModifier = vk_modifiers[i],
                    .drmFormatModifierPlaneCount = 1,
                    .drmFormatModifierTilingFeatures = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
                                                       VK_FORMAT_FEATURE_TRANSFER_DST_BIT,
                };
            }
        }
        list->drmFormatModifierCount = n_vk_modifiers;
        return;
    }
    if (list->pDrmFormatModifierProperties) {
        list->pDrmFormatModifierProperties[0] = (VkDrmFormatModifierPropertiesEXT) {
            .drmFormatModifier = capability_failure == 2 ? I915_FORMAT_MOD_Y_TILED : bo_modifier,
            .drmFormatModifierPlaneCount = capability_failure == 3 ? 2 : 1,
            .drmFormatModifierTilingFeatures = capability_failure == 4 ?
                                                   VK_FORMAT_FEATURE_TRANSFER_SRC_BIT :
                                                   VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
                                                       VK_FORMAT_FEATURE_TRANSFER_DST_BIT,
        };
    }
    list->drmFormatModifierCount = 1;
}
VkResult vkGetPhysicalDeviceImageFormatProperties2(
    VkPhysicalDevice physical,
    const VkPhysicalDeviceImageFormatInfo2 *info,
    VkImageFormatProperties2 *props
) {
    (void) physical;
    (void) info;
    image_capability_queries++;
    if (capability_failure == 5)
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    if (capability_failure == 6)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    props->imageFormatProperties = (VkImageFormatProperties) {
        .maxExtent = { capability_failure == 7 ? 32 : 4096, 4096, 1 },
        .maxMipLevels = 1,
        .maxArrayLayers = 1,
        .sampleCounts = VK_SAMPLE_COUNT_1_BIT,
        .maxResourceSize = 1ull << 30,
    };
    VkExternalImageFormatProperties *external = props->pNext;
    external->externalMemoryProperties = (VkExternalMemoryProperties) {
        .externalMemoryFeatures = capability_failure == 8 ? 0 : VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT,
        .compatibleHandleTypes = capability_failure == 9 ? 0 : VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };
    return VK_SUCCESS;
}
int __wrap_poll(struct pollfd *fds, nfds_t count, int timeout) {
    (void) count;
    (void) timeout;
    fence_wait_calls++;
    if (interrupted_fence_waits > 0) {
        interrupted_fence_waits--;
        errno = EINTR;
        return -1;
    }
    if (fail_fence_wait) {
        errno = EIO;
        return -1;
    }
    fds[0].revents = fence_revents;
    return 1;
}
VkDevice __wrap_vk_renderer_get_device(struct vk_renderer *r) {
    (void) r;
    return HANDLE(VkDevice, 1);
}
VkQueue __wrap_vk_renderer_get_queue(struct vk_renderer *r) {
    (void) r;
    return HANDLE(VkQueue, 2);
}
bool __wrap_vk_renderer_supports_sync_fd_semaphore_export(struct vk_renderer *r) {
    (void) r;
    return export_supported;
}
int __wrap_vk_renderer_find_mem_type(struct vk_renderer *r, VkMemoryPropertyFlags flags, uint32_t bits) {
    (void) r;
    (void) flags;
    (void) bits;
    return fail_step == FAIL_MEM_TYPE ? -1 : 0;
}
struct gbm_bo *gbm_bo_create(struct gbm_device *d, uint32_t w, uint32_t h, uint32_t fmt, uint32_t flags) {
    (void) d;
    (void) w;
    (void) h;
    (void) fmt;
    (void) flags;
    bo_calls++;
    if (fail_step == FAIL_BO || bo_calls == fail_bo_number) {
        errno = ENOMEM;
        return NULL;
    }
    return (void *) &bos[bo_created++];
}
void gbm_bo_destroy(struct gbm_bo *bo) {
    size_t i = ((char *) bo - (char *) bos) / sizeof(bos[0]);
    if (bos[i].destroy)
        bos[i].destroy(bo, bos[i].data);
    bo_destroyed++;
}
struct gbm_bo *gbm_bo_create_with_modifiers(
    struct gbm_device *d,
    uint32_t w,
    uint32_t h,
    uint32_t fmt,
    const uint64_t *modifiers,
    const unsigned int count
) {
    (void) d;
    (void) w;
    (void) h;
    (void) fmt;
    gbm_modifier_calls++;
    n_gbm_requested_modifiers = (int) count;
    for (unsigned int i = 0; i < count && i < ARRAY_SIZE(gbm_requested_modifiers); i++)
        gbm_requested_modifiers[i] = modifiers[i];
    bo_calls++;
    if (fail_step == FAIL_BO || bo_calls == fail_bo_number) {
        errno = ENOMEM;
        return NULL;
    }
    // Like a driver: pick one of the offered modifiers (or misbehave on request).
    bo_modifier = gbm_ignores_modifier_list ? I915_FORMAT_MOD_Y_TILED : modifiers[count - 1];
    return (void *) &bos[bo_created++];
}
uint32_t gbm_bo_get_width(struct gbm_bo *bo) {
    (void) bo;
    return 64;
}
uint32_t gbm_bo_get_height(struct gbm_bo *bo) {
    (void) bo;
    return 64;
}
uint32_t gbm_bo_get_stride(struct gbm_bo *bo) {
    (void) bo;
    return 256;
}
uint32_t gbm_bo_get_stride_for_plane(struct gbm_bo *bo, int plane) {
    (void) bo;
    (void) plane;
    return 256;
}
uint32_t gbm_bo_get_offset(struct gbm_bo *bo, int plane) {
    (void) bo;
    (void) plane;
    return 0;
}
uint64_t gbm_bo_get_modifier(struct gbm_bo *bo) {
    (void) bo;
    return bo_modifier;
}
int gbm_bo_get_plane_count(struct gbm_bo *bo) {
    (void) bo;
    return bo_planes;
}
union gbm_bo_handle gbm_bo_get_handle(struct gbm_bo *bo) {
    return (union gbm_bo_handle) { .u32 = (uint32_t) (((char *) bo - (char *) bos) / sizeof(bos[0])) + 1 };
}
int gbm_bo_get_fd(struct gbm_bo *bo) {
    (void) bo;
    if (fail_step == FAIL_FD) {
        errno = EMFILE;
        return -1;
    }
    last_import_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    return last_import_fd;
}
void *gbm_bo_get_user_data(struct gbm_bo *bo) {
    return bos[((char *) bo - (char *) bos) / sizeof(bos[0])].data;
}
void gbm_bo_set_user_data(struct gbm_bo *bo, void *data, void (*cb)(struct gbm_bo *, void *)) {
    size_t i = ((char *) bo - (char *) bos) / sizeof(bos[0]);
    bos[i].data = data;
    bos[i].destroy = cb;
}
VkResult vkCreateImage(VkDevice d, const VkImageCreateInfo *ci, const VkAllocationCallbacks *a, VkImage *out) {
    (void) d;
    (void) a;
    const VkExternalMemoryImageCreateInfo *external = ci->pNext;
    const VkImageDrmFormatModifierExplicitCreateInfoEXT *modifier = external->pNext;
    explicit_size = modifier->pPlaneLayouts[0].size;
    image_modifier = modifier->drmFormatModifier;
    if (ci->format == VK_FORMAT_UNDEFINED || modifier->drmFormatModifierPlaneCount != (uint32_t) bo_planes ||
        modifier->drmFormatModifier == DRM_FORMAT_MOD_INVALID) {
        invalid_image_creates++;
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    }
    if (fail_step == FAIL_IMAGE)
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    *out = HANDLE(VkImage, ++images_created);
    return VK_SUCCESS;
}
void vkDestroyImage(VkDevice d, VkImage image, const VkAllocationCallbacks *a) {
    (void) d;
    (void) image;
    (void) a;
    if (image == simulated_busy_image)
        images_destroyed_while_busy++;
    images_destroyed++;
}
void vkGetImageSubresourceLayout(VkDevice d, VkImage i, const VkImageSubresource *sub, VkSubresourceLayout *l) {
    (void) d;
    (void) i;
    (void) sub;
    *l = (VkSubresourceLayout
    ) { .offset = fail_step == FAIL_OFFSET ? 1 : 0, .rowPitch = fail_step == FAIL_PITCH ? 257 : 256, .size = layout_size };
}
static VkResult get_memory_props(VkDevice d, VkExternalMemoryHandleTypeFlagBits bits, int fd, VkMemoryFdPropertiesKHR *p) {
    (void) d;
    (void) bits;
    (void) fd;
    p->memoryTypeBits = 1;
    return fail_step == FAIL_MEM_PROPS ? VK_ERROR_INVALID_EXTERNAL_HANDLE : VK_SUCCESS;
}
void vkGetImageMemoryRequirements2(VkDevice d, const VkImageMemoryRequirementsInfo2 *i, VkMemoryRequirements2 *r) {
    (void) d;
    (void) i;
    r->memoryRequirements = (VkMemoryRequirements) { .size = required_size, .alignment = 4096, .memoryTypeBits = 1 };
}
VkResult vkAllocateMemory(VkDevice d, const VkMemoryAllocateInfo *ai, const VkAllocationCallbacks *a, VkDeviceMemory *out) {
    (void) d;
    (void) a;
    allocation_size = ai->allocationSize;
    if (fail_step == FAIL_ALLOC)
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    // Successful import transfers fd ownership to the driver. Close it here.
    const VkImportMemoryFdInfoKHR *import = ai->pNext;
    close(import->fd);
    *out = HANDLE(VkDeviceMemory, ++mem_created);
    return VK_SUCCESS;
}
void vkFreeMemory(VkDevice d, VkDeviceMemory m, const VkAllocationCallbacks *a) {
    (void) d;
    (void) m;
    (void) a;
    mem_freed++;
}
VkResult vkBindImageMemory2(VkDevice d, uint32_t n, const VkBindImageMemoryInfo *i) {
    (void) d;
    (void) n;
    (void) i;
    return fail_step == FAIL_BIND ? VK_ERROR_OUT_OF_DEVICE_MEMORY : VK_SUCCESS;
}
VkResult vkCreateSemaphore(VkDevice d, const VkSemaphoreCreateInfo *ci, const VkAllocationCallbacks *a, VkSemaphore *out) {
    (void) d;
    (void) ci;
    (void) a;
    semaphore_signaled = false;
    *out = HANDLE(VkSemaphore, 100);
    return VK_SUCCESS;
}
void vkDestroySemaphore(VkDevice d, VkSemaphore s_, const VkAllocationCallbacks *a) {
    (void) d;
    (void) s_;
    (void) a;
    semaphore_signaled = false;
}
VkResult vkDeviceWaitIdle(VkDevice d) {
    (void) d;
    idle_calls++;
    if (idle_result == VK_SUCCESS)
        simulated_busy_image = VK_NULL_HANDLE;
    return idle_result;
}
VkResult vkQueueSubmit(VkQueue q, uint32_t n, const VkSubmitInfo *i, VkFence f) {
    (void) q;
    (void) n;
    (void) i;
    (void) f;
    submit_calls++;
    if (semaphore_signaled)
        invalid_signals++;
    semaphore_signaled = true;
    return VK_SUCCESS;
}
static VkResult get_semaphore_fd(VkDevice d, const VkSemaphoreGetFdInfoKHR *i, int *out) {
    (void) d;
    (void) i;
    if (export_result != VK_SUCCESS)
        return export_result;  // failed export does NOT consume payload
    semaphore_signaled = false;
    *out = export_complete ? -1 : open("/dev/null", O_RDONLY | O_CLOEXEC);
    last_fence = *out;
    return VK_SUCCESS;
}
VkResult vkQueueWaitIdle(VkQueue queue) {
    (void) queue;
    return VK_SUCCESS;
}
PFN_vkVoidFunction vkGetDeviceProcAddr(VkDevice d, const char *name) {
    if (!strcmp(name, "vkQueueSubmit"))
        return (PFN_vkVoidFunction) vkQueueSubmit;
    if (!strcmp(name, "vkQueueWaitIdle"))
        return (PFN_vkVoidFunction) vkQueueWaitIdle;
    if (!strcmp(name, "vkDeviceWaitIdle"))
        return (PFN_vkVoidFunction) vkDeviceWaitIdle;

    (void) d;
    if (strcmp(name, "vkGetMemoryFdPropertiesKHR") == 0)
        return fail_step == FAIL_PROC ? NULL : (PFN_vkVoidFunction) get_memory_props;
    if (strcmp(name, "vkGetSemaphoreFdKHR") == 0)
        return (PFN_vkVoidFunction) get_semaphore_fd;
    return NULL;
}
struct drmdev *__wrap_kms_req_builder_get_drmdev(struct kms_req_builder *b) {
    (void) b;
    return (void *) &fake_drm;
}
struct drmdev *__wrap_drmdev_ref(struct drmdev *d) {
    return d;
}
void __wrap_drmdev_unref(struct drmdev *d) {
    (void) d;
}
int __wrap_drmdev_rm_fb(struct drmdev *d, uint32_t fb) {
    (void) d;
    (void) fb;
    return 0;
}
uint32_t __wrap_drmdev_add_fb(
    struct drmdev *d,
    uint32_t w,
    uint32_t h,
    enum pixfmt fmt,
    uint32_t bo,
    uint32_t stride,
    uint32_t offset,
    bool has_mod,
    uint64_t mod
) {
    (void) d;
    (void) w;
    (void) h;
    (void) fmt;
    (void) stride;
    (void) offset;
    addfb_modifier = has_mod ? mod : DRM_FORMAT_MOD_INVALID;
    return bo + 1000;
}
int __wrap_kms_req_builder_push_fb_layer(
    struct kms_req_builder *b,
    const struct kms_fb_layer *l,
    kms_fb_release_cb_t cb,
    kms_deferred_fb_release_cb_t deferred,
    void *data,
    bool *cursor
) {
    (void) b;
    (void) deferred;
    (void) cursor;
    if (l->has_in_fence_fd)
        last_push_fence = l->in_fence_fd;
    if (reject_fence_only && l->has_in_fence_fd)
        return EOPNOTSUPP;
    if (push_error)
        return push_error;
    layers[pushed].cb = cb;
    layers[pushed].data = data;
    layers[pushed].fb = l->drm_fb_id;
    layers[pushed].fd = l->has_in_fence_fd ? l->in_fence_fd : -1;
    pushed++;
    return 0;
}
int __wrap_window_push_composition(struct window *w, struct fl_layer_composition *c) {
    (void) w;
    fl_layer_composition_swap_ptrs(&composition, c);
    return 0;
}
static void observe_deinit(struct surface *surface) {
    deinit_calls++;
    vk_gbm_render_surface_deinit(surface);
}
static void make_surface(void) {
    s = vk_gbm_render_surface_new(tracer, VEC2I(64, 64), (void *) &fake_drm, (void *) &fake_renderer, PIXFMT_ARGB8888, NULL, 0);
    TEST_ASSERT_NOT_NULL(s);
    external_owner = true;
    s->surface.deinit = observe_deinit;
}
static void fill(int i) {
    TEST_ASSERT_EQUAL_INT(0, render_surface_fill(&s->render_surface, &stores[i]));
}
static void queue(int i) {
    TEST_ASSERT_EQUAL_INT(0, render_surface_queue_present(&s->render_surface, &stores[i]));
}
static void destroy_store(int i) {
    if (stores[i].vulkan.destruction_callback) {
        stores[i].vulkan.destruction_callback(stores[i].vulkan.user_data);
        stores[i].vulkan.destruction_callback = NULL;
    }
}
static int present(void) {
    struct fl_layer_props props = { .is_aa_rect = true, .aa_rect = AA_RECT_FROM_COORDS(0, 0, 64, 64) };
    return surface_present_kms(&s->surface, &props, (void *) &fake_builder);
}
void setUp(void) {
    TEST_ASSERT_EQUAL_INT(0, vk_queue_sync_register(HANDLE(VkDevice, 1), HANDLE(VkQueue, 2)));
    fail_fence_wait = fail_fence_dup = false;
    interrupted_fence_waits = 0;
    fence_revents = POLLIN;
    fence_wait_calls = capability_failure = image_capability_queries = 0;
    last_push_fence = -1;
    repaint_snapshot = NULL;
    simulated_busy_image = VK_NULL_HANDLE;
    images_destroyed_while_busy = 0;
    s = NULL;
    external_owner = import_live = false;
    deinit_calls = 0;
    renderer_refs = 1;
    bo_calls = bo_created = bo_destroyed = images_created = images_destroyed = mem_created = mem_freed = 0;
    fail_step = fail_bo_number = invalid_image_creates = 0;
    bo_planes = 1;
    bo_modifier = DRM_FORMAT_MOD_LINEAR;
    memset(vk_modifiers, 0, sizeof(vk_modifiers));
    memset(gbm_requested_modifiers, 0, sizeof(gbm_requested_modifiers));
    n_vk_modifiers = n_gbm_requested_modifiers = gbm_modifier_calls = 0;
    gbm_ignores_modifier_list = false;
    image_modifier = addfb_modifier = DRM_FORMAT_MOD_INVALID;
    last_import_fd = last_fence = -1;
    layout_size = required_size = 16384;
    allocation_size = explicit_size = 0;
    idle_result = export_result = VK_SUCCESS;
    export_supported = true;
    export_complete = semaphore_signaled = false;
    idle_calls = submit_calls = invalid_signals = pushed = push_error = 0;
    reject_fence_only = false;
    memset(stores, 0, sizeof(stores));
    memset(bos, 0, sizeof(bos));
    memset(layers, 0, sizeof(layers));
    compositor = NULL;
    window = NULL;
    scheduler = NULL;
    composition = NULL;
    tracer = tracer_new_with_stubs();
}
void tearDown(void) {
    if (repaint_snapshot)
        surface_unref(repaint_snapshot);
    if (composition)
        fl_layer_composition_unref(composition);
    if (compositor)
        compositor_unref(compositor);
    if (window)
        window_unref(window);
    if (scheduler)
        frame_scheduler_unref(scheduler);
    for (int i = 0; i < ARRAY_SIZE(stores); i++)
        destroy_store(i);
    for (int i = 0; i < pushed; i++) {
        layers[i].cb(layers[i].data);
        if (layers[i].fd >= 0)
            close(layers[i].fd);
    }
    // A failing cycle test must not leak its fixture. Only the test breaks this
    // cycle after measuring it; no equivalent cleanup is injected in production.
    if (s && !deinit_calls && s->front_fb) {
        set_front_fb(s, NULL);
    }
    if (s && !deinit_calls && external_owner)
        surface_unref(&s->surface);
    if (import_live)
        fb_deinit(&imported, HANDLE(VkDevice, 1));
    if (last_import_fd >= 0 && fcntl(last_import_fd, F_GETFD) != -1)
        close(last_import_fd);
    tracer_unref(tracer);
    vk_queue_sync_unregister(HANDLE(VkDevice, 1));
}
static int import_image(void) {
    int r = fb_init(&imported, (void *) &fake_drm, (void *) &fake_renderer, 64, 64, PIXFMT_ARGB8888, NULL, 0);
    import_live = (r == 0);
    return r;
}
static void bind_failure_destroys_image(void) {
    fail_step = FAIL_BIND;
    TEST_ASSERT_NOT_EQUAL(0, import_image());
    TEST_ASSERT_EQUAL_INT(images_created, images_destroyed);
}
static void modifier_explicit_layout_uses_zero_size(void) {
    TEST_ASSERT_EQUAL_INT(0, import_image());
    TEST_ASSERT_EQUAL_UINT64(0, explicit_size);
}
static void allocation_covers_image_memory_requirements(void) {
    required_size = layout_size + 4096;
    TEST_ASSERT_EQUAL_INT(0, import_image());
    TEST_ASSERT_TRUE(allocation_size >= required_size);
}
static void surface_destruction_releases_renderer_reference(void) {
    make_surface();
    external_owner = false;
    surface_unref(&s->surface);
    TEST_ASSERT_EQUAL_INT(1, deinit_calls);
    TEST_ASSERT_EQUAL_INT(1, renderer_refs);
}
static void last_front_buffer_does_not_keep_surface_alive(void) {
    make_surface();
    fill(0);
    queue(0);
    destroy_store(0);
    external_owner = false;
    surface_unref(&s->surface);
    TEST_ASSERT_EQUAL_INT(1, deinit_calls);
}
static void failed_fence_export_does_not_resignal_binary_semaphore(void) {
    make_surface();
    fill(0);
    queue(0);
    export_result = VK_ERROR_OUT_OF_HOST_MEMORY;
    TEST_ASSERT_EQUAL_INT(0, present());
    TEST_ASSERT_EQUAL_INT(0, present());
    TEST_ASSERT_EQUAL_INT(0, invalid_signals);
}
static void failed_device_wait_does_not_present_unsynchronized_image(void) {
    export_supported = false;
    make_surface();
    fill(0);
    queue(0);
    idle_result = VK_ERROR_DEVICE_LOST;
    int r = present();
    TEST_ASSERT_NOT_EQUAL(0, r);
    TEST_ASSERT_EQUAL_INT(0, pushed);
}
static void completed_sync_fd_export_does_not_wait_for_device(void) {
    make_surface();
    fill(0);
    queue(0);
    export_complete = true;
    TEST_ASSERT_EQUAL_INT(0, present());
    TEST_ASSERT_EQUAL_INT(0, idle_calls);
}
static void rejected_kms_push_releases_fd_and_buffer_reference(void) {
    make_surface();
    fill(0);
    queue(0);
    int refs = refcount_get_for_debug(&s->front_fb->n_refs);
    push_error = EIO;
    TEST_ASSERT_EQUAL_INT(EIO, present());
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, last_push_fence);
    TEST_ASSERT_EQUAL_INT(-1, fcntl(last_push_fence, F_GETFD));
    TEST_ASSERT_EQUAL_INT(EBADF, errno);
    TEST_ASSERT_EQUAL_INT(refs, refcount_get_for_debug(&s->front_fb->n_refs));
}
static void import_early_failures_release_resources(void) {
    for (int i = FAIL_BO; i < FAIL_BIND; i++) {
        fail_step = i;
        TEST_ASSERT_NOT_EQUAL(0, import_image());
        TEST_ASSERT_EQUAL_INT(bo_created, bo_destroyed);
        TEST_ASSERT_EQUAL_INT(images_created, images_destroyed);
        TEST_ASSERT_EQUAL_INT(mem_created, mem_freed);
        if (last_import_fd >= 0) {
            TEST_ASSERT_EQUAL_INT(-1, fcntl(last_import_fd, F_GETFD));
            TEST_ASSERT_EQUAL_INT(EBADF, errno);
        }
    }
}
static void partial_surface_initialization_unwinds_previous_images(void) {
    fail_bo_number = 3;
    s = vk_gbm_render_surface_new(tracer, VEC2I(64, 64), (void *) &fake_drm, (void *) &fake_renderer, PIXFMT_ARGB8888, NULL, 0);
    TEST_ASSERT_NULL(s);
    TEST_ASSERT_EQUAL_INT(2, bo_created);
    TEST_ASSERT_EQUAL_INT(bo_created, bo_destroyed);
    TEST_ASSERT_EQUAL_INT(images_created, images_destroyed);
    TEST_ASSERT_EQUAL_INT(mem_created, mem_freed);
    TEST_ASSERT_EQUAL_INT(1, renderer_refs);
}
static const FlutterCompositor *make_compositor(void) {
    scheduler = frame_scheduler_new(false, kDoubleBufferedVsync_PresentMode, NULL, NULL);
    window = dummy_window_new(tracer, scheduler, kVulkan_RendererType, NULL, NULL, VEC2I(64, 64), false, 0, 0, 60);
    compositor = compositor_new(tracer, window);
    return compositor_get_flutter_compositor(compositor);
}
static FlutterLayer layer_for(int i) {
    stores[i].user_data = &s->surface;  // Same association made by create_backing_store.
    return (FlutterLayer) { .struct_size = sizeof(FlutterLayer),
                            .type = kFlutterLayerContentTypeBackingStore,
                            .backing_store = &stores[i],
                            .size = { 64, 64 } };
}
static void two_backing_layers_keep_distinct_images(void) {
    make_surface();
    fill(0);
    fill(1);
    const FlutterCompositor *fc = make_compositor();
    FlutterLayer a = layer_for(0), b = layer_for(1);
    const FlutterLayer *fl[] = { &a, &b };
    TEST_ASSERT_TRUE(fc->present_layers_callback(fl, 2, fc->user_data));
    for (int i = 0; i < 2; i++) {
        struct fl_layer *l = fl_layer_composition_peek_layer(composition, i);
        TEST_ASSERT_EQUAL_INT(0, surface_present_kms(l->surface, &l->props, (void *) &fake_builder));
    }
    TEST_ASSERT_NOT_EQUAL(layers[0].fb, layers[1].fb);
}
static void backing_layer_preserves_horizontal_offset(void) {
    make_surface();
    fill(0);
    const FlutterCompositor *fc = make_compositor();
    FlutterLayer a = layer_for(0);
    a.offset = (FlutterPoint) { 13, 27 };
    const FlutterLayer *fl[] = { &a };
    TEST_ASSERT_TRUE(fc->present_layers_callback(fl, 1, fc->user_data));
    TEST_ASSERT_EQUAL_DOUBLE(13, fl_layer_composition_peek_layer(composition, 0)->props.aa_rect.offset.x);
}
static int reject_queue(struct render_surface *surface, const FlutterBackingStore *store) {
    (void) surface;
    (void) store;
    return EIO;
}
static void compositor_propagates_queue_present_failure(void) {
    make_surface();
    fill(0);
    const FlutterCompositor *fc = make_compositor();
    s->render_surface.queue_present = reject_queue;
    FlutterLayer a = layer_for(0);
    const FlutterLayer *fl[] = { &a };
    TEST_ASSERT_FALSE(fc->present_layers_callback(fl, 1, fc->user_data));
    TEST_ASSERT_NULL(composition);
}
static void failed_export_and_wait_never_reuse_signaled_semaphore(void) {
    make_surface();
    fill(0);
    queue(0);
    export_result = VK_ERROR_OUT_OF_HOST_MEMORY;
    idle_result = VK_ERROR_DEVICE_LOST;
    TEST_ASSERT_EQUAL_INT(EIO, present());
    TEST_ASSERT_EQUAL_INT(EIO, present());
    TEST_ASSERT_EQUAL_INT(0, invalid_signals);
    TEST_ASSERT_EQUAL_INT(1, submit_calls);
    TEST_ASSERT_EQUAL_INT(0, pushed);
    idle_result = VK_SUCCESS;
    TEST_ASSERT_EQUAL_INT(0, present());
    TEST_ASSERT_EQUAL_INT(0, invalid_signals);
}
static void unsupported_kms_fence_waits_then_retries_without_fence(void) {
    make_surface();
    fill(0);
    queue(0);
    reject_fence_only = true;
    TEST_ASSERT_EQUAL_INT(0, present());
    TEST_ASSERT_EQUAL_INT(1, pushed);
    TEST_ASSERT_EQUAL_INT(0, idle_calls);
    TEST_ASSERT_EQUAL_INT(1, fence_wait_calls);
    TEST_ASSERT_EQUAL_INT(-1, layers[0].fd);
    TEST_ASSERT_EQUAL_INT(-1, fcntl(last_push_fence, F_GETFD));
    TEST_ASSERT_EQUAL_INT(EBADF, errno);
}
static void unsupported_kms_fence_with_failed_wait_does_not_present(void) {
    make_surface();
    fill(0);
    queue(0);
    reject_fence_only = true;
    fail_fence_wait = true;
    int refs = refcount_get_for_debug(&s->front_fb->n_refs);
    TEST_ASSERT_EQUAL_INT(EIO, present());
    TEST_ASSERT_EQUAL_INT(0, pushed);
    TEST_ASSERT_EQUAL_INT(refs, refcount_get_for_debug(&s->front_fb->n_refs));
    TEST_ASSERT_EQUAL_INT(-1, fcntl(last_push_fence, F_GETFD));
    TEST_ASSERT_EQUAL_INT(EBADF, errno);
}
static void snapshot_pins_image_across_next_frame_and_engine_release(void) {
    make_surface();
    fill(0);
    const FlutterCompositor *fc = make_compositor();
    FlutterLayer a = layer_for(0);
    const FlutterLayer *fl[] = { &a };
    TEST_ASSERT_TRUE(fc->present_layers_callback(fl, 1, fc->user_data));
    fill(1);
    queue(1);
    destroy_store(0);
    destroy_store(1);
    external_owner = false;
    surface_unref(&s->surface);
    TEST_ASSERT_EQUAL_INT(0, deinit_calls);
    struct fl_layer *l = fl_layer_composition_peek_layer(composition, 0);
    TEST_ASSERT_EQUAL_INT(0, surface_present_kms(l->surface, &l->props, (void *) &fake_builder));
    TEST_ASSERT_EQUAL_UINT32(1001, layers[0].fb);
    fl_layer_composition_unref(composition);
    composition = NULL;
    TEST_ASSERT_EQUAL_INT(0, deinit_calls);  // KMS still holds the original image.
    layers[0].cb(layers[0].data);
    if (layers[0].fd >= 0)
        close(layers[0].fd);
    pushed = 0;
    TEST_ASSERT_EQUAL_INT(1, deinit_calls);
    TEST_ASSERT_EQUAL_INT(1, renderer_refs);
}
// These assert API validity, not support for every possible GBM format.
// Rejecting unsupported layouts before issuing an invalid Vulkan call is safe.
static void multiplane_modifier_is_supported_or_rejected_before_vulkan(void) {
    bo_planes = 2;
    bo_modifier = I915_FORMAT_MOD_Y_TILED_CCS;
    int r = import_image();
    TEST_ASSERT_EQUAL_INT(ENOTSUP, r);
    TEST_ASSERT_EQUAL_INT(0, images_created);
    TEST_ASSERT_EQUAL_INT(0, invalid_image_creates);
    if (r != 0) {
        TEST_ASSERT_EQUAL_INT(bo_created, bo_destroyed);
        TEST_ASSERT_EQUAL_INT(images_created, images_destroyed);
    }
}
static void unsupported_pixel_format_is_rejected_without_abort_or_invalid_vulkan_call(void) {
    int r = fb_init(&imported, (void *) &fake_drm, (void *) &fake_renderer, 64, 64, PIXFMT_RGB565, NULL, 0);
    import_live = (r == 0);
    TEST_ASSERT_NOT_EQUAL(0, r);
    TEST_ASSERT_EQUAL_INT(ENOTSUP, r);
    TEST_ASSERT_EQUAL_INT(0, bo_created);
    TEST_ASSERT_EQUAL_INT(0, images_created);
    TEST_ASSERT_EQUAL_INT(0, invalid_image_creates);
    TEST_ASSERT_EQUAL_INT(bo_created, bo_destroyed);
}
// Models two KMS requests (on-screen + in-flight), each holding two Flutter
// layers. This establishes pool pressure under that overlap, not that the
// installed engine/display scheduler actually reaches it on the Pi.
static void two_layer_three_frame_overlap_can_acquire_next_frame(void) {
    make_surface();
    for (int i = 0; i < 4; i++) {
        fill(i);
        queue(i);
        TEST_ASSERT_EQUAL_INT(0, present());
        destroy_store(i);  // only scanout/front references remain
    }
    TEST_ASSERT_EQUAL_INT(4, pushed);
    fill(4);  // next frame's underlay
    fill(5);  // next frame's overlay
    for (int i = 0; i < 6; i++)
        for (int j = 0; j < i; j++)
            TEST_ASSERT_TRUE(stores[i].vulkan.image->image != stores[j].vulkan.image->image);
}
static void single_layer_three_frame_overlap_fits(void) {
    make_surface();
    for (int i = 0; i < 2; i++) {
        fill(i);
        queue(i);
        TEST_ASSERT_EQUAL_INT(0, present());
        destroy_store(i);
    }
    fill(2);
}
static void repaint_reuses_image_completion_without_new_queue_work(void) {
    make_surface();
    fill(0);
    queue(0);
    repaint_snapshot = render_surface_get_present_surface(&s->render_surface, &stores[0]);
    TEST_ASSERT_NOT_NULL(repaint_snapshot);
    struct fl_layer_props props = { .is_aa_rect = true, .aa_rect = AA_RECT_FROM_COORDS(0, 0, 64, 64) };
    TEST_ASSERT_EQUAL_INT(0, surface_present_kms(repaint_snapshot, &props, (void *) &fake_builder));
    // Model newer Flutter work, while repaint retains the old image identity.
    fill(1);
    queue(1);
    simulated_busy_image = HANDLE(VkImage, stores[1].vulkan.image->image);
    int submits_before_repaint = submit_calls;
    int waits_before_repaint = idle_calls;
    TEST_ASSERT_EQUAL_INT(0, surface_present_kms(repaint_snapshot, &props, (void *) &fake_builder));
    TEST_ASSERT_EQUAL_UINT32(layers[0].fb, layers[1].fb);
    TEST_ASSERT_EQUAL_INT_MESSAGE(submits_before_repaint, submit_calls, "Repaint resubmitted completion work for an old image");
    TEST_ASSERT_EQUAL_INT_MESSAGE(waits_before_repaint, idle_calls, "Repaint waited for unrelated device work");
}
// Model an engine-owned submission which has not completed when its backing
// store is discarded before presentation. No GPU-completion guarantee is
// assumed for the destruction callback; the installed engine contract is open.
static void teardown_without_export_waits_before_destroying_busy_images(void) {
    export_supported = false;
    make_surface();
    fill(0);
    simulated_busy_image = HANDLE(VkImage, stores[0].vulkan.image->image);
    destroy_store(0);
    external_owner = false;
    surface_unref(&s->surface);
    TEST_ASSERT_EQUAL_INT(1, deinit_calls);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, images_destroyed_while_busy, "Destroyed images before simulated GPU completion");
    TEST_ASSERT_EQUAL_INT(images_created, images_destroyed);
}
// Positive control: the existing semaphore-enabled destruction path waits.
static void teardown_with_export_waits_before_destroying_busy_image(void) {
    make_surface();
    fill(0);
    simulated_busy_image = HANDLE(VkImage, stores[0].vulkan.image->image);
    destroy_store(0);
    external_owner = false;
    surface_unref(&s->surface);
    TEST_ASSERT_EQUAL_INT(1, deinit_calls);
    TEST_ASSERT_EQUAL_INT(1, idle_calls);
    TEST_ASSERT_EQUAL_INT(0, images_destroyed_while_busy);
    TEST_ASSERT_EQUAL_INT(images_created, images_destroyed);
}
static void teardown_after_export_disabled_waits_for_later_work(void) {
    make_surface();
    fill(0);
    queue(0);
    export_result = VK_ERROR_OUT_OF_HOST_MEMORY;
    TEST_ASSERT_EQUAL_INT(0, present());  // successful idle fallback retires semaphore
    TEST_ASSERT_EQUAL_INT(1, idle_calls);
    TEST_ASSERT_TRUE(s->render_finished_semaphore == VK_NULL_HANDLE);
    fill(1);  // subsequent engine submission, discarded before presentation
    simulated_busy_image = HANDLE(VkImage, stores[1].vulkan.image->image);
    destroy_store(0);
    destroy_store(1);
    layers[0].cb(layers[0].data);
    if (layers[0].fd >= 0)
        close(layers[0].fd);
    pushed = 0;
    external_owner = false;
    surface_unref(&s->surface);
    TEST_ASSERT_EQUAL_INT(1, deinit_calls);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, images_destroyed_while_busy, "Disabled export path skipped final GPU completion");
}
static void invalid_modifier_is_rejected_before_vulkan_image_creation(void) {
    bo_modifier = DRM_FORMAT_MOD_INVALID;
    TEST_ASSERT_NOT_EQUAL(0, import_image());
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, invalid_image_creates, "Passed DRM_FORMAT_MOD_INVALID to Vulkan explicit modifier import");
    TEST_ASSERT_EQUAL_INT(bo_created, bo_destroyed);
}
static void fbdev_presentation_returns_error_instead_of_aborting(void) {
    make_surface();
    fill(0);
    queue(0);
    struct fl_layer_props props = { .is_aa_rect = true, .aa_rect = AA_RECT_FROM_COORDS(0, 0, 64, 64) };
    TEST_ASSERT_NOT_EQUAL(0, surface_present_fbdev(&s->surface, &props, (void *) &fake_builder));
}
static void non_axis_aligned_layer_is_rejected_instead_of_misrendered(void) {
    make_surface();
    fill(0);
    queue(0);
    struct fl_layer_props props = { .is_aa_rect = false, .aa_rect = AA_RECT_FROM_COORDS(0, 0, 64, 64) };
    TEST_ASSERT_NOT_EQUAL(0, surface_present_kms(&s->surface, &props, (void *) &fake_builder));
    TEST_ASSERT_EQUAL_INT(0, pushed);
}
static void capability_failures_reject_before_image_creation(void) {
    for (capability_failure = 1; capability_failure <= 9; capability_failure++) {
        TEST_ASSERT_NOT_EQUAL(0, import_image());
        TEST_ASSERT_EQUAL_INT(0, images_created);
        TEST_ASSERT_EQUAL_INT(bo_created, bo_destroyed);
    }
}
static void repaint_duplicates_cached_fence_and_keeps_it_alive(void) {
    make_surface();
    fill(0);
    queue(0);
    TEST_ASSERT_EQUAL_INT(0, present());
    int master = s->front_fb->completion_fd;
    TEST_ASSERT_TRUE(master >= 0);
    TEST_ASSERT_NOT_EQUAL(master, layers[0].fd);
    TEST_ASSERT_EQUAL_INT(0, present());
    TEST_ASSERT_EQUAL_INT(1, submit_calls);
    TEST_ASSERT_NOT_EQUAL(layers[0].fd, layers[1].fd);
    close(layers[0].fd);
    layers[0].fd = -1;
    TEST_ASSERT_NOT_EQUAL(-1, fcntl(master, F_GETFD));
    TEST_ASSERT_NOT_EQUAL(-1, fcntl(layers[1].fd, F_GETFD));
}
static void rejected_present_preserves_cached_completion_for_retry(void) {
    make_surface();
    fill(0);
    queue(0);
    push_error = EIO;
    TEST_ASSERT_EQUAL_INT(EIO, present());
    TEST_ASSERT_EQUAL_INT(-1, fcntl(last_push_fence, F_GETFD));
    TEST_ASSERT_NOT_EQUAL(-1, fcntl(s->front_fb->completion_fd, F_GETFD));
    push_error = 0;
    TEST_ASSERT_EQUAL_INT(0, present());
    TEST_ASSERT_EQUAL_INT(1, submit_calls);
}
static void updated_engine_store_renews_completion_without_closing_kms_fd(void) {
    make_surface();
    fill(0);
    queue(0);
    TEST_ASSERT_EQUAL_INT(0, present());
    int old_master = s->front_fb->completion_fd;
    stores[0].did_update = true;
    queue(0);
    TEST_ASSERT_EQUAL_INT(-1, fcntl(old_master, F_GETFD));
    TEST_ASSERT_NOT_EQUAL(-1, fcntl(layers[0].fd, F_GETFD));
    TEST_ASSERT_EQUAL_INT(0, present());
    TEST_ASSERT_EQUAL_INT(2, submit_calls);
}
static void recycled_slot_captures_new_completion(void) {
    make_surface();
    fill(0);
    queue(0);
    TEST_ASSERT_EQUAL_INT(0, present());
    int master = s->front_fb->completion_fd;
    destroy_store(0);
    layers[0].cb(layers[0].data);
    close(layers[0].fd);
    pushed = 0;
    set_front_fb(s, NULL);
    TEST_ASSERT_EQUAL_INT(-1, fcntl(master, F_GETFD));
    fill(0);
    queue(0);
    TEST_ASSERT_EQUAL_INT(0, present());
    TEST_ASSERT_EQUAL_INT(2, submit_calls);
}
static void no_export_repaint_does_not_repeat_device_wait(void) {
    export_supported = false;
    make_surface();
    fill(0);
    queue(0);
    TEST_ASSERT_EQUAL_INT(0, present());
    TEST_ASSERT_EQUAL_INT(1, idle_calls);
    TEST_ASSERT_EQUAL_INT(0, present());
    TEST_ASSERT_EQUAL_INT(1, idle_calls);
}
static void no_kms_fence_repaint_does_not_wait_on_device(void) {
    make_surface();
    fill(0);
    queue(0);
    TEST_ASSERT_EQUAL_INT(0, present());  // normal first scanout
    reject_fence_only = true;  // repaint on a plane without IN_FENCE_FD
    TEST_ASSERT_EQUAL_INT(0, present());
    TEST_ASSERT_EQUAL_INT(1, fence_wait_calls);
    TEST_ASSERT_EQUAL_INT(0, idle_calls);
    TEST_ASSERT_EQUAL_INT(1, submit_calls);
    TEST_ASSERT_EQUAL_INT(0, present());
    TEST_ASSERT_EQUAL_INT(1, fence_wait_calls);
}
static void snapshot_fbdev_returns_error_instead_of_crashing(void) {
    make_surface();
    fill(0);
    queue(0);
    repaint_snapshot = render_surface_get_present_surface(&s->render_surface, &stores[0]);
    TEST_ASSERT_NOT_NULL(repaint_snapshot);
    struct fl_layer_props props = { .is_aa_rect = true, .aa_rect = AA_RECT_FROM_COORDS(0, 0, 64, 64) };
    TEST_ASSERT_EQUAL_INT(EOPNOTSUPP, surface_present_fbdev(repaint_snapshot, &props, (void *) &fake_builder));
}
static void failed_fence_dup_preserves_completion_for_retry(void) {
    make_surface();
    fill(0);
    queue(0);
    int refs = refcount_get_for_debug(&s->front_fb->n_refs);
    fail_fence_dup = true;
    TEST_ASSERT_EQUAL_INT(EMFILE, present());
    TEST_ASSERT_EQUAL_INT(0, pushed);
    TEST_ASSERT_EQUAL_INT(refs, refcount_get_for_debug(&s->front_fb->n_refs));
    TEST_ASSERT_NOT_EQUAL(-1, fcntl(s->front_fb->completion_fd, F_GETFD));
    fail_fence_dup = false;
    TEST_ASSERT_EQUAL_INT(0, present());
    TEST_ASSERT_EQUAL_INT(1, submit_calls);
    TEST_ASSERT_TRUE(fcntl(layers[0].fd, F_GETFD) & FD_CLOEXEC);
}
static void interrupted_fence_wait_retries_without_queue_access(void) {
    make_surface();
    fill(0);
    queue(0);
    reject_fence_only = true;
    interrupted_fence_waits = 2;
    TEST_ASSERT_EQUAL_INT(0, present());
    TEST_ASSERT_EQUAL_INT(3, fence_wait_calls);
    TEST_ASSERT_EQUAL_INT(0, idle_calls);
    TEST_ASSERT_EQUAL_INT(1, pushed);
}
static void errored_sync_file_does_not_present(void) {
    make_surface();
    fill(0);
    queue(0);
    reject_fence_only = true;
    fence_revents = POLLIN | POLLERR;
    TEST_ASSERT_EQUAL_INT(EIO, present());
    TEST_ASSERT_EQUAL_INT(0, pushed);
    TEST_ASSERT_EQUAL_INT(0, idle_calls);
    TEST_ASSERT_EQUAL_INT(-1, fcntl(last_push_fence, F_GETFD));
    TEST_ASSERT_NOT_EQUAL(-1, fcntl(s->front_fb->completion_fd, F_GETFD));
}
static void failed_final_wait_retains_gpu_resources(void) {
    make_surface();
    fill(0);
    struct fb saved[VK_GBM_BUFFER_COUNT];
    memcpy(saved, s->fbs, sizeof(saved));
    VkSemaphore semaphore = s->render_finished_semaphore;
    simulated_busy_image = HANDLE(VkImage, stores[0].vulkan.image->image);
    destroy_store(0);
    external_owner = false;
    idle_result = VK_ERROR_OUT_OF_HOST_MEMORY;
    surface_unref(&s->surface);
    // Capture expectations before fixture-only cleanup. Production deliberately
    // retains handles on this catastrophic error instead of freeing busy memory.
    int destroyed = images_destroyed, freed = mem_freed, released = bo_destroyed;
    int refs = renderer_refs, busy_destroys = images_destroyed_while_busy;
    idle_result = VK_SUCCESS;
    vkDeviceWaitIdle(HANDLE(VkDevice, 1));
    vkDestroySemaphore(HANDLE(VkDevice, 1), semaphore, NULL);
    for (int i = 0; i < VK_GBM_BUFFER_COUNT; i++)
        fb_deinit(&saved[i], HANDLE(VkDevice, 1));
    __wrap_vk_renderer_unref((void *) &fake_renderer);
    TEST_ASSERT_EQUAL_INT(1, deinit_calls);
    TEST_ASSERT_EQUAL_INT(0, destroyed);
    TEST_ASSERT_EQUAL_INT(0, freed);
    TEST_ASSERT_EQUAL_INT(0, released);
    TEST_ASSERT_EQUAL_INT(0, busy_destroys);
    TEST_ASSERT_EQUAL_INT(2, refs);
}
// Open item #7: allocate only with modifiers both the display plane and Vulkan accept.
static struct vk_gbm_render_surface *make_plane_surface(const uint64_t *plane, size_t n_plane) {
    return vk_gbm_render_surface_new(tracer, VEC2I(64, 64), (void *) &fake_drm, (void *) &fake_renderer, PIXFMT_ARGB8888, plane, n_plane);
}
static void present_first_frame_with_modifier(uint64_t expected) {
    TEST_ASSERT_NOT_NULL(s);
    external_owner = true;
    s->surface.deinit = observe_deinit;
    fill(0);
    queue(0);
    TEST_ASSERT_EQUAL_INT(0, present());
    TEST_ASSERT_EQUAL_INT(1, pushed);
    TEST_ASSERT_EQUAL_HEX64(expected, image_modifier);
    TEST_ASSERT_EQUAL_HEX64(expected, addfb_modifier);
}
static void no_common_plane_modifier_fails_cleanly(void) {
    const uint64_t plane[] = { DRM_FORMAT_MOD_BROADCOM_UIF, DRM_FORMAT_MOD_BROADCOM_SAND128 };
    vk_modifiers[0] = DRM_FORMAT_MOD_LINEAR;
    n_vk_modifiers = 1;
    s = make_plane_surface(plane, ARRAY_SIZE(plane));
    TEST_ASSERT_NULL(s);
    // Nothing allocated, nothing to present, and no references leaked.
    TEST_ASSERT_EQUAL_INT(0, bo_calls);
    TEST_ASSERT_EQUAL_INT(0, images_created);
    TEST_ASSERT_EQUAL_INT(0, pushed);
    TEST_ASSERT_EQUAL_INT(1, renderer_refs);
}
static void common_linear_only_allocates_linear(void) {
    const uint64_t plane[] = { DRM_FORMAT_MOD_LINEAR, DRM_FORMAT_MOD_BROADCOM_SAND128 };
    vk_modifiers[0] = DRM_FORMAT_MOD_BROADCOM_UIF;
    vk_modifiers[1] = DRM_FORMAT_MOD_LINEAR;
    n_vk_modifiers = 2;
    s = make_plane_surface(plane, ARRAY_SIZE(plane));
    TEST_ASSERT_EQUAL_INT(VK_GBM_BUFFER_COUNT, gbm_modifier_calls);
    TEST_ASSERT_EQUAL_INT(1, n_gbm_requested_modifiers);
    TEST_ASSERT_EQUAL_HEX64(DRM_FORMAT_MOD_LINEAR, gbm_requested_modifiers[0]);
    present_first_frame_with_modifier(DRM_FORMAT_MOD_LINEAR);
}
static void common_tiled_modifier_is_used(void) {
    const uint64_t plane[] = { DRM_FORMAT_MOD_BROADCOM_UIF, DRM_FORMAT_MOD_BROADCOM_SAND128 };
    vk_modifiers[0] = DRM_FORMAT_MOD_LINEAR;
    vk_modifiers[1] = DRM_FORMAT_MOD_BROADCOM_UIF;
    n_vk_modifiers = 2;
    s = make_plane_surface(plane, ARRAY_SIZE(plane));
    TEST_ASSERT_EQUAL_INT(1, n_gbm_requested_modifiers);
    TEST_ASSERT_EQUAL_HEX64(DRM_FORMAT_MOD_BROADCOM_UIF, gbm_requested_modifiers[0]);
    present_first_frame_with_modifier(DRM_FORMAT_MOD_BROADCOM_UIF);
}
static void allocator_ignoring_common_modifiers_is_rejected(void) {
    const uint64_t plane[] = { DRM_FORMAT_MOD_LINEAR };
    vk_modifiers[0] = DRM_FORMAT_MOD_LINEAR;
    vk_modifiers[1] = I915_FORMAT_MOD_Y_TILED;
    n_vk_modifiers = 2;
    gbm_ignores_modifier_list = true;
    s = make_plane_surface(plane, ARRAY_SIZE(plane));
    TEST_ASSERT_NULL(s);
    TEST_ASSERT_EQUAL_INT(0, images_created);
    TEST_ASSERT_EQUAL_INT(bo_created, bo_destroyed);
    TEST_ASSERT_EQUAL_INT(1, renderer_refs);
}
int main(int argc, char **argv) {
    UNITY_BEGIN();
    int selected = 0;
#define CASE(f)                                  \
    if (argc == 1 || strcmp(argv[1], #f) == 0) { \
        selected++;                              \
        RUN_TEST(f);                             \
    }
    CASE(no_common_plane_modifier_fails_cleanly)
    CASE(common_linear_only_allocates_linear)
    CASE(common_tiled_modifier_is_used)
    CASE(allocator_ignoring_common_modifiers_is_rejected)
    CASE(failed_fence_dup_preserves_completion_for_retry)
    CASE(interrupted_fence_wait_retries_without_queue_access)
    CASE(errored_sync_file_does_not_present)
    CASE(failed_final_wait_retains_gpu_resources)
    CASE(capability_failures_reject_before_image_creation)
    CASE(repaint_duplicates_cached_fence_and_keeps_it_alive)
    CASE(rejected_present_preserves_cached_completion_for_retry)
    CASE(updated_engine_store_renews_completion_without_closing_kms_fd)
    CASE(recycled_slot_captures_new_completion)
    CASE(no_export_repaint_does_not_repeat_device_wait)
    CASE(no_kms_fence_repaint_does_not_wait_on_device)
    CASE(snapshot_fbdev_returns_error_instead_of_crashing)
    CASE(two_layer_three_frame_overlap_can_acquire_next_frame)
    CASE(single_layer_three_frame_overlap_fits)
    CASE(teardown_with_export_waits_before_destroying_busy_image)
    CASE(repaint_reuses_image_completion_without_new_queue_work)
    CASE(teardown_without_export_waits_before_destroying_busy_images)
    CASE(teardown_after_export_disabled_waits_for_later_work)
    CASE(invalid_modifier_is_rejected_before_vulkan_image_creation)
    CASE(fbdev_presentation_returns_error_instead_of_aborting)
    CASE(non_axis_aligned_layer_is_rejected_instead_of_misrendered)
    CASE(multiplane_modifier_is_supported_or_rejected_before_vulkan)
    CASE(unsupported_pixel_format_is_rejected_without_abort_or_invalid_vulkan_call)
    CASE(failed_export_and_wait_never_reuse_signaled_semaphore)
    CASE(unsupported_kms_fence_waits_then_retries_without_fence)
    CASE(unsupported_kms_fence_with_failed_wait_does_not_present)
    CASE(snapshot_pins_image_across_next_frame_and_engine_release)
    CASE(bind_failure_destroys_image)
    CASE(modifier_explicit_layout_uses_zero_size)
    CASE(allocation_covers_image_memory_requirements)
    CASE(surface_destruction_releases_renderer_reference)
    CASE(last_front_buffer_does_not_keep_surface_alive)
    CASE(failed_fence_export_does_not_resignal_binary_semaphore)
    CASE(failed_device_wait_does_not_present_unsynchronized_image)
    CASE(completed_sync_fd_export_does_not_wait_for_device)
    CASE(rejected_kms_push_releases_fd_and_buffer_reference)
    CASE(import_early_failures_release_resources)
    CASE(partial_surface_initialization_unwinds_previous_images)
    CASE(two_backing_layers_keep_distinct_images)
    CASE(backing_layer_preserves_horizontal_offset)
    CASE(compositor_propagates_queue_present_failure)
    return selected ? UNITY_END() : 2;
}
