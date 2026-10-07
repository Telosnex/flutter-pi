# Shared Vulkan queue: review of the actual Telosnex stack

## Fix implemented in the runner (not deployed)

`src/vk_queue_sync.c/.h` now implement the existing embedder interception contract.
`src/flutter-pi.c` supplies the wrapped resolver; both nested resolver entry points
remain intercepted. `vk_renderer_new` registers real per-device/queue dispatch
pointers before engine startup. Surface submit/fallback/destruction and renderer
idle calls use the same wrappers as the engine. Final renderer destruction removes
the registration after its idle wait; construction failures unwind registration.

A process-lifetime mutex covers the **actual driver calls**, including device-wide
idle. It intentionally serializes even separate registered devices; the application
currently uses one. It is not a surface lock or a replacement for GPU fences.
Covered entry points: Submit, WaitIdle, DeviceWaitIdle, BindSparse, PresentKHR,
Submit2/core and KHR, and begin/end/insert debug queue labels. Optional unavailable
functions remain NULL. Unknown future/vendor `vkQueue*` functions fail closed
instead of escaping the lock; currently enabled extensions do not require them.
Non-queue calls pass through. Real pointers are loaded through the real device
resolver, not through the wrappers, and metadata is protected by the same lock.
The engine retains its own private QueueVK lock above this shared dispatch lock.
No SDK patch, engine ABI change, or engine rebuild is needed for this mechanism.

**Verification:** 85/85 Release, 86/86 Debug and 86/86 ASan/UBSan pass. The 17 new
`vulkan-queue-sync` tests compile the actual dispatch implementation and fake only
the driver. Interleaving tests hold the first driver call, observe the competing
thread's attempt to enter the shared lock, then release and join both workers.
They cover both directions of submit/device-idle exclusion, multiple queues,
optional queue operations, unregister versus an active call, both lookup chains,
error returns/unlocking, allocation/registration failure, and argument forwarding.
No sleep is used to arrange the interleaving. Existing surface/renderer tests now
register their fake driver and exercise the real dispatch layer too.

Two isolated mutation builds were checked: removing host-lock calls makes the
inside-driver lock assertion fail (12 unlocked entries); returning raw resolver
pointers makes the lookup assertion fail. These mutations were never applied to
the working source or deployed. Logs: `~/.cache/tpi-device-study/vulkan-audit/queue-sync/`.

A Pi render-node smoke passed 20 allocation/clear/readback/snapshot/teardown cycles.
Its clear submission now follows the nested engine-style instance/device resolver
chain, while surface presentation and teardown use runner wrappers. This is not
a live Flutter/WPE concurrency or Khronos threading-validation run. The service
remained active with PID 112123 and zero restarts; the running runner is unchanged.

Before shipping, build/pin the fixed runner and perform the real engine/WPE soak
and threading-validation run. Writing an export patch alone would not change the
pinned runner build input. The historical investigation below explains provenance
and why the existing hook is sufficient; its "not fixed" statements predate this
implementation. Display-plane modifier compatibility remains separate.

## Historical investigation (before this fix)


## Conclusion

The shared-queue host-synchronization gap is present in the reviewed **patched
Flutter 3.47.1 + flutter-pi** sources, not merely inferred from upstream 3.44.2.
But the earlier claim that fixing it necessarily requires a new engine API was
too strong. The existing embedder proc-address callback explicitly supports
injecting the shared locking needed here. We do not need to acquire Impeller's
private mutex or change the SDK ABI to implement that approach.

This pass is source/provenance investigation only. No production code was changed,
no engine/runner was rebuilt or deployed, and no actual concurrent-driver-call
trace was captured. This does not establish the cause of the backing-store warning.

## Provenance checked

- Project: `/Users/jpo/dev/telosnex`, especially `lib/features/flutter_pi/README.md`,
  `dev/flutter_pi/toolchain.lock.json`, `dev/flutter_pi/engine/patches/series.json`,
  and `lib/third_party/sdk_patches/deployment.json`.
