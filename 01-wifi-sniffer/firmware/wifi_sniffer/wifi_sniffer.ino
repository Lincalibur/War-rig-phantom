// Component 1 — WiFi Sniffer Node (ESP32-C3 #1)
//
// Passive 802.11 management-frame sniffer, covering README steps 1.1-1.6
// in one pass: promiscuous mode init, non-blocking channel hopper,
// management-frame-only filter, header/SSID parsing, a bounded dedup
// table keyed on BSSID (also used for probe-req source MACs), and a
// deauth/disassoc counter. Step 1.7 (probe-req validation) is just a
// field test against this same code, not separate logic.
//
// No display on this node. Reports each newly-discovered AP/prober to the
// hub over ESP-NOW broadcast (deck_report_t, node_id=1) as well as
// printing the full local summary table to Serial (see
// ../../../05-integration).
//
// INITIAL CUT — not yet flashed/tested on real hardware. Frame-parsing
// offsets below are standard 802.11 layout but should be sanity-checked
// against real captured beacons/probes once this is on the board.
//
// Toolchain note: promiscuous RX needs the raw esp_wifi/IDF API, not the
// Arduino WiFi.h wrapper. This still builds as a normal .ino via Arduino
// IDE / arduino-cli with the esp32 board package — arduino-esp32 exposes
// the IDF headers directly, no need for a bare ESP-IDF project.
//
// Board: ESP32C3 Dev Module (Tools > Board > esp32 > ESP32C3 Dev Module)
// Same "USB CDC On Boot" = Enabled gotcha as component 2's board (see
// that .ino) — otherwise Serial goes to the UART pins instead of USB:
//   --fqbn 'esp32:esp32:esp32c3:CDCOnBoot=cdc'
//
// STATUS_LED_PIN is a guess (GPIO8 is the onboard LED on many C3 dev
// boards) — confirm/adjust once the board is in hand.

extern "C" {
  #include "esp_wifi.h"
  #include "esp_event.h"
  #include "esp_wifi_types.h"
  #include "nvs_flash.h"
}
#include <esp_netif.h>
#include <WiFi.h>
#include <esp_now.h>
#include "../../../05-integration/shared/deck_report.h"

static const uint8_t STATUS_LED_PIN = 8;

