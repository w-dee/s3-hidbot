#include "control_session/control_session.hpp"

#include <cstring>

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
    request_cache_ = RequestCache{};
}

void State::clear_hello_cache() {
    hello_cache_ = HelloCache{};
}

void State::initialize(RandomFill random_fill, void *random_context) {
    random_fill_ = random_fill;
    random_context_ = random_context;
    active_session_ = false;
    current_session_[0] = '\0';
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
                                      const ResponseFrame **cached_response) const {
    if (cached_response != nullptr) {
        *cached_response = nullptr;
    }
    if (!hello_cache_.valid || !same_token(hello_cache_.client_nonce, client_nonce)) {
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
                           const ResponseFrame &response) {
    copy_token(current_session_, std::string_view(new_session, kTokenHexLength));
    active_session_ = true;
    clear_normal_cache();
    hello_cache_ = HelloCache{};
    hello_cache_.valid = true;
    copy_token(hello_cache_.client_nonce, client_nonce);
    copy_request(hello_cache_.request, &hello_cache_.request_length, request_bytes);
    copy_token(hello_cache_.session, std::string_view(new_session, kTokenHexLength));
    hello_cache_.response = response;
}

RequestCacheResult State::inspect_request(std::string_view session,
                                          std::int32_t id,
                                          std::string_view request_bytes,
                                          const ResponseFrame **cached_response) const {
    if (cached_response != nullptr) {
        *cached_response = nullptr;
    }
    if (!active_session_ || !same_token(current_session_, session)) {
        return RequestCacheResult::kSessionMismatch;
    }
    if (!request_cache_.valid || id > request_cache_.id) {
        return RequestCacheResult::kAcceptNew;
    }
    if (id < request_cache_.id) {
        return RequestCacheResult::kIdStale;
    }
    if (!same_request(request_cache_.request, request_cache_.request_length, request_bytes)) {
        return RequestCacheResult::kIdConflict;
    }
    if (cached_response != nullptr) {
        *cached_response = &request_cache_.response;
    }
    return RequestCacheResult::kExactRetry;
}

void State::cache_completed_request(std::int32_t id,
                                    std::string_view request_bytes,
                                    const ResponseFrame &response) {
    request_cache_ = RequestCache{};
    request_cache_.valid = true;
    request_cache_.id = id;
    copy_request(request_cache_.request, &request_cache_.request_length, request_bytes);
    request_cache_.response = response;
}

void State::revoke_for_unmount() {
    active_session_ = false;
    current_session_[0] = '\0';
    clear_normal_cache();
    clear_hello_cache();
}

}  // namespace control_session
