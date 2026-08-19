/**
 * @file keyboard_dialog.cpp
 * @brief 支持字母/数字模式、大小写和密码回显的屏幕键盘对话框的实现。
 *
 * @author Dulin
 * @date 2026-07-25
 */

// keyboard_dialog.cpp
// 关键修复：
// 1) 重新使用“面板 QWidget”承载 UI：对话框背景透明时也能稳定显示背景色（否则白底+白字会看不见）
// 2) 改成浅色现代主题：深色文字 + 浅色按钮，符号/字母更清楚
// 3) 输入框文字颜色明确设置，确保输入内容可见（默认 EchoMode=Normal）
// 4) 保持全宽底部弹出、布局紧凑、无 DropShadow（更流畅）
//
// 依赖：keyboard_dialog.h 为通用版（titleLabel/edit/keysHost/... 成员与接口）

#include "keyboard_dialog.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QPushButton>
#include <QLineEdit>
#include <QLabel>
#include <QShowEvent>
#include <QPropertyAnimation>
#include <QGuiApplication>
#include <QScreen>

namespace {
constexpr int kDialogHeight = 340;   // 更紧凑
constexpr int kPadH = 12;
constexpr int kPadV = 10;
constexpr int kRowSpacing = 6;
constexpr int kColSpacing = 6;
constexpr int kKeyMinH    = 44;
constexpr int kAnimMs     = 160;
}

/** @brief 创建统一尺寸和样式属性的屏幕键盘字符键。 */
static QPushButton* makeKeyBtn(const QString &label, QWidget *parent)
{
    auto *b = new QPushButton(label, parent);
    b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    b->setMinimumHeight(kKeyMinH);
    //b->setCursor(Qt::PointingHandCursor);
    b->setFocusPolicy(Qt::NoFocus);
    return b;
}

/** @brief 创建键盘布局和文本编辑框。 */
KeyboardDialog::KeyboardDialog(QWidget *parent)
    : QDialog(parent)
{
    setModal(true);
    setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_TranslucentBackground, true);

    setFixedHeight(kDialogHeight);

    setUpdatesEnabled(false);
    buildUi();

    // ✅ 默认显示明文（调用方需要密码再 setEchoMode(QLineEdit::Password)）
    if (edit) edit->setEchoMode(QLineEdit::Normal);

    switchMode(Mode::Alpha);
    setUpdatesEnabled(true);
}

/** @brief 模态弹出键盘并返回文本，通过 accepted 输出是否确认。 */
QString KeyboardDialog::getText(QWidget *parent,
                               const QString &title,
                               const QString &initialText,
                               QLineEdit::EchoMode echo,
                               int maxLen,
                               bool *accepted)
{
    KeyboardDialog dlg(parent);
    dlg.setTitle(title);
    dlg.setEchoMode(echo);
    dlg.setMaxLength(maxLen);
    dlg.setText(initialText);

    const int ret = dlg.exec();
    const bool ok = (ret == QDialog::Accepted);
    if (accepted) *accepted = ok;
    return ok ? dlg.text() : QString();
}

/** @brief 设置标题。 */
void KeyboardDialog::setTitle(const QString &title)
{
    if (titleLabel) titleLabel->setText(title);
}

/** @brief 设置普通或密码回显模式。 */
void KeyboardDialog::setEchoMode(QLineEdit::EchoMode mode)
{
    if (edit) edit->setEchoMode(mode);
}

/** @brief 设置最大字符数；-1 表示不限制。 */
void KeyboardDialog::setMaxLength(int maxLen)
{
    maxLength = maxLen;
}

/** @brief 设置初始文本。 */
void KeyboardDialog::setText(const QString &text)
{
    if (edit) edit->setText(text);
}

/** @return 当前编辑文本。 */
QString KeyboardDialog::text() const
{
    return edit ? edit->text() : QString();
}

/** @brief 显示时从屏幕底部弹出。 */
void KeyboardDialog::showEvent(QShowEvent *e)
{
    QDialog::showEvent(e);
    popupFromBottom();
}

