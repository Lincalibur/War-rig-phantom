// CyberDeck v2 — BLE node (XIAO ESP32-C6).
//
// Passive BLE advertisement scanner: reports every nearby BLE device (address, RSSI, name,
// manufacturer company id) with GPS position over the wired link to the hub, and flags likely
// trackers (Apple Find My / AirTag, Tile). Full-time scanning — the WiFi radio work lives on the
// other node, so the two never compete for one radio.
//
// Board:  XIAO_ESP32C6 — "USB CDC On Boot: Enabled".
//
// Planned, not implemented: 802.15.4 (Zigbee/Thread) sniffing via esp_ieee802154 on this same
// chip. The scan loop and reporting path here are radio-agnostic on purpose.
//
// NOT YET FLASHED. Pin numbers are from docs/PCB-DESIGN.md — VERIFY for your board.

#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>

#define DECK_NODE_KIND    DECK_NODE_BLE
#define DECK_FW_VERSION   1
// XIAO pin labels: D7 = RX from hub, D6 = TX to hub, D2 = GPS TX in.
#define DECK_LINK_RX_PIN  D7
#define DECK_LINK_TX_PIN  D6
#define DECK_GPS_RX_PIN   D2
#include "../shared/deck_node_core.h"

static const int8_t   RSSI_IMPROVE_DB   = 8;
static const uint32_t REREPORT_MS       = 45000;
static const uint32_t REPORT_MIN_GAP_MS = 2000;

static const uint16_t TABLE_SIZE = 512;
static const uint16_t TABLE_RESET_AT = 400;

struct BleEntity {
  bool     used;
  uint8_t  mac[6];
  int8_t   bestRssi;
  uint8_t  nameLen;
  uint32_t lastReportMs;
};

static BleEntity table[TABLE_SIZE];
static uint16_t  tableCount = 0;
static uint16_t  uniqueDevices = 0;

static uint16_t hashMac(const uint8_t* mac) {
  uint32_t h = ((uint32_t)mac[3] << 16) | ((uint32_t)mac[4] << 8) | mac[5];
  h ^= (uint32_t)mac[2] << 8 ^ mac[1];
  return (uint16_t)((h * 2654435761UL) >> 23) & (TABLE_SIZE - 1);
}

static BleEntity* lookup(const uint8_t* mac, bool* isNew) {
  if (tableCount >= TABLE_RESET_AT) {
    memset(table, 0, sizeof(table));
    tableCount = 0;
  }
  uint16_t i = hashMac(mac);
  for (uint16_t probe = 0; probe < TABLE_SIZE; probe++, i = (i + 1) & (TABLE_SIZE - 1)) {
    if (!table[i].used) { *isNew = true; return &table[i]; }
    if (memcmp(table[i].mac, mac, 6) == 0) { *isNew = false; return &table[i]; }
  }
  *isNew = true;
  return &table[0];
}

class AdvCallback : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice dev) override {
    deckFramesTotal++;

    uint8_t mac[6];
    memcpy(mac, *dev.getAddress().getNative(), 6);  // esp_bd_addr_t, MSB first
    int8_t rssi = (int8_t)dev.getRSSI();

    uint8_t flags = 0;
    if (dev.getAddressType() != BLE_ADDR_TYPE_PUBLIC) flags |= DECK_BLE_FLAG_RANDOM_ADDR;

    uint16_t mfg = 0;
    auto md = dev.getManufacturerData();
    if (md.length() >= 2) {
      mfg = (uint8_t)md[0] | ((uint16_t)(uint8_t)md[1] << 8);
      // Apple continuity payload: type 0x12 (length 0x19) is a Find My / AirTag-class advert.
      if (mfg == 0x004C && md.length() >= 3 && (uint8_t)md[2] == 0x12) flags |= DECK_BLE_FLAG_TRACKER;
    }
    // Tile trackers advertise service UUIDs 0xFEED / 0xFEEC.
    if (dev.haveServiceUUID() &&
        (dev.isAdvertisingService(BLEUUID((uint16_t)0xFEED)) || dev.isAdvertisingService(BLEUUID((uint16_t)0xFEEC))))
      flags |= DECK_BLE_FLAG_TRACKER;

    uint8_t nameLen = 0;
    char name[20] = {0};
    if (dev.haveName()) {
      const char* n = dev.getName().c_str();
      nameLen = (uint8_t)min((size_t)19, strlen(n));
      memcpy(name, n, nameLen);
      if (nameLen) flags |= DECK_BLE_FLAG_HAS_NAME;
    }

    bool isNew;
    BleEntity* e = lookup(mac, &isNew);
    uint32_t now = millis();
    bool due = false;
    if (isNew) {
      memset(e, 0, sizeof(*e));
      e->used = true;
      memcpy(e->mac, mac, 6);
      e->bestRssi = rssi;
      tableCount++;
      uniqueDevices++;
      due = true;
    } else if (nameLen > e->nameLen) {
      due = true;  // learned a name we didn't have
    } else if (now - e->lastReportMs >= REPORT_MIN_GAP_MS) {
      due = rssi >= e->bestRssi + RSSI_IMPROVE_DB || (now - e->lastReportMs) >= REREPORT_MS;
    }
    if (rssi > e->bestRssi) e->bestRssi = rssi;
    if (!due) return;
    e->nameLen = nameLen;
    e->lastReportMs = now;

    deck_ble_obs_t o;
    memset(&o, 0, sizeof(o));
    memcpy(o.mac, mac, 6);
    o.addr_type = (uint8_t)dev.getAddressType();
    o.rssi = rssi;
    o.flags = flags;
    o.mfg_id = mfg;
    o.name_len = nameLen;
    memcpy(o.name, name, nameLen);
    deck_geo_get(&o.geo);
    deck_node_send(DECK_MSG_BLE, &o, sizeof(o));
  }
};

// The BLE scan call blocks, so link + GPS servicing runs in its own task.
static void linkTask(void*) {
  for (;;) {
    deck_node_poll();
    vTaskDelay(pdMS_TO_TICKS(3));
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("CyberDeck v2 BLE node");

  memset(table, 0, sizeof(table));
  deck_node_begin();
  xTaskCreate(linkTask, "link", 4096, nullptr, 2, nullptr);

  BLEDevice::init("");
  BLEScan* scan = BLEDevice::getScan();
  static AdvCallback cb;
  scan->setAdvertisedDeviceCallbacks(&cb, true);  // true = keep duplicates, so RSSI improvements are seen
  scan->setActiveScan(true);                      // active scan also fetches scan-response names
  scan->setInterval(100);
  scan->setWindow(99);
}

void loop() {
  BLEScan* scan = BLEDevice::getScan();
  scan->start(5, false);   // blocks ~5 s, callbacks fire meanwhile
  scan->clearResults();    // free the result vector so RAM doesn't grow
  deck_geo_t g;
  deck_geo_get(&g);
  Serial.printf("ble: devices=%u frames=%lu gps=%s sats=%u dropped=%lu\n", uniqueDevices,
                (unsigned long)deckFramesTotal, g.fix ? "fix" : "none", g.sats, (unsigned long)deckTxDropped);
}
