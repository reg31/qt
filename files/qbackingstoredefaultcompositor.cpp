// Copyright (C) 2021 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#include "qbackingstoredefaultcompositor_p.h"
#include <QtGui/private/qwindow_p.h>
#include <qpa/qplatformgraphicsbuffer.h>
#include <QtCore/qfile.h>

QT_BEGIN_NAMESPACE

using namespace Qt::StringLiterals;

struct alignas(16) GPUData {
    float mat4[16];
    float mat3[12];
    float opacity;
    int swizzle;
};

QBackingStoreDefaultCompositor::~QBackingStoreDefaultCompositor()
{
    reset();
}

void QBackingStoreDefaultCompositor::reset()
{
    m_rhi = nullptr;
    m_psNoBlend.reset();
    m_psBlend.reset();
    m_psPremulBlend.reset();
    m_samplerNearest.reset();
    m_samplerLinear.reset();
    m_vbuf.reset();
    m_texture.reset();
    m_widgetQuadData.reset();
    for (PerQuadData &d : m_textureQuadData)
        d.reset();
}

QRhiTexture *QBackingStoreDefaultCompositor::toTexture(const QPlatformBackingStore *backingStore,
                                                       QRhi *rhi,
                                                       QRhiResourceUpdateBatch *resourceUpdates,
                                                       const QRegion &dirtyRegion,
                                                       QPlatformBackingStore::TextureFlags *flags) const
{
    return toTexture(backingStore->toImage(), rhi, resourceUpdates, dirtyRegion, flags);
}

QRhiTexture *QBackingStoreDefaultCompositor::toTexture(const QImage &sourceImage,
                                                       QRhi *rhi,
                                                       QRhiResourceUpdateBatch *resourceUpdates,
                                                       const QRegion &dirtyRegion,
                                                       QPlatformBackingStore::TextureFlags *flags) const
{
    Q_ASSERT(rhi && resourceUpdates && flags);

    if (!m_rhi)
        m_rhi = rhi;
    else if (m_rhi != rhi)
        return nullptr;

    QImage image = sourceImage;
    if (image.size().isEmpty())
        return nullptr;

    bool needsConversion = false;
    *flags = {};

    switch (image.format()) {
    case QImage::Format_ARGB32_Premultiplied:
        *flags |= QPlatformBackingStore::TexturePremultiplied;
        Q_FALLTHROUGH();
    case QImage::Format_RGB32:
    case QImage::Format_ARGB32:
        *flags |= QPlatformBackingStore::TextureSwizzle;
        break;
    case QImage::Format_RGBA8888_Premultiplied:
        *flags |= QPlatformBackingStore::TexturePremultiplied;
        Q_FALLTHROUGH();
    case QImage::Format_RGBX8888:
    case QImage::Format_RGBA8888:
        break;
    default:
        needsConversion = true;
        break;
    }

    const bool resized = !m_texture || m_texture->pixelSize() != image.size();
    if (dirtyRegion.isEmpty() && !resized)
        return m_texture.get();

    if (needsConversion)
        image = image.convertToFormat(QImage::Format_RGBA8888);
    else
        image.detach();

    if (resized) {
        if (!m_texture)
            m_texture.reset(rhi->newTexture(QRhiTexture::RGBA8, image.size()));
        else
            m_texture->setPixelSize(image.size());
        m_texture->create();
        resourceUpdates->uploadTexture(m_texture.get(), image);
    } else {
        const QRect rect = dirtyRegion.boundingRect() & image.rect();
        QRhiTextureSubresourceUploadDescription subresDesc(image);
        subresDesc.setSourceTopLeft(rect.topLeft());
        subresDesc.setSourceSize(rect.size());
        subresDesc.setDestinationTopLeft(rect.topLeft());
        resourceUpdates->uploadTexture(m_texture.get(), QRhiTextureUploadDescription(QRhiTextureUploadEntry(0, 0, subresDesc)));
    }

    return m_texture.get();
}

static inline QRect scaledRect(const QRect &rect, qreal factor)
{
    return QRect(rect.topLeft() * factor, rect.size() * factor);
}

static inline QPoint scaledOffset(const QPoint &pt, qreal factor)
{
    return pt * factor;
}

