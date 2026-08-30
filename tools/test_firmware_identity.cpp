#include "firmware_identity/firmware_identity.hpp"

#include <array>
#include <cassert>
#include <cstdint>
#include <string>

namespace {

template <typename Predicate>
void expect(Predicate predicate) {
    assert(predicate());
}

}  // namespace

int main() {
    using namespace firmware_identity;

    std::array<std::uint8_t, kAppElfSha256Bytes> zeros{};
    std::array<char, kAppElfSha256HexChars + 1> encoded{};
    expect([&] { return encode_app_elf_sha256(zeros, encoded); });
    expect([&] { return std::string(encoded.data()) == std::string(64, '0'); });
    expect([&] { return !is_valid_raw_app_elf_sha256(zeros); });
    expect([&] { return is_valid_app_elf_sha256(std::string_view(encoded.data())); });

    std::array<std::uint8_t, kAppElfSha256Bytes> incremental{};
    for (std::size_t index = 0; index < incremental.size(); ++index) {
        incremental[index] = static_cast<std::uint8_t>(index);
    }
    expect([&] { return encode_app_elf_sha256(incremental, encoded); });
    expect([&] { return is_valid_raw_app_elf_sha256(incremental); });
    expect([&] {
        return std::string(encoded.data()) ==
               "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f";
    });
    std::array<std::uint8_t, kAppElfSha256Bytes> ff{};
    ff.fill(0xffU);
    expect([&] { return encode_app_elf_sha256(ff, encoded); });
    expect([&] { return std::string(encoded.data()) == std::string(64, 'f'); });

    for (std::string_view valid : {"0.1.0", "0.1.0-dev", "1.2.3-rc.1", "1.2.3+build",
                                   "1.2.3-rc.1+build.2"}) {
        expect([&] { return is_valid_version(valid); });
    }
    for (std::string_view invalid : {"", "1", "1.2", "01.2.3", "1.02.3", "1.2.03",
                                     "1.2.3-", "1.2.3+", "1.2.3-01", "1.2.3-rc..1",
                                     "1.2.3 rc", "1.2.3-rc+build+again"}) {
        expect([&] { return !is_valid_version(invalid); });
    }
    expect([&] { return is_valid_version(std::string(27, '1') + ".0.0"); });
    expect([&] { return !is_valid_version(std::string(28, '1') + ".0.0"); });
    expect([&] { return !is_valid_version("1.2.3-\xC2\xA0"); });

    const SourceRevisionInput absent{};
    const SourceRevisionInput present{true, "0123456789abcdef0123456789abcdef01234567"};
    expect([&] { return is_valid_source_revision(absent); });
    expect([&] { return is_valid_source_revision(present); });
    for (std::string_view invalid : {"0123456789abcdef0123456789abcdef0123456",
                                     "0123456789abcdef0123456789abcdef012345678",
                                     "0123456789ABCDEF0123456789abcdef01234567",
                                     "0123456789abcdef0123456789abcdef0123456g",
                                     "0123456789abcdef0123456789abcdef0123456 "}) {
        expect([&] { return !is_valid_source_revision(SourceRevisionInput{true, invalid}); });
    }

    expect([&] { return is_valid_build_profile(kBuildProfile); });
    expect([&] { return is_valid_build_profile(std::string(31, 'a')); });
    expect([&] { return !is_valid_build_profile(std::string(32, 'a')); });
    for (std::string_view invalid : {"", "Freenove-fnk0085", "freenove_fnk0085", "-freenove",
                                     "freenove-", "freenove--fnk0085", "freenove.fnk0085"}) {
        expect([&] { return !is_valid_build_profile(invalid); });
    }

    Identity identity{};
    expect([&] {
        return build_identity(&identity, "0.1.0-dev", present, incremental, kBuildProfile);
    });
    expect([&] { return std::string(identity.version.data()) == "0.1.0-dev"; });
    expect([&] { return identity.source_revision_present; });
    expect([&] { return std::string(identity.source_revision.data()) == present.value; });
    expect([&] {
        return std::string(identity.app_elf_sha256.data()) ==
               "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f";
    });
    expect([&] { return std::string(identity.build_profile.data()) == kBuildProfile; });
    expect([&] { return !build_identity(&identity, "0.1.0-dev", absent, zeros, kBuildProfile); });
    expect([&] { return !build_identity(&identity, "0.1.0-dev", present, incremental, "bad_profile"); });
    expect([&] { return !is_valid_app_elf_sha256(std::string(63, 'a')); });
    expect([&] { return !is_valid_app_elf_sha256(std::string(64, 'A')); });

    return 0;
}
