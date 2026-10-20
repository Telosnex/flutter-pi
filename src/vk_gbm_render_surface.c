// SPDX-License-Identifier: MIT
/*
 * Vulkan GBM Backing Store
 *
 * - a render surface that can be used for filling flutter vulkan backing stores
 * - and for scanout using KMS
 *
 * Copyright (c) 2022, Hannes Winkler <hanneswinkler2000@web.de>
 */

#include "vk_gbm_render_surface.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdlib.h>

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <vulkan.h>

#include "render_surface.h"
#include "render_surface_private.h"
#include "surface.h"
#include "surface_private.h"
#include "tracer.h"
#include "util/collection.h"
#include "util/logging.h"
#include "util/refcounting.h"
#include "vk_renderer.h"
#include "vk_queue_sync.h"

struct vk_gbm_render_surface;
struct vk_renderer;

struct fb {
    struct gbm_bo *bo;
    VkDeviceMemory memory;
    VkImage image;
    FlutterVulkanImage fl_image;
};

// Two Flutter layers across current, pending and rendering frames. This is a
// bounded pool, not a guarantee for arbitrary layer counts; exhaustion remains
// a recoverable EAGAIN instead of reusing a live image.
enum { VK_GBM_BUFFER_COUNT = 6 };

struct locked_fb {
    struct vk_gbm_render_surface *surface;
    atomic_flag is_locked;
    atomic_bool flutter_owned;
    refcount_t n_refs;
    struct fb *fb;
    // One completion payload per acquired backing-store generation. Retain
    // the original fd and duplicate it for each KMS request, including repaints.
    bool completion_captured;
    int completion_fd;  // -1 with completion_captured means already complete
};

struct vk_gbm_render_surface {
    union {
        struct surface surface;
        struct render_surface render_surface;
    };

#ifdef DEBUG
    uuid_t uuid;
#endif

    /**
     * @brief The vulkan renderer we use for talking to vulkan.
     *
     */
    struct vk_renderer *renderer;

    /**
     * @brief Just some vulkan images that are compatible with GBM/DRM/KMS.
     *
     * Space for two layers in each of three overlapping frames.
     */
    struct fb fbs[VK_GBM_BUFFER_COUNT];

    /**
     * @brief This is just some locking wrapper around the simple fbs above.
     *
     * Any locked_fb for which is_locked is false can be locked and then freely used for anything.
     * Everything that needs that locked_fb for something should keep a reference on it.
     * Once the reference count drops to zero, is_locked will be set to false and the fb is ready to be reused again.
     *
     */
    struct locked_fb locked_fbs[VK_GBM_BUFFER_COUNT];

    /**
     * @brief The framebuffer that was last queued to be presented using @ref vk_gbm_render_surface_queue_present_vulkan.
     *
     * This framebuffer is still locked so we can present it again any time, without worrying about it being acquired by
     * flutter using @ref vk_gbm_render_surface_fill_vulkan. Even when @ref vk_gbm_render_surface_present_kms is called,
     * we don't set this to NULL.
     *
     * This is the framebuffer that will be presented on screen when @ref vk_gbm_render_surface_present_kms or
     * @ref vk_gbm_render_surface_present_fbdev is called.
     *
     */
    struct locked_fb *front_fb;

    /**
     * @brief The pixel format to use for all framebuffers.
     *
     */
    enum pixfmt pixel_format;

    /**
     * @brief Exportable binary semaphore used to hand a render-completion
     * fence (sync-file fd) to KMS as the plane IN_FENCE_FD.
     *
     * VK_NULL_HANDLE if the driver can't export SYNC_FD semaphores; in that
     * case we fall back to vkDeviceWaitIdle before presenting.
     *
     * Exporting a SYNC_FD payload has wait semantics (it resets the
     * semaphore), so a single semaphore can be reused every frame.
     */
    VkSemaphore render_finished_semaphore;
    PFN_vkGetSemaphoreFdKHR get_semaphore_fd;
    bool logged_explicit_sync_fallback;
    bool semaphore_failed;

#ifdef DEBUG
    /**
     * @brief The number of framebuffers that are currently locked.
     *
     */
    atomic_int n_locked_fbs;
#endif
};

static void reset_completion(struct locked_fb *fb) {
    if (fb->completion_captured && fb->completion_fd >= 0)
        close(fb->completion_fd);
    fb->completion_fd = -1;
    fb->completion_captured = false;
}

static void locked_fb_destroy(struct locked_fb *fb) {
    struct vk_gbm_render_surface *surface;

    surface = fb->surface;
    reset_completion(fb);
    fb->surface = NULL;

#ifdef DEBUG
    atomic_fetch_sub(&surface->n_locked_fbs, 1);
#endif
    atomic_flag_clear(&fb->is_locked);
    (void) surface;
}

// External owners (Flutter, immutable layer snapshots, KMS) each retain the
// containing surface. The surface's own front-buffer pin must NOT retain it.
static struct locked_fb *locked_fb_ref(struct locked_fb *fb) {
    surface_ref(CAST_SURFACE(fb->surface));
    refcount_inc(&fb->n_refs);
    return fb;
}
static void locked_fb_unref(struct locked_fb *fb) {
    struct surface *owner = CAST_SURFACE(fb->surface);
    if (!refcount_dec(&fb->n_refs))
        locked_fb_destroy(fb);
    surface_unref(owner);
}
static void set_front_fb(struct vk_gbm_render_surface *s, struct locked_fb *fb) {
    struct locked_fb *old = s->front_fb;
    if (old == fb)
        return;
    if (fb)
        refcount_inc(&fb->n_refs);  // internal pin, no surface ref
    s->front_fb = fb;
    if (old && !refcount_dec(&old->n_refs))
        locked_fb_destroy(old);
}

UNUSED static bool atomic_flag_test(atomic_flag *flag) {
    bool before = atomic_flag_test_and_set(flag);
    if (before == false) {
        atomic_flag_clear(flag);
    }
    return before;
}

static void log_locked_fbs(struct vk_gbm_render_surface *surface, const char *note) {
#ifdef VK_LOG_LOCKED_FBS
    LOG_DEBUG("locked buffers (%s):", note ? note : "");
    for (size_t i = 0; i < ARRAY_SIZE(surface->locked_fbs); i++) {
        LOG_DEBUG_UNPREFIXED(" %zu:%c", i, atomic_flag_test(&surface->locked_fbs[i].is_locked) ? 'y' : 'n');
    }
    LOG_DEBUG_UNPREFIXED("\n");
#else
    (void) surface;
    (void) note;
#endif
}

