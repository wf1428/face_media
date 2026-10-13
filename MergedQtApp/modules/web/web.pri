# @file web.pri
# @brief 新增 Web 模块源文件独立登记；HTTP daemon 有自己的 CMake，不链接 Qt。
# @author Du lin
# @date 2026-10-09
SOURCES += \
    $$PWD/web_paths.cpp \
    $$PWD/web_service.cpp \
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
    $$PWD/web_paths.h \
    $$PWD/web_service.h \
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
