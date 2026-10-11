#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

#include <QDir>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QFontDatabase>
#include <QIcon>
#include <QApplication>
#include <QMetaObject>
#include <QObject>
#include <QSocketNotifier>
#include <QTimer>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include "application_controller.h"
#include "backend.h"
#include "file_dialogs.h"
#include "quickshare.h"

namespace {

constexpr char kQmlHotReloadEnv[] = "NEARBY_QML_HOT_RELOAD";
constexpr char kQmlSourceDirEnv[] = "NEARBY_QML_SOURCE_DIR";
constexpr char kDefaultQmlSourceDir[] = QUICKSHARE_QML_SOURCE_DIR;

struct LoggingScope {
  LoggingScope() { quickshare_logging_init(); }
  ~LoggingScope() { quickshare_logging_shutdown(); }
};

int g_signal_pipe[2] = {-1, -1};

void HandleTerminationSignal(int signal) {
  const char value = static_cast<char>(signal);
  if (g_signal_pipe[1] != -1) {
    static_cast<void>(write(g_signal_pipe[1], &value, sizeof(value)));
  }
}

bool IsHotReloadEnabled() {
  const QByteArray value = qgetenv(kQmlHotReloadEnv);
  return value == "1" || value.toLower() == "true";
}

QDir QmlSourceDir() {
  const QByteArray configured_dir = qgetenv(kQmlSourceDirEnv);
  if (!configured_dir.isEmpty()) {
    return QDir(QString::fromLocal8Bit(configured_dir));
  }
  return QDir(QString::fromUtf8(kDefaultQmlSourceDir));
}

void WatchQmlFiles(QFileSystemWatcher& watcher, const QDir& source_dir) {
  const QStringList existing_files = watcher.files();
  if (!existing_files.isEmpty()) {
    watcher.removePaths(existing_files);
  }

  const QStringList files =
      source_dir.entryList(QStringList() << "*.qml", QDir::Files, QDir::Name);
  for (const QString& file : files) {
    watcher.addPath(source_dir.absoluteFilePath(file));
  }
}

void DestroyRootObjects(QQmlApplicationEngine& engine) {
  const QList<QObject*> root_objects = engine.rootObjects();
  for (QObject* object : root_objects) {
    object->deleteLater();
  }
}

bool ReloadInnerContent(QQmlApplicationEngine& engine) {
  const QList<QObject*> root_objects = engine.rootObjects();
  if (root_objects.isEmpty()) {
    return false;
  }

  QObject* root = root_objects.constFirst();
  return QMetaObject::invokeMethod(root, "reloadInnerContent");
}

bool RequiresFullReload(const QFileInfo& changed_file) {
  return changed_file.fileName() == QStringLiteral("main.qml");
}

void InstallTerminationCleanup(QApplication& app) {
  if (pipe(g_signal_pipe) != 0) {
    return;
  }
  fcntl(g_signal_pipe[0], F_SETFL,
        fcntl(g_signal_pipe[0], F_GETFL, 0) | O_NONBLOCK);
  fcntl(g_signal_pipe[1], F_SETFL,
        fcntl(g_signal_pipe[1], F_GETFL, 0) | O_NONBLOCK);

  auto* notifier =
      new QSocketNotifier(g_signal_pipe[0], QSocketNotifier::Read, &app);
  QObject::connect(notifier, &QSocketNotifier::activated, &app,
                   [&app, notifier](int socket) {
                     notifier->setEnabled(false);
                     char buffer[32];
                     while (read(socket, buffer, sizeof(buffer)) > 0) {
                     }
                     app.quit();
                   });

  struct sigaction action{};
  action.sa_handler = HandleTerminationSignal;
  sigemptyset(&action.sa_mask);
  action.sa_flags = 0;
  sigaction(SIGINT, &action, nullptr);
  sigaction(SIGTERM, &action, nullptr);
}

}  // namespace