- SDK: `/Users/jpo/dev/flutter`, framework HEAD
  `6655482ec06e547f90abf8ae7590466f4415978d`; engine revision
  `5d531788691ec3404cac0cee66ead4007b177363` (Flutter 3.47.1).
- All **11 patch-file hashes and 68 patched-source after-hashes** match the engine
  series manifest. The SDK deployment profile includes that series; the other
  SDK patch sets do not supply queue synchronization.
- Current runtime input digest recomputed from the lock, ordered engine patches
  and build recipes matches `bin/cache/tpi-runtime-release.json`:
  `6412e705e4b6b47dfcc7b28ba9e21a3b1e4a1aa720f9002e3c069d0a04d3ae8d`.
- The actual cached Pi engine and the Pi's `/opt/telosnex/libflutter_engine.so`
  both hash to
  `9287658459dc506d1aeb465676546492b0c441bc3efd4f25a29ea4d3d8643abe`,
  matching that receipt. `/proc/<service-pid>/maps` names that engine file.
  Receipts are build-provenance evidence, not hermetic-build attestations.
- The running runner is `/opt/telosnex-direct-test/flutter-pi`, **not** automatically
  the runner identified by the project lock. The runner patch exports are review
  artifacts; the pinned fork commit is the normal build input. The lock's runner
  commit `a2eddf76` is an ancestor of the reviewed `flutter-pi-argon` HEAD `d964a505`.
  The current working-tree fixes have not been deployed.
- The separate `/Users/jpo/dev/flutter-pi` checkout was also inspected: its
  resolver passes through raw Vulkan addresses and its surface path calls raw
  device-idle. It does not contain a hidden shared-lock solution.

## What the patches actually guarantee

Runner patch `0008-vulkan-replace-per-frame-vkDeviceWaitIdle-with-an-ex.patch`
adds an empty queue submission plus exported SYNC_FD. This is a GPU-completion
mechanism, not mutual exclusion for host calls to the queue. Its claim that
presentation happens after rendering on the raster thread does not exclude
background resource-upload submissions.

Engine patch `0010-impeller-vulkan-compositor-backing-stores.patch` enables
compositor-provided images and makes the root Vulkan surface a no-op. It adds
`DisposeThreadLocalCachedResources()` before the embedder callback; that method
clears descriptor/command-pool caches, not a shared queue lock or a GPU wait.
`Canvas::EndReplay` flushes command buffers; submission order is not exclusive
ownership of the queue.

Patches `0012`–`0016` change allocation, statistics, upload batching, snapshot
caching and sampling. They do not change `QueueVK` or add a queue-lock handoff.
Runner patch `0019` documents surfaceless presentation; it adds no synchronization.

## Concrete call paths in the patched SDK

Paths below are relative to `/Users/jpo/dev/flutter/engine/src/flutter`:

1. `shell/platform/embedder/embedder_surface_vulkan_impeller.cc:64-79` passes
   the runner's device, queue, family and proc-address callback to ContextVK.
2. `impeller/renderer/backend/vulkan/context_vk.cc:451` constructs
   `QueuesVK::FromEmbedderQueue`. `queue_vk.cc:63-68` uses **one QueueVK object
   for graphics, compute and transfer**, wrapping that same supplied VkQueue.
3. `queue_vk.cc:22-45` locks its private mutex for submit/present/debug-marker
   operations. No runner mutex is involved.
4. `shell/platform/embedder/platform_view_embedder.cc:157-159` returns the same
   Impeller context. The image decoder gets its context through IOManager.
   `lib/ui/painting/image_decoder_impeller.cc:689-736` uploads from a concurrent
   worker for Vulkan (the I/O-thread special case is GLES only).
5. That upload calls `GetCommandQueue()->Submit` at `image_decoder_impeller.cc:536`.
   `command_queue_vk.cc:69-77` immediately submits through `GetGraphicsQueue()`.
   Thus raster-thread affinity alone does **not** serialize all queue users.
