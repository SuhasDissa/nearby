// Copyright 2023 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <algorithm>
#include <array>
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <optional>
#include <string>
#include <thread>
#include <map>
#include <memory>
#include <set>
#include <utility>

#include <bluetooth/bluetooth.h>
#include <bluetooth/l2cap.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <sdbus-c++/IProxy.h>
#include <sdbus-c++/Types.h>

#include "absl/container/flat_hash_set.h"
#include "absl/synchronization/mutex.h"
#include "internal/platform/implementation/ble.h"
#include "internal/platform/implementation/linux/ble_v2_medium.h"

#include "connections/implementation/flags/nearby_connections_feature_flags.h"
#include "absl/types/span.h"
#include "absl/time/time.h"
#include "ble_l2cap_server_socket.h"
#include "ble_l2cap_socket.h"
#include "ble_gatt_server.h"
#include "internal/platform/implementation/linux/bluetooth_classic_device.h"
#include "internal/platform/implementation/linux/bluetooth_devices.h"
#include "internal/platform/implementation/linux/bluez.h"
#include "internal/platform/implementation/linux/bluez_advertisement_monitor.h"
#include "internal/platform/implementation/linux/bluez_advertisement_monitor_manager.h"
#include "internal/platform/implementation/linux/bluez_le_advertisement.h"
#include "internal/platform/implementation/linux/dbus.h"
#include "internal/platform/implementation/linux/utils.h"
#include <sdbus-c++/Message.h>
#include <sdbus-c++/ProxyInterfaces.h>
#include <sdbus-c++/StandardInterfaces.h>
#include "internal/platform/implementation/linux/generated/dbus/bluez/advertisement_monitor_server.h"
#include "internal/platform/implementation/linux/generated/dbus/bluez/le_advertisement_manager_client.h"
#include "internal/platform/mac_address.h"
#include "internal/platform/cancellation_flag.h"
#include "internal/platform/prng.h"
#include "internal/flags/nearby_flags.h"

namespace nearby {
namespace linux {

namespace {
using nearby::connections::config_package_nearby::nearby_connections_feature::
    kRefactorBleL2cap;


constexpr absl::Duration kL2capConnectTimeout = absl::Seconds(10);
constexpr absl::Duration kL2capPollSlice = absl::Milliseconds(200);
constexpr absl::Duration kFreshAddressMaxAge = absl::Seconds(10);
constexpr size_t kEndpointIdLength = 4;

// Extracts the 4-byte endpoint id from FEF3 service data. Handles the
// legacy fast advertisement and the regular (extended) advertisement.
std::optional<std::string> ParseEndpointId(const std::vector<uint8_t>& raw) {
  if (raw.empty()) {
    return std::nullopt;
  }
  const bool fast = (raw[0] & 0x02) != 0;
  size_t offset = 1;
  size_t data_size;
  if (fast) {
    if (raw.size() < offset + 1) {
      return std::nullopt;
    }
    data_size = raw[offset++];
  } else {
    if (raw.size() < offset + 3 + 4) {
      return std::nullopt;
    }
    offset += 3;
    data_size = (static_cast<size_t>(raw[offset]) << 24) |
                (static_cast<size_t>(raw[offset + 1]) << 16) |
                (static_cast<size_t>(raw[offset + 2]) << 8) | raw[offset + 3];
    offset += 4;
  }
  if (data_size > raw.size() - offset) {
    return std::nullopt;
  }
  // Inner data: version/pcp byte, [service id hash (3) if not fast], endpoint id.
  size_t inner = 1 + (fast ? 0 : 3);
  if (data_size < inner + kEndpointIdLength || (raw[offset] >> 5) != 1) {
    return std::nullopt;
  }
  std::string id(raw.begin() + offset + inner,
                 raw.begin() + offset + inner + kEndpointIdLength);
  for (char c : id) {
    if (c < 0x20 || c > 0x7e) {
      return std::nullopt;
    }
  }
  return id;
}

int ConnectL2cap(const BluetoothDevice& device, int psm,
                 CancellationFlag* cancellation_flag) {
  int fd = socket(AF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_L2CAP);
  if (fd < 0) {
    LOG(ERROR) << __func__
               << ": Failed to create L2CAP socket: " << std::strerror(errno);
    return -1;
  }

  struct sockaddr_l2 local;
  std::memset(&local, 0, sizeof(local));
  local.l2_family = AF_BLUETOOTH;
  local.l2_bdaddr_type = BDADDR_LE_PUBLIC;
  if (bind(fd, (struct sockaddr*)&local, sizeof(local)) < 0) {
    LOG(WARNING) << __func__
                 << ": Failed to bind L2CAP socket: " << std::strerror(errno);
  }

  uint16_t imtu = 672;
  if (setsockopt(fd, SOL_BLUETOOTH, BT_RCVMTU, &imtu, sizeof(imtu)) < 0) {
    LOG(WARNING) << __func__
                 << ": Failed to set BT_RCVMTU: " << std::strerror(errno);
  }

  struct sockaddr_l2 addr;
  std::memset(&addr, 0, sizeof(addr));
  addr.l2_family = AF_BLUETOOTH;
  addr.l2_psm = htobs(psm);
  addr.l2_bdaddr_type =
      device.GetAddressType() == "random" ? BDADDR_LE_RANDOM : BDADDR_LE_PUBLIC;
  std::string mac_addr = device.GetMacAddress().ToString();
  if (str2ba(mac_addr.c_str(), &addr.l2_bdaddr) < 0) {
    LOG(ERROR) << __func__ << ": Invalid Bluetooth address: " << mac_addr;
    close(fd);
    return -1;
  }

  int flags = fcntl(fd, F_GETFL, 0);
  fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0 &&
      errno != EINPROGRESS) {
    LOG(ERROR) << __func__
               << ": Failed to connect to L2CAP socket: " << std::strerror(errno);
    close(fd);
    return -1;
  }

