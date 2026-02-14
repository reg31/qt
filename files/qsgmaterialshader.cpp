// Copyright (C) 2019 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#include "qsgmaterial.h"
#include "qsgrenderer_p.h"
#include "qsgmaterialshader_p.h"
#include <QtCore/QFile>


#include <flat_map>
#include <mutex>
#include <shared_mutex>


QT_BEGIN_NAMESPACE

/*!
    \class QSGMaterialShader
    \brief The QSGMaterialShader class represents a graphics API independent shader program.
    \inmodule QtQuick
    \ingroup qtquick-scenegraph-materials
    \since 5.14

    QSGMaterialShader represents a combination of vertex and fragment shaders,
    data that define the graphics pipeline state changes, and logic that
    updates graphics resources, such as uniform buffers and textures.

    \note All classes with QSG prefix should be used solely on the scene graph's
    rendering thread. See \l {Scene Graph and Rendering} for more information.

    The QSGMaterial and QSGMaterialShader form a tight relationship. For one
    scene graph (including nested graphs), there is one unique
    QSGMaterialShader instance that encapsulates the shaders and other data
    the scene graph uses to render an object with that material. Each
    QSGGeometryNode can have a unique QSGMaterial that defines how the graphics
    pipeline must be configured while drawing the node. An instance of
    QSGMaterialShader is never created explicitly by the user, it will be
    created on demand by the scene graph through QSGMaterial::createShader().
    The scene graph creates an instance of QSGMaterialShader by calling the
    QSGMaterial::createShader() method, ensuring that there is only one
    instance of each shader implementation.

    In Qt 5, QSGMaterialShader was tied to OpenGL. It was built directly on
    QOpenGLShaderProgram and had functions like \c updateState() that could
    issue arbitrary OpenGL commands. This is no longer the case in Qt 6.
    QSGMaterialShader is not strictly data-oriented, meaning it provides data
    (shaders and the desired pipeline state changes) together with logic that
    updates data in a uniform buffer. Graphics API access is not provided.  This
    means that a QSGMaterialShader cannot make OpenGL, Vulkan, Metal, or Direct
    3D calls on its own. Together with the unified shader management, this
    allows a QSGMaterialShader to be written once, and be functional with any of
    the supported graphics APIs at run time.

    The shaders set by calling the protected setShaderFileName() function
    control what material does with the vertex data from the geometry, and how
    the fragments are shaded. A QSGMaterialShader will typically set a vertex
    and a fragment shader during construction. Changing the shaders afterwards
    may not lead to the desired effect and must be avoided.

    In Qt 6, the default approach is to ship \c{.qsb} files with the application,
    typically embedded via the resource system, and referenced when calling
    setShaderFileName(). The \c{.qsb} files are generated offline, or at latest
    at application build time, from Vulkan-style GLSL source code using the \c
    qsb tool from the Qt Shader Tools module.

    There are three virtuals that can be overridden. These provide the data, or
    the logic to generate the data, for uniform buffers, textures, and pipeline
    state changes.

    updateUniformData() is the function that is most commonly reimplemented in
    subclasses. This function is expected to update the contents of a
    QByteArray that will then be exposed to the shaders as a uniform buffer.
    Any QSGMaterialShader that has a uniform block in its vertex or fragment
    shader must reimplement updateUniformData().

    updateSampledImage() is relevant when the shader code samples textures. The
    function will be invoked for each sampler (or combined image sampler, in
    APIs where relevant), giving it the option to specify which QSGTexture
    should be exposed to the shader.

    The shader pipeline state changes are less often used. One use case is
    materials that wish to use a specific blend mode. The relevant function is
    updateGraphicsPipelineState(). This function is not called unless the
    QSGMaterialShader has opted in by setting the flag
    UpdatesGraphicsPipelineState. The task of the function is to update the
    GraphicsPipelineState struct instance that is passed to it with the
    desired changes. Currently only blending and culling-related features are
    available, other states cannot be controlled by materials.

    A minimal example, that also includes texture support, could be the
    following. Here we assume that Material is the QSGMaterial that creates an
    instance of Shader in its \l{QSGMaterial::createShader()}{createShader()},
    and that it holds a QSGTexture we want to sample in the fragment shader. The
    vertex shader relies only on the modelview-projection matrix.

    \code
        class Shader : public QSGMaterialShader
        {
        public:
            Shader()
            {
                setShaderFileName(VertexStage, QLatin1String(":/materialshader.vert.qsb"));
                setShaderFileName(FragmentStage, QLatin1String(":/materialshader.frag.qsb"));
            }

            bool updateUniformData(RenderState &state, QSGMaterial *, QSGMaterial *)
            {
                bool changed = false;
                QByteArray *buf = state.uniformData();
                if (state.isMatrixDirty()) {
                    const QMatrix4x4 m = state.combinedMatrix();
                    memcpy(buf->data(), m.constData(), 64);
                    changed = true;
                }
                return changed;
            }

            void updateSampledImage(RenderState &, int binding, QSGTexture **texture, QSGMaterial *newMaterial, QSGMaterial *)
            {
                Material *mat = static_cast<Material *>(newMaterial);
                if (binding == 1)
                    *texture = mat->texture();
            }
        };
    \endcode

    The Vulkan-style GLSL source code for the shaders could look like the
    following. These are expected to be preprocessed offline using the \c qsb
    tool, which generates the \c{.qsb} files referenced in the Shader()
    constructor.

    \badcode
        #version 440
        layout(location = 0) in vec4 aVertex;
        layout(location = 1) in vec2 aTexCoord;
        layout(location = 0) out vec2 vTexCoord;
        layout(std140, binding = 0) uniform buf {
            mat4 qt_Matrix;
        } ubuf;
        out gl_PerVertex { vec4 gl_Position; };
        void main() {
            gl_Position = ubuf.qt_Matrix * aVertex;
            vTexCoord = aTexCoord;
        }
    \endcode

    \badcode
        #version 440
        layout(location = 0) in vec2 vTexCoord;
        layout(location = 0) out vec4 fragColor;
        layout(binding = 1) uniform sampler2D srcTex;
        void main() {
            vec4 c = texture(srcTex, vTexCoord);
            fragColor = vec4(c.rgb * 0.5, 1.0);
        }
    \endcode

    \note All classes with QSG prefix should be used solely on the scene graph's
    rendering thread. See \l {Scene Graph and Rendering} for more information.

    \sa QSGMaterial, {Scene Graph - Custom Material}, {Scene Graph - Two Texture Providers}, {Scene Graph - Graph}
 */

