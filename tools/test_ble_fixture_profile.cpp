#include <cassert>
#include <cstdint>

#include "ble_fixture_profile/ble_fixture_profile.hpp"

int main() {
    using namespace ble_fixture_profile;
    const auto &profile = strict_composite();

    assert(kCatalog.size() == 1);
    assert(kCatalog[0] == &profile);
    assert(profile.id == ProfileId::kStrictComposite);
    assert(profile.revision == 1);
    assert(profile.topology == TopologyId::kStrictComposite);
    assert(profile.gatt_template == GattTemplateId::kStrictComposite);
    assert(profile.reports.size() == 2);

    assert(profile.report_map.data() == kStrictReportMap.data());
    assert(profile.report_map.size() == 116);
    const std::array<std::uint8_t, 32> expected_digest{
        0xef,0x1b,0xe4,0x5d,0x8f,0xe7,0xd0,0x63,
        0x75,0x68,0xc8,0x95,0x4b,0x64,0xba,0xb9,
        0x71,0xd5,0xb5,0xf5,0x7b,0xf3,0xd4,0x4f,
        0x1c,0xc0,0x40,0xe8,0xfe,0x5c,0x3d,0x32};
    assert(profile.report_map_sha256 == expected_digest);
    assert(profile.hid_information ==
           (std::array<std::uint8_t, 4>{0x11, 0x01, 0x00, 0x00}));

    const auto *keyboard = find_report(profile, ReportRole::kKeyboardInput);
    const auto *mouse = find_report(profile, ReportRole::kMouseInput);
    assert(keyboard != nullptr && mouse != nullptr);
    assert(keyboard->type == ReportType::kInput);
    assert(keyboard->report_id == 1 && keyboard->value_size == 8);
    assert(keyboard->report_reference ==
           (std::array<std::uint8_t, 2>{0x01, 0x01}));
    assert(keyboard->neutral_value.data() == kStrictNeutralKeyboard.data());
    assert(keyboard->neutral_value.size() == kStrictNeutralKeyboard.size());
    assert(mouse->type == ReportType::kInput);
    assert(mouse->report_id == 2 && mouse->value_size == 5);
    assert(mouse->report_reference ==
           (std::array<std::uint8_t, 2>{0x02, 0x01}));
    assert(mouse->neutral_value.data() == kStrictNeutralMouse.data());
    assert(mouse->neutral_value.size() == kStrictNeutralMouse.size());

    assert(reports_present(profile, report_bit(ReportRole::kKeyboardInput)));
    assert(reports_present(profile, report_bit(ReportRole::kMouseInput)));
    assert(!subscriptions_ready(profile, 0));
    assert(!subscriptions_ready(
        profile, report_bit(ReportRole::kKeyboardInput)));
    assert(!subscriptions_ready(profile, report_bit(ReportRole::kMouseInput)));
    assert(subscriptions_ready(profile,
        report_bit(ReportRole::kKeyboardInput) |
        report_bit(ReportRole::kMouseInput)));

    assert(profile.smp.io_capability == IoCapability::kKeyboardOnly);
    assert(profile.smp.bonding && profile.smp.mitm);
    assert(profile.smp.secure_connections);
    assert(!profile.smp.secure_connections_only);
    assert(profile.smp.security_level == 3);
    assert(has_key_distribution(profile.smp.our_key_distribution,
                                KeyDistribution::kEncryption));
    assert(!has_key_distribution(profile.smp.our_key_distribution,
                                 KeyDistribution::kIdentity));
    assert(has_key_distribution(profile.smp.peer_key_distribution,
                                KeyDistribution::kEncryption));
    assert(has_key_distribution(profile.smp.peer_key_distribution,
                                KeyDistribution::kIdentity));

    assert(profile.security.encrypted);
    assert(profile.security.authenticated);
    assert(profile.security.bonded);
    assert(profile.security.persisted_bond);
    assert(profile.security.identity_resolved);
    assert(!profile.security.secure_connections_required);
    assert(profile.security.key_size == 16);
    assert(profile.attributes.authenticated);
    assert(profile.attributes.key_size == 16);

    assert(profile.cache.schema_revision == 1);
    assert(profile.cache.schema_epoch_value ==
           (std::array<std::uint8_t, 1>{0x01}));
    assert(profile.layout.gatt_service_start == 0x0006);
    assert(profile.layout.epoch_service_start == 0x000e);
    assert(profile.layout.epoch_service_end == 0x0010);
    assert(profile.layout.hid_service_start == 0x0011);
    assert(profile.layout.report_map_value == 0x0015);
    assert(profile.layout.control_point_value == 0x0017);
    assert(profile.layout.keyboard_value == 0x0019);
    assert(profile.layout.mouse_value == 0x001d);
    assert(profile.layout.hid_last_attribute == 0x001f);
}
