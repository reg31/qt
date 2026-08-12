# Qt Quick Threaded Render Loop Modifications

> This README documents only the experimental `qsgthreadedrenderloop.cpp` replacement. The code
> tracks the Qt dev branch, is built as C++23, and is not a supported Qt public API. It requires no
> change to `qsgthreadedrenderloop_p.h`, and `QSGThreadedRenderLoop` method signatures are unchanged.

## Purpose

The modified `qsgthreadedrenderloop.cpp` focuses on two areas:

- Overlap RHI device creation with the interval between window visibility and surface exposure.
- Prevent rendering against an Android surface while the platform is destroying or recreating it.

The implementation also uses value-based render-thread events and modern C++ synchronization,
without changing Qt Quick's scene graph or rendering contracts.

## RHI Warm-Up

`QSGRenderThread::ensureRhiDevice()` separates device and render-context initialization from
swapchain creation. When a window is visible but not yet exposed, `exposureChanged()` can create
and start its render thread. The thread then creates the QRhi before the first scene graph sync.

This prewarm window is platform-dependent and may be short or absent. A window with
`visible: false` is not prewarmed. `maybeUpdate()` creates the platform window when necessary,
but `exposureChanged()` starts prewarming only when the window is visible.

OpenGL still requires a compatible surface. The GUI thread therefore creates Qt's fallback
offscreen surface before the render thread starts. Vulkan, D3D, Metal, and OpenGL continue to use
`QSGRhiSupport::createRhi()` and their normal Qt initialization paths.

`ensureRhi()` completes the work that requires exposure:

1. Initialize the render context if warm-up could not do so.
2. Create the swapchain and depth/stencil buffer.
3. Apply the surface format, sample count, proxy data, and renderer viewport.

The native driver initialization cost is not eliminated. Warm-up only overlaps that cost with
other startup work when the platform provides enough time before exposure.

## Pipeline Cache

There is no custom pipeline-cache implementation in this file. The Qt dev branch already loads
and saves the automatic QRhi pipeline cache in `QSGRhiSupport::createRhi()` and `destroyRhi()`.

The automatic cache remains enabled by default unless the application disables it through
`QQuickGraphicsConfiguration`. Consequently, the measured `createRhi` duration includes Qt's
built-in cache preparation as well as native device or context creation.

## First Exposure

The render thread publishes `rhiReady` after successful device setup. `deferredExposeRequest`
coordinates the first update while warm-up is still running:

1. `handleExposure()` posts the expose event and starts or wakes the render thread.
2. If the RHI is not ready, the GUI thread returns without starting scene graph synchronization.
3. Once initialization completes, the render thread queues `requestUpdate()`.
4. `handleUpdateRequest()` performs the deferred expose sync after `rhiReady` becomes true.

This avoids synchronizing against a partially initialized graphics stack. It does not promise a
specific startup improvement because surface timing and driver initialization vary by backend and
device.

`exposureChanged()` also validates the surface size. An exposed window with an empty swapchain or
platform-surface size is marked non-renderable and retried after 16 ms. The retry captures the
window with `QPointer`, so destruction during the delay safely cancels the work.

## Render-Thread Events

Private GUI-to-render-thread requests are stored by value in a `std::variant` and queued in two
reused `std::vector` buffers. Draining swaps the producer and consumer buffers, preserving FIFO
order while avoiding a heap allocation for each event object.

The event alternatives cover expose, obscure, sync, resource release, grab, render jobs, and
swapchain release. `WMJobEvent` owns its `QRunnable` with `std::unique_ptr` while queued.

Operations that must wait for render-thread completion use a shared atomic token with
`wait()`/`notify_one()`. Per-frame synchronization uses a monotonically increasing serial:

- The GUI thread posts `WMSyncEvent` with a new serial.
- The render thread stores the completed serial with release ordering.
- The GUI thread waits until the acknowledged serial reaches the posted serial.

On the normal sync path, acknowledgement occurs after `syncSceneGraph()` and `sgrc->endSync()`,
but before deferred-deletion processing. Bailout paths acknowledge without attempting an invalid
sync. This keeps the GUI-thread stall as short as the scene graph contract permits.

The per-frame sync event itself needs no heap allocation. Low-frequency blocking operations such
as grabs and resource release still allocate a shared wait token for safe fire-and-wait ownership.

## Surface Lifecycle Safety

`surfaceAboutToBeDestroyed` is an atomic cross-thread barrier. Rendering and grabs check it before
touching a presentable surface, while `postJob()` rejects new render jobs after teardown begins.
The render path checks surface validity before `beginFrame()` and again before presentation; if
the surface disappears during a frame, the frame ends with `QRhi::SkipPresent`.

The event filter handles two teardown signals:

