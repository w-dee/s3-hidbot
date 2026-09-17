#include "control_protocol/control_protocol.hpp"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <limits>
#include <new>
#include <string_view>

#include "cJSON.h"
#include "secure_memory/secure_memory.hpp"

namespace control_protocol {
namespace {

constexpr std::size_t kPrefixLength = sizeof(control_framing::kFramePrefix) - 1;
constexpr std::size_t kMaxCommandBytes = 48;
constexpr std::size_t kMaxJsonStringBytes = 64;
constexpr std::size_t kMaxJsonObjectMembers = 8;
constexpr std::size_t kMaxJsonArrayMembers = 8;
constexpr std::size_t kMaxJsonDepth = 4;
constexpr std::size_t kMaxMetadataBytes = 32;
constexpr std::size_t kMaxSequenceCodeBytes = 320;

constexpr char kLegacyCapabilityJson[] =
    "[\"protocol.hello-v1\",\"system.ping-v1\",\"system.info-v1\",\"usb.status-v1\",\"usb.exposure-control-v1\",\"hid.lease-v1\",\"hid.release-all-v1\",\"hid.keyboard-report-v1\",\"hid.mouse-report-v1\",\"hid.sequence-v1\",\"hid.output-route-v1\",\"hid.output-route-v2\",\"ble.exposure-control-v1\",\"ble.pairing-transaction-v1\",\"ble.bond-administration-v1\",\"ble.fixture-profile-v1\",\"ble.led-observation-v1\"]";
constexpr char kIdentityCapabilityJson[] =
    "[\"protocol.hello-v1\",\"system.ping-v1\",\"system.info-v1\",\"usb.status-v1\",\"usb.exposure-control-v1\",\"hid.lease-v1\",\"hid.release-all-v1\",\"hid.keyboard-report-v1\",\"hid.mouse-report-v1\",\"hid.sequence-v1\",\"firmware.identity-v1\",\"hid.output-route-v1\",\"hid.output-route-v2\",\"ble.exposure-control-v1\",\"ble.pairing-transaction-v1\",\"ble.bond-administration-v1\",\"ble.fixture-profile-v1\",\"ble.led-observation-v1\"]";
struct ResponseSession {
    bool present;
    std::string_view token;
};

constexpr ResponseSession kUncorrelatableSession{false, {}};
constexpr std::size_t kSessionFieldBytes = control_session::kTokenHexLength + 3;

// Every formatter below writes to ResponseFrame::bytes. These conservative
// bounds cover the largest identity-v1 protocol.hello and system.info responses
// metadata, four 32-hex values (top-level session, result session, boot ID,
// and client nonce), sixteen capabilities, maximum int32 id, framing, and LF.
// vsnprintf still fail-closes if a future format exceeds the buffer.
constexpr std::size_t kMaximumHelloResponseBytes =
    kPrefixLength + 180 + kMaxMetadataBytes +
    control_session::kTokenHexLength * 4 + (sizeof(kIdentityCapabilityJson) - 1) + 32 + 10 + 1;
static_assert(kMaximumHelloResponseBytes <= control_session::kMaxResponseBytes);

// Conservative bound for identity-v1 system.info. It includes the frame
// prefix/envelope, maximum int32 id and session token, three bounded legacy
// metadata strings, the maximum C1 identity fields, JSON punctuation, and LF.
constexpr std::size_t kMaximumInfoResponseBytes =
    kPrefixLength + 320 + 10 + control_session::kTokenHexLength +
    kMaxMetadataBytes * 3 + firmware_identity::kVersionMaxBytes +
    firmware_identity::kSourceRevisionChars + firmware_identity::kAppElfSha256HexChars +
    firmware_identity::kBuildProfileMaxBytes + 1;
static_assert(kMaximumInfoResponseBytes <= control_session::kMaxResponseBytes);
constexpr std::size_t kMaximumReleaseAllResponseBytes =
    kPrefixLength + 150 + control_session::kTokenHexLength + 1;
static_assert(kMaximumReleaseAllResponseBytes <= control_session::kMaxResponseBytes);
constexpr std::size_t kMaximumKeyboardReportResponseBytes =
    kPrefixLength + 150 + control_session::kTokenHexLength + 1;
static_assert(kMaximumKeyboardReportResponseBytes <= control_session::kMaxResponseBytes);
constexpr std::size_t kMaximumMouseReportResponseBytes =
    kPrefixLength + 150 + control_session::kTokenHexLength + 1;
static_assert(kMaximumMouseReportResponseBytes <= control_session::kMaxResponseBytes);
constexpr std::size_t kMaximumUsbExposureResponseBytes =
    kPrefixLength + 360 + control_session::kTokenHexLength + 1;
static_assert(kMaximumUsbExposureResponseBytes <= control_session::kMaxResponseBytes);
constexpr std::size_t kMaximumHidRouteResponseBytes =
    kPrefixLength + 220 + control_session::kTokenHexLength + 1;
static_assert(kMaximumHidRouteResponseBytes <= control_session::kMaxResponseBytes);
constexpr std::size_t kMaximumBleExposureResponseBytes =
    kPrefixLength + 300 + control_session::kTokenHexLength + 1;
static_assert(kMaximumBleExposureResponseBytes <= control_session::kMaxResponseBytes);
constexpr std::size_t kMaximumBleBondListResponseBytes =
    kPrefixLength + 220 + control_session::kTokenHexLength +
    3 * (kBondIdHexChars + 150) + 1;
static_assert(kMaximumBleBondListResponseBytes <=
              control_session::kMaxResponseBytes);

bool is_bounded_string(const char *value, std::size_t maximum_length) {
    return value != nullptr && std::strlen(value) <= maximum_length;
}

bool is_safe_metadata_string(const char *value) {
    if (!is_bounded_string(value, kMaxMetadataBytes)) {
        return false;
    }
    for (const char *character = value; *character != '\0'; ++character) {
        if (*character < 0x20 || *character == '"' || *character == '\\') {
            return false;
        }
    }
    return true;
}

template <typename Array>
bool bounded_array_string(const Array &value,
                          std::size_t maximum_length,
                          std::string_view *result) {
    const auto terminator = std::find(value.begin(), value.end(), '\0');
    if (terminator == value.end()) {
        return false;
    }
    const std::size_t length = static_cast<std::size_t>(terminator - value.begin());
    if (length > maximum_length || result == nullptr) {
        return false;
    }
    *result = std::string_view(value.data(), length);
    return true;
}

bool is_nonzero_hex(std::string_view value) {
    return std::any_of(value.begin(), value.end(), [](char character) {
        return character != '0';
    });
}

bool is_valid_identity(const firmware_identity::Identity *identity) {
    if (identity == nullptr) {
        return false;
    }

    std::string_view version;
    std::string_view source_revision;
    std::string_view app_elf_sha256;
    std::string_view build_profile;
    if (!bounded_array_string(identity->version, firmware_identity::kVersionMaxBytes, &version) ||
        !bounded_array_string(identity->source_revision, firmware_identity::kSourceRevisionChars,
                              &source_revision) ||
        !bounded_array_string(identity->app_elf_sha256,
                              firmware_identity::kAppElfSha256HexChars, &app_elf_sha256) ||
        !bounded_array_string(identity->build_profile, firmware_identity::kBuildProfileMaxBytes,
                              &build_profile)) {
        return false;
    }
    if (!firmware_identity::is_valid_version(version) ||
        !firmware_identity::is_valid_source_revision(
            firmware_identity::SourceRevisionInput{identity->source_revision_present,
                                                   source_revision}) ||
        !firmware_identity::is_valid_app_elf_sha256(app_elf_sha256) ||
        !is_nonzero_hex(app_elf_sha256) ||
        !firmware_identity::is_valid_build_profile(build_profile)) {
        return false;
    }
    return true;
}

bool is_integer_number(const cJSON *item) {
    return cJSON_IsNumber(item) && std::isfinite(item->valuedouble) &&
           std::floor(item->valuedouble) == item->valuedouble;
}

bool parse_id(const cJSON *root, std::int32_t *id) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, "id");
    if (!is_integer_number(item) || item->valuedouble < 0 ||
        item->valuedouble > std::numeric_limits<std::int32_t>::max()) {
        return false;
    }
    *id = static_cast<std::int32_t>(item->valuedouble);
    return true;
}

bool is_json_tree_bounded(const cJSON *item, std::size_t depth,
                          const cJSON *long_sequence_code = nullptr) {
    if (item == nullptr || depth > kMaxJsonDepth) {
        return false;
    }
    if (cJSON_IsString(item) &&
        (item->valuestring == nullptr ||
         std::strlen(item->valuestring) >
             (item == long_sequence_code ? kMaxSequenceCodeBytes
                                         : kMaxJsonStringBytes))) {
        return false;
    }

    if (cJSON_IsObject(item) || cJSON_IsArray(item)) {
        std::size_t count = 0;
        const std::size_t maximum = cJSON_IsObject(item) ? kMaxJsonObjectMembers : kMaxJsonArrayMembers;
        for (const cJSON *child = item->child; child != nullptr; child = child->next) {
            if (++count > maximum ||
                (cJSON_IsObject(item) &&
                 (child->string == nullptr || std::strlen(child->string) > kMaxJsonStringBytes)) ||
                !is_json_tree_bounded(child, depth + 1, long_sequence_code)) {
                return false;
            }
        }
    }
    return true;
}

bool object_has_no_duplicate_keys(const cJSON *item) {
    if (item == nullptr) {
        return false;
    }
    if (cJSON_IsObject(item)) {
        for (const cJSON *left = item->child; left != nullptr; left = left->next) {
            if (left->string == nullptr) {
                return false;
            }
            for (const cJSON *right = left->next; right != nullptr; right = right->next) {
                if (right->string == nullptr || std::strcmp(left->string, right->string) == 0) {
                    return false;
                }
            }
        }
    }
    for (const cJSON *child = item->child; child != nullptr; child = child->next) {
        if (!object_has_no_duplicate_keys(child)) {
            return false;
        }
    }
    return true;
}

void wipe_json_strings(cJSON *item) {
    if (item == nullptr) {
        return;
    }
    if (item->valuestring != nullptr) {
        // cJSON strings may contain decoded NUL bytes, so strlen() is not a
        // sufficient bound for secret erasure. valuestring is always a direct
        // cJSON allocator result; wipe its complete allocation before delete.
        secure_memory::zero_allocation(item->valuestring);
    }
    for (cJSON *child = item->child; child != nullptr; child = child->next) {
        wipe_json_strings(child);
    }
}

bool object_has_only_fields(const cJSON *object,
                            const char *const *allowed,
                            std::size_t allowed_count) {
    if (!cJSON_IsObject(object)) {
        return false;
    }
    for (const cJSON *child = object->child; child != nullptr; child = child->next) {
        bool found = false;
        for (std::size_t index = 0; index < allowed_count; ++index) {
            if (std::strcmp(child->string, allowed[index]) == 0) {
                found = true;
                break;
            }
        }
        if (!found) {
            return false;
        }
    }
    return true;
}

bool get_bounded_nonempty_string(const cJSON *object,
                                 const char *name,
                                 std::size_t maximum_length,
                                 std::string_view *value) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (!cJSON_IsString(item) || item->valuestring == nullptr) {
        return false;
    }
    const std::size_t length = std::strlen(item->valuestring);
    if (length == 0 || length > maximum_length) {
        return false;
    }
    *value = std::string_view(item->valuestring, length);
    return true;
}

