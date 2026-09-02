#pragma once

#include <cstddef>
#include <cstdint>
#include <array>
#include <string_view>

#include "control_framing/control_framing.hpp"
#include "control_session/control_session.hpp"
#include "firmware_identity/firmware_identity.hpp"
#include "sensitive_request/sensitive_request.hpp"

namespace control_protocol {

inline constexpr std::uint32_t kProtocolVersion = 1;

struct Metadata {
    const char *project = nullptr;
    const char *target = nullptr;
    const char *idf_version = nullptr;
    // A null identity keeps the protocol core able to exercise legacy
    // metadata in native tests. The running C2 firmware always supplies a
    // validated, stable identity before starting the control plane.
    const firmware_identity::Identity *firmware_identity = nullptr;
};

struct UsbStatus {
    bool mounted;
    bool suspended;
    bool keyboard_ready;
    bool mouse_ready;
};

using UsbStatusProvider = UsbStatus (*)(void *context);

enum class UsbExposureDesired : std::uint8_t {
    kHidden,
    kExposed,
};

enum class UsbExposureObserved : std::uint8_t {
    kDriverNotInstalled,
    kDisconnected,
    kAttaching,
    kMounted,
    kSuspended,
    kDetaching,
};

enum class UsbExposureOperation : std::uint8_t {
    kInstall,
    kUninstall,
};

struct UsbExposureLastError {
    bool present = false;
    UsbExposureOperation operation = UsbExposureOperation::kInstall;
    std::int32_t code = 0;
};

struct UsbExposureStatus {
    UsbExposureDesired desired = UsbExposureDesired::kHidden;
    UsbExposureObserved observed = UsbExposureObserved::kDriverNotInstalled;
    std::uint32_t generation = 0;
    bool mounted = false;
    bool suspended = false;
    bool keyboard_ready = false;
    bool mouse_ready = false;
    bool safety_pending = false;
    bool host_release_uncertain = false;
    bool recovery_required = false;
    UsbExposureLastError last_error{};
};

enum class UsbExposureActionResult : std::uint8_t {
    kAccepted,
    kNoOp,
    kBusy,
};

struct UsbExposureActionOutcome {
    UsbExposureActionResult action_result = UsbExposureActionResult::kBusy;
    bool snapshot_valid = false;
    UsbExposureStatus snapshot{};
};

using UsbExposureStatusProvider = UsbExposureStatus (*)(void *context);
using UsbExposureActionProvider = UsbExposureActionOutcome (*)(void *context);

enum class BleExposureDesired : std::uint8_t { kHidden, kExposed };
enum class BleExposureObserved : std::uint8_t {
    kUninitialized,
    kEnabling,
    kIdle,
    kAdvertising,
    kConnected,
    kDisabling,
    kFault,
};
enum class BleExposureOperation : std::uint8_t { kEnable, kDisable, kRuntime };
struct BleExposureLastError {
    bool present = false;
    BleExposureOperation operation = BleExposureOperation::kRuntime;
    std::int32_t code = 0;
};
struct BleExposureStatus {
    BleExposureDesired desired = BleExposureDesired::kHidden;
    BleExposureObserved observed = BleExposureObserved::kUninitialized;
    std::uint32_t generation = 0;
    bool stack_ready = false;
    bool advertising = false;
    bool connected = false;
    bool recovery_required = false;
    BleExposureLastError last_error{};
};
enum class BleExposureActionResult : std::uint8_t { kAccepted, kNoOp, kBusy };
struct BleExposureActionOutcome {
    BleExposureActionResult action_result = BleExposureActionResult::kBusy;
    bool snapshot_valid = false;
    BleExposureStatus snapshot{};
};
using BleExposureStatusProvider = BleExposureStatus (*)(void *context);
using BleExposureActionProvider = BleExposureActionOutcome (*)(void *context);

enum class BlePairingState : std::uint8_t { kIdle, kSecuring, kWaitingInput };
enum class BlePairingLastResult : std::uint8_t {
    kNone,
    kSucceeded,
    kSmpFailed,
    kTimeout,
    kPeerDisconnected,
    kStoreFull,
    kStorage,
    kQueueOverflow,
    kRepeatPairing,
    kSecurityPolicy,
};
struct BlePairingStatus {
    bool available = true;
    BlePairingState state = BlePairingState::kIdle;
    std::uint32_t generation = 0;
    bool connected = false;
    bool input_pending = false;
    std::uint32_t pairing_id = 0;
    std::uint32_t remaining_ms = 0;
    bool encrypted = false;
    bool authenticated = false;
    bool bonded = false;
    bool secure_connections = false;
    std::uint8_t key_size = 0;
    BlePairingLastResult last_result = BlePairingLastResult::kNone;
};
struct BlePairingRespondRequest {
    std::uint32_t pairing_id = 0;
    std::array<char, 6> passkey{};
};
enum class BlePairingRespondResult : std::uint8_t {
    kAccepted,
    kNotPending,
    kInjectionFailed,
};
using BlePairingStatusProvider = BlePairingStatus (*)(void *context);
using BlePairingRespondProvider = BlePairingRespondResult (*)(
    void *context, const BlePairingRespondRequest &request);

enum class OutputRoute : std::uint8_t {
    kNone,
    kUsb,
};

enum class RouteTransition : std::uint8_t {
    kStable,
    kReleasing,
};

struct HidRouteStatus {
    OutputRoute desired = OutputRoute::kNone;
    OutputRoute active = OutputRoute::kNone;
    std::uint32_t generation = 0;
    RouteTransition transition = RouteTransition::kStable;
    bool ready = false;
};

enum class HidRouteActionResult : std::uint8_t {
    kAccepted,
    kNoOp,
    kBusy,
    kNotReady,
    kSafetyPending,
};

struct HidRouteActionOutcome {
    HidRouteActionResult action_result = HidRouteActionResult::kBusy;
    bool snapshot_valid = false;
    HidRouteStatus snapshot{};
};

using HidRouteStatusProvider = HidRouteStatus (*)(void *context);
using HidRouteActionProvider = HidRouteActionOutcome (*)(void *context,
                                                         OutputRoute desired);
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

enum class MouseReportState : std::uint8_t {
    kAlreadySet,
    kSubmitted,
};

enum class MouseReportFailure : std::uint8_t {
    kNone,
    kNotReady,
    kBusy,
    kSafetyPending,
    kAuthorityLost,
};

struct MouseReportRequest {
    std::uint8_t buttons = 0;
    std::int8_t x = 0;
    std::int8_t y = 0;
    std::int8_t wheel = 0;
    std::int8_t pan = 0;
};

struct MouseReportResult {
    bool success = false;
    bool authority_lost = false;
    MouseReportState state = MouseReportState::kSubmitted;
    MouseReportFailure failure = MouseReportFailure::kNotReady;
};

using MouseReportProvider = MouseReportResult (*)(
    void *context, const MouseReportRequest &request);

struct Config {
    Metadata metadata;
    UsbStatusProvider usb_status_provider;
    void *usb_status_context;
    UsbExposureStatusProvider usb_exposure_status_provider;
    void *usb_exposure_status_context;
    UsbExposureActionProvider usb_attach_provider;
    void *usb_attach_context;
    UsbExposureActionProvider usb_detach_provider;
    void *usb_detach_context;
    BleExposureStatusProvider ble_exposure_status_provider;
    void *ble_exposure_status_context;
    BleExposureActionProvider ble_enable_provider;
    void *ble_enable_context;
    BleExposureActionProvider ble_disable_provider;
    void *ble_disable_context;
    BlePairingStatusProvider ble_pairing_status_provider;
    void *ble_pairing_status_context;
    BlePairingRespondProvider ble_pairing_respond_provider;
    void *ble_pairing_respond_context;
    HidRouteStatusProvider hid_route_status_provider;
    void *hid_route_status_context;
    HidRouteActionProvider hid_route_set_provider;
    void *hid_route_set_context;
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
    MouseReportProvider mouse_report_provider;
    void *mouse_report_context;
};

class Protocol {
  public:
    bool initialize(const Config &config,
                    control_session::RandomFill random_fill,
                    void *random_context,
                    sensitive_request::SecureRandomFill secure_random_fill,
                    void *secure_random_context,
                    sensitive_request::HmacSha256 hmac,
                    void *hmac_context);

