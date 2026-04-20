/*
  PhotoFrame - Slideshow with Web Interface and WebSocket Synchronization

  This project creates an ESP32-powered photo frame that displays a slideshow of images
  from an SD card and hosts a web interface for controlling slideshow speed and
  uploading/deleting images. It also provides a synchronized slideshow webpage using
  WebSockets.

  Created by: Grey Lancaster and the Open-Source Community

  Special Thanks to the following libraries and developers:
  -------------------------------------------------------------------------------
  - WiFiManager by tzapu (tzapu/WiFiManager): Helps manage Wi-Fi connections easily.
  - ESPAsyncWebServer (esp32async): Provides asynchronous web server functionalities.
  - TFT_eSPI by Bodmer (Bodmer/TFT_eSPI): For controlling the TFT display.
  - XPT2046_Bitbang by nitek: For interfacing with the touchscreen.
  - SdFat by Greiman (greiman/SdFat): Advanced SD card handling.
  - JPEGDEC by BitBank (bitbank2/JPEGDEC): JPG decoding for image display.
  - QRCode by ricmoo: To display QR codes on the TFT screen.
  - mDNS (ESP32 Core): Enables access to the device using photoframe.local.
  - FS (ESP32 Core): File system handling.
  - ElegantOTA by Ayush Sharma
*/

#include <WiFi.h>
#include <WiFiManager.h>          // WiFiManager to manage Wi-Fi connections
#include <AsyncTCP.h>             // Async TCP library for WebSocket

#include <AsyncEventSource.h>
#include <TFT_eSPI.h>             // TFT display library
#include <XPT2046_Bitbang.h>      // Touch screen library
#include <SPI.h>
#include <SdFat.h>                // SD card library (SdFat)
#include <JPEGDEC.h>              // JPG decoder library
#include "SPIFFS.h"               // Include SPIFFS to satisfy TFT_eSPI dependency
#include "qrcode.h"               // QR code library
#include <FS.h>                   // Include FS.h
#include "esp_task_wdt.h"         // ESP32 Task Watchdog Timer
#include "esp_heap_caps.h"        // Heap memory debugging
#include <ESPmDNS.h>
#include <Preferences.h>          // Persist settings across reboots
#include <time.h>                 // NTP-based time for night mode
#include <ElegantOTA.h>  // Using the official ElegantOTA with async mode enabled

// Touch Screen pins
#define XPT2046_IRQ 36
#define XPT2046_MOSI 32
#define XPT2046_MISO 39
#define XPT2046_CLK 25
#define XPT2046_CS 33

// SD card pins
#define SD_CS 5 // Chip Select for SD Card (IO5)

TFT_eSPI tft = TFT_eSPI();
JPEGDEC jpeg;
XPT2046_Bitbang ts(XPT2046_MOSI, XPT2046_MISO, XPT2046_CLK, XPT2046_CS);

SPIClass sdSpi(VSPI);
SdSpiConfig sdSpiConfig(SD_CS, SHARED_SPI, SD_SCK_MHZ(10), &sdSpi);  // Use SHARED_SPI mode
SdFat sd;
SdBaseFile sdRoot;
SdBaseFile jpgFile;
int16_t currentIndex = 0;
uint16_t fileCount = 0;

uint32_t timer;
SemaphoreHandle_t xSpiMutex;

AsyncWebServer server(80);
AsyncWebSocket ws("/ws");  // Create a WebSocket object
WiFiManager wm;
volatile bool buttonPressed = false;

bool sdMounted = false;
bool slideshowActive = true;
bool slideshowPaused = false;          // Toggled by center-tap or /toggle-pause
volatile bool pendingPauseBadge  = false;  // set by web handler, drawn by main loop
volatile bool pendingImageReload = false;  // set by web handler, reloads image on main loop

// SD card space cache. freeClusterCount() walks the entire FAT (megabytes
// of SPI reads on a 32 GB card -> multi-second blocking call). We keep a
// snapshot here that is refreshed at boot and after every upload / delete,
// so /status renders without ever touching the SD bus.
static uint64_t cachedCardBytes = 0;
static uint64_t cachedFreeBytes = 0;
static bool     sdSpaceCacheValid = false;

// Touch-input calibration. The XPT2046 reports raw values in its own native
// orientation; the panel mounted on these CYD boards is the mirror of
// `tft.setRotation(3)`, so we invert both axes when mapping to display pixels.
// Verified against the touch_probe build: a corner tap lands within a few px.
const int TS_X_MIN = 200;
const int TS_X_MAX = 3900;
const int TS_Y_MIN = 200;
const int TS_Y_MAX = 3900;

// Function declarations
void setupWebServer();
void loadImage(uint16_t targetIndex);
void decodeJpeg(const char *name);
void displayQRCodeAt(const String& ip, int startX, int startY, int blockSize);
void showStatusScreen(const char* title, const char* message, bool showQR);
void stopSlideshow();
void restartSlideshow();
bool checkAndMountSDCard();
void refreshSdSpaceCache();
uint16_t scanImages();
bool isImageFile(const char* name);
void applyBacklight();
void loadSettings();
void saveSettings();
void initBacklight();
void initTimeNTP();
String makePosixTz(int offset);
String tzLabel(int offset);
void buildShuffleOrder();
void mapTouch(const TouchPoint& p, int& outX, int& outY);
void handleTouchInput();
void drawPauseBadge();
void showInfoOverlay();
void onWebSocketEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type,
                      void *arg, uint8_t *data, size_t len);

// User settings (persisted in NVS)
Preferences prefs;
int X = 10;                       // Slideshow interval (seconds)
uint8_t dayBrightness = 255;      // 0-255
uint8_t nightBrightness = 20;     // 0-255
uint8_t nightStartHour = 22;      // 0-23
uint8_t nightEndHour = 7;         // 0-23
bool nightModeEnabled = true;
int  tzOffsetHours = 0;           // UTC offset (-12..+14), persisted in NVS
bool shuffleMode = false;         // Shuffle playback order, persisted in NVS
uint16_t* shuffleOrder = nullptr; // Heap-allocated permutation array (rebuilt on toggle/scan)
const char* PREFS_NS = "frame";

// Backlight PWM
#ifndef TFT_BL
#define TFT_BL 21
#endif
#define BACKLIGHT_LEDC_CHANNEL 7
#define BACKLIGHT_LEDC_FREQ    5000
#define BACKLIGHT_LEDC_RES     8

// Global variable to store the current image name
String currentImageName = "";
String currentIp = "";              // Set after WiFi connects
const char* MDNS_HOST = "photoframe";
volatile bool wantsInfoScreen = false;  // Triggered by user action
bool inInfoScreen = false;          // Currently showing the info/error screen
// Set true when the current info screen describes a problem with the *current*
// image (Bad Image / Image Too Big / Decode Failed). When true, both manual
// dismissal and the auto-advance timer move on to the *next* image instead of
// reloading the same one (otherwise we'd loop forever on a corrupt file).
bool errorScreenAdvancesIndex = false;
uint32_t bootMillis = 0;
String currentSsid = "";

// Button: track for double-press / advance vs info-screen distinction
volatile uint32_t lastButtonIsrMs = 0;
void IRAM_ATTR buttonInt() {
  uint32_t now = millis();
  if (now - lastButtonIsrMs < 80) return;  // debounce
  lastButtonIsrMs = now;
  buttonPressed = true;
}

// JPG decoding functions
int JPEGDraw(JPEGDRAW *pDraw) {
  tft.pushImage(pDraw->x, pDraw->y, pDraw->iWidth, pDraw->iHeight, pDraw->pPixels);
  return 1;
}

void *myOpen(const char *filename, int32_t *size) {
  jpgFile = sd.open(filename);
  *size = jpgFile.fileSize();
  return &jpgFile;
}

void myClose(void *handle) {
  if (jpgFile) jpgFile.close();
}

