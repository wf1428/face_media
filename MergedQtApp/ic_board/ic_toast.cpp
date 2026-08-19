/**
 * @file ic_toast.cpp
 * @brief 实现 IC 结果覆盖提示、边缘呼吸动画和提示音焦点管理。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#include "ic_toast.h"
#include "components/multimedia/vol_ctrl.h"
#include "components/multimedia/gst_player_widget.h"

#include <QApplication>
#include <QPainter>
#include <QFontMetrics>
#include <QScreen>
#include <QColor>
#include <QtMath>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QDebug>
#include <QSettings>
#include <QStandardPaths>
#include "platform/rk3566_platform.h"

IcToast* IcToast::self_ = nullptr;


/**
 * @brief 初始化透明覆盖层、动画定时器和专用提示音播放器。
 * @param mainWindow 覆盖层所属的主窗口。
 */
IcToast::IcToast(QWidget* mainWindow)
    : QWidget(mainWindow), main_(mainWindow)
{
    // 作为主窗口的子控件，透明/圆角由Qt在同一窗口内合成，不依赖系统compositor
    setAttribute(Qt::WA_TransparentForMouseEvents, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAutoFillBackground(false);

    baseFont_ = QApplication::font();

    hideTimer_.setSingleShot(true);
    connect(&hideTimer_, &QTimer::timeout, this, [this]() {
        passAnimTimer_.stop();
        passFlashStrength_ = 0.0;
        edgeTheme_ = EdgeNone;
        kind_ = Neutral;
        text_.clear();
        boxRect_ = QRect();
        hide();
    });

    passAnimTimer_.setInterval(16); // 约 60fps，呼吸过渡更平滑
    connect(&passAnimTimer_, &QTimer::timeout, this, &IcToast::updatePassEdgeFlash);

    if (main_) main_->installEventFilter(this);

    // 覆盖主窗口客户区
    setGeometry(main_->rect());
    hide();

    voiceFocusRestoreTimer_.setSingleShot(true);
    connect(&voiceFocusRestoreTimer_, &QTimer::timeout,
            this, &IcToast::endVoiceFocus);

    voicePlayer_ = new GstPlayerWidget(this);
    voicePlayer_->setVideoOutput(nullptr);
    voicePlayer_->setVolume(1.0f);
    connect(voicePlayer_, &GstPlayerWidget::videoFinished,
            this, &IcToast::endVoiceFocus);
    connect(voicePlayer_, &GstPlayerWidget::errorOccured, this, [this](const QString &error) {
        qWarning() << "[IC-TOAST] GStreamer voice playback failed:" << error;
        endVoiceFocus();
    });

    // 提前把 qrc 里的语音文件落到临时目录，交给 GStreamer playbin 播放
    passVoiceFile_ = materializeResourceToTemp(":/static/audio/pass.wav");
    failVoiceFile_ = materializeResourceToTemp(":/static/audio/fail.wav");
}

/**
 * @brief 返回单例，并在宿主变化时重新挂接覆盖层。
 * @param mainWindow 新的宿主主窗口。
 * @return 进程内唯一 IcToast 实例。
 */
IcToast* IcToast::instance(QWidget* mainWindow) {
    if (!self_) {
        self_ = new IcToast(mainWindow);
    } else if (self_->main_ != mainWindow) {
        self_->setParent(mainWindow);
        self_->main_ = mainWindow;
        mainWindow->installEventFilter(self_);
        self_->setGeometry(mainWindow->rect());
    }
    return self_;
}


/**
 * @brief 对成功和失败提示分别执行冷却节流。
 * @return 首次触发或距同类上次触发达到 2 秒时返回 true。
 */
bool IcToast::allowTriggerNow(bool isFail)
{
    QElapsedTimer &clock = isFail ? failTriggerClock_ : passTriggerClock_;

    if (!clock.isValid()) {
        clock.start();
        return true;
    }

    if (clock.elapsed() < triggerCooldownMs_) {
        qDebug() << "[IC-TOAST] throttled"
                << "type=" << (isFail ? "fail" : "pass")
                << "elapsed=" << clock.elapsed()
                << "cooldown=" << triggerCooldownMs_;
        return false;
    }

    clock.restart();
    return true;
}

/** @return 外置静态资源优先、qrc 资源回退的提示配置路径。 */
QString IcToast::resolveToastConfigPath() const
{
    return Rk3566Platform::uiConfigPath();
}

/**
 * @brief 解析兼容历史写法的 INI 布尔值。
 *
 * 先去除分号后的行内注释，再接受 0/1、true/false、yes/no 或其他整数。
 */
bool IcToast::parseIniBool(const QVariant& v, bool def)
{
    if (!v.isValid()) return def;

    QString s = v.toString().trimmed();
    const int commentPos = s.indexOf(';');
    if (commentPos >= 0) {
        s = s.left(commentPos).trimmed();
    }

    if (s.isEmpty()) return def;

    if (s == "1") return true;
    if (s == "0") return false;

    if (s.compare("true", Qt::CaseInsensitive) == 0) return true;
    if (s.compare("false", Qt::CaseInsensitive) == 0) return false;
    if (s.compare("yes", Qt::CaseInsensitive) == 0) return true;
    if (s.compare("no", Qt::CaseInsensitive) == 0) return false;

    bool ok = false;
    const int n = s.toInt(&ok);
    if (ok) return n != 0;

    return def;
}

/** @return MusicPlayEn 配置明确启用时返回 true；缺少配置时默认关闭语音。 */
bool IcToast::isVoicePromptEnabled() const
{
    const QString configPath = resolveToastConfigPath();
    if (configPath.isEmpty()) {
        qWarning() << "[IC-TOAST] config not found, voice disabled by default";
        return false;
    }

    QSettings settings(configPath, QSettings::IniFormat);

#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
    settings.setIniCodec("UTF-8");
#endif

    const QVariant v = settings.value("DisplayDirRect/MusicPlayEn", 0);
    const bool enabled = parseIniBool(v, false);

    qInfo() << "[IC-TOAST] voice config path=" << configPath
            << "MusicPlayEn=" << v.toString()
            << "enabled=" << enabled;

    return enabled;
}


/**
 * @brief 显示绿色全屏边缘呼吸效果，并尝试播放通过提示音。
 * @param ms 调用方期望的显示时长；边缘动画周期由内部固定参数控制。
 */
void IcToast::showPass(int ms)
{

    if (!allowTriggerNow(false)) {
        return;
    }

    // 成功提示要显式切到绿色边框主题
    edgeTheme_ = EdgeSuccessGreen;
    kind_ = PassEdgeFlash;
    text_.clear();
    boxRect_ = QRect();

    passAnimTimer_.stop();
    passFlashStrength_ = 0.0;

    if (main_) {
        setGeometry(main_->rect());
    }

    playPassVoice();

    hideTimer_.stop();
    raise();
    show();

    startPassEdgeFlash();
    update();
}

/**
 * @brief 显示本地化失败文本和红色边缘动画，并尝试播放失败提示音。
 * @param reason 协议失败原因码或已本地化文本。
 * @param ms 文字提示自动隐藏时间，单位 ms。
 */
void IcToast::showFail(const QString& reason, int ms)
{
    qWarning().noquote()
            << QString("[IC-TOAST] showFail enter reason=%1 ms=%2")
                  .arg(reason)
                  .arg(ms);

    if (!allowTriggerNow(true)) {
        return;
    }

    edgeTheme_ = EdgeFailRed;
    kind_ = Fail;

    const QString r = localizeFailReason(reason);

    // 先显示失败文字条
    showText(QStringLiteral("%1").arg(r), ms);

    // 再播失败语音
    playFailVoice();

    // 红框走和绿框一样的脉冲逻辑
    startPassEdgeFlash();
    update();
}

/**
 * @brief 按文本尺寸创建居中提示框并启动自动隐藏计时。
 * @param text 显示内容。
 * @param ms 自动隐藏时间，单位 ms。
 */
void IcToast::showText(const QString& text, int ms)
{
    // 只有普通文本时才清动画/清边框
    if (kind_ != Fail) {
        passAnimTimer_.stop();
        passFlashStrength_ = 0.0;
        edgeTheme_ = EdgeNone;
    }

    text_ = text;

    if (main_) {
        setGeometry(main_->rect());
    }

    resizeToFitText();
    moveBoxToScreenCenter();

    raise();
    show();
    armHide(ms);
    update();
}

/**
 * @brief 重启自动隐藏单次定时器。
 * @param ms 距离隐藏的时间，单位 ms。
 */
void IcToast::armHide(int ms) {
    hideTimer_.stop();
    hideTimer_.start(ms);
}


/**
 * @brief 启动固定周期数的边缘呼吸动画。
 */
void IcToast::startPassEdgeFlash()
{
    // 每个周期 2000ms
    passFlashTotalMs_ = passPulsePeriodMs_ * qMax(1, passPulseCount_);
    passFlashStrength_ = 0.3;
    passAnimClock_.restart();
    passAnimTimer_.start();
}

/**
 * @brief 根据动画相位计算 0.3~1.0 的平滑亮度并触发重绘。
 */
void IcToast::updatePassEdgeFlash() {
    const int elapsed = int(passAnimClock_.elapsed());

    if (elapsed >= passFlashTotalMs_) {
        passAnimTimer_.stop();
        passFlashStrength_ = 0.0;
        if (kind_ == PassEdgeFlash) {
            hide();
        } else {
            update();
        }
        return;
    }

    const qreal phase = qreal(elapsed % passPulsePeriodMs_) / qMax(1, passPulsePeriodMs_);
    const qreal easeInOut = 0.5 - 0.5 * qCos(2.0 * M_PI * phase); // 0~1~0
    passFlashStrength_ = 0.3 + 0.7 * easeInOut;

    update();
}

/**
 * @brief 绘制四边线性渐变、四角径向光晕及内侧高光。
 * @param p 当前窗口绘制器。
 */
void IcToast::paintPassEdgeFlash(QPainter& p) {
    const qreal s = qBound<qreal>(0.0, passFlashStrength_, 1.0);
    if (s <= 0.001) return;

    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(Qt::NoPen);

    const qreal w = width();
    const qreal h = height();
    if (w <= 2.0 || h <= 2.0) return;

    const qreal ringWidth = qMin<qreal>(qMin(w, h) * 0.45, qMax(8.0, qreal(passRingWidthPx_)));
    const qreal haloWidth = ringWidth * 1.8;

    const QColor brightBase(QStringLiteral("#00FF88"));
    const QColor midBase = (edgeTheme_ == EdgeFailRed)
            ? QColor(QStringLiteral("#AA2222"))
            : QColor(QStringLiteral("#00AA44"));
    const QColor themedBrightBase = (edgeTheme_ == EdgeFailRed)
            ? QColor(QStringLiteral("#FF5A5A"))
            : brightBase;
    auto withAlpha = [](const QColor& c, int alpha) {
        QColor t = c;
        t.setAlpha(qBound(0, alpha, 255));
        return t;
    };

    const QColor bright = withAlpha(themedBrightBase, int(255.0 * s));
    const QColor mid = withAlpha(midBase, int(220.0 * s));
    const QColor soft = withAlpha(midBase, int(140.0 * s));
    const QColor halo = withAlpha(themedBrightBase, int(120.0 * s));

    auto drawEdgeHalo = [&](const QRectF& r, bool vertical, bool reverse) {
        QLinearGradient g;
        if (vertical) {
            g = reverse ? QLinearGradient(r.bottomLeft(), r.topLeft())
                        : QLinearGradient(r.topLeft(), r.bottomLeft());
        } else {
            g = reverse ? QLinearGradient(r.topRight(), r.topLeft())
                        : QLinearGradient(r.topLeft(), r.topRight());
        }
        g.setColorAt(0.00, halo);
        g.setColorAt(0.45, soft);
        g.setColorAt(1.00, QColor(0, 0, 0, 0));
        p.setBrush(g);
        p.drawRect(r);
    };

    auto drawEdgeRing = [&](const QRectF& r, bool vertical, bool reverse) {
        QLinearGradient g;
        if (vertical) {
            g = reverse ? QLinearGradient(r.bottomLeft(), r.topLeft())
                        : QLinearGradient(r.topLeft(), r.bottomLeft());
        } else {
            g = reverse ? QLinearGradient(r.topRight(), r.topLeft())
                        : QLinearGradient(r.topLeft(), r.topRight());
        }
        g.setColorAt(0.00, bright);
        g.setColorAt(0.55, mid);
        g.setColorAt(1.00, QColor(0, 0, 0, 0));
        p.setBrush(g);
        p.drawRect(r);
    };

    const QRectF topHalo(0.0, 0.0, w, haloWidth);
    const QRectF bottomHalo(0.0, h - haloWidth, w, haloWidth);
    const QRectF leftHalo(0.0, 0.0, haloWidth, h);
    const QRectF rightHalo(w - haloWidth, 0.0, haloWidth, h);

    const QRectF topRing(0.0, 0.0, w, ringWidth);
    const QRectF bottomRing(0.0, h - ringWidth, w, ringWidth);
    const QRectF leftRing(0.0, 0.0, ringWidth, h);
    const QRectF rightRing(w - ringWidth, 0.0, ringWidth, h);

    drawEdgeHalo(topHalo, true, false);
    drawEdgeHalo(bottomHalo, true, true);
    drawEdgeHalo(leftHalo, false, false);
    drawEdgeHalo(rightHalo, false, true);

    drawEdgeRing(topRing, true, false);
    drawEdgeRing(bottomRing, true, true);
    drawEdgeRing(leftRing, false, false);
    drawEdgeRing(rightRing, false, true);

    // 四角叠加径向渐变，让边框过渡更柔和
    auto drawCornerHalo = [&](const QPointF& center) {
        QRadialGradient rg(center, haloWidth);
        rg.setColorAt(0.00, withAlpha(themedBrightBase, int(150.0 * s)));
        rg.setColorAt(0.38, withAlpha(midBase, int(115.0 * s)));
        rg.setColorAt(1.00, QColor(0, 0, 0, 0));
        p.setBrush(rg);
        p.drawEllipse(QRectF(center.x() - haloWidth, center.y() - haloWidth,
                             2.0 * haloWidth, 2.0 * haloWidth));
    };

    drawCornerHalo(QPointF(0.0, 0.0));
    drawCornerHalo(QPointF(w, 0.0));
    drawCornerHalo(QPointF(0.0, h));
    drawCornerHalo(QPointF(w, h));

    // 内发光：在中心内容区边沿加一层轻微高光
    QRectF innerRect(ringWidth, ringWidth, w - 2.0 * ringWidth, h - 2.0 * ringWidth);
    if (innerRect.width() > 2.0 && innerRect.height() > 2.0) {
        const qreal innerGlowDepth = ringWidth * 0.55;
        auto drawInnerGlow = [&](const QRectF& r, bool vertical, bool reverse) {
            QLinearGradient g;
            if (vertical) {
                g = reverse ? QLinearGradient(r.bottomLeft(), r.topLeft())
                            : QLinearGradient(r.topLeft(), r.bottomLeft());
            } else {
                g = reverse ? QLinearGradient(r.topRight(), r.topLeft())
                            : QLinearGradient(r.topLeft(), r.topRight());
            }
            g.setColorAt(0.00, withAlpha(themedBrightBase, int(95.0 * s)));
            g.setColorAt(1.00, QColor(0, 0, 0, 0));
            p.setBrush(g);
            p.setPen(Qt::NoPen);
            p.drawRect(r);
        };

        drawInnerGlow(QRectF(innerRect.left(), innerRect.top(), innerRect.width(), innerGlowDepth), true, false);
        drawInnerGlow(QRectF(innerRect.left(), innerRect.bottom() - innerGlowDepth, innerRect.width(), innerGlowDepth), true, true);
        drawInnerGlow(QRectF(innerRect.left(), innerRect.top(), innerGlowDepth, innerRect.height()), false, false);
        drawInnerGlow(QRectF(innerRect.right() - innerGlowDepth, innerRect.top(), innerGlowDepth, innerRect.height()), false, true);
    }
}


/**
 * @brief 跟随宿主窗口的尺寸、位置和窗口状态变化调整覆盖层。
 * @return 返回基类过滤结果，本类不吞掉宿主事件。
 */
bool IcToast::eventFilter(QObject* watched, QEvent* event) {
    if (watched == main_) {
        // 主窗口尺寸变化时，overlay跟随
        if (event->type() == QEvent::Resize || event->type() == QEvent::Move ||
            event->type() == QEvent::WindowStateChange) {
            if (main_) setGeometry(main_->rect());
            moveBoxToScreenCenter();
            update();
        }
    }
    return QWidget::eventFilter(watched, event);
}

/**
 * @brief 使用加粗放大字体计算当前文本框尺寸，并限制最大排版宽度。
 */
void IcToast::resizeToFitText() {
    QFont f = baseFont_;
    f.setPointSize(f.pointSize() + 8);
    f.setWeight(QFont::Bold);
    setFont(f);

    QFontMetrics fm(f);

    int w = qMin(maxWidth_, width() - 40);
    if (w < 200) w = 200;

    QRect br = fm.boundingRect(QRect(0, 0, w - padX_ * 2, 1000),
                               Qt::TextWordWrap, text_);
    int newW = br.width() + padX_ * 2;
    int newH = br.height() + padY_ * 2;
    if (newH < 128) newH = 128;

    boxRect_.setSize(QSize(newW, newH));
}

/**
 * @brief 将主屏幕中心映射到宿主局部坐标并居中文本框。
 */
void IcToast::moveBoxToScreenCenter() {
    if (!main_) return;

    // 1) 拿屏幕中心
    QScreen* s = QApplication::primaryScreen();
    if (!s) return;
    QPoint screenCenter = s->geometry().center();

    // 2) 把屏幕中心映射到主窗口坐标系
    QPoint mainTopLeftGlobal = main_->mapToGlobal(QPoint(0,0));
    QPoint centerInMain = screenCenter - mainTopLeftGlobal;

    // 3) box在overlay中居中到这个点
    QRect r = boxRect_;
    r.moveCenter(centerInMain);
    boxRect_ = r;
}

/**
 * @brief 绘制边缘主题；非纯成功提示时再绘制圆角文字框。
 */
void IcToast::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    if (edgeTheme_ != EdgeNone) {
        paintPassEdgeFlash(p);
    }

    // 成功：四周绿色柔和脉冲，不显示文字
    if (kind_ == PassEdgeFlash) {
        return;
    }

    // 其余情况沿用文字 toast
    if (text_.isEmpty() || boxRect_.isEmpty()) return;

    const qreal radius = 22.0;

    QColor bg;
    if (kind_ == Fail) {
        bg = QColor(220, 38, 38, 220);
    } else {
        bg = QColor(35, 35, 38, 230);
    }

    p.setPen(Qt::NoPen);
    p.setBrush(bg);
    p.drawRoundedRect(QRectF(boxRect_), radius, radius);

    QLinearGradient g(boxRect_.topLeft(), boxRect_.bottomLeft());
    g.setColorAt(0.0, QColor(255,255,255,22));
    g.setColorAt(0.5, QColor(255,255,255,10));
    g.setColorAt(1.0, QColor(0,0,0,18));
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(QBrush(g), 1.2));
    p.drawRoundedRect(QRectF(boxRect_).adjusted(0.6,0.6,-0.6,-0.6), radius, radius);

    p.setPen(QColor(255,255,255,240));
    p.drawText(boxRect_.adjusted(padX_, padY_, -padX_, -padY_),
               Qt::AlignCenter | Qt::TextWordWrap,
               text_);
}

