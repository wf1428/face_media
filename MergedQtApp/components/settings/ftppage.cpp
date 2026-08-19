/**
 * @file ftppage.cpp
 * @brief 提供 FTP 配置、空间预检、文件下载和流地址分类页面。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#include "ftppage.h"

#include <QRegularExpression>
#include "platform/rk3566_platform.h"


// url历史记忆保存
static const char* kStreamUrlHistoryKey = "stream/url_history";
static const int kMaxUrlHistory = 8;


/** @return FTP 路径含非 ASCII 或保留字符、需要逐段编码时返回 true。 */
static bool needEncodeFtpPath(const QString& path)
{
    // 空格、#、% 以及非 ASCII 字符
    for (const QChar& ch : path) {
        ushort u = ch.unicode();
        if (u > 127) return true; // 中文等非 ASCII

        switch (ch.toLatin1()) {
        case ' ':
        case '#':
        case '%':
        case '?':
        case '[':
        case ']':
        case '{':
        case '}':
        case '^':
        case '|':
        case '\\':
        case '"':
        case '<':
        case '>':
        case '`':
            return true;
        default:
            break;
        }
    }
    return false;
}

/** @brief 对 FTP 路径逐段百分号编码，同时保留目录分隔斜杠。 */
static QString encodeFtpPathPreserveSlash(const QString& rawPath)
{
    QString path = rawPath.trimmed();
    if (!path.startsWith('/')) path = "/" + path;

    QStringList parts = path.split('/', QString::KeepEmptyParts);
    for (QString& part : parts) {
        if (part.isEmpty()) continue; // 保留路径分隔
        part = QString::fromLatin1(QUrl::toPercentEncoding(part));
    }
    return parts.join('/');
}

/** @brief 构造带认证和编码路径的 FTP URL。 */
QString FtpPage::buildFtpUrl(const QString& host, int port,
                             const QString& remotePath,
                             bool* usedEncodedMode) const
{
    QString path = remotePath.trimmed();
    if (!path.startsWith('/')) path = "/" + path;

    const bool needEncode = needEncodeFtpPath(path);

    if (usedEncodedMode) {
        *usedEncodedMode = needEncode;
    }

    const QString finalPath = needEncode ? encodeFtpPathPreserveSlash(path) : path;
    return QString("ftp://%1:%2%3").arg(host).arg(port).arg(finalPath);
}


// ------------------------------
// FtpPage UI + 业务
// ------------------------------
/**
 * @brief 构造页面并初始化 UI、进程与网络测试对象。
 */
FtpPage::FtpPage(QWidget *parent) : QWidget(parent)
{
    buildUi();
    applyStyles();
    installKeyboardFilters();
    loadStreamUrlHistory();
    //loadInputsFromConfigFile(configDir_);

    toastTimer = new QTimer(this);
    toastTimer->setSingleShot(true);
    connect(toastTimer, &QTimer::timeout, this, [this]{
        if (toastLabel) toastLabel->hide();
    });

    // ---------- TCP 测试（异步） ----------
    tcpSock = new QTcpSocket(this);
    tcpTimer = new QTimer(this);
    tcpTimer->setSingleShot(true);

    connect(tcpTimer, &QTimer::timeout, this, [this]{
        if (tcpSock->state() != QAbstractSocket::UnconnectedState) {
            tcpSock->abort(); // 直接终止连接尝试
        }
        setStatusText(QStringLiteral("TCP不可达(超时)"));
        appendLog(QStringLiteral("TCP测试超时"));
    });

    connect(tcpSock, &QTcpSocket::connected, this, [this]{
        tcpTimer->stop();
        setStatusText(QStringLiteral("TCP可达(端口打开)"));
        appendLog(QStringLiteral("TCP连接成功"));
        tcpSock->disconnectFromHost();
    });

    connect(tcpSock,
            QOverload<QAbstractSocket::SocketError>::of(&QTcpSocket::error),
            this, [this](QAbstractSocket::SocketError){
        tcpTimer->stop();
        setStatusText(QStringLiteral("TCP不可达"));
        appendLog(QStringLiteral("TCP连接失败：%1").arg(tcpSock->errorString()));
    });


    // ---------- ftpget 进程 ----------
    ftpProc = new QProcess(this);

    connect(ftpProc, &QProcess::readyReadStandardOutput, this, [this]{
        const QString out = QString::fromLocal8Bit(ftpProc->readAllStandardOutput()).trimmed();

        if (out.isEmpty()) return;

        if (curlMode == CurlMode::Listing) {
            listBuffer += out; // 先收集
        } else {
            const QString t = out.trimmed();
            if (!t.isEmpty()) appendLog(t);
        }
    });
    connect(ftpProc, &QProcess::readyReadStandardError, this, [this]{
        const QString out = QString::fromLocal8Bit(ftpProc->readAllStandardError());
        if (out.isEmpty()) return;
        handleCurlProgressChunk(out, false);
    });

    connect(ftpProc, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart || ftpProcessStartFailureReported_) {
            return;
        }

        ftpProcessStartFailureReported_ = true;
        const QString reason = QStringLiteral("Failed to start FTP download process: %1")
                .arg(ftpProc ? ftpProc->errorString() : QStringLiteral("ftp process is null"));
        appendLog(reason);
        setStatusText(QStringLiteral("FTP下载进程启动失败"));
        curlMode = CurlMode::Idle;
        const QString fileName = QFileInfo(currentDownloadLocalFile_).fileName();
        clearAutoDownloadState();
        emit ftpDownloadFailed(fileName, reason);
    });

    connect(ftpProc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this](int code, QProcess::ExitStatus st){

        if (ftpProcessStartFailureReported_) {
            ftpProcessStartFailureReported_ = false;
            ftpProgressBuffer_.clear();
            return;
        }

        const bool ok = (st == QProcess::NormalExit && code == 0);
        ftpProgressBuffer_.clear();
        if (curlMode == CurlMode::Listing) {
            if (!ok) {
                setStatusText(QStringLiteral("列目录失败"));
                appendLog(QStringLiteral("列目录失败（exit=%1）").arg(code));
                curlMode = CurlMode::Idle;
                return;
            }

            // 解析 listBuffer：每行一个文件名
            QStringList lines = listBuffer.split('\n', QString::SkipEmptyParts);
            QStringList files;
            for (QString s : lines) {
                s = s.trimmed();
                if (s.isEmpty()) continue;
                // --list-only 一般就是文件名；这里简单过滤一下
                if (s == "." || s == "..") continue;
                files << s;
            }

            if (files.isEmpty()) {
                setStatusText(QStringLiteral("目录为空/无文件"));
                appendLog(QStringLiteral("目录为空或无法解析文件列表"));
                curlMode = CurlMode::Idle;
                return;
            }

            dirFilesQueue = files;
            appendLog(QStringLiteral("目录文件数：%1").arg(dirFilesQueue.size()));

            // 开始下载第一个
            startNextDirDownload();
            return;
        }

        // 下载单文件/目录中的某个文件
        if (ok) {
            appendLog(QStringLiteral("FTP本地下载成功"));

            const QString finalFile = currentDownloadLocalFile_;
            const QString partFile  = currentDownloadPartFile_;

            // ===== 先把 .part 原子切换成正式文件 =====
            if (finalFile.isEmpty() || partFile.isEmpty() || !QFileInfo::exists(partFile)) {
                setStatusText(QStringLiteral("下载完成,part文件不存在"));
                appendLog(QStringLiteral("FTP下载完成，但part文件不存在：%1").arg(partFile));
                emit ftpDownloadFailed(QFileInfo(finalFile).fileName(),
                                       QStringLiteral("Temporary part file missing"));
                curlMode = CurlMode::Idle;

                clearAutoDownloadState();
                return;
            }

            QFile::remove(finalFile);   // 若已有旧文件，先删
            if (!QFile::rename(partFile, finalFile)) {
                setStatusText(QStringLiteral("下载完成但改名失败"));
                appendLog(QStringLiteral("FTP下载完成，但rename失败：%1 -> %2").arg(partFile, finalFile));
                emit ftpDownloadFailed(QFileInfo(finalFile).fileName(),
                                       QStringLiteral("Local renaming failed"));
                curlMode = CurlMode::Idle;

                clearAutoDownloadState();
                return;
            }



            if (curlMode == CurlMode::DownloadingDir) {
                // 目录下载：当前文件已安全落盘，再继续下一个
                startNextDirDownload();
            } else {
                setStatusText(QStringLiteral("FTP本地下载完成"));
                curlMode = CurlMode::Idle;

                // ===== 自动下载状态收尾 =====
                lastFinishedAutoDownloadKey_ = activeAutoDownloadKey_;
                clearAutoDownloadState();

                // ✅ 单文件下载完成
                if (QFileInfo::exists(finalFile)) {
                    showToast(QStringLiteral("下载完成：%1")
                              .arg(QFileInfo(finalFile).fileName()),
                              3000);

                    ftpLastProgress_ = 100;
                    emit ftpDownloadProgress(QFileInfo(finalFile).fileName(), 100);
                    emit localFileDownloaded(finalFile);
                    emit ftpDownloadFinished(finalFile);
                } else {
                    showToast(QStringLiteral("下载完成"), 3000);
                }
            }

            return;
        }

        // ---------- 失败/崩溃 ----------
        setStatusText(QStringLiteral("下载失败"));
        appendLog(QStringLiteral("FTP本地下载失败（exit=%1, %2）")
                  .arg(code)
                  .arg(st == QProcess::NormalExit ? "NormalExit" : "CrashExit"));

        emit ftpDownloadFailed(QFileInfo(currentDownloadLocalFile_).fileName(),
                               QStringLiteral("FTP download failed (exit=%1, %2)")
                               .arg(code)
                               .arg(st == QProcess::NormalExit ? "NormalExit" : "CrashExit"));

        ftpLastProgress_ = -1;
        curlMode = CurlMode::Idle;

        // ===== 自动下载状态收尾 =====
        clearAutoDownloadState();
    });

    // 自动下载预检查使用独立异步进程。预检查期间本地轮播继续运行，
    // 只有远端文件可访问且所有校验通过后才启动正式下载。
    ftpPreflightProc_ = new QProcess(this);
    connect(ftpPreflightProc_, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &FtpPage::handleAutoDownloadPreflightFinished);
    connect(ftpPreflightProc_, &QProcess::errorOccurred,
            this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart && autoDownloadInProgress_) {
            failAutoDownloadPreflight(QStringLiteral("Failed to start FTP preflight process: %1")
                                      .arg(ftpPreflightProc_->errorString()));
        }
    });

    // ---------- HTTP mp4 下载进程 ----------
    httpProc = new QProcess(this);

    connect(httpProc, &QProcess::readyReadStandardOutput, this, [this]{
        const QString out = QString::fromLocal8Bit(httpProc->readAllStandardOutput()).trimmed();
        if (!out.isEmpty()) appendLog(QStringLiteral("[HTTP] %1").arg(out));
    });

    connect(httpProc, &QProcess::readyReadStandardError, this, [this]{
        const QString out = QString::fromLocal8Bit(httpProc->readAllStandardError());
        if (out.isEmpty()) return;
        handleCurlProgressChunk(out, true);
    });

    connect(httpProc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this](int code, QProcess::ExitStatus st){

        const bool ok = (st == QProcess::NormalExit && code == 0);

        const QString finalFile = currentHttpDownloadLocalFile_;
        const QString partFile  = finalFile + ".part";

        httpDownloading_ = false;

        if (ok) {
               // 用 rename 原子替换，避免半截文件
               QFile::remove(finalFile); // 若存在旧文件，先删（也可以不删，用 rename 覆盖策略取决于系统）
               if (QFile::rename(partFile, finalFile)) {
                   setStatusText(QStringLiteral("HTTP下载完成"));
                   showToast(QStringLiteral("下载完成：%1").arg(QFileInfo(finalFile).fileName()), 2500);
                   appendLog(QStringLiteral("HTTP下载成功：%1").arg(finalFile));

                   httpLastProgress_ = 100;
                   emit ftpDownloadProgress(QFileInfo(finalFile).fileName(), 100);
                   emit ftpDownloadFinished(finalFile);
               } else {
                   setStatusText(QStringLiteral("下载完成但改名失败"));
                   showToast(QStringLiteral("下载完成但保存失败(重命名失败)"), 3000);
                   appendLog(QStringLiteral("HTTP下载完成，但rename失败：%1 -> %2").arg(partFile, finalFile));

                   emit ftpDownloadFailed(QFileInfo(finalFile).fileName(), QStringLiteral("Local renaming failed"));
               }
               return;
           }

        // 失败：保留 partFile 方便排查
        setStatusText(QStringLiteral("HTTP下载失败"));
        showToast(QStringLiteral("HTTP下载失败"), 2500);
        appendLog(QStringLiteral("HTTP下载失败（exit=%1, %2）")
                  .arg(code)
                  .arg(st == QProcess::NormalExit ? "NormalExit" : "CrashExit"));

        emit ftpDownloadFailed(QFileInfo(finalFile).fileName(), QStringLiteral("HTTP download failed（exit=%1）").arg(st));
    });


    // 默认值
    portEdit->setText("21");
    streamUrlEdit->setText("rtsp://192.168.3.11:8554/test");
    remotePathEdit->setPlaceholderText("/remote/path/video.mp4（暂不支持目录）");
    localPathEdit->setPlaceholderText("选择本地保存路径（可选）");
    streamUrlEdit->setPlaceholderText("例如：rtsp://... 或 http(s)://...m3u8");
    setStatusText("就绪");

}

