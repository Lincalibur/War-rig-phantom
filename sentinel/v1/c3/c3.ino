// SENTINEL - passive counter-surveillance watcher (ESP32-C3 brain)
//
// Standalone, always-on, NEVER transmits. No phone, no BLE control service. It time-slices the one
// C3 radio between a WiFi promiscuous window and a passive BLE scan window, and from those two
// captures it answers three questions on the UNO screen + LED:
//   TRACKERS  - AirTag / offline-finding / Flipper / skimmer BLE tags that keep following you
//   TAILS     - WiFi clients (non-randomized MAC) that reappear around you over time
//   ATTACKS   - deauth/disassoc floods and evil-twin (same-SSID/different-BSSID) APs nearby
//
// Single radio => WiFi promiscuous and BLE scan can't RX at the same instant, so we round-robin:
//   [WIFI ~8s, hop ch1..13]  ->  [BLE ~4s, passive scan]  ->  repeat
// Both stacks stay initialized the whole run; each slice only toggles promiscuous on/off and
// scan start/stop (no per-cycle NimBLE init/deinit - that is the heap-fragmenting path).
//
// BLE scan is PASSIVE (setActiveScan(false)) so SENTINEL emits nothing. Consequence: names that live
// only in a scan RESPONSE (some Flipper/skimmer modules) may be missed; AirTag/offline-finding detection
// uses the ADV manufacturer data and is unaffected.
//
// Link: C3 GPIO21 (Serial1 TX) -> UNO D0 (RX), common GND, 19200 baud. Same wiring as the orchestrator.
// Build: arduino-cli compile --fqbn "esp32:esp32:esp32c3:PartitionScheme=min_spiffs,CDCOnBoot=cdc" sentinel/v1/c3
// See docs/watcher-plan.md.

extern "C" {
  #include "esp_wifi.h"
  #include "esp_event.h"
  #include "esp_wifi_types.h"
  #include "nvs_flash.h"
}
#include <esp_netif.h>
#include <esp_wifi_default.h>
#include <NimBLEDevice.h>

// ---------------------------------------------------------------------------------------
// Link + tunables
// ---------------------------------------------------------------------------------------
static const int      LINK_TX = 21, LINK_RX = 20;
static const uint32_t LINK_BAUD = 19200;
static const int      BOOT_BTN  = 9;        // BOOT strap pin, readable as input after boot
static const int      SCREEN_ROWS = 19, ROW_TEXT_MAX = 38;
static const uint32_t SEND_MS       = 700;     // recompute + stream to the UNO ~1.4x/sec
static const uint32_t FULLREFRESH_MS = 15000;

// Round-robin windows (single-radio time-slice)
static const uint32_t WIFI_WIN_MS = 8000;      // promiscuous: feeds TAILS + ATTACKS
static const uint32_t BLE_WIN_MS  = 4000;      // passive scan: feeds TRACKERS
static const uint32_t HOP_MS      = 200;       // channel hop cadence inside the WiFi window

// Memory / aging
static const uint32_t FORGET_MS = 300000;      // 5 min: brief disappearance must NOT reset persistence

// Detection thresholds (tune on the bench - see docs/watcher-plan.md)
static const uint32_t TRACKER_FOLLOW_SWEEPS = 3;     // BLE tag seen in >= this many sweeps ...
static const uint32_t TAIL_FOLLOW_SWEEPS    = 4;     // WiFi client seen in >= this many sweeps ...
static const uint32_t FOLLOW_MIN_MS         = 60000; // ... AND present at least this long  => follow
static const uint32_t DEAUTH_ALERT_RATE     = 20;    // deauth/disassoc frames per 10s => flood alert

enum { COL_W = 0, COL_GRAY, COL_GREEN, COL_YELLOW, COL_RED, COL_CYAN, COL_ORANGE };

// Encryption classes
enum { ENC_OPEN = 0, ENC_WEP, ENC_WPA, ENC_WPA2, ENC_WPA3, ENC_WPA2_WPA3, ENC_ENT };
#define FLAG_HIDDEN 0x01
#define FLAG_RANDOM 0x02

// ---------------------------------------------------------------------------------------
// Types  (enums/structs used as fn params defined up top - Arduino auto-prototype gotcha)
// ---------------------------------------------------------------------------------------
enum BleKind { BK_PLAIN = 0, BK_AIRTAG, BK_FLIPPER, BK_SKIMMER };

