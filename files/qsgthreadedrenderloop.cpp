// Copyright (C) 2016 The Qt Company Ltd.
// Copyright (C) 2016 Jolla Ltd, author: <gunnar.sletta@jollamobile.com>
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only


#include <QtCore/QMutex>
#include <QtCore/QWaitCondition>
#include <QtCore/QAnimationDriver>
#include <QtCore/QTimer>
#include <QtCore/QStandardPaths>
#include <QtCore/QFile>
#include <QtCore/QDir>
#include <atomic>
#include <variant>
#include <vector>

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
#include <algorithm>

#ifdef Q_OS_DARWIN
#include <QtCore/private/qcore_mac_p.h>
#endif

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
    auto it = std::ranges::find(m_windows, window, &Window::window);
    return it != m_windows.end() ? &*it : nullptr;
}

class WMWindowEvent
{
public:
    WMWindowEvent(QQuickWindow *c) : window(c) { }
    QQuickWindow *window;
};

class WMObscureEvent : public WMWindowEvent
{
public:
    WMObscureEvent(QQuickWindow *c) : WMWindowEvent(c) {}
};

class WMExposedEvent : public WMWindowEvent
{
public:
    WMExposedEvent(QQuickWindow *c)
        : WMWindowEvent(c)
        , size(c->size())
        , dpr(float(c->effectiveDevicePixelRatio()))
    {}
    QSize size;
    float dpr;
};

class WMTryReleaseEvent : public WMWindowEvent
{
public:
    WMTryReleaseEvent(QQuickWindow *win, bool destroy, bool needsFallbackSurface)
        : WMWindowEvent(win)
        , inDestructor(destroy)
    { Q_UNUSED(needsFallbackSurface); }

    bool inDestructor;
};

class WMSyncEvent : public WMWindowEvent
{
public:
    WMSyncEvent(QQuickWindow *c, bool inExpose, bool force, QRhiSwapChainProxyData proxyData, uint64_t serial)
        : WMWindowEvent(c)
        , size(c->size())
        , dpr(float(c->effectiveDevicePixelRatio()))
        , syncInExpose(inExpose)
        , forceRenderPass(force)
        , scProxyData(std::move(proxyData))
        , serial(serial)
    {}
    QSize size;
    float dpr;
    bool syncInExpose;
    bool forceRenderPass;
    QRhiSwapChainProxyData scProxyData;
    uint64_t serial;
};


class WMGrabEvent : public WMWindowEvent
{
public:
    WMGrabEvent(QQuickWindow *c, QImage *result) :
        WMWindowEvent(c), image(result) {}
    QImage *image;
};

class WMJobEvent : public WMWindowEvent
{
public:
    WMJobEvent(QQuickWindow *c, QRunnable *postedJob)
        : WMWindowEvent(c), job(postedJob) {}
    std::unique_ptr<QRunnable> job;
};

class WMReleaseSwapchainEvent : public WMWindowEvent
{
public:
    WMReleaseSwapchainEvent(QQuickWindow *c) :
        WMWindowEvent(c) { }
};

using QSGRenderThreadEvent = std::variant<
    WMObscureEvent,
    WMExposedEvent,
    WMTryReleaseEvent,
    WMSyncEvent,
    WMGrabEvent,
    WMJobEvent,
    WMReleaseSwapchainEvent
>;

class QSGRenderThreadEventQueue
{
public:
    QSGRenderThreadEventQueue()
        : waiting(false)
    {
        m_queue.reserve(8);
        m_drainBuffer.reserve(8);
    }

    void addEvent(QSGRenderThreadEvent &&e) {
        bool wake = false;
        {
            QMutexLocker lock(&mutex);
            m_queue.emplace_back(std::move(e));
            wake = waiting;
        }
        if (wake)
            condition.wakeOne();
    }

    std::vector<QSGRenderThreadEvent> &drain() {
        QMutexLocker lock(&mutex);
        m_drainBuffer.clear();
        std::swap(m_queue, m_drainBuffer);
        return m_drainBuffer;
    }

    std::vector<QSGRenderThreadEvent> &drainOrWait() {
        QMutexLocker lock(&mutex);
        if (m_queue.empty()) {
            waiting = true;
            do {
                condition.wait(&mutex);
            } while (m_queue.empty());
            waiting = false;
        }
        m_drainBuffer.clear();
        std::swap(m_queue, m_drainBuffer);
        return m_drainBuffer;
    }

private:
    QMutex mutex;
    QWaitCondition condition;
    bool waiting;
    std::vector<QSGRenderThreadEvent> m_queue;
    std::vector<QSGRenderThreadEvent> m_drainBuffer;
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
        , syncResultedInChanges(false)
    {
        sgrc = static_cast<QSGDefaultRenderContext *>(renderContext);
#if defined(Q_OS_QNX) || defined(Q_OS_INTEGRITY)
        setStackSize(1024 * 1024);
#endif
    }

    ~QSGRenderThread()
    {
        delete sgrc;
        delete offscreenSurface;
    }

    void invalidateGraphics(QQuickWindow *window, bool inDestructor);

    bool processEvent(QSGRenderThreadEvent &e);
    void run() override;

    void syncAndRender();
    void sync();

    void requestRepaint()
    {
        if (sleeping)
            stopEventProcessing = true;
        if (window)
            pendingUpdate |= RepaintRequest;
    }

    void processEventsAndWaitForMore();
    void processEvents();
    void postEvent(QSGRenderThreadEvent &&e);

public:
    enum {
        SyncRequest         = 0x01,
        RepaintRequest      = 0x02,
        ExposeRequest       = 0x04 | RepaintRequest | SyncRequest
    };

