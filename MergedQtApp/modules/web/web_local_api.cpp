/**
 * @file web_local_api.cpp
 * @brief 标准 Web 本地业务：源数据读取、差量事务、幂等和异步任务。
 * @author Du lin
 * @date 2026-10-16
 */

#include "web_local_api.h"
#include "web_resource_update.h"
#include "web_local_runtime.h"
#include "web_paths.h"
#include "web_face_policy.h"
#include "local_import_gate.h"
#include "common/sql/local_personnel_store.h"
#include "common/sql/web/web_sql.h"
#include "common/sql/web/web_person_repository.h"
#include "common/sql/web/web_account_repository.h"
#include "common/storage_policy.h"
#include "common/network_config.h"
#include "platform/rk3566_platform.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QImageReader>
#include <QNetworkInterface>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QDebug>
#include "ic_board/rs485_floor_frame_builder.h"
#include <QStorageInfo>
#include <QTimer>
#include <QCoreApplication>
#include <QUuid>
#include <algorithm>
#include <cmath>

namespace {

/** @brief 构造本地业务统一的 data/meta 返回结构。 */
QJsonObject reply(const QJsonValue &data, const QJsonObject &meta = {})
{
    return {{"data", data}, {"meta", meta}};
}

/** @brief 生成带毫秒和时区语义的本地操作时间文本。 */
QString now()
{
    return QDateTime::currentDateTime().toString(Qt::ISODateWithMs);
}

/** @brief 生成无大括号的随机任务或资源标识。 */
QString uuid()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

/** @brief 读取 Linux 启动标识，读取失败时返回空值供恢复逻辑判断。 */
QString bootId()
{
    QFile file("/proc/sys/kernel/random/boot_id");
    return file.open(QIODevice::ReadOnly) ? QString::fromLatin1(file.read(128)).trimmed()
                                          : QString();
}

/** @brief 将数据库中保存的 JSON 文本解析为对象。 */
QJsonObject parsed(const QVariant &v)
{
    return QJsonDocument::fromJson(v.toString().toUtf8()).object();
}

/** @brief 检查可选本地数据表是否已经建立。 */
bool table(const QString &name)
{
    return !WebSql::query("SELECT name FROM sqlite_master WHERE type='table' AND name=?", {name})
                .isEmpty();
}

/** @brief 校验整数类型与范围，拒绝小数、非数和越界值。 */
qint64 integer(const QJsonValue &v, qint64 low, qint64 high, const QString &label)
{
    bool ok = false;
    const double d = v.isString() ? v.toString().toDouble(&ok) : v.toDouble(-1);
    if (!v.isString())
        ok = v.isDouble();
    if (!ok || !std::isfinite(d) || std::floor(d) != d || d < low || d > high)
        throw WebError(400, label + "无效");
    return qint64(d);
}

/** @brief 统一解析人员请求中的 personId 或兼容 id 字段。 */
qint64 personId(const QJsonObject &args)
{
    return integer(args.contains("personId") ? args.value("personId") : args.value("id"),
                   1,
                   9007199254740991LL,
                   "人员编号");
}

/** @brief 规范化并校验文本长度和控制字符。 */
QString text(const QJsonObject &args, const QString &key, int max = 160)
{
    if (!args.value(key).isString())
        throw WebError(400, key + "必须是文本");
    const QString s = args.value(key).toString().trimmed();
    if (s.size() > max)
        throw WebError(400, key + "过长");
    for (const auto c : s)
        if (c.unicode() < 32 || c.unicode() == 127)
            throw WebError(400, key + "含控制字符");
    return s;
}

/** @brief 拒绝请求白名单以外的字段，仅保留约定的内部任务参数。 */
void allowed(const QJsonObject &args, const QStringList &names)
{
    for (auto it = args.begin(); it != args.end(); ++it)
        if (!names.contains(it.key()) && it.key() != "_key" && it.key() != "_operation")
            throw WebError(400, "不支持的字段：" + it.key());
}

class Mutation {
    bool committed_ = false;

public:

    /** @brief 获取人员导入互斥门并开启即时写事务；开启失败时释放互斥门。 */
    Mutation()
    {
        if (!LocalImportGate::acquire())
            throw WebError(409, "人员导入或凭证处理正在进行");
        try {
            WebSql::execute("BEGIN IMMEDIATE");
        } catch (...) {
            LocalImportGate::release();
            throw;
        }
    }

    /** @brief 未提交时回滚事务，并在所有退出路径释放人员互斥门。 */
    ~Mutation()
    {
        if (!committed_)
            DbStore::transactionRollback();
        LocalImportGate::release();
    }

    /** @brief 提交当前写事务；提交失败交由析构回滚。 */
    void commit()
    {
        if (!DbStore::transactionCommit())
            throw WebError(503, "保存失败，请查询当前状态");
        committed_ = true;
    }
};

/** @brief 读取人员本地编辑版本，尚未登记时返回初始版本。 */
qint64 revision(qint64 id)
{
    const auto rows =
        WebSql::query("SELECT version FROM web_local_revisions WHERE person_id=?", {id});
    return rows.isEmpty() ? 0 : rows.first().value("version").toLongLong();
}

/** @brief 校验客户端版本并在同一事务内增加人员编辑版本。 */
void change(qint64 id, const QJsonObject &args)
{
    if (args.contains("version") &&
        integer(args.value("version"), 0, 9007199254740991LL, "版本") != revision(id))
        throw WebError(409, "资料已被其他操作更新，请刷新");
    WebSql::execute("INSERT OR IGNORE INTO web_local_revisions(person_id,version) VALUES(?,0)",
                    {id});
    WebSql::execute("UPDATE web_local_revisions SET version=version+1 WHERE person_id=?", {id});
}

/** @brief 生成十进制、十六进制及读头字节序的等价卡号集合。 */
QStringList cardForms(const QString &input)
{
    const auto raw = input.trimmed().toUpper();
    QStringList forms{raw};
    const auto add = [&](const QString &v) {
        if (!forms.contains(v))
            forms.append(v);
    };
    bool ok = false;
    auto n = raw.toULongLong(&ok, 16);
    if (ok && n <= 0xffffffffULL)
        add(QString::number(n));
    n = raw.toULongLong(&ok, 10);
    if (ok && n <= 0xffffffffULL)
        add(QString("%1").arg(n, 8, 16, QLatin1Char('0')).toUpper());
    const auto copy = forms;
    for (const auto &v : copy)
        if (v.size() == 8) {
            auto bytes = QByteArray::fromHex(v.toLatin1());
            if (bytes.size() == 4) {
                std::reverse(bytes.begin(), bytes.end());
                const auto reversed = QString::fromLatin1(bytes.toHex()).toUpper();
                add(reversed);
                const auto number = reversed.toULongLong(&ok, 16);
                if (ok)
                    add(QString::number(number));
            }
        }
    return forms;
}

/** @brief 逐一检查等价卡号，防止占用其他本地或网络人员的卡。 */
void cardConflict(const QString &card, qint64 self)
{
    for (const auto &form : cardForms(card)) {
        if (!WebSql::query(
                 "SELECT person_id FROM local_ic_card WHERE UPPER(card_id)=? AND person_id<>?",
                 {form, self})
                 .isEmpty())
            throw WebError(409, "卡号已属于其他本地人员");
        if (table("network_ic_card") &&
            !WebSql::query("SELECT person_id FROM network_ic_card WHERE UPPER(card_id)=?", {form})
                 .isEmpty())
            throw WebError(409, "卡号已属于网络人员");
    }
}

/** @brief 校验、去重并排序楼层集合，拒绝 0 和范围外楼层。 */
QJsonArray floorNumbers(const QJsonValue &value)
{
    if (!value.isArray() || value.toArray().size() > 128)
        throw WebError(400, "楼层必须是最多128项的数组");
    QList<int> floors;
    for (const auto &v : value.toArray()) {
        const auto n = int(integer(v, -8, 120, "楼层"));
        if (n == 0)
            throw WebError(400, "楼层不能为0");
        if (!floors.contains(n))
            floors.append(n);
    }
    std::sort(floors.begin(), floors.end());
    QJsonArray out;
    for (int n : floors)
        out.append(n);
    return out;
}

/** @brief 校验 HH:mm 格式的日内时间。 */
void validTime(const QString &s)
{
    if (!QRegularExpression("^([01][0-9]|2[0-3]):[0-5][0-9]$").match(s).hasMatch())
        throw WebError(400, "时间需为HH:mm");
}
} // namespace

/** @brief 接收本地异步完成信号，并定时清理本模块的临时图片。 */
WebLocalApi::WebLocalApi(const WebServiceConfig &config, QObject *parent)
    : QObject(parent), config_(config)
{
    connect(
        WebLocalRuntime::instance(),
        &WebLocalRuntime::finished,
        this,
        [this](const QString &id, const QJsonObject &data, int code, const QString &message) {
            finishJob(id, data, code, message);
        },
        Qt::QueuedConnection);
    auto *cleanup = new QTimer(this);
    connect(cleanup, &QTimer::timeout, this, &WebLocalApi::collectAssets);
    cleanup->start(60000);
}

