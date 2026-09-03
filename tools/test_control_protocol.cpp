#include <cassert>
#include <array>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "cJSON.h"
#include "control_framing/control_framing.hpp"
#include "control_protocol/control_protocol.hpp"
#include "firmware_identity/firmware_identity.hpp"
#include "hid_runtime/hid_runtime.hpp"

namespace {

struct AllocationRecord {
    void *storage = nullptr;
    std::size_t length = 0;
};
std::vector<AllocationRecord> cjson_allocations;
bool watch_parser_secret = false;
bool parser_secret_present_at_free = false;
bool watch_embedded_nul_secret = false;
bool embedded_nul_secret_present_at_free = false;

void *tracked_cjson_malloc(std::size_t length) {
    void *storage = std::malloc(length);
    if (storage != nullptr) cjson_allocations.push_back({storage, length});
    return storage;
}

void tracked_cjson_free(void *storage) {
    const auto found = std::find_if(
        cjson_allocations.begin(), cjson_allocations.end(),
        [storage](const AllocationRecord &record) {
            return record.storage == storage;
        });
    if (found != cjson_allocations.end()) {
        constexpr std::array<unsigned char, 6> sentinel{
            '3', '1', '4', '1', '5', '9'};
        if (watch_parser_secret && found->length >= sentinel.size()) {
            const auto *bytes = static_cast<const unsigned char *>(storage);
            for (std::size_t index = 0;
                 index + sentinel.size() <= found->length; ++index) {
                if (std::memcmp(bytes + index, sentinel.data(),
                                sentinel.size()) == 0) {
                    parser_secret_present_at_free = true;
                }
            }
        }
        constexpr std::array<unsigned char, 7> embedded_nul_sentinel{
            '3', '1', 0, '4', '1', '5', '9'};
        if (watch_embedded_nul_secret &&
            found->length >= embedded_nul_sentinel.size()) {
            const auto *bytes = static_cast<const unsigned char *>(storage);
            for (std::size_t index = 0;
                 index + embedded_nul_sentinel.size() <= found->length;
                 ++index) {
                if (std::memcmp(bytes + index, embedded_nul_sentinel.data(),
                                embedded_nul_sentinel.size()) == 0) {
                    embedded_nul_secret_present_at_free = true;
                }
            }
        }
        cjson_allocations.erase(found);
    }
    std::free(storage);
}

constexpr char kNonceA[] = "0123456789abcdef0123456789abcdef";
constexpr char kNonceB[] = "fedcba9876543210fedcba9876543210";
constexpr std::size_t kMaxLogicalMachineFrameBytes = 1023;

firmware_identity::Identity make_test_identity() {
    std::array<std::uint8_t, firmware_identity::kAppElfSha256Bytes> digest{};
    for (std::size_t index = 0; index < digest.size(); ++index) {
        digest[index] = static_cast<std::uint8_t>(index + 1);
    }
    firmware_identity::Identity identity{};
    assert(firmware_identity::build_identity(
        &identity,
        "0.1.0-dev",
        firmware_identity::SourceRevisionInput{},
        digest,
        firmware_identity::kBuildProfile));
    return identity;
}

std::size_t count_occurrences(const std::string &value, std::string_view needle) {
    std::size_t count = 0;
    std::size_t position = 0;
    while ((position = value.find(needle, position)) != std::string::npos) {
        ++count;
        position += needle.size();
    }
    return count;
}

struct Sink {
    std::vector<std::string> frames;
    void (*after_write)(void *context) = nullptr;
    void *after_write_context = nullptr;

    static bool write(void *context, const std::uint8_t *data, std::size_t length) {
        auto *sink = static_cast<Sink *>(context);
        assert(length > 0 && length <= kMaxLogicalMachineFrameBytes);
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
        if (sink->after_write != nullptr) {
            sink->after_write(sink->after_write_context);
        }
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

    static bool secure_fill(void *context, std::uint8_t *output,
                            std::size_t length) {
        fill(context, output, length);
        return true;
    }
};

// TEST-ONLY deterministic provider. This deliberately does not implement or
// claim cryptographic HMAC; production provider integration is firmware-owned.
bool deterministic_hmac(void *, const std::uint8_t *key,
                        std::size_t key_length, const std::uint8_t *input,
                        std::size_t input_length,
                        std::uint8_t output[sensitive_request::kDigestBytes]) {
    assert(key_length == sensitive_request::kKeyBytes);
    for (std::size_t index = 0; index < sensitive_request::kDigestBytes;
         ++index) {
        output[index] = static_cast<std::uint8_t>(
            key[index] ^ static_cast<std::uint8_t>(0xa5U + index));
    }
    for (std::size_t index = 0; index < input_length; ++index) {
        const std::size_t lane = index % sensitive_request::kDigestBytes;
        output[lane] = static_cast<std::uint8_t>(
            output[lane] * 33U + input[index] +
            static_cast<std::uint8_t>(index * 17U));
    }
    output[input_length % sensitive_request::kDigestBytes] ^=
        static_cast<std::uint8_t>(input_length);
    return true;
}

struct StatusSource {
    control_protocol::UsbStatus status{false, false, false, false};

    static control_protocol::UsbStatus get(void *context) {
        return static_cast<StatusSource *>(context)->status;
    }
};

struct AuthoritySource {
    control_session::AuthorityEpoch epoch = 10;

    static control_session::AuthorityEpoch get(void *context) {
        return static_cast<AuthoritySource *>(context)->epoch;
    }
};

struct ExposureSource {
    control_protocol::UsbExposureStatus status{};
    control_protocol::UsbExposureActionResult attach_result =
        control_protocol::UsbExposureActionResult::kAccepted;
    control_protocol::UsbExposureActionResult detach_result =
        control_protocol::UsbExposureActionResult::kAccepted;
    AuthoritySource *authority = nullptr;
    bool advance_attach_before_return = false;
    bool advance_detach_before_return = false;
    int attach_calls = 0;
    int detach_calls = 0;

    static control_protocol::UsbExposureStatus get(void *context) {
        return static_cast<ExposureSource *>(context)->status;
    }

    static control_protocol::UsbExposureActionOutcome attach(void *context) {
        auto *source = static_cast<ExposureSource *>(context);
        ++source->attach_calls;
        if (source->attach_result == control_protocol::UsbExposureActionResult::kAccepted) {
            ++source->status.generation;
            source->status.desired = control_protocol::UsbExposureDesired::kExposed;
            source->status.observed = control_protocol::UsbExposureObserved::kAttaching;
            source->status.recovery_required = false;
            source->status.last_error = {};
            assert(source->authority != nullptr);
            ++source->authority->epoch;
            const control_protocol::UsbExposureStatus accepted = source->status;
            if (source->advance_attach_before_return) {
                source->status.observed = control_protocol::UsbExposureObserved::kMounted;
                source->status.mounted = true;
                source->status.keyboard_ready = true;
                source->status.mouse_ready = true;
            }
            return control_protocol::UsbExposureActionOutcome{
                .action_result = control_protocol::UsbExposureActionResult::kAccepted,
                .snapshot_valid = true,
                .snapshot = accepted,
            };
        }
        if (source->attach_result == control_protocol::UsbExposureActionResult::kNoOp) {
            return control_protocol::UsbExposureActionOutcome{
                .action_result = control_protocol::UsbExposureActionResult::kNoOp,
                .snapshot_valid = true,
                .snapshot = source->status,
            };
        }
        return {};
    }

    static control_protocol::UsbExposureActionOutcome detach(void *context) {
        auto *source = static_cast<ExposureSource *>(context);
        ++source->detach_calls;
        if (source->detach_result == control_protocol::UsbExposureActionResult::kAccepted) {
            source->status.desired = control_protocol::UsbExposureDesired::kHidden;
            source->status.observed = control_protocol::UsbExposureObserved::kDetaching;
            source->status.safety_pending = true;
            assert(source->authority != nullptr);
            ++source->authority->epoch;
            const control_protocol::UsbExposureStatus accepted = source->status;
            if (source->advance_detach_before_return) {
                ++source->status.generation;
                source->status.observed =
                    control_protocol::UsbExposureObserved::kDriverNotInstalled;
                source->status.mounted = false;
                source->status.suspended = false;
                source->status.keyboard_ready = false;
                source->status.mouse_ready = false;
                source->status.safety_pending = false;
            }
            return control_protocol::UsbExposureActionOutcome{
                .action_result = control_protocol::UsbExposureActionResult::kAccepted,
                .snapshot_valid = true,
                .snapshot = accepted,
            };
        }
        if (source->detach_result == control_protocol::UsbExposureActionResult::kNoOp) {
            return control_protocol::UsbExposureActionOutcome{
                .action_result = control_protocol::UsbExposureActionResult::kNoOp,
                .snapshot_valid = true,
                .snapshot = source->status,
            };
        }
        return {};
    }
};

struct BleSource {
    control_protocol::BleExposureStatus status{};
    control_protocol::BleExposureActionResult enable_result =
        control_protocol::BleExposureActionResult::kAccepted;
    control_protocol::BleExposureActionResult disable_result =
        control_protocol::BleExposureActionResult::kAccepted;
    bool advance_before_return = false;
    int enable_calls = 0;
    int disable_calls = 0;

    static control_protocol::BleExposureStatus get(void *context) {
        return static_cast<BleSource *>(context)->status;
    }
    static control_protocol::BleExposureActionOutcome enable(void *context) {
        auto *source = static_cast<BleSource *>(context);
        ++source->enable_calls;
        if (source->enable_result == control_protocol::BleExposureActionResult::kBusy) {
            return {};
        }
        if (source->enable_result == control_protocol::BleExposureActionResult::kAccepted) {
            ++source->status.generation;
            source->status.desired = control_protocol::BleExposureDesired::kExposed;
            source->status.observed = control_protocol::BleExposureObserved::kEnabling;
        }
        const auto accepted = source->status;
        if (source->advance_before_return) {
            source->status.stack_ready = true;
            source->status.advertising = true;
            source->status.observed = control_protocol::BleExposureObserved::kAdvertising;
        }
        return {.action_result = source->enable_result,
                .snapshot_valid = true,
                .snapshot = accepted};
    }
    static control_protocol::BleExposureActionOutcome disable(void *context) {
        auto *source = static_cast<BleSource *>(context);
        ++source->disable_calls;
        if (source->disable_result == control_protocol::BleExposureActionResult::kBusy) {
            return {};
        }
        if (source->disable_result == control_protocol::BleExposureActionResult::kAccepted) {
            ++source->status.generation;
            source->status.desired = control_protocol::BleExposureDesired::kHidden;
            source->status.observed = control_protocol::BleExposureObserved::kDisabling;
        }
        return {.action_result = source->disable_result,
                .snapshot_valid = true,
                .snapshot = source->status};
    }
};

struct RouteSource {
    control_protocol::HidRouteStatus status{};
    control_protocol::HidRouteActionResult result =
        control_protocol::HidRouteActionResult::kAccepted;
    AuthoritySource *authority = nullptr;
    bool finish_release_before_return = false;
    int set_calls = 0;

