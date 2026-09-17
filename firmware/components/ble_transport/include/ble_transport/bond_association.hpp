#pragma once

#include <array>
#include <cstdint>

#include "ble_fixture_profile/ble_fixture_profile.hpp"
#include "ble_security/ble_security.hpp"

namespace ble_transport::detail {

using AssociationPeer = std::array<std::uint8_t, 7>;
using BondClass = ble_fixture_profile::BondAssociationClass;
using ProfileId = ble_fixture_profile::ProfileId;

enum class AssociationState : std::uint8_t { kMissing, kPending, kComplete, kInvalid };
struct AssociationRecord {
    AssociationState state = AssociationState::kMissing;
    BondClass bond_class = BondClass::kStrictComposite;
    bool operator==(const AssociationRecord &) const = default;
};

constexpr const ble_fixture_profile::ProfileDefinition *association_profile(BondClass value) {
    // Separate namespace: neither authentication bits nor profile numeric IDs
    // imply bond provenance. Extend this reviewed mapping with each profile.
    switch (value) {
        case BondClass::kStandaloneMouseJustWorksId7: return &ble_fixture_profile::kStandaloneMouseJustWorksId7;
        case BondClass::kStrictComposite: return &ble_fixture_profile::kStrictComposite;
        case BondClass::kMouseMetadata: return &ble_fixture_profile::kMouseMetadata;
        case BondClass::kStandaloneKeyboardLeds: return &ble_fixture_profile::kStandaloneKeyboardLeds;
        case BondClass::kStandaloneKeyboard: return &ble_fixture_profile::kStandaloneKeyboard;
        case BondClass::kStandaloneMouseJustWorks:
            return &ble_fixture_profile::kStandaloneMouseJustWorks;
    }
    return nullptr;
}

constexpr std::uint32_t encode_association(AssociationRecord record) {
    return 0xa5010000U | (static_cast<std::uint32_t>(record.bond_class) << 8U) |
           static_cast<std::uint32_t>(record.state);
}
constexpr AssociationRecord decode_association(std::uint32_t value) {
    const auto state = static_cast<AssociationState>(value & 0xffU);
    const auto bond_class = static_cast<BondClass>((value >> 8U) & 0xffU);
    if ((value & 0xffff0000U) != 0xa5010000U ||
        (state != AssociationState::kPending && state != AssociationState::kComplete) ||
        bond_class == BondClass::kStrictComposite || association_profile(bond_class) == nullptr)
        return {.state = AssociationState::kInvalid};
    return {state, bond_class};
}

enum class AssociationDecision : std::uint8_t {
    kAllowed, kIncompatible, kIncomplete, kStorageFailure
};
constexpr AssociationDecision association_compatibility(AssociationRecord record, BondClass selected) {
    if (association_profile(selected) == nullptr || record.state == AssociationState::kInvalid)
        return AssociationDecision::kStorageFailure;
    if (record.state == AssociationState::kPending) return AssociationDecision::kIncomplete;
    const auto retained = record.state == AssociationState::kMissing
        ? BondClass::kStrictComposite : record.bond_class;
    return retained == selected ? AssociationDecision::kAllowed : AssociationDecision::kIncompatible;
}

struct AssociationRead { int status = 0; AssociationRecord record{}; };
struct AssociationPair { int status = 0; ble_security::PersistedSecurityEvidence records{}; };

// Serialized by the NimBLE host. Only a nonreused host connection incarnation
// can own pending creation; disconnect/reset discards this volatile claim.
struct AssociationCreation {
    std::uint64_t connection = 0;
    AssociationPeer peer{};
    BondClass bond_class = BondClass::kStrictComposite;
    bool owns(std::uint64_t id, const AssociationPeer &key, BondClass selected) const {
        return id != 0 && connection == id && peer == key && bond_class == selected;
    }
    void retire() { *this = {}; }
};

// Store methods return real I/O status separately from incompatibility. A
// healthy incompatible record must never poison global inventory health.
template <typename Store>
AssociationDecision prepare_association(Store &store, AssociationCreation &claim,
    std::uint64_t connection, const AssociationPeer &peer, BondClass selected) {
    const auto read = store.read(peer);
    if (read.status || read.record.state == AssociationState::kInvalid || !association_profile(selected))
        return AssociationDecision::kStorageFailure;
    if (selected == BondClass::kStrictComposite)
        return association_compatibility(read.record, selected);
    if (claim.owns(connection, peer, selected)) {
        if (read.record.state == AssociationState::kComplete) return AssociationDecision::kIncompatible;
        return read.record.bond_class == selected && read.record.state == AssociationState::kPending
            ? AssociationDecision::kAllowed : AssociationDecision::kStorageFailure;
    }
    if (read.record.state != AssociationState::kMissing)
        return read.record.state == AssociationState::kPending
            ? AssociationDecision::kIncomplete : AssociationDecision::kIncompatible;
    if (connection == 0 || claim.connection != 0) return AssociationDecision::kIncomplete;
    const auto absent = store.prove_security_absent(peer);
    if (absent < 0) return AssociationDecision::kStorageFailure;
    if (!absent) return AssociationDecision::kIncompatible;
    const AssociationRecord pending{AssociationState::kPending, selected};
    if (store.write(peer, pending) != 0) return AssociationDecision::kStorageFailure;
    const auto verified = store.read(peer);
    if (verified.status || verified.record != pending) return AssociationDecision::kStorageFailure;
    claim = {connection, peer, selected};
    return AssociationDecision::kAllowed;
}

template <typename Store>
AssociationDecision complete_association(Store &store, const AssociationCreation &claim,
    std::uint64_t connection, const AssociationPeer &peer, BondClass selected) {
    if (selected == BondClass::kStrictComposite) return AssociationDecision::kAllowed;
    if (!claim.owns(connection, peer, selected)) return AssociationDecision::kIncomplete;
    const auto read = store.read(peer);
    if (read.status || read.record.bond_class != selected ||
        (read.record.state != AssociationState::kPending && read.record.state != AssociationState::kComplete))
        return AssociationDecision::kStorageFailure;
    const auto pair = store.durable_pair(peer);
    if (pair.status) return AssociationDecision::kStorageFailure;
    if (!pair.records.our.found || !pair.records.peer.found)
        return read.record.state == AssociationState::kPending
            ? AssociationDecision::kAllowed : AssociationDecision::kStorageFailure;
    const auto *profile = association_profile(selected);
    if (!profile || !ble_security::State{}.persisted_bond_is_valid(pair.records, profile->id))
        return AssociationDecision::kStorageFailure;
    if (read.record.state == AssociationState::kComplete) return AssociationDecision::kAllowed;
    const AssociationRecord complete{AssociationState::kComplete, selected};
    if (store.write(peer, complete) != 0) return AssociationDecision::kStorageFailure;
    const auto verified = store.read(peer);
    return !verified.status && verified.record == complete
        ? AssociationDecision::kAllowed : AssociationDecision::kStorageFailure;
}

} // namespace ble_transport::detail
