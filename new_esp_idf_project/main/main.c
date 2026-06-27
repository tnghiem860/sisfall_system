#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_adc/adc_oneshot.h"      // API mới
#include "esp_adc/adc_cali.h"         // API mới
#include "esp_adc/adc_cali_scheme.h"  // API mới
#include "esp_log.h"

// ===== CẤU HÌNH =====
#define BAT_ADC_CHANNEL     ADC_CHANNEL_5   // GPIO34
#define ADC_SAMPLES         64
#define DIVIDER_RATIO       2.0f
#define INTERVAL_MS         60000           // 1 phút

static const char* TAG = "BATTERY";

// ===== LOOKUP TABLE =====
typedef struct {
    uint16_t voltage_mv;
    uint8_t  percent;
} bat_point_t;

static const bat_point_t LOOKUP[] = {
    {4200, 100}, {4180,  99}, {4160,  98}, {4140,  97},
    {4120,  96}, {4100,  95}, {4080,  93}, {4060,  91},
    {4040,  89}, {4020,  87}, {4000,  85}, {3980,  83},
    {3960,  80}, {3940,  77}, {3920,  74}, {3900,  71},
    {3880,  68}, {3860,  65}, {3840,  62}, {3820,  59},
    {3800,  56}, {3780,  53}, {3760,  50}, {3740,  47},
    {3720,  44}, {3700,  41}, {3680,  38}, {3660,  35},
    {3640,  32}, {3620,  29}, {3600,  26}, {3580,  23},
    {3560,  20}, {3540,  17}, {3520,  15}, {3500,  13},
    {3480,  11}, {3460,   9}, {3440,   7}, {3420,   5},
    {3400,   4}, {3380,   3}, {3360,   2}, {3300,   1},
    {3000,   0},
};
#define LOOKUP_SIZE (sizeof(LOOKUP) / sizeof(LOOKUP[0]))

// ===== BIẾN GLOBAL ADC =====
static adc_oneshot_unit_handle_t adc_handle;
static adc_cali_handle_t         cali_handle;
static bool                      cali_enable = false;

// ===== KHỞI TẠO ADC + CALIBRATION =====
static void adc_init(void) {
    // 1. Khởi tạo ADC unit
    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id  = ADC_UNIT_1,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&unit_cfg, &adc_handle));

    // 2. Cấu hình channel
    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten    = ADC_ATTEN_DB_12,   // DB_12 thay cho DB_11 (v5.x)
        .bitwidth = ADC_BITWIDTH_12,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(
        adc_handle, BAT_ADC_CHANNEL, &chan_cfg));

    // 3. Calibration — thử Curve Fitting trước (chính xác hơn)
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id  = ADC_UNIT_1,
        .chan     = BAT_ADC_CHANNEL,
        .atten    = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &cali_handle) == ESP_OK) {
        cali_enable = true;
        ESP_LOGI(TAG, "Calibration: Curve Fitting");
    }

#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    adc_cali_line_fitting_config_t cali_cfg = {
        .unit_id   = ADC_UNIT_1,
        .atten     = ADC_ATTEN_DB_12,
        .bitwidth  = ADC_BITWIDTH_12,
    };
    if (adc_cali_create_scheme_line_fitting(&cali_cfg, &cali_handle) == ESP_OK) {
        cali_enable = true;
        ESP_LOGI(TAG, "Calibration: Line Fitting");
    }

#else
    ESP_LOGW(TAG, "Calibration: Khong ho tro, dung raw");
#endif
}

// ===== ĐỌC ADC TRUNG BÌNH =====
static uint32_t read_vbat_mv(void) {
    int32_t sum = 0;

    for (int i = 0; i < ADC_SAMPLES; i++) {
        int raw = 0;
        adc_oneshot_read(adc_handle, BAT_ADC_CHANNEL, &raw);
        sum += raw;
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    int raw_avg = (int)(sum / ADC_SAMPLES);
    uint32_t vadc_mv = 0;

    if (cali_enable) {
        // Có calibration → chính xác
        adc_cali_raw_to_voltage(cali_handle, raw_avg, (int*)&vadc_mv);
    } else {
        // Không có calibration → tính thủ công
        vadc_mv = (uint32_t)((float)raw_avg / 4095.0f * 3300.0f);
    }

    return (uint32_t)(vadc_mv * DIVIDER_RATIO);
}

// ===== LOOKUP TABLE + NỘI SUY =====
static uint8_t voltage_to_percent(uint32_t vbat_mv) {
    if (vbat_mv >= LOOKUP[0].voltage_mv)             return 100;
    if (vbat_mv <= LOOKUP[LOOKUP_SIZE-1].voltage_mv) return 0;

    for (int i = 0; i < (int)LOOKUP_SIZE - 1; i++) {
        if (vbat_mv <= LOOKUP[i].voltage_mv &&
            vbat_mv >= LOOKUP[i+1].voltage_mv) {
            float ratio = (float)(vbat_mv             - LOOKUP[i+1].voltage_mv)
                        / (float)(LOOKUP[i].voltage_mv - LOOKUP[i+1].voltage_mv);
            return (uint8_t)(LOOKUP[i+1].percent
                   + ratio * (LOOKUP[i].percent - LOOKUP[i+1].percent));
        }
    }
    return 0;
}

// ===== TASK =====
static void battery_task(void* arg) {
    ESP_LOGI(TAG, "================================");
    ESP_LOGI(TAG, "   BATTERY MONITOR - ESP-IDF");
    ESP_LOGI(TAG, "================================");

    while (1) {
        uint32_t vbat_mv = read_vbat_mv();
        uint8_t  pct     = voltage_to_percent(vbat_mv);

        ESP_LOGI(TAG, "--------------------------------");
        ESP_LOGI(TAG, "Dien ap pin  : %"PRIu32".%02"PRIu32" V",
                 vbat_mv / 1000, (vbat_mv % 1000) / 10);
        ESP_LOGI(TAG, "Phan tram    : %d %%", pct);

        if      (pct > 50) ESP_LOGI(TAG, "Trang thai   : TOT");
        else if (pct > 20) ESP_LOGW(TAG, "Trang thai   : TRUNG BINH");
        else if (pct > 10) ESP_LOGW(TAG, "Trang thai   : YEU - hay sac!");
        else               ESP_LOGE(TAG, "Trang thai   : RAT YEU - sac ngay!");

        ESP_LOGI(TAG, "--------------------------------");

        vTaskDelay(pdMS_TO_TICKS(INTERVAL_MS));
    }
}

// ===== APP MAIN =====
void app_main(void) {
    adc_init();

    xTaskCreate(
        battery_task,
        "battery_task",
        4096,
        NULL,
        5,
        NULL
    );
}