6. The working runner's `src/flutter-pi.c:421-431` returns raw
   `vkGetInstanceProcAddr` results. Its surface first-present/fallback/destructor
   paths call raw `vkQueueSubmit` / `vkDeviceWaitIdle`. Surface/window locks do
   not participate in the engine worker's queue lock.

The WPE callback path independently calls `wait_for_frame` →
`compositor_request_repaint` → `window_request_repaint` → `surface_present_kms`.
The recent per-generation fence cache removes normal repeated queue access for
an already-captured image; that improves repaints but does not synchronize first
capture, fallback or destruction with engine upload workers.

## Existing API explicitly provides the fix hook

`FlutterVulkanRendererConfig` in `shell/platform/embedder/embedder.h:978-1013`:

- The queue must be protected by a mutex matching the mutex injected through
  `get_instance_proc_address_callback`.
- That callback should substitute thread-safe queue functions, specifically
  naming `vkQueueSubmit` and `vkQueueWaitIdle` as the minimum.
- The header points to `EmbedderTest.CanSwapOutVulkanCalls` in
  `shell/platform/embedder/tests/embedder_vk_unittests.cc:103`.

That test demonstrates interception through **both** `vkGetInstanceProcAddr`
and `vkGetDeviceProcAddr`. It is an interception example, not a concurrency test,
and was read, not newly executed in this investigation. Impeller initializes
Vulkan-Hpp's dispatcher through the supplied callback (`context_vk.cc:159-160`,
`:261`), so proc-pointer substitution is relevant to this backend too.

Also, `present_image_callback`'s host-sync promise in `embedder.h:1018-1022`
explicitly says that callback is **not used with FlutterCompositor**. It cannot
be used to justify this compositor path.

## Recommended narrow fix and tests

Implement the documented resolver interception in flutter-pi, with one shared
host-access lock for this renderer's device/queue, and route the runner's own
calls through the same wrappers. Impeller can retain its private lock; its
underlying Vulkan call then acquires our shared lock too.

Requirements:

- Cover direct lookup and nested instance/device proc-address lookup, including
  the legacy `GetInstanceProcAddr` spelling accepted by this runner. Never hand
  the engine an unwrapped resolver that lets it bypass the lock.
- Cover submit/wait operations and any other queue operations actually exposed
  and used, including Submit2 aliases and debug queue operations when enabled.
  Preserve null results for unavailable functions rather than invent support.
- Cover **vkDeviceWaitIdle**, not just the two queue functions named by the
  header. Surface fallback/teardown, renderer teardown, ContextVK destruction,
  and IdleWaiterVK can call it. A device-wide wait needs serialization with every
  queue on that device; the runner currently creates just one queue.
- Keep real driver function pointers separate from wrapper pointers to avoid
  recursion. Keep registration/lock lifetime valid through engine shutdown and
  all remaining surface releases. Do not call back into the engine while holding
  the shared driver-call lock (private-engine-lock → shared-lock ordering).
- Do not confuse this host mutex with GPU completion or image ownership. Retain
  the sync-file/KMS fences and generation lifetimes. A second queue without an
  inter-queue dependency would not solve the completion problem.

Tests should exercise the actual resolver/wrapper code with a controllable fake
Vulkan driver: hold an engine-side submit in the driver, attempt runner-side
submit/device-idle on another thread, and prove the second cannot enter until
release. Also test engine worker versus runner first-present/fallback/teardown,
both resolver chains, unavailable entry points, errors releasing the lock, and
shutdown lifetime. Then run Vulkan threading validation with image uploads and
WPE repaints on the matched engine/runner. Existing surface mocks alone cannot
prove cross-engine host synchronization.

An engine-exported per-frame completion-fence API remains a useful future design,
but is not a prerequisite for fixing this shared-queue host-access bug.
