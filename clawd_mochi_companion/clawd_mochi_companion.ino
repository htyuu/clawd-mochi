/*
 * ╔══════════════════════════════════════════════════════════════╗
 *   CLAWD MOCHI COMPANION — ESP32-C3 Super Mini + ST7789 1.54"
 *
 *   Upgraded version that connects to Claude Code via Python daemon.
 *   Adds: AP+STA dual mode, multi-WiFi auto-switch (WiFiMulti),
 *         Status view with Claude state, mood-light backgrounds,
 *         daily stats, NVS-persisted WiFi credentials.
 *
 *   Original Clawd Mochi features (eyes/squish/code/canvas) preserved.
 *
 *   Wiring (same as original):
 *     SDA → GPIO 10  (hardware SPI MOSI)
 *     SCL → GPIO 8   (hardware SPI SCK)
 *     RST → GPIO 2
 *     DC  → GPIO 1
 *     CS  → GPIO 4
 *     BL  → GPIO 3
 *     VCC → 3V3
 *     GND → GND
 *
 *   WiFi AP: "ClaWD-Mochi"  pw: clawd1234  → http://192.168.4.1
 *   STA    : connects to saved networks (up to 5), strongest first
 * ╚══════════════════════════════════════════════════════════════╝
 */

#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <math.h>
#include <time.h>
#include <WiFi.h>
#include <WiFiMulti.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <DNSServer.h>      // Captive portal
#include <ArduinoOTA.h>     // OTA wireless update
#include <ESPmDNS.h>        // advertise clawd-mochi.local (network-agnostic hostname)

// ── Pins ──────────────────────────────────────────────────────
#define TFT_CS  4
#define TFT_DC  1
#define TFT_RST 2
#define TFT_BLK 3

Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_RST);

// ── WiFi ──────────────────────────────────────────────────────
const char* AP_SSID = "ClaWD-Mochi";
const char* AP_PASS = "clawd1234";

// Home / fallback networks: loaded from wifi_secrets.h (gitignored, so the
// real SSIDs/passwords never enter git). Generate it with
// scripts/gen_wifi_secrets.sh from WIFI_SSID_N/WIFI_PASS_N env vars, or copy
// wifi_secrets.example.h → wifi_secrets.h and fill in by hand.
#if __has_include("wifi_secrets.h")
  #include "wifi_secrets.h"
#else
  #error "wifi_secrets.h not found. Run scripts/gen_wifi_secrets.sh, or copy wifi_secrets.example.h to wifi_secrets.h and fill in your WiFi credentials."
#endif
WebServer server(80);

// Multi-WiFi: up to 5 saved networks, auto-select strongest
#define MAX_WIFI_CREDS 5
WiFiMulti wifiMulti;
Preferences prefs;            // NVS storage
uint32_t lastWifiScanMs = 0;
#define WIFI_SCAN_INTERVAL_MS 300000UL   // 5 min: rescan + reconnect to strongest

// Captive portal — DNS that resolves anything to AP IP
DNSServer dnsServer;
#define DNS_PORT 53

// ── Mood-light colours (per Claude state) ─────────────────────
// Filled at boot in initColours(). RGB565.
uint16_t MOOD_IDLE, MOOD_THINKING, MOOD_AWAITING, MOOD_DONE, MOOD_ERROR;

// ── Display ───────────────────────────────────────────────────
#define DISP_W 320
#define DISP_H 240

// ── Eye constants (shared by both eye views) ──────────────────
#define EYE_W   30
#define EYE_H   60
#define EYE_GAP 120
#define EYE_OX  0     // horizontal offset
#define EYE_OY  40    // vertical offset upward (subtracted from centre)

// ── Colours ───────────────────────────────────────────────────
uint16_t C_ORANGE, C_DARKBG, C_MUTED, C_GREEN;
#define C_WHITE ST77XX_WHITE
#define C_BLACK ST77XX_BLACK

// ── State ─────────────────────────────────────────────────────
#define VIEW_EYES_NORMAL 0
#define VIEW_EYES_SQUISH 1
#define VIEW_CODE        2
#define VIEW_DRAW        3
#define VIEW_STATUS      4   // NEW: shows Claude work status
#define VIEW_SCREENSAVER 5   // NEW: day/night cycle screensaver

uint8_t  currentView  = VIEW_EYES_NORMAL;
uint8_t  previousView = VIEW_EYES_NORMAL;  // for auto-switch revert
bool     busy         = false;
bool     backlightOn  = true;
uint8_t  animSpeed    = 1;   // 1=slow(default) 2=normal 3=fast

uint16_t animBgColor  = 0;   // background for eye/logo animations
uint16_t drawBgColor  = 0;   // background for canvas

// ── Claude state machine ──────────────────────────────────────
// Enum values are also priorities (higher = takes precedence)
enum ClaudeState {
  CS_IDLE     = 0,
  CS_THINKING = 1,
  CS_DONE     = 2,
  CS_AWAITING = 3,
  CS_ERROR    = 4
};

struct ClaudeStatus {
  ClaudeState state;
  String   tool;          // "Edit" / "Bash" / "Read" ...
  String   task;          // current TodoWrite task title
  uint32_t duration_s;    // time the current tool/task has been running
  uint32_t tokens_used;
  uint32_t tokens_max;
  String   git_branch;
  String   project;
  String   model;         // "sonnet" / "opus" / "haiku"
  uint32_t session_duration_s; // total session wall-clock time
  uint16_t tool_count;        // tool call count this session
  uint32_t last_update_ms;  // for daemon-offline detection
};

ClaudeStatus claudeStatus = {
  CS_IDLE, "", "", 0, 0, 200000, "", "", "", 0, 0, 0
};

bool     moodLightEnabled = true;   // toggle from Web UI
bool     autoSwitchEnabled = true;  // auto switch to Status on important events

// Pending lower-priority status (for priority merging)
ClaudeStatus pendingStatus;
bool        hasPendingStatus = false;

uint32_t errorStartMs     = 0;
uint32_t doneStartMs      = 0;
uint32_t awaitingStartMs  = 0;
#define  ERROR_TIMEOUT_MS 5000
#define  DONE_TIMEOUT_MS  3000
#define  AWAITING_TIMEOUT_MS 30000UL   // awaiting auto-reverts to idle after 30s
#define  DAEMON_OFFLINE_TIMEOUT_MS 60000UL

// Screensaver / idle
#define SCREENSAVER_TIMEOUT_MS 120000UL  // 2 min
uint32_t lastInteractionMs = 0;
uint8_t  idleExpression    = 0;   // 0=normal 1=sleepy 2=puzzled 3=happy
uint32_t lastExpressionChangeMs = 0;
#define EXPRESSION_INTERVAL_MS 15000UL  // 15s between expression changes

// Status-view partial redraw: avoid full-screen fillScreen every frame
// (causes flicker on 240x320). Track whether the static frame is drawn
// and which bg color it was drawn with, so we only repaint the eye area
// + token bar on animation ticks.
bool     statusFrameDrawn   = false;
uint16_t statusFrameBgColor = 0;

// Eye dirty-tracking (defined at top so drawStatusView()'s auto-generated
// prototype can reference them — Arduino hoists function prototypes but
// not variable definitions).
uint8_t lastEyeState   = 255;  // last drawn ClaudeState for eyes
uint8_t lastExpr        = 255;  // last idle expression
bool    lastBlink        = false;
int16_t lastPupilX       = 9999; // last thinking pupil ox (forces redraw on move)

// Daily stats (cached from daemon)
struct DailyStats {
  String   date;             // "2026-06-06"
  uint32_t tools_called;
  uint32_t tokens_total;
  uint16_t sessions;
  uint16_t errors;
  bool     valid;
};
DailyStats dailyStats = { "", 0, 0, 0, 0, false };

// Status view animation frame (for spinning eyes etc.)
uint32_t lastAnimFrameMs = 0;
uint8_t  animFrame = 0;

// ── Terminal ──────────────────────────────────────────────────
#define TERM_COLS      15
#define TERM_ROWS       8
#define TERM_CHAR_W    12
#define TERM_CHAR_H    20
#define TERM_PAD_X      8
#define TERM_PAD_Y     18

bool    termMode    = false;
String  termLines[TERM_ROWS];
uint8_t termRow     = 0;
uint8_t termCol     = 0;

// ── Logo data ─────────────────────────────────────────────────
#define LOGO_CX 120
#define LOGO_CY 105

#define LOGO_TRI_COUNT 162
static const int16_t LOGO_TRIS[][6] PROGMEM = {
  {120,105,65,134,100,114},{120,105,100,114,101,113},{120,105,101,113,100,112},
  {120,105,100,112,99,112},{120,105,99,112,93,111},{120,105,93,111,73,111},
  {120,105,73,111,55,110},{120,105,55,110,38,109},{120,105,38,109,34,108},
  {120,105,34,108,30,103},{120,105,30,103,30,100},{120,105,30,100,34,98},
  {120,105,34,98,39,98},{120,105,39,98,50,99},{120,105,50,99,67,100},
  {120,105,67,100,80,101},{120,105,80,101,98,103},{120,105,98,103,101,103},
  {120,105,101,103,101,102},{120,105,101,102,100,101},{120,105,100,101,100,100},
  {120,105,100,100,82,88},{120,105,82,88,63,76},{120,105,63,76,53,69},
  {120,105,53,69,48,65},{120,105,48,65,45,61},{120,105,45,61,44,54},
  {120,105,44,54,49,49},{120,105,49,49,55,49},{120,105,55,49,57,49},
  {120,105,57,49,64,55},{120,105,64,55,78,66},{120,105,78,66,96,79},
  {120,105,96,79,99,81},{120,105,99,81,100,81},{120,105,100,81,100,80},
  {120,105,100,80,99,78},{120,105,99,78,89,60},{120,105,89,60,78,41},
  {120,105,78,41,73,34},{120,105,73,34,72,29},{120,105,72,29,72,28},
  {120,105,72,28,72,27},{120,105,72,27,71,26},{120,105,71,26,71,25},
  {120,105,71,25,71,24},{120,105,71,24,77,16},{120,105,77,16,80,15},
  {120,105,80,15,87,16},{120,105,87,16,91,19},{120,105,91,19,95,29},
  {120,105,95,29,103,46},{120,105,103,46,114,68},{120,105,114,68,118,75},
  {120,105,118,75,119,81},{120,105,119,81,120,83},{120,105,120,83,121,83},
  {120,105,121,83,121,82},{120,105,121,82,122,69},{120,105,122,69,124,54},
  {120,105,124,54,126,34},{120,105,126,34,126,28},{120,105,126,28,129,21},
  {120,105,129,21,135,18},{120,105,135,18,139,20},{120,105,139,20,143,25},
  {120,105,143,25,142,28},{120,105,142,28,140,42},{120,105,140,42,136,64},
  {120,105,136,64,133,78},{120,105,133,78,135,78},{120,105,135,78,136,76},
  {120,105,136,76,144,67},{120,105,144,67,156,51},{120,105,156,51,162,45},
  {120,105,162,45,168,38},{120,105,168,38,172,35},{120,105,172,35,180,35},
  {120,105,180,35,185,43},{120,105,185,43,183,52},{120,105,183,52,175,62},
  {120,105,175,62,168,71},{120,105,168,71,159,83},{120,105,159,83,153,94},
  {120,105,153,94,154,94},{120,105,154,94,155,94},{120,105,155,94,176,90},
  {120,105,176,90,188,88},{120,105,188,88,201,85},{120,105,201,85,208,88},
  {120,105,208,88,208,91},{120,105,208,91,206,97},{120,105,206,97,191,101},
  {120,105,191,101,174,104},{120,105,174,104,148,110},{120,105,148,110,148,111},
  {120,105,148,111,148,111},{120,105,148,111,160,112},{120,105,160,112,165,112},
  {120,105,165,112,177,112},{120,105,177,112,200,114},{120,105,200,114,205,118},
  {120,105,205,118,209,123},{120,105,209,123,208,126},{120,105,208,126,199,131},
  {120,105,199,131,187,128},{120,105,187,128,159,121},{120,105,159,121,149,119},
  {120,105,149,119,147,119},{120,105,147,119,147,120},{120,105,147,120,156,128},
  {120,105,156,128,170,141},{120,105,170,141,189,158},{120,105,189,158,190,163},
  {120,105,190,163,188,166},{120,105,188,166,185,166},{120,105,185,166,169,153},
  {120,105,169,153,162,148},{120,105,162,148,148,136},{120,105,148,136,147,136},
  {120,105,147,136,147,137},{120,105,147,137,150,142},{120,105,150,142,168,168},
  {120,105,168,168,169,176},{120,105,169,176,168,179},{120,105,168,179,163,180},
  {120,105,163,180,158,179},{120,105,158,179,148,165},{120,105,148,165,137,149},
  {120,105,137,149,129,134},{120,105,129,134,128,135},{120,105,128,135,123,189},
  {120,105,123,189,120,192},{120,105,120,192,115,194},{120,105,115,194,110,191},
  {120,105,110,191,108,185},{120,105,108,185,110,174},{120,105,110,174,113,160},
  {120,105,113,160,116,148},{120,105,116,148,118,134},{120,105,118,134,119,129},
  {120,105,119,129,119,129},{120,105,119,129,118,129},{120,105,118,129,107,144},
  {120,105,107,144,91,166},{120,105,91,166,78,180},{120,105,78,180,75,181},
  {120,105,75,181,70,178},{120,105,70,178,70,173},{120,105,70,173,73,169},
  {120,105,73,169,91,146},{120,105,91,146,102,132},{120,105,102,132,109,124},
  {120,105,109,124,109,123},{120,105,109,123,108,123},{120,105,108,123,61,153},
  {120,105,61,153,52,155},{120,105,52,155,49,151},{120,105,49,151,49,146},
  {120,105,49,146,51,144},{120,105,51,144,65,134},{120,105,65,134,65,134},
};