int32_t myRead(JPEGFILE *handle, uint8_t *buffer, int32_t length) {
  return jpgFile.read(buffer, length);
}

int32_t mySeek(JPEGFILE *handle, int32_t position) {
  return jpgFile.seekSet(position);
}

// Special function for JPEGDEC file operations with SPIFFS
void *spiffsOpen(const char *filename, int32_t *size) {
  File f = SPIFFS.open(filename);
  if (!f) {
    Serial.printf("Failed to open %s from SPIFFS\n", filename);
    return NULL;
  }
  *size = f.size();
  return (void *)new File(f);
}

void spiffsClose(void *handle) {
  if (handle) {
    File *f = (File *)handle;
    f->close();
    delete f;
  }
}

int32_t spiffsRead(JPEGFILE *handle, uint8_t *buffer, int32_t length) {
  File *f = (File *)handle->fHandle;
  return f->read(buffer, length);
}

int32_t spiffsSeek(JPEGFILE *handle, int32_t position) {
  File *f = (File *)handle->fHandle;
  return f->seek(position);
}

// Stop the slideshow by releasing the resources and stopping the decoder
void stopSlideshow() {
  slideshowActive = false;
  xSemaphoreTake(xSpiMutex, portMAX_DELAY);  // Lock SPI access for SD card
  jpeg.close();  // Ensure that the JPEG decoder is closed
  xSemaphoreGive(xSpiMutex);  // Unlock SPI access
  delay(500);  // Add delay to ensure proper transition
  Serial.println("Slideshow stopped.");
}

// Restart the slideshow after WAV playback
void restartSlideshow() {
  slideshowActive = true;
  loadImage(currentIndex);
  Serial.println("Slideshow restarted.");
}

// Match supported image extensions (.jpg / .jpeg, case-insensitive)
bool isImageFile(const char* name) {
  size_t len = strlen(name);
  if (len >= 4 && strcasecmp(name + len - 4, ".jpg") == 0) return true;
  if (len >= 5 && strcasecmp(name + len - 5, ".jpeg") == 0) return true;
  return false;
}

// Count image files on the SD card root
uint16_t scanImages() {
  uint16_t count = 0;
  if (!sdRoot) return 0;
  sdRoot.rewind();
  SdBaseFile entry;
  char name[100];
  while (entry.openNext(&sdRoot, O_RDONLY)) {
    if (!entry.isSubDir()) {
      entry.getName(name, sizeof(name));
      if (isImageFile(name)) count++;
    }
    entry.close();
  }
  return count;
}

// Function to load and display an image
void loadImage(uint16_t logicalIndex) {
  if (!slideshowActive) return;

  // In shuffle mode, translate the logical sequential index into the
  // shuffled physical index so every image is seen once before repeating.
  uint16_t targetIndex = (shuffleMode && shuffleOrder && fileCount > 0)
                           ? shuffleOrder[logicalIndex % fileCount]
                           : logicalIndex;

  if (targetIndex >= fileCount) targetIndex = 0;
  sdRoot.rewind();
  uint16_t index = 0;
  SdBaseFile entry;
  char name[100];
  while (entry.openNext(&sdRoot)) {
    if (!entry.isSubDir()) {
      entry.getName(name, sizeof(name));
      if (isImageFile(name)) {
        if (index == targetIndex) {
          currentImageName = String(name);  // Set current image name
          decodeJpeg(name);
          entry.close();

          // Send WebSocket message to notify clients
          ws.textAll("update");

          return;
        }
        index++;
      }
    }
    entry.close();
  }
}

void decodeJpeg(const char *name) {
  if (!slideshowActive) return;

  xSemaphoreTake(xSpiMutex, portMAX_DELAY);  // Lock SPI access for SD card
  bool opened = jpeg.open(name, myOpen, myClose, myRead, mySeek, JPEGDraw);
  if (!opened) {
    xSemaphoreGive(xSpiMutex);
    Serial.printf("Failed to decode image: %s\n", name);
    char msg[160];
    snprintf(msg, sizeof(msg),
             "Could not decode image:\n  %s\n\nThe file may be corrupt or unsupported.\nUse a 320x240 .jpg instead.",
             name);
    showStatusScreen("Bad Image", msg, currentIp.length() > 0);
    errorScreenAdvancesIndex = true;
    return;
  }
  // Reject anything too large for the framebuffer / RAM. CYD has ~300KB free
  // heap; JPEGDEC streams MCUs but the decoder itself trips on huge files and
  // we end up with a blank screen and no error. 1280x800 is a generous cap
  // that still rejects 4K phone snaps.
  int jw = jpeg.getWidth();
  int jh = jpeg.getHeight();
  const int MAX_W = 1280;
  const int MAX_H = 800;
  if (jw <= 0 || jh <= 0 || jw > MAX_W || jh > MAX_H) {
    jpeg.close();
    xSemaphoreGive(xSpiMutex);
    Serial.printf("Image too large: %s (%dx%d)\n", name, jw, jh);
    char msg[200];
    snprintf(msg, sizeof(msg),
             "Image too large:\n  %s\n  %dx%d\n\nResize to 320x240 .jpg\nbefore uploading.",
             name, jw, jh);
    showStatusScreen("Image Too Big", msg, currentIp.length() > 0);
    errorScreenAdvancesIndex = true;
    return;
  }
  tft.fillScreen(TFT_BLACK);
  int rc = jpeg.decode((tft.width() - jw) / 2, (tft.height() - jh) / 2, 0);
  jpeg.close();
  xSemaphoreGive(xSpiMutex);
  if (rc != 1) {
    Serial.printf("JPEG decode failed for %s (rc=%d)\n", name, rc);
    char msg[200];
    snprintf(msg, sizeof(msg),
             "Decode failed:\n  %s\n  %dx%d\n\nFile may be progressive\nor an unsupported variant.",
             name, jw, jh);
    showStatusScreen("Decode Failed", msg, currentIp.length() > 0);
    errorScreenAdvancesIndex = true;
    return;
  }
  inInfoScreen = false;
}

// Draw a QR code with given IP at a specified position and block size
void displayQRCodeAt(const String& ip, int startX, int startY, int blockSize) {
  String url = "http://" + ip;
  QRCode qrcode;
  uint8_t qrcodeData[qrcode_getBufferSize(4)];
  qrcode_initText(&qrcode, qrcodeData, 4, ECC_MEDIUM, url.c_str());
  int qrSize = qrcode.size;
  // White background border
  tft.fillRect(startX - 2, startY - 2, qrSize * blockSize + 4, qrSize * blockSize + 4, TFT_WHITE);
  for (int y = 0; y < qrSize; y++) {
    for (int x = 0; x < qrSize; x++) {
      tft.fillRect(startX + x * blockSize, startY + y * blockSize, blockSize, blockSize,
                   qrcode_getModule(&qrcode, x, y) ? TFT_BLACK : TFT_WHITE);
    }
  }
}

// Unified info / error screen.
// Layout (320x240, rotation 3):
//   left column: title (red, size 2), message (white, size 1), IP info
//   right column: QR code (when showQR && IP available)
// Draw a word-wrapped block of text at the given (x,y), within `maxWidth`,
// stopping after `maxLines`. Returns the y position immediately below the
// last drawn line.
static int drawWrapped(const char* text, int x, int y, int maxWidth,
                       int textSize, int maxLines, uint16_t color) {
  tft.setTextColor(color);
  tft.setTextSize(textSize);
  int charW = 6 * textSize;
  int lineH = 8 * textSize + 2;
  int maxChars = maxWidth / charW;
  if (maxChars < 1) maxChars = 1;

  int line = 0;
  const char* p = text;
  while (*p && line < maxLines) {
    int chars = 0;
    tft.setCursor(x, y);
    while (*p && *p != '\n' && chars < maxChars) {
      tft.print(*p);
      p++;
      chars++;
    }
    if (*p == '\n') p++;
    y += lineH;
    line++;
  }
  return y;
}

