#include <array>
#include <cassert>
#include <cstdint>

#include "ble_security/ble_security.hpp"
#include "store_delete_result.hpp"

namespace {
using ble_security::LinkSecurityEvidence;
using ble_security::PersistedSecurityEvidence;
using ble_security::ReadinessInhibit;
using ble_security::State;
using ble_security::StoredSecurityRecord;

StoredSecurityRecord valid_record(bool sc = true) {
    return {.found = true, .identity_matches = true, .ltk_present = true,
            .authenticated = true, .secure_connections = sc,
            .key_size = ble_security::kRequiredKeySize};
}

LinkSecurityEvidence valid_link(bool sc = true) {
    return {.encrypted = true, .authenticated = true, .nimble_bonded = true,
            .secure_connections = sc, .identity_resolved = true,
            .key_size = ble_security::kRequiredKeySize};
}

PersistedSecurityEvidence valid_persisted(bool sc = true) {
    return {.our = valid_record(sc), .peer = valid_record(sc)};
}

void verification_matrix() {
    assert(State::persisted_bond_is_valid(valid_persisted(true)));
    assert(State::persisted_bond_is_valid(valid_persisted(false)));
    auto evidence = valid_persisted();
    evidence.our.found = false;
    assert(!State::persisted_bond_is_valid(evidence));
    evidence = valid_persisted();
    evidence.peer.found = false;
    assert(!State::persisted_bond_is_valid(evidence));
    evidence = valid_persisted();
    evidence.peer.identity_matches = false;
    assert(!State::persisted_bond_is_valid(evidence));
    evidence = valid_persisted();
    evidence.our.ltk_present = false;
    assert(!State::persisted_bond_is_valid(evidence));
    evidence = valid_persisted();
    evidence.peer.authenticated = false;
    assert(!State::persisted_bond_is_valid(evidence));
    evidence = valid_persisted();
    evidence.our.key_size = 15;
    assert(!State::persisted_bond_is_valid(evidence));
    evidence = valid_persisted();
    evidence.peer.secure_connections = false;
    assert(!State::persisted_bond_is_valid(evidence));
}

void readiness_and_fencing() {
    constexpr ble_lifecycle::Generation generation = 7;
    constexpr std::uint16_t handle = 41;
    State state;
    state.begin_connection(generation, handle);
    assert(!state.security_ready_for_hid(generation, handle));
    state.apply_verification(generation - 1, handle, valid_link(), valid_persisted());
    state.apply_verification(generation, handle + 1, valid_link(), valid_persisted());
    assert(!state.security_ready_for_hid(generation, handle));
    state.apply_verification(generation, handle, valid_link(), valid_persisted());
    assert(state.security_ready_for_hid(generation, handle));
    assert(!state.security_ready_for_hid(generation - 1, handle));
    assert(!state.security_ready_for_hid(generation, handle + 1));

    for (unsigned condition = 0; condition < 5; ++condition) {
        State weak;
        weak.begin_connection(generation, handle);
        auto link = valid_link();
        if (condition == 0) link.encrypted = false;
        if (condition == 1) link.authenticated = false;
        if (condition == 2) link.nimble_bonded = false;
        if (condition == 3) link.identity_resolved = false;
        if (condition == 4) link.key_size = 15;
        weak.apply_verification(generation, handle, link, valid_persisted());
        assert(!weak.security_ready_for_hid(generation, handle));
    }

    state.apply_store_failure(generation - 1, handle,
        ble_security::StoreFailureKind::kWrite, -1);
    state.apply_store_failure(generation, handle + 1,
        ble_security::StoreFailureKind::kWrite, -1);
    assert(state.security_ready_for_hid(generation, handle));
    state.apply_store_failure(generation, handle,
        ble_security::StoreFailureKind::kCapacityFull, -2);
    assert(!state.security_ready_for_hid(generation, handle));
    const auto failed = state.snapshot();
    assert(failed.coherent && !failed.store_healthy);
    assert(failed.last_store_failure == ble_security::StoreFailureKind::kCapacityFull);

    for (const auto kind : {ble_security::StoreFailureKind::kRead,
                            ble_security::StoreFailureKind::kWrite,
                            ble_security::StoreFailureKind::kDelete}) {
        State store_failure;
        store_failure.begin_connection(generation, handle);
        store_failure.apply_verification(generation, handle, valid_link(),
                                         valid_persisted());
        store_failure.apply_persistent_store_failure(kind, -3);
        const auto failed = store_failure.snapshot();
        assert(!failed.project_verified_bond_persisted);
        assert(!failed.store_healthy && !failed.lifecycle_healthy);
        assert(failed.last_store_failure == kind);
        assert(!store_failure.security_ready_for_hid(generation, handle));
        store_failure.retire_connection(generation, handle);
        assert(!store_failure.snapshot().connected);
        store_failure.begin_connection(generation + 1, handle + 1);
        assert(!store_failure.snapshot().store_healthy);
    }

    State lifecycle;
    lifecycle.begin_connection(generation, handle);
    lifecycle.apply_verification(generation, handle, valid_link(), valid_persisted());
    lifecycle.mark_lifecycle_unhealthy(generation);
    assert(!lifecycle.security_ready_for_hid(generation, handle));
    lifecycle.retire_connection(generation, handle);
    assert(!lifecycle.snapshot().connected);
}

void immediate_inhibit_identity_fencing() {
    constexpr ble_lifecycle::Generation generation_a = 17;
    constexpr ble_lifecycle::Generation generation_b = 18;
    constexpr std::uint16_t reused_handle = 73;
    ReadinessInhibit inhibit;

    inhibit.begin_connection(generation_a, reused_handle);
    assert(!inhibit.inhibits(generation_a, reused_handle));
    assert(!inhibit.inhibit(generation_a - 1, reused_handle, false));
    assert(!inhibit.inhibit(generation_a, reused_handle + 1, false));
    assert(!inhibit.inhibits(generation_a, reused_handle));
    assert(inhibit.inhibit(generation_a, reused_handle, false));
    assert(inhibit.inhibits(generation_a, reused_handle));

    inhibit.retire_connection(generation_a, reused_handle);
    inhibit.begin_connection(generation_b, reused_handle);
    assert(!inhibit.inhibits(generation_b, reused_handle));
    assert(!inhibit.inhibit(generation_a, reused_handle, false));
    assert(!inhibit.inhibits(generation_b, reused_handle));

    assert(inhibit.inhibit(generation_b, reused_handle, true));
    assert(inhibit.persistent_failure_observed());
    assert(inhibit.inhibits(generation_b, reused_handle));
    inhibit.retire_connection(generation_b, reused_handle);
    inhibit.begin_connection(generation_b + 1, reused_handle + 1);
    assert(inhibit.inhibits(generation_b + 1, reused_handle + 1));
}

class FixedIdentityStore {
  public:
    bool insert(std::uint8_t identity) {
        if (contains(identity)) return true;
        if (count_ == identities_.size()) { full_ = true; return false; }
        identities_[count_++] = identity;
        return true;
    }
    bool contains(std::uint8_t identity) const {
        for (std::uint8_t value : identities_) if (value == identity) return true;
        return false;
    }
    bool remove(std::uint8_t identity) {
        for (std::size_t index = 0; index < count_; ++index) {
            if (identities_[index] != identity) continue;
            for (std::size_t next = index + 1; next < count_; ++next) {
                identities_[next - 1] = identities_[next];
            }
            identities_[--count_] = 0;
            full_ = false;
            return true;
        }
        return false;
    }
    std::size_t count() const { return count_; }
    bool full() const { return full_; }
  private:
    std::array<std::uint8_t, ble_security::kBondCapacity> identities_{};
    std::size_t count_ = 0;
    bool full_ = false;
};

void capacity_is_fail_closed() {
    static_assert(ble_security::kBondCapacity == 3);
    FixedIdentityStore store;
    assert(store.insert(1) && store.insert(2) && store.insert(3));
    assert(!store.insert(4) && store.full());
    assert(store.contains(1) && store.contains(2) && store.contains(3));
    assert(!store.contains(4));
    assert(!store.remove(4));
    assert(store.count() == 3);
    assert(store.remove(2));
    assert(store.count() == 2 && store.contains(1) && store.contains(3));
    assert(!store.contains(2));
    assert(store.insert(4) && store.count() == 3);
    assert(store.contains(1) && store.contains(3) && store.contains(4));
    // A reboot model is a copy of persistent records, not a repopulation.
    const FixedIdentityStore rebooted = store;
    assert(!rebooted.contains(2));
}

void delete_callback_exhaustion_is_not_a_storage_failure() {
    using ble_transport::detail::StoreDeleteCallbackResult;
    using ble_transport::detail::classify_store_delete_callback_result;
    constexpr int kDeleted = 0;
    constexpr int kNotFound = 5;
    constexpr int kGenuineFailure = 17;

    static_assert(classify_store_delete_callback_result(kDeleted, kNotFound) ==
                  StoreDeleteCallbackResult::kDeleted);
    static_assert(classify_store_delete_callback_result(kNotFound, kNotFound) ==
                  StoreDeleteCallbackResult::kExhausted);
    static_assert(classify_store_delete_callback_result(kGenuineFailure,
                                                        kNotFound) ==
                  StoreDeleteCallbackResult::kFailure);

    const auto helper_delete_all = [](const auto &callback_results,
                                      bool &fatal_latched) {
        for (const int status : callback_results) {
            const auto result = classify_store_delete_callback_result(
                status, kNotFound);
            if (result == StoreDeleteCallbackResult::kFailure) {
                fatal_latched = true;
                return false;
            }
            if (result == StoreDeleteCallbackResult::kExhausted) {
                return true;
            }
        }
        return false;
    };

    bool fatal_latched = false;
    assert(helper_delete_all(std::array{kDeleted, kDeleted, kNotFound},
                             fatal_latched));
    assert(!fatal_latched);

    fatal_latched = false;
    assert(helper_delete_all(std::array{kNotFound}, fatal_latched));
    assert(!fatal_latched);

    fatal_latched = false;
    assert(!helper_delete_all(std::array{kDeleted, kGenuineFailure},
                              fatal_latched));
    assert(fatal_latched);
}
}  // namespace

int main() {
    verification_matrix();
    readiness_and_fencing();
    immediate_inhibit_identity_fencing();
    capacity_is_fail_closed();
    delete_callback_exhaustion_is_not_a_storage_failure();
    return 0;
}
