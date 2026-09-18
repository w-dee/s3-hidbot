#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "ble_fixture_profile/ble_fixture_profile.hpp"
#include "ble_pairing/ble_pairing.hpp"
#include "ble_security/ble_security.hpp"
#include "hid_route/hid_route.hpp"
#include "hid_runtime/hid_runtime.hpp"
#include "ble_lifecycle/ble_lifecycle.hpp"
#include "ble_lifecycle/stop_transaction.hpp"
#include "usb_lifecycle/usb_lifecycle.hpp"

namespace hid_control_executor {

enum class ControlOperation : std::uint8_t {
    kNone,
    kUsbAttach,
    kUsbDetach,
    kRouteChange,
    kBleEnable,
    kBleDisable,
    kBondAdministration,
    kProfileSelection,
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

enum class UsbRuntimeFaultReason : std::uint8_t {
    kNone,
    kDiagnostic,
    kEventQueueOverflow,
};

struct UsbRuntimeFaultSnapshot {
    UsbRuntimeFaultReason reason = UsbRuntimeFaultReason::kNone;
    std::uint32_t first_event_id = UINT32_MAX;
    std::uint32_t occurrences = 0;
    bool observed_in_isr = false;
};

enum class UsbLifecycleEvent : std::uint8_t {
    kMounted = 1U << 0,
    kUnmounted = 1U << 1,
    kSuspended = 1U << 2,
    kResumed = 1U << 3,
    kSofStall = 1U << 4,
};

inline constexpr std::uint32_t kUsbSofStallTimeoutMs = 100;
inline constexpr std::uint32_t kUsbSofWatchdogSampleMs = 10;

// The executor invokes this only after the runtime has already advanced its
// atomic HID authority. The production sink performs nonblocking eventual
// UART session/cache cleanup; it does not call back into the executor.
using UsbLinkStallSink = void (*)();

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
    kReportMapRead,
    kServiceChangedSubscription,
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
    std::uint16_t report_map_value = 0;
    std::uint16_t keyboard_value = 0;
    std::uint16_t mouse_value = 0;
    std::uint16_t control_point_value = 0;
};

struct BleHidWorkIdentity {
    ble_lifecycle::Generation generation = 0;
    hid_runtime::ProfileActivationEpoch profile_activation_epoch = 0;
    std::uint16_t connection_handle = ble_lifecycle::kNoConnection;
    std::uint16_t characteristic_handle = 0;
};

struct BleHidPeerSnapshot {
    ble_lifecycle::Generation generation = 0;
    std::uint16_t connection_handle = ble_lifecycle::kNoConnection;
    BleHidHandles handles{};
    bool active : 1 = false;
    bool keyboard_notify_enabled : 1 = false;
    bool mouse_notify_enabled : 1 = false;
    bool suspended : 1 = false;
    bool report_map_read : 1 = false;
    bool service_changed_indicate_enabled : 1 = false;
    bool schema_checked : 1 = false;
    bool refresh_requested : 1 = false;
    hid_capability::ReportMask fresh_input_subscriptions = 0;
};

inline constexpr std::uint16_t kGattChangedStartHandle = 0x0001;
inline constexpr std::uint16_t kGattChangedEndHandle = 0xffff;

enum class GattSchemaStoreResultKind : std::uint8_t {
    kCurrent,
    kIncompatible,
    kStale,
    kCapacityFull,
    kStorageFailure,
    kStaleIdentity,
};

struct GattSchemaStoreResult {
    GattSchemaStoreResultKind kind = GattSchemaStoreResultKind::kStaleIdentity;
    std::int32_t status = 0;
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
    hid_runtime::ProfileActivationEpoch profile_activation_epoch = 0;
    ble_lifecycle::Generation ble_generation = 0;
    std::uint16_t connection_handle = ble_lifecycle::kNoConnection;
    hid_runtime::ReportMask present_roles = 0;
    hid_runtime::ReportMask required_input_subscriptions = 0;
    hid_runtime::ReportHandles report_handles{};
    std::uint32_t release_epoch = 0;
};

inline constexpr std::size_t kBondIdHexChars = 32;
using BondId = std::array<char, kBondIdHexChars + 1>;

struct BleBondInfo {
    BondId bond_id{};
    bool our_sec = false;
    bool peer_sec = false;
    bool verified = false;
    bool schema_revision_present = false;
    std::uint8_t schema_revision = 0;
    bool schema_current = false;
    bool connected = false;
};

enum class BleBondListResultKind : std::uint8_t {
    kSuccess,
    kNotReady,
    kStorageFailure,
};

struct BleBondListResult {
    BleBondListResultKind kind = BleBondListResultKind::kNotReady;
    std::array<BleBondInfo, ble_security::kBondCapacity> bonds{};
    std::uint8_t count = 0;
    std::uint8_t available = ble_security::kBondCapacity;
    bool healthy = false;
};

enum class BleBondRemoveResultKind : std::uint8_t {
    kSuccess,
    kNotReady,
    kNotFound,
    kAmbiguous,
    kBusy,
    kStorageFailure,
};

struct BleBondRemoveResult {
    BleBondRemoveResultKind kind = BleBondRemoveResultKind::kNotReady;
    BondId bond_id{};
    std::uint8_t remaining = 0;
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
    bool indicate_enabled = false;
    bool suspended = false;
    std::uint32_t stack_incarnation = 0;
    // Nonzero only for a finite, behavior-owned advertising arm. The
    // controller supplies this nonreused identity before the host arm.
    std::uint64_t advertising_incarnation = 0;
};

class BleEventSink {
  public:
    virtual ~BleEventSink() = default;
    // Callback-safe: implementations must use bounded, zero-wait signaling.
    virtual bool signal_ble_event(BleEvent event) = 0;
    // Reset retires DLE before backend generation/watchdog work begins.
    virtual void retire_dle_on_reset(ble_lifecycle::Generation generation) = 0;
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
    // Called only with the old host proven stopped (or before first init).
    virtual bool configure_profile(ble_fixture_profile::ProfileId id) {
        return id == ble_fixture_profile::ProfileId::kStrictComposite;
    }
    virtual void reset_after_stop() {}
    virtual void set_stack_incarnation(std::uint32_t) {}
    virtual int register_database() = 0;
    // Called only after the NimBLE GATT server has started. A zero result is
    // required before any project HID advertisement may become visible.
    virtual int validate_registered_database() = 0;
    virtual void bind_event_sink(BleEventSink *sink) = 0;
    virtual void set_generation(ble_lifecycle::Generation generation) = 0;
    virtual BleHidHandles hid_handles() const = 0;
    virtual ble_fixture_profile::LedValue led_value(
        ble_lifecycle::Generation, std::uint16_t) const { return {}; }
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
    // Stop is asynchronous and exact-owner scoped. No worker result alone
    // permits reinitialization; the serialized owner must consume proven stop.
    virtual bool configure_profile(ble_fixture_profile::ProfileId id,
                                    std::uint32_t) {
        return id == ble_fixture_profile::ProfileId::kStrictComposite;
    }
    virtual std::uint64_t begin_stop() { return 0; }
    virtual ble_lifecycle::StopStatus poll_stop(std::uint64_t) const {
        return ble_lifecycle::StopStatus::kWrongOwner;
    }
    virtual void expire_stop(std::uint64_t) {}
    virtual bool finish_stop(std::uint64_t) { return false; }
    virtual void set_generation(ble_lifecycle::Generation generation) = 0;
    virtual std::int32_t start_advertising() = 0;
    virtual std::int32_t start_finite_advertising(
        std::uint16_t interval_units, std::uint32_t timeout_ms,
        std::uint64_t advertising_incarnation) {
        (void)interval_units;
        (void)timeout_ms;
        (void)advertising_incarnation;
        return start_advertising();
    }
    virtual std::int32_t stop_advertising() = 0;
    // Stop advertising and initiate teardown of any physical peer, including
    // one whose Connect has not yet reached the executor. Completion requires
    // a separate physical-absence observation; neither API success nor a
    // retired callback alone proves hidden exposure.
    virtual std::int32_t begin_hidden_exposure() = 0;
    virtual bool physical_exposure_hidden() const = 0;
    virtual std::int32_t disconnect(std::uint16_t connection_handle) = 0;
    // This classification is consumed only by the exact, executor-owned
    // security teardown path. Other disconnect callers retain their existing
    // fail-closed treatment of every nonzero initiation result.
    virtual bool security_teardown_already_disconnected(
        std::int32_t disconnect_result) const = 0;
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
    virtual std::int32_t set_connection_data_length(
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
    virtual GattSchemaStoreResult gatt_schema_status(
        ble_lifecycle::Generation generation,
        std::uint16_t connection_handle) = 0;
    virtual GattSchemaStoreResult persist_gatt_schema_current(
        ble_lifecycle::Generation generation,
        std::uint16_t connection_handle) = 0;
    virtual bool gatt_schema_current_for_hid(
        ble_lifecycle::Generation generation,
        std::uint16_t connection_handle) const = 0;
    virtual std::uint16_t service_changed_value_handle() const = 0;
    virtual std::int32_t request_gatt_cache_refresh(
        ble_lifecycle::Generation generation,
        std::uint16_t connection_handle, std::uint16_t start_handle,
        std::uint16_t end_handle) = 0;
    // These store operations are invoked only by Controller's serialized task.
    // Implementations expose opaque IDs and never return security key material.
    virtual BleBondListResult list_bonds() = 0;
    virtual BleBondRemoveResult remove_bond(const BondId &bond_id) = 0;
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
        kProfileSelect,
        kBleEvent,
        kRouteBleActivate,
        kPairingStatus,
        kPairingRespond,
        kBleHidReport,
        kBleRouteReleaseGrace,
        kBondList,
        kBondRemove,
    };

