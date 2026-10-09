// CyberDeck v2 — bench prototype: "Tracker Tripwire"
//
// A standalone counter-surveillance BLE scanner that runs on the HUB BOARD ALONE
// (ESP32-S3-DevKitC-1 + 2.8" ILI9341) with a breadboarded active buzzer and LED.
// No nodes, no GPS, no PCB needed. It proves out the Tier-1 (tracker detection +
// persistence scoring) and Tier-2 (LED + buzzer alert) counter-surveillance features
// before any of that goes into the real split hub/node firmware.
//
// What it does:
//   - Scans BLE continuously and classifies each device (AirTag/Find My, Tile,
//     Samsung SmartTag, or generic).
//   - Scores PERSISTENCE: how long a given MAC keeps reappearing. With no GPS yet,
//     "sticky over time" is the stand-in for "it's moving with me."
//   - ALERTS (red banner + LED + buzzer chirp) when a *tracker-class* device has been
//     sticky past STICKY_MS. BOOT button (GPIO0) acknowledges/snoozes the buzzer.
//   - Shows a live, colour-coded list on the TFT, sorted by persistence.
//
// Board:     ESP32-S3 Dev Module ("USB CDC On Boot: Enabled"). BLE is built in.
// Libraries: Adafruit GFX, Adafruit ILI9341 (BLE comes with the ESP32 core).
//
// Wiring (matches docs/PCB-BOM-AND-NETLIST.md so pins carry over to the real hub):
//   ILI9341:  SCK=GPIO12  MOSI=GPIO11  MISO=GPIO13  CS=GPIO10  DC=GPIO9  RST=GPIO8  LED/BL=GPIO7(+3V3)
//   Buzzer:   GPIO18 -> active buzzer(+),  buzzer(-) -> GND          (J_SPARE pin)
//   LED:      GPIO21 -> 220ohm -> LED(+),  LED(-) -> GND             (J_SPARE pin)
//   Ack btn:  the on-board BOOT button (GPIO0), active low           (no wiring needed)

#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>

// ---------------------------------------------------------------------------------------
// Pins (same as the real hub so this ports cleanly)
// ---------------------------------------------------------------------------------------
static const int PIN_SCLK = 12, PIN_MOSI = 11, PIN_MISO = 13;
static const int PIN_TFT_CS = 10, PIN_TFT_DC = 9, PIN_TFT_RST = 8, PIN_TFT_BL = 7;
static const int PIN_BUZZER = 18;
static const int PIN_LED    = 21;
static const int PIN_ACK    = 0;    // BOOT button, active low

// ---------------------------------------------------------------------------------------
// Tunables
// ---------------------------------------------------------------------------------------
// How long a tracker must keep reappearing before it counts as "following you" and fires
// the alert. 45 s is friendly for a bench/walk test; raise to a few minutes for real use
// to cut false alarms from trackers that are simply parked near you (a neighbour's AirTag).
static const uint32_t STICKY_MS      = 45000;
static const uint32_t FORGET_MS      = 120000;  // drop a device not seen for this long
static const uint32_t SNOOZE_MS      = 60000;   // BOOT press silences the buzzer this long
static const uint32_t CHIRP_GAP_MS   = 4000;    // min gap between buzzer chirps
static const int      TABLE_MAX      = 150;

// Defined up here (before any function) so Arduino's auto-generated prototypes, which are
// hoisted to the top of the file, can see the type.
struct Dev {
  bool     used;
  uint8_t  mac[6];
  uint8_t  cls;
  bool     randomAddr;
  int8_t   bestRssi;
  int8_t   lastRssi;
  uint32_t firstMs;     // first time ever heard  -> persistence = lastMs - firstMs
  uint32_t lastMs;      // most recent hit
  uint16_t hits;
  bool     alerted;     // buzzer already fired for this one
  char     name[20];
};

// ---------------------------------------------------------------------------------------
// Device classes
// ---------------------------------------------------------------------------------------
enum { CLS_NONE = 0, CLS_FINDMY, CLS_TILE, CLS_SMARTTAG, CLS_GENERIC };
static const char* clsName(uint8_t c) {
  switch (c) {
    case CLS_FINDMY:   return "FindMy/AirTag";
    case CLS_TILE:     return "Tile";
    case CLS_SMARTTAG: return "SmartTag";
    case CLS_GENERIC:  return "device";
    default:           return "-";
  }
}
static bool isTracker(uint8_t c) { return c == CLS_FINDMY || c == CLS_TILE || c == CLS_SMARTTAG; }

