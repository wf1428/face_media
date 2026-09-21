/*
 * 单一、长期存活的 Quick 覆盖层宿主。
 * 视频页面正常运行时其对应 QQuickWidget 完全隐藏；弹窗显示期间才参与合成。
 */
import QtQuick 2.12

Item {
    id: root
    objectName: "quickSceneHostRoot"
    // SizeRootObjectToView / C++ 宿主提供尺寸，根节点不能再锚定 parent。
    width: 0
    height: 0
    QtObject {
        id: mediaTheme
        readonly property color panel: "#ffffff"
        readonly property color border: "#dcdfe6"
        readonly property color text: "#303133"
        readonly property color secondaryText: "#606266"
        readonly property color accent: "#409eff"
        readonly property color hover: "#ecf5ff"
        readonly property color pressed: "#d9ecff"
        readonly property color accentText: "#ffffff"
    }
    readonly property bool faceTheme: uiRuntime.dialogFaceTheme

    readonly property color accentColor: {
        switch (uiRuntime.dialogType) {
        case "success": return root.faceTheme ? "#22c55e" : "#67c23a"
        case "warning": return root.faceTheme ? "#f59e0b" : "#e6a23c"
        case "error": return root.faceTheme ? "#ef4444" : "#f56c6c"
        case "question": return root.faceTheme ? "#38bdf8" : mediaTheme.accent
        default: return root.faceTheme ? "#60a5fa" : mediaTheme.accent
        }
    }

    // 每个已迁移页面只在这一处装载。keepAlive 页面创建一次后长期复用；
    // 非常驻页面只在成为当前路由时装载，便于后续按资源体量选择策略。
    Rectangle {
        id: pageBackground
        anchors.fill: parent
        color: "#000000"
        visible: uiRuntime.quickPageActive
    }

    Repeater {
        id: pageRepeater
        model: uiRuntime.moduleCatalog

        Loader {
            anchors.fill: parent
            readonly property string pageSource: modelData.source
            readonly property bool isQuickPage:
                pageSource.length > 0 && modelData.layer !== 6
            active: isQuickPage &&
                    (modelData.keepAlive || modelData.id === uiRuntime.activeModuleId)
            source: isQuickPage ? pageSource : ""
            visible: status === Loader.Ready &&
                     modelData.id === uiRuntime.activeModuleId
            asynchronous: true
        }
    }

    Rectangle {
        id: dimmer
        z: 30
        anchors.fill: parent
        color: "#99000000"
        opacity: uiRuntime.dialogVisible ? 1 : 0
        visible: uiRuntime.dialogVisible

        Behavior on opacity {
            NumberAnimation { duration: 150; easing.type: Easing.OutCubic }
        }

        MouseArea {
            anchors.fill: parent
            // 保持原模态弹窗行为：点击遮罩不自动取消，避免误操作。
            onClicked: mouse.accepted = true
        }
    }

    Rectangle {
        id: card
        objectName: "quickDialogCard"
        z: 31
        width: Math.max(1, Math.min(root.width - 48, 560))
        height: Math.max(1, Math.min(root.height - 48,
                         Math.max(250, contentColumn.implicitHeight + 44)))
        anchors.centerIn: parent
        radius: 16
        color: root.faceTheme ? "#17212b" : mediaTheme.panel
        border.width: 1
        border.color: root.faceTheme ? "#334155" : mediaTheme.border
        visible: uiRuntime.dialogVisible
        opacity: visible ? 1 : 0
        scale: visible ? 1 : 0.94

        Behavior on opacity {
            NumberAnimation { duration: 170; easing.type: Easing.OutCubic }
        }
        Behavior on scale {
            NumberAnimation { duration: 190; easing.type: Easing.OutBack }
        }

        Column {
            id: contentColumn
            x: 22
            y: 22
            width: Math.max(1, parent.width - 44)
            spacing: 16

            Row {
                width: parent.width
                height: 46
                spacing: 13

                Rectangle {
                    width: 44
                    height: 44
                    radius: 12
                    color: Qt.rgba(root.accentColor.r,
                                   root.accentColor.g,
                                   root.accentColor.b, 0.16)

                    Image {
                        anchors.centerIn: parent
                        width: 25
                        height: 25
                        source: uiRuntime.dialogIconSource
                        sourceSize.width: 25
                        sourceSize.height: 25
                    }
                }

                Text {
                    width: parent.width - closeButton.width - 44 - parent.spacing * 2
                    height: parent.height
                    text: uiRuntime.dialogTitle
                    color: root.faceTheme ? "#f8fafc" : mediaTheme.text
                    font.pixelSize: 22
                    font.bold: true
                    verticalAlignment: Text.AlignVCenter
                    elide: Text.ElideRight
                }

                Rectangle {
                    id: closeButton
                    width: 40
                    height: 40
                    radius: 10
                    color: !root.faceTheme ? (closeMouse.pressed ? mediaTheme.pressed :
                           (closeMouse.containsMouse ? mediaTheme.hover : "transparent")) :
                           closeMouse.pressed ? "#334155" :
                           (closeMouse.containsMouse ? "#263445" : "transparent")

                    Text {
                        anchors.centerIn: parent
                        text: "×"
                        color: root.faceTheme ? "#cbd5e1" : mediaTheme.secondaryText
                        font.pixelSize: 28
                    }

                    MouseArea {
                        id: closeMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: uiRuntime.rejectDialog()
                    }
                }
            }

            Flickable {
                width: parent.width
                // 始终先给标题和全部按钮留出空间，长结果仅滚动正文。
                height: Math.min(Math.max(messageText.implicitHeight, 72),
                                 Math.max(1, root.height - 48 - 44 - 46 - 32 - buttons.implicitHeight))
                contentWidth: width
                contentHeight: messageText.implicitHeight
                clip: true

                Text {
                    id: messageText
                    width: parent.width
                    text: uiRuntime.dialogMessage
                    color: root.faceTheme ? "#d6dee8" : mediaTheme.secondaryText
                    font.pixelSize: 18
                    lineHeight: 1.22
                    wrapMode: Text.Wrap
                    textFormat: Text.PlainText
                }
            }

            Flow {
                id: buttons
                width: parent.width
                spacing: 12
                layoutDirection: Qt.RightToLeft

                Repeater {
                    model: uiRuntime.dialogChoices.length > 0
                           ? uiRuntime.dialogChoices : []

                    Rectangle {
                        width: Math.max(104, choiceLabel.implicitWidth + 34)
                        height: 48
                        radius: 10
                        color: choiceMouse.pressed ? Qt.darker(root.accentColor, 1.18)
                                                   : root.accentColor

                        Text {
                            id: choiceLabel
                            anchors.centerIn: parent
                            text: modelData
                            color: root.faceTheme ? "#07111b" : mediaTheme.accentText
                            font.pixelSize: 17
                            font.bold: true
                        }

                        MouseArea {
                            id: choiceMouse
                            anchors.fill: parent
                            onClicked: uiRuntime.acceptDialog(index)
                        }
                    }
                }

                Rectangle {
                    width: Math.max(112, acceptLabel.implicitWidth + 34)
                    height: 48
                    radius: 10
                    color: acceptMouse.pressed ? Qt.darker(root.accentColor, 1.18)
                                               : root.accentColor
                    visible: uiRuntime.dialogChoices.length === 0

                    Text {
                        id: acceptLabel
                        anchors.centerIn: parent
                        text: uiRuntime.dialogAcceptText
                        color: root.faceTheme ? "#07111b" : mediaTheme.accentText
                        font.pixelSize: 17
                        font.bold: true
                    }

                    MouseArea {
                        id: acceptMouse
                        anchors.fill: parent
                        onClicked: uiRuntime.acceptDialog(-1)
                    }
                }

                Rectangle {
                    width: Math.max(104, cancelLabel.implicitWidth + 34)
                    height: 48
                    radius: 10
                    color: root.faceTheme ? (cancelMouse.pressed ? "#334155" : "#253344") :
                           (cancelMouse.pressed ? mediaTheme.pressed : mediaTheme.panel)
                    border.width: 1
                    border.color: root.faceTheme ? "#475569" : mediaTheme.border
                    visible: uiRuntime.dialogCancelText.length > 0

                    Text {
                        id: cancelLabel
                        anchors.centerIn: parent
                        text: uiRuntime.dialogCancelText
                        color: root.faceTheme ? "#e2e8f0" : mediaTheme.secondaryText
                        font.pixelSize: 17
                    }

                    MouseArea {
                        id: cancelMouse
                        anchors.fill: parent
                        onClicked: uiRuntime.rejectDialog()
                    }
                }
            }
        }
    }

    Rectangle {
        id: toast
        z: 20
        width: Math.min(root.width - 48,
                        Math.max(260, toastText.implicitWidth + 54))
        height: Math.max(54, toastText.implicitHeight + 24)
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: Math.max(30, root.height * 0.07)
        radius: height / 2
        color: uiRuntime.toastFaceTheme ? (uiRuntime.toastSuccess ? "#e6226c4d" : "#e6b4232f") :
               (uiRuntime.toastSuccess ? "#f0f9eb" : "#fef0f0")
        border.width: 1
        border.color: uiRuntime.toastFaceTheme ? (uiRuntime.toastSuccess ? "#49d68b" : "#fb7185") :
                      (uiRuntime.toastSuccess ? "#c2e7b0" : "#fbc4c4")
        visible: uiRuntime.toastVisible
        opacity: visible ? 1 : 0
        scale: visible ? 1 : 0.92

        Behavior on opacity { NumberAnimation { duration: 160 } }
        Behavior on scale {
            NumberAnimation { duration: 180; easing.type: Easing.OutBack }
        }

        Text {
            id: toastText
            anchors.centerIn: parent
            width: Math.min(500, root.width - 92)
            text: uiRuntime.toastText
            color: uiRuntime.toastFaceTheme ? "#ffffff" :
                   (uiRuntime.toastSuccess ? "#529b2e" : "#c45656")
            font.pixelSize: 17
            font.bold: true
            wrapMode: Text.Wrap
            horizontalAlignment: Text.AlignHCenter
        }
    }

    Rectangle {
        id: progressDimmer
        z: 14
        anchors.fill: parent
        color: "#b0020617"
        visible: uiRuntime.progressVisible
        opacity: visible ? 1 : 0
        Behavior on opacity { NumberAnimation { duration: 150 } }
        MouseArea {
            anchors.fill: parent
        }
    }

    Rectangle {
        id: progressCard
        z: 15
        width: Math.min(root.width - 48, 500)
        height: 260
        anchors.centerIn: parent
        radius: 18
        color: "#0f172a"
        border.width: 1
        border.color: "#475569"
        visible: uiRuntime.progressVisible
        opacity: visible ? 1 : 0
        scale: visible ? 1 : 0.94

        Behavior on opacity { NumberAnimation { duration: 170 } }
        Behavior on scale {
            NumberAnimation { duration: 190; easing.type: Easing.OutBack }
        }

        Column {
            anchors.fill: parent
            anchors.margins: 34
            spacing: 24

            Text {
                width: parent.width
                text: uiRuntime.progressTitle
                color: "#f8fafc"
                font.pixelSize: 24
                font.bold: true
                horizontalAlignment: Text.AlignHCenter
            }

            Item {
                width: 72
                height: 72
                anchors.horizontalCenter: parent.horizontalCenter

                Rectangle {
                    anchors.fill: parent
                    radius: width / 2
                    color: "transparent"
                    border.width: 6
                    border.color: "#294052"
                }
                Item {
                    id: spinnerArm
                    anchors.fill: parent
                    transformOrigin: Item.Center

                    Rectangle {
                        width: 18
                        height: 18
                        radius: 9
                        x: 27
                        y: -2
                        color: "#2dd4bf"
                    }

                    RotationAnimator on rotation {
                        from: 0
                        to: 360
                        duration: 850
                        loops: Animation.Infinite
                        running: progressCard.visible
                    }
                }
            }

            Rectangle {
                width: parent.width
                height: 24
                radius: 12
                color: "#1e293b"
                border.width: 1
                border.color: "#475569"
                Rectangle {
                    width: parent.width * uiRuntime.progressValue / 100
                    height: parent.height
                    radius: parent.radius
                    color: "#2dd4bf"
                    Behavior on width {
                        NumberAnimation { duration: 180; easing.type: Easing.OutCubic }
                    }
                }
                Text {
                    anchors.centerIn: parent
                    text: uiRuntime.progressValue + "%"
                    color: "#ffffff"
                    font.pixelSize: 14
                    font.bold: true
                }
            }
        }
    }
}
