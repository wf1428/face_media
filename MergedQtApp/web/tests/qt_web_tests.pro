# @file qt_web_tests.pro
# @brief Qt Web 模块隔离测试，不链接摄像头/RKNN/播放器，不访问板端数据库。
# @author Du lin
# @date 2026-10-09

TEMPLATE = app
TARGET = qt_web_tests
QT += core gui network sql testlib widgets
CONFIG += console c++17 testcase
CONFIG -= app_bundle
INCLUDEPATH += $$PWD/../.. $$[QT_INSTALL_HEADERS]/QtZlib
LIBS += -lz

SOURCES += \
    $$PWD/../../common/network_config.cpp \
    $$PWD/../../components/settings/networkpage.cpp \
    $$PWD/../../modules/web/web_upload_presenter.cpp \
    $$PWD/../../components/multimedia/media_toast.cpp \
    $$PWD/../../modules/web/web_local_api.cpp \
    $$PWD/../../modules/web/web_local_runtime.cpp \
    $$PWD/../../ic_board/rs485_floor_frame_builder.cpp \
    $$PWD/../../ic_board/local_access_service.cpp \
    $$PWD/../../modules/web/web_hotspot.cpp \
    $$PWD/qt_web_tests.cpp \
    $$PWD/test_platform.cpp \
    $$PWD/../../common/sql/dbstore.cpp \
    $$PWD/../../common/sql/local_personnel_store.cpp \
    $$PWD/../../common/storage_policy.cpp \
    $$PWD/../../modules/facegate/FaceGateQt/import/PersonXlsParser.cpp \
    $$PWD/../../modules/facegate/FaceGateQt/import/PersonXlsxParser.cpp \
    $$PWD/../../modules/web/web_service.cpp \
    $$PWD/../../modules/web/web_resource_update.cpp \
    $$PWD/../../components/udisk_status/udisk_status.cpp \
    $$PWD/../../modules/web/web_paths.cpp \
    $$PWD/../../modules/web/web_face_policy.cpp \
    $$PWD/../../modules/web/local_import_gate.cpp \
    $$PWD/../../common/sql/web/web_schema.cpp \
    $$PWD/../../common/sql/web/web_account_repository.cpp \
    $$PWD/../../common/sql/web/web_upload_repository.cpp \
    $$PWD/../../common/sql/web/web_import_repository.cpp \
    $$PWD/../../common/sql/web/web_media_repository.cpp \
    $$PWD/../../common/sql/web/web_person_repository.cpp

HEADERS += $$PWD/../../modules/web/web_service.h $$PWD/../../modules/web/web_local_api.h $$PWD/../../modules/web/web_local_runtime.h
HEADERS += $$PWD/../../modules/web/web_resource_update.h $$PWD/../../components/udisk_status/udisk_status.h
win32: INCLUDEPATH += $$PWD/../../.web-build-tools/libarchive/include

# 编译真实后台网络适配器；测试不调用真实网卡命令和设备弹窗。
HEADERS += $$PWD/../../components/settings/networkpage.h
FORMS += $$PWD/../../components/settings/networkpage.ui
win32: QMAKE_CXXFLAGS += -include $$PWD/network_test_compat.h
