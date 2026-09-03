#pragma once

#include <array>
#include <atomic>
#include <cstdint>

#include "hid_control_executor/hid_control_executor.hpp"

struct ble_gatt_access_ctxt;

namespace ble_hid_service {

// Bump only when bonded clients must refresh cache-relevant GATT/HID
// interpretation.  Missing per-peer metadata is legacy revision zero.
inline constexpr std::uint8_t kGattSchemaRevision = 1;
inline constexpr std::array<std::uint8_t, 1> kGattSchemaEpochValue{0x01};

// ESP-IDF v5.5.4 registers the configured GAP and GATT services at 0x0001
// through 0x000d.  Revision 1 consumes the old HID start with one three-
// attribute epoch service, then registers HID at a different start handle.
// Runtime validation below fails closed if the target database no longer
// matches this compile-time topology contract.  Published HID work continues
// to use the handles assigned by NimBLE, never these constants.
inline constexpr std::uint16_t kGattServiceStartHandle = 0x0006;
inline constexpr std::uint16_t kLegacyHidServiceStartHandle = 0x000e;
inline constexpr std::uint16_t kLegacyReportMapValueHandle = 0x0012;
inline constexpr std::uint16_t kLegacyKeyboardValueHandle = 0x0016;
inline constexpr std::uint16_t kLegacyMouseValueHandle = 0x001a;
inline constexpr std::uint16_t kRevision1EpochAttributeCount = 3;
inline constexpr std::uint16_t kRevision1EpochServiceStartHandle = 0x000e;
inline constexpr std::uint16_t kRevision1EpochServiceEndHandle = 0x0010;
inline constexpr std::uint16_t kRevision1HidServiceStartHandle = 0x0011;
inline constexpr std::uint16_t kRevision1ReportMapValueHandle = 0x0015;
inline constexpr std::uint16_t kRevision1ControlPointValueHandle = 0x0017;
inline constexpr std::uint16_t kRevision1KeyboardValueHandle = 0x0019;
inline constexpr std::uint16_t kRevision1MouseValueHandle = 0x001d;
inline constexpr std::uint16_t kRevision1HidLastAttributeHandle = 0x001f;

static_assert(kGattSchemaRevision == 1);
static_assert(kGattSchemaEpochValue.size() == 1);
static_assert(kGattSchemaEpochValue[0] == kGattSchemaRevision);
static_assert(kRevision1EpochServiceStartHandle ==
              kLegacyHidServiceStartHandle);
static_assert(kRevision1EpochServiceEndHandle ==
              kRevision1EpochServiceStartHandle +
                  kRevision1EpochAttributeCount - 1);
static_assert(kRevision1HidServiceStartHandle ==
              kLegacyHidServiceStartHandle +
                  kRevision1EpochAttributeCount);
static_assert(kRevision1ReportMapValueHandle ==
              kRevision1HidServiceStartHandle + 4);
static_assert(kRevision1ControlPointValueHandle ==
              kRevision1HidServiceStartHandle + 6);
static_assert(kRevision1KeyboardValueHandle ==
              kRevision1HidServiceStartHandle + 8);
static_assert(kRevision1MouseValueHandle ==
              kRevision1HidServiceStartHandle + 12);
static_assert(kRevision1HidLastAttributeHandle ==
              kRevision1HidServiceStartHandle + 14);
static_assert(kLegacyKeyboardValueHandle !=
                  kRevision1KeyboardValueHandle &&
              kLegacyKeyboardValueHandle != kRevision1MouseValueHandle &&
              kLegacyMouseValueHandle != kRevision1KeyboardValueHandle &&
              kLegacyMouseValueHandle != kRevision1MouseValueHandle);
inline constexpr std::array<std::uint8_t, 4> kHidInformation{
    0x11, 0x01, 0x00, 0x00};
inline constexpr std::array<std::uint8_t, 8> kNeutralKeyboard{};
inline constexpr std::array<std::uint8_t, 5> kNeutralMouse{};
inline constexpr std::array<std::uint8_t, 2> kKeyboardReportReference{0x01, 0x01};
inline constexpr std::array<std::uint8_t, 2> kMouseReportReference{0x02, 0x01};

inline constexpr std::array<std::uint8_t, 116> kReportMap{
    0x05,0x01,0x09,0x06,0xa1,0x01,0x85,0x01,0x05,0x07,0x19,0xe0,0x29,0xe7,
    0x15,0x00,0x25,0x01,0x75,0x01,0x95,0x08,0x81,0x02,0x95,0x01,0x75,0x08,
    0x81,0x01,0x95,0x06,0x75,0x08,0x15,0x00,0x26,0xff,0x00,0x19,0x00,0x2a,
    0xff,0x00,0x81,0x00,0xc0,
    0x05,0x01,0x09,0x02,0xa1,0x01,0x85,0x02,0x09,0x01,0xa1,0x00,0x05,0x09,
    0x19,0x01,0x29,0x05,0x15,0x00,0x25,0x01,0x95,0x05,0x75,0x01,0x81,0x02,
    0x95,0x01,0x75,0x03,0x81,0x01,0x05,0x01,0x09,0x30,0x09,0x31,0x09,0x38,
    0x15,0x81,0x25,0x7f,0x75,0x08,0x95,0x03,0x81,0x06,0x05,0x0c,0x0a,0x38,
    0x02,0x15,0x81,0x25,0x7f,0x75,0x08,0x95,0x01,0x81,0x06,0xc0,0xc0};

class Database final : public hid_control_executor::BleDatabase {
  public:
    int register_database() override;
    int validate_registered_database() override;
    void bind_event_sink(hid_control_executor::BleEventSink *sink) override;
    void set_generation(ble_lifecycle::Generation generation) override;
    hid_control_executor::BleHidHandles hid_handles() const override;
    hid_control_executor::BleNotifyBackendResult notify_custom(
        std::uint16_t connection_handle, std::uint16_t characteristic_handle,
        const std::uint8_t *payload,
        std::uint16_t payload_length) override;

    static int access(std::uint16_t connection_handle,
                      std::uint16_t attribute_handle,
                      struct ble_gatt_access_ctxt *context, void *argument);

  private:
    bool capture_control_point(std::uint16_t connection_handle,
                               bool suspended);

    hid_control_executor::BleEventSink *event_sink_ = nullptr;
    std::atomic<ble_lifecycle::Generation> generation_{0};
};

}  // namespace ble_hid_service
