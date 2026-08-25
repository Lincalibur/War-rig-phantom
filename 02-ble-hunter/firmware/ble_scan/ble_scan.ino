// Component 2, steps 2.3-2.5 (ESP32-C3 #2)
//
// 2.3 — persistence scoring: each tracked device keeps a rolling history
// of the distinct windows it's appeared in. If a MAC shows up in
// PERSISTENCE_THRESHOLD+ distinct windows within the trailing
// PERSISTENCE_SPAN_WINDOWS span, it's flagged as a suspected tracker
// (repeatedly nearby rather than a one-off passerby or a stationary home
// device that's always in the same one or two windows forever). An alert
// line prints once, the moment a MAC crosses the threshold.
// STATUS: implemented, flashed, NOT YET CONFIRMED LIVE — see README.
//
// 2.4 — rotating-MAC awareness: AirTags/Find My devices rotate their BLE
// address, so MAC-based persistence alone will under-flag them (looks
// like a new device each time). Secondary heuristic: match Apple's Find
// My "offline finding" manufacturer-data payload shape directly —
// company ID 0x004C (little-endian bytes 0x4C,0x00) followed by type
// byte 0x12 — and flag on sight, independent of MAC persistence. This is
// a heuristic match on a payload shape documented by open-source AirTag
// detection projects, not an Apple spec doc — treat false positives as
// possible until field-confirmed against a real AirTag/Find My device.
// STATUS: new, INITIAL CUT, untested against a real AirTag.
//
// 2.5 — RSSI "getting warmer" mode: once ANY device is flagged (by 2.3 or
// 2.4), switch the 0.96" I2C SSD1306 OLED from the idle summary to a live
// RSSI bar for that device so you can walk it down. Display update is
// non-blocking (no delay() in the render path) so the scan loop keeps
// running underneath it.
// STATUS: new, INITIAL CUT, untested — confirm OLED wiring/address below.
//
// Field test (per component README): carry your own phone (known BLE
// device) around — it should get flagged after enough windows; a
// stationary home device should NOT.
//
// No RTC on this board, so windows are bucketed off millis() uptime
// rather than wall-clock time (fine — we only care about relative spacing).
//
// Libraries needed:
//   NimBLE-Arduino (h2zero)
//   Adafruit SSD1306 + Adafruit GFX (Arduino IDE > Manage Libraries)
//
// Board: ESP32C3 Dev Module (Tools > Board > esp32 > ESP32C3 Dev Module)
// IMPORTANT: this board needs "USB CDC On Boot" = Enabled or Serial
// output goes to the UART pins instead of the USB port. Via arduino-cli:
//   --fqbn 'esp32:esp32:esp32c3:CDCOnBoot=cdc'
// If you don't have the esp32 board package yet: File > Preferences >
// Additional Board Manager URLs, add:
//   https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
// then Tools > Board > Boards Manager > install "esp32" by Espressif.
//
// OLED wiring: VCC->3V3, GND->GND, SDA->OLED_SDA_PIN, SCL->OLED_SCL_PIN
// below (defaults are the ESP32-C3 Dev Module's default Wire pins —
// confirm against your board's silkscreen, some C3 boards differ).
// I2C address is 0x3C on the overwhelming majority of these 0.96" boards
// (0x3D on some) — change OLED_I2C_ADDR if the display stays blank.

#include <NimBLEDevice.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

static const uint8_t OLED_SDA_PIN = 8;
static const uint8_t OLED_SCL_PIN = 9;
static const uint8_t OLED_I2C_ADDR = 0x3C;
static const uint8_t OLED_WIDTH = 128;
static const uint8_t OLED_HEIGHT = 64;

Adafruit_SSD1306 display(OLED_WIDTH, OLED_HEIGHT, &Wire, -1);

static const uint32_t SCAN_WINDOW_MS = 3000;   // active scan duration
static const uint32_t SCAN_PAUSE_MS  = 2000;   // gap before next scan

// Sighting time bucket width. Spec is ~10 minutes; shorten this
// (e.g. 30000UL for 30s) only for bench-testing window rollover —
// put it back to the real value before field use.
static const uint32_t WINDOW_MS = 10UL * 60UL * 1000UL;

