/*
  ESP32-CAM AI Viewer

  AI-Thinker ESP32-CAM + 2.8" ILI9341 SPI display (320x240) with XPT2046 touch.
  Shows live video on the display; tap the screen (or type 't' in the Serial Monitor) to take a
  photo, send it to Google Gemini, and show what it sees. Photos and answers are saved to the
  onboard microSD card. WiFi is chosen on screen, and the network, password, and touch
  calibration are saved in flash (NVS).

  Controls:
    tap the screen (live video) or type t     = take a photo and have it evaluated
    tap Done (result screen)    or type n     = go back to live video
    tap Prev / Next             or type < / > = page through a long answer

    links to the components used
    Camera            https://www.amazon.com/dp/B08P2578LV?ref_=ppx_hzsearch_conn_dt_b_fed_asin_title_39
    Display           https://www.amazon.com/dp/B0CJHTMPXF?ref_=ppx_hzsearch_conn_dt_b_fed_asin_title_1
    Voltage regulator https://www.amazon.com/dp/B0DY6C94T7?ref_=ppx_hzsearch_conn_dt_b_fed_asin_title_2

*/

// ===========================
// Includes
// ===========================
// Libraries to install (Arduino IDE: Tools > Manage Libraries, or download from the links):
//   Adafruit GFX Library        https://github.com/adafruit/Adafruit-GFX-Library
//   Adafruit BusIO              https://github.com/adafruit/Adafruit_BusIO  (needed by Adafruit GFX)
//   Adafruit ILI9341            https://github.com/adafruit/Adafruit_ILI9341
//   TJpg_Decoder (Bodmer)       https://github.com/Bodmer/TJpg_Decoder
//   XPT2046_Touchscreen         https://github.com/PaulStoffregen/XPT2046_Touchscreen
//   Adafruit_ILI9341_Keypad     https://github.com/KrisKasprzak/Adafruit_ILI9341_Keypad
//                               (needs version 6.2+ for WiFi passwords longer than 11 characters)
// Included with the ESP32 board package (https://github.com/espressif/arduino-esp32):
//   esp_camera, WiFi, WiFiClientSecure, HTTPClient, SPI, Preferences, SD_MMC, mbedtls
#include <Arduino.h>
#include "esp_camera.h"               // ESP32 core: https://github.com/espressif/esp32-camera
#include <WiFi.h>                     // ESP32 core
#include <WiFiClientSecure.h>         // ESP32 core
#include <HTTPClient.h>               // ESP32 core
#include "mbedtls/base64.h"           // ESP32 core (ESP-IDF)
#include <SPI.h>                      // ESP32 core
#include <Adafruit_GFX.h>             // https://github.com/adafruit/Adafruit-GFX-Library
#include <Adafruit_ILI9341.h>         // https://github.com/adafruit/Adafruit_ILI9341
#include <TJpg_Decoder.h>             // https://github.com/Bodmer/TJpg_Decoder
#include <XPT2046_Touchscreen.h>      // https://github.com/PaulStoffregen/XPT2046_Touchscreen
#include <Preferences.h>              // ESP32 core, saves settings in flash (NVS)
#include "SD_MMC.h"                   // ESP32 core, onboard microSD slot
#include "Adafruit_ILI9341_Keypad.h"  // https://github.com/KrisKasprzak/Adafruit_ILI9341_Keypad (on-screen keyboard)
#include <Fonts/FreeSans18pt7b.h>     // fonts that ship with Adafruit_GFX
#include <Fonts/FreeSans12pt7b.h>
#include <Fonts/FreeSans9pt7b.h>
#include "board_config.h"             // select the camera model in board_config.h

// ESP_WEBCAM_VERSION major.minor
#define ESP_WEBCAM_VERSION 1.02

// ===========================
// Defines
// ===========================

// 2.8" ILI9341 SPI display (320x240)
// VCC -> 5V, GND -> GND, LED -> 3.3V, RESET -> 3.3V, SDO(MISO) -> not connected
// Touch (XPT2046) shares SCK/MOSI: T_CLK -> 14, T_DIN -> 13, T_DO -> 4, T_CS -> 12, T_IRQ -> not connected
// The onboard microSD slot shares GPIO 2, 14, 15 with the display (see the SD card functions), so
// the card and the display take turns: the card is only mounted while a file is being written.
#define TFT_SCK    14
#define TFT_MOSI   13
#define TFT_CS     15
#define TFT_DC     2
#define TOUCH_MISO 4   // T_DO
#define TOUCH_CS   12  // T_CS

// Touch calibration: crosshairs are drawn this far in from the corners
#define CAL_MARGIN 20

// SD card: each evaluation saves /AI_Photos/IMG_0001.jpg + IMG_0001.txt, IMG_0002..., never
// overwriting existing files
#define SD_FOLDER "/AI_Photos"

// LED flash: the bright white LED on the ESP32-CAM (GPIO 4). The pin is shared with the touch
// panel's T_DO line, so the flash is only used while a photo is taken (touch isn't read then).
#define FLASH_OFF          0    // FLASH_MODE choices (see User settings)
#define FLASH_AUTO         1
#define FLASH_ON           2
#define FLASH_LED_PIN      4    // same pin as TOUCH_MISO
#define FLASH_LEDC_CHANNEL 2    // PWM channel for the LED. Must be 0-7 (high-speed group): the camera
                                // clock runs on a low-speed timer that the core doesn't know about,
                                // and channels 8-15 would reconfigure it and stop the camera
#define FLASH_WARMUP_MS    500  // time for auto-exposure to adjust to the flash before the photo

// Text buffer sizes
#define SSID_LEN   33     // WiFi network name: up to 32 characters + terminator
#define PASS_LEN   65     // WiFi password: up to 64 characters + terminator
#define RESULT_MAX 4096   // Gemini's answer (longer answers are cut off)
#define RESP_MAX   32768  // Gemini's full JSON response (allocated in PSRAM while in use)

// UI color theme (RGB565)
#define RGB565(r, g, b) ((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3))
#define COL_BG      RGB565(12, 16, 28)     // near-black navy background
#define COL_PANEL   RGB565(30, 38, 58)     // cards / status panel
#define COL_LINE    RGB565(55, 65, 90)     // divider lines
#define COL_ACCENT  RGB565(0, 190, 255)    // cyan
#define COL_ACCENT2 RGB565(120, 70, 255)   // violet (gradient start)
#define COL_TEXT    RGB565(255, 255, 255)
#define COL_MUTED   RGB565(150, 160, 185)
#define COL_OK      RGB565(70, 225, 130)
#define COL_ERR     RGB565(255, 90, 80)
#define COL_ERR2    RGB565(200, 40, 90)

// Result screen layout
#define HEADER_H      46  // gradient header height (also used by the setup screens)
#define PAGE_BAR_H    44  // height of the button bar at the bottom
#define BTN_H         32
#define BTN_MARGIN    10

// WiFi Setup network list
#define MAX_NETS      20
#define NET_ROW_Y     52   // top of the first list row
#define NET_ROW_H     36   // row pitch
#define NETS_PER_PAGE 4

// WiFi Setup bottom bar: [<] [Rescan] [Back] [>]
#define WB_Y       202
#define WB_H       32
#define WB_PREV_X  10
#define WB_ARROW_W 50
#define WB_SCAN_X  68
#define WB_BACK_X  164
#define WB_TEXT_W  88
#define WB_NEXT_X  260

// ===========================
// User settings: enter your data here
// ===========================

// WiFi: normally chosen on the WiFi Setup screen and saved in flash (NVS).
// Optional default used only when nothing has been saved yet; leave "" to go straight to setup.
const char *DEFAULT_SSID = "YOUR_SSID";
const char *DEFAULT_PASSWORD = "YOUR_PASSWORD";

// AI image evaluation (Google Gemini API, free tier)
// Get a free key at https://aistudio.google.com/apikey
const char *GEMINI_API_KEY = "YOUR_API_KEY";  // keep your real key out of public repos
// Model to use. If you get sporadic results (frequent "busy" / HTTP 503 errors, rate-limit
// errors, or weak answers), try another free-tier model that accepts images:
//   "gemini-3.5-flash-lite"  fast and light, usually the least busy (default)
//   "gemini-3.1-flash-lite"  older lite model, often available when newer ones are busy
//   "gemini-3.6-flash"       better answers, slower, lower free limits
//   "gemini-3.8-flash"       newest and smartest Flash model, busiest at peak times
//   "gemini-2.5-flash"       previous generation, a good fallback
//   "gemini-2.5-flash-lite"  previous generation lite
//   "gemini-flash-latest"    alias that always points to the newest Flash model
// Free-tier models and limits change over time: see https://ai.google.dev/gemini-api/docs/pricing
// and your limits at https://aistudio.google.com/rate-limit
const char *GEMINI_MODEL = "gemini-3.5-flash-lite";
const char *AI_PROMPT = "What is in this photo? Answer in 1-3 short sentences, then list the main objects you see. Include an approximate cost for the items.";

// Camera picture settings, applied at startup.
// Tip: the controls on the camera's web page (http://<camera IP>) change these live, so you can
// find values you like there and then enter them here.
const int CAMERA_BRIGHTNESS = 2;     // -2 (darker) to 2 (brighter)
const int CAMERA_CONTRAST = 0;       // -2 to 2
const int CAMERA_SATURATION = 0;     // -2 (washed out) to 2 (vivid)
const int CAMERA_AE_LEVEL = 1;       // auto-exposure target, -2 to 2: higher lets in more light
const bool CAMERA_NIGHT_MODE = false;  // smarter auto-exposure (AEC DSP) for dim scenes
const gainceiling_t CAMERA_GAIN_CEILING = GAINCEILING_8X;  // max gain, GAINCEILING_2X to 128X:
                                                           // higher = brighter in the dark but grainier
const bool CAMERA_MIRROR = false;    // flip the picture left-right
const bool CAMERA_FLIP = false;      // flip the picture upside down

// LED flash for AI photos
const int FLASH_MODE = FLASH_AUTO;   // FLASH_OFF, FLASH_AUTO (only when the scene is dark), or FLASH_ON
const int FLASH_DARK_LEVEL = 90;     // auto mode: flash when the scene's average brightness (0-255)
                                     // is below this (each photo prints its level to the Serial Monitor).
                                     // Auto-exposure brightens dim rooms, so they often measure 70-100.
