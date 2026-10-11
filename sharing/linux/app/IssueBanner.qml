import QtQuick
import QtQuick.Layouts

Item {
  id: root

  property var issue: ({})
  readonly property color tone: issue.severity === "error" ? Theme.danger
      : issue.severity === "warning" ? Theme.warning : Theme.accent
  readonly property string glyph: issue.severity === "info" ? "i" : "!"

  implicitHeight: card.implicitHeight + 8

  Rectangle {
    id: card
    width: parent.width
    implicitHeight: row.implicitHeight + 24
    radius: Theme.cardRadius
    color: Theme.tint(root.tone, 0.1)
    border.width: 1
    border.color: Theme.tint(root.tone, 0.25)

    RowLayout {
      id: row
      anchors.fill: parent
      anchors.margins: 12
      spacing: 10

      Rectangle {
        Layout.alignment: Qt.AlignTop
        Layout.preferredWidth: 22
        Layout.preferredHeight: 22
        radius: 11
        color: root.tone
        Text {
          anchors.centerIn: parent
          text: root.glyph
          color: "white"
          font.pixelSize: 13
          font.bold: true
        }
      }
      ColumnLayout {
        Layout.fillWidth: true
        spacing: 2
        Text {
          Layout.fillWidth: true
          text: root.issue.title || ""
          color: Theme.ink
          font.pixelSize: 14
          font.bold: true
          wrapMode: Text.WordWrap
        }
        Text {
          Layout.fillWidth: true
          text: root.issue.detail || ""
          color: Theme.muted
          font.pixelSize: 12
          wrapMode: Text.WordWrap
        }
      }
    }
  }
}
