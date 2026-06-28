# Code Test Năng lượng: Gửi dữ liệu Firebase (Network Data)

Thay thế file `main.cc` của bạn bằng code này, nạp xong rút cáp USB.
- Còi kêu **2 tiếng ngắn**: Bắt đầu **trạng thái ĐỘNG** (Bật Data 4G, đẩy sự kiện Firebase).
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
#include "sim_firebase.h"
#include "driver/gpio.h"
}

#define DEVICE_ID "ESP32_FALL_001"
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

    signal_idle();
    vTaskDelay(pdMS_TO_TICKS(5000)); 

    signal_active();
    sim_init();
    vTaskDelay(pdMS_TO_TICKS(15000)); // Chờ SIM nhận sóng

    // Kích hoạt mạng Data
    sim_send_at("AT+QICSGP=1,1,\"v-internet\",\"\",\"\",1", NULL, 0, 1000);
    sim_send_at("AT+QIACT=1", NULL, 0, 5000);
    vTaskDelay(pdMS_TO_TICKS(2000));

    while (1) {
        signal_active();
        long ts = (long)(xTaskGetTickCount() / configTICK_RATE_HZ);
        sim_firebase_push_fall_event(DEVICE_ID, 21.02, 105.83, 0.99, 100.0, ts);

        signal_idle();
        vTaskDelay(pdMS_TO_TICKS(20000)); 
    }
}
```
