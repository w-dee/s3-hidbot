#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string_view>
#include <vector>

#include "ble_hid_service/ble_hid_service.hpp"
#include "host/ble_att.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_hs_mbuf.h"
#include "os/os_mbuf.h"

static_assert(S3_HIDBOT_NIMBLE_FAKE_IDF_VERSION == 50504);
static_assert(BLE_GATT_ACCESS_OP_READ_CHR == 0);
static_assert(BLE_GATT_ACCESS_OP_WRITE_CHR == 1);
static_assert(BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN == 0x0d);
static_assert(BLE_ATT_ERR_UNLIKELY == 0x0e);
static_assert(BLE_ATT_ERR_INSUFFICIENT_RES == 0x11);
static_assert(BLE_GATT_CHR_F_WRITE_NO_RSP == 0x00000004);
static_assert(BLE_GATT_CHR_F_WRITE_AUTHEN == 0x00002000);

namespace {

constexpr std::uint16_t kControlPointUuid = 0x2a4c;
constexpr std::uint16_t kHidServiceUuid = 0x1812;
constexpr std::uint16_t kHidInformationUuid = 0x2a4a;
constexpr std::uint16_t kReportMapUuid = 0x2a4b;
constexpr std::uint16_t kReportUuid = 0x2a4d;
constexpr std::uint16_t kReportReferenceUuid = 0x2908;
constexpr std::uint16_t kAssignedControlPointHandle = 0x7a4c;
constexpr ble_lifecycle::Generation kDistinctiveGeneration = 0xa1b2c3d4U;
constexpr std::uint16_t kDistinctiveConnectionHandle = 0xbeef;

// Frozen strict expectations are intentionally independent of the production
// profile object and adapter aliases whose wiring this test exercises.
constexpr std::array<std::uint8_t, 116> kExpectedStrictReportMap{
    0x05,0x01,0x09,0x06,0xa1,0x01,0x85,0x01,0x05,0x07,0x19,0xe0,0x29,0xe7,
    0x15,0x00,0x25,0x01,0x75,0x01,0x95,0x08,0x81,0x02,0x95,0x01,0x75,0x08,
    0x81,0x01,0x95,0x06,0x75,0x08,0x15,0x00,0x26,0xff,0x00,0x19,0x00,0x2a,
    0xff,0x00,0x81,0x00,0xc0,
    0x05,0x01,0x09,0x02,0xa1,0x01,0x85,0x02,0x09,0x01,0xa1,0x00,0x05,0x09,
    0x19,0x01,0x29,0x05,0x15,0x00,0x25,0x01,0x95,0x05,0x75,0x01,0x81,0x02,
    0x95,0x01,0x75,0x03,0x81,0x01,0x05,0x01,0x09,0x30,0x09,0x31,0x09,0x38,
    0x15,0x81,0x25,0x7f,0x75,0x08,0x95,0x03,0x81,0x06,0x05,0x0c,0x0a,0x38,
    0x02,0x15,0x81,0x25,0x7f,0x75,0x08,0x95,0x01,0x81,0x06,0xc0,0xc0};
constexpr std::array<std::uint8_t, 69> kExpectedMouseReportMap{
    0x05,0x01,0x09,0x02,0xa1,0x01,0x85,0x02,0x09,0x01,0xa1,0x00,0x05,0x09,
    0x19,0x01,0x29,0x05,0x15,0x00,0x25,0x01,0x95,0x05,0x75,0x01,0x81,0x02,
    0x95,0x01,0x75,0x03,0x81,0x01,0x05,0x01,0x09,0x30,0x09,0x31,0x09,0x38,
    0x15,0x81,0x25,0x7f,0x75,0x08,0x95,0x03,0x81,0x06,0x05,0x0c,0x0a,0x38,
    0x02,0x15,0x81,0x25,0x7f,0x75,0x08,0x95,0x01,0x81,0x06,0xc0,0xc0};
constexpr std::array<std::uint8_t, 4> kExpectedHidInformation{
    0x11, 0x01, 0x00, 0x00};
constexpr std::array<std::uint8_t, 2> kExpectedKeyboardReference{0x01, 0x01};
constexpr std::array<std::uint8_t, 2> kExpectedMouseReference{0x02, 0x01};
constexpr std::array<std::uint8_t, 8> kExpectedNeutralKeyboard{};
constexpr std::array<std::uint8_t, 5> kExpectedNeutralMouse{};

const ble_gatt_svc_def *g_registered_services = nullptr;
int g_count_calls = 0;
int g_add_calls = 0;
bool g_topology_mode = false;
std::vector<std::uint8_t> g_appended_bytes;

bool uuid16_equals(const ble_uuid_t *uuid, std::uint16_t value) {
    return uuid != nullptr && uuid->type == BLE_UUID_TYPE_16 &&
           reinterpret_cast<const ble_uuid16_t *>(uuid)->value == value;
}
const ble_gatt_chr_def *find_characteristic(
    std::uint16_t service_uuid, std::uint16_t characteristic_uuid,
    std::size_t occurrence = 0) {
    if (g_registered_services == nullptr) {
        return nullptr;
    }
    for (const ble_gatt_svc_def *service = g_registered_services;
         service->type != BLE_GATT_SVC_TYPE_END; ++service) {
        if (!uuid16_equals(service->uuid, service_uuid) ||
            service->characteristics == nullptr) {
            continue;
        }
        for (const ble_gatt_chr_def *characteristic = service->characteristics;
            characteristic->uuid != nullptr; ++characteristic) {
            if (uuid16_equals(characteristic->uuid, characteristic_uuid)) {
                if (occurrence == 0) {
                    return characteristic;
                }
                --occurrence;
            }
        }
    }
    return nullptr;
}

const ble_gatt_dsc_def *find_descriptor(
    const ble_gatt_chr_def *characteristic, std::uint16_t descriptor_uuid) {
    if (characteristic == nullptr || characteristic->descriptors == nullptr) {
        return nullptr;
    }
    for (const ble_gatt_dsc_def *descriptor = characteristic->descriptors;
         descriptor->uuid != nullptr; ++descriptor) {
        if (uuid16_equals(descriptor->uuid, descriptor_uuid)) {
            return descriptor;
        }
    }
    return nullptr;
}

[[noreturn]] void fail(std::string_view case_name, std::string_view detail) {
    std::cerr << "FAIL: " << case_name << ": " << detail << '\n';
    std::exit(1);
}

void require(bool condition, std::string_view case_name,
             std::string_view detail) {
    if (!condition) {
        fail(case_name, detail);
    }
}

template <std::size_t Size>
void require_served_value(const ble_gatt_chr_def *characteristic,
                          const std::array<std::uint8_t, Size> &expected,
                          std::string_view case_name) {
    require(characteristic != nullptr, case_name,
            "registered characteristic not found");
    require(characteristic->access_cb == &ble_hid_service::Database::access &&
                characteristic->arg != nullptr &&
                characteristic->val_handle != nullptr,
            case_name, "characteristic does not use the production adapter");
    g_appended_bytes.clear();
    os_mbuf output{};
    ble_gatt_access_ctxt context{};
    context.op = BLE_GATT_ACCESS_OP_READ_CHR;
    context.om = &output;
    context.chr = characteristic;
    const int result = characteristic->access_cb(
        kDistinctiveConnectionHandle, *characteristic->val_handle, &context,
        characteristic->arg);
    require(result == 0, case_name, "production read callback rejected access");
    require(g_appended_bytes.size() == expected.size(), case_name,
            "production read callback served the wrong length");
    require(std::equal(g_appended_bytes.begin(), g_appended_bytes.end(),
                       expected.begin(), expected.end()),
            case_name, "production read callback served the wrong bytes");
}

template <std::size_t Size>
void require_served_value(const ble_gatt_dsc_def *descriptor,
                          const std::array<std::uint8_t, Size> &expected,
                          std::string_view case_name) {
    require(descriptor != nullptr, case_name,
            "registered descriptor not found");
    require(descriptor->access_cb == &ble_hid_service::Database::access &&
                descriptor->arg != nullptr,
            case_name, "descriptor does not use the production adapter");
    g_appended_bytes.clear();
    os_mbuf output{};
    ble_gatt_access_ctxt context{};
    context.op = BLE_GATT_ACCESS_OP_READ_DSC;
    context.om = &output;
    context.dsc = descriptor;
    const int result = descriptor->access_cb(
        kDistinctiveConnectionHandle, 0x7d5c, &context, descriptor->arg);
    require(result == 0, case_name, "production read callback rejected access");
    require(g_appended_bytes.size() == expected.size(), case_name,
            "production read callback served the wrong length");
    require(std::equal(g_appended_bytes.begin(), g_appended_bytes.end(),
                       expected.begin(), expected.end()),
            case_name, "production read callback served the wrong bytes");
}

class RecordingSink final : public hid_control_executor::BleEventSink {
  public:
    void retire_dle_on_reset(ble_lifecycle::Generation) override {}
    bool signal_ble_event(hid_control_executor::BleEvent event) override {
        ++call_count;
        last_event = event;
        return accept;
    }