/**
 * @brief 析构时停止后台子进程，释放运行资源。
 */
FtpPage::~FtpPage()
{
    if (ftpProc && ftpProc->state() != QProcess::NotRunning) ftpProc->kill();
    if (ftpPreflightProc_ && ftpPreflightProc_->state() != QProcess::NotRunning) ftpPreflightProc_->kill();
    if (httpProc && httpProc->state() != QProcess::NotRunning) httpProc->kill();

}

/** @brief 将页面诊断日志以 UTF-8 输出到终端。 */
static inline void termPrint(const QString &s)
{
    // 打到终端：stderr 更容易实时看到（不会被 stdout 缓冲卡住）
    fprintf(stderr, "%s\n", s.toLocal8Bit().constData());
    fflush(stderr);
}


/// @brief 更新页面状态文字。
void FtpPage::setStatusText(const QString &text)
{
    if (statusLabel) statusLabel->setText(text);
}

/// @brief 追加时间戳日志到界面和终端。
void FtpPage::appendLog(const QString &line)
{
    const QString ts = QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss");
    const QString msg = QString("[%1] %2").arg(ts, line);

    // UI 日志
    if (logEdit) {
        logEdit->appendPlainText(msg);
    }

    // 终端日志
    termPrint(msg);
}

/// @brief 外部设置流地址输入框文本。
void FtpPage::setStreamUrl(const QString &url)
{
    if (streamUrlEdit) streamUrlEdit->setText(url);
}

/**
 * @brief 构建 FTP/流媒体设置页面布局。
 */
void FtpPage::buildUi()
{
    const double s = AppStyle::uiScaleForScreen();
    auto px = [&](double v) { return int(qRound(v * s)); };

    // 根布局（this）
    root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // ScrollArea（只负责滚动条风格）
    QScrollArea *scroll = AppStyle::createScrollArea(this);
    root->addWidget(scroll);

    // 内容容器
    content = new QWidget(scroll);
    scroll->setWidget(content);

    // 布局,挂在 content 上
    layout = new QVBoxLayout(content);
    layout->setContentsMargins(px(16), px(16), px(16), px(16));
    layout->setSpacing(px(12));

    // 顶部标题 + 状态
    header = new QHBoxLayout();
    title = new QLabel(QStringLiteral("FTP 设置"), this);
    title->setObjectName("pageTitle");

    statusLabel = new QLabel(this);
    statusLabel->setObjectName("statusLabel");
    statusLabel->setMinimumWidth(220);
    statusLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

    header->addWidget(title);
    header->addStretch(1);
    header->addWidget(statusLabel);
    root->addLayout(header);

    // 上半部分：左侧 FTP + 右侧 拉流控制
    topRow = new QHBoxLayout();
    topRow->setSpacing(12);

    // FTP Group
    ftpBox = new QGroupBox(QStringLiteral("FTP 连接与文件操作"), this);
    ftpLayout = new QVBoxLayout(ftpBox);

    ftpForm = new QFormLayout();
    ftpForm->setLabelAlignment(Qt::AlignRight);

    hostEdit = new QLineEdit(this);
    portEdit = new QLineEdit(this);
    portEdit->setValidator(new QIntValidator(1, 65535, this));

    userEdit = new QLineEdit(this);
    passEdit = new QLineEdit(this);
    passEdit->setEchoMode(QLineEdit::Password);

    remotePathEdit = new QLineEdit(this);
    localPathEdit = new QLineEdit(this);

    ftpForm->addRow(QStringLiteral("Host:"), hostEdit);
    ftpForm->addRow(QStringLiteral("Port:"), portEdit);
    ftpForm->addRow(QStringLiteral("Username:"), userEdit);
    ftpForm->addRow(QStringLiteral("Password:"), passEdit);
    ftpForm->addRow(QStringLiteral("Remote Path:"), remotePathEdit);

    // local path + browse
    localRow = new QHBoxLayout();
    btnBrowseLocal = new QPushButton(QStringLiteral("选择..."), this);
    btnBrowseLocal->setObjectName("secondaryBtn");
    localRow->addWidget(localPathEdit, 1);
    localRow->addWidget(btnBrowseLocal);

    ftpForm->addRow(QStringLiteral("Local Path:"), localRow);

    ftpLayout->addLayout(ftpForm);

    // FTP buttons
    ftpBtnRow = new QHBoxLayout();
    btnTestFtp = new QPushButton(QStringLiteral("测试连接"), this);
    btnConnectFtp = new QPushButton(QStringLiteral("连接"), this);
    btnDisconnectFtp = new QPushButton(QStringLiteral("断开"), this);
    btnDownload = new QPushButton(QStringLiteral("下载"), this);

    btnTestFtp->setObjectName("primaryBtn");
    btnConnectFtp->setObjectName("primaryBtn");
    btnDisconnectFtp->setObjectName("dangerBtn");
    btnDownload->setObjectName("successBtn");

    // 读取文件
    btnLoadCfg = new QPushButton(QStringLiteral("读取文件"), this);

    btnLoadCfg->setObjectName("secondaryBtn");
    btnLoadCfg->setSizePolicy(btnDownload->sizePolicy());
    btnLoadCfg->setMinimumHeight(btnDownload->minimumHeight());
    btnLoadCfg->setObjectName("primaryBtn");

    ftpBtnRow->addWidget(btnLoadCfg);
    ftpBtnRow->addWidget(btnTestFtp);
    ftpBtnRow->addWidget(btnConnectFtp);
    ftpBtnRow->addStretch(1);
    ftpBtnRow->addWidget(btnDownload);
    ftpBtnRow->addWidget(btnDisconnectFtp);

    ftpLayout->addSpacing(6);
    ftpLayout->addLayout(ftpBtnRow);

    // Stream Group
    streamBox = new QGroupBox(QStringLiteral("视频流拉取"), this);
    streamLayout = new QVBoxLayout(streamBox);

    streamForm = new QFormLayout();
    streamForm->setLabelAlignment(Qt::AlignRight);

    streamUrlEdit = new QLineEdit(this);
    streamForm->addRow(QStringLiteral("Stream URL:"), streamUrlEdit);

    streamLayout->addLayout(streamForm);

    streamBtnRow = new QHBoxLayout();
    //btnFetchUrl = new QPushButton(QStringLiteral("从MQTT获取地址"), this);
    btnStartStream = new QPushButton(QStringLiteral("开始拉流"), this);
    btnStopStream = new QPushButton(QStringLiteral("停止"), this);

    //btnFetchUrl->setObjectName("primaryBtn");
    btnStartStream->setObjectName("primaryBtn");
    btnStopStream->setObjectName("dangerBtn");

    //streamBtnRow->addWidget(btnFetchUrl);
    streamBtnRow->addStretch(1);
    streamBtnRow->addWidget(btnStartStream);
    streamBtnRow->addWidget(btnStopStream);

    streamLayout->addSpacing(6);
    streamLayout->addLayout(streamBtnRow);

    topRow->addWidget(ftpBox, 1);
    topRow->addWidget(streamBox, 1);
    root->addLayout(topRow);

    // ---------- Toast 提示条（默认隐藏） ----------
    toastLabel = new QLabel(this);
    toastLabel->setObjectName("toastLabel");
    toastLabel->setAlignment(Qt::AlignCenter);
    toastLabel->setVisible(false);
    toastLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    toastLabel->setMinimumHeight(px(36));

    root->addWidget(toastLabel);

    // 显示日志
    showLogCheckBox = new QCheckBox(QStringLiteral("日志"),this);
    showLogCheckBox->setChecked(true);              // 默认显示，也可以 false
    root->addWidget(showLogCheckBox);
    root->addSpacing(px(6));

    // 占位块：用于日志隐藏时保持布局不塌
    logPlaceholder_ = new QWidget(this);
    logPlaceholder_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    logPlaceholder_->setVisible(false); // 默认不显示
    root->addWidget(logPlaceholder_, 1);

    // 底部：日志
    logBox = new QGroupBox(QStringLiteral("日志"), this);
    logLayout = new QVBoxLayout(logBox);

    logEdit = new QPlainTextEdit(this);
    logEdit->setReadOnly(true);
    logEdit->setMaximumBlockCount(500);

    logLayout->addWidget(logEdit);
    root->addWidget(logBox, 1);

    // 连接 UI 信号
    connect(btnBrowseLocal, &QPushButton::clicked, this, [this]{
        const QString file = QFileDialog::getExistingDirectory(this, QStringLiteral("选择本地保存路径"));
        if (!file.isEmpty()) localPathEdit->setText(file);
    });

    connect(btnTestFtp, &QPushButton::clicked, this, &FtpPage::onTestFtpClicked);
    connect(btnConnectFtp, &QPushButton::clicked, this, &FtpPage::onConnectFtpClicked);
    connect(btnDisconnectFtp, &QPushButton::clicked, this, &FtpPage::onDisconnectFtpClicked);
    connect(btnDownload, &QPushButton::clicked, this, &FtpPage::onDownloadClicked);

    //connect(btnFetchUrl, &QPushButton::clicked, this, &FtpPage::onFetchStreamUrlClicked);
    connect(btnStartStream, &QPushButton::clicked, this, &FtpPage::onStartStreamClicked);
    connect(btnStopStream, &QPushButton::clicked, this, &FtpPage::onStopStreamClicked);

    connect(showLogCheckBox, &QCheckBox::toggled, this, [this](bool checked){
        if (logBox) logBox->setVisible(checked);
        if (logPlaceholder_) logPlaceholder_->setVisible(!checked);
        showToast(checked ? QStringLiteral("日志已开启") : QStringLiteral("日志已隐藏"), 1200);
    });

    connect(btnLoadCfg, &QPushButton::clicked, this, [this]{loadInputsFromConfigFile(configDir_);});


    const bool showLog = showLogCheckBox->isChecked();
    if (logBox) logBox->setVisible(showLog);
    if (logPlaceholder_) logPlaceholder_->setVisible(!showLog);
}


