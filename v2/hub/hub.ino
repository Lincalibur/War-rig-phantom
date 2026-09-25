// CyberDeck v2 — hub (ESP32-S3 + 2.8" ILI9341 + microSD + 5 buttons).
//
// Receives observations from the WiFi node and BLE node over two wired UARTs, keeps de-duplicated
// tables of what is around, shows it on the screen, and logs every geotagged sighting to the SD
// card in WiGLE CSV format (upload-ready) — access points and BLE devices to one file, client
// devices to a second.
//
// Board: ESP32S3 Dev Module — "USB CDC On Boot: Enabled", PSRAM: "OPI PSRAM" (N16R8 board; GPIO33-37 are left unused for it).
// Libraries: Adafruit GFX Library, Adafruit ILI9341 (SD, SPI come with the ESP32 core).
//
// Buttons: LEFT/RIGHT = page, UP/DOWN = scroll, SELECT = change sort.
// Pages:   NETWORKS, BLE, CLIENTS, STATUS.
//
// NOT YET FLASHED. Pin numbers are from docs/PCB-BOM-AND-NETLIST.md — VERIFY against your devkit.

#include <SPI.h>
#include <SD.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>
#include <time.h>
#include "../shared/deck_link.h"

// ---------------------------------------------------------------------------------------
// Pins
// ---------------------------------------------------------------------------------------
static const int PIN_SCLK = 12, PIN_MOSI = 11, PIN_MISO = 13;
static const int PIN_TFT_CS = 10, PIN_TFT_DC = 9, PIN_TFT_RST = 8, PIN_TFT_BL = 7;
static const int PIN_SD_CS = 14;
static const int PIN_CC1101_CS = 15;   // reserved; held high so it doesn't fight the shared bus
static const int PIN_WIFI_RX = 4, PIN_WIFI_TX = 5;   // UART1 <-> WiFi node
static const int PIN_BLE_RX  = 1, PIN_BLE_TX  = 2;   // UART2 <-> BLE node
static const int PIN_BTN_UP = 38, PIN_BTN_DOWN = 39, PIN_BTN_LEFT = 40, PIN_BTN_RIGHT = 41, PIN_BTN_SEL = 42;
static const int PIN_BAT_LOW = 47;     // PowerBoost LBO, active low

// ---------------------------------------------------------------------------------------
// Types (kept above every function: Arduino auto-generates prototypes, and structs used as
// parameters must already be defined where they are inserted)
// ---------------------------------------------------------------------------------------
static const int WIFI_MAX = 600, CLIENT_MAX = 400, BLE_MAX = 400;

struct WifiRec   { bool used; uint8_t mac[6]; int8_t rssi; uint8_t ch, enc, flags; char ssid[33]; uint32_t lastMs; uint16_t hits; };
struct ClientRec { bool used; uint8_t mac[6], ap[6]; int8_t rssi; uint8_t ch, flags; char probe[33]; uint32_t lastMs; };
struct BleRec    { bool used; uint8_t mac[6]; int8_t rssi; uint8_t flags; uint16_t mfg; char name[21]; uint32_t lastMs; uint16_t hits; };

static WifiRec   wifiTab[WIFI_MAX];
static ClientRec clientTab[CLIENT_MAX];
static BleRec    bleTab[BLE_MAX];
static int wifiN = 0, clientN = 0, bleN = 0;

struct Link {
  HardwareSerial* port;
  deck_link_parser_t parser;
  bool     seen;
  uint32_t lastHelloMs;
  uint8_t  kind, fw;
  uint32_t nodeFrames, nodeDropped, nodeUptime;
  uint32_t framesWindow, lastRateMs;
  uint16_t fps;              // link frames/sec, for the status page
  deck_stats_t stats;
  bool     haveStats;
};

// ---------------------------------------------------------------------------------------
// Display
// ---------------------------------------------------------------------------------------
static SPIClass hubSpi(FSPI);
static Adafruit_ILI9341 tft(&hubSpi, PIN_TFT_DC, PIN_TFT_CS, PIN_TFT_RST);

