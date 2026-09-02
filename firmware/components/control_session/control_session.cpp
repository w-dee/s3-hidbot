#include "control_session/control_session.hpp"

#include <cstring>

#include "secure_memory/secure_memory.hpp"

namespace control_session {
namespace {

constexpr char kHexDigits[] = "0123456789abcdef";

bool same_token(const char *cached, std::string_view value) {
    return value.size() == kTokenHexLength &&
           std::memcmp(cached, value.data(), kTokenHexLength) == 0;
}

}  // namespace

bool is_lower_hex_token(std::string_view value) {
    if (value.size() != kTokenHexLength) {
        return false;
    }
    for (const char character : value) {
        if (!((character >= '0' && character <= '9') ||
              (character >= 'a' && character <= 'f'))) {
            return false;
        }
    }
    return true;
}

void State::copy_token(char destination[kTokenStorageBytes], std::string_view token) {
    std::memcpy(destination, token.data(), kTokenHexLength);
    destination[kTokenHexLength] = '\0';
}

void State::copy_request(char destination[kMaxRequestBytes + 1],
                         std::size_t *destination_length,
                         std::string_view request) {
    std::memcpy(destination, request.data(), request.size());
    destination[request.size()] = '\0';
    *destination_length = request.size();
}

bool State::same_request(const char *cached,
                         std::size_t cached_length,
                         std::string_view request) {
    return request.size() == cached_length &&
           std::memcmp(cached, request.data(), cached_length) == 0;
}

void State::clear_normal_cache() {
    secure_memory::zero(request_cache_.request, sizeof(request_cache_.request));
    secure_memory::zero(request_cache_.digest.data(), request_cache_.digest.size());
    request_cache_ = RequestCache{};
}

void State::clear_hello_cache() {
    hello_cache_ = HelloCache{};
}

void State::initialize(RandomFill random_fill, void *random_context) {
    initialize(random_fill, random_context, nullptr, nullptr);
}

void State::initialize(RandomFill random_fill, void *random_context,
                       NowFn now_fn, void *now_context) {
    random_fill_ = random_fill;
    random_context_ = random_context;
    now_fn_ = now_fn;
    now_context_ = now_context;
    active_session_ = false;
    current_session_[0] = '\0';
    session_authority_epoch_ = 0;
    lease_deadline_us_ = 0;
    clear_normal_cache();
    clear_hello_cache();
    generate_token(boot_id_);
}

const char *State::boot_id() const {
    return boot_id_;
}

const char *State::current_session() const {
    return current_session_;
}

bool State::has_active_session() const {
    return active_session_;
}

bool State::authority_epoch_matches(AuthorityEpoch current_epoch) const {
    return active_session_ && session_authority_epoch_ == current_epoch;
}

AuthorityEpoch State::session_authority_epoch() const {
    return session_authority_epoch_;
}

std::uint64_t State::now() const {
    return now_fn_ != nullptr ? now_fn_(now_context_) : 0;
}

bool State::refresh_lease() {
    if (!active_session_) {
        return false;
    }
    lease_deadline_us_ = now() + kLeaseMicroseconds;
    return true;
}

bool State::service_lease() {
    if (!active_session_ || now() < lease_deadline_us_) {
        return false;
    }
    clear_authority();
    return true;
}

void State::clear_authority() {
    active_session_ = false;
    current_session_[0] = '\0';
    session_authority_epoch_ = 0;
    lease_deadline_us_ = 0;
    clear_normal_cache();
    clear_hello_cache();
}

void State::revoke_for_takeover() {
    clear_authority();
}

void State::generate_token(char output[kTokenStorageBytes]) {
    std::uint8_t random_bytes[kTokenHexLength / 2]{};
    if (random_fill_ != nullptr) {
        random_fill_(random_context_, random_bytes, sizeof(random_bytes));
    }
    for (std::size_t index = 0; index < sizeof(random_bytes); ++index) {
        output[index * 2] = kHexDigits[random_bytes[index] >> 4];
        output[index * 2 + 1] = kHexDigits[random_bytes[index] & 0x0fU];
    }
    output[kTokenHexLength] = '\0';
}

HelloCacheResult State::inspect_hello(std::string_view client_nonce,
                                      std::string_view request_bytes,
                                      AuthorityEpoch current_epoch,
                                      const ResponseFrame **cached_response) const {
    if (cached_response != nullptr) {
        *cached_response = nullptr;
    }
    // Retry authority is scoped to the lifecycle authority epoch. A matching
    // nonce from an older epoch is a fresh hello, not a resurrection of the
    // previous session or response.
    if (!hello_cache_.valid || hello_cache_.authority_epoch != current_epoch ||
        !same_token(hello_cache_.client_nonce, client_nonce)) {
        return HelloCacheResult::kNewClient;
    }
    if (!same_request(hello_cache_.request, hello_cache_.request_length, request_bytes)) {
        return HelloCacheResult::kNonceConflict;
    }
    if (cached_response != nullptr) {
        *cached_response = &hello_cache_.response;
    }
    return HelloCacheResult::kExactRetry;
}

void State::activate_hello(std::string_view client_nonce,
                           std::string_view request_bytes,
                           const char *new_session,
                           AuthorityEpoch authority_epoch,
                           const ResponseFrame &response) {
    copy_token(current_session_, std::string_view(new_session, kTokenHexLength));
    active_session_ = true;
    session_authority_epoch_ = authority_epoch;
    lease_deadline_us_ = now() + kLeaseMicroseconds;
    clear_normal_cache();
    hello_cache_ = HelloCache{};
    hello_cache_.valid = true;
    copy_token(hello_cache_.client_nonce, client_nonce);
    copy_request(hello_cache_.request, &hello_cache_.request_length, request_bytes);
    copy_token(hello_cache_.session, std::string_view(new_session, kTokenHexLength));
    hello_cache_.authority_epoch = authority_epoch;
    hello_cache_.response = response;
}

RequestCacheResult State::inspect_request(std::string_view session,
                                          std::int32_t id,
                                          std::string_view request_bytes,
                                          AuthorityEpoch current_epoch,
                                          const ResponseFrame **cached_response) const {
    if (cached_response != nullptr) {
        *cached_response = nullptr;
    }
    // This check is intentionally before normal-cache replay and lease
    // refresh. An old exact retry must never make a retired session look live.
    if (!active_session_ || session_authority_epoch_ != current_epoch ||
        !same_token(current_session_, session)) {
        return RequestCacheResult::kSessionMismatch;
    }
    if (!request_cache_.valid || request_cache_.authority_epoch != current_epoch ||
        id > request_cache_.id) {
        return RequestCacheResult::kAcceptNew;
    }
    if (id < request_cache_.id) {
        return RequestCacheResult::kIdStale;
    }
    if (request_cache_.sensitive ||
        !same_request(request_cache_.request, request_cache_.request_length, request_bytes)) {
        return RequestCacheResult::kIdConflict;
    }
    if (cached_response != nullptr) {
        *cached_response = &request_cache_.response;
    }
    return RequestCacheResult::kExactRetry;
}

RequestCacheResult State::inspect_sensitive_request(
    std::string_view session, std::int32_t id, std::size_t payload_length,
    const sensitive_request::Digest &digest, AuthorityEpoch current_epoch,
    const ResponseFrame **cached_response) const {
    if (cached_response != nullptr) {
        *cached_response = nullptr;
    }
    if (!active_session_ || session_authority_epoch_ != current_epoch ||
        !same_token(current_session_, session)) {
        return RequestCacheResult::kSessionMismatch;
    }
    if (!request_cache_.valid || request_cache_.authority_epoch != current_epoch ||
        id > request_cache_.id) {
        return RequestCacheResult::kAcceptNew;
    }
    if (id < request_cache_.id) {
        return RequestCacheResult::kIdStale;
    }
    if (!request_cache_.sensitive || request_cache_.request_length != payload_length ||
        !sensitive_request::constant_time_equal(request_cache_.digest, digest)) {
        return RequestCacheResult::kIdConflict;
    }
    if (cached_response != nullptr) {
        *cached_response = &request_cache_.response;
    }
    return RequestCacheResult::kExactRetry;
}

void State::cache_completed_request(std::int32_t id,
                                    std::string_view request_bytes,
                                    AuthorityEpoch authority_epoch,
                                    const ResponseFrame &response) {
    request_cache_ = RequestCache{};
    request_cache_.valid = true;
    request_cache_.sensitive = false;
    request_cache_.id = id;
    request_cache_.authority_epoch = authority_epoch;
    copy_request(request_cache_.request, &request_cache_.request_length, request_bytes);
    request_cache_.response = response;
}

void State::cache_completed_sensitive_request(
    std::int32_t id, std::size_t payload_length,
    const sensitive_request::Digest &digest, AuthorityEpoch authority_epoch,
    const ResponseFrame &response) {
    clear_normal_cache();
    request_cache_.valid = true;
    request_cache_.sensitive = true;
    request_cache_.id = id;
    request_cache_.request_length = payload_length;
    request_cache_.digest = digest;
    request_cache_.authority_epoch = authority_epoch;
    request_cache_.response = response;
}

#ifdef CONTROL_SESSION_NATIVE_TEST
State::RequestCacheSnapshot State::request_cache_snapshot_for_test() const {
    bool raw_storage_zero = true;
    for (const char byte : request_cache_.request) {
        raw_storage_zero = raw_storage_zero && byte == 0;
    }
    return {.valid = request_cache_.valid,
            .sensitive = request_cache_.sensitive,
            .id = request_cache_.id,
            .payload_length = request_cache_.request_length,
            .raw_storage_zero = raw_storage_zero,
            .digest = request_cache_.digest};
}
#endif

void State::revoke_for_lifecycle_invalidation(AuthorityEpoch current_epoch) {
    if ((active_session_ && session_authority_epoch_ != current_epoch) ||
        (hello_cache_.valid && hello_cache_.authority_epoch != current_epoch) ||
        (request_cache_.valid && request_cache_.authority_epoch != current_epoch)) {
        clear_authority();
    }
}

}  // namespace control_session
