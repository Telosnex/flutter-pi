# Vulkan renderer audit — 2026-10-06

## Latest status: first on-device soak (2026-10-20) — partial, not passed

Runner `telosnex/2026-10-20-e3b24ec` on the CM5 (`raspberrypi`), Debian
`vulkan-validationlayers` 1.4.309 with `VK_LAYER_KHRONOS_validation` and
`VK_VALIDATION_VALIDATE_SYNC=1` set through a temporary `zz-experiment.conf`
(removed after the run). One process (PID 4053157) ran 11 h 56 min. About 40
minutes of it (19:21–20:00) were interactive use, including one visible WPE
webview opened at 19:26:20 and closed at 19:41:07.

| Check | Result |
| --- | --- |
| Runner restarts | 0 |
| Validation threading errors (shared queue) | 0 |
| `VK_ERROR_*`, device lost, fence timeouts | 0 |
| Other validation messages | 20 at startup only, then none: `SYNC-HAZARD-WRITE-AFTER-WRITE` (render-pass depth clear, mipmap `vkCmdBlitImage`) and `SYNC-HAZARD-READ-AFTER-WRITE` (`vkCmdCopyImageToBuffer` readback), each after a layout-transition barrier with the wrong stage. flutter-pi records none of these commands, so they come from the engine. |
| Surface-pool EAGAIN | **14**, all 19:26:20–19:26:40, when the visible webview opened |
| WPE frames | **Unknown count; nothing visible.** `webview_direct.c` scanned out at least one WebKit buffer on a plane (`Plane path live: presenting 1920x1125 at (0,0)`). The GPU copy path stayed unused (`destroyed after 0 frames, ... 0 source imports`; that counter covers only copied frames). Per-frame counts need `FLUTTERPI_WEBVIEW_STATS`, which was not set. The webview area showed nothing on screen. |

Pool dump at the first EAGAIN (1920x1200, 6 slots): every slot has
`flutter=0`. Only slot 4 is `front=1` (refs=3). Slot 2 has refs=2, and slots 0,
1, 3 and 5 have refs=1. So five slots are neither rendered by Flutter nor shown,
but are still referenced: pending present, a KMS callback that has not released
them, or a missing unref. The burst stopped by itself after 20 s.

Conclusion: the shared-queue lock showed no threading error under validation,
but the soak does not meet its goal. WebKit renders with its own GL context, so
it reaches the Vulkan queue only through repaints it triggers
(`compositor_request_repaint` → `surface_present_kms` on the WebKit thread).
With no visible, continuously updating webview, that concurrency was barely
exercised. Open:

1. Why the visible WPE webview showed nothing although its plane was presented.
2. What holds five non-front slots when a layer appears.
3. The engine sync hazards above, for the patched-engine layout item.

Rerun the soak after 1 and 2.

Follow-up (2026-10-21, runner `telosnex/2026-10-21-0aa85f3`): the webview
rendered on plane p107 between Flutter's lower and upper layers. A KMS trace
showed each pool exhaustion matched a brief third Flutter layer (2 layers x 3
buffers = all 6 slots), so item 2 was pool sizing, not a leak. The pool is now
9 slots. The same release sets premultiplied blend mode on every plane above
the bottom one, which fixed a black square behind the cursor. On device, the
webview flicker and the cursor square are both gone. Item 1 did not reproduce.
Item 3 and the full soak rerun remain open.

Soak rerun (2026-10-21, runner `telosnex/2026-10-21-0aa85f3`, Khronos
validation with `VK_VALIDATION_VALIDATE_SYNC=1` through a temporary
`zz-experiment.conf`, removed afterwards). One process (PID 4079553) ran
55 min 45 s with a visible WPE webview on plane p107: opened 08:19:44, closed
09:08:54, reopened 09:08:59 at a different placement (1745x1023 at 79,49),
closed 09:09:23, reopened 09:09:26 full width, with page loads in each.

| Check | Result |
| --- | --- |
| Runner restarts | 0 |
| Validation threading errors (shared queue) | 0 |
| `VK_ERROR_*`, device lost, fence timeouts | 0 |
| Surface-pool EAGAIN | 0 (14 in the 2026-10-20 run with 6 slots) |
| Other validation messages | The same 20 engine sync hazards at startup only (item 3) |

