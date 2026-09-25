// CyberDeck v2 — code common to every field node: GPS reader, wired link to the hub,
// thread-safe outbound queue, and the 1 Hz heartbeat.
//
// The including sketch must #define these BEFORE including this file:
//   DECK_NODE_KIND      DECK_NODE_WIFI or DECK_NODE_BLE
//   DECK_FW_VERSION     small integer
//   DECK_LINK_RX_PIN    node GPIO wired to the hub's TX for this node
//   DECK_LINK_TX_PIN    node GPIO wired to the hub's RX for this node
//   DECK_GPS_RX_PIN     node GPIO wired to the GPS module's TX
//   DECK_GPS_BAUD       (optional, default 9600)
//
// Requires "USB CDC On Boot: Enabled" so Serial is the USB port and hardware UART0 is free for
// the GPS. Serial1 carries the hub link.
//
// Threading: deck_node_send() and deck_geo_get() are safe to call from any task (e.g. the WiFi
// promiscuous callback or the BLE scan callback). deck_node_poll() must be called from exactly
// one task.

#ifndef DECK_NODE_CORE_H
#define DECK_NODE_CORE_H

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include "deck_link.h"

#ifndef DECK_GPS_BAUD
#define DECK_GPS_BAUD 9600
#endif

static HardwareSerial gpsSerial(0);

// Sketches bump this as they examine radio frames; reported in the heartbeat.
static volatile uint32_t deckFramesTotal = 0;

// ---------------------------------------------------------------------------------------
// GPS (NMEA RMC + GGA)
// ---------------------------------------------------------------------------------------
static deck_geo_t geoNow;
static uint32_t   geoLastRmcMs = 0;
static bool       geoRmcValid = false;
static portMUX_TYPE geoMux = portMUX_INITIALIZER_UNLOCKED;

static char    nmeaLine[100];
static uint8_t nmeaLen = 0;

// Days since 1970-01-01 for a civil date (Howard Hinnant's algorithm).
static int32_t deckDaysFromCivil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int)doe - 719468;
}

// "ddmm.mmmm" / "dddmm.mmmm" -> degrees * 1e7, sign applied by hemisphere letter.
static int32_t deckParseCoord(const char* s, char hemi) {
  if (!s || !*s) return 0;
  double v = atof(s);
  int deg = (int)(v / 100.0);
  double minutes = v - deg * 100.0;
  double d = deg + minutes / 60.0;
  if (hemi == 'S' || hemi == 'W') d = -d;
  return (int32_t)(d * 1e7);
}

static void deckHandleNmea(char* line) {
  if (line[0] != '$') return;
  char* star = strchr(line, '*');
  if (!star) return;
  uint8_t sum = 0;
  for (char* c = line + 1; c < star; c++) sum ^= (uint8_t)*c;
  if (strtoul(star + 1, nullptr, 16) != sum) return;
  *star = '\0';

  char* f[20];
  int n = 0;
  char* p = line;
  f[n++] = p;
  while (*p && n < 20) {
    if (*p == ',') { *p = '\0'; f[n++] = p + 1; }
    p++;
  }
  if (n < 7) return;
  const char* tag = f[0] + 3;  // skip "$GN" / "$GP" / "$GL"

  if (strncmp(tag, "RMC", 3) == 0 && n >= 10) {
    bool valid = f[2][0] == 'A';
    portENTER_CRITICAL(&geoMux);
    geoRmcValid = valid;
    if (valid) {
      geoNow.lat_e7 = deckParseCoord(f[3], f[4][0]);
      geoNow.lon_e7 = deckParseCoord(f[5], f[6][0]);
      // time hhmmss(.ss), date ddmmyy
      if (strlen(f[1]) >= 6 && strlen(f[9]) >= 6) {
        int hh = (f[1][0] - '0') * 10 + (f[1][1] - '0');
        int mm = (f[1][2] - '0') * 10 + (f[1][3] - '0');
        int ss = (f[1][4] - '0') * 10 + (f[1][5] - '0');
        int dd = (f[9][0] - '0') * 10 + (f[9][1] - '0');
        int mo = (f[9][2] - '0') * 10 + (f[9][3] - '0');
        int yy = 2000 + (f[9][4] - '0') * 10 + (f[9][5] - '0');
        geoNow.utc = (uint32_t)deckDaysFromCivil(yy, mo, dd) * 86400UL + hh * 3600UL + mm * 60UL + ss;
      }
      geoLastRmcMs = millis();
    }
    portEXIT_CRITICAL(&geoMux);
  } else if (strncmp(tag, "GGA", 3) == 0 && n >= 10) {
    portENTER_CRITICAL(&geoMux);
    geoNow.sats = (uint8_t)atoi(f[7]);
    geoNow.hdop_x10 = (uint8_t)constrain((int)(atof(f[8]) * 10.0), 0, 255);
    geoNow.alt_m = (int16_t)atof(f[9]);
    portEXIT_CRITICAL(&geoMux);
  }
}

