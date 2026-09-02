#include "ble_security/ble_security.hpp"

namespace ble_security {
namespace {

bool record_is_valid(const StoredSecurityRecord &record) {
    return record.found && record.identity_matches && record.ltk_present &&
           record.authenticated && record.key_size == kRequiredKeySize;
}

}  // namespace

void State::begin_write() {
    sequence_.fetch_add(1, std::memory_order_acq_rel);
}

void State::end_write() {
    sequence_.fetch_add(1, std::memory_order_release);
}

void State::begin_connection(ble_lifecycle::Generation generation,
                             std::uint16_t connection_handle,
                             bool lifecycle_healthy) {
    begin_write();
    generation_.store(generation, std::memory_order_relaxed);
    connection_handle_.store(connection_handle, std::memory_order_relaxed);
    std::uint32_t flags = kConnected;
    if (persistent_store_healthy_.load(std::memory_order_relaxed)) {
        flags |= kStoreHealthy;
    }
    if (lifecycle_healthy) {
        flags |= kLifecycleHealthy;
    }
    flags_.store(flags, std::memory_order_relaxed);
    key_size_.store(0, std::memory_order_relaxed);
    last_store_failure_.store(StoreFailureKind::kNone,
                              std::memory_order_relaxed);
    last_store_status_.store(0, std::memory_order_relaxed);
    end_write();
}

void State::retire_connection(ble_lifecycle::Generation generation,
                              std::uint16_t connection_handle) {
    const Snapshot current = snapshot();
    if (!current.coherent || current.generation != generation ||
        current.connection_handle != connection_handle) {
        return;
    }
    begin_write();
    connection_handle_.store(ble_lifecycle::kNoConnection,
                             std::memory_order_relaxed);
    flags_.store(persistent_store_healthy_.load(std::memory_order_relaxed)
                     ? static_cast<std::uint32_t>(kStoreHealthy)
                     : std::uint32_t{0},
                 std::memory_order_relaxed);
    key_size_.store(0, std::memory_order_relaxed);
    end_write();
}

void State::observe_store_failure(ble_lifecycle::Generation generation,
                                  std::uint16_t connection_handle,
                                  StoreFailureKind kind, std::int32_t status,
                                  bool persistent_store_unhealthy) {
    const Snapshot current = snapshot();
    if (!current.coherent || current.generation != generation ||
        current.connection_handle != connection_handle) {
        return;
    }
    if (persistent_store_unhealthy) {
        persistent_store_healthy_.store(false, std::memory_order_release);
    }
    begin_write();
    std::uint32_t flags = flags_.load(std::memory_order_relaxed);
    flags &= ~(kPersisted | kStoreHealthy);
    flags_.store(flags, std::memory_order_relaxed);
    last_store_failure_.store(kind, std::memory_order_relaxed);
    last_store_status_.store(status, std::memory_order_relaxed);
    end_write();
}

void State::apply_verification(ble_lifecycle::Generation generation,
                               std::uint16_t connection_handle,
                               LinkSecurityEvidence link,
                               PersistedSecurityEvidence persisted) {
    const Snapshot current = snapshot();
    if (!current.coherent || !current.connected ||
        current.generation != generation ||
        current.connection_handle != connection_handle) {
        return;
    }
    begin_write();
    std::uint32_t flags = kConnected;
    if (link.encrypted) {
        flags |= kEncrypted;
    }
    if (link.authenticated) {
        flags |= kAuthenticated;
    }
    if (link.nimble_bonded) {
        flags |= kNimbleBonded;
    }
    if (link.secure_connections) {
        flags |= kSecureConnections;
    }
    if (link.identity_resolved) {
        flags |= kIdentityResolved;
    }
    if (current.store_healthy &&
        persistent_store_healthy_.load(std::memory_order_relaxed)) {
        flags |= kStoreHealthy;
        if (persisted_bond_is_valid(persisted)) {
            flags |= kPersisted;
        }
    }
    if (current.lifecycle_healthy) {
        flags |= kLifecycleHealthy;
    }
    flags_.store(flags, std::memory_order_relaxed);
    key_size_.store(link.key_size, std::memory_order_relaxed);
    end_write();
}

void State::mark_lifecycle_unhealthy(ble_lifecycle::Generation generation) {
    const Snapshot current = snapshot();
    if (!current.coherent || current.generation != generation) {
        return;
    }
    begin_write();
    flags_.fetch_and(~(kLifecycleHealthy | kPersisted),
                     std::memory_order_relaxed);
    end_write();
}

Snapshot State::snapshot() const {
    constexpr unsigned kAttempts = 3;
    for (unsigned attempt = 0; attempt < kAttempts; ++attempt) {
        const std::uint32_t before = sequence_.load(std::memory_order_acquire);
        if ((before & 1U) != 0) {
            continue;
        }
        Snapshot result{};
        result.generation = generation_.load(std::memory_order_relaxed);
        result.connection_handle =
            connection_handle_.load(std::memory_order_relaxed);
        const std::uint32_t flags = flags_.load(std::memory_order_relaxed);
        result.connected = (flags & kConnected) != 0;
        result.encrypted = (flags & kEncrypted) != 0;
        result.authenticated = (flags & kAuthenticated) != 0;
        result.nimble_bonded = (flags & kNimbleBonded) != 0;
        result.project_verified_bond_persisted = (flags & kPersisted) != 0;
        result.secure_connections = (flags & kSecureConnections) != 0;
        result.identity_resolved = (flags & kIdentityResolved) != 0;
        result.store_healthy = (flags & kStoreHealthy) != 0;
        result.lifecycle_healthy = (flags & kLifecycleHealthy) != 0;
        result.key_size = key_size_.load(std::memory_order_relaxed);
        result.last_store_failure =
            last_store_failure_.load(std::memory_order_relaxed);
        result.last_store_status =
            last_store_status_.load(std::memory_order_relaxed);
        const std::uint32_t after = sequence_.load(std::memory_order_acquire);
        if (before == after) {
            result.coherent = true;
            return result;
        }
    }
    return {};
}

bool State::security_ready_for_hid(
    ble_lifecycle::Generation generation,
    std::uint16_t connection_handle) const {
    const Snapshot value = snapshot();
    return value.coherent && value.generation == generation &&
           value.connection_handle == connection_handle && value.connected &&
           value.encrypted && value.authenticated && value.nimble_bonded &&
           value.project_verified_bond_persisted && value.identity_resolved &&
           value.key_size == kRequiredKeySize && value.store_healthy &&
           value.lifecycle_healthy;
}

bool State::persisted_bond_is_valid(
    const PersistedSecurityEvidence &persisted) {
    return record_is_valid(persisted.our) && record_is_valid(persisted.peer) &&
           persisted.our.secure_connections ==
               persisted.peer.secure_connections;
}

}  // namespace ble_security
