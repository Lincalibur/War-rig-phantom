// Component 4 — Hub Console (ESP32 + attached screen)
//
// Covers README steps 4.1-4.4. Screen model/pin mapping is still TBD, so
// all rendering goes through renderCurrentScreen() below, which currently
// prints to Serial — swap its Serial.print calls for real display draw
// calls once the part is confirmed, nothing else needs to change.
//
// Screen navigation for now (until physical controls are decided) is
// driven over Serial: type 1-5 in the Serial Monitor to switch screens:
//   1 overview   2 wifi   3 ble   4 subghz   5 node health
//
// INITIAL CUT — not yet flashed/tested on real hardware. Boots with
// synthetic/fake sample data per 4.1 so the dashboard/menu system can be
// exercised with no radio link at all; the ESP-NOW (4.2) receiver below
// is wired in but WON'T have live senders until Components 1-3 are
// standalone-complete and 05-integration brings them up one at a time.
// (ESP-NOW-consolidated redesign: the sub-ghz node used to reach this hub
// over UART from an Arduino Uno — see git history pre-esp-now-consolidation
// — it's now a third ESP-NOW sender like the other two nodes, node_id=3.)
//
// Board: any ESP32 dev module. All three field nodes (wifi, ble, sub-ghz)
// report over ESP-NOW now — no UART link to any node, so the earlier
// "needs a second hardware UART" constraint no longer applies.
//
// Needs the shared struct from 05-integration — this include assumes the
// firmware folder stays at its current relative path under the repo.
#include "../../../05-integration/shared/deck_report.h"

#include <WiFi.h>
#include <esp_now.h>

// ---------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------

static const uint8_t  MAX_WIFI_ENTRIES = 32;
static const uint8_t  MAX_BLE_ENTRIES  = 32;
static const uint8_t  MAX_SUBGHZ_ENTRIES = 16;
static const uint32_t NODE_STALE_MS = 60000;   // node considered offline after this long silent
static const uint32_t REFRESH_INTERVAL_MS = 3000;

enum NodeId { NODE_WIFI = 1, NODE_BLE = 2, NODE_SUBGHZ = 3 };

// ---------------------------------------------------------------------
// 4.4 — aggregation store (in-memory; SD-card rolling log is a later add)
// ---------------------------------------------------------------------

struct WifiEntry {
  bool used;
  uint8_t mac[6];
  char label[20];
  int8_t rssi;
  uint8_t channel;
  uint32_t lastSeenMs;
};

struct BleEntry {
  bool used;
  uint8_t mac[6];
  char label[20];
  int8_t rssi;
  bool flagged;
  uint32_t lastSeenMs;
};

struct SubGhzEntry {
  bool used;
  char protocol[16];
  unsigned long code;
  uint16_t pulseLength;
  uint8_t repeats;
  uint32_t lastSeenMs;
};

struct NodeHealth {
  bool everSeen;
  uint32_t lastSeenMs;
  uint32_t totalReports;
};

static WifiEntry   wifiTable[MAX_WIFI_ENTRIES];
static BleEntry     bleTable[MAX_BLE_ENTRIES];
static SubGhzEntry subghzTable[MAX_SUBGHZ_ENTRIES];
static NodeHealth  nodeHealth[4];  // index by NodeId (1-3 used, 0 unused)

static uint16_t trackerAlertCount = 0;

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

static int findBle(const uint8_t* mac) {
  for (int i = 0; i < MAX_BLE_ENTRIES; i++)
    if (bleTable[i].used && memcmp(bleTable[i].mac, mac, 6) == 0) return i;
  return -1;
}
static int allocBle() {
  for (int i = 0; i < MAX_BLE_ENTRIES; i++) if (!bleTable[i].used) return i;
  int oldest = 0;
  for (int i = 1; i < MAX_BLE_ENTRIES; i++)
    if (bleTable[i].lastSeenMs < bleTable[oldest].lastSeenMs) oldest = i;
  return oldest;
}

static int allocSubGhz() {
  for (int i = 0; i < MAX_SUBGHZ_ENTRIES; i++) if (!subghzTable[i].used) return i;
  int oldest = 0;
  for (int i = 1; i < MAX_SUBGHZ_ENTRIES; i++)
    if (subghzTable[i].lastSeenMs < subghzTable[oldest].lastSeenMs) oldest = i;
  return oldest;
}

static void touchNodeHealth(uint8_t nodeId) {
  if (nodeId < 1 || nodeId > 3) return;
  nodeHealth[nodeId].everSeen = true;
  nodeHealth[nodeId].lastSeenMs = millis();
  nodeHealth[nodeId].totalReports++;
}

// ---------------------------------------------------------------------
// 4.2 — ESP-NOW receiver (wifi + ble nodes)
// ---------------------------------------------------------------------

