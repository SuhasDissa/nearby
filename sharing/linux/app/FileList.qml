import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
  id: root

  property alias model: list.model
  property string summary: ""
  signal removeRequested(int index)
  signal addRequested
  signal cancelRequested

  implicitHeight: Math.min(list.contentHeight, 224) + footer.implicitHeight + 25
  radius: Theme.cardRadius
  color: Theme.surface
  border.width: 1
  border.color: Theme.hairline
  Behavior on implicitHeight {
    NumberAnimation {
      duration: Theme.reduceMotion ? 0 : 200
      easing.type: Easing.Bezier
      easing.bezierCurve: Theme.enterCurve
    }
  }

  ListView {
    id: list
    anchors.left: parent.left
    anchors.right: parent.right
    anchors.top: parent.top
    anchors.margins: 6
    height: Math.min(contentHeight, 224)
    clip: true
    boundsBehavior: Flickable.StopAtBounds
    ScrollBar.vertical: ScrollBar {}

    add: Transition {
      NumberAnimation { property: "opacity"; from: 0; to: 1; duration: 220 }
      NumberAnimation {
        property: "y"; from: Theme.reduceMotion ? 0 : 8; duration: 220
        easing.type: Easing.Bezier; easing.bezierCurve: Theme.enterCurve
      }
    }
    remove: Transition {
      NumberAnimation { property: "opacity"; to: 0; duration: 160 }
    }
    displaced: Transition {
      NumberAnimation {
        property: "y"; duration: Theme.reduceMotion ? 0 : 200
        easing.type: Easing.Bezier; easing.bezierCurve: Theme.enterCurve
      }
    }

    delegate: Item {
      id: row
      required property int index
      required property string name
      required property string sizeText
      required property string iconName
      width: list.width - 4
      height: 48

      RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 8
        anchors.rightMargin: 4
        spacing: 12

        Rectangle {
          Layout.preferredWidth: 32
          Layout.preferredHeight: 32
          radius: 8
          color: Theme.tint(Theme.accent, 0.12)
          Text {
            anchors.centerIn: parent
            visible: row.iconName.length === 0
            text: {
              var dot = row.name.lastIndexOf(".")
              return dot > 0 ? row.name.substring(dot + 1, dot + 4).toUpperCase() : "FILE"
            }
            color: Theme.accent
            font.pixelSize: 10
            font.bold: true
          }
          AbstractButton {
            anchors.centerIn: parent
            visible: row.iconName.length > 0
            focusPolicy: Qt.NoFocus
            hoverEnabled: false
            padding: 0
            display: AbstractButton.IconOnly
            icon.name: row.iconName
            icon.color: Theme.accent
            icon.width: 20
            icon.height: 20
            background: null
          }
        }
        ColumnLayout {
          Layout.fillWidth: true
          spacing: 0
          Text {
            Layout.fillWidth: true
            text: row.name
            elide: Text.ElideMiddle
            color: Theme.ink
            font.pixelSize: 14
          }
          Text {
            text: row.sizeText
            color: Theme.muted
            font.pixelSize: 12
            font.features: { "tnum": 1 }
          }
        }
        AbstractButton {
          id: remove
          Layout.preferredWidth: 32
          Layout.preferredHeight: 32
          hoverEnabled: true
          onClicked: root.removeRequested(row.index)
          Accessible.name: "Remove " + row.name
          contentItem: Text {
            text: "×"
            color: Theme.muted
            font.pixelSize: 20
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
          }
          background: Rectangle {
            radius: 8
            color: remove.hovered || remove.down ? Theme.tint(Theme.ink, 0.08) : "transparent"
            border.width: remove.visualFocus ? 2 : 0
            border.color: Theme.accent
          }
        }
      }
    }
  }

  RowLayout {
    id: footer
    anchors.left: parent.left
    anchors.right: parent.right
    anchors.bottom: parent.bottom
    anchors.margins: 10
    anchors.topMargin: 0
    spacing: 8
    Text {
      Layout.fillWidth: true
      text: root.summary
      color: Theme.muted
      font.pixelSize: 12
      font.features: { "tnum": 1 }
    }
    AppButton { text: "Add more"; onClicked: root.addRequested() }
    AppButton { text: "Cancel"; danger: true; onClicked: root.cancelRequested() }
  }
}
