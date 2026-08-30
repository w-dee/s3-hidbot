#include "firmware_identity/firmware_identity.hpp"

#if !defined(FIRMWARE_IDENTITY_NATIVE_TEST)
#include "identity_build_config.hpp"
#endif

#include <algorithm>
#include <cstring>

namespace {

constexpr bool is_ascii_alphanumeric(char value) noexcept {
    return (value >= '0' && value <= '9') || (value >= 'A' && value <= 'Z') ||
           (value >= 'a' && value <= 'z');
}

constexpr bool is_lower_hex(char value) noexcept {
    return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
}

constexpr bool is_decimal(char value) noexcept {
    return value >= '0' && value <= '9';
}

bool is_numeric_identifier(std::string_view value) noexcept {
    if (value.empty()) {
        return false;
    }
    if (value.size() > 1 && value.front() == '0') {
        return false;
    }
    return std::all_of(value.begin(), value.end(), is_decimal);
}

bool is_prerelease_identifier(std::string_view value) noexcept {
    if (value.empty()) {
        return false;
    }
    bool has_non_digit = false;
    bool has_letter_or_hyphen = false;
    for (char character : value) {
        if (!is_ascii_alphanumeric(character) && character != '-') {
            return false;
        }
        if (!is_decimal(character)) {
            has_non_digit = true;
        }
        if ((character >= 'A' && character <= 'Z') ||
            (character >= 'a' && character <= 'z') || character == '-') {
            has_letter_or_hyphen = true;
        }
    }
    if (!has_non_digit) {
        return is_numeric_identifier(value);
    }
    return has_letter_or_hyphen;
}

bool is_build_identifier(std::string_view value) noexcept {
    if (value.empty()) {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](char character) {
        return is_ascii_alphanumeric(character) || character == '-';
    });
}

bool is_dot_separated(std::string_view value, bool prerelease) noexcept {
    if (value.empty()) {
        return false;
    }
    std::size_t segment_start = 0;
    while (segment_start < value.size()) {
        const std::size_t separator = value.find('.', segment_start);
        const std::size_t segment_end = separator == std::string_view::npos ? value.size() : separator;
        const std::string_view segment = value.substr(segment_start, segment_end - segment_start);
        if (!(prerelease ? is_prerelease_identifier(segment) : is_build_identifier(segment))) {
            return false;
        }
        if (separator == std::string_view::npos) {
            return true;
        }
        segment_start = separator + 1;
    }
    return false;
}

bool is_all_zero(std::span<const std::uint8_t, firmware_identity::kAppElfSha256Bytes> raw) noexcept {
    return std::all_of(raw.begin(), raw.end(), [](std::uint8_t value) { return value == 0; });
}

}  // namespace

