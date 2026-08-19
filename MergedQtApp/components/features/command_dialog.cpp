/**
 * @file command_dialog.cpp
 * @brief 设备调试与配置命令对话框的实现。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#include <QMap>
#include <QTimer>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QDebug>
#include <QSize>
#include <QFrame>
#include <QScreen>
#include <QStyleFactory>
#include <QGuiApplication>

#include "command_dialog.h"
#include "platform/rk3566_platform.h"


/** @brief 按动作键触发与对应按钮相同的业务信号。 */
void CommandDialog::triggerAction(const QString &actionKey)
{
    emit actionClicked(actionKey);
}

/** @brief 从配置读取对话框尺寸并限制在当前屏幕可用范围内。 */
QSize CommandDialog::loadWindowSizeFromConfig() const
{
    // 外部资源根目录
    const QString configPath = Rk3566Platform::uiConfigPath();
    qDebug() << "开始读取磁盘配置文件:" << configPath;

    // 默认窗口大小
    int windowWidth = 1024;
    int windowHeight = 768;

    if (QFile::exists(configPath)) {
        QSettings settings(configPath, QSettings::IniFormat);
        settings.setIniCodec("UTF-8");

        settings.beginGroup("backRect");
        windowWidth = settings.value("backPic_x_Size", 1024).toInt();
        windowHeight = settings.value("backPic_y_Size", 768).toInt();
        settings.endGroup();

        qDebug() << "读取配置文件成功:" << configPath
                 << ", backPic_x_Size=" << windowWidth
                 << ", backPic_y_Size=" << windowHeight;
    } else {
        qDebug() << "配置文件不存在，设置界面使用默认窗口大小: 1024x768";
    }

    // 防御性限制，避免异常值
    if (windowWidth < 640)  windowWidth = 640;
    if (windowHeight < 480) windowHeight = 480;

    return QSize(windowWidth, windowHeight);
}


/** @brief 创建配置卡片、日志区并从数据库加载初始值。 */
CommandDialog::CommandDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle("命令配置");
    setModal(false);
    setWindowFlag(Qt::WindowContextHelpButtonHint, false);

    setStyle(QStyleFactory::create("Fusion"));
    setAttribute(Qt::WA_StyledBackground, true);

    const QSize cfgSize = loadWindowSizeFromConfig();
    QSize finalSize = cfgSize;

    QScreen *screen = QGuiApplication::primaryScreen();
    if (screen) {
        const QRect avail = screen->availableGeometry();

        finalSize.setWidth(qMin(finalSize.width(), avail.width()));
        finalSize.setHeight(qMin(finalSize.height(), avail.height()));
    }

    // 允许 800x600 这种小屏，不再强制拉到 920x620
    if (finalSize.width() < 640)  finalSize.setWidth(640);
    if (finalSize.height() < 480) finalSize.setHeight(480);

    // 小屏启用紧凑布局
    m_compactLayout = (finalSize.width() <= 900 || finalSize.height() <= 620);

    resize(finalSize);

    // 小屏不要强制 900x600，否则 800x600 会被撑出屏幕
    if (m_compactLayout) {
        setMinimumSize(640, 480);
    } else {
        setMinimumSize(900, 600);
    }

    auto *root = new QVBoxLayout(this);

    if (m_compactLayout) {
        root->setContentsMargins(8, 8, 8, 8);
        root->setSpacing(8);
    } else {
        root->setContentsMargins(14, 12, 14, 12);
        root->setSpacing(12);
    }

    root->addWidget(makeHeaderCard(), 0);
    root->addWidget(buildContent(), 1);

    auto *bottom = new QHBoxLayout();
    bottom->setContentsMargins(0, 0, 0, 0);
    bottom->addStretch();

    auto *btnExit = new QPushButton("关闭", this);
    btnExit->setObjectName("btnSecondary");
    btnExit->setCursor(Qt::PointingHandCursor);
    btnExit->setFixedSize(84, 36);
    connect(btnExit, &QPushButton::clicked, this, &QDialog::close);
    bottom->addWidget(btnExit);

    root->addLayout(bottom);

    applyStyle();

    appendLog("=== 命令窗口已打开 ===");

    connect(this, &CommandDialog::sendClicked, this, &CommandDialog::onSendClicked);
    connect(this, &CommandDialog::actionClicked, this, &CommandDialog::onActionClicked);

    // 刷卡扇区下发回执
    connect(IcEventBridge::instance(), &IcEventBridge::cardReaderCommandFinished,
            this,
            [this](const QString &sourceTag, bool ok, const QString &reason) {
        if (sourceTag != QStringLiteral("command_dialog_set_sector")) {
            return;
        }

        if (ok) {
            appendLog("刷卡扇区命令已下发到刷卡器");
        } else {
            appendLog(QString("刷卡扇区命令下发失败: %1").arg(reason));
        }
    }, Qt::QueuedConnection);

    initDbAndLoadUi();
}