  const auto deadline =
      std::chrono::steady_clock::now() +
      std::chrono::milliseconds(absl::ToInt64Milliseconds(kL2capConnectTimeout));
  while (true) {
    if (cancellation_flag != nullptr && cancellation_flag->Cancelled()) {
      LOG(INFO) << __func__ << ": L2CAP connect cancelled";
      close(fd);
      return -1;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      LOG(ERROR) << __func__ << ": L2CAP connect to " << mac_addr
                 << " timed out";
      close(fd);
      return -1;
    }
    struct pollfd pfd = {fd, POLLOUT, 0};
    int ret = poll(&pfd, 1, absl::ToInt64Milliseconds(kL2capPollSlice));
    if (ret < 0 && errno != EINTR) {
      LOG(ERROR) << __func__ << ": poll failed: " << std::strerror(errno);
      close(fd);
      return -1;
    }
    if (ret > 0) {
      break;
    }
  }

  int so_error = 0;
  socklen_t len = sizeof(so_error);
  if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &len) < 0 ||
      so_error != 0) {
    LOG(ERROR) << __func__ << ": Failed to connect to L2CAP socket: "
               << std::strerror(so_error);
    close(fd);
    return -1;
  }

  fcntl(fd, F_SETFL, flags);
  return fd;
}

absl::Mutex g_ble_adv_mutex;
bool g_ble_adv_ok = true;
std::string g_ble_adv_error;
}  // namespace

