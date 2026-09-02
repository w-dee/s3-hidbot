#include "sensitive_request/sensitive_request.hpp"

#include <cstring>

#include "secure_memory/secure_memory.hpp"

#ifdef ESP_PLATFORM
extern "C" {
#include "mbedtls/constant_time.h"
}
#endif

namespace sensitive_request {
namespace {

constexpr std::uint8_t kDomain[] = "s3-hidbot/ble.pairing.respond/v1";
constexpr std::size_t kDomainBytes = sizeof(kDomain) - 1;
constexpr std::size_t kLengthBytes = 4;

}  // namespace

Identity::~Identity() { clear(); }

bool Identity::initialize(SecureRandomFill random_fill, void *random_context,
                          HmacSha256 hmac, void *hmac_context) {
    clear();
    if (random_fill == nullptr || hmac == nullptr ||
        !random_fill(random_context, key_.data(), key_.size())) {
        clear();
        return false;
    }
    hmac_ = hmac;
    hmac_context_ = hmac_context;
    ready_ = true;
    return true;
}

bool Identity::ready() const { return ready_; }

bool Identity::digest(std::string_view payload, Digest *output) const {
    if (!ready_ || output == nullptr || payload.size() > kMaxPayloadBytes) {
        return false;
    }
    std::array<std::uint8_t, kDomainBytes + kLengthBytes + kMaxPayloadBytes>
        input{};
    std::memcpy(input.data(), kDomain, kDomainBytes);
    const std::uint32_t length = static_cast<std::uint32_t>(payload.size());
    input[kDomainBytes] = static_cast<std::uint8_t>(length >> 24U);
    input[kDomainBytes + 1] = static_cast<std::uint8_t>(length >> 16U);
    input[kDomainBytes + 2] = static_cast<std::uint8_t>(length >> 8U);
    input[kDomainBytes + 3] = static_cast<std::uint8_t>(length);
    std::memcpy(input.data() + kDomainBytes + kLengthBytes, payload.data(),
                payload.size());
    const bool success = hmac_(hmac_context_, key_.data(), key_.size(),
                               input.data(),
                               kDomainBytes + kLengthBytes + payload.size(),
                               output->data());
    secure_memory::zero(input.data(), input.size());
    if (!success) {
        secure_memory::zero(output->data(), output->size());
    }
    return success;
}

bool constant_time_equal(const Digest &left, const Digest &right) {
#ifdef ESP_PLATFORM
    return mbedtls_ct_memcmp(left.data(), right.data(), left.size()) == 0;
#else
    volatile std::uint8_t difference = 0;
    for (std::size_t index = 0; index < left.size(); ++index) {
        difference = static_cast<std::uint8_t>(difference |
                                               (left[index] ^ right[index]));
    }
    return difference == 0;
#endif
}

void Identity::clear() {
    secure_memory::zero(key_.data(), key_.size());
    hmac_ = nullptr;
    hmac_context_ = nullptr;
    ready_ = false;
}

}  // namespace sensitive_request
