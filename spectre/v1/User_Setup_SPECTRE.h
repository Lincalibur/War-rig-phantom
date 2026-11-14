// TFT_eSPI User_Setup for the SPECTRE HUB — 3.5" ILI9486 parallel (mcufriend-style) UNO shield.
//
// Deploy: copy this over  <Arduino>/libraries/TFT_eSPI/User_Setup.h  (User_Setup_Select.h already
// includes <User_Setup.h> by default), then rebuild spectre/v1/spectre.
//
// The shield is 8-bit PARALLEL. All 8 data pins must be on GPIOs < 32 so one register write drives
// the bus. Pins are the SPECTRE wiring (see docs/hardware-layout.md).
//
// Controller: this red 3.5" shield is almost always ILI9486 (320x480). If the screen stays white or
// shows wrong colours, try the alternates below one at a time (ILI9488, then HX8357 / ILI9481).

#define USER_SETUP_INFO "SPECTRE Parallel ILI9486"

// ---- Driver (uncomment ONE) ----
// Confirmed by the diag read on 2026-11-14: RDID 0xD3 -> 94 88 = ILI9488.
//#define ILI9486_DRIVER
#define ILI9488_DRIVER
//#define HX8357D_DRIVER
//#define ILI9481_DRIVER
//#define ILI9341_DRIVER     // 2.4"/2.8" panels only

// ---- 8-bit parallel bus on the ESP32 ----
#define TFT_PARALLEL_8_BIT

#define TFT_WIDTH  320
#define TFT_HEIGHT 480

// Control lines
#define TFT_CS   33   // LCD_CS (library pulls permanently low)
#define TFT_DC   15   // LCD_RS / DC
#define TFT_RST  32   // LCD_RST
#define TFT_WR    4   // LCD_WR write strobe
#define TFT_RD    2   // LCD_RD read strobe

// Data bus (all < 32)
#define TFT_D0   12
#define TFT_D1   13
#define TFT_D2   26
#define TFT_D3   25
#define TFT_D4   17
#define TFT_D5   16
#define TFT_D6   27
#define TFT_D7   14

// Backlight is hardwired to VCC on this shield — no TFT_BL.

// ---- Fonts ----
#define LOAD_GLCD
#define LOAD_FONT2
#define LOAD_FONT4
#define LOAD_FONT6
#define LOAD_FONT7
#define LOAD_FONT8
#define LOAD_GFXFF
#define SMOOTH_FONT
