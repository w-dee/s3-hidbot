#pragma once

#include "firmware_identity/firmware_identity.hpp"

namespace firmware_identity_adapter {

// Construct the validated identity for the currently running ESP-IDF image.
// The adapter is the only layer that knows about esp_app_desc_t; the C1
// firmware_identity component remains portable and descriptor-independent.
bool build_runtime_identity(firmware_identity::Identity *output) noexcept;

}  // namespace firmware_identity_adapter
