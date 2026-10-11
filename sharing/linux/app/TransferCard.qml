import QtQuick
import QtQuick.Layouts

Rectangle {
  id: root

  property int status: 0
  property bool incoming: false
  property string peer: ""
  property string message: ""
  property real progress: 0
  property string speedText: ""
  property string sizeText: ""
  signal cancelRequested
  signal closeRequested

  readonly property bool complete: status === 5
  readonly property bool failed: status === 6 || status === 7 || status === 8 || status === 9
  readonly property bool active: !complete && !failed
  readonly property color tone: complete ? Theme.success : failed ? Theme.danger : Theme.accent

  implicitHeight: column.implicitHeight + 32
  radius: Theme.cardRadius
  color: complete || failed
      ? Qt.tint(Theme.surface, Theme.tint(tone, 0.1)) : Theme.surface
  border.width: 1
  border.color: complete || failed ? Theme.tint(tone, 0.3) : Theme.hairline

  onFailedChanged: if (failed && !Theme.reduceMotion) shake.restart()
  onCompleteChanged: if (complete) check.reveal()

  transform: Translate { id: offset }
  SequentialAnimation {
    id: shake
    loops: 3
    NumberAnimation { target: offset; property: "x"; to: -6; duration: 50 }
    NumberAnimation { target: offset; property: "x"; to: 6; duration: 50 }
    onFinished: offset.x = 0
  }

  ColumnLayout {
    id: column
    anchors.fill: parent
    anchors.margins: 16
    spacing: 10

    RowLayout {
      Layout.fillWidth: true
      spacing: 10
      Rectangle {
        id: check
        visible: root.complete
        Layout.preferredWidth: 24
        Layout.preferredHeight: 24
        radius: 12
        color: Theme.success
        opacity: 1
        function reveal() {
          if (!Theme.reduceMotion) {
            checkAnim.restart()
          }
        }
        ParallelAnimation {
          id: checkAnim
          NumberAnimation { target: check; property: "scale"; from: 0.8; to: 1; duration: 220
            easing.type: Easing.Bezier; easing.bezierCurve: Theme.enterCurve }
          NumberAnimation { target: check; property: "opacity"; from: 0; to: 1; duration: 220 }
        }
        Text {
          anchors.centerIn: parent
          text: "✓"
          color: "white"
          font.pixelSize: 14
          font.bold: true
        }
      }
      Text {
        Layout.fillWidth: true
        text: root.complete ? "Done"
            : root.failed ? root.message
            : (root.incoming ? "Receiving from " : "Sending to ") + (root.peer.length > 0 ? root.peer : "device")
        color: root.failed ? Theme.danger : Theme.ink
        font.pixelSize: 14
        font.bold: true
        elide: Text.ElideRight
        wrapMode: root.failed ? Text.WordWrap : Text.NoWrap
      }
      AppButton {
        visible: root.active
        text: "Cancel"
        danger: true
        onClicked: root.cancelRequested()
      }
      AppButton {
        visible: !root.active
        text: "Close"
        tone: root.tone
        onClicked: root.closeRequested()
      }
    }

    Rectangle {
      visible: root.active
      Layout.fillWidth: true
      Layout.preferredHeight: 6
      radius: 3
      color: Theme.tint(Theme.ink, 0.1)
      Rectangle {
        height: parent.height
        radius: 3
        color: Theme.accent
        width: parent.width * Math.max(0, Math.min(100, root.progress)) / 100
        Behavior on width { NumberAnimation { duration: 250; easing.type: Easing.OutCubic } }
      }
    }

    RowLayout {
      visible: root.active
      Layout.fillWidth: true
      Text {
        text: root.status === 4 ? Math.round(root.progress) + "%" : root.message
        color: Theme.ink
        font.pixelSize: 12
        font.features: { "tnum": 1 }
        elide: Text.ElideRight
        Layout.fillWidth: root.status !== 4
      }
      Text {
        visible: root.status === 4 && root.speedText.length > 0
        text: root.speedText
        color: Theme.muted
        font.pixelSize: 12
        font.features: { "tnum": 1 }
      }
      Item { Layout.fillWidth: true; visible: root.status === 4 }
      Text {
        visible: root.status === 4
        text: root.sizeText
        color: Theme.muted
        font.pixelSize: 12
        font.features: { "tnum": 1 }
      }
    }
  }
}
