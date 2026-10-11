import QtQuick
import QtQuick.Controls

ApplicationWindow {
  id: window
  width: 440
  height: 680
  minimumWidth: 380
  minimumHeight: 560
  visible: true
  title: qsTr("QuickShare")
  color: Theme.bg

  Binding {
    target: Theme
    property: "active"
    value: window.visible
  }

  Component.onCompleted: Theme.reduceMotion =
      Qt.application.arguments.indexOf("--reduce-motion") >= 0 || backend.reduceMotion

  onClosing: function(close) {
    if (appController.closeToTray) {
      close.accepted = false
      window.hide()
    }
  }

  Loader {
    id: contentLoader
    anchors.fill: parent
    source: "AppContent.qml"
  }

  function reloadInnerContent() {
    const nextSource = "AppContent.qml?rev=" + Date.now()
    contentLoader.active = false
    contentLoader.source = ""
    contentLoader.active = true
    contentLoader.source = nextSource
  }
}