COMPILE_ASSERT(offsetof(struct vk_gbm_render_surface, surface) == 0);
COMPILE_ASSERT(offsetof(struct vk_gbm_render_surface, render_surface.surface) == 0);

#ifdef DEBUG
static const uuid_t uuid = CONST_UUID(0x26, 0xfe, 0x91, 0x53, 0x75, 0xf2, 0x41, 0x90, 0xa1, 0xf5, 0xba, 0xe1, 0x1b, 0x28, 0xd5, 0xe5);
#endif

#define CAST_THIS(ptr) CAST_VK_GBM_RENDER_SURFACE(ptr)
#define CAST_THIS_UNCHECKED(ptr) CAST_VK_GBM_RENDER_SURFACE_UNCHECKED(ptr)

#ifdef DEBUG
ATTR_PURE struct vk_gbm_render_surface *__checked_cast_vk_gbm_render_surface(void *ptr) {
    struct vk_gbm_render_surface *surface;

    surface = CAST_VK_GBM_RENDER_SURFACE_UNCHECKED(ptr);
    ASSERT(uuid_equals(surface->uuid, uuid));
    return surface;
}
#endif

void vk_gbm_render_surface_deinit(struct surface *s);
static int vk_gbm_render_surface_present_kms(struct surface *s, const struct fl_layer_props *props, struct kms_req_builder *builder);
static int vk_gbm_render_surface_present_fbdev(struct surface *s, const struct fl_layer_props *props, struct fbdev_commit_builder *builder);
static int vk_gbm_render_surface_fill(struct render_surface *surface, FlutterBackingStore *fl_store);
static struct surface *vk_gbm_render_surface_get_present_surface(struct render_surface *s, const FlutterBackingStore *store);
static int vk_gbm_render_surface_queue_present(struct render_surface *surface, const FlutterBackingStore *fl_store);

static bool is_srgb_format(VkFormat vk_format) {
    return vk_format == VK_FORMAT_R8G8B8A8_SRGB || vk_format == VK_FORMAT_B8G8R8A8_SRGB;
}

static VkFormat srgb_to_unorm_format(VkFormat vk_format) {
    assert(is_srgb_format(vk_format));
    if (vk_format == VK_FORMAT_R8G8B8A8_SRGB) {
        return VK_FORMAT_R8G8B8A8_UNORM;
    } else if (vk_format == VK_FORMAT_B8G8R8A8_SRGB) {
        return VK_FORMAT_B8G8R8A8_UNORM;
    } else {
        UNREACHABLE();
    }
}

static const VkImageUsageFlags fb_image_usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                                VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

static int check_import_support(struct vk_renderer *renderer, VkFormat format, uint64_t modifier, int width, int height) {
    if (modifier == DRM_FORMAT_MOD_INVALID || width <= 0 || height <= 0)
        return ENOTSUP;

    VkPhysicalDevice physical = vk_renderer_get_physical_device(renderer);
    VkDrmFormatModifierPropertiesListEXT modifiers = {
        .sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT,
    };
    VkFormatProperties2 formats = { .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2, .pNext = &modifiers };
    vkGetPhysicalDeviceFormatProperties2(physical, format, &formats);
    if (modifiers.drmFormatModifierCount == 0)
        return ENOTSUP;
    modifiers.pDrmFormatModifierProperties = calloc(modifiers.drmFormatModifierCount, sizeof(*modifiers.pDrmFormatModifierProperties));
    if (!modifiers.pDrmFormatModifierProperties)
        return ENOMEM;
    vkGetPhysicalDeviceFormatProperties2(physical, format, &formats);
    bool supported = false;
    const VkFormatFeatureFlags required = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
                                          VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    for (uint32_t i = 0; i < modifiers.drmFormatModifierCount; i++) {
        const VkDrmFormatModifierPropertiesEXT *p = &modifiers.pDrmFormatModifierProperties[i];
        if (p->drmFormatModifier == modifier && p->drmFormatModifierPlaneCount == 1 &&
            (p->drmFormatModifierTilingFeatures & required) == required) {
            supported = true;
            break;
        }
    }
    free(modifiers.pDrmFormatModifierProperties);
    if (!supported)
        return ENOTSUP;

    VkPhysicalDeviceImageDrmFormatModifierInfoEXT modifier_info = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT,
        .drmFormatModifier = modifier,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkPhysicalDeviceExternalImageFormatInfo external_info = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
        .pNext = &modifier_info,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };
    VkPhysicalDeviceImageFormatInfo2 info = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
        .pNext = &external_info,
        .format = format,
        .type = VK_IMAGE_TYPE_2D,
        .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
        .usage = fb_image_usage,
    };
    VkExternalImageFormatProperties external_props = { .sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES };
    VkImageFormatProperties2 props = { .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, .pNext = &external_props };
    VkResult result = vkGetPhysicalDeviceImageFormatProperties2(physical, &info, &props);
    if (result == VK_ERROR_FORMAT_NOT_SUPPORTED)
        return ENOTSUP;
    if (result != VK_SUCCESS) {
        LOG_VK_ERROR(result, "Couldn't query Vulkan dma-buf import support");
        return EIO;
    }
    if (!(external_props.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) ||
        !(external_props.externalMemoryProperties.compatibleHandleTypes & VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT) ||
        props.imageFormatProperties.maxExtent.width < (uint32_t) width ||
        props.imageFormatProperties.maxExtent.height < (uint32_t) height || props.imageFormatProperties.maxExtent.depth < 1 ||
        props.imageFormatProperties.maxMipLevels < 1 || props.imageFormatProperties.maxArrayLayers < 1 ||
        !(props.imageFormatProperties.sampleCounts & VK_SAMPLE_COUNT_1_BIT))
        return ENOTSUP;
    return 0;
}

static bool modifier_in_list(uint64_t modifier, const uint64_t *modifiers, size_t n_modifiers) {
    for (size_t i = 0; i < n_modifiers; i++) {
        if (modifiers[i] == modifier)
            return true;
    }
    return false;
}

/**
 * @brief Keep the plane modifiers that this Vulkan device can import and render to.
 *
 * @param plane_modifiers The modifiers the target KMS plane accepts for this format.
 * @param out Receives the common modifiers, in plane order, without duplicates.
 *            Must have room for @p n_plane_modifiers entries.
 * @param n_out Receives the number of common modifiers. Zero means there is none.
 * @returns 0, or an errno if a Vulkan query failed (an unsupported modifier is not an error).
 */
