# Webview lifecycle review — 2026-10-06

Baseline: d964a50. Device restored to this build (SHA-256 starts
73d195a5057b534b); scanout and explicit sync remain enabled. The uncommitted
`webkit_web_view_terminate_web_process()` experiment is removed. It regressed
reopening in the real application. Do not reintroduce it as a memory fix.

## Ownership map

- `gstplayer_new()` creates a DMA-BUF surface (one owning reference) and
  registers it with the compositor (another reference).
- `webview_direct_new()` borrows the player GPU/surface/compositor pointers.
  WebKit objects live on the shared WebKit thread/context. All views share
  `WebKitWebContext` and `WPEDisplay`; no per-view process isolation contract
  follows from the absence of related popup views.
- Each scanout hold owns a WPEView and WPEBuffer reference. The surface owns
  its next buffer; KMS requests take additional buffer references. Final buffer
  release queues WebKit release and unrefs on the WebKit thread.
- Each unresolved texture frame also owns a reference back to its surface.
- `gstplayer_destroy()` stops the WebKit producer before releasing the GPU and
  player texture, but omits the platform surface/registration cleanup.
- Flutter compositions own surfaces; committed KMS requests own buffers.
  Cleanup must preserve these independent display-side references until retired.

## Confirmed code defects (not all demonstrated runtime causes)

1. **Surface ownership never released.** `gstplayer_destroy()` neither removes
   the platform-view registration nor unrefs `player->dmabuf_surface`.
2. **Reference cycle.** `dmabuf_surface_push_dmabuf()` queues an unresolved
   texture frame holding `surface_ref(s)` in the texture owned by `s`.
   The final frame is only destroyed when that texture is destroyed.
   The resolver is currently an EIO stub and does not even use userdata.
3. **Final buffer omitted from destructor.** `dmabuf_surface_deinit()` destroys
   the texture but never drops `next_buf`. Fixing (1) alone cannot fix this.
4. **Broken unregister path.** `compositor_set_platform_view()` asserts that
   surface is non-NULL although NULL is its removal operation; it deletes an
   inline dynarray element, then accesses/frees the old element pointer.
   Save the surface before deletion, unref it, and never free an inline entry.
5. **Release/debug lifetime mismatch.** The release compositor casts a platform
   view ID directly to a pointer. Merely adding unregistration/unref can turn
   late Flutter compositions into use-after-free. Registry lookup and acquiring
   a reference must be consistent in all builds. Debug lookup currently does
   not take the equivalent reference either.
6. **Repaint callback error contract mismatch.** `window_request_repaint()` may
   register a listener, fail to push, and return an error while retaining that
   listener. `flutterpi_wpe_view_render_buffer()` interprets any error as "no
   listener registered" and drops the callback's reference. A later callback
   can therefore access freed state/double-release the reference.
7. **Frame completion is not generation-specific.** A delayed scanout callback
   finishes whichever buffer is currently waiting. If the 100 ms timeout
   already finished frame A, A's callback can acknowledge frame B. Callback
   cancellation/ownership and frame identity need explicit contracts.

Items 1–5 are real leaks/lifetime hazards. They do NOT by themselves prove why
an entire WebKit process remains alive. The retained-final-buffer experiment
below specifically argues against that shortcut for simple pages.

## Test environment and observations

Test program is in `~/.cache/tpi-device-study/direct-harness/`; diagnostic
sources/logs in `~/.cache/tpi-device-study/webkit-review/`. Pi logs are named
`/var/tmp/review-*.log`. The test uses the real WebKit and webview implementation
but a stub surface/compositor, so it cannot validate items 1–6 end to end.

The SSH-session D-Bus setup blocked standalone page loading. A syscall trace
showed the child waiting in accessibility/D-Bus setup; running with
`dbus-run-session` restored loads without changing webview code. Private-context
heartbeat/default-context pumping did not fix it. Earlier claims that standalone
programs could not use WebKit on this Pi were unsupported.

Using baseline d964a50 code, with a private D-Bus session:

- Local HTML: rendering, JavaScript, mouse, keyboard, touch and scrolling checks
  passed. Six reopen/close cycles reached the requested frame counts and all
  web processes exited within 78 ms after close.
- Holding the final buffer across close: four local-page cycles exited their
  processes in 29–77 ms *before* the held buffer was released.
- ESPN + scanout: all three cycles retained at least one web process beyond the
  post-close waits (2 s with final buffer held, then 5 s after releasing it).
  Final descendant RSS was about 977 MB with two web processes still alive.
- ESPN + copy: both cycles retained web processes beyond 5 s after close.
  Therefore this retention is not specific to direct scanout.
- ESPN + explicit termination (test executable only): one cycle had no web
  processes remaining; the next two retained one. Later ESPN loads committed;
  this test did not reproduce the user's full reopening regression.

