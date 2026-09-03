#include <array>
#include <cassert>
#include <cstdint>

#include "ble_security/ble_security.hpp"
#include "persistent_store_recovery.hpp"
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

using ble_transport::detail::RecoveryPlanKind;
using ble_transport::detail::SchemaIdentitySet;
using ble_transport::detail::SchemaFirstRemovalStage;
using ble_transport::detail::SecurityIdentitySet;
using ble_transport::detail::StoreIdentity;

constexpr StoreIdentity identity(std::uint8_t suffix,
                                 std::uint8_t type = 0) {
    return {.type = type, .value = {suffix, 2, 3, 4, 5, 6}};
}

template <typename Set, typename... Identities>
constexpr Set identity_set(Identities... identities) {
    Set output{};
    ((output.identities[output.count++] = identities), ...);
    return output;
}

void orphan_recovery_and_restore_integrity_matrix() {
    const auto empty_security = identity_set<SecurityIdentitySet>();
    const auto empty_schema = identity_set<SchemaIdentitySet>();
    const auto a = identity(1);
    const auto b = identity(2, 1);
    const auto c = identity(3);

    // Orphan 1: no security and one schema produces one exact deletion.
    auto plan = ble_transport::detail::make_recovery_plan(
        empty_security, empty_security, empty_security, empty_security,
        identity_set<SchemaIdentitySet>(a));
    assert(plan.kind == RecoveryPlanKind::kReady);
    assert(plan.orphan_count == 1);
    assert(ble_transport::detail::same_identity(plan.orphans[0], a));

    // Orphan 2: an already-clean reboot is a mutation-free no-op.
    plan = ble_transport::detail::make_recovery_plan(
        empty_security, empty_security, empty_security, empty_security,
        empty_schema);
    assert(plan.kind == RecoveryPlanKind::kReady && plan.orphan_count == 0);

    const auto bond_a = identity_set<SecurityIdentitySet>(a);
    // Orphan 3 and 4: a two-sided bond is preserved with or without schema.
    plan = ble_transport::detail::make_recovery_plan(
        bond_a, bond_a, bond_a, bond_a, identity_set<SchemaIdentitySet>(a));
    assert(plan.kind == RecoveryPlanKind::kReady && plan.orphan_count == 0);
    plan = ble_transport::detail::make_recovery_plan(
        bond_a, bond_a, bond_a, bond_a, empty_schema);
    assert(plan.kind == RecoveryPlanKind::kReady && plan.orphan_count == 0);
    assert(State::persisted_bond_is_valid(valid_persisted()));

    // Orphan 5 and 6: either half-bond direction fails before mutation.
    plan = ble_transport::detail::make_recovery_plan(
        bond_a, empty_security, bond_a, empty_security,
        identity_set<SchemaIdentitySet>(a));
    assert(plan.kind == RecoveryPlanKind::kStorageFailure);
    plan = ble_transport::detail::make_recovery_plan(
        empty_security, bond_a, empty_security, bond_a,
        identity_set<SchemaIdentitySet>(a));
    assert(plan.kind == RecoveryPlanKind::kStorageFailure);

    // Orphan 7: preserve two live peers and sort only proven orphans.
    const auto bonds = identity_set<SecurityIdentitySet>(a, b);
    plan = ble_transport::detail::make_recovery_plan(
        bonds, bonds, bonds, bonds,
        identity_set<SchemaIdentitySet>(c, b, a));
    assert(plan.kind == RecoveryPlanKind::kReady && plan.orphan_count == 1);
    assert(ble_transport::detail::same_identity(plan.orphans[0], c));

    // Orphan 8: a verified-delete failure stops the recovery executor and is
    // never reported as success.
    plan = ble_transport::detail::make_recovery_plan(
        empty_security, empty_security, empty_security, empty_security,
        identity_set<SchemaIdentitySet>(c, a));
    int recovery_delete_calls = 0;
    const auto failed_recovery = ble_transport::detail::run_orphan_recovery(
        plan, [&](const StoreIdentity &) {
            ++recovery_delete_calls;
            return 19;
        });
    assert(failed_recovery.status == 19 && failed_recovery.completed == 0);
    assert(recovery_delete_calls == 1);

    // Orphan 9: canonical parsing rejects malformed, uppercase,
    // wrong-length, invalid-type, and ambiguous zero identities.
    StoreIdentity parsed{};
    assert(ble_transport::detail::parse_canonical_schema_key(
        "r00010203040506", parsed));
    assert(ble_transport::detail::same_identity(parsed, a));
    for (const char *key : {"r0001020304050", "r000102030405060",
                            "r0001020304050A", "r02010203040506",
                            "r00000000000000"}) {
        assert(!ble_transport::detail::parse_canonical_schema_key(key,
                                                                  parsed));
    }
    auto malformed = identity_set<SchemaIdentitySet>(a);
    malformed.trustworthy = false;
    plan = ble_transport::detail::make_recovery_plan(
        empty_security, empty_security, empty_security, empty_security,
        malformed);
    assert(plan.kind == RecoveryPlanKind::kStorageFailure);
    auto duplicate = identity_set<SchemaIdentitySet>(a, a);
    plan = ble_transport::detail::make_recovery_plan(
        empty_security, empty_security, empty_security, empty_security,
        duplicate);
    assert(plan.kind == RecoveryPlanKind::kStorageFailure);

    // Orphan 10: apply the first plan, then model the next boot as clean.
    auto schemas = identity_set<SchemaIdentitySet>(a);
    plan = ble_transport::detail::make_recovery_plan(
        empty_security, empty_security, empty_security, empty_security,
        schemas);
    assert(plan.orphan_count == 1);
    schemas.count = 0;
    const auto second_boot = ble_transport::detail::make_recovery_plan(
        empty_security, empty_security, empty_security, empty_security,
        schemas);
    assert(second_boot.kind == RecoveryPlanKind::kReady);
    assert(second_boot.orphan_count == 0);
    recovery_delete_calls = 0;
    const auto clean_execution = ble_transport::detail::run_orphan_recovery(
        second_boot, [&](const StoreIdentity &) {
            ++recovery_delete_calls;
            return 0;
        });
    assert(clean_execution.status == 0 && clean_execution.completed == 0);
    assert(recovery_delete_calls == 0);

    // Restore 1: order-independent persistent and restored sets agree.
    const auto persistent = identity_set<SecurityIdentitySet>(a, b);
    const auto restored = identity_set<SecurityIdentitySet>(b, a);
    plan = ble_transport::detail::make_recovery_plan(
        persistent, persistent, restored, restored, empty_schema);
    assert(plan.kind == RecoveryPlanKind::kReady);

    // Restore 2 and 3: either persistent/RAM mismatch direction fails.
    plan = ble_transport::detail::make_recovery_plan(
        bond_a, bond_a, empty_security, empty_security, empty_schema);
    assert(plan.kind == RecoveryPlanKind::kStorageFailure);
    plan = ble_transport::detail::make_recovery_plan(
        empty_security, empty_security, bond_a, bond_a, empty_schema);
    assert(plan.kind == RecoveryPlanKind::kStorageFailure);

    // Restore 4 and 5: invalid NVS type and size are executable layout
    // failures, then production maps either to an untrustworthy model.
    static_assert(ble_transport::detail::valid_security_record_layout(
        true, 64, 64));
    static_assert(!ble_transport::detail::valid_security_record_layout(
        false, 64, 64));
    static_assert(!ble_transport::detail::valid_security_record_layout(
        true, 63, 64));
    static_assert(ble_transport::detail::valid_schema_record_layout(true, 1));
    static_assert(!ble_transport::detail::valid_schema_record_layout(false,
                                                                      1));
    static_assert(!ble_transport::detail::valid_schema_record_layout(true,
                                                                      2));
    for (int fault = 0; fault < 2; ++fault) {
        auto untrustworthy = bond_a;
        untrustworthy.trustworthy = false;
        plan = ble_transport::detail::make_recovery_plan(
            untrustworthy, bond_a, bond_a, bond_a,
            identity_set<SchemaIdentitySet>(c));
        assert(plan.kind == RecoveryPlanKind::kStorageFailure);
    }

    // Restore 6: duplicate exact persistent identity is impossible/fatal.
    auto duplicate_security = identity_set<SecurityIdentitySet>(a, a);
    plan = ble_transport::detail::make_recovery_plan(
        duplicate_security, bond_a, bond_a, bond_a, empty_schema);
    assert(plan.kind == RecoveryPlanKind::kStorageFailure);

    // Restore 7: no count can silently exceed the configured capacity.
    auto overflow = bond_a;
    overflow.count = overflow.identities.size() + 1;
    plan = ble_transport::detail::make_recovery_plan(
        overflow, bond_a, bond_a, bond_a, empty_schema);
    assert(plan.kind == RecoveryPlanKind::kStorageFailure);
}