/// @brief 应用页面内局部样式。
void FtpPage::applyStyles()
{
    const double s = AppStyle::uiScaleForScreen();
    auto px = [&](double v) { return int(qRound(v * s)); };

    setStyleSheet(QString(
        "QGroupBox { font-size:%1px; color:#303133; border:1px solid #ebeef5; border-radius:%2px; margin-top:%3px; }"
        "QGroupBox::title { subcontrol-origin: margin; left:%4px; padding:0 %5px; }"
        "#pageTitle { font-size:%6px; font-weight:600; color:#303133; }"
        "#statusLabel { color:#606266; }"
        "#hintLabel { color:#909399; font-size:%7px; }"
        "QLineEdit { height:%8px; border:1px solid #dcdfe6; border-radius:%9px; padding:0 %10px; font-size:%11px; color:#303133; }"
        "QLineEdit:focus { border-color:#409eff; }"
        "QPlainTextEdit { border:1px solid #dcdfe6; border-radius:%12px; padding:%13px; font-size:%14px; color:#303133; }"
        "#videoFrame { background-color:#111; border:1px solid #2c2c2c; border-radius:%15px; }"
        "#videoFrame[playing=\"true\"] { background: transparent; }"
        "QPushButton { height:%16px; border-radius:%17px; padding:0 %18px; font-size:%19px; }"
        "#toastLabel { background-color: rgba(64,158,255,0.15); color:#409eff; "
        "border:1px solid rgba(64,158,255,0.35); border-radius:6px; "
        "padding:6px 10px; font-size:%20px; }"
        "QPushButton#primaryBtn { border:1px solid #409eff; background-color:#409eff; color:white; }"
        "QPushButton#primaryBtn:hover { background-color:#66b1ff; border-color:#66b1ff; }"
        "QPushButton#primaryBtn:pressed { background-color:#337ecc; border-color:#337ecc; }"
        "QPushButton#secondaryBtn { border:1px solid #dcdfe6; background-color:white; color:#606266; }"
        "QPushButton#secondaryBtn:hover { border-color:#409eff; background-color:#ecf5ff; color:#409eff; }"
        "QPushButton#secondaryBtn:pressed { border-color:#337ecc; background-color:#d9ecff; color:#337ecc; }"
        "QPushButton#dangerBtn { border:1px solid #f56c6c; background-color:#f56c6c; color:white; }"
        "QPushButton#dangerBtn:hover { background-color:#f78989; border-color:#f78989; }"
        "QPushButton#dangerBtn:pressed { background-color:#dd6161; border-color:#dd6161; }"
        "QPushButton#successBtn { border:1px solid #67c23a; background-color:#67c23a; color:white; }"
        "QPushButton#successBtn:hover { background-color:#85ce61; border-color:#85ce61; }"
        "QPushButton#successBtn:pressed { background-color:#5daf34; border-color:#5daf34; }"
    )
    .arg(px(14)).arg(px(8)).arg(px(10)).arg(px(12)).arg(px(6))
    .arg(px(18)).arg(px(12))
    .arg(px(34)).arg(px(6)).arg(px(10)).arg(px(14))
    .arg(px(6)).arg(px(8)).arg(px(12))
    .arg(px(6))
    .arg(px(34)).arg(px(6)).arg(px(14)).arg(px(13))
    .arg(px(13))
    );
}

// ------------------------------
// FTP 按钮逻辑（直接调用 worker）
// ------------------------------
/// @brief 校验 FTP 基础参数并返回规范化结果。
bool FtpPage::validateBasicInputs(QString &host, int &port,
                                  QString &user, QString &pass)
{
    host = hostEdit->text().trimmed();
    port = portEdit->text().toInt();
    user = userEdit->text();
    pass = passEdit->text();

    if (host.isEmpty()) {
        setStatusText(QStringLiteral("Host 为空"));
        appendLog(QStringLiteral("操作失败：Host 为空"));
        return false;
    }
    if (port <= 0 || port > 65535) {
        setStatusText(QStringLiteral("Port 非法"));
        appendLog(QStringLiteral("操作失败：Port 非法 (%1)").arg(port));
        return false;
    }
    return true;
}

/** @return 路径以目录语义结尾时返回 true。 */
bool FtpPage::isRemoteDir(const QString &remotePath) const
{
    return remotePath.endsWith('/');
}

/** @brief 通过 curl --list-only 获取远端目录文件队列。 */
void FtpPage::startFtpListDirByProcess(const QString &host, int port,
                                       const QString &user, const QString &pass,
                                       const QString &remoteDir)
{
    // remoteDir 要以 / 结尾
    QString dir = remoteDir.trimmed();
    if (!dir.endsWith('/')) dir += '/';
    if (!dir.startsWith('/')) dir = "/" + dir;

    // 找 curl
    QString curlPath;
    if (QFileInfo("/usr/bin/curl").exists()) curlPath = "/usr/bin/curl";
    else if (QFileInfo("/bin/curl").exists()) curlPath = "/bin/curl";
    else {
        setStatusText(QStringLiteral("缺少 curl"));
        appendLog(QStringLiteral("系统找不到 curl（/usr/bin/curl 或 /bin/curl）"));
        return;
    }

    // 如果进程在跑，先停
    if (ftpProc->state() != QProcess::NotRunning) {
        ftpProc->kill();
    }

    // 组 URL
    bool encodedMode = false;
    const QString url = buildFtpUrl(host, port, dir, &encodedMode);

    // 组参数：列目录
    QStringList args;
    args << "--ftp-ssl"
         << "--ftp-pasv"
         << "--insecure"          // 证书：二选一
         << "-u" << QString("%1:%2").arg(user, pass)
         << "--list-only"
         << "-v"
         << url;

    // 切到 Listing 模式，清空 buffer
    curlMode = CurlMode::Listing;
    listBuffer.clear();

    setStatusText(QStringLiteral("正在列目录..."));
    appendLog(QStringLiteral("列目录：%1 %2").arg(curlPath, args.join(' ')));

    ftpProc->start(curlPath, args);
}

/** @brief 顺序启动目录队列中的下一文件，避免并发写盘。 */
void FtpPage::startNextDirDownload()
{
    if (dirFilesQueue.isEmpty()) {
        setStatusText(QStringLiteral("目录下载完成"));
        appendLog(QStringLiteral("目录下载完成"));
        curlMode = CurlMode::Idle;

        showToast(QStringLiteral("目录下载完成（共%1个文件）").arg(dirFilesQueue.size()), 3000);
        return;
    }

    const QString fileName = dirFilesQueue.takeFirst();
    const QString remoteFile = dirRemoteDir.endsWith('/')
        ? (dirRemoteDir + fileName)
        : (dirRemoteDir + "/" + fileName);

    const QString localFile = QDir(dirLocalDir).filePath(fileName);

    setStatusText(QStringLiteral("正在下载：%1").arg(fileName));
    appendLog(QStringLiteral("目录下载：remote='%1' -> local='%2'").arg(remoteFile, localFile));

    curlMode = CurlMode::DownloadingDir;

    // ✅ 记录目录下载最后一个文件
    lastDirDownloadedFile_ = localFile;

    startFtpDownloadByProcess(dirHost, dirPort, dirUser, dirPass, remoteFile, localFile);
}



/** @brief 通过 curl 子进程下载一个 FTP 文件到临时路径。 */
void FtpPage::startFtpDownloadByProcess(const QString &host, int port,
                                        const QString &user, const QString &pass,
                                        const QString &remotePath, const QString &localPath)
{
    if (!ftpProc) {
        const QString reason = QStringLiteral("FTP download process is not initialized");
        clearAutoDownloadState();
        emit ftpDownloadFailed(QFileInfo(localPath).fileName(), reason);
        return;
    }

    // curl 也可以下载目录但会拿到列表，不是你要的文件；先沿用“不支持目录”
    if (remotePath.endsWith('/')) {
        setStatusText(QStringLiteral("暂不支持目录下载"));
        appendLog(QStringLiteral("Remote Path 是目录：%1（请填写具体文件 /xx/video.mp4）").arg(remotePath));
        clearAutoDownloadState();
        emit ftpDownloadFailed(QFileInfo(remotePath).fileName(), QStringLiteral("Currently, the download of table of contents is not supported."));
        return;
    }

    // 确保本地目录存在
    QFileInfo fi(localPath);
    QDir dir = fi.dir();
    if (!dir.exists() && !dir.mkpath(".")) {
        const QString reason = QStringLiteral("Unable to create local download directory: %1")
                .arg(dir.absolutePath());
        appendLog(reason);
        clearAutoDownloadState();
        emit ftpDownloadFailed(QFileInfo(localPath).fileName(), reason);
        return;
    }

    // 如果已有下载在跑，先停掉
    if (ftpProc->state() != QProcess::NotRunning) {
        appendLog(QStringLiteral("已有下载任务在执行，终止旧任务"));
        ftpProc->kill();
    }

    // 选择 curl 路径
    QString curlPath;
    if (QFileInfo("/usr/bin/curl").exists()) curlPath = "/usr/bin/curl";
    else if (QFileInfo("/bin/curl").exists()) curlPath = "/bin/curl";
    else {
        setStatusText(QStringLiteral("缺少 curl"));
        appendLog(QStringLiteral("系统找不到 curl"));
        appendLog(QStringLiteral("返回 530 TLS required，ftpget 不支持 TLS"));
        clearAutoDownloadState();
        emit ftpDownloadFailed(QFileInfo(localPath).fileName(), QStringLiteral("The system is lacking curl."));
        return;
    }

    // 组 URL：显式 FTPS 通常还是 ftp://host:port/path，然后 curl 加 --ftp-ssl
    bool encodedMode = false;
    const QString url = buildFtpUrl(host, port, remotePath, &encodedMode);

    // ===== FTP 先下载到 .part =====
    const QString partPath = localPath + ".part";

    // 清理上次残留的 part 文件
    if (QFileInfo::exists(partPath)) {
        QFile::remove(partPath);
    }

    // curl 参数：
    // --ftp-ssl: 显式 FTPS（AUTH TLS）
    // --ftp-pasv: 被动模式（常见更稳）
    // -u user:pass
    // -o localPath
    // --connect-timeout: 连接超时
    // --max-time: 总超时（大文件可以不设或设大点）
    // -v: 输出详细日志，便于定位 530/证书等问题
    QStringList args;
    args << "--ftp-ssl"
         << "--ftp-pasv"
         << "-u" << QString("%1:%2").arg(user, pass)
         << "-o" << partPath
         << "--connect-timeout" << "8"
         << "--speed-limit" << "1"
         << "--speed-time" << "60"
         << "--max-time" << "0"
         << "-v"
         << "--insecure"    // 临时跳过证书校验
         << url;

    // 记录正式文件和临时文件下载的本地文件路径，finished(ok) 时用它触发播放
    currentDownloadLocalFile_ = localPath;
    currentDownloadPartFile_ = partPath;

    ftpProgressBuffer_.clear();
    ftpLastProgress_ = -1;
    ftpProcessStartFailureReported_ = false;

    setStatusText(QStringLiteral("正在下载..."));
    appendLog(QStringLiteral("执行：%1 %2").arg(curlPath, args.join(' ')));
    appendLog(QStringLiteral("FTP下载采用临时文件：%1 -> %2").arg(partPath, localPath));

    emit ftpDownloadStarted(QFileInfo(localPath).fileName());
    emit ftpDownloadProgress(QFileInfo(localPath).fileName(), 0);
    ftpProc->start(curlPath, args);
}

