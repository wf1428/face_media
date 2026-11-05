/**
 * @file dsp_observation_parity.cpp
 * @brief 对固定合成输入输出PCM及摘要，供诊断接入前后逐配置对比，不访问真实声卡或INI。
 * @author Du lin
 * @date 2026-11-04
 */
#include <QCoreApplication>
#include <QTextStream>
#include <QFile>
#include <cmath>
#include "src/call/AudioProcessing.h"
/** @brief 遍历16种处理开关，覆盖大声、缺参考、代次改变和原重置，保存最终PCM并打印摘要。
 * @param[in] argc 必须为2。
 * @param[in] argv 第二项为隔离测试输出文件路径，只写合成PCM。
 * @return 0成功；2参数或文件读写失败。
 */
int main(int argc,char **argv) {
    QCoreApplication app(argc,argv);QTextStream output(stdout);
    if(argc!=2) return 2;QFile pcm(QString::fromLocal8Bit(argv[1]));if(!pcm.open(QIODevice::WriteOnly)) return 2;
    for(unsigned flags=0;flags<16;++flags) {
        VoiceCall::ProcessingConfig config;config.aecEnabled=flags&1;config.preprocessEnabled=flags&2;
        config.denoiseEnabled=flags&4;config.agcEnabled=flags&8;
        config.libraryPath=QString::fromLocal8Bit(qgetenv("VOICE_TEST_SPEEX_LIBRARY"));
        QString info;VoiceCall::AudioProcessing processing(config,info);uint32_t seed=137,hash=2166136261U;
        for(int frame=0;frame<500;++frame) {
            VoiceCall::Pcm mic{},reference{};
            for(int i=0;i<VoiceCall::Samples;++i) {
                const double t=double(frame*VoiceCall::Samples+i)/16000;
                seed=seed*1664525U+1013904223U;
                reference[size_t(i)]=int16_t(11000*std::sin(2*3.141592653589793*470*t));
                const int near=int((frame>=150 && frame<300 ? 42000:6000)*std::sin(2*3.141592653589793*1130*t));
                const int mixed=near+int(reference[size_t(i)])/3+int(seed%1001)-500;
                mic[size_t(i)]=int16_t(std::max(-32768,std::min(32767,mixed)));
            }
            if(frame==400) processing.reset(QStringLiteral("fixture reset"));
            processing.process(mic,frame%127<3 ? nullptr:&reference,frame<300 ? 1:2);
            QByteArray bytes;bytes.reserve(VoiceCall::Samples*2);
            for(const auto sample:mic) { const auto value=uint16_t(sample);bytes.append(char(value));bytes.append(char(value>>8)); }
            if(pcm.write(bytes)!=bytes.size()) return 2;
            for(const auto sample:mic) { const auto value=uint16_t(sample);hash=(hash^uint8_t(value))*16777619U;hash=(hash^uint8_t(value>>8))*16777619U; }
        }
        const auto stats=processing.stats();
        output<<flags<<','<<hash<<','<<stats.aecFrames<<','<<stats.preprocessFrames<<','<<stats.bypassFrames<<','<<stats.failures<<'\n';
    }
    return 0;
}
