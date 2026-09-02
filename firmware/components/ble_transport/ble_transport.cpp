#include "ble_transport/ble_transport.hpp"

#include <cstring>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs_flash.h"

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
    ble_hs_cfg.sm_bonding = 0;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 0;
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
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

}  // namespace ble_transport
