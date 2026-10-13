/* 多媒体控制台整页 Quick 主菜单。 */
import QtQuick 2.12

Rectangle {
    id: root
    QtObject {
        id: theme
        readonly property color background: "#f5f7fa"
        readonly property color panel: "#ffffff"
        readonly property color border: "#dcdfe6"
        readonly property color text: "#303133"
        readonly property color secondaryText: "#606266"
        readonly property color accent: "#409eff"
        readonly property color hover: "#ecf5ff"
        readonly property color pressed: "#d9ecff"
    }
    color: theme.background
    signal actionTriggered(int index)

    readonly property var actions: [
        { "title": "设置",     "icon": "qrc:/static/icons/system.png" },
        { "title": "功能管理", "icon": "qrc:/static/icons/applications.png" },
        { "title": "主界面",   "icon": "qrc:/static/icons/ui_media.png" },
        { "title": "调试",     "icon": "qrc:/static/icons/debug.png" },
        { "title": "文件管理", "icon": "qrc:/static/icons/file_manage.png" },
        { "title": "重启设备", "icon": "qrc:/static/icons/shutdown.png" }
    ]

    Rectangle {
        width: parent.width * 0.7
        height: parent.height * 0.55
        x: -width * 0.25
        y: -height * 0.2
        radius: width / 2
        color: theme.hover
        opacity: 0.22
    }

    Column {
        anchors.fill: parent
        anchors.margins: Math.max(24, Math.min(root.width, root.height) * 0.055)
        spacing: 22

        Text {
            width: parent.width
            text: "智能设备控制台"
            color: theme.text
            font.pixelSize: Math.max(25, Math.min(36, root.width / 30))
            font.bold: true
        }

        GridView {
            id: grid
            width: parent.width
            height: parent.height - y
            cellWidth: width / (root.width >= root.height ? 3 : 2)
            cellHeight: height / (root.width >= root.height ? 2 : 3)
            model: root.actions
            interactive: false

            delegate: Item {
                width: grid.cellWidth
                height: grid.cellHeight

                Rectangle {
                    id: card
                    anchors.fill: parent
                    anchors.margins: 10
                    radius: 18
                    color: cardMouse.pressed ? theme.pressed :
                           (cardMouse.containsMouse ? theme.hover : theme.panel)
                    border.width: 1
                    border.color: cardMouse.containsMouse ? theme.accent : theme.border
                    scale: cardMouse.pressed ? 0.965 : 1

                    Behavior on color { ColorAnimation { duration: 150 } }
                    Behavior on scale {
                        NumberAnimation { duration: 110; easing.type: Easing.OutCubic }
                    }

                    Column {
                        anchors.centerIn: parent
                        width: parent.width - 28
                        spacing: 10
                        Image {
                            anchors.horizontalCenter: parent.horizontalCenter
                            width: Math.min(68, card.height * 0.36)
                            height: width
                            source: modelData.icon
                            sourceSize.width: width
                            sourceSize.height: height
                            fillMode: Image.PreserveAspectFit
                        }
                        Text {
                            width: parent.width
                            text: modelData.title
                            color: theme.secondaryText
                            font.pixelSize: 19
                            font.bold: true
                            horizontalAlignment: Text.AlignHCenter
                        }
                    }

                    MouseArea {
                        id: cardMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: root.actionTriggered(index)
                    }
                }
            }
        }
    }
}
