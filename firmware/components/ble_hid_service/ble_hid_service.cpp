#include "ble_hid_service/ble_hid_service.hpp"

#include "host/ble_att.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "os/os_mbuf.h"

namespace ble_hid_service {
namespace {

enum class AccessTarget : std::uint8_t {
    kInformation,
    kReportMap,
    kControlPoint,
    kKeyboardReport,
    kMouseReport,
    kKeyboardReference,
    kMouseReference,
};

ble_uuid16_t s_hid_service = BLE_UUID16_INIT(0x1812);
ble_uuid16_t s_hid_information = BLE_UUID16_INIT(0x2a4a);
ble_uuid16_t s_report_map = BLE_UUID16_INIT(0x2a4b);
ble_uuid16_t s_control_point = BLE_UUID16_INIT(0x2a4c);
ble_uuid16_t s_report = BLE_UUID16_INIT(0x2a4d);
ble_uuid16_t s_report_reference = BLE_UUID16_INIT(0x2908);

std::uint16_t s_keyboard_value_handle = 0;
std::uint16_t s_mouse_value_handle = 0;
std::uint16_t s_information_value_handle = 0;
std::uint16_t s_report_map_value_handle = 0;
std::uint16_t s_control_point_value_handle = 0;
Database *s_database = nullptr;

void *target(AccessTarget value) {
    return reinterpret_cast<void *>(static_cast<std::uintptr_t>(value) + 1U);
}

AccessTarget target_from(void *argument) {
    return static_cast<AccessTarget>(reinterpret_cast<std::uintptr_t>(argument) - 1U);
}

ble_gatt_dsc_def s_keyboard_descriptors[] = {
    {.uuid = &s_report_reference.u,
     .att_flags = BLE_ATT_F_READ,
     .min_key_size = 0,
     .access_cb = Database::access,
     .arg = target(AccessTarget::kKeyboardReference)},
    {},
};

ble_gatt_dsc_def s_mouse_descriptors[] = {
    {.uuid = &s_report_reference.u,
     .att_flags = BLE_ATT_F_READ,
     .min_key_size = 0,
     .access_cb = Database::access,
     .arg = target(AccessTarget::kMouseReference)},
    {},
};

ble_gatt_chr_def s_characteristics[] = {
    {.uuid = &s_hid_information.u,
     .access_cb = Database::access,
     .arg = target(AccessTarget::kInformation),
     .flags = BLE_GATT_CHR_F_READ,
     .val_handle = &s_information_value_handle},
    {.uuid = &s_report_map.u,
     .access_cb = Database::access,
     .arg = target(AccessTarget::kReportMap),
     .flags = BLE_GATT_CHR_F_READ,
     .val_handle = &s_report_map_value_handle},
    {.uuid = &s_control_point.u,
     .access_cb = Database::access,
     .arg = target(AccessTarget::kControlPoint),
     .flags = BLE_GATT_CHR_F_WRITE_NO_RSP,
     .val_handle = &s_control_point_value_handle},
    {.uuid = &s_report.u,
     .access_cb = Database::access,
     .arg = target(AccessTarget::kKeyboardReport),
     .descriptors = s_keyboard_descriptors,
     .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
     .val_handle = &s_keyboard_value_handle},
    {.uuid = &s_report.u,
     .access_cb = Database::access,
     .arg = target(AccessTarget::kMouseReport),
     .descriptors = s_mouse_descriptors,
     .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
     .val_handle = &s_mouse_value_handle},
    {},
};

ble_gatt_svc_def s_services[] = {
    {.type = BLE_GATT_SVC_TYPE_PRIMARY,
     .uuid = &s_hid_service.u,
     .characteristics = s_characteristics},
    {},
};

template <std::size_t Size>
int append(struct os_mbuf *buffer,
           const std::array<std::uint8_t, Size> &value) {
    return os_mbuf_append(buffer, value.data(), value.size()) == 0
               ? 0
               : BLE_ATT_ERR_INSUFFICIENT_RES;
}

}  // namespace

int Database::register_database() {
    if (s_database != nullptr && s_database != this) {
        return BLE_HS_EALREADY;
    }
    s_database = this;
    int result = ble_gatts_count_cfg(s_services);
    if (result == 0) {
        result = ble_gatts_add_svcs(s_services);
    }
    return result;
}

int Database::validate_registered_database() {
    if (s_database != this) {
        return BLE_HS_ENOENT;
    }
    std::uint16_t service_handle = 0;
    int result = ble_gatts_find_svc(&s_hid_service.u, &service_handle);
    if (result != 0 || service_handle == 0) {
        return result != 0 ? result : BLE_HS_ENOENT;
    }

    struct RequiredCharacteristic {
        const ble_uuid_t *uuid;
        std::uint16_t assigned_handle;
    };
    const RequiredCharacteristic required[] = {
        {&s_hid_information.u, s_information_value_handle},
        {&s_report_map.u, s_report_map_value_handle},
        {&s_control_point.u, s_control_point_value_handle},
    };
    for (const RequiredCharacteristic &characteristic : required) {
        std::uint16_t found_handle = 0;
        result = ble_gatts_find_chr(&s_hid_service.u, characteristic.uuid, nullptr,
                                    &found_handle);
        if (result != 0 || found_handle == 0 ||
            found_handle != characteristic.assigned_handle) {
            return result != 0 ? result : BLE_HS_ENOENT;
        }
    }
    if (s_keyboard_value_handle == 0 || s_mouse_value_handle == 0 ||
        s_keyboard_value_handle == s_mouse_value_handle) {
        return BLE_HS_ENOENT;
    }
    return 0;
}

void Database::clear_peer_state() {
    subscriptions_.clear();
    suspended_.store(false, std::memory_order_release);
}

void Database::on_subscribe(std::uint16_t attribute_handle, bool enabled) {
    subscriptions_.observe(attribute_handle, s_keyboard_value_handle,
                           s_mouse_value_handle, enabled);
}

bool Database::keyboard_subscribed() const {
    return subscriptions_.keyboard();
}

bool Database::mouse_subscribed() const {
    return subscriptions_.mouse();
}

bool Database::suspended() const {
    return suspended_.load(std::memory_order_acquire);
}

int Database::access(std::uint16_t, std::uint16_t,
                     struct ble_gatt_access_ctxt *context, void *argument) {
    if (context == nullptr || argument == nullptr) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    switch (target_from(argument)) {
        case AccessTarget::kInformation:
            return append(context->om, kHidInformation);
        case AccessTarget::kReportMap:
            return append(context->om, kReportMap);
        case AccessTarget::kKeyboardReport:
            return append(context->om, kNeutralKeyboard);
        case AccessTarget::kMouseReport:
            return append(context->om, kNeutralMouse);
        case AccessTarget::kKeyboardReference:
            return append(context->om, kKeyboardReportReference);
        case AccessTarget::kMouseReference:
            return append(context->om, kMouseReportReference);
        case AccessTarget::kControlPoint: {
            if (context->op != BLE_GATT_ACCESS_OP_WRITE_CHR ||
                OS_MBUF_PKTLEN(context->om) != 1 || s_database == nullptr) {
                return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            }
            std::uint8_t value = 0xff;
            std::uint16_t length = 0;
            if (ble_hs_mbuf_to_flat(context->om, &value, sizeof(value), &length) != 0 ||
                length != 1 || value > 1) {
                return BLE_ATT_ERR_UNLIKELY;
            }
            s_database->suspended_.store(value == 0, std::memory_order_release);
            return 0;
        }
    }
    return BLE_ATT_ERR_UNLIKELY;
}

}  // namespace ble_hid_service
