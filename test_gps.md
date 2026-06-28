# Code Test Năng lượng: Lấy tọa độ GPS

Bạn copy toàn bộ đoạn code này đè vào file `main.cc`, nạp code rồi rút cáp USB ra.
- Còi kêu **2 tiếng ngắn**: Bắt đầu **trạng thái ĐỘNG** (Bật GPS dò vệ tinh).
- Còi kêu **1 tiếng dài**: Bắt đầu **trạng thái TĨNH** (Nghỉ chờ).

```cpp
#include <cstdint>
#include <cstring>
#include <cmath>

extern "C" {
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "sim_module.h"
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

    // Dòng tĩnh hệ thống
    signal_idle();
    vTaskDelay(pdMS_TO_TICKS(5000)); 

    // Bật SIM
    signal_active();
    sim_init();
    vTaskDelay(pdMS_TO_TICKS(10000));

    while (1) {
        sim_gps_t gps;
        
        // Trạng thái bật GPS (Ăn dòng liên tục khoảng vài chục mA)
        signal_active();
        sim_gps_get_location_retry(&gps, 10, 5000);

        // Trạng thái nghỉ GPS
        signal_idle();
        vTaskDelay(pdMS_TO_TICKS(30000)); 
    }
}
```
