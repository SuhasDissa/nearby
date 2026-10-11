#include "sharing/linux/app/backend.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <mutex>
#include <utility>

#include <QDebug>
#include <QLocale>
#include <QMetaObject>
#include <QPointer>
#include <QSysInfo>
#include <QUrl>
#include <QVariantMap>

#include "absl/time/time.h"
#include "connections/implementation/flags/nearby_connections_feature_flags.h"
#include "internal/base/file_path.h"
#include "internal/flags/nearby_flags.h"
#include "internal/platform/implementation/platform.h"
#include "sharing/advertisement.h"
#include "sharing/file_attachment.h"
#include "sharing/flags/generated/nearby_sharing_feature_flags.h"
#include "sharing/nearby_sharing_service_factory.h"
#include "sharing/nearby_sharing_settings.h"
#include "sharing/proto/enums.pb.h"

extern "C" int nearby_linux_pairing_prompt_count();
extern "C" int nearby_linux_ble_advertising_ok();
extern "C" void nearby_linux_copy_ble_advertising_error(char* buffer,
                                                       int buffer_size);

namespace {

using NearbySharingService = nearby::sharing::NearbySharingService;

constexpr std::chrono::seconds kShutdownTimeout(10);
constexpr int kReceiveTimeoutMilliseconds = 30 * 1000;
constexpr int kIncomingOfferTimeoutMilliseconds = 45 * 1000;
constexpr int kIssuesRefreshMilliseconds = 2000;
constexpr qint64 kSpeedWindowMilliseconds = 1000;
constexpr qint64 kMinSpeedSampleMilliseconds = 250;

QString ToQString(const std::string& value) {
  return QString::fromStdString(value);
}

QString NormalizeLocalPath(const QString& path) {
  const QUrl url(path);
  return url.isValid() && url.isLocalFile() ? url.toLocalFile() : path;
}

void ConfigureFlags() {
  nearby::NearbyFlags::GetInstance().OverrideBoolFlagValue(
      nearby::sharing::config_package_nearby::nearby_sharing_feature::
          kEnableBleForTransfer,
      true);
  nearby::NearbyFlags::GetInstance().OverrideBoolFlagValue(
      nearby::connections::config_package_nearby::nearby_connections_feature::
          kEnableBleL2cap,
      true);
  nearby::NearbyFlags::GetInstance().OverrideBoolFlagValue(
      nearby::connections::config_package_nearby::nearby_connections_feature::
          kRefactorBleL2cap,
      true);
  nearby::NearbyFlags::GetInstance().OverrideBoolFlagValue(
      nearby::connections::config_package_nearby::nearby_connections_feature::
          kEnableDynamicRoleSwitch,
      true);
  nearby::NearbyFlags::GetInstance().OverrideBoolFlagValue(
      nearby::connections::config_package_nearby::nearby_connections_feature::
          kEnableWifiDirect,
      true);
  nearby::NearbyFlags::GetInstance().OverrideBoolFlagValue(
      nearby::connections::config_package_nearby::nearby_connections_feature::
          kEnableWifiDirectGcOnly,
      true);
}

std::unique_ptr<nearby::sharing::AttachmentContainer> CreateFileAttachments(
    const std::vector<std::string>& paths) {
  nearby::sharing::AttachmentContainer::Builder builder;
  for (const std::string& path : paths) {
    builder.AddFileAttachment(
        nearby::sharing::FileAttachment(nearby::FilePath(path)));
  }
  return builder.Build();
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

NearbySharingService::StatusCodes ShutdownService(
    NearbySharingService& service) {
  struct State {
    std::mutex mutex;
    std::condition_variable cv;
    std::optional<NearbySharingService::StatusCodes> status;
  };
  auto state = std::make_shared<State>();
  service.Shutdown([state](NearbySharingService::StatusCodes status) {
    {
      std::lock_guard<std::mutex> lock(state->mutex);
      state->status = status;
    }
    state->cv.notify_one();
  });

  std::unique_lock<std::mutex> lock(state->mutex);
  if (!state->cv.wait_for(lock, kShutdownTimeout,
                          [&] { return state->status.has_value(); })) {
    return NearbySharingService::StatusCodes::kError;
  }
  return *state->status;
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
      return ToQString(target.device_name);
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
      row.transfer->status() != TransferMetadata::Status::kInProgress) {
    row.speed_samples.clear();
    row.speed_text.clear();
    return;
  }

  const qint64 now = clock_.elapsed();
  row.speed_samples.push_back(
      {now, static_cast<qint64>(row.transfer->transferred_bytes())});
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
      return target.has_value() && !target->device_name.empty()
                 ? ToQString(target->device_name)
                 : QStringLiteral("Unknown device");
    case StatusRole:
      return transfer.has_value() ? ToQString(TransferMetadata::StatusToString(
                                        transfer->status()))
                                  : QString();
    case ProgressRole:
      return transfer.has_value()
                 ? std::clamp(static_cast<double>(transfer->progress()) / 100.0,
                              0.0, 1.0)
                 : 0.0;
    case TransferredBytesRole:
      return QVariant::fromValue<qulonglong>(
          transfer.has_value() ? transfer->transferred_bytes() : 0);
    case TotalBytesRole:
      return QVariant::fromValue<qint64>(row.total_bytes);
    case TotalAttachmentsCountRole:
      return transfer.has_value() ? transfer->total_attachments_count() : 0;
    case TransferredAttachmentsCountRole:
      return transfer.has_value() ? transfer->transferred_attachments_count()
                                  : 0;
    case IsFinalStatusRole:
      return transfer.has_value() && transfer->is_final_status();
    case AwaitingLocalConfirmationRole:
      return transfer.has_value() &&
             transfer->status() ==
                 TransferMetadata::Status::kAwaitingLocalConfirmation;
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

void Backend::TransferCallback::OnTransferUpdate(
    const ShareTarget& share_target,
    const nearby::sharing::AttachmentContainer& attachment_container,
    const TransferMetadata& transfer_metadata) {
  backend_.OnTransferUpdate(receive_mode_, share_target, attachment_container,
                            transfer_metadata);
}

Backend::Backend(QObject* parent)
    : QObject(parent),
      targets_(this),
      transfers_(this),
      platform_(
          std::make_unique<nearby::sharing::linux::LinuxSharingPlatform>()),
      fast_init_manager_(&platform_->GetFastInitiationManager()),
      send_transfer_callback_(*this, false),
      receive_transfer_callback_(*this, true) {
  receive_timeout_timer_.setSingleShot(true);
  receive_timeout_timer_.setInterval(kReceiveTimeoutMilliseconds);
  connect(&receive_timeout_timer_, &QTimer::timeout, this,
          &Backend::OnReceiveTimeout);

  ConfigureFlags();
  try {
    wifi_direct_ =
        nearby::api::ImplementationPlatform::CreateWifiDirectMedium() !=
        nullptr;
  } catch (...) {
    wifi_direct_ = false;
  }
  service_ = nearby::sharing::NearbySharingServiceFactory::GetInstance()
                 ->CreateSharingService(*platform_, &analytics_recorder_,
                                        /*event_logger=*/nullptr,
                                        /*supports_file_sync=*/false);
  if (service_ == nullptr) {
    qCritical() << "Failed to create NearbySharingService";
    desired_mode_ = Mode::kNone;
    return;
  }

  issues_timer_.setInterval(kIssuesRefreshMilliseconds);
  connect(&issues_timer_, &QTimer::timeout, this, &Backend::RefreshIssues);
  issues_timer_.start();
  RefreshIssues();

  service_->GetSettings()->SetDataUsage(
      nearby::sharing::proto::WIFI_ONLY_DATA_USAGE);
  QPointer<Backend> backend(this);
  service_->SetVisibility(
      nearby::sharing::proto::DEVICE_VISIBILITY_EVERYONE, absl::ZeroDuration(),
      [backend](NearbySharingService::StatusCodes status) mutable {
        if (!backend) {
          return;
        }
        PostStatus(backend, [backend, status]() {
          if (!backend || backend->shutting_down_) {
            return;
          }
          backend->initialized_ =
              status == NearbySharingService::StatusCodes::kOk;
          backend->ReportStatus(QStringLiteral("Initialize"), status);
          if (backend->initialized_) {
            backend->StartFastInitiationMonitoring();
          } else {
            backend->desired_mode_ = Mode::kNone;
          }
        });
      });
}

Backend::Backend(NearbySharingService& service,
                 nearby::api::FastInitiationManager& fast_init_manager,
                 int receive_timeout_milliseconds, QObject* parent)
    : QObject(parent),
      targets_(this),
      transfers_(this),
      fast_init_manager_(&fast_init_manager),
      service_(&service),
      send_transfer_callback_(*this, false),
      receive_transfer_callback_(*this, true),
      initialized_(true) {
  receive_timeout_timer_.setSingleShot(true);
  receive_timeout_timer_.setInterval(receive_timeout_milliseconds);
  connect(&receive_timeout_timer_, &QTimer::timeout, this,
          &Backend::OnReceiveTimeout);
  StartFastInitiationMonitoring();
}

Backend::~Backend() {
  shutdown();
}

QString Backend::hostname() const {
  return QSysInfo::machineHostName();
}

void Backend::startReceive() {
  if (service_ == nullptr || shutting_down_) {
    return;
  }
  StartFastInitiationMonitoring();
}

void Backend::startDiscovery() {
  if (service_ == nullptr || shutting_down_) {
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
  if (service_ == nullptr || shutting_down_ || paths.isEmpty()) {
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
  QPointer<Backend> backend(this);
  service_->SendAttachments(
      share_target_id, CreateFileAttachments(native_paths),
      [backend, share_target_id](NearbySharingService::StatusCodes status) {
        if (!backend) {
          return;
        }
        PostStatus(backend, [backend, share_target_id, status]() {
          if (!backend || backend->shutting_down_) {
            return;
          }
          backend->ReportStatus(QStringLiteral("Send files"), status);
          if (status != NearbySharingService::StatusCodes::kOk) {
            emit backend->outgoingTransferStartFailed(share_target_id);
          }
        });
      });
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
  if (service_ == nullptr || shutting_down_ ||
      active_mode_ == Mode::kDiscovery || desired_mode_ == Mode::kDiscovery ||
      fast_init_scan_stop_in_flight_) {
    return;
  }
  if (!enabled && !incoming_offer_timers_.isEmpty()) {
    return;
  }
  StartFastInitiationMonitoring();
}

void Backend::accept(qint64 share_target_id) {
  if (service_ != nullptr && !shutting_down_) {
    ResolveIncomingOffer(share_target_id);
    service_->Accept(share_target_id,
                     StatusCallback(QStringLiteral("Accept transfer")));
  }
}

void Backend::reject(qint64 share_target_id) {
  if (service_ != nullptr && !shutting_down_) {
    ResolveIncomingOffer(share_target_id);
    service_->Reject(share_target_id,
                     StatusCallback(QStringLiteral("Reject transfer")));
  }
}

void Backend::cancel(qint64 share_target_id) {
  if (service_ != nullptr && !shutting_down_) {
    service_->Cancel(share_target_id,
                     StatusCallback(QStringLiteral("Cancel transfer")));
  }
}

void Backend::shutdown() {
  if (service_ == nullptr || shutting_down_) {
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
  auto& fast_init_manager = *fast_init_manager_;
  if (fast_init_manager.IsScanning()) {
    fast_init_manager.StopScanning(nullptr);
  }
  desired_mode_ = Mode::kNone;
  const auto status = ShutdownService(*service_);
  if (status != NearbySharingService::StatusCodes::kOk) {
    qWarning() << "Nearby Sharing shutdown failed:"
               << NearbySharingService::StatusCodeToString(status);
  }
  service_ = nullptr;
}

void Backend::OnShareTargetDiscovered(const ShareTarget& target) {
  QPointer<Backend> backend(this);
  PostStatus(this, [backend, target]() {
    if (!backend || backend->shutting_down_) {
      return;
    }
    backend->targets_.ApplyTarget(target);
    backend->transfers_.ApplyTarget(target);
  });
}

void Backend::OnShareTargetUpdated(const ShareTarget& target) {
  OnShareTargetDiscovered(target);
}

void Backend::OnShareTargetLost(const ShareTarget& target) {
  QPointer<Backend> backend(this);
  PostStatus(this, [backend, id = target.id]() {
    if (backend && !backend->shutting_down_) {
      backend->targets_.RemoveTarget(id);
    }
  });
}

void Backend::OnTransferUpdate(
    bool receive_mode, const ShareTarget& target,
    const nearby::sharing::AttachmentContainer& attachments,
    const TransferMetadata& transfer) {
  const int64_t total_bytes = attachments.GetTotalAttachmentsSize();
  const int attachment_count = attachments.GetAttachmentCount();
  QPointer<Backend> backend(this);
  PostStatus(this, [backend, receive_mode, target, transfer, total_bytes,
                    attachment_count]() {
    if (!backend || backend->shutting_down_) {
      return;
    }
    backend->targets_.ApplyTarget(target);
    backend->transfers_.ApplyTransfer(receive_mode, target, transfer,
                                      total_bytes);
    if (!transfer.is_final_status()) {
      backend->finished_transfer_ids_.remove(target.id);
    }
    if (receive_mode &&
        transfer.status() ==
            TransferMetadata::Status::kAwaitingLocalConfirmation) {
      backend->receive_offer_received_ = true;
      backend->receive_timeout_timer_.stop();
      backend->StartIncomingOfferTimer(target.id);
      emit backend->incomingTransfer(target.id);
      emit backend->incomingOffer(target.id, ToQString(target.device_name),
                                  attachment_count, total_bytes);
      return;
    }

    if (receive_mode) {
      backend->ResolveIncomingOffer(target.id);
    }
    if (transfer.is_final_status() &&
        !backend->finished_transfer_ids_.contains(target.id)) {
      backend->finished_transfer_ids_.insert(target.id);
      emit backend->transferFinished(
          target.id, receive_mode, ToQString(target.device_name),
          ToQString(TransferMetadata::StatusToString(transfer.status())));
      if (receive_mode) {
        backend->StartFastInitiationMonitoring();
      }
    }
  });
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
  if (service_ != nullptr && !shutting_down_) {
    service_->Reject(share_target_id,
                     StatusCallback(QStringLiteral("Expire incoming offer")));
  }
}

void Backend::SetDesiredMode(Mode mode) {
  desired_mode_ = mode;
  DriveMode();
}

void Backend::DriveMode() {
  if (!initialized_ || service_ == nullptr || shutting_down_ ||
      mode_operation_in_flight_ || active_mode_ == desired_mode_) {
    return;
  }

  mode_operation_in_flight_ = true;
  QPointer<Backend> backend(this);
  if (active_mode_ == Mode::kReceive) {
    service_->UnregisterReceiveSurface(
        &receive_transfer_callback_,
        [backend](NearbySharingService::StatusCodes status) mutable {
          if (backend) {
            PostStatus(backend, [backend, status]() {
              if (backend) {
                backend->OnModeStopped(Mode::kReceive, status);
              }
            });
          }
        });
    return;
  }
  if (active_mode_ == Mode::kDiscovery) {
    service_->UnregisterSendSurface(
        &send_transfer_callback_,
        [backend](NearbySharingService::StatusCodes status) mutable {
          if (backend) {
            PostStatus(backend, [backend, status]() {
              if (backend) {
                backend->OnModeStopped(Mode::kDiscovery, status);
              }
            });
          }
        });
    return;
  }

  const Mode mode = desired_mode_;
  if (mode == Mode::kReceive) {
    service_->RegisterReceiveSurface(
        &receive_transfer_callback_,
        NearbySharingService::ReceiveSurfaceState::kForeground,
        nearby::sharing::Advertisement::BlockedVendorId::kNone,
        [backend, mode](NearbySharingService::StatusCodes status) mutable {
          if (backend) {
            PostStatus(backend, [backend, mode, status]() {
              if (backend) {
                backend->OnModeStarted(mode, status);
              }
            });
          }
        });
    return;
  }
  if (mode == Mode::kDiscovery) {
    service_->RegisterSendSurface(
        &send_transfer_callback_, this,
        NearbySharingService::SendSurfaceState::kForeground,
        nearby::sharing::Advertisement::BlockedVendorId::kNone,
        /*disable_wifi_hotspot=*/false,
        [backend, mode](NearbySharingService::StatusCodes status) mutable {
          if (backend) {
            PostStatus(backend, [backend, mode, status]() {
              if (backend) {
                backend->OnModeStarted(mode, status);
              }
            });
          }
        });
    return;
  }

  mode_operation_in_flight_ = false;
}

void Backend::OnModeStopped(Mode mode,
                            NearbySharingService::StatusCodes status) {
  mode_operation_in_flight_ = false;
  if (shutting_down_) {
    return;
  }
  if (status == NearbySharingService::StatusCodes::kOk ||
      status == NearbySharingService::StatusCodes::kStatusAlreadyStopped) {
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

void Backend::OnModeStarted(Mode mode,
                            NearbySharingService::StatusCodes status) {
  mode_operation_in_flight_ = false;
  if (shutting_down_) {
    return;
  }
  if (status == NearbySharingService::StatusCodes::kOk) {
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
  if (!initialized_ || service_ == nullptr || shutting_down_ ||
      !monitoring_requested_ || fast_init_scan_stop_in_flight_ ||
      mode_operation_in_flight_ || active_mode_ != Mode::kNone ||
      desired_mode_ != Mode::kNone) {
    return;
  }

  if (fallback_receive_window_ && fallback_receive_used_) {
    OpenReceiveWindow(/*fallback=*/true);
    return;
  }

  auto& fast_init_manager = *fast_init_manager_;
  if (fast_init_manager.IsScanning()) {
    return;
  }

  sender_present_ = false;
  receive_trigger_armed_ = true;
  QPointer<Backend> backend(this);
  fast_init_manager.StartScanning(
      [backend]() {
        if (backend) {
          PostStatus(backend, [backend]() {
            if (backend) {
              backend->OnFastInitiationDevicesDiscovered();
            }
          });
        }
      },
      [backend]() {
        if (backend) {
          PostStatus(backend, [backend]() {
            if (backend) {
              backend->OnFastInitiationDevicesNotDiscovered();
            }
          });
        }
      },
      [backend](nearby::api::FastInitiationManager::Error error) {
        if (backend) {
          PostStatus(backend, [backend, error]() {
            if (backend) {
              backend->OnFastInitiationScanningError(error);
            }
          });
        }
      });
}

void Backend::StopFastInitiationScanningForDiscovery() {
  auto& fast_init_manager = *fast_init_manager_;
  if (!fast_init_manager.IsScanning()) {
    SetDesiredMode(Mode::kDiscovery);
    return;
  }
  if (fast_init_scan_stop_in_flight_) {
    return;
  }

  fast_init_scan_stop_in_flight_ = true;
  QPointer<Backend> backend(this);
  fast_init_manager.StopScanning([backend]() {
    if (backend) {
      PostStatus(backend, [backend]() {
        if (backend) {
          backend->OnFastInitiationScanningStoppedForDiscovery();
        }
      });
    }
  });
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

void Backend::OnFastInitiationScanningError(
    nearby::api::FastInitiationManager::Error error) {
  if (shutting_down_ || !monitoring_requested_) {
    return;
  }
  qWarning() << "Fast Initiation scanning failed with error"
             << static_cast<int>(error);
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

std::function<void(NearbySharingService::StatusCodes)> Backend::StatusCallback(
    QString operation) {
  QPointer<Backend> backend(this);
  return [backend, operation = std::move(operation)](
             NearbySharingService::StatusCodes status) {
    if (backend) {
      PostStatus(backend, [backend, operation, status]() {
        if (backend && !backend->shutting_down_) {
          backend->ReportStatus(operation, status);
        }
      });
    }
  };
}

void Backend::ReportStatus(const QString& operation,
                           NearbySharingService::StatusCodes status) {
  if (status != NearbySharingService::StatusCodes::kOk) {
    qWarning().noquote()
        << operation + QStringLiteral(" failed: ") +
               ToQString(NearbySharingService::StatusCodeToString(status));
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
  if (service_ == nullptr || shutting_down_) {
    return issues;
  }
  if (!service_->IsBluetoothPowered()) {
    issues.push_back(CreateIssue(
        QStringLiteral("bluetooth"), QStringLiteral("error"),
        QStringLiteral("Bluetooth is off"),
        QStringLiteral(
            "Turn Bluetooth on so nearby devices can find this PC.")));
  }
  if (active_mode_ == Mode::kReceive && nearby_linux_ble_advertising_ok() == 0) {
    char error[256] = {};
    nearby_linux_copy_ble_advertising_error(error, sizeof(error));
    issues.push_back(CreateIssue(
        QStringLiteral("advertising"), QStringLiteral("error"),
        QStringLiteral("Phones can't find this PC"),
        QStringLiteral("Bluetooth advertising failed (%1). Restart the "
                       "Bluetooth service and try again.")
            .arg(QString::fromUtf8(error))));
  }
  if (nearby_linux_pairing_prompt_count() > 0) {
    issues.push_back(CreateIssue(
        QStringLiteral("pairing"), QStringLiteral("warning"),
        QStringLiteral("Bluetooth pairing request"),
        QStringLiteral(
            "BlueZ is asking to pair with a device. Confirm the prompt on "
            "this PC and on the phone to continue.")));
  }
  if (!wifi_direct_ && !service_->IsLanConnected()) {
    issues.push_back(CreateIssue(
        QStringLiteral("slow"), QStringLiteral("info"),
        QStringLiteral("Large files will be slow"),
        QStringLiteral("No Wi-Fi Direct or local network found, so transfers "
                       "stay on Bluetooth.")));
  }
  return issues;
}