static const uint8_t BROADCAST_MAC[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

static const uint32_t CHANNEL_HOP_MS = 300;   // 1.2
static const uint8_t  CHANNEL_MIN = 1;
static const uint8_t  CHANNEL_MAX = 13;

static const uint8_t  MAX_ENTITIES = 64;      // 1.5, fixed table, no heap growth
static const uint32_t SUMMARY_INTERVAL_MS = 15000;

// Management frame subtypes (low nibble of frame-control byte 0)
enum {
  SUBTYPE_PROBE_REQ  = 0x4,
  SUBTYPE_PROBE_RESP = 0x5,
  SUBTYPE_BEACON     = 0x8,
  SUBTYPE_DISASSOC   = 0xA,
  SUBTYPE_DEAUTH     = 0xC,
};

struct EntityRecord {
  bool     used;
  uint8_t  mac[6];       // BSSID for beacon/probe-resp, src MAC for probe-req
  char     ssid[33];     // 32 bytes max + null
  int8_t   rssiMin, rssiMax;
  uint8_t  lastChannel;
  uint8_t  lastFrameType;  // last subtype seen, for at-a-glance summary
  uint32_t firstSeenMs, lastSeenMs;
  uint32_t hits;
};

static EntityRecord entities[MAX_ENTITIES];
static uint32_t deauthCount = 0;
static uint32_t disassocCount = 0;
static uint32_t mgmtFrameCount = 0;
static uint8_t  currentChannel = CHANNEL_MIN;
static uint32_t lastHopMs = 0;
static uint32_t lastSummaryMs = 0;

// 802.11 MAC header — common to every mgmt frame subtype we handle.
typedef struct __attribute__((packed)) {
  uint8_t frame_ctrl[2];
  uint8_t duration[2];
  uint8_t addr1[6];  // dst
  uint8_t addr2[6];  // src
  uint8_t addr3[6];  // bssid
  uint8_t seq_ctrl[2];
} wifi_mac_hdr_t;

static int findEntity(const uint8_t* mac) {
  for (int i = 0; i < MAX_ENTITIES; i++) {
    if (entities[i].used && memcmp(entities[i].mac, mac, 6) == 0) return i;
  }
  return -1;
}

// Evicts the least-recently-seen entry if the table is full (1.5).
static int allocEntity() {
  for (int i = 0; i < MAX_ENTITIES; i++) if (!entities[i].used) return i;
  int oldest = 0;
  for (int i = 1; i < MAX_ENTITIES; i++)
    if (entities[i].lastSeenMs < entities[oldest].lastSeenMs) oldest = i;
  return oldest;
}

// Sends one report per newly-discovered entity rather than per frame —
// mgmt frames arrive far faster than ESP-NOW should reasonably be spammed,
// and the hub only needs to know an AP/prober exists plus its latest
// rssi/channel, not every beacon interval.
static void sendWifiReport(const uint8_t* mac, const char* ssid, int8_t rssi, uint8_t channel) {
  deck_report_t report;
  memset(&report, 0, sizeof(report));
  report.node_id = 1;  // NODE_WIFI
  report.ts = millis();
  memcpy(report.mac, mac, 6);
  strncpy(report.label, ssid, sizeof(report.label) - 1);
  report.label[sizeof(report.label) - 1] = '\0';
  report.rssi = rssi;
  report.channel = channel;

  esp_now_send(BROADCAST_MAC, (uint8_t*)&report, sizeof(report));
}

static void recordEntity(const uint8_t* mac, const char* ssid, int8_t rssi,
                          uint8_t channel, uint8_t frameType) {
  uint32_t now = millis();
  int idx = findEntity(mac);
  bool isNew = idx < 0;
  if (isNew) {
    idx = allocEntity();
    memset(&entities[idx], 0, sizeof(EntityRecord));
    entities[idx].used = true;
    memcpy(entities[idx].mac, mac, 6);
    entities[idx].rssiMin = rssi;
    entities[idx].rssiMax = rssi;
    entities[idx].firstSeenMs = now;
  }
  EntityRecord& e = entities[idx];
  if (ssid[0] != '\0' || e.ssid[0] == '\0') {
    strncpy(e.ssid, ssid, sizeof(e.ssid) - 1);
    e.ssid[sizeof(e.ssid) - 1] = '\0';
  }
  if (rssi < e.rssiMin) e.rssiMin = rssi;
  if (rssi > e.rssiMax) e.rssiMax = rssi;
  e.lastChannel = channel;
  e.lastFrameType = frameType;
  e.lastSeenMs = now;
  e.hits++;

  if (isNew) sendWifiReport(e.mac, e.ssid, rssi, channel);
}

// Walks tagged params looking for tag 0 (SSID). `body` must point just
// past the fixed-length fields for the given subtype (1.4).
static void parseSSID(const uint8_t* body, int bodyLen, char* out, size_t outLen) {
  out[0] = '\0';
  int i = 0;
  while (i + 2 <= bodyLen) {
    uint8_t tag = body[i];
    uint8_t len = body[i + 1];
    if (i + 2 + len > bodyLen) break;
    if (tag == 0) {
      int copyLen = len < (int)(outLen - 1) ? len : (int)(outLen - 1);
      memcpy(out, &body[i + 2], copyLen);
      out[copyLen] = '\0';
      return;
    }
    i += 2 + len;
  }
}

static void IRAM_ATTR onPacket(void* buf, wifi_promiscuous_pkt_type_t type) {
  if (type != WIFI_PKT_MGMT) return;   // 1.3: drop data/control frames

  const wifi_promiscuous_pkt_t* pkt = (const wifi_promiscuous_pkt_t*)buf;
  const uint8_t* payload = pkt->payload;
  int len = pkt->rx_ctrl.sig_len;
  if (len < (int)sizeof(wifi_mac_hdr_t)) return;

  const wifi_mac_hdr_t* hdr = (const wifi_mac_hdr_t*)payload;
  uint8_t typeBits = (hdr->frame_ctrl[0] >> 2) & 0x3;
  uint8_t subtype  = (hdr->frame_ctrl[0] >> 4) & 0xF;
  if (typeBits != 0) return;  // not management — shouldn't happen given the filter, but safe

  mgmtFrameCount++;
  int8_t  rssi    = pkt->rx_ctrl.rssi;
  uint8_t channel = pkt->rx_ctrl.channel;

  switch (subtype) {
    case SUBTYPE_DEAUTH:
      deauthCount++;   // 1.6: count only, no action
      return;
    case SUBTYPE_DISASSOC:
      disassocCount++;
      return;
    case SUBTYPE_BEACON:
    case SUBTYPE_PROBE_RESP: {
      // Fixed fields before tags: timestamp(8) + beacon interval(2) + capability(2) = 12 bytes
      const uint8_t* body = payload + sizeof(wifi_mac_hdr_t) + 12;
      int bodyLen = len - (int)sizeof(wifi_mac_hdr_t) - 12;
      if (bodyLen <= 0) return;
      char ssid[33];
      parseSSID(body, bodyLen, ssid, sizeof(ssid));
      recordEntity(hdr->addr3, ssid, rssi, channel, subtype);
      break;
    }
    case SUBTYPE_PROBE_REQ: {
      // No fixed fields — tags start right after the MAC header. Reveals
      // src (addr2) probing for a remembered SSID (1.7 privacy angle).
      const uint8_t* body = payload + sizeof(wifi_mac_hdr_t);
      int bodyLen = len - (int)sizeof(wifi_mac_hdr_t);
      if (bodyLen <= 0) return;
      char ssid[33];
      parseSSID(body, bodyLen, ssid, sizeof(ssid));
      // Tracked under the probing device's own MAC, not a BSSID — it's
      // not an AP, but the same table works fine as a generic entity log.
      recordEntity(hdr->addr2, ssid, rssi, channel, subtype);
      break;
    }
    default:
      break;
  }
}

static void hopChannel() {
  currentChannel++;
  if (currentChannel > CHANNEL_MAX) currentChannel = CHANNEL_MIN;
  esp_wifi_set_channel(currentChannel, WIFI_SECOND_CHAN_NONE);
}

static const char* frameTypeName(uint8_t subtype) {
  switch (subtype) {
    case SUBTYPE_BEACON:     return "beacon";
    case SUBTYPE_PROBE_REQ:  return "probe_req";
    case SUBTYPE_PROBE_RESP: return "probe_resp";
    default:                 return "?";
  }
}

static void printSummary() {
  Serial.printf("=== wifi sniffer: %lu mgmt frames, %lu deauth, %lu disassoc, ch=%u ===\n",
                (unsigned long)mgmtFrameCount, (unsigned long)deauthCount,
                (unsigned long)disassocCount, currentChannel);
  for (int i = 0; i < MAX_ENTITIES; i++) {
    if (!entities[i].used) continue;
    EntityRecord& e = entities[i];
    Serial.printf("mac=%02x:%02x:%02x:%02x:%02x:%02x  ssid=\"%s\"  rssi=%d/%d  ch=%u  type=%s  hits=%lu\n",
                  e.mac[0], e.mac[1], e.mac[2], e.mac[3], e.mac[4], e.mac[5],
                  e.ssid, e.rssiMin, e.rssiMax, e.lastChannel,
                  frameTypeName(e.lastFrameType), (unsigned long)e.hits);
  }
  Serial.println("=====================================================================");
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("WiFi sniffer node — steps 1.1-1.6");

  pinMode(STATUS_LED_PIN, OUTPUT);
  digitalWrite(STATUS_LED_PIN, LOW);

  memset(entities, 0, sizeof(entities));

  nvs_flash_init();
  esp_netif_init();
  esp_event_loop_create_default();
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  esp_wifi_init(&cfg);
  esp_wifi_set_storage(WIFI_STORAGE_RAM);
  // STA mode (not WIFI_MODE_NULL) — ESP-NOW requires the driver in STA or
  // AP mode. We never call esp_wifi_connect(), so this stays unassociated;
  // promiscuous capture + channel hopping work the same either way.
  esp_wifi_set_mode(WIFI_MODE_STA);
  esp_wifi_start();
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_promiscuous_rx_cb(&onPacket);
  esp_wifi_set_channel(currentChannel, WIFI_SECOND_CHAN_NONE);

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed");
  } else {
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, BROADCAST_MAC, 6);
    peer.channel = 0;
    peer.encrypt = false;
    if (esp_now_add_peer(&peer) != ESP_OK) {
      Serial.println("ESP-NOW add broadcast peer failed");
    }
  }

  lastHopMs = millis();
  lastSummaryMs = millis();
}

void loop() {
  uint32_t now = millis();

  if (now - lastHopMs >= CHANNEL_HOP_MS) {   // 1.2: non-blocking channel hopper
    hopChannel();
    lastHopMs = now;
    digitalWrite(STATUS_LED_PIN, (currentChannel % 2) == 0 ? HIGH : LOW);  // scanning heartbeat
  }

  if (now - lastSummaryMs >= SUMMARY_INTERVAL_MS) {
    printSummary();
    lastSummaryMs = now;
  }
}
