#pragma once

#include <array>
#include <atomic>
#include <cstdint>

#include "ble_fixture_profile/ble_fixture_profile.hpp"
#include "hid_control_executor/hid_control_executor.hpp"

struct ble_gatt_access_ctxt;

namespace ble_hid_service {

inline constexpr const auto &kStrictProfile =
    ble_fixture_profile::strict_composite();

// Bump only when bonded clients must refresh cache-relevant GATT/HID
// interpretation.  Missing per-peer metadata is legacy revision zero.
inline constexpr std::uint8_t kGattSchemaRevision =
    kStrictProfile.cache.schema_revision;
inline constexpr const auto &kGattSchemaEpochValue =
    kStrictProfile.cache.schema_epoch_value;

// ESP-IDF v5.5.4 registers the configured GAP and GATT services at 0x0001
// through 0x000d.  Revision 1 consumes the old HID start with one three-
// attribute epoch service, then registers HID at a different start handle.
// Runtime validation below fails closed if the target database no longer
// matches this compile-time topology contract.  Published HID work continues
// to use the handles assigned by NimBLE, never these constants.
inline constexpr std::uint16_t kGattServiceStartHandle =
    kStrictProfile.layout.gatt_service_start;
inline constexpr std::uint16_t kLegacyHidServiceStartHandle =
    kStrictProfile.layout.legacy_hid_service_start;
inline constexpr std::uint16_t kLegacyReportMapValueHandle =
    kStrictProfile.layout.legacy_report_map_value;
inline constexpr std::uint16_t kLegacyKeyboardValueHandle =
    kStrictProfile.layout.legacy_keyboard_value;
inline constexpr std::uint16_t kLegacyMouseValueHandle =
    kStrictProfile.layout.legacy_mouse_value;
inline constexpr std::uint16_t kRevision1EpochAttributeCount =
    kStrictProfile.layout.epoch_attribute_count;
inline constexpr std::uint16_t kRevision1EpochServiceStartHandle =
    kStrictProfile.layout.epoch_service_start;
inline constexpr std::uint16_t kRevision1EpochServiceEndHandle =
    kStrictProfile.layout.epoch_service_end;
inline constexpr std::uint16_t kRevision1HidServiceStartHandle =
    kStrictProfile.layout.hid_service_start;
inline constexpr std::uint16_t kRevision1ReportMapValueHandle =
    kStrictProfile.layout.report_map_value;
inline constexpr std::uint16_t kRevision1ControlPointValueHandle =
    kStrictProfile.layout.control_point_value;
inline constexpr std::uint16_t kRevision1KeyboardValueHandle =
    kStrictProfile.layout.keyboard_value;
inline constexpr std::uint16_t kRevision1MouseValueHandle =
    kStrictProfile.layout.mouse_value;
inline constexpr std::uint16_t kRevision1HidLastAttributeHandle =
    kStrictProfile.layout.hid_last_attribute;

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
inline constexpr const auto &kHidInformation = kStrictProfile.hid_information;
inline constexpr const auto &kKeyboardReport = *ble_fixture_profile::find_report(
    kStrictProfile, ble_fixture_profile::ReportRole::kKeyboardInput);
inline constexpr const auto &kMouseReport = *ble_fixture_profile::find_report(
    kStrictProfile, ble_fixture_profile::ReportRole::kMouseInput);
inline constexpr const auto &kNeutralKeyboard = kKeyboardReport.neutral_value;
inline constexpr const auto &kNeutralMouse = kMouseReport.neutral_value;
inline constexpr const auto &kKeyboardReportReference =
    kKeyboardReport.report_reference;
inline constexpr const auto &kMouseReportReference =
    kMouseReport.report_reference;
inline constexpr const auto &kReportMap = kStrictProfile.report_map;

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
