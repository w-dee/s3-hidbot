#include "ble_transport/ble_transport.hpp"

#include "store_delete_result.hpp"

#include <array>
#include <cstring>

#include "ble_hid_service/ble_hid_service.hpp"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/ble_sm.h"
#include "host/ble_store.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "mbedtls/md.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "secure_memory/secure_memory.hpp"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "store/config/ble_store_config.h"

extern "C" void ble_store_config_init(void);

namespace ble_transport {
namespace {
constexpr char kDeviceName[] = "s3-hidbot";
constexpr std::uint16_t kHidAppearance = 0x03c0;
constexpr std::uint16_t kAdvertisingInterval = 64;  // 40 ms in 0.625-ms units.
constexpr std::uint64_t kSyncTimeoutUs = 10'000'000;
constexpr std::uint64_t kDisconnectTimeoutUs = 5'000'000;
constexpr std::int32_t kLifecycleTimeoutError = -3;
constexpr std::uint16_t kConnectionIntervalMin = 12;  // 15 ms.
constexpr std::uint16_t kConnectionIntervalMax = 24;  // 30 ms.
constexpr std::uint16_t kSupervisionTimeout = 400;    // 4 s.
constexpr char kLogTag[] = "ble_transport";
ble_uuid16_t s_hid_service_uuid = BLE_UUID16_INIT(0x1812);
ble_uuid16_t s_gatt_service_uuid = BLE_UUID16_INIT(0x1801);
ble_uuid16_t s_service_changed_uuid = BLE_UUID16_INIT(0x2a05);
constexpr char kSchemaNamespace[] = "hid_schema";
constexpr char kSchemaKeyHex[] = "0123456789abcdef";
constexpr char kBondIdDomain[] = "s3-hidbot/bond-id/v1";

struct StoredPeer {
    ble_addr_t identity{};
    ble_security::StoredSecurityRecord our{};
    ble_security::StoredSecurityRecord peer{};
};

bool has_exact_identity(const ble_addr_t &identity);

bool same_identity(const ble_addr_t &left, const ble_addr_t &right) {
    return ble_addr_cmp(&left, &right) == 0;
}

bool valid_identity(const ble_addr_t &identity) {
    return (identity.type == BLE_ADDR_PUBLIC ||
            identity.type == BLE_ADDR_RANDOM) &&
           has_exact_identity(identity);
}

ble_security::StoredSecurityRecord security_record(
    const ble_addr_t &identity, int result,
    const ble_store_value_sec &value) {
    return {
        .found = result == 0,
        .identity_matches = result == 0 &&
                            same_identity(identity, value.peer_addr),
        .ltk_present = result == 0 && value.ltk_present != 0,
        .authenticated = result == 0 && value.authenticated != 0,
        .secure_connections = result == 0 && value.sc != 0,
        .key_size = result == 0 ? value.key_size : std::uint8_t{0},
    };
}

bool make_bond_id(const ble_addr_t &identity,
                  hid_control_executor::BondId &output) {
    std::array<std::uint8_t, sizeof(kBondIdDomain) - 1 + 1 + 6> input{};
    std::memcpy(input.data(), kBondIdDomain, sizeof(kBondIdDomain) - 1);
    input[sizeof(kBondIdDomain) - 1] = identity.type;
    std::memcpy(input.data() + sizeof(kBondIdDomain), identity.val,
                sizeof(identity.val));
    std::array<std::uint8_t, 32> digest{};
    const mbedtls_md_info_t *info =
        mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (info == nullptr ||
        mbedtls_md(info, input.data(), input.size(), digest.data()) != 0) {
        return false;
    }
    for (std::size_t index = 0;
         index < hid_control_executor::kBondIdHexChars / 2; ++index) {
        output[index * 2] = kSchemaKeyHex[digest[index] >> 4U];
        output[index * 2 + 1] = kSchemaKeyHex[digest[index] & 0x0fU];
    }
    output[hid_control_executor::kBondIdHexChars] = '\0';
    return true;
}

void schema_key(const ble_addr_t &identity, char (&key)[16]) {
    key[0] = 'r';
    key[1] = kSchemaKeyHex[identity.type >> 4U];
    key[2] = kSchemaKeyHex[identity.type & 0x0fU];
    for (std::size_t index = 0; index < sizeof(identity.val); ++index) {
        key[3 + index * 2] = kSchemaKeyHex[identity.val[index] >> 4U];
        key[4 + index * 2] = kSchemaKeyHex[identity.val[index] & 0x0fU];
    }
    key[15] = '\0';
}

esp_err_t read_schema_revision(const ble_addr_t &identity,
                               std::uint8_t &revision) {
    nvs_handle_t handle = 0;
    esp_err_t result = nvs_open(kSchemaNamespace, NVS_READONLY, &handle);
    if (result == ESP_OK) {
        char key[16]{};
        schema_key(identity, key);
        result = nvs_get_u8(handle, key, &revision);
        nvs_close(handle);
    }
    return result;
}

esp_err_t delete_schema_revision(const ble_addr_t &identity) {
    nvs_handle_t handle = 0;
    esp_err_t result = nvs_open(kSchemaNamespace, NVS_READWRITE, &handle);
    if (result == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (result == ESP_OK) {
        char key[16]{};
        schema_key(identity, key);
        result = nvs_erase_key(handle, key);
        if (result == ESP_ERR_NVS_NOT_FOUND) {
            result = ESP_OK;
        } else if (result == ESP_OK) {
            result = nvs_commit(handle);
        }
        nvs_close(handle);
    }
    return result;
}

bool peer_identity(std::uint16_t connection_handle, ble_addr_t &identity) {
    ble_gap_conn_desc descriptor{};
    if (ble_gap_conn_find(connection_handle, &descriptor) != 0) {
        return false;
    }
    identity = descriptor.peer_id_addr;
    return true;
}

bool has_exact_identity(const ble_addr_t &identity) {
    if (identity.type != 0) {
        return true;
    }
    for (const std::uint8_t byte : identity.val) {
        if (byte != 0) {
            return true;
        }
    }
    return false;
}
}  // namespace

Backend *Backend::instance_ = nullptr;

std::int32_t Backend::initialize(hid_control_executor::BleEventSink *sink,
                                 hid_control_executor::BleDatabase *database,
                                 ble_lifecycle::Generation generation) {
    if (initialized_) {
        return 0;
    }
    if (sink == nullptr || instance_ != nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    sink_ = sink;
    database_ = database;
    generation_.store(generation, std::memory_order_release);
    if (database_ != nullptr) {
        database_->bind_event_sink(sink_);
        database_->set_generation(generation);
    }
    instance_ = this;
    // Never erase NVS as an initialization fallback; unrelated data must not
    // be destroyed merely to expose BLE.
    esp_err_t result = nvs_flash_init();
    if (result != ESP_OK) {
        instance_ = nullptr;
        sink_ = nullptr;
        return result;
    }
    result = nimble_port_init();
    if (result != ESP_OK) {
        instance_ = nullptr;
        sink_ = nullptr;
        return result;
    }
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_KEYBOARD_ONLY;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_sc_only = 0;
    ble_hs_cfg.sm_sec_lvl = 3;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC;
    ble_hs_cfg.sm_their_key_dist =
        BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    // Install the supported NimBLE store, then interpose only enough to
    // observe failures. Every wrapper delegates to the saved implementation
    // exactly once and never examines or logs secret material.
    ble_store_config_init();
    original_store_read_ = ble_hs_cfg.store_read_cb;
    original_store_write_ = ble_hs_cfg.store_write_cb;
    original_store_delete_ = ble_hs_cfg.store_delete_cb;
    if (original_store_read_ == nullptr || original_store_write_ == nullptr ||
        original_store_delete_ == nullptr) {
        instance_ = nullptr;
        sink_ = nullptr;
        return ESP_ERR_INVALID_STATE;
    }
    ble_hs_cfg.store_read_cb = store_read;
    ble_hs_cfg.store_write_cb = store_write;
    ble_hs_cfg.store_delete_cb = store_delete;
    ble_hs_cfg.store_status_cb = store_status;
    ble_hs_cfg.store_status_arg = this;
    // Queue the mandatory standard server foundation before the project
    // service. ble_gatts_start() consumes all queued definitions when the
    // NimBLE host task starts.
    ble_svc_gap_init();
    ble_svc_gatt_init();
    result = ble_svc_gap_device_name_set(kDeviceName);
    if (result == 0) {
        result = ble_svc_gap_device_appearance_set(kHidAppearance);
    }
    if (result != 0) {
        instance_ = nullptr;
        sink_ = nullptr;
        return result;
    }
    const esp_timer_create_args_t timer_args{
        .callback = timeout_callback,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "ble_lifecycle",
        .skip_unhandled_events = true,
    };
    result = esp_timer_create(&timer_args, &timeout_timer_);
    if (result != ESP_OK) {
        return result;
    }
    const esp_timer_create_args_t pairing_timer_args{
        .callback = pairing_timeout_callback,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "ble_pairing",
        .skip_unhandled_events = true,
    };
    result = esp_timer_create(&pairing_timer_args, &pairing_timer_);
    if (result != ESP_OK) {
        return result;
    }
    const esp_timer_create_args_t route_release_timer_args{
        .callback = route_release_grace_callback,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "ble_route_release",
        .skip_unhandled_events = true,
    };
    result = esp_timer_create(&route_release_timer_args,
                              &route_release_timer_);
    if (result != ESP_OK) {
        return result;
    }
    if (database != nullptr) {
        const int database_result = database->register_database();
        if (database_result != 0) {
            instance_ = nullptr;
            sink_ = nullptr;
            return database_result;
        }
    }
    result = arm_timeout(kSyncTimeoutUs, LifecycleTimeoutPurpose::kSync);
    if (result != ESP_OK) {
        instance_ = nullptr;
        sink_ = nullptr;
        return result;
    }
    initialized_ = true;
    nimble_port_freertos_init(host_task);
    return 0;
}

void Backend::set_generation(ble_lifecycle::Generation generation) {
    generation_.store(generation, std::memory_order_release);
    if (database_ != nullptr) {
        database_->set_generation(generation);
    }
}

void Backend::host_task(void *) { nimble_port_run(); }

void Backend::timeout_callback(void *context) {
    auto *backend = static_cast<Backend *>(context);
    if (backend == nullptr) {
        return;
    }
    const auto purpose = backend->timeout_ownership_.begin_timeout();
    if (purpose != LifecycleTimeoutPurpose::kNone) {
        const bool published = backend->signal(
            hid_control_executor::BleEventKind::kTimeout,
            ble_lifecycle::kNoConnection, kLifecycleTimeoutError);
        if (!published && backend->sink_ != nullptr) {
            // A one-shot timer has no remaining fallback after this callback.
            // Preserve its terminal observation even if the backend generation
            // legitimately leads the executor across a Reset handoff.
            backend->sink_->signal_ble_lifecycle_handoff_failure();
        }
        // Retain the firing owner through durable event/fallback publication.
        // A different purpose cannot acquire this timer incarnation early.
        (void)backend->timeout_ownership_.complete_timeout(purpose);
    }
}

void Backend::pairing_timeout_callback(void *context) {
    auto *backend = static_cast<Backend *>(context);
    if (backend != nullptr && backend->sink_ != nullptr) {
        (void)backend->sink_->signal_ble_event({
            .kind = hid_control_executor::BleEventKind::kPairingTimeout,
            .generation = backend->pairing_timer_generation_.load(
                std::memory_order_acquire),
            .connection_handle = backend->pairing_timer_connection_.load(
                std::memory_order_acquire),
            .status = 0,
            .pairing_id = backend->pairing_timer_id_.load(
                std::memory_order_acquire),
        });
    }
}

std::int32_t Backend::arm_timeout(std::uint64_t microseconds,
                                  LifecycleTimeoutPurpose purpose) {
    // Same-purpose re-arm and cross-purpose replacement are both rejected.
    // The current owner remains intact and its caller retains forward progress.
    if (!timeout_ownership_.try_acquire(purpose)) {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t result = esp_timer_start_once(timeout_timer_, microseconds);
    if (result != ESP_OK) {
        (void)timeout_ownership_.release_after_arm_failure(purpose);
    }
    return result;
}

void Backend::cancel_timeout(LifecycleTimeoutPurpose purpose) {
    // Keep the exact owner in a cancelling phase until the physical timer has
    // stopped. A new purpose cannot reuse the handle in the CAS-to-stop gap.
    if (!timeout_ownership_.begin_cancel(purpose)) {
        return;
    }
    if (timeout_timer_ != nullptr && esp_timer_is_active(timeout_timer_)) {
        (void)esp_timer_stop(timeout_timer_);
    }
    (void)timeout_ownership_.complete_cancel(purpose);
}

bool Backend::signal(hid_control_executor::BleEventKind kind,
                     std::uint16_t connection_handle, std::int32_t status) {
    return sink_ != nullptr && sink_->signal_ble_event({
                                   .kind = kind,
                                   .generation = generation_.load(
                                       std::memory_order_acquire),
                                   .connection_handle = connection_handle,
                                   .status = status,
                               });
}

void Backend::on_sync() {
    if (instance_ == nullptr) {
        return;
    }
    int result = ble_hs_util_ensure_addr(0);
    if (result == 0) {
        result = ble_hs_id_infer_auto(0, &instance_->own_address_type_);
    }
    std::uint16_t service_changed_handle = 0;
    if (result == 0) {
        result = ble_gatts_find_chr(
            &s_gatt_service_uuid.u, &s_service_changed_uuid.u, nullptr,
            &service_changed_handle);
        if (result == 0 && service_changed_handle == 0) {
            result = BLE_HS_ENOENT;
        }
    }
    instance_->service_changed_value_handle_.store(
        result == 0 ? service_changed_handle : 0, std::memory_order_release);
    if (instance_->sink_ == nullptr) {
        return;
    }
    const bool published = instance_->signal(
        result == 0 ? hid_control_executor::BleEventKind::kSync
                    : hid_control_executor::BleEventKind::kTimeout,
        ble_lifecycle::kNoConnection, result);
    if (!published) {
        instance_->sink_->signal_ble_lifecycle_handoff_failure();
    }
    // The queued event or monotonic failure latch now owns progress. Never
    // destroy the timer fallback before one of those durable paths exists.
    instance_->cancel_timeout(LifecycleTimeoutPurpose::kSync);
}

void Backend::on_reset(int reason) {
    if (instance_ != nullptr) {
        // Retire callback identity immediately. The serialized state owner
        // advances by the same single uint32 step when it consumes this event,
        // so a following sync callback already carries the new identity.
        const auto retired = instance_->generation_.fetch_add(
            1, std::memory_order_acq_rel);
        if (instance_->database_ != nullptr) {
            instance_->database_->set_generation(retired + 1U);
        }
        instance_->current_connection_.store(ble_lifecycle::kNoConnection,
                                             std::memory_order_release);
        instance_->identity_resolved_.store(false, std::memory_order_release);
        instance_->gatt_schema_current_.store(false,
                                             std::memory_order_release);
        instance_->service_changed_value_handle_.store(
            0, std::memory_order_release);
        const std::int32_t timeout_result = instance_->arm_timeout(
            kSyncTimeoutUs, LifecycleTimeoutPurpose::kSync);
        if (instance_->sink_ != nullptr) {
            const bool published = instance_->sink_->signal_ble_event({
                .kind = hid_control_executor::BleEventKind::kReset,
                .generation = retired,
                .connection_handle = ble_lifecycle::kNoConnection,
                .status = reason,
            });
            if (!published || timeout_result != ESP_OK) {
                instance_->sink_->signal_ble_lifecycle_handoff_failure();
            }
        }
    }
}

int Backend::on_gap_event(struct ble_gap_event *event, void *context) {
    auto *backend = static_cast<Backend *>(context);
    if (backend == nullptr || event == nullptr) {
        return 0;
    }
    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            (void)backend->signal(
                event->connect.status == 0
                    ? hid_control_executor::BleEventKind::kConnect
                    : hid_control_executor::BleEventKind::kAdvertisingComplete,
                event->connect.conn_handle, event->connect.status);
            break;
        case BLE_GAP_EVENT_DISCONNECT:
            // Transfer progress ownership to the queued event before removing
            // the disconnect watchdog. If publication fails, leaving the
            // watchdog armed provides another bounded terminal observation.
            if (backend->signal(hid_control_executor::BleEventKind::kDisconnect,
                                event->disconnect.conn.conn_handle,
                                event->disconnect.reason)) {
                backend->cancel_timeout(
                    LifecycleTimeoutPurpose::kDisconnect);
            }
            break;
        case BLE_GAP_EVENT_ENC_CHANGE:
            (void)backend->signal(
                hid_control_executor::BleEventKind::kEncryptionChange,
                event->enc_change.conn_handle, event->enc_change.status);
            break;
        case BLE_GAP_EVENT_IDENTITY_RESOLVED:
            (void)backend->signal(
                hid_control_executor::BleEventKind::kIdentityResolved,
                event->identity_resolved.conn_handle, 0);
            break;
        case BLE_GAP_EVENT_PARING_COMPLETE:
            (void)backend->signal(
                hid_control_executor::BleEventKind::kPairingComplete,
                event->pairing_complete.conn_handle,
                event->pairing_complete.status);
            break;
        case BLE_GAP_EVENT_PASSKEY_ACTION:
            // The queue receives only the action classification; no passkey
            // or other secret is copied from or into callback state.
            (void)backend->signal(
                hid_control_executor::BleEventKind::kPasskeyAction,
                event->passkey.conn_handle,
                event->passkey.params.action == BLE_SM_IOACT_INPUT ? 1 : 0);
            break;
        case BLE_GAP_EVENT_REPEAT_PAIRING:
            (void)backend->signal(
                hid_control_executor::BleEventKind::kRepeatPairing,
                event->repeat_pairing.conn_handle, 0);
            return BLE_GAP_REPEAT_PAIRING_IGNORE;
        case BLE_GAP_EVENT_AUTHORIZE: {
            ble_gap_conn_desc descriptor{};
            const bool accepted =
                ble_gap_conn_find(event->authorize.conn_handle, &descriptor) ==
                    0 &&
                descriptor.sec_state.encrypted &&
                descriptor.sec_state.authenticated &&
                descriptor.sec_state.key_size ==
                    ble_security::kRequiredKeySize;
            event->authorize.out_response =
                accepted ? BLE_GAP_AUTHORIZE_ACCEPT
                         : BLE_GAP_AUTHORIZE_REJECT;
            break;
        }
        case BLE_GAP_EVENT_ADV_COMPLETE:
            (void)backend->signal(
                hid_control_executor::BleEventKind::kAdvertisingComplete,
                ble_lifecycle::kNoConnection, event->adv_complete.reason);
            break;
        case BLE_GAP_EVENT_SUBSCRIBE:
            if (backend->database_ != nullptr && backend->sink_ != nullptr) {
                hid_control_executor::BleSubscriptionReason reason =
                    hid_control_executor::BleSubscriptionReason::kUnknown;
                switch (event->subscribe.reason) {
                    case BLE_GAP_SUBSCRIBE_REASON_WRITE:
                        reason = hid_control_executor::BleSubscriptionReason::kWrite;
                        break;
                    case BLE_GAP_SUBSCRIBE_REASON_TERM:
                        reason = hid_control_executor::BleSubscriptionReason::kTerm;
                        break;
                    case BLE_GAP_SUBSCRIBE_REASON_RESTORE:
                        reason = hid_control_executor::BleSubscriptionReason::kRestore;
                        break;
                    default:
                        break;
                }
                const auto generation = backend->generation_.load(
                    std::memory_order_acquire);
                if (event->subscribe.attr_handle ==
                        backend->service_changed_value_handle_.load(
                            std::memory_order_acquire) &&
                    reason !=
                        hid_control_executor::BleSubscriptionReason::kUnknown) {
                    (void)backend->sink_->signal_ble_event({
                        .kind = hid_control_executor::BleEventKind::
                            kServiceChangedSubscription,
                        .generation = generation,
                        .connection_handle = event->subscribe.conn_handle,
                        .attribute_handle = event->subscribe.attr_handle,
                        .subscription_reason = reason,
                        .indicate_enabled =
                            event->subscribe.cur_indicate != 0,
                    });
                    break;
                }
                const auto handles = backend->database_->hid_handles();
                hid_control_executor::BleHidInterface interface =
                    hid_control_executor::BleHidInterface::kUnknown;
                if (event->subscribe.attr_handle == handles.keyboard_value) {
                    interface = hid_control_executor::BleHidInterface::kKeyboard;
                } else if (event->subscribe.attr_handle == handles.mouse_value) {
                    interface = hid_control_executor::BleHidInterface::kMouse;
                }
                if (interface != hid_control_executor::BleHidInterface::kUnknown &&
                    reason != hid_control_executor::BleSubscriptionReason::kUnknown) {
                    (void)backend->sink_->signal_ble_event({
                        .kind = hid_control_executor::BleEventKind::kSubscription,
                        .generation = generation,
                        .connection_handle = event->subscribe.conn_handle,
                        .attribute_handle = event->subscribe.attr_handle,
                        .hid_interface = interface,
                        .subscription_reason = reason,
                        .notify_enabled = event->subscribe.cur_notify != 0,
                    });
                }
            }
            break;
        default:
            break;
    }
    return 0;
}

std::int32_t Backend::start_advertising() {
    if (!initialized_ || database_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    const int database_result = database_->validate_registered_database();
    if (database_result != 0) {
        return database_result;
    }
    ble_hs_adv_fields fields{};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids16 = &s_hid_service_uuid;
    fields.num_uuids16 = 1;
    fields.uuids16_is_complete = 1;
    fields.appearance = kHidAppearance;
    fields.appearance_is_present = 1;
    fields.name = reinterpret_cast<std::uint8_t *>(const_cast<char *>(kDeviceName));
    fields.name_len = std::strlen(kDeviceName);
    fields.name_is_complete = 1;
    int result = ble_gap_adv_set_fields(&fields);
    if (result != 0) {
        return result;
    }
    ble_gap_adv_params parameters{};
    parameters.conn_mode = BLE_GAP_CONN_MODE_UND;
    parameters.disc_mode = BLE_GAP_DISC_MODE_GEN;
    parameters.itvl_min = kAdvertisingInterval;
    parameters.itvl_max = kAdvertisingInterval;
    return ble_gap_adv_start(own_address_type_, nullptr, BLE_HS_FOREVER, &parameters,
                             on_gap_event, this);
}

std::int32_t Backend::stop_advertising() { return ble_gap_adv_stop(); }

std::int32_t Backend::disconnect(std::uint16_t connection_handle) {
    // Establish the typed watchdog before NimBLE can expose completion on
    // another core.  A callback can therefore only cancel an already-owned
    // Disconnect watchdog, and this caller never arms one after initiation.
    const std::int32_t timeout_result = arm_timeout(
        kDisconnectTimeoutUs, LifecycleTimeoutPurpose::kDisconnect);
    if (timeout_result != ESP_OK) {
        return timeout_result;
    }
    const int terminate_result =
        ble_gap_terminate(connection_handle, BLE_ERR_REM_USER_CONN_TERM);
    if (terminate_result == 0 || terminate_result == BLE_HS_EALREADY) {
        // EALREADY specifically means this connection is already terminating;
        // its Disconnect callback is still the owner of forward progress.
        return 0;
    }
    // ENOTCONN and every other nonzero result initiated no new termination in
    // ESP-IDF v5.5.4 NimBLE.  Release only this exact purpose; a concurrent
    // Sync watchdog cannot be cancelled by this cleanup.
    cancel_timeout(LifecycleTimeoutPurpose::kDisconnect);
    return terminate_result;
}

bool Backend::security_teardown_already_disconnected(
    std::int32_t disconnect_result) const {
    return disconnect_result == BLE_HS_ENOTCONN;
}

std::int32_t Backend::arm_ble_route_release_grace(
    hid_control_executor::BleRouteReleaseIdentity identity) {
    if (route_release_timer_ == nullptr ||
        route_release_timer_active_.load(std::memory_order_acquire) ||
        esp_timer_is_active(route_release_timer_)) {
        return ESP_ERR_INVALID_STATE;
    }
    route_release_authority_epoch_.store(identity.authority_epoch,
                                         std::memory_order_relaxed);
    route_release_route_generation_.store(identity.route_generation,
                                          std::memory_order_relaxed);
    route_release_ble_generation_.store(identity.ble_generation,
                                        std::memory_order_relaxed);
    route_release_connection_.store(identity.connection_handle,
                                    std::memory_order_relaxed);
    route_release_keyboard_handle_.store(
        identity.keyboard_characteristic_handle, std::memory_order_relaxed);
    route_release_mouse_handle_.store(identity.mouse_characteristic_handle,
                                      std::memory_order_relaxed);
    route_release_epoch_.store(identity.release_epoch,
                               std::memory_order_relaxed);
    route_release_timer_active_.store(true, std::memory_order_release);
    const esp_err_t result = esp_timer_start_once(
        route_release_timer_,
        static_cast<std::uint64_t>(
            hid_control_executor::kBleRouteReleaseGraceMs) * 1000U);
    if (result != ESP_OK) {
        route_release_timer_active_.store(false, std::memory_order_release);
    }
    return result;
}

void Backend::cancel_ble_route_release_grace(
    hid_control_executor::BleRouteReleaseIdentity identity) {
    const bool exact =
        route_release_authority_epoch_.load(std::memory_order_acquire) ==
            identity.authority_epoch &&
        route_release_route_generation_.load(std::memory_order_acquire) ==
            identity.route_generation &&
        route_release_ble_generation_.load(std::memory_order_acquire) ==
            identity.ble_generation &&
        route_release_connection_.load(std::memory_order_acquire) ==
            identity.connection_handle &&
        route_release_keyboard_handle_.load(std::memory_order_acquire) ==
            identity.keyboard_characteristic_handle &&
        route_release_mouse_handle_.load(std::memory_order_acquire) ==
            identity.mouse_characteristic_handle &&
        route_release_epoch_.load(std::memory_order_acquire) ==
            identity.release_epoch;
    if (!exact ||
        !route_release_timer_active_.exchange(false,
                                              std::memory_order_acq_rel)) {
        return;
    }
    if (esp_timer_is_active(route_release_timer_)) {
        (void)esp_timer_stop(route_release_timer_);
    }
}

void Backend::route_release_grace_callback(void *context) {
    auto *backend = static_cast<Backend *>(context);
    if (backend == nullptr ||
        !backend->route_release_timer_active_.exchange(
            false, std::memory_order_acq_rel) ||
        backend->sink_ == nullptr) {
        return;
    }
    (void)backend->sink_->signal_ble_route_release_grace({
        .authority_epoch = backend->route_release_authority_epoch_.load(
            std::memory_order_acquire),
        .route_generation = backend->route_release_route_generation_.load(
            std::memory_order_acquire),
        .ble_generation = backend->route_release_ble_generation_.load(
            std::memory_order_acquire),
        .connection_handle = backend->route_release_connection_.load(
            std::memory_order_acquire),
        .keyboard_characteristic_handle =
            backend->route_release_keyboard_handle_.load(
                std::memory_order_acquire),
        .mouse_characteristic_handle =
            backend->route_release_mouse_handle_.load(
                std::memory_order_acquire),
        .release_epoch = backend->route_release_epoch_.load(
            std::memory_order_acquire),
    });
}

std::int32_t Backend::terminate_orphan_connection(
    std::uint16_t connection_handle) {
    // NimBLE invokes GAP callbacks without the host mutex held. This call
    // issues the HCI Disconnect request without synchronously delivering the
    // later application Disconnect event. With
    // CONFIG_BT_NIMBLE_MAX_CONNECTIONS=1, the request occurs before any later
    // connection can reuse the callback-provided handle.
    return ble_gap_terminate(connection_handle, BLE_ERR_REM_USER_CONN_TERM);
}

std::int32_t Backend::configure_connection(std::uint16_t connection_handle) {
    ble_gap_upd_params parameters{};
    parameters.itvl_min = kConnectionIntervalMin;
    parameters.itvl_max = kConnectionIntervalMax;
    parameters.latency = 0;
    parameters.supervision_timeout = kSupervisionTimeout;
    parameters.min_ce_len = 0;
    parameters.max_ce_len = 0;
    return ble_gap_update_params(connection_handle, &parameters);
}

std::int32_t Backend::initiate_security(std::uint16_t connection_handle) {
    return ble_gap_security_initiate(connection_handle);
}

std::int32_t Backend::inject_passkey(std::uint16_t connection_handle,
                                     std::uint32_t passkey) {
    ble_sm_io input{};
    input.action = BLE_SM_IOACT_INPUT;
    input.passkey = passkey;
    const std::int32_t result = ble_sm_inject_io(connection_handle, &input);
    secure_memory::zero(&input, sizeof(input));
    secure_memory::zero(&passkey, sizeof(passkey));
    return result;
}

std::uint64_t Backend::monotonic_time_us() const {
    return static_cast<std::uint64_t>(esp_timer_get_time());
}

void Backend::arm_pairing_timeout(ble_lifecycle::Generation generation,
                                  std::uint16_t connection_handle,
                                  std::uint32_t pairing_id) {
    cancel_pairing_timeout();
    pairing_timer_generation_.store(generation, std::memory_order_relaxed);
    pairing_timer_connection_.store(connection_handle,
                                    std::memory_order_relaxed);
    pairing_timer_id_.store(pairing_id, std::memory_order_release);
    (void)esp_timer_start_once(pairing_timer_,
                               static_cast<std::uint64_t>(
                                   ble_pairing::kInputTimeoutMs) * 1000U);
}

void Backend::cancel_pairing_timeout() {
    if (pairing_timer_ != nullptr && esp_timer_is_active(pairing_timer_)) {
        (void)esp_timer_stop(pairing_timer_);
    }
    pairing_timer_id_.store(0, std::memory_order_release);
}

void Backend::begin_security(ble_lifecycle::Generation generation,
                             std::uint16_t connection_handle) {
    current_connection_.store(connection_handle, std::memory_order_release);
    identity_resolved_.store(false, std::memory_order_release);
    gatt_schema_current_.store(false, std::memory_order_release);
    security_inhibit_.begin_connection(generation, connection_handle);
    security_.begin_connection(generation, connection_handle);
}

void Backend::retire_security(ble_lifecycle::Generation generation,
                              std::uint16_t connection_handle) {
    security_inhibit_.retire_connection(generation, connection_handle);
    security_.retire_connection(generation, connection_handle);
    if (current_connection_.load(std::memory_order_acquire) ==
        connection_handle) {
        current_connection_.store(ble_lifecycle::kNoConnection,
                                  std::memory_order_release);
        identity_resolved_.store(false, std::memory_order_release);
        gatt_schema_current_.store(false, std::memory_order_release);
    }
}

void Backend::mark_security_unhealthy(
    ble_lifecycle::Generation generation) {
    security_.mark_lifecycle_unhealthy(generation);
}

void Backend::apply_store_failure(
    ble_lifecycle::Generation generation,
    std::uint16_t connection_handle,
    ble_security::StoreFailureKind kind, std::int32_t status) {
    security_.apply_store_failure(generation, connection_handle, kind, status);
}

void Backend::apply_persistent_store_failure(
    ble_security::StoreFailureKind kind, std::int32_t status) {
    security_.apply_persistent_store_failure(kind, status);
}

bool Backend::persistent_store_failure_observed() const {
    return security_inhibit_.persistent_failure_observed();
}

void Backend::record_heap_checkpoint(HeapCheckpoint checkpoint) {
    static constexpr const char *kLabels[] = {
        "cold-boot", "before-first-enable", "advertising",
        "connected", "readvertising", "hidden-idle",
    };
    constexpr std::uint32_t capabilities = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    ESP_LOGI(kLogTag, "heap checkpoint=%s free=%u minimum=%u largest=%u",
             kLabels[static_cast<std::size_t>(checkpoint)],
             static_cast<unsigned>(heap_caps_get_free_size(capabilities)),
             static_cast<unsigned>(heap_caps_get_minimum_free_size(capabilities)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(capabilities)));
}

ble_security::Snapshot Backend::security_snapshot() const {
    auto snapshot = security_.snapshot();
    if (snapshot.coherent && security_inhibit_.inhibits(
                                 snapshot.generation,
                                 snapshot.connection_handle)) {
        snapshot.project_verified_bond_persisted = false;
        snapshot.store_healthy = false;
    }
    return snapshot;
}

bool Backend::security_ready_for_hid(
    ble_lifecycle::Generation generation,
    std::uint16_t connection_handle) const {
    return !security_inhibit_.inhibits(generation, connection_handle) &&
           security_.security_ready_for_hid(generation, connection_handle);
}

hid_control_executor::GattSchemaStoreResult Backend::gatt_schema_status(
    ble_lifecycle::Generation generation,
    std::uint16_t connection_handle) {
    using Kind = hid_control_executor::GattSchemaStoreResultKind;
    if (generation_.load(std::memory_order_acquire) != generation ||
        current_connection_.load(std::memory_order_acquire) !=
            connection_handle ||
        !security_ready_for_hid(generation, connection_handle)) {
        return {.kind = Kind::kStaleIdentity};
    }
    ble_addr_t identity{};
    if (!peer_identity(connection_handle, identity)) {
        return {.kind = Kind::kStaleIdentity};
    }
    std::uint8_t revision = 0;
    const esp_err_t result = read_schema_revision(identity, revision);
    if (result == ESP_ERR_NVS_NOT_FOUND) {
        return {.kind = Kind::kStale};
    }
    if (result != ESP_OK) {
        return {.kind = Kind::kStorageFailure, .status = result};
    }
    if (revision != ble_hid_service::kGattSchemaRevision) {
        return {.kind = Kind::kStale};
    }
    gatt_schema_current_.store(true, std::memory_order_release);
    return {.kind = Kind::kCurrent};
}

hid_control_executor::GattSchemaStoreResult
Backend::persist_gatt_schema_current(
    ble_lifecycle::Generation generation,
    std::uint16_t connection_handle) {
    using Kind = hid_control_executor::GattSchemaStoreResultKind;
    if (generation_.load(std::memory_order_acquire) != generation ||
        current_connection_.load(std::memory_order_acquire) !=
            connection_handle ||
        !security_ready_for_hid(generation, connection_handle)) {
        return {.kind = Kind::kStaleIdentity};
    }
    ble_addr_t identity{};
    if (!peer_identity(connection_handle, identity)) {
        return {.kind = Kind::kStaleIdentity};
    }
    char key[16]{};
    schema_key(identity, key);
    nvs_handle_t handle = 0;
    esp_err_t result = nvs_open(kSchemaNamespace, NVS_READWRITE, &handle);
    if (result == ESP_OK) {
        result = nvs_set_u8(handle, key,
                            ble_hid_service::kGattSchemaRevision);
    }
    if (result == ESP_OK) {
        result = nvs_commit(handle);
    }
    if (handle != 0) {
        nvs_close(handle);
    }
    if (result == ESP_ERR_NVS_NOT_ENOUGH_SPACE) {
        return {.kind = Kind::kCapacityFull, .status = result};
    }
    if (result != ESP_OK) {
        return {.kind = Kind::kStorageFailure, .status = result};
    }
    std::uint8_t verified = 0;
    result = read_schema_revision(identity, verified);
    if (result != ESP_OK ||
        verified != ble_hid_service::kGattSchemaRevision) {
        return {.kind = Kind::kStorageFailure,
                .status = result != ESP_OK ? result : ESP_ERR_INVALID_STATE};
    }
    gatt_schema_current_.store(true, std::memory_order_release);
    return {.kind = Kind::kCurrent};
}

bool Backend::gatt_schema_current_for_hid(
    ble_lifecycle::Generation generation,
    std::uint16_t connection_handle) const {
    return generation_.load(std::memory_order_acquire) == generation &&
           current_connection_.load(std::memory_order_acquire) ==
               connection_handle &&
           gatt_schema_current_.load(std::memory_order_acquire);
}

std::uint16_t Backend::service_changed_value_handle() const {
    return service_changed_value_handle_.load(std::memory_order_acquire);
}

std::int32_t Backend::request_gatt_cache_refresh(
    ble_lifecycle::Generation generation, std::uint16_t connection_handle,
    std::uint16_t start_handle, std::uint16_t end_handle) {
    if (generation_.load(std::memory_order_acquire) != generation ||
        current_connection_.load(std::memory_order_acquire) !=
            connection_handle ||
        service_changed_value_handle() == 0 ||
        !security_ready_for_hid(generation, connection_handle) ||
        start_handle == 0 || start_handle > end_handle) {
        return ESP_ERR_INVALID_STATE;
    }
    ble_svc_gatt_changed(start_handle, end_handle);
    return 0;
}

hid_control_executor::BleBondListResult Backend::list_bonds() {
    using Result = hid_control_executor::BleBondListResult;
    using Kind = hid_control_executor::BleBondListResultKind;
    constexpr std::size_t kDetectionCapacity =
        ble_security::kBondCapacity + 1;
    if (!initialized_) {
        return {};
    }
    if (persistent_store_failure_observed()) {
        return {.kind = Kind::kStorageFailure};
    }

    std::array<StoredPeer, kDetectionCapacity> peers{};
    std::size_t peer_count = 0;
    const auto fail_storage = [this](std::int32_t status) {
        observe_store_failure(ble_security::StoreFailureKind::kRead, status,
                              true, ble_lifecycle::kNoConnection);
        return Result{.kind = Kind::kStorageFailure};
    };
    const auto collect = [&](bool our) -> std::int32_t {
        for (std::uint8_t index = 0; index < kDetectionCapacity; ++index) {
            ble_store_key_sec key{};
            key.idx = index;
            ble_store_value_sec value{};
            const int result = our ? ble_store_read_our_sec(&key, &value)
                                   : ble_store_read_peer_sec(&key, &value);
            if (result == BLE_HS_ENOENT) {
                return 0;
            }
            if (result != 0 || !valid_identity(value.peer_addr)) {
                return result != 0 ? result : BLE_HS_EINVAL;
            }
            std::size_t peer_index = 0;
            while (peer_index < peer_count &&
                   !same_identity(peers[peer_index].identity,
                                  value.peer_addr)) {
                ++peer_index;
            }
            if (peer_index == peer_count) {
                if (peer_count == peers.size()) {
                    return BLE_HS_ENOMEM;
                }
                peers[peer_count++].identity = value.peer_addr;
            } else if ((our && peers[peer_index].our.found) ||
                       (!our && peers[peer_index].peer.found)) {
                return BLE_HS_ESTORE_FAIL;
            }
            auto &record = our ? peers[peer_index].our
                               : peers[peer_index].peer;
            record = security_record(peers[peer_index].identity, result,
                                     value);
        }
        return BLE_HS_ENOMEM;
    };

    std::int32_t status = collect(true);
    if (status == 0) {
        status = collect(false);
    }
    if (status != 0 || peer_count > ble_security::kBondCapacity) {
        return fail_storage(status != 0 ? status : BLE_HS_ENOMEM);
    }

    Result result{.kind = Kind::kSuccess, .healthy = true};
    ble_addr_t connected_identity{};
    const std::uint16_t connection =
        current_connection_.load(std::memory_order_acquire);
    const bool connected_identity_valid =
        connection != ble_lifecycle::kNoConnection &&
        peer_identity(connection, connected_identity);
    for (std::size_t index = 0; index < peer_count; ++index) {
        auto &output = result.bonds[index];
        const auto &peer = peers[index];
        if (!make_bond_id(peer.identity, output.bond_id)) {
            return fail_storage(BLE_HS_ESTORE_FAIL);
        }
        output.our_sec = peer.our.found;
        output.peer_sec = peer.peer.found;
        output.verified = ble_security::State::persisted_bond_is_valid(
            {.our = peer.our, .peer = peer.peer});
        std::uint8_t revision = 0;
        const esp_err_t schema_result =
            read_schema_revision(peer.identity, revision);
        if (schema_result == ESP_OK) {
            output.schema_revision_present = true;
            output.schema_revision = revision;
            output.schema_current =
                revision == ble_hid_service::kGattSchemaRevision;
        } else if (schema_result != ESP_ERR_NVS_NOT_FOUND) {
            return fail_storage(schema_result);
        }
        output.connected = connected_identity_valid &&
                           same_identity(peer.identity, connected_identity);
        result.healthy = result.healthy && output.verified;
    }
    result.count = static_cast<std::uint8_t>(peer_count);
    result.available = static_cast<std::uint8_t>(
        ble_security::kBondCapacity - peer_count);

    for (std::size_t left = 0; left < peer_count; ++left) {
        for (std::size_t right = left + 1; right < peer_count; ++right) {
            const int order = std::strcmp(result.bonds[left].bond_id.data(),
                                          result.bonds[right].bond_id.data());
            if (order == 0) {
                result.healthy = false;
            } else if (order > 0) {
                const auto temporary = result.bonds[left];
                result.bonds[left] = result.bonds[right];
                result.bonds[right] = temporary;
            }
        }
    }
    return result;
}

hid_control_executor::BleBondRemoveResult Backend::remove_bond(
    const hid_control_executor::BondId &bond_id) {
    using Result = hid_control_executor::BleBondRemoveResult;
    using Kind = hid_control_executor::BleBondRemoveResultKind;
    Result result{.kind = Kind::kNotFound, .bond_id = bond_id};
    const auto before = list_bonds();
    if (before.kind ==
        hid_control_executor::BleBondListResultKind::kNotReady) {
        result.kind = Kind::kNotReady;
        return result;
    }
    if (before.kind ==
        hid_control_executor::BleBondListResultKind::kStorageFailure) {
        result.kind = Kind::kStorageFailure;
        return result;
    }
    std::size_t match_count = 0;
    std::size_t match_index = 0;
    for (std::size_t index = 0; index < before.count; ++index) {
        if (std::strcmp(before.bonds[index].bond_id.data(), bond_id.data()) ==
            0) {
            match_index = index;
            ++match_count;
        }
    }
    if (match_count == 0) {
        return result;
    }
    if (match_count != 1) {
        result.kind = Kind::kAmbiguous;
        return result;
    }
    if (!before.healthy) {
        result.kind = Kind::kStorageFailure;
        return result;
    }
    if (before.bonds[match_index].connected) {
        result.kind = Kind::kBusy;
        return result;
    }

    std::array<ble_addr_t, ble_security::kBondCapacity> identities{};
    int identity_count = 0;
    int status = ble_store_util_bonded_peers(
        identities.data(), &identity_count,
        static_cast<int>(identities.size()));
    ble_addr_t target{};
    std::size_t exact_identity_matches = 0;
    for (int index = 0; status == 0 && index < identity_count; ++index) {
        hid_control_executor::BondId candidate{};
        if (!make_bond_id(identities[static_cast<std::size_t>(index)],
                          candidate)) {
            status = BLE_HS_ESTORE_FAIL;
            break;
        }
        if (std::strcmp(candidate.data(), bond_id.data()) == 0) {
            target = identities[static_cast<std::size_t>(index)];
            ++exact_identity_matches;
        }
    }
    if (status != 0 || exact_identity_matches != 1) {
        if (status != 0) {
            observe_store_failure(ble_security::StoreFailureKind::kRead,
                                  status, true,
                                  ble_lifecycle::kNoConnection);
            result.kind = Kind::kStorageFailure;
        } else {
            result.kind = exact_identity_matches == 0 ? Kind::kNotFound
                                                       : Kind::kAmbiguous;
        }
        return result;
    }

    status = ble_store_util_delete_peer(&target);
    if (status != 0) {
        observe_store_failure(ble_security::StoreFailureKind::kDelete, status,
                              true, ble_lifecycle::kNoConnection);
        result.kind = Kind::kStorageFailure;
        return result;
    }

    ble_store_key_sec key{};
    key.peer_addr = target;
    ble_store_value_sec value{};
    const int our_status = ble_store_read_our_sec(&key, &value);
    const int peer_status = ble_store_read_peer_sec(&key, &value);
    std::uint8_t revision = 0;
    const esp_err_t schema_status = read_schema_revision(target, revision);
    const auto after = list_bonds();
    bool others_preserved =
        after.kind == hid_control_executor::BleBondListResultKind::kSuccess &&
        after.healthy && after.count + 1U == before.count;
    for (std::size_t old_index = 0;
         others_preserved && old_index < before.count; ++old_index) {
        if (old_index == match_index) {
            continue;
        }
        bool found = false;
        for (std::size_t new_index = 0; new_index < after.count; ++new_index) {
            const auto &old_bond = before.bonds[old_index];
            const auto &new_bond = after.bonds[new_index];
            found = found ||
                (std::strcmp(old_bond.bond_id.data(),
                             new_bond.bond_id.data()) == 0 &&
                 old_bond.our_sec == new_bond.our_sec &&
                 old_bond.peer_sec == new_bond.peer_sec &&
                 old_bond.verified == new_bond.verified &&
                 old_bond.schema_revision_present ==
                     new_bond.schema_revision_present &&
                 old_bond.schema_revision == new_bond.schema_revision &&
                 old_bond.schema_current == new_bond.schema_current &&
                 old_bond.connected == new_bond.connected);
        }
        others_preserved = found;
    }
    if (our_status != BLE_HS_ENOENT || peer_status != BLE_HS_ENOENT ||
        schema_status != ESP_ERR_NVS_NOT_FOUND || !others_preserved) {
        const std::int32_t failure =
            our_status != BLE_HS_ENOENT ? our_status
            : peer_status != BLE_HS_ENOENT ? peer_status
            : schema_status != ESP_ERR_NVS_NOT_FOUND ? schema_status
                                                     : BLE_HS_ESTORE_FAIL;
        observe_store_failure(ble_security::StoreFailureKind::kDelete,
                              failure, true,
                              ble_lifecycle::kNoConnection);
        result.kind = Kind::kStorageFailure;
        return result;
    }
    result.kind = Kind::kSuccess;
    result.remaining = after.count;
    return result;
}

void Backend::refresh_security(std::uint16_t connection_handle,
                               bool identity_resolved_event) {
    const auto generation = generation_.load(std::memory_order_acquire);
    if (current_connection_.load(std::memory_order_acquire) !=
        connection_handle) {
        return;
    }

    ble_gap_conn_desc descriptor{};
    if (ble_gap_conn_find(connection_handle, &descriptor) != 0) {
        return;
    }
    if (identity_resolved_event) {
        identity_resolved_.store(true, std::memory_order_release);
    }
    const bool ota_is_rpa = descriptor.peer_ota_addr.type == BLE_ADDR_RANDOM &&
                            (descriptor.peer_ota_addr.val[5] & 0xc0U) == 0x40U;
    const bool identity_resolved =
        identity_resolved_.load(std::memory_order_acquire) || !ota_is_rpa ||
        ble_addr_cmp(&descriptor.peer_id_addr, &descriptor.peer_ota_addr) != 0;

    ble_store_key_sec key{};
    key.peer_addr = descriptor.peer_id_addr;
    ble_store_value_sec our{};
    ble_store_value_sec peer{};
    const int our_result = ble_store_read_our_sec(&key, &our);
    const int peer_result = ble_store_read_peer_sec(&key, &peer);
    const auto record = [&key](int result,
                               const ble_store_value_sec &value) {
        return ble_security::StoredSecurityRecord{
            .found = result == 0,
            .identity_matches =
                result == 0 && ble_addr_cmp(&key.peer_addr, &value.peer_addr) == 0,
            .ltk_present = result == 0 && value.ltk_present != 0,
            .authenticated = result == 0 && value.authenticated != 0,
            .secure_connections = result == 0 && value.sc != 0,
            .key_size = result == 0 ? value.key_size : std::uint8_t{0},
        };
    };
    security_.apply_verification(
        generation, connection_handle,
        {.encrypted = descriptor.sec_state.encrypted != 0,
         .authenticated = descriptor.sec_state.authenticated != 0,
         .nimble_bonded = descriptor.sec_state.bonded != 0,
         .secure_connections =
             our_result == 0 && peer_result == 0 && our.sc != 0 && peer.sc != 0,
         .identity_resolved = identity_resolved,
         .key_size = static_cast<std::uint8_t>(descriptor.sec_state.key_size)},
        {.our = record(our_result, our), .peer = record(peer_result, peer)});
}

void Backend::observe_store_failure(ble_security::StoreFailureKind kind,
                                    std::int32_t status,
                                    bool persistent_store_unhealthy,
                                    std::uint16_t connection_handle) {
    const auto generation = generation_.load(std::memory_order_acquire);
    (void)security_inhibit_.inhibit(generation, connection_handle,
                                    persistent_store_unhealthy);
    if (sink_ != nullptr) {
        (void)sink_->signal_ble_event({
            .kind = persistent_store_unhealthy
                        ? hid_control_executor::BleEventKind::kStorageFailure
                        : hid_control_executor::BleEventKind::kStoreFull,
            .generation = generation,
            .connection_handle = connection_handle,
            .status = status,
            .store_failure_kind = kind,
        });
    }
}

int Backend::store_read(int object_type, const union ble_store_key *key,
                        union ble_store_value *value) {
    if (instance_ == nullptr || instance_->original_store_read_ == nullptr) {
        return BLE_HS_EINVAL;
    }
    return instance_->original_store_read_(object_type, key, value);
}

int Backend::store_write(int object_type,
                         const union ble_store_value *value) {
    if (instance_ == nullptr || instance_->original_store_write_ == nullptr) {
        return BLE_HS_EINVAL;
    }
    const int result = instance_->original_store_write_(object_type, value);
    if (result != 0) {
        instance_->observe_store_failure(
            result == BLE_HS_ESTORE_CAP
                ? ble_security::StoreFailureKind::kCapacityFull
                : ble_security::StoreFailureKind::kWrite,
            result, result != BLE_HS_ESTORE_CAP,
            instance_->current_connection_.load(std::memory_order_acquire));
    }
    return result;
}

int Backend::store_delete(int object_type, const union ble_store_key *key) {
    if (instance_ == nullptr || instance_->original_store_delete_ == nullptr) {
        return BLE_HS_EINVAL;
    }
    const int result = instance_->original_store_delete_(object_type, key);
    const auto delete_result =
        detail::classify_store_delete_callback_result(result,
                                                      BLE_HS_ENOENT);
    if (delete_result == detail::StoreDeleteCallbackResult::kDeleted &&
        key != nullptr &&
        (object_type == BLE_STORE_OBJ_TYPE_OUR_SEC ||
         object_type == BLE_STORE_OBJ_TYPE_PEER_SEC) &&
        has_exact_identity(key->sec.peer_addr)) {
        const esp_err_t schema_result =
            delete_schema_revision(key->sec.peer_addr);
        if (schema_result != ESP_OK) {
            instance_->observe_store_failure(
                ble_security::StoreFailureKind::kDelete, schema_result, true,
                instance_->current_connection_.load(
                    std::memory_order_acquire));
            return BLE_HS_ESTORE_FAIL;
        }
    }
    if (delete_result == detail::StoreDeleteCallbackResult::kFailure) {
        instance_->observe_store_failure(
            ble_security::StoreFailureKind::kDelete, result, true,
            instance_->current_connection_.load(std::memory_order_acquire));
    }
    return result;
}

int Backend::store_status(struct ble_store_status_event *event, void *argument) {
    auto *backend = static_cast<Backend *>(argument);
    if (backend == nullptr || event == nullptr) {
        return BLE_HS_ESTORE_CAP;
    }
    switch (event->event_code) {
        case BLE_STORE_EVENT_FULL:
            backend->observe_store_failure(
                ble_security::StoreFailureKind::kCapacityFull,
                BLE_HS_ESTORE_CAP, false, event->full.conn_handle);
            break;
        case BLE_STORE_EVENT_OVERFLOW:
            backend->observe_store_failure(
                ble_security::StoreFailureKind::kCapacityFull,
                BLE_HS_ESTORE_CAP, false,
                backend->current_connection_.load(std::memory_order_acquire));
            break;
        default:
            break;
    }
    // A nonzero response tells NimBLE that no room was made. Never invoke the
    // round-robin helper and never evict an existing bond.
    return BLE_HS_ESTORE_CAP;
}

}  // namespace ble_transport
