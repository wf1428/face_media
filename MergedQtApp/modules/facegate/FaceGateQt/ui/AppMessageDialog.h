/**
 * @file AppMessageDialog.h
 * @brief 实现项目统一消息和确认弹窗，避免依赖系统 QMessageBox。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#ifndef APPMESSAGEDIALOG_H
#define APPMESSAGEDIALOG_H

#include <QDialog>
#include <QString>
#include <QStringList>

class QLabel;
class QPushButton;

/**
 * @brief 项目统一消息弹窗，替代系统 QMessageBox。
 *
 * 风格参考 shadcn/ui 的 Dialog / AlertDialog：深色卡片、圆角边框、状态图标、触摸友好按钮。
 * 处理提示、确认和直接按钮选择，不接管输入框和虚拟键盘逻辑。
 */
class AppMessageDialog : public QDialog
{
    Q_OBJECT
public:
    /** @brief 消息弹窗的视觉和语义类型。 */
    enum class Type {
        Info,
        Success,
        Warning,
        Error,
        Question
    };

    /** @brief 构造提示、确认或直接选项弹窗并按类型建立界面。 */
    explicit AppMessageDialog(Type type,
                              const QString &title,
                              const QString &message,
                              QWidget *parent = nullptr,
                              bool confirmMode = false,
                              const QString &confirmText = QString(),
                              const QString &cancelText = QString(),
                              const QStringList &choiceTexts = QStringList());

    /** @brief 显示阻塞式信息提示。 */
    static void information(QWidget *parent, const QString &title, const QString &message);

    /** @brief 显示阻塞式成功提示。 */
    static void success(QWidget *parent, const QString &title, const QString &message);

    /** @brief 显示阻塞式警告提示。 */
    static void warning(QWidget *parent, const QString &title, const QString &message);

    /** @brief 显示阻塞式错误提示。 */
    static void error(QWidget *parent, const QString &title, const QString &message);

    /** @return 用户选择确认时返回 true。 */
    static bool confirm(QWidget *parent,
                        const QString &title,
                        const QString &message,
                        const QString &confirmText = QStringLiteral("确定"),
                        const QString &cancelText = QStringLiteral("取消"));

    /** @return 用户选择的选项序号；关闭或取消时返回 -1。 */
    static int choose(QWidget *parent,
                      const QString &title,
                      const QString &message,
                      const QStringList &choices,
                      const QString &cancelText = QStringLiteral("取消"));

protected:
    /** @brief 显示时稳定文本布局并居中到父窗口。 */
    void showEvent(QShowEvent *event) override;

private:
    /** @return 指定类型对应的资源图标路径。 */
    static QString iconPath(Type type);

    /** @return 指定类型的样式名称。 */
    static QString typeName(Type type);

    /** @return 指定类型默认确认按钮文本。 */
    static QString defaultAcceptText(Type type);

    /** @brief 创建标题、消息和触摸友好按钮。 */
    void buildUi(const QString &title, const QString &message);

    /** @brief 加载项目统一弹窗样式表。 */
    void loadStyleSheet();

    /** @brief 将弹窗移动到父窗口或屏幕中心。 */
    void centerOnParent();

    /** @brief 在字体与窗口几何稳定后重新计算文本换行。 */
    void stabilizeTextLayout();

private:
    Type type_ = Type::Info; /**< 当前消息类型。 */
    bool confirmMode_ = false; /**< 是否显示确认和取消两个按钮。 */
    QString confirmText_;      /**< 自定义确认文本。 */
    QString cancelText_;       /**< 自定义取消文本。 */
    QStringList choiceTexts_;  /**< 选择模式下直接显示的选项按钮。 */
    int selectedChoice_ = -1;  /**< 选择模式下用户选择的选项序号。 */
};

#endif // APPMESSAGEDIALOG_H
