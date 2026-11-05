/**
 * @file CallPanel.cpp
 * @brief 实现非模态通话状态、电平图及异步挂断操作。
 * @author Du lin
 * @date 2026-10-29
 * @note 最近修改：2026-10-30；修改人：Du lin；移除面板空闲呼叫和手动接听按钮，平台来电自动连接时只显示状态、电平与挂断；空闲隐藏覆盖层。
 */
#include "CallPanel.h"
#include "CallManager.h"
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QEvent>
#include <QPainter>
#include <QPaintEvent>
#include <array>
#include <cmath>

/** @brief 接收 PCM 均方根幅度的滚动图；UI 所有，固定 60 点、100ms 采样，保留约 6 秒。
 * @note 只画经过校验的接收电平，与扬声器音量无关；不持有或操作音频设备。
 */
class ReceiveAudioPlot final : public QWidget {
public:
    /** @brief 创建无音频资源的绘图子控件。
     * @param[in] parent Qt 所有者，控件随通话面板销毁。
     */
    explicit ReceiveAudioPlot(QWidget *parent):QWidget(parent) {
        setObjectName(QStringLiteral("callReceiveAudioPlot"));setMinimumHeight(76);setMinimumWidth(270);
    }
    /** @brief 清空图表，防止下一通话显示旧电平。 */
    void clear() { levels_.fill(0);cursor_=0;update(); }
    /** @brief 添加一次电平观察；没有新鲜音频时补零。
     * @param[in] level 已校验下行电平，最近帧超过 250ms 视为中断。
     */
    void append(const VoiceCall::ReceivedAudioLevel &level) {
        float value=0;
        if (level.frames && level.ageMs>=0 && level.ageMs<=250 && level.rms>0) {
            const double db=20*std::log10(double(level.rms)/32768);
            value=float(qBound(0.0,(db+60)/60,1.0));
        }
        levels_[cursor_]=value;cursor_=(cursor_+1)%levels_.size();update();
    }
protected:
    /** @brief 绘制固定数量的网格和幅度柱；不在绘图时获取音频队列锁。
     * @param[in] event Qt 绘制事件，借用且不释放。
     */
    void paintEvent(QPaintEvent *event) override {
        Q_UNUSED(event);
        QPainter painter(this);painter.setRenderHint(QPainter::Antialiasing);
        painter.fillRect(rect(),QColor("#0f2738"));
        auto font=painter.font();font.setPixelSize(11);painter.setFont(font);
        painter.setPen(QColor("#afc4d5"));painter.drawText(8,15,QStringLiteral("接收音频幅度 · 最近 6 秒 / dBFS"));
        const QRectF area(8,24,qMax(1,width()-52),qMax(1,height()-32));
        for (int i=0;i<3;++i) {
            const qreal y=area.top()+area.height()*i/2;
            painter.setPen(QColor("#29485d"));painter.drawLine(QPointF(area.left(),y),QPointF(area.right(),y));
            painter.setPen(QColor("#91aabd"));painter.drawText(QPointF(area.right()+5,y+4),QString::number(-30*i));
        }
        const qreal step=area.width()/levels_.size();
        for (size_t i=0;i<levels_.size();++i) {
            const float value=levels_[(cursor_+i)%levels_.size()];
            const qreal height=value*area.height();
            painter.fillRect(QRectF(area.left()+i*step,area.bottom()-height,qMax(1.0,step-1),height),
                             value>0.95f ? QColor("#ffb578") : QColor("#60d9ca"));
        }
    }
private:
    std::array<float,60> levels_{}; /**< 显示用 -60..0dB 电平映射，0 是图底；没有 PCM 正文。 */
    size_t cursor_=0; /**< 下次写槽，按 60 回绕，最新观察位于图右侧。 */
};

