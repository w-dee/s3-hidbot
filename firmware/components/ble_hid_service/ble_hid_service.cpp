#include "ble_hid_service/ble_hid_service.hpp"

#include "host/ble_att.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_hs_mbuf.h"
#include "host/ble_uuid.h"
#include "os/os_mbuf.h"

namespace ble_hid_service {
namespace {

enum class AccessTarget : std::uint8_t {
    kSchemaEpoch,
    kInformation,
    kReportMap,
    kControlPoint,
    kKeyboardReport,
    kMouseReport,
    kKeyboardReference,
    kMouseReference,
};

// Project-owned internal UUIDs.  Canonical service UUID:
// 5f7d0a10-7e38-4ed1-b97b-1fa4e83c2a10
// Canonical characteristic UUID:
// 5f7d0a11-7e38-4ed1-b97b-1fa4e83c2a10
ble_uuid128_t s_schema_epoch_service = BLE_UUID128_INIT(
    0x10, 0x2a, 0x3c, 0xe8, 0xa4, 0x1f, 0x7b, 0xb9,
    0xd1, 0x4e, 0x38, 0x7e, 0x10, 0x0a, 0x7d, 0x5f);
ble_uuid128_t s_schema_epoch_characteristic = BLE_UUID128_INIT(
    0x10, 0x2a, 0x3c, 0xe8, 0xa4, 0x1f, 0x7b, 0xb9,
    0xd1, 0x4e, 0x38, 0x7e, 0x11, 0x0a, 0x7d, 0x5f);
ble_uuid16_t s_gatt_service = BLE_UUID16_INIT(0x1801);
ble_uuid16_t s_hid_service = BLE_UUID16_INIT(0x1812);
ble_uuid16_t s_hid_information = BLE_UUID16_INIT(0x2a4a);
ble_uuid16_t s_report_map = BLE_UUID16_INIT(0x2a4b);
ble_uuid16_t s_control_point = BLE_UUID16_INIT(0x2a4c);
ble_uuid16_t s_report = BLE_UUID16_INIT(0x2a4d);
ble_uuid16_t s_report_reference = BLE_UUID16_INIT(0x2908);

std::uint16_t s_keyboard_value_handle = 0;
std::uint16_t s_mouse_value_handle = 0;
std::uint16_t s_schema_epoch_value_handle = 0;
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

ble_gatt_chr_def s_schema_epoch_characteristics[] = {
    {.uuid = &s_schema_epoch_characteristic.u,
     .access_cb = Database::access,
     .arg = target(AccessTarget::kSchemaEpoch),
     .descriptors = nullptr,
     .flags = BLE_GATT_CHR_F_READ,
     .min_key_size = 0,
     .val_handle = &s_schema_epoch_value_handle,
     .cpfd = nullptr},
    {},
};

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
     .flags = BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_WRITE_AUTHEN,
     .min_key_size = kStrictProfile.attributes.key_size,
     .val_handle = &s_control_point_value_handle},
    {.uuid = &s_report.u,
     .access_cb = Database::access,
     .arg = target(AccessTarget::kKeyboardReport),
     .descriptors = s_keyboard_descriptors,
     .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_AUTHEN |
              BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_NOTIFY_INDICATE_AUTHEN |
              BLE_GATT_CHR_F_NOTIFY_INDICATE_AUTHOR,
     .min_key_size = kStrictProfile.attributes.key_size,
     .val_handle = &s_keyboard_value_handle},
    {.uuid = &s_report.u,
     .access_cb = Database::access,
     .arg = target(AccessTarget::kMouseReport),
     .descriptors = s_mouse_descriptors,
     .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_AUTHEN |
              BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_NOTIFY_INDICATE_AUTHEN |
              BLE_GATT_CHR_F_NOTIFY_INDICATE_AUTHOR,
     .min_key_size = kStrictProfile.attributes.key_size,
     .val_handle = &s_mouse_value_handle},
    {},
};

// Fixed storage for one reviewed mouse-only template. No public descriptor
// input is accepted, and this storage is rewritten only after a proven stop.
ble_gatt_chr_def s_mouse_characteristics[5]{};

ble_gatt_svc_def s_services[] = {
    {.type = BLE_GATT_SVC_TYPE_PRIMARY,
     .uuid = &s_schema_epoch_service.u,
     .includes = nullptr,
     .characteristics = s_schema_epoch_characteristics},
    {.type = BLE_GATT_SVC_TYPE_PRIMARY,
     .uuid = &s_hid_service.u,
     .characteristics = s_characteristics},
    {},
};