// ---------------------------------------------------------------------------------------
// Device table
// ---------------------------------------------------------------------------------------
static Dev      tab[TABLE_MAX];
static int      devCount = 0;
static uint32_t alertsTotal = 0;

static portMUX_TYPE tabMux = portMUX_INITIALIZER_UNLOCKED;  // scan runs in the BLE task

static Dev* findOrAdd(const uint8_t* mac, bool* isNew) {
  int free = -1, oldest = 0;
  for (int i = 0; i < TABLE_MAX; i++) {
    if (!tab[i].used) { if (free < 0) free = i; continue; }
    if (memcmp(tab[i].mac, mac, 6) == 0) { *isNew = false; return &tab[i]; }
    if (tab[i].lastMs < tab[oldest].lastMs || !tab[oldest].used) oldest = i;
  }
  *isNew = true;
  int slot = free >= 0 ? free : oldest;      // evict the stalest if the table is full
  if (free >= 0) devCount++;
  memset(&tab[slot], 0, sizeof(Dev));
  tab[slot].used = true;
  memcpy(tab[slot].mac, mac, 6);
  return &tab[slot];
}

static uint32_t persistence(const Dev& d) { return d.lastMs - d.firstMs; }
static bool     sticky(const Dev& d)      { return persistence(d) >= STICKY_MS; }

// ---------------------------------------------------------------------------------------
// BLE classification (same logic family as node_ble.ino, expanded with SmartTag)
// ---------------------------------------------------------------------------------------
class AdvCallback : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice dev) override {
    uint8_t mac[6];
    memcpy(mac, dev.getAddress().getNative(), 6);   // core 3.x: getNative() returns uint8_t*
    int8_t rssi = (int8_t)dev.getRSSI();

    uint8_t cls = CLS_GENERIC;

    // Apple Find My / AirTag: manufacturer 0x004C, continuity type 0x12.
    auto md = dev.getManufacturerData();
    if (md.length() >= 3) {
      uint16_t mfg = (uint8_t)md[0] | ((uint16_t)(uint8_t)md[1] << 8);
      if (mfg == 0x004C && (uint8_t)md[2] == 0x12) cls = CLS_FINDMY;
    }
    // Tile: service UUID 0xFEED / 0xFEEC.
    if (dev.haveServiceUUID() &&
        (dev.isAdvertisingService(BLEUUID((uint16_t)0xFEED)) ||
         dev.isAdvertisingService(BLEUUID((uint16_t)0xFEEC))))
      cls = CLS_TILE;
    // Samsung SmartTag (SmartThings Find): service UUID 0xFD5A.
    if (dev.haveServiceUUID() && dev.isAdvertisingService(BLEUUID((uint16_t)0xFD5A)))
      cls = CLS_SMARTTAG;

    char name[20] = {0};
    uint8_t nameLen = 0;
    if (dev.haveName()) {
      const char* n = dev.getName().c_str();
      nameLen = (uint8_t)min((size_t)19, strlen(n));
      memcpy(name, n, nameLen);
    }

    uint32_t now = millis();
    portENTER_CRITICAL(&tabMux);
    bool isNew;
    Dev* d = findOrAdd(mac, &isNew);
    if (isNew) { d->firstMs = now; d->bestRssi = rssi; d->cls = cls; }
    // Keep the most specific classification we've ever seen for this MAC.
    if (cls != CLS_GENERIC) d->cls = cls;
    d->randomAddr = dev.getAddressType() != BLE_ADDR_PUBLIC;   // core 3.x renamed the enum
    d->lastRssi = rssi;
    if (rssi > d->bestRssi) d->bestRssi = rssi;
    d->lastMs = now;
    d->hits++;
    if (nameLen && d->name[0] == '\0') { memcpy(d->name, name, nameLen); }
    portEXIT_CRITICAL(&tabMux);
  }
};

