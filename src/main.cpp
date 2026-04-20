/*
  PhotoFrame - Slideshow with Web Interface and WebSocket Synchronization

  ESP32-powered photo frame: displays a slideshow from SD card, hosts a web
  interface for speed control, image upload/delete, brightness and night-mode
  settings, shuffle mode, and provides a synchronised /slideshow WebSocket page.

  Created by: Grey Lancaster and the Open-Source Community

  Libraries:
    WiFiManager (tzapu), ESPAsyncWebServer (esp32async), TFT_eSPI (Bodmer),
    XPT2046_Bitbang (nitek), SdFat (greiman), JPEGDEC (bitbank2),
    QRCode (ricmoo), ElegantOTA (ayushsharma82)
*/

#include "globals.h"
#include "display.h"
#include "sdcard.h"
#include "settings.h"
#include "touch.h"
#include "web_routes.h"

// ---------------------------------------------------------------------------
// Global object definitions (extern-declared in globals.h)
// ---------------------------------------------------------------------------
TFT_eSPI          tft;
JPEGDEC           jpeg;
XPT2046_Bitbang   ts(XPT2046_MOSI, XPT2046_MISO, XPT2046_CLK, XPT2046_CS);
SPIClass          sdSpi(VSPI);
SdSpiConfig       sdSpiConfig(SD_CS, SHARED_SPI, SD_SCK_MHZ(10), &sdSpi);
SdFat             sd;
SdBaseFile        sdRoot;
SdBaseFile        jpgFile;
AsyncWebServer    server(80);
AsyncWebSocket    ws("/ws");
WiFiManager       wm;
SemaphoreHandle_t xSpiMutex;
Preferences       prefs;

// ---------------------------------------------------------------------------
// Runtime state definitions
// ---------------------------------------------------------------------------
int16_t       currentIndex     = 0;
uint16_t      fileCount        = 0;
uint32_t      timer            = 0;
volatile bool buttonPressed    = false;
bool          sdMounted        = false;
bool          slideshowActive  = true;
bool          slideshowPaused  = false;
volatile bool pendingPauseBadge  = false;
volatile bool pendingImageReload = false;
uint64_t      cachedCardBytes  = 0;
uint64_t      cachedFreeBytes  = 0;
bool          sdSpaceCacheValid = false;
String        currentImageName = "";
String        currentIp        = "";
const char*   MDNS_HOST        = "photoframe";
volatile bool wantsInfoScreen  = false;
bool          inInfoScreen     = false;
bool          errorScreenAdvancesIndex = false;
uint32_t      bootMillis       = 0;
String        currentSsid      = "";
volatile uint32_t lastButtonIsrMs = 0;

// ---------------------------------------------------------------------------
// Settings definitions
// ---------------------------------------------------------------------------
int       X               = 10;
uint8_t   dayBrightness   = 255;
uint8_t   nightBrightness = 20;
uint8_t   nightStartHour  = 22;
uint8_t   nightEndHour    = 7;
bool      nightModeEnabled = true;
int       tzOffsetHours   = 0;
bool      shuffleMode     = false;
uint16_t* shuffleOrder    = nullptr;
const char* PREFS_NS      = "frame";

// ---------------------------------------------------------------------------
// ISR
// ---------------------------------------------------------------------------
void IRAM_ATTR buttonInt() {
  uint32_t now = millis();
  if (now - lastButtonIsrMs < 80) return;  // debounce
  lastButtonIsrMs = now;
  buttonPressed = true;
}

// ---------------------------------------------------------------------------
// TOUCH_PROBE build — minimal setup/loop for hardware verification
// ---------------------------------------------------------------------------
#ifdef TOUCH_PROBE

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
  bool irqLow  = digitalRead(XPT2046_IRQ) == LOW;
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

#else  // !TOUCH_PROBE — normal firmware

// ---------------------------------------------------------------------------
// setup()
// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  bootMillis = millis();

  pinMode(0, INPUT);
  attachInterrupt(0, buttonInt, FALLING);
  // Onboard RGB LED pins (CYD): keep off (active LOW)
  pinMode(4, OUTPUT);  digitalWrite(4, HIGH);
  pinMode(16, OUTPUT); digitalWrite(16, HIGH);
  pinMode(17, OUTPUT); digitalWrite(17, HIGH);

  loadSettings();

  tft.init();
  tft.setRotation(3);
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_RED);

#define ILI9341_GAMMASET 0x26
#ifdef ENV_CYD2B
  tft.writecommand(ILI9341_GAMMASET); tft.writedata(2); delay(120);
  tft.writecommand(ILI9341_GAMMASET); tft.writedata(1);
#endif

  tft.setTextSize(3);
  tft.setSwapBytes(true);
  tft.setViewport(0, 0, 320, 240);

  initBacklight();

  if (!SPIFFS.begin(true)) {
    Serial.println("SPIFFS Mount Failed");
    showStatusScreen("Storage Error",
                     "Internal flash storage (SPIFFS) failed to mount.\n\nThe device cannot continue.\nTry re-flashing the firmware image.",
                     false);
    while (true) delay(1000);
  }

  // Debug: list SPIFFS contents
  Serial.println("Listing files in SPIFFS:");
  File spiffsRoot = SPIFFS.open("/");
  if (spiffsRoot && spiffsRoot.isDirectory()) {
    File f = spiffsRoot.openNextFile();
    while (f) {
      Serial.printf("  %s (%d bytes)\n", f.name(), f.size());
      f = spiffsRoot.openNextFile();
    }
  }

  // Vanity splash from SPIFFS
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

  delay(2000);  // let the splash be visible

  showStatusScreen("Connecting to WiFi",
                   "If this screen stays for >30s,\nconnect your phone/PC WiFi to:\n  ESP32_AP\nand open http://192.168.4.1\nto configure your network.",
                   false);

  wm.setConfigPortalTimeout(180);
  bool wifiConnected = wm.autoConnect("ESP32_AP");

  if (!wifiConnected) {
    Serial.println("WiFi setup failed/timed out. Continuing offline.");
    showStatusScreen("Offline Mode",
                     "WiFi setup timed out.\nThe slideshow will still run from the SD card.\n\nReboot and connect to ESP32_AP\nto try WiFi setup again.",
                     false);
    delay(3000);
  } else {
    while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.println("Waiting for WiFi..."); }
    currentSsid = WiFi.SSID();

    if (!MDNS.begin(MDNS_HOST)) {
      Serial.println("Error starting mDNS");
    } else {
      Serial.printf("mDNS started: %s.local\n", MDNS_HOST);
    }

    initTimeNTP();

    ElegantOTA.begin(&server);
    ws.onEvent(onWebSocketEvent);
    server.addHandler(&ws);
    ElegantOTA.onStart([]() { Serial.println("OTA update started"); });
    ElegantOTA.onEnd([](bool success) {
      Serial.printf("OTA finished: %s. Rebooting...\n", success ? "OK" : "FAILED");
      delay(1000);
    });

    setupWebServer();

    String ip = WiFi.localIP().toString();
    for (int retries = 0; ip == "0.0.0.0" && retries < 10; retries++) {
      delay(500);
      ip = WiFi.localIP().toString();
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

// ---------------------------------------------------------------------------
// loop()
// ---------------------------------------------------------------------------
void loop() {
  // Capture `now` *after* handleTouchInput: the handler can call loadImage()
  // (hundreds of ms) and bump `timer`. A stale `now` would cause uint32_t
  // underflow and immediately trip the auto-advance check.
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
    buttonPressed = false;
  }

  // Re-apply backlight every ~30s so night-mode kicks in around the boundary
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
