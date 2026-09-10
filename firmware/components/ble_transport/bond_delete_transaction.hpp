#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "persistent_store_recovery.hpp"

namespace ble_transport::detail {

// A single, versioned, exact-identity receipt. Only an accepted user removal
// creates it. No wildcard, implicit eviction, or arbitrary orphan recovery.
inline constexpr char kDeleteNamespace[] = "hid_bond_tx";
inline constexpr char kDeleteKey[] = "delete_v1";
inline constexpr char kDeleteBondNamespace[] = "nimble_bond";
inline constexpr int kDeleteInvalid = -1;
using DeleteIntent = std::array<std::uint8_t, 8>;

struct DeleteRead {
    int status = 0;
    bool present = false;
    std::size_t size = 0;
};

constexpr int absence_status(int status, int absent, int invalid) {
    return status == absent ? 0 : status == 0 ? invalid : status;
}

inline DeleteIntent deletion_intent(const StoreIdentity &identity) {
    DeleteIntent bytes{1, identity.type};
    for (std::size_t i = 0; i < 6; ++i) bytes[i + 2] = identity.value[i];
    return bytes;
}

inline bool decode_intent(const DeleteIntent &bytes, StoreIdentity &identity) {
    if (bytes[0] != 1) return false;
    identity.type = bytes[1];
    for (std::size_t i = 0; i < 6; ++i) identity.value[i] = bytes[i + 2];
    return valid_identity(identity);
}

struct DeleteCategory {
    const char *prefix;
    std::size_t count;
    std::size_t size;
    std::size_t identity_offset;
};
// Aux first is useful on reboot, but the receipt, not ordering, provides
// recoverability across any core boundary. These layouts are target-asserted.
inline constexpr std::array<DeleteCategory, 5> kDeleteCategories{{
    {"cccd_sec_", 15, 16, 0}, {"rpa_rec_", 3, 14, 7},
    {"csfc_sec_", 3, 8, 0}, {"our_sec_", 3, 88, 0},
    {"peer_sec_", 3, 88, 0},
}};

inline bool deletion_slot(const char *key, bool &recognized) {
    recognized = false;
    for (const auto &category : kDeleteCategories) {
        if (std::strncmp(key, category.prefix, std::strlen(category.prefix)) != 0) continue;
        recognized = true;
        for (std::size_t i = 1; i <= category.count; ++i) {
            char expected[16]{};
            std::snprintf(expected, sizeof(expected), "%s%u", category.prefix,
                          static_cast<unsigned>(i));
            if (std::strcmp(key, expected) == 0) return true;
        }
        return false;
    }
    return true;
}

inline StoreIdentity deletion_identity(const std::uint8_t *bytes) {
    StoreIdentity identity{.type = bytes[0]};
    for (std::size_t i = 0; i < 6; ++i) identity.value[i] = bytes[i + 1];
    return identity;
}

template <typename Store>
int read_delete_intent(Store &store, StoreIdentity &identity, bool &present) {
    DeleteIntent bytes{};
    const auto read = store.read(kDeleteNamespace, kDeleteKey, bytes.data(), bytes.size());
    present = read.present;
    if (read.status) return read.status;
    if (!read.present) return 0;
    return read.size == bytes.size() && decode_intent(bytes, identity) ? 0 : kDeleteInvalid;
}

template <typename Store>
int begin_delete_intent(Store &store, const StoreIdentity &identity) {
    if (!valid_identity(identity)) return kDeleteInvalid;
    StoreIdentity old{}; bool present = false;
    int status = read_delete_intent(store, old, present);
    if (status || present) return status ? status : kDeleteInvalid;
    const auto bytes = deletion_intent(identity);
    status = store.write(kDeleteNamespace, kDeleteKey, bytes.data(), bytes.size());
    if (status) return status;
    status = read_delete_intent(store, old, present);
    return status ? status : present && same_identity(old, identity) ? 0 : kDeleteInvalid;
}

// Validate every known slot before any key erase. NVS-only target records
// are included. RPA alias overlap with another canonical peer fails closed
// before NimBLE's broader alias matching could touch that peer.
template <typename Store>
int target_delete_scan(Store &store, const StoreIdentity &target,
                       std::array<bool, 27> &selected) {
    if (!valid_identity(target)) return kDeleteInvalid;
    int status = store.validate_layout();
    if (status) return status;
    selected.fill(false);
    std::size_t slot = 0;
    for (const auto &category : kDeleteCategories) {
        for (std::size_t index = 1; index <= category.count; ++index, ++slot) {
            char key[16]{};
            std::snprintf(key, sizeof(key), "%s%u", category.prefix,
                          static_cast<unsigned>(index));
            std::array<std::uint8_t, 88> bytes{};
            const auto read = store.read(kDeleteBondNamespace, key, bytes.data(), bytes.size());
            if (read.status) { store.wipe(bytes.data(), bytes.size()); return read.status; }
            if (read.present) {
                const auto identity = deletion_identity(bytes.data() + category.identity_offset);
                const auto alias = deletion_identity(bytes.data());
                const bool ours = same_identity(identity, target);
                const bool alias_overlap = category.identity_offset == 7 &&
                    !ours && same_identity(alias, target);
                if (read.size != category.size || !valid_identity(identity) || alias_overlap) {
                    store.wipe(bytes.data(), bytes.size()); return kDeleteInvalid;
                }
                selected[slot] = ours;
            }
            store.wipe(bytes.data(), bytes.size());
        }
    }
    return 0;
}

template <typename Store>
int verify_durable_target_absent(Store &store, const StoreIdentity &target) {
    std::array<bool, 27> selected{};
    const int status = target_delete_scan(store, target, selected);
    if (status) return status;
    for (const bool present : selected) if (present) return kDeleteInvalid;
    return store.verify_schema_absent(target);
}

template <typename Store>
int complete_durable_target(Store &store, const StoreIdentity &target) {
    std::array<bool, 27> selected{};
    int status = target_delete_scan(store, target, selected);
    if (status) return status;
    std::size_t slot = 0;
    for (const auto &category : kDeleteCategories) {
        for (std::size_t index = 1; index <= category.count; ++index, ++slot) {
            if (!selected[slot]) continue;
            char key[16]{};
            std::snprintf(key, sizeof(key), "%s%u", category.prefix,
                          static_cast<unsigned>(index));
            status = store.erase(kDeleteBondNamespace, key);
            if (status) return status;
        }
    }
    status = store.delete_schema(target);
    return status ? status : verify_durable_target_absent(store, target);
}

template <typename Store>
int finish_delete_intent(Store &store, const StoreIdentity &target) {
    StoreIdentity pending{}; bool present = false;
    int status = read_delete_intent(store, pending, present);
    if (status || !present || !same_identity(pending, target)) return status ? status : kDeleteInvalid;
    status = verify_durable_target_absent(store, target);
    if (status) return status;
    status = store.erase(kDeleteNamespace, kDeleteKey);
    if (status) return status;
    status = read_delete_intent(store, pending, present);
    return status ? status : present ? kDeleteInvalid : 0;
}

template <typename Store>
int resume_exact_deletion(Store &store) {
    StoreIdentity target{}; bool present = false;
    int status = read_delete_intent(store, target, present);
    if (status || !present) return status;
    status = complete_durable_target(store, target);
    return status ? status : finish_delete_intent(store, target);
}

template <typename Store, typename RemoveRam, typename VerifyRam>
int run_journaled_removal(Store &store, const StoreIdentity &target,
                          RemoveRam remove_ram, VerifyRam verify_ram) {
    std::array<bool, 27> selected{};
    int status = target_delete_scan(store, target, selected);
    if (status) return status;
    status = begin_delete_intent(store, target);
    if (status) return status;
    status = remove_ram();
    if (status) return status;
    status = complete_durable_target(store, target);
    if (status) return status;
    status = verify_ram();
    return status ? status : finish_delete_intent(store, target);
}

}  // namespace ble_transport::detail