/** @brief 向日志框追加一行；调用方必须在 UI 线程调用或通过 invokeMethod 投递。 */
void CommandDialog::appendLog(const QString &line)
{
    if (!m_logEdit) return;

    const QString ts = QDateTime::currentDateTime().toString("hh:mm:ss.zzz");
    m_logEdit->appendPlainText(QString("[%1] %2").arg(ts, line));

    // 限制行数，避免越跑越卡
    const int maxBlocks = 800; // 自行调
    if (m_logEdit->document()->blockCount() > maxBlocks) {
        QTextCursor c(m_logEdit->document());
        c.movePosition(QTextCursor::Start);
        for (int i = 0; i < 200; ++i) {
            c.select(QTextCursor::LineUnderCursor);
            c.removeSelectedText();
            c.deleteChar();
        }
    }
}

/** @brief 清空调试日志框。 */
void CommandDialog::clearLog()
{
    if (m_logEdit) m_logEdit->clear();
}


/** 创建带统一边距、标题和行容器的配置分区。 */
QWidget* CommandDialog::makeSectionCard(const QString& title, const QList<QWidget*>& rows)
{
    auto *card = new QWidget(this);
    card->setObjectName("sectionCard");
    card->setAttribute(Qt::WA_StyledBackground, true);

    auto *v = new QVBoxLayout(card);
    if (m_compactLayout) {
        v->setContentsMargins(10, 10, 10, 8);
        v->setSpacing(0);
    } else {
        v->setContentsMargins(16, 14, 16, 10);
        v->setSpacing(0);
    }

    auto *titleLabel = new QLabel(title, card);
    titleLabel->setObjectName("sectionTitle");
    v->addWidget(titleLabel);
    v->addSpacing(6);

    for (int i = 0; i < rows.size(); ++i) {
        QWidget *row = rows[i];
        v->addWidget(row);
    }

    return card;
}


// 顶部头部卡片
QWidget* CommandDialog::makeHeaderCard()
{
    auto *card = new QWidget(this);
    card->setObjectName("headerCard");
    card->setAttribute(Qt::WA_StyledBackground, true);

    auto *h = new QHBoxLayout(card);
    h->setContentsMargins(18, 14, 18, 14);
    h->setSpacing(8);

    auto *title = new QLabel("设备配置", card);
    title->setObjectName("pageTitle");
    h->addWidget(title);
    h->addStretch();

    return card;
}


// 快捷操作单独做成卡片
QWidget* CommandDialog::makeActionCard()
{
    auto *card = new QWidget(this);
    card->setObjectName("sectionCard");
    card->setAttribute(Qt::WA_StyledBackground, true);

    auto *v = new QVBoxLayout(card);
    v->setContentsMargins(14, 14, 14, 14);
    v->setSpacing(10);

    auto *title = new QLabel("系统操作", card);
    title->setObjectName("sectionTitle");
    v->addWidget(title);

    auto makeBtn = [this](const QString &text, const QString &obj, const QString &action) {
        auto *btn = new QPushButton(text, this);
        btn->setObjectName(obj);
        btn->setCursor(Qt::PointingHandCursor);
        btn->setFixedHeight(36);
        connect(btn, &QPushButton::clicked, this, [this, action]() {
            emit actionClicked(action);
        });
        return btn;
    };

    v->addWidget(makeBtn("同步时间", "btnGhost", "sync_time"));
    v->addWidget(makeBtn("配置二维码接口", "btnGhost", "insert_qr_api"));
    v->addWidget(makeBtn("重启设备", "btnDanger", "reboot"));

    auto *line = new QFrame(card);
    line->setObjectName("rowDivider");
    line->setFrameShape(QFrame::HLine);
    v->addWidget(line);

    auto *toolsTitle = new QLabel("维护工具", card);
    toolsTitle->setObjectName("lblMinor");
    v->addWidget(toolsTitle);

    auto *grid = new QGridLayout();
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing(8);
    grid->setVerticalSpacing(8);

    QStringList keyTexts = {"KEY1", "KEY2", "KEY3", "KEY4"};
    QStringList keyActions = {
        "legacy_key1_query_registered_no",
        "legacy_key2_query_board_time",
        "legacy_key3_query_registered_cards",
        "legacy_key4_reset"
    };

    for (int i = 0; i < keyTexts.size(); ++i) {
        auto *btn = new QPushButton(keyTexts[i], card);
        btn->setObjectName("btnGhost");
        btn->setCursor(Qt::PointingHandCursor);
        btn->setFixedHeight(34);
        connect(btn, &QPushButton::clicked, this, [this, keyActions, i]() {
            emit actionClicked(keyActions[i]);
        });
        grid->addWidget(btn, i / 2, i % 2);
    }

    v->addLayout(grid);
    return card;
}


