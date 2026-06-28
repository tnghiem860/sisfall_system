#include <cstdint>
#include <cstring>
#include <cmath>

extern "C" {
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "mpu6050.h"
#include "driver/gpio.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
}

#include "ai_model.h"

static const char *TAG = "WIRELESS_TEST";

// ── Cấu hình WiFi & UDP ──────────────────────────────────────────
#define WIFI_SSID      "KANA Coffee 24h"
#define WIFI_PASS      "68686868"
#define TARGET_IP      "192.168.110.126"   // IP máy tính đang chạy plot_data.py
#define TARGET_PORT    5005

// ── Cấu hình AI ─────────────────────────────────────────────────
#define WINDOW_SIZE         150
#define NUM_CHANNELS        3
#define POST_FALL_SAMPLES   75
#define IMPACT_THRESHOLD    2.56f   // AM² >= 1.6g

// ── Cấu hình Buzzer ─────────────────────────────────────────────
#define BUZZER_GPIO     GPIO_NUM_13
#define BUZZER_BEEP_MS  2000    // Kêu 2 giây khi phát hiện té ngã

// ── UDP Socket ──────────────────────────────────────────────────
static int               s_sock = -1;
static struct sockaddr_in s_dest;

// ── Buzzer ──────────────────────────────────────────────────────
static void buzzer_init(void)
{
    gpio_reset_pin(BUZZER_GPIO);
    gpio_set_direction(BUZZER_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(BUZZER_GPIO, 0);
}

// Task kêu còi (chạy riêng để không block vòng lặp đọc sensor)
static void buzzer_task(void *arg)
{
    uint32_t beep_ms = (uint32_t)(uintptr_t)arg;
    gpio_set_level(BUZZER_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(beep_ms));
    gpio_set_level(BUZZER_GPIO, 0);
    vTaskDelete(NULL);
}

static void buzzer_beep(uint32_t ms)
{
    xTaskCreate(buzzer_task, "buzzer", 1024, (void *)(uintptr_t)ms, 5, NULL);
}

// ── WiFi event handler ──────────────────────────────────────────
static EventGroupHandle_t s_wifi_eg;
#define WIFI_CONNECTED_BIT BIT0

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "WiFi ngắt kết nối, thử lại...");
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Đã có IP: " IPSTR, IP2STR(&e->ip_info.ip));
        xEventGroupSetBits(s_wifi_eg, WIFI_CONNECTED_BIT);
    }
}

static void wifi_init(void)
{
    s_wifi_eg = xEventGroupCreate();

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,    wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT,   IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL));

    wifi_config_t wifi_cfg = {};
    strncpy((char *)wifi_cfg.sta.ssid,     WIFI_SSID, sizeof(wifi_cfg.sta.ssid) - 1);
    strncpy((char *)wifi_cfg.sta.password, WIFI_PASS, sizeof(wifi_cfg.sta.password) - 1);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_connect();

    ESP_LOGI(TAG, "Đang kết nối WiFi '%s'...", WIFI_SSID);
    xEventGroupWaitBits(s_wifi_eg, WIFI_CONNECTED_BIT, false, true, portMAX_DELAY);
    ESP_LOGI(TAG, "WiFi kết nối thành công!");

    // Mở UDP socket
    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (s_sock < 0) {
        ESP_LOGE(TAG, "Không tạo được socket!");
        return;
    }
    s_dest.sin_family      = AF_INET;
    s_dest.sin_port        = htons(TARGET_PORT);
    s_dest.sin_addr.s_addr = inet_addr(TARGET_IP);
    ESP_LOGI(TAG, "UDP socket sẵn sàng -> %s:%d", TARGET_IP, TARGET_PORT);
}

// ── Gửi gói UDP dữ liệu cảm biến ──────────────────────────────
static void udp_send_data(float ax, float ay, float az, float am2)
{
    if (s_sock < 0) return;
    char buf[128];
    int len = snprintf(buf, sizeof(buf), "DATA:%.4f,%.4f,%.4f,%.4f\n", ax, ay, az, am2);
    sendto(s_sock, buf, len, 0, (struct sockaddr *)&s_dest, sizeof(s_dest));
}

// ── Gửi sự kiện té ngã ─────────────────────────────────────────
static void udp_send_fall(float conf)
{
    if (s_sock < 0) return;
    char buf[64];
    int len = snprintf(buf, sizeof(buf), "FALL:%.4f\n", conf);
    sendto(s_sock, buf, len, 0, (struct sockaddr *)&s_dest, sizeof(s_dest));
}