struct Ap  { bool used; uint8_t bssid[6]; int8_t rssi; uint8_t ch, enc, flags, ssidLen;
             char ssid[33]; uint32_t firstMs, lastMs, sweeps; bool evilTwin; };
struct Cli { bool used; uint8_t mac[6], ap[6]; int8_t rssi; uint8_t ch, flags, probeLen;
             char probe[33]; bool haveAp; uint32_t firstMs, lastMs, sweeps; };
struct BleDev { bool used; char key[18]; int8_t rssi; uint8_t kind; char name[16];
                uint32_t firstMs, lastMs, sweeps; };

typedef struct __attribute__((packed)) {
  uint8_t frame_ctrl[2], duration[2], addr1[6], addr2[6], addr3[6], seq_ctrl[2];
} wifi_mac_hdr_t;

static const int AP_MAX = 40, CLI_MAX = 80, BLE_MAX = 24;
static Ap     apTab[AP_MAX];
static Cli    cliTab[CLI_MAX];
static BleDev bleTab[BLE_MAX];
static int apCount = 0, cliCount = 0, bleCount = 0;

static volatile uint32_t framesTotal = 0, deauthTotal = 0, mgmtTotal = 0;
static uint32_t bleSeen = 0;
static uint8_t  channels[] = {1,2,3,4,5,6,7,8,9,10,11,12,13};
static const int NUM_CH = sizeof(channels);
static uint8_t  curCh = 1;
static int      chIdx = 0;

// Detection state (recomputed each refresh)
static uint32_t deauthRate10s = 0;      // deauth/disassoc frames per 10s (from the last WiFi window)
static int      evilTwinCount = 0;      // # of distinct SSIDs claimed by >1 BSSID
static bool     alertDeauth=false, alertEvil=false, alertTracker=false, alertTail=false;

// Round-robin state
enum Phase { PH_WIFI, PH_BLE };
static Phase    phase = PH_WIFI;
static uint32_t phaseStart = 0;
static uint32_t lastHop = 0;

// ---------------------------------------------------------------------------------------
// Link helpers (to the UNO)  R<rr><c><text> | C clear | L<g><r> leds
// ---------------------------------------------------------------------------------------
static char    shadow[SCREEN_ROWS][ROW_TEXT_MAX + 1];
static uint8_t shColor[SCREEN_ROWS];
static bool    forceAll = true;

static void linkClear() { Serial1.print("C\n"); delay(20); for (int i=0;i<SCREEN_ROWS;i++){ shadow[i][0]=0; shColor[i]=0xFF; } forceAll = true; }
static void linkLeds(bool green, bool red) { Serial1.printf("L%d%d\n", green ? 1 : 0, red ? 1 : 0); delay(5); }

static void setRow(int r, uint8_t color, const char* text) {
  char buf[ROW_TEXT_MAX + 1];
  snprintf(buf, sizeof(buf), "%.*s", ROW_TEXT_MAX, text);
  if (!forceAll && color == shColor[r] && strcmp(buf, shadow[r]) == 0) return;
  strcpy(shadow[r], buf); shColor[r] = color;
  Serial1.printf("R%02d%u%s\n", r, color, buf);
  delay(150);  // pace > one full TFT row-draw so the UNO's 64-byte RX buffer never overflows
}

// ---------------------------------------------------------------------------------------
// Table upserts (LRU-evict oldest when full)
// ---------------------------------------------------------------------------------------
static Ap* apUpsert(const uint8_t* bssid, bool* isNew) {
  int freeI = -1, oldest = 0;
  for (int i = 0; i < AP_MAX; i++) {
    if (!apTab[i].used) { if (freeI < 0) freeI = i; continue; }
    if (memcmp(apTab[i].bssid, bssid, 6) == 0) { *isNew = false; return &apTab[i]; }
    if (apTab[i].lastMs < apTab[oldest].lastMs || !apTab[oldest].used) oldest = i;
  }
  *isNew = true;
  int s = freeI >= 0 ? freeI : oldest;
  if (freeI >= 0) apCount++;
  memset(&apTab[s], 0, sizeof(Ap));
  apTab[s].used = true; memcpy(apTab[s].bssid, bssid, 6); apTab[s].firstMs = millis();
  return &apTab[s];
}
static Cli* cliUpsert(const uint8_t* mac, bool* isNew) {
  int freeI = -1, oldest = 0;
  for (int i = 0; i < CLI_MAX; i++) {
    if (!cliTab[i].used) { if (freeI < 0) freeI = i; continue; }
    if (memcmp(cliTab[i].mac, mac, 6) == 0) { *isNew = false; return &cliTab[i]; }
    if (cliTab[i].lastMs < cliTab[oldest].lastMs || !cliTab[oldest].used) oldest = i;
  }
  *isNew = true;
  int s = freeI >= 0 ? freeI : oldest;
  if (freeI >= 0) cliCount++;
  memset(&cliTab[s], 0, sizeof(Cli));
  cliTab[s].used = true; memcpy(cliTab[s].mac, mac, 6); cliTab[s].firstMs = millis();
  return &cliTab[s];
}
static bool isMulticast(const uint8_t* m) { return m[0] & 0x01; }