// ---------------------------------------------------------------------------------------
// Alert layer (LED + active buzzer, driven straight from the GPIO)
// ---------------------------------------------------------------------------------------
static bool     ledOn = false;
static uint32_t snoozeUntil = 0, lastChirpMs = 0, chirpOffMs = 0;

static void startChirp() {
  uint32_t now = millis();
  if (now < snoozeUntil || now - lastChirpMs < CHIRP_GAP_MS) return;
  digitalWrite(PIN_BUZZER, HIGH);
  chirpOffMs  = now + 150;   // 150 ms beep, turned off in serviceAlerts()
  lastChirpMs = now;
}

static void serviceAlerts(bool anyAlert) {
  uint32_t now = millis();
  if (chirpOffMs && now >= chirpOffMs) { digitalWrite(PIN_BUZZER, LOW); chirpOffMs = 0; }
  // LED: steady while any tracker alert stands, off otherwise.
  bool want = anyAlert;
  if (want != ledOn) { ledOn = want; digitalWrite(PIN_LED, want ? HIGH : LOW); }
}

static void pollAckButton() {
  static bool wasDown = false;
  bool down = digitalRead(PIN_ACK) == LOW;
  if (down && !wasDown) {                 // press = snooze buzzer + clear current latches
    snoozeUntil = millis() + SNOOZE_MS;
    digitalWrite(PIN_BUZZER, LOW); chirpOffMs = 0;
  }
  wasDown = down;
}

// ---------------------------------------------------------------------------------------
// Display (flicker-free row cache, same technique as hub.ino)
// ---------------------------------------------------------------------------------------
static SPIClass hubSpi(FSPI);
static Adafruit_ILI9341 tft(&hubSpi, PIN_TFT_DC, PIN_TFT_CS, PIN_TFT_RST);

static const uint16_t C_BG = 0x0000, C_FG = 0xFFFF, C_GRAY = 0x8410, C_GREEN = 0x07E0,
                      C_RED = 0xF800, C_YELLOW = 0xFFE0, C_CYAN = 0x07FF, C_HEAD = 0x0010;
static const int ROW_H = 10, COLS = 53, ROWS = 24;

static char     rowCache[ROWS][COLS + 1];
static uint16_t rowFg[ROWS], rowBg[ROWS];

static void drawRow(int row, const char* text, uint16_t fg = C_FG, uint16_t bg = C_BG) {
  char buf[COLS + 1];
  snprintf(buf, sizeof(buf), "%-*.*s", COLS, COLS, text);
  if (strcmp(buf, rowCache[row]) == 0 && rowFg[row] == fg && rowBg[row] == bg) return;
  strcpy(rowCache[row], buf);
  rowFg[row] = fg; rowBg[row] = bg;
  tft.setCursor(0, row * ROW_H + 1);
  tft.setTextColor(fg, bg);
  tft.print(buf);
}

static void fmtMac(char* out, const uint8_t* m) {
  snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
}

// Sorted view: trackers first, then by persistence.
static int idx[TABLE_MAX];
static int cmpDev(const void* a, const void* b) {
  const Dev& x = tab[*(const int*)a]; const Dev& y = tab[*(const int*)b];
  int xt = isTracker(x.cls), yt = isTracker(y.cls);
  if (xt != yt) return yt - xt;
  uint32_t px = persistence(x), py = persistence(y);
  return (py > px) - (py < px);
}

