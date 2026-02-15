// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#include "qsgrenderloop_p.h"
#include "qsgthreadedrenderloop_p.h"
#include "qsgrhisupport_p.h"
#include <private/qquickanimatorcontroller_p.h>

#include <QtCore/QCoreApplication>
#include <QtCore/QElapsedTimer>
#include <QtCore/QLibraryInfo>
#include <QtCore/private/qabstractanimation_p.h>

#include <QtGui/QOffscreenSurface>
#include <QtGui/private/qguiapplication_p.h>
#include <qpa/qplatformintegration.h>
#include <QPlatformSurfaceEvent>

#include <QtQml/private/qqmlglobal_p.h>

#include <QtQuick/QQuickWindow>
#include <QtQuick/private/qquickwindow_p.h>
#include <QtQuick/private/qquickitem_p.h>
#include <QtQuick/private/qsgcontext_p.h>
#include <QtQuick/private/qsgrenderer_p.h>
#include <private/qquickprofiler_p.h>
#include <qtquick_tracepoints_p.h>

#include <private/qsgrhishadereffectnode_p.h>

#include <private/qsgdefaultrendercontext_p.h>

#ifdef Q_OS_WIN
#include <QtCore/qt_windows.h>
#endif

QT_BEGIN_NAMESPACE

extern bool qsg_useConsistentTiming();

#define ENABLE_DEFAULT_BACKEND

Q_TRACE_POINT(qtquick, QSG_renderWindow_entry)
Q_TRACE_POINT(qtquick, QSG_renderWindow_exit)
Q_TRACE_POINT(qtquick, QSG_polishItems_entry)
Q_TRACE_POINT(qtquick, QSG_polishItems_exit)
Q_TRACE_POINT(qtquick, QSG_sync_entry)
Q_TRACE_POINT(qtquick, QSG_sync_exit)
Q_TRACE_POINT(qtquick, QSG_render_entry)
Q_TRACE_POINT(qtquick, QSG_render_exit)
Q_TRACE_POINT(qtquick, QSG_swap_entry)
Q_TRACE_POINT(qtquick, QSG_swap_exit)

DEFINE_BOOL_CONFIG_OPTION(qmlNoThreadedRenderer, QML_BAD_GUI_RENDER_LOOP);
DEFINE_BOOL_CONFIG_OPTION(qmlForceThreadedRenderer, QML_FORCE_THREADED_RENDERER);

QSGRenderLoop *QSGRenderLoop::s_instance = nullptr;

QSGRenderLoop::~QSGRenderLoop()
{
}

void QSGRenderLoop::cleanup()
{
    if (!s_instance) [[unlikely]]
        return;
    for (QQuickWindow *w : s_instance->windows()) {
        QQuickWindowPrivate *wd = QQuickWindowPrivate::get(w);
        if (wd->windowManager == s_instance) {
           s_instance->windowDestroyed(w);
           wd->windowManager = nullptr;
        }
    }
    delete s_instance;
    s_instance = nullptr;
}

QSurface::SurfaceType QSGRenderLoop::windowSurfaceType() const
{
#ifdef ENABLE_DEFAULT_BACKEND
    return QSGRhiSupport::instance()->windowSurfaceType();
#else
    return QSurface::RasterSurface;
#endif
}

void QSGRenderLoop::postJob(QQuickWindow *window, QRunnable *job)
{
    Q_ASSERT(job);
#ifdef ENABLE_DEFAULT_BACKEND
    Q_ASSERT(window);
    QQuickWindowPrivate *cd = QQuickWindowPrivate::get(window);
    if (cd->rhi) [[likely]]
        cd->rhi->makeThreadLocalNativeContextCurrent();
    job->run();
#else
    Q_UNUSED(window);
    job->run();
#endif
    delete job;
}

#ifdef ENABLE_DEFAULT_BACKEND
class QSGGuiThreadRenderLoop : public QSGRenderLoop
{
    Q_OBJECT
public:
    QSGGuiThreadRenderLoop();
    ~QSGGuiThreadRenderLoop();

