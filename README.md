# Automatic Attendance System

Hệ thống điểm danh tự động tích hợp đa cảm biến sử dụng vi điều khiển ESP32 và FreeRTOS.

## Tính năng chính
- **Điểm danh đa phương thức:**
  - Cảm biến vân tay (Adafruit Optical Fingerprint Sensor qua Serial2)
  - Thẻ từ RFID RC522 (Giao tiếp SPI)
  - Bàn phím ma trận 4x4 (Keypad) nhập mã PIN / chức năng quản lý
- **Màn hình hiển thị:** LCD I2C 16x2 hiển thị thông báo, thời gian và trạng thái điểm danh.
- **Xử lý bất đồng bộ:** Sử dụng FreeRTOS Queue và Task để gửi dữ liệu điểm danh qua HTTP Client mà không gây nghẽn luồng xử lý chính.
- **Đồng bộ thời gian:** Tự động lấy thời gian thực từ NTP Server qua kết nối WiFi.
- **Lưu trữ cấu hình:** Sử dụng ESP32 `Preferences` để lưu trữ dữ liệu người dùng và thiết lập an toàn trong bộ nhớ flash.

## Yêu cầu phần cứng
- ESP32 Development Board
- Cảm biến vân tay quang học (AS608 / FPM10A / R307 /...)
- Module đọc thẻ RFID-RC522
- Bàn phím ma trận 4x4
- Màn hình LCD 1602 kèm Module I2C
- Nguồn cấp ổn định (5V / 2A)

## Thư viện sử dụng
- `Adafruit_Fingerprint`
- `MFRC522`
- `Keypad`
- `LiquidCrystal_I2C`
- `Preferences`
- `WiFi`, `HTTPClient`, `WiFiClientSecure`
