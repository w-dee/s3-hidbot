#include "control_protocol/control_protocol.hpp"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string_view>

#include "cJSON.h"

namespace control_protocol {
namespace {

constexpr std::size_t kPrefixLength = sizeof(control_framing::kFramePrefix) - 1;
constexpr std::size_t kMaxCommandBytes = 48;
constexpr std::size_t kMaxJsonStringBytes = 64;
constexpr std::size_t kMaxJsonObjectMembers = 8;
constexpr std::size_t kMaxJsonArrayMembers = 8;
constexpr std::size_t kMaxJsonDepth = 4;
constexpr std::size_t kMaxMetadataBytes = 32;

constexpr char kCapabilityJson[] =
    "[\"protocol.hello-v1\",\"system.ping-v1\",\"system.info-v1\",\"usb.status-v1\",\"hid.lease-v1\"]";
struct ResponseSession {
    bool present;
    std::string_view token;
};

constexpr ResponseSession kUncorrelatableSession{false, {}};
constexpr std::size_t kSessionFieldBytes = control_session::kTokenHexLength + 3;

// Every formatter below writes to ResponseFrame::bytes. This conservative
// bound covers the largest U2 response: protocol.hello with bounded project
// metadata, four 32-hex values (top-level session, result session, boot ID,
// and client nonce), capability list, maximum int32 id, framing, and LF.
// vsnprintf still fail-closes if a future format exceeds the buffer.
constexpr std::size_t kMaximumHelloResponseBytes =
    kPrefixLength + 180 + kMaxMetadataBytes +
    control_session::kTokenHexLength * 4 + (sizeof(kCapabilityJson) - 1) + 32 + 10 + 1;
static_assert(kMaximumHelloResponseBytes <= control_session::kMaxResponseBytes);

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

bool is_json_tree_bounded(const cJSON *item, std::size_t depth) {
    if (item == nullptr || depth > kMaxJsonDepth) {
        return false;
    }
    if (cJSON_IsString(item) &&
        (item->valuestring == nullptr || std::strlen(item->valuestring) > kMaxJsonStringBytes)) {
        return false;
    }

    if (cJSON_IsObject(item) || cJSON_IsArray(item)) {
        std::size_t count = 0;
        const std::size_t maximum = cJSON_IsObject(item) ? kMaxJsonObjectMembers : kMaxJsonArrayMembers;
        for (const cJSON *child = item->child; child != nullptr; child = child->next) {
            if (++count > maximum ||
                (cJSON_IsObject(item) &&
                 (child->string == nullptr || std::strlen(child->string) > kMaxJsonStringBytes)) ||
                !is_json_tree_bounded(child, depth + 1)) {
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
        !is_lower_hex_token_cstr(result_session)) {
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
                        kCapabilityJson);
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

}  // namespace

bool Protocol::initialize(const Config &config,
                          control_session::RandomFill random_fill,
                          void *random_context) {
    if (config.output == nullptr || config.usb_status_provider == nullptr ||
        config.authority_epoch_provider == nullptr || random_fill == nullptr ||
        !is_safe_metadata_string(config.metadata.project) ||
        !is_safe_metadata_string(config.metadata.target) ||
        !is_safe_metadata_string(config.metadata.idf_version)) {
        return false;
    }
    config_ = config;
    session_.initialize(random_fill, random_context, config.now, config.now_context);
    initialized_ = true;
    lease_revoke_notified_ = false;
    return true;
}

bool Protocol::write_frame(const control_session::ResponseFrame &frame) const {
    return frame.length > 0 && frame.length <= control_session::kMaxResponseBytes &&
           config_.output(config_.output_context, frame.bytes, frame.length);
}

control_session::ResponseFrame &Protocol::prepare_response_scratch() {
    response_scratch_ = {};
    return response_scratch_;
}

void Protocol::on_hid_lifecycle_invalidation() {
    if (initialized_) {
        // Lifecycle publication is the primary correctness barrier. This
        // serialized cleanup only retires cached/session state from an older
        // epoch; it cannot revoke a hello that already captured the current
        // epoch before this coalesced notification was consumed.
        session_.revoke_for_lifecycle_invalidation(
            config_.authority_epoch_provider(config_.authority_epoch_context));
        lease_revoke_notified_ = false;
    }
}

void Protocol::on_hid_safety_failure() {
    if (!initialized_) {
        return;
    }
    // HID report delivery is now uncertain. Revoke authority in the protocol
    // task before asking the runtime to maintain its interface safety state.
    session_.revoke_for_takeover();
    lease_revoke_notified_ = false;
    if (config_.hid_safety_failure != nullptr) {
        config_.hid_safety_failure(config_.hid_safety_failure_context);
    }
}

void Protocol::service() {
    if (!initialized_) {
        return;
    }
    on_hid_lifecycle_invalidation();
    if (!session_.service_lease()) {
        return;
    }
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

    std::memcpy(request_json_scratch_, payload.data(), payload.size());
    request_json_scratch_[payload.size()] = '\0';
    const char *parse_end = nullptr;
    cJSON *root = cJSON_ParseWithLengthOpts(request_json_scratch_, payload.size() + 1, &parse_end, true);
    if (root == nullptr) {
        auto &response = prepare_response_scratch();
        if (make_error(&response, kUncorrelatableSession, false, 0, "MALFORMED_JSON", "request is not valid JSON")) {
            write_frame(response);
        }
        return;
    }

    auto finish = [&root]() { cJSON_Delete(root); };
    if (!cJSON_IsObject(root) || !is_json_tree_bounded(root, 0) ||
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
            const bool current_epoch_session = session_.authority_epoch_matches(authority_epoch);
            session_.revoke_for_takeover();
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
        session_.activate_hello(client_nonce, payload, new_session, authority_epoch, response);
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

    // This acquire-backed value is the request-side authority linearization
    // point. inspect_request checks it before any normal cache replay.
    const control_session::AuthorityEpoch authority_epoch =
        config_.authority_epoch_provider(config_.authority_epoch_context);
    const control_session::ResponseFrame *cached_response = nullptr;
    const control_session::RequestCacheResult cache_result =
        session_.inspect_request(session, id, payload, authority_epoch, &cached_response);
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

    session_.cache_completed_request(id, payload, authority_epoch, response);
    write_frame(response);
    finish();
}

}  // namespace control_protocol