static void render() {
  uint32_t now = millis();

  // Snapshot counts under the lock, then release (the list draw can race harmlessly).
  int total = 0, trackers = 0, alerts = 0;
  portENTER_CRITICAL(&tabMux);
  for (int i = 0; i < TABLE_MAX; i++) {
    if (!tab[i].used) continue;
    if (now - tab[i].lastMs > FORGET_MS) { tab[i].used = false; devCount--; continue; }
    total++;
    if (isTracker(tab[i].cls)) trackers++;
    if (isTracker(tab[i].cls) && sticky(tab[i])) alerts++;
  }
  portEXIT_CRITICAL(&tabMux);

  char b[80];
  bool snoozed = now < snoozeUntil;
  drawRow(0, "CyberDeck  TRACKER TRIPWIRE", C_CYAN, C_HEAD);
  snprintf(b, sizeof(b), "devices %d   trackers %d   ALERTS %d%s", total, trackers, alerts,
           snoozed ? "  [muted]" : "");
  drawRow(1, b, alerts ? C_RED : C_FG, C_HEAD);

  if (alerts) drawRow(2, "!! A TRACKER IS FOLLOWING YOU -- BOOT=mute", C_RED, C_HEAD);
  else        drawRow(2, "RSSI  SEEN  CLASS          NAME/ADDRESS", C_YELLOW);

  int n = 0;
  for (int i = 0; i < TABLE_MAX; i++) if (tab[i].used) idx[n++] = i;
  qsort(idx, n, sizeof(int), cmpDev);

  const int listTop = 3, listRows = ROWS - 4;
  for (int r = 0; r < listRows; r++) {
    int row = listTop + r;
    if (r >= n) { drawRow(row, ""); continue; }
    const Dev& d = tab[idx[r]];
    char mac[18], who[24];
    fmtMac(mac, d.mac);
    if (d.name[0]) snprintf(who, sizeof(who), "%s", d.name);
    else           snprintf(who, sizeof(who), "%s%s", mac, d.randomAddr ? " ~" : "");
    uint32_t secs = persistence(d) / 1000;
    snprintf(b, sizeof(b), "%4d %4lus  %-13s  %s", d.lastRssi, (unsigned long)secs, clsName(d.cls), who);

    uint16_t col;
    if (isTracker(d.cls)) col = sticky(d) ? C_RED : C_YELLOW;
    else                  col = (now - d.lastMs > 30000) ? C_GRAY : C_GREEN;
    drawRow(row, b, col);
  }

  // Fire the buzzer once per newly-sticky tracker, and keep the LED/alert state current.
  bool anyAlert = false;
  portENTER_CRITICAL(&tabMux);
  for (int i = 0; i < TABLE_MAX; i++) {
    if (!tab[i].used || !isTracker(tab[i].cls)) continue;
    if (sticky(tab[i])) {
      anyAlert = true;
      if (!tab[i].alerted) { tab[i].alerted = true; alertsTotal++; portEXIT_CRITICAL(&tabMux); startChirp(); portENTER_CRITICAL(&tabMux); }
    }
  }
  portEXIT_CRITICAL(&tabMux);
  serviceAlerts(anyAlert);
}

// ---------------------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);

  pinMode(PIN_BUZZER, OUTPUT); digitalWrite(PIN_BUZZER, LOW);
  pinMode(PIN_LED, OUTPUT);    digitalWrite(PIN_LED, LOW);
  pinMode(PIN_ACK, INPUT_PULLUP);
  pinMode(PIN_TFT_CS, OUTPUT); digitalWrite(PIN_TFT_CS, HIGH);
  pinMode(PIN_TFT_BL, OUTPUT); digitalWrite(PIN_TFT_BL, HIGH);

  hubSpi.begin(PIN_SCLK, PIN_MISO, PIN_MOSI, -1);
  tft.begin(40000000);
  tft.setRotation(1);        // 320x240 landscape
  tft.fillScreen(C_BG);
  tft.setTextSize(1);
  tft.setTextWrap(false);
  for (int r = 0; r < ROWS; r++) { rowCache[r][0] = 1; rowCache[r][1] = 0; }

  drawRow(0, "CyberDeck  TRACKER TRIPWIRE", C_CYAN, C_HEAD);
  drawRow(2, "starting BLE scan...", C_GRAY);

  BLEDevice::init("");
  BLEScan* scan = BLEDevice::getScan();
  static AdvCallback cb;
  scan->setAdvertisedDeviceCallbacks(&cb, true);  // keep duplicates -> see RSSI/persistence
  scan->setActiveScan(true);                      // also pulls scan-response names
  scan->setInterval(100);
  scan->setWindow(99);
  Serial.println("Tracker Tripwire up");
}

void loop() {
  BLEScan* scan = BLEDevice::getScan();
  scan->start(3, false);     // blocks ~3 s while callbacks fire
  scan->clearResults();      // free the result vector so RAM doesn't grow

  pollAckButton();
  render();

  // keep the buzzer/LED timing responsive during the short gap between scans
  for (int i = 0; i < 20; i++) { pollAckButton(); serviceAlerts(ledOn); delay(10); }
}
