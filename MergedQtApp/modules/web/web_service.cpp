/**
 * @file web_service.cpp
 * @brief Web 命令协调；不链接 HTTP 库、不控制播放/空调/电梯、不发送 MQTT。
 * @author Du lin
 * @date 2026-10-09
 */
#include "web_service.h"
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
#ifdef Q_OS_UNIX
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace {
constexpr qint64 MiB = 1024LL*1024; /**< 文件资源预算单位。 */
/** @return 不带大括号的随机受控标识。 */
QString uuid() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
/** @return 字符串清单的 JSON 数组。 */
QJsonArray strings(const QStringList &values) { QJsonArray result; for (const auto &v:values) result.append(v); return result; }
/** @brief 不保留内部磁盘路径的业务失败数组追加。 */
void appendFailures(QJsonObject &result,const QJsonArray &faces) {
    QJsonArray failures=result.value("failedNames").toArray();
    for (const auto &value:faces) {
        const auto face=value.toObject();
        const QString item=face.value("personName").toString()+QStringLiteral("（")+face.value("message").toString()+QStringLiteral("）");
        if (!failures.contains(item)) failures.append(item);
    }
    result.insert("failedNames",failures);
    result.insert("failed",failures.size());
}
}

WebService::WebService(const WebServiceConfig &config,QObject *parent) : QObject(parent),config_(config) {}
WebService::~WebService() { if (importLease_) LocalImportGate::release(); }

void WebService::initialize() {
    if (!DbStore::bootstrap(config_.database)) throw WebError(503,QStringLiteral("统一数据库无法打开"));
    WebSchema::migrate();
    WebAccounts::provision(config_.factory);
    if (!QDir().mkpath(config_.imports) || QFileInfo(config_.imports).isSymLink()) throw WebError(503,QStringLiteral("人员上传缓存目录不可用"));
    WebImports::recover();
    recover();
}

QJsonObject WebService::publicTask(const QVariantMap &task) const {
    QJsonObject result=WebSql::object(task);
    result.remove("part_path"); result.remove("final_path"); result.remove("owner_id");
    return result;
}

void WebService::checkBudget(const QString &kind,qint64 incoming) {
    const bool video=kind=="video";
    const QString root=video?config_.media:config_.imports;
    if (video) WebPaths::mounted(root,config_.mount);
    const qint64 used=video?StoragePolicy::mediaUsage().bytes:WebPaths::usage(root);
    const qint64 reserved=WebUploads::reserved(video?QStringLiteral("video"):QStringLiteral("person"));
    const qint64 limit=video?StoragePolicy::mediaLimitBytes():200*MiB;
    QStorageInfo storage(root); storage.refresh();
    if (incoming<0 || used+reserved+incoming>limit || !storage.isReady() || storage.bytesAvailable()<reserved+incoming+64*MiB)
        throw WebError(507,QStringLiteral("设备可用空间或上传配额不足"));
}