static int select_common_modifiers(
    struct vk_renderer *renderer,
    VkFormat vk_format,
    int width,
    int height,
    const uint64_t *plane_modifiers,
    size_t n_plane_modifiers,
    uint64_t *out,
    size_t *n_out
) {
    size_t n = 0;

    for (size_t i = 0; i < n_plane_modifiers; i++) {
        uint64_t modifier = plane_modifiers[i];
        if (modifier == DRM_FORMAT_MOD_INVALID || modifier_in_list(modifier, out, n))
            continue;

        int ok = check_import_support(renderer, vk_format, modifier, width, height);
        if (ok == ENOTSUP)
            continue;
        if (ok != 0)
            return ok;
        out[n++] = modifier;
    }

    *n_out = n;
    return 0;
}

static VkFormat get_render_vk_format(enum pixfmt pixel_format) {
    VkFormat vk_format = get_pixfmt_info(pixel_format)->vk_format;

    /// FIXME: Right now, using any _SRGB format (for example VK_FORMAT_B8G8R8A8_SRGB) will not work because
    /// that'll break some assertions inside flutter / skia. (VK_FORMAT_B8G8R8A8_SRGB maps to GrColorType::kRGBA_8888_SRGB,
    /// but some other part of flutter will use GrColorType::kRGBA_8888 so you'll get a mismatch at some point)
    /// We're just converting the _SRGB to a _UNORM here, but I'm not really sure that's guaranteed to work.
    /// (_UNORM can mean anything basically)
    if (vk_format != VK_FORMAT_UNDEFINED && is_srgb_format(vk_format)) {
        vk_format = srgb_to_unorm_format(vk_format);
    }
    return vk_format;
}

/**
 * @param modifiers The modifiers both Vulkan and the target plane accept (see select_common_modifiers),
 *                  or NULL if the plane does not list modifiers. Then GBM picks a scanout-capable
 *                  layout from usage flags, as the GL path does.
 */
static int fb_init(
    struct fb *fb,
    struct gbm_device *gbm_device,
    struct vk_renderer *renderer,
    int width,
    int height,
    enum pixfmt pixel_format,
    const uint64_t *modifiers,
    size_t n_modifiers
) {
    PFN_vkGetMemoryFdPropertiesKHR get_memory_fd_props;
    VkSubresourceLayout layout;
    VkDeviceMemory img_device_memory;
    struct gbm_bo *bo;
    VkFormat vk_format;
    VkDevice device;
    VkResult ok;
    VkImage vkimg;
    int fd;

    vk_format = get_render_vk_format(pixel_format);
    if (vk_format == VK_FORMAT_UNDEFINED) {
        LOG_ERROR("Pixel format is not supported by the Vulkan renderer.\n");
        return ENOTSUP;
    }

    device = vk_renderer_get_device(renderer);

    uint32_t gbm_format = get_pixfmt_info(pixel_format)->gbm_format;
    bo = NULL;
    if (modifiers != NULL) {
        // Restricted to modifiers Vulkan can also import. Request scanout use
        // explicitly: on split render/display devices (Pi 5: v3d + vc4), Mesa
        // uses it to place the buffer where the display controller can read it.
        bo = gbm_bo_create_with_modifiers2(
            gbm_device,
            width,
            height,
            gbm_format,
            modifiers,
            n_modifiers,
            GBM_BO_USE_RENDERING | GBM_BO_USE_SCANOUT
        );
        if (bo == NULL) {
            LOG_ERROR("Could not create GBM BO with the plane modifiers. gbm_bo_create_with_modifiers2: %s\n", strerror(errno));
            LOG_ERROR("Will retry without modifiers and keep the result only if it uses a common modifier.\n");
        }
    }
    if (bo == NULL) {
        bo = gbm_bo_create(gbm_device, width, height, gbm_format, GBM_BO_USE_RENDERING | GBM_BO_USE_SCANOUT);
    }
    if (bo == NULL) {
        LOG_ERROR("Could not create GBM BO. gbm_bo_create: %s\n", strerror(errno));
        return EIO;
    }

    // GBM may ignore the list (or we fell back to usage flags). KMS would then
    // reject the buffer at ADDFB2 / atomic commit time, so reject it now.
    if (modifiers != NULL && !modifier_in_list(gbm_bo_get_modifier(bo), modifiers, n_modifiers)) {
        LOG_ERROR(
            "GBM allocated modifier 0x%016llx, which the display plane or Vulkan does not accept.\n",
            (unsigned long long) gbm_bo_get_modifier(bo)
        );
        gbm_bo_destroy(bo);
        return ENOTSUP;
    }

    // The import below describes one memory plane and binds one dma-buf.
    // Multi-plane modifiers (including auxiliary compression planes) require
    // additional layouts and potentially separate memory bindings. Reject them
    // before Vulkan sees an incomplete image description.
    if (gbm_bo_get_plane_count(bo) != 1) {
        LOG_ERROR("Vulkan GBM import only supports single-plane buffers.\n");
        gbm_bo_destroy(bo);
        return ENOTSUP;
    }

    int support = check_import_support(renderer, vk_format, gbm_bo_get_modifier(bo), width, height);
    if (support != 0) {
        LOG_ERROR("GBM format/modifier cannot be imported for Vulkan rendering.\n");
        gbm_bo_destroy(bo);
        return support;
    }

    ok = vkCreateImage(
        device,
        &(VkImageCreateInfo) {
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .flags = 0,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = vk_format,
            .extent = { .width = width, .height = height, .depth = 1 },
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            // Tell vulkan that the tiling we want to use is determined by a DRM format modifier
            // (in our case DRM_FORMAT_MOD_LINEAR, but could be something else as well, using a device-supported
            // modifier is probably faster if that's possible)
            .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
            // These are the usage flags flutter will use too internally
            .usage = fb_image_usage,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .queueFamilyIndexCount = 0,
            .pQueueFamilyIndices = 0,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .pNext =
                &(VkExternalMemoryImageCreateInfo) {
                    .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
                    .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
                    .pNext =
                        &(VkImageDrmFormatModifierExplicitCreateInfoEXT) {
                            .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
                            .drmFormatModifierPlaneCount = 1,
                            .drmFormatModifier = gbm_bo_get_modifier(bo),
                            .pPlaneLayouts =
                                (VkSubresourceLayout[1]) {
                                    {
                                        .offset = gbm_bo_get_offset(bo, 0),
                                        .size = 0,  // Required by VUID-size-02267; offset/pitch describe the layout.
                                        .rowPitch = gbm_bo_get_stride_for_plane(bo, 0),
                                        .arrayPitch = 0,
                                        .depthPitch = 0,
                                    },
                                },
                        },
                },
        },
        NULL,
        &vkimg
    );
    if (ok != VK_SUCCESS) {
        LOG_VK_ERROR(ok, "Could not create Vulkan image. vkCreateImage");
        goto fail_destroy_bo;
    }

    // We _should_ only have one plane in the linear case.
    // Query the layout of that plane to check if it matches the GBM BOs layout.
    vkGetImageSubresourceLayout(
        device,
        vkimg,
        &(VkImageSubresource) {
            .aspectMask = VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT,  // For v3dv, this doesn't really matter
            .mipLevel = 0,
            .arrayLayer = 0,
        },
        &layout
    );

    // Just some paranoid checks that the layout matches (had some issues with that initially)
    if (gbm_bo_get_offset(bo, 0) != layout.offset) {
        LOG_ERROR("GBM BO layout doesn't match image layout. This is probably a driver / kernel bug.\n");
        goto fail_destroy_image;
    }

    if (gbm_bo_get_stride_for_plane(bo, 0) != layout.rowPitch) {
        LOG_ERROR("GBM BO layout doesn't match image layout. This is probably a driver / kernel bug.\n");
        goto fail_destroy_image;
    }

    // gbm_bo_get_fd will dup us a new dmabuf fd.
    // So if we don't use it, we need to close it.
    fd = gbm_bo_get_fd(bo);
    if (fd < 0) {
        LOG_ERROR("Couldn't get dmabuf fd for GBM buffer. gbm_bo_get_fd: %s\n", strerror(errno));
        goto fail_destroy_image;
    }

    // find out as which memory types we can import our dmabuf fd
    VkMemoryFdPropertiesKHR fd_memory_props = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR,
        .pNext = NULL,
        .memoryTypeBits = 0,
    };

    get_memory_fd_props = (PFN_vkGetMemoryFdPropertiesKHR) vkGetDeviceProcAddr(device, "vkGetMemoryFdPropertiesKHR");
    if (get_memory_fd_props == NULL) {
        LOG_ERROR("Couldn't resolve vkGetMemoryFdPropertiesKHR.\n");
        goto fail_close_fd;
    }

    ok = get_memory_fd_props(device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd, &fd_memory_props);
    if (ok != VK_SUCCESS) {
        LOG_VK_ERROR(ok, "Couldn't get dmabuf memory properties. vkGetMemoryFdPropertiesKHR");
        goto fail_close_fd;
    }

    // Find out the memory requirements for our image (the supported memory types for import)
    VkMemoryRequirements2 image_memory_reqs = { .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,
                                                .memoryRequirements = { 0 },
                                                .pNext = NULL };

    vkGetImageMemoryRequirements2(
        device,
        &(VkImageMemoryRequirementsInfo2) {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2,
            .image = vkimg,
            .pNext = NULL,
        },
        &image_memory_reqs
    );

    // Find a memory type that fits both to the dmabuf and the image
    int mem = vk_renderer_find_mem_type(renderer, 0, image_memory_reqs.memoryRequirements.memoryTypeBits & fd_memory_props.memoryTypeBits);
    if (mem < 0) {
        LOG_ERROR("Couldn't find a memory type that's both supported by the image and the dmabuffer.\n");
        goto fail_close_fd;
    }

    // now, create a VkDeviceMemory instance from our dmabuf.
    // after successful import, the fd is owned by the device memory object
    // and we don't need to close it.
    ok = vkAllocateMemory(
        device,
        &(VkMemoryAllocateInfo) {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = image_memory_reqs.memoryRequirements.size,
            .memoryTypeIndex = mem,
            .pNext =
                &(VkImportMemoryFdInfoKHR) {
                    .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
                    .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
                    .fd = fd,
                    .pNext =
                        &(VkMemoryDedicatedAllocateInfo) {
                            .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
                            .image = vkimg,
                            .buffer = VK_NULL_HANDLE,
                            .pNext = NULL,
                        },
                },
        },
        NULL,
        &img_device_memory
    );
    if (ok != VK_SUCCESS) {
        LOG_VK_ERROR(ok, "Couldn't import dmabuf as vulkan device memory. vkAllocateMemory");
        goto fail_close_fd;
    }

    ok = vkBindImageMemory2(
        device,
        1,
        &(VkBindImageMemoryInfo) {
            .sType = VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO,
            .image = vkimg,
            .memory = img_device_memory,
            .memoryOffset = 0,
            .pNext = NULL,
        }
    );
    if (ok != VK_SUCCESS) {
        LOG_VK_ERROR(ok, "Couldn't bind dmabuf-backed vulkan device memory to vulkan image. vkBindImageMemory2");
        goto fail_free_device_memory;
    }

    fb->bo = bo;
    fb->memory = img_device_memory;
    fb->image = vkimg;

    COMPILE_ASSERT(sizeof(FlutterVulkanImage) == 24);
    fb->fl_image = (FlutterVulkanImage) {
        .struct_size = sizeof(FlutterVulkanImage),
        .image = (FlutterVulkanImageHandle) fb->image,
        .format = vk_format,
    };

    return 0;