    void show(QQuickWindow *window) override;
    void hide(QQuickWindow *window) override;

    void windowDestroyed(QQuickWindow *window) override;

    void renderWindow(QQuickWindow *window);
    void exposureChanged(QQuickWindow *window) override;
    QImage grab(QQuickWindow *window) override;

    void maybeUpdate(QQuickWindow *window) override;
    void update(QQuickWindow *window) override { maybeUpdate(window); }
    void handleUpdateRequest(QQuickWindow *) override;

    void releaseResources(QQuickWindow *) override;

    QAnimationDriver *animationDriver() const override { return nullptr; }

    QSGContext *sceneGraphContext() const override;
    QSGRenderContext *createRenderContext(QSGContext *) const override;

    void releaseSwapchain(QQuickWindow *window);
    void handleDeviceLoss();
    void teardownGraphics();

    bool eventFilter(QObject *watched, QEvent *event) override;

    struct WindowData {
        WindowData()
            : updatePending(false),
              rhiDeviceLost(false),
              rhiDoomed(false)
        { }
        QRhi *rhi = nullptr;
        bool ownRhi = true;
        QSGRenderContext *rc = nullptr;
        QElapsedTimer timeBetweenRenders;
        int sampleCount = 1;
        bool updatePending : 1;
        bool rhiDeviceLost : 1;
        bool rhiDoomed : 1;
    };

    bool ensureRhi(QQuickWindow *window, WindowData &data);

    QHash<QQuickWindow *, WindowData> m_windows;

    QOffscreenSurface *offscreenSurface = nullptr;
    QSGContext *sg;
    mutable QSet<QSGRenderContext *> pendingRenderContexts;

    bool m_inPolish = false;

    bool swRastFallbackDueToSwapchainFailure = false;
};
#endif

QSGRenderLoop *QSGRenderLoop::instance()
{
    if (!s_instance) {

        QSGRhiSupport::checkEnvQSgInfo();

        s_instance = QSGContext::createWindowManager();
#ifdef ENABLE_DEFAULT_BACKEND
        if (!s_instance) {
            QSGRhiSupport *rhiSupport = QSGRhiSupport::instance();

            QSGRenderLoopType loopType;
            if (rhiSupport->rhiBackend() != QRhi::OpenGLES2) {
                loopType = ThreadedRenderLoop;
            } else {
                if (QGuiApplicationPrivate::platformIntegration()->hasCapability(QPlatformIntegration::ThreadedOpenGL))
                    loopType = ThreadedRenderLoop;
                else
                    loopType = BasicRenderLoop;
            }

            switch (rhiSupport->rhiBackend()) {
            case QRhi::Null:
                loopType = BasicRenderLoop;
                break;

            case QRhi::D3D11:
                break;

            default:
                break;
            }

            if (qmlNoThreadedRenderer())
                loopType = BasicRenderLoop;
            else if (qmlForceThreadedRenderer())
                loopType = ThreadedRenderLoop;

            if (Q_UNLIKELY(qEnvironmentVariableIsSet("QSG_RENDER_LOOP"))) {
                const QByteArray loopName = qgetenv("QSG_RENDER_LOOP");
                if (loopName == "windows") {
                    qWarning("The 'windows' render loop is no longer supported. Using 'basic' instead.");
                    loopType = BasicRenderLoop;
                } else if (loopName == "basic") {
                    loopType = BasicRenderLoop;
                } else if (loopName == "threaded") {
                    loopType = ThreadedRenderLoop;
                }
            }

            switch (loopType) {
            case ThreadedRenderLoop:
                qCDebug(QSG_LOG_INFO, "threaded render loop");
                s_instance = new QSGThreadedRenderLoop();
                break;
            default:
                qCDebug(QSG_LOG_INFO, "QSG: basic render loop");
                s_instance = new QSGGuiThreadRenderLoop();
                break;
            }
        }
#endif
    }
    return s_instance;
}

void QSGRenderLoop::setInstance(QSGRenderLoop *instance)
{
    Q_ASSERT(!s_instance);
    s_instance = instance;
}

