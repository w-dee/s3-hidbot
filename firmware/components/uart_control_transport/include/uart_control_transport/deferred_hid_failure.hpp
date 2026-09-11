#pragma once

#include "control_session/control_session.hpp"

namespace uart_control_transport {

// Fixed-capacity deferred-event storage. Callers provide synchronization.
// A failure for the current owner supersedes stale/internal maintenance, while
// a late stale or internal failure cannot displace current-owner maintenance.
class DeferredHidFailure {
  public:
    void publish(control_session::LocalOwnerId source_owner_id,
                 control_session::LocalOwnerId current_owner_id) {
        if (!pending_ || owner_id_ == source_owner_id ||
            (source_owner_id != 0 && source_owner_id == current_owner_id)) {
            owner_id_ = source_owner_id;
            pending_ = true;
        }
    }

    bool take(control_session::LocalOwnerId *source_owner_id) {
        if (source_owner_id != nullptr) *source_owner_id = 0;
        if (!pending_) return false;
        if (source_owner_id != nullptr) *source_owner_id = owner_id_;
        owner_id_ = 0;
        pending_ = false;
        return true;
    }

    void discard_if_not_current(
        control_session::LocalOwnerId current_owner_id) {
        if (pending_ && owner_id_ != current_owner_id) {
            owner_id_ = 0;
            pending_ = false;
        }
    }

  private:
    control_session::LocalOwnerId owner_id_ = 0;
    bool pending_ = false;
};

}  // namespace uart_control_transport