bool format_frame(control_session::ResponseFrame *frame, const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    const int length = std::vsnprintf(reinterpret_cast<char *>(frame->bytes),
                                      sizeof(frame->bytes),
                                      format,
                                      arguments);
    va_end(arguments);
    if (length < 0 || static_cast<std::size_t>(length) >= sizeof(frame->bytes)) {
        frame->length = 0;
        return false;
    }
    frame->length = static_cast<std::size_t>(length);
    return true;
}

bool append_frame(control_session::ResponseFrame *frame,
                  const char *format, ...) {
    if (frame == nullptr || frame->length >= sizeof(frame->bytes)) {
        return false;
    }
    va_list arguments;
    va_start(arguments, format);
    const int length = std::vsnprintf(
        reinterpret_cast<char *>(frame->bytes + frame->length),
        sizeof(frame->bytes) - frame->length, format, arguments);
    va_end(arguments);
    if (length < 0 ||
        static_cast<std::size_t>(length) >=
            sizeof(frame->bytes) - frame->length) {
        frame->length = 0;
        return false;
    }
    frame->length += static_cast<std::size_t>(length);
    return true;
}

bool format_session_field(char output[kSessionFieldBytes], ResponseSession session) {
    if (!session.present) {
        std::memcpy(output, "null", sizeof("null"));
        return true;
    }
    if (!control_session::is_lower_hex_token(session.token)) {
        return false;
    }
    const int length = std::snprintf(output,
                                     kSessionFieldBytes,
                                     "\"%.*s\"",
                                     static_cast<int>(session.token.size()),
                                     session.token.data());
    return length == static_cast<int>(control_session::kTokenHexLength + 2);
}

bool is_lower_hex_token_cstr(const char *value) {
    return value != nullptr &&
           control_session::is_lower_hex_token(std::string_view(value));
}

bool make_error(control_session::ResponseFrame *frame,
                ResponseSession session,
                bool has_id,
                std::int32_t id,
                const char *code,
                const char *message) {
    char session_field[kSessionFieldBytes]{};
    if (!format_session_field(session_field, session)) {
        frame->length = 0;
        return false;
    }
    if (has_id) {
        return format_frame(frame,
                            "@HIDBOT {\"type\":\"response\",\"v\":1,\"id\":%ld,"
                            "\"session\":%s,\"ok\":false,"
                            "\"error\":{\"code\":\"%s\",\"message\":\"%s\"}}\n",
                            static_cast<long>(id),
                            session_field,
                            code,
                            message);
    }
    return format_frame(frame,
                        "@HIDBOT {\"type\":\"response\",\"v\":1,\"id\":null,"
                        "\"session\":%s,\"ok\":false,"
                        "\"error\":{\"code\":\"%s\",\"message\":\"%s\"}}\n",
                        session_field,
                        code,
                        message);
}

bool make_ping(control_session::ResponseFrame *frame,
               ResponseSession session,
               std::int32_t id) {
    char session_field[kSessionFieldBytes]{};
    if (!format_session_field(session_field, session)) {
        frame->length = 0;
        return false;
    }
    return format_frame(frame,
                        "@HIDBOT {\"type\":\"response\",\"v\":1,\"id\":%ld,"
                        "\"session\":%s,\"ok\":true,"
                        "\"result\":{\"pong\":true}}\n",
                        static_cast<long>(id),
                        session_field);
}

bool make_info(control_session::ResponseFrame *frame,
               ResponseSession session,
               std::int32_t id,
               const Metadata &metadata) {
    char session_field[kSessionFieldBytes]{};
    if (!format_session_field(session_field, session)) {
        frame->length = 0;
        return false;
    }
    if (metadata.firmware_identity == nullptr) {
        return format_frame(frame,
                            "@HIDBOT {\"type\":\"response\",\"v\":1,\"id\":%ld,"
                            "\"session\":%s,\"ok\":true,\"result\":{"
                            "\"project\":\"%s\",\"target\":\"%s\",\"idf_version\":\"%s\","
                            "\"protocol_version\":1}}\n",
                            static_cast<long>(id),
                            session_field,
                            metadata.project,
                            metadata.target,
                            metadata.idf_version);
    }
    if (!is_valid_identity(metadata.firmware_identity)) {
        frame->length = 0;
        return false;
    }
    const firmware_identity::Identity &identity = *metadata.firmware_identity;
    if (!identity.source_revision_present) {
        return format_frame(frame,
                            "@HIDBOT {\"type\":\"response\",\"v\":1,\"id\":%ld,"
                            "\"session\":%s,\"ok\":true,\"result\":{"
                            "\"project\":\"%s\",\"target\":\"%s\",\"idf_version\":\"%s\","
                            "\"protocol_version\":1,\"firmware\":{"
                            "\"version\":\"%s\",\"source_revision\":null,"
                            "\"app_elf_sha256\":\"%s\",\"build_profile\":\"%s\"}}}\n",
                            static_cast<long>(id),
                            session_field,
                            metadata.project,
                            metadata.target,
                            metadata.idf_version,
                            identity.version.data(),
                            identity.app_elf_sha256.data(),
                            identity.build_profile.data());
    }
    return format_frame(frame,
                        "@HIDBOT {\"type\":\"response\",\"v\":1,\"id\":%ld,"
                        "\"session\":%s,\"ok\":true,\"result\":{"
                        "\"project\":\"%s\",\"target\":\"%s\",\"idf_version\":\"%s\","
                        "\"protocol_version\":1,\"firmware\":{"
                        "\"version\":\"%s\",\"source_revision\":\"%s\","
                        "\"app_elf_sha256\":\"%s\",\"build_profile\":\"%s\"}}}\n",
                        static_cast<long>(id),
                        session_field,
                        metadata.project,
                        metadata.target,
                        metadata.idf_version,
                        identity.version.data(),
                        identity.source_revision.data(),
                        identity.app_elf_sha256.data(),
                        identity.build_profile.data());
}

const char *json_bool(bool value) {
    return value ? "true" : "false";
}

bool make_usb_status(control_session::ResponseFrame *frame,
                     ResponseSession session,
                     std::int32_t id,
                     UsbStatus status) {
    char session_field[kSessionFieldBytes]{};
    if (!format_session_field(session_field, session)) {
        frame->length = 0;
        return false;
    }
    return format_frame(frame,
                        "@HIDBOT {\"type\":\"response\",\"v\":1,\"id\":%ld,"
                        "\"session\":%s,\"ok\":true,\"result\":{"
                        "\"mounted\":%s,\"suspended\":%s,\"keyboard_ready\":%s,\"mouse_ready\":%s}}\n",
                        static_cast<long>(id),
                        session_field,
                        json_bool(status.mounted),
                        json_bool(status.suspended),
                        json_bool(status.keyboard_ready),
                        json_bool(status.mouse_ready));
}

const char *usb_exposure_desired_json(UsbExposureDesired desired) {
    return desired == UsbExposureDesired::kExposed ? "exposed" : "hidden";
}

const char *usb_exposure_observed_json(UsbExposureObserved observed) {
    switch (observed) {
        case UsbExposureObserved::kDisconnected:
            return "disconnected";
        case UsbExposureObserved::kAttaching:
            return "attaching";
        case UsbExposureObserved::kMounted:
            return "mounted";
        case UsbExposureObserved::kSuspended:
            return "suspended";
        case UsbExposureObserved::kDetaching:
            return "detaching";
        case UsbExposureObserved::kDriverNotInstalled:
        default:
            return "driver_not_installed";
    }
}

const char *usb_exposure_operation_json(UsbExposureOperation operation) {
    switch (operation) {
        case UsbExposureOperation::kUninstall:
            return "uninstall";
        case UsbExposureOperation::kRuntime:
            return "runtime";
        case UsbExposureOperation::kInstall:
        default:
            return "install";
    }
}

bool make_usb_exposure_status(control_session::ResponseFrame *frame,
                              ResponseSession session,
                              std::int32_t id,
                              UsbExposureStatus status) {
    char session_field[kSessionFieldBytes]{};
    if (!format_session_field(session_field, session)) {
        frame->length = 0;
        return false;
    }
    if (!status.last_error.present) {
        return format_frame(frame,
                            "@HIDBOT {\"type\":\"response\",\"v\":1,\"id\":%ld,"
                            "\"session\":%s,\"ok\":true,\"result\":{"
                            "\"desired\":\"%s\",\"observed\":\"%s\",\"generation\":%lu,"
                            "\"mounted\":%s,\"suspended\":%s,\"keyboard_ready\":%s,\"mouse_ready\":%s,"
                            "\"safety_pending\":%s,\"host_release_uncertain\":%s,"
                            "\"recovery_required\":%s,\"last_error\":null}}\n",
                            static_cast<long>(id), session_field,
                            usb_exposure_desired_json(status.desired),
                            usb_exposure_observed_json(status.observed),
                            static_cast<unsigned long>(status.generation),
                            json_bool(status.mounted), json_bool(status.suspended),
                            json_bool(status.keyboard_ready), json_bool(status.mouse_ready),
                            json_bool(status.safety_pending),
                            json_bool(status.host_release_uncertain),
                            json_bool(status.recovery_required));
    }
    return format_frame(frame,
                        "@HIDBOT {\"type\":\"response\",\"v\":1,\"id\":%ld,"
                        "\"session\":%s,\"ok\":true,\"result\":{"
                        "\"desired\":\"%s\",\"observed\":\"%s\",\"generation\":%lu,"
                        "\"mounted\":%s,\"suspended\":%s,\"keyboard_ready\":%s,\"mouse_ready\":%s,"
                        "\"safety_pending\":%s,\"host_release_uncertain\":%s,"
                        "\"recovery_required\":%s,\"last_error\":{\"operation\":\"%s\",\"code\":%ld}}}\n",
                        static_cast<long>(id), session_field,
                        usb_exposure_desired_json(status.desired),
                        usb_exposure_observed_json(status.observed),
                        static_cast<unsigned long>(status.generation),
                        json_bool(status.mounted), json_bool(status.suspended),
                        json_bool(status.keyboard_ready), json_bool(status.mouse_ready),
                        json_bool(status.safety_pending),
                        json_bool(status.host_release_uncertain),
                        json_bool(status.recovery_required),
                        usb_exposure_operation_json(status.last_error.operation),
                        static_cast<long>(status.last_error.code));
}

const char *ble_exposure_desired_json(BleExposureDesired desired) {
    return desired == BleExposureDesired::kExposed ? "exposed" : "hidden";
}

const char *ble_exposure_observed_json(BleExposureObserved observed) {
    switch (observed) {
        case BleExposureObserved::kEnabling: return "enabling";
        case BleExposureObserved::kIdle: return "idle";
        case BleExposureObserved::kAdvertising: return "advertising";
        case BleExposureObserved::kConnected: return "connected";
        case BleExposureObserved::kDisabling: return "disabling";
        case BleExposureObserved::kFault: return "fault";
        case BleExposureObserved::kUninitialized:
        default: return "uninitialized";
    }
}

const char *ble_exposure_operation_json(BleExposureOperation operation) {
    switch (operation) {
        case BleExposureOperation::kEnable: return "enable";
        case BleExposureOperation::kDisable: return "disable";
        case BleExposureOperation::kRuntime:
        default: return "runtime";
    }
}

