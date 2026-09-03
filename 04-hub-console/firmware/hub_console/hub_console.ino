// Component 4 — Hub Console (ESP32-1732S019 + integrated 1.9" ST7789 screen)
//
// Covers README steps 4.1-4.4. Board is a Sunton/NDE3D ESP32-1732S019 —
// see docs/ESP32-1732S019_Datasheet.pdf. Display is driven via LovyanGFX
// using that datasheet's verified pinout (HSPI: SCLK14/MOSI13/CS15/DC2,
// RST tied to EN, backlight GPIO21). Non-touch, no physical buttons.
//
// The ESP-NOW (4.2) receiver aggregates the active field nodes (wifi
// sniffer, wifi spectrum) into in-memory tables and renders everything on
// one static dashboard (renderDashboard()) — no screen switching, no
// auto-cycle timer, no serial-digit navigation. The board has no touch and
// no physical buttons, so a single always-current screen is the only UI
// that's usable standalone with no laptop tethered.
//
// Sub-ghz (node_id=3) is intentionally NOT handled here right now — that
// board's hardware is confirmed faulty and isn't in use, so this hub
// doesn't carry the dead weight of a table/section for a node that will
// never report. If/when replacement sub-ghz hardware comes in, node_id=3
// handling can be reintroduced (see git history for the prior version:
// SubGhzEntry/subghzTable/applySubGhzReport() + the SUBGHZ dashboard
// section + the "sgz" health tag).
//
// Needs the shared struct from 05-integration — this include assumes the
// firmware folder stays at its current relative path under the repo.
#include "../../../05-integration/shared/deck_report.h"

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

#define LGFX_USE_V1
#include <LovyanGFX.hpp>

class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ST7789 _panel;
  lgfx::Bus_SPI _bus;
public:
  LGFX() {
    { auto c = _bus.config();
      c.spi_host = SPI2_HOST;  // HSPI
      c.spi_mode = 0;
      c.freq_write = 40000000;
      c.pin_sclk = 14;
      c.pin_mosi = 13;
      c.pin_miso = -1;
      c.pin_dc = 2;
      _bus.config(c); _panel.setBus(&_bus); }
    { auto c = _panel.config();
      c.pin_cs = 15;
      c.pin_rst = -1;         // reset tied to EN
      c.panel_width = 170;
      c.panel_height = 320;
      c.offset_x = 35;        // 170-wide panel offset
      c.offset_y = 0;
      c.invert = true;        // IPS panel
      c.rgb_order = false;
      _panel.config(c); }
    setPanel(&_panel);
  }
};

static LGFX tft;
static const uint8_t TFT_BACKLIGHT_PIN = 21;

// ---------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------

static const uint8_t  MAX_WIFI_ENTRIES = 32;
static const uint8_t  MAX_DEVICE_ENTRIES = 16;
static const uint32_t NODE_STALE_MS = 60000;   // node considered offline after this long silent
static const uint32_t REFRESH_INTERVAL_MS = 3000;
static const uint8_t  NUM_SPECTRUM_CHANNELS = 13;

// NODE_SUBGHZ (3) exists in the wire format (deck_report.h) but isn't
// handled here — see header comment.
enum NodeId { NODE_WIFI = 1, NODE_WIFI_SPECTRUM = 2 };

// ---------------------------------------------------------------------
// 4.4 — aggregation store (in-memory; SD-card rolling log is a later add)
// ---------------------------------------------------------------------

struct WifiEntry {
  bool used;
  uint8_t mac[6];
  char label[20];
  int8_t rssi;
  uint8_t channel;
  uint8_t encType;   // DECK_ENC_*
  bool hidden;
  uint32_t lastSeenMs;
};

// Nearby client devices (probe-req senders) — a device's own MAC + the
// SSID it's asking for (often empty, since modern phones randomize MACs
// and many suppress the SSID in probes — MAC+RSSI+channel alone is still
// "something is here"). No enc/hidden fields — those describe an AP, not
// a client.
struct DeviceEntry {
  bool used;
  uint8_t mac[6];
  char label[20];
  int8_t rssi;
  uint8_t channel;
  uint32_t lastSeenMs;
};

