// Shared wire struct for C3 nodes -> hub over ESP-NOW (see ../README.md, 5.1).
// Included by both node and hub firmware — keep node/hub copies in sync
// (or #include this exact file by relative path from each sketch).
//
// ESP-NOW payload cap is 250 bytes — this struct must stay well under that.

#ifndef DECK_REPORT_H
#define DECK_REPORT_H

#include <stdint.h>

typedef struct __attribute__((packed)) {
  uint8_t  node_id;      // 1 = wifi sniffer, 2 = wifi spectrum node, 3 = sub-ghz
  uint32_t ts;
  uint8_t  mac[6];       // unused (zeroed) for node_id 2, 3; node_id 1 only sets this
                          // on a heartbeat (zeroed) — real AP/device data travels in
                          // deck_wifi_batch_t now, see below
  char     label[20];    // sub-ghz protocol label, null-terminated (node_id 3 only)
  int8_t   rssi;         // unused (0) — node_id 1's real data is in deck_wifi_batch_t
  uint8_t  channel;      // unused (0) — same
  uint8_t  flags;        // bit0 = suspect (rogue-AP dup / deauth-suspect), bit1 = heartbeat
  uint32_t subghz_code;      // node_id 3 only
  uint16_t subghz_pulse_len; // node_id 3 only, microseconds
  uint8_t  subghz_repeats;   // node_id 3 only
  uint8_t  enc_type;         // unused (0) — same as rssi/channel above
  uint8_t  chan_util[13];    // node_id 2 only — per-channel (1-13) utilization, 0-255
} deck_report_t;

#define DECK_REPORT_FLAG_SUSPECT      0x01
// Sent on a timer regardless of whether anything real was detected, so the
// hub can show a node as LIVE/STALE independent of whether its sensor is
// finding anything. Hub must skip table updates for these — see
// onEspNowRecv() in hub_console.ino.
#define DECK_REPORT_FLAG_HEARTBEAT    0x02

// deck_wifi_entry_t.enc_and_flags low 3 bits (node_id 1 only)
#define DECK_ENC_OPEN 0
#define DECK_ENC_WEP  1
#define DECK_ENC_WPA  2
#define DECK_ENC_WPA2 3
#define DECK_ENC_WPA3 4

// ---------------------------------------------------------------------
// WiFi batch report (node_id=1 only) — everything the sniffer has newly
// discovered since the last batch, sent as ONE ESP-NOW packet on a timer,
// instead of one packet per discovery. Confirmed in the field
// (2026-09-03): sending one packet per discovery was unreliable — of
// ~10 real nearby networks the sniffer found locally, only 1 reliably
// reached the hub. Root cause: esp_wifi_set_channel() was called to
// restore the scan channel immediately after esp_now_send() returned,
// without waiting for the driver's actual send-completion callback —
// esp_now_send() only queues the packet, it doesn't block until it's
// physically gone out over the air, so restoring the channel that fast
// can yank the radio off DECK_ESPNOW_CHANNEL mid-transmission, silently
// dropping the packet. Fixed two ways: (1) batching means far fewer
// channel-park/restore cycles in the first place, and (2) each send now
// waits (bounded) for esp_now_register_send_cb() before restoring the
// channel — see espNowSendAndWait() in wifi_sniffer.ino/wifi_spectrum.ino.
#define DECK_WIFI_BATCH_MAX_ENTRIES 8
#define DECK_WIFI_BATCH_INTERVAL_MS 2000UL

// Low 3 bits: DECK_ENC_* value. Bits 4/5: hidden/is-device. A distinct,
// more tightly packed bitfield than deck_report_t's own flags byte, since
// this is a compact per-entry field repeated up to 8x in one packet.
#define DECK_WIFI_ENTRY_ENC_MASK   0x07
#define DECK_WIFI_ENTRY_HIDDEN     0x10
// Entry is a probe-req from a nearby client device (its own MAC + the
// SSID it's probing for), not a beacon/probe-resp AP.
#define DECK_WIFI_ENTRY_IS_DEVICE  0x20

typedef struct __attribute__((packed)) {
  uint8_t mac[6];
  int8_t  rssi;
  uint8_t channel;
  uint8_t enc_and_flags;   // see DECK_WIFI_ENTRY_* above
  char    label[16];       // SSID (network) or probed-SSID (device), often empty for the latter
} deck_wifi_entry_t;

typedef struct __attribute__((packed)) {
  uint8_t node_id;   // always NODE_WIFI (1) — distinguished from deck_report_t
                      // purely by wire size, since ESP-NOW hands the receiver
                      // an exact byte count and the two struct sizes differ.
  uint32_t ts;
  uint8_t count;      // valid entries, 0 < count <= DECK_WIFI_BATCH_MAX_ENTRIES
  deck_wifi_entry_t entries[DECK_WIFI_BATCH_MAX_ENTRIES];
} deck_wifi_batch_t;   // 1+4+1 + 8*25 = 206 bytes, under ESP-NOW's 250-byte cap

// How often each field node sends a heartbeat when it has nothing else to
// report. Hub considers a node STALE after NODE_STALE_MS (hub_console.ino),
// which must stay comfortably larger than this.
#define DECK_HEARTBEAT_INTERVAL_MS 10000UL

// ESP-NOW requires sender and receiver to be on the same WiFi channel —
// peer.channel=0 just means "whatever channel this device is on right
// now", it does NOT force a match. All nodes + hub must explicitly pin to
// this channel (the wifi sniffer, which needs to hop 1-13 to scan, has to
// briefly switch to this channel just to send each report, then resume
// hopping — see wifi_sniffer.ino's sendWifiReport()).
#define DECK_ESPNOW_CHANNEL 1

#endif  // DECK_REPORT_H
