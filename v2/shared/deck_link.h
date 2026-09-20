// CyberDeck v2 — wired node<->hub link protocol.
//
// Replaces the ESP-NOW transport. Each field node (WiFi, BLE) talks to the hub over its own
// point-to-point 3.3V UART at DECK_LINK_BAUD. Nodes never transmit RF, so there is no
// channel-parking, send-callback, or near-field-desense problem to work around.
//
// Frame:   A5 5A | type | len | payload[len] | crc16_hi | crc16_lo
// CRC:     CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF) over type,len,payload.
// Layout:  every struct is packed, little-endian (native on ESP32).
//
// Included by the node sketches AND the hub sketch — this file is the single source of truth.

#ifndef DECK_LINK_H
#define DECK_LINK_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define DECK_LINK_BAUD         921600UL
#define DECK_LINK_SOF0         0xA5
#define DECK_LINK_SOF1         0x5A
#define DECK_LINK_MAX_PAYLOAD  96
#define DECK_LINK_OVERHEAD     6      // SOF0 SOF1 type len crc_hi crc_lo

enum {
  DECK_MSG_HELLO       = 0x01,
  DECK_MSG_WIFI_AP     = 0x10,
  DECK_MSG_WIFI_CLIENT = 0x11,
  DECK_MSG_BLE         = 0x20,
  DECK_MSG_STATS       = 0x30,
};

enum {
  DECK_NODE_WIFI = 1,
  DECK_NODE_BLE  = 2,
};

// Encryption classes for DECK_MSG_WIFI_AP.enc
enum {
  DECK_ENC_OPEN      = 0,
  DECK_ENC_WEP       = 1,
  DECK_ENC_WPA       = 2,
  DECK_ENC_WPA2      = 3,
  DECK_ENC_WPA3      = 4,
  DECK_ENC_WPA2_WPA3 = 5,
  DECK_ENC_WPA2_ENT  = 6,   // 802.1X / enterprise
};

// DECK_MSG_WIFI_AP / WIFI_CLIENT .flags
#define DECK_WIFI_FLAG_HIDDEN      0x01
#define DECK_WIFI_FLAG_RANDOM_MAC  0x02   // locally-administered bit set (private/rotating MAC)

// DECK_MSG_BLE .flags
#define DECK_BLE_FLAG_TRACKER      0x01   // AirTag / Find-My / Tile style beacon
#define DECK_BLE_FLAG_RANDOM_ADDR  0x02
#define DECK_BLE_FLAG_HAS_NAME     0x04

// Position + time attached to every observation, so the hub never has to guess where a
// sighting happened. fix==0 means lat/lon/utc are not valid.
typedef struct __attribute__((packed)) {
  int32_t  lat_e7;
  int32_t  lon_e7;
  uint32_t utc;        // unix seconds (UTC), 0 if unknown
  int16_t  alt_m;
  uint8_t  fix;        // 0 = no fix, 1 = valid fix
  uint8_t  sats;
  uint8_t  hdop_x10;   // HDOP * 10
} deck_geo_t;

// Node liveness, ~1 Hz, always sent (even when nothing is seen).
typedef struct __attribute__((packed)) {
  uint8_t    node_kind;
  uint8_t    fw_version;
  uint32_t   uptime_ms;
  deck_geo_t geo;
  uint32_t   frames_total;   // radio frames/adverts examined since boot
  uint32_t   tx_dropped;     // observations dropped because the UART queue was full
} deck_hello_t;

// DECK_MSG_WIFI_AP: an access point.  DECK_MSG_WIFI_CLIENT: a station.
//   AP:     mac = BSSID, ap_mac = zeros, ssid = network name.
//   Client: mac = station, ap_mac = BSSID it talks to (from data frames) or zeros,
//           ssid = SSID it is probing for (from probe requests) or empty.
typedef struct __attribute__((packed)) {
  uint8_t    mac[6];
  uint8_t    ap_mac[6];
  int8_t     rssi;
  uint8_t    channel;       // primary channel; >14 means 5 GHz
  uint8_t    enc;           // DECK_ENC_*  (APs only)
  uint8_t    flags;         // DECK_WIFI_FLAG_*
  uint8_t    ssid_len;
  char       ssid[32];
  deck_geo_t geo;
} deck_wifi_obs_t;

