#ifndef SHARING_LINUX_LIB_QUICKSHARE_H_
#define SHARING_LINUX_LIB_QUICKSHARE_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Plain C interface to the Nearby Share sharing service. All other symbols in
 * libquickshare.so are hidden.
 *
 * Threading: functions may be called from any thread. Every callback runs on
 * a library-owned thread, or synchronously inside the call that triggered it,
 * so receivers must copy what they need and hand off to their own thread. No
 * callback is invoked once quickshare_shutdown() or quickshare_destroy() has
 * returned. Completion callbacks (QuickShareStatusFn, QuickShareDoneFn) fire at
 * most once and may be dropped if the library shuts down first.
 */

typedef struct QuickShare QuickShare;

/* Mirrors NearbySharingService::StatusCodes. */
enum QuickShareStatusCode {
  QUICKSHARE_OK = 0,
  QUICKSHARE_ERROR = 1,
  QUICKSHARE_OUT_OF_ORDER_API_CALL = 2,
  QUICKSHARE_ALREADY_STOPPED = 3,
  QUICKSHARE_TRANSFER_ALREADY_IN_PROGRESS = 4,
  QUICKSHARE_NO_AVAILABLE_CONNECTION_MEDIUM = 5,
  QUICKSHARE_IRRECOVERABLE_HARDWARE_ERROR = 6,
  QUICKSHARE_INVALID_ARGUMENT = 7
};

/* Mirrors TransferMetadata::Status. */
enum QuickShareTransferStatus {
  QUICKSHARE_TRANSFER_UNKNOWN = 0,
  QUICKSHARE_TRANSFER_CONNECTING = 1,
  QUICKSHARE_TRANSFER_AWAITING_LOCAL_CONFIRMATION = 2,
  QUICKSHARE_TRANSFER_AWAITING_REMOTE_ACCEPTANCE = 3,
  QUICKSHARE_TRANSFER_IN_PROGRESS = 4,
  QUICKSHARE_TRANSFER_COMPLETE = 5,
  QUICKSHARE_TRANSFER_FAILED = 6,
  QUICKSHARE_TRANSFER_REJECTED = 7,
  QUICKSHARE_TRANSFER_CANCELLED = 8,
  QUICKSHARE_TRANSFER_TIMED_OUT = 9,
  QUICKSHARE_TRANSFER_MEDIA_UNAVAILABLE = 10,
  QUICKSHARE_TRANSFER_NOT_ENOUGH_SPACE = 11,
  QUICKSHARE_TRANSFER_UNSUPPORTED_ATTACHMENT_TYPE = 12,
  QUICKSHARE_TRANSFER_DEVICE_AUTHENTICATION_FAILED = 13,
  QUICKSHARE_TRANSFER_INCOMPLETE_PAYLOADS = 14
};

typedef struct QuickShareTarget {
  int64_t id;
  char device_name[256];
  /* ShareTargetType: 0 unknown, 1 phone, 2 tablet, 3 laptop, 4 car, 5 foldable. */
  int type;
} QuickShareTarget;

typedef struct QuickShareTransfer {
  QuickShareTarget target;
  /* Non-zero for the receive surface, zero for the send surface. */
  int receive_mode;
  int status;           /* QuickShareTransferStatus */
  char status_name[32]; /* e.g. "kInProgress" */
  int is_final_status;
  double progress_percent; /* 0..100 */
  uint64_t transferred_bytes;
  int64_t total_bytes;
  int attachment_count;
  int total_attachments_count;
  int transferred_attachments_count;
} QuickShareTransfer;

typedef void (*QuickShareStatusFn)(int status, void* user);
typedef void (*QuickShareDoneFn)(void* user);

typedef struct QuickShareCallbacks {
  /* A share target was discovered or its details changed. */
  void (*on_target_found)(const QuickShareTarget* target, void* user);
  void (*on_target_lost)(const QuickShareTarget* target, void* user);
  void (*on_transfer_update)(const QuickShareTransfer* transfer, void* user);
  void* user;
} QuickShareCallbacks;

typedef struct QuickShareFastInitCallbacks {
  void (*on_devices_discovered)(void* user);
  void (*on_devices_not_discovered)(void* user);
  /* error mirrors nearby::api::FastInitiationManager::Error. */
  void (*on_error)(int error, void* user);
  void* user;
} QuickShareFastInitCallbacks;

/* Opens the process-wide native log file. Call once, before quickshare_create. */
void quickshare_logging_init(void);
void quickshare_logging_shutdown(void);

/* Returns null if the sharing service could not be created. */
QuickShare* quickshare_create(void);
void quickshare_destroy(QuickShare* qs);
void quickshare_set_callbacks(QuickShare* qs,
                              const QuickShareCallbacks* callbacks);

/* Applies Wi-Fi-only data usage and "Everyone" visibility. */
void quickshare_initialize(QuickShare* qs, QuickShareStatusFn done, void* user);
/* Blocks up to 10 seconds. Returns a QuickShareStatusCode. */
int quickshare_shutdown(QuickShare* qs);

void quickshare_register_receive_surface(QuickShare* qs, QuickShareStatusFn done,
                                         void* user);
void quickshare_unregister_receive_surface(QuickShare* qs,
                                           QuickShareStatusFn done, void* user);
void quickshare_register_send_surface(QuickShare* qs, QuickShareStatusFn done,
                                      void* user);
void quickshare_unregister_send_surface(QuickShare* qs, QuickShareStatusFn done,
                                        void* user);

void quickshare_send_files(QuickShare* qs, int64_t target_id,
                           const char* const* paths, int count,
                           QuickShareStatusFn done, void* user);
void quickshare_accept(QuickShare* qs, int64_t target_id,
                       QuickShareStatusFn done, void* user);
void quickshare_reject(QuickShare* qs, int64_t target_id,
                       QuickShareStatusFn done, void* user);
void quickshare_cancel(QuickShare* qs, int64_t target_id,
                       QuickShareStatusFn done, void* user);

int quickshare_fast_init_is_scanning(QuickShare* qs);
void quickshare_fast_init_start_scanning(
    QuickShare* qs, const QuickShareFastInitCallbacks* callbacks);
/* done may be null. */
void quickshare_fast_init_stop_scanning(QuickShare* qs, QuickShareDoneFn done,
                                        void* user);

int quickshare_is_bluetooth_powered(QuickShare* qs);
int quickshare_is_lan_connected(QuickShare* qs);
/* Whether a Wi-Fi Direct medium could be created; probed once at create. */
int quickshare_wifi_direct_available(QuickShare* qs);
int quickshare_pairing_prompt_count(QuickShare* qs);
int quickshare_ble_advertising_ok(QuickShare* qs);
void quickshare_copy_ble_advertising_error(QuickShare* qs, char* buffer,
                                           int buffer_size);

/* Writes a NUL-terminated description of a QuickShareStatusCode. */
void quickshare_status_string(int status, char* buffer, int buffer_size);

#ifdef __cplusplus
}
#endif

#endif  // SHARING_LINUX_LIB_QUICKSHARE_H_