struct NodeHealth {
  bool everSeen;
  uint32_t lastSeenMs;
  uint32_t totalReports;
};

static WifiEntry   wifiTable[MAX_WIFI_ENTRIES];
static DeviceEntry deviceTable[MAX_DEVICE_ENTRIES];
static NodeHealth  nodeHealth[3];  // index by NodeId (1-2 used, 0 unused)

// Spectrum node reports one full-sweep snapshot at a time — no per-entry
// table needed, just the latest bar values.
static uint8_t  channelUtil[NUM_SPECTRUM_CHANNELS];
static uint32_t spectrumLastSeenMs = 0;

// ---------------------------------------------------------------------
// MAC OUI -> vendor lookup (small static table, common consumer/IoT
// vendors only — not exhaustive, "?" for anything unlisted).
// ---------------------------------------------------------------------
struct OuiEntry { uint8_t oui[3]; const char* name; };
static const OuiEntry OUI_TABLE[] = {
  {{0x00, 0x1B, 0x63}, "Apple"},
  {{0x28, 0xCF, 0xE9}, "Apple"},
  {{0x3C, 0x22, 0xFB}, "Apple"},
  {{0xA4, 0x83, 0xE7}, "Apple"},
  {{0xF0, 0x18, 0x98}, "Apple"},
  {{0x00, 0x16, 0x6C}, "Samsung"},
  {{0x8C, 0x71, 0xF8}, "Samsung"},
  {{0xE8, 0x50, 0x8B}, "Samsung"},
  {{0x54, 0x60, 0x09}, "Google"},
  {{0xF4, 0xF5, 0xD8}, "Google"},
  {{0x18, 0xB4, 0x30}, "Amazon"},
  {{0x68, 0x37, 0xE9}, "Amazon"},
  {{0xFC, 0xF5, 0xC4}, "Espressif"},
  {{0x24, 0x6F, 0x28}, "Espressif"},
  {{0xB8, 0x27, 0xEB}, "Raspberry Pi"},
  {{0xDC, 0xA6, 0x32}, "Raspberry Pi"},
  {{0xE4, 0x5F, 0x01}, "Raspberry Pi"},
  {{0x00, 0x1A, 0x11}, "Google"},
  {{0x00, 0x0C, 0x29}, "VMware"},
  {{0x00, 0x50, 0x56}, "VMware"},
  {{0x00, 0x1D, 0xD8}, "Microsoft"},
  {{0x7C, 0x1E, 0x52}, "Microsoft"},
  {{0x00, 0x25, 0x9C}, "Cisco"},
  {{0x00, 0x1F, 0x3B}, "Intel"},
  {{0x3C, 0x97, 0x0E}, "TP-Link"},
  {{0x50, 0xC7, 0xBF}, "TP-Link"},
  {{0xEC, 0x08, 0x6B}, "TP-Link"},
  {{0xB0, 0x4E, 0x26}, "Ubiquiti"},
  {{0x24, 0xA4, 0x3C}, "Ubiquiti"},
};
static const uint8_t OUI_TABLE_LEN = sizeof(OUI_TABLE) / sizeof(OUI_TABLE[0]);

static const char* lookupVendor(const uint8_t* mac) {
  for (uint8_t i = 0; i < OUI_TABLE_LEN; i++) {
    if (memcmp(mac, OUI_TABLE[i].oui, 3) == 0) return OUI_TABLE[i].name;
  }
  return "?";
}

static int findWifi(const uint8_t* mac) {
  for (int i = 0; i < MAX_WIFI_ENTRIES; i++)
    if (wifiTable[i].used && memcmp(wifiTable[i].mac, mac, 6) == 0) return i;
  return -1;
}
static int allocWifi() {
  for (int i = 0; i < MAX_WIFI_ENTRIES; i++) if (!wifiTable[i].used) return i;
  int oldest = 0;
  for (int i = 1; i < MAX_WIFI_ENTRIES; i++)
    if (wifiTable[i].lastSeenMs < wifiTable[oldest].lastSeenMs) oldest = i;
  return oldest;
}