// BlueZ only exposes AdvertisementMonitorManager when bluetoothd is started
// with experimental features. Without it, watch Device1.ServiceData from a
// normal discovery session. That is enough for Quick Share's FEF3 and FE2C
// advertisements.
struct BleV2Medium::ServiceDataDiscovery
    : public sdbus::ProxyInterfaces<sdbus::ObjectManager_proxy>,
      public std::enable_shared_from_this<BleV2Medium::ServiceDataDiscovery> {
  struct Session {
    api::ble::BleMedium::ScanningCallback callback;
  };

  ServiceDataDiscovery(sdbus::IConnection& bus, BluetoothAdapter& adapter,
                       BluetoothDevices& devices)
      : ProxyInterfaces(bus, sdbus::ServiceName("org.bluez"),
                        sdbus::ObjectPath("/")),
        adapter_path_(adapter.GetObjectPath()),
        devices_(devices) {
    registerProxy();
    match_ = bus.addMatch(
        "type='signal',sender='org.bluez',"
        "interface='org.freedesktop.DBus.Properties',"
        "member='PropertiesChanged',arg0='org.bluez.Device1'",
        [this](sdbus::Message message) {
          OnPropertiesChanged(std::move(message));
        },
        sdbus::return_slot);
  }

  ~ServiceDataDiscovery() {
    match_.reset();
    unregisterProxy();
  }

  void Watch(const Uuid& service_uuid,
             api::ble::BleMedium::ScanningCallback callback) {
    std::vector<sdbus::ObjectPath> existing;
    {
      absl::MutexLock lock(&mutex_);
      sessions_[service_uuid] = std::make_shared<Session>();
      sessions_[service_uuid]->callback = std::move(callback);
    }
    try {
      for (const auto& [path, interfaces] : GetManagedObjects()) {
        if (interfaces.count(sdbus::InterfaceName("org.bluez.Device1")) == 1) {
          existing.push_back(path);
        }
      }
    } catch (const sdbus::Error& error) {
      LOG(WARNING) << "ServiceDataDiscovery: GetManagedObjects failed: "
                   << error.getMessage();
    }
    // Watch runs under BleMedium's lock and the found callback takes it again,
    // so report devices BlueZ already knows about from another thread.
    std::thread([weak = weak_from_this(), existing = std::move(existing)] {
      auto self = weak.lock();
      if (self == nullptr) {
        return;
      }
      for (const auto& path : existing) {
        self->Refresh(path);
      }
    }).detach();
    std::shared_ptr<Session> session;
    {
      absl::MutexLock lock(&mutex_);
      auto found = sessions_.find(service_uuid);
      if (found != sessions_.end()) {
        session = found->second;
      }
    }
    if (session != nullptr && session->callback.start_scanning_result) {
      session->callback.start_scanning_result(absl::OkStatus());
    }
  }

  void Unwatch(const Uuid& service_uuid) {
    std::shared_ptr<Session> session;
    std::vector<api::ble::BlePeripheral::UniqueId> lost_ids;
    {
      absl::MutexLock lock(&mutex_);
      auto found = sessions_.find(service_uuid);
      if (found == sessions_.end()) {
        return;
      }
      session = found->second;
      sessions_.erase(found);
      std::vector<api::ble::BlePeripheral::UniqueId> drop;
      for (auto& [id, uuids] : seen_) {
        if (uuids.erase(service_uuid) > 0) {
          lost_ids.push_back(id);
          if (uuids.empty()) {
            drop.push_back(id);
          }
        }
      }
      for (auto id : drop) {
        seen_.erase(id);
      }
    }
    if (session != nullptr && session->callback.advertisement_lost_cb) {
      for (auto id : lost_ids) {
        session->callback.advertisement_lost_cb(id);
      }
    }
  }

  std::optional<api::ble::BlePeripheral::UniqueId> FreshestPeripheral(
      api::ble::BlePeripheral::UniqueId id) const {
    absl::MutexLock lock(&mutex_);
    auto endpoint = device_endpoint_.find(id);
    if (endpoint == device_endpoint_.end()) {
      return std::nullopt;
    }
    auto latest = endpoint_latest_.find(endpoint->second);
    if (latest == endpoint_latest_.end() || latest->second.id == id ||
        absl::Now() - latest->second.seen > kFreshAddressMaxAge) {
      return std::nullopt;
    }
    return latest->second.id;
  }

  bool empty() const {
    absl::MutexLock lock(&mutex_);
    return sessions_.empty();
  }

  void onInterfacesAdded(
      const sdbus::ObjectPath& object_path,
      const std::map<sdbus::InterfaceName,
                     std::map<sdbus::PropertyName, sdbus::Variant>>&
          interfaces) override {
    if (interfaces.count(sdbus::InterfaceName("org.bluez.Device1")) == 1) {
      Refresh(object_path);
    }
  }

  void onInterfacesRemoved(
      const sdbus::ObjectPath& object_path,
      const std::vector<sdbus::InterfaceName>& interfaces) override {
    for (const auto& interface : interfaces) {
      if (std::string(interface) == "org.bluez.Device1") {
        Lose(object_path);
        return;
      }
    }
  }

  void OnPropertiesChanged(sdbus::Message message) {
    std::string interface;
    message >> interface;
    if (interface == "org.bluez.Device1") {
      Refresh(sdbus::ObjectPath(std::string(message.getPath())));
    }
  }

  bool OnAdapter(const sdbus::ObjectPath& path) const {
    const std::string path_text(path);
    const std::string adapter_text(adapter_path_);
    return path_text.rfind(adapter_text, 0) == 0;
  }

  void Refresh(const sdbus::ObjectPath& path) {
    if (!OnAdapter(path)) {
      return;
    }
    auto device = devices_.add_new_device(path);
    if (device == nullptr || !device->GetMacAddress().IsSet()) {
      return;
    }
    auto service_data = device->ServiceData();
    if (service_data.has_value()) {
      for (const auto& [uuid_text, value] : *service_data) {
        if (uuid_text.find("fef3") == std::string::npos &&
            uuid_text.find("FEF3") == std::string::npos) {
          continue;
        }
        try {
          auto raw = value.get<std::vector<uint8_t>>();
          std::string hex;
          char buf[3];
          for (uint8_t byte : raw) {
            snprintf(buf, sizeof(buf), "%02x", byte);
            hex += buf;
          }
          LOG(INFO) << "FEF3 raw " << std::string(path) << " (" << raw.size()
                    << " bytes): " << hex;
          if (auto endpoint_id = ParseEndpointId(raw)) {
            absl::MutexLock lock(&mutex_);
            device_endpoint_[device->GetMacAddress().address()] = *endpoint_id;
            endpoint_latest_[*endpoint_id] = {
                device->GetMacAddress().address(), absl::Now()};
          }
        } catch (const sdbus::Error&) {
        }
      }
    }
    api::ble::BleAdvertisementData advertisement;
    absl::flat_hash_set<Uuid> present;
    if (service_data.has_value()) {
      for (const auto& [uuid_text, value] : *service_data) {
        auto uuid = UuidFromString(uuid_text);
        if (!uuid.has_value()) {
          continue;
        }
        std::vector<uint8_t> bytes;
        try {
          bytes = value.get<std::vector<uint8_t>>();
        } catch (const sdbus::Error&) {
          continue;
        }
        advertisement.service_data.emplace(
            *uuid, std::string(bytes.begin(), bytes.end()));
        present.insert(*uuid);
      }
    }

    const auto id = device->GetMacAddress().address();
    struct Event {
      bool found = false;
      std::shared_ptr<Session> session;
    };
    std::vector<Event> events;
    {
      absl::MutexLock lock(&mutex_);
      auto& seen_for_device = seen_[id];
      for (const auto& [uuid, session] : sessions_) {
        const bool now = present.contains(uuid);
        const bool before = seen_for_device.contains(uuid);
        // Re-report found on every update: the upstream peripheral tracker
        // expires advertisements that are not refreshed.
        if (!now && !before) {
          continue;
        }
        if (now) {
          seen_for_device.insert(uuid);
        } else {
          seen_for_device.erase(uuid);
        }
        events.push_back(Event{now, session});
      }
      if (seen_for_device.empty()) {
        seen_.erase(id);
      }
    }
    for (const auto& event : events) {
      if (event.found && event.session->callback.advertisement_found_cb) {
        event.session->callback.advertisement_found_cb(id, advertisement);
      } else if (!event.found &&
                 event.session->callback.advertisement_lost_cb) {
        event.session->callback.advertisement_lost_cb(id);
      }
    }
  }

  void Lose(const sdbus::ObjectPath& path) {
    if (!OnAdapter(path)) {
      return;
    }
    auto device = devices_.get_device_by_path(path);
    if (device == nullptr || !device->GetMacAddress().IsSet()) {
      return;
    }
    const auto id = device->GetMacAddress().address();
    std::vector<std::shared_ptr<Session>> sessions;
    {
      absl::MutexLock lock(&mutex_);
      auto seen = seen_.find(id);
      if (seen == seen_.end()) {
        return;
      }
      for (const auto& uuid : seen->second) {
        auto session = sessions_.find(uuid);
        if (session != sessions_.end()) {
          sessions.push_back(session->second);
        }
      }
      seen_.erase(seen);
    }
    for (const auto& session : sessions) {
      if (session->callback.advertisement_lost_cb) {
        session->callback.advertisement_lost_cb(id);
      }
    }
  }

  sdbus::ObjectPath adapter_path_;
  BluetoothDevices& devices_;
  struct LatestSighting {
    api::ble::BlePeripheral::UniqueId id;
    absl::Time seen;
  };

  sdbus::Slot match_;
  mutable absl::Mutex mutex_;
  absl::flat_hash_map<api::ble::BlePeripheral::UniqueId, std::string>
      device_endpoint_ ABSL_GUARDED_BY(mutex_);
  absl::flat_hash_map<std::string, LatestSighting> endpoint_latest_
      ABSL_GUARDED_BY(mutex_);
  absl::flat_hash_map<Uuid, std::shared_ptr<Session>> sessions_
      ABSL_GUARDED_BY(mutex_);
  absl::flat_hash_map<api::ble::BlePeripheral::UniqueId,
                      absl::flat_hash_set<Uuid>>
      seen_ ABSL_GUARDED_BY(mutex_);
};

