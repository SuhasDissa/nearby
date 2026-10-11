#include "sharing/linux/app/backend.h"

#include <algorithm>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <utility>

#include <QDebug>
#include <QLocale>
#include <QMetaObject>
#include <QPointer>
#include <QSysInfo>
#include <QUrl>
#include <QVariantMap>

namespace {

constexpr int kReceiveTimeoutMilliseconds = 30 * 1000;
constexpr int kIncomingOfferTimeoutMilliseconds = 45 * 1000;
constexpr int kIssuesRefreshMilliseconds = 2000;
constexpr qint64 kSpeedWindowMilliseconds = 1000;
constexpr qint64 kMinSpeedSampleMilliseconds = 250;

using StatusHandler = std::function<void(int)>;

QString NormalizeLocalPath(const QString& path) {
  const QUrl url(path);
  return url.isValid() && url.isLocalFile() ? url.toLocalFile() : path;
}

ShareTargetInfo ToTargetInfo(const QuickShareTarget& target) {
  return {target.id, QString::fromUtf8(target.device_name), target.type};
}

TransferInfo ToTransferInfo(const QuickShareTransfer& transfer) {
  return {transfer.status,
          QString::fromUtf8(transfer.status_name),
          transfer.progress_percent,
          transfer.transferred_bytes,
          transfer.total_attachments_count,
          transfer.transferred_attachments_count,
          transfer.is_final_status != 0};
}

QVariantMap CreateIssue(const QString& kind, const QString& severity,
                        const QString& title, const QString& detail) {
  QVariantMap issue;
  issue.insert(QStringLiteral("kind"), kind);
  issue.insert(QStringLiteral("severity"), severity);
  issue.insert(QStringLiteral("title"), title);
  issue.insert(QStringLiteral("detail"), detail);
  return issue;
}

void StatusThunk(int status, void* user) {
  std::unique_ptr<StatusHandler> handler(static_cast<StatusHandler*>(user));
  (*handler)(status);
}

void PostStatus(Backend* backend, std::function<void()> callback) {
  QPointer<Backend> guarded_backend(backend);
  if (!guarded_backend) {
    return;
  }
  QMetaObject::invokeMethod(
      guarded_backend,
      [guarded_backend, callback = std::move(callback)]() mutable {
        if (guarded_backend) {
          callback();
        }
      },
      Qt::QueuedConnection);
}

// Completion handler that hands the status to the GUI thread.
StatusHandler* PostedStatus(Backend* backend,
                            std::function<void(Backend&, int)> callback) {
  QPointer<Backend> guarded_backend(backend);
  return new StatusHandler(
      [guarded_backend, callback = std::move(callback)](int status) {
        if (!guarded_backend) {
          return;
        }
        PostStatus(guarded_backend, [guarded_backend, callback, status]() {
          if (guarded_backend) {
            callback(*guarded_backend, status);
          }
        });
      });
}

}  // namespace

ShareTargetModel::ShareTargetModel(QObject* parent)
    : QAbstractListModel(parent) {}

int ShareTargetModel::rowCount(const QModelIndex& parent) const {
  return parent.isValid() ? 0 : static_cast<int>(targets_.size());
}

QVariant ShareTargetModel::data(const QModelIndex& index, int role) const {
  if (!index.isValid() || index.row() < 0 ||
      index.row() >= static_cast<int>(targets_.size())) {
    return {};
  }

  const ShareTarget& target = targets_[index.row()];
  switch (role) {
    case IdRole:
      return QVariant::fromValue<qint64>(target.id);
    case DeviceNameRole:
      return target.device_name;
    case TypeRole:
      return static_cast<int>(target.type);
    default:
      return {};
  }
}

QHash<int, QByteArray> ShareTargetModel::roleNames() const {
  return {
      {IdRole, "targetId"},
      {DeviceNameRole, "deviceName"},
      {TypeRole, "type"},
  };
}

void ShareTargetModel::ApplyTarget(const ShareTarget& target) {
  const int row = IndexOf(target.id);
  if (row < 0) {
    const int insert_row = static_cast<int>(targets_.size());
    beginInsertRows(QModelIndex(), insert_row, insert_row);
    targets_.push_back(target);
    endInsertRows();
    return;
  }

  targets_[row] = target;
  emit dataChanged(index(row), index(row), {IdRole, DeviceNameRole, TypeRole});
}