const int FLASH_BRIGHTNESS = 128;    // 1-255 (255 = full power, which is very bright and gets hot)

// ===========================
// Types
// (types used by function parameters must be declared before the first function, because the
// Arduino IDE inserts its automatic function prototypes there)
// ===========================

// Layout of a block of wrapped text
struct TextBox {
  const GFXfont *font;
  int x, w;       // left edge and width in pixels
  int baseline;   // baseline of the first line
  int lineH;      // line spacing
  int ascent;     // approx. cap height, used to center bullets
  int maxLines;   // lines per page
};

// Buttons on the result screen
enum ResultButton { BTN_NONE, BTN_PREV, BTN_DONE, BTN_NEXT };

// Setup-screen button styles
enum ButtonStyle { STYLE_NORMAL, STYLE_PRIMARY, STYLE_DISABLED };

// One network from a WiFi scan
struct WiFiNet {
  char ssid[SSID_LEN];
  int rssi;
  bool secure;
};

// A Stream that collects everything written to it in a fixed char buffer (always
// NUL-terminated). HTTPClient writes Gemini's response into it, so no String is needed.
// Data that doesn't fit is dropped.
class BufferStream : public Stream {
public:
  BufferStream(char *buf, size_t cap) : buf(buf), cap(cap) {
    clear();
  }
  void clear() {
    len = 0;
    buf[0] = '\0';
  }
  using Print::write;
  size_t write(uint8_t c) override {
    return write(&c, 1);
  }
  size_t write(const uint8_t *data, size_t size) override {
    size_t room = cap - 1 - len;
    size_t n = size < room ? size : room;
    memcpy(buf + len, data, n);
    len += n;
    buf[len] = '\0';
    return size;  // report everything as written so HTTPClient keeps reading to the end
  }
  int available() override {
    return 0;
  }
  int read() override {
    return -1;
  }
  int peek() override {
    return -1;
  }
private:
  char *buf;
  size_t cap;
  size_t len;
};

// Functions in app_httpd.cpp (the IDE only creates automatic prototypes for .ino files)
void startCameraServer();
void setupLedFlash();

// ===========================
// Global variables
// ===========================

// Touch calibration: raw XPT2046 readings at the left/right/top/bottom screen edges.
// Measured by the calibration screen (tap the splash screen at boot, or it runs automatically if
// nothing is saved) and stored in flash. A panel that counts backwards just gets left > right
// (or top > bottom) and map() handles it. These defaults are used until then.
long calLeft = 300, calRight = 3800, calTop = 300, calBottom = 3800;

// WiFi credentials (loaded from flash, set on the WiFi Setup screen)
char wifiSsid[SSID_LEN] = "";
char wifiPass[PASS_LEN] = "";

// Path without extension of the last saved photo, "" if not saved
char lastPhotoBase[48] = "";

// Gemini's answer (or an error message) for the last photo
char aiResult[RESULT_MAX] = "";

// Running totals used while measuring how dark a frame is
uint32_t lumaSum = 0;
uint32_t lumaCount = 0;

// True while the flash LED is driven by PWM (false: plain on/off fallback)
bool flashPwm = false;

// Result screen state
bool showingResult = false;    // true while an AI result is on screen (live video paused)
char resultBody[RESULT_MAX];   // answer text, cleaned up for the display font
bool resultOk = true;          // false when showing an error (red header)
TextBox resultBox;             // layout of the answer text
int resultPage = 0;
int resultPages = 1;

// ===========================
// Objects
// ===========================
Adafruit_ILI9341 tft(&SPI, TFT_DC, TFT_CS, -1);  // RESET is tied to 3.3V, so the library uses a software reset
XPT2046_Touchscreen touch(TOUCH_CS);             // no T_IRQ pin: the library polls touch pressure over SPI
Keyboard keyboard(&tft, &touch);                 // on-screen keyboard for the WiFi password
Preferences prefs;                               // flash storage for calibration, WiFi, and file counter

// ===========================
// setup()
// ===========================
void setup() {

  Serial.begin(115200);

  Serial.setDebugOutput(true);

  Serial.println("Starting...");
  Serial.println();

  setupDisplay();

  drawSplash();

  // Touch calibration: runs if nothing valid is saved, or if the user taps the splash screen
  if (!loadCalibration() || calibrationRequested()) {
    runTouchCalibration();
    drawSplash();
  }

  // On-screen keyboard (WiFi password), styled to match the rest of the UI
  keyboard.init(COL_BG, COL_TEXT, COL_PANEL, COL_BG, COL_ACCENT, &FreeSans12pt7b);
  keyboard.setDisplayColor(COL_TEXT, COL_BG);  // input box color, input text color
  keyboard.setCornerRadius(5);
  splashStatus("Starting camera...", COL_TEXT, "");

  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.frame_size = FRAMESIZE_UXGA;
  config.pixel_format = PIXFORMAT_JPEG;  // for streaming
  //config.pixel_format = PIXFORMAT_RGB565; // for face detection/recognition
  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.jpeg_quality = 12;
  config.fb_count = 1;

  // if PSRAM IC present, init with UXGA resolution and higher JPEG quality
  //                      for larger pre-allocated frame buffer.
  if (config.pixel_format == PIXFORMAT_JPEG) {
    if (psramFound()) {
      config.jpeg_quality = 10;
      config.fb_count = 2;
      config.grab_mode = CAMERA_GRAB_LATEST;
    } else {
      // Limit the frame size when PSRAM is not available
      config.frame_size = FRAMESIZE_SVGA;
      config.fb_location = CAMERA_FB_IN_DRAM;
    }
  } else {
    // Best option for face detection/recognition
    config.frame_size = FRAMESIZE_240X240;
#if CONFIG_IDF_TARGET_ESP32S3
    config.fb_count = 2;
#endif
  }

#if defined(CAMERA_MODEL_ESP_EYE)
  pinMode(13, INPUT_PULLUP);
  pinMode(14, INPUT_PULLUP);
#endif

  // camera init
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed with error 0x%x", err);
    char errLine[24];
    snprintf(errLine, sizeof(errLine), "Error 0x%x", err);
    splashStatus("Camera init failed!", COL_ERR, errLine);
    return;
  }

  sensor_t *s = esp_camera_sensor_get();
  // initial sensors are flipped vertically and colors are a bit saturated
  if (s->id.PID == OV3660_PID) {
    s->set_vflip(s, 1);        // flip it back
    s->set_brightness(s, 1);   // up the brightness just a bit
    s->set_saturation(s, -2);  // lower the saturation
  }
  // drop down frame size for higher initial frame rate (QVGA = 320x240, same as the display)
  if (config.pixel_format == PIXFORMAT_JPEG) {
    s->set_framesize(s, FRAMESIZE_QVGA);
  }

  // Picture settings (see User settings)
  s->set_brightness(s, CAMERA_BRIGHTNESS);
  s->set_contrast(s, CAMERA_CONTRAST);
  s->set_saturation(s, CAMERA_SATURATION);
  s->set_ae_level(s, CAMERA_AE_LEVEL);
  s->set_aec2(s, CAMERA_NIGHT_MODE ? 1 : 0);
  s->set_gainceiling(s, CAMERA_GAIN_CEILING);
  s->set_hmirror(s, CAMERA_MIRROR ? 1 : 0);
  s->set_vflip(s, CAMERA_FLIP ? 1 : 0);

#if defined(CAMERA_MODEL_M5STACK_WIDE) || defined(CAMERA_MODEL_M5STACK_ESP32CAM)
  s->set_vflip(s, 1);
  s->set_hmirror(s, 1);
#endif

#if defined(CAMERA_MODEL_ESP32S3_EYE)
  s->set_vflip(s, 1);
#endif

// Setup LED Flash if LED pin is defined in camera_pins.h
#if defined(LED_GPIO_NUM)
  setupLedFlash();
#endif

  // Check the SD card (mounting it resets the display, so redraw the splash afterwards)
  splashStatus("Checking SD card...", COL_TEXT, "");
  char sdInfo[40];
  bool sdOk = checkSDCard(sdInfo, sizeof(sdInfo));
  drawSplash();
  splashStatus(sdInfo, sdOk ? COL_OK : COL_ERR, sdOk ? "Photos and answers will be saved" : "Photos won't be saved");
  delay(1500);

  // WiFi Setup / Continue choice, then connect (runs WiFi Setup if needed)
  startWiFi();

  startCameraServer();

  Serial.print("Camera Ready! Use 'http://");
  Serial.print(WiFi.localIP());
  Serial.println("' to connect");
  Serial.println("Tap the screen or type 't' to evaluate a photo; tap Done or type 'n' to return to live video");

  IPAddress ip = WiFi.localIP();
  char ipLine[32];
  snprintf(ipLine, sizeof(ipLine), "http://%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
  splashStatus(ipLine, COL_OK, "Tap the screen to ask the AI");
  delay(3000);
  tft.fillScreen(COL_BG);
}

// ===========================
// loop()
// ===========================
void loop() {
  if (Serial.available()) {
    char c = Serial.read();
    if (c == 't' || c == 'T') {
      runEvaluation();
    } else if (c == 'n' || c == 'N') {
      backToLiveVideo();
    } else if (c == '<') {
      changePage(-1);
    } else if (c == '>') {
      changePage(1);
    }
  }

  // A tap anywhere during live video takes a photo. On the result screen only the buttons
  // respond: Prev / Next turn the page, Done goes back to live video.
  if (screenPressed()) {
    int x, y;
    touchPosition(x, y);
    waitForRelease();
    if (!showingResult) {
      runEvaluation();
    } else {
      switch (resultButtonAt(x, y)) {
        case BTN_PREV: changePage(-1); break;
        case BTN_NEXT: changePage(1); break;
        case BTN_DONE: backToLiveVideo(); break;
        default: break;  // tap on the text: ignore
      }
    }
  }

  if (showingResult) {
    delay(10);
  } else {
    showCameraFrame();
  }
}

// ===========================
// Display and live video
// ===========================

