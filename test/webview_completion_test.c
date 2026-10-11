// SPDX-License-Identifier: MIT
// White-box completion state tests. No WebKit subprocess, GPU or display needed.
#include <unity.h>

#include "../src/plugins/gstreamer_video_player/webview_direct.c"
static void_callback_t pending_cb;
static void *pending_data;
static bool reject, inline_callback;
static struct webview_direct *view;
static GObject *buffer;

int compositor_request_repaint(struct compositor *c, void_callback_t cb, void *data) {
    (void) c;
    if (reject)
        return EIO;
    if (inline_callback)
        cb(data);
    else {
        pending_cb = cb;
        pending_data = data;
    }
    return 0;
}
bool compositor_cancel_repaint(struct compositor *c, void_callback_t cb, void *data) {
    (void) c;
    if (pending_cb != cb || pending_data != data)
        return false;
    pending_cb = NULL;
    pending_data = NULL;
    return true;
}
int dmabuf_surface_push_dmabuf(struct dmabuf_surface *s, const struct dmabuf *b, dmabuf_release_cb_t cb) {
    (void) s;
    (void) b;
    (void) cb;
    return EIO;
}
bool dmabuf_surface_take_import_failed(struct dmabuf_surface *s) {
    (void) s;
    return false;
}
unsigned dmabuf_surface_take_unshown_count(void) {
    return 0;
}
bool kms_trace_enabled(void) {
    return false;
}
void kms_trace(const char *format, ...) {
    (void) format;
}

void setUp(void) {
    shared.context = g_main_context_new();
    g_main_context_push_thread_default(shared.context);
    view = g_new0(struct webview_direct, 1);
    g_atomic_ref_count_init(&view->n_refs);
    g_mutex_init(&view->move_lock);
    buffer = g_object_new(G_TYPE_OBJECT, NULL);
    pending_cb = NULL;
    pending_data = NULL;
    reject = inline_callback = false;
}
void tearDown(void) {
    if (view) {
        finish_waiting_frame(view);
        view_unref(view);
    }
    TEST_ASSERT_NULL(pending_cb);
    g_object_unref(buffer);
    while (g_main_context_iteration(shared.context, false)) {
    }
    g_main_context_pop_thread_default(shared.context);
    g_main_context_unref(shared.context);
    shared.context = NULL;
}
static void start(void) {
    wait_for_frame(view, (WPEBuffer *) buffer, false);
}
static void assert_idle(void) {
    TEST_ASSERT_NULL(view->waiting_buffer);
    TEST_ASSERT_NULL(view->waiting_completion);
    TEST_ASSERT_NULL(view->waiting_timeout);
    TEST_ASSERT_TRUE(g_atomic_ref_count_compare(&view->n_refs, 1));
}
static void test_rejected_repaint_has_no_callback_reference(void) {
    reject = true;
    start();
    assert_idle();
}
static void test_inline_callback_before_request_returns(void) {
    inline_callback = true;
    start();
    assert_idle();
}
static void test_timeout_cancels_idle_listener_without_leaking_view(void) {
    for (int i = 0; i < 100; i++) {
        start();
        on_frame_timeout(view);
        assert_idle();
    }
}
static void test_old_dispatched_callback_cannot_complete_new_frame(void) {
    start();
    void_callback_t old_cb = pending_cb;
    void *old_data = pending_data;
    pending_cb = NULL;
    pending_data = NULL;  // Display thread already took it.
    on_frame_timeout(view);
    start();
    struct frame_completion *current = view->waiting_completion;
    old_cb(old_data);  // Delayed delivery of A must not acknowledge B.
    TEST_ASSERT_EQUAL_PTR(current, view->waiting_completion);
    TEST_ASSERT_EQUAL_PTR(buffer, view->waiting_buffer);
    finish_waiting_frame(view);
    assert_idle();
}
static void test_close_after_callback_dispatched_is_safe(void) {
    start();
    void_callback_t cb = pending_cb;
    void *data = pending_data;
    pending_cb = NULL;
    pending_data = NULL;
    finish_waiting_frame(view);
    TEST_ASSERT_TRUE(g_atomic_ref_count_compare(&view->n_refs, 2));
    view_unref(view);
    view = NULL;
    cb(data);  // Final callback releases state, without touching any surface.
}
static gpointer dispatch_from_display_thread(gpointer data) {
    on_frame_scanout(data);
    return NULL;
}
static void test_queued_cross_thread_callback_cannot_complete_new_frame(void) {
    start();
    void *old_data = pending_data;
    pending_cb = NULL;
    pending_data = NULL;
    g_thread_join(g_thread_new("test-display", dispatch_from_display_thread, old_data));
    // The callback is now queued, not yet run on the WebKit context.
    TEST_ASSERT_NOT_NULL(view->waiting_buffer);
    on_frame_timeout(view);
    start();
    struct frame_completion *current = view->waiting_completion;
    while (g_main_context_iteration(shared.context, false)) {
    }
    TEST_ASSERT_EQUAL_PTR(current, view->waiting_completion);
    TEST_ASSERT_EQUAL_PTR(buffer, view->waiting_buffer);
    finish_waiting_frame(view);
    assert_idle();
}
int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_rejected_repaint_has_no_callback_reference);
    RUN_TEST(test_inline_callback_before_request_returns);
    RUN_TEST(test_timeout_cancels_idle_listener_without_leaking_view);
    RUN_TEST(test_old_dispatched_callback_cannot_complete_new_frame);
    RUN_TEST(test_close_after_callback_dispatched_is_safe);
    RUN_TEST(test_queued_cross_thread_callback_cannot_complete_new_frame);
    return UNITY_END();
}
