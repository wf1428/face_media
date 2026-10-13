/**
 * @file displayrotationpage.h
 * @brief Weston 显示方向查询与切换页面。
 */

#ifndef DISPLAYROTATIONPAGE_H
#define DISPLAYROTATIONPAGE_H

#include <QWidget>
#include <QString>

class QComboBox;
class QLabel;
class QPushButton;
class QProcess;
class QShowEvent;

/** @brief 供多媒体和人脸后台共用的显示旋转设置页。 */
class DisplayRotationPage final : public QWidget
{
public:
    explicit DisplayRotationPage(QWidget *parent = nullptr);

protected:
    void showEvent(QShowEvent *event) override;

private:
    QString controlProgram() const;
    void refreshCurrentRotation();
    void requestRotation();
    void setCurrentRotation(int degrees);

    QComboBox *rotationCombo_ = nullptr;
    QLabel *currentRotationLabel_ = nullptr;
    QLabel *statusLabel_ = nullptr;
    QPushButton *applyButton_ = nullptr;
    QProcess *queryProcess_ = nullptr;
    bool queryStarted_ = false;
    int currentRotation_ = -1;
};

#endif // DISPLAYROTATIONPAGE_H