// ---------------------------------------------------------------------------------------
// 802.11 tagged-parameter parsing
// ---------------------------------------------------------------------------------------
static bool parseSSID(const uint8_t* body, int len, const uint8_t** ssid, uint8_t* out) {
  int i = 0;
  while (i + 2 <= len) {
    uint8_t tag = body[i], l = body[i + 1];
    if (i + 2 + l > len) break;
    if (tag == 0) { *ssid = &body[i + 2]; *out = l > 32 ? 32 : l; return true; }
    i += 2 + l;
  }
  return false;
}
static uint8_t parseDsChannel(const uint8_t* body, int len) {
  int i = 0;
  while (i + 2 <= len) {
    uint8_t tag = body[i], l = body[i + 1];
    if (i + 2 + l > len) break;
    if (tag == 3 && l == 1) return body[i + 2];
    i += 2 + l;
  }
  return 0;
}
static uint8_t parseEncryption(const uint8_t* body, int len, uint16_t cap) {
  if ((cap & 0x0010) == 0) return ENC_OPEN;
  bool rsn = false, wpa1 = false, sae = false, psk = false, ent = false;
  int i = 0;
  while (i + 2 <= len) {
    uint8_t tag = body[i], l = body[i + 1];
    if (i + 2 + l > len) break;
    int start = i + 2, end = i + 2 + l;
    if (tag == 48) {
      rsn = true;
      int off = start + 2 + 4;
      if (off + 2 <= end) { uint16_t pw = body[off] | (body[off+1] << 8); off += 2 + (int)pw * 4; }
      if (off + 2 <= end) {
        uint16_t akm = body[off] | (body[off+1] << 8); off += 2;
        for (uint16_t k = 0; k < akm && off + 4 <= end; k++, off += 4) {
          uint8_t t = body[off + 3];
          if (t == 8 || t == 9) sae = true;
          else if (t == 2 || t == 4 || t == 6) psk = true;
          else if (t == 1 || t == 3 || t == 5 || t == 11 || t == 12) ent = true;
        }
      }
    } else if (tag == 221 && l >= 4 && body[start]==0x00 && body[start+1]==0x50 &&
               body[start+2]==0xF2 && body[start+3]==0x01) { wpa1 = true; }
    i += 2 + l;
  }
  if (rsn) {
    if (ent && !psk && !sae) return ENC_ENT;
    if (sae && psk) return ENC_WPA2_WPA3;
    if (sae) return ENC_WPA3;
    return ENC_WPA2;
  }
  return wpa1 ? ENC_WPA : ENC_WEP;
}
static const char* encShort(uint8_t e) {
  switch (e) { case ENC_OPEN: return "OPEN"; case ENC_WEP: return "WEP"; case ENC_WPA: return "WPA";
    case ENC_WPA2: return "WPA2"; case ENC_WPA3: return "WPA3"; case ENC_WPA2_WPA3: return "WPA23";
    case ENC_ENT: return "ENT"; default: return "?"; }
}