    bool signal_ble_route_release_grace(
        hid_control_executor::BleRouteReleaseIdentity) override {
        fail("sink", "unexpected route-release signal");
    }

    void signal_ble_lifecycle_handoff_failure() override {
        fail("sink", "unexpected lifecycle handoff signal");
    }

    void reset(bool accept_event = true) {
        accept = accept_event;
        call_count = 0;
        last_event = {};
    }

    bool accept = true;
    int call_count = 0;
    hid_control_executor::BleEvent last_event{};
};

struct MbufFixture {
    explicit MbufFixture(std::uint8_t first_value) {
        first_data[0] = first_value;
        first.om_data = first_data.data();
        first.om_len = 1;
        first.test_packet_length = 1;
    }

    static MbufFixture empty() {
        MbufFixture fixture(0);
        fixture.first.om_len = 0;
        fixture.first.test_packet_length = 0;
        return fixture;
    }

    static MbufFixture two_bytes(std::uint8_t first_value,
                                 std::uint8_t second_value) {
        MbufFixture fixture(first_value);
        fixture.first_data[1] = second_value;
        fixture.first.om_len = 2;
        fixture.first.test_packet_length = 2;
        return fixture;
    }

    static MbufFixture zero_length_head_then_byte(std::uint8_t value) {
        MbufFixture fixture(0);
        fixture.second_data[0] = value;
        fixture.first.om_len = 0;
        fixture.first.om_next = &fixture.second;
        fixture.first.test_packet_length = 1;
        fixture.second.om_data = fixture.second_data.data();
        fixture.second.om_len = 1;
        return fixture;
    }

