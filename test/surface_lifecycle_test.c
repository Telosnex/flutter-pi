// SPDX-License-Identifier: MIT
// Real surfaces, texture registry and compositor; only KMS/engine edges faked.
#include <errno.h>

#include <unity.h>

#include "compositor_ng.h"
#include "dmabuf_surface.h"
#include "frame_scheduler.h"
#include "modesetting.h"
#include "surface.h"
#include "surface_private.h"
#include "texture_registry.h"
#include "tracer.h"
#include "window.h"

static struct tracer *tracer;
static struct texture_registry *registry;
static struct compositor *compositor;
static struct window *window;
static struct frame_scheduler *scheduler;
static struct fl_layer_composition *presented;
static int registered, released, removed_fbs, drm_refs, fail_register, fail_push;
static void_callback_t retire;
static void *retire_data;
static int fake_device, fake_builder;

static int reg(void *data, int64_t id) {
    (void) data;
    (void) id;
    if (fail_register)
        return EIO;
    registered++;
    return 0;
}
static int unreg(void *data, int64_t id) {
    (void) data;
    (void) id;
    registered--;
    return 0;
}
static int available(void *data, int64_t id) {
    (void) data;
    (void) id;
    return 0;
}
static void release_buffer(struct dmabuf *buffer) {
    (void) buffer;
    released++;
}

struct drmdev *__wrap_kms_req_builder_get_drmdev(struct kms_req_builder *builder) {
    (void) builder;
    return (void *) &fake_device;
}
struct drmdev *__wrap_drmdev_ref(struct drmdev *dev) {
    drm_refs++;
    return dev;
}
void __wrap_drmdev_unref(struct drmdev *dev) {
    (void) dev;
    drm_refs--;
}
int __wrap_drmdev_rm_fb(struct drmdev *dev, uint32_t id) {
    (void) dev;
    (void) id;
    removed_fbs++;
    return 0;
}
uint32_t __wrap_drmdev_add_fb_from_dmabuf(
    struct drmdev *dev,
    uint32_t w,
    uint32_t h,
    enum pixfmt fmt,
    int fd,
    uint32_t pitch,
    uint32_t offset,
    bool modifier,
    uint64_t value
) {
    (void) dev;
    (void) w;
    (void) h;
    (void) fmt;
    (void) fd;
    (void) pitch;
    (void) offset;
    (void) modifier;
    (void) value;
    return 42;
}
int __wrap_kms_req_builder_push_fb_layer(
    struct kms_req_builder *builder,
    const struct kms_fb_layer *layer,
    kms_fb_release_cb_t cb,
    kms_deferred_fb_release_cb_t deferred,
    void *data,
    bool *cursor
) {
    (void) builder;
    (void) layer;
    (void) deferred;
    (void) cursor;
    if (fail_push)
        return EIO;
    retire = cb;
    retire_data = data;
    return 0;
}
int __wrap_window_push_composition(struct window *w, struct fl_layer_composition *composition) {
    (void) w;
    fl_layer_composition_swap_ptrs(&presented, composition);
    return 0;
}

