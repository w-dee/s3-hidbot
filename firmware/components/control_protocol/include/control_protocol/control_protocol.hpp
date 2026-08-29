#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "control_framing/control_framing.hpp"
#include "control_session/control_session.hpp"

namespace control_protocol {

inline constexpr std::uint32_t kProtocolVersion = 1;

struct Metadata {
    const char *project;
    const char *target;
    const char *idf_version;
};

struct UsbStatus {
    bool mounted;
    bool suspended;
    bool keyboard_ready;
    bool mouse_ready;
};

using UsbStatusProvider = UsbStatus (*)(void *context);
using OutputSink = bool (*)(void *context, const std::uint8_t *data, std::size_t length);

struct Config {
    Metadata metadata;
    UsbStatusProvider usb_status_provider;
    void *usb_status_context;
    OutputSink output;
    void *output_context;
};

class Protocol {
  public:
    bool initialize(const Config &config,
                    control_session::RandomFill random_fill,
                    void *random_context);

    void handle_framing_event(const control_framing::Event &event);
    void on_usb_unmount();

  private:
    void handle_frame(std::string_view payload);
    control_session::ResponseFrame &prepare_response_scratch();
    bool write_frame(const control_session::ResponseFrame &frame) const;

    Config config_{};
    control_session::State session_{};
    bool initialized_ = false;
    // Protocol is consumed only by the UART RX task. Keeping these reusable
    // workspaces on the Protocol instance avoids placing multi-kilobyte JSON
    // and response buffers on that task's stack.
    char request_json_scratch_[control_session::kMaxRequestBytes + 1]{};
    control_session::ResponseFrame response_scratch_{};
};

}  // namespace control_protocol