static void applyWifiReport(const deck_report_t& r) {
  int idx = findWifi(r.mac);
  if (idx < 0) idx = allocWifi();
  WifiEntry& e = wifiTable[idx];
  e.used = true;
  memcpy(e.mac, r.mac, 6);
  strncpy(e.label, r.label, sizeof(e.label) - 1);
  e.label[sizeof(e.label) - 1] = '\0';
  e.rssi = r.rssi;
  e.channel = r.channel;
  e.lastSeenMs = millis();
}

static void applyBleReport(const deck_report_t& r) {
  int idx = findBle(r.mac);
  if (idx < 0) idx = allocBle();
  BleEntry& e = bleTable[idx];
  bool wasFlagged = e.used && e.flagged;
  e.used = true;
  memcpy(e.mac, r.mac, 6);
  strncpy(e.label, r.label, sizeof(e.label) - 1);
  e.label[sizeof(e.label) - 1] = '\0';
  e.rssi = r.rssi;
  e.flagged = (r.flags & DECK_REPORT_FLAG_SUSPECT) != 0;
  e.lastSeenMs = millis();
  if (e.flagged && !wasFlagged) trackerAlertCount++;
}

static void applySubGhzReport(const deck_report_t& r) {
  int idx = allocSubGhz();
  SubGhzEntry& e = subghzTable[idx];
  e.used = true;
  strncpy(e.protocol, r.label, sizeof(e.protocol) - 1);
  e.protocol[sizeof(e.protocol) - 1] = '\0';
  e.code = r.subghz_code;
  e.pulseLength = r.subghz_pulse_len;
  e.repeats = r.subghz_repeats;
  e.lastSeenMs = millis();
}

// arduino-esp32 core >= 2.0.x signature. If building against an older
// core, swap this for: void onEspNowRecv(const uint8_t* mac, const
// uint8_t* data, int len)
static void onEspNowRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  if (len != sizeof(deck_report_t)) return;  // malformed/foreign packet, drop
  deck_report_t report;
  memcpy(&report, data, sizeof(report));

  touchNodeHealth(report.node_id);
  switch (report.node_id) {
    case NODE_WIFI:   applyWifiReport(report);   break;
    case NODE_BLE:    applyBleReport(report);    break;
    case NODE_SUBGHZ: applySubGhzReport(report); break;
    default: break;  // unknown node id, ignore
  }
}