QJsonObject WebService::createUpload(const QJsonObject &args,int owner) {
    const QString kind=args.value("kind").toString(), name=WebPaths::filename(args.value("name").toString());
    const double sizeValue=args.value("size").toDouble(-1);
    if (sizeValue<=0 || sizeValue>8.0*1024*1024*1024) throw WebError(400,QStringLiteral("文件大小不符合要求"));
    const qint64 size=static_cast<qint64>(sizeValue);
    const qint64 maxSize=kind=="video"?StoragePolicy::mediaLimitBytes():(kind=="person_excel"?20*MiB:10*MiB);
    if (!WebPaths::accepts(kind,name) || size<=0 || sizeValue!=double(size) || size>maxSize) throw WebError(400,QStringLiteral("文件类型或大小不符合要求"));
    // 限制同时待上传任务，避免大量小任务挤占设备资源。
    const auto active=WebUploads::incomplete();
    if (active.size()>=8) throw WebError(429,QStringLiteral("待上传任务过多，请先完成或取消"));
    for (const auto &task:active) if (kind=="video" && task.value("kind")=="video") throw WebError(409,QStringLiteral("已有视频上传任务，请先完成或取消"));
    QString root=config_.media, target;
    QVariant batch;
    const QString id=uuid(),suffix=QFileInfo(name).suffix().toLower();
    if (kind!="video") {
        batch=args.value("batch_id").toString();
        const auto import=WebImports::get(batch.toString(),owner);
        if (import.value("state")!="OPEN") throw WebError(409,QStringLiteral("导入批次已经封存"));
        bool declared=false;
        for (const auto &item:import.value("result").toObject().value("manifest").toArray()) {
            const auto file=item.toObject();
            if (file.value("name").toString()==name && file.value("kind")==kind && file.value("size").toDouble()==sizeValue) declared=true;
        }
        if (!declared) throw WebError(400,QStringLiteral("文件与本批次所选清单不一致"));
        const auto entries=WebSql::query(QStringLiteral("SELECT original_name,kind,state FROM web_upload_tasks WHERE batch_id=? AND state NOT IN ('FAILED','CANCELLED')"),{batch});
        if (entries.size()>=500) throw WebError(400,QStringLiteral("每批最多 500 份文件"));
        for (const auto &entry:entries) {
            if (entry.value("original_name").toString().compare(name,Qt::CaseInsensitive)==0 || (kind=="person_excel" && entry.value("kind")=="person_excel"))
                throw WebError(409,QStringLiteral("同一批次不能重复上传同名文件或多个 Excel"));
        }
        root=QDir(config_.imports).filePath(batch.toString());
        if (!QDir().mkpath(root)) throw WebError(503,QStringLiteral("无法创建批次目录"));
        WebPaths::inside(QDir(root).filePath("check"),config_.imports);
        WebPaths::uploadDirectory(root);
        target=QDir(root).filePath(id+'.'+suffix);
    } else {
        WebPaths::mounted(root,config_.mount);
        target=QDir(root).filePath(name);
        if (QFileInfo::exists(target)) throw WebError(409,QStringLiteral("同名视频已存在，请更换名称"));
        if (!QDir().mkpath(QDir(root).filePath("_uploads"))) throw WebError(503,QStringLiteral("无法创建视频缓存目录"));
        WebPaths::inside(QDir(root).filePath("_uploads/check"),config_.media);
        WebPaths::uploadDirectory(QDir(root).filePath("_uploads"));
    }
    const QString part=kind=="video"?QDir(root).filePath("_uploads/"+id+".part"):target+".part";
    WebPaths::inside(target,kind=="video"?config_.media:config_.imports);
    WebPaths::inside(part,kind=="video"?config_.media:config_.imports);
    checkBudget(kind,size);
    const QVariantMap task={{"id",id},{"owner_id",owner},{"kind",kind},{"batch_id",batch},{"original_name",name},{"expected_bytes",size},{"part_path",part},{"final_path",target}};
    WebUploads::create(task);
    auto result=publicTask(WebUploads::get(id,owner)); result.insert("part_path",part);
    return result;
}