template <typename ByteRange>
int append(struct os_mbuf *buffer, const ByteRange &value) {
    return os_mbuf_append(buffer, value.data(), value.size()) == 0
               ? 0
               : BLE_ATT_ERR_INSUFFICIENT_RES;
}

}  // namespace

bool Database::configure_profile(ble_fixture_profile::ProfileId id) {
    if (registered_) return false;
    const auto *definition = ble_fixture_profile::find_definition(id);
    if (definition == nullptr) return false;
    profile_ = definition;
    return true;
}

void Database::reset_after_stop() {
    registered_ = false;
    s_keyboard_value_handle = s_mouse_value_handle = 0;
    s_schema_epoch_value_handle = s_information_value_handle = 0;
    s_report_map_value_handle = s_control_point_value_handle = 0;
    if (s_database == this) s_database = nullptr;
}

int Database::register_database() {
    if (s_database != nullptr && s_database != this) {
        return BLE_HS_EALREADY;
    }
    if (registered_) return BLE_HS_EALREADY;
    s_database = this;
    registered_ = true; // Even a partial registration requires proven teardown.
    s_services[1].characteristics = s_characteristics;
    if (profile_->gatt_template == ble_fixture_profile::GattTemplateId::kMouseOnly) {
        s_mouse_characteristics[0] = s_characteristics[0];
        s_mouse_characteristics[1] = s_characteristics[1];
        s_mouse_characteristics[2] = s_characteristics[2];
        s_mouse_characteristics[2].flags =
            BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_WRITE_ENC;
        s_mouse_characteristics[3] = s_characteristics[4];
        s_mouse_characteristics[3].flags =
            BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC |
            BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_NOTIFY_INDICATE_ENC |
            BLE_GATT_CHR_F_NOTIFY_INDICATE_AUTHOR;
        s_mouse_characteristics[2].min_key_size = profile_->attributes.key_size;
        s_mouse_characteristics[3].min_key_size = profile_->attributes.key_size;
        s_services[1].characteristics = s_mouse_characteristics;
    }
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
    std::uint16_t gatt_service_handle = 0;
    int result = ble_gatts_find_svc(&s_gatt_service.u, &gatt_service_handle);
    if (result != 0 ||
        gatt_service_handle != kGattServiceStartHandle) {
        return result != 0 ? result : BLE_HS_ENOENT;
    }
    std::uint16_t epoch_service_handle = 0;
    result = ble_gatts_find_svc(&s_schema_epoch_service.u,
                                &epoch_service_handle);
    if (result != 0 ||
        epoch_service_handle != kRevision1EpochServiceStartHandle) {
        return result != 0 ? result : BLE_HS_ENOENT;
    }
    std::uint16_t epoch_value_handle = 0;
    result = ble_gatts_find_chr(&s_schema_epoch_service.u,
                                &s_schema_epoch_characteristic.u, nullptr,
                                &epoch_value_handle);
    if (result != 0 || epoch_value_handle == 0 ||
        epoch_value_handle != s_schema_epoch_value_handle ||
        epoch_value_handle != kRevision1EpochServiceEndHandle) {
        return result != 0 ? result : BLE_HS_ENOENT;
    }
    std::uint16_t hid_service_handle = 0;
    result = ble_gatts_find_svc(&s_hid_service.u, &hid_service_handle);
    if (result != 0 ||
        hid_service_handle != kRevision1HidServiceStartHandle) {
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
    const auto &layout = profile_->layout;
    if ((layout.keyboard_value != 0 && s_keyboard_value_handle == 0) ||
        (layout.mouse_value != 0 && s_mouse_value_handle == 0) ||
        (s_keyboard_value_handle != 0 &&
         s_keyboard_value_handle == s_mouse_value_handle) ||
        s_report_map_value_handle != layout.report_map_value ||
        s_control_point_value_handle != layout.control_point_value ||
        s_keyboard_value_handle != layout.keyboard_value ||
        s_mouse_value_handle != layout.mouse_value) {
        return BLE_HS_ENOENT;
    }
    return 0;
}

void Database::bind_event_sink(hid_control_executor::BleEventSink *sink) {
    event_sink_ = sink;
}

void Database::set_generation(ble_lifecycle::Generation generation) {
    generation_.store(generation, std::memory_order_release);
}

hid_control_executor::BleHidHandles Database::hid_handles() const {
    return {
        .report_map_value = s_report_map_value_handle,
        .keyboard_value = s_keyboard_value_handle,
        .mouse_value = s_mouse_value_handle,
        .control_point_value = s_control_point_value_handle,
    };
}

hid_control_executor::BleNotifyBackendResult Database::notify_custom(
    std::uint16_t connection_handle, std::uint16_t characteristic_handle,
    const std::uint8_t *payload, std::uint16_t payload_length) {
    const bool keyboard = s_keyboard_value_handle != 0 &&
                          characteristic_handle == s_keyboard_value_handle &&
                          payload_length == kNeutralKeyboard.size();
    const bool mouse = s_mouse_value_handle != 0 &&
                       characteristic_handle == s_mouse_value_handle &&
                       payload_length == kNeutralMouse.size();
    if (payload == nullptr || (!keyboard && !mouse)) {
        return hid_control_executor::BleNotifyBackendResult::kStackRejected;
    }
    struct os_mbuf *buffer = ble_hs_mbuf_from_flat(payload, payload_length);
    if (buffer == nullptr) {
        return hid_control_executor::BleNotifyBackendResult::kResourceFailure;
    }
    // ble_gatts_notify_custom consumes buffer on every configured return path.
    // No caller-side free or retry is valid after this call.
    const int result = ble_gatts_notify_custom(
        connection_handle, characteristic_handle, buffer);
    if (result == 0) {
        return hid_control_executor::BleNotifyBackendResult::kStackAccepted;
    }
    return result == BLE_HS_ENOMEM
               ? hid_control_executor::BleNotifyBackendResult::kResourceFailure
               : hid_control_executor::BleNotifyBackendResult::kStackRejected;
}

bool Database::capture_control_point(std::uint16_t connection_handle,
                                     bool suspended) {
    return event_sink_ != nullptr && event_sink_->signal_ble_event({
        .kind = hid_control_executor::BleEventKind::kControlPoint,
        .generation = generation_.load(std::memory_order_acquire),
        .connection_handle = connection_handle,
        .attribute_handle = s_control_point_value_handle,
        .suspended = suspended,
    });
}

int Database::access(std::uint16_t connection_handle,
                     std::uint16_t attribute_handle,
                     struct ble_gatt_access_ctxt *context, void *argument) {
    if (context == nullptr || argument == nullptr || s_database == nullptr) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    switch (target_from(argument)) {
        case AccessTarget::kSchemaEpoch:
            return context->op == BLE_GATT_ACCESS_OP_READ_CHR
                       ? append(context->om, s_database->profile_->cache.schema_epoch_value)
                       : BLE_ATT_ERR_READ_NOT_PERMITTED;
        case AccessTarget::kInformation:
            return append(context->om, s_database->profile_->hid_information);
        case AccessTarget::kReportMap: {
            const auto &report_map = s_database->profile_->report_map;
            const int result = append(context->om, report_map);
            if (result == 0 && context->op == BLE_GATT_ACCESS_OP_READ_CHR &&
                s_database != nullptr && s_database->event_sink_ != nullptr) {
                (void)s_database->event_sink_->signal_ble_event({
                    .kind = hid_control_executor::BleEventKind::kReportMapRead,
                    .generation = s_database->generation_.load(
                        std::memory_order_acquire),
                    .connection_handle = connection_handle,
                    .attribute_handle = attribute_handle,
                });
            }
            return result;
        }
        case AccessTarget::kKeyboardReport:
            return s_keyboard_value_handle != 0
                ? append(context->om, kNeutralKeyboard) : BLE_ATT_ERR_UNLIKELY;
        case AccessTarget::kMouseReport:
            return append(context->om, kNeutralMouse);
        case AccessTarget::kKeyboardReference:
            return s_keyboard_value_handle != 0
                ? append(context->om, kKeyboardReportReference) : BLE_ATT_ERR_UNLIKELY;
        case AccessTarget::kMouseReference: {
            const auto *report = ble_fixture_profile::find_report(
                *s_database->profile_, ble_fixture_profile::ReportRole::kMouseInput);
            return report != nullptr ? append(context->om, report->report_reference)
                                     : BLE_ATT_ERR_UNLIKELY;
        }
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
            return s_database->capture_control_point(connection_handle,
                                                     value == 0)
                       ? 0
                       : BLE_ATT_ERR_INSUFFICIENT_RES;
        }
    }
    return BLE_ATT_ERR_UNLIKELY;
}

}  // namespace ble_hid_service
