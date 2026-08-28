#include <cassert>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "control_framing/control_framing.hpp"
#include "control_protocol/control_protocol.hpp"

namespace {

constexpr char kNonceA[] = "0123456789abcdef0123456789abcdef";
constexpr char kNonceB[] = "fedcba9876543210fedcba9876543210";

struct Sink {
    std::vector<std::string> frames;

    static bool write(void *context, const std::uint8_t *data, std::size_t length) {
        auto *sink = static_cast<Sink *>(context);
        assert(length > 0 && length <= 1024);
        const std::string frame(reinterpret_cast<const char *>(data), length);
        assert(frame.starts_with("@HIDBOT "));
        assert(frame.back() == '\n');
        assert(frame.find("\"type\":\"response\"") != std::string::npos);
        assert(frame.find("\"v\":1") != std::string::npos);
        assert(frame.find("\"id\":") != std::string::npos);
        const std::size_t session_position = frame.find("\"session\":");
        const std::size_t ok_position = frame.find("\"ok\":");
        assert(session_position != std::string::npos && session_position < ok_position);
        assert(frame.find("\"ok\":true") != std::string::npos ||
               frame.find("\"ok\":false") != std::string::npos);
        const bool has_result = frame.find("\"result\":") != std::string::npos;
        const bool has_error = frame.find("\"error\":") != std::string::npos;
        assert(has_result != has_error);
        sink->frames.push_back(frame);
        return true;
    }

    const std::string &last() const {
        assert(!frames.empty());
        return frames.back();
    }
};

struct RandomSource {
    std::uint8_t next = 0;

    static void fill(void *context, std::uint8_t *output, std::size_t length) {
        auto *source = static_cast<RandomSource *>(context);
        for (std::size_t index = 0; index < length; ++index) {
            output[index] = static_cast<std::uint8_t>(source->next + index);
        }
        source->next = static_cast<std::uint8_t>(source->next + length);
    }
};

struct StatusSource {
    control_protocol::UsbStatus status{false, false, false, false};

    static control_protocol::UsbStatus get(void *context) {
        return static_cast<StatusSource *>(context)->status;
    }
};

struct Fixture {
    Sink sink;
    RandomSource random;
    StatusSource status;
    control_protocol::Protocol protocol;

    explicit Fixture(std::uint8_t random_seed = 0) {
        random.next = random_seed;
        const control_protocol::Config config{
            .metadata = {"s3-hidbot", "esp32s3", "v5.5.4"},
            .usb_status_provider = StatusSource::get,
            .usb_status_context = &status,
            .output = Sink::write,
            .output_context = &sink,
        };
        assert(protocol.initialize(config, RandomSource::fill, &random));
    }