QJsonObject WebService::commit(const QString &id,int owner) {
    auto task=WebUploads::get(id,owner);
    const QString state=task.value("state").toString(),kind=task.value("kind").toString();
    if (state=="READY") return publicTask(task);
    if (state!="RECEIVED" && state!="VALIDATING" && state!="COMMITTING") throw WebError(409,QStringLiteral("上传任务尚未接收完整或已终止"));
    const QString root=kind=="video"?config_.media:config_.imports;
    if (kind=="video") WebPaths::mounted(root,config_.mount);
    const QString part=task.value("part_path").toString(),final=task.value("final_path").toString();
    const bool renamed=state=="COMMITTING" && !QFileInfo::exists(part) && QFileInfo::exists(final);
    const QString candidate=renamed?final:part;
    WebPaths::inside(candidate,root,true);
    if (QFileInfo(candidate).size()!=task.value("expected_bytes").toLongLong()) throw WebError(400,QStringLiteral("上传文件长度不完整"));
    checkBudget(kind);
    WebUploads::setState(id,renamed?"COMMITTING":"VALIDATING",QFileInfo(candidate).size());
    if (kind=="video") {
        const GstVideoValidationResult validation=validateVideoWithGStreamer(candidate);
        if (validation.status!=GstVideoValidationStatus::Valid)
            throw WebError(validation.status==GstVideoValidationStatus::Unavailable?503:400,validation.message);
    } else if (kind=="person_excel") {
        // 临时文件以 .part 结尾，XLS 格式按原始后缀判断，预检使用最终路径副本规则。
        const bool xls=task.value("original_name").toString().endsWith(".xls",Qt::CaseInsensitive);
        if (xls) { QFile file(candidate); if (!file.open(QIODevice::ReadOnly) || file.read(8)!=QByteArray::fromHex("d0cf11e0a1b11ae1")) throw WebError(400,QStringLiteral("不是有效 XLS 文件")); }
        else WebPaths::validateExcel(candidate);
    } else {
        QImageReader reader(candidate); reader.setDecideFormatFromContent(true);
        const QSize dimensions=reader.size();
        const QByteArray format=reader.format().toLower();
        const QString ext=QFileInfo(task.value("original_name").toString()).suffix().toLower();
        if (!dimensions.isValid() || qint64(dimensions.width())*dimensions.height()>16000000 || !reader.canRead() || !(format==ext.toLatin1() || (format=="jpeg" && ext=="jpg"))) throw WebError(400,QStringLiteral("人脸图片格式、尺寸或扩展名不匹配"));
    }
    WebPaths::sync(candidate);
    if (!renamed) {
        if (QFileInfo::exists(final)) throw WebError(409,QStringLiteral("目标文件已存在，拒绝覆盖"));
        WebUploads::setState(id,"COMMITTING",QFileInfo(candidate).size());
        if (!QFile::rename(part,final)) throw WebError(503,QStringLiteral("文件发布失败，保留临时文件待重试"));
        WebPaths::sync(QFileInfo(part).absolutePath(),true);
    }
    WebPaths::sync(QFileInfo(final).absolutePath(),true);
    if (!DbStore::transactionBegin()) throw WebError(503,QStringLiteral("文件登记事务失败"));
    try {
        if (kind=="video") WebMedia::index(final,id); else WebImports::addAsset(task);
        WebUploads::setState(id,"READY",QFileInfo(final).size());
        if (!DbStore::transactionCommit()) throw WebError(503,QStringLiteral("文件登记提交失败"));
    } catch (...) { DbStore::transactionRollback(); throw; }
    return publicTask(WebUploads::get(id,owner));
}

QJsonObject WebService::files() {
    WebPaths::mounted(config_.media,config_.mount);
    QJsonArray items;
    for (const auto &file:QDir(config_.media).entryInfoList(QDir::Files | QDir::NoSymLinks,QDir::Name)) {
        if (!WebPaths::accepts("video",file.fileName())) continue;
        WebPaths::inside(file.absoluteFilePath(),config_.media,true);
        // 若是 COMMITTING 的 Web 文件，等待其数据库发布完成再向网页展示。
        if (!WebSql::query(QStringLiteral("SELECT id FROM web_upload_tasks WHERE final_path=? AND state NOT IN ('READY','FAILED','CANCELLED')"),{file.absoluteFilePath()}).isEmpty()) continue;
        WebMedia::index(file.absoluteFilePath());
        const auto rows=WebSql::query(QStringLiteral("SELECT id,name,bytes,modified_ms,state FROM web_media_files WHERE path=?"),{file.absoluteFilePath()});
        if (!rows.isEmpty()) items.append(WebSql::object(rows.first()));
    }
    return {{"files",items}};
}