    struct Action {
        struct LifecyclePayload {
            usb_lifecycle::Snapshot lifecycle{};
            hid_route::Snapshot route{};
            ControlOperation operation = ControlOperation::kNone;
        };

        struct OperationPayload {
            ControlOperation operation = ControlOperation::kNone;
            std::uint32_t mailbox_token = 0;
        };

        struct BleEventPayload {
            BleEvent event{};
        };

        struct HidReportPayload {
            hid_runtime::Interface interface = hid_runtime::Interface::kKeyboard;
            hid_runtime::HidWorkToken work{};
        };

        union Payload {
            LifecyclePayload lifecycle;
            OperationPayload operation;
            BleEventPayload ble_event;
            HidReportPayload hid_report;

            constexpr Payload() : operation{} {}
            constexpr explicit Payload(LifecyclePayload value)
                : lifecycle(value) {}
            constexpr explicit Payload(OperationPayload value)
                : operation(value) {}
            constexpr explicit Payload(BleEventPayload value)
                : ble_event(value) {}
            constexpr explicit Payload(HidReportPayload value)
                : hid_report(value) {}
        };

        ActionKind kind = ActionKind::kUsbInstall;
        Payload payload{};

        static Action empty(ActionKind kind) {
            Action result{};
            result.kind = kind;
            return result;
        }

