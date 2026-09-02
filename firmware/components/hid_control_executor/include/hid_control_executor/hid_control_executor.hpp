#pragma once

#include <array>
#include <atomic>
#include <cstdint>

#include "ble_pairing/ble_pairing.hpp"
#include "ble_security/ble_security.hpp"
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
    kPasskeyAction,
    kEncryptionChange,
    kPairingComplete,
    kIdentityResolved,
    kRepeatPairing,
    kPairingTimeout,
    kStoreFull,
    kStorageFailure,
};

struct BleEvent {
    BleEventKind kind = BleEventKind::kSync;
    ble_lifecycle::Generation generation = 0;
    std::uint16_t connection_handle = ble_lifecycle::kNoConnection;
    std::int32_t status = 0;
    std::uint32_t pairing_id = 0;
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
    virtual std::int32_t initiate_security(
        std::uint16_t connection_handle) = 0;
    virtual std::int32_t inject_passkey(std::uint16_t connection_handle,
                                       std::uint32_t passkey) = 0;
    virtual std::uint64_t monotonic_time_us() const = 0;
    virtual void arm_pairing_timeout(ble_lifecycle::Generation generation,
                                     std::uint16_t connection_handle,
                                     std::uint32_t pairing_id) = 0;
    virtual void cancel_pairing_timeout() = 0;
    virtual void begin_security(ble_lifecycle::Generation generation,
                                std::uint16_t connection_handle) = 0;
    virtual void refresh_security(std::uint16_t connection_handle,
                                  bool identity_resolved_event = false) = 0;
    virtual void retire_security(ble_lifecycle::Generation generation,
                                 std::uint16_t connection_handle) = 0;
    virtual void mark_security_unhealthy(
        ble_lifecycle::Generation generation) = 0;
    virtual ble_security::Snapshot security_snapshot() const = 0;
    virtual bool security_ready_for_hid(
        ble_lifecycle::Generation generation,
        std::uint16_t connection_handle) const = 0;
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

struct PairingStatusSnapshot {
    bool available = false;
    ble_pairing::Snapshot pairing{};
    ble_security::Snapshot security{};
    ble_lifecycle::Generation generation = 0;
    bool connected = false;
    std::uint32_t remaining_ms = 0;
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
        kPairingStatus,
        kPairingRespond,
    };

    struct Action {
        ActionKind kind = ActionKind::kUsbInstall;
        usb_lifecycle::Snapshot lifecycle{};
        hid_route::Snapshot route{};
        ControlOperation operation = ControlOperation::kNone;
        BleEvent ble_event{};
        std::uint32_t mailbox_token = 0;
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
    ble_pairing::Snapshot pairing_snapshot() const;
    PairingStatusSnapshot request_pairing_status();
    ble_pairing::RespondResult request_pairing_response(
        std::uint32_t pairing_id,
        const std::array<char, 6> &six_digit_secret);

    // Internal executor-owned seam. It is deliberately not connected to the
    // UART protocol; callers must already run in the serialized owner context.
    ble_pairing::RespondResult respond_to_pairing(
        ble_lifecycle::Generation generation,
        std::uint16_t connection_handle, std::uint32_t pairing_id,
        const std::array<char, 6> &six_digit_secret);

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
    void set_next_pairing_id_for_test(std::uint32_t value);
    bool pairing_mailbox_zero_for_test() const;
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
    void terminate_security_connection(ble_pairing::LastResult result,
                                       bool fatal);
    void reconcile_security(std::uint16_t connection_handle,
                            bool pairing_complete_seen);
    bool consume_ble_overflow();
    void reconcile_pairing_deadline();
    PairingStatusSnapshot current_pairing_status() const;
    void wipe_pairing_mailbox();
    void complete_pairing_rpc(std::uint32_t token);

#ifndef HID_CONTROL_EXECUTOR_NATIVE_TEST
    static void task_entry(void *context);
    void task_loop();
#endif

    hid_runtime::Runtime *runtime_ = nullptr;
    Backend *backend_ = nullptr;
    BleBackend *ble_backend_ = nullptr;
    BleDatabase *ble_database_ = nullptr;
    ble_lifecycle::StateMachine ble_state_{};
    ble_pairing::StateMachine pairing_state_{};
    bool initialized_ = false;
    std::atomic<ControlOperation> active_operation_{ControlOperation::kNone};
    std::atomic_bool ble_event_overflow_{false};
    std::atomic<ble_lifecycle::Generation> overflow_generation_{0};
    std::atomic<std::uint16_t> overflow_connection_{
        ble_lifecycle::kNoConnection};
    bool pairing_complete_seen_ = false;
    std::uint64_t pairing_deadline_us_ = 0;
    struct PairingMailbox {
        ble_lifecycle::Generation generation = 0;
        std::uint16_t connection_handle = ble_lifecycle::kNoConnection;
        std::uint32_t pairing_id = 0;
        std::array<char, 6> secret{};
        std::uint32_t token = 0;
        bool occupied = false;
    } pairing_mailbox_{};
    std::atomic<std::uint32_t> pairing_rpc_pending_{0};
    std::uint32_t next_pairing_rpc_token_ = 1;
    PairingStatusSnapshot pairing_rpc_status_{};
    ble_pairing::RespondResult pairing_rpc_result_ =
        ble_pairing::RespondResult::kNotPending;

#ifdef HID_CONTROL_EXECUTOR_NATIVE_TEST
    Action native_queue_[8]{};
    std::uint8_t native_head_ = 0;
    std::uint8_t native_count_ = 0;
    bool fail_next_enqueue_ = false;
#endif
};

}  // namespace hid_control_executor
