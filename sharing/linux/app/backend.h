#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include <QAbstractListModel>
#include <QElapsedTimer>
#include <QHash>
#include <QList>
#include <QObject>
#include <QPair>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariantList>

#include <QtQmlIntegration/qqmlintegration.h>

#include "quickshare.h"

struct ShareTargetInfo {
  int64_t id = 0;
  QString device_name;
  int type = 0;
};

struct TransferInfo {
  int status = QUICKSHARE_TRANSFER_UNKNOWN;
  QString status_name;
  double progress_percent = 0;
  quint64 transferred_bytes = 0;
  int total_attachments_count = 0;
  int transferred_attachments_count = 0;
  bool is_final_status = false;
};

class ShareTargetModel : public QAbstractListModel {
  Q_OBJECT

 public:
  using ShareTarget = ShareTargetInfo;

  enum Role {
    IdRole = Qt::UserRole + 1,
    DeviceNameRole,
    TypeRole,
  };

  explicit ShareTargetModel(QObject* parent = nullptr);

  int rowCount(const QModelIndex& parent = QModelIndex()) const override;
  QVariant data(const QModelIndex& index, int role) const override;
  QHash<int, QByteArray> roleNames() const override;

  void ApplyTarget(const ShareTarget& target);
  void RemoveTarget(int64_t target_id);
  void Clear();
  std::optional<ShareTarget> FindTarget(int64_t target_id) const;

 private:
  int IndexOf(int64_t target_id) const;

  std::vector<ShareTarget> targets_;
};

class ShareTransferModel : public QAbstractListModel {
  Q_OBJECT

 public:
  using ShareTarget = ShareTargetInfo;
  using TransferMetadata = TransferInfo;

  enum Role {
    IdRole = Qt::UserRole + 1,
    DirectionRole,
    DeviceNameRole,
    StatusRole,
    ProgressRole,
    TransferredBytesRole,
    TotalBytesRole,
    TotalAttachmentsCountRole,
    TransferredAttachmentsCountRole,
    IsFinalStatusRole,
    AwaitingLocalConfirmationRole,
    LocalPathRole,
    SpeedTextRole,
  };

  explicit ShareTransferModel(QObject* parent = nullptr);

  int rowCount(const QModelIndex& parent = QModelIndex()) const override;
  QVariant data(const QModelIndex& index, int role) const override;
  QHash<int, QByteArray> roleNames() const override;

  void ApplyTarget(const ShareTarget& target);
  void ApplyTransfer(bool receive_mode, const ShareTarget& target,
                     const TransferMetadata& transfer, int64_t total_bytes);
  void PrepareOutgoingTransfer(int64_t target_id, const QString& local_path,
                               const std::optional<ShareTarget>& target);

 private:
  struct Row {
    int64_t id = 0;
    bool receive_mode = false;
    std::optional<ShareTarget> target;
    std::optional<TransferMetadata> transfer;
    int64_t total_bytes = 0;
    QString local_path;
    QList<QPair<qint64, qint64>> speed_samples;
    QString speed_text;
  };

  int IndexOf(int64_t target_id) const;
  void UpdateSpeed(Row& row);
  QVariant DataForRow(const Row& row, int role) const;
  void EmitRowChanged(int row);

  std::vector<Row> transfers_;
  QElapsedTimer clock_;
};

class Backend : public QObject {
  Q_OBJECT
  QML_ELEMENT
  Q_PROPERTY(QString hostname READ hostname CONSTANT)
  Q_PROPERTY(bool reduceMotion READ reduceMotion CONSTANT)
  Q_PROPERTY(QAbstractListModel* targets READ targets CONSTANT)
  Q_PROPERTY(QAbstractListModel* transfers READ transfers CONSTANT)
  Q_PROPERTY(QVariantList issues READ issues NOTIFY issuesChanged)
  Q_PROPERTY(bool receiveEnabled READ receiveEnabled WRITE setReceiveEnabled
                 NOTIFY receiveEnabledChanged)

 public:
  explicit Backend(QObject* parent = nullptr);
  ~Backend() override;

