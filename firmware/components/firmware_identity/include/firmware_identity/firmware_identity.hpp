#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace firmware_identity {

inline constexpr std::size_t kAppElfSha256Bytes = 32;
inline constexpr std::size_t kAppElfSha256HexChars = 64;
inline constexpr std::size_t kVersionMaxBytes = 31;
inline constexpr std::size_t kSourceRevisionChars = 40;
inline constexpr std::size_t kBuildProfileMaxBytes = 31;
inline constexpr std::string_view kBuildProfile = "freenove-fnk0085";

struct SourceRevisionInput {
    bool present = false;
    std::string_view value{};
};

struct Identity {
    std::array<char, kVersionMaxBytes + 1> version{};
    bool source_revision_present = false;
    std::array<char, kSourceRevisionChars + 1> source_revision{};
    std::array<char, kAppElfSha256HexChars + 1> app_elf_sha256{};
    std::array<char, kBuildProfileMaxBytes + 1> build_profile{};
};

bool encode_app_elf_sha256(
    std::span<const std::uint8_t, kAppElfSha256Bytes> raw,
    std::span<char, kAppElfSha256HexChars + 1> output) noexcept;

bool is_valid_version(std::string_view value) noexcept;
bool is_valid_source_revision(SourceRevisionInput value) noexcept;
bool is_valid_raw_app_elf_sha256(
    std::span<const std::uint8_t, kAppElfSha256Bytes> value) noexcept;
bool is_valid_app_elf_sha256(std::string_view value) noexcept;
bool is_valid_build_profile(std::string_view value) noexcept;

bool build_identity(
    Identity* output,
    std::string_view version,
    SourceRevisionInput source_revision,
    std::span<const std::uint8_t, kAppElfSha256Bytes> raw_app_elf_sha256,
    std::string_view build_profile) noexcept;

SourceRevisionInput configured_source_revision() noexcept;

}  // namespace firmware_identity
