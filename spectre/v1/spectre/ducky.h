// CyberDeck — BLE HID keyboard + DuckyScript engine (for the BLE_HID_TRAP mode)
//
// Turns the C3 into a BLE HID keyboard (HID-over-GATT). When a host pairs, it runs one of the
// embedded DuckyScripts (default: rickroll). Scripts are embedded in flash (no SD on this build).
//
// Included by c3.ino AFTER <NimBLEDevice.h>. Exposes: duckyStart/duckyStop, duckyVictimReady,
// duckyRunSelected, and the g_ducky* state the UNO screen reads. Standard Windows DuckyScript:
// REM, DEFAULT_DELAY, DELAY, STRING, ENTER, and key combos (GUI r / CTRL ALT t / F11 / etc.).
//
// Scope note: this is an active HID-injection tool — own-lab / consenting-demo use only.

#pragma once
#include <NimBLEHIDDevice.h>

// ---- tunables ----
static const uint16_t DUCKY_KEYDELAY = 10;     // ms between key down/up
static const uint32_t DUCKY_PAIR_MS  = 2500;   // wait after connect for pairing/encryption

// ---- embedded scripts (index 0 = default) ----
struct DuckyScript { const char* name; const char* body; };
static const DuckyScript DUCKY[] = {
  // index 0 = default. KDE/Kubuntu target: KRunner (Alt+F2) runs xdg-open to launch the browser.
  { "Rickroll KDE",
    "DEFAULT_DELAY 80\nDELAY 1500\nALT F2\nDELAY 900\n"
    "STRING xdg-open https://youtu.be/dQw4w9WgXcQ\nDELAY 300\nENTER\n" },
  { "Rickroll Win",
    "DEFAULT_DELAY 90\nDELAY 1200\nGUI r\nDELAY 450\n"
    "STRING https://youtu.be/dQw4w9WgXcQ\nENTER\n" },
  { "Notepad Hello",
    "DEFAULT_DELAY 80\nDELAY 1000\nGUI r\nDELAY 400\nSTRING notepad\nENTER\n"
    "DELAY 600\nSTRING Hello from CyberDeck BLE Ducky\nENTER\n" },
  { "Fake Update",
    "DELAY 1000\nGUI r\nDELAY 500\nSTRING chrome\nENTER\nDELAY 1500\n"
    "STRING https://fakeupdate.net/wnc/\nENTER\nDELAY 2000\nF11\n" },
  { "Lock Screen",
    "DELAY 800\nGUI l\n" },
  { "Task Manager",
    "DELAY 1000\nCTRL SHIFT ESC\n" },
  { "Hacker CMD",
    "DEFAULT_DELAY 70\nDELAY 1000\nGUI r\nDELAY 400\nSTRING cmd\nENTER\nDELAY 500\n"
    "STRING color 0a\nENTER\nSTRING echo Initializing neural handshake...\nENTER\nDELAY 400\n"
    "STRING echo Just kidding - harmless echo text.\nENTER\nSTRING whoami\nENTER\n" },
};
static const int DUCKY_COUNT = sizeof(DUCKY) / sizeof(DUCKY[0]);

int           g_duckyScript = 0;   // selected index
volatile int  g_duckyState  = 0;   // 0 arming, 1 paired, 2 running, 3 done
bool          g_duckyRan    = false;

static const char* duckyName(int i) { return (i >= 0 && i < DUCKY_COUNT) ? DUCKY[i].name : "?"; }
static int         duckyCount()     { return DUCKY_COUNT; }

// ---------------------------------------------------------------------------------------
// HID keyboard descriptor (report ID 1, 8-byte reports)
// ---------------------------------------------------------------------------------------
static const uint8_t HID_REPORT_MAP[] = {
  0x05,0x01, 0x09,0x06, 0xA1,0x01,
  0x85,0x01,
  0x05,0x07, 0x19,0xE0, 0x29,0xE7, 0x15,0x00, 0x25,0x01, 0x75,0x01, 0x95,0x08, 0x81,0x02,
  0x95,0x01, 0x75,0x08, 0x81,0x01,
  0x95,0x06, 0x75,0x08, 0x15,0x00, 0x25,0x65, 0x05,0x07, 0x19,0x00, 0x29,0x65, 0x81,0x00,
  0xC0
};