Limitations: per-cycle frame count can include about:blank. Load-event logs were
also inspected, but tests still need an assertion tied to the requested document.
Some synthetic runs print successful checks then exit 134 with "A connection to
the bus can't be made" during shutdown. This is unresolved and must not be hidden
or called a clean passing suite. RSS sums include shared pages and all descendants;
they are not a measurement of uniquely reclaimable physical memory.

## Next changes, in order

1. Put the harness in the repository with a bounded private-session runner,
   document-specific load assertions, stable view/process identifiers, and
   shutdown checks. Separate fixture/shutdown errors from application failures.
2. Add ownership tests against the actual surface/compositor, not just the stub.
   Cover removal of the last/middle registry entry, queued/in-flight buffers,
   delayed callbacks, failed commits, late Flutter frames and ID reuse.
3. Repair items 1–7 as coherent lifetime changes, with debug and release coverage.
   Never release a scanout buffer merely because its webview closed.
4. Separately investigate WebKit retention with real sites: process pool,
   frame/site-isolation counts, service/shared workers and outstanding shutdown
   scopes. Upstream release source lists these as reasons to defer termination;
   determine which applies to the installed custom package before changing it.
5. Retest simultaneous views, navigation, video, close during load and reopening.
   Only then deploy another behavior change.

Previous claims that delayed exits proved "WebKit only cleans up when another
view closes", and that one view necessarily owns one process, are withdrawn.

## Implemented and verified after the review

The ownership map/defects above describe **baseline d964a50**, not the revised
code. Items 1–7 are repaired together:

- Stop the producer, unregister and clear the queued surface, then release the
  player reference. KMS keeps independent references until retirement.
- Remove the unresolved-texture reference cycle; release the last queued buffer
  in surface destruction. Invoke producer release callbacks outside its lock.
- Use registry lookup plus a reference in **both** build modes. Surface IDs are
  monotonic tokens, not reusable addresses. Missing late IDs become empty layers.
- Repair inline-array removal without freeing an inline entry; destroy old
  surfaces outside the compositor lock.
- Add cancellable scanout listeners and an explicit error/ownership contract.
  If failure races a dispatched callback, report callback acceptance instead of
  inviting the caller to free its userdata twice.
- Use per-frame completion tickets. Old callbacks cannot acknowledge a newer
  frame; timeouts cancel idle listeners, and callbacks already dispatched hold
  the state alive independently of the player/surface.
- Also fix adjacent failure-path defects: texture registration returned freed
  storage, failed texture resolution dereferenced NULL, and a failed KMS-builder
  allocation tried to unref NULL.

The standalone harness is now `test/webview/`, with repeatable build/run
instructions. Native tests cover actual surface/compositor code (including
concurrent unregister/composition), failure paths and cross-thread completion.
All 8 CTest executables pass in Debug and Release. The 4 lifecycle-related
executables pass ASan/UBSan with leak detection enabled. These sanitizer tests
are native state tests, **not** sanitizer builds of WebKit or the full appliance.

### Shutdown abort identified separately

A private-session GDB run reproduced the post-test SIGABRT. The backtrace was
`WebKit::WebsiteDataStore::~WebsiteDataStore` via GObject finalization and
`__run_exit_handlers` on the process's exiting thread. Upstream
`webkit_network_session_get_default()` uses a function-static `GRefPtr`.
Our WebKit runs on a separate thread, so the default session's static destructor
runs on the wrong thread. Use an explicitly created shared network session,
with the **same persistent default directories/settings**, owned for the life
of the WebKit thread just like the context/display. This is not a per-view
session, ephemeral-profile change or attempt to forcibly stop worker processes.
The formerly aborting runs now exit 0 normally.

### Actual process-retention causes (installed WPE 2.54.0)

The installed package is `2.54.0-2~deb13telosnex2`. Enabling its existing release
logs (`WEBKIT_DEBUG_OUTPUT=stderr`, `WEBKIT_DEBUG=Process,ProcessSwapping,ServiceWorker`)
showed **two different reasons** on ESPN after pages/frames/shutdown scopes were
released:

1. `canTerminateAuxiliaryProcess: returns false because process is running
   service workers` for the original process.
2. A later process was added to `WebProcessCache` with **capacity 32**, despite
   our document-viewer setting. Thus the earlier claim that this setting had
   disabled process caching was incorrect.

Matching upstream 2.54 source explains (2): `WebProcessCache::updateCapacity()`
reads the process-global cache model in the process-pool constructor, but
`WebProcessPool::setCacheModel()` updates back/forward/resource caches without
updating the already-created process-cache capacity. The revised startup sets
DocumentViewer on a temporary context, releases that unused context, then
creates the actual shared context with the model already in effect. This uses
public APIs, without relying on passing NULL to the cache-model setter.

After this initialization-order change, logs report `Cache is disabled because
cache model is not PrimaryWebBrowser` and `Not caching process because the
cache has no capacity`. In two ESPN cycles the second process exited; the
worker process remained temporarily, then exited naturally about 10.5 seconds
after the last close. Three YouTube cycles likewise reached zero web processes
about 10 seconds after the last close. The shared network process remained
(~100–112 MB summed descendant RSS). This establishes cache and worker retention
for these runs, not a universal time bound for arbitrary websites/workers.

