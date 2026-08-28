#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "class/hid/hid_device.h"

namespace {

constexpr gpio_num_t kOnboardLed = GPIO_NUM_2;
constexpr gpio_num_t kBootButton = GPIO_NUM_0;
constexpr TickType_t kBlinkInterval = pdMS_TO_TICKS(1000);
constexpr TickType_t kButtonPollInterval = pdMS_TO_TICKS(10);
constexpr uint8_t kButtonDebounceSamples = 3;
constexpr char kLogTag[] = "s3_hidbot";

constexpr uint8_t kKeyboardInterface = 0;
constexpr uint8_t kMouseInterface = 1;
constexpr uint8_t kHidInterfaceCount = 2;
constexpr uint8_t kKeyboardEndpoint = 0x81;
constexpr uint8_t kMouseEndpoint = 0x82;
constexpr uint8_t kHidEndpointSize = 16;
constexpr uint8_t kHidPollingIntervalMs = 10;
constexpr uint8_t kKeyboardStringIndex = 4;
constexpr uint8_t kMouseStringIndex = 5;

static_assert(kHidInterfaceCount == 2);
static_assert(kKeyboardInterface != kMouseInterface);
static_assert(kKeyboardEndpoint != kMouseEndpoint);
static_assert(kButtonDebounceSamples >= 2);

enum class ButtonState : uint8_t {
    kReleased,
    kDebouncingPress,
    kPressed,
    kDebouncingRelease,
};

class ButtonDebouncer {
  public:
    constexpr bool update(bool pressed) {
        switch (state_) {
            case ButtonState::kReleased:
                if (pressed) {
                    state_ = ButtonState::kDebouncingPress;
                    consecutive_samples_ = 1;
                }
                break;
            case ButtonState::kDebouncingPress:
                if (!pressed) {
                    state_ = ButtonState::kReleased;
                    consecutive_samples_ = 0;
                } else if (++consecutive_samples_ >= kButtonDebounceSamples) {
                    state_ = ButtonState::kPressed;
                    consecutive_samples_ = 0;
                    return true;
                }
                break;
            case ButtonState::kPressed:
                if (!pressed) {
                    state_ = ButtonState::kDebouncingRelease;
                    consecutive_samples_ = 1;
                }
                break;
            case ButtonState::kDebouncingRelease:
                if (pressed) {
                    state_ = ButtonState::kPressed;
                    consecutive_samples_ = 0;
                } else if (++consecutive_samples_ >= kButtonDebounceSamples) {
                    state_ = ButtonState::kReleased;
                    consecutive_samples_ = 0;
                }
                break;
        }
        return false;
    }

  private:
    ButtonState state_ = ButtonState::kReleased;
    uint8_t consecutive_samples_ = 0;
};

constexpr bool button_debouncer_self_test() {
    ButtonDebouncer button;

    // Initial bounce does not create an edge; three stable pressed samples do.
    if (button.update(true) || button.update(false) || button.update(true) ||
        button.update(true) || !button.update(true)) {
        return false;
    }
    // A held button cannot create another edge.
    if (button.update(true) || button.update(true)) {
        return false;
    }
    // Release bounce does not re-arm until three stable released samples.
    if (button.update(false) || button.update(true) || button.update(false) ||
        button.update(false) || button.update(false)) {
        return false;
    }
    // A new debounced press creates exactly one new edge.
    return !button.update(true) && !button.update(true) && button.update(true) &&
           !button.update(true);
}

static_assert(button_debouncer_self_test());

const uint8_t kKeyboardReportDescriptor[] = {
    TUD_HID_REPORT_DESC_KEYBOARD(),
};

const uint8_t kMouseReportDescriptor[] = {
    TUD_HID_REPORT_DESC_MOUSE(),
};

const char kLanguageId[] = {0x09, 0x04};
const char kManufacturerString[] = "s3-hidbot";
const char kProductString[] = "s3-hidbot";
const char kSerialString[] = "bring-up";
const char kKeyboardString[] = "s3-hidbot Keyboard";
const char kMouseString[] = "s3-hidbot Mouse";
const char *kHidStringDescriptors[] = {
    kLanguageId,
    kManufacturerString,
    kProductString,
    kSerialString,
    kKeyboardString,
    kMouseString,
};

constexpr size_t kConfigurationDescriptorLength =
    TUD_CONFIG_DESC_LEN + (kHidInterfaceCount * TUD_HID_DESC_LEN);

const uint8_t kHidConfigurationDescriptor[] = {
    TUD_CONFIG_DESCRIPTOR(1, kHidInterfaceCount, 0, kConfigurationDescriptorLength, 0, 100),
    TUD_HID_DESCRIPTOR(kKeyboardInterface, kKeyboardStringIndex, HID_ITF_PROTOCOL_KEYBOARD,
                       sizeof(kKeyboardReportDescriptor), kKeyboardEndpoint, kHidEndpointSize,
                       kHidPollingIntervalMs),
    TUD_HID_DESCRIPTOR(kMouseInterface, kMouseStringIndex, HID_ITF_PROTOCOL_MOUSE,
                       sizeof(kMouseReportDescriptor), kMouseEndpoint, kHidEndpointSize,
                       kHidPollingIntervalMs),
};

static_assert(sizeof(kHidConfigurationDescriptor) == kConfigurationDescriptorLength);

void usb_event_handler(tinyusb_event_t *event, void *argument) {
    (void)argument;

    switch (event->id) {
        case TINYUSB_EVENT_ATTACHED:
            ESP_LOGI(kLogTag, "USB HID mounted");
            break;
        case TINYUSB_EVENT_DETACHED:
            ESP_LOGI(kLogTag, "USB HID unmounted");
            break;
        case TINYUSB_EVENT_SUSPENDED:
            ESP_LOGI(kLogTag, "USB HID suspended (remote wakeup: %s)",
                     event->suspended.remote_wakeup ? "enabled" : "disabled");
            break;
        case TINYUSB_EVENT_RESUMED:
            ESP_LOGI(kLogTag, "USB HID resumed");
            break;
        default:
            break;
    }
}

}  // namespace

