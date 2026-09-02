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

enum class Transition : std::uint8_t {
    kStable,
    kReleasing,
};

// Opaque route-authority epoch. Consumers may compare only for exact equality
// or inequality; this is not a route-change count or an ordering value.
using Generation = std::uint32_t;

struct Snapshot {
    OutputRoute desired = OutputRoute::kNone;
    OutputRoute active = OutputRoute::kNone;
    Generation generation = 0;
    Transition transition = Transition::kStable;
    bool invalidation_pending = false;
    // False means bounded observation could not prove a cross-field
    // publication. The returned values are still an internally coherent,
    // fail-closed none snapshot and must never be treated as ready.
    bool coherent = true;
};

// This component owns output-route selection and its independent invalidation
// token only. Transport lifecycle, safety state, and report data remain in
// their respective transport/runtime components.
class StateMachine final {
  public:
    StateMachine();

    void initialize_cold_boot();
    // Bounded coherent observation. Readers never spin indefinitely: after a
    // small fixed retry budget, snapshot() returns a coherent fail-closed none
    // view with coherent=false. Accepted command responses still use the
    // immutable snapshot owned by their transition controller.
    Snapshot snapshot() const;
    bool matches(OutputRoute route, Generation generation) const;

    // The compatibility policy is the only U7.2A caller of this transition.
    // It is non-blocking: a callback-side invalidation wins fail-closed.
    bool commit_usb_if_none();

    // Serialized route-controller transitions. begin_usb_release() publishes
    // desired=none/active=usb/releasing without retiring the old generation;
    // complete_usb_release_if_matches() retires it only after safety work.
    bool begin_usb_release(Snapshot *stage_a);
    bool complete_usb_release_if_matches(Snapshot expected);

    // Invalidation is callback-safe and does not wait for a control executor.
    // It closes the unsafe gate before it attempts the committed transition.
    bool invalidate();
    bool invalidate_if_matches(Snapshot expected);

#ifdef HID_ROUTE_NATIVE_TEST
    using GenerationPublishedHook = void (*)(StateMachine &state);
    void set_generation_for_test(Generation generation);
    void set_generation_published_hook_for_test(GenerationPublishedHook hook);
    void set_publication_busy_for_test(bool busy);
#endif

  private:
    void begin_publication();
    void end_publication();
    bool try_enter_writer();
    void leave_writer();
    bool commit_none_locked(OutputRoute expected_route, Generation expected_generation,
                            bool require_exact_match);

    static_assert(std::atomic<Generation>::is_always_lock_free);
    static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
    static_assert(std::atomic<std::uint8_t>::is_always_lock_free);
    std::atomic<std::uint32_t> publication_sequence_{0};
    std::atomic<Generation> generation_{0};
    std::atomic<std::uint8_t> desired_{static_cast<std::uint8_t>(OutputRoute::kNone)};
    std::atomic<std::uint8_t> active_{static_cast<std::uint8_t>(OutputRoute::kNone)};
    std::atomic<std::uint8_t> transition_{static_cast<std::uint8_t>(Transition::kStable)};
    std::atomic_bool writer_active_{false};
    std::atomic_bool invalidation_pending_{false};
#ifdef HID_ROUTE_NATIVE_TEST
    GenerationPublishedHook generation_published_hook_ = nullptr;
#endif
};

}  // namespace hid_route