// ---------------------------------------------------------------------------------------
// Promiscuous callback (WiFi task - keep cheap). Catalogue APs + clients; count deauth/disassoc.
// ---------------------------------------------------------------------------------------
static void noteAp(const uint8_t* bssid, const uint8_t* ssid, uint8_t ssidLen, bool hidden,
                   int8_t rssi, uint8_t ch, uint8_t enc) {
  bool isNew; Ap* a = apUpsert(bssid, &isNew);
  a->rssi = rssi; a->ch = ch; a->enc = enc;
  a->flags = hidden ? FLAG_HIDDEN : 0;
  if (ssidLen) { memcpy(a->ssid, ssid, ssidLen); a->ssid[ssidLen] = 0; a->ssidLen = ssidLen; }
  a->lastMs = millis();
}
static void noteClient(const uint8_t* mac, const uint8_t* ap, const uint8_t* probe,
                       uint8_t probeLen, int8_t rssi, uint8_t ch) {
  if (isMulticast(mac)) return;
  bool isNew; Cli* c = cliUpsert(mac, &isNew);
  c->rssi = rssi; c->ch = ch;
  c->flags = (mac[0] & 0x02) ? FLAG_RANDOM : 0;
  if (ap) { memcpy(c->ap, ap, 6); c->haveAp = true; }
  if (probeLen) { memcpy(c->probe, probe, probeLen); c->probe[probeLen] = 0; c->probeLen = probeLen; }
  c->lastMs = millis();
}
static void onPacket(void* buf, wifi_promiscuous_pkt_type_t type) {
  if (type != WIFI_PKT_MGMT && type != WIFI_PKT_DATA) return;
  const wifi_promiscuous_pkt_t* pkt = (const wifi_promiscuous_pkt_t*)buf;
  const uint8_t* p = pkt->payload;
  int len = pkt->rx_ctrl.sig_len;
  if (len < (int)sizeof(wifi_mac_hdr_t)) return;
  const wifi_mac_hdr_t* h = (const wifi_mac_hdr_t*)p;
  uint8_t tb = (h->frame_ctrl[0] >> 2) & 0x3, st = (h->frame_ctrl[0] >> 4) & 0xF;
  int8_t rssi = pkt->rx_ctrl.rssi; uint8_t ch = pkt->rx_ctrl.channel;
  framesTotal++;

  if (tb == 2) {  // data: learn station <-> BSSID association
    uint8_t toDs = h->frame_ctrl[1] & 0x01, fromDs = h->frame_ctrl[1] & 0x02;
    if (toDs && !fromDs)      noteClient(h->addr2, h->addr1, nullptr, 0, rssi, ch);
    else if (!toDs && fromDs) noteClient(h->addr1, h->addr2, nullptr, 0, rssi, ch);
    return;
  }
  if (tb != 0) return;
  mgmtTotal++;
  switch (st) {
    case 0xC: deauthTotal++; return;   // deauth
    case 0xA: deauthTotal++; return;   // disassoc
    case 0x8: case 0x5: {              // beacon / probe response
      const uint8_t* fixed = p + sizeof(wifi_mac_hdr_t);
      const uint8_t* body = fixed + 12;
      int bl = len - (int)sizeof(wifi_mac_hdr_t) - 12;
      if (bl <= 0) return;
      uint16_t cap = fixed[10] | (fixed[11] << 8);
      const uint8_t* ssid = nullptr; uint8_t sl = 0;
      bool has = parseSSID(body, bl, &ssid, &sl);
      bool hidden = has && (sl == 0 || ssid[0] == 0);
      if (hidden) sl = 0;
      uint8_t dc = parseDsChannel(body, bl);
      noteAp(h->addr3, ssid, sl, hidden, rssi, dc ? dc : ch, parseEncryption(body, bl, cap));
      break;
    }
    case 0x4: {                        // probe request (from a client)
      const uint8_t* body = p + sizeof(wifi_mac_hdr_t);
      int bl = len - (int)sizeof(wifi_mac_hdr_t);
      const uint8_t* ssid = nullptr; uint8_t sl = 0;
      if (bl > 0) parseSSID(body, bl, &ssid, &sl);
      noteClient(h->addr2, nullptr, ssid, sl, rssi, ch);
      break;
    }
  }
}

// ---------------------------------------------------------------------------------------
// WiFi radio lifecycle (init once, keep started, toggle promiscuous per window)
// ---------------------------------------------------------------------------------------
static bool wifiInited = false, promiscOn = false;
static void wifiInitOnce() {
  if (wifiInited) return;
  nvs_flash_init();
  esp_netif_init();
  esp_event_loop_create_default();
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  esp_wifi_init(&cfg);
  esp_wifi_set_storage(WIFI_STORAGE_RAM);
  esp_wifi_set_mode(WIFI_MODE_STA);
  esp_wifi_start();
  delay(100);                                 // let the MAC/radio settle before first channel set
  wifi_promiscuous_filter_t filt = { .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA };
  esp_wifi_set_promiscuous_filter(&filt);
  esp_wifi_set_promiscuous_rx_cb(&onPacket);
  wifiInited = true;
}
static void promiscStart() {
  if (promiscOn) return;
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(channels[chIdx], WIFI_SECOND_CHAN_NONE);
  curCh = channels[chIdx];
  promiscOn = true;
}
static void promiscStop() {
  if (!promiscOn) return;
  esp_wifi_set_promiscuous(false);
  promiscOn = false;
}
static void hop() {
  if (!promiscOn) return;
  for (int t = 0; t < NUM_CH; t++) {
    chIdx = (chIdx + 1) % NUM_CH;
    if (esp_wifi_set_channel(channels[chIdx], WIFI_SECOND_CHAN_NONE) == ESP_OK) { curCh = channels[chIdx]; return; }
  }
}

