#include <cassert>
#include <cstdint>

#include "hid_route/hid_route.hpp"

namespace {

bool nested_invalidation_result = true;

void invalidate_after_generation_publish(hid_route::StateMachine &state) {
    nested_invalidation_result = state.invalidate();
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
    test_generation_wrap_uses_exact_match_not_ordering();
    test_pre_publish_invalidation_aborts_but_consumes_authority_epoch();
    test_pre_publish_invalidation_abort_wraps_modulo_uint32();
    test_usb_release_has_coherent_stage_a_and_final_publications();
    test_bounded_snapshot_falls_back_fail_closed();
}