void showStatusScreen(const char* title, const char* message, bool showQR) {
  inInfoScreen = true;
  // Default: this screen does NOT advance the slideshow on dismissal. The
  // image-error wrapper below opts in.
  errorScreenAdvancesIndex = false;
  tft.fillScreen(TFT_BLACK);
  bool haveIp = currentIp.length() > 0;
  bool drawQr = showQR && haveIp;

  // ---- Top banner: full-width red bar with the title in white ----
  // Hard to miss: this is "what is wrong" / "what state am I in".
  const int bannerH = 26;
  tft.fillRect(0, 0, 320, bannerH, TFT_RED);
  tft.setTextColor(TFT_WHITE, TFT_RED);
  tft.setTextSize(2);
  tft.setCursor(8, 6);
  tft.print(title);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);  // restore default bg for later text

  // ---- Message panel: yellow-bordered box with bright yellow text ----
  // This is the "details" the user actually needs to read. The QR (if any)
  // sits to the right of this panel.
  const int panelY = bannerH + 6;            // 32
  const int panelH = 110;                    // 32..142
  const int qrSize = 99;                     // 33 modules * 3px block size
  const int qrX = 215;
  const int qrY = panelY + 2;
  const int panelX = 4;
  const int panelW = drawQr ? (qrX - panelX - 8) : (320 - panelX * 2);

  tft.drawRect(panelX, panelY, panelW, panelH, TFT_YELLOW);
  tft.drawRect(panelX + 1, panelY + 1, panelW - 2, panelH - 2, TFT_YELLOW);

  const int padX = panelX + 8;
  const int padY = panelY + 8;
  const int textW = panelW - 16;

  // Try to render the message at size 2 if every line fits at that size,
  // otherwise fall back to size 1 so long filenames still wrap cleanly.
  bool fitsBig = true;
  {
    int maxBigChars = textW / 12;
    int chars = 0;
    for (const char* q = message; *q; q++) {
      if (*q == '\n') { chars = 0; continue; }
      if (++chars > maxBigChars) { fitsBig = false; break; }
    }
  }
  int textSize = fitsBig ? 2 : 1;
  int maxLines = fitsBig ? (panelH - 16) / 18 : (panelH - 16) / 10;
  drawWrapped(message, padX, padY, textW, textSize, maxLines, TFT_YELLOW);

  // ---- Bottom strip: IP / mDNS in cyan, hint in green ----
  int infoY = panelY + panelH + 8;  // 150
  if (haveIp) {
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.setTextSize(1);
    tft.setCursor(8, infoY);
    tft.print("Open in browser:");

    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    tft.setTextSize(2);
    tft.setCursor(8, infoY + 14);
    tft.print(currentIp);

    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.setTextSize(1);
    tft.setCursor(8, infoY + 38);
    tft.print("or ");
    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    tft.print(MDNS_HOST);
    tft.print(".local");

    if (drawQr) {
      tft.setTextColor(TFT_WHITE, TFT_BLACK);
      tft.setCursor(8, infoY + 52);
      tft.print("or scan QR -->");
    }
  } else {
    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    tft.setTextSize(1);
    tft.setCursor(8, infoY + 14);
    tft.print("Waiting for WiFi...");
  }

  // ---- QR code on the right ----
  if (drawQr) {
    displayQRCodeAt(currentIp, qrX, qrY, 3);
  }

  Serial.printf("[STATUS] %s -- %s\n", title, message);
}

// Recompute and cache total/free bytes on the SD card. Caller MUST hold
// xSpiMutex. This is the only place freeClusterCount() runs -- it walks
// the whole FAT and is far too slow to call from a request handler.
void refreshSdSpaceCache() {
  if (!sdMounted) {
    sdSpaceCacheValid = false;
    return;
  }
  cachedCardBytes = (uint64_t)sd.card()->sectorCount() * 512ULL;
  uint32_t freeClusters      = sd.vol()->freeClusterCount();
  uint32_t sectorsPerCluster = sd.vol()->sectorsPerCluster();
  cachedFreeBytes = (uint64_t)freeClusters * sectorsPerCluster * 512ULL;
  sdSpaceCacheValid = true;
  Serial.printf("SD space cache: %.1f MB total, %.1f MB free\n",
                (double)cachedCardBytes / (1024.0 * 1024.0),
                (double)cachedFreeBytes / (1024.0 * 1024.0));
}

// Function to check and mount SD card if not already mounted
bool checkAndMountSDCard() {
  if (!sdMounted) {  // Only try to mount if it's not already mounted
    // Try to remount the SD card
    if (!sd.begin(sdSpiConfig)) {
      Serial.println("SD Card Mount Failed");
      sdMounted = false;
      return false;
    } else {
      sdMounted = true;  // Successfully mounted SD card
      Serial.println("SD Card Mounted Successfully");
    }
  }
  return true;
}

// Tracks the per-upload state set by the chunk callback
static bool uploadRejected = false;
static String uploadRejectReason;

// ---------------------------------------------------------------------------
// Web UI templating helpers
// ---------------------------------------------------------------------------
// All page templates live in SPIFFS under /web/. Per-route lambdas can
// capture additional state and forward unknown vars to webProcessor() for
// the common substitutions.
String webProcessor(const String& var) {
  if (var == "X")               return String(X);
  if (var == "DAY_BR")          return String(dayBrightness);
  if (var == "NIGHT_BR")        return String(nightBrightness);
  if (var == "NIGHT_START")     return String(nightStartHour);
  if (var == "NIGHT_END")       return String(nightEndHour);
  if (var == "NIGHT_CHK")       return nightModeEnabled ? String("checked") : String("");
  if (var == "PAUSE_LABEL")     return slideshowPaused ? String("Resume Slideshow") : String("Pause Slideshow");
  if (var == "PAUSE_BTN_CLASS") return slideshowPaused ? String("button secondary") : String("button");
  if (var == "TZ_OFFSET")       return String(tzOffsetHours);
  if (var == "SHUFFLE_LABEL")     return shuffleMode ? String("Shuffle: On") : String("Shuffle: Off");
  if (var == "SHUFFLE_BTN_CLASS") return shuffleMode ? String("button") : String("button secondary");
  return String();
}

// Send an HTML template from SPIFFS. Falls back to a helpful 500 if the
// filesystem image hasn't been uploaded (firmware OTA without fs OTA).
void sendHtml(AsyncWebServerRequest* req, const char* path,
              AwsTemplateProcessor processor = nullptr) {
  if (!SPIFFS.exists(path)) {
    String body =
      "<!DOCTYPE html><html><body style='font-family:sans-serif;padding:20px'>"
      "<h1>Web UI not uploaded</h1>"
      "<p>The page <code>" + String(path) + "</code> isn't in flash.</p>"
      "<p>Upload the filesystem image once via USB "
      "(<code>pio run -t uploadfs</code>) or push it via "
      "<a href='/update'>/update</a> in <b>Filesystem</b> mode.</p>"
      "</body></html>";
    req->send(500, "text/html", body);
    return;
  }
  if (processor) {
    req->send(SPIFFS, path, "text/html", false, processor);
  } else {
    req->send(SPIFFS, path, "text/html", false, webProcessor);
  }
}

