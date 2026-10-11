#include "sharing/linux/lib/quickshare.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/time/time.h"
#include "connections/implementation/flags/nearby_connections_feature_flags.h"
#include "internal/base/file_path.h"
#include "internal/flags/nearby_flags.h"
#include "internal/platform/implementation/platform.h"
#include "sharing/advertisement.h"
#include "sharing/attachment_container.h"
#include "sharing/file_attachment.h"
#include "sharing/flags/generated/nearby_sharing_feature_flags.h"
#include "sharing/linux/lib/native_file_logging.h"
#include "sharing/linux/nearby_noop_analytics_recorder.h"
#include "sharing/linux/platform/linux_sharing_platform.h"
#include "sharing/nearby_sharing_service.h"
#include "sharing/nearby_sharing_service_factory.h"
#include "sharing/nearby_sharing_settings.h"
#include "sharing/proto/enums.pb.h"
#include "sharing/share_target.h"
#include "sharing/share_target_discovered_callback.h"
#include "sharing/transfer_metadata.h"
#include "sharing/transfer_update_callback.h"

extern "C" int nearby_linux_pairing_prompt_count();
extern "C" int nearby_linux_ble_advertising_ok();
extern "C" void nearby_linux_copy_ble_advertising_error(char* buffer,
                                                       int buffer_size);

namespace {

using nearby::sharing::NearbySharingService;
using nearby::sharing::ShareTarget;
using nearby::sharing::TransferMetadata;
using StatusCodes = NearbySharingService::StatusCodes;
using Status = TransferMetadata::Status;

constexpr std::chrono::seconds kShutdownTimeout(10);

static_assert(static_cast<int>(StatusCodes::kOk) == QUICKSHARE_OK);
static_assert(static_cast<int>(StatusCodes::kError) == QUICKSHARE_ERROR);
static_assert(static_cast<int>(StatusCodes::kOutOfOrderApiCall) ==
              QUICKSHARE_OUT_OF_ORDER_API_CALL);
static_assert(static_cast<int>(StatusCodes::kStatusAlreadyStopped) ==
              QUICKSHARE_ALREADY_STOPPED);
static_assert(static_cast<int>(StatusCodes::kTransferAlreadyInProgress) ==
              QUICKSHARE_TRANSFER_ALREADY_IN_PROGRESS);
static_assert(static_cast<int>(StatusCodes::kNoAvailableConnectionMedium) ==
              QUICKSHARE_NO_AVAILABLE_CONNECTION_MEDIUM);
static_assert(static_cast<int>(StatusCodes::kIrrecoverableHardwareError) ==
              QUICKSHARE_IRRECOVERABLE_HARDWARE_ERROR);
static_assert(static_cast<int>(StatusCodes::kInvalidArgument) ==
              QUICKSHARE_INVALID_ARGUMENT);
static_assert(static_cast<int>(Status::kUnknown) == QUICKSHARE_TRANSFER_UNKNOWN);
static_assert(static_cast<int>(Status::kConnecting) ==
              QUICKSHARE_TRANSFER_CONNECTING);
static_assert(static_cast<int>(Status::kAwaitingLocalConfirmation) ==
              QUICKSHARE_TRANSFER_AWAITING_LOCAL_CONFIRMATION);
static_assert(static_cast<int>(Status::kAwaitingRemoteAcceptance) ==
              QUICKSHARE_TRANSFER_AWAITING_REMOTE_ACCEPTANCE);
static_assert(static_cast<int>(Status::kInProgress) ==
              QUICKSHARE_TRANSFER_IN_PROGRESS);
static_assert(static_cast<int>(Status::kComplete) ==
              QUICKSHARE_TRANSFER_COMPLETE);
static_assert(static_cast<int>(Status::kFailed) == QUICKSHARE_TRANSFER_FAILED);
static_assert(static_cast<int>(Status::kRejected) ==
              QUICKSHARE_TRANSFER_REJECTED);
static_assert(static_cast<int>(Status::kCancelled) ==
              QUICKSHARE_TRANSFER_CANCELLED);
static_assert(static_cast<int>(Status::kTimedOut) ==
              QUICKSHARE_TRANSFER_TIMED_OUT);
static_assert(static_cast<int>(Status::kMediaUnavailable) ==
              QUICKSHARE_TRANSFER_MEDIA_UNAVAILABLE);
static_assert(static_cast<int>(Status::kNotEnoughSpace) ==
              QUICKSHARE_TRANSFER_NOT_ENOUGH_SPACE);
static_assert(static_cast<int>(Status::kUnsupportedAttachmentType) ==
              QUICKSHARE_TRANSFER_UNSUPPORTED_ATTACHMENT_TYPE);
static_assert(static_cast<int>(Status::kDeviceAuthenticationFailed) ==
              QUICKSHARE_TRANSFER_DEVICE_AUTHENTICATION_FAILED);
static_assert(static_cast<int>(Status::kIncompletePayloads) ==
              QUICKSHARE_TRANSFER_INCOMPLETE_PAYLOADS);

std::unique_ptr<nearby::sharing::linux::NativeFileLogging> g_logging;

void CopyToBuffer(char* buffer, int buffer_size, const std::string& value) {
  if (buffer == nullptr || buffer_size <= 0) {
    return;
  }
  const int count = std::min(buffer_size - 1, static_cast<int>(value.size()));
  if (count > 0) {
    memcpy(buffer, value.data(), static_cast<size_t>(count));
  }
  buffer[count] = '\0';
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

QuickShareTarget ToTarget(const ShareTarget& target) {
  QuickShareTarget out = {};
  out.id = target.id;
  CopyToBuffer(out.device_name, sizeof(out.device_name), target.device_name);
  out.type = static_cast<int>(target.type);
  return out;
}

}  // namespace

struct QuickShare {
  class TransferCallback final : public nearby::sharing::TransferUpdateCallback {
   public:
    TransferCallback(QuickShare& owner, bool receive_mode)
        : owner_(owner), receive_mode_(receive_mode) {}