/**
 * @brief 将 qrc 音频资源复制到临时目录供 GStreamer playbin 读取。
 *
 * 已存在且大小相同的临时文件直接复用，避免每次提示重复写盘。
 */
QString IcToast::materializeResourceToTemp(const QString& resourcePath)
{
    QFile in(resourcePath);
    if (!in.exists()) {
        qWarning() << "[IC-TOAST] voice resource not found:" << resourcePath;
        return QString();
    }
    if (!in.open(QIODevice::ReadOnly)) {
        qWarning() << "[IC-TOAST] open voice resource failed:" << resourcePath << in.errorString();
        return QString();
    }

    const QByteArray data = in.readAll();
    in.close();

    QString tempDir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    if (tempDir.trimmed().isEmpty()) {
        tempDir = "/tmp";
    }
    tempDir += "/voice";
    QDir().mkpath(tempDir);

    const QString outPath = tempDir + "/" + QFileInfo(resourcePath).fileName();

    QFile out(outPath);
    if (out.exists() && out.size() == data.size()) {
        return outPath;
    }

    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qWarning() << "[IC-TOAST] open temp voice file failed:" << outPath << out.errorString();
        return QString();
    }

    if (out.write(data) != data.size()) {
        qWarning() << "[IC-TOAST] write temp voice file failed:" << outPath << out.errorString();
        out.close();
        return QString();
    }
    out.close();

    return outPath;
}

