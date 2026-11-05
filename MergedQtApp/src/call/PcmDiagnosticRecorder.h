/**
 * @file PcmDiagnosticRecorder.h
 * @brief 在诊断线程按需保存有限时长的输入/中间/最终/参考PCM，不参与通话播放或上传。
 * @author Du lin
 * @date 2026-11-04
 */
#pragma once
#include "PcmDiagnostics.h"
#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QString>
#include <array>
#include <memory>

namespace VoiceCall {
/** @brief 采集线程发布的可选PCM副本；通过固定SPSC交接，丢诊断不能阻塞音频。 */
struct PcmDiagnosticFrame {
    uint64_t index=0;int64_t captureUs=0;unsigned valid=0; /**< 本地采集序号、首采样单调微秒、bit0原始/bit1AEC/bit2PP/bit3最终/bit4对齐参考。 */
    Pcm raw{},aec{},preprocess{},output{},reference{}; /**< 五条同帧mono320点；无效阶段不能当作真实静音，须读取valid。 */
    ProcessingObservation observation; /**< 与PCM同帧的只读幅度/AGC证据。 */
};
/** @brief 仅诊断线程使用的有限录音；调用者显式创建才启用。
 * @details 最多保留100帧前史，首次原始峰值>=12000或手动触发后保存最多500帧合计10秒，
 * 仅写给定独立目录；不覆盖既有目录，不改DSP、不等待UI、不发送录音到MQTT。
 * 文件包含实际语音，marker默认不存在，生产默认不录音。
 */
class PcmDiagnosticRecorder {
public:
    /** @brief 构造有限历史，尚不创建文件。
     * @param[in] directory 本通话新目录，必须不存在；只由后台诊断调用。
     */
    explicit PcmDiagnosticRecorder(QString directory):directory_(std::move(directory)),history_(new std::array<PcmDiagnosticFrame,100>) {}
    /** @brief 已开始时补写WAV长度；只在诊断线程退出后销毁。 */
    ~PcmDiagnosticRecorder() { finish(); }
    /** @brief 提交一帧副本；触发前仅覆盖固定前史，触发后写有限文件。
     * @param[in] frame 真实采集帧副本，序号缺口在CSV标记，不能假定文件时间连续。
     * @param[in] manual true为调试标记触发；不影响实际通话数据。
     */
    void consume(const PcmDiagnosticFrame &frame,bool manual=false) {
        if(done_) return;
        if(!started_ && (manual || frame.observation.raw.peak>=12000)) {
            if(!open()) { done_=true;return; }
            for(size_t n=count_;n>0 && !done_;--n) write((*history_)[(cursor_+history_->size()-n)%history_->size()]);
        }
        if(started_) { if(!done_) write(frame);return; }
        (*history_)[cursor_]=frame;cursor_=(cursor_+1)%history_->size();count_=std::min(count_+1,history_->size());
    }
    /** @brief 完成有限录音或通话结束；幂等，不删除录音文件。
     * @note 序号有缺口时WAV按实际记录帧拼接，CSV的gap标识缺失，禁止据文件时长推断无丢诊断。
     */
    void finish() {
        if(!started_ || closed_) return;
        closed_=true;done_=true;
        for(auto &file:files_) { if(file.isOpen()) { file.seek(0);const auto bytes=header(uint32_t(frames_*Samples*2));if(file.write(bytes.data(),bytes.size())!=int64_t(bytes.size())) failed_=true;file.close(); } }
        csv_.close();
        notice_=QStringLiteral("[call-pcm] done directory=%1 frames=%2 gaps=%3 writeError=%4 limitFrames=500").arg(directory_).arg(frames_).arg(gaps_).arg(failed_);
    }
    /** @return 一次性状态消息，不包含PCM；仅所属诊断线程读取。 */
    QString takeNotice() { auto result=notice_;notice_.clear();return result; }
    /** @return true已经完成/失败，不再接收诊断录音，实际通话不受影响。 */
    bool done() const { return done_; }
private:
    /** @brief 生成单声道16kHz S16LE WAV44字节头，不依赖本机字节序。 */
    static std::array<char,44> header(uint32_t dataBytes) {
        std::array<char,44> out{};
        const auto word=[&](size_t at,uint32_t value,int bytes) { for(int i=0;i<bytes;++i) out[at+size_t(i)]=char(value>>(8*i)); };
        std::copy_n("RIFF",4,out.begin());word(4,36+dataBytes,4);std::copy_n("WAVEfmt ",8,out.begin()+8);
        word(16,16,4);word(20,1,2);word(22,1,2);word(24,16000,4);word(28,32000,4);word(32,2,2);word(34,16,2);
        std::copy_n("data",4,out.begin()+36);word(40,dataBytes,4);return out;
    }
    /** @brief 首次触发时创建全新目录和五条mono WAV/索引；任何失败只关闭诊断录音。 */
    bool open() {
        if(QFileInfo::exists(directory_) || !QDir().mkpath(directory_)) { notice_=QStringLiteral("[call-pcm] create directory failed");return false; }
        static const char *names[]={"raw.wav","aec.wav","preprocess.wav","upload.wav","reference.wav"};
        started_=true;
        for(size_t i=0;i<files_.size();++i) {
            files_[i].setFileName(directory_+'/'+QString::fromLatin1(names[i]));
            const auto bytes=header(0);
            if(!files_[i].open(QIODevice::WriteOnly) || files_[i].write(bytes.data(),bytes.size())!=int64_t(bytes.size())) { failed_=true;finish();return false; }
        }
        csv_.setFileName(directory_+QStringLiteral("/frames.csv"));
        if(!csv_.open(QIODevice::WriteOnly) || csv_.write("index,captureFirstMonoUs,valid,gap,rawHash,aecHash,preHash,uploadHash,agcGainValid,agcGainDb\n")<0) { failed_=true;finish();return false; }
        notice_=QStringLiteral("[call-pcm] started directory=%1 preFrames=%2 maxFrames=500").arg(directory_).arg(count_);return true;
    }
    /** @brief 写一个实际记录帧；无效中间阶段只占位，CSV有效位明确区分。 */
    void write(const PcmDiagnosticFrame &frame) {
        const std::array<const Pcm*,5> stages{{&frame.raw,&frame.aec,&frame.preprocess,&frame.output,&frame.reference}};
        std::array<char,Samples*2> bytes{};
        for(size_t stage=0;stage<stages.size();++stage) {
            for(int i=0;i<Samples;++i) { const auto v=(frame.valid&(1U<<stage)) ? uint16_t((*stages[stage])[size_t(i)]) : uint16_t(0);bytes[size_t(i)*2]=char(v);bytes[size_t(i)*2+1]=char(v>>8); }
            if(files_[stage].write(bytes.data(),bytes.size())!=int64_t(bytes.size())) { failed_=true;finish();return; }
        }
        const auto gap=lastIndex_ && frame.index>lastIndex_+1 ? frame.index-lastIndex_-1 : 0;gaps_+=gap;lastIndex_=frame.index;
        const auto &o=frame.observation;
        const auto row=QStringLiteral("%1,%2,%3,%4,%5,%6,%7,%8,%9,%10\n").arg(frame.index).arg(frame.captureUs).arg(frame.valid).arg(gap)
            .arg(o.raw.hash).arg(o.aec.hash).arg(o.preprocess.hash).arg(o.output.hash).arg(o.pre.gainValid).arg(o.pre.gainDb).toUtf8();
        if(csv_.write(row)!=row.size()) { failed_=true;finish();return; }
        if(++frames_>=500) finish();
    }
    QString directory_,notice_;std::unique_ptr<std::array<PcmDiagnosticFrame,100>> history_; /**< 诊断线程独占固定前史及目录/一次性通知。 */
    std::array<QFile,5> files_;QFile csv_;size_t count_=0,cursor_=0; /**< 五路独立文件及前史索引，无跨线程访问。 */
    uint64_t frames_=0,gaps_=0,lastIndex_=0;bool started_=false,done_=false,closed_=false,failed_=false; /**< 有限录音进度/错误，仅诊断线程修改。 */
};
}