    void ensureRhiDevice();
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

    std::atomic<bool> active;

    QMutex mutex;
    QWaitCondition waitCondition;

    QElapsedTimer m_threadTimeBetweenRenders;

    QQuickWindow *window;
    QSize windowSize;
    float dpr = 1;
    QRhiSwapChainProxyData scProxyData;
    int rhiSampleCount = 1;
    bool rhiDeviceLost = false;
    bool rhiDoomed = false;
    bool guiNotifiedAboutRhiFailure = false;
    bool swRastFallbackDueToSwapchainFailure = false;
    bool lastFrameValid = false;
    bool pipelineCacheLoaded = false;
    std::atomic<bool> rhiReady{false};

    bool stopEventProcessing;
    QSGRenderThreadEventQueue eventQueue;

    bool syncResultedInChanges;
    QSGRenderer *m_connectedRenderer = nullptr;

    std::atomic<uint64_t> syncAcknowledgedSerial{0};
    std::atomic<uint64_t> renderCompletedSerial{0};
    uint64_t lastPostedSyncSerial = 0;
    uint64_t currentSyncSerial = 0;
    int pipelinedFramesRemaining = 0;

    QSize m_lastPixelSize;

    bool firstFrameAfterExpose = false;

public slots:
    void sceneGraphChanged() {
        syncResultedInChanges = true;
    }
};

namespace {
template<class... Ts> struct overloaded : Ts... { using Ts::operator()...; };
template<class... Ts> overloaded(Ts...) -> overloaded<Ts...>;

static const QString &pipelineCachePath()
{
    static const QString path = QStandardPaths::writableLocation(QStandardPaths::CacheLocation)
                                + QLatin1String("/qsg_pipeline_cache.bin");
    return path;
}

Q_GLOBAL_STATIC(QMutex, pipelineCacheFileMutex)

static void savePipelineCache(QRhi *rhi)
{
    const QByteArray data = rhi->pipelineCacheData();
    if (data.isEmpty())
        return;
    const QString &path = pipelineCachePath();
    static const QString dirPath = QFileInfo(path).absolutePath();
    QMutexLocker fileLock(pipelineCacheFileMutex());
    QDir().mkpath(dirPath);
    QFile f(path);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        f.write(data);
    qCDebug(QSG_LOG_RENDERLOOP, "RHI pipeline cache saved (%lld bytes)", (long long)data.size());
}

static void loadPipelineCache(QRhi *rhi)
{
    QFile f(pipelineCachePath());
    if (!f.open(QIODevice::ReadOnly))
        return;
    if (uchar *mapped = f.map(0, f.size())) {
        rhi->setPipelineCacheData(QByteArray::fromRawData(reinterpret_cast<const char *>(mapped), f.size()));
        f.unmap(mapped);
    } else {
        rhi->setPipelineCacheData(f.readAll());
    }
    qCDebug(QSG_LOG_RENDERLOOP, "RHI pipeline cache loaded (%lld bytes)", (long long)f.size());
}

}

