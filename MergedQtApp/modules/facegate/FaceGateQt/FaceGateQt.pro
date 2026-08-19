QT += core gui widgets sql virtualkeyboard network qml quick quickwidgets

greaterThan(QT_MAJOR_VERSION, 4): QT += widgets

TARGET = FaceGateQt
TEMPLATE = app
CONFIG += c++17 thread link_pkgconfig

packagesExist(librga) {
    PKGCONFIG += librga
} else:packagesExist(rockchip_rga) {
    PKGCONFIG += rockchip_rga
} else {
    error("FaceGateQt requires librga/rockchip_rga; CPU YUV conversion fallback is disabled")
}

DEFINES += QT_DEPRECATED_WARNINGS

INCLUDEPATH += \
    $$PWD/config \
    $$PWD/audio \
    $$PWD/core \
    $$PWD/database \
    $$PWD/import \
    $$PWD/gate \
    $$PWD/ui \
    $$PWD/../../.. \
    $$PWD/../../../components/rga


INSPIREFACE_ROOT = /home/lubancat/rk3566_inspireface/inspireface-sdk/inspireface-linux-aarch64-rk356x-rk3588-1.2.3/InspireFace

SYSROOT = /opt/sysroot_ubuntu20

RKNN_ROOT = /home/lubancat/rknn-toolkit2/rknpu2/runtime/Linux/librknn_api

!isEmpty(INSPIREFACE_ROOT) {
    DEFINES += FACEGATE_WITH_INSPIREFACE
    INCLUDEPATH += $$INSPIREFACE_ROOT/include
    LIBS += -L$$INSPIREFACE_ROOT/lib -lInspireFace
}

!isEmpty(RKNN_ROOT) {
    DEFINES += FACEGATE_WITH_RKNN
    INCLUDEPATH += $$RKNN_ROOT/include
    LIBS += -L$$SYSROOT/usr/lib/aarch64-linux-gnu -lrknnrt
}

unix: LIBS += -lpthread -lz

SOURCES += \
    main.cpp \
    ../../../common/face_image_sync_bridge.cpp \
    ../../../common/sql/dbstore.cpp \
    ../../../common/sql/network_personnel_store.cpp \
    ../../../platform/rk3566_platform.cpp \
    config/AppConfig.cpp \
    audio/AudioService.cpp \
    ../../../components/rga/RgaImageProcessor.cpp \
    core/CameraCaptureBackend.cpp \
    core/CameraService.cpp \
    core/FaceEngine.cpp \
    core/FaceInferenceWorker.cpp \
    core/LivenessWorker.cpp \
    core/MaintenanceWorker.cpp \
    core/SnapshotService.cpp \
    core/VerificationController.cpp \
    database/DatabaseWorker.cpp \
    database/MysqlFaceRepository.cpp \
    import/PersonExportWorker.cpp \
    import/PersonImportWorker.cpp \
    import/PersonXlsParser.cpp \
    import/PersonXlsxExporter.cpp \
    import/PersonXlsxParser.cpp \
    import/UsbPersonExportTarget.cpp \
    import/UsbPersonImportSource.cpp \
    gate/GateOutputService.cpp \
    ui/AccessPasswordDialog.cpp \
    ui/AppMessageDialog.cpp \
    ui/AppPasswordDialog.cpp \
    ui/AdminLoginDialog.cpp \
    ui/AdminPanel.cpp \
    ui/CameraPreviewWidget.cpp \
    ui/EnrollPreviewWidget.cpp \
    ui/FaceEnrollWidget.cpp \
    ui/MainWindow.cpp

HEADERS += \
    ../../../common/face_image_sync_bridge.h \
    ../../../common/sql/dbstore.h \
    ../../../common/sql/network_personnel_store.h \
    ../../../platform/rk3566_platform.h \
    config/AppConfig.h \
    config/CameraProfile.h \
    audio/AudioService.h \
    ../../../components/rga/RgaImageProcessor.h \
    core/CameraCaptureBackend.h \
    core/CameraService.h \
    core/FaceEngine.h \
    core/FaceInferenceWorker.h \
    core/ImageUtils.h \
    core/LivenessWorker.h \
    core/MaintenanceWorker.h \
    core/SnapshotService.h \
    core/VerificationController.h \
    core/VerificationTypes.h \
    database/DatabaseWorker.h \
    database/IFaceRepository.h \
    database/ISyncRepository.h \
    database/MysqlFaceRepository.h \
    database/RemoteSyncStub.h \
    import/PersonExportWorker.h \
    import/PersonImportWorker.h \
    import/PersonXlsParser.h \
    import/PersonXlsxExporter.h \
    import/PersonXlsxParser.h \
    import/UsbPersonExportTarget.h \
    import/UsbPersonImportSource.h \
    gate/GateOutputService.h \
    ui/AccessPasswordDialog.h \
    ui/AppMessageDialog.h \
    ui/AppPasswordDialog.h \
    ui/AdminLoginDialog.h \
    ui/AdminPanel.h \
    ui/CameraPreviewWidget.h \
    ui/EnrollPreviewWidget.h \
    ui/FaceEnrollWidget.h \
    ui/MainWindow.h


RESOURCES += facegate_resources.qrc

target.path = /home/cat/FaceGateQt
INSTALLS += target
