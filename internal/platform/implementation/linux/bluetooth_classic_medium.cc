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

#include <cerrno>
#include <chrono>
#include <cstring>
#include <memory>

#include <bluetooth/bluetooth.h>
#include <bluetooth/rfcomm.h>
#include <bluetooth/sdp.h>
#include <bluetooth/sdp_lib.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <sdbus-c++/IProxy.h>
#include <sdbus-c++/Types.h>

#include "absl/strings/string_view.h"
#include "internal/platform/implementation/bluetooth_classic.h"
#include "internal/platform/implementation/linux/bluetooth_adapter.h"
#include "internal/platform/implementation/linux/bluez_agent.h"
#include "internal/platform/implementation/linux/bluetooth_bluez_profile.h"
#include "internal/platform/implementation/linux/bluetooth_classic_device.h"
#include "internal/platform/implementation/linux/bluetooth_classic_medium.h"

#include "internal/platform/implementation/linux/bluetooth_classic_server_socket.h"
#include "internal/platform/implementation/linux/bluetooth_classic_socket.h"
#include "internal/platform/implementation/linux/stream.h"
#include "internal/platform/logging.h"

namespace nearby {
namespace linux {
namespace {

constexpr char kBluezAgentPath[] = "/com/google/nearby/bluetooth/agent";
constexpr int kRfcommConnectTimeoutMs = 15000;
constexpr int kRfcommPollSliceMs = 200;

class MacOnlyDevice : public api::BluetoothDevice {
 public:
  explicit MacOnlyDevice(MacAddress mac_address) : mac_address_(mac_address) {}

  std::string GetName() const override { return ""; }
  MacAddress GetMacAddress() const override { return mac_address_; }

 private:
  MacAddress mac_address_;
};

class RfcommSocket final : public api::BluetoothSocket {
 public:
  RfcommSocket(api::BluetoothDevice *device, int fd)
      : fd_(fd), device_(device), output_stream_(fd_), input_stream_(fd_) {}

  InputStream &GetInputStream() override { return input_stream_; }
  OutputStream &GetOutputStream() override { return output_stream_; }
  Exception Close() override {
    input_stream_.Close();
    output_stream_.Close();
    if (fd_ >= 0) {
      close(fd_);
      fd_ = -1;
    }
    return Exception{Exception::kSuccess};
  }
  api::BluetoothDevice *GetRemoteDevice() override { return device_; }

 private:
  int fd_;
  api::BluetoothDevice *device_;
  linux::OutputStream output_stream_;
  linux::InputStream input_stream_;
};

int FindRfcommChannel(const bdaddr_t &remote, const std::string &service_uuid) {
  std::string hex;
  for (char c : service_uuid) {
    if (c != '-') hex += c;
  }
  if (hex.size() != 32) {
    LOG(ERROR) << __func__ << ": Invalid service UUID " << service_uuid;
    return -1;
  }
  uint8_t bytes[16];
  for (int i = 0; i < 16; ++i) {
    bytes[i] = static_cast<uint8_t>(std::stoi(hex.substr(i * 2, 2), nullptr, 16));
  }
  uuid_t uuid;
  sdp_uuid128_create(&uuid, bytes);

  bdaddr_t any = {};
  sdp_session_t *session = sdp_connect(&any, &remote, SDP_RETRY_IF_BUSY);
  if (session == nullptr) {
    LOG(ERROR) << __func__
               << ": sdp_connect failed: " << std::strerror(errno);
    return -1;
  }

  sdp_list_t *search = sdp_list_append(nullptr, &uuid);
  uint32_t range = 0x0000ffff;
  sdp_list_t *attrs = sdp_list_append(nullptr, &range);
  sdp_list_t *response = nullptr;
  int channel = -1;

  if (sdp_service_search_attr_req(session, search, SDP_ATTR_REQ_RANGE, attrs,
                                  &response) != 0) {
    LOG(ERROR) << __func__ << ": SDP search failed: " << std::strerror(errno);
  } else {
    for (sdp_list_t *r = response; r != nullptr && channel < 0; r = r->next) {
      sdp_list_t *protos = nullptr;
      if (sdp_get_access_protos(static_cast<sdp_record_t *>(r->data),
                                &protos) == 0) {
        int port = sdp_get_proto_port(protos, RFCOMM_UUID);
        if (port > 0) channel = port;
        for (sdp_list_t *p = protos; p != nullptr; p = p->next) {
          sdp_list_free(static_cast<sdp_list_t *>(p->data), nullptr);
        }
        sdp_list_free(protos, nullptr);
      }
      sdp_record_free(static_cast<sdp_record_t *>(r->data));
    }
    sdp_list_free(response, nullptr);
  }

  sdp_list_free(search, nullptr);
  sdp_list_free(attrs, nullptr);
  sdp_close(session);
  return channel;
}

int ConnectRfcomm(const bdaddr_t &remote, int channel,
                  CancellationFlag *cancellation_flag) {
  int fd = socket(AF_BLUETOOTH, SOCK_STREAM, BTPROTO_RFCOMM);
  if (fd < 0) {
    LOG(ERROR) << __func__ << ": socket failed: " << std::strerror(errno);
    return -1;
  }

  struct bt_security sec = {};
  sec.level = BT_SECURITY_LOW;
  if (setsockopt(fd, SOL_BLUETOOTH, BT_SECURITY, &sec, sizeof(sec)) != 0) {
    LOG(WARNING) << __func__
                 << ": BT_SECURITY failed: " << std::strerror(errno);
  }

  int flags = fcntl(fd, F_GETFL, 0);
  fcntl(fd, F_SETFL, flags | O_NONBLOCK);

  struct sockaddr_rc addr = {};
  addr.rc_family = AF_BLUETOOTH;
  addr.rc_channel = static_cast<uint8_t>(channel);
  bacpy(&addr.rc_bdaddr, &remote);

  int err = 0;
  if (connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
    if (errno != EINPROGRESS) {
      err = errno;
    } else {
      err = ETIMEDOUT;
      for (int waited = 0; waited < kRfcommConnectTimeoutMs;
           waited += kRfcommPollSliceMs) {
        if (cancellation_flag != nullptr && cancellation_flag->Cancelled()) {
          err = ECANCELED;
          break;
        }
        struct pollfd pfd = {fd, POLLOUT, 0};
        int rc = poll(&pfd, 1, kRfcommPollSliceMs);
        if (rc < 0 && errno != EINTR) {
          err = errno;
          break;
        }
        if (rc > 0) {
          socklen_t len = sizeof(err);
          if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0) {
            err = errno;
          }
          break;
        }
      }
    }
  }