// Start the shared SPI bus, the display (landscape), and the touch controller, then clear the
// screen. Also called after every SD card access, because the card borrows the display's pins.
void setupDisplay() {
  pinMode(TOUCH_CS, OUTPUT);
  digitalWrite(TOUCH_CS, HIGH);  // keep the touch controller deselected until it's set up
  SPI.begin(TFT_SCK, TOUCH_MISO, TFT_MOSI, TFT_CS);
  tft.begin(40000000);  // lower to 20000000 if the picture is garbled
  touch.begin(SPI);     // shares the display's SPI bus
  tft.setRotation(1);   // landscape, 320x240
  touch.setRotation(1); // match the display orientation
  tft.fillScreen(COL_BG);
  tft.setTextSize(1);
  TJpgDec.setCallback(tftOutput);
}

// TJpg_Decoder callback: it hands us decoded blocks of pixels, which we push to the display
bool tftOutput(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bitmap) {
  if (y >= tft.height()) return false;  // stop decoding once we're off the bottom of the screen
  tft.drawRGBBitmap(x, y, bitmap, w, h);
  return true;
}

// Decode a JPEG onto the display, halving its size until it fits (e.g. VGA 640x480 -> 320x240)
void drawJpegFit(const uint8_t *jpg, size_t len, int w, int h) {
  uint8_t scale = 1;
  while (scale < 8 && (w / scale > tft.width() || h / scale > tft.height())) {
    scale *= 2;
  }
  TJpgDec.setJpgScale(scale);
  TJpgDec.drawJpg(0, 0, jpg, len);
}

// Grab a frame from the camera and draw it on the display
void showCameraFrame() {
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) return;
  if (fb->format == PIXFORMAT_JPEG) {
    drawJpegFit(fb->buf, fb->len, fb->width, fb->height);
  }
  esp_camera_fb_return(fb);
}

// ===========================
// LED flash
// ===========================

// TJpg_Decoder callback used by frameBrightness(): adds up the brightness of each pixel
// instead of drawing it
bool measureOutput(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bitmap) {
  uint32_t n = (uint32_t)w * h;
  for (uint32_t i = 0; i < n; i++) {
    uint16_t p = bitmap[i];
    int r = (p >> 11) << 3;          // RGB565 -> 0-255 per color
    int g = ((p >> 5) & 0x3F) << 2;
    int b = (p & 0x1F) << 3;
    lumaSum += (r * 3 + g * 6 + b) / 10;  // approximate perceived brightness
  }
  lumaCount += n;
  return true;
}

// Average brightness (0 = black, 255 = white) of a camera frame, decoded at 1/8 size
int frameBrightness(camera_fb_t *fb) {
  lumaSum = 0;
  lumaCount = 0;
  TJpgDec.setCallback(measureOutput);
  TJpgDec.setJpgScale(8);
  TJpgDec.drawJpg(0, 0, fb->buf, fb->len);
  TJpgDec.setCallback(tftOutput);
  return lumaCount ? lumaSum / lumaCount : 255;
}

// Should this photo use the flash? FLASH_AUTO measures the current live picture.
bool flashNeeded() {
  if (FLASH_MODE == FLASH_ON) return true;
  if (FLASH_MODE != FLASH_AUTO) return false;
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) return false;
  int level = frameBrightness(fb);
  esp_camera_fb_return(fb);
  Serial.printf("[AI] Scene brightness %d (flash below %d)\n", level, FLASH_DARK_LEVEL);
  return level < FLASH_DARK_LEVEL;
}

// Turn the flash LED on at FLASH_BRIGHTNESS. GPIO 4 is borrowed from the touch panel's T_DO
// line, so the SPI bus is released first: nothing may be drawn until flashOff().
void flashOn() {
  SPI.end();
  flashPwm = ledcAttachChannel(FLASH_LED_PIN, 5000, 8, FLASH_LEDC_CHANNEL);
  if (flashPwm) {
    ledcWrite(FLASH_LED_PIN, FLASH_BRIGHTNESS);
  } else {
    // PWM unavailable: plain on/off at full brightness instead
    Serial.println("[AI] Flash PWM setup failed, using full brightness");
    pinMode(FLASH_LED_PIN, OUTPUT);
    digitalWrite(FLASH_LED_PIN, HIGH);
  }
}

// Turn the flash LED off and give GPIO 4 back to the SPI bus (display and touch)
void flashOff() {
  if (flashPwm) {
    ledcWrite(FLASH_LED_PIN, 0);
    ledcDetach(FLASH_LED_PIN);
  } else {
    digitalWrite(FLASH_LED_PIN, LOW);
  }
  SPI.begin(TFT_SCK, TOUCH_MISO, TFT_MOSI, TFT_CS);
}

// ===========================
// Text helpers
// ===========================

// Append up to n characters of src to dst at position pos. dst never overflows and always
// stays NUL-terminated. Returns the new length of dst.
int appendText(char *dst, size_t dstSize, int pos, const char *src, int n) {
  while (n-- > 0 && *src && pos + 1 < (int)dstSize) {
    dst[pos++] = *src++;
  }
  dst[pos] = '\0';
  return pos;
}

