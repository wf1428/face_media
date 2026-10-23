/**
 * @file web_service.cpp
 * @brief Web 命令协调；不链接 HTTP 库、不控制播放/空调/电梯、不发送 MQTT。
 * @author Du lin
 * @date 2026-10-09
 * @note 最近修改：2026-10-23；修改人：Du lin；增加人员图片归档上传及异步展开，复用原账号匹配和防重入库，保留媒体限制及存储空间检查。
 */

#include "web_service.h"
#include "common/media_content_policy.h"
#include "platform/rk3566_platform.h"
#include "web_resource_update.h"
#include "web_hotspot.h"
#include "web_local_api.h"
#include "web_local_runtime.h"
#include "web_paths.h"
#include "web_face_policy.h"
#include "local_import_gate.h"
#include "components/multimedia/gst_video_validation.h"
#include "common/sql/web/web_sql.h"
#include "common/sql/web/web_schema.h"
#include "common/sql/web/web_account_repository.h"
#include "common/sql/web/web_upload_repository.h"
#include "common/sql/web/web_import_repository.h"
#include "common/sql/web/web_media_repository.h"
#include "common/sql/web/web_person_repository.h"
#include "common/sql/local_personnel_store.h"
#include "common/storage_policy.h"
#include "modules/facegate/FaceGateQt/import/PersonXlsParser.h"
#include "modules/facegate/FaceGateQt/import/PersonXlsxParser.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QImageReader>
#include <QRegularExpression>
#include <QSet>
#include <QStorageInfo>
#include <QTimer>
#include <QUuid>
#include <QDebug>
#include <QJsonDocument>
#include <QStandardPaths>
#include <memory>
#ifdef Q_OS_UNIX
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace {

constexpr qint64 MiB = 1024LL * 1024; /**< 文件资源预算单位。 */

/**
 * @brief 生成用于任务或批次的随机 UUID。
 * @return 不带大括号的随机受控标识。
 */
QString uuid()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

/**
 * @brief 按原顺序将字符串列表转换为 JSON 数组。
 * @param values 按占位符顺序绑定的参数。
 * @return 字符串清单的 JSON 数组。
 */
QJsonArray strings(const QStringList &values)
{
    QJsonArray result;
    for (const auto &v : values)
        result.append(v);
    return result;
}

/**
 * @brief 不保留内部磁盘路径的业务失败数组追加。
 * @param[in,out] result 处理结果。
 * @param faces 待校验的人脸条目，含人员及资源信息。
 */
void appendFailures(QJsonObject &result, const QJsonArray &faces)
{
    QJsonArray failures = result.value("failedNames").toArray();
    for (const auto &value : faces) {
        const auto face = value.toObject();
        const QString item = face.value("personName").toString() + QStringLiteral("（") +
                             face.value("message").toString() + QStringLiteral("）");
        if (!failures.contains(item))
            failures.append(item);
    }
    result.insert("failedNames", failures);
    result.insert("failed", failures.size());
}
} // namespace

/**
 * @brief 保存 Web 业务配置并绑定到 Qt 对象生命周期。
 * @param config 设备受控配置，包含运行所需路径或功能设置。
 * @param parent Qt 父对象；对象销毁由父子关系管理，空指针表示无父对象。
 */
WebService::WebService(const WebServiceConfig &config, QObject *parent)
    : QObject(parent), config_(config)
{
}

/**
 * @brief 等待文件工作线程安全退出，再释放尚未结束的本地人员导入互斥门。
 */
WebService::~WebService()
{
    if (faceArchiveWorker_) {
        faceArchiveWorker_->requestInterruption();
        faceArchiveWorker_->wait();
    }
    if (resourceWorker_) {
        resourceWorker_->requestInterruption();
        resourceWorker_->wait(); // 等待安全检查点回滚，避免销毁仍在运行的线程。
    }
    if (importLease_)
        LocalImportGate::release();
}

/**
 * @brief 初始化统一数据库、工厂账号及中断任务恢复。
 */
void WebService::initialize()
{
    if (!DbStore::bootstrap(config_.database))
        throw WebError(503, QStringLiteral("统一数据库无法打开"));
    WebSchema::migrate();
    WebAccounts::provision(config_.factory);
    if (!QDir().mkpath(config_.imports) || QFileInfo(config_.imports).isSymLink())
        throw WebError(503, QStringLiteral("人员上传缓存目录不可用"));
    WebImports::recover();
    recover();
    if (!config_.networkConfig.isEmpty())
        hotspot_ = new WebHotspot(config_.networkConfig,
                                  config_.hotspotHelper,
                                  config_.webReadyFile,
                                  this);
    if (!local_) {
        local_ = new WebLocalApi(config_, this);
        local_->initialize();
        connect(local_, &WebLocalApi::personnelChanged, this, &WebService::personnelChanged);
        connect(local_,
                &WebLocalApi::faceValidationRequested,
                this,
                &WebService::faceValidationRequested);
    }
}

/**
 * @brief 生成不包含服务器内部路径的上传任务响应。
 * @param task 持久化上传任务记录。
 * @return 不暴露内部磁盘路径的上传任务状态。
 */
QJsonObject WebService::publicTask(const QVariantMap &task) const
{
    QJsonObject result = WebSql::object(task);
    result.remove("part_path");
    result.remove("final_path");
    result.remove("owner_id");
    if (task.value("kind") == "resource_package" && task.value("state") == "READY") {
        QFile status(task.value("part_path").toString() + ".result.json");
        if (status.open(QIODevice::ReadOnly)) {
            const auto details = QJsonDocument::fromJson(status.readAll()).object();
            for (const QString &key : {QStringLiteral("updatedDirectories"),
                                       QStringLiteral("restartScheduled"),
                                       QStringLiteral("restartError")})
                if (details.contains(key))
                    result.insert(key, details.value(key));
        }
    }
    return result;
}

/**
 * @brief 检查目标存储空间和上传预留配额。
 * @return 与 MQTT 相同的 pic 保存目录。
 */
QString WebService::pictureDirectory() const
{
    return QDir::cleanPath(QDir(config_.media).filePath("../pic"));
}

/**
 * @brief 使用任务创建时冻结的目标目录，避免后续切换小图设置改变上传位置。
 * @param task 持久化上传任务记录。
 * @return 任务固定的 pic 或 video 目标根目录。
 */
