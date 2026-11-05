# @file diagnostic_cause_tests.pro
# @brief 编译UI分段、PCM阶段、有限录音与调度证据的无硬件回归。
# @author Du lin
# @date 2026-11-04
QT += core gui widgets testlib
CONFIG += console testcase c++17 thread
TARGET = diagnostic_cause_tests
INCLUDEPATH += $$PWD/../../..
SOURCES += diagnostic_cause_tests.cpp ../AudioProcessing.cpp ../SpeexDsp.cpp ../../../common/debug/ui_performance.cpp
HEADERS += ../AudioProcessing.h ../SpeexDsp.h ../PcmDiagnostics.h ../PcmDiagnosticRecorder.h ../SchedulingDiagnostics.h ../../../common/debug/ui_performance.h ../../../common/debug/thread_observation.h