    void OnTransferUpdate(
        const ShareTarget& share_target,
        const nearby::sharing::AttachmentContainer& attachment_container,
        const TransferMetadata& metadata) override {
      QuickShareTransfer transfer = {};
      transfer.target = ToTarget(share_target);
      transfer.receive_mode = receive_mode_ ? 1 : 0;
      transfer.status = static_cast<int>(metadata.status());
      CopyToBuffer(transfer.status_name, sizeof(transfer.status_name),
                   TransferMetadata::StatusToString(metadata.status()));
      transfer.is_final_status = metadata.is_final_status() ? 1 : 0;
      transfer.progress_percent = metadata.progress();
      transfer.transferred_bytes = metadata.transferred_bytes();
      transfer.total_bytes = attachment_container.GetTotalAttachmentsSize();
      transfer.attachment_count = attachment_container.GetAttachmentCount();
      transfer.total_attachments_count = metadata.total_attachments_count();
      transfer.transferred_attachments_count =
          metadata.transferred_attachments_count();
      owner_.Dispatch([&](const QuickShareCallbacks& callbacks) {
        if (callbacks.on_transfer_update != nullptr) {
          callbacks.on_transfer_update(&transfer, callbacks.user);
        }
      });
    }

   private:
    QuickShare& owner_;
    bool receive_mode_;
  };

  class DiscoveryCallback final
      : public nearby::sharing::ShareTargetDiscoveredCallback {
   public:
    explicit DiscoveryCallback(QuickShare& owner) : owner_(owner) {}

    void OnShareTargetDiscovered(const ShareTarget& share_target) override {
      Found(share_target);
    }
    void OnShareTargetUpdated(const ShareTarget& share_target) override {
      Found(share_target);
    }
    void OnShareTargetLost(const ShareTarget& share_target) override {
      const QuickShareTarget target = ToTarget(share_target);
      owner_.Dispatch([&](const QuickShareCallbacks& callbacks) {
        if (callbacks.on_target_lost != nullptr) {
          callbacks.on_target_lost(&target, callbacks.user);
        }
      });
    }

   private:
    void Found(const ShareTarget& share_target) {
      const QuickShareTarget target = ToTarget(share_target);
      owner_.Dispatch([&](const QuickShareCallbacks& callbacks) {
        if (callbacks.on_target_found != nullptr) {
          callbacks.on_target_found(&target, callbacks.user);
        }
      });
    }

    QuickShare& owner_;
  };

  QuickShare()
      : platform(
            std::make_unique<nearby::sharing::linux::LinuxSharingPlatform>()),
        send_callback(*this, false),
        receive_callback(*this, true),
        discovery_callback(*this) {}

  // Callbacks are invoked under mu so that Deactivate() waits for in-flight
  // ones and nothing fires afterwards. Receivers must not call back into this
  // library from a callback.
  template <typename F>
  void Dispatch(F&& fn) {
    std::lock_guard<std::mutex> lock(mu);
    if (active) {
      fn(callbacks);
    }
  }

  void Complete(QuickShareStatusFn done, void* user, StatusCodes status) {
    if (done == nullptr) {
      return;
    }
    std::lock_guard<std::mutex> lock(mu);
    if (active) {
      done(static_cast<int>(status), user);
    }
  }

  std::function<void(StatusCodes)> StatusCallback(QuickShareStatusFn done,
                                                 void* user) {
    return [this, done, user](StatusCodes status) {
      Complete(done, user, status);
    };
  }