No process-termination workaround was reintroduced. Do not change that just to
make a five-second process-count assertion pass.

### Hardware coverage and remaining limits

Pi integration tests pass with both direct-buffer and GPU-copy paths: input,
JavaScript, requested-document loads, red/green VP8 playback, navigation away
from video, simultaneous views, repeated reopen/close and close during load.
The harness uses a fake display/flip clock; these are **not** full application
or real KMS end-to-end tests. Keep a known-good device binary for rollback and
check the appliance's actual open/close/reopen flow before calling deployment
accepted. Live-site pages/ads/workers can change between runs. No bounded test
proves absence of all leaks or guarantees immediate release on every site.

### Test deployment, 2026-10-06 16:40 PDT

Installed the checked Release build only in the existing experimental runner
location, `/opt/telosnex-direct-test/flutter-pi`. No app bundle, WPE package,
service environment or production `/opt/telosnex/flutter-pi` was replaced.
Candidate SHA-256:
`c5cd2079d1358dd2ffdd49803ce6f37b870915a472ede15873dadbd3052af053`.

Saved baseline to `/opt/telosnex-direct-test/flutter-pi.pre-lifecycle`, SHA-256:
`73d195a5057b534b05570f2f0c0f2795ebe0b2ae55e1164926416e1f66fc9a43`.
Service restarted successfully, active/running with zero automatic restarts;
Flutter/KMS startup produced normal frame-pacing logs. That is a startup smoke
check, not acceptance of the full application webview open/close/reopen flow.

Rollback, preserving the running executable until the atomic rename:

```sh
sudo cp -p /opt/telosnex-direct-test/flutter-pi.pre-lifecycle /opt/telosnex-direct-test/flutter-pi.next
sudo mv /opt/telosnex-direct-test/flutter-pi.next /opt/telosnex-direct-test/flutter-pi
sudo systemctl restart telosnex.service
```

Final local-fixture runs with the cache-initialization fix: six cycles each for
scanout and copy, all functional checks passed, all local-page web processes
exited after every close (slowest 30/31 ms), both programs exited 0. Logs:
`/var/tmp/lifecycle-final-{1,0}.log`,
`/var/tmp/lifecycle-espn-cache-init.log`, and
`/var/tmp/lifecycle-youtube-cache-init.log`.

### Actual-app follow-up and temporary logging

The user reported successful app use, including YouTube video playback. The
view closed at 16:48:36 PDT. GPU teardown was logged and the service had no
crash/restart, but one WPEWebProcess (~125 MiB RSS) remained at 16:50:53.
Thus actual-app retention is **not resolved/attributed** by the fixture result.
There were 27 backing-store failures and 11 fence timeouts during that session;
earlier logs also contained both classes, so this does not establish a new
regression. No further such errors appeared during the subsequent idle checks.

At 16:51 PDT, enabled targeted logging via a temporary runtime-only drop-in:
`/run/systemd/system/telosnex.service.d/90-webkit-lifecycle-debug.conf`, setting
`WEBKIT_DEBUG_OUTPUT=stderr` and
`WEBKIT_DEBUG=Process,ProcessSwapping,ProcessSuspension,ServiceWorker`.
Restarted the service and confirmed these variables in its process environment.
The restart resets the old observation; do not count its process disappearance
as natural cleanup. Next compare actual-app YouTube closes without/with video.

Remove **only this diagnostic override** after collecting the reproductions:

```sh
sudo rm /run/systemd/system/telosnex.service.d/90-webkit-lifecycle-debug.conf
sudo systemctl daemon-reload
sudo systemctl restart telosnex.service
```

### Actual-app YouTube A/B result

With process-lifetime logging enabled, the user's no-playback run closed at
16:51:49 PDT: page PID 111270 shut down immediately and service-worker PID 111080
shut down at 16:51:59 after the network process reported its worker connection
was no longer needed. In the subsequent video-playback run, the view closed at
16:53:54 (1395 frames); page PID 111658 shut down immediately and worker PID
111686 shut down at 16:54:04 for the same reason. At 16:54:46, **before any
restart**, no web processes remained; only the shared network process remained.
Both runs had zero automatic service restarts. Cache logs confirmed zero
process-cache capacity.

The comparison did not reproduce the earlier >2-minute process retention and
does not establish video as its cause. Do not claim the earlier retained process
was explained, or all leaks ruled out. Backing-store failures and two fence
timeouts were still logged during the playback run; those are a separate
unresolved rendering issue.

Saved the complete A/B journal locally to
`~/.cache/tpi-device-study/lifecycle-fix/actual-app-youtube-ab.log`. Removed only
the temporary runtime logging drop-in and restarted the service afterward;
confirmed healthy service startup and absence of `WEBKIT_DEBUG*` in its process
environment. No new cleanup/termination workaround was added.
