#pragma once

#ifndef S3_HIDBOT_U76A_NIMBLE_FAKE
#error "The U7.6A NimBLE fake is test-only"
#endif

// ESP-IDF v5.5.4 ESP-NimBLE ATT values used by ble_hid_service.cpp.
#define BLE_ATT_ERR_READ_NOT_PERMITTED 0x02
#define BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN 0x0d
#define BLE_ATT_ERR_UNLIKELY 0x0e
#define BLE_ATT_ERR_INSUFFICIENT_RES 0x11

#define BLE_ATT_F_READ 0x01
