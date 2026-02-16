// Copyright (C) 2016 The Qt Company Ltd.
// Copyright (C) 2016 Jolla Ltd, author: <gunnar.sletta@jollamobile.com>
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#include <QtCore/QMutex>
#include <QtCore/QWaitCondition>
#include <QtCore/QAnimationDriver>
#include <QtCore/QQueue>
#include <QtCore/QTimer>
#include <QtCore/QElapsedTimer>

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

#include <ranges>
#include <algorithm>

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

// OPTIMIZED: sync(bool inExpose) from optimized_syncAndRender.cpp
void QSGRenderThread::sync(bool inExpose)
{
    QMutexLocker lock(&mutex);

    bool canSync = (rhi && windowSize.width() > 0 && windowSize.height() > 0);
    
    if (canSync) [[likely]] {
        rhi->makeThreadLocalNativeContextCurrent();
        
        auto *d = QQuickWindowPrivate::get(window);
        if (d->renderer) [[likely]]
            d->renderer->clearChangedFlag();
        
        d->syncSceneGraph();
        sgrc->endSync();

        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }

    if (!inExpose) [[likely]] {
        waitCondition.wakeOne();
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

// OPTIMIZED: syncAndRender() from optimized_syncAndRender.cpp
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

    if (profileFrames) [[unlikely]] {
        m_threadTimeBetweenRenders.restart();
    }

    auto *d = QQuickWindowPrivate::get(window);
    const bool syncRequested = (pendingUpdate & SyncRequest);
    const bool exposeRequested = (pendingUpdate & ExposeRequest) == ExposeRequest;
    pendingUpdate = 0;

    auto *cd = d;
    const bool hasValidSwapChain = (cd->swapchain && windowSize.width() > 0 && windowSize.height() > 0);
    
    if (hasValidSwapChain) [[likely]] {
        cd->swapchain->setProxyData(scProxyData);
        const QSize effectiveOutputSize = cd->swapchain->surfacePixelSize();
        
        if (effectiveOutputSize.isEmpty()) [[unlikely]] {
            if (syncRequested) [[likely]] {
                QMutexLocker lock(&mutex);
                waitCondition.wakeOne();
            }
            return;
        }

        const QSize previousOutputSize = cd->swapchain->currentPixelSize();
        if (previousOutputSize != effectiveOutputSize || cd->swapchainJustBecameRenderable) [[unlikely]] {
            cd->hasActiveSwapchain = cd->swapchain->createOrResize();
            
            if (!cd->hasActiveSwapchain) [[unlikely]] {
                if (rhi->isDeviceLost()) {
                    handleDeviceLoss();
                    QCoreApplication::postEvent(window, new QEvent(QEvent::Type(QQuickWindowPrivate::FullUpdateRequest)));
                    if (syncRequested) {
                        QMutexLocker lock(&mutex);
                        waitCondition.wakeOne();
                    }
                    return;
                } else if (previousOutputSize.isEmpty() && !swRastFallbackDueToSwapchainFailure && 
                          QSGRhiSupport::instance()->attemptReinitWithSwRastUponFail()) {
                    swRastFallbackDueToSwapchainFailure = true;
                    teardownGraphics();
                    QCoreApplication::postEvent(window, new QEvent(QEvent::Type(QQuickWindowPrivate::FullUpdateRequest)));
                    if (syncRequested) {
                        QMutexLocker lock(&mutex);
                        waitCondition.wakeOne();
                    }
                    return;
                }
            }

            cd->swapchainJustBecameRenderable = false;
            cd->hasRenderableSwapchain = cd->hasActiveSwapchain;
        }

        emit window->beforeFrameBegin();

        QRhi::FrameOpResult frameResult = rhi->beginFrame(cd->swapchain);
        if (frameResult != QRhi::FrameOpSuccess) [[unlikely]] {
            if (frameResult == QRhi::FrameOpDeviceLost)
                handleDeviceLoss();
            if (frameResult == QRhi::FrameOpDeviceLost || frameResult == QRhi::FrameOpSwapChainOutOfDate)
                QCoreApplication::postEvent(window, new QEvent(QEvent::Type(QQuickWindowPrivate::FullUpdateRequest)));
            if (syncRequested) {
                QMutexLocker lock(&mutex);
                waitCondition.wakeOne();
            }
            emit window->afterFrameEnd();
            return;
        }
    }

    if (syncRequested) [[likely]] {
        sync(exposeRequested);
    }
    
#ifndef QSG_NO_RENDER_TIMING
    if (profileFrames) [[unlikely]]
        syncTime = threadTimer.nsecsElapsed();
#endif
    Q_TRACE(QSG_sync_exit);
    Q_QUICK_SG_PROFILE_RECORD(QQuickProfiler::SceneGraphRenderLoopFrame,
                              QQuickProfiler::SceneGraphRenderLoopSync);

    Q_TRACE(QSG_render_entry);

    if (animatorDriver->isRunning()) [[unlikely]] {
        d->animationController->lock();
        animatorDriver->advance();
        d->animationController->unlock();
    }

    const bool canRender = d->renderer && hasValidSwapChain;
    double lastCompletedGpuTime = 0;
    if (canRender) [[likely]] {
        if (!syncRequested) [[unlikely]]
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
        if (cd->swapchain && rhi->isRecordingFrame()) [[unlikely]]
            rhi->endFrame(cd->swapchain, QRhi::SkipPresent);
    }

   if (hasValidSwapChain) [[likely]]
        emit window->afterFrameEnd();

    if (exposeRequested) [[unlikely]] {
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

// OPTIMIZED: ensureRhi() from final_optimized_methods.cpp
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

// OPTIMIZED: run() from final_optimized_methods.cpp
void QSGRenderThread::run()
{
    animatorDriver = sgrc->sceneGraphContext()->createAnimationDriver(nullptr);
    animatorDriver->install();
    if (QQmlDebugConnector::service<QQmlProfilerService>()) [[unlikely]]
        QQuickProfiler::registerAnimationCallback();

    m_threadTimeBetweenRenders.start();

    for (int frames = 0; active; ) [[likely]] {
#ifdef Q_OS_DARWIN
        QMacAutoReleasePool frameReleasePool;
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

QSGThreadedRenderLoop::QSGThreadedRenderLoop()
    : sg(QSGContext::createDefaultContext())
    , m_animation_timer(0)
{
    m_animation_driver = sg->createAnimationDriver(this);

    connect(m_animation_driver, SIGNAL(started()), this, SLOT(animationStarted()));
    connect(m_animation_driver, SIGNAL(stopped()), this, SLOT(animationStopped()));

    m_animation_driver->install();
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

// OPTIMIZED: handleExposure(QQuickWindow *window) from final_optimized_methods.cpp
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
        }
        break;
    default:
        break;
    }
    return QObject::eventFilter(watched, event);
}

void QSGThreadedRenderLoop::handleUpdateRequest(QQuickWindow *window)
{
    if (!QQuickWindowPrivate::get(window)->updatesEnabled) return;
    Window *w = windowFor(window);
    if (w) polishAndSync(w);
}

void QSGThreadedRenderLoop::maybeUpdate(QQuickWindow *window)
{
    Window *w = windowFor(window);
    if (w) maybeUpdate(w);
}

void QSGThreadedRenderLoop::maybeUpdate(Window *w)
{
    if (!QCoreApplication::instance()) return;
    if (!w || !w->thread->isRunning()) return;

    QThread *current = QThread::currentThread();
    if (current == w->thread && w->thread->rhi && w->thread->rhi->isDeviceLost()) return;
    if (current != QCoreApplication::instance()->thread() && (current != w->thread || !m_lockedForSync)) {
        qWarning() << "Updates can only be scheduled from GUI thread or from QQuickItem::updatePaintNode()";
        return;
    }

    if (current == w->thread) {
        w->updateDuringSync = true;
        return;
    }

    if (m_inPolish) return;
    postUpdateRequest(w);
}

void QSGThreadedRenderLoop::update(QQuickWindow *window)
{
    Window *w = windowFor(window);
    if (!w) return;

    const bool isRenderThread = QThread::currentThread() == w->thread;
    if (QPlatformWindow *platformWindow = window->handle()) {
        if (isRenderThread && !platformWindow->allowsIndependentThreadedRendering()) {
            QTimer::singleShot(0, window, [=]{ window->requestUpdate(); });
            return;
        }
    }

    if (isRenderThread) {
       w->thread->requestRepaint();
       return;
    }

    w->forceRenderPass = true;
    maybeUpdate(w);
}


void QSGThreadedRenderLoop::releaseResources(QQuickWindow *window)
{
    Window *w = windowFor(window);
    if (w) releaseResources(w, false);
}

void QSGThreadedRenderLoop::releaseResources(Window *w, bool inDestructor)
{
    w->thread->mutex.lock();
    if (w->thread->isRunning() && w->thread->active) {
        QQuickWindow *window = w->window;
        w->thread->postEvent(new WMTryReleaseEvent(window, inDestructor, window->handle() == nullptr));
        w->thread->waitCondition.wait(&w->thread->mutex);

        if (!w->thread->active) {
            w->thread->wait();
        }
    }
    w->thread->mutex.unlock();
}


void QSGThreadedRenderLoop::polishAndSync(Window *w, bool inExpose)
{
    QQuickWindow *window = w->window;
    if (!w->thread || !w->thread->window) return;

    QQuickWindowPrivate::get(window)->deliveryAgentPrivate()->flushFrameSynchronousEvents(window);
    w = windowFor(window);
    if (!w || !w->thread || !w->thread->window) return;

    Q_TRACE_SCOPE(QSG_polishAndSync);
    QElapsedTimer timer;
    qint64 polishTime = 0, waitTime = 0, syncTime = 0;

    const qint64 elapsedSinceLastMs = w->timeBetweenPolishAndSyncs.restart();

    if (w->actualWindowFormat.swapInterval() != 0 && sg->isVSyncDependent(m_animation_driver)) {
        w->psTimeAccumulator += elapsedSinceLastMs;
        w->psTimeSampleCount += 1;
        static const int PS_TIME_SAMPLE_LENGTH = 20;
        if (w->psTimeSampleCount > PS_TIME_SAMPLE_LENGTH) {
            const float t = w->psTimeAccumulator / w->psTimeSampleCount;
            const float vsyncRate = sg->vsyncIntervalForAnimationDriver(m_animation_driver);
            const float threshold = vsyncRate * 0.5f;
            const bool badVSync = t < threshold;
            if (badVSync && !w->badVSync) {
                w->badVSync = true;
                startOrStopAnimationTimer();
            }
            w->psTimeAccumulator = 0.0f;
            w->psTimeSampleCount = 0;
        }
    }

    const bool profileFrames = QSG_LOG_TIME_RENDERLOOP().isDebugEnabled();
    if (profileFrames) timer.start();

    Q_QUICK_SG_PROFILE_START(QQuickProfiler::SceneGraphPolishAndSync);
    Q_TRACE(QSG_polishItems_entry);

    QQuickWindowPrivate *d = QQuickWindowPrivate::get(window);
    m_inPolish = true;
    d->polishItems();
    m_inPolish = false;

    if (profileFrames) polishTime = timer.nsecsElapsed();
    Q_TRACE(QSG_polishItems_exit);
    Q_QUICK_SG_PROFILE_RECORD(QQuickProfiler::SceneGraphPolishAndSync, QQuickProfiler::SceneGraphPolishAndSyncPolish);

    w = windowFor(window);
    if (!w || !w->thread || !w->thread->window) return;

    Q_TRACE(QSG_wait_entry);
    w->updateDuringSync = false;
    emit window->afterAnimating();

    const QRhiSwapChainProxyData scProxyData = QRhi::updateSwapChainProxyData(QSGRhiSupport::instance()->rhiBackend(), window);

    w->thread->mutex.lock();
    m_lockedForSync = true;
    w->thread->postEvent(new WMSyncEvent(window, inExpose, w->forceRenderPass, scProxyData));
    w->forceRenderPass = false;

    if (profileFrames) waitTime = timer.nsecsElapsed();
    Q_TRACE(QSG_wait_exit);
    Q_QUICK_SG_PROFILE_RECORD(QQuickProfiler::SceneGraphPolishAndSync, QQuickProfiler::SceneGraphPolishAndSyncWait);
    Q_TRACE(QSG_sync_entry);

    w->thread->waitCondition.wait(&w->thread->mutex);
    m_lockedForSync = false;
    w->thread->mutex.unlock();

    if (profileFrames) syncTime = timer.nsecsElapsed();
    Q_TRACE(QSG_sync_exit);
    Q_QUICK_SG_PROFILE_RECORD(QQuickProfiler::SceneGraphPolishAndSync, QQuickProfiler::SceneGraphPolishAndSyncSync);
    Q_TRACE(QSG_animations_entry);

    if (m_animation_timer == 0 && m_animation_driver->isRunning()) {
        auto advanceAnimations = [this, window=QPointer(window)] {
            m_animation_driver->advance();
            if (window) window->requestUpdate();
            emit timeToIncubate();
        };

#if defined(Q_OS_APPLE)
        if (inExpose) {
            QMetaObject::invokeMethod(this, advanceAnimations, Qt::QueuedConnection);
        } else
#endif
        {
            advanceAnimations();
        }
    } else if (w->updateDuringSync) {
        postUpdateRequest(w);
    }

    if (profileFrames) {
        qCDebug(QSG_LOG_TIME_RENDERLOOP, "[window %p][gui thread] Frame prepared, polish=%d ms, lock=%d ms, sync=%d ms, anims=%d ms",
                window, int(polishTime / 1000000), int((waitTime - polishTime) / 1000000),
                int((syncTime - waitTime) / 1000000), int((timer.nsecsElapsed() - syncTime) / 1000000));
    }

    Q_TRACE(QSG_animations_exit);
    Q_QUICK_SG_PROFILE_END(QQuickProfiler::SceneGraphPolishAndSync, QQuickProfiler::SceneGraphPolishAndSyncAnimations);
}

bool QSGThreadedRenderLoop::event(QEvent *e)
{
    if (e->type() == QEvent::Timer) {
        QTimerEvent *te = static_cast<QTimerEvent *>(e);
        if (te->timerId() == m_animation_timer) {
            m_animation_driver->advance();
            emit timeToIncubate();
            return true;
        }
    }
    return QObject::event(e);
}

QImage QSGThreadedRenderLoop::grab(QQuickWindow *window)
{
    Window *w = windowFor(window);
    Q_ASSERT(w);
    if (!w->thread->isRunning()) return QImage();
    if (!window->handle()) window->create();

    QQuickWindowPrivate *d = QQuickWindowPrivate::get(window);
    m_inPolish = true;
    d->polishItems();
    m_inPolish = false;

    QImage result;
    w->thread->mutex.lock();
    m_lockedForSync = true;
    w->thread->postEvent(new WMGrabEvent(window, &result));
    w->thread->waitCondition.wait(&w->thread->mutex);
    m_lockedForSync = false;
    w->thread->mutex.unlock();

    return result;
}

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
