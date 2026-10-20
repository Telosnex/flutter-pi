# Vulkan audit: intentionally red regressions

These tests assert the **desired** behavior of the current native renderer and
its compositor/KMS boundaries. They are not `WILL_FAIL` tests and do not invert
failures into passes. With `ENABLE_TESTS=ON` and `ENABLE_VULKAN=ON`, they are part
of the normal CTest run. Until the defects are fixed, that run is red.

Configure/build the normal Linux project with its development dependencies and
Unity available, for example:

```sh
cmake -S . -B build-audit -DCMAKE_BUILD_TYPE=Debug \
  -DENABLE_TESTS=ON -DENABLE_VULKAN=ON -DVULKAN_DEBUG=AUTO -DLTO=OFF
cmake --build build-audit --parallel
ctest --test-dir build-audit -L vulkan-audit --output-on-failure --timeout 10
```

Repeat in Release. For ASan/UBSan use a separate Debug build, also with:

```sh
-DCMAKE_C_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' \
-DCMAKE_EXE_LINKER_FLAGS='-fsanitize=address,undefined'
```

Run its tests with `ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1`.
The old signed bit-31 shift and zero-bound VLA require UBSan to detect;
the current candidate already fixes both. The AUTO-validation configuration check is registered
only for Debug builds requesting `VULKAN_DEBUG=AUTO`.

Select one regression with, for example:

```sh
ctest --test-dir build-audit -R '^vulkan_audit.bind_failure_destroys_image$' -V
```

To check the pre-audit suite separately (not a claim that the whole suite passes):

```sh
ctest --test-dir build-audit -LE vulkan-audit --output-on-failure
```

## Boundaries

- `renderer_test.c` compiles the real renderer and replaces Vulkan entry points
  with a deterministic fake ICD. Its source-local allocator fills allocations
  with a known nonzero pattern to expose the uninitialized refcount reliably.
- `surface_test.c` compiles the real Vulkan surface and compositor. Driver/GBM,
  renderer-object and KMS edges are faked; real surface references, locking,
  initialization/unwind, backing-store callbacks and layer processing execute.
  Real disposable `/dev/null` FDs exercise import/close ownership, not DMA-BUF
  validity. Successful mocked imports consume those FDs like a driver would.
- `kms_test.c` compiles the actual KMS builder and captures libdrm properties.
  An unsignaled eventfd is only an opaque fence token. No sync-file ioctl or
  kernel fence completion is exercised. Tests cover preserving/rejecting an
  explicit dependency, including libdrm property-add failure.
- `window_test.c` executes the cached render-surface path with a synthetic cached
  surface and a fake modifier list. A source-local allocator injects OOM in the
  real modifier-array allocation. It does not allocate a Vulkan image.

Normal assertion failures clean up their fixtures. Abort/segfault cases run in
separate CTest processes and cannot execute their normal teardown. In particular,
teardown releases the internal front-buffer pin after expectations are checked.
This is fixture cleanup, not a production fix or a replacement for an assertion. Mock resource counters expose missing Vulkan-handle destruction; ASan alone
cannot detect a GPU-handle leak. No test opens a DRM device, starts Flutter/WPE,
or contacts the running Pi service.

See `docs/vulkan-renderer-audit.md` for findings, priorities, exact results and
hardware-dependent questions left open.

## Current status

All regressions pass: **91 Release / 92 Debug / 92 ASan+UBSan**. The seven
source-review cases formerly labeled `vulkan-audit-open` are now selected with
`vulkan-audit-source-review` and remain ordinary tests, not WILL_FAIL cases.

```sh
ctest --test-dir build-audit --output-on-failure
# Only the seven formerly red source-review regressions:
ctest --test-dir build-audit -L vulkan-audit-source-review --output-on-failure
```

The 17 `vulkan-queue-sync` cases exercise the actual shared-lock resolver/dispatch
implementation against a fake driver, including controlled two-thread exclusion
without sleeps. Existing surface tests also route through this layer. A native
render-node smoke uses nested engine-style proc lookup, but a real Flutter/WPE
concurrency soak and Vulkan threading validation have not been run.

The pool remains bounded (six slots). The `vulkan-audit-modifiers` cases check
that Vulkan allocates only with modifiers the display plane also lists, and that
no common modifier fails surface creation cleanly. They use a fake driver, so
real KMS acceptance on the Pi still has to be checked on the device. See `docs/vulkan-shared-queue-review.md` and the newest
audit report section for provenance, mutation checks and integration limits.

The opt-in `vulkan_gbm_smoke` binary is not registered in CTest. It opens only a
specified DRM render node, exercises real import/clear/readback and teardown,
and never takes DRM master or changes the appliance display/service. It does
not exercise real Flutter composition, KMS commits, WebKit or engine queue
concurrency. Do not substitute its result for full-device acceptance.

## Earlier follow-up regression fixes

The three allocation/format/multi-plane regressions are now green. Select them
with `ctest -L vulkan-audit-followup --output-on-failure` (the former
`vulkan-audit-open` label was renamed). Unsupported formats and multi-plane
imports are rejected cleanly, not newly supported. Empty modifier lists and GL
modifier allocation failure have separate regressions. At that earlier checkpoint, 47 Release / 48 Debug / 48 ASan+UBSan passed;
the new open regressions above were not yet part of the suite.
