/**
 * @file udisk_status.cpp
 * @brief 轮询 U 盘状态 JSON 并在宿主底部显示状态/进度的覆盖层的实现。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#include "udisk_status.h"

#include <QWidget>
#include <QLabel>
#include <QProgressBar>
#include <QTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>

static const char* kStatusPath = "/tmp/udisk_status.json";

/** @brief 保存宿主窗口并创建覆盖层控件。 */
UdiskStatusOverlay::UdiskStatusOverlay(QWidget *hostWindow, QObject *parent)
    : QObject(parent), m_host(hostWindow)
{
    initUi();
    //layoutUi();
    setVisible(false); // ✅ 默认不显示（符合“平时不显示”）
}

/** @brief 以指定毫秒间隔启动状态轮询。 */
void UdiskStatusOverlay::start(int intervalMs)
{
    layoutUi(); // ✅ 这里再布局（此时窗口大概率已经准备好了）

    if (!m_timer) {
        m_timer = new QTimer(this);
        connect(m_timer, &QTimer::timeout, this, &UdiskStatusOverlay::pollOnce);
    }
    m_timer->setInterval(intervalMs);
    m_timer->start();
}

/** @brief 停止轮询并隐藏覆盖层。 */
void UdiskStatusOverlay::stop()
{
    if (m_timer) m_timer->stop();
}

/** @brief 显式显示或隐藏覆盖层。 */
void UdiskStatusOverlay::setVisible(bool on)
{
    if (m_overlay) m_overlay->setVisible(on);
    if (on && m_overlay) m_overlay->raise();
}

/** @return 覆盖层当前可见时返回 true。 */
bool UdiskStatusOverlay::isVisible() const
{
    return m_overlay ? m_overlay->isVisible() : false;
}

/** @brief 宿主尺寸变化后重新执行贴底布局。 */
void UdiskStatusOverlay::onHostResized()
{
    layoutUi();
}

/** @brief 创建半透明标签和进度条。 */
void UdiskStatusOverlay::initUi()
{
    // 叠加层容器（挂在主窗口上，不放到stack里）
    m_overlay = new QWidget(m_host);
    m_overlay->setAttribute(Qt::WA_StyledBackground, true);
    m_overlay->setAttribute(Qt::WA_TransparentForMouseEvents, true); // 不挡点击
    m_overlay->setObjectName("udiskOverlay");

    // 样式：黑底半透明 + 白字 + 圆角
    m_overlay->setStyleSheet(
        "#udiskOverlay { background: rgba(0,0,0,160); border-radius: 10px; }"
        "QLabel { color: white; font-size: 22px; padding-left: 12px; padding-right: 12px; }"
        "QProgressBar { height: 18px; border-radius: 8px; background: rgba(255,255,255,40);"
        "              border: 1px solid rgba(255,255,255,70); text-align: center; color: white; }"
        // ✅ 关键：chunk 必须有 background，否则可能透明看不到
        "QProgressBar::chunk { border-radius: 8px;"
        "                      background-color: rgb(0, 200, 83); }"
    );

    m_label = new QLabel(m_overlay);
    m_label->setText(QStringLiteral("等待插入U盘"));
    m_label->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);

    m_progress = new QProgressBar(m_overlay);
    m_progress->setRange(0, 100);
    m_progress->setValue(0);
    m_progress->setTextVisible(true);          // ✅ 显示文字
    m_progress->setFormat("%p%");              // ✅ 显示百分比（0%~100%）
}

/** @brief 将覆盖层贴到宿主窗口底部。 */
void UdiskStatusOverlay::layoutUi()
{
    if (!m_host || !m_overlay || !m_label || !m_progress) return;

    const int margin = 14;
    const int overlayH = 86;

    const int w = m_host->width() - margin * 2;
    const int h = overlayH;
    const int x = margin;
    const int y = m_host->height() - margin - overlayH;

    m_overlay->setGeometry(x, y, w, h);

    m_label->setGeometry(0, 0, w, 52);
    m_progress->setGeometry(12, 56, w - 24, 20);

    m_overlay->raise();
}

