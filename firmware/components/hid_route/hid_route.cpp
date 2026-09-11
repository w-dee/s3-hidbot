#include "hid_route/hid_route.hpp"

namespace hid_route {

ExactInvalidationClaim::~ExactInvalidationClaim() { release(); }

bool ExactInvalidationClaim::retire() {
    return owner_ != nullptr && owner_->retire_invalidation_claim(this);
}

void ExactInvalidationClaim::release() {
    if (owner_ != nullptr) {
        owner_->release_invalidation_claim(this);
    }
}

StateMachine::StateMachine() { initialize_cold_boot(); }

void StateMachine::initialize_cold_boot() {
    publication_sequence_.store(0, std::memory_order_release);
    generation_.store(0, std::memory_order_release);
    desired_.store(static_cast<std::uint8_t>(OutputRoute::kNone), std::memory_order_release);
    active_.store(static_cast<std::uint8_t>(OutputRoute::kNone), std::memory_order_release);
    transition_.store(static_cast<std::uint8_t>(Transition::kStable), std::memory_order_release);
    writer_active_.store(false, std::memory_order_release);
    conditional_token_.store(kNoConditionalInvalidationToken,
                             std::memory_order_release);
    conditional_generation_.store(0, std::memory_order_release);
    next_conditional_token_.store(1, std::memory_order_release);
    durable_generation_.store(0, std::memory_order_release);
    durable_state_.store(kDurableNone, std::memory_order_release);
    usb_publication_state_.store(0, std::memory_order_release);
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
            .invalidation_pending = invalidation_pending(),
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
    return !invalidation_pending() &&
           static_cast<OutputRoute>(active_.load(std::memory_order_acquire)) == route &&
           generation_.load(std::memory_order_acquire) == generation;
}

bool StateMachine::invalidation_pending() const {
    return (usb_publication_state_.load(std::memory_order_acquire) &
            (kUsbPublicationActive | kUsbPublicationVeto)) != 0 ||
           invalidation_pending_without_usb_publication();
}

bool StateMachine::invalidation_pending_without_usb_publication() const {
    return conditional_token_.load(std::memory_order_acquire) !=
               kNoConditionalInvalidationToken ||
           durable_state_.load(std::memory_order_acquire) != kDurableNone;
}

UsbPublicationCut StateMachine::advance_usb_publication_serial(
    UsbPublicationCut state) {
    return ((state & kUsbPublicationSerialMask) + 1U) &
           kUsbPublicationSerialMask;
}

UsbPublicationCut StateMachine::usb_publication_cut() const {
    return usb_publication_state_.load(std::memory_order_acquire);
}

void StateMachine::publish_usb_lifecycle_veto() {
    UsbPublicationCut current =
        usb_publication_state_.load(std::memory_order_acquire);
    do {
        const UsbPublicationCut flags =
            current & (kUsbPublicationActive | kUsbPublicationVeto);
        const UsbPublicationCut desired =
            advance_usb_publication_serial(current) | flags |
            ((flags & kUsbPublicationActive) != 0 ? kUsbPublicationVeto : 0U);
        if (usb_publication_state_.compare_exchange_weak(
                current, desired, std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            return;
        }
    } while (true);
}

bool StateMachine::begin_usb_publication(UsbPublicationCut expected_cut,
                                         UsbPublicationCut *identity) {
    if (identity == nullptr ||
        (expected_cut & (kUsbPublicationActive | kUsbPublicationVeto)) != 0) {
        return false;
    }
    const UsbPublicationCut publication =
        advance_usb_publication_serial(expected_cut) | kUsbPublicationActive;
    if (!usb_publication_state_.compare_exchange_strong(
            expected_cut, publication, std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        return false;
    }
    *identity = publication;
    return true;
}

bool StateMachine::finish_usb_publication(UsbPublicationCut identity) {
    UsbPublicationCut expected = identity;
    return usb_publication_state_.compare_exchange_strong(
        expected, advance_usb_publication_serial(identity),
        std::memory_order_acq_rel, std::memory_order_acquire);
}

void StateMachine::clear_usb_publication(UsbPublicationCut identity) {
    // The caller still owns the sole route writer acquired for identity.
    // Lifecycle events may advance its serial, but no later publication can
    // replace the active flag until this owner clears it.
    UsbPublicationCut current =
        usb_publication_state_.load(std::memory_order_acquire);
    while ((current & kUsbPublicationActive) != 0) {
        const UsbPublicationCut desired =
            advance_usb_publication_serial(current);
        if (usb_publication_state_.compare_exchange_weak(
                current, desired, std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            return;
        }
    }
    (void)identity;
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

void StateMachine::clear_superseded_conditional_invalidation_locked() {
    const ConditionalInvalidationToken token =
        conditional_token_.load(std::memory_order_acquire);
    if (token == kNoConditionalInvalidationToken ||
        token == kConditionalPublishing) {
        return;
    }
    const Generation target =
        conditional_generation_.load(std::memory_order_acquire);
    const bool still_exact_usb =
        generation_.load(std::memory_order_acquire) == target &&
        static_cast<OutputRoute>(desired_.load(std::memory_order_acquire)) ==
            OutputRoute::kUsb &&
        static_cast<OutputRoute>(active_.load(std::memory_order_acquire)) ==
            OutputRoute::kUsb &&
        static_cast<Transition>(transition_.load(std::memory_order_acquire)) ==
            Transition::kStable;
    if (!still_exact_usb) {
        ConditionalInvalidationToken expected = token;
        (void)conditional_token_.compare_exchange_strong(
            expected, kNoConditionalInvalidationToken,
            std::memory_order_acq_rel, std::memory_order_acquire);
    }
}

void StateMachine::process_durable_invalidation_locked() {
    if (durable_state_.load(std::memory_order_acquire) != kDurableActive) {
        return;
    }
    const Generation target = durable_generation_.load(std::memory_order_acquire);
    const Generation current = generation_.load(std::memory_order_acquire);
    if (current == target &&
        static_cast<OutputRoute>(active_.load(std::memory_order_acquire)) !=
            OutputRoute::kNone) {
        (void)commit_none_locked(
            static_cast<OutputRoute>(active_.load(std::memory_order_acquire)),
            target, true);
    }
    // The target was either retired above or was already superseded by an
    // authoritative generation change. Only that proof consumes durability.
    if (generation_.load(std::memory_order_acquire) != target ||
        static_cast<OutputRoute>(active_.load(std::memory_order_acquire)) ==
            OutputRoute::kNone) {
        std::uint8_t expected = kDurableActive;
        (void)durable_state_.compare_exchange_strong(
            expected, kDurableNone, std::memory_order_acq_rel,
            std::memory_order_acquire);
    }
}

void StateMachine::leave_writer_with_handoff() {
    process_durable_invalidation_locked();
    clear_superseded_conditional_invalidation_locked();
    leave_writer();
}

bool StateMachine::publish_durable_invalidation(Generation generation) {
    std::uint8_t expected = kDurableNone;
    if (!durable_state_.compare_exchange_strong(
            expected, kDurablePublishing, std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        return expected == kDurableActive &&
               durable_generation_.load(std::memory_order_acquire) == generation;
    }
    durable_generation_.store(generation, std::memory_order_release);
    durable_state_.store(kDurableActive, std::memory_order_release);
    return true;
}

ConditionalInvalidationToken StateMachine::allocate_conditional_token() {
    ConditionalInvalidationToken token =
        next_conditional_token_.fetch_add(1, std::memory_order_relaxed);
    while (token == kNoConditionalInvalidationToken ||
           token == kConditionalPublishing) {
        token = next_conditional_token_.fetch_add(1, std::memory_order_relaxed);
    }
    return token;
}

bool StateMachine::publish_conditional_invalidation(
    Generation generation, ConditionalInvalidationToken *conditional_token) {
    if (conditional_token == nullptr) {
        return false;
    }
    if (*conditional_token != kNoConditionalInvalidationToken) {
        return conditional_invalidation_matches(generation,
                                                *conditional_token);
    }
    ConditionalInvalidationToken expected = kNoConditionalInvalidationToken;
    if (!conditional_token_.compare_exchange_strong(
            expected, kConditionalPublishing, std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        return false;
    }
    const ConditionalInvalidationToken token = allocate_conditional_token();
    conditional_generation_.store(generation, std::memory_order_release);
    conditional_token_.store(token, std::memory_order_release);
    *conditional_token = token;
    return true;
}

bool StateMachine::conditional_invalidation_matches(
    Generation generation,
    ConditionalInvalidationToken conditional_token) const {
    return conditional_token != kNoConditionalInvalidationToken &&
           conditional_token != kConditionalPublishing &&
           conditional_generation_.load(std::memory_order_acquire) == generation &&
           conditional_token_.load(std::memory_order_acquire) == conditional_token;
}

bool StateMachine::commit_if_none(OutputRoute route,
                                  UsbPublicationCut expected_cut) {
    if (route != OutputRoute::kUsb && route != OutputRoute::kBle) {
        return false;
    }
    if (!try_enter_writer()) {
        return false;
    }
    if (invalidation_pending() ||
        static_cast<OutputRoute>(desired_.load(std::memory_order_acquire)) != OutputRoute::kNone ||
        static_cast<OutputRoute>(active_.load(std::memory_order_acquire)) != OutputRoute::kNone ||
        static_cast<Transition>(transition_.load(std::memory_order_acquire)) != Transition::kStable) {
        leave_writer_with_handoff();
        return false;
    }
    UsbPublicationCut usb_publication_identity = 0;
    if (route == OutputRoute::kUsb &&
        !begin_usb_publication(expected_cut, &usb_publication_identity)) {
        leave_writer_with_handoff();
        return false;
    }
    // Publish the new authority epoch before usb becomes usable. The
    // transient is fail-closed because the route remains none until this
    // store. An invalidation that preempts publication intentionally retires
    // this epoch even though no route change commits; never roll it back.
    begin_publication();
    const Generation published_generation =
        generation_.fetch_add(1, std::memory_order_acq_rel) + 1U;
#ifdef HID_ROUTE_NATIVE_TEST
    if (generation_published_hook_ != nullptr) {
        generation_published_hook_(*this);
    }
#endif
    if (invalidation_pending_without_usb_publication() ||
        (route == OutputRoute::kUsb &&
         usb_publication_state_.load(std::memory_order_acquire) !=
             usb_publication_identity)) {
        desired_.store(static_cast<std::uint8_t>(OutputRoute::kNone), std::memory_order_release);
        active_.store(static_cast<std::uint8_t>(OutputRoute::kNone), std::memory_order_release);
        transition_.store(static_cast<std::uint8_t>(Transition::kStable), std::memory_order_release);
        end_publication();
        if (route == OutputRoute::kUsb) {
            clear_usb_publication(usb_publication_identity);
        }
        leave_writer_with_handoff();
        return false;
    }
    desired_.store(static_cast<std::uint8_t>(route), std::memory_order_release);
    active_.store(static_cast<std::uint8_t>(route), std::memory_order_release);
    transition_.store(static_cast<std::uint8_t>(Transition::kStable), std::memory_order_release);
    end_publication();
    if (route == OutputRoute::kUsb) {
#ifdef HID_ROUTE_NATIVE_TEST
        if (usb_publication_before_release_hook_ != nullptr) {
            const WriterAcquiredHook hook =
                usb_publication_before_release_hook_;
            usb_publication_before_release_hook_ = nullptr;
            hook(*this);
        }
#endif
        if (!finish_usb_publication(usb_publication_identity)) {
            (void)commit_none_locked(OutputRoute::kUsb,
                                     published_generation, true);
            clear_usb_publication(usb_publication_identity);
            leave_writer_with_handoff();
            return false;
        }
#ifdef HID_ROUTE_NATIVE_TEST
        if (usb_publication_after_release_hook_ != nullptr) {
            const WriterAcquiredHook hook =
                usb_publication_after_release_hook_;
            usb_publication_after_release_hook_ = nullptr;
            hook(*this);
        }
#endif
    }
    leave_writer_with_handoff();
    const Snapshot committed = snapshot();
    return committed.coherent && !committed.invalidation_pending &&
           committed.desired == route && committed.active == route &&
           committed.generation == published_generation &&
           committed.transition == Transition::kStable;
}

bool StateMachine::commit_usb_if_none() {
    return commit_usb_if_none(usb_publication_cut());
}

bool StateMachine::commit_usb_if_none(UsbPublicationCut expected_cut) {
    return commit_if_none(OutputRoute::kUsb, expected_cut);
}

bool StateMachine::commit_ble_if_none() {
    return commit_if_none(OutputRoute::kBle);
}

bool StateMachine::begin_release(OutputRoute route, Snapshot *stage_a) {
    if (stage_a == nullptr || !try_enter_writer()) {
        return false;
    }
#ifdef HID_ROUTE_NATIVE_TEST
    if (writer_acquired_hook_ != nullptr) {
        const WriterAcquiredHook hook = writer_acquired_hook_;
        writer_acquired_hook_ = nullptr;
        hook(*this);
    }
#endif
    if (invalidation_pending() ||
        static_cast<OutputRoute>(desired_.load(std::memory_order_acquire)) != route ||
        static_cast<OutputRoute>(active_.load(std::memory_order_acquire)) != route ||
        static_cast<Transition>(transition_.load(std::memory_order_acquire)) != Transition::kStable) {
        leave_writer_with_handoff();
        return false;
    }
    begin_publication();
    desired_.store(static_cast<std::uint8_t>(OutputRoute::kNone), std::memory_order_release);
    transition_.store(static_cast<std::uint8_t>(Transition::kReleasing), std::memory_order_release);
    end_publication();
    *stage_a = snapshot();
    leave_writer_with_handoff();
    const Snapshot current = snapshot();
    return stage_a->coherent && current.coherent &&
           !current.invalidation_pending &&
           current.desired == OutputRoute::kNone &&
           current.active == route && current.generation == stage_a->generation &&
           current.transition == Transition::kReleasing;
}

bool StateMachine::complete_release(OutputRoute route, Snapshot expected) {
    if (!try_enter_writer()) {
        return false;
    }
    const bool matches_stage_a =
        !invalidation_pending() &&
        expected.desired == OutputRoute::kNone && expected.active == route &&
        expected.transition == Transition::kReleasing &&
        static_cast<OutputRoute>(desired_.load(std::memory_order_acquire)) == OutputRoute::kNone &&
        static_cast<OutputRoute>(active_.load(std::memory_order_acquire)) == route &&
        static_cast<Transition>(transition_.load(std::memory_order_acquire)) == Transition::kReleasing &&
        generation_.load(std::memory_order_acquire) == expected.generation;
    if (!matches_stage_a) {
        leave_writer_with_handoff();
        return false;
    }
    begin_publication();
    active_.store(static_cast<std::uint8_t>(OutputRoute::kNone), std::memory_order_release);
    generation_.fetch_add(1, std::memory_order_acq_rel);
    transition_.store(static_cast<std::uint8_t>(Transition::kStable), std::memory_order_release);
    end_publication();
    leave_writer_with_handoff();
    if (invalidation_pending()) {
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
    const Generation target = generation_.load(std::memory_order_acquire);
    if (!publish_durable_invalidation(target)) {
        return false;
    }
    if (!try_enter_writer()) {
        return false;
    }
    const bool changed = commit_none_locked(OutputRoute::kUsb, 0, false);
    leave_writer_with_handoff();
    return changed;
}

bool StateMachine::invalidate_if_matches(Snapshot expected) {
    if (!expected.coherent || expected.active == OutputRoute::kNone) {
        return false;
    }
    const Snapshot observed = snapshot();
    if (!observed.coherent || observed.desired != expected.desired ||
        observed.active != expected.active ||
        observed.generation != expected.generation ||
        observed.transition != expected.transition ||
        !publish_durable_invalidation(expected.generation)) {
        return false;
    }
    if (!try_enter_writer()) {
        return false;
    }
    const bool changed = commit_none_locked(expected.active,
                                            expected.generation, true);
    leave_writer_with_handoff();
    return changed;
}

InvalidationClaimResult StateMachine::claim_invalidation_if_matches(
    Snapshot expected, ConditionalInvalidationToken *conditional_token,
    ExactInvalidationClaim *claim) {
    if (conditional_token == nullptr || claim == nullptr || claim->active() ||
        !expected.coherent ||
        expected.active == OutputRoute::kNone) {
        return InvalidationClaimResult::kStale;
    }

    const Snapshot observed = snapshot();
    if (observed.coherent &&
        (observed.desired != expected.desired ||
         observed.active != expected.active ||
         observed.generation != expected.generation ||
         observed.transition != expected.transition)) {
        if (*conditional_token != kNoConditionalInvalidationToken) {
            (void)cancel_conditional_invalidation(expected.generation,
                                                  *conditional_token);
            *conditional_token = kNoConditionalInvalidationToken;
        }
        return InvalidationClaimResult::kStale;
    }

    if (!publish_conditional_invalidation(expected.generation,
                                          conditional_token)) {
        return InvalidationClaimResult::kStale;
    }
    if (!try_enter_writer()) {
        return InvalidationClaimResult::kPending;
    }

    const bool exact =
        static_cast<OutputRoute>(desired_.load(std::memory_order_acquire)) ==
            expected.desired &&
        static_cast<OutputRoute>(active_.load(std::memory_order_acquire)) ==
            expected.active &&
        generation_.load(std::memory_order_acquire) == expected.generation &&
        static_cast<Transition>(transition_.load(std::memory_order_acquire)) ==
            expected.transition;
    if (!exact) {
        (void)cancel_conditional_invalidation(expected.generation,
                                              *conditional_token);
        *conditional_token = kNoConditionalInvalidationToken;
        leave_writer_with_handoff();
        return InvalidationClaimResult::kStale;
    }

    claim->owner_ = this;
    claim->expected_ = expected;
    claim->conditional_token_ = *conditional_token;
    return InvalidationClaimResult::kClaimedExact;
}

bool StateMachine::cancel_conditional_invalidation(
    Generation generation,
    ConditionalInvalidationToken conditional_token) {
    if (!conditional_invalidation_matches(generation, conditional_token)) {
        return false;
    }
    ConditionalInvalidationToken expected = conditional_token;
    return conditional_token_.compare_exchange_strong(
        expected, kNoConditionalInvalidationToken,
        std::memory_order_acq_rel, std::memory_order_acquire);
}

bool StateMachine::retire_invalidation_claim(ExactInvalidationClaim *claim) {
    if (claim == nullptr || claim->owner_ != this) {
        return false;
    }
    // This publication is the exact retirement linearization point. The
    // writer and pending gate remain held across the caller's side effects.
    const bool changed = commit_none_locked(claim->expected_.active,
                                            claim->expected_.generation, true);
    if (changed) {
        (void)cancel_conditional_invalidation(
            claim->expected_.generation, claim->conditional_token_);
        claim->conditional_token_ = kNoConditionalInvalidationToken;
    }
    return changed;
}

void StateMachine::release_invalidation_claim(ExactInvalidationClaim *claim) {
    if (claim == nullptr || claim->owner_ != this) {
        return;
    }
    (void)cancel_conditional_invalidation(claim->expected_.generation,
                                          claim->conditional_token_);
    claim->conditional_token_ = kNoConditionalInvalidationToken;
    claim->owner_ = nullptr;
    leave_writer_with_handoff();
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

void StateMachine::set_writer_acquired_hook_for_test(WriterAcquiredHook hook) {
    writer_acquired_hook_ = hook;
}

void StateMachine::set_usb_publication_before_release_hook_for_test(
    WriterAcquiredHook hook) {
    usb_publication_before_release_hook_ = hook;
}

void StateMachine::set_usb_publication_after_release_hook_for_test(
    WriterAcquiredHook hook) {
    usb_publication_after_release_hook_ = hook;
}

void StateMachine::set_usb_publication_serial_for_test(
    UsbPublicationCut serial) {
    usb_publication_state_.store(serial & kUsbPublicationSerialMask,
                                 std::memory_order_release);
}

void StateMachine::set_publication_busy_for_test(bool busy) {
    publication_sequence_.store(busy ? 1U : 2U, std::memory_order_release);
}
#endif

}  // namespace hid_route