static const uint8_t MAX_TRACKED = 40;   // fixed table, no heap growth
static const uint32_t SUMMARY_INTERVAL_MS = 15000;  // print table every 15s

// Persistence scoring: flag a MAC if it's shown up in N+ distinct
// windows within the trailing SPAN windows. At the spec's 10-min
// WINDOW_MS, SPAN=6 means "3+ windows within the last hour".
static const uint8_t PERSISTENCE_THRESHOLD    = 3;
static const uint8_t PERSISTENCE_SPAN_WINDOWS = 6;
static const uint8_t WINDOW_HISTORY_LEN       = 8;  // ring buffer, >= SPAN

struct TrackedDevice {
  bool     used;
  char     mac[18];          // "xx:xx:xx:xx:xx:xx\0"
  int8_t   rssi;
  char     name[32];
  uint32_t lastSeenMs;
  uint32_t lastWindowIndex;
  uint16_t totalSightings;
  uint32_t windowHistory[WINDOW_HISTORY_LEN];  // most recent distinct window indices seen
  uint8_t  historyCount;                       // how many slots in windowHistory are valid
  uint8_t  historyHead;                        // next slot to write (ring buffer)
  bool     flagged;
};

static TrackedDevice tracked[MAX_TRACKED];
static uint32_t lastSummaryMs = 0;

static int findTracked(const char* mac) {
  for (int i = 0; i < MAX_TRACKED; i++) {
    if (tracked[i].used && strcmp(tracked[i].mac, mac) == 0) return i;
  }
  return -1;
}

// Returns index of a free slot, evicting the least-recently-seen
// entry if the table is full.
static int allocTracked() {
  for (int i = 0; i < MAX_TRACKED; i++) {
    if (!tracked[i].used) return i;
  }
  int oldest = 0;
  for (int i = 1; i < MAX_TRACKED; i++) {
    if (tracked[i].lastSeenMs < tracked[oldest].lastSeenMs) oldest = i;
  }
  return oldest;
}

// Count of windowHistory entries within PERSISTENCE_SPAN_WINDOWS of currentWindowIndex.
static uint8_t countRecentWindows(const TrackedDevice& t, uint32_t currentWindowIndex) {
  uint8_t count = 0;
  for (uint8_t i = 0; i < t.historyCount; i++) {
    uint32_t w = t.windowHistory[i];
    if (currentWindowIndex - w < PERSISTENCE_SPAN_WINDOWS) count++;
  }
  return count;
}

static void recordSighting(const char* mac, int8_t rssi, const char* name) {
  uint32_t now = millis();
  uint32_t windowIndex = now / WINDOW_MS;

  int idx = findTracked(mac);
  if (idx < 0) {
    idx = allocTracked();
    memset(&tracked[idx], 0, sizeof(TrackedDevice));
    tracked[idx].used = true;
    strncpy(tracked[idx].mac, mac, sizeof(tracked[idx].mac) - 1);
    tracked[idx].mac[sizeof(tracked[idx].mac) - 1] = '\0';
    tracked[idx].lastWindowIndex = windowIndex - 1;  // force first-window record below
  }

  TrackedDevice& t = tracked[idx];
  t.rssi = rssi;
  strncpy(t.name, name, sizeof(t.name) - 1);
  t.name[sizeof(t.name) - 1] = '\0';
  t.lastSeenMs = now;
  t.totalSightings++;

  if (windowIndex != t.lastWindowIndex) {
    t.windowHistory[t.historyHead] = windowIndex;
    t.historyHead = (t.historyHead + 1) % WINDOW_HISTORY_LEN;
    if (t.historyCount < WINDOW_HISTORY_LEN) t.historyCount++;
    t.lastWindowIndex = windowIndex;
  }

  if (!t.flagged && countRecentWindows(t, windowIndex) >= PERSISTENCE_THRESHOLD) {
    t.flagged = true;
    Serial.printf(
      "*** SUSPICIOUS: mac=%s flagged — seen in %u of last %u windows, rssi=%d ***\n",
      t.mac, countRecentWindows(t, windowIndex), PERSISTENCE_SPAN_WINDOWS, rssi
    );
  }
}