bool make_profile_status(control_session::ResponseFrame *frame,
                         ResponseSession session, std::int32_t id,
                         ble_fixture_profile::SelectionSnapshot status) {
    char session_field[kSessionFieldBytes]{};
    const auto *selected = ble_fixture_profile::find_profile(status.selected);
    const auto *active = ble_fixture_profile::find_profile(status.active);
    if (!format_session_field(session_field, session) || selected == nullptr ||
        (status.active_present && active == nullptr)) return false;
    char active_json[64]{};
    std::snprintf(active_json, sizeof(active_json), status.active_present ? "\"%s\"" : "null",
                  active == nullptr ? "" : active->name);
    using Transition = ble_fixture_profile::SelectionTransition;
    const char *transition = status.transition == Transition::kFault ? "fault"
        : status.transition == Transition::kInitializing ? "initializing" : "stable";
    return format_frame(frame,
        "@HIDBOT {\"type\":\"response\",\"v\":1,\"id\":%ld,"
        "\"session\":%s,\"ok\":true,\"result\":{\"selected\":\"%s\","
        "\"active\":%s,\"transition\":\"%s\"}}\n",
        static_cast<long>(id), session_field, selected->name, active_json, transition);
}

bool make_led_status(control_session::ResponseFrame *frame,
                     ResponseSession session, std::int32_t id,
                     ble_fixture_profile::LedStatus status) {
    char session_field[kSessionFieldBytes]{};
    if (!format_session_field(session_field, session) || status.leds > 31 ||
        (!status.supported && status.valid) || (!status.valid && status.leds != 0)) return false;
    return format_frame(frame,
        "@HIDBOT {\"type\":\"response\",\"v\":1,\"id\":%ld,"
        "\"session\":%s,\"ok\":true,\"result\":{\"supported\":%s,\"valid\":%s,\"leds\":%u}}\n",
        static_cast<long>(id), session_field, status.supported ? "true" : "false",
        status.valid ? "true" : "false", static_cast<unsigned>(status.leds));
}

bool make_profile_list(control_session::ResponseFrame *frame,
                       ResponseSession session, std::int32_t id) {
    char session_field[kSessionFieldBytes]{};
    if (!format_session_field(session_field, session)) return false;
    char entries[800]{};
    std::size_t used = 0;
    for (const auto *profile : ble_fixture_profile::kCatalog) {
        char digest[65]{};
        for (std::size_t i = 0; i < profile->report_map_sha256.size(); ++i) {
            std::snprintf(digest + i * 2, 3, "%02x", profile->report_map_sha256[i]);
        }
        const int written = std::snprintf(entries + used, sizeof(entries) - used,
            "%s{\"id\":\"%s\",\"rev\":%u,\"schema\":%u,\"map\":\"%s\","
            "\"bond\":%u,\"identity\":%u}", used == 0 ? "" : ",", profile->name,
            static_cast<unsigned>(profile->revision),
            static_cast<unsigned>(profile->cache.schema_revision), digest,
            static_cast<unsigned>(profile->bond_class),
            static_cast<unsigned>(profile->identity_class));
        if (written < 0 || static_cast<std::size_t>(written) >= sizeof(entries) - used) return false;
        used += static_cast<std::size_t>(written);
    }
    return format_frame(frame,
        "@HIDBOT {\"type\":\"response\",\"v\":1,\"id\":%ld,"
        "\"session\":%s,\"ok\":true,\"result\":{\"profiles\":[%s]}}\n",
        static_cast<long>(id), session_field, entries);
}

bool make_ble_exposure_status(control_session::ResponseFrame *frame,
                              ResponseSession session, std::int32_t id,
                              BleExposureStatus status) {
    char session_field[kSessionFieldBytes]{};
    if (!format_session_field(session_field, session)) {
        frame->length = 0;
        return false;
    }
    if (!status.last_error.present) {
        return format_frame(frame,
                            "@HIDBOT {\"type\":\"response\",\"v\":1,\"id\":%ld,"
                            "\"session\":%s,\"ok\":true,\"result\":{"
                            "\"desired\":\"%s\",\"observed\":\"%s\",\"generation\":%lu,"
                            "\"stack_ready\":%s,\"advertising\":%s,\"connected\":%s,"
                            "\"recovery_required\":%s,\"last_error\":null}}\n",
                            static_cast<long>(id), session_field,
                            ble_exposure_desired_json(status.desired),
                            ble_exposure_observed_json(status.observed),
                            static_cast<unsigned long>(status.generation),
                            json_bool(status.stack_ready), json_bool(status.advertising),
                            json_bool(status.connected), json_bool(status.recovery_required));
    }
    return format_frame(frame,
                        "@HIDBOT {\"type\":\"response\",\"v\":1,\"id\":%ld,"
                        "\"session\":%s,\"ok\":true,\"result\":{"
                        "\"desired\":\"%s\",\"observed\":\"%s\",\"generation\":%lu,"
                        "\"stack_ready\":%s,\"advertising\":%s,\"connected\":%s,"
                        "\"recovery_required\":%s,\"last_error\":{"
                        "\"operation\":\"%s\",\"code\":%ld}}}\n",
                        static_cast<long>(id), session_field,
                        ble_exposure_desired_json(status.desired),
                        ble_exposure_observed_json(status.observed),
                        static_cast<unsigned long>(status.generation),
                        json_bool(status.stack_ready), json_bool(status.advertising),
                        json_bool(status.connected), json_bool(status.recovery_required),
                        ble_exposure_operation_json(status.last_error.operation),
                        static_cast<long>(status.last_error.code));
}

const char *output_route_json(OutputRoute route) {
    return route == OutputRoute::kUsb ? "usb"
         : route == OutputRoute::kBle ? "ble" : "none";
}

const char *route_transition_json(RouteTransition transition) {
    return transition == RouteTransition::kReleasing ? "releasing" : "stable";
}

bool make_hid_route_status(control_session::ResponseFrame *frame,
                           ResponseSession session,
                           std::int32_t id,
                           HidRouteStatus status) {
    char session_field[kSessionFieldBytes]{};
    if (!format_session_field(session_field, session)) {
        frame->length = 0;
        return false;
    }
    return format_frame(frame,
                        "@HIDBOT {\"type\":\"response\",\"v\":1,\"id\":%ld,"
                        "\"session\":%s,\"ok\":true,\"result\":{"
                        "\"desired\":\"%s\",\"active\":\"%s\",\"generation\":%lu,"
                        "\"transition\":\"%s\",\"ready\":%s}}\n",
                        static_cast<long>(id), session_field,
                        output_route_json(status.desired),
                        output_route_json(status.active),
                        static_cast<unsigned long>(status.generation),
                        route_transition_json(status.transition),
                        json_bool(status.ready));
}

bool make_versioned_hid_route_status(control_session::ResponseFrame *frame,
                                     ResponseSession session, std::int32_t id,
                                     HidRouteStatus status, bool route_v2) {
    return !route_v2 && status.active == OutputRoute::kBle
               ? make_error(frame, session, true, id,
                            "HID_ROUTE_V2_REQUIRED", "v2 required")
               : make_hid_route_status(frame, session, id, status);
}

const char *release_state_json(ReleaseAllInterfaceState state) {
    return state == ReleaseAllInterfaceState::kSubmitted ? "submitted" : "already_up";
}

bool make_release_all(control_session::ResponseFrame *frame,
                      ResponseSession session,
                      std::int32_t id,
                      ReleaseAllResult result) {
    char session_field[kSessionFieldBytes]{};
    if (!format_session_field(session_field, session)) {
        frame->length = 0;
        return false;
    }
    return format_frame(frame,
                        "@HIDBOT {\"type\":\"response\",\"v\":1,\"id\":%ld,"
                        "\"session\":%s,\"ok\":true,\"result\":{"
                        "\"keyboard\":\"%s\",\"mouse\":\"%s\"}}\n",
                        static_cast<long>(id), session_field,
                        release_state_json(result.keyboard),
                        release_state_json(result.mouse));
}

bool make_hello(control_session::ResponseFrame *frame,
                ResponseSession session,
                std::int32_t id,
                const Metadata &metadata,
                const char *client_nonce,
                const char *boot_id,
                const char *result_session) {
    char session_field[kSessionFieldBytes]{};
    if (!format_session_field(session_field, session) ||
        !is_lower_hex_token_cstr(client_nonce) ||
        !is_lower_hex_token_cstr(boot_id) ||
        !is_lower_hex_token_cstr(result_session) ||
        (metadata.firmware_identity != nullptr && !is_valid_identity(metadata.firmware_identity))) {
        frame->length = 0;
        return false;
    }
    return format_frame(frame,
                        "@HIDBOT {\"type\":\"response\",\"v\":1,\"id\":%ld,"
                        "\"session\":%s,\"ok\":true,\"result\":{"
                        "\"project\":\"%s\",\"protocol_version\":1,"
                        "\"client_nonce\":\"%s\",\"boot_id\":\"%s\","
                        "\"session\":\"%s\",\"lease_ms\":%lu,\"capabilities\":%s}}\n",
                        static_cast<long>(id),
                        session_field,
                        metadata.project,
                        client_nonce,
                        boot_id,
                        result_session,
                        static_cast<unsigned long>(control_session::kLeaseMilliseconds),
                        metadata.firmware_identity != nullptr ? kIdentityCapabilityJson
                                                               : kLegacyCapabilityJson);
}

bool validate_no_params(const cJSON *params) {
    if (params == nullptr) {
        return true;
    }
    return cJSON_IsObject(params) && params->child == nullptr;
}

bool validate_hello_params(const cJSON *params, std::string_view *client_nonce) {
    static constexpr const char *kFields[] = {"client_nonce"};
    if (!cJSON_IsObject(params) || !object_has_only_fields(params, kFields, 1) ||
        !get_bounded_nonempty_string(params, "client_nonce", control_session::kTokenHexLength,
                                     client_nonce)) {
        return false;
    }
    return control_session::is_lower_hex_token(*client_nonce);
}

bool validate_pairing_respond_params(const cJSON *params,
                                     BlePairingRespondRequest *request) {
    static constexpr const char *kFields[] = {"pairing_id", "passkey"};
    if (request == nullptr || !cJSON_IsObject(params) ||
        !object_has_only_fields(params, kFields, 2) ||
        cJSON_GetArraySize(params) != 2) {
        return false;
    }
    const cJSON *pairing_id =
        cJSON_GetObjectItemCaseSensitive(params, "pairing_id");
    if (!is_integer_number(pairing_id) || pairing_id->valuedouble < 1 ||
        pairing_id->valuedouble > std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }
    std::string_view passkey;
    if (!get_bounded_nonempty_string(params, "passkey", 6, &passkey) ||
        passkey.size() != request->passkey.size()) {
        return false;
    }
    for (std::size_t index = 0; index < passkey.size(); ++index) {
        if (passkey[index] < '0' || passkey[index] > '9') {
            return false;
        }
        request->passkey[index] = passkey[index];
    }
    request->pairing_id = static_cast<std::uint32_t>(pairing_id->valuedouble);
    return true;
}

bool valid_bond_id(const BondId &bond_id) {
    return bond_id[kBondIdHexChars] == '\0' &&
           control_session::is_lower_hex_token(
               std::string_view(bond_id.data(), kBondIdHexChars));
}