// modifier bits
#define MOD_CTRL  0x01
#define MOD_SHIFT 0x02
#define MOD_ALT   0x04
#define MOD_GUI   0x08

static NimBLEServer*         hidSrv   = nullptr;
static NimBLEHIDDevice*      hidDev   = nullptr;
static NimBLECharacteristic* hidInput = nullptr;
static bool                  hidUp    = false;
static bool                  hidTearing = false;
static uint32_t              hidRunAt = 0;

// ---- ASCII -> HID usage (US layout) ----
static bool asciiToHid(char c, uint8_t& mod, uint8_t& key) {
  mod = 0; key = 0;
  if (c >= 'a' && c <= 'z') { key = 0x04 + (c - 'a'); return true; }
  if (c >= 'A' && c <= 'Z') { mod = MOD_SHIFT; key = 0x04 + (c - 'A'); return true; }
  if (c >= '1' && c <= '9') { key = 0x1E + (c - '1'); return true; }
  if (c == '0') { key = 0x27; return true; }
  switch (c) {
    case ' ':  key = 0x2C; return true;
    case '\n': key = 0x28; return true;
    case '\t': key = 0x2B; return true;
    case '-':  key = 0x2D; return true;   case '_': mod = MOD_SHIFT; key = 0x2D; return true;
    case '=':  key = 0x2E; return true;   case '+': mod = MOD_SHIFT; key = 0x2E; return true;
    case '[':  key = 0x2F; return true;   case '{': mod = MOD_SHIFT; key = 0x2F; return true;
    case ']':  key = 0x30; return true;   case '}': mod = MOD_SHIFT; key = 0x30; return true;
    case '\\': key = 0x31; return true;   case '|': mod = MOD_SHIFT; key = 0x31; return true;
    case ';':  key = 0x33; return true;   case ':': mod = MOD_SHIFT; key = 0x33; return true;
    case '\'': key = 0x34; return true;   case '"': mod = MOD_SHIFT; key = 0x34; return true;
    case '`':  key = 0x35; return true;   case '~': mod = MOD_SHIFT; key = 0x35; return true;
    case ',':  key = 0x36; return true;   case '<': mod = MOD_SHIFT; key = 0x36; return true;
    case '.':  key = 0x37; return true;   case '>': mod = MOD_SHIFT; key = 0x37; return true;
    case '/':  key = 0x38; return true;   case '?': mod = MOD_SHIFT; key = 0x38; return true;
    case '!':  mod = MOD_SHIFT; key = 0x1E; return true;
    case '@':  mod = MOD_SHIFT; key = 0x1F; return true;
    case '#':  mod = MOD_SHIFT; key = 0x20; return true;
    case '$':  mod = MOD_SHIFT; key = 0x21; return true;
    case '%':  mod = MOD_SHIFT; key = 0x22; return true;
    case '^':  mod = MOD_SHIFT; key = 0x23; return true;
    case '&':  mod = MOD_SHIFT; key = 0x24; return true;
    case '*':  mod = MOD_SHIFT; key = 0x25; return true;
    case '(':  mod = MOD_SHIFT; key = 0x26; return true;
    case ')':  mod = MOD_SHIFT; key = 0x27; return true;
  }
  return false;
}