// Build the <input>+<span> rows for /delete. Caller MUST hold xSpiMutex.
String buildDeleteFileList() {
  String html;
  sdRoot.rewind();
  SdBaseFile entry;
  char name[100];
  bool fileFound = false;
  while (entry.openNext(&sdRoot)) {
    entry.getName(name, sizeof(name));
    if (strcasecmp(name, "System Volume Information") == 0) {
      entry.close();
      continue;
    }
    bool isImg = isImageFile(name);
    String cls = isImg ? "image" : "other";
    html += "<div class='file-row'>";
    html += "<input type='checkbox' name='file' value='" + String(name) + "'>";
    if (isImg) {
      html += "<img class='thumb' src='/sd-image?name=" + String(name) + "' loading='lazy'>";
    }
    html += "<span class='filename " + cls + "'>" + String(name);
    if (!isImg) html += " (not an image)";
    html += "</span></div>";
    fileFound = true;
    entry.close();
  }
  if (!fileFound) html = "<p>No files on SD card.</p>";
  return html;
}

// Build the <tr> rows that fill /status. Pure in-memory render -- no SD
// I/O, no mutex acquisition, no chance of stalling the AsyncTCP task or
// the slideshow. Card size / free space come from the cache, which is
// refreshed at boot and after every upload / delete.
String buildStatusRows() {
  uint32_t up = millis() / 1000;
  uint32_t days  = up / 86400; up %= 86400;
  uint32_t hours = up / 3600;  up %= 3600;
  uint32_t mins  = up / 60;    uint32_t secs = up % 60;
  char uptimeStr[64];
  snprintf(uptimeStr, sizeof(uptimeStr), "%lud %02lu:%02lu:%02lu",
           (unsigned long)days, (unsigned long)hours,
           (unsigned long)mins, (unsigned long)secs);

  String html;
  auto row = [&](const String& k, const String& v) {
    html += "<tr><th>" + k + "</th><td>" + v + "</td></tr>";
  };
  row("Firmware build",  String(__DATE__) + " " + String(__TIME__));
  row("Uptime",          String(uptimeStr));
  char timeBuf[32] = "(NTP not yet synced)";
  struct tm tm_now;
  if (getLocalTime(&tm_now, 0)) {
    strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M:%S", &tm_now);
  }
  row("Local time",      String(timeBuf));
  row("Timezone",        tzLabel(tzOffsetHours));
  row("Free heap",       String(esp_get_free_heap_size()) + " bytes");
  row("WiFi SSID",       currentSsid.length() ? currentSsid : String("(offline)"));
  row("WiFi RSSI",       WiFi.status() == WL_CONNECTED
                           ? String(WiFi.RSSI()) + " dBm" : String("n/a"));
  row("IP address",      currentIp.length() ? currentIp : String("(none)"));
  row("mDNS host",       String(MDNS_HOST) + ".local");
  row("Image count",     String(fileCount));
  row("Current image",   currentImageName.length() ? currentImageName : String("(none)"));
  row("Slideshow speed", String(X) + " s");
  row("Playback",        slideshowPaused ? String("paused")
                          : (inInfoScreen ? String("info screen") : String("running")));
  if (sdMounted && sdSpaceCacheValid) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%.1f MB", (double)cachedCardBytes / (1024.0 * 1024.0));
    row("SD total", String(buf));
    snprintf(buf, sizeof(buf), "%.1f MB", (double)cachedFreeBytes / (1024.0 * 1024.0));
    row("SD free",  String(buf));
  } else if (sdMounted) {
    row("SD card", "mounted (size cache pending)");
  } else {
    row("SD card", "not mounted");
  }
  row("Day brightness",   String(dayBrightness));
  row("Night brightness", String(nightBrightness));
  row("Night window",     nightModeEnabled
                          ? (String(nightStartHour) + ":00 to " + String(nightEndHour) + ":00")
                          : String("disabled"));
  return html;
}