// ---------------------------------------------------------------------
// 2.4 — rotating-MAC awareness via Apple Find My payload matching
// ---------------------------------------------------------------------
// Company ID 0x004C (Apple) is transmitted little-endian as bytes
// 0x4C,0x00 at the start of manufacturer data; byte[2] == 0x12 is the
// "Find My" (offline finding) advertisement type used by AirTags and
// other Find My network accessories. Heuristic, not an Apple spec doc —
// field-confirm against a real AirTag/Find My device before trusting it.
static bool isAppleFindMyPayload(const std::string& mfgData) {
  if (mfgData.size() < 3) return false;
  const uint8_t* d = (const uint8_t*)mfgData.data();
  return d[0] == 0x4C && d[1] == 0x00 && d[2] == 0x12;
}

// Flags on sight rather than waiting for MAC persistence, since a
// rotating address won't accumulate window history on one MAC. Reuses
// the same tracked[] table/flagged bool as 2.3's persistence flag so
// printSummary()/the OLED warmer mode treat both flag sources alike.
static void flagRotatingMacSuspect(const char* mac, int8_t rssi) {
  int idx = findTracked(mac);
  if (idx < 0) idx = allocTracked();
  TrackedDevice& t = tracked[idx];
  if (!t.flagged) {
    t.flagged = true;
    Serial.printf(
      "*** SUSPICIOUS (rotating-MAC / Find My payload): mac=%s rssi=%d ***\n",
      mac, rssi
    );
  }
}

// ---------------------------------------------------------------------
// 2.5 — RSSI "getting warmer" mode: live OLED bar for the current
// highest-priority flagged device.
// ---------------------------------------------------------------------

struct HotTarget {
  bool     active;
  char     mac[18];
  int8_t   rssi;
  uint32_t lastUpdateMs;
};
static HotTarget hotTarget = { false, "", 0, 0 };
static const uint32_t HOT_TARGET_TIMEOUT_MS = 30000;  // fall back to idle screen if not re-seen

static void renderWarmerScreen() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println("TRACKING SUSPECT:");
  display.setCursor(0, 12);
  display.println(hotTarget.mac);
  display.setCursor(0, 26);
  display.print("rssi: ");
  display.println(hotTarget.rssi);

  // Bar: map rssi range [-100 far .. -30 very close] to full display width.
  int rssi = hotTarget.rssi;
  if (rssi < -100) rssi = -100;
  if (rssi > -30) rssi = -30;
  int barWidth = map(rssi, -100, -30, 0, OLED_WIDTH);
  display.drawRect(0, 44, OLED_WIDTH, 16, SSD1306_WHITE);
  display.fillRect(0, 44, barWidth, 16, SSD1306_WHITE);

  display.display();
}

static void renderIdleScreen() {
  int trackedCount = 0, flaggedCount = 0;
  for (int i = 0; i < MAX_TRACKED; i++) {
    if (tracked[i].used) {
      trackedCount++;
      if (tracked[i].flagged) flaggedCount++;
    }
  }
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println("BLE hunter - scanning");
  display.setCursor(0, 20);
  display.print("tracked: ");
  display.println(trackedCount);
  display.setCursor(0, 34);
  display.print("flagged: ");
  display.println(flaggedCount);
  display.display();
}

// Updates the hot target and redraws immediately from inside the scan
// callback — this is what makes the bar feel live while walking a
// device down, without adding any delay() to the render path.
static void updateHotTarget(const char* mac, int8_t rssi) {
  hotTarget.active = true;
  strncpy(hotTarget.mac, mac, sizeof(hotTarget.mac) - 1);
  hotTarget.mac[sizeof(hotTarget.mac) - 1] = '\0';
  hotTarget.rssi = rssi;
  hotTarget.lastUpdateMs = millis();
  renderWarmerScreen();
}

