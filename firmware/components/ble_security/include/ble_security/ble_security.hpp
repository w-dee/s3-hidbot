#pragma once

#include <atomic>
#include <cstdint>

#include "ble_lifecycle/ble_lifecycle.hpp"

namespace ble_security {

static_assert(std::atomic<std::uint32_t>::is_always_lock_free,
              "BLE callback coordination requires lock-free 32-bit atomics");
static_assert(std::atomic<std::uint16_t>::is_always_lock_free,
              "BLE callback coordination requires lock-free 16-bit atomics");
static_assert(std::atomic_bool::is_always_lock_free,
              "BLE callback coordination requires lock-free bool atomics");

constexpr std::uint8_t kRequiredKeySize = 16;
constexpr std::uint8_t kBondCapacity = 3;

enum class StoreFailureKind : std::uint8_t {
    kNone,
    kCapacityFull,
    kWrite,
    kDelete,
};

struct StoredSecurityRecord {
    bool found = false;
    bool identity_matches = false;
    bool ltk_present = false;
    bool authenticated = false;
    bool secure_connections = false;
    std::uint8_t key_size = 0;
};

struct PersistedSecurityEvidence {
    StoredSecurityRecord our{};
    StoredSecurityRecord peer{};
};

struct LinkSecurityEvidence {
    bool encrypted = false;
    bool authenticated = false;
    bool nimble_bonded = false;
    bool secure_connections = false;
    bool identity_resolved = false;
    std::uint8_t key_size = 0;
};

struct Snapshot {
    ble_lifecycle::Generation generation = 0;
    std::uint16_t connection_handle = ble_lifecycle::kNoConnection;
    bool connected = false;
    bool encrypted = false;
    bool authenticated = false;
    bool nimble_bonded = false;
    bool project_verified_bond_persisted = false;
    bool secure_connections = false;
    bool identity_resolved = false;
    bool store_healthy = true;
    bool lifecycle_healthy = false;
    bool coherent = false;
    std::uint8_t key_size = 0;
    StoreFailureKind last_store_failure = StoreFailureKind::kNone;
    std::int32_t last_store_status = 0;
};

// Callback-safe, fail-closed readiness inhibition. The serialized executor
// publishes one exact connection identity at a time. A callback may only set
// the inhibition token for that published identity; retirement or a new
// executor-owned connection epoch makes an old token irrelevant without a
// callback-side clear. Fatal persistent-store failure is global until reboot.
class ReadinessInhibit final {
  public:
    void begin_connection(ble_lifecycle::Generation generation,
                          std::uint16_t connection_handle);
    void retire_connection(ble_lifecycle::Generation generation,
                           std::uint16_t connection_handle);
    bool inhibit(ble_lifecycle::Generation generation,
                 std::uint16_t connection_handle,
                 bool persistent_store_unhealthy);
    bool inhibits(ble_lifecycle::Generation generation,
                  std::uint16_t connection_handle) const;
    bool persistent_failure_observed() const;

  private:
    std::atomic<ble_lifecycle::Generation> generation_{0};
    std::atomic<std::uint16_t> connection_handle_{
        ble_lifecycle::kNoConnection};
    std::atomic<std::uint32_t> authority_token_{0};
    std::atomic<std::uint32_t> inhibited_token_{0};
    std::atomic_bool persistent_store_unhealthy_{false};
    // Executor-owned; zero is reserved for an inactive authority.
    std::uint32_t next_authority_token_ = 1;
};

// Fixed-size, zero-allocation compound security state. The serialized HID
// control executor is its sole runtime writer; concurrent status/HID readers
// obtain a bounded coherent snapshot through the sequence counter. The
// sequence counter is a single-writer/read-many mechanism, not multi-writer
// synchronization. Callback-side immediate inhibition is kept separately in
// ReadinessInhibit.
class State final {
  public:
    void begin_connection(ble_lifecycle::Generation generation,
                          std::uint16_t connection_handle,
                          bool lifecycle_healthy = true);
    void retire_connection(ble_lifecycle::Generation generation,
                           std::uint16_t connection_handle);
    void apply_store_failure(ble_lifecycle::Generation generation,
                             std::uint16_t connection_handle,
                             StoreFailureKind kind, std::int32_t status);
    void apply_persistent_store_failure(StoreFailureKind kind,
                                        std::int32_t status);
    void apply_verification(ble_lifecycle::Generation generation,
                            std::uint16_t connection_handle,
                            LinkSecurityEvidence link,
                            PersistedSecurityEvidence persisted);
    void mark_lifecycle_unhealthy(ble_lifecycle::Generation generation);

    Snapshot snapshot() const;
    bool security_ready_for_hid(
        ble_lifecycle::Generation generation,
        std::uint16_t connection_handle) const;

    static bool persisted_bond_is_valid(
        const PersistedSecurityEvidence &persisted);

  private:
    enum Flag : std::uint32_t {
        kConnected = 1U << 0,
        kEncrypted = 1U << 1,
        kAuthenticated = 1U << 2,
        kNimbleBonded = 1U << 3,
        kPersisted = 1U << 4,
        kSecureConnections = 1U << 5,
        kIdentityResolved = 1U << 6,
        kStoreHealthy = 1U << 7,
        kLifecycleHealthy = 1U << 8,
    };

    void begin_write();
    void end_write();

    std::atomic<std::uint32_t> sequence_{0};
    std::atomic<ble_lifecycle::Generation> generation_{0};
    std::atomic<std::uint16_t> connection_handle_{ble_lifecycle::kNoConnection};
    std::atomic<std::uint32_t> flags_{kStoreHealthy};
    std::atomic<std::uint8_t> key_size_{0};
    std::atomic<StoreFailureKind> last_store_failure_{StoreFailureKind::kNone};
    std::atomic<std::int32_t> last_store_status_{0};
    std::atomic_bool persistent_store_healthy_{true};
};

}  // namespace ble_security
