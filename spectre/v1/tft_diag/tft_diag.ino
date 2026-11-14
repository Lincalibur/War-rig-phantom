// SPECTRE TFT bring-up diagnostic (parallel ILI9486/88 shield).
//
// Isolates the display from the whole orchestrator: boots, reads the controller ID over the
// parallel bus, prints it on Serial @115200, then cycles the screen RED -> GREEN -> BLUE with a
// label. Uses the same TFT_eSPI User_Setup the main firmware uses (ILI9486 parallel).
//
// What the result tells us:
//   - Serial prints at all            -> the ESP32 boots and runs (not a strapping/brownout lockup)
//   - "RDID 0xD3 -> 00 94 86"         -> controller is ILI9486 (current setup is correct)
//   - "RDID 0xD3 -> 00 94 88"         -> it's ILI9488: switch the driver in User_Setup_SPECTRE.h
//   - "RDID 0xD3 -> 00 00 00 / FF.."  -> the READ line (RD=GPIO2) or a data line isn't connected
//   - colour bars appear              -> the panel + wiring work; the app just needs the right driver
//   - still white with good Serial ID -> wrong driver for that ID, or a control line (RST/WR/CS) is off

#include <TFT_eSPI.h>
TFT_eSPI tft;

static void readId(const char* tag, uint8_t cmd) {
  uint8_t b1 = tft.readcommand8(cmd, 1);
  uint8_t b2 = tft.readcommand8(cmd, 2);
  uint8_t b3 = tft.readcommand8(cmd, 3);
  uint8_t b4 = tft.readcommand8(cmd, 4);
  Serial.printf("%s 0x%02X -> %02X %02X %02X %02X\n", tag, cmd, b1, b2, b3, b4);
}

void setup() {
  Serial.begin(115200);
  delay(800);
  Serial.println();
  Serial.println("================ SPECTRE TFT DIAG ================");
  Serial.println("[diag] ESP32 booted, sketch is running.");

  tft.init();
  Serial.println("[diag] tft.init() returned.");

  readId("RDID", 0xD3);     // ILI9486 -> 00 94 86, ILI9488 -> 00 94 88
  readId("RDDID", 0x04);    // manufacturer/driver id (varies)

  tft.setRotation(0);       // portrait (vertical)
  Serial.printf("[diag] width=%d height=%d\n", tft.width(), tft.height());
  Serial.println("[diag] cycling colours RED/GREEN/BLUE on the panel...");
}

void loop() {
  uint8_t d3a = tft.readcommand8(0xD3, 2), d3b = tft.readcommand8(0xD3, 3);
  Serial.println("--------------------------------------------------");
  Serial.printf(">>> CONTROLLER ID 0xD3=[%02X %02X]\n", d3a, d3b);

  // WRITE-THEN-READBACK: fill the screen, then read a pixel back out of display memory.
  // If the read-back colour matches, writes ARE reaching the controller (so it's an init/display
  // problem). If it reads white/black/garbage, the write path (WR or a data line) is dropping bytes.
  struct C { uint16_t v; const char* n; } cs[] = {
    {TFT_RED,"RED"}, {TFT_GREEN,"GREEN"}, {TFT_BLUE,"BLUE"}, {TFT_WHITE,"WHITE"}, {TFT_BLACK,"BLACK"} };
  static int i = 0;
  C c = cs[i];
  tft.fillScreen(c.v);
  tft.setTextColor(TFT_YELLOW, c.v); tft.setTextSize(4); tft.setCursor(16, 24); tft.print(c.n);
  uint16_t rb = tft.readPixel(40, 40);
  Serial.printf("    wrote %-5s (0x%04X)  read back 0x%04X  ", c.n, c.v, rb);
  if (rb == c.v)             Serial.println("=> MATCH: writes reach GRAM. Issue is init/display-on/inversion, NOT wiring.");
  else if (rb == 0xFFFF)     Serial.println("=> reads WHITE: write path dropping bytes (WR/data jumper).");
  else if (rb == 0x0000)     Serial.println("=> reads BLACK: write path dropping bytes (WR/data jumper).");
  else                       Serial.println("=> MISMATCH/garbage: marginal WR or a data line.");
  Serial.printf("    panel should look: %s\n", c.n);
  i = (i + 1) % 5;
  delay(2500);
}
