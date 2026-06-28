import socket
import threading
import time
import csv
import os
from datetime import datetime
import matplotlib.pyplot as plt
import matplotlib.animation as animation
import matplotlib.patches as mpatches
import matplotlib.gridspec as gridspec
from collections import deque

# --- CONFIGURATION ---
UDP_IP = "0.0.0.0"
UDP_PORT = 5005
SAMPLE_RATE = 25
WINDOW_SECONDS = 6  # 150 mẫu @ 25Hz = 6 giây
MAX_POINTS = WINDOW_SECONDS * SAMPLE_RATE
CSV_FILENAME = f"fall_test_log_{datetime.now().strftime('%Y%m%d_%H%M%S')}.csv"
SNAPSHOT_DIR = "fall_snapshots"

# Tạo folder lưu ảnh
os.makedirs(SNAPSHOT_DIR, exist_ok=True)
print(f"[*] Ảnh snapshot sẽ được lưu vào: {os.path.abspath(SNAPSHOT_DIR)}/")

# --- DATA STORAGE ---
times    = deque(maxlen=500)
x_data   = deque(maxlen=500)
y_data   = deque(maxlen=500)
z_data   = deque(maxlen=500)
am2_data = deque(maxlen=500)

fall_events    = []  # [(timestamp, confidence)]
window_events  = []  # [(win_start, win_end, is_fall, confidence)]
pending_snapshots = []  # Queue ảnh cần lưu từ thread UDP -> main thread

# Create socket
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.bind((UDP_IP, UDP_PORT))

print(f"[*] Listening for UDP packets on port {UDP_PORT}...")
print(f"[*] Saving data to {CSV_FILENAME}")

# Initialize CSV
with open(CSV_FILENAME, mode='w', newline='') as file:
    csv.writer(file).writerow(["Timestamp", "Type", "X", "Y", "Z", "AM2", "Isfall", "Confidence"])

start_time = time.time()
snapshot_lock = threading.Lock()

def udp_listener():
    while True:
        try:
            data, addr = sock.recvfrom(1024)
            message = data.decode('utf-8').strip()
            current_time = time.time() - start_time

            # Format 1: DATA:X,Y,Z,AM2
            if message.startswith("DATA:"):
                parts = message.replace("DATA:", "").split(",")
                if len(parts) == 4:
                    x, y, z, am2 = map(float, parts)
                    times.append(current_time)
                    x_data.append(x)
                    y_data.append(y)
                    z_data.append(z)
                    am2_data.append(am2)
                    with open(CSV_FILENAME, mode='a', newline='') as f:
                        csv.writer(f).writerow([current_time, "DATA", x, y, z, am2, 0, ""])

            # Format 2: FALL:confidence
            elif message.startswith("FALL:"):
                confidence = float(message.replace("FALL:", ""))
                print(f"[!] FALL DETECTED! Confidence: {confidence*100:.1f}% at {current_time:.1f}s")
                fall_events.append((current_time, confidence))
                with open(CSV_FILENAME, mode='a', newline='') as f:
                    csv.writer(f).writerow([current_time, "FALL", "", "", "", "", 1, confidence])

            # Format 3: WINDOW:is_fall,confidence
            elif message.startswith("WINDOW:"):
                parts = message.replace("WINDOW:", "").split(",")
                if len(parts) == 2:
                    is_fall = int(parts[0])
                    conf    = float(parts[1])
                    win_end   = current_time
                    win_start = current_time - WINDOW_SECONDS
                    window_events.append((win_start, win_end, is_fall, conf))

                    label = "FALL" if is_fall else "ADL"
                    print(f"[W] AI Window [{win_start:.1f}s ~ {win_end:.1f}s] → {label} ({conf*100:.1f}%)")
                    with open(CSV_FILENAME, mode='a', newline='') as f:
                        csv.writer(f).writerow([current_time, "WINDOW", "", "", "", "", is_fall, conf])

                    # Đánh dấu cần chụp ảnh snapshot cho window này
                    with snapshot_lock:
                        pending_snapshots.append((win_start, win_end, is_fall, conf, current_time))

        except Exception as e:
            print(f"Error: {e}")