static const uint16_t C_BG = 0x0000, C_FG = 0xFFFF, C_GRAY = 0x8410, C_GREEN = 0x07E0, C_RED = 0xF800,
                      C_YELLOW = 0xFFE0, C_CYAN = 0x07FF, C_ORANGE = 0xFD20, C_HEAD = 0x0010;
static const int ROW_H = 10, COLS = 53, ROWS = 24, LIST_TOP = 3, LIST_ROWS = 20;

static char     rowCache[ROWS][COLS + 1];
static uint16_t rowFg[ROWS], rowBg[ROWS];

static void invalidateRows() {
  for (int r = 0; r < ROWS; r++) { rowCache[r][0] = 1; rowCache[r][1] = 0; }  // impossible content
}

// Redraws a text row only if it changed — keeps the ILI9341 from flickering.
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

// ---------------------------------------------------------------------------------------
// Data model
// ---------------------------------------------------------------------------------------
static Link wifiLink, bleLink;

static deck_geo_t lastGeo;
static uint32_t   lastGeoMs = 0;
static bool       lowBattery = false;

static uint16_t chanSmooth[13];

// ---------------------------------------------------------------------------------------
// SD logging (WiGLE CSV 1.4)
// ---------------------------------------------------------------------------------------
static bool sdOk = false;
static File wigleFile, clientFile;
static char wigleName[40], clientName[40];
static uint32_t wigleRows = 0, clientRows = 0, noFixRows = 0, lastFlushMs = 0;

static size_t appendCsv(char* dst, size_t cap, size_t pos, const char* s, size_t n) {
  bool quote = memchr(s, ',', n) || memchr(s, '"', n);
  if (quote && pos + 1 < cap) dst[pos++] = '"';
  for (size_t i = 0; i < n && pos + 2 < cap; i++) {
    char c = s[i];
    if ((uint8_t)c < 0x20) continue;        // strip control characters
    if (c == '"') dst[pos++] = '"';
    dst[pos++] = c;
  }
  if (quote && pos + 1 < cap) dst[pos++] = '"';
  dst[pos] = '\0';
  return pos;
}

static void fmtMac(char* out, const uint8_t* m) {
  snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
}

static void fmtTime(char* out, size_t cap, uint32_t utc) {
  time_t t = (time_t)utc;
  struct tm tmv;
  gmtime_r(&t, &tmv);
  strftime(out, cap, "%Y-%m-%d %H:%M:%S", &tmv);
}

static const char* wigleAuth(uint8_t enc) {
  switch (enc) {
    case DECK_ENC_OPEN:      return "[ESS]";
    case DECK_ENC_WEP:       return "[WEP][ESS]";
    case DECK_ENC_WPA:       return "[WPA-PSK-TKIP][ESS]";
    case DECK_ENC_WPA2:      return "[WPA2-PSK-CCMP][ESS]";
    case DECK_ENC_WPA3:      return "[WPA3-SAE-CCMP][ESS]";
    case DECK_ENC_WPA2_WPA3: return "[WPA2-PSK-CCMP][WPA3-SAE-CCMP][ESS]";
    case DECK_ENC_WPA2_ENT:  return "[WPA2-EAP-CCMP][ESS]";
    default:                 return "[ESS]";
  }
}

static const char* encName(uint8_t enc) {
  switch (enc) {
    case DECK_ENC_OPEN: return "OPEN";
    case DECK_ENC_WEP:  return "WEP";
    case DECK_ENC_WPA:  return "WPA";
    case DECK_ENC_WPA2: return "WPA2";
    case DECK_ENC_WPA3: return "WPA3";
    case DECK_ENC_WPA2_WPA3: return "WPA23";
    case DECK_ENC_WPA2_ENT:  return "ENT";
    default: return "?";
  }
}