/** @brief 延迟创建功放控制器，并在持久化音量大于 0 时恢复该音量。 */
void IcToast::ensureAmpReady()
{
    if (!ampCtrl_) {
        ampCtrl_ = new AmpVolumeController(this);
    }

    const int volume = ampCtrl_->loadVolumeFromFile();
    if (volume > 0) {
        ampCtrl_->setVolume(volume);
    }
}

/** @brief 检查路径、配置和播放器后播放提示音，并建立 2.2 秒语音焦点兜底。 */
void IcToast::playVoiceFile(const QString& localFilePath)
{
    if (localFilePath.trimmed().isEmpty()) {
        qWarning() << "[IC-TOAST] local voice file path empty";
        return;
    }

    if (!isVoicePromptEnabled()) {
        qInfo() << "[IC-TOAST] voice disabled by config";
        return;
    }
    if (!voicePlayer_) {
        qWarning() << "[IC-TOAST] GStreamer voice player unavailable";
        return;
    }

    ensureAmpReady();
    beginVoiceFocus();

    voicePlayer_->stop();
    voicePlayer_->setVideoOutput(nullptr);
    voicePlayer_->setVolume(1.0f);
    voicePlayer_->setMedia(localFilePath);
    voicePlayer_->play();

    qInfo() << "[IC-TOAST] GStreamer play voice:" << localFilePath;
    voiceFocusRestoreTimer_.start(2200);
}