static int findDevice(const uint8_t* mac) {
  for (int i = 0; i < MAX_DEVICE_ENTRIES; i++)
    if (deviceTable[i].used && memcmp(deviceTable[i].mac, mac, 6) == 0) return i;
  return -1;
}
static int allocDevice() {
  for (int i = 0; i < MAX_DEVICE_ENTRIES; i++) if (!deviceTable[i].used) return i;
  int oldest = 0;
  for (int i = 1; i < MAX_DEVICE_ENTRIES; i++)
    if (deviceTable[i].lastSeenMs < deviceTable[oldest].lastSeenMs) oldest = i;
  return oldest;
}

static void touchNodeHealth(uint8_t nodeId) {
  if (nodeId < 1 || nodeId > 2) return;
  nodeHealth[nodeId].everSeen = true;
  nodeHealth[nodeId].lastSeenMs = millis();
  nodeHealth[nodeId].totalReports++;
}

// ---------------------------------------------------------------------
// 4.2 — ESP-NOW receiver (wifi + ble nodes)
// ---------------------------------------------------------------------

static void applyWifiEntry(const deck_wifi_entry_t& e) {
  bool isDevice = (e.enc_and_flags & DECK_WIFI_ENTRY_IS_DEVICE) != 0;
  if (isDevice) {
    int idx = findDevice(e.mac);
    if (idx < 0) idx = allocDevice();
    DeviceEntry& d = deviceTable[idx];
    d.used = true;
    memcpy(d.mac, e.mac, 6);
    strncpy(d.label, e.label, sizeof(d.label) - 1);
    d.label[sizeof(d.label) - 1] = '\0';
    d.rssi = e.rssi;
    d.channel = e.channel;
    d.lastSeenMs = millis();
  } else {
    int idx = findWifi(e.mac);
    if (idx < 0) idx = allocWifi();
    WifiEntry& w = wifiTable[idx];
    w.used = true;
    memcpy(w.mac, e.mac, 6);
    strncpy(w.label, e.label, sizeof(w.label) - 1);
    w.label[sizeof(w.label) - 1] = '\0';
    w.rssi = e.rssi;
    w.channel = e.channel;
    w.encType = e.enc_and_flags & DECK_WIFI_ENTRY_ENC_MASK;
    w.hidden = (e.enc_and_flags & DECK_WIFI_ENTRY_HIDDEN) != 0;
    w.lastSeenMs = millis();
  }
}

static void applyWifiBatch(const deck_wifi_batch_t& batch) {
  uint8_t n = batch.count > DECK_WIFI_BATCH_MAX_ENTRIES ? DECK_WIFI_BATCH_MAX_ENTRIES : batch.count;
  for (uint8_t i = 0; i < n; i++) applyWifiEntry(batch.entries[i]);
}

static void applySpectrumReport(const deck_report_t& r) {
  memcpy(channelUtil, r.chan_util, NUM_SPECTRUM_CHANNELS);
  spectrumLastSeenMs = millis();
}

// arduino-esp32 core >= 2.0.x signature. If building against an older
// core, swap this for: void onEspNowRecv(const uint8_t* mac, const
// uint8_t* data, int len)
static void onEspNowRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  Serial.printf("RX len=%d (wifi_batch=%d report=%d)\n",
                len, (int)sizeof(deck_wifi_batch_t), (int)sizeof(deck_report_t));

  // Wifi batches are a different (larger) fixed size than deck_report_t,
  // so an exact length match unambiguously tells the two apart — see
  // deck_wifi_batch_t's comment in deck_report.h.
  if (len == sizeof(deck_wifi_batch_t)) {
    deck_wifi_batch_t batch;
    memcpy(&batch, data, sizeof(batch));
    Serial.printf("  wifi batch: node=%u count=%u\n", batch.node_id, batch.count);
    touchNodeHealth(batch.node_id);
    applyWifiBatch(batch);
    return;
  }

  if (len != sizeof(deck_report_t)) {
    Serial.println("  size matched neither struct — dropped");
    return;
  }
  deck_report_t report;
  memcpy(&report, data, sizeof(report));

  touchNodeHealth(report.node_id);
  if (report.flags & DECK_REPORT_FLAG_HEARTBEAT) return;  // health-only, no sensor data
  switch (report.node_id) {
    case NODE_WIFI_SPECTRUM: applySpectrumReport(report); break;
    default: break;  // NODE_WIFI only sends batches + heartbeats now; sub-ghz inactive
  }
}

