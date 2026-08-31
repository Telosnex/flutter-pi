#include <stdbool.h>
#include <stdint.h>

#include <frame_scheduler.h>
#include <unity.h>

void setUp() {
}

void tearDown() {
}

// --- callback recording helpers ------------------------------------------

struct cb_log {
    int n_presents;
    void *last_present_userdata;
    int n_cancels;
    void *last_cancel_userdata;
    int n_vsync_replies;
    intptr_t last_baton;
    uint64_t last_frame_start_ns;
    uint64_t last_next_frame_start_ns;
};

static struct cb_log log_;

static void reset_log() {
    log_ = (struct cb_log){ 0 };
}

static void on_present(void *userdata) {
    log_.n_presents++;
    log_.last_present_userdata = userdata;
}

static void on_cancel(void *userdata) {
    log_.n_cancels++;
    log_.last_cancel_userdata = userdata;
}

static void on_vsync_reply(void *userdata, intptr_t baton, uint64_t frame_start_ns, uint64_t next_frame_start_ns) {
    (void) userdata;
    log_.n_vsync_replies++;
    log_.last_baton = baton;
    log_.last_frame_start_ns = frame_start_ns;
    log_.last_next_frame_start_ns = next_frame_start_ns;
}

#define FRAME_INTERVAL_NS (1000000000ull / 60)

static struct frame_scheduler *make_scheduler() {
    struct frame_scheduler *s = frame_scheduler_new(true, kDoubleBufferedVsync_PresentMode, on_vsync_reply, NULL);
    TEST_ASSERT_NOT_NULL(s);
    frame_scheduler_set_frame_interval(s, FRAME_INTERVAL_NS);
    return s;
}

// --- tests ----------------------------------------------------------------

void test_present_when_idle_presents_immediately() {
    reset_log();
    struct frame_scheduler *s = make_scheduler();

    int frame1;
    frame_scheduler_present_frame(s, on_present, &frame1, on_cancel);
    TEST_ASSERT_EQUAL_INT(1, log_.n_presents);
    TEST_ASSERT_EQUAL_PTR(&frame1, log_.last_present_userdata);
    TEST_ASSERT_EQUAL_INT(0, log_.n_cancels);

    frame_scheduler_unref(s);
}

void test_present_while_in_flight_is_queued_until_scanout() {
    reset_log();
    struct frame_scheduler *s = make_scheduler();

    int frame1, frame2;
    frame_scheduler_present_frame(s, on_present, &frame1, on_cancel);
    TEST_ASSERT_EQUAL_INT(1, log_.n_presents);

    // frame2 must not be presented while frame1's flip is outstanding.
    frame_scheduler_present_frame(s, on_present, &frame2, on_cancel);
    TEST_ASSERT_EQUAL_INT(1, log_.n_presents);
    TEST_ASSERT_EQUAL_INT(0, log_.n_cancels);

    // frame1 scanned out --> frame2 presented.
    frame_scheduler_on_scanout(s, true, 1000);
    TEST_ASSERT_EQUAL_INT(2, log_.n_presents);
    TEST_ASSERT_EQUAL_PTR(&frame2, log_.last_present_userdata);

    frame_scheduler_unref(s);
}

void test_newer_frame_displaces_queued_frame() {
    reset_log();
    struct frame_scheduler *s = make_scheduler();

    int frame1, frame2, frame3;
    frame_scheduler_present_frame(s, on_present, &frame1, on_cancel);
    frame_scheduler_present_frame(s, on_present, &frame2, on_cancel);
    frame_scheduler_present_frame(s, on_present, &frame3, on_cancel);

    // frame2 was displaced by frame3 and must have been cancelled.
    TEST_ASSERT_EQUAL_INT(1, log_.n_presents);
    TEST_ASSERT_EQUAL_INT(1, log_.n_cancels);
    TEST_ASSERT_EQUAL_PTR(&frame2, log_.last_cancel_userdata);

    frame_scheduler_on_scanout(s, true, 1000);
    TEST_ASSERT_EQUAL_INT(2, log_.n_presents);
    TEST_ASSERT_EQUAL_PTR(&frame3, log_.last_present_userdata);

    frame_scheduler_unref(s);
}

