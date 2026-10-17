// SPDX-License-Identifier: MIT
/*
 * Display wait
 *
 * Lets flutter-pi wait for a display to be connected, instead of exiting when
 * a GPU is present but no connector is connected (for example, a closed lid or
 * a monitor that is off). The wait uses a udev monitor on the DRM subsystem,
 * plus a slow timer for drivers that send no hotplug events.
 */

#ifndef _FLUTTERPI_SRC_DISPLAY_WAIT_H
#define _FLUTTERPI_SRC_DISPLAY_WAIT_H

#include <stdbool.h>

/// The interval of the backup timer, for drivers that send no hotplug events.
#define DISPLAY_WAIT_RESCAN_INTERVAL_MS 30000

enum drm_scan_result {
    /// A device with a connected display was found.
    kDisplayFound_DrmScanResult,

    /// At least one device with a primary node could be opened, but none has a
    /// connected display. Waiting can help.
    kNoDisplay_DrmScanResult,

    /// No device with a primary node could be opened. Waiting does not help.
    kNoDevice_DrmScanResult,
};

/**
 * @brief Classifies the result of a scan of the DRM devices.
 *
 * @param n_opened    The number of devices with a primary node that could be opened.
 * @param n_connected The number of those devices that have a connected connector.
 */
enum drm_scan_result drm_scan_classify(int n_opened, int n_connected);

/**
 * @brief Whether a udev event can mean that a DRM connector changed state.
 *
 * The kernel sends a "change" event with HOTPLUG=1 on the DRM device when a
 * connector is connected or disconnected. "add" covers a DRM device that
 * appears after startup (for example, a late driver probe).
 *
 * All arguments can be NULL.
 */
bool display_wait_is_drm_hotplug(const char *subsystem, const char *action, const char *hotplug);

struct display_wait;

/**
 * @brief Starts to listen for DRM hotplug events.
 *
 * Create this before the scan that it guards, so that an event between the scan
 * and the wait is not lost.
 *
 * Returns NULL only if out of memory. If udev is not available, the wait uses
 * only the timer.
 */
struct display_wait *display_wait_new(void);

void display_wait_destroy(struct display_wait *wait);

/**
 * @brief Whether the wait receives hotplug events (false: timer only).
 */
bool display_wait_has_hotplug_events(const struct display_wait *wait);

/**
 * @brief Blocks until a DRM hotplug event or until @p timeout_ms passes.
 *
 * @returns 1 for a hotplug event, 0 for a timeout, or a negative errno value.
 */
int display_wait_for_hotplug(struct display_wait *wait, int timeout_ms);

#endif  // _FLUTTERPI_SRC_DISPLAY_WAIT_H