static void openLogs() {
  sdOk = SD.begin(PIN_SD_CS, hubSpi, 20000000);
  if (!sdOk) return;
  SD.mkdir("/wardrive");
  int idx = 1;
  for (; idx < 10000; idx++) {
    snprintf(wigleName, sizeof(wigleName), "/wardrive/wigle_%04d.csv", idx);
    if (!SD.exists(wigleName)) break;
  }
  snprintf(clientName, sizeof(clientName), "/wardrive/clients_%04d.csv", idx);
  wigleFile = SD.open(wigleName, FILE_WRITE);
  clientFile = SD.open(clientName, FILE_WRITE);
  if (!wigleFile || !clientFile) { sdOk = false; return; }
  wigleFile.println("WigleWifi-1.4,appRelease=CyberDeck2,model=CyberDeck,release=2.0,device=CyberDeck,display=ILI9341,board=ESP32-S3,brand=DIY");
  wigleFile.println("MAC,SSID,AuthMode,FirstSeen,Channel,RSSI,CurrentLatitude,CurrentLongitude,AltitudeMeters,AccuracyMeters,Type");
  clientFile.println("MAC,APMAC,ProbedSSID,Time,Channel,RSSI,Latitude,Longitude,Flags");
}

static void closeLogs() {
  if (!sdOk) return;
  wigleFile.flush(); wigleFile.close();
  clientFile.flush(); clientFile.close();
  sdOk = false;
}

static void geoFields(char* buf, size_t cap, size_t* pos, const deck_geo_t& g) {
  uint8_t acc = g.hdop_x10 / 2 < 3 ? 3 : g.hdop_x10 / 2;
  *pos += snprintf(buf + *pos, cap - *pos, "%.7f,%.7f,%d,%u", g.lat_e7 / 1e7, g.lon_e7 / 1e7, g.alt_m, acc);
}

// Only geotagged sightings are written — WiGLE ignores rows without coordinates.
static void logWifi(const deck_wifi_obs_t& o) {
  if (!sdOk || !o.geo.fix) { noFixRows++; return; }
  char line[220], mac[18], ts[24];
  size_t pos = 0;
  fmtMac(mac, o.mac);
  fmtTime(ts, sizeof(ts), o.geo.utc);
  pos += snprintf(line, sizeof(line), "%s,", mac);
  pos = appendCsv(line, sizeof(line), pos, o.ssid, o.ssid_len);
  pos += snprintf(line + pos, sizeof(line) - pos, ",%s,%s,%u,%d,", wigleAuth(o.enc), ts, o.channel, o.rssi);
  geoFields(line, sizeof(line), &pos, o.geo);
  snprintf(line + pos, sizeof(line) - pos, ",WIFI");
  wigleFile.println(line);
  wigleRows++;
}

static void logBle(const deck_ble_obs_t& o) {
  if (!sdOk || !o.geo.fix) { noFixRows++; return; }
  char line[200], mac[18], ts[24];
  size_t pos = 0;
  fmtMac(mac, o.mac);
  fmtTime(ts, sizeof(ts), o.geo.utc);
  pos += snprintf(line, sizeof(line), "%s,", mac);
  pos = appendCsv(line, sizeof(line), pos, o.name, o.name_len);
  pos += snprintf(line + pos, sizeof(line) - pos, ",Misc [LE],%s,0,%d,", ts, o.rssi);
  geoFields(line, sizeof(line), &pos, o.geo);
  snprintf(line + pos, sizeof(line) - pos, ",BLE");
  wigleFile.println(line);
  wigleRows++;
}

static void logClient(const deck_wifi_obs_t& o) {
  if (!sdOk || !o.geo.fix) { noFixRows++; return; }
  char line[200], mac[18], ap[18], ts[24];
  size_t pos = 0;
  fmtMac(mac, o.mac);
  fmtMac(ap, o.ap_mac);
  fmtTime(ts, sizeof(ts), o.geo.utc);
  pos += snprintf(line, sizeof(line), "%s,%s,", mac, ap);
  pos = appendCsv(line, sizeof(line), pos, o.ssid, o.ssid_len);
  snprintf(line + pos, sizeof(line) - pos, ",%s,%u,%d,%.7f,%.7f,%u", ts, o.channel, o.rssi,
           o.geo.lat_e7 / 1e7, o.geo.lon_e7 / 1e7, o.flags);
  clientFile.println(line);
  clientRows++;
}

