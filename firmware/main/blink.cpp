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

static_assert(kHidInterfaceCount == 2);
static_assert(kKeyboardInterface != kMouseInterface);
static_assert(kKeyboardEndpoint != kMouseEndpoint);

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

    ESP_ERROR_CHECK(gpio_config(&configuration));
    ESP_LOGI(kLogTag, "S3-HIDBOT BLINK READY");

    tinyusb_config_t tinyusb_configuration = TINYUSB_DEFAULT_CONFIG(usb_event_handler);
    tinyusb_configuration.descriptor.device = nullptr;
    tinyusb_configuration.descriptor.full_speed_config = kHidConfigurationDescriptor;
    tinyusb_configuration.descriptor.string = kHidStringDescriptors;
    tinyusb_configuration.descriptor.string_count =
        sizeof(kHidStringDescriptors) / sizeof(kHidStringDescriptors[0]);
    ESP_ERROR_CHECK(tinyusb_driver_install(&tinyusb_configuration));
    ESP_LOGI(kLogTag, "S3-HIDBOT USB HID READY");

    while (true) {
        ESP_ERROR_CHECK(gpio_set_level(kOnboardLed, 1));
        vTaskDelay(kBlinkInterval);
        ESP_ERROR_CHECK(gpio_set_level(kOnboardLed, 0));
        vTaskDelay(kBlinkInterval);
    }
}
