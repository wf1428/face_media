/**
 * @file featuresdialog.h
 * @brief 使用 Qt Quick 呈现的设备功能开关页面。
 */

#ifndef FEATURESDIALOG_H
#define FEATURESDIALOG_H

#include <QDialog>
#include <QList>
#include <QVariantList>

/** @brief 功能开关和三种互斥协议模式的持久化值对象。 */
struct FeatureSettings
{
    bool offline_v1 = false;
    bool online_v1  = false;
    bool online_v2  = false;
    bool air_sys    = false;
    bool media_sys  = false;
    bool broad_sys  = false;
    bool tbk_sys    = false;
};

class QEvent;
class QCheckBox;
class QHideEvent;
class QQuickWidget;
class QShowEvent;
class QTimer;
class QWidget;

/**
 * @brief 功能页面的 C++ ViewModel 与模态宿主。
 *
 * QML 只处理显示和动画；协议互斥、配置读写和确认返回仍由 C++ 执行。
 */
class FeaturesDialog : public QDialog
{
    Q_OBJECT
    Q_PROPERTY(QVariantList featureItems READ featureItems NOTIFY featureItemsChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusTextChanged)

public:
    explicit FeaturesDialog(QWidget *parent = nullptr);
    ~FeaturesDialog() override;

    FeatureSettings getFeatureSettings() const;
    void setFeatureSettings(const FeatureSettings &settings);

    QVariantList featureItems() const;
    QString statusText() const;

    Q_INVOKABLE void setFeatureEnabled(int index, bool enabled);
    Q_INVOKABLE void applyAndClose();
    Q_INVOKABLE void cancelAndClose();

signals:
    void featureItemsChanged();
    void statusTextChanged();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    void centerInHost();
    void setStatusText(const QString &text);
    void syncFallbackControls();

    QQuickWidget *quickView_ = nullptr;
    QWidget *waylandBlocker_ = nullptr;
    QTimer *statusTimer_ = nullptr;
    QList<QCheckBox *> fallbackChecks_;
    bool embeddedForWayland_ = false;
    bool featureEnabled_[7] = {false, false, false, false, false, false, false};
    QString statusText_;
    QString previousModuleId_;
};

#endif // FEATURESDIALOG_H
