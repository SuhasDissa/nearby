#pragma once

#include <QList>
#include <QObject>
#include <QString>
#include <QUrl>

// QFileDialog (not Qt.labs.platform / QtQuick.Dialogs) so the native platform
// dialog is used, which goes through xdg-desktop-portal when the portal
// platform theme is active. Requires QApplication.
class FileDialogs : public QObject {
  Q_OBJECT

 public:
  explicit FileDialogs(QObject* parent = nullptr);

  // Empty list when cancelled.
  Q_INVOKABLE QList<QUrl> openFiles(const QString& title) const;
};