// Function to handle file upload completion
void handleFileUpload(AsyncWebServerRequest *request) {
  if (uploadRejected) {
    String reason = uploadRejectReason.length() ? uploadRejectReason : String("Upload rejected");
    uploadRejected = false;
    uploadRejectReason = "";
    restartSlideshow();

    if (!SPIFFS.exists("/web/upload_rejected.html")) {
      request->send(400, "text/html",
                    String("<h1>Upload Rejected</h1><p>") + reason +
                    "</p><p>Only .jpg / .jpeg images are accepted.</p>"
                    "<a href=\"/upload_file\">Try again</a>");
      return;
    }

    AsyncWebServerResponse *response = request->beginResponse(
        SPIFFS, "/web/upload_rejected.html", "text/html", false,
        [reason](const String& v) -> String {
          if (v == "REJECT_REASON") return reason;
          return webProcessor(v);
        });
    response->setCode(400);
    request->send(response);
    return;
  }

  sendHtml(request, "/web/upload_done.html");

  // Rebuild the file list after upload (slideshow is still stopped, but
  // scanImages walks the SD so we still serialize against any other web ops)
  if (xSemaphoreTake(xSpiMutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
    fileCount = scanImages();
    if (shuffleMode) buildShuffleOrder();
    xSemaphoreGive(xSpiMutex);
  }
  currentIndex = 0;

  restartSlideshow();
}

// WebSocket event handler
void onWebSocketEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len) {
  if (type == WS_EVT_CONNECT) {
    Serial.printf("WebSocket client #%u connected from %s\n", client->id(), client->remoteIP().toString().c_str());
    // Let the client know the current image immediately when they connect
    client->text("update");
  } else if (type == WS_EVT_DISCONNECT) {
    Serial.printf("WebSocket client #%u disconnected\n", client->id());
  }
}
// Setup the web server. All page templates live in SPIFFS under /web/;
// this function wires URL routes to those templates.
void setupWebServer() {
  // Static assets: shared CSS + favicon. Served verbatim with HTTP caching
  // so the browser doesn't re-fetch the stylesheet on every navigation.
  server.serveStatic("/style.css",   SPIFFS, "/web/style.css")
        .setCacheControl("max-age=3600");
  server.serveStatic("/favicon.ico", SPIFFS, "/favicon.ico")
        .setCacheControl("max-age=86400");

  // Main menu / about / slideshow viewer (purely static templates)
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *r) {
    sendHtml(r, "/web/index.html");
  });
  server.on("/about", HTTP_GET, [](AsyncWebServerRequest *r) {
    sendHtml(r, "/web/about.html");
  });
  server.on("/convert", HTTP_GET, [](AsyncWebServerRequest *r) {
    sendHtml(r, "/web/convert.html");
  });
  server.on("/slideshow", HTTP_GET, [](AsyncWebServerRequest *r) {
    sendHtml(r, "/web/slideshow.html");
  });

  // Slideshow speed (form + submit)
  server.on("/speed", HTTP_GET, [](AsyncWebServerRequest *r) {
    sendHtml(r, "/web/speed.html");
  });

  server.on("/set-speed", HTTP_POST, [](AsyncWebServerRequest *r) {
    if (r->hasParam("speed", true)) {
      int newX = r->getParam("speed", true)->value().toInt();
      if (newX >= 1 && newX <= 3600) {
        X = newX;
        saveSettings();
        Serial.printf("Slideshow speed updated to: %d seconds (saved)\n", X);
      }
    }
    sendHtml(r, "/web/speed_set.html");
  });

  // Upload form (POST is handled below by handleFileUpload + chunk callback)
  server.on("/upload_file", HTTP_GET, [](AsyncWebServerRequest *r) {
    sendHtml(r, "/web/upload.html");
  });

  // File upload processing handler
  server.on("/upload_file", HTTP_POST, handleFileUpload,
      [](AsyncWebServerRequest *request, const String& filename, size_t index, uint8_t *data, size_t len, bool final) {
          static SdBaseFile file;
          static bool sdLocked = false;
          if (index == 0) {
              uploadRejected = false;
              uploadRejectReason = "";
              if (!isImageFile(filename.c_str())) {
                  uploadRejected = true;
                  uploadRejectReason = "\"" + filename + "\" is not a .jpg/.jpeg file";
                  Serial.printf("Upload rejected: %s (not an image)\n", filename.c_str());
                  return;
              }
              // Stop slideshow and grab the SD mutex for the whole upload so
              // the slideshow thread can't interleave SPI ops with our writes.
              stopSlideshow();
              if (xSemaphoreTake(xSpiMutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
                  uploadRejected = true;
                  uploadRejectReason = "SD card busy";
                  restartSlideshow();
                  return;
              }
              sdLocked = true;
              if (!file.open(("/" + filename).c_str(), O_WRITE | O_CREAT | O_TRUNC)) {
                  uploadRejected = true;
                  uploadRejectReason = "Failed to open file on SD";
                  xSemaphoreGive(xSpiMutex);
                  sdLocked = false;
                  return;
              }
          }
          if (uploadRejected) return;
          if (file.write(data, len) != len) {
              uploadRejected = true;
              uploadRejectReason = "SD write failed (card full?)";
          }
          if (final) {
              file.close();
              if (sdLocked) {
                  xSemaphoreGive(xSpiMutex);
                  sdLocked = false;
              }
          }
      }
  );

  // Delete page: lists every file on the SD root with a confirm-then-delete form.
  // The file list is built up-front under xSpiMutex and injected via the
  // %FILE_LIST% placeholder in the template.
  server.on("/delete", HTTP_GET, [](AsyncWebServerRequest *r) {
    if (xSemaphoreTake(xSpiMutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
      r->send(503, "text/html", "<h3>SD busy, try again</h3>");
      return;
    }
    if (!checkAndMountSDCard()) {
      xSemaphoreGive(xSpiMutex);
      r->send(500, "text/html", "<h3>SD Card Mount Failed!</h3>");
      return;
    }
    String fileList = buildDeleteFileList();
    xSemaphoreGive(xSpiMutex);

    sendHtml(r, "/web/delete.html", [fileList](const String& v) -> String {
      if (v == "FILE_LIST") return fileList;
      return webProcessor(v);
    });
  });

  // Apply selected deletions and report the outcome.
  server.on("/delete_files", HTTP_POST, [](AsyncWebServerRequest *r) {
    int params = r->params();
    bool deletionSuccess = true;
    if (xSemaphoreTake(xSpiMutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
      r->send(503, "text/html", "<h3>SD busy, try again</h3>");
      return;
    }
    for (int i = 0; i < params; i++) {
      const AsyncWebParameter* p = r->getParam(i);
      if (!p->isPost()) continue;
      String fileToDelete = "/" + p->value();
      if (sd.exists(fileToDelete.c_str())) {
        if (sd.remove(fileToDelete.c_str())) {
          Serial.printf("File deleted: %s\n", fileToDelete.c_str());
        } else {
          Serial.printf("Failed to delete file: %s\n", fileToDelete.c_str());
          deletionSuccess = false;
        }
      } else {
        Serial.printf("File not found: %s\n", fileToDelete.c_str());
        deletionSuccess = false;
      }
    }
    fileCount = scanImages();
    if (shuffleMode) buildShuffleOrder();
    xSemaphoreGive(xSpiMutex);
    if (currentIndex >= fileCount) currentIndex = 0;

    String result = deletionSuccess
        ? String("Selected files deleted successfully!")
        : String("Failed to delete some files. Check serial log for details.");
    sendHtml(r, "/web/delete_done.html", [result](const String& v) -> String {
      if (v == "DELETE_RESULT") return result;
      return webProcessor(v);
    });
  });

  // Status / diagnostic page. The big <tr> table is generated server-side and
  // injected via the %STATUS_ROWS% placeholder.
  server.on("/status", HTTP_GET, [](AsyncWebServerRequest *r) {
    String rows = buildStatusRows();
    sendHtml(r, "/web/status.html", [rows](const String& v) -> String {
      if (v == "STATUS_ROWS") return rows;
      return webProcessor(v);
    });
  });

  // Manual SD-space refresh. The cache is normally updated after every
  // upload/delete; this endpoint is for the cases where files changed
  // out-of-band (e.g. card pulled and edited on a PC). Generous timeout
  // because freeClusterCount() can take several seconds on a big card.
  server.on("/refresh-sd-space", HTTP_POST, [](AsyncWebServerRequest *r) {
    if (xSemaphoreTake(xSpiMutex, pdMS_TO_TICKS(15000)) == pdTRUE) {
      refreshSdSpaceCache();
      xSemaphoreGive(xSpiMutex);
    } else {
      Serial.println("/refresh-sd-space: SPI mutex busy, skipping");
    }
    r->redirect("/status");
  });

  server.on("/toggle-pause", HTTP_POST, [](AsyncWebServerRequest *r) {
    slideshowPaused = !slideshowPaused;
    Serial.printf("[web] slideshow %s\n", slideshowPaused ? "paused" : "resumed");
    if (slideshowPaused) pendingPauseBadge  = true;
    else                 pendingImageReload = true;
    r->redirect("/");
  });

  server.on("/toggle-shuffle", HTTP_POST, [](AsyncWebServerRequest *r) {
    shuffleMode = !shuffleMode;
    Serial.printf("[web] shuffle %s\n", shuffleMode ? "on" : "off");
    if (shuffleMode) buildShuffleOrder();
    saveSettings();
    r->redirect("/");
  });

  // Display settings (brightness + night-mode window).
  server.on("/display", HTTP_GET, [](AsyncWebServerRequest *r) {
    sendHtml(r, "/web/display.html");
  });

  server.on("/set-display", HTTP_POST, [](AsyncWebServerRequest *r) {
    auto getInt = [&](const char* name, int def) {
      if (r->hasParam(name, true))
        return (long)r->getParam(name, true)->value().toInt();
      return (long)def;
    };
    dayBrightness    = (uint8_t)constrain(getInt("dayBri",   dayBrightness),   0, 255);
    nightBrightness  = (uint8_t)constrain(getInt("nightBri", nightBrightness), 0, 255);
    nightStartHour   = (uint8_t)constrain(getInt("nStart",   nightStartHour),  0, 23);
    nightEndHour     = (uint8_t)constrain(getInt("nEnd",     nightEndHour),    0, 23);
    nightModeEnabled = r->hasParam("nightOn", true);
    if (r->hasParam("tz", true)) {
      int newOffset = constrain((int)r->getParam("tz", true)->value().toInt(), -12, 14);
      tzOffsetHours = newOffset;
      setenv("TZ", makePosixTz(tzOffsetHours).c_str(), 1);
      tzset();
    }
    saveSettings();
    applyBacklight();
    r->redirect("/display");
  });

  // (/about and /slideshow are wired at the top of this function next to /.)

  // Route to serve the current image - FIXED VERSION
server.on("/current_image", HTTP_GET, [](AsyncWebServerRequest *request) {
  if (currentImageName == "") {
    request->send(404, "text/plain", "No image");
    return;
  }

  // Store the current image name at request time
  String requestImageName = currentImageName;
  
  // Create a new scope for the SPI mutex to check file exists
  {
    // Take the mutex just to check if the file exists and get its size
    xSemaphoreTake(xSpiMutex, portMAX_DELAY);
    
    SdBaseFile *file = new SdBaseFile();
    if (!file->open(requestImageName.c_str(), O_RDONLY)) {
      Serial.println("Failed to open image file for initial check");
      xSemaphoreGive(xSpiMutex);  // Release the mutex on error
      delete file;
      request->send(404, "text/plain", "Image not found");
      return;
    }
    
    // Get the file size before setting up the chunked response
    uint32_t fileSize = file->fileSize();
    file->close();
    delete file;
    
    xSemaphoreGive(xSpiMutex);  // Release the mutex after checking file exists
    Serial.println("Image file exists, setting up chunked response");
  }

  // Create a self-contained response handler that manages its own mutex
  // Pass the current image name by value to the lambda
  AsyncWebServerResponse *response = request->beginChunkedResponse("image/jpeg",
    [requestImageName](uint8_t *buffer, size_t maxLen, size_t index) -> size_t {
      // Each chunk operation takes and releases the mutex
      xSemaphoreTake(xSpiMutex, portMAX_DELAY);
      
      // Open the file for each chunk to avoid keeping file handles between chunks
      SdBaseFile file;
      if (!file.open(requestImageName.c_str(), O_RDONLY)) {
        Serial.println("Failed to open image file during chunked read");
        xSemaphoreGive(xSpiMutex);
        return 0;  // End of file or error
      }
      
      // Seek to the current position
      if (index > 0) {
        file.seekSet(index);
      }
      
      // Read the data
      size_t bytesRead = file.read(buffer, maxLen);
      
      // Close file and release mutex
      file.close();
      xSemaphoreGive(xSpiMutex);
      
      return bytesRead;
    });

  response->addHeader("Content-Disposition", "inline; filename=" + requestImageName);
  response->addHeader("Access-Control-Allow-Origin", "*");

  request->send(response);
});

  // Serve any image from the SD card by name — used for thumbnails on /delete.
  server.on("/sd-image", HTTP_GET, [](AsyncWebServerRequest *r) {
    if (!r->hasParam("name")) { r->send(400, "text/plain", "missing name"); return; }
    String name = r->getParam("name")->value();
    if (!isImageFile(name.c_str()) || name.indexOf('/') >= 0 || name.indexOf("..") >= 0) {
      r->send(400, "text/plain", "bad filename"); return;
    }
    String path = "/" + name;
    {
      if (xSemaphoreTake(xSpiMutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
        r->send(503, "text/plain", "busy"); return;
      }
      SdBaseFile f;
      bool ok = f.open(path.c_str(), O_RDONLY);
      if (ok) f.close();
      xSemaphoreGive(xSpiMutex);
      if (!ok) { r->send(404, "text/plain", "not found"); return; }
    }
    AsyncWebServerResponse *resp = r->beginChunkedResponse("image/jpeg",
      [path](uint8_t *buffer, size_t maxLen, size_t index) -> size_t {
        if (xSemaphoreTake(xSpiMutex, pdMS_TO_TICKS(2000)) != pdTRUE) return 0;
        SdBaseFile file;
        if (!file.open(path.c_str(), O_RDONLY)) { xSemaphoreGive(xSpiMutex); return 0; }
        if (index > 0) file.seekSet(index);
        size_t got = file.read(buffer, maxLen);
        file.close();
        xSemaphoreGive(xSpiMutex);
        return got;
      });
    resp->addHeader("Cache-Control", "max-age=3600");
    r->send(resp);
  });


  // (WebSocket is registered in setup() before setupWebServer is called.)

  // 404 fallback
  server.onNotFound([](AsyncWebServerRequest *request) {
    request->send(404, "text/html",
                  "<h3>404 - Page Not Found</h3>"
                  "<p><a href=\"/\">Back to PhotoFrame</a></p>");
  });

  Serial.println("Starting web server...");
  server.begin();
  Serial.println("Web server started.");
}

// Settings persistence (NVS via Preferences)
void loadSettings() {
  prefs.begin(PREFS_NS, true);
  X = prefs.getInt("speed", 10);
  if (X < 1) X = 10;
  dayBrightness = prefs.getUChar("dayBri", 255);
  nightBrightness = prefs.getUChar("nightBri", 20);
  nightStartHour = prefs.getUChar("nStart", 22);
  nightEndHour = prefs.getUChar("nEnd", 7);
  nightModeEnabled = prefs.getBool("nightOn", true);
  tzOffsetHours = constrain(prefs.getInt("tzOffset", 0), -12, 14);
  shuffleMode = prefs.getBool("shuffle", false);
  prefs.end();
  Serial.printf("Loaded settings: X=%d dayBri=%u nightBri=%u night=%u-%u on=%d tz=%+d shuffle=%d\n",
                X, dayBrightness, nightBrightness, nightStartHour, nightEndHour, nightModeEnabled,
                tzOffsetHours, shuffleMode);
}

void saveSettings() {
  prefs.begin(PREFS_NS, false);
  prefs.putInt("speed", X);
  prefs.putUChar("dayBri", dayBrightness);
  prefs.putUChar("nightBri", nightBrightness);
  prefs.putUChar("nStart", nightStartHour);
  prefs.putUChar("nEnd", nightEndHour);
  prefs.putBool("nightOn", nightModeEnabled);
  prefs.putInt("tzOffset", tzOffsetHours);
  prefs.putBool("shuffle", shuffleMode);
  prefs.end();
}

// Backlight PWM
void initBacklight() {
  ledcSetup(BACKLIGHT_LEDC_CHANNEL, BACKLIGHT_LEDC_FREQ, BACKLIGHT_LEDC_RES);
  ledcAttachPin(TFT_BL, BACKLIGHT_LEDC_CHANNEL);
  ledcWrite(BACKLIGHT_LEDC_CHANNEL, dayBrightness);
}

// Decide and apply current backlight level based on local time + settings
void applyBacklight() {
  uint8_t target = dayBrightness;
  if (nightModeEnabled) {
    struct tm tm_now;
    if (getLocalTime(&tm_now, 5)) {
      int h = tm_now.tm_hour;
      bool inNight;
      if (nightStartHour == nightEndHour) {
        inNight = false;
      } else if (nightStartHour < nightEndHour) {
        inNight = (h >= nightStartHour && h < nightEndHour);
      } else {
        // Wraps midnight
        inNight = (h >= nightStartHour || h < nightEndHour);
      }
      target = inNight ? nightBrightness : dayBrightness;
    }
  }
  ledcWrite(BACKLIGHT_LEDC_CHANNEL, target);
}

// Build POSIX TZ string from a standard UTC offset (e.g. -3 → "<UTC-3>3")
// POSIX offsets are WEST of UTC — the opposite sign of the conventional notation.
String makePosixTz(int offset) {
  if (offset == 0) return "UTC0";
  String s = "<UTC";
  if (offset > 0) s += "+";
  s += String(offset) + ">" + String(-offset);
  return s;
}

// Human-readable label for the UTC offset (e.g. -3 → "UTC-3")
String tzLabel(int offset) {
  if (offset == 0) return "UTC";
  String s = "UTC";
  if (offset > 0) s += "+";
  s += String(offset);
  return s;
}

// Fisher-Yates shuffle of indices 0..fileCount-1. Called after scanImages()
// when shuffle mode is on, and again each time the index wraps to 0.
void buildShuffleOrder() {
  free(shuffleOrder);
  shuffleOrder = nullptr;
  if (fileCount == 0) return;
  shuffleOrder = (uint16_t*)malloc(fileCount * sizeof(uint16_t));
  if (!shuffleOrder) return;
  for (uint16_t i = 0; i < fileCount; i++) shuffleOrder[i] = i;
  for (uint16_t i = fileCount - 1; i > 0; i--) {
    uint16_t j = (uint16_t)(esp_random() % (uint32_t)(i + 1));
    uint16_t tmp = shuffleOrder[i]; shuffleOrder[i] = shuffleOrder[j]; shuffleOrder[j] = tmp;
  }
  Serial.printf("[shuffle] new order built for %u files\n", fileCount);
}

// Configure NTP and apply the persisted POSIX TZ string so local time is correct
void initTimeNTP() {
  configTzTime(makePosixTz(tzOffsetHours).c_str(), "pool.ntp.org", "time.nist.gov");
}

// Map raw touch into display pixel coords for tft.setRotation(3). Both axes
// are inverted because the panel is mounted as the mirror of the touch IC's
// native orientation. Used by both the TOUCH_PROBE build and the touch UX in
// the main firmware.
void mapTouch(const TouchPoint& p, int& outX, int& outY) {
  int rx = constrain(p.xRaw, TS_X_MIN, TS_X_MAX);
  int ry = constrain(p.yRaw, TS_Y_MIN, TS_Y_MAX);
  outX = map(rx, TS_X_MIN, TS_X_MAX, tft.width()  - 1, 0);
  outY = map(ry, TS_Y_MIN, TS_Y_MAX, tft.height() - 1, 0);
}

#ifndef TOUCH_PROBE
// ---- Touch UX ---------------------------------------------------------------
// Tap left third  -> previous image
// Tap right third -> next image
// Tap centre      -> pause / resume
// Long-press      -> show IP / QR overlay (any tap dismisses it)
// Any tap on a status / info overlay also dismisses it.

const uint32_t TOUCH_LONG_PRESS_MS = 800;
const uint32_t TOUCH_DEBOUNCE_MS   = 150;
const int      TOUCH_Z_THRESHOLD   = 100;  // ignore noise / very soft taps

static bool     touchHeld          = false;
static uint32_t touchStartMs       = 0;
static int      touchStartX        = 0;
static int      touchStartY        = 0;
static bool     longPressFired     = false;
static uint32_t lastTouchEndMs     = 0;

void drawPauseBadge() {
  // Yellow-bordered "PAUSE" badge in the upper-right; cleared the next time
  // an image is drawn (loadImage -> tft.fillScreen).
  const int bw = 76, bh = 22, bx = 320 - bw - 4, by = 4;
  tft.fillRect(bx, by, bw, bh, TFT_BLACK);
  tft.drawRect(bx, by, bw, bh, TFT_YELLOW);
  tft.drawRect(bx + 1, by + 1, bw - 2, bh - 2, TFT_YELLOW);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(bx + 8, by + 4);
  tft.print("PAUSE");
}

void showInfoOverlay() {
  // Long-press summons the standard status screen with the IP/QR. We pause
  // the slideshow timer (via inInfoScreen check in loop) until the user taps.
  if (currentIp.length() > 0) {
    showStatusScreen("PhotoFrame Info",
                     "Tap anywhere to return\nto the slideshow.",
                     true);
  } else {
    showStatusScreen("PhotoFrame Info",
                     "Not connected to WiFi.\nTap to return to slideshow.",
                     false);
  }
}

void handleTouchInput() {
  if (millis() - lastTouchEndMs < TOUCH_DEBOUNCE_MS) return;

  TouchPoint p = ts.getTouch();
  bool pressed = (p.zRaw > TOUCH_Z_THRESHOLD) || (digitalRead(XPT2046_IRQ) == LOW);
  uint32_t now = millis();

  if (pressed && !touchHeld) {
    int dx, dy;
    mapTouch(p, dx, dy);
    touchHeld      = true;
    touchStartMs   = now;
    touchStartX    = dx;
    touchStartY    = dy;
    longPressFired = false;
    return;
  }

  if (pressed && touchHeld && !longPressFired) {
    if (now - touchStartMs >= TOUCH_LONG_PRESS_MS) {
      longPressFired = true;
      Serial.printf("[touch] long-press @ (%d,%d)\n", touchStartX, touchStartY);
      showInfoOverlay();
    }
    return;
  }

  if (!pressed && touchHeld) {
    touchHeld      = false;
    lastTouchEndMs = now;
    if (longPressFired) return;  // long press already handled

    // Short tap dispatch
    if (inInfoScreen) {
      // Dismiss any status overlay. If the screen described a problem with
      // the current image (Bad Image / Too Big / Decode Failed) we have to
      // skip past it on dismissal -- otherwise loadImage() would re-show the
      // same broken file and re-trigger the error, looping forever.
      bool advance = errorScreenAdvancesIndex;
      Serial.printf("[touch] tap dismissed info screen (advance=%d)\n", advance);
      inInfoScreen = false;
      errorScreenAdvancesIndex = false;
      if (fileCount > 0) {
        if (advance) currentIndex = (currentIndex + 1) % fileCount;
        loadImage(currentIndex);
        if (slideshowPaused) drawPauseBadge();
      }
      timer = millis();
      return;
    }

    if (fileCount == 0) return;  // nothing to advance

    int w = tft.width();
    if (touchStartX < w / 3) {
      // Left third -> previous
      currentIndex = (currentIndex == 0) ? (fileCount - 1) : (currentIndex - 1);
      Serial.printf("[touch] previous (idx=%d)\n", currentIndex);
      loadImage(currentIndex);
      if (slideshowPaused) drawPauseBadge();
      timer = millis();
    } else if (touchStartX >= 2 * w / 3) {
      // Right third -> next
      currentIndex = (currentIndex + 1) % fileCount;
      Serial.printf("[touch] next (idx=%d)\n", currentIndex);
      loadImage(currentIndex);
      if (slideshowPaused) drawPauseBadge();
      timer = millis();
    } else {
      // Centre -> toggle pause
      slideshowPaused = !slideshowPaused;
      Serial.printf("[touch] %s\n", slideshowPaused ? "pause" : "resume");
      if (slideshowPaused) {
        drawPauseBadge();
      } else {
        // Redraw image to clear the badge, restart slideshow timer
        loadImage(currentIndex);
        timer = millis();
      }
    }
  }
}
#endif  // !TOUCH_PROBE

#ifdef TOUCH_PROBE
// Diagnostic build that does nothing but stream raw touchscreen samples to
// Serial and TFT. Build with: -DTOUCH_PROBE (see platformio.ini env :touch_probe)
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n=== TOUCH PROBE MODE ===");

  pinMode(4, OUTPUT);  digitalWrite(4, HIGH);
  pinMode(16, OUTPUT); digitalWrite(16, HIGH);
  pinMode(17, OUTPUT); digitalWrite(17, HIGH);

  tft.init();
  tft.setRotation(3);
  tft.setSwapBytes(true);
  tft.fillScreen(TFT_BLACK);
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);

  pinMode(XPT2046_IRQ, INPUT);
  ts.begin();

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(8, 8);
  tft.println("Touch Probe");
  tft.setTextSize(1);
  tft.setCursor(8, 32);
  tft.println("Tap the screen to print X/Y/Z.");
  tft.setCursor(8, 46);
  tft.println("Last sample appears at the bottom.");
  tft.setCursor(8, 60);
  tft.println("Touched cells are drawn green.");
}

void loop() {
  TouchPoint p = ts.getTouch();
  bool irqLow = digitalRead(XPT2046_IRQ) == LOW;
  if (p.zRaw > 0 || irqLow) {
    int dx, dy;
    mapTouch(p, dx, dy);
    Serial.printf("touch  raw(x=%4d y=%4d z=%4d)  mapped(x=%3d y=%3d)  irq=%d\n",
                  p.xRaw, p.yRaw, p.zRaw, dx, dy, irqLow);
    if (dx >= 0 && dx < tft.width() && dy >= 0 && dy < tft.height()) {
      tft.fillCircle(dx, dy, 4, TFT_GREEN);
    }
    tft.fillRect(0, 220, 320, 20, TFT_BLACK);
    tft.setCursor(8, 222);
    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    tft.setTextSize(2);
    tft.printf("X=%d Y=%d Z=%d", dx, dy, p.zRaw);
  }
  delay(20);
}

#else  // !TOUCH_PROBE -- normal firmware

void setup() {
  Serial.begin(115200);
  bootMillis = millis();

  pinMode(0, INPUT);
  attachInterrupt(0, buttonInt, FALLING);
  // Onboard RGB LED pins (CYD): keep off (active LOW)
  pinMode(4, OUTPUT); digitalWrite(4, HIGH);
  pinMode(16, OUTPUT); digitalWrite(16, HIGH);
  pinMode(17, OUTPUT); digitalWrite(17, HIGH);

  // Load persisted settings before initialising hardware that depends on them
  loadSettings();

  tft.init();
  tft.setRotation(3);
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_RED);

#define ILI9341_GAMMASET 0x26  // Gamma curve command for ILI9341
#ifdef ENV_CYD2B
  tft.writecommand(ILI9341_GAMMASET);
  tft.writedata(2);
  delay(120);
  tft.writecommand(ILI9341_GAMMASET);
  tft.writedata(1);
#endif

  tft.setTextSize(3);
  tft.setSwapBytes(true);
  tft.setViewport(0, 0, 320, 240);

  initBacklight();

  // Initialize SPIFFS
  if (!SPIFFS.begin(true)) {
    Serial.println("SPIFFS Mount Failed");
    showStatusScreen("Storage Error",
                     "Internal flash storage (SPIFFS) failed to mount.\n\nThe device cannot continue.\nTry re-flashing the firmware image.",
                     false);
    while (true) delay(1000);
  }

  // List SPIFFS contents (debug only)
  Serial.println("Listing files in SPIFFS:");
  File spiffsRoot = SPIFFS.open("/");
  if (spiffsRoot && spiffsRoot.isDirectory()) {
    File file = spiffsRoot.openNextFile();
    while (file) {
      Serial.printf("  %s (%d bytes)\n", file.name(), file.size());
      file = spiffsRoot.openNextFile();
    }
  }

  // Show vanity.jpg splash if available, briefly
  if (SPIFFS.exists("/vanity.jpg")) {
    tft.fillScreen(TFT_BLACK);
    if (jpeg.open("/vanity.jpg", spiffsOpen, spiffsClose, spiffsRead, spiffsSeek, JPEGDraw)) {
      jpeg.decode(0, 0, 0);
      jpeg.close();
    }
  }

  pinMode(XPT2046_IRQ, INPUT);
  ts.begin();
  xSpiMutex = xSemaphoreCreateMutex();
  esp_task_wdt_init(30, true);

  // Brief splash window so the vanity is visible (~2s) without blocking too long
  delay(2000);

  // Show "Connecting to WiFi" status (no QR yet, IP not known)
  showStatusScreen("Connecting to WiFi",
                   "If this screen stays for >30s,\nconnect your phone/PC WiFi to:\n  ESP32_AP\nand open http://192.168.4.1\nto configure your network.",
                   false);

  // Connect to WiFi using WiFiManager. If no saved creds, captive portal opens.
  // On portal timeout, fall back to offline slideshow rather than dead-looping.
  wm.setConfigPortalTimeout(180);
  bool wifiConnected = wm.autoConnect("ESP32_AP");

  if (!wifiConnected) {
    Serial.println("WiFi setup failed/timed out. Continuing offline.");
    showStatusScreen("Offline Mode",
                     "WiFi setup timed out.\nThe slideshow will still run from the SD card.\n\nReboot and connect to ESP32_AP\nto try WiFi setup again.",
                     false);
    delay(3000);
  } else {
    while (WiFi.status() != WL_CONNECTED) {
      delay(500);
      Serial.println("Waiting for WiFi...");
    }
    currentSsid = WiFi.SSID();

    if (!MDNS.begin(MDNS_HOST)) {
      Serial.println("Error starting mDNS");
    } else {
      Serial.printf("mDNS started: %s.local\n", MDNS_HOST);
    }

    initTimeNTP();

    // ElegantOTA + WebSocket
    ElegantOTA.begin(&server);
    ws.onEvent(onWebSocketEvent);
    server.addHandler(&ws);
    ElegantOTA.onStart([]() { Serial.println("OTA update started"); });
    ElegantOTA.onEnd([](bool success) {
      Serial.printf("OTA finished: %s. Rebooting...\n", success ? "OK" : "FAILED");
      delay(1000);
    });

    setupWebServer();

    // Wait for IP (a few seconds)
    String ip = WiFi.localIP().toString();
    int retries = 0;
    while (ip == "0.0.0.0" && retries < 10) {
      delay(500);
      ip = WiFi.localIP().toString();
      retries++;
    }

    if (ip == "0.0.0.0") {
      Serial.println("Failed to obtain IP address.");
      showStatusScreen("No IP Address",
                       "Connected to WiFi but the router\nnever assigned an IP address.\n\nThe slideshow will still run from the SD card.",
                       false);
      delay(3000);
    } else {
      Serial.printf("Assigned IP: %s\n", ip.c_str());
      currentIp = ip;
    }
  }

  applyBacklight();

  // Initialize SD card and scan for images
  bool sdOk = checkAndMountSDCard();
  if (!sdOk) {
    showStatusScreen("No SD Card",
                     "No SD card was detected.\nInsert a FAT32-formatted card\nwith .jpg images, then reboot.",
                     true);
  } else {
    sdRoot = sd.open("/");
    if (!sdRoot) {
      showStatusScreen("SD Card Error",
                       "Could not open the SD card root\ndirectory. Check the card and reboot.",
                       true);
    } else {
      fileCount = scanImages();
      Serial.printf("Found %d images.\n", fileCount);
      if (shuffleMode) buildShuffleOrder();
      // Prime the SD-space cache up front so /status renders instantly later.
      // freeClusterCount() can take several seconds on a big card; doing it
      // here (before the web server is running) keeps it off the critical path.
      if (xSemaphoreTake(xSpiMutex, portMAX_DELAY) == pdTRUE) {
        refreshSdSpaceCache();
        xSemaphoreGive(xSpiMutex);
      }
      if (fileCount == 0) {
        showStatusScreen("No Images Found",
                         "SD card is mounted but contains\nno .jpg or .jpeg files.\n\nUpload some via the web interface.",
                         true);
      }
    }
  }

  currentIndex = 0;
  if (fileCount > 0) {
    // If we successfully greeted the user with the IP/QR screen, hold it briefly
    // so they have time to scan it before the slideshow takes over.
    if (currentIp.length() > 0) {
      showStatusScreen("PhotoFrame Ready",
                       "Tap left/right to change image.\nTap centre to pause.\nLong-press for IP / QR.",
                       true);
      delay(8000);
    }
    loadImage(currentIndex);
  }

  timer = millis();
}


