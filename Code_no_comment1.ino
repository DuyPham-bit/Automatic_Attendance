#include <Adafruit_Fingerprint.h>
#include <HTTPClient.h>
#include <Keypad.h>
#include <LiquidCrystal_I2C.h>
#include <MFRC522.h>
#include <Preferences.h>
#include <SPI.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <Wire.h>
#include <time.h>

#define MAX_USERS 128
#define ADMIN_ID 1
#define QUEUE_SIZE 50
#define ADMIN_TIMEOUT 30000

// Cấu trúc lưu trữ dữ liệu điểm danh để gửi lên mạng
struct HttpJob {
  char action[16];  // Loại hành động: enroll, attend, delete
  int fingerID;     // ID vân tay của người dùng
  char rfidUID[20]; // Mã thẻ RFID
  char status[32];  // Trạng thái: On-Time, Late, Absent
};

// Khai báo Hàng đợi (Queue) và Task của FreeRTOS để gửi mạng chạy ngầm
QueueHandle_t httpQueue;
TaskHandle_t httpTaskHandle;

// Cấu hình các chân cho bàn phím ma trận 4x4
const byte ROWS = 4;
const byte COLS = 4;
char keys[ROWS][COLS] = {{'1', '2', '3', 'A'},
                         {'4', '5', '6', 'B'},
                         {'7', '8', '9', 'C'},
                         {'*', '0', '#', 'D'}};
byte rowPins[ROWS] = {13, 14, 27, 26};
byte colPins[COLS] = {25, 33, 2, 15};
Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

// Khởi tạo màn hình LCD I2C 16x2
LiquidCrystal_I2C lcd(0x27, 16, 2);

// Cấu hình các chân cho Module đọc thẻ RFID RC522
#define SS_PIN 5
#define RST_PIN 4
MFRC522 rfid(SS_PIN, RST_PIN);

// Cấu hình cảm biến vân tay sử dụng giao tiếp Serial2
#define FP_SERIAL Serial2
#define FP_RX 16
#define FP_TX 17
#define FP_BAUD 57600
Adafruit_Fingerprint finger = Adafruit_Fingerprint(&FP_SERIAL);
int fpFailCount = 0;

#define BUZZER 32

// Cấu hình mạng WiFi và đường dẫn Google Apps Script (GAS)
const char *ssid = "123456789";
const char *password = "12345679";
const String GAS_URL = "https://script.google.com/macros/s/"
                       "AKfycbyKFCw2EXTl0Z4eOBkDsTkUXsyvh0ZlPXmqistWFU80_"
                       "6EaaPZU12hwUBgSgnhYo0yiQA/exec";

int classStartHour = 12, classStartMin = 52;
int classEndHour = 12, classEndMin = 53;
int lateAfterMins = 0;

// Khai báo các chế độ hoạt động (State Machine) của hệ thống
enum SysMode {
  MODE_IDLE,           // Trạng thái rảnh (Hiển thị giờ)
  MODE_ADMIN_MENU,     // Đang trong Menu Quản trị
  MODE_REGISTRATION,   // Chế độ đăng ký người dùng
  MODE_DELETE,         // Chế độ chờ xóa người dùng
  MODE_DELETE_CONFIRM, // Chế độ xác nhận xóa
  MODE_ATTENDANCE,     // Giờ điểm danh vào (Check-in)
  MODE_CHECKOUT,       // Giờ tan học (Check-out)
  MODE_EARLY_LEAVE     // Xác nhận về sớm
};
SysMode currentMode = MODE_IDLE;
unsigned long manualModeStartTime = 0;
String inputDeleteID = "";
int targetDeleteID = -1;
int earlyLeaveID = -1;
String earlyLeaveRFID = "";

int currentTotalMins = 0;
int startTotalMins = classStartHour * 60 + classStartMin;
int endTotalMins = classEndHour * 60 + classEndMin;

#define GMT_OFFSET_SEC 25200
#define DAYLIGHT_OFFSET_SEC 0

Preferences prefs;
bool hasCheckedIn[MAX_USERS] = {false};
bool hasCheckedOut[MAX_USERS] = {false};
bool autoAbsentSent = false;
bool wifiWasConnected = false;

// Các biến toàn cục dùng cho điểm danh và hiển thị
unsigned long lastScanTime = 0;
int lastFingerID = -1;
String lastCardUID = "";
unsigned long lastDisplay = 0;
unsigned long lastWiFiRetry = 0;

// ==========================================
// 1. HÀM TIỆN ÍCH (HELPER FUNCTIONS)
// ==========================================

// Hàm hiển thị 2 dòng chữ lên LCD (Có chức năng chống nhấp nháy màn hình)
void lcdShow(String line1, String line2) {
  static String oldLine1 = "";
  static String oldLine2 = "";
  if (line1 != oldLine1 || line2 != oldLine2) {
    while (line1.length() < 16)
      line1 += " ";
    while (line2.length() < 16)
      line2 += " ";
    lcd.setCursor(0, 0);
    lcd.print(line1);
    lcd.setCursor(0, 1);
    lcd.print(line2);
    oldLine1 = line1;
    oldLine2 = line2;
  }
}

