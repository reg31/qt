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


static inline QRect toBottomLeftRect(const QRect &topLeftRect, int windowHeight)
{
    return QRect(topLeftRect.x(), windowHeight - topLeftRect.bottomRight().y() - 1,
                 topLeftRect.width(), topLeftRect.height());
}

static QShader getShader(const QString &name)
{
    QFile f(name);
    return f.open(QIODevice::ReadOnly) ? QShader::fromSerialized(f.readAll()) : QShader();
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
    const float dpr = (float)window->devicePixelRatio();
    const QSize sz = window->size();
    const float iW = 2.0f / (sz.width() * dpr), iH = 2.0f / (sz.height() * dpr);
    const float ySign = invY ? -1.0f : 1.0f;
    const bool linear = (sz.width() * sourceDevicePixelRatio) > (sz.width() * dpr);

    auto computeUbo = [&](const QRectF &tRect, const QRectF &sRect, const QSize &texSz, bool flipS, int swz) {
        GPUData d = { {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1}, {0}, 1.0f, swz };
        const float tw = (float)tRect.width(), th = (float)tRect.height();
        d.mat4[0] = tw * iW * 0.5f;
        d.mat4[5] = ySign * th * iH * 0.5f;
        d.mat4[12] = (float)tRect.x() * iW + d.mat4[0] - 1.0f;
        d.mat4[13] = invY ? ((float)tRect.y() * iH + th * iH * 0.5f - 1.0f) : (1.0f - (float)tRect.y() * iH - th * iH * 0.5f);
        const float isW = 1.0f / texSz.width(), isH = 1.0f / texSz.height();
        d.mat3[0] = (float)sRect.width() * isW;
        d.mat3[5] = (float)sRect.height() * isH;
        d.mat3[8] = (float)sRect.x() * isW;
        d.mat3[9] = (float)sRect.y() * isH;
        if (!flipS) { d.mat3[5] = -d.mat3[5]; d.mat3[9] = 1.0f - d.mat3[9]; }
        return d;
    };

    int swz = (tFlags & QPlatformBackingStore::TextureSwizzle) ? (std::endian::native == std::endian::little ? 1 : 2) : 0;
    if (m_texture) {
        GPUData d = computeUbo({0, 0, sz.width() * dpr, sz.height() * dpr}, toBottomLeftRect(scaledRect({QPoint(), sz}, sourceDevicePixelRatio).translated(scaledOffset(offset, sFactor)), m_texture->pixelSize().height()), m_texture->pixelSize(), tFlags & QPlatformBackingStore::TextureFlip, swz);
        uBatch->updateDynamicBuffer(m_widgetQuadData.ubuf, 0, sizeof(GPUData), &d);
        if (linear) updatePerQuadData(&m_widgetQuadData, m_texture.get(), nullptr, NeedsLinearFiltering);
    }

    m_textureQuadData.resize(nT);
    for (int i = 0; i < nT; ++i) {
        const QRect cr = textures->clipRect(i);
        if (cr.isEmpty()) { m_textureQuadData[i].reset(); continue; }
        QRect gm = textures->geometry(i); gm.translate(-offset);
        if (auto *t = textures->texture(i)) {
            if (!m_textureQuadData[i].isValid()) m_textureQuadData[i] = createPerQuadData(t, textures->textureExtra(i));
            else updatePerQuadData(&m_textureQuadData[i], t, textures->textureExtra(i));
            GPUData d = computeUbo(scaledRect(gm & cr.translated(gm.topLeft()), dpr), scaledRect(toBottomLeftRect(cr, gm.height()), dpr), scaledRect(gm, dpr).size(), (textures->flags(i).testFlag(QPlatformTextureList::MirrorVertically) != invS), 0);
            uBatch->updateDynamicBuffer(m_textureQuadData[i].ubuf, 0, sizeof(GPUData), &d);
            if (linear) updatePerQuadData(&m_textureQuadData[i], t, textures->textureExtra(i), NeedsLinearFiltering);
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
