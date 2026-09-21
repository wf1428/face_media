/* 文件管理、调试工具等后台页共用的 Quick 标题操作栏。 */
import QtQuick 2.12

Rectangle {
    id: root
    QtObject {
        id: theme
        readonly property color panel: "#ffffff"
        readonly property color border: "#dcdfe6"
        readonly property color text: "#303133"
        readonly property color secondaryText: "#606266"
        readonly property color accent: "#409eff"
        readonly property color hover: "#ecf5ff"
        readonly property color pressed: "#d9ecff"
    }
    property string title: ""
    property string detail: ""
    property var actions: []
    signal actionTriggered(int index)

    color: theme.panel
    radius: 14
    border.width: 1
    border.color: theme.border

    Row {
        id: actionRow
        width: implicitWidth
        height: 44
        anchors.right: parent.right
        anchors.rightMargin: 14
        anchors.verticalCenter: parent.verticalCenter
        spacing: 9

        Repeater {
            model: root.actions
            Rectangle {
                width: Math.max(82, actionText.implicitWidth + 30)
                height: 44
                radius: 10
                color: actionMouse.pressed ? theme.pressed :
                       (actionMouse.containsMouse ? theme.hover : theme.panel)
                border.width: 1
                border.color: actionMouse.containsMouse ? theme.accent : theme.border
                scale: actionMouse.pressed ? 0.96 : 1

                Behavior on color { ColorAnimation { duration: 140 } }
                Behavior on scale { NumberAnimation { duration: 100 } }

                Text {
                    id: actionText
                    anchors.centerIn: parent
                    text: modelData
                    color: theme.secondaryText
                    font.pixelSize: 16
                    font.bold: true
                }
                MouseArea {
                    id: actionMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    onClicked: root.actionTriggered(index)
                }
            }
        }
    }

    Column {
        anchors.left: parent.left
        anchors.leftMargin: 18
        anchors.right: actionRow.left
        anchors.rightMargin: 14
        anchors.verticalCenter: parent.verticalCenter
        spacing: 4

        Text {
            width: parent.width
            text: root.title
            color: theme.text
            font.pixelSize: 22
            font.bold: true
            elide: Text.ElideRight
        }
        Text {
            width: parent.width
            visible: root.detail.length > 0
            text: root.detail
            color: theme.secondaryText
            font.pixelSize: 15
            elide: Text.ElideMiddle
        }
    }
}