/** @brief 创建标题、编辑器和功能按键。 */
void KeyboardDialog::buildUi()
{
    // 透明背景 + 底部面板
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // 顶部留空，把面板压到底部
    root->addStretch(1);

    auto *panel = new QWidget(this);
    panel->setObjectName("kbdPanel");
    panel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    panel->setFixedHeight(height());
    root->addWidget(panel);

    auto *lay = new QVBoxLayout(panel);
    lay->setContentsMargins(kPadH, kPadV, kPadH, kPadV);
    lay->setSpacing(8);

    titleLabel = new QLabel(QStringLiteral("输入"), panel);
    titleLabel->setObjectName("kbdTitle");
    titleLabel->setAlignment(Qt::AlignCenter);
    titleLabel->setFixedHeight(22);
    lay->addWidget(titleLabel);

    edit = new QLineEdit(panel);
    edit->setObjectName("kbdEdit");
    edit->setReadOnly(true);      // 不弹系统输入法
    edit->setFixedHeight(40);
    lay->addWidget(edit);

    keysHost = new QWidget(panel);
    keysHost->setObjectName("kbdKeysHost");
    keysHost->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    lay->addWidget(keysHost, 1);

    auto *btnRow = new QHBoxLayout();
    btnRow->setSpacing(8);

    auto *cancel = new QPushButton(QStringLiteral("取消"), panel);
    auto *ok = new QPushButton(QStringLiteral("确认"), panel);
    cancel->setObjectName("kbdAction");
    ok->setObjectName("kbdActionPrimary");
    cancel->setFixedHeight(40);
    ok->setFixedHeight(40);
//    cancel->setCursor(Qt::PointingHandCursor);
//    ok->setCursor(Qt::PointingHandCursor);
    cancel->setFocusPolicy(Qt::NoFocus);
    ok->setFocusPolicy(Qt::NoFocus);

    connect(ok, &QPushButton::clicked, this, &QDialog::accept);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);

    btnRow->addWidget(cancel);
    btnRow->addWidget(ok);
    lay->addLayout(btnRow);

    // ✅ 浅色现代主题：确保在任何底图上都清晰可读
    setStyleSheet(R"(
        QDialog { background: transparent; }

        QWidget#kbdPanel {
            background: rgba(248, 249, 251, 250);
            border: 1px solid rgba(0,0,0,18);
            border-radius: 16px;
        }

        QLabel#kbdTitle {
            color: rgba(20, 20, 22, 235);
            font-size: 13px;
        }

        QLineEdit#kbdEdit {
            background: rgba(255,255,255,255);
            color: rgba(18,18,20,245);          /* ✅ 输入内容清晰可见 */
            border: 1px solid rgba(0,0,0,20);
            border-radius: 12px;
            padding: 0 10px;
            font-size: 16px;
        }

        QPushButton {
            background: rgba(255,255,255,255);
            color: rgba(18,18,20,245);          /* ✅ 键帽字/符号清晰可见 */
            border: 1px solid rgba(0,0,0,18);
            border-radius: 13px;
            font-size: 14px;
        }
        QPushButton:hover { background: rgba(245,246,248,255); }
        QPushButton:pressed { background: rgba(235,236,240,255); }

        QPushButton#kbdAction {
            background: rgba(255,255,255,255);
        }
        QPushButton#kbdActionPrimary {
            background: rgba(62, 150, 255, 235);
            border: 1px solid rgba(62, 150, 255, 255);
            color: white;
        }
        QPushButton#kbdActionPrimary:hover { background: rgba(62, 150, 255, 255); }
        QPushButton#kbdActionPrimary:pressed { background: rgba(54, 135, 235, 255); }
    )");
}

/** @brief 切换布局并重建按键区。 */
void KeyboardDialog::switchMode(Mode m)
{
    mode = m;

    if (keysLayout) {
        QLayoutItem *child;
        while ((child = keysLayout->takeAt(0)) != nullptr) {
            if (child->widget()) child->widget()->deleteLater();
            delete child;
        }
        delete keysLayout;
        keysLayout = nullptr;
    }

    letterBtns.clear();
    shiftBtn = nullptr;
    toggleBtn = nullptr;

    setUpdatesEnabled(false);
    if (mode == Mode::Alpha) buildAlphaKeys();
    else buildNumericKeys();
    setUpdatesEnabled(true);
}

