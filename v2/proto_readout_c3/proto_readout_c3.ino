// CyberDeck v2 — bench prototype: C3 WiFi-promiscuous scanner for the big-screen readout
//
// Pairs with v2/proto_readout_uno. WiFi ONLY (no BLE — it crashes the single-radio C3 under
// WiFi coexistence). The C3 runs 802.11 promiscuous mode: it captures beacons/probe-responses
// (access points), probe requests (nearby devices looking for networks) and data frames
// (client <-> AP links), counts deauth/disassoc, and hops channels 1-13. Every ~400 ms it
// streams "draw this row" (R) commands to the UNO, with a status row that changes every send
// so liveness is always visible.
//
// Counter-surveillance angle: the CLIENTS list is sorted by PERSISTENCE (how long a device has
// kept reappearing) — the longest-persisting unknown device near you is the "following me"
// candidate. Phones constantly emit probe requests, so this sees people, not just routers.
//
// Board: ESP32C3 Dev Module, "USB CDC On Boot: Enabled".
// Link:  C3 GPIO21 (Serial1 TX) -> UNO D0 (RX), common GND. 19200 baud.

extern "C" {
  #include "esp_wifi.h"
  #include "esp_event.h"
  #include "esp_wifi_types.h"
  #include "nvs_flash.h"
}
#include <esp_netif.h>

// ---------------------------------------------------------------------------------------
// Link + tunables
// ---------------------------------------------------------------------------------------
static const int      LINK_TX = 21, LINK_RX = 20;
static const uint32_t LINK_BAUD = 19200;
static const int      SCREEN_ROWS = 20, ROW_TEXT_MAX = 34;
static const uint32_t SEND_MS     = 400;    // stream rows to the UNO ~2.5x/sec
static const uint32_t FULLREFRESH_MS = 15000;  // rare full repaints -> no constant 20-row burst
static const uint32_t HOP_MS      = 200;    // channel dwell
static const uint32_t FORGET_MS   = 90000;  // drop an entity not seen this long

enum { COL_W = 0, COL_GRAY, COL_GREEN, COL_YELLOW, COL_RED, COL_CYAN, COL_ORANGE };

// Encryption classes
enum { ENC_OPEN = 0, ENC_WEP, ENC_WPA, ENC_WPA2, ENC_WPA3, ENC_WPA2_WPA3, ENC_ENT };
#define FLAG_HIDDEN 0x01
#define FLAG_RANDOM 0x02

// ---------------------------------------------------------------------------------------
// Types (above all functions: Arduino hoists prototypes to the top of the file)
// ---------------------------------------------------------------------------------------
struct Ap  { bool used; uint8_t bssid[6]; int8_t rssi; uint8_t ch, enc, flags, ssidLen;
             char ssid[33]; uint32_t firstMs, lastMs; };
struct Cli { bool used; uint8_t mac[6], ap[6]; int8_t rssi; uint8_t ch, flags, probeLen;
             char probe[33]; bool haveAp; uint32_t firstMs, lastMs; };

typedef struct __attribute__((packed)) {
  uint8_t frame_ctrl[2], duration[2], addr1[6], addr2[6], addr3[6], seq_ctrl[2];
} wifi_mac_hdr_t;

static const int AP_MAX = 40, CLI_MAX = 80;
static Ap  apTab[AP_MAX];
static Cli cliTab[CLI_MAX];
static int apCount = 0, cliCount = 0;

static volatile uint32_t framesTotal = 0, deauthTotal = 0, mgmtTotal = 0;
static uint8_t  channels[] = {1,2,3,4,5,6,7,8,9,10,11,12,13};
static const int NUM_CH = sizeof(channels);
static uint8_t  curCh = 1;
static int      chIdx = 0;
static uint32_t g_cycles = 0;

