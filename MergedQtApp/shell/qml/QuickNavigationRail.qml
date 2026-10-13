/* 人脸后台和系统设置共用的动画导航栏。 */
import QtQuick 2.12

Item {
    id: root
    property bool faceTheme: false
    QtObject {
        id: mediaTheme
        readonly property color panel: "#ffffff"
        readonly property color sidebar: "#f0f0f0"
        readonly property color border: "#dcdfe6"
        readonly property color text: "#303133"
        readonly property color secondaryText: "#606266"
        readonly property color accent: "#409eff"
        readonly property color hover: "#ecf5ff"
        readonly property color pressed: "#d9ecff"
        readonly property color selected: "#ecf5ff"
    }
    property var items: []
    property int currentIndex: 0
    property bool horizontal: false
    property string title: ""
    property string subtitle: ""
    property bool backVisible: false

    signal itemActivated(int index)
    signal backRequested()

    Rectangle {
        anchors.fill: parent
        color: root.faceTheme ? "#111c2d" : mediaTheme.sidebar
        border.width: 1
        border.color: root.faceTheme ? "#26364c" : mediaTheme.border
    }

    Text {
        id: titleLabel
        visible: !root.horizontal && root.title.length > 0
        anchors.left: parent.left
        anchors.leftMargin: 18
        anchors.top: parent.top
        anchors.topMargin: 20
        text: root.title
        color: root.faceTheme ? "#f8fafc" : mediaTheme.text
        font.pixelSize: 23
        font.bold: true
    }

    ListView {
        id: navigationList
        x: root.horizontal ? 12 : 10
        y: root.horizontal ? 9 : 64
        width: parent.width - x -
               (root.horizontal ? (root.backVisible ? 72 : 12) : 10)
        height: root.horizontal ? parent.height - 18 :
                Math.max(1, backButton.y - y - 14)
        orientation: root.horizontal ? ListView.Horizontal : ListView.Vertical
        spacing: 8
        clip: true
        model: root.items
        boundsBehavior: Flickable.StopAtBounds

        delegate: Rectangle {
            id: navItem
            width: root.horizontal
                   ? Math.max(112, (navigationList.width - 8 * Math.max(0, root.items.length - 1)) /
                                      Math.max(1, root.items.length))
                   : navigationList.width
            height: root.horizontal ? navigationList.height : 52
            radius: 11
            color: !root.faceTheme ? (index === root.currentIndex ? mediaTheme.selected :
                   (navMouse.pressed ? mediaTheme.pressed :
                    (navMouse.containsMouse ? mediaTheme.hover : mediaTheme.panel))) :
                   index === root.currentIndex ? "#214b72" :
                   (navMouse.pressed ? "#22334a" :
                    (navMouse.containsMouse ? "#1b2b41" : "transparent"))
            border.width: index === root.currentIndex ? 1 : 0
            border.color: root.faceTheme ? "#3b82b8" : mediaTheme.accent

            Behavior on color {
                ColorAnimation { duration: 150 }
            }
            Behavior on scale {
                NumberAnimation { duration: 100; easing.type: Easing.OutCubic }
            }
            scale: navMouse.pressed ? 0.97 : 1

            Rectangle {
                id: marker
                width: root.horizontal ? 32 : 4
                height: root.horizontal ? 4 : 28
                radius: 2
                color: root.faceTheme ? "#4ab3ff" : mediaTheme.accent
                visible: index === root.currentIndex
                x: root.horizontal ? (parent.width - width) / 2 : 0
                y: root.horizontal ? parent.height - height :
                                     (parent.height - height) / 2
            }

            Text {
                anchors.centerIn: parent
                width: parent.width - 22
                text: modelData
                color: root.faceTheme ? (index === root.currentIndex ? "#ffffff" : "#c3cfdd") :
                       (index === root.currentIndex ? mediaTheme.accent : mediaTheme.secondaryText)
                font.pixelSize: root.horizontal ? 16 : 17
                font.bold: index === root.currentIndex
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                elide: Text.ElideRight
            }

            MouseArea {
                id: navMouse
                anchors.fill: parent
                hoverEnabled: true
                onClicked: root.itemActivated(index)
            }
        }
    }

    Rectangle {
        id: backButton
        visible: root.backVisible
        width: root.horizontal ? 48 : parent.width - 20
        height: 46
        radius: 11
        x: root.horizontal ? parent.width - width - 12 : 10
        y: root.horizontal ? (parent.height - height) / 2 :
                             parent.height - height - 14
        color: !root.faceTheme ? (backMouse.pressed ? mediaTheme.pressed :
               (backMouse.containsMouse ? mediaTheme.hover : mediaTheme.panel)) :
               backMouse.pressed ? "#31435b" :
               (backMouse.containsMouse ? "#26384f" : "#1b2a3e")
        border.width: 1
        border.color: root.faceTheme ? "#3a4d66" : mediaTheme.border

        Text {
            anchors.centerIn: parent
            text: root.horizontal ? "‹" : "返回"
            color: root.faceTheme ? "#e2e8f0" : mediaTheme.text
            font.pixelSize: root.horizontal ? 32 : 17
            font.bold: true
        }

        MouseArea {
            id: backMouse
            anchors.fill: parent
            hoverEnabled: true
            onClicked: root.backRequested()
        }
    }
}