CallPanel::CallPanel(CallManager *manager,QWidget *parent):QWidget(parent),manager_(manager)
{
    setObjectName(QStringLiteral("voiceCallPanel"));
    setAttribute(Qt::WA_StyledBackground,true);
    setStyleSheet(QStringLiteral("#voiceCallPanel { background: #16354a; border: 1px solid #5b859d; border-radius: 10px; }"
        "#voiceCallPanel QLabel { color: white; } #voiceCallPanel QPushButton { min-height: 38px; padding: 2px 12px; }"));
    auto *layout=new QVBoxLayout(this);
    label_=new QLabel(QStringLiteral("语音通话未连接"),this); layout->addWidget(label_);
    plot_=new ReceiveAudioPlot(this);layout->addWidget(plot_);
    meterLabel_=new QLabel(QStringLiteral("连接后显示接收幅度"),this);
    meterLabel_->setObjectName(QStringLiteral("callReceiveAudioLevel"));layout->addWidget(meterLabel_);
    meterTimer_.setInterval(100);
    connect(&meterTimer_,&QTimer::timeout,this,&CallPanel::refreshMeter);
    auto *buttons=new QHBoxLayout;
    hangup_=new QPushButton(QStringLiteral("挂断"),this);
    hangup_->setObjectName(QStringLiteral("callHangup"));
    buttons->addWidget(hangup_);
    layout->addLayout(buttons);
    connect(hangup_,&QPushButton::clicked,manager_.data(),&CallManager::hangup);
    connect(manager_.data(),&CallManager::changed,this,[this](const QString &text) { label_->setText(text); refresh(); });
    connect(manager_.data(),&QObject::destroyed,this,[this]() { meterTimer_.stop();hide(); });
    setFixedWidth(330);
    parent->installEventFilter(this); refresh();
}
bool CallPanel::eventFilter(QObject *watched,QEvent *event) {
    if (watched==parentWidget() && event->type()==QEvent::Resize) refresh();
    return QWidget::eventFilter(watched,event);
}
void CallPanel::refresh() {
    if (!manager_) { meterTimer_.stop();hide();return; }
    const auto state=manager_->state();
    const bool idle=state==CallManager::Idle;
    setVisible(!idle);
    hangup_->setVisible(!idle); hangup_->setEnabled(state!=CallManager::Ending);
    plot_->setVisible(!idle);meterLabel_->setVisible(!idle);
    if (state==CallManager::Talking) {
        if (!meterTimer_.isActive()) { plot_->clear();refreshMeter();meterTimer_.start(); }
    } else {
        meterTimer_.stop();
        if (state!=CallManager::Ending) { plot_->clear();meterLabel_->setText(QStringLiteral("连接后显示接收幅度")); }
    }
    adjustSize();
    if (parentWidget()) move(qMax(0,parentWidget()->width()-width()-16),16);
    if (isVisible()) raise();
}

void CallPanel::refreshMeter()
{
    if (!manager_ || manager_->state()!=CallManager::Talking) return;
    const auto level=manager_->receivedAudioLevel();plot_->append(level);
    QString text,style;
    if (!level.frames || level.ageMs<0) {
        text=QStringLiteral("等待有效下行音频…");style=QStringLiteral("color:#a9becf;");
    } else if (level.ageMs>250) {
        text=QStringLiteral("音频已中断 · %1 ms 未收到").arg(level.ageMs);style=QStringLiteral("color:#ffcc86;");
    } else if (level.rms<=1) {
        text=QStringLiteral("已收到音频 · 当前接近静音");style=QStringLiteral("color:#ffcc86;");
    } else {
        const double db=20*std::log10(double(level.rms)/32768);
        text=QStringLiteral("已收到音频 · 峰值 %1% · %2 dBFS")
            .arg(double(level.peak)*100/32768,0,'f',1).arg(db,0,'f',0);
        style=QStringLiteral("color:#70e1ce;");
    }
    meterLabel_->setText(text);
    if (meterLabel_->styleSheet()!=style) meterLabel_->setStyleSheet(style);
}
