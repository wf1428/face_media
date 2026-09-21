/**
 * @file ic_worker.cpp
 * @brief 在非 UI 线程中运行 IC 串口、二维码、RS485 和协议处理逻辑。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#include "ic_worker.h"

#include "ic_event_bridge.h"
#include "common/sql/dbstore.h"
#include "local_access_service.h"
#include "network_access_service.h"

#include <QDateTime>
#include <QDebug>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QProcess>
#include <QSettings>
#include <QThread>
#include <QTimer>
#include "platform/rk3566_platform.h"

namespace {

/**
 * @brief 读取运行时 MQTT 协议模式并兼容历史别名。
 * @return "offline" 或 "online"；配置文件不存在时沿用在线模式。
 */
enum class RuntimeIcMode {
    OfflineV1,
    OnlineV1,
    OnlineV2
};

RuntimeIcMode runtimeIcModeFromIni()
{
    const QString kCfgPath = Rk3566Platform::netConfigPath();
    if (!QFileInfo::exists(kCfgPath)) {
        return RuntimeIcMode::OnlineV2;
    }

    QSettings ini(kCfgPath, QSettings::IniFormat);
    ini.setIniCodec("UTF-8");

    if (ini.value(QStringLiteral("feature/online_v1"), false).toBool()) {
        return RuntimeIcMode::OnlineV1;
    }
    if (ini.value(QStringLiteral("feature/online_v2"), false).toBool()) {
        return RuntimeIcMode::OnlineV2;
    }
    if (ini.value(QStringLiteral("feature/offline_v1"), false).toBool()) {
        return RuntimeIcMode::OfflineV1;
    }

    QString mode = ini.value("mqtt/protocol_mode", "online").toString().trimmed().toLower();
    return (mode == QStringLiteral("online") || mode == QStringLiteral("legacy"))
            ? RuntimeIcMode::OnlineV2 : RuntimeIcMode::OfflineV1;
}

/** @return 当前配置要求走完整离线 IC 校验时返回 true。 */
bool isOfflineIcMode()
{
    return runtimeIcModeFromIni() == RuntimeIcMode::OfflineV1;
}

/**
 * @brief 设置系统时钟并尽力同步 RTC。
 *
 * RTC 同步失败只记录告警，不推翻已经成功的系统时间设置结果。
 */
bool applySystemDateTimeLikeCore(const QDateTime &dt, QString *err)
{
    if (!dt.isValid()) {
        if (err) *err = "datetime_invalid";
        return false;
    }

    if (!Rk3566Platform::setSystemDateTime(dt, err)) {
        return false;
    }
    QString rtcError;
    if (!Rk3566Platform::syncRtcFromSystem(&rtcError)) {
        qWarning() << "[time] RTC sync failed:" << rtcError;
    }
    return true;
}

/** @brief 从固定格式的在线 JSON 二维码报文中提取非空 qrCode 字段。 */
bool parseOnlineQrCode(const QString &frame, QString *qrCodeOut)
{
    if (!frame.trimmed().startsWith("{")) return false;

    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(frame.toUtf8(), &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject()) return false;

    const QJsonObject object = doc.object();
    const QString qrCode = object.value(QStringLiteral("qrCode"))
            .toString().trimmed();
    if (qrCode.isEmpty()) return false;

    if (qrCodeOut) *qrCodeOut = qrCode;
    return true;
}

} // namespace

/** @brief 保存 I/O 配置；硬件对象在 start() 所在线程中延迟创建。 */
IcWorker::IcWorker(const IcSerialConfig &io, QObject *parent)
    : QObject(parent), io_(io)
{
}

/** @brief 销毁前关闭串口和当前线程数据库连接。 */
IcWorker::~IcWorker()
{
    stop();
}