/** @brief 播放已物化的通过提示音。 */
void IcToast::playPassVoice()
{
    playVoiceFile(passVoiceFile_);
}

/** @brief 播放已物化的失败提示音。 */
void IcToast::playFailVoice()
{
    playVoiceFile(failVoiceFile_);
}

/** @return 基础 DAC 43 加 7，且不超过 55。 */
int IcToast::boostedToastDac() const
{
    const int baseDac = 43;   // 固定初始 DAC
    const int boostStep = 7;  // 在 43 基础上再提升
    return qMin(55, baseDac + boostStep);
}

/**
 * @brief 临时压低宿主中的业务播放器并提升功放音量。
 *
 * 重复调用时不覆盖最初保存值，只延长兜底恢复定时器。
 */
void IcToast::beginVoiceFocus()
{
    ensureAmpReady();
    voiceFocusRestoreTimer_.stop();

    if (voiceFocusActive_) {
        voiceFocusRestoreTimer_.start(2200);
        return;
    }

    voiceFocusActive_ = true;
    duckedPlayers_.clear();

    // 1) 先压低视频音量
    if (main_) {
        const auto players = main_->findChildren<GstPlayerWidget*>();
        for (GstPlayerWidget* p : players) {
            if (!p || p == voicePlayer_) continue;

            DuckedPlayerState st;
            st.player = p;
            st.volume = p->volume();
            duckedPlayers_.push_back(st);

            // 压到 12%，提示音更容易听见
            const float duckV = qMin(st.volume, 0.12f);
            p->setVolume(duckV);
        }
    }

    // 2) 功放音量临时拉高一点
    if (ampCtrl_) {
        savedAmpDac_ = ampCtrl_->currentRawDac();
        ampCtrl_->setRawDacTemporary(boostedToastDac());
    }

    // 3) 兜底恢复
    voiceFocusRestoreTimer_.start(2200);
}