// ---- token (ENTER, GUI, F11, ...) -> modifier bit or key code ----
static bool tokenIsModifier(const char* t, uint8_t& bit) {
  if (!strcasecmp(t, "CTRL") || !strcasecmp(t, "CONTROL")) { bit = MOD_CTRL;  return true; }
  if (!strcasecmp(t, "SHIFT"))                              { bit = MOD_SHIFT; return true; }
  if (!strcasecmp(t, "ALT"))                                { bit = MOD_ALT;   return true; }
  if (!strcasecmp(t, "GUI") || !strcasecmp(t, "WINDOWS") ||
      !strcasecmp(t, "WIN") || !strcasecmp(t, "COMMAND"))   { bit = MOD_GUI;   return true; }
  return false;
}
static uint8_t keyNameToHid(const char* t) {
  if (strlen(t) == 1) { uint8_t m, k; if (asciiToHid(t[0], m, k)) return k; return 0; }
  if (!strcasecmp(t, "ENTER") || !strcasecmp(t, "RETURN")) return 0x28;
  if (!strcasecmp(t, "ESC")   || !strcasecmp(t, "ESCAPE")) return 0x29;
  if (!strcasecmp(t, "BACKSPACE")) return 0x2A;
  if (!strcasecmp(t, "TAB"))       return 0x2B;
  if (!strcasecmp(t, "SPACE"))     return 0x2C;
  if (!strcasecmp(t, "CAPSLOCK"))  return 0x39;
  if (!strcasecmp(t, "DELETE") || !strcasecmp(t, "DEL")) return 0x4C;
  if (!strcasecmp(t, "INSERT"))    return 0x49;
  if (!strcasecmp(t, "HOME"))      return 0x4A;
  if (!strcasecmp(t, "END"))       return 0x4D;
  if (!strcasecmp(t, "PAGEUP"))    return 0x4B;
  if (!strcasecmp(t, "PAGEDOWN"))  return 0x4E;
  if (!strcasecmp(t, "UP")    || !strcasecmp(t, "UPARROW"))    return 0x52;
  if (!strcasecmp(t, "DOWN")  || !strcasecmp(t, "DOWNARROW"))  return 0x51;
  if (!strcasecmp(t, "LEFT")  || !strcasecmp(t, "LEFTARROW"))  return 0x50;
  if (!strcasecmp(t, "RIGHT") || !strcasecmp(t, "RIGHTARROW")) return 0x4F;
  if (!strcasecmp(t, "PRINTSCREEN")) return 0x46;
  if ((t[0] == 'F' || t[0] == 'f') && strlen(t) >= 2) {       // F1..F12
    int n = atoi(t + 1);
    if (n >= 1 && n <= 12) return 0x3A + (n - 1);
  }
  return 0;
}

