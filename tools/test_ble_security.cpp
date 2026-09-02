#include <array>
#include <cassert>
#include <cstdint>

#include "ble_security/ble_security.hpp"

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
        ble_security::StoreFailureKind::kWrite, -1, false);
    state.apply_store_failure(generation, handle + 1,
        ble_security::StoreFailureKind::kWrite, -1, false);
    assert(state.security_ready_for_hid(generation, handle));
    state.apply_store_failure(generation, handle,
        ble_security::StoreFailureKind::kCapacityFull, -2, false);
    assert(!state.security_ready_for_hid(generation, handle));
    const auto failed = state.snapshot();
    assert(failed.coherent && !failed.store_healthy);
    assert(failed.last_store_failure == ble_security::StoreFailureKind::kCapacityFull);

    for (const auto kind : {ble_security::StoreFailureKind::kWrite,
                            ble_security::StoreFailureKind::kDelete}) {
        State store_failure;
        store_failure.begin_connection(generation, handle);
        store_failure.apply_verification(generation, handle, valid_link(),
                                         valid_persisted());
        store_failure.apply_store_failure(generation, handle, kind, -3, true);
        assert(!store_failure.snapshot().project_verified_bond_persisted);
        assert(!store_failure.security_ready_for_hid(generation, handle));
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
}
}  // namespace

int main() {
    verification_matrix();
    readiness_and_fencing();
    immediate_inhibit_identity_fencing();
    capacity_is_fail_closed();
    return 0;
}
