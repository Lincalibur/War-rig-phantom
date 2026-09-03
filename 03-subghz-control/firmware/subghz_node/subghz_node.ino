// Component 3 — Sub-GHz Node (ESP32-C3), ESP-NOW-consolidated redesign.
//
// Replaces the original Arduino Uno + big LCD design (see git history /
// the pre-esp-now-consolidation branch for that version). This node now
// only does 3.1-3.3 (rc-switch decode, raw pulse-timing fallback, event
// logging) and reports over ESP-NOW as deck_report_t, node_id=3. The
// control-surface/menu/LCD responsibilities (former 3.4-3.6) move to the
// hub console — this device carries no display and no buttons.
//
// INITIAL CUT — not yet flashed/tested on real hardware.
//
// Hardware / wiring:
// - Sub-GHz RX module (RXB6/XY-MK-5V) data-out pin fans out to BOTH
//   RCSWITCH_PIN and RAW_PIN below.
// - Unlike the Uno, ESP32-C3 GPIOs can all do external interrupts, so
//   these aren't pinned to fixed INT0/INT1 — pick any free GPIOs, just
//   keep RCSWITCH_PIN and RAW_PIN wired to the same RX module output.
// - Antenna: ~17.3cm wire, quarter-wave for 433MHz, on the RX module's
//   antenna pad (per [[cyberdeck_power_supply_requirements]] plan, no
//   external/U.FL antenna on this pass).
//
// Library needed: rc-switch (sui77) — Arduino IDE > Tools > Manage
// Libraries > search "rc-switch".
//
// Board: ESP32C3 Dev Module.
// FQBN: esp32:esp32:esp32c3:CDCOnBoot=cdc   (required for Serial over USB)
//
// ESP-NOW: broadcasts to FF:FF:FF:FF:FF:FF rather than pairing to the
// hub's specific MAC — the hub just needs to be running esp_now_init() +
// esp_now_register_recv_cb() to receive broadcast packets, no MAC
// exchange step needed before this node can be flashed/tested standalone.

#include <RCSwitch.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include "../../../05-integration/shared/deck_report.h"

static const uint8_t RCSWITCH_PIN = 4;
static const uint8_t RAW_PIN      = 5;

RCSwitch rcSwitch;

