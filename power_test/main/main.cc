#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_log.h"

static const char *TAG = "GPS_TEST";

#define SIM_UART_NUM      UART_NUM_2
#define SIM_TX_PIN        17
#define SIM_RX_PIN        16
#define BUF_SIZE          1024

// Hàm gửi lệnh AT qua UART
static void send_at(const char* cmd) {
    uart_write_bytes(SIM_UART_NUM, cmd, strlen(cmd));
    uart_write_bytes(SIM_UART_NUM, "\r\n", 2);
    ESP_LOGI(TAG, "Gửi: %s", cmd);
}

// Hàm chờ đọc phản hồi từ UART
static void wait_response(int delay_ms) {
    uint8_t data[BUF_SIZE];
    int len = uart_read_bytes(SIM_UART_NUM, data, BUF_SIZE - 1, pdMS_TO_TICKS(delay_ms));
    if (len > 0) {
        data[len] = '\0';
        ESP_LOGI(TAG, "Nhận được:\n%s", (char*)data);
    } else {
        ESP_LOGW(TAG, "Timeout, không có phản hồi");
    }
}

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "=== BẮT ĐẦU TEST CHỨC NĂNG GPS (EG800K) ===");

    // 1. Cấu hình UART kết nối với module SIM
    uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE
    };
    uart_param_config(SIM_UART_NUM, &uart_config);
    uart_set_pin(SIM_UART_NUM, SIM_TX_PIN, SIM_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    uart_driver_install(SIM_UART_NUM, BUF_SIZE * 2, 0, 0, NULL, 0);

    vTaskDelay(pdMS_TO_TICKS(2000));

    // 2. Test kết nối cơ bản
    send_at("AT");
    wait_response(1000);

    // 3. Tắt GPS, đổi sang chế độ GPS+BeiDou+GLONASS rồi bật lại
    ESP_LOGI(TAG, "Tắt GPS cũ để đổi chế độ...");
    send_at("AT+QGPSEND");
    wait_response(2000);

    ESP_LOGI(TAG, "Bật chế độ GPS+BeiDou+GLONASS (gnssconfig=3)...");
    send_at("AT+QGPSCFG=\"gnssconfig\",3");
    wait_response(2000);

    ESP_LOGI(TAG, "Khởi động lại GPS engine...");
    send_at("AT+QGPS=1");
    wait_response(2000);

    // 4. Vòng lặp liên tục hỏi tọa độ mỗi 3 giây
    // Kiểm tra trạng thái GPS engine trước
    ESP_LOGI(TAG, "=== Kiểm tra trạng thái GPS ===");
    send_at("AT+QGPS?");          // GPS engine có đang bật không?
    wait_response(2000);
    send_at("AT+QGPSCFG=\"gnssconfig\"");  // GNSS mode nào đang dùng?
    wait_response(2000);
    send_at("AT+QGPSGNMEA=\"GSV\"");       // Xem số vệ tinh đang theo dõi
    wait_response(3000);

    int count = 1;
    while (1) {
        ESP_LOGI(TAG, "--- Lần quét thứ %d ---", count++);
        send_at("AT+QGPSLOC=2");
        wait_response(2000);

        // Cứ 5 lần hỏi 1 lần xem NMEA thô để bắt bệnh
        if (count % 5 == 0) {
            ESP_LOGI(TAG, ">>> Đọc NMEA RMC (Dữ liệu định vị cơ bản) <<<");
            send_at("AT+QGPSGNMEA=\"RMC\"");
	    wait_response(2000);

            ESP_LOGI(TAG, ">>> Đọc NMEA GGA (Độ cao & Vệ tinh) <<<");
            send_at("AT+QGPSGNMEA=\"GGA\"");
            wait_response(2000);

            ESP_LOGI(TAG, ">>> Đọc NMEA GSV (Số vệ tinh trong tầm nhìn) <<<");
            send_at("AT+QGPSGNMEA=\"GSV\"");
            wait_response(2000);
        }

        ESP_LOGI(TAG, "Đợi 3 giây rồi quét tiếp...");
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}