#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace control_framing {

inline constexpr char kFramePrefix[] = "@HIDBOT ";
inline constexpr std::size_t kMaxRequestLineBytes = 512;
inline constexpr std::size_t kFrameStorageBytes = kMaxRequestLineBytes + 1;
inline constexpr std::size_t kTransportSyncLength = 4;

enum class EventKind : std::uint8_t {
    kFrame,
    kOverlongProtocolFrame,
    kTransportSync,
};

struct Event {
    EventKind kind;
    std::string_view payload;
};

using EventCallback = void (*)(void *context, const Event &event);

// Consumes the project-owned UART byte stream. It recognizes only protocol
// candidates that begin at the start of a line with kFramePrefix. Normal
// diagnostic lines are ignored without being buffered. A raw four-NUL token
// clears only the framing state; it never changes higher-level state.
class Transport {
  public:
    void reset();

    void consume(const std::uint8_t *data,
                 std::size_t length,
                 EventCallback callback,
                 void *context);

  private:
    enum class LineState : std::uint8_t {
        kSeekingPrefix,
        kCollectingFrame,
        kIgnoringLine,
        kDiscardingFrame,
    };

    void reset_line();
    void consume_line_byte(std::uint8_t byte, EventCallback callback, void *context);
    void emit(EventKind kind, std::string_view payload, EventCallback callback, void *context);

    LineState line_state_ = LineState::kSeekingPrefix;
    std::size_t prefix_match_length_ = 0;
    std::size_t frame_length_ = 0;
    std::uint8_t consecutive_zeros_ = 0;
    char frame_[kFrameStorageBytes]{};
};

}  // namespace control_framing
