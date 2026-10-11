import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Popup {
  id: root

  property var info: ({})
  signal accepted
  signal declined

  parent: Overlay.overlay
  anchors.centerIn: parent
  width: Math.min(parent.width - 40, 400)
  modal: true
  padding: 20
  closePolicy: Popup.NoAutoClose
  transformOrigin: Item.Center

  enter: Transition {
    NumberAnimation { property: "opacity"; from: 0; to: 1; duration: 180 }
    NumberAnimation {
      property: "scale"; from: Theme.reduceMotion ? 1 : 0.96; to: 1; duration: 180
      easing.type: Easing.Bezier; easing.bezierCurve: Theme.enterCurve
    }
  }
  exit: Transition {
    NumberAnimation { property: "opacity"; to: 0; duration: 140 }
    NumberAnimation { property: "scale"; to: Theme.reduceMotion ? 1 : 0.98; duration: 140 }
  }

  Overlay.modal: Rectangle { color: Theme.tint("black", Theme.dark ? 0.55 : 0.35) }

  background: Rectangle {
    radius: 18
    color: Theme.surface
    border.width: 1
    border.color: Theme.hairline
  }

  contentItem: ColumnLayout {
    spacing: 14

    Text {
      Layout.fillWidth: true
      text: (root.info.name || "A device") + " wants to share"
      color: Theme.ink
      font.pixelSize: 20
      font.weight: Font.DemiBold
      font.letterSpacing: -0.2
      wrapMode: Text.WordWrap
    }

    ListView {
      id: files
      Layout.fillWidth: true
      Layout.preferredHeight: Math.min(contentHeight, 5 * 32)
      clip: true
      model: root.info.fileList || []
      boundsBehavior: Flickable.StopAtBounds
      ScrollBar.vertical: ScrollBar {}
      delegate: Text {
        required property string modelData
        width: files.width
        height: 32
        text: modelData
        elide: Text.ElideMiddle
        verticalAlignment: Text.AlignVCenter
        color: Theme.ink
        font.pixelSize: 14
      }
    }

    Text {
      text: root.info.totalText || ""
      color: Theme.muted
      font.pixelSize: 12
      font.features: { "tnum": 1 }
    }

    Rectangle {
      visible: (root.info.token || "").length > 0
      Layout.fillWidth: true
      Layout.preferredHeight: pin.implicitHeight + 40
      radius: Theme.cardRadius
      color: Theme.tint(Theme.accent, 0.08)
      ColumnLayout {
        anchors.centerIn: parent
        spacing: 4
        Text {
          id: pin
          Layout.alignment: Qt.AlignHCenter
          text: root.info.token || ""
          color: Theme.accent
          font.pixelSize: 28
          font.weight: Font.DemiBold
          font.letterSpacing: 4
          font.features: { "tnum": 1 }
        }
        Text {
          Layout.alignment: Qt.AlignHCenter
          text: "Check this code matches the other device"
          color: Theme.muted
          font.pixelSize: 12
        }
      }
    }

    RowLayout {
      Layout.fillWidth: true
      spacing: 10
      Item { Layout.fillWidth: true }
      AppButton { text: "Decline"; onClicked: root.declined() }
      AppButton { text: "Accept"; primary: true; onClicked: root.accepted() }
    }
  }
}
