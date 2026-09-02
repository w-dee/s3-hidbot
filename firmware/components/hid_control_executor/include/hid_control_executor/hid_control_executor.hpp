#pragma once

#include <atomic>
#include <cstdint>

#include "hid_route/hid_route.hpp"
#include "hid_runtime/hid_runtime.hpp"
#include "ble_lifecycle/ble_lifecycle.hpp"
#include "usb_lifecycle/usb_lifecycle.hpp"

namespace hid_control_executor {

enum class ControlOperation : std::uint8_t {
    kNone,
    kUsbAttach,
    kUsbDetach,
    kRouteChange,
    kBleEnable,
    kBleDisable,
};

enum class BackendResultKind : std::uint8_t {
    kSuccess,
    kCleanInstallFailure,
    kAmbiguousInstallFailure,
    kUninstallFailure,
};

struct BackendResult {
    BackendResultKind kind = BackendResultKind::kAmbiguousInstallFailure;
    std::int32_t error_code = 0;
};

// Only Controller's dedicated control task invokes this backend. The
// concrete firmware backend owns the public esp_tinyusb calls; native tests
// use a deterministic fake and never link TinyUSB.
class Backend {
  public:
    virtual ~Backend() = default;
    virtual BackendResult install() = 0;
    virtual BackendResult uninstall() = 0;
};

enum class BleEventKind : std::uint8_t {
    kSync,
    kConnect,
    kDisconnect,
    kAdvertisingComplete,
    kReset,
    kTimeout,
};

struct BleEvent {
    BleEventKind kind = BleEventKind::kSync;
    ble_lifecycle::Generation generation = 0;
    std::uint16_t connection_handle = ble_lifecycle::kNoConnection;
    std::int32_t status = 0;
};

class BleEventSink {
  public:
    virtual ~BleEventSink() = default;
    // Callback-safe: implementations must use bounded, zero-wait signaling.
    virtual bool signal_ble_event(BleEvent event) = 0;
};

class BleDatabase {
  public:
    virtual ~BleDatabase() = default;
    virtual int register_database() = 0;
    // Called only after the NimBLE GATT server has started. A zero result is
    // required before any project HID advertisement may become visible.
    virtual int validate_registered_database() = 0;
    virtual void clear_peer_state() = 0;
    virtual void on_subscribe(std::uint16_t attribute_handle, bool enabled) = 0;
};

class BleBackend {
  public:
    enum class HeapCheckpoint : std::uint8_t {
        kColdBoot,
        kBeforeFirstEnable,
        kAdvertising,
        kConnected,
        kReadvertising,
        kHiddenIdle,
    };

    virtual ~BleBackend() = default;
    virtual std::int32_t initialize(BleEventSink *sink, BleDatabase *database,
                                    ble_lifecycle::Generation generation) = 0;
    virtual void set_generation(ble_lifecycle::Generation generation) = 0;
    virtual std::int32_t start_advertising() = 0;
    virtual std::int32_t stop_advertising() = 0;
    virtual std::int32_t disconnect(std::uint16_t connection_handle) = 0;
    virtual std::int32_t configure_connection(
        std::uint16_t connection_handle) = 0;
    virtual void record_heap_checkpoint(HeapCheckpoint checkpoint) = 0;
};

struct ExposureSnapshot {
    usb_lifecycle::Snapshot lifecycle{};
    hid_runtime::StatusSnapshot runtime{};
};

// Immutable command evidence.  Its snapshot is valid only for an accepted or
// no-op transition; a scheduling failure is deliberately returned as Busy
// without exposing a pre-schedule snapshot.
struct CommandOutcome {
    usb_lifecycle::TransitionResult action_result = usb_lifecycle::TransitionResult::kBusy;
    bool snapshot_valid = false;
    ExposureSnapshot snapshot{};
};

struct RouteCommandOutcome {
    hid_runtime::RouteTransitionResult action_result =
        hid_runtime::RouteTransitionResult::kBusy;
    bool snapshot_valid = false;
    hid_runtime::RouteStatusSnapshot snapshot{};
};

struct BleCommandOutcome {
    ble_lifecycle::TransitionResult action_result =
        ble_lifecycle::TransitionResult::kBusy;
    bool snapshot_valid = false;
    ble_lifecycle::Snapshot snapshot{};
};

class Controller final : public usb_lifecycle::Executor, public BleEventSink {
  public:
    enum class ActionKind : std::uint8_t {
        kUsbInstall,
        kUsbDetach,
        kRouteRelease,
        kBleEnable,
        kBleDisable,
        kBleEvent,
    };

    struct Action {
        ActionKind kind = ActionKind::kUsbInstall;
        usb_lifecycle::Snapshot lifecycle{};
        hid_route::Snapshot route{};
        ControlOperation operation = ControlOperation::kNone;
        BleEvent ble_event{};
    };

    bool initialize(hid_runtime::Runtime *runtime, Backend *backend,
                    BleBackend *ble_backend = nullptr,
                    BleDatabase *ble_database = nullptr);

    CommandOutcome request_attach();
    CommandOutcome request_detach();
    RouteCommandOutcome request_route(hid_route::OutputRoute desired);
    ExposureSnapshot snapshot() const;
    hid_runtime::RouteStatusSnapshot route_snapshot() const;
    BleCommandOutcome request_ble_enable();
    BleCommandOutcome request_ble_disable();
    ble_lifecycle::Snapshot ble_snapshot() const;

    // usb_lifecycle::Executor. Calls originate in the UART/control task and
    // are stored in the shared fixed control-action queue.
    bool schedule(usb_lifecycle::ExecutorAction action,
                  usb_lifecycle::Snapshot snapshot) override;
    bool signal_ble_event(BleEvent event) override;

#ifdef HID_CONTROL_EXECUTOR_NATIVE_TEST
    bool process_one_for_test();
    ControlOperation active_operation_for_test() const;
    bool reserve_operation_for_test(ControlOperation operation);
    void release_operation_for_test(ControlOperation operation);
    void fail_next_enqueue_for_test();
#endif

  private:
    void process(Action action);
    bool enqueue(Action action);
    bool claim_operation(ControlOperation operation);
    void release_operation(ControlOperation operation);
    static ControlOperation operation_for(usb_lifecycle::ExecutorAction action);
    void process_ble_event(BleEvent event);
    void fail_ble(ble_lifecycle::Generation generation,
                  ble_lifecycle::Operation operation, std::int32_t code,
                  ControlOperation owner);

#ifndef HID_CONTROL_EXECUTOR_NATIVE_TEST
    static void task_entry(void *context);
    void task_loop();
#endif

    hid_runtime::Runtime *runtime_ = nullptr;
    Backend *backend_ = nullptr;
    BleBackend *ble_backend_ = nullptr;
    BleDatabase *ble_database_ = nullptr;
    ble_lifecycle::StateMachine ble_state_{};
    bool initialized_ = false;
    std::atomic<ControlOperation> active_operation_{ControlOperation::kNone};
    std::atomic_bool ble_event_overflow_{false};

#ifdef HID_CONTROL_EXECUTOR_NATIVE_TEST
    Action native_queue_[2]{};
    std::uint8_t native_head_ = 0;
    std::uint8_t native_count_ = 0;
    bool fail_next_enqueue_ = false;
#endif
};

}  // namespace hid_control_executor