        static Action with_operation(ActionKind kind,
                                     ControlOperation operation,
                                     std::uint32_t mailbox_token = 0) {
            return Action{
                .kind = kind,
                .payload = Payload(OperationPayload{operation, mailbox_token}),
            };
        }

        static Action with_lifecycle(ActionKind kind,
                                     usb_lifecycle::Snapshot lifecycle,
                                     hid_route::Snapshot route,
                                     ControlOperation operation) {
            return Action{
                .kind = kind,
                .payload = Payload(LifecyclePayload{lifecycle, route, operation}),
            };
        }

        static Action with_ble_event(BleEvent event) {
            return Action{
                .kind = ActionKind::kBleEvent,
                .payload = Payload(BleEventPayload{event}),
            };
        }

        static Action with_hid_report(hid_runtime::Interface interface,
                                      hid_runtime::HidWorkToken work) {
            return Action{
                .kind = ActionKind::kBleHidReport,
                .payload = Payload(HidReportPayload{interface, work}),
            };
        }
    };

    static_assert(std::is_trivially_copyable_v<Action>);
    static_assert(sizeof(Action) >= 56 && sizeof(Action) <= 64);
    static_assert(alignof(Action) <= alignof(std::uint64_t));

    bool initialize(hid_runtime::Runtime *runtime, Backend *backend,
                    BleBackend *ble_backend = nullptr,
                    BleDatabase *ble_database = nullptr,
                    UsbLinkStallSink usb_link_stall_sink = nullptr);

