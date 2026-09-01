#pragma once

#include <cstdint>

#include "hid_runtime/hid_runtime.hpp"
#include "usb_lifecycle/usb_lifecycle.hpp"

namespace usb_exposure_control {

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

// Only Controller's dedicated lifecycle task invokes this backend. The
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

class Controller final : public usb_lifecycle::Executor {
  public:
    struct Action {
        usb_lifecycle::ExecutorAction kind = usb_lifecycle::ExecutorAction::kInstall;
        usb_lifecycle::Snapshot snapshot{};
    };

    bool initialize(hid_runtime::Runtime *runtime, Backend *backend);

    usb_lifecycle::TransitionResult request_attach();
    usb_lifecycle::TransitionResult request_detach();
    ExposureSnapshot snapshot() const;

    // usb_lifecycle::Executor. Calls originate in the UART/control task and
    // are stored in a fixed, serialized action queue.
    bool schedule(usb_lifecycle::ExecutorAction action,
                  usb_lifecycle::Snapshot snapshot) override;

#ifdef USB_EXPOSURE_CONTROL_NATIVE_TEST
    bool process_one_for_test();
#endif

  private:
    void process(Action action);

#ifndef USB_EXPOSURE_CONTROL_NATIVE_TEST
    static void task_entry(void *context);
    void task_loop();
#endif

    hid_runtime::Runtime *runtime_ = nullptr;
    Backend *backend_ = nullptr;
    bool initialized_ = false;

#ifdef USB_EXPOSURE_CONTROL_NATIVE_TEST
    Action native_queue_[2]{};
    std::uint8_t native_head_ = 0;
    std::uint8_t native_count_ = 0;
#endif
};

}  // namespace usb_exposure_control