    MbufFixture(const MbufFixture &other)
        : first_data(other.first_data), second_data(other.second_data),
          first(other.first), second(other.second) {
        rebind(other);
    }

    MbufFixture &operator=(const MbufFixture &other) {
        first_data = other.first_data;
        second_data = other.second_data;
        first = other.first;
        second = other.second;
        rebind(other);
        return *this;
    }

    void rebind(const MbufFixture &other) {
        first.om_data = first_data.data();
        second.om_data = second_data.data();
        first.om_next = other.first.om_next == nullptr ? nullptr : &second;
    }

    std::array<std::uint8_t, 2> first_data{};
    std::array<std::uint8_t, 1> second_data{};
    os_mbuf first{};
    os_mbuf second{};
};

struct CallbackFixture {
    const ble_gatt_chr_def *characteristic;
    RecordingSink *sink;

    int invoke(MbufFixture &mbuf,
               std::uint8_t operation = BLE_GATT_ACCESS_OP_WRITE_CHR) const {
        ble_gatt_access_ctxt context{};
        context.op = operation;
        context.om = &mbuf.first;
        context.chr = characteristic;
        return characteristic->access_cb(kDistinctiveConnectionHandle,
                                         *characteristic->val_handle, &context,
                                         characteristic->arg);
    }

