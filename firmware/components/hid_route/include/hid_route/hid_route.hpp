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
    void set_generation_for_test(Generation generation);
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
};

}  // namespace hid_route
