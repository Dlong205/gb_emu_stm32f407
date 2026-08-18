# STM32 GameBoy Emulator Console

Dự án máy chơi game cầm tay (Handheld Console) tự chế, giả lập thành công hệ máy **Nintendo GameBoy** nguyên thuỷ, được phát triển trên vi điều khiển **STM32F407VE** kết hợp với màn hình **OLED 128x64** và thẻ nhớ vi mô (MicroSD). Dự án sử dụng nhân giả lập mã nguồn mở siêu nhẹ [Peanut-GB](https://github.com/deltabeard/Peanut-GB).

## 🚀 Tính Năng Chính
- **Đọc ROM GameBoy trực tiếp từ thẻ SD**: Tự động tìm kiếm, phân tích Header, kiểm tra mã CRC và load ROM GameBoy (như *Pokemon Red, Super Mario Land, v.v.*) từ giao tiếp tốc độ cao SDIO.
- **Trình xuất hình ảnh tối ưu**: Cân chỉnh tỉ lệ khung hình (Aspect Ratio) từ độ phân giải 160x144 gốc của GameBoy xuống màn hình OLED 128x64 thông qua các thuật toán Shift-Bit siêu tốc độ.
- **Quản lý bộ nhớ đệm (Cache) đỉnh cao**: Do thẻ nhớ SD có độ trễ cực lớn nên dự án đã được tích hợp bộ đệm (Direct-Mapped Cache) O(1) cùng cơ chế lưu trữ tĩnh Bank 0 trên RAM, đảm bảo tốc độ phản hồi lệnh của CPU tương đương bộ nhớ trong.

## 🛠 Phần Cứng (Hardware)
- **MCU**: Mạch phát triển STM32F407VE (Black Board) - Nhân Cortex-M4 @ 168MHz.
- **Màn Hình**: OLED 0.96" hoặc 1.3" (Giao tiếp I2C - SSD1306).
- **Lưu Trữ**: Khe cắm MicroSD sử dụng chuẩn SDIO 1-bit.
- **Môi Trường Phát Triển**: PlatformIO & STM32Cube HAL.

## 📈 Tiến Độ Dự Án Đến Hiện Tại (Status & Progress)
Dự án đã vượt qua giai đoạn Proof-of-Concept (POC) và đang trong giai đoạn tối ưu hoá sâu phần lõi. Dưới đây là các cột mốc đã hoàn thiện:

- [x] **Khởi tạo hệ thống cốt lõi**: Hoàn tất cấu hình HAL cho I2C, SDIO và System Clock (168MHz).
- [x] **Tích hợp File System**: Tích hợp FATFS để thao tác trực tiếp với file ROM (.GB).
- [x] **Sửa lỗi Boot ROM**: Khắc phục thành công lỗi sai bộ đệm thẻ SD (`GB_INIT_INVALID_CHECKSUM` / Lỗi e=2) khiến ROM bị hỏng dữ liệu khi load vào Peanut-GB. 
- [x] **Tối ưu hoá hiệu năng (Performance Boost)**: 
  - Đã đập bỏ thuật toán tìm kiếm tuyến tính O(N) gây rớt khung hình.
  - Viết lại hàm `gb_rom_read_cb` sử dụng thiết kế **Direct Mapped Cache 32 vùng** (16KB) không đụng độ (Collision-free) cho Bank N và một **Dedicated RAM Bank** cho Bank 0.
  - Vượt qua giới hạn tốc độ I2C của màn hình bằng thuật toán **Bỏ qua khung hình (Frame Skip)**.
  - Tối ưu hóa vòng lặp `render_to_oled` bằng cách tính toán thẳng bảng bù toạ độ (LUT) và ghi trực tiếp Byte (không qua hàm DrawPixel tốn tài nguyên).
  - Ép xung SDIO `ClockDiv=0x04` và loại bỏ cờ biên dịch tiết kiệm flash `-Os` để ép trình biên dịch GCC mở hết tốc lực bằng cờ `-O3`. Kết quả FPS đã bứt phá từ mức 5 FPS lên mức có thể chơi được.

### Kế Hoạch Sắp Tới (TODOs)
- [ ] Tích hợp phím bấm (Buttons / D-Pad) qua ngắt ngoài (EXTI).
- [ ] Bổ sung âm thanh (Audio/PWM hoặc I2S).
- [ ] Chuyển đổi màn hình sang chuẩn giao tiếp SPI (để tháo gỡ hoàn toàn giới hạn I2C Bottleneck 400kHz và đạt mượt mà 60 FPS).
- [ ] Lưu/Tải File Save (.SAV) của game lên thẻ nhớ.

## 📝 Hướng Dẫn Biên Dịch (Build & Upload)
Mở dự án này bằng **VSCode + PlatformIO**.
Đảm bảo bạn đã cắm mạch ST-Link vào board STM32F407.

1. Nhấn nút **Clean** (Thùng rác) để xoá bỏ các build cũ (đặc biệt khi vừa thay đổi cờ tối ưu `-O3`).
2. Copy 1 file ROM (.GB) vào thẻ nhớ SD, cắm vào mạch.
3. Nhấn **Build** và **Upload** để nạp thẳng firmware vào chip.

---
*Developed with PlatformIO & STM32 HAL.*
