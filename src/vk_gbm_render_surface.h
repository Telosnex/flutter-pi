// SPDX-License-Identifier: MIT
/*
 * Vulkan GBM render surface
 *
 * - used as a render target for flutter vulkan rendering
 * - can be scanned out using KMS
 *
 * Copyright (c) 2022, Hannes Winkler <hanneswinkler2000@web.de>
 */

#ifndef _FLUTTERPI_SRC_VK_GBM_RENDER_SURFACE_H
#define _FLUTTERPI_SRC_VK_GBM_RENDER_SURFACE_H

#include <stddef.h>
#include <stdint.h>

#include "compositor_ng.h"
#include "pixel_format.h"
#include "util/collection.h"

struct tracer;
struct gbm_device;
struct vk_gbm_render_surface;

#define CAST_VK_GBM_RENDER_SURFACE_UNCHECKED(ptr) ((struct vk_gbm_render_surface *) (ptr))
#ifdef DEBUG
    #define CAST_VK_GBM_RENDER_SURFACE(ptr) __checked_cast_vk_gbm_render_surface(ptr)
ATTR_PURE struct vk_gbm_render_surface *__checked_cast_vk_gbm_render_surface(void *ptr);
#else
    #define CAST_VK_GBM_RENDER_SURFACE(ptr) CAST_VK_GBM_RENDER_SURFACE_UNCHECKED(ptr)
#endif

/**
 * @brief Creates a Vulkan render surface whose buffers KMS can scan out.
 *
 * @param allowed_modifiers The modifiers the target display plane accepts for @p pixel_format,
 *                          or NULL if the plane doesn't list any. Buffers are allocated only with
 *                          modifiers that are in this list and that the Vulkan device can import.
 *                          If there is no such modifier, this logs an error and returns NULL.
 * @param n_allowed_modifiers The number of entries in @p allowed_modifiers.
 */
struct vk_gbm_render_surface *vk_gbm_render_surface_new(
    struct tracer *tracer,
    struct vec2i size,
    struct gbm_device *device,
    struct vk_renderer *renderer,
    enum pixfmt pixel_format,
    const uint64_t *allowed_modifiers,
    size_t n_allowed_modifiers
);

#endif  // _FLUTTERPI_SRC_VK_GBM_RENDER_SURFACE_H