// Hàm phát tiếng bíp từ Còi chip (Buzzer)
void beep(int times, int duration) {
  for (int i = 0; i < times; i++) {
    digitalWrite(BUZZER, HIGH);
    delay(duration);
    digitalWrite(BUZZER, LOW);
    delay(50);
  }
}

String urlEncode(String str) {
  String encodedString = "";
  char c, code0, code1;
  for (int i = 0; i < str.length(); i++) {
    c = str.charAt(i);
    if (c == ' ') {
      encodedString += "%20";
    } else if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      encodedString += c;
    } else {
      code1 = (c & 0xf) + '0';
      if ((c & 0xf) > 9)
        code1 = (c & 0xf) - 10 + 'A';
      c = (c >> 4) & 0xf;
      code0 = c + '0';
      if (c > 9)
        code0 = c - 10 + 'A';
      encodedString += '%';
      encodedString += code0;
      encodedString += code1;
    }
  }
  return encodedString;
}

// Hàm hiển thị trạng thái màn hình chờ (Giờ giấc, trạng thái điểm danh, hướng dẫn phím)
void displayStatus(bool force = false) {
  if (force || millis() - lastDisplay > 1000) {
    lastDisplay = millis();
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo))
      return;

    int currentH = timeinfo.tm_hour;
    int currentM = timeinfo.tm_min;
    String timeStr = "";
    if (currentH < 10)
      timeStr += "0";
    timeStr += String(currentH) + ":";
    if (currentM < 10)
      timeStr += "0";
    timeStr += String(currentM);

    if (currentMode == MODE_ADMIN_MENU)
      lcdShow("A:Add D:Del", "C:Exit");
    else if (currentMode == MODE_REGISTRATION)
      lcdShow("A:FP B:Card+FP", "C:Back");
    else if (currentMode == MODE_DELETE) {
      lcdShow("Scan Card/FP", "B:Back");
    } else if (currentMode == MODE_DELETE_CONFIRM) {
      lcdShow("Del ID:" + String(targetDeleteID), "A:Confirm C:Cancel");
    } else if (currentMode == MODE_EARLY_LEAVE) {
      lcdShow("Early-CheckOut?", "A:Yes C:No");
    } else if (currentMode == MODE_ATTENDANCE)
      lcdShow(timeStr, "Check-In Open");
    else if (currentMode == MODE_CHECKOUT)
      lcdShow(timeStr, "Check-Out Open");
  }
}

// ==========================================
// 2. HÀM MẠNG & ĐỒNG BỘ DỮ LIỆU (NETWORK)
// ==========================================

// Hàm lưu tạm dữ liệu điểm danh vào bộ nhớ Flash (offline) khi mất WiFi
bool queueRecord(String action, int fingerID, String rfidUID, String status) {
  prefs.begin("pending", false);      // Mở không gian nhớ tên 'pending'
  int head = prefs.getInt("head", 0); // Vị trí đầu hàng đợi
  int tail = prefs.getInt("tail", 0); // Vị trí cuối hàng đợi
  int cnt = prefs.getInt("cnt", 0);   // Số lượng phần tử đang lưu

  if (cnt >= QUEUE_SIZE) {
    lcdShow("Queue Full!", "Oldest Dropped");
    beep(2, 100);
    Serial.println("[WARNING] Queue Full! Dropping oldest record.");
    delay(1500);
    prefs.remove(String(head).c_str());
    head = (head + 1) % QUEUE_SIZE;
    cnt--;
  }

  String record =
      action + "|" + String(fingerID) + "|" + rfidUID + "|" + status;
  size_t written = prefs.putString(String(tail).c_str(), record.c_str());
  if (written == 0) {
    prefs.end();
    Serial.println("[ERROR] Failed to save to Preferences");
    return false;
  }
  tail = (tail + 1) % QUEUE_SIZE;
  cnt++;
  prefs.putInt("head", head);
  prefs.putInt("tail", tail);
  prefs.putInt("cnt", cnt);
  prefs.end();
  Serial.println("[Queue] Saved (total: " + String(cnt) + ")");
  return true;
}

// Hàm gửi request HTTP GET tới Google Sheets để đẩy dữ liệu
int sendHttpGet(String url, int timeoutMs) {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.begin(client, url);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(timeoutMs);
  int httpCode = http.GET();
  http.end();
  return httpCode;
}

