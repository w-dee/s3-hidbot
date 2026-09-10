#include "bond_delete_transaction.hpp"

#include <algorithm>
#include <cassert>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace bond_delete_test {
using namespace ble_transport::detail;
using Key = std::pair<std::string, std::string>;
using Disk = std::map<Key, std::vector<std::uint8_t>>;
constexpr int kFailure = 77;

StoreIdentity identity(unsigned peer) {
    StoreIdentity id{.type = 1}; id.value[0] = peer; return id;
}
std::vector<std::uint8_t> record(unsigned peer, const DeleteCategory &c) {
    std::vector<std::uint8_t> bytes(c.size, 0);
    bytes[c.identity_offset] = 1; bytes[c.identity_offset + 1] = peer;
    if (c.size == 88) { bytes[10] = 16; bytes[40] = 1; bytes[80] = 3; }
    return bytes;
}
std::string slot_key(const DeleteCategory &c, unsigned n) {
    return std::string(c.prefix) + std::to_string(n);
}
std::string schema_key(const StoreIdentity &id) { return "peer" + std::to_string(id.value[0]); }

struct Store {
    Disk disk;
    int calls = 0, cut = -1;
    bool after = false;
    std::string ignore_erase;
    bool fail() { return calls++ == cut; }
    DeleteRead read(const char *ns, const char *key, std::uint8_t *bytes, std::size_t capacity) {
        if (fail()) return {.status = kFailure};
        const auto it = disk.find({ns, key});
        if (it == disk.end()) return {};
        if (it->second.size() > capacity) return {.status = kFailure};
        std::copy(it->second.begin(), it->second.end(), bytes);
        return {.present = true, .size = it->second.size()};
    }
    int write(const char *ns, const char *key, const std::uint8_t *bytes, std::size_t size) {
        const bool error = fail();
        if (error && !after) return kFailure;
        disk[{ns, key}] = {bytes, bytes + size};
        if (error || fail()) return kFailure; // mutation then commit error
        return 0;
    }
    int erase(const char *ns, const char *key) {
        const bool error = fail();
        if (error && !after) return kFailure;
        if (ignore_erase != key) disk.erase({ns, key});
        if (error || fail()) return kFailure;
        return 0;
    }
    int validate_layout() {
        if (fail()) return kFailure;
        for (const auto &[key, value] : disk) {
            (void)value;
            if (key.first != kDeleteBondNamespace) continue;
            bool recognized{};
            if (!deletion_slot(key.second.c_str(), recognized)) return kDeleteInvalid;
        }
        return 0;
    }
    int delete_schema(const StoreIdentity &id) {
        return erase("hid_schema", schema_key(id).c_str());
    }
    int verify_schema_absent(const StoreIdentity &id) {
        if (fail()) return kFailure;
        return disk.count({"hid_schema", schema_key(id)}) ? kDeleteInvalid : 0;
    }
    void wipe(void *bytes, std::size_t size) { std::memset(bytes, 0, size); }
};

Store fixture() {
    Store s;
    for (unsigned peer = 1; peer <= 3; ++peer) {
        for (const auto &category : kDeleteCategories)
            s.disk[{kDeleteBondNamespace, slot_key(category, peer)}] = record(peer, category);
        s.disk[{"hid_schema", schema_key(identity(peer))}] = {1};
    }
    s.disk[{"other", "untouched"}] = {11, 22, 33};
    return s;
}

bool target_present(const Key &key) {
    return key == Key{"hid_schema", "peer3"} ||
        (key.first == kDeleteBondNamespace && key.second.back() == '3');
}
Disk preserved(const Disk &disk) {
    Disk out;
    for (const auto &[key, value] : disk)
        if (!target_present(key) && key.first != kDeleteNamespace) out[key] = value;
    return out;
}
bool target_absent(const Disk &disk) {
    return std::none_of(disk.begin(), disk.end(), [](const auto &entry) { return target_present(entry.first); });
}

// Actual production transaction engine; RAM/store callback model reproduces
// schema-first, core-first NimBLE persistence and error-after-mutation cuts.
int remove(Store &s, const std::string &nvs_only = {}, int unexpected = -1) {
    return run_journaled_removal(s, identity(3), [&]() {
        const auto result = run_schema_first_removal([&]() -> std::int32_t {
            return s.delete_schema(identity(3));
        }, [&]() -> std::int32_t {
            for (const int category : {3, 4, 0, 1, 2}) {
                const auto key = slot_key(kDeleteCategories[category], 3);
                if (key == nvs_only) continue;
                const int status = s.erase(kDeleteBondNamespace, key.c_str());
                if (status) return status;
            }
            return 0;
        }, []() -> std::int32_t { return 0; });
        return result.status;
    }, [&]() {
        if (s.fail()) return kFailure;
        // The same normalization used by the backend: present is failure,
        // never the successful read status returned to its caller.
        for (int category = 0; category < 6; ++category) {
            const int status = absence_status(category == unexpected ? 0 : 5, 5, kDeleteInvalid);
            if (status) return status;
        }
        return 0;
    });
}