bool validate_bond_remove_params(const cJSON *params, BondId *bond_id) {
    static constexpr const char *kFields[] = {"bond_id"};
    std::string_view value;
    if (bond_id == nullptr || !cJSON_IsObject(params) ||
        !object_has_only_fields(params, kFields, 1) ||
        cJSON_GetArraySize(params) != 1 ||
        !get_bounded_nonempty_string(params, "bond_id", kBondIdHexChars,
                                     &value) ||
        value.size() != kBondIdHexChars ||
        !control_session::is_lower_hex_token(value)) {
        return false;
    }
    std::memcpy(bond_id->data(), value.data(), value.size());
    (*bond_id)[kBondIdHexChars] = '\0';
    return true;
}

const char *pairing_state_json(BlePairingState state) {
    switch (state) {
        case BlePairingState::kSecuring: return "securing";
        case BlePairingState::kWaitingInput: return "waiting_input";
        case BlePairingState::kIdle:
        default: return "idle";
    }
}

const char *pairing_last_result_json(BlePairingLastResult result) {
    switch (result) {
        case BlePairingLastResult::kSucceeded: return "succeeded";
        case BlePairingLastResult::kSmpFailed: return "smp_failed";
        case BlePairingLastResult::kTimeout: return "timeout";
        case BlePairingLastResult::kPeerDisconnected: return "peer_disconnected";
        case BlePairingLastResult::kStoreFull: return "store_full";
        case BlePairingLastResult::kStorage: return "storage";
        case BlePairingLastResult::kQueueOverflow: return "queue_overflow";
        case BlePairingLastResult::kRepeatPairing: return "repeat_pairing";
        case BlePairingLastResult::kSecurityPolicy: return "security_policy";
        case BlePairingLastResult::kNone:
        default: return "none";
    }
}

bool make_pairing_status(control_session::ResponseFrame *frame,
                         ResponseSession session, std::int32_t id,
                         BlePairingStatus status) {
    char session_field[kSessionFieldBytes]{};
    if (!format_session_field(session_field, session)) {
        frame->length = 0;
        return false;
    }
    if (!status.available) {
        return make_error(frame, session, true, id, "INTERNAL_ERROR",
                          "pairing status is unavailable");
    }
    const bool waiting = status.state == BlePairingState::kWaitingInput;
    if (waiting && (!status.input_pending || status.pairing_id == 0)) {
        frame->length = 0;
        return false;
    }
    if (!waiting) {
        return format_frame(
            frame,
            "@HIDBOT {\"type\":\"response\",\"v\":1,\"id\":%ld,"
            "\"session\":%s,\"ok\":true,\"result\":{"
            "\"state\":\"%s\",\"generation\":%lu,\"connected\":%s,"
            "\"pairing_id\":null,\"action\":null,\"remaining_ms\":null,"
            "\"encrypted\":%s,\"authenticated\":%s,\"bonded\":%s,"
            "\"secure_connections\":%s,\"key_size\":%u,\"last_result\":\"%s\"}}\n",
            static_cast<long>(id), session_field,
            pairing_state_json(status.state),
            static_cast<unsigned long>(status.generation),
            json_bool(status.connected), json_bool(status.encrypted),
            json_bool(status.authenticated), json_bool(status.bonded),
            json_bool(status.secure_connections),
            static_cast<unsigned>(status.key_size),
            pairing_last_result_json(status.last_result));
    }
    return format_frame(
        frame,
        "@HIDBOT {\"type\":\"response\",\"v\":1,\"id\":%ld,"
        "\"session\":%s,\"ok\":true,\"result\":{"
        "\"state\":\"waiting_input\",\"generation\":%lu,\"connected\":%s,"
        "\"pairing_id\":%lu,\"action\":\"passkey_input\",\"remaining_ms\":%lu,"
        "\"encrypted\":%s,\"authenticated\":%s,\"bonded\":%s,"
        "\"secure_connections\":%s,\"key_size\":%u,\"last_result\":\"%s\"}}\n",
        static_cast<long>(id), session_field,
        static_cast<unsigned long>(status.generation),
        json_bool(status.connected), static_cast<unsigned long>(status.pairing_id),
        static_cast<unsigned long>(status.remaining_ms), json_bool(status.encrypted),
        json_bool(status.authenticated), json_bool(status.bonded),
        json_bool(status.secure_connections), static_cast<unsigned>(status.key_size),
        pairing_last_result_json(status.last_result));
}

bool make_pairing_respond(control_session::ResponseFrame *frame,
                          ResponseSession session, std::int32_t id,
                          std::uint32_t pairing_id) {
    char session_field[kSessionFieldBytes]{};
    if (!format_session_field(session_field, session)) {
        frame->length = 0;
        return false;
    }
    return format_frame(frame,
                        "@HIDBOT {\"type\":\"response\",\"v\":1,\"id\":%ld,"
                        "\"session\":%s,\"ok\":true,\"result\":{"
                        "\"accepted\":true,\"pairing_id\":%lu}}\n",
                        static_cast<long>(id), session_field,
                        static_cast<unsigned long>(pairing_id));
}

bool make_bond_list(control_session::ResponseFrame *frame,
                    ResponseSession session, std::int32_t id,
                    const BleBondListResult &result) {
    char session_field[kSessionFieldBytes]{};
    if (!format_session_field(session_field, session) || result.count > 3 ||
        result.available != 3U - result.count) {
        frame->length = 0;
        return false;
    }
    if (!format_frame(frame,
                      "@HIDBOT {\"type\":\"response\",\"v\":1,\"id\":%ld,"
                      "\"session\":%s,\"ok\":true,\"result\":{"
                      "\"capacity\":3,\"count\":%u,\"available\":%u,"
                      "\"healthy\":%s,\"bonds\":[",
                      static_cast<long>(id), session_field,
                      static_cast<unsigned>(result.count),
                      static_cast<unsigned>(result.available),
                      json_bool(result.healthy))) {
        return false;
    }
    bool expected_healthy = true;
    for (std::size_t index = 0; index < result.count; ++index) {
        const auto &bond = result.bonds[index];
        if (!valid_bond_id(bond.bond_id) ||
            (bond.schema_current && !bond.schema_revision_present) ||
            (bond.verified && (!bond.our_sec || !bond.peer_sec)) ||
            (index != 0 && std::strcmp(result.bonds[index - 1].bond_id.data(),
                                       bond.bond_id.data()) >= 0)) {
            frame->length = 0;
            return false;
        }
        expected_healthy = expected_healthy && bond.verified;
        if (!append_frame(
                frame,
                "%s{\"bond_id\":\"%s\",\"our_sec\":%s,\"peer_sec\":%s,"
                "\"verified\":%s,\"schema_revision\":",
                index == 0 ? "" : ",", bond.bond_id.data(),
                json_bool(bond.our_sec), json_bool(bond.peer_sec),
                json_bool(bond.verified))) {
            return false;
        }
        if (bond.schema_revision_present) {
            if (!append_frame(frame, "%u",
                              static_cast<unsigned>(bond.schema_revision))) {
                return false;
            }
        } else if (!append_frame(frame, "null")) {
            return false;
        }
        if (!append_frame(frame,
                          ",\"schema_current\":%s,\"connected\":%s}",
                          json_bool(bond.schema_current),
                          json_bool(bond.connected))) {
            return false;
        }
    }
    if (result.healthy != expected_healthy) {
        frame->length = 0;
        return false;
    }
    return append_frame(frame, "]}}\n");
}

bool make_bond_remove(control_session::ResponseFrame *frame,
                      ResponseSession session, std::int32_t id,
                      const BleBondRemoveResult &result) {
    char session_field[kSessionFieldBytes]{};
    if (!format_session_field(session_field, session) ||
        !valid_bond_id(result.bond_id) || result.remaining >= 3) {
        frame->length = 0;
        return false;
    }
    return format_frame(frame,
                        "@HIDBOT {\"type\":\"response\",\"v\":1,\"id\":%ld,"
                        "\"session\":%s,\"ok\":true,\"result\":{"
                        "\"bond_id\":\"%s\",\"removed\":true,"
                        "\"remaining\":%u}}\n",
                        static_cast<long>(id), session_field,
                        result.bond_id.data(),
                        static_cast<unsigned>(result.remaining));
}

bool validate_route_set_params(const cJSON *params, bool allow_ble,
                               OutputRoute *route) {
    static constexpr const char *kFields[] = {"route"};
    std::string_view value;
    if (route == nullptr || !cJSON_IsObject(params) ||
        !object_has_only_fields(params, kFields, 1) ||
        !get_bounded_nonempty_string(params, "route", 4, &value)) {
        return false;
    }
    if (value == "none") {
        *route = OutputRoute::kNone;
        return true;
    }
    if (value == "usb") {
        *route = OutputRoute::kUsb;
        return true;
    }
    if (allow_ble && value == "ble") {
        *route = OutputRoute::kBle;
        return true;
    }
    return false;
}

bool allowed_keyboard_usage(std::uint8_t usage) {
    return (usage >= 0x04U && usage <= 0xA4U) ||
           (usage >= 0xB0U && usage <= 0xDDU);
}

bool validate_keyboard_report_params(const cJSON *params,
                                    KeyboardReportRequest *request) {
    static constexpr const char *kFields[] = {"modifiers", "keys"};
    if (request == nullptr || !cJSON_IsObject(params) ||
        !object_has_only_fields(params, kFields, 2)) {
        return false;
    }
    const cJSON *modifiers = cJSON_GetObjectItemCaseSensitive(params, "modifiers");
    if (!is_integer_number(modifiers) || modifiers->valuedouble < 0 ||
        modifiers->valuedouble > 255) {
        return false;
    }
    const cJSON *keys = cJSON_GetObjectItemCaseSensitive(params, "keys");
    if (!cJSON_IsArray(keys) || cJSON_GetArraySize(keys) < 0 ||
        cJSON_GetArraySize(keys) > 6) {
        return false;
    }
    request->modifiers = static_cast<std::uint8_t>(modifiers->valuedouble);
    request->keycodes = {};
    int previous = -1;
    int key_index = 0;
    for (const cJSON *key = keys->child; key != nullptr; key = key->next) {
        if (!is_integer_number(key) || key->valuedouble < 0 || key->valuedouble > 255) {
            return false;
        }
        const int value = static_cast<int>(key->valuedouble);
        if (value <= previous || !allowed_keyboard_usage(static_cast<std::uint8_t>(value))) {
            return false;
        }
        request->keycodes[static_cast<std::size_t>(key_index++)] =
            static_cast<std::uint8_t>(value);
        previous = value;
    }
    return true;
}

