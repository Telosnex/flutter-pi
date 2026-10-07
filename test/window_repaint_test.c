// SPDX-License-Identifier: MIT
// White-box test: exercise the real repaint path without opening a DRM device.
#include <unity.h>

#include "../src/window.c"
static struct window *w;
static struct frame_scheduler *scheduler;
static int callbacks;
static struct drm_crtc crtc;
static bool flip_during_push;
static void completed(void *data) {
    (void) data;
    callbacks++;
}
struct kms_req_builder *__wrap_drmdev_create_request_builder(struct drmdev *dev, uint32_t crtc_id) {
    (void) dev;
    (void) crtc_id;
    if (flip_during_push)
        frame_scheduler_on_scanout(scheduler, false, 0);
    return NULL;  // Allocation fails before reaching scheduler_present_frame.
}
void setUp(void) {
    struct tracer *tracer = tracer_new_with_stubs();
    scheduler = frame_scheduler_new(false, kDoubleBufferedVsync_PresentMode, NULL, NULL);
    w = dummy_window_new(tracer, scheduler, kOpenGL_RendererType, NULL, NULL, VEC2I(640, 480), false, 0, 0, 60);
    tracer_unref(tracer);
    w->push_composition = kms_window_push_composition;
    w->kms.crtc = &crtc;
    w->composition = fl_layer_composition_new(0);
    callbacks = 0;
    flip_during_push = false;
}
void tearDown(void) {
    window_unref(w);
    frame_scheduler_unref(scheduler);
}
static void test_failed_push_cancels_listener(void) {
    TEST_ASSERT_NOT_EQUAL(0, window_request_repaint(w, completed, NULL));
    frame_scheduler_on_scanout(scheduler, false, 0);
    TEST_ASSERT_EQUAL_INT(0, callbacks);
}
static void test_failure_after_dispatch_reports_callback_acceptance(void) {
    flip_during_push = true;
    TEST_ASSERT_EQUAL_INT(0, window_request_repaint(w, completed, NULL));
    TEST_ASSERT_EQUAL_INT(1, callbacks);
    frame_scheduler_on_scanout(scheduler, false, 0);
    TEST_ASSERT_EQUAL_INT(1, callbacks);
}
int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_failed_push_cancels_listener);
    RUN_TEST(test_failure_after_dispatch_reports_callback_acceptance);
    return UNITY_END();
}