static void setupEspNow() {
  WiFi.mode(WIFI_STA);
  delay(100);  // WiFi.macAddress() reads all-zero if queried immediately
               // after mode(STA), before the driver finishes bringing up —
               // confirmed via eFuse-vs-macAddress() diagnostic on real hw.
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

enum Screen { SCREEN_OVERVIEW, SCREEN_WIFI, SCREEN_BLE, SCREEN_SUBGHZ, SCREEN_HEALTH };
static Screen currentScreen = SCREEN_OVERVIEW;

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
  strncpy(wifiTable[1].label, "(hidden)", sizeof(wifiTable[1].label) - 1);
  wifiTable[1].rssi = -71;
  wifiTable[1].channel = 11;
  wifiTable[1].lastSeenMs = millis();

  // A stationary fake BLE device and one flagged fake tracker
  memset(&bleTable[0], 0, sizeof(BleEntry));
  bleTable[0].used = true;
  uint8_t mac3[6] = {0x68, 0xCC, 0x12, 0x49, 0x35, 0x0C};
  memcpy(bleTable[0].mac, mac3, 6);
  strncpy(bleTable[0].label, "phone", sizeof(bleTable[0].label) - 1);
  bleTable[0].rssi = -60;
  bleTable[0].flagged = false;
  bleTable[0].lastSeenMs = millis();

  memset(&bleTable[1], 0, sizeof(BleEntry));
  bleTable[1].used = true;
  uint8_t mac4[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
  memcpy(bleTable[1].mac, mac4, 6);
  strncpy(bleTable[1].label, "unknown", sizeof(bleTable[1].label) - 1);
  bleTable[1].rssi = -45;
  bleTable[1].flagged = true;
  bleTable[1].lastSeenMs = millis();
  trackerAlertCount = 1;

  // A fake sub-ghz capture
  memset(&subghzTable[0], 0, sizeof(SubGhzEntry));
  subghzTable[0].used = true;
  strncpy(subghzTable[0].protocol, "rc-switch:1", sizeof(subghzTable[0].protocol) - 1);
  subghzTable[0].code = 4194305;
  subghzTable[0].pulseLength = 350;
  subghzTable[0].repeats = 3;
  subghzTable[0].lastSeenMs = millis();

  // Pretend all three nodes have reported recently
  for (int i = 1; i <= 3; i++) {
    nodeHealth[i].everSeen = true;
    nodeHealth[i].lastSeenMs = millis();
    nodeHealth[i].totalReports = 1;
  }
}

static const char* nodeName(int id) {
  switch (id) {
    case NODE_WIFI:   return "wifi sniffer";
    case NODE_BLE:    return "ble hunter";
    case NODE_SUBGHZ: return "sub-ghz";
    default:          return "?";
  }
}

static void renderOverview() {
  int wifiCount = 0, bleCount = 0, subghzCount = 0;
  for (int i = 0; i < MAX_WIFI_ENTRIES; i++) if (wifiTable[i].used) wifiCount++;
  for (int i = 0; i < MAX_BLE_ENTRIES; i++) if (bleTable[i].used) bleCount++;
  for (int i = 0; i < MAX_SUBGHZ_ENTRIES; i++) if (subghzTable[i].used) subghzCount++;

  Serial.println("=== OVERVIEW ===");
  Serial.printf("wifi aps/probes: %d   ble devices: %d (flagged: %u)   subghz captures: %d\n",
                wifiCount, bleCount, trackerAlertCount, subghzCount);
}

static void renderWifi() {
  Serial.println("=== WIFI ===");
  for (int i = 0; i < MAX_WIFI_ENTRIES; i++) {
    if (!wifiTable[i].used) continue;
    WifiEntry& e = wifiTable[i];
    Serial.printf("mac=%02x:%02x:%02x:%02x:%02x:%02x  ssid=\"%s\"  rssi=%d  ch=%u\n",
                  e.mac[0], e.mac[1], e.mac[2], e.mac[3], e.mac[4], e.mac[5],
                  e.label, e.rssi, e.channel);
  }
}

static void renderBle() {
  Serial.println("=== BLE ===");
  for (int i = 0; i < MAX_BLE_ENTRIES; i++) {
    if (!bleTable[i].used) continue;
    BleEntry& e = bleTable[i];
    Serial.printf("mac=%02x:%02x:%02x:%02x:%02x:%02x  name=\"%s\"  rssi=%d  %s\n",
                  e.mac[0], e.mac[1], e.mac[2], e.mac[3], e.mac[4], e.mac[5],
                  e.label, e.rssi, e.flagged ? "*** SUSPECT TRACKER ***" : "");
  }
}

static void renderSubGhz() {
  Serial.println("=== SUB-GHZ ===");
  for (int i = 0; i < MAX_SUBGHZ_ENTRIES; i++) {
    if (!subghzTable[i].used) continue;
    SubGhzEntry& e = subghzTable[i];
    Serial.printf("protocol=%s  code=%lu  pulse=%u  repeats=%u\n",
                  e.protocol, e.code, e.pulseLength, e.repeats);
  }
}

static void renderHealth() {
  Serial.println("=== NODE HEALTH ===");
  uint32_t now = millis();
  for (int id = 1; id <= 3; id++) {
    NodeHealth& h = nodeHealth[id];
    if (!h.everSeen) {
      Serial.printf("%-14s never seen\n", nodeName(id));
      continue;
    }
    uint32_t age = now - h.lastSeenMs;
    Serial.printf("%-14s last seen %lus ago  reports=%lu  %s\n",
                  nodeName(id), (unsigned long)(age / 1000),
                  (unsigned long)h.totalReports,
                  age > NODE_STALE_MS ? "*** STALE ***" : "ok");
  }
}

static void renderCurrentScreen() {
  switch (currentScreen) {
    case SCREEN_OVERVIEW: renderOverview(); break;
    case SCREEN_WIFI:     renderWifi();     break;
    case SCREEN_BLE:      renderBle();      break;
    case SCREEN_SUBGHZ:   renderSubGhz();   break;
    case SCREEN_HEALTH:   renderHealth();   break;
  }
  Serial.println("(1=overview 2=wifi 3=ble 4=subghz 5=health)");
}

static void pollScreenSelect() {
  while (Serial.available()) {
    char c = Serial.read();
    switch (c) {
      case '1': currentScreen = SCREEN_OVERVIEW; renderCurrentScreen(); break;
      case '2': currentScreen = SCREEN_WIFI;     renderCurrentScreen(); break;
      case '3': currentScreen = SCREEN_BLE;      renderCurrentScreen(); break;
      case '4': currentScreen = SCREEN_SUBGHZ;   renderCurrentScreen(); break;
      case '5': currentScreen = SCREEN_HEALTH;   renderCurrentScreen(); break;
      default: break;  // ignore newlines etc
    }
  }
}

// ---------------------------------------------------------------------

static uint32_t lastRefreshMs = 0;

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("Hub console — steps 4.1-4.4");

  memset(wifiTable, 0, sizeof(wifiTable));
  memset(bleTable, 0, sizeof(bleTable));
  memset(subghzTable, 0, sizeof(subghzTable));
  memset(nodeHealth, 0, sizeof(nodeHealth));

  loadSyntheticData();   // 4.1: fake data until real links are wired in

  setupEspNow();         // 4.2

  renderCurrentScreen();
}

void loop() {
  pollScreenSelect();

  uint32_t now = millis();
  if (now - lastRefreshMs >= REFRESH_INTERVAL_MS) {
    renderCurrentScreen();
    lastRefreshMs = now;
  }
}
