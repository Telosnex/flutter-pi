// SPDX-License-Identifier: MIT
#include "user_input_scroll.h"

#include <unity.h>

static struct user_input_scroll scroll;
static FlutterPointerEvent events[USER_INPUT_SCROLL_MAX_EVENTS];

void setUp(void) {
    scroll = (struct user_input_scroll){ .device_id = 42 };
}
void tearDown(void) {
}

static size_t sample(bool has_x, double dx, bool has_y, double dy, uint64_t time, bool natural) {
    struct user_input_scroll_sample sample = {
        .timestamp = time, .x = 250, .y = 350, .has_x = has_x, .has_y = has_y, .delta_x = dx, .delta_y = dy
    };
    return user_input_scroll_finger(&scroll, &sample, natural, events);
}

static void assert_event(size_t index, FlutterPointerPhase phase, uint64_t time, double pan_x, double pan_y) {
    FlutterPointerEvent *event = &events[index];
    TEST_ASSERT_EQUAL(sizeof(*event), event->struct_size);
    TEST_ASSERT_EQUAL(phase, event->phase);
    TEST_ASSERT_EQUAL_UINT64(time, event->timestamp);
    TEST_ASSERT_EQUAL_INT64(42, event->device);
    TEST_ASSERT_EQUAL(kFlutterPointerDeviceKindTrackpad, event->device_kind);
    TEST_ASSERT_EQUAL(kFlutterPointerSignalKindNone, event->signal_kind);
    TEST_ASSERT_EQUAL_INT64(0, event->buttons);
    TEST_ASSERT_EQUAL_DOUBLE(250, event->x);
    TEST_ASSERT_EQUAL_DOUBLE(350, event->y);
    TEST_ASSERT_EQUAL_DOUBLE(pan_x, event->pan_x);
    TEST_ASSERT_EQUAL_DOUBLE(pan_y, event->pan_y);
    TEST_ASSERT_EQUAL_DOUBLE(1, event->scale);
    TEST_ASSERT_EQUAL_DOUBLE(0, event->rotation);
    TEST_ASSERT_EQUAL_DOUBLE(0, event->scroll_delta_x);
    TEST_ASSERT_EQUAL_DOUBLE(0, event->scroll_delta_y);
}

static void test_natural_preference(void) {
    bool natural = false;
    TEST_ASSERT_TRUE(user_input_scroll_parse_natural(NULL, &natural));
    TEST_ASSERT_TRUE(natural);
    TEST_ASSERT_TRUE(user_input_scroll_parse_natural("0", &natural));
    TEST_ASSERT_FALSE(natural);
    TEST_ASSERT_TRUE(user_input_scroll_parse_natural("1", &natural));
    TEST_ASSERT_TRUE(natural);
    TEST_ASSERT_FALSE(user_input_scroll_parse_natural("yes", &natural));
    TEST_ASSERT_FALSE(user_input_scroll_parse_natural("", &natural));
    TEST_ASSERT_TRUE(natural);  // Invalid settings leave the default unchanged.
}

static void test_first_motion_adds_starts_and_updates_without_losing_delta(void) {
    TEST_ASSERT_EQUAL(3, sample(true, 1.25, true, 2.5, 1000000001, true));
    assert_event(0, kAdd, 1000000001, 0, 0);
    assert_event(1, kPanZoomStart, 1000000001, 0, 0);
    assert_event(2, kPanZoomUpdate, 1000000001, 1.25, 2.5);
}

static void test_pan_is_cumulative_and_natural_in_both_directions(void) {
    sample(true, 4, true, 8, 1000, true);
    TEST_ASSERT_EQUAL(1, sample(true, -1.5, true, -10, 9000, true));
    assert_event(0, kPanZoomUpdate, 9000, 2.5, -2);
}

static void test_traditional_reverses_both_axes_only_once(void) {
    TEST_ASSERT_EQUAL(3, sample(true, 4, true, -8, 1000, false));
    assert_event(2, kPanZoomUpdate, 1000, -4, 8);
}

static void test_vertical_only_stop_ends_without_zero_velocity_update(void) {
    sample(false, 0, true, 12, 1000, true);
    TEST_ASSERT_EQUAL(1, sample(false, 0, true, 0, 9000, true));
    assert_event(0, kPanZoomEnd, 9000, 0, 12);
    TEST_ASSERT_FALSE(scroll.active);
}