// ---- send one key chord ----
static void hidPress(uint8_t mod, uint8_t key) {
  if (!hidInput) return;
  uint8_t r[8]   = { mod, 0, key, 0, 0, 0, 0, 0 };
  hidInput->setValue(r, 8);  hidInput->notify();  delay(DUCKY_KEYDELAY);
  uint8_t rel[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
  hidInput->setValue(rel, 8); hidInput->notify(); delay(DUCKY_KEYDELAY);
}
static void hidTypeString(const char* s) {
  for (const char* p = s; *p; ++p) { uint8_t m, k; if (asciiToHid(*p, m, k)) hidPress(m, k); }
}

// ---- execute one ducky line ----
static uint32_t g_defaultDelay = 0;
static void duckyExecLine(const char* line) {
  while (*line == ' ' || *line == '\t') line++;
  if (!*line) return;

  // first token
  char cmd[16]; int ci = 0;
  const char* p = line;
  while (*p && *p != ' ' && ci < 15) cmd[ci++] = *p++;
  cmd[ci] = 0;
  const char* rest = (*p == ' ') ? p + 1 : p;   // remainder (preserves case/spaces)

  if (!strcasecmp(cmd, "REM")) return;
  if (!strcasecmp(cmd, "DEFAULT_DELAY") || !strcasecmp(cmd, "DEFAULTDELAY")) {
    g_defaultDelay = atol(rest); return;
  }
  if (!strcasecmp(cmd, "DELAY") || !strcasecmp(cmd, "WAIT_FOR")) { delay(atol(rest)); return; }
  if (!strcasecmp(cmd, "STRING"))   { hidTypeString(rest); delay(g_defaultDelay); return; }
  if (!strcasecmp(cmd, "STRINGLN")) { hidTypeString(rest); hidPress(0, 0x28); delay(g_defaultDelay); return; }

  // otherwise: a key chord (ENTER / GUI r / CTRL ALT t / F11 / ...)
  char buf[64]; strncpy(buf, line, sizeof(buf) - 1); buf[sizeof(buf) - 1] = 0;
  uint8_t mod = 0, key = 0;
  for (char* tok = strtok(buf, " "); tok; tok = strtok(nullptr, " ")) {
    uint8_t bit;
    if (tokenIsModifier(tok, bit)) mod |= bit;
    else { uint8_t k = keyNameToHid(tok); if (k) key = k; }
  }
  if (mod || key) hidPress(mod, key);
  delay(g_defaultDelay);
}

static void duckyRunBody(const char* body) {
  g_defaultDelay = 0;
  char line[96]; int li = 0;
  for (const char* p = body; ; ++p) {
    if (*p == '\n' || *p == 0) {
      line[li] = 0;
      if (li > 0) duckyExecLine(line);
      li = 0;
      if (*p == 0) break;
    } else if (*p != '\r' && li < (int)sizeof(line) - 1) {
      line[li++] = *p;
    }
  }
}

// ---------------------------------------------------------------------------------------
// HID server lifecycle
// ---------------------------------------------------------------------------------------
class HidSrvCB : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* s, NimBLEConnInfo& info) override {
    g_duckyState = 1; g_duckyRan = false; hidRunAt = millis() + DUCKY_PAIR_MS;
  }
  void onDisconnect(NimBLEServer* s, NimBLEConnInfo& info, int reason) override {
    g_duckyState = 0; g_duckyRan = false;
    if (!hidTearing) NimBLEDevice::startAdvertising();
  }
};

static void duckyStart(const char* baitName) {
  if (hidUp) return;
  g_duckyState = 0; g_duckyRan = false;
  NimBLEDevice::init(baitName);
  NimBLEDevice::setSecurityAuth(true, false, true);          // bond, no MITM -> just-works pairing
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);

  hidSrv = NimBLEDevice::createServer();
  hidSrv->setCallbacks(new HidSrvCB());
  hidDev = new NimBLEHIDDevice(hidSrv);
  hidInput = hidDev->getInputReport(1);
  hidDev->setManufacturer("CyberDeck");
  hidDev->setPnp(0x02, 0xE502, 0xA111, 0x0210);
  hidDev->setHidInfo(0x00, 0x01);
  hidDev->setReportMap((uint8_t*)HID_REPORT_MAP, sizeof(HID_REPORT_MAP));
  hidDev->setBatteryLevel(100);
  hidDev->getHidService()->start();
  hidDev->getDeviceInfoService()->start();
  hidDev->getBatteryService()->start();

  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->setAppearance(0x03C1);                                // HID keyboard
  adv->addServiceUUID(hidDev->getHidService()->getUUID());
  adv->enableScanResponse(true);
  adv->start();
  hidUp = true;
}

static void duckyStop() {
  if (!hidUp) return;
  hidTearing = true;
  NimBLEDevice::deinit(true);
  hidSrv = nullptr; hidDev = nullptr; hidInput = nullptr;
  hidUp = false; hidTearing = false; g_duckyState = 0; g_duckyRan = false;
}

// true when a host is paired and the pairing settle time has elapsed
static bool duckyVictimReady() {
  return hidUp && g_duckyState >= 1 && !g_duckyRan && millis() >= hidRunAt;
}
static void duckyRunSelected() {
  if (g_duckyScript < 0 || g_duckyScript >= DUCKY_COUNT) g_duckyScript = 0;
  duckyRunBody(DUCKY[g_duckyScript].body);
}
