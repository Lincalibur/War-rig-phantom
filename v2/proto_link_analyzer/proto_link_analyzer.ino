// CyberDeck v2 — UNO link analyzer (bring-up diagnostic ONLY)
//
// Flash this on the UNO instead of the readout to characterize the C3 -> UNO link. It never
// leaves the stats screen, so we can read exactly what is happening on the wire:
//
//   RX bytes / bytes/s : is data flowing at all, and steadily or in bursts?
//   ctrl bytes         : non-printable bytes. HIGH => wrong baud / garbage (e.g. the C3's
//                        115200 ROM boot log on GPIO21 during a brown-out reboot loop).
//   lines / valid R    : how many complete lines parse as valid "R" row commands.
//   R per s            : steady ~50/s => healthy stream;  0 or occasional => link/power fault.
//   last R             : seconds since the last valid frame.
//   raw stream         : the actual text (printable chars) coming across.
//
// Reading it:
//   bytes/s ~0, ctrl ~0           -> no data: dead/flaky D0 tap, GND, or C3 not transmitting.
//   bytes climb, ctrl HIGH        -> baud mismatch or boot-loop garbage (C3 resetting).
//   bytes climb, valid R ticking  -> link is GOOD; the problem was elsewhere (readout logic).
//   one valid R then frozen       -> C3 sent once then died (brown-out / crash).
//
// Hardware: Arduino UNO + 3.5" mcufriend TFT. Link: C3 GPIO21 -> UNO D0, common GND, 19200.
// Libraries: MCUFRIEND_kbv, Adafruit GFX.

#include <MCUFRIEND_kbv.h>
#include <Adafruit_GFX.h>

MCUFRIEND_kbv tft;
static const uint16_t BLACK = 0x0000, WHITE = 0xFFFF, GRAY = 0x8410, GREEN = 0x07E0,
                      YELLOW = 0xFFE0, CYAN = 0x07FF, RED = 0xF800;

static char     line[48];
static uint8_t  linePos = 0;
static uint32_t rxBytes = 0, ctrlBytes = 0, lastValidMs = 0;
static uint16_t lines = 0, validR = 0, badLines = 0;
static uint16_t bps = 0, rps = 0;
static uint32_t pB = 0; static uint16_t pR = 0, pL = 0, lps = 0;
static bool     everValid = false;

static const int LOG_N = 12, LOG_W = 52;
static char logbuf[LOG_N][LOG_W + 1];
static int  logHead = 0;
static bool dirtyLog = true;

static void pushLog(const char* s) {
  logHead = (logHead + 1) % LOG_N;
  snprintf(logbuf[logHead], LOG_W + 1, "%s", s);
  dirtyLog = true;
}

static void kv(int y, const char* label, uint32_t v, uint16_t col) {
  char b[36];
  snprintf(b, sizeof(b), "%s%-9lu", label, (unsigned long)v);
  tft.setTextSize(2); tft.setTextColor(col, BLACK); tft.setCursor(0, y); tft.print(b);
}

static void handleLine() {
  lines++;
  pushLog(line);
  if (line[0] == 'R' && linePos >= 4) {
    int rr = (line[1] - '0') * 10 + (line[2] - '0');
    uint8_t c = line[3] - '0';
    if (rr >= 0 && rr < 20 && c <= 6) { validR++; everValid = true; lastValidMs = millis(); return; }
  }
  badLines++;
}

static void drawConsole() {
  tft.fillRect(0, 196, 480, 124, BLACK);
  tft.setTextSize(1); tft.setTextColor(YELLOW, BLACK); tft.setCursor(0, 196);
  tft.print("raw stream from C3 (printable):");
  for (int i = 0; i < LOG_N; i++) {
    int idx = (logHead + 1 + i) % LOG_N;
    tft.setTextColor(GREEN, BLACK); tft.setCursor(0, 208 + i * 9);
    char b[LOG_W + 1]; snprintf(b, sizeof(b), "%-*.*s", LOG_W, LOG_W, logbuf[idx]);
    tft.print(b);
  }
  dirtyLog = false;
}

void setup() {
  Serial.begin(19200);
  uint16_t id = tft.readID();
  if (id == 0x0 || id == 0xFFFF) id = 0x9486;
  tft.begin(id);
  tft.setRotation(1);
  tft.fillScreen(BLACK);
  tft.setTextSize(2); tft.setTextColor(CYAN, BLACK); tft.setCursor(0, 0);
  tft.print("C3 LINK ANALYZER");
  for (int i = 0; i < LOG_N; i++) logbuf[i][0] = '\0';
}

void loop() {
  while (Serial.available()) {
    char ch = (char)Serial.read();
    rxBytes++;
    if (ch == '\n' || ch == '\r') {
      line[linePos] = '\0';
      if (linePos > 0) handleLine();
      linePos = 0;
    } else {
      if ((uint8_t)ch < 0x20) ctrlBytes++;
      if (linePos < (int)sizeof(line) - 1) line[linePos++] = ch; else linePos = 0;
    }
  }

  static uint32_t lastSec = 0, lastDraw = 0;
  uint32_t now = millis();
  if (now - lastSec >= 1000) {
    lastSec = now;
    bps = (uint16_t)(rxBytes - pB); pB = rxBytes;
    rps = validR - pR; pR = validR;
    lps = lines - pL; pL = lines;
  }
  if (now - lastDraw >= 250) {
    lastDraw = now;
    kv(24,  "RX bytes:", rxBytes, WHITE);
    kv(44,  "bytes/s :", bps, bps ? GREEN : GRAY);
    kv(64,  "ctrl by :", ctrlBytes, ctrlBytes > rxBytes / 4 ? RED : GRAY);
    kv(84,  "lines   :", lines, WHITE);
    kv(104, "lines/s :", lps, WHITE);
    kv(124, "valid R :", validR, validR ? GREEN : RED);
    kv(144, "R per s :", rps, rps ? GREEN : (everValid ? RED : GRAY));
    kv(164, "bad line:", badLines, badLines ? YELLOW : GRAY);
    uint32_t ago = everValid ? (now - lastValidMs) / 1000 : 99999;
    kv(184, "lastR  s:", ago, ago < 3 ? GREEN : RED);
    if (dirtyLog) drawConsole();
  }
}
