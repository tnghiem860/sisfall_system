#include "sim_firebase.h"
#include "sim_module.h"   // SIM_UART_LOCK / SIM_UART_UNLOCK

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_random.h"

static const char *TAG = "SIM_FIREBASE";

static bool fb_at(const char *cmd, uint32_t timeout_ms, const char *expect);

#define SIM_UART_NUM     UART_NUM_2
#define RX_BUF_SIZE       2048
#define HTTP_INPUT_TO     10    // giây để gửi body
#define HTTP_RESP_TO      30    // giây chờ response

// URL paths
#define COMMIT_PATH \
    "/v1/projects/" FIREBASE_PROJECT_ID \
    "/databases/(default)/documents:commit?key=" FIREBASE_API_KEY

#define FALL_EVENTS_PATH \
    "/v1/projects/" FIREBASE_PROJECT_ID \
    "/databases/(default)/documents/fall_events?key=" FIREBASE_API_KEY

static char s_rx_buf[RX_BUF_SIZE];

// ── helpers ──────────────────────────────────────────────────────

static void fb_flush(void)
{
    uart_flush_input(SIM_UART_NUM);
    memset(s_rx_buf, 0, sizeof(s_rx_buf));
}

static void fb_send(const char *data)
{
    uart_write_bytes(SIM_UART_NUM, data, strlen(data));
}

static void fb_sendline(const char *cmd)
{
    ESP_LOGI(TAG, ">> %s", cmd);
    uart_write_bytes(SIM_UART_NUM, cmd, strlen(cmd));
    uart_write_bytes(SIM_UART_NUM, "\r\n", 2);
}

static int fb_read(uint32_t timeout_ms, const char *expect)
{
    memset(s_rx_buf, 0, sizeof(s_rx_buf));
    int total = 0;
    TickType_t start = xTaskGetTickCount();

    while ((xTaskGetTickCount() - start) * portTICK_PERIOD_MS < timeout_ms) {
        int space = (int)sizeof(s_rx_buf) - total - 1;
        if (space <= 0) break;
        int n = uart_read_bytes(SIM_UART_NUM, (uint8_t *)(s_rx_buf + total),
                                space, pdMS_TO_TICKS(100));
        if (n > 0) {
            total += n;
            s_rx_buf[total] = '\0';
            if (expect && strstr(s_rx_buf, expect)) break;
            if (strstr(s_rx_buf, "ERROR")) break;
            if (!expect && strstr(s_rx_buf, "OK")) break;
        }
    }
    ESP_LOGI(TAG, "<< %s", total ? s_rx_buf : "TIMEOUT");
    return total;
}

static bool fb_at(const char *cmd, uint32_t timeout_ms, const char *expect)
{
    fb_flush();
    fb_sendline(cmd);
    fb_read(timeout_ms, expect);
    return (expect && strstr(s_rx_buf, expect)) ||
           (!expect && strstr(s_rx_buf, "OK"));
}

// ── Tự dò APN theo nhà mạng ──────────────────────────────────────
// Đọc IMSI -> suy ra nhà mạng (MCC+MNC) -> thử APN tương ứng trước,
// rồi thử lần lượt các APN còn lại cho tới khi PDP có IP.
// APN chạy được được lưu vào NVS cùng IMSI: lần khởi động sau (cùng SIM)
// dùng lại ngay, đổi SIM thì IMSI khác nên tự dò lại.

#define APN_NVS_NAMESPACE  "sim_apn"
#define APN_MAX_LEN        32
#define APN_MAX_CAND       8

// APN mặc định theo PLMN (MCC 452 = Việt Nam)
static const struct { const char *plmn; const char *apn; } VN_APN_TABLE[] = {
    { "45201", "m-wap"      },   // MobiFone
    { "45202", "m3-world"   },   // VinaPhone
    { "45204", "v-internet" },   // Viettel
    { "45205", "internet"   },   // Vietnamobile
    { "45206", "v-internet" },   // Viettel
    { "45207", "internet"   },   // Gmobile
    { "45208", "v-internet" },   // Viettel
};