namespace firmware_identity {

bool encode_app_elf_sha256(
    std::span<const std::uint8_t, kAppElfSha256Bytes> raw,
    std::span<char, kAppElfSha256HexChars + 1> output) noexcept {
    constexpr char kHex[] = "0123456789abcdef";
    for (std::size_t index = 0; index < raw.size(); ++index) {
        output[index * 2] = kHex[(raw[index] >> 4U) & 0x0fU];
        output[index * 2 + 1] = kHex[raw[index] & 0x0fU];
    }
    output[kAppElfSha256HexChars] = '\0';
    return true;
}

bool is_valid_version(std::string_view value) noexcept {
    if (value.empty() || value.size() > kVersionMaxBytes) {
        return false;
    }
    for (char character : value) {
        if (static_cast<unsigned char>(character) > 0x7fU || character == ' ' || character == '\t' ||
            character == '\n' || character == '\r') {
            return false;
        }
    }

    const std::size_t prerelease_separator = value.find('-');
    const std::size_t build_separator = value.find('+');
    const std::size_t core_end = std::min(
        prerelease_separator == std::string_view::npos ? value.size() : prerelease_separator,
        build_separator == std::string_view::npos ? value.size() : build_separator);
    const std::string_view core = value.substr(0, core_end);
    std::size_t core_start = 0;
    for (unsigned int component = 0; component < 3; ++component) {
        const std::size_t separator = core.find('.', core_start);
        const std::size_t component_end = separator == std::string_view::npos ? core.size() : separator;
        if (!is_numeric_identifier(core.substr(core_start, component_end - core_start))) {
            return false;
        }
        if (component < 2) {
            if (separator == std::string_view::npos) {
                return false;
            }
            core_start = separator + 1;
        } else if (separator != std::string_view::npos || component_end != core.size()) {
            return false;
        }
    }

    if (prerelease_separator != std::string_view::npos &&
        (build_separator == std::string_view::npos || prerelease_separator < build_separator)) {
        const std::size_t prerelease_end = build_separator == std::string_view::npos ? value.size() : build_separator;
        if (!is_dot_separated(value.substr(prerelease_separator + 1, prerelease_end - prerelease_separator - 1), true)) {
            return false;
        }
    }
    if (build_separator != std::string_view::npos &&
        !is_dot_separated(value.substr(build_separator + 1), false)) {
        return false;
    }
    return true;
}

bool is_valid_source_revision(SourceRevisionInput value) noexcept {
    if (!value.present) {
        return value.value.empty();
    }
    return value.value.size() == kSourceRevisionChars &&
           std::all_of(value.value.begin(), value.value.end(), is_lower_hex);
}

bool is_valid_raw_app_elf_sha256(
    std::span<const std::uint8_t, kAppElfSha256Bytes> value) noexcept {
    return !is_all_zero(value);
}

bool is_valid_app_elf_sha256(std::string_view value) noexcept {
    return value.size() == kAppElfSha256HexChars &&
           std::all_of(value.begin(), value.end(), is_lower_hex);
}

bool is_valid_build_profile(std::string_view value) noexcept {
    if (value.empty() || value.size() > kBuildProfileMaxBytes) {
        return false;
    }
    std::size_t segment_start = 0;
    while (segment_start < value.size()) {
        const std::size_t separator = value.find('-', segment_start);
        const std::size_t segment_end = separator == std::string_view::npos ? value.size() : separator;
        const std::string_view segment = value.substr(segment_start, segment_end - segment_start);
        if (segment.empty() || !std::all_of(segment.begin(), segment.end(), [](char character) {
                return (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9');
            })) {
            return false;
        }
        if (separator == std::string_view::npos) {
            return true;
        }
        segment_start = separator + 1;
    }
    return false;
}

bool build_identity(
    Identity* output,
    std::string_view version,
    SourceRevisionInput source_revision,
    std::span<const std::uint8_t, kAppElfSha256Bytes> raw_app_elf_sha256,
    std::string_view build_profile) noexcept {
    if (output == nullptr || !is_valid_version(version) || !is_valid_source_revision(source_revision) ||
        !is_valid_raw_app_elf_sha256(raw_app_elf_sha256) || !is_valid_build_profile(build_profile)) {
        return false;
    }
    output->version.fill('\0');
    output->source_revision.fill('\0');
    output->app_elf_sha256.fill('\0');
    output->build_profile.fill('\0');
    std::memcpy(output->version.data(), version.data(), version.size());
    if (source_revision.present) {
        output->source_revision_present = true;
        std::memcpy(output->source_revision.data(), source_revision.value.data(), source_revision.value.size());
    } else {
        output->source_revision_present = false;
    }
    std::span<char, kAppElfSha256HexChars + 1> encoded(output->app_elf_sha256);
    (void)encode_app_elf_sha256(raw_app_elf_sha256, encoded);
    std::memcpy(output->build_profile.data(), build_profile.data(), build_profile.size());
    return true;
}

SourceRevisionInput configured_source_revision() noexcept {
#if defined(FIRMWARE_IDENTITY_NATIVE_TEST)
    return {};
#else
    if (!build::kSourceRevisionAvailable) {
        return {};
    }
    return SourceRevisionInput{true, build::kSourceRevision};
#endif
}

}  // namespace firmware_identity