static void setupEspNow() {
  WiFi.mode(WIFI_STA);
  delay(100);  // WiFi.macAddress() reads all-zero if queried immediately
               // after mode(STA), before the driver finishes bringing up —
               // confirmed via eFuse-vs-macAddress() diagnostic on real hw.
  esp_wifi_set_channel(DECK_ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed");
    return;
  }
  esp_now_register_recv_cb(onEspNowRecv);
  Serial.print("Hub MAC (give this to the node sketches' peer config): ");
  Serial.println(WiFi.macAddress());
}

// ---------------------------------------------------------------------
// 4.1 — dashboard UI, synthetic data until real links are live
// ---------------------------------------------------------------------

static void loadSyntheticData() {
  // A couple of fake WiFi APs
  memset(&wifiTable[0], 0, sizeof(WifiEntry));
  wifiTable[0].used = true;
  uint8_t mac1[6] = {0xAA, 0xBB, 0xCC, 0x11, 0x22, 0x33};
  memcpy(wifiTable[0].mac, mac1, 6);
  strncpy(wifiTable[0].label, "HomeNet-5G", sizeof(wifiTable[0].label) - 1);
  wifiTable[0].rssi = -52;
  wifiTable[0].channel = 6;
  wifiTable[0].lastSeenMs = millis();

  memset(&wifiTable[1], 0, sizeof(WifiEntry));
  wifiTable[1].used = true;
  uint8_t mac2[6] = {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x01};
  memcpy(wifiTable[1].mac, mac2, 6);
  strncpy(wifiTable[1].label, "", sizeof(wifiTable[1].label) - 1);
  wifiTable[1].rssi = -71;
  wifiTable[1].channel = 11;
  wifiTable[1].encType = DECK_ENC_WPA2;
  wifiTable[1].hidden = true;
  wifiTable[1].lastSeenMs = millis();

  wifiTable[0].encType = DECK_ENC_WPA3;

  // A fake spectrum snapshot — rising utilization toward the middle channels
  for (uint8_t i = 0; i < NUM_SPECTRUM_CHANNELS; i++) {
    int mid = NUM_SPECTRUM_CHANNELS / 2;
    int dist = abs((int)i - mid);
    channelUtil[i] = (uint8_t)(200 - dist * 25 > 0 ? 200 - dist * 25 : 10);
  }
  spectrumLastSeenMs = millis();

  // Pretend both active nodes have reported recently
  for (int i = 1; i <= 2; i++) {
    nodeHealth[i].everSeen = true;
    nodeHealth[i].lastSeenMs = millis();
    nodeHealth[i].totalReports = 1;
  }
}

static const char* encTypeShort(uint8_t encType) {
  switch (encType) {
    case DECK_ENC_OPEN: return "O";
    case DECK_ENC_WEP:  return "WEP";
    case DECK_ENC_WPA:  return "WPA";
    case DECK_ENC_WPA2: return "WPA2";
    case DECK_ENC_WPA3: return "WPA3";
    default:            return "?";
  }
}

// Same SSID string (non-empty) seen from 2+ different BSSIDs — classic
// evil-twin/rogue-AP shape. Cheap O(n^2) over a <=32-entry table, run
// fresh at render time rather than tracked incrementally, since entries
// can also age out/get evicted (unlike the old BLE per-entry flagged bit,
// which only ever needed to be set once).
static bool isDuplicateSsid(int idx) {
  WifiEntry& target = wifiTable[idx];
  if (target.label[0] == '\0') return false;
  for (int i = 0; i < MAX_WIFI_ENTRIES; i++) {
    if (i == idx || !wifiTable[i].used) continue;
    if (strcmp(wifiTable[i].label, target.label) == 0 &&
        memcmp(wifiTable[i].mac, target.mac, 6) != 0) {
      return true;
    }
  }
  return false;
}

