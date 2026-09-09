// SPDX-License-Identifier: MIT
#include "user_input_scroll.h"

#include <string.h>

bool user_input_scroll_parse_natural(const char *value, bool *natural) {
    if (value == NULL || strcmp(value, "1") == 0) {
        *natural = true;
        return true;
    }
    if (strcmp(value, "0") == 0) {
        *natural = false;
        return true;
    }
    return false;
}

static FlutterPointerEvent trackpad_event(const struct user_input_scroll *scroll, FlutterPointerPhase phase, uint64_t timestamp) {
    return (FlutterPointerEvent){
        .struct_size = sizeof(FlutterPointerEvent),
        .phase = phase,
        .timestamp = timestamp,
        .x = scroll->x,
        .y = scroll->y,
        .device = scroll->device_id,
        .signal_kind = kFlutterPointerSignalKindNone,
        .device_kind = kFlutterPointerDeviceKindTrackpad,
        .pan_x = scroll->pan_x,
        .pan_y = scroll->pan_y,
        .scale = 1.0,
    };
}

size_t user_input_scroll_finger(
    struct user_input_scroll *scroll,
    const struct user_input_scroll_sample *sample,
    bool natural,
    FlutterPointerEvent events[USER_INPUT_SCROLL_MAX_EVENTS]
) {
    double dx = sample->has_x ? sample->delta_x : 0;
    double dy = sample->has_y ? sample->delta_y : 0;
    size_t count = 0;

    if (!scroll->active) {
        // Orphan/duplicate stop events must not create a gesture or a fling.
        if (dx == 0 && dy == 0) {
            return 0;
        }
        scroll->x = sample->x;
        scroll->y = sample->y;
        scroll->pan_x = scroll->pan_y = 0;
        scroll->x_active = scroll->y_active = false;
        if (!scroll->added) {
            events[count++] = trackpad_event(scroll, kAdd, sample->timestamp);
            scroll->added = true;
        }
        events[count++] = trackpad_event(scroll, kPanZoomStart, sample->timestamp);
        scroll->active = true;
    }

    if (sample->has_x) {
        scroll->x_active = dx != 0;
    }
    if (sample->has_y) {
        scroll->y_active = dy != 0;
    }

    if (dx != 0 || dy != 0) {
        // Pan describes content/finger displacement, not scroll-offset changes.
        // Finger units already match relative pointer pixels; the wheel's
        // degrees-to-pixels multiplier must NOT be applied here.
        scroll->pan_x += natural ? dx : -dx;
        scroll->pan_y += natural ? dy : -dy;
        events[count++] = trackpad_event(scroll, kPanZoomUpdate, sample->timestamp);
    }

    if (!scroll->x_active && !scroll->y_active) {
        // End only after every participating axis has stopped. Do not inject a
        // zero-motion Update: it would contaminate Flutter's velocity samples.
        events[count++] = trackpad_event(scroll, kPanZoomEnd, sample->timestamp);
        scroll->active = false;
    }
    return count;
}

size_t user_input_scroll_remove(
    struct user_input_scroll *scroll,
    uint64_t timestamp,
    FlutterPointerEvent events[USER_INPUT_SCROLL_MAX_EVENTS]
) {
    size_t count = 0;
    if (scroll->active) {
        events[count++] = trackpad_event(scroll, kPanZoomEnd, timestamp);
    }
    if (scroll->added) {
        events[count++] = trackpad_event(scroll, kRemove, timestamp);
    }
    *scroll = (struct user_input_scroll){ .device_id = scroll->device_id };
    return count;
}

FlutterPointerEvent user_input_scroll_wheel(const struct user_input_scroll_sample *sample, int64_t device_id, int64_t buttons) {
    return (FlutterPointerEvent){
        .struct_size = sizeof(FlutterPointerEvent),
        .phase = buttons & kFlutterPointerButtonMousePrimary ? kMove : kHover,
        .timestamp = sample->timestamp,
        .x = sample->x,
        .y = sample->y,
        .device = device_id,
        .signal_kind = kFlutterPointerSignalKindScroll,
        .scroll_delta_x = sample->has_x ? sample->delta_x / 15.0 * 53.0 : 0,
        .scroll_delta_y = sample->has_y ? sample->delta_y / 15.0 * 53.0 : 0,
        .device_kind = kFlutterPointerDeviceKindMouse,
        .buttons = buttons,
    };
}