fail_free_device_memory:
    vkFreeMemory(device, img_device_memory, NULL);
    goto fail_destroy_image;

fail_close_fd:
    close(fd);

fail_destroy_image:
    vkDestroyImage(device, vkimg, NULL);

fail_destroy_bo:
    gbm_bo_destroy(bo);
    return EIO;
}

static void fb_deinit(struct fb *fb, VkDevice device) {
    ASSERT_NOT_NULL(fb);

    vkDestroyImage(device, fb->image, NULL);
    vkFreeMemory(device, fb->memory, NULL);
    gbm_bo_destroy(fb->bo);
}

int vk_gbm_render_surface_init(
    struct vk_gbm_render_surface *surface,
    struct tracer *tracer,
    struct vec2i size,
    struct gbm_device *gbm_device,
    struct vk_renderer *renderer,
    enum pixfmt pixel_format,
    const uint64_t *allowed_modifiers,
    size_t n_allowed_modifiers
) {
    uint64_t *common_modifiers = NULL;
    size_t n_common_modifiers = 0;
    int ok;

    // The window passes the modifiers the chosen display plane accepts for this
    // format (the same list the GL path gets). Allocate only with modifiers that
    // Vulkan can import too; otherwise KMS rejects the frame at present time.
    if (allowed_modifiers != NULL && n_allowed_modifiers > 0) {
        VkFormat vk_format = get_render_vk_format(pixel_format);
        if (vk_format == VK_FORMAT_UNDEFINED) {
            LOG_ERROR("Pixel format is not supported by the Vulkan renderer.\n");
            return ENOTSUP;
        }

        common_modifiers = calloc(n_allowed_modifiers, sizeof *common_modifiers);
        if (common_modifiers == NULL) {
            return ENOMEM;
        }

        ok = select_common_modifiers(
            renderer,
            vk_format,
            (int) size.x,
            (int) size.y,
            allowed_modifiers,
            n_allowed_modifiers,
            common_modifiers,
            &n_common_modifiers
        );
        if (ok != 0) {
            free(common_modifiers);
            return ok;
        }

        if (n_common_modifiers == 0) {
            LOG_ERROR(
                "No DRM format modifier for %s is accepted by both the display plane (%zu offered) and the Vulkan device. "
                "Can't create a Vulkan render surface that KMS can scan out.\n",
                get_pixfmt_info(pixel_format)->name,
                n_allowed_modifiers
            );
            free(common_modifiers);
            return ENOTSUP;
        }
    }

    ok = render_surface_init(CAST_RENDER_SURFACE_UNCHECKED(surface), tracer, size);
    if (ok != 0) {
        free(common_modifiers);
        return EIO;
    }

    for (int i = 0; i < ARRAY_SIZE(surface->fbs); i++) {
        ok = fb_init(
            surface->fbs + i,
            gbm_device,
            renderer,
            (int) size.x,
            (int) size.y,
            pixel_format,
            common_modifiers,
            n_common_modifiers
        );
        if (ok != 0) {
            LOG_ERROR("Could not initialize vulkan GBM framebuffer.\n");
            goto fail_deinit_previous_fbs;
        }

        continue;

fail_deinit_previous_fbs:
        for (int j = 0; j < i; j++) {
            fb_deinit(surface->fbs + j, vk_renderer_get_device(renderer));
        }
        goto fail_deinit_render_surface;
    }
    free(common_modifiers);
    common_modifiers = NULL;

    // Logged once per surface so a device log shows which layout was chosen.
    LOG_ERROR(
        "Vulkan GBM surface %dx%d uses modifier 0x%016llx.\n",
        (int) size.x,
        (int) size.y,
        (unsigned long long) gbm_bo_get_modifier(surface->fbs[0].bo)
    );

    for (int i = 0; i < ARRAY_SIZE(surface->locked_fbs); i++) {
        surface->locked_fbs[i].surface = NULL;
        surface->locked_fbs[i].is_locked = (atomic_flag) ATOMIC_FLAG_INIT;
        atomic_init(&surface->locked_fbs[i].flutter_owned, false);
        surface->locked_fbs[i].n_refs = REFCOUNT_INIT_0;
        surface->locked_fbs[i].fb = surface->fbs + i;
        surface->locked_fbs[i].completion_captured = false;
        surface->locked_fbs[i].completion_fd = -1;
    }

    COMPILE_ASSERT(ARRAY_SIZE(surface->fbs) == ARRAY_SIZE(surface->locked_fbs));

    surface->surface.present_kms = vk_gbm_render_surface_present_kms;
    surface->surface.present_fbdev = vk_gbm_render_surface_present_fbdev;
    surface->surface.deinit = vk_gbm_render_surface_deinit;
    surface->render_surface.fill = vk_gbm_render_surface_fill;
    surface->render_surface.queue_present = vk_gbm_render_surface_queue_present;
    surface->render_surface.get_present_surface = vk_gbm_render_surface_get_present_surface;

#ifdef DEBUG
    uuid_copy(&surface->uuid, uuid);
#endif

    surface->renderer = vk_renderer_ref(renderer);
    surface->front_fb = NULL;
    surface->pixel_format = pixel_format;
#ifdef DEBUG
    surface->n_locked_fbs = 0;
#endif

    surface->render_finished_semaphore = VK_NULL_HANDLE;
    surface->get_semaphore_fd = NULL;
    surface->logged_explicit_sync_fallback = false;
    surface->semaphore_failed = false;
    if (vk_renderer_supports_sync_fd_semaphore_export(renderer)) {
        VkDevice device = vk_renderer_get_device(renderer);

        PFN_vkGetSemaphoreFdKHR get_semaphore_fd = (PFN_vkGetSemaphoreFdKHR) vkGetDeviceProcAddr(device, "vkGetSemaphoreFdKHR");
        if (get_semaphore_fd != NULL) {
            VkSemaphore semaphore = VK_NULL_HANDLE;
            VkResult vk_ok = vkCreateSemaphore(
                device,
                &(VkSemaphoreCreateInfo) {
                    .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
                    .flags = 0,
                    .pNext =
                        &(VkExportSemaphoreCreateInfo) {
                            .sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO,
                            .handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
                            .pNext = NULL,
                        },
                },
                NULL,
                &semaphore
            );
            if (vk_ok == VK_SUCCESS) {
                surface->render_finished_semaphore = semaphore;
                surface->get_semaphore_fd = get_semaphore_fd;
            } else {
                LOG_VK_ERROR(vk_ok, "Couldn't create exportable render-completion semaphore. vkCreateSemaphore");
            }
        }
    }

    if (surface->render_finished_semaphore == VK_NULL_HANDLE) {
        LOG_DEBUG(
            "Vulkan driver doesn't support exporting render-completion fences as sync-file fds. "
            "Falling back to waiting for device idle before presenting, which is slow.\n"
        );
    }

    return 0;

fail_deinit_render_surface:
    free(common_modifiers);
    render_surface_deinit(CAST_SURFACE_UNCHECKED(surface));
    return EIO;
}

