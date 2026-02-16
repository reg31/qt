// Copyright (C) 2016 The Qt Company Ltd.
// Copyright (C) 2016 Jolla Ltd, author: <gunnar.sletta@jollamobile.com>
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only


#include <QtCore/QMutex>
#include <QtCore/QWaitCondition>
#include <QtCore/QAnimationDriver>
#include <QtCore/QQueue>
#include <QtCore/QTimer>

#include <QtGui/QGuiApplication>
#include <QtGui/QScreen>
#include <QtGui/QOffscreenSurface>

#include <qpa/qwindowsysteminterface.h>

#include <QtQuick/QQuickWindow>
#include <private/qquickwindow_p.h>
#include <private/qquickitem_p.h>
#include <QtGui/qpa/qplatformwindow_p.h>

#include <QtQuick/private/qsgrenderer_p.h>

#include "qsgthreadedrenderloop_p.h"
#include "qsgrhisupport_p.h"
#include <private/qquickanimatorcontroller_p.h>

#include <private/qquickprofiler_p.h>
#include <private/qqmldebugserviceinterfaces_p.h>
#include <private/qqmldebugconnector_p.h>

#include <private/qsgrhishadereffectnode_p.h>
#include <private/qsgdefaultrendercontext_p.h>

#include <qtquick_tracepoints_p.h>

#ifdef Q_OS_DARWIN
#include <QtCore/private/qcore_mac_p.h>
#endif

/*
   Overall design:

   There are two classes here. QSGThreadedRenderLoop and
   QSGRenderThread. All communication between the two is based on
   event passing and we have a number of custom events.

   In this implementation, the render thread is never blocked and the
   GUI thread will initiate a polishAndSync which will block and wait
   for the render thread to pick it up and release the block only
   after the render thread is done syncing. The reason for this
   is:

   1. Clear blocking paradigm. We only have one real "block" point
   (polishAndSync()) and all blocking is initiated by GUI and picked
   up by Render at specific times based on events. This makes the
   execution deterministic.

   2. Render does not have to interact with GUI. This is done so that
   the render thread can run its own animation system which stays
   alive even when the GUI thread is blocked doing i/o, object
   instantiation, QPainter-painting or any other non-trivial task.

   ---

   There is one thread per window and one QRhi instance per thread.

   ---

   The render thread has affinity to the GUI thread until a window
   is shown. From that moment and until the window is destroyed, it
   will have affinity to the render thread. (moved back at the end
   of run for cleanup).

   ---

   The render loop is active while any window is exposed. All visible
   windows are tracked, but only exposed windows are actually added to
   the render thread and rendered. That means that if all windows are
   obscured, we might end up cleaning up the SG and GL context (if all
   windows have disabled persistency). Especially for multiprocess,
   low-end systems, this should be quite important.

 */

QT_BEGIN_NAMESPACE

Q_TRACE_POINT(qtquick, QSG_polishAndSync_entry)
Q_TRACE_POINT(qtquick, QSG_polishAndSync_exit)
Q_TRACE_POINT(qtquick, QSG_wait_entry)
Q_TRACE_POINT(qtquick, QSG_wait_exit)
Q_TRACE_POINT(qtquick, QSG_syncAndRender_entry)
Q_TRACE_POINT(qtquick, QSG_syncAndRender_exit)
Q_TRACE_POINT(qtquick, QSG_animations_entry)
Q_TRACE_POINT(qtquick, QSG_animations_exit)

#define QSG_RT_PAD "                    (RT) %s"

extern Q_GUI_EXPORT QImage qt_gl_read_framebuffer(const QSize &size, bool alpha_format, bool include_alpha);

// RL: Render Loop
// RT: Render Thread


QSGThreadedRenderLoop::Window *QSGThreadedRenderLoop::windowFor(QQuickWindow *window)
{
    for (const auto &t : std::as_const(m_windows)) {
        if (t.window == window)
            return const_cast<Window *>(&t);
    }
    return nullptr;
}

class WMWindowEvent : public QEvent
{
public:
    WMWindowEvent(QQuickWindow *c, QEvent::Type type) : QEvent(type), window(c) { }
    QQuickWindow *window;
};

class WMTryReleaseEvent : public WMWindowEvent
{
public:
    WMTryReleaseEvent(QQuickWindow *win, bool destroy, bool needsFallbackSurface)
        : WMWindowEvent(win, QEvent::Type(WM_TryRelease))
        , inDestructor(destroy)
        , needsFallback(needsFallbackSurface)
    {}

    bool inDestructor;
    bool needsFallback;
};

class WMSyncEvent : public WMWindowEvent
{
public:
    WMSyncEvent(QQuickWindow *c, bool inExpose, bool force, const QRhiSwapChainProxyData &scProxyData)
        : WMWindowEvent(c, QEvent::Type(WM_RequestSync))
        , size(c->size())
        , dpr(float(c->effectiveDevicePixelRatio()))
        , syncInExpose(inExpose)
        , forceRenderPass(force)
        , scProxyData(scProxyData)
    {}
    QSize size;
    float dpr;
    bool syncInExpose;
    bool forceRenderPass;
    QRhiSwapChainProxyData scProxyData;
};


class WMGrabEvent : public WMWindowEvent
{
public:
    WMGrabEvent(QQuickWindow *c, QImage *result) :
        WMWindowEvent(c, QEvent::Type(WM_Grab)), image(result) {}
    QImage *image;
};

class WMJobEvent : public WMWindowEvent
{
public:
    WMJobEvent(QQuickWindow *c, QRunnable *postedJob)
        : WMWindowEvent(c, QEvent::Type(WM_PostJob)), job(postedJob) {}
    ~WMJobEvent() { delete job; }
    QRunnable *job;
};

class WMReleaseSwapchainEvent : public WMWindowEvent
{
public:
    WMReleaseSwapchainEvent(QQuickWindow *c) :
        WMWindowEvent(c, QEvent::Type(WM_ReleaseSwapchain)) { }
};

class QSGRenderThreadEventQueue : public QQueue<QEvent *>
{
public:
    QSGRenderThreadEventQueue()
        : waiting(false)
    {
    }

    void addEvent(QEvent *e) {
        mutex.lock();
        enqueue(e);
        if (waiting)
            condition.wakeOne();
        mutex.unlock();
    }

    QEvent *takeEvent(bool wait) {
        mutex.lock();
        if (size() == 0 && wait) {
            waiting = true;
            condition.wait(&mutex);
            waiting = false;
        }
        QEvent *e = dequeue();
        mutex.unlock();
        return e;
    }

    bool hasMoreEvents() {
        mutex.lock();
        bool has = !isEmpty();
        mutex.unlock();
        return has;
    }

private:
    QMutex mutex;
    QWaitCondition condition;
    bool waiting;
};


class QSGRenderThread : public QThread
{
    Q_OBJECT
public:
    QSGRenderThread(QSGThreadedRenderLoop *w, QSGRenderContext *renderContext)
        : wm(w)
        , rhi(nullptr)
        , ownRhi(true)
        , offscreenSurface(nullptr)
        , animatorDriver(nullptr)
        , pendingUpdate(0)
        , sleeping(false)
        , active(false)
        , window(nullptr)
        , stopEventProcessing(false)
    {
        sgrc = static_cast<QSGDefaultRenderContext *>(renderContext);
#if defined(Q_OS_QNX) || defined(Q_OS_INTEGRITY)
        // The render thread requires a larger stack than the default (256k).
        setStackSize(1024 * 1024);
#endif
    }

    ~QSGRenderThread()
    {
        delete sgrc;
        delete offscreenSurface;
    }

    void invalidateGraphics(QQuickWindow *window, bool inDestructor);

    bool event(QEvent *) override;
    void run() override;

    void syncAndRender();
    void sync(bool inExpose);

    void requestRepaint()
    {
        if (sleeping)
            stopEventProcessing = true;
        if (window)
            pendingUpdate |= RepaintRequest;
    }

    void processEventsAndWaitForMore();
    void processEvents();
    void postEvent(QEvent *e);

public:
    enum {
        SyncRequest         = 0x01,
        RepaintRequest      = 0x02,
        ExposeRequest       = 0x04 | RepaintRequest | SyncRequest
    };