void QSGRenderLoop::handleContextCreationFailure(QQuickWindow *window)
{
    static QSet<QQuickWindow *> recurseGuard;
    if (recurseGuard.contains(window)) [[unlikely]]
        return;
    recurseGuard.insert(window);

    QString translatedMessage;
    QString untranslatedMessage;
    QQuickWindowPrivate::rhiCreationFailureMessage(QSGRhiSupport::instance()->rhiBackendName(),
                                                   &translatedMessage,
                                                   &untranslatedMessage);
    const bool signalEmitted =
        QQuickWindowPrivate::get(window)->emitError(QQuickWindow::ContextNotAvailable,
                                                    translatedMessage);
#if defined(Q_OS_WIN)
    if (!signalEmitted && !QLibraryInfo::isDebugBuild() && !GetConsoleWindow()) [[unlikely]] {
        MessageBox(0, (LPCTSTR) translatedMessage.utf16(),
                   (LPCTSTR)(QCoreApplication::applicationName().utf16()),
                   MB_OK | MB_ICONERROR);
    }
#endif
    if (!signalEmitted) [[unlikely]]
        qFatal("%s", qPrintable(untranslatedMessage));

    recurseGuard.remove(window);
}

#ifdef ENABLE_DEFAULT_BACKEND
QSGGuiThreadRenderLoop::QSGGuiThreadRenderLoop()
{
    if (qsg_useConsistentTiming()) {
        QUnifiedTimer::instance(true)->setConsistentTiming(true);
        qCDebug(QSG_LOG_INFO, "using fixed animation steps");
    }

    sg = QSGContext::createDefaultContext();
}

QSGGuiThreadRenderLoop::~QSGGuiThreadRenderLoop()
{
    qDeleteAll(pendingRenderContexts);
    delete sg;
}

void QSGGuiThreadRenderLoop::show(QQuickWindow *window)
{
    auto [iterator, inserted] = m_windows.try_emplace(window);
    iterator->second.timeBetweenRenders.start();
    maybeUpdate(window);
}

void QSGGuiThreadRenderLoop::hide(QQuickWindow *window)
{
    QQuickWindowPrivate *cd = QQuickWindowPrivate::get(window);
    cd->fireAboutToStop();
    auto it = m_windows.find(window);
    if (it != m_windows.end()) [[likely]]
        it->updatePending = false;
}

void QSGGuiThreadRenderLoop::windowDestroyed(QQuickWindow *window)
{
    hide(window);

    auto it = m_windows.find(window);
    if (it == m_windows.end()) [[unlikely]]
        return;

    WindowData data = std::move(*it);
    m_windows.erase(it);

    auto *d = QQuickWindowPrivate::get(window);

    if (data.rhi) [[likely]]
        data.rhi->makeThreadLocalNativeContextCurrent();

    if (d->swapchain) [[likely]] {
        if (window->handle()) [[likely]]
            releaseSwapchain(window);
        else [[unlikely]]
            qWarning("QSGGuiThreadRenderLoop cleanup with QQuickWindow %p swapchain %p still alive, this should not happen.",
                     window, d->swapchain);
    }

    d->cleanupNodesOnShutdown();

#if QT_CONFIG(quick_shadereffect)
    QSGRhiShaderEffectNode::resetMaterialTypeCache(window);
#endif

    if (data.rc) [[likely]] {
        data.rc->invalidate();
        delete data.rc;
    }

    if (data.ownRhi) [[likely]]
        QSGRhiSupport::instance()->destroyRhi(data.rhi, d->graphicsConfig);

    d->rhi = nullptr;
    d->animationController.reset();

    if (m_windows.isEmpty()) [[unlikely]] {
        delete offscreenSurface;
        offscreenSurface = nullptr;
    }
}

void QSGGuiThreadRenderLoop::teardownGraphics()
{
    for (auto it = m_windows.begin(); it != m_windows.end(); ++it) {
        if (!it->rhi) [[unlikely]]
            continue;

        auto *wd = QQuickWindowPrivate::get(it.key());
        wd->cleanupNodesOnShutdown();
        
        if (it->rc) [[likely]]
            it->rc->invalidate();
        
        releaseSwapchain(it.key());
        
        if (it->ownRhi) [[likely]]
            QSGRhiSupport::instance()->destroyRhi(it->rhi, {});
        
        it->rhi = nullptr;
    }
}