/** @brief 销毁业务对象时释放未完成人脸任务持有的人员互斥门。 */
WebLocalApi::~WebLocalApi()
{
    if (!faceLeases_.isEmpty())
        LocalImportGate::release();
}

/** @brief 初始化本地辅助表、兼容字段及中断任务恢复状态。 */
void WebLocalApi::initialize()
{

    // 复用原本地表结构，仅补充 Web 编辑字段和任务辅助表。
    LocalPersonnelStore store;
    if (!store.initialize())
        throw WebError(503, "本地人员数据未就绪");
    bool remark = false;
    for (const auto &r : WebSql::query("PRAGMA table_info(person)"))
        if (r.value("name") == "remark")
            remark = true;
    if (!remark)
        WebSql::execute("ALTER TABLE person ADD COLUMN remark TEXT NOT NULL DEFAULT ''");
    WebSql::execute("CREATE TABLE IF NOT EXISTS web_local_revisions(person_id INTEGER PRIMARY "
                    "KEY,version INTEGER NOT NULL)");
    WebSql::execute(
        "CREATE TABLE IF NOT EXISTS web_local_operations(id TEXT PRIMARY KEY,owner INTEGER NOT "
        "NULL,kind TEXT NOT NULL,request_key TEXT NOT NULL,hash TEXT NOT NULL,args TEXT NOT "
        "NULL,state TEXT NOT NULL,result TEXT NOT NULL DEFAULT '{}',code INTEGER NOT NULL DEFAULT "
        "200,message TEXT NOT NULL DEFAULT '',created_at INTEGER NOT NULL,updated_at INTEGER NOT "
        "NULL,UNIQUE(owner,kind,request_key))");
    WebSql::execute("CREATE TABLE IF NOT EXISTS web_local_assets(id TEXT PRIMARY KEY,owner INTEGER "
                    "NOT NULL,path TEXT NOT NULL,created_at INTEGER NOT NULL)");
    WebSql::execute("CREATE TABLE IF NOT EXISTS web_local_schema_versions(version INTEGER PRIMARY "
                    "KEY,applied_at TEXT NOT NULL)");
    WebSql::execute(
        "INSERT OR IGNORE INTO web_local_schema_versions(version,applied_at) VALUES(1,?)",
        {now()});
    // In-flight actions are not blindly replayed after a process restart.

    // 重启操作先核对启动标识；其余未确定任务只标记 UNKNOWN，不自动重放。
    const auto boot = bootId();
    for (const auto &r : WebSql::query("SELECT id,args FROM web_local_operations WHERE "
                                       "kind='linux-reboot' AND state='PENDING'")) {
        const auto before = parsed(r.value("args")).value("_bootId").toString();
        if (!boot.isEmpty() && !before.isEmpty() && boot != before)
            WebSql::execute(
                "UPDATE web_local_operations SET state='APPLIED',result=?,message='' WHERE id=?",
                {WebSql::json(QJsonObject{{"state", "COMPLETED"}, {"bootId", boot}}),
                 r.value("id")});
    }
    WebSql::execute(
        "UPDATE web_local_operations SET "
        "state='UNKNOWN',code=503,message='设备进程已重启，请核对实际状态' WHERE state='PENDING'");
    QDir().mkpath(QDir(config_.imports).filePath("_local"));
    collectAssets();
}

/** @brief 按保留时间清理 Web 暂存图片和已结束任务，不处理原导入目录。 */
void WebLocalApi::collectAssets()
{
    // Only this module's generated files are eligible; U-disk/import files are untouched.
    try {
        const auto stamp = QDateTime::currentMSecsSinceEpoch();
        for (const auto &asset :
             WebSql::query("SELECT id,path FROM web_local_assets WHERE created_at<?",
                           {stamp - 600000})) {
            QFile::remove(asset.value("path").toString());
            WebSql::execute("DELETE FROM web_local_assets WHERE id=?", {asset.value("id")});
        }
        for (const auto &file : QDir(QDir(config_.imports).filePath("_local"))
                                    .entryInfoList({"*.jpg"}, QDir::Files | QDir::NoSymLinks)) {
            const auto rows =
                WebSql::query("SELECT kind,state,updated_at FROM web_local_operations WHERE id=?",
                              {file.completeBaseName()});
            if (!rows.isEmpty() && rows.first().value("state") == "PENDING")
                continue;
            const auto ttl = !rows.isEmpty() && rows.first().value("kind") == "screenshot"
                                 ? 120000LL
                                 : 86400000LL;
            if (stamp - file.lastModified().toMSecsSinceEpoch() > ttl)
                QFile::remove(file.absoluteFilePath());
        }
        WebSql::execute("DELETE FROM web_local_operations WHERE state<>'PENDING' AND updated_at<?",
                        {stamp - 7LL * 86400000});
    } catch (const WebError &error) {
        qWarning() << "[WEB] local asset cleanup failed" << error.message;
    }
}

/** @brief 读取未删除本地人员的公开资料和编辑版本。 */
QJsonObject WebLocalApi::person(qint64 id) const
{
    const auto rows =
        WebSql::query("SELECT id,name,COALESCE(NULLIF(account,''),person_no) AS "
                      "account,employee_no AS employeeNo,department,position,phone,remark,enabled "
                      "AS status FROM person WHERE id=? AND deleted=0",
                      {id});
    if (rows.isEmpty())
        throw WebError(404, "本地人员不存在");
    auto p = WebSql::object(rows.first());
    p.insert("source", "local");
    p.insert("version", double(revision(id)));
    return p;
}

/** @brief 从原本地表汇总人员的卡、二维码、人脸、规则、楼层和实际用量。 */
QJsonObject WebLocalApi::credential(qint64 id, bool images) const
{
    person(id);

    // 各项凭证均读取原业务表，公开响应仅携带受控图片元数据。
    QJsonArray cards, qrs, faces, floors;
    for (const auto &r : WebSql::query("SELECT card_id AS cardId,name FROM local_ic_card WHERE "
                                       "person_id=? AND active=1 ORDER BY card_id",
                                       {id}))
        cards.append(WebSql::object(r));
    for (const auto &r :
         WebSql::query("SELECT qr_unique AS qrUnique,qr_name AS name,CASE WHEN qr_type IN "
                       "('VISITOR','QR_VISITOR') THEN 'QR_VISITOR' ELSE 'QR_STANDALONE' END AS "
                       "qrType,valid_from AS validFrom,valid_to AS validTo,max_use_count AS "
                       "maxUseCount FROM local_qr_code WHERE person_id=? AND active=1",
                       {id}))
        qrs.append(WebSql::object(r));
    for (const auto &r : WebSql::query(
             "SELECT id,image_path,created_at FROM face_feature WHERE person_id=? ORDER BY id",
             {id})) {
        QJsonObject f{{"faceId", r.value("id").toString()},
                      {"name", QFileInfo(r.value("image_path").toString()).completeBaseName()},
                      {"registerTime", r.value("created_at").toString()}};
        if (images) {
            const auto path = r.value("image_path").toString();
            try {
                WebPaths::inside(path,
                                 QDir(Rk3566Platform::applicationRoot()).filePath("enrollments"),
                                 true);
                f.insert("_assetPath", path);
            } catch (const WebError &) {
                f.insert("imageAvailable", false);
            }
        }
        faces.append(f);
    }
    for (const auto &r : WebSql::query(
             "SELECT DISTINCT floor_no FROM local_floor WHERE person_id=? ORDER BY floor_no",
             {id}))
        floors.append(r.value("floor_no").toInt());

    // 将持久化规则列映射为标准字段，消费用量另行读取，充值命令不回显。
    QJsonObject rules;
    const auto rows = WebSql::query("SELECT * FROM local_access_rule WHERE person_id=?", {id});
    const auto r = rows.isEmpty() ? QVariantMap{} : rows.first();
    const QList<QPair<QString, QString>> flags{{"time", "time_enabled"},
                                               {"count", "count_enabled"},
                                               {"amount", "amount_enabled"},
                                               {"duration", "duration_enabled"},
                                               {"relay", "relay_enabled"},
                                               {"controlElevator", "control_elevator"},
                                               {"remoteCall", "remote_call_enabled"},
                                               {"cardPassword", "card_password_enabled"}};
    for (const auto &f : flags)
        rules.insert(f.first, r.value(f.second).toBool());
    const QList<QPair<QString, QString>> fields{
        {"countDataTotal", "count_total"},
        {"countDataAddCount", "count_add_count"},
        {"amountDataTotal", "amount_total"},
        {"amountDataAddAmount", "amount_add_amount"},
        {"amountDataUnitPrice", "amount_unit_price"},
        {"durationDataStartTime", "duration_start"},
        {"durationDataEndTime", "duration_end"},
        {"controlElevatorDataValueType", "elevator_value_type"},
        {"remoteCallDataCurrentDayCallCount", "remote_call_current_day_count"},
        {"remoteCallDataEveryDayUpdateCallCount", "remote_call_every_day_count"},
        {"cardPasswordDataPassword", "card_password"}};
    for (const auto &f : fields) {
        const auto value = r.value(f.second);
        const bool stringField =
            f.first.startsWith("durationData") || f.first == "cardPasswordDataPassword";
        rules.insert(f.first,
                     value.isNull()
                         ? (stringField
                                ? QJsonValue(QStringLiteral(""))
                                : QJsonValue(f.first == "controlElevatorDataValueType" ? 1 : 0))
                         : QJsonValue::fromVariant(value));
    }
    // Recharge fields are commands, never replay the last imported delta as a new command.
    rules.insert("countDataAddCount", 0);
    rules.insert("amountDataAddAmount", 0);
    QJsonArray timings;
    for (const auto &t : WebSql::query("SELECT days_json,start_time,end_time FROM "
                                       "local_timing_rule WHERE person_id=? ORDER BY rule_order",
                                       {id}))
        timings.append(QJsonObject{
            {"days", QJsonDocument::fromJson(t.value("days_json").toString().toUtf8()).array()},
            {"timeRange",
             QJsonArray{t.value("start_time").toString(), t.value("end_time").toString()}}});
    rules.insert("timingRules", timings);
    const auto usage =
        WebSql::query("SELECT remaining_count,access_count,remaining_amount,used_amount FROM "
                      "local_access_usage WHERE person_id=?",
                      {id});
    return {{"personId", double(id)},
            {"icCards", cards},
            {"qrCodes", qrs},
            {"faces", faces},
            {"floors", floors},
            {"rules", rules},
            {"usage", usage.isEmpty() ? QJsonObject{} : WebSql::object(usage.first())},
            {"version", double(revision(id))}};
}

