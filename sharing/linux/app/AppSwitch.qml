import QtQuick
import QtQuick.Controls

Switch {
  id: control

  implicitWidth: 48
  implicitHeight: 28
  hoverEnabled: true
  padding: 0

  indicator: Rectangle {
    anchors.centerIn: parent
    width: 44
    height: 26
    radius: 13
    color: control.checked ? Theme.accent : Theme.tint(Theme.ink, 0.16)
    opacity: control.enabled ? 1 : 0.45
    border.width: control.visualFocus ? 2 : 0
    border.color: Theme.accent
    Behavior on color { ColorAnimation { duration: 150 } }

    Rectangle {
      y: 3
      x: control.checked ? parent.width - width - 3 : 3
      width: 20
      height: 20
      radius: 10
      color: control.checked ? Theme.onAccent : Theme.surface
      Behavior on x {
        NumberAnimation {
          duration: Theme.reduceMotion ? 0 : 180
          easing.type: Easing.Bezier
          easing.bezierCurve: Theme.enterCurve
        }
      }
    }
  }
  contentItem: Item {}
}