BleV2Medium::~BleV2Medium() {

  LOG(INFO) << __func__ << ": BleV2Medium cleanup ";
  try {
      StopAdvertising();
    } catch (const std::exception& error) {
      LOG(ERROR) << __func__
                 << ": Failed to stop advertising: " <<
                 error.what();
    }
}

BleV2Medium::BleV2Medium(BluetoothAdapter& adapter)
    : system_bus_(adapter.GetConnection()),
      adapter_(adapter),
      devices_(std::make_unique<BluetoothDevices>(
          system_bus_, adapter_.GetObjectPath())),
      gatt_discovery_(std::make_shared<BluezGattDiscovery>(system_bus_)),
      root_object_manager_(std::make_unique<RootObjectManager>(
          *system_bus_,
          sdbus::ObjectPath(
              "/com/google/nearby/medium/ble/advertisement/monitor"))),
      adv_monitor_manager_(
          bluez::AdvertisementMonitorManager::
              DiscoverAdvertisementMonitorManager(*system_bus_, adapter_)),
      adv_manager_(std::make_unique<bluez::LEAdvertisementManager>(*system_bus_,
                                                                   adapter)),
      cur_adv_(nullptr) {
  if (!gatt_discovery_->InitializeKnownServices()) {
    LOG(WARNING) << __func__
                 << ": Failed to initialize known GATT services cache.";
  }

  if (adv_monitor_manager_) {
    LOG(INFO) << __func__
              << ": Registering path "
                 "/com/google/nearby/medium/ble/advertisement/monitor with "
                 "AdvertisementMonitorManager at "
              << adv_monitor_manager_->getProxy().getObjectPath();
    adv_monitor_manager_->SetRegisterMonitorReplyCallback(
        [this](std::optional<sdbus::Error> error) {
          OnRegisterMonitorReply(std::move(error));
        });
    adv_monitor_manager_->RegisterMonitor(
        root_object_manager_->getObject().getObjectPath());
  } else {
    adv_monitor_manager_ready_notification_.Notify();
  }
}

void BleV2Medium::OnRegisterMonitorReply(std::optional<sdbus::Error> error) {
  {
    absl::MutexLock lock(&adv_monitor_manager_ready_mutex_);
    adv_monitor_manager_ready_ = !error.has_value() || !error->isValid();
    if (error.has_value() && error->isValid()) {
      adv_monitor_manager_error_name_ = error->getName();
      adv_monitor_manager_error_message_ = error->getMessage();
    }
  }

  if (error.has_value() && error->isValid()) {
    LOG(ERROR) << __func__ << ": Got error '" << error->getName()
               << "' with message '" << error->getMessage()
               << "' while calling RegisterMonitor on object "
               << adv_monitor_manager_->getProxy().getObjectPath();
  }

  adv_monitor_manager_ready_notification_.Notify();
}

bool BleV2Medium::WaitForAdvertisementMonitorManager() {
  if (adv_monitor_manager_ == nullptr) {
    LOG(WARNING) << __func__ << ": Advertising monitor not supported by BlueZ";
    return false;
  }

  adv_monitor_manager_ready_notification_.WaitForNotification();

  absl::MutexLock lock(&adv_monitor_manager_ready_mutex_);
  if (adv_monitor_manager_ready_) {
    return true;
  }

  LOG(WARNING)
      << __func__
      << ": AdvertisementMonitorManager registration failed with name '"
      << adv_monitor_manager_error_name_ << "' and message '"
      << adv_monitor_manager_error_message_ << "'";
  return false;
}

// sync api
// called twice. Once with extended regular advertisement ( when
// IsExtendedAdvertisementsAvailable() == true ) and another for GATT-backed
// header advertisement for legacy devices
bool BleV2Medium::StartAdvertising(
    const api::ble::BleAdvertisementData& advertising_data,
    api::ble::AdvertiseParameters advertise_set_parameters) {
  // if (!advertising_data.is_extended_advertisement)
  //{
  //   // can't send two LE advertisements at the same
  //   return true;
  // }
  if (!adapter_.IsEnabled()) {
    LOG(WARNING) << "BLE cannot start advertising because the "
                    "bluetooth adapter is not enabled.";
    return false;
  }

  if (advertising_data.service_data.empty()) {
    LOG(WARNING)
        << "BLE cannot start to advertise due to invalid service data.";
    return false;
  }

  absl::MutexLock l(&advs_mutex_);

  const bool is_extended = advertising_data.is_extended_advertisement;
  if (!is_extended && extended_adv_registered_) {
    LOG(INFO) << __func__
              << ": Extended advertisement already registered; skipping "
                 "non-extended advertisement";
    return true;
  }

  if (!adv_capabilities_read_) {
    adv_capabilities_read_ = true;
    try {
      auto caps = adv_manager_->SupportedCapabilities();
      if (auto c = caps.find("MinTxPower"); c != caps.end()) {
        adv_min_tx_power_ = c->second.get<int16_t>();
      }
      if (auto c = caps.find("MaxTxPower"); c != caps.end()) {
        adv_max_tx_power_ = c->second.get<int16_t>();
      }
    } catch (const sdbus::Error& e) {
      DBUS_LOG_PROPERTY_GET_ERROR(adv_manager_, "SupportedCapabilities", e);
    }
  }

  int16_t tx_power = bluez::TxPowerLevelDbm(advertise_set_parameters.tx_power_level);
  if (adv_min_tx_power_.has_value()) {
    tx_power = std::max(tx_power, *adv_min_tx_power_);
  }
  if (adv_max_tx_power_.has_value()) {
    tx_power = std::min(tx_power, *adv_max_tx_power_);
  }

  const char* const types[] = {"peripheral", "broadcast"};
  for (const char* type : types) {
    advs_.push_front(bluez::LEAdvertisement::CreateLEAdvertisement(
        *system_bus_, advertising_data, advertise_set_parameters, type,
        tx_power));
    auto it = advs_.begin();

    LOG(INFO) << __func__ << ": Registering " << type
              << " advertisement, is_extended: " << is_extended << " "
              << (*it)->getObject().getObjectPath() << " tx_power: "
              << tx_power << " on bluetooth adapter "
              << adapter_.GetObjectPath();

    try {
      adv_manager_->RegisterAdvertisementSync(
          (*it)->getObject().getObjectPath(), {});
    } catch (const sdbus::Error& e) {
      advs_.erase(it);
      LOG(WARNING) << __func__ << ": " << type
                   << " advertisement registration failed: " << e.getName()
                   << ": " << e.getMessage();
      DBUS_LOG_METHOD_CALL_ERROR(adv_manager_, "RegisterAdvertisementSync", e);
      absl::MutexLock error_lock(&g_ble_adv_mutex);
      g_ble_adv_ok = false;
      g_ble_adv_error = e.getName() + ": " + e.getMessage();
      continue;
    }

    LOG(INFO) << __func__ << ": Advertisement registered as " << type;
    if (is_extended) {
      extended_adv_registered_ = true;
    }
    absl::MutexLock error_lock(&g_ble_adv_mutex);
    g_ble_adv_ok = true;
    g_ble_adv_error.clear();
    return true;
  }

  return false;
}