class ScanCallbacks : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* device) override {
    // Keep the std::string alive for the whole function — .c_str() on a
    // temporary would dangle the moment the statement that created it ends.
    std::string macStr = device->getAddress().toString();
    const char* mac = macStr.c_str();
    int8_t rssi = (int8_t)device->getRSSI();

    recordSighting(mac, rssi, device->haveName() ? device->getName().c_str() : "");

    bool appleFindMy = false;
    if (device->haveManufacturerData()) {
      appleFindMy = isAppleFindMyPayload(device->getManufacturerData());
      if (appleFindMy) flagRotatingMacSuspect(mac, rssi);
    }

    int idx = findTracked(mac);
    bool isFlagged = (idx >= 0 && tracked[idx].flagged) || appleFindMy;
    if (isFlagged) updateHotTarget(mac, rssi);
  }
};

static void printSummary() {
  uint32_t currentWindowIndex = millis() / WINDOW_MS;
  Serial.println("=== tracked devices (mac, rssi, recent_windows, sightings, flagged) ===");
  for (int i = 0; i < MAX_TRACKED; i++) {
    if (!tracked[i].used) continue;
    Serial.printf(
      "mac=%s  rssi=%d  recent_windows=%u/%u  sightings=%u  flagged=%s  name=\"%s\"\n",
      tracked[i].mac,
      tracked[i].rssi,
      countRecentWindows(tracked[i], currentWindowIndex),
      PERSISTENCE_SPAN_WINDOWS,
      tracked[i].totalSightings,
      tracked[i].flagged ? "YES" : "no",
      tracked[i].name
    );
  }
  Serial.println("=========================================================================");
}

NimBLEScan* pScan = nullptr;

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("BLE hunter node — steps 2.3-2.5");

  Wire.begin(OLED_SDA_PIN, OLED_SCL_PIN);
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_I2C_ADDR)) {
    Serial.println("SSD1306 init failed — check wiring/address (OLED_I2C_ADDR)");
  } else {
    display.clearDisplay();
    display.display();
  }

  NimBLEDevice::init("");
  pScan = NimBLEDevice::getScan();
  // wantDuplicates=true is required — NimBLE defaults to suppressing
  // onResult for any MAC it's already reported once, for the life of the
  // scan object (not just within one scan window). Persistence scoring
  // fundamentally needs repeat sightings of the same MAC across time, so
  // without this every device would only ever show sightings=1, forever
  // (confirmed live 2026-08-13 — this was the actual cause of 2.3 never
  // being able to fire, not an environment/RF issue as first suspected).
  pScan->setScanCallbacks(new ScanCallbacks(), true);
  pScan->setActiveScan(true);   // request scan responses (gets device names)
  pScan->setInterval(100);
  pScan->setWindow(99);
  // Second, SEPARATE duplicate filter — this one lives in the BLE
  // controller itself (NimBLEScan's default ctor sets filter_duplicates=1),
  // and silently drops repeat advertisements from an already-seen MAC
  // before they ever reach the host/software callback layer. The
  // wantDuplicates=true above only controls the software side; without
  // ALSO disabling this one, sightings would stick at 1 forever regardless.
  pScan->setDuplicateFilter(0);

  memset(tracked, 0, sizeof(tracked));
  lastSummaryMs = millis();
}

void loop() {
  pScan->start(SCAN_WINDOW_MS, false);  // NimBLE-Arduino 2.x: duration is milliseconds, not seconds
  pScan->clearResults();

  // 2.5: drop back to the idle screen once the tracked suspect hasn't
  // been re-seen in a while (walked out of range, or it stopped
  // advertising) rather than leaving a stale warmer-mode bar on screen.
  if (hotTarget.active && millis() - hotTarget.lastUpdateMs > HOT_TARGET_TIMEOUT_MS) {
    hotTarget.active = false;
  }
  if (!hotTarget.active) {
    renderIdleScreen();
  }

  if (millis() - lastSummaryMs >= SUMMARY_INTERVAL_MS) {
    printSummary();
    lastSummaryMs = millis();
  }

  delay(SCAN_PAUSE_MS);
}