/** @brief 按同一 SQL 快照统计人员凭证，并读取实际热点 IP 与媒体分区容量。 */
QJsonObject WebLocalApi::overview() const
{
    // One SQLite statement gives all counters the same read snapshot.
    auto out = WebSql::object(
        WebSql::query("SELECT (SELECT COUNT(*) FROM person WHERE deleted=0) AS personCount,"
                      "(SELECT COUNT(*) FROM face_feature f JOIN person p ON p.id=f.person_id "
                      "WHERE p.deleted=0 AND LENGTH(f.feature_blob)>0) AS faceCount,"
                      "(SELECT COUNT(*) FROM local_ic_card c JOIN person p ON p.id=c.person_id "
                      "WHERE p.deleted=0 AND c.active=1) AS icCount,"
                      "(SELECT COUNT(*) FROM local_qr_code q JOIN person p ON p.id=q.person_id "
                      "WHERE p.deleted=0 AND q.active=1) AS qrCount")
            .first());
    out.insert("observedAt", now());
    QString ip;
    for (const auto &iface : QNetworkInterface::allInterfaces())
        for (const auto &entry : iface.addressEntries())
            if (entry.ip().toString() == "192.168.10.1")
                ip = entry.ip().toString();
    out.insert("deviceIp", ip.isEmpty() ? QJsonValue::Null : QJsonValue(ip));
    try {
        WebPaths::mounted(config_.media, config_.mount);
        QStorageInfo storage(config_.media);
        storage.refresh();
        if (!storage.isValid() || !storage.isReady() || storage.bytesTotal() <= 0)
            throw WebError(503, "存储未就绪");
        const auto used = storage.bytesTotal() - storage.bytesFree();
        const double gib = 1073741824.0;
        out.insert("storage",
                   QJsonObject{{"total", storage.bytesTotal() / gib},
                               {"used", used / gib},
                               {"usedPercent", 100.0 * used / storage.bytesTotal()}});
        out.insert("storageAvailable", true);
    } catch (const WebError &) {
        out.insert("storage", QJsonValue::Null);
        out.insert("storageAvailable", false);
    }
    return out;
}

/** @brief 报告当前已接入能力、硬件就绪状态和允许编辑的规则字段。 */
QJsonObject WebLocalApi::capabilities() const
{
    auto out = WebLocalRuntime::instance()->capabilities();
    out.insert("personnel", true);
    out.insert("credentials", true);
    out.insert("faceEnrollment", true);
    out.insert("elevatorRead", true);
    out.insert("floorScheduleWrite", false);
    out.insert("screenSleep", false);
    out.insert("boardPower", false);
    out.insert("air", false);
    out.insert("configPackage", WebResourceUpdate::available());
    out.insert("mqttRequired", false);
    out.insert("bootId", bootId().isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(bootId()));
    out.insert("ruleFields",
               QJsonArray{"time",
                          "count",
                          "amount",
                          "duration",
                          "controlElevator",
                          "remoteCall",
                          "cardPassword"});
    return out;
}

/** @brief 按账号、业务类型和幂等键登记或复用任务，校验请求摘要及资源上限。 */
QString WebLocalApi::beginJob(const QString &kind, const QJsonObject &args, int owner)
{

    // 请求摘要不包含幂等键；相同账号、类型、键只对应同一份请求内容。
    auto clean = args;
    clean.remove("_key");
    const auto hash = QString::fromLatin1(
        QCryptographicHash::hash(QJsonDocument(clean).toJson(QJsonDocument::Compact),
                                 QCryptographicHash::Sha256)
            .toHex());
    QString key = args.value("_key").toString();
    if (key.isEmpty())
        key = uuid();
    if (key.size() > 128)
        throw WebError(400, "幂等标识过长");
    const auto rows = WebSql::query(
        "SELECT id,hash FROM web_local_operations WHERE owner=? AND kind=? AND request_key=?",
        {owner, kind, key});
    if (!rows.isEmpty()) {
        if (rows.first().value("hash") != hash)
            throw WebError(409, "同一幂等标识对应不同请求");
        return rows.first().value("id").toString();
    }

    // 新任务先检查并发数量，图片类任务额外检查暂存空间。
    const auto pending =
        WebSql::query("SELECT COUNT(*) AS n FROM web_local_operations WHERE state='PENDING'")
            .first()
            .value("n")
            .toInt();
    if (pending >= 16)
        throw WebError(409, "设备本地任务繁忙");
    if (kind == "screenshot" || kind == "face") {
        collectAssets();
        qint64 bytes = 0;
        for (const auto &file :
             QDir(QDir(config_.imports).filePath("_local")).entryInfoList(QDir::Files))
            bytes += file.size();
        if (bytes > 58LL * 1024 * 1024)
            throw WebError(507, "本地图片暂存空间繁忙，请稍后重试");
    }
    const auto id = uuid();
    const auto stamp = QDateTime::currentMSecsSinceEpoch();
    if (kind == "linux-reboot")
        clean.insert("_bootId", bootId());
    WebSql::execute("INSERT INTO "
                    "web_local_operations(id,owner,kind,request_key,hash,args,state,created_at,"
                    "updated_at) VALUES(?,?,?,?,?,?,'PENDING',?,?)",
                    {id, owner, kind, key, hash, WebSql::json(clean), stamp, stamp});
    return id;
}

/** @brief 按账号归属读取异步任务状态、结果和业务错误。 */
QJsonObject WebLocalApi::job(const QString &id, int owner) const
{
    const auto rows =
        WebSql::query("SELECT * FROM web_local_operations WHERE id=? AND owner=?", {id, owner});
    if (rows.isEmpty())
        throw WebError(404, "本地操作不存在");
    const auto r = rows.first();
    return {{"operationId", id},
            {"state", r.value("state").toString()},
            {"result", parsed(r.value("result"))},
            {"code", r.value("code").toInt()},
            {"message", r.value("message").toString()}};
}

/** @brief 保存任务完成事实；严格模式下让结果与业务写入在同一事务提交。 */
void WebLocalApi::finishJob(
    const QString &id, const QJsonObject &data, int code, const QString &message, bool strict)
{
    try {
        auto out = data;
        if (out.contains("assetPath")) {
            const auto path = out.take("assetPath").toString();
            WebPaths::inside(path, config_.imports, true);
            out.insert("_assetPath", path);
            out.insert("_assetTarget", "base64Image");
        }
        const QString state = out.value("state") == "REBOOTING" && code == 200 ? "PENDING"
                              : code == 200                                    ? "APPLIED"
                                                                               : "FAILED";
        WebSql::execute(
            "UPDATE web_local_operations SET state=?,result=?,code=?,message=?,updated_at=? WHERE "
            "id=? AND state='PENDING'",
            {state,
             WebSql::json(out),
             code,
             code == 200 ? QStringLiteral("") : (message.isNull() ? QStringLiteral("") : message),
             QDateTime::currentMSecsSinceEpoch(),
             id});
    } catch (const WebError &e) {
        if (strict)
            throw;
        qWarning() << "[WEB] local operation result persistence failed" << id << e.message;
    }
}

