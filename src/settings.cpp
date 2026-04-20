#include "settings.h"

void loadSettings() {
  prefs.begin(PREFS_NS, true);
  X                = prefs.getInt("speed", 10);
  if (X < 1) X    = 10;
  dayBrightness    = prefs.getUChar("dayBri",  255);
  nightBrightness  = prefs.getUChar("nightBri", 20);
  nightStartHour   = prefs.getUChar("nStart",   22);
  nightEndHour     = prefs.getUChar("nEnd",       7);
  nightModeEnabled = prefs.getBool("nightOn",   true);
  tzOffsetHours    = constrain(prefs.getInt("tzOffset", 0), -12, 14);
  shuffleMode      = prefs.getBool("shuffle",  false);
  prefs.end();
  Serial.printf("Loaded settings: X=%d dayBri=%u nightBri=%u night=%u-%u on=%d tz=%+d shuffle=%d\n",
                X, dayBrightness, nightBrightness, nightStartHour, nightEndHour,
                nightModeEnabled, tzOffsetHours, shuffleMode);
}

void saveSettings() {
  prefs.begin(PREFS_NS, false);
  prefs.putInt("speed",     X);
  prefs.putUChar("dayBri",  dayBrightness);
  prefs.putUChar("nightBri",nightBrightness);
  prefs.putUChar("nStart",  nightStartHour);
  prefs.putUChar("nEnd",    nightEndHour);
  prefs.putBool("nightOn",  nightModeEnabled);
  prefs.putInt("tzOffset",  tzOffsetHours);
  prefs.putBool("shuffle",  shuffleMode);
  prefs.end();
}

void initBacklight() {
  ledcSetup(BACKLIGHT_LEDC_CHANNEL, BACKLIGHT_LEDC_FREQ, BACKLIGHT_LEDC_RES);
  ledcAttachPin(TFT_BL, BACKLIGHT_LEDC_CHANNEL);
  ledcWrite(BACKLIGHT_LEDC_CHANNEL, dayBrightness);
}

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
        inNight = (h >= nightStartHour || h < nightEndHour);
      }
      target = inNight ? nightBrightness : dayBrightness;
    }
  }
  ledcWrite(BACKLIGHT_LEDC_CHANNEL, target);
}

// Build POSIX TZ string from a standard UTC offset.
// POSIX offsets are WEST of UTC — opposite sign from the conventional notation.
// Examples: offset=-3 (Buenos Aires) → "<UTC-3>3"
//           offset=+1 (Paris)        → "<UTC+1>-1"
String makePosixTz(int offset) {
  if (offset == 0) return "UTC0";
  String s = "<UTC";
  if (offset > 0) s += "+";
  s += String(offset) + ">" + String(-offset);
  return s;
}

String tzLabel(int offset) {
  if (offset == 0) return "UTC";
  String s = "UTC";
  if (offset > 0) s += "+";
  s += String(offset);
  return s;
}

// Fisher-Yates shuffle of indices 0..fileCount-1.
// Called after scanImages() when shuffle mode is on, and again each time the
// index wraps to 0 so every cycle uses a fresh random order.
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

void initTimeNTP() {
  configTzTime(makePosixTz(tzOffsetHours).c_str(), "pool.ntp.org", "time.nist.gov");
}