// Append a Unicode code point to dst as UTF-8. Returns the new length of dst.
int appendUtf8(char *dst, size_t dstSize, int pos, uint32_t cp) {
  char b[3];
  int n;
  if (cp < 0x80) {
    b[0] = (char)cp;
    n = 1;
  } else if (cp < 0x800) {
    b[0] = (char)(0xC0 | (cp >> 6));
    b[1] = (char)(0x80 | (cp & 0x3F));
    n = 2;
  } else {
    b[0] = (char)(0xE0 | (cp >> 12));
    b[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    b[2] = (char)(0x80 | (cp & 0x3F));
    n = 3;
  }
  return appendText(dst, dstSize, pos, b, n);
}

// Width in pixels of the first n characters of s drawn in the given GFX font
int textWidthN(const char *s, int n, const GFXfont *font) {
  int w = 0;
  for (int i = 0; i < n && s[i]; i++) {
    uint8_t c = s[i];
    if (c >= font->first && c <= font->last) {
      w += font->glyph[c - font->first].xAdvance;
    }
  }
  return w;
}

// Width in pixels of a string drawn in the given GFX font
int textWidth(const char *s, const GFXfont *font) {
  return textWidthN(s, strlen(s), font);
}

// Copy s into out, shortened with "..." if needed so it fits in maxW pixels. Returns out.
char *fitText(const char *s, const GFXfont *font, int maxW, char *out, size_t outSize) {
  strlcpy(out, s, outSize);
  if (textWidth(out, font) <= maxW) return out;
  int dots = textWidth("...", font);
  int n = strlen(out);
  while (n > 0 && textWidthN(out, n, font) + dots > maxW) n--;
  if (n + 4 > (int)outSize) n = outSize - 4;
  strcpy(out + n, "...");
  return out;
}

// ===========================
// UI drawing helpers
// ===========================

// Print a string in the given font and color with its baseline at (x, baseline)
void printAt(const char *s, int x, int baseline, const GFXfont *font, uint16_t color) {
  tft.setFont(font);
  tft.setTextColor(color);
  tft.setCursor(x, baseline);
  tft.print(s);
}

// Print a string horizontally centered on the screen
void printCentered(const char *s, int baseline, const GFXfont *font, uint16_t color) {
  printAt(s, (tft.width() - textWidth(s, font)) / 2, baseline, font, color);
}

// Mix two RGB565 colors: t = 0 gives a, t = n gives b
uint16_t blend565(uint16_t a, uint16_t b, int t, int n) {
  int r = ((a >> 11) * (n - t) + (b >> 11) * t) / n;
  int g = (((a >> 5) & 0x3F) * (n - t) + ((b >> 5) & 0x3F) * t) / n;
  int bl = ((a & 0x1F) * (n - t) + (b & 0x1F) * t) / n;
  return (r << 11) | (g << 5) | bl;
}

// Horizontal gradient band across the screen width. Only columns x0..x1 are drawn, so part of
// the band can be repainted behind changing text (pass 0, tft.width() for the whole band).
void drawGradientBar(int y, int h, uint16_t left, uint16_t right, int x0, int x1) {
  int w = tft.width();
  if (x1 > w) x1 = w;
  for (int x = x0; x < x1; x++) {
    tft.drawFastVLine(x, y, h, blend565(left, right, x, w - 1));
  }
}

// Little camera icon centered on (cx, cy)
void drawCameraIcon(int cx, int cy) {
  tft.fillRoundRect(cx - 20, cy - 34, 26, 12, 3, COL_PANEL);  // viewfinder bump
  tft.fillRoundRect(cx - 42, cy - 26, 84, 54, 10, COL_PANEL);  // body
  tft.drawRoundRect(cx - 42, cy - 26, 84, 54, 10, COL_ACCENT);
  tft.fillCircle(cx, cy + 1, 20, COL_BG);  // lens
  tft.drawCircle(cx, cy + 1, 20, COL_ACCENT);
  tft.drawCircle(cx, cy + 1, 13, COL_ACCENT2);
  tft.fillCircle(cx, cy + 1, 6, COL_ACCENT);
  tft.fillCircle(cx + 30, cy - 15, 3, COL_OK);  // status light
}

// Start-up screen: gradient title bar, camera icon, and a status area we can update
void drawSplash() {
  tft.fillScreen(COL_BG);
  drawGradientBar(0, 44, COL_ACCENT2, COL_ACCENT, 0, tft.width());
  printCentered("ESP32-CAM  AI Viewer", 29, &FreeSans12pt7b, COL_TEXT);
  drawCameraIcon(tft.width() / 2, 104);
  tft.drawFastHLine(40, 152, tft.width() - 80, COL_LINE);
}

// Replace the splash screen's status area with one large line (in color1) and a smaller, muted
// second line (pass "" for none)
void splashStatus(const char *line1, uint16_t color1, const char *line2) {
  char buf[96];
  tft.fillRect(0, 156, tft.width(), tft.height() - 156, COL_BG);
  printCentered(fitText(line1, &FreeSans12pt7b, tft.width() - 16, buf, sizeof(buf)), 186, &FreeSans12pt7b, color1);
  if (line2[0]) {
    printCentered(fitText(line2, &FreeSans9pt7b, tft.width() - 16, buf, sizeof(buf)), 214, &FreeSans9pt7b, COL_MUTED);
  }
}

// Rounded status panel with a 3-step progress bar, drawn over the frozen photo
void drawStatus(const char *msg, int step) {
  const int x = 8, h = 38, w = tft.width() - 16;
  const int y = tft.height() - h - 8;
  char buf[96];
  tft.fillRoundRect(x, y, w, h, 10, COL_PANEL);
  tft.drawRoundRect(x, y, w, h, 10, COL_ACCENT);
  // progress track + fill
  const int px = x + 12, pw = w - 24;
  tft.fillRoundRect(px, y + 6, pw, 4, 2, COL_BG);
  tft.fillRoundRect(px, y + 6, pw * step / 3, 4, 2, COL_ACCENT);
  printAt(fitText(msg, &FreeSans9pt7b, w - 24, buf, sizeof(buf)), x + 12, y + 30, &FreeSans9pt7b, COL_TEXT);
}

// Report progress to both the Serial Monitor and the display
void aiStatus(const char *msg, int step) {
  Serial.printf("[AI] %s\n", msg);
  drawStatus(msg, step);
}

// Rounded text button: normal (panel), primary (filled accent), or disabled (grayed out)
void drawButton(int x, int y, int w, int h, const char *label, ButtonStyle style) {
  uint16_t fill = style == STYLE_PRIMARY ? COL_ACCENT : (style == STYLE_NORMAL ? COL_PANEL : COL_BG);
  uint16_t border = style == STYLE_DISABLED ? COL_LINE : COL_ACCENT;
  uint16_t fg = style == STYLE_PRIMARY ? COL_BG : (style == STYLE_NORMAL ? COL_TEXT : COL_LINE);
  tft.fillRoundRect(x, y, w, h, 8, fill);
  tft.drawRoundRect(x, y, w, h, 8, border);
  printAt(label, x + (w - textWidth(label, &FreeSans12pt7b)) / 2, y + h / 2 + 8, &FreeSans12pt7b, fg);
}

// Small button with a left/right triangle
void drawArrowButton(int x, int y, int w, int h, bool right, bool enabled) {
  tft.fillRoundRect(x, y, w, h, 8, enabled ? COL_PANEL : COL_BG);
  tft.drawRoundRect(x, y, w, h, 8, enabled ? COL_ACCENT : COL_LINE);
  int cx = x + w / 2, cy = y + h / 2;
  uint16_t c = enabled ? COL_ACCENT : COL_LINE;
  if (right) {
    tft.fillTriangle(cx - 5, cy - 8, cx - 5, cy + 8, cx + 7, cy, c);
  } else {
    tft.fillTriangle(cx + 5, cy - 8, cx + 5, cy + 8, cx - 7, cy, c);
  }
}

// Is (px, py) on the rectangle, with a few pixels of slack
bool inRect(int px, int py, int x, int y, int w, int h) {
  const int pad = 4;
  return px >= x - pad && px <= x + w + pad && py >= y - pad && py <= y + h + pad;
}

// Clear the screen and draw the gradient header used by the setup screens
void drawSetupHeader(const char *title) {
  tft.fillScreen(COL_BG);
  drawGradientBar(0, HEADER_H, COL_ACCENT2, COL_ACCENT, 0, tft.width());
  tft.drawFastHLine(0, HEADER_H, tft.width(), COL_TEXT);
  printAt(title, 14, 35, &FreeSans18pt7b, COL_TEXT);
}

// ===========================
// Touch input
// ===========================

// True when the screen is being pressed (two readings 20 ms apart, to ignore noise)
bool screenPressed() {
  if (!touch.touched()) return false;
  delay(20);
  return touch.touched();
}

// Convert a raw touch reading to screen coordinates using the calibration
void mapTouch(const TS_Point &p, int &x, int &y) {
  x = constrain(map(p.x, calLeft, calRight, 0, tft.width() - 1), 0, tft.width() - 1);
  y = constrain(map(p.y, calTop, calBottom, 0, tft.height() - 1), 0, tft.height() - 1);
}

// Screen position of the current touch (also printed to the Serial Monitor)
void touchPosition(int &x, int &y) {
  TS_Point p = touch.getPoint();
  mapTouch(p, x, y);
  Serial.printf("[AI] Screen tapped at x=%d, y=%d (raw %d, %d)\n", x, y, p.x, p.y);
}

// Wait (up to 3 s) for the finger to lift so one tap doesn't trigger twice
void waitForRelease() {
  unsigned long t0 = millis();
  while (touch.touched() && millis() - t0 < 3000) {
    delay(20);
  }
}

// Block until the screen is tapped; returns where
void waitForTap(int &x, int &y) {
  while (!screenPressed()) delay(10);
  touchPosition(x, y);
  waitForRelease();
}

// ===========================
// Touch calibration
// ===========================

// A real calibration is inside the 0 - 4095 range and spans most of it
bool touchCalibrationValid() {
  if (calLeft < 0 || calLeft > 4095 || calRight < 0 || calRight > 4095) return false;
  if (calTop < 0 || calTop > 4095 || calBottom < 0 || calBottom > 4095) return false;
  return abs(calRight - calLeft) > 1000 && abs(calBottom - calTop) > 1000;
}

// Load the saved calibration; returns false (and keeps the defaults) if there isn't a valid one
bool loadCalibration() {
  long l = calLeft, r = calRight, t = calTop, b = calBottom;
  prefs.begin("touch", false);
  calLeft = prefs.getLong("left", -1);
  calRight = prefs.getLong("right", -1);
  calTop = prefs.getLong("top", -1);
  calBottom = prefs.getLong("bottom", -1);
  prefs.end();
  if (touchCalibrationValid()) {
    Serial.printf("Touch calibration loaded: L %ld  R %ld  T %ld  B %ld\n", calLeft, calRight, calTop, calBottom);
    return true;
  }
  calLeft = l;
  calRight = r;
  calTop = t;
  calBottom = b;
  Serial.println("No saved touch calibration");
  return false;
}

// Save the current calibration to flash
void saveCalibration() {
  prefs.begin("touch", false);
  prefs.putLong("left", calLeft);
  prefs.putLong("right", calRight);
  prefs.putLong("top", calTop);
  prefs.putLong("bottom", calBottom);
  prefs.end();
}

// Calibration screen text: title, instruction, and a small step line, centered
void drawCalibrationText(const char *title, const char *line, const char *step) {
  tft.fillScreen(COL_BG);
  printCentered(title, 100, &FreeSans12pt7b, COL_ACCENT);
  tft.drawFastHLine(70, 112, tft.width() - 140, COL_LINE);
  printCentered(line, 140, &FreeSans12pt7b, COL_TEXT);
  if (step[0]) printCentered(step, 165, &FreeSans9pt7b, COL_MUTED);
}

// Draw a crosshair at (X, Y), wait for a press, and return the averaged raw touch reading
void getTouchTarget(int X, int Y, long &rawX, long &rawY) {
  tft.drawCircle(X, Y, 9, COL_ACCENT);
  tft.drawCircle(X, Y, 10, COL_ACCENT);
  tft.drawFastHLine(X - 16, Y, 33, COL_TEXT);
  tft.drawFastVLine(X, Y - 16, 33, COL_TEXT);
  tft.fillCircle(X, Y, 2, COL_ERR);

  // wait for any current press (like the one that started calibration) to end, then a new one
  while (touch.touched()) delay(10);
  delay(200);
  while (!touch.touched()) delay(10);
  delay(50);  // let the reading settle

  long sx = 0, sy = 0;
  int samples = 0;
  while (touch.touched() && samples < 32) {
    TS_Point p = touch.getPoint();
    sx += p.x;
    sy += p.y;
    samples++;
    delay(10);
  }
  if (samples == 0) samples = 1;
  rawX = sx / samples;
  rawY = sy / samples;

  tft.fillCircle(X, Y, 10, COL_OK);  // confirm the tap
  while (touch.touched()) delay(10);
  delay(200);
}

// Tap crosshairs near the top-left and bottom-right corners, work out the raw reading at each
// screen edge, let the user test it by drawing, then save it
void runTouchCalibration() {
  const int W = tft.width(), H = tft.height();
  long x1, y1, x2, y2;
  long oldL = calLeft, oldR = calRight, oldT = calTop, oldB = calBottom;

  Serial.println("Touch calibration started");
  drawCalibrationText("Touch calibration", "Tap the crosshair", "Step 1 of 2");
  getTouchTarget(CAL_MARGIN, CAL_MARGIN, x1, y1);
  drawCalibrationText("Touch calibration", "Tap the crosshair", "Step 2 of 2");
  getTouchTarget(W - CAL_MARGIN, H - CAL_MARGIN, x2, y2);

  // raw counts per pixel times the margin gives the raw value at the true edge
  calLeft = constrain(x1 - ((x2 - x1) * CAL_MARGIN) / (W - 2 * CAL_MARGIN), 0, 4095);
  calRight = constrain(x2 + ((x2 - x1) * CAL_MARGIN) / (W - 2 * CAL_MARGIN), 0, 4095);
  calTop = constrain(y1 - ((y2 - y1) * CAL_MARGIN) / (H - 2 * CAL_MARGIN), 0, 4095);
  calBottom = constrain(y2 + ((y2 - y1) * CAL_MARGIN) / (H - 2 * CAL_MARGIN), 0, 4095);
  Serial.printf("Touch calibration: L %ld  R %ld  T %ld  B %ld\n", calLeft, calRight, calTop, calBottom);

  // Let the user check it: dots should follow the finger. A progress bar counts down 6 s.
  drawCalibrationText("Draw to test", "Dots follow your finger", "");
  const unsigned long TEST_MS = 6000;
  const int bx = 60, bw = W - 120, by = 190;
  tft.fillRoundRect(bx, by, bw, 4, 2, COL_ACCENT);
  unsigned long t0 = millis();
  while (millis() - t0 < TEST_MS) {
    if (touch.touched()) {
      int x, y;
      mapTouch(touch.getPoint(), x, y);
      tft.fillCircle(x, y, 3, COL_ACCENT);
    }
    int left = bw - bw * (millis() - t0) / TEST_MS;
    tft.fillRect(bx + left, by, bw - left, 4, COL_BG);
    delay(10);
  }

  if (touchCalibrationValid()) {
    saveCalibration();
    drawCalibrationText("Calibration saved", "", "");
  } else {
    // taps were too close together: go back to the previous values
    calLeft = oldL;
    calRight = oldR;
    calTop = oldT;
    calBottom = oldB;
    Serial.println("Touch calibration rejected, not saved");
    drawCalibrationText("Calibration failed", "Not saved", "Restart and tap splash to retry");
  }
  delay(2000);
}

// On the splash screen, give the user a few seconds to tap for touch calibration
bool calibrationRequested() {
  const unsigned long WINDOW_MS = 3000;
  splashStatus("Tap to calibrate touch", COL_TEXT, "or wait to continue");
  const int bx = 60, bw = tft.width() - 120, by = 226;
  tft.fillRoundRect(bx, by, bw, 4, 2, COL_ACCENT);
  unsigned long t0 = millis();
  while (millis() - t0 < WINDOW_MS) {
    if (screenPressed()) return true;
    int left = bw - bw * (millis() - t0) / WINDOW_MS;
    tft.fillRect(bx + left, by, bw - left, 4, COL_BG);
    delay(20);
  }
  return false;
}

// ===========================
// SD card
// The microSD slot uses GPIO 14 (CLK), 15 (CMD), and 2 (DAT0) in 1-bit mode, the same pins as the
// display's SCK, CS, and DC. So the two take turns: release the display's SPI bus, mount the
// card, do the file work, unmount, then re-initialize the display. The display is reset in the
// process, so the caller must redraw the screen afterwards. (1-bit mode leaves GPIO 4 and 12
// free for the touch panel.)
// ===========================

// Release the display's SPI bus and mount the card; on failure the display is restored
bool sdBegin() {
  SPI.end();
  pinMode(TFT_MOSI, INPUT_PULLUP);  // GPIO 13 is the card's DAT3: must be high to stay in SD mode
  if (!SD_MMC.begin("/sdcard", true) || SD_MMC.cardType() == CARD_NONE) {
    Serial.println("[SD] No SD card");
    sdEnd();
    return false;
  }
  return true;
}

// Unmount the card and give the pins back to the display (the screen is cleared)
void sdEnd() {
  SD_MMC.end();
  setupDisplay();
}

// Boot-time check: puts a status line like "SD card: 14.6 GB free" in info.
// Returns true if a card is present.
bool checkSDCard(char *info, size_t infoSize) {
  if (!sdBegin()) {
    strlcpy(info, "No SD card", infoSize);
    return false;
  }
  float freeGB = (SD_MMC.totalBytes() - SD_MMC.usedBytes()) / (1024.0 * 1024.0 * 1024.0);
  snprintf(info, infoSize, "SD card: %.1f GB free", freeGB);
  Serial.printf("[SD] %s\n", info);
  sdEnd();
  return true;
}

// Save a JPEG as SD_FOLDER/IMG_nnnn.jpg. The number comes from a counter kept in flash and is
// bumped past any existing .jpg/.txt with that name, so files are never overwritten (even if the
// card was used elsewhere). Puts the path without extension in base ("" on failure) and returns
// true if the photo was saved.
bool savePhotoToSD(const uint8_t *buf, size_t len, char *base, size_t baseSize) {
  base[0] = '\0';
  if (!sdBegin()) return false;

  if (!SD_MMC.exists(SD_FOLDER)) SD_MMC.mkdir(SD_FOLDER);

  prefs.begin("sd", false);
  int n = prefs.getInt("next", 1);
  char name[40], jpgPath[48], txtPath[48];
  while (true) {
    snprintf(name, sizeof(name), SD_FOLDER "/IMG_%04d", n);
    snprintf(jpgPath, sizeof(jpgPath), "%s.jpg", name);
    snprintf(txtPath, sizeof(txtPath), "%s.txt", name);
    if (!SD_MMC.exists(jpgPath) && !SD_MMC.exists(txtPath)) break;
    n++;
  }
  prefs.putInt("next", n + 1);
  prefs.end();

  File f = SD_MMC.open(jpgPath, FILE_WRITE);
  bool ok = f && f.write(buf, len) == len;
  if (f) f.close();
  sdEnd();

  if (!ok) {
    Serial.printf("[SD] Failed to write %s\n", jpgPath);
    return false;
  }
  Serial.printf("[SD] Saved %s (%u bytes)\n", jpgPath, len);
  strlcpy(base, name, baseSize);
  return true;
}

// Save Gemini's answer (or the error) next to the photo as SD_FOLDER/IMG_nnnn.txt
void saveResultToSD(const char *base, bool ok, const char *text) {
  if (!sdBegin()) return;
  char path[56];
  snprintf(path, sizeof(path), "%s.txt", base);
  File f = SD_MMC.open(path, FILE_WRITE);
  if (f) {
    const char *photoName = strrchr(base, '/');
    photoName = photoName ? photoName + 1 : base;
    f.printf("Photo:  %s.jpg\r\n", photoName);
    f.printf("Model:  %s\r\n", GEMINI_MODEL);
    f.printf("Prompt: %s\r\n", AI_PROMPT);
    f.printf("Result: %s\r\n", ok ? "OK" : "ERROR");
    f.print("----------------------------------------\r\n");
    // Windows-friendly line endings: drop any \r, write each \n as \r\n
    for (const char *c = text; *c; c++) {
      if (*c == '\r') continue;
      if (*c == '\n') {
        f.print("\r\n");
      } else {
        f.write((uint8_t)*c);
      }
    }
    f.print("\r\n");
    f.close();
    Serial.printf("[SD] Saved %s\n", path);
  } else {
    Serial.printf("[SD] Failed to write %s\n", path);
  }
  sdEnd();
}

// ===========================
// WiFi
// ===========================

// Load the saved network and password (or the defaults if nothing is saved)
void loadWiFiCredentials() {
  prefs.begin("wifi", false);
  if (prefs.isKey("ssid")) {
    prefs.getString("ssid", wifiSsid, sizeof(wifiSsid));
  } else {
    strlcpy(wifiSsid, DEFAULT_SSID, sizeof(wifiSsid));
  }
  if (prefs.isKey("pass")) {
    prefs.getString("pass", wifiPass, sizeof(wifiPass));
  } else {
    strlcpy(wifiPass, DEFAULT_PASSWORD, sizeof(wifiPass));
  }
  prefs.end();
}

// Save the current network and password to flash
void saveWiFiCredentials() {
  prefs.begin("wifi", false);
  prefs.putString("ssid", wifiSsid);
  prefs.putString("pass", wifiPass);
  prefs.end();
  Serial.printf("WiFi credentials saved for %s\n", wifiSsid);
}

// Start-up choice after the camera and SD card checks: returns true for Continue,
// false for WiFi Setup
bool startupMenu() {
  const int W = tft.width();
  char line[48], fitted[48];
  drawSplash();
  tft.fillRect(0, 156, W, tft.height() - 156, COL_BG);
  snprintf(line, sizeof(line), "WiFi: %s", wifiSsid);
  printCentered(fitText(line, &FreeSans9pt7b, W - 16, fitted, sizeof(fitted)), 176, &FreeSans9pt7b, COL_MUTED);

  const int bw = 136, bh = 40, by = 190;
  const int x1 = 16, x2 = W - 16 - bw;
  drawButton(x1, by, bw, bh, "WiFi Setup", STYLE_NORMAL);
  drawButton(x2, by, bw, bh, "Continue", STYLE_PRIMARY);

  while (true) {
    int x, y;
    waitForTap(x, y);
    if (inRect(x, y, x1, by, bw, bh)) return false;
    if (inRect(x, y, x2, by, bw, bh)) return true;
  }
}

// Try to join a network (15 s timeout), showing progress on the splash screen.
// On failure, error describes why.
bool connectWiFi(const char *ssid, const char *pass, char *error, size_t errorSize) {
  const unsigned long TIMEOUT_MS = 15000;
  drawSplash();
  splashStatus("Connecting to WiFi...", COL_TEXT, ssid);
  const int bx = 60, bw = tft.width() - 120, by = 226;
  tft.fillRoundRect(bx, by, bw, 4, 2, COL_ACCENT);

  WiFi.disconnect(true);
  delay(100);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, pass);
  WiFi.setSleep(false);
  Serial.printf("WiFi connecting to %s", ssid);

  unsigned long t0 = millis();
  while (millis() - t0 < TIMEOUT_MS) {
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println(" connected");
      return true;
    }
    int left = bw - bw * (millis() - t0) / TIMEOUT_MS;
    tft.fillRect(bx + left, by, bw - left, 4, COL_BG);
    delay(100);
  }

  wl_status_t st = WiFi.status();
  if (st == WL_NO_SSID_AVAIL) {
    strlcpy(error, "Network not found", errorSize);
  } else if (st == WL_CONNECT_FAILED) {
    strlcpy(error, "Wrong password?", errorSize);
  } else {
    strlcpy(error, "Could not connect", errorSize);
  }
  Serial.printf(" failed: %s\n", error);
  WiFi.disconnect(true);
  return false;
}

