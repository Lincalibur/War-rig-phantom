// Component 2 — WiFi Channel/Spectrum Node (ESP32-C3 #2 + 0.96" SSD1306 OLED)
//
// Replaces the original BLE hunter role (see git history / the
// pre-2026-09-01 tree for that version, dropped: NimBLE was the tightest
// flash budget on the fleet at 94%, and its feature set — persistence/
// AirTag tracker flagging — had felt lackluster in practice). This board
// is repurposed to a WiFi channel congestion / spectrum-lite analyzer:
// sweeps all 13 2.4GHz channels, measures activity per channel (frame
// count + average RSSI over a short dwell), and shows a live bar graph on
// its own OLED — useful standalone, no hub required — while also
// reporting a summarized snapshot to the hub after every full sweep.
//
// Board: ESP32C3 Dev Module (Tools > Board > esp32 > ESP32C3 Dev Module)
// IMPORTANT: this board needs "USB CDC On Boot" = Enabled or Serial
// output goes to the UART pins instead of the USB port. Via arduino-cli:
//   --fqbn 'esp32:esp32:esp32c3:CDCOnBoot=cdc'
//
// OLED wiring: VCC->3V3, GND->GND, SDA->OLED_SDA_PIN, SCL->OLED_SCL_PIN
// below. I2C address is 0x3C on the overwhelming majority of these 0.96"
// boards (0x3D on some) — change OLED_I2C_ADDR if the display stays blank.
//
// Libraries needed: Adafruit SSD1306 + Adafruit GFX (Arduino IDE > Manage
// Libraries). NimBLE-Arduino is no longer a dependency of this board.

extern "C" {
  #include "esp_wifi.h"
  #include "esp_event.h"
  #include "esp_wifi_types.h"
  #include "nvs_flash.h"
}
#include <esp_netif.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <WiFi.h>
#include <esp_now.h>
#include "../../../05-integration/shared/deck_report.h"

static const uint8_t OLED_SDA_PIN = 8;
static const uint8_t OLED_SCL_PIN = 9;
static const uint8_t OLED_I2C_ADDR = 0x3C;
static const uint8_t OLED_WIDTH = 128;
static const uint8_t OLED_HEIGHT = 64;

Adafruit_SSD1306 display(OLED_WIDTH, OLED_HEIGHT, &Wire, -1);

static const uint8_t BROADCAST_MAC[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

static const uint8_t  CHANNEL_MIN = 1;
static const uint8_t  CHANNEL_MAX = 13;
static const uint8_t  NUM_CHANNELS = CHANNEL_MAX - CHANNEL_MIN + 1;  // 13
static const uint32_t DWELL_MS = 200;   // time spent listening on each channel per sweep

// ---------------------------------------------------------------------
// Per-channel activity accumulation. onPacket() runs in the WiFi driver's
// own task context (same constraint wifi_sniffer.ino documents for its
// own promiscuous callback) — it only touches these plain counters, no
// radio calls, so no queue is needed here the way sendWifiReport() needs
// one; the actual esp_now_send() happens from loop() after a full sweep.
// ---------------------------------------------------------------------
static volatile uint32_t frameCount[NUM_CHANNELS];
static volatile int32_t  rssiSum[NUM_CHANNELS];

static uint8_t channelUtil[NUM_CHANNELS];  // normalized 0-255, last completed sweep
static uint8_t currentChannel = CHANNEL_MIN;
static uint32_t lastDwellStartMs = 0;
static bool oledAvailable = false;  // set once in setup() if display.begin() succeeds

static void IRAM_ATTR onPacket(void* buf, wifi_promiscuous_pkt_type_t type) {
  const wifi_promiscuous_pkt_t* pkt = (const wifi_promiscuous_pkt_t*)buf;
  uint8_t ch = pkt->rx_ctrl.channel;
  if (ch < CHANNEL_MIN || ch > CHANNEL_MAX) return;
  uint8_t idx = ch - CHANNEL_MIN;
  frameCount[idx]++;
  rssiSum[idx] += pkt->rx_ctrl.rssi;
}

// Finishes the sweep currently in progress: normalizes accumulated counts
// into channelUtil[], clears the accumulators, and reports the snapshot
// to the hub. Frame count alone (not RSSI) drives utilization — a
// channel packed with many weak-signal frames is still "busy" in the way
// that matters for picking a clear channel, which RSSI alone wouldn't
// capture.
// Normalization ceiling — frame count per 200ms dwell that counts as "full
// bar". Originally 60, tuned by ear with no real data; confirmed in the
// field (2026-09-03) that typical ambient traffic here is only 2-6 frames
// per dwell (occasionally ~15 on a busy channel), so 60 made every bar
// look nearly empty. Lowered to make realistic traffic actually visible.
static const uint32_t MAX_EXPECTED_FRAMES_PER_DWELL = 20;

static void finishSweep() {
  Serial.print("sweep: raw frame counts per channel (1-13):");
  for (uint8_t i = 0; i < NUM_CHANNELS; i++) {
    uint32_t count = frameCount[i];
    Serial.printf(" %lu", (unsigned long)count);
    uint32_t scaled = (count * 255UL) / MAX_EXPECTED_FRAMES_PER_DWELL;
    channelUtil[i] = (uint8_t)(scaled > 255 ? 255 : scaled);
    frameCount[i] = 0;
    rssiSum[i] = 0;
  }
  Serial.println();
}

// ---------------------------------------------------------------------
// ESP-NOW send with a bounded wait for the driver's actual send-
// completion callback before restoring the sweep channel. Without this,
// esp_wifi_set_channel() right after esp_now_send() can yank the radio
// off DECK_ESPNOW_CHANNEL before the packet has actually gone out over
// the air — esp_now_send() only queues the packet, it doesn't block
// until transmission finishes. Confirmed on the wifi sniffer node (same
// channel-park pattern): without this wait, most sends were silently
// dropped. See deck_report.h's deck_wifi_batch_t comment for the story.
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
  uint32_t start = millis();
  while (!espNowSendDone && (millis() - start) < ESPNOW_SEND_WAIT_TIMEOUT_MS) { /* spin */ }
  esp_wifi_set_channel(restoreChannel, WIFI_SECOND_CHAN_NONE);
  return result;
}