void setUp(void) {
    registered = released = removed_fbs = drm_refs = fail_register = fail_push = 0;
    retire = NULL;
    presented = NULL;
    tracer = tracer_new_with_stubs();
    registry = texture_registry_new(&(struct texture_registry_interface) { reg, unreg, available }, NULL);
    scheduler = frame_scheduler_new(false, kDoubleBufferedVsync_PresentMode, NULL, NULL);
    window = dummy_window_new(tracer, scheduler, kOpenGL_RendererType, NULL, NULL, VEC2I(640, 480), false, 0, 0, 60);
    compositor = compositor_new(tracer, window);
}
void tearDown(void) {
    if (presented)
        fl_layer_composition_unref(presented);
    if (retire)
        retire(retire_data);
    compositor_unref(compositor);
    window_unref(window);
    frame_scheduler_unref(scheduler);
    TEST_ASSERT_EQUAL_INT(0, registered);
    TEST_ASSERT_EQUAL_INT(0, drm_refs);
    texture_registry_destroy(registry);
    tracer_unref(tracer);
}
static struct dmabuf_surface *make_surface(void) {
    struct dmabuf_surface *s = dmabuf_surface_new(tracer, registry);
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_EQUAL_INT(
        0,
        dmabuf_surface_push_dmabuf(s, &(struct dmabuf) { .width = 16, .height = 16, .format = PIXFMT_XRGB8888 }, release_buffer)
    );
    return s;
}
static void show_id(int64_t id) {
    FlutterPlatformView view = { .struct_size = sizeof view, .identifier = id };
    FlutterLayer layer = { .struct_size = sizeof layer,
                           .type = kFlutterLayerContentTypePlatformView,
                           .platform_view = &view,
                           .size = { 16, 16 } };
    const FlutterLayer *layers[] = { &layer };
    const FlutterCompositor *fc = compositor_get_flutter_compositor(compositor);
    TEST_ASSERT_TRUE(fc->present_layers_callback(layers, 1, fc->user_data));
}
static void test_final_queued_buffer_and_texture_are_released(void) {
    struct dmabuf_surface *s = make_surface();
    TEST_ASSERT_EQUAL_INT(1, registered);
    surface_unref(CAST_SURFACE_UNCHECKED(s));
    TEST_ASSERT_EQUAL_INT(1, released);
    TEST_ASSERT_EQUAL_INT(0, registered);
}
static void test_texture_resolver_failure_is_safe(void) {
    struct dmabuf_surface *s = make_surface();
#ifdef HAVE_EGL_GLES2
    FlutterOpenGLTexture texture;
    TEST_ASSERT_FALSE(texture_registry_gl_external_texture_frame_callback(registry, dmabuf_surface_get_texture_id(s), 16, 16, &texture));
#endif
    surface_unref(CAST_SURFACE_UNCHECKED(s));
    TEST_ASSERT_EQUAL_INT(1, released);
}
static void test_clear_does_not_release_in_flight_buffer(void) {
    struct dmabuf_surface *s = make_surface();
    struct fl_layer_props props = { .is_aa_rect = true, .aa_rect = AA_RECT_FROM_COORDS(0, 0, 16, 16) };
    TEST_ASSERT_EQUAL_INT(0, surface_present_kms(CAST_SURFACE_UNCHECKED(s), &props, (void *) &fake_builder));
    dmabuf_surface_clear(s);
    dmabuf_surface_clear(s);
    surface_unref(CAST_SURFACE_UNCHECKED(s));
    TEST_ASSERT_EQUAL_INT(0, released);
    TEST_ASSERT_EQUAL_INT(0, removed_fbs);
    TEST_ASSERT_EQUAL_INT(0, registered);
    retire(retire_data);
    retire = NULL;
    TEST_ASSERT_EQUAL_INT(1, released);
    TEST_ASSERT_EQUAL_INT(1, removed_fbs);
}
static void test_failed_kms_push_keeps_only_queue_reference(void) {
    struct dmabuf_surface *s = make_surface();
    fail_push = 1;
    struct fl_layer_props props = { .is_aa_rect = true, .aa_rect = AA_RECT_FROM_COORDS(0, 0, 16, 16) };
    TEST_ASSERT_EQUAL_INT(EIO, surface_present_kms(CAST_SURFACE_UNCHECKED(s), &props, (void *) &fake_builder));
    surface_unref(CAST_SURFACE_UNCHECKED(s));
    TEST_ASSERT_EQUAL_INT(1, released);
    TEST_ASSERT_EQUAL_INT(1, removed_fbs);
}
static void test_unregister_middle_last_and_late_flutter_frame(void) {
    struct dmabuf_surface *s[3];
    int64_t ids[3];
    for (int i = 0; i < 3; i++) {
        s[i] = make_surface();
        ids[i] = surface_get_id(CAST_SURFACE_UNCHECKED(s[i]));
        compositor_set_platform_view(compositor, ids[i], CAST_SURFACE_UNCHECKED(s[i]));
        surface_unref(CAST_SURFACE_UNCHECKED(s[i]));  // Registry now owns it.
    }
    show_id(ids[1]);
    TEST_ASSERT_EQUAL_PTR(s[1], fl_layer_composition_peek_layer(presented, 0)->surface);
    compositor_set_platform_view(compositor, ids[1], NULL);
    TEST_ASSERT_EQUAL_INT(0, released);  // The already acquired composition owns it.
    compositor_set_platform_view(compositor, ids[1], NULL);  // Idempotent removal.
    show_id(ids[1]);  // Stale ID becomes an empty surface; never dereference it.
    TEST_ASSERT_NOT_EQUAL((intptr_t) s[1], (intptr_t) fl_layer_composition_peek_layer(presented, 0)->surface);
    TEST_ASSERT_EQUAL_INT(1, released);
    show_id(ids[2]);  // Removing the middle did not corrupt the moved last entry.
    TEST_ASSERT_EQUAL_PTR(s[2], fl_layer_composition_peek_layer(presented, 0)->surface);
    compositor_set_platform_view(compositor, ids[2], NULL);
    compositor_set_platform_view(compositor, ids[0], NULL);
    show_id(ids[2]);
    TEST_ASSERT_EQUAL_INT(3, released);
    struct dmabuf_surface *new_s = make_surface();
    for (int i = 0; i < 3; i++)
        TEST_ASSERT_NOT_EQUAL(ids[i], surface_get_id(CAST_SURFACE_UNCHECKED(new_s)));
    surface_unref(CAST_SURFACE_UNCHECKED(new_s));
}
static void test_texture_registration_failure_returns_null(void) {
    fail_register = 1;
    TEST_ASSERT_NULL(dmabuf_surface_new(tracer, registry));
}
static void *toggle_registration(void *userdata) {
    struct surface *surface = userdata;
    for (int i = 0; i < 1000; i++) {
        compositor_set_platform_view(compositor, surface_get_id(surface), surface);
        compositor_set_platform_view(compositor, surface_get_id(surface), NULL);
    }
    return NULL;
}
static void test_unregister_concurrent_with_flutter_frame(void) {
    struct dmabuf_surface *s = make_surface();
    struct surface *surface = CAST_SURFACE_UNCHECKED(s);
    pthread_t thread;
    TEST_ASSERT_EQUAL_INT(0, pthread_create(&thread, NULL, toggle_registration, surface));
    for (int i = 0; i < 1000; i++) {
        show_id(surface_get_id(surface));
        TEST_ASSERT_NOT_NULL(fl_layer_composition_peek_layer(presented, 0)->surface);
    }
    TEST_ASSERT_EQUAL_INT(0, pthread_join(thread, NULL));
    fl_layer_composition_unref(presented);
    presented = NULL;
    surface_unref(surface);
    TEST_ASSERT_EQUAL_INT(1, released);
}
int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_final_queued_buffer_and_texture_are_released);
    RUN_TEST(test_texture_resolver_failure_is_safe);
    RUN_TEST(test_clear_does_not_release_in_flight_buffer);
    RUN_TEST(test_failed_kms_push_keeps_only_queue_reference);
    RUN_TEST(test_unregister_middle_last_and_late_flutter_frame);
    RUN_TEST(test_texture_registration_failure_returns_null);
    RUN_TEST(test_unregister_concurrent_with_flutter_frame);
    return UNITY_END();
}
