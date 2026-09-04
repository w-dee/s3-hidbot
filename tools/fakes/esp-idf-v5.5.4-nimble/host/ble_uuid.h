#pragma once

#ifndef S3_HIDBOT_U76A_NIMBLE_FAKE
#error "The U7.6A NimBLE fake is test-only"
#endif

#include <stdint.h>

// Pinned to the ESP-IDF v5.5.4 ESP-NimBLE ble_uuid.h surface used here.
#define S3_HIDBOT_NIMBLE_FAKE_IDF_VERSION 50504

enum {
    BLE_UUID_TYPE_16 = 16,
    BLE_UUID_TYPE_32 = 32,
    BLE_UUID_TYPE_128 = 128,
};

typedef struct {
    uint8_t type;
} ble_uuid_t;

typedef struct {
    ble_uuid_t u;
    uint16_t value;
} ble_uuid16_t;

typedef struct {
    ble_uuid_t u;
    uint8_t value[16];
} ble_uuid128_t;

#define BLE_UUID16_INIT(uuid16)                                             \
    {                                                                       \
        .u = {.type = BLE_UUID_TYPE_16}, .value = (uuid16),                 \
    }

#define BLE_UUID128_INIT(...)                                               \
    {                                                                       \
        .u = {.type = BLE_UUID_TYPE_128}, .value = {__VA_ARGS__},           \
    }
