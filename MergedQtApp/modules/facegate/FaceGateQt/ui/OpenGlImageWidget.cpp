/**
 * @file OpenGlImageWidget.cpp
 * @brief 人脸预览 RGB 图像的 OpenGL ES 纹理渲染实现。
 */

#include "OpenGlImageWidget.h"

#include <QDebug>
#include <QOpenGLContext>
#include <QOpenGLShaderProgram>
#include <QtMath>

/** @brief 创建不保留上一帧内容的局部 OpenGL 画布。 */
OpenGlImageWidget::OpenGlImageWidget(QWidget *parent)
    : QOpenGLWidget(parent)
{
    setUpdateBehavior(QOpenGLWidget::NoPartialUpdate);
}

/** @brief 在有效上下文中释放纹理、VBO 和着色器。 */
OpenGlImageWidget::~OpenGlImageWidget()
{
    if (context() && context()->isValid()) {
        makeCurrent();
        releaseGlResources();
        doneCurrent();
    } else {
        delete program_;
        program_ = nullptr;
    }
}

/** @brief 保存纹理源；已是紧密 RGB888 时不做颜色转换。 */
void OpenGlImageWidget::setOpenGlImage(const QImage &image)
{
    if (image.isNull()) {
        clearOpenGlImage();
        return;
    }

    if (image.format() == QImage::Format_RGB888
        && image.bytesPerLine() == image.width() * 3) {
        textureImage_ = image;
    } else if (image.format() == QImage::Format_RGBA8888
               && image.bytesPerLine() == image.width() * 4) {
        textureImage_ = image;
    } else {
        textureImage_ = image.convertToFormat(QImage::Format_RGBA8888);
    }
    textureUploadPending_ = true;
}

/** @brief 清除待显示图像。 */
void OpenGlImageWidget::clearOpenGlImage()
{
    textureImage_ = QImage();
    textureUploadPending_ = false;
}

/** @brief 初始化纹理、动态顶点缓冲和着色器。 */
void OpenGlImageWidget::initializeGL()
{
    initializeOpenGLFunctions();
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);

    glGenTextures(1, &texture_);
    glGenBuffers(1, &vertexBuffer_);
    ensureShaderProgram();

    const GLubyte *renderer = glGetString(GL_RENDERER);
    const GLubyte *version = glGetString(GL_VERSION);
    qInfo() << "[FACE-PREVIEW-GL] initialized"
            << "widget=" << metaObject()->className()
            << "renderer=" << (renderer ? reinterpret_cast<const char *>(renderer) : "<null>")
            << "version=" << (version ? reinterpret_cast<const char *>(version) : "<null>");
}

/** @brief 创建适用于 OpenGL ES 2.0 的单纹理着色器。 */
bool OpenGlImageWidget::ensureShaderProgram()
{
    if (program_) {
        return true;
    }

    auto *program = new QOpenGLShaderProgram;
    static const char *vertexShader =
        "attribute highp vec2 vertexIn;\n"
        "attribute highp vec2 textureIn;\n"
        "varying highp vec2 textureOut;\n"
        "void main(void)\n"
        "{\n"
        "    gl_Position = vec4(vertexIn, 0.0, 1.0);\n"
        "    textureOut = textureIn;\n"
        "}\n";
    static const char *fragmentShader =
        "precision mediump float;\n"
        "varying highp vec2 textureOut;\n"
        "uniform sampler2D imageTexture;\n"
        "void main(void)\n"
        "{\n"
        "    gl_FragColor = texture2D(imageTexture, textureOut);\n"
        "}\n";

    if (!program->addShaderFromSourceCode(QOpenGLShader::Vertex, vertexShader)
        || !program->addShaderFromSourceCode(QOpenGLShader::Fragment, fragmentShader)
        || !program->link()) {
        qWarning() << "[FACE-PREVIEW-GL] shader creation failed:" << program->log();
        delete program;
        return false;
    }
    program_ = program;
    return true;
}

