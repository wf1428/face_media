/**
 * @file command_dialog.h
 * @brief 设备调试与配置命令对话框。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#ifndef COMMAND_DIALOG_H
#define COMMAND_DIALOG_H

#include <QRegularExpression>
#include <QRandomGenerator>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSpacerItem>
#include <QPlainTextEdit>
#include <QDateTime>
#include <QProcess>
#include <QSettings>
#include <QCoreApplication>


#include "common/sql/dbstore.h"
#include "components/cursoroverlay/keyboard_dialog.h"
#include "ic_board/device_config_sync.h"
#include "ic_board/ic_event_bridge.h"
#include "platform/rk3566_platform.h"

class QLineEdit;
class QPushButton;
class QPlainTextEdit;

/**
 * @brief 设备调试与配置命令对话框。
 *
 * 将界面字段与本地配置数据库键映射，并把发送、检测、重启等操作转换为统一的
 * actionClicked/sendClicked 信号；实际设备操作由外部业务对象执行。
 */
class CommandDialog : public QDialog
{
    Q_OBJECT
public:
    /** @brief 创建配置卡片、日志区并从数据库加载初始值。 */
    explicit CommandDialog(QWidget *parent = nullptr);

    /** @brief 向日志框追加一行；调用方必须在 UI 线程调用或通过 invokeMethod 投递。 */
    void appendLog(const QString &line);
    /** @brief 清空调试日志框。 */
    void clearLog();

    /** @brief 按动作键触发与对应按钮相同的业务信号。 */
    void triggerAction(const QString &actionKey);

signals:
    /** @brief 请求外部发送指定配置字段和值。 */
    void sendClicked(const QString &fieldKey, const QString &value);
    /** @brief 请求外部执行生成、检测、重启或同步时间等动作。 */
    void actionClicked(const QString &actionKey);

protected:
    /** @brief 为输入框接入软键盘并处理对话框内的交互事件。 */
    bool eventFilter(QObject *obj, QEvent *event) override;


private:
    /** @brief 创建滚动区域中的全部配置卡片。 */
    QWidget* buildContent();
    /** @brief 创建一个带输入框和操作按钮的配置行。 */
    QWidget* makeRow(const QString &key,
                     const QString &labelText,
                     const QString &defaultValue,
                     const QString &btnText = "发送",
                     const QString &btnObjectName = "btnSend",
                     bool requiredMark = true);

    /** @brief 应用适配当前屏幕尺寸的控件样式。 */
    void applyStyle();

    /** @brief 初始化配置数据库并把持久化值填入界面。 */
    void initDbAndLoadUi();
    /** @brief 从数据库重新加载全部可编辑字段。 */
    void loadDbToUi();
    /** @brief 校验一个界面字段并写入对应数据库键。 */
    bool applyUiKeyToDb(const QString& uiKey, const QString& uiVal, QString* err);

    /** @brief 将界面字段键转换为数据库字段键。 */
    QString dbKeyForUiKey(const QString& uiKey) const;
    /** @brief 按数据库字段规则归一化输入值，失败时填写 err。 */
    QString normalizeValue(const QString& dbKey, const QString& v, QString* err) const;

    /** @brief 从配置读取对话框尺寸并限制在当前屏幕可用范围内。 */
    QSize loadWindowSizeFromConfig() const;
    /** @brief 创建带标题的配置分区卡片。 */
    QWidget* makeSectionCard(const QString& title, const QList<QWidget*>& rows);
    /** @brief 创建设备动作按钮卡片。 */
    QWidget* makeActionCard();
    /** @brief 创建对话框标题卡片。 */
    QWidget* makeHeaderCard();

private:
    QMap<QString, QLineEdit*> m_edits;  /**< 界面字段键到输入框的映射。 */

    /** 调试动作执行结果的显示区域。 */
    QPlainTextEdit *m_logEdit = nullptr;

    /** 配置数据库路径。 */
    QString m_dbPath = Rk3566Platform::databasePath();

    /** 小屏幕下使用紧凑间距和字号。 */
    bool m_compactLayout = false;


private slots:
    /** @brief 持久化字段后向外发送配置命令。 */
    void onSendClicked(const QString& key, const QString& val);
    /** @brief 转发动作按钮请求。 */
    void onActionClicked(const QString& action);

};

#endif // COMMAND_DIALOG_H