/** @brief 创建滚动区域中的全部配置卡片。 */
QWidget* CommandDialog::buildContent()
{
    auto makeLogCard = [this](QWidget *parent, bool compact) -> QWidget* {
        auto *logCard = new QWidget(parent);
        logCard->setObjectName("logCard");
        logCard->setAttribute(Qt::WA_StyledBackground, true);

        auto *logCardLay = new QVBoxLayout(logCard);
        logCardLay->setContentsMargins(compact ? 10 : 14,
                                       compact ? 10 : 14,
                                       compact ? 10 : 14,
                                       compact ? 10 : 14);
        logCardLay->setSpacing(compact ? 8 : 10);

        auto *logHead = new QWidget(logCard);
        auto *logHeadLay = new QHBoxLayout(logHead);
        logHeadLay->setContentsMargins(0, 0, 0, 0);
        logHeadLay->setSpacing(8);

        auto *logTitle = new QLabel("运行日志", logHead);
        logTitle->setObjectName("logTitle");
        logHeadLay->addWidget(logTitle);
        logHeadLay->addStretch();

        auto *btnClear = new QPushButton("清空", logHead);
        btnClear->setObjectName("btnGhost");
        btnClear->setCursor(Qt::PointingHandCursor);
        btnClear->setFixedSize(64, 32);
        connect(btnClear, &QPushButton::clicked, this, &CommandDialog::clearLog);
        logHeadLay->addWidget(btnClear);

        m_logEdit = new QPlainTextEdit(logCard);
        m_logEdit->setObjectName("logEdit");
        m_logEdit->setReadOnly(true);
        m_logEdit->setMaximumBlockCount(800);

        if (compact) {
            m_logEdit->setMinimumHeight(120);
        }

        logCardLay->addWidget(logHead);
        logCardLay->addWidget(m_logEdit, 1);

        return logCard;
    };

    auto *securityCard = makeSectionCard("设备与身份", {
        makeRow("swipe_pwd",   "刷卡密钥",   "20250101", "保存", "btnPrimary"),
        makeRow("qr_pwd",      "二维码密钥", "250101",   "保存", "btnPrimary"),
        makeRow("fan_area",    "刷卡扇区",   "6",        "保存", "btnPrimary"),
        makeRow("elevator_no", "电梯梯号",   "1",        "保存", "btnPrimary"),
        makeRow("device_no",   "设备号",     "YCEE250101V00001", "保存", "btnPrimary"),
        makeRow("mac_addr",    "MAC 地址",   "B8.6E.3D.00.00.00", "保存", "btnPrimary")
    });

    auto *networkCard = makeSectionCard("网络配置", {
        makeRow("static_ip",     "静态 IP",      "192.168.3.111", "保存", "btnPrimary"),
        makeRow("gateway",       "网关地址",     "192.168.3.1",   "保存", "btnPrimary"),
        makeRow("subnet_mask",   "子网掩码",     "255.255.255.0", "保存", "btnPrimary"),
        makeRow("dns_server",    "DNS 服务器",   "192.168.1.1",   "保存", "btnPrimary"),
        makeRow("dns_ip",        "DNS IP",       "192.168.1.110", "保存", "btnPrimary"),
        makeRow("dns_gateway",   "DNS 网关",     "192.168.1.1",   "保存", "btnPrimary"),
        makeRow("dns_mask",      "DNS 子网掩码", "255.255.255.0", "保存", "btnPrimary"),
        makeRow("remote_server", "远程服务器",   "192.168.3.9",   "保存", "btnPrimary")
    });

    auto *runtimeCard = makeSectionCard("运行参数", {
        makeRow("card_register_count",    "卡号注册次数", "0",  "保存", "btnPrimary"),
        makeRow("msg_id",                 "消息 ID",      "0",  "保存", "btnPrimary"),
        makeRow("network_interface_mode", "网络接口模式", "0",  "保存", "btnPrimary"),
        makeRow("cardinfo_output_mode",   "卡信息输出",   "0",  "保存", "btnPrimary"),
        makeRow("server_output_mode",     "服务输出",     "0",  "保存", "btnPrimary"),
        makeRow("relay_num_value",        "继电器数量",   "3",  "保存", "btnPrimary"),
        makeRow("relay_times_value",      "继电器时长",   "10", "保存", "btnPrimary")
    });

    if (m_compactLayout) {
        // 小屏模式：整体滚动，上下排列，避免 800x600 被左右两栏挤压
        auto *scroll = new QScrollArea(this);
        scroll->setObjectName("pageScroll");
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);

        auto *wrap = new QWidget(scroll);
        auto *v = new QVBoxLayout(wrap);
        v->setContentsMargins(0, 0, 0, 0);
        v->setSpacing(8);

        v->addWidget(securityCard);
        v->addWidget(networkCard);
        v->addWidget(runtimeCard);
        v->addWidget(makeActionCard());
        v->addWidget(makeLogCard(wrap, true));

        scroll->setWidget(wrap);
        return scroll;
    }

    // 正常大屏模式：保持原来的左右布局
    auto *w = new QWidget(this);
    auto *bodyLay = new QHBoxLayout(w);
    bodyLay->setContentsMargins(0, 0, 0, 0);
    bodyLay->setSpacing(12);

    auto *leftScroll = new QScrollArea(w);
    leftScroll->setObjectName("leftScroll");
    leftScroll->setWidgetResizable(true);
    leftScroll->setFrameShape(QFrame::NoFrame);

    auto *leftWrap = new QWidget(leftScroll);
    auto *leftLay = new QVBoxLayout(leftWrap);
    leftLay->setContentsMargins(0, 0, 4, 0);
    leftLay->setSpacing(12);

    leftLay->addWidget(securityCard);
    leftLay->addWidget(networkCard);
    leftLay->addWidget(runtimeCard);
    leftLay->addStretch();

    leftScroll->setWidget(leftWrap);

    auto *rightCol = new QWidget(w);
    rightCol->setObjectName("sideColumn");
    rightCol->setMinimumWidth(320);
    rightCol->setMaximumWidth(360);

    auto *rightLay = new QVBoxLayout(rightCol);
    rightLay->setContentsMargins(0, 0, 0, 0);
    rightLay->setSpacing(12);

    rightLay->addWidget(makeActionCard(), 0);
    rightLay->addWidget(makeLogCard(rightCol, false), 1);

    bodyLay->addWidget(leftScroll, 1);
    bodyLay->addWidget(rightCol, 0);

    return w;
}