/** @brief 事务化新增、差量修改或软删除本地人员，保留现有凭证与消费记录。 */
QJsonObject WebLocalApi::mutatePerson(const QString &operation, const QJsonObject &args, int)
{
    allowed(args,
            {"id",
             "name",
             "account",
             "employeeNo",
             "department",
             "position",
             "phone",
             "remark",
             "status",
             "version"});

    // 人员修改与幂等结果共用写事务，失败时不会留下半更新资料。
    Mutation tx;
    qint64 id = 0;
    if (operation != "person.add") {
        id = personId(args);
        person(id);
        change(id, args);
    }
    if (operation == "person.delete") {
        WebSql::execute("UPDATE person SET deleted=1,enabled=0,updated_at=? WHERE id=?",
                        {now(), id});
        const auto result = reply(QJsonObject{{"id", double(id)}, {"deleted", true}});
        if (args.contains("_operation"))
            finishJob(args.value("_operation").toString(), result, 200, {}, true);
        tx.commit();
        emit personnelChanged();
        return result;
    }
    QJsonObject data = operation == "person.add" ? QJsonObject{} : person(id);
    const QStringList
        fields{"name", "account", "employeeNo", "department", "position", "phone", "remark"};

    // 仅修改明确允许的基本字段，保留人员已有凭证和稳定编号。
    for (const auto &key : fields)
        if (!data.value(key).isString())
            data.insert(key, QStringLiteral(""));
    for (const auto &key : fields)
        if (args.contains(key))
            data.insert(key, text(args, key, key == "remark" ? 1000 : 160));
    if (data.value("name").toString().isEmpty())
        throw WebError(400, "姓名不能为空");
    QString account = data.value("account").toString();
    if (account.isEmpty())
        account = "WEB_" + uuid().left(12);
    if (!WebSql::query("SELECT id FROM person WHERE deleted=0 AND id<>? AND "
                       "(LOWER(COALESCE(NULLIF(account,''),person_no))=LOWER(?) OR person_no=?)",
                       {id, account, account})
             .isEmpty())
        throw WebError(409, "账号已存在");
    const auto phone = data.value("phone").toString();
    if (!phone.isEmpty() &&
        !WebSql::query("SELECT id FROM person WHERE id<>? AND deleted=0 AND phone=?", {id, phone})
             .isEmpty())
        throw WebError(409, "手机号已属于其他本地人员");
    if (table("network_person")) {
        if (!phone.isEmpty() &&
            !WebSql::query("SELECT person_id FROM network_person WHERE deleted=0 AND phone=?",
                           {phone})
                 .isEmpty())
            throw WebError(409, "手机号已属于网络人员");
        for (const auto &r :
             WebSql::query("SELECT person_json FROM network_person WHERE deleted=0")) {
            auto p = parsed(r.value("person_json"));
            if (p.contains("person"))
                p = p.value("person").toObject();
            if (p.value("account").toString().compare(account, Qt::CaseInsensitive) == 0)
                throw WebError(409, "账号已属于网络人员");
        }
    }
    const int enabled = args.contains("status")
                            ? int(integer(args.value("status"), 0, 1, "启用状态"))
                            : data.value("status").toInt(1);
    if (operation == "person.add") {
        WebSql::execute(
            "INSERT INTO "
            "person(person_no,account,name,employee_no,department,position,phone,remark,enabled,"
            "deleted,person_source,created_at,updated_at) VALUES(?,?,?,?,?,?,?,?,?,0,'manual',?,?)",
            {account,
             account,
             data.value("name").toString(),
             data.value("employeeNo").toString(),
             data.value("department").toString(),
             data.value("position").toString(),
             phone,
             data.value("remark").toString(),
             enabled,
             now(),
             now()});
        id = WebSql::query("SELECT last_insert_rowid() AS id").first().value("id").toLongLong();
        change(id, {});
    } else
        WebSql::execute("UPDATE person SET "
                        "account=?,name=?,employee_no=?,department=?,position=?,phone=?,remark=?,"
                        "enabled=?,updated_at=? WHERE id=?",
                        {account,
                         data.value("name").toString(),
                         data.value("employeeNo").toString(),
                         data.value("department").toString(),
                         data.value("position").toString(),
                         phone,
                         data.value("remark").toString(),
                         enabled,
                         now(),
                         id});
    const auto out = reply(person(id));
    if (args.contains("_operation"))
        finishJob(args.value("_operation").toString(), out, 200, {}, true);
    tx.commit();
    emit personnelChanged();
    return out;
}