    void require_exact_event(std::string_view case_name, bool suspended) const {
        require(sink->call_count == 1, case_name, "expected exactly one event");
        const auto &event = sink->last_event;
        require(event.kind == hid_control_executor::BleEventKind::kControlPoint,
                case_name, "wrong event kind");
        require(event.generation == kDistinctiveGeneration, case_name,
                "generation was not preserved");
        require(event.connection_handle == kDistinctiveConnectionHandle,
                case_name, "connection handle was not preserved");
        require(event.attribute_handle == kAssignedControlPointHandle, case_name,
                "registered value handle was not preserved");
        require(event.suspended == suspended, case_name,
                "suspend state was not preserved");
    }

    void require_rejected_without_event(int result, int expected,
                                        std::string_view case_name) const {
        require(result == expected, case_name, "unexpected ATT result");
        require(sink->call_count == 0, case_name,
                "malformed input reached the event sink");
    }
};

}  // namespace

extern "C" int ble_gatts_count_cfg(const ble_gatt_svc_def *services) {
    ++g_count_calls;
    return services == nullptr ? BLE_HS_EINVAL : 0;
}

extern "C" int ble_gatts_add_svcs(const ble_gatt_svc_def *services) {
    ++g_add_calls;
    g_registered_services = services;
    std::uint16_t next_handle = 0x7001;
    if (g_topology_mode) {
        // Independent sequential ATT allocator: service, declaration, value,
        // automatic CCCD, then explicit descriptors (pinned NimBLE ordering).
        next_handle = 0x000e;
        for (const auto *service = services; service->type != BLE_GATT_SVC_TYPE_END;
             ++service) {
            ++next_handle;
            for (const auto *chr = service->characteristics; chr->uuid != nullptr; ++chr) {
                ++next_handle;
                *chr->val_handle = next_handle++;
                if ((chr->flags & BLE_GATT_CHR_F_NOTIFY) != 0) ++next_handle;
                for (const auto *dsc = chr->descriptors;
                     dsc != nullptr && dsc->uuid != nullptr; ++dsc) ++next_handle;
            }
        }
        return 0;
    }
    for (const ble_gatt_svc_def *service = services;
         service != nullptr && service->type != BLE_GATT_SVC_TYPE_END; ++service) {
        if (service->characteristics == nullptr) {
            continue;
        }
        for (const ble_gatt_chr_def *characteristic = service->characteristics;
             characteristic->uuid != nullptr; ++characteristic) {
            if (characteristic->val_handle != nullptr) {
                *characteristic->val_handle =
                    uuid16_equals(characteristic->uuid, kControlPointUuid)
                        ? kAssignedControlPointHandle
                        : next_handle++;
            }
        }
    }
    return 0;
}

extern "C" int ble_gatts_find_svc(const ble_uuid_t *uuid, std::uint16_t *handle) {
    if (!g_topology_mode) return BLE_HS_ENOENT;
    if (uuid16_equals(uuid, 0x1801)) *handle = 6;
    else if (uuid16_equals(uuid, kHidServiceUuid)) *handle = 0x0011;
    else if (uuid->type == BLE_UUID_TYPE_128) *handle = 0x000e;
    else return BLE_HS_ENOENT;
    return 0;
}

extern "C" int ble_gatts_find_chr(const ble_uuid_t *service_uuid,
                                   const ble_uuid_t *uuid,
                                   std::uint16_t *, std::uint16_t *handle) {
    if (!g_topology_mode) return BLE_HS_ENOENT;
    if (service_uuid->type == BLE_UUID_TYPE_128 && uuid->type == BLE_UUID_TYPE_128) {
        *handle = *g_registered_services[0].characteristics[0].val_handle;
        return 0;
    }
    if (!uuid16_equals(service_uuid, kHidServiceUuid) || uuid->type != BLE_UUID_TYPE_16)
        return BLE_HS_ENOENT;
    const auto *chr = find_characteristic(kHidServiceUuid,
        reinterpret_cast<const ble_uuid16_t *>(uuid)->value);
    if (chr == nullptr) return BLE_HS_ENOENT;
    *handle = *chr->val_handle;
    return 0;
}

