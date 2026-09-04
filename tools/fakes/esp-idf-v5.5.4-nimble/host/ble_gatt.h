#pragma once

#ifndef S3_HIDBOT_U76A_NIMBLE_FAKE
#error "The U7.6A NimBLE fake is test-only"
#endif

#include <stdint.h>

#include "host/ble_uuid.h"

struct os_mbuf;
struct ble_gatt_access_ctxt;
struct ble_gatt_chr_def;
struct ble_gatt_dsc_def;
struct ble_gatt_svc_def;
struct ble_gatt_cpfd;

typedef int ble_gatt_access_fn(uint16_t connection_handle,
                               uint16_t attribute_handle,
                               struct ble_gatt_access_ctxt *context,
                               void *argument);
typedef uint32_t ble_gatt_chr_flags;

// Values from ESP-IDF v5.5.4 host/ble_gatt.h used by production here.
#define BLE_GATT_ACCESS_OP_READ_CHR 0
#define BLE_GATT_ACCESS_OP_WRITE_CHR 1
#define BLE_GATT_ACCESS_OP_READ_DSC 2
#define BLE_GATT_ACCESS_OP_WRITE_DSC 3

#define BLE_GATT_CHR_F_READ 0x00000002
#define BLE_GATT_CHR_F_WRITE_NO_RSP 0x00000004
#define BLE_GATT_CHR_F_NOTIFY 0x00000010
#define BLE_GATT_CHR_F_READ_AUTHEN 0x00000400
#define BLE_GATT_CHR_F_WRITE_AUTHEN 0x00002000
#define BLE_GATT_CHR_F_NOTIFY_INDICATE_AUTHEN 0x00010000
#define BLE_GATT_CHR_F_NOTIFY_INDICATE_AUTHOR 0x00020000

#define BLE_GATT_SVC_TYPE_END 0
#define BLE_GATT_SVC_TYPE_PRIMARY 1
#define BLE_GATT_SVC_TYPE_SECONDARY 2

// Field order and types match v5.5.4 for the initialized/consumed surface.
struct ble_gatt_chr_def {
    const ble_uuid_t *uuid;
    ble_gatt_access_fn *access_cb;
    void *arg;
    struct ble_gatt_dsc_def *descriptors;
    ble_gatt_chr_flags flags;
    uint8_t min_key_size;
    uint16_t *val_handle;
    struct ble_gatt_cpfd *cpfd;
};

struct ble_gatt_svc_def {
    uint8_t type;
    const ble_uuid_t *uuid;
    const struct ble_gatt_svc_def **includes;
    const struct ble_gatt_chr_def *characteristics;
};

struct ble_gatt_dsc_def {
    const ble_uuid_t *uuid;
    uint8_t att_flags;
    uint8_t min_key_size;
    ble_gatt_access_fn *access_cb;
    void *arg;
};

struct ble_gatt_access_ctxt {
    uint8_t op;
    struct os_mbuf *om;
    union {
        const struct ble_gatt_chr_def *chr;
        const struct ble_gatt_dsc_def *dsc;
    };
    uint16_t offset;
};

#ifdef __cplusplus
extern "C" {
#endif

int ble_gatts_count_cfg(const struct ble_gatt_svc_def *services);
int ble_gatts_add_svcs(const struct ble_gatt_svc_def *services);
int ble_gatts_find_svc(const ble_uuid_t *uuid, uint16_t *output_handle);
int ble_gatts_find_chr(const ble_uuid_t *service_uuid,
                       const ble_uuid_t *characteristic_uuid,
                       uint16_t *output_definition_handle,
                       uint16_t *output_value_handle);
int ble_gatts_notify_custom(uint16_t connection_handle,
                            uint16_t attribute_handle,
                            struct os_mbuf *mbuf);

#ifdef __cplusplus
}
#endif