/** @brief 处理人脸资源或事务化修改指定凭证、规则和人员楼层。 */
QJsonObject
WebLocalApi::mutateCredential(const QString &operation, const QJsonObject &args, int owner)
{
    const auto id = personId(args);
    const auto p = person(id);
    if (operation == "credential.addSubItem" && args.value("subType") == "FACE") {
        allowed(args, {"personId", "subType", "subData", "version"});
        const auto data = args.value("subData").toObject();
        allowed(data, {"assetId", "name"});

        // 验证图片资源归属与真实大小，HTTP 字节内容不经过业务 IPC。
        const auto assets =
            WebSql::query("SELECT path FROM web_local_assets WHERE id=? AND owner=?",
                          {data.value("assetId").toString(), owner});
        if (assets.isEmpty())
            throw WebError(404, "人脸图片资源不存在");
        const auto assetPath = assets.first().value("path").toString();
        WebPaths::inside(assetPath, QDir(config_.imports).filePath("_local"), true);
        QFile source(assetPath);
        if (!source.open(QIODevice::ReadOnly) || source.size() <= 0 || source.size() > 512 * 1024)
            throw WebError(413, "单张人脸图片最多512KiB");
        const auto bytes = source.read(512 * 1024 + 1);
        source.close();

        // 用实际图片摘要参与幂等判断，重新上传相同内容不会重复录入。
        auto canonical = args;
        canonical.insert(
            "subData",
            QJsonObject{
                {"name", data.value("name").toString()},
                {"imageHash",
                 QString::fromLatin1(
                     QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex())}});
        const auto token = beginJob("face", canonical, owner);
        const auto current = job(token, owner);
        const auto discard = [&]() {
            QFile::remove(assetPath);
            WebSql::execute("DELETE FROM web_local_assets WHERE id=? AND owner=?",
                            {data.value("assetId").toString(), owner});
        };
        if (current.value("state") != "PENDING" || faceLeases_.contains(token)) {
            discard();
            return reply(current);
        }
        if (!LocalImportGate::acquire()) {
            finishJob(token, {}, 409, "人员导入正在进行");
            throw WebError(409, "人员导入正在进行");
        }
        const auto path = QDir(config_.imports).filePath("_local/" + token + ".jpg");
        try {
            QSaveFile image(path);
            if (!image.open(QIODevice::WriteOnly) || image.write(bytes) != bytes.size() ||
                !image.commit())
                throw WebError(503, "图片暂存失败");
            discard();
            QImageReader reader(path);
            reader.setDecideFormatFromContent(true);
            const auto size = reader.size();
            if (!reader.canRead() || !(reader.format() == "jpeg" || reader.format() == "png") ||
                !size.isValid() || qint64(size.width()) * size.height() > 16777216)
                throw WebError(400, "图片类型或尺寸无效");
            faceLeases_.insert(token, true);
            auto payload = args;
            payload.insert("imagePath", path);
            payload.remove("subData");
            payload.insert("name", data.value("name").toString().left(160));
            WebSql::execute("UPDATE web_local_operations SET args=? WHERE id=?",
                            {WebSql::json(payload), token});
            emit faceValidationRequested(
                token,
                QJsonArray{QJsonObject{{"localPersonId", QString::number(id)},
                                       {"personNo", p.value("account")},
                                       {"personName", p.value("name")},
                                       {"phone", p.value("phone")},
                                       {"imagePath", path}}});
            QTimer::singleShot(120000, this, [this, token]() {
                if (faceLeases_.remove(token)) {
                    LocalImportGate::release();
                    finishJob(token, {}, 504, "人脸校验超时");
                }
            });
        } catch (const WebError &e) {
            QFile::remove(path);
            faceLeases_.remove(token);
            LocalImportGate::release();
            finishJob(token, {}, e.status, e.message);
            throw;
        }
        return reply(job(token, owner));
    }

    // 非人脸校验任务在短写事务内处理，先校验版本再修改指定子项。
    QString removedPhoto;
    Mutation tx;
    change(id, args);
    if (operation == "credential.add") {
        allowed(args, {"personId", "extra", "version"});
        if (args.contains("extra") && !args.value("extra").isObject())
            throw WebError(400, "凭证扩展必须是对象");
        if (!args.value("extra").toObject().isEmpty())
            throw WebError(400, "请使用规则或凭证子项接口保存内容");
        WebSql::execute("INSERT OR IGNORE INTO local_access_rule(person_id) VALUES(?)", {id});
    } else if (operation == "personFloor.save" || operation == "personFloor.delete") {
        allowed(args, {"personId", "floor", "version"});
        const auto floors =
            operation.endsWith("delete") ? QJsonArray{} : floorNumbers(args.value("floor"));
        WebSql::execute("DELETE FROM local_floor WHERE person_id=?", {id});
        QSettings settings(config_.networkConfig, QSettings::IniFormat);
        const auto device = settings.value("mqtt/client_id").toString();
        if (device.isEmpty())
            throw WebError(503, "设备号尚未就绪");
        for (const auto &f : floors)
            WebSql::execute("INSERT INTO local_floor(person_id,device_id,floor_no) VALUES(?,?,?)",
                            {id, device, f.toInt()});
        const auto result = reply(QJsonObject{{"personId", double(id)},
                                              {"floors", floors},
                                              {"version", double(revision(id))}});
        if (args.contains("_operation"))
            finishJob(args.value("_operation").toString(), result, 200, {}, true);
        tx.commit();
        return result;
    } else if (operation == "credential.addSubItem") {
        allowed(args, {"personId", "subType", "subData", "version"});
        const auto type = args.value("subType").toString();
        const auto d = args.value("subData").toObject();
        if (type == "IC_CARD") {
            allowed(d, {"cardId", "name"});
            const auto card = text(d, "cardId", 32).toUpper();
            if (!QRegularExpression("^[0-9A-F]{1,32}$").match(card).hasMatch())
                throw WebError(400, "卡号需为十六进制或十进制数字");
            cardConflict(card, id);
            WebSql::execute(
                "INSERT OR REPLACE INTO local_ic_card(card_id,person_id,name,bind_time,active) "
                "VALUES(?,?,?,?,1)",
                {card, id, d.value("name").toString().left(160), now()});
        } else if (type == "QR_STANDALONE" || type == "QR_VISITOR") {
            allowed(d, {"qrUnique", "qrType", "name", "validFrom", "validTo", "maxUseCount"});
            const auto code = text(d, "qrUnique", 8);
            if (!QRegularExpression("^[0-9]{8}$").match(code).hasMatch())
                throw WebError(400, "二维码编号需为8位数字");
            if (!WebSql::query("SELECT person_id FROM local_qr_code WHERE qr_unique=?", {code})
                     .isEmpty())
                throw WebError(409, "二维码已登记");
            if (table("network_qr_code") &&
                !WebSql::query("SELECT person_id FROM network_qr_code WHERE qr_unique=?", {code})
                     .isEmpty())
                throw WebError(409, "二维码已属于网络人员");
            const auto from = d.value("validFrom").toString(), to = d.value("validTo").toString();
            if (type == "QR_VISITOR" &&
                (!QDateTime::fromString(to, Qt::ISODate).isValid() ||
                 QDateTime::fromString(to, Qt::ISODate) <= QDateTime::currentDateTime()))
                throw WebError(400, "访客二维码需设置未来的截止时间");
            if (!from.isEmpty() && !QDateTime::fromString(from, Qt::ISODate).isValid())
                throw WebError(400, "二维码开始时间无效");
            if (!to.isEmpty() && !QDateTime::fromString(to, Qt::ISODate).isValid())
                throw WebError(400, "二维码截止时间无效");
            if (!from.isEmpty() && !to.isEmpty() &&
                QDateTime::fromString(from, Qt::ISODate) >= QDateTime::fromString(to, Qt::ISODate))
                throw WebError(400, "二维码有效期顺序错误");
            const auto max = d.contains("maxUseCount")
                                 ? integer(d.value("maxUseCount"), 0, 1000000000, "二维码次数")
                                 : 0;
            WebSql::execute(
                "INSERT INTO "
                "local_qr_code(qr_unique,person_id,qr_name,qr_type,purpose,valid_from,valid_to,max_"
                "use_count,used_count,bind_time,active) VALUES(?,?,?,?,?,?,?,?,0,?,1)",
                {code,
                 id,
                 d.value("name").toString().left(160),
                 type == "QR_VISITOR" ? "VISITOR" : "CREDENTIAL",
                 "ACCESS",
                 from,
                 to,
                 max,
                 now()});
        } else
            throw WebError(400, "凭证类型无效");
    } else if (operation == "credential.removeSubItem") {
        allowed(args, {"personId", "subType", "itemId", "version"});
        const auto type = args.value("subType").toString();
        const auto item = args.value("itemId").toVariant().toString();
        if (item.isEmpty())
            throw WebError(400, "缺少凭证标识");
        if (type == "IC_CARD")
            WebSql::execute("DELETE FROM local_ic_card WHERE person_id=? AND card_id=?",
                            {id, item});
        else if (type.startsWith("QR_"))
            WebSql::execute("DELETE FROM local_qr_code WHERE person_id=? AND qr_unique=?",
                            {id, item});
        else if (type == "FACE") {
            const auto old =
                WebSql::query("SELECT image_path FROM face_feature WHERE person_id=? AND id=?",
                              {id, item});
            if (!old.isEmpty())
                removedPhoto = old.first().value("image_path").toString();
            WebSql::execute("DELETE FROM face_feature WHERE person_id=? AND id=?", {id, item});
            QStringList paths;
            for (const auto &r :
                 WebSql::query("SELECT image_path FROM face_feature WHERE person_id=?", {id}))
                paths.append(r.value("image_path").toString());
            WebSql::execute("UPDATE person SET has_face=?,face_file_list=? WHERE id=?",
                            {paths.isEmpty() ? 0 : 1, paths.join(','), id});
        } else
            throw WebError(400, "凭证类型无效");
    } else if (operation == "credential.update") {
        allowed(args, {"personId", "extra", "version"});
        const auto extra = args.value("extra").toObject();
        allowed(extra, {"rules"});
        if (!extra.value("rules").isObject())
            throw WebError(400, "缺少规则内容");

        // 将规则补丁合并到当前配置，保留未编辑规则与既有消费记录。
        const auto patch = extra.value("rules").toObject();
        auto rules = credential(id, false).value("rules").toObject();
        const bool existingRelay = rules.value("relay").toBool();
        const QStringList ruleKeys{"time",
                                   "timingRules",
                                   "count",
                                   "countDataTotal",
                                   "countDataAddCount",
                                   "amount",
                                   "amountDataTotal",
                                   "amountDataAddAmount",
                                   "amountDataUnitPrice",
                                   "duration",
                                   "durationDataStartTime",
                                   "durationDataEndTime",
                                   "relay",
                                   "relayStates",
                                   "relayOnTime",
                                   "controlElevator",
                                   "controlElevatorDataValueType",
                                   "remoteCall",
                                   "remoteCallDataCurrentDayCallCount",
                                   "remoteCallDataEveryDayUpdateCallCount",
                                   "cardPassword",
                                   "cardPasswordDataPassword"};
        allowed(patch, ruleKeys);
        for (auto it = patch.begin(); it != patch.end(); ++it)
            rules.insert(it.key(), it.value());
        for (const auto &flag : QStringList{"time",
                                            "count",
                                            "amount",
                                            "duration",
                                            "relay",
                                            "controlElevator",
                                            "remoteCall",
                                            "cardPassword"})
            if (!rules.value(flag).isBool())
                throw WebError(400, flag + "必须是布尔值");
        if (rules.value("relay").toBool() != existingRelay ||
            !rules.value("relayStates").toArray().isEmpty() ||
            rules.value("relayOnTime").toDouble() > 0)
            throw WebError(501, "本地继电器规则执行尚未接入");
        auto countTotal = rules.value("countDataTotal").isNull()
                              ? 0
                              : integer(rules.value("countDataTotal"), 0, 1000000000, "总次数");
        auto countAdd = patch.contains("countDataAddCount")
                            ? integer(patch.value("countDataAddCount"), 0, 1000000000, "新增次数")
                            : 0;
        const auto money = [](QJsonValue v, const QString &label) {
            if (v.isNull())
                return 0.0;
            const double n = v.toDouble(-1);
            if (!v.isDouble() || !std::isfinite(n) || n < 0 || n > 1000000000)
                throw WebError(400, label + "无效");
            return n;
        };
        const double amountTotal = money(rules.value("amountDataTotal"), "总金额"),
                     amountAdd = patch.contains("amountDataAddAmount")
                                     ? money(patch.value("amountDataAddAmount"), "新增金额")
                                     : 0,
                     price = money(rules.value("amountDataUnitPrice"), "单价");
        if (rules.value("amount").toBool() && price <= 0)
            throw WebError(400, "金额规则需设置有效单价");
        const auto type =
            rules.value("controlElevatorDataValueType").isNull()
                ? 1
                : integer(rules.value("controlElevatorDataValueType"), 1, 3, "梯控类型");
        const auto remote = rules.value("remoteCallDataCurrentDayCallCount").isNull()
                                ? 0
                                : integer(rules.value("remoteCallDataCurrentDayCallCount"),
                                          0,
                                          1000000000,
                                          "当天呼梯次数");
        const auto daily = rules.value("remoteCallDataEveryDayUpdateCallCount").isNull()
                               ? 0
                               : integer(rules.value("remoteCallDataEveryDayUpdateCallCount"),
                                         0,
                                         1000000000,
                                         "每日呼梯次数");
        const auto pass = rules.value("cardPasswordDataPassword").toString().trimmed();
        if (rules.value("cardPassword").toBool()) {
            if (!QRegularExpression("^[0-9]{1,16}$").match(pass).hasMatch())
                throw WebError(400, "通行密码需为1至16位数字");
            if (!WebSql::query("SELECT person_id FROM local_access_rule WHERE "
                               "card_password_enabled=1 AND card_password=? AND person_id<>?",
                               {pass, id})
                     .isEmpty())
                throw WebError(409, "通行密码已属于其他人员");
            if (table("network_access_rule") &&
                !WebSql::query("SELECT person_id FROM network_access_rule WHERE "
                               "card_password_enabled=1 AND card_password=?",
                               {pass})
                     .isEmpty())
                throw WebError(409, "通行密码已属于网络人员");
        }
        auto start = rules.value("durationDataStartTime").toString(),
             end = rules.value("durationDataEndTime").toString();
        if (rules.value("duration").toBool()) {
            auto a = QDateTime::fromString(start, Qt::ISODate),
                 b = QDateTime::fromString(end, Qt::ISODate);
            if (!a.isValid())
                a = QDateTime::fromString(start, "yyyy-MM-dd HH:mm:ss");
            if (!b.isValid())
                b = QDateTime::fromString(end, "yyyy-MM-dd HH:mm:ss");
            if (!a.isValid() || !b.isValid() || a >= b)
                throw WebError(400, "有效期起止时间无效");
        }
        const auto timings = rules.value("timingRules").toArray();
        if (timings.size() > 64)
            throw WebError(400, "时段规则过多");
        if (rules.value("time").toBool() && timings.isEmpty())
            throw WebError(400, "请设置通行时段");
        WebSql::execute("DELETE FROM local_timing_rule WHERE person_id=?", {id});
        int index = 0;
        for (const auto &v : timings) {
            const auto t = v.toObject();
            const auto range = t.value("timeRange").toArray(), days = t.value("days").toArray();
            if (range.size() != 2 || days.isEmpty() || days.size() > 7)
                throw WebError(400, "通行时段无效");
            validTime(range[0].toString());
            validTime(range[1].toString());
            for (const auto &d : days)
                integer(d, 1, 7, "星期");
            WebSql::execute(
                "INSERT INTO local_timing_rule(person_id,rule_order,days_json,start_time,end_time) "
                "VALUES(?,?,?,?,?)",
                {id,
                 index++,
                 QString::fromUtf8(QJsonDocument(days).toJson(QJsonDocument::Compact)),
                 range[0].toString(),
                 range[1].toString()});
        }
        WebSql::execute("INSERT OR IGNORE INTO local_access_rule(person_id) VALUES(?)", {id});
        // Preserve counters/usage; only explicit recharge deltas are added once.
        rules.insert("countDataAddCount", 0);
        rules.insert("amountDataAddAmount", 0);
        WebSql::execute(
            "UPDATE local_access_rule SET "
            "time_enabled=?,count_enabled=?,amount_enabled=?,duration_enabled=?,control_elevator=?,"
            "remote_call_enabled=?,card_password_enabled=?,card_password=?,count_total=?,count_add_"
            "count=0,amount_total=?,amount_add_amount=0,amount_unit_price=?,duration_start=?,"
            "duration_end=?,elevator_value_type=?,remote_call_current_day_count=COALESCE(remote_"
            "call_current_day_count,?),remote_call_every_day_count=?,raw_json=? WHERE person_id=?",
            {rules.value("time").toBool(),
             rules.value("count").toBool(),
             rules.value("amount").toBool(),
             rules.value("duration").toBool(),
             rules.value("controlElevator").toBool(),
             rules.value("remoteCall").toBool(),
             rules.value("cardPassword").toBool(),
             pass,
             countTotal,
             amountTotal,
             price,
             start,
             end,
             type,
             remote,
             daily,
             WebSql::json(rules),
             id});
        WebSql::execute("INSERT OR IGNORE INTO "
                        "local_access_usage(person_id,remaining_count,access_count,remaining_"
                        "amount,used_amount,updated_at) VALUES(?,?,0,?,0,?)",
                        {id, countTotal, amountTotal, now()});
        WebSql::execute("UPDATE local_access_usage SET "
                        "remaining_count=COALESCE(remaining_count,?)+?,remaining_amount=COALESCE("
                        "remaining_amount,?)+?,updated_at=? WHERE person_id=?",
                        {countTotal, countAdd, amountTotal, amountAdd, now(), id});
    } else
        throw WebError(404, "本地凭证操作不存在");
    const auto out = reply(credential(id));
    if (args.contains("_operation"))
        finishJob(args.value("_operation").toString(), out, 200, {}, true);
    tx.commit();
    if (!removedPhoto.isEmpty() && QFileInfo(removedPhoto).fileName().startsWith("web_") &&
        WebSql::query("SELECT id FROM face_feature WHERE image_path=?", {removedPhoto}).isEmpty()) {
        try {
            WebPaths::inside(removedPhoto,
                             QDir(Rk3566Platform::applicationRoot()).filePath("enrollments"),
                             true);
            QFile::remove(removedPhoto);
        } catch (const WebError &) {
        }
    }
    emit personnelChanged();
    return out;
}

