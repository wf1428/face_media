/**
 * @file icboard_splitter.cpp
 * @brief IC 卡板连续字节流拆帧及刷卡结果判定器的实现。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#include "icboard_splitter.h"

static constexpr int kLenPass    = 36;   // fcbbuf
static constexpr int kLenVisitor = 103;  // 访客卡原始帧
static constexpr int kLenMulti   = 115;  // 多层卡原始帧

/** @brief 初始化单次触发的通过帧等待定时器。 */
IcBoardSplitter::IcBoardSplitter(QObject *parent)
    : QObject(parent)
{
    m_waitTimer.setSingleShot(true);
    connect(&m_waitTimer, &QTimer::timeout, this, [this](){
        if (!m_waitingPass) return;
        m_waitingPass = false;
        emit swipeResult(false, m_lastCardType); // 超时 -> 不通过
    });
}

/**
 * @brief 设置收到卡片原始帧后等待通过帧的时间窗口。
 * @param ms 等待时间，单位 ms；小于 50 的值按 50 处理。
 */
void IcBoardSplitter::setWaitPassMs(int ms)
{
    if (ms < 50) ms = 50;
    m_waitMs = ms;
}

/** @return 缓冲区中 "#@" 帧头的起始位置，不存在时返回 -1。 */
int IcBoardSplitter::findHeader() const
{
    return m_buf.indexOf("#@");
}

/** @brief 检查帧是否为以 "#@" 开头、"#!" 结尾的 36 字节通过帧。 */
bool IcBoardSplitter::isPassFrame(const QByteArray &f) const
{
    return f.size() == kLenPass && f.mid(0,2) == "#@" && f.mid(34,2) == "#!";
}

/** @brief 识别 103/115 字节卡片帧，并写出对应中文卡类型。 */
bool IcBoardSplitter::isRawCardFrame(const QByteArray &f, QString &typeOut) const
{
    if (f.size() == kLenMulti && f.mid(0,2) == "#@")   { typeOut = "多层卡"; return true; }
    if (f.size() == kLenVisitor && f.mid(0,2) == "#@") { typeOut = "访客卡"; return true; }
    return false;
}

/** @brief 追加原始串口字节并尽可能拆出全部完整帧。 */
void IcBoardSplitter::feed(const QByteArray &data)
{
    if (data.isEmpty()) return;
    m_buf.append(data);
    trySplit();
}

/** @brief 循环消费缓冲区中的完整帧，并保留可能跨包的数据。 */
void IcBoardSplitter::trySplit()
{
    while (true) {
        int idx = findHeader();
        if (idx < 0) {
            // 仅保留最后1字节，防止 "#@" 跨包
            if (m_buf.size() > 1) m_buf = m_buf.right(1);
            return;
        }
        if (idx > 0) m_buf.remove(0, idx);

        // 头在0位置，但不够长度就等待
        if (m_buf.size() < 2) return;

        // 1) 优先抓“通过帧”36字节，且尾为 "#!"
        if (m_buf.size() >= kLenPass) {
            if (m_buf.mid(34, 2) == "#!") {
                QByteArray frame = m_buf.left(kLenPass);
                m_buf.remove(0, kLenPass);
                emit frameReady(frame);
                onFrame(frame);
                continue;
            }
        }

        // 2) 原始帧：103 或 115
        // 2.1 如果够103，而且103后面紧跟下一帧头 "#@"，则先出103（避免把103+下一帧头开头误当115）
        if (m_buf.size() >= kLenVisitor) {
            if (m_buf.size() >= kLenVisitor + 2 && m_buf.mid(kLenVisitor, 2) == "#@") {
                QByteArray frame = m_buf.left(kLenVisitor);
                m_buf.remove(0, kLenVisitor);
                emit frameReady(frame);
                onFrame(frame);
                continue;
            }
        }

        // 2.2 如果够115，出115
        if (m_buf.size() >= kLenMulti) {
            QByteArray frame = m_buf.left(kLenMulti);
            m_buf.remove(0, kLenMulti);
            emit frameReady(frame);
            onFrame(frame);
            continue;
        }

        // 不够完整
        return;
    }
}

/** @brief 根据帧类型推进“等待通过帧”状态并产生最终判定。 */
void IcBoardSplitter::onFrame(const QByteArray &frame)
{
    // A) 收到通过帧：判通过（如果正在等待则结束等待）
    if (isPassFrame(frame)) {
        if (m_waitingPass) {
            m_waitingPass = false;
            m_waitTimer.stop();
            emit swipeResult(true, m_lastCardType);
        } else {
            // 没看到原始帧也收到了通过帧：也算通过（类型未知）
            emit swipeResult(true, "未知卡");
        }
        return;
    }

    // B) 收到原始帧：开始等待通过帧
    QString type;
    if (isRawCardFrame(frame, type)) {
        m_lastCardType = type;
        m_waitingPass = true;
        m_waitTimer.start(m_waitMs);
        return;
    }

    // 其他帧忽略
}
