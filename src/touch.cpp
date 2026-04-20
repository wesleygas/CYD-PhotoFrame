#include "touch.h"
#include "display.h"
#include "sdcard.h"

// Map raw XPT2046 touch into display pixel coords for tft.setRotation(3).
// Both axes are inverted because the panel is mounted as the mirror of the
// touch IC's native orientation.
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
// Long-press      -> show IP / QR overlay (any tap dismisses)
// Any tap on a status / info overlay also dismisses it.

const uint32_t TOUCH_LONG_PRESS_MS = 800;
const uint32_t TOUCH_DEBOUNCE_MS   = 150;
const int      TOUCH_Z_THRESHOLD   = 100;

static bool     touchHeld      = false;
static uint32_t touchStartMs   = 0;
static int      touchStartX    = 0;
static int      touchStartY    = 0;
static bool     longPressFired = false;
static uint32_t lastTouchEndMs = 0;

void drawPauseBadge() {
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
    if (longPressFired) return;

    // Short tap dispatch
    if (inInfoScreen) {
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

    if (fileCount == 0) return;

    int w = tft.width();
    if (touchStartX < w / 3) {
      currentIndex = (currentIndex == 0) ? (fileCount - 1) : (currentIndex - 1);
      Serial.printf("[touch] previous (idx=%d)\n", currentIndex);
      loadImage(currentIndex);
      if (slideshowPaused) drawPauseBadge();
      timer = millis();
    } else if (touchStartX >= 2 * w / 3) {
      currentIndex = (currentIndex + 1) % fileCount;
      Serial.printf("[touch] next (idx=%d)\n", currentIndex);
      loadImage(currentIndex);
      if (slideshowPaused) drawPauseBadge();
      timer = millis();
    } else {
      slideshowPaused = !slideshowPaused;
      Serial.printf("[touch] %s\n", slideshowPaused ? "pause" : "resume");
      if (slideshowPaused) {
        drawPauseBadge();
      } else {
        loadImage(currentIndex);
        timer = millis();
      }
    }
  }
}

#endif  // !TOUCH_PROBE