/** @brief 接收既有推理线程结果，复核图库冲突并追加单张本地人脸。 */
bool WebLocalApi::applyFaces(const QString &token,
                             const QJsonArray &accepted,
                             const QJsonArray &failed)
{
    if (!faceLeases_.contains(token))
        return false;
    try {
        const auto row =
            WebSql::query("SELECT owner,args FROM web_local_operations WHERE id=? AND kind='face'",
                          {token})
                .first();
        const auto args = parsed(row.value("args"));
        const auto id = personId(args);
        person(id);
        if (!failed.isEmpty() || accepted.isEmpty())
            throw WebError(422,
                           failed.isEmpty()
                               ? "人脸未通过校验"
                               : failed.first().toObject().value("message").toString());
        const auto f = accepted.first().toObject();
        QVector<FaceRecord> local, network;
        WebFacePolicy::currentGalleries(&local, &network);
        QJsonArray verified, rejected;
        WebFacePolicy::filter(accepted,
                              local,
                              network,
                              float(f.value("duplicateThreshold").toDouble()),
                              &verified,
                              &rejected);
        if (verified.isEmpty() || !rejected.isEmpty())
            throw WebError(409, "人脸与当前图库冲突，请重新核对");

        // 复用已提取特征和存储配额；只追加当前人脸，不使用批次覆盖入口。
        const auto feature = QByteArray::fromBase64(f.value("featureBase64").toString().toLatin1());
        if (feature.isEmpty())
            throw WebError(422, "人脸特征无效");
        QFile source(args.value("imagePath").toString());
        if (!source.open(QIODevice::ReadOnly))
            throw WebError(503, "人脸原图不可读");
        const auto dir = QDir(Rk3566Platform::applicationRoot()).filePath("enrollments");
        if (!QDir().mkpath(dir))
            throw WebError(503, "注册照目录不可用");
        const auto path = QDir(dir).filePath("web_" + token + ".jpg");
        QString error;
        if (!StoragePolicy::writeRegistrationPhoto(path, source.readAll(), &error))
            throw WebError(503, error);
        // Append a single face. Do not call Excel's replace-all-faces entry.
        try {
            WebSql::execute("BEGIN IMMEDIATE");
            person(id);
            change(id, args);
            local.clear();
            network.clear();
            verified = QJsonArray{};
            rejected = QJsonArray{};
            WebFacePolicy::currentGalleries(&local, &network);
            WebFacePolicy::filter(accepted,
                                  local,
                                  network,
                                  float(f.value("duplicateThreshold").toDouble()),
                                  &verified,
                                  &rejected);
            if (verified.isEmpty() || !rejected.isEmpty())
                throw WebError(409, "人脸与当前图库冲突，请重新核对");
            WebSql::execute("INSERT INTO "
                            "face_feature(person_id,image_path,feature_blob,feature_size,model_"
                            "version,quality,created_at) VALUES(?,?,?,?,?,?,?)",
                            {id,
                             path,
                             feature,
                             feature.size(),
                             f.value("modelVersion").toString(),
                             f.value("faceQuality").toDouble(),
                             now()});
            QStringList paths;
            for (const auto &r :
                 WebSql::query("SELECT image_path FROM face_feature WHERE person_id=?", {id}))
                paths.append(r.value("image_path").toString());
            WebSql::execute("UPDATE person SET has_face=1,face_file_list=?,updated_at=? WHERE id=?",
                            {paths.join(','), now(), id});
            finishJob(token, credential(id), 200, {}, true);
            if (!DbStore::transactionCommit())
                throw WebError(503, "人脸提交失败");
        } catch (...) {
            DbStore::transactionRollback();
            QFile::remove(path);
            throw;
        }
        QFile::remove(args.value("imagePath").toString());
        emit personnelChanged();
    } catch (const WebError &e) {
        finishJob(token, {}, e.status, e.message);
    }
    faceLeases_.remove(token);
    LocalImportGate::release();
    return true;
}

