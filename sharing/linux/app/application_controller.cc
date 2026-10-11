#include "sharing/linux/app/application_controller.h"

#include <QApplication>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusReply>
#include <QDebug>
#include <QIcon>
#include <QMenu>
#include <QQuickWindow>
#include <QStringList>
#include <QSystemTrayIcon>
#include <QTimer>
#include <QVariantMap>

#include "sharing/linux/app/backend.h"

namespace {

constexpr char kApplicationService[] = "io.github.quickshare_linux.QuickShare";
constexpr char kApplicationPath[] = "/io/github/quickshare_linux/QuickShare";
constexpr char kApplicationInterface[] = "io.github.quickshare_linux.QuickShare";
constexpr char kNotificationService[] = "org.freedesktop.Notifications";
constexpr char kNotificationPath[] = "/org/freedesktop/Notifications";
constexpr char kNotificationInterface[] = "org.freedesktop.Notifications";
constexpr int kOfferTimeoutMilliseconds = 45 * 1000;
constexpr int kResultTimeoutMilliseconds = 10 * 1000;

QString FormatBytes(qint64 bytes) {
  if (bytes <= 0) {
    return {};
  }
  static constexpr const char* kUnits[] = {"B", "KB", "MB", "GB", "TB"};
  double value = static_cast<double>(bytes);
  int unit = 0;
  while (value >= 1024.0 && unit < 4) {
    value /= 1024.0;
    ++unit;
  }
  const int precision = unit == 0 ? 0 : (value >= 10.0 ? 1 : 2);
  return QStringLiteral("%1 %2").arg(QString::number(value, 'f', precision),
                                     QString::fromLatin1(kUnits[unit]));
}

}  // namespace

ApplicationController::ApplicationController(QApplication& application,
                                             QObject* parent)
    : QObject(parent), application_(application) {
  QDBusConnection bus = QDBusConnection::sessionBus();
  if (bus.isConnected()) {
    bus.connect(kNotificationService, kNotificationPath, kNotificationInterface,
                QStringLiteral("ActionInvoked"), this,
                SLOT(OnNotificationAction(uint, QString)));
    bus.connect(kNotificationService, kNotificationPath, kNotificationInterface,
                QStringLiteral("NotificationClosed"), this,
                SLOT(OnNotificationClosed(uint, uint)));

    QDBusInterface notifications(kNotificationService, kNotificationPath,
                                 kNotificationInterface, bus);
    const QDBusReply<QStringList> capabilities =
        notifications.call(QStringLiteral("GetCapabilities"));
    actions_supported_ =
        capabilities.isValid() && capabilities.value().contains("actions");
  }
}

ApplicationController::~ApplicationController() {
  const QList<qint64> pending_offers = transfer_to_notification_.keys();
  for (qint64 share_target_id : pending_offers) {
    CloseOfferNotification(share_target_id);
  }
  if (tray_icon_ != nullptr) {
    tray_icon_->setContextMenu(nullptr);
  }
  QDBusConnection bus = QDBusConnection::sessionBus();
  if (owns_dbus_service_ && bus.isConnected()) {
    bus.unregisterService(kApplicationService);
  }
  if (bus.isConnected()) {
    bus.unregisterObject(kApplicationPath);
  }
}

bool ApplicationController::ClaimSingleInstance() {
  QDBusConnection bus = QDBusConnection::sessionBus();
  if (!bus.isConnected()) {
    qWarning() << "Session D-Bus is unavailable; single-instance activation "
                  "and actionable notifications are disabled";
    return true;
  }

  if (!bus.registerObject(kApplicationPath, this,
                          QDBusConnection::ExportScriptableSlots)) {
    qWarning() << "Unable to export QuickShare activation object:"
               << bus.lastError().message();
  }
  if (bus.registerService(kApplicationService)) {
    owns_dbus_service_ = true;
    return true;
  }

  QDBusInterface existing(kApplicationService, kApplicationPath,
                          kApplicationInterface, bus);
  const QDBusReply<void> activation =
      existing.call(QStringLiteral("ShowWindow"));
  if (activation.isValid()) {
    return false;
  }

  qWarning() << "Unable to claim or activate the QuickShare D-Bus service:"
             << activation.error().message();
  return true;
}