/** @brief 创建一个带输入框和操作按钮的配置行。 */
QWidget* CommandDialog::makeRow(const QString &key, const QString &labelText, const QString &defaultValue,
                               const QString &btnText, const QString &btnObjectName, bool requiredMark)
{
    auto *row = new QWidget(this);
    row->setObjectName("formRow");
    row->setAttribute(Qt::WA_StyledBackground, true);

    auto *v = new QVBoxLayout(row);
    if (m_compactLayout) {
        v->setContentsMargins(0, 6, 0, 6);
    } else {
        v->setContentsMargins(0, 10, 0, 10);
    }

    auto *top = new QWidget(row);
    auto *topLay = new QHBoxLayout(top);
    topLay->setContentsMargins(0, 0, 0, 0);
    topLay->setSpacing(6);

    auto *lab = new QLabel(top);
    lab->setObjectName("lbl");
    if (requiredMark)
        lab->setText(QString("<span style='color:#EF4444'>*</span> %1").arg(labelText));
    else
        lab->setText(labelText);

    topLay->addWidget(lab);
    topLay->addStretch();

    v->addWidget(top);

    auto *shell = new QWidget(row);
    shell->setObjectName("inputShell");
    shell->setAttribute(Qt::WA_StyledBackground, true);

    auto *h = new QHBoxLayout(shell);
    h->setContentsMargins(12, 8, 8, 8);
    h->setSpacing(6);

    auto *edit = new QLineEdit(row);
    edit->setObjectName("edit");
    edit->setText(defaultValue);
    edit->setReadOnly(true);
    edit->setFixedHeight(36);
    edit->setClearButtonEnabled(true);
    edit->setPlaceholderText(QString("请输入%1").arg(labelText));
    edit->installEventFilter(this);

    h->addWidget(edit, 1);
    m_edits[key] = edit;

    if (key == "mac_addr") {
        auto *assistBtn = new QPushButton("生成", row);
        assistBtn->setObjectName("btnAssist");
        assistBtn->setCursor(Qt::PointingHandCursor);
        assistBtn->setFixedSize(64, 34);
        connect(assistBtn, &QPushButton::clicked, this, [this]() {
            emit actionClicked("gen_mac");
        });
        h->addWidget(assistBtn);
    } else if (key == "device_no") {
        auto *assistBtn = new QPushButton("获取", row);
        assistBtn->setObjectName("btnAssist");
        assistBtn->setCursor(Qt::PointingHandCursor);
        assistBtn->setFixedSize(64, 34);
        connect(assistBtn, &QPushButton::clicked, this, [this]() {
            emit actionClicked("gen_device_no");
        });
        h->addWidget(assistBtn);
    }

    auto *btn = new QPushButton(btnText, row);
    btn->setObjectName(btnObjectName);
    btn->setCursor(Qt::PointingHandCursor);
    btn->setFixedSize(64, 34);

    connect(btn, &QPushButton::clicked, this, [this, key]() {
        auto it = m_edits.find(key);
        const QString val = (it != m_edits.end() && it.value()) ? it.value()->text() : QString();
        emit sendClicked(key, val);
    });

    h->addWidget(btn);
    v->addWidget(shell);

    return row;
}


