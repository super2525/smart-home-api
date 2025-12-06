#include <WiFi.h>
#include <WiFiManager.h>       
#include <ESPAsyncWebServer.h> 
#include <AsyncTCP.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>  
#include <ArduinoJson.h> 
#include <Update.h>
#include <vector> 

// ป้องกัน Sleep หรือหลุดบ่อย แต่ต้องต่อ C1000uF ก่อนเพราะกินไฟเยอะ
//#include <esp_wifi.h>
//WiFi.setTxPower(WIFI_POWER_19_5dBm);
//esp_wifi_set_ps(WIFI_PS_NONE);

const char* serverBaseUrl = "https://smart-home-api-4qwz.onrender.com"; 
const char* deviceID = "esp32-01"; 
const char* apiUser = "admin";
const char* apiPass = "22556677";

const char* apSSID = "Smart_Home_111";
const char* apPass = "0826165992";

const int controlPins[] = { 5, 26, 17, 18, 19, 22, 4, 23, 14, 27, 33 };
const int numPins = sizeof(controlPins) / sizeof(controlPins[0]);
const int ALARM_PIN_INDEX = 9; 

AsyncWebServer server(80);
WiFiManager wm;

String jwtToken = "";
unsigned long lastCheckTime = 0;
const long checkInterval = 2000; 

String connectionStatus = "Booting...";
int lastHttpCode = 0;
unsigned long lastSyncSuccessTime = 0;

std::vector<String> sysLogs;

int currentErrorCode = 0; 
unsigned long prevAlarmMillis = 0;
int blinkCounter = 0;     
bool isLedOn = false;     
bool inPauseMode = false; 
String led ="Off";
int builtinled = 13;

void logEvent(String msg) {
  String timeStr = "[" + String(millis() / 1000) + "s] ";
  String fullMsg = timeStr + msg;
  Serial.println(fullMsg);
  sysLogs.insert(sysLogs.begin(), fullMsg);
  if (sysLogs.size() > 20) sysLogs.pop_back();
}

void handleSmartAlarm() {
  if (currentErrorCode == 0) return; 

  unsigned long currentMillis = millis();
  int pin = controlPins[ALARM_PIN_INDEX];

  if (inPauseMode) {
    if (currentMillis - prevAlarmMillis >= 10000) { 
      inPauseMode = false;
      prevAlarmMillis = currentMillis;
      blinkCounter = 0; 
    }
    digitalWrite(pin, HIGH);
    return;
  }

  if (currentMillis - prevAlarmMillis >= 500) {
    prevAlarmMillis = currentMillis;
    
    if (isLedOn) {
      isLedOn = false;
      digitalWrite(pin, HIGH); 
      blinkCounter++; 

      if (blinkCounter >= currentErrorCode) {
        inPauseMode = true; 
      }
    } else {
      isLedOn = true;
      digitalWrite(pin, LOW); 
    }
  }
}

void setError(int code, String msg) {
  if (currentErrorCode != code) {
    currentErrorCode = code;
    logEvent("Error " + String(code) + ": " + msg);
    
    inPauseMode = false;
    blinkCounter = 0;
    isLedOn = false;
    prevAlarmMillis = millis(); 
    digitalWrite(controlPins[ALARM_PIN_INDEX], HIGH); 
  }
}

void loginToServer() {
  if (WiFi.status() != WL_CONNECTED) {
    setError(1, "LAN Disconnected");
    return;
  }

  WiFiClientSecure client;
  client.setInsecure(); 

  HTTPClient http;
  String url = String(serverBaseUrl) + "/api/login";
  
  if (!http.begin(client, url)) {
     setError(3, "Cannot Reach Render (Login Step)");
     return;
  }
  
  http.addHeader("Content-Type", "application/json");

  JsonDocument doc; 
  doc["username"] = apiUser;
  doc["password"] = apiPass;
  String requestBody;
  serializeJson(doc, requestBody);
  
  int code = http.POST(requestBody);
  lastHttpCode = code;

  if (code == 200) {
    String response = http.getString();
    JsonDocument resDoc;
    deserializeJson(resDoc, response);
    jwtToken = String(resDoc["token"].as<const char*>());
    
    logEvent("Login OK");
    connectionStatus = "Login Success";
  } else {
    if (code == 401 || code == 403) {
      setError(2, "Login Failed (Auth)");
    } 
    else if (code >= 500) {
      setError(4, "Server Internal Error (Login)");
    } 
    else {
      setError(3, "Login Error HTTP " + String(code));
    }
  }
  http.end();
}

void updateHardware(uint16_t mask) {
  for (int i = 0; i < numPins; i++) {
    bool isOn = (mask >> i) & 1;
    digitalWrite(controlPins[i], isOn ? LOW : HIGH); 
  }
}