/** @brief 使用短 TCP 连接测试 FTP 主机和端口。 */
void FtpPage::onTestFtpClicked()
{
    QString host, user, pass;
    int port = 0;
    if (!validateBasicInputs(host, port, user, pass)) return;

    startTcpTest(host, port);
}

/** @brief 校验并保存 FTP 参数为页面“已连接”状态。 */
void FtpPage::onConnectFtpClicked()
{
    QString host, user, pass;
    int port = 0;
    if (!validateBasicInputs(host, port, user, pass)) return;

    cfgHost = host;
    cfgPort = port;
    cfgUser = user;
    cfgPass = pass;
    ftpConfigured = true;

    setStatusText(QStringLiteral("已保存连接信息"));
    appendLog(QStringLiteral("保存 FTP 配置：%1:%2 user=%3").arg(host).arg(port).arg(user));

    startTcpTest(host, port);   // ✅ 异步，不阻塞

}

/** @brief 清除页面 FTP 配置状态并中止相关任务。 */
void FtpPage::onDisconnectFtpClicked()
{
    ftpConfigured = false;
    cfgHost.clear();
    cfgUser.clear();
    cfgPass.clear();
    cfgPort = 21;

    setStatusText(QStringLiteral("已断开（清除配置）"));
    appendLog(QStringLiteral("清除 FTP 配置"));
}

/** @brief 从 curl 增量输出中提取下载百分比并节流上报。 */
void FtpPage::handleCurlProgressChunk(const QString &chunk, bool isHttpDownload)
{
    QString &buffer = isHttpDownload ? httpProgressBuffer_ : ftpProgressBuffer_;
    int &lastProgress = isHttpDownload ? httpLastProgress_ : ftpLastProgress_;

    buffer += chunk;
    buffer.replace('\r', '\n');

    const int lastBreak = buffer.lastIndexOf('\n');
    if (lastBreak < 0) {
        return;
    }

    const QString ready = buffer.left(lastBreak + 1);
    buffer = buffer.mid(lastBreak + 1);

    const QString fileName = QFileInfo(isHttpDownload ? currentHttpDownloadLocalFile_
                                                      : currentDownloadLocalFile_).fileName();

    static const QRegularExpression kProgressRe(
        QStringLiteral("^\\s*(\\d{1,3})\\s+\\S+\\s+\\d{1,3}\\s+\\S+.*$"));

    const QStringList lines = ready.split('\n', QString::SkipEmptyParts);
    for (const QString &rawLine : lines) {
        const QString line = rawLine.trimmed();
        if (line.isEmpty()) {
            continue;
        }

        const auto match = kProgressRe.match(rawLine);
        if (match.hasMatch()) {
            const int percent = qBound(0, match.captured(1).toInt(), 100);
            if (percent != lastProgress) {
                lastProgress = percent;
                setStatusText(QStringLiteral("正在下载...%1%").arg(percent));
                emit ftpDownloadProgress(fileName, percent);
            }
            continue;
        }

        if (isHttpDownload) {
            appendLog(QStringLiteral("[HTTP] %1").arg(line));
        } else {
            appendLog(line);
        }
    }
}


/**
 * @brief 处理“下载”按钮。
 *
 * 根据输入 URL/路径自动分流到 FTP 目录下载、FTP 文件下载、
 * 或 HTTP 文件下载逻辑。
 */
void FtpPage::onDownloadClicked()
{
    QString host, user, pass;
    int port = 0;
    if (!validateBasicInputs(host, port, user, pass)) return;

    QString remotePath = remotePathEdit->text().trimmed();
    QString localDir   = localPathEdit->text().trimmed();

    remotePath = normalizeRemoteFilePath(remotePath);

    if (remotePath.isEmpty()) {
        setStatusText(QStringLiteral("Remote Path 为空"));
        appendLog(QStringLiteral("下载失败：Remote Path 为空"));
        return;
    }

    if (localDir.isEmpty()) {
        const QString dir = QFileDialog::getExistingDirectory(this, QStringLiteral("选择本地保存目录"));
        if (dir.isEmpty()) {
            appendLog(QStringLiteral("取消下载：未选择本地保存目录"));
            return;
        }
        localDir = dir;
    }

    localDir = normalizeLocalDirPath(localDir, remotePath);

    if (localDir.isEmpty()) {
        setStatusText(QStringLiteral("Local Path 非法"));
        appendLog(QStringLiteral("下载失败：Local Path 非法"));
        return;
    }

    if (remotePathEdit) remotePathEdit->setText(remotePath);
    if (localPathEdit)  localPathEdit->setText(localDir);   // 放目录

    if (isRemoteDir(remotePath)) {
        dirHost = host;
        dirPort = port;
        dirUser = user;
        dirPass = pass;
        dirRemoteDir = remotePath;
        dirLocalDir = localDir;   // 是目录

        appendLog(QStringLiteral("提交目录下载：ftp://%1:%2 remote='%3' -> localDir='%4'")
                  .arg(host).arg(port).arg(remotePath, localDir));

        startFtpListDirByProcess(host, port, user, pass, remotePath);
        return;
    }

    const QString finalLocalPath = resolveFtpLocalFilePath(localDir, remotePath);

    appendLog(QStringLiteral("本地保存目录：%1").arg(localDir));
    appendLog(QStringLiteral("本地保存文件：%1").arg(finalLocalPath));
    appendLog(QStringLiteral("提交下载：ftp://%1:%2 remote='%3' -> local='%4'")
              .arg(host).arg(port).arg(remotePath, finalLocalPath));

    // 下载前空间校验
    const QString displayName = QFileInfo(finalLocalPath).fileName();
    if (!checkDownloadSpaceOrReport(host, port, user, pass, remotePath, finalLocalPath, displayName)) {
        return;
    }

    startFtpDownloadByProcess(host, port, user, pass, remotePath, finalLocalPath);
}


/** @return 去除查询和末尾斜杠后的远端文件名。 */
static QString fileNameFromRemotePath(const QString& remotePath)
{
    QString rp = remotePath.trimmed();
    if (rp.isEmpty()) return QString();

    while (rp.endsWith('/')) {
        rp.chop(1);
    }

    return QFileInfo(rp).fileName();
}

/** @brief 将路径分隔符统一并折叠重复斜杠。 */
static QString cleanPathSeparators(QString p)
{
    p = QDir::fromNativeSeparators(p.trimmed());
    while (p.contains("//")) p.replace("//", "/");
    return p;
}

/** @return 最后路径段具有常见文件扩展名形式时返回 true。 */
static bool looksLikeFileSegment(const QString& name)
{
    if (name.isEmpty()) return false;
    if (name == "." || name == "..") return false;

    const int dotPos = name.lastIndexOf('.');
    if (dotPos <= 0 || dotPos == name.size() - 1) {
        return false;
    }

    const QString ext = name.mid(dotPos + 1).toLower();
    static const QSet<QString> kKnownFileExts = {
        "mp4","MP4","avi","AVI","mov","MOV","mkv","MKV","wmv","WMV","flv","FLV","ts","TS","mpg","MPG","m3u8",
        "jpg","jpeg","png","bmp",
        "txt","json","ini","cfg","log","bin","zip"
    };
    return kKnownFileExts.contains(ext);
}

/** @brief 将本地输入归一化为目录，并兼容误填文件路径。 */
QString FtpPage::normalizeLocalDirPath(const QString& rawLocalPath,
                                       const QString& remotePath) const
{
    QString p = cleanPathSeparators(rawLocalPath);
    if (p.isEmpty()) return QString();

    QString remoteName = fileNameFromRemotePath(remotePath);

    // 先去掉末尾多余的 /
    while (p.size() > 1 && p.endsWith('/')) {
        p.chop(1);
    }

    // 如果路径尾巴反复拼上了 remote 文件名，就一路剥掉
    while (!remoteName.isEmpty()) {
        QFileInfo fi(p);
        const QString tail = fi.fileName();
        if (tail.compare(remoteName, Qt::CaseInsensitive) == 0) {
            p = fi.path();
            while (p.size() > 1 && p.endsWith('/')) p.chop(1);
            continue;
        }
        break;
    }

    // 如果最终尾段看起来像文件名，也剥掉
    QFileInfo fi(p);
    if (fi.exists() && fi.isFile()) {
        p = fi.path();
    } else {
        const QString tail = fi.fileName();
        if (looksLikeFileSegment(tail)) {
            p = fi.path();
        }
    }

    return cleanPathSeparators(p);
}


/** @brief 统一远端路径分隔符和开头斜杠。 */
QString FtpPage::normalizeRemoteFilePath(QString remotePath)
{
    remotePath = QDir::fromNativeSeparators(remotePath.trimmed());
    while (remotePath.contains("//")) remotePath.replace("//", "/");

    if (remotePath.isEmpty()) return QString();

    if (!remotePath.startsWith('/')) {
        remotePath.prepend('/');
    }

    // 单文件下载场景：去掉末尾多余的 /
    while (remotePath.size() > 1 && remotePath.endsWith('/')) {
        remotePath.chop(1);
    }

    return remotePath;
}


