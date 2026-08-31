// SPDX-License-Identifier: MIT
/*
 * Frame scheduler
 *
 * Manages scheduling of frames, rendering, flutter vsync requests/replies.
 *
 * Copyright (c) 2022, Hannes Winkler <hanneswinkler2000@web.de>
 */

#include "frame_scheduler.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#include "compositor_ng.h"
#include "util/collection.h"
#include "util/lock_ops.h"
#include "util/logging.h"
#include "util/macros.h"
#include "util/refcounting.h"

/**
 * State machine
 * =============
 *
 * The frame scheduler drives an asynchronous, KMS-page-flip-paced presentation
 * pipeline. Its invariants are:
 *
 *  - At most one atomic commit is in flight (submitted to the kernel, page
 *    flip not yet observed) at any time. This is required because a
 *    non-blocking atomic commit returns EBUSY while another commit is still
 *    pending on the same CRTC.
 *    Tracked by @ref frame_scheduler.waiting_for_scanout.
 *
 *  - At most one frame is queued behind the in-flight commit. A newer frame
 *    displaces (cancels) the queued one, similar to Vulkan's MAILBOX present
 *    mode. Tracked by @ref frame_scheduler.has_queued_frame.
 *
 *  - At most one flutter vsync request (baton) is pending. In double-buffered
 *    mode it's replied to on the next page flip that doesn't immediately
 *    present another queued frame, so flutter produces (at most) one new frame
 *    per display refresh, right at the start of the vblank period.
 *
 * All external callbacks (present_cb, cancel_cb, vsync_cb) are invoked WITHOUT
 * the scheduler lock held, so they're free to call back into the scheduler.
 *
 * Frame pacing statistics
 * =======================
 *
 * If the FLUTTERPI_FRAME_PACING_LOG environment variable is set to a positive
 * integer N, the scheduler will log a one-line summary of the observed
 * presentation timing every N seconds. This is cheap (a few counters and a
 * small ring buffer, flushed at scanout time) and intended to be usable in
 * production to diagnose pacing issues without special builds.
 */

#define FRAME_PACING_RING_SIZE 256

struct frame_scheduler {
    refcount_t n_refs;

    bool uses_frame_requests;
    enum present_mode present_mode;
    fl_vsync_callback_t vsync_cb;
    void *userdata;

    pthread_mutex_t mutex;

    /// Duration of one display refresh cycle, in nanoseconds.
    /// Used for the frame-start / next-frame-start timestamps sent to flutter.
    uint64_t frame_interval_ns;

    /// True if an atomic commit is in flight and we're waiting for its
    /// page-flip event.
    bool waiting_for_scanout;

    /// The (single) frame queued behind the in-flight commit, if any.
    bool has_queued_frame;
    struct {
        void_callback_t present_cb;
        void_callback_t cancel_cb;
        void *userdata;
    } queued_frame;

    /// The pending flutter vsync request baton, if any. 0 means none.
    intptr_t pending_vsync_baton;

    struct {
        /// 0 if disabled, otherwise the reporting interval in nanoseconds.
        uint64_t report_interval_ns;
        uint64_t window_start_ns;
        uint64_t last_scanout_ns;
        uint64_t last_present_ns;
        uint32_t n_presented;
        uint32_t n_displaced;
        uint32_t n_scanouts;
        uint32_t n_vsyncs_deferred;
        uint32_t n_intervals;
        uint32_t intervals_us[FRAME_PACING_RING_SIZE];
        uint32_t n_commit_latencies;
        uint64_t commit_latency_sum_us;
        uint32_t commit_latency_max_us;
    } stats;
};

DEFINE_REF_OPS(frame_scheduler, n_refs)
DEFINE_STATIC_LOCK_OPS(frame_scheduler, mutex)

