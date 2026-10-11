import QtQuick
import QtQuick.Layouts

Rectangle {
    id: root

    property var issue: ({})
    readonly property color tone: issue.severity === "error" ? "#D94C4C"
                                  : issue.severity === "warning" ? "#C98A12" : "#0D6D90"

    implicitHeight: row.implicitHeight + 24
    radius: 16
    color: "#FFFFFF"
    border.color: root.tone
    border.width: 1

    RowLayout {
        id: row
        anchors.fill: parent
        anchors.margins: 12
        spacing: 12

        Rectangle {
            Layout.alignment: Qt.AlignTop
            Layout.preferredWidth: 22
            Layout.preferredHeight: 22
            radius: 11
            color: root.tone

            Text {
                anchors.centerIn: parent
                text: root.issue.severity === "info" ? "i" : "!"
                color: "white"
                font.pointSize: 11
                font.weight: 700
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 2

            Text {
                Layout.fillWidth: true
                text: root.issue.title || ""
                color: "#1A1C1E"
                font.pointSize: 12
                font.weight: 700
                wrapMode: Text.WordWrap
            }

            Text {
                Layout.fillWidth: true
                text: root.issue.detail || ""
                color: "#57707A"
                font.pointSize: 10
                wrapMode: Text.WordWrap
            }
        }
    }
}
