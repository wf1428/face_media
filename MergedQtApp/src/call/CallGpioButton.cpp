/**
 * @file CallGpioButton.cpp
 * @brief 实现 GPIO40 输入轮询、60ms 消抖、长按抑制和可用性代次过滤。
 * @author Du lin
 * @date 2026-10-30
 * @note 最近修改：2026-11-03；修改人：Du lin；每次业务状态刷新均推进按键代次，防止拨号/挂断切换后执行旧按下；保留60ms消抖、长按抑制和后台GPIO访问。
 */
#include "CallGpioButton.h"
#include <QDir>
#include <QFile>
#include <QElapsedTimer>

namespace {
constexpr int SampleMs=20; /**< 采样间隔，单位毫秒，只在后台休眠。 */
constexpr int DebounceMs=60; /**< 按下和释放均须保持的最短时间，单位毫秒。 */
constexpr int RetryMs=2000; /**< 配置/读取失败后的重试间隔，单位毫秒。 */

/** @brief 后台写入一个 sysfs 属性，包含打开和完整写入检查。
 * @param[in] path 属性绝对路径，仅用于 GPIO40 的 export/direction/active_low。
 * @param[in] bytes 交给内核的 ASCII 属性值。
 * @param[out] error 失败时包含路径与 Qt 文件错误。
 * @return true 已完整提交；false 打开或写入失败。
 */
bool writeAttribute(const QString &path,const QByteArray &bytes,QString &error)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Unbuffered) || file.write(bytes)!=bytes.size()) {
        error=QStringLiteral("%1: %2").arg(path,file.errorString());return false;
    }
    return true;
}
}

void VoiceCall::CallButtonDebounce::reset()
{
    candidateKnown_=false;stableKnown_=false;armed_=false;
}
quint64 VoiceCall::CallButtonDebounce::sample(bool high,qint64 nowMs,quint64 eligibility)
{
    if (!candidateKnown_ || candidateHigh_!=high) {
        candidateKnown_=true;candidateHigh_=high;
        candidateSinceMs_=nowMs;candidateEligibility_=eligibility;return 0;
    }
    if (nowMs-candidateSinceMs_<DebounceMs || (stableKnown_ && stableHigh_==high)) return 0;
    stableKnown_=true;stableHigh_=high;
    if (!high) { armed_=true;return 0; }
    const bool pressed=armed_;armed_=false;
    return pressed && (candidateEligibility_ & 1) && candidateEligibility_==eligibility ? eligibility : 0;
}

CallGpioButton::CallGpioButton(const QString &sysfsRoot,QObject *parent)
    : QThread(parent),sysfsRoot_(sysfsRoot)
{
    connect(this,&CallGpioButton::pressDetected,this,[this](quint64 token) {
        // 即使前后都允许按键，键盘先呼叫或会话状态改变后也不能将旧按下解释为挂断。
        if ((token & 1) && eligibility_.load()==token && !isInterruptionRequested()) emit callRequested();
    },Qt::QueuedConnection);
}
CallGpioButton::~CallGpioButton() { stopAsync();wait(); }
void CallGpioButton::setCallAvailable(bool available)
{
    const quint64 before=eligibility_.load();
    // 同为可用也推进代次：空闲拨号与会话中挂断是不同动作，排队事件不可跨状态复用。
    eligibility_.store(((before & ~quint64(1))+2) | quint64(available));
}
void CallGpioButton::stopAsync() { setCallAvailable(false);requestInterruption(); }

bool CallGpioButton::openInput(QFile &value,QString &error) const
{
    const QDir root(sysfsRoot_);
    const QString gpio=root.filePath(QStringLiteral("gpio40"));
    if (!QDir(gpio).exists()) {
        // 已由用户导出时直接复用；若与另一导出者竞争，节点已存在也视为成功。
        if (!writeAttribute(root.filePath(QStringLiteral("export")),"40\n",error) && !QDir(gpio).exists()) return false;
    }
    if (!writeAttribute(gpio+QStringLiteral("/direction"),"in\n",error)) return false;
    const QString activeLow=gpio+QStringLiteral("/active_low");
    if (QFile::exists(activeLow) && !writeAttribute(activeLow,"0\n",error)) return false;
    value.setFileName(gpio+QStringLiteral("/value"));
    if (!value.open(QIODevice::ReadOnly | QIODevice::Unbuffered)) {
        error=QStringLiteral("%1: %2").arg(value.fileName(),value.errorString());return false;
    }
    error.clear();return true;
}
void CallGpioButton::run()
{
    QFile value;VoiceCall::CallButtonDebounce debounce;
    QElapsedTimer clock;clock.start();qint64 retryAt=0;QString lastError;
    while (!isInterruptionRequested()) {
        QString error;
        if (!value.isOpen() && clock.elapsed()>=retryAt) {
            if (openInput(value,error)) {
                debounce.reset();lastError.clear();
                emit logMessage(QStringLiteral("[call-gpio] GPIO40 input ready; active-high; debounce=60ms"));
            }
        }
        if (value.isOpen()) {
            QByteArray bytes;
            if (value.seek(0)) bytes=value.read(8).trimmed();
            if (bytes=="0" || bytes=="1") {
                const quint64 token=debounce.sample(bytes=="1",clock.elapsed(),eligibility_.load());
                if (token) emit pressDetected(token);
            } else {
                error=QStringLiteral("%1: invalid/read-failed value (%2)").arg(value.fileName(),value.errorString());
            }
        }
        if (!error.isEmpty()) {
            value.close();debounce.reset();retryAt=clock.elapsed()+RetryMs;
            if (lastError!=error) { lastError=error;emit logMessage(QStringLiteral("[call-gpio] %1; retry in 2000ms").arg(error)); }
        }
        QThread::msleep(SampleMs);
    }
    value.close();
}