struct RemovalFixture {
    bool schema = true;
    bool our = true;
    bool peer = true;
    bool auxiliary = true;
    bool unrelated = true;
    bool fail_schema = false;
    bool fail_peer = false;
    bool fail_verify = false;
    std::array<int, 3> order{};
    std::size_t order_count = 0;

    std::int32_t delete_schema() {
        order[order_count++] = 1;
        if (fail_schema) return 11;
        schema = false;
        return 0;
    }
    std::int32_t delete_peer() {
        order[order_count++] = 2;
        if (fail_peer) return 12;
        our = false;
        peer = false;
        auxiliary = false;
        return 0;
    }
    std::int32_t verify() {
        order[order_count++] = 3;
        return !fail_verify && !schema && !our && !peer && !auxiliary &&
                       unrelated
                   ? 0
                   : 13;
    }
};

auto run_removal(RemovalFixture &fixture) {
    return ble_transport::detail::run_schema_first_removal(
        [&] { return fixture.delete_schema(); },
        [&] { return fixture.delete_peer(); },
        [&] { return fixture.verify(); });
}

void schema_first_removal_and_crash_cut_matrix() {
    // Cut 1 / Removal 1: before entry nothing changed; execution is 1,2,3.
    RemovalFixture untouched;
    assert(untouched.schema && untouched.our && untouched.peer);
    auto result = run_removal(untouched);
    assert(result.stage == SchemaFirstRemovalStage::kComplete);
    assert((untouched.order == std::array<int, 3>{1, 2, 3}));

    // Removal 2 / Orphan 8: companion failure prevents peer deletion.
    RemovalFixture schema_failure;
    schema_failure.fail_schema = true;
    result = run_removal(schema_failure);
    assert(result.stage == SchemaFirstRemovalStage::kSchemaDelete);
    assert(schema_failure.order_count == 1);
    assert(schema_failure.schema && schema_failure.our &&
           schema_failure.peer);

    // Cut 2, Removal 3: power loss or peer failure after schema deletion is
    // a two-sided authoritative bond in the conservative stale-schema state.
    RemovalFixture peer_failure;
    peer_failure.fail_peer = true;
    result = run_removal(peer_failure);
    assert(result.stage == SchemaFirstRemovalStage::kPeerDelete);
    assert(!peer_failure.schema && peer_failure.our && peer_failure.peer);
    assert(peer_failure.order_count == 2);
    assert(State::persisted_bond_is_valid(valid_persisted()));

    // Cut 3 / Removal 4: completed peer deletion has no schema or auxiliary
    // residue and leaves the unrelated peer unchanged.
    RemovalFixture success;
    result = run_removal(success);
    assert(result.stage == SchemaFirstRemovalStage::kComplete);
    assert(!success.schema && !success.our && !success.peer &&
           !success.auxiliary && success.unrelated);

    // A postcondition fault is never false success and cannot roll schema
    // back or select another peer.
    RemovalFixture postcondition_failure;
    postcondition_failure.fail_verify = true;
    result = run_removal(postcondition_failure);
    assert(result.stage == SchemaFirstRemovalStage::kPostcondition);
    assert(!postcondition_failure.schema &&
           postcondition_failure.unrelated);

    // Cut 4: reboot after Cut 2 preserves security authority while missing
    // schema is stale.  Recovery plans no deletion for that valid bond.
    const auto a = identity(9);
    const auto bond = identity_set<SecurityIdentitySet>(a);
    const auto no_schema = identity_set<SchemaIdentitySet>();
    const auto reboot_after_cut = ble_transport::detail::make_recovery_plan(
        bond, bond, bond, bond, no_schema);
    assert(reboot_after_cut.kind == RecoveryPlanKind::kReady);
    assert(reboot_after_cut.orphan_count == 0);

    // Cut 5: the historical old-order state converges to clean no-bond after
    // its exact schema is selected and removed.
    const auto no_security = identity_set<SecurityIdentitySet>();
    const auto historical = ble_transport::detail::make_recovery_plan(
        no_security, no_security, no_security, no_security,
        identity_set<SchemaIdentitySet>(a));
    assert(historical.kind == RecoveryPlanKind::kReady);
    assert(historical.orphan_count == 1);
}
}  // namespace

int main() {
    verification_matrix();
    readiness_and_fencing();
    immediate_inhibit_identity_fencing();
    capacity_is_fail_closed();
    delete_callback_exhaustion_is_not_a_storage_failure();
    orphan_recovery_and_restore_integrity_matrix();
    schema_first_removal_and_crash_cut_matrix();
    return 0;
}
