#pragma once

#include <cstddef>
#include <cstdint>
#include <array>
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
using AuthorityEpochProvider = control_session::AuthorityEpoch (*)(void *context);
using OutputSink = bool (*)(void *context, const std::uint8_t *data, std::size_t length);
using SafetyCallback = void (*)(void *context);

enum class ReleaseAllInterfaceState : std::uint8_t {
    kAlreadyUp,
    kSubmitted,
};

struct ReleaseAllResult {
    bool success = false;
    bool authority_lost = false;
    ReleaseAllInterfaceState keyboard = ReleaseAllInterfaceState::kAlreadyUp;
    ReleaseAllInterfaceState mouse = ReleaseAllInterfaceState::kAlreadyUp;
};

using ReleaseAllProvider = ReleaseAllResult (*)(void *context);

enum class KeyboardReportState : std::uint8_t {
    kAlreadySet,
    kSubmitted,
};

enum class KeyboardReportFailure : std::uint8_t {
    kNone,
    kNotReady,
    kBusy,
    kSafetyPending,
    kAuthorityLost,
};

struct KeyboardReportRequest {
    std::uint8_t modifiers = 0;
    std::array<std::uint8_t, 6> keycodes{};
};

struct KeyboardReportResult {
    bool success = false;
    bool authority_lost = false;
    KeyboardReportState state = KeyboardReportState::kSubmitted;
    KeyboardReportFailure failure = KeyboardReportFailure::kNotReady;
};

using KeyboardReportProvider = KeyboardReportResult (*)(
    void *context, const KeyboardReportRequest &request);

struct Config {
    Metadata metadata;
    UsbStatusProvider usb_status_provider;
    void *usb_status_context;
    AuthorityEpochProvider authority_epoch_provider;
    void *authority_epoch_context;
    OutputSink output;
    void *output_context;
    control_session::NowFn now;
    void *now_context;
    SafetyCallback lease_expired;
    void *lease_expired_context;
    SafetyCallback session_takeover;
    void *session_takeover_context;
    SafetyCallback hid_safety_failure;
    void *hid_safety_failure_context;
    ReleaseAllProvider release_all_provider;
    void *release_all_context;
    KeyboardReportProvider keyboard_report_provider;
    void *keyboard_report_context;
};

class Protocol {
  public:
    bool initialize(const Config &config,
                    control_session::RandomFill random_fill,
                    void *random_context);

    void handle_framing_event(const control_framing::Event &event);
    void on_hid_lifecycle_invalidation();
    void on_hid_safety_failure();
    void service();

  private:
    void handle_frame(std::string_view payload);
    control_session::ResponseFrame &prepare_response_scratch();
    bool write_frame(const control_session::ResponseFrame &frame) const;

    Config config_{};
    control_session::State session_{};
    bool initialized_ = false;
    bool lease_revoke_notified_ = false;
    // Protocol is consumed only by the UART RX task. Keeping these reusable
    // workspaces on the Protocol instance avoids placing multi-kilobyte JSON
    // and response buffers on that task's stack.
    char request_json_scratch_[control_session::kMaxRequestBytes + 1]{};
    control_session::ResponseFrame response_scratch_{};
};

}  // namespace control_protocol
