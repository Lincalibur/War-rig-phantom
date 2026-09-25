// CyberDeck v2 — WiFi node (XIAO ESP32-C5 preferred; a C6 works as a 2.4 GHz-only fallback).
//
// Passive 802.11 sniffer. Channel-hops 2.4 GHz (and 5 GHz on the C5), and reports over the wired
// UART link to the hub:
//   - access points        (beacons / probe responses): SSID, BSSID, channel, encryption, RSSI
//   - client devices       (probe requests): MAC + the SSID they are probing for
//   - client associations  (data-frame headers): which station talks to which BSSID
//   - deauth/disassoc counts and per-channel activity
// Every observation carries the GPS position/time. It never transmits RF.
//
// Board:  ESP32-C5 Dev Module (or XIAO_ESP32C5) — "USB CDC On Boot: Enabled".
//         Fallback: XIAO_ESP32C6 (2.4 GHz only, channel list adapts automatically).
//
// NOT YET FLASHED. Pin numbers below are from docs/PCB-BOM-AND-NETLIST.md — VERIFY them for
// your exact XIAO variant before wiring.

extern "C" {
  #include "esp_wifi.h"
  #include "esp_event.h"
  #include "esp_wifi_types.h"
  #include "nvs_flash.h"
}
#include <esp_netif.h>

#define DECK_NODE_KIND    DECK_NODE_WIFI
#define DECK_FW_VERSION   1
// XIAO pin labels (defined by the XIAO_ESP32C5/C6 board variants, so one PCB footprint fits both):
// D7 = RX from hub, D6 = TX to hub, D2 = GPS TX in.
#define DECK_LINK_RX_PIN  D7
#define DECK_LINK_TX_PIN  D6
#define DECK_GPS_RX_PIN   D2
#include "../shared/deck_node_core.h"

// ---------------------------------------------------------------------------------------
// Tunables
// ---------------------------------------------------------------------------------------
static const uint32_t DWELL_24_MS   = 150;   // beacons go out every ~102 ms, so >=1 beacon per dwell
static const uint32_t DWELL_5_MS    = 110;
static const int8_t   RSSI_IMPROVE_DB      = 6;      // re-report a stronger sighting (better WiGLE position)
static const uint32_t REREPORT_MS          = 30000;  // and re-report anything still visible every 30 s (route trace)
static const uint32_t REPORT_MIN_GAP_MS    = 1500;   // per-entity rate limit
static const uint32_t STATS_INTERVAL_MS    = 2000;

// Channel plan. 5 GHz only on the C5; set_channel() failures (region-disallowed channels) are skipped.
#if defined(CONFIG_IDF_TARGET_ESP32C5)
static const uint8_t CHANNELS[] = {
  1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13,
  36, 40, 44, 48, 52, 56, 60, 64,
  100, 104, 108, 112, 116, 120, 124, 128, 132, 136, 140, 144,
  149, 153, 157, 161, 165
};
#else
static const uint8_t CHANNELS[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13};
#endif
static const size_t NUM_CHANNELS = sizeof(CHANNELS);

// ---------------------------------------------------------------------------------------
// Entity table: open-addressed hash keyed on MAC. When it gets ~80% full it is cleared and
// everything simply re-reports as "new" (the hub dedups; the cost is a few repeated log rows).
// ---------------------------------------------------------------------------------------
static const uint16_t TABLE_SIZE = 512;
static const uint16_t TABLE_RESET_AT = 400;

enum { KIND_AP = 1, KIND_CLIENT = 2 };

struct Entity {
  bool     used;
  uint8_t  kind;
  uint8_t  mac[6];
  uint8_t  ap[6];        // client: last BSSID it talked to
  int8_t   bestRssi;
  uint8_t  enc;
  uint8_t  flags;
  uint8_t  channel;
  uint8_t  ssidLen;
  char     ssid[32];
  uint32_t lastReportMs;
};

static Entity   table[TABLE_SIZE];
static uint16_t tableCount = 0;
static uint16_t apCount = 0, clientCount = 0;

