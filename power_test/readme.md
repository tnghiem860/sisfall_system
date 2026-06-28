Dưới đây là các lưu đồ hoạt động của hệ thống (Firmware Flowchart) được chia nhỏ theo từng chức năng để tiện theo dõi và đưa vào báo cáo:

### 1. Lưu đồ Hoạt động chính (Main System Flow)
Quản lý trạng thái khởi động, ngủ sâu (Deep Sleep) và đánh thức (Wake-up) bằng các sự kiện khác nhau.

```mermaid
flowchart TD
    Start(["Khởi động hệ thống"]) --> Init["Khởi tạo MPU6050, AI Model, SIM/Firebase"]
    Init --> ConfigWakeup["Cấu hình Wakeup: INT MPU, SOS Button, Timer"]
    ConfigWakeup --> WaitLoop{"Chờ sự kiện / Sleep"}

    WaitLoop -->|"Ngắt MPU6050"| HandleMPU["Chuyển sang: Khối Tiền xử lý MPU"]
    WaitLoop -->|"Hàng đợi GPS/MQTT"| ProcessGPS["Cập nhật tọa độ lên Firebase"]
    WaitLoop -->|"Timer 30p"| ProcessStatus["Báo cáo trạng thái định kỳ"]
    WaitLoop -->|"Ngắt SOS"| ProcessSOS["Chuyển sang: Lưu đồ Cảnh báo & Hủy"]

    ProcessGPS --> WaitLoop
    ProcessStatus --> WaitLoop
    HandleMPU -.-> WaitLoop
    ProcessSOS -.-> WaitLoop
```

### 2. Khối Tiền xử lý Dữ liệu & Bắt ngưỡng Va chạm (Data Collection & Impact Detection)
Thực hiện đọc MPU6050 liên tục và tính toán Vector gia tốc để phát hiện khoảnh khắc va chạm ban đầu.

```mermaid
flowchart TD
    Start(["Ngắt MPU6050"]) --> ReadData["Đọc dữ liệu Gia tốc X, Y, Z"]
    ReadData --> RingBuffer["Lưu mẫu vào Circular Buffer (150 mẫu)"]
    RingBuffer --> CalcAM2["Tính độ lớn gia tốc AM² = X² + Y² + Z²"]
    
    CalcAM2 --> CheckImpact{"AM² > 2.56g² ?"}
    
    CheckImpact -->|"Có (Va chạm)"| PostFallConfig["Đánh dấu sự kiện va chạm<br/>Chuyển trạng thái: Thu thập hậu va chạm"]
    CheckImpact -->|"Không"| IsImpactDetected{"Đang ở trạng thái<br/>Thu thập hậu va chạm?"}
    
    PostFallConfig --> Return["Trở lại Chờ sự kiện"]
    IsImpactDetected -->|"Không"| Return
    IsImpactDetected -->|"Có"| NextPhase["Chuyển sang: Khối Suy luận AI"]
```

### 3. Khối Chạy Suy luận AI & Đánh giá (Window Extraction & AI Inference)
Chờ thu thập đủ các mẫu dữ liệu đệm sau cú ngã để đảm bảo điểm va chạm nằm ở giữa cửa sổ dữ liệu, sau đó đưa vào mạng Neural Network.

```mermaid
flowchart TD
    Start(["Chuyển từ Khối Tiền xử lý"]) --> CountSamples{"Đã thu thập đủ<br/>75 mẫu hậu va chạm?"}
    
    CountSamples -->|"Chưa đủ"| Return["Trở lại Chờ sự kiện (Tiếp tục lấy mẫu)"]
    CountSamples -->|"Đủ"| BuildWindow["Trích xuất cửa sổ 150 mẫu<br/>quanh tâm va chạm"]
    
    BuildWindow --> AI_Inference["Chạy mô hình AI TFLite (Int8)"]
    
    AI_Inference --> CheckAI{"AI đánh giá là Té ngã<br/>(Confidence >= 50%)?"}
    
    CheckAI -->|"Không (Bình thường)"| ResetState["Reset trạng thái Va chạm"]
    ResetState --> Return
    
    CheckAI -->|"Có (Té ngã)"| TriggerAlert["Chuyển sang: Lưu đồ Cảnh báo"]
```

### 4. Lưu đồ Xử lý Cảnh báo & Hủy báo động (Alert & Cancellation Logic)
Kích hoạt khi AI xác định té ngã hoặc người dùng chủ động nhấn nút SOS.

```mermaid
flowchart TD
    Start(["AI kết luận Té Ngã / Nhấn SOS"]) --> CheckCooldown{"Đã qua thời gian<br/>Cooldown chưa?"}

    CheckCooldown -->|"Chưa"| Return["Trở lại Chờ sự kiện"]
    CheckCooldown -->|"Đã qua"| BuzzerCountdown["Đếm ngược còi báo động (5s)"]

    BuzzerCountdown --> CheckCancel{"Người dùng nhấn<br/>nút SOS để hủy?"}

    CheckCancel -->|"Có (Hủy)"| SendCancelEvent["Gửi cập nhật trạng thái: Hủy báo động"]
    CheckCancel -->|"Không (Bỏ qua)"| FetchGPS["Tiến trình nền: Bật SIM & lấy GPS"]
    
    FetchGPS --> PushFirebase["Đẩy cảnh báo lên Firebase"]
    PushFirebase --> CheckACK{"Chờ 20s xem App<br/>có xác nhận (ACK)?"}
    
    CheckACK -->|"Có ACK"| EndAlert["Kết thúc báo động (Tiết kiệm SMS)"]
    CheckACK -->|"Không ACK / Mất mạng"| SendSMS["Gửi SMS dự phòng kèm tọa độ"]
    
    SendCancelEvent --> Return
    SendSMS --> Return
    EndAlert --> Return
```