int main(int argc, char* argv[]) {
  LoggingScope logging;

  // The AppImage bundles no gtk3/kde theme plugin, so file dialogs would fall
  // back to Qt's own. The portal plugin is bundled but Qt only selects it
  // inside Flatpak/Snap. linuxdeploy's AppRun hook sets "gtk2" on GNOME/XFCE,
  // which is never bundled, so replace that too.
  if (qEnvironmentVariableIsSet("APPIMAGE")) {
    const QByteArray theme = qgetenv("QT_QPA_PLATFORMTHEME");
    if (theme.isEmpty() || theme == "gtk2") {
      qputenv("QT_QPA_PLATFORMTHEME", "xdgdesktopportal");
    }
  }

  // QApplication (not QGuiApplication) is required so QFileDialog can use the
  // native platform file picker, which routes through xdg-desktop-portal.
  QApplication app(argc, argv);
  app.setApplicationName(QStringLiteral("QuickShare"));
  app.setOrganizationDomain(QStringLiteral("io.github.quickshare_linux"));
  app.setDesktopFileName(QStringLiteral("io.github.quickshare_linux.QuickShare"));
  app.setWindowIcon(QIcon(QStringLiteral(":/icons/quickshare.svg")));
  app.setQuitOnLastWindowClosed(false);

  ApplicationController application_controller(app);
  if (!application_controller.ClaimSingleInstance()) {
    return 0;
  }

  Backend backend;
  application_controller.SetBackend(&backend);
  application_controller.InitializeTray();
  QQmlApplicationEngine engine;
  InstallTerminationCleanup(app);

  engine.rootContext()->setContextProperty("backend", &backend);
  engine.rootContext()->setContextProperty("appController",
                                           &application_controller);
  FileDialogs file_dialogs;
  engine.rootContext()->setContextProperty("fileDialogs", &file_dialogs);
  QObject::connect(&engine, &QQmlApplicationEngine::objectCreated,
                   &application_controller,
                   [&application_controller](QObject* object, const QUrl&) {
                     if (object != nullptr) {
                       application_controller.AttachWindow(object);
                     }
                   });

  const int fontId = QFontDatabase::addApplicationFont(":/googlesans_var.ttf");

  if (fontId != -1) {
    const QStringList fontFamilies =
        QFontDatabase::applicationFontFamilies(fontId);
    if (!fontFamilies.isEmpty()) {
      const QFont defaultFont(fontFamilies.at(0));
      app.setFont(defaultFont);
    }
  }

  if (IsHotReloadEnabled()) {
    const QDir qml_source_dir = QmlSourceDir();
    const QUrl source_url =
        QUrl::fromLocalFile(qml_source_dir.absoluteFilePath("main.qml"));
    QFileSystemWatcher watcher;
    QTimer reload_timer;
    reload_timer.setSingleShot(true);
    reload_timer.setInterval(75);
    QString changed_path;

    const auto fullReload = [&engine, &watcher, qml_source_dir, source_url]() {
      WatchQmlFiles(watcher, qml_source_dir);
      DestroyRootObjects(engine);
      engine.clearComponentCache();
      engine.load(source_url);
    };

    const auto reload = [&engine, &watcher, qml_source_dir, source_url,
                         &changed_path, &fullReload]() {
      WatchQmlFiles(watcher, qml_source_dir);
      const QFileInfo changed_file(changed_path);

      if (RequiresFullReload(changed_file) || engine.rootObjects().isEmpty()) {
        fullReload();
        return;
      }

      engine.clearComponentCache();
      if (!ReloadInnerContent(engine)) {
        fullReload();
      }
    };

    QObject::connect(&reload_timer, &QTimer::timeout, &app, reload);
    QObject::connect(&watcher, &QFileSystemWatcher::fileChanged, &app,
                     [&reload_timer, &changed_path](const QString& path) {
                       changed_path = path;
                       reload_timer.start();
                     });
    QObject::connect(&watcher, &QFileSystemWatcher::directoryChanged, &app,
                     [&reload_timer, &changed_path](const QString& path) {
                       changed_path = path;
                       reload_timer.start();
                     });

    watcher.addPath(qml_source_dir.absolutePath());
    fullReload();

    const int result = app.exec();
    backend.shutdown();
    return result;
  }

  // Loaded via the qrc scheme since it's compiled into the binary
  const QUrl url(QStringLiteral("qrc:/main.qml"));

  QObject::connect(
      &engine, &QQmlApplicationEngine::objectCreated, &app,
      [url](QObject* obj, const QUrl& objUrl) {
        if (!obj && url == objUrl) QCoreApplication::exit(-1);
      },
      Qt::QueuedConnection);

  engine.load(url);

  const int result = app.exec();
  backend.shutdown();
  return result;
}
