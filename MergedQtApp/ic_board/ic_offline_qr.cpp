/**
 * @file ic_offline_qr.cpp
 * @brief 解析离线楼层二维码和时间同步二维码，并校验有效期。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#include "ic_offline_qr.h"
#include <QCryptographicHash>
#include <QDate>
#include <QRegularExpression>

/** @brief 创建离线二维码解析器。 */
OfflineQr::OfflineQr(QObject* parent) : QObject(parent) {}

/** @brief 更新楼层二维码校验密钥，例如 "444888"。 */
void OfflineQr::setSecret(const QString& secret) { secret_ = secret; }

/** @return QString 的 UTF-8 字节表示。 */
static inline QByteArray bytesOf(const QString& s) { return s.toUtf8(); }

/** @return 输入数据的 32 位小写 MD5 十六进制字符串。 */
QString OfflineQr::md5HexLower(const QByteArray& data) {
    QByteArray md5 = QCryptographicHash::hash(data, QCryptographicHash::Md5);
    return QString::fromLatin1(md5.toHex()); // 小写hex
}

/** @brief 解析十进制优先、十六进制兼容的 time: 时间同步报文。 */
bool OfflineQr::parseTimeSync(const QByteArray& b, QDateTime& out, int& outWeek, QString& why) {
    // 兼容两种：十进制优先；再兼容%x的16进制
    QString s = QString::fromUtf8(b).trimmed();

    // 十进制：time:2026-03-02 12:34:56 2#
    {
        QRegularExpression re(R"(time:(\d{4})-(\d{1,2})-(\d{1,2})\s+(\d{1,2}):(\d{1,2}):(\d{1,2})\s+(\d{1,2})#)");
        auto m = re.match(s);
        if (m.hasMatch()) {
            int Y=m.captured(1).toInt();
            int Mo=m.captured(2).toInt();
            int D=m.captured(3).toInt();
            int h=m.captured(4).toInt();
            int mi=m.captured(5).toInt();
            int se=m.captured(6).toInt();
            outWeek=m.captured(7).toInt();
            QDate date(Y,Mo,D);
            QTime time(h,mi,se);
            if (!date.isValid() || !time.isValid()) { why="time十进制解析到非法日期/时间"; return false; }
            out = QDateTime(date,time);
            return true;
        }
    }

    // 十六进制兼容：time:20xx-xx-xx xx:xx:xx xx#
    {
        QRegularExpression re(R"(time:20([0-9a-fA-F]{2})-([0-9a-fA-F]{1,2})-([0-9a-fA-F]{1,2})\s+([0-9a-fA-F]{1,2}):([0-9a-fA-F]{1,2}):([0-9a-fA-F]{1,2})\s+([0-9a-fA-F]{1,2})#)");
        auto m = re.match(s);
        if (m.hasMatch()) {
            auto hx=[&](const QString& x){ return x.toInt(nullptr,16); };
            int Y=2000+hx(m.captured(1));
            int Mo=hx(m.captured(2));
            int D=hx(m.captured(3));
            int h=hx(m.captured(4));
            int mi=hx(m.captured(5));
            int se=hx(m.captured(6));
            outWeek=hx(m.captured(7));
            QDate date(Y,Mo,D);
            QTime time(h,mi,se);
            if (!date.isValid() || !time.isValid()) { why="time十六进制解析到非法日期/时间"; return false; }
            out = QDateTime(date,time);
            return true;
        }
    }

    why = "time格式不匹配";
    return false;
}


/** @brief 依次尝试楼层授权码和时间同步码，并返回稳定的业务结果。 */
QrResult OfflineQr::handleFrame(const QString& frame) {
    QrResult r;
    QByteArray b = bytesOf(frame);

    // 1) 楼层二维码
    // ======================== 楼层二维码：md5#payload#date# ====================
    // 整帧以 `#` 结束
    if (frame.endsWith('#')) {
        // split 会在末尾多出一个空字段，所以 KeepEmptyParts
        const QStringList parts = frame.split('#',  QString::KeepEmptyParts);

        // 分割后满足[md5] [payload] [date] [""]  => size=4
        // 字段长度分别为 md5 = 32; payload = 32; date = 8;
        if (parts.size() >= 4 && parts[3].isEmpty()
            && parts[0].size() == 32
            && parts[1].size() == 32
            && parts[2].size() == 8) {

            const QString md5Field = parts[0];
            const QString payload  = parts[1];
            const QString dateStr  = parts[2];

            // 1) 日期字段校验为数字
            for (QChar c : dateStr) {
                if (!c.isDigit()) {
                    r.type = QrResult::Invalid;
                    r.reason = "date不是8位数字YYYYMMDD";
                    return r;
                }
            }
            r.md5Date = dateStr;

            // 2) 计算期望MD5 = MD5(secret + date)

            // secret_ 用最新配置刷新
            const QString expect = md5HexLower((secret_ + dateStr).toLatin1());

            // 3) 比对 md5字段
//            if (!md5Field.compare(expect, Qt::CaseInsensitive) == 0) {
//                // 上面这行容易误读，改成下面清晰写法：
//            }
            if (md5Field.compare(expect, Qt::CaseInsensitive) != 0) {
                r.type = QrResult::Invalid;
                r.reason = "密钥不通过（md5字段不匹配）";
                return r;
            }

            // 4) 过期判断：current >= qr date => 过期
            const int current = QDate::currentDate().toString("yyyyMMdd").toInt();
            const int qrDate  = dateStr.toInt();
            if (current >= qrDate) {
                r.type = QrResult::Invalid;
                r.reason = QString("二维码过期: current=%1 >= qr=%2").arg(current).arg(qrDate);
                return r;
            }

            // 5) 组帧：# 0x40 payload(32字节ASCII) # !
            QByteArray out;
            out.resize(36);
            out[0] = char(0x23);
            out[1] = char(0x40);

            QByteArray payloadBytes = payload.toLatin1(); // 32字节ASCII
            memcpy(out.data() + 2, payloadBytes.constData(), 32);

            out[34] = char(0x23);
            out[35] = char(0x21);

            r.type = QrResult::FloorCmd;
            r.rs485Frame = out;
            r.reason = "楼层二维码通过";

            // 留口
            emit sigBeep();
            //emit sigRs485SendRequest(out);
            return r;
        }
    }

    // 2) 时间二维码
    if (b.endsWith('#') && b.contains("time")) {
        QDateTime dt;
        int week = 0;
        QString why;
        if (!parseTimeSync(b, dt, week, why)) {
            r.type = QrResult::Invalid;
            r.reason = "二维码更新时间解析失败: " + why;
            return r;
        }

        r.type = QrResult::TimeSync;
        r.reason = QString("时间二维码解析通过: %1 (week=%2)，未实际设置系统时间")
                    .arg(dt.toString("yyyy-MM-dd HH:mm:ss"))
                    .arg(week);

        emit sigBeep();
        emit sigSetSystemDateTimeRequest(dt); // 留口
        return r;
    }

    r.type = QrResult::Invalid;
    r.reason = "不匹配楼层二维码/时间二维码格式";
    return r;
}
