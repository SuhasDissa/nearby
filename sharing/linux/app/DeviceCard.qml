import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ItemDelegate {
  id: root

  required property string deviceName
  property bool receiveDisabled: false
  signal sendRequested

  height: 64
  padding: 0
  hoverEnabled: true
  enabled: !receiveDisabled
  onClicked: sendRequested()
  scale: down ? 0.985 : 1
  Behavior on scale { NumberAnimation { duration: 120; easing.type: Easing.OutCubic } }

  background: Rectangle {
    anchors.fill: parent
    anchors.topMargin: 3
    anchors.bottomMargin: 3
    radius: Theme.cardRadius
    color: root.hovered && root.enabled
        ? Qt.tint(Theme.surface, Theme.tint(Theme.accent, 0.06)) : Theme.surface
    border.width: root.visualFocus ? 2 : 1
    border.color: root.visualFocus ? Theme.accent : Theme.hairline
    Behavior on color { ColorAnimation { duration: 120 } }
  }

  contentItem: RowLayout {
    spacing: 12
    opacity: root.enabled ? 1 : 0.55

    Rectangle {
      Layout.leftMargin: 14
      Layout.preferredWidth: 36
      Layout.preferredHeight: 36
      radius: 18
      color: Theme.tint(Theme.accent, 0.14)
      Text {
        anchors.centerIn: parent
        text: root.deviceName.length > 0 ? root.deviceName.charAt(0).toUpperCase() : "?"
        color: Theme.accent
        font.pixelSize: 16
        font.weight: Font.DemiBold
      }
    }
    Text {
      Layout.fillWidth: true
      text: root.deviceName
      elide: Text.ElideRight
      color: Theme.ink
      font.pixelSize: 14
    }
    Rectangle {
      Layout.rightMargin: 14
      Layout.preferredWidth: label.implicitWidth + 24
      Layout.preferredHeight: 28
      radius: 8
      color: root.receiveDisabled ? Theme.tint(Theme.muted, 0.14) : Theme.tint(Theme.accent, 0.12)
      Text {
        id: label
        anchors.centerIn: parent
        text: root.receiveDisabled ? "Busy" : "Send"
        color: root.receiveDisabled ? Theme.muted : Theme.accent
        font.pixelSize: 13
        font.weight: Font.DemiBold
      }
    }
  }
}
