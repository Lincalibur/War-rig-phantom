// CyberDeck v2 — bench prototype: UNO big-screen readout
//
// Pairs with v2/proto_readout_c3. The ESP32-C3 scans WiFi (promiscuous) and streams "draw this
// row" commands over a one-way 19200-baud UART link; this UNO + 3.5" TFT just paints them. The
// UNO holds no tables, so its 2 KB RAM is never the bottleneck.
//
// Rows (set by the C3): 0 title, 1 live status, 2 AP header, 3-10 APs, 11 client header,
// 12-18 clients (sorted by persistence = "following me" candidates). A slim bottom line shows
// C3 link health (LIVE / LINK LOST), drawn by the UNO itself.
//
// Hardware: Arduino UNO + 3.5" mcufriend TFT shield (ILI9486/88, 8-bit parallel, microSD).
// Link:     C3 GPIO21 (TX) -> UNO D0 (RX), common GND. 19200 baud. See docs/proto-readout-wiring.html.
// Libraries: MCUFRIEND_kbv, Adafruit GFX.  (SD logging is the next addition — slot is on the shield.)
//
// NOTE 1: unplug the C3 -> D0 wire while uploading a sketch to the UNO (D0 is the USB RX).
// NOTE 2: build rows with LITERAL printf widths ("%-40.40s"); AVR printf does NOT support the
//         '*' dynamic-width specifier and silently renders nothing (this cost a long debug).

#include <MCUFRIEND_kbv.h>
#include <Adafruit_GFX.h>

MCUFRIEND_kbv tft;

static const uint16_t BLACK = 0x0000;
static const uint16_t PALETTE[] = { 0xFFFF, 0x8410, 0x07E0, 0xFFE0, 0xF800, 0x07FF, 0xFD20 };
static const uint16_t GREEN = 0x07E0, RED = 0xF800, GRAY = 0x8410;

static const int ROW_H = 16, COLS = 40, MAXROWS = 19;   // rows 0-18; bottom line is the link status

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

static void handleLine() {
  if (line[0] == 'R' && linePos >= 4) {
    int rr = (line[1] - '0') * 10 + (line[2] - '0');
    uint8_t c = line[3] - '0';
    if (rr >= 0 && rr < 20 && c <= 6) {
      everValid = true;
      lastValidMs = millis();
      drawRow(rr, c, line + 4);
    }
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
