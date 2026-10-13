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

    // 横屏宿主本身已经收缩到键盘的实际显示范围，因此这里只做设计尺寸
    // 到宿主尺寸的等比适配；竖屏继续保持原来的满宽缩放方式。
    property bool landscapeHost: false
    property real designWidth: 1024
    property real naturalHeight: 360
    property real panelScale: landscapeHost
                              ? Math.min(width / designWidth,
                                         height / naturalHeight)
                              : (height > 0 ? height / naturalHeight : 1)

    Rectangle {
        anchors.fill: parent
        color: "#101820"
    }

    Item {
        id: scaledPanel
        width: root.landscapeHost
               ? root.designWidth
               : (root.panelScale > 0 ? root.width / root.panelScale : root.width)
        height: root.naturalHeight
        scale: root.panelScale
        transformOrigin: Item.TopLeft
        x: root.landscapeHost
           ? Math.max(0, (root.width - width * scale) / 2)
           : 0
        y: root.landscapeHost
           ? Math.max(0, root.height - height * scale)
           : 0

        InputPanel {
            id: inputPanel
            objectName: "embeddedInputPanel"
            anchors.fill: parent
        }
    }
}
