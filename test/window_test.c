#include <stdbool.h>
#include <stddef.h>

#include <unity.h>
#include <window.h>

void setUp() {
}

void tearDown() {
}

static drmModeModeInfo mode(uint16_t width, uint16_t height, uint32_t nominal_refresh, uint32_t millihertz, uint32_t type, uint32_t flags) {
    return (drmModeModeInfo){
        .clock = millihertz,
        .hdisplay = width,
        .htotal = 1000,
        .vdisplay = height,
        .vtotal = 1000,
        .vrefresh = nominal_refresh,
        .type = type,
        .flags = flags,
    };
}

static drmModeModeInfo modes[6];

static void reset_modes() {
    modes[0] = mode(1280, 720, 60, 60000, 0, 0);
    modes[1] = mode(1920, 1080, 60, 60000, DRM_MODE_TYPE_PREFERRED, 0);
    modes[2] = mode(1920, 1080, 100, 100000, 0, 0);
    modes[3] = mode(1920, 1080, 120, 120187, 0, 0);
    modes[4] = mode(1920, 1080, 144, 144000, 0, 0);
    modes[5] = mode(1920, 1080, 240, 240000, 0, DRM_MODE_FLAG_INTERLACE);
}

static void assert_selection(const char *request, size_t expected_index, bool expected_match) {
    bool matched = false;
    drmModeModeInfo *selected = window_select_videomode(modes, sizeof(modes) / sizeof(modes[0]), request, &matched);

    TEST_ASSERT_EQUAL_PTR(&modes[expected_index], selected);
    TEST_ASSERT_EQUAL(expected_match, matched);
}

void test_no_request_uses_exact_preferred_timing() {
    reset_modes();
    assert_selection(NULL, 1, true);
    assert_selection("preferred", 1, true);
}

void test_preferred_max_uses_highest_progressive_refresh() {
    reset_modes();
    assert_selection("preferred@max", 4, true);
}

void test_preferred_numeric_refresh_is_a_cap() {
    reset_modes();
    assert_selection("preferred@120", 3, true);
    assert_selection("preferred@110", 2, true);
    assert_selection("preferred@75", 1, true);
}

void test_preferred_cap_falls_back_when_no_mode_is_below_it() {
    reset_modes();
    assert_selection("preferred@50", 1, false);
}

void test_invalid_preferred_refresh_falls_back() {
    reset_modes();
    assert_selection("preferred@fast", 1, false);
    assert_selection("preferred@0", 1, false);
}

void test_explicit_modes_keep_existing_matching_behavior() {
    reset_modes();
    assert_selection("1920x1080", 5, true);
    assert_selection("1920x1080@120", 3, true);
    assert_selection("1024x600", 1, false);
}

void test_missing_preferred_mode_uses_largest_fallback_resolution() {
    reset_modes();
    modes[1].type = 0;
    assert_selection("preferred", 5, true);
    assert_selection("preferred@120", 3, true);
}

int main() {
    UNITY_BEGIN();

    RUN_TEST(test_no_request_uses_exact_preferred_timing);
    RUN_TEST(test_preferred_max_uses_highest_progressive_refresh);
    RUN_TEST(test_preferred_numeric_refresh_is_a_cap);
    RUN_TEST(test_preferred_cap_falls_back_when_no_mode_is_below_it);
    RUN_TEST(test_invalid_preferred_refresh_falls_back);
    RUN_TEST(test_explicit_modes_keep_existing_matching_behavior);
    RUN_TEST(test_missing_preferred_mode_uses_largest_fallback_resolution);

    return UNITY_END();
}