QString WebService::taskRoot(const QVariantMap &task) const
{
    const QString kind = task.value("kind").toString();
    if (kind == "ad_image" && MediaContentPolicy::isInDirectory(task.value("final_path").toString(), pictureDirectory()))
        return pictureDirectory();
    return kind == "video" || kind == "ad_image" ? config_.media : config_.imports;
}

/**
 * @brief 校验已占用空间、未完成上传预留及新增资源所需字节。
 * @param kind 资源或任务类型，用于选择目录、配额和校验规则。
 * @param incoming 本次新增资源大小，单位为字节。
 */
void WebService::checkBudget(const QString &kind, qint64 incoming)
{
    if (kind == "resource_package") {
        QStorageInfo storage(config_.imports);
        storage.refresh();
        if (incoming < 0 || incoming > 512 * MiB || !storage.isReady() ||
            storage.bytesAvailable() < incoming + 64 * MiB)
            throw WebError(507, QStringLiteral("资源包存储空间不足"));
        return; // 展开空间由独立进程按目标 eMMC 的实际剩余空间逐项核对。
    }
    const bool video = kind == "video" || kind == "ad_image";
    const QString root = video ? config_.media : config_.imports;
    if (video)
        WebPaths::mounted(root, config_.mount);
    const qint64 used =
        video ? StoragePolicy::mediaUsage().bytes + WebPaths::usage(pictureDirectory())
              : WebPaths::usage(root) - WebPaths::usage(QDir(root).filePath("resource-packages"));
    const qint64 reserved =
        WebUploads::reserved(video ? QStringLiteral("video") : QStringLiteral("person"));
    const qint64 limit = video ? StoragePolicy::mediaLimitBytes() : 200 * MiB;
    QStorageInfo storage(root);
    storage.refresh();
    if (incoming < 0 || used + reserved + incoming > limit || !storage.isReady() ||
        storage.bytesAvailable() < reserved + incoming + 64 * MiB)
        throw WebError(507, QStringLiteral("设备可用空间或上传配额不足"));
}

/**
 * @brief 校验清单和容量后创建受控上传任务。
 * @param args 操作对应的结构化参数。
 * @param owner 已认证 Web 账号主键，用于任务归属校验。
 * @return 新建或复用的上传任务及受控暂存路径。
 * @note 视频最多 500 MiB，广告图片及人员图片包最多 100 MiB；仍由 checkBudget 核对配额及实际空间。
 */
QJsonObject WebService::createUpload(const QJsonObject &args, int owner)
{
    // 先校验客户端声明的类别、原始文件名和长度，避免非法任务占用配额。
    const QString kind = args.value("kind").toString(),
                  name = WebPaths::filename(args.value("name").toString());
    const double sizeValue = args.value("size").toDouble(-1);
    if (sizeValue <= 0 || sizeValue > 8.0 * 1024 * 1024 * 1024)
        throw WebError(400, QStringLiteral("文件大小不符合要求"));
    const qint64 size = static_cast<qint64>(sizeValue);
    const bool advertisement = kind == "video" || kind == "ad_image";
    const qint64 maxSize = kind == "video" ? 500 * MiB
                           : (kind == "ad_image" || kind == "person_face_archive") ? 100 * MiB
                           : kind == "resource_package" ? 512 * MiB
                           : kind == "person_excel" ? 20 * MiB : 10 * MiB;
    if (!WebPaths::accepts(kind, name) || size <= 0 || sizeValue != double(size) || size > maxSize)
        throw WebError(400, QStringLiteral("文件类型或大小不符合要求"));

    // 限制同时待上传任务，避免大量小任务挤占设备资源。
    const auto active = WebUploads::incomplete();
    if (active.size() >= 8)
        throw WebError(429, QStringLiteral("待上传任务过多，请先完成或取消"));
    for (const auto &task : active) {
        const QString activeKind = task.value("kind").toString();
        if ((kind == "resource_package" &&
             (activeKind == "resource_package" || activeKind == "video" || activeKind == "ad_image")) ||
            (advertisement && activeKind == "resource_package"))
            throw WebError(409,
                           QStringLiteral("资源包更新与视频上传不能同时进行，请先完成当前任务"));
        if (advertisement && (activeKind == "video" || activeKind == "ad_image"))
            throw WebError(409, QStringLiteral("已有视频上传任务，请先完成或取消"));
    }
    QString root = config_.media, target;
    QVariant batch;
    const QString id = uuid(), suffix = QFileInfo(name).suffix().toLower();
    if (kind == "resource_package") {
        if (!WebResourceUpdate::available())
            throw WebError(503, QStringLiteral("Qt 资源解压库不可用，请检查系统 libarchive"));
        if (!QFileInfo(config_.resourceControl).isFile() ||
            !QFileInfo(config_.resourceControl).isExecutable() ||
            config_.resourceRestartPrefix.isEmpty() ||
            QStandardPaths::findExecutable(config_.resourceRestartPrefix.first()).isEmpty())
            throw WebError(
                503,
                QStringLiteral(
                    "资源更新后的 Qt 重启入口不可用，请检查 weston-control 和 systemd-run"));
        root = QDir(config_.imports).filePath("resource-packages");
        if (!QDir().mkpath(root))
            throw WebError(503, QStringLiteral("无法创建资源包缓存目录"));
        WebPaths::uploadDirectory(root);
        target = QDir(root).filePath(id + '.' + suffix);
    } else if (!advertisement) {
        if (kind == "person_face_archive" && !WebResourceUpdate::available())
            throw WebError(503, QStringLiteral("图片压缩包解压库不可用"));
        // 人员文件必须属于当前账号的开放批次，并与预先提交的清单逐项一致。
        batch = args.value("batch_id").toString();
        const auto import = WebImports::get(batch.toString(), owner);
        if (import.value("state") != "OPEN")
            throw WebError(409, QStringLiteral("导入批次已经封存"));
        bool declared = false;
        for (const auto &item : import.value("result").toObject().value("manifest").toArray()) {
            const auto file = item.toObject();
            if (file.value("name").toString() == name && file.value("kind") == kind &&
                file.value("size").toDouble() == sizeValue)
                declared = true;
        }
        if (!declared)
            throw WebError(400, QStringLiteral("文件与本批次所选清单不一致"));
        const auto entries = WebSql::query(
            QStringLiteral("SELECT original_name,kind,state FROM web_upload_tasks WHERE batch_id=? "
                           "AND state NOT IN ('FAILED','CANCELLED')"),
            {batch});
        if (entries.size() >= 500)
            throw WebError(400, QStringLiteral("每批最多 500 份文件"));
        for (const auto &entry : entries) {
            if (entry.value("original_name").toString().compare(name, Qt::CaseInsensitive) == 0 ||
                (kind == "person_excel" && entry.value("kind") == "person_excel") ||
                (kind == "person_face_archive" && entry.value("kind") == "person_face_archive"))
                throw WebError(409, QStringLiteral("同一批次不能重复上传同名文件或多个 Excel"));
        }
        root = QDir(config_.imports).filePath(batch.toString());
        if (!QDir().mkpath(root))
            throw WebError(503, QStringLiteral("无法创建批次目录"));
        WebPaths::inside(QDir(root).filePath("check"), config_.imports);
        WebPaths::uploadDirectory(root);
        target = QDir(root).filePath(id + '.' + suffix);
    } else {
        // 视频只写入媒体挂载点；发布前使用同挂载点内的临时目录。
        if (kind == "ad_image" && (args.contains("smallImageMode")
                ? MediaContentPolicy::smallImageMode(args.value("smallImageMode"))
                : MediaContentPolicy::smallImageMode(Rk3566Platform::netConfigPath()))) {
            root = pictureDirectory();
            WebPaths::inside(root, QFileInfo(config_.media).absolutePath());
            if (!QDir().mkpath(root)) throw WebError(503, "无法创建小图目录");
        }
        WebPaths::mounted(root, config_.mount);
        target = QDir(root).filePath(name);
        if (QFileInfo::exists(target))
            throw WebError(409, QStringLiteral("同名视频已存在，请更换名称"));
        if (!QDir().mkpath(QDir(root).filePath("_uploads")))
            throw WebError(503, QStringLiteral("无法创建视频缓存目录"));
        WebPaths::inside(QDir(root).filePath("_uploads/check"), root);
        WebPaths::uploadDirectory(QDir(root).filePath("_uploads"));
    }

    // 路径和空间确认后才在数据库中登记任务。
    const QString part =
        advertisement ? QDir(root).filePath("_uploads/" + id + ".part") : target + ".part";
    WebPaths::inside(target, advertisement ? root : config_.imports);
    WebPaths::inside(part, advertisement ? root : config_.imports);
    checkBudget(kind, size);
    const QVariantMap task = {{"id", id},
                              {"owner_id", owner},
                              {"kind", kind},
                              {"batch_id", batch},
                              {"original_name", name},
                              {"expected_bytes", size},
                              {"part_path", part},
                              {"final_path", target}};
    WebUploads::create(task);
    auto result = publicTask(WebUploads::get(id, owner));
    result.insert("part_path", part);
    return result;
}