// ---------------------------------------------------------------------------------------
// Table upserts
// ---------------------------------------------------------------------------------------
static void noteGeo(const deck_geo_t& g) {
  if (g.fix) { lastGeo = g; lastGeoMs = millis(); }
}

// Finds `mac` in a table; if absent, takes a free slot or evicts the stalest one.
template <typename T>
static T* upsert(T* tab, int cap, int* count, const uint8_t* mac, bool* isNew) {
  int free = -1, oldest = 0;
  for (int i = 0; i < cap; i++) {
    if (!tab[i].used) { if (free < 0) free = i; continue; }
    if (memcmp(tab[i].mac, mac, 6) == 0) { *isNew = false; return &tab[i]; }
    if (tab[i].lastMs < tab[oldest].lastMs || !tab[oldest].used) oldest = i;
  }
  *isNew = true;
  int slot = free >= 0 ? free : oldest;
  if (free >= 0) (*count)++;
  memset(&tab[slot], 0, sizeof(T));
  tab[slot].used = true;
  memcpy(tab[slot].mac, mac, 6);
  return &tab[slot];
}

static void onWifiAp(const deck_wifi_obs_t& o) {
  noteGeo(o.geo);
  bool isNew;
  WifiRec* r = upsert(wifiTab, WIFI_MAX, &wifiN, o.mac, &isNew);
  r->rssi = o.rssi; r->ch = o.channel; r->enc = o.enc; r->flags = o.flags;
  if (o.ssid_len > 0) { memcpy(r->ssid, o.ssid, o.ssid_len); r->ssid[o.ssid_len] = '\0'; }
  r->lastMs = millis();
  r->hits++;
  logWifi(o);
}

static void onWifiClient(const deck_wifi_obs_t& o) {
  noteGeo(o.geo);
  bool isNew;
  ClientRec* r = upsert(clientTab, CLIENT_MAX, &clientN, o.mac, &isNew);
  r->rssi = o.rssi; r->ch = o.channel; r->flags = o.flags;
  static const uint8_t zero[6] = {0};
  if (memcmp(o.ap_mac, zero, 6) != 0) memcpy(r->ap, o.ap_mac, 6);
  if (o.ssid_len > 0) { memcpy(r->probe, o.ssid, o.ssid_len); r->probe[o.ssid_len] = '\0'; }
  r->lastMs = millis();
  logClient(o);
}

static void onBle(const deck_ble_obs_t& o) {
  noteGeo(o.geo);
  bool isNew;
  BleRec* r = upsert(bleTab, BLE_MAX, &bleN, o.mac, &isNew);
  r->rssi = o.rssi; r->flags = o.flags; r->mfg = o.mfg_id;
  if (o.name_len > 0) { memcpy(r->name, o.name, o.name_len); r->name[o.name_len] = '\0'; }
  r->lastMs = millis();
  r->hits++;
  logBle(o);
}

// ---------------------------------------------------------------------------------------
// Link handling
// ---------------------------------------------------------------------------------------
static void handleFrame(Link& l, uint8_t type, const uint8_t* p, uint8_t len) {
  l.framesWindow++;
  switch (type) {
    case DECK_MSG_HELLO:
      if (len == sizeof(deck_hello_t)) {
        deck_hello_t h; memcpy(&h, p, sizeof(h));
        l.seen = true; l.lastHelloMs = millis();
        l.kind = h.node_kind; l.fw = h.fw_version;
        l.nodeFrames = h.frames_total; l.nodeDropped = h.tx_dropped; l.nodeUptime = h.uptime_ms;
        noteGeo(h.geo);
      }
      break;
    case DECK_MSG_WIFI_AP:
      if (len == sizeof(deck_wifi_obs_t)) { deck_wifi_obs_t o; memcpy(&o, p, sizeof(o)); onWifiAp(o); }
      break;
    case DECK_MSG_WIFI_CLIENT:
      if (len == sizeof(deck_wifi_obs_t)) { deck_wifi_obs_t o; memcpy(&o, p, sizeof(o)); onWifiClient(o); }
      break;
    case DECK_MSG_BLE:
      if (len == sizeof(deck_ble_obs_t)) { deck_ble_obs_t o; memcpy(&o, p, sizeof(o)); onBle(o); }
      break;
    case DECK_MSG_STATS:
      if (len == sizeof(deck_stats_t)) {
        memcpy(&l.stats, p, sizeof(deck_stats_t));
        l.haveStats = true;
        for (int i = 0; i < 13; i++) chanSmooth[i] = (chanSmooth[i] * 7 + l.stats.chan_frames[i] * 3) / 10;
      }
      break;
  }
}

