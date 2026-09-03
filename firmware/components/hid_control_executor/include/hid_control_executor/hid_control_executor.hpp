#pragma once

#include <array>
#include <atomic>
#include <cstddef>
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
    kSubscription,
    kControlPoint,
};

enum class BleHidInterface : std::uint8_t {
    kUnknown,
    kKeyboard,
    kMouse,
};

enum class BleSubscriptionReason : std::uint8_t {
    kUnknown,
    kWrite,
    kTerm,
    kRestore,
};

enum class BleNotifyBackendResult : std::uint8_t {
    kStackAccepted,
    kResourceFailure,
    kStackRejected,
};

enum class BleHidSubmitResult : std::uint8_t {
    kStackAccepted,
    kNotReady,
    kStale,
    kResourceFailure,
    kStackRejected,
};

struct BleHidHandles {
    std::uint16_t keyboard_value = 0;
    std::uint16_t mouse_value = 0;
    std::uint16_t control_point_value = 0;
};

struct BleHidWorkIdentity {
    ble_lifecycle::Generation generation = 0;
    std::uint16_t connection_handle = ble_lifecycle::kNoConnection;
    std::uint16_t characteristic_handle = 0;
};

struct BleHidPeerSnapshot {
    ble_lifecycle::Generation generation = 0;
    std::uint16_t connection_handle = ble_lifecycle::kNoConnection;
    BleHidHandles handles{};
    bool active = false;
    bool keyboard_notify_enabled = false;
    bool mouse_notify_enabled = false;
    bool suspended = false;
};

using BleKeyboardReport = std::array<std::uint8_t, 8>;
using BleMouseReport = std::array<std::uint8_t, 5>;
inline constexpr BleKeyboardReport kBleKeyboardAllUp{};
inline constexpr BleMouseReport kBleMouseAllUp{};
inline constexpr std::uint32_t kBleRouteReleaseGraceMs = 100;

// Exact identity of one BLE-route safety retirement. It is never interpreted
// as the current peer: every field must still match the retained old route.
struct BleRouteReleaseIdentity {
    hid_runtime::AuthorityEpoch authority_epoch = 0;
    hid_runtime::RouteGeneration route_generation = 0;
    ble_lifecycle::Generation ble_generation = 0;
    std::uint16_t connection_handle = ble_lifecycle::kNoConnection;
    std::uint16_t keyboard_characteristic_handle = 0;
    std::uint16_t mouse_characteristic_handle = 0;
    std::uint32_t release_epoch = 0;
};

struct BleEvent {
    BleEventKind kind = BleEventKind::kSync;
    ble_lifecycle::Generation generation = 0;
    std::uint16_t connection_handle = ble_lifecycle::kNoConnection;
    std::int32_t status = 0;
    std::uint32_t pairing_id = 0;
    std::uint16_t attribute_handle = 0;
    BleHidInterface hid_interface = BleHidInterface::kUnknown;
    BleSubscriptionReason subscription_reason =
        BleSubscriptionReason::kUnknown;
    ble_security::StoreFailureKind store_failure_kind =
        ble_security::StoreFailureKind::kNone;
    bool notify_enabled = false;
    bool suspended = false;
};

class BleEventSink {
  public:
    virtual ~BleEventSink() = default;
    // Callback-safe: implementations must use bounded, zero-wait signaling.
    virtual bool signal_ble_event(BleEvent event) = 0;
    virtual bool signal_ble_route_release_grace(
        BleRouteReleaseIdentity identity) = 0;
    // Called only after a Reset, post-Reset Sync, or one-shot lifecycle timeout
    // event could not enter the fixed queue. This is a durable fail-closed
    // handoff, not a generic event authority, so a backend generation that
    // legitimately leads the executor cannot be mistaken for an arbitrary
    // future generic overflow.
    virtual void signal_ble_lifecycle_handoff_failure() = 0;
};