/**
 * @brief 仅提示真实视频上传任务；完整接收不等于内容校验和发布完成。
 * @param task 持久化上传任务记录。
 * @param state 实际执行状态。
 */
void WebService::reportVideoUpload(const QVariantMap &task, const QString &state)
{
    if (task.value("kind").toString() == "resource_package") {
        const auto total = task.value("expected_bytes").toLongLong();
        const auto bytes = task.value("received_bytes").toLongLong();
        const int percent = total > 0 ? qBound(0, int(30.0 * bytes / total), 30) : 0;
        emit WebLocalRuntime::instance()->resourceProgress(
            task.value("id").toString(),
            state == "FAILED"       ? QStringLiteral("资源包接收中断")
            : state == "VALIDATING" ? QStringLiteral("正在检查资源包")
                                    : QStringLiteral("正在接收资源包"),
            percent,
            state == "FAILED");
        return;
    }
    if (task.value("kind").toString() != "video" && task.value("kind").toString() != "ad_image")
        return;
    const auto total = task.value("expected_bytes").toLongLong();
    const auto received = task.value("received_bytes").toLongLong();
    const int percent = state == "COMPLETED" ? 100
                        : total > 0          ? qBound(0, int(100.0 * received / total), 99)
                                             : 0;
    emit WebLocalRuntime::instance()->uploadProgress(task.value("id").toString(),
                                                     task.value("original_name").toString(),
                                                     percent,
                                                     state);
}

/**
 * @brief 验证并发布已完整接收的文件。
 * @param id 目标记录或任务的标识。
 * @param owner 已认证 Web 账号主键，用于任务归属校验。
 * @return 已完成长度、格式、同步检查后的幂等发布结果。
 */