    CommandOutcome request_attach();
    CommandOutcome request_detach();
    RouteCommandOutcome request_route(hid_route::OutputRoute desired);
    // Internal-only test seam. Production BLE activation enters through
    // request_route() and is dispatched into the serialized owner context.
    RouteCommandOutcome activate_ble_route_internal();
#ifndef HID_CONTROL_EXECUTOR_NATIVE_TEST
    hid_runtime::KeyboardReportResult keyboard_report(
        std::uint8_t modifiers,
        const std::array<std::uint8_t, 6> &keycodes,
        hid_runtime::ReportOriginOwnerId originating_local_owner_id);
    hid_runtime::MouseReportResult mouse_report(
        std::uint8_t buttons, std::int8_t x, std::int8_t y,
        std::int8_t vertical, std::int8_t horizontal,
        hid_runtime::ReportOriginOwnerId originating_local_owner_id);
    hid_runtime::KeyboardReportResult sequence_keyboard_report(
        hid_runtime::SequenceAuthority sequence,
        std::uint8_t modifiers,
        const std::array<std::uint8_t, 6> &keycodes,
        hid_runtime::ReportOriginOwnerId originating_local_owner_id);
    hid_runtime::MouseReportResult sequence_mouse_report(
        hid_runtime::SequenceAuthority sequence, std::uint8_t buttons,
        hid_runtime::ReportOriginOwnerId originating_local_owner_id);
#endif
    hid_runtime::KeyboardReportBeginResult queue_ble_keyboard_report(
        std::uint8_t modifiers,
        const std::array<std::uint8_t, 6> &keycodes,
        hid_runtime::SequenceAuthority sequence = {},
        hid_runtime::HidTicketId *ticket_id = nullptr,
        hid_runtime::ReportOriginOwnerId originating_local_owner_id = 0);
    hid_runtime::MouseReportBeginResult queue_ble_mouse_report(
        std::uint8_t buttons, std::int8_t x, std::int8_t y,
        std::int8_t vertical, std::int8_t horizontal,
        hid_runtime::SequenceAuthority sequence = {},
        hid_runtime::HidTicketId *ticket_id = nullptr,
        hid_runtime::ReportOriginOwnerId originating_local_owner_id = 0);
    ExposureSnapshot snapshot() const;
    hid_runtime::RouteStatusSnapshot route_snapshot();
    ble_fixture_profile::SelectionSnapshot profile_snapshot() const;
    ble_fixture_profile::LedStatus led_status() const;
    ble_fixture_profile::SelectionOutcome request_profile_select(
        ble_fixture_profile::ProfileId id);
    BleCommandOutcome request_ble_enable();
    BleCommandOutcome request_ble_disable();
    ble_lifecycle::Snapshot ble_snapshot() const;
    ble_pairing::Snapshot pairing_snapshot() const;
    PairingStatusSnapshot request_pairing_status();
    ble_pairing::RespondResult request_pairing_response(
        std::uint32_t pairing_id,
        const std::array<char, 6> &six_digit_secret);
    BleBondListResult request_bond_list();
    BleBondRemoveResult request_bond_remove(const BondId &bond_id);

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
    void retire_dle_on_reset(ble_lifecycle::Generation generation) override;
    void signal_hid_authority_change() override;
    // TinyUSB callback/ISR seam. It performs only bounded lock-free state
    // publication plus a task notification; teardown remains task-owned.
    void signal_usb_runtime_fault(UsbRuntimeFaultReason reason,
                                  std::uint32_t event_id, bool in_isr);
    void signal_usb_lifecycle_event(UsbLifecycleEvent event);
    UsbRuntimeFaultSnapshot usb_runtime_fault_snapshot() const;

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
    enum class BleGraceSignalPhase : std::uint8_t {
        kAfterIdentityValidation,
        kAfterClaim,
    };
    using BleGraceSignalHook = void (*)(Controller &controller);
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
    void set_ble_grace_signal_hook_for_test(BleGraceSignalPhase phase,
                                            BleGraceSignalHook hook);
    void set_ble_generation_for_test(ble_lifecycle::Generation generation);
    void drive_profile_selection_for_test();
    void drive_ble_disable_for_test();
    void set_stack_incarnation_for_test(std::uint32_t value);
    ControlOperation active_operation_for_test() const;
    bool reserve_operation_for_test(ControlOperation operation);
    void release_operation_for_test(ControlOperation operation);
    void fail_next_enqueue_for_test();
    void set_next_pairing_id_for_test(std::uint32_t value);
    bool pairing_mailbox_zero_for_test() const;
    bool expire_ble_route_release_grace_for_test();
    BleRouteReleaseIdentity ble_route_release_identity_for_test() const;
    std::size_t ble_grace_available_slots_for_test() const;
    void set_ble_grace_next_incarnation_for_test(std::uint64_t value);
    bool reconcile_security_disconnect_absent_for_test(
        ble_lifecycle::Generation generation,
        std::uint16_t connection_handle);
    void set_usb_runtime_fault_occurrences_for_test(std::uint32_t value);
    bool service_usb_sof_watchdog_for_test(std::uint32_t now_ms);
    bool usb_sof_watchdog_armed_for_test() const;
    using SofWatchdogBeforeFenceHook = void (*)(Controller &controller);
    void set_sof_watchdog_before_fence_hook_for_test(
        SofWatchdogBeforeFenceHook hook);
#endif