    static control_protocol::HidRouteStatus get(void *context) {
        return static_cast<RouteSource *>(context)->status;
    }

    static control_protocol::HidRouteActionOutcome set(
        void *context, control_protocol::OutputRoute desired) {
        auto *source = static_cast<RouteSource *>(context);
        ++source->set_calls;
        if (source->result == control_protocol::HidRouteActionResult::kAccepted) {
            assert(source->authority != nullptr);
            ++source->authority->epoch;
            if (desired == control_protocol::OutputRoute::kUsb ||
                desired == control_protocol::OutputRoute::kBle) {
                ++source->status.generation;
                source->status.desired = desired;
                source->status.active = desired;
                source->status.transition = control_protocol::RouteTransition::kStable;
                source->status.ready = true;
                return control_protocol::HidRouteActionOutcome{
                    .action_result = source->result,
                    .snapshot_valid = true,
                    .snapshot = source->status,
                };
            }
            source->status.desired = control_protocol::OutputRoute::kNone;
            source->status.transition = control_protocol::RouteTransition::kReleasing;
            source->status.ready = false;
            const control_protocol::HidRouteStatus accepted = source->status;
            if (source->finish_release_before_return) {
                source->status.active = control_protocol::OutputRoute::kNone;
                ++source->status.generation;
                source->status.transition = control_protocol::RouteTransition::kStable;
            }
            return control_protocol::HidRouteActionOutcome{
                .action_result = source->result,
                .snapshot_valid = true,
                .snapshot = accepted,
            };
        }
        if (source->result == control_protocol::HidRouteActionResult::kNoOp) {
            return control_protocol::HidRouteActionOutcome{
                .action_result = source->result,
                .snapshot_valid = true,
                .snapshot = source->status,
            };
        }
        return control_protocol::HidRouteActionOutcome{
            .action_result = source->result,
            .snapshot_valid = false,
        };
    }
};

struct ReleaseSource {
    control_protocol::ReleaseAllResult result{
        .success = true,
        .authority_lost = false,
        .keyboard = control_protocol::ReleaseAllInterfaceState::kAlreadyUp,
        .mouse = control_protocol::ReleaseAllInterfaceState::kAlreadyUp,
    };
    int calls = 0;

    static control_protocol::ReleaseAllResult get(void *context) {
        auto *source = static_cast<ReleaseSource *>(context);
        ++source->calls;
        return source->result;
    }
};

struct KeyboardSource {
    control_protocol::KeyboardReportResult result{
        .success = true,
        .authority_lost = false,
        .state = control_protocol::KeyboardReportState::kSubmitted,
        .failure = control_protocol::KeyboardReportFailure::kNone,
    };
    control_protocol::KeyboardReportRequest request{};
    int calls = 0;

    static control_protocol::KeyboardReportResult get(
        void *context, const control_protocol::KeyboardReportRequest &request) {
        auto *source = static_cast<KeyboardSource *>(context);
        source->request = request;
        ++source->calls;
        return source->result;
    }
};

struct MouseSource {
    control_protocol::MouseReportResult result{
        .success = true,
        .authority_lost = false,
        .state = control_protocol::MouseReportState::kSubmitted,
        .failure = control_protocol::MouseReportFailure::kNone,
    };
    control_protocol::MouseReportRequest request{};
    int calls = 0;

    static control_protocol::MouseReportResult get(
        void *context, const control_protocol::MouseReportRequest &request) {
        auto *source = static_cast<MouseSource *>(context);
        source->request = request;
        ++source->calls;
        return source->result;
    }
};

struct PairingSource {
    control_protocol::BlePairingStatus status{};
    control_protocol::BlePairingRespondResult respond_result =
        control_protocol::BlePairingRespondResult::kAccepted;
    control_protocol::BlePairingRespondRequest request{};
    int status_calls = 0;
    int respond_calls = 0;

    static control_protocol::BlePairingStatus get(void *context) {
        auto *source = static_cast<PairingSource *>(context);
        ++source->status_calls;
        return source->status;
    }

    static control_protocol::BlePairingRespondResult respond(
        void *context,
        const control_protocol::BlePairingRespondRequest &request) {
        auto *source = static_cast<PairingSource *>(context);
        ++source->respond_calls;
        source->request = request;
        return source->respond_result;
    }
};

struct BondSource {
    control_protocol::BleBondListResult list_result{
        .kind = control_protocol::BleBondListResultKind::kSuccess,
        .healthy = true,
    };
    control_protocol::BleBondRemoveResult remove_result{};
    control_protocol::BondId requested_id{};
    int list_calls = 0;
    int remove_calls = 0;

    static control_protocol::BleBondListResult list(void *context) {
        auto *source = static_cast<BondSource *>(context);
        ++source->list_calls;
        return source->list_result;
    }

    static control_protocol::BleBondRemoveResult remove(
        void *context, const control_protocol::BondId &bond_id) {
        auto *source = static_cast<BondSource *>(context);
        ++source->remove_calls;
        source->requested_id = bond_id;
        source->remove_result.bond_id = bond_id;
        return source->remove_result;
    }
};

void increment_authority(void *context) {
    ++static_cast<AuthoritySource *>(context)->epoch;
}

struct LeaseClock {
    std::uint64_t value = 0;

    static std::uint64_t now(void *context) {
        return static_cast<LeaseClock *>(context)->value;
    }
};

struct LeaseFixture {
    Sink sink;
    RandomSource random;
    LeaseClock clock;
    AuthoritySource authority;
    ExposureSource exposure;
    BleSource ble;
    PairingSource pairing;
    BondSource bonds;
    RouteSource route;
    ReleaseSource release;
    int expired_callbacks = 0;
    int takeover_callbacks = 0;
    int hid_failure_callbacks = 0;
    hid_runtime::StateMachine runtime;
    control_protocol::Protocol protocol;

    static control_protocol::UsbStatus usb_status(void *context) {
        const auto status = static_cast<LeaseFixture *>(context)->runtime.status();
        return control_protocol::UsbStatus{
            status.mounted,
            status.suspended,
            status.keyboard_ready,
            status.mouse_ready,
        };
    }

    static control_protocol::UsbExposureStatus exposure_status(void *context) {
        auto *fixture = static_cast<LeaseFixture *>(context);
        const auto lifecycle = fixture->runtime.usb_lifecycle_snapshot();
        const auto status = fixture->runtime.status();
        return control_protocol::UsbExposureStatus{
            .desired = static_cast<control_protocol::UsbExposureDesired>(lifecycle.desired),
            .observed = static_cast<control_protocol::UsbExposureObserved>(lifecycle.observed),
            .generation = lifecycle.generation,
            .mounted = status.mounted,
            .suspended = status.suspended,
            .keyboard_ready = status.keyboard_ready,
            .mouse_ready = status.mouse_ready,
            .safety_pending = lifecycle.safety_pending,
            .host_release_uncertain = lifecycle.host_release_uncertain,
            .recovery_required = lifecycle.recovery_required,
            .last_error = {
                .present = lifecycle.last_error.present,
                .operation = static_cast<control_protocol::UsbExposureOperation>(
                    lifecycle.last_error.operation),
                .code = lifecycle.last_error.code,
            },
        };
    }

    static void expired(void *context) {
        auto *fixture = static_cast<LeaseFixture *>(context);
        ++fixture->expired_callbacks;
        fixture->runtime.request_release_all();
    }

    static void takeover(void *context) {
        auto *fixture = static_cast<LeaseFixture *>(context);
        ++fixture->takeover_callbacks;
        fixture->runtime.request_release_all();
    }

    static void hid_failure(void *context) {
        auto *fixture = static_cast<LeaseFixture *>(context);
        ++fixture->hid_failure_callbacks;
        fixture->runtime.request_release_all();
    }

    LeaseFixture() {
        exposure.authority = &authority;
        route.authority = &authority;
        const control_protocol::Config config{
            .metadata = {"s3-hidbot", "esp32s3", "v5.5.4"},
            .usb_status_provider = LeaseFixture::usb_status,
            .usb_status_context = this,
            .usb_exposure_status_provider = LeaseFixture::exposure_status,
            .usb_exposure_status_context = this,
            .usb_attach_provider = ExposureSource::attach,
            .usb_attach_context = &exposure,
            .usb_detach_provider = ExposureSource::detach,
            .usb_detach_context = &exposure,
            .ble_exposure_status_provider = BleSource::get,
            .ble_exposure_status_context = &ble,
            .ble_enable_provider = BleSource::enable,
            .ble_enable_context = &ble,
            .ble_disable_provider = BleSource::disable,
            .ble_disable_context = &ble,
            .ble_pairing_status_provider = PairingSource::get,
            .ble_pairing_status_context = &pairing,
            .ble_pairing_respond_provider = PairingSource::respond,
            .ble_pairing_respond_context = &pairing,
            .ble_bond_list_provider = BondSource::list,
            .ble_bond_list_context = &bonds,
            .ble_bond_remove_provider = BondSource::remove,
            .ble_bond_remove_context = &bonds,
            .hid_route_status_provider = RouteSource::get,
            .hid_route_status_context = &route,
            .hid_route_set_provider = RouteSource::set,
            .hid_route_set_context = &route,
            .authority_epoch_provider = AuthoritySource::get,
            .authority_epoch_context = &authority,
            .output = Sink::write,
            .output_context = &sink,
            .now = LeaseClock::now,
            .now_context = &clock,
            .lease_expired = LeaseFixture::expired,
            .lease_expired_context = this,
            .session_takeover = LeaseFixture::takeover,
            .session_takeover_context = this,
            .hid_safety_failure = LeaseFixture::hid_failure,
            .hid_safety_failure_context = this,
            .release_all_provider = ReleaseSource::get,
            .release_all_context = &release,
            .keyboard_report_provider = nullptr,
            .keyboard_report_context = nullptr,
            .mouse_report_provider = nullptr,
            .mouse_report_context = nullptr,
        };
        assert(protocol.initialize(config, RandomSource::fill, &random,
                                   RandomSource::secure_fill, &random,
                                   deterministic_hmac, nullptr));
    }

    void payload(std::string_view json) {
        protocol.handle_framing_event(
            control_framing::Event{control_framing::EventKind::kFrame, json});
    }
};

struct Fixture {
    Sink sink;
    RandomSource random;
    StatusSource status;
    AuthoritySource authority;
    ExposureSource exposure;
    BleSource ble;
    PairingSource pairing;
    BondSource bonds;
    RouteSource route;
    ReleaseSource release;
    KeyboardSource keyboard;
    MouseSource mouse;
    firmware_identity::Identity identity{};
    bool identity_enabled = false;
    control_protocol::Protocol protocol;