    void ensureRhi();
    void teardownGraphics();
    void handleDeviceLoss();

    QSGThreadedRenderLoop *wm;
    QRhi *rhi;
    bool ownRhi;
    QSGDefaultRenderContext *sgrc;
    QOffscreenSurface *offscreenSurface;

    QAnimationDriver *animatorDriver;

    uint pendingUpdate;
    bool sleeping;

    volatile bool active;

    QMutex mutex;
    QWaitCondition waitCondition;

    QElapsedTimer m_threadTimeBetweenRenders;

    QQuickWindow *window; // Will be 0 when window is not exposed
    QSize windowSize;
    float dpr = 1;
    QRhiSwapChainProxyData scProxyData;
    int rhiSampleCount = 1;
    bool rhiDeviceLost = false;
    bool rhiDoomed = false;
    bool guiNotifiedAboutRhiFailure = false;
    bool swRastFallbackDueToSwapchainFailure = false;

    // Local event queue stuff...
    bool stopEventProcessing;
    QSGRenderThreadEventQueue eventQueue;
};

bool QSGRenderThread::event(QEvent *e)
{
    switch ((int) e->type()) {

    case WM_Obscure: {
        qCDebug(QSG_LOG_RENDERLOOP, QSG_RT_PAD, "WM_Obscure");

        Q_ASSERT(!window || window == static_cast<WMWindowEvent *>(e)->window);

        mutex.lock();
        if (window) {
            QQuickWindowPrivate::get(window)->fireAboutToStop();
            qCDebug(QSG_LOG_RENDERLOOP, QSG_RT_PAD, "- window removed");
            window = nullptr;
        }
        waitCondition.wakeOne();
        mutex.unlock();

        return true; }


    case WM_Exposed: {
        qCDebug(QSG_LOG_RENDERLOOP, QSG_RT_PAD, "WM_Exposed");

        mutex.lock();
        window = static_cast<WMWindowEvent *>(e)->window;
        waitCondition.wakeOne();
        mutex.unlock();

        return true; }

    case WM_RequestSync: {
        qCDebug(QSG_LOG_RENDERLOOP, QSG_RT_PAD, "WM_RequestSync");
        WMSyncEvent *se = static_cast<WMSyncEvent *>(e);
        if (sleeping)
            stopEventProcessing = true;
        window = se->window;
        windowSize = se->size;
        dpr = se->dpr;
        scProxyData = se->scProxyData;

        pendingUpdate |= SyncRequest;
        if (se->syncInExpose) {
            qCDebug(QSG_LOG_RENDERLOOP, QSG_RT_PAD, "- triggered from expose");
            pendingUpdate |= ExposeRequest;
        }
        if (se->forceRenderPass) {
            qCDebug(QSG_LOG_RENDERLOOP, QSG_RT_PAD, "- repaint regardless");
            pendingUpdate |= RepaintRequest;
        }
        return true; }

    case WM_TryRelease: {
        qCDebug(QSG_LOG_RENDERLOOP, QSG_RT_PAD, "WM_TryRelease");
        mutex.lock();
        wm->m_lockedForSync = true;
        WMTryReleaseEvent *wme = static_cast<WMTryReleaseEvent *>(e);
        if (!window || wme->inDestructor) {
            qCDebug(QSG_LOG_RENDERLOOP, QSG_RT_PAD, "- setting exit flag and invalidating");
            invalidateGraphics(wme->window, wme->inDestructor);
            active = rhi != nullptr;
            Q_ASSERT_X(!wme->inDestructor || !active, "QSGRenderThread::invalidateGraphics()", "Thread's active state is not set to false when shutting down");
            if (sleeping)
                stopEventProcessing = true;
        } else {
            qCDebug(QSG_LOG_RENDERLOOP, QSG_RT_PAD, "- not releasing because window is still active");
            if (window) {
                QQuickWindowPrivate *d = QQuickWindowPrivate::get(window);
                qCDebug(QSG_LOG_RENDERLOOP, QSG_RT_PAD, "- requesting external renderers such as Quick 3D to release cached resources");
                emit d->context->releaseCachedResourcesRequested();
                if (d->renderer) {
                    qCDebug(QSG_LOG_RENDERLOOP, QSG_RT_PAD, "- requesting renderer to release cached resources");
                    d->renderer->releaseCachedResources();
                }
#if QT_CONFIG(quick_shadereffect)
                QSGRhiShaderEffectNode::garbageCollectMaterialTypeCache(window);
#endif
            }
        }
        waitCondition.wakeOne();
        wm->m_lockedForSync = false;
        mutex.unlock();
        return true;
    }

    case WM_Grab: {
        qCDebug(QSG_LOG_RENDERLOOP, QSG_RT_PAD, "WM_Grab");
        WMGrabEvent *ce = static_cast<WMGrabEvent *>(e);
        Q_ASSERT(ce->window);
        Q_ASSERT(ce->window == window || !window);
        mutex.lock();
        if (ce->window) {
            if (rhi) {
                QQuickWindowPrivate *cd = QQuickWindowPrivate::get(ce->window);
                // The assumption is that the swapchain is usable, because on
                // expose the thread starts up and renders a frame so one cannot
                // get here without having done at least one on-screen frame.
                cd->rhi->beginFrame(cd->swapchain);
                cd->rhi->makeThreadLocalNativeContextCurrent(); // for custom GL rendering before/during/after sync
                cd->syncSceneGraph();
                sgrc->endSync();
                cd->renderSceneGraph();
                *ce->image = QSGRhiSupport::instance()->grabAndBlockInCurrentFrame(rhi, cd->swapchain->currentFrameCommandBuffer());
                cd->rhi->endFrame(cd->swapchain, QRhi::SkipPresent);
            }
            ce->image->setDevicePixelRatio(ce->window->effectiveDevicePixelRatio());
        }
        qCDebug(QSG_LOG_RENDERLOOP, QSG_RT_PAD, "- waking gui to handle result");
        waitCondition.wakeOne();
        mutex.unlock();
        return true;
    }

    case WM_PostJob: {
        qCDebug(QSG_LOG_RENDERLOOP, QSG_RT_PAD, "WM_PostJob");
        WMJobEvent *ce = static_cast<WMJobEvent *>(e);
        Q_ASSERT(ce->window == window);
        if (window) {
            if (rhi)
                rhi->makeThreadLocalNativeContextCurrent();
            ce->job->run();
            delete ce->job;
            ce->job = nullptr;
            qCDebug(QSG_LOG_RENDERLOOP, QSG_RT_PAD, "- job done");
        }
        return true;
    }

    case WM_ReleaseSwapchain: {
        qCDebug(QSG_LOG_RENDERLOOP, QSG_RT_PAD, "WM_ReleaseSwapchain");
        WMReleaseSwapchainEvent *ce = static_cast<WMReleaseSwapchainEvent *>(e);
        // forget about 'window' here that may be null when already unexposed
        Q_ASSERT(ce->window);
        mutex.lock();
        if (ce->window) {
            wm->releaseSwapchain(ce->window);
            qCDebug(QSG_LOG_RENDERLOOP, QSG_RT_PAD, "- swapchain released");
        }
        waitCondition.wakeOne();
        mutex.unlock();
        return true;
    }

    default:
        break;
    }
    return QThread::event(e);
}

