// Copyright (C) 2016 The Qt Company Ltd.
// Copyright (C) 2016 Jolla Ltd, author: <gunnar.sletta@jollamobile.com>
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only


#include <QtCore/QMutex>
#include <QtCore/QWaitCondition>
#include <QtCore/QAnimationDriver>
#include <QtCore/QCoreApplication>
#include <QtCore/QTimer>
#include <QtCore/QStandardPaths>
#include <QtCore/QFile>
#include <QtCore/QHash>
#include <QtCore/QPointer>
#include <QtCore/QSet>
#include <QtCore/QSaveFile>
#include <QtCore/QDir>
#include <QtCore/QThreadPool>
#include <atomic>
#include <memory>
#include <variant>
#include <utility>
#include <vector>

#include <QtGui/QOffscreenSurface>
#include <QtGui/QGuiApplication>
#include <QtGui/QPlatformSurfaceEvent>

#include <QtQuick/QQuickWindow>
#include <private/qquickwindow_p.h>
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

using namespace Qt::StringLiterals;

Q_TRACE_POINT(qtquick, QSG_polishAndSync_entry)
Q_TRACE_POINT(qtquick, QSG_polishAndSync_exit)
Q_TRACE_POINT(qtquick, QSG_wait_entry)
Q_TRACE_POINT(qtquick, QSG_wait_exit)
Q_TRACE_POINT(qtquick, QSG_syncAndRender_entry)
Q_TRACE_POINT(qtquick, QSG_syncAndRender_exit)
Q_TRACE_POINT(qtquick, QSG_animations_entry)
Q_TRACE_POINT(qtquick, QSG_animations_exit)

#define QSG_RT_PAD "                    (RT) %s"

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

using RenderThreadWaitToken = std::shared_ptr<std::atomic<bool>>;

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
    WMTryReleaseEvent(QQuickWindow *win, bool destroy, bool needsFallbackSurface, RenderThreadWaitToken waitToken)
        : WMWindowEvent(win)
        , inDestructor(destroy)
        , needsFallbackSurface(needsFallbackSurface)
        , done(std::move(waitToken))
    {}

    bool inDestructor;
    bool needsFallbackSurface;
    RenderThreadWaitToken done;
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
    WMGrabEvent(QQuickWindow *c, QImage *result, RenderThreadWaitToken waitToken) :
        WMWindowEvent(c), image(result), done(std::move(waitToken)) {}
    QImage *image;
    RenderThreadWaitToken done;
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
    WMReleaseSwapchainEvent(QQuickWindow *c, RenderThreadWaitToken waitToken) :
        WMWindowEvent(c), done(std::move(waitToken)) { }
    RenderThreadWaitToken done;
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
        m_queue.reserve(2);
        m_drainBuffer.reserve(2);
    }

    void addEvent(QSGRenderThreadEvent &&e) {
        const bool wake = [&] {
            QMutexLocker lock(&mutex);
            m_queue.emplace_back(std::move(e));
            return waiting;
        }();
        if (wake)
            condition.wakeOne();
    }

    std::vector<QSGRenderThreadEvent> &drain() {
        QMutexLocker lock(&mutex);
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

    void invalidateGraphics(QQuickWindow *window, bool inDestructor, bool needsFallbackSurface);

    void processEvent(QSGRenderThreadEvent &e);
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

    QQuickWindow *window;
    QSize windowSize;
    float dpr = 1;
    QRhiSwapChainProxyData scProxyData;
    int rhiSampleCount = 1;
    bool rhiDeviceLost = false;
    bool rhiDoomed = false;
    bool swRastFallbackDueToSwapchainFailure = false;
    bool lastFrameValid = false;
    std::atomic<bool> rhiReady{false};
    std::atomic<bool> deferredExposeRequest{false};
    std::atomic<bool> surfaceAboutToBeDestroyed{false};

    bool stopEventProcessing;
    QSGRenderThreadEventQueue eventQueue;

    bool syncResultedInChanges;
    QPointer<QSGRenderer> m_connectedRenderer;

    std::atomic<uint64_t> syncAcknowledgedSerial{0};
    uint64_t lastPostedSyncSerial = 0;
    uint64_t currentSyncSerial = 0;
    bool syncDoneBeforeEnsure = false;

    QSize m_lastPixelSize;


public slots:
    void sceneGraphChanged() {
        syncResultedInChanges = true;
    }
};

