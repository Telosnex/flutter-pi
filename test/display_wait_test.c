// SPDX-License-Identifier: MIT
/*
 * Tests for the --wait-for-display decisions (no DRM hardware needed).
 */

#include "display_wait.h"

#include <unity.h>

void setUp(void) {
}

void tearDown(void) {
}

static void test_classify_no_devices(void) {
    TEST_ASSERT_EQUAL_INT(kNoDevice_DrmScanResult, drm_scan_classify(0, 0));
}

static void test_classify_devices_without_display(void) {
    // A closed lid: the GPU opens, but no connector is connected.
    TEST_ASSERT_EQUAL_INT(kNoDisplay_DrmScanResult, drm_scan_classify(1, 0));
    TEST_ASSERT_EQUAL_INT(kNoDisplay_DrmScanResult, drm_scan_classify(2, 0));
}

static void test_classify_display_present(void) {
    TEST_ASSERT_EQUAL_INT(kDisplayFound_DrmScanResult, drm_scan_classify(1, 1));
    TEST_ASSERT_EQUAL_INT(kDisplayFound_DrmScanResult, drm_scan_classify(2, 1));
}

static void test_hotplug_change_event(void) {
    TEST_ASSERT_TRUE(display_wait_is_drm_hotplug("drm", "change", "1"));
}

static void test_change_without_hotplug_is_ignored(void) {
    // Other DRM "change" events (for example, a lease change) don't mean a connector changed.
    TEST_ASSERT_FALSE(display_wait_is_drm_hotplug("drm", "change", NULL));
    TEST_ASSERT_FALSE(display_wait_is_drm_hotplug("drm", "change", "0"));
}

static void test_new_drm_device_counts(void) {
    TEST_ASSERT_TRUE(display_wait_is_drm_hotplug("drm", "add", NULL));
}

static void test_other_events_are_ignored(void) {
    TEST_ASSERT_FALSE(display_wait_is_drm_hotplug("drm", "remove", NULL));
    TEST_ASSERT_FALSE(display_wait_is_drm_hotplug("input", "change", "1"));
    TEST_ASSERT_FALSE(display_wait_is_drm_hotplug(NULL, "change", "1"));
    TEST_ASSERT_FALSE(display_wait_is_drm_hotplug("drm", NULL, "1"));
}

static void test_wait_times_out(void) {
    struct display_wait *wait = display_wait_new();
    TEST_ASSERT_NOT_NULL(wait);

    // On a build host, a DRM hotplug in this window is very unlikely; either
    // result is valid, but the call must return and must not report an error.
    int result = display_wait_for_hotplug(wait, 50);
    TEST_ASSERT_TRUE(result == 0 || result == 1);

    display_wait_destroy(wait);
}

int main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_classify_no_devices);
    RUN_TEST(test_classify_devices_without_display);
    RUN_TEST(test_classify_display_present);
    RUN_TEST(test_hotplug_change_event);
    RUN_TEST(test_change_without_hotplug_is_ignored);
    RUN_TEST(test_new_drm_device_counts);
    RUN_TEST(test_other_events_are_ignored);
    RUN_TEST(test_wait_times_out);

    return UNITY_END();
}