void ShareTargetModel::RemoveTarget(int64_t target_id) {
  const int row = IndexOf(target_id);
  if (row < 0) {
    return;
  }
  beginRemoveRows(QModelIndex(), row, row);
  targets_.erase(targets_.begin() + row);
  endRemoveRows();
}

void ShareTargetModel::Clear() {
  if (targets_.empty()) {
    return;
  }
  beginResetModel();
  targets_.clear();
  endResetModel();
}

std::optional<ShareTargetModel::ShareTarget> ShareTargetModel::FindTarget(
    int64_t target_id) const {
  const int row = IndexOf(target_id);
  return row < 0 ? std::nullopt : std::optional<ShareTarget>(targets_[row]);
}

int ShareTargetModel::IndexOf(int64_t target_id) const {
  for (int i = 0; i < static_cast<int>(targets_.size()); ++i) {
    if (targets_[i].id == target_id) {
      return i;
    }
  }
  return -1;
}

ShareTransferModel::ShareTransferModel(QObject* parent)
    : QAbstractListModel(parent) {
  clock_.start();
}

int ShareTransferModel::rowCount(const QModelIndex& parent) const {
  return parent.isValid() ? 0 : static_cast<int>(transfers_.size());
}

QVariant ShareTransferModel::data(const QModelIndex& index, int role) const {
  if (!index.isValid() || index.row() < 0 ||
      index.row() >= static_cast<int>(transfers_.size())) {
    return {};
  }
  return DataForRow(transfers_[index.row()], role);
}

QHash<int, QByteArray> ShareTransferModel::roleNames() const {
  return {
      {IdRole, "transferId"},
      {DirectionRole, "direction"},
      {DeviceNameRole, "deviceName"},
      {StatusRole, "status"},
      {ProgressRole, "progress"},
      {TransferredBytesRole, "transferredBytes"},
      {TotalBytesRole, "totalBytes"},
      {TotalAttachmentsCountRole, "totalAttachmentsCount"},
      {TransferredAttachmentsCountRole, "transferredAttachmentsCount"},
      {IsFinalStatusRole, "isFinalStatus"},
      {AwaitingLocalConfirmationRole, "awaitingLocalConfirmation"},
      {LocalPathRole, "localPath"},
      {SpeedTextRole, "speedText"},
  };
}

void ShareTransferModel::ApplyTarget(const ShareTarget& target) {
  const int row = IndexOf(target.id);
  if (row < 0) {
    return;
  }
  transfers_[row].target = target;
  EmitRowChanged(row);
}

void ShareTransferModel::ApplyTransfer(bool receive_mode,
                                       const ShareTarget& target,
                                       const TransferMetadata& transfer,
                                       int64_t total_bytes) {
  int row = IndexOf(target.id);
  if (row < 0) {
    row = static_cast<int>(transfers_.size());
    beginInsertRows(QModelIndex(), row, row);
    Row new_row{target.id, receive_mode, target, transfer, total_bytes, {}};
    UpdateSpeed(new_row);
    transfers_.push_back(std::move(new_row));
    endInsertRows();
    return;
  }

  transfers_[row].receive_mode = receive_mode;
  transfers_[row].target = target;
  transfers_[row].transfer = transfer;
  transfers_[row].total_bytes = total_bytes;
  UpdateSpeed(transfers_[row]);
  EmitRowChanged(row);
}

void ShareTransferModel::PrepareOutgoingTransfer(
    int64_t target_id, const QString& local_path,
    const std::optional<ShareTarget>& target) {
  int row = IndexOf(target_id);
  if (row < 0) {
    row = static_cast<int>(transfers_.size());
    beginInsertRows(QModelIndex(), row, row);
    Row transfer;
    transfer.id = target_id;
    transfer.target = target;
    transfer.local_path = local_path;
    transfers_.push_back(std::move(transfer));
    endInsertRows();
    return;
  }

  transfers_[row].receive_mode = false;
  transfers_[row].target = target;
  transfers_[row].transfer.reset();
  transfers_[row].total_bytes = 0;
  transfers_[row].local_path = local_path;
  transfers_[row].speed_samples.clear();
  transfers_[row].speed_text.clear();
  EmitRowChanged(row);
}