/** @brief 结合本地目录和远端文件名得到最终目标路径。 */
QString FtpPage::resolveFtpLocalFilePath(const QString& localBaseDir,
                                         const QString& remotePath) const
{
    QString base = normalizeLocalDirPath(localBaseDir, remotePath).trimmed();
    if (base.isEmpty()) return QString();

    QString name = fileNameFromRemotePath(remotePath);
    if (name.isEmpty()) {
        name = "download.bin";
    }

    return QDir(base).filePath(name);
}


// ------------------------------
// 拉流按钮逻辑
// ------------------------------
//void FtpPage::onFetchStreamUrlClicked()
//{
//    appendLog(QStringLiteral("请求通过 MQTT 获取视频流地址"));
//    emit requestStreamUrlFromMqtt();
//}


static inline QString trimQuotes(QString s) {
    s = s.trimmed();
    if ((s.startsWith('\"') && s.endsWith('\"')) ||
        (s.startsWith('\'') && s.endsWith('\''))) {
        s = s.mid(1, s.size() - 2).trimmed();
    }
    return s;
}

/// @brief 识别 URL 类型并返回标准化信息。
FtpPage::UrlInfo FtpPage::classifyUrl(const QString& input) const
{
    UrlInfo info;
    info.normalizedUrl = trimQuotes(input);
    if (info.normalizedUrl.isEmpty()) {
        info.detail = "empty";
        return info;
    }

    const QString u = info.normalizedUrl;
    const QString lower = u.toLower();

    // ---- scheme 判断 ----
    if (lower.startsWith("rtsp://")) {
        info.kind = UrlKind::RtspLive;
        info.isLive = true;
        info.detail = "scheme=rtsp";
        return info;
    }
    if (lower.startsWith("rtmp://") || lower.startsWith("rtmps://")) {
        info.kind = UrlKind::RtmpLive;
        info.isLive = true;
        info.detail = "scheme=rtmp/rtmps";
        return info;
    }
    if (lower.startsWith("srt://")) {
        info.kind = UrlKind::SrtLive;
        info.isLive = true;
        info.detail = "scheme=srt";
        return info;
    }
    if (lower.startsWith("udp://")) {
        info.kind = UrlKind::UdpLive;
        info.isLive = true;
        info.detail = "scheme=udp";
        return info;
    }

    // file:// 或 绝对/相对本地路径
    if (lower.startsWith("file://")) {
        info.kind = UrlKind::LocalFile;
        info.isLive = false;
        info.detail = "scheme=file";
        return info;
    }
    if (QFileInfo::exists(u)) {
        info.kind = UrlKind::LocalFile;
        info.isLive = false;
        info.detail = "local path exists";
        return info;
    }

    // http(s) -> 根据后缀判断：m3u8=直播，mp4等=文件
    const bool isHttp = lower.startsWith("http://") || lower.startsWith("https://");
    if (isHttp) {
        QUrl qu(u);
        const QString pathLower = qu.path().toLower();

        if (pathLower.endsWith(".m3u8")) {
            info.kind = UrlKind::HlsLive;
            info.isLive = true;
            info.detail = "http(s) + .m3u8";
            return info;
        }

        // ✅ 把 .ts 当直播（http-ts live）
        if (pathLower.endsWith(".ts")) {
            info.kind = UrlKind::HttpTsLive;
            info.isLive = true;
            info.detail = "http(s) + .ts (treat as live)";
            return info;
        }

        // 文件后缀（按需要扩展）
        static const QStringList kVideoExts = {
            ".mp4", ".mov", ".mkv", ".avi", ".flv", ".webm"
        };

        for (const auto& ext : kVideoExts) {
            if (pathLower.endsWith(ext)) {
                info.kind = UrlKind::HttpFile;
                info.isLive = false;
                info.detail = QString("http(s) + %1").arg(ext);
                return info;
            }
        }

        info.kind = UrlKind::Unknown;
        info.isLive = false;
        info.detail = "http(s) unknown suffix";
        return info;
    }

    info.kind = UrlKind::Unknown;
    info.detail = "unknown scheme";
    return info;
}

/** @brief 下载 HTTP 文件或播放已存在的本地副本。 */
void FtpPage::handleHttpFileUrl(const UrlInfo& info)
{
    // 选择保存路径 -> curl 下载 -> 完成 toast
    const QString saveFile = pickHttpSaveFilePath(info.normalizedUrl);
    if (saveFile.isEmpty()) {
        appendLog(QStringLiteral("取消HTTP下载：未选择保存路径"));
        showToast(QStringLiteral("已取消（未选择保存路径）"), 2000);
        return;
    }

    appendLog(QStringLiteral("HTTP下载请求：%1 -> %2").arg(info.normalizedUrl, saveFile));
    startHttpMp4DownloadByProcess(info.normalizedUrl, saveFile);
}

/** @brief 把直播地址和类型交给外部播放器页面。 */
void FtpPage::handleLiveUrl(const UrlInfo& info)
{
    // 直播：提示用户是否跳转播放页
    setStatusText(QStringLiteral("识别到直播流"));
    showToast(QStringLiteral("识别到直播流，准备播放"), 1500);
    appendLog(QStringLiteral("直播流：%1 (%2)").arg(info.normalizedUrl, info.detail));

    QString kindStr;
    switch (info.kind) {
        case UrlKind::RtspLive:     kindStr = "rtsp"; break;
        case UrlKind::HlsLive:      kindStr = "hls";  break;
        case UrlKind::HttpTsLive:   kindStr = "http-ts"; break;
        case UrlKind::RtmpLive:     kindStr = "rtmp"; break;
        case UrlKind::SrtLive:      kindStr = "srt";  break;
        case UrlKind::UdpLive:      kindStr = "udp";  break;
        default:                    kindStr = "live"; break;
    }

    const auto ret = QMessageBox::question(
        this,
        QStringLiteral("提示"),
        QStringLiteral("检测到直播流地址：\n%1\n\n是否跳转到播放页？")
            .arg(info.normalizedUrl),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::Yes
    );

    if (ret == QMessageBox::Yes) {
        appendLog(QStringLiteral("用户确认：跳转到播放页 kind=%1").arg(kindStr));
        emit requestOpenPlayerPage(info.normalizedUrl, kindStr);
    } else {
        appendLog(QStringLiteral("用户取消：不跳转播放页"));
        setStatusText(QStringLiteral("已取消"));
    }
}


/// @brief 处理“开始拉流”按钮并启动播放器。
void FtpPage::onStartStreamClicked()
{
    const QString input = streamUrlEdit->text().trimmed();
    if (input.isEmpty()) {
        setStatusText(QStringLiteral("Stream URL 为空"));
        appendLog(QStringLiteral("开始失败：Stream URL 为空"));
        showToast(QStringLiteral("Stream URL 为空"), 2500);
        return;
    }

    const UrlInfo info = classifyUrl(input);
    appendLog(QStringLiteral("URL识别：%1, kind=%2, live=%3, %4")
              .arg(info.normalizedUrl)
              .arg(int(info.kind))
              .arg(info.isLive ? "true" : "false")
              .arg(info.detail));

    // 文件 vs 直播 分流
    if (!info.isLive) {
        if (info.kind == UrlKind::HttpFile) {
            handleHttpFileUrl(info);
            return;
        }

        if (info.kind == UrlKind::LocalFile) {
            appendLog(QStringLiteral("本地文件URL：%1").arg(info.normalizedUrl));
            showToast(QStringLiteral("本地文件URL"), 2500);
            // 也可直接跳转播放器页：
            // emit requestOpenPlayerPage(info.normalizedUrl, "file");
            return;
        }

        appendLog(QStringLiteral("暂不支持的文件URL：%1").arg(info.normalizedUrl));
        showToast(QStringLiteral("暂不支持该类型文件URL"), 3000);
        return;
    }

    // 直播流：rtsp/rtmp/hls/srt/udp...
    handleLiveUrl(info);
}

/// @brief 停止当前播放并更新状态提示。
void FtpPage::onStopStreamClicked()
{
    // 1) 停止HTTP下载（保持原逻辑）
    if (httpProc && httpProc->state() != QProcess::NotRunning) {
        appendLog(QStringLiteral("停止HTTP下载"));
        httpProc->kill();
        httpDownloading_ = false;
        setStatusText(QStringLiteral("已停止HTTP下载"));
        showToast(QStringLiteral("已停止HTTP下载"), 2000);
        return;
    }

    // 2) 停止直播播放（预留播放器接口）
    if (player_) {
        appendLog(QStringLiteral("停止直播播放"));
        player_->stop();
        setStatusText(QStringLiteral("已停止播放"));
        showToast(QStringLiteral("已停止播放"), 1500);
        return;
    }

    showToast(QStringLiteral("当前没有任务"), 2000);
}


// 查 FTP 文件大小
qint64 FtpPage::queryFtpRemoteFileSize(const QString &host, int port, const QString &user, const QString &pass,
                                        const QString &remotePath, QString *errorMessage) const
{
    QString curlPath;
    if (QFileInfo("/usr/bin/curl").exists()) curlPath = "/usr/bin/curl";
    else if (QFileInfo("/bin/curl").exists()) curlPath = "/bin/curl";
    else {
        if (errorMessage) *errorMessage = QStringLiteral("The system is lacking curl.");
        return -1;
    }

    bool encodedMode = false;
    const QString url = buildFtpUrl(host, port, remotePath, &encodedMode);

    QProcess proc;
    QStringList args;
    args << "--ftp-ssl"
         << "--ftp-pasv"
         << "--insecure"
         << "-u" << QString("%1:%2").arg(user, pass)
         << "--head"
         << "--silent"
         << "--show-error"
         << url;

    proc.start(curlPath, args);
    if (!proc.waitForStarted(3000)) {
        if (errorMessage) *errorMessage = QStringLiteral("Failed to start the curl command to query the size of the remote file");
        return -1;
    }

    if (!proc.waitForFinished(20000)) {
        proc.kill();
        if (errorMessage) *errorMessage = QStringLiteral("Timeout for querying the size of remote files");
        return -1;
    }

    const QString stdOut = QString::fromLocal8Bit(proc.readAllStandardOutput());
    const QString stdErr = QString::fromLocal8Bit(proc.readAllStandardError());
    const QString allText = stdOut + "\n" + stdErr;

    if (proc.exitStatus() != QProcess::NormalExit || proc.exitCode() != 0) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Failed to query the size of the remote file（exit=%1） %2")
                    .arg(proc.exitCode())
                    .arg(stdErr.trimmed());
        }
        return -1;
    }

    QRegularExpression re("Content-Length\\s*:\\s*(\\d+)", QRegularExpression::CaseInsensitiveOption);
    QRegularExpressionMatch m = re.match(allText);
    if (!m.hasMatch()) {
        if (errorMessage) *errorMessage = QStringLiteral("Failed to obtain the size of the remote file");
        return -1;
    }

    bool ok = false;
    const qint64 size = m.captured(1).toLongLong(&ok);
    if (!ok || size < 0) {
        if (errorMessage) *errorMessage = QStringLiteral("Failed to parse the size of the remote file");
        return -1;
    }

    return size;
}