  QString hostname() const;
  bool reduceMotion() const;
  QAbstractListModel* targets() { return &targets_; }
  QAbstractListModel* transfers() { return &transfers_; }
  QVariantList issues() const { return issues_; }
  bool receiveEnabled() const { return receive_enabled_; }

  Q_INVOKABLE void startReceive();
  Q_INVOKABLE void startDiscovery();
  Q_INVOKABLE bool sendFiles(qint64 share_target_id, const QStringList& paths);
  Q_INVOKABLE qint64 fileSize(const QString& path) const;
  Q_INVOKABLE QString formatSize(qint64 bytes) const;
  Q_INVOKABLE void setReceiveEnabled(bool enabled);
  Q_INVOKABLE void accept(qint64 share_target_id);
  Q_INVOKABLE void reject(qint64 share_target_id);
  Q_INVOKABLE void cancel(qint64 share_target_id);
  void shutdown();

 signals:
  void incomingTransfer(qint64 share_target_id);
  void incomingOffer(qint64 share_target_id, QString device_name,
                     int attachment_count, qint64 total_bytes);
  void incomingOfferResolved(qint64 share_target_id);
  void transferFinished(qint64 share_target_id, bool receive_mode,
                        QString device_name, QString status);
  void outgoingTransferStartFailed(qint64 share_target_id);
  void issuesChanged();
  void receiveEnabledChanged();

 private:
  friend class BackendTestPeer;

  using ShareTarget = ShareTargetInfo;
  using TransferMetadata = TransferInfo;

  enum class Mode {
    kNone,
    kReceive,
    kDiscovery,
  };

  // Takes ownership of qs.
  Backend(QuickShare* qs, int receive_timeout_milliseconds, QObject* parent);

  void ConnectLibrary();
  static void TargetFound(const QuickShareTarget* target, void* user);
  static void TargetLost(const QuickShareTarget* target, void* user);
  static void TransferUpdate(const QuickShareTransfer* transfer, void* user);
  static void FastInitDevicesDiscovered(void* user);
  static void FastInitDevicesNotDiscovered(void* user);
  static void FastInitError(int error, void* user);
  static void FastInitScanningStopped(void* user);

  void OnTransferUpdate(bool receive_mode, const ShareTarget& target,
                        const TransferMetadata& transfer, int64_t total_bytes,
                        int attachment_count);

  void SetDesiredMode(Mode mode);
  void DriveMode();
  void OnModeStopped(Mode mode, int status);
  void OnModeStarted(Mode mode, int status);
  void StartFastInitiationMonitoring();
  void MaybeStartFastInitiationScanning();
  void StopFastInitiationScanningForDiscovery();
  void OnFastInitiationScanningStoppedForDiscovery();
  void OnFastInitiationDevicesDiscovered();
  void OnFastInitiationDevicesNotDiscovered();
  void OnFastInitiationScanningError(int error);
  void OpenReceiveWindow(bool fallback);
  void OnReceiveTimeout();
  void StartIncomingOfferTimer(qint64 share_target_id);
  void ResolveIncomingOffer(qint64 share_target_id);
  void OnIncomingOfferTimeout(qint64 share_target_id);
  void ReportStatus(const QString& operation, int status);
  void RefreshIssues();
  QVariantList BuildIssues() const;

  ShareTargetModel targets_;
  ShareTransferModel transfers_;
  QuickShare* qs_ = nullptr;
  QTimer receive_timeout_timer_;
  QTimer issues_timer_;
  QVariantList issues_;
  QHash<qint64, QTimer*> incoming_offer_timers_;
  QSet<qint64> finished_transfer_ids_;
  Mode active_mode_ = Mode::kNone;
  Mode desired_mode_ = Mode::kNone;
  bool initialized_ = false;
  bool mode_operation_in_flight_ = false;
  bool monitoring_requested_ = true;
  bool fast_init_scan_stop_in_flight_ = false;
  bool sender_present_ = false;
  bool receive_trigger_armed_ = true;
  bool receive_window_pending_ = false;
  bool receive_offer_received_ = false;
  bool fallback_receive_window_ = false;
  bool fallback_receive_used_ = false;
  bool wifi_direct_ = false;
  bool receive_enabled_ = false;
  bool shutting_down_ = false;
};
