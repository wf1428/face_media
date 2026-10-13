/* 完整 Quick 功能开关页面；配置与互斥规则由 featuresBridge 处理。 */
import QtQuick 2.12

Rectangle {
    id: root
    QtObject {
        id: theme
        readonly property color panel: "#ffffff"
        readonly property color background: "#f5f7fa"
        readonly property color border: "#dcdfe6"
        readonly property color text: "#303133"
        readonly property color secondaryText: "#606266"
        readonly property color accent: "#409eff"
        readonly property color hover: "#ecf5ff"
        readonly property color pressed: "#d9ecff"
        readonly property color accentText: "#ffffff"
    }
    color: theme.panel
    radius: 16
    border.width: 1
    border.color: theme.border

    Column {
        anchors.fill: parent
        anchors.margins: 24
        spacing: 14

        Row {
            width: parent.width
            height: 48

            Text {
                width: parent.width - closeButton.width
                height: parent.height
                text: "功能选项"
                color: theme.text
                font.pixelSize: 24
                font.bold: true
                verticalAlignment: Text.AlignVCenter
            }

            Rectangle {
                id: closeButton
                width: 42
                height: 42
                radius: 11
                color: closeMouse.pressed ? theme.pressed : theme.background
                Text {
                    anchors.centerIn: parent
                    text: "×"
                    color: theme.secondaryText
                    font.pixelSize: 29
                }
                MouseArea {
                    id: closeMouse
                    anchors.fill: parent
                    onClicked: featuresBridge.cancelAndClose()
                }
            }
        }

        Flickable {
            width: parent.width
            height: parent.height - 48 - statusArea.height - buttonRow.height - 42
            contentWidth: width
            contentHeight: featureColumn.implicitHeight
            clip: true
            boundsBehavior: Flickable.StopAtBounds

            Column {
                id: featureColumn
                width: parent.width
                spacing: 9
                Repeater {
                    model: featuresBridge.featureItems
                    Rectangle {
                        width: featureColumn.width
                        height: 58
                        radius: 12
                        color: switchMouse.pressed ? theme.pressed : theme.background
                        border.width: modelData.enabled ? 1 : 0
                        border.color: theme.accent

                        Text {
                            anchors.left: parent.left
                            anchors.leftMargin: 16
                            anchors.right: toggle.left
                            anchors.rightMargin: 12
                            anchors.verticalCenter: parent.verticalCenter
                            text: modelData.text
                            color: modelData.enabled ? theme.text : theme.secondaryText
                            font.pixelSize: 17
                            elide: Text.ElideRight
                        }
                        Rectangle {
                            id: toggle
                            width: 54
                            height: 30
                            radius: 15
                            anchors.right: parent.right
                            anchors.rightMargin: 14
                            anchors.verticalCenter: parent.verticalCenter
                            color: modelData.enabled ? theme.accent : "#c0c4cc"
                            Behavior on color { ColorAnimation { duration: 150 } }
                            Rectangle {
                                width: 24
                                height: 24
                                radius: 12
                                y: 3
                                x: modelData.enabled ? parent.width - width - 3 : 3
                                color: "#ffffff"
                                Behavior on x {
                                    NumberAnimation { duration: 170; easing.type: Easing.OutCubic }
                                }
                            }
                        }
                        MouseArea {
                            id: switchMouse
                            anchors.fill: parent
                            onClicked: featuresBridge.setFeatureEnabled(index, !modelData.enabled)
                        }
                    }
                }
            }
        }

        Rectangle {
            id: statusArea
            width: parent.width
            height: 42
            radius: 10
            color: theme.hover
            opacity: featuresBridge.statusText.length > 0 ? 1 : 0
            Behavior on opacity { NumberAnimation { duration: 150 } }
            Text {
                anchors.centerIn: parent
                width: parent.width - 24
                text: featuresBridge.statusText
                color: theme.accent
                font.pixelSize: 15
                horizontalAlignment: Text.AlignHCenter
                elide: Text.ElideRight
            }
        }

        Row {
            id: buttonRow
            width: parent.width
            height: 50
            spacing: 12
            layoutDirection: Qt.RightToLeft
            Rectangle {
                width: 120
                height: 48
                radius: 11
                color: applyMouse.pressed ? "#337ecc" : theme.accent
                Text {
                    anchors.centerIn: parent
                    text: "应用"
                    color: theme.accentText
                    font.pixelSize: 17
                    font.bold: true
                }
                MouseArea {
                    id: applyMouse
                    anchors.fill: parent
                    onClicked: featuresBridge.applyAndClose()
                }
            }
            Rectangle {
                width: 104
                height: 48
                radius: 11
                color: cancelMouse.pressed ? theme.pressed : theme.panel
                border.width: 1
                border.color: theme.border
                Text {
                    anchors.centerIn: parent
                    text: "取消"
                    color: theme.secondaryText
                    font.pixelSize: 17
                }
                MouseArea {
                    id: cancelMouse
                    anchors.fill: parent
                    onClicked: featuresBridge.cancelAndClose()
                }
            }
        }
    }
}