// 查本地剩余空间
qint64 FtpPage::queryAvailableBytesForPath(const QString &localPath,
                                            qint64 *totalBytes,
                                            QString *errorMessage) const
{
    if (totalBytes) *totalBytes = 0;

    QFileInfo fi(localPath);
    QString dirPath = fi.isDir() ? fi.absoluteFilePath() : fi.absolutePath();
    if (dirPath.isEmpty()) {
        if (errorMessage) *errorMessage = QStringLiteral("Local path is illegal");
        return -1;
    }

    QStorageInfo storage(dirPath);
    storage.refresh();

    if (!storage.isValid() || !storage.isReady()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Unable to obtain storage space information：%1").arg(dirPath);
        }
        return -1;
    }

    if (totalBytes) *totalBytes = storage.bytesTotal();
    return storage.bytesAvailable();
}


/** @brief 比较远端大小和本地余量，并统一记录和发送拒绝原因。 */
bool FtpPage::checkDownloadSpaceOrReport(const QString &host, int port,
                                         const QString &user, const QString &pass,
                                         const QString &remotePath,
                                         const QString &localPath,
                                         const QString &displayName)
{
    QString err;

    qint64 totalBytes = 0;
    const qint64 availableBytes = queryAvailableBytesForPath(localPath, &totalBytes, &err);
    if (availableBytes < 0 || totalBytes <= 0 || availableBytes > totalBytes) {
        if (err.isEmpty()) {
            err = QStringLiteral("存储空间数据无效");
        }
        setStatusText(QStringLiteral("存储空间检查失败"));
        appendLog(QStringLiteral("存储空间检查失败：%1").arg(err));
        showToast(QStringLiteral("存储空间检查失败"), 2500);
        emit ftpDownloadFailed(displayName, err);
        return false;
    }

    const qint64 usedBytes = totalBytes - availableBytes;
    const int usedPercent = static_cast<int>((usedBytes * 100) / totalBytes);
    const qint64 maxAllowedUsedBytes = (totalBytes * 90) / 100;
    const qint64 bytesBeforeDownloadLimit = qMax<qint64>(0, maxAllowedUsedBytes - usedBytes);

    if (usedBytes >= maxAllowedUsedBytes) {
        const QString msg = QStringLiteral("存储空间不足：当前已使用%1%")
                .arg(usedPercent);
        setStatusText(QStringLiteral("存储空间不足"));
        showToast(QStringLiteral("存储空间不足"), 3000);
        appendLog(msg);
        emit ftpDownloadFailed(displayName, msg);
        return false;
    }

    const qint64 remoteSize = queryFtpRemoteFileSize(host, port, user, pass, remotePath, &err);
    if (remoteSize < 0) {
        setStatusText(QStringLiteral("下载前校验失败"));
        appendLog(QStringLiteral("下载前校验失败：%1").arg(err));
        showToast(QStringLiteral("下载前校验失败"), 2500);
        emit ftpDownloadFailed(displayName, err);
        return false;
    }

    if (remoteSize > bytesBeforeDownloadLimit) {
        const QString msg = QStringLiteral("存储空间不足：文件大小=%1 bytes，仅可写入=%2 bytes")
                .arg(remoteSize)
                .arg(bytesBeforeDownloadLimit);
        setStatusText(QStringLiteral("存储空间不足"));
        showToast(QStringLiteral("存储空间不足"), 3000);
        appendLog(msg);
        emit ftpDownloadFailed(displayName, msg);
        return false;
    }

    // 预留 10MB 安全余量，避免文件系统波动/日志/rename 等边界问题
    static const qint64 kSafetyMarginBytes = 10LL * 1024 * 1024;
    if (remoteSize > availableBytes - kSafetyMarginBytes) {
        const QString msg = QStringLiteral("Insufficient storage space: File size=%1 bytes，space available=%2 bytes")
                .arg(remoteSize)
                .arg(availableBytes);

        setStatusText(QStringLiteral("存储空间不足"));
        showToast(QStringLiteral("存储空间不足"), 3000);
        appendLog(msg);
        emit ftpDownloadFailed(displayName, msg);
        return false;
    }

    appendLog(QStringLiteral("下载前校验通过：文件大小=%1 bytes，可用空间=%2 bytes")
              .arg(remoteSize)
              .arg(availableBytes));
    return true;
}


// ------------------------------
// worker -> UI
// ------------------------------
void FtpPage::onWorkerLog(const QString &line)
{
    appendLog(line);
}

/** @brief 接收后台 FTP Worker 状态并同步到页面状态栏。 */
void FtpPage::onWorkerStatus(const QString &text)
{
    setStatusText(text);
}

// ------------------------------
// 键盘相关
// ------------------------------
void FtpPage::installKeyboardFilters()
{
    hostEdit->installEventFilter(this);
    portEdit->installEventFilter(this);
    userEdit->installEventFilter(this);
    passEdit->installEventFilter(this);
    remotePathEdit->installEventFilter(this);
    localPathEdit->installEventFilter(this);
    streamUrlEdit->installEventFilter(this);
}

/// @brief 统一拦截输入框点击，弹出软键盘。
bool FtpPage::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::MouseButtonPress) {
        if (auto *edit = qobject_cast<QLineEdit *>(watched)) {
            if (!edit->isEnabled() || edit->isReadOnly()) {
                return QWidget::eventFilter(watched, event);
            }

            if (edit == hostEdit) {
                openKeyboardFor(edit, "请输入 FTP Host（IP/域名）", QLineEdit::Normal, 64);
                return true;
            }
            if (edit == portEdit) {
                openKeyboardForPort(edit, "请输入端口号（1-65535）", 1, 65535);
                return true;
            }
            if (edit == userEdit) {
                openKeyboardFor(edit, "请输入用户名", QLineEdit::Normal, 64);
                return true;
            }
            if (edit == passEdit) {
                openKeyboardFor(edit, "请输入密码", QLineEdit::Password, 64);
                return true;
            }
            if (edit == remotePathEdit) {
                openKeyboardFor(edit, "请输入 Remote Path（文件，如 /xx/video.mp4）", QLineEdit::Normal, 256);
                return true;
            }
            if (edit == localPathEdit) {
                openKeyboardFor(edit, "请输入 Local Path（可选）", QLineEdit::Normal, 256);
                return true;
            }
            if (edit == streamUrlEdit) {
                // 先弹出历史列表；没有历史也会给“手动输入...”
                auto* me = static_cast<QMouseEvent*>(event);
                showStreamUrlHistoryPopup(me->globalPos());
                return true;
            }

            openKeyboardFor(edit, "请输入内容", edit->echoMode(), 128);
            return true;
        }
    }

    return QWidget::eventFilter(watched, event);
}

/** @brief 使用指定回显模式和长度编辑普通文本。 */
void FtpPage::openKeyboardFor(QLineEdit *edit, const QString &title,
                             QLineEdit::EchoMode echo, int maxLen)
{
    if (!edit) return;

    bool ok = false;
    const QString text = KeyboardDialog::getText(
        this, title, edit->text(), echo, maxLen, &ok
    );

    if (ok) {
        const QString t = text.trimmed();
        edit->setText(t);
        edit->setFocus();

        if (edit == streamUrlEdit) {
            addStreamUrlHistory(t);
        }
    }
}

/// @brief 显示顶部短时提示条。
void FtpPage::showToast(const QString& text, int ms)
{
    if (!toastLabel) return;

    toastLabel->setText(text);
    toastLabel->show();
    toastLabel->raise();

    if (toastTimer) {
        toastTimer->start(ms);
    }
}


/** @brief 使用整数键盘编辑并限制端口范围。 */
void FtpPage::openKeyboardForPort(QLineEdit *edit, const QString &title,
                                 int minV, int maxV)
{
    if (!edit) return;

    bool ok = false;
    const QString text = KeyboardDialog::getText(
        this, title,
        edit->text().isEmpty() ? QString::number(21) : edit->text(),
        QLineEdit::Normal, 5, &ok
    );
    if (!ok) return;

    bool okNum = false;
    int v = text.trimmed().toInt(&okNum);
    if (!okNum) {
        QMessageBox::warning(this, "提示", "端口必须是数字");
        return;
    }
    if (v < minV || v > maxV) {
        QMessageBox::warning(this, "提示", QString("端口范围 %1-%2").arg(minV).arg(maxV));
        return;
    }

    edit->setText(QString::number(v));
    edit->setFocus();
}

/// @brief 异步发起 TCP 端口连通性测试。
void FtpPage::startTcpTest(const QString &host, int port)
{
    setStatusText(QStringLiteral("正在测试..."));
    appendLog(QStringLiteral("开始TCP测试：%1:%2").arg(host).arg(port));

    tcpTimer->start(3000);          // 3秒超时
    tcpSock->abort();               // 清理旧状态
    tcpSock->connectToHost(host, port);
}


/** @brief 根据 URL 文件名和视频目录选择安全的本地保存路径。 */
QString FtpPage::pickHttpSaveFilePath(const QString& urlStr)
{
    // 优先使用你页面上“Local Path”（如果是目录就拼文件名）
    QString base = localPathEdit ? localPathEdit->text().trimmed() : QString();

    // 如果没填，就弹窗选目录
    if (base.isEmpty()) {
        const QString dir = QFileDialog::getExistingDirectory(this, QStringLiteral("选择HTTP视频保存目录"));
        if (dir.isEmpty()) return QString();
        base = dir;
        if (localPathEdit) localPathEdit->setText(base);
    }

    QFileInfo baseInfo(base);
    QDir saveDir;

    // base 是目录：直接用
    if (baseInfo.exists() && baseInfo.isDir()) {
        saveDir = QDir(base);
    } else if (base.endsWith('/')) {
        saveDir = QDir(base);
        if (!saveDir.exists()) saveDir.mkpath(".");
    } else {
        // base 是一个文件路径：直接返回（用户自己指定文件名）
        QFileInfo fi(base);
        QDir d = fi.dir();
        if (!d.exists()) d.mkpath(".");
        return base;
    }

    // 从 URL 提取文件名
    QUrl u(urlStr);
    QString name = QFileInfo(u.path()).fileName();
    if (name.isEmpty()) {
        name = QDateTime::currentDateTime().toString("yyyyMMdd_hhmmss") + ".mp4";
    }
    // 保证后缀
    if (!name.endsWith(".mp4", Qt::CaseInsensitive)) {
        name += ".mp4";
    }

    return saveDir.filePath(name);
}

