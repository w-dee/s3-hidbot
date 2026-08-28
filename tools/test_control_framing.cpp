#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "control_framing/control_framing.hpp"

namespace {

using control_framing::Event;
using control_framing::EventKind;

struct Observation {
    EventKind kind;
    std::string payload;
};

struct Collector {
    std::vector<Observation> events;
};

void collect(void *context, const Event &event) {
    auto *collector = static_cast<Collector *>(context);
    collector->events.push_back(Observation{event.kind, std::string(event.payload)});
}

void feed(control_framing::Transport *transport,
          Collector *collector,
          std::string_view text) {
    transport->consume(reinterpret_cast<const std::uint8_t *>(text.data()),
                       text.size(),
                       collect,
                       collector);
}

void feed_bytes(control_framing::Transport *transport,
                Collector *collector,
                const std::vector<std::uint8_t> &bytes) {
    transport->consume(bytes.data(), bytes.size(), collect, collector);
}

void expect(const Collector &collector,
            std::initializer_list<EventKind> kinds) {
    assert(collector.events.size() == kinds.size());
    const bool equal = std::equal(collector.events.begin(),
                                  collector.events.end(),
                                  kinds.begin(),
                                  [](const Observation &observation, EventKind kind) {
                                      return observation.kind == kind;
                                  });
    assert(equal);
}

void test_complete_partial_multiple_and_logs() {
    control_framing::Transport transport;
    Collector collector;

    feed(&transport, &collector, "boot diagnostic\n@HID");
    assert(collector.events.empty());
    feed(&transport, &collector, "BOT {\"v\":1}\nnormal log\n@HIDBOT second\n");

    expect(collector, {EventKind::kFrame, EventKind::kFrame});
    assert(collector.events[0].payload == "{\"v\":1}");
    assert(collector.events[1].payload == "second");
}

void test_crlf_and_line_bounds() {
    control_framing::Transport transport;
    Collector collector;
    constexpr std::size_t prefix_length = sizeof(control_framing::kFramePrefix) - 1;

    feed(&transport, &collector, "@HIDBOT crlf\r\n");
    expect(collector, {EventKind::kFrame});
    assert(collector.events[0].payload == "crlf");

    collector.events.clear();
    const std::string max_line = std::string(control_framing::kFramePrefix) +
        std::string(control_framing::kMaxRequestLineBytes - prefix_length, 'x') + "\n";
    feed(&transport, &collector, max_line);
    expect(collector, {EventKind::kFrame});
    assert(collector.events[0].payload.size() ==
           control_framing::kMaxRequestLineBytes - prefix_length);

    collector.events.clear();
    const std::string overlong = std::string(control_framing::kFramePrefix) +
        std::string(control_framing::kMaxRequestLineBytes - prefix_length + 1, 'x') + "\n";
    feed(&transport, &collector, overlong + "@HIDBOT recovered\n");
    expect(collector, {EventKind::kOverlongProtocolFrame, EventKind::kFrame});
    assert(collector.events[1].payload == "recovered");
}

void test_transport_sync() {
    control_framing::Transport transport;
    Collector collector;

    feed(&transport, &collector, "@HIDBOT partial");
    feed_bytes(&transport, &collector, {0, 0, 0, 0});
    feed(&transport, &collector, "@HIDBOT after-sync\n");
    expect(collector, {EventKind::kTransportSync, EventKind::kFrame});
    assert(collector.events[1].payload == "after-sync");

    collector.events.clear();
    transport.reset();
    const std::array<std::array<std::uint8_t, 1>, 4> split_sync = {{{0}, {0}, {0}, {0}}};
    for (const auto &chunk : split_sync) {
        transport.consume(chunk.data(), chunk.size(), collect, &collector);
    }
    feed(&transport, &collector, "@HIDBOT split\n");
    expect(collector, {EventKind::kTransportSync, EventKind::kFrame});
    assert(collector.events[1].payload == "split");
}

void test_short_and_long_nul_runs() {
    for (const std::size_t count : {1U, 2U, 3U}) {
        control_framing::Transport transport;
        Collector collector;
        std::vector<std::uint8_t> bytes(count, 0);
        bytes.push_back('x');
        bytes.push_back('\n');
        feed_bytes(&transport, &collector, bytes);
        assert(std::none_of(collector.events.begin(),
                            collector.events.end(),
                            [](const Observation &event) {
                                return event.kind == EventKind::kTransportSync;
                            }));
    }

    control_framing::Transport transport;
    Collector collector;
    feed_bytes(&transport, &collector, {0, 0, 0, 0, 0, 0, 0});
    feed(&transport, &collector, "@HIDBOT long-sync\n");
    expect(collector, {EventKind::kTransportSync, EventKind::kFrame});
    assert(collector.events[1].payload == "long-sync");
}

void test_logs_before_and_after_a_frame() {
    control_framing::Transport transport;
    Collector collector;
    feed(&transport,
         &collector,
         "I (123) app: before\n@HIDBOT {\"type\":\"synthetic\"}\nI (124) app: after\n");
    expect(collector, {EventKind::kFrame});
    assert(collector.events[0].payload == "{\"type\":\"synthetic\"}");
}

}  // namespace

int main() {
    test_complete_partial_multiple_and_logs();
    test_crlf_and_line_bounds();
    test_transport_sync();
    test_short_and_long_nul_runs();
    test_logs_before_and_after_a_frame();
    return 0;
}