bool IcWorker::handleStoredCard(const QString &cardId, const QByteArray &rawFrame)
{
    const QString normalizedCard = cardId.trimmed().toUpper();
    const QList<CardHolderInfo> networkHolders = networkAccess_
            ? networkAccess_->cardHolders(normalizedCard) : QList<CardHolderInfo>();
    const QList<CardHolderInfo> localHolders = localAccess_
            ? localAccess_->cardHolders(normalizedCard) : QList<CardHolderInfo>();
    const int holderCount = networkHolders.size() + localHolders.size();
    if (holderCount == 0) return false;

    if (holderCount > 1) {
        QString reason;
        if (holderCount > 2) {
            reason = QStringLiteral("多个人员卡号冲突");
        } else {
            QStringList names;
            for (const CardHolderInfo &holder : networkHolders) {
                names.append(holder.name + QStringLiteral("人员"));
            }
            for (const CardHolderInfo &holder : localHolders) {
                names.append(holder.name + QStringLiteral("人员"));
            }
            reason = QStringLiteral("卡号冲突：%1")
                    .arg(names.join(QStringLiteral("和")));
        }
        qWarning().noquote() << "[IC-CARD-ROUTE]" << reason;
        IcEventBridge::instance()->emitToastFailRequested(reason);
        return true;
    }

    const bool useNetwork = !networkHolders.isEmpty();
    qInfo().noquote() << QStringLiteral("[IC-CARD-ROUTE] card=%1 selected=%2")
                         .arg(normalizedCard)
                         .arg(useNetwork ? QStringLiteral("network")
                                         : QStringLiteral("local"));

    const QDateTime now = QDateTime::currentDateTime();
    if (useNetwork) {
        finishNetworkCard(networkAccess_->checkCardId(normalizedCard, now), rawFrame);
    } else {
        finishLocalCard(localAccess_->checkCardId(normalizedCard, now));
    }
    return true;
}

void IcWorker::finishNetworkCard(const NetworkAccessResult &result,
                                 const QByteArray &rawFrame)
{
    QStringList floorTexts;
    for (int floor : result.floors) floorTexts.append(QString::number(floor));
    const QString floors = floorTexts.join(QLatin1Char(','));

    if (!result.pass) {
        qInfo() << "[NETWORK-PERSON][IC] DENY" << "code=" << result.code << result.reason;
        IcEventBridge::instance()->emitToastFailRequested(result.reason, result.code);
        IcEventBridge::instance()->emitCardAccessFinished(
                    result.personId, result.credential, floors,
                    result.rs485Frame, false, result.reason);
        return;
    }

    const bool sent = rs485_ && rs485_->sendFrame(result.rs485Frame);
    qInfo() << "[NETWORK-PERSON][IC]" << (sent ? "PASS" : "RS485_FAIL")
            << "personId=" << result.personId
            << "cardId=" << result.credential
            << "floors=" << result.floors
            << "frame=" << result.rs485Frame.toHex(' ');
    if (!sent) {
        const QString reason = QStringLiteral("RS485发送失败");
        IcEventBridge::instance()->emitToastFailRequested(reason);
        IcEventBridge::instance()->emitCardAccessFinished(
                    result.personId, result.credential, floors,
                    result.rs485Frame, false, reason);
        return;
    }

    if (!networkAccess_ || !networkAccess_->recordSuccessfulAccess(result)) {
        const QString reason = QStringLiteral("刷卡通行次数/金额扣减失败");
        qWarning().noquote() << "[NETWORK-PERSON][IC] DEDUCT_FAIL"
                             << "personId=" << result.personId
                             << "cardId=" << result.credential;
        IcEventBridge::instance()->emitToastFailRequested(reason);
        IcEventBridge::instance()->emitCardAccessFinished(
                    result.personId, result.credential, floors,
                    result.rs485Frame, false, reason);
        return;
    }

    IcEventBridge::instance()->emitToastPassRequested();
    IcEventBridge::instance()->emitCardAccessFinished(
                result.personId, result.credential, floors,
                result.rs485Frame, true, QString());
    IcEventBridge::instance()->emitCardPassed(result.credential, QString(), rawFrame);
}

void IcWorker::finishLocalCard(const NetworkAccessResult &result)
{
    if (!result.pass) {
        IcEventBridge::instance()->emitToastFailRequested(result.reason, result.code);
        return;
    }

    const bool sent = rs485_ && rs485_->sendFrame(result.rs485Frame);
    const bool recorded = sent && localAccess_
            && localAccess_->recordSuccessfulAccess(result);
    if (recorded) {
        IcEventBridge::instance()->emitToastPassRequested();
    } else {
        IcEventBridge::instance()->emitToastFailRequested(
                    !sent ? QStringLiteral("RS485发送失败")
                          : QStringLiteral("本地通行次数/金额扣减失败"));
    }
}

