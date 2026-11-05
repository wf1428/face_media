# @file voice_call_tests.pro
# @brief 在本机 Qt 测试真实协议、通话状态机、IPC 适配器和音频线程，硬件边界使用可控替身。
# @author Du lin
# @date 2026-10-29
# @note 最近修改：2026-11-04；修改人：Du lin；登记无互斥下行缓存和采集PCM路径切换平滑头文件，保留DSP库/协议及原依赖。
QT += core network widgets testlib
CONFIG += console testcase c++17
CONFIG -= app_bundle
TARGET = voice_call_tests
INCLUDEPATH += $$PWD/../../..
SOURCES += voice_call_tests.cpp fake_audio_device.cpp \
    ../../../common/debug/ui_performance.cpp \
    ../AudioProcessing.cpp ../SpeexDsp.cpp ../CallGpioButton.cpp ../CallManager.cpp ../MqttCallAdapter.cpp ../AudioEngine.cpp ../CallPanel.cpp \
    ../../../components/settings/mqtt/mqttipcclient.cpp \
    ../../../components/cursoroverlay/keyboard_dialog.cpp
HEADERS += ../RealtimeAudio.h ../PlaybackReference.h ../SpeexDsp.h ../CallGpioButton.h ../CallPanel.h ../CallManager.h ../MqttCallAdapter.h ../AudioEngine.h \
    ../../../components/settings/mqtt/mqttipcclient.h \
    ../../../components/cursoroverlay/keyboard_dialog.h

SOURCES += ../../../components/settings/mqtt/mqttpacketparser.cpp ../CallAudioTransport.cpp
HEADERS += ../../../components/settings/mqtt/mqttpacketparser.h ../CallAudioTransport.h

HEADERS += ../ConcurrentJitterBuffer.h ../CaptureContinuity.h