void QSGGuiThreadRenderLoop::handleDeviceLoss()
{
    qWarning("Graphics device lost, cleaning up scenegraph and releasing RHIs");

    for (auto it = m_windows.begin(); it != m_windows.end(); ++it) {
        if (!it->rhi) [[unlikely]]
            continue;

        if (!it->rhi->isDeviceLost()) [[likely]]
            continue;

        auto *wd = QQuickWindowPrivate::get(it.key());
        wd->cleanupNodesOnShutdown();

        if (it->rc) [[likely]]
            it->rc->invalidate();

        releaseSwapchain(it.key());
        it->rhiDeviceLost = true;

        if (it->ownRhi) [[likely]]
            QSGRhiSupport::instance()->destroyRhi(it->rhi, {});
        
        it->rhi = nullptr;
    }
}

void QSGGuiThreadRenderLoop::releaseSwapchain(QQuickWindow *window)
{
    auto *wd = QQuickWindowPrivate::get(window);
    
    delete std::exchange(wd->rpDescForSwapchain, nullptr);
    delete std::exchange(wd->swapchain, nullptr);
    delete std::exchange(wd->depthStencilForSwapchain, nullptr);
    
    wd->hasActiveSwapchain = false;
    wd->hasRenderableSwapchain = false;
    wd->swapchainJustBecameRenderable = false;
}

bool QSGGuiThreadRenderLoop::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() != QEvent::PlatformSurface) [[likely]]
        return QObject::eventFilter(watched, event);

    const auto surfaceEvent = static_cast<QPlatformSurfaceEvent *>(event);
    if (surfaceEvent->surfaceEventType() != QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed) [[likely]]
        return QObject::eventFilter(watched, event);

    if (auto *w = qobject_cast<QQuickWindow *>(watched)) [[likely]]
        releaseSwapchain(w);

    return QObject::eventFilter(watched, event);
}

bool QSGGuiThreadRenderLoop::ensureRhi(QQuickWindow *window, WindowData &data)
{
    auto *cd = QQuickWindowPrivate::get(window);
    auto *rhiSupport = QSGRhiSupport::instance();

    if (!data.rhi) [[unlikely]] {
        if (data.rhiDoomed) [[unlikely]] 
            return false;

        if (!offscreenSurface) [[unlikely]]
            offscreenSurface = rhiSupport->maybeCreateOffscreenSurface(window);

        auto [rhi, own] = rhiSupport->createRhi(window, offscreenSurface, swRastFallbackDueToSwapchainFailure);
        data.rhi = rhi;
        data.ownRhi = own;

        if (data.rhi) [[likely]] {
            data.rhiDeviceLost = false;
            data.rhi->makeThreadLocalNativeContextCurrent();
            data.sampleCount = rhiSupport->chooseSampleCountForWindowWithRhi(window, data.rhi);
            cd->rhi = data.rhi;

            QSGDefaultRenderContext::InitParams rcParams{
                .rhi = data.rhi,
                .sampleCount = data.sampleCount,
                .initialSurfacePixelSize = window->size() * window->effectiveDevicePixelRatio(),
                .maybeSurface = window
            };
            cd->context->initialize(&rcParams);
        } else {
            if (!data.rhiDeviceLost) [[likely]] {
                data.rhiDoomed = true;
                handleContextCreationFailure(window);
            }
            return false;
        }
    }

    if (data.rhi && !cd->swapchain) [[unlikely]] {
        cd->rhi = data.rhi;
        rhiSupport->prepareWindowForRhi(window);

        const auto fmt = window->requestedFormat();
        const auto alphaSize = fmt.alphaBufferSize();
        const auto swapInterval = fmt.swapInterval();
        
        QRhiSwapChain::Flags flags = QRhiSwapChain::UsedAsTransferSource;
        if (alphaSize > 0)
            flags |= QRhiSwapChain::SurfaceHasPreMulAlpha;
        if (swapInterval == 0)
            flags |= QRhiSwapChain::NoVSync;

        cd->swapchain = data.rhi->newSwapChain();
        static const bool depthEnabled = qEnvironmentVariableIsEmpty("QSG_NO_DEPTH_BUFFER");
        if (depthEnabled) [[likely]] {
            cd->depthStencilForSwapchain = data.rhi->newRenderBuffer(
                QRhiRenderBuffer::DepthStencil, {}, data.sampleCount, 
                QRhiRenderBuffer::UsedWithSwapChainOnly);
            cd->swapchain->setDepthStencil(cd->depthStencilForSwapchain);
        }
        cd->swapchain->setWindow(window);
        rhiSupport->applySwapChainFormat(cd->swapchain, window);
        cd->swapchain->setSampleCount(data.sampleCount);
        cd->swapchain->setFlags(flags);
        cd->rpDescForSwapchain = cd->swapchain->newCompatibleRenderPassDescriptor();
        cd->swapchain->setRenderPassDescriptor(cd->rpDescForSwapchain);
        window->installEventFilter(this);
    }

    if (!data.rc) [[unlikely]] {
        data.rc = cd->context;
        pendingRenderContexts.remove(data.rc);
    }

    return data.rhi != nullptr;
}