  private:
    // The timer dispatcher can retain one old producer while the serialized
    // owner arms its replacement. Two fixed slots keep each arm, callback
    // claim, due publication, cancellation, and consumption attached to the
    // same complete retirement identity and non-reused incarnation.
    class BleRouteGraceAuthority {
      public:
        static constexpr std::size_t kSlotCount = 2;

        struct Claim {
            std::size_t slot = kSlotCount;
            std::uint64_t ownership = 0;
            BleRouteReleaseIdentity identity{};

            constexpr bool valid() const { return slot < kSlotCount; }
        };

        Claim arm(BleRouteReleaseIdentity identity);
        Claim find_armed(BleRouteReleaseIdentity identity) const;
        Claim claim(Claim candidate);
        bool publish_due(Claim claim);
        void cancel(Claim claim);
        bool consume_due(Claim claim, BleRouteReleaseIdentity identity);
        std::size_t available_slots_for_test() const;
        void set_next_incarnation_for_test(std::uint64_t value) {
            next_incarnation_.store(value, std::memory_order_release);
        }
        static bool identities_equal(BleRouteReleaseIdentity left,
                                     BleRouteReleaseIdentity right);

      private:
        enum class State : std::uint8_t {
            kIdle,
            kArming,
            kArmed,
            kClaimed,
            kCanceledClaimed,
            kDue,
        };