static void pollLink(Link& l) {
  int budget = 1024;  // bytes per call, so one chatty link can't starve the UI
  while (budget-- > 0 && l.port->available()) {
    if (deck_link_feed(&l.parser, (uint8_t)l.port->read()))
      handleFrame(l, l.parser.type, l.parser.payload, l.parser.len);
  }
  uint32_t now = millis();
  if (now - l.lastRateMs >= 1000) {
    l.fps = l.framesWindow;
    l.framesWindow = 0;
    l.lastRateMs = now;
  }
}

static bool nodeLive(const Link& l) { return l.seen && (millis() - l.lastHelloMs) < 3000; }

// ---------------------------------------------------------------------------------------
// UI
// ---------------------------------------------------------------------------------------
enum Page { PAGE_NETWORKS, PAGE_BLE, PAGE_CLIENTS, PAGE_STATUS, PAGE_COUNT };
enum Sort { SORT_RSSI, SORT_RECENT, SORT_COUNT };
static int page = PAGE_NETWORKS, sortMode = SORT_RSSI, scroll = 0;
static bool dirty = true;
static uint32_t lastRenderMs = 0;

static int idxBuf[WIFI_MAX];   // scratch for sorted views (largest table)

static int cmpWifi(const void* a, const void* b) {
  const WifiRec& x = wifiTab[*(const int*)a]; const WifiRec& y = wifiTab[*(const int*)b];
  return sortMode == SORT_RSSI ? (int)y.rssi - x.rssi : (int)(y.lastMs > x.lastMs) - (int)(y.lastMs < x.lastMs);
}
static int cmpBle(const void* a, const void* b) {
  const BleRec& x = bleTab[*(const int*)a]; const BleRec& y = bleTab[*(const int*)b];
  return sortMode == SORT_RSSI ? (int)y.rssi - x.rssi : (int)(y.lastMs > x.lastMs) - (int)(y.lastMs < x.lastMs);
}
static int cmpClient(const void* a, const void* b) {
  const ClientRec& x = clientTab[*(const int*)a]; const ClientRec& y = clientTab[*(const int*)b];
  return sortMode == SORT_RSSI ? (int)y.rssi - x.rssi : (int)(y.lastMs > x.lastMs) - (int)(y.lastMs < x.lastMs);
}

template <typename T>
static int collect(T* tab, int cap, int (*cmp)(const void*, const void*)) {
  int n = 0;
  for (int i = 0; i < cap; i++) if (tab[i].used) idxBuf[n++] = i;
  qsort(idxBuf, n, sizeof(int), cmp);
  return n;
}

static const char* nodeTag(const Link& l) { return nodeLive(l) ? "+" : (l.seen ? "!" : "-"); }

