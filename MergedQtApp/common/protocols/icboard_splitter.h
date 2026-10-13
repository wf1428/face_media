/**
 * @file icboard_splitter.h
 * @brief IC 卡板连续字节流拆帧及刷卡结果判定器。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#ifndef ICBOARD_SPLITTER_H
#define ICBOARD_SPLITTER_H

#include <QObject>
#include <QByteArray>
#include <QTimer>

/**
 * @brief IC 卡板连续字节流拆帧及刷卡结果判定器。
 *
 * 支持 36 字节通过帧、103 字节访客卡帧和 115 字节多层卡帧。收到卡片原始帧后，
 * 会在限定时间内等待通过帧；超时则产生不通过结果。
 */
class IcBoardSplitter : public QObject {
    Q_OBJECT
public:
    /** @brief 初始化单次触发的通过帧等待定时器。 */
    explicit IcBoardSplitter(QObject *parent = nullptr);

    /** @brief 追加原始串口字节并尽可能拆出全部完整帧。 */
    void feed(const QByteArray &data);

    /**
     * @brief 设置收到卡片原始帧后等待通过帧的时间窗口。
     * @param ms 等待时间，单位 ms；小于 50 的值按 50 处理。
     */
    void setWaitPassMs(int ms);

signals:
    /** @brief 输出拆出的 36、103 或 115 字节完整帧，供日志或上层解析使用。 */
    void frameReady(const QByteArray &frame);

    /** @brief 输出刷卡判定结果及“多层卡”“访客卡”或“未知卡”类型。 */
    void swipeResult(bool passed, QString cardType);

private:
    QByteArray m_buf; /**< 跨 readyRead 调用保存的未消费串口数据。 */

    QTimer m_waitTimer;                   /**< 等待 36 字节通过帧的单次定时器。 */
    bool m_waitingPass = false;           /**< 当前是否已收到卡片帧并等待通过帧。 */
    QString m_lastCardType = "未知卡";    /**< 最近一次卡片原始帧的类型。 */
    int m_waitMs = 600;                   /**< 通过帧等待窗口，单位 ms。 */

private:
    /** @brief 循环消费缓冲区中的完整帧，并保留可能跨包的数据。 */
    void trySplit();

    /** @return 缓冲区中 "#@" 帧头的起始位置，不存在时返回 -1。 */
    int  findHeader() const;

    /** @brief 检查帧是否为以 "#@" 开头、"#!" 结尾的 36 字节通过帧。 */
    bool isPassFrame(const QByteArray &f) const;

    /** @brief 识别 103/115 字节卡片帧，并写出对应中文卡类型。 */
    bool isRawCardFrame(const QByteArray &f, QString &typeOut) const;

    /** @brief 根据帧类型推进“等待通过帧”状态并产生最终判定。 */
    void onFrame(const QByteArray &frame);
};


#endif // ICBOARD_SPLITTER_H