void test_vsync_request_replied_immediately_when_idle() {
    reset_log();
    struct frame_scheduler *s = make_scheduler();

    frame_scheduler_on_fl_vsync_request(s, 0x1234);
    TEST_ASSERT_EQUAL_INT(1, log_.n_vsync_replies);
    TEST_ASSERT_EQUAL(0x1234, log_.last_baton);
    // frame start/target should be interval apart.
    TEST_ASSERT_EQUAL_UINT64(FRAME_INTERVAL_NS, log_.last_next_frame_start_ns - log_.last_frame_start_ns);

    frame_scheduler_unref(s);
}

void test_vsync_request_deferred_until_scanout() {
    reset_log();
    struct frame_scheduler *s = make_scheduler();

    int frame1;
    frame_scheduler_present_frame(s, on_present, &frame1, on_cancel);

    frame_scheduler_on_fl_vsync_request(s, 0x1234);
    TEST_ASSERT_EQUAL_INT(0, log_.n_vsync_replies);

    // Scanout with a timestamp: the reply must use it.
    frame_scheduler_on_scanout(s, true, 5000000);
    TEST_ASSERT_EQUAL_INT(1, log_.n_vsync_replies);
    TEST_ASSERT_EQUAL(0x1234, log_.last_baton);
    TEST_ASSERT_EQUAL_UINT64(5000000, log_.last_frame_start_ns);
    TEST_ASSERT_EQUAL_UINT64(5000000 + FRAME_INTERVAL_NS, log_.last_next_frame_start_ns);

    frame_scheduler_unref(s);
}

void test_vsync_reply_waits_while_queued_frame_present() {
    reset_log();
    struct frame_scheduler *s = make_scheduler();

    int frame1, frame2;
    frame_scheduler_present_frame(s, on_present, &frame1, on_cancel);
    frame_scheduler_present_frame(s, on_present, &frame2, on_cancel);
    frame_scheduler_on_fl_vsync_request(s, 0x1234);

    // First scanout presents the queued frame2; baton stays pending.
    frame_scheduler_on_scanout(s, true, 1000);
    TEST_ASSERT_EQUAL_INT(2, log_.n_presents);
    TEST_ASSERT_EQUAL_INT(0, log_.n_vsync_replies);

    // Second scanout has nothing queued; baton is replied.
    frame_scheduler_on_scanout(s, true, 2000);
    TEST_ASSERT_EQUAL_INT(1, log_.n_vsync_replies);
    TEST_ASSERT_EQUAL(0x1234, log_.last_baton);

    frame_scheduler_unref(s);
}

void test_present_failure_resets_pipeline() {
    reset_log();
    struct frame_scheduler *s = make_scheduler();

    int frame1, frame2;
    frame_scheduler_present_frame(s, on_present, &frame1, on_cancel);
    frame_scheduler_present_frame(s, on_present, &frame2, on_cancel);
    frame_scheduler_on_fl_vsync_request(s, 0x1234);

    // frame1's commit failed: queued frame2 must be cancelled, baton replied.
    frame_scheduler_on_present_failed(s);
    TEST_ASSERT_EQUAL_INT(1, log_.n_cancels);
    TEST_ASSERT_EQUAL_PTR(&frame2, log_.last_cancel_userdata);
    TEST_ASSERT_EQUAL_INT(1, log_.n_vsync_replies);
    TEST_ASSERT_EQUAL(0x1234, log_.last_baton);

    // Pipeline is idle again; next present goes through immediately.
    int frame3;
    frame_scheduler_present_frame(s, on_present, &frame3, on_cancel);
    TEST_ASSERT_EQUAL_INT(2, log_.n_presents);
    TEST_ASSERT_EQUAL_PTR(&frame3, log_.last_present_userdata);

    frame_scheduler_unref(s);
}

void test_spurious_scanout_is_ignored() {
    reset_log();
    struct frame_scheduler *s = make_scheduler();

    frame_scheduler_on_scanout(s, true, 1000);
    TEST_ASSERT_EQUAL_INT(0, log_.n_presents);
    TEST_ASSERT_EQUAL_INT(0, log_.n_vsync_replies);

    frame_scheduler_unref(s);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_present_when_idle_presents_immediately);
    RUN_TEST(test_present_while_in_flight_is_queued_until_scanout);
    RUN_TEST(test_newer_frame_displaces_queued_frame);
    RUN_TEST(test_vsync_request_replied_immediately_when_idle);
    RUN_TEST(test_vsync_request_deferred_until_scanout);
    RUN_TEST(test_vsync_reply_waits_while_queued_frame_present);
    RUN_TEST(test_present_failure_resets_pipeline);
    RUN_TEST(test_spurious_scanout_is_ignored);
    return UNITY_END();
}
