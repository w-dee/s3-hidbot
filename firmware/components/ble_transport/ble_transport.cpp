#include "ble_transport/ble_transport.hpp"

#include <cstring>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/ble_sm.h"
#include "host/ble_store.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs_flash.h"
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
    if (database != nullptr) {
        const int database_result = database->register_database();
        if (database_result != 0) {
            instance_ = nullptr;
            sink_ = nullptr;
            return database_result;
        }
    }
    initialized_ = true;
    nimble_port_freertos_init(host_task);
    arm_timeout(kSyncTimeoutUs);
    return 0;
}

void Backend::set_generation(ble_lifecycle::Generation generation) {
    generation_.store(generation, std::memory_order_release);
}

void Backend::host_task(void *) { nimble_port_run(); }

void Backend::timeout_callback(void *context) {
    auto *backend = static_cast<Backend *>(context);
    if (backend != nullptr) {
        (void)backend->signal(hid_control_executor::BleEventKind::kTimeout,
                              ble_lifecycle::kNoConnection,
                              kLifecycleTimeoutError);
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

void Backend::arm_timeout(std::uint64_t microseconds) {
    cancel_timeout();
    (void)esp_timer_start_once(timeout_timer_, microseconds);
}

void Backend::cancel_timeout() {
    if (timeout_timer_ != nullptr && esp_timer_is_active(timeout_timer_)) {
        (void)esp_timer_stop(timeout_timer_);
    }
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
    instance_->cancel_timeout();
    int result = ble_hs_util_ensure_addr(0);
    if (result == 0) {
        result = ble_hs_id_infer_auto(0, &instance_->own_address_type_);
    }
    (void)instance_->signal(result == 0
                                ? hid_control_executor::BleEventKind::kSync
                                : hid_control_executor::BleEventKind::kTimeout,
                            ble_lifecycle::kNoConnection, result);
}

void Backend::on_reset(int reason) {
    if (instance_ != nullptr) {
        // Retire callback identity immediately. The serialized state owner
        // advances by the same single uint32 step when it consumes this event,
        // so a following sync callback already carries the new identity.
        const auto retired = instance_->generation_.fetch_add(
            1, std::memory_order_acq_rel);
        instance_->current_connection_.store(ble_lifecycle::kNoConnection,
                                             std::memory_order_release);
        instance_->identity_resolved_.store(false, std::memory_order_release);
        instance_->arm_timeout(kSyncTimeoutUs);
        if (instance_->sink_ != nullptr) {
            (void)instance_->sink_->signal_ble_event({
                .kind = hid_control_executor::BleEventKind::kReset,
                .generation = retired,
                .connection_handle = ble_lifecycle::kNoConnection,
                .status = reason,
            });
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
            backend->cancel_timeout();
            (void)backend->signal(hid_control_executor::BleEventKind::kDisconnect,
                                  event->disconnect.conn.conn_handle,
                                  event->disconnect.reason);
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
            if (backend->database_ != nullptr) {
                backend->database_->on_subscribe(event->subscribe.attr_handle,
                                                 event->subscribe.cur_notify != 0);
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
    const int result =
        ble_gap_terminate(connection_handle, BLE_ERR_REM_USER_CONN_TERM);
    if (result == 0) {
        arm_timeout(kDisconnectTimeoutUs);
    }
    return result;
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
    return ble_sm_inject_io(connection_handle, &input);
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
    security_.begin_connection(generation, connection_handle);
}

void Backend::retire_security(ble_lifecycle::Generation generation,
                              std::uint16_t connection_handle) {
    security_.retire_connection(generation, connection_handle);
    if (current_connection_.load(std::memory_order_acquire) ==
        connection_handle) {
        current_connection_.store(ble_lifecycle::kNoConnection,
                                  std::memory_order_release);
        identity_resolved_.store(false, std::memory_order_release);
    }
}

void Backend::mark_security_unhealthy(
    ble_lifecycle::Generation generation) {
    security_.mark_lifecycle_unhealthy(generation);
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
    return security_.snapshot();
}

bool Backend::security_ready_for_hid(
    ble_lifecycle::Generation generation,
    std::uint16_t connection_handle) const {
    return security_.security_ready_for_hid(generation, connection_handle);
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
    security_.observe_store_failure(
        generation_.load(std::memory_order_acquire),
        connection_handle, kind, status, persistent_store_unhealthy);
    (void)signal(persistent_store_unhealthy
                     ? hid_control_executor::BleEventKind::kStorageFailure
                     : hid_control_executor::BleEventKind::kStoreFull,
                 connection_handle, status);
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
    if (result != 0) {
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