struct frame_scheduler *
frame_scheduler_new(bool uses_frame_requests, enum present_mode present_mode, fl_vsync_callback_t vsync_cb, void *userdata) {
    struct frame_scheduler *scheduler;

    // uses_frame_requests? => vsync_cb != NULL
    assert(!uses_frame_requests || vsync_cb != NULL);

    scheduler = calloc(1, sizeof *scheduler);
    if (scheduler == NULL) {
        return NULL;
    }

    scheduler->n_refs = REFCOUNT_INIT_1;
    scheduler->uses_frame_requests = uses_frame_requests;
    scheduler->present_mode = present_mode;
    scheduler->vsync_cb = vsync_cb;
    scheduler->userdata = userdata;

    pthread_mutex_init(&scheduler->mutex, get_default_mutex_attrs());

    // A sane default. The window will tell us the real refresh rate using
    // frame_scheduler_set_frame_interval once it has selected a mode.
    scheduler->frame_interval_ns = 1000000000 / 60;

    scheduler->waiting_for_scanout = false;
    scheduler->has_queued_frame = false;
    scheduler->pending_vsync_baton = 0;

    const char *pacing_log = getenv("FLUTTERPI_FRAME_PACING_LOG");
    if (pacing_log != NULL && atoi(pacing_log) > 0) {
        scheduler->stats.report_interval_ns = atoi(pacing_log) * 1000000000ull;
        // Deliberately not LOG_DEBUG: the statistics are opt-in via environment
        // variable and meant to be usable in release builds.
        fprintf(stderr, "frame_scheduler.c: Frame pacing statistics will be logged every %d seconds.\n", atoi(pacing_log));
    }

    return scheduler;
}

void frame_scheduler_destroy(struct frame_scheduler *scheduler) {
    // If a queued frame is still present at destruction time, cancel it so its
    // resources are released.
    if (scheduler->has_queued_frame && scheduler->queued_frame.cancel_cb != NULL) {
        scheduler->queued_frame.cancel_cb(scheduler->queued_frame.userdata);
    }
    pthread_mutex_destroy(&scheduler->mutex);
    free(scheduler);
}

bool frame_scheduler_uses_frame_requests(struct frame_scheduler *scheduler) {
    ASSERT_NOT_NULL(scheduler);
    return scheduler->uses_frame_requests;
}

void frame_scheduler_set_frame_interval(struct frame_scheduler *scheduler, uint64_t interval_ns) {
    ASSERT_NOT_NULL(scheduler);
    assert(interval_ns > 0);

    frame_scheduler_lock(scheduler);
    scheduler->frame_interval_ns = interval_ns;
    frame_scheduler_unlock(scheduler);
}

void frame_scheduler_on_fl_vsync_request(struct frame_scheduler *scheduler, intptr_t vsync_baton) {
    intptr_t displaced_baton;
    bool reply_now;

    ASSERT_NOT_NULL(scheduler);
    assert(vsync_baton != 0);
    assert(scheduler->uses_frame_requests);

    displaced_baton = 0;
    reply_now = false;

    frame_scheduler_lock(scheduler);

    if (scheduler->present_mode == kDoubleBufferedVsync_PresentMode && scheduler->waiting_for_scanout) {
        // Defer the reply to the next page flip, so flutter starts producing
        // its next frame right at the beginning of a vblank period, with
        // accurate timestamps.
        //
        // The engine should only ever have a single vsync request pending.
        // If somehow there's already one, reply to the old one right away
        // so it's not lost.
        assert(scheduler->pending_vsync_baton == 0);
        if (scheduler->pending_vsync_baton != 0) {
            displaced_baton = scheduler->pending_vsync_baton;
        }

        scheduler->pending_vsync_baton = vsync_baton;
        scheduler->stats.n_vsyncs_deferred++;
    } else {
        // Triple-buffered mode, or nothing in flight right now: let flutter
        // start rendering immediately.
        reply_now = true;
    }

    frame_scheduler_unlock(scheduler);

    if (displaced_baton != 0) {
        uint64_t now = get_monotonic_time();
        scheduler->vsync_cb(scheduler->userdata, displaced_baton, now, now + scheduler->frame_interval_ns);
    }

    if (reply_now) {
        uint64_t now = get_monotonic_time();
        scheduler->vsync_cb(scheduler->userdata, vsync_baton, now, now + scheduler->frame_interval_ns);
    }
}

