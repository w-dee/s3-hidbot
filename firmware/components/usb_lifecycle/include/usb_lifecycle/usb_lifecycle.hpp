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
    kInstallAndConnect,
    kConnect,
    kDisconnect,
};

struct Snapshot {
    DesiredExposure desired = DesiredExposure::kExposed;
    ObservedState observed = ObservedState::kDriverNotInstalled;
    Generation generation = 0;
    Generation uncertainty_generation = 0;
    bool safety_pending = false;
    bool host_release_uncertain = false;
};

// The only future owner of install/connect/disconnect side effects.  U7.1A
// deliberately supplies no hardware implementation; the fake in native tests
// proves transition ordering without calling TinyUSB.
class Executor {
  public:
    virtual ~Executor() = default;
    virtual void schedule(ExecutorAction action, Snapshot snapshot) = 0;
};

class StateMachine {
  public:
    StateMachine();

    // Preserve today's boot policy until U7.1B: USB is intended exposed, but
    // the observed driver state is established only by callbacks.
    void initialize_current_boot_policy();

    // Future control-plane intent. These are internal only in U7.1A.
    void request_attach(Executor &executor);
    void request_detach(Executor &executor);

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
    static constexpr std::uint8_t kObservedShift = 3;

    void set_observed(ObservedState observed);
    ObservedState observed() const;
    DesiredExposure desired() const;
    void advance_generation();

    static_assert(std::atomic<Generation>::is_always_lock_free);
    std::atomic<Generation> generation_{0};
    std::atomic<Generation> uncertainty_generation_{0};
    std::atomic<std::uint8_t> state_{kDesiredBit};
};

}  // namespace usb_lifecycle