static volatile uint32_t mgmtFrames = 0, deauthCount = 0, disassocCount = 0;
static volatile uint16_t chanFrames[13];
static volatile uint16_t fiveGhzFrames = 0;
static uint8_t  currentChannel = 1;
static size_t   chanIdx = 0;
static uint32_t lastHopMs = 0, lastStatsMs = 0;

typedef struct __attribute__((packed)) {
  uint8_t frame_ctrl[2];
  uint8_t duration[2];
  uint8_t addr1[6];
  uint8_t addr2[6];
  uint8_t addr3[6];
  uint8_t seq_ctrl[2];
} wifi_mac_hdr_t;

static uint16_t hashMac(const uint8_t* mac) {
  uint32_t h = ((uint32_t)mac[3] << 16) | ((uint32_t)mac[4] << 8) | mac[5];
  h ^= (uint32_t)mac[2] << 8 ^ mac[1];
  return (uint16_t)((h * 2654435761UL) >> 23) & (TABLE_SIZE - 1);
}

// Returns the slot for `mac`, or a free slot to insert into (isNew=true).
static Entity* lookup(const uint8_t* mac, bool* isNew) {
  if (tableCount >= TABLE_RESET_AT) {
    memset(table, 0, sizeof(table));
    tableCount = apCount = clientCount = 0;
  }
  uint16_t i = hashMac(mac);
  for (uint16_t probe = 0; probe < TABLE_SIZE; probe++, i = (i + 1) & (TABLE_SIZE - 1)) {
    if (!table[i].used) { *isNew = true; return &table[i]; }
    if (memcmp(table[i].mac, mac, 6) == 0) { *isNew = false; return &table[i]; }
  }
  *isNew = true;
  return &table[0];  // unreachable given the reset threshold
}

static bool isMulticast(const uint8_t* mac) { return mac[0] & 0x01; }

// ---------------------------------------------------------------------------------------
// Reporting
// ---------------------------------------------------------------------------------------
static void report(Entity& e) {
  deck_wifi_obs_t o;
  memset(&o, 0, sizeof(o));
  memcpy(o.mac, e.mac, 6);
  memcpy(o.ap_mac, e.ap, 6);
  o.rssi = e.bestRssi;
  o.channel = e.channel;
  o.enc = e.enc;
  o.flags = e.flags | ((e.mac[0] & 0x02) ? DECK_WIFI_FLAG_RANDOM_MAC : 0);
  o.ssid_len = e.ssidLen;
  memcpy(o.ssid, e.ssid, e.ssidLen);
  deck_geo_get(&o.geo);
  deck_node_send(e.kind == KIND_AP ? DECK_MSG_WIFI_AP : DECK_MSG_WIFI_CLIENT, &o, sizeof(o));
  e.lastReportMs = millis();
}

static bool dueForReport(const Entity& e, bool isNew, bool infoChanged, int8_t rssi) {
  uint32_t now = millis();
  if (isNew || infoChanged) return true;
  if (now - e.lastReportMs < REPORT_MIN_GAP_MS) return false;
  if (rssi >= e.bestRssi + RSSI_IMPROVE_DB) return true;
  return (now - e.lastReportMs) >= REREPORT_MS;
}