int ShareTransferModel::IndexOf(int64_t target_id) const {
  for (int i = 0; i < static_cast<int>(transfers_.size()); ++i) {
    if (transfers_[i].id == target_id) {
      return i;
    }
  }
  return -1;
}

void ShareTransferModel::UpdateSpeed(Row& row) {
  if (!row.transfer.has_value() ||
      row.transfer->status != QUICKSHARE_TRANSFER_IN_PROGRESS) {
    row.speed_samples.clear();
    row.speed_text.clear();
    return;
  }

  const qint64 now = clock_.elapsed();
  row.speed_samples.push_back(
      {now, static_cast<qint64>(row.transfer->transferred_bytes)});
  while (row.speed_samples.size() > 2 &&
         now - row.speed_samples.first().first > kSpeedWindowMilliseconds) {
    row.speed_samples.removeFirst();
  }
  const qint64 elapsed =
      row.speed_samples.last().first - row.speed_samples.first().first;
  if (elapsed >= kMinSpeedSampleMilliseconds) {
    const double rate = (row.speed_samples.last().second -
                         row.speed_samples.first().second) *
                        1000.0 / elapsed;
    row.speed_text = QLocale().formattedDataSize(static_cast<qint64>(rate), 1,
                                                 QLocale::DataSizeSIFormat) +
                     QStringLiteral("/s");
  }
}

QVariant ShareTransferModel::DataForRow(const Row& row, int role) const {
  const auto& target = row.target;
  const auto& transfer = row.transfer;
  switch (role) {
    case IdRole:
      return QVariant::fromValue<qint64>(row.id);
    case DirectionRole:
      return row.receive_mode ? QStringLiteral("receive")
                              : QStringLiteral("send");
    case DeviceNameRole:
      return target.has_value() && !target->device_name.isEmpty()
                 ? target->device_name
                 : QStringLiteral("Unknown device");
    case StatusRole:
      return transfer.has_value() ? transfer->status_name : QString();
    case ProgressRole:
      return transfer.has_value()
                 ? std::clamp(transfer->progress_percent / 100.0,
                              0.0, 1.0)
                 : 0.0;
    case TransferredBytesRole:
      return QVariant::fromValue<qulonglong>(
          transfer.has_value() ? transfer->transferred_bytes : 0);
    case TotalBytesRole:
      return QVariant::fromValue<qint64>(row.total_bytes);
    case TotalAttachmentsCountRole:
      return transfer.has_value() ? transfer->total_attachments_count : 0;
    case TransferredAttachmentsCountRole:
      return transfer.has_value() ? transfer->transferred_attachments_count
                                  : 0;
    case IsFinalStatusRole:
      return transfer.has_value() && transfer->is_final_status;
    case AwaitingLocalConfirmationRole:
      return transfer.has_value() &&
             transfer->status == QUICKSHARE_TRANSFER_AWAITING_LOCAL_CONFIRMATION;
    case LocalPathRole:
      return row.local_path;
    case SpeedTextRole:
      return row.speed_text;
    default:
      return {};
  }
}

void ShareTransferModel::EmitRowChanged(int row) {
  emit dataChanged(
      index(row), index(row),
      {IdRole, DirectionRole, DeviceNameRole, StatusRole, ProgressRole,
       TransferredBytesRole, TotalBytesRole, TotalAttachmentsCountRole,
       TransferredAttachmentsCountRole, IsFinalStatusRole,
       AwaitingLocalConfirmationRole, LocalPathRole, SpeedTextRole});
}

Backend::Backend(QObject* parent)
    : QObject(parent), targets_(this), transfers_(this) {
  receive_timeout_timer_.setSingleShot(true);
  receive_timeout_timer_.setInterval(kReceiveTimeoutMilliseconds);
  connect(&receive_timeout_timer_, &QTimer::timeout, this,
          &Backend::OnReceiveTimeout);

  qs_ = quickshare_create();
  if (qs_ == nullptr) {
    qCritical() << "Failed to create NearbySharingService";
    desired_mode_ = Mode::kNone;
    return;
  }
  wifi_direct_ = quickshare_wifi_direct_available(qs_) != 0;
  ConnectLibrary();

  issues_timer_.setInterval(kIssuesRefreshMilliseconds);
  connect(&issues_timer_, &QTimer::timeout, this, &Backend::RefreshIssues);
  issues_timer_.start();
  RefreshIssues();

  quickshare_initialize(
      qs_, StatusThunk,
      PostedStatus(this, [](Backend& backend, int status) {
        if (backend.shutting_down_) {
          return;
        }
        backend.initialized_ = status == QUICKSHARE_OK;
        backend.ReportStatus(QStringLiteral("Initialize"), status);
        if (backend.initialized_) {
          backend.StartFastInitiationMonitoring();
        } else {
          backend.desired_mode_ = Mode::kNone;
        }
      }));
}

