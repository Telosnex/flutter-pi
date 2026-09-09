// SPDX-License-Identifier: MIT
#ifndef FLUTTERPI_USER_INPUT_SCROLL_H
#define FLUTTERPI_USER_INPUT_SCROLL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <flutter_embedder.h>

// A libinput axis sample. Missing axes are not stop events; present zero axes are.
// Finger deltas must be normalized to libinput's non-natural (finger motion)
// direction before passing them here. Coordinates/deltas are Flutter view pixels.
struct user_input_scroll_sample {
    uint64_t timestamp;  // Monotonic microseconds, not milliseconds.
    double x, y;
    bool has_x, has_y;
    double delta_x, delta_y;
};

// One state per physical pointer device. The gesture device ID must be distinct
// from the shared mouse cursor (which may be down or moving during a gesture).
struct user_input_scroll {
    int64_t device_id;
    bool added, active, x_active, y_active;
    double x, y, pan_x, pan_y;
};

#define USER_INPUT_SCROLL_MAX_EVENTS 3

// NULL defaults to natural scrolling. Only "0" and "1" are valid overrides.
bool user_input_scroll_parse_natural(const char *value, bool *natural);

// Returns up to Add + PanZoomStart + PanZoomUpdate. Pan is cumulative from the
// start, at a fixed cursor anchor; Flutter owns velocity estimation/inertia.
size_t user_input_scroll_finger(
    struct user_input_scroll *scroll,
    const struct user_input_scroll_sample *sample,
    bool natural,
    FlutterPointerEvent events[USER_INPUT_SCROLL_MAX_EVENTS]
);

// End any open gesture before removing its device, including on seat suspend.
size_t user_input_scroll_remove(
    struct user_input_scroll *scroll,
    uint64_t timestamp,
    FlutterPointerEvent events[USER_INPUT_SCROLL_MAX_EVENTS]
);

// Retains the existing mouse-wheel/continuous-scroll conversion and direction.
FlutterPointerEvent user_input_scroll_wheel(const struct user_input_scroll_sample *sample, int64_t device_id, int64_t buttons);

#endif