void IcWorker::finishEmbeddedPermissionCard(const QByteArray &frame)
{
    const DeviceConfig cfg = DeviceConfigSync::cfg();
    const auto result = OfflineChecker::check(
                frame, cfg.floorNum, cfg.cardSecret.toLatin1(),
                QDateTime::currentDateTime());
    const QString type = result.type == OfflineChecker::CardType::Multi115
            ? QStringLiteral("MULTI")
            : result.type == OfflineChecker::CardType::Visitor103
              ? QStringLiteral("VISITOR") : QStringLiteral("UNKNOWN");

    if (!result.pass) {
        qInfo().noquote() << QStringLiteral("[%1] DENY source=embedded reason=%2")
                             .arg(type, result.reason);
        IcEventBridge::instance()->emitToastFailRequested(result.reason);
        return;
    }

    qInfo() << "[" << type << "] PASS source=embedded"
            << "relayNum=" << result.relayNum
            << "relayTimes=" << result.relayTimes
            << "fcbbuf36=" << result.fcbbuf36.toHex(' ');
    IcEventBridge::instance()->emitToastPassRequested();
    if (rs485_) rs485_->sendFrame(result.fcbbuf36);
}

/**
 * @brief 在 worker 线程中创建并启动全部 IC 链路对象。
 *
 * @note 刷卡器、二维码、RS485、协议处理和 IC 数据库初始化都归该线程所有；
 *       UI 提示通过 IcEventBridge 发回主线程。
 */