bool validate_mouse_report_params(const cJSON *params,
                                  MouseReportRequest *request) {
    static constexpr const char *kFields[] = {"buttons", "x", "y", "wheel", "pan"};
    if (request == nullptr || !cJSON_IsObject(params) ||
        !object_has_only_fields(params, kFields, 5)) {
        return false;
    }
    const cJSON *buttons = cJSON_GetObjectItemCaseSensitive(params, "buttons");
    const cJSON *x = cJSON_GetObjectItemCaseSensitive(params, "x");
    const cJSON *y = cJSON_GetObjectItemCaseSensitive(params, "y");
    const cJSON *wheel = cJSON_GetObjectItemCaseSensitive(params, "wheel");
    const cJSON *pan = cJSON_GetObjectItemCaseSensitive(params, "pan");
    if (!is_integer_number(buttons) || buttons->valuedouble < 0 || buttons->valuedouble > 31 ||
        !is_integer_number(x) || x->valuedouble < -127 || x->valuedouble > 127 ||
        !is_integer_number(y) || y->valuedouble < -127 || y->valuedouble > 127 ||
        !is_integer_number(wheel) || wheel->valuedouble < -127 || wheel->valuedouble > 127 ||
        !is_integer_number(pan) || pan->valuedouble < -127 || pan->valuedouble > 127) {
        return false;
    }
    request->buttons = static_cast<std::uint8_t>(buttons->valuedouble);
    request->x = static_cast<std::int8_t>(x->valuedouble);
    request->y = static_cast<std::int8_t>(y->valuedouble);
    request->wheel = static_cast<std::int8_t>(wheel->valuedouble);
    request->pan = static_cast<std::int8_t>(pan->valuedouble);
    return true;
}

bool validate_sequence_start_params(const cJSON *params,
                                    std::string_view raw_payload,
                                    std::string_view *code) {
    static constexpr const char *kFields[] = {"code"};
    if (code == nullptr || !cJSON_IsObject(params) ||
        !object_has_only_fields(params, kFields, 1) ||
        raw_payload.find('\\') != std::string_view::npos) {
        return false;
    }
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(params, "code");
    if (!cJSON_IsString(value) || value->valuestring == nullptr) return false;
    const std::size_t length = std::strlen(value->valuestring);
    if (length == 0 || length > kMaxSequenceCodeBytes) return false;
    *code = std::string_view(value->valuestring, length);
    return true;
}

bool validate_sequence_status_params(const cJSON *params,
                                     std::int32_t *sequence_id) {
    static constexpr const char *kFields[] = {"sequence_id"};
    if (sequence_id == nullptr || !cJSON_IsObject(params) ||
        !object_has_only_fields(params, kFields, 1)) return false;
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(params, "sequence_id");
    if (!is_integer_number(value) || value->valuedouble < 0 ||
        value->valuedouble > std::numeric_limits<std::int32_t>::max()) return false;
    *sequence_id = static_cast<std::int32_t>(value->valuedouble);
    return true;
}

bool make_sequence_start(control_session::ResponseFrame *frame,
                         ResponseSession session, std::int32_t id) {
    char session_field[kSessionFieldBytes]{};
    if (!format_session_field(session_field, session)) return false;
    return format_frame(frame,
                        "@HIDBOT {\"type\":\"response\",\"v\":1,\"id\":%ld,"
                        "\"session\":%s,\"ok\":true,\"result\":{"
                        "\"sequence_id\":%ld,\"state\":\"accepted\"}}\n",
                        static_cast<long>(id), session_field,
                        static_cast<long>(id));
}

bool make_sequence_status(control_session::ResponseFrame *frame,
                          ResponseSession session, std::int32_t id,
                          const SequenceStatus &status) {
    char session_field[kSessionFieldBytes]{};
    if (!format_session_field(session_field, session)) return false;
    const char *state = status.state == SequenceState::kAccepted ? "accepted"
        : status.state == SequenceState::kRunning ? "running"
        : status.state == SequenceState::kCompleted ? "completed"
        : status.state == SequenceState::kAborted ? "aborted" : "failed";
    char failed_token[16]{};
    char code[48]{};
    if (status.failed_token_present) {
        std::snprintf(failed_token, sizeof(failed_token), "%u",
                      static_cast<unsigned>(status.failed_token));
    } else {
        std::snprintf(failed_token, sizeof(failed_token), "null");
    }
    if (status.code != nullptr) {
        std::snprintf(code, sizeof(code), "\"%s\"", status.code);
    } else {
        std::snprintf(code, sizeof(code), "null");
    }
    return format_frame(frame,
                        "@HIDBOT {\"type\":\"response\",\"v\":1,\"id\":%ld,"
                        "\"session\":%s,\"ok\":true,\"result\":{"
                        "\"sequence_id\":%ld,\"state\":\"%s\",\"started\":%s,"
                        "\"executed\":%u,\"failed_token\":%s,\"code\":%s}}\n",
                        static_cast<long>(id), session_field,
                        static_cast<long>(status.sequence_id), state,
                        status.started ? "true" : "false",
                        static_cast<unsigned>(status.executed),
                        failed_token, code);
}

bool make_keyboard_report(control_session::ResponseFrame *frame,
                          ResponseSession session,
                          std::int32_t id,
                          KeyboardReportResult result) {
    char session_field[kSessionFieldBytes]{};
    if (!format_session_field(session_field, session)) {
        frame->length = 0;
        return false;
    }
    const char *state = result.state == KeyboardReportState::kAlreadySet
                            ? "already_set"
                            : "submitted";
    return format_frame(frame,
                        "@HIDBOT {\"type\":\"response\",\"v\":1,\"id\":%ld,"
                        "\"session\":%s,\"ok\":true,\"result\":{\"state\":\"%s\"}}\n",
                        static_cast<long>(id), session_field, state);
}

bool make_keyboard_error(control_session::ResponseFrame *frame,
                         ResponseSession session,
                         std::int32_t id,
                         KeyboardReportFailure failure) {
    const char *code = "HID_NOT_READY";
    const char *message = "keyboard endpoint is not ready";
    switch (failure) {
        case KeyboardReportFailure::kBusy:
            code = "HID_BUSY";
            message = "keyboard report is busy";
            break;
        case KeyboardReportFailure::kSafetyPending:
            code = "HID_SAFETY_PENDING";
            message = "HID safety recovery is pending";
            break;
        case KeyboardReportFailure::kAuthorityLost:
            code = "SESSION_MISMATCH";
            message = "request session is not active";
            break;
        case KeyboardReportFailure::kUnsupportedOperation:
            code = "HID_UNSUPPORTED_OPERATION";
            message = "active HID route has no keyboard report role";
            break;
        case KeyboardReportFailure::kNotReady:
        case KeyboardReportFailure::kNone:
            break;
    }
    return make_error(frame, session, true, id, code, message);
}

bool make_mouse_report(control_session::ResponseFrame *frame,
                       ResponseSession session,
                       std::int32_t id,
                       MouseReportResult result) {
    char session_field[kSessionFieldBytes]{};
    if (!format_session_field(session_field, session)) {
        frame->length = 0;
        return false;
    }
    const char *state = result.state == MouseReportState::kAlreadySet
                            ? "already_set"
                            : "submitted";
    return format_frame(frame,
                        "@HIDBOT {\"type\":\"response\",\"v\":1,\"id\":%ld,"
                        "\"session\":%s,\"ok\":true,\"result\":{\"state\":\"%s\"}}\n",
                        static_cast<long>(id), session_field, state);
}

bool make_mouse_error(control_session::ResponseFrame *frame,
                      ResponseSession session,
                      std::int32_t id,
                      MouseReportFailure failure) {
    const char *code = "HID_NOT_READY";
    const char *message = "mouse endpoint is not ready";
    switch (failure) {
        case MouseReportFailure::kBusy:
            code = "HID_BUSY";
            message = "mouse report is busy";
            break;
        case MouseReportFailure::kSafetyPending:
            code = "HID_SAFETY_PENDING";
            message = "HID safety recovery is pending";
            break;
        case MouseReportFailure::kAuthorityLost:
            code = "SESSION_MISMATCH";
            message = "request session is not active";
            break;
        case MouseReportFailure::kUnsupportedOperation:
            code = "HID_UNSUPPORTED_OPERATION";
            message = "active HID route has no mouse report role";
            break;
        case MouseReportFailure::kNotReady:
        case MouseReportFailure::kNone:
            break;
    }
    return make_error(frame, session, true, id, code, message);
}

}  // namespace

bool Protocol::initialize(const Config &config,
                          control_session::RandomFill random_fill,
                          void *random_context,
                          sensitive_request::SecureRandomFill secure_random_fill,
                          void *secure_random_context,
                          sensitive_request::HmacSha256 hmac,
                          void *hmac_context) {
    if (config.output == nullptr || config.usb_status_provider == nullptr ||
        config.usb_exposure_status_provider == nullptr || config.usb_attach_provider == nullptr ||
        config.usb_detach_provider == nullptr || config.hid_route_status_provider == nullptr ||
        config.ble_led_status_provider == nullptr || config.ble_profile_status_provider == nullptr || config.ble_profile_select_provider == nullptr ||
        config.ble_exposure_status_provider == nullptr || config.ble_enable_provider == nullptr ||
        config.ble_disable_provider == nullptr ||
        config.ble_pairing_status_provider == nullptr ||
        config.ble_pairing_respond_provider == nullptr ||
        config.ble_bond_list_provider == nullptr ||
        config.ble_bond_remove_provider == nullptr ||
        config.hid_route_set_provider == nullptr || config.authority_epoch_provider == nullptr ||
        config.sequence_start_provider == nullptr ||
        config.sequence_status_provider == nullptr ||
        random_fill == nullptr ||
        !is_safe_metadata_string(config.metadata.project) ||
        !is_safe_metadata_string(config.metadata.target) ||
        !is_safe_metadata_string(config.metadata.idf_version) ||
        (config.metadata.firmware_identity != nullptr &&
         !is_valid_identity(config.metadata.firmware_identity))) {
        return false;
    }
    config_ = config;
    if (!sensitive_identity_.initialize(secure_random_fill,
                                        secure_random_context, hmac,
                                        hmac_context)) {
        return false;
    }
    session_.initialize(random_fill, random_context, config.now, config.now_context);
    session_.clear_transition_retry();
    initialized_ = true;
    lease_revoke_notified_ = false;
    return true;
}

bool Protocol::write_frame(const control_session::ResponseFrame &frame) const {
    return frame.length > 0 && frame.length <= control_session::kMaxResponseBytes &&
           config_.output(config_.output_context, frame.bytes, frame.length);
}

control_session::ResponseFrame &Protocol::prepare_response_scratch() {
    secure_memory::zero(scratch_storage_, sizeof(scratch_storage_));
    return *new (scratch_storage_) control_session::ResponseFrame{};
}

bool Protocol::replay_control_transition_retry(std::string_view session, std::int32_t id,
                                               std::string_view payload) {
    const control_session::ResponseFrame *cached =
        session_.inspect_transition_retry(session, id, payload);
    return cached != nullptr && write_frame(*cached);
}

void Protocol::cache_control_transition_retry(
    std::string_view session, std::int32_t id, std::string_view payload,
    const control_session::ResponseFrame &response) {
    session_.cache_transition_retry(session, id, payload, response);
}

void Protocol::retire_sequence_owner(
    control_session::LocalOwnerId local_owner_id) {
    if (local_owner_id != 0 && config_.sequence_owner_retired != nullptr) {
        config_.sequence_owner_retired(
            config_.sequence_owner_retired_context, local_owner_id);
    }
}

void Protocol::on_hid_lifecycle_invalidation() {
    if (initialized_) {
        // Lifecycle publication is the primary correctness barrier. This
        // serialized cleanup only retires cached/session state from an older
        // epoch; it cannot revoke a hello that already captured the current
        // epoch before this coalesced notification was consumed.
        const control_session::AuthorityEpoch current_epoch =
            config_.authority_epoch_provider(config_.authority_epoch_context);
        const control_session::AuthorityEpoch retired_epoch =
            session_.session_authority_epoch();
        const control_session::LocalOwnerId retired_owner =
            session_.local_owner_id();
        const bool authority_retired = session_.has_active_session() &&
                                       retired_epoch != current_epoch;
        session_.revoke_for_lifecycle_invalidation(current_epoch);
        if (authority_retired) retire_sequence_owner(retired_owner);
        lease_revoke_notified_ = false;
    }
}

