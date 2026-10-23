/**
 * @file web_face_policy.cpp
 * @brief 沿用网络下发防重的 float 向量余弦/阈值范围，Web 不替换其他身份的人脸。
 * @author Du lin
 * @date 2026-10-09
 */

#include "web_face_policy.h"
#include "common/sql/web/web_sql.h"
#include <cmath>
#include <cstring>

namespace {

/**
 * @brief 计算以 float 字节保存的两个人脸特征的余弦相似度。
 * @param left 第一个待比较的字节序列。
 * @param right 第二个待比较的字节序列。
 * @return float 特征余弦；复制到对齐 float 后计算，坏值/维度不一致返回 -1。
 */
float cosine(const QByteArray &left, const QByteArray &right)
{
    if (left.isEmpty() || left.size() != right.size() || left.size() % sizeof(float))
        return -1;
    double dot = 0, a = 0, b = 0;
    for (int i = 0; i < left.size(); i += sizeof(float)) {
        float x = 0, y = 0;
        std::memcpy(&x, left.constData() + i, sizeof(float));
        std::memcpy(&y, right.constData() + i, sizeof(float));
        if (!std::isfinite(x) || !std::isfinite(y))
            return -1;
        dot += double(x) * y;
        a += double(x) * x;
        b += double(y) * y;
    }
    return a > 0 && b > 0 ? float(dot / (std::sqrt(a) * std::sqrt(b))) : -1;
}
} // namespace

/**
 * @brief 依据本地和网络人脸库及下发阈值筛选待导入人脸。
 * @param input 待校验的输入数据。
 * @param local 本地人员图库。
 * @param network 网络人员图库。
 * @param configured 配置的余弦防重阈值，使用时限制在 [0, 1]。
 * @param[out] accepted 追加通过防重的人脸，调用方提供有效数组指针。
 * @param[out] failed 追加失败条目及原因，调用方提供有效数组指针。
 */
void WebFacePolicy::filter(const QJsonArray &input,
                           const QVector<FaceRecord> &local,
                           const QVector<FaceRecord> &network,
                           float configured,
                           QJsonArray *accepted,
                           QJsonArray *failed)
{
    // 防重阈值沿用网络人员下发参数，并限制在余弦相似度的有效范围。
    const float threshold = qBound(0.0f, configured, 1.0f);
    for (const auto &value : input) {
        auto face = value.toObject();
        const auto blob = QByteArray::fromBase64(face.value("featureBase64").toString().toLatin1());
        const qint64 self = face.value("localPersonId").toString().toLongLong();
        QString reason;

        // 同一比较规则遍历本地或网络图库；本地当前人员自身不算重复。
        const auto check = [&](const QVector<FaceRecord> &records, bool fromNetwork) {
            for (const auto &record : records) {
                if (!fromNetwork && record.person.id == self)
                    continue;
                if (record.feature.modelVersion != face.value("modelVersion").toString() ||
                    record.feature.blob.size() != blob.size() ||
                    cosine(record.feature.blob, record.feature.blob) < 0) {
                    reason = QStringLiteral("已有图库模型或特征不一致，无法安全校验重复人脸");
                    return;
                }
                if (cosine(blob, record.feature.blob) >= threshold) {
                    reason = QStringLiteral("人脸与%1人员“%2”重复")
                                 .arg(fromNetwork ? QStringLiteral("网络") : QStringLiteral("本地"),
                                      record.person.name);
                    return;
                }
            }
        };
        if (self <= 0 || face.value("modelVersion").toString().isEmpty() || cosine(blob, blob) < 0)
            reason = QStringLiteral("待导入人脸特征无效");

        // 依次与本地图库、网络图库以及本批已经接收的人脸比较。
        if (reason.isEmpty())
            check(local, false);
        if (reason.isEmpty())
            check(network, true);
        bool duplicateSelf = false;
        if (reason.isEmpty())
            for (const auto &value : *accepted) {
                const auto other = value.toObject();
                if (cosine(blob,
                           QByteArray::fromBase64(
                               other.value("featureBase64").toString().toLatin1())) < threshold)
                    continue;
                if (other.value("localPersonId") == face.value("localPersonId"))
                    duplicateSelf = true;
                else
                    reason = QStringLiteral("人脸与本批人员“%1”重复")
                                 .arg(other.value("personName").toString());
                break;
            }
        if (!reason.isEmpty()) {
            face.insert("message", reason);
            failed->append(face);
        } else if (!duplicateSelf) {
            face.insert("duplicateThreshold", double(threshold));
            accepted->append(face);
        }
    }
}

/**
 * @brief 读取本地和网络人员的人脸特征用于防重。
 * @param[out] local 输出本地图库，先清空再填入，不可为空。
 * @param[out] network 输出网络图库，先清空再填入，不可为空。
 */
void WebFacePolicy::currentGalleries(QVector<FaceRecord> *local, QVector<FaceRecord> *network)
{
    // 两类人员共享统一数据库，但各自从对应的人员和人脸表取特征。
    local->clear();
    network->clear();

    // 将两类 SQL 查询结果转换为统一的比较记录。
    const auto append = [](const QList<QVariantMap> &rows, QVector<FaceRecord> *output) {
        for (const auto &row : rows) {
            FaceRecord face;
            face.person.id = row.value("local_id").toLongLong();
            face.person.name = row.value("name").toString();
            face.feature.blob = row.value("feature_blob").toByteArray();
            face.feature.modelVersion = row.value("model_version").toString();
            output->append(face);
        }
    };
    append(WebSql::query(QStringLiteral(
               "SELECT p.id AS local_id,p.name,f.feature_blob,f.model_version FROM person p JOIN "
               "face_feature f ON f.person_id=p.id WHERE p.enabled=1 AND COALESCE(p.deleted,0)=0")),
           local);
    const auto tables =
        WebSql::query(QStringLiteral("SELECT name FROM sqlite_master WHERE type='table' AND name "
                                     "IN ('network_person','network_face')"));
    if (tables.isEmpty())
        return; // 新出厂尚无网络模型时，空网络图库合法。
    if (tables.size() != 2)
        throw WebError(503, QStringLiteral("网络图库结构未就绪"));
    append(WebSql::query(QStringLiteral(
               "SELECT 0 AS local_id,p.name,f.feature_blob,f.model_version FROM network_person p "
               "JOIN network_face f ON f.person_id=p.person_id WHERE p.active=1 AND p.deleted=0 "
               "AND p.status=1 AND f.status='IMAGE_READY' AND f.feature_blob IS NOT NULL")),
           network);
}