static inline QRegion scaledRegion(const QRegion &region, qreal factor, const QPoint &offset)
{
    if (offset.isNull() && factor <= 1.0)
        return region;
    QVarLengthArray<QRect, 4> rects;
    rects.reserve(region.rectCount());
    for (const QRect &rect : region)
        rects.append(scaledRect(rect.translated(offset), factor));
    QRegion deviceRegion;
    deviceRegion.setRects(rects.constData(), rects.size());
    return deviceRegion;
}

static QMatrix4x4 targetTransform(const QRectF &target, const QRect &viewport, bool invertY)
{
    const qreal x_scale = target.width() / viewport.width();
    const qreal y_scale = target.height() / viewport.height();
    const QPointF relative_to_viewport = target.topLeft() - viewport.topLeft();
    const qreal x_translate = x_scale - 1.0 + ((relative_to_viewport.x() / viewport.width()) * 2.0);
    qreal y_translate = invertY ? (y_scale - 1.0 + ((relative_to_viewport.y() / viewport.height()) * 2.0))
                                : (-y_scale + 1.0 - ((relative_to_viewport.y() / viewport.height()) * 2.0));

    QMatrix4x4 matrix;
    matrix(0, 3) = x_translate;
    matrix(1, 3) = y_translate;
    matrix(0, 0) = x_scale;
    matrix(1, 1) = (invertY ? -1.0 : 1.0) * y_scale;
    return matrix;
}

enum class SourceTransformOrigin { BottomLeft, TopLeft };

static QMatrix3x3 sourceTransform(const QRectF &subTexture, const QSize &textureSize, SourceTransformOrigin origin)
{
    qreal x_scale = subTexture.width() / textureSize.width();
    qreal y_scale = subTexture.height() / textureSize.height();
    const QPointF topLeft = subTexture.topLeft();
    const qreal x_translate = topLeft.x() / textureSize.width();
    qreal y_translate = topLeft.y() / textureSize.height();

    if (origin == SourceTransformOrigin::TopLeft) {
        y_scale = -y_scale;
        y_translate = 1.0 - y_translate;
    }

    QMatrix3x3 matrix;
    matrix(0, 2) = x_translate;
    matrix(1, 2) = y_translate;
    matrix(0, 0) = x_scale;
    matrix(1, 1) = y_scale;
    return matrix;
}

static inline QRect toBottomLeftRect(const QRect &topLeftRect, int windowHeight)
{
    return QRect(topLeftRect.x(), windowHeight - topLeftRect.bottomRight().y() - 1,
                 topLeftRect.width(), topLeftRect.height());
}

static bool prepareDrawForRenderToTextureWidget(const QPlatformTextureList *textures, int idx, QWindow *window, const QRect &deviceWindowRect, const QPoint &offset, bool invertTargetY, bool invertSource, QMatrix4x4 *target, QMatrix3x3 *source)
{
    const QRect clipRect = textures->clipRect(idx);
    if (clipRect.isEmpty()) return false;
    QRect rectInWindow = textures->geometry(idx);
    rectInWindow.translate(-offset);
    const QRect clippedRectInWindow = rectInWindow & clipRect.translated(rectInWindow.topLeft());
    const QRect srcRect = toBottomLeftRect(clipRect, rectInWindow.height());
    const qreal dpr = window->devicePixelRatio();
    *target = targetTransform(scaledRect(clippedRectInWindow, dpr), deviceWindowRect, invertTargetY);
    *source = sourceTransform(scaledRect(srcRect, dpr), scaledRect(rectInWindow, dpr).size(), invertSource ? SourceTransformOrigin::TopLeft : SourceTransformOrigin::BottomLeft);
    return true;
}

static QShader getShader(const QString &name)
{
    QFile f(name);
    return f.open(QIODevice::ReadOnly) ? QShader::fromSerialized(f.readAll()) : QShader();
}

static void updateMatrix3x3(QRhiResourceUpdateBatch *resourceUpdates, QRhiBuffer *ubuf, const QMatrix3x3 &m)
{
    float f[12];
    const float *src = static_cast<const float *>(m.constData());
    memcpy(f, src, 3 * sizeof(float));
    memcpy(f + 4, src + 3, 3 * sizeof(float));
    memcpy(f + 8, src + 6, 3 * sizeof(float));
    resourceUpdates->updateDynamicBuffer(ubuf, 64, 48, f);
}