// ---------------------------------------------------------------------------------------
// Table upserts (linear, evict stalest when full)
// ---------------------------------------------------------------------------------------
static Ap* apUpsert(const uint8_t* bssid, bool* isNew) {
  int free = -1, oldest = 0;
  for (int i = 0; i < AP_MAX; i++) {
    if (!apTab[i].used) { if (free < 0) free = i; continue; }
    if (memcmp(apTab[i].bssid, bssid, 6) == 0) { *isNew = false; return &apTab[i]; }
    if (apTab[i].lastMs < apTab[oldest].lastMs || !apTab[oldest].used) oldest = i;
  }
  *isNew = true;
  int s = free >= 0 ? free : oldest;
  if (free >= 0) apCount++;
  memset(&apTab[s], 0, sizeof(Ap));
  apTab[s].used = true; memcpy(apTab[s].bssid, bssid, 6); apTab[s].firstMs = millis();
  return &apTab[s];
}
static Cli* cliUpsert(const uint8_t* mac, bool* isNew) {
  int free = -1, oldest = 0;
  for (int i = 0; i < CLI_MAX; i++) {
    if (!cliTab[i].used) { if (free < 0) free = i; continue; }
    if (memcmp(cliTab[i].mac, mac, 6) == 0) { *isNew = false; return &cliTab[i]; }
    if (cliTab[i].lastMs < cliTab[oldest].lastMs || !cliTab[oldest].used) oldest = i;
  }
  *isNew = true;
  int s = free >= 0 ? free : oldest;
  if (free >= 0) cliCount++;
  memset(&cliTab[s], 0, sizeof(Cli));
  cliTab[s].used = true; memcpy(cliTab[s].mac, mac, 6); cliTab[s].firstMs = millis();
  return &cliTab[s];
}
static bool isMulticast(const uint8_t* m) { return m[0] & 0x01; }