static const char* nodeTag(int id) {
  switch (id) {
    case NODE_WIFI:          return "wifi";
    case NODE_WIFI_SPECTRUM: return "spec";
    default:                 return "?";
  }
}

// One-line health strip: a short tag per node, colored by state, plus age
// in seconds — no separate health screen needed, it's always visible.
static void drawHealthStrip() {
  uint32_t now = millis();
  tft.setTextColor(TFT_WHITE);
  tft.print("health ");
  for (int id = 1; id <= 2; id++) {
    NodeHealth& h = nodeHealth[id];
    if (!h.everSeen) {
      tft.setTextColor(TFT_DARKGREY);
      tft.printf("%s:never  ", nodeTag(id));
      continue;
    }
    uint32_t age = (now - h.lastSeenMs) / 1000;
    bool stale = (now - h.lastSeenMs) > NODE_STALE_MS;
    tft.setTextColor(stale ? TFT_RED : TFT_GREEN);
    tft.printf("%s:%lus  ", nodeTag(id), (unsigned long)age);
  }
  tft.println();
}

// Row caps sized so the worst case (every section maxed, both "+more"
// lines shown) still fits the 170px-tall panel at text size 1 (~8px/line):
// 1 health + 1 net-hdr + 8 wifi + 1 more + 1 dev-hdr + 4 dev + 1 more +
// 1 spec-hdr = 17 lines (~136px) + 16px bar row + 13px top offset =
// ~165px, comfortably under 170. No sub-ghz section — see header comment.
static const uint8_t MAX_VISIBLE_WIFI_ROWS = 8;
static const uint8_t MAX_VISIBLE_DEVICE_ROWS = 4;

