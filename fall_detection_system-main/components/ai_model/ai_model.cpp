#include "ai_model.h"
#include "model_data.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/system_setup.h"
#include "tensorflow/lite/schema/schema_generated.h"

static const char *TAG = "AI_MODEL";

namespace {
    // Tăng từ 11 lên 14 để dự phòng thêm các Op mới sinh ra từ Reshape
    using Resolver = tflite::MicroMutableOpResolver<14>;
    Resolver                   *resolver      = nullptr;
    const tflite::Model        *model         = nullptr;
    tflite::MicroInterpreter   *interpreter   = nullptr;
    TfLiteTensor               *input_tensor  = nullptr;
    TfLiteTensor               *output_tensor = nullptr;

    // FIX (dram): Dùng heap thay vì static array để tránh tràn BSS segment.
    // Model thực tế chỉ dùng ~5KB, đặt 20KB để có headroom an toàn.
    constexpr size_t ARENA_SIZE = 20 * 1024;
    static uint8_t *tensor_arena = nullptr;
}

esp_err_t ai_model_init(void)
{
    tflite::InitializeTarget();

    if (tensor_arena == nullptr) {
        tensor_arena = (uint8_t *)heap_caps_malloc(ARENA_SIZE, MALLOC_CAP_8BIT);
        if (tensor_arena == nullptr) {
            ESP_LOGE(TAG, "Không cấp được %u bytes cho tensor_arena!", (unsigned)ARENA_SIZE);
            return ESP_ERR_NO_MEM;
        }
    }

    model = tflite::GetModel(fd_cnn_model);
    if (model->version() != TFLITE_SCHEMA_VERSION) {
        ESP_LOGE(TAG, "Model schema mismatch: %lu vs %d",
                 model->version(), TFLITE_SCHEMA_VERSION);
        return ESP_FAIL;
    }

    static Resolver static_resolver;
    resolver = &static_resolver;
    resolver->AddExpandDims();
    resolver->AddConv2D();
    resolver->AddMaxPool2D();
    resolver->AddFullyConnected();
    resolver->AddSoftmax();
    resolver->AddReshape();
    resolver->AddMean();
    resolver->AddAdd();
    resolver->AddMul();
    resolver->AddQuantize();
    resolver->AddDequantize();
    
    // Thêm các Op mới được sinh ra từ Reshape layer
    resolver->AddShape();
    resolver->AddStridedSlice();
    resolver->AddPack();

    static tflite::MicroInterpreter static_interpreter(
        model, *resolver, tensor_arena, ARENA_SIZE);
    interpreter = &static_interpreter;

    if (interpreter->AllocateTensors() != kTfLiteOk) {
        ESP_LOGE(TAG, "AllocateTensors failed — tăng ARENA_SIZE (hiện tại %u bytes)",
                 (unsigned)ARENA_SIZE);
        return ESP_FAIL;
    }

    input_tensor  = interpreter->input(0);
    output_tensor = interpreter->output(0);

    // Kiểm tra shape: [1, 150, 3]
    ESP_LOGI(TAG, "Input  dims: [%d, %d, %d]",
             input_tensor->dims->data[0],
             input_tensor->dims->data[1],
             input_tensor->dims->data[2]);
    ESP_LOGI(TAG, "Output dims: [%d, %d]",
             output_tensor->dims->data[0],
             output_tensor->dims->data[1]);

    // FIX (arena): Log để calibrate ARENA_SIZE — đặt = used * 1.2
    size_t used = interpreter->arena_used_bytes();
    ESP_LOGI(TAG, "Arena used: %u / %u bytes (%.0f%% full)",
             (unsigned)used, (unsigned)ARENA_SIZE,
             100.0f * used / ARENA_SIZE);
    if (used > ARENA_SIZE * 9 / 10) {
        ESP_LOGW(TAG, "Arena >90%% full — tăng ARENA_SIZE để tránh lỗi runtime");
    }

    return ESP_OK;
}

esp_err_t ai_model_run(const float window[][AI_NUM_CHANNELS],
                        ai_result_t *result)
{
    if (!interpreter || !result) return ESP_ERR_INVALID_STATE;

    int8_t *in = input_tensor->data.int8;

    for (int t = 0; t < AI_WINDOW_SIZE; t++) {
        for (int c = 0; c < AI_NUM_CHANNELS; c++) {
            // Sử dụng tham số từ model_data.h (tự động khớp với model mới)
            float norm = 2.0f * (window[t][c] - NORM_ACCEL_MIN) / (NORM_ACCEL_MAX - NORM_ACCEL_MIN) - 1.0f;
            
            // Công thức Quantize mới
            int32_t q = (int32_t)roundf(norm / INPUT_SCALE + INPUT_ZERO_POINT);
            
            if (q >  127) q =  127;
            if (q < -128) q = -128;
            in[t * AI_NUM_CHANNELS + c] = (int8_t)q;
        }
    }

    if (interpreter->Invoke() != kTfLiteOk) {
        ESP_LOGE(TAG, "Invoke failed");
        return ESP_FAIL;
    }

    int8_t *out = output_tensor->data.int8;
    result->raw_out[0] = out[0];  // ADL
    result->raw_out[1] = out[1];  // Fall

    float prob_adl  = (out[0] - OUTPUT_ZERO_POINT) * OUTPUT_SCALE;
    float prob_fall = (out[1] - OUTPUT_ZERO_POINT) * OUTPUT_SCALE;

    result->is_fall    = (prob_fall > prob_adl);
    result->confidence = result->is_fall ? prob_fall : prob_adl;

    ESP_LOGD(TAG, "ADL=%.3f  FALL=%.3f  → %s",
             prob_adl, prob_fall,
             result->is_fall ? "FALL" : "ADL");

    return ESP_OK;
}