// ---------------------------------------------------------------------------------------
// BLE passive scan (init once; start/stop per window). Classify tracker-class devices.
// ---------------------------------------------------------------------------------------
static bool bleScanning = false;

static bool ciContains(const char* hay, const char* needle) {
  size_t nl = strlen(needle);
  for (const char* q = hay; *q; q++) if (strncasecmp(q, needle, nl) == 0) return true;
  return false;
}
static bool payloadMfr(const std::vector<uint8_t>& pl, const uint8_t** out, int* outLen) {
  size_t i = 0;
  while (i + 1 < pl.size()) {
    uint8_t len = pl[i];
    if (len == 0 || i + 1 + len > pl.size()) break;
    if (pl[i + 1] == 0xFF && len >= 3) { *out = &pl[i + 2]; *outLen = len - 1; return true; }
    i += 1 + len;
  }
  return false;
}
static BleKind classifyBle(const NimBLEAdvertisedDevice* d, const char* name) {
  std::vector<uint8_t> pl = d->getPayload();
  const uint8_t* m; int ml;
  if (payloadMfr(pl, &m, &ml) && ml >= 3 && m[0] == 0x4C && m[1] == 0x00) {
    if (m[2] == 0x12) return BK_AIRTAG;                  // Apple offline-finding / AirTag
  }
  if (name[0]) {
    if (ciContains(name, "flipper")) return BK_FLIPPER;
    if (ciContains(name, "HC-05") || ciContains(name, "HC-06") || ciContains(name, "HC-03") ||
        ciContains(name, "AT-09") || ciContains(name, "MLT-BT05") || ciContains(name, "JDY") ||
        ciContains(name, "BT05")  || ciContains(name, "skim"))
      return BK_SKIMMER;
  }
  return BK_PLAIN;
}
static void bleOnAdv(const NimBLEAdvertisedDevice* d) {
  bleSeen++;
  char name[16] = {0};
  std::string nm = d->getName();
  if (!nm.empty()) snprintf(name, sizeof(name), "%s", nm.c_str());
  BleKind k = classifyBle(d, name);
  char key[18]; snprintf(key, sizeof(key), "%s", d->getAddress().toString().c_str());
  int freeI = -1, oldest = 0;
  for (int i = 0; i < BLE_MAX; i++) {
    if (!bleTab[i].used) { if (freeI < 0) freeI = i; continue; }
    if (strcmp(bleTab[i].key, key) == 0) {
      bleTab[i].rssi = d->getRSSI(); bleTab[i].lastMs = millis();
      if (k != BK_PLAIN) bleTab[i].kind = k;              // upgrade classification if we learn more
      if (name[0]) snprintf(bleTab[i].name, sizeof(bleTab[i].name), "%s", name);
      return;
    }
    if (bleTab[i].lastMs < bleTab[oldest].lastMs || !bleTab[oldest].used) oldest = i;
  }
  int s = freeI >= 0 ? freeI : oldest;
  if (freeI >= 0) bleCount++;
  memset(&bleTab[s], 0, sizeof(BleDev));
  bleTab[s].used = true; bleTab[s].rssi = d->getRSSI(); bleTab[s].kind = k;
  bleTab[s].firstMs = bleTab[s].lastMs = millis();
  snprintf(bleTab[s].key, sizeof(bleTab[s].key), "%s", key);
  if (name[0]) snprintf(bleTab[s].name, sizeof(bleTab[s].name), "%s", name);
}
class ScanCB : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* d) override { bleOnAdv(d); }
};
static ScanCB scanCb;
static void bleScanStart() {
  NimBLEScan* s = NimBLEDevice::getScan();
  s->setScanCallbacks(&scanCb, false);
  s->setActiveScan(false);                                // PASSIVE - SENTINEL transmits nothing
  s->setInterval(80); s->setWindow(80);                   // ~100% duty passive listening
  s->start(0, false, true);                               // 0 = until stopped
  bleScanning = true;
}
static void bleScanStop() {
  if (!bleScanning) return;
  NimBLEDevice::getScan()->stop();
  bleScanning = false;
}

