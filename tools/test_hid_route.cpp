#include <cassert>
#include <cstdint>

#include "hid_route/hid_route.hpp"

namespace {

bool nested_invalidation_result = true;
hid_route::InvalidationClaimResult nested_claim_result =
    hid_route::InvalidationClaimResult::kStale;
hid_route::Snapshot pending_expected{};
hid_route::ConditionalInvalidationToken pending_token =
    hid_route::kNoConditionalInvalidationToken;

void invalidate_after_generation_publish(hid_route::StateMachine &state) {
    nested_invalidation_result = state.invalidate();
}

void claim_after_generation_publish(hid_route::StateMachine &state) {
    hid_route::ExactInvalidationClaim claim;
    nested_claim_result =
        state.claim_invalidation_if_matches(pending_expected, &pending_token,
                                            &claim);
    assert(!claim.active());
}

void test_cold_boot_is_none_generation_zero() {
    hid_route::StateMachine state;
    const hid_route::Snapshot snapshot = state.snapshot();
    assert(snapshot.desired == hid_route::OutputRoute::kNone);
    assert(snapshot.active == hid_route::OutputRoute::kNone);
    assert(snapshot.transition == hid_route::Transition::kStable);
    assert(snapshot.coherent);
    assert(snapshot.generation == 0);
    assert(!snapshot.invalidation_pending);
}

void test_commit_and_invalidation_change_generation_exactly_once() {
    hid_route::StateMachine state;
    assert(state.commit_usb_if_none());
    const hid_route::Snapshot usb = state.snapshot();
    assert(usb.desired == hid_route::OutputRoute::kUsb);
    assert(usb.active == hid_route::OutputRoute::kUsb);
    assert(usb.generation == 1);
    assert(state.matches(hid_route::OutputRoute::kUsb, usb.generation));

    assert(!state.commit_usb_if_none());
    assert(state.snapshot().generation == usb.generation);
    assert(state.invalidate_if_matches(usb));
    const hid_route::Snapshot none = state.snapshot();
    assert(none.desired == hid_route::OutputRoute::kNone);
    assert(none.active == hid_route::OutputRoute::kNone);
    assert(none.generation == 2);
    assert(!state.matches(hid_route::OutputRoute::kUsb, usb.generation));

    assert(!state.invalidate());
    assert(state.snapshot().generation == none.generation);
}

void test_exact_identity_rejects_stale_invalidation() {
    hid_route::StateMachine state;
    assert(state.commit_usb_if_none());
    const hid_route::Snapshot current = state.snapshot();
    const hid_route::Snapshot stale{
        .desired = hid_route::OutputRoute::kUsb,
        .active = hid_route::OutputRoute::kUsb,
        .generation = current.generation + 1,
    };
    assert(!state.invalidate_if_matches(stale));
    assert(state.matches(hid_route::OutputRoute::kUsb, current.generation));
}

void test_exact_claim_holds_admission_closed_through_retirement() {
    hid_route::StateMachine state;
    assert(state.commit_usb_if_none());
    const auto usb = state.snapshot();
    hid_route::ExactInvalidationClaim claim;
    hid_route::ConditionalInvalidationToken token =
        hid_route::kNoConditionalInvalidationToken;
    assert(state.claim_invalidation_if_matches(usb, &token, &claim) ==
           hid_route::InvalidationClaimResult::kClaimedExact);
    assert(claim.active());
    assert(state.snapshot().invalidation_pending);
    assert(!state.commit_usb_if_none());
    assert(claim.retire());
    const auto retired = state.snapshot();
    assert(retired.active == hid_route::OutputRoute::kNone);
    assert(retired.generation == usb.generation + 1U);
    assert(!retired.invalidation_pending);
    claim.release();
    assert(!state.snapshot().invalidation_pending);

    hid_route::ExactInvalidationClaim stale_claim;
    token = hid_route::kNoConditionalInvalidationToken;
    assert(state.claim_invalidation_if_matches(usb, &token, &stale_claim) ==
           hid_route::InvalidationClaimResult::kStale);
    assert(!stale_claim.active());
}

void test_exact_claim_writer_conflict_is_pending_and_resolves_fail_closed() {
    hid_route::StateMachine state;
    state.set_generation_for_test(41);
    pending_expected = hid_route::Snapshot{
        .desired = hid_route::OutputRoute::kUsb,
        .active = hid_route::OutputRoute::kUsb,
        .generation = 42,
        .transition = hid_route::Transition::kStable,
    };
    nested_claim_result = hid_route::InvalidationClaimResult::kStale;
    pending_token = hid_route::kNoConditionalInvalidationToken;
    state.set_generation_published_hook_for_test(
        claim_after_generation_publish);
    assert(!state.commit_usb_if_none());
    state.set_generation_published_hook_for_test(nullptr);
    assert(nested_claim_result ==
           hid_route::InvalidationClaimResult::kPending);
    const auto final = state.snapshot();
    assert(final.coherent);
    assert(!final.invalidation_pending);
    assert(final.active == hid_route::OutputRoute::kNone);
    // The in-progress USB publication consumes generation 42, then observes
    // the pending gate and aborts before exposing the route.
    assert(final.generation == 42);
}

void test_conditional_pending_requires_exact_generation_and_owner_token() {
    hid_route::StateMachine state;
    assert(state.commit_usb_if_none());
    pending_expected = state.snapshot();
    pending_token = hid_route::kNoConditionalInvalidationToken;
    nested_claim_result = hid_route::InvalidationClaimResult::kStale;
    state.set_writer_acquired_hook_for_test(claim_after_generation_publish);
    hid_route::Snapshot unused_stage{};
    assert(!state.begin_usb_release(&unused_stage));
    assert(nested_claim_result == hid_route::InvalidationClaimResult::kPending);
    assert(pending_token != hid_route::kNoConditionalInvalidationToken);
    assert(state.snapshot().invalidation_pending);
    assert(!state.cancel_conditional_invalidation(
        pending_expected.generation + 1U, pending_token));
    assert(!state.cancel_conditional_invalidation(
        pending_expected.generation, pending_token + 1U));
    assert(state.snapshot().invalidation_pending);

    hid_route::ExactInvalidationClaim claim;
    assert(state.claim_invalidation_if_matches(pending_expected, &pending_token,
                                               &claim) ==
           hid_route::InvalidationClaimResult::kClaimedExact);
    assert(claim.retire());
    claim.release();
    const auto retired = state.snapshot();
    assert(retired.active == hid_route::OutputRoute::kNone);
    assert(retired.generation == pending_expected.generation + 1U);
    assert(!retired.invalidation_pending);
}

void test_internal_ble_commit_is_single_route_authority() {
    hid_route::StateMachine state;
    assert(state.commit_ble_if_none());
    const auto ble = state.snapshot();
    assert(ble.desired == hid_route::OutputRoute::kBle);
    assert(ble.active == hid_route::OutputRoute::kBle);
    assert(ble.transition == hid_route::Transition::kStable);
    assert(state.matches(hid_route::OutputRoute::kBle, ble.generation));
    assert(!state.commit_usb_if_none());
    assert(!state.commit_ble_if_none());
    assert(state.invalidate_if_matches(ble));
    assert(state.snapshot().active == hid_route::OutputRoute::kNone);
}

void test_generation_wrap_uses_exact_match_not_ordering() {
    hid_route::StateMachine state;
    state.set_generation_for_test(UINT32_MAX);
    assert(state.commit_usb_if_none());
    const hid_route::Snapshot wrapped_usb = state.snapshot();
    assert(wrapped_usb.active == hid_route::OutputRoute::kUsb);
    assert(wrapped_usb.generation == 0);
    assert(state.invalidate_if_matches(wrapped_usb));
    assert(state.snapshot().generation == 1);
}

void test_usb_publication_cut_wrap_does_not_poison_fresh_selection() {
    hid_route::StateMachine state;
    constexpr hid_route::UsbPublicationCut kLastSerial =
        (hid_route::UsbPublicationCut{1} << 30U) - 1U;
    state.set_usb_publication_serial_for_test(kLastSerial);
    const auto stale_cut = state.usb_publication_cut();

    state.publish_usb_lifecycle_veto();
    assert(!state.commit_usb_if_none(stale_cut));
    assert(state.snapshot().active == hid_route::OutputRoute::kNone);
    assert(!state.snapshot().invalidation_pending);

    const auto fresh_cut = state.usb_publication_cut();
    assert(fresh_cut == 0);
    assert(state.commit_usb_if_none(fresh_cut));
    const auto usb = state.snapshot();
    assert(usb.active == hid_route::OutputRoute::kUsb);
    assert(!usb.invalidation_pending);
}

void verify_pre_publish_invalidation_abort(hid_route::Generation initial,
                                           hid_route::Generation expected) {
    hid_route::StateMachine state;
    state.set_generation_for_test(initial);
    state.set_generation_published_hook_for_test(invalidate_after_generation_publish);
    nested_invalidation_result = true;

    assert(!state.commit_usb_if_none());
    assert(!nested_invalidation_result);
    const hid_route::Snapshot final = state.snapshot();
    assert(final.active == hid_route::OutputRoute::kNone);
    assert(final.generation == expected);
    assert(!final.invalidation_pending);
    assert(!state.matches(hid_route::OutputRoute::kUsb, initial));
    assert(!state.matches(hid_route::OutputRoute::kUsb, expected));
}

void test_pre_publish_invalidation_aborts_but_consumes_authority_epoch() {
    // snapshot() is an atomic-field observation, so none plus the newly
    // advanced authority epoch is a valid transient and final fail-closed
    // result. The aborted transition must not roll back or increment twice.
    verify_pre_publish_invalidation_abort(41, 42);
}

void test_pre_publish_invalidation_abort_wraps_modulo_uint32() {
    verify_pre_publish_invalidation_abort(UINT32_MAX, 0);
}

void test_usb_release_has_coherent_stage_a_and_final_publications() {
    hid_route::StateMachine state;
    assert(state.commit_usb_if_none());
    hid_route::Snapshot stage_a{};
    assert(state.begin_usb_release(&stage_a));
    assert(stage_a.coherent);
    assert(stage_a.desired == hid_route::OutputRoute::kNone);
    assert(stage_a.active == hid_route::OutputRoute::kUsb);
    assert(stage_a.transition == hid_route::Transition::kReleasing);
    assert(stage_a.generation == 1);
    assert(state.complete_usb_release_if_matches(stage_a));
    const auto final = state.snapshot();
    assert(final.coherent);
    assert(final.desired == hid_route::OutputRoute::kNone);
    assert(final.active == hid_route::OutputRoute::kNone);
    assert(final.transition == hid_route::Transition::kStable);
    assert(final.generation == 2);
}

void test_ble_release_has_coherent_stage_a_and_exact_completion() {
    hid_route::StateMachine state;
    assert(state.commit_ble_if_none());
    hid_route::Snapshot stage_a{};
    assert(state.begin_ble_release(&stage_a));
    assert(stage_a.desired == hid_route::OutputRoute::kNone);
    assert(stage_a.active == hid_route::OutputRoute::kBle);
    assert(stage_a.transition == hid_route::Transition::kReleasing);
    const auto stale = hid_route::Snapshot{.desired = hid_route::OutputRoute::kNone,
                                           .active = hid_route::OutputRoute::kBle,
                                           .generation = stage_a.generation + 1,
                                           .transition = hid_route::Transition::kReleasing};
    assert(!state.complete_ble_release_if_matches(stale));
    assert(state.complete_ble_release_if_matches(stage_a));
    const auto none = state.snapshot();
    assert(none.active == hid_route::OutputRoute::kNone);
    assert(none.transition == hid_route::Transition::kStable);
    assert(none.generation == stage_a.generation + 1);
}

void test_bounded_snapshot_falls_back_fail_closed() {
    hid_route::StateMachine state;
    assert(state.commit_usb_if_none());
    state.set_publication_busy_for_test(true);
    const auto fallback = state.snapshot();
    assert(!fallback.coherent);
    assert(fallback.invalidation_pending);
    assert(fallback.desired == hid_route::OutputRoute::kNone);
    assert(fallback.active == hid_route::OutputRoute::kNone);
    assert(fallback.transition == hid_route::Transition::kStable);
    state.set_publication_busy_for_test(false);
    assert(state.snapshot().coherent);
}

}  // namespace

int main() {
    test_cold_boot_is_none_generation_zero();
    test_commit_and_invalidation_change_generation_exactly_once();
    test_exact_identity_rejects_stale_invalidation();
    test_exact_claim_holds_admission_closed_through_retirement();
    test_exact_claim_writer_conflict_is_pending_and_resolves_fail_closed();
    test_conditional_pending_requires_exact_generation_and_owner_token();
    test_internal_ble_commit_is_single_route_authority();
    test_generation_wrap_uses_exact_match_not_ordering();
    test_usb_publication_cut_wrap_does_not_poison_fresh_selection();
    test_pre_publish_invalidation_aborts_but_consumes_authority_epoch();
    test_pre_publish_invalidation_abort_wraps_modulo_uint32();
    test_usb_release_has_coherent_stage_a_and_final_publications();
    test_ble_release_has_coherent_stage_a_and_exact_completion();
    test_bounded_snapshot_falls_back_fail_closed();
}