Backend::Backend(QuickShare* qs, int receive_timeout_milliseconds,
                 QObject* parent)
    : QObject(parent),
      targets_(this),
      transfers_(this),
      qs_(qs),
      initialized_(true) {
  receive_timeout_timer_.setSingleShot(true);
  receive_timeout_timer_.setInterval(receive_timeout_milliseconds);
  connect(&receive_timeout_timer_, &QTimer::timeout, this,
          &Backend::OnReceiveTimeout);
  ConnectLibrary();
  StartFastInitiationMonitoring();
}

Backend::~Backend() {
  shutdown();
  if (qs_ != nullptr) {
    quickshare_destroy(qs_);
    qs_ = nullptr;
  }
}

void Backend::ConnectLibrary() {
  const QuickShareCallbacks callbacks = {&Backend::TargetFound,
                                         &Backend::TargetLost,
                                         &Backend::TransferUpdate, this};
  quickshare_set_callbacks(qs_, &callbacks);
}

QString Backend::hostname() const {
  return QSysInfo::machineHostName();
}

bool Backend::reduceMotion() const {
  return qEnvironmentVariableIntValue("QUICKSHARE_REDUCE_MOTION") != 0;
}

void Backend::startReceive() {
  if (qs_ == nullptr || shutting_down_) {
    return;
  }
  StartFastInitiationMonitoring();
}

void Backend::startDiscovery() {
  if (qs_ == nullptr || shutting_down_) {
    return;
  }
  monitoring_requested_ = false;
  receive_timeout_timer_.stop();
  receive_window_pending_ = false;
  receive_offer_received_ = false;
  fallback_receive_window_ = false;
  StopFastInitiationScanningForDiscovery();
}

bool Backend::sendFiles(qint64 share_target_id, const QStringList& paths) {
  if (qs_ == nullptr || shutting_down_ || paths.isEmpty()) {
    return false;
  }
  std::vector<std::string> native_paths;
  for (const QString& path : paths) {
    const QString local_path = NormalizeLocalPath(path);
    std::error_code error;
    if (!std::filesystem::is_regular_file(local_path.toStdString(), error)) {
      qWarning() << "Send files failed: path is not a regular file"
                 << local_path;
      return false;
    }
    native_paths.push_back(local_path.toStdString());
  }
  const auto target = targets_.FindTarget(share_target_id);
  if (!target.has_value()) {
    qWarning() << "Send files failed: unknown target" << share_target_id;
    return false;
  }

  transfers_.PrepareOutgoingTransfer(
      share_target_id,
      paths.size() == 1 ? NormalizeLocalPath(paths.first()) : QString(),
      target);
  std::vector<const char*> c_paths;
  for (const std::string& path : native_paths) {
    c_paths.push_back(path.c_str());
  }
  quickshare_send_files(
      qs_, share_target_id, c_paths.data(), static_cast<int>(c_paths.size()),
      StatusThunk,
      PostedStatus(this, [share_target_id](Backend& backend, int status) {
        if (backend.shutting_down_) {
          return;
        }
        backend.ReportStatus(QStringLiteral("Send files"), status);
        if (status != QUICKSHARE_OK) {
          emit backend.outgoingTransferStartFailed(share_target_id);
        }
      }));
  return true;
}

qint64 Backend::fileSize(const QString& path) const {
  std::error_code error;
  const auto size = std::filesystem::file_size(
      NormalizeLocalPath(path).toStdString(), error);
  return error ? 0 : static_cast<qint64>(size);
}

QString Backend::formatSize(qint64 bytes) const {
  return QLocale().formattedDataSize(bytes, 1, QLocale::DataSizeSIFormat);
}

