import shutil

shutil.copy('3.md', '3_backup.md')

with open('3.md', 'r', encoding='utf-8') as f:
    lines = f.readlines()

out = []

for line in lines:
    if line.startswith('![](data:image') and len(line) > 1000:
        continue
        
    out.append(line)
    
    if '3.5.1 Lưu đồ thuật toán' in line:
        out.append('\n**1. Lưu đồ Hoạt động chính (Main System Flow)**\n\n')
        out.append('```mermaid\nflowchart TD\n    Start(["Khởi động hệ thống"]) --> Init["Khởi tạo MPU6050, AI Model, SIM/Firebase"]\n    Init --> ConfigWakeup["Cấu hình Wakeup: INT MPU, SOS Button, Timer"]\n    ConfigWakeup --> WaitLoop{"Chờ sự kiện / Sleep"}\n\n    WaitLoop -->|"Ngắt MPU6050"| HandleMPU["Chuyển sang: Khối Tiền xử lý MPU"]\n    WaitLoop -->|"Hàng đợi GPS/MQTT"| ProcessGPS["Cập nhật tọa độ lên Firebase"]\n    WaitLoop -->|"Timer 30p"| ProcessStatus["Báo cáo trạng thái định kỳ"]\n    WaitLoop -->|"Ngắt SOS"| ProcessSOS["Chuyển sang: Lưu đồ Cảnh báo & Hủy"]\n\n    ProcessGPS --> WaitLoop\n    ProcessStatus --> WaitLoop\n    HandleMPU -.-> WaitLoop\n    ProcessSOS -.-> WaitLoop\n```\n\n')
        
    elif '3.5.2 Cơ chế phát hiện té ngã' in line:
        out.append('\n**2. Khối Tiền xử lý Dữ liệu & Bắt ngưỡng Va chạm (Data Collection & Impact Detection)**\n\n')
        out.append('```mermaid\nflowchart TD\n    Start(["Ngắt MPU6050"]) --> ReadData["Đọc dữ liệu Gia tốc X, Y, Z"]\n    ReadData --> RingBuffer["Lưu mẫu vào Circular Buffer (150 mẫu)"]\n    RingBuffer --> CalcAM2["Tính độ lớn gia tốc AM² = X² + Y² + Z²"]\n    \n    CalcAM2 --> CheckImpact{"AM² > 2.56g² ?"}\n    \n    CheckImpact -->|"Có (Va chạm)"| PostFallConfig["Đánh dấu sự kiện va chạm<br/>Chuyển trạng thái: Thu thập hậu va chạm"]\n    CheckImpact -->|"Không"| IsImpactDetected{"Đang ở trạng thái<br/>Thu thập hậu va chạm?"}\n    \n    PostFallConfig --> Return["Trở lại Chờ sự kiện"]\n    IsImpactDetected -->|"Không"| Return\n    IsImpactDetected -->|"Có"| NextPhase["Chuyển sang: Khối Suy luận AI"]\n```\n\n')
        out.append('\n**3. Khối Chạy Suy luận AI & Đánh giá (Window Extraction & AI Inference)**\n\n')
        out.append('```mermaid\nflowchart TD\n    Start(["Chuyển từ Khối Tiền xử lý"]) --> CountSamples{"Đã thu thập đủ<br/>75 mẫu hậu va chạm?"}\n    \n    CountSamples -->|"Chưa đủ"| Return["Trở lại Chờ sự kiện (Tiếp tục lấy mẫu)"]\n    CountSamples -->|"Đủ"| BuildWindow["Trích xuất cửa sổ 150 mẫu<br/>quanh tâm va chạm"]\n    \n    BuildWindow --> AI_Inference["Chạy mô hình AI TFLite (Int8)"]\n    \n    AI_Inference --> CheckAI{"AI đánh giá là Té ngã<br/>(Confidence >= 50%)?"}\n    \n    CheckAI -->|"Không (Bình thường)"| ResetState["Reset trạng thái Va chạm"]\n    ResetState --> Return\n    \n    CheckAI -->|"Có (Té ngã)"| TriggerAlert["Chuyển sang: Lưu đồ Cảnh báo"]\n```\n\n')
        
    elif '3.5.4 Cơ chế truyền dữ liệu cảnh báo' in line:
        out.append('\n**4. Lưu đồ Xử lý Cảnh báo & Hủy báo động (Alert & Cancellation Logic)**\n\n')
        out.append('```mermaid\nflowchart TD\n    Start(["AI kết luận Té Ngã / Nhấn SOS"]) --> CheckCooldown{"Đã qua thời gian<br/>Cooldown chưa?"}\n\n    CheckCooldown -->|"Chưa"| Return["Trở lại Chờ sự kiện"]\n    CheckCooldown -->|"Đã qua"| BuzzerCountdown["Đếm ngược còi báo động (5s)"]\n\n    BuzzerCountdown --> CheckCancel{"Người dùng nhấn<br/>nút SOS để hủy?"}\n\n    CheckCancel -->|"Có (Hủy)"| SendCancelEvent["Gửi cập nhật trạng thái: Hủy báo động"]\n    CheckCancel -->|"Không (Bỏ qua)"| FetchGPS["Tiến trình nền: Bật SIM & lấy GPS"]\n    \n    FetchGPS --> PushFirebase["Đẩy cảnh báo lên Firebase"]\n    PushFirebase --> CheckACK{"Chờ 20s xem App<br/>có xác nhận (ACK)?"}\n    \n    CheckACK -->|"Có ACK"| EndAlert["Kết thúc báo động (Tiết kiệm SMS)"]\n    CheckACK -->|"Không ACK / Mất mạng"| SendSMS["Gửi SMS dự phòng kèm tọa độ"]\n    \n    SendCancelEvent --> Return\n    SendSMS --> Return\n    EndAlert --> Return\n```\n\n')

with open('3.md', 'w', encoding='utf-8') as f:
    f.writelines(out)

print('Finished rewriting 3.md!')