// ── Gửi marker đánh dấu vùng 150 mẫu đưa vào AI ───────────────
// Format: WINDOW:start_time,end_time (tính theo số mẫu, phía Python tự quy đổi giây)
// Để đơn giản, gửi WINDOW:conf để Python biết khi nào window được gửi vào model
static void udp_send_window_marker(float conf, bool is_fall)
{
    if (s_sock < 0) return;
    char buf[64];
    // Gửi marker: WINDOW:is_fall,confidence
    // Python sẽ vẽ vùng tô màu tại thời điểm nhận được marker này
    int len = snprintf(buf, sizeof(buf), "WINDOW:%d,%.4f\n", is_fall ? 1 : 0, conf);
    sendto(s_sock, buf, len, 0, (struct sockaddr *)&s_dest, sizeof(s_dest));
}

// ── Task chính: Đọc MPU6050 → Chạy AI → Gửi UDP ───────────────
static void main_task(void *arg)
{
    // Khởi tạo I2C & MPU6050
    i2c_master_bus_handle_t bus;
    ESP_ERROR_CHECK(i2c_master_init(&bus));
    ESP_ERROR_CHECK(mpu6050_init(bus));
    ESP_LOGI(TAG, "MPU6050 OK");

    // Khởi tạo AI
    ESP_ERROR_CHECK(ai_model_init());
    ESP_LOGI(TAG, "AI model OK");

    // Ring buffer
    static float ring[WINDOW_SIZE][NUM_CHANNELS];
    int  head  = 0;
    int  count = 0;

    // Trạng thái phát hiện va chạm
    bool impact      = false;
    int  post_count  = 0;
    // Đánh dấu mẫu lúc impact để tính điểm bắt đầu của window
    int  impact_sample_idx = 0;   // tổng số mẫu tại thời điểm impact
    int  total_samples = 0;       // bộ đếm mẫu toàn cục

    ESP_LOGI(TAG, "=== Bắt đầu thu thập dữ liệu ===");

    while (1) {
        // Đọc 1 mẫu MPU6050
        mpu6050_data_t imu;
        mpu6050_clear_interrupt();
        if (mpu6050_read(&imu) != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(40));
            continue;
        }

        // KHÔNG dùng ma trận xoay bằng phần mềm nữa!
        // Vì khi bạn đeo thiết bị thực tế sang hông phải, cảm biến phần cứng đã tự động xoay 90 độ,
        // tạo ra dữ liệu khớp hoàn toàn 1:1 với dữ liệu đã xoay bằng code Python lúc train.
        float ax = imu.ax_g;
        float ay = imu.ay_g;
        float az = imu.az_g;
        float am2 = ax*ax + ay*ay + az*az;

        // Nạp vào ring buffer
        ring[head][0] = ax;
        ring[head][1] = ay;
        ring[head][2] = az;
        head = (head + 1) % WINDOW_SIZE;
        if (count < WINDOW_SIZE) count++;
        total_samples++;

        // Logic phát hiện va chạm (impact)
        if (!impact && am2 > IMPACT_THRESHOLD && count >= POST_FALL_SAMPLES) {
            impact           = true;
            post_count       = 0;
            impact_sample_idx = total_samples;
            ESP_LOGW(TAG, "Impact! AM²=%.3f", am2);
        }

        if (impact) {
            if (++post_count >= POST_FALL_SAMPLES && count >= WINDOW_SIZE) {
                // Build window 150 mẫu từ ring buffer
                float window[WINDOW_SIZE][NUM_CHANNELS];
                for (int i = 0; i < WINDOW_SIZE; i++) {
                    int idx = (head + i) % WINDOW_SIZE;
                    window[i][0] = ring[idx][0];
                    window[i][1] = ring[idx][1];
                    window[i][2] = ring[idx][2];
                }
                impact = false;

                // Chạy AI
                ai_result_t res;
                if (ai_model_run(window, &res) == ESP_OK) {
                    ESP_LOGI(TAG, "AI: fall=%d conf=%.3f", res.is_fall, res.confidence);

                    // Gửi marker window về Python (để đánh dấu vùng 150 mẫu trên đồ thị)
                    // Gửi trước để Python biết thời điểm window kết thúc = ngay bây giờ
                    udp_send_window_marker(res.confidence, res.is_fall);

                    if (res.is_fall && res.confidence >= 0.5f) {
                        // Gửi sự kiện té ngã
                        udp_send_fall(res.confidence);
                        // Bật còi buzzer 2 giây (chạy task riêng, không block)
                        buzzer_beep(BUZZER_BEEP_MS);
                        ESP_LOGW(TAG, "FALL DETECTED! conf=%.1f%% — Buzzer ON",
                                 res.confidence * 100.0f);
                    }
                }
            }
        }

        // Gửi dữ liệu cảm biến về máy tính
        udp_send_data(ax, ay, az, am2);

        vTaskDelay(pdMS_TO_TICKS(40));  // ~25Hz
    }
}

// ── app_main ────────────────────────────────────────────────────
extern "C" void app_main(void)
{
    buzzer_init();
    wifi_init();
    xTaskCreate(main_task, "main_task", 8192, NULL, 5, NULL);
}