/**
 * @file ftppage.h
 * @brief 提供 FTP 配置、空间预检、文件下载和流地址分类页面。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#pragma once

#include <memory>
#include <utility>

#include <QWidget>
#include <QLineEdit>
#include <QTcpSocket>
#include <QProcess>
#include <QScrollArea>
#include <QScrollBar>
#include <QStyle>
#include <QApplication>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QPushButton>
#include <QPlainTextEdit>
#include <QCheckBox>
#include <QLabel>
#include <QTimer>
#include <QFrame>
#include <QIntValidator>
#include <QFileDialog>
#include <QDateTime>
#include <QEvent>
#include <QMessageBox>
#include <QFileInfo>
#include <QDir>
#include <QMenu>
#include <QUrl>
#include <cstdio>
#include <QStyleOption>
#include <QSettings>
#include <QStorageInfo>

#include "appstyle.h"
#include "components/cursoroverlay/keyboard_dialog.h"
#include "components/multimedia/gst_player_widget.h"
#include "platform/rk3566_platform.h"



/**
 * @brief 流播放器抽象接口。
 *
 * FtpPage 通过该接口解耦具体播放器实现，便于替换为不同解码/渲染后端。
 */
class IStreamPlayer {
public:
    /** @brief 允许通过接口指针安全销毁播放器实现。 */
    virtual ~IStreamPlayer() = default;
    /// @brief 播放指定媒体 URL。
    virtual bool play(const QString& url) = 0;
    /// @brief 停止当前播放。
    virtual void stop() = 0;
};



/**
 * @brief FTP 与流地址设置页面。
 *
 * 负责：
 * - FTP 参数输入、连通性测试与文件下载；
 * - 从 MQTT 请求流地址并触发播放；
 * - 页面日志、状态提示、输入历史与配置加载。
 */
class FtpPage : public QWidget
{
    Q_OBJECT
public:
    /** @brief 创建 FTP、下载和流地址设置控件及异步进程对象。 */
    explicit FtpPage(QWidget *parent = nullptr);
    /** @brief 停止下载/探测进程和工作线程后释放页面资源。 */
    ~FtpPage() override;

    /// @brief 获取视频显示窗口句柄（供外部播放器绑定）。
    WId videoWinId() const;
    /// @brief 设置页面状态栏文本。
    void setStatusText(const QString &text);
    /// @brief 追加一行日志到页面与终端输出。
    void appendLog(const QString &line);
    /// @brief 设置流地址输入框内容。
    void setStreamUrl(const QString &url);

    /// @brief 注入播放器实现；为空时使用页面默认处理逻辑。
    void setStreamPlayer(std::shared_ptr<IStreamPlayer> p) { player_ = std::move(p); }

    /** @brief 根据已加载配置发起一次去重的自动 FTP 下载。 */
    void startAutoDownloadFromConfig();
    /** @brief 预加载自动 FTP 参数，供 MQTT 下载命令直接使用。 */
    void prepareAutoFtp();

public slots:
    /** @brief 外部播放器启动成功时显示提示。 */
    void onStreamStartedOk() { showToast(QStringLiteral("拉流成功"), 2500); }
    /** @brief 外部播放器启动失败时显示原因。 */
    void onStreamStartFailed(const QString& reason) { showToast(QStringLiteral("拉流失败：%1").arg(reason), 3000); }

signals:
    /// @brief 请求 MQTT 页面返回最新流地址。
    void requestStreamUrlFromMqtt();

    /// @brief 请求外部打开播放器页面。
    /// @param url 媒体地址
    /// @param kind 媒体类型（如 rtsp/hls/http-file）
    void requestOpenPlayerPage(const QString& url, const QString& kind);

    /** @brief 本地文件下载完成，可交给播放器插入播放列表。 */
    void localFileDownloaded(const QString& filePath);