namespace {
template<class... Ts> struct overloaded : Ts... { using Ts::operator()...; };
template<class... Ts> overloaded(Ts...) -> overloaded<Ts...>;

constexpr qint64 NsecsPerMillisecond = 1000000;
constexpr qsizetype PipelineCachePathExtraReserve = 32;
constexpr auto PipelineCacheFilePrefix = "/qsg_pipeline_cache_backend_"_L1;
constexpr auto PipelineCacheFileSuffix = ".bin"_L1;

bool depthBufferEnabled()
{
    static const bool enabled = qEnvironmentVariableIsEmpty("QSG_NO_DEPTH_BUFFER");
    return enabled;
}

constexpr int nsecsToMillis(qint64 nsecs) noexcept
{
    return int(nsecs / NsecsPerMillisecond);
}

constexpr int pipelineCacheBackendKey(QRhi::Implementation backend) noexcept
{
    return int(backend);
}

QString pipelineCachePath(const QString &cacheDir, QRhi::Implementation backend)
{
    QString path = cacheDir;
    path.reserve(cacheDir.size() + PipelineCachePathExtraReserve);
    path += PipelineCacheFilePrefix;
    path += QString::number(pipelineCacheBackendKey(backend));
    path += PipelineCacheFileSuffix;
    return path;
}

QString pipelineCachePath(QRhi::Implementation backend)
{
    return pipelineCachePath(QStandardPaths::writableLocation(QStandardPaths::CacheLocation), backend);
}

QMutex *pipelineCacheMutex()
{
    static QMutex mutex;
    return &mutex;
}

QHash<int, QByteArray> &pipelineCacheDataByBackend()
{
    static QHash<int, QByteArray> dataByBackend;
    return dataByBackend;
}

QByteArray ensurePipelineCacheDataLoaded(QRhi::Implementation backend)
{
    const int key = pipelineCacheBackendKey(backend);
    {
        QMutexLocker lock(pipelineCacheMutex());
        QHash<int, QByteArray> &dataByBackend = pipelineCacheDataByBackend();
        const auto it = dataByBackend.constFind(key);
        if (it != dataByBackend.constEnd())
            return it.value();
    }

    QByteArray data;
    QFile f(pipelineCachePath(backend));
    if (f.open(QIODevice::ReadOnly))
        data = f.readAll();

    QMutexLocker lock(pipelineCacheMutex());
    QHash<int, QByteArray> &dataByBackend = pipelineCacheDataByBackend();
    auto it = dataByBackend.find(key);
    if (it == dataByBackend.end())
        it = dataByBackend.insert(key, std::move(data));
    return it.value();
}

void preloadPipelineCache(QRhi::Implementation backend)
{
    QThreadPool::globalInstance()->start([backend]() {
        const QByteArray data = ensurePipelineCacheDataLoaded(backend);
        if (!data.isEmpty()) {
            qCDebug(QSG_LOG_RENDERLOOP, "RHI %s pipeline cache preloaded for backend %d (%lld bytes)",
                    QRhi::backendName(backend), pipelineCacheBackendKey(backend), (long long)data.size());
        }
    });
}

void savePipelineCache(QRhi *rhi)
{
    if (!rhi || rhi->isDeviceLost())
        return;

    QByteArray data = rhi->pipelineCacheData();
    if (data.isEmpty())
        return;
    const QRhi::Implementation backend = rhi->backend();
    const int key = pipelineCacheBackendKey(backend);
    QString dirPath = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    QString path = pipelineCachePath(dirPath, backend);
    {
        QMutexLocker lock(pipelineCacheMutex());
        pipelineCacheDataByBackend().insert(key, data);
    }
    QThreadPool::globalInstance()->start([backend, key, data = std::move(data), path = std::move(path), dirPath = std::move(dirPath)]() {
        QMutexLocker lock(pipelineCacheMutex());
        if (pipelineCacheDataByBackend().value(key) != data)
            return;

        QDir().mkpath(dirPath);
        QSaveFile f(path);
        bool saved = false;
        if (f.open(QIODevice::WriteOnly)) {
            f.write(data);
            saved = f.commit();
        }
        if (saved) {
            qCDebug(QSG_LOG_RENDERLOOP, "RHI %s pipeline cache saved for backend %d (%lld bytes)",
                    QRhi::backendName(backend), key, (long long)data.size());
        }
    });
}

void loadPipelineCache(QRhi *rhi)
{
    const QRhi::Implementation backend = rhi->backend();
    const QByteArray data = ensurePipelineCacheDataLoaded(backend);
    if (!data.isEmpty()) {
        rhi->setPipelineCacheData(data);
        qCDebug(QSG_LOG_RENDERLOOP, "RHI %s pipeline cache loaded for backend %d (%lld bytes)",
                QRhi::backendName(backend), pipelineCacheBackendKey(backend), (long long)data.size());
    }
}

}

