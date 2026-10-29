# @file web.pri
# @brief 新增 Web 模块源文件独立登记；HTTP daemon 有自己的 CMake，不链接 Qt。
# @author Du lin
# @date 2026-10-09
# @note 最近修改：2026-10-26；修改人：Du lin；登记统一 NetworkManager 后端，供本地/Web 网络配置及隔离测试共同使用。

SOURCES += \
    $$PWD/../../common/network_config.cpp \
    $$PWD/../../common/network_manager.cpp \
    $$PWD/web_upload_presenter.cpp \
    $$PWD/web_local_api.cpp \
    $$PWD/web_local_runtime.cpp \
    $$PWD/web_floor_schedule.cpp \
    $$PWD/web_hotspot.cpp \
    $$PWD/web_paths.cpp \
    $$PWD/web_service.cpp \
    $$PWD/web_resource_update.cpp \
    $$PWD/web_bridge_server.cpp \
    $$PWD/web_face_bridge.cpp \
    $$PWD/web_face_validation.cpp \
    $$PWD/web_face_policy.cpp \
    $$PWD/local_import_gate.cpp \
    $$PWD/../../common/sql/web/web_schema.cpp \
    $$PWD/../../common/sql/web/web_account_repository.cpp \
    $$PWD/../../common/sql/web/web_upload_repository.cpp \
    $$PWD/../../common/sql/web/web_import_repository.cpp \
    $$PWD/../../common/sql/web/web_media_repository.cpp \
    $$PWD/../../common/sql/web/web_person_repository.cpp

HEADERS += \
    $$PWD/../../common/network_config.h \
    $$PWD/web_upload_presenter.h \
    $$PWD/web_local_api.h \
    $$PWD/web_local_runtime.h \
    $$PWD/web_floor_schedule.h \
    $$PWD/web_hotspot.h \
    $$PWD/web_paths.h \
    $$PWD/web_service.h \
    $$PWD/web_resource_update.h \
    $$PWD/web_bridge_server.h \
    $$PWD/web_face_bridge.h \
    $$PWD/web_face_policy.h \
    $$PWD/local_import_gate.h \
    $$PWD/../../common/sql/web/web_sql.h \
    $$PWD/../../common/sql/web/web_schema.h \
    $$PWD/../../common/sql/web/web_account_repository.h \
    $$PWD/../../common/sql/web/web_upload_repository.h \
    $$PWD/../../common/sql/web/web_import_repository.h \
    $$PWD/../../common/sql/web/web_media_repository.h \
    $$PWD/../../common/sql/web/web_person_repository.h

HEADERS += $$PWD/../../common/network_manager.h