void frame_scheduler_present_frame(struct frame_scheduler *scheduler, void_callback_t present_cb, void *userdata, void_callback_t cancel_cb) {
    void_callback_t displaced_cancel_cb;
    void *displaced_userdata;
    bool present_now;

    ASSERT_NOT_NULL(scheduler);
    ASSERT_NOT_NULL(present_cb);

    displaced_cancel_cb = NULL;
    displaced_userdata = NULL;
    present_now = false;

    frame_scheduler_lock(scheduler);

    if (scheduler->waiting_for_scanout) {
        // A commit is already in flight; queue this frame behind it.
        // If there's already a queued frame, the new one displaces it.
        // (Mailbox semantics.)
        if (scheduler->has_queued_frame) {
            displaced_cancel_cb = scheduler->queued_frame.cancel_cb;
            displaced_userdata = scheduler->queued_frame.userdata;
            scheduler->stats.n_displaced++;
        }

        scheduler->has_queued_frame = true;
        scheduler->queued_frame.present_cb = present_cb;
        scheduler->queued_frame.cancel_cb = cancel_cb;
        scheduler->queued_frame.userdata = userdata;
    } else {
        // Nothing in flight; present right away.
        scheduler->waiting_for_scanout = true;
        present_now = true;

        scheduler->stats.n_presented++;
        scheduler->stats.last_present_ns = get_monotonic_time();
    }

    frame_scheduler_unlock(scheduler);

    if (displaced_cancel_cb != NULL) {
        displaced_cancel_cb(displaced_userdata);
    }

    if (present_now) {
        present_cb(userdata);
    }
}

void frame_scheduler_on_present_failed(struct frame_scheduler *scheduler) {
    void_callback_t queued_cancel_cb;
    void *queued_userdata;
    intptr_t pending_baton;

    ASSERT_NOT_NULL(scheduler);

    queued_cancel_cb = NULL;
    queued_userdata = NULL;

    frame_scheduler_lock(scheduler);

    // The commit was never submitted to the kernel, so no page-flip event will
    // arrive. Reset the pipeline so the next frame can be presented, cancel
    // any queued frame (its contents are stale anyway), and reply to a pending
    // vsync request so the engine doesn't stall forever.
    assert(scheduler->waiting_for_scanout);
    scheduler->waiting_for_scanout = false;

    if (scheduler->has_queued_frame) {
        queued_cancel_cb = scheduler->queued_frame.cancel_cb;
        queued_userdata = scheduler->queued_frame.userdata;
        scheduler->has_queued_frame = false;
        memset(&scheduler->queued_frame, 0, sizeof scheduler->queued_frame);
    }

    pending_baton = scheduler->pending_vsync_baton;
    scheduler->pending_vsync_baton = 0;

    frame_scheduler_unlock(scheduler);

    if (queued_cancel_cb != NULL) {
        queued_cancel_cb(queued_userdata);
    }

    if (pending_baton != 0) {
        uint64_t now = get_monotonic_time();
        scheduler->vsync_cb(scheduler->userdata, pending_baton, now, now + scheduler->frame_interval_ns);
    }
}

static int compare_u32(const void *a, const void *b) {
    uint32_t lhs = *(const uint32_t *) a, rhs = *(const uint32_t *) b;
    return lhs < rhs ? -1 : (lhs > rhs ? 1 : 0);
}

static void frame_scheduler_maybe_report_stats_locked(struct frame_scheduler *scheduler, uint64_t now) {
    if (scheduler->stats.report_interval_ns == 0) {
        return;
    }

    if (scheduler->stats.window_start_ns == 0) {
        scheduler->stats.window_start_ns = now;
        return;
    }

    if (now - scheduler->stats.window_start_ns < scheduler->stats.report_interval_ns) {
        return;
    }

    uint32_t n = scheduler->stats.n_intervals;
    uint32_t p50 = 0, p99 = 0, max = 0;
    if (n > 0) {
        qsort(scheduler->stats.intervals_us, n, sizeof(uint32_t), compare_u32);
        p50 = scheduler->stats.intervals_us[n / 2];
        p99 = scheduler->stats.intervals_us[(n * 99) / 100];
        max = scheduler->stats.intervals_us[n - 1];
    }

    uint32_t avg_commit_latency =
        scheduler->stats.n_commit_latencies != 0 ?
            (uint32_t) (scheduler->stats.commit_latency_sum_us / scheduler->stats.n_commit_latencies) :
            0;

    fprintf(
        stderr,
        "frame pacing: window=%.1fs scanouts=%" PRIu32 " presented=%" PRIu32 " displaced=%" PRIu32 " vsyncs_deferred=%" PRIu32
        " flip_interval p50/p99/max=%.1f/%.1f/%.1fms commit->flip avg/max=%.1f/%.1fms\n",
        (now - scheduler->stats.window_start_ns) / 1e9,
        scheduler->stats.n_scanouts,
        scheduler->stats.n_presented,
        scheduler->stats.n_displaced,
        scheduler->stats.n_vsyncs_deferred,
        p50 / 1e3,
        p99 / 1e3,
        max / 1e3,
        avg_commit_latency / 1e3,
        scheduler->stats.commit_latency_max_us / 1e3
    );

    scheduler->stats.window_start_ns = now;
    scheduler->stats.n_presented = 0;
    scheduler->stats.n_displaced = 0;
    scheduler->stats.n_scanouts = 0;
    scheduler->stats.n_vsyncs_deferred = 0;
    scheduler->stats.n_intervals = 0;
    scheduler->stats.n_commit_latencies = 0;
    scheduler->stats.commit_latency_sum_us = 0;
    scheduler->stats.commit_latency_max_us = 0;
}