static QRhiGraphicsPipeline *createGraphicsPipeline(QRhi *rhi, QRhiShaderResourceBindings *srb, QRhiRenderPassDescriptor *rpDesc, int blendMode)
{
    QRhiGraphicsPipeline *ps = rhi->newGraphicsPipeline();
    if (blendMode > 0) {
        QRhiGraphicsPipeline::TargetBlend blend;
        blend.enable = true;
        blend.srcColor = (blendMode == 2) ? QRhiGraphicsPipeline::One : QRhiGraphicsPipeline::SrcAlpha;
        blend.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
        blend.srcAlpha = QRhiGraphicsPipeline::One;
        blend.dstAlpha = QRhiGraphicsPipeline::One;
        ps->setTargetBlends({ blend });
    }
    ps->setShaderStages({
        { QRhiShaderStage::Vertex, getShader(":/qt-project.org/gui/painting/shaders/backingstorecompose.vert.qsb"_L1) },
        { QRhiShaderStage::Fragment, getShader(":/qt-project.org/gui/painting/shaders/backingstorecompose.frag.qsb"_L1) }
    });
    QRhiVertexInputLayout inputLayout;
    inputLayout.setBindings({ { 5 * sizeof(float) } });
    inputLayout.setAttributes({ { 0, 0, QRhiVertexInputAttribute::Float3, 0 },
                                { 0, 1, QRhiVertexInputAttribute::Float2, quint32(3 * sizeof(float)) } });
    ps->setVertexInputLayout(inputLayout);
    ps->setShaderResourceBindings(srb);
    ps->setRenderPassDescriptor(rpDesc);
    if (!ps->create()) { delete ps; return nullptr; }
    return ps;
}

static const int UBUF_SIZE = 120;

void QBackingStoreDefaultCompositor::updateUniforms(PerQuadData *d, QRhiResourceUpdateBatch *resourceUpdates, const QMatrix4x4 &target, const QMatrix3x3 &source, UpdateUniformOptions options)
{
    resourceUpdates->updateDynamicBuffer(d->ubuf, 0, 64, target.constData());
    updateMatrix3x3(resourceUpdates, d->ubuf, source);
    float opacity = 1.0f;
    resourceUpdates->updateDynamicBuffer(d->ubuf, 112, 4, &opacity);
    qint32 textureSwizzle = options;
    resourceUpdates->updateDynamicBuffer(d->ubuf, 116, 4, &textureSwizzle);
}

QBackingStoreDefaultCompositor::PerQuadData QBackingStoreDefaultCompositor::createPerQuadData(QRhiTexture *texture, QRhiTexture *textureExtra)
{
    PerQuadData d;
    d.ubuf = m_rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, UBUF_SIZE);
    d.ubuf->create();
    d.srb = m_rhi->newShaderResourceBindings();
    d.srb->setBindings({
        QRhiShaderResourceBinding::uniformBuffer(0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage, d.ubuf, 0, UBUF_SIZE),
        QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage, texture, m_samplerNearest.get())
    });
    d.srb->create();
    d.lastUsedTexture = texture;
    if (textureExtra) {
        d.srbExtra = m_rhi->newShaderResourceBindings();
        d.srbExtra->setBindings({
            QRhiShaderResourceBinding::uniformBuffer(0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage, d.ubuf, 0, UBUF_SIZE),
            QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage, textureExtra, m_samplerNearest.get())
        });
        d.srbExtra->create();
    }
    d.lastUsedTextureExtra = textureExtra;
    return d;
}

void QBackingStoreDefaultCompositor::updatePerQuadData(PerQuadData *d, QRhiTexture *texture, QRhiTexture *textureExtra, UpdateQuadDataOptions options)
{
    const QRhiSampler::Filter filter = options.testFlag(NeedsLinearFiltering) ? QRhiSampler::Linear : QRhiSampler::Nearest;
    if ((d->lastUsedTexture == texture && d->lastUsedFilter == filter) || !d->srb)
        return;

    QRhiSampler *sampler = filter == QRhiSampler::Linear ? m_samplerLinear.get() : m_samplerNearest.get();
    d->srb->setBindings({
        QRhiShaderResourceBinding::uniformBuffer(0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage, d->ubuf, 0, UBUF_SIZE),
        QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage, texture, sampler)
    });
    d->srb->updateResources(QRhiShaderResourceBindings::BindingsAreSorted);
    d->lastUsedTexture = texture;
    d->lastUsedFilter = filter;
    if (textureExtra) {
        d->srbExtra->setBindings({
            QRhiShaderResourceBinding::uniformBuffer(0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage, d->ubuf, 0, UBUF_SIZE),
            QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage, textureExtra, sampler)
        });
        d->srbExtra->updateResources(QRhiShaderResourceBindings::BindingsAreSorted);
        d->lastUsedTextureExtra = textureExtra;
    }
}

