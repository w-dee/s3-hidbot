#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

#include "sensitive_request/sensitive_request.hpp"

namespace {

struct RecordingHmac {
    std::array<std::uint8_t, sensitive_request::kKeyBytes> observed_key{};
    std::vector<std::uint8_t> observed_input;
    std::size_t observed_key_length = 0;
    bool random_success = true;

    static bool random(void *context, std::uint8_t *output,
                       std::size_t length) {
        auto *fixture = static_cast<RecordingHmac *>(context);
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
        auto *fixture = static_cast<RecordingHmac *>(context);
        assert(key_length == fixture->observed_key.size());
        fixture->observed_key_length = key_length;
        std::memcpy(fixture->observed_key.data(), key, key_length);
        fixture->observed_input.assign(input, input + input_length);
        // TEST-ONLY deterministic provider. It is input-sensitive for retry
        // identity tests but intentionally is not a cryptographic algorithm.
        for (std::size_t index = 0; index < sensitive_request::kDigestBytes;
             ++index) {
            output[index] = static_cast<std::uint8_t>(
                key[index] ^ static_cast<std::uint8_t>(0xa5U + index));
        }
        for (std::size_t index = 0; index < input_length; ++index) {
            const std::size_t lane = index % sensitive_request::kDigestBytes;
            output[lane] = static_cast<std::uint8_t>(
                output[lane] * 33U + input[index] +
                static_cast<std::uint8_t>(index * 17U));
        }
        output[input_length % sensitive_request::kDigestBytes] ^=
            static_cast<std::uint8_t>(input_length);
        return true;
    }
};

void test_exact_hmac_layout_key_and_deterministic_behavior() {
    static_assert(sensitive_request::kDigestBytes == 32);
    RecordingHmac crypto;
    sensitive_request::Identity identity;
    assert(identity.initialize(RecordingHmac::random, &crypto,
                               RecordingHmac::hmac, &crypto));
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
    std::array<std::uint8_t, sensitive_request::kKeyBytes> expected_key{};
    for (std::size_t index = 0; index < expected_key.size(); ++index) {
        expected_key[index] = static_cast<std::uint8_t>(index);
    }
    assert(crypto.observed_key_length == sensitive_request::kKeyBytes);
    assert(crypto.observed_key == expected_key);

    sensitive_request::Digest exact_retry{};
    assert(identity.digest(payload, &exact_retry));
    assert(sensitive_request::constant_time_equal(digest, exact_retry));

    sensitive_request::Digest changed{};
    assert(identity.digest("{\"v\":1,\"id\":3}", &changed));
    assert(!sensitive_request::constant_time_equal(digest, changed));
    auto comparison_mismatch = digest;
    comparison_mismatch.back() ^= 1U;
    assert(!sensitive_request::constant_time_equal(
        digest, comparison_mismatch));
}

void test_rng_failure_and_clear() {
    RecordingHmac crypto;
    crypto.random_success = false;
    sensitive_request::Identity identity;
    assert(!identity.initialize(RecordingHmac::random, &crypto,
                                RecordingHmac::hmac, &crypto));
    assert(!identity.ready());
    sensitive_request::Digest digest{};
    assert(!identity.digest("secret", &digest));
    for (const std::uint8_t byte : digest) assert(byte == 0);
}

}  // namespace

int main() {
    test_exact_hmac_layout_key_and_deterministic_behavior();
    test_rng_failure_and_clear();
    return 0;
}
