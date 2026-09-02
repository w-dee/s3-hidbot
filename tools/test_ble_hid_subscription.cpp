#include <cassert>

#include "ble_hid_service/subscription_state.hpp"

int main() {
    ble_hid_service::SubscriptionState state;
    assert(!state.keyboard());
    assert(!state.mouse());

    state.observe(10, 10, 20, true);
    assert(state.keyboard());
    assert(!state.mouse());
    state.observe(20, 10, 20, true);
    assert(state.keyboard());
    assert(state.mouse());

    state.observe(10, 10, 20, false);
    assert(!state.keyboard());
    assert(state.mouse());
    state.clear();
    assert(!state.keyboard());
    assert(!state.mouse());
}