/** @brief 将界面字段键转换为数据库字段键。 */
QString CommandDialog::dbKeyForUiKey(const QString& uiKey) const
{
    static const QMap<QString, QString> kMap = {
        {"swipe_pwd", "card_secret"},
        {"fan_area", "sector_num"},
        {"qr_pwd", "qrcode_secret"},
        {"elevator_no", "floor_num"},
        {"static_ip", "ip_static"},
        {"gateway", "ip_gateway"},
        {"subnet_mask", "ip_mask"},
        {"dns_server", "dns_server"},
        {"dns_ip", "dns_ip"},
        {"dns_gateway", "dns_gateway"},
        {"dns_mask", "dns_mask"},
        {"remote_server", "remote_ip"},
        {"network_interface_mode", "network_interface"},
        {"cardinfo_output_mode", "cardinfo_output"},
        {"server_output_mode", "server_output"},
        {"relay_num_value", "relay_num"},
        {"relay_times_value", "relay_times"},
        {"mac_addr", "mac"},
        {"device_no", "device_name"},
        {"card_register_count", "register_times"},
        {"msg_id", "message_id"},
    };
    return kMap.value(uiKey, QString());
}

/** @brief 按数据库字段规则归一化输入值，失败时填写 err。 */
QString CommandDialog::normalizeValue(const QString& dbKey, const QString& v, QString* err) const
{
    QString s = v.trimmed();
    if (err) err->clear();

    if (dbKey == "card_secret") {
        if (s.size() != 8) { if(err) *err = "刷卡密钥必须8位"; return {}; }
        return s;
    }
    if (dbKey == "qrcode_secret") {
        if (s.size() != 6) { if(err) *err = "二维码密钥必须6位"; return {}; }
        return s;
    }

    if (dbKey == "sector_num") {
        bool ok = false;
        int n = s.toInt(&ok);

        // sector_num <= 0 或 255无效
        if (!ok || n <= 0 || n == 255) {
            if (err) *err = "刷卡扇区必须为正整数且不能为255";
            return {};
        }

        return QString::number(n);
    }

    if (dbKey == "floor_num") {
        bool ok=false; int n=s.toInt(&ok);
        if (!ok || n <= 0 || n == 255) { if(err) *err="楼号必须为正整数且不能为255"; return {}; }
        return QString::number(n);
    }
    if (dbKey == "register_times" || dbKey == "message_id" ||
        dbKey == "relay_num" || dbKey == "relay_times" || dbKey == "network_interface" ||
        dbKey == "cardinfo_output" || dbKey == "server_output") {
        bool ok=false; int n=s.toInt(&ok);
        if(!ok) { if(err) *err="必须是整数"; return {}; }
        return QString::number(n);
    }
    if (dbKey == "mac") {
        // 允许UI里用 B8.6E.3D.00.00.00，也允许用 B8:6E:...
        s = s.replace('.', ':').toUpper();
        QRegularExpression re("^[0-9A-F]{2}(:[0-9A-F]{2}){5}$");
        if (!re.match(s).hasMatch()) { if(err) *err="MAC格式应为 B8:6E:3D:00:00:00"; return {}; }
        return s;
    }
    // IP类简单校验：允许直接存字符串
    if (dbKey.startsWith("ip_") || dbKey.startsWith("dns_") || dbKey == "remote_ip") {
        if (s.isEmpty()) { if(err) *err="不能为空"; return {}; }
        return s;
    }
    if (dbKey == "device_name") {
        if (s.isEmpty()) { if(err) *err="设备号不能为空"; return {}; }
        if (s.size() > 16) s = s.left(16);
        return s;
    }

    return s;
}

