#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace hid_capability {

// Report roles are finite. LED Output is host-to-device observation and never
// joins the input producer, subscription readiness, or all-up route masks.
enum class ReportRole : std::uint8_t {
    kKeyboardInput = 0,
    kMouseInput = 1,
    kLedOutput = 2,
    kCount = 3,
};

using ReportMask = std::uint8_t;
inline constexpr std::size_t kReportRoleCount =
    static_cast<std::size_t>(ReportRole::kCount);

constexpr std::size_t role_index(ReportRole role) {
    return static_cast<std::size_t>(role);
}

constexpr ReportMask report_bit(ReportRole role) {
    return static_cast<ReportMask>(1U << role_index(role));
}

inline constexpr ReportMask kKeyboardInput =
    report_bit(ReportRole::kKeyboardInput);
inline constexpr ReportMask kMouseInput = report_bit(ReportRole::kMouseInput);
inline constexpr ReportMask kInputRoles = kKeyboardInput | kMouseInput;
inline constexpr ReportMask kKnownRoles =
    kInputRoles | report_bit(ReportRole::kLedOutput);

constexpr bool is_subset(ReportMask subset, ReportMask superset) {
    return (subset & superset) == subset;
}

constexpr bool has_role(ReportMask roles, ReportRole role) {
    return (roles & report_bit(role)) != 0;
}

struct ReportHandles {
    std::array<std::uint16_t, kReportRoleCount> values{};

    constexpr std::uint16_t get(ReportRole role) const {
        return values[role_index(role)];
    }

    constexpr void set(ReportRole role, std::uint16_t handle) {
        values[role_index(role)] = handle;
    }
};

// Every present role owns one nonzero, unique handle. Absent roles must remain
// zero so an incoming event can never alias an absent capability.
constexpr bool valid_handles(ReportMask present_roles,
                             const ReportHandles &handles) {
    if ((present_roles & static_cast<ReportMask>(~kKnownRoles)) != 0) {
        return false;
    }
    for (std::size_t index = 0; index < kReportRoleCount; ++index) {
        const ReportMask bit = static_cast<ReportMask>(1U << index);
        const bool present = (present_roles & bit) != 0;
        if (present != (handles.values[index] != 0)) {
            return false;
        }
        if (!present) {
            continue;
        }
        for (std::size_t prior = 0; prior < index; ++prior) {
            if (handles.values[prior] == handles.values[index]) {
                return false;
            }
        }
    }
    return true;
}

constexpr bool input_route_definition_valid(
    ReportMask present_roles, ReportMask required_subscriptions,
    const ReportHandles &handles) {
    return present_roles != 0 && is_subset(present_roles, kInputRoles) &&
           is_subset(required_subscriptions, present_roles) &&
           valid_handles(present_roles, handles);
}

constexpr bool input_route_ready(ReportMask present_roles,
                                 ReportMask required_subscriptions,
                                 ReportMask ready_subscriptions) {
    return present_roles != 0 && is_subset(present_roles, kInputRoles) &&
           is_subset(required_subscriptions, present_roles) &&
           is_subset(required_subscriptions, ready_subscriptions);
}

}  // namespace hid_capability