// Thử theo thứ tự này sau khi đã thử APN suy ra từ IMSI (MVNO, SIM lạ...)
static const char *const APN_FALLBACK[] = {
    "v-internet", "m-wap", "m3-world", "internet", "m-i090",
};

// Cần giữ UART lock. Ghi IMSI (14-15 chữ số) vào imsi.
static bool read_imsi(char *imsi, size_t size)
{
    if (!fb_at("AT+CIMI", 3000, "OK")) return false;

    const char *p = s_rx_buf;
    while (*p) {
        if (!isdigit((unsigned char)*p)) { p++; continue; }
        size_t n = 0;
        while (isdigit((unsigned char)p[n])) n++;
        if (n >= 14 && n <= 15 && n < size) {
            memcpy(imsi, p, n);
            imsi[n] = '\0';
            return true;
        }
        p += n;
    }
    return false;
}

static const char *apn_for_imsi(const char *imsi)
{
    for (size_t i = 0; i < sizeof(VN_APN_TABLE) / sizeof(VN_APN_TABLE[0]); i++) {
        if (strncmp(imsi, VN_APN_TABLE[i].plmn, 5) == 0) return VN_APN_TABLE[i].apn;
    }
    return NULL;
}

static bool apn_nvs_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "[GPRS] NVS không dùng được (%s) — bỏ qua cache APN", esp_err_to_name(err));
        return false;
    }
    return true;
}

