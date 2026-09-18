#include "ble_transport/bond_association.hpp"

#include <cassert>
#include <map>
#include <iostream>

using namespace ble_transport::detail;
using D = AssociationDecision;
using S = AssociationState;
constexpr BondClass mouse = BondClass::kStandaloneMouseJustWorks;
constexpr BondClass strict = BondClass::kStrictComposite;
constexpr BondClass sleep_profile_class = BondClass::kMouseSimulatedSleepV1;
constexpr BondClass host_security_class =
    BondClass::kMouseHostInitiatedSecurity;
constexpr AssociationPeer peer{1, 2, 3, 4, 5, 6, 7};
constexpr AssociationPeer other{1, 3, 3, 4, 5, 6, 7};

struct Store {
    std::map<AssociationPeer, AssociationRecord> records;
    ble_security::PersistedSecurityEvidence pair{};
    unsigned calls = 0, writes = 0;
    unsigned fail_at = 0;
    bool wrong_reread = false;
    bool failure() { return ++calls == fail_at; }
    AssociationRead read(const AssociationPeer &key) {
        if (failure()) return {.status = 77};
        return {.record = records.contains(key) ? records.at(key) : AssociationRecord{}};
    }
    int prove_security_absent(const AssociationPeer &) {
        return failure() ? -1 : pair.our.found || pair.peer.found ? 0 : 1;
    }
    int write(const AssociationPeer &key, AssociationRecord value) {
        if (failure()) return 77;
        records[key] = value; ++writes;
        if (wrong_reread) records[key].bond_class = strict;
        return 0;
    }
    AssociationPair durable_pair(const AssociationPeer &) {
        return failure() ? AssociationPair{.status = 77} : AssociationPair{.records = pair};
    }
};
ble_security::StoredSecurityRecord valid(bool authenticated = false) {
    return {.found=true, .identity_matches=true, .ltk_present=true,
        .authenticated=authenticated, .secure_connections=true, .key_size=16};
}