void Protocol::on_hid_safety_failure(
    control_session::LocalOwnerId source_owner_id) {
    if (!initialized_) {
        return;
    }
    // HID report delivery is now uncertain. Retire only the owner captured by
    // the callback; a newer local owner remains active if the notification was
    // delayed. Runtime safety maintenance is requested only while that source
    // is still current, or while no replacement owner exists.
    const bool has_current_owner = session_.has_active_session();
    const bool source_is_current = has_current_owner &&
                                   session_.local_owner_id() == source_owner_id;
    if (source_is_current) {
        session_.revoke_for_takeover();
    }
    retire_sequence_owner(source_owner_id);
    lease_revoke_notified_ = false;
    if ((!has_current_owner || source_is_current) &&
        config_.hid_safety_failure != nullptr) {
        config_.hid_safety_failure(config_.hid_safety_failure_context);
    }
}

control_session::LocalOwnerId Protocol::local_owner_id() const {
    return session_.has_active_session() ? session_.local_owner_id() : 0;
}

void Protocol::service() {
    if (!initialized_) {
        return;
    }
    on_hid_lifecycle_invalidation();
    const control_session::LocalOwnerId retired_owner =
        session_.has_active_session() ? session_.local_owner_id() : 0;
    if (!session_.service_lease()) {
        return;
    }
    retire_sequence_owner(retired_owner);
    if (!lease_revoke_notified_) {
        lease_revoke_notified_ = true;
        if (config_.lease_expired != nullptr) {
            config_.lease_expired(config_.lease_expired_context);
        }
    }
}

void Protocol::handle_framing_event(const control_framing::Event &event) {
    if (!initialized_) {
        return;
    }
    if (event.kind == control_framing::EventKind::kOverlongProtocolFrame) {
        auto &response = prepare_response_scratch();
        if (make_error(&response, kUncorrelatableSession, false, 0, "LINE_TOO_LONG", "request line exceeds 512 bytes")) {
            write_frame(response);
        }
        return;
    }
    if (event.kind == control_framing::EventKind::kFrame) {
        handle_frame(event.payload);
    }
}

