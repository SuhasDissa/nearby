import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
  id: window

  property var currentId: 0
  property var outgoingId: 0
  property bool startFailed: false
  property real totalSize: 0

  readonly property bool sendMode: fileModel.count > 0
  readonly property bool outgoingActive: outgoingId !== 0
  readonly property string mode: sendMode ? "send"
      : (backend.receiveEnabled || currentId !== 0) ? "receive" : "off"
  readonly property string statusText: sendMode
      ? (outgoingActive ? "Transferring" : "Choose a device to send to")
      : backend.receiveEnabled ? "Visible to nearby devices"
      : "Visible when a nearby device starts sharing"
  readonly property string selectedSizeText: {
    var count = fileModel.count
    return count + (count === 1 ? " file" : " files") + " · " + backend.formatSize(totalSize)
  }
  readonly property bool advertisingFailed: {
    for (var i = 0; i < backend.issues.length; ++i) {
      if (backend.issues[i].kind === "advertising") {
        return true
      }
    }
    return false
  }

  Component.onCompleted: Theme.sync(issueModel, backend.issues, "kind")

  function statusCode(name) {
    switch (name) {
    case "kConnecting": return 1
    case "kAwaitingLocalConfirmation": return 2
    case "kAwaitingRemoteAcceptance": return 3
    case "kInProgress": return 4
    case "kComplete": return 5
    case "kFailed": return 6
    case "kReject": return 7
    case "kCancelled": return 8
    case "kTimedOut": return 9
    case "": case "kUnknown": return 0
    default: return 6
    }
  }

  function statusMessage(code, send, done, total) {
    switch (code) {
    case 1: return send ? "Connecting" : "Preparing transfer"
    case 2: return "Incoming share request"
    case 3: return "Waiting for receiver"
    case 4: return (send ? "Sending" : "Receiving") + (total > 0
        ? " " + Math.min(done + 1, total) + " of " + total + " items" : "")
    case 7: return "Rejected"
    case 8: return "Cancelled"
    case 9: return "Timed out"
    case 6: return "Transfer failed"
    default: return send ? "Starting send" : "Preparing transfer"
    }
  }

  function addFiles(urls) {
    var wasEmpty = fileModel.count === 0
    for (var i = 0; i < urls.length; ++i) {
      var path = urls[i].toString()
      var exists = false
      for (var j = 0; j < fileModel.count; ++j) {
        if (fileModel.get(j).path === path) {
          exists = true
          break
        }
      }
      if (exists) {
        continue
      }
      var size = backend.fileSize(path)
      fileModel.append({
        path: path,
        name: decodeURIComponent(path).split("/").pop(),
        sizeText: backend.formatSize(size),
        size: size,
        iconName: ""
      })
      totalSize += size
    }
    if (wasEmpty && fileModel.count > 0) {
      backend.startDiscovery()
    }
  }

  function chooseFiles() {
    var urls = fileDialogs.openFiles("Choose files to share")
    if (urls.length > 0) {
      addFiles(urls)
    }
  }

  function removeFile(index) {
    totalSize -= fileModel.get(index).size
    fileModel.remove(index)
    if (fileModel.count === 0) {
      reset()
    }
  }

  function reset() {
    fileModel.clear()
    totalSize = 0
    outgoingId = 0
    currentId = 0
    startFailed = false
    backend.startReceive()
  }

  function cancelSend() {
    if (outgoingActive) {
      backend.cancel(outgoingId)
    } else {
      reset()
    }
  }

  function sendTo(id) {
    if (outgoingActive) {
      return
    }
    var paths = []
    for (var i = 0; i < fileModel.count; ++i) {
      paths.push(fileModel.get(i).path)
    }
    if (backend.sendFiles(id, paths)) {
      startFailed = false
      outgoingId = id
      currentId = id
    }
  }

  function dismissTransfer() {
    if (sendMode) {
      reset()
    } else {
      currentId = 0
    }
  }

  ListModel { id: issueModel }
  ListModel { id: fileModel }

  Connections {
    target: backend
    function onIssuesChanged() { Theme.sync(issueModel, backend.issues, "kind") }
    function onIncomingTransfer(id) {
      if (!window.outgoingActive) {
        window.currentId = id
      }
    }
    function onOutgoingTransferStartFailed(id) {
      if (id === window.outgoingId) {
        window.startFailed = true
      }
    }
    function onIncomingOffer(id, name, count, total) {
      incomingDialog.targetId = id
      incomingDialog.info = {
        name: name,
        fileList: [],
        totalText: count + (count === 1 ? " item" : " items") + " · " + backend.formatSize(total)
      }
      incomingDialog.open()
    }
    function onIncomingOfferResolved(id) {
      if (incomingDialog.opened && incomingDialog.targetId === id) {
        incomingDialog.close()
      }
    }
  }

  DropArea {
    id: drop
    anchors.fill: parent
    onDropped: function(event) { window.addFiles(event.urls) }
  }

  Flickable {
    id: scroll
    anchors.fill: parent
    contentWidth: width
    contentHeight: content.implicitHeight + 40
    boundsBehavior: Flickable.StopAtBounds
    clip: true
    ScrollBar.vertical: ScrollBar {}

    ColumnLayout {
      id: content
      x: 20
      y: 20
      width: scroll.width - 40
      spacing: 14

      RowLayout {
        Layout.fillWidth: true
        spacing: 14

        RippleAvatar {
          name: backend.hostname
          mode: window.mode
          failed: window.advertisingFailed
          Layout.preferredWidth: 56
          Layout.preferredHeight: 56
        }
        ColumnLayout {
          Layout.fillWidth: true
          spacing: 2
          Text {
            Layout.fillWidth: true
            text: backend.hostname
            color: Theme.ink
            font.pixelSize: 20
            font.weight: Font.DemiBold
            font.letterSpacing: -0.2
            elide: Text.ElideRight
          }
          RowLayout {
            spacing: 8
            Rectangle {
              readonly property bool on: window.mode !== "off"
              Layout.preferredWidth: pill.implicitWidth + 16
              Layout.preferredHeight: 20
              radius: 10
              color: on ? Theme.tint(Theme.accent, 0.14) : Theme.tint(Theme.muted, 0.16)
              Text {
                id: pill
                anchors.centerIn: parent
                text: window.mode === "send" ? "Sending"
                    : window.mode === "receive" ? "Receiving" : "Hidden"
                color: parent.on ? Theme.accent : Theme.muted
                font.pixelSize: 12
                font.weight: Font.DemiBold
              }
            }
            Text {
              Layout.fillWidth: true
              text: window.statusText
              color: Theme.muted
              font.pixelSize: 12
              elide: Text.ElideRight
            }
          }
        }
        AppSwitch {
          checked: backend.receiveEnabled
          enabled: !window.sendMode
          onToggled: backend.setReceiveEnabled(checked)
          Accessible.name: "Receive"
          ToolTip.visible: hovered && window.sendMode
          ToolTip.text: "Receiving is paused while sending files"
        }
      }

      ListView {
        id: issueList
        Layout.fillWidth: true
        Layout.preferredHeight: contentHeight
        interactive: false
        model: issueModel
        Behavior on Layout.preferredHeight {
          NumberAnimation {
            duration: Theme.reduceMotion ? 0 : 200
            easing.type: Easing.Bezier
            easing.bezierCurve: Theme.enterCurve
          }
        }
        add: Transition {
          NumberAnimation { property: "opacity"; from: 0; to: 1; duration: 220 }
          NumberAnimation {
            property: "y"; from: Theme.reduceMotion ? 0 : -10; duration: 220
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
        delegate: IssueBanner {
          required property string severity
          required property string title
          required property string detail
          width: issueList.width
          issue: ({ severity: severity, title: title, detail: detail })
        }
      }

      Item {
        id: filesArea
        Layout.fillWidth: true
        Layout.preferredHeight: window.sendMode ? fileCard.implicitHeight : 150

        Rectangle {
          id: dropZone
          anchors.fill: parent
          visible: opacity > 0
          opacity: window.sendMode ? 0 : 1
          radius: Theme.cardRadius
          color: drop.containsDrag ? Theme.tint(Theme.accent, 0.1) : "transparent"
          scale: drop.containsDrag && !Theme.reduceMotion ? 1.01 : 1
          Behavior on opacity { NumberAnimation { duration: 150 } }
          Behavior on scale { NumberAnimation { duration: 180; easing.type: Easing.OutCubic } }
          Behavior on color { ColorAnimation { duration: 150 } }

          Canvas {
            anchors.fill: parent
            property color stroke: drop.containsDrag ? Theme.accent : Theme.tint(Theme.ink, 0.25)
            onStrokeChanged: requestPaint()
            onWidthChanged: requestPaint()
            onHeightChanged: requestPaint()
            onPaint: {
              var ctx = getContext("2d")
              ctx.reset()
              ctx.lineWidth = 1.5
              ctx.strokeStyle = stroke
              ctx.setLineDash([6, 5])
              var r = Theme.cardRadius
              var w = width - 2
              var h = height - 2
              ctx.beginPath()
              ctx.moveTo(1 + r, 1)
              ctx.arcTo(1 + w, 1, 1 + w, 1 + h, r)
              ctx.arcTo(1 + w, 1 + h, 1, 1 + h, r)
              ctx.arcTo(1, 1 + h, 1, 1, r)
              ctx.arcTo(1, 1, 1 + w, 1, r)
              ctx.closePath()
              ctx.stroke()
            }
          }

          ColumnLayout {
            anchors.centerIn: parent
            spacing: 10
            Text {
              Layout.alignment: Qt.AlignHCenter
              text: drop.containsDrag ? "Release to send" : "Drop files here"
              color: Theme.ink
              font.pixelSize: 14
              font.weight: Font.DemiBold
            }
            AppButton {
              Layout.alignment: Qt.AlignHCenter
              text: "Choose files"
              primary: true
              onClicked: window.chooseFiles()
            }
          }
        }

        FileList {
          id: fileCard
          width: parent.width
          visible: opacity > 0
          opacity: window.sendMode ? 1 : 0
          model: fileModel
          summary: window.selectedSizeText
          Behavior on opacity { NumberAnimation { duration: 180 } }
          onRemoveRequested: function(index) { window.removeFile(index) }
          onAddRequested: window.chooseFiles()
          onCancelRequested: window.cancelSend()
        }
        Behavior on Layout.preferredHeight {
          NumberAnimation {
            duration: Theme.reduceMotion ? 0 : 200
            easing.type: Easing.Bezier
            easing.bezierCurve: Theme.enterCurve
          }
        }
      }

      Repeater {
        model: backend.transfers
        TransferCard {
          readonly property bool startFailure:
              window.startFailed && model.transferId === window.outgoingId
          readonly property int code: startFailure ? 6 : window.statusCode(model.status)
          readonly property bool sending: model.direction === "send"

          Layout.fillWidth: true
          visible: model.transferId === window.currentId
          status: code
          incoming: !sending
          peer: model.deviceName
          message: startFailure ? "Could not start sending"
              : window.statusMessage(code, sending, model.transferredAttachmentsCount,
                                     model.totalAttachmentsCount)
          progress: model.progress * 100
          speedText: model.speedText
          sizeText: Qt.locale().formattedDataSize(model.transferredBytes, 1, Locale.DataSizeSIFormat)
              + " of " + Qt.locale().formattedDataSize(model.totalBytes, 1, Locale.DataSizeSIFormat)
          onCancelRequested: backend.cancel(model.transferId)
          onCloseRequested: window.dismissTransfer()
        }
      }

      RowLayout {
        visible: window.sendMode
        Layout.fillWidth: true
        Text {
          text: "Nearby devices"
          color: Theme.ink
          font.pixelSize: 14
          font.weight: Font.DemiBold
        }
        Text {
          id: searching
          property int dots: 0
          text: "Searching" + ".".repeat(dots)
          color: Theme.muted
          font.pixelSize: 12
          Timer {
            running: window.visible && window.sendMode && !Theme.reduceMotion
            interval: 400
            repeat: true
            onTriggered: searching.dots = (searching.dots + 1) % 4
          }
        }
      }

      ListView {
        id: deviceList
        visible: window.sendMode && count > 0
        Layout.fillWidth: true
        Layout.preferredHeight: visible ? contentHeight : 0
        interactive: false
        model: backend.targets
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
          NumberAnimation { property: "y"; duration: Theme.reduceMotion ? 0 : 200 }
        }
        delegate: DeviceCard {
          required property var targetId
          width: deviceList.width
          receiveDisabled: window.outgoingActive
          onSendRequested: window.sendTo(targetId)
        }
      }

      Text {
        visible: window.sendMode && deviceList.count === 0
        Layout.fillWidth: true
        text: "No devices yet. On the phone, open Quick Share and set visibility to Everyone."
        color: Theme.muted
        font.pixelSize: 12
        wrapMode: Text.WordWrap
        horizontalAlignment: Text.AlignHCenter
        topPadding: 12
      }

      Rectangle {
        visible: window.mode === "receive" && window.currentId === 0
        Layout.fillWidth: true
        Layout.preferredHeight: hint.implicitHeight + 32
        radius: Theme.cardRadius
        color: Theme.surface
        border.width: 1
        border.color: Theme.hairline
        Text {
          id: hint
          anchors.fill: parent
          anchors.margins: 16
          text: "Ready to receive. Open Quick Share on your phone and pick " + backend.hostname + "."
          color: Theme.muted
          font.pixelSize: 14
          wrapMode: Text.WordWrap
        }
      }
    }
  }

  IncomingDialog {
    id: incomingDialog
    property var targetId: 0
    onAccepted: { close(); backend.accept(targetId) }
    onDeclined: { close(); backend.reject(targetId) }
  }
}