struct vk_gbm_render_surface *vk_gbm_render_surface_new(
    struct tracer *tracer,
    struct vec2i size,
    struct gbm_device *device,
    struct vk_renderer *renderer,
    enum pixfmt pixel_format,
    const uint64_t *allowed_modifiers,
    size_t n_allowed_modifiers
) {
    struct vk_gbm_render_surface *surface;
    int ok;

    surface = malloc(sizeof *surface);
    if (surface == NULL) {
        goto fail_return_null;
    }

    ok = vk_gbm_render_surface_init(surface, tracer, size, device, renderer, pixel_format, allowed_modifiers, n_allowed_modifiers);
    if (ok != 0) {
        goto fail_free_surface;
    }

    return surface;

fail_free_surface:
    free(surface);

fail_return_null:
    return NULL;
}

void vk_gbm_render_surface_deinit(struct surface *s) {
    struct vk_gbm_render_surface *vk_surface;

    vk_surface = CAST_THIS(s);
    set_front_fb(vk_surface, NULL);

    // A discarded backing store can still have GPU work in flight, regardless
    // of whether semaphore export was available or has since been disabled.
    // The engine resolver and runner calls use the same queue host-access lock.
    VkResult waited = vk_queue_sync_device_wait_idle(vk_renderer_get_device(vk_surface->renderer));
    if (waited != VK_SUCCESS && waited != VK_ERROR_DEVICE_LOST) {
        // There is no error return from a surface destructor. Retain the GPU
        // resources and renderer reference rather than free memory still in use.
        LOG_VK_ERROR(waited, "Final Vulkan wait failed; retaining GPU resources for safety");
        render_surface_deinit(s);
        return;
    }
    if (vk_surface->render_finished_semaphore != VK_NULL_HANDLE) {
        vkDestroySemaphore(vk_renderer_get_device(vk_surface->renderer), vk_surface->render_finished_semaphore, NULL);
    }

    for (int i = 0; i < ARRAY_SIZE(vk_surface->fbs); i++) {
        fb_deinit(vk_surface->fbs + i, vk_renderer_get_device(vk_surface->renderer));
    }

    vk_renderer_unref(vk_surface->renderer);
    render_surface_deinit(s);
}