/** @brief 读取公共梯控配置，或保存楼层/模式后通过本地 IO 异步发送。 */
QJsonObject WebLocalApi::elevator(const QString &operation, const QJsonObject &args, int owner)
{
    const QString key = "online_v1_floor_limit_bits";
    auto bits = DbStore::getConfig(key, QString(128, '0')).toString().toLatin1();
    if (bits.size() != 128 ||
        !QRegularExpression("^[01]{128}$").match(QString::fromLatin1(bits)).hasMatch())
        throw WebError(503, "设备楼层状态格式无效");
    const auto floors = [&]() {
        QJsonArray a;
        for (int n = -8; n <= 120; ++n) {
            if (!n)
                continue;
            const int bit = n < 0 ? n + 128 : n - 1;
            a.append(QJsonObject{{"floor", n}, {"status", bits.at(bit) == '1' ? 1 : 0}});
        }
        return a;
    };
    if (operation == "elevator.floorLimit.get")
        return reply(floors(),
                     {{"stateSource", "device_config"}, {"hardwareConfirmed", QJsonValue::Null}});
    if (operation == "elevator.floorSchedule.get") {
        const auto raw = DbStore::getConfig("online_v1_floor_schedules", "[]").toString().toUtf8();
        QJsonParseError e;
        const auto doc = QJsonDocument::fromJson(raw, &e);
        if (e.error != QJsonParseError::NoError || !doc.isArray())
            throw WebError(503, "定时梯控规则格式无效");
        return reply(QJsonObject{{"platformSchedules", QJsonValue::Null},
                                 {"deviceSchedules", doc.array()},
                                 {"matched", QJsonValue::Null},
                                 {"comparisonScope", "local"},
                                 {"writable", false}});
    }
    if (operation.startsWith("elevator.floorSchedule."))
        throw WebError(501,
                       "定时梯控执行器仍归属原MQTT模块；本轮保持原模块不动，仅提供真实规则读取");
    const QStringList modeKeys{"masterSwitch",
                               "delay",
                               "parking",
                               "driver",
                               "independent",
                               "ext1",
                               "ext2",
                               "ext3",
                               "ext4"};
    auto mode = parsed(DbStore::getConfig("online_v1_elevator_mode", "{}"));
    for (const auto &k : modeKeys) {
        const auto v = mode.value(k);
        mode.insert(k, v.isBool() ? v.toBool() : v.toInt() == 1);
    }
    if (operation == "elevator.elevatorMode.get")
        return reply(mode,
                     {{"stateSource", "device_config"}, {"hardwareConfirmed", QJsonValue::Null}});
    if (operation.startsWith("elevator.screenSleep."))
        throw WebError(501, "无电梯信号休眠尚未接入实际显示控制");
    if (operation != "elevator.floorLimit.set" && operation != "elevator.elevatorMode.set")
        throw WebError(404, "梯控操作不存在");
    if (!WebLocalRuntime::instance()->capabilities().value("elevatorWrite").toBool())
        throw WebError(503, "本地RS485控制未就绪");

    // 先验证楼层或完整模式开关，再登记幂等任务和保存配置。
    QHash<int, int> changes;
    if (operation == "elevator.floorLimit.set") {
        allowed(args, {"floors"});
        if (!args.value("floors").isArray() || args.value("floors").toArray().isEmpty() ||
            args.value("floors").toArray().size() > 128)
            throw WebError(400, "请选择有效楼层");
        for (const auto &v : args.value("floors").toArray()) {
            const auto f = v.toObject();
            allowed(f, {"floor", "action"});
            const int n = int(integer(f.value("floor"), -8, 120, "楼层")),
                      action = int(integer(f.value("action"), 0, 1, "动作"));
            if (!n)
                throw WebError(400, "楼层不能为0");
            if (changes.contains(n) && changes.value(n) != action)
                throw WebError(400, "同一楼层动作冲突");
            changes.insert(n, action);
        }
    } else {
        allowed(args, modeKeys);
        for (const auto &k : modeKeys) {
            const auto v = args.value(k);
            if (!v.isBool() && !(v.isDouble() && (v.toDouble() == 0 || v.toDouble() == 1)))
                throw WebError(400, "请提交完整模式开关");
            mode.insert(k, v.isBool() ? v.toBool() : v.toInt() == 1);
        }
    }
    const auto token = beginJob(operation, args, owner);
    const auto record = job(token, owner);
    if (record.value("state") != "PENDING" || !record.value("result").toObject().isEmpty())
        return reply(record);
    // Metadata is persisted before sending. UI shows saved/sent separately.
    try {
        WebSql::execute("BEGIN IMMEDIATE");
        bits = DbStore::getConfig(key, QString(128, '0')).toString().toLatin1();
        if (bits.size() != 128 ||
            !QRegularExpression("^[01]{128}$").match(QString::fromLatin1(bits)).hasMatch())
            throw WebError(503, "设备楼层状态格式无效");
        for (auto it = changes.cbegin(); it != changes.cend(); ++it)
            bits[it.key() < 0 ? it.key() + 128 : it.key() - 1] = it.value() ? '1' : '0';
        WebSql::execute("INSERT OR REPLACE INTO config(key,value) VALUES(?,?)",
                        {key, QString::fromLatin1(bits)});
        if (operation == "elevator.elevatorMode.set")
            WebSql::execute(
                "INSERT OR REPLACE INTO config(key,value) VALUES('online_v1_elevator_mode',?)",
                {WebSql::json(mode)});
        WebSql::execute("UPDATE web_local_operations SET result=? WHERE id=?",
                        {QString("{\"dispatched\":true}"), token});
        if (!DbStore::transactionCommit())
            throw WebError(503, "梯控配置保存失败");
    } catch (const WebError &error) {
        DbStore::transactionRollback();
        finishJob(token, {}, error.status, error.message);
        throw;
    }

    // 把业务楼层开放位转换为原板卡限行位序，再叠加特殊模式。
    QByteArray restrictions(128, '1');
    for (int n = -8; n <= 120; ++n) {
        if (!n)
            continue;
        restrictions[n < 0 ? n + 8 : n + 7] = bits.at(n < 0 ? n + 128 : n - 1) == '1' ? '0' : '1';
    }
    const auto frame =
        Rs485FloorFrameBuilder::applyElevatorModeOverlay(QByteArray("OPEN") + restrictions);
    emit WebLocalRuntime::instance()->requested(token,
                                                "rs485",
                                                {{"frame", QString::fromLatin1(frame.toBase64())}});
    QTimer::singleShot(8000, this, [this, token]() {
        finishJob(
            token,
            {{"saved", true}, {"transportSent", false}, {"hardwareConfirmed", QJsonValue::Null}},
            504,
            "串口执行结果超时，请核对实际设备状态");
    });
    return reply(job(token, owner));
}