QJsonObject WebService::commit(const QString &id, int owner)
{
    // 中断恢复可能遇到已重命名文件，此时仍需完成数据库发布。
    auto task = WebUploads::get(id, owner);
    const QString state = task.value("state").toString(), kind = task.value("kind").toString();
    if (state == "READY")
        return publicTask(task);
    if (state != "RECEIVED" && state != "VALIDATING" && state != "COMMITTING")
        throw WebError(409, QStringLiteral("上传任务尚未接收完整或已终止"));
    if (kind == "resource_package") {
        if (state == "COMMITTING")
            return publicTask(task); // 重复提交只查询，不重复覆盖资源。
        const QString part = task.value("part_path").toString();
        WebPaths::inside(part, config_.imports, true);
        if (QFileInfo(part).size() != task.value("expected_bytes").toLongLong())
            throw WebError(400, QStringLiteral("资源包接收长度不完整"));
        WebPaths::sync(part);
        return startResourcePackage(task);
    }
    const QString root = taskRoot(task);
    if (kind == "video" || kind == "ad_image")
        WebPaths::mounted(root, config_.mount);
    const QString part = task.value("part_path").toString(),
                  final = task.value("final_path").toString();
    const bool renamed =
        state == "COMMITTING" && !QFileInfo::exists(part) && QFileInfo::exists(final);
    const QString candidate = renamed ? final : part;
    WebPaths::inside(candidate, root, true);
    if (QFileInfo(candidate).size() != task.value("expected_bytes").toLongLong())
        throw WebError(400, QStringLiteral("上传文件长度不完整"));
    checkBudget(kind);
    WebUploads::setState(id, renamed ? "COMMITTING" : "VALIDATING", QFileInfo(candidate).size());
    reportVideoUpload(WebUploads::get(id, owner), "VALIDATING");

    // 按文件类别沿用 GStreamer、Excel 或图片校验方式。
    if (kind == "video") {
        const GstVideoValidationResult validation = validateVideoWithGStreamer(candidate);
        if (validation.status != GstVideoValidationStatus::Valid)
            throw WebError(validation.status == GstVideoValidationStatus::Unavailable ? 503 : 400,
                           validation.message);
    } else if (kind == "ad_image") {
        QImageReader reader(candidate);
        reader.setDecideFormatFromContent(true);
        const QSize dimensions = reader.size();
        if (!dimensions.isValid() || qint64(dimensions.width()) * dimensions.height() > 64000000 || !reader.canRead())
            throw WebError(400, "广告图片格式或尺寸无效");
        if (reader.read().isNull()) throw WebError(400, "广告图片内容无法解码");
    } else if (kind == "person_excel") {
        // 临时文件以 .part 结尾，XLS 格式按原始后缀判断，预检使用最终路径副本规则。
        const bool xls =
            task.value("original_name").toString().endsWith(".xls", Qt::CaseInsensitive);
        if (xls) {
            QFile file(candidate);
            if (!file.open(QIODevice::ReadOnly) ||
                file.read(8) != QByteArray::fromHex("d0cf11e0a1b11ae1"))
                throw WebError(400, QStringLiteral("不是有效 XLS 文件"));
        } else
            WebPaths::validateExcel(candidate);
    } else if (kind == "person_face_archive") {
        // 这里只发布已完整接收的归档；开始导入后先后台展开校验，再写人员资料。
        if (!WebResourceUpdate::available()) throw WebError(503, QStringLiteral("图片解压库不可用"));
    } else {
        WebPaths::validateFaceImage(candidate, task.value("original_name").toString());
    }

    // 先持久化文件内容，再将临时文件原子改名为最终文件。
    WebPaths::sync(candidate);
    if (!renamed) {
        if (QFileInfo::exists(final))
            throw WebError(409, QStringLiteral("目标文件已存在，拒绝覆盖"));
        WebUploads::setState(id, "COMMITTING", QFileInfo(candidate).size());
        if (!QFile::rename(part, final))
            throw WebError(503, QStringLiteral("文件发布失败，保留临时文件待重试"));
        WebPaths::sync(QFileInfo(part).absolutePath(), true);
    }
    WebPaths::sync(QFileInfo(final).absolutePath(), true);

    // 文件落盘后通过同一 Qt 数据库事务登记可见状态。
    if (!DbStore::transactionBegin())
        throw WebError(503, QStringLiteral("文件登记事务失败"));
    try {
        if (kind == "video" || kind == "ad_image")
            WebMedia::index(final, id);
        else
            WebImports::addAsset(task);
        WebUploads::setState(id, "READY", QFileInfo(final).size());
        if (!DbStore::transactionCommit())
            throw WebError(503, QStringLiteral("文件登记提交失败"));
    } catch (...) {
        DbStore::transactionRollback();
        throw;
    }
    // 只有内容校验、文件原子发布、数据库提交都成功，才通知设备播放。
    reportVideoUpload(WebUploads::get(id, owner), "COMPLETED");
    if (kind == "video" || kind == "ad_image")
        emit WebLocalRuntime::instance()->videoUploaded(final);
    return publicTask(WebUploads::get(id, owner));
}

/**
 * @brief 启动 Qt 内部更新线程，文件 IO 与解压均不占用 Web RPC 线程。
 * @param task 持久化上传任务记录。
 * @param recovery 是否处理上次中断的资源更新。
 * @return 异步资源更新任务的受理结果。
 */
QJsonObject WebService::startResourcePackage(const QVariantMap &task, bool recovery)
{
    if (resourceWorker_ && resourceWorker_->isRunning())
        throw WebError(409, QStringLiteral("已有资源更新任务正在执行"));
    const QString id = task.value("id").toString();
    const QString part = task.value("part_path").toString();
    WebPaths::inside(part, config_.imports);
    QStringList restart = config_.resourceRestartPrefix;
    restart << "--unit=ycest-web-resource-" + id << "--on-active=3s"
            << "--timer-property=AccuracySec=100ms" << "--timer-property=RemainAfterElapse=no"
            << "--collect" << config_.resourceControl << "apply-layout"
            << QDir(config_.resourceRoot).filePath("logo/ycest_cfg.ini");
    auto *worker = new WebResourceUpdate(
        {part, config_.resourceRoot, part + ".result.json", restart, recovery},
        this);
    resourceWorker_ = worker;
    WebUploads::setState(id, "COMMITTING", task.value("expected_bytes").toLongLong());
    connect(worker,
            &WebResourceUpdate::progress,
            this,
            [id](int percent, const QString &message, const QString &phase) {
                emit WebLocalRuntime::instance()->resourceProgress(
                    id,
                    message,
                    phase == "FAILED" ? 0 : 30 + 70 * percent / 100,
                    phase == "READY" || phase == "FAILED");
            });
    connect(worker, &WebResourceUpdate::completed, this, [this, id](const QJsonObject &result) {
        finishResourcePackage(id, result);
    });
    connect(worker, &QThread::finished, this, [this, worker]() {
        if (resourceWorker_ == worker)
            resourceWorker_ = nullptr;
        worker->deleteLater();
    });
    worker->start(QThread::LowPriority);
    return publicTask(WebUploads::get(id, task.value("owner_id").toInt()));
}

/**
 * @brief 解压结果回到原 Web 线程后才登记数据库，不跨线程访问 SQLite。
 * @param id 目标记录或任务的标识。
 * @param result 处理结果。
 */
void WebService::finishResourcePackage(const QString &id, const QJsonObject &result)
{
    const auto task = WebUploads::get(id);
    const QString part = task.value("part_path").toString();
    const bool ready = result.value("state") == "READY";
    WebUploads::setState(id,
                         ready ? "READY" : "FAILED",
                         task.value("expected_bytes").toLongLong(),
                         ready ? QStringLiteral("")
                               : result.value("error").toString(QStringLiteral("资源更新失败")));
    if (QFileInfo::exists(part) && !QFile::remove(part))
        qWarning() << "[WEB] resource package cache cleanup deferred" << id;
    qInfo() << "[WEB] resource package" << id << (ready ? "READY" : "FAILED")
            << (ready ? result.value("updatedDirectories") : result.value("error"));
}

/**
 * @brief 返回设备目录中可管理的视频文件清单。
 * @return video 目录的视频与图片清单。
 */