struct gbm_bo_meta {
    struct drmdev *drmdev;
    uint32_t fb_id;
};

static void on_destroy_gbm_bo_meta(struct gbm_bo *bo, void *meta_void) {
    struct gbm_bo_meta *meta;
    int ok;

    ASSERT_NOT_NULL(bo);
    ASSERT_NOT_NULL(meta_void);
    meta = meta_void;
    (void) bo;

    ok = drmdev_rm_fb(meta->drmdev, meta->fb_id);
    if (ok != 0) {
        LOG_ERROR("Couldn't remove DRM framebuffer.\n");
    }

    drmdev_unref(meta->drmdev);
    free(meta);
}

static void on_release_layer(void *userdata) {
    struct vk_gbm_render_surface *surface;
    struct locked_fb *fb;

    ASSERT_NOT_NULL(userdata);

    fb = userdata;
    surface = CAST_THIS_UNCHECKED(surface_ref(CAST_SURFACE_UNCHECKED(fb->surface)));

    locked_fb_unref(fb);

    log_locked_fbs(surface, "release_layer");
    surface_unref(CAST_SURFACE_UNCHECKED(surface));
}

static int present_fb(struct surface *s, struct locked_fb *frame, const struct fl_layer_props *props, struct kms_req_builder *builder) {
    struct vk_gbm_render_surface *vk_surface;
    struct gbm_bo_meta *meta;
    struct drmdev *drmdev;
    struct gbm_bo *bo;
    enum pixfmt pixel_format;
    uint32_t fb_id;
    int ok;

    vk_surface = CAST_THIS(s);
    (void) props;
    (void) builder;

    if (!props->is_aa_rect)
        return EOPNOTSUPP;

    surface_lock(s);

    if (frame == NULL)
        frame = vk_surface->front_fb;
    if (frame == NULL) {
        ok = EINVAL;
        goto fail_unlock;
    }
    ASSERT_NOT_NULL_MSG(
        frame,
        "There's no framebuffer available for scanout right now. Make sure you called render_surface_queue_present() before presenting."
    );

    bo = frame->fb->bo;
    meta = gbm_bo_get_user_data(bo);
    if (meta == NULL) {
        meta = malloc(sizeof *meta);
        if (meta == NULL) {
            ok = ENOMEM;
            goto fail_unlock;
        }

        drmdev = kms_req_builder_get_drmdev(builder);
        ASSERT_NOT_NULL(drmdev);

        TRACER_BEGIN(vk_surface->surface.tracer, "drmdev_add_fb (non-opaque)");
        fb_id = drmdev_add_fb(
            drmdev,
            gbm_bo_get_width(bo),
            gbm_bo_get_height(bo),
            vk_surface->pixel_format,
            gbm_bo_get_handle(bo).u32,
            gbm_bo_get_stride(bo),
            gbm_bo_get_offset(bo, 0),
            true,
            gbm_bo_get_modifier(bo)
        );
        TRACER_END(vk_surface->surface.tracer, "drmdev_add_fb (non-opaque)");

        if (fb_id == 0) {
            ok = EIO;
            LOG_ERROR("Couldn't add GBM buffer as DRM framebuffer.\n");
            goto fail_free_meta;
        }

        meta->drmdev = drmdev_ref(drmdev);
        meta->fb_id = fb_id;
        gbm_bo_set_user_data(bo, meta, on_destroy_gbm_bo_meta);
    } else {
        // We can only add this GBM BO to a single KMS device as an fb right now.
        ASSERT_EQUALS_MSG(
            meta->drmdev,
            kms_req_builder_get_drmdev(builder),
            "Currently GBM BOs can only be scanned out on a single KMS device for their whole lifetime."
        );
    }

    /*
    LOG_DEBUG(
        "vk_gbm_render_surface_present_kms:\n"
        "    src_x, src_y, src_w, src_h: %f %f %f %f\n"
        "    dst_x, dst_y, dst_w, dst_h: %f %f %f %f\n",
        0.0, 0.0,
        s->render_surface.size.x,
        s->render_surface.size.y,
        props->aa_rect.offset.x,
        props->aa_rect.offset.y,
        props->aa_rect.size.x,
        props->aa_rect.size.y
    );
    */

    // Use the actual buffer format. Reinterpreting ARGB as opaque XRGB would
    // discard alpha that upper Flutter layers need for platform-view composition.
    fb_id = meta->fb_id;
    pixel_format = vk_surface->pixel_format;

    // KMS must not scan the framebuffer out before the GPU has finished
    // rendering into it. Unlike EGL/GL, Vulkan does not reliably participate
    // in implicit dma-resv synchronization, so hand KMS an explicit
    // render-completion fence:
    //
    // The engine must submit rendering before presenting its backing store.
    // This path also runs for platform-thread repaints, not just raster-thread
    // presentation. The shared Vulkan dispatch wrapper serializes this with
    // engine queue access; the surface mutex alone would not be sufficient.
    // Submit an empty batch that signals our exportable binary
    // semaphore -- queue submission order guarantees it signals only after
    // all previously submitted work has completed -- then export the payload
    // as a sync-file fd and attach it to the KMS plane as IN_FENCE_FD.
    //
    // If that's unsupported (or fails), fall back to a full device wait.
    int in_fence_fd = -1;
    if (!frame->completion_captured) {
        bool render_complete = false;
        if (vk_surface->render_finished_semaphore != VK_NULL_HANDLE && !vk_surface->semaphore_failed) {
            VkResult vk_ok;

            TRACER_BEGIN(vk_surface->surface.tracer, "export render-completion fence");
            vk_ok = vk_queue_sync_submit(
                vk_renderer_get_queue(vk_surface->renderer),
                1,
                &(VkSubmitInfo) {
                    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                    .waitSemaphoreCount = 0,
                    .commandBufferCount = 0,
                    .signalSemaphoreCount = 1,
                    .pSignalSemaphores = (VkSemaphore[1]) { vk_surface->render_finished_semaphore },
                    .pNext = NULL,
                },
                VK_NULL_HANDLE
            );
            if (vk_ok == VK_SUCCESS) {
                vk_ok = vk_surface->get_semaphore_fd(
                    vk_renderer_get_device(vk_surface->renderer),
                    &(VkSemaphoreGetFdInfoKHR) {
                        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR,
                        .semaphore = vk_surface->render_finished_semaphore,
                        .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
                        .pNext = NULL,
                    },
                    &in_fence_fd
                );
                if (vk_ok == VK_SUCCESS) {
                    // -1 is a successful, already-signaled SYNC_FD payload.
                    render_complete = true;
                } else {
                    vk_surface->semaphore_failed = true;  // failed export did not consume the signal
                    in_fence_fd = -1;
                    if (!vk_surface->logged_explicit_sync_fallback) {
                        vk_surface->logged_explicit_sync_fallback = true;
                        LOG_VK_ERROR(vk_ok, "Couldn't export render-completion fence, waiting for device idle instead. vkGetSemaphoreFdKHR");
                    }
                }
            } else {
                vk_surface->semaphore_failed = true;
                if (!vk_surface->logged_explicit_sync_fallback) {
                    vk_surface->logged_explicit_sync_fallback = true;
                    LOG_VK_ERROR(vk_ok, "Couldn't submit render-completion fence signal, waiting for device idle instead. vkQueueSubmit");
                }
            }
            TRACER_END(vk_surface->surface.tracer, "export render-completion fence");
        }

        if (!render_complete) {
            TRACER_BEGIN(vk_surface->surface.tracer, "vkDeviceWaitIdle");
            VkResult wait_result = vk_queue_sync_device_wait_idle(vk_renderer_get_device(vk_surface->renderer));
            TRACER_END(vk_surface->surface.tracer, "vkDeviceWaitIdle");
            if (wait_result != VK_SUCCESS) {
                LOG_VK_ERROR(wait_result, "Render-completion wait failed; not presenting image");
                ok = EIO;
                goto fail_unlock;
            }
            if (vk_surface->semaphore_failed && vk_surface->render_finished_semaphore != VK_NULL_HANDLE) {
                // Idle completes the pending signal but does not unsignal a binary
                // semaphore. Retire it and use the checked idle fallback thereafter.
                vkDestroySemaphore(vk_renderer_get_device(vk_surface->renderer), vk_surface->render_finished_semaphore, NULL);
                vk_surface->render_finished_semaphore = VK_NULL_HANDLE;
                vk_surface->get_semaphore_fd = NULL;
            }
        }

        frame->completion_fd = in_fence_fd;
        frame->completion_captured = true;
    }
    // KMS owns a distinct fd. The retained payload is reusable for repaints and
    // survives a rejected KMS request; closing one request cannot invalidate it.
    in_fence_fd = frame->completion_fd >= 0 ? fcntl(frame->completion_fd, F_DUPFD_CLOEXEC, 0) : -1;
    if (frame->completion_fd >= 0 && in_fence_fd < 0) {
        ok = errno;
        goto fail_unlock;
    }

    TRACER_BEGIN(vk_surface->surface.tracer, "kms_req_builder_push_fb_layer");
    locked_fb_ref(frame);
    for (;;) {
        ok = kms_req_builder_push_fb_layer(
            builder,
            &(const struct kms_fb_layer) {
                .drm_fb_id = fb_id,
                .format = pixel_format,
                .has_modifier = true,
                .modifier = gbm_bo_get_modifier(bo),

                .dst_x = (int32_t) props->aa_rect.offset.x,
                .dst_y = (int32_t) props->aa_rect.offset.y,
                .dst_w = (uint32_t) props->aa_rect.size.x,
                .dst_h = (uint32_t) props->aa_rect.size.y,

                .src_x = 0,
                .src_y = 0,
                .src_w = DOUBLE_TO_FP1616_ROUNDED(vk_surface->render_surface.size.x),
                .src_h = DOUBLE_TO_FP1616_ROUNDED(vk_surface->render_surface.size.y),

                // see egl_gbm_render_surface_present_kms
                .has_rotation = true,
                .rotation = PLANE_TRANSFORM_ROTATE_0,
                .enforce_rotation = false,

                // On success, the KMS request takes ownership of the fd.
                .has_in_fence_fd = in_fence_fd >= 0,
                .in_fence_fd = in_fence_fd,
            },
            on_release_layer,
            NULL,
            frame,
            NULL
        );
        if (ok != EOPNOTSUPP || in_fence_fd < 0)
            break;
        // Wait only for this image, not all unrelated work on the engine queue.
        // poll() does not access VkQueue and also works on repaints to a plane
        // that cannot accept IN_FENCE_FD.
        struct pollfd pfd = { .fd = in_fence_fd, .events = POLLIN };
        int result;
        do {
            result = poll(&pfd, 1, -1);
        } while (result < 0 && errno == EINTR);
        close(in_fence_fd);
        in_fence_fd = -1;
        if (result <= 0 || !(pfd.revents & POLLIN) || (pfd.revents & (POLLERR | POLLNVAL))) {
            ok = EIO;
            break;
        }
        reset_completion(frame);
        frame->completion_captured = true;  // the retained fence has now completed
    }
    TRACER_END(vk_surface->surface.tracer, "kms_req_builder_push_fb_layer");
    if (ok != 0) {
        if (in_fence_fd >= 0) {
            close(in_fence_fd);
        }
        goto fail_unref_locked_fb;
    }

    surface_unlock(s);
    return ok;

fail_unref_locked_fb:
    locked_fb_unref(frame);
    goto fail_unlock;

fail_free_meta:
    free(meta);

fail_unlock:
    surface_unlock(s);
    return ok;
}