        static constexpr std::uint64_t kStateMask = 0xffU;
        static constexpr unsigned kStateBits = 8;
        static constexpr std::uint64_t kMaxIncarnation =
            UINT64_MAX >> kStateBits;

        static constexpr std::uint64_t pack(std::uint64_t incarnation,
                                            State state) {
            return (incarnation << kStateBits) |
                   static_cast<std::uint8_t>(state);
        }
        static constexpr State state_of(std::uint64_t ownership) {
            return static_cast<State>(ownership & kStateMask);
        }
        static constexpr std::uint64_t with_state(std::uint64_t ownership,
                                                  State state) {
            return (ownership & ~kStateMask) |
                   static_cast<std::uint8_t>(state);
        }
        std::uint64_t allocate_incarnation();

        struct AtomicIdentity {
            std::atomic<hid_runtime::AuthorityEpoch> authority_epoch{0};
            std::atomic<hid_runtime::RouteGeneration> route_generation{0};
            std::atomic<hid_runtime::ProfileActivationEpoch>
                profile_activation_epoch{0};
            std::atomic<ble_lifecycle::Generation> ble_generation{0};
            std::atomic<std::uint16_t> connection_handle{
                ble_lifecycle::kNoConnection};
            std::atomic<hid_runtime::ReportMask> present_roles{0};
            std::atomic<hid_runtime::ReportMask> required_subscriptions{0};
            std::array<std::atomic<std::uint16_t>,
                       hid_capability::kReportRoleCount>
                report_handles{};
            std::atomic<std::uint32_t> release_epoch{0};

            void store(BleRouteReleaseIdentity identity);
            BleRouteReleaseIdentity load() const;
        };

        struct Slot {
            std::atomic<std::uint64_t> ownership{pack(0, State::kIdle)};
            AtomicIdentity identity{};
        };

        std::array<Slot, kSlotCount> slots_{};
        std::atomic<std::uint64_t> next_incarnation_{1};
        std::atomic<std::size_t> next_slot_{0};
    };