// async api
//  runs with nearby presence
std::unique_ptr<api::ble::BleMedium::AdvertisingSession>
BleV2Medium::StartAdvertising(
    const api::ble::BleAdvertisementData& advertising_data,
    api::ble::AdvertiseParameters advertise_set_parameters,
    AdvertisingCallback callback) {
  if (!adapter_.IsEnabled()) {
    LOG(WARNING) << ": BLE cannot start advertising because the "
                    "bluetooth adapter is not enabled.";
    return nullptr;
  }

  if (advertising_data.service_data.empty()) {
    LOG(WARNING)
        << ": BLE cannot start to advertise due to invalid service data.";
    return nullptr;
  }

  std::shared_ptr<AdvertisingCallback> shared_cb =
      std::make_shared<AdvertisingCallback>(std::move(callback));

  absl::MutexLock lock(&advs_mutex_);
  advs_.push_front(bluez::LEAdvertisement::CreateLEAdvertisement(
      *system_bus_, advertising_data, advertise_set_parameters));
  auto adv_it = advs_.begin();

  // Keep async API surface, but register using the same typed DBus path as the
  // working sync implementation to avoid signature mismatch (oa{sv} vs sa{sv}).
  try {
    adv_manager_->RegisterAdvertisementSync(
        (*adv_it)->getObject().getObjectPath(), {});
    shared_cb->start_advertising_result(absl::OkStatus());
  } catch (const sdbus::Error& e) {
    advs_.erase(adv_it);
    DBUS_LOG_METHOD_CALL_ERROR(adv_manager_, "RegisterAdvertisementSync", e);
    auto name = e.getName();
    std::string msg = e.getMessage();
    absl::Status status;
    if (name == "org.bluez.Error.InvalidArguments" ||
        name == "org.bluez.Error.InvalidLength") {
      status = absl::InvalidArgumentError(msg);
    } else if (name == "org.bluez.Error.AlreadyExists") {
      status = absl::AlreadyExistsError(msg);
    } else if (name == "org.bluez.Error.NotPermitted") {
      status = absl::ResourceExhaustedError(msg);
    } else {
      status = absl::UnknownError(msg);
    }
    shared_cb->start_advertising_result(std::move(status));
    return nullptr;
  }

  absl::AnyInvocable<absl::Status()> stop_adv = [&, adv_it]() {
    LOG(INFO) << __func__ << ": Unregistering advertisement object "
              << (*adv_it)->getObject().getObjectPath();
    absl::MutexLock lock(&advs_mutex_);
    try {
      adv_manager_->UnregisterAdvertisementSync(
          (*adv_it)->getObject().getObjectPath());
    } catch (const sdbus::Error& e) {
      DBUS_LOG_METHOD_CALL_ERROR(adv_manager_, "UnregisterAdvertisementSync",
                                 e);
      return absl::UnknownError(e.getMessage());
    }
    advs_.erase(adv_it);
    return absl::OkStatus();
  };
  return std::make_unique<api::ble::BleMedium::AdvertisingSession>(
      api::ble::BleMedium::AdvertisingSession{std::move(stop_adv)});
}

bool BleV2Medium::StopAdvertising() {
  LOG(INFO) << __func__ << ": Stop advertising called";
  absl::MutexLock l(&advs_mutex_);
  bool ok = true;
  for (auto& adv : advs_) {
    try {
      adv_manager_->UnregisterAdvertisementSync(
          adv->getObject().getObjectPath());
    } catch (const sdbus::Error& e) {
      DBUS_LOG_METHOD_CALL_ERROR(adv_manager_, "UnregisterAdvertisementSync",
                                 e);
      ok = false;
    }
  }

  advs_.clear();
  extended_adv_registered_ = false;
  return ok;
}

bool BleV2Medium::StartFallbackScan(
    const Uuid& service_uuid, api::ble::BleMedium::ScanningCallback callback) {
  LOG(INFO) << __func__
            << ": Scanning " << std::string{service_uuid}
            << " from Device1.ServiceData because BlueZ has no "
               "AdvertisementMonitorManager";
  if (service_data_discovery_ == nullptr) {
    service_data_discovery_ = std::make_shared<ServiceDataDiscovery>(
        *system_bus_, adapter_, *devices_);
  }
  if (!StartLEDiscovery()) {
    return false;
  }
  service_data_discovery_->Watch(service_uuid, std::move(callback));
  return true;
}

