/**
 * @file qt_web_tests.cpp
 * @brief 实际 SQLite/文件/原 Excel 解析与本地仓储集成测试，以及阈值防重测试。
 * @author Du lin
 * @date 2026-10-09
 * @note 最近修改：2026-10-22；修改人：Du lin；补充媒体上传单文件大小边界及超限请求不占用任务配额的验证，保留原媒体发布和空间检查用例。
 */

#include "modules/web/web_service.h"
#include "modules/web/web_resource_update.h"
#include "components/udisk_status/udisk_status.h"
#include "common/network_config.h"
#include "modules/web/web_local_runtime.h"
#include "modules/web/web_local_api.h"
#include "ic_board/local_access_service.h"
#include "ic_board/rs485_floor_frame_builder.h"
#include <QImage>
#include <QBuffer>
#include <QElapsedTimer>
#include "modules/web/web_hotspot.h"
#include "modules/web/web_paths.h"
#include "modules/web/web_face_policy.h"
#include "modules/web/local_import_gate.h"
#include "common/sql/web/web_sql.h"
#include "common/sql/web/web_schema.h"
#include "common/sql/web/web_upload_repository.h"
#include "common/sql/web/web_import_repository.h"
#include "common/sql/web/web_media_repository.h"
#include "common/sql/web/web_account_repository.h"
#include "common/sql/web/web_person_repository.h"
#include "common/sql/local_personnel_store.h"
#include "platform/rk3566_platform.h"
#include "modules/facegate/FaceGateQt/import/PersonXlsxParser.h"
#include <QCoreApplication>
#include <QApplication>
#include <QWidget>
#include <QProgressBar>
#include <QLabel>
#include "modules/web/web_upload_presenter.h"
#include "common/media_content_policy.h"
#include "modules/web/web_floor_schedule.h"
#include <QFile>
#include <QDir>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTest>
#include <QSignalSpy>
#include <QTimer>
#include <QFontDatabase>
#include <QStorageInfo>
#include <QSettings>
#include <functional>
#include <cstring>
#include <cstdio>