static void deckGpsPoll() {
  while (gpsSerial.available()) {
    char c = (char)gpsSerial.read();
    if (c == '\n' || c == '\r') {
      if (nmeaLen > 0) {
        nmeaLine[nmeaLen] = '\0';
        deckHandleNmea(nmeaLine);
        nmeaLen = 0;
      }
    } else if (nmeaLen < sizeof(nmeaLine) - 1) {
      nmeaLine[nmeaLen++] = c;
    } else {
      nmeaLen = 0;  // overlong garbage, resync on the next line
    }
  }
}

// Snapshot of the current position. fix is forced to 0 if the GPS has gone quiet for >3 s.
static void deck_geo_get(deck_geo_t* out) {
  portENTER_CRITICAL(&geoMux);
  *out = geoNow;
  bool valid = geoRmcValid && (millis() - geoLastRmcMs) < 3000;
  portEXIT_CRITICAL(&geoMux);
  out->fix = valid ? 1 : 0;
  if (!valid) { out->lat_e7 = 0; out->lon_e7 = 0; out->utc = 0; }
}

// ---------------------------------------------------------------------------------------
// Outbound queue + link
// ---------------------------------------------------------------------------------------
typedef struct {
  uint8_t type;
  uint8_t len;
  uint8_t payload[DECK_LINK_MAX_PAYLOAD];
} deck_tx_msg_t;

static const uint8_t DECK_TX_QUEUE_LEN = 48;
static QueueHandle_t deckTxQueue = nullptr;
static volatile uint32_t deckTxDropped = 0;
static uint32_t deckLastHelloMs = 0;

// Thread-safe. Drops (and counts) the message if the queue is full rather than blocking a
// radio callback.
static void deck_node_send(uint8_t type, const void* payload, uint8_t len) {
  if (!deckTxQueue || len > DECK_LINK_MAX_PAYLOAD) return;
  deck_tx_msg_t m;
  m.type = type;
  m.len = len;
  memcpy(m.payload, payload, len);
  if (xQueueSend(deckTxQueue, &m, 0) != pdTRUE) deckTxDropped++;
}

static void deck_node_begin() {
  deckTxQueue = xQueueCreate(DECK_TX_QUEUE_LEN, sizeof(deck_tx_msg_t));
  memset(&geoNow, 0, sizeof(geoNow));

  Serial1.setTxBufferSize(2048);
  Serial1.setRxBufferSize(256);
  Serial1.begin(DECK_LINK_BAUD, SERIAL_8N1, DECK_LINK_RX_PIN, DECK_LINK_TX_PIN);

  gpsSerial.setRxBufferSize(1024);
  gpsSerial.begin(DECK_GPS_BAUD, SERIAL_8N1, DECK_GPS_RX_PIN, -1);
}

static void deckSendHello() {
  deck_hello_t h;
  memset(&h, 0, sizeof(h));
  h.node_kind = DECK_NODE_KIND;
  h.fw_version = DECK_FW_VERSION;
  h.uptime_ms = millis();
  deck_geo_get(&h.geo);
  h.frames_total = deckFramesTotal;
  h.tx_dropped = deckTxDropped;
  deck_node_send(DECK_MSG_HELLO, &h, sizeof(h));
}

// Call from one task only, as often as possible (every few ms).
static void deck_node_poll() {
  deckGpsPoll();

  uint32_t now = millis();
  if (now - deckLastHelloMs >= 1000) {
    deckLastHelloMs = now;
    deckSendHello();
  }

  deck_tx_msg_t m;
  uint8_t frame[DECK_LINK_OVERHEAD + DECK_LINK_MAX_PAYLOAD];
  while (xQueuePeek(deckTxQueue, &m, 0) == pdTRUE) {
    if ((size_t)Serial1.availableForWrite() < (size_t)DECK_LINK_OVERHEAD + m.len) break;  // try later
    xQueueReceive(deckTxQueue, &m, 0);
    size_t n = deck_link_encode(m.type, m.payload, m.len, frame);
    Serial1.write(frame, n);
  }
}

#endif  // DECK_NODE_CORE_H
