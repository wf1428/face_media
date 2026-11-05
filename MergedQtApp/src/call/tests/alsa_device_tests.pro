# @file alsa_device_tests.pro
# @brief 编译实际 ALSA 封装，以 API 替身检查单声道/双声道转换和错误恢复。
# @author Du lin
# @date 2026-10-29
QT += core testlib
QT -= gui
CONFIG += console testcase c++17
CONFIG -= app_bundle
TARGET = alsa_device_tests
INCLUDEPATH += $$PWD $$PWD/../../..
SOURCES += alsa_device_tests.cpp ../AlsaAudioDevice.cpp
