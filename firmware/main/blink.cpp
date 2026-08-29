#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "hid_runtime/hid_runtime.hpp"
#include "uart_control_transport/uart_control_transport.hpp"
#include "class/hid/hid_device.h"
#include "device/usbd.h"

namespace {

constexpr gpio_num_t kOnboardLed = GPIO_NUM_2;
constexpr TickType_t kMainLoopInterval = pdMS_TO_TICKS(10);
#if defined(CONFIG_S3_HIDBOT_BOOT_MOUSE_DIAGNOSTIC) && CONFIG_S3_HIDBOT_BOOT_MOUSE_DIAGNOSTIC
constexpr gpio_num_t kBootButton = GPIO_NUM_0;
constexpr uint8_t kButtonDebounceSamples = 3;
#endif
constexpr TickType_t kBlinkInterval = pdMS_TO_TICKS(1000);
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

hid_runtime::Runtime s_hid_runtime;

control_protocol::UsbStatus usb_status(void *) {
    const hid_runtime::StatusSnapshot status = s_hid_runtime.status_snapshot();
    return control_protocol::UsbStatus{status.mounted, status.suspended,
                                       status.keyboard_ready, status.mouse_ready};
}

control_session::AuthorityEpoch hid_authority_epoch(void *) {
    return s_hid_runtime.authority_epoch();
}

void request_hid_safety_release(void *) {
    s_hid_runtime.request_release_all();
}

control_protocol::ReleaseAllResult release_all(void *) {
    const hid_runtime::ReleaseAllResult result = s_hid_runtime.release_all();
    const auto convert = [](hid_runtime::ReleaseAllInterfaceState state) {
        return state == hid_runtime::ReleaseAllInterfaceState::kSubmitted
                   ? control_protocol::ReleaseAllInterfaceState::kSubmitted
                   : control_protocol::ReleaseAllInterfaceState::kAlreadyUp;
    };
    return control_protocol::ReleaseAllResult{
        .success = result.success,
        .authority_lost = result.authority_lost,
        .keyboard = convert(result.keyboard),
        .mouse = convert(result.mouse),
    };
}

control_protocol::KeyboardReportResult keyboard_report(
    void *, const control_protocol::KeyboardReportRequest &request) {
    const hid_runtime::KeyboardReportResult result =
        s_hid_runtime.keyboard_report(request.modifiers, request.keycodes);
    const auto failure = [](hid_runtime::KeyboardReportFailure value) {
        switch (value) {
            case hid_runtime::KeyboardReportFailure::kBusy:
                return control_protocol::KeyboardReportFailure::kBusy;
            case hid_runtime::KeyboardReportFailure::kSafetyPending:
                return control_protocol::KeyboardReportFailure::kSafetyPending;
            case hid_runtime::KeyboardReportFailure::kAuthorityLost:
                return control_protocol::KeyboardReportFailure::kAuthorityLost;
            case hid_runtime::KeyboardReportFailure::kNone:
                return control_protocol::KeyboardReportFailure::kNone;
            case hid_runtime::KeyboardReportFailure::kNotReady:
            default:
                return control_protocol::KeyboardReportFailure::kNotReady;
        }
    };
    return control_protocol::KeyboardReportResult{
        .success = result.success,
        .authority_lost = result.authority_lost,
        .state = result.state == hid_runtime::KeyboardReportState::kAlreadySet
                     ? control_protocol::KeyboardReportState::kAlreadySet
                     : control_protocol::KeyboardReportState::kSubmitted,
        .failure = failure(result.failure),
    };
}

control_protocol::MouseReportResult mouse_report(
    void *, const control_protocol::MouseReportRequest &request) {
    const hid_runtime::MouseReportResult result =
        s_hid_runtime.mouse_report(request.buttons, request.x, request.y,
                                   request.wheel, request.pan);
    const auto failure = [](hid_runtime::MouseReportFailure value) {
        switch (value) {
            case hid_runtime::MouseReportFailure::kBusy:
                return control_protocol::MouseReportFailure::kBusy;
            case hid_runtime::MouseReportFailure::kSafetyPending:
                return control_protocol::MouseReportFailure::kSafetyPending;
            case hid_runtime::MouseReportFailure::kAuthorityLost:
                return control_protocol::MouseReportFailure::kAuthorityLost;
            case hid_runtime::MouseReportFailure::kNone:
                return control_protocol::MouseReportFailure::kNone;
            case hid_runtime::MouseReportFailure::kNotReady:
            default:
                return control_protocol::MouseReportFailure::kNotReady;
        }
    };
    return control_protocol::MouseReportResult{
        .success = result.success,
        .authority_lost = result.authority_lost,
        .state = result.state == hid_runtime::MouseReportState::kAlreadySet
                     ? control_protocol::MouseReportState::kAlreadySet
                     : control_protocol::MouseReportState::kSubmitted,
        .failure = failure(result.failure),
    };
}

static_assert(kHidInterfaceCount == 2);
static_assert(kKeyboardInterface != kMouseInterface);
static_assert(kKeyboardEndpoint != kMouseEndpoint);
#if defined(CONFIG_S3_HIDBOT_BOOT_MOUSE_DIAGNOSTIC) && CONFIG_S3_HIDBOT_BOOT_MOUSE_DIAGNOSTIC
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
#endif

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
            s_hid_runtime.on_mount();
            uart_control_transport::on_hid_lifecycle_invalidation();
            ESP_LOGI(kLogTag, "USB HID mounted");
            break;
        case TINYUSB_EVENT_DETACHED:
            s_hid_runtime.on_unmount();
            uart_control_transport::on_hid_lifecycle_invalidation();
            ESP_LOGI(kLogTag, "USB HID unmounted");
            break;
        case TINYUSB_EVENT_SUSPENDED:
            s_hid_runtime.on_suspend();
            uart_control_transport::on_hid_lifecycle_invalidation();
            ESP_LOGI(kLogTag, "USB HID suspended (remote wakeup: %s)",
                     event->suspended.remote_wakeup ? "enabled" : "disabled");
            break;
        case TINYUSB_EVENT_RESUMED:
            s_hid_runtime.on_resume();
            uart_control_transport::on_hid_lifecycle_invalidation();
            ESP_LOGI(kLogTag, "USB HID resumed");
            break;
        default:
            break;
    }
}

}  // namespace

