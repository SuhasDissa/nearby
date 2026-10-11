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

#include <vector>

#include "internal/platform/implementation/ble.h"
#include "internal/platform/implementation/linux/bluez.h"
#include "internal/platform/implementation/linux/bluez_le_advertisement.h"
#include "internal/platform/logging.h"
#include "internal/platform/uuid.h"

namespace nearby {
namespace linux {
namespace bluez {
  std::string BytesToHexString(const std::vector<uint8_t>& bytes) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (uint8_t b : bytes) {
      oss << std::setw(2) << static_cast<int>(b);
    }
    return oss.str();
  }
LEAdvertisement::LEAdvertisement(
    sdbus::IConnection& system_bus, sdbus::ObjectPath path,
    const api::ble::BleAdvertisementData& advertising_data,
    api::ble::AdvertiseParameters advertise_set_parameters, std::string type,
    std::optional<int16_t> tx_power_override)
    : AdaptorInterfaces(system_bus, std::move(path)),
      is_extended_advertisement_(advertising_data.is_extended_advertisement),
      advertise_set_parameters_(advertise_set_parameters),
      type_(std::move(type)),
      tx_power_override_(tx_power_override) {
  for (const auto& [uuid, data] : advertising_data.service_data) {
    std::string uuid_string(uuid);
    std::vector<uint8_t> data_bytes(data.size());
    const auto* bytes = data.data();

    if (is_extended_advertisement_) {
      service_uuids_.push_back(uuid_string);
    }
    // service_uuids_.push_back("0000FE2C-0000-1000-8000-00805F9B34FB");
    // service_uuids_.push_back("0000FE2C-0000-1000-8000-00805F9B34FB");
    for (size_t i = 0; i < data.size(); i++) {
      data_bytes[i] = bytes[i];
    }
    LOG(INFO)<< __func__ << ": " << uuid_string;
    LOG(INFO)<< __func__ << ": " << BytesToHexString(data_bytes);
    service_data_.insert({uuid_string, std::move(sdbus::Variant(data_bytes))});
  }

  // This controller accepts a single LE advertisement. The phone's share
  // sheet looks for the fast-init UUID 0xFE2C, while the Quick Share payload
  // is on 0xFEF3. Put the notify beacon in the same extended packet.
  if (is_extended_advertisement_ &&
      service_data_.count("0000FE2C-0000-1000-8000-00805F9B34FB") == 0 &&
      service_data_.count("0000fe2c-0000-1000-8000-00805f9b34fb") == 0) {
    const std::vector<uint8_t> fast_init = {
        0xfc, 0x12, 0x8e, 0x00, 0x2d, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80};
    service_data_.insert(
        {"0000FE2C-0000-1000-8000-00805F9B34FB", sdbus::Variant(fast_init)});
    service_uuids_.push_back("0000FE2C-0000-1000-8000-00805F9B34FB");
  }

  registerAdaptor();

  LOG(INFO) << __func__
                       << ": Created a org.bluez.LEAdvertisement1 instance at "
                       << getObject().getObjectPath();
}
}  // namespace bluez
}  // namespace linux
}  // namespace nearby
