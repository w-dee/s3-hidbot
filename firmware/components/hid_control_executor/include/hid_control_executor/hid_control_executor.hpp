#pragma once

#include <atomic>
#include <cstdint>

#include "hid_route/hid_route.hpp"
#include "hid_runtime/hid_runtime.hpp"
#include "usb_lifecycle/usb_lifecycle.hpp"

namespace hid_control_executor {

enum class ControlOperation : std::uint8_t {
    kNone,
    kUsbAttach,
    kUsbDetach,
    // U7.2A foundation-only placeholders; they execute no route/BLE work.
    kRouteChange,
    kBleChange,
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

class Controller final : public usb_lifecycle::Executor {
  public:
    enum class ActionKind : std::uint8_t {
        kUsbInstall,
        kUsbDetach,
        kRouteRelease,
    };

    struct Action {
        ActionKind kind = ActionKind::kUsbInstall;
        usb_lifecycle::Snapshot lifecycle{};
        hid_route::Snapshot route{};
        ControlOperation operation = ControlOperation::kNone;
    };

    bool initialize(hid_runtime::Runtime *runtime, Backend *backend);

    CommandOutcome request_attach();
    CommandOutcome request_detach();
    RouteCommandOutcome request_route(hid_route::OutputRoute desired);
    ExposureSnapshot snapshot() const;
    hid_runtime::RouteStatusSnapshot route_snapshot() const;

    // usb_lifecycle::Executor. Calls originate in the UART/control task and
    // are stored in the shared fixed control-action queue.
    bool schedule(usb_lifecycle::ExecutorAction action,
                  usb_lifecycle::Snapshot snapshot) override;

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

#ifndef HID_CONTROL_EXECUTOR_NATIVE_TEST
    static void task_entry(void *context);
    void task_loop();
#endif

    hid_runtime::Runtime *runtime_ = nullptr;
    Backend *backend_ = nullptr;
    bool initialized_ = false;
    std::atomic<ControlOperation> active_operation_{ControlOperation::kNone};

#ifdef HID_CONTROL_EXECUTOR_NATIVE_TEST
    Action native_queue_[2]{};
    std::uint8_t native_head_ = 0;
    std::uint8_t native_count_ = 0;
    bool fail_next_enqueue_ = false;
#endif
};

}  // namespace hid_control_executor