/** @brief 初始化配置数据库并把持久化值填入界面。 */
void CommandDialog::initDbAndLoadUi()
{
    if (!DbStore::bootstrap(m_dbPath)) {
        appendLog(QString("数据库初始化失败: %1").arg(DbStore::lastError()));
        return;
    }
    appendLog("数据库初始化成功");


    // 启动后立即做一次轻量读写自检，确认当前进程可持续读写 DB。
    const QString probe = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
    if (DbStore::setConfig("qt_db_probe_last_startup", probe)) {
        appendLog(QString("数据库读写自检通过: qt_db_probe_last_startup=%1")
                  .arg(DbStore::getConfig("qt_db_probe_last_startup", probe).toString()));
    } else {
        appendLog(QString("数据库读写自检失败: %1").arg(DbStore::lastError()));
    }


    loadDbToUi();
}

/** @brief 从数据库重新加载全部可编辑字段。 */
void CommandDialog::loadDbToUi()
{
    // 把DB里的值填到UI（若没有则用当前UI默认值保持不变）
    for (auto it = m_edits.begin(); it != m_edits.end(); ++it) {
        const QString uiKey = it.key();
        QLineEdit* edit = it.value();
        if (!edit) continue;

        const QString dbKey = dbKeyForUiKey(uiKey);
        if (dbKey.isEmpty()) continue;

        QVariant val = DbStore::getConfig(dbKey, edit->text()); // 取不到就用UI当前显示值
        edit->setText(val.toString());
    }

    appendLog("已从数据库加载参数到界面");
}

/** @brief 校验一个界面字段并写入对应数据库键。 */
bool CommandDialog::applyUiKeyToDb(const QString& uiKey, const QString& uiVal, QString* err)
{
    const QString dbKey = dbKeyForUiKey(uiKey);
    if (dbKey.isEmpty()) {
        if (err) *err = "未知参数key";
        return false;
    }

    QString normErr;
    const QString normVal = normalizeValue(dbKey, uiVal, &normErr);
    if (normVal.isEmpty() && !normErr.isEmpty()) {
        if (err) *err = normErr;
        return false;
    }

    if (!DbStore::setConfig(dbKey, normVal)) {
        if (err) *err = QString("写入数据库失败: %1").arg(DbStore::lastError());
        return false;
    }

    // 写完再读回，确保显示的是最终值
    const QString readBack = DbStore::getConfig(dbKey, normVal).toString();
    if (m_edits.contains(uiKey) && m_edits[uiKey]) {
        m_edits[uiKey]->setText(readBack);
    }

    appendLog(QString("设置成功: %1 = %2").arg(dbKey, readBack));

    // 更新配置
    DeviceConfigSync::load();

    return true;
}

/** @brief 持久化字段后向外发送配置命令。 */
void CommandDialog::onSendClicked(const QString& key, const QString& val)
{
    QString err;
    if (!applyUiKeyToDb(key, val, &err)) {
        appendLog(QString("设置失败[%1]: %2").arg(key, err));
        return;
    }

    if (key == "fan_area") {
        //保存 sector_num 到 DB
        const QString sector = DbStore::getConfig("sector_num", val).toString().trimmed();

        // 构造同样的 ASCII 命令
        const QByteArray cmd = QString("set sector num=%1\r\n").arg(sector).toLatin1();

        // 通过 IcEventBridge 投递给 IcWorker
        IcEventBridge::instance()->requestCardReaderCommand(cmd,QStringLiteral("command_dialog_set_sector"));

        appendLog(QString("刷卡扇区已保存，准备下发刷卡器: %1").arg(QString::fromLatin1(cmd).trimmed()));
    }

}