static void noteAp(const uint8_t* bssid, const char* ssid, uint8_t ssidLen, bool hidden,
                   int8_t rssi, uint8_t channel, uint8_t enc) {
  bool isNew;
  Entity* e = lookup(bssid, &isNew);
  bool infoChanged = false;
  if (isNew) {
    memset(e, 0, sizeof(*e));
    e->used = true;
    e->kind = KIND_AP;
    memcpy(e->mac, bssid, 6);
    e->bestRssi = rssi;
    tableCount++;
    apCount++;
  } else if (e->kind != KIND_AP) {
    return;  // a station MAC that also beaconed; leave the original classification alone
  }
  if (ssidLen > 0 && (e->ssidLen != ssidLen || memcmp(e->ssid, ssid, ssidLen) != 0)) {
    memcpy(e->ssid, ssid, ssidLen);
    e->ssidLen = ssidLen;
    infoChanged = !isNew;
  }
  uint8_t flags = hidden && ssidLen == 0 ? DECK_WIFI_FLAG_HIDDEN : 0;
  if (e->enc != enc || e->flags != flags || e->channel != channel) infoChanged = infoChanged || !isNew;
  e->enc = enc;
  e->flags = flags;
  e->channel = channel;
  bool due = dueForReport(*e, isNew, infoChanged, rssi);
  if (rssi > e->bestRssi) e->bestRssi = rssi;
  if (due) report(*e);
}

static void noteClient(const uint8_t* mac, const uint8_t* ap, const char* probeSsid, uint8_t ssidLen,
                       int8_t rssi, uint8_t channel) {
  if (isMulticast(mac)) return;
  bool isNew;
  Entity* e = lookup(mac, &isNew);
  bool infoChanged = false;
  if (isNew) {
    memset(e, 0, sizeof(*e));
    e->used = true;
    e->kind = KIND_CLIENT;
    memcpy(e->mac, mac, 6);
    e->bestRssi = rssi;
    tableCount++;
    clientCount++;
  } else if (e->kind != KIND_CLIENT) {
    return;  // this MAC is a known AP
  }
  if (ap && memcmp(e->ap, ap, 6) != 0) { memcpy(e->ap, ap, 6); infoChanged = true; }
  if (ssidLen > 0 && (e->ssidLen != ssidLen || memcmp(e->ssid, probeSsid, ssidLen) != 0)) {
    memcpy(e->ssid, probeSsid, ssidLen);
    e->ssidLen = ssidLen;
    infoChanged = true;
  }
  e->channel = channel;
  bool due = dueForReport(*e, isNew, infoChanged, rssi);
  if (rssi > e->bestRssi) e->bestRssi = rssi;
  if (due) report(*e);
}

// ---------------------------------------------------------------------------------------
// 802.11 parsing (tagged parameters)
// ---------------------------------------------------------------------------------------
// Finds tag 0 (SSID). Returns true if the tag exists; *len may be 0 (hidden network).
static bool parseSSID(const uint8_t* body, int bodyLen, const uint8_t** ssid, uint8_t* len) {
  int i = 0;
  while (i + 2 <= bodyLen) {
    uint8_t tag = body[i], l = body[i + 1];
    if (i + 2 + l > bodyLen) break;
    if (tag == 0) {
      *ssid = &body[i + 2];
      *len = l > 32 ? 32 : l;
      return true;
    }
    i += 2 + l;
  }
  return false;
}

// Tag 3 (DS Parameter Set) carries the AP's real 2.4 GHz channel. Without it, an AP heard on an
// adjacent channel would be logged on the wrong one. Returns 0 if absent (5 GHz beacons).
static uint8_t parseDsChannel(const uint8_t* body, int bodyLen) {
  int i = 0;
  while (i + 2 <= bodyLen) {
    uint8_t tag = body[i], l = body[i + 1];
    if (i + 2 + l > bodyLen) break;
    if (tag == 3 && l == 1) return body[i + 2];
    i += 2 + l;
  }
  return 0;
}