    /** @brief FTP 下载任务已开始。 */
    void ftpDownloadStarted(const QString& fileName);
    /** @brief FTP 下载完成，参数为最终本地路径。 */
    void ftpDownloadFinished(const QString& filePath);
    /** @brief FTP 下载失败并给出原因。 */
    void ftpDownloadFailed(const QString& fileName, const QString& reason);
    /** @brief FTP 下载进度更新，percent 为 0～100。 */
    void ftpDownloadProgress(const QString& fileName, int percent);
    /** @brief 目标文件已经存在，未重复下载。 */
    void ftpDownloadAlreadyExists(const QString& filePath, const QString& reason);
    /** @brief 可用磁盘空间不足，下载被拒绝。 */
    void ftpDownloadStorageRejected(const QString& fileName, const QString& reason);
    /** @brief 参数或任务状态不允许本次下载。 */
    void ftpDownloadRequestRejected(const QString& fileName, const QString& reason);


private slots:
    /** @brief 使用短 TCP 连接测试 FTP 主机和端口。 */
    void onTestFtpClicked();
    /** @brief 校验并保存 FTP 参数为页面“已连接”状态。 */
    void onConnectFtpClicked();
    /** @brief 清除页面 FTP 配置状态并中止相关任务。 */
    void onDisconnectFtpClicked();
    /** @brief 根据远端路径类型发起单文件或目录下载。 */
    void onDownloadClicked();

    /** @brief 分类地址并发出播放或文件下载请求。 */
    void onStartStreamClicked();
    /** @brief 停止注入的流播放器。 */
    void onStopStreamClicked();

    /// @brief Worker 日志回调。
    void onWorkerLog(const QString &line);
    /// @brief Worker 状态回调。
    void onWorkerStatus(const QString &text);

protected:
    /** @brief 点击输入框时打开文本、密码或端口屏幕键盘。 */
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    /** @brief 以代码方式创建页面控件和布局。 */
    void buildUi();
    /** @brief 应用适配触摸屏的页面样式。 */
    void applyStyles();

    /// @name 软键盘辅助
    /// @{
    /** @brief 给所有可编辑输入框安装软键盘事件过滤。 */
    void installKeyboardFilters();
    /** @brief 使用指定回显模式和长度编辑普通文本。 */
    void openKeyboardFor(QLineEdit *edit, const QString &title,
                         QLineEdit::EchoMode echo, int maxLen);
    /** @brief 使用整数键盘编辑并限制端口范围。 */
    void openKeyboardForPort(QLineEdit *edit, const QString &title,
                             int minV, int maxV);
    /// @}

    /** @brief 校验主机、端口和账号并返回归一化值。 */
    bool validateBasicInputs(QString &host, int &port,
                             QString &user, QString &pass);


    /** @brief 异步建立 TCP 测试连接并设置超时。 */
    void startTcpTest(const QString &host, int port);
    /** @brief 同步测试 TCP 端点，供后台预检流程使用。 */
    /** @brief 通过 curl 子进程下载一个 FTP 文件到临时路径。 */
    void startFtpDownloadByProcess(const QString &host, int port,
                                   const QString &user, const QString &pass,
                                   const QString &remotePath, const QString &localPath);

    /** @brief 通过 curl --list-only 获取远端目录文件队列。 */
    void startFtpListDirByProcess(const QString &host, int port,
                                 const QString &user, const QString &pass,
                                 const QString &remoteDir);

    /** @brief 顺序启动目录队列中的下一文件，避免并发写盘。 */
    void startNextDirDownload();
    /** @return 路径以目录语义结尾时返回 true。 */
    bool isRemoteDir(const QString &remotePath) const;

    /// @name HTTP 文件下载
    /// @{
    /** @brief 通过 curl 子进程下载 HTTP 媒体文件。 */
    void startHttpMp4DownloadByProcess(const QString& url, const QString& localFile);
    /** @brief 根据 URL 文件名和视频目录选择安全的本地保存路径。 */
    QString pickHttpSaveFilePath(const QString& url);
    /// @}


    /// @name 流地址历史
    /// @{
    /** @brief 从设置中加载最近使用的流地址。 */
    void loadStreamUrlHistory();
    /** @brief 持久化最多八条流地址历史。 */
    void saveStreamUrlHistory();
    /** @brief 去重后把地址移到历史首位。 */
    void addStreamUrlHistory(const QString& url);
    /** @brief 在指定全局位置显示流地址历史菜单。 */
    void showStreamUrlHistoryPopup(const QPoint& globalPos);
    /// @}

    /// @brief 从配置目录读取 FTP/MQTT 相关输入参数。
    bool loadInputsFromConfigFile(const QString& cfgDir, bool showTip = true);