    explicit Fixture(std::uint8_t random_seed = 0, bool with_identity = false)
        : identity_enabled(with_identity) {
        random.next = random_seed;
        if (identity_enabled) {
            identity = make_test_identity();
        }
        exposure.authority = &authority;
        route.authority = &authority;
        assert(protocol.initialize(configuration(), RandomSource::fill, &random,
                                   RandomSource::secure_fill, &random,
                                   deterministic_hmac, nullptr));
    }

    control_protocol::Config configuration() {
        return control_protocol::Config{
            .metadata = {
                .project = "s3-hidbot",
                .target = "esp32s3",
                .idf_version = "v5.5.4",
                .firmware_identity = identity_enabled ? &identity : nullptr,
            },
            .usb_status_provider = StatusSource::get,
            .usb_status_context = &status,
            .usb_exposure_status_provider = ExposureSource::get,
            .usb_exposure_status_context = &exposure,
            .usb_attach_provider = ExposureSource::attach,
            .usb_attach_context = &exposure,
            .usb_detach_provider = ExposureSource::detach,
            .usb_detach_context = &exposure,
            .ble_exposure_status_provider = BleSource::get,
            .ble_exposure_status_context = &ble,
            .ble_enable_provider = BleSource::enable,
            .ble_enable_context = &ble,
            .ble_disable_provider = BleSource::disable,
            .ble_disable_context = &ble,
            .ble_pairing_status_provider = PairingSource::get,
            .ble_pairing_status_context = &pairing,
            .ble_pairing_respond_provider = PairingSource::respond,
            .ble_pairing_respond_context = &pairing,
            .ble_bond_list_provider = BondSource::list,
            .ble_bond_list_context = &bonds,
            .ble_bond_remove_provider = BondSource::remove,
            .ble_bond_remove_context = &bonds,
            .hid_route_status_provider = RouteSource::get,
            .hid_route_status_context = &route,
            .hid_route_set_provider = RouteSource::set,
            .hid_route_set_context = &route,
            .authority_epoch_provider = AuthoritySource::get,
            .authority_epoch_context = &authority,
            .output = Sink::write,
            .output_context = &sink,
            .now = nullptr,
            .now_context = nullptr,
            .lease_expired = nullptr,
            .lease_expired_context = nullptr,
            .session_takeover = nullptr,
            .session_takeover_context = nullptr,
            .hid_safety_failure = nullptr,
            .hid_safety_failure_context = nullptr,
            .release_all_provider = ReleaseSource::get,
            .release_all_context = &release,
            .keyboard_report_provider = KeyboardSource::get,
            .keyboard_report_context = &keyboard,
            .mouse_report_provider = MouseSource::get,
            .mouse_report_context = &mouse,
        };
    }