// Hàm đồng bộ dữ liệu (gửi các lượt điểm danh đang lưu offline lên mạng)
void flushQueue() {
  prefs.begin("pending", true);
  int head = prefs.getInt("head", 0);
  int cnt = prefs.getInt("cnt", 0);
  prefs.end();
  if (cnt == 0)
    return;

  lcdShow("Syncing...", String(cnt) + " records");

  int sent = 0;
  int idx = head;
  for (int i = 0; i < cnt; i++) {
    prefs.begin("pending", true);
    String record = prefs.getString(String(idx).c_str(), "");
    prefs.end();
    if (record == "") {
      sent++;
      idx = (idx + 1) % QUEUE_SIZE;
      continue;
    }

    int p1 = record.indexOf('|');
    int p2 = record.indexOf('|', p1 + 1);
    int p3 = record.indexOf('|', p2 + 1);
    if (p1 < 0 || p2 < 0 || p3 < 0) {
      sent++;
      idx = (idx + 1) % QUEUE_SIZE;
      continue;
    }

    String act = record.substring(0, p1);
    int fid = record.substring(p1 + 1, p2).toInt();
    String rfid = record.substring(p2 + 1, p3);
    String status = record.substring(p3 + 1);

    String url = GAS_URL + "?action=" + act + "&id=" + String(fid) +
                 "&rfid=" + rfid + "&status=" + urlEncode(status);
    int httpCode = sendHttpGet(url, 3000);

    if (httpCode == 200) {
      prefs.begin("pending", false);
      prefs.remove(String(idx).c_str());
      prefs.end();
      sent++;
    } else {
      break;
    }
    idx = (idx + 1) % QUEUE_SIZE;
    delay(200);
  }

  int newHead = (head + sent) % QUEUE_SIZE;
  int remaining = cnt - sent;
  prefs.begin("pending", false);
  prefs.putInt("head", newHead);
  prefs.putInt("cnt", remaining);
  prefs.end();

  if (remaining == 0) {
    lcdShow("Sync OK!", "Sent: " + String(sent));
    Serial.println("[Queue] All flushed!");
  } else {
    lcdShow("Sync Partial", String(sent) + "/" + String(cnt));
  }
  beep(1, 150);
  delay(2500);
}

// Task chạy ngầm của FreeRTOS: Liên tục lấy dữ liệu từ Queue để gửi mạng
void httpSendTask(void *param) {
  HttpJob job;
  while (true) {
    if (xQueueReceive(httpQueue, &job, portMAX_DELAY) == pdTRUE) {
      if (WiFi.status() != WL_CONNECTED) {
        queueRecord(String(job.action), job.fingerID, String(job.rfidUID),
                    String(job.status));
        Serial.println("[HTTP Task] Offline -> queued");
        continue;
      }
      String url = GAS_URL + "?action=" + String(job.action) +
                   "&id=" + String(job.fingerID) +
                   "&rfid=" + String(job.rfidUID) +
                   "&status=" + urlEncode(String(job.status));
      int httpCode = sendHttpGet(url, 5000);
      if (httpCode == 200) {
        Serial.println("[HTTP Task] Sent OK");
      } else {
        Serial.println("[HTTP Task] Failed -> queued");
        queueRecord(String(job.action), job.fingerID, String(job.rfidUID),
                    String(job.status));
      }
    }
  }
}

// Hàm đẩy lệnh vào Queue của FreeRTOS, nếu Queue đầy thì tự động lưu Offline
bool sendToSheet(String action, int fingerID, String rfidUID, String status) {
  HttpJob job;
  strncpy(job.action, action.c_str(), 15);
  job.action[15] = '\0';
  job.fingerID = fingerID;
  strncpy(job.rfidUID, rfidUID.c_str(), 19);
  job.rfidUID[19] = '\0';
  strncpy(job.status, status.c_str(), 31);
  job.status[31] = '\0';

  if (xQueueSend(httpQueue, &job, 0) == pdTRUE) {
    Serial.println("[sendToSheet] Job queued to FreeRTOS");
    return true;
  } else {
    Serial.println("[sendToSheet] FreeRTOS queue full -> save to Flash");
    return queueRecord(action, fingerID, rfidUID, status);
  }
}

// ==========================================
// 3. HÀM CẢM BIẾN & THIẾT BỊ (HARDWARE)
// ==========================================

// Hàm đọc thẻ RFID, trả về mã UID (chuỗi Hex) của thẻ
String getRFID() {
  if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial())
    return "";

  beep(1, 30);

  String uid = "";
  char buf[3];
  for (byte i = 0; i < rfid.uid.size; i++) {
    sprintf(buf, "%02X", rfid.uid.uidByte[i]);
    uid += buf;
  }
  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();
  return uid;
}