void QSGGuiThreadRenderLoop::renderWindow(QQuickWindow *window)
{
    auto winDataIt = m_windows.find(window);
    if (winDataIt == m_windows.end()) [[unlikely]] 
        return;

    WindowData &data = winDataIt.value();
    bool alsoSwap = data.updatePending;
    data.updatePending = false;

    auto *cd = QQuickWindowPrivate::get(window);
    if (!cd->isRenderable() || !cd->updatesEnabled) [[unlikely]] 
        return;

    if (!ensureRhi(window, data)) [[unlikely]] 
        return;

    cd->deliveryAgentPrivate()->flushFrameSynchronousEvents(window);
    if (!m_windows.contains(window)) [[unlikely]] 
        return;

    if (!cd->swapchain) [[unlikely]] 
        return;

    QSize effectiveOutputSize = cd->swapchain->surfacePixelSize();
    if (effectiveOutputSize.isEmpty()) [[unlikely]] 
        return;

    Q_TRACE_SCOPE(QSG_renderWindow);
    
    m_inPolish = true;
    cd->polishItems();
    m_inPolish = false;

    emit window->afterAnimating();

    const auto currentSize = cd->swapchain->currentPixelSize();
    if (currentSize != effectiveOutputSize || cd->swapchainJustBecameRenderable) [[unlikely]] {
        cd->hasActiveSwapchain = cd->swapchain->createOrResize();
        if (!cd->hasActiveSwapchain) [[unlikely]] {
            if (data.rhi->isDeviceLost()) [[unlikely]] { 
                handleDeviceLoss(); 
                return; 
            }
            else if (QSGRhiSupport::instance()->attemptReinitWithSwRastUponFail()) {
                swRastFallbackDueToSwapchainFailure = true;
                teardownGraphics();
                return;
            }
        }
        cd->swapchainJustBecameRenderable = false;
        cd->hasRenderableSwapchain = cd->hasActiveSwapchain;
    }

    emit window->beforeFrameBegin();
    
    if (data.rhi->beginFrame(cd->swapchain) != QRhi::FrameOpSuccess) [[unlikely]] {
        if (data.rhi->isDeviceLost()) [[unlikely]]
            handleDeviceLoss();
        emit window->afterFrameEnd();
        return;
    }

    data.rhi->makeThreadLocalNativeContextCurrent();
    cd->syncSceneGraph();
    data.rc->endSync();
    cd->renderSceneGraph();
    
    const bool needsPresent = alsoSwap && window->isVisible();
    QRhi::EndFrameFlags flags;
    if (!needsPresent)
        flags = QRhi::SkipPresent;
    
    if (data.rhi->endFrame(cd->swapchain, flags) != QRhi::FrameOpSuccess) [[unlikely]] {
        if (data.rhi->isDeviceLost()) [[unlikely]]
            handleDeviceLoss();
    }
    
    if (needsPresent) [[likely]]
        cd->fireFrameSwapped();

    emit window->afterFrameEnd();
    
    if (data.updatePending) [[unlikely]] 
        maybeUpdate(window);
}