    /** @brief 显示页面顶部的非阻塞限时提示。 */
    void showToast(const QString& text, int ms = 3000);

    /** @brief 构造带认证和编码路径的 FTP URL。 */
    QString buildFtpUrl(const QString& host, int port,
                        const QString& remotePath,
                        bool* usedEncodedMode = nullptr) const;

    /** @brief 将本地输入归一化为目录，并兼容误填文件路径。 */
    QString normalizeLocalDirPath(const QString& rawLocalPath,
                                  const QString& remotePath) const;
    /** @brief 统一远端路径分隔符和开头斜杠。 */
    QString normalizeRemoteFilePath(QString remotePath);

    /** @brief 结合本地目录和远端文件名得到最终目标路径。 */
    QString resolveFtpLocalFilePath(const QString& localBaseDir,
                                    const QString& remotePath) const;

    /** @brief 查询 FTP 远端文件大小，失败返回负值并填写原因。 */
    qint64 queryFtpRemoteFileSize(const QString &host, int port,
                                  const QString &user, const QString &pass,
                                  const QString &remotePath,
                                  QString *errorMessage = nullptr) const;

    /** @brief 查询目标路径所在挂载点可用空间，并可返回总容量。 */
    qint64 queryAvailableBytesForPath(const QString &localPath,
                                      qint64 *totalBytes,
                                      QString *errorMessage = nullptr) const;

    /** @brief 比较远端大小和本地余量，并统一记录和发送拒绝原因。 */
    bool checkDownloadSpaceOrReport(const QString &host, int port,
                                    const QString &user, const QString &pass,
                                    const QString &remotePath,
                                    const QString &localPath,
                                    const QString &displayName);

    /** @brief 启动自动下载前的异步远端信息和空间预检。 */
    void startAutoDownloadPreflight(const QString &host, int port,
                                    const QString &user, const QString &pass,
                                    const QString &remotePath,
                                    const QString &localPath,
                                    const QString &displayName,
                                    const QString &taskKey);
    /** @brief 解析预检进程结果并继续下载或报告失败。 */
    void handleAutoDownloadPreflightFinished(int exitCode, QProcess::ExitStatus exitStatus);
    /** @brief 终止自动下载预检并统一发送失败信号。 */
    void failAutoDownloadPreflight(const QString &reason);
    /** @brief 清除自动下载任务键和预检参数。 */
    void clearAutoDownloadState();

    /** @brief 从 curl 增量输出中提取下载百分比并节流上报。 */
    void handleCurlProgressChunk(const QString &chunk, bool isHttpDownload);



private:
    /** @brief FTP curl 子进程当前承担的任务类型。 */
    enum class CurlMode { Idle, Listing, DownloadingOne, DownloadingDir };
    CurlMode curlMode = CurlMode::Idle;

    QString dirHost, dirUser, dirPass, dirRemoteDir, dirLocalDir;
    int dirPort = 21;

    QStringList dirFilesQueue;
    QString listBuffer;   /**< 累计 curl --list-only 输出，完成后统一拆分。 */


    /// URL 历史（最多 8 条）。
    QStringList streamUrlHistory_;
    QMenu* streamUrlMenu_ = nullptr;


    // UI
    QLineEdit *hostEdit = nullptr;
    QLineEdit *portEdit = nullptr;
    QLineEdit *userEdit = nullptr;
    QLineEdit *passEdit = nullptr;
    QLineEdit *remotePathEdit = nullptr;
    QLineEdit *localPathEdit = nullptr;
    QLineEdit *streamUrlEdit = nullptr;

    QPushButton *btnTestFtp = nullptr;
    QPushButton *btnConnectFtp = nullptr;
    QPushButton *btnDisconnectFtp = nullptr;
    QPushButton *btnDownload = nullptr;
    QPushButton *btnLoadCfg = nullptr;
    QPushButton *btnBrowseLocal = nullptr;
    QPushButton *btnFetchUrl = nullptr;
    QPushButton *btnStartStream = nullptr;
    QPushButton *btnStopStream = nullptr;

    QGroupBox   *ftpBox     = nullptr;
    QGroupBox   *logBox     = nullptr;
    QGroupBox   *streamBox     = nullptr;

    QFormLayout *ftpForm    = nullptr;
    QFormLayout *streamForm    = nullptr;