static int vk_gbm_render_surface_present_kms(struct surface *s, const struct fl_layer_props *props, struct kms_req_builder *builder) {
    return present_fb(s, NULL, props, builder);
}

// A composition must retain image identity, not a pointer to a mutable "latest
// frame" surface shared by all backing-store layers and subsequent repaints.
struct vk_frame_surface {
    struct surface surface;
    struct locked_fb *frame;
};
static int frame_present_kms(struct surface *s, const struct fl_layer_props *props, struct kms_req_builder *builder) {
    struct vk_frame_surface *frame = (void *) s;
    return present_fb(CAST_SURFACE(frame->frame->surface), frame->frame, props, builder);
}
static void frame_deinit(struct surface *s) {
    struct vk_frame_surface *frame = (void *) s;
    locked_fb_unref(frame->frame);
    surface_deinit(s);
}
static struct surface *vk_gbm_render_surface_get_present_surface(struct render_surface *s, const FlutterBackingStore *store) {
    struct vk_gbm_render_surface *owner = CAST_THIS(s);
    struct vk_frame_surface *snapshot = malloc(sizeof(*snapshot));
    if (!snapshot)
        return NULL;
    surface_lock(&s->surface);
    struct locked_fb *fb = NULL;
    for (int i = 0; i < ARRAY_SIZE(owner->locked_fbs); i++) {
        if (owner->fbs[i].fl_image.image == store->vulkan.image->image && owner->locked_fbs[i].surface == owner) {
            fb = &owner->locked_fbs[i];
            break;
        }
    }
    if (!fb || surface_init(&snapshot->surface, s->surface.tracer) != 0) {
        surface_unlock(&s->surface);
        free(snapshot);
        return NULL;
    }
    snapshot->frame = locked_fb_ref(fb);
    snapshot->surface.deinit = frame_deinit;
    snapshot->surface.present_kms = frame_present_kms;
    snapshot->surface.present_fbdev = vk_gbm_render_surface_present_fbdev;
    surface_unlock(&s->surface);
    return &snapshot->surface;
}