void QSGRenderThread::invalidateGraphics(QQuickWindow *window, bool inDestructor)
{
    qCDebug(QSG_LOG_RENDERLOOP, QSG_RT_PAD, "invalidateGraphics()");

    if (!rhi)
        return;

    if (!window) {
        qCWarning(QSG_LOG_RENDERLOOP, "QSGThreadedRenderLoop:QSGRenderThread: no window to make current...");
        return;
    }

    bool wipeSG = inDestructor || !window->isPersistentSceneGraph();
    bool wipeGraphics = inDestructor || (wipeSG && !window->isPersistentGraphics());

    rhi->makeThreadLocalNativeContextCurrent();

    QQuickWindowPrivate *dd = QQuickWindowPrivate::get(window);

    // The canvas nodes must be cleaned up regardless if we are in the destructor..
    if (wipeSG) {
        dd->cleanupNodesOnShutdown();
#if QT_CONFIG(quick_shadereffect)
        QSGRhiShaderEffectNode::resetMaterialTypeCache(window);
#endif
    } else {
        qCDebug(QSG_LOG_RENDERLOOP, QSG_RT_PAD, "- persistent SG, avoiding cleanup");
        return;
    }

    sgrc->invalidate();
    QCoreApplication::processEvents();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    if (inDestructor)
        dd->animationController.reset();

    qCDebug(QSG_LOG_RENDERLOOP, QSG_RT_PAD, "- invalidating scene graph");

    if (wipeGraphics) {
        if (dd->swapchain) {
            if (window->handle()) {
                // We get here when exiting via QCoreApplication::quit() instead of
                // through QWindow::close().
                wm->releaseSwapchain(window);
            } else {
                qWarning("QSGThreadedRenderLoop cleanup with QQuickWindow %p swapchain %p still alive, this should not happen.",
                         window, dd->swapchain);
            }
        }
        if (ownRhi)
            QSGRhiSupport::instance()->destroyRhi(rhi, dd->graphicsConfig);
        rhi = nullptr;
        dd->rhi = nullptr;
        qCDebug(QSG_LOG_RENDERLOOP, QSG_RT_PAD, "- QRhi destroyed");
    } else {
        qCDebug(QSG_LOG_RENDERLOOP, QSG_RT_PAD, "- persistent GL, avoiding cleanup");
    }
}

/*
    Enters the mutex lock to make sure GUI is blocking and performs
    sync, then wakes GUI.
 */
void QSGRenderThread::sync(bool inExpose)
{
    qCDebug(QSG_LOG_RENDERLOOP, QSG_RT_PAD, "sync()");
    mutex.lock();

    Q_ASSERT_X(wm->m_lockedForSync, "QSGRenderThread::sync()", "sync triggered on bad terms as gui is not already locked...");

    bool canSync = true;
    if (rhi) {
        if (windowSize.width() > 0 && windowSize.height() > 0) {
            // With the rhi making the (OpenGL) context current serves only one
            // purpose: to enable external OpenGL rendering connected to one of
            // the QQuickWindow signals (beforeSynchronizing, beforeRendering,
            // etc.) to function like it did on the direct OpenGL path. For our
            // own rendering this call would not be necessary.
            rhi->makeThreadLocalNativeContextCurrent();
        } else {
            // Zero size windows do not initialize a swapchain and
            // rendercontext. So no sync or render can be done then.
            canSync = false;
        }
    } else {
        canSync = false;
    }
    if (canSync) {
        QQuickWindowPrivate *d = QQuickWindowPrivate::get(window);
        // If the scene graph was touched since the last sync() make sure it sends the
        // changed signal.
        if (d->renderer)
            d->renderer->clearChangedFlag();
        d->syncSceneGraph();
        sgrc->endSync();

        // Process deferred deletes now, directly after the sync as
        // deleteLater on the GUI must now also have resulted in SG changes
        // and the delete is a safe operation.
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    } else {
        qCDebug(QSG_LOG_RENDERLOOP, QSG_RT_PAD, "- window has bad size, sync aborted");
    }

    // Two special cases: For grabs we do not care about blocking the gui
    // (main) thread. When this is from an expose, we will keep locked until
    // the frame is rendered (submitted), so in that case waking happens later
    // in syncAndRender(). Otherwise, wake now and let the main thread go on
    // while we render.
    if (!inExpose) {
        qCDebug(QSG_LOG_RENDERLOOP, QSG_RT_PAD, "- sync complete, waking Gui");
        waitCondition.wakeOne();
        mutex.unlock();
    }
}

void QSGRenderThread::teardownGraphics()
{
    QQuickWindowPrivate *wd = QQuickWindowPrivate::get(window);
    wd->cleanupNodesOnShutdown();
    sgrc->invalidate();
    wm->releaseSwapchain(window);
    if (ownRhi)
        QSGRhiSupport::instance()->destroyRhi(rhi, {});
    rhi = nullptr;
}

void QSGRenderThread::handleDeviceLoss()
{
    if (!rhi || !rhi->isDeviceLost())
        return;

    qWarning("Graphics device lost, cleaning up scenegraph and releasing RHI");
    teardownGraphics();
    rhiDeviceLost = true;
}