// ---------------------------------------------------------------------------------------
// Sweep accounting + aging
//   At the end of each window, any device updated since the window opened gets sweeps++.
// ---------------------------------------------------------------------------------------
static void creditWifiSweep(uint32_t winOpen) {
  for (int i = 0; i < AP_MAX; i++)  if (apTab[i].used  && apTab[i].lastMs  >= winOpen) apTab[i].sweeps++;
  for (int i = 0; i < CLI_MAX; i++) if (cliTab[i].used && cliTab[i].lastMs >= winOpen) cliTab[i].sweeps++;
}
static void creditBleSweep(uint32_t winOpen) {
  for (int i = 0; i < BLE_MAX; i++) if (bleTab[i].used && bleTab[i].lastMs >= winOpen) bleTab[i].sweeps++;
}
static void ageOut() {
  uint32_t now = millis();
  for (int i = 0; i < AP_MAX; i++)  if (apTab[i].used  && now - apTab[i].lastMs  > FORGET_MS) { apTab[i].used  = false; apCount--;  }
  for (int i = 0; i < CLI_MAX; i++) if (cliTab[i].used && now - cliTab[i].lastMs > FORGET_MS) { cliTab[i].used = false; cliCount--; }
  for (int i = 0; i < BLE_MAX; i++) if (bleTab[i].used && now - bleTab[i].lastMs > FORGET_MS) { bleTab[i].used = false; bleCount--; }
}

// ---------------------------------------------------------------------------------------
// Detection / sorting helpers
// ---------------------------------------------------------------------------------------
static uint32_t lifeMs(uint32_t firstMs, uint32_t lastMs) { return lastMs - firstMs; }
static bool bleFollowing(const BleDev& b) {
  return b.kind != BK_PLAIN && b.sweeps >= TRACKER_FOLLOW_SWEEPS && lifeMs(b.firstMs, b.lastMs) >= FOLLOW_MIN_MS;
}
static bool cliTailing(const Cli& c) {
  return !(c.flags & FLAG_RANDOM) && c.sweeps >= TAIL_FOLLOW_SWEEPS && lifeMs(c.firstMs, c.lastMs) >= FOLLOW_MIN_MS;
}

// trackers first by "following", then sweeps, then RSSI
static int bleSorted(int* idx) {
  int n = 0; for (int i = 0; i < BLE_MAX; i++) if (bleTab[i].used && bleTab[i].kind != BK_PLAIN) idx[n++] = i;
  for (int i = 1; i < n; i++) { int k = idx[i], j = i - 1;
    while (j >= 0) {
      const BleDev& a = bleTab[idx[j]]; const BleDev& b = bleTab[k];
      bool af = bleFollowing(a), bf = bleFollowing(b);
      bool swap = (bf && !af) || (bf == af && a.sweeps < b.sweeps);
      if (!swap) break;
      idx[j+1] = idx[j]; j--;
    }
    idx[j+1] = k; }
  return n;
}
// tails: non-random clients by sweeps then lifespan (most persistent first)
static int cliSorted(int* idx) {
  int n = 0; for (int i = 0; i < CLI_MAX; i++) if (cliTab[i].used) idx[n++] = i;
  for (int i = 1; i < n; i++) { int k = idx[i], j = i - 1;
    while (j >= 0) {
      const Cli& a = cliTab[idx[j]]; const Cli& b = cliTab[k];
      uint32_t as = a.sweeps, bs = b.sweeps;
      bool swap = (as < bs) || (as == bs && lifeMs(a.firstMs,a.lastMs) < lifeMs(b.firstMs,b.lastMs));
      if (!swap) break;
      idx[j+1] = idx[j]; j--;
    }
    idx[j+1] = k; }
  return n;
}
// Mark evil-twins: same SSID advertised by >1 distinct BSSID. Returns # of colliding SSIDs.
static int markEvilTwins() {
  for (int i = 0; i < AP_MAX; i++) if (apTab[i].used) apTab[i].evilTwin = false;
  int collisions = 0;
  for (int i = 0; i < AP_MAX; i++) {
    if (!apTab[i].used || apTab[i].evilTwin || apTab[i].ssidLen == 0) continue;
    int matches = 0;
    for (int j = 0; j < AP_MAX; j++) {
      if (!apTab[j].used || apTab[j].ssidLen == 0) continue;
      if (strcmp(apTab[i].ssid, apTab[j].ssid) == 0 && memcmp(apTab[i].bssid, apTab[j].bssid, 6) != 0)
        matches++;
    }
    if (matches > 0) {
      collisions++;
      for (int j = 0; j < AP_MAX; j++)
        if (apTab[j].used && strcmp(apTab[i].ssid, apTab[j].ssid) == 0) apTab[j].evilTwin = true;
    }
  }
  return collisions;
}