static void renderHeader() {
  char b[80];
  uint32_t now = millis();
  bool fix = lastGeo.fix && (now - lastGeoMs) < 5000;
  snprintf(b, sizeof(b), "GPS %s %2usat  SD %s  W%s B%s  %s", fix ? "FIX" : "---", lastGeo.sats,
           sdOk ? "ok" : "NO", nodeTag(wifiLink), nodeTag(bleLink), lowBattery ? "LOW BATTERY" : "");
  drawRow(0, b, lowBattery ? C_RED : (fix ? C_GREEN : C_YELLOW), C_HEAD);

  int ap5 = 0, trackers = 0, openN = 0;
  for (int i = 0; i < WIFI_MAX; i++) if (wifiTab[i].used) { if (wifiTab[i].ch > 14) ap5++; if (wifiTab[i].enc == DECK_ENC_OPEN) openN++; }
  for (int i = 0; i < BLE_MAX; i++) if (bleTab[i].used && (bleTab[i].flags & DECK_BLE_FLAG_TRACKER)) trackers++;
  snprintf(b, sizeof(b), "WiFi %d (5G %d, open %d)  BLE %d (trk %d)  Cli %d", wifiN, ap5, openN, bleN, trackers, clientN);
  drawRow(1, b, C_CYAN);
}

static void renderFooter() {
  char b[80];
  static const char* names[] = {"NETWORKS", "BLE", "CLIENTS", "STATUS"};
  snprintf(b, sizeof(b), "<%s> %s  log:%lu rows  nofix:%lu", names[page], sortMode == SORT_RSSI ? "by RSSI" : "recent",
           (unsigned long)wigleRows, (unsigned long)noFixRows);
  drawRow(ROWS - 1, b, C_GRAY, C_HEAD);
}

static void clampScroll(int n) {
  if (scroll > n - LIST_ROWS) scroll = n - LIST_ROWS;
  if (scroll < 0) scroll = 0;
}

static void renderNetworks() {
  int n = collect(wifiTab, WIFI_MAX, cmpWifi);
  clampScroll(n);
  drawRow(2, "RSSI  CH ENC   CLI SSID", C_YELLOW);
  uint32_t now = millis();
  for (int r = 0; r < LIST_ROWS; r++) {
    int row = LIST_TOP + r;
    if (scroll + r >= n) { drawRow(row, ""); continue; }
    const WifiRec& w = wifiTab[idxBuf[scroll + r]];
    int cli = 0;
    for (int i = 0; i < CLIENT_MAX; i++) if (clientTab[i].used && memcmp(clientTab[i].ap, w.mac, 6) == 0) cli++;
    char b[80];
    snprintf(b, sizeof(b), "%4d %3u %-5s %3d %s%s", w.rssi, w.ch, encName(w.enc), cli,
             (w.flags & DECK_WIFI_FLAG_HIDDEN) ? "<hidden>" : "", w.ssid);
    uint16_t col = (now - w.lastMs > 60000) ? C_GRAY : (w.enc == DECK_ENC_OPEN ? C_RED : (w.enc == DECK_ENC_WEP ? C_ORANGE : C_GREEN));
    drawRow(row, b, col);
  }
}

static void renderBle() {
  int n = collect(bleTab, BLE_MAX, cmpBle);
  clampScroll(n);
  drawRow(2, "RSSI MFG  ADDRESS            NAME", C_YELLOW);
  uint32_t now = millis();
  for (int r = 0; r < LIST_ROWS; r++) {
    int row = LIST_TOP + r;
    if (scroll + r >= n) { drawRow(row, ""); continue; }
    const BleRec& d = bleTab[idxBuf[scroll + r]];
    char mac[18], b[80];
    fmtMac(mac, d.mac);
    snprintf(b, sizeof(b), "%4d %04X %s %s%s", d.rssi, d.mfg, mac, (d.flags & DECK_BLE_FLAG_TRACKER) ? "[TRK] " : "", d.name);
    uint16_t col = (d.flags & DECK_BLE_FLAG_TRACKER) ? C_RED : ((now - d.lastMs > 60000) ? C_GRAY : C_GREEN);
    drawRow(row, b, col);
  }
}

