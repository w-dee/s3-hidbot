#pragma once

#include <atomic>
#include <cstdint>

namespace ble_hid_service {

// BLE-local readiness evidence for the future routing phase. It never grants
// HID authority and has no notification side effect.
class SubscriptionState final {
  public:
    void observe(std::uint16_t attribute_handle,
                 std::uint16_t keyboard_value_handle,
                 std::uint16_t mouse_value_handle, bool enabled) {
        if (attribute_handle == keyboard_value_handle) {
            keyboard_.store(enabled, std::memory_order_release);
        } else if (attribute_handle == mouse_value_handle) {
            mouse_.store(enabled, std::memory_order_release);
        }
    }

    void clear() {
        keyboard_.store(false, std::memory_order_release);
        mouse_.store(false, std::memory_order_release);
    }

    bool keyboard() const {
        return keyboard_.load(std::memory_order_acquire);
    }
    bool mouse() const { return mouse_.load(std::memory_order_acquire); }

  private:
    std::atomic_bool keyboard_{false};
    std::atomic_bool mouse_{false};
};

}  // namespace ble_hid_service
