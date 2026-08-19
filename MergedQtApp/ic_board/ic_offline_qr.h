/**
 * @file ic_offline_qr.h
 * @brief 解析离线楼层二维码和时间同步二维码，并校验有效期。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#ifndef IC_OFFLINE_QR_H
#define IC_OFFLINE_QR_H

#include <QObject>
#include <QByteArray>
#include <QString>
#include <QDateTime>

/** @brief 离线二维码解析结果。 */
struct QrResult {
    /** @brief 二维码业务类型。 */
    enum Type { Invalid, FloorCmd, TimeSync } type = Invalid;
    QString reason;          /**< 中文通过说明或失败原因。 */
    QByteArray rs485Frame;   /**< FloorCmd 通过时构造的 36 字节 RS485 帧。 */
    QString md5Date;         /**< 楼层二维码携带的 YYYYMMDD 有效期。 */
};

/**
 * @brief 楼层授权和时间同步二维码的离线解析器。
 *
 * 楼层码格式为 md5#payload#date#，MD5 输入是配置密钥与日期拼接；
 * 时间码仅解析并通过信号请求上层设置系统时间，本类不直接操作系统时钟。
 */
class OfflineQr : public QObject {
    Q_OBJECT
public:
    /** @brief 创建离线二维码解析器。 */
    explicit OfflineQr(QObject* parent=nullptr);

    /** @brief 更新楼层二维码校验密钥，例如 "444888"。 */
    void setSecret(const QString& secret);

    /**
     * @brief 解析一条完整二维码报文。
     * @param frame 楼层码或以 '#' 结束的 time: 时间码。
     * @return 业务类型、原因及可选 RS485 控制帧。
     */
    QrResult handleFrame(const QString& frame);

signals:
    /** @brief 请求上层播放确认蜂鸣；本类不直接操作蜂鸣器。 */
    void sigBeep();

    /** @brief 预留的继电器动作请求。 */
    void sigRelayAction(int relayNum, int relayTimes);

    /** @brief 预留的 RS485 发送请求。 */
    void sigRs485SendRequest(const QByteArray& frame);

    /** @brief 请求上层设置系统时间并同步 RTC。 */
    void sigSetSystemDateTimeRequest(const QDateTime& dt);

private:
    QString secret_ = "444888"; /**< 楼层二维码 MD5 校验密钥。 */

    /** @return 报文末尾包含 "#YYYYMMDD#" 结构时返回 true。 */

    /** @brief 从报文末尾提取 8 位纯数字日期。 */

    /** @return 输入数据的 32 位小写 MD5 十六进制字符串。 */
    static QString md5HexLower(const QByteArray& data);

    /** @brief 要求当前日期严格早于二维码 YYYYMMDD 日期。 */

    /** @brief 按旧固定偏移 33 提取 32 字节楼层 payload。 */

    /** @brief 解析十进制优先、十六进制兼容的 time: 时间同步报文。 */
    static bool parseTimeSync(const QByteArray& b, QDateTime& out, int& outWeek, QString& why);
};

#endif // IC_OFFLINE_QR_H
