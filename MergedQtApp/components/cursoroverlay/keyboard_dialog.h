/**
 * @file keyboard_dialog.h
 * @brief 支持字母/数字模式、大小写和密码回显的屏幕键盘对话框。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#ifndef KEYBOARDDIALOG_H
#define KEYBOARDDIALOG_H

#include <QDialog>
#include <QLineEdit>

class QLineEdit;
class QLabel;
class QPushButton;

/** @brief 支持字母/数字模式、大小写和密码回显的屏幕键盘对话框。 */
class KeyboardDialog : public QDialog
{
    Q_OBJECT
public:
    /** @brief 创建键盘布局和文本编辑框。 */
    explicit KeyboardDialog(QWidget *parent = nullptr);

    /** @brief 模态弹出键盘并返回文本，通过 accepted 输出是否确认。 */
    static QString getText(QWidget *parent,
                           const QString &title = QStringLiteral("输入"),
                           const QString &initialText = QString(),
                           QLineEdit::EchoMode echo = QLineEdit::Normal,
                           int maxLen = -1,
                           bool *accepted = nullptr);

    /** @brief 设置标题。 */
    void setTitle(const QString &title);

    /** @brief 设置普通或密码回显模式。 */
    void setEchoMode(QLineEdit::EchoMode mode);

    /** @brief 设置最大字符数；-1 表示不限制。 */
    void setMaxLength(int maxLen);

    /** @brief 设置初始文本。 */
    void setText(const QString &text);

    /** @return 当前编辑文本。 */
    QString text() const;

protected:
    /** @brief 显示时从屏幕底部弹出。 */
    void showEvent(QShowEvent *e) override;

private slots:
    /** @brief 把被点击字符键文本追加到编辑框。 */
    void onKeyClicked();
    /** @brief 删除编辑框末尾一个字符。 */
    void onBackspace();
    /** @brief 清空当前输入。 */
    void onClear();
    /** @brief 切换字母大小写并刷新键帽。 */
    void onShift();
    /** @brief 在字母和数字键盘之间切换。 */
    void onToggleMode();

private:
    /** @brief 键盘字符布局模式。 */
    enum class Mode { Alpha, Numeric };

    /** @brief 创建标题、编辑器和功能按键。 */
    void buildUi();

    /** @brief 构建字母键布局。 */
    void buildAlphaKeys();

    /** @brief 构建数字键布局。 */
    void buildNumericKeys();

    /** @brief 切换布局并重建按键区。 */
    void switchMode(Mode m);

    /** @brief 根据 shiftUpper 更新字母键标签。 */
    void updateCase();

    /** @brief 在最大长度约束内追加文本。 */
    void appendText(const QString &t);

    /** @brief 计算屏幕几何并执行底部弹出动画。 */
    void popupFromBottom();

private:
    QLabel   *titleLabel = nullptr;
    QLineEdit *edit = nullptr;

    QWidget *keysHost = nullptr;     /**< 键盘区域容器。 */
    QLayout *keysLayout = nullptr;   /**< 当前字母或数字键盘布局。 */

    QList<QPushButton*> letterBtns;  /**< 需要随 shift 更新标签的字母键。 */

    QPushButton *shiftBtn = nullptr;
    QPushButton *toggleBtn = nullptr;

    Mode mode = Mode::Alpha;
    bool shiftUpper = false;
    int maxLength = -1;
};


#endif //KEYBOARDDIALOG_H