void Backend::setReceiveEnabled(bool enabled) {
  if (receive_enabled_ == enabled) {
    return;
  }
  receive_enabled_ = enabled;
  emit receiveEnabledChanged();
  RefreshIssues();
  if (qs_ == nullptr || shutting_down_ || active_mode_ == Mode::kDiscovery ||
      desired_mode_ == Mode::kDiscovery || fast_init_scan_stop_in_flight_) {
    return;
  }
  if (!enabled && !incoming_offer_timers_.isEmpty()) {
    return;
  }
  StartFastInitiationMonitoring();
}

void Backend::accept(qint64 share_target_id) {
  if (qs_ != nullptr && !shutting_down_) {
    ResolveIncomingOffer(share_target_id);
    quickshare_accept(
        qs_, share_target_id, StatusThunk,
        PostedStatus(this, [](Backend& backend, int status) {
          if (!backend.shutting_down_) {
            backend.ReportStatus(QStringLiteral("Accept transfer"), status);
          }
        }));
  }
}

void Backend::reject(qint64 share_target_id) {
  if (qs_ != nullptr && !shutting_down_) {
    ResolveIncomingOffer(share_target_id);
    quickshare_reject(
        qs_, share_target_id, StatusThunk,
        PostedStatus(this, [](Backend& backend, int status) {
          if (!backend.shutting_down_) {
            backend.ReportStatus(QStringLiteral("Reject transfer"), status);
          }
        }));
  }
}

void Backend::cancel(qint64 share_target_id) {
  if (qs_ != nullptr && !shutting_down_) {
    quickshare_cancel(
        qs_, share_target_id, StatusThunk,
        PostedStatus(this, [](Backend& backend, int status) {
          if (!backend.shutting_down_) {
            backend.ReportStatus(QStringLiteral("Cancel transfer"), status);
          }
        }));
  }
}

void Backend::shutdown() {
  if (qs_ == nullptr || shutting_down_) {
    return;
  }
  shutting_down_ = true;
  monitoring_requested_ = false;
  receive_timeout_timer_.stop();
  issues_timer_.stop();
  const QList<QTimer*> offer_timers = incoming_offer_timers_.values();
  incoming_offer_timers_.clear();
  for (QTimer* timer : offer_timers) {
    timer->stop();
  }
  if (quickshare_fast_init_is_scanning(qs_)) {
    quickshare_fast_init_stop_scanning(qs_, nullptr, nullptr);
  }
  desired_mode_ = Mode::kNone;
  const int status = quickshare_shutdown(qs_);
  if (status != QUICKSHARE_OK) {
    char description[128] = {};
    quickshare_status_string(status, description, sizeof(description));
    qWarning() << "Nearby Sharing shutdown failed:" << description;
  }
}

// The library calls the static callbacks below on its own threads. Each one
// copies its arguments into Qt types before queueing work for the GUI thread.
void Backend::TargetFound(const QuickShareTarget* target_data, void* user) {
  auto* backend = static_cast<Backend*>(user);
  const ShareTarget target = ToTargetInfo(*target_data);
  PostStatus(backend, [backend, target]() {
    if (backend->shutting_down_) {
      return;
    }
    backend->targets_.ApplyTarget(target);
    backend->transfers_.ApplyTarget(target);
  });
}

void Backend::TargetLost(const QuickShareTarget* target_data, void* user) {
  auto* backend = static_cast<Backend*>(user);
  const int64_t id = target_data->id;
  PostStatus(backend, [backend, id]() {
    if (!backend->shutting_down_) {
      backend->targets_.RemoveTarget(id);
    }
  });
}

void Backend::TransferUpdate(const QuickShareTransfer* transfer_data,
                             void* user) {
  auto* backend = static_cast<Backend*>(user);
  const bool receive_mode = transfer_data->receive_mode != 0;
  const ShareTarget target = ToTargetInfo(transfer_data->target);
  const TransferMetadata transfer = ToTransferInfo(*transfer_data);
  const int64_t total_bytes = transfer_data->total_bytes;
  const int attachment_count = transfer_data->attachment_count;
  PostStatus(backend, [backend, receive_mode, target, transfer, total_bytes,
                       attachment_count]() {
    if (!backend->shutting_down_) {
      backend->OnTransferUpdate(receive_mode, target, transfer, total_bytes,
                                attachment_count);
    }
  });
}

