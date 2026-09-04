#pragma once

#ifndef S3_HIDBOT_U76A_NIMBLE_FAKE
#error "The U7.6A NimBLE fake is test-only"
#endif

#include <stdint.h>

struct os_mbuf;

// ESP-IDF v5.5.4 ESP-NimBLE host errors used by ble_hid_service.cpp.
#define BLE_HS_EALREADY 2
#define BLE_HS_EINVAL 3
#define BLE_HS_EMSGSIZE 4
#define BLE_HS_ENOENT 5
#define BLE_HS_ENOMEM 6

#ifdef __cplusplus
extern "C" {
#endif

struct os_mbuf *ble_hs_mbuf_from_flat(const void *buffer, uint16_t length);

#ifdef __cplusplus
}
#endif