extern "C" void tud_sof_cb(uint32_t) {
    s_hid_runtime.service_sof();
}

extern "C" void tud_hid_report_complete_cb(uint8_t instance, uint8_t const *report,
                                            uint16_t length) {
    s_hid_runtime.on_report_complete(instance, report, length);
}

extern "C" void tud_hid_report_failed_cb(uint8_t instance, hid_report_type_t report_type,
                                           uint8_t const *report, uint16_t length) {
    // Host-to-device HID output reports (for example keyboard LEDs) are not
    // project-owned input state and must not trigger the input safety path.
    if (report_type == HID_REPORT_TYPE_INPUT) {
        if (s_hid_runtime.on_report_failed(instance, report, length)) {
            uart_control_transport::on_hid_safety_failure();
        }
    }
}

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
    s_hid_runtime.initialize();
    const gpio_config_t configuration = {
        .pin_bit_mask = 1ULL << kOnboardLed,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ESP_ERROR_CHECK(gpio_config(&configuration));
#if defined(CONFIG_S3_HIDBOT_BOOT_MOUSE_DIAGNOSTIC) && CONFIG_S3_HIDBOT_BOOT_MOUSE_DIAGNOSTIC
    const gpio_config_t button_configuration = {
        .pin_bit_mask = 1ULL << kBootButton,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&button_configuration));
    ESP_LOGI(kLogTag, "BOOT button GPIO0 is active-low; hold during reset selects download boot");
#endif
    ESP_LOGI(kLogTag, "S3-HIDBOT BLINK READY");

    tinyusb_config_t tinyusb_configuration = TINYUSB_DEFAULT_CONFIG(usb_event_handler);
    tinyusb_configuration.descriptor.device = nullptr;
    tinyusb_configuration.descriptor.full_speed_config = kHidConfigurationDescriptor;
    tinyusb_configuration.descriptor.string = kHidStringDescriptors;
    tinyusb_configuration.descriptor.string_count =
        sizeof(kHidStringDescriptors) / sizeof(kHidStringDescriptors[0]);
    ESP_ERROR_CHECK(tinyusb_driver_install(&tinyusb_configuration));
    ESP_LOGI(kLogTag, "S3-HIDBOT USB HID READY");
    const control_protocol::Config protocol_config = {
        .metadata = {
            .project = "s3-hidbot",
            .target = CONFIG_IDF_TARGET,
            .idf_version = IDF_VER,
        },
        .usb_status_provider = usb_status,
        .usb_status_context = nullptr,
        .authority_epoch_provider = hid_authority_epoch,
        .authority_epoch_context = nullptr,
        .output = nullptr,
        .output_context = nullptr,
        .now = nullptr,
        .now_context = nullptr,
        .lease_expired = request_hid_safety_release,
        .lease_expired_context = nullptr,
        .session_takeover = request_hid_safety_release,
        .session_takeover_context = nullptr,
        .hid_safety_failure = request_hid_safety_release,
        .hid_safety_failure_context = nullptr,
        .release_all_provider = release_all,
        .release_all_context = nullptr,
        .keyboard_report_provider = keyboard_report,
        .keyboard_report_context = nullptr,
        .mouse_report_provider = mouse_report,
        .mouse_report_context = nullptr,
    };
    ESP_ERROR_CHECK(uart_control_transport::start(&protocol_config));
#if defined(CONFIG_S3_HIDBOT_BOOT_MOUSE_DIAGNOSTIC) && CONFIG_S3_HIDBOT_BOOT_MOUSE_DIAGNOSTIC
    ESP_LOGI(kLogTag, "S3-HIDBOT MOUSE TEST READY");

    ButtonDebouncer button;
#endif
    TickType_t last_led_toggle = xTaskGetTickCount();
    bool led_on = true;
    ESP_ERROR_CHECK(gpio_set_level(kOnboardLed, 1));
    while (true) {
#if defined(CONFIG_S3_HIDBOT_BOOT_MOUSE_DIAGNOSTIC) && CONFIG_S3_HIDBOT_BOOT_MOUSE_DIAGNOSTIC
        const bool button_pressed = gpio_get_level(kBootButton) == 0;
        if (button.update(button_pressed)) {
            if (!s_hid_runtime.queue_mouse_report(0, 10, 0, 0, 0)) {
                ESP_LOGI(kLogTag, "MOUSE TEST REPORT DROPPED NOT READY");
            }
        }
        if (s_hid_runtime.take_report_sent(hid_runtime::Interface::kMouse)) {
            ESP_LOGI(kLogTag, "MOUSE TEST REPORT SENT");
        }
        if (s_hid_runtime.take_report_failed(hid_runtime::Interface::kMouse)) {
            ESP_LOGI(kLogTag, "MOUSE TEST REPORT DROPPED SEND FAILURE");
        }
#endif

        const TickType_t now = xTaskGetTickCount();
        if (now - last_led_toggle >= kBlinkInterval) {
            led_on = !led_on;
            ESP_ERROR_CHECK(gpio_set_level(kOnboardLed, led_on ? 1 : 0));
            last_led_toggle = now;
        }
        vTaskDelay(kMainLoopInterval);
    }
}