    QVBoxLayout *root  = nullptr;
    QVBoxLayout *layout  = nullptr;
    QVBoxLayout *ftpLayout  = nullptr;
    QVBoxLayout *logLayout  = nullptr;
    QVBoxLayout *streamLayout  = nullptr;

    QHBoxLayout *topRow     = nullptr;
    QHBoxLayout *localRow   = nullptr;
    QHBoxLayout *ftpBtnRow  = nullptr;
    QHBoxLayout *streamBtnRow  = nullptr;
    QHBoxLayout *header  = nullptr;

    QWidget *content = nullptr;
    QWidget* logPlaceholder_ = nullptr;   /**< 日志隐藏时维持布局的占位块。 */

    QLabel *title = nullptr;
    QLabel *statusLabel = nullptr;

    QPlainTextEdit *logEdit = nullptr;

    /** 控制日志区域是否可见。 */
    QCheckBox *showLogCheckBox = nullptr;

    /// “连接”状态标记：用于 UI 语义，不代表 FTP 长连接会话。
    bool ftpConfigured = false;
    QString cfgHost;
    int cfgPort = 21;
    QString cfgUser;
    QString cfgPass;


    /// 执行 curl/ftp 相关子进程。
    QProcess *ftpProc = nullptr;
    QProcess *ftpPreflightProc_ = nullptr;
    QTcpSocket *tcpSock = nullptr;
    QTimer *tcpTimer = nullptr;

    /// 当前文件下载路径及目录下载过程中最后成功文件。
    QString currentDownloadLocalFile_;
    QString currentDownloadPartFile_;
    QString lastDirDownloadedFile_;
    QString ftpProgressBuffer_;
    int ftpLastProgress_ = -1;
    bool ftpProcessStartFailureReported_ = false;

    /// HTTP 文件下载进程状态。
    QProcess *httpProc = nullptr;
    QString currentHttpDownloadLocalFile_;
    bool httpDownloading_ = false;
    QString httpProgressBuffer_;
    int httpLastProgress_ = -1;


    /** @brief 输入地址分类结果，用于区分下载和直播路径。 */
    enum class UrlKind {
        Unknown,    /**< 无法识别。 */
        HttpFile,   /**< HTTP(S) 普通媒体文件。 */
        HttpTsLive, /**< HTTP TS 直播。 */
        HlsLive,    /**< HTTP(S) HLS/m3u8 直播。 */
        RtspLive,   /**< RTSP 直播。 */
        RtmpLive,   /**< RTMP/RTMPS 直播。 */
        SrtLive,    /**< SRT 直播。 */
        UdpLive,    /**< UDP 直播。 */
        LocalFile   /**< file URL 或本地路径。 */
    };

    /** @brief 归一化后的地址及分类依据。 */
    struct UrlInfo {
        UrlKind kind = UrlKind::Unknown; /**< 地址类型。 */
        bool isLive = false;             /**< 是否应进入直播路径。 */
        QString normalizedUrl;           /**< 可直接交给下载器或播放器的地址。 */
        QString detail;                  /**< 识别原因或扩展名，供日志使用。 */
    };

    /** @brief 识别输入地址类型并生成归一化地址。 */
    UrlInfo classifyUrl(const QString& input) const;
    /** @brief 下载 HTTP 文件或播放已存在的本地副本。 */
    void handleHttpFileUrl(const UrlInfo& info);
    /** @brief 把直播地址和类型交给外部播放器页面。 */
    void handleLiveUrl(const UrlInfo& info);

    std::shared_ptr<IStreamPlayer> player_;


    /// 配置文件路径参数。
    QString configDir_ = Rk3566Platform::netConfigDir(); /**< 网络配置目录。 */
    QString configFileName_ = "net_cfg.ini";             /**< 网络配置文件名。 */

    /// 页面顶部提示条控制对象。
    QLabel* toastLabel = nullptr;
    QTimer* toastTimer = nullptr;

    bool autoDownloadInProgress_ = false;
    QString activeAutoDownloadKey_;
    QString lastFinishedAutoDownloadKey_;

    QString preflightHost_;
    int preflightPort_ = 21;
    QString preflightUser_;
    QString preflightPass_;
    QString preflightRemotePath_;
    QString preflightLocalPath_;
    QString preflightDisplayName_;


};