// Scan, drop hidden networks and duplicates (keep the strongest), sort strongest first
int scanNetworks(WiFiNet *nets) {
  WiFi.disconnect(true);
  delay(100);
  WiFi.mode(WIFI_STA);
  int n = WiFi.scanNetworks();
  int count = 0;
  for (int i = 0; i < n; i++) {
    char s[SSID_LEN];
    strlcpy(s, WiFi.SSID(i).c_str(), sizeof(s));  // the WiFi library returns a String: copy it right away
    if (s[0] == '\0') continue;
    int j = 0;
    while (j < count && strcmp(nets[j].ssid, s) != 0) j++;
    if (j < count) {
      if (WiFi.RSSI(i) > nets[j].rssi) nets[j].rssi = WiFi.RSSI(i);
      continue;
    }
    if (count >= MAX_NETS) continue;
    strlcpy(nets[count].ssid, s, sizeof(nets[count].ssid));
    nets[count].rssi = WiFi.RSSI(i);
    nets[count].secure = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
    count++;
  }
  WiFi.scanDelete();
  // simple insertion sort by signal strength
  for (int i = 1; i < count; i++) {
    WiFiNet t = nets[i];
    int j = i - 1;
    while (j >= 0 && nets[j].rssi < t.rssi) {
      nets[j + 1] = nets[j];
      j--;
    }
    nets[j + 1] = t;
  }
  Serial.printf("WiFi scan: %d networks\n", count);
  return count;
}

// 4-bar signal strength icon, bottom-left at (x, y)
void drawSignalBars(int x, int y, int rssi) {
  int level = rssi > -55 ? 4 : rssi > -65 ? 3 : rssi > -75 ? 2 : 1;
  for (int i = 0; i < 4; i++) {
    int h = 4 + i * 3;
    tft.fillRect(x + i * 5, y - h, 3, h, i < level ? COL_ACCENT : COL_LINE);
  }
}

