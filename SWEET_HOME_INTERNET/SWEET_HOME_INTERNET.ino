#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <ESP8266WebServer.h>
#include <WiFiManager.h> 
#include <vector>
#include <time.h> // เพิ่ม Time เพื่อช่วยเรื่อง SSL

// ================= CONFIG =================

// AP Config (ประตูหลังบ้าน - เข้าได้เสมอผ่าน 192.168.4.1)
const char* ap_ssid = "Garage_Master";
const char* ap_pass = "12345678";

// Server Config (Render HTTPS)
const char* serverBaseUrl = "https://smart-home-api-4qwz.onrender.com"; 
const char* deviceID = "esp8266-01"; 
const char* apiUser = "admin";
const char* apiPass = "22556677";

// --- 🛠️ PIN MAPPING 🛠️ ---
// Index 0: โรงรถ (D6 -> GPIO 12)
// Index 1: หน้าบ้าน (D5 -> GPIO 14)
const int controlPins[] = {12, 14}; 
const int numPins = 2;

// ใช้ Builtin LED เป็น Panic Alarm
const int ALARM_PIN = LED_BUILTIN; 

// ================= GLOBAL VARS =================
ESP8266WebServer server(80);
WiFiManager wm; 

String jwtToken = "";
unsigned long lastCheckTime = 0;
const long checkInterval = 2000; 

// Status & Logs
String connectionStatus = "Booting...";
int currentErrorCode = 0; 
std::vector<String> sysLogs;
unsigned long prevAlarmMillis = 0;
int blinkCounter = 0;
bool isLedOn = false;
bool inPauseMode = false;

// ================= HELPER FUNCTIONS =================

void logEvent(String msg) {
  String timeStr = "[" + String(millis() / 1000) + "s] ";
  sysLogs.insert(sysLogs.begin(), timeStr + msg);
  if (sysLogs.size() > 20) sysLogs.pop_back();
  Serial.println(msg);
}

void setError(int code, String msg) {
  if (currentErrorCode != code) {
    currentErrorCode = code;
    logEvent("ERR " + String(code) + ": " + msg);
    inPauseMode = false; blinkCounter = 0; isLedOn = false;
    digitalWrite(ALARM_PIN, HIGH); 
  }
}

void handleAlarm() {
  if (currentErrorCode == 0) {
    digitalWrite(ALARM_PIN, HIGH); 
    return;
  }
  unsigned long currentMillis = millis();
  if (inPauseMode) {
    if (currentMillis - prevAlarmMillis >= 5000) { 
      inPauseMode = false; prevAlarmMillis = currentMillis; blinkCounter = 0;
    }
    digitalWrite(ALARM_PIN, HIGH);
    return;
  }
  if (currentMillis - prevAlarmMillis >= 300) { 
    prevAlarmMillis = currentMillis;
    if (isLedOn) {
      isLedOn = false; digitalWrite(ALARM_PIN, HIGH);
      blinkCounter++;
      if (blinkCounter >= currentErrorCode) inPauseMode = true;
    } else {
      isLedOn = true; digitalWrite(ALARM_PIN, LOW);
    }
  }
}

// ================= CORE FUNCTIONS (HTTPS BearSSL) =================

void loginToServer() {
  if (WiFi.status() != WL_CONNECTED) { setError(1, "WiFi Lost"); return; }

  // ใช้ BearSSL สำหรับ HTTPS (Render)
  std::unique_ptr<BearSSL::WiFiClientSecure> client(new BearSSL::WiFiClientSecure);
  client->setInsecure(); // Skip Cert Check
  client->setBufferSizes(4096, 1024); // ขยาย Buffer กันหลุด
  client->setTimeout(10000);

  HTTPClient http;
  String url = String(serverBaseUrl) + "/api/login";

  if (!http.begin(*client, url)) {
    setError(3, "Conn Fail (Login)"); return;
  }

  http.addHeader("Content-Type", "application/json");
  JsonDocument doc;
  doc["username"] = apiUser;
  doc["password"] = apiPass;
  String json;
  serializeJson(doc, json);

  int code = http.POST(json);
  if (code == 200) {
    String payload = http.getString();
    JsonDocument res;
    deserializeJson(res, payload);
    jwtToken = String(res["token"].as<const char*>());
    
    if(currentErrorCode != 0) currentErrorCode = 0; 
    connectionStatus = "Login OK";
    logEvent("Login Success.");
  } else {
    String payload = http.getString();
    logEvent("Login Fail: " + String(code) + " " + payload);
    if(code==401) setError(2, "Auth Fail");
    else setError(3, "Login HTTP " + String(code));
  }
  http.end();
}