# Snapshot: Vẽ và lưu ảnh cho 1 window AI
def save_snapshot(win_start, win_end, is_fall, conf, ts):
    t_arr  = list(times)
    x_arr  = list(x_data)
    y_arr  = list(y_data)
    z_arr  = list(z_data)
    am2_arr = list(am2_data)

    # Lọc chỉ lấy data trong vùng window (thêm buffer 1s mỗi bên)
    buf = 1.0
    t_filter = [t for t in t_arr if (win_start - buf) <= t <= (win_end + buf)]
    if len(t_filter) < 5:
        return  # Không đủ data để vẽ

    def filter_arr(arr):
        return [v for t, v in zip(t_arr, arr) if (win_start - buf) <= t <= (win_end + buf)]

    tx = filter_arr(x_arr)
    ty = filter_arr(y_arr)
    tz = filter_arr(z_arr)
    tam2 = filter_arr(am2_arr)

    # Tạo figure riêng (không ảnh hưởng figure live)
    fig_snap, (sa1, sa2) = plt.subplots(2, 1, figsize=(10, 6))
    fig_snap.patch.set_facecolor('#1a1a2e')

    label    = "FALL" if is_fall else "ADL"
    color_bg = '#ff572220' if is_fall else '#4caf5020'
    color_hl = '#ff5722'   if is_fall else '#4caf50'
    title_color = '#ff5722' if is_fall else '#4caf50'

    fig_snap.suptitle(
        f"AI Result: {label}  |  Confidence: {conf*100:.1f}%  |  t={ts:.1f}s",
        fontsize=14, fontweight='bold', color=title_color,
        backgroundcolor='#0f3460'
    )

    for ax in (sa1, sa2):
        ax.set_facecolor('#16213e')
        ax.tick_params(colors='white')
        ax.xaxis.label.set_color('white')
        ax.yaxis.label.set_color('white')
        ax.title.set_color('white')
        for spine in ax.spines.values():
            spine.set_edgecolor('#0f3460')
        # Vùng tô màu 150 mẫu
        ax.axvspan(win_start, win_end, alpha=0.15, color=color_hl)
        ax.axvline(x=win_end,   color=color_hl, lw=1.5, linestyle=':')
        ax.axvline(x=win_start, color=color_hl, lw=1.5, linestyle=':')

    # Plot ax1: X, Y, Z
    sa1.plot(t_filter, tx,  color='#e94560', lw=1.5, label='X')
    sa1.plot(t_filter, ty,  color='#0f9b58', lw=1.5, label='Y')
    sa1.plot(t_filter, tz,  color='#3282b8', lw=1.5, label='Z')
    sa1.set_ylim(-5, 5)
    sa1.set_ylabel('Gia tốc (g)', color='white')
    sa1.set_title('Gia tốc X, Y, Z', color='white')
    sa1.legend(loc='upper right', facecolor='#0f3460', labelcolor='white', fontsize=9)
    sa1.grid(True, color='#0f3460', alpha=0.5)

    # Text chú thích vùng window trên ax1
    mid = (win_start + win_end) / 2
    sa1.text(mid, 4.5, f"← 150 mẫu đưa vào AI ({WINDOW_SECONDS}s) →",
             ha='center', color=color_hl, fontsize=9, fontweight='bold')

    # Plot ax2: AM²
    sa2.plot(t_filter, tam2, color='#e040fb', lw=1.5, label='AM²')
    sa2.axhline(y=2.56, color='#ff9800', linestyle='--', lw=1.5, label='Ngưỡng 2.56g²')
    sa2.set_ylim(0, max(max(tam2) * 1.2, 4) if tam2 else 4)
    sa2.set_ylabel('AM² (g²)', color='white')
    sa2.set_xlabel('Thời gian (s)', color='white')
    sa2.set_title('Độ lớn gia tốc (AM²)', color='white')
    sa2.legend(loc='upper right', facecolor='#0f3460', labelcolor='white', fontsize=9)
    sa2.grid(True, color='#0f3460', alpha=0.5)

    # Vẽ đường FALL event nếu có trong khoảng này
    for (ft, fc) in fall_events:
        if (win_start - buf) <= ft <= (win_end + buf):
            sa1.axvline(x=ft, color='#ff1744', lw=2, linestyle='-', label=f'FALL {fc*100:.0f}%')
            sa2.axvline(x=ft, color='#ff1744', lw=2, linestyle='-')

    plt.tight_layout()

    # Tên file: FALL_087_20260628_103512.png
    ts_str  = datetime.now().strftime('%Y%m%d_%H%M%S')
    conf_str = f"{int(conf*100):03d}"
    filename = f"{label}_{conf_str}pct_{ts_str}.png"
    filepath = os.path.join(SNAPSHOT_DIR, filename)
    fig_snap.savefig(filepath, dpi=120, bbox_inches='tight', facecolor=fig_snap.get_facecolor())
    plt.close(fig_snap)
    print(f"[+] Snapshot saved: {filepath}")

# Start the UDP listener thread
thread = threading.Thread(target=udp_listener, daemon=True)
thread.start()

# --- MAIN PLOT SETUP ---
fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(12, 7))
fig.canvas.manager.set_window_title('Real-time Fall Detection Monitor')
fig.patch.set_facecolor('#1a1a2e')
for ax in (ax1, ax2):
    ax.set_facecolor('#16213e')
    ax.tick_params(colors='white')
    ax.xaxis.label.set_color('white')
    ax.yaxis.label.set_color('white')
    ax.title.set_color('white')
    for spine in ax.spines.values():
        spine.set_edgecolor('#0f3460')