QJsonObject WebService::files()
{
    WebPaths::mounted(config_.media, config_.mount);
    QJsonArray items;
    for (const QString &directory : {config_.media, pictureDirectory()}) {
      if (!QFileInfo::exists(directory)) continue;
      WebPaths::mounted(directory, config_.mount);
      for (const auto &file : QDir(directory).entryInfoList(QDir::Files | QDir::NoSymLinks, QDir::Name)) {
        if (!WebPaths::accepts("ad_image", file.fileName()) &&
            (directory != config_.media || !WebPaths::accepts("video", file.fileName())))
            continue;
        WebPaths::inside(file.absoluteFilePath(), directory, true);

        // 若是 COMMITTING 的 Web 文件，等待其数据库发布完成再向网页展示。
        if (!WebSql::query(QStringLiteral("SELECT id FROM web_upload_tasks WHERE final_path=? AND "
                                          "state NOT IN ('READY','FAILED','CANCELLED')"),
                           {file.absoluteFilePath()})
                 .isEmpty())
            continue;
        WebMedia::index(file.absoluteFilePath());
        const auto rows = WebSql::query(
            QStringLiteral(
                "SELECT id,name,bytes,modified_ms,state FROM web_media_files WHERE path=?"),
            {file.absoluteFilePath()});
        if (!rows.isEmpty()) {
            auto item = WebSql::object(rows.first());
            item.insert("mediaType", MediaContentPolicy::isImage(file.fileName()) ? "IMAGE" : "VIDEO");
            item.insert("smallImageMode", directory == pictureDirectory() ? 1 : 0);
            items.append(item);
        }
      }
    }
    return {{"files", items}};
}

/**
 * @brief 将版本化 IPC 操作分派到对应的 Qt Web 业务。
 * @param operation 白名单业务操作名。
 * @param args 操作对应的结构化参数。
 * @param owner 已认证 Web 账号主键，用于任务归属校验。
 * @return 白名单操作的业务结果；校验或执行失败抛出 WebError。
 */
