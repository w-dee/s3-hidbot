#pragma once

#include <atomic>
#include <cstdint>

namespace usb_lifecycle {

// ESP32-S3 provides lock-free 32-bit atomics, whereas a 64-bit atomic would
// require an emulation lock in a lifecycle callback.  The generation is always
// paired with the independently advancing authority epoch; native tests cover
// the defined unsigned wrap boundary.
using Generation = std::uint32_t;

enum class DesiredExposure : std::uint8_t {
    kHidden,
    kExposed,
};

enum class ObservedState : std::uint8_t {
    kDriverNotInstalled,
    kDisconnected,
    kAttaching,
    kMounted,
    kSuspended,
    kDetaching,
};

enum class ExecutorAction : std::uint8_t {
    kInstall,
    kUninstall,
};

enum class TransitionResult : std::uint8_t {
    kAccepted,
    kNoOp,
    kBusy,
};

enum class LifecycleOperation : std::uint8_t {
    kInstall,
    kUninstall,
};

struct LastError {
    bool present = false;
    LifecycleOperation operation = LifecycleOperation::kInstall;
    std::int32_t code = 0;
};

struct Snapshot {
    DesiredExposure desired = DesiredExposure::kExposed;
    ObservedState observed = ObservedState::kDriverNotInstalled;
    Generation generation = 0;
    Generation uncertainty_generation = 0;
    bool safety_pending = false;
    bool host_release_uncertain = false;
    bool recovery_required = false;
    LastError last_error{};
};

// The lifecycle state captured when a transition has been accepted for
// execution.  The snapshot is intentionally copied before the executor can
// observe the action, so a public command response never depends on later
// install/uninstall progress.
struct TransitionOutcome {
    TransitionResult action_result = TransitionResult::kBusy;
    bool snapshot_valid = false;
    Snapshot snapshot{};
};

// The only future owner of install/connect/disconnect side effects.  U7.1A
// deliberately supplies no hardware implementation; the fake in native tests
// proves transition ordering without calling TinyUSB.
class Executor {
  public:
    virtual ~Executor() = default;
    // Returns false only when a bounded executor cannot accept the action.
    // StateMachine never silently drops an accepted lifecycle action.
    virtual bool schedule(ExecutorAction action, Snapshot snapshot) = 0;
};

class StateMachine {
  public:
    StateMachine();

    // U7.1B begins with no native USB stack. UART remains independently
    // available through the CH343 path.
    void initialize_hidden_boot_policy();

    // Future control-plane intent. These are internal only in U7.1A.
    TransitionOutcome request_attach(Executor &executor);
    TransitionOutcome request_detach(Executor &executor);

    // Completion is owned by the one lifecycle side-effect executor. Install
    // success means the driver is present, not that a host configured it.
    void complete_install_success();
    void complete_install_clean_failure(std::int32_t error_code);
    void complete_install_ambiguous_failure(std::int32_t error_code);

    // This is the Stage-B teardown linearization point. It advances exactly
    // once for an accepted detach, immediately before public driver uninstall.
    Generation begin_uninstall();
    void complete_uninstall_success();
    void complete_uninstall_failure(std::int32_t error_code);

    // TinyUSB callbacks reconcile observations. They never change desired
    // intent. A false return means an obsolete observation was ignored.
    bool observe_mount();
    bool observe_unmount();
    bool observe_suspend();
    bool observe_resume();

    // Future detach safety accounting. Hidden never implies this proof.
    void mark_release_pending();
    void mark_release_confirmed();
    void mark_release_uncertain();
    void mark_release_uncertain_for_generation(Generation generation);

    Snapshot snapshot() const;
    Generation generation() const;
    bool accepts_hid(bool endpoint_ready) const;
    bool has_unresolved_prior_generation() const;

#ifdef USB_LIFECYCLE_NATIVE_TEST
    void set_generation_for_test(Generation generation);
#endif

  private:
    static constexpr std::uint8_t kDesiredBit = 1U << 0;
    static constexpr std::uint8_t kSafetyPendingBit = 1U << 1;
    static constexpr std::uint8_t kUncertainBit = 1U << 2;
    static constexpr std::uint8_t kRecoveryRequiredBit = 1U << 3;
    static constexpr std::uint8_t kObservedShift = 4;

    void set_observed(ObservedState observed);
    ObservedState observed() const;
    DesiredExposure desired() const;
    void advance_generation();
    void clear_last_error();
    void record_error(LifecycleOperation operation, std::int32_t error_code);
    void set_recovery_required(bool required);

    static_assert(std::atomic<Generation>::is_always_lock_free);
    std::atomic<Generation> generation_{0};
    std::atomic<Generation> uncertainty_generation_{0};
    std::atomic<std::int32_t> last_error_code_{0};
    std::atomic<std::uint8_t> last_error_operation_{0};
    std::atomic_bool last_error_present_{false};
    std::atomic_bool teardown_boundary_started_{false};
    std::atomic<std::uint8_t> state_{0};
};

}  // namespace usb_lifecycle