static void renderClients() {
  int n = collect(clientTab, CLIENT_MAX, cmpClient);
  clampScroll(n);
  drawRow(2, "RSSI  CH MAC               AP / PROBING", C_YELLOW);
  uint32_t now = millis();
  static const uint8_t zero[6] = {0};
  for (int r = 0; r < LIST_ROWS; r++) {
    int row = LIST_TOP + r;
    if (scroll + r >= n) { drawRow(row, ""); continue; }
    const ClientRec& c = clientTab[idxBuf[scroll + r]];
    char mac[18], b[80];
    fmtMac(mac, c.mac);
    if (memcmp(c.ap, zero, 6) != 0) {
      const char* apName = "";
      for (int i = 0; i < WIFI_MAX; i++) if (wifiTab[i].used && memcmp(wifiTab[i].mac, c.ap, 6) == 0) { apName = wifiTab[i].ssid; break; }
      snprintf(b, sizeof(b), "%4d %3u %s -> %s", c.rssi, c.ch, mac, apName[0] ? apName : "(ap)");
    } else {
      snprintf(b, sizeof(b), "%4d %3u %s ? %s", c.rssi, c.ch, mac, c.probe);
    }
    drawRow(row, b, (now - c.lastMs > 60000) ? C_GRAY : C_GREEN);
  }
}

static void nodeLine(int row, const char* name, const Link& l) {
  char b[80];
  if (!l.seen) snprintf(b, sizeof(b), "%s: no data yet", name);
  else snprintf(b, sizeof(b), "%s: %s up %lus  fw%u  fps %u  seen %lu  drop %lu  crc %lu", name,
                nodeLive(l) ? "LIVE" : "STALE", (unsigned long)(l.nodeUptime / 1000), l.fw, l.fps,
                (unsigned long)l.nodeFrames, (unsigned long)l.nodeDropped, (unsigned long)l.parser.crc_errors);
  drawRow(row, b, nodeLive(l) ? C_GREEN : C_RED);
}

static void renderStatus() {
  drawRow(2, "STATUS", C_YELLOW);
  nodeLine(3, "WiFi", wifiLink);
  nodeLine(4, "BLE ", bleLink);
  char b[80];
  snprintf(b, sizeof(b), "lat %.5f lon %.5f alt %dm hdop %.1f", lastGeo.lat_e7 / 1e7, lastGeo.lon_e7 / 1e7, lastGeo.alt_m, lastGeo.hdop_x10 / 10.0);
  drawRow(5, b);
  snprintf(b, sizeof(b), "SD: %s", sdOk ? wigleName : "not mounted / no card");
  drawRow(6, b, sdOk ? C_FG : C_RED);
  snprintf(b, sizeof(b), "rows: wifi/ble %lu  clients %lu  no-fix skipped %lu", (unsigned long)wigleRows, (unsigned long)clientRows, (unsigned long)noFixRows);
  drawRow(7, b);
  if (wifiLink.haveStats) {
    snprintf(b, sizeof(b), "deauth %lu  disassoc %lu  mgmt %lu  5GHz/2s %u", (unsigned long)wifiLink.stats.deauth,
             (unsigned long)wifiLink.stats.disassoc, (unsigned long)wifiLink.stats.mgmt_frames, wifiLink.stats.five_ghz_frames);
    drawRow(8, b, wifiLink.stats.deauth > 20 ? C_RED : C_FG);
  } else drawRow(8, "");
  drawRow(9, "2.4 GHz channel activity (frames / 2s)", C_YELLOW);

  // Bars: rows 10..22 are cleared and redrawn as graphics.
  const int top = 10 * ROW_H, height = 12 * ROW_H;
  tft.fillRect(0, top, 320, height, C_BG);
  uint16_t maxv = 1;
  for (int i = 0; i < 13; i++) if (chanSmooth[i] > maxv) maxv = chanSmooth[i];
  for (int i = 0; i < 13; i++) {
    int h = (int)((uint32_t)chanSmooth[i] * (height - 24) / maxv);
    int x = 6 + i * 24;
    tft.fillRect(x, top + height - 12 - h, 18, h, (i == 0 || i == 5 || i == 10) ? C_GREEN : C_CYAN);
    tft.setCursor(x + (i < 9 ? 6 : 3), top + height - 9);
    tft.setTextColor(C_GRAY, C_BG);
    tft.print(i + 1);
  }
}

