#include "hid_route/hid_route.hpp"

namespace hid_route {

StateMachine::StateMachine() { initialize_cold_boot(); }

void StateMachine::initialize_cold_boot() {
    publication_sequence_.store(0, std::memory_order_release);
    generation_.store(0, std::memory_order_release);
    desired_.store(static_cast<std::uint8_t>(OutputRoute::kNone), std::memory_order_release);
    active_.store(static_cast<std::uint8_t>(OutputRoute::kNone), std::memory_order_release);
    transition_.store(static_cast<std::uint8_t>(Transition::kStable), std::memory_order_release);
    writer_active_.store(false, std::memory_order_release);
    invalidation_pending_.store(false, std::memory_order_release);
}

Snapshot StateMachine::snapshot() const {
    constexpr unsigned kMaxAttempts = 3;
    for (unsigned attempt = 0; attempt < kMaxAttempts; ++attempt) {
        const std::uint32_t before = publication_sequence_.load(std::memory_order_acquire);
        if ((before & 1U) != 0) {
            continue;
        }
        const Snapshot result{
            .desired = static_cast<OutputRoute>(desired_.load(std::memory_order_acquire)),
            .active = static_cast<OutputRoute>(active_.load(std::memory_order_acquire)),
            .generation = generation_.load(std::memory_order_acquire),
            .transition = static_cast<Transition>(transition_.load(std::memory_order_acquire)),
            .invalidation_pending = invalidation_pending_.load(std::memory_order_acquire),
            .coherent = true,
        };
        const std::uint32_t after = publication_sequence_.load(std::memory_order_acquire);
        if (before == after && (after & 1U) == 0) {
            return result;
        }
    }
    return Snapshot{
        .desired = OutputRoute::kNone,
        .active = OutputRoute::kNone,
        .generation = generation_.load(std::memory_order_acquire),
        .transition = Transition::kStable,
        .invalidation_pending = true,
        .coherent = false,
    };
}

bool StateMachine::matches(OutputRoute route, Generation generation) const {
    return !invalidation_pending_.load(std::memory_order_acquire) &&
           static_cast<OutputRoute>(active_.load(std::memory_order_acquire)) == route &&
           generation_.load(std::memory_order_acquire) == generation;
}

void StateMachine::begin_publication() {
    publication_sequence_.fetch_add(1, std::memory_order_acq_rel);
}

void StateMachine::end_publication() {
    publication_sequence_.fetch_add(1, std::memory_order_release);
}

bool StateMachine::try_enter_writer() {
    bool expected = false;
    return writer_active_.compare_exchange_strong(expected, true, std::memory_order_acq_rel,
                                                  std::memory_order_acquire);
}

void StateMachine::leave_writer() { writer_active_.store(false, std::memory_order_release); }

bool StateMachine::commit_if_none(OutputRoute route) {
    if (route != OutputRoute::kUsb && route != OutputRoute::kBle) {
        return false;
    }
    if (!try_enter_writer()) {
        return false;
    }
    if (invalidation_pending_.load(std::memory_order_acquire) ||
        static_cast<OutputRoute>(desired_.load(std::memory_order_acquire)) != OutputRoute::kNone ||
        static_cast<OutputRoute>(active_.load(std::memory_order_acquire)) != OutputRoute::kNone ||
        static_cast<Transition>(transition_.load(std::memory_order_acquire)) != Transition::kStable) {
        leave_writer();
        return false;
    }
    // Publish the new authority epoch before usb becomes usable. The
    // transient is fail-closed because the route remains none until this
    // store. An invalidation that preempts publication intentionally retires
    // this epoch even though no route change commits; never roll it back.
    begin_publication();
    generation_.fetch_add(1, std::memory_order_acq_rel);
#ifdef HID_ROUTE_NATIVE_TEST
    if (generation_published_hook_ != nullptr) {
        generation_published_hook_(*this);
    }
#endif
    if (invalidation_pending_.load(std::memory_order_acquire)) {
        desired_.store(static_cast<std::uint8_t>(OutputRoute::kNone), std::memory_order_release);
        active_.store(static_cast<std::uint8_t>(OutputRoute::kNone), std::memory_order_release);
        transition_.store(static_cast<std::uint8_t>(Transition::kStable), std::memory_order_release);
        end_publication();
        leave_writer();
        (void)invalidate();
        return false;
    }
    desired_.store(static_cast<std::uint8_t>(route), std::memory_order_release);
    active_.store(static_cast<std::uint8_t>(route), std::memory_order_release);
    transition_.store(static_cast<std::uint8_t>(Transition::kStable), std::memory_order_release);
    end_publication();
    leave_writer();
    if (invalidation_pending_.load(std::memory_order_acquire)) {
        (void)invalidate();
        return false;
    }
    return true;
}

bool StateMachine::commit_usb_if_none() {
    return commit_if_none(OutputRoute::kUsb);
}

bool StateMachine::commit_ble_if_none() {
    return commit_if_none(OutputRoute::kBle);
}

bool StateMachine::begin_release(OutputRoute route, Snapshot *stage_a) {
    if (stage_a == nullptr || !try_enter_writer()) {
        return false;
    }
    if (invalidation_pending_.load(std::memory_order_acquire) ||
        static_cast<OutputRoute>(desired_.load(std::memory_order_acquire)) != route ||
        static_cast<OutputRoute>(active_.load(std::memory_order_acquire)) != route ||
        static_cast<Transition>(transition_.load(std::memory_order_acquire)) != Transition::kStable) {
        leave_writer();
        return false;
    }
    begin_publication();
    desired_.store(static_cast<std::uint8_t>(OutputRoute::kNone), std::memory_order_release);
    transition_.store(static_cast<std::uint8_t>(Transition::kReleasing), std::memory_order_release);
    end_publication();
    *stage_a = snapshot();
    leave_writer();
    if (invalidation_pending_.load(std::memory_order_acquire)) {
        (void)invalidate();
        return false;
    }
    return stage_a->coherent && stage_a->desired == OutputRoute::kNone &&
           stage_a->active == route &&
           stage_a->transition == Transition::kReleasing;
}

bool StateMachine::complete_release(OutputRoute route, Snapshot expected) {
    if (!try_enter_writer()) {
        return false;
    }
    const bool matches_stage_a =
        !invalidation_pending_.load(std::memory_order_acquire) &&
        expected.desired == OutputRoute::kNone && expected.active == route &&
        expected.transition == Transition::kReleasing &&
        static_cast<OutputRoute>(desired_.load(std::memory_order_acquire)) == OutputRoute::kNone &&
        static_cast<OutputRoute>(active_.load(std::memory_order_acquire)) == route &&
        static_cast<Transition>(transition_.load(std::memory_order_acquire)) == Transition::kReleasing &&
        generation_.load(std::memory_order_acquire) == expected.generation;
    if (!matches_stage_a) {
        leave_writer();
        return false;
    }
    begin_publication();
    active_.store(static_cast<std::uint8_t>(OutputRoute::kNone), std::memory_order_release);
    generation_.fetch_add(1, std::memory_order_acq_rel);
    transition_.store(static_cast<std::uint8_t>(Transition::kStable), std::memory_order_release);
    end_publication();
    leave_writer();
    if (invalidation_pending_.load(std::memory_order_acquire)) {
        (void)invalidate();
        return false;
    }
    return true;
}

bool StateMachine::begin_usb_release(Snapshot *stage_a) {
    return begin_release(OutputRoute::kUsb, stage_a);
}

bool StateMachine::complete_usb_release_if_matches(Snapshot expected) {
    return complete_release(OutputRoute::kUsb, expected);
}

bool StateMachine::begin_ble_release(Snapshot *stage_a) {
    return begin_release(OutputRoute::kBle, stage_a);
}

bool StateMachine::complete_ble_release_if_matches(Snapshot expected) {
    return complete_release(OutputRoute::kBle, expected);
}

bool StateMachine::commit_none_locked(OutputRoute expected_route,
                                     Generation expected_generation,
                                     bool require_exact_match) {
    const OutputRoute current_route =
        static_cast<OutputRoute>(active_.load(std::memory_order_acquire));
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
    begin_publication();
    desired_.store(static_cast<std::uint8_t>(OutputRoute::kNone), std::memory_order_release);
    active_.store(static_cast<std::uint8_t>(OutputRoute::kNone), std::memory_order_release);
    generation_.fetch_add(1, std::memory_order_acq_rel);
    transition_.store(static_cast<std::uint8_t>(Transition::kStable), std::memory_order_release);
    end_publication();
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
    const bool changed = commit_none_locked(expected.active, expected.generation, true);
    invalidation_pending_.store(false, std::memory_order_release);
    leave_writer();
    return changed;
}

#ifdef HID_ROUTE_NATIVE_TEST
void StateMachine::set_generation_for_test(Generation generation) {
    begin_publication();
    generation_.store(generation, std::memory_order_release);
    end_publication();
}

void StateMachine::set_generation_published_hook_for_test(GenerationPublishedHook hook) {
    generation_published_hook_ = hook;
}

void StateMachine::set_publication_busy_for_test(bool busy) {
    publication_sequence_.store(busy ? 1U : 2U, std::memory_order_release);
}
#endif

}  // namespace hid_route
