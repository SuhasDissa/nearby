import QtQuick
import QtQuick.Controls

Button {
  id: control

  property bool primary: false
  property bool danger: false
  property color tone: danger ? Theme.danger : Theme.accent

  implicitHeight: 36
  implicitWidth: Math.max(72, contentItem.implicitWidth + 28)
  hoverEnabled: true
  scale: down ? 0.97 : 1
  Behavior on scale {
    NumberAnimation { duration: 120; easing.type: Easing.OutCubic }
  }

  HoverHandler { id: hover }

  contentItem: Text {
    text: control.text
    font.pixelSize: 14
    font.weight: Font.DemiBold
    color: !control.enabled ? Theme.muted
         : control.primary ? Theme.onAccent
         : control.tone
    horizontalAlignment: Text.AlignHCenter
    verticalAlignment: Text.AlignVCenter
    elide: Text.ElideRight
    opacity: control.enabled ? 1 : 0.6
  }

  background: Rectangle {
    radius: Theme.controlRadius
    color: control.primary
        ? (control.enabled ? control.tone : Theme.tint(Theme.muted, 0.3))
        : Theme.tint(control.tone, !control.enabled ? 0.04
                                  : control.down ? 0.2
                                  : hover.hovered ? 0.14 : 0.08)
    border.width: control.visualFocus ? 2 : 0
    border.color: Theme.accent
    Behavior on color { ColorAnimation { duration: 120 } }
  }
}