/** @brief 转发动作按钮请求。 */
void CommandDialog::onActionClicked(const QString& action)
{
    if (action == "gen_mac") {
        // 生成一个 MAC（示例：固定前3字节，后3字节随机）
        QString mac = "B8:6E:3D";
        for (int i=0;i<3;i++) {
            int b = QRandomGenerator::global()->bounded(0, 256);
            mac += QString(":%1").arg(b, 2, 16, QChar('0')).toUpper();
        }
        if (m_edits.contains("mac_addr") && m_edits["mac_addr"]) {
            m_edits["mac_addr"]->setText(mac);
        }
        // 同步写库
        QString err;
        applyUiKeyToDb("mac_addr", mac, &err);
        return;
    }

    if (action == "gen_device_no") {
        // 读取 MQTT 已有 device_name，其次 client_id，再回退 DB 当前值。
        QString dev;
        QSettings ini(Rk3566Platform::netConfigPath(), QSettings::IniFormat);
        ini.setIniCodec("UTF-8");
        dev = ini.value("mqtt/device_name").toString().trimmed();

        if (dev.isEmpty()) {
            dev = ini.value("mqtt/client_id").toString().trimmed();
        }

        if (dev.isEmpty()) {
            dev = DbStore::getConfig("device_name", "").toString().trimmed();
        }

        if (dev.isEmpty()) {
            appendLog("读取设备号失败：mqtt/device_name 与 mqtt/client_id 均为空。");
            return;
        }

        if (m_edits.contains("device_no") && m_edits["device_no"]) {
            m_edits["device_no"]->setText(dev);
        }

        QString err;
        if (!applyUiKeyToDb("device_no", dev, &err)) {
            appendLog(QString("写入设备号失败: %1").arg(err));
            return;
        }

        appendLog(QString("设备号已从 MQTT 配置读取并写入: %1").arg(dev));
        return;
    }

    if (action == "legacy_key1_query_registered_no") {
        appendLog("KEY1:保留入口");
        return;
    }

    if (action == "legacy_key2_query_board_time") {
        // Core: KEY2 -> sync_flash_information() + clock_out()
        appendLog("KEY2: 查询主板信息和当前时间");
        appendLog(QString("主板信息: device_name=%1, floor_num=%2, register_times=%3")
                  .arg(DbStore::getConfig("device_name", "").toString(),
                       DbStore::getConfig("floor_num", 1).toString(),
                       DbStore::getConfig("register_times", 0).toString()));
        appendLog(QString("密钥信息: card_secret=%1, qrcode_secret=%2")
                  .arg(DbStore::getConfig("card_secret", "").toString(),
                       DbStore::getConfig("qrcode_secret", "").toString()));
        appendLog(QString("current time:%1")
                  .arg(QDateTime::currentDateTime().toString("yyyy/MM/dd HH:mm:ss ddd")));
        return;
    }

    if (action == "legacy_key3_query_registered_cards") {
        // Core: KEY3 -> query_registered_card()
        appendLog("KEY3: 查询注册卡号");
        const QList<QVariantMap> rows =
            DbStore::query("SELECT key AS cfg_key FROM config WHERE key LIKE ? ORDER BY key ASC",
                           {QString("card_allow_%")});

        if (rows.isEmpty()) {
            appendLog("registered card: (empty)");
        } else {
            for (int i = 0; i < rows.size(); ++i) {
                const QString cfgKey = rows[i].value("cfg_key").toString();
                const QString cardId = cfgKey.mid(QString("card_allow_").size());
                appendLog(QString("%1: %2").arg(i).arg(cardId));
            }
        }
        appendLog(QString("registered times:%1").arg(DbStore::getConfig("register_times", 0).toString()));
        return;
    }

    if (action == "legacy_key4_reset") {
        // Core: KEY4 -> HAL_NVIC_SystemReset()
        appendLog("KEY4: 主板复位(映射重启流程)");
        onActionClicked("reboot");
        return;
    }

    if (action == "reboot") {
        appendLog("准备重启系统...");

        DbStore::checkpoint();
        DbStore::close();
        QCoreApplication::processEvents();

        QString error;
        if (!Rk3566Platform::reboot(&error)) {
            appendLog(QStringLiteral("重启失败: %1").arg(error));
        }
        return;
    }

    if (action == "sync_time") {
        appendLog("sync_time: 这里可接 NTP/系统时间同步逻辑");
        return;
    }

    if (action == "insert_qr_api") {
        appendLog("insert_qr_api: 这里可插入二维码接口逻辑");
        return;
    }
}


/** @brief 为输入框接入软键盘并处理对话框内的交互事件。 */
bool CommandDialog::eventFilter(QObject *obj, QEvent *event)
{
    if (event->type() == QEvent::MouseButtonPress)
    {
        QLineEdit *edit = qobject_cast<QLineEdit *>(obj);
        if (edit)
        {
            bool ok = false;

            QString text = KeyboardDialog::getText(
                        this,
                        "输入内容",
                        edit->text(),
                        QLineEdit::Normal,
                        64,
                        &ok);

            if (ok)
            {
                edit->setText(text);
            }

            return true;
        }
    }

    return QDialog::eventFilter(obj, event);
}