/** @brief 通过 curl 子进程下载 HTTP 媒体文件。 */
void FtpPage::startHttpMp4DownloadByProcess(const QString& urlStr, const QString& localFile)
{
    emit ftpDownloadStarted(QFileInfo(localFile).fileName());

    if (!httpProc) return;

    // 1) curl 路径
    QString curlPath;
    if (QFileInfo("/usr/bin/curl").exists()) curlPath = "/usr/bin/curl";
    else if (QFileInfo("/bin/curl").exists()) curlPath = "/bin/curl";
    else {
        setStatusText(QStringLiteral("缺少 curl"));
        showToast(QStringLiteral("系统缺少curl，无法HTTP下载"), 3000);
        appendLog(QStringLiteral("系统找不到 curl（/usr/bin/curl 或 /bin/curl）"));
        emit ftpDownloadFailed(QFileInfo(localFile).fileName(), QStringLiteral("The system is lacking curl."));
        return;
    }

    // 2) 清理 URL（非常关键）
    QString url = urlStr.trimmed();
    // 去掉用户粘贴进来的首尾引号
    if ((url.startsWith('\"') && url.endsWith('\"')) ||
        (url.startsWith('\'') && url.endsWith('\''))) {
        url = url.mid(1, url.size() - 2).trimmed();
    }

    if (url.isEmpty()) {
        setStatusText(QStringLiteral("URL为空"));
        showToast(QStringLiteral("URL为空"), 2000);
        return;
    }

    // 3) 确保目录存在
    QFileInfo fi(localFile);
    QDir d = fi.dir();
    if (!d.exists()) d.mkpath(".");

    // 4) 临时文件 + 最终文件
    const QString partFile = localFile + ".part";

    // 5) 若已有任务在跑，先终止
    if (httpProc->state() != QProcess::NotRunning) {
        appendLog(QStringLiteral("已有HTTP下载任务，终止旧任务"));
        httpProc->kill();
    }

    // 6) 记录状态
    currentHttpDownloadLocalFile_ = localFile;
    httpDownloading_ = true;
    httpProgressBuffer_.clear();
    httpLastProgress_ = -1;

    // 7) curl 参数
    // -L 跟随重定向
    // -f/--fail：HTTP 4xx/5xx 直接失败（否则会把错误页保存下来）
    // --show-error：失败时输出原因
    // -o partFile：先写 part，成功再改名
    QStringList args;
    args << "-L"
         << "--fail"
         << "--show-error"
         << "-o" << partFile
         << "--connect-timeout" << "8"
         << "--max-time" << "0"
         << url;

    setStatusText(QStringLiteral("正在HTTP下载..."));
    showToast(QStringLiteral("开始HTTP下载..."), 1500);
    appendLog(QStringLiteral("HTTP下载：%1 %2").arg(curlPath, args.join(' ')));

    emit ftpDownloadProgress(QFileInfo(localFile).fileName(), 0);

    // 8) 开始
    httpProc->start(curlPath, args);
}

/** @brief 从设置中加载最近使用的流地址。 */
void FtpPage::loadStreamUrlHistory()
{
    QSettings s; // 默认 OrganizationName/ApplicationName
    streamUrlHistory_ = s.value(kStreamUrlHistoryKey).toStringList();

    // 清理空项 + 截断
    QStringList cleaned;
    for (auto u : streamUrlHistory_) {
        u = u.trimmed();
        if (!u.isEmpty() && !cleaned.contains(u)) cleaned << u;
        if (cleaned.size() >= kMaxUrlHistory) break;
    }
    streamUrlHistory_ = cleaned;
}

/** @brief 持久化最多八条流地址历史。 */
void FtpPage::saveStreamUrlHistory()
{
    QSettings s;
    s.setValue(kStreamUrlHistoryKey, streamUrlHistory_);
}

/** @brief 去重后把地址移到历史首位。 */
void FtpPage::addStreamUrlHistory(const QString& url)
{
    QString u = url.trimmed();
    u = trimQuotes(u);
    if (u.isEmpty()) return;

    streamUrlHistory_.removeAll(u);   // 去重：把旧的删掉
    streamUrlHistory_.prepend(u);     // 最新放最前
    while (streamUrlHistory_.size() > kMaxUrlHistory)
        streamUrlHistory_.removeLast();

    saveStreamUrlHistory();
}


/** @brief 在指定全局位置显示流地址历史菜单。 */
void FtpPage::showStreamUrlHistoryPopup(const QPoint& globalPos)
{
    if (!streamUrlMenu_) streamUrlMenu_ = new QMenu(this);
    streamUrlMenu_->clear();

    // 历史项
    for (const auto& u : streamUrlHistory_) {
        QAction* act = streamUrlMenu_->addAction(u);
        connect(act, &QAction::triggered, this, [this, u]{
            streamUrlEdit->setText(u);
            streamUrlEdit->setFocus();
        });
    }

    if (!streamUrlHistory_.isEmpty())
        streamUrlMenu_->addSeparator();

    // 手动输入
    QAction* manual = streamUrlMenu_->addAction(QStringLiteral("手动输入..."));
    connect(manual, &QAction::triggered, this, [this]{
        openKeyboardFor(streamUrlEdit, "请输入 Stream URL（rtsp/http...）", QLineEdit::Normal, 512);
    });

    streamUrlMenu_->popup(globalPos);
}

// 输入配置文件
/// @brief 从 net_cfg.ini 加载页面默认输入参数。
bool FtpPage::loadInputsFromConfigFile(const QString& cfgDir, bool showTip)
{
    const QString cfgPath = QDir(cfgDir).filePath(configFileName_);

    if (!QFileInfo::exists(cfgPath)) {
        if (showTip) {
            showToast(QStringLiteral("已启用配置文件填充：%1").arg(cfgPath), 2500);
        }
        appendLog(QStringLiteral("配置读取失败：文件不存在 %1").arg(cfgPath));
        return false;
    }

    QSettings ini(cfgPath, QSettings::IniFormat);
    ini.setIniCodec("UTF-8");

    const bool useCfg = ini.value("main/use_config", false).toBool();
    if (!useCfg) {
        showToast(QStringLiteral("配置文件开关use_config=false，继续手动输入"), 2800);
        appendLog(QStringLiteral("配置文件存在，但use_config=false，不覆盖输入框"));
        return true; // 读取成功但不启用覆盖
    }

    // 读取字段（给默认值：如果缺失就保持原值或用默认）
    const QString host = ini.value("ftp/host", hostEdit->text()).toString().trimmed();
    const int port      = ini.value("ftp/port", portEdit->text().toInt()).toInt();
    const QString user  = ini.value("ftp/username", userEdit->text()).toString();
    const QString pass  = ini.value("ftp/password", passEdit->text()).toString();
    const QString url   = ini.value("stream/url", streamUrlEdit->text()).toString().trimmed();
    const QString rpathRaw = ini.value("ftp/remote_path", remotePathEdit->text()).toString().trimmed();
    const QString lpathRaw = ini.value("ftp/local_path", localPathEdit->text()).toString().trimmed();

    const QString rpath = normalizeRemoteFilePath(rpathRaw);
    const QString lpath = normalizeLocalDirPath(lpathRaw, rpath);


    // 基础合法性保护
    int safePort = port;
    if (safePort <= 0 || safePort > 65535) safePort = 21;

    // 填充 UI
    if (hostEdit) hostEdit->setText(host);
    if (portEdit) portEdit->setText(QString::number(safePort));
    if (userEdit) userEdit->setText(user);
    if (passEdit) passEdit->setText(pass);
    if (remotePathEdit) remotePathEdit->setText(rpath);
    if (localPathEdit) localPathEdit->setText(lpath);
    if (streamUrlEdit) streamUrlEdit->setText(url);

    appendLog(QStringLiteral("配置清理：remote_path '%1' -> '%2'").arg(rpathRaw, rpath));
    appendLog(QStringLiteral("配置清理：local_path '%1' -> '%2'").arg(lpathRaw, lpath));

    // 读配置时就把坏值修回 ini
    if (rpath != rpathRaw) ini.setValue("ftp/remote_path", rpath);
    if (lpath != lpathRaw) ini.setValue("ftp/local_path", lpath);
    if (rpath != rpathRaw || lpath != lpathRaw) ini.sync();

    if (showTip) {
        showToast(QStringLiteral("已启用配置文件填充：%1").arg(cfgPath), 2500);
    }
    appendLog(QStringLiteral("配置填充成功：%1").arg(cfgPath));

    // 加入URL历史记忆
    // addStreamUrlHistory(url);

    return true;
}

/** @brief 预加载自动 FTP 参数，供 MQTT 下载命令直接使用。 */
void FtpPage::prepareAutoFtp()
{
    appendLog("自动模式：FTP模块已就绪，等待 MQTT 下发下载任务");

    const QString cfgPath = QDir(configDir_).filePath(configFileName_);
    if (!QFileInfo::exists(cfgPath)) {
        appendLog(QStringLiteral("自动FTP提示：配置文件不存在 %1").arg(cfgPath));
        return;
    }

    QSettings ini(cfgPath, QSettings::IniFormat);
    ini.setIniCodec("UTF-8");

    const bool autoEnable = ini.value("auto/enable", false).toBool();
    const bool autoFtp    = ini.value("auto/auto_ftp", false).toBool();

    if (!autoEnable || !autoFtp) {
        appendLog(QStringLiteral("自动FTP未启用：auto/enable=%1 auto/auto_ftp=%2")
                  .arg(autoEnable ? "true" : "false")
                  .arg(autoFtp ? "true" : "false"));
        return;
    }

    appendLog(QStringLiteral("自动FTP已启用：等待 MQTT 写入 FTP 下载参数"));
}


/** @brief 清除自动下载任务键和预检参数。 */
void FtpPage::clearAutoDownloadState()
{
    autoDownloadInProgress_ = false;
    activeAutoDownloadKey_.clear();
    preflightHost_.clear();
    preflightPort_ = 21;
    preflightUser_.clear();
    preflightPass_.clear();
    preflightRemotePath_.clear();
    preflightLocalPath_.clear();
    preflightDisplayName_.clear();
}


/** @brief 终止自动下载预检并统一发送失败信号。 */
void FtpPage::failAutoDownloadPreflight(const QString &reason)
{
    if (!autoDownloadInProgress_) {
        return;
    }

    const QString fileName = preflightDisplayName_;
    const QString finalReason = reason.trimmed().isEmpty()
            ? QStringLiteral("FTP preflight failed")
            : reason.trimmed();

    setStatusText(QStringLiteral("FTP下载前检查失败"));
    appendLog(QStringLiteral("自动下载预检查失败：%1").arg(finalReason));
    showToast(QStringLiteral("FTP连接失败，继续本地播放"), 3000);
    clearAutoDownloadState();
    emit ftpDownloadFailed(fileName, finalReason);
}


