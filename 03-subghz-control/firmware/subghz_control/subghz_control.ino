// Component 3 — Sub-GHz Node + Control Surface (Arduino Uno)
//
// Covers README steps 3.1-3.3, 3.5, 3.6. Step 3.4 (big LCD driver) is
// deliberately stubbed out behind renderScreen()/ScreenBackend below —
// wire it up once the exact part (character LCD vs graphic TFT) is
// confirmed, everything else here is already screen-agnostic and prints
// the same content to Serial in the meantime.
//
// INITIAL CUT — not yet flashed/tested on real hardware.
//
// Hardware / wiring:
// - Sub-GHz RX module (RXB6/XY-MK-5V) data-out pin fans out to BOTH
//   RCSWITCH_PIN and RAW_PIN below — it's a plain digital signal, driving
//   two Uno inputs from one output pin is fine electrically.
// - Uno only has two external-interrupt-capable pins (D2, D3), so
//   RCSWITCH_PIN and RAW_PIN are pinned to those; don't move them without
//   also fixing the interrupt wiring.
// - Buttons: NEXT_PIN / SELECT_PIN wired to remaining digital pins,
//   other side to GND (using INPUT_PULLUP, so no external resistors).
// - Antenna: ~17.3cm wire, quarter-wave for 433MHz, on the RX module's
//   antenna pad.
//
// Library needed: rc-switch (sui77) — Arduino IDE > Tools > Manage
// Libraries > search "rc-switch".
//
// Board: Arduino Uno.

#include <RCSwitch.h>

static const uint8_t RCSWITCH_PIN = 2;   // D2 = INT0
static const uint8_t RAW_PIN      = 3;   // D3 = INT1
static const uint8_t NEXT_PIN     = 4;
static const uint8_t SELECT_PIN   = 5;

RCSwitch rcSwitch;

// Struct used by buttonPressed() below, defined here (right after
// includes) rather than down in the 3.5 section — the Arduino IDE's
// auto-generated function prototypes get inserted at the top of the
// file, and a prototype referencing Button needs the type already
// visible at that point or the build fails with "not declared".
struct Button {
  uint8_t  pin;
  bool     lastReading;
  bool     stableState;
  uint32_t lastChangeMs;
};

// ---------------------------------------------------------------------
// 3.2 — raw pulse-timing fallback, for anything rc-switch doesn't decode
// ---------------------------------------------------------------------
// rc-switch only surfaces a result when one of its known fixed-code
// protocols matches; it gives no public access to raw timings on a
// failed decode. So RAW_PIN runs an independent edge-timing capture:
// record each pulse width, and once a gap longer than RAW_GAP_TIMEOUT_US
// closes out a burst, treat that burst as one candidate code. If
// rc-switch decoded something in roughly the same window we skip logging
// the raw version (avoid double-logging the same real signal); otherwise
// log it as a raw/unrecognized capture.

static const uint16_t RAW_BUF_LEN = 200;         // ~400 bytes, fine on Uno's 2KB SRAM
static const uint32_t RAW_GAP_TIMEOUT_US = 4000;  // gap that ends a burst
static const uint8_t  RAW_MIN_PULSES = 8;         // discard shorter noise runts

volatile uint16_t rawPulses[RAW_BUF_LEN];
volatile uint16_t rawPulseCount = 0;
volatile uint32_t rawLastEdgeUs = 0;
volatile bool     rawBurstReady = false;

void rawEdgeISR() {
  uint32_t now = micros();
  uint32_t delta = now - rawLastEdgeUs;
  rawLastEdgeUs = now;

  if (delta > RAW_GAP_TIMEOUT_US) {
    // Gap closes out whatever was accumulating before this edge.
    if (rawPulseCount >= RAW_MIN_PULSES && !rawBurstReady) {
      rawBurstReady = true;   // main loop drains + clears this
      return;
    }
    rawPulseCount = 0;        // too short, discard as noise
  }

  if (!rawBurstReady && rawPulseCount < RAW_BUF_LEN) {
    rawPulses[rawPulseCount++] = (delta > 0xFFFF) ? 0xFFFF : (uint16_t)delta;
  }
}

// ---------------------------------------------------------------------
// 3.3 — log format: matches the UART CSV the hub expects in 05-integration
// (type,timestamp,protocol,code,pulse_len,repeats), printed here to
// Serial now and switched to the hub UART wire later without a format
// change.
// ---------------------------------------------------------------------

static uint32_t subghzHitCount = 0;
static char     lastEventProtocol[16] = "(none)";
static unsigned long lastEventCode = 0;
static uint16_t lastEventPulse = 0;

