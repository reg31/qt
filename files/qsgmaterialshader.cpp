// Copyright (C) 2019 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#include "qsgmaterial.h"
#include "qsgrenderer_p.h"
#include "qsgmaterialshader_p.h"
#include <QtCore/QFile>
#include <QFileInfo>
#include <QDateTime>
#include <flat_map>
#include <shared_mutex>

QT_BEGIN_NAMESPACE

static inline QRhiShaderResourceBinding::StageFlags toSrbStage(QShader::Stage stage)
{
    switch (stage) {
    case QShader::VertexStage: return QRhiShaderResourceBinding::VertexStage;
    case QShader::FragmentStage: return QRhiShaderResourceBinding::FragmentStage;
    default: return {};
    }
}

QShader QSGMaterialShaderPrivate::loadShader(const QString &filename)
{
    struct CacheEntry {
        QShader shader;
        QDateTime mtime;
    };
    static std::flat_map<QString, CacheEntry> diskCache;
    static std::shared_mutex cacheMutex;

    const QDateTime currentMtime = QFileInfo(filename).lastModified();

    {
        std::shared_lock lock(cacheMutex);
        if (auto it = diskCache.find(filename); it != diskCache.end()) [[likely]] {
            if (it->second.mtime == currentMtime) [[likely]]
                return it->second.shader;
        }
    }

    std::unique_lock lock(cacheMutex);
    auto it = diskCache.find(filename);
    if (it != diskCache.end() && it->second.mtime == currentMtime) [[likely]]
        return it->second.shader;

    QFile f(filename);
    if (f.open(QIODevice::ReadOnly)) [[likely]] {
        auto &entry = diskCache[filename];
        entry.shader = QShader::fromSerialized(f.readAll());
        entry.mtime = currentMtime;
        return entry.shader;
    }
    return {};
}

void QSGMaterialShaderPrivate::clearCachedRendererData()
{
    for (int i = 0; i < MAX_SHADER_RESOURCE_BINDINGS; ++i) {
        textureBindingTable[i].clear();
        samplerBindingTable[i].clear();
    }
}

void QSGMaterialShaderPrivate::prepare(QShader::Variant vertexShaderVariant)
{
    if (vertexShader && vertexShader->shaderVariant == vertexShaderVariant && shaderFileNames.empty()) [[likely]]
        return;

    ubufBinding = -1;
    ubufSize = 0;
    ubufStages = {};
    std::fill_n(combinedImageSamplerBindings, MAX_SHADER_RESOURCE_BINDINGS, QRhiShaderResourceBinding::StageFlags{});
    std::fill_n(combinedImageSamplerCount, MAX_SHADER_RESOURCE_BINDINGS, 0);
    vertexShader = fragmentShader = nullptr;
    masterUniformData.clear();

    clearCachedRendererData();

    static constexpr QShader::Stage stages[] = { QShader::VertexStage, QShader::FragmentStage };

    for (const auto stage : stages) {
        if (auto it = shaderFileNames.find(stage); it != shaderFileNames.end()) {
            if (const QShader s = loadShader(it.value()); s.isValid()) [[likely]]
                shaders[stage] = ShaderStageData(s);
            shaderFileNames.erase(it);
        }
    }

    if (auto it = shaders.find(QShader::VertexStage); it != shaders.end()) [[likely]] {
        auto &vsData = it.value();
        vsData.shaderVariant = vertexShaderVariant;
        vsData.vertexInputLocations.clear();
        vsData.qt_order_attrib_location = -1;

        const auto desc = vsData.shader.description();
        for (const auto &v : desc.inputVariables()) {
            if (vertexShaderVariant == QShader::BatchableVertexShader && v.name == "_qt_order") [[unlikely]]
                vsData.qt_order_attrib_location = v.location;
            else
                vsData.vertexInputLocations.append(v.location);
        }
    }

    for (auto it = shaders.begin(); it != shaders.end(); ++it) {
        auto &stageData = it.value();
        const auto desc = stageData.shader.description();
        const auto stageFlag = toSrbStage(stageData.shader.stage());

        for (const auto &ub : desc.uniformBlocks()) {
            if (ub.binding >= 0 && (ubufBinding == -1 || ubufBinding == ub.binding)) [[likely]] {
                ubufBinding = ub.binding;
                if (ub.size > ubufSize) {
                    ubufSize = ub.size;
                    masterUniformData.fill('\0', ubufSize);
                }
                ubufStages |= stageFlag;
            }
        }

        for (const auto &var : desc.combinedImageSamplers()) {
            if (var.binding >= 0 && var.binding < MAX_SHADER_RESOURCE_BINDINGS) [[likely]] {
                combinedImageSamplerBindings[var.binding] |= stageFlag;
                int count = 1;
                for (int dim : var.arrayDims) count *= dim;
                combinedImageSamplerCount[var.binding] = count;
            }
        }

        if (it.key() == QShader::VertexStage) vertexShader = &stageData;
        else if (it.key() == QShader::FragmentStage) fragmentShader = &stageData;
    }
}

QSGMaterialShader::QSGMaterialShader()
    : d_ptr(new QSGMaterialShaderPrivate(this))
{
}

QSGMaterialShader::QSGMaterialShader(QSGMaterialShaderPrivate &dd)
    : d_ptr(&dd)
{
}

QSGMaterialShader::~QSGMaterialShader()
{
}