/** @brief 把最新 RGB/RGBA 图像上传到复用纹理。 */
bool OpenGlImageWidget::uploadPendingImage()
{
    if (!textureUploadPending_) {
        return texture_ != 0 && !textureImage_.isNull();
    }
    if (texture_ == 0 || textureImage_.isNull()) {
        return false;
    }

    const bool rgb = textureImage_.format() == QImage::Format_RGB888;
    const GLenum format = rgb ? GL_RGB : GL_RGBA;
    const QSize imageSize = textureImage_.size();
    const bool allocate = allocatedTextureSize_ != imageSize
        || allocatedTextureFormat_ != format;

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    GLint previousUnpackAlignment = 4;
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &previousUnpackAlignment);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    if (allocate) {
        glTexImage2D(GL_TEXTURE_2D,
                     0,
                     format,
                     imageSize.width(),
                     imageSize.height(),
                     0,
                     format,
                     GL_UNSIGNED_BYTE,
                     textureImage_.constBits());
    } else {
        glTexSubImage2D(GL_TEXTURE_2D,
                        0,
                        0,
                        0,
                        imageSize.width(),
                        imageSize.height(),
                        format,
                        GL_UNSIGNED_BYTE,
                        textureImage_.constBits());
    }

    glPixelStorei(GL_UNPACK_ALIGNMENT, previousUnpackAlignment);
    const GLenum error = glGetError();
    glBindTexture(GL_TEXTURE_2D, 0);
    if (error != GL_NO_ERROR) {
        qWarning() << "[FACE-PREVIEW-GL] texture upload failed"
                   << "widget=" << metaObject()->className()
                   << "size=" << imageSize
                   << "glError=" << QStringLiteral("0x%1").arg(error, 0, 16);
        return false;
    }

    allocatedTextureSize_ = imageSize;
    allocatedTextureFormat_ = format;
    textureUploadPending_ = false;
    return true;
}

/** @brief 清空当前控件的 OpenGL FBO。 */
void OpenGlImageWidget::clearOpenGlSurface(const QColor &color)
{
    const qreal dpr = devicePixelRatioF();
    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, qMax(1, qRound(width() * dpr)), qMax(1, qRound(height() * dpr)));
    glClearColor(color.redF(), color.greenF(), color.blueF(), color.alphaF());
    glClear(GL_COLOR_BUFFER_BIT);
}

/** @brief 上传最新图像，并用一个翻转纹理坐标的矩形绘制到目标区域。 */
bool OpenGlImageWidget::drawOpenGlImage(const QRect &targetRect)
{
    if (targetRect.isEmpty() || !ensureShaderProgram() || vertexBuffer_ == 0
        || !uploadPendingImage()) {
        return false;
    }

    const GLfloat left = 2.0f * static_cast<GLfloat>(targetRect.left()) / qMax(1, width()) - 1.0f;
    const GLfloat right = 2.0f * static_cast<GLfloat>(targetRect.right() + 1) / qMax(1, width()) - 1.0f;
    const GLfloat top = 1.0f - 2.0f * static_cast<GLfloat>(targetRect.top()) / qMax(1, height());
    const GLfloat bottom = 1.0f - 2.0f * static_cast<GLfloat>(targetRect.bottom() + 1) / qMax(1, height());
    const GLfloat vertices[] = {
        left,  bottom, 0.0f, 1.0f,
        right, bottom, 1.0f, 1.0f,
        left,  top,    0.0f, 0.0f,
        right, top,    1.0f, 0.0f
    };

    const qreal dpr = devicePixelRatioF();
    glViewport(0, 0, qMax(1, qRound(width() * dpr)), qMax(1, qRound(height() * dpr)));
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture_);

    program_->bind();
    program_->setUniformValue("imageTexture", 0);
    const int vertexLocation = program_->attributeLocation("vertexIn");
    const int textureLocation = program_->attributeLocation("textureIn");
    if (vertexLocation < 0 || textureLocation < 0) {
        program_->release();
        glBindTexture(GL_TEXTURE_2D, 0);
        return false;
    }

    glBindBuffer(GL_ARRAY_BUFFER, vertexBuffer_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(vertexLocation);
    glEnableVertexAttribArray(textureLocation);
    glVertexAttribPointer(vertexLocation, 2, GL_FLOAT, GL_FALSE,
                          4 * sizeof(GLfloat), reinterpret_cast<const void *>(0));
    glVertexAttribPointer(textureLocation, 2, GL_FLOAT, GL_FALSE,
                          4 * sizeof(GLfloat), reinterpret_cast<const void *>(2 * sizeof(GLfloat)));
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray(vertexLocation);
    glDisableVertexAttribArray(textureLocation);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    program_->release();
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0);
    return glGetError() == GL_NO_ERROR;
}

/** @return 是否保存了可供纹理上传的图像。 */
bool OpenGlImageWidget::hasOpenGlImage() const
{
    return !textureImage_.isNull();
}

/** @brief 释放当前上下文拥有的 GL 资源。 */
void OpenGlImageWidget::releaseGlResources()
{
    if (vertexBuffer_ != 0) {
        glDeleteBuffers(1, &vertexBuffer_);
        vertexBuffer_ = 0;
    }
    if (texture_ != 0) {
        glDeleteTextures(1, &texture_);
        texture_ = 0;
    }
    delete program_;
    program_ = nullptr;
    allocatedTextureSize_ = QSize();
    allocatedTextureFormat_ = 0;
}
