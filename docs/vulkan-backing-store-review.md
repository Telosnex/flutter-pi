# Vulkan backing-store exhaustion investigation

## Reported error, 2026-10-06

The Pi is running the Vulkan/Impeller renderer. The journal's generic Flutter
message, `Could not create the embedder backing store`, follows
`compositor_ng.c: Couldn't fill flutter backing store ... Vulkan image`.

This identifies `vk_gbm_render_surface_fill()` as the failing operation, not
surface creation. That function has one failure path: none of its **four
preallocated framebuffer slots** is available. It performs no GPU allocation
there. In Release it returned EIO without explaining why; in Debug it asserted.
The logs alone do not identify which owners occupied the slots.

## Reproduced ownership defects

A new GPU-independent test includes the real Vulkan surface implementation and
uses synthetic image handles. Against the prior implementation, four of five
ownership tests failed:

1. Fill -> engine destruction callback without queue_present leaves the slot
   locked (one framebuffer ref remains).
2. Replacing the front buffer unlocks the old slot even if Flutter has not yet
   called its destruction callback. The extra reference retained by the old
   callback protects the surface allocation, **not** that particular image.
3. Presenting the same store twice consumes another owner's reference: the
   front-buffer pointer can remain set while its slot is unlocked.
4. Repeatedly discarding unpresented stores would exhaust the fixed pool.

The independent KMS-reference test already passed before the change.

The embedder header documents `FlutterVulkanBackingStore.destruction_callback`
as the notification that the engine no longer needs that backing store. The
initial locked_fb reference therefore belongs to Flutter until this callback;
it must not be consumed by queue_present. Front-buffer and KMS references are
separate. These are actual ownership defects; the test does **not** establish
that discarded stores caused the particular intermittent errors in the journal.
The installed engine is customized, so a review of upstream engine source is
not proof of its exact runtime callback ordering.

## Local candidate (not deployed)

- Supply the locked framebuffer as the Vulkan destruction-callback baton and
  release its reference from that callback. Its reference already protects the
  containing surface, so no duplicate surface reference is needed.
- queue_present swaps the front-buffer reference but no longer consumes
  Flutter's reference. Stores can be dropped or re-presented safely.
- On exhaustion, return EAGAIN instead of asserting/misreporting EIO; log the
  pool dimensions and each slot's refcount/Flutter-owner/front-buffer status.
  Counts are diagnostic snapshots and may change during concurrent retirement.
- Preserve the actual errno and requested dimensions in the compositor log.

No pool-size increase, wait loop, forced buffer reuse, semaphore/KMS scheduling
change, Flutter engine change, or WebKit change is included.

All six Vulkan tests pass, including bounded pool exhaustion/recovery. All nine
CTest executables pass in Release and Debug, both with ENABLE_VULKAN=ON. All five
lifecycle-related executables (including this new test) pass ASan/UBSan with
leak detection. These tests do not execute real Vulkan or KMS operations.

The running Pi binary remains the lifecycle test build, SHA-256
`c5cd2079d1358dd2ffdd49803ce6f37b870915a472ede15873dadbd3052af053`.
Before declaring the warning resolved, test the candidate on the actual app
with webview opening/closing and inspect any new pool-exhaustion snapshots.
Temporary pressure from engine/KMS owners may remain even after fixing the
ownership defects. If it does, use the snapshots to choose a bounded buffering
or pacing change rather than blindly increasing the pool.


## Superseded by the broader audit/fix

The local candidate above was subsequently extended to fix the full audit's
confirmed lifetime, import, per-layer identity, resize and synchronization-error
findings. See `vulkan-renderer-audit.md` for original red evidence and current
green results. Do not treat this earlier six-test report as the deployment gate.
