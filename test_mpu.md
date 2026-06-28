# Code Test Năng lượng: Đọc MPU6050 liên tục

Đoạn code này mô phỏng giao tiếp I2C với MPU6050 liên tục ở tốc độ 100Hz.
- Còi kêu **2 tiếng ngắn**: Bắt đầu **đọc MPU6050 liên tục trong 30 giây**.
- Còi kêu **1 tiếng dài**: Dừng đọc 30 giây (Để MPU vào trạng thái im lặng).

```cpp
#include <cstdint>
#include <cstring>
#include <cmath>

extern "C" {
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "mpu6050.h"
#include "driver/gpio.h"
}

#define BUZZER_GPIO GPIO_NUM_13

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
    mpu6050_init();

    while (1) {
        // Chu kỳ nghỉ (TĨNH)
        signal_idle();
        vTaskDelay(pdMS_TO_TICKS(30000));

        // Chu kỳ đọc MPU (ĐỘNG)
        signal_active();
        long start_time = xTaskGetTickCount();
        while ((xTaskGetTickCount() - start_time) < pdMS_TO_TICKS(30000)) {
            float ax, ay, az;
            mpu6050_read_accel(&ax, &ay, &az);
            vTaskDelay(pdMS_TO_TICKS(10)); // 100Hz
        }
    }
}
```