void QSGRenderThread::processEvent(QSGRenderThreadEvent &e)
{
    std::visit(overloaded {
    [&](WMObscureEvent &) {
        qCDebug(QSG_LOG_RENDERLOOP, QSG_RT_PAD, "WM_Obscure");
        if (window) {
            QQuickWindowPrivate::get(window)->fireAboutToStop();
            qCDebug(QSG_LOG_RENDERLOOP, QSG_RT_PAD, "- window removed");
            window = nullptr;
            lastFrameValid = false;
            deferredExposeRequest.store(false, std::memory_order_release);
        }
    },
    [&](WMExposedEvent &e) {
        qCDebug(QSG_LOG_RENDERLOOP, QSG_RT_PAD, "WM_Exposed");
        window = e.window;
        windowSize = e.size;
        dpr = e.dpr;
        m_lastPixelSize = QSize(static_cast<int>(e.size.width() * e.dpr),
                                static_cast<int>(e.size.height() * e.dpr));
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
    },
    [&](WMTryReleaseEvent &e) {
        qCDebug(QSG_LOG_RENDERLOOP, QSG_RT_PAD, "WM_TryRelease");
        {
            if (!window || e.inDestructor) {
                qCDebug(QSG_LOG_RENDERLOOP, QSG_RT_PAD, "- setting exit flag and invalidating");
                invalidateGraphics(e.window, e.inDestructor, e.needsFallbackSurface);
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
        e.done->store(true, std::memory_order_release);
        e.done->notify_one();
    },
    [&](WMGrabEvent &e) {
        qCDebug(QSG_LOG_RENDERLOOP, QSG_RT_PAD, "WM_Grab");
        Q_ASSERT(e.window);
        Q_ASSERT(e.window == window || !window);
        if (rhi) {
            QQuickWindowPrivate *cd = QQuickWindowPrivate::get(e.window);
            if (cd->swapchain
                    && !surfaceAboutToBeDestroyed.load(std::memory_order_acquire)
                    && e.window->isExposed()) {
                rhi->makeThreadLocalNativeContextCurrent();
                const QRhi::FrameOpResult beginFrameResult = rhi->beginFrame(cd->swapchain);
                if (beginFrameResult == QRhi::FrameOpSuccess) {
                    cd->syncSceneGraph();
                    sgrc->endSync();
                    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
                    cd->renderSceneGraph();
                    *e.image = QSGRhiSupport::instance()->grabAndBlockInCurrentFrame(rhi, cd->swapchain->currentFrameCommandBuffer());
                    const QRhi::FrameOpResult result = rhi->endFrame(cd->swapchain, QRhi::SkipPresent);
                    if (result == QRhi::FrameOpSuccess) {
                        e.image->setDevicePixelRatio(e.window->effectiveDevicePixelRatio());
                    } else {
                        *e.image = {};
                        lastFrameValid = false;
                        if (result == QRhi::FrameOpDeviceLost)
                            handleDeviceLoss();
                        else if (result == QRhi::FrameOpSwapChainOutOfDate) {
                            cd->hasActiveSwapchain = false;
                            cd->swapchainJustBecameRenderable = true;
                            QMetaObject::invokeMethod(e.window, &QQuickWindow::update, Qt::QueuedConnection);
                        }
                    }
                } else {
                    lastFrameValid = false;
                    if (beginFrameResult == QRhi::FrameOpDeviceLost)
                        handleDeviceLoss();
                    else if (beginFrameResult == QRhi::FrameOpSwapChainOutOfDate) {
                        cd->hasActiveSwapchain = false;
                        cd->hasRenderableSwapchain = false;
                        cd->swapchainJustBecameRenderable = true;
                        QMetaObject::invokeMethod(e.window, &QQuickWindow::update, Qt::QueuedConnection);
                    }
                }
            } else {
                qCDebug(QSG_LOG_RENDERLOOP, QSG_RT_PAD, "- skipping grab, surface is not presentable");
            }
        }
        qCDebug(QSG_LOG_RENDERLOOP, QSG_RT_PAD, "- waking gui to handle result");
        e.done->store(true, std::memory_order_release);
        e.done->notify_one();
    },
    [&](WMJobEvent &e) {
        qCDebug(QSG_LOG_RENDERLOOP, QSG_RT_PAD, "WM_PostJob");
        Q_ASSERT(e.window == window);
        if (window) {
            if (rhi)
                rhi->makeThreadLocalNativeContextCurrent();
            e.job->run();
            // Destroy before the next drained render-thread event can run.
            e.job.reset();
            qCDebug(QSG_LOG_RENDERLOOP, QSG_RT_PAD, "- job done");
        }
    },
    [&](WMReleaseSwapchainEvent &e) {
        qCDebug(QSG_LOG_RENDERLOOP, QSG_RT_PAD, "WM_ReleaseSwapchain");
        Q_ASSERT(e.window);

        if (rhi)
            rhi->makeThreadLocalNativeContextCurrent();
        
        wm->releaseSwapchain(e.window);
        lastFrameValid = false;
        qCDebug(QSG_LOG_RENDERLOOP, QSG_RT_PAD, "- swapchain released");
        e.done->store(true, std::memory_order_release);
        e.done->notify_one();
    }
    }, e);
}

void QSGRenderThread::invalidateGraphics(QQuickWindow *window, bool inDestructor, bool needsFallbackSurface)
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

    if (!needsFallbackSurface)
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
                         static_cast<void *>(window),
                         static_cast<void *>(dd->swapchain));
            }
        }
        if (ownRhi) {
            savePipelineCache(rhi);
            QSGRhiSupport::instance()->destroyRhi(rhi, dd->graphicsConfig);
        }
        rhi = nullptr;
        rhiReady.store(false, std::memory_order_release);
        deferredExposeRequest.store(false, std::memory_order_release);
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

    // The GUI can continue once its state has been consumed. The remaining cleanup
    // only touches render-thread-owned objects and completes before the next event.
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
    wd->rhi = nullptr;
    rhiReady.store(false, std::memory_order_release);
    deferredExposeRequest.store(false, std::memory_order_release);
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
    Q_ASSERT(window);
    auto *cd = QQuickWindowPrivate::get(window);
    Q_ASSERT(cd);

    const uint update = std::exchange(pendingUpdate, 0);
    const bool syncRequested = (update & SyncRequest);
    const bool exposeRequested = (update & ExposeRequest) == ExposeRequest;
    const bool repaintRequested = (update & RepaintRequest);
    const bool profileFrame = QSG_LOG_TIME_RENDERLOOP().isDebugEnabled();
    QElapsedTimer frameTimer;
    if (profileFrame)
        frameTimer.start();
    qint64 afterSyncTime = 0;
    qint64 afterSwapchainTime = 0;
    qint64 afterBeginFrameTime = 0;
    qint64 afterRenderTime = 0;
    qint64 afterEndFrameTime = 0;

    const bool animatorRunning = animatorDriver->isRunning();
    const bool hasValidSwapChain = (cd->swapchain && windowSize.isValid());
    const auto surfaceIsPresentable = [this] {
        return !surfaceAboutToBeDestroyed.load(std::memory_order_acquire) && window->isExposed();
    };

    if (hasValidSwapChain && !rhi->isRecordingFrame()) [[likely]] {
        rhi->makeThreadLocalNativeContextCurrent();
    }

    if (animatorRunning) [[unlikely]] {
        cd->animationController->lock();
        animatorDriver->advance();
        cd->animationController->unlock();
    }

    if (syncRequested && !syncDoneBeforeEnsure) [[likely]] {
        sync();
    }
    if (profileFrame)
        afterSyncTime = frameTimer.nsecsElapsed();

    if (syncRequested && !syncResultedInChanges && !exposeRequested
        && lastFrameValid && !repaintRequested && !animatorRunning) {
        qCDebug(QSG_LOG_RENDERLOOP, QSG_RT_PAD, "- sync produced no changes, skipping render");
        return;
    }

    if (hasValidSwapChain && !surfaceIsPresentable()) {
        qCDebug(QSG_LOG_RENDERLOOP, QSG_RT_PAD, "- surface is not presentable, skipping render");
        cd->hasRenderableSwapchain = false;
        lastFrameValid = false;
        return;
    }

    bool gpuStarted = false;
    bool frameBeginSignaled = false;
    if (hasValidSwapChain) [[likely]] {
        cd->swapchain->setProxyData(scProxyData);
        const QSize effectiveOutputSize = cd->swapchain->surfacePixelSize();

        if (!effectiveOutputSize.isEmpty()) [[likely]] {
            const QSize previousOutputSize = cd->swapchain->currentPixelSize();
            if (previousOutputSize != effectiveOutputSize || cd->swapchainJustBecameRenderable) [[unlikely]] {
                cd->hasActiveSwapchain = cd->swapchain->createOrResize();

                if (!cd->hasActiveSwapchain) [[unlikely]] {
                    lastFrameValid = false;
                    if (rhi->isDeviceLost()) {
                        handleDeviceLoss();
                    } else if (previousOutputSize.isEmpty() && !swRastFallbackDueToSwapchainFailure &&
                              QSGRhiSupport::instance()->attemptReinitWithSwRastUponFail()) {
                        swRastFallbackDueToSwapchainFailure = true;
                        teardownGraphics();
                    } else {
                        cd->swapchainJustBecameRenderable = true;
                        QMetaObject::invokeMethod(window, &QQuickWindow::update, Qt::QueuedConnection);
                    }
                } else {
                    cd->swapchainJustBecameRenderable = false;
                }

                cd->hasRenderableSwapchain = cd->hasActiveSwapchain;
            }
            if (profileFrame)
                afterSwapchainTime = frameTimer.nsecsElapsed();

            if (cd->hasActiveSwapchain) {
                if (!surfaceIsPresentable()) {
                    qCDebug(QSG_LOG_RENDERLOOP, QSG_RT_PAD, "- surface became non-presentable before beginFrame, skipping render");
                    cd->hasRenderableSwapchain = false;
                    lastFrameValid = false;
                    return;
                }

                emit window->beforeFrameBegin();
                frameBeginSignaled = true;

                const QRhi::FrameOpResult beginFrameResult = rhi->beginFrame(cd->swapchain);
                if (beginFrameResult == QRhi::FrameOpSuccess) {
                    gpuStarted = true;
                } else {
                    lastFrameValid = false;
                    if (beginFrameResult == QRhi::FrameOpDeviceLost)
                        handleDeviceLoss();
                    else if (beginFrameResult == QRhi::FrameOpSwapChainOutOfDate) {
                        cd->hasActiveSwapchain = false;
                        cd->hasRenderableSwapchain = false;
                        cd->swapchainJustBecameRenderable = true;
                    }
                    emit window->afterFrameEnd();
                    if (exposeRequested
                            || beginFrameResult == QRhi::FrameOpDeviceLost
                            || beginFrameResult == QRhi::FrameOpSwapChainOutOfDate) {
                        QMetaObject::invokeMethod(wm, [wm = this->wm, win = this->window]() {
                            if (QSGThreadedRenderLoop::Window *w = wm->windowFor(win))
                                w->forceRenderPass = true;
                        }, Qt::QueuedConnection);
                        QMetaObject::invokeMethod(window, &QQuickWindow::update, Qt::QueuedConnection);
                    }
                    return;
                }
            }
            if (profileFrame)
                afterBeginFrameTime = frameTimer.nsecsElapsed();
        }
    }

    if (exposeRequested && !gpuStarted) {
        QMetaObject::invokeMethod(wm, [wm = this->wm, win = this->window]() {
            if (QSGThreadedRenderLoop::Window *w = wm->windowFor(win))
                w->forceRenderPass = true;
        }, Qt::QueuedConnection);
        QMetaObject::invokeMethod(window, &QQuickWindow::update, Qt::QueuedConnection);
        return;
    }

    if (gpuStarted && cd->renderer) [[likely]] {
        cd->renderSceneGraph();
        if (profileFrame)
            afterRenderTime = frameTimer.nsecsElapsed();

        const bool skipPresent = !surfaceIsPresentable();
        const QRhi::FrameOpResult endFrameResult = skipPresent
                ? rhi->endFrame(cd->swapchain, QRhi::SkipPresent)
                : rhi->endFrame(cd->swapchain);

        if (endFrameResult != QRhi::FrameOpSuccess) [[unlikely]] {
            if (endFrameResult == QRhi::FrameOpDeviceLost)
                handleDeviceLoss();
            else if (endFrameResult == QRhi::FrameOpSwapChainOutOfDate) {
                cd->hasActiveSwapchain = false;
                cd->hasRenderableSwapchain = false;
                cd->swapchainJustBecameRenderable = true;
            }
            QMetaObject::invokeMethod(window, &QQuickWindow::update, Qt::QueuedConnection);
            lastFrameValid = false;
        } else {
            lastFrameValid = !skipPresent;
            if (skipPresent)
                cd->hasRenderableSwapchain = false;
            if (!skipPresent && animatorRunning)
                pendingUpdate |= RepaintRequest;
        }
        if (profileFrame)
            afterEndFrameTime = frameTimer.nsecsElapsed();

        if (!skipPresent && endFrameResult == QRhi::FrameOpSuccess)
            cd->fireFrameSwapped();
    } else if (gpuStarted) {
        const QRhi::FrameOpResult endFrameResult = rhi->endFrame(cd->swapchain, QRhi::SkipPresent);
        if (endFrameResult == QRhi::FrameOpDeviceLost)
            handleDeviceLoss();
        else if (endFrameResult == QRhi::FrameOpSwapChainOutOfDate) {
            cd->hasActiveSwapchain = false;
            cd->hasRenderableSwapchain = false;
            cd->swapchainJustBecameRenderable = true;
            QMetaObject::invokeMethod(window, &QQuickWindow::update, Qt::QueuedConnection);
        }
        if (profileFrame)
            afterEndFrameTime = frameTimer.nsecsElapsed();
        lastFrameValid = false;
    }

    if (frameBeginSignaled) [[likely]]
        emit window->afterFrameEnd();

    if (profileFrame) {
        const qint64 totalTime = frameTimer.nsecsElapsed();
        if (!afterSwapchainTime)
            afterSwapchainTime = afterSyncTime;
        if (!afterBeginFrameTime)
            afterBeginFrameTime = afterSwapchainTime;
        if (!afterRenderTime)
            afterRenderTime = afterBeginFrameTime;
        if (!afterEndFrameTime)
            afterEndFrameTime = afterRenderTime;
        qCDebug(QSG_LOG_TIME_RENDERLOOP,
                "[window %p][render thread] frame: sync=%d ms, swapchain=%d ms, beginFrame=%d ms, renderSceneGraph=%d ms, endFrame=%d ms, total=%d ms",
                static_cast<void *>(window),
                nsecsToMillis(afterSyncTime),
                nsecsToMillis(afterSwapchainTime - afterSyncTime),
                nsecsToMillis(afterBeginFrameTime - afterSwapchainTime),
                nsecsToMillis(afterRenderTime - afterBeginFrameTime),
                nsecsToMillis(afterEndFrameTime - afterRenderTime),
                nsecsToMillis(totalTime));
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

    const bool profileRhiInit = QSG_LOG_TIME_RENDERLOOP().isDebugEnabled();
    QElapsedTimer rhiInitTimer;
    if (profileRhiInit)
        rhiInitTimer.start();

    auto *rhiSupport = QSGRhiSupport::instance();
    auto rhiResult = rhiSupport->createRhi(window, offscreenSurface, swRastFallbackDueToSwapchainFailure);
    qint64 createRhiTime = 0;
    if (profileRhiInit)
        createRhiTime = rhiInitTimer.nsecsElapsed();
    rhi = rhiResult.rhi;
    ownRhi = rhiResult.own;
    if (rhi) {
        rhiDeviceLost = false;
        rhiSampleCount = rhiSupport->chooseSampleCountForWindowWithRhi(window, rhi);
        rhi->makeThreadLocalNativeContextCurrent();
        qint64 rhiSetupTime = 0;
        if (profileRhiInit)
            rhiSetupTime = rhiInitTimer.nsecsElapsed();
        loadPipelineCache(rhi);
        qint64 pipelineCacheTime = 0;
        if (profileRhiInit)
            pipelineCacheTime = rhiInitTimer.nsecsElapsed();
        if (!sgrc->rhi()) {
            const QSize pixelSize = m_lastPixelSize.isValid()
                ? m_lastPixelSize
                : QSize(static_cast<int>(windowSize.width() * dpr),
                        static_cast<int>(windowSize.height() * dpr));
            if (pixelSize.isValid()) {
                QSGDefaultRenderContext::InitParams params;
                params.rhi = rhi;
                params.sampleCount = rhiSampleCount;
                params.initialSurfacePixelSize = pixelSize;
                params.maybeSurface = window;
                sgrc->initialize(&params);
            }
        }
        if (profileRhiInit) {
            const qint64 totalTime = rhiInitTimer.nsecsElapsed();
            qCDebug(QSG_LOG_TIME_RENDERLOOP,
                    "[window %p][render thread] RHI warm-up: createRhi=%d ms, rhiSetup=%d ms, pipelineCache=%d ms, renderContext=%d ms, total=%d ms",
                    static_cast<void *>(window),
                    nsecsToMillis(createRhiTime),
                    nsecsToMillis(rhiSetupTime - createRhiTime),
                    nsecsToMillis(pipelineCacheTime - rhiSetupTime),
                    nsecsToMillis(totalTime - pipelineCacheTime),
                    nsecsToMillis(totalTime));
        }
        rhiReady.store(true, std::memory_order_release);
        if (deferredExposeRequest.load(std::memory_order_acquire) && window && window->isExposed())
            QMetaObject::invokeMethod(window, &QQuickWindow::requestUpdate, Qt::QueuedConnection);
    } else {
        if (profileRhiInit) {
            qCDebug(QSG_LOG_TIME_RENDERLOOP,
                    "[window %p][render thread] RHI warm-up failed after %d ms",
                    static_cast<void *>(window),
                    nsecsToMillis(createRhiTime));
        }
        deferredExposeRequest.store(false, std::memory_order_release);
        if (!rhiDeviceLost) {
            rhiDoomed = true;
            QCoreApplication::postEvent(window,
                    new QEvent(QEvent::Type(QQuickWindowPrivate::TriggerContextCreationFailure)));
        }
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
        if (depthBufferEnabled()) [[likely]] {
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

    if (window) {
        ensureRhiDevice();
    }

    while (active.load(std::memory_order_relaxed)) [[likely]] {
#ifdef Q_OS_DARWIN
        QMacAutoReleasePool frameReleasePool;
#endif
        processEvents();
        QCoreApplication::sendPostedEvents(nullptr, 0);

        if (!active.load(std::memory_order_relaxed))
            break;

        if (window) [[likely]] {
            syncDoneBeforeEnsure = false;
            if ((pendingUpdate & SyncRequest) && rhi && !QQuickWindowPrivate::get(window)->swapchain) [[unlikely]] {
                syncDoneBeforeEnsure = true;
                sync();
            }
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
#ifdef Q_OS_ANDROID
    qGuiApp->installEventFilter(this);
#endif

    preloadPipelineCache(QSGRhiSupport::instance()->rhiBackend());

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

namespace {
QSet<QQuickWindow *> &prewarmedWindows()
{
    static QSet<QQuickWindow *> windows;
    return windows;
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
    prewarmedWindows().remove(window);

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
            if (safeWindow->isVisible()) {
                QQuickWindow *candidate = safeWindow.data();
                qCDebug(QSG_LOG_RENDERLOOP) << "pre-warming render thread for" << candidate;
                auto *wd = QQuickWindowPrivate::get(candidate);
                auto *renderContext = wd->context;
                pendingRenderContexts.remove(renderContext);
                m_windows.emplace_back();
                Window *w = &m_windows.back();
                w->window = candidate;
                w->actualWindowFormat = candidate->format();
                w->thread = new QSGRenderThread(this, renderContext);
                w->updateDuringSync = false;
                w->forceRenderPass = true;
                w->badVSync = false;
                w->psTimeAccumulator = 0.0f;
                w->psTimeSampleCount = 0;
                w->timeBetweenPolishAndSyncs.start();
                auto *rhiSupport = QSGRhiSupport::instance();
                w->thread->offscreenSurface = rhiSupport->maybeCreateOffscreenSurface(candidate);
                w->thread->window = candidate;
                w->thread->windowSize = candidate->size();
                w->thread->dpr = float(candidate->effectiveDevicePixelRatio());
                w->thread->scProxyData = QRhi::updateSwapChainProxyData(rhiSupport->rhiBackend(), candidate);
                candidate->installEventFilter(this);
                if (auto *controller = wd->animationController.get();
                    controller->thread() != w->thread) [[unlikely]]
                    controller->moveToThread(w->thread);
                w->thread->active.store(true, std::memory_order_relaxed);
                if (w->thread->thread() == QThread::currentThread()) [[unlikely]] {
                    w->thread->sgrc->moveToThread(w->thread);
                    w->thread->moveToThread(w->thread);
                }
                prewarmedWindows().insert(candidate);
                w->thread->start();
            }
        } else if (prewarmedWindows().contains(safeWindow.data())) {
            if (safeWindow->isVisible())
                qCDebug(QSG_LOG_RENDERLOOP) << "- already pre-warmed";
            else
                handleObscurity(w);
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
    prewarmedWindows().remove(window);
    // Surface validity is GUI-thread-owned; queued render events may be stale.
    w->thread->surfaceAboutToBeDestroyed.store(false, std::memory_order_release);
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
        w->thread->postEvent(WMExposedEvent(window));
        w->thread->deferredExposeRequest.store(true, std::memory_order_release);
        w->thread->start();
        if (w->thread->rhiReady.load(std::memory_order_acquire))
            QMetaObject::invokeMethod(window, &QQuickWindow::requestUpdate, Qt::QueuedConnection);
        startOrStopAnimationTimer();
        return;
    } else {
        w->thread->postEvent(WMExposedEvent(w->window));
        w->thread->deferredExposeRequest.store(true, std::memory_order_release);
        if (!w->thread->rhiReady.load(std::memory_order_acquire)) {
            startOrStopAnimationTimer();
            return;
        }
    }
    w->thread->deferredExposeRequest.store(false, std::memory_order_release);
    polishAndSync(w, true);
    startOrStopAnimationTimer();
}

void QSGThreadedRenderLoop::handleObscurity(Window *w)
{
    if (!w)
        return;

    qCDebug(QSG_LOG_RENDERLOOP) << "handleObscurity()" << w->window;
    const bool wasPrewarmed = prewarmedWindows().remove(w->window);
    w->thread->surfaceAboutToBeDestroyed.store(true, std::memory_order_release);
    if (w->thread->isRunning()) {
        if (!wasPrewarmed && !QQuickWindowPrivate::get(w->window)->updatesEnabled) {
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
#ifdef Q_OS_ANDROID
    case QEvent::ApplicationStateChange:
        if (watched == qGuiApp
                && static_cast<QApplicationStateChangeEvent *>(event)->applicationState()
                <= Qt::ApplicationHidden) {
            // Stop presentation before Android destroys the native surface.
            for (Window &w : m_windows) {
                w.thread->surfaceAboutToBeDestroyed.store(true, std::memory_order_release);
                if (!w.thread->isRunning())
                    continue;

                auto isDone = std::make_shared<std::atomic<bool>>(false);
                w.thread->postEvent(WMReleaseSwapchainEvent(w.window, isDone));
                isDone->wait(false, std::memory_order_acquire);
            }
        }
        break;
#endif
    case QEvent::PlatformSurface:
        if (static_cast<QPlatformSurfaceEvent *>(event)->surfaceEventType() == QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed) {
            QQuickWindow *window = qobject_cast<QQuickWindow *>(watched);
            if (window) {
                Window *w = windowFor(window);
                if (w && w->thread->isRunning()) {
                    auto isDone = std::make_shared<std::atomic<bool>>(false);
                    w->thread->surfaceAboutToBeDestroyed.store(true, std::memory_order_release);
                    w->thread->postEvent(WMReleaseSwapchainEvent(window, isDone));
                    isDone->wait(false, std::memory_order_acquire);
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
        if (w) {
            if (!safeWindow->isExposed())
                return;
            if (w->thread->deferredExposeRequest.load(std::memory_order_acquire)
                    && !w->thread->rhiReady.load(std::memory_order_acquire)) {
                qCDebug(QSG_LOG_RENDERLOOP, "- first expose update deferred while RHI warm-up is running");
                return;
            }

            // Keep the lightweight expose path when the surface showed up before RHI warm-up finished.
            const bool inExpose =
                    w->thread->deferredExposeRequest.exchange(false, std::memory_order_acq_rel);
            polishAndSync(w, inExpose);
        }
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
        QPointer<QQuickWindow> safeWindow = w->window;
        QMetaObject::invokeMethod(this, [this, safeWindow]() {
            if (safeWindow)
                if (Window *safeW = windowFor(safeWindow))
                    maybeUpdate(safeW);
        }, Qt::QueuedConnection);
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
    if (w) {
        if (prewarmedWindows().contains(window))
            handleObscurity(w);
        releaseResources(w, false);
    }
}

void QSGThreadedRenderLoop::releaseResources(Window *w, bool inDestructor)
{
    qCDebug(QSG_LOG_RENDERLOOP) << "releaseResources()" << (inDestructor ? "in destructor" : "in api-call") << w->window;

    if (w->thread->isRunning() && w->thread->active.load(std::memory_order_acquire)) {
        QQuickWindow *window = w->window;

        qCDebug(QSG_LOG_RENDERLOOP, "- posting release request to render thread");
        auto isDone = std::make_shared<std::atomic<bool>>(false);
        w->thread->postEvent(WMTryReleaseEvent(window, inDestructor, inDestructor || window->handle() == nullptr, isDone));
        isDone->wait(false, std::memory_order_acquire);

        if (!w->thread->active.load(std::memory_order_acquire)) {
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
    const auto canSyncWindow = [&] {
        return w && w->thread
                && !w->thread->surfaceAboutToBeDestroyed.load(std::memory_order_acquire)
                && (inExpose || w->window->isExposed());
    };
    if (!canSyncWindow()) {
        qCDebug(QSG_LOG_RENDERLOOP, "- not exposed, abort");
        return;
    }

    QQuickWindowPrivate::get(window)->deliveryAgentPrivate()->flushFrameSynchronousEvents(window);
    w = windowFor(window);
    if (!canSyncWindow()) {
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

    if (w->actualWindowFormat.swapInterval() != 0 && sg->isVSyncDependent(m_animation_driver)) {
        static constexpr int   PS_TIME_SAMPLE_LENGTH     = 20;
        static constexpr int   PS_RECOVERY_SAMPLE_LENGTH = 10;
        static constexpr float PS_OUTLIER_FACTOR         = 3.0f;
        static constexpr float PS_DETECT_THRESHOLD       = 0.5f;
        static constexpr float PS_RECOVERY_THRESHOLD     = 0.75f;

        const float vsyncRate = sg->vsyncIntervalForAnimationDriver(m_animation_driver);
        const bool isOutlier  = elapsedSinceLastMs > vsyncRate * PS_OUTLIER_FACTOR;

        if (!w->badVSync) {
            if (isOutlier) {
                w->psTimeAccumulator = 0.0f;
                w->psTimeSampleCount = 0;
            } else {
                w->psTimeAccumulator += elapsedSinceLastMs;
                w->psTimeSampleCount += 1;
                if (w->psTimeSampleCount > PS_TIME_SAMPLE_LENGTH) [[unlikely]] {
                    const float t = w->psTimeAccumulator / w->psTimeSampleCount;
                    if (t < vsyncRate * PS_DETECT_THRESHOLD) {
                        w->badVSync = true;
                        qCDebug(QSG_LOG_INFO, "Window %p is determined to have broken vsync throttling (%f < %f) "
                                              "switching to system timer to drive gui thread animations to remedy this "
                                              "(however, render thread animators will likely advance at an incorrect rate).",
                                static_cast<void *>(w->window), t, vsyncRate * PS_DETECT_THRESHOLD);
                        startOrStopAnimationTimer();
                    }
                    w->psTimeAccumulator = 0.0f;
                    w->psTimeSampleCount = 0;
                }
            }
        } else {
            if (isOutlier) {
                w->psTimeAccumulator = 0.0f;
                w->psTimeSampleCount = 0;
            } else {
                w->psTimeAccumulator += elapsedSinceLastMs;
                w->psTimeSampleCount += 1;
                if (w->psTimeSampleCount > PS_RECOVERY_SAMPLE_LENGTH) [[unlikely]] {
                    const float t = w->psTimeAccumulator / w->psTimeSampleCount;
                    if (t >= vsyncRate * PS_RECOVERY_THRESHOLD) {
                        w->badVSync = false;
                        qCDebug(QSG_LOG_INFO, "Window %p vsync throttling has recovered (%f >= %f), "
                                              "switching back to vsync-based animation.",
                                static_cast<void *>(w->window), t, vsyncRate * PS_RECOVERY_THRESHOLD);
                        startOrStopAnimationTimer();
                    }
                    w->psTimeAccumulator = 0.0f;
                    w->psTimeSampleCount = 0;
                }
            }
        }
    }

    const bool profileFrames = QSG_LOG_TIME_RENDERLOOP().isDebugEnabled();
    timer.start();
    if (profileFrames) {
        qCDebug(QSG_LOG_TIME_RENDERLOOP, "[window %p][gui thread] polishAndSync: start, elapsed since last call: %d ms",
                static_cast<void *>(window),
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

    if (!canSyncWindow()) {
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
        m_lockedForSync = true;
        const uint64_t serial = ++w->thread->lastPostedSyncSerial;
        w->thread->postEvent(WMSyncEvent(window, inExpose, w->forceRenderPass, std::move(scProxyData), serial));
        w->forceRenderPass = false;

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
        const float vsyncMs = sg->vsyncIntervalForAnimationDriver(m_animation_driver);
        const qint64 frameElapsedMs = timer.elapsed();
        const int remaining = static_cast<int>(vsyncMs - frameElapsedMs) - 1;
        if (remaining > 1)
            QTimer::singleShot(remaining, Qt::PreciseTimer, w->window,
                               [window = QPointer(w->window)]() { if (window) window->requestUpdate(); });
        else
            postUpdateRequest(w);
    }

    if (profileFrames) {
        qCDebug(QSG_LOG_TIME_RENDERLOOP, "[window %p][gui thread] Frame prepared, polish=%d ms, lock=%d ms, sync=%d ms, animations=%d ms",
                static_cast<void *>(window),
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
    if (e->type() == QEvent::Timer) {
        Q_ASSERT(sg->isVSyncDependent(m_animation_driver));
        auto *te = static_cast<QTimerEvent *>(e);
        if (te->timerId() == m_animation_timer) {
            qCDebug(QSG_LOG_RENDERLOOP, "- ticking non-render thread timer");
            m_animation_driver->advance();
            emit timeToIncubate();
            return true;
        }
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
    auto isDone = std::make_shared<std::atomic<bool>>(false);
    m_lockedForSync = true;
    qCDebug(QSG_LOG_RENDERLOOP, "- posting grab event");
    w->thread->postEvent(WMGrabEvent(window, &result, isDone));
    isDone->wait(false, std::memory_order_acquire);
    m_lockedForSync = false;

    qCDebug(QSG_LOG_RENDERLOOP, "- grab complete");

    return result;
}

void QSGThreadedRenderLoop::postJob(QQuickWindow *window, QRunnable *job)
{
    Window *w = windowFor(window);
    if (w && w->thread && w->thread->isRunning() && w->thread->active.load(std::memory_order_acquire)
            && !w->thread->surfaceAboutToBeDestroyed.load(std::memory_order_acquire))
        w->thread->postEvent(WMJobEvent(window, job));
    else
        delete job;
}

QT_END_NAMESPACE

#include "qsgthreadedrenderloop.moc"
#include "moc_qsgthreadedrenderloop_p.cpp"
