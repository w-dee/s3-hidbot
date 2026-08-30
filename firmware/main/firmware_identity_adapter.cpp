#include "firmware_identity_adapter.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "esp_app_desc.h"

namespace {

bool bounded_descriptor_string(const char *field,
                               std::size_t field_bytes,
                               std::size_t maximum_bytes,
                               std::string_view *value) noexcept {
    if (field == nullptr || value == nullptr) {
        return false;
    }
    std::size_t length = 0;
    while (length < field_bytes && field[length] != '\0') {
        ++length;
    }
    if (length == 0 || length == field_bytes || length > maximum_bytes) {
        return false;
    }
    *value = std::string_view(field, length);
    return true;
}

}  // namespace

namespace firmware_identity_adapter {

bool build_runtime_identity(firmware_identity::Identity *output) noexcept {
    if (output == nullptr) {
        return false;
    }

    const esp_app_desc_t *description = esp_app_get_description();
    if (description == nullptr || description->magic_word != ESP_APP_DESC_MAGIC_WORD) {
        return false;
    }

    std::string_view version;
    if (!bounded_descriptor_string(description->version,
                                   sizeof(description->version),
                                   firmware_identity::kVersionMaxBytes,
                                   &version)) {
        return false;
    }

    // The descriptor lives in flash/DROM. Copy through a volatile pointer so
    // the compiler cannot replace the byte-wise read with an unsafe bulk read.
    std::array<std::uint8_t, firmware_identity::kAppElfSha256Bytes> raw_digest{};
    const volatile std::uint8_t *source = description->app_elf_sha256;
    for (std::size_t index = 0; index < raw_digest.size(); ++index) {
        raw_digest[index] = source[index];
    }

    const firmware_identity::SourceRevisionInput source_revision =
        firmware_identity::configured_source_revision();
    if (!firmware_identity::is_valid_source_revision(source_revision)) {
        return false;
    }

    return firmware_identity::build_identity(output,
                                             version,
                                             source_revision,
                                             raw_digest,
                                             firmware_identity::kBuildProfile);
}

}  // namespace firmware_identity_adapter