void QSGRenderThread::syncAndRender()
{
    const bool profileFrames = QSG_LOG_TIME_RENDERLOOP().isDebugEnabled();
    QElapsedTimer threadTimer;
    qint64 syncTime = 0, renderTime = 0;
    if (profileFrames) [[unlikely]]
        threadTimer.start();
    Q_TRACE_SCOPE(QSG_syncAndRender);
    Q_QUICK_SG_PROFILE_START(QQuickProfiler::SceneGraphRenderLoopFrame);
    Q_TRACE(QSG_sync_entry);

    qCDebug(QSG_LOG_RENDERLOOP, QSG_RT_PAD, "syncAndRender()");

    if (profileFrames) [[unlikely]] {
        const qint64 elapsedSinceLastMs = m_threadTimeBetweenRenders.restart();
        qCDebug(QSG_LOG_TIME_RENDERLOOP, "[window %p][render thread %p] syncAndRender: start, elapsed since last call: %d ms",
                window,
                QThread::currentThread(),
                int(elapsedSinceLastMs));
    }

    QQuickWindowPrivate *d = QQuickWindowPrivate::get(window);

    const bool syncRequested = (pendingUpdate & SyncRequest);
    const bool exposeRequested = (pendingUpdate & ExposeRequest) == ExposeRequest;
    pendingUpdate = 0;

    QQuickWindowPrivate *cd = QQuickWindowPrivate::get(window);
    QSGRhiSupport *rhiSupport = QSGRhiSupport::instance();
    const bool hasValidSwapChain = (cd->swapchain && windowSize.width() > 0 && windowSize.height() > 0);
    if (hasValidSwapChain) [[likely]] {
        cd->swapchain->setProxyData(scProxyData);
        const QSize effectiveOutputSize = cd->swapchain->surfacePixelSize();
        if (effectiveOutputSize.isEmpty()) [[unlikely]] {
            if (syncRequested) [[likely]] {
                mutex.lock();
                waitCondition.wakeOne();
                mutex.unlock();
            }
            return;
        }

        const QSize previousOutputSize = cd->swapchain->currentPixelSize();
        if (previousOutputSize != effectiveOutputSize || cd->swapchainJustBecameRenderable) [[unlikely]] {
            if (cd->swapchainJustBecameRenderable)
                qCDebug(QSG_LOG_RENDERLOOP, QSG_RT_PAD, "just became exposed");

            cd->hasActiveSwapchain = cd->swapchain->createOrResize();
            if (!cd->hasActiveSwapchain) [[unlikely]] {
                bool bailOut = false;
                if (rhi->isDeviceLost()) {
                    handleDeviceLoss();
                    bailOut = true;
                } else if (previousOutputSize.isEmpty() && !swRastFallbackDueToSwapchainFailure && rhiSupport->attemptReinitWithSwRastUponFail()) {
                    qWarning("Failed to create swapchain."
                             " Retrying by requesting a software rasterizer, if applicable for the 3D API implementation.");
                    swRastFallbackDueToSwapchainFailure = true;
                    teardownGraphics();
                    bailOut = true;
                }
                if (bailOut) {
                    QCoreApplication::postEvent(window, new QEvent(QEvent::Type(QQuickWindowPrivate::FullUpdateRequest)));
                    if (syncRequested) {
                        qCDebug(QSG_LOG_RENDERLOOP, QSG_RT_PAD, "- bailing out due to failed swapchain init, wake Gui");
                        mutex.lock();
                        waitCondition.wakeOne();
                        mutex.unlock();
                    }
                    return;
                }
            }

            cd->swapchainJustBecameRenderable = false;
            cd->hasRenderableSwapchain = cd->hasActiveSwapchain;

            if (!cd->hasActiveSwapchain) [[unlikely]]
                qWarning("Failed to build or resize swapchain");
            else
                qCDebug(QSG_LOG_RENDERLOOP) << "rhi swapchain size" << cd->swapchain->currentPixelSize();
        }

        emit window->beforeFrameBegin();

        Q_ASSERT(rhi == cd->rhi);
        QRhi::FrameOpResult frameResult = rhi->beginFrame(cd->swapchain);
        if (frameResult != QRhi::FrameOpSuccess) [[unlikely]] {
            if (frameResult == QRhi::FrameOpDeviceLost)
                handleDeviceLoss();
            else if (frameResult == QRhi::FrameOpError)
                qWarning("Failed to start frame");
            if (frameResult == QRhi::FrameOpDeviceLost || frameResult == QRhi::FrameOpSwapChainOutOfDate)
                QCoreApplication::postEvent(window, new QEvent(QEvent::Type(QQuickWindowPrivate::FullUpdateRequest)));
            if (syncRequested) {
                qCDebug(QSG_LOG_RENDERLOOP, QSG_RT_PAD, "- bailing out due to failed beginFrame, wake Gui");
                mutex.lock();
                waitCondition.wakeOne();
                mutex.unlock();
            }
            emit window->afterFrameEnd();
            return;
        }
    }

    if (syncRequested) [[likely]] {
        qCDebug(QSG_LOG_RENDERLOOP, QSG_RT_PAD, "- updatePending, doing sync");
        sync(exposeRequested);
    }
#ifndef QSG_NO_RENDER_TIMING
    if (profileFrames) [[unlikely]]
        syncTime = threadTimer.nsecsElapsed();
#endif
    Q_TRACE(QSG_sync_exit);
    Q_QUICK_SG_PROFILE_RECORD(QQuickProfiler::SceneGraphRenderLoopFrame,
                              QQuickProfiler::SceneGraphRenderLoopSync);

    qCDebug(QSG_LOG_RENDERLOOP, QSG_RT_PAD, "- rendering started");

    Q_TRACE(QSG_render_entry);

    pendingUpdate = 0;

    if (animatorDriver->isRunning()) [[unlikely]] {
        d->animationController->lock();
        animatorDriver->advance();
        d->animationController->unlock();
    }

    const bool canRender = d->renderer && hasValidSwapChain;
    double lastCompletedGpuTime = 0;
    if (canRender) [[likely]] {
        if (!syncRequested)
            rhi->makeThreadLocalNativeContextCurrent();

        d->renderSceneGraph();

        if (profileFrames) [[unlikely]]
            renderTime = threadTimer.nsecsElapsed();
        Q_TRACE(QSG_render_exit);
        Q_QUICK_SG_PROFILE_RECORD(QQuickProfiler::SceneGraphRenderLoopFrame,
                                  QQuickProfiler::SceneGraphRenderLoopRender);
        Q_TRACE(QSG_swap_entry);

        QRhi::FrameOpResult frameResult = rhi->endFrame(cd->swapchain);
        if (frameResult != QRhi::FrameOpSuccess) [[unlikely]] {
            if (frameResult == QRhi::FrameOpDeviceLost)
                handleDeviceLoss();
            else if (frameResult == QRhi::FrameOpError)
                qWarning("Failed to end frame");
            if (frameResult == QRhi::FrameOpDeviceLost || frameResult == QRhi::FrameOpSwapChainOutOfDate)
                QCoreApplication::postEvent(window, new QEvent(QEvent::Type(QQuickWindowPrivate::FullUpdateRequest)));
        } else {
            lastCompletedGpuTime = cd->swapchain->currentFrameCommandBuffer()->lastCompletedGpuTime();
        }
        d->fireFrameSwapped();
    } else {
        Q_TRACE(QSG_render_exit);
        Q_QUICK_SG_PROFILE_SKIP(QQuickProfiler::SceneGraphRenderLoopFrame,
                                QQuickProfiler::SceneGraphRenderLoopSync, 1);
        Q_TRACE(QSG_swap_entry);
        qCDebug(QSG_LOG_RENDERLOOP, QSG_RT_PAD, "- window not ready, skipping render");
        if (cd->swapchain && rhi->isRecordingFrame())
            rhi->endFrame(cd->swapchain, QRhi::SkipPresent);
    }

    qCDebug(QSG_LOG_RENDERLOOP, QSG_RT_PAD, "- rendering done");

   if (hasValidSwapChain)
        emit window->afterFrameEnd();

    if (exposeRequested) {
        qCDebug(QSG_LOG_RENDERLOOP, QSG_RT_PAD, "- wake Gui after expose");
        waitCondition.wakeOne();
        mutex.unlock();
    }

    if (profileFrames) [[unlikely]] {
        qCDebug(QSG_LOG_TIME_RENDERLOOP,
                "[window %p][render thread %p] syncAndRender: frame rendered in %dms, sync=%d, render=%d, swap=%d",
                window,
                QThread::currentThread(),
                int(threadTimer.elapsed()),
                int((syncTime/1000000)),
                int((renderTime - syncTime) / 1000000),
                int((threadTimer.nsecsElapsed() - renderTime) / 1000000));
        if (!qFuzzyIsNull(lastCompletedGpuTime) && cd->graphicsConfig.timestampsEnabled()) {
            qCDebug(QSG_LOG_TIME_RENDERLOOP, "[window %p][render thread %p] syncAndRender: last retrieved GPU frame time was %.4f ms",
                    window,
                    QThread::currentThread(),
                    lastCompletedGpuTime * 1000.0);
        }
    }

    Q_TRACE(QSG_swap_exit);
    Q_QUICK_SG_PROFILE_END(QQuickProfiler::SceneGraphRenderLoopFrame,
                           QQuickProfiler::SceneGraphRenderLoopSwap);
}


void QSGRenderThread::postEvent(QEvent *e)
{
    eventQueue.addEvent(e);
}

void QSGRenderThread::processEvents()
{
    qCDebug(QSG_LOG_RENDERLOOP, QSG_RT_PAD, "--- begin processEvents()");
    while (eventQueue.hasMoreEvents()) {
        QEvent *e = eventQueue.takeEvent(false);
        event(e);
        delete e;
    }
    qCDebug(QSG_LOG_RENDERLOOP, QSG_RT_PAD, "--- done processEvents()");
}

void QSGRenderThread::processEventsAndWaitForMore()
{
    qCDebug(QSG_LOG_RENDERLOOP, QSG_RT_PAD, "--- begin processEventsAndWaitForMore()");
    stopEventProcessing = false;
    while (!stopEventProcessing) {
        QEvent *e = eventQueue.takeEvent(true);
        event(e);
        delete e;
    }
    qCDebug(QSG_LOG_RENDERLOOP, QSG_RT_PAD, "--- done processEventsAndWaitForMore()");
}
void QSGRenderThread::ensureRhi()
{
    if (!rhi) [[unlikely]] {
        if (rhiDoomed) [[unlikely]] return;
        auto *rhiSupport = QSGRhiSupport::instance();
        auto rhiResult = rhiSupport->createRhi(window, offscreenSurface, swRastFallbackDueToSwapchainFailure);
        rhi = rhiResult.rhi;
        ownRhi = rhiResult.own;
        if (rhi) [[likely]] {
            rhiDeviceLost = false;
            rhiSampleCount = rhiSupport->chooseSampleCountForWindowWithRhi(window, rhi);
            rhi->makeThreadLocalNativeContextCurrent();
        } else {
            if (!rhiDeviceLost) [[likely]] rhiDoomed = true;
            return;
        }
    }

    const QSize pixelSize = windowSize * dpr;
    if (!sgrc->rhi() && pixelSize.isValid()) [[unlikely]] {
        rhi->makeThreadLocalNativeContextCurrent();
        QSGDefaultRenderContext::InitParams params;
        params.rhi = rhi;
        params.sampleCount = rhiSampleCount;
        params.initialSurfacePixelSize = pixelSize;
        params.maybeSurface = window;
        sgrc->initialize(&params);
    }

    auto *cd = QQuickWindowPrivate::get(window);
    if (rhi && !cd->swapchain) [[unlikely]] {
        cd->rhi = rhi;
        const auto requestedFormat = window->requestedFormat();
        QRhiSwapChain::Flags flags = QRhiSwapChain::UsedAsTransferSource;

        if (requestedFormat.alphaBufferSize() > 0) flags |= QRhiSwapChain::SurfaceHasPreMulAlpha;
        if (requestedFormat.swapInterval() == 0) flags |= QRhiSwapChain::NoVSync;

        cd->swapchain = rhi->newSwapChain();
        static const bool depthEnabled = qEnvironmentVariableIsEmpty("QSG_NO_DEPTH_BUFFER");
        if (depthEnabled) [[likely]] {
            cd->depthStencilForSwapchain = rhi->newRenderBuffer(QRhiRenderBuffer::DepthStencil, {}, rhiSampleCount, QRhiRenderBuffer::UsedWithSwapChainOnly);
            cd->swapchain->setDepthStencil(cd->depthStencilForSwapchain);
        }

        cd->swapchain->setWindow(window);
        cd->swapchain->setProxyData(scProxyData);
        QSGRhiSupport::instance()->applySwapChainFormat(cd->swapchain, window);
        cd->swapchain->setSampleCount(rhiSampleCount);
        cd->swapchain->setFlags(flags);
        cd->rpDescForSwapchain = cd->swapchain->newCompatibleRenderPassDescriptor();
        cd->swapchain->setRenderPassDescriptor(cd->rpDescForSwapchain);

        if (auto *renderer = cd->renderer) [[likely]] {
            const QRect viewport(QPoint(0, 0), pixelSize);
            renderer->setDeviceRect(viewport);
            renderer->setViewportRect(viewport);
            renderer->setProjectionMatrixToRect(QRectF(QPointF(0, 0), windowSize));
            renderer->setDevicePixelRatio(dpr);
        }
    }
}