// Hàm đăng ký vân tay mới (Yêu cầu đặt tay 2 lần để xác nhận)
bool enrollFinger(int id) {
  int p = -1;

  delay(100);

  unsigned long start = millis();
  lcdShow("Place Finger", "Hold still...");
  while (p != FINGERPRINT_OK && millis() - start < 10000) {
    p = finger.getImage();
    delay(100);
  }
  if (p == FINGERPRINT_OK)
    beep(1, 30);

  if (p != FINGERPRINT_OK) {

    lcdShow("Timeout!", "Try Again");
    beep(1, 30);
    return false;
  }

  p = finger.image2Tz(1);
  Serial.print("[Enroll] image2Tz(1) = ");
  Serial.println(p);
  if (p != FINGERPRINT_OK) {
    lcdShow("Scan Error 1", "Try Again");
    beep(1, 30);
    return false;
  }

  p = finger.fingerFastSearch();
  if (p == FINGERPRINT_OK) {
    lcdShow("FP Exists!", "ID: " + String(finger.fingerID));
    beep(2, 100);
    delay(1500);
    return false;
  }

  lcdShow("Remove Finger", "Please...");
  delay(1000);

  unsigned long liftStart = millis();
  while (finger.getImage() != FINGERPRINT_NOFINGER) {
    if (millis() - liftStart > 5000)
      break;
    delay(50);
  }
  delay(300);

  p = -1;
  start = millis();
  lcdShow("Place Finger", "Again...");
  while (p != FINGERPRINT_OK && millis() - start < 10000) {
    p = finger.getImage();
    delay(100);
  }
  if (p == FINGERPRINT_OK)
    beep(1, 30);

  if (p != FINGERPRINT_OK) {
    lcdShow("Timeout!", "Try Again");
    beep(1, 30);
    return false;
  }

  p = finger.image2Tz(2);
  Serial.print("[Enroll] image2Tz(2) = ");
  Serial.println(p);
  if (p != FINGERPRINT_OK) {
    lcdShow("Scan Error 2", "Try Again");
    beep(1, 30);
    return false;
  }

  p = finger.createModel();
  Serial.print("[Enroll] createModel = ");
  Serial.println(p);
  if (p != FINGERPRINT_OK) {
    lcdShow("Match Error", "Use SAME finger!");
    beep(2, 100);
    delay(2000);
    return false;
  }

  p = finger.storeModel(id);
  if (p != FINGERPRINT_OK) {

    lcdShow("Save Error", "Try Again");
    beep(1, 30);
    return false;
  }

  lcdShow("Enroll OK!", "");
  beep(1, 150);
  return true;
}

// Hàm khởi động lại cảm biến vân tay nếu bị lỗi kết nối/treo
void reinitFP() {
  Serial2.end();
  delay(100);
  Serial2.begin(FP_BAUD, SERIAL_8N1, FP_RX, FP_TX);
  delay(300);
  if (!finger.verifyPassword()) {
    Serial.println("[FP] Reinit verify failed!");
  }
}

// Hàm quét vân tay nhanh (Không bị delay chặn luồng), trả về ID người dùng
int scanFingerNoWait() {
  uint8_t p = finger.getImage();

  if (p == FINGERPRINT_NOFINGER) {
    fpFailCount = 0;
    return 0;
  }

  Serial.print("getImage = ");
  Serial.println(p);

  if (p != FINGERPRINT_OK) {
    if (p == FINGERPRINT_PACKETRECIEVEERR) {
      fpFailCount++;
      Serial.print("[FP] PACKETRECIEVEERR count=");
      Serial.println(fpFailCount);
      if (fpFailCount >= 10) {
        Serial.println("[FP] Too many errors, reinitializing...");
        reinitFP();
        fpFailCount = 0;
      }
      lcdShow("Sensor Error", "Try again");
      delay(500);
    } else {
      lcdShow("Image Too Messy", "Place again");
    }
    return -1;
  }

  fpFailCount = 0;

  p = finger.image2Tz(1);
  Serial.print("image2Tz = ");
  Serial.println(p);

  if (p != FINGERPRINT_OK) {
    lcdShow("Place Finger Again", "Bad Image");
    return -1;
  }

  beep(1, 30);

  p = finger.fingerFastSearch();
  Serial.print("search = ");
  Serial.println(p);

  unsigned long waitLift = millis();
  while (finger.getImage() != FINGERPRINT_NOFINGER) {
    if (millis() - waitLift > 3000)
      break;
    delay(20);
  }

  if (p == FINGERPRINT_OK) {
    return (int)finger.fingerID;
  } else if (p == FINGERPRINT_NOTFOUND) {
    lcdShow("Unknown Finger", "Not registered");
    beep(2, 100);
    delay(3000);
    return -2;
  } else if (p == FINGERPRINT_PACKETRECIEVEERR) {
    Serial.println("[FP] Search: packet error");
    lcdShow("Comm Error", "Try again");
    delay(2500);
    return -1;
  } else {
    Serial.print("[FP] Search error code: ");
    Serial.println(p);
    lcdShow("Search Error", "Code: " + String(p));
    delay(1000);
    return -1;
  }
}

// ==========================================
// 4. HÀM QUẢN LÝ NGƯỜI DÙNG (USER MNGT)
// ==========================================

// Hàm đánh dấu người dùng đã tồn tại trong bộ nhớ Flash
void markUserActive(int id) {
  prefs.begin("vtay", false);
  prefs.putString(String(id).c_str(), "1");
  prefs.end();
}

