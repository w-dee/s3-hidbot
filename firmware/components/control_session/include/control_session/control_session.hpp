#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "sensitive_request/sensitive_request.hpp"

namespace control_session {

inline constexpr std::size_t kTokenHexLength = 32;
inline constexpr std::size_t kTokenStorageBytes = kTokenHexLength + 1;
inline constexpr std::size_t kMaxRequestBytes = 504;
inline constexpr std::size_t kMaxResponseBytes = 1536;
inline constexpr std::uint32_t kLeaseMilliseconds = 5000;
inline constexpr std::uint64_t kLeaseMicroseconds = 5000000;

struct ResponseFrame {
    std::uint8_t bytes[kMaxResponseBytes]{};
    std::size_t length = 0;
};

using RandomFill = void (*)(void *context, std::uint8_t *output, std::size_t length);
using NowFn = std::uint64_t (*)(void *context);
using AuthorityEpoch = std::uint32_t;
using LocalOwnerId = std::uint64_t;

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
    void initialize(RandomFill random_fill, void *random_context,
                    NowFn now, void *now_context);

    const char *boot_id() const;
    const char *current_session() const;
    bool has_active_session() const;
    bool authority_epoch_matches(AuthorityEpoch current_epoch) const;
    AuthorityEpoch session_authority_epoch() const;
    LocalOwnerId local_owner_id() const;
    bool refresh_lease();
    bool service_lease();
    void revoke_for_takeover();

    HelloCacheResult inspect_hello(std::string_view client_nonce,
                                   std::string_view request_bytes,
                                   AuthorityEpoch current_epoch,
                                   const ResponseFrame **cached_response) const;
    bool activate_hello(std::string_view client_nonce,
                        std::string_view request_bytes,
                        const char *new_session,
                        AuthorityEpoch authority_epoch,
                        const ResponseFrame &response);
    void generate_token(char output[kTokenStorageBytes]);

    RequestCacheResult inspect_request(std::string_view session,
                                       std::int32_t id,
                                       std::string_view request_bytes,
                                       AuthorityEpoch current_epoch,
                                       const ResponseFrame **cached_response) const;
    RequestCacheResult inspect_sensitive_request(
        std::string_view session, std::int32_t id, std::size_t payload_length,
        const sensitive_request::Digest &digest,
        AuthorityEpoch current_epoch,
        const ResponseFrame **cached_response) const;
    void cache_completed_request(std::int32_t id,
                                 std::string_view request_bytes,
                                 AuthorityEpoch authority_epoch,
                                 const ResponseFrame &response);
    void cache_completed_sensitive_request(
        std::int32_t id, std::size_t payload_length,
        const sensitive_request::Digest &digest,
        AuthorityEpoch authority_epoch, const ResponseFrame &response);

    const ResponseFrame *inspect_transition_retry(
        std::string_view session, std::int32_t id,
        std::string_view request_bytes) const;
    void cache_transition_retry(std::string_view session, std::int32_t id,
                                std::string_view request_bytes,
                                const ResponseFrame &response);
    void clear_transition_retry();

    void revoke_for_lifecycle_invalidation(AuthorityEpoch current_epoch);

#ifdef CONTROL_SESSION_NATIVE_TEST
    struct RequestCacheSnapshot {
        bool valid = false;
        bool sensitive = false;
        std::int32_t id = 0;
        std::size_t payload_length = 0;
        bool raw_storage_zero = false;
        sensitive_request::Digest digest{};
    };
    RequestCacheSnapshot request_cache_snapshot_for_test() const;
    bool transition_cache_active_for_test() const;
    bool retry_cache_contains_for_test(std::string_view bytes) const;
    void set_next_local_owner_id_for_test(LocalOwnerId next_owner_id);
#endif

  private:
    struct HelloCache {
        bool valid = false;
        char client_nonce[kTokenStorageBytes]{};
        char request[kMaxRequestBytes + 1]{};
        std::size_t request_length = 0;
        AuthorityEpoch authority_epoch = 0;
        ResponseFrame response{};
    };

    struct RequestCache {
        bool valid = false;
        bool sensitive = false;
        std::int32_t id = 0;
        char request[kMaxRequestBytes + 1]{};
        std::size_t request_length = 0;
        sensitive_request::Digest digest{};
        AuthorityEpoch authority_epoch = 0;
        ResponseFrame response{};
    };

    struct TransitionRetryCache {
        bool active = false;
        std::int32_t id = 0;
        char session[kTokenStorageBytes]{};
        char request[kMaxRequestBytes + 1]{};
        std::size_t request_length = 0;
        ResponseFrame response{};
    };

    union RetryCacheStorage {
        RequestCache normal;
        TransitionRetryCache transition;

        constexpr RetryCacheStorage() : normal{} {}
    };

    enum class RetryCacheKind : std::uint8_t {
        kNormal,
        kTransition,
    };

    static void copy_token(char destination[kTokenStorageBytes], std::string_view token);
    static void copy_request(char destination[kMaxRequestBytes + 1],
                             std::size_t *destination_length,
                             std::string_view request);
    static bool same_request(const char *cached,
                             std::size_t cached_length,
                             std::string_view request);
    RequestCache &activate_normal_cache();
    void clear_normal_cache();
    void clear_hello_cache();
    std::uint64_t now() const;
    void clear_authority();

    RandomFill random_fill_ = nullptr;
    void *random_context_ = nullptr;
    char boot_id_[kTokenStorageBytes]{};
    char current_session_[kTokenStorageBytes]{};
    bool active_session_ = false;
    RetryCacheKind retry_cache_kind_ = RetryCacheKind::kNormal;
    AuthorityEpoch session_authority_epoch_ = 0;
    LocalOwnerId local_owner_id_ = 0;
    // initialize() establishes the first valid owner. Keeping static storage
    // zero-initialized avoids making the containing Protocol image-backed.
    LocalOwnerId next_local_owner_id_ = 0;
    NowFn now_fn_ = nullptr;
    void *now_context_ = nullptr;
    std::uint64_t lease_deadline_us_ = 0;
    HelloCache hello_cache_{};
    RetryCacheStorage retry_cache_{};
};

bool is_lower_hex_token(std::string_view value);

}  // namespace control_session
