// SPDX-License-Identifier: MIT
/*
 * Display wait
 *
 * See display_wait.h.
 */

#include "display_wait.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <libudev.h>

#include "util/logging.h"

struct display_wait {
    struct udev *udev;
    struct udev_monitor *monitor;
    int fd;
};

enum drm_scan_result drm_scan_classify(int n_opened, int n_connected) {
    if (n_connected > 0) {
        return kDisplayFound_DrmScanResult;
    } else if (n_opened > 0) {
        return kNoDisplay_DrmScanResult;
    } else {
        return kNoDevice_DrmScanResult;
    }
}

bool display_wait_is_drm_hotplug(const char *subsystem, const char *action, const char *hotplug) {
    if (subsystem == NULL || strcmp(subsystem, "drm") != 0 || action == NULL) {
        return false;
    }

    if (strcmp(action, "add") == 0) {
        return true;
    }

    return strcmp(action, "change") == 0 && hotplug != NULL && strcmp(hotplug, "1") == 0;
}

struct display_wait *display_wait_new(void) {
    struct display_wait *wait;
    int ok;

    wait = calloc(1, sizeof *wait);
    if (wait == NULL) {
        return NULL;
    }

    wait->fd = -1;

    wait->udev = udev_new();
    if (wait->udev == NULL) {
        LOG_ERROR("Couldn't create a udev context.\n");
        return wait;
    }

    wait->monitor = udev_monitor_new_from_netlink(wait->udev, "udev");
    if (wait->monitor == NULL) {
        LOG_ERROR("Couldn't create a udev monitor.\n");
        goto fail_unref_udev;
    }

    ok = udev_monitor_filter_add_match_subsystem_devtype(wait->monitor, "drm", NULL);
    if (ok < 0) {
        LOG_ERROR("Couldn't filter the udev monitor for DRM events: %s\n", strerror(-ok));
        goto fail_unref_monitor;
    }

    ok = udev_monitor_enable_receiving(wait->monitor);
    if (ok < 0) {
        LOG_ERROR("Couldn't enable the udev monitor: %s\n", strerror(-ok));
        goto fail_unref_monitor;
    }

    wait->fd = udev_monitor_get_fd(wait->monitor);
    return wait;

fail_unref_monitor:
    udev_monitor_unref(wait->monitor);
    wait->monitor = NULL;

fail_unref_udev:
    udev_unref(wait->udev);
    wait->udev = NULL;
    return wait;
}

void display_wait_destroy(struct display_wait *wait) {
    if (wait == NULL) {
        return;
    }

    if (wait->monitor != NULL) {
        udev_monitor_unref(wait->monitor);
    }
    if (wait->udev != NULL) {
        udev_unref(wait->udev);
    }
    free(wait);
}

bool display_wait_has_hotplug_events(const struct display_wait *wait) {
    return wait != NULL && wait->fd >= 0;
}

static int64_t monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/// Reads all queued events. Returns true if any of them is a DRM hotplug.
static bool drain_events(struct display_wait *wait) {
    struct udev_device *device;
    bool hotplug = false;

    while ((device = udev_monitor_receive_device(wait->monitor)) != NULL) {
        if (display_wait_is_drm_hotplug(
                udev_device_get_subsystem(device),
                udev_device_get_action(device),
                udev_device_get_property_value(device, "HOTPLUG")
            )) {
            hotplug = true;
        }
        udev_device_unref(device);
    }

    return hotplug;
}

int display_wait_for_hotplug(struct display_wait *wait, int timeout_ms) {
    int64_t deadline = monotonic_ms() + timeout_ms;

    for (;;) {
        int64_t remaining = deadline - monotonic_ms();
        if (remaining <= 0) {
            return 0;
        }

        struct pollfd pfd = { .fd = wait->fd, .events = POLLIN };
        int ok = poll(&pfd, wait->fd >= 0 ? 1 : 0, (int) remaining);
        if (ok < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -errno;
        } else if (ok == 0) {
            return 0;
        }

        if (drain_events(wait)) {
            return 1;
        }
    }
}