void IcWorker::start()
{
    if (started_) return;
    started_ = true;

    DeviceConfig cfg = DeviceConfigSync::bootstrapAndLoad(io_.dbPath);

    networkAccess_ = new NetworkAccessService;
    if (!networkAccess_->initialize()) {
        qWarning() << "[IC-WORKER] online_v1 network access database init failed:"
                   << DbStore::lastError();
    }
    localAccess_ = new LocalAccessService;
    if (!localAccess_->initialize()) {
        qWarning() << "[IC-WORKER] local access database init failed:"
                   << DbStore::lastError();
    }

    qInfo() << "[IC-WORKER] start thread=" << QThread::currentThread()
            << "floorNum=" << cfg.floorNum
            << "cardSecret=" << cfg.cardSecret;

    // ================= RS485 =================
    rs485_ = new Rs485DriverPort(this);
    QObject::connect(IcEventBridge::instance(), &IcEventBridge::rs485SendRequested,
                     this, [this](const QByteArray &frame, const QString &sourceTag) {
        qInfo() << "[IC-BRIDGE] RS485 TX req"
                << "sourceTag=" << sourceTag
                << "len=" << frame.size()
                << "hex=" << frame.toHex(' ');

        if (!rs485_) {
            IcEventBridge::instance()->emitRs485SendFinished(
                        sourceTag, false, QStringLiteral("RS485未初始化"));
            return;
        }

        const bool ok = rs485_->sendFrame(frame);
        if (!ok) {
            qWarning() << "[IC-BRIDGE] RS485 send failed, sourceTag=" << sourceTag;
            IcEventBridge::instance()->emitRs485SendFinished(
                        sourceTag, false, QStringLiteral("RS485发送失败"));
            return;
        }

        IcEventBridge::instance()->emitRs485SendFinished(sourceTag, true, QString());
    }, Qt::QueuedConnection);

    if (!rs485_->open(io_.rs485Dev, io_.rs485Baud)) {
        qWarning() << "[IoBootstrap] RS485 open failed:" << io_.rs485Dev;
    } else {
        rs485_->setInterByteTimeoutMs(10);

        QObject::connect(rs485_, &Rs485DriverPort::frameReceived, this,
                         [](const QByteArray& f) {
            qInfo() << "[RS485 RX] len=" << f.size()
                    << "hex=" << f.toHex(' ')
                    << "ascii=" << QString::fromLatin1(f);
            IcEventBridge::instance()->emitRs485FrameReceived(f);
        });

        QObject::connect(rs485_, &Rs485DriverPort::errorOccured, this,
                         [](const QString& e) {
            qWarning() << "[RS485 ERR]" << e;
        });

        qInfo() << "[IoBootstrap] RS485 started:" << io_.rs485Dev
                << "baud=" << io_.rs485Baud;
    }

    QObject::connect(IcEventBridge::instance(), &IcEventBridge::faceRecognized,
                     this,
                     [this](const QString &personId, qint64 localPersonId,
                            const QString &personName, const QString &faceHash) {
        Q_UNUSED(personName)

        if (localPersonId > 0) {
            const NetworkAccessResult result = localAccess_
                    ? localAccess_->checkFacePerson(localPersonId, QDateTime::currentDateTime())
                    : NetworkAccessResult();
            QStringList floorTexts;
            for (int floor : result.floors) floorTexts.append(QString::number(floor));
            const QString floors = floorTexts.join(QLatin1Char(','));
            const QString localNo = personId.trimmed().isEmpty()
                    ? QString::number(localPersonId) : personId.trimmed();
            if (!result.pass) {
                IcEventBridge::instance()->emitLocalFaceAccessFinished(
                            localNo, faceHash, floors, result.rs485Frame, false,
                            result.reason.isEmpty() ? QStringLiteral("本地人脸权限校验失败") : result.reason,
                            result.code);
                return;
            }
            const bool sent = rs485_ && rs485_->sendFrame(result.rs485Frame);
            const bool recorded = sent && localAccess_ && localAccess_->recordSuccessfulAccess(result);
            IcEventBridge::instance()->emitLocalFaceAccessFinished(
                        localNo, faceHash, floors, result.rs485Frame, recorded,
                        !sent ? QStringLiteral("RS485楼层权限下发失败")
                              : (!recorded ? QStringLiteral("本地通行次数/金额扣减失败") : QString()),
                        recorded ? 200 : (!sent ? 0 : 4501));
            return;
        }

        if (runtimeIcModeFromIni() != RuntimeIcMode::OnlineV1) {
            qInfo().noquote() << "[FACE-ACCESS] ignored outside online_v1"
                              << "personId=" << personId.trimmed();
            return;
        }

        const QString normalizedId = personId.trimmed();
        const NetworkAccessResult result = networkAccess_
                ? networkAccess_->checkFacePerson(normalizedId,
                                                  QDateTime::currentDateTime())
                : NetworkAccessResult();

        QStringList floorTexts;
        for (int floor : result.floors) {
            floorTexts.append(QString::number(floor));
        }
        const QString floors = floorTexts.join(QLatin1Char(','));

        if (!result.pass) {
            const QString reason = result.reason.isEmpty()
                    ? QStringLiteral("人脸楼层权限校验失败") : result.reason;
            qInfo().noquote() << "[FACE-ACCESS] DENY"
                              << "personId=" << normalizedId
                              << "code=" << result.code
                              << "reason=" << reason;
            IcEventBridge::instance()->emitFaceAccessFinished(
                        normalizedId, faceHash, floors,
                        result.rs485Frame, false, reason, result.code);
            return;
        }

        const bool sent = rs485_ && rs485_->sendFrame(result.rs485Frame);
        if (!sent) {
            const QString reason = QStringLiteral("RS485楼层权限下发失败");
            qWarning().noquote() << "[FACE-ACCESS] RS485_FAIL"
                                 << "personId=" << normalizedId
                                 << "floors=" << floors
                                 << "frame=" << result.rs485Frame.toHex(' ');
            IcEventBridge::instance()->emitFaceAccessFinished(
                        normalizedId, faceHash, floors,
                        result.rs485Frame, false, reason, 0);
            return;
        }

        if (!networkAccess_ || !networkAccess_->recordSuccessfulAccess(result)) {
            const QString reason = QStringLiteral("人脸通行次数/金额扣减失败");
            qWarning().noquote() << "[FACE-ACCESS] DEDUCT_FAIL"
                                 << "personId=" << normalizedId;
            IcEventBridge::instance()->emitFaceAccessFinished(
                        normalizedId, faceHash, floors,
                        result.rs485Frame, false, reason, 4501);
            return;
        }

        qInfo().noquote() << "[FACE-ACCESS] PASS"
                          << "personId=" << normalizedId
                          << "floors=" << floors
                          << "frame=" << result.rs485Frame.toHex(' ');
        IcEventBridge::instance()->emitFaceAccessFinished(
                    normalizedId, faceHash, floors,
                    result.rs485Frame, true, QString(), 200);
    }, Qt::QueuedConnection);

    // ================= 密码通行 =================
    QObject::connect(IcEventBridge::instance(), &IcEventBridge::passwordAccessRequested,
                     this,
                     [this](const QString &password) {
        const NetworkAccessResult localResult = localAccess_
                ? localAccess_->checkPassword(password, QDateTime::currentDateTime())
                : NetworkAccessResult();
        if (localResult.pass || !localResult.personId.isEmpty()) {
            QStringList localFloors;
            for (int floor : localResult.floors) localFloors.append(QString::number(floor));
            const QString floors = localFloors.join(QLatin1Char(','));
            if (!localResult.pass) {
                IcEventBridge::instance()->emitLocalPasswordAccessFinished(
                            localResult.personId, floors, localResult.rs485Frame,
                            false, localResult.reason, localResult.code);
                return;
            }
            const bool sent = rs485_ && rs485_->sendFrame(localResult.rs485Frame);
            const bool recorded = sent && localAccess_
                    && localAccess_->recordSuccessfulAccess(localResult);
            IcEventBridge::instance()->emitLocalPasswordAccessFinished(
                        localResult.personId, floors, localResult.rs485Frame, recorded,
                        !sent ? QStringLiteral("RS485楼层权限下发失败")
                              : (!recorded ? QStringLiteral("本地通行次数/金额扣减失败") : QString()),
                        recorded ? 200 : (!sent ? 0 : 4501));
            return;
        }
        if (runtimeIcModeFromIni() != RuntimeIcMode::OnlineV1) {
            IcEventBridge::instance()->emitPasswordAccessFinished(
                        QString(), QString(), QByteArray(), false,
                        QStringLiteral("密码通行仅支持 online_v1 模式"), 0);
            return;
        }

        const NetworkAccessResult result = networkAccess_
                ? networkAccess_->checkPassword(password,
                                                QDateTime::currentDateTime())
                : NetworkAccessResult();
        QStringList floorTexts;
        for (int floor : result.floors) {
            floorTexts.append(QString::number(floor));
        }
        const QString floors = floorTexts.join(QLatin1Char(','));

        if (!result.pass) {
            const QString reason = result.reason.isEmpty()
                    ? QStringLiteral("密码通行权限校验失败") : result.reason;
            qInfo().noquote() << "[PASSWORD-ACCESS] DENY"
                              << "personId=" << result.personId
                              << "code=" << result.code
                              << "reason=" << reason;
            IcEventBridge::instance()->emitPasswordAccessFinished(
                        result.personId, floors, result.rs485Frame,
                        false, reason, result.code);
            return;
        }

        const bool sent = rs485_ && rs485_->sendFrame(result.rs485Frame);
        if (!sent) {
            const QString reason = QStringLiteral("RS485楼层权限下发失败");
            qWarning().noquote() << "[PASSWORD-ACCESS] RS485_FAIL"
                                 << "personId=" << result.personId
                                 << "floors=" << floors
                                 << "frame=" << result.rs485Frame.toHex(' ');
            IcEventBridge::instance()->emitPasswordAccessFinished(
                        result.personId, floors, result.rs485Frame,
                        false, reason, 0);
            return;
        }

        qInfo().noquote() << "[PASSWORD-ACCESS] PASS"
                          << "personId=" << result.personId
                          << "floors=" << floors
                          << "frame=" << result.rs485Frame.toHex(' ');

        // 485 已成功下发后，先按启用的次数/金额规则原子扣减本地状态，
        // 再发出完成信号；MQTT 侧收到该信号后才会上报 password.accessResult。
        if (!networkAccess_ || !networkAccess_->recordSuccessfulAccess(result)) {
            const QString reason = QStringLiteral("密码通行次数/金额扣减失败");
            qWarning().noquote() << "[PASSWORD-ACCESS] DEDUCT_FAIL"
                                 << "personId=" << result.personId;
            IcEventBridge::instance()->emitPasswordAccessFinished(
                        result.personId, floors, result.rs485Frame,
                        false, reason, 4501);
            return;
        }

        IcEventBridge::instance()->emitPasswordAccessFinished(
                    result.personId, floors, result.rs485Frame,
                    true, QString(), 200);
    }, Qt::QueuedConnection);

    // ================= 刷卡 =================
    reader_ = new CardSerial2Reader(this);

    QObject::connect(reader_, &CardSerial2Reader::cardFrame, this,
                     [this](CardSerial2Reader::CardType type, const QByteArray& frame) {
        qDebug() << "Card frame type="
                 << (type == CardSerial2Reader::CardType::Multi115 ? "multi115" :
                     type == CardSerial2Reader::CardType::Visitor103 ? "visitor103" : "unknown")
                 << "len=" << frame.size()
                 << "head=" << frame.left(2)
                 << "hex=" << frame.toHex(' ');

        const QString cardId = QString::fromLatin1(frame.mid(35, 8)).trimmed().toUpper();
        if (!handleStoredCard(cardId, frame)) {
            finishEmbeddedPermissionCard(frame);
        }
    });

    QObject::connect(reader_, &CardSerial2Reader::uidCardFrame, this,
                     [this](const QString &cardId, const QByteArray &frame) {
        qInfo() << "[CARD-UID] RX cardId=" << cardId << "raw=" << frame;
        if (!handleStoredCard(cardId, frame)) {
            IcEventBridge::instance()->emitToastFailRequested(
                        QStringLiteral("空卡：%1").arg(cardId));
        }
    });

    readerHeartbeatElapsed_.start();
    readerHeartbeatWarningElapsed_.invalidate();
    QObject::connect(reader_, &CardSerial2Reader::heartbeatReceived, this, [this]() {
        readerHeartbeatElapsed_.restart();
        readerHeartbeatWarningElapsed_.invalidate();
        if (readerHeartbeatFault_) {
            readerHeartbeatFault_ = false;
            qInfo() << "[CARD-READER] heartbeat recovered";
        }
    });

    readerHeartbeatTimer_ = new QTimer(this);
    readerHeartbeatTimer_->setInterval(1000);
    QObject::connect(readerHeartbeatTimer_, &QTimer::timeout, this, [this]() {
        const qint64 elapsed = readerHeartbeatElapsed_.elapsed();
        if (elapsed < 60000) return;
        if (readerHeartbeatWarningElapsed_.isValid()
                && readerHeartbeatWarningElapsed_.elapsed() < 30000) return;

        readerHeartbeatFault_ = true;
        if (readerHeartbeatWarningElapsed_.isValid()) {
            readerHeartbeatWarningElapsed_.restart();
        } else {
            readerHeartbeatWarningElapsed_.start();
        }
        QSettings ini(Rk3566Platform::netConfigPath(), QSettings::IniFormat);
        ini.setIniCodec("UTF-8");
        const bool logEnabled = ini.value(
                    QStringLiteral("card_reader/heartbeat_log_enabled"), true).toBool();
        if (logEnabled) {
            qWarning() << "[CARD-READER] heartbeat timeout, elapsedMs=" << elapsed;
        }
        IcEventBridge::instance()->emitCardReaderHeartbeatWarning(
                    QStringLiteral("读头错误，请检查读头是否正常。"));
    });
    readerHeartbeatTimer_->start();

    QObject::connect(reader_, &CardSerial2Reader::errorOccured, this,
                     [](const QString& err) {
        qDebug() << err;
    });

    if (!reader_->start(io_.cardDev, io_.cardBaud)) {
        qWarning() << "[IoBootstrap] card reader open failed:" << io_.cardDev;
    } else {
        qInfo() << "[IoBootstrap] card reader started:" << io_.cardDev
                << "baud=" << io_.cardBaud;
    }

    // 接收 CommandDialog 投递的刷卡器命令
    QObject::connect(IcEventBridge::instance(), &IcEventBridge::cardReaderCommandRequested,
                     this,
                     [this](const QByteArray &command, const QString &sourceTag) {
        qInfo() << "[IC-BRIDGE] card reader TX req"
                << "sourceTag=" << sourceTag
                << "len=" << command.size()
                << "ascii=" << QString::fromLatin1(command).trimmed();

        if (!reader_) {
            IcEventBridge::instance()->emitCardReaderCommandFinished(sourceTag, false, QStringLiteral("刷卡串口对象未初始化"));
            return;
        }

        QString err;
        const bool ok = reader_->sendCommand(command, &err);

        IcEventBridge::instance()->emitCardReaderCommandFinished(sourceTag, ok, err);}, Qt::QueuedConnection
    );


    // ================= 二维码 =================
    qrReceiver_ = new QrSerialReceiver(this);
    qr_ = new OfflineQr(this);
    qr_->setSecret(cfg.qrSecret);

    QObject::connect(qr_, &OfflineQr::sigSetSystemDateTimeRequest, this,
                     [](const QDateTime& dt) {
        if (!isOfflineIcMode()) {
            return;
        }

        QString err;
        if (applySystemDateTimeLikeCore(dt, &err)) {
            qInfo() << "[QR] time sync applied:" << dt.toString("yyyy-MM-dd HH:mm:ss");
        } else {
            qWarning() << "[QR] time sync failed:" << err;
        }
    });

    if (!qrReceiver_->open(io_.qrDev, io_.qrBaud)) {
        qWarning() << "[IoBootstrap] qr receiver open failed:" << io_.qrDev;
    } else {
        qInfo() << "[IoBootstrap] qr receiver started:" << io_.qrDev
                << "baud=" << io_.qrBaud;
    }

    QObject::connect(qrReceiver_, &QrSerialReceiver::qrFrameReceived, this,
                     [this](const QString& frame) {
        qInfo() << "[QR RX] frame:" << frame;

        QString localQrCode;
        if (!parseOnlineQrCode(frame, &localQrCode)) localQrCode = frame.trimmed();
        const NetworkAccessResult localResult = localAccess_
                ? localAccess_->checkQrCode(localQrCode, QDateTime::currentDateTime())
                : NetworkAccessResult();
        if (localResult.pass || !localResult.personId.isEmpty()) {
            if (!localResult.pass) {
                IcEventBridge::instance()->emitToastFailRequested(localResult.reason, localResult.code);
                return;
            }
            const bool sent = rs485_ && rs485_->sendFrame(localResult.rs485Frame);
            const bool recorded = sent && localAccess_
                    && localAccess_->recordSuccessfulAccess(localResult);
            if (recorded) IcEventBridge::instance()->emitToastPassRequested();
            else IcEventBridge::instance()->emitToastFailRequested(
                        !sent ? QStringLiteral("RS485发送失败")
                              : QStringLiteral("本地通行次数/金额扣减失败"));
            return;
        }

        const RuntimeIcMode runtimeMode = runtimeIcModeFromIni();
        if (runtimeMode == RuntimeIcMode::OnlineV2) {
            QString qrCode;
            if (parseOnlineQrCode(frame, &qrCode)) {
                qInfo() << "[IC-BRIDGE] onlineQrScanned qrCode=" << qrCode;
                IcEventBridge::instance()->emitOnlineQrScanned(qrCode);
                return;
            }

            IcEventBridge::instance()->emitToastFailRequested(
                        QStringLiteral("当前为在线模式,请切换模式后再重试!"));
            return;
        }

        if (runtimeMode == RuntimeIcMode::OnlineV1) {
            QString qrCode;
            if (!parseOnlineQrCode(frame, &qrCode)) {
                IcEventBridge::instance()->emitToastFailRequested(
                            QStringLiteral("二维码格式错误"));
                return;
            }

            qInfo() << "[ONLINE-V1][QR] scanned, wait platform authorization"
                    << "qrCode=" << qrCode;
            IcEventBridge::instance()->emitOnlineV1QrScanned(qrCode);
            return;
        }

        qr_->setSecret(DeviceConfigSync::cfg().qrSecret);
        const auto res = qr_->handleFrame(frame);

        qInfo() << "Type:" << res.type << "Reason:" << res.reason;

        if (res.type == QrResult::FloorCmd) {
            qInfo() << "RS485 frame hex:" << res.rs485Frame.toHex();
            IcEventBridge::instance()->emitToastPassRequested();

            if (rs485_) {
                rs485_->sendFrame(res.rs485Frame);
            }
        } else if (res.type == QrResult::TimeSync) {
            IcEventBridge::instance()->emitToastPassRequested();
        } else {
            IcEventBridge::instance()->emitToastFailRequested(res.reason);
        }
    });

}

/**
 * @brief 停止 IC 串口设备，并关闭当前线程的数据库连接。
 *
 * @note 该槽函数必须在 worker 线程执行，因为 QSerialPort 和 QSqlDatabase
 *       连接都绑定到创建它们的线程。
 */
void IcWorker::stop()
{
    if (readerHeartbeatTimer_) {
        readerHeartbeatTimer_->stop();
    }
    if (reader_) {
        reader_->stop();
    }
    if (qrReceiver_) {
        qrReceiver_->close();
    }
    if (rs485_) {
        rs485_->close();
    }

    delete networkAccess_;
    networkAccess_ = nullptr;
    delete localAccess_;
    localAccess_ = nullptr;

    DbStore::checkpoint();
    DbStore::close();

    started_ = false;
}
