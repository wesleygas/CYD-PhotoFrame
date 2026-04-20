#pragma once

#include <WiFi.h>
#include <WiFiManager.h>
#include <AsyncTCP.h>
#include <AsyncEventSource.h>
#include <TFT_eSPI.h>
#include <XPT2046_Bitbang.h>
#include <SPI.h>
#include <SdFat.h>
#include <JPEGDEC.h>
#include "SPIFFS.h"
#include "qrcode.h"
#include <FS.h>
#include "esp_task_wdt.h"
#include "esp_heap_caps.h"
#include <ESPmDNS.h>
#include <Preferences.h>
#include <time.h>
#include <ElegantOTA.h>

// Touch Screen pins
#define XPT2046_IRQ  36
#define XPT2046_MOSI 32
#define XPT2046_MISO 39
#define XPT2046_CLK  25
#define XPT2046_CS   33

// SD card chip select
#define SD_CS 5

// Backlight PWM
#ifndef TFT_BL
#define TFT_BL 21
#endif
#define BACKLIGHT_LEDC_CHANNEL 7
#define BACKLIGHT_LEDC_FREQ    5000
#define BACKLIGHT_LEDC_RES     8

// Touch calibration constants (panel is mirrored vs tft.setRotation(3))
constexpr int TS_X_MIN = 200;
constexpr int TS_X_MAX = 3900;
constexpr int TS_Y_MIN = 200;
constexpr int TS_Y_MAX = 3900;

// ---------------------------------------------------------------------------
// Hardware objects
// ---------------------------------------------------------------------------
extern TFT_eSPI          tft;
extern JPEGDEC           jpeg;
extern XPT2046_Bitbang   ts;
extern SPIClass          sdSpi;
extern SdSpiConfig       sdSpiConfig;
extern SdFat             sd;
extern SdBaseFile        sdRoot;
extern SdBaseFile        jpgFile;
extern AsyncWebServer    server;
extern AsyncWebSocket    ws;
extern WiFiManager       wm;
extern SemaphoreHandle_t xSpiMutex;
extern Preferences       prefs;

// ---------------------------------------------------------------------------
// Runtime state
// ---------------------------------------------------------------------------
extern int16_t        currentIndex;
extern uint16_t       fileCount;
extern uint32_t       timer;
extern volatile bool  buttonPressed;
extern bool           sdMounted;
extern bool           slideshowActive;
extern bool           slideshowPaused;
extern volatile bool  pendingPauseBadge;   // set by web handler, drawn by main loop
extern volatile bool  pendingImageReload;  // set by web handler, triggers loadImage in main loop
extern uint64_t       cachedCardBytes;
extern uint64_t       cachedFreeBytes;
extern bool           sdSpaceCacheValid;
extern String         currentImageName;
extern String         currentIp;
extern const char*    MDNS_HOST;
extern volatile bool  wantsInfoScreen;
extern bool           inInfoScreen;
extern bool           errorScreenAdvancesIndex;
extern uint32_t       bootMillis;
extern String         currentSsid;
extern volatile uint32_t lastButtonIsrMs;

// ---------------------------------------------------------------------------
// User settings (persisted in NVS via Preferences)
// ---------------------------------------------------------------------------
extern int       X;                 // slideshow interval (seconds)
extern uint8_t   dayBrightness;
extern uint8_t   nightBrightness;
extern uint8_t   nightStartHour;
extern uint8_t   nightEndHour;
extern bool      nightModeEnabled;
extern int       tzOffsetHours;     // UTC offset (-12..+14)
extern bool      shuffleMode;
extern uint16_t* shuffleOrder;      // heap-allocated permutation array
extern const char* PREFS_NS;