void QSGRenderThread::run()
{
    animatorDriver = sgrc->sceneGraphContext()->createAnimationDriver(nullptr);
    animatorDriver->install();
    if (QQmlDebugConnector::service<QQmlProfilerService>()) [[unlikely]]
        QQuickProfiler::registerAnimationCallback();

    m_threadTimeBetweenRenders.start();

    for (int frames = 0; active; ) [[likely]] {
        std::optional<QMacAutoReleasePool> pool;
#ifdef Q_OS_DARWIN
        pool.emplace();
#endif
        if (window) [[likely]] {
            ensureRhi();
            syncAndRender();
            if (rhiDoomed && !guiNotifiedAboutRhiFailure) [[unlikely]] {
                guiNotifiedAboutRhiFailure = true;
                QCoreApplication::postEvent(window, new QEvent(QEvent::Type(QQuickWindowPrivate::TriggerContextCreationFailure)));
            }
            ++frames;
        } else {
            frames = 0;
        }

        processEvents();
        if (frames < 5) [[unlikely]] {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 0);
        } else {
            QCoreApplication::processEvents();
        }

        if (active && (pendingUpdate == 0 || !window)) [[unlikely]] {
            sleeping = true;
            frames = 0;
            processEventsAndWaitForMore();
            sleeping = false;
        }
    }

    if (rhi) [[likely]] {
        rhi->makeThreadLocalNativeContextCurrent();
    }

    delete animatorDriver;
    animatorDriver = nullptr;

    if (auto target = wm->thread(); target != QThread::currentThread()) {
        sgrc->moveToThread(target);
        moveToThread(target);
    }
}

QSGThreadedRenderLoop::~QSGThreadedRenderLoop()
{
    qDeleteAll(pendingRenderContexts);
    delete sg;
}

QSGRenderContext *QSGThreadedRenderLoop::createRenderContext(QSGContext *sg) const
{
    auto context = sg->createRenderContext();
    pendingRenderContexts.insert(context);
    return context;
}

void QSGThreadedRenderLoop::postUpdateRequest(Window *w)
{
    w->window->requestUpdate();
}

QAnimationDriver *QSGThreadedRenderLoop::animationDriver() const
{
    return m_animation_driver;
}

QSGContext *QSGThreadedRenderLoop::sceneGraphContext() const
{
    return sg;
}

bool QSGThreadedRenderLoop::anyoneShowing() const
{
    for (int i=0; i<m_windows.size(); ++i) {
        QQuickWindow *c = m_windows.at(i).window;
        if (c->isVisible() && c->isExposed())
            return true;
    }
    return false;
}

bool QSGThreadedRenderLoop::interleaveIncubation() const
{
    return m_animation_driver->isRunning() && anyoneShowing();
}

void QSGThreadedRenderLoop::animationStarted()
{
    qCDebug(QSG_LOG_RENDERLOOP, "- animationStarted()");
    startOrStopAnimationTimer();

    for (int i=0; i<m_windows.size(); ++i)
        postUpdateRequest(const_cast<Window *>(&m_windows.at(i)));
}

void QSGThreadedRenderLoop::animationStopped()
{
    qCDebug(QSG_LOG_RENDERLOOP, "- animationStopped()");
    startOrStopAnimationTimer();
}


void QSGThreadedRenderLoop::startOrStopAnimationTimer()
{
    if (!sg->isVSyncDependent(m_animation_driver))
        return;

    int exposedWindows = 0;
    int unthrottledWindows = 0;
    int badVSync = 0;
    const Window *theOne = nullptr;
    for (int i=0; i<m_windows.size(); ++i) {
        const Window &w = m_windows.at(i);
        if (w.window->isVisible() && w.window->isExposed()) {
            ++exposedWindows;
            theOne = &w;
            if (w.actualWindowFormat.swapInterval() == 0)
                ++unthrottledWindows;
            if (w.badVSync)
                ++badVSync;
        }
    }

    // Best case: with 1 exposed windows we can advance regular animations in
    // polishAndSync() and rely on being throttled to vsync. (no normal system
    // timer needed)
    //
    // Special case: with no windows exposed (e.g. on Windows: all of them are
    // minimized) run a normal system timer to make non-visual animation
    // functional still.
    //
    // Not so ideal case: with more than one window exposed we have to use the
    // same path as the no-windows case since polishAndSync() is now called
    // potentially for multiple windows over time so it cannot take care of
    // advancing the animation driver anymore.
    //
    // On top, another case: a window with vsync disabled should disable all the
    // good stuff and go with the system timer.
    //
    // Similarly, if there is at least one window where we determined that
    // vsync based blocking is not working as expected, that should make us
    // choose the timer based way.

    const bool canUseVSyncBasedAnimation = exposedWindows == 1 && unthrottledWindows == 0 && badVSync == 0;

    if (m_animation_timer != 0 && (canUseVSyncBasedAnimation || !m_animation_driver->isRunning())) {
        qCDebug(QSG_LOG_RENDERLOOP, "*** Stopping system (not vsync-based) animation timer (exposedWindows=%d unthrottledWindows=%d badVSync=%d)",
                exposedWindows, unthrottledWindows, badVSync);
        killTimer(m_animation_timer);
        m_animation_timer = 0;
        // If animations are running, make sure we keep on animating
        if (m_animation_driver->isRunning())
            postUpdateRequest(const_cast<Window *>(theOne));
    } else if (m_animation_timer == 0 && !canUseVSyncBasedAnimation && m_animation_driver->isRunning()) {
        qCDebug(QSG_LOG_RENDERLOOP, "*** Starting system (not vsync-based) animation timer (exposedWindows=%d unthrottledWindows=%d badVSync=%d)",
                exposedWindows, unthrottledWindows, badVSync);
        m_animation_timer = startTimer(int(sg->vsyncIntervalForAnimationDriver(m_animation_driver)));
    }
}

/*
    Removes this window from the list of tracked windowes in this
    window manager. hide() will trigger obscure, which in turn will
    stop rendering.

    This function will be called during QWindow::close() which will
    also destroy the QPlatformWindow so it is important that this
    triggers handleObscurity() and that rendering for that window
    is fully done and over with by the time this function exits.
 */

void QSGThreadedRenderLoop::hide(QQuickWindow *window)
{
    qCDebug(QSG_LOG_RENDERLOOP) << "hide()" << window;

    if (window->isExposed())
        handleObscurity(windowFor(window));

    releaseResources(window);
}

void QSGThreadedRenderLoop::resize(QQuickWindow *window)
{
    qCDebug(QSG_LOG_RENDERLOOP) << "resize()" << window;

    Window *w = windowFor(window);
    if (!w)
        return;

    w->psTimeAccumulator = 0.0f;
    w->psTimeSampleCount = 0;
}