void syncStateFromRemote() {
  if (WiFi.status() != WL_CONNECTED) {
    setError(1, "LAN Disconnected");
    return; 
  }
  
  if (jwtToken == "") { 
    loginToServer(); 
    return; 
  }

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  String url = String(serverBaseUrl) + "/api/device/" + String(deviceID) + "/getState";

  if (!http.begin(client, url)) {
    setError(3, "Cannot Reach Render (Sync Step)");
    return;
  }

  http.addHeader("Authorization", "Bearer " + jwtToken);
  
  int code = http.GET();
  lastHttpCode = code;

  if (code == 200) {
    
    if (http.getSize() >= 2) {
      WiFiClient *stream = http.getStreamPtr();
      uint8_t h = 0, l = 0;
      if (stream->available()) h = stream->read();
      if (stream->available()) l = stream->read();
      
      uint16_t bitmask = (h << 8) | l;
      
      if (currentErrorCode != 0) {
        currentErrorCode = 0;
        logEvent("System Recovered");
        inPauseMode = false;
        isLedOn = false;
      }

      static uint16_t lastMask = 0xFFFF;
      if (bitmask != lastMask) {
        logEvent("State Changed: " + String(bitmask));
        lastMask = bitmask;
        if (led="On") {
          digitalWrite(builtinled,HIGH);
          led = "Off";
        } else {
          digitalWrite(builtinled,LOW);
          led = "On";          
        }
      }

      updateHardware(bitmask);
      connectionStatus = "Online & Synced";
      lastSyncSuccessTime = millis();
    }
  } 
  else if (code == 401 || code == 403) {
    setError(2, "Token Expired (401)");
    loginToServer();
  } 
  else if (code >= 500) {
    setError(4, "Server/DB Error (500)");
  } 
  else {
    setError(3, "Sync Error HTTP " + String(code));
  }
  http.end();
}

void setupWebUpdate() {
  server.on("/update", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(200, "text/html", 
      "<form method='POST' action='/update' enctype='multipart/form-data'>"
      "<h2>ESP32 OTA</h2><input type='file' name='update'><br><br><input type='submit' value='Update Firmware'></form>");
  });

  server.on("/update", HTTP_POST, [](AsyncWebServerRequest *request){
    bool restartRequired = !Update.hasError();
    request->send(200, "text/plain", restartRequired ? "OK. Rebooting..." : "Failed");
    if (restartRequired) ESP.restart();
  }, [](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final){
    if(!index) Update.begin(UPDATE_SIZE_UNKNOWN);
    Update.write(data,len);
    if(final) Update.end(true);
  });
}

void setup() {
  Serial.begin(115200);

  for (int i = 0; i < numPins; i++) {
    pinMode(controlPins[i], OUTPUT);
    digitalWrite(controlPins[i], HIGH);
  }

  logEvent("System Booting...");

  wm.setConfigPortalTimeout(300); 
  bool res = wm.autoConnect(apSSID, apPass); 
  
  if(!res) { 
    logEvent("WiFi Connect Failed"); 
  } else { 
    logEvent("WiFi Connected IP: " + WiFi.localIP().toString()); 
  }

  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(apSSID, apPass); 

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    String html = "<html><head>";
    html += "<meta charset='UTF-8'>";
    html += "<meta http-equiv='refresh' content='3'>";
    html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
    html += "<style>body{font-family:sans-serif;padding:20px;text-align:center;} .ok{color:green;} .err{color:red;font-weight:bold;}</style>";
    html += "</head><body>";
    
    html += "<h2>ESP32 Diagnosis</h2>";
    
    html += "<p>Status: ";
    if (currentErrorCode == 0) html += "<span class='ok'>Online</span>";
    else html += "<span class='err'>ERROR CODE: " + String(currentErrorCode) + "</span>";
    html += "</p>";

    html += "<div style='text-align:left; border:1px solid #ccc; padding:10px; margin:10px auto; max-width:400px;'>";
    html += "<strong>Blink Codes (Pin 27):</strong><br>";
    html += "1 Blink: LAN Disconnected<br>";
    html += "2 Blinks: Login Failed<br>";
    html += "3 Blinks: Render Unreachable<br>";
    html += "4 Blinks: Server/DB Error";
    html += "</div>";

    html += "<p>LAN IP: " + WiFi.localIP().toString() + "</p>";
    
    html += "<hr><h3>Logs</h3>";
    html += "<div style='background:#eee; text-align:left; padding:10px; font-family:monospace;'>";
    for (String &l : sysLogs) {
      html += l + "<br>";
    }
    html += "</div>";
    
    html += "<p><a href='/update'>OTA</a> | <a href='/reset'>Reset WiFi</a></p>";
    html += "</body></html>";
    
    request->send(200, "text/html", html);
  });
  
  server.on("/reset", HTTP_GET, [](AsyncWebServerRequest *request){
     wm.resetSettings();
     request->send(200, "text/plain", "WiFi Cleared. Restarting...");
     delay(1000);
     ESP.restart();
  });

  setupWebUpdate();
  server.begin();

  loginToServer();
}

void loop() {
  handleSmartAlarm();
  delay(1);

  if (WiFi.status() == WL_CONNECTED) {
    if (millis() - lastCheckTime >= checkInterval) {
      lastCheckTime = millis();
      syncStateFromRemote();
      delay(1);
    }
  } else {
    if (currentErrorCode != 1) setError(1, "WiFi Disconnected");
    connectionStatus = "WiFi Disconnected";
  }
}
