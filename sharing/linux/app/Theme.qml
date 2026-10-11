pragma Singleton
import QtQuick

QtObject {
  id: theme

  property bool reduceMotion: false
  property bool active: true

  readonly property bool dark: Qt.styleHints.colorScheme === Qt.ColorScheme.Dark
  readonly property color bg: dark ? "#0F1420" : "#EEF2F7"
  readonly property color surface: dark ? "#182033" : "#FFFFFF"
  readonly property color ink: dark ? "#E6EBF5" : "#14213D"
  readonly property color muted: dark ? "#8C99AE" : "#6B7A90"
  readonly property color accent: dark ? "#6E8BFF" : "#2F5BEA"
  readonly property color danger: dark ? "#FF6B6F" : "#D93D42"
  readonly property color success: dark ? "#3CCB8F" : "#1F9D6B"
  readonly property color warning: dark ? "#F2B24C" : "#B7791F"
  readonly property color hairline: Qt.rgba(ink.r, ink.g, ink.b, 0.08)
  readonly property color onAccent: dark ? "#0F1420" : "#FFFFFF"

  readonly property int cardRadius: 14
  readonly property int controlRadius: 10
  readonly property var enterCurve: [0.23, 1, 0.32, 1, 1, 1]

  function tint(color, alpha) {
    return Qt.rgba(color.r, color.g, color.b, alpha)
  }

  function sync(model, items, key) {
    var keys = {}
    for (var i = 0; i < items.length; ++i) {
      keys[items[i][key]] = true
    }
    for (var r = model.count - 1; r >= 0; --r) {
      if (!keys[model.get(r)[key]]) {
        model.remove(r)
      }
    }
    for (var n = 0; n < items.length; ++n) {
      var at = -1
      for (var m = n; m < model.count; ++m) {
        if (model.get(m)[key] === items[n][key]) {
          at = m
          break
        }
      }
      if (at < 0) {
        model.insert(n, items[n])
      } else {
        if (at !== n) {
          model.move(at, n, 1)
        }
        model.set(n, items[n])
      }
    }
  }
}