// Capability privacy bit says "some cipher"; RSN (tag 48) AKM list distinguishes PSK / SAE /
// enterprise; the Microsoft vendor IE (tag 221, 00:50:F2 type 1) marks legacy WPA1.
static uint8_t parseEncryption(const uint8_t* body, int bodyLen, uint16_t capInfo) {
  if ((capInfo & 0x0010) == 0) return DECK_ENC_OPEN;
  bool hasRSN = false, hasWPA1 = false, sae = false, psk = false, ent = false;
  int i = 0;
  while (i + 2 <= bodyLen) {
    uint8_t tag = body[i], len = body[i + 1];
    if (i + 2 + len > bodyLen) break;
    int start = i + 2, end = i + 2 + len;
    if (tag == 48) {
      hasRSN = true;
      int off = start + 2 + 4;  // version(2) + group cipher(4)
      if (off + 2 <= end) {
        uint16_t pairwise = body[off] | (body[off + 1] << 8);
        off += 2 + (int)pairwise * 4;
      }
      if (off + 2 <= end) {
        uint16_t akmCount = body[off] | (body[off + 1] << 8);
        off += 2;
        for (uint16_t k = 0; k < akmCount && off + 4 <= end; k++, off += 4) {
          uint8_t t = body[off + 3];  // OUI(3) + suite type(1)
          if (t == 8 || t == 9) sae = true;
          else if (t == 2 || t == 4 || t == 6) psk = true;
          else if (t == 1 || t == 3 || t == 5 || t == 11 || t == 12) ent = true;
        }
      }
    } else if (tag == 221 && len >= 4 && body[start] == 0x00 && body[start + 1] == 0x50 &&
               body[start + 2] == 0xF2 && body[start + 3] == 0x01) {
      hasWPA1 = true;
    }
    i += 2 + len;
  }
  if (hasRSN) {
    if (ent && !psk && !sae) return DECK_ENC_WPA2_ENT;
    if (sae && psk) return DECK_ENC_WPA2_WPA3;
    if (sae) return DECK_ENC_WPA3;
    return DECK_ENC_WPA2;
  }
  return hasWPA1 ? DECK_ENC_WPA : DECK_ENC_WEP;
}

// ---------------------------------------------------------------------------------------
// Promiscuous callback (runs in the WiFi task — keep it cheap, never block)
// ---------------------------------------------------------------------------------------
static void onPacket(void* buf, wifi_promiscuous_pkt_type_t type) {
  if (type != WIFI_PKT_MGMT && type != WIFI_PKT_DATA) return;
  const wifi_promiscuous_pkt_t* pkt = (const wifi_promiscuous_pkt_t*)buf;
  const uint8_t* payload = pkt->payload;
  int len = pkt->rx_ctrl.sig_len;
  if (len < (int)sizeof(wifi_mac_hdr_t)) return;

  const wifi_mac_hdr_t* hdr = (const wifi_mac_hdr_t*)payload;
  uint8_t typeBits = (hdr->frame_ctrl[0] >> 2) & 0x3;
  uint8_t subtype  = (hdr->frame_ctrl[0] >> 4) & 0xF;
  int8_t  rssi     = pkt->rx_ctrl.rssi;
  uint8_t channel  = pkt->rx_ctrl.channel;

  deckFramesTotal++;
  if (channel >= 1 && channel <= 13) chanFrames[channel - 1]++;
  else fiveGhzFrames++;

  if (typeBits == 2) {  // data frame: learn station<->BSSID associations from the address fields
    uint8_t toDs = hdr->frame_ctrl[1] & 0x01, fromDs = hdr->frame_ctrl[1] & 0x02;
    if (toDs && !fromDs)      noteClient(hdr->addr2, hdr->addr1, nullptr, 0, rssi, channel);  // STA -> AP
    else if (!toDs && fromDs) noteClient(hdr->addr1, hdr->addr2, nullptr, 0, rssi, channel);  // AP -> STA
    return;
  }
  if (typeBits != 0) return;

  mgmtFrames++;
  switch (subtype) {
    case 0xC: deauthCount++; return;
    case 0xA: disassocCount++; return;
    case 0x8:    // beacon
    case 0x5: {  // probe response
      const uint8_t* fixed = payload + sizeof(wifi_mac_hdr_t);
      const uint8_t* body = fixed + 12;  // timestamp(8) + interval(2) + capability(2)
      int bodyLen = len - (int)sizeof(wifi_mac_hdr_t) - 12;
      if (bodyLen <= 0) return;
      uint16_t cap = fixed[10] | (fixed[11] << 8);
      const uint8_t* ssid = nullptr;
      uint8_t ssidLen = 0;
      bool hasTag = parseSSID(body, bodyLen, &ssid, &ssidLen);
      // Hidden networks beacon an empty (or all-zero) SSID.
      bool hidden = hasTag && (ssidLen == 0 || ssid[0] == 0);
      if (hidden) ssidLen = 0;
      uint8_t apChannel = parseDsChannel(body, bodyLen);
      noteAp(hdr->addr3, (const char*)ssid, ssidLen, hidden, rssi, apChannel ? apChannel : channel,
             parseEncryption(body, bodyLen, cap));
      break;
    }
    case 0x4: {  // probe request — reveals a nearby device and a network it remembers
      const uint8_t* body = payload + sizeof(wifi_mac_hdr_t);
      int bodyLen = len - (int)sizeof(wifi_mac_hdr_t);
      const uint8_t* ssid = nullptr;
      uint8_t ssidLen = 0;
      if (bodyLen > 0) parseSSID(body, bodyLen, &ssid, &ssidLen);
      noteClient(hdr->addr2, nullptr, (const char*)ssid, ssidLen, rssi, channel);
      break;
    }
    default: break;
  }
}