    void payload(std::string_view json) {
        protocol.handle_framing_event(
            control_framing::Event{control_framing::EventKind::kFrame, json});
    }
};

void require_contains(const std::string &value, std::string_view expected) {
    if (value.find(expected) == std::string::npos) {
        std::fprintf(stderr, "missing [%.*s] in [%s]\n",
                     static_cast<int>(expected.size()), expected.data(),
                     value.c_str());
    }
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

control_protocol::BondId bond_id(char digit) {
    control_protocol::BondId result{};
    result.fill(digit);
    result.back() = '\0';
    return result;
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
    assert(transport.storage_zero_for_test());
    assert(fixture.protocol.request_scratch_zero_for_test());
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
    require_contains(first_hello, "hid.mouse-report-v1");

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

    ++fixture.authority.epoch;
    fixture.protocol.on_hid_lifecycle_invalidation();
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
        assert(frame.size() <= kMaxLogicalMachineFrameBytes);
    }
}

void test_identity_hello_and_info_shapes() {
    Fixture fixture(0, true);
    fixture.payload(hello_request(1, kNonceA));
    const std::string hello = fixture.sink.last();
    const std::string session = extract_string(hello, "session");

    for (std::string_view capability : {
             "protocol.hello-v1", "system.ping-v1", "system.info-v1",
             "usb.status-v1", "usb.exposure-control-v1", "hid.lease-v1", "hid.release-all-v1",
             "hid.keyboard-report-v1", "hid.mouse-report-v1", "firmware.identity-v1",
             "hid.output-route-v1", "hid.output-route-v2",
             "ble.exposure-control-v1",
         }) {
        assert(count_occurrences(hello, std::string("\"") + std::string(capability) + "\"") == 1);
    }
    assert(hello.find("\"firmware\":") == std::string::npos);
    assert(count_occurrences(hello, "\"result\":") == 1);

    fixture.payload(request(2, session, "system.info"));
    const std::string null_revision_info = fixture.sink.last();
    require_contains(null_revision_info,
                     "\"firmware\":{\"version\":\"0.1.0-dev\",\"source_revision\":null,");
    require_contains(null_revision_info,
                     "\"app_elf_sha256\":\"0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20\"");
    require_contains(null_revision_info, "\"build_profile\":\"freenove-fnk0085\"}");
    assert(null_revision_info.size() <= kMaxLogicalMachineFrameBytes);

    constexpr char kFullRevision[] =
        "0123456789abcdef0123456789abcdef01234567";
    std::array<std::uint8_t, firmware_identity::kAppElfSha256Bytes> full_digest{};
    full_digest.fill(0xabU);
    assert(firmware_identity::build_identity(
        &fixture.identity,
        "1.2.3+build",
        firmware_identity::SourceRevisionInput{true, kFullRevision},
        full_digest,
        firmware_identity::kBuildProfile));
    fixture.payload(request(3, session, "system.info"));
    const std::string full_revision_info = fixture.sink.last();
    require_contains(full_revision_info,
                     "\"source_revision\":\"0123456789abcdef0123456789abcdef01234567\"");
    require_contains(full_revision_info, "\"version\":\"1.2.3+build\"");
    assert(full_revision_info.size() <= kMaxLogicalMachineFrameBytes);

    const std::string maximum_version = std::string(27, '1') + ".0.0";
    const std::string maximum_profile(31, 'a');
    std::array<std::uint8_t, firmware_identity::kAppElfSha256Bytes> maximum_digest{};
    maximum_digest.fill(0x5aU);
    assert(firmware_identity::build_identity(
        &fixture.identity,
        maximum_version,
        firmware_identity::SourceRevisionInput{true, kFullRevision},
        maximum_digest,
        maximum_profile));
    fixture.payload(request(4, session, "system.info"));
    assert(fixture.sink.last().size() <= kMaxLogicalMachineFrameBytes);
}

void test_invalid_identity_rejected_at_protocol_initialization() {
    auto expect_rejected = [](auto mutate) {
        Fixture fixture(0, true);
        mutate(fixture.identity);
        control_protocol::Protocol rejected;
        assert(!rejected.initialize(fixture.configuration(), RandomSource::fill,
                                    &fixture.random, RandomSource::secure_fill,
                                    &fixture.random, deterministic_hmac, nullptr));
    };

    expect_rejected([](firmware_identity::Identity &identity) {
        identity.version[0] = '\0';
    });
    expect_rejected([](firmware_identity::Identity &identity) {
        identity.source_revision_present = true;
        identity.source_revision[0] = 'G';
    });
    expect_rejected([](firmware_identity::Identity &identity) {
        std::fill(identity.app_elf_sha256.begin(), identity.app_elf_sha256.end(), '0');
        identity.app_elf_sha256.back() = '\0';
    });
    expect_rejected([](firmware_identity::Identity &identity) {
        identity.build_profile[0] = 'F';
    });
}

void test_response_scratch_reuse() {
    Fixture fixture;

    fixture.payload("{bad");
    require_contains(fixture.sink.last(), "\"code\":\"MALFORMED_JSON\"");

    fixture.payload(hello_request(1, kNonceA));
    const std::string session = extract_string(fixture.sink.last(), "session");
    const std::string hello_response = fixture.sink.last();
    assert(hello_response.find("MALFORMED_JSON") == std::string::npos);

    fixture.payload(request(2, session, "system.ping"));
    const std::string ping_response = fixture.sink.last();
    require_contains(ping_response, "\"pong\":true");
    assert(ping_response.find("MALFORMED_JSON") == std::string::npos);

    fixture.payload(request(3, session, "system.ping", "null"));
    const std::string invalid_params_response = fixture.sink.last();
    require_contains(invalid_params_response, "\"code\":\"INVALID_PARAMS\"");
    assert(invalid_params_response.find("\"pong\":true") == std::string::npos);

    fixture.payload(request(4, session, "system.info"));
    const std::string info_response = fixture.sink.last();
    require_contains(info_response, "\"project\":\"s3-hidbot\"");
    assert(info_response.find("\"pong\":true") == std::string::npos);
    assert(info_response.find("INVALID_PARAMS") == std::string::npos);

    const std::string status_request = request(5, session, "usb.status");
    fixture.payload(status_request);
    const std::string status_response = fixture.sink.last();
    require_contains(status_response, "\"mounted\":false");
    assert(status_response.find("\"project\":") == std::string::npos);
    assert(status_response.find("\"pong\":true") == std::string::npos);

    fixture.payload(status_request);
    assert(fixture.sink.last() == status_response);
}

void test_usb_exposure_commands_schema_and_lifecycle_retry_cache() {
    Fixture fixture;
    fixture.payload(hello_request(1, kNonceA));
    const std::string session_a = extract_string(fixture.sink.last(), "session");

    fixture.exposure.status = {
        .desired = control_protocol::UsbExposureDesired::kHidden,
        .observed = control_protocol::UsbExposureObserved::kDriverNotInstalled,
        .generation = 0,
        .mounted = false,
        .suspended = false,
        .keyboard_ready = false,
        .mouse_ready = false,
        .safety_pending = false,
        .host_release_uncertain = false,
        .recovery_required = false,
        .last_error = {},
    };
    fixture.payload(request(2, session_a, "usb.exposure.status"));
    const std::string cold = fixture.sink.last();
    require_contains(cold,
                     "\"result\":{\"desired\":\"hidden\",\"observed\":\"driver_not_installed\","
                     "\"generation\":0,\"mounted\":false,\"suspended\":false,"
                     "\"keyboard_ready\":false,\"mouse_ready\":false,\"safety_pending\":false,"
                     "\"host_release_uncertain\":false,\"recovery_required\":false,\"last_error\":null}");
    assert(count_occurrences(cold, "\"desired\":") == 1);
    assert(count_occurrences(cold, "\"last_error\":") == 1);

    const std::string attach = request(3, session_a, "usb.attach");
    fixture.payload(attach);
    const std::string accepted_attach = fixture.sink.last();
    require_contains(accepted_attach,
                     "\"desired\":\"exposed\",\"observed\":\"attaching\",\"generation\":1");
    assert(fixture.exposure.attach_calls == 1);
    // Lifecycle task completion is asynchronous. The exact original retry
    // remains the accepted linearization response and does not rerun attach.
    fixture.exposure.status.observed = control_protocol::UsbExposureObserved::kDisconnected;
    fixture.payload(attach);
    assert(fixture.sink.last() == accepted_attach);
    assert(fixture.exposure.attach_calls == 1);
    fixture.payload(request(4, session_a, "usb.exposure.status"));
    require_contains(fixture.sink.last(), "\"code\":\"SESSION_MISMATCH\"");

    fixture.payload(hello_request(1, kNonceB));
    const std::string session_b = extract_string(fixture.sink.last(), "session");
    fixture.exposure.attach_result = control_protocol::UsbExposureActionResult::kNoOp;
    const std::string no_op_attach = request(2, session_b, "usb.attach");
    fixture.payload(no_op_attach);
    const std::string no_op_response = fixture.sink.last();
    require_contains(no_op_response, "\"observed\":\"disconnected\"");
    assert(fixture.exposure.attach_calls == 2);
    fixture.payload(no_op_attach);
    assert(fixture.sink.last() == no_op_response);
    assert(fixture.exposure.attach_calls == 2);

    fixture.exposure.detach_result = control_protocol::UsbExposureActionResult::kBusy;
    fixture.payload(request(3, session_b, "usb.detach"));
    require_contains(fixture.sink.last(), "\"code\":\"HID_BUSY\"");
    assert(fixture.exposure.detach_calls == 1);

    fixture.exposure.detach_result = control_protocol::UsbExposureActionResult::kAccepted;
    const std::string detach = request(4, session_b, "usb.detach");
    fixture.payload(detach);
    const std::string accepted_detach = fixture.sink.last();
    require_contains(accepted_detach,
                     "\"desired\":\"hidden\",\"observed\":\"detaching\"");
    assert(fixture.exposure.detach_calls == 2);
    fixture.exposure.status.observed = control_protocol::UsbExposureObserved::kDriverNotInstalled;
    fixture.exposure.status.safety_pending = false;
    fixture.payload(detach);
    assert(fixture.sink.last() == accepted_detach);
    assert(fixture.exposure.detach_calls == 2);

    fixture.payload(hello_request(1, "00112233445566778899aabbccddeeff"));
    const std::string session_c = extract_string(fixture.sink.last(), "session");
    fixture.exposure.status = {
        .desired = control_protocol::UsbExposureDesired::kHidden,
        .observed = control_protocol::UsbExposureObserved::kDetaching,
        .generation = 2,
        .mounted = false,
        .suspended = false,
        .keyboard_ready = false,
        .mouse_ready = false,
        .safety_pending = true,
        .host_release_uncertain = true,
        .recovery_required = true,
        .last_error = {
            .present = true,
            .operation = control_protocol::UsbExposureOperation::kUninstall,
            .code = -7,
        },
    };
    fixture.payload("{\"v\":1,\"id\":1,\"session\":\"" + session_c +
                    "\",\"cmd\":\"usb.exposure.status\"}");
    require_contains(fixture.sink.last(),
                     "\"last_error\":{\"operation\":\"uninstall\",\"code\":-7}");
    const int attach_calls_before_invalid = fixture.exposure.attach_calls;
    const int detach_calls_before_invalid = fixture.exposure.detach_calls;
    fixture.payload(request(2, session_c, "usb.attach", "{\"unexpected\":true}"));
    require_contains(fixture.sink.last(), "\"code\":\"INVALID_PARAMS\"");
    fixture.payload(request(3, session_c, "usb.detach", "{\"unexpected\":true}"));
    require_contains(fixture.sink.last(), "\"code\":\"INVALID_PARAMS\"");
    fixture.payload(request(4, session_c, "usb.exposure.status", "{\"unexpected\":true}"));
    require_contains(fixture.sink.last(), "\"code\":\"INVALID_PARAMS\"");
    assert(fixture.exposure.attach_calls == attach_calls_before_invalid);
    assert(fixture.exposure.detach_calls == detach_calls_before_invalid);
}

void test_usb_exposure_transition_response_is_frozen_before_provider_progress() {
    Fixture fixture;
    fixture.exposure.status = {
        .desired = control_protocol::UsbExposureDesired::kHidden,
        .observed = control_protocol::UsbExposureObserved::kDriverNotInstalled,
        .generation = 0,
        .mounted = false,
        .suspended = false,
        .keyboard_ready = false,
        .mouse_ready = false,
        .safety_pending = false,
        .host_release_uncertain = false,
        .recovery_required = false,
        .last_error = {},
    };
    fixture.payload(hello_request(1, kNonceA));
    const std::string attach_session = extract_string(fixture.sink.last(), "session");
    fixture.exposure.advance_attach_before_return = true;
    const std::string attach = request(2, attach_session, "usb.attach");
    fixture.payload(attach);
    const std::string accepted_attach = fixture.sink.last();
    require_contains(accepted_attach,
                     "\"desired\":\"exposed\",\"observed\":\"attaching\",\"generation\":1,"
                     "\"mounted\":false,\"suspended\":false,\"keyboard_ready\":false,"
                     "\"mouse_ready\":false");
    assert(fixture.exposure.status.observed == control_protocol::UsbExposureObserved::kMounted);
    assert(fixture.exposure.status.mounted);
    fixture.payload(attach);
    assert(fixture.sink.last() == accepted_attach);
    assert(fixture.exposure.attach_calls == 1);

    fixture.payload(hello_request(3, kNonceB));
    const std::string detach_session = extract_string(fixture.sink.last(), "session");
    fixture.exposure.advance_detach_before_return = true;
    const std::string detach = request(4, detach_session, "usb.detach");
    fixture.payload(detach);
    const std::string accepted_detach = fixture.sink.last();
    require_contains(accepted_detach,
                     "\"desired\":\"hidden\",\"observed\":\"detaching\",\"generation\":1,"
                     "\"mounted\":true,\"suspended\":false,\"keyboard_ready\":true,"
                     "\"mouse_ready\":true,\"safety_pending\":true");
    assert(fixture.exposure.status.observed ==
           control_protocol::UsbExposureObserved::kDriverNotInstalled);
    assert(fixture.exposure.status.generation == 2);
    fixture.payload(detach);
    assert(fixture.sink.last() == accepted_detach);
    assert(fixture.exposure.detach_calls == 1);
}

void test_lease_refresh_expiry_and_takeover() {
    LeaseFixture fixture;
    fixture.payload(hello_request(1, kNonceA));
    assert(fixture.expired_callbacks == 0);
    assert(fixture.takeover_callbacks == 0);
    assert(!fixture.runtime.release_requested_for_test());
    assert(!fixture.runtime.usb_lifecycle_snapshot().safety_pending);
    const std::string first = fixture.sink.last();
    const std::string first_session = extract_string(first, "session");
    require_contains(first, "\"lease_ms\":5000");
    require_contains(first, "\"hid.lease-v1\"");

    fixture.clock.value = control_session::kLeaseMicroseconds - 1;
    fixture.protocol.service();
    assert(fixture.expired_callbacks == 0);
    fixture.payload(request(2, first_session, "system.ping"));
    assert(fixture.sink.last().find("\"pong\":true") != std::string::npos);

    fixture.clock.value += control_session::kLeaseMicroseconds - 1;
    fixture.protocol.service();
    assert(fixture.expired_callbacks == 0);
    fixture.clock.value += 1;
    fixture.protocol.service();
    assert(fixture.expired_callbacks == 1);
    assert(!fixture.runtime.release_requested_for_test());
    assert(!fixture.runtime.usb_lifecycle_snapshot().safety_pending);
    assert(!fixture.runtime.usb_lifecycle_snapshot().host_release_uncertain);
    fixture.payload(request(3, first_session, "system.ping"));
    require_contains(fixture.sink.last(), "\"code\":\"SESSION_MISMATCH\"");

    fixture.payload(hello_request(4, kNonceB));
    const std::string second = fixture.sink.last();
    const std::string second_session = extract_string(second, "session");
    assert(second_session != first_session);
    fixture.payload(hello_request(5, kNonceA));
    assert(fixture.takeover_callbacks == 1);
    assert(!fixture.runtime.release_requested_for_test());
    const auto after_takeover = fixture.runtime.usb_lifecycle_snapshot();
    assert(after_takeover.desired == usb_lifecycle::DesiredExposure::kHidden);
    assert(after_takeover.observed == usb_lifecycle::ObservedState::kDriverNotInstalled);
    assert(after_takeover.generation == 0);
    assert(!after_takeover.safety_pending && !after_takeover.host_release_uncertain);
    const std::string third = fixture.sink.last();
    fixture.payload(hello_request(5, kNonceA));
    assert(fixture.sink.last() == third);
    assert(fixture.takeover_callbacks == 1);
}

void test_hidden_provisioning_like_two_client_sessions_stay_clean() {
    {
        LeaseFixture fixture;
        fixture.payload(hello_request(1, kNonceA));
        const std::string session = extract_string(fixture.sink.last(), "session");
        fixture.payload(request(2, session, "system.info"));
        fixture.payload(hello_request(3, kNonceB));
        assert(fixture.takeover_callbacks == 1);
        assert(fixture.expired_callbacks == 0);
        assert(!fixture.runtime.release_requested_for_test());
        assert(!fixture.runtime.usb_lifecycle_snapshot().safety_pending);
        const std::string new_session = extract_string(fixture.sink.last(), "session");
        fixture.payload(request(4, new_session, "usb.exposure.status"));
        require_contains(fixture.sink.last(),
                         "\"desired\":\"hidden\",\"observed\":\"driver_not_installed\"");
        require_contains(fixture.sink.last(), "\"safety_pending\":false");
    }

    {
        LeaseFixture fixture;
        fixture.payload(hello_request(1, kNonceA));
        const std::string session = extract_string(fixture.sink.last(), "session");
        fixture.payload(request(2, session, "system.info"));
        fixture.clock.value = control_session::kLeaseMicroseconds;
        fixture.protocol.service();
        assert(fixture.expired_callbacks == 1);
        assert(!fixture.runtime.release_requested_for_test());
        assert(!fixture.runtime.usb_lifecycle_snapshot().safety_pending);

        fixture.payload(hello_request(3, kNonceB));
        assert(fixture.takeover_callbacks == 0);
        assert(fixture.sink.last().find("\"ok\":true") != std::string::npos);
        assert(!fixture.runtime.usb_lifecycle_snapshot().safety_pending);
        const std::string new_session = extract_string(fixture.sink.last(), "session");
        fixture.payload(request(4, new_session, "usb.exposure.status"));
        require_contains(fixture.sink.last(), "\"safety_pending\":false");
    }
}

void test_hid_failure_revokes_authority() {
    LeaseFixture fixture;
    fixture.payload(hello_request(1, kNonceA));
    const std::string session = extract_string(fixture.sink.last(), "session");

    fixture.protocol.on_hid_safety_failure();
    assert(fixture.hid_failure_callbacks == 1);
    fixture.payload(request(2, session, "system.ping"));
    require_contains(fixture.sink.last(), "\"code\":\"SESSION_MISMATCH\"");

    fixture.payload(hello_request(3, kNonceB));
    assert(fixture.sink.last().find("\"ok\":true") != std::string::npos);
}

void test_authority_epoch_barrier_and_retry_scoping() {
    LeaseFixture fixture;
    fixture.payload(hello_request(1, kNonceA));
    const std::string session_a = extract_string(fixture.sink.last(), "session");
    const std::string ping = request(2, session_a, "system.ping");

    fixture.payload(ping);
    const std::string same_epoch_response = fixture.sink.last();
    fixture.payload(ping);
    assert(fixture.sink.last() == same_epoch_response);

    // The normal exact-retry cache is checked only after the current
    // authority epoch validates the session.
    ++fixture.authority.epoch;
    fixture.payload(ping);
    require_contains(fixture.sink.last(), "\"code\":\"SESSION_MISMATCH\"");
    require_top_level_session(fixture.sink.last(), {});

    // Exact hello retries are stable in one epoch. Across an epoch transition
    // the same nonce and bytes create a fresh session rather than replaying A.
    fixture.payload(hello_request(1, kNonceA));
    const std::string hello_b = fixture.sink.last();
    const std::string session_b = extract_string(hello_b, "session");
    assert(session_b != session_a);
    fixture.payload(hello_request(1, kNonceA));
    assert(fixture.sink.last() == hello_b);

    // A hello accepted while the link is suspended is current only until the
    // resume lifecycle publication advances the injected authority epoch.
    ++fixture.authority.epoch;
    fixture.payload(hello_request(3, kNonceB));
    const std::string suspended_session = extract_string(fixture.sink.last(), "session");
    ++fixture.authority.epoch;
    fixture.payload(request(4, suspended_session, "usb.status"));
    require_contains(fixture.sink.last(), "\"code\":\"SESSION_MISMATCH\"");
    fixture.payload(hello_request(3, kNonceB));
    assert(extract_string(fixture.sink.last(), "session") != suspended_session);
}

void test_same_rx_batch_observes_published_epoch() {
    Fixture fixture;
    fixture.payload(hello_request(1, kNonceA));
    const std::string session = extract_string(fixture.sink.last(), "session");

    // The write for frame A models lifecycle publication before frame B is
    // dispatched from the same framing batch. Frame B must not inherit A's
    // old session or exact-cache authority.
    fixture.sink.after_write = increment_authority;
    fixture.sink.after_write_context = &fixture.authority;
    control_framing::Transport transport;
    feed_wire(&transport,
              &fixture.protocol,
              std::string(control_framing::kFramePrefix) +
                  request(2, session, "system.ping") + "\n" +
                  std::string(control_framing::kFramePrefix) +
                  request(3, session, "system.ping") + "\n");
    assert(fixture.sink.frames.size() >= 3);
    require_contains(fixture.sink.frames[fixture.sink.frames.size() - 2], "\"pong\":true");
    require_contains(fixture.sink.last(), "\"code\":\"SESSION_MISMATCH\"");
}

void test_release_all_result_cache_and_pending_error() {
    Fixture fixture;
    fixture.payload(hello_request(1, kNonceA));
    const std::string session = extract_string(fixture.sink.last(), "session");

    fixture.release.result = control_protocol::ReleaseAllResult{
        .success = true,
        .authority_lost = false,
        .keyboard = control_protocol::ReleaseAllInterfaceState::kAlreadyUp,
        .mouse = control_protocol::ReleaseAllInterfaceState::kSubmitted,
    };
    const std::string release = request(1, session, "hid.release_all");
    fixture.payload(release);
    const std::string success = fixture.sink.last();
    require_contains(success, "\"keyboard\":\"already_up\"");
    require_contains(success, "\"mouse\":\"submitted\"");
    assert(fixture.release.calls == 1);
    fixture.payload(release);
    assert(fixture.sink.last() == success);
    assert(fixture.release.calls == 1);

    fixture.release.result.success = false;
    fixture.payload(request(2, session, "hid.release_all"));
    const std::string pending = fixture.sink.last();
    require_contains(pending, "\"code\":\"HID_SAFETY_PENDING\"");
    assert(pending.find("\"result\":") == std::string::npos);
    assert(fixture.release.calls == 2);
    fixture.payload(request(2, session, "hid.release_all"));
    assert(fixture.sink.last() == pending);
    assert(fixture.release.calls == 2);
}

void test_keyboard_report_schema_result_and_cache() {
    Fixture fixture;
    fixture.payload(hello_request(1, kNonceA));
    const std::string session = extract_string(fixture.sink.last(), "session");
    require_contains(fixture.sink.last(), "hid.keyboard-report-v1");

    const std::string valid = request(
        2, session, "hid.keyboard.report", "{\"modifiers\":2,\"keys\":[4,5,164,176,221]}");
    fixture.payload(valid);
    const std::string success = fixture.sink.last();
    require_contains(success, "\"state\":\"submitted\"");
    assert(fixture.keyboard.calls == 1);
    assert(fixture.keyboard.request.modifiers == 2);
    assert(fixture.keyboard.request.keycodes[0] == 4 && fixture.keyboard.request.keycodes[4] == 221);
    fixture.payload(valid);
    assert(fixture.sink.last() == success);
    assert(fixture.keyboard.calls == 1);

    const char *invalid[] = {
        "{}", "{\"modifiers\":true,\"keys\":[]}",
        "{\"modifiers\":0.5,\"keys\":[]}",
        "{\"modifiers\":0,\"keys\":[4,4]}",
        "{\"modifiers\":0,\"keys\":[5,4]}",
        "{\"modifiers\":0,\"keys\":[0]}",
        "{\"modifiers\":0,\"keys\":[3]}",
        "{\"modifiers\":0,\"keys\":[165]}",
        "{\"modifiers\":0,\"keys\":[222]}",
        "{\"modifiers\":0,\"keys\":[224]}",
        "{\"modifiers\":0,\"keys\":[4,5,6,7,8,9,10]}",
        "{\"modifiers\":0,\"keys\":[4],\"extra\":1}",
    };
    for (const char *params : invalid) {
        fixture.payload(request(3, session, "hid.keyboard.report", params));
        require_contains(fixture.sink.last(), "\"code\":\"INVALID_PARAMS\"");
    }
    // Invalid requests do not consume the provider or cache a runtime result.
    assert(fixture.keyboard.calls == 1);
}

void test_mouse_report_schema_result_and_cache() {
    Fixture fixture;
    fixture.payload(hello_request(1, kNonceA));
    const std::string session = extract_string(fixture.sink.last(), "session");
    require_contains(fixture.sink.last(), "hid.mouse-report-v1");

    const std::string valid = request(
        2, session, "hid.mouse.report",
        "{\"buttons\":3,\"x\":1,\"y\":-2,\"wheel\":0,\"pan\":4}");
    fixture.payload(valid);
    const std::string success = fixture.sink.last();
    require_contains(success, "\"state\":\"submitted\"");
    assert(fixture.mouse.calls == 1);
    assert(fixture.mouse.request.buttons == 3 && fixture.mouse.request.x == 1 &&
           fixture.mouse.request.y == -2 && fixture.mouse.request.wheel == 0 &&
           fixture.mouse.request.pan == 4);
    fixture.payload(valid);
    assert(fixture.sink.last() == success);
    assert(fixture.mouse.calls == 1);

    const char *valid_edges[] = {
        "{\"buttons\":0,\"x\":-127,\"y\":127,\"wheel\":-127,\"pan\":127}",
        "{\"buttons\":31,\"x\":127,\"y\":-127,\"wheel\":127,\"pan\":-127}",
    };
    int edge_id = 20;
    for (const char *params : valid_edges) {
        fixture.payload(request(edge_id++, session, "hid.mouse.report", params));
        require_contains(fixture.sink.last(), "\"state\":\"submitted\"");
    }
    assert(fixture.mouse.calls == 3);

    const char *invalid[] = {
        "{}",
        "{\"buttons\":true,\"x\":0,\"y\":0,\"wheel\":0,\"pan\":0}",
        "{\"buttons\":-1,\"x\":0,\"y\":0,\"wheel\":0,\"pan\":0}",
        "{\"buttons\":32,\"x\":0,\"y\":0,\"wheel\":0,\"pan\":0}",
        "{\"buttons\":0,\"x\":-128,\"y\":0,\"wheel\":0,\"pan\":0}",
        "{\"buttons\":0,\"x\":128,\"y\":0,\"wheel\":0,\"pan\":0}",
        "{\"buttons\":0,\"x\":0.5,\"y\":0,\"wheel\":0,\"pan\":0}",
        "{\"buttons\":0,\"x\":0,\"y\":-128,\"wheel\":0,\"pan\":0}",
        "{\"buttons\":0,\"x\":0,\"y\":128,\"wheel\":0,\"pan\":0}",
        "{\"buttons\":0,\"x\":0,\"y\":0.5,\"wheel\":0,\"pan\":0}",
        "{\"buttons\":0,\"x\":0,\"y\":true,\"wheel\":0,\"pan\":0}",
        "{\"buttons\":0,\"x\":0,\"y\":0,\"wheel\":-128,\"pan\":0}",
        "{\"buttons\":0,\"x\":0,\"y\":0,\"wheel\":128,\"pan\":0}",
        "{\"buttons\":0,\"x\":0,\"y\":0,\"wheel\":true,\"pan\":0}",
        "{\"buttons\":0,\"x\":0,\"y\":0,\"wheel\":0.5,\"pan\":0}",
        "{\"buttons\":0,\"x\":0,\"y\":0,\"wheel\":0,\"pan\":-128}",
        "{\"buttons\":0,\"x\":0,\"y\":0,\"wheel\":0,\"pan\":128}",
        "{\"buttons\":0,\"x\":0,\"y\":0,\"wheel\":0,\"pan\":0.5}",
        "{\"buttons\":0,\"x\":0,\"y\":0,\"wheel\":0,\"pan\":true}",
        "{\"buttons\":0,\"x\":0,\"y\":0,\"wheel\":0}",
        "{\"buttons\":0,\"x\":0,\"y\":0,\"wheel\":0,\"pan\":0,\"extra\":1}",
    };
    int invalid_id = 22;
    for (const char *params : invalid) {
        fixture.payload(request(invalid_id++, session, "hid.mouse.report", params));
        require_contains(fixture.sink.last(), "\"code\":\"INVALID_PARAMS\"");
    }
    assert(fixture.mouse.calls == 3);
}

void test_hid_route_schema_frozen_retry_and_errors() {
    Fixture fixture(0, true);
    fixture.payload(hello_request(1, kNonceA));
    const std::string hello = fixture.sink.last();
    const std::string session = extract_string(hello, "session");
    assert(count_occurrences(hello, "\"hid.output-route-v1\"") == 1);
    assert(count_occurrences(hello, "\"hid.output-route-v2\"") == 1);
    assert(count_occurrences(hello, "-v1\"") == 14);
    assert(hello.size() <= kMaxLogicalMachineFrameBytes);

    fixture.payload(request(2, session, "hid.route.status"));
    const std::string boot = fixture.sink.last();
    require_contains(boot, "\"desired\":\"none\"");
    require_contains(boot, "\"active\":\"none\"");
    require_contains(boot, "\"generation\":0");
    require_contains(boot, "\"transition\":\"stable\"");
    require_contains(boot, "\"ready\":false");
    assert(boot.find("\"mounted\":") == std::string::npos);
    assert(boot.find("\"safety_pending\":") == std::string::npos);

    int id = 3;
    for (const char *params : {
             "{}", "{\"route\":null}", "{\"route\":1}",
             "{\"route\":\"ble\"}", "{\"route\":\"USB\"}",
             "{\"route\":\"usb\",\"extra\":true}",
         }) {
        fixture.payload(request(id++, session, "hid.route.set", params));
        require_contains(fixture.sink.last(), "\"code\":\"INVALID_PARAMS\"");
    }
    assert(fixture.route.set_calls == 0);

    const std::string select = request(id++, session, "hid.route.set",
                                       "{\"route\":\"usb\"}");
    fixture.payload(select);
    const std::string accepted_usb = fixture.sink.last();
    require_contains(accepted_usb, "\"desired\":\"usb\"");
    require_contains(accepted_usb, "\"active\":\"usb\"");
    require_contains(accepted_usb, "\"generation\":1");
    require_contains(accepted_usb, "\"transition\":\"stable\"");
    require_contains(accepted_usb, "\"ready\":true");
    assert(fixture.route.set_calls == 1);
    fixture.payload(select);
    assert(fixture.sink.last() == accepted_usb);
    assert(fixture.route.set_calls == 1);
    fixture.payload(request(id, session, "hid.route.set", "{\"route\":\"none\"}"));
    require_contains(fixture.sink.last(), "\"code\":\"SESSION_MISMATCH\"");

    fixture.payload(hello_request(1, kNonceB));
    const std::string second_session = extract_string(fixture.sink.last(), "session");
    fixture.route.finish_release_before_return = true;
    const std::string release = request(1, second_session, "hid.route.set",
                                        "{\"route\":\"none\"}");
    fixture.payload(release);
    const std::string accepted_release = fixture.sink.last();
    require_contains(accepted_release, "\"desired\":\"none\"");
    require_contains(accepted_release, "\"active\":\"usb\"");
    require_contains(accepted_release, "\"generation\":1");
    require_contains(accepted_release, "\"transition\":\"releasing\"");
    require_contains(accepted_release, "\"ready\":false");
    assert(fixture.route.status.active == control_protocol::OutputRoute::kNone);
    assert(fixture.route.status.generation == 2);
    fixture.payload(release);
    assert(fixture.sink.last() == accepted_release);
    assert(fixture.route.set_calls == 2);

    Fixture noop;
    noop.route.result = control_protocol::HidRouteActionResult::kNoOp;
    noop.payload(hello_request(1, kNonceA));
    const std::string noop_session = extract_string(noop.sink.last(), "session");
    const std::string noop_request = request(2, noop_session, "hid.route.set",
                                             "{\"route\":\"none\"}");
    noop.payload(noop_request);
    const std::string noop_response = noop.sink.last();
    noop.payload(noop_request);
    assert(noop.sink.last() == noop_response);
    assert(noop.route.set_calls == 1);
    noop.payload(request(3, noop_session, "system.ping"));
    require_contains(noop.sink.last(), "\"pong\":true");

    for (const auto &[result, code] : {
             std::pair{control_protocol::HidRouteActionResult::kBusy, "HID_BUSY"},
             std::pair{control_protocol::HidRouteActionResult::kNotReady, "HID_NOT_READY"},
             std::pair{control_protocol::HidRouteActionResult::kSafetyPending,
                       "HID_SAFETY_PENDING"},
         }) {
        Fixture failure;
        failure.route.result = result;
        failure.payload(hello_request(1, kNonceA));
        const std::string failure_session = extract_string(failure.sink.last(), "session");
        failure.payload(request(2, failure_session, "hid.route.set",
                                "{\"route\":\"usb\"}"));
        require_contains(failure.sink.last(), std::string("\"code\":\"") + code + "\"");
    }
}

void test_hid_route_v2_schema_retry_session_and_v1_ble_compatibility() {
    Fixture fixture(0, true);
    fixture.payload(hello_request(1, kNonceA));
    const std::string session = extract_string(fixture.sink.last(), "session");

    fixture.payload(request(2, session, "hid.route.v2.status"));
    require_contains(
        fixture.sink.last(),
        "\"result\":{\"desired\":\"none\",\"active\":\"none\","
        "\"generation\":0,\"transition\":\"stable\",\"ready\":false}");

    for (const char *params : {
             "{}", "{\"route\":null}", "{\"route\":1}",
             "{\"route\":\"wireless\"}", "{\"route\":\"BLE\"}",
             "{\"route\":\"ble\",\"extra\":true}",
         }) {
        fixture.payload(request(3, session, "hid.route.v2.set", params));
        require_contains(fixture.sink.last(), "\"code\":\"INVALID_PARAMS\"");
    }
    assert(fixture.route.set_calls == 0);

    const std::string select_ble = request(
        3, session, "hid.route.v2.set", "{\"route\":\"ble\"}");
    fixture.payload(select_ble);
    const std::string accepted_ble = fixture.sink.last();
    require_contains(
        accepted_ble,
        "\"result\":{\"desired\":\"ble\",\"active\":\"ble\","
        "\"generation\":1,\"transition\":\"stable\",\"ready\":true}");
    assert(fixture.route.set_calls == 1);
    fixture.payload(select_ble);
    assert(fixture.sink.last() == accepted_ble);
    assert(fixture.route.set_calls == 1);

    fixture.payload(hello_request(1, kNonceB));
    const std::string ble_session = extract_string(fixture.sink.last(), "session");
    fixture.payload(request(1, ble_session, "hid.route.status"));
    require_contains(fixture.sink.last(), "\"code\":\"HID_ROUTE_V2_REQUIRED\"");

    fixture.route.result = control_protocol::HidRouteActionResult::kBusy;
    fixture.payload(request(2, ble_session, "hid.route.set",
                            "{\"route\":\"usb\"}"));
    require_contains(fixture.sink.last(), "\"code\":\"HID_BUSY\"");
    assert(fixture.route.status.active == control_protocol::OutputRoute::kBle);

    fixture.route.result = control_protocol::HidRouteActionResult::kAccepted;
    const std::string v1_retire = request(
        3, ble_session, "hid.route.set", "{\"route\":\"none\"}");
    fixture.payload(v1_retire);
    const std::string retirement_response = fixture.sink.last();
    require_contains(retirement_response,
                     "\"code\":\"HID_ROUTE_V2_REQUIRED\"");
    assert(fixture.route.status.desired == control_protocol::OutputRoute::kNone);
    assert(fixture.route.status.active == control_protocol::OutputRoute::kBle);
    assert(fixture.route.status.transition ==
           control_protocol::RouteTransition::kReleasing);
    fixture.payload(v1_retire);
    assert(fixture.sink.last() == retirement_response);

    Fixture noop;
    noop.route.result = control_protocol::HidRouteActionResult::kNoOp;
    noop.route.status.desired = control_protocol::OutputRoute::kBle;
    noop.route.status.active = control_protocol::OutputRoute::kBle;
    noop.route.status.ready = true;
    noop.payload(hello_request(1, kNonceA));
    const std::string noop_session = extract_string(noop.sink.last(), "session");
    const std::string noop_request = request(
        2, noop_session, "hid.route.v2.set", "{\"route\":\"ble\"}");
    noop.payload(noop_request);
    const std::string noop_response = noop.sink.last();
    noop.payload(noop_request);
    assert(noop.sink.last() == noop_response);
    assert(noop.route.set_calls == 1);
    noop.payload(request(2, noop_session, "hid.route.v2.set",
                         "{\"route\":\"none\"}"));
    require_contains(noop.sink.last(), "\"code\":\"REQUEST_ID_CONFLICT\"");
    noop.payload(request(3, noop_session, "system.ping"));
    require_contains(noop.sink.last(), "\"pong\":true");

    Fixture failure;
    failure.route.result = control_protocol::HidRouteActionResult::kNotReady;
    failure.payload(hello_request(1, kNonceA));
    const std::string failure_session =
        extract_string(failure.sink.last(), "session");
    failure.payload(request(2, failure_session, "hid.route.v2.set",
                            "{\"route\":\"ble\"}"));
    require_contains(failure.sink.last(), "\"code\":\"HID_NOT_READY\"");
    failure.payload(request(3, failure_session, "system.ping"));
    require_contains(failure.sink.last(), "\"pong\":true");
}

void test_ble_exposure_schema_frozen_retry_and_authority_isolation() {
    Fixture fixture(0, true);
    fixture.payload(hello_request(1, kNonceA));
    const std::string hello = fixture.sink.last();
    const std::string session = extract_string(hello, "session");
    assert(hello.size() <= kMaxLogicalMachineFrameBytes);
    assert(count_occurrences(hello, "ble.exposure-control-v1") == 1);
    assert(count_occurrences(hello, "-v1\"") == 14);

    fixture.payload(request(2, session, "ble.exposure.status"));
    const std::string cold = fixture.sink.last();
    require_contains(cold,
        "\"result\":{\"desired\":\"hidden\",\"observed\":\"uninitialized\","
        "\"generation\":0,\"stack_ready\":false,\"advertising\":false,"
        "\"connected\":false,\"recovery_required\":false,\"last_error\":null}");
    assert(cold.find("address") == std::string::npos);
    assert(cold.find("passkey") == std::string::npos);
    assert(cold.find("bond") == std::string::npos);
    assert(cold.find("encrypted") == std::string::npos);

    fixture.ble.advance_before_return = true;
    const std::string enable_request = request(3, session, "ble.enable");
    const auto authority_before = fixture.authority.epoch;
    fixture.payload(enable_request);
    const std::string accepted = fixture.sink.last();
    require_contains(accepted, "\"desired\":\"exposed\",\"observed\":\"enabling\"");
    require_contains(accepted, "\"generation\":1,\"stack_ready\":false");
    assert(fixture.ble.status.observed ==
           control_protocol::BleExposureObserved::kAdvertising);
    assert(fixture.authority.epoch == authority_before);
    fixture.payload(enable_request);
    assert(fixture.sink.last() == accepted);
    assert(fixture.ble.enable_calls == 1);

    fixture.payload(request(4, session, "system.ping"));
    require_contains(fixture.sink.last(), "\"pong\":true");
    fixture.payload(request(5, session, "ble.exposure.status"));
    require_contains(fixture.sink.last(), "\"observed\":\"advertising\"");

    fixture.payload(request(6, session, "ble.disable", "{\"unexpected\":true}"));
    require_contains(fixture.sink.last(), "\"code\":\"INVALID_PARAMS\"");
    assert(fixture.ble.disable_calls == 0);
    const std::string disable_request = request(7, session, "ble.disable");
    fixture.payload(disable_request);
    const std::string disable_accepted = fixture.sink.last();
    require_contains(disable_accepted, "\"observed\":\"disabling\"");
    assert(fixture.authority.epoch == authority_before);
    assert(fixture.ble.disable_calls == 1);
    fixture.payload(disable_request);
    assert(fixture.sink.last() == disable_accepted);
    assert(fixture.ble.disable_calls == 1);
    fixture.payload(request(8, session, "system.ping"));
    require_contains(fixture.sink.last(), "\"pong\":true");

    Fixture busy;
    busy.payload(hello_request(1, kNonceA));
    const std::string busy_session = extract_string(busy.sink.last(), "session");
    busy.ble.enable_result = control_protocol::BleExposureActionResult::kBusy;
    busy.payload(request(2, busy_session, "ble.enable"));
    require_contains(busy.sink.last(), "\"code\":\"HID_BUSY\"");
    assert(busy.ble.status.generation == 0);
    busy.ble.disable_result = control_protocol::BleExposureActionResult::kBusy;
    busy.payload(request(3, busy_session, "ble.disable"));
    require_contains(busy.sink.last(), "\"code\":\"HID_BUSY\"");
    assert(busy.ble.disable_calls == 1);
}

void test_ble_pairing_status_exact_schema() {
    Fixture fixture(0, true);
    fixture.payload(hello_request(1, kNonceA));
    const std::string hello = fixture.sink.last();
    const std::string session = extract_string(hello, "session");
    assert(count_occurrences(hello, "ble.pairing-transaction-v1") == 1);
    assert(hello.find("ble.pairing-control-v1") == std::string::npos);
    assert(hello.find("ble.bond-store-v1") == std::string::npos);
    assert(count_occurrences(hello, "-v1\"") == 14);

    fixture.payload(request(2, session, "ble.pairing.status"));
    require_contains(
        fixture.sink.last(),
        "\"result\":{\"state\":\"idle\",\"generation\":0,\"connected\":false,"
        "\"pairing_id\":null,\"action\":null,\"remaining_ms\":null,"
        "\"encrypted\":false,\"authenticated\":false,\"bonded\":false,"
        "\"secure_connections\":false,\"key_size\":0,\"last_result\":\"none\"}");

    fixture.pairing.status = {
        .state = control_protocol::BlePairingState::kWaitingInput,
        .generation = 7,
        .connected = true,
        .input_pending = true,
        .pairing_id = 12,
        .remaining_ms = 24000,
        .last_result = control_protocol::BlePairingLastResult::kTimeout,
    };
    fixture.payload("{\"v\":1,\"id\":3,\"session\":\"" + session +
                    "\",\"cmd\":\"ble.pairing.status\"}");
    const std::string waiting = fixture.sink.last();
    require_contains(waiting, "\"state\":\"waiting_input\",\"generation\":7");
    require_contains(waiting, "\"pairing_id\":12,\"action\":\"passkey_input\",\"remaining_ms\":24000");
    require_contains(waiting, "\"last_result\":\"timeout\"");

    fixture.pairing.status = {
        .state = control_protocol::BlePairingState::kIdle,
        .generation = 8,
        .connected = true,
        .encrypted = true,
        .authenticated = true,
        .bonded = true,
        .secure_connections = true,
        .key_size = 16,
        .last_result = control_protocol::BlePairingLastResult::kSucceeded,
    };
    fixture.payload(request(4, session, "ble.pairing.status"));
    const std::string secured = fixture.sink.last();
    require_contains(secured, "\"pairing_id\":null,\"action\":null,\"remaining_ms\":null");
    require_contains(secured, "\"encrypted\":true,\"authenticated\":true,\"bonded\":true");
    require_contains(secured, "\"secure_connections\":true,\"key_size\":16,\"last_result\":\"succeeded\"");

    fixture.payload(request(5, session, "ble.pairing.status", "{\"extra\":1}"));
    require_contains(fixture.sink.last(), "\"code\":\"INVALID_PARAMS\"");

    fixture.pairing.status = {
        .state = control_protocol::BlePairingState::kSecuring,
        .generation = 9,
        .connected = true,
    };
    fixture.payload(request(6, session, "ble.pairing.status"));
    require_contains(fixture.sink.last(),
                     "\"state\":\"securing\",\"generation\":9,\"connected\":true");
    require_contains(fixture.sink.last(),
                     "\"pairing_id\":null,\"action\":null,\"remaining_ms\":null");

    const std::array<std::pair<control_protocol::BlePairingLastResult,
                               std::string_view>, 10>
        last_results{{
            {control_protocol::BlePairingLastResult::kNone, "none"},
            {control_protocol::BlePairingLastResult::kSucceeded, "succeeded"},
            {control_protocol::BlePairingLastResult::kSmpFailed, "smp_failed"},
            {control_protocol::BlePairingLastResult::kTimeout, "timeout"},
            {control_protocol::BlePairingLastResult::kPeerDisconnected,
             "peer_disconnected"},
            {control_protocol::BlePairingLastResult::kStoreFull, "store_full"},
            {control_protocol::BlePairingLastResult::kStorage, "storage"},
            {control_protocol::BlePairingLastResult::kQueueOverflow,
             "queue_overflow"},
            {control_protocol::BlePairingLastResult::kRepeatPairing,
             "repeat_pairing"},
            {control_protocol::BlePairingLastResult::kSecurityPolicy,
             "security_policy"},
        }};
    std::int32_t status_id = 10;
    for (const auto &[value, spelling] : last_results) {
        fixture.pairing.status = {
            .state = control_protocol::BlePairingState::kIdle,
            .last_result = value,
        };
        fixture.payload(request(status_id++, session, "ble.pairing.status"));
        require_contains(fixture.sink.last(),
                         "\"last_result\":\"" + std::string(spelling) + "\"");
    }

    fixture.pairing.status.available = false;
    fixture.payload(request(status_id, session, "ble.pairing.status"));
    require_contains(fixture.sink.last(), "\"code\":\"INTERNAL_ERROR\"");
}

void test_ble_pairing_respond_parser_retry_and_errors() {
    Fixture fixture(0, true);
    fixture.payload(hello_request(1, kNonceA));
    const std::string session = extract_string(fixture.sink.last(), "session");
    const std::string accepted_request = request(
        2, session, "ble.pairing.respond",
        "{\"pairing_id\":12,\"passkey\":\"000123\"}");
    fixture.payload(accepted_request);
    const std::string accepted = fixture.sink.last();
    require_contains(accepted, "\"result\":{\"accepted\":true,\"pairing_id\":12}");
    assert(fixture.pairing.respond_calls == 1);
    assert(fixture.pairing.request.pairing_id == 12);
    assert(fixture.pairing.request.passkey ==
           (std::array<char, 6>{'0', '0', '0', '1', '2', '3'}));
    assert(fixture.protocol.request_scratch_zero_for_test());
    const auto cache = fixture.protocol.request_cache_snapshot_for_test();
    assert(cache.valid && cache.sensitive && cache.id == 2);
    assert(cache.payload_length == accepted_request.size());
    assert(cache.raw_storage_zero);

    control_framing::Transport transport;
    feed_wire(&transport, &fixture.protocol,
              std::string(control_framing::kFramePrefix) + accepted_request + "\n");
    assert(fixture.sink.last() == accepted);
    assert(fixture.pairing.respond_calls == 1);
    assert(transport.storage_zero_for_test());
    assert(fixture.protocol.request_scratch_zero_for_test());
    fixture.payload(request(2, session, "ble.pairing.respond",
                            "{\"pairing_id\":12,\"passkey\":\"000124\"}"));
    require_contains(fixture.sink.last(), "\"code\":\"REQUEST_ID_CONFLICT\"");
    fixture.payload(request(2, session, "ble.pairing.respond",
                            "{\"pairing_id\":13,\"passkey\":\"000123\"}"));
    require_contains(fixture.sink.last(), "\"code\":\"REQUEST_ID_CONFLICT\"");
    fixture.payload("{\"v\":1,\"id\":2,\"session\":\"" + session +
                    "\", \"cmd\":\"ble.pairing.respond\",\"params\":{\"pairing_id\":12,\"passkey\":\"000123\"}}");
    require_contains(fixture.sink.last(), "\"code\":\"REQUEST_ID_CONFLICT\"");
    assert(fixture.pairing.respond_calls == 1);

    auto expect_invalid = [](std::string_view params) {
        Fixture invalid(0, true);
        invalid.payload(hello_request(1, kNonceA));
        const std::string invalid_session = extract_string(invalid.sink.last(), "session");
        invalid.payload(request(2, invalid_session, "ble.pairing.respond", params));
        require_contains(invalid.sink.last(), "\"code\":\"INVALID_PARAMS\"");
        assert(invalid.pairing.respond_calls == 0);
        assert(invalid.protocol.request_scratch_zero_for_test());
    };
    expect_invalid("{\"pairing_id\":1,\"passkey\":123456}");
    expect_invalid("{\"pairing_id\":1,\"passkey\":\"１２３４５６\"}");
    expect_invalid("{\"pairing_id\":1,\"passkey\":\" 12345\"}");
    expect_invalid("{\"pairing_id\":1,\"passkey\":\"12345\"}");
    expect_invalid("{\"pairing_id\":1,\"passkey\":\"1234567\"}");
    expect_invalid("{\"pairing_id\":1,\"passkey\":\"12345\\n\"}");
    expect_invalid("{\"pairing_id\":1,\"passkey\":\"123456\",\"extra\":0}");
    expect_invalid("{\"pairing_id\":1}");
    expect_invalid("{\"pairing_id\":0,\"passkey\":\"123456\"}");
    expect_invalid("{\"pairing_id\":-1,\"passkey\":\"123456\"}");
    expect_invalid("{\"pairing_id\":1.5,\"passkey\":\"123456\"}");
    expect_invalid("{\"pairing_id\":\"1\",\"passkey\":\"123456\"}");
    expect_invalid("{\"pairing_id\":4294967296,\"passkey\":\"123456\"}");
    for (std::string_view passkey : {"000000", "999999"}) {
        Fixture valid(0, true);
        valid.payload(hello_request(1, kNonceA));
        const std::string valid_session = extract_string(valid.sink.last(), "session");
        valid.payload(request(2, valid_session, "ble.pairing.respond",
                              "{\"pairing_id\":4294967295,\"passkey\":\"" +
                                  std::string(passkey) + "\"}"));
        require_contains(valid.sink.last(), "\"accepted\":true");
    }

    for (const auto &[provider_result, code] : {
             std::pair{control_protocol::BlePairingRespondResult::kNotPending,
                       "BLE_PAIRING_NOT_PENDING"},
             std::pair{control_protocol::BlePairingRespondResult::kInjectionFailed,
                       "BLE_PAIRING_FAILED"},
         }) {
        Fixture failure(0, true);
        failure.pairing.respond_result = provider_result;
        failure.payload(hello_request(1, kNonceA));
        const std::string failure_session = extract_string(failure.sink.last(), "session");
        failure.payload(request(2, failure_session, "ble.pairing.respond",
                                "{\"pairing_id\":1,\"passkey\":\"123456\"}"));
        require_contains(failure.sink.last(), std::string("\"code\":\"") + code + "\"");
        assert(failure.sink.last().find("123456") == std::string::npos);
    }
}

void test_ble_bond_administration_schema_errors_and_retry() {
    {
        Fixture fixture(0, true);
        fixture.payload(hello_request(1, kNonceA));
        require_contains(fixture.sink.last(),
                         "\"ble.bond-administration-v1\"");
        const std::string session = extract_string(fixture.sink.last(), "session");

        fixture.payload(request(2, session, "ble.bond.list"));
        require_contains(
            fixture.sink.last(),
            "\"result\":{\"capacity\":3,\"count\":0,\"available\":3,"
            "\"healthy\":true,\"bonds\":[]}");
        assert(fixture.bonds.list_calls == 1);

        fixture.bonds.list_result.count = 3;
        fixture.bonds.list_result.available = 0;
        fixture.bonds.list_result.bonds[0] = {
            .bond_id = bond_id('1'), .our_sec = true, .peer_sec = true,
            .verified = true, .schema_revision_present = false};
        fixture.bonds.list_result.bonds[1] = {
            .bond_id = bond_id('2'), .our_sec = true, .peer_sec = true,
            .verified = true, .schema_revision_present = true,
            .schema_revision = 1, .schema_current = false};
        fixture.bonds.list_result.bonds[2] = {
            .bond_id = bond_id('f'), .our_sec = true, .peer_sec = true,
            .verified = true, .schema_revision_present = true,
            .schema_revision = 2, .schema_current = true};
        fixture.payload(request(3, session, "ble.bond.list"));
        const std::string three = fixture.sink.last();
        require_contains(three, "\"count\":3,\"available\":0,\"healthy\":true");
        require_contains(three, "\"schema_revision\":null");
        require_contains(three, "\"schema_revision\":2,\"schema_current\":true");
        assert(three.find(std::string(32, '1')) < three.find(std::string(32, '2')));
        assert(three.find(std::string(32, '2')) < three.find(std::string(32, 'f')));
        assert(three.find("ltk") == std::string::npos);
        assert(three.find("irk") == std::string::npos);
        assert(three.find("csrk") == std::string::npos);
        assert(three.size() <= kMaxLogicalMachineFrameBytes);
    }

    {
        Fixture fixture(0, true);
        fixture.payload(hello_request(1, kNonceA));
        const std::string session = extract_string(fixture.sink.last(), "session");
        fixture.bonds.remove_result = {
            .kind = control_protocol::BleBondRemoveResultKind::kSuccess,
            .remaining = 2};
        const std::string exact = request(
            2, session, "ble.bond.remove",
            "{\"bond_id\":\"0123456789abcdef0123456789abcdef\"}");
        fixture.payload(exact);
        const std::string accepted = fixture.sink.last();
        require_contains(accepted,
                         "\"bond_id\":\"0123456789abcdef0123456789abcdef\","
                         "\"removed\":true,\"remaining\":2");
        assert(fixture.bonds.remove_calls == 1);
        assert(std::strcmp(fixture.bonds.requested_id.data(),
                           "0123456789abcdef0123456789abcdef") == 0);
        fixture.payload(exact);
        assert(fixture.sink.last() == accepted);
        assert(fixture.bonds.remove_calls == 1);
        fixture.payload(request(
            2, session, "ble.bond.remove",
            "{\"bond_id\":\"1123456789abcdef0123456789abcdef\"}"));
        require_contains(fixture.sink.last(), "\"code\":\"REQUEST_ID_CONFLICT\"");
        assert(fixture.bonds.remove_calls == 1);
    }

    const auto expect_invalid = [](std::string_view params) {
        Fixture fixture(0, true);
        fixture.payload(hello_request(1, kNonceA));
        const std::string session = extract_string(fixture.sink.last(), "session");
        fixture.payload(request(2, session, "ble.bond.remove", params));
        require_contains(fixture.sink.last(), "\"code\":\"INVALID_PARAMS\"");
        assert(fixture.bonds.remove_calls == 0);
    };
    expect_invalid("{}");
    expect_invalid("{\"bond_id\":\"0123\"}");
    expect_invalid("{\"bond_id\":\"0123456789ABCDEF0123456789ABCDEF\"}");
    expect_invalid("{\"bond_id\":\"0123456789abcdef0123456789abcdef\",\"extra\":0}");
    expect_invalid("{\"bond_id\":1}");

    const std::array<std::pair<control_protocol::BleBondRemoveResultKind,
                               std::string_view>, 5> failures{{
        {control_protocol::BleBondRemoveResultKind::kNotReady, "BLE_NOT_READY"},
        {control_protocol::BleBondRemoveResultKind::kNotFound, "BLE_BOND_NOT_FOUND"},
        {control_protocol::BleBondRemoveResultKind::kAmbiguous, "BLE_BOND_AMBIGUOUS"},
        {control_protocol::BleBondRemoveResultKind::kBusy, "BLE_BOND_BUSY"},
        {control_protocol::BleBondRemoveResultKind::kStorageFailure,
         "BLE_BOND_STORAGE"},
    }};
    for (const auto &[kind, code] : failures) {
        Fixture fixture(0, true);
        fixture.bonds.remove_result.kind = kind;
        fixture.payload(hello_request(1, kNonceA));
        const std::string session = extract_string(fixture.sink.last(), "session");
        fixture.payload(request(
            2, session, "ble.bond.remove",
            "{\"bond_id\":\"0123456789abcdef0123456789abcdef\"}"));
        require_contains(fixture.sink.last(),
                         std::string("\"code\":\"") + std::string(code) + "\"");
        assert(fixture.bonds.remove_calls == 1);
    }

    for (const auto &[kind, code] : {
             std::pair{control_protocol::BleBondListResultKind::kNotReady,
                       "BLE_NOT_READY"},
             std::pair{control_protocol::BleBondListResultKind::kStorageFailure,
                       "BLE_BOND_STORAGE"},
         }) {
        Fixture fixture(0, true);
        fixture.bonds.list_result.kind = kind;
        fixture.payload(hello_request(1, kNonceA));
        const std::string session = extract_string(fixture.sink.last(), "session");
        fixture.payload(request(2, session, "ble.bond.list"));
        require_contains(fixture.sink.last(),
                         std::string("\"code\":\"") + code + "\"");
    }
}

void test_cjson_secret_is_wiped_before_free() {
    Fixture fixture(0, true);
    fixture.payload(hello_request(1, kNonceA));
    const std::string session = extract_string(fixture.sink.last(), "session");
    watch_parser_secret = true;
    parser_secret_present_at_free = false;
    fixture.payload(request(2, session, "ble.pairing.respond",
                            "{\"pairing_id\":1,\"passkey\":\"314159\"}"));
    watch_parser_secret = false;
    assert(!parser_secret_present_at_free);
    assert(fixture.pairing.respond_calls == 1);

    watch_embedded_nul_secret = true;
    embedded_nul_secret_present_at_free = false;
    fixture.payload(request(3, session, "ble.pairing.respond",
                            "{\"pairing_id\":1,\"passkey\":\"31\\u00004159\"}"));
    watch_embedded_nul_secret = false;
    require_contains(fixture.sink.last(), "\"code\":\"INVALID_PARAMS\"");
    assert(!embedded_nul_secret_present_at_free);
    assert(fixture.pairing.respond_calls == 1);
}

bool fail_secure_random(void *, std::uint8_t *, std::size_t) { return false; }

void test_pairing_rng_failure_is_startup_fail_closed() {
    Fixture fixture(0, true);
    control_protocol::Protocol rejected;
    assert(!rejected.initialize(fixture.configuration(), RandomSource::fill,
                                &fixture.random, fail_secure_random, nullptr,
                                deterministic_hmac, nullptr));
    assert(fixture.pairing.respond_calls == 0);
}

}  // namespace

int main() {
    cJSON_Hooks hooks{tracked_cjson_malloc, tracked_cjson_free};
    cJSON_InitHooks(&hooks);
    test_strict_envelope_and_framing();
    test_nonce_session_and_hello_cache();
    test_request_cache_and_commands();
    test_response_scratch_reuse();
    test_usb_exposure_commands_schema_and_lifecycle_retry_cache();
    test_usb_exposure_transition_response_is_frozen_before_provider_progress();
    test_stale_response_correlation();
    test_lease_refresh_expiry_and_takeover();
    test_hidden_provisioning_like_two_client_sessions_stay_clean();
    test_hid_failure_revokes_authority();
    test_authority_epoch_barrier_and_retry_scoping();
    test_same_rx_batch_observes_published_epoch();
    test_release_all_result_cache_and_pending_error();
    test_keyboard_report_schema_result_and_cache();
    test_mouse_report_schema_result_and_cache();
    test_hid_route_schema_frozen_retry_and_errors();
    test_hid_route_v2_schema_retry_session_and_v1_ble_compatibility();
    test_ble_exposure_schema_frozen_retry_and_authority_isolation();
    test_ble_pairing_status_exact_schema();
    test_ble_pairing_respond_parser_retry_and_errors();
    test_ble_bond_administration_schema_errors_and_retry();
    test_cjson_secret_is_wiped_before_free();
    test_pairing_rng_failure_is_startup_fail_closed();
    test_identity_hello_and_info_shapes();
    test_invalid_identity_rejected_at_protocol_initialization();
    return 0;
}