    void process(Action action);
    void drive_profile_selection();
    void drive_ble_disable();
    std::int32_t start_profile_advertising(
        ble_lifecycle::Generation generation, bool slow = false);
    bool simulated_sleep_entry_ready() const;
    const ble_fixture_profile::ProfileDefinition &selected_profile() const;
    void publish_profile(bool active, ble_fixture_profile::SelectionTransition transition);
    bool enqueue(Action action);
    void request_executor_wake();
    void request_executor_wake_from_isr();
    bool reconcile_usb_runtime_fault();
    bool reconcile_usb_sof_watchdog(std::uint32_t now_ms);
    void disarm_usb_sof_watchdog();
    void reconcile_usb_lifecycle_logs();
    bool reconcile_ble_fallbacks(const Action *action);
    bool claim_operation(ControlOperation operation);
    void release_operation(ControlOperation operation);
    static ControlOperation operation_for(usb_lifecycle::ExecutorAction action);
    void process_ble_event(BleEvent event);
    bool reconcile_ble_disconnect(BleEvent event, bool expected);
    bool reconcile_security_disconnect_absent(
        ble_lifecycle::Generation generation,
        std::uint16_t connection_handle);
    void fail_ble(ble_lifecycle::Generation generation,
                  ble_lifecycle::Operation operation, std::int32_t code,
                  ControlOperation owner);
    void commit_persistent_store_failure(
        ble_security::StoreFailureKind kind, std::int32_t status);
    void terminate_security_connection(ble_pairing::LastResult result,
                                       bool fatal);
    void reconcile_security(std::uint16_t connection_handle,
                            bool terminal_evidence_ready);
    void reconcile_gatt_cache();
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
    std::uint32_t begin_serialized_rpc();
    bool bond_remove_eligible() const;
    void begin_ble_hid_peer(ble_lifecycle::Generation generation,
                            std::uint16_t connection_handle);
    void clear_ble_hid_peer();
    bool current_ble_hid_identity(BleHidWorkIdentity identity,
                                  BleHidInterface interface) const;
    bool ble_hid_interface_ready(BleHidWorkIdentity identity,
                                 BleHidInterface interface) const;
    bool ble_explicit_release_ready(
        hid_runtime::ReleaseAllSnapshot transaction,
        BleHidInterface interface) const;
    void drive_ble_explicit_release();
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
    UsbLinkStallSink usb_link_stall_sink_ = nullptr;
    ble_lifecycle::StateMachine ble_state_{};
    // Profile ID, active presence and transition are one coherent word.
    std::atomic<std::uint32_t> profile_word_{0};
    // Separate nonreused callback incarnation; saturates at UINT32_MAX.
    std::atomic<std::uint32_t> ble_stack_incarnation_{0};
    std::uint64_t profile_stop_id_ = 0;
    std::uint64_t profile_deadline_us_ = 0;
    std::uint64_t ble_disable_deadline_us_ = 0;
    ble_lifecycle::Generation ble_disable_generation_ = 0;
    enum class SimulatedSleepStage : std::uint8_t {
        kInactive,
        kFastAdvertising,
        kSlowAdvertising,
        kSleepPending,
        kAsleep,
    };
    struct SimulatedSleepState {
        SimulatedSleepStage stage = SimulatedSleepStage::kInactive;
        ble_lifecycle::Generation generation = 0;
        std::uint32_t stack_incarnation = 0;
        std::uint64_t advertising_incarnation = 0;
    } simulated_sleep_{};
    std::uint64_t next_advertising_incarnation_ = 1;
    // Protected by the short DLE admission critical section, never across HCI.
    void observe_dle_event(BleEvent event);
    bool claim_dle(BleEvent event);
    ble_lifecycle::Generation dle_generation_ = 0;
    std::uint16_t dle_connection_ = ble_lifecycle::kNoConnection;
    bool dle_seen_ = false;
    bool dle_available_ = false;
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
    static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
    static_assert(std::atomic<std::uint8_t>::is_always_lock_free);
    static_assert(std::atomic<UsbRuntimeFaultReason>::is_always_lock_free);
    static_assert(std::atomic_bool::is_always_lock_free);
    std::atomic<UsbRuntimeFaultReason> usb_runtime_fault_reason_{
        UsbRuntimeFaultReason::kNone};
    std::atomic<std::uint32_t> usb_runtime_fault_event_{UINT32_MAX};
    std::atomic<std::uint32_t> usb_runtime_fault_occurrences_{0};
    std::atomic_bool usb_runtime_fault_in_isr_{false};
    std::atomic<std::uint8_t> usb_lifecycle_log_bits_{0};
    bool usb_runtime_fault_committed_ = false;
    struct UsbSofWatchdogState {
        hid_runtime::UsbLinkWatchdogSnapshot identity{};
        // While pending, this word stores the exact conditional route token;
        // otherwise it stores the last heartbeat-progress timestamp.
        std::uint32_t last_progress_ms = 0;
        bool armed = false;
        bool pending = false;
    } usb_sof_watchdog_{};
    bool pairing_complete_seen_ = false;
    bool pairing_terminal_committed_ = false;
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
    BondId bond_remove_mailbox_{};
    BleBondListResult bond_list_rpc_result_{};
    BleBondRemoveResult bond_remove_rpc_result_{};
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
    BleRouteGraceAuthority ble_route_grace_authority_{};
    BleRouteGraceAuthority::Claim ble_route_grace_claim_{};
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
    BleGraceSignalHook ble_grace_signal_hook_ = nullptr;
    BleGraceSignalPhase ble_grace_signal_phase_ =
        BleGraceSignalPhase::kAfterIdentityValidation;
    SofWatchdogBeforeFenceHook sof_watchdog_before_fence_hook_ = nullptr;
#endif
};

}  // namespace hid_control_executor
