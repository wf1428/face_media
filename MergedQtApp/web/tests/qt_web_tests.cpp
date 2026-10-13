/**
 * @file qt_web_tests.cpp
 * @brief 实际 SQLite/文件/原 Excel 解析与本地仓储集成测试，以及阈值防重测试。
 * @author Du lin
 * @date 2026-10-09
 */
#include "modules/web/web_service.h"
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
#include <QFile>
#include <QDir>
#include <QJsonDocument>
#include <QPasswordDigestor>
#include <QTemporaryDir>
#include <QTest>
#include <QStorageInfo>
#include <functional>
#include <cstring>
#include <cstdio>

namespace {
/** @brief 不受 release/NDEBUG 影响的检查。 */
void check(bool value,const char *message) { if (!value) throw std::runtime_error(message); }
/** @brief 检查指定操作必定被拒绝，并核对状态码。 */
void rejects(int status,const std::function<void()> &action) {
    try { action(); } catch (const WebError &e) { check(e.status==status,"unexpected error status"); return; }
    throw std::runtime_error("unsafe operation unexpectedly accepted");
}
/** @return 生成质量提取后候选结构，向量用真实 float 二进制。 */
QJsonObject face(qint64 person,float x,float y) {
    float vector[2]={x,y}; const QByteArray bytes(reinterpret_cast<const char *>(vector),sizeof(vector));
    return {{"localPersonId",QString::number(person)},{"personName",QString::number(person)},{"featureBase64",QString::fromLatin1(bytes.toBase64())},{"modelVersion","test-v1"}};
}
/** @brief 测试账号由与工厂工具相同的 PBKDF2 算法生成，无生产默认密码。 */
void factory(const QString &path) {
    const QByteArray salt(32,'s');
    const auto hash=QPasswordDigestor::deriveKeyPbkdf2(QCryptographicHash::Sha256,QByteArray("test-secret"),salt,210000,32);
    QFile file(path); check(file.open(QIODevice::WriteOnly),"factory fixture write");
    file.write(QJsonDocument(QJsonObject{{"username","tester"},{"salt_hex",QString::fromLatin1(salt.toHex())},{"hash_hex",QString::fromLatin1(hash.toHex())},{"iterations",210000}}).toJson());
}
/** @brief 验证 Web 数据库没有第二份人员模型、迁移幂等及账号摘要校验。 */
void schemaAndAuthentication() {
    WebSchema::migrate(); WebSchema::migrate();
    check(WebSql::query("SELECT * FROM web_schema_versions").size()==1,"migration idempotency");
    check(WebAccounts::authenticate("tester","test-secret").value("id").toInt()==1,"correct credential rejected");
    rejects(401,[]() { WebAccounts::authenticate("tester","wrong"); });
    rejects(401,[]() { WebAccounts::authenticate("missing","test-secret"); });
}
/** @brief 验证网络下发同一参数可调整防重结果、自身更新允许、同批和跨库冲突拒绝。 */
void duplicateThreshold() {
    FaceRecord other; other.person.id=2; other.person.name="已有人员"; other.feature.modelVersion="test-v1";
    other.feature.blob=QByteArray::fromBase64(face(2,1,0).value("featureBase64").toString().toLatin1());
    QJsonArray accepted,failed;
    WebFacePolicy::filter({face(1,0.8f,0.6f)},{other},{},0.75f,&accepted,&failed);
    check(accepted.isEmpty() && failed.size()==1,"local threshold conflict missed");
    accepted={}; failed={}; WebFacePolicy::filter({face(1,0.8f,0.6f)},{other},{},0.9f,&accepted,&failed);
    check(accepted.size()==1 && failed.isEmpty(),"threshold adjustment ignored");
    accepted={}; failed={}; WebFacePolicy::filter({face(2,1,0)},{other},{},0.5f,&accepted,&failed);
    check(accepted.size()==1,"self replacement rejected");
    accepted={}; failed={}; WebFacePolicy::filter({face(1,1,0)},{},{other},0.5f,&accepted,&failed);
    check(accepted.isEmpty() && failed.size()==1,"network duplicate missed");
    accepted={}; failed={}; WebFacePolicy::filter({face(1,1,0),face(1,1,0),face(2,1,0)},{},{},0.5f,&accepted,&failed);
    check(accepted.size()==1 && failed.size()==1,"same batch duplicate semantics");
    other.feature.modelVersion="other-model"; accepted={}; failed={}; WebFacePolicy::filter({face(1,1,0)},{other},{},0.5f,&accepted,&failed);
    check(accepted.isEmpty(),"incompatible gallery model accepted");
}
/** @brief 使用上传任务实际写盘、原解析器和原仓储核对部分成功及网络/卡号冲突。 */
void excelUploadAndImport(WebService &service,const QString &fixture,const QString &root) {
    LocalPersonnelStore store; check(store.initialize(),"local schema initialize");
    WebSql::execute("CREATE TABLE network_person(person_id TEXT PRIMARY KEY,name TEXT,phone TEXT,person_json TEXT,active INTEGER,deleted INTEGER,status INTEGER)");
    WebSql::execute("CREATE TABLE network_face(id INTEGER PRIMARY KEY,person_id TEXT,status TEXT,feature_blob BLOB,model_version TEXT)");
    WebSql::execute("CREATE TABLE network_ic_card(card_id TEXT,person_id TEXT)");
    WebSql::execute("INSERT INTO network_person VALUES('network_a','网络重复','13812345680','{}',1,0,1)");
    WebSql::execute("INSERT INTO network_ic_card VALUES('ABCD1234','network_a')");
    const auto parsed=PersonXlsxParser::parse(fixture);
    check(parsed.ok && parsed.records.size()==3 && parsed.warnings.size()>=2,"original Excel validation mismatch");
    const qint64 size=QFileInfo(fixture).size();
    const auto batch=service.dispatch("imports.create",{{"manifest",QJsonArray{QJsonObject{{"kind","person_excel"},{"name","任意目录上传.xlsx"},{"size",double(size)}}}}},1); const QString batchId=batch.value("id").toString();
    auto task=service.dispatch("uploads.create",{{"kind","person_excel"},{"name","任意目录上传.xlsx"},{"size",double(size)},{"batch_id",batchId}},1);
    const QString id=task.value("id").toString();
    rejects(404,[&]() { WebUploads::get(id,2); });
    auto start=service.dispatch("uploads.begin",{{"id",id}},1); const QString part=start.value("part_path").toString();
    rejects(409,[&]() { service.dispatch("uploads.commit",{{"id",id}},1); });
    check(QFile::copy(fixture,part),"upload file copy");
    service.dispatch("uploads.received",{{"id",id}},1);
    auto committed=service.dispatch("uploads.commit",{{"id",id}},1);
    check(committed.value("state")=="READY" && !QFileInfo::exists(part),"Excel atomic publish failed");
    check(!committed.contains("part_path") && !committed.contains("final_path"),"internal paths leaked");
    check(service.dispatch("uploads.commit",{{"id",id}},1).value("state")=="READY","commit not idempotent");
    service.dispatch("imports.start",{{"id",batchId}},1);
    for (int i=0;i<50;++i) { QCoreApplication::processEvents(); if (WebImports::get(batchId).value("state")=="PARTIAL_FAILED") break; QTest::qWait(10); }
    const auto done=WebImports::get(batchId);
    check(done.value("state")=="PARTIAL_FAILED","batch did not finish partially");
    check(done.value("result").toObject().value("inserted").toInt()==1 && done.value("result").toObject().value("failed").toInt()==2,"original partial success/conflict mismatch");
    check(WebSql::query("SELECT id FROM person WHERE account='web_a'").size()==1,"local person missing");
    check(WebSql::query("SELECT * FROM network_person").size()==1,"network personnel modified");
    WebSql::execute("CREATE TABLE network_access_rule(id INTEGER)");
    rejects(503,[]() { WebPeople::verifyConflictSources(); });
    WebSql::execute("DROP TABLE network_access_rule");
    service.dispatch("imports.start",{{"id",batchId}},1);
    check(WebSql::query("SELECT id FROM person").size()==1,"same batch replayed");
    check(QFileInfo::exists(QDir(root).filePath("access_control.db")),"unexpected database path");
}
/** @brief 核对缺少图片不能封存、取消清理、接收中断及不确定批次不重放。 */
void interruptedBatches(WebService &service,const QString &fixture,const QString &imports) {
    const qint64 size=QFileInfo(fixture).size();
    const QJsonArray manifest={QJsonObject{{"name","retry.xlsx"},{"kind","person_excel"},{"size",double(size)}},QJsonObject{{"name","web_a_1.jpg"},{"kind","person_face"},{"size",100}}};
    const auto batch=service.dispatch("imports.create",{{"manifest",manifest}},1); const auto batchId=batch.value("id").toString();
    const auto task=service.dispatch("uploads.create",{{"kind","person_excel"},{"name","retry.xlsx"},{"size",double(size)},{"batch_id",batchId}},1); const auto taskId=task.value("id").toString();
    const auto start=service.dispatch("uploads.begin",{{"id",taskId}},1);
    check(QFile::copy(fixture,start.value("part_path").toString()),"manifest fixture copy failed");
    service.dispatch("uploads.received",{{"id",taskId}},1); service.dispatch("uploads.commit",{{"id",taskId}},1);
    rejects(409,[&]() { service.dispatch("imports.start",{{"id",batchId}},1); });
    const auto status=service.dispatch("imports.status",{{"id",batchId}},1);
    check(status.value("assets").toArray().size()==1 && status.value("result").toObject().value("manifest").toArray().size()==2,"refresh lost original manifest");
    service.dispatch("imports.discard",{{"id",batchId}},1);
    check(!QFileInfo::exists(QDir(imports).filePath(batchId)),"discard left staging files");
    const auto restart=service.dispatch("imports.create",{{"manifest",QJsonArray{manifest.first()}}},1); const auto restartId=restart.value("id").toString();
    const auto pending=service.dispatch("uploads.create",{{"kind","person_excel"},{"name","retry.xlsx"},{"size",double(size)},{"batch_id",restartId}},1); const auto pendingId=pending.value("id").toString();
    const auto receiving=service.dispatch("uploads.begin",{{"id",pendingId}},1);
    QFile part(receiving.value("part_path").toString()); check(part.open(QIODevice::WriteOnly),"partial write failed"); part.write("partial"); part.close();
    rejects(400,[&]() { service.dispatch("uploads.received",{{"id",pendingId}},1); });
    service.initialize();
    check(service.dispatch("uploads.status",{{"id",pendingId}},1).value("state")=="FAILED" && !QFileInfo::exists(part.fileName()),"restart did not clear interrupted receive");
    WebImports::setState(restartId,"IMPORTING"); service.initialize();
    check(service.dispatch("imports.start",{{"id",restartId}},1).value("state")=="RECOVERY_REQUIRED","uncertain import replayed");
    check(WebSql::query("SELECT id FROM person").size()==1,"recovery changed personnel");
}
/** @brief 验证旧目录文件也可列举与删除，.part 不暴露，未接入业务不创建路由。 */
void mediaDirectory(WebService &service,const QString &media) {
    QFile video(QDir(media).filePath("ftp-existing.mp4")); check(video.open(QIODevice::WriteOnly),"existing fixture open"); video.write("existing directory fixture"); video.close();
    QFile part(QDir(media).filePath("partial.mp4.part")); check(part.open(QIODevice::WriteOnly),"part fixture open"); part.write("not complete"); part.close();
    const auto files=service.dispatch("files.list",{},1).value("files").toArray();
    check(files.size()==1 && files.first().toObject().value("name")=="ftp-existing.mp4","temporary exposed or existing file omitted");
    const auto id=files.first().toObject().value("id").toString();
    check(video.open(QIODevice::Append),"replace fixture open"); video.write("replacement"); video.close();
    rejects(409,[&]() { service.dispatch("files.delete",{{"id",id}},1); });
    const auto changed=service.dispatch("files.list",{},1).value("files").toArray().first().toObject().value("id").toString();
    check(changed!=id,"same-path replacement kept stale identity");
    rejects(404,[&]() { service.dispatch("files.delete",{{"id",id}},1); });
    service.dispatch("files.delete",{{"id",changed}},1);
    check(!QFileInfo::exists(video.fileName()) && QFileInfo::exists(part.fileName()),"delete affected wrong file");
    check(service.dispatch("files.list",{},1).value("files").toArray().isEmpty(),"deleted media still listed");
    rejects(404,[&]() { service.dispatch("files.get",{{"id",id}},1); });
}
}
int main(int argc,char **argv) {
    QCoreApplication app(argc,argv);
    try {
        check(argc==2,"usage: qt_web_tests fixture.xlsx"); QTemporaryDir temporary; check(temporary.isValid(),"temp directory failed");
        qputenv("WEB_TEST_ROOT",temporary.path().toLocal8Bit());
        const QString root=temporary.path(),media=QDir(root).filePath("static/demoResources/images/video"),imports=QDir(root).filePath("imports");
        QDir().mkpath(media); QDir().mkpath(imports); const QString credentials=QDir(root).filePath("factory.json"); factory(credentials);
        // 测试允许临时目录所在实际文件系统；生产仍固定检查 /mnt/UDISK，不绕过挂载检查。
        WebService service({Rk3566Platform::databasePath(),media,QStorageInfo(media).rootPath(),imports,credentials,"ffprobe"}); service.initialize();
        schemaAndAuthentication(); duplicateThreshold();
        rejects(400,[]() { WebPaths::filename("../escape.mp4"); });
        rejects(400,[&]() { WebPaths::inside(QDir(root).filePath("escape.mp4"),media); });
        rejects(401,[&]() { service.dispatch("files.list",{},0); });
        rejects(404,[&]() { service.dispatch("device.play",{},1); });
        check(LocalImportGate::acquire(),"gate initial acquisition"); check(!LocalImportGate::acquire(),"gate overlap allowed"); LocalImportGate::release();
        excelUploadAndImport(service,QString::fromLocal8Bit(argv[1]),root);
        interruptedBatches(service,QString::fromLocal8Bit(argv[1]),imports);
        mediaDirectory(service,media);
        DbStore::close(); std::fprintf(stdout,"Qt Web integration checks passed\n"); return 0;
    } catch (const std::exception &error) { std::fprintf(stderr,"Qt Web test failed: %s\nSQL: %s\n",error.what(),DbStore::lastError().toUtf8().constData()); DbStore::close(); return 1; }
}
