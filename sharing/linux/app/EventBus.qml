pragma Singleton

import QtQuick

QtObject {
    signal filesSelected(var filePaths)
    signal cancelPendingShareRequested()
    signal shareTargetSelected(var shareTargetId)
}