void QBackingStoreDefaultCompositor::ensureResources(QRhiResourceUpdateBatch *resourceUpdates, QRhiRenderPassDescriptor *rpDesc)
{
    if (!m_vbuf) {
        static const float vertexData[] = { -1,-1,0,0,0, -1,1,0,0,1, 1,-1,0,1,0, -1,1,0,0,1, 1,-1,0,1,0, 1,1,0,1,1 };
        m_vbuf.reset(m_rhi->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer, sizeof(vertexData)));
        if (m_vbuf->create()) resourceUpdates->uploadStaticBuffer(m_vbuf.get(), vertexData);
    }
    if (!m_samplerNearest) {
        m_samplerNearest.reset(m_rhi->newSampler(QRhiSampler::Nearest, QRhiSampler::Nearest, QRhiSampler::None, QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge));
        m_samplerNearest->create();
    }
    if (!m_samplerLinear) {
        m_samplerLinear.reset(m_rhi->newSampler(QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::None, QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge));
        m_samplerLinear->create();
    }
    if (!m_widgetQuadData.isValid())
        m_widgetQuadData = createPerQuadData(m_texture.get());
    if (!m_psNoBlend) m_psNoBlend.reset(createGraphicsPipeline(m_rhi, m_widgetQuadData.srb, rpDesc, 0));
    if (!m_psBlend) m_psBlend.reset(createGraphicsPipeline(m_rhi, m_widgetQuadData.srb, rpDesc, 1));
    if (!m_psPremulBlend) m_psPremulBlend.reset(createGraphicsPipeline(m_rhi, m_widgetQuadData.srb, rpDesc, 2));
}