/** @brief 分派标准本地业务指令，处理能力缺口、幂等写入与异步任务提交。 */
QJsonObject WebLocalApi::dispatch(const QString &operation, const QJsonObject &args, int owner)
{
    if (operation == "auth.changePassword")
        return reply(WebAccounts::changePassword(owner,
                                                 args.value("oldPassword").toString(),
                                                 args.value("newPassword").toString()));

    // 标准视频流按实测大小收口，后续校验和发布继续走原上传任务。
    if (operation == "media.received") {
        const auto rows =
            WebSql::query("SELECT part_path,expected_bytes FROM web_upload_tasks WHERE id=? AND "
                          "owner_id=? AND kind='video' AND state='RECEIVING'",
                          {args.value("id").toString(), owner});
        if (rows.isEmpty())
            throw WebError(404, "视频接收任务不存在");
        const auto size = integer(args.value("size"),
                                  1,
                                  rows.first().value("expected_bytes").toLongLong(),
                                  "视频长度");
        const auto path = rows.first().value("part_path").toString();
        WebPaths::mounted(config_.media, config_.mount);
        WebPaths::inside(path, config_.media, true);
        if (!QFileInfo(path).isFile() || QFileInfo(path).size() != size)
            throw WebError(400, "实际视频长度不一致");
        WebSql::execute("UPDATE web_upload_tasks SET expected_bytes=? WHERE id=? AND owner_id=? "
                        "AND state='RECEIVING'",
                        {size, args.value("id").toString(), owner});
        return reply(QJsonObject{});
    }

    // 图片资源句柄仅在 Qt 与 HTTP 之间使用，并校验账号归属和存活时间。
    if (operation == "face.stage") {
        collectAssets();
        if (WebSql::query("SELECT id FROM web_local_assets").size() >= 16)
            throw WebError(409, "图片接收任务繁忙");
        qint64 used = 0;
        for (const auto &f :
             QDir(QDir(config_.imports).filePath("_local")).entryInfoList(QDir::Files))
            used += f.size();
        if (used > 58LL * 1024 * 1024)
            throw WebError(507, "本地图片暂存空间繁忙");
        const auto id = uuid(),
                   path = QDir(config_.imports).filePath("_local/asset_" + id + ".jpg");
        WebSql::execute("INSERT INTO web_local_assets(id,owner,path,created_at) VALUES(?,?,?,?)",
                        {id, owner, path, QDateTime::currentMSecsSinceEpoch()});
        return reply(QJsonObject{{"assetId", id}, {"path", path}});
    }
    if (operation == "face.discard") {
        for (const auto &r :
             WebSql::query("SELECT path FROM web_local_assets WHERE id=? AND owner=?",
                           {args.value("assetId").toString(), owner}))
            QFile::remove(r.value("path").toString());
        WebSql::execute("DELETE FROM web_local_assets WHERE id=? AND owner=?",
                        {args.value("assetId").toString(), owner});
        return reply(QJsonObject{});
    }

    // 只读接口直接采集本地源数据，不查询 Broker 或 MQTT 回执。
    if (operation == "device.capabilities")
        return reply(capabilities());
    if (operation == "device.overview")
        return reply(overview());
    if (operation == "operations.get")
        return reply(job(args.value("id").toString(), owner));
    if (operation == "person.list") {
        const int offset =
            args.contains("offset") ? int(integer(args.value("offset"), 0, 100000000, "偏移")) : 0;
        const int limit =
            args.contains("limit") ? int(integer(args.value("limit"), 1, 500, "页大小")) : 100;
        QJsonArray people;
        for (const auto &r : WebSql::query(
                 "SELECT id FROM person WHERE deleted=0 ORDER BY id DESC LIMIT ? OFFSET ?",
                 {limit, offset}))
            people.append(person(r.value("id").toLongLong()));
        const int total = WebSql::query("SELECT COUNT(*) AS n FROM person WHERE deleted=0")
                              .first()
                              .value("n")
                              .toInt();
        return reply(people,
                     {{"offset", offset},
                      {"limit", limit},
                      {"total", total},
                      {"hasMore", offset + people.size() < total}});
    }
    if (operation == "person.detail")
        return reply(person(personId(args)));
    if (operation == "credential.get")
        return reply(credential(personId(args)));

    // 成功项取真实特征表，待处理和失败项取持久化的本地人脸任务。
    if (operation == "person.faceRegisterResult") {
        const auto id = personId(args);
        auto faces = credential(id).value("faces").toArray();
        QJsonArray results;
        int pending = 0, failed = 0;
        for (const auto &v : faces) {
            auto f = v.toObject();
            f.insert("result", 1);
            f.insert("msg", "");
            results.append(f);
        }
        for (const auto &r :
             WebSql::query("SELECT id,args,state,message FROM web_local_operations WHERE "
                           "kind='face' AND state<>'APPLIED' ORDER BY created_at DESC LIMIT 200")) {
            const auto a = parsed(r.value("args"));
            if (a.value("personId").toVariant().toLongLong() != id)
                continue;
            const bool wait = r.value("state") == "PENDING";
            if (wait)
                ++pending;
            else
                ++failed;
            QJsonObject f{{"faceId", r.value("id").toString()},
                          {"result", wait ? QJsonValue(QJsonValue::Null) : QJsonValue(0)},
                          {"msg", wait ? QString("等待特征提取") : r.value("message").toString()},
                          {"registerTime", QJsonValue::Null}};
            const auto path = a.value("imagePath").toString();
            if (QFileInfo::exists(path)) {
                WebPaths::inside(path, config_.imports, true);
                f.insert("_assetPath", path);
            }
            results.append(f);
        }
        return reply(QJsonObject{{"total", results.size()},
                                 {"successCount", faces.size()},
                                 {"failCount", failed},
                                 {"pendingCount", pending},
                                 {"results", results}});
    }
    if (operation == "device.networkConfig.get") {
        QString error;
        const auto value = NetworkConfig::read(config_.networkConfig, &error);
        if (!error.isEmpty())
            throw WebError(503, error);
        return reply(value);
    }
    if (operation == "device.networkConfig.set") {
        allowed(args, {"dhcp", "ip", "netmask", "gateway", "dns"});
        QString error;
        const auto saved = NetworkConfig::read(config_.networkConfig, &error);
        if (!error.isEmpty())
            throw WebError(503, error);
        QJsonObject value;
        if (!NetworkConfig::normalize(args, saved, &value, &error))
            throw WebError(400, error);
        const auto token = beginJob("networkConfig", args, owner);
        const auto current = job(token, owner);
        const auto dispatched =
            WebSql::query("SELECT result FROM web_local_operations WHERE id=?", {token})
                .first()
                .value("result")
                .toString();
        if (current.value("state") != "PENDING" || dispatched != "{}")
            return reply(current);
        if (!WebSql::query("SELECT id FROM web_local_operations WHERE kind='networkConfig' "
                           "AND state='PENDING' AND id<>?",
                           {token})
                 .isEmpty()) {
            finishJob(token, {}, 409, QStringLiteral("另一项网络配置正在执行"));
            throw WebError(409, QStringLiteral("另一项网络配置正在执行"));
        }
        WebSql::execute("UPDATE web_local_operations SET result=? WHERE id=?",
                        {QStringLiteral("{\"dispatched\":true}"), token});
        emit WebLocalRuntime::instance()->requested(token, "networkConfig", value);
        QTimer::singleShot(60000, this, [this, token]() {
            finishJob(token, {}, 504, QStringLiteral("网络设置执行超时，请刷新核对配置和实际地址"));
        });
        return reply(job(token, owner));
    }
    if (operation == "device.getPlayInfo") {
        const auto state = WebLocalRuntime::instance()->media();
        if (!state.value("available").toBool())
            throw WebError(503, "实际播放器尚未就绪");
        return reply(state);
    }
    if (operation.startsWith("elevator."))
        return elevator(operation, args, owner);

    // 缺少实际执行能力时明确报未接入，不保存伪状态或伪造成功。
    if (operation.startsWith("device.airControl."))
        throw WebError(501, "此设备尚未接入实际空调驱动");
    if (operation == "device.reboot")
        throw WebError(501, "此设备尚未接入实际定时开关机和唤醒能力");
    if (operation == "device.screenshot" || operation == "device.videoControl" ||
        operation == "device.linux-reboot") {
        if (operation == "device.videoControl") {
            allowed(args, {"videoType", "videoVolume", "videoUrl", "videoPath", "file"});
            const auto type = args.value("videoType").toString();
            if (!QStringList{"LIVE", "RECORDED", "NONE"}.contains(type))
                throw WebError(400, "播放类型无效");
            if (args.contains("videoVolume"))
                integer(args.value("videoVolume"), 0, 100, "音量");
            if (type == "LIVE") {
                const QUrl url(args.value("videoUrl").toString());
                if (!url.isValid() || url.host().isEmpty() ||
                    !QStringList{"rtsp", "rtmp", "rtmps", "http", "https", "udp", "srt"}.contains(
                        url.scheme()))
                    throw WebError(400, "直播地址无效");
            }
            if (type == "RECORDED" && !args.value("videoPath").toString().isEmpty()) {
                const auto name = WebPaths::filename(args.value("videoPath").toString());
                WebPaths::mounted(config_.media, config_.mount);
                WebPaths::inside(QDir(config_.media).filePath(name), config_.media, true);
                if (args.contains("file") && args.value("file") != args.value("videoPath"))
                    throw WebError(400, "播放文件字段不一致");
            }
        }
        const auto kind = operation.mid(7);
        const auto token = beginJob(kind, args, owner);
        const auto current = job(token, owner);
        // All submitted actions get a persistent dispatch mark before queuing.
        const auto dispatched =
            WebSql::query("SELECT result FROM web_local_operations WHERE id=?", {token})
                .first()
                .value("result")
                .toString();
        if (current.value("state") != "PENDING" || dispatched != "{}")
            return reply(current);
        WebSql::execute("UPDATE web_local_operations SET result=? WHERE id=?",
                        {QString("{\"dispatched\":true}"), token});
        auto request = args;
        request.remove("_key");
        if (kind == "screenshot")
            request.insert("output", QDir(config_.imports).filePath("_local/" + token + ".jpg"));
        emit WebLocalRuntime::instance()->requested(token,
                                                    kind == "linux-reboot" ? "reboot" : kind,
                                                    request);
        QTimer::singleShot(10000, this, [this, token]() {
            finishJob(token, {}, 504, "本地操作执行结果超时，请核对实际状态");
        });
        return reply(job(token, owner));
    }

    // 同步写入仍登记操作结果；重试先复用结果，避免再次充值或新增。
    if (operation.startsWith("person.") || operation.startsWith("credential.") ||
        operation.startsWith("personFloor.")) {
        if (operation == "credential.addSubItem" && args.value("subType") == "FACE")
            return mutateCredential(operation, args, owner);
        const auto token = beginJob(operation, args, owner);
        const auto current = job(token, owner);
        if (current.value("state") == "APPLIED")
            return current.value("result").toObject();
        if (current.value("state") != "PENDING")
            throw WebError(current.value("code").toInt(409), current.value("message").toString());
        auto request = args;
        request.insert("_operation", token);
        try {
            return operation == "person.add" || operation == "person.update" ||
                           operation == "person.delete"
                       ? mutatePerson(operation, request, owner)
                       : mutateCredential(operation, request, owner);
        } catch (const WebError &e) {
            finishJob(token, {}, e.status, e.message);
            throw;
        }
    }
    throw WebError(404, "本地API不存在");
}