extern "C" uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance) {
    switch (instance) {
        case kKeyboardInterface:
            return kKeyboardReportDescriptor;
        case kMouseInterface:
            return kMouseReportDescriptor;
        default:
            return nullptr;
    }
}

extern "C" uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                                             hid_report_type_t report_type, uint8_t *buffer,
                                             uint16_t request_length) {
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)request_length;
    return 0;
}

extern "C" void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                                        hid_report_type_t report_type, uint8_t const *buffer,
                                        uint16_t buffer_size) {
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)buffer_size;
}

extern "C" void app_main() {
    const gpio_config_t configuration = {
        .pin_bit_mask = 1ULL << kOnboardLed,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    const gpio_config_t button_configuration = {
        .pin_bit_mask = 1ULL << kBootButton,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ESP_ERROR_CHECK(gpio_config(&configuration));
    ESP_ERROR_CHECK(gpio_config(&button_configuration));
    ESP_LOGI(kLogTag, "S3-HIDBOT BLINK READY");
    ESP_LOGI(kLogTag, "BOOT button GPIO0 is active-low; hold during reset selects download boot");

    tinyusb_config_t tinyusb_configuration = TINYUSB_DEFAULT_CONFIG(usb_event_handler);
    tinyusb_configuration.descriptor.device = nullptr;
    tinyusb_configuration.descriptor.full_speed_config = kHidConfigurationDescriptor;
    tinyusb_configuration.descriptor.string = kHidStringDescriptors;
    tinyusb_configuration.descriptor.string_count =
        sizeof(kHidStringDescriptors) / sizeof(kHidStringDescriptors[0]);
    ESP_ERROR_CHECK(tinyusb_driver_install(&tinyusb_configuration));
    ESP_LOGI(kLogTag, "S3-HIDBOT USB HID READY");
    ESP_LOGI(kLogTag, "S3-HIDBOT MOUSE TEST READY");

    ButtonDebouncer button;
    TickType_t last_led_toggle = xTaskGetTickCount();
    bool led_on = true;
    ESP_ERROR_CHECK(gpio_set_level(kOnboardLed, 1));
    while (true) {
        const bool button_pressed = gpio_get_level(kBootButton) == 0;
        if (button.update(button_pressed)) {
            if (tud_hid_n_ready(kMouseInterface)) {
                const bool sent = tud_hid_n_mouse_report(kMouseInterface, 0, 0, 10, 0, 0, 0);
                if (sent) {
                    ESP_LOGI(kLogTag, "MOUSE TEST REPORT SENT");
                } else {
                    ESP_LOGI(kLogTag, "MOUSE TEST REPORT DROPPED SEND FAILURE");
                }
            } else {
                ESP_LOGI(kLogTag, "MOUSE TEST REPORT DROPPED NOT READY");
            }
        }

        const TickType_t now = xTaskGetTickCount();
        if (now - last_led_toggle >= kBlinkInterval) {
            led_on = !led_on;
            ESP_ERROR_CHECK(gpio_set_level(kOnboardLed, led_on ? 1 : 0));
            last_led_toggle = now;
        }
        vTaskDelay(kButtonPollInterval);
    }
}