QJsonObject WebService::dispatch(const QString &operation,const QJsonObject &args,int owner) {
    // 新 Web 连接的关键状态使用 FULL；原 Qt 连接配置不变。
    WebSql::execute(QStringLiteral("PRAGMA synchronous=FULL"));
    if (operation=="auth.login") return WebAccounts::authenticate(args.value("username").toString(),args.value("password").toString());
    if (operation=="health") return {{"ready",true},{"version",1}};
    if (owner<=0 || WebSql::query(QStringLiteral("SELECT id FROM web_accounts WHERE id=? AND enabled=1"),{owner}).isEmpty()) throw WebError(401,QStringLiteral("请先登录"));
    const QString id=args.value("id").toString();
    if (operation=="uploads.create") return createUpload(args,owner);
    if (operation.startsWith("uploads.")) {
        const auto task=WebUploads::get(id,owner);
        const QString state=task.value("state").toString(),kind=task.value("kind").toString();
        if (operation=="uploads.status") return publicTask(task);
        if (operation=="uploads.begin") {
            if (state!="CREATED") throw WebError(409,QStringLiteral("任务已接收或开始，不能重复传输"));
            checkBudget(kind);
            WebUploads::setState(id,"RECEIVING");
            return {{"part_path",task.value("part_path").toString()},{"size",double(task.value("expected_bytes").toLongLong())}};
        }
        if (operation=="uploads.progress" || operation=="uploads.received") {
            if (state!="RECEIVING") throw WebError(409,QStringLiteral("上传任务状态不允许继续接收"));
            const qint64 bytes=QFileInfo(task.value("part_path").toString()).size();
            if (bytes>task.value("expected_bytes").toLongLong()) throw WebError(400,QStringLiteral("接收长度超过声明大小"));
            checkBudget(kind);
            if (operation=="uploads.received" && bytes!=task.value("expected_bytes").toLongLong()) throw WebError(400,QStringLiteral("文件接收不完整"));
            WebUploads::setState(id,operation=="uploads.received"?"RECEIVED":"RECEIVING",bytes);
            return publicTask(WebUploads::get(id,owner));
        }
        if (operation=="uploads.commit") return commit(id,owner);
        if (operation=="uploads.cancel") {
            if (state=="READY" || state=="COMMITTING") throw WebError(409,QStringLiteral("任务已经发布或正在发布，不能取消"));
            const QString part=task.value("part_path").toString();
            WebPaths::inside(part,kind=="video"?config_.media:config_.imports);
#ifdef Q_OS_UNIX
            // daemon 崩溃而 Qt 仍运行时允许取消失去写入者的任务，活跃 flock 不可解除。
            if (state=="RECEIVING" && QFileInfo::exists(part)) {
                const int fd=::open(QFile::encodeName(part).constData(),O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
                if (fd<0) throw WebError(503,QStringLiteral("临时文件无法检查"));
                const bool unlocked=::flock(fd,LOCK_EX|LOCK_NB)==0; ::close(fd);
                if (!unlocked) throw WebError(409,QStringLiteral("文件仍在传输，请等待连接结束"));
            }
#endif
            if (QFileInfo::exists(part) && !QFile::remove(part)) throw WebError(503,QStringLiteral("临时文件清理失败"));
            WebUploads::setState(id,"CANCELLED"); return {{"cancelled",true}};
        }
        if (operation=="uploads.failed") {
            if (state!="RECEIVING") throw WebError(409,QStringLiteral("任务已停止接收"));
            const QString part=task.value("part_path").toString();
            WebPaths::inside(part,kind=="video"?config_.media:config_.imports);
            if (QFileInfo::exists(part) && !QFile::remove(part)) throw WebError(503,QStringLiteral("临时文件清理失败"));
            WebUploads::setState(id,"FAILED",0,QStringLiteral("传输未完成，请重新上传")); return {{"failed",true}};
        }
    }
    if (operation=="files.list") return files();
    if (operation=="persons.list") return WebPeople::list();
    if (operation=="persons.get") return WebPeople::get(id);
    if (operation=="files.get" || operation=="files.delete") {
        WebPaths::mounted(config_.media,config_.mount);
        const auto item=WebMedia::get(id);
        const QString path=item.value("path").toString();
        WebPaths::inside(path,config_.media,operation=="files.get");
        if (operation=="files.get") {
            if (item.value("state")!="READY") throw WebError(409,QStringLiteral("文件正在删除"));
            return {{"path",path},{"name",item.value("name").toString()},{"bytes",double(QFileInfo(path).size())}};
        }
#ifdef Q_OS_LINUX
        // 原播放器保持不变；检查当前进程实际打开的文件描述符，保护正在使用的视频。
        for (const auto &fd:QDir("/proc/self/fd").entryInfoList(QDir::Files | QDir::System | QDir::NoDotAndDotDot))
            if (QFileInfo(fd.absoluteFilePath()).symLinkTarget()==path) throw WebError(423,QStringLiteral("文件正在使用，请稍后重试"));
#endif
        WebMedia::deleting(id);
        if (QFileInfo::exists(path) && !QFile::remove(path)) throw WebError(503,QStringLiteral("删除失败，请重试"));
        WebPaths::sync(config_.media,true); WebMedia::removed(id); return {{"deleted",true}};
    }
    if (operation=="imports.create") {
        const auto manifest=args.value("manifest").toArray();
        if (manifest.isEmpty() || manifest.size()>500) throw WebError(400,QStringLiteral("请选择一份 Excel 和可选的人脸图片，每批最多 500 份文件"));
        QSet<QString> names; int excelCount=0; qint64 total=0;
        for (const auto &item:manifest) {
            const auto file=item.toObject(); const auto name=WebPaths::filename(file.value("name").toString());
            const auto kind=file.value("kind").toString(); const double size=file.value("size").toDouble(-1);
            if ((kind!="person_excel" && kind!="person_face") || !WebPaths::accepts(kind,name) || size<=0 || size>(kind=="person_excel"?20*MiB:10*MiB) || size!=double(qint64(size)) || names.contains(name.toCaseFolded()))
                throw WebError(400,QStringLiteral("所选文件清单存在格式、大小或重名问题"));
            names.insert(name.toCaseFolded()); total+=qint64(size); if (kind=="person_excel") ++excelCount;
        }
        if (excelCount!=1) throw WebError(400,QStringLiteral("每批必须包含且仅包含一份 Excel"));
        checkBudget("person_excel",total);
        return WebImports::get(WebImports::create(owner,manifest),owner);
    }
    if (operation=="imports.status") return WebImports::get(id,owner);
    if (operation=="imports.discard") {
        if (WebImports::get(id,owner).value("state")!="OPEN") throw WebError(409,QStringLiteral("已封存批次不能放弃或重放"));
        for (const auto &task:WebSql::query(QStringLiteral("SELECT id,state FROM web_upload_tasks WHERE batch_id=? AND state NOT IN ('READY','FAILED','CANCELLED')"),{id})) {
            if (task.value("state")=="RECEIVING" || task.value("state")=="COMMITTING") throw WebError(409,QStringLiteral("批次仍有文件传输或发布，请先查询上传任务"));
            dispatch("uploads.cancel",{{"id",task.value("id").toString()}},owner);
        }
        WebImports::setState(id,"CANCELLED"); cleanupBatch(id); return {{"cancelled",true}};
    }
    if (operation=="imports.start") {
        const auto batch=WebImports::get(id,owner);
        if (batch.value("state")!="OPEN") return batch; // 重复点击不重新执行人员写入。
        if (!currentBatch_.isEmpty()) throw WebError(409,QStringLiteral("已有本地人员导入任务"));
        const auto pending=WebSql::query(QStringLiteral("SELECT id FROM web_upload_tasks WHERE batch_id=? AND state NOT IN ('READY','FAILED','CANCELLED')"),{id});
        if (!pending.isEmpty()) throw WebError(409,QStringLiteral("批次文件尚未完成上传"));
        int excelCount=0; for (const auto &asset:WebImports::assets(id)) if (asset.value("kind")=="person_excel") ++excelCount;
        if (excelCount!=1) throw WebError(400,QStringLiteral("请上传一份 Excel 人员表"));
        const auto assets=WebImports::assets(id);
        const auto manifest=batch.value("result").toObject().value("manifest").toArray();
        if (manifest.size()!=assets.size()) throw WebError(409,QStringLiteral("所选清单尚未全部上传完成，不能导入部分文件"));
        for (const auto &item:manifest) {
            const auto file=item.toObject(); bool found=false;
            for (const auto &asset:assets) if (asset.value("original_name").toString()==file.value("name").toString() && asset.value("kind").toString()==file.value("kind").toString()) found=true;
            if (!found) throw WebError(409,QStringLiteral("批次上传文件与所选清单不一致"));
        }
        if (!LocalImportGate::acquire()) throw WebError(409,QStringLiteral("U 盘或 Web 正在导入本地人员，请稍后重试"));
        importLease_=true; currentBatch_=id;
        WebImports::setState(id,"IMPORTING");
        QTimer::singleShot(0,this,[this,id]() { importBatch(id); });
        return WebImports::get(id,owner);
    }
    throw WebError(404,QStringLiteral("本版本未接入该操作"));
}

void WebService::importBatch(const QString &id) {
    try {
        const auto assets=WebImports::assets(id);
        QString excel;
        for (const auto &asset:assets) if (asset.value("kind")=="person_excel") excel=asset.value("path").toString();
        WebPaths::inside(excel,config_.imports,true); WebPaths::validateExcel(excel);
        const auto parsed=excel.endsWith(".xls",Qt::CaseInsensitive)?PersonXlsParser::parse(excel):PersonXlsxParser::parse(excel);
        importResult_={{"warnings",strings(parsed.warnings)}};
        if (!parsed.ok) throw WebError(400,parsed.error);
        if (parsed.records.size()>5000) throw WebError(400,QStringLiteral("每批最多 5000 名人员"));
        QVector<QJsonObject> records; for (const auto &record:parsed.records) records.append(record.data);
        LocalPersonnelStore store;
        if (!store.initialize()) throw WebError(503,QStringLiteral("本地人员资料未就绪"));
        WebPeople::verifyConflictSources();
        const auto imported=store.importSpreadsheetPersonnel(records);
        importResult_.insert("inserted",imported.inserted); importResult_.insert("updated",imported.updated);
        importResult_.insert("unchanged",imported.unchanged); importResult_.insert("failed",imported.failed);
        importResult_.insert("failedNames",strings(imported.failedNames)); importResult_.insert("unchangedNames",strings(imported.unchangedNames));
        importResult_.insert("message",imported.error); importResult_.insert("savedFaces",0);
        if (imported.inserted+imported.updated>0) emit personnelChanged();
        QHash<QString,LocalImportedPerson> people; QSet<QString> ambiguous;
        for (const auto &person:imported.importedPeople) { const auto key=person.personNo.trimmed().toCaseFolded(); if (people.contains(key)) ambiguous.insert(key); else people.insert(key,person); }
        QJsonArray faces,warnings=importResult_.value("warnings").toArray();
        const QRegularExpression naming(QStringLiteral("^(.+)_([0-9]+)$"));
        for (const auto &asset:assets) {
            if (asset.value("kind")!="person_face") continue;
            const QString original=asset.value("original_name").toString();
            const auto match=naming.match(QFileInfo(original).completeBaseName());
            const QString key=match.captured(1).trimmed().toCaseFolded();
            if (!match.hasMatch() || !people.contains(key) || ambiguous.contains(key)) { warnings.append(QStringLiteral("跳过未能匹配本批成功人员的图片：%1").arg(original)); continue; }
            const auto person=people.value(key);
            const QString path=asset.value("path").toString(); WebPaths::inside(path,config_.imports,true);
            faces.append(QJsonObject{{"localPersonId",QString::number(person.personId)},{"personNo",person.personNo},{"personName",person.name},{"phone",person.phone},{"imagePath",path},{"sourceFileName",original}});
        }
        importResult_.insert("warnings",warnings);
        if (faces.isEmpty()) { finishImport(imported.failed>0 || !imported.error.isEmpty()?"PARTIAL_FAILED":"DONE"); return; }
        WebImports::setState(id,"VALIDATING_FACES",importResult_);
        emit faceValidationRequested(id,faces);
        QTimer::singleShot(120000,this,[this,id]() {
            if (currentBatch_!=id) return;
            importResult_.insert("message",QStringLiteral("人脸校验超时，人员成功部分已保留；请核对后新建批次"));
            finishImport("PARTIAL_FAILED");
        });
    } catch (const WebError &e) { importResult_.insert("message",e.message); finishImport("FAILED"); }
    catch (const std::exception &e) { qWarning()<<"[WEB] import exception"<<e.what(); importResult_.insert("message",QStringLiteral("人员导入异常，请核对成功部分")); finishImport("RECOVERY_REQUIRED"); }
}

void WebService::applyFaces(const QString &token,const QJsonArray &accepted,const QJsonArray &failed) {
    if (token!=currentBatch_ || currentBatch_.isEmpty()) return;
    try {
        WebImports::setState(token,"SAVING_FACES",importResult_);
        appendFailures(importResult_,failed);
        QVector<FaceRecord> local,network;
        WebFacePolicy::currentGalleries(&local,&network);
        QJsonArray verified,rejected;
        const float threshold=accepted.isEmpty()?1.0f:float(accepted.first().toObject().value("duplicateThreshold").toDouble());
        WebFacePolicy::filter(accepted,local,network,threshold,&verified,&rejected);
        appendFailures(importResult_,rejected);
        LocalPersonnelStore store; int saved=0; QStringList failures; QString error;
        if (!store.replaceValidatedFaces(verified,&saved,&failures,&error)) {
            auto list=importResult_.value("failedNames").toArray(); for (const auto &item:failures) if (!list.contains(item)) list.append(item);
            importResult_.insert("failedNames",list); importResult_.insert("failed",list.size()); importResult_.insert("message",error);
        }
        importResult_.insert("savedFaces",saved);
        if (saved>0) emit personnelChanged();
        finishImport(importResult_.value("failed").toInt()>0 || !importResult_.value("message").toString().isEmpty()?"PARTIAL_FAILED":"DONE");
    } catch (const WebError &e) { importResult_.insert("message",e.message); finishImport("RECOVERY_REQUIRED"); }
}
void WebService::finishImport(const QString &state) {
    const QString id=currentBatch_;
    try { WebImports::setState(id,state,importResult_); if (state!="RECOVERY_REQUIRED") cleanupBatch(id); }
    catch (const WebError &e) { qWarning()<<"[WEB] cannot persist import result"<<e.message; }
    currentBatch_.clear(); importResult_={};
    if (importLease_) { importLease_=false; LocalImportGate::release(); }
}
void WebService::cleanupBatch(const QString &id) {
    if (!QRegularExpression(QStringLiteral("^[a-f0-9-]{36}$")).match(id).hasMatch()) throw WebError(400,QStringLiteral("批次编号无效"));
    const QString path=QDir(config_.imports).filePath(id);
    if (!QFileInfo::exists(path)) return;
    WebPaths::inside(path,config_.imports);
    if (!QDir(path).removeRecursively()) throw WebError(503,QStringLiteral("批次中间文件清理失败，稍后由维护流程重试"));
}
void WebService::recover() {
    for (const auto &task:WebUploads::incomplete()) {
        const QString id=task.value("id").toString(),state=task.value("state").toString(),part=task.value("part_path").toString();
        try {
            if (state=="RECEIVED" || state=="VALIDATING" || state=="COMMITTING") commit(id,task.value("owner_id").toInt());
            else {
                WebPaths::inside(part,task.value("kind")=="video"?config_.media:config_.imports);
                if (QFileInfo::exists(part) && !QFile::remove(part)) throw WebError(503,QStringLiteral("残留临时文件清理失败"));
                WebUploads::setState(id,"FAILED",0,QStringLiteral("服务重启，未完成的接收已终止"));
            }
        } catch (const WebError &e) { qWarning()<<"[WEB] recovery deferred"<<id<<e.message; }
    }
    for (const auto &row:WebSql::query(QStringLiteral("SELECT id,path FROM web_media_files WHERE state='DELETING'")))
        if (!QFileInfo::exists(row.value("path").toString())) WebMedia::removed(row.value("id").toString());
    for (const auto &row:WebSql::query(QStringLiteral("SELECT id FROM web_person_import_batches WHERE state IN ('DONE','PARTIAL_FAILED','FAILED','CANCELLED')"))) {
        try { cleanupBatch(row.value("id").toString()); } catch (const WebError &error) { qWarning()<<"[WEB] deferred cache cleanup"<<error.message; }
    }
}