static void render() {
  if (page == PAGE_STATUS) { renderHeader(); renderStatus(); renderFooter(); return; }
  renderHeader();
  if (page == PAGE_NETWORKS) renderNetworks();
  else if (page == PAGE_BLE) renderBle();
  else renderClients();
  renderFooter();
}

static void changePage(int delta) {
  page = (page + delta + PAGE_COUNT) % PAGE_COUNT;
  scroll = 0;
  tft.fillScreen(C_BG);
  invalidateRows();
  dirty = true;
}

// ---------------------------------------------------------------------------------------
// Buttons (active low, internal pull-ups; held UP/DOWN auto-repeats)
// ---------------------------------------------------------------------------------------
struct Button { int pin; bool repeat; uint32_t nextMs; bool wasDown; };
static Button buttons[] = {
  {PIN_BTN_UP, true, 0, false}, {PIN_BTN_DOWN, true, 0, false}, {PIN_BTN_LEFT, false, 0, false},
  {PIN_BTN_RIGHT, false, 0, false}, {PIN_BTN_SEL, false, 0, false},
};

static void onButton(int pin) {
  if (pin == PIN_BTN_UP) scroll -= 1;
  else if (pin == PIN_BTN_DOWN) scroll += 1;
  else if (pin == PIN_BTN_LEFT) { changePage(-1); return; }
  else if (pin == PIN_BTN_RIGHT) { changePage(+1); return; }
  else if (pin == PIN_BTN_SEL) sortMode = (sortMode + 1) % SORT_COUNT;
  if (scroll < 0) scroll = 0;
  dirty = true;
}

static void pollButtons() {
  uint32_t now = millis();
  for (Button& b : buttons) {
    bool down = digitalRead(b.pin) == LOW;
    if (down && !b.wasDown) { onButton(b.pin); b.nextMs = now + 400; }
    else if (down && b.repeat && now >= b.nextMs) { onButton(b.pin); b.nextMs = now + 100; }
    b.wasDown = down;
  }
}

// ---------------------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);

  // Deselect every SPI device before the bus is used.
  for (int cs : {PIN_TFT_CS, PIN_SD_CS, PIN_CC1101_CS}) { pinMode(cs, OUTPUT); digitalWrite(cs, HIGH); }
  pinMode(PIN_TFT_BL, OUTPUT); digitalWrite(PIN_TFT_BL, HIGH);
  for (Button& b : buttons) pinMode(b.pin, INPUT_PULLUP);
  pinMode(PIN_BAT_LOW, INPUT_PULLUP);

  hubSpi.begin(PIN_SCLK, PIN_MISO, PIN_MOSI, -1);
  tft.begin(40000000);
  tft.setRotation(1);   // 320x240 landscape
  tft.fillScreen(C_BG);
  tft.setTextSize(1);
  tft.setTextWrap(false);
  invalidateRows();

  wifiLink.port = &Serial1;
  bleLink.port = &Serial2;
  Serial1.setRxBufferSize(4096);
  Serial2.setRxBufferSize(4096);
  Serial1.begin(DECK_LINK_BAUD, SERIAL_8N1, PIN_WIFI_RX, PIN_WIFI_TX);
  Serial2.begin(DECK_LINK_BAUD, SERIAL_8N1, PIN_BLE_RX, PIN_BLE_TX);

  openLogs();
  Serial.printf("CyberDeck v2 hub — SD %s\n", sdOk ? wigleName : "unavailable");
}

void loop() {
  pollLink(wifiLink);
  pollLink(bleLink);
  pollButtons();

  uint32_t now = millis();

  if (!lowBattery && digitalRead(PIN_BAT_LOW) == LOW) {
    lowBattery = true;
    closeLogs();            // flush and close before the battery cuts out
    dirty = true;
  }
  if (sdOk && now - lastFlushMs >= 5000) {
    lastFlushMs = now;
    wigleFile.flush();
    clientFile.flush();
  }

  if (dirty || now - lastRenderMs >= 500) {
    dirty = false;
    lastRenderMs = now;
    render();
  }
}
