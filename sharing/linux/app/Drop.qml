import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
import QtQuick.Shapes
import Qt.labs.platform as Platform
import QtCore
import "."

Rectangle {
    id: dropZone
    color: "transparent"

    property real totalSize: 0
    readonly property bool hasFiles: selectedFiles.count > 0

    ListModel {
        id: selectedFiles
    }

    function addFiles(urls) {
        for (let url of urls) {
            const path = url.toString();
            let exists = false;
            for (let i = 0; i < selectedFiles.count; ++i) {
                if (selectedFiles.get(i).path === path) {
                    exists = true;
                    break;
                }
            }
            if (exists) {
                continue;
            }
            const size = backend.fileSize(path);
            selectedFiles.append({
                path: path,
                name: decodeURIComponent(path).split("/").pop(),
                size: size
            });
            totalSize += size;
        }
    }

    function removeFile(index) {
        totalSize -= selectedFiles.get(index).size;
        selectedFiles.remove(index);
    }

    function shareSelected() {
        let paths = [];
        for (let i = 0; i < selectedFiles.count; ++i) {
            paths.push(selectedFiles.get(i).path);
        }
        selectedFiles.clear();
        totalSize = 0;
        EventBus.filesSelected(paths);
    }
    Platform.FileDialog {
        id: fileDialog
        title: "Choose files to share"
        fileMode: Platform.FileDialog.OpenFiles
        folder: StandardPaths.writableLocation(StandardPaths.HomeLocation)

        nameFilters: ["All files (*)"]

        onAccepted: {
            dropZone.addFiles(fileDialog.files);
        }
        onRejected: {
            console.log("File picker canceled");
        }
    }
    DropArea {
        anchors.fill: parent

        onExited: {
            dropText.text = "Drag and drop files to share";
            dropZone.color = "transparent";
        }
        onEntered: k => {
            console.log("Entered");
        }

        // The action: What happens when the user releases the item here

        onDropped: drop => {
            drop.acceptProposedAction();
            if (drop.hasUrls) {
                dropZone.addFiles(drop.urls);
            }
        }
    }
    ColumnLayout {

        visible: !dropZone.hasFiles
        anchors.centerIn: parent
        Button {
            icon.source: "qrc:icons/up_file.svg"
            icon.height: 80
            icon.width: 80
            icon.color: "#195871"
            Layout.fillWidth: true
            background: Rectangle {
                color: "transparent"
            }
        }

        Text {
            id: dropText
            text: "Drag and drop files to share"
            font.pointSize: 18
            color: "#333333"
        }
        Text {
            id: subText
            text: "Click to open file picker"
            font.pointSize: 12
            color: "gray"
            horizontalAlignment: Text.AlignHCenter
            Layout.fillWidth: true
        }
    }

    MouseArea {
        anchors.fill: parent
        visible: !dropZone.hasFiles
        cursorShape: Qt.PointingHandCursor // Changes cursor to a hand on hover
        onClicked: {
            fileDialog.open(); // Opens the native OS file picker
        }
    }
    Canvas {
        id: dashedBorderCanvas
        anchors.fill: parent
        visible: !dropZone.hasFiles

        onPaint: {
            var ctx = getContext("2d");
            ctx.clearRect(0, 0, width, height);

            // Setup styling parameters
            ctx.strokeStyle = "#91C8DE";
            ctx.lineWidth = 3;

            // Set the dash pattern array: [length of dash, length of space]
            ctx.setLineDash([8, 6]);

            // Draw a rectangle border (X, Y, Width, Height)
            // Offset by half line-width (1px) so the line doesn't get clipped on the edges
            ctx.strokeRect(80, 80, width - 160, height - 160);
        }
    }

    ColumnLayout {
        visible: dropZone.hasFiles
        anchors.centerIn: parent
        width: Math.min(parent.width - 64, 520)
        spacing: 16

        Text {
            Layout.fillWidth: true
            text: selectedFiles.count === 1 ? "1 file selected" : selectedFiles.count + " files selected"
            font.pointSize: 18
            font.weight: 600
            color: "#377B95"
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(selectedFiles.count * 48, 240) + 12
            radius: 20
            color: "#DCF5FF"
            border.color: "#91C8DE"
            border.width: 2

            ListView {
                anchors.fill: parent
                anchors.margins: 6
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                model: selectedFiles
                ScrollBar.vertical: ScrollBar {}

                delegate: RowLayout {
                    id: fileRow
                    required property int index
                    required property string name
                    required property real size
                    width: ListView.view.width
                    height: 48
                    spacing: 12

                    ColumnLayout {
                        Layout.fillWidth: true
                        Layout.leftMargin: 12
                        spacing: 0

                        Text {
                            Layout.fillWidth: true
                            text: fileRow.name
                            elide: Text.ElideMiddle
                            color: "#1A1C1E"
                            font.pointSize: 12
                        }
                        Text {
                            text: backend.formatSize(fileRow.size)
                            color: "gray"
                            font.pointSize: 10
                        }
                    }

                    Button {
                        id: removeButton
                        Layout.preferredWidth: 32
                        Layout.preferredHeight: 32
                        onClicked: dropZone.removeFile(fileRow.index)
                        contentItem: Text {
                            text: "×"
                            color: "#57707A"
                            font.pointSize: 16
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }
                        background: Rectangle {
                            radius: 8
                            color: removeButton.hovered ? "#BCE5F5" : "transparent"
                        }
                    }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 10

            Text {
                Layout.fillWidth: true
                text: "Total " + backend.formatSize(dropZone.totalSize)
                color: "gray"
                font.pointSize: 12
            }

            Button {
                id: addButton
                text: "Add more"
                onClicked: fileDialog.open()
                contentItem: Text {
                    text: addButton.text
                    font.pointSize: 14
                    font.weight: 500
                    color: "#06384C"
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle {
                    implicitWidth: 100
                    implicitHeight: 40
                    radius: 10
                    color: addButton.hovered ? "#BCE5F5" : "transparent"
                    border.color: "#91C8DE"
                    border.width: 2
                }
            }

            Button {
                id: shareButton
                text: "Share"
                onClicked: dropZone.shareSelected()
                contentItem: Text {
                    text: shareButton.text
                    font.pointSize: 14
                    font.weight: 500
                    color: "white"
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle {
                    implicitWidth: 100
                    implicitHeight: 40
                    radius: 10
                    color: shareButton.hovered ? "#195871" : "#06384C"
                    border.color: "#91C8DE"
                    border.width: 2
                }
            }
        }
    }
}