namespace {

/**
 * @brief 不受 release/NDEBUG 影响的检查。
 * @param value 断言条件。
 * @param message 断言失败时显示的诊断文本。
 */
void check(bool value, const char *message)
{
    if (!value)
        throw std::runtime_error(message);
}

/**
 * @brief 检查指定操作必定被拒绝，并核对状态码。
 * @param status 预期的 WebError 状态码。
 * @param action 应抛出预期业务错误的测试操作。
 */
void rejects(int status, const std::function<void()> &action)
{
    try {
        action();
    } catch (const WebError &e) {
        if (e.status != status)
            throw std::runtime_error(QString("expected %1, got %2: %3")
                                         .arg(status)
                                         .arg(e.status)
                                         .arg(e.message)
                                         .toStdString());
        return;
    }
    throw std::runtime_error("unsafe operation unexpectedly accepted");
}

/**
 * @brief 构造用于防重测试的人脸特征条目。
 * @param person 测试人员主键。
 * @param x 测试特征向量第一分量。
 * @param y 测试特征向量第二分量。
 * @return 生成质量提取后候选结构，向量用真实 float 二进制。
 */
QJsonObject face(qint64 person, float x, float y)
{
    float vector[2] = {x, y};
    const QByteArray bytes(reinterpret_cast<const char *>(vector), sizeof(vector));
    return {{"localPersonId", QString::number(person)},
            {"personName", QString::number(person)},
            {"featureBase64", QString::fromLatin1(bytes.toBase64())},
            {"modelVersion", "test-v1"}};
}

/**
 * @brief 使用 Python hashlib 的固定摘要，独立校验 Qt 端 PBKDF2 实现。
 * @param path 待访问的文件或配置路径。
 */
void factory(const QString &path)
{
    const QByteArray salt(32, 's');
    const QByteArray hash =
        QByteArray::fromHex("1bce69b173714905809bccb58300436570034e39bdc23c3e7644dafcf82c4fd4");
    QFile file(path);
    check(file.open(QIODevice::WriteOnly), "factory fixture write");
    file.write(QJsonDocument(QJsonObject{{"username", "tester"},
                                         {"salt_hex", QString::fromLatin1(salt.toHex())},
                                         {"hash_hex", QString::fromLatin1(hash.toHex())},
                                         {"iterations", 210000}})
                   .toJson());
}

/**
 * @brief 验证 Web 数据库没有第二份人员模型、迁移幂等及账号摘要校验。
 */
void schemaAndAuthentication()
{
    WebSchema::migrate();
    WebSchema::migrate();
    check(WebSql::query("SELECT * FROM web_schema_versions").size() == 1, "migration idempotency");
    check(WebAccounts::authenticate("tester", "test-secret").value("id").toInt() == 1,
          "correct credential rejected");
    rejects(401, []() {
        WebAccounts::authenticate("tester", "wrong");
    });
    rejects(401, []() {
        WebAccounts::authenticate("missing", "test-secret");
    });
}

/**
 * @brief 返回隔离测试使用的热点辅助程序路径，不操作真实网络。
 * @param root 受控根目录。
 * @return 生成的热点辅助脚本完整路径。
 */
QString mockHotspotHelper(const QString &root)
{
#ifdef Q_OS_WIN
    return QDir(root).filePath("portal-helper.exe");
#else
    return QDir(root).filePath("portal-helper");
#endif
}

/**
 * @brief 验证设备身份、默认 SSID、热点保存和更新失败的实际反馈。
 * @param service 隔离配置下的真实 Web 服务实例。
 * @param network 网络人员图库。
 * @param root 受控根目录。
 * @param ready 测试 Web 就绪标记文件路径。
 */
void hotspotConfiguration(WebService &service,
                          const QString &network,
                          const QString &root,
                          const QString &ready)
{
    const auto write = [](const QString &path, const QByteArray &data) {
        QFile file(path);
        check(file.open(QIODevice::WriteOnly), "hotspot fixture write failed");
        check(file.write(data) == data.size(), "hotspot fixture incomplete");
    };
    const auto read = [](const QString &path) {
        QFile file(path);
        check(file.open(QIODevice::ReadOnly), "hotspot fixture read failed");
        return file.readAll();
    };
    const auto helper = mockHotspotHelper(root);
    check(QFile::copy(QCoreApplication::applicationFilePath(), helper),
          "helper fixture copy failed");
    QFile::setPermissions(helper, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    qputenv("WEB_TEST_NM_ROOT", root.toLocal8Bit());
    write(network, "[mqtt]\nclient_id=\n[ftp]\nhost=unchanged\n");
    rejects(503, [&]() {
        service.dispatch("device.info", {}, 0);
    });
    check(!QFileInfo::exists(ready), "ready before client_id");
    write(network, "[mqtt]\nclient_id=YCEEQZ12345ABCDE\n[ftp]\nhost=unchanged\n");
    // No WiFi, no NM profile: Qt must still become ready without configuring networking.
    service.dispatch("device.info", {}, 0);
    check(read(ready).endsWith("YCEEQZ12345ABCDE\nYC_ABCDE_0000\n"),
          "Qt readiness/default SSID invalid");
    check(!QFileInfo::exists(QDir(root).filePath("nm-commands")),
          "Qt readiness configured a hotspot");
    auto info = service.dispatch("device.info", {}, 0);
    check(info.value("deviceName") == "YC_ABCDE_0000" &&
              info.value("deviceId") == "YCEEQZ12345ABCDE",
          "identity mismatch");
    write(network, "[mqtt]\nclient_id=YCEEQZ9876512345\n[ftp]\nhost=unchanged\n");
    check(service.dispatch("device.info", {}, 0).value("wifiName") == "YC_12345_0000",
          "updated default SSID missing");
    rejects(401, [&]() {
        service.dispatch("device.wifi.save", {{"ssid", "Forbidden"}}, 0);
    });
    for (const auto &ssid : QStringList{QString(),
                                        " leading",
                                        "trailing ",
                                        "bad\ncommand",
                                        QString(33, 'a'),
                                        QString(11, QChar(0x4e2d))})
        rejects(400, [&]() {
            service.dispatch("device.wifi.save", {{"ssid", ssid}}, 1);
        });
    rejects(503, [&]() {
        service.dispatch("device.wifi.save", {{"ssid", "No active hotspot"}}, 1);
    });
    write(QDir(root).filePath("nm-active"), "1");
    for (const auto &password : QStringList{"short",
                                            "bad\npassword",
                                            QString(64, 'z'),
                                            QStringLiteral("中文密码不能作为PSK")})
        rejects(400, [&]() {
            service.dispatch("device.wifi.save",
                             {{"ssid", "unchanged"}, {"password", password}},
                             1);
        });
    rejects(400, [&]() {
        service.dispatch("device.wifi.save", {{"ssid", "unchanged"}, {"password", 12345678}}, 1);
    });
    info =
        service.dispatch("device.wifi.save",
                         {{"ssid", QStringLiteral("用户热点_01")}, {"password", "New-Hotspot-123"}},
                         1);
    check(info.value("restartPending").toBool() &&
              info.value("wifiName") == QStringLiteral("用户热点_01"),
          "SSID not persisted");
    check(read(QDir(root).filePath("nm-password")) == "New-Hotspot-123\n",
          "hotspot password did not reach helper stdin");
    check(!info.contains("password"), "hotspot password leaked in response");
    rejects(409, [&]() {
        service.dispatch("device.wifi.save", {{"ssid", "second-change"}}, 1);
    });
    const auto log = QDir(root).filePath("nm-commands");
    QTest::qWait(1800);
    check(!read(log).contains("refresh"), "hotspot updated before 3s notice");
    QTest::qWait(1800);
    check(read(log).contains("refresh"), "delayed NM reactivation missing");
    check(!service.dispatch("device.info", {}, 0).value("restartPending").toBool(),
          "update did not finish");
    write(network, "[mqtt]\nclient_id=YCEEQZ54321FFFFF\n[ftp]\nhost=unchanged\n");
    check(service.dispatch("device.info", {}, 0).value("wifiName") == QStringLiteral("用户热点_01"),
          "custom SSID overwritten");
    {
        WebHotspot restarted(network, helper, {}, nullptr);
        restarted.synchronize();
        check(restarted.info().value("wifiName") == QStringLiteral("用户热点_01"),
              "custom SSID lost on Qt restart");
    }
    {
        WebHotspot unavailable(network, QDir(root).filePath("missing-helper"), {}, nullptr);
        unavailable.synchronize();
        check(unavailable.info().value("wifiName") == "YC_FFFFF_0000",
              "missing helper blocked Qt identity");
        rejects(503, [&]() {
            unavailable.save("Must-not-save");
        });
    }
    write(QDir(root).filePath("nm-fail-refresh"), "1");
    service.dispatch("device.wifi.save", {{"ssid", "Saved-but-update-failed"}}, 1);
    QTest::qWait(3600);
    check(!service.dispatch("device.info", {}, 0).value("wifiUpdateError").toString().isEmpty(),
          "refresh failure was hidden");
    check(read(network).contains("host=unchanged"), "network config unexpectedly changed");
}

/**
 * @brief 验证网络下发同一参数可调整防重结果、自身更新允许、同批和跨库冲突拒绝。
 */
void duplicateThreshold()
{
    FaceRecord other;
    other.person.id = 2;
    other.person.name = "已有人员";
    other.feature.modelVersion = "test-v1";
    other.feature.blob =
        QByteArray::fromBase64(face(2, 1, 0).value("featureBase64").toString().toLatin1());
    QJsonArray accepted, failed;
    WebFacePolicy::filter({face(1, 0.8f, 0.6f)}, {other}, {}, 0.75f, &accepted, &failed);
    check(accepted.isEmpty() && failed.size() == 1, "local threshold conflict missed");
    accepted = {};
    failed = {};
    WebFacePolicy::filter({face(1, 0.8f, 0.6f)}, {other}, {}, 0.9f, &accepted, &failed);
    check(accepted.size() == 1 && failed.isEmpty(), "threshold adjustment ignored");
    accepted = {};
    failed = {};
    WebFacePolicy::filter({face(2, 1, 0)}, {other}, {}, 0.5f, &accepted, &failed);
    check(accepted.size() == 1, "self replacement rejected");
    accepted = {};
    failed = {};
    WebFacePolicy::filter({face(1, 1, 0)}, {}, {other}, 0.5f, &accepted, &failed);
    check(accepted.isEmpty() && failed.size() == 1, "network duplicate missed");
    accepted = {};
    failed = {};
    WebFacePolicy::filter({face(1, 1, 0), face(1, 1, 0), face(2, 1, 0)},
                          {},
                          {},
                          0.5f,
                          &accepted,
                          &failed);
    check(accepted.size() == 1 && failed.size() == 1, "same batch duplicate semantics");
    other.feature.modelVersion = "other-model";
    accepted = {};
    failed = {};
    WebFacePolicy::filter({face(1, 1, 0)}, {other}, {}, 0.5f, &accepted, &failed);
    check(accepted.isEmpty(), "incompatible gallery model accepted");
}

/**
 * @brief 使用上传任务实际写盘、原解析器和原仓储核对部分成功及网络/卡号冲突。
 * @param service 隔离配置下的真实 Web 服务实例。
 * @param fixture 测试夹具文件路径。
 * @param root 受控根目录。
 */
void excelUploadAndImport(WebService &service, const QString &fixture, const QString &root)
{
    LocalPersonnelStore store;
    check(store.initialize(), "local schema initialize");
    WebSql::execute("CREATE TABLE network_person(person_id TEXT PRIMARY KEY,name TEXT,phone "
                    "TEXT,person_json TEXT,active INTEGER,deleted INTEGER,status INTEGER)");
    WebSql::execute("CREATE TABLE network_face(id INTEGER PRIMARY KEY,person_id TEXT,status "
                    "TEXT,feature_blob BLOB,model_version TEXT)");
    WebSql::execute("CREATE TABLE network_ic_card(card_id TEXT,person_id TEXT)");
    WebSql::execute(
        "INSERT INTO network_person VALUES('network_a','网络重复','13812345680','{}',1,0,1)");
    WebSql::execute("INSERT INTO network_ic_card VALUES('ABCD1234','network_a')");
    const auto parsed = PersonXlsxParser::parse(fixture);
    check(parsed.ok && parsed.records.size() == 3 && parsed.warnings.size() >= 2,
          "original Excel validation mismatch");
    const qint64 size = QFileInfo(fixture).size();
    const auto batch = service.dispatch("imports.create",
                                        {{"manifest",
                                          QJsonArray{QJsonObject{{"kind", "person_excel"},
                                                                 {"name", "任意目录上传.xlsx"},
                                                                 {"size", double(size)}}}}},
                                        1);
    const QString batchId = batch.value("id").toString();
    auto task = service.dispatch("uploads.create",
                                 {{"kind", "person_excel"},
                                  {"name", "任意目录上传.xlsx"},
                                  {"size", double(size)},
                                  {"batch_id", batchId}},
                                 1);
    const QString id = task.value("id").toString();
    rejects(404, [&]() {
        WebUploads::get(id, 2);
    });
    auto start = service.dispatch("uploads.begin", {{"id", id}}, 1);
    const QString part = start.value("part_path").toString();
    rejects(409, [&]() {
        service.dispatch("uploads.commit", {{"id", id}}, 1);
    });
    check(QFile::copy(fixture, part), "upload file copy");
    service.dispatch("uploads.received", {{"id", id}}, 1);
    auto committed = service.dispatch("uploads.commit", {{"id", id}}, 1);
    check(committed.value("state") == "READY" && !QFileInfo::exists(part),
          "Excel atomic publish failed");
    check(!committed.contains("part_path") && !committed.contains("final_path"),
          "internal paths leaked");
    check(service.dispatch("uploads.commit", {{"id", id}}, 1).value("state") == "READY",
          "commit not idempotent");
    service.dispatch("imports.start", {{"id", batchId}}, 1);
    for (int i = 0; i < 50; ++i) {
        QCoreApplication::processEvents();
        if (WebImports::get(batchId).value("state") == "PARTIAL_FAILED")
            break;
        QTest::qWait(10);
    }
    const auto done = WebImports::get(batchId);
    check(done.value("state") == "PARTIAL_FAILED", "batch did not finish partially");
    check(done.value("result").toObject().value("inserted").toInt() == 1 &&
              done.value("result").toObject().value("failed").toInt() == 2,
          "original partial success/conflict mismatch");
    check(WebSql::query("SELECT id FROM person WHERE account='web_a'").size() == 1,
          "local person missing");
    check(WebSql::query("SELECT * FROM network_person").size() == 1, "network personnel modified");
    WebSql::execute("CREATE TABLE network_access_rule(id INTEGER)");
    rejects(503, []() {
        WebPeople::verifyConflictSources();
    });
    WebSql::execute("DROP TABLE network_access_rule");
    service.dispatch("imports.start", {{"id", batchId}}, 1);
    check(WebSql::query("SELECT id FROM person").size() == 1, "same batch replayed");
    check(QFileInfo::exists(QDir(root).filePath("access_control.db")), "unexpected database path");
}

/**
 * @brief 核对缺少图片不能封存、取消清理、接收中断及不确定批次不重放。
 * @param service 隔离配置下的真实 Web 服务实例。
 * @param fixture 测试夹具文件路径。
 * @param imports 测试导入暂存目录。
 */
void interruptedBatches(WebService &service, const QString &fixture, const QString &imports)
{
    const qint64 size = QFileInfo(fixture).size();
    const QJsonArray manifest = {
        QJsonObject{{"name", "retry.xlsx"}, {"kind", "person_excel"}, {"size", double(size)}},
        QJsonObject{{"name", "web_a_1.jpg"}, {"kind", "person_face"}, {"size", 100}}};
    const auto batch = service.dispatch("imports.create", {{"manifest", manifest}}, 1);
    const auto batchId = batch.value("id").toString();
    const auto task = service.dispatch("uploads.create",
                                       {{"kind", "person_excel"},
                                        {"name", "retry.xlsx"},
                                        {"size", double(size)},
                                        {"batch_id", batchId}},
                                       1);
    const auto taskId = task.value("id").toString();
    const auto start = service.dispatch("uploads.begin", {{"id", taskId}}, 1);
    check(QFile::copy(fixture, start.value("part_path").toString()),
          "manifest fixture copy failed");
    service.dispatch("uploads.received", {{"id", taskId}}, 1);
    service.dispatch("uploads.commit", {{"id", taskId}}, 1);
    rejects(409, [&]() {
        service.dispatch("imports.start", {{"id", batchId}}, 1);
    });
    const auto status = service.dispatch("imports.status", {{"id", batchId}}, 1);
    check(status.value("assets").toArray().size() == 1 &&
              status.value("result").toObject().value("manifest").toArray().size() == 2,
          "refresh lost original manifest");
    service.dispatch("imports.discard", {{"id", batchId}}, 1);
    check(!QFileInfo::exists(QDir(imports).filePath(batchId)), "discard left staging files");
    const auto restart =
        service.dispatch("imports.create", {{"manifest", QJsonArray{manifest.first()}}}, 1);
    const auto restartId = restart.value("id").toString();
    const auto pending = service.dispatch("uploads.create",
                                          {{"kind", "person_excel"},
                                           {"name", "retry.xlsx"},
                                           {"size", double(size)},
                                           {"batch_id", restartId}},
                                          1);
    const auto pendingId = pending.value("id").toString();
    const auto receiving = service.dispatch("uploads.begin", {{"id", pendingId}}, 1);
    QFile part(receiving.value("part_path").toString());
    check(part.open(QIODevice::WriteOnly), "partial write failed");
    part.write("partial");
    part.close();
    rejects(400, [&]() {
        service.dispatch("uploads.received", {{"id", pendingId}}, 1);
    });
    service.initialize();
    check(service.dispatch("uploads.status", {{"id", pendingId}}, 1).value("state") == "FAILED" &&
              !QFileInfo::exists(part.fileName()),
          "restart did not clear interrupted receive");
    WebImports::setState(restartId, "IMPORTING");
    service.initialize();
    check(service.dispatch("imports.start", {{"id", restartId}}, 1).value("state") ==
              "RECOVERY_REQUIRED",
          "uncertain import replayed");
    check(WebSql::query("SELECT id FROM person").size() == 1, "recovery changed personnel");
}

/**
 * @brief 普通 eMMC 目录不需独立挂载；容量来自视频目录，缺失目录仍拒绝。
 * @param service 隔离配置下的真实 Web 服务实例。
 * @param root 受控根目录。
 * @param media 测试媒体目录。
 */
void emmcStorage(WebService &service, const QString &root, const QString &media)
{
    const auto oldMountHint = QDir(root).filePath("unused-mount-hint/missing");
    check(!QFileInfo::exists(oldMountHint), "mount hint fixture unexpectedly exists");
    WebPaths::mounted(media, oldMountHint);

    const auto missing = QDir(root).filePath("missing-video-directory");
    rejects(503, [&]() {
        WebPaths::mounted(missing, oldMountHint);
    });
    check(!QFileInfo::exists(missing), "availability check created a missing media directory");
    QFile regular(QDir(root).filePath("not-a-video-directory"));
    check(regular.open(QIODevice::WriteOnly), "media file fixture creation failed");
    regular.close();
    rejects(503, [&]() {
        WebPaths::mounted(regular.fileName(), oldMountHint);
    });
#ifdef Q_OS_UNIX
    const auto linked = QDir(root).filePath("linked-video-directory");
    check(QFile::link(media, linked), "media directory symlink fixture failed");
    rejects(503, [&]() {
        WebPaths::mounted(linked, oldMountHint);
    });
#endif

    const auto overview = service.dispatch("local.device.overview", {}, 1).value("data").toObject();
    QStorageInfo actual(media);
    actual.refresh();
    check(overview.value("storageAvailable").toBool() && overview.value("storage").isObject(),
          "ordinary eMMC directory storage reported unavailable");
    const auto values = overview.value("storage").toObject();
    check(values.value("total").toDouble() == actual.bytesTotal() / 1073741824.0 &&
              values.value("used").toDouble() >= 0 && values.value("usedPercent").toDouble() >= 0 &&
              values.value("usedPercent").toDouble() <= 100,
          "overview did not report media filesystem capacity");
    std::fprintf(stdout,
                 "eMMC storage checks passed: ordinary directory, legacy hint, missing path, "
                 "actual capacity\n");
}

/**
 * @brief 验证手机上传的真实字节进度、失败保持原播放、发布幂等及串行自动播放。
 * @param service 隔离配置下的真实 Web 服务实例。
 */
void videoUploadDevice(WebService &service)
{
    QWidget device;
    device.resize(800, 600);
    WebUploadPresenter presenter(&device);
    auto *runtime = WebLocalRuntime::instance();
    QObject::connect(runtime,
                     &WebLocalRuntime::uploadProgress,
                     &presenter,
                     &WebUploadPresenter::updateProgress,
                     Qt::QueuedConnection);
    QSignalSpy notifications(runtime, &WebLocalRuntime::uploadProgress);
    QSignalSpy published(runtime, &WebLocalRuntime::videoUploaded);
    QSignalSpy completed(runtime, &WebLocalRuntime::finished);
    QObject source;
    int controls = 0;
    QJsonObject actual{{"videoType", "RECORDED"},
                       {"videoPath", "old.mp4"},
                       {"playbackState", "PLAYING"},
                       {"canStartLocalPlayback", false}};
    runtime->bindMedia(
        &source,
        [&] {
            return actual;
        },
        [&](const QJsonObject &args) {
            ++controls;
            actual = args;
            actual["playbackState"] = "PLAYING";
        });
    device.show();
    const QByteArray bytes("web-test-video-valid");
    const auto task =
        service.dispatch("uploads.create",
                         {{"kind", "video"}, {"name", "phone-upload.mp4"}, {"size", bytes.size()}},
                         1);
    const auto id = task.value("id");
    const auto start = service.dispatch("uploads.begin", {{"id", id}}, 1);
    QCoreApplication::processEvents();
    auto *toast = device.findChild<QWidget *>("webUploadProgress");
    auto *bar = device.findChild<QProgressBar *>();
    check(toast && toast->isVisible() && bar && bar->value() == 0, "upload start card missing");
    QFile part(start.value("part_path").toString());
    check(part.open(QIODevice::WriteOnly) && part.write(bytes.left(10)) == 10,
          "partial video fixture write");
    part.flush();
    service.dispatch("uploads.progress", {{"id", id}}, 1);
    QCoreApplication::processEvents();
    check(bar->value() > 0 && bar->value() < 100, "actual receive bytes did not update card");
    device.grab().save("web-upload-progress.png");
    rejects(400, [&] {
        service.dispatch("uploads.received", {{"id", id}}, 1);
    });
    check(published.isEmpty() && controls == 0, "incomplete file started playback");
    part.write(bytes.mid(10));
    part.close();
    service.dispatch("uploads.received", {{"id", id}}, 1);
    QCoreApplication::processEvents();
    check(bar->value() == 99 && controls == 0, "receive reached complete before validation");
    check(service.dispatch("uploads.commit", {{"id", id}}, 1).value("state") == "READY",
          "valid video did not publish");
    QCoreApplication::processEvents();
    check(bar->value() == 100 && published.count() == 1, "published video completion missing");
    service.dispatch("uploads.commit", {{"id", id}}, 1);
    check(published.count() == 1, "idempotent commit restarted video");
    QTest::qWait(1100);
    check(controls == 0, "hidden/suspended player was started");
    actual["canStartLocalPlayback"] = true;
    QTest::qWait(1300);
    check(controls == 1 && actual.value("videoPath") == "phone-upload.mp4",
          "published video did not use actual player control");
    check(!completed.isEmpty() && completed.last().at(2).toInt() == 200,
          "autoplay completed without real state confirmation");
    // 校验失败的文件不触发播放，设备显示失败并保留原视频。
    const auto bad = service
                         .dispatch("uploads.create",
                                   {{"kind", "video"}, {"name", "bad-upload.mp4"}, {"size", 3}},
                                   1)
                         .value("id");
    const auto badStart = service.dispatch("uploads.begin", {{"id", bad}}, 1);
    QFile invalid(badStart.value("part_path").toString());
    check(invalid.open(QIODevice::WriteOnly) && invalid.write("bad") == 3, "invalid fixture");
    invalid.close();
    service.dispatch("uploads.received", {{"id", bad}}, 1);
    rejects(503, [&] {
        service.dispatch("uploads.commit", {{"id", bad}}, 1);
    });
    QCoreApplication::processEvents();
    check(notifications.last().at(3).toString() == "FAILED" && controls == 1 &&
              published.count() == 1,
          "failed validation changed playback");
    QTest::qWait(2700);
    check(!toast->isVisible(), "upload result did not disappear");
    service.dispatch("uploads.cancel", {{"id", bad}}, 1);
    const auto list = service.dispatch("files.list", {}, 1).value("files").toArray();
    for (const auto &f : list)
        if (f.toObject().value("name") == "phone-upload.mp4")
            service.dispatch("files.delete", {{"id", f.toObject().value("id")}}, 1);
    runtime->bindMedia(nullptr, {}, {});
}

/**
 * @brief 验证 INI 源回读、校验、异步执行、幂等及失败时不伪造配置。
 * @param service 隔离配置下的真实 Web 服务实例。
 * @param root 受控根目录。
 */
void networkConfiguration(WebService &service, const QString &root)
{
    const auto path = QDir(root).filePath("net_cfg.ini");
    {
        QSettings ini(path, QSettings::IniFormat);
        ini.setValue("network/iface", "eth0");
        ini.setValue("network/dhcp", false);
        ini.setValue("network/ip", "192.168.3.20");
        ini.setValue("network/netmask", "255.255.255.0");
        ini.setValue("network/gateway", "192.168.3.1");
        ini.setValue("network/dns", "1.1.1.1");
        ini.setValue("mqtt/host", "keep-mqtt-host");
        ini.setValue("ftp/host", "keep-ftp-host");
        ini.setValue("mode/type", "offline_v1");
        ini.sync();
    }
    const auto read = [&]() {
        return service.dispatch("local.device.networkConfig.get", {}, 1).value("data").toObject();
    };
    check(read().value("ip") == "192.168.3.20" && !read().value("dhcp").toBool(),
          "network source did not read original INI keys");
    rejects(401, [&] {
        service.dispatch("local.device.networkConfig.get", {}, 0);
    });
    const QJsonObject config{{"dhcp", false},
                             {"ip", "192.168.3.55"},
                             {"netmask", "255.255.255.0"},
                             {"gateway", "192.168.3.1"},
                             {"dns", "1.1.1.1, 8.8.8.8"},
                             {"_key", "ip-static"}};
    auto *runtime = WebLocalRuntime::instance();
    QObject owner;
    int applies = 0;
    runtime->bindNetwork(&owner, [&](const QString &token, const QJsonObject &args) {
        ++applies;
        QString error;
        check(args.value("iface") == "eth0" && args.value("dns") == "1.1.1.1 8.8.8.8",
              "normalized interface/DNS not passed to actual backend binding");
        check(NetworkConfig::save(path, args, &error), "network INI save failed");
        auto result = args;
        result["saved"] = true;
        result["applied"] = true;
        runtime->complete(token, result);
    });
    const auto set = [&](const QJsonObject &args) {
        return service.dispatch("local.device.networkConfig.set", args, 1).value("data").toObject();
    };
    const auto waitDone = [&](const QJsonObject &args) {
        auto state = set(args);
        for (int n = 0; n < 100 && state.value("state") == "PENDING"; ++n) {
            QTest::qWait(10);
            state = set(args);
        }
        return state;
    };
    auto invalid = config;
    invalid["netmask"] = "255.0.255.0";
    rejects(400, [&] {
        set(invalid);
    });
    invalid = config;
    invalid["gateway"] = "10.0.0.1";
    rejects(400, [&] {
        set(invalid);
    });
    invalid = config;
    invalid["iface"] = "wlan0";
    rejects(400, [&] {
        set(invalid);
    });
    invalid = config;
    invalid["ip"] = "192.168.10.55";
    rejects(400, [&] {
        set(invalid);
    });
    invalid = config;
    invalid["dns"] = "1.1.1.1\nreboot";
    rejects(400, [&] {
        set(invalid);
    });
    check(applies == 0 && read().value("ip") == "192.168.3.20", "invalid input changed networking");
    auto task = set(config);
    check(task.value("state") == "PENDING" && applies == 0, "network apply blocked IPC dispatch");
    QCoreApplication::processEvents();
    check(waitDone(config).value("state") == "APPLIED" && applies == 1,
          "network idempotency failed");
    QCoreApplication::processEvents();
    check(applies == 1 && read().value("ip") == "192.168.3.55",
          "network save not visible on reload");
    {
        QSettings ini(path, QSettings::IniFormat);
        check(ini.value("mqtt/host") == "keep-mqtt-host" &&
                  ini.value("ftp/host") == "keep-ftp-host",
              "network save modified unrelated groups");
        check(ini.value("network/use_config").toBool() &&
                  ini.value("network/auto_connect").toBool(),
              "saved network not compatible with original auto-connect keys");
        check(ini.value("network/web_managed").toBool(), "explicit network intent lost on restart");
    }
    // 取消绑定后，实际控制入口缺失必须失败，不能只写 INI 并报告成功。
    runtime->bindNetwork(nullptr, {});
    auto missing = config;
    missing["_key"] = "ip-no-backend";
    set(missing);
    QCoreApplication::processEvents();
    check(waitDone(missing).value("state") == "FAILED", "missing network backend reported success");
    // DHCP 新模式清除旧静态值，取得租约后可通过同一写入函数同步实际地址。
    QString error;
    QJsonObject dhcp;
    check(NetworkConfig::normalize({{"dhcp", true}}, read(), &dhcp, &error), "DHCP input rejected");
    check(NetworkConfig::save(path, dhcp, &error), "DHCP mode save failed");
    check(read().value("dhcp").toBool() && read().value("ip").toString().isEmpty(),
          "stale static IP retained");
    dhcp["ip"] = "192.168.3.80";
    dhcp["netmask"] = "255.255.255.0";
    check(NetworkConfig::save(path, dhcp, &error) && read().value("ip") == "192.168.3.80",
          "DHCP lease did not persist to same config fields");
    std::fprintf(stdout,
                 "Network API checks passed: INI source, offline, validation, async, idempotency, "
                 "DHCP keys\n");
}

/**
 * @brief 验证旧目录文件也可列举与删除，.part 不暴露，未接入业务不创建路由。
 * @param service 隔离配置下的真实 Web 服务实例。
 * @param media 测试媒体目录。
 */
void mediaDirectory(WebService &service, const QString &media)
{
    QFile video(QDir(media).filePath("ftp-existing.mp4"));
    check(video.open(QIODevice::WriteOnly), "existing fixture open");
    video.write("existing directory fixture");
    video.close();
    QFile part(QDir(media).filePath("partial.mp4.part"));
    check(part.open(QIODevice::WriteOnly), "part fixture open");
    part.write("not complete");
    part.close();
    const auto files = service.dispatch("files.list", {}, 1).value("files").toArray();
    check(files.size() == 1 && files.first().toObject().value("name") == "ftp-existing.mp4",
          "temporary exposed or existing file omitted");
    const auto id = files.first().toObject().value("id").toString();
    check(video.open(QIODevice::Append), "replace fixture open");
    video.write("replacement");
    video.close();
    rejects(409, [&]() {
        service.dispatch("files.delete", {{"id", id}}, 1);
    });
    const auto changed = service.dispatch("files.list", {}, 1)
                             .value("files")
                             .toArray()
                             .first()
                             .toObject()
                             .value("id")
                             .toString();
    check(changed != id, "same-path replacement kept stale identity");
    rejects(404, [&]() {
        service.dispatch("files.delete", {{"id", id}}, 1);
    });
    service.dispatch("files.delete", {{"id", changed}}, 1);
    check(!QFileInfo::exists(video.fileName()) && QFileInfo::exists(part.fileName()),
          "delete affected wrong file");
    check(service.dispatch("files.list", {}, 1).value("files").toArray().isEmpty(),
          "deleted media still listed");
    rejects(404, [&]() {
        service.dispatch("files.get", {{"id", id}}, 1);
    });
}

/**
 * @brief 图片使用真实解码、原子发布及两种目录；来源路径在创建时固定。
 * @param service 隔离配置下的真实 Web 服务实例。
 * @param media 测试媒体目录。
 */
void advertisementImages(WebService &service, const QString &media)
{
    const qint64 mib = 1024LL * 1024;
    // 仅声明长度、创建并取消任务，不实际分配或写入数百 MiB 夹具。
    for (const auto &limit : QList<QPair<QString, qint64>>{{"video", 500 * mib}, {"ad_image", 100 * mib}}) {
        const QString name = limit.first == "video" ? "size-boundary.mp4" : "size-boundary.png";
        const int activeBefore = WebUploads::incomplete().size();
        rejects(400, [&] {
            service.dispatch("uploads.create", {{"kind", limit.first}, {"name", name},
                             {"size", double(limit.second + 1)}}, 1);
        });
        check(WebUploads::incomplete().size() == activeBefore, "oversized media reserved upload space");
        const auto boundary = service.dispatch("uploads.create", {{"kind", limit.first}, {"name", name},
                                                 {"size", double(limit.second)}}, 1);
        service.dispatch("uploads.cancel", {{"id", boundary.value("id")}}, 1);
        check(WebUploads::incomplete().size() == activeBefore, "cancelled boundary media kept reservation");
    }
    const QString pic = QDir::cleanPath(QDir(media).filePath("../pic"));
    QImage image(64, 40, QImage::Format_RGB32);
    image.fill(Qt::green);
    QByteArray bytes;
    QBuffer buffer(&bytes);
    check(buffer.open(QIODevice::WriteOnly) && image.save(&buffer, "PNG"), "image fixture encode");
    auto *runtime = WebLocalRuntime::instance();
    QSignalSpy ready(runtime, &WebLocalRuntime::videoUploaded);
    QSignalSpy removed(runtime, &WebLocalRuntime::mediaFileDeleted);
    for (int small : {0, 1}) {
        const QString name = QStringLiteral("任意名称.%1.PNG").arg(small);
        const auto task = service.dispatch("uploads.create", {{"kind", "ad_image"}, {"name", name},
                                            {"size", bytes.size()}, {"smallImageMode", small}}, 1);
        const auto id = task.value("id");
        const auto start = service.dispatch("uploads.begin", {{"id", id}}, 1);
        const QString root = small ? pic : media;
        check(QFileInfo(start.value("part_path").toString()).absolutePath() == root + "/_uploads", "image destination incorrect");
        // 修改全局模式不应改变已创建任务的落盘目录。
        check(MediaContentPolicy::saveSelection(Rk3566Platform::netConfigPath(), "IMAGE", !small), "save shared choice");
        QFile file(start.value("part_path").toString());
        check(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(), "image receive"); file.close();
        service.dispatch("uploads.received", {{"id", id}}, 1);
        check(service.dispatch("uploads.commit", {{"id", id}}, 1).value("state") == "READY", "image publish");
        const QString full = QDir(root).filePath(name);
        check(QFileInfo::exists(full) && ready.last().at(0).toString() == full, "image path notification");
        const int notifications = ready.size(); service.dispatch("uploads.commit", {{"id", id}}, 1);
        check(ready.size() == notifications, "duplicate image commit notified twice");
        QString fileId;
        for (const auto &value : service.dispatch("files.list", {}, 1).value("files").toArray()) {
            const auto item = value.toObject();
            if (item.value("name") == name) {
                check(item.value("mediaType") == "IMAGE" && item.value("smallImageMode").toInt() == small, "image inventory metadata");
                fileId = item.value("id").toString();
            }
        }
        check(!fileId.isEmpty(), "image not in inventory");
        const auto download = service.dispatch("files.get", {{"id", fileId}}, 1);
        check(download.value("path") == full && download.value("bytes").toInt() == bytes.size(), "image download path");
        service.dispatch("files.delete", {{"id", fileId}}, 1);
        check(!QFileInfo::exists(full) && removed.last().at(0).toString() == full, "image deletion did not clear player cache");
    }
    for (const auto &ext : QStringList{"jpg", "jpeg", "png", "bmp", "gif", "webp", "tif", "tiff"})
        check(WebPaths::accepts("ad_image", "poster." + ext.toUpper()), "common image extension rejected");
    const auto invalid = service.dispatch("uploads.create", {{"kind", "ad_image"}, {"name", "bad.jpg"}, {"size", 3}, {"smallImageMode", 1}}, 1);
    const auto id = invalid.value("id"); const auto begin = service.dispatch("uploads.begin", {{"id", id}}, 1);
    QFile bad(begin.value("part_path").toString()); check(bad.open(QIODevice::WriteOnly), "bad image fixture"); bad.write("bad"); bad.close();
    service.dispatch("uploads.received", {{"id", id}}, 1);
    rejects(400, [&] { service.dispatch("uploads.commit", {{"id", id}}, 1); });
    service.dispatch("uploads.cancel", {{"id", id}}, 1);
    check(!QFileInfo::exists(bad.fileName()), "small image cancellation failed");
    std::fprintf(stdout, "Advertisement image checks passed: decoding, directories, task snapshot, inventory, download, delete, formats\n");
}

/**
 * @brief Web 与原 MQTT 共用的模式存储入口交叉读写，并保护通行帧高八位。
 * @param service 隔离配置下的真实 Web 服务实例。
 */
void sharedElevatorModes(WebService &service)
{
    using namespace Rs485FloorFrameBuilder;
    const QString modeKey = "online_v1_elevator_mode";
    const bool hadMode = DbStore::hasConfig(modeKey);
    const QVariant original = DbStore::getConfig(modeKey);
    const QVariant floors = DbStore::getConfig("online_v1_floor_limit_bits");
    const QStringList fields{"delay", "parking", "driver", "independent", "ext1", "ext2", "ext3", "ext4"};
    const auto stateJson = [](const ElevatorModeState &state) {
        return QJsonObject{{"masterSwitch", state.masterSwitch}, {"delay", state.delay},
                           {"parking", state.parking}, {"driver", state.driver},
                           {"independent", state.independent}, {"ext1", state.ext1},
                           {"ext2", state.ext2}, {"ext3", state.ext3}, {"ext4", state.ext4}};
    };
    const auto readWeb = [&] {
        return service.dispatch("local.elevator.elevatorMode.get", {}, 1).value("data").toObject();
    };
    auto *runtime = WebLocalRuntime::instance();
    runtime->setDeviceReady(true);
    QObject receiver;
    QList<QByteArray> frames;
    const auto transport = QObject::connect(runtime, &WebLocalRuntime::deviceRequested, &receiver,
        [&](const QString &token, const QString &operation, const QJsonObject &args) {
            if (operation != "rs485") return;
            frames.append(QByteArray::fromBase64(args.value("frame").toString().toLatin1()));
            runtime->complete(token, {{"saved", true}, {"transportSent", true}}, 200);
        });
    const auto writeWeb = [&](QJsonObject mode, const QString &key) {
        mode.insert("_key", key);
        const auto task = service.dispatch("local.elevator.elevatorMode.set", mode, 1).value("data").toObject();
        QElapsedTimer timer; timer.start();
        QJsonObject job;
        do {
            QCoreApplication::processEvents();
            QTest::qWait(1);
            job = service.dispatch("local.operations.get", {{"id", task.value("operationId")}}, 1).value("data").toObject();
        } while (job.value("state") == "PENDING" && timer.elapsed() < 3000);
        check(job.value("state") == "APPLIED", "Web mode did not complete serial dispatch");
    };
    ElevatorModeState initial;
    check(saveElevatorModeState(initial), "clear mode fixture");
    writeWeb(stateJson(initial), "mode-baseline");
    const QByteArray baseline = frames.last();
    check(baseline.size() == 132 && baseline.startsWith("OPEN"), "mode used temporary passage frame");
    for (int index = 0; index < fields.size(); ++index) {
        // 使用原 MQTT 设置/查询直接调用的真实公共函数；不链接或修改 MQTT 管理器。
        ElevatorModeState mqtt;
        mqtt.masterSwitch = true;
        bool *switches[] = {&mqtt.delay, &mqtt.parking, &mqtt.driver, &mqtt.independent,
                            &mqtt.ext1, &mqtt.ext2, &mqtt.ext3, &mqtt.ext4};
        *switches[index] = true;
        check(saveElevatorModeState(mqtt), "MQTT mode storage entry failed");
        const auto mqttMode = stateJson(mqtt);
        check(readWeb() == mqttMode, "Web read stale/different mode after MQTT storage write");

        auto webMode = mqttMode;
        for (const auto &field : fields) webMode[field] = !webMode.value(field).toBool();
        writeWeb(webMode, QString("mode-web-%1").arg(index));
        check(stateJson(elevatorModeState()) == webMode, "MQTT query entry read stale/different Web mode");
        check(readWeb() == webMode, "Web query disagreed with shared MQTT state");
        const auto wire = frames.last();
        check(wire.left(124) == baseline.left(124), "mode changed ordinary floor state");
        for (int bit = 0; bit < fields.size(); ++bit)
            check(wire.at(124 + bit) == (webMode.value(fields[bit]).toBool() ? '1' : '0'), "Web mode wire mapping differs from shared state");
        check(applyElevatorModeOverlay(baseline) == wire, "MQTT/common final overlay differs from Web frame");
        const QByteArray passage = QByteArray("#@") + QByteArray(32, 'F') + "#!";
        const QByteArray protectedPassage = applyElevatorModeOverlay(passage);
        check(protectedPassage == passage.left(32) + "00#!", "card/QR frame could trigger occupied mode channels");
        check(readWeb() == webMode, "passage frame changed persisted mode");
    }
    auto disabled = readWeb();
    disabled["masterSwitch"] = false;
    writeWeb(disabled, "mode-disable");
    check(frames.last() == baseline && stateJson(elevatorModeState()) == disabled,
          "master disable did not restore original floors/shared switches");
    check(DbStore::getConfig("online_v1_floor_limit_bits") == floors, "mode write overwrote original floor permissions");
    QObject::disconnect(transport);
    runtime->setDeviceReady(false);
    if (hadMode) check(DbStore::setConfig(modeKey, original), "restore original mode");
    else DbStore::deleteConfig(modeKey);
    std::fprintf(stdout, "Shared elevator mode checks passed: Web/MQTT storage cross-read, eight mode bits, persistent OPEN frames, card/QR protection, master restore\n");
}

/**
 * @brief 无MQTT链接的真实Web执行器：校验、位序、重复抑制、共享规则与重建恢复。
 */
void independentFloorSchedules()
{
    const QStringList keys{"online_v1_floor_schedules", "online_v1_floor_schedule_bits",
                            "online_v1_floor_limit_bits", "online_v1_floor_schedule_last_minute",
                            "floor_schedule_local_enabled"};
    QMap<QString, QVariant> backup;
    for (const auto &key : keys) if (DbStore::hasConfig(key)) backup.insert(key, DbStore::getConfig(key));
    for (const auto &key : keys) DbStore::deleteConfig(key);
    const QDateTime due(QDate::currentDate().addDays(1), QTime(8, 17));
    const auto rule = [&](const QString &hash, int action = 1, const QJsonArray &floors = QJsonArray{-8, -1, 1, 120}) {
        return QJsonObject{{"hash", hash}, {"action", action}, {"floors", floors},
                           {"weekDays", QJsonArray{due.date().dayOfWeek()}}, {"time", "08:17"}};
    };
    QList<QByteArray> frames;
    QStringList tags;
    QObject owner;
    const auto send = [&](const QByteArray &frame, const QString &tag) { frames.append(frame); tags.append(tag); return true; };
    int status = 200;
    QString error;
    QJsonArray saved;
    {
        WebFloorSchedule scheduler(&owner, send);
        const QJsonArray platform{rule("platform")};
        check(DbStore::setConfig("online_v1_floor_schedules", QString::fromUtf8(QJsonDocument(platform).toJson(QJsonDocument::Compact))), "shared rule fixture");
        scheduler.checkDue(due);
        check(frames.isEmpty(), "Web executed without local enable flag");
        auto result = scheduler.apply("set", {{"schedules", QJsonArray{rule("web")}}, {"baseSchedules", platform}}, &status, &error);
        check(status == 200 && result.value("saved").toBool(), "independent Web save failed");
        saved = result.value("deviceSchedules").toArray();
        check(DbStore::getConfig("floor_schedule_local_enabled").toBool(), "Web flag not persisted");
        scheduler.checkDue(due);
        check(frames.size() == 1 && frames.last().size() == 132 && frames.last().startsWith("OPEN"), "Web due frame missing");
        for (int i : {4, 11, 12, 131}) check(frames.last().at(i) == '0', "negative/positive floor board mapping incorrect");
        check(tags.last().startsWith("webFloorSchedule:"), "Web sender still uses MQTT source");
        check(DbStore::getConfig("online_v1_floor_schedule_last_minute").toString() == due.toString("yyyy-MM-dd HH:mm"), "shared minute marker missing");
        scheduler.checkDue(due);
        scheduler.apply("set", {{"schedules", saved}, {"baseSchedules", saved}}, &status, &error);
        scheduler.checkDue(due);
        check(status == 200 && frames.size() == 1, "same rules or minute resent frame");
        scheduler.apply("set", {{"schedules", QJsonArray{rule("stale")}}, {"baseSchedules", platform}}, &status, &error);
        check(status == 409, "stale Web snapshot overwrote shared rules");
        scheduler.apply("set", {{"schedules", QJsonArray{rule("invalid", 1, QJsonArray{0})}}}, &status, &error);
        check(status == 400, "floor zero accepted");
        scheduler.apply("set", {{"schedules", QJsonArray{rule("dup"), rule("dup")}}}, &status, &error);
        check(status == 400, "duplicate hash accepted");
        scheduler.apply("set", {{"schedules", QJsonArray{rule("open", 1), rule("limit", 0)}}}, &status, &error);
        check(status == 400, "conflicting actions accepted");
        auto invalidTime = rule("time"); invalidTime["time"] = "25:00";
        scheduler.apply("set", {{"schedules", QJsonArray{invalidTime}}}, &status, &error);
        check(status == 400, "invalid clock accepted");
    }
    // 新对象没有内存去重状态，仍读取持久化的分钟标记。
    WebFloorSchedule restarted(&owner, send);
    restarted.checkDue(due);
    check(frames.size() == 1, "recreated Web executor lost persisted deduplication");
    const QDateTime following = due.addDays(7);
    // 模拟原执行器已写入同一共享分钟，Web不得再发送。
    DbStore::setConfig("online_v1_floor_schedule_last_minute", following.toString("yyyy-MM-dd HH:mm"));
    restarted.checkDue(following);
    check(frames.size() == 1, "Web ignored shared execution marker");
    const auto previousBits = DbStore::getConfig("online_v1_floor_limit_bits");
    restarted.apply("delete", {{"hashList", QJsonArray{"web"}}, {"baseSchedules", saved}}, &status, &error);
    check(status == 200 && frames.size() == 1 && DbStore::getConfig("online_v1_floor_limit_bits") == previousBits, "delete changed already applied floor state");
    restarted.checkDue(due.addDays(14)); check(frames.size() == 1, "deleted rule executed");
    // 验证真正的周期回调，而不仅是手动调用checkDue；覆盖等待期间跨分钟的情况。
    const auto now = QDateTime::currentDateTime();
    auto periodic = rule("periodic");
    periodic["time"] = now.time().toString("HH:mm");
    periodic["weekDays"] = QJsonArray{1, 2, 3, 4, 5, 6, 7};
    auto nextMinute = periodic;
    nextMinute["hash"] = "periodic-next";
    nextMinute["time"] = now.addSecs(60).time().toString("HH:mm");
    restarted.apply("set", {{"schedules", QJsonArray{periodic, nextMinute}}}, &status, &error);
    check(status == 200, "periodic fixture save failed");
    const int beforeTimer = frames.size();
    DbStore::setConfig("online_v1_floor_schedule_last_minute", QStringLiteral(""));
    QElapsedTimer waiting; waiting.start();
    while (frames.size() == beforeTimer && waiting.elapsed() < 6500) QTest::qWait(25);
    check(frames.size() == beforeTimer + 1, "independent Web timer did not execute without MQTT");
    for (const auto &key : keys) {
        if (backup.contains(key)) DbStore::setConfig(key, backup.value(key));
        else DbStore::deleteConfig(key);
    }
    std::fprintf(stdout, "Independent Web scheduler checks passed: shared format, no MQTT link, offline timer logic, conflicts, mapping, persistence, deduplication\n");
}

/**
 * @brief 验证本地人员、凭证、梯控与任务查询业务。
 * Regression of local transactions, actual source adapters and MQTT-independent modes.
 * @param service 隔离配置下的真实 Web 服务实例。
 * @param root 受控根目录。
 */
void localBusiness(WebService &service, const QString &root)
{
    const auto call =
        [&](const QString &operation, const QJsonObject &args = QJsonObject{}, int owner = 1) {
            return service.dispatch("local." + operation, args, owner);
        };
    const auto data = [&](const QString &operation, const QJsonObject &args = QJsonObject{}) {
        return call(operation, args).value("data").toObject();
    };
    const auto caps = data("device.capabilities");
    check(caps.value("personnel").toBool() && !caps.value("mqttRequired").toBool(),
          "local capabilities depend on MQTT");
    rejects(401, [&] {
        call("device.overview", {}, 0);
    });
    for (const auto &mode : QStringList{"offline_v1", "online_v1", "online_v2"}) {
        QSettings settings(QDir(root).filePath("net_cfg.ini"), QSettings::IniFormat);
        settings.setValue("mode/type", mode);
        settings.sync();
        check(data("device.overview").value("personCount").toInt() == 1, "mode gated local reads");
    }
    rejects(501, [&] {
        call("device.airControl.set", {{"power", true}});
    });
    rejects(503, [&] {
        call("elevator.floorSchedule.set");
    });
    rejects(501, [&] {
        call("device.reboot");
    });
    rejects(503, [&] {
        call("device.getPlayInfo");
    });
    const QJsonObject create{{"name", "Local API person"},
                             {"account", "local_api_1"},
                             {"_key", "create-one"}};
    auto p = data("person.add", create);
    const auto id = p.value("id");
    check(p.value("version").toInt() == 1, "new revision incorrect");
    check(data("person.add", create).value("id") == id, "same-key person create replayed");
    auto changed = create;
    changed.insert("name", "different");
    rejects(409, [&] {
        call("person.add", changed);
    });
    const auto page = call("person.list", {{"offset", 0}, {"limit", 1}});
    check(page.value("data").toArray().size() == 1 &&
              page.value("meta").toObject().value("hasMore").toBool(),
          "pagination omitted metadata");
    const auto sub = [&](const QString &type, const QJsonObject &item, const QString &key) {
        auto payload = item;
        if (type == "FACE") {
            const auto staged = data("face.stage");
            QFile file(staged.value("path").toString());
            check(file.open(QIODevice::WriteOnly | QIODevice::NewOnly), "face stage open");
            const auto bytes =
                QByteArray::fromBase64(item.value("faceBase64").toString().toLatin1());
            check(file.write(bytes) == bytes.size(), "face stage write");
            file.close();
            payload = {{"assetId", staged.value("assetId")}, {"name", item.value("name")}};
        }
        return data("credential.addSubItem",
                    {{"personId", id}, {"subType", type}, {"subData", payload}, {"_key", key}});
    };
    sub("IC_CARD", {{"cardId", "01020304"}, {"name", "card"}}, "card-one");
    rejects(409, [&] {
        sub("IC_CARD", {{"cardId", "3412CDAB"}}, "network-reverse");
    });
    rejects(400, [&] {
        sub("QR_VISITOR", {{"qrUnique", "12345678"}}, "visitor-without-expiry");
    });
    sub("QR_VISITOR",
        {{"qrUnique", "12345678"},
         {"validTo", QDateTime::currentDateTime().addDays(1).toString(Qt::ISODate)}},
        "visitor-one");
    auto c = data("credential.get", {{"personId", id}});
    auto updated =
        data("person.update", {{"id", id}, {"name", "Renamed"}, {"version", c.value("version")}});
    c = data("credential.get", {{"personId", id}});
    check(c.value("icCards").toArray().size() == 1 && c.value("qrCodes").toArray().size() == 1,
          "person edit replaced credentials");
    rejects(409, [&] {
        call("person.update", {{"id", id}, {"name", "stale"}, {"version", 1}});
    });
    check(data("person.detail", {{"id", id}}).value("name") == "Renamed",
          "stale update changed person");
    QJsonObject rules{{"controlElevator", true},
                      {"cardPassword", true},
                      {"cardPasswordDataPassword", "617283"},
                      {"count", true},
                      {"countDataTotal", 10},
                      {"countDataAddCount", 0},
                      {"amount", true},
                      {"amountDataTotal", 100},
                      {"amountDataUnitPrice", 2.5}};
    data("credential.update",
         {{"personId", id}, {"extra", QJsonObject{{"rules", rules}}}, {"_key", "initial-rules"}});
    WebSql::execute(
        "UPDATE local_access_usage SET "
        "remaining_count=6,access_count=4,remaining_amount=90,used_amount=10 WHERE person_id=?",
        {id.toInt()});
    const QJsonObject recharge{
        {"personId", id},
        {"extra",
         QJsonObject{{"rules", QJsonObject{{"countDataAddCount", 3}, {"amountDataAddAmount", 5}}}}},
        {"_key", "one-recharge"}};
    data("credential.update", recharge);
    data("credential.update", recharge);
    auto usage = data("credential.get", {{"personId", id}}).value("usage").toObject();
    check(usage.value("remaining_count").toInt() == 9 && usage.value("access_count").toInt() == 4 &&
              usage.value("remaining_amount").toDouble() == 95 &&
              usage.value("used_amount").toDouble() == 10,
          "recharge replayed or consumption reset");
    data("personFloor.save", {{"personId", id}, {"floor", QJsonArray{-8, 1, 120, 1}}});
    rejects(400, [&] {
        call("personFloor.save", {{"personId", id}, {"floor", QJsonArray{0}}});
    });
    c = data("credential.get", {{"personId", id}});
    check(c.value("floors").toArray() == QJsonArray{-8, 1, 120} &&
              c.value("usage").toObject() == usage,
          "floor edit touched usage or rollback failed");
    rejects(501, [&] {
        call("credential.update",
             {{"personId", id}, {"extra", QJsonObject{{"rules", QJsonObject{{"relay", true}}}}}});
    });
    LocalAccessService access;
    const auto granted = access.checkCardId("01020304");
    if (!granted.pass)
        throw std::runtime_error(
            ("local access rejected Web card: " + granted.reason).toStdString());
    check(granted.floors == QList<int>{-8, 1, 120} && access.checkQrCode("12345678").pass &&
              access.checkPassword("617283").pass,
          "Web credentials did not reach original offline access service");
    check(access.recordSuccessfulAccess(granted), "original access consumption failed");
    const auto consumed = data("credential.get", {{"personId", id}}).value("usage").toObject();
    check(consumed.value("remaining_count").toInt() == 8 &&
              consumed.value("remaining_amount").toDouble() == 92.5,
          "Web usage did not reflect original access consumption");
    const auto before = c.value("version");
    const QJsonObject invalidTiming{
        {"time", true},
        {"timingRules",
         QJsonArray{
             QJsonObject{{"days", QJsonArray{8}}, {"timeRange", QJsonArray{"09:00", "18:00"}}}}}};
    rejects(400, [&] {
        call("credential.update",
             {{"personId", id}, {"extra", QJsonObject{{"rules", invalidTiming}}}});
    });
    check(data("credential.get", {{"personId", id}}).value("version") == before,
          "failed transaction changed revision");

    // Real image file + controlled extractor callback: verifies append semantics, not RKNN quality.
    QImage image(16, 16, QImage::Format_RGB32);
    image.fill(Qt::red);
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    check(image.save(&buffer, "JPG"), "face JPEG fixture failed");
    int faceSignals = 0;
    const auto connection =
        QObject::connect(&service,
                         &WebService::faceValidationRequested,
                         &service,
                         [&](const QString &token, const QJsonArray &faces) {
                             ++faceSignals;
                             const auto input = faces.first().toObject();
                             auto candidate =
                                 face(input.value("localPersonId").toString().toLongLong(), 0, 1);
                             candidate.insert("duplicateThreshold", 0.8);
                             service.applyFaces(token, QJsonArray{candidate}, {});
                         });
    auto firstFace =
        sub("FACE", {{"faceBase64", QString::fromLatin1(bytes.toBase64())}}, "face-one");
    if (firstFace.value("state") != "APPLIED")
        throw std::runtime_error(QJsonDocument(firstFace).toJson().toStdString());
    sub("FACE", {{"faceBase64", QString::fromLatin1(bytes.toBase64())}}, "face-two");
    c = data("credential.get", {{"personId", id}});
    check(faceSignals == 2 && c.value("faces").toArray().size() == 2 &&
              c.value("icCards").toArray().size() == 1,
          "single face replaced existing faces or credentials");
    sub("FACE", {{"faceBase64", QString::fromLatin1(bytes.toBase64())}}, "face-two");
    check(faceSignals == 2, "face idempotency repeated extraction");
    QObject::disconnect(connection);

    QObject source;
    QJsonObject actual{{"videoType", "NONE"},
                       {"videoVolume", 20},
                       {"videoPath", ""},
                       {"playbackState", "STOPPED"}};
    auto *runtime = WebLocalRuntime::instance();
    runtime->bindMedia(
        &source,
        [&] {
            return actual;
        },
        [&](const QJsonObject &args) {
            actual = args;
            actual.insert("playbackState", "PLAYING");
        });
    runtime->bindScreen(&source, [&](const QString &token) {
        runtime->image(token, image);
    });
    const auto poll = [&](QString token) {
        QElapsedTimer timer;
        timer.start();
        QJsonObject job;
        do {
            QCoreApplication::processEvents();
            QTest::qWait(10);
            job = data("operations.get", {{"id", token}});
        } while (job.value("state") == "PENDING" && timer.elapsed() < 3000);
        return job;
    };
    check(data("device.getPlayInfo").value("videoVolume").toInt() == 20,
          "media read bypassed source");
    auto task = data("device.videoControl",
                     {{"videoType", "NONE"}, {"videoVolume", 33}, {"_key", "media-once"}});
    check(poll(task.value("operationId").toString()).value("state") == "APPLIED",
          "local media task failed");
    task = data("device.videoControl", {{"videoType", "RECORDED"}, {"contentType", "IMAGE"}, {"smallImageMode", 1}, {"_key", "image-choice"}});
    check(poll(task.value("operationId").toString()).value("state") == "APPLIED", "shared image selection failed");
    check(MediaContentPolicy::contentType(Rk3566Platform::netConfigPath()) == "IMAGE" && MediaContentPolicy::smallImageMode(Rk3566Platform::netConfigPath()), "Web wrote different playback config");
    MediaContentPolicy::saveSelection(Rk3566Platform::netConfigPath(), "VIDEO", false);
    const auto shared = data("device.getPlayInfo");
    check(shared.value("contentType") == "VIDEO" && shared.value("smallImageMode").toInt() == 0, "Web read stale selection after external write");
    int scheduleCalls = 0;
    WebFloorSchedule scheduler(&source, [](const QByteArray &, const QString &) { return true; });
    runtime->bindFloorSchedule(&scheduler, [&](const QString &token, const QJsonObject &args) {
        check(args.value("action") == "set" && args.value("data").toObject().contains("schedules"), "scheduler request lost data");
        ++scheduleCalls;
        int status = 200; QString error;
        const auto result = scheduler.apply(args.value("action").toString(), args.value("data").toObject(), &status, &error);
        runtime->complete(token, result, status, error);
    });
    check(data("device.capabilities").value("floorScheduleWrite").toBool(), "bound scheduler still disabled");
    const QJsonObject scheduled{{"schedules", QJsonArray{}}, {"_key", "schedule-once"}};
    task = data("elevator.floorSchedule.set", scheduled);
    data("elevator.floorSchedule.set", scheduled);
    check(poll(task.value("operationId").toString()).value("state") == "APPLIED" && scheduleCalls == 1, "scheduler request not dispatched exactly once");
    runtime->bindFloorSchedule(nullptr, {});
    WebSql::execute("INSERT INTO web_accounts(id,username,salt,password_hash,iterations) SELECT "
                    "2,'other',salt,password_hash,iterations FROM web_accounts WHERE id=1");
    rejects(404, [&] {
        call("operations.get", {{"id", task.value("operationId")}}, 2);
    });
    task = data("device.screenshot", {{"_key", "capture-once"}});
    auto captured = poll(task.value("operationId").toString());
    check(captured.value("state") == "APPLIED" &&
              QFileInfo::exists(captured.value("result").toObject().value("_assetPath").toString()),
          "actual screenshot file not produced");
    runtime->setDeviceReady(true);
    int writes = 0;
    auto transport = QObject::connect(
        runtime,
        &WebLocalRuntime::deviceRequested,
        &source,
        [&](const QString &token, const QString &operation, const QJsonObject &args) {
            if (operation != "rs485")
                return;
            ++writes;
            auto frame = QByteArray::fromBase64(args.value("frame").toString().toLatin1());
            check(frame.size() == 132 && frame.startsWith("OPEN") && frame.at(4) == '0' &&
                      frame.at(12) == '0',
                  "business floor to board mapping incorrect");
            runtime->complete(
                token,
                {{"saved", true}, {"transportSent", true}, {"hardwareConfirmed", QJsonValue::Null}},
                200,
                {});
        });
    const QJsonObject floorRequest{{"floors",
                                    QJsonArray{QJsonObject{{"floor", -8}, {"action", 1}},
                                               QJsonObject{{"floor", 1}, {"action", 1}}}},
                                   {"_key", "floor-once"}};
    task = data("elevator.floorLimit.set", floorRequest);
    data("elevator.floorLimit.set", floorRequest);
    check(poll(task.value("operationId").toString()).value("state") == "APPLIED" && writes == 1,
          "duplicate pending request resent serial frame");
    QObject::disconnect(transport);
    runtime->setDeviceReady(false);
    runtime->bindMedia(nullptr, {}, {});
    runtime->bindScreen(nullptr, {});
    const auto snapshot = WebSql::query("SELECT * FROM network_person");
    data("person.delete", {{"id", id}});
    rejects(404, [&] {
        call("person.detail", {{"id", id}});
    });
    check(WebSql::query("SELECT * FROM network_person") == snapshot,
          "web mutated network personnel");
    rejects(400, [&] {
        call("auth.changePassword", {{"oldPassword", "bad"}, {"newPassword", "new-secret-123"}});
    });
    data("auth.changePassword",
         {{"oldPassword", "test-secret"}, {"newPassword", "new-secret-123"}});
    rejects(401, [] {
        WebAccounts::authenticate("tester", "test-secret");
    });
    check(WebAccounts::authenticate("tester", "new-secret-123").value("id").toInt() == 1,
          "new password rejected");
    const auto upload =
        service.dispatch("uploads.create",
                         {{"kind", "video"}, {"name", "multipart.mp4"}, {"size", 1024}},
                         1);
    const auto uploadId = upload.value("id");
    const auto receive = service.dispatch("uploads.begin", {{"id", uploadId}}, 1);
    QFile streamed(receive.value("part_path").toString());
    check(streamed.open(QIODevice::WriteOnly) && streamed.write("video-bytes") == 11,
          "streamed video fixture failed");
    streamed.close();
    rejects(400, [&] {
        call("media.received", {{"id", uploadId}, {"size", 12}});
    });
    call("media.received", {{"id", uploadId}, {"size", 11}});
    check(service.dispatch("uploads.received", {{"id", uploadId}}, 1).value("state") == "RECEIVED",
          "multipart measured length rejected by original upload pipeline");
    service.dispatch("uploads.cancel", {{"id", uploadId}}, 1);
    WebSql::execute(
        "INSERT INTO "
        "web_local_operations(id,owner,kind,request_key,hash,args,state,created_at,updated_at) "
        "VALUES('33333333-3333-4333-8333-333333333333',1,'videoControl','restart-test','','{}','"
        "PENDING',?,?)",
        {QDateTime::currentMSecsSinceEpoch(), QDateTime::currentMSecsSinceEpoch()});
    {
        WebLocalApi restored({Rk3566Platform::databasePath(),
                              QDir(root).filePath("static/demoResources/images/video"),
                              QStorageInfo(root).rootPath(),
                              QDir(root).filePath("imports"),
                              {},
                              QDir(root).filePath("net_cfg.ini"),
                              {},
                              {}});
        restored.initialize();
        check(data("operations.get", {{"id", "33333333-3333-4333-8333-333333333333"}})
                      .value("state") == "UNKNOWN",
              "interrupted hardware action was replayed or reported successful");
    }
    std::fprintf(stdout,
                 "Local API checks passed: modes, CRUD, transactions, recharge idempotency, faces, "
                 "media, screenshot, serial dispatch, password\n");
}

} // namespace

/**
 * @brief 原 U 盘覆盖层直接展示资源进度，且不发送 U 盘状态事件。
 */
void resourceDeviceProgress()
{
    QWidget host;
#ifdef Q_OS_WIN
    // offscreen 测试不启动原主程序字体初始化，仅为截图载入本机中文字体。
    const int fontId = QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/msyh.ttc"));
    const auto families = QFontDatabase::applicationFontFamilies(fontId);
    if (!families.isEmpty()) host.setFont(QFont(families.first(), 12));
#endif
    host.resize(900, 600);
    host.show();
    UdiskStatusOverlay overlay(&host);
    QSignalSpy udisk(&overlay, &UdiskStatusOverlay::udiskStateChanged);
    overlay.showResourceProgress("first", QStringLiteral("正在接收资源包"), 12, false);
    auto *bar = host.findChild<QProgressBar *>();
    check(bar && bar->isVisible() && bar->value() == 12, "resource progress not shown");
    overlay.showResourceProgress("first", QStringLiteral("正在解压资源包"), 72, false);
    check(bar->value() == 72 && udisk.count() == 0, "resource progress changed U-disk state");
    check(host.findChild<QLabel *>()->text() == QStringLiteral("正在解压资源包"),
          "resource overlay message missing");
    host.resize(1100, 650);
    QTest::qWait(30);
    overlay.showResourceProgress("first", QStringLiteral("设备即将重启，请稍候"), 100, true);
    check(bar->value() == 100, "resource complete did not reach 100 percent");
    overlay.showResourceProgress("second", QStringLiteral("正在解压资源包"), 48, false);
    QTest::qWait(3100);
    check(overlay.isVisible() && bar->value() == 48, "old result timer hid new resource task");
    check(host.grab().save("resource-device-progress.png"), "resource progress screenshot failed");
}

/**
 * @brief 第二个目录发布失败时，前一个目录和备份均恢复，且不安排重启。
 * @param root 受控根目录。
 * @param fixture 测试夹具文件路径。
 */
void resourceRollback(const QString &root, const QString &fixture)
{
    const QString target = QDir(root).filePath("rollback-resources");
    for (const auto &name : {QStringLiteral("logo"), QStringLiteral("pic")}) {
        QDir().mkpath(QDir(target).filePath(name));
        QFile old(QDir(target).filePath(name + "/old.txt"));
        check(old.open(QIODevice::WriteOnly), "rollback fixture create");
        old.write("old");
    }
    const QString record = QDir(root).filePath("rollback-restart.log");
    bool injected = false;
    auto result = WebResourceUpdate::execute(
        {QDir(QFileInfo(fixture).absolutePath()).filePath("resource-pair.zip"),
         target,
         QDir(root).filePath("rollback-result.json"),
         {QCoreApplication::applicationFilePath(), "record-resource-restart", record},
         false},
        [&](int, const QString &message, const QString &phase) {
            if (phase == "COMMITTING" && message.endsWith("pic") && !injected) {
                injected = true;
                const QString stage = QDir(target).filePath(".ycest-update/stage/pic");
                check(QFileInfo(stage).canonicalFilePath().startsWith(
                          QFileInfo(target).canonicalFilePath() + '/'),
                      "fault injection escaped temporary fixture");
                check(QDir(stage).removeRecursively(), "publish fault injection failed");
            }
        });
    check(injected && result.value("state") == "FAILED", "publish fault was not rejected");
    for (const auto &name : {QStringLiteral("logo"), QStringLiteral("pic")})
        check(QFileInfo::exists(QDir(target).filePath(name + "/old.txt")),
              "publish failure lost original resource");
    check(!QFileInfo::exists(record), "failed update scheduled restart");
}

/**
 * @brief 真实资源上传经 Qt 白名单、异步解压、结果查询，核对未提供目录保留。
 * @param service 隔离配置下的真实 Web 服务实例。
 * @param root 受控根目录。
 * @param fixture 测试夹具文件路径。
 */
void resourcePackageUpload(WebService &service, const QString &root, const QString &fixture)
{
    QSignalSpy progress(WebLocalRuntime::instance(), &WebLocalRuntime::resourceProgress);
    QTimer heartbeat;
    int ticks = 0;
    QObject::connect(&heartbeat, &QTimer::timeout, [&]() {
        ++ticks;
    });
    heartbeat.start(5);
    const QString images = QDir(root).filePath("resources");
    for (const QString &name : {QStringLiteral("logo"),
                                QStringLiteral("pic"),
                                QStringLiteral("tl"),
                                QStringLiteral("video")}) {
        check(QDir().mkpath(QDir(images).filePath(name)), "resource fixture mkdir");
        QFile file(QDir(images).filePath(name + "/old.txt"));
        check(file.open(QIODevice::WriteOnly), "resource fixture write");
        file.write(name.toUtf8());
    }
    auto upload = [&](const QString &name) {
        QFile archive(QDir(QFileInfo(fixture).absolutePath()).filePath(name));
        check(archive.open(QIODevice::ReadOnly), "resource archive fixture missing");
        auto task = service.dispatch(
            "uploads.create",
            {{"kind", "resource_package"}, {"name", name}, {"size", double(archive.size())}},
            1);
        const QString id = task.value("id").toString();
        rejects(401, [&]() {
            service.dispatch("uploads.status", {{"id", id}}, 999);
        });
        rejects(404, [&]() {
            WebUploads::get(id, 999);
        });
        rejects(409, [&]() {
            service.dispatch("uploads.create",
                             {{"kind", "video"}, {"name", "busy.mp4"}, {"size", 10}},
                             1);
        });
        const auto receiving = service.dispatch("uploads.begin", {{"id", id}}, 1);
        QFile part(receiving.value("part_path").toString());
        check(part.open(QIODevice::WriteOnly), "resource upload part create");
        part.write(archive.readAll());
        part.close();
        service.dispatch("uploads.received", {{"id", id}}, 1);
        const auto started = service.dispatch("uploads.commit", {{"id", id}}, 1);
        check(started.value("state") == "COMMITTING", "resource not asynchronous");
        const auto repeated = service.dispatch("uploads.commit", {{"id", id}}, 1);
        check(repeated.value("state") == "COMMITTING", "duplicate resource commit replayed");
        QElapsedTimer timer;
        timer.start();
        do {
            QTest::qWait(25);
            task = service.dispatch("uploads.status", {{"id", id}}, 1);
        } while (task.value("state") == "COMMITTING" && timer.elapsed() < 15000);
        check(!QFileInfo::exists(part.fileName()), "resource archive cache not removed");
        check(!task.contains("part_path"), "resource status exposed internal path");
        return task;
    };
    const auto ready = upload("resource.zip");
    check(ready.value("restartScheduled").toBool(), "resource update did not schedule Qt restart");
    if (ready.value("state") != "READY")
        throw std::runtime_error(ready.value("error").toString().toStdString());
    check(ready.value("updatedDirectories").toArray() == QJsonArray{"logo"},
          "updated directories missing");
    check(!QFileInfo::exists(QDir(images).filePath("logo/old.txt")), "old logo not replaced");
    check(QFileInfo::exists(QDir(images).filePath("logo/new.txt")), "new logo missing");
    for (const QString &name :
         {QStringLiteral("pic"), QStringLiteral("tl"), QStringLiteral("video")})
        check(QFileInfo::exists(QDir(images).filePath(name + "/old.txt")),
              "absent directory erased");
    const auto failed = upload("resource-bad.zip");
    check(failed.value("state") == "FAILED" && !failed.value("error").toString().isEmpty(),
          "unsafe package accepted or error missing");
    check(QFileInfo::exists(QDir(images).filePath("logo/new.txt")),
          "bad package touched resources");
    const auto rar = upload("resource.rar");
    if (rar.value("state") != "READY")
        throw std::runtime_error(rar.value("error").toString().toStdString());
    check(rar.value("restartScheduled").toBool(), "RAR update did not schedule Qt restart");
    check(QFileInfo::exists(QDir(images).filePath(QStringLiteral("logo/层级/中文.txt"))),
          "RAR upload lost Unicode resource filename");
    for (const QString &name :
         {QStringLiteral("pic"), QStringLiteral("tl"), QStringLiteral("video")})
        check(QFileInfo::exists(QDir(images).filePath(name + "/old.txt")),
              "RAR update erased absent directory");
    QFile restart(QDir(root).filePath("resource-restart.log"));
    check(restart.open(QIODevice::ReadOnly), "Qt restart command not recorded");
    const auto invocation = QJsonDocument::fromJson(restart.readLine()).array();
    const auto rarInvocation = QJsonDocument::fromJson(restart.readLine()).array();
    check(restart.atEnd() && rarInvocation.contains("--on-active=3s") &&
              rarInvocation.contains("apply-layout") && invocation.contains("--on-active=3s") &&
              invocation.contains("apply-layout") &&
              invocation.contains(QDir(images).filePath("logo/ycest_cfg.ini")),
          "restart repeated, missing delay, or bypassed existing layout entry");
    check(ticks > 0 && progress.count() > 4, "resource worker blocked UI or emitted no progress");
    bool extracted = false, complete = false;
    for (const auto &item : progress) {
        extracted = extracted || item.at(1).toString().contains(QStringLiteral("解压"));
        complete = complete || (item.at(2).toInt() == 100 && item.at(3).toBool());
    }
    check(extracted && complete,
          "native resource progress did not cover extraction and completion");
}

/**
 * @brief 初始化运行环境并执行模块主流程。
 * @param argc 命令行参数个数。
 * @param argv 命令行参数数组。
 * @return 全部测试通过返回 0；断言或异常失败返回非零值。
 */
int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    // 原生测试调度器只记录命令，任何测试都不重启真实 Qt 或系统服务。
    if (argc >= 3 && QByteArray(argv[1]) == "record-resource-restart") {
        QFile record(QString::fromLocal8Bit(argv[2]));
        if (!record.open(QIODevice::Append))
            return 1;
        QJsonArray arguments;
        for (int i = 3; i < argc; ++i)
            arguments.append(QString::fromLocal8Bit(argv[i]));
        record.write(QJsonDocument(arguments).toJson(QJsonDocument::Compact) + '\n');
        return 0;
    }
    // 开发测试入口调用与正式 QThread 相同的 C++ 更新逻辑，Python 仅负责造包。
    if (argc == 5 &&
        (QByteArray(argv[1]) == "resource-task" || QByteArray(argv[1]) == "resource-recover")) {
        const QString resultPath = QDir::fromNativeSeparators(QString::fromLocal8Bit(argv[4]));
        const auto result = WebResourceUpdate::execute(
            {QString::fromLocal8Bit(argv[2]),
             QString::fromLocal8Bit(argv[3]),
             resultPath,
             {QCoreApplication::applicationFilePath(),
              "record-resource-restart",
              QDir(QFileInfo(resultPath).absolutePath()).filePath("restart.log")},
             QByteArray(argv[1]) == "resource-recover"});
        const auto data = QJsonDocument(result).toJson(QJsonDocument::Compact);
        std::fwrite(data.constData(), 1, size_t(data.size()), stdout);
        return result.value("state") == "READY" ? 0 : 1;
    }
    if (argc >= 2 && (QByteArray(argv[1]) == "ssid" || QByteArray(argv[1]) == "save" ||
                      QByteArray(argv[1]) == "request-refresh")) {
        const QDir root(QString::fromLocal8Bit(qgetenv("WEB_TEST_NM_ROOT")));
        QFile ssid(root.filePath("nm-ssid"));
        const QByteArray action(argv[1]);
        if (action == "ssid") {
            if (!ssid.open(QIODevice::ReadOnly))
                return 1;
            const auto data = ssid.readAll();
            std::fwrite(data.constData(), 1, size_t(data.size()), stdout);
            return 0;
        }
        if (!QFileInfo::exists(root.filePath("nm-active")))
            return 1;
        QFile log(root.filePath("nm-commands"));
        if (!log.open(QIODevice::Append))
            return 1;
        log.write(action + '\n');
        if (action == "save") {
            if ((argc != 3 && argc != 4) || !ssid.open(QIODevice::WriteOnly))
                return 1;
            if (argc == 4) {
                if (QByteArray(argv[3]) != "--password-stdin")
                    return 1;
                QFile input;
                if (!input.open(stdin, QIODevice::ReadOnly))
                    return 1;
                const auto password = input.readLine();
                QFile output(root.filePath("nm-password"));
                if (!output.open(QIODevice::WriteOnly) || output.write(password) != password.size())
                    return 1;
            }
            ssid.write(QString::fromLocal8Bit(argv[2]).toUtf8() + '\n');
        }
        return action == "request-refresh" && QFileInfo::exists(root.filePath("nm-fail-refresh"))
                   ? 1
                   : 0;
    }
    try {
        check(argc == 2, "usage: qt_web_tests fixture.xlsx");
        QTemporaryDir temporary;
        check(temporary.isValid(), "temp directory failed");
        qputenv("WEB_TEST_ROOT", temporary.path().toLocal8Bit());
        const QString root = temporary.path(),
                      media = QDir(root).filePath("static/demoResources/images/video"),
                      imports = QDir(root).filePath("imports");
        QDir().mkpath(media);
        QDir().mkpath(imports);
        const QString credentials = QDir(root).filePath("factory.json");
        factory(credentials);

        // 所有文件/上传/概览回归均使用普通目录，并传入无对应挂载的旧配置值。
        WebService service({Rk3566Platform::databasePath(),
                            media,
                            QDir(root).filePath("unused-mount-hint/missing"),
                            imports,
                            credentials,
                            QDir(root).filePath("net_cfg.ini"),
                            mockHotspotHelper(root),
                            QDir(root).filePath("web-ready"),
                            QDir(root).filePath("resources"),
                            QCoreApplication::applicationFilePath(),
                            QStringList{QCoreApplication::applicationFilePath(),
                                        "record-resource-restart",
                                        QDir(root).filePath("resource-restart.log")}});
        service.initialize();
        schemaAndAuthentication();
        emmcStorage(service, root, media);
        hotspotConfiguration(service,
                             QDir(root).filePath("net_cfg.ini"),
                             root,
                             QDir(root).filePath("web-ready"));
        duplicateThreshold();
        rejects(400, []() {
            WebPaths::filename("../escape.mp4");
        });
        rejects(400, [&]() {
            WebPaths::inside(QDir(root).filePath("escape.mp4"), media);
        });
        rejects(401, [&]() {
            service.dispatch("files.list", {}, 0);
        });
        rejects(404, [&]() {
            service.dispatch("device.play", {}, 1);
        });
        check(LocalImportGate::acquire(), "gate initial acquisition");
        check(!LocalImportGate::acquire(), "gate overlap allowed");
        LocalImportGate::release();
        excelUploadAndImport(service, QString::fromLocal8Bit(argv[1]), root);
        interruptedBatches(service, QString::fromLocal8Bit(argv[1]), imports);
        videoUploadDevice(service);
        mediaDirectory(service, media);
        localBusiness(service, root);
        sharedElevatorModes(service);
        independentFloorSchedules();
        advertisementImages(service, media);
        networkConfiguration(service, root);
        resourcePackageUpload(service, root, QString::fromLocal8Bit(argv[1]));
        resourceRollback(root, QString::fromLocal8Bit(argv[1]));
        resourceDeviceProgress();
        DbStore::close();
        std::fprintf(stdout, "Qt Web integration checks passed\n");
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr,
                     "Qt Web test failed: %s\nSQL: %s\n",
                     error.what(),
                     DbStore::lastError().toUtf8().constData());
        DbStore::close();
        return 1;
    }
}
