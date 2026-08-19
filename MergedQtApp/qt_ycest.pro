TARGET = MergedQtApp
QT += core gui widgets opengl serialport network sql virtualkeyboard qml quick quickwidgets

CONFIG += c++17 thread link_pkgconfig
PKGCONFIG += gstreamer-1.0 gstreamer-video-1.0 gstreamer-allocators-1.0

# EGLFS 视频链路强制使用 Rockchip RGA。只兼容不同 BSP 使用的
# pkg-config 包名；如果开发文件不存在则直接中止构建，不启用 CPU 回退。
packagesExist(librga) {
    PKGCONFIG += librga
} else {
    packagesExist(rockchip_rga) {
        PKGCONFIG += rockchip_rga
    } else {
        error("RK3566 EGLFS video requires Rockchip librga development files (librga.pc or rockchip_rga.pc)")
    }
}

# The following define makes your compiler emit warnings if you use
# any Qt feature that has been marked deprecated (the exact warnings
# depend on your compiler). Please consult the documentation of the
# deprecated API in order to know how to port your code away from it.
DEFINES += QT_DEPRECATED_WARNINGS PNG_SKIP_iCCP_CHECK=1

# RK3566 使用 GStreamer 1.0。交叉编译时请让 PKG_CONFIG_SYSROOT_DIR 和
# PKG_CONFIG_LIBDIR 指向 RK3566 sysroot，qmake 会通过 pkg-config 注入头文件与库。
DEFINES += QT_YCEST_RK3566

# FaceGate SDK settings are kept separate from multimedia configuration.
INSPIREFACE_ROOT = /home/lubancat/rk3566_inspireface/inspireface-sdk/inspireface-linux-aarch64-rk356x-rk3588-1.2.3/InspireFace
SYSROOT = /opt/sysroot_ubuntu20
RKNN_ROOT = /home/lubancat/rknn-toolkit2/rknpu2/runtime/Linux/librknn_api

