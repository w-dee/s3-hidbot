#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {

constexpr gpio_num_t kOnboardLed = GPIO_NUM_2;
constexpr TickType_t kBlinkInterval = pdMS_TO_TICKS(1000);
constexpr char kLogTag[] = "s3_hidbot";

}  // namespace

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

    while (true) {
        ESP_ERROR_CHECK(gpio_set_level(kOnboardLed, 1));
        vTaskDelay(kBlinkInterval);
        ESP_ERROR_CHECK(gpio_set_level(kOnboardLed, 0));
        vTaskDelay(kBlinkInterval);
    }
}