// Hàm chuẩn bị xóa người dùng (Kiểm tra xem ID có tồn tại không)
void prepareDelete(int id) {
  if (id == 1) {
    lcdShow("Admin Locked", "Cannot Delete");
    beep(1, 30);
    delay(300);
    return;
  }
  prefs.begin("vtay", true);
  bool exists = prefs.getString(String(id).c_str(), "") != "";
  prefs.end();
  if (!exists) {
    lcdShow("Unknown User", "Not Found");
    beep(1, 30);
    delay(2000);
    return;
  }
  targetDeleteID = id;
  currentMode = MODE_DELETE_CONFIRM;
  manualModeStartTime = millis();
  beep(1, 80);
}

// Hàm xóa người dùng thông qua thao tác quẹt thẻ RFID
void deleteUser(String cardUID) {
  prefs.begin("bind", true);
  int fingerID = prefs.getInt(cardUID.c_str(), -1);
  prefs.end();
  if (fingerID == -1) {
    lcdShow("Not Found!", "Not registered");
    beep(1, 30);
    delay(2000);
    return;
  }
  prepareDelete(fingerID);
}

// Hàm xóa người dùng thông qua thao tác quét vân tay
void deleteUserByFingerID(int fingerID) { prepareDelete(fingerID); }

// Hàm tổng hợp: Đăng ký người dùng mới (Kết hợp Thẻ + Vân tay, hoặc chỉ Vân tay)
void registerNewUser(String cardUID = "") {
  if (cardUID != "") {
    prefs.begin("bind", true);
    int existingID = prefs.getInt(cardUID.c_str(), -1);
    prefs.end();
    if (existingID != -1) {
      lcdShow("Already Reg!", "ID: " + String(existingID));
      beep(1, 80);
      delay(300);
      return;
    }
  }

  int newID = -1;
  prefs.begin("vtay", true);
  for (int i = ADMIN_ID + 1; i < MAX_USERS; i++) {
    if (prefs.getString(String(i).c_str(), "") == "") {
      newID = i;
      break;
    }
  }
  prefs.end();
  if (newID == -1) {
    lcdShow("Error", "System Full!");
    delay(300);
    return;
  }

  lcdShow("Auto ID: " + String(newID),
          cardUID == "" ? "Place Finger..." : "Wait...");

  if (enrollFinger(newID)) {
    if (cardUID != "") {
      prefs.begin("bind", false);
      prefs.putInt(cardUID.c_str(), newID);
      prefs.end();
    }

    String revCard = (cardUID == "") ? "NORFID" : cardUID;
    prefs.begin("rev", false);
    prefs.putString(String(newID).c_str(), revCard);
    prefs.end();

    markUserActive(newID);
    sendToSheet("enroll", newID, revCard, "Thanh cong");
    lcdShow("Saved OK", "ID: " + String(newID));
    beep(1, 150);
  } else {
    lcdShow("Enroll Failed", "Try Again");
    beep(1, 30);
  }
  delay(300);
}

// ==========================================
// 5. HÀM NGHIỆP VỤ ĐIỂM DANH (ATTENDANCE)
// ==========================================

// Hàm tự động điểm danh VẮNG MẶT cho những người chưa đến khi hết giờ
void sendAutoAbsent() {

  lcdShow("Auto Absent", "Running...");

  bool isActive[MAX_USERS] = {false};
  prefs.begin("vtay", true);
  for (int i = 2; i < MAX_USERS; i++) {
    isActive[i] = (prefs.getString(String(i).c_str(), "") != "");
  }
  prefs.end();

  int count = 0;
  for (int i = 2; i < MAX_USERS; i++) {
    if (!isActive[i])
      continue;
    if (hasCheckedIn[i])
      continue;
    lcdShow("Absent:", "ID: " + String(i));
    sendToSheet("attend", i, "AUTO", "Absent");
    hasCheckedOut[i] = true;
    count++;
    delay(100);
  }

  if (count == 0) {
    lcdShow("Auto Absent", "Done. All In");
  } else {
    lcdShow("Auto Absent", "Sent " + String(count));
  }
  beep(1, 80);
  delay(1000);
}

// Cập nhật thời gian từ NTP và tự động đổi chế độ Check-in / Check-out
void updateTimeAndMode() {
  static unsigned long lastUpdate = 0;
  if (millis() - lastUpdate < 5000)
    return;
  lastUpdate = millis();

  struct tm timeinfo;
  if (!getLocalTime(&timeinfo))
    return;

  {
    int todayKey = timeinfo.tm_mday * 100 + timeinfo.tm_mon;
    prefs.begin("state", false);
    int savedDayKey = prefs.getInt("daykey", -1);
    if (savedDayKey != todayKey) {
      memset(hasCheckedIn, 0, sizeof(hasCheckedIn));
      memset(hasCheckedOut, 0, sizeof(hasCheckedOut));
      autoAbsentSent = false;
      prefs.putInt("daykey", todayKey);
      Serial.println("[DailyReset] New day");
    }
    prefs.end();
  }

  int currentH = timeinfo.tm_hour;
  int currentM = timeinfo.tm_min;
  currentTotalMins = currentH * 60 + currentM;

  if (endTotalMins >= 1440 && currentTotalMins < startTotalMins) {
    currentTotalMins += 1440;
  }

  if (currentMode != MODE_REGISTRATION && currentMode != MODE_DELETE &&
      currentMode != MODE_ADMIN_MENU && currentMode != MODE_DELETE_CONFIRM &&
      currentMode != MODE_EARLY_LEAVE) {

    if (currentTotalMins < endTotalMins) {
      currentMode = MODE_ATTENDANCE;
    } else {
      if (currentMode != MODE_CHECKOUT && !autoAbsentSent) {
        currentMode = MODE_CHECKOUT;
        autoAbsentSent = true;
        sendAutoAbsent();
      } else {
        currentMode = MODE_CHECKOUT;
      }
    }
  }
}