INCLUDEPATH += \
    $$PWD \
    $$PWD/shell \
    $$PWD/modules/multimedia \
    $$PWD/modules/facegate \
    $$PWD/modules/facegate/FaceGateQt/config \
    $$PWD/modules/facegate/FaceGateQt/audio \
    $$PWD/modules/facegate/FaceGateQt/core \
    $$PWD/modules/facegate/FaceGateQt/database \
    $$PWD/modules/facegate/FaceGateQt/import \
    $$PWD/modules/facegate/FaceGateQt/gate \
    $$PWD/modules/facegate/FaceGateQt/ui \
    $$PWD/components/rga

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
    common/face_image_sync_bridge.cpp \
    common/clients/icboard_client.cpp \
    common/clients/signalboard_client.cpp \
    common/protocols/icboard_decoder.cpp \
    common/protocols/icboard_splitter.cpp \
    common/protocols/signalboard_decoder.cpp \
    common/protocols/signalboard_splitter.cpp \
    common/sql/dbstore.cpp \
    common/sql/network_personnel_store.cpp \
    common/workers/icboard_worker.cpp \
    common/workers/signalboard_worker.cpp \
    components/cursoroverlay/CursorOverlay.cpp \
    components/rga/RgaImageProcessor.cpp \
    components/cursoroverlay/keyboard_dialog.cpp \
    components/features/command_dialog.cpp \
    components/features/key_service.cpp \
    components/file_manager/file_manager_page.cpp \
    components/multimedia/disk_usage_monitor.cpp \
    components/multimedia/network_status_monitor.cpp \
    components/multimedia/media_toast.cpp \
    components/multimedia/netInfo_popup.cpp \
    components/multimedia/snapshot_processor.cpp \
    components/multimedia/gst_player_widget.cpp \
    components/multimedia/vol_ctrl.cpp \
    components/multimedia/volumepopup.cpp \
    components/settings/appstyle.cpp \
    components/settings/auto_connect_manager.cpp \
    components/settings/ftppage.cpp \
    components/settings/mqtt/ic_mqtt_gateway.cpp \
    components/settings/mqtt/mqttmessagerouter.cpp \
    components/settings/mqtt/mqttipcclient.cpp \
    components/settings/mqtt/mqttmanager.cpp \
    components/settings/mqtt/mqttservice.cpp \
    components/udisk_status/udisk_status.cpp \
    ic_board/core_migration_audit/core_migration_sync.cpp \
    ic_board/core_migration_audit/core_network_card_compat.cpp \
    ic_board/device_config_sync.cpp \
    ic_board/ic_board.cpp \
    ic_board/ic_event_bridge.cpp \
    ic_board/ic_offline.cpp \
    ic_board/ic_offline_checker.cpp \
    ic_board/ic_offline_qr.cpp \
    ic_board/network_access_service.cpp \
    ic_board/rs485_floor_frame_builder.cpp \
    ic_board/ic_toast.cpp \
    ic_board/ic_worker.cpp \
    ic_board/rs485_driver_port.cpp \
    ic_board/serial_init.cpp \
    platform/rk3566_platform.cpp \
    shell/AppShell.cpp \
    shell/Sr505PresenceSensor.cpp \
    shell/ModeController.cpp \
    modules/multimedia/MultimediaModuleAdapter.cpp \
    modules/facegate/FaceGateModuleAdapter.cpp \
    modules/facegate/FaceGateQt/config/AppConfig.cpp \
    modules/facegate/FaceGateQt/audio/AudioService.cpp \
    modules/facegate/FaceGateQt/core/CameraCaptureBackend.cpp \
    modules/facegate/FaceGateQt/core/CameraService.cpp \
    modules/facegate/FaceGateQt/core/FaceEngine.cpp \
    modules/facegate/FaceGateQt/core/FaceInferenceWorker.cpp \
    modules/facegate/FaceGateQt/core/LivenessWorker.cpp \
    modules/facegate/FaceGateQt/core/MaintenanceWorker.cpp \
    modules/facegate/FaceGateQt/core/SnapshotService.cpp \
    modules/facegate/FaceGateQt/core/VerificationController.cpp \
    modules/facegate/FaceGateQt/database/DatabaseWorker.cpp \
    modules/facegate/FaceGateQt/database/MysqlFaceRepository.cpp \
    modules/facegate/FaceGateQt/import/PersonExportWorker.cpp \
    modules/facegate/FaceGateQt/import/PersonImportWorker.cpp \
    modules/facegate/FaceGateQt/import/PersonXlsParser.cpp \
    modules/facegate/FaceGateQt/import/PersonXlsxExporter.cpp \
    modules/facegate/FaceGateQt/import/PersonXlsxParser.cpp \
    modules/facegate/FaceGateQt/import/UsbPersonExportTarget.cpp \
    modules/facegate/FaceGateQt/import/UsbPersonImportSource.cpp \
    modules/facegate/FaceGateQt/gate/GateOutputService.cpp \
    modules/facegate/FaceGateQt/ui/AccessPasswordDialog.cpp \
    modules/facegate/FaceGateQt/ui/AppMessageDialog.cpp \
    modules/facegate/FaceGateQt/ui/AppPasswordDialog.cpp \
    modules/facegate/FaceGateQt/ui/AdminLoginDialog.cpp \
    modules/facegate/FaceGateQt/ui/AdminPanel.cpp \
    modules/facegate/FaceGateQt/ui/CameraPreviewWidget.cpp \
    modules/facegate/FaceGateQt/ui/EnrollPreviewWidget.cpp \
    modules/facegate/FaceGateQt/ui/FaceEnrollWidget.cpp \
    modules/facegate/FaceGateQt/ui/MainWindow.cpp \
    main.cpp \
    mainwindow.cpp \
    components/splash/splashscreen.cpp \
    components/settings/settingsdialog.cpp \
    components/settings/networkpage.cpp \
    components/settings/bluetoothpage.cpp \
    components/settings/mqttpage.cpp \
    components/settings/systeminfopage.cpp \
    components/settings/standbylockpage.cpp \
    components/features/featuresdialog.cpp \
    components/multimedia/multimediademo.cpp