QPlatformBackingStore::FlushResult QBackingStoreDefaultCompositor::flush(QPlatformBackingStore *backingStore, QRhi *rhi, QRhiSwapChain *swapchain, QWindow *window, qreal sourceDevicePixelRatio, const QRegion &region, const QPoint &offset, QPlatformTextureList *textures, bool translucentBackground, qreal sourceTransformFactor)
{
    if (!rhi || !swapchain) return QPlatformBackingStore::FlushFailed;
    if (!m_rhi) m_rhi = rhi; else if (m_rhi != rhi) return QPlatformBackingStore::FlushFailed;

    auto *wp = qt_window_private(window);
    if (!wp->receivedExpose) return QPlatformBackingStore::FlushSuccess;
    wp->lastComposeTime.start();

    if (swapchain->currentPixelSize() != swapchain->surfacePixelSize()) swapchain->createOrResize();
    auto fOp = rhi->beginFrame(swapchain);
    if (fOp == QRhi::FrameOpSwapChainOutOfDate) { if (!swapchain->createOrResize()) return QPlatformBackingStore::FlushFailed; fOp = rhi->beginFrame(swapchain); }
    if (fOp != QRhi::FrameOpSuccess) return fOp == QRhi::FrameOpDeviceLost ? QPlatformBackingStore::FlushFailedDueToLostDevice : QPlatformBackingStore::FlushFailed;

    auto *uBatch = rhi->nextResourceUpdateBatch();
    QPlatformBackingStore::TextureFlags tFlags;
    const qreal sFactor = sourceTransformFactor ? sourceTransformFactor : sourceDevicePixelRatio;

    if (auto *gb = backingStore->graphicsBuffer(); gb && gb->lock(QPlatformGraphicsBuffer::SWReadAccess)) {
        toTexture(QImage(gb->data(), gb->size().width(), gb->size().height(), gb->bytesPerLine(), QImage::toImageFormat(gb->format())), rhi, uBatch, scaledRegion(region, sFactor, offset), &tFlags);
        if (gb->origin() == QPlatformGraphicsBuffer::OriginBottomLeft) tFlags |= QPlatformBackingStore::TextureFlip;
        gb->unlock();
    } else toTexture(backingStore, rhi, uBatch, scaledRegion(region, sFactor, offset), &tFlags);

    const int nT = textures ? textures->count() : 0;
    if (!m_texture && nT == 0) { rhi->endFrame(swapchain); return QPlatformBackingStore::FlushSuccess; }

    ensureResources(uBatch, swapchain->renderPassDescriptor());
    const bool invY = !rhi->isYUpInNDC(), invS = !rhi->isYUpInFramebuffer();
    const qreal dpr = window->devicePixelRatio();
    const QRect devWinRect(QPoint(0, 0), window->size() * dpr);

    if (m_texture) {
        QMatrix4x4 target = targetTransform(QRectF(devWinRect), devWinRect, invY);
        QMatrix3x3 source = sourceTransform(scaledRect(toBottomLeftRect(scaledRect({QPoint(), window->size()}, sourceDevicePixelRatio).translated(scaledOffset(offset, sFactor)), m_texture->pixelSize().height()), 1.0), m_texture->pixelSize(), (tFlags & QPlatformBackingStore::TextureFlip) ? SourceTransformOrigin::TopLeft : SourceTransformOrigin::BottomLeft);
        updateUniforms(&m_widgetQuadData, uBatch, target, source, (tFlags & QPlatformBackingStore::TextureSwizzle) ? (std::endian::native == std::endian::little ? SwizzleBGR : SwizzleRGB) : NoSwizzle);
        if ((window->size().width() * sourceDevicePixelRatio) > devWinRect.width()) updatePerQuadData(&m_widgetQuadData, m_texture.get(), nullptr, NeedsLinearFiltering);
    }

    m_textureQuadData.resize(nT);
    for (int i = 0; i < nT; ++i) {
        QMatrix4x4 target; QMatrix3x3 source;
        if (prepareDrawForRenderToTextureWidget(textures, i, window, devWinRect, offset, invY, invS, &target, &source)) {
            if (!m_textureQuadData[i].isValid()) m_textureQuadData[i] = createPerQuadData(textures->texture(i), textures->textureExtra(i));
            else updatePerQuadData(&m_textureQuadData[i], textures->texture(i), textures->textureExtra(i));
            updateUniforms(&m_textureQuadData[i], uBatch, target, source, NoSwizzle);
        } else m_textureQuadData[i].reset();
    }

    auto *cb = swapchain->currentFrameCommandBuffer();
    cb->resourceUpdate(uBatch);
    auto drawPass = [&](QRhiRenderTarget *rt) {
        cb->beginPass(rt, translucentBackground ? Qt::transparent : Qt::black, { 1.0f, 0 });
        cb->setViewport({ 0, 0, (float)rt->pixelSize().width(), (float)rt->pixelSize().height() });
        QRhiCommandBuffer::VertexInput vbi(m_vbuf.get(), 0); cb->setVertexInput(0, 1, &vbi);
        cb->setGraphicsPipeline(m_psNoBlend.get());
        for (int i = 0; i < nT; ++i) {
            if (!textures->flags(i).testFlag(QPlatformTextureList::StacksOnTop) && m_textureQuadData[i].isValid()) {
                cb->setShaderResources((rt == swapchain->currentFrameRenderTarget(QRhiSwapChain::RightBuffer) && m_textureQuadData[i].srbExtra) ? m_textureQuadData[i].srbExtra : m_textureQuadData[i].srb);
                cb->draw(6);
            }
        }
        cb->setGraphicsPipeline((tFlags & QPlatformBackingStore::TexturePremultiplied) ? m_psPremulBlend.get() : m_psBlend.get());
        if (m_texture) { cb->setShaderResources(m_widgetQuadData.srb); cb->draw(6); }
        for (int i = 0; i < nT; ++i) {
            const auto f = textures->flags(i);
            if (f.testFlag(QPlatformTextureList::StacksOnTop) && m_textureQuadData[i].isValid()) {
                cb->setGraphicsPipeline(f.testFlag(QPlatformTextureList::NeedsPremultipliedAlphaBlending) ? m_psPremulBlend.get() : m_psBlend.get());
                cb->setShaderResources((rt == swapchain->currentFrameRenderTarget(QRhiSwapChain::RightBuffer) && m_textureQuadData[i].srbExtra) ? m_textureQuadData[i].srbExtra : m_textureQuadData[i].srb);
                cb->draw(6);
            }
        }
        cb->endPass();
    };

    if (swapchain->window()->format().stereo()) {
        drawPass(swapchain->currentFrameRenderTarget(QRhiSwapChain::LeftBuffer));
        drawPass(swapchain->currentFrameRenderTarget(QRhiSwapChain::RightBuffer));
    } else drawPass(swapchain->currentFrameRenderTarget());

    rhi->endFrame(swapchain);
    return QPlatformBackingStore::FlushSuccess;
}

QT_END_NAMESPACE
