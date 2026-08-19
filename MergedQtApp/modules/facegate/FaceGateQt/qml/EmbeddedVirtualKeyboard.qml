/*
 * 作者: Dulin
 * 日期: 2026-07-03
 * 功能: 管理员界面使用的内嵌 Qt Virtual Keyboard 输入面板。
 */

import QtQuick 2.12
import QtQuick.VirtualKeyboard 2.2

Item {
    id: root
    width: 1024
    // InputPanel 原始布局按 360 像素高度设计。管理后台空间不足时，
    // 不再直接裁剪面板，而是整体缩放，避免出现“键盘过大且下半部分被隐藏”。
    height: 300
    clip: true

    property real naturalHeight: 360
    property real panelScale: height > 0 ? height / naturalHeight : 1

    Rectangle {
        anchors.fill: parent
        color: "#101820"
    }

    Item {
        id: scaledPanel
        width: root.panelScale > 0 ? root.width / root.panelScale : root.width
        height: root.naturalHeight
        scale: root.panelScale
        transformOrigin: Item.TopLeft

        InputPanel {
            id: inputPanel
            objectName: "embeddedInputPanel"
            anchors.fill: parent
        }
    }
}
