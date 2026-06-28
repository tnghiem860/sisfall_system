#include <cstdint>
#include <cstring>
#include <cmath>

extern "C" {
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
}

#include "ai_model.h"

#define BUZZER_GPIO GPIO_NUM_13
static float dummy_input[150][3];

void init_buzzer() {
    gpio_reset_pin(BUZZER_GPIO);
    gpio_set_direction(BUZZER_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(BUZZER_GPIO, 0);
}

void signal_active() {
    gpio_set_level(BUZZER_GPIO, 1); vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(BUZZER_GPIO, 0); vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(BUZZER_GPIO, 1); vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(BUZZER_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(500));
}

void signal_idle() {
    gpio_set_level(BUZZER_GPIO, 1); vTaskDelay(pdMS_TO_TICKS(500));
    gpio_set_level(BUZZER_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(500));
}

extern "C" void app_main(void)
{
    init_buzzer();
    memset(dummy_input, 0, sizeof(dummy_input));
    ai_model_init();

    while (1) {
        // Chu kỳ tĩnh: 30s
        signal_idle();
        vTaskDelay(pdMS_TO_TICKS(30000));

        // Chu kỳ động: 30s chạy AI liên tục
        signal_active();
        long start_time = xTaskGetTickCount();
        while ((xTaskGetTickCount() - start_time) < pdMS_TO_TICKS(30000)) {
            ai_result_t dummy_res;
            ai_model_run(dummy_input, &dummy_res);
            vTaskDelay(1); // Tránh watchdog
        }
    }
}