void BleV2Medium::StopFallbackScan(const Uuid& service_uuid) {
  if (service_data_discovery_ == nullptr) {
    return;
  }
  service_data_discovery_->Unwatch(service_uuid);
  if (!service_data_discovery_->empty() || !active_adv_monitors_.empty()) {
    return;
  }
  try {
    adapter_.GetBluezAdapterObject().StopDiscovery();
  } catch (const sdbus::Error& error) {
    DBUS_LOG_METHOD_CALL_ERROR(&adapter_.GetBluezAdapterObject(),
                               "StopDiscovery", error);
  }
}

bool BleV2Medium::StartScanning(const Uuid& service_uuid,
                                api::ble::TxPowerLevel tx_power_level,
                                ScanCallback callback) {
  if (cur_monitored_service_uuid_.has_value()) {
    LOG(ERROR) << __func__ << ": A sync scanning session is already active for "
               << std::string{*cur_monitored_service_uuid_};
    return false;
  }

  if (!WaitForAdvertisementMonitorManager() || !MonitorManagerSupportsOr()) {
    api::ble::BleMedium::ScanningCallback scanning;
    scanning.advertisement_found_cb =
        std::move(callback.advertisement_found_cb);
    return StartFallbackScan(service_uuid, std::move(scanning));
  }

  absl::MutexLock lock(&active_adv_monitors_mutex_);
  if (active_adv_monitors_.count(service_uuid) == 1) {
    LOG(ERROR) << __func__ << ": an advertising session for service "
               << std::string{service_uuid} << " already exists";
    return false;
  }

  auto monitor = std::make_unique<bluez::AdvertisementMonitor>(
      *system_bus_, service_uuid, tx_power_level, "or_patterns", devices_,
      std::move(callback));
  try {
    // why is this emitted?
    monitor->emitInterfacesAddedSignal({sdbus::InterfaceName(
        org::bluez::AdvertisementMonitor1_adaptor::INTERFACE_NAME)});

    // adv_monitor_manager_ -> RegisterMonitor(monitor ->
    // getObject().getObjectPath());
    LOG(INFO) << __func__ << ": Registered advertisement monitor with path "
              << monitor->getObject().getObjectPath();
  } catch (const sdbus::Error& e) {
    LOG(ERROR) << __func__
               << ": error emitting InterfacesAdded signal for object path "
               << monitor->getObject().getObjectPath() << " with name '"
               << e.getName() << "' and message '" << e.getMessage() << "'";
    return false;
  }
  auto device_watcher = std::make_unique<DeviceWatcher>(
      *system_bus_, adapter_.GetObjectPath(), adapter_, devices_);
  if (!StartLEDiscovery()) {
    LOG(ERROR) << __func__ << ": Could not start LE discovery on adapter "
               << adapter_.GetObjectPath();
    device_watcher = nullptr;
    try {
      monitor->emitInterfacesRemovedSignal({sdbus::InterfaceName(
          org::bluez::AdvertisementMonitor1_adaptor::INTERFACE_NAME)});
    } catch (const sdbus::Error& e) {
      LOG(ERROR) << __func__
                 << ": error emitting InterfacesRemoved signal for object path "
                 << monitor->getObject().getObjectPath() << " with name '"
                 << e.getName() << "' and message '" << e.getMessage() << "'";
    }
    return false;
  }
  LOG(INFO) << __func__ << " :Started monitoring for service UUID: "
            << std::string(service_uuid);

  active_adv_monitors_[service_uuid] =
      std::make_pair(std::move(monitor), std::move(device_watcher));
  cur_monitored_service_uuid_ = service_uuid;
  return true;
}