// Trả true nếu NVS có APN đã lưu cho đúng SIM (IMSI) này.
static bool apn_cache_load(const char *imsi, char *apn, size_t size)
{
    nvs_handle_t h;
    if (nvs_open(APN_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return false;

    char saved_imsi[20];
    size_t len = sizeof(saved_imsi);
    bool ok = nvs_get_str(h, "imsi", saved_imsi, &len) == ESP_OK &&
              strcmp(saved_imsi, imsi) == 0;
    if (ok) {
        len = size;
        ok = nvs_get_str(h, "apn", apn, &len) == ESP_OK;
    }
    nvs_close(h);
    return ok;
}

static void apn_cache_save(const char *imsi, const char *apn)
{
    nvs_handle_t h;
    if (nvs_open(APN_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_set_str(h, "imsi", imsi) == ESP_OK &&
        nvs_set_str(h, "apn", apn) == ESP_OK) {
        nvs_commit(h);
    }
    nvs_close(h);
}

// Thêm apn vào danh sách nếu chưa có (giữ nguyên thứ tự ưu tiên).
static int add_candidate(const char **list, int n, const char *apn)
{
    if (!apn || !apn[0] || n >= APN_MAX_CAND) return n;
    for (int i = 0; i < n; i++) {
        if (strcmp(list[i], apn) == 0) return n;
    }
    list[n] = apn;
    return n + 1;
}

bool sim_firebase_start_pdp(void)
{
    if (SIM_UART_LOCK() != pdTRUE) {
        ESP_LOGE(TAG, "[GPRS] start_pdp: cannot get UART mutex");
        return false;
    }

    ESP_LOGI(TAG, "[GPRS] Configuring TCP/IP stack...");

    // Lập danh sách APN cần thử: cache -> theo nhà mạng -> phần còn lại
    char imsi[20] = "";
    bool have_imsi = read_imsi(imsi, sizeof(imsi));
    bool nvs_ok = have_imsi && apn_nvs_init();

    char cached[APN_MAX_LEN] = "";
    const char *cand[APN_MAX_CAND];
    int n = 0;

    if (nvs_ok && apn_cache_load(imsi, cached, sizeof(cached))) {
        ESP_LOGI(TAG, "[GPRS] APN đã lưu cho SIM này: \"%s\"", cached);
        n = add_candidate(cand, n, cached);
    }
    if (have_imsi) {
        char plmn[6];
        memcpy(plmn, imsi, 5);
        plmn[5] = '\0';
        const char *apn = apn_for_imsi(imsi);
        ESP_LOGI(TAG, "[GPRS] PLMN=%s -> APN gợi ý: %s", plmn, apn ? apn : "(không rõ)");
        n = add_candidate(cand, n, apn);
    } else {
        ESP_LOGW(TAG, "[GPRS] Không đọc được IMSI — thử lần lượt các APN");
    }
    for (size_t i = 0; i < sizeof(APN_FALLBACK) / sizeof(APN_FALLBACK[0]); i++) {
        n = add_candidate(cand, n, APN_FALLBACK[i]);
    }

    bool got_ip = false;
    for (int i = 0; i < n && !got_ip; i++) {
        char cmd[160];
        ESP_LOGI(TAG, "[GPRS] Thử APN \"%s\" (%d/%d)", cand[i], i + 1, n);

        snprintf(cmd, sizeof(cmd), "AT+QICSGP=1,1,\"%s\",\"\",\"\",1", cand[i]);
        fb_at(cmd, 5000, "OK");
        fb_at("AT+QIDEACT=1", 10000, "OK");

        // Tra mutex truoc delay dai
        SIM_UART_UNLOCK();
        vTaskDelay(pdMS_TO_TICKS(2000));
        if (SIM_UART_LOCK() != pdTRUE) return false;

        fb_at("AT+QIACT=1", 30000, "OK");

        // Kiem tra IP
        if (fb_at("AT+QIACT?", 5000, "+QIACT: 1")) {
            ESP_LOGI(TAG, "[GPRS] PDP context 1 is active (Got IP) — APN \"%s\"", cand[i]);
            got_ip = true;
            if (nvs_ok && strcmp(cached, cand[i]) != 0) {
                apn_cache_save(imsi, cand[i]);
            }
        }
    }
    if (!got_ip) {
        ESP_LOGE(TAG, "[GPRS] Không APN nào cấp được IP (đã thử %d APN)", n);
    }

    // Config HTTP to use PDP context 1
    fb_at("AT+QHTTPSTOP", 2000, NULL);
    fb_at("AT+QHTTPCFG=\"contextid\",1", 3000, "OK");
    fb_at("AT+QHTTPCFG=\"responseheader\",1", 3000, "OK");

    SIM_UART_UNLOCK();
    return got_ip;
}

// ── Thực hiện HTTP POST lên Firestore ────────────────────────────
// path: đường dẫn REST (COMMIT_PATH hoặc FALL_EVENTS_PATH)
// body: chuỗi JSON
static bool fb_http_post(const char *path, const char *body)
{
    if (SIM_UART_LOCK() != pdTRUE) {
        ESP_LOGE(TAG, "Cannot get UART mutex");
        return false;
    }

    // 0. Giải phóng HTTP(S) session cũ nếu có (chống lỗi HTTP(S) busy)
    fb_at("AT+QHTTPSTOP", 2000, NULL);

    // 1. Cấu hình HTTP SSL (không xác minh cert — phù hợp cho đồ án)
    fb_at("AT+QHTTPCFG=\"contextid\",1",   3000, NULL);
    fb_at("AT+QHTTPCFG=\"sslctxid\",1",    3000, NULL);
    fb_at("AT+QHTTPCFG=\"contenttype\",4", 3000, NULL); // application/json
    fb_at("AT+QHTTPCFG=\"responseheader\",0", 3000, NULL);

    // SSL: tắt verify cert để tránh phức tạp (test mode)
    fb_at("AT+QSSLCFG=\"sslversion\",1,4",    3000, NULL); // TLS 1.2
    fb_at("AT+QSSLCFG=\"ciphersuite\",1,0xFFFF", 3000, NULL);
    fb_at("AT+QSSLCFG=\"ignorelocaltime\",1", 3000, NULL);
    fb_at("AT+QSSLCFG=\"seclevel\",1,0",      3000, NULL); // no cert check

    // 2. Set URL
    char url[512];
    snprintf(url, sizeof(url), "https://" FIREBASE_HOST "%s", path);

    char cmd[48];
    snprintf(cmd, sizeof(cmd), "AT+QHTTPURL=%d,%d", (int)strlen(url), 5);
    fb_flush();
    fb_sendline(cmd);
    fb_read(5000, "CONNECT");
    if (!strstr(s_rx_buf, "CONNECT")) {
        ESP_LOGE(TAG, "QHTTPURL: no CONNECT prompt");
        SIM_UART_UNLOCK();
        return false;
    }
    fb_send(url);
    fb_read(5000, "OK");

    // 3. POST body
    int bodylen = strlen(body);
    snprintf(cmd, sizeof(cmd), "AT+QHTTPPOST=%d,%d,%d",
             bodylen, HTTP_INPUT_TO, HTTP_RESP_TO);
    fb_flush();
    fb_sendline(cmd);
    fb_read(10000, "CONNECT");
    if (!strstr(s_rx_buf, "CONNECT")) {
        ESP_LOGE(TAG, "QHTTPPOST: no CONNECT prompt");
        SIM_UART_UNLOCK();
        return false;
    }

    ESP_LOGI(TAG, "Sending body (%d bytes)...", bodylen);
    fb_send(body);

    // 4. Chờ response
    fb_read(HTTP_RESP_TO * 1000, "+QHTTPPOST:");

    // HTTP 200 = thành công
    bool ok = strstr(s_rx_buf, ",200") != NULL;
    if (ok) {
        ESP_LOGI(TAG, "Firestore POST OK (HTTP 200)");
    } else {
        ESP_LOGE(TAG, "Firestore POST FAILED: %s", s_rx_buf);
    }

    SIM_UART_UNLOCK();
    return ok;
}

// ── Thực hiện HTTP GET từ Firestore ──────────────────────────────
static bool fb_http_get(const char *path, char *resp_buf, size_t resp_size)
{
    if (SIM_UART_LOCK() != pdTRUE) {
        ESP_LOGE(TAG, "Cannot get UART mutex for GET");
        return false;
    }

    fb_at("AT+QHTTPSTOP", 2000, NULL);
    fb_at("AT+QHTTPCFG=\"contextid\",1",   3000, NULL);
    fb_at("AT+QHTTPCFG=\"sslctxid\",1",    3000, NULL);
    fb_at("AT+QHTTPCFG=\"responseheader\",0", 3000, NULL);

    char url[512];
    snprintf(url, sizeof(url), "https://" FIREBASE_HOST "%s", path);

    char cmd[48];
    snprintf(cmd, sizeof(cmd), "AT+QHTTPURL=%d,%d", (int)strlen(url), 5);
    fb_flush();
    fb_sendline(cmd);
    fb_read(5000, "CONNECT");
    if (!strstr(s_rx_buf, "CONNECT")) {
        ESP_LOGE(TAG, "QHTTPURL: no CONNECT prompt");
        SIM_UART_UNLOCK();
        return false;
    }
    fb_send(url);
    fb_read(5000, "OK");

    fb_flush();
    fb_sendline("AT+QHTTPGET=30");
    fb_read(30000, "+QHTTPGET:");

    bool ok = strstr(s_rx_buf, ",200") != NULL;
    if (ok) {
        fb_flush();
        fb_sendline("AT+QHTTPREAD=30");
        fb_read(30000, "+QHTTPREAD: 0"); // Đọc toàn bộ response body
        if (resp_buf) {
            strncpy(resp_buf, s_rx_buf, resp_size - 1);
            resp_buf[resp_size - 1] = '\0';
        }
    } else {
        ESP_LOGW(TAG, "Firestore GET FAILED or Not Found: %s", s_rx_buf);
    }

    SIM_UART_UNLOCK();
    return ok;
}

// ── Public API ────────────────────────────────────────────────────

void sim_firebase_init(void)
{
    ESP_LOGI(TAG, "Firebase component init (no extra setup needed)");
}

bool sim_firebase_update_status(const char *device_id,
                                 float lat,  float lon,
                                 float gps_accuracy,
                                 int   battery_pct,
                                 bool  fall_detected,
                                 int8_t ack_fall_opt,
                                 float confidence)
{
    char doc_name[128];
    snprintf(doc_name, sizeof(doc_name),
             "projects/" FIREBASE_PROJECT_ID
             "/databases/(default)/documents/devices/%s",
             device_id ? device_id : "ESP32_FALL_001");

    char body[1024];
    
    // Xây dựng chuỗi fields và updateMask
    char fields_str[512];
    char mask_str[256];
    
    if (lat == 0.0f && lon == 0.0f) {
        if (ack_fall_opt != -1) {
            snprintf(fields_str, sizeof(fields_str),
                "\"device_id\":{\"stringValue\":\"%s\"},"
                "\"gps_accuracy\":{\"doubleValue\":%.2f},"
                "\"battery_pct\":{\"integerValue\":\"%d\"},"
                "\"fall_detected\":{\"booleanValue\":%s},"
                "\"ack_fall\":{\"booleanValue\":%s},"
                "\"confidence\":{\"doubleValue\":%.4f}",
                device_id ? device_id : "ESP32_FALL_001",
                (double)gps_accuracy, battery_pct,
                fall_detected ? "true" : "false",
                ack_fall_opt == 1 ? "true" : "false",
                (double)confidence);
            snprintf(mask_str, sizeof(mask_str), "\"device_id\",\"gps_accuracy\",\"battery_pct\",\"fall_detected\",\"ack_fall\",\"confidence\"");
        } else {
            snprintf(fields_str, sizeof(fields_str),
                "\"device_id\":{\"stringValue\":\"%s\"},"
                "\"gps_accuracy\":{\"doubleValue\":%.2f},"
                "\"battery_pct\":{\"integerValue\":\"%d\"},"
                "\"fall_detected\":{\"booleanValue\":%s},"
                "\"confidence\":{\"doubleValue\":%.4f}",
                device_id ? device_id : "ESP32_FALL_001",
                (double)gps_accuracy, battery_pct,
                fall_detected ? "true" : "false",
                (double)confidence);
            snprintf(mask_str, sizeof(mask_str), "\"device_id\",\"gps_accuracy\",\"battery_pct\",\"fall_detected\",\"confidence\"");
        }
        snprintf(body, sizeof(body),
            "{\"writes\":[{\"update\":{\"name\":\"%s\",\"fields\":{%s}},\"updateMask\":{\"fieldPaths\":[%s]}}]}",
            doc_name, fields_str, mask_str);
        ESP_LOGW(TAG, "No GPS fix — updating status WITHOUT overwriting lat/lng");
    } else {
        if (ack_fall_opt != -1) {
            snprintf(fields_str, sizeof(fields_str),
                "\"device_id\":{\"stringValue\":\"%s\"},"
                "\"lat\":{\"doubleValue\":%.6f},"
                "\"lng\":{\"doubleValue\":%.6f},"
                "\"gps_accuracy\":{\"doubleValue\":%.2f},"
                "\"battery_pct\":{\"integerValue\":\"%d\"},"
                "\"fall_detected\":{\"booleanValue\":%s},"
                "\"ack_fall\":{\"booleanValue\":%s},"
                "\"confidence\":{\"doubleValue\":%.4f}",
                device_id ? device_id : "ESP32_FALL_001",
                (double)lat, (double)lon,
                (double)gps_accuracy, battery_pct,
                fall_detected ? "true" : "false",
                ack_fall_opt == 1 ? "true" : "false",
                (double)confidence);
            snprintf(mask_str, sizeof(mask_str), "\"device_id\",\"lat\",\"lng\",\"gps_accuracy\",\"battery_pct\",\"fall_detected\",\"ack_fall\",\"confidence\"");
        } else {
            snprintf(fields_str, sizeof(fields_str),
                "\"device_id\":{\"stringValue\":\"%s\"},"
                "\"lat\":{\"doubleValue\":%.6f},"
                "\"lng\":{\"doubleValue\":%.6f},"
                "\"gps_accuracy\":{\"doubleValue\":%.2f},"
                "\"battery_pct\":{\"integerValue\":\"%d\"},"
                "\"fall_detected\":{\"booleanValue\":%s},"
                "\"confidence\":{\"doubleValue\":%.4f}",
                device_id ? device_id : "ESP32_FALL_001",
                (double)lat, (double)lon,
                (double)gps_accuracy, battery_pct,
                fall_detected ? "true" : "false",
                (double)confidence);
            snprintf(mask_str, sizeof(mask_str), "\"device_id\",\"lat\",\"lng\",\"gps_accuracy\",\"battery_pct\",\"fall_detected\",\"confidence\"");
        }
        // updateMask: chỉ ghi các field trên. Thiếu mask thì Firestore thay cả document,
        // xóa luôn các field do App ghi (emergency_mode, ack_fall, phone_number).
        snprintf(body, sizeof(body),
            "{\"writes\":[{\"update\":{\"name\":\"%s\",\"fields\":{%s}},\"updateMask\":{\"fieldPaths\":[%s]}}]}",
            doc_name, fields_str, mask_str);
    }

    ESP_LOGI(TAG, "Updating status to Firestore (lat=%.4f, lng=%.4f)...", (double)lat, (double)lon);
    return fb_http_post(COMMIT_PATH, body);
}

bool sim_firebase_push_fall_event(const char *device_id,
                                   float lat,  float lon,
                                   float confidence,
                                   int   battery_pct)
{
    // Tạo document ID ngẫu nhiên (vd: evt_a1b2c3d4e5f6g7h8)
    uint32_t r1 = esp_random();
    uint32_t r2 = esp_random();
    char doc_id[64];
    snprintf(doc_id, sizeof(doc_id), "evt_%08lx%08lx", (unsigned long)r1, (unsigned long)r2);

    // Dùng Firestore FieldTransform "setToServerValue": "REQUEST_TIME" cho trường fall_time
    char body[800];
    snprintf(body, sizeof(body),
        "{\"writes\":[{"
            "\"update\":{"
                "\"name\":\"projects/" FIREBASE_PROJECT_ID "/databases/(default)/documents/fall_events/%s\","
                "\"fields\":{"
                    "\"device_id\":{\"stringValue\":\"%s\"},"
                    "\"lat\":{\"doubleValue\":%.6f},"
                    "\"lng\":{\"doubleValue\":%.6f},"
                    "\"confidence\":{\"doubleValue\":%.4f},"
                    "\"battery_at_event\":{\"integerValue\":\"%d\"}"
                "}"
            "},"
            "\"updateTransforms\":[{"
                "\"fieldPath\":\"fall_time\","
                "\"setToServerValue\":\"REQUEST_TIME\""
            "}]"
        "}]}",
        doc_id,
        device_id ? device_id : "ESP32_FALL_001",
        (double)lat, (double)lon,
        (double)confidence,
        battery_pct
    );

    ESP_LOGI(TAG, "Pushing fall event to Firestore (serverTimestamp)...");
    return fb_http_post(COMMIT_PATH, body);
}

// Xóa các hàm liên quan đến clear_location_request vì emergency_mode là một trạng thái (state)
// được thiết lập và xóa hoàn toàn từ phía App.

bool sim_firebase_get_commands(const char *device_id, bool *emergency_mode, bool *ack_fall)
{
    char path[256];
    snprintf(path, sizeof(path),
             "/v1/projects/" FIREBASE_PROJECT_ID
             "/databases/(default)/documents/devices/%s?key=" FIREBASE_API_KEY "&t=%ld",
             device_id ? device_id : "ESP32_FALL_001",
             (long)xTaskGetTickCount());

    char resp[RX_BUF_SIZE];
    bool ok = fb_http_get(path, resp, sizeof(resp));
    if (!ok) return false;

    bool found_any = false;

    if (emergency_mode) {
        char *loc = strstr(resp, "\"emergency_mode\"");
        if (loc) {
            char window[256];
            strncpy(window, loc, sizeof(window) - 1);
            window[sizeof(window) - 1] = '\0';
            
            char *true_ptr = strstr(window, "true");
            char *false_ptr = strstr(window, "false");
            if (true_ptr && (!false_ptr || true_ptr < false_ptr)) {
                *emergency_mode = true;
            } else {
                *emergency_mode = false;
            }
            found_any = true;
        }
    }

    if (ack_fall) {
        char *loc = strstr(resp, "\"ack_fall\"");
        if (loc) {
            char window[256];
            strncpy(window, loc, sizeof(window) - 1);
            window[sizeof(window) - 1] = '\0';
            
            char *true_ptr = strstr(window, "true");
            char *false_ptr = strstr(window, "false");
            if (true_ptr && (!false_ptr || true_ptr < false_ptr)) {
                *ack_fall = true;
            } else {
                *ack_fall = false;
            }
            found_any = true;
        } else {
            // Nếu chưa có trường ack_fall trong Firebase → coi như chưa ACK
            *ack_fall = false;
        }
    }

    return found_any;
}

// ── Số điện thoại nhận SMS: mảng sms_numbers trong devices/{device_id} ──
// Dạng Firestore: "sms_numbers": {"arrayValue": {"values": [{"stringValue": "03..."}, ...]}}
// Các lệnh ghi trạng thái đều có updateMask nên không đụng field này.

#define PHONE_MIN_LEN      8
#define PHONE_MAX_DIGITS   15

// Chuẩn hóa số trong [s, e): bỏ khoảng trắng, '-', '.'; chỉ nhận chữ số (cho
// phép '+' đầu). Số này đi thẳng vào lệnh AT+CMGS nên không được chứa ký tự nào khác.
static bool normalize_phone(const char *s, const char *e, char *out, size_t size)
{
    size_t n = 0;
    for (; s < e; s++) {
        if (*s == ' ' || *s == '-' || *s == '.') continue;
        bool ok_char = isdigit((unsigned char)*s) || (n == 0 && *s == '+');
        if (!ok_char || n >= PHONE_MAX_DIGITS + 1 || n + 1 >= size) return false;
        out[n++] = *s;
    }
    if (n < PHONE_MIN_LEN) return false;
    out[n] = '\0';
    return true;
}

// Đọc chuỗi JSON bắt đầu ngay sau dấu nháy mở tại *pp. Trả về [start, end) và
// đặt *pp sau dấu nháy đóng. Trả false nếu chuỗi không kết thúc.
static bool json_read_string(const char **pp, const char **start, const char **end)
{
    const char *p = *pp;
    *start = p;
    while (*p && *p != '"') {
        if (*p == '\\' && p[1]) p++;   // bỏ qua ký tự escape
        p++;
    }
    if (*p != '"') return false;
    *end = p;
    *pp = p + 1;
    return true;
}

// Lấy các số hợp lệ trong mảng sms_numbers. Trả số lượng số hợp lệ (0 nếu
// mảng rỗng/sai định dạng); *found = false nếu không có field sms_numbers.
static int parse_sms_numbers(const char *resp, char (*out)[SIM_PHONE_BUF], int max, bool *found)
{
    *found = false;
    const char *key = strstr(resp, "\"sms_numbers\"");
    if (!key) return 0;
    *found = true;

    // Đi đúng cấu trúc: "sms_numbers": { "arrayValue": { "values": [ ...
    // (không tìm '[' gần nhất vì có thể là mảng của field khác phía sau)
    const char *av = strstr(key, "\"arrayValue\"");
    if (!av || av - key > 40) return 0;                     // không phải kiểu mảng
    const char *p = av + strlen("\"arrayValue\"");
    while (*p == ' ' || *p == '\r' || *p == '\n' || *p == '\t' || *p == ':') p++;
    if (*p != '{') return 0;
    p++;
    while (*p == ' ' || *p == '\r' || *p == '\n' || *p == '\t') p++;
    if (*p != '"') return 0;                                // "arrayValue": {} = mảng rỗng
    p++;
    const char *vs, *ve;
    if (!json_read_string(&p, &vs, &ve)) return 0;
    if ((size_t)(ve - vs) != strlen("values") || strncmp(vs, "values", ve - vs) != 0) return 0;
    while (*p == ' ' || *p == '\r' || *p == '\n' || *p == '\t' || *p == ':') p++;
    if (*p != '[') return 0;
    p++;

    int count = 0;
    while (*p && *p != ']') {
        if (*p != '"') { p++; continue; }
        p++;
        const char *ts, *te;
        if (!json_read_string(&p, &ts, &te)) break;
        if ((size_t)(te - ts) != strlen("stringValue") || strncmp(ts, "stringValue", te - ts) != 0) {
            continue;   // token khác ("values", ...)
        }
        // "stringValue" : "<số>"
        const char *q = strchr(p, '"');
        if (!q) break;
        q++;
        const char *vs, *ve;
        if (!json_read_string(&q, &vs, &ve)) break;
        p = q;

        char num[SIM_PHONE_BUF];
        if (count < max && normalize_phone(vs, ve, num, sizeof(num))) {
            bool dup = false;
            for (int i = 0; i < count; i++) {
                if (strcmp(out[i], num) == 0) { dup = true; break; }
            }
            if (!dup) {
                memcpy(out[count], num, sizeof(num));
                count++;
            }
        }
    }
    return count;
}

phone_status_t sim_firebase_get_sms_numbers(const char *device_id,
                                            char (*numbers)[SIM_PHONE_BUF],
                                            int max, int *count)
{
    if (!numbers || !count || max <= 0) return PHONE_NET_ERR;
    *count = 0;

    char path[256];
    snprintf(path, sizeof(path),
             "/v1/projects/" FIREBASE_PROJECT_ID
             "/databases/(default)/documents/devices/%s?key=" FIREBASE_API_KEY "&t=%ld",
             device_id ? device_id : "ESP32_FALL_001",
             (long)xTaskGetTickCount());

    char resp[RX_BUF_SIZE];
    if (!fb_http_get(path, resp, sizeof(resp))) return PHONE_NET_ERR;

    bool found;
    *count = parse_sms_numbers(resp, numbers, max, &found);
    if (!found) return PHONE_ABSENT;
    if (*count == 0) {
        ESP_LOGW(TAG, "sms_numbers có trên Firebase nhưng không có số hợp lệ");
        return PHONE_INVALID;
    }
    return PHONE_OK;
}

bool sim_firebase_seed_sms_numbers(const char *device_id, const char *phone)
{
    // updateMask chỉ có sms_numbers: không đụng field nào khác của document.
    // currentDocument.exists=true: không tự tạo document mới ngoài ý muốn.
    char body[480];
    snprintf(body, sizeof(body),
        "{\"writes\":[{"
            "\"update\":{"
                "\"name\":\"projects/" FIREBASE_PROJECT_ID "/databases/(default)/documents/devices/%s\","
                "\"fields\":{\"sms_numbers\":{\"arrayValue\":{\"values\":["
                    "{\"stringValue\":\"%s\"}"
                "]}}}"
            "},"
            "\"updateMask\":{\"fieldPaths\":[\"sms_numbers\"]},"
            "\"currentDocument\":{\"exists\":true}"
        "}]}",
        device_id ? device_id : "ESP32_FALL_001", phone);

    ESP_LOGI(TAG, "Ghi số điện thoại mặc định lên Firebase (sms_numbers chưa có)...");
    return fb_http_post(COMMIT_PATH, body);
}
