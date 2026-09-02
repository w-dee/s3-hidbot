#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace sensitive_request {

inline constexpr std::size_t kKeyBytes = 32;
inline constexpr std::size_t kDigestBytes = 32;
inline constexpr std::size_t kMaxPayloadBytes = 504;

using Key = std::array<std::uint8_t, kKeyBytes>;
using Digest = std::array<std::uint8_t, kDigestBytes>;
using SecureRandomFill = bool (*)(void *context, std::uint8_t *output,
                                  std::size_t length);
using HmacSha256 = bool (*)(void *context, const std::uint8_t *key,
                            std::size_t key_length,
                            const std::uint8_t *input,
                            std::size_t input_length,
                            std::uint8_t output[kDigestBytes]);

bool constant_time_equal(const Digest &left, const Digest &right);

// Boot-lifetime identity for byte-exact secret-bearing requests. The HMAC
// input is ASCII "s3-hidbot/ble.pairing.respond/v1", followed by the payload
// length as one unsigned 32-bit big-endian value, followed by the exact
// framing-normalized JSON payload bytes.
class Identity {
  public:
    ~Identity();

    bool initialize(SecureRandomFill random_fill, void *random_context,
                    HmacSha256 hmac, void *hmac_context);
    bool ready() const;
    bool digest(std::string_view payload, Digest *output) const;
    void clear();

  private:
    Key key_{};
    HmacSha256 hmac_ = nullptr;
    void *hmac_context_ = nullptr;
    bool ready_ = false;
};

}  // namespace sensitive_request
