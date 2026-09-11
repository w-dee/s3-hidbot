#include <array>
#include <cassert>
#include <cstdint>
#include <limits>
#include <string_view>

#include "control_session/control_session.hpp"

namespace {

struct Clock {
    std::uint64_t value = 0;
    static std::uint64_t now(void *context) {
        return static_cast<Clock *>(context)->value;
    }
};

void random_fill(void *, std::uint8_t *output, std::size_t length) {
    for (std::size_t index = 0; index < length; ++index) {
        output[index] = static_cast<std::uint8_t>(index);
    }
}

constexpr char kNonce[] = "0123456789abcdef0123456789abcdef";
constexpr char kRequest[] = "{\"hello\":true}";
constexpr char kSession[] = "abcdef0123456789abcdef0123456789";
constexpr control_session::AuthorityEpoch kEpochOne = 10;
constexpr control_session::AuthorityEpoch kEpochTwo = 11;

void test_lease_lifecycle() {
    Clock clock;
    control_session::State state;
    state.initialize(random_fill, nullptr, Clock::now, &clock);
    control_session::ResponseFrame response{};
    assert(state.activate_hello(kNonce, kRequest, kSession, kEpochOne, response));
    assert(state.local_owner_id() == 1);
    assert(state.has_active_session());

    clock.value = control_session::kLeaseMicroseconds - 1;
    assert(!state.service_lease());
    assert(state.has_active_session());
    assert(state.refresh_lease());
    clock.value += control_session::kLeaseMicroseconds - 1;
    assert(!state.service_lease());
    clock.value += 1;
    assert(state.service_lease());
    assert(!state.has_active_session());
    assert(!state.refresh_lease());
}

void test_takeover_clears_retry_authority() {
    control_session::State state;
    state.initialize(random_fill, nullptr);
    control_session::ResponseFrame response{};
    assert(state.activate_hello(kNonce, kRequest, kSession, kEpochOne, response));
    state.cache_completed_request(1, "{\"ping\":true}", kEpochOne, response);
    state.revoke_for_takeover();
    assert(!state.has_active_session());
    const control_session::ResponseFrame *cached = nullptr;
    assert(state.inspect_request(kSession, 1, "{\"ping\":true}", kEpochOne, &cached) ==
           control_session::RequestCacheResult::kSessionMismatch);
}

void test_authority_epoch_scopes_session_and_caches() {
    Clock clock;
    control_session::State state;
    state.initialize(random_fill, nullptr, Clock::now, &clock);
    control_session::ResponseFrame response{};
    response.bytes[0] = 'o';
    response.length = 1;
    assert(state.activate_hello(kNonce, kRequest, kSession, kEpochOne, response));
    state.cache_completed_request(1, "{\"ping\":true}", kEpochOne, response);

    const control_session::ResponseFrame *cached = nullptr;
    assert(state.inspect_request(kSession, 1, "{\"ping\":true}", kEpochOne, &cached) ==
           control_session::RequestCacheResult::kExactRetry);
    assert(cached == &response || cached->length == response.length);
    assert(state.inspect_request(kSession, 1, "{\"ping\":true}", kEpochTwo, &cached) ==
           control_session::RequestCacheResult::kSessionMismatch);
    // Epoch mismatch performs no hidden lease refresh.
    clock.value = control_session::kLeaseMicroseconds;
    assert(state.service_lease());
    assert(!state.has_active_session());

    state.initialize(random_fill, nullptr, Clock::now, &clock);
    assert(state.activate_hello(kNonce, kRequest, kSession, kEpochOne, response));
    assert(state.inspect_hello(kNonce, kRequest, kEpochOne, &cached) ==
           control_session::HelloCacheResult::kExactRetry);
    assert(state.inspect_hello(kNonce, kRequest, kEpochTwo, &cached) ==
           control_session::HelloCacheResult::kNewClient);
    state.revoke_for_lifecycle_invalidation(kEpochTwo);
    assert(!state.has_active_session());
}

void test_local_owner_allocator_is_unique_nonwrapping_and_retry_stable() {
    control_session::State state;
    state.initialize(random_fill, nullptr);
    control_session::ResponseFrame response{};

    assert(state.activate_hello(kNonce, kRequest, kSession, kEpochOne, response));
    const auto first = state.local_owner_id();
    assert(first != 0);
    const control_session::ResponseFrame *cached = nullptr;
    assert(state.inspect_hello(kNonce, kRequest, kEpochOne, &cached) ==
           control_session::HelloCacheResult::kExactRetry);
    assert(state.local_owner_id() == first);

    state.revoke_for_takeover();
    assert(state.activate_hello(kNonce, kRequest, kSession, kEpochOne, response));
    assert(state.local_owner_id() == first + 1);

    state.revoke_for_takeover();
    state.set_next_local_owner_id_for_test(
        static_cast<control_session::LocalOwnerId>(
            std::numeric_limits<std::uint32_t>::max()));
    assert(state.activate_hello(kNonce, kRequest, kSession, kEpochOne, response));
    assert(state.local_owner_id() == std::numeric_limits<std::uint32_t>::max());
    state.revoke_for_takeover();
    assert(state.activate_hello(kNonce, kRequest, kSession, kEpochOne, response));
    assert(state.local_owner_id() ==
           static_cast<control_session::LocalOwnerId>(
               std::numeric_limits<std::uint32_t>::max()) + 1U);

    state.revoke_for_takeover();
    state.set_next_local_owner_id_for_test(
        std::numeric_limits<control_session::LocalOwnerId>::max());
    assert(state.activate_hello(kNonce, kRequest, kSession, kEpochOne, response));
    assert(state.local_owner_id() ==
           std::numeric_limits<control_session::LocalOwnerId>::max());
    state.revoke_for_takeover();
    assert(!state.activate_hello(kNonce, kRequest, kSession, kEpochOne, response));
    assert(!state.has_active_session());
    assert(state.local_owner_id() == 0);
}

}  // namespace

int main() {
    test_lease_lifecycle();
    test_takeover_clears_retry_authority();
    test_authority_epoch_scopes_session_and_caches();
    test_local_owner_allocator_is_unique_nonwrapping_and_retry_stable();
    return 0;
}