// Hàm xử lý logic cốt lõi: Điểm danh (Check-in) hoặc Tan học (Check-out)
void processAttendance(int fingerID, String cardUID) {
  if (fingerID == ADMIN_ID)
    return;

  unsigned long now = millis();
  if (fingerID > 0 && fingerID == lastFingerID && now - lastScanTime < 3000)
    return;
  if (cardUID != "" && cardUID == lastCardUID && now - lastScanTime < 3000)
    return;
  lastFingerID = fingerID;
  lastCardUID = cardUID;
  lastScanTime = now;

  if (currentMode == MODE_ATTENDANCE) {
    if (hasCheckedIn[fingerID]) {
      if (hasCheckedOut[fingerID]) {
        // Đã checkout sớm rồi -> báo đã ra về
        lcdShow("Already CheckOut", "ID: " + String(fingerID));
        beep(2, 100);
        delay(3000);
        return;
      }
      // Học sinh đã check-in rồi -> hỏi xác nhận về sớm
      earlyLeaveID = fingerID;
      earlyLeaveRFID = (cardUID == "") ? "FINGER" : cardUID;
      currentMode = MODE_EARLY_LEAVE;
      manualModeStartTime = millis();
      lcdShow("Early-CheckOut?", "A:Yes C:No");
      beep(1, 80);
      delay(2000);
      return;
    }
    String status;
    if (currentTotalMins <= startTotalMins + lateAfterMins)
      status = "On-Time";
    else
      status = "Late";

    hasCheckedIn[fingerID] = true;

    if (status == "Late") {
      lcdShow("Late!!!", "ID: " + String(fingerID));
    } else {
      lcdShow("Welcome!", "ID: " + String(fingerID));
    }

    String rfid = (cardUID == "") ? "FINGER" : cardUID;
    sendToSheet("attend", fingerID, rfid, status);

  } else if (currentMode == MODE_CHECKOUT) {
    if (hasCheckedOut[fingerID]) {
      lcdShow("Already Out", "ID: " + String(fingerID));
      delay(3000);
      return;
    }
    bool didCheckIn = hasCheckedIn[fingerID];
    hasCheckedOut[fingerID] = true;

    String rfid = (cardUID == "") ? "FINGER" : cardUID;
    if (!didCheckIn) {
      lcdShow("ID: " + String(fingerID), "Marked Absent");
      sendToSheet("attend", fingerID, rfid, "Absent");
    } else {
      lcdShow("Goodbye", "ID: " + String(fingerID));
      sendToSheet("attend", fingerID, rfid, "Check-Out");
    }
  }
  delay(3000);
  rfid.PCD_Init();
}

// Hàm kiểm tra và điểm danh riêng cho người dùng quét bằng thẻ RFID
void checkAttendanceByRFID(String cardUID) {
  prefs.begin("bind", true);
  int expectedFingerID = prefs.getInt(cardUID.c_str(), -1);
  prefs.end();
  if (expectedFingerID == -1) {
    lcdShow("Unknown Card", "Not Found");
    beep(1, 30);
    delay(3000);
    return;
  }
  processAttendance(expectedFingerID, cardUID);
}

// ==========================================
// 6. HÀM CỐT LÕI (CORE FUNCTIONS)
// ==========================================