  void Deactivate() {
    std::lock_guard<std::mutex> lock(mu);
    active = false;
  }

  std::mutex mu;
  bool active = true;
  QuickShareCallbacks callbacks = {};

  nearby::sharing::linux::NoOpAnalyticsRecorder analytics;
  std::unique_ptr<nearby::sharing::linux::LinuxSharingPlatform> platform;
  NearbySharingService* service = nullptr;
  bool wifi_direct = false;
  TransferCallback send_callback;
  TransferCallback receive_callback;
  DiscoveryCallback discovery_callback;
};

extern "C" {

#define QUICKSHARE_EXPORT __attribute__((visibility("default")))

QUICKSHARE_EXPORT void quickshare_logging_init(void) {
  g_logging = nearby::sharing::linux::NativeFileLogging::Initialize();
}

QUICKSHARE_EXPORT void quickshare_logging_shutdown(void) {
  g_logging.reset();
}

QUICKSHARE_EXPORT QuickShare* quickshare_create(void) {
  ConfigureFlags();
  auto qs = std::make_unique<QuickShare>();
  try {
    qs->wifi_direct =
        nearby::api::ImplementationPlatform::CreateWifiDirectMedium() !=
        nullptr;
  } catch (...) {
    qs->wifi_direct = false;
  }
  qs->service = nearby::sharing::NearbySharingServiceFactory::GetInstance()
                    ->CreateSharingService(*qs->platform, &qs->analytics,
                                           /*event_logger=*/nullptr,
                                           /*supports_file_sync=*/false);
  if (qs->service == nullptr) {
    return nullptr;
  }
  return qs.release();
}

QUICKSHARE_EXPORT void quickshare_destroy(QuickShare* qs) {
  if (qs == nullptr) {
    return;
  }
  qs->Deactivate();
  delete qs;
}

QUICKSHARE_EXPORT void quickshare_set_callbacks(
    QuickShare* qs, const QuickShareCallbacks* callbacks) {
  std::lock_guard<std::mutex> lock(qs->mu);
  qs->callbacks = callbacks != nullptr ? *callbacks : QuickShareCallbacks{};
}

QUICKSHARE_EXPORT void quickshare_initialize(QuickShare* qs,
                                             QuickShareStatusFn done,
                                             void* user) {
  qs->service->GetSettings()->SetDataUsage(
      nearby::sharing::proto::WIFI_ONLY_DATA_USAGE);
  qs->service->SetVisibility(
      nearby::sharing::proto::DEVICE_VISIBILITY_EVERYONE, absl::ZeroDuration(),
      [qs, done, user](StatusCodes status) {
        qs->Complete(done, user, status);
      });
}

QUICKSHARE_EXPORT int quickshare_shutdown(QuickShare* qs) {
  struct State {
    std::mutex mutex;
    std::condition_variable cv;
    std::optional<StatusCodes> status;
  };
  auto state = std::make_shared<State>();
  qs->service->Shutdown([state](StatusCodes status) {
    {
      std::lock_guard<std::mutex> lock(state->mutex);
      state->status = status;
    }
    state->cv.notify_one();
  });

  StatusCodes result = StatusCodes::kError;
  {
    std::unique_lock<std::mutex> lock(state->mutex);
    if (state->cv.wait_for(lock, kShutdownTimeout,
                           [&] { return state->status.has_value(); })) {
      result = *state->status;
    }
  }
  qs->Deactivate();
  return static_cast<int>(result);
}

QUICKSHARE_EXPORT void quickshare_register_receive_surface(
    QuickShare* qs, QuickShareStatusFn done, void* user) {
  qs->service->RegisterReceiveSurface(
      &qs->receive_callback, NearbySharingService::ReceiveSurfaceState::kForeground,
      nearby::sharing::Advertisement::BlockedVendorId::kNone,
      [qs, done, user](StatusCodes status) { qs->Complete(done, user, status); });
}

QUICKSHARE_EXPORT void quickshare_unregister_receive_surface(
    QuickShare* qs, QuickShareStatusFn done, void* user) {
  qs->service->UnregisterReceiveSurface(
      &qs->receive_callback,
      [qs, done, user](StatusCodes status) { qs->Complete(done, user, status); });
}

QUICKSHARE_EXPORT void quickshare_register_send_surface(
    QuickShare* qs, QuickShareStatusFn done, void* user) {
  qs->service->RegisterSendSurface(
      &qs->send_callback, &qs->discovery_callback,
      NearbySharingService::SendSurfaceState::kForeground,
      nearby::sharing::Advertisement::BlockedVendorId::kNone,
      /*disable_wifi_hotspot=*/false,
      [qs, done, user](StatusCodes status) { qs->Complete(done, user, status); });
}

QUICKSHARE_EXPORT void quickshare_unregister_send_surface(
    QuickShare* qs, QuickShareStatusFn done, void* user) {
  qs->service->UnregisterSendSurface(
      &qs->send_callback,
      [qs, done, user](StatusCodes status) { qs->Complete(done, user, status); });
}

QUICKSHARE_EXPORT void quickshare_send_files(QuickShare* qs, int64_t target_id,
                                             const char* const* paths,
                                             int count, QuickShareStatusFn done,
                                             void* user) {
  nearby::sharing::AttachmentContainer::Builder builder;
  for (int i = 0; i < count; ++i) {
    builder.AddFileAttachment(
        nearby::sharing::FileAttachment(nearby::FilePath(paths[i])));
  }
  qs->service->SendAttachments(target_id, builder.Build(),
                               qs->StatusCallback(done, user));
}

QUICKSHARE_EXPORT void quickshare_accept(QuickShare* qs, int64_t target_id,
                                         QuickShareStatusFn done, void* user) {
  qs->service->Accept(target_id, qs->StatusCallback(done, user));
}

QUICKSHARE_EXPORT void quickshare_reject(QuickShare* qs, int64_t target_id,
                                         QuickShareStatusFn done, void* user) {
  qs->service->Reject(target_id, qs->StatusCallback(done, user));
}

QUICKSHARE_EXPORT void quickshare_cancel(QuickShare* qs, int64_t target_id,
                                         QuickShareStatusFn done, void* user) {
  qs->service->Cancel(target_id, qs->StatusCallback(done, user));
}

QUICKSHARE_EXPORT int quickshare_fast_init_is_scanning(QuickShare* qs) {
  return qs->platform->GetFastInitiationManager().IsScanning() ? 1 : 0;
}

QUICKSHARE_EXPORT void quickshare_fast_init_start_scanning(
    QuickShare* qs, const QuickShareFastInitCallbacks* callbacks) {
  const QuickShareFastInitCallbacks cb = *callbacks;
  qs->platform->GetFastInitiationManager().StartScanning(
      [qs, cb]() {
        qs->Dispatch([&](const QuickShareCallbacks&) {
          if (cb.on_devices_discovered != nullptr) {
            cb.on_devices_discovered(cb.user);
          }
        });
      },
      [qs, cb]() {
        qs->Dispatch([&](const QuickShareCallbacks&) {
          if (cb.on_devices_not_discovered != nullptr) {
            cb.on_devices_not_discovered(cb.user);
          }
        });
      },
      [qs, cb](nearby::api::FastInitiationManager::Error error) {
        qs->Dispatch([&](const QuickShareCallbacks&) {
          if (cb.on_error != nullptr) {
            cb.on_error(static_cast<int>(error), cb.user);
          }
        });
      });
}

QUICKSHARE_EXPORT void quickshare_fast_init_stop_scanning(QuickShare* qs,
                                                          QuickShareDoneFn done,
                                                          void* user) {
  std::function<void()> callback;
  if (done != nullptr) {
    callback = [qs, done, user]() {
      qs->Dispatch([&](const QuickShareCallbacks&) { done(user); });
    };
  }
  qs->platform->GetFastInitiationManager().StopScanning(std::move(callback));
}

QUICKSHARE_EXPORT int quickshare_is_bluetooth_powered(QuickShare* qs) {
  return qs->service->IsBluetoothPowered() ? 1 : 0;
}

QUICKSHARE_EXPORT int quickshare_is_lan_connected(QuickShare* qs) {
  return qs->service->IsLanConnected() ? 1 : 0;
}

QUICKSHARE_EXPORT int quickshare_wifi_direct_available(QuickShare* qs) {
  return qs->wifi_direct ? 1 : 0;
}

QUICKSHARE_EXPORT int quickshare_pairing_prompt_count(QuickShare*) {
  return nearby_linux_pairing_prompt_count();
}

QUICKSHARE_EXPORT int quickshare_ble_advertising_ok(QuickShare*) {
  return nearby_linux_ble_advertising_ok();
}

QUICKSHARE_EXPORT void quickshare_copy_ble_advertising_error(QuickShare*,
                                                             char* buffer,
                                                             int buffer_size) {
  nearby_linux_copy_ble_advertising_error(buffer, buffer_size);
}

QUICKSHARE_EXPORT void quickshare_status_string(int status, char* buffer,
                                                int buffer_size) {
  CopyToBuffer(buffer, buffer_size,
               NearbySharingService::StatusCodeToString(
                   static_cast<StatusCodes>(status)));
}

}  // extern "C"