static void frame_scheduler_record_scanout_stats_locked(struct frame_scheduler *scheduler, uint64_t timestamp_ns) {
    scheduler->stats.n_scanouts++;

    if (scheduler->stats.last_scanout_ns != 0 && timestamp_ns > scheduler->stats.last_scanout_ns) {
        uint64_t interval_us = (timestamp_ns - scheduler->stats.last_scanout_ns) / 1000;
        if (scheduler->stats.n_intervals < FRAME_PACING_RING_SIZE) {
            scheduler->stats.intervals_us[scheduler->stats.n_intervals++] = (uint32_t) MIN2(interval_us, (uint64_t) UINT32_MAX);
        }
    }
    scheduler->stats.last_scanout_ns = timestamp_ns;

    if (scheduler->stats.last_present_ns != 0 && timestamp_ns > scheduler->stats.last_present_ns) {
        uint64_t latency_us = (timestamp_ns - scheduler->stats.last_present_ns) / 1000;
        scheduler->stats.n_commit_latencies++;
        scheduler->stats.commit_latency_sum_us += latency_us;
        if (latency_us > scheduler->stats.commit_latency_max_us) {
            scheduler->stats.commit_latency_max_us = (uint32_t) MIN2(latency_us, (uint64_t) UINT32_MAX);
        }
        scheduler->stats.last_present_ns = 0;
    }
}

void frame_scheduler_on_scanout(struct frame_scheduler *scheduler, bool has_timestamp, uint64_t timestamp_ns) {
    void_callback_t present_cb;
    void *present_userdata;
    intptr_t pending_baton;

    ASSERT_NOT_NULL(scheduler);
    assert(!has_timestamp || timestamp_ns != 0);

    present_cb = NULL;
    present_userdata = NULL;
    pending_baton = 0;

    if (!has_timestamp) {
        timestamp_ns = get_monotonic_time();
    }

    frame_scheduler_lock(scheduler);

    if (!scheduler->waiting_for_scanout) {
        // Not expecting a scanout right now. This can happen if the pipeline
        // was reset by frame_scheduler_on_present_failed after the commit was
        // actually submitted. Ignore.
        frame_scheduler_unlock(scheduler);
        return;
    }

    frame_scheduler_record_scanout_stats_locked(scheduler, timestamp_ns);

    if (scheduler->has_queued_frame) {
        // Present the queued frame right away; we remain in the
        // waiting-for-scanout state. A pending vsync request stays pending;
        // it'll be replied to on a later flip. (Backpressure: with double
        // buffering we don't want flutter to render further ahead.)
        present_cb = scheduler->queued_frame.present_cb;
        present_userdata = scheduler->queued_frame.userdata;
        scheduler->has_queued_frame = false;
        memset(&scheduler->queued_frame, 0, sizeof scheduler->queued_frame);

        scheduler->stats.n_presented++;
        scheduler->stats.last_present_ns = get_monotonic_time();
    } else {
        scheduler->waiting_for_scanout = false;

        pending_baton = scheduler->pending_vsync_baton;
        scheduler->pending_vsync_baton = 0;
    }

    frame_scheduler_maybe_report_stats_locked(scheduler, timestamp_ns);

    frame_scheduler_unlock(scheduler);

    if (pending_baton != 0) {
        // Reply with the actual page-flip timestamp: flutter should begin its
        // frame now (start of this vblank period), targeting the next vblank.
        scheduler->vsync_cb(scheduler->userdata, pending_baton, timestamp_ns, timestamp_ns + scheduler->frame_interval_ns);
    }

    if (present_cb != NULL) {
        present_cb(present_userdata);
    }
}
