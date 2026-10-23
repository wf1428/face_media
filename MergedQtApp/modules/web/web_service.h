/**
 * @file web_service.h
 * @brief Qt 独立 Web 工作线程：上传发布、文件清单和本地 Excel 批次。
 * @author Du lin
 * @date 2026-10-09
 * @note 最近修改：2026-10-23；修改人：Du lin；人员批次支持图片压缩包的后台解压及原图片匹配流程，退出时等待解压线程停止。
 */

#pragma once

#include <QObject>
#include <QJsonObject>
#include <QJsonArray>
#include <QVariantMap>
#include <QStringList>

class WebHotspot;
class WebLocalApi;
class WebResourceUpdate;
class QThread;

/** @brief 板端 Web 路径配置；生产由环境/平台确定，测试可以使用临时目录。 */
struct WebServiceConfig {
    QString database;      /**< 现有统一 SQLite 路径。 */
    QString media;         /**< 现有视频根目录，不由浏览器指定。 */
    QString mount;         /**< 旧媒体挂载点配置；保留兼容，不作为目录可用性条件。 */
    QString imports;       /**< Excel/图片批次暂存根目录。 */
    QString factory;       /**< 仅空 Web 账号库读入的工厂摘要文件。 */
    QString networkConfig; /**< 实际 net_cfg.ini 路径。 */
    QString hotspotHelper; /**< 固定的 NetworkManager 热点管理程序。 */
    QString webReadyFile;  /**< Qt 设备号及 Web 模块就绪标记，不依赖热点。 */
    /** @brief 固定的资源最终目录；与 U 盘资源目标一致，不由请求修改。 */
    QString resourceRoot = QStringLiteral("/mnt/UDISK/res/static/demoResources/images");
    /** @brief 资源更新使用的固定应用启停管理程序。 */
    QString resourceControl = QStringLiteral("/usr/local/sbin/weston-control");
    /** @brief systemd 独立定时任务负责重启，避免执行进程随 Qt 退出被终止。 */
    QStringList resourceRestartPrefix{QStringLiteral("systemd-run")};
};

/** @brief 仅在专属线程访问 DbStore；所有 RPC 由白名单操作分发。 */
class WebService final : public QObject {
    Q_OBJECT

public:

    /**
     * @brief 保存受控配置，启动后才打开本线程数据库。
     * @param config 设备受控配置，包含运行所需路径或功能设置。
     * @param parent Qt 父对象；对象销毁由父子关系管理，空指针表示无父对象。
     */
    explicit WebService(const WebServiceConfig &config, QObject *parent = nullptr);

    /**
     * @brief 退出时释放唯一导入预约，未完成批次在重启后核对。
     */
    ~WebService() override;

    /**
     * @brief 初始化 Web schema、工厂凭据与中断任务恢复。
     */
    void initialize();

    /**
     * @brief 按白名单分派 Web 操作并返回实际业务结果。
     * @param operation 白名单业务操作名。
     * @param args 操作对应的结构化参数。
     * @param owner 已认证 Web 账号主键，用于任务归属校验。
     * @return 版本化操作的 JSON 数据；失败抛出 WebError，由 IPC 统一封装。
     */
    QJsonObject dispatch(const QString &operation, const QJsonObject &args, int owner);

public slots:

    /**
     * @brief 分派单张 Web 人脸或当前导入批次的异步校验结果。
     * @param token 贯穿请求与异步结果的任务标识。
     * @param accepted 通过质量、防重校验的人脸条目。
     * @param failed 校验失败的人脸条目及原因。
     */
    void applyFaces(const QString &token, const QJsonArray &accepted, const QJsonArray &failed);

signals:

    /**
     * @brief 异步提交质量/防重校验给现有推理 worker。
     * @param token 贯穿请求与异步结果的任务标识。
     * @param faces 待校验的人脸条目，含人员及资源信息。
     */
    void faceValidationRequested(const QString &token, const QJsonArray &faces);

    /**
     * @brief 已写入本地数据，需要刷新现有图库/列表。
     */
    void personnelChanged();

private:

    /**
     * @brief 从持久化任务中提取可公开给网页的状态。
     * @param task 持久化上传任务记录。
     * @return 隐藏内部磁盘路径后的任务状态。
     */
    QJsonObject publicTask(const QVariantMap &task) const;