// --- HÀM KHỞI TẠO HỆ THỐNG (CHẠY 1 LẦN KHI BẬT NGUỒN) ---
void setup() {
  Serial.begin(115200);
  unsigned long t0 = millis();
  while (!Serial && millis() - t0 < 5000)
    ;
  delay(500);

  if (endTotalMins < startTotalMins)
    endTotalMins += 1440;
  pinMode(BUZZER, OUTPUT);
  digitalWrite(BUZZER, LOW);

  Wire.begin();
  lcd.init();
  lcd.backlight();
  lcdShow("Starting...", "Please Wait");
  delay(1000);

  Serial2.setRxBufferSize(1024);
  Serial2.begin(FP_BAUD, SERIAL_8N1, FP_RX, FP_TX);
  delay(500);
  if (!finger.verifyPassword()) {
    lcdShow("Fingerprint", "Error");
    Serial.println("[FP] Sensor not found!");
    while (1)
      delay(1);
  }

  SPI.begin();
  rfid.PCD_Init();
  delay(50);
  if (rfid.PCD_ReadRegister(rfid.VersionReg) == 0x00 ||
      rfid.PCD_ReadRegister(rfid.VersionReg) == 0xFF) {
    lcdShow("RFID", "Error");
    while (1)
      delay(1);
  }

  lcdShow("Connecting...", "WiFi");
  WiFi.begin(ssid, password);
  int retryWiFi = 0;
  while (WiFi.status() != WL_CONNECTED && retryWiFi < 40) {
    delay(500);
    retryWiFi++;
  }

  lcdShow("Syncing Time", "Please Wait");
  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, "pool.ntp.org",
             "time.google.com", "time.nist.gov");
  struct tm timeinfo;
  int retryNTP = 0;
  while (!getLocalTime(&timeinfo) && retryNTP < 20) {
    delay(500);
    retryNTP++;
  }
  if (retryNTP >= 20) {
    lcdShow("NTP Failed!", "Restarting...");
    delay(3000);
    ESP.restart();
  }

  Serial2.end();
  delay(100);
  Serial2.begin(FP_BAUD, SERIAL_8N1, FP_RX, FP_TX);
  delay(500);
  if (!finger.verifyPassword()) {
    Serial.println("[FP] WARNING: Sensor may be unstable!");
  }
  httpQueue = xQueueCreate(10, sizeof(HttpJob));
  xTaskCreatePinnedToCore(httpSendTask, "HTTP", 8192, NULL, 1, &httpTaskHandle,
                          0);
  Serial.println("[FreeRTOS] HTTP Task started on Core 0");
}