// Single static dashboard — everything visible at once, no screen
// switching. Redrawn on every REFRESH_INTERVAL_MS tick.
static void renderDashboard() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE);
  tft.setTextSize(1);
  tft.setCursor(0, 0);

  drawHealthStrip();
  tft.drawFastHLine(0, 10, tft.width(), TFT_DARKGREY);
  tft.setCursor(0, 13);

  // --- Networks (APs) ---
  int wifiCount = 0, hiddenCount = 0, dupCount = 0, openCount = 0;
  for (int i = 0; i < MAX_WIFI_ENTRIES; i++) {
    if (!wifiTable[i].used) continue;
    wifiCount++;
    if (wifiTable[i].hidden) hiddenCount++;
    if (wifiTable[i].encType == DECK_ENC_OPEN) openCount++;
    if (isDuplicateSsid(i)) dupCount++;
  }
  tft.setTextColor(TFT_CYAN);
  tft.printf("NETWORKS %d  hidden:%d open:%d", wifiCount, hiddenCount, openCount);
  if (dupCount > 0) { tft.setTextColor(TFT_RED); tft.printf("  rogue:%d", dupCount); }
  tft.println();

  uint8_t order[MAX_WIFI_ENTRIES];
  uint8_t n = 0;
  for (uint8_t i = 0; i < MAX_WIFI_ENTRIES; i++) if (wifiTable[i].used) order[n++] = i;
  for (uint8_t i = 1; i < n; i++) {
    uint8_t key = order[i];
    int8_t keyRssi = wifiTable[key].rssi;
    int j = i - 1;
    while (j >= 0 && wifiTable[order[j]].rssi < keyRssi) {
      order[j + 1] = order[j];
      j--;
    }
    order[j + 1] = key;
  }
  uint8_t shownWifi = n < MAX_VISIBLE_WIFI_ROWS ? n : MAX_VISIBLE_WIFI_ROWS;
  for (uint8_t k = 0; k < shownWifi; k++) {
    WifiEntry& e = wifiTable[order[k]];
    bool dup = isDuplicateSsid(order[k]);
    tft.setTextColor(dup ? TFT_RED : TFT_WHITE);
    tft.printf(" %s%s%s %ddBm ch%u %s %s\n",
               e.label[0] ? e.label : "(unnamed)", e.hidden ? "[H]" : "", dup ? "[DUP]" : "",
               e.rssi, e.channel, encTypeShort(e.encType), lookupVendor(e.mac));
  }
  if (n > shownWifi) {
    tft.setTextColor(TFT_DARKGREY);
    tft.printf(" +%u more\n", n - shownWifi);
  }

  // --- Nearby devices (probe-req senders) ---
  int devCount = 0;
  for (int i = 0; i < MAX_DEVICE_ENTRIES; i++) if (deviceTable[i].used) devCount++;
  tft.setTextColor(TFT_CYAN);
  tft.printf("DEVICES %d\n", devCount);

  uint8_t dorder[MAX_DEVICE_ENTRIES];
  uint8_t dn = 0;
  for (uint8_t i = 0; i < MAX_DEVICE_ENTRIES; i++) if (deviceTable[i].used) dorder[dn++] = i;
  for (uint8_t i = 1; i < dn; i++) {
    uint8_t key = dorder[i];
    int8_t keyRssi = deviceTable[key].rssi;
    int j = i - 1;
    while (j >= 0 && deviceTable[dorder[j]].rssi < keyRssi) {
      dorder[j + 1] = dorder[j];
      j--;
    }
    dorder[j + 1] = key;
  }
  uint8_t shownDev = dn < MAX_VISIBLE_DEVICE_ROWS ? dn : MAX_VISIBLE_DEVICE_ROWS;
  tft.setTextColor(TFT_WHITE);
  for (uint8_t k = 0; k < shownDev; k++) {
    DeviceEntry& e = deviceTable[dorder[k]];
    tft.printf(" %s %ddBm ch%u %s\n",
               e.label[0] ? e.label : "(no SSID)", e.rssi, e.channel, lookupVendor(e.mac));
  }
  if (dn > shownDev) {
    tft.setTextColor(TFT_DARKGREY);
    tft.printf(" +%u more\n", dn - shownDev);
  }

  // --- Spectrum: one compact row of mini bars ---
  uint32_t specAge = millis() - spectrumLastSeenMs;
  bool specLive = spectrumLastSeenMs != 0 && specAge < NODE_STALE_MS;
  tft.setTextColor(TFT_CYAN);
  tft.printf("SPECTRUM %s\n", specLive ? "live" : "no data");
  {
    int barTop = tft.getCursorY();
    const int barMaxH = 14;
    const int barWidth = tft.width() / NUM_SPECTRUM_CHANNELS;
    for (uint8_t i = 0; i < NUM_SPECTRUM_CHANNELS; i++) {
      int h = map(channelUtil[i], 0, 255, 0, barMaxH);
      int x = i * barWidth;
      uint16_t color = channelUtil[i] > 170 ? TFT_RED : (channelUtil[i] > 80 ? TFT_YELLOW : TFT_GREEN);
      tft.fillRect(x, barTop + (barMaxH - h), barWidth - 1, h, specLive ? color : TFT_DARKGREY);
    }
    tft.setCursor(0, barTop + barMaxH + 2);
  }
}

// ---------------------------------------------------------------------

static uint32_t lastRefreshMs = 0;

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("Hub console — steps 4.1-4.4");

  pinMode(TFT_BACKLIGHT_PIN, OUTPUT);
  digitalWrite(TFT_BACKLIGHT_PIN, HIGH);
  tft.init();
  tft.setRotation(1);  // landscape, 320x170
  tft.fillScreen(TFT_BLACK);

  memset(wifiTable, 0, sizeof(wifiTable));
  memset(deviceTable, 0, sizeof(deviceTable));
  memset(channelUtil, 0, sizeof(channelUtil));
  memset(nodeHealth, 0, sizeof(nodeHealth));

  // loadSyntheticData();  // disabled for live power test — real ESP-NOW data only

  setupEspNow();         // 4.2

  renderDashboard();
}

void loop() {
  uint32_t now = millis();
  if (now - lastRefreshMs >= REFRESH_INTERVAL_MS) {
    renderDashboard();
    lastRefreshMs = now;
  }
}