static inline QShader::Stage toShaderStage(QSGMaterialShader::Stage stage)
{
    switch (stage) {
    case QSGMaterialShader::VertexStage: return QShader::VertexStage;
    case QSGMaterialShader::FragmentStage: return QShader::FragmentStage;
    default: Q_UNREACHABLE_RETURN(QShader::VertexStage);
    }
}

void QSGMaterialShader::setShader(Stage stage, const QShader &shader)
{
    Q_D(QSGMaterialShader);
    d->shaders[toShaderStage(stage)] = QSGMaterialShaderPrivate::ShaderStageData(shader);
}

void QSGMaterialShader::setShaderFileName(Stage stage, const QString &filename)
{
    Q_D(QSGMaterialShader);
    d->shaderFileNames[toShaderStage(stage)] = filename;
}

void QSGMaterialShader::setShaderFileName(Stage stage, const QString &filename, int viewCount)
{
    Q_D(QSGMaterialShader);
    QString suffix;
    switch (viewCount) {
        case 2: suffix = QStringLiteral(".mv2qsb"); break;
        case 3: suffix = QStringLiteral(".mv3qsb"); break;
        case 4: suffix = QStringLiteral(".mv4qsb"); break;
        default: break;
    }
    d->shaderFileNames[toShaderStage(stage)] = filename + suffix;
}

QSGMaterialShader::Flags QSGMaterialShader::flags() const
{
    Q_D(const QSGMaterialShader);
    return d->flags;
}

void QSGMaterialShader::setFlag(Flags flags, bool on)
{
    Q_D(QSGMaterialShader);
    if (on)
        d->flags |= flags;
    else
        d->flags &= ~flags;
}

void QSGMaterialShader::setFlags(Flags flags)
{
    Q_D(QSGMaterialShader);
    d->flags = flags;
}

int QSGMaterialShader::combinedImageSamplerCount(int binding) const
{
    Q_D(const QSGMaterialShader);
    return (binding >= 0 && binding < d->MAX_SHADER_RESOURCE_BINDINGS) ? d->combinedImageSamplerCount[binding] : 0;
}

bool QSGMaterialShader::updateUniformData(RenderState &, QSGMaterial *, QSGMaterial *)
{
    return false;
}

void QSGMaterialShader::updateSampledImage(RenderState &, int, QSGTexture **, QSGMaterial *, QSGMaterial *)
{
}

bool QSGMaterialShader::updateGraphicsPipelineState(RenderState &, GraphicsPipelineState *, QSGMaterial *, QSGMaterial *)
{
    return false;
}

float QSGMaterialShader::RenderState::opacity() const
{
    Q_ASSERT(m_data);
    return float(static_cast<const QSGRenderer *>(m_data)->currentOpacity());
}

float QSGMaterialShader::RenderState::determinant() const
{
    Q_ASSERT(m_data);
    return float(static_cast<const QSGRenderer *>(m_data)->determinant());
}

QMatrix4x4 QSGMaterialShader::RenderState::combinedMatrix() const
{
    Q_ASSERT(m_data);
    return static_cast<const QSGRenderer *>(m_data)->currentCombinedMatrix(0);
}

QMatrix4x4 QSGMaterialShader::RenderState::combinedMatrix(qsizetype index) const
{
    Q_ASSERT(m_data);
    return static_cast<const QSGRenderer *>(m_data)->currentCombinedMatrix(index);
}

float QSGMaterialShader::RenderState::devicePixelRatio() const
{
    Q_ASSERT(m_data);
    return float(static_cast<const QSGRenderer *>(m_data)->devicePixelRatio());
}

QMatrix4x4 QSGMaterialShader::RenderState::modelViewMatrix() const
{
    Q_ASSERT(m_data);
    return static_cast<const QSGRenderer *>(m_data)->currentModelViewMatrix();
}

QMatrix4x4 QSGMaterialShader::RenderState::projectionMatrix() const
{
    Q_ASSERT(m_data);
    return static_cast<const QSGRenderer *>(m_data)->currentProjectionMatrix(0);
}

QMatrix4x4 QSGMaterialShader::RenderState::projectionMatrix(qsizetype index) const
{
    Q_ASSERT(m_data);
    return static_cast<const QSGRenderer *>(m_data)->currentProjectionMatrix(index);
}

qsizetype QSGMaterialShader::RenderState::projectionMatrixCount() const
{
    Q_ASSERT(m_data);
    return static_cast<const QSGRenderer *>(m_data)->projectionMatrixCount();
}

QRect QSGMaterialShader::RenderState::viewportRect() const
{
    Q_ASSERT(m_data);
    return static_cast<const QSGRenderer *>(m_data)->viewportRect();
}

QRect QSGMaterialShader::RenderState::deviceRect() const
{
    Q_ASSERT(m_data);
    return static_cast<const QSGRenderer *>(m_data)->deviceRect();
}

QByteArray *QSGMaterialShader::RenderState::uniformData()
{
    Q_ASSERT(m_data);
    return static_cast<const QSGRenderer *>(m_data)->currentUniformData();
}

QRhiResourceUpdateBatch *QSGMaterialShader::RenderState::resourceUpdateBatch()
{
    Q_ASSERT(m_data);
    return static_cast<const QSGRenderer *>(m_data)->currentResourceUpdateBatch();
}

QRhi *QSGMaterialShader::RenderState::rhi()
{
    Q_ASSERT(m_data);
    return static_cast<const QSGRenderer *>(m_data)->currentRhi();
}

QT_END_NAMESPACE