/** @brief 构建字母键布局。 */
void KeyboardDialog::buildAlphaKeys()
{
    auto *grid = new QGridLayout(keysHost);
    keysLayout = grid;
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing(kColSpacing);
    grid->setVerticalSpacing(kRowSpacing);

    for (int i = 0; i < 12; ++i) grid->setColumnStretch(i, 1);

    auto addKey = [&](const QString &label, const QString &key, int r, int c, int cs = 1) {
        auto *b = makeKeyBtn(label, keysHost);
        b->setProperty("key", key);
        connect(b, &QPushButton::clicked, this, &KeyboardDialog::onKeyClicked);
        grid->addWidget(b, r, c, 1, cs);
        if (key.size() == 1 && key[0].isLetter()) letterBtns.push_back(b);
        return b;
    };

    const QString row1 = "Q W E R T Y U I O P";
    const QString row2 = "A S D F G H J K L";
    const QString row3 = "Z X C V B N M";

    const QStringList keys1 = row1.split(' ', QString::SkipEmptyParts);
    for (int i = 0; i < keys1.size(); ++i) addKey(keys1[i], keys1[i], 0, 1 + i);

    const QStringList keys2 = row2.split(' ', QString::SkipEmptyParts);
    for (int i = 0; i < keys2.size(); ++i) addKey(keys2[i], keys2[i], 1, 2 + i);

    shiftBtn = makeKeyBtn(QStringLiteral("Shift"), keysHost);
    connect(shiftBtn, &QPushButton::clicked, this, &KeyboardDialog::onShift);
    grid->addWidget(shiftBtn, 2, 0, 1, 2);

    const QStringList keys3 = row3.split(' ', QString::SkipEmptyParts);
    for (int i = 0; i < keys3.size(); ++i) addKey(keys3[i], keys3[i], 2, 2 + i);

    auto *bk = makeKeyBtn(QStringLiteral("⌫"), keysHost);
    connect(bk, &QPushButton::clicked, this, &KeyboardDialog::onBackspace);
    grid->addWidget(bk, 2, 9, 1, 2);

    toggleBtn = makeKeyBtn(QStringLiteral("123"), keysHost);
    connect(toggleBtn, &QPushButton::clicked, this, &KeyboardDialog::onToggleMode);
    grid->addWidget(toggleBtn, 3, 0, 1, 2);

    auto *space = makeKeyBtn(QStringLiteral("空格"), keysHost);
    space->setProperty("key", QStringLiteral(" "));
    connect(space, &QPushButton::clicked, this, &KeyboardDialog::onKeyClicked);
    grid->addWidget(space, 3, 2, 1, 6);

    auto *clear = makeKeyBtn(QStringLiteral("清空"), keysHost);
    connect(clear, &QPushButton::clicked, this, &KeyboardDialog::onClear);
    grid->addWidget(clear, 3, 8, 1, 2);

    addKey(".", ".", 3, 10);
    addKey("@", "@", 3, 11);

    updateCase();
}

/** @brief 构建数字键布局。 */
void KeyboardDialog::buildNumericKeys()
{
    auto *grid = new QGridLayout(keysHost);
    keysLayout = grid;
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing(kColSpacing);
    grid->setVerticalSpacing(kRowSpacing);

    for (int i = 0; i < 12; ++i) grid->setColumnStretch(i, 1);

    auto addKey = [&](const QString &label, const QString &key, int r, int c, int cs = 1) {
        auto *b = makeKeyBtn(label, keysHost);
        b->setProperty("key", key);
        connect(b, &QPushButton::clicked, this, &KeyboardDialog::onKeyClicked);
        grid->addWidget(b, r, c, 1, cs);
        return b;
    };

    int n = 1;
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            addKey(QString::number(n), QString::number(n), r, c * 2, 2);
            ++n;
        }
    }

    auto *clear = makeKeyBtn(QStringLiteral("清空"), keysHost);
    connect(clear, &QPushButton::clicked, this, &KeyboardDialog::onClear);
    grid->addWidget(clear, 3, 0, 1, 2);

    addKey("0", "0", 3, 2, 2);

    auto *bk = makeKeyBtn(QStringLiteral("⌫"), keysHost);
    connect(bk, &QPushButton::clicked, this, &KeyboardDialog::onBackspace);
    grid->addWidget(bk, 3, 4, 1, 2);

    addKey("-", "-", 0, 6, 2);
    addKey("_", "_", 0, 8, 2);
    addKey(".", ".", 0, 10, 2);

    addKey("/", "/", 1, 6, 2);
    addKey("@", "@", 1, 8, 2);
    addKey(":", ":", 1, 10, 2);

    addKey("?", "?", 2, 6, 2);
    addKey("!", "!", 2, 8, 2);
    addKey("+", "+", 2, 10, 2);

    toggleBtn = makeKeyBtn(QStringLiteral("ABC"), keysHost);
    connect(toggleBtn, &QPushButton::clicked, this, &KeyboardDialog::onToggleMode);
    grid->addWidget(toggleBtn, 3, 6, 1, 2);

    auto *space = makeKeyBtn(QStringLiteral("空格"), keysHost);
    space->setProperty("key", QStringLiteral(" "));
    connect(space, &QPushButton::clicked, this, &KeyboardDialog::onKeyClicked);
    grid->addWidget(space, 3, 8, 1, 4);
}