void Backend::OnTransferUpdate(bool receive_mode, const ShareTarget& target,
                               const TransferMetadata& transfer,
                               int64_t total_bytes, int attachment_count) {
  targets_.ApplyTarget(target);
  transfers_.ApplyTransfer(receive_mode, target, transfer, total_bytes);
  if (!transfer.is_final_status) {
    finished_transfer_ids_.remove(target.id);
  }
  if (receive_mode &&
      transfer.status == QUICKSHARE_TRANSFER_AWAITING_LOCAL_CONFIRMATION) {
    receive_offer_received_ = true;
    receive_timeout_timer_.stop();
    StartIncomingOfferTimer(target.id);
    emit incomingTransfer(target.id);
    emit incomingOffer(target.id, target.device_name, attachment_count,
                       total_bytes);
    return;
  }

  if (receive_mode) {
    ResolveIncomingOffer(target.id);
  }
  if (transfer.is_final_status && !finished_transfer_ids_.contains(target.id)) {
    finished_transfer_ids_.insert(target.id);
    emit transferFinished(target.id, receive_mode, target.device_name,
                          transfer.status_name);
    if (receive_mode) {
      StartFastInitiationMonitoring();
    }
  }
}

void Backend::StartIncomingOfferTimer(qint64 share_target_id) {
  ResolveIncomingOffer(share_target_id);
  auto* timer = new QTimer(this);
  timer->setSingleShot(true);
  timer->setInterval(kIncomingOfferTimeoutMilliseconds);
  connect(timer, &QTimer::timeout, this, [this, share_target_id]() {
    OnIncomingOfferTimeout(share_target_id);
  });
  incoming_offer_timers_.insert(share_target_id, timer);
  timer->start();
}

void Backend::ResolveIncomingOffer(qint64 share_target_id) {
  QTimer* timer = incoming_offer_timers_.take(share_target_id);
  if (timer == nullptr) {
    return;
  }
  timer->stop();
  timer->deleteLater();
  emit incomingOfferResolved(share_target_id);
}

void Backend::OnIncomingOfferTimeout(qint64 share_target_id) {
  if (!incoming_offer_timers_.contains(share_target_id)) {
    return;
  }
  ResolveIncomingOffer(share_target_id);
  if (qs_ != nullptr && !shutting_down_) {
    quickshare_reject(
        qs_, share_target_id, StatusThunk,
        PostedStatus(this, [](Backend& backend, int status) {
          if (!backend.shutting_down_) {
            backend.ReportStatus(QStringLiteral("Expire incoming offer"),
                                 status);
          }
        }));
  }
}

void Backend::SetDesiredMode(Mode mode) {
  desired_mode_ = mode;
  DriveMode();
}

void Backend::DriveMode() {
  if (!initialized_ || qs_ == nullptr || shutting_down_ ||
      mode_operation_in_flight_ || active_mode_ == desired_mode_) {
    return;
  }

  mode_operation_in_flight_ = true;
  if (active_mode_ == Mode::kReceive) {
    quickshare_unregister_receive_surface(
        qs_, StatusThunk,
        PostedStatus(this, [](Backend& backend, int status) {
          backend.OnModeStopped(Mode::kReceive, status);
        }));
    return;
  }
  if (active_mode_ == Mode::kDiscovery) {
    quickshare_unregister_send_surface(
        qs_, StatusThunk,
        PostedStatus(this, [](Backend& backend, int status) {
          backend.OnModeStopped(Mode::kDiscovery, status);
        }));
    return;
  }

  const Mode mode = desired_mode_;
  if (mode == Mode::kReceive) {
    quickshare_register_receive_surface(
        qs_, StatusThunk,
        PostedStatus(this, [mode](Backend& backend, int status) {
          backend.OnModeStarted(mode, status);
        }));
    return;
  }
  if (mode == Mode::kDiscovery) {
    quickshare_register_send_surface(
        qs_, StatusThunk,
        PostedStatus(this, [mode](Backend& backend, int status) {
          backend.OnModeStarted(mode, status);
        }));
    return;
  }

  mode_operation_in_flight_ = false;
}

