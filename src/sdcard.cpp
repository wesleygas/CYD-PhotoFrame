#include "sdcard.h"
#include "display.h"

// ---------------------------------------------------------------------------
// JPEG decoder callbacks (SD card)
// ---------------------------------------------------------------------------
int JPEGDraw(JPEGDRAW* pDraw) {
  tft.pushImage(pDraw->x, pDraw->y, pDraw->iWidth, pDraw->iHeight, pDraw->pPixels);
  return 1;
}

void* myOpen(const char* filename, int32_t* size) {
  jpgFile = sd.open(filename);
  *size = jpgFile.fileSize();
  return &jpgFile;
}

void myClose(void* handle) {
  if (jpgFile) jpgFile.close();
}

int32_t myRead(JPEGFILE* handle, uint8_t* buffer, int32_t length) {
  return jpgFile.read(buffer, length);
}

int32_t mySeek(JPEGFILE* handle, int32_t position) {
  return jpgFile.seekSet(position);
}

// ---------------------------------------------------------------------------
// JPEG decoder callbacks (SPIFFS — vanity splash)
// ---------------------------------------------------------------------------
void* spiffsOpen(const char* filename, int32_t* size) {
  File f = SPIFFS.open(filename);
  if (!f) {
    Serial.printf("Failed to open %s from SPIFFS\n", filename);
    return NULL;
  }
  *size = f.size();
  return (void*)new File(f);
}

void spiffsClose(void* handle) {
  if (handle) {
    File* f = (File*)handle;
    f->close();
    delete f;
  }
}

int32_t spiffsRead(JPEGFILE* handle, uint8_t* buffer, int32_t length) {
  File* f = (File*)handle->fHandle;
  return f->read(buffer, length);
}

int32_t spiffsSeek(JPEGFILE* handle, int32_t position) {
  File* f = (File*)handle->fHandle;
  return f->seek(position);
}

// ---------------------------------------------------------------------------
// SD card helpers
// ---------------------------------------------------------------------------
void stopSlideshow() {
  slideshowActive = false;
  xSemaphoreTake(xSpiMutex, portMAX_DELAY);
  jpeg.close();
  xSemaphoreGive(xSpiMutex);
  delay(500);
  Serial.println("Slideshow stopped.");
}

void restartSlideshow() {
  slideshowActive = true;
  loadImage(currentIndex);
  Serial.println("Slideshow restarted.");
}

bool isImageFile(const char* name) {
  size_t len = strlen(name);
  if (len >= 4 && strcasecmp(name + len - 4, ".jpg") == 0) return true;
  if (len >= 5 && strcasecmp(name + len - 5, ".jpeg") == 0) return true;
  return false;
}

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

void loadImage(uint16_t logicalIndex) {
  if (!slideshowActive) return;

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
          currentImageName = String(name);
          decodeJpeg(name);
          entry.close();
          ws.textAll("update");
          return;
        }
        index++;
      }
    }
    entry.close();
  }
}

void decodeJpeg(const char* name) {
  if (!slideshowActive) return;

  xSemaphoreTake(xSpiMutex, portMAX_DELAY);
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

bool checkAndMountSDCard() {
  if (!sdMounted) {
    if (!sd.begin(sdSpiConfig)) {
      Serial.println("SD Card Mount Failed");
      sdMounted = false;
      return false;
    }
    sdMounted = true;
    Serial.println("SD Card Mounted Successfully");
  }
  return true;
}

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
