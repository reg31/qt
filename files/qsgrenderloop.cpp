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

QSGRenderLoop *QSGRenderLoop::s_instance = nullptr;

QSGRenderLoop::~QSGRenderLoop() {}

void QSGRenderLoop::cleanup()
{
    if (!s_instance) [[unlikely]] return;
    for (QQuickWindow *w : s_instance->windows()) {
        auto *wd = QQuickWindowPrivate::get(w);
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
    auto *cd = QQuickWindowPrivate::get(window);
    if (cd->rhi) [[likely]] cd->rhi->makeThreadLocalNativeContextCurrent();
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
        WindowData() : updatePending(false), rhiDeviceLost(false), rhiDoomed(false) {}
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
    if (!s_instance) [[unlikely]] {
        QSGRhiSupport::checkEnvQSgInfo();
        s_instance = QSGContext::createWindowManager();
#ifdef ENABLE_DEFAULT_BACKEND
        if (!s_instance) {
            auto *rhiSupport = QSGRhiSupport::instance();
            QSGRenderLoopType loopType = (rhiSupport->rhiBackend() != QRhi::OpenGLES2 || QGuiApplicationPrivate::platformIntegration()->hasCapability(QPlatformIntegration::ThreadedOpenGL)) 
                ? ThreadedRenderLoop : BasicRenderLoop;

            if (rhiSupport->rhiBackend() == QRhi::Null) loopType = BasicRenderLoop;
            if (qmlNoThreadedRenderer()) loopType = BasicRenderLoop;
            else if (qmlForceThreadedRenderer()) loopType = ThreadedRenderLoop;

            static const QByteArray envLoop = qgetenv("QSG_RENDER_LOOP");
            if (!envLoop.isEmpty()) {
                if (envLoop == "basic") loopType = BasicRenderLoop;
                else if (envLoop == "threaded") loopType = ThreadedRenderLoop;
            }

            if (loopType == ThreadedRenderLoop) s_instance = new QSGThreadedRenderLoop();
            else s_instance = new QSGGuiThreadRenderLoop();
        }
#endif
        qAddPostRoutine(QSGRenderLoop::cleanup);
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
    if (recurseGuard.contains(window)) [[unlikely]] return;
    recurseGuard.insert(window);
    QString translatedMessage, untranslatedMessage;
    QQuickWindowPrivate::rhiCreationFailureMessage(QSGRhiSupport::instance()->rhiBackendName(), &translatedMessage, &untranslatedMessage);
    const bool signalEmitted = QQuickWindowPrivate::get(window)->emitError(QQuickWindow::ContextNotAvailable, translatedMessage);
#if defined(Q_OS_WIN)
    if (!signalEmitted && !QLibraryInfo::isDebugBuild() && !GetConsoleWindow()) {
        MessageBox(0, (LPCTSTR)translatedMessage.utf16(), (LPCTSTR)(QCoreApplication::applicationName().utf16()), MB_OK | MB_ICONERROR);
    }
#endif
    if (!signalEmitted) qFatal("%s", qPrintable(untranslatedMessage));
    recurseGuard.remove(window);
}

#ifdef ENABLE_DEFAULT_BACKEND
QSGGuiThreadRenderLoop::QSGGuiThreadRenderLoop()
{
    if (qsg_useConsistentTiming()) [[unlikely]] QUnifiedTimer::instance(true)->setConsistentTiming(true);
    sg = QSGContext::createDefaultContext();
}

QSGGuiThreadRenderLoop::~QSGGuiThreadRenderLoop()
{
    qDeleteAll(pendingRenderContexts);
    delete sg;
}

void QSGGuiThreadRenderLoop::show(QQuickWindow *window)
{
    auto it = m_windows.find(window);
    if (it == m_windows.end()) [[unlikely]] {
        it = m_windows.insert(window, WindowData{});
        it.value().timeBetweenRenders.start();
    }
    maybeUpdate(window);
}

void QSGGuiThreadRenderLoop::hide(QQuickWindow *window)
{
    QQuickWindowPrivate::get(window)->fireAboutToStop();
    auto it = m_windows.find(window);
    if (it != m_windows.end()) [[likely]] it.value().updatePending = false;
}

void QSGGuiThreadRenderLoop::windowDestroyed(QQuickWindow *window)
{
    hide(window);
    WindowData data = m_windows.value(window, {});
    m_windows.remove(window);
    auto *d = QQuickWindowPrivate::get(window);
    if (data.rhi) [[likely]] data.rhi->makeThreadLocalNativeContextCurrent();
    if (d->swapchain) [[unlikely]] releaseSwapchain(window);
    d->cleanupNodesOnShutdown();
#if QT_CONFIG(quick_shadereffect)
    QSGRhiShaderEffectNode::resetMaterialTypeCache(window);
#endif
    if (data.rc) { data.rc->invalidate(); delete data.rc; }
    if (data.ownRhi) QSGRhiSupport::instance()->destroyRhi(data.rhi, d->graphicsConfig);
    d->rhi = nullptr;
    d->animationController.reset();
    if (m_windows.isEmpty()) [[unlikely]] { delete offscreenSurface; offscreenSurface = nullptr; }
}

void QSGGuiThreadRenderLoop::teardownGraphics()
{
    for (auto it = m_windows.begin(); it != m_windows.end(); ++it) {
        if (it->rhi) [[likely]] {
            QQuickWindowPrivate::get(it.key())->cleanupNodesOnShutdown();
            if (it->rc) it->rc->invalidate();
            releaseSwapchain(it.key());
            if (it->ownRhi) QSGRhiSupport::instance()->destroyRhi(it->rhi, {});
            it->rhi = nullptr;
        }
    }
}

void QSGGuiThreadRenderLoop::handleDeviceLoss()
{
    for (auto it = m_windows.begin(); it != m_windows.end(); ++it) {
        if (!it->rhi || !it->rhi->isDeviceLost()) [[likely]] continue;
        QQuickWindowPrivate::get(it.key())->cleanupNodesOnShutdown();
        if (it->rc) it->rc->invalidate();
        releaseSwapchain(it.key());
        it->rhiDeviceLost = true;
        if (it->ownRhi) QSGRhiSupport::instance()->destroyRhi(it->rhi, {});
        it->rhi = nullptr;
    }
}

void QSGGuiThreadRenderLoop::releaseSwapchain(QQuickWindow *window)
{
    auto *wd = QQuickWindowPrivate::get(window);
    delete std::exchange(wd->rpDescForSwapchain, nullptr);
    delete std::exchange(wd->swapchain, nullptr);
    delete std::exchange(wd->depthStencilForSwapchain, nullptr);
    wd->hasActiveSwapchain = wd->hasRenderableSwapchain = wd->swapchainJustBecameRenderable = false;
}

bool QSGGuiThreadRenderLoop::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::PlatformSurface) [[unlikely]] {
        if (static_cast<QPlatformSurfaceEvent *>(event)->surfaceEventType() == QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed) {
            if (auto *w = qobject_cast<QQuickWindow *>(watched)) releaseSwapchain(w);
        }
    }
    return QObject::eventFilter(watched, event);
}