extern "C" int ble_gatts_notify_custom(std::uint16_t, std::uint16_t,
                                        os_mbuf *) {
    return 0;
}

extern "C" os_mbuf *ble_hs_mbuf_from_flat(const void *, std::uint16_t) {
    return nullptr;
}

extern "C" int os_mbuf_append(os_mbuf *, const void *data,
                               std::uint16_t length) {
    const auto *bytes = static_cast<const std::uint8_t *>(data);
    g_appended_bytes.insert(g_appended_bytes.end(), bytes, bytes + length);
    return 0;
}

extern "C" int ble_hs_mbuf_to_flat(const os_mbuf *mbuf, void *flat,
                                     std::uint16_t maximum_length,
                                     std::uint16_t *copied_length) {
    if (mbuf == nullptr) {
        return BLE_HS_EINVAL;
    }
    if (mbuf->test_flatten_status != 0) {
        return mbuf->test_flatten_status;
    }

    const std::uint16_t copy_length =
        std::min<std::uint16_t>(OS_MBUF_PKTLEN(mbuf), maximum_length);
    auto *output = static_cast<std::uint8_t *>(flat);
    std::uint16_t copied = 0;
    for (const os_mbuf *fragment = mbuf;
         fragment != nullptr && copied < copy_length;
         fragment = fragment->om_next) {
        const std::uint16_t fragment_copy = std::min<std::uint16_t>(
            fragment->om_len,
            static_cast<std::uint16_t>(copy_length - copied));
        if (fragment_copy != 0) {
            std::memcpy(output + copied, fragment->om_data, fragment_copy);
            copied += fragment_copy;
        }
    }
    if (copied != copy_length) {
        return BLE_HS_EINVAL;
    }
    if (copied_length != nullptr) {
        *copied_length = mbuf->test_override_flatten_length
                             ? mbuf->test_flatten_length
                             : copy_length;
    }
    return OS_MBUF_PKTLEN(mbuf) > maximum_length ? BLE_HS_EMSGSIZE : 0;
}

