// Display-free smoke test for 02-ble-hunter's BLE scan, split out from
// ble_scan.ino to isolate "does the scan itself find anything" from the
// OLED/persistence-scoring logic. No Wire/Adafruit deps, no display wiring
// needed. Not meant to be kept in sync with ble_scan.ino long-term —
// throwaway diagnostic sketch.
//
// Board: ESP32C3 Dev Module
// FQBN: esp32:esp32:esp32c3:CDCOnBoot=cdc   (required for Serial over USB)

#include <NimBLEDevice.h>

static const uint32_t SCAN_WINDOW_MS = 3000;
static const uint32_t SCAN_PAUSE_MS  = 2000;

class ScanCallbacks : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* device) override {
    std::string macStr = device->getAddress().toString();
    Serial.printf(
      "seen: mac=%s rssi=%d name=\"%s\"\n",
      macStr.c_str(),
      (int)device->getRSSI(),
      device->haveName() ? device->getName().c_str() : ""
    );
  }
};

NimBLEScan* pScan = nullptr;

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("BLE scan smoke test — no display, no scoring");

  NimBLEDevice::init("");
  pScan = NimBLEDevice::getScan();
  pScan->setScanCallbacks(new ScanCallbacks(), true);
  pScan->setActiveScan(true);
  pScan->setInterval(100);
  pScan->setWindow(99);
  pScan->setDuplicateFilter(0);
}

void loop() {
  Serial.println("-- starting scan window --");
  pScan->start(SCAN_WINDOW_MS, false);  // NimBLE-Arduino 2.x: duration is milliseconds
  pScan->clearResults();
  delay(SCAN_PAUSE_MS);
}