int main() {
    assert(encode_association({S::kPending, mouse}) == 0xa5010101U);
    assert(encode_association({S::kComplete, mouse}) == 0xa5010102U);
    for (const auto word : {0U, 0xa5010002U, 0xa5010100U, 0xa5010103U,
                           0xa501ff02U, 0xa5020102U, 0xa4010102U})
        assert(decode_association(word).state == S::kInvalid);
    assert(decode_association(0xa5010101U) == (AssociationRecord{S::kPending, mouse}));
    assert(decode_association(0xa5010102U) == (AssociationRecord{S::kComplete, mouse}));
    assert(encode_association({S::kComplete, sleep_profile_class}) == 0xa5010602U);
    assert(decode_association(0xa5010602U) ==
           (AssociationRecord{S::kComplete, sleep_profile_class}));
    assert(association_profile(sleep_profile_class) ==
           &ble_fixture_profile::kMouseSimulatedSleepV1);
    assert(encode_association({S::kComplete, host_security_class}) ==
           0xa5010702U);
    assert(decode_association(0xa5010702U) ==
           (AssociationRecord{S::kComplete, host_security_class}));
    assert(association_profile(host_security_class) ==
           &ble_fixture_profile::kMouseHostInitiatedSecurity);
    assert(association_compatibility({}, strict) == D::kAllowed);
    assert(association_compatibility({}, mouse) == D::kIncompatible);
    assert(association_compatibility({S::kComplete, mouse}, strict) == D::kIncompatible);
    assert(association_compatibility({S::kComplete, mouse}, mouse) == D::kAllowed);
    assert(association_compatibility({S::kComplete, mouse}, sleep_profile_class) ==
           D::kIncompatible);
    assert(association_compatibility({S::kComplete, mouse}, host_security_class) ==
           D::kIncompatible);
    assert(association_compatibility({S::kPending, mouse}, mouse) == D::kIncomplete);
    for (const bool our : {false,true}) {
        Store store; AssociationCreation claim;
        (our ? store.pair.our : store.pair.peer) = valid(true);
        assert(prepare_association(store, claim, 1, peer, mouse) == D::kIncompatible);
        assert(store.writes == 0 && claim.connection == 0);
    }
    // Each prepare failure leaves no usable record and cannot mint ownership.
    for (unsigned cut = 1; cut <= 4; ++cut) {
        Store store; AssociationCreation claim; store.fail_at = cut;
        assert(prepare_association(store, claim, 1, peer, mouse) == D::kStorageFailure);
        assert(claim.connection == 0);
        if (store.records.contains(peer)) assert(store.records.at(peer).state == S::kPending);
        store.fail_at = 0;
        if (store.records.contains(peer))
            assert(prepare_association(store, claim, 2, peer, mouse) == D::kIncomplete);
    }
    {
        Store store; AssociationCreation claim; store.wrong_reread = true;
        assert(prepare_association(store, claim, 1, peer, mouse) == D::kStorageFailure);
        assert(claim.connection == 0);
    }
    Store store; AssociationCreation claim;
    assert(prepare_association(store, claim, 1, peer, mouse) == D::kAllowed);
    assert(store.records.at(peer).state == S::kPending && store.writes == 1);
    assert(prepare_association(store, claim, 1, peer, mouse) == D::kAllowed);
    assert(prepare_association(store, claim, 2, peer, mouse) == D::kIncomplete);
    assert(prepare_association(store, claim, 1, other, mouse) == D::kIncomplete);
    assert(complete_association(store, claim, 2, peer, mouse) == D::kIncomplete);
    assert(complete_association(store, claim, 1, other, mouse) == D::kIncomplete);
    store.pair.our = valid();
    assert(complete_association(store, claim, 1, peer, mouse) == D::kAllowed);
    assert(store.records.at(peer).state == S::kPending);
    store.pair.peer = valid();
    for (unsigned invalid=0;invalid<5;++invalid) {
        auto bad=store;
        if(invalid==0) bad.pair.peer.authenticated=true;
        if(invalid==1) bad.pair.peer.key_size=15;
        if(invalid==2) bad.pair.peer.identity_matches=false;
        if(invalid==3) bad.pair.peer.ltk_present=false;
        if(invalid==4) bad.pair.peer.secure_connections=false;
        assert(complete_association(bad,claim,1,peer,mouse)==D::kStorageFailure);
        assert(bad.records.at(peer).state==S::kPending);
    }
    // Lost volatile claim (disconnect/reboot) never adopts even a complete pair.
    auto retired = claim; retired.retire();
    assert(prepare_association(store, retired, 2, peer, mouse) == D::kIncomplete);
    assert(complete_association(store, retired, 1, peer, mouse) == D::kIncomplete);
    for (unsigned cut=1;cut<=4;++cut) {
        auto interrupted=store; interrupted.calls=0;interrupted.fail_at=cut;
        assert(complete_association(interrupted,claim,1,peer,mouse)==D::kStorageFailure);
        // A failed reread can leave durable complete; it still cannot authorize
        // another live transaction to rewrite keys. Startup verifies the pair.
        interrupted.fail_at=0;
        assert(prepare_association(interrupted,retired,2,peer,mouse)!=D::kAllowed);
    }
    assert(complete_association(store,claim,1,peer,mouse)==D::kAllowed);
    assert(store.records.at(peer).state==S::kComplete && store.writes==2);
    assert(prepare_association(store,claim,1,peer,mouse)==D::kIncompatible);
    assert(prepare_association(store,retired,2,peer,strict)==D::kIncompatible);
    assert(prepare_association(store,retired,2,peer,mouse)==D::kIncompatible);
    assert(store.writes==2);
    // Preferred SC still permits a wholly coherent 16-byte Legacy pair.
    Store legacy; AssociationCreation legacy_claim;
    assert(prepare_association(legacy,legacy_claim,9,peer,mouse)==D::kAllowed);
    legacy.pair.our=legacy.pair.peer=valid();
    legacy.pair.our.secure_connections=legacy.pair.peer.secure_connections=false;
    assert(complete_association(legacy,legacy_claim,9,peer,mouse)==D::kAllowed);
    std::cout << "PASS: bond association exact creation, interruption, compatibility\n";
}