// Small padlock icon, top-left at (x, y)
void drawLock(int x, int y) {
  tft.drawCircleHelper(x + 5, y + 6, 4, 1 | 2, COL_MUTED);  // shackle (top half circle)
  tft.drawFastVLine(x + 1, y + 6, 2, COL_MUTED);
  tft.drawFastVLine(x + 9, y + 6, 2, COL_MUTED);
  tft.fillRoundRect(x, y + 7, 11, 9, 2, COL_MUTED);
}

// Draw one page of the network list with signal bars, names, padlocks, and the bottom bar
void drawNetworkList(WiFiNet *nets, int count, int page, bool allowBack) {
  const int W = tft.width();
  int pages = max(1, (count + NETS_PER_PAGE - 1) / NETS_PER_PAGE);
  drawSetupHeader("WiFi Setup");

  // network count or page number at the right of the header
  char info[16];
  if (pages > 1) {
    snprintf(info, sizeof(info), "%d / %d", page + 1, pages);
  } else {
    snprintf(info, sizeof(info), "%d found", count);
  }
  printAt(info, W - 12 - textWidth(info, &FreeSans9pt7b), 30, &FreeSans9pt7b, COL_TEXT);

  if (count == 0) {
    printCentered("No networks found", 120, &FreeSans12pt7b, COL_TEXT);
    printCentered("Tap Rescan to try again", 148, &FreeSans9pt7b, COL_MUTED);
  }
  char name[SSID_LEN + 4];
  for (int r = 0; r < NETS_PER_PAGE; r++) {
    int i = page * NETS_PER_PAGE + r;
    if (i >= count) break;
    int ry = NET_ROW_Y + r * NET_ROW_H;
    bool saved = strcmp(nets[i].ssid, wifiSsid) == 0;
    tft.fillRoundRect(8, ry, W - 16, NET_ROW_H - 4, 6, COL_PANEL);
    if (saved) tft.drawRoundRect(8, ry, W - 16, NET_ROW_H - 4, 6, COL_ACCENT);
    drawSignalBars(18, ry + 24, nets[i].rssi);
    printAt(fitText(nets[i].ssid, &FreeSans12pt7b, W - 50 - 36, name, sizeof(name)), 46, ry + 23, &FreeSans12pt7b, COL_TEXT);
    if (nets[i].secure) drawLock(W - 30, ry + 7);
  }

  tft.drawFastHLine(14, WB_Y - 6, W - 28, COL_LINE);
  drawArrowButton(WB_PREV_X, WB_Y, WB_ARROW_W, WB_H, false, page > 0);
  drawButton(WB_SCAN_X, WB_Y, WB_TEXT_W, WB_H, "Rescan", STYLE_NORMAL);
  drawButton(WB_BACK_X, WB_Y, WB_TEXT_W, WB_H, "Back", allowBack ? STYLE_NORMAL : STYLE_DISABLED);
  drawArrowButton(WB_NEXT_X, WB_Y, WB_ARROW_W, WB_H, true, page < pages - 1);
}

// Full-screen keyboard for the password. Puts the password in out and returns true, or
// returns false if canceled (or nothing entered).
bool askPassword(const char *ssid, char *out, size_t outSize) {
  char prompt[48], fitted[48];
  tft.fillScreen(COL_BG);
  keyboard.setTouchLimits(calRight, calLeft, calBottom, calTop);  // library maps 320 -> 0, so pass them reversed
  keyboard.clearInput();
  snprintf(prompt, sizeof(prompt), "Password: %s", ssid);
  keyboard.setInitialText(fitText(prompt, &FreeSans12pt7b, 290, fitted, sizeof(fitted)));
  keyboard.getInput();
  waitForRelease();
  strlcpy(out, keyboard.data, outSize);
  return out[0] != '\0';
}

// WiFi Setup: scan, pick a network, enter its password, connect, and save.
// Returns true once connected, false if the user pressed Back (only offered when allowBack).
bool runWiFiSetup(bool allowBack) {
  static WiFiNet nets[MAX_NETS];
  int count = 0;
  int page = 0;
  bool needScan = true;

  while (true) {
    if (needScan) {
      drawSetupHeader("WiFi Setup");
      printCentered("Scanning for networks...", 130, &FreeSans12pt7b, COL_TEXT);
      count = scanNetworks(nets);
      page = 0;
      needScan = false;
    }
    int pages = max(1, (count + NETS_PER_PAGE - 1) / NETS_PER_PAGE);
    drawNetworkList(nets, count, page, allowBack);

    // wait for a tap on a network row or a button
    int sel = -1;
    while (sel < 0) {
      int x, y;
      waitForTap(x, y);
      if (inRect(x, y, WB_PREV_X, WB_Y, WB_ARROW_W, WB_H) && page > 0) {
        page--;
        break;
      }
      if (inRect(x, y, WB_NEXT_X, WB_Y, WB_ARROW_W, WB_H) && page < pages - 1) {
        page++;
        break;
      }
      if (inRect(x, y, WB_SCAN_X, WB_Y, WB_TEXT_W, WB_H)) {
        needScan = true;
        break;
      }
      if (allowBack && inRect(x, y, WB_BACK_X, WB_Y, WB_TEXT_W, WB_H)) {
        return false;
      }
      if (y >= NET_ROW_Y && y < NET_ROW_Y + NETS_PER_PAGE * NET_ROW_H) {
        int i = page * NETS_PER_PAGE + (y - NET_ROW_Y) / NET_ROW_H;
        if (i < count) sel = i;
      }
    }
    if (sel < 0) continue;  // page change or rescan

    // password (open networks don't need one)
    char pass[PASS_LEN] = "";
    if (nets[sel].secure && !askPassword(nets[sel].ssid, pass, sizeof(pass))) {
      continue;  // canceled: back to the list
    }

    char error[40];
    if (connectWiFi(nets[sel].ssid, pass, error, sizeof(error))) {
      strlcpy(wifiSsid, nets[sel].ssid, sizeof(wifiSsid));
      strlcpy(wifiPass, pass, sizeof(wifiPass));
      saveWiFiCredentials();
      char line[48];
      snprintf(line, sizeof(line), "Saved %s", wifiSsid);
      splashStatus("Connected!", COL_OK, line);
      delay(1500);
      return true;
    }
    splashStatus(error, COL_ERR, "Tap a network to try again");
    delay(2500);
  }
}

// Start-up WiFi: offer WiFi Setup / Continue, connect with the saved network, and fall back
// to WiFi Setup when there is no saved network or it can't be joined
void startWiFi() {
  loadWiFiCredentials();
  bool trySaved = wifiSsid[0] != '\0' && startupMenu();
  while (true) {
    if (trySaved) {
      char error[40];
      if (connectWiFi(wifiSsid, wifiPass, error, sizeof(error))) return;
      char line[80];
      snprintf(line, sizeof(line), "%s: %s", error, wifiSsid);
      splashStatus(line, COL_ERR, "Opening WiFi Setup...");
      delay(2500);
    }
    if (runWiFiSetup(wifiSsid[0] != '\0')) return;
    trySaved = true;  // Back: try the saved network again
  }
}

// ===========================
// Photo evaluation
// ===========================

// Take a photo, have it evaluated, save the answer next to the photo, and show the result
void runEvaluation() {
  bool ok = takePhotoAndEvaluate(aiResult, sizeof(aiResult));
  if (lastPhotoBase[0]) {
    saveResultToSD(lastPhotoBase, ok, aiResult);  // resets the display; showResult redraws it all
  }
  showResult(aiResult, ok);
  showingResult = true;
}

// Leave the result screen and resume live video
void backToLiveVideo() {
  Serial.println("[AI] Back to live video");
  tft.fillScreen(COL_BG);
  showingResult = false;
}