static void logSubGhzEvent(const char* protocol, unsigned long code,
                            uint16_t pulseLength, uint8_t repeats) {
  subghzHitCount++;
  strncpy(lastEventProtocol, protocol, sizeof(lastEventProtocol) - 1);
  lastEventProtocol[sizeof(lastEventProtocol) - 1] = '\0';
  lastEventCode = code;
  lastEventPulse = pulseLength;

  Serial.print("SUBGHZ,");
  Serial.print(millis() / 1000UL);
  Serial.print(',');
  Serial.print(protocol);
  Serial.print(',');
  Serial.print(code);
  Serial.print(',');
  Serial.print(pulseLength);
  Serial.print(',');
  Serial.println(repeats);
}

static void pollSubGhz() {
  // 3.1 — rc-switch known-protocol decode
  if (rcSwitch.available()) {
    unsigned long code = rcSwitch.getReceivedValue();
    if (code != 0) {
      char protoLabel[16];
      snprintf(protoLabel, sizeof(protoLabel), "rc-switch:%d", rcSwitch.getReceivedProtocol());
      logSubGhzEvent(protoLabel, code, rcSwitch.getReceivedDelay(), 1);
    }
    rcSwitch.resetAvailable();
  }

  // 3.2 — raw fallback, only when rc-switch didn't already claim this burst
  if (rawBurstReady) {
    noInterrupts();
    uint16_t count = rawPulseCount;
    uint16_t firstPulse = count > 0 ? rawPulses[0] : 0;
    interrupts();

    // Fold the captured pulses into a coarse "code" (sum) purely so the
    // CSV `code` column has something identifying — real fallback
    // analysis happens later, offline, against the raw dump.
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
// 3.5 — control input (buttons), non-blocking debounce
// ---------------------------------------------------------------------

static const uint32_t DEBOUNCE_MS = 50;

static Button nextButton   = { NEXT_PIN,   HIGH, HIGH, 0 };
static Button selectButton = { SELECT_PIN, HIGH, HIGH, 0 };

// Returns true once, on the falling edge (button pressed, pulled LOW),
// after the new state has been stable for DEBOUNCE_MS.
static bool buttonPressed(Button& b) {
  bool reading = digitalRead(b.pin);
  uint32_t now = millis();

  if (reading != b.lastReading) {
    b.lastChangeMs = now;
    b.lastReading = reading;
  }

  if ((now - b.lastChangeMs) >= DEBOUNCE_MS && reading != b.stableState) {
    b.stableState = reading;
    return b.stableState == LOW;   // pressed
  }
  return false;
}

// ---------------------------------------------------------------------
// 3.6 — state machine: idle/status -> menu -> detail view
// ---------------------------------------------------------------------

enum ScreenState { SCREEN_IDLE, SCREEN_MENU, SCREEN_DETAIL };
static ScreenState screenState = SCREEN_IDLE;

// 3.4 stub: swap the Serial.print calls below for real LCD draw calls
// once the part is confirmed. Keeping all rendering behind this one
// function means nothing else in the state machine needs to change.
static void renderScreen() {
  switch (screenState) {
    case SCREEN_IDLE:
      Serial.print("[IDLE] sub-ghz hits: ");
      Serial.println(subghzHitCount);
      break;
    case SCREEN_MENU:
      Serial.println("[MENU] > last capture (SELECT)   back: NEXT again to cycle");
      break;
    case SCREEN_DETAIL:
      Serial.print("[DETAIL] protocol=");
      Serial.print(lastEventProtocol);
      Serial.print(" code=");
      Serial.print(lastEventCode);
      Serial.print(" pulse=");
      Serial.println(lastEventPulse);
      break;
  }
}

static void handleControls() {
  bool changed = false;

  if (buttonPressed(nextButton)) {
    screenState = (ScreenState)((screenState + 1) % 3);
    changed = true;
  }

  if (buttonPressed(selectButton) && screenState == SCREEN_MENU) {
    screenState = SCREEN_DETAIL;
    changed = true;
  }

  if (changed) renderScreen();
}

static const uint32_t IDLE_REFRESH_MS = 2000;
static uint32_t lastIdleRefreshMs = 0;

// ---------------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("Sub-GHz + control node — steps 3.1-3.3, 3.5-3.6");

  pinMode(NEXT_PIN, INPUT_PULLUP);
  pinMode(SELECT_PIN, INPUT_PULLUP);

  rcSwitch.enableReceive(digitalPinToInterrupt(RCSWITCH_PIN));

  pinMode(RAW_PIN, INPUT);
  attachInterrupt(digitalPinToInterrupt(RAW_PIN), rawEdgeISR, CHANGE);

  renderScreen();
}

void loop() {
  pollSubGhz();
  handleControls();

  uint32_t now = millis();
  if (screenState == SCREEN_IDLE && now - lastIdleRefreshMs >= IDLE_REFRESH_MS) {
    renderScreen();
    lastIdleRefreshMs = now;
  }
}