/*
    If the window is first hide it, then perform a complete cleanup
    with releaseResources which will take down the GL context and
    exit the rendering thread.
 */
void QSGThreadedRenderLoop::windowDestroyed(QQuickWindow *window)
{
    qCDebug(QSG_LOG_RENDERLOOP) << "begin windowDestroyed()" << window;

    Window *w = windowFor(window);
    if (!w)
        return;

    handleObscurity(w);
    releaseResources(w, true);

    QSGRenderThread *thread = w->thread;
    while (thread->isRunning())
        QThread::yieldCurrentThread();
    Q_ASSERT(thread->thread() == QThread::currentThread());
    delete thread;

    for (int i=0; i<m_windows.size(); ++i) {
        if (m_windows.at(i).window == window) {
            m_windows.removeAt(i);
            break;
        }
    }

    // Now that we altered the window list, we may need to stop the animation
    // timer even if we didn't via handleObscurity. This covers the case where
    // we destroy a visible & exposed QQuickWindow.
    startOrStopAnimationTimer();

    qCDebug(QSG_LOG_RENDERLOOP) << "done windowDestroyed()" << window;
}

void QSGThreadedRenderLoop::releaseSwapchain(QQuickWindow *window)
{
    QQuickWindowPrivate *wd = QQuickWindowPrivate::get(window);
    delete wd->rpDescForSwapchain;
    wd->rpDescForSwapchain = nullptr;
    delete wd->swapchain;
    wd->swapchain = nullptr;
    delete wd->depthStencilForSwapchain;
    wd->depthStencilForSwapchain = nullptr;
    wd->hasActiveSwapchain = wd->hasRenderableSwapchain = wd->swapchainJustBecameRenderable = false;
}

void QSGThreadedRenderLoop::exposureChanged(QQuickWindow *window)
{
    qCDebug(QSG_LOG_RENDERLOOP) << "exposureChanged()" << window;

    // This is tricker than used to be. We want to detect having an empty
    // surface size (which may be the case even when window->size() is
    // non-empty, on some platforms with some graphics APIs!) as well as the
    // case when the window just became "newly exposed" (e.g. after a
    // minimize-restore on Windows, or when switching between fully obscured -
    // not fully obscured on macOS)
    QQuickWindowPrivate *wd = QQuickWindowPrivate::get(window);
    if (!window->isExposed())
        wd->hasRenderableSwapchain = false;

    bool skipThisExpose = false;
    if (window->isExposed() && wd->hasActiveSwapchain && wd->swapchain->surfacePixelSize().isEmpty()) {
        wd->hasRenderableSwapchain = false;
        skipThisExpose = true;
    }

    if (window->isExposed() && !wd->hasRenderableSwapchain && wd->hasActiveSwapchain
            && !wd->swapchain->surfacePixelSize().isEmpty())
    {
        wd->hasRenderableSwapchain = true;
        wd->swapchainJustBecameRenderable = true;
    }

    if (window->isExposed()) {
        if (!skipThisExpose)
            handleExposure(window);
    } else {
        Window *w = windowFor(window);
        if (w)
            handleObscurity(w);
    }
}

/*
    Will post an event to the render thread that this window should
    start to render.
 */
void QSGThreadedRenderLoop::handleExposure(QQuickWindow *window)
{
    auto it = std::ranges::find_if(m_windows, [window](const Window &w) { return w.window == window; });
    Window *w = nullptr;

    if (it != m_windows.end()) [[likely]] {
        w = &(*it);
        if (!QQuickWindowPrivate::get(window)->updatesEnabled) [[unlikely]] return;
    } else {
        auto *wd = QQuickWindowPrivate::get(window);
        auto *renderContext = wd->context;
        pendingRenderContexts.remove(renderContext);
        
        m_windows.emplace_back();
        w = &m_windows.back();
        w->window = window;
        w->actualWindowFormat = window->format();
        w->thread = new QSGRenderThread(this, renderContext);
        w->updateDuringSync = false;
        w->forceRenderPass = true;
        w->badVSync = false;
        w->psTimeAccumulator = 0.0f;
        w->psTimeSampleCount = 0;
        w->timeBetweenPolishAndSyncs.start();
    }

    if (!w->window->handle()) [[unlikely]] window->create();

    if (!w->thread->isRunning()) {
        w->thread->window = window;
        if (!w->thread->rhi) {
            auto *rhiSupport = QSGRhiSupport::instance();
            if (!w->thread->offscreenSurface) [[unlikely]]
                w->thread->offscreenSurface = rhiSupport->maybeCreateOffscreenSurface(window);
            w->thread->scProxyData = QRhi::updateSwapChainProxyData(rhiSupport->rhiBackend(), window);
            window->installEventFilter(this);
        }

        if (auto *controller = QQuickWindowPrivate::get(w->window)->animationController.get(); 
            controller->thread() != w->thread) [[unlikely]]
            controller->moveToThread(w->thread);

        w->thread->active = true;
        if (w->thread->thread() == QThread::currentThread()) [[unlikely]] {
            w->thread->sgrc->moveToThread(w->thread);
            w->thread->moveToThread(w->thread);
        }
        w->thread->start();
    } else {
        QMutexLocker lock(&w->thread->mutex);
        w->thread->postEvent(new WMWindowEvent(w->window, QEvent::Type(WM_Exposed)));
        w->thread->waitCondition.wakeOne();
    }

    polishAndSync(w, true);
    startOrStopAnimationTimer();
}

/*
    This function posts an event to the render thread to remove the window
    from the list of windowses to render.

    It also starts up the non-vsync animation tick if no more windows
    are showing.
 */
void QSGThreadedRenderLoop::handleObscurity(Window *w)
{
    if (!w)
        return;

    qCDebug(QSG_LOG_RENDERLOOP) << "handleObscurity()" << w->window;
    if (w->thread->isRunning()) {
        if (!QQuickWindowPrivate::get(w->window)->updatesEnabled) {
            qCDebug(QSG_LOG_RENDERLOOP, "- updatesEnabled is false, abort");
            return;
        }
        w->thread->mutex.lock();
        w->thread->postEvent(new WMWindowEvent(w->window, QEvent::Type(WM_Obscure)));
        w->thread->waitCondition.wait(&w->thread->mutex);
        w->thread->mutex.unlock();
    }
    startOrStopAnimationTimer();
}

bool QSGThreadedRenderLoop::eventFilter(QObject *watched, QEvent *event)
{
    switch (event->type()) {
    case QEvent::PlatformSurface:
        // this is the proper time to tear down the swapchain (while the native window and surface are still around)
        if (static_cast<QPlatformSurfaceEvent *>(event)->surfaceEventType() == QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed) {
            QQuickWindow *window = qobject_cast<QQuickWindow *>(watched);
            if (window) {
                Window *w = windowFor(window);
                if (w && w->thread->isRunning()) {
                    w->thread->mutex.lock();
                    w->thread->postEvent(new WMReleaseSwapchainEvent(window));
                    w->thread->waitCondition.wait(&w->thread->mutex);
                    w->thread->mutex.unlock();
                }
            }
            // keep this filter on the window - needed for uncommon but valid
            // sequences of calls like window->destroy(); window->show();
        }
        break;
    default:
        break;
    }
    return QObject::eventFilter(watched, event);
}

void QSGThreadedRenderLoop::handleUpdateRequest(QQuickWindow *window)
{
    qCDebug(QSG_LOG_RENDERLOOP) <<  "- update request" << window;
    if (!QQuickWindowPrivate::get(window)->updatesEnabled) {
        qCDebug(QSG_LOG_RENDERLOOP, "- updatesEnabled is false, abort");
        return;
    }
    Window *w = windowFor(window);
    if (w)
        polishAndSync(w);
}

void QSGThreadedRenderLoop::maybeUpdate(QQuickWindow *window)
{
    Window *w = windowFor(window);
    if (w)
        maybeUpdate(w);
}

/*
    Called whenever the QML scene has changed. Will post an event to
    ourselves that a sync is needed.
 */