#define LOGO_SEG_COUNT 162
static const int16_t LOGO_SEGS[][4] PROGMEM = {
  {65,134,100,114},{100,114,101,113},{101,113,100,112},{100,112,99,112},
  {99,112,93,111},{93,111,73,111},{73,111,55,110},{55,110,38,109},
  {38,109,34,108},{34,108,30,103},{30,103,30,100},{30,100,34,98},
  {34,98,39,98},{39,98,50,99},{50,99,67,100},{67,100,80,101},
  {80,101,98,103},{98,103,101,103},{101,103,101,102},{101,102,100,101},
  {100,101,100,100},{100,100,82,88},{82,88,63,76},{63,76,53,69},
  {53,69,48,65},{48,65,45,61},{45,61,44,54},{44,54,49,49},
  {49,49,55,49},{55,49,57,49},{57,49,64,55},{64,55,78,66},
  {78,66,96,79},{96,79,99,81},{99,81,100,81},{100,81,100,80},
  {100,80,99,78},{99,78,89,60},{89,60,78,41},{78,41,73,34},
  {73,34,72,29},{72,29,72,28},{72,28,72,27},{72,27,71,26},
  {71,26,71,25},{71,25,71,24},{71,24,77,16},{77,16,80,15},
  {80,15,87,16},{87,16,91,19},{91,19,95,29},{95,29,103,46},
  {103,46,114,68},{114,68,118,75},{118,75,119,81},{119,81,120,83},
  {120,83,121,83},{121,83,121,82},{121,82,122,69},{122,69,124,54},
  {124,54,126,34},{126,34,126,28},{126,28,129,21},{129,21,135,18},
  {135,18,139,20},{139,20,143,25},{143,25,142,28},{142,28,140,42},
  {140,42,136,64},{136,64,133,78},{133,78,135,78},{135,78,136,76},
  {136,76,144,67},{144,67,156,51},{156,51,162,45},{162,45,168,38},
  {168,38,172,35},{172,35,180,35},{180,35,185,43},{185,43,183,52},
  {183,52,175,62},{175,62,168,71},{168,71,159,83},{159,83,153,94},
  {153,94,154,94},{154,94,155,94},{155,94,176,90},{176,90,188,88},
  {188,88,201,85},{201,85,208,88},{208,88,208,91},{208,91,206,97},
  {206,97,191,101},{191,101,174,104},{174,104,148,110},{148,110,148,111},
  {148,111,148,111},{148,111,160,112},{160,112,165,112},{165,112,177,112},
  {177,112,200,114},{200,114,205,118},{205,118,209,123},{209,123,208,126},
  {208,126,199,131},{199,131,187,128},{187,128,159,121},{159,121,149,119},
  {149,119,147,119},{147,119,147,120},{147,120,156,128},{156,128,170,141},
  {170,141,189,158},{189,158,190,163},{190,163,188,166},{188,166,185,166},
  {185,166,169,153},{169,153,162,148},{162,148,148,136},{148,136,147,136},
  {147,136,147,137},{147,137,150,142},{150,142,168,168},{168,168,169,176},
  {169,176,168,179},{168,179,163,180},{163,180,158,179},{158,179,148,165},
  {148,165,137,149},{137,149,129,134},{129,134,128,135},{128,135,123,189},
  {123,189,120,192},{120,192,115,194},{115,194,110,191},{110,191,108,185},
  {108,185,110,174},{110,174,113,160},{113,160,116,148},{116,148,118,134},
  {118,134,119,129},{119,129,119,129},{119,129,118,129},{118,129,107,144},
  {107,144,91,166},{91,166,78,180},{78,180,75,181},{75,181,70,178},
  {70,178,70,173},{70,173,73,169},{73,169,91,146},{91,146,102,132},
  {102,132,109,124},{109,124,109,123},{109,123,108,123},{108,123,61,153},
  {61,153,52,155},{52,155,49,151},{49,151,49,146},{49,146,51,144},
  {51,144,65,134},{65,134,65,134},
};

// ═════════════════════════════════════════════════════════════
//  HELPERS
// ═════════════════════════════════════════════════════════════

int speedMs(int ms) {
  if (animSpeed == 3) return ms / 2;
  if (animSpeed == 1) return ms * 2;
  return ms;
}