// ---------------------------------------------------------------------
// ESP-NOW report — one per completed sweep (~2.6s): this board's own
// primary channel wanders as it sweeps, so it must briefly park on
// DECK_ESPNOW_CHANNEL to actually reach the hub.
// ---------------------------------------------------------------------
static void sendSpectrumReport() {
  deck_report_t report;
  memset(&report, 0, sizeof(report));
  report.node_id = 2;  // NODE_WIFI_SPECTRUM
  report.ts = millis();
  memcpy(report.chan_util, channelUtil, NUM_CHANNELS);

  esp_err_t result = espNowSendAndWait((uint8_t*)&report, sizeof(report), currentChannel);
  Serial.printf("spectrum report sent: esp_now_send() = %d (%s)\n",
                result, result == ESP_OK ? "OK" : "FAILED");
}

static uint32_t lastHeartbeatMs = 0;
// Re-randomized after every heartbeat so this node's periodic sends drift
// relative to other CyberDeck nodes instead of staying phase-locked —
// broadcast ESP-NOW frames get no 802.11 ACK/retry, so two nodes sending
// at the same instant every cycle can silently lose one side's packets
// every single time. See wifi_sniffer.ino's setup() for the fuller story.
static uint32_t heartbeatIntervalMs = DECK_HEARTBEAT_INTERVAL_MS;

static uint32_t jitteredInterval(uint32_t base) {
  return base + (esp_random() % 500);
}

static void sendHeartbeat() {
  deck_report_t report;
  memset(&report, 0, sizeof(report));
  report.node_id = 2;  // NODE_WIFI_SPECTRUM
  report.ts = millis();
  report.flags = DECK_REPORT_FLAG_HEARTBEAT;
  espNowSendAndWait((uint8_t*)&report, sizeof(report), currentChannel);
}

// ---------------------------------------------------------------------
// Live bar graph — drawn after every completed sweep, not per-frame, so
// this never competes with the promiscuous RX callback for OLED/I2C time.
// ---------------------------------------------------------------------
// No-op if the OLED never initialized (this board doesn't have one wired
// up right now) — avoids wasting time on I2C calls to nothing every sweep.
static void renderBarGraph() {
  if (!oledAvailable) return;
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println("WiFi spectrum (ch 1-13)");

  const int graphTop = 12;
  const int graphHeight = OLED_HEIGHT - graphTop - 8;  // leave room for channel numbers
  const int barWidth = OLED_WIDTH / NUM_CHANNELS;

  for (uint8_t i = 0; i < NUM_CHANNELS; i++) {
    int h = map(channelUtil[i], 0, 255, 0, graphHeight);
    int x = i * barWidth;
    display.fillRect(x, graphTop + (graphHeight - h), barWidth - 1, h, SSD1306_WHITE);
  }
  display.drawFastHLine(0, graphTop + graphHeight, OLED_WIDTH, SSD1306_WHITE);
  display.display();
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("WiFi spectrum node — channel congestion sweep");

  Wire.begin(OLED_SDA_PIN, OLED_SCL_PIN);
  oledAvailable = display.begin(SSD1306_SWITCHCAPVCC, OLED_I2C_ADDR);
  if (!oledAvailable) {
    Serial.println("SSD1306 init failed (or not wired up on this board) — running headless");
  } else {
    display.clearDisplay();
    display.display();
  }

  memset((void*)frameCount, 0, sizeof(frameCount));
  memset((void*)rssiSum, 0, sizeof(rssiSum));
  memset(channelUtil, 0, sizeof(channelUtil));

  nvs_flash_init();
  esp_netif_init();
  esp_event_loop_create_default();
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  esp_wifi_init(&cfg);
  esp_wifi_set_storage(WIFI_STORAGE_RAM);
  esp_wifi_set_mode(WIFI_MODE_STA);
  esp_wifi_start();
  // Random startup stagger — see heartbeatIntervalMs's comment above for
  // why: without this, two CyberDeck nodes powered on together can send
  // in permanent lockstep and silently lose each other's broadcasts.
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

  lastDwellStartMs = millis();
  lastHeartbeatMs = millis();
}

void loop() {
  uint32_t now = millis();

  if (now - lastDwellStartMs >= DWELL_MS) {
    currentChannel++;
    if (currentChannel > CHANNEL_MAX) {
      currentChannel = CHANNEL_MIN;
      finishSweep();
      renderBarGraph();
      delay(esp_random() % 150);  // desync from other nodes' periodic sends — see heartbeatIntervalMs comment
      sendSpectrumReport();
    }
    esp_wifi_set_channel(currentChannel, WIFI_SECOND_CHAN_NONE);
    lastDwellStartMs = now;
  }

  if (now - lastHeartbeatMs >= heartbeatIntervalMs) {
    sendHeartbeat();
    lastHeartbeatMs = now;
    heartbeatIntervalMs = jitteredInterval(DECK_HEARTBEAT_INTERVAL_MS);
  }
}
