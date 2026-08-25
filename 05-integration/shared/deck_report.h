// Shared wire struct for C3 nodes -> hub over ESP-NOW (see ../README.md, 5.1).
// Included by both node and hub firmware — keep node/hub copies in sync
// (or #include this exact file by relative path from each sketch).
//
// ESP-NOW payload cap is 250 bytes — this struct must stay well under that.

#ifndef DECK_REPORT_H
#define DECK_REPORT_H

#include <stdint.h>

typedef struct __attribute__((packed)) {
  uint8_t  node_id;      // 1 = wifi sniffer, 2 = ble hunter
  uint32_t ts;
  uint8_t  mac[6];
  char     label[20];    // SSID or BLE name, truncated, null-terminated
  int8_t   rssi;
  uint8_t  channel;
  uint8_t  flags;        // bit0 = tracker-suspect / deauth-suspect
} deck_report_t;

#define DECK_REPORT_FLAG_SUSPECT 0x01

#endif  // DECK_REPORT_H