Result: the shared-queue sync and the 9-slot pool pass on device with Flutter
and a visible WPE webview, including close/reopen and a changed webview
placement. The run was just under the 1-hour target. Item 3 (engine barrier
hazards) remains open. Full log: `/tmp/soak-20261020.log` on the
operator Mac (not durable).

## Latest status: display-plane modifier intersection (#7) fixed

The window already picks the modifiers the display plane accepts for the
render format and passes them to the GL surface. It now passes the same list to
`vk_gbm_render_surface_new()`. The Vulkan surface keeps only the plane modifiers
that pass `check_import_support()` (single memory plane, color-attachment and
transfer features, dma-buf import, size). It then allocates every buffer with
`gbm_bo_create_with_modifiers2()`, that common list, and
`GBM_BO_USE_RENDERING | GBM_BO_USE_SCANOUT`. The scanout flag matters on split
render/display devices such as the Pi 5 (v3d + vc4). If GBM ignores the list,
or the list allocation fails and the usage-flag fallback returns another
modifier, the buffer is rejected before Vulkan or KMS sees it.

If the plane and Vulkan have no modifier in common, surface creation logs
`No DRM format modifier for <format> is accepted by both the display plane ...`
and returns NULL. Nothing is allocated, and no frame reaches KMS. If the plane
lists no modifiers (no `IN_FORMATS`), the surface keeps the old usage-flag
allocation, as the GL path does.

New regressions (label `vulkan-audit-modifiers`, plus two window tests):
`no_common_plane_modifier_fails_cleanly`, `common_linear_only_allocates_linear`,
`common_tiled_modifier_is_used`, `allocator_ignoring_common_modifiers_is_rejected`,
`vulkan_surface_receives_plane_modifiers` and
`vulkan_surface_without_plane_modifiers_gets_null`. The tiled and LINEAR cases
check the modifier given to GBM, to `vkCreateImage` and to ADDFB2. Results:
**91/91 Release (CI `telosnex/build.sh`), 92/92 Debug+ASan/UBSan.** Three mutations
fail as expected: the window passing NULL to Vulkan, skipping the intersection,
and dropping the post-allocation modifier check.

These are fake-driver tests. They don't show which modifier the Pi selects. The
runner logs `Vulkan GBM surface <w>x<h> uses modifier 0x...` once per surface.
The on-device check is that line plus no atomic commit or ADDFB2 errors in the
runner log.

Known limit, shared with the GL path: the window takes the modifier list from
the first plane on the CRTC that supports the format. That may not be the
plane that finally shows the layer.

## Latest status: shared-queue host synchronization implemented

The patched SDK and installed engine were identified and matched to their runtime
receipt; the embedder API already provides a shared-lock interception hook.
The runner now uses it for engine and runner queue/idle operations. No SDK API
change was required. See [the shared-queue review](vulkan-shared-queue-review.md)
for implementation, provenance, concurrency tests and remaining integration limits.

**85/85 Release, 86/86 Debug, 86/86 ASan/UBSan pass**, including 17 new dispatch and
concurrency tests. Two lock/resolver mutation checks fail as expected. A real Pi
GPU smoke passed 20 cycles through the nested resolver. No service deployment or
restart; the Flutter/WPE soak rerun (2026-10-21, below) passed. Display-plane
modifier intersection was open at this checkpoint; it is fixed above. Historical statements below that queue
synchronization is unimplemented are superseded by this section.


## Earlier checkpoint: seven source-review regressions fixed, not deployed

The seven tests added in the preceding pass now pass. Full results are
**68/68 Release, 69/69 Debug and 69/69 ASan/UBSan**, with leak detection enabled.
Twelve additional tests cover capability rejection, retained fence ownership,
retry/renewal, slot recycling, sync-file waits/errors, fd-duplication failure,
snapshot fbdev rejection and a failed final GPU wait. None use WILL_FAIL.
The original red logs remain available separately.

### Implementation and limits

