#pragma once

#include <atomic>
#include <cstdint>

namespace hid_route {

enum class OutputRoute : std::uint8_t {
    kNone,
    kUsb,
    // Placeholder only. U7.2A supplies no BLE adapter or public behavior.
    kBle,
};

// Opaque route-authority epoch. Consumers may compare only for exact equality
// or inequality; this is not a route-change count or an ordering value.
using Generation = std::uint32_t;

struct Snapshot {
    OutputRoute route = OutputRoute::kNone;
    Generation generation = 0;
    bool invalidation_pending = false;
};

// This component owns output-route selection and its independent invalidation
// token only. Transport lifecycle, safety state, and report data remain in
// their respective transport/runtime components.
class StateMachine final {
  public:
    StateMachine();

    void initialize_cold_boot();
    // Atomic-field observation for internal fail-closed checks. This is not a
    // coherent transaction snapshot for a future public wire contract;
    // transient combinations such as none plus a newly advanced epoch are
    // valid. A future accepted route response must use its controller-owned
    // immutable transaction snapshot, not a later live snapshot().
    Snapshot snapshot() const;
    bool matches(OutputRoute route, Generation generation) const;

    // The compatibility policy is the only U7.2A caller of this transition.
    // It is non-blocking: a callback-side invalidation wins fail-closed.
    bool commit_usb_if_none();

    // Invalidation is callback-safe and does not wait for a control executor.
    // It closes the unsafe gate before it attempts the committed transition.
    bool invalidate();
    bool invalidate_if_matches(Snapshot expected);

#ifdef HID_ROUTE_NATIVE_TEST
    using GenerationPublishedHook = void (*)(StateMachine &state);
    void set_generation_for_test(Generation generation);
    void set_generation_published_hook_for_test(GenerationPublishedHook hook);
#endif

  private:
    bool try_enter_writer();
    void leave_writer();
    bool commit_none_locked(OutputRoute expected_route, Generation expected_generation,
                            bool require_exact_match);

    static_assert(std::atomic<Generation>::is_always_lock_free);
    static_assert(std::atomic<std::uint8_t>::is_always_lock_free);
    std::atomic<Generation> generation_{0};
    std::atomic<std::uint8_t> route_{static_cast<std::uint8_t>(OutputRoute::kNone)};
    std::atomic_bool writer_active_{false};
    std::atomic_bool invalidation_pending_{false};
#ifdef HID_ROUTE_NATIVE_TEST
    GenerationPublishedHook generation_published_hook_ = nullptr;
#endif
};

}  // namespace hid_route