void QSGGuiThreadRenderLoop::exposureChanged(QQuickWindow *window)
{
    auto *wd = QQuickWindowPrivate::get(window);

    if (!window->isExposed() || (wd->hasActiveSwapchain && wd->swapchain->surfacePixelSize().isEmpty())) [[unlikely]]
        wd->hasRenderableSwapchain = false;

    if (window->isExposed() && !wd->hasRenderableSwapchain && wd->hasActiveSwapchain
            && !wd->swapchain->surfacePixelSize().isEmpty()) [[likely]]
    {
        wd->hasRenderableSwapchain = true;
        wd->swapchainJustBecameRenderable = true;
    }

    auto winDataIt = m_windows.find(window);
    if (winDataIt != m_windows.end()) [[likely]] {
        if (window->isExposed() && (!winDataIt->rhi || !wd->hasActiveSwapchain || wd->hasRenderableSwapchain)) {
            winDataIt->updatePending = true;
            renderWindow(window);
        }
    }
}

QImage QSGGuiThreadRenderLoop::grab(QQuickWindow *window)
{
    auto winDataIt = m_windows.find(window);
    if (winDataIt == m_windows.end()) [[unlikely]]
        return QImage();

    if (!ensureRhi(window, *winDataIt)) [[unlikely]]
        return QImage();

    auto *cd = QQuickWindowPrivate::get(window);
    m_inPolish = true;
    cd->polishItems();
    m_inPolish = false;

    if (cd->rhi->beginFrame(cd->swapchain) != QRhi::FrameOpSuccess) [[unlikely]]
        return QImage();

    cd->rhi->makeThreadLocalNativeContextCurrent();
    cd->syncSceneGraph();
    cd->renderSceneGraph();
    
    QImage image = QSGRhiSupport::instance()->grabAndBlockInCurrentFrame(
        cd->rhi, cd->swapchain->currentFrameCommandBuffer());
    
    cd->rhi->endFrame(cd->swapchain, QRhi::SkipPresent);

    image.setDevicePixelRatio(window->effectiveDevicePixelRatio());
    return image;
}

void QSGGuiThreadRenderLoop::maybeUpdate(QQuickWindow *window)
{
    auto winDataIt = m_windows.find(window);
    if (winDataIt == m_windows.end()) [[unlikely]]
        return;

    auto *cd = QQuickWindowPrivate::get(window);
    if (!cd->isRenderable()) [[unlikely]]
        return;

    winDataIt->updatePending = true;

    if (m_inPolish) [[unlikely]]
        return;

    window->requestUpdate();
}

QSGContext *QSGGuiThreadRenderLoop::sceneGraphContext() const
{
    return sg;
}

QSGRenderContext *QSGGuiThreadRenderLoop::createRenderContext(QSGContext *sg) const
{
    QSGRenderContext *rc = sg->createRenderContext();
    pendingRenderContexts.insert(rc);
    return rc;
}

void QSGGuiThreadRenderLoop::releaseResources(QQuickWindow *w)
{
    QQuickWindowPrivate *d = QQuickWindowPrivate::get(w);
    emit d->context->releaseCachedResourcesRequested();
    if (d->renderer) [[likely]]
        d->renderer->releaseCachedResources();
#if QT_CONFIG(quick_shadereffect)
    QSGRhiShaderEffectNode::garbageCollectMaterialTypeCache(w);
#endif
}

void QSGGuiThreadRenderLoop::handleUpdateRequest(QQuickWindow *window)
{
    renderWindow(window);
}

#endif

QT_END_NAMESPACE

#include "qsgrenderloop.moc"
#include "moc_qsgrenderloop_p.cpp"
