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
  uint8_t  encType;        // DECK_ENC_* — meaningful for beacon/probe-resp entities only
  bool     hidden;         // tag 0 present with length 0 — beacon/probe-resp entities only
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

// ---------------------------------------------------------------------
// ESP-NOW send with a bounded wait for the driver's actual send-
// completion callback before restoring the scan channel. Without this,
// esp_wifi_set_channel() right after esp_now_send() can yank the radio
// off DECK_ESPNOW_CHANNEL before the packet has actually gone out over
// the air — esp_now_send() only queues the packet, it doesn't block
// until transmission finishes. Confirmed in the field: without this
// wait, only ~1 in 10 discoveries was reaching the hub. See
// deck_report.h's deck_wifi_batch_t comment for the full story.
// ---------------------------------------------------------------------
static volatile bool espNowSendDone = true;

static void onEspNowSendDone(const wifi_tx_info_t* txInfo, esp_now_send_status_t status) {
  espNowSendDone = true;
}

static const uint32_t ESPNOW_SEND_WAIT_TIMEOUT_MS = 20;

static esp_err_t espNowSendAndWait(const uint8_t* data, size_t len, uint8_t restoreChannel) {
  esp_wifi_set_channel(DECK_ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
  espNowSendDone = false;
  esp_err_t result = esp_now_send(BROADCAST_MAC, data, len);
  Serial.printf("SEND len=%u result=%d (%s)\n", (unsigned)len, result, result == ESP_OK ? "OK" : "ERR");
  uint32_t start = millis();
  while (!espNowSendDone && (millis() - start) < ESPNOW_SEND_WAIT_TIMEOUT_MS) { /* spin */ }
  Serial.printf("  send-cb %s after %lums\n", espNowSendDone ? "fired" : "TIMED OUT",
                (unsigned long)(millis() - start));
  esp_wifi_set_channel(restoreChannel, WIFI_SECOND_CHAN_NONE);
  return result;
}

// ---------------------------------------------------------------------
// Batches newly-discovered entities into ONE ESP-NOW packet sent on a
// timer (DECK_WIFI_BATCH_INTERVAL_MS), instead of one packet per
// discovery — fewer channel-park events, and each individual entity no
// longer depends on its own send succeeding. recordEntity() runs inside
// the promiscuous RX callback (the WiFi driver's own task context), so
// enqueueEntry() only appends to this lock-free single-producer/consumer
// ring buffer here; flushPendingReports(), called from loop() (a normal
// task context), builds and sends the actual batch packet.
// ---------------------------------------------------------------------
static const uint8_t PENDING_QUEUE_LEN = DECK_WIFI_BATCH_MAX_ENTRIES + 1;  // +1 so full/empty are distinguishable
static deck_wifi_entry_t pendingEntries[PENDING_QUEUE_LEN];
static volatile uint8_t pendingHead = 0;  // next slot to write
static volatile uint8_t pendingTail = 0;  // next slot to read

static void enqueueEntry(const uint8_t* mac, const char* label, int8_t rssi, uint8_t channel,
                          uint8_t encType, bool hidden, bool isDevice) {
  uint8_t nextHead = (pendingHead + 1) % PENDING_QUEUE_LEN;
  if (nextHead == pendingTail) return;  // queue full — drop rather than block the RX callback

  deck_wifi_entry_t& e = pendingEntries[pendingHead];
  memcpy(e.mac, mac, 6);
  e.rssi = rssi;
  e.channel = channel;
  e.enc_and_flags = (encType & DECK_WIFI_ENTRY_ENC_MASK) |
                    (hidden ? DECK_WIFI_ENTRY_HIDDEN : 0) |
                    (isDevice ? DECK_WIFI_ENTRY_IS_DEVICE : 0);
  strncpy(e.label, label, sizeof(e.label) - 1);
  e.label[sizeof(e.label) - 1] = '\0';
  pendingHead = nextHead;
}

static uint32_t lastHeartbeatMs = 0;
static uint32_t lastBatchSendMs = 0;
// Re-randomized after every send (see espNowJitteredIntervalMs()) so this
// node's periodic sends drift relative to other CyberDeck nodes instead of
// staying phase-locked with them — see the startup-stagger comment in
// setup() for why that matters.
static uint32_t heartbeatIntervalMs = DECK_HEARTBEAT_INTERVAL_MS;
static uint32_t batchIntervalMs = DECK_WIFI_BATCH_INTERVAL_MS;

static uint32_t jitteredInterval(uint32_t base) {
  return base + (esp_random() % 500);
}

static void sendHeartbeat() {
  deck_report_t report;
  memset(&report, 0, sizeof(report));
  report.node_id = 1;  // NODE_WIFI
  report.ts = millis();
  report.flags = DECK_REPORT_FLAG_HEARTBEAT;
  espNowSendAndWait((uint8_t*)&report, sizeof(report), currentChannel);
}

static bool pendingQueueNearFull() {
  uint8_t used = (pendingHead + PENDING_QUEUE_LEN - pendingTail) % PENDING_QUEUE_LEN;
  return used >= DECK_WIFI_BATCH_MAX_ENTRIES;
}

// Sends as soon as the queue fills up (so a burst of discoveries doesn't
// have to wait out the full interval), otherwise waits for
// DECK_WIFI_BATCH_INTERVAL_MS since the last send.
static uint32_t lastFlushDebugMs = 0;

static void flushPendingReports() {
  if (pendingHead == pendingTail) return;  // nothing queued
  uint32_t now = millis();
  bool nearFull = pendingQueueNearFull();
  bool intervalUp = (now - lastBatchSendMs) >= batchIntervalMs;
  if (now - lastFlushDebugMs >= 500) {
    Serial.printf("flush check: head=%u tail=%u nearFull=%d intervalUp=%d\n",
                  pendingHead, pendingTail, nearFull, intervalUp);
    lastFlushDebugMs = now;
  }
  if (!nearFull && !intervalUp) return;

  deck_wifi_batch_t batch;
  memset(&batch, 0, sizeof(batch));
  batch.node_id = 1;  // NODE_WIFI
  batch.ts = now;
  while (pendingTail != pendingHead && batch.count < DECK_WIFI_BATCH_MAX_ENTRIES) {
    batch.entries[batch.count++] = pendingEntries[pendingTail];
    pendingTail = (pendingTail + 1) % PENDING_QUEUE_LEN;
  }
  esp_err_t result = espNowSendAndWait((uint8_t*)&batch, sizeof(batch), currentChannel);
  if (result != ESP_OK) Serial.printf("wifi batch esp_now_send() FAILED: %d\n", result);
  lastBatchSendMs = now;
  batchIntervalMs = jitteredInterval(DECK_WIFI_BATCH_INTERVAL_MS);
}

// encType/hidden are only meaningful for beacon/probe-resp entities (the
// AP itself) — probe-req callers pass DECK_ENC_OPEN/false, which is
// harmless since those entities key on the probing client's own MAC, a
// different table slot than the AP it's asking about.
static void recordEntity(const uint8_t* mac, const char* ssid, int8_t rssi,
                          uint8_t channel, uint8_t frameType,
                          uint8_t encType, bool hidden) {
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
  if (frameType != SUBTYPE_PROBE_REQ) {
    e.encType = encType;
    e.hidden = hidden;
  }
  e.lastSeenMs = now;
  e.hits++;

  if (isNew) enqueueEntry(e.mac, e.ssid, rssi, channel, e.encType, e.hidden,
                           frameType == SUBTYPE_PROBE_REQ);
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

// tag 0 (SSID) present with length 0 is the standard "hidden network" beacon
// shape (vs. a legitimately-named network, or simply not having seen a
// beacon for this BSSID yet, which parseSSID also leaves as an empty
// string but which isHiddenSSID never gets called for).
static bool isHiddenSSID(const uint8_t* body, int bodyLen) {
  int i = 0;
  while (i + 2 <= bodyLen) {
    uint8_t tag = body[i];
    uint8_t len = body[i + 1];
    if (i + 2 + len > bodyLen) break;
    if (tag == 0) return len == 0;
    i += 2 + len;
  }
  return false;
}

// Capability-info bit 4 (Privacy) says only whether *some* cipher is in
// use, not which. Distinguishing WPA2 from WPA3 needs the RSN element's
// AKM suite list (tag 48); legacy WPA1 (no RSN, just a Microsoft vendor IE)
// needs tag 221 with OUI 00:50:F2 type 1. No RSN/WPA IE at all + Privacy
// set means old-style WEP, which predates both.
static uint8_t parseEncryption(const uint8_t* body, int bodyLen, uint16_t capInfo) {
  if ((capInfo & 0x0010) == 0) return DECK_ENC_OPEN;

  bool hasRSN = false, hasWPA1 = false, wpa3Akm = false;
  int i = 0;
  while (i + 2 <= bodyLen) {
    uint8_t tag = body[i];
    uint8_t len = body[i + 1];
    if (i + 2 + len > bodyLen) break;
    int elemStart = i + 2;
    int elemEnd = i + 2 + len;

    if (tag == 48) {  // RSN element
      hasRSN = true;
      int off = elemStart + 2;  // skip version(2)
      off += 4;                 // skip group cipher suite(4)
      if (off + 2 <= elemEnd) {
        uint16_t pairwiseCount = body[off] | (body[off + 1] << 8);
        off += 2 + (int)pairwiseCount * 4;
      }
      if (off + 2 <= elemEnd) {
        uint16_t akmCount = body[off] | (body[off + 1] << 8);
        off += 2;
        for (uint16_t k = 0; k < akmCount && off + 4 <= elemEnd; k++) {
          uint8_t suiteType = body[off + 3];  // OUI(3) + type(1)
          if (suiteType == 8 || suiteType == 9) wpa3Akm = true;  // SAE / FT-SAE
          off += 4;
        }
      }
    } else if (tag == 221 && len >= 4 &&
               body[elemStart] == 0x00 && body[elemStart + 1] == 0x50 &&
               body[elemStart + 2] == 0xF2 && body[elemStart + 3] == 0x01) {
      hasWPA1 = true;  // Microsoft WPA1 vendor IE
    }
    i += 2 + len;
  }

  if (hasRSN) return wpa3Akm ? DECK_ENC_WPA3 : DECK_ENC_WPA2;
  if (hasWPA1) return DECK_ENC_WPA;
  return DECK_ENC_WEP;
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
      const uint8_t* fixedFields = payload + sizeof(wifi_mac_hdr_t);
      const uint8_t* body = fixedFields + 12;
      int bodyLen = len - (int)sizeof(wifi_mac_hdr_t) - 12;
      if (bodyLen <= 0) return;
      uint16_t capInfo = fixedFields[10] | (fixedFields[11] << 8);
      char ssid[33];
      parseSSID(body, bodyLen, ssid, sizeof(ssid));
      uint8_t encType = parseEncryption(body, bodyLen, capInfo);
      bool hidden = isHiddenSSID(body, bodyLen);
      recordEntity(hdr->addr3, ssid, rssi, channel, subtype, encType, hidden);
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
      // Encryption/hidden don't apply to a prober, recordEntity() ignores
      // these two args for SUBTYPE_PROBE_REQ.
      recordEntity(hdr->addr2, ssid, rssi, channel, subtype, DECK_ENC_OPEN, false);
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

static const char* encTypeName(uint8_t encType) {
  switch (encType) {
    case DECK_ENC_OPEN: return "open";
    case DECK_ENC_WEP:  return "wep";
    case DECK_ENC_WPA:  return "wpa";
    case DECK_ENC_WPA2: return "wpa2";
    case DECK_ENC_WPA3: return "wpa3";
    default:            return "?";
  }
}

static void printSummary() {
  Serial.printf("=== wifi sniffer: %lu mgmt frames, %lu deauth, %lu disassoc, ch=%u ===\n",
                (unsigned long)mgmtFrameCount, (unsigned long)deauthCount,
                (unsigned long)disassocCount, currentChannel);
  for (int i = 0; i < MAX_ENTITIES; i++) {
    if (!entities[i].used) continue;
    EntityRecord& e = entities[i];
    Serial.printf("mac=%02x:%02x:%02x:%02x:%02x:%02x  ssid=\"%s\"%s  rssi=%d/%d  ch=%u  type=%s  enc=%s  hits=%lu\n",
                  e.mac[0], e.mac[1], e.mac[2], e.mac[3], e.mac[4], e.mac[5],
                  e.ssid, e.hidden ? " (hidden)" : "", e.rssiMin, e.rssiMax, e.lastChannel,
                  frameTypeName(e.lastFrameType), encTypeName(e.encType), (unsigned long)e.hits);
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
  // Random startup stagger: ESP-NOW broadcast frames get no 802.11 ACK/
  // retry (unlike unicast), so if this node and another CyberDeck node
  // power on at close to the same instant (e.g. off the same powerbank),
  // their independent periodic send timers can stay locked in phase —
  // confirmed in the field: with the wifi spectrum node also running, the
  // hub stopped receiving ANY of this node's batches, not just some.
  // A random 0-500ms offset here plus the jittered intervals below
  // (flushPendingReports()/loop()'s heartbeat check) breaks that lock.
  delay(esp_random() % 500);
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
    esp_now_register_send_cb(onEspNowSendDone);
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

  if (now - lastHeartbeatMs >= heartbeatIntervalMs) {
    sendHeartbeat();
    lastHeartbeatMs = now;
    heartbeatIntervalMs = jitteredInterval(DECK_HEARTBEAT_INTERVAL_MS);
  }

  flushPendingReports();
}