    void payload(std::string_view json) {
        protocol.handle_framing_event(
            control_framing::Event{control_framing::EventKind::kFrame, json});
    }
};

void require_contains(const std::string &value, std::string_view expected) {
    assert(value.find(expected) != std::string::npos);
}

void require_top_level_session(const std::string &frame, std::string_view expected) {
    const std::size_t session_position = frame.find("\"session\":");
    const std::size_t ok_position = frame.find("\"ok\":");
    assert(session_position != std::string::npos && session_position < ok_position);
    if (expected.empty()) {
        assert(frame.compare(session_position, sizeof("\"session\":null") - 1,
                             "\"session\":null") == 0);
    } else {
        const std::string marker = "\"session\":\"" + std::string(expected) + "\"";
        assert(frame.compare(session_position, marker.size(), marker) == 0);
    }
}

std::string extract_string(const std::string &json, std::string_view name) {
    const std::string marker = "\"" + std::string(name) + "\":\"";
    const std::size_t start = json.find(marker);
    assert(start != std::string::npos);
    const std::size_t value_start = start + marker.size();
    const std::size_t value_end = json.find('"', value_start);
    assert(value_end != std::string::npos);
    return json.substr(value_start, value_end - value_start);
}

std::string hello_request(int id, std::string_view nonce) {
    return "{\"v\":1,\"id\":" + std::to_string(id) +
        ",\"cmd\":\"protocol.hello\",\"params\":{\"client_nonce\":\"" +
        std::string(nonce) + "\"}}";
}

std::string request(int id, std::string_view session, std::string_view command,
                    std::string_view params = "{}") {
    return "{\"v\":1,\"id\":" + std::to_string(id) + ",\"session\":\"" +
        std::string(session) + "\",\"cmd\":\"" + std::string(command) +
        "\",\"params\":" + std::string(params) + "}";
}

void route_to_protocol(void *context, const control_framing::Event &event) {
    static_cast<control_protocol::Protocol *>(context)->handle_framing_event(event);
}

void feed_wire(control_framing::Transport *transport,
               control_protocol::Protocol *protocol,
               std::string_view bytes) {
    transport->consume(reinterpret_cast<const std::uint8_t *>(bytes.data()),
                       bytes.size(),
                       route_to_protocol,
                       protocol);
}

void test_strict_envelope_and_framing() {
    Fixture fixture;
    fixture.payload("{bad");
    require_contains(fixture.sink.last(), "\"code\":\"MALFORMED_JSON\"");
    require_top_level_session(fixture.sink.last(), {});
    fixture.payload("{\"v\":1,\"cmd\":\"protocol.hello\",\"params\":{}}");
    require_contains(fixture.sink.last(), "\"id\":null");
    require_top_level_session(fixture.sink.last(), {});
    fixture.payload("{\"v\":1,\"id\":0,\"params\":{}}");
    require_contains(fixture.sink.last(), "\"code\":\"INVALID_REQUEST\"");
    require_top_level_session(fixture.sink.last(), {});
    fixture.payload("{\"v\":1,\"id\":1.5,\"cmd\":\"protocol.hello\",\"params\":{}}");
    require_contains(fixture.sink.last(), "\"code\":\"INVALID_REQUEST\"");
    require_top_level_session(fixture.sink.last(), {});
    fixture.payload("{\"v\":1,\"id\":-1,\"cmd\":\"protocol.hello\",\"params\":{}}");
    require_contains(fixture.sink.last(), "\"id\":null");
    require_top_level_session(fixture.sink.last(), {});
    fixture.payload("{\"v\":1,\"id\":2147483648,\"cmd\":\"protocol.hello\",\"params\":{}}");
    require_contains(fixture.sink.last(), "\"id\":null");
    require_top_level_session(fixture.sink.last(), {});
    fixture.payload("{\"v\":2,\"id\":1,\"cmd\":\"protocol.hello\",\"params\":{}}");
    require_contains(fixture.sink.last(), "\"code\":\"UNSUPPORTED_PROTOCOL_VERSION\"");
    require_top_level_session(fixture.sink.last(), {});
    fixture.payload("{\"v\":1,\"id\":1,\"cmd\":\"\",\"params\":{}}");
    require_contains(fixture.sink.last(), "\"code\":\"INVALID_REQUEST\"");
    require_top_level_session(fixture.sink.last(), {});
    fixture.payload("{\"v\":1,\"id\":1,\"cmd\":\"" + std::string(49, 'x') + "\",\"params\":{}}");
    require_contains(fixture.sink.last(), "\"code\":\"INVALID_REQUEST\"");
    require_top_level_session(fixture.sink.last(), {});
    fixture.payload("{\"v\":1,\"v\":1,\"id\":1,\"cmd\":\"protocol.hello\",\"params\":{}}");
    require_contains(fixture.sink.last(), "\"code\":\"INVALID_REQUEST\"");
    require_top_level_session(fixture.sink.last(), {});

    const std::string hello = hello_request(1, kNonceA);
    control_framing::Transport transport;
    feed_wire(&transport, &fixture.protocol, "unrelated log\n@HIDBOT " + hello + "\r\n");
    require_contains(fixture.sink.last(), "\"ok\":true");
    const std::string session = extract_string(fixture.sink.last(), "session");
    require_top_level_session(fixture.sink.last(), session);
    assert(extract_string(fixture.sink.last(), "client_nonce") == kNonceA);
    assert(fixture.sink.last().find("\"session\":\"" + session + "\"") !=
           fixture.sink.last().rfind("\"session\":\"" + session + "\""));

    fixture.payload("{\"v\":1,\"id\":6,\"cmd\":\"system.ping\",\"params\":{}}");
    require_contains(fixture.sink.last(), "\"code\":\"INVALID_REQUEST\"");
    require_top_level_session(fixture.sink.last(), {});
    fixture.payload(request(7, "bad", "system.ping"));
    require_contains(fixture.sink.last(), "\"code\":\"INVALID_REQUEST\"");
    require_top_level_session(fixture.sink.last(), {});
    fixture.payload(request(6, "0123456789abcdef0123456789abcde0", "system.ping"));
    require_contains(fixture.sink.last(), "\"code\":\"SESSION_MISMATCH\"");
    require_top_level_session(fixture.sink.last(), {});

    fixture.payload(request(2, session, "system.ping", "null"));
    require_contains(fixture.sink.last(), "\"code\":\"INVALID_PARAMS\"");
    require_top_level_session(fixture.sink.last(), {});
    fixture.payload(request(2, session, "system.ping", "[]"));
    require_contains(fixture.sink.last(), "\"code\":\"INVALID_PARAMS\"");
    require_top_level_session(fixture.sink.last(), {});
    fixture.payload(request(2, session, "system.ping", "true"));
    require_contains(fixture.sink.last(), "\"code\":\"INVALID_PARAMS\"");
    require_top_level_session(fixture.sink.last(), {});
    fixture.payload("{\"v\":1,\"id\":2,\"session\":\"" + session +
                    "\",\"cmd\":\"system.ping\"}");
    require_contains(fixture.sink.last(), "\"pong\":true");
    require_top_level_session(fixture.sink.last(), session);
    fixture.payload("{\"v\":1,\"id\":3,\"session\":\"" + session +
                    "\",\"cmd\":\"system.ping\",\"params\":{},\"extra\":true}");
    require_contains(fixture.sink.last(), "\"code\":\"INVALID_REQUEST\"");
    require_top_level_session(fixture.sink.last(), {});
    fixture.payload("{\"v\":1,\"id\":3,\"session\":\"" + session +
                    "\",\"cmd\":\"system.ping\",\"params\":{\"x\":1}}");
    require_contains(fixture.sink.last(), "\"code\":\"INVALID_PARAMS\"");
    require_top_level_session(fixture.sink.last(), session);
    fixture.payload("{\"v\":1,\"id\":3,\"cmd\":\"protocol.hello\",\"params\":{\"client_nonce\":\"" +
                    std::string(kNonceB) + "\",\"client_nonce\":\"" + std::string(kNonceB) + "\"}}");
    require_contains(fixture.sink.last(), "\"code\":\"INVALID_REQUEST\"");
    require_top_level_session(fixture.sink.last(), {});

    std::string overlong = std::string(control_framing::kFramePrefix) +
        std::string(control_framing::kMaxRequestLineBytes -
                        (sizeof(control_framing::kFramePrefix) - 1) + 1,
                    'x') + "\n";
    feed_wire(&transport, &fixture.protocol, overlong);
    require_contains(fixture.sink.last(), "\"code\":\"LINE_TOO_LONG\"");
    require_top_level_session(fixture.sink.last(), {});
    feed_wire(&transport, &fixture.protocol,
              std::string(control_framing::kFramePrefix) + request(4, session, "system.ping") + "\n");
    require_contains(fixture.sink.last(), "\"pong\":true");
    require_top_level_session(fixture.sink.last(), session);

    feed_wire(&transport, &fixture.protocol, "@HIDBOT partial");
    const std::vector<std::uint8_t> sync = {0, 0, 0, 0};
    transport.consume(sync.data(), sync.size(), route_to_protocol, &fixture.protocol);
    feed_wire(&transport, &fixture.protocol,
              std::string(control_framing::kFramePrefix) + request(5, session, "system.ping") + "\n");
    require_contains(fixture.sink.last(), "\"pong\":true");
    require_top_level_session(fixture.sink.last(), session);
}

void test_stale_response_correlation() {
    Fixture fixture;
    fixture.payload(hello_request(1, kNonceA));
    const std::string hello_one = fixture.sink.last();
    const std::string session_one = extract_string(hello_one, "session");
    fixture.payload(request(5, session_one, "system.ping"));
    const std::string response_one = fixture.sink.last();
    require_top_level_session(response_one, session_one);

    fixture.payload(hello_request(1, kNonceB));
    const std::string hello_two = fixture.sink.last();
    const std::string session_two = extract_string(hello_two, "session");
    assert(session_two != session_one);
    fixture.payload(request(5, session_two, "system.ping"));
    const std::string response_two = fixture.sink.last();
    require_top_level_session(response_two, session_two);

    assert(response_one != response_two);
    assert(response_one.find("\"id\":5") != std::string::npos);
    assert(response_two.find("\"id\":5") != std::string::npos);
    assert(extract_string(hello_one, "client_nonce") == kNonceA);
    assert(extract_string(hello_two, "client_nonce") == kNonceB);
    require_top_level_session(hello_one, session_one);
    require_top_level_session(hello_two, session_two);
    assert(hello_one.find("\"client_nonce\":\"" + std::string(kNonceA) + "\"") !=
           std::string::npos);
    assert(hello_two.find("\"client_nonce\":\"" + std::string(kNonceB) + "\"") !=
           std::string::npos);
}

void test_nonce_session_and_hello_cache() {
    Fixture fixture;
    fixture.payload(hello_request(1, "0123"));
    require_contains(fixture.sink.last(), "\"code\":\"INVALID_PARAMS\"");
    fixture.payload(hello_request(1, "0123456789ABCDEF0123456789ABCDEF"));
    require_contains(fixture.sink.last(), "\"code\":\"INVALID_PARAMS\"");
    require_top_level_session(fixture.sink.last(), {});
    fixture.payload(hello_request(1, "0123456789abcdef0123456789abcdeg"));
    require_contains(fixture.sink.last(), "\"code\":\"INVALID_PARAMS\"");
    require_top_level_session(fixture.sink.last(), {});
    fixture.payload(hello_request(1, "0123456789abcdef0123456789abcdef"));
    const std::string first_hello = fixture.sink.last();
    const std::string first_session = extract_string(first_hello, "session");
    const std::string first_boot_id = extract_string(first_hello, "boot_id");
    assert(first_session.size() == 32 && first_boot_id.size() == 32);
    require_top_level_session(first_hello, first_session);
    assert(extract_string(first_hello, "client_nonce") == kNonceA);
    const std::string session_marker = "\"session\":\"" + first_session + "\"";
    assert(first_hello.find(session_marker) != first_hello.rfind(session_marker));
    require_contains(first_hello, "\"protocol.hello-v1\"");
    require_contains(first_hello, "\"usb.status-v1\"");
    assert(first_hello.find("hid.mouse.report-v1") == std::string::npos);

    fixture.payload(hello_request(1, kNonceA));
    assert(fixture.sink.last() == first_hello);
    fixture.payload(hello_request(2, kNonceA));
    require_contains(fixture.sink.last(), "\"code\":\"CLIENT_NONCE_CONFLICT\"");
    require_top_level_session(fixture.sink.last(), {});

    fixture.payload(hello_request(3, kNonceB));
    const std::string second_session = extract_string(fixture.sink.last(), "session");
    assert(second_session != first_session);
    fixture.payload(request(1, first_session, "system.ping"));
    require_contains(fixture.sink.last(), "\"code\":\"SESSION_MISMATCH\"");
    require_top_level_session(fixture.sink.last(), {});

    fixture.protocol.on_usb_unmount();
    fixture.payload(request(1, second_session, "system.ping"));
    require_contains(fixture.sink.last(), "\"code\":\"SESSION_MISMATCH\"");
    require_top_level_session(fixture.sink.last(), {});
    fixture.payload(hello_request(1, kNonceA));
    const std::string post_unmount_hello = fixture.sink.last();
    const std::string post_unmount_session = extract_string(post_unmount_hello, "session");
    assert(post_unmount_session != first_session);
    assert(post_unmount_hello != first_hello);
    require_top_level_session(post_unmount_hello, post_unmount_session);
    assert(extract_string(post_unmount_hello, "client_nonce") == kNonceA);

    Fixture reset_fixture(64);
    reset_fixture.payload(hello_request(1, kNonceA));
    assert(extract_string(reset_fixture.sink.last(), "boot_id") != first_boot_id);
}

void test_request_cache_and_commands() {
    Fixture fixture;
    fixture.payload(hello_request(1, kNonceA));
    const std::string session = extract_string(fixture.sink.last(), "session");

    const std::string ping_one = request(1, session, "system.ping");
    fixture.payload(ping_one);
    const std::string ping_response = fixture.sink.last();
    require_contains(ping_response, "\"pong\":true");
    require_top_level_session(ping_response, session);
    fixture.payload(ping_one);
    assert(fixture.sink.last() == ping_response);
    fixture.payload(request(1, session, "system.info"));
    require_contains(fixture.sink.last(), "\"code\":\"REQUEST_ID_CONFLICT\"");
    require_top_level_session(fixture.sink.last(), session);

    fixture.payload(request(2, session, "system.info"));
    require_contains(fixture.sink.last(), "\"project\":\"s3-hidbot\"");
    require_contains(fixture.sink.last(), "\"target\":\"esp32s3\"");
    require_top_level_session(fixture.sink.last(), session);
    assert(fixture.sink.last().find("/home/") == std::string::npos);

    fixture.status.status = {true, false, true, false};
    fixture.payload(request(10, session, "usb.status"));
    require_contains(fixture.sink.last(), "\"mounted\":true");
    require_contains(fixture.sink.last(), "\"mouse_ready\":false");
    require_top_level_session(fixture.sink.last(), session);
    fixture.status.status = {false, true, false, true};
    fixture.payload(request(11, session, "usb.status"));
    require_contains(fixture.sink.last(), "\"mounted\":false");
    require_contains(fixture.sink.last(), "\"suspended\":true");
    require_contains(fixture.sink.last(), "\"keyboard_ready\":false");
    require_contains(fixture.sink.last(), "\"mouse_ready\":true");
    require_top_level_session(fixture.sink.last(), session);
    fixture.payload(request(9, session, "system.ping"));
    require_contains(fixture.sink.last(), "\"code\":\"REQUEST_ID_STALE\"");
    require_top_level_session(fixture.sink.last(), session);

    const std::string unknown = request(12, session, "unknown.command");
    fixture.payload(unknown);
    const std::string unknown_response = fixture.sink.last();
    require_contains(unknown_response, "\"code\":\"UNKNOWN_COMMAND\"");
    require_top_level_session(unknown_response, session);
    fixture.payload(unknown);
    assert(fixture.sink.last() == unknown_response);

    fixture.payload(request(2147483647, session, "system.ping"));
    require_contains(fixture.sink.last(), "\"pong\":true");
    require_top_level_session(fixture.sink.last(), session);
    fixture.payload(request(13, session, "system.ping"));
    require_contains(fixture.sink.last(), "\"code\":\"REQUEST_ID_STALE\"");
    require_top_level_session(fixture.sink.last(), session);

    fixture.payload(hello_request(2, kNonceB));
    const std::string new_session = extract_string(fixture.sink.last(), "session");
    fixture.payload(request(1, new_session, "system.ping"));
    require_contains(fixture.sink.last(), "\"pong\":true");
    require_top_level_session(fixture.sink.last(), new_session);

    for (const std::string &frame : fixture.sink.frames) {
        assert(frame.size() <= 1024);
    }
}

}  // namespace

int main() {
    test_strict_envelope_and_framing();
    test_nonce_session_and_hello_cache();
    test_request_cache_and_commands();
    test_stale_response_correlation();
    return 0;
}