static void test_horizontal_only_stop(void) {
    sample(true, 12, false, 0, 1000, true);
    TEST_ASSERT_EQUAL(1, sample(true, 0, false, 0, 9000, true));
    assert_event(0, kPanZoomEnd, 9000, 12, 0);
}

static void test_missing_axis_is_not_a_stop(void) {
    sample(true, 4, true, 8, 1000, true);
    TEST_ASSERT_EQUAL(1, sample(true, 3, false, 0, 9000, true));
    assert_event(0, kPanZoomUpdate, 9000, 7, 8);
    TEST_ASSERT_EQUAL(0, sample(true, 0, false, 0, 17000, true));
    TEST_ASSERT_TRUE(scroll.active);
    TEST_ASSERT_EQUAL(1, sample(false, 0, true, 0, 25000, true));
    assert_event(0, kPanZoomEnd, 25000, 7, 8);
}

static void test_axis_stop_can_accompany_other_axis_motion(void) {
    sample(true, 4, true, 8, 1000, true);
    TEST_ASSERT_EQUAL(1, sample(true, 0, true, 9, 9000, true));
    assert_event(0, kPanZoomUpdate, 9000, 4, 17);
    TEST_ASSERT_EQUAL(1, sample(false, 0, true, 0, 17000, true));
    assert_event(0, kPanZoomEnd, 17000, 4, 17);
}

static void test_axis_can_restart_before_gesture_ends(void) {
    sample(true, 4, true, 8, 1000, true);
    sample(true, 0, false, 0, 9000, true);
    sample(true, 2, false, 0, 17000, true);
    TEST_ASSERT_EQUAL(0, sample(false, 0, true, 0, 25000, true));
    TEST_ASSERT_EQUAL(1, sample(true, 0, false, 0, 33000, true));
    assert_event(0, kPanZoomEnd, 33000, 6, 8);
}

static void test_orphan_duplicate_and_empty_stops_are_ignored(void) {
    TEST_ASSERT_EQUAL(0, sample(true, 0, true, 0, 1000, true));
    TEST_ASSERT_EQUAL(0, sample(false, 999, false, 999, 2000, true));
    TEST_ASSERT_FALSE(scroll.added);
    sample(true, 1, true, 2, 3000, true);
    TEST_ASSERT_EQUAL(0, sample(false, 0, false, 0, 4000, true));
    TEST_ASSERT_TRUE(scroll.active);
    TEST_ASSERT_EQUAL(1, sample(true, 0, true, 0, 5000, true));
    TEST_ASSERT_EQUAL(0, sample(true, 0, true, 0, 6000, true));
}

static void test_new_gesture_resets_pan_and_keeps_device(void) {
    sample(true, 4, true, 8, 1000, true);
    sample(true, 0, true, 0, 9000, true);
    TEST_ASSERT_EQUAL(2, sample(true, 2, true, 3, 17000, true));
    assert_event(0, kPanZoomStart, 17000, 0, 0);
    assert_event(1, kPanZoomUpdate, 17000, 2, 3);
}

static void test_anchor_does_not_follow_shared_mouse_during_pan(void) {
    sample(true, 4, true, 8, 1000, true);
    struct user_input_scroll_sample moved = {
        .timestamp = 9000, .x = 800, .y = 900, .has_y = true, .delta_y = 1
    };
    TEST_ASSERT_EQUAL(1, user_input_scroll_finger(&scroll, &moved, true, events));
    assert_event(0, kPanZoomUpdate, 9000, 4, 9);
    sample(true, 0, true, 0, 17000, true);
    moved.timestamp = 25000;
    TEST_ASSERT_EQUAL(2, user_input_scroll_finger(&scroll, &moved, true, events));
    TEST_ASSERT_EQUAL_DOUBLE(800, events[0].x);
    TEST_ASSERT_EQUAL_DOUBLE(900, events[0].y);
}

static void test_devices_do_not_share_pan_state(void) {
    sample(true, 4, true, 8, 1000, true);
    struct user_input_scroll second = { .device_id = 99 };
    struct user_input_scroll_sample next = { .timestamp = 2000, .has_y = true, .delta_y = 6 };
    TEST_ASSERT_EQUAL(3, user_input_scroll_finger(&second, &next, true, events));
    TEST_ASSERT_EQUAL_INT64(99, events[2].device);
    TEST_ASSERT_EQUAL_DOUBLE(6, events[2].pan_y);
    TEST_ASSERT_EQUAL(1, sample(true, 0, true, 0, 3000, true));
    assert_event(0, kPanZoomEnd, 3000, 4, 8);
    TEST_ASSERT_TRUE(second.active);
}