static const uint8_t BROADCAST_MAC[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

// ---------------------------------------------------------------------
// 3.2 — raw pulse-timing fallback, for anything rc-switch doesn't decode
// ---------------------------------------------------------------------
// rc-switch only surfaces a result when one of its known fixed-code
// protocols matches; it gives no public access to raw timings on a
// failed decode. So RAW_PIN runs an independent edge-timing capture:
// record each pulse width, and once a gap longer than RAW_GAP_TIMEOUT_US
// closes out a burst, treat that burst as one candidate code.

static const uint16_t RAW_BUF_LEN = 200;
static const uint32_t RAW_GAP_TIMEOUT_US = 4000;
static const uint8_t  RAW_MIN_PULSES = 8;

volatile uint16_t rawPulses[RAW_BUF_LEN];
volatile uint16_t rawPulseCount = 0;
volatile uint32_t rawLastEdgeUs = 0;
volatile bool     rawBurstReady = false;

void IRAM_ATTR rawEdgeISR() {
  uint32_t now = micros();
  uint32_t delta = now - rawLastEdgeUs;
  rawLastEdgeUs = now;

  if (delta > RAW_GAP_TIMEOUT_US) {
    if (rawPulseCount >= RAW_MIN_PULSES && !rawBurstReady) {
      rawBurstReady = true;
      return;
    }
    rawPulseCount = 0;
  }

  if (!rawBurstReady && rawPulseCount < RAW_BUF_LEN) {
    rawPulses[rawPulseCount++] = (delta > 0xFFFF) ? 0xFFFF : (uint16_t)delta;
  }
}

// ---------------------------------------------------------------------
// 3.3 — event logging + ESP-NOW send
// ---------------------------------------------------------------------

static uint32_t subghzHitCount = 0;

static void sendSubGhzReport(const char* protocol, uint32_t code,
                              uint16_t pulseLength, uint8_t repeats) {
  deck_report_t report;
  memset(&report, 0, sizeof(report));
  report.node_id = 3;  // NODE_SUBGHZ
  report.ts = millis();
  strncpy(report.label, protocol, sizeof(report.label) - 1);
  report.label[sizeof(report.label) - 1] = '\0';
  report.subghz_code = code;
  report.subghz_pulse_len = pulseLength;
  report.subghz_repeats = repeats;

  esp_now_send(BROADCAST_MAC, (uint8_t*)&report, sizeof(report));
}

static void logSubGhzEvent(const char* protocol, uint32_t code,
                            uint16_t pulseLength, uint8_t repeats) {
  subghzHitCount++;
  Serial.printf("SUBGHZ,%lu,%s,%lu,%u,%u\n",
                millis() / 1000UL, protocol, (unsigned long)code, pulseLength, repeats);
  sendSubGhzReport(protocol, code, pulseLength, repeats);
}

// No periodic report otherwise — sub-ghz only sends when it decodes
// something, so "never seen" on the hub's health screen used to be
// indistinguishable from "board is dead". This makes "board is alive but
// nothing decoded" verifiable from the hub screen alone.
static uint32_t lastHeartbeatMs = 0;

static void sendHeartbeat() {
  deck_report_t report;
  memset(&report, 0, sizeof(report));
  report.node_id = 3;  // NODE_SUBGHZ
  report.ts = millis();
  report.flags = DECK_REPORT_FLAG_HEARTBEAT;
  esp_now_send(BROADCAST_MAC, (uint8_t*)&report, sizeof(report));
}

static void pollSubGhz() {
  // 3.1 — rc-switch known-protocol decode
  if (rcSwitch.available()) {
    unsigned long code = rcSwitch.getReceivedValue();
    if (code != 0) {
      char protoLabel[16];
      snprintf(protoLabel, sizeof(protoLabel), "rc-switch:%d", rcSwitch.getReceivedProtocol());
      logSubGhzEvent(protoLabel, (uint32_t)code, rcSwitch.getReceivedDelay(), 1);
    }
    rcSwitch.resetAvailable();
  }

  // 3.2 — raw fallback, only when rc-switch didn't already claim this burst
  if (rawBurstReady) {
    noInterrupts();
    uint16_t count = rawPulseCount;
    uint16_t firstPulse = count > 0 ? rawPulses[0] : 0;
    interrupts();

    uint32_t pulseSum = 0;
    noInterrupts();
    for (uint16_t i = 0; i < count; i++) pulseSum += rawPulses[i];
    interrupts();

    logSubGhzEvent("raw", pulseSum, firstPulse, 1);

    noInterrupts();
    rawPulseCount = 0;
    rawBurstReady = false;
    interrupts();
  }
}

// ---------------------------------------------------------------------

static void setupEspNow() {
  WiFi.mode(WIFI_STA);
  delay(100);  // driver init is async — see hub_console.ino's macAddress() fix
  esp_wifi_set_channel(DECK_ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed");
    return;
  }
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, BROADCAST_MAC, 6);
  peer.channel = 0;
  peer.encrypt = false;
  if (esp_now_add_peer(&peer) != ESP_OK) {
    Serial.println("ESP-NOW add broadcast peer failed");
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("Sub-GHz node — steps 3.1-3.3, ESP-NOW report only");

  setupEspNow();

  rcSwitch.enableReceive(digitalPinToInterrupt(RCSWITCH_PIN));

  pinMode(RAW_PIN, INPUT);
  attachInterrupt(digitalPinToInterrupt(RAW_PIN), rawEdgeISR, CHANGE);
}

void loop() {
  pollSubGhz();

  if (millis() - lastHeartbeatMs >= DECK_HEARTBEAT_INTERVAL_MS) {
    sendHeartbeat();
    lastHeartbeatMs = millis();
  }
}