// ---------------------------------------------------------------------------------------
// Screen composer (streams the three sections to the UNO)
// ---------------------------------------------------------------------------------------
static const char* bleKindTag(uint8_t k) {
  switch (k) { case BK_AIRTAG: return "AirTag"; case BK_FLIPPER: return "Flippr";
               case BK_SKIMMER: return "Skimmr"; default: return "dev"; }
}

static void recomputeDetections(uint32_t lastWifiDeauth, uint32_t lastWifiMs) {
  // deauth rate per 10s from the most recent completed WiFi window
  if (lastWifiMs > 0) deauthRate10s = (uint32_t)((uint64_t)lastWifiDeauth * 10000UL / lastWifiMs);
  evilTwinCount = markEvilTwins();

  alertDeauth = deauthRate10s >= DEAUTH_ALERT_RATE;
  alertEvil   = evilTwinCount > 0;
  alertTracker = false;
  for (int i = 0; i < BLE_MAX; i++) if (bleTab[i].used && bleFollowing(bleTab[i])) alertTracker = true;
  alertTail = false;
  for (int i = 0; i < CLI_MAX; i++) if (cliTab[i].used && cliTailing(cliTab[i])) alertTail = true;
}

static void compose() {
  char b[ROW_TEXT_MAX + 1];
  uint32_t now = millis();
  bool anyAlert = alertDeauth || alertEvil || alertTracker || alertTail;

  // Header
  snprintf(b, sizeof(b), "SENTINEL %s ch%-2u up%lus", phase == PH_WIFI ? "WIFI" : "BLE ",
           curCh, (unsigned long)(now / 1000));
  setRow(0, COL_CYAN, b);

  // Alert banner (worst first)
  if (alertDeauth)      { snprintf(b, sizeof(b), "! DEAUTH FLOOD  %lu/10s", (unsigned long)deauthRate10s); setRow(1, COL_RED, b); }
  else if (alertEvil)   { snprintf(b, sizeof(b), "! EVIL-TWIN AP  %d ssid", evilTwinCount);                setRow(1, COL_RED, b); }
  else if (alertTracker){ setRow(1, COL_RED, "! TRACKER FOLLOWING YOU"); }
  else if (alertTail)   { setRow(1, COL_RED, "! DEVICE TAILING YOU"); }
  else                  { setRow(1, COL_GREEN, "all clear - watching"); }

  // TRACKERS
  setRow(2, COL_YELLOW, "TRACKERS  kind   rssi swp age");
  int idx[CLI_MAX], n = bleSorted(idx);
  for (int r = 0; r < 4; r++) {
    if (r >= n) { setRow(3 + r, COL_GRAY, ""); continue; }
    const BleDev& d = bleTab[idx[r]];
    uint32_t age = lifeMs(d.firstMs, d.lastMs) / 1000;
    snprintf(b, sizeof(b), "%-6s %-7.7s %4d %3lu %lus", bleKindTag(d.kind),
             d.name[0] ? d.name : d.key + 12, d.rssi, (unsigned long)d.sweeps, (unsigned long)age);
    setRow(3 + r, bleFollowing(d) ? COL_RED : COL_GREEN, b);
  }

  // TAILS
  setRow(7, COL_YELLOW, "TAILS rssi swp age  mac    tag");
  n = cliSorted(idx);
  int shown = 0;
  for (int i = 0; i < n && shown < 5; i++) {
    const Cli& c = cliTab[idx[i]];
    uint32_t age = lifeMs(c.firstMs, c.lastMs) / 1000;
    char tail[16];
    if (c.probeLen)    snprintf(tail, sizeof(tail), "?%s", c.probe);
    else if (c.haveAp) snprintf(tail, sizeof(tail), ">%02x%02x%02x", c.ap[3], c.ap[4], c.ap[5]);
    else               tail[0] = 0;
    snprintf(b, sizeof(b), "%4d %3lu %3lus %02x%02x%02x%s %s", c.rssi, (unsigned long)c.sweeps,
             (unsigned long)age, c.mac[3], c.mac[4], c.mac[5], (c.flags & FLAG_RANDOM) ? "~" : "", tail);
    uint8_t col = cliTailing(c) ? COL_RED : ((c.flags & FLAG_RANDOM) ? COL_GRAY
                 : (now - c.lastMs > 30000 ? COL_GRAY : COL_GREEN));
    setRow(8 + shown, col, b);
    shown++;
  }
  for (int r = shown; r < 5; r++) setRow(8 + r, COL_GRAY, "");

  // ATTACKS
  snprintf(b, sizeof(b), "ATTACKS deauth%lu/10s twin%d", (unsigned long)deauthRate10s, evilTwinCount);
  setRow(13, COL_YELLOW, b);
  int er = 0;
  for (int i = 0; i < AP_MAX && er < 3; i++) {
    if (!apTab[i].used || !apTab[i].evilTwin) continue;
    snprintf(b, sizeof(b), "twin %02x%02x%02x %-5s %s", apTab[i].bssid[3], apTab[i].bssid[4],
             apTab[i].bssid[5], encShort(apTab[i].enc), apTab[i].ssidLen ? apTab[i].ssid : "<hidden>");
    setRow(14 + er, COL_ORANGE, b);
    er++;
  }
  for (int r = er; r < 3; r++) setRow(14 + r, COL_GRAY, "");

  // Stats footer
  snprintf(b, sizeof(b), "fr%lu AP%d CL%d BLE%d/%lu", (unsigned long)framesTotal, apCount, cliCount,
           bleCount, (unsigned long)bleSeen);
  setRow(18, COL_GREEN, b);

  linkLeds(true, anyAlert);   // green = alive, red = any active alert
  forceAll = false;
}