/** @brief 读取一次状态文件并刷新覆盖层。 */
void UdiskStatusOverlay::pollOnce()
{
    // 其他消息保护
    if (QDateTime::currentMSecsSinceEpoch() < m_manualMessageUntilMs) {
        return;
    }

    QFile f(QString::fromUtf8(kStatusPath));
    if (!f.open(QIODevice::ReadOnly)) {
        // 文件暂时不存在：给一次提示即可（2~3s 自动隐藏）
        if (m_lastState != "no_file") {
            m_lastState = "no_file";
            m_lastProgress = -1;

            const int token = ++m_hideToken;
            m_label->setText(QStringLiteral("状态文件不存在，等待后台服务..."));
            m_progress->setVisible(false);
            m_progress->setValue(0);

            setVisible(true);
            m_overlay->raise();

            QTimer::singleShot(m_doneTipMs, this, [this, token]() {
                if (token != m_hideToken) return;
                setVisible(false);
            });

            emit udiskStateChanged(m_lastState, 0);
        }
        return;
    }

    const QByteArray data = f.readAll();
    f.close();

    updateFromJson(data);
}

/** @brief 解析状态 JSON，处理插入、复制进度、完成和拔出。 */
void UdiskStatusOverlay::updateFromJson(const QByteArray &data)
{
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(data, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        return;
    }

    const QJsonObject o = doc.object();
    const QString state = o.value("state").toString();
    const QString msg = o.value("message").toString();
    const int progress = o.value("progress").toInt(0);
    const int doneCount = o.value("done_count").toInt(0);
    const int totalCount = o.value("total_count").toInt(0);
    const QString curFile = o.value("current_file").toString();
    const int errorCode = o.value("error_code").toInt(0);

    if (state.isEmpty()) return;

    // 记录上一个状态（用于 idle/removed 的“别立刻压掉提示”）
    const QString prevState = m_lastState;

    // ========= 1) idle：隐藏（但刚 removed 时别立刻压掉 2~3s 提示） =========
    if (state == "idle") {
        if (prevState == "removed") {
            // 让 removed 的延迟隐藏自己完成
            m_lastState = state;
            m_lastProgress = -1;
            return;
        }

        ++m_hideToken; // 取消所有延迟隐藏任务
        setVisible(false);
        m_progress->setVisible(false);
        m_progress->setValue(0);
        m_lastState = state;
        m_lastProgress = -1;
        return;
    }

    // 记住：确实发生过插入（后续才允许显示“已拔出”）
    if (state == "inserted" || state == "waiting" || state == "copying" || state == "done" || state == "error") {
        m_seenInserted = true;
    }

    // removed：如果从未插入过，不显示
    if (state == "removed" && !m_seenInserted) {
        ++m_hideToken;
        setVisible(false);
        m_progress->setVisible(false);
        m_progress->setValue(0);
        m_lastState = state;
        m_lastProgress = -1;
        return;
    }

    // ========= 2) 更新策略（按脚本一致） =========
    // copying：进度/文件/计数变化都要更新
    // 其它状态：state 或 message 变化才更新（脚本的 waiting/error 可能更新 message）
    const bool stateChanged = (state != m_lastState);
    const bool msgChanged = (msg != m_label->text()); // 简单判断：文字不同就算变化
    const bool progressChanged = (progress != m_lastProgress);

    if (state == "copying") {
        // copying 需要持续刷新
        if (!stateChanged && !progressChanged && !curFile.isEmpty()) {
            // 如果你希望“文件名变化也刷新”，可以再加一个缓存变量，这里先不做
        }
        // 不 return：copying 直接继续更新
    } else {
        // 非 copying：只有 state 变化或 message 变化才更新
        if (!stateChanged && !msgChanged) {
            emit udiskStateChanged(state, m_progress->value());
            return;
        }
    }

    m_lastState = state;
    m_lastProgress = progress;

    // 进入非 done/removed 状态：取消旧的延迟隐藏（防止处理中被隐藏）
    if (state != "done" && state != "removed") {
        ++m_hideToken;
    }

    // ========= 3) 文案 + 进度条行为=========
    QString text;

    if (state == "inserted") {
        // 脚本 message 已经是 “已插入U盘”
        text = msg.isEmpty() ? QStringLiteral("已插入U盘") : msg;
        m_progress->setVisible(false);
        m_progress->setValue(0);
    }
    else if (state == "waiting") {
        text = msg.isEmpty() ? QStringLiteral("U盘已插入，等待就绪...") : msg;
        m_progress->setVisible(false);
        m_progress->setValue(0);
    }
    else if (state == "copying") {
        // 显示进度条 + 百分比
        const int p = qBound(0, progress, 100);

        // 组装文字：msg + (done/total) + file + percent
        if (!curFile.isEmpty()) {
            text = QString("%1  (%2/%3)  %4  %5%")
                       .arg(msg.isEmpty() ? QStringLiteral("正在拷贝...") : msg)
                       .arg(doneCount)
                       .arg(totalCount)
                       .arg(curFile)
                       .arg(p);
        } else {
            text = QString("%1  (%2/%3)  %4%")
                       .arg(msg.isEmpty() ? QStringLiteral("正在拷贝...") : msg)
                       .arg(doneCount)
                       .arg(totalCount)
                       .arg(p);
        }

        m_progress->setVisible(true);
        m_progress->setValue(p); //
    }
    else if (state == "done") {
        // 脚本 message: "拷贝完成，请拔出U盘"
        text = msg.isEmpty() ? QStringLiteral("拷贝完成，请拔出U盘") : msg;
        m_progress->setVisible(true);
        m_progress->setValue(100);
    }
    else if (state == "removed") {
        text = msg.isEmpty() ? QStringLiteral("U盘已拔出") : msg;
        m_progress->setVisible(false);
        m_progress->setValue(0);

        // 一次流程结束，复位
        m_seenInserted = false;
    }
    else if (state == "error") {
        // 脚本会给详细 msg + error_code
        if (!msg.isEmpty() && errorCode != 0) {
            text = QString("%1 (错误码:%2)").arg(msg).arg(errorCode);
        } else if (!msg.isEmpty()) {
            text = msg;
        } else if (errorCode != 0) {
            text = QStringLiteral("发生错误 (错误码:%1)").arg(errorCode);
        } else {
            text = QStringLiteral("发生错误");
        }

        // error 可能带有 progress（比如拷贝中拔出，脚本会写 prog）
        const int p = qBound(0, progress, 100);
        if (p > 0 && totalCount > 0) {
            m_progress->setVisible(true);
            m_progress->setValue(p);
        } else {
            m_progress->setVisible(false);
            m_progress->setValue(0);
        }

        // ✅ 重要：error 不自动隐藏，让用户能看到
        ++m_hideToken; // 取消任何旧的延迟隐藏
    }
    else {
        // 兜底：直接显示脚本 msg
        text = msg.isEmpty() ? QStringLiteral("处理中...") : msg;
        m_progress->setVisible(false);
        m_progress->setValue(0);
    }

    m_label->setText(text);

    // 显示出来
    setVisible(true);
    m_overlay->raise();

    // ========= 4) done/removed：显示 2~3s 后隐藏 =========
    if (state == "done" || state == "removed") {
        const int token = ++m_hideToken;
        QTimer::singleShot(m_doneTipMs, this, [this, token]() {
            if (token != m_hideToken) return;
            setVisible(false);
        });
    }

    emit udiskStateChanged(state, m_progress->value());
}


/** @brief 临时显示媒体业务消息，并在 durationMs 后恢复自动状态。 */
void UdiskStatusOverlay::showMessage(const QString &text, int durationMs)
{
    if (!m_overlay || !m_label || !m_progress) return;

    ++m_hideToken;
    const int token = m_hideToken;

    m_manualMessageUntilMs = QDateTime::currentMSecsSinceEpoch() + durationMs;

    m_label->setText(text);
    m_progress->setVisible(false);
    m_progress->setValue(0);

    setVisible(true);
    m_overlay->raise();

    if (durationMs > 0) {
        QTimer::singleShot(durationMs, this, [this, token]() {
            if (token != m_hideToken) return;
            setVisible(false);
        });
    }
}