/** @brief 应用适配当前屏幕尺寸的控件样式。 */
void CommandDialog::applyStyle()
{
    setStyleSheet(R"(
        QDialog {
            background-color: #F5F5F7;
        }

        QScrollArea#leftScroll,
        QScrollArea#pageScroll,
        QScrollArea {
            background-color: transparent;
            border: none;
        }

        QWidget#headerCard,
        QWidget#sectionCard,
        QWidget#logCard {
            background-color: #FFFFFF;
            border: 1px solid #E8E8EC;
            border-radius: 16px;
        }

        QWidget#formRow {
            background-color: transparent;
            border: none;
        }

        QWidget#inputShell {
            background-color: #F8F8FA;
            border: 1px solid #E6E6EA;
            border-radius: 12px;
        }

        QWidget#inputShell:hover {
            background-color: #FAFAFB;
            border: 1px solid #D6D6DC;
        }

        QLabel#pageTitle {
            color: #111111;
            font-size: 22px;
            font-weight: 700;
        }

        QLabel#sectionTitle,
        QLabel#logTitle {
            color: #111111;
            font-size: 15px;
            font-weight: 700;
        }

        QLabel#lbl {
            color: #1F2937;
            font-size: 13px;
            font-weight: 600;
        }

        QLabel#lblMinor {
            color: #4B5563;
            font-size: 12px;
            font-weight: 600;
        }

        QFrame#rowDivider {
            background-color: #EFEFF2;
            border: none;
            max-height: 1px;
            min-height: 1px;
        }

        QLineEdit#edit {
            background-color: transparent;
            color: #111827;
            border: none;
            padding: 0 2px;
            font-size: 14px;
            font-weight: 500;
            selection-background-color: #D1FAE5;
        }

        QLineEdit#edit:focus {
            background-color: transparent;
            border: none;
        }

        QPushButton {
            min-height: 34px;
            border-radius: 10px;
            font-size: 13px;
            font-weight: 600;
            padding: 0 12px;
        }

        QPushButton#btnPrimary {
            color: #FFFFFF;
            background-color: #111111;
            border: 1px solid #111111;
        }

        QPushButton#btnPrimary:hover {
            background-color: #1F1F1F;
            border: 1px solid #1F1F1F;
        }

        QPushButton#btnPrimary:pressed {
            background-color: #000000;
            border: 1px solid #000000;
        }

        QPushButton#btnAssist {
            color: #111827;
            background-color: #FFFFFF;
            border: 1px solid #D8D8DE;
        }

        QPushButton#btnAssist:hover {
            background-color: #F9FAFB;
            border: 1px solid #BFC3CC;
        }

        QPushButton#btnAssist:pressed {
            background-color: #F3F4F6;
        }

        QPushButton#btnSecondary {
            background-color: #FFFFFF;
            color: #374151;
            border: 1px solid #D8D8DE;
        }

        QPushButton#btnSecondary:hover {
            background-color: #F9FAFB;
            border: 1px solid #BFC3CC;
        }

        QPushButton#btnSecondary:pressed {
            background-color: #F3F4F6;
        }

        QPushButton#btnGhost {
            background-color: #F8F8FA;
            color: #374151;
            border: 1px solid #E5E7EB;
        }

        QPushButton#btnGhost:hover {
            background-color: #FFFFFF;
            border: 1px solid #D1D5DB;
        }

        QPushButton#btnGhost:pressed {
            background-color: #F3F4F6;
        }

        QPushButton#btnDanger {
            background-color: #FFF1F2;
            color: #DC2626;
            border: 1px solid #FECACA;
        }

        QPushButton#btnDanger:hover {
            background-color: #FFE4E6;
            border: 1px solid #FDA4AF;
        }

        QPushButton#btnDanger:pressed {
            background-color: #FECDD3;
        }

        QPushButton#btnDangerGhost {
            background-color: #FFFFFF;
            color: #DC2626;
            border: 1px solid #FECACA;
        }

        QPushButton#btnDangerGhost:hover {
            background-color: #FFF1F2;
            border: 1px solid #FDA4AF;
        }

        QPushButton#btnDangerGhost:pressed {
            background-color: #FFE4E6;
        }

        QPlainTextEdit#logEdit {
            background-color: #F8F8FA;
            color: #111827;
            border: 1px solid #E8E8EC;
            border-radius: 12px;
            padding: 12px;
            font-size: 12px;
        }

        QScrollBar:vertical {
            width: 8px;
            background-color: transparent;
            margin: 2px;
        }

        QScrollBar::handle:vertical {
            background-color: #D1D5DB;
            border-radius: 4px;
            min-height: 36px;
        }

        QScrollBar::handle:vertical:hover {
            background-color: #9CA3AF;
        }

        QScrollBar::add-line:vertical,
        QScrollBar::sub-line:vertical,
        QScrollBar::add-page:vertical,
        QScrollBar::sub-page:vertical {
            background-color: transparent;
            border: none;
            height: 0px;
        }
    )");
}
