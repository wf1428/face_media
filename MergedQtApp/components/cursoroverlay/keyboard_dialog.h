/**
 * @file keyboard_dialog.h
 * @brief 支持字母/数字模式、大小写和密码回显的屏幕键盘对话框。
 *
 * @author Du lin
 * @date 2026-07-25
 * @note 最近修改：2026-11-03；修改人：Du lin；新增启动时只读加载呼叫按钮显示开关，默认隐藏；隐藏时物理键仍能独立关闭密码键盘并呼叫，确认和通用输入保持不变。
 */

#ifndef KEYBOARDDIALOG_H
#define KEYBOARDDIALOG_H

#include <QDialog>
#include <QLineEdit>

class QLineEdit;
class QLabel;
class QPushButton;
class QWidget;
class QEvent;

/** @brief 支持字母/数字模式、大小写和密码回显的屏幕键盘对话框。 */
class KeyboardDialog : public QDialog
{
    Q_OBJECT
public:
    /** @brief 创建键盘布局和文本编辑框。 */
    explicit KeyboardDialog(QWidget *parent = nullptr);

    /** @brief 独立的呼叫结果码；不同于确认输入，不授予密码验证成功。 */
    static constexpr int CallRequestedResult = 2;

    /** @brief GUI启动时读取VoiceCallDsp/showPropertyCallButton并缓存，缺项/读取失败默认隐藏。
     * @param[in] configPath net_cfg.ini路径；缺失时不创建或补写文件。
     * @note 只在GUI线程调用；生产在主窗口初始化时加载一次，打开键盘不重读，修改后重启生效。
     *       只控制屏幕按钮可见性，不禁用物理键的独立呼叫退出结果。
     */
    static void loadPropertyCallButtonVisibility(const QString &configPath);

    /** @brief 在 GUI 线程模态输入；可选呼叫动作关闭键盘后单独返回，不确认密码。
     * @param[in] parent 借用宿主窗口；Wayland 使用既有根窗口覆盖层。
     * @param[in] title 键盘标题。
     * @param[in] initialText 初始输入文本。
     * @param[in] echo 文本或密码回显方式。
     * @param[in] maxLen 最大字符数；负数不限制。
     * @param[out] accepted 非空时仅确认按钮返回 true；取消和呼叫均为 false。
     * @param[out] callRequested 非空时支持独立呼叫退出并输出是否触发；按钮是否显示取决于启动配置。空指针禁用该动作。
     * @param[in] callEnabled 弹出时的呼叫可用性快照；控制器发送前仍须再次校验。
     * @return 确认时为输入文本；取消或呼叫时为空。返回前对话框已关闭并销毁。
     */
    static QString getText(QWidget *parent,
                           const QString &title = QStringLiteral("输入"),
                           const QString &initialText = QString(),
                           QLineEdit::EchoMode echo = QLineEdit::Normal,
                           int maxLen = -1,
                           bool *accepted = nullptr,
                           bool *callRequested = nullptr,
                           bool callEnabled = true);

    /** @brief 把当前可见、支持独立呼叫结果的密码键盘关闭，即使屏幕呼叫按钮被配置隐藏。
     * @return true 已清除输入并关闭；false 为不可见或普通输入键盘，未改变其状态。
     * @pre 外部物理键调用前须确认控制器可呼叫；按钮点击本身受按钮 enabled 限制。
     * @note 不读取 GPIO、不发送信令；调用者仍通过 getText 的输出转交原呼叫入口。
     */
    bool requestPropertyCall();

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

    /** @brief Wayland 宿主尺寸变化时同步覆盖层几何。 */
    bool eventFilter(QObject *watched, QEvent *event) override;

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
    QWidget *panel = nullptr;       /**< 随弹窗高度同步变化的键盘面板。 */
    QLabel   *titleLabel = nullptr;
    QLineEdit *edit = nullptr;

    QWidget *keysHost = nullptr;     /**< 键盘区域容器。 */
    QLayout *keysLayout = nullptr;   /**< 当前字母或数字键盘布局。 */

    QList<QPushButton*> letterBtns;  /**< 需要随 shift 更新标签的字母键。 */

    QPushButton *shiftBtn = nullptr;
    QPushButton *toggleBtn = nullptr;
    QPushButton *callButton_ = nullptr; /**< 面板拥有的可选呼叫动作；默认隐藏，不确认输入。 */
    bool propertyCallSupported_ = false; /**< true表示调用者接收独立呼叫结果；与屏幕按钮显隐无关，普通输入为false。 */

    Mode mode = Mode::Alpha;
    bool shiftUpper = false;
    int maxLength = -1;
    bool embeddedForWayland = false; /**< Wayland 下作为根窗口内部覆盖层显示。 */
};


#endif //KEYBOARDDIALOG_H