void Protocol::handle_frame(std::string_view payload) {
    if (payload.size() > control_session::kMaxRequestBytes) {
        auto &response = prepare_response_scratch();
        if (make_error(&response, kUncorrelatableSession, false, 0, "LINE_TOO_LONG", "request line exceeds 512 bytes")) {
            write_frame(response);
        }
        return;
    }

    if (std::memchr(payload.data(), '\0', payload.size()) != nullptr) {
        auto &response = prepare_response_scratch();
        if (make_error(&response, kUncorrelatableSession, false, 0,
                       "MALFORMED_JSON", "request contains an embedded NUL")) {
            write_frame(response);
        }
        return;
    }

    char *const request_json_scratch =
        reinterpret_cast<char *>(scratch_storage_);
    secure_memory::zero(scratch_storage_, sizeof(scratch_storage_));
    std::memcpy(request_json_scratch, payload.data(), payload.size());
    request_json_scratch[payload.size()] = '\0';
    const char *parse_end = nullptr;
    cJSON *root = cJSON_ParseWithLengthOpts(request_json_scratch,
                                           payload.size() + 1,
                                           &parse_end, true);
    if (root == nullptr) {
        auto &response = prepare_response_scratch();
        if (make_error(&response, kUncorrelatableSession, false, 0, "MALFORMED_JSON", "request is not valid JSON")) {
            write_frame(response);
        }
        secure_memory::zero(request_json_scratch,
                            control_session::kMaxRequestBytes + 1);
        return;
    }

    sensitive_request::Digest sensitive_digest{};
    auto finish = [&]() {
        wipe_json_strings(root);
        cJSON_Delete(root);
        root = nullptr;
        secure_memory::zero(request_json_scratch,
                            control_session::kMaxRequestBytes + 1);
        secure_memory::zero(sensitive_digest.data(), sensitive_digest.size());
    };
    const cJSON *long_sequence_code = nullptr;
    if (cJSON_IsObject(root)) {
        const cJSON *early_command =
            cJSON_GetObjectItemCaseSensitive(root, "cmd");
        const cJSON *early_params =
            cJSON_GetObjectItemCaseSensitive(root, "params");
        if (cJSON_IsString(early_command) && early_command->valuestring != nullptr &&
            std::strcmp(early_command->valuestring, "hid.sequence.start") == 0 &&
            cJSON_IsObject(early_params)) {
            long_sequence_code =
                cJSON_GetObjectItemCaseSensitive(early_params, "code");
        }
    }
    if (!cJSON_IsObject(root) ||
        !is_json_tree_bounded(root, 0, long_sequence_code) ||
        !object_has_no_duplicate_keys(root)) {
        auto &response = prepare_response_scratch();
        if (make_error(&response, kUncorrelatableSession, false, 0, "INVALID_REQUEST", "request must be a bounded object")) {
            write_frame(response);
        }
        finish();
        return;
    }

    std::int32_t id = 0;
    if (!parse_id(root, &id)) {
        auto &response = prepare_response_scratch();
        if (make_error(&response, kUncorrelatableSession, false, 0, "INVALID_REQUEST", "id must be a non-negative int32")) {
            write_frame(response);
        }
        finish();
        return;
    }

    const cJSON *version = cJSON_GetObjectItemCaseSensitive(root, "v");
    if (!is_integer_number(version)) {
        auto &response = prepare_response_scratch();
        if (make_error(&response, kUncorrelatableSession, true, id, "INVALID_REQUEST", "v must be an integer")) {
            write_frame(response);
        }
        finish();
        return;
    }
    if (version->valuedouble != kProtocolVersion) {
        auto &response = prepare_response_scratch();
        if (make_error(&response, kUncorrelatableSession, true, id, "UNSUPPORTED_PROTOCOL_VERSION", "only protocol version 1 is supported")) {
            write_frame(response);
        }
        finish();
        return;
    }

    std::string_view command;
    if (!get_bounded_nonempty_string(root, "cmd", kMaxCommandBytes, &command)) {
        auto &response = prepare_response_scratch();
        if (make_error(&response, kUncorrelatableSession, true, id, "INVALID_REQUEST", "cmd must be a bounded non-empty string")) {
            write_frame(response);
        }
        finish();
        return;
    }

    if (command == "protocol.hello") {
        static constexpr const char *kHelloFields[] = {"v", "id", "cmd", "params"};
        if (!object_has_only_fields(root, kHelloFields, 4)) {
            auto &response = prepare_response_scratch();
            if (make_error(&response, kUncorrelatableSession, true, id, "INVALID_REQUEST", "hello envelope has unknown fields")) {
                write_frame(response);
            }
            finish();
            return;
        }

        std::string_view client_nonce;
        const cJSON *params = cJSON_GetObjectItemCaseSensitive(root, "params");
        if (!validate_hello_params(params, &client_nonce)) {
            auto &response = prepare_response_scratch();
            if (make_error(&response, kUncorrelatableSession, true, id, "INVALID_PARAMS", "client_nonce must be 32 lowercase hex characters")) {
                write_frame(response);
            }
            finish();
            return;
        }

        const control_session::AuthorityEpoch authority_epoch =
            config_.authority_epoch_provider(config_.authority_epoch_context);
        const control_session::ResponseFrame *cached_response = nullptr;
        const control_session::HelloCacheResult cache_result =
            session_.inspect_hello(client_nonce, payload, authority_epoch, &cached_response);
        if (cache_result == control_session::HelloCacheResult::kExactRetry) {
            session_.refresh_lease();
            write_frame(*cached_response);
            finish();
            return;
        }
        if (cache_result == control_session::HelloCacheResult::kNonceConflict) {
            auto &response = prepare_response_scratch();
            if (make_error(&response, kUncorrelatableSession, true, id, "CLIENT_NONCE_CONFLICT", "client_nonce was previously used with different request bytes")) {
                write_frame(response);
            }
            finish();
            return;
        }

        if (session_.has_active_session()) {
            // Revoke first so the old authority cannot issue another command
            // while the safety release is being started by the runtime.
            const control_session::LocalOwnerId retired_owner =
                session_.local_owner_id();
            const bool current_epoch_session = session_.authority_epoch_matches(authority_epoch);
            session_.revoke_for_takeover();
            retire_sequence_owner(retired_owner);
            if (current_epoch_session && config_.session_takeover != nullptr) {
                config_.session_takeover(config_.session_takeover_context);
            }
        }
        char new_session[control_session::kTokenStorageBytes]{};
        session_.generate_token(new_session);
        auto &response = prepare_response_scratch();
        if (!make_hello(&response,
                        ResponseSession{true, std::string_view(new_session, control_session::kTokenHexLength)},
                        id,
                        config_.metadata,
                        client_nonce.data(),
                        session_.boot_id(),
                        new_session)) {
            make_error(&response, kUncorrelatableSession, true, id, "INTERNAL_ERROR", "response serialization failed");
            write_frame(response);
            finish();
            return;
        }
        if (!session_.activate_hello(client_nonce, payload, new_session,
                                     authority_epoch, response)) {
            make_error(&response, kUncorrelatableSession, true, id,
                       "INTERNAL_ERROR", "local owner identity exhausted");
            write_frame(response);
            finish();
            return;
        }
        // A successful fresh handshake supersedes any lifecycle retry proof
        // tied to the former session. Lifecycle invalidation itself must not
        // clear that proof: accepted attach/detach retries occur after it.
        session_.clear_transition_retry();
        lease_revoke_notified_ = false;
        write_frame(response);
        finish();
        return;
    }

    static constexpr const char *kNormalFields[] = {"v", "id", "session", "cmd", "params"};
    if (!object_has_only_fields(root, kNormalFields, 5)) {
        auto &response = prepare_response_scratch();
        if (make_error(&response, kUncorrelatableSession, true, id, "INVALID_REQUEST", "request envelope has unknown fields")) {
            write_frame(response);
        }
        finish();
        return;
    }

    std::string_view session;
    if (!get_bounded_nonempty_string(root, "session", control_session::kTokenHexLength, &session) ||
        !control_session::is_lower_hex_token(session)) {
        auto &response = prepare_response_scratch();
        if (make_error(&response, kUncorrelatableSession, true, id, "INVALID_REQUEST", "session must be 32 lowercase hex characters")) {
            write_frame(response);
        }
        finish();
        return;
    }
    const cJSON *params = cJSON_GetObjectItemCaseSensitive(root, "params");
    if (params != nullptr && !cJSON_IsObject(params)) {
        auto &response = prepare_response_scratch();
        if (make_error(&response, kUncorrelatableSession, true, id, "INVALID_PARAMS", "params must be an object")) {
            write_frame(response);
        }
        finish();
        return;
    }

    bool route_v2 = false;
    if (command == "hid.route.v2.status") {
        command = "hid.route.status";
        route_v2 = true;
    } else if (command == "hid.route.v2.set") {
        command = "hid.route.set";
        route_v2 = true;
    }

    // Attach/detach revoke the authority epoch as part of their accepted
    // transition. Their exact retry is therefore retained separately from the
    // normal epoch-scoped request cache and must replay identical bytes only.
    if ((command == "usb.attach" || command == "usb.detach" ||
         command == "hid.route.set") &&
        replay_control_transition_retry(session, id, payload)) {
        finish();
        return;
    }

    // This acquire-backed value is the request-side authority linearization
    // point. inspect_request checks it before any normal cache replay.
    const control_session::AuthorityEpoch authority_epoch =
        config_.authority_epoch_provider(config_.authority_epoch_context);
    const control_session::ResponseFrame *cached_response = nullptr;
    const bool sensitive_command = command == "ble.pairing.respond";
    if (sensitive_command && !sensitive_identity_.digest(payload, &sensitive_digest)) {
        auto &response = prepare_response_scratch();
        if (make_error(&response, kUncorrelatableSession, true, id,
                       "INTERNAL_ERROR", "pairing request is unavailable")) {
            write_frame(response);
        }
        finish();
        return;
    }
    const control_session::RequestCacheResult cache_result = sensitive_command
        ? session_.inspect_sensitive_request(session, id, payload.size(),
                                             sensitive_digest, authority_epoch,
                                             &cached_response)
        : session_.inspect_request(session, id, payload, authority_epoch,
                                   &cached_response);
    if (cache_result == control_session::RequestCacheResult::kSessionMismatch) {
        auto &response = prepare_response_scratch();
        if (make_error(&response, kUncorrelatableSession, true, id, "SESSION_MISMATCH", "request session is not active")) {
            write_frame(response);
        }
        finish();
        return;
    }
    const ResponseSession current_session{
        true,
        std::string_view(session_.current_session(), control_session::kTokenHexLength),
    };
    if (cache_result == control_session::RequestCacheResult::kExactRetry) {
        session_.refresh_lease();
        write_frame(*cached_response);
        finish();
        return;
    }
    if (cache_result == control_session::RequestCacheResult::kIdConflict ||
        cache_result == control_session::RequestCacheResult::kIdStale) {
        auto &response = prepare_response_scratch();
        const char *code = cache_result == control_session::RequestCacheResult::kIdConflict ?
            "REQUEST_ID_CONFLICT" : "REQUEST_ID_STALE";
        const char *message = cache_result == control_session::RequestCacheResult::kIdConflict ?
            "request id was previously used with different request bytes" :
            "request id is older than the last completed request";
        if (make_error(&response, current_session, true, id, code, message)) {
            write_frame(response);
        }
        finish();
        return;
    }

    auto &response = prepare_response_scratch();
    bool completed = false;
    bool semantically_valid = false;
    bool cache_response = true;
    if (command == "system.ping") {
        if (!validate_no_params(params)) {
            make_error(&response, current_session, true, id, "INVALID_PARAMS", "system.ping accepts no params");
        } else {
            semantically_valid = true;
            completed = make_ping(&response, current_session, id);
        }
    } else if (command == "system.info") {
        if (!validate_no_params(params)) {
            make_error(&response, current_session, true, id, "INVALID_PARAMS", "system.info accepts no params");
        } else {
            semantically_valid = true;
            completed = make_info(&response, current_session, id, config_.metadata);
        }
    } else if (command == "usb.status") {
        if (!validate_no_params(params)) {
            make_error(&response, current_session, true, id, "INVALID_PARAMS", "usb.status accepts no params");
        } else {
            semantically_valid = true;
            completed = make_usb_status(&response, current_session, id,
                                        config_.usb_status_provider(config_.usb_status_context));
        }
    } else if (command == "usb.exposure.status") {
        if (!validate_no_params(params)) {
            make_error(&response, current_session, true, id, "INVALID_PARAMS",
                       "usb.exposure.status accepts no params");
        } else {
            semantically_valid = true;
            completed = make_usb_exposure_status(
                &response, current_session, id,
                config_.usb_exposure_status_provider(config_.usb_exposure_status_context));
        }
    } else if (command == "usb.attach" || command == "usb.detach") {
        if (!validate_no_params(params)) {
            make_error(&response, current_session, true, id, "INVALID_PARAMS",
                       command == "usb.attach" ? "usb.attach accepts no params"
                                               : "usb.detach accepts no params");
        } else {
            semantically_valid = true;
            const UsbExposureActionOutcome action = command == "usb.attach"
                                                         ? config_.usb_attach_provider(
                                                               config_.usb_attach_context)
                                                         : config_.usb_detach_provider(
                                                               config_.usb_detach_context);
            if (action.action_result == UsbExposureActionResult::kBusy) {
                completed = make_error(&response, current_session, true, id, "HID_BUSY",
                                       "USB lifecycle transition is active");
            } else {
                if (!action.snapshot_valid) {
                    completed = make_error(&response, current_session, true, id, "INTERNAL_ERROR",
                                           "USB lifecycle transition snapshot is unavailable");
                } else {
                    completed = make_usb_exposure_status(&response, current_session, id,
                                                         action.snapshot);
                }
                if (completed && action.action_result == UsbExposureActionResult::kAccepted) {
                    // Lifecycle intent publishes a new authority epoch. Cache
                    // this exact response before revoking the session so the
                    // one permitted same-ID retry remains byte-identical even
                    // after asynchronous install/uninstall progress.
                    cache_control_transition_retry(session, id, payload, response);
                    const control_session::LocalOwnerId retired_owner =
                        session_.local_owner_id();
                    session_.revoke_for_lifecycle_invalidation(
                        config_.authority_epoch_provider(config_.authority_epoch_context));
                    retire_sequence_owner(retired_owner);
                    lease_revoke_notified_ = false;
                    write_frame(response);
                    finish();
                    return;
                }
            }
        }
    } else if (command == "ble.led.status") {
        if (!validate_no_params(params)) {
            make_error(&response, current_session, true, id, "INVALID_PARAMS", "BLE LED query accepts no params");
        } else {
            semantically_valid = true;
            completed = make_led_status(&response, current_session, id,
                config_.ble_led_status_provider(config_.ble_led_status_context));
        }
    } else if (command == "ble.profile.list" || command == "ble.profile.status") {
        if (!validate_no_params(params)) {
            make_error(&response, current_session, true, id, "INVALID_PARAMS",
                       "BLE profile query accepts no params");
        } else {
            semantically_valid = true;
            completed = command == "ble.profile.list"
                ? make_profile_list(&response, current_session, id)
                : make_profile_status(&response, current_session, id,
                    config_.ble_profile_status_provider(config_.ble_profile_status_context));
        }
    } else if (command == "ble.profile.select") {
        static constexpr const char *fields[] = {"profile"};
        std::string_view name;
        const ble_fixture_profile::ProfileDefinition *profile = nullptr;
        if (payload.find("\\u0000") != std::string_view::npos ||
            !cJSON_IsObject(params) || !object_has_only_fields(params, fields, 1) ||
            cJSON_GetArraySize(params) != 1 ||
            !get_bounded_nonempty_string(params, "profile", 48, &name) ||
            (profile = ble_fixture_profile::find_profile(name)) == nullptr) {
            make_error(&response, current_session, true, id, "INVALID_PARAMS",
                       "finite BLE profile ID is required");
        } else {
            semantically_valid = true;
            const auto outcome = config_.ble_profile_select_provider(
                config_.ble_profile_select_context, profile->id);
            if (outcome.result == ble_fixture_profile::SelectionResult::kBusy) {
                completed = make_error(&response, current_session, true, id,
                                       "HID_BUSY", "BLE profile selection requires quiescence");
            } else {
                completed = make_profile_status(&response, current_session, id, outcome.snapshot);
            }
        }
    } else if (command == "ble.exposure.status") {
        if (!validate_no_params(params)) {
            make_error(&response, current_session, true, id, "INVALID_PARAMS",
                       "ble.exposure.status accepts no params");
        } else {
            semantically_valid = true;
            completed = make_ble_exposure_status(
                &response, current_session, id,
                config_.ble_exposure_status_provider(
                    config_.ble_exposure_status_context));
        }
    } else if (command == "ble.enable" || command == "ble.disable") {
        if (!validate_no_params(params)) {
            make_error(&response, current_session, true, id, "INVALID_PARAMS",
                       command == "ble.enable" ? "ble.enable accepts no params"
                                               : "ble.disable accepts no params");
        } else {
            semantically_valid = true;
            const BleExposureActionOutcome action =
                command == "ble.enable"
                    ? config_.ble_enable_provider(config_.ble_enable_context)
                    : config_.ble_disable_provider(config_.ble_disable_context);
            if (action.action_result == BleExposureActionResult::kBusy) {
                completed = make_error(&response, current_session, true, id,
                                       "HID_BUSY", "control transition is active");
            } else if (!action.snapshot_valid) {
                completed = make_error(&response, current_session, true, id,
                                       "INTERNAL_ERROR",
                                       "BLE lifecycle snapshot is unavailable");
            } else {
                // BLE exposure is not HID output authority. The normal
                // epoch-scoped retry cache preserves this frozen Stage-A
                // response without revoking the active USB HID session.
                completed = make_ble_exposure_status(&response, current_session,
                                                     id, action.snapshot);
            }
        }
    } else if (command == "ble.pairing.status") {
        if (!validate_no_params(params)) {
            make_error(&response, current_session, true, id, "INVALID_PARAMS",
                       "ble.pairing.status accepts no params");
        } else {
            semantically_valid = true;
            completed = make_pairing_status(
                &response, current_session, id,
                config_.ble_pairing_status_provider(
                    config_.ble_pairing_status_context));
        }
    } else if (command == "ble.pairing.respond") {
        BlePairingRespondRequest pairing_request{};
        if (!validate_pairing_respond_params(params, &pairing_request)) {
            make_error(&response, current_session, true, id, "INVALID_PARAMS",
                       "pairing response params are invalid");
        } else {
            semantically_valid = true;
            const std::uint32_t pairing_id = pairing_request.pairing_id;
            const BlePairingRespondResult result =
                config_.ble_pairing_respond_provider(
                    config_.ble_pairing_respond_context, pairing_request);
            secure_memory::zero(&pairing_request, sizeof(pairing_request));
            switch (result) {
                case BlePairingRespondResult::kAccepted:
                    completed = make_pairing_respond(&response, current_session,
                                                     id, pairing_id);
                    break;
                case BlePairingRespondResult::kInjectionFailed:
                    completed = make_error(&response, current_session, true, id,
                                           "BLE_PAIRING_FAILED",
                                           "pairing response was not accepted");
                    break;
                case BlePairingRespondResult::kNotPending:
                default:
                    completed = make_error(&response, current_session, true, id,
                                           "BLE_PAIRING_NOT_PENDING",
                                           "pairing input is not pending");
                    break;
            }
        }
    } else if (command == "ble.bond.list") {
        if (!validate_no_params(params)) {
            make_error(&response, current_session, true, id, "INVALID_PARAMS",
                       "ble.bond.list accepts no params");
        } else {
            semantically_valid = true;
            const BleBondListResult result = config_.ble_bond_list_provider(
                config_.ble_bond_list_context);
            switch (result.kind) {
                case BleBondListResultKind::kSuccess:
                    completed = make_bond_list(&response, current_session, id,
                                               result);
                    break;
                case BleBondListResultKind::kStorageFailure:
                    completed = make_error(&response, current_session, true, id,
                                           "BLE_BOND_STORAGE",
                                           "bond store is not trustworthy");
                    break;
                case BleBondListResultKind::kNotReady:
                default:
                    completed = make_error(&response, current_session, true, id,
                                           "BLE_NOT_READY",
                                           "BLE bond store is not initialized");
                    break;
            }
        }
    } else if (command == "ble.bond.remove") {
        BondId bond_id{};
        if (!validate_bond_remove_params(params, &bond_id)) {
            make_error(&response, current_session, true, id, "INVALID_PARAMS",
                       "ble.bond.remove requires one exact bond_id");
        } else {
            semantically_valid = true;
            const BleBondRemoveResult result =
                config_.ble_bond_remove_provider(
                    config_.ble_bond_remove_context, bond_id);
            switch (result.kind) {
                case BleBondRemoveResultKind::kSuccess:
                    completed = make_bond_remove(&response, current_session,
                                                 id, result);
                    break;
                case BleBondRemoveResultKind::kNotFound:
                    completed = make_error(&response, current_session, true, id,
                                           "BLE_BOND_NOT_FOUND",
                                           "exact bond was not found");
                    break;
                case BleBondRemoveResultKind::kAmbiguous:
                    completed = make_error(&response, current_session, true, id,
                                           "BLE_BOND_AMBIGUOUS",
                                           "bond identifier is not unique");
                    break;
                case BleBondRemoveResultKind::kBusy:
                    completed = make_error(&response, current_session, true, id,
                                           "BLE_BOND_BUSY",
                                           "bond removal is not currently safe");
                    break;
                case BleBondRemoveResultKind::kStorageFailure:
                    completed = make_error(&response, current_session, true, id,
                                           "BLE_BOND_STORAGE",
                                           "bond removal postcondition failed");
                    break;
                case BleBondRemoveResultKind::kNotReady:
                default:
                    completed = make_error(&response, current_session, true, id,
                                           "BLE_NOT_READY",
                                           "BLE bond store is not initialized");
                    break;
            }
        }
    } else if (command == "hid.route.status") {
        if (!validate_no_params(params)) {
            make_error(&response, current_session, true, id, "INVALID_PARAMS",
                       route_v2 ? "invalid route"
                                : "hid.route.status accepts no params");
        } else {
            semantically_valid = true;
            const HidRouteStatus status = config_.hid_route_status_provider(
                config_.hid_route_status_context);
            completed = make_versioned_hid_route_status(
                &response, current_session, id, status, route_v2);
        }
    } else if (command == "hid.route.set") {
        OutputRoute desired{};
        if (!validate_route_set_params(params, route_v2, &desired)) {
            make_error(&response, current_session, true, id, "INVALID_PARAMS",
                       route_v2
                           ? "invalid route"
                           : "hid.route.set route must be none or usb");
        } else {
            semantically_valid = true;
            const HidRouteActionOutcome action =
                config_.hid_route_set_provider(config_.hid_route_set_context, desired);
            switch (action.action_result) {
                case HidRouteActionResult::kBusy:
                    completed = make_error(&response, current_session, true, id,
                                           "HID_BUSY", "stable none needed");
                    break;
                case HidRouteActionResult::kNotReady:
                    completed = make_error(&response, current_session, true, id,
                                           "HID_NOT_READY", "requested HID route is not ready");
                    break;
                case HidRouteActionResult::kSafetyPending:
                    completed = make_error(&response, current_session, true, id,
                                           "HID_SAFETY_PENDING",
                                           "HID safety recovery is pending");
                    break;
                case HidRouteActionResult::kAccepted:
                case HidRouteActionResult::kNoOp:
                    if (!action.snapshot_valid) {
                        completed = make_error(&response, current_session, true, id,
                                               "INTERNAL_ERROR",
                                               "HID route snapshot is unavailable");
                        break;
                    }
                    completed = make_versioned_hid_route_status(
                        &response, current_session, id, action.snapshot,
                        route_v2);
                    if (completed &&
                        action.action_result == HidRouteActionResult::kAccepted) {
                        cache_control_transition_retry(session, id, payload, response);
                        const control_session::LocalOwnerId retired_owner =
                            session_.local_owner_id();
                        session_.revoke_for_lifecycle_invalidation(
                            config_.authority_epoch_provider(config_.authority_epoch_context));
                        retire_sequence_owner(retired_owner);
                        lease_revoke_notified_ = false;
                        write_frame(response);
                        finish();
                        return;
                    }
                    break;
            }
        }
    } else if (command == "hid.release_all") {
        if (!validate_no_params(params)) {
            make_error(&response, current_session, true, id, "INVALID_PARAMS", "hid.release_all accepts no params");
        } else {
            semantically_valid = true;
            const ReleaseAllResult release_result = config_.release_all_provider != nullptr
                                                         ? config_.release_all_provider(config_.release_all_context)
                                                         : ReleaseAllResult{};
            if (release_result.authority_lost ||
                config_.authority_epoch_provider(config_.authority_epoch_context) != authority_epoch) {
                semantically_valid = false;
                cache_response = false;
                completed = make_error(&response, kUncorrelatableSession, true, id,
                                       "SESSION_MISMATCH", "request session is not active");
            } else if (!release_result.success) {
                completed = make_error(&response, current_session, true, id,
                                       "HID_SAFETY_PENDING", "all-up safety release is pending");
            } else {
                completed = make_release_all(&response, current_session, id, release_result);
            }
        }
    } else if (command == "hid.sequence.start") {
        std::string_view code;
        if (!validate_sequence_start_params(params, payload, &code)) {
            make_error(&response, current_session, true, id, "INVALID_PARAMS",
                       "sequence code is invalid");
        } else {
            const SequenceStartResult result = config_.sequence_start_provider(
                config_.sequence_start_context, session_.local_owner_id(), id, code);
            if (result == SequenceStartResult::kInvalid) {
                make_error(&response, current_session, true, id, "INVALID_PARAMS",
                           "sequence code is invalid");
            } else {
                semantically_valid = true;
                if (result == SequenceStartResult::kBusy) {
                    completed = make_error(&response, current_session, true, id,
                                           "HID_BUSY", "HID sequence executor is busy");
                } else if (result == SequenceStartResult::kSafetyPending) {
                    completed = make_error(&response, current_session, true, id,
                                           "HID_SAFETY_PENDING",
                                           "HID safety recovery is pending");
                } else if (result == SequenceStartResult::kNotReady) {
                    completed = make_error(&response, current_session, true, id,
                                           "HID_NOT_READY", "HID route is not ready");
                } else if (result ==
                           SequenceStartResult::kUnsupportedOperation) {
                    completed = make_error(
                        &response, current_session, true, id,
                        "HID_UNSUPPORTED_OPERATION",
                        "active HID route lacks a sequence report role");
                } else {
                    completed = make_sequence_start(&response, current_session, id);
                }
            }
        }
    } else if (command == "hid.sequence.status") {
        std::int32_t sequence_id = 0;
        if (!validate_sequence_status_params(params, &sequence_id)) {
            make_error(&response, current_session, true, id, "INVALID_PARAMS",
                       "sequence status params are invalid");
        } else {
            semantically_valid = true;
            SequenceStatus status{};
            if (!config_.sequence_status_provider(config_.sequence_status_context,
                                                  session_.local_owner_id(),
                                                  sequence_id, &status)) {
                completed = make_error(&response, current_session, true, id,
                                       "SEQUENCE_NOT_FOUND",
                                       "sequence status is not retained");
            } else {
                completed = make_sequence_status(&response, current_session, id,
                                                 status);
            }
        }
    } else if (command == "hid.keyboard.report") {
        KeyboardReportRequest keyboard_request{};
        if (!validate_keyboard_report_params(params, &keyboard_request)) {
            make_error(&response, current_session, true, id, "INVALID_PARAMS",
                       "keyboard report params are invalid");
        } else {
            semantically_valid = true;
            const KeyboardReportResult keyboard_result =
                config_.keyboard_report_provider != nullptr
                    ? config_.keyboard_report_provider(config_.keyboard_report_context,
                                                       session_.local_owner_id(),
                                                       keyboard_request)
                    : KeyboardReportResult{};
            if (keyboard_result.authority_lost ||
                keyboard_result.failure == KeyboardReportFailure::kAuthorityLost ||
                config_.authority_epoch_provider(config_.authority_epoch_context) != authority_epoch) {
                semantically_valid = false;
                cache_response = false;
                completed = make_error(&response, kUncorrelatableSession, true, id,
                                       "SESSION_MISMATCH", "request session is not active");
            } else if (!keyboard_result.success) {
                completed = make_keyboard_error(&response, current_session, id,
                                                keyboard_result.failure);
            } else {
                completed = make_keyboard_report(&response, current_session, id,
                                                 keyboard_result);
            }
        }
    } else if (command == "hid.mouse.report") {
        MouseReportRequest mouse_request{};
        if (!validate_mouse_report_params(params, &mouse_request)) {
            make_error(&response, current_session, true, id, "INVALID_PARAMS",
                       "mouse report params are invalid");
        } else {
            semantically_valid = true;
            const MouseReportResult mouse_result =
                config_.mouse_report_provider != nullptr
                    ? config_.mouse_report_provider(config_.mouse_report_context,
                                                    session_.local_owner_id(),
                                                    mouse_request)
                    : MouseReportResult{};
            if (mouse_result.authority_lost ||
                mouse_result.failure == MouseReportFailure::kAuthorityLost ||
                config_.authority_epoch_provider(config_.authority_epoch_context) != authority_epoch) {
                semantically_valid = false;
                cache_response = false;
                completed = make_error(&response, kUncorrelatableSession, true, id,
                                       "SESSION_MISMATCH", "request session is not active");
            } else if (!mouse_result.success) {
                completed = make_mouse_error(&response, current_session, id,
                                             mouse_result.failure);
            } else {
                completed = make_mouse_report(&response, current_session, id,
                                              mouse_result);
            }
        }
    } else {
        if (!validate_no_params(params)) {
            make_error(&response, current_session, true, id, "INVALID_PARAMS", "unknown command accepts no params");
        } else {
            completed = make_error(&response, current_session, true, id, "UNKNOWN_COMMAND", "command is not registered");
        }
    }

    if (semantically_valid) {
        // A valid current-session request, including an operational error,
        // proves the authority is alive. Invalid envelopes/params never
        // refresh this deadline.
        session_.refresh_lease();
    }

    if (!completed) {
        if (response.length == 0) {
            make_error(&response, current_session, true, id, "INTERNAL_ERROR", "response serialization failed");
        }
        write_frame(response);
        finish();
        return;
    }

    if (cache_response) {
        if (sensitive_command) {
            session_.cache_completed_sensitive_request(
                id, payload.size(), sensitive_digest, authority_epoch, response);
        } else {
            session_.cache_completed_request(id, payload, authority_epoch, response);
        }
    }
    write_frame(response);
    finish();
}

#ifdef CONTROL_PROTOCOL_NATIVE_TEST
bool Protocol::request_scratch_zero_for_test() const {
    for (std::size_t index = 0;
         index < control_session::kMaxRequestBytes + 1; ++index) {
        if (scratch_storage_[index] != 0) {
            return false;
        }
    }
    return true;
}

control_session::State::RequestCacheSnapshot
Protocol::request_cache_snapshot_for_test() const {
    return session_.request_cache_snapshot_for_test();
}
#endif

}  // namespace control_protocol