HEADERS += \
    common/face_image_sync_bridge.h \
    common/clients/icboard_client.h \
    common/clients/signalboard_client.h \
    common/debug/probe_log.h \
    common/protocols/icboard_decoder.h \
    common/protocols/icboard_splitter.h \
    common/protocols/signalboard_decoder.h \
    common/protocols/signalboard_splitter.h \
    common/sql/dbstore.h \
    common/sql/network_personnel_store.h \
    common/workers/icboard_worker.h \
    common/workers/signalboard_worker.h \
    components/cursoroverlay/CursorOverlay.h \
    components/rga/RgaImageProcessor.h \
    components/cursoroverlay/keyboard_dialog.h \
    components/features/command_dialog.h \
    components/features/key_service.h \
    components/file_manager/file_manager_page.h \
    components/multimedia/disk_usage_monitor.h \
    components/multimedia/network_status_monitor.h \
    components/multimedia/media_toast.h \
    components/multimedia/netinfo_popup.h \
    components/multimedia/snapshot_processor.h \
    components/multimedia/gst_player_widget.h \
    components/multimedia/vol_ctrl.h \
    components/multimedia/volumepopup.h \
    components/settings/appstyle.h \
    components/settings/auto_connect_manager.h \
    components/settings/ftppage.h \
    components/settings/mqtt/ic_mqtt_gateway.h \
    components/settings/mqtt/mqttmessagerouter.h \
    components/settings/mqtt/mqttipcclient.h \
    components/settings/mqtt/mqttmanager.h \
    components/settings/mqtt/mqttservice.h \
    components/udisk_status/udisk_status.h \
    ic_board/core_migration_audit/core_migration_sync.h \
    ic_board/core_migration_audit/core_network_card_compat.h \
    ic_board/device_config_sync.h \
    ic_board/ic_board.h \
    ic_board/ic_event_bridge.h \
    ic_board/ic_offline.h \
    ic_board/ic_offline_checker.h \
    ic_board/ic_offline_qr.h \
    ic_board/network_access_service.h \
    ic_board/rs485_floor_frame_builder.h \
    ic_board/ic_toast.h \
    ic_board/ic_worker.h \
    ic_board/rs485_driver_port.h \
    ic_board/serial_init.h \
    platform/rk3566_platform.h \
    shell/AppShell.h \
    shell/Sr505PresenceSensor.h \
    shell/IApplicationModule.h \
    shell/IPresenceSensor.h \
    shell/ModeController.h \
    modules/multimedia/MultimediaModuleAdapter.h \
    modules/facegate/FaceGateModuleAdapter.h \
    modules/facegate/FaceGateQt/config/AppConfig.h \
    modules/facegate/FaceGateQt/config/CameraProfile.h \
    modules/facegate/FaceGateQt/audio/AudioService.h \
    modules/facegate/FaceGateQt/core/CameraCaptureBackend.h \
    modules/facegate/FaceGateQt/core/CameraService.h \
    modules/facegate/FaceGateQt/core/FaceEngine.h \
    modules/facegate/FaceGateQt/core/FaceInferenceWorker.h \
    modules/facegate/FaceGateQt/core/ImageUtils.h \
    modules/facegate/FaceGateQt/core/LivenessWorker.h \
    modules/facegate/FaceGateQt/core/MaintenanceWorker.h \
    modules/facegate/FaceGateQt/core/SnapshotService.h \
    modules/facegate/FaceGateQt/core/VerificationController.h \
    modules/facegate/FaceGateQt/core/VerificationTypes.h \
    modules/facegate/FaceGateQt/database/DatabaseWorker.h \
    modules/facegate/FaceGateQt/database/IFaceRepository.h \
    modules/facegate/FaceGateQt/database/ISyncRepository.h \
    modules/facegate/FaceGateQt/database/MysqlFaceRepository.h \
    modules/facegate/FaceGateQt/database/RemoteSyncStub.h \
    modules/facegate/FaceGateQt/import/PersonExportWorker.h \
    modules/facegate/FaceGateQt/import/PersonImportWorker.h \
    modules/facegate/FaceGateQt/import/PersonXlsParser.h \
    modules/facegate/FaceGateQt/import/PersonXlsxExporter.h \
    modules/facegate/FaceGateQt/import/PersonXlsxParser.h \
    modules/facegate/FaceGateQt/import/UsbPersonExportTarget.h \
    modules/facegate/FaceGateQt/import/UsbPersonImportSource.h \
    modules/facegate/FaceGateQt/gate/GateOutputService.h \
    modules/facegate/FaceGateQt/ui/AccessPasswordDialog.h \
    modules/facegate/FaceGateQt/ui/AppMessageDialog.h \
    modules/facegate/FaceGateQt/ui/AppPasswordDialog.h \
    modules/facegate/FaceGateQt/ui/AdminLoginDialog.h \
    modules/facegate/FaceGateQt/ui/AdminPanel.h \
    modules/facegate/FaceGateQt/ui/CameraPreviewWidget.h \
    modules/facegate/FaceGateQt/ui/EnrollPreviewWidget.h \
    modules/facegate/FaceGateQt/ui/FaceEnrollWidget.h \
    modules/facegate/FaceGateQt/ui/MainWindow.h \
    mainwindow.h \
    components/splash/splashscreen.h \
    components/settings/settingsdialog.h \
    components/settings/networkpage.h \
    components/settings/bluetoothpage.h \
    components/settings/mqttpage.h \
    components/settings/systeminfopage.h \
    components/settings/standbylockpage.h \
    components/features/featuresdialog.h \
    components/multimedia/multimediademo.h

FORMS += \
    mainwindow.ui \
    components/features/featuresdialog.ui \
    components/multimedia/multimediademo.ui \
    components/settings/networkpage.ui

# 资源文件
RESOURCES += \
    resources.qrc \
    modules/facegate/FaceGateQt/facegate_resources.qrc

# .pro.user files are user-specific and should be excluded from version control
# The following line is a comment to remind users that user-specific files should be handled appropriately
# qt_ycest.pro.user

# Default rules for deployment.
qnx: target.path = /tmp/$${TARGET}/bin
else: unix:!android: target.path = /opt/$${TARGET}/bin
!isEmpty(target.path): INSTALLS += target