void ApplicationController::InitializeTray() {
  tray_menu_ = std::make_unique<QMenu>();
  tray_menu_->addAction(tr("Show QuickShare"), this,
                        &ApplicationController::ShowWindow);
  tray_menu_->addSeparator();
  tray_menu_->addAction(tr("Quit"), this, &ApplicationController::Quit);

  tray_icon_ = new QSystemTrayIcon(
      QIcon(QStringLiteral(":/icons/quickshare.svg")), this);
  tray_icon_->setToolTip(tr("QuickShare"));
  tray_icon_->setContextMenu(tray_menu_.get());
  connect(tray_icon_, &QSystemTrayIcon::activated, this,
          [this](QSystemTrayIcon::ActivationReason reason) {
            if (reason == QSystemTrayIcon::Trigger ||
                reason == QSystemTrayIcon::DoubleClick) {
              ShowWindow();
            }
          });
  tray_icon_->show();

  auto* tray_availability_timer = new QTimer(this);
  tray_availability_timer->setInterval(2000);
  connect(tray_availability_timer, &QTimer::timeout, this,
          &ApplicationController::UpdateTrayAvailability);
  tray_availability_timer->start();
  UpdateTrayAvailability();
}

void ApplicationController::SetBackend(Backend* backend) {
  if (backend == nullptr) {
    return;
  }
  connect(backend, &Backend::incomingOffer, this,
          &ApplicationController::OnIncomingOffer);
  connect(backend, &Backend::incomingOfferResolved, this,
          &ApplicationController::OnIncomingOfferResolved);
  connect(backend, &Backend::transferFinished, this,
          &ApplicationController::OnTransferFinished);
  connect(this, &ApplicationController::acceptRequested, backend,
          &Backend::accept);
  connect(this, &ApplicationController::rejectRequested, backend,
          &Backend::reject);
}

void ApplicationController::AttachWindow(QObject* root_object) {
  window_ = qobject_cast<QQuickWindow*>(root_object);
  if (window_ != nullptr && pending_activation_) {
    pending_activation_ = false;
    ShowWindow();
  }
}

void ApplicationController::ShowWindow() {
  if (window_ == nullptr) {
    pending_activation_ = true;
    return;
  }
  window_->show();
  window_->raise();
  window_->requestActivate();
}

void ApplicationController::Quit() {
  application_.quit();
}

void ApplicationController::OnIncomingOffer(qint64 share_target_id,
                                            const QString& device_name,
                                            int attachment_count,
                                            qint64 total_bytes) {
  if (!actions_supported_) {
    ShowWindow();
    return;
  }

  CloseOfferNotification(share_target_id);
  const QStringList actions = {
      QStringLiteral("default"), QStringLiteral("Open"),
      QStringLiteral("accept"),  QStringLiteral("Accept"),
      QStringLiteral("decline"), QStringLiteral("Decline"),
  };
  const uint notification_id = SendNotification(
      tr("Incoming share"),
      IncomingOfferBody(device_name, attachment_count, total_bytes), actions,
      kOfferTimeoutMilliseconds);
  if (notification_id == 0) {
    ShowWindow();
    return;
  }
  notification_to_transfer_.insert(notification_id, share_target_id);
  transfer_to_notification_.insert(share_target_id, notification_id);
}

void ApplicationController::OnIncomingOfferResolved(qint64 share_target_id) {
  CloseOfferNotification(share_target_id);
}