// --- VÒNG LẶP CHÍNH CỦA HỆ THỐNG (CHẠY LIÊN TỤC NHƯ TRÁI TIM) ---
void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    wifiWasConnected = false;
    if (millis() - lastWiFiRetry > 10000) {
      WiFi.reconnect();
      lastWiFiRetry = millis();
    }
  } else {
    if (!wifiWasConnected) {
      wifiWasConnected = true;
      flushQueue();
    }
  }
  updateTimeAndMode();

  if (currentMode == MODE_ADMIN_MENU || currentMode == MODE_REGISTRATION ||
      currentMode == MODE_DELETE || currentMode == MODE_DELETE_CONFIRM ||
      currentMode == MODE_EARLY_LEAVE) {
    if (millis() - manualModeStartTime > ADMIN_TIMEOUT) {
      currentMode = MODE_IDLE;
      lcdShow("Admin Timeout", "Auto Exit");
      beep(2, 80);
      delay(1500);
    }
  }

  char key = keypad.getKey();
  if (key) {

    if (currentMode == MODE_ADMIN_MENU) {
      if (key == 'A') {
        currentMode = MODE_REGISTRATION;
        manualModeStartTime = millis();
        lcdShow("A:FP B:Card+FP", "C:Back");
      } else if (key == 'D') {
        bool hasUsers = false;
        prefs.begin("vtay", true);
        for (int i = ADMIN_ID + 1; i < MAX_USERS; i++) {
          if (prefs.getString(String(i).c_str(), "") != "") {
            hasUsers = true;
            break;
          }
        }
        prefs.end();

        if (!hasUsers) {
          lcdShow("Empty System", "No Users Found");
          beep(2, 100);
          delay(1500);
          displayStatus(true);
        } else {
          currentMode = MODE_DELETE;
          inputDeleteID = "";
          manualModeStartTime = millis();
          displayStatus(true);
        }
      } else if (key == 'C') {
        currentMode = MODE_IDLE;
        displayStatus();
      }
      return;
    }

    if (currentMode == MODE_REGISTRATION) {
      if (key == 'A') {
        registerNewUser("");
        manualModeStartTime = millis();
        lcdShow("A:FP B:Card+FP", "C:Back");
      } else if (key == 'B') {
        lcdShow("Scan Card Now", "To bind FP...");
        unsigned long waitStart = millis();
        String newCard = "";
        while (millis() - waitStart < 10000) {
          newCard = getRFID();
          if (newCard != "")
            break;
          delay(50);
        }
        if (newCard != "") {
          registerNewUser(newCard);
        } else {
          lcdShow("Timeout!", "No Card");
          beep(1, 30);
          delay(1000);
        }
        manualModeStartTime = millis();
        lcdShow("A:FP B:Card+FP", "C:Back");
      } else if (key == 'C') {
        currentMode = MODE_ADMIN_MENU;
        manualModeStartTime = millis();
        lcdShow("A:Add D:Del", "C:Exit");
      }
      return;
    }

    if (currentMode == MODE_DELETE) {
      if (key == 'B') {
        currentMode = MODE_ADMIN_MENU;
        manualModeStartTime = millis();
        lcdShow("A:Add D:Del", "C:Exit");
      }
      return;
    }

    if (currentMode == MODE_DELETE_CONFIRM) {
      if (key == 'A' || key == 'B' || key == 'D' || key == '#') {
        lcdShow("Deleting All...", "ID: " + String(targetDeleteID));
        uint8_t p = finger.deleteModel(targetDeleteID);
        if (p != FINGERPRINT_OK) {
          lcdShow("Delete Error", "Try Again");
          beep(2, 100);
          delay(1500);
          currentMode = MODE_DELETE;
          inputDeleteID = "";
          manualModeStartTime = millis();
          displayStatus(true);
          return;
        }

        prefs.begin("rev", true);
        String cardUID =
            prefs.getString(String(targetDeleteID).c_str(), "NORFID");
        prefs.end();

        if (cardUID != "NORFID" && cardUID != "") {
          prefs.begin("bind", false);
          prefs.remove(cardUID.c_str());
          prefs.end();
        }

        prefs.begin("rev", false);
        prefs.remove(String(targetDeleteID).c_str());
        prefs.end();

        prefs.begin("vtay", false);
        prefs.remove(String(targetDeleteID).c_str());
        prefs.end();

        sendToSheet("delete", targetDeleteID, cardUID, "Deleted");
        lcdShow("Delete OK", "ID: " + String(targetDeleteID));
        beep(1, 30);
        delay(1000);

        currentMode = MODE_DELETE;
        inputDeleteID = "";
        manualModeStartTime = millis();
        displayStatus(true);
      } else if (key == 'C' || key == '*') {
        currentMode = MODE_DELETE;
        inputDeleteID = "";
        manualModeStartTime = millis();
        displayStatus(true);
      }
      return;
    }

    // Xử lý xác nhận về sớm
    if (currentMode == MODE_EARLY_LEAVE) {
      if (key == 'A') {
        // Xác nhận về sớm
        lcdShow("Early-CheckOut", "ID: " + String(earlyLeaveID));
        beep(1, 150);
        delay(2000);
        lcdShow("Goodbye!", "ID: " + String(earlyLeaveID));
        delay(1500);
        hasCheckedOut[earlyLeaveID] = true;
        sendToSheet("attend", earlyLeaveID, earlyLeaveRFID, "Early-CheckOut");
        currentMode = MODE_ATTENDANCE;
        earlyLeaveID = -1;
        earlyLeaveRFID = "";
      } else if (key == 'C') {
        // Hủy về sớm
        lcdShow("Cancelled", "Stay in class");
        beep(1, 30);
        delay(2000);
        currentMode = MODE_ATTENDANCE;
        earlyLeaveID = -1;
        earlyLeaveRFID = "";
      }
      return;
    }
  }
  // --- 1. XỬ LÝ KHI QUẸT THẺ RFID ---
  String cardUID = getRFID();
  if (cardUID != "") { // Nếu quét được thẻ
    if (currentMode == MODE_EARLY_LEAVE) {
      return; // Đang chờ xác nhận về sớm -> bỏ qua thẻ
    }
    if (currentMode == MODE_ADMIN_MENU) {
      lcdShow("A:Add D:Del", "C:Exit"); // Đang ở menu admin thì nhắc bấm phím
      delay(300);
      return;
    }
    if (currentMode == MODE_REGISTRATION) {
      lcdShow("Press B",
              "to bind card"); // Nhắc người dùng bấm B để gắn thẻ với vân tay
      delay(300);
      return;
    }
    if (currentMode == MODE_DELETE) {
      deleteUser(cardUID); // Đang ở chế độ xóa -> Xóa user này
      return;
    }
    checkAttendanceByRFID(
        cardUID); // Trạng thái bình thường -> Thực hiện điểm danh
    return;
  }

  // --- 2. XỬ LÝ KHI QUÉT VÂN TAY ---
  int fingerID = scanFingerNoWait();
  if (fingerID > 0) { // Nếu vân tay hợp lệ (có trong bộ nhớ)

    if (fingerID == ADMIN_ID) {
      // Quét trúng vân tay Admin -> Vào thẳng Menu Quản trị
      currentMode = MODE_ADMIN_MENU;
      manualModeStartTime = millis();
      lcdShow("A:Add D:Del", "C:Exit");
      beep(1, 150);
      return;
    }

    if (currentMode == MODE_ADMIN_MENU || currentMode == MODE_REGISTRATION ||
        currentMode == MODE_EARLY_LEAVE) {
      // Đang bận ở menu quản trị hoặc đăng ký thì bỏ qua vân tay thường
      return;
    }
    if (currentMode == MODE_DELETE) {
      deleteUserByFingerID(
          fingerID); // Đang ở chế độ xóa -> Xóa user theo vân tay này
      return;
    }
    if (currentMode == MODE_ATTENDANCE || currentMode == MODE_CHECKOUT) {
      processAttendance(fingerID, "");
    }
    return;

  } else if (fingerID == -1) {
    delay(800);
    return;
  } else if (fingerID == -2) {
    return;
  }
  displayStatus();
  delay(10);
}