bool BleV2Medium::StopScanning() {
  if (!cur_monitored_service_uuid_.has_value()) {
    LOG(ERROR) << __func__ << ": No sync scanning session is currently active.";
    return false;
  }

  if (!WaitForAdvertisementMonitorManager()) {
    // TODO: Implement manual monitoring.
    return false;
  }

  auto& adapter = adapter_.GetBluezAdapterObject();
  LOG(INFO) << __func__ << ": Stopping discovery for adapter "
            << adapter.getProxy().getObjectPath();
  try {
    adapter.StopDiscovery();  // this will stop bluetooth classic discovery as
                              // well. do we want this?
  } catch (const sdbus::Error& e) {
    DBUS_LOG_METHOD_CALL_ERROR(&adapter, "StopDiscovery", e);
  }

  absl::MutexLock lock(&active_adv_monitors_mutex_);
  auto monitor_it = active_adv_monitors_.find(*cur_monitored_service_uuid_);
  assert(monitor_it != active_adv_monitors_.end());
  {
    auto& [_uuid, session] = *monitor_it;
    auto& [adv_monitor, _watcher] = session;

    LOG(INFO) << __func__ << ": Removing advertising monitor "
              << adv_monitor->getObject().getObjectPath();
    adv_monitor->emitInterfacesRemovedSignal({sdbus::InterfaceName(
        org::bluez::AdvertisementMonitor1_adaptor::INTERFACE_NAME)});
  }
  active_adv_monitors_.erase(monitor_it);
  cur_monitored_service_uuid_ = std::nullopt;

  return true;
}
std::unique_ptr<api::ble::BleMedium::ScanningSession>
BleV2Medium::StartScanning(const Uuid& service_uuid,
                           api::ble::TxPowerLevel tx_power_level,
                           ScanningCallback callback) {
  if (!WaitForAdvertisementMonitorManager() || !MonitorManagerSupportsOr()) {
    if (!StartFallbackScan(service_uuid, std::move(callback))) {
      return nullptr;
    }
    return std::make_unique<ScanningSession>(ScanningSession{
        .stop_scanning =
            [this, service_uuid]() {
              StopFallbackScan(service_uuid);
              return absl::OkStatus();
            },
    });
  }

  absl::MutexLock lock(&active_adv_monitors_mutex_);
  if (active_adv_monitors_.count(service_uuid) == 1) {
    LOG(ERROR) << __func__ << ": Service " << std::string{service_uuid}
               << " is already being advertised";
    return nullptr;
  }

  auto monitor = std::make_unique<bluez::AdvertisementMonitor>(
      *system_bus_, service_uuid, tx_power_level, "or_patterns", devices_,
      std::move(callback));
  try {
    monitor->emitInterfacesAddedSignal({sdbus::InterfaceName(
        org::bluez::AdvertisementMonitor1_adaptor::INTERFACE_NAME)});
  } catch (const sdbus::Error& e) {
    LOG(ERROR) << __func__
               << ": error emitting InterfacesAdded signal for object path "
               << monitor->getObject().getObjectPath() << " with name '"
               << e.getName() << "' and message '" << e.getMessage() << "'";
    return nullptr;
  }

  auto device_watcher = std::make_unique<DeviceWatcher>(
      *system_bus_, adapter_.GetObjectPath(), adapter_, devices_);
  if (!StartLEDiscovery()) {
    LOG(ERROR) << __func__ << ": Could not start LE discovery on adapter "
               << adapter_.GetObjectPath();
    try {
      monitor->emitInterfacesRemovedSignal({sdbus::InterfaceName(
          org::bluez::AdvertisementMonitor1_adaptor::INTERFACE_NAME)});
    } catch (const sdbus::Error& e) {
      LOG(ERROR) << __func__
                 << ": error emitting InterfacesRemoved signal for object path "
                 << monitor->getObject().getObjectPath() << " with name '"
                 << e.getName() << "' and message '" << e.getMessage() << "'";
    }
    return nullptr;
  }

  active_adv_monitors_[service_uuid] =
      std::make_pair(std::move(monitor), std::move(device_watcher));

  return std::make_unique<ScanningSession>(
      ScanningSession{.stop_scanning = [this, service_uuid]() {
        absl::MutexLock lock(&active_adv_monitors_mutex_);
        if (active_adv_monitors_.count(service_uuid) == 0) {
          LOG(ERROR) << __func__ << ": Advertising monitor for service "
                     << std::string{service_uuid} << " does not exist anymore";
          return absl::NotFoundError(
              "Advertising monitor for this service does not exist");
        }

        auto& [monitor, watcher] = active_adv_monitors_[service_uuid];
        try {
          monitor->emitInterfacesRemovedSignal({sdbus::InterfaceName(
              org::bluez::AdvertisementMonitor1_adaptor::INTERFACE_NAME)});
        } catch (const sdbus::Error& e) {
          LOG(ERROR)
              << __func__
              << ": error emitting InterfacesRemoved signal for object path "
              << monitor->getObject().getObjectPath() << " with name '"
              << e.getName() << "' and message '" << e.getMessage() << "'";
        }

        auto& adapter = adapter_.GetBluezAdapterObject();
        absl::Status status;
        try {
          adapter.StopDiscovery();
          status = absl::OkStatus();
        } catch (const sdbus::Error& e) {
          DBUS_LOG_METHOD_CALL_ERROR(&adapter, "StopDiscovery", e);
          status = absl::InternalError(e.getMessage());
        }
        active_adv_monitors_.erase(service_uuid);
        return status;
      }});
}

std::unique_ptr<api::ble::GattServer> BleV2Medium::StartGattServer(
    api::ble::ServerGattConnectionCallback callback) {
  LOG(INFO) << __func__ << ": Starting Linux GATT server.";
  return std::make_unique<GattServer>(*system_bus_, adapter_, devices_,
                                      std::move(callback));
}

std::unique_ptr<api::ble::GattClient> BleV2Medium::ConnectToGattServer(
    api::ble::BlePeripheral::UniqueId peripheral_id,
    api::ble::TxPowerLevel tx_power_level,
    api::ble::ClientGattConnectionCallback callback) {
  auto device = devices_->get_device_by_unique_id(peripheral_id);
  if (!device) {
    LOG(ERROR) << __func__ << ": Failed to find device with unique ID "
               << peripheral_id;
    return nullptr;
  }

  // Tx power is not configurable for this Linux GATT path yet.
  (void)tx_power_level;

  auto peripheral_object_path = device->GetObjectPath();
  LOG(INFO) << __func__ << ": Creating Linux GATT client for peripheral "
            << peripheral_object_path;
  return std::make_unique<GattClient>(system_bus_, peripheral_object_path,
                                      gatt_discovery_,
                                      std::move(callback.disconnected_cb));
}

std::unique_ptr<api::ble::BleServerSocket> BleV2Medium::OpenServerSocket(
    const std::string& service_id) {
  LOG(INFO) << __func__ << ": Opening BLE server socket for service "
            << service_id;
  return std::make_unique<BleV2ServerSocket>(service_id);
}

std::unique_ptr<api::ble::BleL2capServerSocket>
BleV2Medium::OpenL2capServerSocket(const std::string& service_id) {
  // return nullptr;
  LOG(INFO) << __func__ << ": Opening L2CAP server socket for service "
            << service_id;

  auto server_socket =
      std::make_unique<linux::BleL2capServerSocket>(psm_, service_id);

  return server_socket;
}

// This is supposed to be for a socket on top of Weave protocol.
std::unique_ptr<api::ble::BleSocket> BleV2Medium::Connect(
    const std::string& service_id, api::ble::TxPowerLevel tx_power_level,
    api::ble::BlePeripheral::UniqueId peripheral_id,
    CancellationFlag* cancellation_flag) {
  LOG(INFO) << __func__ << ": Not implemented on linux ";
  return nullptr;
}

