#include "sharing/linux/app/file_dialogs.h"

#include <QFileDialog>
#include <QStandardPaths>

FileDialogs::FileDialogs(QObject* parent) : QObject(parent) {}

QList<QUrl> FileDialogs::openFiles(const QString& title) const {
  QFileDialog dialog;
  dialog.setWindowTitle(title);
  dialog.setAcceptMode(QFileDialog::AcceptOpen);
  dialog.setFileMode(QFileDialog::ExistingFiles);
  dialog.setDirectory(
      QStandardPaths::writableLocation(QStandardPaths::HomeLocation));
  if (dialog.exec() != QDialog::Accepted) {
    return {};
  }
  return dialog.selectedUrls();
}
