# @file dsp_tests.pro
# @brief 运行真实SpeexDSP、配置和样本时间参考的硬件无关回归。
# @author Du lin
# @date 2026-10-31
QT += core testlib
QT -= gui
CONFIG += console testcase c++17
CONFIG -= app_bundle
TARGET = dsp_tests
INCLUDEPATH += $$PWD/../../..
SOURCES += dsp_tests.cpp ../AudioProcessing.cpp ../SpeexDsp.cpp
HEADERS += ../AudioProcessing.h ../SpeexDsp.h ../PlaybackReference.h