void Backend::OnModeStopped(Mode mode, int status) {
  mode_operation_in_flight_ = false;
  if (shutting_down_) {
    return;
  }
  if (status == QUICKSHARE_OK || status == QUICKSHARE_ALREADY_STOPPED) {
    active_mode_ = Mode::kNone;
    if (mode == Mode::kDiscovery) {
      targets_.Clear();
    }
  } else {
    desired_mode_ = active_mode_;
    ReportStatus(QStringLiteral("Stop sharing mode"), status);
  }
  DriveMode();
  MaybeStartFastInitiationScanning();
}

void Backend::OnModeStarted(Mode mode, int status) {
  mode_operation_in_flight_ = false;
  if (shutting_down_) {
    return;
  }
  if (status == QUICKSHARE_OK) {
    active_mode_ = mode;
    if (mode == Mode::kReceive && receive_window_pending_) {
      receive_window_pending_ = false;
      receive_offer_received_ = false;
      receive_timeout_timer_.start();
    }
  } else {
    desired_mode_ = Mode::kNone;
    receive_window_pending_ = false;
    receive_timeout_timer_.stop();
    if (fallback_receive_window_) {
      monitoring_requested_ = false;
    }
    ReportStatus(QStringLiteral("Start sharing mode"), status);
  }
  DriveMode();
  MaybeStartFastInitiationScanning();
}

void Backend::StartFastInitiationMonitoring() {
  monitoring_requested_ = true;
  fallback_receive_used_ = false;
  fallback_receive_window_ = false;
  receive_window_pending_ = false;
  receive_offer_received_ = false;
  receive_timeout_timer_.stop();
  SetDesiredMode(receive_enabled_ ? Mode::kReceive : Mode::kNone);
  MaybeStartFastInitiationScanning();
}

void Backend::MaybeStartFastInitiationScanning() {
  if (!initialized_ || qs_ == nullptr || shutting_down_ ||
      !monitoring_requested_ || fast_init_scan_stop_in_flight_ ||
      mode_operation_in_flight_ || active_mode_ != Mode::kNone ||
      desired_mode_ != Mode::kNone) {
    return;
  }

  if (fallback_receive_window_ && fallback_receive_used_) {
    OpenReceiveWindow(/*fallback=*/true);
    return;
  }

  if (quickshare_fast_init_is_scanning(qs_)) {
    return;
  }

  sender_present_ = false;
  receive_trigger_armed_ = true;
  const QuickShareFastInitCallbacks callbacks = {
      &Backend::FastInitDevicesDiscovered,
      &Backend::FastInitDevicesNotDiscovered, &Backend::FastInitError, this};
  quickshare_fast_init_start_scanning(qs_, &callbacks);
}

void Backend::FastInitDevicesDiscovered(void* user) {
  auto* backend = static_cast<Backend*>(user);
  PostStatus(backend, [backend]() {
    backend->OnFastInitiationDevicesDiscovered();
  });
}

void Backend::FastInitDevicesNotDiscovered(void* user) {
  auto* backend = static_cast<Backend*>(user);
  PostStatus(backend, [backend]() {
    backend->OnFastInitiationDevicesNotDiscovered();
  });
}

void Backend::FastInitError(int error, void* user) {
  auto* backend = static_cast<Backend*>(user);
  PostStatus(backend, [backend, error]() {
    backend->OnFastInitiationScanningError(error);
  });
}

void Backend::FastInitScanningStopped(void* user) {
  auto* backend = static_cast<Backend*>(user);
  PostStatus(backend, [backend]() {
    backend->OnFastInitiationScanningStoppedForDiscovery();
  });
}

void Backend::StopFastInitiationScanningForDiscovery() {
  if (!quickshare_fast_init_is_scanning(qs_)) {
    SetDesiredMode(Mode::kDiscovery);
    return;
  }
  if (fast_init_scan_stop_in_flight_) {
    return;
  }

  fast_init_scan_stop_in_flight_ = true;
  quickshare_fast_init_stop_scanning(qs_, &Backend::FastInitScanningStopped,
                                     this);
}

void Backend::OnFastInitiationScanningStoppedForDiscovery() {
  fast_init_scan_stop_in_flight_ = false;
  sender_present_ = false;
  receive_trigger_armed_ = true;
  if (!shutting_down_ && !monitoring_requested_) {
    SetDesiredMode(Mode::kDiscovery);
  } else {
    MaybeStartFastInitiationScanning();
  }
}

