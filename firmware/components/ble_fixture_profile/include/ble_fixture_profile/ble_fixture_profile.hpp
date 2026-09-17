#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "hid_capability/hid_capability.hpp"

namespace ble_fixture_profile {

enum class ProfileId : std::uint8_t {
    kStrictComposite = 0,
    kStandaloneMouseJustWorks = 1,
    kStandaloneKeyboard = 2,
    kStandaloneMouseJustWorksId7 = 3,
    kStandaloneKeyboardLeds = 4,
};

// Profile identity and bond/cache interpretation are separate namespaces.
enum class BondAssociationClass : std::uint8_t {
    kStrictComposite = 0, kStandaloneMouseJustWorks = 1, kStandaloneKeyboard = 2,
    kStandaloneMouseJustWorksId7 = 3, kStandaloneKeyboardLeds = 4
};
enum class LogicalIdentityClass : std::uint8_t { kSharedFixture = 0 };
enum class SelectionTransition : std::uint8_t { kStable, kInitializing, kFault };
struct SelectionSnapshot {
    ProfileId selected = ProfileId::kStrictComposite;
    ProfileId active = ProfileId::kStrictComposite;
    bool active_present = false;
    SelectionTransition transition = SelectionTransition::kStable;
};
enum class SelectionResult : std::uint8_t { kNoOp, kAccepted, kBusy };
struct SelectionOutcome {
    SelectionResult result = SelectionResult::kBusy;
    SelectionSnapshot snapshot{};
};

enum class TopologyId : std::uint8_t {
    kStrictComposite = 0,
    kMouseOnly = 1,
    kKeyboardOnly = 2,
    kKeyboardWithLeds = 3,
};

enum class GattTemplateId : std::uint8_t {
    kStrictComposite = 0,
    kMouseOnly = 1,
    kKeyboardOnly = 2,
    kKeyboardWithLeds = 3,
};

enum class GattLayoutId : std::uint8_t {
    kStrictRevision1 = 0,
    kMouseRevision2 = 1,
    kKeyboardRevision3 = 2,
    kMouseId7Revision4 = 3,
    kKeyboardLedsRevision5 = 4,
};

enum class SmpPolicyId : std::uint8_t {
    kAuthenticatedKeyboardOnly = 0,
    kNoInputNoOutput = 1,
};

enum class SecurityPolicyId : std::uint8_t {
    kAuthenticatedBonded = 0,
    kUnauthenticatedBonded = 1,
};

enum class AttributePolicyId : std::uint8_t {
    kAuthenticated = 0,
    kEncrypted = 1,
};

enum class CachePolicyId : std::uint8_t {
    kStrictRevision1 = 0,
    kMouseRevision2 = 1,
    kKeyboardRevision3 = 2,
    kMouseId7Revision4 = 3,
    kKeyboardLedsRevision5 = 4,
};

using ReportRole = hid_capability::ReportRole;

enum class ReportType : std::uint8_t {
    kInput = 1,
    kOutput = 2,
};

template <typename T>
struct View {
    const T *values;
    std::size_t length;