// Take a photo, save it to the SD card, send it to Gemini, and put the answer (or an error
// message) in result. Returns true on success.
bool takePhotoAndEvaluate(char *result, size_t resultSize) {
  lastPhotoBase[0] = '\0';
  auto fail = [&](const char *msg) {
    Serial.printf("[AI] %s\n", msg);
    strlcpy(result, msg, resultSize);
    return false;
  };

  if (strstr(GEMINI_API_KEY, "PASTE_YOUR") != NULL) {
    return fail("Set GEMINI_API_KEY in the sketch first (free key: https://aistudio.google.com/apikey)");
  }
  if (WiFi.status() != WL_CONNECTED) {
    return fail("WiFi not connected");
  }

  // Check whether the scene is dark enough for the flash (measured on the live picture)
  bool useFlash = flashNeeded();
  aiStatus(useFlash ? "Taking photo (flash)..." : "Taking photo...", 1);

  // Use at least VGA for the photo (the stream defaults to QVGA), then restore the stream size
  sensor_t *s = esp_camera_sensor_get();
  framesize_t prevSize = (framesize_t)s->status.framesize;
  if (prevSize < FRAMESIZE_VGA) {
    s->set_framesize(s, FRAMESIZE_VGA);
  }

  // Flash on (nothing can be drawn until it's off again), then discard stale frames so we get a
  // fresh image at the new size; with the flash, also give auto-exposure time to adjust
  if (useFlash) flashOn();
  unsigned long warmStart = millis();
  int discarded = 0;
  while (discarded < 2 || (useFlash && millis() - warmStart < FLASH_WARMUP_MS)) {
    camera_fb_t *old = esp_camera_fb_get();
    if (old) esp_camera_fb_return(old);
    discarded++;
  }

  camera_fb_t *fb = esp_camera_fb_get();
  if (useFlash) flashOff();
  if (!fb) {
    s->set_framesize(s, prevSize);
    return fail("Camera capture failed");
  }
  Serial.printf("[AI] Captured %ux%u JPEG, %u bytes\n", fb->width, fb->height, fb->len);

  // Save the photo to the SD card first: that resets the display, so draw the photo afterwards
  drawStatus("Saving photo...", 1);
  bool saved = savePhotoToSD(fb->buf, fb->len, lastPhotoBase, sizeof(lastPhotoBase));
  drawJpegFit(fb->buf, fb->len, fb->width, fb->height);  // freeze the photo on screen while we wait
  char line[96];
  if (saved) {
    snprintf(line, sizeof(line), "Saved %s.jpg", strrchr(lastPhotoBase, '/') + 1);
    drawStatus(line, 1);
  } else {
    drawStatus("Photo captured (not saved, no SD card)", 1);
  }

  // Build the JSON request body: prefix (with the prompt) + base64(jpeg) + suffix
  const char *prefixFmt = "{\"contents\":[{\"parts\":[{\"text\":\"%s\"},{\"inline_data\":{\"mime_type\":\"image/jpeg\",\"data\":\"";
  const char *suffix = "\"}}]}]}";
  int prefixLen = snprintf(NULL, 0, prefixFmt, AI_PROMPT);

  size_t b64Len = 0;
  mbedtls_base64_encode(NULL, 0, &b64Len, fb->buf, fb->len);  // gives required size incl. NUL
  size_t bodyCap = prefixLen + b64Len + strlen(suffix) + 1;
  char *body = (char *)(psramFound() ? ps_malloc(bodyCap) : malloc(bodyCap));
  if (!body) {
    esp_camera_fb_return(fb);
    s->set_framesize(s, prevSize);
    return fail("Out of memory for request body");
  }

  snprintf(body, prefixLen + 1, prefixFmt, AI_PROMPT);
  size_t written = 0;
  mbedtls_base64_encode((unsigned char *)body + prefixLen, b64Len, &written, fb->buf, fb->len);
  size_t bodyLen = prefixLen + written;
  memcpy(body + bodyLen, suffix, strlen(suffix));
  bodyLen += strlen(suffix);
  body[bodyLen] = '\0';

  esp_camera_fb_return(fb);
  s->set_framesize(s, prevSize);

  // Buffer for Gemini's JSON response
  char *resp = (char *)(psramFound() ? ps_malloc(RESP_MAX) : malloc(RESP_MAX));
  if (!resp) {
    free(body);
    return fail("Out of memory for the response");
  }
  BufferStream respStream(resp, RESP_MAX);

  // Send to Gemini; on 503 (model overloaded) retry up to MAX_RETRIES more times, RETRY_DELAY_MS apart
  const int MAX_RETRIES = 3;
  const unsigned long RETRY_DELAY_MS = 15000;
  char url[160];
  snprintf(url, sizeof(url), "https://generativelanguage.googleapis.com/v1beta/models/%s:generateContent", GEMINI_MODEL);
  int code = 0;
  unsigned long t0 = 0;

  for (int attempt = 0; attempt <= MAX_RETRIES; attempt++) {

    Serial.printf("Attempt %d of %d)...\n", attempt, MAX_RETRIES);

    if (attempt == 0) {
      aiStatus("Asking Gemini what it sees...", 2);
    } else {
      Serial.printf("[AI] Gemini busy (503), retrying in %lu s (retry %d of %d)...\n", RETRY_DELAY_MS / 1000, attempt, MAX_RETRIES);
      // Count down on screen while we wait
      for (int sec = RETRY_DELAY_MS / 1000; sec > 0; sec--) {
        snprintf(line, sizeof(line), "Gemini busy - retry %d/%d in %d s", attempt, MAX_RETRIES, sec);
        drawStatus(line, 2);
        delay(1000);
      }
      snprintf(line, sizeof(line), "Retry %d/%d: asking Gemini...", attempt, MAX_RETRIES);
      aiStatus(line, 2);
    }

    WiFiClientSecure client;
    client.setInsecure();  // skip certificate validation to keep things simple
    HTTPClient http;
    http.begin(client, url);
    http.setTimeout(30000);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("x-goog-api-key", GEMINI_API_KEY);

    t0 = millis();
    code = http.POST((uint8_t *)body, bodyLen);
    if (code <= 0) {
      // errorToString returns a String: copy its text right away
      snprintf(line, sizeof(line), "Request failed: %s", HTTPClient::errorToString(code).c_str());
      http.end();
      free(body);
      free(resp);
      return fail(line);
    }
    respStream.clear();
    http.writeToStream(&respStream);  // read the whole response into resp
    http.end();

    if (code != 503) break;
  }
  free(body);

  if (code != 200) {
    Serial.printf("[AI] HTTP %d from Gemini:\n%.600s\n", code, resp);
    char geminiMsg[200];
    extractJsonString(resp, "\"error\"", "\"message\"", geminiMsg, sizeof(geminiMsg));
    char msg[300];
    if (code == 503) {
      snprintf(msg, sizeof(msg), "Gemini still busy after %d retries (HTTP 503). %s", MAX_RETRIES, geminiMsg);
    } else {
      snprintf(msg, sizeof(msg), "HTTP %d from Gemini. %s", code, geminiMsg);
    }
    free(resp);
    return fail(msg);
  }

  int answerLen = extractJsonString(resp, "\"candidates\"", "\"text\"", result, resultSize);
  if (answerLen == 0) {
    Serial.printf("%.600s\n", resp);
    free(resp);
    return fail("Could not find an answer in the response");
  }
  free(resp);

  drawStatus("Got the answer!", 3);
  Serial.printf("\n===== AI evaluation (%lu ms) =====\n", millis() - t0);
  Serial.println(result);
  Serial.println("==================================\n");
  return true;
}

// Find the first "key": "string" value that appears after section in a JSON response, decode
// its escapes, and put it in out. Returns its length (0 if not found).
int extractJsonString(const char *json, const char *section, const char *key, char *out, size_t outSize) {
  out[0] = '\0';
  const char *p = strstr(json, section);
  if (!p) return 0;
  p = strstr(p, key);
  if (!p) return 0;
  p = strchr(p, ':');
  if (!p) return 0;
  p = strchr(p + 1, '"');
  if (!p) return 0;
  p++;

  int o = 0;
  while (*p && *p != '"') {
    if (*p == '\\' && p[1]) {
      p++;
      switch (*p) {
        case 'n': o = appendText(out, outSize, o, "\n", 1); break;
        case 't': o = appendText(out, outSize, o, " ", 1); break;
        case 'r': break;
        case 'u':
          if (p[1] && p[2] && p[3] && p[4]) {
            char hex[5] = { p[1], p[2], p[3], p[4], '\0' };
            o = appendUtf8(out, outSize, o, strtoul(hex, NULL, 16));
            p += 4;
          }
          break;
        default: o = appendText(out, outSize, o, p, 1); break;  // \" \\ \/
      }
    } else {
      o = appendText(out, outSize, o, p, 1);
    }
    p++;
  }
  return o;
}

// ===========================
// Result screen
// ===========================

// Clean up Gemini's text for the display fonts: drop markdown and map common Unicode
// punctuation to plain ASCII. The result goes in out.
void toDisplayText(const char *in, char *out, size_t outSize) {
  int o = 0;
  out[0] = '\0';
  bool lineStart = true;
  int len = strlen(in);
  for (int i = 0; i < len; i++) {
    uint8_t c = in[i];
    if (c == '\n') {
      o = appendText(out, outSize, o, "\n", 1);
      lineStart = true;
      continue;
    }
    if (lineStart && c == '#') continue;  // markdown heading
    if (c == '*') {
      // "* item" at the start of a line is a bullet; any other '*' is bold/italic markup
      if (lineStart && i + 1 < len && in[i + 1] == ' ') o = appendText(out, outSize, o, "-", 1);
      continue;
    }
    if (c != ' ') lineStart = false;
    if (c < 0x80) {
      o = appendText(out, outSize, o, &in[i], 1);
      continue;
    }

    // Decode a UTF-8 sequence
    uint32_t cp;
    int extra;
    if ((c & 0xE0) == 0xC0) {
      cp = c & 0x1F;
      extra = 1;
    } else if ((c & 0xF0) == 0xE0) {
      cp = c & 0x0F;
      extra = 2;
    } else if ((c & 0xF8) == 0xF0) {
      cp = c & 0x07;
      extra = 3;
    } else {
      continue;
    }
    for (int k = 0; k < extra && i + 1 < len; k++) {
      cp = (cp << 6) | (in[++i] & 0x3F);
    }
    switch (cp) {
      case 0x2018:
      case 0x2019: o = appendText(out, outSize, o, "'", 1); break;
      case 0x201C:
      case 0x201D: o = appendText(out, outSize, o, "\"", 1); break;
      case 0x2013:
      case 0x2014:
      case 0x2022: o = appendText(out, outSize, o, "-", 1); break;
      case 0x2026: o = appendText(out, outSize, o, "...", 3); break;
      default: o = appendText(out, outSize, o, "?", 1); break;
    }
  }
}

// Word-wrap text by pixel width inside box. Lines starting with "- " get a round bullet and a
// hanging indent; lines ending in ':' are drawn as accent-colored headings. When draw is true,
// lines firstLine .. firstLine + maxLines - 1 are drawn (one page). Returns the total number of
// lines the text needs.
int wrapText(const char *text, const TextBox &box, int firstLine, bool draw) {
  int spaceW = textWidth(" ", box.font);
  int lines = 0;
  if (draw) tft.setFont(box.font);

  // Draw one finished line if it is on the requested page, and count it
  auto emit = [&](const char *line, int indent, uint16_t color, bool bullet) {
    if (draw && lines >= firstLine && lines < firstLine + box.maxLines) {
      int baseline = box.baseline + (lines - firstLine) * box.lineH;
      if (bullet) {
        tft.fillCircle(box.x + 5, baseline - box.ascent / 2, box.ascent > 20 ? 5 : 3, COL_ACCENT);
      }
      tft.setTextColor(color);
      tft.setCursor(box.x + indent, baseline);
      tft.print(line);
    }
    lines++;
  };

  char line[128];  // the line being built
  const char *p = text;
  while (*p) {
    // One paragraph runs from p to the next newline (or the end of the text)
    const char *end = strchr(p, '\n');
    if (!end) end = p + strlen(p);
    const char *next = (*end == '\n') ? end + 1 : end;

    // Trim spaces at both ends; skip blank lines to save space
    while (p < end && isspace((unsigned char)*p)) p++;
    while (end > p && isspace((unsigned char)end[-1])) end--;
    if (p == end) {
      p = next;
      continue;
    }

    bool bullet = (end - p >= 2 && p[0] == '-' && p[1] == ' ');
    if (bullet) {
      p += 2;
      while (p < end && *p == ' ') p++;
    }
    bool heading = !bullet && end[-1] == ':';
    int indent = bullet ? (box.ascent > 20 ? 20 : 16) : 0;
    int avail = box.w - indent;
    uint16_t color = heading ? COL_ACCENT : COL_TEXT;
    bool paraStart = true;  // the bullet goes on the paragraph's first line only

    int lineLen = 0;
    int lineW = 0;
    line[0] = '\0';
    const char *w = p;
    while (w < end) {
      // next word: w .. we
      while (w < end && *w == ' ') w++;
      if (w >= end) break;
      const char *we = w;
      while (we < end && *we != ' ') we++;
      int wlen = we - w;
      int ww = textWidthN(w, wlen, box.font);

      // Split words that are wider than a whole line
      while (ww > avail && wlen > 1) {
        int n = wlen - 1;
        while (n > 1 && textWidthN(w, n, box.font) > avail) n--;
        if (lineLen) {
          emit(line, indent, color, bullet && paraStart);
          paraStart = false;
        }
        lineLen = appendText(line, sizeof(line), 0, w, n);
        emit(line, indent, color, bullet && paraStart);
        paraStart = false;
        lineLen = 0;
        w += n;
        wlen -= n;
        ww = textWidthN(w, wlen, box.font);
      }

      if (lineLen == 0) {
        lineLen = appendText(line, sizeof(line), 0, w, wlen);
        lineW = ww;
      } else if (lineW + spaceW + ww <= avail) {
        lineLen = appendText(line, sizeof(line), lineLen, " ", 1);
        lineLen = appendText(line, sizeof(line), lineLen, w, wlen);
        lineW += spaceW + ww;
      } else {
        emit(line, indent, color, bullet && paraStart);
        paraStart = false;
        lineLen = appendText(line, sizeof(line), 0, w, wlen);
        lineW = ww;
      }
      w = we;
    }
    if (lineLen) emit(line, indent, color, bullet && paraStart);
    p = next;
  }
  return lines;
}