- `ApplicationStateChange` on Android marks every surface unavailable and waits for each running
  render thread to release its swapchain.
- `QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed` performs the same handshake for one window.

The Android application event filter is temporary but still required. Remove it only after the
QtBase asynchronous Android EGL/Vulkan surface change (`735089`) is included in the minimum
supported Qt version. `handleObscurity()` sets the barrier before queuing its render-thread event;
`handleExposure()` clears it for the next surface lifecycle.

## Device Loss and Swapchain Recovery

`beginFrame()` and `endFrame()` results are checked. Device loss tears down the scene graph and
owned RHI. An out-of-date swapchain is marked non-renderable and an update is queued so it can be
recreated later.

If initial swapchain creation fails and `QSGRhiSupport` permits a software fallback, the render
loop tears down its graphics state and retries with software rendering preferred. If initial QRhi
creation itself fails, `TriggerContextCreationFailure` is posted to the window instead of leaving
the GUI thread waiting indefinitely.

When an RHI is retained while the window swapchain has been released, the render loop handles the
pending sync before `ensureRhi()` reattaches window graphics resources. `syncDoneBeforeEnsure`
then prevents the same request from being synchronized twice in that frame.

`lastFrameValid` allows unchanged frames to end with `QRhi::SkipPresent`, avoiding redundant scene
rendering and presentation when synchronization produced no changes. A frame is considered valid
only after a successful presentation.

Normal resource release continues to honor `isPersistentSceneGraph()` and
`isPersistentGraphics()`. A QRhi supplied by the application is detached but never destroyed by
the render loop. Grabs use the same surface and frame-result checks; a failed grab returns an empty
image, and an out-of-date swapchain schedules recovery.

## Update Scheduling

Updates requested from outside the GUI thread are queued back to the render loop instead of being
dropped. The queued callback retains the window through `QPointer` and looks up its current
`Window` record before use, avoiding stale pointers during shutdown. Scheduling stops when the
render thread is inactive; render-thread-originated updates are also rejected after device loss.
If surface teardown has begun, the later `polishAndSync()` presentability guard safely drops the
work.

When an update is requested during scene graph synchronization, the next request is paced against
the remaining vsync interval instead of being posted immediately when time remains. Vsync health
sampling ignores large timing outliers, switches broken throttling to the system animation timer,
and switches back after a shorter recovery sample confirms normal throttling.

## C++ Usage

The implementation uses modern language and library features where they simplify ownership or
synchronization:

- `std::ranges::find` and `std::ranges::any_of`
- `std::variant` and `std::visit`
- `std::unique_ptr` for queued render jobs
- `std::exchange` for state transitions
- `std::atomic::wait()` and `notify_one()`
- Explicit atomic memory ordering at GUI/render-thread boundaries
- `[[likely]]` and `[[unlikely]]` on measured hot and exceptional paths

This render-loop replacement changes no `QSGThreadedRenderLoop` method signature or framework
header.

## Timing Logs

With `qt.scenegraph.time.renderloop` enabled, a warm Windows MinGW Debug launch using Qt's
default animation driver reported:

```text
RHI warm-up: createRhi=76 ms, rhiSetup=0 ms, renderContext=0 ms, total=76 ms
Frame prepared, polish=0 ms, lock=0 ms, sync=5 ms, animations=4 ms
frame: sync=3 ms, swapchain=2 ms, beginFrame=0 ms, renderSceneGraph=5 ms, endFrame=3 ms, total=14 ms
```

These are measured values, not expected targets. A cold run of the same configuration reported
102 ms in `createRhi` and 24 ms for the first render-thread frame, demonstrating the startup
variance introduced by native driver and operating-system state. Testing with
`QSG_USE_SIMPLE_ANIMATION_DRIVER` showed no startup difference.

The warm-up fields measure:

- `createRhi`: the complete `QSGRhiSupport::createRhi()` call, including native driver
  device/context creation and Qt's built-in pipeline-cache preparation.
- `rhiSetup`: sample-count selection and making the native context current.
- `renderContext`: conditional `QSGDefaultRenderContext::initialize()` work.
- `total`: elapsed time for the complete warm-up function after successful RHI creation.

Each field is truncated independently to whole milliseconds, so the displayed components may
differ from `total` by a few milliseconds. Frame logs separately report sync, swapchain,
begin-frame, scene rendering, end-frame, and total time.

## Validation

The current source passes C++23 syntax checks against local Qt dev branch builds with:

- MinGW 64-bit
- Android NDK ARM64-v8a
- Android NDK armeabi-v7a

These are compile checks, not a substitute for runtime lifecycle testing. Android testing should
cover launch, background/foreground, screen rotation, surface recreation, window destruction,
device loss where available, and both OpenGL and Vulkan backends.
