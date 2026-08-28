#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace control_session {

inline constexpr std::size_t kTokenHexLength = 32;
inline constexpr std::size_t kTokenStorageBytes = kTokenHexLength + 1;
inline constexpr std::size_t kMaxRequestBytes = 504;
inline constexpr std::size_t kMaxResponseBytes = 1024;

struct ResponseFrame {
    std::uint8_t bytes[kMaxResponseBytes]{};
    std::size_t length = 0;
};

using RandomFill = void (*)(void *context, std::uint8_t *output, std::size_t length);

enum class HelloCacheResult : std::uint8_t {
    kNewClient,
    kExactRetry,
    kNonceConflict,
};

enum class RequestCacheResult : std::uint8_t {
    kAcceptNew,
    kExactRetry,
    kIdConflict,
    kIdStale,
    kSessionMismatch,
};

// Fixed-capacity session and response caches. Tokens are epoch markers, not
// authentication credentials. All request identity comparisons use original
// framing-normalized JSON bytes rather than reserialized JSON.
class State {
  public:
    void initialize(RandomFill random_fill, void *random_context);

    const char *boot_id() const;
    const char *current_session() const;
    bool has_active_session() const;

    HelloCacheResult inspect_hello(std::string_view client_nonce,
                                   std::string_view request_bytes,
                                   const ResponseFrame **cached_response) const;
    void activate_hello(std::string_view client_nonce,
                        std::string_view request_bytes,
                        const char *new_session,
                        const ResponseFrame &response);
    void generate_token(char output[kTokenStorageBytes]);

    RequestCacheResult inspect_request(std::string_view session,
                                       std::int32_t id,
                                       std::string_view request_bytes,
                                       const ResponseFrame **cached_response) const;
    void cache_completed_request(std::int32_t id,
                                 std::string_view request_bytes,
                                 const ResponseFrame &response);

    // USB unmount is a safety boundary. It revokes the active control session
    // and both retry caches so a cached hello cannot revive that session.
    void revoke_for_unmount();

  private:
    struct HelloCache {
        bool valid = false;
        char client_nonce[kTokenStorageBytes]{};
        char request[kMaxRequestBytes + 1]{};
        std::size_t request_length = 0;
        char session[kTokenStorageBytes]{};
        ResponseFrame response{};
    };

    struct RequestCache {
        bool valid = false;
        std::int32_t id = 0;
        char request[kMaxRequestBytes + 1]{};
        std::size_t request_length = 0;
        ResponseFrame response{};
    };

    static void copy_token(char destination[kTokenStorageBytes], std::string_view token);
    static void copy_request(char destination[kMaxRequestBytes + 1],
                             std::size_t *destination_length,
                             std::string_view request);
    static bool same_request(const char *cached,
                             std::size_t cached_length,
                             std::string_view request);
    void clear_normal_cache();
    void clear_hello_cache();

    RandomFill random_fill_ = nullptr;
    void *random_context_ = nullptr;
    char boot_id_[kTokenStorageBytes]{};
    char current_session_[kTokenStorageBytes]{};
    bool active_session_ = false;
    HelloCache hello_cache_{};
    RequestCache request_cache_{};
};

bool is_lower_hex_token(std::string_view value);

}  // namespace control_session