void Backend::OnFastInitiationDevicesDiscovered() {
  if (shutting_down_ || !monitoring_requested_) {
    return;
  }
  sender_present_ = true;
  const bool should_open_receive = receive_trigger_armed_;
  receive_trigger_armed_ = false;
  if (!should_open_receive || active_mode_ != Mode::kNone ||
      desired_mode_ != Mode::kNone || mode_operation_in_flight_) {
    return;
  }
  OpenReceiveWindow(/*fallback=*/false);
}

void Backend::OnFastInitiationDevicesNotDiscovered() {
  if (shutting_down_ || !monitoring_requested_) {
    return;
  }
  sender_present_ = false;
  receive_trigger_armed_ = true;
}

void Backend::OnFastInitiationScanningError(int error) {
  if (shutting_down_ || !monitoring_requested_) {
    return;
  }
  qWarning() << "Fast Initiation scanning failed with error" << error;
  if (fallback_receive_used_) {
    monitoring_requested_ = false;
    return;
  }

  fallback_receive_used_ = true;
  fallback_receive_window_ = true;
  if (active_mode_ == Mode::kReceive || desired_mode_ == Mode::kReceive) {
    return;
  }
  if (active_mode_ == Mode::kNone && desired_mode_ == Mode::kNone &&
      !mode_operation_in_flight_) {
    OpenReceiveWindow(/*fallback=*/true);
  }
}

void Backend::OpenReceiveWindow(bool fallback) {
  fallback_receive_window_ = fallback;
  receive_window_pending_ = true;
  receive_offer_received_ = false;
  SetDesiredMode(Mode::kReceive);
}

void Backend::OnReceiveTimeout() {
  if (shutting_down_ || receive_offer_received_ ||
      active_mode_ != Mode::kReceive) {
    return;
  }
  if (fallback_receive_window_) {
    monitoring_requested_ = false;
  }
  receive_window_pending_ = false;
  fallback_receive_window_ = false;
  SetDesiredMode(Mode::kNone);
}

void Backend::ReportStatus(const QString& operation, int status) {
  if (status != QUICKSHARE_OK) {
    char description[128] = {};
    quickshare_status_string(status, description, sizeof(description));
    qWarning().noquote() << operation + QStringLiteral(" failed: ") +
                                QString::fromUtf8(description);
  }
}

void Backend::RefreshIssues() {
  const QVariantList issues = BuildIssues();
  if (issues != issues_) {
    issues_ = issues;
    emit issuesChanged();
  }
}

QVariantList Backend::BuildIssues() const {
  QVariantList issues;
  if (qs_ == nullptr || shutting_down_) {
    return issues;
  }
  if (!quickshare_is_bluetooth_powered(qs_)) {
    issues.push_back(CreateIssue(
        QStringLiteral("bluetooth"), QStringLiteral("error"),
        QStringLiteral("Bluetooth is off"),
        QStringLiteral(
            "Turn Bluetooth on so nearby devices can find this PC.")));
  }
  if (active_mode_ == Mode::kReceive && quickshare_ble_advertising_ok(qs_) == 0) {
    char error[256] = {};
    quickshare_copy_ble_advertising_error(qs_, error, sizeof(error));
    issues.push_back(CreateIssue(
        QStringLiteral("advertising"), QStringLiteral("error"),
        QStringLiteral("Phones can't find this PC"),
        QStringLiteral("Bluetooth advertising failed (%1). Restart the "
                       "Bluetooth service and try again.")
            .arg(QString::fromUtf8(error))));
  }
  if (quickshare_pairing_prompt_count(qs_) > 0) {
    issues.push_back(CreateIssue(
        QStringLiteral("pairing"), QStringLiteral("warning"),
        QStringLiteral("Bluetooth pairing request"),
        QStringLiteral(
            "BlueZ is asking to pair with a device. Confirm the prompt on "
            "this PC and on the phone to continue.")));
  }
  if (!wifi_direct_ && !quickshare_is_lan_connected(qs_)) {
    issues.push_back(CreateIssue(
        QStringLiteral("slow"), QStringLiteral("info"),
        QStringLiteral("Large files will be slow"),
        QStringLiteral("No Wi-Fi Direct or local network found, so transfers "
                       "stay on Bluetooth.")));
  }
  return issues;
}