void QSGThreadedRenderLoop::maybeUpdate(Window *w)
{
    if (!QCoreApplication::instance())
        return;

    if (!w || !w->thread->isRunning())
        return;

    QThread *current = QThread::currentThread();
    if (current == w->thread && w->thread->rhi && w->thread->rhi->isDeviceLost())
        return;
    if (current != QCoreApplication::instance()->thread() && (current != w->thread || !m_lockedForSync)) {
        qWarning() << "Updates can only be scheduled from GUI thread or from QQuickItem::updatePaintNode()";
        return;
    }

    qCDebug(QSG_LOG_RENDERLOOP) << "update from item" << w->window;

    // Call this function from the Gui thread later as startTimer cannot be
    // called from the render thread.
    if (current == w->thread) {
        qCDebug(QSG_LOG_RENDERLOOP, "- on render thread");
        w->updateDuringSync = true;
        return;
    }

    // An updatePolish() implementation may call update() to get the QQuickItem
    // dirtied. That's fine but it also leads to calling this function.
    // Requesting another update is a waste then since the updatePolish() call
    // will be followed up with a round of sync and render.
    if (m_inPolish)
        return;

    postUpdateRequest(w);
}

/*
    Called when the QQuickWindow should be explicitly repainted. This function
    can also be called on the render thread when the GUI thread is blocked to
    keep render thread animations alive.
 */
void QSGThreadedRenderLoop::update(QQuickWindow *window)
{
    Window *w = windowFor(window);
    if (!w)
        return;

    const bool isRenderThread = QThread::currentThread() == w->thread;

    if (QPlatformWindow *platformWindow = window->handle()) {
        // If the window is being resized we don't want to schedule unthrottled
        // updates on the render thread, as this will starve the main thread
        // from getting drawables for displaying the updated window size.
        if (isRenderThread && !platformWindow->allowsIndependentThreadedRendering()) {
            // In most cases the window will already have update requested
            // due to the animator triggering a sync, but just in case we
            // schedule an update request on the main thread explicitly.
            qCDebug(QSG_LOG_RENDERLOOP) << "window is resizing. update on window" << w->window;
            QTimer::singleShot(0, window, [=]{ window->requestUpdate(); });
            return;
        }
    }

    if (isRenderThread) {
       qCDebug(QSG_LOG_RENDERLOOP) << "update on window - on render thread" << w->window;
       w->thread->requestRepaint();
       return;
    }

    qCDebug(QSG_LOG_RENDERLOOP) << "update on window" << w->window;
    // We set forceRenderPass because we want to make sure the QQuickWindow
    // actually does a full render pass after the next sync.
    w->forceRenderPass = true;
    maybeUpdate(w);
}


void QSGThreadedRenderLoop::releaseResources(QQuickWindow *window)
{
    Window *w = windowFor(window);
    if (w)
        releaseResources(w, false);
}

/*
 * Release resources will post an event to the render thread to
 * free up the SG and GL resources and exists the render thread.
 */
void QSGThreadedRenderLoop::releaseResources(Window *w, bool inDestructor)
{
    qCDebug(QSG_LOG_RENDERLOOP) << "releaseResources()" << (inDestructor ? "in destructor" : "in api-call") << w->window;

    w->thread->mutex.lock();
    if (w->thread->isRunning() && w->thread->active) {
        QQuickWindow *window = w->window;

        // The platform window might have been destroyed before
        // hide/release/windowDestroyed is called, so we may need to have a
        // fallback surface to perform the cleanup of the scene graph and the
        // RHI resources.

        qCDebug(QSG_LOG_RENDERLOOP, "- posting release request to render thread");
        w->thread->postEvent(new WMTryReleaseEvent(window, inDestructor, window->handle() == nullptr));
        w->thread->waitCondition.wait(&w->thread->mutex);

        // Avoid a shutdown race condition.
        // If SG is invalidated and 'active' becomes false, the thread's run()
        // method will exit. handleExposure() relies on QThread::isRunning() (because it
        // potentially needs to start the thread again) and our mutex cannot be used to
        // track the thread stopping, so we wait a few nanoseconds extra so the thread
        // can exit properly.
        if (!w->thread->active) {
            qCDebug(QSG_LOG_RENDERLOOP) << " - waiting for render thread to exit" << w->window;
            w->thread->wait();
            qCDebug(QSG_LOG_RENDERLOOP) << " - render thread finished" << w->window;
        }
    }
    w->thread->mutex.unlock();
}


/* Calls polish on all items, then requests synchronization with the render thread
 * and blocks until that is complete. Returns false if it aborted; otherwise true.
 */