// ---------------------------------------------------------------------------------------
// Round-robin radio scheduler
// ---------------------------------------------------------------------------------------
static uint32_t lastWifiDeauthDelta = 0, lastWifiElapsed = 0, wifiWinDeauthStart = 0;

static void enterWifi() {
  bleScanStop();
  phase = PH_WIFI; phaseStart = millis(); lastHop = phaseStart;
  wifiWinDeauthStart = deauthTotal;
  promiscStart();
}
static void enterBle() {
  promiscStop();
  phase = PH_BLE; phaseStart = millis();
  bleScanStart();
}

// ---------------------------------------------------------------------------------------
// Setup / loop
// ---------------------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  Serial1.begin(LINK_BAUD, SERIAL_8N1, LINK_RX, LINK_TX);
  pinMode(BOOT_BTN, INPUT_PULLUP);
  delay(400);
  linkClear();
  setRow(0, COL_CYAN, "SENTINEL booting...");

  wifiInitOnce();
  NimBLEDevice::init("");      // no name - we never advertise, scan only

  linkClear();
  enterWifi();
}

void loop() {
  uint32_t now = millis();

  // --- radio scheduler: advance the round-robin window ---
  if (phase == PH_WIFI) {
    if (now - lastHop >= HOP_MS) { lastHop = now; hop(); }
    if (now - phaseStart >= WIFI_WIN_MS) {
      lastWifiDeauthDelta = deauthTotal - wifiWinDeauthStart;
      lastWifiElapsed     = now - phaseStart;
      creditWifiSweep(phaseStart);
      enterBle();
    }
  } else { // PH_BLE
    if (now - phaseStart >= BLE_WIN_MS) {
      creditBleSweep(phaseStart);
      enterWifi();
    }
  }

  // --- BOOT button: clear all tables / counters for a fresh baseline ---
  static uint32_t btnDown = 0;
  if (digitalRead(BOOT_BTN) == LOW) { if (!btnDown) btnDown = now; }
  else if (btnDown) {
    btnDown = 0;
    memset(apTab, 0, sizeof(apTab)); memset(cliTab, 0, sizeof(cliTab)); memset(bleTab, 0, sizeof(bleTab));
    apCount = cliCount = bleCount = 0; framesTotal = deauthTotal = mgmtTotal = bleSeen = 0;
    linkClear();
  }

  // --- periodic recompute + stream to the UNO ---
  static uint32_t lastSend = 0, lastFull = 0;
  if (now - lastSend >= SEND_MS) {
    lastSend = now;
    ageOut();
    recomputeDetections(lastWifiDeauthDelta, lastWifiElapsed);
    if (now - lastFull >= FULLREFRESH_MS) { lastFull = now; forceAll = true; }
    compose();
  }
}
