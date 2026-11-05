# @file ui_performance_tests.pro
# @brief 构建真实诊断QApplication和事件/日志计时回归，不依赖声卡或GStreamer。
# @author Du lin
# @date 2026-11-03
QT += core widgets testlib
CONFIG += console testcase c++17
CONFIG -= app_bundle
TARGET = ui_performance_tests
INCLUDEPATH += $$PWD/../../..
SOURCES += ui_performance_tests.cpp ../../../common/debug/ui_performance.cpp
HEADERS += ../../../common/debug/ui_performance.h
