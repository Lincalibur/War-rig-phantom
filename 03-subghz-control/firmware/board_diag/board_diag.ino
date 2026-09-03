// Standalone hardware diagnostic for the ESP32-WROOM-32 sub-ghz board.
//
// Not part of the CyberDeck ESP-NOW mesh — this is a throwaway test sketch
// to answer one question: is this specific board/power-supply combo
// actually broken, independent of subghz_node.ino's own code (RCSwitch,
// interrupts, ESP-NOW). It does progressively more radio-heavy things
// (WiFi scan -> WiFi AP + web server -> BLE scan) and logs the result of
// each step both to Serial and to a live web page served from the board's
// own AP, so results are visible whether you have a laptop on serial or
// just a phone in the field.
//
// If the board brownout-loops before even the WiFi scan finishes, that's
// still useful data (points hard at power, not code) — the reset reason
// is logged as literally the first thing in setup(), before any radio init.
//
// Usage: flash this, then either watch Serial at 115200, or once it's up,
// connect a phone/laptop to WiFi "CyberDeck-Diag" (password "diagnose1")
// and browse to http://192.168.4.1/ — the page auto-refreshes.
//
// FQBN: esp32:esp32:esp32

#include <WiFi.h>
#include <WebServer.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <esp_system.h>

static const uint8_t RCSWITCH_PIN = 4;
static const uint8_t RAW_PIN      = 5;

static const char* AP_SSID = "CyberDeck-Diag";
static const char* AP_PASS = "diagnose1";

WebServer server(80);

// ---------------------------------------------------------------------
// Rolling log buffer — shown on the web page, mirrored to Serial.
// ---------------------------------------------------------------------
static const uint8_t LOG_LINES   = 60;
static const uint8_t LOG_LINELEN = 110;
static char logBuf[LOG_LINES][LOG_LINELEN];
static uint8_t logHead = 0;   // next slot to write
static uint8_t logCount = 0;  // how many valid lines (caps at LOG_LINES)

static void addLog(const char* fmt, ...) {
  char line[LOG_LINELEN];
  va_list args;
  va_start(args, fmt);
  vsnprintf(line, sizeof(line), fmt, args);
  va_end(args);

  char stamped[LOG_LINELEN];
  snprintf(stamped, sizeof(stamped), "[%8lu] %s", (unsigned long)millis(), line);

  strncpy(logBuf[logHead], stamped, LOG_LINELEN - 1);
  logBuf[logHead][LOG_LINELEN - 1] = '\0';
  logHead = (logHead + 1) % LOG_LINES;
  if (logCount < LOG_LINES) logCount++;

  Serial.println(stamped);
}

static const char* resetReasonName(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:   return "POWERON (cold boot)";
    case ESP_RST_SW:        return "SW (software reset)";
    case ESP_RST_PANIC:     return "PANIC (crash)";
    case ESP_RST_INT_WDT:   return "INT_WDT (interrupt watchdog)";
    case ESP_RST_TASK_WDT:  return "TASK_WDT (task watchdog)";
    case ESP_RST_WDT:       return "WDT (other watchdog)";
    case ESP_RST_BROWNOUT:  return "BROWNOUT — power supply can't hold voltage under load";
    case ESP_RST_EXT:       return "EXT (external reset pin)";
    default:                return "other/unknown";
  }
}

// ---------------------------------------------------------------------
// Self-tests — cheapest/least radio-heavy first, so a partial run still
// tells us where the board actually stops working.
// ---------------------------------------------------------------------

static void testWifiScan() {
  addLog("--- TEST: WiFi scan ---");
  WiFi.mode(WIFI_STA);
  int n = WiFi.scanNetworks();
  if (n < 0) {
    addLog("WiFi scan FAILED (returned %d)", n);
    return;
  }
  addLog("WiFi scan OK: %d networks found", n);
  for (int i = 0; i < n && i < 5; i++) {
    addLog("  \"%s\" rssi=%d ch=%d", WiFi.SSID(i).c_str(), WiFi.RSSI(i), WiFi.channel(i));
  }
}