QJsonObject WebService::dispatch(const QString &operation, const QJsonObject &args, int owner)
{
    // 新 Web 连接的关键状态使用 FULL；原 Qt 连接配置不变。
    WebSql::execute(QStringLiteral("PRAGMA synchronous=FULL"));
    if (operation == "auth.login")
        return WebAccounts::authenticate(args.value("username").toString(),
                                         args.value("password").toString());
    if (operation == "health")
        return {{"ready", true}, {"version", 1}};
    if (operation == "device.info") {
        if (!hotspot_)
            throw WebError(503, QStringLiteral("设备热点配置未接入"));
        hotspot_->synchronize();
        return hotspot_->info();
    }
    if (owner <= 0 ||
        WebSql::query(QStringLiteral("SELECT id FROM web_accounts WHERE id=? AND enabled=1"),
                      {owner})
            .isEmpty())
        throw WebError(401, QStringLiteral("请先登录"));
    if (operation == "device.wifi.save") {
        if (!hotspot_)
            throw WebError(503, QStringLiteral("设备热点配置未接入"));
        for (auto it = args.begin(); it != args.end(); ++it)
            if ((it.key() != "ssid" && it.key() != "password") || !it.value().isString())
                throw WebError(400, QStringLiteral("热点配置字段无效"));
        return hotspot_->save(args.value("ssid").toString(), args.value("password").toString());
    }
    if (operation == "local.device.configPackage.upload")
        return commit(args.value("id").toString(), owner);
    if (operation == "local.resource.received") {
        const auto task = WebUploads::get(args.value("id").toString(), owner);
        const qint64 bytes = QFileInfo(task.value("part_path").toString()).size();
        if (task.value("kind") != "resource_package" || task.value("state") != "RECEIVING" ||
            bytes <= 0 || bytes > task.value("expected_bytes").toLongLong() ||
            args.value("size").toDouble() != double(bytes))
            throw WebError(400, QStringLiteral("资源包实际接收长度无效"));
        WebPaths::inside(task.value("part_path").toString(), config_.imports, true);
        WebSql::execute("UPDATE web_upload_tasks SET expected_bytes=? WHERE id=?",
                        {bytes, task.value("id")});
        return {};
    }
    if (operation.startsWith("local."))
        return local_->dispatch(operation.mid(6), args, owner);
    const QString id = args.value("id").toString();

    // 上传操作统一核对任务归属和状态，再推进接收、提交或取消。
    if (operation == "uploads.create")
        return createUpload(args, owner);
    if (operation.startsWith("uploads.")) {
        const auto task = WebUploads::get(id, owner);
        const QString state = task.value("state").toString(), kind = task.value("kind").toString();
        if (operation == "uploads.status")
            return publicTask(task);
        if (operation == "uploads.begin") {
            if (state != "CREATED")
                throw WebError(409, QStringLiteral("任务已接收或开始，不能重复传输"));
            checkBudget(kind);
            WebUploads::setState(id, "RECEIVING");
            reportVideoUpload(WebUploads::get(id, owner), "STARTED");
            return {{"part_path", task.value("part_path").toString()},
                    {"size", double(task.value("expected_bytes").toLongLong())}};
        }
        if (operation == "uploads.progress" || operation == "uploads.received") {
            if (state != "RECEIVING")
                throw WebError(409, QStringLiteral("上传任务状态不允许继续接收"));
            const qint64 bytes = QFileInfo(task.value("part_path").toString()).size();
            if (bytes > task.value("expected_bytes").toLongLong())
                throw WebError(400, QStringLiteral("接收长度超过声明大小"));
            checkBudget(kind);
            if (operation == "uploads.received" &&
                bytes != task.value("expected_bytes").toLongLong())
                throw WebError(400, QStringLiteral("文件接收不完整"));
            WebUploads::setState(id,
                                 operation == "uploads.received" ? "RECEIVED" : "RECEIVING",
                                 bytes);
            reportVideoUpload(WebUploads::get(id, owner),
                              operation == "uploads.received" ? "VALIDATING" : "PROGRESS");
            return publicTask(WebUploads::get(id, owner));
        }
        if (operation == "uploads.commit") {
            try {
                return commit(id, owner);
            } catch (...) {
                reportVideoUpload(WebUploads::get(id, owner), "FAILED");
                throw;
            }
        }
        if (operation == "uploads.cancel") {
            if (state == "READY" || state == "COMMITTING")
                throw WebError(409, QStringLiteral("任务已经发布或正在发布，不能取消"));
            const QString part = task.value("part_path").toString();
            WebPaths::inside(part, taskRoot(task));
#ifdef Q_OS_UNIX
            // daemon 崩溃而 Qt 仍运行时允许取消失去写入者的任务，活跃 flock 不可解除。
            if (state == "RECEIVING" && QFileInfo::exists(part)) {
                const int fd =
                    ::open(QFile::encodeName(part).constData(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
                if (fd < 0)
                    throw WebError(503, QStringLiteral("临时文件无法检查"));
                const bool unlocked = ::flock(fd, LOCK_EX | LOCK_NB) == 0;
                ::close(fd);
                if (!unlocked)
                    throw WebError(409, QStringLiteral("文件仍在传输，请等待连接结束"));
            }
#endif
            if (QFileInfo::exists(part) && !QFile::remove(part))
                throw WebError(503, QStringLiteral("临时文件清理失败"));
            WebUploads::setState(id, "CANCELLED");
            reportVideoUpload(task, "FAILED");
            return {{"cancelled", true}};
        }
        if (operation == "uploads.failed") {
            if (state != "RECEIVING")
                throw WebError(409, QStringLiteral("任务已停止接收"));
            const QString part = task.value("part_path").toString();
            WebPaths::inside(part, taskRoot(task));
            if (QFileInfo::exists(part) && !QFile::remove(part))
                throw WebError(503, QStringLiteral("临时文件清理失败"));
            WebUploads::setState(id, "FAILED", 0, QStringLiteral("传输未完成，请重新上传"));
            reportVideoUpload(task, "FAILED");
            return {{"failed", true}};
        }
    }

    // 文件删除会检查当前播放器实际占用的文件描述符。
    if (operation == "files.list")
        return files();
    if (operation == "persons.list")
        return WebPeople::list();
    if (operation == "persons.get")
        return WebPeople::get(id);
    if (operation == "files.get" || operation == "files.delete") {
        WebPaths::mounted(config_.media, config_.mount);
        const auto item = WebMedia::get(id);
        const QString path = item.value("path").toString();
        const QString root = MediaContentPolicy::isInDirectory(path, pictureDirectory()) ? pictureDirectory() : config_.media;
        if (root == pictureDirectory() && !MediaContentPolicy::isImage(path))
            throw WebError(400, "小图目录只管理图片");
        WebPaths::mounted(root, config_.mount);
        WebPaths::inside(path, root, operation == "files.get");
        if (operation == "files.get") {
            if (item.value("state") != "READY")
                throw WebError(409, QStringLiteral("文件正在删除"));
            return {{"path", path},
                    {"name", item.value("name").toString()},
                    {"bytes", double(QFileInfo(path).size())}};
        }
#ifdef Q_OS_LINUX
        // 原播放器保持不变；检查当前进程实际打开的文件描述符，保护正在使用的视频。
        for (const auto &fd :
             QDir("/proc/self/fd").entryInfoList(QDir::Files | QDir::System | QDir::NoDotAndDotDot))
            if (QFileInfo(fd.absoluteFilePath()).symLinkTarget() == path)
                throw WebError(423, QStringLiteral("文件正在使用，请稍后重试"));
#endif
        WebMedia::deleting(id);
        if (QFileInfo::exists(path) && !QFile::remove(path))
            throw WebError(503, QStringLiteral("删除失败，请重试"));
        WebPaths::sync(root, true);
        WebMedia::removed(id);
        emit WebLocalRuntime::instance()->mediaFileDeleted(path);
        return {{"deleted", true}};
    }

    // 导入批次的声明清单一次确定，Excel 必须恰好一份。
    if (operation == "imports.create") {
        const auto manifest = args.value("manifest").toArray();
        if (manifest.isEmpty() || manifest.size() > 500)
            throw WebError(
                400,
                QStringLiteral("请选择一份 Excel 和可选的人脸图片，每批最多 500 份文件"));
        QSet<QString> names;
        int excelCount = 0, archiveCount = 0, imageCount = 0;
        qint64 total = 0;
        for (const auto &item : manifest) {
            const auto file = item.toObject();
            const auto name = WebPaths::filename(file.value("name").toString());
            const auto kind = file.value("kind").toString();
            const double size = file.value("size").toDouble(-1);
            if ((kind != "person_excel" && kind != "person_face" && kind != "person_face_archive") ||
                !WebPaths::accepts(kind, name) || size <= 0 ||
                size > (kind == "person_excel" ? 20 * MiB : kind == "person_face_archive" ? 100 * MiB : 10 * MiB) ||
                size != double(qint64(size)) || names.contains(name.toCaseFolded()))
                throw WebError(400, QStringLiteral("所选文件清单存在格式、大小或重名问题"));
            names.insert(name.toCaseFolded());
            total += qint64(size);
            if (kind == "person_excel")
                ++excelCount;
            else if (kind == "person_face_archive") ++archiveCount;
            else ++imageCount;
        }
        if (excelCount != 1)
            throw WebError(400, QStringLiteral("每批必须包含且仅包含一份 Excel"));
        if (archiveCount > 1 || (archiveCount && imageCount))
            throw WebError(400, QStringLiteral("人脸图片请使用直接上传或单个压缩包中的一种方式"));
        checkBudget("person_excel", total);
        return WebImports::get(WebImports::create(owner, manifest), owner);
    }
    if (operation == "imports.status")
        return WebImports::get(id, owner);
    if (operation == "imports.discard") {
        if (WebImports::get(id, owner).value("state") != "OPEN")
            throw WebError(409, QStringLiteral("已封存批次不能放弃或重放"));
        for (const auto &task :
             WebSql::query(QStringLiteral("SELECT id,state FROM web_upload_tasks WHERE batch_id=? "
                                          "AND state NOT IN ('READY','FAILED','CANCELLED')"),
                           {id})) {
            if (task.value("state") == "RECEIVING" || task.value("state") == "COMMITTING")
                throw WebError(409, QStringLiteral("批次仍有文件传输或发布，请先查询上传任务"));
            dispatch("uploads.cancel", {{"id", task.value("id").toString()}}, owner);
        }
        WebImports::setState(id, "CANCELLED");
        cleanupBatch(id);
        return {{"cancelled", true}};
    }

    // 启动前核对所有清单文件已上传，并申请与 U 盘导入共用的互斥门。
    if (operation == "imports.start") {
        const auto batch = WebImports::get(id, owner);
        if (batch.value("state") != "OPEN")
            return batch; // 重复点击不重新执行人员写入。
        if (!currentBatch_.isEmpty())
            throw WebError(409, QStringLiteral("已有本地人员导入任务"));
        const auto pending =
            WebSql::query(QStringLiteral("SELECT id FROM web_upload_tasks WHERE batch_id=? AND "
                                         "state NOT IN ('READY','FAILED','CANCELLED')"),
                          {id});
        if (!pending.isEmpty())
            throw WebError(409, QStringLiteral("批次文件尚未完成上传"));
        int excelCount = 0;
        for (const auto &asset : WebImports::assets(id))
            if (asset.value("kind") == "person_excel")
                ++excelCount;
        if (excelCount != 1)
            throw WebError(400, QStringLiteral("请上传一份 Excel 人员表"));
        const auto assets = WebImports::assets(id);
        const auto manifest = batch.value("result").toObject().value("manifest").toArray();
        if (manifest.size() != assets.size())
            throw WebError(409, QStringLiteral("所选清单尚未全部上传完成，不能导入部分文件"));
        for (const auto &item : manifest) {
            const auto file = item.toObject();
            bool found = false;
            for (const auto &asset : assets)
                if (asset.value("original_name").toString() == file.value("name").toString() &&
                    asset.value("kind").toString() == file.value("kind").toString())
                    found = true;
            if (!found)
                throw WebError(409, QStringLiteral("批次上传文件与所选清单不一致"));
        }
        if (!LocalImportGate::acquire())
            throw WebError(409, QStringLiteral("U 盘或 Web 正在导入本地人员，请稍后重试"));
        importLease_ = true;
        currentBatch_ = id;
        WebImports::setState(id, "IMPORTING");
        QTimer::singleShot(0, this, [this, id]() {
            importBatch(id);
        });
        return WebImports::get(id, owner);
    }
    throw WebError(404, QStringLiteral("本版本未接入该操作"));
}

/**
 * @brief 图片包先后台展开，解析 Excel 后按原规则处理人员及账号匹配的人脸。
 * @param id 目标记录或任务的标识。
 * @param extracted 后台解压返回的图片清单；初次调用为空。
 */
void WebService::importBatch(const QString &id, const QJsonArray &extracted)
{
    if (currentBatch_ != id) return;
    try {
        // 使用 Qt 原有的 XLS/XLSX 解析器和本地人员导入入口。
        auto assets = WebImports::assets(id);
        if (extracted.isEmpty()) {
            for (const auto &asset : assets) {
                if (asset.value("kind") != "person_face_archive") continue;
                const QString archive = asset.value("path").toString();
                WebPaths::inside(archive, config_.imports, true);
                const QString destination = QDir(QFileInfo(archive).absolutePath()).filePath("face-images");
                const qint64 budget = 200 * MiB - WebPaths::usage(config_.imports)
                        + WebPaths::usage(QDir(config_.imports).filePath("resource-packages"))
                        - WebUploads::reserved("person");
                auto result = std::make_shared<QJsonObject>();
                WebImports::setState(id, "IMPORTING", {{"message", QStringLiteral("正在解压人脸图片")}});
                auto *thread = QThread::create([archive, destination, budget, result] {
                    try {
                        result->insert("images", WebResourceUpdate::extractFaceImages(archive, destination, budget));
                    } catch (const WebError &e) {
                        result->insert("error", e.message);
                    } catch (const std::exception &) {
                        result->insert("error", QStringLiteral("人员图片压缩包处理异常"));
                    }
                });
                faceArchiveWorker_ = thread;
                thread->setParent(this);
                connect(thread, &QThread::finished, this, [this, id, result, thread] {
                    faceArchiveWorker_ = nullptr;
                    thread->deleteLater();
                    if (currentBatch_ != id) return;
                    if (result->contains("error")) {
                        importResult_ = {{"message", result->value("error")}};
                        finishImport("FAILED");
                    } else importBatch(id, result->value("images").toArray());
                });
                thread->start();
                return;
            }
        }
        if (!extracted.isEmpty()) checkBudget("person_excel");
        for (const auto &image : extracted) assets.append(image.toObject().toVariantMap());
        QString excel;
        for (const auto &asset : assets)
            if (asset.value("kind") == "person_excel")
                excel = asset.value("path").toString();
        WebPaths::inside(excel, config_.imports, true);
        WebPaths::validateExcel(excel);
        const auto parsed = excel.endsWith(".xls", Qt::CaseInsensitive)
                                ? PersonXlsParser::parse(excel)
                                : PersonXlsxParser::parse(excel);
        importResult_ = {{"warnings", strings(parsed.warnings)}};
        if (!parsed.ok)
            throw WebError(400, parsed.error);
        if (parsed.records.size() > 5000)
            throw WebError(400, QStringLiteral("每批最多 5000 名人员"));
        QVector<QJsonObject> records;
        for (const auto &record : parsed.records)
            records.append(record.data);
        LocalPersonnelStore store;
        if (!store.initialize())
            throw WebError(503, QStringLiteral("本地人员资料未就绪"));
        WebPeople::verifyConflictSources();
        const auto imported = store.importSpreadsheetPersonnel(records);
        importResult_.insert("inserted", imported.inserted);
        importResult_.insert("updated", imported.updated);
        importResult_.insert("unchanged", imported.unchanged);
        importResult_.insert("failed", imported.failed);
        importResult_.insert("failedNames", strings(imported.failedNames));
        importResult_.insert("unchangedNames", strings(imported.unchangedNames));
        importResult_.insert("message", imported.error);
        importResult_.insert("savedFaces", 0);
        if (imported.inserted + imported.updated > 0)
            emit personnelChanged();

        // 只把文件名能匹配本批次成功人员的图片送入人脸校验。
        QHash<QString, LocalImportedPerson> people;
        QSet<QString> ambiguous;
        for (const auto &person : imported.importedPeople) {
            const auto key = person.personNo.trimmed().toCaseFolded();
            if (people.contains(key))
                ambiguous.insert(key);
            else
                people.insert(key, person);
        }
        QJsonArray faces, warnings = importResult_.value("warnings").toArray();
        const QRegularExpression naming(QStringLiteral("^(.+)_([0-9]+)$"));
        for (const auto &asset : assets) {
            if (asset.value("kind") != "person_face")
                continue;
            const QString original = asset.value("original_name").toString();
            const auto match = naming.match(QFileInfo(original).completeBaseName());
            const QString key = match.captured(1).trimmed().toCaseFolded();
            if (!match.hasMatch() || !people.contains(key) || ambiguous.contains(key)) {
                warnings.append(QStringLiteral("跳过未能匹配本批成功人员的图片：%1").arg(original));
                continue;
            }
            const auto person = people.value(key);
            const QString path = asset.value("path").toString();
            WebPaths::inside(path, config_.imports, true);
            faces.append(QJsonObject{{"localPersonId", QString::number(person.personId)},
                                     {"personNo", person.personNo},
                                     {"personName", person.name},
                                     {"phone", person.phone},
                                     {"imagePath", path},
                                     {"sourceFileName", original}});
        }
        importResult_.insert("warnings", warnings);
        if (faces.isEmpty()) {
            finishImport(imported.failed > 0 || !imported.error.isEmpty() ? "PARTIAL_FAILED"
                                                                          : "DONE");
            return;
        }

        // 人脸推理由原 Qt 工作线程异步完成，超时保留已成功的人员记录。
        WebImports::setState(id, "VALIDATING_FACES", importResult_);
        emit faceValidationRequested(id, faces);
        QTimer::singleShot(120000, this, [this, id]() {
            if (currentBatch_ != id)
                return;
            importResult_.insert(
                "message",
                QStringLiteral("人脸校验超时，人员成功部分已保留；请核对后新建批次"));
            finishImport("PARTIAL_FAILED");
        });
    } catch (const WebError &e) {
        importResult_.insert("message", e.message);
        finishImport("FAILED");
    } catch (const std::exception &e) {
        qWarning() << "[WEB] import exception" << e.what();
        importResult_.insert("message", QStringLiteral("人员导入异常，请核对成功部分"));
        finishImport("RECOVERY_REQUIRED");
    }
}

/**
 * @brief 写入通过校验的人脸图片及人员关联。
 * @param token 贯穿请求与异步结果的任务标识。
 * @param accepted 通过质量、防重校验的人脸条目。
 * @param failed 校验失败的人脸条目及原因。
 */
void WebService::applyFaces(const QString &token,
                            const QJsonArray &accepted,
                            const QJsonArray &failed)
{
    if (local_ && local_->applyFaces(token, accepted, failed))
        return;
    if (token != currentBatch_ || currentBatch_.isEmpty())
        return;
    try {
        WebImports::setState(token, "SAVING_FACES", importResult_);
        appendFailures(importResult_, failed);

        // 同时检查本地和网络人脸库，阈值来自原网络人员下发参数。
        QVector<FaceRecord> local, network;
        WebFacePolicy::currentGalleries(&local, &network);
        QJsonArray verified, rejected;
        const float threshold =
            accepted.isEmpty()
                ? 1.0f
                : float(accepted.first().toObject().value("duplicateThreshold").toDouble());
        WebFacePolicy::filter(accepted, local, network, threshold, &verified, &rejected);
        appendFailures(importResult_, rejected);

        // 通过防重的图片才交给原本地人员存储层落库。
        LocalPersonnelStore store;
        int saved = 0;
        QStringList failures;
        QString error;
        if (!store.replaceValidatedFaces(verified, &saved, &failures, &error)) {
            auto list = importResult_.value("failedNames").toArray();
            for (const auto &item : failures)
                if (!list.contains(item))
                    list.append(item);
            importResult_.insert("failedNames", list);
            importResult_.insert("failed", list.size());
            importResult_.insert("message", error);
        }
        importResult_.insert("savedFaces", saved);
        if (saved > 0)
            emit personnelChanged();
        finishImport(importResult_.value("failed").toInt() > 0 ||
                             !importResult_.value("message").toString().isEmpty()
                         ? "PARTIAL_FAILED"
                         : "DONE");
    } catch (const WebError &e) {
        importResult_.insert("message", e.message);
        finishImport("RECOVERY_REQUIRED");
    }
}

/**
 * @brief 汇总导入结果并结束当前批次。
 * @param state 实际执行状态。
 */
void WebService::finishImport(const QString &state)
{
    const QString id = currentBatch_;
    try {
        WebImports::setState(id, state, importResult_);
        if (state != "RECOVERY_REQUIRED")
            cleanupBatch(id);
    } catch (const WebError &e) {
        qWarning() << "[WEB] cannot persist import result" << e.message;
    }
    currentBatch_.clear();
    importResult_ = {};
    if (importLease_) {
        importLease_ = false;
        LocalImportGate::release();
    }
}

/**
 * @brief 清理批次临时文件和任务记录。
 * @param id 目标记录或任务的标识。
 */
void WebService::cleanupBatch(const QString &id)
{
    if (!QRegularExpression(QStringLiteral("^[a-f0-9-]{36}$")).match(id).hasMatch())
        throw WebError(400, QStringLiteral("批次编号无效"));
    const QString path = QDir(config_.imports).filePath(id);
    if (!QFileInfo::exists(path))
        return;
    WebPaths::inside(path, config_.imports);
    if (!QDir(path).removeRecursively())
        throw WebError(503, QStringLiteral("批次中间文件清理失败，稍后由维护流程重试"));
}

/**
 * @brief 恢复或标记上次中断的上传和导入任务。
 */
void WebService::recover()
{
    // 重启后只尝试幂等的已接收任务；接收中的临时文件按失败清理。
    for (const auto &task : WebUploads::incomplete()) {
        const QString id = task.value("id").toString(), state = task.value("state").toString(),
                      part = task.value("part_path").toString();
        try {
            if (task.value("kind") == "resource_package" && state == "COMMITTING") {
                startResourcePackage(task, true); // 只恢复/回滚中断事务，绝不重放资源包。
                continue;
            }
            if (state == "RECEIVED" || state == "VALIDATING" || state == "COMMITTING")
                commit(id, task.value("owner_id").toInt());
            else {
                WebPaths::inside(part,
                                 taskRoot(task));
                if (QFileInfo::exists(part) && !QFile::remove(part))
                    throw WebError(503, QStringLiteral("残留临时文件清理失败"));
                WebUploads::setState(id,
                                     "FAILED",
                                     0,
                                     QStringLiteral("服务重启，未完成的接收已终止"));
            }
        } catch (const WebError &e) {
            qWarning() << "[WEB] recovery deferred" << id << e.message;
        }
    }
    for (const auto &row : WebSql::query(
             QStringLiteral("SELECT id,path FROM web_media_files WHERE state='DELETING'")))
        if (!QFileInfo::exists(row.value("path").toString()))
            WebMedia::removed(row.value("id").toString());
    for (const auto &row :
         WebSql::query(QStringLiteral("SELECT id FROM web_person_import_batches WHERE state IN "
                                      "('DONE','PARTIAL_FAILED','FAILED','CANCELLED')"))) {
        try {
            cleanupBatch(row.value("id").toString());
        } catch (const WebError &error) {
            qWarning() << "[WEB] deferred cache cleanup" << error.message;
        }
    }
}