bool QSGRenderThread::processEvent(QSGRenderThreadEvent &e)
{
    return std::visit(overloaded {
    [&](WMObscureEvent &) {
        qCDebug(QSG_LOG_RENDERLOOP, QSG_RT_PAD, "WM_Obscure");
        if (window) {
            QQuickWindowPrivate::get(window)->fireAboutToStop();
            qCDebug(QSG_LOG_RENDERLOOP, QSG_RT_PAD, "- window removed");
            window = nullptr;
            lastFrameValid = false;
        }
        return true;
    },
    [&](WMExposedEvent &e) {
        qCDebug(QSG_LOG_RENDERLOOP, QSG_RT_PAD, "WM_Exposed");
        window = e.window;
        windowSize = e.size;
        dpr = e.dpr;
        m_lastPixelSize = QSize(static_cast<int>(e.size.width() * e.dpr),
                                static_cast<int>(e.size.height() * e.dpr));
        pipelinedFramesRemaining = 0;
        renderCompletedSerial.store(syncAcknowledgedSerial.load(std::memory_order_relaxed),
                                    std::memory_order_relaxed);
        firstFrameAfterExpose = true;
        return true;
    },
    [&](WMSyncEvent &e) {
        qCDebug(QSG_LOG_RENDERLOOP, QSG_RT_PAD, "WM_RequestSync");
        if (sleeping)
            stopEventProcessing = true;
        window = e.window;
        windowSize = e.size;
        dpr = e.dpr;
        m_lastPixelSize = QSize(static_cast<int>(e.size.width() * e.dpr),
                                static_cast<int>(e.size.height() * e.dpr));
        scProxyData = std::move(e.scProxyData);
        currentSyncSerial = e.serial;

        pendingUpdate |= SyncRequest;
        if (e.syncInExpose) {
            qCDebug(QSG_LOG_RENDERLOOP, QSG_RT_PAD, "- triggered from expose");
            pendingUpdate |= ExposeRequest;
        }
        if (e.forceRenderPass) {
            qCDebug(QSG_LOG_RENDERLOOP, QSG_RT_PAD, "- repaint regardless");
            pendingUpdate |= RepaintRequest;
        }
        return true;
    },
    [&](WMTryReleaseEvent &e) {
        qCDebug(QSG_LOG_RENDERLOOP, QSG_RT_PAD, "WM_TryRelease");
        {
            QMutexLocker lock(&mutex);
            if (!window || e.inDestructor) {
                qCDebug(QSG_LOG_RENDERLOOP, QSG_RT_PAD, "- setting exit flag and invalidating");
                invalidateGraphics(e.window, e.inDestructor);
                active.store(rhi != nullptr, std::memory_order_relaxed);
                Q_ASSERT_X(!e.inDestructor || !active, "QSGRenderThread::invalidateGraphics()", "Thread's active state is not set to false when shutting down");
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
        }
        waitCondition.wakeOne();
        return true;
    },
    [&](WMGrabEvent &e) {
        qCDebug(QSG_LOG_RENDERLOOP, QSG_RT_PAD, "WM_Grab");
        Q_ASSERT(e.window);
        Q_ASSERT(e.window == window || !window);
        {
            QMutexLocker lock(&mutex);
            if (rhi) {
                QQuickWindowPrivate *cd = QQuickWindowPrivate::get(e.window);
                cd->rhi->makeThreadLocalNativeContextCurrent();
                cd->rhi->beginFrame(cd->swapchain);
                if (!lastFrameValid) {
                    cd->syncSceneGraph();
                    sgrc->endSync();
                    cd->renderSceneGraph();
                }
                *e.image = QSGRhiSupport::instance()->grabAndBlockInCurrentFrame(rhi, cd->swapchain->currentFrameCommandBuffer());
                cd->rhi->endFrame(cd->swapchain, QRhi::SkipPresent);
                e.image->setDevicePixelRatio(e.window->effectiveDevicePixelRatio());
            }
        }
        qCDebug(QSG_LOG_RENDERLOOP, QSG_RT_PAD, "- waking gui to handle result");
        waitCondition.wakeOne();
        return true;
    },
    [&](WMJobEvent &e) {
        qCDebug(QSG_LOG_RENDERLOOP, QSG_RT_PAD, "WM_PostJob");
        Q_ASSERT(e.window == window);
        if (window) {
            if (rhi)
                rhi->makeThreadLocalNativeContextCurrent();
            e.job->run();
            e.job.reset();
            qCDebug(QSG_LOG_RENDERLOOP, QSG_RT_PAD, "- job done");
        }
        return true;
    },
    [&](WMReleaseSwapchainEvent &e) {
        qCDebug(QSG_LOG_RENDERLOOP, QSG_RT_PAD, "WM_ReleaseSwapchain");
        Q_ASSERT(e.window);
        {
            QMutexLocker lock(&mutex);
            wm->releaseSwapchain(e.window);
            lastFrameValid = false;
            qCDebug(QSG_LOG_RENDERLOOP, QSG_RT_PAD, "- swapchain released");
        }
        waitCondition.wakeOne();
        return true;
    }
    }, e);
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
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    if (inDestructor)
        dd->animationController.reset();

    qCDebug(QSG_LOG_RENDERLOOP, QSG_RT_PAD, "- invalidating scene graph");

    lastFrameValid = false;

    if (wipeGraphics) {
        if (dd->swapchain) {
            if (window->handle()) {
                wm->releaseSwapchain(window);
            } else {
                qWarning("QSGThreadedRenderLoop cleanup with QQuickWindow %p swapchain %p still alive, this should not happen.",
                         window, dd->swapchain);
            }
        }
        if (ownRhi) {
            savePipelineCache(rhi);
            QSGRhiSupport::instance()->destroyRhi(rhi, dd->graphicsConfig);
        }
        rhi = nullptr;
        rhiReady.store(false, std::memory_order_release);
        dd->rhi = nullptr;
        qCDebug(QSG_LOG_RENDERLOOP, QSG_RT_PAD, "- QRhi destroyed");
    } else {
        qCDebug(QSG_LOG_RENDERLOOP, QSG_RT_PAD, "- persistent GL, avoiding cleanup");
    }
}

void QSGRenderThread::sync()
{
    auto *d = QQuickWindowPrivate::get(window);
    bool canSync = (rhi && windowSize.isValid());

    if (canSync) [[likely]] {
        rhi->makeThreadLocalNativeContextCurrent();
        if (d->renderer) [[likely]] {
            if (d->renderer != m_connectedRenderer) {
                if (m_connectedRenderer)
                    disconnect(m_connectedRenderer, &QSGRenderer::sceneGraphChanged, this, &QSGRenderThread::sceneGraphChanged);
                connect(d->renderer, &QSGRenderer::sceneGraphChanged, this, &QSGRenderThread::sceneGraphChanged, Qt::DirectConnection);
                m_connectedRenderer = d->renderer;
            }
            d->renderer->clearChangedFlag();
        }
        syncResultedInChanges = false;
        d->syncSceneGraph();
    }

    syncAcknowledgedSerial.store(currentSyncSerial, std::memory_order_release);
    syncAcknowledgedSerial.notify_one();

    if (canSync) [[likely]]
        sgrc->endSync();

    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

void QSGRenderThread::teardownGraphics()
{
    QQuickWindowPrivate *wd = QQuickWindowPrivate::get(window);
    wd->cleanupNodesOnShutdown();
    sgrc->invalidate();
    wm->releaseSwapchain(window);
    if (ownRhi) {
        savePipelineCache(rhi);
        QSGRhiSupport::instance()->destroyRhi(rhi, {});
    }
    rhi = nullptr;
    rhiReady.store(false, std::memory_order_release);
    lastFrameValid = false;
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
    auto *cd = QQuickWindowPrivate::get(window);
    const bool syncRequested = (pendingUpdate & SyncRequest);
    const bool exposeRequested = (pendingUpdate & ExposeRequest) == ExposeRequest;
    const bool repaintRequested = (pendingUpdate & RepaintRequest);
    pendingUpdate = 0;
    [[assume(window != nullptr)]];
    [[assume(cd != nullptr)]];

    const bool animatorRunning = animatorDriver->isRunning();
    const bool hasValidSwapChain = (cd->swapchain && windowSize.isValid());

    if (hasValidSwapChain && !rhi->isRecordingFrame()) [[likely]] {
        rhi->makeThreadLocalNativeContextCurrent();
    }

    if (animatorRunning) [[unlikely]] {
        cd->animationController->lock();
        animatorDriver->advance();
        cd->animationController->unlock();
    }

    if (syncRequested) [[likely]] {
        sync();
    }

    if (syncRequested && !syncResultedInChanges && !exposeRequested
        && lastFrameValid && !repaintRequested && !animatorRunning) {
        qCDebug(QSG_LOG_RENDERLOOP, QSG_RT_PAD, "- sync produced no changes, skipping render");
        {
            QMutexLocker lock(&mutex);
            renderCompletedSerial.store(currentSyncSerial, std::memory_order_relaxed);
        }
        waitCondition.wakeOne();
        return;
    }

    bool gpuStarted = false;
    if (hasValidSwapChain) [[likely]] {
        cd->swapchain->setProxyData(scProxyData);
        const QSize effectiveOutputSize = cd->swapchain->surfacePixelSize();

        if (!effectiveOutputSize.isEmpty()) [[likely]] {
            const QSize previousOutputSize = cd->swapchain->currentPixelSize();
            if (previousOutputSize != effectiveOutputSize || cd->swapchainJustBecameRenderable) [[unlikely]] {
                cd->hasActiveSwapchain = cd->swapchain->createOrResize();

                if (!cd->hasActiveSwapchain) [[unlikely]] {
                    if (rhi->isDeviceLost()) {
                        handleDeviceLoss();
                    } else if (previousOutputSize.isEmpty() && !swRastFallbackDueToSwapchainFailure &&
                              QSGRhiSupport::instance()->attemptReinitWithSwRastUponFail()) {
                        swRastFallbackDueToSwapchainFailure = true;
                        teardownGraphics();
                    }
                }

                cd->swapchainJustBecameRenderable = false;
                cd->hasRenderableSwapchain = cd->hasActiveSwapchain;
            }

            if (cd->hasActiveSwapchain) {
                emit window->beforeFrameBegin();

                if (rhi->beginFrame(cd->swapchain) == QRhi::FrameOpSuccess) {
                    gpuStarted = true;
                } else {
                    if (rhi->isDeviceLost())
                        handleDeviceLoss();
                    emit window->afterFrameEnd();
                }
            }
        }
    }

    if (exposeRequested && !gpuStarted) {
        QMetaObject::invokeMethod(wm, [wm = this->wm, win = this->window]() {
            if (QSGThreadedRenderLoop::Window *w = wm->windowFor(win))
                w->forceRenderPass = true;
        }, Qt::QueuedConnection);
        QCoreApplication::postEvent(window, new QEvent(QEvent::Type(QQuickWindowPrivate::FullUpdateRequest)));
        return;
    }

    if (gpuStarted && cd->renderer) [[likely]] {
#ifdef Q_OS_ANDROID
        if (firstFrameAfterExpose) {
            auto *cb = cd->swapchain->currentFrameCommandBuffer();
            cb->beginPass(cd->swapchain->currentFrameRenderTarget(),
                          cd->renderer->clearColor(),
                          { 1.0f, 0 },
                          cd->rpDescForSwapchain);
            cb->endPass();

            firstFrameAfterExpose = false;
            pendingUpdate |= RepaintRequest;
        } else
#endif
        {
            cd->renderSceneGraph();
        }

        const bool asyncPresent = rhi->backend() != QRhi::OpenGLES2;
        if (asyncPresent) {
            {
                QMutexLocker lock(&mutex);
                renderCompletedSerial.store(currentSyncSerial, std::memory_order_relaxed);
            }
            waitCondition.wakeOne();
        }

        if (rhi->endFrame(cd->swapchain) != QRhi::FrameOpSuccess) [[unlikely]] {
            if (rhi->isDeviceLost())
                handleDeviceLoss();
            QCoreApplication::postEvent(window, new QEvent(QEvent::Type(QQuickWindowPrivate::FullUpdateRequest)));
            lastFrameValid = false;
            if (!asyncPresent) {
                {
                    QMutexLocker lock(&mutex);
                    renderCompletedSerial.store(currentSyncSerial, std::memory_order_relaxed);
                }
                waitCondition.wakeOne();
            }
        } else {
            lastFrameValid = true;
            if (animatorRunning)
                pendingUpdate |= RepaintRequest;
            if (!asyncPresent) {
                {
                    QMutexLocker lock(&mutex);
                    renderCompletedSerial.store(currentSyncSerial, std::memory_order_relaxed);
                }
                waitCondition.wakeOne();
            }
        }

        cd->fireFrameSwapped();
    } else if (gpuStarted) {
        rhi->endFrame(cd->swapchain, QRhi::SkipPresent);
        lastFrameValid = false;
        {
            QMutexLocker lock(&mutex);
            renderCompletedSerial.store(currentSyncSerial, std::memory_order_relaxed);
        }
        waitCondition.wakeOne();
    }

    if (hasValidSwapChain) [[likely]]
        emit window->afterFrameEnd();

    if (!gpuStarted) {
        {
            QMutexLocker lock(&mutex);
            renderCompletedSerial.store(currentSyncSerial, std::memory_order_relaxed);
        }
        waitCondition.wakeOne();
    }
}

void QSGRenderThread::postEvent(QSGRenderThreadEvent &&e)
{
    eventQueue.addEvent(std::move(e));
}

void QSGRenderThread::processEvents()
{
    qCDebug(QSG_LOG_RENDERLOOP, QSG_RT_PAD, "--- begin processEvents()");
    auto &batch = eventQueue.drain();
    for (auto &e : batch)
        processEvent(e);
    batch.clear();
    qCDebug(QSG_LOG_RENDERLOOP, QSG_RT_PAD, "--- done processEvents()");
}

void QSGRenderThread::processEventsAndWaitForMore()
{
    qCDebug(QSG_LOG_RENDERLOOP, QSG_RT_PAD, "--- begin processEventsAndWaitForMore()");
    stopEventProcessing = false;
    while (!stopEventProcessing) {
        auto &batch = eventQueue.drainOrWait();
        for (auto &e : batch)
            processEvent(e);
        batch.clear();
    }
    qCDebug(QSG_LOG_RENDERLOOP, QSG_RT_PAD, "--- done processEventsAndWaitForMore()");
}

void QSGRenderThread::ensureRhiDevice()
{
    if (rhi || rhiDoomed) [[likely]]
        return;

    auto *rhiSupport = QSGRhiSupport::instance();
    auto rhiResult = rhiSupport->createRhi(window, offscreenSurface, swRastFallbackDueToSwapchainFailure);
    rhi = rhiResult.rhi;
    ownRhi = rhiResult.own;
    if (rhi) {
        rhiDeviceLost = false;
        rhiSampleCount = rhiSupport->chooseSampleCountForWindowWithRhi(window, rhi);
        rhi->makeThreadLocalNativeContextCurrent();
        if (!pipelineCacheLoaded) [[unlikely]] {
            loadPipelineCache(rhi);
            pipelineCacheLoaded = true;
        }
        rhiReady.store(true, std::memory_order_release);
    } else {
        if (!rhiDeviceLost)
            rhiDoomed = true;
    }
}

void QSGRenderThread::ensureRhi()
{
    auto *cd = QQuickWindowPrivate::get(window);

    if (rhi && cd->swapchain) [[likely]]
        return;

    if (!m_lastPixelSize.isValid())
        m_lastPixelSize = QSize(static_cast<int>(windowSize.width() * dpr),
                                static_cast<int>(windowSize.height() * dpr));

    const QSize &pixelSize = m_lastPixelSize;

    ensureRhiDevice();

    if (!rhi)
        return;

    if (!sgrc->rhi() && pixelSize.isValid()) [[unlikely]] {
        rhi->makeThreadLocalNativeContextCurrent();
        QSGDefaultRenderContext::InitParams params;
        params.rhi = rhi;
        params.sampleCount = rhiSampleCount;
        params.initialSurfacePixelSize = pixelSize;
        params.maybeSurface = window;
        sgrc->initialize(&params);
    }

    if (!cd->swapchain && window->isExposed()) [[unlikely]] {
        cd->rhi = rhi;
        const auto requestedFormat = window->format();
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

    if (window)
        ensureRhiDevice();

    while (active.load(std::memory_order_relaxed)) [[likely]] {
#ifdef Q_OS_DARWIN
        QMacAutoReleasePool frameReleasePool;
#endif
        processEvents();
        QCoreApplication::sendPostedEvents(nullptr, 0);

        if (window) [[likely]] {
            ensureRhi();
            if (pendingUpdate != 0 || animatorDriver->isRunning())
                syncAndRender();
        }

        if (active && (pendingUpdate == 0 || !window)) [[unlikely]] {
            sleeping = true;
            processEventsAndWaitForMore();
            sleeping = false;
        }
    }

    if (rhi) [[likely]]
        rhi->makeThreadLocalNativeContextCurrent();

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

    connect(m_animation_driver, &QAnimationDriver::started, this, &QSGThreadedRenderLoop::animationStarted);
    connect(m_animation_driver, &QAnimationDriver::stopped, this, &QSGThreadedRenderLoop::animationStopped);

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
    return std::ranges::any_of(m_windows, [](const auto &w) {
        return w.window->isVisible() && w.window->isExposed();
    });
}

bool QSGThreadedRenderLoop::interleaveIncubation() const
{
    return m_animation_driver->isRunning() && anyoneShowing();
}

void QSGThreadedRenderLoop::animationStarted()
{
    qCDebug(QSG_LOG_RENDERLOOP, "- animationStarted()");
    startOrStopAnimationTimer();

    for (auto &w : m_windows)
        postUpdateRequest(&w);
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

    const bool animationsRunning = m_animation_driver->isRunning();

    if (!animationsRunning && m_animation_timer == 0)
        return;

    int exposedWindows = 0;
    int unthrottledWindows = 0;
    int badVSync = 0;
    const Window *theOne = nullptr;
    for (const auto &w : m_windows) {
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

    if (m_animation_timer != 0 && (canUseVSyncBasedAnimation || !animationsRunning)) {
        qCDebug(QSG_LOG_RENDERLOOP, "*** Stopping system (not vsync-based) animation timer (exposedWindows=%d unthrottledWindows=%d badVSync=%d)",
                exposedWindows, unthrottledWindows, badVSync);
        killTimer(m_animation_timer);
        m_animation_timer = 0;
        if (animationsRunning)
            postUpdateRequest(const_cast<Window *>(theOne));
    } else if (m_animation_timer == 0 && !canUseVSyncBasedAnimation && animationsRunning) {
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
    thread->wait();
    Q_ASSERT(thread->thread() == QThread::currentThread());
    delete thread;

    m_windows.removeIf([window](const Window &w) { return w.window == window; });

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
    QPointer<QQuickWindow> safeWindow = window;

    if (!safeWindow) return;

    QQuickWindowPrivate *wd = QQuickWindowPrivate::get(safeWindow);
    if (!safeWindow->isExposed())
        wd->hasRenderableSwapchain = false;

    bool skipThisExpose = false;
    if (safeWindow->isExposed()) {
        QSize surfaceSize;
        if (wd->hasActiveSwapchain && wd->swapchain)
            surfaceSize = wd->swapchain->surfacePixelSize();
        else if (safeWindow->handle())
            surfaceSize = safeWindow->handle()->geometry().size()
                          * safeWindow->effectiveDevicePixelRatio();

        if (surfaceSize.isEmpty()) {
            wd->hasRenderableSwapchain = false;
            skipThisExpose = true;
            QPointer<QQuickWindow> retryWindow = safeWindow;
            QTimer::singleShot(16, this, [this, retryWindow]() {
                if (retryWindow && retryWindow->isExposed())
                    handleExposure(retryWindow);
            });
        }
    }

    if (safeWindow->isExposed() && !wd->hasRenderableSwapchain && wd->hasActiveSwapchain
            && !wd->swapchain->surfacePixelSize().isEmpty())
    {
        wd->hasRenderableSwapchain = true;
        wd->swapchainJustBecameRenderable = true;
    }

    if (safeWindow->isExposed()) {
        if (!skipThisExpose)
            handleExposure(safeWindow);
    } else {
        Window *w = windowFor(safeWindow);
        if (!w) {
            qCDebug(QSG_LOG_RENDERLOOP) << "pre-warming render thread for" << safeWindow;
            auto *wd = QQuickWindowPrivate::get(safeWindow);
            auto *renderContext = wd->context;
            pendingRenderContexts.remove(renderContext);
            m_windows.emplace_back();
            w = &m_windows.back();
            w->window = safeWindow;
            w->actualWindowFormat = safeWindow->format();
            w->thread = new QSGRenderThread(this, renderContext);
            w->updateDuringSync = false;
            w->forceRenderPass = true;
            w->badVSync = false;
            w->psTimeAccumulator = 0.0f;
            w->psTimeSampleCount = 0;
            w->timeBetweenPolishAndSyncs.start();
            auto *rhiSupport = QSGRhiSupport::instance();
            w->thread->offscreenSurface = rhiSupport->maybeCreateOffscreenSurface(safeWindow);
            w->thread->window = safeWindow;
            w->thread->windowSize = safeWindow->size();
            w->thread->dpr = float(safeWindow->effectiveDevicePixelRatio());
            w->thread->scProxyData = QRhi::updateSwapChainProxyData(rhiSupport->rhiBackend(), safeWindow);
            safeWindow->installEventFilter(this);
            if (auto *controller = wd->animationController.get();
                controller->thread() != w->thread) [[unlikely]]
                controller->moveToThread(w->thread);
            w->thread->active.store(true, std::memory_order_relaxed);
            if (w->thread->thread() == QThread::currentThread()) [[unlikely]] {
                w->thread->sgrc->moveToThread(w->thread);
                w->thread->moveToThread(w->thread);
            }
            w->thread->start();
        } else {
            handleObscurity(w);
        }
    }
}

void QSGThreadedRenderLoop::handleExposure(QQuickWindow *window)
{
    auto it = std::ranges::find(m_windows, window, &Window::window);
    Window *w = nullptr;
    if (it != m_windows.end()) [[likely]] {
        w = &*it;
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
            w->thread->windowSize = window->size();
            w->thread->dpr = float(window->effectiveDevicePixelRatio());
            w->thread->scProxyData = QRhi::updateSwapChainProxyData(rhiSupport->rhiBackend(), window);
            window->installEventFilter(this);
        }
        if (auto *controller = QQuickWindowPrivate::get(w->window)->animationController.get();
            controller->thread() != w->thread) [[unlikely]]
            controller->moveToThread(w->thread);
        w->thread->active.store(true, std::memory_order_relaxed);
        if (w->thread->thread() == QThread::currentThread()) [[unlikely]] {
            w->thread->sgrc->moveToThread(w->thread);
            w->thread->moveToThread(w->thread);
        }
        w->thread->start();
    } else {
        w->thread->postEvent(WMExposedEvent(w->window));
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
        w->thread->postEvent(WMObscureEvent(w->window));
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
                    QMutexLocker lock(&w->thread->mutex);
                    w->thread->postEvent(WMReleaseSwapchainEvent(window));
                    w->thread->waitCondition.wait(&w->thread->mutex);
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
    QPointer<QQuickWindow> safeWindow = window;
    QMetaObject::invokeMethod(this, [this, safeWindow]() {
        if (!safeWindow) return;
        if (!QQuickWindowPrivate::get(safeWindow)->updatesEnabled) return;
        Window *w = windowFor(safeWindow);
        if (w)
            polishAndSync(w);
    }, Qt::QueuedConnection);
}

void QSGThreadedRenderLoop::maybeUpdate(QQuickWindow *window)
{
    Window *w = windowFor(window);
    if (w) {
        maybeUpdate(w);
        return;
    }
    if (!window->isExposed()) [[unlikely]] {
        if (!window->handle())
            window->create();
        exposureChanged(window);
    }
}

void QSGThreadedRenderLoop::maybeUpdate(Window *w)
{
    if (!QCoreApplication::instance() || !w || !w->thread->isRunning() || !w->thread->active)
        return;

    QThread *current = QThread::currentThread();
    if (current == w->thread && w->thread->rhi && w->thread->rhi->isDeviceLost())
        return;
    if (current != QCoreApplication::instance()->thread() && (current != w->thread || !m_lockedForSync)) {
        QMetaObject::invokeMethod(this, [this, w]() { maybeUpdate(w); }, Qt::QueuedConnection);
        return;
    }

    qCDebug(QSG_LOG_RENDERLOOP) << "update from item" << w->window;

    if (current == w->thread) {
        qCDebug(QSG_LOG_RENDERLOOP, "- on render thread");
        w->updateDuringSync = true;
        return;
    }

    if (m_inPolish)
        return;

    postUpdateRequest(w);
}

void QSGThreadedRenderLoop::update(QQuickWindow *window)
{
    Window *w = windowFor(window);
    if (!w) {
        if (!window->isExposed()) [[unlikely]] {
            if (!window->handle())
                window->create();
            exposureChanged(window);
        }
        return;
    }

    const bool isRenderThread = QThread::currentThread() == w->thread;

    if (QPlatformWindow *platformWindow = window->handle()) {
        if (isRenderThread && !platformWindow->allowsIndependentThreadedRendering()) {
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
    w->forceRenderPass = true;
    maybeUpdate(w);
}


void QSGThreadedRenderLoop::releaseResources(QQuickWindow *window)
{
    Window *w = windowFor(window);
    if (w)
        releaseResources(w, false);
}

void QSGThreadedRenderLoop::releaseResources(Window *w, bool inDestructor)
{
    qCDebug(QSG_LOG_RENDERLOOP) << "releaseResources()" << (inDestructor ? "in destructor" : "in api-call") << w->window;

    QMutexLocker threadLock(&w->thread->mutex);
    if (w->thread->isRunning() && w->thread->active) {
        QQuickWindow *window = w->window;

        qCDebug(QSG_LOG_RENDERLOOP, "- posting release request to render thread");
        w->thread->postEvent(WMTryReleaseEvent(window, inDestructor, window->handle() == nullptr));
        w->thread->waitCondition.wait(&w->thread->mutex);

        if (!w->thread->active) {
            qCDebug(QSG_LOG_RENDERLOOP) << " - waiting for render thread to exit" << w->window;
            w->thread->wait();
            qCDebug(QSG_LOG_RENDERLOOP) << " - render thread finished" << w->window;
        }
    }
}

void QSGThreadedRenderLoop::polishAndSync(Window *w, bool inExpose)
{
    qCDebug(QSG_LOG_RENDERLOOP) << "polishAndSync" << (inExpose ? "(in expose)" : "(normal)") << w->window;

    QQuickWindow *window = w->window;
    if (!w->thread || (!w->thread->window && !inExpose)) {
        qCDebug(QSG_LOG_RENDERLOOP, "- not exposed, abort");
        return;
    }

    QQuickWindowPrivate::get(window)->deliveryAgentPrivate()->flushFrameSynchronousEvents(window);
    w = windowFor(window);
    if (!w || !w->thread || (!w->thread->window && !inExpose)) {
        qCDebug(QSG_LOG_RENDERLOOP, "- removed after event flushing, abort");
        return;
    }

    if (w->thread->lastPostedSyncSerial > 0) [[likely]] {
        uint64_t observed = w->thread->syncAcknowledgedSerial.load(std::memory_order_acquire);
        if (observed < w->thread->lastPostedSyncSerial) [[unlikely]] {
            qCDebug(QSG_LOG_RENDERLOOP, "- waiting for previous async sync to complete before polishing");
            Q_TRACE(QSG_wait_entry);
            while (observed < w->thread->lastPostedSyncSerial) {
                w->thread->syncAcknowledgedSerial.wait(observed, std::memory_order_acquire);
                observed = w->thread->syncAcknowledgedSerial.load(std::memory_order_acquire);
            }
            Q_TRACE(QSG_wait_exit);
        }
    }

    Q_TRACE(QSG_polishAndSync);
    QElapsedTimer timer;
    qint64 polishTime = 0;
    qint64 waitTime = 0;
    qint64 syncTime = 0;

    const qint64 elapsedSinceLastMs = w->timeBetweenPolishAndSyncs.restart();

    if (!w->badVSync && w->actualWindowFormat.swapInterval() != 0 && sg->isVSyncDependent(m_animation_driver)) {
        w->psTimeAccumulator += elapsedSinceLastMs;
        w->psTimeSampleCount += 1;
        static const int PS_TIME_SAMPLE_LENGTH = 20;
        if (w->psTimeSampleCount > PS_TIME_SAMPLE_LENGTH) [[unlikely]] {
            const float t = w->psTimeAccumulator / w->psTimeSampleCount;
            const float vsyncRate = sg->vsyncIntervalForAnimationDriver(m_animation_driver);
            const float threshold = vsyncRate * 0.5f;
            const bool badVSync = t < threshold;
            if (badVSync && !w->badVSync) {
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

    if (!w->thread || (!w->thread->window && !inExpose)) {
        qCDebug(QSG_LOG_RENDERLOOP, "- removed after polishing, abort");
        return;
    }

    Q_TRACE(QSG_wait_entry);
    w->updateDuringSync = false;

    emit window->afterAnimating();

    QRhiSwapChainProxyData scProxyData =
            QRhi::updateSwapChainProxyData(QSGRhiSupport::instance()->rhiBackend(), window);

    qCDebug(QSG_LOG_RENDERLOOP, "- lock for sync");
    {
        static constexpr int HYSTERESIS_FRAMES = 5;
        const float vsyncMs = sg->vsyncIntervalForAnimationDriver(m_animation_driver);
        const unsigned long ADAPTIVE_TIMEOUT_MS = static_cast<unsigned long>(
            qMax(4.0f, vsyncMs * 0.75f));
        const bool supportsAsyncPresent =
            QSGRhiSupport::instance()->rhiBackend() != QRhi::OpenGLES2;

        m_lockedForSync = true;
        const uint64_t serial = ++w->thread->lastPostedSyncSerial;
        w->thread->postEvent(WMSyncEvent(window, inExpose, w->forceRenderPass, std::move(scProxyData), serial));
        w->forceRenderPass = false;

        if (inExpose && w->thread->rhiReady.load(std::memory_order_acquire)) {
            qCDebug(QSG_LOG_RENDERLOOP, "- inExpose (pre-warmed): skipping sync wait for fast startup");
            m_lockedForSync = false;
            Q_TRACE(QSG_wait_exit);
            Q_TRACE(QSG_sync_exit);
            Q_TRACE(QSG_animations_exit);
            Q_QUICK_SG_PROFILE_END(QQuickProfiler::SceneGraphPolishAndSync,
                                   QQuickProfiler::SceneGraphPolishAndSyncAnimations);
            return;
        }

        if (profileFrames)
            waitTime = timer.nsecsElapsed();
        Q_TRACE(QSG_wait_exit);
        Q_QUICK_SG_PROFILE_RECORD(QQuickProfiler::SceneGraphPolishAndSync,
                                  QQuickProfiler::SceneGraphPolishAndSyncWait);
        Q_TRACE(QSG_sync_entry);

        qCDebug(QSG_LOG_RENDERLOOP, "- waiting for sync");
        uint64_t observed = w->thread->syncAcknowledgedSerial.load(std::memory_order_acquire);
        while (observed < serial) {
            w->thread->syncAcknowledgedSerial.wait(observed, std::memory_order_acquire);
            observed = w->thread->syncAcknowledgedSerial.load(std::memory_order_acquire);
        }
        m_lockedForSync = false;
        qCDebug(QSG_LOG_RENDERLOOP, "- sync done");

        Q_TRACE(QSG_sync_exit);
        Q_QUICK_SG_PROFILE_RECORD(QQuickProfiler::SceneGraphPolishAndSync,
                                  QQuickProfiler::SceneGraphPolishAndSyncSync);

        if (!inExpose) {
            if (w->thread->pipelinedFramesRemaining > 0) {
                qCDebug(QSG_LOG_RENDERLOOP, "- pipeline mode (%d frames remaining)",
                        w->thread->pipelinedFramesRemaining);
                --w->thread->pipelinedFramesRemaining;
            } else if (!supportsAsyncPresent) {
                qCDebug(QSG_LOG_RENDERLOOP, "- OpenGL backend: staying in pipeline mode");
            } else {
                if (w->thread->renderCompletedSerial.load(std::memory_order_relaxed) < serial) {
                    QMutexLocker timeoutLock(&w->thread->mutex);
                    w->thread->waitCondition.wait(&w->thread->mutex, ADAPTIVE_TIMEOUT_MS);
                }
                if (w->thread->renderCompletedSerial.load(std::memory_order_relaxed) < serial) {
                    qCDebug(QSG_LOG_RENDERLOOP, "- render overran budget, entering pipeline mode");
                    w->thread->pipelinedFramesRemaining = HYSTERESIS_FRAMES;
                } else {
                    qCDebug(QSG_LOG_RENDERLOOP, "- zero-latency frame");
                }
            }
        }
    }

    if (profileFrames)
        syncTime = timer.nsecsElapsed();
    Q_TRACE(QSG_animations_entry);

    if (m_animation_timer == 0 && m_animation_driver->isRunning()) {
        auto advanceAnimations = [this, window = QPointer(window)] {
            qCDebug(QSG_LOG_RENDERLOOP, "- advancing animations");
            m_animation_driver->advance();
            qCDebug(QSG_LOG_RENDERLOOP, "- animations done..");
            if (window)
                window->requestUpdate();
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
        qCDebug(QSG_LOG_TIME_RENDERLOOP, "[window %p][gui thread] Frame prepared, polish=%d ms, lock=%d ms, sync+renderWait=%d ms, animations=%d ms",
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
    switch (std::to_underlying(e->type())) {

    case std::to_underlying(QEvent::Timer): {
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
    {
        QMutexLocker locker(&w->thread->mutex);
        m_lockedForSync = true;
        qCDebug(QSG_LOG_RENDERLOOP, "- posting grab event");
        w->thread->postEvent(WMGrabEvent(window, &result));
        w->thread->waitCondition.wait(&w->thread->mutex);
        m_lockedForSync = false;
    }

    qCDebug(QSG_LOG_RENDERLOOP, "- grab complete");

    return result;
}

void QSGThreadedRenderLoop::postJob(QQuickWindow *window, QRunnable *job)
{
    Window *w = windowFor(window);
    if (w && w->thread && w->thread->window)
        w->thread->postEvent(WMJobEvent(window, job));
    else
        delete job;
}

QT_END_NAMESPACE

#include "qsgthreadedrenderloop.moc"
#include "moc_qsgthreadedrenderloop_p.cpp"
