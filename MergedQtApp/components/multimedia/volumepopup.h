/**
 * @file volumepopup.h
 * @brief 从右侧滑入的触摸音量调节面板。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#ifndef VOLUMEPOPUP_H
#define VOLUMEPOPUP_H

#include <QWidget>
#include <QDir>
#include <QFile>
#include <QSettings>

class QSlider;
class QLabel;
class AmpVolumeController;

/** @brief 从右侧滑入的触摸音量调节面板。 */
class VolumePanel : public QWidget
{
    Q_OBJECT
public:
    /** @brief 绑定音量控制器并创建滑块、标签和动画。 */
    explicit VolumePanel(AmpVolumeController *ctrl, QWidget *parent = nullptr);

    /** @brief 将外部音量同步到滑块和标签。 */
    void setCurrentVolume(int v);

    /** @brief 从右侧屏幕外滑入。 */
    void showSlideIn();

    /** @brief 滑出到右侧后隐藏。 */
    void hideSlideOut();

    /** @return 面板逻辑上已显示时返回 true。 */
    bool isShown() const { return m_shown; }

protected:
    /** @brief 消费面板内点击，避免事件穿透到底层播放器。 */
    void mousePressEvent(QMouseEvent *e) override;

signals:
    /** @brief 用户拖动滑块后输出音量和变更原因。 */
    void manualVolumeChanged(int volume, const QString &reason);

private:
    AmpVolumeController *m_ctrl = nullptr;
    QSlider *m_slider = nullptr;
    QLabel  *m_label  = nullptr;

    bool m_shown = false;
    int m_panelW ;     /**< 面板宽度，单位 px。 */
    int m_panelH ;     /**< 面板高度，单位 px。 */
    int m_panelY ;     /**< 垂直居中后的 y 坐标。 */
    int m_screenW ;
    int m_screenH ;

    /** @brief 根据屏幕尺寸计算显示和隐藏几何位置。 */
    void applyGeometry();
};

#endif // VOLUMEPOPUP_H
