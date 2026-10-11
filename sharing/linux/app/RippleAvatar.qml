import QtQuick

Item {
  id: root

  property string name: ""
  property string mode: "off"
  property bool failed: false

  readonly property bool receiving: mode === "receive" && !failed
  readonly property bool sending: mode === "send"
  readonly property bool animating: Theme.active && !Theme.reduceMotion
  readonly property color tone: failed ? Theme.danger
      : (receiving || sending) ? Theme.accent : Theme.muted

  implicitWidth: 56
  implicitHeight: 56

  Repeater {
    model: 3
    Rectangle {
      id: ring
      required property int index
      anchors.centerIn: parent
      width: 40
      height: 40
      radius: 20
      color: "transparent"
      border.width: 2
      border.color: root.tone
      visible: root.receiving && root.animating
      opacity: 0
      scale: 1

      SequentialAnimation {
        running: ring.visible
        loops: Animation.Infinite
        PauseAnimation { duration: ring.index * 800 }
        ParallelAnimation {
          NumberAnimation {
            target: ring; property: "scale"; from: 1; to: 2.2
            duration: 2400; easing.type: Easing.OutCubic
          }
          NumberAnimation {
            target: ring; property: "opacity"; from: 0.35; to: 0
            duration: 2400; easing.type: Easing.OutCubic
          }
        }
      }
    }
  }

  Rectangle {
    anchors.centerIn: parent
    width: 52
    height: 52
    radius: 26
    color: "transparent"
    border.width: 2
    border.color: Theme.tint(root.tone, root.failed ? 0.8 : 0.35)
    visible: !root.sending && (root.failed || !root.receiving || !root.animating)
  }

  Canvas {
    id: sweep
    anchors.centerIn: parent
    width: 52
    height: 52
    visible: root.sending
    property real angle: 0
    onAngleChanged: requestPaint()
    onVisibleChanged: requestPaint()
    onPaint: {
      var ctx = getContext("2d")
      ctx.reset()
      ctx.lineWidth = 3
      ctx.lineCap = "round"
      ctx.strokeStyle = Qt.rgba(Theme.accent.r, Theme.accent.g, Theme.accent.b, 0.25)
      ctx.beginPath()
      ctx.arc(26, 26, 24, 0, Math.PI * 2)
      ctx.stroke()
      ctx.strokeStyle = Theme.accent
      ctx.beginPath()
      ctx.arc(26, 26, 24, angle, angle + Math.PI * 0.6)
      ctx.stroke()
    }
    NumberAnimation on angle {
      running: sweep.visible && root.animating
      from: 0; to: Math.PI * 2
      duration: 1400
      loops: Animation.Infinite
    }
  }

  Rectangle {
    anchors.centerIn: parent
    width: 40
    height: 40
    radius: 20
    color: Theme.tint(root.tone, 0.14)
    Text {
      anchors.centerIn: parent
      text: root.name.length > 0 ? root.name.charAt(0).toUpperCase() : "?"
      color: root.tone
      font.pixelSize: 18
      font.weight: Font.DemiBold
    }
  }

  Rectangle {
    visible: root.failed
    x: parent.width - width - 4
    y: 2
    width: 18
    height: 18
    radius: 9
    color: Theme.danger
    border.width: 2
    border.color: Theme.bg
    Text {
      anchors.centerIn: parent
      text: "!"
      color: "white"
      font.pixelSize: 12
      font.bold: true
    }
  }
}
