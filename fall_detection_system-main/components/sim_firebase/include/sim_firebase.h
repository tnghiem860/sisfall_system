#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ── Cấu hình Firebase ─────────────────────────────────────────────
#define FIREBASE_PROJECT_ID   "sis-fall"
#define FIREBASE_API_KEY      "AIzaSyCLcLjmODx8oOUe26YhATqyb4Jd1fg4ZZs"
#define FIREBASE_HOST         "firestore.googleapis.com"

/**
 * @brief Khởi tạo module Firebase (thiết lập HTTP URL cho EG800K)
 *        Gọi một lần sau khi PDP context đã active.
 */
void sim_firebase_init(void);

/**
 * @brief Kích hoạt PDP context để có IP (cần cho HTTP).
 *        Tự dò APN theo nhà mạng (đọc IMSI, thử APN tương ứng rồi các APN
 *        còn lại) và lưu APN chạy được vào NVS theo IMSI của SIM.
 * @return true nếu thành công
 */
bool sim_firebase_start_pdp(void);

/**
 * @brief Cập nhật trạng thái thiết bị lên Firestore (upsert document).
 *        Path: /devices/{device_id}
 *        Gửi mỗi 2 phút. Đã bỏ trường rssi và timestamp.
 */
bool sim_firebase_update_status(const char *device_id,
                                 float lat,  float lon,
                                 float gps_accuracy,
                                 int   battery_pct,
                                 bool  fall_detected,
                                 int8_t ack_fall_opt,
                                 float confidence);

/**
 * @brief Gửi event té ngã lên collection fall_events.
 *        Sử dụng Firestore serverTimestamp cho trường fall_time
 *        để đảm bảo thời gian chính xác (không phụ thuộc NTP trên ESP32).
 */
bool sim_firebase_push_fall_event(const char *device_id,
                                   float lat,  float lon,
                                   float confidence,
                                   int   battery_pct);

/**
 * @brief Lấy trạng thái từ Firebase (emergency_mode và ack_fall)
 */
bool sim_firebase_get_commands(const char *device_id, bool *emergency_mode, bool *ack_fall);

#define SIM_MAX_SMS_NUMBERS  5    // tối đa số nhận SMS lấy từ Firebase (mỗi số = 1 SMS/lần báo)
#define SIM_PHONE_BUF        20   // buffer cho 1 số (tối đa 16 ký tự + '\0')

typedef enum {
    PHONE_OK,        // đọc được ít nhất 1 số hợp lệ
    PHONE_ABSENT,    // đọc document được nhưng chưa có field sms_numbers
    PHONE_INVALID,   // có field nhưng không có số hợp lệ (rỗng / sai định dạng)
    PHONE_NET_ERR,   // không đọc được document (lỗi mạng, rules, ...)
} phone_status_t;

/**
 * @brief Đọc danh sách số nhận SMS từ Firestore: devices/{device_id}.sms_numbers
 *        (mảng chuỗi). Mỗi số được chuẩn hóa (bỏ khoảng trắng, '-', '.'), chỉ nhận
 *        chữ số / '+' đầu, bỏ số trùng.
 * @param numbers  mảng đầu ra [max][SIM_PHONE_BUF]
 * @param count    số lượng số hợp lệ đọc được (chỉ dùng khi trả PHONE_OK)
 */
phone_status_t sim_firebase_get_sms_numbers(const char *device_id,
                                            char (*numbers)[SIM_PHONE_BUF],
                                            int max, int *count);

/**
 * @brief Ghi mảng sms_numbers gồm 1 số mặc định vào devices/{device_id}.
 *        Chỉ đụng field sms_numbers (updateMask). Chỉ nên gọi khi
 *        sim_firebase_get_sms_numbers trả PHONE_ABSENT.
 */
bool sim_firebase_seed_sms_numbers(const char *device_id, const char *phone);


#ifdef __cplusplus
}
#endif