/*!
    \enum QSGMaterialShader::Flag
    Flag values to indicate special material properties.

    \value UpdatesGraphicsPipelineState Setting this flag enables calling
    updateGraphicsPipelineState().
 */

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

QSGMaterialShader::QSGMaterialShader() : d_ptr(new QSGMaterialShaderPrivate(this)) {}
QSGMaterialShader::QSGMaterialShader(QSGMaterialShaderPrivate &dd) : d_ptr(&dd) {}
QSGMaterialShader::~QSGMaterialShader() {}

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
    QString suffix = (viewCount == 2) ? u".mv2qsb" : (viewCount == 3) ? u".mv3qsb" : (viewCount == 4) ? u".mv4qsb" : u"";
    d->shaderFileNames[toShaderStage(stage)] = filename + suffix;
}

QSGMaterialShader::Flags QSGMaterialShader::flags() const { return d_ptr->flags; }
void QSGMaterialShader::setFlag(Flags flags, bool on) { if (on) d_ptr->flags |= flags; else d_ptr->flags &= ~flags; }
void QSGMaterialShader::setFlags(Flags flags) { d_ptr->flags = flags; }

int QSGMaterialShader::combinedImageSamplerCount(int binding) const
{
    Q_D(const QSGMaterialShader);
    return (binding >= 0 && binding < d->MAX_SHADER_RESOURCE_BINDINGS) ? d->combinedImageSamplerCount[binding] : 0;
}

bool QSGMaterialShader::updateUniformData(RenderState &, QSGMaterial *, QSGMaterial *) { return false; }
void QSGMaterialShader::updateSampledImage(RenderState &, int, QSGTexture **, QSGMaterial *, QSGMaterial *) {}
bool QSGMaterialShader::updateGraphicsPipelineState(RenderState &, GraphicsPipelineState *, QSGMaterial *, QSGMaterial *) { return false; }

float QSGMaterialShader::RenderState::opacity() const { return float(static_cast<const QSGRenderer *>(m_data)->currentOpacity()); }
float QSGMaterialShader::RenderState::determinant() const { return float(static_cast<const QSGRenderer *>(m_data)->determinant()); }
QMatrix4x4 QSGMaterialShader::RenderState::combinedMatrix() const { return static_cast<const QSGRenderer *>(m_data)->currentCombinedMatrix(0); }
QMatrix4x4 QSGMaterialShader::RenderState::combinedMatrix(qsizetype index) const { return static_cast<const QSGRenderer *>(m_data)->currentCombinedMatrix(index); }
float QSGMaterialShader::RenderState::devicePixelRatio() const { return float(static_cast<const QSGRenderer *>(m_data)->devicePixelRatio()); }
QMatrix4x4 QSGMaterialShader::RenderState::modelViewMatrix() const { return static_cast<const QSGRenderer *>(m_data)->currentModelViewMatrix(); }
QMatrix4x4 QSGMaterialShader::RenderState::projectionMatrix() const { return static_cast<const QSGRenderer *>(m_data)->currentProjectionMatrix(0); }
QMatrix4x4 QSGMaterialShader::RenderState::projectionMatrix(qsizetype index) const { return static_cast<const QSGRenderer *>(m_data)->currentProjectionMatrix(index); }
qsizetype QSGMaterialShader::RenderState::projectionMatrixCount() const { return static_cast<const QSGRenderer *>(m_data)->projectionMatrixCount(); }
QRect QSGMaterialShader::RenderState::viewportRect() const { return static_cast<const QSGRenderer *>(m_data)->viewportRect(); }
QRect QSGMaterialShader::RenderState::deviceRect() const { return static_cast<const QSGRenderer *>(m_data)->deviceRect(); }
QByteArray *QSGMaterialShader::RenderState::uniformData() { return static_cast<const QSGRenderer *>(m_data)->currentUniformData(); }
QRhiResourceUpdateBatch *QSGMaterialShader::RenderState::resourceUpdateBatch() { return static_cast<const QSGRenderer *>(m_data)->currentResourceUpdateBatch(); }
QRhi *QSGMaterialShader::RenderState::rhi() { return static_cast<const QSGRenderer *>(m_data)->currentRhi(); }


QT_END_NAMESPACE