// Text box for the result body in the given font, between y = top and y = bottom
TextBox makeTextBox(const GFXfont *f, int top, int bottom) {
  TextBox box;
  box.font = f;
  box.x = 14;
  box.w = tft.width() - 28;
  box.ascent = f->yAdvance * 6 / 10;
  box.lineH = f->yAdvance * 9 / 10;
  box.baseline = top + box.ascent;
  int descent = f->yAdvance / 5;
  box.maxLines = 1 + (bottom - descent - box.baseline) / box.lineH;
  return box;
}

// Width of each button in the result screen's bottom bar: [< Prev] [Done] [Next >]
int buttonW() {
  return (tft.width() - 4 * BTN_MARGIN) / 3;
}

// Top edge of the result screen's bottom buttons
int buttonY() {
  return tft.height() - BTN_H - 6;
}

// Left edge of a result screen button (single-page answers only show Done, in the middle)
int buttonX(ResultButton b) {
  return BTN_MARGIN + (b - BTN_PREV) * (buttonW() + BTN_MARGIN);
}

// Which result-screen button (if any) is at screen position (x, y). Slightly generous hit area.
ResultButton resultButtonAt(int x, int y) {
  const int pad = 6;
  if (y < buttonY() - pad) return BTN_NONE;
  const ResultButton buttons[] = { BTN_PREV, BTN_DONE, BTN_NEXT };
  for (ResultButton b : buttons) {
    if (b != BTN_DONE && resultPages <= 1) continue;  // Prev/Next only exist on multi-page answers
    int bx = buttonX(b);
    if (x >= bx - pad && x <= bx + buttonW() + pad) return b;
  }
  return BTN_NONE;
}

// Filled accent "Done" button with a check mark
void drawDoneButton(int x, int y, int w, int h) {
  tft.fillRoundRect(x, y, w, h, 8, COL_ACCENT);
  const char *label = "Done";
  int tw = textWidth(label, &FreeSans12pt7b);
  int cx = x + (w - (tw + 20)) / 2;
  int cy = y + h / 2;
  for (int t = 0; t < 3; t++) {  // 3 px thick check mark
    tft.drawLine(cx, cy + t - 1, cx + 4, cy + 5 + t - 1, COL_BG);
    tft.drawLine(cx + 4, cy + 5 + t - 1, cx + 12, cy - 5 + t - 1, COL_BG);
  }
  printAt(label, cx + 20, cy + 8, &FreeSans12pt7b, COL_BG);
}

// Rounded Prev/Next button with a triangle arrow; grayed out when disabled
void drawPageButton(int x, int y, int w, int h, bool next, bool enabled) {
  uint16_t fg = enabled ? COL_TEXT : COL_LINE;
  tft.fillRoundRect(x, y, w, h, 8, enabled ? COL_PANEL : COL_BG);
  tft.drawRoundRect(x, y, w, h, 8, enabled ? COL_ACCENT : COL_LINE);
  const char *label = next ? "Next" : "Prev";
  int tw = textWidth(label, &FreeSans12pt7b);
  int contentW = tw + 8 + 10;  // text + gap + arrow
  int cx = x + (w - contentW) / 2;
  int cy = y + h / 2;
  if (next) {
    printAt(label, cx, cy + 8, &FreeSans12pt7b, fg);
    int ax = cx + tw + 8;
    tft.fillTriangle(ax, cy - 7, ax, cy + 7, ax + 10, cy, enabled ? COL_ACCENT : COL_LINE);
  } else {
    tft.fillTriangle(cx + 10, cy - 7, cx + 10, cy + 7, cx, cy, enabled ? COL_ACCENT : COL_LINE);
    printAt(label, cx + 18, cy + 8, &FreeSans12pt7b, fg);
  }
}

// Left gradient color of the result header (violet for answers, magenta for errors)
uint16_t headerLeft() {
  return resultOk ? COL_ACCENT2 : COL_ERR2;
}

// Right gradient color of the result header (cyan for answers, red for errors)
uint16_t headerRight() {
  return resultOk ? COL_ACCENT : COL_ERR;
}

// Page counter ("2 / 3") and page dots at the right end of the header
void drawPageCounter() {
  const int W = tft.width();
  const int x0 = W - 100;
  drawGradientBar(0, HEADER_H, headerLeft(), headerRight(), x0, W);  // repaint behind the counter
  char counter[16];
  snprintf(counter, sizeof(counter), "%d / %d", resultPage + 1, resultPages);
  printAt(counter, W - 12 - textWidth(counter, &FreeSans12pt7b), 25, &FreeSans12pt7b, COL_TEXT);
  if (resultPages <= 8) {
    const int gap = 10;
    int dx = W - 16 - (resultPages - 1) * gap;
    for (int i = 0; i < resultPages; i++) {
      if (i == resultPage) {
        tft.fillCircle(dx + i * gap, 36, 3, COL_TEXT);
      } else {
        tft.drawCircle(dx + i * gap, 36, 2, COL_TEXT);
      }
    }
  }
}

// Draw the body text, page counter, and button bar for the current page of the result
void drawResultPage() {
  const int W = tft.width();
  const int H = tft.height();
  bool paged = resultPages > 1;

  tft.fillRect(0, HEADER_H + 1, W, H - HEADER_H - 1, COL_BG);
  wrapText(resultBody, resultBox, resultPage * resultBox.maxLines, true);
  tft.drawFastHLine(14, H - PAGE_BAR_H, W - 28, COL_LINE);

  int bw = buttonW(), by = buttonY();
  drawDoneButton(buttonX(BTN_DONE), by, bw, BTN_H);
  if (paged) {
    drawPageButton(buttonX(BTN_PREV), by, bw, BTN_H, false, resultPage > 0);
    drawPageButton(buttonX(BTN_NEXT), by, bw, BTN_H, true, resultPage < resultPages - 1);
    drawPageCounter();
  }
}

// Go forward/back a page (delta = +1 / -1) if there is one
void changePage(int delta) {
  int page = resultPage + delta;
  if (!showingResult || page < 0 || page >= resultPages) return;
  resultPage = page;
  Serial.printf("[AI] Page %d of %d\n", resultPage + 1, resultPages);
  drawResultPage();
}

// Show the AI's answer (or an error) full screen: gradient header with icon + title, then the
// wrapped body text. Short answers use the largest font that fits on one page; long answers are
// split into pages (12pt) with Prev/Next buttons.
void showResult(const char *text, bool ok) {
  const int W = tft.width();
  const int H = tft.height();

  tft.fillScreen(COL_BG);

  // Header
  resultOk = ok;
  drawGradientBar(0, HEADER_H, headerLeft(), headerRight(), 0, W);
  tft.drawFastHLine(0, HEADER_H, W, COL_TEXT);
  const int ix = 26, iy = HEADER_H / 2;
  if (ok) {
    // "lens / eye" icon
    tft.drawCircle(ix, iy, 14, COL_TEXT);
    tft.drawCircle(ix, iy, 13, COL_TEXT);
    tft.fillCircle(ix, iy, 6, COL_TEXT);
  } else {
    // warning triangle
    tft.fillTriangle(ix, iy - 14, ix - 15, iy + 12, ix + 15, iy + 12, COL_TEXT);
    tft.fillRect(ix - 1, iy - 6, 3, 10, COL_ERR2);
    tft.fillRect(ix - 1, iy + 6, 3, 3, COL_ERR2);
  }
  printAt(ok ? "AI says" : "Error", 50, 35, &FreeSans18pt7b, COL_TEXT);

  // Body: use the largest font that fits on one page (18pt -> 12pt -> 9pt) ...
  toDisplayText(text, resultBody, sizeof(resultBody));
  resultPage = 0;
  resultPages = 1;
  const int top = HEADER_H + 10;
  const int bottom = H - PAGE_BAR_H - 6;
  const GFXfont *fonts[] = { &FreeSans18pt7b, &FreeSans12pt7b, &FreeSans9pt7b };
  bool fits = false;
  for (const GFXfont *f : fonts) {
    resultBox = makeTextBox(f, top, bottom);
    if (wrapText(resultBody, resultBox, 0, false) <= resultBox.maxLines) {
      fits = true;
      break;
    }
  }
  // ... otherwise split it into pages in 12pt with Prev/Next buttons
  if (!fits) {
    resultBox = makeTextBox(&FreeSans12pt7b, top, bottom);
    int lines = wrapText(resultBody, resultBox, 0, false);
    resultPages = (lines + resultBox.maxLines - 1) / resultBox.maxLines;
    Serial.printf("[AI] Answer needs %d pages (tap Prev/Next, or type < / >)\n", resultPages);
  }

  drawResultPage();
}