bool QSGGuiThreadRenderLoop::ensureRhi(QQuickWindow *window, WindowData &data)
{
    auto *cd = QQuickWindowPrivate::get(window);
    auto *rhiSupport = QSGRhiSupport::instance();
    if (!data.rhi) [[unlikely]] {
        if (data.rhiDoomed) [[unlikely]] return false;
        if (!offscreenSurface) [[unlikely]] offscreenSurface = rhiSupport->maybeCreateOffscreenSurface(window);
        auto res = rhiSupport->createRhi(window, offscreenSurface, swRastFallbackDueToSwapchainFailure);
        data.rhi = res.rhi;
        data.ownRhi = res.own;
        if (data.rhi) [[likely]] {
            data.rhiDeviceLost = false;
            data.rhi->makeThreadLocalNativeContextCurrent();
            data.sampleCount = rhiSupport->chooseSampleCountForWindowWithRhi(window, data.rhi);
            cd->rhi = data.rhi;
            QSGDefaultRenderContext::InitParams p{
                .rhi = data.rhi, .sampleCount = data.sampleCount,
                .initialSurfacePixelSize = window->size() * window->effectiveDevicePixelRatio(),
                .maybeSurface = window
            };
            cd->context->initialize(&p);
        } else {
            if (!data.rhiDeviceLost) [[likely]] { data.rhiDoomed = true; handleContextCreationFailure(window); }
            return false;
        }
    }
    if (data.rhi && !cd->swapchain) [[unlikely]] {
        cd->rhi = data.rhi;
        rhiSupport->prepareWindowForRhi(window);
        const auto fmt = window->requestedFormat();
        auto flags = QRhiSwapChain::UsedAsTransferSource;
        if (fmt.alphaBufferSize() > 0) flags |= QRhiSwapChain::SurfaceHasPreMulAlpha;
        if (fmt.swapInterval() == 0) flags |= QRhiSwapChain::NoVSync;
        cd->swapchain = data.rhi->newSwapChain();
        if (!qEnvironmentVariableIsSet("QSG_NO_DEPTH_BUFFER")) [[likely]] {
            cd->depthStencilForSwapchain = data.rhi->newRenderBuffer(QRhiRenderBuffer::DepthStencil, {}, data.sampleCount, QRhiRenderBuffer::UsedWithSwapChainOnly);
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
    auto it = m_windows.find(window);
    if (it == m_windows.end()) [[unlikely]] return;
    auto &data = it.value();
    const bool alsoSwap = std::exchange(data.updatePending, false);
    auto *cd = QQuickWindowPrivate::get(window);
    if (!cd->isRenderable() || !cd->updatesEnabled) [[unlikely]] return;
    if (!ensureRhi(window, data)) [[unlikely]] return;
    cd->deliveryAgentPrivate()->flushFrameSynchronousEvents(window);
    if (!m_windows.contains(window)) [[unlikely]] return;
    if (cd->swapchain) [[likely]] {
        const auto size = cd->swapchain->surfacePixelSize();
        if (size.isEmpty()) [[unlikely]] return;
        if (cd->swapchain->currentPixelSize() != size || cd->swapchainJustBecameRenderable) [[unlikely]] {
            cd->hasActiveSwapchain = cd->swapchain->createOrResize();
            if (!cd->hasActiveSwapchain) [[unlikely]] {
                if (data.rhi->isDeviceLost()) { handleDeviceLoss(); return; }
                else if (QSGRhiSupport::instance()->attemptReinitWithSwRastUponFail()) {
                    swRastFallbackDueToSwapchainFailure = true;
                    teardownGraphics(); return;
                }
            }
            cd->swapchainJustBecameRenderable = false;
            cd->hasRenderableSwapchain = cd->hasActiveSwapchain;
        }
        emit window->beforeFrameBegin();
        if (data.rhi->beginFrame(cd->swapchain) != QRhi::FrameOpSuccess) [[unlikely]] {
            if (data.rhi->isDeviceLost()) handleDeviceLoss();
            emit window->afterFrameEnd(); return;
        }
    }
    m_inPolish = true;
    cd->polishItems();
    m_inPolish = false;
    emit window->afterAnimating();
    data.rhi->makeThreadLocalNativeContextCurrent();
    cd->syncSceneGraph();
    data.rc->endSync();
    cd->renderSceneGraph();
    if (cd->swapchain) [[likely]] {
        QRhi::EndFrameFlags f = (alsoSwap && window->isVisible()) ? QRhi::None : QRhi::SkipPresent;
        if (data.rhi->endFrame(cd->swapchain, f) != QRhi::FrameOpSuccess) [[unlikely]] { if (data.rhi->isDeviceLost()) handleDeviceLoss(); }
    }
    if (alsoSwap && window->isVisible()) cd->fireFrameSwapped();
    emit window->afterFrameEnd();
    if (data.updatePending) [[unlikely]] maybeUpdate(window);
}

void QSGGuiThreadRenderLoop::exposureChanged(QQuickWindow *window)
{
    auto *wd = QQuickWindowPrivate::get(window);
    if (!window->isExposed() || (wd->hasActiveSwapchain && wd->swapchain->surfacePixelSize().isEmpty())) [[unlikely]] wd->hasRenderableSwapchain = false;
    if (window->isExposed() && !wd->hasRenderableSwapchain && wd->hasActiveSwapchain && !wd->swapchain->surfacePixelSize().isEmpty()) [[likely]] {
        wd->hasRenderableSwapchain = true;
        wd->swapchainJustBecameRenderable = true;
    }
    auto it = m_windows.find(window);
    if (it != m_windows.end()) [[likely]] {
        if (window->isExposed() && (!it.value().rhi || !wd->hasActiveSwapchain || wd->hasRenderableSwapchain)) {
            it.value().updatePending = true;
            renderWindow(window);
        }
    }
}

QImage QSGGuiThreadRenderLoop::grab(QQuickWindow *window)
{
    auto it = m_windows.find(window);
    if (it == m_windows.end() || !ensureRhi(window, it.value())) [[unlikely]] return {};
    auto *cd = QQuickWindowPrivate::get(window);
    m_inPolish = true;
    cd->polishItems();
    m_inPolish = false;
    cd->rhi->beginFrame(cd->swapchain);
    cd->rhi->makeThreadLocalNativeContextCurrent();
    cd->syncSceneGraph();
    cd->renderSceneGraph();
    auto img = QSGRhiSupport::instance()->grabAndBlockInCurrentFrame(cd->rhi, cd->swapchain->currentFrameCommandBuffer());
    cd->rhi->endFrame(cd->swapchain, QRhi::SkipPresent);
    img.setDevicePixelRatio(window->effectiveDevicePixelRatio());
    return img;
}

void QSGGuiThreadRenderLoop::maybeUpdate(QQuickWindow *window)
{
    auto it = m_windows.find(window);
    if (it == m_windows.end() || !QQuickWindowPrivate::get(window)->isRenderable()) [[unlikely]] return;
    it.value().updatePending = true;
    if (m_inPolish) [[unlikely]] return;
    window->requestUpdate();
}

QSGContext *QSGGuiThreadRenderLoop::sceneGraphContext() const { return sg; }

QSGRenderContext *QSGGuiThreadRenderLoop::createRenderContext(QSGContext *sg) const
{
    auto *rc = sg->createRenderContext();
    pendingRenderContexts.insert(rc);
    return rc;
}

void QSGGuiThreadRenderLoop::releaseResources(QQuickWindow *w)
{
    auto *d = QQuickWindowPrivate::get(w);
    emit d->context->releaseCachedResourcesRequested();
    if (d->renderer) d->renderer->releaseCachedResources();
#if QT_CONFIG(quick_shadereffect)
    QSGRhiShaderEffectNode::garbageCollectMaterialTypeCache(w);
#endif
}

void QSGGuiThreadRenderLoop::handleUpdateRequest(QQuickWindow *window) { renderWindow(window); }
#endif

void QSGRhiSupport::applySettings()
{
    m_settingsApplied = true;
    QSGRhiSupport::checkEnvQSgInfo();
    if (m_requested.valid) [[unlikely]] {
        static const std::unordered_map<QSGRendererInterface::GraphicsApi, QRhi::Implementation> apiMap = {
            {QSGRendererInterface::OpenGL, QRhi::OpenGLES2}, {QSGRendererInterface::Direct3D11, QRhi::D3D11},
            {QSGRendererInterface::Direct3D12, QRhi::D3D12}, {QSGRendererInterface::Vulkan, QRhi::Vulkan},
            {QSGRendererInterface::Metal, QRhi::Metal}, {QSGRendererInterface::Null, QRhi::Null}
        };
        m_rhiBackend = apiMap.at(m_requested.api);
    } else [[likely]] {
        static const QByteArray envRhi = qgetenv("QSG_RHI_BACKEND").toLower();
        if (!envRhi.isEmpty()) {
            if (envRhi == "gl" || envRhi == "gles2" || envRhi == "opengl") m_rhiBackend = QRhi::OpenGLES2;
            else if (envRhi == "d3d11" || envRhi == "d3d") m_rhiBackend = QRhi::D3D11;
            else if (envRhi == "d3d12") m_rhiBackend = QRhi::D3D12;
            else if (envRhi == "vulkan") m_rhiBackend = QRhi::Vulkan;
            else if (envRhi == "metal") m_rhiBackend = QRhi::Metal;
            else if (envRhi == "null") m_rhiBackend = QRhi::Null;
        } else {
#if defined(Q_OS_WIN)
            m_rhiBackend = QRhi::D3D11;
#elif QT_CONFIG(metal)
            m_rhiBackend = QRhi::Metal;
#elif QT_CONFIG(opengl)
            m_rhiBackend = QRhi::OpenGLES2;
#else
            m_rhiBackend = QRhi::Vulkan;
#endif
            adjustToPlatformQuirks();
        }
    }
}

QSGRhiSupport::RhiCreateResult QSGRhiSupport::createRhi(QQuickWindow *window, QSurface *offscreenSurface, bool forcePreferSwRenderer)
{
    auto *wd = QQuickWindowPrivate::get(window);
    const auto *custom = QQuickGraphicsDevicePrivate::get(&wd->customDeviceObjects);
    if (custom->type == QQuickGraphicsDevicePrivate::Type::Rhi) [[unlikely]] {
        if (auto *rhi = custom->u.rhi) { preparePipelineCache(rhi, window); return { rhi, false }; }
    }
    const bool preferSoftware = wd->graphicsConfig.prefersSoftwareDevice() || forcePreferSwRenderer;
    const bool pipelineCacheSave = !wd->graphicsConfig.pipelineCacheSaveFile().isEmpty() || (wd->graphicsConfig.isAutomaticPipelineCacheEnabled() && !(window->flags() & (Qt::ToolTip | Qt::SplashScreen)));
    QRhi::Flags flags = QRhi::SuppressSmokeTestWarnings;
    if (wd->graphicsConfig.isDebugMarkersEnabled()) flags |= QRhi::EnableDebugMarkers;
    if (wd->graphicsConfig.timestampsEnabled()) flags |= QRhi::EnableTimestamps;
    if (preferSoftware) flags |= QRhi::PreferSoftwareRenderer;
    if (pipelineCacheSave) flags |= QRhi::EnablePipelineCacheDataSave;
    auto backend = rhiBackend();
    QRhi *rhi = nullptr;
    if (backend == QRhi::OpenGLES2) [[likely]] {
#if QT_CONFIG(opengl)
        QRhiGles2InitParams p{ .format = window->requestedFormat(), .fallbackSurface = offscreenSurface, .window = window };
        rhi = QRhi::create(backend, &p, flags, (custom->type == QQuickGraphicsDevicePrivate::Type::OpenGLContext) ? (QRhiNativeHandles*)new QRhiGles2NativeHandles{ .context = custom->u.context } : nullptr);
#endif
    } else if (backend == QRhi::Vulkan) {
#if QT_CONFIG(vulkan)
        prepareWindowForRhi(window);
        QRhiVulkanInitParams p{ .inst = window->vulkanInstance(), .window = window->handle() ? window : nullptr };
        rhi = QRhi::create(backend, &p, flags);
#endif
    } else if (backend == QRhi::D3D11 || backend == QRhi::D3D12) {
#ifdef Q_OS_WIN
        if (backend == QRhi::D3D11) { QRhiD3D11InitParams p{ .enableDebugLayer = wd->graphicsConfig.isDebugLayerEnabled() }; rhi = QRhi::create(backend, &p, flags); }
        else { QRhiD3D12InitParams p{ .enableDebugLayer = wd->graphicsConfig.isDebugLayerEnabled() }; rhi = QRhi::create(backend, &p, flags); }
#endif
    }
    if (rhi) [[likely]] preparePipelineCache(rhi, window);
    return { rhi, true };
}

void QSGRhiSupport::preparePipelineCache(QRhi *rhi, QQuickWindow *window)
{
    auto *wd = QQuickWindowPrivate::get(window);
    auto path = wd->graphicsConfig.pipelineCacheLoadFile();
    if (path.isEmpty() && wd->graphicsConfig.isAutomaticPipelineCacheEnabled()) [[likely]] {
        if (!(window->flags() & (Qt::ToolTip | Qt::SplashScreen))) path = automaticPipelineCacheFileName(rhi);
    }
    if (path.isEmpty()) [[unlikely]] return;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) [[unlikely]] return;
    const auto size = file.size();
    if (size <= 0) [[unlikely]] return;
    QByteArray buffer(size, Qt::Uninitialized);
    if (file.read(buffer.data(), size) == size) [[likely]] rhi->setPipelineCacheData(std::move(buffer));
}

void QSGThreadedRenderLoop::handleExposure(QQuickWindow *window)
{
    auto *w = windowFor(window);
    if (!w) [[unlikely]] {
        auto *wd = QQuickWindowPrivate::get(window);
        auto *rc = wd->context;
        pendingRenderContexts.remove(rc);
        m_windows.emplace_back(Window{
            .window = window, .thread = new QSGRenderThread(this, rc), .actualWindowFormat = window->format(),
            .timeBetweenPolishAndSyncs = []{ QElapsedTimer t; t.start(); return t; }(),
            .forceRenderPass = true
        });
        w = &m_windows.back();
    }
    if (!w->window->handle()) [[unlikely]] w->window->create();
    if (!w->thread->isRunning()) [[unlikely]] {
        w->thread->window = window;
        if (!w->thread->rhi) [[likely]] {
            auto *rs = QSGRhiSupport::instance();
            if (!w->thread->offscreenSurface) w->thread->offscreenSurface = rs->maybeCreateOffscreenSurface(window);
            w->thread->scProxyData = QRhi::updateSwapChainProxyData(rs->rhiBackend(), window);
            window->installEventFilter(this);
        }
        w->thread->active = true;
        w->thread->start();
    } else [[likely]] {
        std::lock_guard lock(w->thread->mutex);
        w->thread->postEvent(new WMWindowEvent(w->window, (QEvent::Type)WM_Exposed));
        w->thread->waitCondition.wait(&w->thread->mutex);
    }
    polishAndSync(w, true);
    startOrStopAnimationTimer();
}

void QSGRenderThread::syncAndRender()
{
    auto *cd = QQuickWindowPrivate::get(window);
    const uint update = std::exchange(pendingUpdate, 0);
    if (cd->swapchain && windowSize.isValid()) [[likely]] {
        cd->swapchain->setProxyData(scProxyData);
        const auto size = cd->swapchain->surfacePixelSize();
        if (size.isEmpty()) [[unlikely]] { if (update & SyncRequest) { std::lock_guard lock(mutex); waitCondition.wakeOne(); } return; }
        if (cd->swapchain->currentPixelSize() != size || cd->swapchainJustBecameRenderable) [[unlikely]] {
            cd->hasActiveSwapchain = cd->swapchain->createOrResize();
            cd->swapchainJustBecameRenderable = false;
            cd->hasRenderableSwapchain = true;
        }
        emit window->beforeFrameBegin();
        if (rhi->beginFrame(cd->swapchain) != QRhi::FrameOpSuccess) [[unlikely]] {
            if (update & SyncRequest) { std::lock_guard lock(mutex); waitCondition.wakeOne(); }
            emit window->afterFrameEnd(); return;
        }
    }
    if (update & SyncRequest) sync((update & ExposeRequest) == ExposeRequest);
    if (animatorDriver->isRunning()) [[unlikely]] { std::lock_guard lock(cd->animationController->mutex()); animatorDriver->advance(); }
    if (cd->renderer && cd->hasActiveSwapchain) [[likely]] {
        if (!(update & SyncRequest)) rhi->makeThreadLocalNativeContextCurrent();
        cd->renderSceneGraph();
        if (rhi->endFrame(cd->swapchain) != QRhi::FrameOpSuccess) [[unlikely]] handleDeviceLoss();
        cd->fireFrameSwapped();
    }
    if (cd->hasActiveSwapchain) emit window->afterFrameEnd();
    if (update & ExposeRequest) { std::lock_guard lock(mutex); waitCondition.wakeOne(); }
}

void QSGRenderThread::ensureRhi()
{
    if (!rhi) [[unlikely]] {
        if (rhiDoomed) return;
        auto res = QSGRhiSupport::instance()->createRhi(window, offscreenSurface, swRastFallbackDueToSwapchainFailure);
        rhi = res.rhi; ownRhi = res.own;
        if (!rhi) { rhiDoomed = true; return; }
        rhiSampleCount = QSGRhiSupport::instance()->chooseSampleCountForWindowWithRhi(window, rhi);
    }
    if (!sgrc->rhi() && windowSize.isValid()) [[unlikely]] {
        rhi->makeThreadLocalNativeContextCurrent();
        QSGDefaultRenderContext::InitParams p{ .rhi = rhi, .sampleCount = rhiSampleCount, .initialSurfacePixelSize = windowSize * dpr, .maybeSurface = window };
        sgrc->initialize(&p);
    }
    auto *cd = QQuickWindowPrivate::get(window);
    if (rhi && !cd->swapchain) [[unlikely]] {
        cd->rhi = rhi; cd->swapchain = rhi->newSwapChain();
        if (!qEnvironmentVariableIsSet("QSG_NO_DEPTH_BUFFER")) {
            cd->depthStencilForSwapchain = rhi->newRenderBuffer(QRhiRenderBuffer::DepthStencil, {}, rhiSampleCount, QRhiRenderBuffer::UsedWithSwapChainOnly);
            cd->swapchain->setDepthStencil(cd->depthStencilForSwapchain);
        }
        cd->swapchain->setWindow(window);
        QSGRhiSupport::instance()->applySwapChainFormat(cd->swapchain, window);
        cd->swapchain->setSampleCount(rhiSampleCount);
        cd->swapchain->setFlags(QRhiSwapChain::UsedAsTransferSource);
        cd->rpDescForSwapchain = cd->swapchain->newCompatibleRenderPassDescriptor();
        cd->swapchain->setRenderPassDescriptor(cd->rpDescForSwapchain);
    }
}

QT_END_NAMESPACE

#include "qsgrenderloop.moc"
#include "moc_qsgrenderloop_p.cpp"
