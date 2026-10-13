/**
 * @file OpenGlImageWidget.h
 * @brief 为人脸预览控件提供 RGB 图像纹理上传和 OpenGL ES 绘制。
 */

#ifndef OPEN_GL_IMAGE_WIDGET_H
#define OPEN_GL_IMAGE_WIDGET_H

#include <QColor>
#include <QImage>
#include <QOpenGLFunctions>
#include <QOpenGLWidget>
#include <QRect>

class QOpenGLShaderProgram;

/** @brief 在 QOpenGLWidget 的 FBO 中上传并绘制一张 RGB/RGBA 纹理。 */
class OpenGlImageWidget : public QOpenGLWidget, protected QOpenGLFunctions {
public:
    explicit OpenGlImageWidget(QWidget *parent = nullptr);
    ~OpenGlImageWidget() override;

protected:
    /** @brief 设置下一次 paintGL 要上传的图像。 */
    void setOpenGlImage(const QImage &image);

    /** @brief 清除 CPU 侧图像和现有纹理内容标记。 */
    void clearOpenGlImage();

    /** @brief 清空当前 OpenGL FBO。 */
    void clearOpenGlSurface(const QColor &color = Qt::black);

    /** @brief 将当前图像纹理绘制到控件逻辑坐标中的目标矩形。 */
    bool drawOpenGlImage(const QRect &targetRect);

    /** @return 当前是否有可绘制图像。 */
    bool hasOpenGlImage() const;

    void initializeGL() override;

private:
    bool ensureShaderProgram();
    bool uploadPendingImage();
    void releaseGlResources();

    QImage textureImage_;
    QOpenGLShaderProgram *program_ = nullptr;
    GLuint texture_ = 0;
    GLuint vertexBuffer_ = 0;
    QSize allocatedTextureSize_;
    GLenum allocatedTextureFormat_ = 0;
    bool textureUploadPending_ = false;
};

#endif