uint16_t hexToRgb565(String hex) {
  hex.replace("#", "");
  if (hex.length() != 6) return C_WHITE;
  long v = strtol(hex.c_str(), nullptr, 16);
  return tft.color565((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
}

void setBacklight(bool on) {
  backlightOn = on;
  digitalWrite(TFT_BLK, on ? HIGH : LOW);
}

void initColours() {
  // C_ORANGE = tft.color565(170, 72, 28);
  C_ORANGE    = tft.color565(218, 17, 0);
  C_DARKBG    = tft.color565(10,  12,  16);
  C_MUTED     = tft.color565(90,  88,  86);
  C_GREEN     = tft.color565(80, 220, 130);
  // Mood-light colours (Claude state → background)
  MOOD_IDLE      = C_ORANGE;                           // original orange
  MOOD_THINKING  = tft.color565(0,   80,  160);        // calm blue
  MOOD_AWAITING  = tft.color565(200, 160, 0);          // amber yellow
  MOOD_DONE      = tft.color565(0,   160, 80);         // success green
  MOOD_ERROR     = tft.color565(200, 20,  20);         // error red
  animBgColor = C_ORANGE;
  drawBgColor = C_ORANGE;
}

// ═════════════════════════════════════════════════════════════
//  LOGO
// ═════════════════════════════════════════════════════════════

void drawLogoFilled(uint16_t bg, uint16_t fg) {
  tft.fillScreen(bg);
  for (uint16_t i = 0; i < LOGO_TRI_COUNT; i++) {
    tft.fillTriangle(
      pgm_read_word(&LOGO_TRIS[i][0]), pgm_read_word(&LOGO_TRIS[i][1]),
      pgm_read_word(&LOGO_TRIS[i][2]), pgm_read_word(&LOGO_TRIS[i][3]),
      pgm_read_word(&LOGO_TRIS[i][4]), pgm_read_word(&LOGO_TRIS[i][5]),
      fg);
  }
  tft.setTextColor(fg); tft.setTextSize(2);
  tft.setCursor(LOGO_CX - 54, 210); tft.print("Anthropic");
  tft.setCursor(LOGO_CX - 53, 210); tft.print("Anthropic");
}

// ═════════════════════════════════════════════════════════════
//  VIEWS
// ═════════════════════════════════════════════════════════════

// Eye helpers — shared constants via #define EYE_*
inline int16_t eyeLX(int16_t ox) {
  return (DISP_W - (EYE_W * 2 + EYE_GAP)) / 2 + EYE_OX + ox;
}
inline int16_t eyeRX(int16_t ox) { return eyeLX(ox) + EYE_W + EYE_GAP; }
inline int16_t eyeY()            { return (DISP_H - EYE_H) / 2 - EYE_OY; }
inline int16_t eyeCY()           { return eyeY() + EYE_H / 2; }

void drawNormalEyes(int16_t ox = 0, bool blink = false) {
  tft.fillScreen(animBgColor);
  const int16_t lx = eyeLX(ox), rx = eyeRX(ox), ey = eyeY();
  if (!blink) {
    tft.fillRect(lx, ey, EYE_W, EYE_H, C_BLACK);
    tft.fillRect(rx, ey, EYE_W, EYE_H, C_BLACK);
  } else {
    tft.fillRect(lx, ey + EYE_H / 2 - 3, EYE_W, 6, C_BLACK);
    tft.fillRect(rx, ey + EYE_H / 2 - 3, EYE_W, 6, C_BLACK);
  }
}

void drawChevron(int16_t cx, int16_t cy, int16_t arm, int16_t reach,
                 uint8_t thk, bool rightFacing, uint16_t col) {
  for (int8_t t = -(int8_t)thk; t <= (int8_t)thk; t++) {
    if (rightFacing) {
      tft.drawLine(cx - reach/2, cy - arm + t, cx + reach/2, cy + t,      col);
      tft.drawLine(cx + reach/2, cy + t,       cx - reach/2, cy + arm + t, col);
    } else {
      tft.drawLine(cx + reach/2, cy - arm + t, cx - reach/2, cy + t,      col);
      tft.drawLine(cx - reach/2, cy + t,       cx + reach/2, cy + arm + t, col);
    }
  }
}

void drawSquishEyes(bool closed = false) {
  tft.fillScreen(animBgColor);
  const int16_t lx = eyeLX(0), rx = eyeRX(0), cy = eyeCY();
  const int16_t arm   = EYE_H / 2;
  const int16_t reach = EYE_W / 2;
  const int16_t lcx   = lx + EYE_W / 2;
  const int16_t rcx   = rx + EYE_W / 2;
  if (!closed) {
    drawChevron(lcx, cy, arm, reach, 10, true,  C_BLACK);
    drawChevron(rcx, cy, arm, reach, 10, false, C_BLACK);
  } else {
    tft.fillRect(lx, cy - 5, EYE_W, 10, C_BLACK);
    tft.fillRect(rx, cy - 5, EYE_W, 10, C_BLACK);
  }
}

void drawCodeView() {
  termMode = false;
  tft.fillScreen(C_DARKBG);
  tft.fillRect(0, 0,          DISP_W, 4, C_ORANGE);
  tft.fillRect(0, DISP_H - 4, DISP_W, 4, C_ORANGE);
  tft.setTextColor(C_ORANGE); tft.setTextSize(4);
  tft.setCursor((DISP_W - 144) / 2, DISP_H / 2 - 52); tft.print("Claude");
  tft.setTextColor(C_WHITE);  tft.setTextSize(4);
  tft.setCursor((DISP_W - 96) / 2,  DISP_H / 2 + 8);  tft.print("Code");
  tft.fillRect((DISP_W - 96) / 2, DISP_H / 2 + 52, 96, 3, C_ORANGE);
}

// ═════════════════════════════════════════════════════════════
//  STATUS VIEW (Claude Code companion display)
// ═════════════════════════════════════════════════════════════

uint16_t moodColor(ClaudeState s) {
  // Mood light: background colour follows Claude's state.
  switch (s) {
    case CS_IDLE:     return MOOD_IDLE;
    case CS_THINKING: return MOOD_THINKING;
    case CS_AWAITING: return MOOD_AWAITING;
    case CS_DONE:     return MOOD_DONE;
    case CS_ERROR:    return MOOD_ERROR;
  }
  return MOOD_IDLE;
}

// Pick a high-contrast text colour for a given background.
uint16_t textOn(uint16_t bg) {
  uint8_t r = (bg >> 11) & 0x1F;
  uint8_t g = (bg >>  5) & 0x3F;
  uint8_t b =  bg        & 0x1F;
  // luminance approx (scaled to 5-bit)
  uint16_t lum = (r * 2 + g + b * 2);
  return lum > 96 ? C_BLACK : C_WHITE;
}

// Truncate a string to fit `maxChars`, adding ".." if cut.
String fit(const String& s, uint8_t maxChars) {
  if (s.length() <= maxChars) return s;
  if (maxChars < 3) return s.substring(0, maxChars);
  return s.substring(0, maxChars - 2) + "..";
}

// Draw a Wi-Fi signal icon (4 bars) based on RSSI dBm.
void drawWifiIcon(int16_t x, int16_t y, int8_t rssi, uint16_t col) {
  // 4 bars, height 4/7/10/13 px, width 3 px, gap 2 px
  uint8_t bars = 0;
  if      (rssi >= -55) bars = 4;
  else if (rssi >= -65) bars = 3;
  else if (rssi >= -75) bars = 2;
  else if (rssi >= -85) bars = 1;
  for (uint8_t i = 0; i < 4; i++) {
    int16_t h = 4 + i * 3;
    int16_t bx = x + i * 5;
    int16_t by = y + (13 - h);
    if (i < bars) tft.fillRect(bx, by, 3, h, col);
    else          tft.drawRect(bx, by, 3, h, col);
  }
}

// Eye geometry used by both clearStatusEyeBoxes() and drawStatusEyes().
#define STATUS_EW 28
#define STATUS_EH 44
#define STATUS_GAP 36
#define STATUS_CY 80
#define STATUS_LX ((DISP_W - STATUS_EW * 2 - STATUS_GAP) / 2)
#define STATUS_RX (STATUS_LX + STATUS_EW + STATUS_GAP)
#define STATUS_EY (STATUS_CY - STATUS_EH / 2)

// Clear only the two eye boxes (not the full-width eye band) before
// redrawing the eyes — avoids flicker from clearing the whole band.
void clearStatusEyeBoxes(uint16_t bg) {
  // +2 padding so the awaiting/bigger-eye shapes that slightly exceed the
  // box are fully erased.
  tft.fillRect(STATUS_LX - 2, STATUS_EY - 2, STATUS_EW + 4, STATUS_EH + 4, bg);
  tft.fillRect(STATUS_RX - 2, STATUS_EY - 2, STATUS_EW + 4, STATUS_EH + 4, bg);
}

// Draw the state-specific eye animation.
// `centreY` is where the eyes are vertically centred.
void drawStatusEyes(int16_t centreY) {
  const int16_t eW = STATUS_EW, eH = STATUS_EH, gap = STATUS_GAP;
  const int16_t lx = STATUS_LX, rx = STATUS_RX;
  const int16_t ey = STATUS_EY;
  uint16_t bg = moodColor(claudeStatus.state);
  uint16_t fg = C_BLACK;  // eyes are black on the orange background

  switch (claudeStatus.state) {
    case CS_IDLE: {
      // Expression rotation (4 moods)
      switch (idleExpression) {
        case 0: { // normal — slow blink
          bool blink = (animFrame % 60) < 4;
          if (blink) {
            tft.fillRect(lx, ey + eH/2 - 3, eW, 6, fg);
            tft.fillRect(rx, ey + eH/2 - 3, eW, 6, fg);
          } else {
            tft.fillRect(lx, ey, eW, eH, fg);
            tft.fillRect(rx, ey, eW, eH, fg);
          }
          break;
        }
        case 1: { // sleepy — half-closed droopy eyes
          tft.fillRect(lx, ey + eH/2 - 4, eW, 8, fg);
          tft.fillRect(rx, ey + eH/2 - 4, eW, 8, fg);
          // tiny slit of pupil
          tft.drawFastHLine(lx + 4, ey + eH/2 - 1, eW - 8, bg);
          tft.drawFastHLine(rx + 4, ey + eH/2 - 1, eW - 8, bg);
          break;
        }
        case 2: { // puzzled — one eye bigger
          tft.fillRect(lx, ey + 6, eW, eH - 12, fg);
          tft.fillRect(rx, ey, eW, eH, fg);
          // pupils looking up-left
          tft.fillCircle(lx + eW/2 - 3, ey + eH/2 - 4, 4, bg);
          tft.fillCircle(rx + eW/2 - 3, ey + eH/2 - 4, 5, bg);
          break;
        }
        case 3: { // happy — squint > <
          const int16_t cy = ey + eH/2;
          const int16_t arm = eH / 2 - 10, reach = eW / 2;
          for (int8_t t = -3; t <= 3; t++) {
            tft.drawLine(lx + reach, cy - arm + t, lx, cy + t, fg);
            tft.drawLine(lx, cy + t, lx + reach, cy + arm + t, fg);
            tft.drawLine(rx, cy - arm + t, rx + reach, cy + t, fg);
            tft.drawLine(rx + reach, cy + t, rx, cy + arm + t, fg);
          }
          break;
        }
      }
      break;
    }
    case CS_THINKING: {
      const int16_t ox = 4 * cos(animFrame * 0.3);
      const int16_t oy = 3 * sin(animFrame * 0.3);
      tft.fillRect(lx, ey, eW, eH, fg);
      tft.fillRect(rx, ey, eW, eH, fg);
      tft.fillCircle(lx + eW/2 + ox, ey + eH/2 + oy, 5, bg);
      tft.fillCircle(rx + eW/2 + ox, ey + eH/2 + oy, 5, bg);
      break;
    }
    case CS_AWAITING: {
      // Patient waiting: solid black eyes with a slow blink — no white
      // flash (user wants black eyes on the orange background).
      bool blink = (animFrame % 90) < 6;
      if (blink) {
        tft.fillRect(lx, ey + eH/2 - 3, eW, 6, fg);
        tft.fillRect(rx, ey + eH/2 - 3, eW, 6, fg);
      } else {
        tft.fillRect(lx, ey, eW, eH, fg);
        tft.fillRect(rx, ey, eW, eH, fg);
      }
      break;
    }
    case CS_DONE: {
      const int16_t cy = ey + eH/2;
      const int16_t arm = eH / 2 - 6, reach = eW / 2;
      for (int8_t t = -4; t <= 4; t++) {
        tft.drawLine(lx + reach, cy - arm + t, lx, cy + t, fg);
        tft.drawLine(lx, cy + t, lx + reach, cy + arm + t, fg);
        tft.drawLine(rx, cy - arm + t, rx + reach, cy + t, fg);
        tft.drawLine(rx + reach, cy + t, rx, cy + arm + t, fg);
      }
      break;
    }
    case CS_ERROR: {
      const int16_t cy = ey + eH/2;
      for (int8_t t = -3; t <= 3; t++) {
        tft.drawLine(lx, ey + t, lx + eW, ey + eH + t, fg);
        tft.drawLine(lx + eW, ey + t, lx, ey + eH + t, fg);
        tft.drawLine(rx, ey + t, rx + eW, ey + eH + t, fg);
        tft.drawLine(rx + eW, ey + t, rx, ey + eH + t, fg);
      }
      break;
    }
  }
}

// Forward declarations (drawStatusView calls these, defined below)
void drawStatusFrame(uint16_t bg);
void drawStatusEyesArea(uint16_t bg);
void drawStatusTextLines(uint16_t fg);

void drawStatusView() {
  uint16_t bg = moodColor(claudeStatus.state);
  // Full redraw if frame not yet drawn, or bg color changed (state changed).
  // (Static text like tool/task is refreshed on this path; it doesn't change
  //  between pushes, so we don't need to repaint it on every animation tick.)
  if (!statusFrameDrawn || bg != statusFrameBgColor) {
    statusFrameDrawn   = true;
    statusFrameBgColor = bg;
    drawStatusFrame(bg);
    // Full frame redraw cleared the eye area — force eyes to repaint next.
    lastEyeState = 255; lastExpr = 255; lastBlink = false; lastPupilX = 9999;
  }
  // Always repaint dynamic parts (eyes + token bar)
  drawStatusEyesArea(bg);
}

// Full static frame: bg fill, top bar, all text, token bar outline.
// Called once per entry into Status view, or when state changes bg color.
void drawStatusFrame(uint16_t bg) {
  uint16_t fg = textOn(bg);
  tft.fillScreen(bg);

  // ── Top bar (22px) — git · project | model | daemon | wifi ──
  tft.fillRect(0, 0, DISP_W, 22, C_DARKBG);
  tft.fillRect(0, 22, DISP_W, 2, C_ORANGE);
  tft.setTextColor(C_WHITE); tft.setTextSize(1);

  // Left: git branch + project
  tft.setCursor(4, 8);
  String topLine = "";
  if (claudeStatus.git_branch.length() > 0) topLine = claudeStatus.git_branch;
  if (claudeStatus.project.length() > 0) {
    if (topLine.length()) topLine += " ";
    topLine += claudeStatus.project;
  }
  if (topLine.length() == 0) topLine = "Clawd Mochi";
  tft.print(fit(topLine, 20));

  // Center-right: model badge (full name, e.g. "glm-latest")
  if (claudeStatus.model.length() > 0) {
    tft.setCursor(148, 8);
    tft.setTextColor(C_ORANGE); tft.print(fit(claudeStatus.model, 14));
    tft.setTextColor(C_WHITE);
  }

  // Daemon offline indicator (top-right, before wifi)
  bool daemonOnline = (millis() - claudeStatus.last_update_ms) < DAEMON_OFFLINE_TIMEOUT_MS
                      || claudeStatus.last_update_ms == 0;
  if (!daemonOnline) {
    tft.setCursor(182, 8); tft.setTextColor(C_MUTED); tft.print("!");
    tft.setTextColor(C_WHITE);
  }

  // Wifi icon (far right)
  if (WiFi.status() == WL_CONNECTED) {
    drawWifiIcon(DISP_W - 24, 5, WiFi.RSSI(), C_WHITE);
  } else {
    tft.setCursor(DISP_W - 20, 8); tft.setTextColor(C_MUTED); tft.print("AP");
  }

  // ── Eyes + token bar + text lines painted by drawStatusEyesArea() ──

  // ── Tool + task + session lines (y=132..172) ─────────────
  drawStatusTextLines(fg);

  // ── Bottom: token progress bar (y=195) ───────────────────
  int16_t barX = 8, barY = 195, barW = DISP_W - 16, barH = 10;
  tft.drawRect(barX, barY, barW, barH, fg);
  if (claudeStatus.tokens_max > 0) {
    uint32_t pct = (uint32_t)claudeStatus.tokens_used * (barW - 2) / claudeStatus.tokens_max;
    if (pct > (uint32_t)(barW - 2)) pct = barW - 2;
    tft.fillRect(barX + 1, barY + 1, pct, barH - 2, fg);
  }

  // ── Bottom labels (y=210..224) ───────────────────────────
  // Left: token count, Right: daily stats
  tft.setTextColor(fg); tft.setTextSize(1);

  // Token label (left)
  tft.setCursor(6, 210);
  String tokLabel = String(claudeStatus.tokens_used / 1000) + "k/" +
                    String(claudeStatus.tokens_max / 1000) + "k";
  tft.print(tokLabel);

  // Model short name under token (if space)
  if (claudeStatus.model.length() > 0) {
    tft.setCursor(6, 222);
    tft.print(claudeStatus.model);
  }

  // Daily stats (right-aligned at y=210)
  if (dailyStats.valid) {
    String ds = String(dailyStats.tools_called) + " tools today";
    int16_t tw = ds.length() * 6;  // textSize 1 = 6px/char
    tft.setCursor(max(6, DISP_W - 6 - tw), 210);
    tft.print(ds);
  }

  // Daemon offline note (right-aligned at y=222)
  if (!daemonOnline) {
    tft.setCursor(DISP_W - 80, 222);
    tft.setTextColor(C_MUTED);
    tft.print("daemon offline");
  }
}

// Paint the dynamic text lines (tool + duration, task, session info).
// Extracted so both drawStatusFrame (full) and drawStatusEyesArea (per-tick)
// can render them — this lets duration_s / session_duration refresh every
// tick without a full-screen fillScreen (which would flicker).
void drawStatusTextLines(uint16_t fg) {
  tft.setTextColor(fg); tft.setTextSize(1);
  // Tool line (y=132): ">ToolName Ns"
  if (claudeStatus.tool.length() > 0) {
    tft.setCursor(6, 132);
    String toolLine = ">" + fit(claudeStatus.tool, 12) + " " + String(claudeStatus.duration_s) + "s";
    tft.print(toolLine);
  }
  // Task line (y=146)
  if (claudeStatus.task.length() > 0) {
    tft.setCursor(6, 146);
    tft.print("-"); tft.print(fit(claudeStatus.task, 30));
  }
  // Session info line (y=164): "M:SS #count"
  bool hasSessionInfo = (claudeStatus.session_duration_s > 0 || claudeStatus.tool_count > 0);
  if (hasSessionInfo) {
    tft.setCursor(6, 164);
    String info = "";
    if (claudeStatus.session_duration_s > 0) {
      uint16_t mins = claudeStatus.session_duration_s / 60;
      uint8_t  secs = claudeStatus.session_duration_s % 60;
      info = String(mins) + ":" + (secs < 10 ? "0" : "") + String(secs);
    }
    if (claudeStatus.tool_count > 0) {
      if (info.length()) info += " ";
      info += "#" + String(claudeStatus.tool_count);
    }
    tft.print(info);
  }
}

// Dynamic repaint (called every animation tick + on every status push).
// Only touches the eye band, text lines, and token bar — the static frame
// (bg, top bar) is left untouched, eliminating full-screen flicker.
//
// Eyes are redrawn ONLY when the visible shape changes (blink toggle,
// state/expression switch, or thinking pupil position) — clearing just the
// eye boxes instead of the whole eye band avoids the per-tick flicker that
// repainting the full-width band caused.
void drawStatusEyesArea(uint16_t bg) {
  uint16_t fg = textOn(bg);
  // Top bar right side: live clock (HH:MM, Beijing time via NTP) + wifi.
  // Repainted every tick so the clock updates; sits left of the wifi icon.
  tft.fillRect(DISP_W - 82, 0, 82, 22, C_DARKBG);
  {
    struct tm tinfo;
    if (getLocalTime(&tinfo, 50)) {
      char timeStr[6];
      snprintf(timeStr, sizeof timeStr, "%02d:%02d", tinfo.tm_hour, tinfo.tm_min);
      tft.setTextColor(C_WHITE); tft.setTextSize(1);
      tft.setCursor(DISP_W - 78, 8);
      tft.print(timeStr);
    }
  }
  if (WiFi.status() == WL_CONNECTED) {
    drawWifiIcon(DISP_W - 24, 5, WiFi.RSSI(), C_WHITE);
  } else {
    tft.setCursor(DISP_W - 20, 8); tft.setTextColor(C_MUTED); tft.print("AP");
  }

  // ── Eyes: redraw only when the visible shape changes ────────────
  // Compute the current eye "signature" and compare to last draw.
  uint8_t st = claudeStatus.state;
  bool blink = false;
  int16_t pupilX = 0;
  bool needRedraw = false;

  if (st == CS_IDLE && idleExpression == 0) {
    blink = (animFrame % 60) < 4;
  } else if (st == CS_AWAITING) {
    blink = (animFrame % 90) < 6;
  } else if (st == CS_THINKING) {
    pupilX = 4 * cos(animFrame * 0.3);   // moving pupil → redraw every frame
  }

  if (st != lastEyeState)              needRedraw = true;
  if (st == CS_IDLE && idleExpression != lastExpr) needRedraw = true;
  if ((st == CS_IDLE && idleExpression == 0) || st == CS_AWAITING) {
    if (blink != lastBlink)            needRedraw = true;
  }
  if (st == CS_THINKING && pupilX != lastPupilX) needRedraw = true;

  if (needRedraw) {
    // Clear ONLY the eye boxes (not the full eye band) before redrawing,
    // so neighbouring pixels don't flash.
    clearStatusEyeBoxes(bg);
    drawStatusEyes(80);
    lastEyeState = st;
    lastExpr = idleExpression;
    lastBlink = blink;
    lastPupilX = pupilX;
  }

  // Text band: y 128..172 (tool/task/session lines) — cleared + repainted
  // every tick so duration_s / session_duration update live.
  tft.fillRect(0, 128, DISP_W, 46, bg);
  drawStatusTextLines(fg);

  // Token bar fill (outline stays from the frame; just refill the inner)
  int16_t barX = 8, barY = 195, barW = DISP_W - 16, barH = 10;
  tft.fillRect(barX + 1, barY + 1, barW - 2, barH - 2, bg);  // clear old fill
  if (claudeStatus.tokens_max > 0) {
    uint32_t pct = (uint32_t)claudeStatus.tokens_used * (barW - 2) / claudeStatus.tokens_max;
    if (pct > (uint32_t)(barW - 2)) pct = barW - 2;
    tft.fillRect(barX + 1, barY + 1, pct, barH - 2, fg);
  }
}

// ═════════════════════════════════════════════════════════════
//  SCREENSAVER (view 5) — entered after 2 min idle
//  Unified look: orange background + black eyes + orange pupils + clock.
//  Uses partial redraw (background drawn once, only eyes/clock refresh
//  per frame) to avoid the full-screen flicker that fillScreen() every
//  tick would cause.
// ═════════════════════════════════════════════════════════════

bool saverBlinking = false;     // current eye state (avoids redundant redraw)
char saverLastTime[6] = "";    // last drawn clock string (avoid redrawing same minute)
uint8_t lastDrawnView = 255;  // view we last fully painted (force bg repaint on change)

// Eye geometry (kept stable so partial redraw can clear just the eye box).
#define SAVER_EW 22
#define SAVER_EH 34
#define SAVER_GAP 28
#define SAVER_CY 110
#define SAVER_LX ((DISP_W - SAVER_EW * 2 - SAVER_GAP) / 2)
#define SAVER_RX (SAVER_LX + SAVER_EW + SAVER_GAP)
#define SAVER_EY (SAVER_CY - SAVER_EH / 2)

void drawSaverEyes(bool blink) {
  uint16_t fg = C_BLACK;
  uint16_t bg = C_ORANGE;
  // Clear the eye region first (covers pupil when switching to blink),
  // then draw the current state.
  tft.fillRect(SAVER_LX - 2, SAVER_EY - 2, SAVER_EW + 4, SAVER_EH + 4, bg);
  tft.fillRect(SAVER_RX - 2, SAVER_EY - 2, SAVER_EW + 4, SAVER_EH + 4, bg);
  if (blink) {
    tft.fillRect(SAVER_LX, SAVER_CY - 2, SAVER_EW, 4, fg);
    tft.fillRect(SAVER_RX, SAVER_CY - 2, SAVER_EW, 4, fg);
  } else {
    tft.fillRect(SAVER_LX, SAVER_EY, SAVER_EW, SAVER_EH, fg);
    tft.fillRect(SAVER_RX, SAVER_EY, SAVER_EW, SAVER_EH, fg);
    tft.fillCircle(SAVER_LX + SAVER_EW / 2, SAVER_EY + SAVER_EH / 2, 4, bg);
    tft.fillCircle(SAVER_RX + SAVER_EW / 2, SAVER_EY + SAVER_EH / 2, 4, bg);
  }
}

void drawSaverClock(const char* timeStr) {
  // Clear clock band then redraw (textSize 2 → 12px/char, 5 chars).
  tft.fillRect(0, 38, DISP_W, 20, C_ORANGE);
  tft.setTextColor(C_BLACK); tft.setTextSize(2);
  uint8_t tw = strlen(timeStr) * 12;
  tft.setCursor((DISP_W - tw) / 2, 40);
  tft.print(timeStr);
}

void drawScreenSaverView() {
  // Paint the full background once on entry (or when the view changed
  // since we last drew). After that, only eyes/clock refresh per frame.
  if (lastDrawnView != VIEW_SCREENSAVER) {
    tft.fillScreen(C_ORANGE);
    saverBlinking = false;
    saverLastTime[0] = '\0';
    lastDrawnView = VIEW_SCREENSAVER;
  }

  // Eyes: only redraw when the blink state actually changes.
  bool blink = (animFrame % 60) < 3;   // ~6s blink at 10fps
  if (blink != saverBlinking) {
    saverBlinking = blink;
    drawSaverEyes(blink);
  }

  // Clock: only redraw when the minute string changes.
  struct tm t;
  if (getLocalTime(&t, 50)) {
    char timeStr[6];
    snprintf(timeStr, sizeof timeStr, "%02d:%02d", t.tm_hour, t.tm_min);
    if (strcmp(timeStr, saverLastTime) != 0) {
      strcpy(saverLastTime, timeStr);
      drawSaverClock(timeStr);
    }
  }
}
// ═════════════════════════════════════════════════════════════

void applyStatus(const ClaudeStatus& incoming) {
  // Repeated IDLE heartbeats keep daemon-online fields fresh, but they should
  // not reset the screensaver timer. Treat real work/status changes as activity.
  bool statusActivity = (incoming.state != CS_IDLE ||
                         incoming.state != claudeStatus.state ||
                         incoming.tool       != claudeStatus.tool ||
                         incoming.task       != claudeStatus.task ||
                         incoming.project    != claudeStatus.project ||
                         incoming.git_branch != claudeStatus.git_branch ||
                         incoming.model      != claudeStatus.model ||
                         incoming.tool_count != claudeStatus.tool_count ||
                         incoming.session_duration_s != claudeStatus.session_duration_s);
  if (statusActivity) {
    lastInteractionMs = millis();
  }
  if (statusActivity && currentView == VIEW_SCREENSAVER) {
    currentView = previousView;
  }
  // Only force a full frame repaint if the static text or state actually
  // changed. Daemon pushes frequently (heartbeat + duration loop), so
  // unconditionally forcing a full fillScreen here would re-introduce flicker.
  bool staticChanged = (incoming.state      != claudeStatus.state      ||
                        incoming.tool       != claudeStatus.tool       ||
                        incoming.task       != claudeStatus.task       ||
                        incoming.model      != claudeStatus.model      ||
                        incoming.git_branch != claudeStatus.git_branch ||
                        incoming.project    != claudeStatus.project    ||
                        incoming.tool_count  != claudeStatus.tool_count  ||
                        incoming.session_duration_s != claudeStatus.session_duration_s ||
                        incoming.tokens_max != claudeStatus.tokens_max);
  if (staticChanged) statusFrameDrawn = false;

  // ── Priority merging ──────────────────────────────────────
  // If a high-priority transient state (ERROR/AWAITING/DONE) is
  // currently active and the incoming state is lower priority,
  // remember it as pending but don't override the higher state.
  bool currentIsTransient = (claudeStatus.state == CS_ERROR ||
                             claudeStatus.state == CS_AWAITING ||
                             claudeStatus.state == CS_DONE);
  if (currentIsTransient && incoming.state < claudeStatus.state) {
    pendingStatus = incoming;
    hasPendingStatus = true;
    // still refresh non-state fields so info stays current
    claudeStatus.tool         = incoming.tool;
    claudeStatus.task         = incoming.task;
    claudeStatus.tokens_used  = incoming.tokens_used;
    claudeStatus.tokens_max   = incoming.tokens_max;
    claudeStatus.git_branch   = incoming.git_branch;
    claudeStatus.project      = incoming.project;
    claudeStatus.model        = incoming.model;
    claudeStatus.session_duration_s = incoming.session_duration_s;
    claudeStatus.tool_count   = incoming.tool_count;
    claudeStatus.last_update_ms = millis();
    if (currentView == VIEW_STATUS) drawStatusView();
    return;
  }

  // Track special state transitions for timeouts
  if (incoming.state == CS_ERROR && claudeStatus.state != CS_ERROR) {
    errorStartMs = millis();
  }
  if (incoming.state == CS_DONE && claudeStatus.state != CS_DONE) {
    doneStartMs = millis();
  }
  if (incoming.state == CS_AWAITING && claudeStatus.state != CS_AWAITING) {
    awaitingStartMs = millis();
  }

  claudeStatus = incoming;
  claudeStatus.last_update_ms = millis();
  hasPendingStatus = false;

  // Auto-switch to Status view on important events
  if (autoSwitchEnabled && currentView != VIEW_STATUS) {
    if (incoming.state == CS_ERROR ||
        incoming.state == CS_AWAITING ||
        incoming.state == CS_DONE) {
      previousView = currentView;
      currentView = VIEW_STATUS;
    }
  }

  if (currentView == VIEW_STATUS) drawStatusView();
}

// Apply pending status if any (called when transient state times out)
void applyPendingIfAny() {
  if (!hasPendingStatus) {
    claudeStatus.state = CS_IDLE;
    return;
  }
  ClaudeStatus p = pendingStatus;
  hasPendingStatus = false;
  // Apply the pending status (now that the high-priority state is gone)
  // Don't re-trigger auto-switch since the user is already in a view they chose.
  claudeStatus = p;
  claudeStatus.last_update_ms = millis();
}

// Called from loop() to handle timeouts and animation frames.
void tickStateMachine() {
  uint32_t now = millis();

  // ERROR timeout → apply pending or revert to IDLE
  if (claudeStatus.state == CS_ERROR && now - errorStartMs > ERROR_TIMEOUT_MS) {
    applyPendingIfAny();
    if (autoSwitchEnabled && currentView == VIEW_STATUS && previousView != VIEW_STATUS) {
      currentView = previousView;
    } else if (currentView == VIEW_STATUS) {
      drawStatusView();
    }
  }
  // DONE timeout → apply pending or revert to IDLE
  if (claudeStatus.state == CS_DONE && now - doneStartMs > DONE_TIMEOUT_MS) {
    applyPendingIfAny();
    if (autoSwitchEnabled && currentView == VIEW_STATUS && previousView != VIEW_STATUS) {
      currentView = previousView;
    } else if (currentView == VIEW_STATUS) {
      drawStatusView();
    }
  }
  // AWAITING timeout → apply pending or revert to IDLE
  // (awaiting had no timeout before, so it got stuck with eyes flashing forever)
  if (claudeStatus.state == CS_AWAITING && now - awaitingStartMs > AWAITING_TIMEOUT_MS) {
    applyPendingIfAny();
    if (autoSwitchEnabled && currentView == VIEW_STATUS && previousView != VIEW_STATUS) {
      currentView = previousView;
    } else if (currentView == VIEW_STATUS) {
      drawStatusView();
    }
  }

  // Daemon offline check
  if (claudeStatus.last_update_ms > 0 &&
      now - claudeStatus.last_update_ms > DAEMON_OFFLINE_TIMEOUT_MS &&
      claudeStatus.state != CS_IDLE) {
    claudeStatus.state = CS_IDLE;
    if (currentView == VIEW_STATUS) drawStatusView();
  }

  // ── Expression rotation (IDLE in Status view) ─────────────
  if (claudeStatus.state == CS_IDLE && currentView == VIEW_STATUS) {
    if (now - lastExpressionChangeMs > EXPRESSION_INTERVAL_MS) {
      lastExpressionChangeMs = now;
      idleExpression = (idleExpression + 1) % 4;
    }
  } else {
    idleExpression = 0;
  }

  // ── Screensaver auto-entry ────────────────────────────────
  if (currentView != VIEW_SCREENSAVER &&
      claudeStatus.state == CS_IDLE &&
      now - lastInteractionMs > SCREENSAVER_TIMEOUT_MS) {
    previousView = currentView;
    currentView = VIEW_SCREENSAVER;
    // drawScreenSaverView() repaints the background when lastDrawnView
    // doesn't match — no explicit reset needed here.
  }

  // ── Animation frame tick (10 fps) ─────────────────────────
  if ((currentView == VIEW_STATUS || currentView == VIEW_SCREENSAVER)
      && now - lastAnimFrameMs > 100) {
    lastAnimFrameMs = now;
    animFrame++;
    if (currentView == VIEW_STATUS) drawStatusView();
    else drawScreenSaverView();
  }
}

// ═════════════════════════════════════════════════════════════
//  TERMINAL
// ═════════════════════════════════════════════════════════════

void termClear() {
  for (uint8_t i = 0; i < TERM_ROWS; i++) termLines[i] = "";
  termRow = 0; termCol = 0;
}

void termDrawHeader() {
  tft.fillRect(0, 0, DISP_W, TERM_PAD_Y + 1, C_DARKBG);
  tft.setTextColor(C_ORANGE); tft.setTextSize(1);
  tft.setCursor(TERM_PAD_X, 4); tft.print("clawd@mochi terminal");
  tft.drawFastHLine(0, TERM_PAD_Y, DISP_W, C_ORANGE);
}

// Prefix "clawd:~$ " in green, drawn only when the row has content
void termDrawPrefix(int16_t yy) {
  tft.setTextColor(C_GREEN); tft.setTextSize(1);
  tft.setCursor(TERM_PAD_X, yy + 6);
  tft.print("clawd:~$ ");
}

#define PREFIX_PX 54   // 9 chars × 6px = 54px at textSize 1

void termDrawLine(uint8_t r) {
  const int16_t yy = TERM_PAD_Y + 4 + r * TERM_CHAR_H;
  tft.fillRect(0, yy, DISP_W, TERM_CHAR_H, C_DARKBG);
  // show prefix only on the currently active (cursor) line
  if (r == termRow) termDrawPrefix(yy);
  tft.setTextColor(C_WHITE); tft.setTextSize(2);
  tft.setCursor(TERM_PAD_X + PREFIX_PX, yy + 1);
  tft.print(termLines[r]);
  if (r == termRow) {
    const int16_t cx = TERM_PAD_X + PREFIX_PX + termCol * TERM_CHAR_W;
    tft.fillRect(cx, yy + 1, TERM_CHAR_W - 2, TERM_CHAR_H - 2, C_GREEN);
  }
}

void termDrawLastChar() {
  if (termCol == 0) return;
  const int16_t yy    = TERM_PAD_Y + 4 + termRow * TERM_CHAR_H;
  const int16_t baseX = TERM_PAD_X + PREFIX_PX;
  const uint8_t prev  = termCol - 1;
  // erase prev cell (had cursor block)
  tft.fillRect(baseX + prev * TERM_CHAR_W, yy + 1, TERM_CHAR_W, TERM_CHAR_H - 1, C_DARKBG);
  tft.setTextColor(C_WHITE); tft.setTextSize(2);
  tft.setCursor(baseX + prev * TERM_CHAR_W, yy + 1);
  tft.print(termLines[termRow][prev]);
  // new cursor
  tft.fillRect(baseX + termCol * TERM_CHAR_W, yy + 1, TERM_CHAR_W - 2, TERM_CHAR_H - 2, C_GREEN);
}

void termDrawBackspace() {
  const int16_t yy    = TERM_PAD_Y + 4 + termRow * TERM_CHAR_H;
  const int16_t baseX = TERM_PAD_X + PREFIX_PX;
  // erase deleted char + old cursor
  tft.fillRect(baseX + termCol * TERM_CHAR_W, yy + 1, TERM_CHAR_W * 2, TERM_CHAR_H - 1, C_DARKBG);
  // new cursor
  tft.fillRect(baseX + termCol * TERM_CHAR_W, yy + 1, TERM_CHAR_W - 2, TERM_CHAR_H - 2, C_GREEN);
  // if line now empty, erase the prefix too
  if (termLines[termRow].length() == 0) {
    tft.fillRect(0, yy, TERM_PAD_X + PREFIX_PX, TERM_CHAR_H, C_DARKBG);
  }
}

void termFullRedraw() {
  tft.fillScreen(C_DARKBG);
  termDrawHeader();
  for (uint8_t r = 0; r < TERM_ROWS; r++) termDrawLine(r);
}

void termScroll() {
  for (uint8_t i = 0; i < TERM_ROWS - 1; i++) termLines[i] = termLines[i + 1];
  termLines[TERM_ROWS - 1] = "";
  termRow = TERM_ROWS - 1;
  termFullRedraw();
}

void termAddChar(char c) {
  if (c == '\n' || c == '\r') {
    const int16_t yy = TERM_PAD_Y + 4 + termRow * TERM_CHAR_H;
    // erase cursor on current row
    tft.fillRect(TERM_PAD_X + PREFIX_PX + termCol * TERM_CHAR_W,
                 yy + 1, TERM_CHAR_W, TERM_CHAR_H - 1, C_DARKBG);
    termRow++; termCol = 0;
    if (termRow >= TERM_ROWS) { termScroll(); return; }
    termDrawLine(termRow);  // draws prefix on new line
  } else if (c == '\b' || c == 127) {
    if (termCol > 0) {
      termCol--;
      termLines[termRow].remove(termLines[termRow].length() - 1);
      termDrawBackspace();
    }
  } else if (c >= 32 && c < 127) {
    if (termCol >= TERM_COLS) {
      termRow++; termCol = 0;
      if (termRow >= TERM_ROWS) { termScroll(); return; }
    }
    // draw prefix on first char of this line
    if (termCol == 0) termDrawPrefix(TERM_PAD_Y + 4 + termRow * TERM_CHAR_H);
    termLines[termRow] += c;
    termCol++;
    termDrawLastChar();
  }
}

// ═════════════════════════════════════════════════════════════
//  ANIMATIONS
// ═════════════════════════════════════════════════════════════

void animNormalEyes() {
  busy = true;
  const int16_t offs[] = {-16, 16, -16, 16, 0};
  for (uint8_t i = 0; i < 5; i++) { drawNormalEyes(offs[i]); delay(speedMs(80)); }
  drawNormalEyes(0, true);  delay(speedMs(100));
  drawNormalEyes(0, false); delay(speedMs(70));
  drawNormalEyes(0, true);  delay(speedMs(70));
  drawNormalEyes(0, false);
  busy = false;
}

void animSquishEyes() {
  busy = true;
  for (uint8_t i = 0; i < 3; i++) {
    drawSquishEyes(false); delay(speedMs(160));
    drawSquishEyes(true);  delay(speedMs(100));
  }
  drawSquishEyes(false);
  busy = false;
}

void animLogoReveal() {
  busy = true;
  tft.fillScreen(animBgColor);
  for (uint16_t i = 0; i < LOGO_SEG_COUNT; i++) {
    int16_t x1 = pgm_read_word(&LOGO_SEGS[i][0]);
    int16_t y1 = pgm_read_word(&LOGO_SEGS[i][1]);
    int16_t x2 = pgm_read_word(&LOGO_SEGS[i][2]);
    int16_t y2 = pgm_read_word(&LOGO_SEGS[i][3]);
    tft.drawLine(x1, y1, x2, y2, C_WHITE);
    tft.drawLine(x1 + 1, y1, x2 + 1, y2, C_WHITE);
    if (i % 4 == 0) { server.handleClient(); delay(speedMs(8)); }
  }
  drawLogoFilled(animBgColor, C_WHITE);
  delay(1500);
  busy = false;
}

// ═════════════════════════════════════════════════════════════
//  WEB PAGE
// ═════════════════════════════════════════════════════════════
const char INDEX_HTML[] PROGMEM = R"rawhtml(
<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1,user-scalable=no">
<title>Clawd Mochi</title>
<style>
*{box-sizing:border-box;margin:0;padding:0;-webkit-tap-highlight-color:transparent}
body{background:#1c1c20;font-family:'Courier New',monospace;color:#e8e4dc;
  display:flex;flex-direction:column;align-items:center;
  padding:20px 14px 52px;gap:14px;min-height:100vh}

.hdr{text-align:center;padding:2px 0 4px}
.mascot{font-size:15px;color:#c96a3e;line-height:1.3;font-weight:bold;
  font-family:'Courier New',monospace;display:block;letter-spacing:1px}
.sitename{font-size:10px;color:#5a5048;margin-top:8px;letter-spacing:3px}

.sec{width:100%;max-width:390px;font-size:10px;color:#8a8278;
  letter-spacing:2px;font-weight:bold;padding:0 2px}

/* Busy bar */
.busy{width:100%;max-width:390px;height:2px;background:#2e2a28;
  border-radius:1px;overflow:hidden;opacity:0;transition:opacity .2s}
.busy.show{opacity:1}
.busy-i{height:100%;width:30%;background:#c96a3e;border-radius:1px;
  animation:sl 1s linear infinite}
@keyframes sl{0%{margin-left:-30%}100%{margin-left:100%}}

/* Controls */
.ctrl{display:flex;gap:8px;width:100%;max-width:390px}
.cbtn{flex:1;background:#252428;border:1.5px solid #38343a;border-radius:10px;
  color:#b8b4ac;font-family:'Courier New',monospace;font-size:11px;font-weight:bold;
  padding:12px 4px;cursor:pointer;text-align:center;transition:all .12s}
.cbtn:active:not(:disabled){transform:scale(.94)}
.cbtn:disabled{opacity:.3;cursor:default}
.cbtn.on{border-color:#c96a3e;color:#c96a3e;background:#201408}
.cbtn.dim{border-color:#2e2a28;color:#4a4540}

/* View grid */
.vgrid{display:grid;grid-template-columns:1fr 1fr;gap:8px;width:100%;max-width:390px}
.vbtn{background:#252428;border:1.5px solid #38343a;border-radius:12px;
  color:#d8d4cc;font-family:'Courier New',monospace;
  padding:14px 6px 10px;cursor:pointer;text-align:center;
  transition:all .12s;user-select:none}
.vbtn:active:not(:disabled){transform:scale(.94)}
.vbtn:disabled{opacity:.3;cursor:default}
.vbtn .ic{font-size:20px;display:block;margin-bottom:4px;line-height:1;color:#c96a3e}
.vbtn .nm{font-size:12px;font-weight:bold;color:#e8e4dc}
.vbtn .ht{font-size:9px;color:#8a8278;margin-top:3px}
.vbtn.active{border-color:#c96a3e;background:#201408}
.vbtn[data-v="1"].active{border-color:#c96a3e;background:#201408}
.vbtn[data-v="2"].active{border-color:#4a8acd;background:#0c1628}
.vbtn[data-v="3"].active{border-color:#38343a;background:#201c18}

/* Speed slider */
.speed-row{width:100%;max-width:390px;display:flex;align-items:center;gap:10px}
.sl{font-size:10px;color:#6a6058;white-space:nowrap;min-width:36px}
input[type=range]{flex:1;accent-color:#c96a3e;cursor:pointer;height:20px}
.sv{font-size:11px;color:#c96a3e;min-width:44px;text-align:right;font-weight:bold}

/* Terminal */
.twrap{width:100%;max-width:390px;display:none;flex-direction:column;gap:8px}
.twrap.open{display:flex}
.thdr{display:flex;justify-content:space-between;align-items:center}
.tttl{font-size:11px;color:#28b878;letter-spacing:1px;font-weight:bold}
.tx{background:#0c1e12;border:2px solid #1a4828;border-radius:9px;
  color:#28b878;font-family:'Courier New',monospace;font-size:13px;
  font-weight:bold;padding:10px 18px;cursor:pointer}
.tx:active{background:#081410}
.trow{display:flex;gap:6px}
.tin{flex:1;background:#0c1018;border:1.5px solid #1a2820;border-radius:9px;
  color:#40d880;font-family:'Courier New',monospace;font-size:15px;
  padding:11px;outline:none}
.tin::placeholder{color:#2a3828}
.tgo{background:#1a9060;border:none;border-radius:9px;color:#fff;
  font-family:'Courier New',monospace;font-size:22px;font-weight:bold;
  padding:11px 16px;cursor:pointer;min-width:52px}
.tgo:active{background:#0f6040}

/* Canvas */
.cwrap{width:100%;max-width:390px;background:#222028;border:1.5px solid #38343a;
  border-radius:12px;padding:12px;flex-direction:column;gap:10px;display:none}
.cwrap.open{display:flex}
.crow{display:flex;gap:8px}
.ci{display:flex;flex-direction:column;align-items:center;gap:4px;flex:1}
.cl{font-size:10px;color:#7a7068;letter-spacing:1px;font-weight:bold}
.cs{width:100%;height:38px;border-radius:7px;border:1.5px solid #38343a;cursor:pointer;padding:0}
.dacts{display:flex;gap:7px}
.db{flex:1;background:#1c1820;border:1.5px solid #38343a;border-radius:9px;
  color:#c0bab8;font-family:'Courier New',monospace;font-size:11px;
  font-weight:bold;padding:11px 4px;cursor:pointer;transition:all .12s}
.db:active{transform:scale(.95);background:#281838}
.db.hi{border-color:#c96a3e;color:#c96a3e}
canvas{width:100%;border-radius:8px;border:1.5px solid #38343a;
  touch-action:none;cursor:crosshair;display:block}

/* Toast */
.toast{position:fixed;bottom:18px;left:50%;transform:translateX(-50%);
  background:#252428;border:1.5px solid #38343a;border-radius:9px;
  font-size:12px;color:#d8d4cc;padding:7px 16px;opacity:0;
  transition:opacity .18s;pointer-events:none;white-space:nowrap;z-index:99}
.toast.show{opacity:1}
</style>
</head>
<body>

<div class="hdr">
  <span class="mascot">&#x2590;&#x259B;&#x2588;&#x2588;&#x2588;&#x259C;&#x258C;<br>&#x259C;&#x2588;&#x2588;&#x2588;&#x2588;&#x2588;&#x259B;<br>&#x2598;&#x2598;&nbsp;&#x259D;&#x259D;</span>
  <div class="sitename">CLAWD &middot; MOCHI &middot; 控制台</div>
</div>

<div class="busy" id="busy"><div class="busy-i"></div></div>

<div class="sec">// 控制</div>
<div class="ctrl">
  <button class="cbtn on" id="blBtn" onclick="toggleBL()">&#9728; 屏幕开</button>
  <button class="cbtn on" id="moodBtn" onclick="toggleMood()">&#127752; 心情灯开</button>
  <button class="cbtn on" id="autoBtn" onclick="toggleAuto()">&#8635; 自动切换开</button>
</div>

<div class="sec">// 视图</div>
<div class="vgrid">
  <button class="vbtn active" data-v="0" onclick="setView(0)">
    <span class="ic">&#9632; &#9632;</span>
    <span class="nm">普通眼睛</span>
    <span class="ht">摇摆 + 眨眼</span>
  </button>
  <button class="vbtn" data-v="1" onclick="setView(1)">
    <span class="ic">&gt; &lt;</span>
    <span class="nm">眯眼</span>
    <span class="ht">开 / 合</span>
  </button>
  <button class="vbtn" data-v="2" onclick="setView(2)">
    <span class="ic">{ }</span>
    <span class="nm">Claude Code</span>
    <span class="ht">打开终端</span>
  </button>
  <button class="vbtn" data-v="3" onclick="toggleCanvas()">
    <span class="ic">&#11035;</span>
    <span class="nm">画板</span>
    <span class="ht">在屏幕上画画</span>
  </button>
  <button class="vbtn" data-v="4" onclick="setView(4)" style="grid-column:span 2">
    <span class="ic">&#128202;</span>
    <span class="nm">状态</span>
    <span class="ht">Claude 工作状态（心情灯）</span>
  </button>
</div>

<div style="text-align:center;margin:10px 0">
  <a href="/wifi" style="color:#da1100;font-size:12px;text-decoration:none">&#128225; WiFi 设置与网络</a>
</div>

<div class="sec">// 速度</div>
<div class="speed-row">
  <span class="sl">慢</span>
  <input type="range" id="spd" min="1" max="3" value="1" step="1" oninput="setSpeed(this.value)">
  <span class="sv" id="spdV">慢</span>
</div>

<div class="ctrl">
  <div class="ci" style="flex:1;display:flex;flex-direction:column;gap:4px;align-items:stretch">
    <span class="cl" style="font-size:10px;color:#8a8278;letter-spacing:1px;font-weight:bold;text-align:center">背景色</span>
    <input type="color" class="cs" id="bgCol" value="#aa4818" oninput="onBgChange(this.value)">
  </div>
  <div class="ci" style="flex:1;display:flex;flex-direction:column;gap:4px;align-items:stretch">
    <span class="cl" style="font-size:10px;color:#8a8278;letter-spacing:1px;font-weight:bold;text-align:center">画笔颜色</span>
    <input type="color" class="cs" id="penCol" value="#000000">
  </div>
</div>

<div class="sec">// 终端</div>
<div class="twrap" id="twrap">
  <div class="thdr">
    <span class="tttl">&#9658; clawd:~$</span>
    <button class="tx" onclick="closeTerm()">&#x2715; 退出终端</button>
  </div>
  <div class="trow">
    <input class="tin" id="tin" type="text" placeholder="在此输入..."
           autocomplete="off" autocorrect="off" autocapitalize="off" spellcheck="false">
    <button class="tgo" onclick="termEnter()">&#8629;</button>
  </div>
</div>

<div class="cwrap" id="cwrap">
  <div class="dacts">
    <button class="db hi" onclick="clearAll()">&#11035; 清空</button>
    <button class="db" style="border-color:#28b878;color:#28b878" onclick="toggleCanvas()">&#10003; 完成</button>
  </div>
  <canvas id="cvs" width="320" height="240"></canvas>
</div>

<div class="toast" id="toast"></div>

<script>
let activeView  = 0;
let termOpen    = false;
let canvasOpen  = false;
let blOn        = true;
let isBusy      = false;
let drawing     = false;
let lastX = 0, lastY = 0;
let tt;

const spdLabels = ['','慢','中','快'];

// ── Toast ──────────────────────────────────────────────────────
function toast(msg, ok=true) {
  const el = document.getElementById('toast');
  el.textContent = msg;
  el.style.borderColor = ok ? '#28b878' : '#c96a3e';
  el.classList.add('show');
  clearTimeout(tt);
  tt = setTimeout(() => el.classList.remove('show'), 1300);
}

// ── Busy ────────────────────────────────────────────────────────
function setBusy(b) {
  isBusy = b;
  document.getElementById('busy').classList.toggle('show', b);
  const locked = b || termOpen;
  document.querySelectorAll('.vbtn').forEach(el => {
    // when canvas open, keep canvas btn (data-v=3) active so user can exit
    el.disabled = canvasOpen ? parseInt(el.dataset.v) !== 3 : locked;
  });
  document.querySelectorAll('.lbtn').forEach(el => el.disabled = locked || canvasOpen);
  document.querySelectorAll('.cbtn').forEach(el => {
    if (el.id !== 'blBtn') el.disabled = locked;
  });
}

// ── HTTP ────────────────────────────────────────────────────────
async function req(path) {
  try { const r = await fetch(path); return r.ok; }
  catch(e) { toast('连接失败', false); return false; }
}

async function waitNotBusy() {
  for (let i = 0; i < 100; i++) {
    try {
      const r = await fetch('/state');
      const j = await r.json();
      if (!j.busy) return;
    } catch(e) {}
    await new Promise(r => setTimeout(r, 150));
  }
}

// ── Background colour ───────────────────────────────────────────
async function onBgChange(hex) {
  if (canvasOpen) {
    await req('/draw/clear?bg=' + encodeURIComponent(hex));
  } else {
    await req('/redraw?bg=' + encodeURIComponent(hex));
  }
  redrawCanvas(hex);
}

// ── Speed ───────────────────────────────────────────────────────
async function setSpeed(v) {
  document.getElementById('spdV').textContent = spdLabels[v];
  await req('/speed?v=' + v);
}

// ── Views ───────────────────────────────────────────────────────
async function setView(v) {
  if (isBusy || termOpen || canvasOpen) return;
  if (v === 3) { toggleCanvas(); return; }  // canvas button in grid
  const keys = ['w','s','d','x','x'];  // index 4 = status (uses 'x')
  if (v === 4) {
    if (!await req('/cmd?k=x')) return;
    activeView = 4;
    document.querySelectorAll('.vbtn').forEach(b =>
      b.classList.toggle('active', parseInt(b.dataset.v) === 4));
    toast('状态视图');
    return;
  }
  if (!await req('/cmd?k=' + keys[v])) return;
  activeView = v;
  document.querySelectorAll('.vbtn').forEach(b =>
    b.classList.toggle('active', parseInt(b.dataset.v) === v));
  if (v === 2) {
    termOpen = true;
    document.getElementById('twrap').classList.add('open');
    setBusy(false);   // re-run to apply termOpen lock
    setBusy(false);
    document.querySelectorAll('.vbtn,.lbtn').forEach(b => b.disabled = true);
    const cvb = document.getElementById('cvBtn'); if (cvb) cvb.disabled = true;
    document.getElementById('tin').focus();
    toast('终端已打开');
    return;
  }
  setBusy(true);
  await waitNotBusy();
  setBusy(false);
}

// ── Logo animations (kept for startup, not exposed in UI) ──────

// ── Backlight ───────────────────────────────────────────────────
async function toggleBL() {
  blOn = !blOn;
  await req('/backlight?on=' + (blOn ? 1 : 0));
  const b = document.getElementById('blBtn');
  b.textContent = blOn ? '\u2600 \u5c4f\u5e55\u5f00' : '\u25cb \u5c4f\u5e55\u5173';
  b.classList.toggle('on', blOn);
  b.classList.toggle('dim', !blOn);
}

// ── Mood light + auto-switch toggles ───────────────────────────
async function toggleMood() {
  const btn = document.getElementById('moodBtn');
  const on = btn.classList.contains('on');
  await fetch('/mood?on=' + (on ? 0 : 1));
  btn.textContent = on ? '\u{1F30A} 心情灯关' : '\u{1F308} 心情灯开';
  btn.classList.toggle('on', !on);
  btn.classList.toggle('dim', on);
}
async function toggleAuto() {
  const btn = document.getElementById('autoBtn');
  const on = btn.classList.contains('on');
  await fetch('/autosw?on=' + (on ? 0 : 1));
  btn.textContent = on ? '自动切换关' : '自动切换开';
  btn.classList.toggle('on', !on);
  btn.classList.toggle('dim', on);
}

// ── Canvas toggle ───────────────────────────────────────────────
async function toggleCanvas() {
  canvasOpen = !canvasOpen;
  document.getElementById('cwrap').classList.toggle('open', canvasOpen);
  const b = document.getElementById('cvBtn');
  if (b) { b.classList.toggle('on', canvasOpen); b.textContent = canvasOpen ? '\u2b1b \u753b\u677f\u5f00' : '\u2b1b \u753b\u677f'; }
  // highlight the canvas vbtn (data-v=3) in the grid
  document.querySelectorAll('.vbtn').forEach(btn =>
    btn.classList.toggle('active', canvasOpen && parseInt(btn.dataset.v) === 3));
  await req('/canvas?on=' + (canvasOpen ? 1 : 0));
  if (canvasOpen) {
    const bg = document.getElementById('bgCol').value;
    redrawCanvas(bg);
    await req('/draw/clear?bg=' + encodeURIComponent(bg));
    // lock all other buttons
    document.querySelectorAll('.vbtn,.lbtn').forEach(b => b.disabled = true);
    toast('画板已激活');
  } else {
    setBusy(false);   // re-evaluate locks
    toast('画板已关闭');
  }
}

// ── Terminal ────────────────────────────────────────────────────
const tin = document.getElementById('tin');
let lastVal = '';
tin.addEventListener('input', async () => {
  const cur = tin.value, prev = lastVal;
  if (cur.length > prev.length) {
    await req('/char?c=' + encodeURIComponent(cur[cur.length - 1]));
  } else if (cur.length < prev.length) {
    await req('/char?c=%08');
  }
  lastVal = cur;
});
async function termEnter() {
  await req('/char?c=%0A');
  tin.value = ''; lastVal = ''; tin.focus();
}
tin.addEventListener('keydown', e => {
  if (e.key === 'Enter') { e.preventDefault(); termEnter(); }
});
async function closeTerm() {
  await req('/cmd?k=q');
  termOpen = false;
  document.getElementById('twrap').classList.remove('open');
  setBusy(false);
  toast('终端已关闭');
}

// ── Canvas drawing — send full stroke on finger lift ────────────
const cvs = document.getElementById('cvs');
const ctx = cvs.getContext('2d');
let strokePts = [];

function getPos(e) {
  const r = cvs.getBoundingClientRect();
  const sx = cvs.width / r.width, sy = cvs.height / r.height;
  const s = e.touches ? e.touches[0] : e;
  return { x: (s.clientX - r.left) * sx, y: (s.clientY - r.top) * sy };
}

function redrawCanvas(hex) {
  ctx.fillStyle = hex;
  ctx.fillRect(0, 0, cvs.width, cvs.height);
}

function startDraw(e) {
  e.preventDefault();
  drawing = true;
  strokePts = [];
  const p = getPos(e); lastX = p.x; lastY = p.y;
  strokePts.push({ x: Math.round(p.x), y: Math.round(p.y) });
  // draw dot on canvas preview only — no display send yet
  ctx.beginPath(); ctx.arc(p.x, p.y, 2, 0, Math.PI * 2);
  ctx.fillStyle = document.getElementById('penCol').value; ctx.fill();
}
function moveDraw(e) {
  if (!drawing) return; e.preventDefault();
  const p = getPos(e);
  ctx.beginPath(); ctx.moveTo(lastX, lastY); ctx.lineTo(p.x, p.y);
  ctx.strokeStyle = document.getElementById('penCol').value;
  ctx.lineWidth = 4; ctx.lineCap = 'round'; ctx.stroke();
  strokePts.push({ x: Math.round(p.x), y: Math.round(p.y) });
  lastX = p.x; lastY = p.y;
}
async function endDraw(e) {
  if (!drawing) return; drawing = false;
  if (!canvasOpen || strokePts.length < 1) return;
  const pen = document.getElementById('penCol').value.replace('#', '');
  const pts = strokePts.map(p => p.x + ',' + p.y).join(';');
  await req('/draw/stroke?pen=' + pen + '&pts=' + encodeURIComponent(pts));
  strokePts = [];
}

cvs.addEventListener('mousedown',  startDraw);
cvs.addEventListener('mousemove',  moveDraw);
cvs.addEventListener('mouseup',    endDraw);
cvs.addEventListener('mouseleave', endDraw);
cvs.addEventListener('touchstart', startDraw, {passive:false});
cvs.addEventListener('touchmove',  moveDraw,  {passive:false});
cvs.addEventListener('touchend',   endDraw);

// Clear = clear both web canvas and display
async function clearAll() {
  const bg = document.getElementById('bgCol').value;
  redrawCanvas(bg);
  await req('/draw/clear?bg=' + encodeURIComponent(bg));
  toast('已清空');
}

// Init: sync speed and backlight from ESP32, reset bg to default
(async () => {
  try {
    const r = await fetch('/state');
    const j = await r.json();
    // Sync speed
    const spd = j.speed || 1;
    document.getElementById('spd').value = spd;
    document.getElementById('spdV').textContent = spdLabels[spd];
    // Sync backlight
    if (j.bl === false) {
      blOn = false;
      const b = document.getElementById('blBtn');
      b.textContent = '\u25cb \u5c4f\u5e55\u5173';
      b.classList.remove('on'); b.classList.add('dim');
    }
    // Sync mood light toggle
    if (j.mood === false) {
      const m = document.getElementById('moodBtn');
      m.textContent = '\u{1F30A} 心情灯关';
      m.classList.remove('on'); m.classList.add('dim');
    }
    // Sync auto-switch toggle
    if (j.autosw === false) {
      const a = document.getElementById('autoBtn');
      a.textContent = '自动切换关';
      a.classList.remove('on'); a.classList.add('dim');
    }
  } catch(e) {}
  // Always reset bg picker to default orange on page load
  document.getElementById('bgCol').value = '#aa4818';
  redrawCanvas('#aa4818');
})();
</script>
</body>
</html>
)rawhtml";

// ═════════════════════════════════════════════════════════════
//  WEB ROUTES
// ═════════════════════════════════════════════════════════════

void routeRoot() {
  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
  server.sendHeader("Pragma", "no-cache");
  server.send_P(200, "text/html", INDEX_HTML);
}

void routeCmd() {
  if (!server.hasArg("k") || server.arg("k").isEmpty()) {
    server.send(400, "application/json", "{\"e\":1}"); return;
  }
  const char c = server.arg("k")[0];
  lastInteractionMs = millis();  // wake from screensaver
  // If currently in screensaver, exit to previous view first
  if (currentView == VIEW_SCREENSAVER) currentView = previousView;

  if (termMode) {
    if (c == 'q') { termMode = false; drawCodeView(); }
    server.send(200, "application/json", "{\"ok\":1}"); return;
  }

  server.send(200, "application/json", "{\"ok\":1}");
  switch (c) {
    case 'w': currentView = VIEW_EYES_NORMAL; animNormalEyes(); break;
    case 's': currentView = VIEW_EYES_SQUISH; animSquishEyes(); break;
    case 'd':
      currentView = VIEW_CODE; drawCodeView();
      termMode = true; termClear(); termFullRedraw(); break;
    case 'a':
      currentView = VIEW_EYES_NORMAL;
      animLogoReveal();
      break;
    case 'x':   // show Status view
      currentView = VIEW_STATUS;
      lastInteractionMs = millis();
      statusFrameDrawn = false;  // force full repaint on (re)entry
      drawStatusView();
      break;
  }
}

void routeChar() {
  if (!termMode) { server.send(200, "application/json", "{\"ok\":1}"); return; }
  const String val = server.arg("c");
  if (val.length() > 0) termAddChar(val[0]);
  server.send(200, "application/json", "{\"ok\":1}");
}

void routeSpeed() {
  if (server.hasArg("v")) animSpeed = constrain(server.arg("v").toInt(), 1, 3);
  server.send(200, "application/json", "{\"ok\":1}");
}

// /redraw?bg=hex — set animBg and immediately redraw current view
void routeRedraw() {
  if (server.hasArg("bg")) {
    animBgColor = hexToRgb565(server.arg("bg"));
    drawBgColor = animBgColor;
  }
  switch (currentView) {
    case VIEW_EYES_NORMAL: drawNormalEyes(); break;
    case VIEW_EYES_SQUISH: drawSquishEyes(); break;
    case VIEW_CODE:        drawCodeView();   break;
    case VIEW_DRAW:        tft.fillScreen(drawBgColor); break;
    case VIEW_STATUS:      drawStatusView(); break;
    case VIEW_SCREENSAVER: drawScreenSaverView(); break;
  }
  server.send(200, "application/json", "{\"ok\":1}");
}

void routeCanvas() {
  const bool on = server.hasArg("on") && server.arg("on") == "1";
  if (on) { currentView = VIEW_DRAW; tft.fillScreen(drawBgColor); }
  server.send(200, "application/json", "{\"ok\":1}");
}

void routeDrawClear() {
  const String bg = server.hasArg("bg") ? server.arg("bg") : "#aa4818";
  drawBgColor = hexToRgb565(bg);
  animBgColor = drawBgColor;  // keep in sync
  currentView = VIEW_DRAW; termMode = false;
  tft.fillScreen(drawBgColor);
  server.send(200, "application/json", "{\"ok\":1}");
}

void routeDrawStroke() {
  if (!server.hasArg("pts") || !server.hasArg("pen")) {
    server.send(200, "application/json", "{\"ok\":1}"); return;
  }
  const uint16_t color = hexToRgb565(server.arg("pen"));
  const String   data  = server.arg("pts");
  currentView = VIEW_DRAW;

  struct Pt { int16_t x, y; };
  Pt prev = {-1, -1};
  int start = 0;
  while (start < (int)data.length()) {
    int semi = data.indexOf(';', start);
    if (semi == -1) semi = data.length();
    String entry = data.substring(start, semi);
    const int comma = entry.indexOf(',');
    if (comma > 0) {
      const int16_t x = entry.substring(0, comma).toInt();
      const int16_t y = entry.substring(comma + 1).toInt();
      if (prev.x >= 0) {
        tft.drawLine(prev.x, prev.y, x, y, color);
        tft.drawLine(prev.x + 1, prev.y, x + 1, y, color);
        tft.drawLine(prev.x, prev.y + 1, x, y + 1, color);
      } else {
        tft.fillCircle(x, y, 2, color);
      }
      prev = {x, y};
    }
    start = semi + 1;
  }
  server.send(200, "application/json", "{\"ok\":1}");
}

void routeBacklight() {
  setBacklight(server.hasArg("on") && server.arg("on") == "1");
  server.send(200, "application/json", "{\"ok\":1}");
}

// GET /mood?on=1|0 — toggle mood-light background
void routeMood() {
  moodLightEnabled = server.hasArg("on") && server.arg("on") == "1";
  prefs.begin("clawd", false);
  prefs.putBool("mood", moodLightEnabled);
  prefs.end();
  if (currentView == VIEW_STATUS) drawStatusView();
  server.send(200, "application/json", "{\"ok\":1}");
}

// GET /autosw?on=1|0 — toggle auto-switch to Status on important events
void routeAutoSw() {
  autoSwitchEnabled = server.hasArg("on") && server.arg("on") == "1";
  prefs.begin("clawd", false);
  prefs.putBool("autosw", autoSwitchEnabled);
  prefs.end();
  server.send(200, "application/json", "{\"ok\":1}");
}

// Convert RGB565 back to #RRGGBB for state endpoint
String rgb565ToHex(uint16_t c) {
  uint8_t r = ((c >> 11) & 0x1F) << 3;
  uint8_t g = ((c >> 5)  & 0x3F) << 2;
  uint8_t b = (c & 0x1F) << 3;
  char buf[8];
  snprintf(buf, sizeof(buf), "#%02x%02x%02x", r, g, b);
  return String(buf);
}

void routeState() {
  String j = "{\"view\":"; j += currentView;
  j += ",\"busy\":";   j += busy        ? "true" : "false";
  j += ",\"term\":";   j += termMode    ? "true" : "false";
  j += ",\"bl\":";     j += backlightOn ? "true" : "false";
  j += ",\"speed\":";  j += animSpeed;
  j += ",\"mood\":";   j += moodLightEnabled  ? "true" : "false";
  j += ",\"autosw\":"; j += autoSwitchEnabled ? "true" : "false";
  j += ",\"sta\":\"";
  if (WiFi.status() == WL_CONNECTED) { j += WiFi.SSID(); }
  j += "\",\"ip\":\"";
  if (WiFi.status() == WL_CONNECTED) { j += WiFi.localIP().toString(); }
  j += "\",\"rssi\":"; j += (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : 0;
  j += ",\"claude\":{";
  j += "\"state\":";    j += (int)claudeStatus.state;
  j += ",\"tool\":\"";   j += claudeStatus.tool;   j += "\"";
  j += ",\"task\":\"";   j += claudeStatus.task;   j += "\"";
  j += ",\"dur\":";      j += claudeStatus.duration_s;
  j += ",\"tu\":";       j += claudeStatus.tokens_used;
  j += ",\"tm\":";       j += claudeStatus.tokens_max;
  j += ",\"branch\":\"";j += claudeStatus.git_branch; j += "\"";
  j += ",\"project\":\"";j += claudeStatus.project;   j += "\"";
  j += ",\"model\":\"";  j += claudeStatus.model;    j += "\"";
  j += ",\"sdur\":";     j += claudeStatus.session_duration_s;
  j += ",\"tcnt\":";     j += claudeStatus.tool_count;
  j += "}}";
  server.send(200, "application/json", j);
}

// ═════════════════════════════════════════════════════════════
//  CLAUDE STATUS API (daemon → ESP32)
// ═════════════════════════════════════════════════════════════

// POST /api/status   { state, tool, task, duration_s, tokens_used, tokens_max, git_branch, project }
void routeApiStatus() {
  if (!server.hasArg("plain")) {
    server.send(400, "application/json", "{\"e\":\"no body\"}");
    return;
  }
  StaticJsonDocument<512> doc;
  DeserializationError err = deserializeJson(doc, server.arg("plain"));
  if (err) {
    server.send(400, "application/json", "{\"e\":\"bad json\"}");
    return;
  }

  ClaudeStatus s;
  const char* st = doc["state"] | "idle";
  if      (strcmp(st, "thinking") == 0) s.state = CS_THINKING;
  else if (strcmp(st, "awaiting") == 0) s.state = CS_AWAITING;
  else if (strcmp(st, "done")     == 0) s.state = CS_DONE;
  else if (strcmp(st, "error")    == 0) s.state = CS_ERROR;
  else                                  s.state = CS_IDLE;
  s.tool        = (const char*)(doc["tool"]       | "");
  s.task        = (const char*)(doc["task"]       | "");
  s.duration_s  = doc["duration_s"]  | 0;
  s.tokens_used = doc["tokens_used"] | 0;
  s.tokens_max  = doc["tokens_max"]  | 200000;
  s.git_branch  = (const char*)(doc["git_branch"] | "");
  s.project     = (const char*)(doc["project"]    | "");
  s.model       = (const char*)(doc["model"]       | "");
  s.session_duration_s = doc["session_duration_s"] | 0;
  s.tool_count  = doc["tool_count"]  | 0;
  s.last_update_ms = millis();

  applyStatus(s);
  server.send(200, "application/json", "{\"ok\":1}");
}

// POST /api/stats/daily   { date, tools_called, tokens_total, sessions, errors }
void routeApiStatsDaily() {
  if (!server.hasArg("plain")) {
    server.send(400, "application/json", "{\"e\":\"no body\"}");
    return;
  }
  StaticJsonDocument<256> doc;
  if (deserializeJson(doc, server.arg("plain"))) {
    server.send(400, "application/json", "{\"e\":\"bad json\"}");
    return;
  }
  dailyStats.date         = (const char*)(doc["date"] | "");
  dailyStats.tools_called = doc["tools_called"] | 0;
  dailyStats.tokens_total = doc["tokens_total"] | 0;
  dailyStats.sessions     = doc["sessions"]     | 0;
  dailyStats.errors       = doc["errors"]       | 0;
  dailyStats.valid        = true;

  // Persist to NVS so it survives reboot
  prefs.begin("clawd", false);
  prefs.putString("stats_date",   dailyStats.date);
  prefs.putUInt("stats_tools",    dailyStats.tools_called);
  prefs.putUInt("stats_tokens",   dailyStats.tokens_total);
  prefs.putUShort("stats_sessions", dailyStats.sessions);
  prefs.putUShort("stats_errors",   dailyStats.errors);
  prefs.end();

  if (currentView == VIEW_STATUS) drawStatusView();
  server.send(200, "application/json", "{\"ok\":1}");
}

// ═════════════════════════════════════════════════════════════
//  WIFI MANAGEMENT API
// ═════════════════════════════════════════════════════════════

uint8_t loadWifiCount() {
  prefs.begin("wifi", true);
  uint8_t n = prefs.getUChar("count", 0);
  prefs.end();
  return n;
}

void saveWifiCount(uint8_t n) {
  prefs.begin("wifi", false);
  prefs.putUChar("count", n);
  prefs.end();
}

bool loadWifiAt(uint8_t i, String& ssid, String& pass) {
  prefs.begin("wifi", true);
  String k_s = "s" + String(i);
  String k_p = "p" + String(i);
  ssid = prefs.getString(k_s.c_str(), "");
  pass = prefs.getString(k_p.c_str(), "");
  prefs.end();
  return ssid.length() > 0;
}

void saveWifiAt(uint8_t i, const String& ssid, const String& pass) {
  prefs.begin("wifi", false);
  String k_s = "s" + String(i);
  String k_p = "p" + String(i);
  prefs.putString(k_s.c_str(), ssid);
  prefs.putString(k_p.c_str(), pass);
  prefs.end();
}

void clearWifiAt(uint8_t i) {
  prefs.begin("wifi", false);
  String k_s = "s" + String(i);
  String k_p = "p" + String(i);
  prefs.remove(k_s.c_str());
  prefs.remove(k_p.c_str());
  prefs.end();
}

// Rebuild WiFiMulti from NVS-stored credentials.
void rebuildWifiMulti() {
  // WiFiMulti has no clear API on older cores; we just re-add all known.
  // Adding the same AP twice is harmless (it dedups internally).
  uint8_t n = loadWifiCount();
  for (uint8_t i = 0; i < n; i++) {
    String s, p;
    if (loadWifiAt(i, s, p)) {
      wifiMulti.addAP(s.c_str(), p.c_str());
    }
  }
}

// GET /wifi/list  → JSON [{ssid, connected}, ...]
void routeWifiList() {
  uint8_t n = loadWifiCount();
  String cur = (WiFi.status() == WL_CONNECTED) ? WiFi.SSID() : "";
  String j = "[";
  for (uint8_t i = 0; i < n; i++) {
    String s, p;
    if (!loadWifiAt(i, s, p)) continue;
    if (j.length() > 1) j += ",";
    j += "{\"ssid\":\""; j += s; j += "\",\"connected\":";
    j += (s == cur ? "true" : "false"); j += "}";
  }
  j += "]";
  server.send(200, "application/json", j);
}

// GET /wifi/scan  → JSON of nearby networks with RSSI
void routeWifiScan() {
  int n = WiFi.scanNetworks();
  String j = "[";
  uint8_t saved = loadWifiCount();
  for (int i = 0; i < n; i++) {
    if (j.length() > 1) j += ",";
    String ssid = WiFi.SSID(i);
    bool isSaved = false;
    for (uint8_t k = 0; k < saved; k++) {
      String s, p;
      if (loadWifiAt(k, s, p) && s == ssid) { isSaved = true; break; }
    }
    j += "{\"ssid\":\""; j += ssid;
    j += "\",\"rssi\":"; j += WiFi.RSSI(i);
    j += ",\"saved\":";  j += isSaved ? "true" : "false";
    j += "}";
  }
  j += "]";
  WiFi.scanDelete();
  server.send(200, "application/json", j);
}

// POST /wifi/add  { ssid, password }
void routeWifiAdd() {
  if (!server.hasArg("plain")) { server.send(400, "application/json", "{\"e\":\"no body\"}"); return; }
  StaticJsonDocument<256> doc;
  if (deserializeJson(doc, server.arg("plain"))) {
    server.send(400, "application/json", "{\"e\":\"bad json\"}"); return;
  }
  String ssid = (const char*)(doc["ssid"] | "");
  String pass = (const char*)(doc["password"] | "");
  if (ssid.length() == 0) { server.send(400, "application/json", "{\"e\":\"empty ssid\"}"); return; }

  uint8_t n = loadWifiCount();
  // Check if it already exists → update password
  for (uint8_t i = 0; i < n; i++) {
    String s, p;
    if (loadWifiAt(i, s, p) && s == ssid) {
      saveWifiAt(i, ssid, pass);
      rebuildWifiMulti();
      server.send(200, "application/json", "{\"ok\":1,\"updated\":1}"); return;
    }
  }
  // New entry
  if (n >= MAX_WIFI_CREDS) {
    server.send(400, "application/json", "{\"e\":\"full\"}"); return;
  }
  saveWifiAt(n, ssid, pass);
  saveWifiCount(n + 1);
  wifiMulti.addAP(ssid.c_str(), pass.c_str());
  // try connect immediately
  wifiMulti.run(5000);
  server.send(200, "application/json", "{\"ok\":1}");
}

// POST /wifi/delete  { ssid }
void routeWifiDelete() {
  if (!server.hasArg("plain")) { server.send(400, "application/json", "{\"e\":\"no body\"}"); return; }
  StaticJsonDocument<128> doc;
  if (deserializeJson(doc, server.arg("plain"))) {
    server.send(400, "application/json", "{\"e\":\"bad json\"}"); return;
  }
  String ssid = (const char*)(doc["ssid"] | "");
  uint8_t n = loadWifiCount();
  int8_t found = -1;
  for (uint8_t i = 0; i < n; i++) {
    String s, p;
    if (loadWifiAt(i, s, p) && s == ssid) { found = i; break; }
  }
  if (found < 0) { server.send(404, "application/json", "{\"e\":\"not found\"}"); return; }
  // Shift remaining entries down
  for (uint8_t i = found; i + 1 < n; i++) {
    String s, p;
    loadWifiAt(i + 1, s, p);
    saveWifiAt(i, s, p);
  }
  clearWifiAt(n - 1);
  saveWifiCount(n - 1);
  // disconnect if currently on it; rebuild Multi (note: WiFiMulti can't forget,
  // but adding fewer APs after rebuild is harmless since the deleted SSID
  // simply won't be retried as a saved network)
  if (WiFi.status() == WL_CONNECTED && WiFi.SSID() == ssid) {
    WiFi.disconnect();
    wifiMulti.run(5000);
  }
  server.send(200, "application/json", "{\"ok\":1}");
}

// GET /wifi  → tiny config page
void routeWifiPage() {
  static const char WIFI_HTML[] PROGMEM = R"rawhtml(
<!doctype html><html><head><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1">
<title>Clawd WiFi</title>
<style>
body{font-family:-apple-system,sans-serif;background:#0a0c10;color:#eee;padding:16px;margin:0;max-width:480px;margin:auto}
h1{color:#da1100;font-size:22px;margin:0 0 12px}
h2{font-size:14px;color:#888;margin:20px 0 8px;text-transform:uppercase}
.card{background:#15181d;border-radius:10px;padding:12px;margin-bottom:10px;display:flex;justify-content:space-between;align-items:center}
.tag{font-size:11px;background:#0a8;color:#000;padding:2px 6px;border-radius:4px;margin-left:6px}
button{background:#da1100;color:#fff;border:0;padding:10px 16px;border-radius:8px;cursor:pointer;font-size:14px}
button.secondary{background:#333}
input{background:#0a0c10;color:#eee;border:1px solid #333;padding:10px;border-radius:8px;width:calc(100% - 22px);margin-bottom:8px;font-size:14px}
.rssi{color:#888;font-size:12px}
a{color:#da1100}
</style></head><body>
<h1>🦀 Clawd Mochi · WiFi</h1>
<div id=current></div>
<h2>已保存的网络</h2>
<div id=saved></div>
<h2>附近的网络</h2>
<button onclick=scan() class=secondary>🔄 扫描</button>
<div id=scan></div>
<h2>手动添加</h2>
<input id=ssid placeholder=SSID>
<input id=pwd placeholder=密码 type=password>
<button onclick=add()>保存</button>
<p><a href=/>← 返回控制台</a></p>
<script>
async function load(){
  const r=await fetch('/state');const s=await r.json();
  document.getElementById('current').innerHTML=
    s.sta?`<div class=card>已连接：<b>${s.sta}</b>（${s.ip}，${s.rssi}dBm）</div>`
         :`<div class=card>未连接 — 仅热点模式</div>`;
  const list=await(await fetch('/wifi/list')).json();
  document.getElementById('saved').innerHTML=list.length?list.map(n=>
    `<div class=card><span>${n.ssid}${n.connected?'<span class=tag>已连接</span>':''}</span>
     <button class=secondary onclick="del('${n.ssid.replace(/'/g,"\\'")}')">删除</button></div>`).join(''):'<div class=card>暂无</div>';
}
async function scan(){
  document.getElementById('scan').innerHTML='扫描中…';
  const list=await(await fetch('/wifi/scan')).json();
  list.sort((a,b)=>b.rssi-a.rssi);
  document.getElementById('scan').innerHTML=list.map(n=>
    `<div class=card><span>${n.ssid} <span class=rssi>${n.rssi}dBm</span>${n.saved?'<span class=tag>已保存</span>':''}</span>
     <button onclick="pick('${n.ssid.replace(/'/g,"\\'")}')">使用</button></div>`).join('');
}
function pick(s){document.getElementById('ssid').value=s;document.getElementById('pwd').focus();}
async function add(){
  const ssid=document.getElementById('ssid').value;const pwd=document.getElementById('pwd').value;
  if(!ssid)return;
  await fetch('/wifi/add',{method:'POST',body:JSON.stringify({ssid,password:pwd})});
  document.getElementById('ssid').value='';document.getElementById('pwd').value='';
  setTimeout(load,500);
}
async function del(s){
  await fetch('/wifi/delete',{method:'POST',body:JSON.stringify({ssid:s})});
  load();
}
load();
</script></body></html>
)rawhtml";
  server.send_P(200, "text/html", WIFI_HTML);
}

void routeNotFound() { server.send(404, "text/plain", "未找到"); }

// ═════════════════════════════════════════════════════════════
//  SETUP
// ═════════════════════════════════════════════════════════════

void setup() {
  Serial.begin(115200);

  pinMode(TFT_BLK, OUTPUT);
  setBacklight(true);

  SPI.begin(8, -1, 10, TFT_CS);   // SCK=8, MOSI=10
  tft.init(240, 320);
  tft.setSPISpeed(40000000);
  tft.setRotation(1);
  initColours();

  // ── Boot splash ────────────────────────────────────────────
  tft.fillScreen(animBgColor);
  tft.setTextColor(C_WHITE); tft.setTextSize(3);
  tft.setCursor(DISP_W / 2 - 54, DISP_H / 2 - 22); tft.print("Clawd");
  tft.setCursor(DISP_W / 2 - 54, DISP_H / 2 + 14); tft.print("Mochi");
  delay(1200);

  // ── Logo shown once at startup ─────────────────────────────
  animLogoReveal();

  // ── Restore cached daily stats from NVS ────────────────────
  prefs.begin("clawd", true);
  dailyStats.date         = prefs.getString("stats_date", "");
  dailyStats.tools_called = prefs.getUInt("stats_tools", 0);
  dailyStats.tokens_total = prefs.getUInt("stats_tokens", 0);
  dailyStats.sessions     = prefs.getUShort("stats_sessions", 0);
  dailyStats.errors       = prefs.getUShort("stats_errors", 0);
  dailyStats.valid        = dailyStats.date.length() > 0;
  // Restore mood-light / auto-switch toggles
  moodLightEnabled  = prefs.getBool("mood",   true);
  autoSwitchEnabled = prefs.getBool("autosw", true);
  prefs.end();

  // ── Start WiFi (AP + STA dual mode) ────────────────────────
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID, AP_PASS);

  rebuildWifiMulti();
  // Always add the home/fallback networks from wifi_secrets.h so we connect
  // even with no saved NVS credentials (and even if AP provisioning fails).
  for (int i = 0; i < WIFI_GROUP_COUNT; i++) {
    wifiMulti.addAP(WIFI_GROUP[i].ssid, WIFI_GROUP[i].pass);
  }

  // NTP: sync real time (UTC+8 = Beijing) for the screensaver clock.
  // Non-blocking — syncs in the background once STA connects.
  configTime(8 * 3600, 0, "pool.ntp.org", "ntp1.aliyun.com");

  // Power-only stability: when powered from a charger (no USB host), the
  // radio init can be flaky and the first connect attempt often fails.
  // A short settle delay + a few retry attempts improve the success rate.
  // loop() also retries every 3s if STA stays down, so this is best-effort
  // and won't block server.begin() for too long if it keeps failing.
  delay(300);
  for (int i = 0; i < 3; i++) {
    if (wifiMulti.run(3000) == WL_CONNECTED) break;
    delay(400);
  }

  // ── WiFi info screen (stays until first web request) ───────
  tft.fillScreen(C_DARKBG);
  tft.fillRect(0, 0, DISP_W, 4, C_ORANGE);
  tft.setTextColor(C_WHITE);  tft.setTextSize(2);
  tft.setCursor(12, 16);  tft.print("WiFi: ClaWD-Mochi");
  tft.setTextColor(C_MUTED);  tft.setTextSize(1);
  tft.setCursor(12, 44);  tft.print("password: clawd1234");
  tft.setTextColor(C_WHITE);  tft.setTextSize(2);
  tft.setCursor(12, 68);  tft.print("AP IP:");
  tft.setTextColor(C_ORANGE); tft.setTextSize(2);
  tft.setCursor(12, 94);  tft.print("192.168.4.1");

  if (WiFi.status() == WL_CONNECTED) {
    tft.setTextColor(C_GREEN); tft.setTextSize(1);
    tft.setCursor(12, 124); tft.print("STA: " + WiFi.SSID());
    tft.setCursor(12, 138); tft.print("IP : " + WiFi.localIP().toString());
  } else {
    tft.setTextColor(C_MUTED); tft.setTextSize(1);
    tft.setCursor(12, 124); tft.print("STA: connect failed");
  }

  // ── Register routes ────────────────────────────────────────
  server.on("/",            HTTP_GET,  routeRoot);
  server.on("/cmd",         HTTP_GET,  routeCmd);
  server.on("/char",        HTTP_GET,  routeChar);
  server.on("/speed",       HTTP_GET,  routeSpeed);
  server.on("/redraw",      HTTP_GET,  routeRedraw);
  server.on("/canvas",      HTTP_GET,  routeCanvas);
  server.on("/draw/clear",  HTTP_GET,  routeDrawClear);
  server.on("/draw/stroke", HTTP_GET,  routeDrawStroke);
  server.on("/backlight",   HTTP_GET,  routeBacklight);
  server.on("/mood",        HTTP_GET,  routeMood);
  server.on("/autosw",      HTTP_GET,  routeAutoSw);
  server.on("/state",       HTTP_GET,  routeState);
  // ── Claude status API
  server.on("/api/status",      HTTP_POST, routeApiStatus);
  server.on("/api/stats/daily", HTTP_POST, routeApiStatsDaily);
  // ── WiFi management
  server.on("/wifi",         HTTP_GET,  routeWifiPage);
  server.on("/wifi/list",    HTTP_GET,  routeWifiList);
  server.on("/wifi/scan",    HTTP_GET,  routeWifiScan);
  server.on("/wifi/add",     HTTP_POST, routeWifiAdd);
  server.on("/wifi/delete",  HTTP_POST, routeWifiDelete);
  server.onNotFound(routeNotFound);
  server.begin();

  // ── mDNS: advertise a stable hostname so the daemon doesn't need a
  // hard-coded IP that changes with every router (UU→192.168.0.x,
  // 66yun_wuhan→192.168.100.x, …). The daemon resolves
  // clawd-mochi.local regardless of which network the device joins.
  if (MDNS.begin("clawd-mochi")) {
    MDNS.addService("http", "tcp", 80);
  }

  // ── Captive portal (DNS resolves anything to AP IP) ───────
  // Phones see "no internet" on the AP and auto-open 192.168.4.1
  dnsServer.start(DNS_PORT, "*", IPAddress(192, 168, 4, 1));

  // ── OTA wireless update ───────────────────────────────────
  // After first connection, you can re-upload via Arduino IDE
  // by selecting the network port (clawd-mochi.local).
  ArduinoOTA.setHostname("clawd-mochi");
  ArduinoOTA.onStart([]() {
    tft.fillScreen(C_DARKBG);
    tft.setTextColor(C_ORANGE); tft.setTextSize(2);
    tft.setCursor(20, 100); tft.print("OTA Update...");
  });
  ArduinoOTA.onProgress([](unsigned int p, unsigned int t) {
    int16_t pct = t > 0 ? (p * 100 / t) : 0;
    tft.fillRect(20, 130, 200, 16, C_DARKBG);
    tft.setTextColor(C_WHITE); tft.setTextSize(2);
    tft.setCursor(20, 130); tft.print(String(pct) + "%");
  });
  ArduinoOTA.onError([](ota_error_t e) {
    tft.fillScreen(MOOD_ERROR);
    tft.setTextColor(C_WHITE); tft.setTextSize(2);
    tft.setCursor(20, 100); tft.print("OTA Failed");
  });
  ArduinoOTA.begin();

  lastWifiScanMs = millis();
  lastInteractionMs = millis();
  // WiFi info stays on screen — first button press triggers setView/cmd
  // which will replace it with the correct view
}

// ═════════════════════════════════════════════════════════════
//  LOOP
// ═════════════════════════════════════════════════════════════

void loop() {
  server.handleClient();
  dnsServer.processNextRequest();  // captive portal
  ArduinoOTA.handle();              // OTA updates
  tickStateMachine();

  // Detect view switches: when we leave the screensaver, clear the
  // "last drawn view" so the next screensaver entry repaints its background
  // (other views overwrite the screen with their own fillScreen).
  if (currentView != VIEW_SCREENSAVER) {
    lastDrawnView = 255;
  }

  // WiFi keep-alive:
  //  - If STA is down, retry every 3s (fast recovery from drops).
  //  - If STA is up, rescan for strongest network every 5 min.
  uint32_t now = millis();
  bool staUp = (WiFi.status() == WL_CONNECTED);
  uint32_t interval = staUp ? WIFI_SCAN_INTERVAL_MS : 3000UL;
  if (now - lastWifiScanMs > interval) {
    lastWifiScanMs = now;
    // The WIFI_GROUP networks from wifi_secrets.h were added in setup, so
    // wifiMulti.run() always has a home network to fall back on even
    // if NVS has no saved creds.
    uint8_t s = wifiMulti.run();
    (void)s; // non-blocking; reselects strongest known network
  }
}