- **Pool pressure (#2):** six slots support the tested two-layer/three-frame
  overlap. Both framebuffer and ownership arrays use the same count, and the
  exhaustion/recovery regression derives its capacity from that constant. This
  increases eager buffer allocation from four to six (50% more image storage).
  It is a bounded fix for this workload, not an adaptive pool: greater overlap
  or more layers can still return EAGAIN. No on-device warning causality is claimed.
- **Repaint completion (#3):** first scanout captures one completion payload per
  backing-store generation. The slot retains the master fd and each KMS request
  gets a CLOEXEC duplicate. An already-complete export or successful idle fallback
  is cached too. Failed request/duplication does not discard the reusable payload.
  Releasing/recycling a slot or presenting an engine store with did_update resets
  its cache. Existing KMS duplicates remain valid when the master closes.
- **Planes without IN_FENCE_FD:** wait on that image's sync-file with poll rather
  than waiting for the whole Vulkan device. EINTR retries; error events/failures
  reject presentation. Once signaled, later repaints need neither another queue
  submission nor a device wait.
- **Teardown (#4):** always wait before destroying GPU resources, independent of
  export support or prior semaphore retirement. Device loss permits destruction;
  another wait error logs and deliberately retains GPU handles and the renderer
  reference, rather than freeing potentially busy memory. This catastrophic-error
  path leaks those resources until process exit; it is not automatic recovery.
  A fault-injection regression verifies this policy and cleans up its own mocks.
- **Modifier validation (#5):** reject INVALID and query the physical device's
  modifier list, memory-plane count, required tiling features, image-format usage
  support, dimensions/sample limits and dma-buf importability/compatible handle
  type before vkCreateImage. Unsupported combinations unwind the GBM BO cleanly.
  Multi-plane and unsupported pixel-format rejection from the previous pass remain.
- **Unsupported outputs/transforms (#9/#10):** raw and snapshot fbdev presentation
  return EOPNOTSUPP; non-axis-aligned Vulkan presentation returns EOPNOTSUPP before
  scanout side effects in Release as well as Debug. Neither feature is implemented.
- **Stale comment (#8):** corrected; actual alpha-bearing format is preserved.

First scanout still captures its completion lazily in present, not via an engine
per-image completion API. **Shared-queue host synchronization (#1) remains open**;
these changes do not provide an engine-shared queue lock, and surface teardown
and first scanout/fallback still access the engine's queue/device. A surface-local
lock cannot resolve that. The display-plane/Vulkan modifier intersection (#7)
also remained separate at this checkpoint (now fixed, see the latest status): validating Vulkan importability is not proof that the
selected display plane can scan the image. Do not deploy on the strength of unit
results alone; the customized engine contract and real Flutter/KMS integration
still require validation.

### Evidence and fixture adjustments

The seven existing red expectations were preserved. Three older fence tests now
check the fd *handed to KMS* rather than the exported master, since retaining the
master is deliberate. The no-IN_FENCE_FD wait tests now inject/observe poll failure
instead of device-idle failure; they additionally verify zero device waits. The
capacity regression still tests actual exhaustion and recovery, at six slots.
New capability and poll mocks model driver boundaries; they are not GPU validation.

A fresh Pi render-node smoke passed **20 real GBM/Vulkan allocation, red-clear,
CPU-readback, snapshot and teardown cycles**, including the new import queries.
It did not take DRM master, commit to KMS, start Flutter, or restart the service.
No new Khronos validation-layer or engine concurrency coverage is claimed.

Fresh logs: `~/.cache/tpi-device-study/vulkan-audit/source-review-fixed/`.
Select the seven previously red cases with `ctest -L vulkan-audit-source-review`;
the old `vulkan-audit-open` label was renamed. Run unfiltered CTest for the full
suite. Historical checkpoint sections below retain their original counts and
wording and do not describe the current tree.


## Historical checkpoint: source-review regressions left red

Added **seven red tests across six of the ten reported concerns**, plus two green
controls. All seven fail in Release, Debug and ASan/UBSan. The remaining suite
passes **49/49 Release, 50/50 Debug and 50/50 ASan/UBSan**. Full totals are 56,
57 and 57 respectively, each with seven failures. Production source hashes are
unchanged from the start of this pass. No device deployment, service restart or
on-device frame trace was performed.

### Assessment of the ten reported concerns

| # | Assessment and test evidence |
| --- | --- |
| 1 | Serious engine-integration concern. The code submits/waits without an engine-shared lock. No claim that this turn verified the customized engine's lock/serialization contract; a mock cannot establish that. A surface-local mutex is not a replacement for queue-wide engine cooperation. |
| 2 | **Modeled red:** retaining four distinct images through KMS callbacks exhausts the four-slot pool; the next fill returns EAGAIN (11). The model represents two two-layer frames held for display and a third being produced. It releases Flutter ownership of the first four, so engine caching is not required for the failure. The one-layer/three-frame control passes. It does not execute real atomic scheduling or prove this overlap occurs on the Pi, nor that this is the cause of the appliance warning. Six slots is the modeled requirement, not a universal maximum. |
| 3 | **Red:** repainting an immutable snapshot after a newer image is queued increases vkQueueSubmit calls from one to two. This proves fresh completion work is submitted for an old image, not actual GPU latency or an on-device stall. |
| 4 | **Two modeled reds:** without export support, or after failed export and successful fallback retired the semaphore, a later discarded engine submission's image is destroyed without a final wait. The mock tracks one specific busy image. The semaphore-enabled teardown control waits and passes. These models assume the engine can release an unpresented backing store before GPU completion; that release/completion contract must be checked against the customized engine. They do not demonstrate a real GPU use-after-free. |
| 5 | **Red for the invalid-modifier case:** DRM_FORMAT_MOD_INVALID is passed into explicit Vulkan modifier image creation before being rejected by the mock. The broader format/modifier/usage capability intersection remains source-level review; no real-driver unsupported-combination sweep was done. |
| 6 | Already fixed before this pass: multi-plane GBM buffers are rejected with ENOTSUP before Vulkan image creation. Its existing test still passes. |
| 7 | **Fixed** (see the latest status). The window's plane modifier list now reaches the Vulkan constructor. Buffers are allocated only with modifiers that both the plane and Vulkan accept. No common modifier fails surface creation with a clear log line. Originally: window modifier selection was supplied to GL but not the Vulkan constructor/GBM allocation. A later KMS rejection is not necessarily invalid Vulkan usage. |
| 8 | Confirmed stale comment: the path uses the transparent pixel format, not the claimed XRGB cast. This is not proof that using transparency is wrong for the current display/composition. No test encodes an unverified Pi 4 workaround as required behavior. |
| 9 | **Red:** the raw Vulkan surface's fbdev present callback traps at UNIMPLEMENTED instead of returning an error. This is a direct API test, not proof that startup selects this combination or that the complete fbdev route reaches it. |
| 10 | **Red:** directly presenting a non-axis-aligned Vulkan layer aborts in Debug/ASan and returns success in Release, handing the bounding rectangle to KMS. The test requests graceful rejection, not implementation of arbitrary transforms. It does not prove that a particular engine layer or rotation configuration reaches this path. |

### New cases and reproduction

`test/vulkan_audit/surface_test.c` contains:

- `two_layer_three_frame_overlap_can_acquire_next_frame`
- `repaint_reuses_image_completion_without_new_queue_work`
- `teardown_without_export_waits_before_destroying_busy_images`
- `teardown_after_export_disabled_waits_for_later_work`
- `invalid_modifier_is_rejected_before_vulkan_image_creation`
- `fbdev_presentation_returns_error_instead_of_aborting`
- `non_axis_aligned_layer_is_rejected_instead_of_misrendered`

These are normal CTest failures, not WILL_FAIL or disabled cases. The
`vulkan-audit-open` label is reused for these **new** tests; the earlier three
fixed regressions retain `vulkan-audit-followup`.

```sh
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir /tmp/fpi/build-review-asan -L vulkan-audit-open --output-on-failure
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir /tmp/fpi/build-review-asan -LE vulkan-audit-open --output-on-failure
```

Repeat with `/tmp/fpi/build` or `/tmp/fpi/build-review-debug`. Fresh red/control
logs and build configuration summaries are under
`~/.cache/tpi-device-study/vulkan-audit/source-review-red/` on the host and
`/tmp/new-source-*-{red,controls}.log` on the builder.

**Next integration check:** trace per-frame backing-layer count, backing-store
image identity, acquisition/release and KMS current/pending ownership with the
webview open. Merely counting layers does not establish simultaneous ownership
or prove pool exhaustion. No claim about the exact installed engine should be
upgraded based solely on a matching upstream version or these mock tests.

The sections below preserve earlier checkpoints; references there to "current"
or "open" describe those historical checkpoints, not the latest status above.


## Earlier checkpoint: three follow-up defects fixed, not deployed

The three findings previously listed as open are now fixed. Modifier-list
allocation failure returns NULL without changing the cached surface; zero
matching modifiers require no allocation. Unsupported Vulkan pixel formats
return ENOTSUP before GBM/Vulkan allocation. Multi-plane GBM buffers are destroyed
and rejected with ENOTSUP before vkCreateImage: full multi-plane import support
was **not** added.

The original red-test expectations remain, with stricter rejection/resource
checks added. Two further tests cover the shared GL allocation-failure path and
an empty modifier list. Full results: **47/47 Release, 48/48 Debug, 48/48
ASan/UBSan**, with leak detection and halt-on-UB enabled. The historical
`vulkan-audit-open` CTest label is now `vulkan-audit-followup`.

A fresh Pi smoke also passed 20 real GBM/Vulkan allocation, clear/readback,
snapshot and teardown cycles with the fixes. This exercises the supported
single-plane path, not unsupported multi-plane hardware or Flutter/KMS scanout.
The application service stayed active with zero restarts; no deployment occurred.

Fresh logs: `~/.cache/tpi-device-study/vulkan-audit/three-fixes/`.
Earlier red results below are historical evidence, not the current test status.
Engine/shared-queue and external-image integration questions remain unresolved.


## Scope and state

Reviewed renderer construction/capability selection, GBM/DMA-BUF image import,
backing-store ownership, composition snapshots, resize caching, explicit-sync
export/fallback and KMS request ownership/unwind. Also inspected the Vulkan
callbacks/configuration in `flutter-pi.c` and the surface/window interfaces.
This is a native-code audit, **not** a completed audit of the customized Flutter
engine or the GPU/kernel implementations.

The checkout already contained uncommitted lifecycle and Vulkan fixes when
this audit pass started. They were preserved. Existing regression fixtures were
recovered from the Linux builder, reviewed, expanded and rebuilt against this
checkout. No production source was changed during this pass; additions are
tests, test-build wiring and this report. No candidate was deployed and the Pi
service was not restarted.

**Result: three additional defects remain red in the current working tree.**
Do not describe the complete suite as passing. Existing local fixes for the
older defects have passing regression coverage, but are not deployment acceptance.

## Still-open findings (new failing tests)

### P1: modifier-list allocation failure dereferences NULL

`src/window.c:1539`, `kms_window_get_render_surface_internal()`, allocates a
modifier array but does not check a failed nonzero allocation. The following
`extract_modifiers_for_pixel_format()` writes through NULL at line 1455. This
path is used by both Vulkan and GL surface creation, including resize.

Test: `modifier_allocation_failure_does_not_dereference_null`.
A source-local allocator fails the modifier-array allocation; a synthetic DRM
plane advertises one real modifier entry. Release/Debug segfault. UBSan reports
`store to null pointer of type 'uint64_t'`. The desired contract rejects the
new surface while retaining the previous cached surface.

### P2: unsupported forced pixel format aborts or makes an invalid Vulkan call

`src/vk_gbm_render_surface.c:245`, `fb_init()`, checks the Vulkan mapping only
with an assertion. RGB565 is an accepted CLI pixel format and reaches this path
through the window's forced-format selection, but its mapping is
`VK_FORMAT_UNDEFINED`. Debug aborts; Release passes that invalid format into
`vkCreateImage`. A real driver may return an error, but issuing the call violates
the API's valid-usage requirements and is not a safe validation strategy.

Test: `unsupported_pixel_format_is_rejected_without_abort_or_invalid_vulkan_call`.
The fake driver records invalid image-create arguments; it does not pretend to
support an undefined format. Supporting RGB565 is not required to pass: rejecting
it before the Vulkan call is sufficient.

### P2: multi-plane modifier is unconditionally described as one memory plane

`src/vk_gbm_render_surface.c:297`, `fb_init()`, hardcodes
`drmFormatModifierPlaneCount = 1` and supplies only plane 0's layout without
checking GBM's memory-plane count. A packed RGB format can still have a modifier
with an auxiliary memory plane (e.g. Intel Y-tiled CCS).

Test: `multiplane_modifier_is_supported_or_rejected_before_vulkan` supplies that
two-plane modifier. The fake ICD detects the incorrect one-plane descriptor.
Either proper multi-plane support or an early, clean rejection satisfies the
assertion. This is a portability defect, **not evidence that the Pi's current
one-plane modifier caused its backing-store warnings**.

Specification: `VkImageDrmFormatModifierExplicitCreateInfoEXT`,
VUID `drmFormatModifierPlaneCount-02265`. Modifier memory planes are not the same
as a pixel format's color planes.

## Older defects: existing candidate fixes and regression coverage

These are not newly introduced changes from this pass. The corresponding
assertions currently pass. The tests execute production implementation code,
with driver and system boundaries replaced as described below.

| Area | Defect in pre-fix code | Principal regression(s), prefixed `vulkan_audit.` |
|---|---|---|
| Renderer lifetime | Constructor leaves the reference count uninitialized | `constructor_initializes_reference_count` |
| Capability selection | Accepts a Vulkan 1.0 device although 1.1 is required | `rejects_device_below_required_api_version` |
| Enumeration | Zero device extensions produce a zero-bound VLA | `rejects_device_with_no_extensions_without_undefined_behavior` |
| Memory types | Signed `1 << 31` has undefined behavior | `memory_type_bit31_has_no_undefined_shift` |
| Validation | Debug callback returns VK_TRUE, requesting call abortion | `validation_callback_does_not_abort_vulkan_calls` |
| Build configuration | AUTO validation misses CMake's `Debug` spelling | `debug_auto_enables_validation` (Debug/AUTO only) |
| Image unwind | Bind failure omits vkDestroyImage | `bind_failure_destroys_image` |
| Import layout | Explicit modifier layout supplies a nonzero size | `modifier_explicit_layout_uses_zero_size` |
| Import allocation | Uses subresource layout size instead of image memory requirements | `allocation_covers_image_memory_requirements` |
| Surface lifetime | Renderer reference is never released; front buffer forms a self-cycle | `surface_destruction_releases_renderer_reference`, `last_front_buffer_does_not_keep_surface_alive` |
| Frame identity | Multiple backing layers and later repaints resolve to the same mutable latest image | `two_backing_layers_keep_distinct_images`, `snapshot_pins_image_across_next_frame_and_engine_release` |
| Composition | Wrong horizontal offset and ignored queue-present errors | `backing_layer_preserves_horizontal_offset`, `compositor_propagates_queue_present_failure` |
| Resize | Cache returns an image with the wrong requested dimensions | `cached_surface_matches_requested_dimensions`, `failed_resize_keeps_old_cache_alive` |
| Semaphore state | Export failure leaves a signaled binary semaphore that gets signaled again | `failed_fence_export_does_not_resignal_binary_semaphore`, `failed_export_and_wait_never_reuse_signaled_semaphore` |
| Synchronization | Failed device-idle wait still permits presentation | `failed_device_wait_does_not_present_unsynchronized_image` |
| Completed export | Successful SYNC_FD export of -1 unnecessarily invokes fallback wait | `completed_sync_fd_export_does_not_wait_for_device` |
| KMS fences | Legacy or unsupported atomic plane silently discards explicit dependency; property failure ignored | `legacy_path_must_not_discard_explicit_fence`, `atomic_plane_without_fence_must_not_silently_accept`, `failed_fence_property_must_fail_layer_submission` |
| KMS rollback | Failed property addition must restore atomic request/plane availability without stealing caller FD/reference | `failed_property_rolls_back_request_plane_and_caller_ownership` |

Additional passing controls cover constructor-stage failures, early import
failures/FD transfer, partial pool initialization, KMS push rejection, successful
fence handoff, checked no-fence retry, same-size cache reuse and minimum-version
acceptance. The six `vulkan_backing_store_test` cases separately cover dropped
stores, re-presentation, independent KMS ownership and bounded pool exhaustion.

## Red evidence and current results

Fresh current-tree runs (Linux builder):

| Configuration | Full CTest result |
|---|---|
| Release, Vulkan ON | 42 pass, **3 fail**, 45 total |
| Debug, Vulkan ON, AUTO validation | 43 pass, **3 fail**, 46 total |
| Debug + ASan/UBSan, leak detection, Vulkan ON | 43 pass, **3 fail**, 46 total |

All three failures are the still-open findings above, not inverted expected
failures. Debug has one extra build-configuration test. Each case runs in its
own CTest process with a timeout, so the intentional abort/segfault does not
prevent the other cases from executing. An assertion/crash is an actual red
result; `WILL_FAIL` is not used.

As a sanity check, a **throwaway source copy** with three early-rejection guards
made those three tests green in Debug. Those guards were not applied to this
checkout. The tests therefore distinguish a small safe correction from the
current behavior, without requiring a new rendering feature.

Also freshly replayed the renderer tests against the exact
`d964a50:src/vk_renderer.c` in a throwaway tree with UBSan: **5 fail, 2 pass**.
The five failures are uninitialized refs, the validation callback, API-version
selection, zero-bound VLA and signed bit-31 shift. All seven pass against the
current renderer. This replay changes only that renderer source; it is not a
claim that the entire current test suite builds against an old checkout.

Earlier saved pre-fix logs additionally show 18/26 failures in Release,
19/27 in Debug and 20/27 with ASan/UBSan. Those are historical evidence, not the
fresh full-suite counts above. The working-tree fixes predated this audit pass.

Evidence under `~/.cache/tpi-device-study/vulkan-audit/`:
- `current-release.log`, `current-debug.log`, `current-asan-ubsan.log`;
- `renderer-baseline-replay.log`, `proof-guards-green.log`, `proof-guards.py`;
- earlier `release.log`, `debug.log`, `asan-ubsan.log`;
- `start.patch` captures the already-modified starting checkout.

## Important unresolved integration questions

1. **Shared-queue host synchronization.** `flutter-pi.c` gives the engine the
   same VkQueue used by `present_fb()` and `vkDeviceWaitIdle` fallbacks. Platform
   repaints are not necessarily on the engine raster thread. A surface mutex
   is not a queue lock shared with the customized engine, and different surfaces
   have different mutexes. Audit the engine's queue locking/submission contract
   with Vulkan synchronization validation before treating this as safe. No
   deterministic current end-to-end race reproduction has been established;
   a fake test assuming a particular engine schedule would not prove it.
2. **Image layout and external queue-family ownership.** The import path starts
   with UNDEFINED layout and relies on engine rendering/transition behavior.
   Check the actual customized engine's final layout, release/acquire barriers,
   GBM modifier negotiation, supported usages and enabled-device features. A
   sync-file alone does not establish every layout/ownership requirement.
3. **Real KMS/GPU load and pool pressure.** Unit tests prove reference and failure
   contracts, not freedom from four-slot pressure, GPU fence timeouts or actual
   scanout artifacts. Exercise multi-layer Flutter content, WebKit repaint,
   resize, dropped frames and close/reopen on the device before deployment.
4. **Validation coverage.** The saved render-node smoke log reports that Khronos
   validation was unavailable. Its clear/readback cycles are useful, but cannot
   be presented as a Vulkan-validation pass or an engine/KMS integration test.
5. **fbdev fallback.** Vulkan `present_fbdev` remains explicitly unimplemented.
   It is not an audited/working alternative to KMS.

The automatic tests never open a DRM device or start Flutter/WebKit. They cannot
prove shader correctness, visual output, driver resource reclamation, every
thread interleaving, or correct queue ownership by external code. No claim that
these three remaining defects explain the user's particular Pi warning is made.

## Fresh reproduction after artifact restoration

The archived checkout tests/docs, builder sources/builds/logs and device probes
were restored without path conflicts. Production fixes were left unchanged.
The current checkout sources/tests were then copied to the Linux builder,
reconfigured and rebuilt in Release, Debug and Debug+ASan/UBSan.

All three open regressions fail in **each** configuration:

| Regression | Release | Debug | ASan/UBSan |
| --- | --- | --- | --- |
| Modifier-array allocation failure | Segmentation fault | Segmentation fault | `window.c:1455`: store to null pointer of type `uint64_t` |
| Unsupported RGB565 | Mock detects invalid `VK_FORMAT_UNDEFINED` image creation | Assertion abort in `fb_init` | Same assertion abort |
| Multi-plane GBM modifier | Mock detects wrong modifier plane count | Same assertion failure in the test | Same assertion failure in the test |

These are fault-injection tests exercising production source, not a real-driver
or Pi KMS reproduction. The multi-plane fixture advertises a two-plane Intel
CCS modifier; it does not establish that the Pi selects such a modifier.
No `WILL_FAIL` inversion, disabled assertions, or production fixes were added.

The remaining suite stays green: **42/42 Release**, **43/43 Debug**, and
**43/43 ASan/UBSan**, including the zero-extension device regression. Thus the
combined suite has exactly three failing tests in each configuration.

Commands (on the builder, after configure/build):

```sh
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir /tmp/fpi/build-review-asan -L vulkan-audit-open --output-on-failure
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir /tmp/fpi/build-review-asan -LE vulkan-audit-open --output-on-failure
```

Repeat with `/tmp/fpi/build` and `/tmp/fpi/build-review-debug` for Release and
Debug. Fresh outputs are saved on the host in
`~/.cache/tpi-device-study/vulkan-audit/restored-reproduction/` (separate open
and control logs for each configuration), and on the builder under
`/tmp/reproduced-{open,controls}-*-results.log`.
No application deployment or service restart was performed.
