# Code Test Năng lượng: Gửi SMS

Bạn copy toàn bộ đoạn code này đè vào file `main.cc`, cắm cáp nạp code xong thì **rút cáp USB ra**, cấp nguồn ngoài (qua máy đo dòng). 
- Còi kêu **2 tiếng ngắn**: Hệ thống bắt đầu vào **trạng thái ĐỘNG** (Gửi SMS, dò sóng).
- Còi kêu **1 tiếng dài**: Hệ thống bắt đầu vào **trạng thái TĨNH** (Nghỉ ngơi chờ chu kỳ tiếp theo).
Lưu ý: Còi sẽ tắt trước khi đo để không làm sai lệch dòng điện.

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

#define PHONE_NUMBER "0853344779"
#define BUZZER_GPIO GPIO_NUM_13

void init_buzzer() {
    gpio_reset_pin(BUZZER_GPIO);
    gpio_set_direction(BUZZER_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(BUZZER_GPIO, 0);
}

void signal_active() {
    // 2 tiếng bíp ngắn: Trạng thái ĐỘNG
    gpio_set_level(BUZZER_GPIO, 1); vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(BUZZER_GPIO, 0); vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(BUZZER_GPIO, 1); vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(BUZZER_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(500)); // Nghỉ 0.5s để tắt hẳn còi
}

void signal_idle() {
    // 1 tiếng bíp dài: Trạng thái TĨNH
    gpio_set_level(BUZZER_GPIO, 1); vTaskDelay(pdMS_TO_TICKS(500));
    gpio_set_level(BUZZER_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(500)); // Nghỉ 0.5s để tắt hẳn còi
}

extern "C" void app_main(void)
{
    init_buzzer();

    // Đoạn 1: Chờ dòng điện tĩnh ban đầu
    signal_idle(); 
    vTaskDelay(pdMS_TO_TICKS(5000)); 

    // Đoạn 2: Khởi động SIM
    signal_active(); 
    sim_init();
    vTaskDelay(pdMS_TO_TICKS(15000)); 

    while (1) {
        // Đoạn 3: Gửi SMS (Đỉnh dòng cao)
        signal_active();
        sim_send_sms(PHONE_NUMBER, "Test Cong Suat: SMS");

        // Đoạn 4: Nghỉ ngơi (Dòng tĩnh duy trì)
        signal_idle();
        vTaskDelay(pdMS_TO_TICKS(30000)); 
    }
}
```
