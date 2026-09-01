#include <cassert>
#include <cstdint>

#include "hid_route/hid_route.hpp"

namespace {

void test_cold_boot_is_none_generation_zero() {
    hid_route::StateMachine state;
    const hid_route::Snapshot snapshot = state.snapshot();
    assert(snapshot.route == hid_route::OutputRoute::kNone);
    assert(snapshot.generation == 0);
    assert(!snapshot.invalidation_pending);
}

void test_commit_and_invalidation_change_generation_exactly_once() {
    hid_route::StateMachine state;
    assert(state.commit_usb_if_none());
    const hid_route::Snapshot usb = state.snapshot();
    assert(usb.route == hid_route::OutputRoute::kUsb);
    assert(usb.generation == 1);
    assert(state.matches(hid_route::OutputRoute::kUsb, usb.generation));

    assert(!state.commit_usb_if_none());
    assert(state.snapshot().generation == usb.generation);
    assert(state.invalidate_if_matches(usb));
    const hid_route::Snapshot none = state.snapshot();
    assert(none.route == hid_route::OutputRoute::kNone);
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
        .route = hid_route::OutputRoute::kUsb,
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
    assert(wrapped_usb.route == hid_route::OutputRoute::kUsb);
    assert(wrapped_usb.generation == 0);
    assert(state.invalidate_if_matches(wrapped_usb));
    assert(state.snapshot().generation == 1);
}

}  // namespace

int main() {
    test_cold_boot_is_none_generation_zero();
    test_commit_and_invalidation_change_generation_exactly_once();
    test_exact_identity_rejects_stale_invalidation();
    test_generation_wrap_uses_exact_match_not_ordering();
}