class BleDatabase {
  public:
    virtual ~BleDatabase() = default;
    virtual int register_database() = 0;
    // Called only after the NimBLE GATT server has started. A zero result is
    // required before any project HID advertisement may become visible.
    virtual int validate_registered_database() = 0;
    virtual void bind_event_sink(BleEventSink *sink) = 0;
    virtual void set_generation(ble_lifecycle::Generation generation) = 0;
    virtual BleHidHandles hid_handles() const = 0;
    virtual BleNotifyBackendResult notify_custom(
        std::uint16_t connection_handle, std::uint16_t characteristic_handle,
        const std::uint8_t *payload, std::uint16_t payload_length) = 0;
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
    virtual std::int32_t arm_ble_route_release_grace(
        BleRouteReleaseIdentity identity) = 0;
    virtual void cancel_ble_route_release_grace(
        BleRouteReleaseIdentity identity) = 0;
    // Callback-safe immediate teardown for a successful physical Connect whose
    // event could not be delivered. The executor never adopts this connection.
    virtual std::int32_t terminate_orphan_connection(
        std::uint16_t connection_handle) = 0;
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
    // These compound-security mutation seams are executor-only. Callback and
    // UART contexts may publish simple atomic inhibition/lifecycle Stage A,
    // but must enqueue work before calling any of these methods.
    virtual void begin_security(ble_lifecycle::Generation generation,
                                std::uint16_t connection_handle) = 0;
    virtual void refresh_security(std::uint16_t connection_handle,
                                  bool identity_resolved_event = false) = 0;
    virtual void retire_security(ble_lifecycle::Generation generation,
                                 std::uint16_t connection_handle) = 0;
    virtual void mark_security_unhealthy(
        ble_lifecycle::Generation generation) = 0;
    // Serialized executor commit of connection-local failure evidence.
    virtual void apply_store_failure(
        ble_lifecycle::Generation generation,
        std::uint16_t connection_handle,
        ble_security::StoreFailureKind kind, std::int32_t status) = 0;
    // Serialized executor commit of a subsystem-global persistent-store fault.
    virtual void apply_persistent_store_failure(
        ble_security::StoreFailureKind kind, std::int32_t status) = 0;
    virtual bool persistent_store_failure_observed() const = 0;
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
    bool ble_route_ready = false;
    ble_pairing::Snapshot pairing{};
    ble_security::Snapshot security{};
    ble_lifecycle::Generation generation = 0;
    bool connected = false;
    std::uint32_t remaining_ms = 0;
};