bool BleV2Medium::IsExtendedAdvertisementsAvailable() {
  try {
    auto supported_channels = adv_manager_->SupportedSecondaryChannels();
    return !supported_channels.empty();
  } catch (const sdbus::Error& e) {
    DBUS_LOG_PROPERTY_GET_ERROR(adv_manager_, "SupportedSecondaryChannels", e);
    return false;
  }
}

bool BleV2Medium::StartLEDiscovery() {
  std::map<std::string, sdbus::Variant> filter;
  filter["Transport"] = sdbus::Variant("auto");
  filter["DuplicateData"] = sdbus::Variant(true);
  auto& adapter = adapter_.GetBluezAdapterObject();

  try {
    adapter.SetDiscoveryFilter(filter);
  } catch (const sdbus::Error& e) {
    DBUS_LOG_METHOD_CALL_ERROR(&adapter, "SetDiscoveryFilter", e);
    return false;
  }

  try {
    LOG(INFO) << __func__ << ": Starting LE discovery on "
              << adapter.getProxy().getObjectPath();
    adapter.StartDiscovery();
  } catch (const sdbus::Error& e) {
    if (e.getName() != "org.bluez.Error.InProgress") {
      DBUS_LOG_METHOD_CALL_ERROR(&adapter, "StartDiscovery", e);
      return false;
    }
  }

  return true;
}

// std::unique_ptr<api::ble::BleSocket> BleV2Medium::Connect(
//     const std::string &service_id, api::ble::TxPowerLevel tx_power_level,
//     api::ble::BlePeripheral &peripheral,
//     CancellationFlag *cancellation_flag) {
//   LOG(WARNING) << __func__ << ": BLE socket connections not implemented on
//   Linux"; return nullptr;
// }

std::unique_ptr<api::ble::BleL2capSocket> BleV2Medium::ConnectOverL2cap(
    int psm, const std::string& service_id,
    api::ble::TxPowerLevel tx_power_level,
    api::ble::BlePeripheral::UniqueId peripheral_id,
    CancellationFlag* cancellation_flag) {
  auto device = devices_->get_device_by_unique_id(peripheral_id);
  if (!device) {
    LOG(ERROR) << __func__ << ": Failed to find device with unique ID "
               << peripheral_id;
    return nullptr;
  }

  auto freshest = [&]() -> std::shared_ptr<BluetoothDevice> {
    if (service_data_discovery_ == nullptr) {
      return nullptr;
    }
    auto id = service_data_discovery_->FreshestPeripheral(peripheral_id);
    return id.has_value() ? devices_->get_device_by_unique_id(*id) : nullptr;
  };

  if (auto fresh = freshest()) {
    LOG(INFO) << __func__ << ": Using fresher address "
              << fresh->GetMacAddress().ToString() << " instead of "
              << device->GetMacAddress().ToString();
    device = fresh;
  }

  LOG(INFO) << __func__ << ": Connecting to L2CAP PSM " << psm << " on device "
            << device->GetMacAddress().ToString();
  int fd = ConnectL2cap(*device, psm, cancellation_flag);
  if (fd < 0 && !(cancellation_flag != nullptr && cancellation_flag->Cancelled())) {
    auto fresh = freshest();
    if (fresh && fresh->GetMacAddress().address() !=
                     device->GetMacAddress().address()) {
      LOG(INFO) << __func__ << ": Retrying on fresher address "
                << fresh->GetMacAddress().ToString();
      fd = ConnectL2cap(*fresh, psm, cancellation_flag);
    }
  }
  if (fd < 0) {
    return nullptr;
  }

  LOG(INFO) << __func__ << ": Successfully connected to L2CAP socket";
  return std::make_unique<BleL2capSocket>(fd, peripheral_id, service_id);
}

bool BleV2Medium::StartMultipleServicesScanning(
    const std::vector<Uuid>& service_uuids,
    api::ble::TxPowerLevel tx_power_level, ScanCallback callback) {
  LOG(WARNING) << __func__
               << ": Multiple services scanning not implemented on Linux. "
               << "Use single service scanning instead.";
  return false;
}

bool BleV2Medium::PauseMediumScanning() {
  LOG(INFO) << __func__
            << ": Pause scanning not implemented, returning success";
  return true;
}

bool BleV2Medium::ResumeMediumScanning() {
  LOG(INFO) << __func__
            << ": Resume scanning not implemented, returning success";
  return true;
}

void BleV2Medium::AddAlternateUuidForService(uint16_t uuid,
                                             const std::string& service_id) {
  LOG(INFO) << __func__
            << ": Alternate UUID mapping not implemented. UUID: " << uuid
            << ", service_id: " << service_id;
}

std::optional<api::ble::BlePeripheral::UniqueId>
BleV2Medium::RetrieveBlePeripheralIdFromNativeId(
    const std::string& ble_peripheral_native_id) {
  LOG(WARNING) << __func__
               << ": Retrieval from native ID not implemented. Native ID: "
               << ble_peripheral_native_id;
  return std::nullopt;
}

}  // namespace linux
}  // namespace nearby

extern "C" int nearby_linux_ble_advertising_ok() {
  absl::MutexLock lock(&nearby::linux::g_ble_adv_mutex);
  return nearby::linux::g_ble_adv_ok ? 1 : 0;
}

extern "C" void nearby_linux_copy_ble_advertising_error(char* buffer,
                                                        int buffer_size) {
  if (buffer == nullptr || buffer_size <= 0) {
    return;
  }
  absl::MutexLock lock(&nearby::linux::g_ble_adv_mutex);
  std::snprintf(buffer, buffer_size, "%s",
                nearby::linux::g_ble_adv_error.c_str());
}
