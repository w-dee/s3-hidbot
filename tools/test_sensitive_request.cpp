#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

#include <openssl/sha.h>

#include "sensitive_request/sensitive_request.hpp"

namespace {

struct CryptoFixture {
    std::array<std::uint8_t, sensitive_request::kKeyBytes> observed_key{};
    std::vector<std::uint8_t> observed_input;
    bool random_success = true;

    static bool random(void *context, std::uint8_t *output,
                       std::size_t length) {
        auto *fixture = static_cast<CryptoFixture *>(context);
        if (!fixture->random_success) return false;
        for (std::size_t index = 0; index < length; ++index) {
            output[index] = static_cast<std::uint8_t>(index);
        }
        return true;
    }

    static bool hmac(void *context, const std::uint8_t *key,
                     std::size_t key_length, const std::uint8_t *input,
                     std::size_t input_length,
                     std::uint8_t output[sensitive_request::kDigestBytes]) {
        auto *fixture = static_cast<CryptoFixture *>(context);
        assert(key_length == fixture->observed_key.size());
        std::memcpy(fixture->observed_key.data(), key, key_length);
        fixture->observed_input.assign(input, input + input_length);
        std::array<std::uint8_t, 64> inner_pad{};
        std::array<std::uint8_t, 64> outer_pad{};
        std::memcpy(inner_pad.data(), key, key_length);
        std::memcpy(outer_pad.data(), key, key_length);
        for (std::size_t index = 0; index < inner_pad.size(); ++index) {
            inner_pad[index] ^= 0x36U;
            outer_pad[index] ^= 0x5cU;
        }
        std::vector<std::uint8_t> inner(inner_pad.begin(), inner_pad.end());
        inner.insert(inner.end(), input, input + input_length);
        std::array<std::uint8_t, SHA256_DIGEST_LENGTH> inner_digest{};
        SHA256(inner.data(), inner.size(), inner_digest.data());
        std::vector<std::uint8_t> outer(outer_pad.begin(), outer_pad.end());
        outer.insert(outer.end(), inner_digest.begin(), inner_digest.end());
        SHA256(outer.data(), outer.size(), output);
        return true;
    }
};

void test_exact_hmac_layout_and_vector() {
    CryptoFixture crypto;
    sensitive_request::Identity identity;
    assert(identity.initialize(CryptoFixture::random, &crypto,
                               CryptoFixture::hmac, &crypto));
    constexpr std::string_view payload = "{\"v\":1,\"id\":2}";
    sensitive_request::Digest digest{};
    assert(identity.digest(payload, &digest));
    constexpr std::uint8_t expected_input[] = {
        0x73,0x33,0x2d,0x68,0x69,0x64,0x62,0x6f,0x74,0x2f,0x62,0x6c,
        0x65,0x2e,0x70,0x61,0x69,0x72,0x69,0x6e,0x67,0x2e,0x72,0x65,
        0x73,0x70,0x6f,0x6e,0x64,0x2f,0x76,0x31,0x00,0x00,0x00,0x0e,
        0x7b,0x22,0x76,0x22,0x3a,0x31,0x2c,0x22,0x69,0x64,0x22,0x3a,
        0x32,0x7d,
    };
    assert(crypto.observed_input ==
           std::vector<std::uint8_t>(std::begin(expected_input),
                                     std::end(expected_input)));
    constexpr sensitive_request::Digest expected_digest{
        0xc1,0xb0,0x57,0xbe,0x1e,0x29,0x61,0x8c,
        0xa0,0xf5,0x79,0xba,0x82,0x05,0xe2,0xa1,
        0xaa,0x69,0xd6,0xa1,0xe8,0x0e,0x86,0x10,
        0xa3,0x54,0xdb,0x1f,0x27,0xc7,0x9c,0x16,
    };
    assert(sensitive_request::constant_time_equal(digest, expected_digest));
    auto different = expected_digest;
    different.back() ^= 1U;
    assert(!sensitive_request::constant_time_equal(digest, different));
}

void test_rng_failure_and_clear() {
    CryptoFixture crypto;
    crypto.random_success = false;
    sensitive_request::Identity identity;
    assert(!identity.initialize(CryptoFixture::random, &crypto,
                                CryptoFixture::hmac, &crypto));
    assert(!identity.ready());
    sensitive_request::Digest digest{};
    assert(!identity.digest("secret", &digest));
    for (const std::uint8_t byte : digest) assert(byte == 0);
}

}  // namespace

int main() {
    test_exact_hmac_layout_and_vector();
    test_rng_failure_and_clear();
    return 0;
}
