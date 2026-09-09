# Trackpad scrolling

Finger/edge scrolling from libinput is translated into Flutter trackpad
`PanZoomStart`, cumulative `PanZoomUpdate`, and `PanZoomEnd` events. Flutter's
scroll recognizers can then estimate velocity and apply their normal ballistic
physics. There is no synthetic scroll timer in the embedder and no new kernel
driver.

## Direction and units

Natural scrolling is the default: fingers move content in the same direction,
on both axes. Set `FLUTTER_PI_TOUCHPAD_NATURAL_SCROLL=0` in the runner's
environment for traditional scrolling, or `1` for natural scrolling. Unset means
natural; invalid values log an error and use natural. This affects **only**
libinput FINGER-source scrolling. Mouse-wheel/trackpoint/continuous scrolling,
clicks, cursor acceleration and tap-to-click settings are unchanged.

The adapter first undoes libinput's own natural-scroll flag, if enabled, then
applies the runner preference when converting physical finger displacement to
Flutter pan. Thus it cannot accidentally invert twice. Finger deltas use
libinput's pointer-pixel units; only wheels retain the old `degrees / 15 * 53`
conversion. Pan is in view coordinates, like relative mouse movement; the
cursor anchor is transformed from display to view coordinates.

## Lifecycle

- Each physical pointer device reserves a separate Flutter gesture ID, distinct
  from the shared mouse cursor and other touchpads.
- A gesture fixes its cursor anchor at first movement, begins with zero pan,
  and includes the first movement in its first Update.
- Zero on a **present** axis means that axis stopped. An absent axis means
  unchanged, not stopped. End is sent only once all participating axes stop.
- End preserves accumulated pan without adding a zero-motion Update that
  would corrupt velocity sampling. Timestamps remain monotonic microseconds.
- Device removal/seat suspension emits End (if active), then Remove. Destruction
  without callbacks remains silent.
- Only legacy `POINTER_AXIS` is consumed. Newer libinput also emits
  `POINTER_SCROLL_*`; consuming both would double-scroll. This change deliberately
  leaves the wheel stream alone rather than also changing high-resolution wheel
  support.

A fresh scrolling gesture cancels Flutter's existing fling. Merely resting a
finger is **not** guaranteed to cancel it: the adapter does not consume raw
contact or libinput hold gestures. Pinch/rotation and hardware tuning are outside
this change.

## Tests

Host-only, with address and undefined-behavior sanitizers:

```sh
git submodule update --init third_party/Unity
./test/run_user_input_scroll_tests.sh
```

On a Linux machine with the normal build dependencies, configure with
`-DENABLE_TESTS=ON`, build, then run:

```sh
ctest --test-dir build --output-on-failure -R '^(user_input_scroll_test|window_test)$'
```

The 16 scroll tests cover direction/configuration, cumulative pan, subpixel
precision, timestamps, single/both/staggered axis stops, duplicate stops,
recontact, concurrent devices/shared mouse, removal and unchanged wheel output.
The complete modified runner has also been built as ARM64 with OpenGL, Vulkan,
GStreamer/WPE and session support. No live runner was replaced during validation.

## Telosnex rollout dependency

Native Flutter Scrollables already accept these events. Custom widgets that
listen **only** for `PointerScrollEvent` need a companion update. In particular,
Telosnex's `FlutterPiWpeTexture` must receive the matching pan listener change
before or alongside this runner. It translates content pan into WPE axis ticks
and reuses its existing touch-fling implementation; it must not negate pan as
it does wheel offsets.

Companion app tests exercise both-axis natural direction and release inertia
with Telosnex's Linux scroll behavior, cancellation by a new gesture, WPE
texture scaling, wheel compatibility, and timer disposal. Physical Argon feel,
webpage behavior and acceptance remain to be checked during a coordinated
rollout. Do not promote this local candidate via an ad-hoc overwrite: publish
and pin the reviewed fork revision, produce its expected runner digest, and
use the normal maintenance/deployment path together with the app change.