static void test_remove_or_suspend_closes_gesture_before_remove(void) {
    sample(true, 4, true, 8, 1000, true);
    TEST_ASSERT_EQUAL(2, user_input_scroll_remove(&scroll, 9000, events));
    assert_event(0, kPanZoomEnd, 9000, 4, 8);
    assert_event(1, kRemove, 9000, 4, 8);
    TEST_ASSERT_FALSE(scroll.added);
    TEST_ASSERT_FALSE(scroll.active);
    TEST_ASSERT_EQUAL(0, user_input_scroll_remove(&scroll, 17000, events));
    TEST_ASSERT_EQUAL(3, sample(true, 1, true, 2, 25000, true));
}

static void test_remove_idle_device_does_not_generate_an_end(void) {
    TEST_ASSERT_EQUAL(0, user_input_scroll_remove(&scroll, 1000, events));
    sample(true, 4, true, 8, 2000, true);
    sample(true, 0, true, 0, 3000, true);
    TEST_ASSERT_EQUAL(1, user_input_scroll_remove(&scroll, 4000, events));
    assert_event(0, kRemove, 4000, 4, 8);
}

static void test_wheels_keep_mouse_identity_units_direction_and_buttons(void) {
    sample(true, 4, true, 8, 1000, true);
    struct user_input_scroll_sample wheel = {
        .timestamp = 2000, .x = 100, .y = 200, .has_x = true, .has_y = true, .delta_x = -15, .delta_y = 15
    };
    FlutterPointerEvent event = user_input_scroll_wheel(&wheel, 7, kFlutterPointerButtonMousePrimary);
    TEST_ASSERT_EQUAL(kFlutterPointerDeviceKindMouse, event.device_kind);
    TEST_ASSERT_EQUAL(kFlutterPointerSignalKindScroll, event.signal_kind);
    TEST_ASSERT_EQUAL(kMove, event.phase);
    TEST_ASSERT_EQUAL_INT64(7, event.device);
    TEST_ASSERT_EQUAL_INT64(kFlutterPointerButtonMousePrimary, event.buttons);
    TEST_ASSERT_EQUAL_UINT64(2000, event.timestamp);
    TEST_ASSERT_EQUAL_DOUBLE(-53, event.scroll_delta_x);
    TEST_ASSERT_EQUAL_DOUBLE(53, event.scroll_delta_y);
    TEST_ASSERT_EQUAL_DOUBLE(0, event.pan_x);
    TEST_ASSERT_EQUAL_DOUBLE(0, event.pan_y);
    TEST_ASSERT_TRUE(scroll.active);
    TEST_ASSERT_EQUAL_DOUBLE(8, scroll.pan_y);

    wheel.has_x = false;
    wheel.delta_y = 7.5;
    event = user_input_scroll_wheel(&wheel, 7, 0);
    TEST_ASSERT_EQUAL(kHover, event.phase);
    TEST_ASSERT_EQUAL_DOUBLE(0, event.scroll_delta_x);
    TEST_ASSERT_EQUAL_DOUBLE(26.5, event.scroll_delta_y);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_natural_preference);
    RUN_TEST(test_first_motion_adds_starts_and_updates_without_losing_delta);
    RUN_TEST(test_pan_is_cumulative_and_natural_in_both_directions);
    RUN_TEST(test_traditional_reverses_both_axes_only_once);
    RUN_TEST(test_vertical_only_stop_ends_without_zero_velocity_update);
    RUN_TEST(test_horizontal_only_stop);
    RUN_TEST(test_missing_axis_is_not_a_stop);
    RUN_TEST(test_axis_stop_can_accompany_other_axis_motion);
    RUN_TEST(test_axis_can_restart_before_gesture_ends);
    RUN_TEST(test_orphan_duplicate_and_empty_stops_are_ignored);
    RUN_TEST(test_new_gesture_resets_pan_and_keeps_device);
    RUN_TEST(test_anchor_does_not_follow_shared_mouse_during_pan);
    RUN_TEST(test_devices_do_not_share_pan_state);
    RUN_TEST(test_remove_or_suspend_closes_gesture_before_remove);
    RUN_TEST(test_remove_idle_device_does_not_generate_an_end);
    RUN_TEST(test_wheels_keep_mouse_identity_units_direction_and_buttons);
    return UNITY_END();
}