    constexpr const T *begin() const { return values; }
    constexpr const T *end() const { return values + length; }
    constexpr const T *data() const { return values; }
    constexpr std::size_t size() const { return length; }
};

using ByteView = View<std::uint8_t>;

using ReportMask = hid_capability::ReportMask;
using hid_capability::report_bit;

enum class IoCapability : std::uint8_t {
    kKeyboardOnly = 0,
    kNoInputNoOutput = 1,
};

enum class KeyDistribution : std::uint8_t {
    kNone = 0,
    kEncryption = 1U << 0,
    kIdentity = 1U << 1,
};

constexpr KeyDistribution operator|(KeyDistribution left,
                                    KeyDistribution right) {
    return static_cast<KeyDistribution>(static_cast<std::uint8_t>(left) |
                                        static_cast<std::uint8_t>(right));
}

constexpr bool has_key_distribution(KeyDistribution value,
                                    KeyDistribution required) {
    return (static_cast<std::uint8_t>(value) &
            static_cast<std::uint8_t>(required)) != 0;
}

struct ReportDefinition {
    ReportRole role;
    ReportType type;
    std::uint8_t report_id;
    std::uint8_t value_size;
    std::array<std::uint8_t, 2> report_reference;
    ByteView neutral_value;
};

struct SmpPolicy {
    SmpPolicyId id;
    IoCapability io_capability;
    bool bonding;
    bool mitm;
    bool secure_connections;
    bool secure_connections_only;
    std::uint8_t security_level;
    KeyDistribution our_key_distribution;
    KeyDistribution peer_key_distribution;
};

struct SecurityOutcomePolicy {
    SecurityPolicyId id;
    bool encrypted;
    bool authenticated;
    bool bonded;
    bool persisted_bond;
    bool identity_resolved;
    bool secure_connections_required;
    std::uint8_t key_size;
};

struct AttributePolicy {
    AttributePolicyId id;
    bool authenticated;
    std::uint8_t key_size;
};

struct CachePolicy {
    CachePolicyId id;
    std::uint8_t schema_revision;
    std::array<std::uint8_t, 1> schema_epoch_value;
};

struct GattLayout {
    GattLayoutId id;
    std::uint16_t gatt_service_start;
    std::uint16_t legacy_hid_service_start;
    std::uint16_t legacy_report_map_value;
    std::uint16_t legacy_keyboard_value;
    std::uint16_t legacy_mouse_value;
    std::uint16_t epoch_attribute_count;
    std::uint16_t epoch_service_start;
    std::uint16_t epoch_service_end;
    std::uint16_t hid_service_start;
    std::uint16_t report_map_value;
    std::uint16_t control_point_value;
    std::uint16_t keyboard_value;
    std::uint16_t mouse_value;
    std::uint16_t hid_last_attribute;
    std::uint16_t led_output_value = 0;
};

inline constexpr std::array<std::uint8_t, 4> kStrictHidInformation{
    0x11, 0x01, 0x00, 0x00};
inline constexpr std::array<std::uint8_t, 8> kStrictNeutralKeyboard{};
inline constexpr std::array<std::uint8_t, 5> kStrictNeutralMouse{};

inline constexpr std::array<std::uint8_t, 116> kStrictReportMap{
    0x05,0x01,0x09,0x06,0xa1,0x01,0x85,0x01,0x05,0x07,0x19,0xe0,0x29,0xe7,
    0x15,0x00,0x25,0x01,0x75,0x01,0x95,0x08,0x81,0x02,0x95,0x01,0x75,0x08,
    0x81,0x01,0x95,0x06,0x75,0x08,0x15,0x00,0x26,0xff,0x00,0x19,0x00,0x2a,
    0xff,0x00,0x81,0x00,0xc0,
    0x05,0x01,0x09,0x02,0xa1,0x01,0x85,0x02,0x09,0x01,0xa1,0x00,0x05,0x09,
    0x19,0x01,0x29,0x05,0x15,0x00,0x25,0x01,0x95,0x05,0x75,0x01,0x81,0x02,
    0x95,0x01,0x75,0x03,0x81,0x01,0x05,0x01,0x09,0x30,0x09,0x31,0x09,0x38,
    0x15,0x81,0x25,0x7f,0x75,0x08,0x95,0x03,0x81,0x06,0x05,0x0c,0x0a,0x38,
    0x02,0x15,0x81,0x25,0x7f,0x75,0x08,0x95,0x01,0x81,0x06,0xc0,0xc0};

inline constexpr std::array<std::uint8_t, 32> kStrictReportMapSha256{
    0xef,0x1b,0xe4,0x5d,0x8f,0xe7,0xd0,0x63,
    0x75,0x68,0xc8,0x95,0x4b,0x64,0xba,0xb9,
    0x71,0xd5,0xb5,0xf5,0x7b,0xf3,0xd4,0x4f,
    0x1c,0xc0,0x40,0xe8,0xfe,0x5c,0x3d,0x32};

inline constexpr std::array<ReportDefinition, 2> kStrictReports{{
    {.role = ReportRole::kKeyboardInput,
     .type = ReportType::kInput,
     .report_id = 1,
     .value_size = 8,
     .report_reference = {0x01, 0x01},
     .neutral_value = {kStrictNeutralKeyboard.data(),
                       kStrictNeutralKeyboard.size()}},
    {.role = ReportRole::kMouseInput,
     .type = ReportType::kInput,
     .report_id = 2,
     .value_size = 5,
     .report_reference = {0x02, 0x01},
     .neutral_value = {kStrictNeutralMouse.data(), kStrictNeutralMouse.size()}},
}};

struct ProfileDefinition {
    ProfileId id;
    const char *name;
    BondAssociationClass bond_class;
    LogicalIdentityClass identity_class;
    std::uint16_t revision;
    TopologyId topology;
    GattTemplateId gatt_template;
    GattLayout layout;
    View<ReportDefinition> reports;
    // Input producer/route roles only; Output reports never join release/readiness.
    ReportMask supported_reports;
    ReportMask required_input_subscriptions;
    ByteView report_map;
    std::array<std::uint8_t, 32> report_map_sha256;
    std::array<std::uint8_t, 4> hid_information;
    SmpPolicy smp;
    SecurityOutcomePolicy security;
    AttributePolicy attributes;
    CachePolicy cache;
};

inline constexpr ProfileDefinition kStrictComposite{
    .id = ProfileId::kStrictComposite,
    .name = "strict_composite",
    .bond_class = BondAssociationClass::kStrictComposite,
    .identity_class = LogicalIdentityClass::kSharedFixture,
    .revision = 1,
    .topology = TopologyId::kStrictComposite,
    .gatt_template = GattTemplateId::kStrictComposite,
    .layout = {
        .id = GattLayoutId::kStrictRevision1,
        .gatt_service_start = 0x0006,
        .legacy_hid_service_start = 0x000e,
        .legacy_report_map_value = 0x0012,
        .legacy_keyboard_value = 0x0016,
        .legacy_mouse_value = 0x001a,
        .epoch_attribute_count = 3,
        .epoch_service_start = 0x000e,
        .epoch_service_end = 0x0010,
        .hid_service_start = 0x0011,
        .report_map_value = 0x0015,
        .control_point_value = 0x0017,
        .keyboard_value = 0x0019,
        .mouse_value = 0x001d,
        .hid_last_attribute = 0x001f,
    },
    .reports = {kStrictReports.data(), kStrictReports.size()},
    .supported_reports = report_bit(ReportRole::kKeyboardInput) |
                         report_bit(ReportRole::kMouseInput),
    .required_input_subscriptions = report_bit(ReportRole::kKeyboardInput) |
                                    report_bit(ReportRole::kMouseInput),
    .report_map = {kStrictReportMap.data(), kStrictReportMap.size()},
    .report_map_sha256 = kStrictReportMapSha256,
    .hid_information = kStrictHidInformation,
    .smp = {
        .id = SmpPolicyId::kAuthenticatedKeyboardOnly,
        .io_capability = IoCapability::kKeyboardOnly,
        .bonding = true,
        .mitm = true,
        .secure_connections = true,
        .secure_connections_only = false,
        .security_level = 3,
        .our_key_distribution = KeyDistribution::kEncryption,
        .peer_key_distribution = KeyDistribution::kEncryption |
                                 KeyDistribution::kIdentity,
    },
    .security = {
        .id = SecurityPolicyId::kAuthenticatedBonded,
        .encrypted = true,
        .authenticated = true,
        .bonded = true,
        .persisted_bond = true,
        .identity_resolved = true,
        .secure_connections_required = false,
        .key_size = 16,
    },
    .attributes = {
        .id = AttributePolicyId::kAuthenticated,
        .authenticated = true,
        .key_size = 16,
    },
    .cache = {
        .id = CachePolicyId::kStrictRevision1,
        .schema_revision = 1,
        .schema_epoch_value = {0x01},
    },
};

// Reviewed finite mouse-only definition; strict bytes and policy stay frozen.
inline constexpr std::array<std::uint8_t, 69> kMouseReportMap{
    0x05,0x01,0x09,0x02,0xa1,0x01,0x85,0x02,0x09,0x01,0xa1,0x00,0x05,0x09,
    0x19,0x01,0x29,0x05,0x15,0x00,0x25,0x01,0x95,0x05,0x75,0x01,0x81,0x02,
    0x95,0x01,0x75,0x03,0x81,0x01,0x05,0x01,0x09,0x30,0x09,0x31,0x09,0x38,
    0x15,0x81,0x25,0x7f,0x75,0x08,0x95,0x03,0x81,0x06,0x05,0x0c,0x0a,0x38,
    0x02,0x15,0x81,0x25,0x7f,0x75,0x08,0x95,0x01,0x81,0x06,0xc0,0xc0};
inline constexpr std::array<std::uint8_t, 32> kMouseReportMapSha256{
    0xc2,0xfb,0x16,0x5f,0xfe,0x3f,0x84,0xfc,0x41,0x60,0xb0,0x13,0xe1,0x59,
    0x14,0xdf,0xfb,0xde,0xcb,0xc3,0x30,0xc3,0xc3,0x15,0x05,0x1d,0xc6,0x92,
    0x22,0x62,0xe9,0x24};
inline constexpr std::array<ReportDefinition, 1> kMouseReports{{
    {.role = ReportRole::kMouseInput,
     .type = ReportType::kInput,
     .report_id = 2,
     .value_size = 5,
     .report_reference = {2, 1},
     .neutral_value = {kStrictNeutralMouse.data(), kStrictNeutralMouse.size()}},
}};
inline constexpr ProfileDefinition kStandaloneMouseJustWorks = [] {
    auto profile = kStrictComposite;
    profile.id = ProfileId::kStandaloneMouseJustWorks;
    profile.name = "standalone_mouse_just_works";
    profile.bond_class = BondAssociationClass::kStandaloneMouseJustWorks;
    profile.topology = TopologyId::kMouseOnly;
    profile.gatt_template = GattTemplateId::kMouseOnly;
    profile.layout.id = GattLayoutId::kMouseRevision2;
    profile.layout.keyboard_value = 0;
    profile.layout.mouse_value = 0x0019;
    profile.layout.hid_last_attribute = 0x001b;
    profile.reports = {kMouseReports.data(), kMouseReports.size()};
    profile.supported_reports = report_bit(ReportRole::kMouseInput);
    profile.required_input_subscriptions = report_bit(ReportRole::kMouseInput);
    profile.report_map = {kMouseReportMap.data(), kMouseReportMap.size()};
    profile.report_map_sha256 = kMouseReportMapSha256;
    profile.smp.id = SmpPolicyId::kNoInputNoOutput;
    profile.smp.io_capability = IoCapability::kNoInputNoOutput;
    profile.smp.mitm = false;
    profile.smp.security_level = 2;
    profile.security.id = SecurityPolicyId::kUnauthenticatedBonded;
    profile.security.authenticated = false;
    profile.attributes.id = AttributePolicyId::kEncrypted;
    profile.attributes.authenticated = false;
    profile.cache.id = CachePolicyId::kMouseRevision2;
    profile.cache.schema_revision = 2;
    profile.cache.schema_epoch_value = {2};
    return profile;
}();

// Keyboard-only collection, independently bounded and hash-pinned. No Output
// Report or mouse collection is part of this first keyboard profile.
inline constexpr std::array<std::uint8_t, 47> kKeyboardReportMap{0x05,0x01,0x09,0x06,0xa1,0x01,0x85,0x01,0x05,0x07,0x19,0xe0,0x29,0xe7,0x15,0x00,0x25,0x01,0x75,0x01,0x95,0x08,0x81,0x02,0x95,0x01,0x75,0x08,0x81,0x01,0x95,0x06,0x75,0x08,0x15,0x00,0x26,0xff,0x00,0x19,0x00,0x2a,0xff,0x00,0x81,0x00,0xc0};
inline constexpr std::array<std::uint8_t, 32> kKeyboardReportMapSha256{0xd5,0x6a,0x8a,0xa0,0xef,0xc3,0xf4,0x12,0x6a,0x0a,0xea,0x1d,0xf4,0xc6,0xf6,0xf1,0xb5,0xbc,0x1d,0x62,0x4a,0x71,0x01,0x59,0xc3,0x4b,0x1c,0xdb,0x02,0xbc,0x45,0xae};
inline constexpr std::array<ReportDefinition, 1> kKeyboardReports{{
    {.role = ReportRole::kKeyboardInput, .type = ReportType::kInput,
     .report_id = 1, .value_size = 8, .report_reference = {1, 1},
     .neutral_value = {kStrictNeutralKeyboard.data(), kStrictNeutralKeyboard.size()}},
}};
inline constexpr ProfileDefinition kStandaloneKeyboard = [] {
    auto profile = kStrictComposite;
    profile.id = ProfileId::kStandaloneKeyboard;
    profile.name = "standalone_keyboard";
    profile.bond_class = BondAssociationClass::kStandaloneKeyboard;
    profile.topology = TopologyId::kKeyboardOnly;
    profile.gatt_template = GattTemplateId::kKeyboardOnly;
    profile.layout.id = GattLayoutId::kKeyboardRevision3;
    profile.layout.mouse_value = 0;
    profile.layout.hid_last_attribute = 0x001b;
    profile.reports = {kKeyboardReports.data(), kKeyboardReports.size()};
    profile.supported_reports = report_bit(ReportRole::kKeyboardInput);
    profile.required_input_subscriptions = report_bit(ReportRole::kKeyboardInput);
    profile.report_map = {kKeyboardReportMap.data(), kKeyboardReportMap.size()};
    profile.report_map_sha256 = kKeyboardReportMapSha256;
    profile.cache.id = CachePolicyId::kKeyboardRevision3;
    profile.cache.schema_revision = 3;
    profile.cache.schema_epoch_value = {3};
    return profile;
}();

// Finite Report ID variation: same input semantics, independent cache/bond authority.
inline constexpr std::array<std::uint8_t, 69> kMouseId7ReportMap{0x05,0x01,0x09,0x02,0xa1,0x01,0x85,0x07,0x09,0x01,0xa1,0x00,0x05,0x09,0x19,0x01,0x29,0x05,0x15,0x00,0x25,0x01,0x95,0x05,0x75,0x01,0x81,0x02,0x95,0x01,0x75,0x03,0x81,0x01,0x05,0x01,0x09,0x30,0x09,0x31,0x09,0x38,0x15,0x81,0x25,0x7f,0x75,0x08,0x95,0x03,0x81,0x06,0x05,0x0c,0x0a,0x38,0x02,0x15,0x81,0x25,0x7f,0x75,0x08,0x95,0x01,0x81,0x06,0xc0,0xc0};
inline constexpr std::array<std::uint8_t, 32> kMouseId7ReportMapSha256{0x7e,0x06,0xb7,0x73,0xbb,0x36,0xde,0xa8,0x3e,0x1f,0x0f,0x76,0xd9,0xb4,0x9c,0x46,0x25,0x6d,0x1a,0x21,0xd7,0x01,0x83,0x15,0x4c,0xa4,0x18,0x6e,0x61,0x0e,0xf6,0x28};
inline constexpr std::array<ReportDefinition, 1> kMouseId7Reports{{
    {.role = ReportRole::kMouseInput, .type = ReportType::kInput,
     .report_id = 7, .value_size = 5, .report_reference = {7, 1},
     .neutral_value = {kStrictNeutralMouse.data(), kStrictNeutralMouse.size()}},
}};
inline constexpr ProfileDefinition kStandaloneMouseJustWorksId7 = [] {
    auto profile = kStandaloneMouseJustWorks;
    profile.id = ProfileId::kStandaloneMouseJustWorksId7;
    profile.name = "standalone_mouse_just_works_id7";
    profile.bond_class = BondAssociationClass::kStandaloneMouseJustWorksId7;
    profile.layout.id = GattLayoutId::kMouseId7Revision4;
    profile.reports = {kMouseId7Reports.data(), kMouseId7Reports.size()};
    profile.report_map = {kMouseId7ReportMap.data(), kMouseId7ReportMap.size()};
    profile.report_map_sha256 = kMouseId7ReportMapSha256;
    profile.cache.id = CachePolicyId::kMouseId7Revision4;
    profile.cache.schema_revision = 4;
    profile.cache.schema_epoch_value = {4};
    return profile;
}();

// Keyboard Input ID1 plus five standard host LED Output bits, ID1/type2.
inline constexpr std::array<std::uint8_t, 69> kKeyboardLedsReportMap{0x05,0x01,0x09,0x06,0xa1,0x01,0x85,0x01,0x05,0x07,0x19,0xe0,0x29,0xe7,0x15,0x00,0x25,0x01,0x75,0x01,0x95,0x08,0x81,0x02,0x95,0x01,0x75,0x08,0x81,0x01,0x95,0x06,0x75,0x08,0x15,0x00,0x26,0xff,0x00,0x19,0x00,0x2a,0xff,0x00,0x81,0x00,0x95,0x05,0x75,0x01,0x05,0x08,0x19,0x01,0x29,0x05,0x15,0x00,0x25,0x01,0x91,0x02,0x95,0x01,0x75,0x03,0x91,0x01,0xc0};
inline constexpr std::array<std::uint8_t, 32> kKeyboardLedsReportMapSha256{0xbc,0x08,0xd7,0x9c,0xc4,0x59,0x91,0x44,0x6b,0x3f,0x46,0xb3,0x7b,0x23,0xe6,0xc8,0x2a,0x86,0xa3,0xad,0x94,0x50,0xf1,0xfe,0x68,0x0e,0x4e,0x11,0x34,0xfd,0x50,0x4d};
inline constexpr std::array<std::uint8_t, 1> kNeutralLeds{};
inline constexpr std::array<ReportDefinition, 2> kKeyboardLedsReports{{
    kKeyboardReports[0],
    {.role = ReportRole::kLedOutput, .type = ReportType::kOutput,
     .report_id = 1, .value_size = 1, .report_reference = {1, 2},
     .neutral_value = {kNeutralLeds.data(), kNeutralLeds.size()}},
}};
inline constexpr ProfileDefinition kStandaloneKeyboardLeds = [] {
    auto profile = kStandaloneKeyboard;
    profile.id = ProfileId::kStandaloneKeyboardLeds;
    profile.name = "standalone_keyboard_leds";
    profile.bond_class = BondAssociationClass::kStandaloneKeyboardLeds;
    profile.topology = TopologyId::kKeyboardWithLeds;
    profile.gatt_template = GattTemplateId::kKeyboardWithLeds;
    profile.layout.id = GattLayoutId::kKeyboardLedsRevision5;
    profile.layout.led_output_value = 0x001d;
    profile.layout.hid_last_attribute = 0x001e;
    profile.reports = {kKeyboardLedsReports.data(), kKeyboardLedsReports.size()};
    profile.report_map = {kKeyboardLedsReportMap.data(), kKeyboardLedsReportMap.size()};
    profile.report_map_sha256 = kKeyboardLedsReportMapSha256;
    profile.cache.id = CachePolicyId::kKeyboardLedsRevision5;
    profile.cache.schema_revision = 5;
    profile.cache.schema_epoch_value = {5};
    return profile;
}();

struct LedValue { bool valid = false; std::uint8_t leds = 0; };
struct LedStatus { bool supported = false; bool valid = false; std::uint8_t leds = 0; };

inline constexpr std::array<const ProfileDefinition *, 5> kCatalog{
    &kStrictComposite, &kStandaloneMouseJustWorks, &kStandaloneKeyboard,
    &kStandaloneMouseJustWorksId7, &kStandaloneKeyboardLeds};

// Internal reviewed definitions can precede public lifecycle enablement.
constexpr const ProfileDefinition *find_definition(ProfileId id) {
    switch (id) {
        case ProfileId::kStrictComposite: return &kStrictComposite;
        case ProfileId::kStandaloneMouseJustWorks: return &kStandaloneMouseJustWorks;
        case ProfileId::kStandaloneKeyboard: return &kStandaloneKeyboard;
        case ProfileId::kStandaloneKeyboardLeds: return &kStandaloneKeyboardLeds;
        case ProfileId::kStandaloneMouseJustWorksId7: return &kStandaloneMouseJustWorksId7;
    }
    return nullptr;
}

constexpr const ProfileDefinition *find_profile(std::string_view name) {
    for (const auto *profile : kCatalog) {
        if (name == profile->name) return profile;
    }
    return nullptr;
}

constexpr const ProfileDefinition *find_profile(ProfileId id) {
    for (const auto *profile : kCatalog) {
        if (id == profile->id) return profile;
    }
    return nullptr;
}

constexpr const ProfileDefinition &strict_composite() {
    return kStrictComposite;
}

constexpr const ReportDefinition *find_report(const ProfileDefinition &profile,
                                              ReportRole role) {
    for (const ReportDefinition &report : profile.reports) {
        if (report.role == role) {
            return &report;
        }
    }
    return nullptr;
}

constexpr bool reports_present(const ProfileDefinition &profile,
                               ReportMask reports) {
    return (profile.supported_reports & reports) == reports;
}

constexpr bool subscriptions_ready(const ProfileDefinition &profile,
                                   ReportMask subscriptions) {
    return (subscriptions & profile.required_input_subscriptions) ==
           profile.required_input_subscriptions;
}

static_assert(kStrictComposite.report_map.size() == 116);
static_assert(kCatalog.size() == 5 &&
              kCatalog[0]->id == ProfileId::kStrictComposite);
static_assert(kStrictReports[0].report_id == 1 &&
              kStrictReports[0].value_size == 8);
static_assert(kStrictReports[1].report_id == 2 &&
              kStrictReports[1].value_size == 5);
static_assert(kStrictReports[0].role != kStrictReports[1].role &&
              (kStrictReports[0].report_id != kStrictReports[1].report_id ||
               kStrictReports[0].type != kStrictReports[1].type));
static_assert(kStrictReports[0].report_reference[0] ==
                  kStrictReports[0].report_id &&
              kStrictReports[0].report_reference[1] ==
                  static_cast<std::uint8_t>(kStrictReports[0].type) &&
              kStrictReports[1].report_reference[0] ==
                  kStrictReports[1].report_id &&
              kStrictReports[1].report_reference[1] ==
                  static_cast<std::uint8_t>(kStrictReports[1].type));
static_assert(kStrictComposite.required_input_subscriptions == 0x03);
static_assert(reports_present(
    kStrictComposite, kStrictComposite.required_input_subscriptions));
static_assert(kStrictComposite.smp.mitm &&
              kStrictComposite.smp.secure_connections &&
              !kStrictComposite.smp.secure_connections_only);
static_assert(kStrictComposite.security.authenticated &&
              !kStrictComposite.security.secure_connections_required &&
              kStrictComposite.security.key_size == 16);

}  // namespace ble_fixture_profile