void ApplicationController::OnTransferFinished(qint64 share_target_id,
                                               bool receive_mode,
                                               const QString& device_name,
                                               const QString& status) {
  CloseOfferNotification(share_target_id);
  if (!receive_mode ||
      !background_accepted_transfers_.remove(share_target_id)) {
    return;
  }

  const bool complete = status == QStringLiteral("kComplete");
  const QString peer =
      device_name.isEmpty() ? tr("the nearby device") : device_name;
  SendNotification(complete ? tr("Share received") : tr("Share failed"),
                   complete
                       ? tr("Finished receiving from %1").arg(peer)
                       : tr("Could not receive the share from %1").arg(peer),
                   {}, kResultTimeoutMilliseconds);
}

void ApplicationController::OnNotificationAction(uint notification_id,
                                                 const QString& action_key) {
  const auto transfer = notification_to_transfer_.constFind(notification_id);
  if (transfer == notification_to_transfer_.cend()) {
    return;
  }
  const qint64 share_target_id = transfer.value();

  if (action_key == QStringLiteral("default")) {
    ShowWindow();
    return;
  }
  if (action_key == QStringLiteral("accept")) {
    background_accepted_transfers_.insert(share_target_id);
    emit acceptRequested(share_target_id);
  } else if (action_key == QStringLiteral("decline")) {
    emit rejectRequested(share_target_id);
  } else {
    return;
  }
  CloseOfferNotification(share_target_id);
}

void ApplicationController::OnNotificationClosed(uint notification_id,
                                                 uint /*reason*/) {
  RemoveNotificationMapping(notification_id);
}

void ApplicationController::UpdateTrayAvailability() {
  const bool available = QSystemTrayIcon::isSystemTrayAvailable();
  application_.setQuitOnLastWindowClosed(!available);
  if (close_to_tray_ == available) {
    return;
  }
  close_to_tray_ = available;
  emit closeToTrayChanged();
}

uint ApplicationController::SendNotification(const QString& summary,
                                             const QString& body,
                                             const QStringList& actions,
                                             int timeout_milliseconds) {
  QDBusConnection bus = QDBusConnection::sessionBus();
  if (!bus.isConnected()) {
    return 0;
  }
  QDBusInterface notifications(kNotificationService, kNotificationPath,
                               kNotificationInterface, bus);
  const QDBusReply<uint> reply =
      notifications.call(QStringLiteral("Notify"), QStringLiteral("QuickShare"),
                         uint{0}, QStringLiteral("quickshare"), summary, body,
                         actions, QVariantMap{}, timeout_milliseconds);
  if (!reply.isValid()) {
    qWarning() << "Unable to send desktop notification:"
               << reply.error().message();
    return 0;
  }
  return reply.value();
}

void ApplicationController::CloseOfferNotification(qint64 share_target_id) {
  const auto notification =
      transfer_to_notification_.constFind(share_target_id);
  if (notification == transfer_to_notification_.cend()) {
    return;
  }
  const uint notification_id = notification.value();
  RemoveNotificationMapping(notification_id);

  QDBusConnection bus = QDBusConnection::sessionBus();
  if (!bus.isConnected()) {
    return;
  }
  QDBusInterface notifications(kNotificationService, kNotificationPath,
                               kNotificationInterface, bus);
  notifications.asyncCall(QStringLiteral("CloseNotification"), notification_id);
}

void ApplicationController::RemoveNotificationMapping(uint notification_id) {
  const auto transfer = notification_to_transfer_.find(notification_id);
  if (transfer == notification_to_transfer_.end()) {
    return;
  }
  transfer_to_notification_.remove(transfer.value());
  notification_to_transfer_.erase(transfer);
}

QString ApplicationController::IncomingOfferBody(const QString& device_name,
                                                 int attachment_count,
                                                 qint64 total_bytes) const {
  const QString peer =
      device_name.isEmpty() ? tr("A nearby device") : device_name;
  QString contents;
  if (attachment_count > 0) {
    contents = attachment_count == 1 ? tr("1 item")
                                     : tr("%1 items").arg(attachment_count);
  } else {
    contents = FormatBytes(total_bytes);
  }
  if (contents.isEmpty()) {
    contents = tr("a share");
  }
  return tr("%1 wants to send %2")
      .arg(peer, contents);
}