// ---------------------------------------------------------------------------------------
// 802.11 tagged-parameter parsing (ported from node_wifi.ino)
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
// Promiscuous callback (WiFi task — keep cheap, never block)
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

  if (tb == 2) {  // data: learn station<->BSSID
    uint8_t toDs = h->frame_ctrl[1] & 0x01, fromDs = h->frame_ctrl[1] & 0x02;
    if (toDs && !fromDs)      noteClient(h->addr2, h->addr1, nullptr, 0, rssi, ch);
    else if (!toDs && fromDs) noteClient(h->addr1, h->addr2, nullptr, 0, rssi, ch);
    return;
  }
  if (tb != 0) return;
  mgmtTotal++;
  switch (st) {
    case 0xC: deauthTotal++; return;
    case 0xA: deauthTotal++; return;
    case 0x8: case 0x5: {  // beacon / probe response
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
    case 0x4: {  // probe request: a nearby device + a network it remembers
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
// Row streaming (shadow + diff)
// ---------------------------------------------------------------------------------------
static char    shadow[SCREEN_ROWS][ROW_TEXT_MAX + 1];
static uint8_t shColor[SCREEN_ROWS];
static bool    forceAll = true;

static void setRow(int r, uint8_t color, const char* text) {
  char buf[ROW_TEXT_MAX + 1];
  snprintf(buf, sizeof(buf), "%.*s", ROW_TEXT_MAX, text);
  if (!forceAll && color == shColor[r] && strcmp(buf, shadow[r]) == 0) return;
  strcpy(shadow[r], buf); shColor[r] = color;
  Serial1.printf("R%02d%u%s\n", r, color, buf);
  delay(150);  // pace >1 full TFT row-draw: the UNO finishes painting a row before the next byte
               // arrives, so its 64-byte RX buffer never overflows (this was the "bad lines" bug)
}

static void ageOut() {
  uint32_t now = millis();
  for (int i = 0; i < AP_MAX; i++)  if (apTab[i].used  && now - apTab[i].lastMs  > FORGET_MS) { apTab[i].used  = false; apCount--;  }
  for (int i = 0; i < CLI_MAX; i++) if (cliTab[i].used && now - cliTab[i].lastMs > FORGET_MS) { cliTab[i].used = false; cliCount--; }
}

static int apSorted(int* idx) {   // by RSSI
  int n = 0; for (int i = 0; i < AP_MAX; i++) if (apTab[i].used) idx[n++] = i;
  for (int i = 1; i < n; i++) { int k = idx[i], j = i - 1;
    while (j >= 0 && apTab[idx[j]].rssi < apTab[k].rssi) { idx[j+1] = idx[j]; j--; } idx[j+1] = k; }
  return n;
}
static int cliSorted(int* idx) {  // by PERSISTENCE (longest-following first)
  int n = 0; for (int i = 0; i < CLI_MAX; i++) if (cliTab[i].used) idx[n++] = i;
  for (int i = 1; i < n; i++) { int k = idx[i], j = i - 1;
    uint32_t pk = cliTab[k].lastMs - cliTab[k].firstMs;
    while (j >= 0 && (cliTab[idx[j]].lastMs - cliTab[idx[j]].firstMs) < pk) { idx[j+1] = idx[j]; j--; }
    idx[j+1] = k; }
  return n;
}

static void composeAndSend() {
  char b[ROW_TEXT_MAX + 1];
  uint32_t now = millis();

  setRow(0, COL_CYAN, "CyberDeck  WiFi RECON");
  snprintf(b, sizeof(b), "ch%2u fr%lu cyc%lu AP%d CL%d dx%lu", curCh,
           (unsigned long)framesTotal, (unsigned long)g_cycles, apCount, cliCount,
           (unsigned long)deauthTotal);
  setRow(1, deauthTotal ? COL_RED : COL_GREEN, b);   // changes every send -> visible liveness

  setRow(2, COL_YELLOW, "APs  rssi ch enc   ssid");
  int idx[CLI_MAX], n = apSorted(idx);
  for (int r = 0; r < 8; r++) {
    if (r >= n) { setRow(3 + r, COL_GRAY, ""); continue; }
    const Ap& a = apTab[idx[r]];
    snprintf(b, sizeof(b), "%4d %2u %-5s %s", a.rssi, a.ch, encShort(a.enc),
             (a.flags & FLAG_HIDDEN) ? "<hidden>" : a.ssid);
    uint8_t col = a.enc == ENC_OPEN ? COL_RED : (a.enc == ENC_WEP ? COL_ORANGE : COL_GREEN);
    setRow(3 + r, col, b);
  }

  setRow(11, COL_YELLOW, "CLIENTS rssi seen mac    ->/probe");
  n = cliSorted(idx);
  for (int r = 0; r < 8; r++) {
    if (r >= n) { setRow(12 + r, COL_GRAY, ""); continue; }
    const Cli& c = cliTab[idx[r]];
    uint32_t secs = (c.lastMs - c.firstMs) / 1000;
    char tail[20];
    if (c.probeLen)   snprintf(tail, sizeof(tail), "?%s", c.probe);
    else if (c.haveAp) snprintf(tail, sizeof(tail), ">%02x%02x%02x", c.ap[3], c.ap[4], c.ap[5]);
    else              tail[0] = 0;
    snprintf(b, sizeof(b), "%4d %3lus %02x%02x%02x%s %s", c.rssi, (unsigned long)secs,
             c.mac[3], c.mac[4], c.mac[5], (c.flags & FLAG_RANDOM) ? "~" : "", tail);
    uint8_t col = (now - c.lastMs > 20000) ? COL_GRAY : (secs > 60 ? COL_ORANGE : COL_GREEN);
    setRow(12 + r, col, b);
  }
  forceAll = false;
}

// ---------------------------------------------------------------------------------------
static void hop() {
  for (int t = 0; t < NUM_CH; t++) {
    chIdx = (chIdx + 1) % NUM_CH;
    if (esp_wifi_set_channel(channels[chIdx], WIFI_SECOND_CHAN_NONE) == ESP_OK) { curCh = channels[chIdx]; return; }
  }
}

void setup() {
  Serial.begin(115200);
  Serial1.begin(LINK_BAUD, SERIAL_8N1, LINK_RX, LINK_TX);
  memset(apTab, 0, sizeof(apTab));
  memset(cliTab, 0, sizeof(cliTab));

  nvs_flash_init();
  esp_netif_init();
  esp_event_loop_create_default();
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  esp_wifi_init(&cfg);
  esp_wifi_set_storage(WIFI_STORAGE_RAM);
  esp_wifi_set_mode(WIFI_MODE_STA);
  esp_wifi_start();
  wifi_promiscuous_filter_t filt = { .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA };
  esp_wifi_set_promiscuous_filter(&filt);
  esp_wifi_set_promiscuous_rx_cb(&onPacket);
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(channels[0], WIFI_SECOND_CHAN_NONE);
  Serial.println("C3 WiFi recon up");
}

void loop() {
  static uint32_t lastHop = 0, lastSend = 0, lastFull = 0;
  uint32_t now = millis();

  if (now - lastHop >= HOP_MS) { lastHop = now; hop(); }

  if (now - lastSend >= SEND_MS) {
    lastSend = now;
    g_cycles++;
    ageOut();
    if (now - lastFull >= FULLREFRESH_MS) { forceAll = true; lastFull = now; }
    composeAndSend();
    Serial.printf("ch=%u frames=%lu aps=%d clients=%d deauth=%lu\n", curCh,
                  (unsigned long)framesTotal, apCount, cliCount, (unsigned long)deauthTotal);
  }
  delay(3);
}
