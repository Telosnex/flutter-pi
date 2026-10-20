// SPDX-License-Identifier: MIT
// The cached-surface path is shared by GL/Vulkan. No driver calls are reached.
#include <stdbool.h>
#include <stdlib.h>
static bool fail_modifier_allocation;
static void *audit_calloc(size_t n, size_t size) {
    return fail_modifier_allocation ? NULL : calloc(n, size);
}
#define calloc audit_calloc
#include "../../src/window.c"
#undef calloc

#include <unity.h>

#include "dummy_render_surface.h"
#include "render_surface_private.h"
static struct window w;
static struct tracer *tracer;
static bool fail_allocate;
static bool advertise_modifiers;
static bool empty_modifiers;
static struct drm_plane fake_plane;
static struct drm_crtc fake_crtc;
// Allocation edges return a correctly sized synthetic surface; they do not
// decide whether the production window cache should reuse or replace it.
struct drm_plane *__wrap___next_plane(struct drmdev *d, struct drm_plane *p) {
    (void) d;
    return advertise_modifiers && !p ? &fake_plane : NULL;
}
struct gbm_device *__wrap_drmdev_get_gbm_device(struct drmdev *d) {
    (void) d;
    return NULL;
}
static uint64_t vk_received_modifiers[4];
static size_t n_vk_received_modifiers;
static bool vk_received_null_modifiers;
struct vk_gbm_render_surface *__wrap_vk_gbm_render_surface_new(
    struct tracer *t,
    struct vec2i size,
    struct gbm_device *d,
    struct vk_renderer *r,
    enum pixfmt fmt,
    const uint64_t *modifiers,
    size_t n_modifiers
) {
    (void) d;
    (void) r;
    (void) fmt;
    vk_received_null_modifiers = modifiers == NULL;
    n_vk_received_modifiers = n_modifiers;
    for (size_t i = 0; modifiers && i < n_modifiers && i < 4; i++)
        vk_received_modifiers[i] = modifiers[i];
    return fail_allocate ? NULL : (void *) dummy_render_surface_new(t, size);
}
void __wrap_drm_plane_for_each_modified_format(struct drm_plane *p, drm_plane_modified_format_callback_t cb, void *data) {
    if (!empty_modifiers) {
        cb(p, 0, PIXFMT_ARGB8888, DRM_FORMAT_MOD_LINEAR, data);
        cb(p, 0, PIXFMT_ARGB8888, DRM_FORMAT_MOD_BROADCOM_UIF, data);
    }
}
void setUp(void) {
    fail_allocate = fail_modifier_allocation = advertise_modifiers = empty_modifiers = false;
    memset(vk_received_modifiers, 0, sizeof(vk_received_modifiers));
    n_vk_received_modifiers = 0;
    vk_received_null_modifiers = false;
    fake_plane = (struct drm_plane) { .possible_crtcs = 1, .type = kPrimary_DrmPlaneType, .supports_modifiers = true };
    fake_crtc = (struct drm_crtc) { .bitmask = 1 };
    tracer = tracer_new_with_stubs();
    struct dummy_render_surface *s = dummy_render_surface_new(tracer, VEC2I(64, 64));
    TEST_ASSERT_NOT_NULL(s);
    w = (struct window) { .tracer = tracer, .renderer_type = kVulkan_RendererType, .render_surface = CAST_RENDER_SURFACE_UNCHECKED(s) };
}
void tearDown(void) {
    surface_unref(CAST_SURFACE_UNCHECKED(w.render_surface));
    tracer_unref(tracer);
}
static void cached_surface_matches_requested_dimensions(void) {
    struct render_surface *s = kms_window_get_render_surface_internal(&w, true, VEC2I(128, 96));
    // Rejection is safe; silently returning a differently sized image is not.
    TEST_ASSERT_TRUE(s == NULL || (s->size.x == 128 && s->size.y == 96));
}
static void same_dimensions_can_reuse_cached_surface(void) {
    TEST_ASSERT_EQUAL_PTR(w.render_surface, kms_window_get_render_surface_internal(&w, true, VEC2I(64, 64)));
}
static void failed_resize_keeps_old_cache_alive(void) {
    struct render_surface *old = w.render_surface;
    fail_allocate = true;
    TEST_ASSERT_NULL(kms_window_get_render_surface_internal(&w, true, VEC2I(128, 96)));
    TEST_ASSERT_EQUAL_PTR(old, w.render_surface);
    TEST_ASSERT_EQUAL_INT(64, render_surface_get_size(old).x);
}
static void modifier_allocation_failure_does_not_dereference_null(void) {
    struct render_surface *old = w.render_surface;
    w.kms.crtc = &fake_crtc;
    advertise_modifiers = fail_modifier_allocation = true;
    TEST_ASSERT_NULL(kms_window_get_render_surface_internal(&w, true, VEC2I(128, 96)));
    TEST_ASSERT_EQUAL_PTR(old, w.render_surface);
}
static void gl_modifier_allocation_failure_preserves_cache(void) {
    w.renderer_type = kOpenGL_RendererType;
    modifier_allocation_failure_does_not_dereference_null();
}
static void empty_modifier_list_does_not_require_allocation(void) {
    w.kms.crtc = &fake_crtc;
    advertise_modifiers = empty_modifiers = fail_modifier_allocation = true;
    struct render_surface *s = kms_window_get_render_surface_internal(&w, true, VEC2I(128, 96));
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_EQUAL_INT(128, s->size.x);
    TEST_ASSERT_EQUAL_INT(96, s->size.y);
}
static void vulkan_surface_receives_plane_modifiers(void) {
    w.kms.crtc = &fake_crtc;
    advertise_modifiers = true;
    TEST_ASSERT_NOT_NULL(kms_window_get_render_surface_internal(&w, true, VEC2I(128, 96)));
    TEST_ASSERT_FALSE(vk_received_null_modifiers);
    TEST_ASSERT_EQUAL_size_t(2, n_vk_received_modifiers);
    TEST_ASSERT_EQUAL_HEX64(DRM_FORMAT_MOD_LINEAR, vk_received_modifiers[0]);
    TEST_ASSERT_EQUAL_HEX64(DRM_FORMAT_MOD_BROADCOM_UIF, vk_received_modifiers[1]);
}
static void vulkan_surface_without_plane_modifiers_gets_null(void) {
    w.kms.crtc = &fake_crtc;
    TEST_ASSERT_NOT_NULL(kms_window_get_render_surface_internal(&w, true, VEC2I(128, 96)));
    TEST_ASSERT_TRUE(vk_received_null_modifiers);
    TEST_ASSERT_EQUAL_size_t(0, n_vk_received_modifiers);
}
int main(int argc, char **argv) {
    UNITY_BEGIN();
    int selected = 0;
#define CASE(f)                                  \
    if (argc == 1 || strcmp(argv[1], #f) == 0) { \
        selected++;                              \
        RUN_TEST(f);                             \
    }
    CASE(vulkan_surface_receives_plane_modifiers)
    CASE(vulkan_surface_without_plane_modifiers_gets_null)
    CASE(gl_modifier_allocation_failure_preserves_cache)
    CASE(empty_modifier_list_does_not_require_allocation)
    CASE(modifier_allocation_failure_does_not_dereference_null)
    CASE(failed_resize_keeps_old_cache_alive)
    CASE(cached_surface_matches_requested_dimensions)
    CASE(same_dimensions_can_reuse_cached_surface)
    return selected ? UNITY_END() : 2;
}