void QSGThreadedRenderLoop::polishAndSync(Window *w, bool inExpose)
{
    qCDebug(QSG_LOG_RENDERLOOP) << "polishAndSync" << (inExpose ? "(in expose)" : "(normal)") << w->window;

    QQuickWindow *window = w->window;
    if (!w->thread || !w->thread->window) {
        qCDebug(QSG_LOG_RENDERLOOP, "- not exposed, abort");
        return;
    }

    // Flush pending touch events.
    QQuickWindowPrivate::get(window)->deliveryAgentPrivate()->flushFrameSynchronousEvents(window);
    // The delivery of the event might have caused the window to stop rendering
    w = windowFor(window);
    if (!w || !w->thread || !w->thread->window) {
        qCDebug(QSG_LOG_RENDERLOOP, "- removed after event flushing, abort");
        return;
    }

    Q_TRACE_SCOPE(QSG_polishAndSync);
    QElapsedTimer timer;
    qint64 polishTime = 0;
    qint64 waitTime = 0;
    qint64 syncTime = 0;

    const qint64 elapsedSinceLastMs = w->timeBetweenPolishAndSyncs.restart();

    if (w->actualWindowFormat.swapInterval() != 0 && sg->isVSyncDependent(m_animation_driver)) {
        w->psTimeAccumulator += elapsedSinceLastMs;
        w->psTimeSampleCount += 1;
        // cannot be too high because we'd then delay recognition of broken vsync at start
        static const int PS_TIME_SAMPLE_LENGTH = 20;
        if (w->psTimeSampleCount > PS_TIME_SAMPLE_LENGTH) {
            const float t = w->psTimeAccumulator / w->psTimeSampleCount;
            const float vsyncRate = sg->vsyncIntervalForAnimationDriver(m_animation_driver);

            // What this means is that the last PS_TIME_SAMPLE_LENGTH frames
            // average to an elapsed time of t milliseconds, whereas the animation
            // driver (assuming a single window, vsync-based advancing) assumes a
            // vsyncRate milliseconds for a frame. If now we see that the elapsed
            // time is way too low (less than half of the approx. expected value),
            // then we assume that something is wrong with vsync.
            //
            // This will not capture everything. Consider a 144 Hz screen with 6.9
            // ms vsync rate, the half of that is below the default 5 ms timer of
            // QWindow::requestUpdate(), so this will not trigger even if the
            // graphics stack does not throttle. But then the whole workaround is
            // not that important because the animations advance anyway closer to
            // what's expected (e.g. advancing as if 6-7 ms passed after ca. 5 ms),
            // the gap is a lot smaller than with the 60 Hz case (animations
            // advancing as if 16 ms passed after just ca. 5 ms) The workaround
            // here is present mainly for virtual machines and other broken
            // environments, most of which will persumably report a 60 Hz screen.

            const float threshold = vsyncRate * 0.5f;
            const bool badVSync = t < threshold;
            if (badVSync && !w->badVSync) {
                // Once we determine something is wrong with the frame rate, set
                // the flag for the rest of the lifetime of the window. This is
                // saner and more deterministic than allowing it to be turned on
                // and off. (a window resize can take up time, leading to higher
                // elapsed times, thus unnecessarily starting to switch modes,
                // while some platforms seem to have advanced logic (and adaptive
                // refresh rates an whatnot) that can eventually start throttling
                // an unthrottled window, potentially leading to a continuous
                // switching of modes back and forth which is not desirable.
                w->badVSync = true;
                qCDebug(QSG_LOG_INFO, "Window %p is determined to have broken vsync throttling (%f < %f) "
                                      "switching to system timer to drive gui thread animations to remedy this "
                                      "(however, render thread animators will likely advance at an incorrect rate).",
                        w->window, t, threshold);
                startOrStopAnimationTimer();
            }

            w->psTimeAccumulator = 0.0f;
            w->psTimeSampleCount = 0;
        }
    }

    const bool profileFrames = QSG_LOG_TIME_RENDERLOOP().isDebugEnabled();
    if (profileFrames) {
        timer.start();
        qCDebug(QSG_LOG_TIME_RENDERLOOP, "[window %p][gui thread] polishAndSync: start, elapsed since last call: %d ms",
                window,
                int(elapsedSinceLastMs));
    }
    Q_QUICK_SG_PROFILE_START(QQuickProfiler::SceneGraphPolishAndSync);
    Q_TRACE(QSG_polishItems_entry);

    QQuickWindowPrivate *d = QQuickWindowPrivate::get(window);
    m_inPolish = true;
    d->polishItems();
    m_inPolish = false;

    if (profileFrames)
        polishTime = timer.nsecsElapsed();
    Q_TRACE(QSG_polishItems_exit);
    Q_QUICK_SG_PROFILE_RECORD(QQuickProfiler::SceneGraphPolishAndSync,
                              QQuickProfiler::SceneGraphPolishAndSyncPolish);

    w = windowFor(window);
    if (!w || !w->thread || !w->thread->window) {
        qCDebug(QSG_LOG_RENDERLOOP, "- removed after polishing, abort");
        return;
    }

    Q_TRACE(QSG_wait_entry);
    w->updateDuringSync = false;

    emit window->afterAnimating();

    const QRhiSwapChainProxyData scProxyData =
            QRhi::updateSwapChainProxyData(QSGRhiSupport::instance()->rhiBackend(), window);

    qCDebug(QSG_LOG_RENDERLOOP, "- lock for sync");
    w->thread->mutex.lock();
    m_lockedForSync = true;
    w->thread->postEvent(new WMSyncEvent(window, inExpose, w->forceRenderPass, scProxyData));
    w->forceRenderPass = false;

    qCDebug(QSG_LOG_RENDERLOOP, "- wait for sync");
    if (profileFrames)
        waitTime = timer.nsecsElapsed();
    Q_TRACE(QSG_wait_exit);
    Q_QUICK_SG_PROFILE_RECORD(QQuickProfiler::SceneGraphPolishAndSync,
                              QQuickProfiler::SceneGraphPolishAndSyncWait);
    Q_TRACE(QSG_sync_entry);

    w->thread->waitCondition.wait(&w->thread->mutex);
    m_lockedForSync = false;
    w->thread->mutex.unlock();
    qCDebug(QSG_LOG_RENDERLOOP, "- unlock after sync");

    if (profileFrames)
        syncTime = timer.nsecsElapsed();
    Q_TRACE(QSG_sync_exit);
    Q_QUICK_SG_PROFILE_RECORD(QQuickProfiler::SceneGraphPolishAndSync,
                              QQuickProfiler::SceneGraphPolishAndSyncSync);
    Q_TRACE(QSG_animations_entry);

    // Now is the time to advance the regular animations (as we are throttled
    // to vsync due to the wait above), but this is only relevant when there is
    // one single window. With multiple windows m_animation_timer is active,
    // and advance() happens instead in response to a good old timer event, not
    // here. (the above applies only when the QSGAnimationDriver reports
    // isVSyncDependent() == true, if not then we always use the driver and
    // just advance here)
    if (m_animation_timer == 0 && m_animation_driver->isRunning()) {
        auto advanceAnimations = [this, window=QPointer(window)] {
            qCDebug(QSG_LOG_RENDERLOOP, "- advancing animations");
            m_animation_driver->advance();
            qCDebug(QSG_LOG_RENDERLOOP, "- animations done..");

            // We need to trigger another update round to keep all animations
            // running correctly. For animations that lead to a visual change (a
            // property change in some item leading to dirtying the item and so
            // ending up in maybeUpdate()) this would not be needed, but other
            // animations would then stop functioning since there is nothing
            // advancing the animation system if we do not call postUpdateRequest()
            // here and nothing else leads to it either. This has an unfortunate
            // side effect in multi window cases: one can end up in a situation
            // where a non-animating window gets updates continuously because there
            // is an animation running in some other window that is non-exposed or
            // even closed already (if it was exposed we would not hit this branch,
            // however). Sadly, there is nothing that can be done about it.
            if (window)
                window->requestUpdate();

            emit timeToIncubate();
        };

#if defined(Q_OS_APPLE)
        if (inExpose) {
            // If we are handling an expose event the system is expecting us to
            // produce a frame that it can present on screen to the user. Advancing
            // animations at this point might result in changing properties of the
            // window in a way that invalidates the current frame, resulting in the
            // discarding of the current frame before the user ever sees it. To give
            // the system a chance to present the current frame we defer the advance
            // of the animations until the start of the next event loop pass, which
            // should still give plenty of time to compute the new render state before
            // the next expose event or update request.
            QMetaObject::invokeMethod(this, advanceAnimations, Qt::QueuedConnection);
        } else
#endif // Q_OS_APPLE
        {
            // For regular update requests we assume we can advance here synchronously
            advanceAnimations();
        }
    } else if (w->updateDuringSync) {
        postUpdateRequest(w);
    }

    if (profileFrames) {
        qCDebug(QSG_LOG_TIME_RENDERLOOP, "[window %p][gui thread] Frame prepared, polish=%d ms, lock=%d ms, blockedForSync=%d ms, animations=%d ms",
                window,
                int(polishTime / 1000000),
                int((waitTime - polishTime) / 1000000),
                int((syncTime - waitTime) / 1000000),
                int((timer.nsecsElapsed() - syncTime) / 1000000));
    }

    Q_TRACE(QSG_animations_exit);
    Q_QUICK_SG_PROFILE_END(QQuickProfiler::SceneGraphPolishAndSync,
                           QQuickProfiler::SceneGraphPolishAndSyncAnimations);
}

bool QSGThreadedRenderLoop::event(QEvent *e)
{
    switch ((int) e->type()) {

    case QEvent::Timer: {
        Q_ASSERT(sg->isVSyncDependent(m_animation_driver));
        QTimerEvent *te = static_cast<QTimerEvent *>(e);
        if (te->timerId() == m_animation_timer) {
            qCDebug(QSG_LOG_RENDERLOOP, "- ticking non-render thread timer");
            m_animation_driver->advance();
            emit timeToIncubate();
            return true;
        }
        break;
    }

    default:
        break;
    }

    return QObject::event(e);
}



/*
    Locks down GUI and performs a grab the scene graph, then returns the result.

    Since the QML scene could have changed since the last time it was rendered,
    we need to polish and sync the scene graph. This might seem superfluous, but
     - QML changes could have triggered deleteLater() which could have removed
       textures or other objects from the scene graph, causing render to crash.
     - Autotests rely on grab(), setProperty(), grab(), compare behavior.
 */

QImage QSGThreadedRenderLoop::grab(QQuickWindow *window)
{
    qCDebug(QSG_LOG_RENDERLOOP) << "grab()" << window;

    Window *w = windowFor(window);
    Q_ASSERT(w);

    if (!w->thread->isRunning())
        return QImage();

    if (!window->handle())
        window->create();

    qCDebug(QSG_LOG_RENDERLOOP, "- polishing items");
    QQuickWindowPrivate *d = QQuickWindowPrivate::get(window);
    m_inPolish = true;
    d->polishItems();
    m_inPolish = false;

    QImage result;
    w->thread->mutex.lock();
    m_lockedForSync = true;
    qCDebug(QSG_LOG_RENDERLOOP, "- posting grab event");
    w->thread->postEvent(new WMGrabEvent(window, &result));
    w->thread->waitCondition.wait(&w->thread->mutex);
    m_lockedForSync = false;
    w->thread->mutex.unlock();

    qCDebug(QSG_LOG_RENDERLOOP, "- grab complete");

    return result;
}

/*
 * Posts a new job event to the render thread.
 * Returns true if posting succeeded.
 */
void QSGThreadedRenderLoop::postJob(QQuickWindow *window, QRunnable *job)
{
    Window *w = windowFor(window);
    if (w && w->thread && w->thread->window)
        w->thread->postEvent(new WMJobEvent(window, job));
    else
        delete job;
}

QT_END_NAMESPACE

#include "qsgthreadedrenderloop.moc"
#include "moc_qsgthreadedrenderloop_p.cpp"
