#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "ble_security/ble_security.hpp"

namespace ble_transport::detail {

// A correct image can have at most one schema record for each live bond.  Keep
// a larger, explicit recovery ceiling so several historical orphan records can
// still be repaired without allowing malformed NVS to consume unbounded RAM.
// Reaching this ceiling fails closed; it is never silently truncated.
inline constexpr std::size_t kSchemaRecoveryCapacity = 16;

struct StoreIdentity {
    std::uint8_t type = 0;
    std::array<std::uint8_t, 6> value{};
};

constexpr bool same_identity(const StoreIdentity &left,
                             const StoreIdentity &right) {
    return left.type == right.type && left.value == right.value;
}

constexpr bool valid_identity(const StoreIdentity &identity) {
    if (identity.type > 1) {
        return false;
    }
    if (identity.type != 0) {
        return true;
    }
    for (const auto byte : identity.value) {
        if (byte != 0) {
            return true;
        }
    }
    return false;
}

constexpr bool valid_security_record_layout(bool blob_type,
                                            std::size_t actual_size,
                                            std::size_t expected_size) {
    return blob_type && actual_size == expected_size;
}

constexpr bool valid_schema_record_layout(bool unsigned_byte_type,
                                          std::size_t actual_size) {
    return unsigned_byte_type && actual_size == sizeof(std::uint8_t);
}

constexpr int lower_hex_value(char value) {
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    return -1;
}

constexpr bool parse_canonical_schema_key(const char *key,
                                          StoreIdentity &identity) {
    if (key == nullptr || key[0] != 'r') {
        return false;
    }
    for (std::size_t index = 1; index < 15; ++index) {
        if (key[index] == '\0') {
            return false;
        }
    }
    if (key[15] != '\0') {
        return false;
    }
    const int type_high = lower_hex_value(key[1]);
    const int type_low = lower_hex_value(key[2]);
    if (type_high < 0 || type_low < 0) {
        return false;
    }
    identity.type = static_cast<std::uint8_t>((type_high << 4) | type_low);
    for (std::size_t index = 0; index < identity.value.size(); ++index) {
        const int high = lower_hex_value(key[3 + index * 2]);
        const int low = lower_hex_value(key[4 + index * 2]);
        if (high < 0 || low < 0) {
            return false;
        }
        identity.value[index] =
            static_cast<std::uint8_t>((high << 4) | low);
    }
    return valid_identity(identity);
}

template <std::size_t Capacity>
struct IdentitySet {
    std::array<StoreIdentity, Capacity> identities{};
    std::size_t count = 0;
    bool trustworthy = true;
};

using SecurityIdentitySet = IdentitySet<ble_security::kBondCapacity>;
using SchemaIdentitySet = IdentitySet<kSchemaRecoveryCapacity>;

template <std::size_t Capacity>
constexpr bool valid_set(const IdentitySet<Capacity> &set) {
    if (!set.trustworthy || set.count > set.identities.size()) {
        return false;
    }
    for (std::size_t left = 0; left < set.count; ++left) {
        if (!valid_identity(set.identities[left])) {
            return false;
        }
        for (std::size_t right = left + 1; right < set.count; ++right) {
            if (same_identity(set.identities[left], set.identities[right])) {
                return false;
            }
        }
    }
    return true;
}

template <std::size_t Capacity>
constexpr bool contains(const IdentitySet<Capacity> &set,
                        const StoreIdentity &identity) {
    for (std::size_t index = 0; index < set.count; ++index) {
        if (same_identity(set.identities[index], identity)) {
            return true;
        }
    }
    return false;
}

constexpr bool same_set(const SecurityIdentitySet &left,
                        const SecurityIdentitySet &right) {
    if (!valid_set(left) || !valid_set(right) || left.count != right.count) {
        return false;
    }
    for (std::size_t index = 0; index < left.count; ++index) {
        if (!contains(right, left.identities[index])) {
            return false;
        }
    }
    return true;
}

enum class RecoveryPlanKind : std::uint8_t {
    kReady,
    kStorageFailure,
};

struct RecoveryPlan {
    RecoveryPlanKind kind = RecoveryPlanKind::kStorageFailure;
    std::array<StoreIdentity, kSchemaRecoveryCapacity> orphans{};
    std::size_t orphan_count = 0;
};

constexpr bool identity_less(const StoreIdentity &left,
                             const StoreIdentity &right) {
    if (left.type != right.type) {
        return left.type < right.type;
    }
    for (std::size_t index = 0; index < left.value.size(); ++index) {
        if (left.value[index] != right.value[index]) {
            return left.value[index] < right.value[index];
        }
    }
    return false;
}

// All inputs describe the complete bounded pre-startup model.  No caller may
// mutate schema NVS until this function has accepted every set.
constexpr RecoveryPlan make_recovery_plan(
    const SecurityIdentitySet &persistent_our,
    const SecurityIdentitySet &persistent_peer,
    const SecurityIdentitySet &restored_our,
    const SecurityIdentitySet &restored_peer,
    const SchemaIdentitySet &schemas) {
    RecoveryPlan plan{};
    if (!valid_set(persistent_our) || !valid_set(persistent_peer) ||
        !valid_set(restored_our) || !valid_set(restored_peer) ||
        !valid_set(schemas) || !same_set(persistent_our, restored_our) ||
        !same_set(persistent_peer, restored_peer)) {
        return plan;
    }

    // A half bond is globally inconsistent even when it has no schema entry.
    for (std::size_t index = 0; index < persistent_our.count; ++index) {
        if (!contains(persistent_peer, persistent_our.identities[index])) {
            return plan;
        }
    }
    for (std::size_t index = 0; index < persistent_peer.count; ++index) {
        if (!contains(persistent_our, persistent_peer.identities[index])) {
            return plan;
        }
    }

    for (std::size_t index = 0; index < schemas.count; ++index) {
        const auto &identity = schemas.identities[index];
        const bool our = contains(persistent_our, identity);
        const bool peer = contains(persistent_peer, identity);
        if (our != peer) {
            return RecoveryPlan{};
        }
        if (!our) {
            plan.orphans[plan.orphan_count++] = identity;
        }
    }

    // NVS iterator order is not a public ordering guarantee.  Freeze a stable
    // project order before performing the first delete.
    for (std::size_t left = 0; left < plan.orphan_count; ++left) {
        for (std::size_t right = left + 1; right < plan.orphan_count; ++right) {
            if (identity_less(plan.orphans[right], plan.orphans[left])) {
                const auto temporary = plan.orphans[left];
                plan.orphans[left] = plan.orphans[right];
                plan.orphans[right] = temporary;
            }
        }
    }
    plan.kind = RecoveryPlanKind::kReady;
    return plan;
}

struct RecoveryExecutionResult {
    std::int32_t status = 0;
    std::size_t completed = 0;
};

template <typename DeleteVerified>
RecoveryExecutionResult run_orphan_recovery(
    const RecoveryPlan &plan, DeleteVerified delete_verified) {
    if (plan.kind != RecoveryPlanKind::kReady ||
        plan.orphan_count > plan.orphans.size()) {
        return {.status = -1, .completed = 0};
    }
    for (std::size_t index = 0; index < plan.orphan_count; ++index) {
        const std::int32_t status = delete_verified(plan.orphans[index]);
        if (status != 0) {
            return {.status = status, .completed = index};
        }
    }
    return {.status = 0, .completed = plan.orphan_count};
}

enum class SchemaFirstRemovalStage : std::uint8_t {
    kSchemaDelete,
    kPeerDelete,
    kPostcondition,
    kComplete,
};

struct SchemaFirstRemovalResult {
    SchemaFirstRemovalStage stage = SchemaFirstRemovalStage::kSchemaDelete;
    std::int32_t status = 0;
};

// This executable transaction skeleton is shared by production and native
// fault-cut tests, so schema-first ordering is not merely a textual contract.
template <typename DeleteSchema, typename DeletePeer, typename Verify>
SchemaFirstRemovalResult run_schema_first_removal(
    DeleteSchema delete_schema, DeletePeer delete_peer, Verify verify) {
    std::int32_t status = delete_schema();
    if (status != 0) {
        return {.stage = SchemaFirstRemovalStage::kSchemaDelete,
                .status = status};
    }
    status = delete_peer();
    if (status != 0) {
        return {.stage = SchemaFirstRemovalStage::kPeerDelete,
                .status = status};
    }
    status = verify();
    if (status != 0) {
        return {.stage = SchemaFirstRemovalStage::kPostcondition,
                .status = status};
    }
    return {.stage = SchemaFirstRemovalStage::kComplete, .status = 0};
}

}  // namespace ble_transport::detail