int run() {
    int cases = 0;
    const auto initial = fixture(); const auto keep = preserved(initial.disk);
    auto success = initial; assert(remove(success) == 0);
    assert(target_absent(success.disk) && preserved(success.disk) == keep);
    assert(!success.disk.count({kDeleteNamespace, kDeleteKey})); ++cases;
    const int operations = success.calls;
    for (bool after : {false, true}) for (int cut = 0; cut < operations; ++cut) {
        auto s = initial; s.cut = cut; s.after = after;
        const int status = remove(s);
        assert(status != 0); // public adapter must map this to BLE_BOND_STORAGE
        const bool recovery_required = status != 0;
        assert(recovery_required && preserved(s.disk) == keep);
        const bool pending = s.disk.count({kDeleteNamespace, kDeleteKey});
        if (!pending) assert(s.disk == initial.disk || target_absent(s.disk));
        // No same-boot retry: latch blocks list/remove until restart.
        s.cut = -1; s.calls = 0;
        assert(resume_exact_deletion(s) == 0);
        assert(preserved(s.disk) == keep);
        if (pending) assert(target_absent(s.disk));
        assert(!s.disk.count({kDeleteNamespace, kDeleteKey}));
        const auto recovered = s.disk;
        assert(resume_exact_deletion(s) == 0 && s.disk == recovered); ++cases;
    }
    for (int category = 0; category < 6; ++category) {
        auto s = initial; assert(remove(s, {}, category) != 0);
        assert(s.disk.count({kDeleteNamespace, kDeleteKey}));
        assert(resume_exact_deletion(s) == 0 && preserved(s.disk) == keep); ++cases;
    }
    for (int category = 0; category < 3; ++category) {
        const auto key = slot_key(kDeleteCategories[category], 3);
        auto s = initial; assert(remove(s, key) == 0); // NVS-only residual is actually cleaned
        assert(target_absent(s.disk)); ++cases;
        s = initial; s.ignore_erase = key;
        assert(remove(s, key) != 0 && s.disk.count({kDeleteBondNamespace, key}));
        assert(s.disk.count({kDeleteNamespace, kDeleteKey}));
        s.ignore_erase.clear(); assert(resume_exact_deletion(s) == 0);
        assert(target_absent(s.disk) && preserved(s.disk) == keep); ++cases;
    }
    // Inject every startup/resume I/O boundary as well, with and without an
    // erase becoming durable despite its error response. Repeated restart is
    // deterministic, narrowly targeted, and cannot modify unrelated peers.
    auto pending = initial; assert(begin_delete_intent(pending, identity(3)) == 0);
    auto resumed = pending; resumed.calls = 0; assert(resume_exact_deletion(resumed) == 0);
    for (bool after : {false, true}) for (int cut = 0; cut < resumed.calls; ++cut) {
        auto s = pending; s.calls = 0; s.cut = cut; s.after = after;
        assert(resume_exact_deletion(s) != 0 && preserved(s.disk) == keep);
        s.calls = 0; s.cut = -1; assert(resume_exact_deletion(s) == 0);
        assert(target_absent(s.disk) && preserved(s.disk) == keep); ++cases;
    }
    for (int malformed = 0; malformed < 5; ++malformed) {
        auto s = initial;
        switch (malformed) {
        case 0: s.disk[{kDeleteBondNamespace, "cccd_sec_3"}].resize(2); break;
        case 1: s.disk[{kDeleteBondNamespace, "cccd_sec_16"}] = record(3, kDeleteCategories[0]); break;
        case 2: s.disk[{kDeleteBondNamespace, "rpa_rec_1"}][0] = 1;
                s.disk[{kDeleteBondNamespace, "rpa_rec_1"}][1] = 3; break;
        case 3: s.disk[{kDeleteNamespace, kDeleteKey}] = {99}; break;
        case 4: { const auto intent = deletion_intent(identity(1));
                  s.disk[{kDeleteNamespace, kDeleteKey}] = {intent.begin(), intent.end()}; break; }
        }
        const auto before = s.disk; assert(remove(s) != 0 && s.disk == before); ++cases;
    }
    // An unjournaled half-bond is not swept by the new startup seam and the
    // existing production startup recovery rejects it as untrustworthy.
    auto half = initial; half.disk.erase({kDeleteBondNamespace, "our_sec_3"});
    const auto half_disk = half.disk; assert(resume_exact_deletion(half) == 0 && half.disk == half_disk);
    SecurityIdentitySet our{}, peer{};
    our.identities[0] = identity(1); our.count = 1; peer = our;
    peer.identities[1] = identity(3); peer.count = 2;
    assert(make_recovery_plan(our, peer, our, peer, {}).kind == RecoveryPlanKind::kStorageFailure); ++cases;
    assert(absence_status(0, 5, -1) != 0 && absence_status(5, 5, -1) == 0 && absence_status(77, 5, -1) == 77); ++cases;
    std::cout << "PASS: exact deletion transaction cases=" << cases
              << " normal_io_boundaries=" << operations
              << " reboot_io_boundaries=" << resumed.calls << '\n';
    return 0;
}
}  // namespace bond_delete_test
#ifndef BOND_DELETE_TEST_FIXTURE_ONLY
int main() { return bond_delete_test::run(); }
#endif