int main() {
    ble_hid_service::Database database;
    RecordingSink sink;
    database.bind_event_sink(&sink);
    database.set_generation(kDistinctiveGeneration);

    require(database.register_database() == 0, "registration",
            "production service registration failed");
    require(g_count_calls == 1 && g_add_calls == 1, "registration",
            "production registration did not use the normal NimBLE path");

    const ble_gatt_chr_def *hid_information =
        find_characteristic(kHidServiceUuid, kHidInformationUuid);
    const ble_gatt_chr_def *report_map =
        find_characteristic(kHidServiceUuid, kReportMapUuid);
    const ble_gatt_chr_def *keyboard_report =
        find_characteristic(kHidServiceUuid, kReportUuid, 0);
    const ble_gatt_chr_def *mouse_report =
        find_characteristic(kHidServiceUuid, kReportUuid, 1);
    require_served_value(hid_information, kExpectedHidInformation,
                         "GATT HID Information");
    require_served_value(report_map, kExpectedStrictReportMap,
                         "GATT Report Map");
    require_served_value(keyboard_report, kExpectedNeutralKeyboard,
                         "GATT keyboard neutral report");
    require_served_value(mouse_report, kExpectedNeutralMouse,
                         "GATT mouse neutral report");
    require_served_value(
        find_descriptor(keyboard_report, kReportReferenceUuid),
        kExpectedKeyboardReference, "GATT keyboard Report Reference");
    require_served_value(find_descriptor(mouse_report, kReportReferenceUuid),
                         kExpectedMouseReference,
                         "GATT mouse Report Reference");
    std::cout << "PASS: production GATT-served strict HID values\n";

    const ble_gatt_chr_def *control_point =
        find_characteristic(kHidServiceUuid, kControlPointUuid);
    require(control_point != nullptr, "registration",
            "registered HID Control Point not found");
    require(control_point->access_cb == &ble_hid_service::Database::access,
            "registration", "unexpected production callback");
    require(control_point->arg != nullptr, "registration",
            "production callback argument was not captured");
    require(control_point->val_handle != nullptr &&
                *control_point->val_handle == kAssignedControlPointHandle,
            "registration", "fake registration did not assign the handle");
    require(control_point->flags ==
                (BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_WRITE_AUTHEN) &&
                control_point->min_key_size == 16,
            "registration", "Control Point registration contract changed");

    const CallbackFixture callback{control_point, &sink};

    sink.reset();
    auto cp1 = MbufFixture(0x00);
    require(callback.invoke(cp1) == 0, "CP1", "Suspend was rejected");
    callback.require_exact_event("CP1", true);
    std::cout << "PASS: CP1 Suspend\n";

    sink.reset();
    auto cp2 = MbufFixture(0x01);
    require(callback.invoke(cp2) == 0, "CP2", "Exit Suspend was rejected");
    callback.require_exact_event("CP2", false);
    std::cout << "PASS: CP2 Exit Suspend\n";

    sink.reset();
    auto cp3 = MbufFixture::empty();
    callback.require_rejected_without_event(
        callback.invoke(cp3), BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN, "CP3");
    std::cout << "PASS: CP3 empty payload\n";

    sink.reset();
    auto cp4 = MbufFixture::two_bytes(0x00, 0x01);
    callback.require_rejected_without_event(
        callback.invoke(cp4), BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN, "CP4");
    std::cout << "PASS: CP4 two-byte payload\n";

    sink.reset();
    auto cp5 = MbufFixture(0x02);
    callback.require_rejected_without_event(callback.invoke(cp5),
                                            BLE_ATT_ERR_UNLIKELY, "CP5");
    std::cout << "PASS: CP5 invalid 0x02\n";

    sink.reset();
    auto cp6 = MbufFixture(0xff);
    callback.require_rejected_without_event(callback.invoke(cp6),
                                            BLE_ATT_ERR_UNLIKELY, "CP6");
    std::cout << "PASS: CP6 invalid 0xff\n";

    sink.reset();
    auto cp7 = MbufFixture(0x00);
    callback.require_rejected_without_event(
        callback.invoke(cp7, BLE_GATT_ACCESS_OP_READ_CHR),
        BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN, "CP7");
    std::cout << "PASS: CP7 wrong GATT operation\n";

    sink.reset();
    auto cp8 = MbufFixture(0x00);
    cp8.first.test_flatten_status = BLE_HS_EINVAL;
    callback.require_rejected_without_event(callback.invoke(cp8),
                                            BLE_ATT_ERR_UNLIKELY, "CP8");
    std::cout << "PASS: CP8 flatten failure\n";

    sink.reset();
    auto cp9 = MbufFixture(0x00);
    cp9.first.test_override_flatten_length = true;
    cp9.first.test_flatten_length = 0;
    callback.require_rejected_without_event(callback.invoke(cp9),
                                            BLE_ATT_ERR_UNLIKELY, "CP9");
    std::cout << "PASS: CP9 flattened-length mismatch\n";

    sink.reset(false);
    auto cp10 = MbufFixture(0x01);
    require(callback.invoke(cp10) == BLE_ATT_ERR_INSUFFICIENT_RES, "CP10",
            "sink rejection reported false success");
    callback.require_exact_event("CP10", false);
    require(sink.call_count == 1, "CP10", "callback retried the rejected event");
    std::cout << "PASS: CP10 sink rejection\n";

    sink.reset();
    auto cp11 = MbufFixture(0x00);
    require(callback.invoke(cp11) == 0, "CP11", "valid metadata case rejected");
    callback.require_exact_event("CP11", true);
    std::cout << "PASS: CP11 distinctive metadata\n";

    sink.reset();
    // A one-byte payload cannot occupy two nonempty fragments. A valid
    // zero-length head followed by one nonempty byte still forces traversal.
    auto cp12 = MbufFixture::zero_length_head_then_byte(0x01);
    require(callback.invoke(cp12) == 0, "CP12", "chained payload was rejected");
    callback.require_exact_event("CP12", false);
    std::cout << "PASS: CP12 zero-length head plus one-byte fragment\n";

    using ble_fixture_profile::ProfileId;
    require(!database.configure_profile(ProfileId::kStandaloneMouseJustWorks),
            "mouse", "live database accepted mutation");
    // Model an external, proven full host teardown. Neither API can mutate
    // the live service; the executor is responsible for proving stop first.
    database.reset_after_stop();
    g_topology_mode = true;
    require(!database.configure_profile(static_cast<ProfileId>(255)),
            "mouse", "unknown finite profile accepted");
    require(database.configure_profile(ProfileId::kStandaloneMouseJustWorks),
            "mouse", "stopped database rejected mouse definition");
    require(database.register_database() == 0, "mouse", "registration failed");
    require(database.validate_registered_database() == 0,
            "mouse topology", "registered topology rejected");
    require(database.hid_handles().mouse_value == 0x0019,
            "mouse topology", "mouse expected value handle changed");
    require_served_value(g_registered_services[0].characteristics,
                         std::array<std::uint8_t, 1>{2}, "mouse schema epoch");
    require(database.hid_handles().keyboard_value == 0,
            "mouse", "stale keyboard handle survived teardown");
    require(find_characteristic(kHidServiceUuid, kReportUuid, 1) == nullptr,
            "mouse", "extra input characteristic/CCCD exposed");
    const auto *input = find_characteristic(kHidServiceUuid, kReportUuid);
    require(input != nullptr && input->flags ==
                (BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC |
                 BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_NOTIFY_INDICATE_ENC |
                 BLE_GATT_CHR_F_NOTIFY_INDICATE_AUTHOR) && input->min_key_size == 16,
            "mouse", "input ATT security is not encrypted/key-size 16");
    require_served_value(input, kExpectedNeutralMouse, "mouse neutral");
    require_served_value(find_descriptor(input, kReportReferenceUuid),
                         kExpectedMouseReference, "mouse reference");
    require_served_value(find_characteristic(kHidServiceUuid, kReportMapUuid),
                         kExpectedMouseReportMap, "mouse report map");
    const auto *mouse_control = find_characteristic(kHidServiceUuid, kControlPointUuid);
    require(mouse_control->flags ==
                (BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_WRITE_ENC) &&
                mouse_control->min_key_size == 16,
            "mouse", "Control Point accidentally requires authentication");
    require(database.notify_custom(1, 0, kExpectedNeutralKeyboard.data(), 8) ==
                hid_control_executor::BleNotifyBackendResult::kStackRejected,
            "mouse", "absent keyboard handle accepted");
    database.reset_after_stop();
    require(database.configure_profile(ProfileId::kStrictComposite),
            "strict restore", "strict selection failed");
    require(database.register_database() == 0, "strict restore", "registration failed");
    require_served_value(find_characteristic(kHidServiceUuid, kReportMapUuid),
                         kExpectedStrictReportMap, "strict restored map");
    require(find_characteristic(kHidServiceUuid, kReportUuid, 1) != nullptr,
            "strict restore", "composite topology not restored");
    require(database.validate_registered_database() == 0,
            "strict topology", "strict registered topology rejected");
    require_served_value(g_registered_services[0].characteristics,
                         std::array<std::uint8_t, 1>{1}, "strict schema epoch");
    const auto *restored_mouse = find_characteristic(kHidServiceUuid, kReportUuid, 1);
    *restored_mouse->val_handle = 0;
    require(database.validate_registered_database() != 0,
            "strict topology", "missing required mouse handle accepted");
    std::cout << "PASS: finite mouse GATT consumption and strict restoration\n";

    std::cout << "PASS: production BLE HID Control Point GATT boundary\n";
    return 0;
}
