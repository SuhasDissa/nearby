#pragma once

#include <cstdint>

#include "quickshare.h"

// Stand-in for libquickshare.so used by backend_test. Completion callbacks run
// synchronously inside the call, like the old in-process service fakes.
struct QuickShare {
  QuickShareCallbacks callbacks = {};
  QuickShareFastInitCallbacks fast_init_callbacks = {};
  bool scanning = false;
  bool send_registered = false;
  bool receive_registered = false;
  QuickShareDoneFn stopped_callback = nullptr;
  void* stopped_user = nullptr;
  int accept_count = 0;
  int reject_count = 0;
  int64_t last_accepted_target = 0;
  int64_t last_rejected_target = 0;

  void FireDiscovered() {
    fast_init_callbacks.on_devices_discovered(fast_init_callbacks.user);
  }
  void FireNotDiscovered() {
    fast_init_callbacks.on_devices_not_discovered(fast_init_callbacks.user);
  }
  void FireError(int error) {
    fast_init_callbacks.on_error(error, fast_init_callbacks.user);
  }
  void CompleteStop();
  void FireReceiveTransfer(int64_t target_id, int status);
};