/** @brief 恢复语音焦点前保存的播放器音量和功放 DAC。 */
void IcToast::endVoiceFocus()
{
    voiceFocusRestoreTimer_.stop();

    // 恢复视频音量
    for (const DuckedPlayerState& st : duckedPlayers_) {
        if (!st.player) continue;
        st.player->setVolume(st.volume);
    }
    duckedPlayers_.clear();

    // 恢复功放音量
    if (ampCtrl_ && savedAmpDac_ >= 0) {
        ampCtrl_->setRawDacTemporary(savedAmpDac_);
    }

    savedAmpDac_ = -1;
    voiceFocusActive_ = false;
}

/** @return 文本包含 U+4E00~U+9FFF 汉字时返回 true。 */
bool IcToast::looksLikeChinese(const QString& text) const
{
    for (const QChar& ch : text) {
        const ushort u = ch.unicode();
        if (u >= 0x4E00 && u <= 0x9FFF) {
            return true;
        }
    }
    return false;
}

/** @return 已知协议原因码对应的中文提示；未知文本原样返回以便诊断。 */
QString IcToast::localizeFailReason(const QString& reason) const
{
    const QString raw = reason.trimmed();
    if (raw.isEmpty()) {
        return QStringLiteral("未知原因");
    }

    // 已经是中文，直接返回
    if (looksLikeChinese(raw)) {
        // 对现有中文原因做一点统一优化
        if (raw.startsWith(QStringLiteral("二维码过期"))) {
            return QStringLiteral("二维码已过期");
        }
        if (raw.startsWith(QStringLiteral("二维码更新时间解析失败"))) {
            return QStringLiteral("时间二维码解析失败");
        }
        if (raw == QStringLiteral("楼层二维码通过") ||
            raw.startsWith(QStringLiteral("时间二维码解析通过"))) {
            return QStringLiteral("验证通过");
        }
        return raw;
    }

    static const QHash<QString, QString> kReasonMap = {
        { QStringLiteral("frame_invalid"),            QStringLiteral("卡片或二维码格式无效") },
        { QStringLiteral("frame_len_unsupported"),    QStringLiteral("卡片数据长度不支持") },
        { QStringLiteral("card_id_invalid"),          QStringLiteral("卡号无效") },
        { QStringLiteral("visitor_time_parse_fail"),  QStringLiteral("访客卡有效期解析失败") },
        { QStringLiteral("multi_time_parse_fail"),    QStringLiteral("多层卡时间段解析失败") },
        { QStringLiteral("time_deny"),                QStringLiteral("当前时间段无权限") },
        { QStringLiteral("card_unregistered"),        QStringLiteral("卡片未注册") },
        { QStringLiteral("secret_fail"),              QStringLiteral("密钥校验失败") },
        { QStringLiteral("floor_deny"),               QStringLiteral("当前楼层无权限") },
        { QStringLiteral("expired"),                  QStringLiteral("卡片已过期") },
        { QStringLiteral("no_times"),                 QStringLiteral("卡片次数已用完") },
        { QStringLiteral("datetime_invalid"),         QStringLiteral("时间数据无效") },
        { QStringLiteral("pass"),                     QStringLiteral("验证通过") },
        { QStringLiteral("ok"),                       QStringLiteral("验证通过") },
        { QStringLiteral("unknown"),                  QStringLiteral("未知原因") }
    };

    const auto it = kReasonMap.constFind(raw);
    if (it != kReasonMap.constEnd()) {
        return it.value();
    }

    // 兜底：未知英文码，原样返回，便于继续排查
    return raw;
}