typedef struct __attribute__((packed)) {
  uint8_t    mac[6];        // as printed (MSB first)
  uint8_t    addr_type;     // 0 public, 1 random
  int8_t     rssi;
  uint8_t    flags;         // DECK_BLE_FLAG_*
  uint16_t   mfg_id;        // BT SIG company id from manufacturer data, 0 if none
  uint8_t    name_len;
  char       name[20];
  deck_geo_t geo;
} deck_ble_obs_t;

// WiFi node only, ~2 Hz: what the sniffer is seeing overall.
typedef struct __attribute__((packed)) {
  uint8_t  node_kind;
  uint32_t mgmt_frames;
  uint32_t deauth;
  uint32_t disassoc;
  uint16_t ap_count;         // distinct APs the node has seen since its table last reset
  uint16_t client_count;
  uint16_t chan_frames[13];  // frames per 2.4 GHz channel since the last STATS
  uint16_t five_ghz_frames;  // 5 GHz frames (all channels) since the last STATS
} deck_stats_t;

// ---------------------------------------------------------------------------------------
// Framing
// ---------------------------------------------------------------------------------------
static inline uint16_t deck_crc16(const uint8_t* d, size_t n) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < n; i++) {
    crc ^= (uint16_t)d[i] << 8;
    for (int b = 0; b < 8; b++) crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
  }
  return crc;
}

// `out` must hold at least DECK_LINK_OVERHEAD + len bytes. Returns the frame length.
static inline size_t deck_link_encode(uint8_t type, const void* payload, uint8_t len, uint8_t* out) {
  out[0] = DECK_LINK_SOF0;
  out[1] = DECK_LINK_SOF1;
  out[2] = type;
  out[3] = len;
  memcpy(out + 4, payload, len);
  uint16_t crc = deck_crc16(out + 2, 2 + (size_t)len);
  out[4 + len] = (uint8_t)(crc >> 8);
  out[5 + len] = (uint8_t)(crc & 0xFF);
  return DECK_LINK_OVERHEAD + len;
}

// Byte-at-a-time decoder. feed() returns true exactly when a complete, CRC-valid frame has
// just been received; type/len/payload are then valid until the next feed() call.
typedef struct {
  uint8_t  state;
  uint8_t  type;
  uint8_t  len;
  uint8_t  pos;
  uint8_t  crc_hi;
  uint8_t  payload[DECK_LINK_MAX_PAYLOAD];
  uint32_t frames_ok;
  uint32_t crc_errors;
} deck_link_parser_t;

static inline bool deck_link_feed(deck_link_parser_t* p, uint8_t b) {
  switch (p->state) {
    case 0: if (b == DECK_LINK_SOF0) p->state = 1; break;
    case 1: p->state = (b == DECK_LINK_SOF1) ? 2 : (b == DECK_LINK_SOF0 ? 1 : 0); break;
    case 2: p->type = b; p->state = 3; break;
    case 3:
      p->len = b; p->pos = 0;
      if (b > DECK_LINK_MAX_PAYLOAD) p->state = 0;
      else p->state = (b == 0) ? 5 : 4;
      break;
    case 4:
      p->payload[p->pos++] = b;
      if (p->pos >= p->len) p->state = 5;
      break;
    case 5: p->crc_hi = b; p->state = 6; break;
    case 6: {
      p->state = 0;
      uint8_t tmp[2 + DECK_LINK_MAX_PAYLOAD];
      tmp[0] = p->type; tmp[1] = p->len;
      memcpy(tmp + 2, p->payload, p->len);
      uint16_t crc = deck_crc16(tmp, 2 + (size_t)p->len);
      if (crc == (uint16_t)((p->crc_hi << 8) | b)) { p->frames_ok++; return true; }
      p->crc_errors++;
      break;
    }
  }
  return false;
}

#endif  // DECK_LINK_H