    /**
     * @brief 从上传任务实际字节生成设备提示；人员 Excel/图片不进入视频提示。
     * @param task 持久化上传任务记录。
     * @param state 实际执行状态。
     */
    void reportVideoUpload(const QVariantMap &task, const QString &state);

    /**
     * @brief 校验文件、目录及配额后登记上传任务。
     * @param args 操作对应的结构化参数。
     * @param owner 已认证 Web 账号主键，用于任务归属校验。
     * @return 新上传任务及 daemon 所需的受控临时路径。
     */
    QJsonObject createUpload(const QJsonObject &args, int owner);

    /**
     * @brief 核验已接收文件并幂等发布到固定目标目录。
     * @param id 目标记录或任务的标识。
     * @param owner 已认证 Web 账号主键，用于任务归属校验。
     * @return 幂等发布结果；执行长度、内容、空间及同步检查。
     */
    QJsonObject commit(const QString &id, int owner);

    /**
     * @brief 在 Qt 专用线程解压替换资源，不阻塞界面或 Web RPC。
     * @param task 持久化上传任务记录。
     * @param recovery 是否处理上次中断的资源更新。
     * @return 异步资源更新任务的受理结果。
     */
    QJsonObject startResourcePackage(const QVariantMap &task, bool recovery = false);

    /**
     * @brief 按目录替换结果更新任务，保存可查询的成功目录或失败原因。
     * @param id 目标记录或任务的标识。
     * @param result 处理结果。
     */
    void finishResourcePackage(const QString &id, const QJsonObject &result);

    /**
     * @brief 扫描实际 video 目录并构造视频及图片清单。
     * @return 基于目录实际文件构建的完整视频清单。
     */
    QJsonObject files();
    /**
     * @brief 图片与视频共用媒体配额；小图目录与MQTT的pic目录对应。
     * @return 与 MQTT 相同的 pic 保存目录。
     */
    QString pictureDirectory() const;
    /**
     * @brief 使用任务创建时冻结的目标目录，避免后续切换小图设置改变上传位置。
     * @param task 持久化上传任务记录。
     * @return 任务固定的 pic 或 video 目标根目录。
     */
    QString taskRoot(const QVariantMap &task) const;

    /**
     * @brief 根据当前真实空间、原配额及 Web 预留检查资源。
     * @param kind 资源或任务类型，用于选择目录、配额和校验规则。
     * @param incoming 本次新增资源大小，单位为字节。
     */
    void checkBudget(const QString &kind, qint64 incoming = 0);

    /**
     * @brief 后台执行封存批次；图片包先在独立线程解压，成功后使用原人员及图片入口。
     * @param id 目标记录或任务的标识。
     * @param extracted 已验证的展开图片清单；初次调用为空，解压回调传入非空结果。
     */
    void importBatch(const QString &id, const QJsonArray &extracted = {});

    /**
     * @brief 保存最终结果并释放本地导入互斥预约。
     * @param state 实际执行状态。
     */
    void finishImport(const QString &state);

    /**
     * @brief 只清理受控 UUID 批次的中间文件；保留数据库统计，不删除正式视频/注册照。
     * @param id 目标记录或任务的标识。
     */
    void cleanupBatch(const QString &id);

    /**
     * @brief 恢复已落盘未登记任务；不完整接收标失败并清理临时文件。
     */
    void recover();

    WebServiceConfig config_;                     /**< 不可由请求修改的设备配置。 */
    WebResourceUpdate *resourceWorker_ = nullptr; /**< 当前归档更新线程。 */
    QThread *faceArchiveWorker_ = nullptr; /**< 本批次文件解压线程，不访问数据库；析构先等待停止。 */
    WebHotspot *hotspot_ = nullptr; /**< 本线程管理的热点配置及就绪标记对象。 */
    WebLocalApi *local_ = nullptr; /**< 本线程持有的标准本地业务分发器。 */
    QString currentBatch_;     /**< 单次 Web 本地导入的批次号。 */
    QJsonObject importResult_; /**< 逐阶段保留的实际结果。 */
    bool importLease_ = false; /**< 当前持有本地导入互斥权。 */
};