  if (err != 0) {
    LOG(ERROR) << __func__ << ": RFCOMM connect to channel " << channel
               << " failed: " << std::strerror(err);
    close(fd);
    return -1;
  }

  fcntl(fd, F_SETFL, flags);
  return fd;
}

}  // namespace

BluetoothClassicMedium::BluetoothClassicMedium(BluetoothAdapter &adapter)
    : system_bus_(adapter.GetConnection()),
      adapter_(adapter),
      devices_(nullptr),
      device_watcher_(nullptr),
      agent_manager_(std::make_unique<AgentManager>(*system_bus_)),
      profile_manager_(nullptr) {
  devices_ = GetSharedBluetoothDevices(system_bus_, adapter_.GetObjectPath());
  profile_manager_ =
      std::make_unique<ProfileManager>(*system_bus_, *devices_);

  if (!agent_manager_->Register(
          /*capability=*/absl::string_view("NoInputNoOutput"),
          sdbus::ObjectPath(kBluezAgentPath))) {
    LOG(WARNING) << __func__
                 << ": Failed to register default BlueZ agent at "
                 << kBluezAgentPath;
  }
}

bool BluetoothClassicMedium::StartDiscovery(
    DiscoveryCallback discovery_callback) {
  device_watcher_ = std::make_unique<DeviceWatcher>(
      *system_bus_, adapter_.GetObjectPath(), adapter_, devices_,
      std::make_unique<DiscoveryCallback>(std::move(discovery_callback)));

  std::map<std::string, sdbus::Variant> filter;
  filter["Transport"] = sdbus::Variant("auto");
  auto &adapter = adapter_.GetBluezAdapterObject();

  try {
    adapter.SetDiscoveryFilter(filter);
  } catch (const sdbus::Error &e) {
    DBUS_LOG_METHOD_CALL_ERROR(&adapter, "SetDiscoveryFilter", e);
    device_watcher_ = nullptr;
    return false;
  }

  try {
    LOG(INFO) << __func__ << ": Starting BR/EDR discovery on "
                      << adapter_.GetObjectPath();
    adapter.StartDiscovery();
  } catch (const sdbus::Error &e) {
    if (e.getName() != "org.bluez.Error.InProgress") {
      DBUS_LOG_METHOD_CALL_ERROR(&adapter, "StartDiscovery", e);
      device_watcher_ = nullptr;
      return false;
    }
  }

  return true;
}

bool BluetoothClassicMedium::StopDiscovery() {
  auto &adapter = adapter_.GetBluezAdapterObject();
  LOG(INFO) << __func__ << "Stopping discovery on "
                    << adapter.getProxy().getObjectPath();
  auto ret = true;
  try {
    adapter.StopDiscovery();
  } catch (const sdbus::Error &e) {
    DBUS_LOG_METHOD_CALL_ERROR(&adapter, "StopDiscovery", e);
    ret = false;
  }
  device_watcher_ = nullptr;

  return ret;
}

std::unique_ptr<api::BluetoothSocket>
BluetoothClassicMedium::ConnectToServiceDirect(
    api::BluetoothDevice &remote_device, const std::string &service_uuid,
    CancellationFlag *cancellation_flag) {
  auto address = remote_device.GetMacAddress();
  bdaddr_t remote;
  str2ba(address.ToString().c_str(), &remote);

  LOG(INFO) << __func__ << ": Direct RFCOMM connect to " << address.ToString()
            << " service " << service_uuid;
  int channel = FindRfcommChannel(remote, service_uuid);
  if (channel < 0) {
    LOG(ERROR) << __func__ << ": No RFCOMM channel found via SDP";
    return nullptr;
  }
  LOG(INFO) << __func__ << ": SDP found RFCOMM channel " << channel;

  int fd = ConnectRfcomm(remote, channel, cancellation_flag);
  if (fd < 0) return nullptr;
  LOG(INFO) << __func__ << ": RFCOMM connected to " << address.ToString();

  return std::make_unique<RfcommSocket>(&remote_device, fd);
}

std::unique_ptr<api::BluetoothSocket> BluetoothClassicMedium::ConnectToService(
    api::BluetoothDevice &remote_device, const std::string &service_uuid,
    CancellationFlag *cancellation_flag) {
  if (devices_->get_device_by_address(remote_device.GetMacAddress()) ==
      nullptr) {
    return ConnectToServiceDirect(remote_device, service_uuid,
                                  cancellation_flag);
  }

  if (!profile_manager_->ProfileRegistered(service_uuid)) {
    if (!profile_manager_->Register(std::nullopt, service_uuid)) {
      LOG(ERROR) << __func__ << ": Could not register profile "
                         << service_uuid << " with Bluez";
      return nullptr;
    }
  }

  auto address = remote_device.GetMacAddress();
  auto device = devices_->get_device_by_address(address);
  if (device == nullptr) {
    LOG(ERROR) << __func__ << ": Device " << address.ToString()
                       << " is no longer known";
    return nullptr;
  }
  if (!device -> Bonded())
  {

    LOG(ERROR) << __func__ << ": Device " << address.ToString()
                       << " is not Bonded";
  }
  // Mark as pending BEFORE calling ConnectToProfile to win the race
  profile_manager_->MarkPendingOutgoing(service_uuid, address);

  if (!device->ConnectToProfile(service_uuid)) {
    profile_manager_->ClearPendingOutgoing(service_uuid, address);
    return ConnectToServiceDirect(remote_device, service_uuid,
                                  cancellation_flag);
  }

  auto fd = profile_manager_->GetServiceRecordFD(remote_device, service_uuid,
                                                 cancellation_flag);
  if (!fd.has_value()) {
    LOG(WARNING) << __func__
                         << ": Failed to get a new connection for profile "
                         << service_uuid << " for device " << address.ToString();
    return nullptr;
  }

  return std::unique_ptr<api::BluetoothSocket>(
      new BluetoothSocket(device, fd.value()));
}

std::shared_ptr<api::BluetoothServerSocket>
BluetoothClassicMedium::ListenForService(const std::string &service_name,
                                         const std::string &service_uuid) {
  if (!profile_manager_->ProfileRegistered(service_uuid)) {
    if (!profile_manager_->Register(service_name, service_uuid)) {
      LOG(ERROR) << __func__ << ": Could not register profile "
                         << service_name << " " << service_uuid
                         << " with Bluez";
      return nullptr;
    }
  }

  return std::shared_ptr<api::BluetoothServerSocket>(
      new BluetoothServerSocket(*profile_manager_, service_uuid));
}

api::BluetoothDevice *BluetoothClassicMedium::GetRemoteDevice(
MacAddress mac_address) {
  // When BLE is discovering, it looks for remote devices to connect to using BT classic. If only
  auto device = devices_->get_device_by_address(mac_address);
  if (device != nullptr) return device.get();

  absl::MutexLock lock(&mac_only_devices_mutex_);
  auto &entry = mac_only_devices_[mac_address.ToString()];
  if (entry == nullptr) entry = std::make_unique<MacOnlyDevice>(mac_address);
  return entry.get();
}

}  // namespace linux
}  // namespace nearby
