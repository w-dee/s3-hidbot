#include "hid_route/hid_route.hpp"

namespace hid_route {

StateMachine::StateMachine() { initialize_cold_boot(); }

void StateMachine::initialize_cold_boot() {
    generation_.store(0, std::memory_order_release);
    route_.store(static_cast<std::uint8_t>(OutputRoute::kNone), std::memory_order_release);
    writer_active_.store(false, std::memory_order_release);
    invalidation_pending_.store(false, std::memory_order_release);
}

Snapshot StateMachine::snapshot() const {
    return Snapshot{
        .route = static_cast<OutputRoute>(route_.load(std::memory_order_acquire)),
        .generation = generation_.load(std::memory_order_acquire),
        .invalidation_pending = invalidation_pending_.load(std::memory_order_acquire),
    };
}

bool StateMachine::matches(OutputRoute route, Generation generation) const {
    return !invalidation_pending_.load(std::memory_order_acquire) &&
           static_cast<OutputRoute>(route_.load(std::memory_order_acquire)) == route &&
           generation_.load(std::memory_order_acquire) == generation;
}

bool StateMachine::try_enter_writer() {
    bool expected = false;
    return writer_active_.compare_exchange_strong(expected, true, std::memory_order_acq_rel,
                                                  std::memory_order_acquire);
}

void StateMachine::leave_writer() { writer_active_.store(false, std::memory_order_release); }

bool StateMachine::commit_usb_if_none() {
    if (!try_enter_writer()) {
        return false;
    }
    const Snapshot before = snapshot();
    if (before.invalidation_pending || before.route != OutputRoute::kNone) {
        leave_writer();
        return false;
    }
    // Publish the new authority epoch before usb becomes usable. The
    // transient is fail-closed because the route remains none until this
    // store. An invalidation that preempts publication intentionally retires
    // this epoch even though no route change commits; never roll it back.
    generation_.fetch_add(1, std::memory_order_acq_rel);
#ifdef HID_ROUTE_NATIVE_TEST
    if (generation_published_hook_ != nullptr) {
        generation_published_hook_(*this);
    }
#endif
    if (invalidation_pending_.load(std::memory_order_acquire)) {
        leave_writer();
        (void)invalidate();
        return false;
    }
    route_.store(static_cast<std::uint8_t>(OutputRoute::kUsb), std::memory_order_release);
    leave_writer();
    if (invalidation_pending_.load(std::memory_order_acquire)) {
        (void)invalidate();
        return false;
    }
    return true;
}

bool StateMachine::commit_none_locked(OutputRoute expected_route,
                                     Generation expected_generation,
                                     bool require_exact_match) {
    const OutputRoute current_route =
        static_cast<OutputRoute>(route_.load(std::memory_order_acquire));
    const Generation current_generation = generation_.load(std::memory_order_acquire);
    if (current_route == OutputRoute::kNone) {
        return false;
    }
    if (require_exact_match &&
        (current_route != expected_route || current_generation != expected_generation)) {
        return false;
    }
    // Gate first: no unsafe work from the retired route may pass while the
    // independent generation publication follows.
    route_.store(static_cast<std::uint8_t>(OutputRoute::kNone), std::memory_order_release);
    generation_.fetch_add(1, std::memory_order_acq_rel);
    return true;
}

bool StateMachine::invalidate() {
    invalidation_pending_.store(true, std::memory_order_release);
    if (!try_enter_writer()) {
        return false;
    }
    const bool changed = commit_none_locked(OutputRoute::kUsb, 0, false);
    invalidation_pending_.store(false, std::memory_order_release);
    leave_writer();
    return changed;
}

bool StateMachine::invalidate_if_matches(Snapshot expected) {
    invalidation_pending_.store(true, std::memory_order_release);
    if (!try_enter_writer()) {
        return false;
    }
    const bool changed = commit_none_locked(expected.route, expected.generation, true);
    invalidation_pending_.store(false, std::memory_order_release);
    leave_writer();
    return changed;
}

#ifdef HID_ROUTE_NATIVE_TEST
void StateMachine::set_generation_for_test(Generation generation) {
    generation_.store(generation, std::memory_order_release);
}

void StateMachine::set_generation_published_hook_for_test(GenerationPublishedHook hook) {
    generation_published_hook_ = hook;
}
#endif

}  // namespace hid_route
