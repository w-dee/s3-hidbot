#include <cassert>
#include <cstdint>
#include <vector>
#include "ble_lifecycle/stop_transaction.hpp"

using ble_lifecycle::StopStatus;
using ble_lifecycle::StopTransaction;

struct Operations {
    std::vector<unsigned> calls;
    unsigned fail_at = 0;
    unsigned expire_at = 0;
    StopTransaction *transaction = nullptr;
    StopTransaction::Id id = 0;
    bool step(unsigned number) {
        calls.push_back(number);
        if (expire_at == number) assert(transaction->expire(id));
        return fail_at != number;
    }
    bool host_stop() { return step(1); }
    bool await_host_exit() { return step(2); }
    void delete_host_task() { assert(step(3)); }
    bool deinitialize() { return step(4); }
    bool retire_timers() { return step(5); }
};

int main() {
    StopTransaction transaction;
    assert(transaction.initialization_allowed());
    const auto first = transaction.begin();
    assert(first != 0 && !transaction.initialization_allowed());
    assert(transaction.begin() == 0);
    assert(!transaction.consume(first));
    assert(!transaction.complete(first, StopStatus::kConsumed));
    Operations operations;
    const auto result = ble_lifecycle::run_stop_sequence(operations);
    assert(result == StopStatus::kStopped);
    assert((operations.calls == std::vector<unsigned>{1,2,3,4,5}));
    assert(transaction.complete(first, result));
    assert(!transaction.initialization_allowed()); // worker success is insufficient
    assert(!transaction.complete(first, StopStatus::kHostStopFailure));
    assert(!transaction.consume(first + 1));
    assert(transaction.consume(first));
    assert(transaction.initialization_allowed());
    const auto second = transaction.begin();
    assert(second > first);
    assert(!transaction.complete(first, StopStatus::kStopped));
    assert(!transaction.expire(first));
    assert(!transaction.consume(first));
    assert(transaction.status(second) == StopStatus::kRunning);
    assert(transaction.status(first) == StopStatus::kWrongOwner);
    assert(transaction.expire(second));
    assert(!transaction.complete(second, StopStatus::kStopped));
    assert(!transaction.consume(second));
    assert(transaction.begin() == 0 && !transaction.initialization_allowed());

    for (unsigned stage : {1U, 2U, 4U, 5U}) {
        StopTransaction owner;
        const auto id = owner.begin();
        Operations fail;
        fail.fail_at = stage;
        const auto failure = ble_lifecycle::run_stop_sequence(fail);
        const auto expected = stage == 1 ? StopStatus::kHostStopFailure
            : stage == 2 ? StopStatus::kHostExitFailure
            : stage == 4 ? StopStatus::kDeinitFailure : StopStatus::kTimerFailure;
        assert(failure == expected);
        assert(fail.calls.size() == stage && fail.calls.back() == stage);
        assert(owner.complete(id, failure));
        assert(!owner.consume(id) && !owner.initialization_allowed());
        assert(owner.begin() == 0);
    }
    for (unsigned stage = 1; stage <= 5; ++stage) {
        StopTransaction owner;
        const auto id = owner.begin();
        Operations late;
        late.expire_at = stage;
        late.transaction = &owner;
        late.id = id;
        const auto completed_late = ble_lifecycle::run_stop_sequence(late);
        assert(completed_late == StopStatus::kStopped);
        assert(!owner.complete(id, completed_late));
        assert(owner.status(id) == StopStatus::kExpired);
        assert(!owner.consume(id) && !owner.initialization_allowed());
    }
    // Deadline wins even when the worker finished but its proof was not yet
    // consumed by the owner; late polling cannot turn uncertainty into success.
    StopTransaction unconsumed;
    const auto id = unconsumed.begin();
    assert(unconsumed.complete(id, StopStatus::kStopped));
    assert(unconsumed.expire(id));
    assert(!unconsumed.consume(id));

    StopTransaction exhausted;
    exhausted.set_next_id_for_test(StopTransaction::kMaxId);
    const auto last = exhausted.begin();
    assert(last == StopTransaction::kMaxId);
    assert(exhausted.complete(last, StopStatus::kStopped));
    assert(exhausted.consume(last));
    assert(exhausted.begin() == 0);
    assert(!exhausted.complete(0, StopStatus::kStopped));
    assert(!exhausted.consume(0) && !exhausted.expire(0));
}
