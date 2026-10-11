#include "sharing/linux/app/fake_quickshare.h"

#include <cstdio>
#include <cstring>

void QuickShare::CompleteStop() {
  QuickShareDoneFn done = stopped_callback;
  stopped_callback = nullptr;
  if (done != nullptr) {
    done(stopped_user);
  }
}

void QuickShare::FireReceiveTransfer(int64_t target_id, int status) {
  QuickShareTransfer transfer = {};
  transfer.target.id = target_id;
  transfer.receive_mode = 1;
  transfer.status = status;
  const char* name = status == QUICKSHARE_TRANSFER_COMPLETE
                         ? "kComplete"
                         : "kAwaitingLocalConfirmation";
  std::snprintf(transfer.status_name, sizeof(transfer.status_name), "%s", name);
  transfer.is_final_status = status == QUICKSHARE_TRANSFER_COMPLETE;
  callbacks.on_transfer_update(&transfer, callbacks.user);
}

extern "C" {

void quickshare_logging_init(void) {}
void quickshare_logging_shutdown(void) {}

QuickShare* quickshare_create(void) { return new QuickShare; }
void quickshare_destroy(QuickShare* qs) { delete qs; }
void quickshare_set_callbacks(QuickShare* qs,
                              const QuickShareCallbacks* callbacks) {
  qs->callbacks = *callbacks;
}

void quickshare_initialize(QuickShare*, QuickShareStatusFn done, void* user) {
  done(QUICKSHARE_OK, user);
}
int quickshare_shutdown(QuickShare*) { return QUICKSHARE_OK; }

void quickshare_register_receive_surface(QuickShare* qs, QuickShareStatusFn done,
                                         void* user) {
  qs->receive_registered = true;
  done(QUICKSHARE_OK, user);
}
void quickshare_unregister_receive_surface(QuickShare* qs,
                                           QuickShareStatusFn done,
                                           void* user) {
  qs->receive_registered = false;
  done(QUICKSHARE_OK, user);
}
void quickshare_register_send_surface(QuickShare* qs, QuickShareStatusFn done,
                                      void* user) {
  qs->send_registered = true;
  done(QUICKSHARE_OK, user);
}
void quickshare_unregister_send_surface(QuickShare* qs, QuickShareStatusFn done,
                                        void* user) {
  qs->send_registered = false;
  done(QUICKSHARE_OK, user);
}

void quickshare_send_files(QuickShare*, int64_t, const char* const*, int,
                           QuickShareStatusFn done, void* user) {
  done(QUICKSHARE_OK, user);
}
void quickshare_accept(QuickShare* qs, int64_t target_id,
                       QuickShareStatusFn done, void* user) {
  ++qs->accept_count;
  qs->last_accepted_target = target_id;
  done(QUICKSHARE_OK, user);
}
void quickshare_reject(QuickShare* qs, int64_t target_id,
                       QuickShareStatusFn done, void* user) {
  ++qs->reject_count;
  qs->last_rejected_target = target_id;
  done(QUICKSHARE_OK, user);
}
void quickshare_cancel(QuickShare*, int64_t, QuickShareStatusFn done,
                       void* user) {
  done(QUICKSHARE_OK, user);
}

int quickshare_fast_init_is_scanning(QuickShare* qs) { return qs->scanning; }
void quickshare_fast_init_start_scanning(
    QuickShare* qs, const QuickShareFastInitCallbacks* callbacks) {
  qs->fast_init_callbacks = *callbacks;
  qs->scanning = true;
}
void quickshare_fast_init_stop_scanning(QuickShare* qs, QuickShareDoneFn done,
                                        void* user) {
  qs->stopped_callback = done;
  qs->stopped_user = user;
  qs->scanning = false;
}

int quickshare_is_bluetooth_powered(QuickShare*) { return 1; }
int quickshare_is_lan_connected(QuickShare*) { return 1; }
int quickshare_wifi_direct_available(QuickShare*) { return 0; }
int quickshare_pairing_prompt_count(QuickShare*) { return 0; }
int quickshare_ble_advertising_ok(QuickShare*) { return 1; }
void quickshare_copy_ble_advertising_error(QuickShare*, char* buffer,
                                           int buffer_size) {
  if (buffer_size > 0) {
    buffer[0] = '\0';
  }
}
void quickshare_status_string(int status, char* buffer, int buffer_size) {
  std::snprintf(buffer, static_cast<size_t>(buffer_size), "status %d", status);
}

}  // extern "C"