static void startApAndServer() {
  addLog("--- TEST: starting AP + web server ---");
  WiFi.mode(WIFI_AP_STA);  // keep STA for further scans, add AP for the phone
  bool ok = WiFi.softAP(AP_SSID, AP_PASS);
  addLog(ok ? "AP up: SSID=\"%s\" IP=%s" : "AP FAILED to start",
         AP_SSID, WiFi.softAPIP().toString().c_str());

  server.on("/", HTTP_GET, []() {
    String html = "<!doctype html><html><head><meta http-equiv='refresh' content='3'>"
                  "<title>CyberDeck board diag</title>"
                  "<style>body{background:#111;color:#0f0;font-family:monospace;"
                  "font-size:13px;white-space:pre-wrap;padding:8px}</style></head><body>";
    html += "CyberDeck sub-ghz board diagnostic — uptime ";
    html += String(millis() / 1000);
    html += "s, free heap ";
    html += String(ESP.getFreeHeap());
    html += " bytes\n\n";
    uint8_t start = (logCount < LOG_LINES) ? 0 : logHead;
    for (uint8_t i = 0; i < logCount; i++) {
      uint8_t idx = (start + i) % LOG_LINES;
      html += logBuf[idx];
      html += "\n";
    }
    html += "</body></html>";
    server.send(200, "text/html", html);
  });
  server.begin();
  addLog("Web server started — connect to WiFi \"%s\" (pass \"%s\"), browse http://%s/",
         AP_SSID, AP_PASS, WiFi.softAPIP().toString().c_str());
}

static void testBleScan() {
  addLog("--- TEST: BLE scan ---");
  BLEDevice::init("CyberDeck-Diag");
  BLEScan* pScan = BLEDevice::getScan();
  pScan->setActiveScan(true);
  BLEScanResults* results = pScan->start(3, false);
  int count = results ? results->getCount() : -1;
  addLog(count >= 0 ? "BLE scan OK: %d devices found" : "BLE scan FAILED", count);
  pScan->clearResults();
}

static void testSubGhzPins() {
  addLog("--- TEST: sub-ghz RX pins (no decode — needs RXB6 wired to actually test RF) ---");
  pinMode(RCSWITCH_PIN, INPUT);
  pinMode(RAW_PIN, INPUT);
  addLog("RCSWITCH_PIN(GPIO%u)=%d  RAW_PIN(GPIO%u)=%d  (stable reads only prove the pins"
         " aren't stuck — nothing is wired to drive them right now)",
         RCSWITCH_PIN, digitalRead(RCSWITCH_PIN), RAW_PIN, digitalRead(RAW_PIN));
}

// ---------------------------------------------------------------------

static uint32_t lastPeriodicMs = 0;
static const uint32_t PERIODIC_MS = 20000;

void setup() {
  Serial.begin(115200);
  delay(1000);

  // First thing, before any radio/peripheral init — if the last boot ended
  // in a brownout, this line survives even if everything after it doesn't.
  esp_reset_reason_t reason = esp_reset_reason();
  addLog("=== BOOT === last reset reason: %s", resetReasonName(reason));
  addLog("Free heap at boot: %u bytes", ESP.getFreeHeap());

  testWifiScan();
  startApAndServer();   // AP up early so partial results are visible even if BLE test below crashes it
  testBleScan();
  testSubGhzPins();

  addLog("=== all startup tests complete, board is alive and stable ===");
  lastPeriodicMs = millis();
}

void loop() {
  server.handleClient();

  if (millis() - lastPeriodicMs >= PERIODIC_MS) {
    lastPeriodicMs = millis();
    int n = WiFi.scanNetworks();
    addLog("periodic check: uptime=%lus heap=%u wifi_scan=%d networks",
           (unsigned long)(millis() / 1000), ESP.getFreeHeap(), n);
  }
}
