// SPDX-License-Identifier: MIT
// Match GTK/Wayland axis-unit policy without real devices or recorded input.
#include "user_input_scroll.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>

int main(void) {
    const double ratios[] = {1, 1.5, 2};
    for (unsigned d = 0; d < sizeof(ratios) / sizeof(ratios[0]); d++) {
        for (int natural = 0; natural <= 1; natural++) {
            for (int axes = 1; axes <= 3; axes++) {
                struct user_input_scroll scroll = { .device_id = 42 };
                const struct user_input_scroll_sample raw = {
                    .timestamp = 1000000, .x = 300, .y = 300,
                    .has_x = axes & 1, .has_y = axes & 2,
                    .delta_x = 2, .delta_y = -2,
                };
                FlutterPointerEvent pan[USER_INPUT_SCROLL_MAX_EVENTS];
                double gtk_pan_x = 0, gtk_pan_y = 0;
                for (int i = 0; i < 10; i++) {
                    struct user_input_scroll_sample sample = raw;
                    sample.timestamp += i * 32000;
                    // Exact representability in wl_fixed avoids rounding ambiguity.
                    const double sign = natural ? 1 : -1;
                    gtk_pan_x += raw.has_x ? sign * raw.delta_x / 10.0 * 53.0 : 0;
                    gtk_pan_y += raw.has_y ? sign * raw.delta_y / 10.0 * 53.0 : 0;
                    user_input_scroll_apply_desktop_units(&sample, ratios[d]);
                    size_t n = user_input_scroll_finger(&scroll, &sample, natural, pan);
                    assert(n == (i == 0 ? 3 : 1));
                    const FlutterPointerEvent *last = &pan[n - 1];
                    assert(last->phase == kPanZoomUpdate);
                    assert(last->timestamp == sample.timestamp);
                    assert(last->x == raw.x && last->y == raw.y);
                    assert(fabs(gtk_pan_x - last->pan_x / ratios[d]) < 1e-9);
                    assert(fabs(gtk_pan_y - last->pan_y / ratios[d]) < 1e-9);
                }
                // End-of-axis sentinels must remain zero, not create new motion.
                struct user_input_scroll_sample stop = raw;
                stop.timestamp += 320000;
                stop.delta_x = stop.delta_y = 0;
                user_input_scroll_apply_desktop_units(&stop, ratios[d]);
                assert(user_input_scroll_finger(&scroll, &stop, natural, pan) == 1);
                assert(pan[0].phase == kPanZoomEnd);
                assert(pan[0].timestamp == stop.timestamp);

                // This policy must not change wheel multipliers or mouse buttons.
                FlutterPointerEvent wheel = user_input_scroll_wheel(&raw, 7, 1);
                assert(wheel.buttons == 1 && wheel.device == 7);
                assert(fabs(wheel.scroll_delta_x - (raw.has_x ? raw.delta_x * 53.0 / 15.0 : 0)) < 1e-9);
                assert(fabs(wheel.scroll_delta_y - (raw.has_y ? raw.delta_y * 53.0 / 15.0 : 0)) < 1e-9);
            }
        }
    }
    puts("PASS: desktop parity at DPR 1/1.5/2, axes/directions, timestamps, stops and unchanged wheels");
}