/** @brief 根据 shiftUpper 更新字母键标签。 */
void KeyboardDialog::updateCase()
{
    if (mode != Mode::Alpha) return;

    for (auto *b : letterBtns) {
        const QString k = b->property("key").toString();
        if (k.size() == 1 && k[0].isLetter()) {
            b->setText(shiftUpper ? k.toUpper() : k.toLower());
        }
    }
    if (shiftBtn) shiftBtn->setText(shiftUpper ? QStringLiteral("SHIFT") : QStringLiteral("Shift"));
}

/** @brief 在最大长度约束内追加文本。 */
void KeyboardDialog::appendText(const QString &t)
{
    if (!edit) return;
    const QString s = edit->text();
    if (maxLength >= 0 && (s.size() + t.size() > maxLength)) return;
    edit->setText(s + t);
}

/** @brief 把被点击字符键文本追加到编辑框。 */
void KeyboardDialog::onKeyClicked()
{
    auto *b = qobject_cast<QPushButton*>(sender());
    if (!b) return;

    QString key = b->property("key").toString();
    if (key.isEmpty()) key = b->text();

    if (mode == Mode::Alpha && key.size() == 1 && key[0].isLetter()) {
        key = shiftUpper ? key.toUpper() : key.toLower();
        appendText(key);

        if (shiftUpper) { // 单次 Shift
            shiftUpper = false;
            updateCase();
        }
        return;
    }

    appendText(key);
}

/** @brief 删除编辑框末尾一个字符。 */
void KeyboardDialog::onBackspace()
{
    if (!edit) return;
    QString s = edit->text();
    if (!s.isEmpty()) s.chop(1);
    edit->setText(s);
}

/** @brief 清空当前输入。 */
void KeyboardDialog::onClear()
{
    if (edit) edit->clear();
}

/** @brief 切换字母大小写并刷新键帽。 */
void KeyboardDialog::onShift()
{
    shiftUpper = !shiftUpper;
    updateCase();
}

/** @brief 在字母和数字键盘之间切换。 */
void KeyboardDialog::onToggleMode()
{
    switchMode(mode == Mode::Alpha ? Mode::Numeric : Mode::Alpha);
}

/** @brief 计算屏幕几何并执行底部弹出动画。 */
void KeyboardDialog::popupFromBottom()
{
    QRect base;
    if (parentWidget()) base = parentWidget()->frameGeometry();
    else base = QGuiApplication::primaryScreen()->availableGeometry();

    const int h = height();
    const QRect target(base.x(), base.y() + base.height() - h, base.width(), h);

    setFixedWidth(target.width());

    const QPoint endPos(target.topLeft());
    const QPoint startPos(target.x(), target.y() + h + 8);

    move(startPos);

    auto *anim = new QPropertyAnimation(this, "pos");
    anim->setDuration(kAnimMs);
    anim->setStartValue(startPos);
    anim->setEndValue(endPos);
    anim->setEasingCurve(QEasingCurve::OutCubic);
    anim->start(QAbstractAnimation::DeleteWhenStopped);
}