void loop() {
  // Touch input is sampled every loop pass. Each handler call returns
  // quickly unless an action fires -- in which case it can call loadImage()
  // (decode + display, ~hundreds of ms) and bump `timer`. Capture `now`
  // *after* the handler so we never compare an old `now` against a freshly
  // updated `timer` (uint32_t underflow would make the auto-advance check
  // pass immediately, double-advancing the slideshow on every tap).
  handleTouchInput();
  if (pendingPauseBadge) {
    pendingPauseBadge = false;
    drawPauseBadge();
  }
  if (pendingImageReload) {
    pendingImageReload = false;
    loadImage(currentIndex);
    timer = millis();
  }
  uint32_t now = millis();

  // Slideshow auto-advance is suppressed while paused or while a non-error
  // info overlay is shown. An *image-error* overlay does NOT block the
  // timer -- otherwise a single broken file would freeze the frame until
  // the user touched the screen.
  bool blockAutoAdvance = slideshowPaused ||
                          (inInfoScreen && !errorScreenAdvancesIndex);
  if (fileCount > 0 && !blockAutoAdvance) {
    if ((now - timer > (uint32_t)X * 1000) || buttonPressed) {
      uint16_t nextIdx = (currentIndex + 1) % fileCount;
      if (shuffleMode && nextIdx == 0) buildShuffleOrder();
      currentIndex = nextIdx;
      loadImage(currentIndex);
      timer = millis();
      buttonPressed = false;
    }
  } else if (buttonPressed) {
    buttonPressed = false;  // consume; can't advance right now
  }

  // Re-apply backlight every ~30s so night mode kicks in around the boundary
  static uint32_t lastBacklightMs = 0;
  if (now - lastBacklightMs > 30000) {
    lastBacklightMs = now;
    applyBacklight();
  }

  ElegantOTA.loop();
  ws.cleanupClients();

  delay(10);
}

#endif  // TOUCH_PROBE