void syncState() {
  if (WiFi.status() != WL_CONNECTED) { setError(1, "WiFi Lost"); return; }
  if (jwtToken == "") { loginToServer(); return; }

  std::unique_ptr<BearSSL::WiFiClientSecure> client(new BearSSL::WiFiClientSecure);
  client->setInsecure();
  client->setBufferSizes(4096, 1024);
  client->setTimeout(5000);

  HTTPClient http;
  String url = String(serverBaseUrl) + "/api/device/" + String(deviceID) + "/getState";

  if (!http.begin(*client, url)) {
    setError(3, "Conn Fail (Sync)"); return;
  }

  http.addHeader("Authorization", "Bearer " + jwtToken);
  int code = http.GET();

  if (code == 200) {
    if (http.getSize() >= 2) {
      WiFiClient *stream = http.getStreamPtr();
      unsigned long timeout = millis();
      while(stream->available() < 2 && millis() - timeout < 1000) { delay(10); }

      if(stream->available() >= 2) {
        uint8_t h = stream->read();
        uint8_t l = stream->read();
        uint16_t mask = (h << 8) | l;

        if(currentErrorCode != 0) { currentErrorCode = 0; logEvent("Online (Cloud)"); }
        
        static uint16_t lastMask = 0xFFFF;
        if(mask != lastMask) {
          logEvent("State: " + String(mask));
          lastMask = mask;
          updateHardware(mask);
        }
        connectionStatus = "Synced";
      }
    }
  } else if (code == 401 || code == 403) {
    setError(2, "Token Expired"); jwtToken = "";
  } else {
    setError(3, "Sync HTTP " + String(code));
  }
  http.end();
}

void updateHardware(uint16_t mask) {
  for (int i = 0; i < numPins; i++) {
    bool isOn = (mask >> i) & 1;
    // Active LOW (LOW = ติด)
    digitalWrite(controlPins[i], isOn ? LOW : HIGH); 
  }
}

// ================= WEB & OTA =================

const char* updateIndex = "<form method='POST' action='/update' enctype='multipart/form-data'><h2>OTA Update</h2><input type='file' name='update'><input type='submit' value='Update'></form>";

void setupWebServer() {
  server.on("/", HTTP_GET, []() {
    String h = "<html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'><meta http-equiv='refresh' content='5'></head>";
    h += "<body style='font-family:sans-serif;text-align:center;'>";
    h += "<h2>Garage Master (DHCP)</h2>";
    h += "<p>Status: <b>" + (currentErrorCode==0 ? String("ONLINE") : ("ERR " + String(currentErrorCode))) + "</b></p>";
    h += "<p>LAN IP: " + WiFi.localIP().toString() + "</p>";
    h += "<p>AP IP: " + WiFi.softAPIP().toString() + " (" + String(ap_ssid) + ")</p>";
    h += "<hr><h3>Logs</h3><div style='background:#eee;text-align:left;padding:10px;font-family:monospace;'>";
    for(String &l : sysLogs) h += l + "<br>";
    h += "</div><br>";
    h += "<a href='/update'>OTA Update</a> | ";
    h += "<a href='/resetwifi' onclick=\"return confirm('Reset WiFi Settings?')\">Reset WiFi</a>";
    h += "</body></html>";
    server.send(200, "text/html", h);
  });

  server.on("/reboot",HTTP_GET,[](){
    server.send(200, "text/plain", "Rebooting...");
    delay(1000);
    ESP.restart();
  });

  server.on("/resetwifi",HTTP_GET,[](){
    wm.resetSettings();
    server.send(200, "text/plain", "WiFi Settings Cleared! Rebooting...");
    delay(1000);
    ESP.restart();
  });

  server.on("/update", HTTP_GET, [](){ server.send(200, "text/html", updateIndex); });
  
  server.on("/update", HTTP_POST, [](){
    server.send(200, "text/plain", (Update.hasError())?"FAIL":"OK! Rebooting...");
    ESP.restart();
  }, [](){
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
      uint32_t maxSketchSpace = (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
      Update.begin(maxSketchSpace);
    } else if (upload.status == UPLOAD_FILE_WRITE) {
      Update.write(upload.buf, upload.currentSize);
    } else if (upload.status == UPLOAD_FILE_END) {
      Update.end(true);
    }
    yield();
  });
  
  server.begin();
}

// ================= MAIN =================

void setup() {
  Serial.begin(115200);
  pinMode(ALARM_PIN, OUTPUT); digitalWrite(ALARM_PIN, HIGH);

  for (int i = 0; i < numPins; i++) {
    pinMode(controlPins[i], OUTPUT);
    digitalWrite(controlPins[i], HIGH);
  }

  // ตั้งเวลา NTP สำหรับ HTTPS
  configTime(7 * 3600, 0, "pool.ntp.org", "time.nist.gov");

  // --- WiFiManager Setup (DHCP) ---
  wm.setConfigPortalTimeout(180); 
  
  // ไม่มี wm.setSTAStaticIPConfig(...) แล้ว --> ใช้ DHCP จาก Router
  
  if(!wm.autoConnect("Garage_Config", "12345678")) {
    Serial.println("Failed to connect or timeout");
  } else {
    Serial.println("Connected to WiFi");
  }

  // เปิด AP ไว้เสมอเผื่อหา IP ไม่เจอ
  WiFi.mode(WIFI_AP_STA); 
  WiFi.softAP(ap_ssid, ap_pass);

  Serial.print("LAN IP: "); Serial.println(WiFi.localIP());
  Serial.print("AP IP: "); Serial.println(WiFi.softAPIP());

  setupWebServer();
  logEvent("System Boot successful.");
  loginToServer();
}

void loop() {
  server.handleClient();
  handleAlarm();

  if (millis() - lastCheckTime >= checkInterval) {
    lastCheckTime = millis();
    syncState();
  }
}
