#pragma once

#include <cstdint>

namespace ble_transport::detail {

enum class StoreDeleteCallbackResult : std::uint8_t {
    kDeleted,
    kExhausted,
    kFailure,
};

// The NimBLE delete-all helper deliberately calls the store callback until
// it returns its not-found status.  Keep that status visible to NimBLE while
// distinguishing normal iteration exhaustion from a genuine store failure.
constexpr StoreDeleteCallbackResult classify_store_delete_callback_result(
    int status, int not_found_status) {
    if (status == 0) {
        return StoreDeleteCallbackResult::kDeleted;
    }
    return status == not_found_status
               ? StoreDeleteCallbackResult::kExhausted
               : StoreDeleteCallbackResult::kFailure;
}

}  // namespace ble_transport::detail