    void handle_framing_event(const control_framing::Event &event);
    void on_hid_lifecycle_invalidation();
    void on_hid_safety_failure();
    void service();

#ifdef CONTROL_PROTOCOL_NATIVE_TEST
    bool request_scratch_zero_for_test() const;
    control_session::State::RequestCacheSnapshot
    request_cache_snapshot_for_test() const;
#endif

  private:
    void handle_frame(std::string_view payload);
    control_session::ResponseFrame &prepare_response_scratch();
    bool write_frame(const control_session::ResponseFrame &frame) const;
    bool replay_control_transition_retry(std::string_view session, std::int32_t id,
                                         std::string_view payload);
    void cache_control_transition_retry(
        std::string_view session, std::int32_t id, std::string_view payload,
        const control_session::ResponseFrame &response);

    Config config_{};
    control_session::State session_{};
    sensitive_request::Identity sensitive_identity_{};
    bool initialized_ = false;
    bool lease_revoke_notified_ = false;
    // Protocol is consumed only by the UART RX task. Keeping these reusable
    // workspaces on the Protocol instance avoids placing multi-kilobyte JSON
    // and response buffers on that task's stack.
    char request_json_scratch_[control_session::kMaxRequestBytes + 1]{};
    control_session::ResponseFrame response_scratch_{};
    struct ControlTransitionRetryCache {
        bool active = false;
        std::int32_t id = 0;
        std::array<char, control_session::kTokenStorageBytes> session{};
        std::array<char, control_session::kMaxRequestBytes + 1> payload{};
        std::size_t payload_length = 0;
        control_session::ResponseFrame response{};
    } control_transition_retry_cache_{};
};

}  // namespace control_protocol