line_x,   = ax1.plot([], [], color='#e94560', lw=1.5, label='X')
line_y,   = ax1.plot([], [], color='#0f9b58', lw=1.5, label='Y')
line_z,   = ax1.plot([], [], color='#3282b8', lw=1.5, label='Z')
line_am2, = ax2.plot([], [], color='#e040fb', lw=1.5, label='AM²')

ax1.set_ylim(-3, 3)
ax1.set_ylabel('Gia tốc (g)', color='white')
ax1.set_title('Gia tốc X, Y, Z (MPU6050)', color='white')
ax1.legend(loc='upper right', facecolor='#0f3460', labelcolor='white')
ax1.grid(True, color='#0f3460', alpha=0.5)

ax2.set_ylim(0, 10)
ax2.set_ylabel('AM² (g²)', color='white')
ax2.set_xlabel('Thời gian (s)', color='white')
ax2.set_title('Độ lớn gia tốc (AM²) + Kết quả AI', color='white')
ax2.axhline(y=2.56, color='#ff9800', linestyle='--', lw=1.5, label='Ngưỡng 2.56g²')
ax2.grid(True, color='#0f3460', alpha=0.5)

fall_vlines_ax1  = []
fall_vlines_ax2  = []
window_patches_ax1 = []
window_patches_ax2 = []

DISPLAY_DURATION = MAX_POINTS / SAMPLE_RATE

def update(frame):
    global fall_vlines_ax1, fall_vlines_ax2
    global window_patches_ax1, window_patches_ax2

    # ── Xử lý snapshot queue (phải chạy trên main thread matplotlib) ──
    with snapshot_lock:
        snaps = list(pending_snapshots)
        pending_snapshots.clear()
    for snap in snaps:
        try:
            save_snapshot(*snap)
        except Exception as e:
            print(f"[!] Snapshot error: {e}")

    if len(times) == 0:
        return line_x, line_y, line_z, line_am2

    t_arr = list(times)
    line_x.set_data(t_arr, list(x_data))
    line_y.set_data(t_arr, list(y_data))
    line_z.set_data(t_arr, list(z_data))
    line_am2.set_data(t_arr, list(am2_data))

    current_time = t_arr[-1]
    win_end   = max(DISPLAY_DURATION, current_time)
    win_start = win_end - DISPLAY_DURATION
    ax1.set_xlim(win_start, win_end)
    ax2.set_xlim(win_start, win_end)

    # Xóa marker cũ
    for obj in fall_vlines_ax1 + fall_vlines_ax2:
        obj.remove()
    fall_vlines_ax1.clear(); fall_vlines_ax2.clear()
    for p in window_patches_ax1 + window_patches_ax2:
        p.remove()
    window_patches_ax1.clear(); window_patches_ax2.clear()

    # Vẽ vùng tô 150 mẫu
    for (ws, we, is_fall, conf) in window_events:
        if we < win_start or ws > win_end:
            continue
        color = '#ff5722' if is_fall else '#4caf50'
        p1 = ax1.axvspan(ws, we, alpha=0.15, color=color)
        p2 = ax2.axvspan(ws, we, alpha=0.15, color=color)
        l1 = ax1.axvline(x=we, color=color, lw=1, linestyle=':')
        l2 = ax2.axvline(x=we, color=color, lw=1, linestyle=':')
        window_patches_ax1 += [p1, l1]
        window_patches_ax2 += [p2, l2]
        if win_start <= we <= win_end:
            label = f"{'FALL' if is_fall else 'ADL'} {conf*100:.0f}%"
            t1 = ax2.text(we + 0.05, 9.0, label, color=color, fontsize=8, fontweight='bold')
            window_patches_ax2.append(t1)

    # Vẽ đường dọc đỏ FALL event
    for (ft, conf) in fall_events:
        if win_start <= ft <= win_end:
            l1 = ax1.axvline(x=ft, color='#ff1744', lw=2, linestyle='-')
            l2 = ax2.axvline(x=ft, color='#ff1744', lw=2, linestyle='-')
            fall_vlines_ax1.append(l1)
            fall_vlines_ax2.append(l2)

    return line_x, line_y, line_z, line_am2

# Legend bổ sung
patch_fall = mpatches.Patch(color='#ff5722', alpha=0.4, label='Window → FALL')
patch_adl  = mpatches.Patch(color='#4caf50', alpha=0.4, label='Window → ADL')
line_fall  = plt.Line2D([0], [0], color='#ff1744', lw=2, label='FALL Event')
ax2.legend(handles=[ax2.get_lines()[0], ax2.get_lines()[1], patch_fall, patch_adl, line_fall],
           loc='upper right', facecolor='#0f3460', labelcolor='white', fontsize=8)

ani = animation.FuncAnimation(fig, update, interval=40, blit=False, cache_frame_data=False)
plt.tight_layout()
plt.show()