static int
vk_gbm_render_surface_present_fbdev(struct surface *s, const struct fl_layer_props *props, struct fbdev_commit_builder *builder) {
    (void) s;
    (void) props;
    (void) builder;
    return EOPNOTSUPP;
}

static void on_flutter_release_backing_store(void *userdata) {
    struct locked_fb *fb = userdata;
    // This also covers a store discarded before present_layers. Presentation
    // takes its own front-buffer reference; it must not consume Flutter's ref.
    atomic_store_explicit(&fb->flutter_owned, false, memory_order_relaxed);
    locked_fb_unref(fb);
}

static int vk_gbm_render_surface_fill(struct render_surface *s, FlutterBackingStore *fl_store) {
    struct vk_gbm_render_surface *render_surface;
    int i, ok;

    render_surface = CAST_THIS(s);

    surface_lock(CAST_SURFACE_UNCHECKED(s));

    // Try to find & lock a locked_fb we can use.
    // Note we use atomics here even though we hold the surfaces' mutex because
    // releasing a locked_fb is possibly done without the mutex.
    for (i = 0; i < ARRAY_SIZE(render_surface->locked_fbs); i++) {
        if (atomic_flag_test_and_set(&render_surface->locked_fbs[i].is_locked) == false) {
            goto locked;
        }
    }

    // This is pool pressure, not a failed Vulkan/GBM allocation. Do not reuse a
    // live image or assert in Debug: Flutter may still own unpresented stores,
    // and KMS may hold the current, in-flight and queued frames independently.
    LOG_ERROR("Vulkan backing-store pool exhausted (%dx%d, %zu slots).\n", s->size.x, s->size.y, ARRAY_SIZE(render_surface->locked_fbs));
    for (int slot = 0; slot < ARRAY_SIZE(render_surface->locked_fbs); slot++) {
        struct locked_fb *fb = &render_surface->locked_fbs[slot];
        // Counts can change concurrently as engine/KMS callbacks retire refs.
        LOG_ERROR(
            "  slot %d: refs=%d flutter=%d front=%d\n",
            slot,
            refcount_get_for_debug(&fb->n_refs),
            atomic_load_explicit(&fb->flutter_owned, memory_order_relaxed),
            render_surface->front_fb == fb
        );
    }
    ok = EAGAIN;
    goto fail_unlock;

locked:;
    reset_completion(&render_surface->locked_fbs[i]);
#ifdef DEBUG
    atomic_fetch_add(&render_surface->n_locked_fbs, 1);
    log_locked_fbs(CAST_THIS_UNCHECKED(s), "fill");
    //ASSERT_MSG(before + 1 <= 3, "sanity check failed: too many locked fbs for double-buffered vsync");
#endif
    render_surface->locked_fbs[i].surface = CAST_VK_GBM_RENDER_SURFACE(surface_ref(CAST_SURFACE_UNCHECKED(s)));
    render_surface->locked_fbs[i].n_refs = REFCOUNT_INIT_1;
    atomic_store_explicit(&render_surface->locked_fbs[i].flutter_owned, true, memory_order_relaxed);

    COMPILE_ASSERT(sizeof(FlutterVulkanBackingStore) == 16 || sizeof(FlutterVulkanBackingStore) == 32);
    fl_store->type = kFlutterBackingStoreTypeVulkan;
    fl_store->vulkan = (FlutterVulkanBackingStore) {
        .struct_size = sizeof(FlutterVulkanBackingStore),
        .image = &render_surface->locked_fbs[i].fb->fl_image,
        .user_data = &render_surface->locked_fbs[i],
        .destruction_callback = on_flutter_release_backing_store,
    };

    surface_unlock(CAST_SURFACE_UNCHECKED(s));

    return 0;

fail_unlock:
    surface_unlock(CAST_SURFACE_UNCHECKED(s));
    return ok;
}

static int vk_gbm_render_surface_queue_present(struct render_surface *s, const FlutterBackingStore *fl_store) {
    struct vk_gbm_render_surface *vk_surface;
    struct locked_fb *fb;

    vk_surface = CAST_THIS(s);

    ASSERT_EQUALS(fl_store->type, kFlutterBackingStoreTypeVulkan);

    surface_lock(CAST_SURFACE_UNCHECKED(s));

    // find out which fb this image belongs too
    fb = NULL;
    for (int i = 0; i < ARRAY_SIZE(vk_surface->locked_fbs); i++) {
        if (vk_surface->locked_fbs[i].fb->fl_image.image == fl_store->vulkan.image->image) {
            fb = vk_surface->locked_fbs + i;
            break;
        }
    }

    if (fb == NULL) {
        LOG_ERROR("The vulkan image flutter wants to present is not known to this render surface.\n");
        surface_unlock(CAST_SURFACE_UNCHECKED(s));
        return EINVAL;
    }

    // A reused engine store may contain new rendering even without a new fill.
    // Already-submitted KMS requests own their own copies of the old fence.
    if (fl_store->did_update)
        reset_completion(fb);

    // Replace the front fb with the new one
    // (will unref the old one if not NULL internally)
    set_front_fb(vk_surface, fb);

    // Flutter's reference is released only by its destruction callback, not
    // here: a store may be presented more than once or discarded unpresented.

    log_locked_fbs(vk_surface, "queue_present");

    surface_unlock(CAST_SURFACE_UNCHECKED(s));
    return 0;
}
