#include "control_framing/control_framing.hpp"

namespace control_framing {
namespace {

constexpr std::size_t kPrefixLength = sizeof(kFramePrefix) - 1;

}  // namespace

void Transport::reset_line() {
    line_state_ = LineState::kSeekingPrefix;
    prefix_match_length_ = 0;
    frame_length_ = 0;
    frame_[0] = '\0';
}

void Transport::reset() {
    reset_line();
    consecutive_zeros_ = 0;
}

void Transport::emit(EventKind kind,
                     std::string_view payload,
                     EventCallback callback,
                     void *context) {
    if (callback != nullptr) {
        callback(context, Event{kind, payload});
    }
}

void Transport::consume_line_byte(std::uint8_t byte,
                                  EventCallback callback,
                                  void *context) {
    if (byte == '\n') {
        if (line_state_ == LineState::kCollectingFrame) {
            std::size_t payload_length = frame_length_ - kPrefixLength;
            if (payload_length > 0 && frame_[frame_length_ - 1] == '\r') {
                --payload_length;
            }
            emit(EventKind::kFrame,
                 std::string_view(frame_ + kPrefixLength, payload_length),
                 callback,
                 context);
        } else if (line_state_ == LineState::kDiscardingFrame) {
            emit(EventKind::kOverlongProtocolFrame, {}, callback, context);
        }
        reset_line();
        return;
    }

    switch (line_state_) {
        case LineState::kSeekingPrefix:
            if (byte != static_cast<std::uint8_t>(kFramePrefix[prefix_match_length_])) {
                line_state_ = LineState::kIgnoringLine;
                return;
            }
            frame_[frame_length_++] = static_cast<char>(byte);
            ++prefix_match_length_;
            if (prefix_match_length_ == kPrefixLength) {
                line_state_ = LineState::kCollectingFrame;
            }
            return;
        case LineState::kCollectingFrame:
            if (frame_length_ < kMaxRequestLineBytes) {
                frame_[frame_length_++] = static_cast<char>(byte);
            } else {
                line_state_ = LineState::kDiscardingFrame;
            }
            return;
        case LineState::kIgnoringLine:
        case LineState::kDiscardingFrame:
            return;
    }
}

void Transport::consume(const std::uint8_t *data,
                        std::size_t length,
                        EventCallback callback,
                        void *context) {
    if (data == nullptr) {
        return;
    }

    for (std::size_t index = 0; index < length; ++index) {
        const std::uint8_t byte = data[index];
        if (byte == 0) {
            if (consecutive_zeros_ < kTransportSyncLength) {
                ++consecutive_zeros_;
                if (consecutive_zeros_ == kTransportSyncLength) {
                    reset_line();
                    emit(EventKind::kTransportSync, {}, callback, context);
                }
            }
            continue;
        }

        if (consecutive_zeros_ < kTransportSyncLength) {
            for (std::uint8_t zero = 0; zero < consecutive_zeros_; ++zero) {
                consume_line_byte(0, callback, context);
            }
        }
        consecutive_zeros_ = 0;
        consume_line_byte(byte, callback, context);
    }
}

}  // namespace control_framing