/** @brief 启动自动下载前的异步远端信息和空间预检。 */
void FtpPage::startAutoDownloadPreflight(const QString &host, int port,
                                         const QString &user, const QString &pass,
                                         const QString &remotePath,
                                         const QString &localPath,
                                         const QString &displayName,
                                         const QString &taskKey)
{
    if (!ftpPreflightProc_) {
        failAutoDownloadPreflight(QStringLiteral("FTP preflight process is not initialized"));
        return;
    }

    QString curlPath;
    if (QFileInfo("/usr/bin/curl").exists()) curlPath = "/usr/bin/curl";
    else if (QFileInfo("/bin/curl").exists()) curlPath = "/bin/curl";
    else {
        failAutoDownloadPreflight(QStringLiteral("The system is lacking curl."));
        return;
    }

    if (ftpPreflightProc_->state() != QProcess::NotRunning) {
        failAutoDownloadPreflight(QStringLiteral("Another FTP preflight is already running"));
        return;
    }

    preflightHost_ = host;
    preflightPort_ = port;
    preflightUser_ = user;
    preflightPass_ = pass;
    preflightRemotePath_ = remotePath;
    preflightLocalPath_ = localPath;
    preflightDisplayName_ = displayName;
    activeAutoDownloadKey_ = taskKey;

    bool encodedMode = false;
    const QString url = buildFtpUrl(host, port, remotePath, &encodedMode);

    QStringList args;
    args << "--ftp-ssl"
         << "--ftp-pasv"
         << "--insecure"
         << "-u" << QString("%1:%2").arg(user, pass)
         << "--head"
         << "--silent"
         << "--show-error"
         << "--connect-timeout" << "8"
         << "--speed-limit" << "1"
         << "--speed-time" << "10"
         << "--max-time" << "20"
         << url;

    appendLog(QStringLiteral("自动下载异步预检查：ftp://%1:%2 remote='%3'")
              .arg(host).arg(port).arg(remotePath));
    ftpPreflightProc_->start(curlPath, args);
}


/** @brief 解析预检进程结果并继续下载或报告失败。 */
void FtpPage::handleAutoDownloadPreflightFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    if (!ftpPreflightProc_) {
        return;
    }

    const QString stdOut = QString::fromLocal8Bit(ftpPreflightProc_->readAllStandardOutput());
    const QString stdErr = QString::fromLocal8Bit(ftpPreflightProc_->readAllStandardError());

    if (!autoDownloadInProgress_ || activeAutoDownloadKey_.isEmpty()) {
        return;
    }

    if (exitStatus != QProcess::NormalExit || exitCode != 0) {
        failAutoDownloadPreflight(
                    QStringLiteral("Failed to query remote FTP file (exit=%1): %2")
                    .arg(exitCode)
                    .arg(stdErr.trimmed()));
        return;
    }

    const QString allText = stdOut + "\n" + stdErr;
    const QRegularExpression re("Content-Length\\s*:\\s*(\\d+)",
                                QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch match = re.match(allText);
    if (!match.hasMatch()) {
        failAutoDownloadPreflight(QStringLiteral("Failed to obtain remote FTP file size"));
        return;
    }

    bool sizeOk = false;
    const qint64 remoteSize = match.captured(1).toLongLong(&sizeOk);
    if (!sizeOk || remoteSize < 0) {
        failAutoDownloadPreflight(QStringLiteral("Failed to parse remote FTP file size"));
        return;
    }

    const QString host = preflightHost_;
    const int port = preflightPort_;
    const QString user = preflightUser_;
    const QString pass = preflightPass_;
    const QString remotePath = preflightRemotePath_;
    const QString localPath = preflightLocalPath_;
    const QString displayName = preflightDisplayName_;

    const QFileInfo localInfo(localPath);
    if (localInfo.exists() && localInfo.isFile() && localInfo.size() == remoteSize) {
        const QString reason = QStringLiteral("文件已存在且大小一致：%1 (%2 bytes)")
                .arg(localInfo.fileName())
                .arg(remoteSize);
        lastFinishedAutoDownloadKey_ = activeAutoDownloadKey_;
        clearAutoDownloadState();
        setStatusText(QStringLiteral("文件已存在，无需重复下载"));
        showToast(QStringLiteral("文件已存在，无需重复下载"), 3000);
        appendLog(reason);
        emit ftpDownloadAlreadyExists(localPath, reason);
        return;
    }

    QString storageError;
    qint64 totalBytes = 0;
    const qint64 availableBytes = queryAvailableBytesForPath(localPath, &totalBytes, &storageError);
    if (availableBytes < 0 || totalBytes <= 0 || availableBytes > totalBytes) {
        failAutoDownloadPreflight(storageError.isEmpty()
                                  ? QStringLiteral("Invalid storage space information")
                                  : storageError);
        return;
    }

    const qint64 usedBytes = totalBytes - availableBytes;
    const int usedPercent = static_cast<int>((usedBytes * 100) / totalBytes);
    const qint64 maxAllowedUsedBytes = (totalBytes * 90) / 100;
    const qint64 bytesBeforeDownloadLimit = qMax<qint64>(0, maxAllowedUsedBytes - usedBytes);
    static const qint64 kSafetyMarginBytes = 10LL * 1024 * 1024;

    QString storageReason;
    if (usedBytes >= maxAllowedUsedBytes) {
        storageReason = QStringLiteral("存储空间不足：当前已使用%1%，已达到90%下载保护阈值")
                .arg(usedPercent);
    } else if (remoteSize > bytesBeforeDownloadLimit) {
        storageReason = QStringLiteral("存储空间不足：文件大小=%1 bytes，90%阈值前仅可写入=%2 bytes")
                .arg(remoteSize)
                .arg(bytesBeforeDownloadLimit);
    } else if (remoteSize > availableBytes - kSafetyMarginBytes) {
        storageReason = QStringLiteral("存储空间不足：文件大小=%1 bytes，可用空间=%2 bytes")
                .arg(remoteSize)
                .arg(availableBytes);
    }

    if (!storageReason.isEmpty()) {
        clearAutoDownloadState();
        setStatusText(QStringLiteral("存储空间不足"));
        showToast(QStringLiteral("存储空间不足"), 3000);
        appendLog(storageReason);
        emit ftpDownloadStorageRejected(displayName, storageReason);
        return;
    }

    appendLog(QStringLiteral("自动下载预检查通过：远端大小=%1 bytes，可用空间=%2 bytes")
              .arg(remoteSize)
              .arg(availableBytes));

    startFtpDownloadByProcess(host, port, user, pass, remotePath, localPath);
}


/** @brief 根据已加载配置发起一次去重的自动 FTP 下载。 */
void FtpPage::startAutoDownloadFromConfig()
{
    appendLog("自动模式：准备从配置文件读取 FTP 下载参数");

    const QString cfgPath = QDir(configDir_).filePath(configFileName_);
    if (!QFileInfo::exists(cfgPath)) {
        const QString reason = QStringLiteral("自动下载失败：配置文件不存在 %1").arg(cfgPath);
        appendLog(reason);
        emit ftpDownloadRequestRejected(QString(), reason);
        return;
    }

    QSettings ini(cfgPath, QSettings::IniFormat);
    ini.setIniCodec("UTF-8");

    const bool autoEnable = ini.value("auto/enable", false).toBool();
    const bool autoFtp    = ini.value("auto/auto_ftp", false).toBool();

    if (!autoEnable || !autoFtp) {
        const QString reason = QStringLiteral("自动下载未启用：auto/enable=%1 auto/auto_ftp=%2")
                .arg(autoEnable ? "true" : "false")
                .arg(autoFtp ? "true" : "false");
        appendLog(reason);
        emit ftpDownloadRequestRejected(QString(), reason);
        return;
    }

    if (!loadInputsFromConfigFile(configDir_, false)) {
        const QString reason = QStringLiteral("自动下载失败：读取配置文件失败");
        appendLog(reason);
        emit ftpDownloadRequestRejected(QString(), reason);
        return;
    }

    QString host, user, pass;
    int port = 0;
    if (!validateBasicInputs(host, port, user, pass)) {
        const QString reason = QStringLiteral("自动下载失败：FTP基础参数无效");
        appendLog(reason);
        emit ftpDownloadRequestRejected(QString(), reason);
        return;
    }

    QString remotePath = remotePathEdit ? remotePathEdit->text().trimmed() : QString();
    remotePath = normalizeRemoteFilePath(remotePath);
    if (remotePath.isEmpty()) {
        const QString reason = QStringLiteral("自动下载失败：remote_path 为空");
        appendLog(reason);
        emit ftpDownloadRequestRejected(QString(), reason);
        return;
    }

    if (isRemoteDir(remotePath)) {
        const QString reason = QStringLiteral("自动下载暂不支持目录 remote_path=%1").arg(remotePath);
        appendLog(reason);
        emit ftpDownloadRequestRejected(QFileInfo(remotePath).fileName(), reason);
        return;
    }

    QString localDir = Rk3566Platform::videoDir();
    localDir = normalizeLocalDirPath(localDir, remotePath);
    if (localDir.isEmpty()) {
        const QString reason = QStringLiteral("自动下载失败：固定 local_dir 非法");
        appendLog(reason);
        emit ftpDownloadRequestRejected(QFileInfo(remotePath).fileName(), reason);
        return;
    }

    if (remotePathEdit) remotePathEdit->setText(remotePath);
    if (localPathEdit)  localPathEdit->setText(localDir);

    ini.setValue("ftp/remote_path", remotePath);
    ini.setValue("ftp/local_path", localDir);
    ini.sync();

    const QString finalLocalPath = resolveFtpLocalFilePath(localDir, remotePath);
    const QString taskKey = QString("%1|%2|%3|%4")
            .arg(host)
            .arg(port)
            .arg(remotePath)
            .arg(finalLocalPath);

    if (autoDownloadInProgress_ && activeAutoDownloadKey_ == taskKey) {
        appendLog(QStringLiteral("自动下载忽略：相同任务正在进行中"));
        return;
    }
    if (autoDownloadInProgress_) {
        appendLog(QStringLiteral("自动下载忽略：已有自动下载任务正在执行"));
        return;
    }

    const QString displayName = QFileInfo(finalLocalPath).fileName();
    appendLog(QStringLiteral("自动下载目录：%1").arg(localDir));
    appendLog(QStringLiteral("自动下载开始：ftp://%1:%2 remote='%3' -> local='%4'")
              .arg(host).arg(port).arg(remotePath, finalLocalPath));

    autoDownloadInProgress_ = true;
    activeAutoDownloadKey_ = taskKey;

    QString storageError;
    qint64 totalBytes = 0;
    const qint64 availableBytes = queryAvailableBytesForPath(finalLocalPath, &totalBytes, &storageError);
    if (availableBytes < 0 || totalBytes <= 0 || availableBytes > totalBytes) {
        clearAutoDownloadState();
        const QString reason = storageError.isEmpty()
                ? QStringLiteral("Invalid storage space information")
                : storageError;
        appendLog(reason);
        emit ftpDownloadRequestRejected(displayName, reason);
        return;
    }

    const qint64 usedBytes = totalBytes - availableBytes;
    const qint64 maxAllowedUsedBytes = (totalBytes * 90) / 100;
    if (usedBytes >= maxAllowedUsedBytes) {
        const int usedPercent = static_cast<int>((usedBytes * 100) / totalBytes);
        const QString reason = QStringLiteral("存储空间不足：当前已使用%1%，已达到90%下载保护阈值")
                .arg(usedPercent);
        clearAutoDownloadState();
        setStatusText(QStringLiteral("存储空间不足"));
        showToast(QStringLiteral("存储空间不足"), 3000);
        appendLog(reason);
        emit ftpDownloadStorageRejected(displayName, reason);
        return;
    }

    startAutoDownloadPreflight(host, port, user, pass, remotePath,
                               finalLocalPath, displayName, taskKey);
}
