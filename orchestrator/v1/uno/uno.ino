// CyberDeck v2 — orchestrator UNO readout (milestone 1)
//
// Dumb UART-driven display for the C3 orchestrator. Paints "draw row" (R) commands and now also
// handles two new one-way commands from the C3:
//   R<rr><c><text>  draw row rr (00-18), colour c (0-6), text        (unchanged)
//   C               clear the whole screen (sent on every mode change)
//   L<g><r>         LED state: g=green/passive (onboard D13), r=red/active (discrete pin A5)
//
// LEDs live HERE, not on the C3 (the C3 drives them over the one-way link). The LED reflects the
// last command the C3 sent, so it can go stale if the link drops — the on-screen status is the
// source of truth. Green = onboard D13 (free; SD unused). Red = discrete LED on A5 + ~330R to GND.
//
// Hardware: Arduino UNO + 3.5" mcufriend TFT shield (ILI9486/88, 8-bit parallel).
// Link:     C3 GPIO21 (TX) -> UNO D0 (RX), common GND. 19200 baud. See docs/orchestration-plan.md.
// Libraries: MCUFRIEND_kbv, Adafruit GFX.
//
// NOTE 1: power the C3 down while uploading this sketch (its bytes on D0 corrupt the bootloader).
// NOTE 2: build rows with LITERAL printf widths ("%-40.40s"); AVR printf does NOT support '*'.

#include <MCUFRIEND_kbv.h>
#include <Adafruit_GFX.h>

MCUFRIEND_kbv tft;

static const uint16_t BLACK = 0x0000;
static const uint16_t PALETTE[] = { 0xFFFF, 0x8410, 0x07E0, 0xFFE0, 0xF800, 0x07FF, 0xFD20 };
static const uint16_t GREEN = 0x07E0, RED = 0xF800, GRAY = 0x8410;

static const int ROW_H = 16, COLS = 40, MAXROWS = 19;   // rows 0-18; bottom line is link status

static const uint8_t GREEN_LED = 13;    // onboard L LED (passive)
static const uint8_t RED_LED   = A5;    // discrete LED (active/TX) + ~330R to GND

static char     line[48];
static uint8_t  linePos = 0;
static uint32_t lastValidMs = 0;
static bool     everValid = false;

static void drawRow(int rr, uint8_t c, const char* text) {
  if (rr < 0 || rr >= MAXROWS) return;
  if (c > 6) c = 0;
  char buf[COLS + 1];
  snprintf(buf, sizeof(buf), "%-40.40s", text);   // literal width (see NOTE 2)
  tft.setTextColor(PALETTE[c], BLACK);
  tft.setTextSize(2);
  tft.setCursor(0, rr * ROW_H);
  tft.print(buf);
}

static void setLeds(bool green, bool red) {
  digitalWrite(GREEN_LED, green ? HIGH : LOW);
  digitalWrite(RED_LED,   red   ? HIGH : LOW);
}

static void handleLine() {
  char cmd = line[0];
  if (cmd == 'R' && linePos >= 4) {
    int rr = (line[1] - '0') * 10 + (line[2] - '0');
    uint8_t c = line[3] - '0';
    if (rr >= 0 && rr < 20 && c <= 6) {
      everValid = true;
      lastValidMs = millis();
      drawRow(rr, c, line + 4);
    }
  } else if (cmd == 'C') {
    tft.fillScreen(BLACK);
    everValid = true;
    lastValidMs = millis();
  } else if (cmd == 'L' && linePos >= 3) {
    setLeds(line[1] == '1', line[2] == '1');
    everValid = true;
    lastValidMs = millis();
  }
}

static void linkStatus() {
  bool live = everValid && (millis() - lastValidMs < 2000);
  const char* s = !everValid ? "waiting for C3..." : (live ? "C3 link: LIVE" : "C3 link: LOST");
  char pad[41];
  snprintf(pad, sizeof(pad), "%-40.40s", s);
  tft.setTextSize(1);
  tft.setTextColor(!everValid ? GRAY : (live ? GREEN : RED), BLACK);
  tft.setCursor(0, 306);
  tft.print(pad);
}

void setup() {
  Serial.begin(19200);
  pinMode(GREEN_LED, OUTPUT);
  pinMode(RED_LED, OUTPUT);
  setLeds(false, false);

  uint16_t id = tft.readID();
  if (id == 0x0 || id == 0xFFFF) id = 0x9486;   // fallback for shields that don't report an ID
  tft.begin(id);
  tft.setRotation(1);                            // 480x320 landscape
  tft.fillScreen(BLACK);
}

void loop() {
  while (Serial.available()) {
    char ch = (char)Serial.read();
    if (ch == '\n' || ch == '\r') {
      line[linePos] = '\0';
      if (linePos > 0) handleLine();
      linePos = 0;
    } else if (linePos < (int)sizeof(line) - 1) {
      line[linePos++] = ch;
    } else {
      linePos = 0;   // overlong garbage, resync on the next newline
    }
  }

  static uint32_t lastStatus = 0;
  if (millis() - lastStatus >= 500) { lastStatus = millis(); linkStatus(); }
}