class Controller final : public usb_lifecycle::Executor,
                         public BleEventSink,
                         public hid_runtime::AuthorityEventSink {
  public:
    static constexpr std::size_t kActionQueueDepth = 12;
    enum class ActionKind : std::uint8_t {
        kUsbInstall,
        kUsbDetach,
        kRouteRelease,
        kBleEnable,
        kBleDisable,
        kBleEvent,
        kRouteBleActivate,
        kPairingStatus,
        kPairingRespond,
        kBleHidReport,
        kBleRouteReleaseGrace,
    };

    struct Action {
        ActionKind kind = ActionKind::kUsbInstall;
        usb_lifecycle::Snapshot lifecycle{};
        hid_route::Snapshot route{};
        ControlOperation operation = ControlOperation::kNone;
        BleEvent ble_event{};
        hid_runtime::Interface hid_interface = hid_runtime::Interface::kKeyboard;
        hid_runtime::HidWorkToken hid_work{};
        std::uint32_t mailbox_token = 0;
    };

    bool initialize(hid_runtime::Runtime *runtime, Backend *backend,
                    BleBackend *ble_backend = nullptr,
                    BleDatabase *ble_database = nullptr);

    CommandOutcome request_attach();
    CommandOutcome request_detach();
    RouteCommandOutcome request_route(hid_route::OutputRoute desired);
    // Internal-only test seam. Production BLE activation enters through
    // request_route() and is dispatched into the serialized owner context.
    RouteCommandOutcome activate_ble_route_internal();
#ifndef HID_CONTROL_EXECUTOR_NATIVE_TEST
    hid_runtime::KeyboardReportResult keyboard_report(
        std::uint8_t modifiers,
        const std::array<std::uint8_t, 6> &keycodes);
    hid_runtime::MouseReportResult mouse_report(
        std::uint8_t buttons, std::int8_t x, std::int8_t y,
        std::int8_t vertical, std::int8_t horizontal);
#endif
    hid_runtime::KeyboardReportBeginResult queue_ble_keyboard_report(
        std::uint8_t modifiers,
        const std::array<std::uint8_t, 6> &keycodes);
    hid_runtime::MouseReportBeginResult queue_ble_mouse_report(
        std::uint8_t buttons, std::int8_t x, std::int8_t y,
        std::int8_t vertical, std::int8_t horizontal);
    ExposureSnapshot snapshot() const;
    hid_runtime::RouteStatusSnapshot route_snapshot();
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
    bool signal_ble_route_release_grace(
        BleRouteReleaseIdentity identity) override;
    void signal_ble_lifecycle_handoff_failure() override;
    void signal_hid_authority_change() override;

    // Internal BLE adapter seams. The general runtime reaches these only via
    // an exact U7.4B ticket in the serialized control-owner context; UART,
    // host, and CLI have no direct adapter or BLE-route entry point.
    BleHidPeerSnapshot ble_hid_peer_snapshot() const;
    bool ble_link_ready() const;
    BleHidSubmitResult submit_ble_keyboard(
        BleHidWorkIdentity identity, const BleKeyboardReport &report);
    BleHidSubmitResult submit_ble_mouse(
        BleHidWorkIdentity identity, const BleMouseReport &report);

#ifdef HID_CONTROL_EXECUTOR_NATIVE_TEST
    using OverflowConsumeHook = void (*)(Controller &controller);
    enum class BleEnqueueFailurePhase : std::uint8_t {
        kBeforeGenericFallback,
        kAfterGenericFallback,
    };
    using BleEnqueueFailureHook = void (*)(Controller &controller);
    using ProcessAfterReconciliationHook = void (*)(Controller &controller);
    bool process_one_for_test();
    bool process_wake_cycle_for_test();
    bool executor_wake_pending_for_test() const;
    bool dequeue_one_for_test(Action &action);
    void process_for_test(Action action);
    void set_overflow_consume_hook_for_test(OverflowConsumeHook hook);
    void set_ble_enqueue_failure_hook_for_test(
        BleEnqueueFailurePhase phase, BleEnqueueFailureHook hook);
    void set_process_after_reconciliation_hook_for_test(
        ProcessAfterReconciliationHook hook);
    void set_ble_generation_for_test(ble_lifecycle::Generation generation);
    ControlOperation active_operation_for_test() const;
    bool reserve_operation_for_test(ControlOperation operation);
    void release_operation_for_test(ControlOperation operation);
    void fail_next_enqueue_for_test();
    void set_next_pairing_id_for_test(std::uint32_t value);
    bool pairing_mailbox_zero_for_test() const;
    bool expire_ble_route_release_grace_for_test();
    BleRouteReleaseIdentity ble_route_release_identity_for_test() const;
#endif

  private:
    void process(Action action);
    bool enqueue(Action action);
    void request_executor_wake();
    bool reconcile_ble_fallbacks(const Action *action);
    bool claim_operation(ControlOperation operation);
    void release_operation(ControlOperation operation);
    static ControlOperation operation_for(usb_lifecycle::ExecutorAction action);
    void process_ble_event(BleEvent event);
    void fail_ble(ble_lifecycle::Generation generation,
                  ble_lifecycle::Operation operation, std::int32_t code,
                  ControlOperation owner);
    void commit_persistent_store_failure(
        ble_security::StoreFailureKind kind, std::int32_t status);
    void terminate_security_connection(ble_pairing::LastResult result,
                                       bool fatal);
    void reconcile_security(std::uint16_t connection_handle,
                            bool pairing_complete_seen);
    bool event_targets_current_ble_authority(BleEvent event) const;
    void mark_ble_event_overflow(BleEvent event);
    bool ble_event_overflow_pending(
        ble_lifecycle::Generation generation) const;
    void mark_ble_route_loss(BleEvent event);
    bool ble_route_loss_pending(ble_lifecycle::Generation generation) const;
    void clear_ble_route_loss(ble_lifecycle::Generation generation);
    static bool event_immediately_loses_ble_hid_readiness(BleEvent event);
    bool reconcile_ble_lifecycle_handoff_failure();
    void fail_current_ble_queue_overflow();
    bool consume_ble_overflow();
    void reconcile_pairing_deadline();
    PairingStatusSnapshot current_pairing_status() const;
    RouteCommandOutcome activate_ble_route();
    bool ble_route_ready() const;
    void wipe_pairing_mailbox();
    void complete_pairing_rpc(std::uint32_t token);
    void begin_ble_hid_peer(ble_lifecycle::Generation generation,
                            std::uint16_t connection_handle);
    void clear_ble_hid_peer();
    bool current_ble_hid_identity(BleHidWorkIdentity identity,
                                  BleHidInterface interface) const;
    bool ble_hid_interface_ready(BleHidWorkIdentity identity,
                                 BleHidInterface interface) const;
    BleHidSubmitResult submit_ble_report(
        BleHidWorkIdentity identity, BleHidInterface interface,
        const std::uint8_t *payload, std::uint16_t payload_length);
    void retire_ble_route_if_unready();
    void drive_ble_route_retirement();
    bool ble_route_release_identity_current(
        BleRouteReleaseIdentity identity) const;
    bool ble_safety_release_ready(BleRouteReleaseIdentity identity,
                                  BleHidInterface interface) const;
    void submit_ble_safety_release(BleRouteReleaseIdentity identity);
    void start_ble_route_disconnect(BleRouteReleaseIdentity identity);
    void note_ble_route_disconnect_result(
        ble_lifecycle::Generation generation,
        std::uint16_t connection_handle, std::int32_t result);
    void complete_ble_route_release_on_disconnect(BleEvent event);
    void cancel_ble_route_release_grace(BleRouteReleaseIdentity identity);
    bool enqueue_ble_hid_work(hid_runtime::Interface interface,
                              hid_runtime::HidWorkToken token);
    static hid_runtime::BleSubmitResult submit_runtime_ble_report(
        void *context, hid_runtime::Interface interface,
        hid_runtime::HidWorkToken token, const std::uint8_t *payload,
        std::uint16_t payload_length);

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
    // A nonzero value is the exact BLE lifecycle authority whose event stream
    // became uncertain. Generation zero has a separate bit so zero can remain
    // the inactive sentinel for the primary atomic. Producers replace only a
    // stale authority with the current one; they never publish a connection
    // tuple or clear pending uncertainty.
    static_assert(std::atomic<ble_lifecycle::Generation>::is_always_lock_free);
    static_assert(std::atomic_bool::is_always_lock_free);
    std::atomic<ble_lifecycle::Generation> overflow_authority_{0};
    std::atomic_bool overflow_authority_zero_{false};
    // Callback-side, generation-fenced fail-closed bell for physical events
    // that revoke HID readiness before their queued compound-state update is
    // consumed. It gates notification submission but never mutates route or
    // peer state outside the serialized executor.
    std::atomic<ble_lifecycle::Generation> ble_route_loss_authority_{0};
    std::atomic_bool ble_route_loss_authority_zero_{false};
    // Reset/Sync is a lifecycle ownership transfer: the backend may already
    // own the next generation while the executor still owns the retired one.
    // A failed publication is therefore a separate boot-lifetime fail-closed
    // latch, not another generation mailbox. It is monotonic until reboot.
    std::atomic_bool ble_lifecycle_handoff_failure_{false};
    bool ble_lifecycle_handoff_failure_committed_ = false;
    // Executor-owned acknowledgment of the boot-lifetime backend latch.
    // The callback-side latch itself remains monotonic and authoritative.
    bool persistent_store_failure_committed_ = false;
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
    RouteCommandOutcome route_rpc_result_{};
    BleHidPeerSnapshot ble_hid_peer_{};
    enum class BleRouteReleasePhase : std::uint8_t {
        kNone,
        kGrace,
        kDisconnecting,
        kFault,
    };
    BleRouteReleaseIdentity ble_route_release_{};
    BleRouteReleasePhase ble_route_release_phase_ =
        BleRouteReleasePhase::kNone;
    ControlOperation ble_route_release_owner_ = ControlOperation::kNone;
    std::atomic_bool ble_route_grace_armed_{false};
    std::atomic_bool ble_route_grace_due_{false};
    std::atomic_bool ble_route_disconnect_observed_{false};

#ifdef HID_CONTROL_EXECUTOR_NATIVE_TEST
    Action native_queue_[kActionQueueDepth]{};
    std::uint8_t native_head_ = 0;
    std::uint8_t native_count_ = 0;
    bool fail_next_enqueue_ = false;
    std::atomic_bool native_executor_wake_pending_{false};
    OverflowConsumeHook overflow_consume_hook_ = nullptr;
    BleEnqueueFailureHook ble_enqueue_failure_hook_ = nullptr;
    BleEnqueueFailurePhase ble_enqueue_failure_phase_ =
        BleEnqueueFailurePhase::kBeforeGenericFallback;
    ProcessAfterReconciliationHook process_after_reconciliation_hook_ = nullptr;
#endif
};

}  // namespace hid_control_executor