// ---------------------------------------------------------------------------------------
static void hop() {
  for (size_t tries = 0; tries < NUM_CHANNELS; tries++) {
    chanIdx = (chanIdx + 1) % NUM_CHANNELS;
    if (esp_wifi_set_channel(CHANNELS[chanIdx], WIFI_SECOND_CHAN_NONE) == ESP_OK) {
      currentChannel = CHANNELS[chanIdx];
      return;
    }
  }
}

static void sendStats() {
  deck_stats_t s;
  memset(&s, 0, sizeof(s));
  s.node_kind = DECK_NODE_KIND;
  s.mgmt_frames = mgmtFrames;
  s.deauth = deauthCount;
  s.disassoc = disassocCount;
  s.ap_count = apCount;
  s.client_count = clientCount;
  for (int i = 0; i < 13; i++) { s.chan_frames[i] = chanFrames[i]; chanFrames[i] = 0; }
  s.five_ghz_frames = fiveGhzFrames;
  fiveGhzFrames = 0;
  deck_node_send(DECK_MSG_STATS, &s, sizeof(s));
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("CyberDeck v2 WiFi node");

  memset(table, 0, sizeof(table));
  deck_node_begin();

  nvs_flash_init();
  esp_netif_init();
  esp_event_loop_create_default();
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  esp_wifi_init(&cfg);
  esp_wifi_set_storage(WIFI_STORAGE_RAM);
  esp_wifi_set_mode(WIFI_MODE_STA);  // never connect; STA is just the state promiscuous mode needs
#if defined(CONFIG_IDF_TARGET_ESP32C5)
  esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO);  // VERIFY: name/availability depends on the IDF version
#endif
  esp_wifi_start();

  wifi_promiscuous_filter_t filt = { .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA };
  esp_wifi_set_promiscuous_filter(&filt);
  esp_wifi_set_promiscuous_rx_cb(&onPacket);
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(CHANNELS[0], WIFI_SECOND_CHAN_NONE);
  lastHopMs = millis();
}

void loop() {
  uint32_t now = millis();
  uint32_t dwell = currentChannel <= 14 ? DWELL_24_MS : DWELL_5_MS;
  if (now - lastHopMs >= dwell) {
    hop();
    lastHopMs = now;
  }
  if (now - lastStatsMs >= STATS_INTERVAL_MS) {
    lastStatsMs = now;
    sendStats();
    deck_geo_t g;
    deck_geo_get(&g);
    Serial.printf("ch=%u frames=%lu aps=%u clients=%u gps=%s sats=%u dropped=%lu\n", currentChannel,
                  (unsigned long)deckFramesTotal, apCount, clientCount, g.fix ? "fix" : "none", g.sats,
                  (unsigned long)deckTxDropped);
  }
  deck_node_poll();
  delay(2);
}
