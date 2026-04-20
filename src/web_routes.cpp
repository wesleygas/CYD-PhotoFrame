#include "web_routes.h"
#include "sdcard.h"
#include "settings.h"
#include "display.h"

// Per-upload state set by the chunk callback and read by the completion handler.
static bool   uploadRejected     = false;
static String uploadRejectReason;

// ---------------------------------------------------------------------------
// Template processor shared across all routes
// ---------------------------------------------------------------------------
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

// Send a SPIFFS HTML template. Falls back to a descriptive 500 if the
// filesystem image has not been uploaded yet.
void sendHtml(AsyncWebServerRequest* req, const char* path,
              AwsTemplateProcessor processor) {
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

// ---------------------------------------------------------------------------
// Per-route data builders (run under xSpiMutex where noted)
// ---------------------------------------------------------------------------

// Caller MUST hold xSpiMutex.
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
    html += "<div class='file-row'>";
    html += "<input type='checkbox' name='file' value='" + String(name) + "'>";
    if (isImg) {
      html += "<img class='thumb' src='/sd-image?name=" + String(name) + "' loading='lazy'>";
    }
    html += "<span class='filename " + String(isImg ? "image" : "other") + "'>" + String(name);
    if (!isImg) html += " (not an image)";
    html += "</span></div>";
    fileFound = true;
    entry.close();
  }
  if (!fileFound) html = "<p>No files on SD card.</p>";
  return html;
}

// Pure in-memory render — no SD I/O, no mutex needed.
String buildStatusRows() {
  uint32_t up   = millis() / 1000;
  uint32_t days = up / 86400; up %= 86400;
  uint32_t hrs  = up / 3600;  up %= 3600;
  uint32_t mins = up / 60;    uint32_t secs = up % 60;
  char uptimeStr[64];
  snprintf(uptimeStr, sizeof(uptimeStr), "%lud %02lu:%02lu:%02lu",
           (unsigned long)days, (unsigned long)hrs,
           (unsigned long)mins, (unsigned long)secs);

  String html;
  auto row = [&](const String& k, const String& v) {
    html += "<tr><th>" + k + "</th><td>" + v + "</td></tr>";
  };

  row("Firmware build",  String(__DATE__) + " " + String(__TIME__));
  row("Uptime",          String(uptimeStr));

  char timeBuf[32] = "(NTP not yet synced)";
  struct tm tm_now;
  if (getLocalTime(&tm_now, 0)) strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M:%S", &tm_now);
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

  row("Day brightness",  String(dayBrightness));
  row("Night brightness",String(nightBrightness));
  row("Night window",    nightModeEnabled
                           ? (String(nightStartHour) + ":00 to " + String(nightEndHour) + ":00")
                           : String("disabled"));
  return html;
}

// ---------------------------------------------------------------------------
// Upload handlers
// ---------------------------------------------------------------------------
void handleFileUpload(AsyncWebServerRequest* request) {
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

    AsyncWebServerResponse* response = request->beginResponse(
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

  if (xSemaphoreTake(xSpiMutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
    fileCount = scanImages();
    if (shuffleMode) buildShuffleOrder();
    xSemaphoreGive(xSpiMutex);
  }
  currentIndex = 0;
  restartSlideshow();
}

// ---------------------------------------------------------------------------
// WebSocket handler
// ---------------------------------------------------------------------------
void onWebSocketEvent(AsyncWebSocket* server, AsyncWebSocketClient* client,
                      AwsEventType type, void* arg, uint8_t* data, size_t len) {
  if (type == WS_EVT_CONNECT) {
    Serial.printf("WebSocket client #%u connected from %s\n",
                  client->id(), client->remoteIP().toString().c_str());
    client->text("update");
  } else if (type == WS_EVT_DISCONNECT) {
    Serial.printf("WebSocket client #%u disconnected\n", client->id());
  }
}

// ---------------------------------------------------------------------------
// Route setup
// ---------------------------------------------------------------------------
void setupWebServer() {
  server.serveStatic("/style.css",   SPIFFS, "/web/style.css")
        .setCacheControl("max-age=3600");
  server.serveStatic("/favicon.ico", SPIFFS, "/favicon.ico")
        .setCacheControl("max-age=86400");

  server.on("/", HTTP_GET, [](AsyncWebServerRequest* r) {
    sendHtml(r, "/web/index.html");
  });
  server.on("/about", HTTP_GET, [](AsyncWebServerRequest* r) {
    sendHtml(r, "/web/about.html");
  });
  server.on("/convert", HTTP_GET, [](AsyncWebServerRequest* r) {
    sendHtml(r, "/web/convert.html");
  });
  server.on("/slideshow", HTTP_GET, [](AsyncWebServerRequest* r) {
    sendHtml(r, "/web/slideshow.html");
  });

  server.on("/speed", HTTP_GET, [](AsyncWebServerRequest* r) {
    sendHtml(r, "/web/speed.html");
  });
  server.on("/set-speed", HTTP_POST, [](AsyncWebServerRequest* r) {
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

  server.on("/upload_file", HTTP_GET, [](AsyncWebServerRequest* r) {
    sendHtml(r, "/web/upload.html");
  });
  server.on("/upload_file", HTTP_POST, handleFileUpload,
    [](AsyncWebServerRequest* request, const String& filename,
       size_t index, uint8_t* data, size_t len, bool final) {
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

  server.on("/delete", HTTP_GET, [](AsyncWebServerRequest* r) {
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

  server.on("/delete_files", HTTP_POST, [](AsyncWebServerRequest* r) {
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
        if (!sd.remove(fileToDelete.c_str())) {
          Serial.printf("Failed to delete file: %s\n", fileToDelete.c_str());
          deletionSuccess = false;
        } else {
          Serial.printf("File deleted: %s\n", fileToDelete.c_str());
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

  server.on("/status", HTTP_GET, [](AsyncWebServerRequest* r) {
    String rows = buildStatusRows();
    sendHtml(r, "/web/status.html", [rows](const String& v) -> String {
      if (v == "STATUS_ROWS") return rows;
      return webProcessor(v);
    });
  });

  server.on("/refresh-sd-space", HTTP_POST, [](AsyncWebServerRequest* r) {
    if (xSemaphoreTake(xSpiMutex, pdMS_TO_TICKS(15000)) == pdTRUE) {
      refreshSdSpaceCache();
      xSemaphoreGive(xSpiMutex);
    } else {
      Serial.println("/refresh-sd-space: SPI mutex busy, skipping");
    }
    r->redirect("/status");
  });

  server.on("/toggle-pause", HTTP_POST, [](AsyncWebServerRequest* r) {
    slideshowPaused = !slideshowPaused;
    Serial.printf("[web] slideshow %s\n", slideshowPaused ? "paused" : "resumed");
    if (slideshowPaused) pendingPauseBadge  = true;
    else                 pendingImageReload = true;
    r->redirect("/");
  });

  server.on("/toggle-shuffle", HTTP_POST, [](AsyncWebServerRequest* r) {
    shuffleMode = !shuffleMode;
    Serial.printf("[web] shuffle %s\n", shuffleMode ? "on" : "off");
    if (shuffleMode) buildShuffleOrder();
    saveSettings();
    r->redirect("/");
  });

  server.on("/display", HTTP_GET, [](AsyncWebServerRequest* r) {
    sendHtml(r, "/web/display.html");
  });
  server.on("/set-display", HTTP_POST, [](AsyncWebServerRequest* r) {
    auto getInt = [&](const char* name, int def) -> long {
      if (r->hasParam(name, true)) return r->getParam(name, true)->value().toInt();
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

  // Serve the currently-displayed image (used by the /slideshow web viewer)
  server.on("/current_image", HTTP_GET, [](AsyncWebServerRequest* request) {
    if (currentImageName == "") { request->send(404, "text/plain", "No image"); return; }
    String requestImageName = currentImageName;
    {
      xSemaphoreTake(xSpiMutex, portMAX_DELAY);
      SdBaseFile* f = new SdBaseFile();
      if (!f->open(requestImageName.c_str(), O_RDONLY)) {
        xSemaphoreGive(xSpiMutex);
        delete f;
        request->send(404, "text/plain", "Image not found");
        return;
      }
      f->close();
      delete f;
      xSemaphoreGive(xSpiMutex);
    }
    AsyncWebServerResponse* response = request->beginChunkedResponse("image/jpeg",
      [requestImageName](uint8_t* buffer, size_t maxLen, size_t index) -> size_t {
        xSemaphoreTake(xSpiMutex, portMAX_DELAY);
        SdBaseFile file;
        if (!file.open(requestImageName.c_str(), O_RDONLY)) {
          xSemaphoreGive(xSpiMutex);
          return 0;
        }
        if (index > 0) file.seekSet(index);
        size_t bytesRead = file.read(buffer, maxLen);
        file.close();
        xSemaphoreGive(xSpiMutex);
        return bytesRead;
      });
    response->addHeader("Content-Disposition", "inline; filename=" + requestImageName);
    response->addHeader("Access-Control-Allow-Origin", "*");
    request->send(response);
  });

  // Serve any SD-card image by name (used for thumbnails on /delete)
  server.on("/sd-image", HTTP_GET, [](AsyncWebServerRequest* r) {
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
    AsyncWebServerResponse* resp = r->beginChunkedResponse("image/jpeg",
      [path](uint8_t* buffer, size_t maxLen, size_t index) -> size_t {
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

  server.onNotFound([](AsyncWebServerRequest* request) {
    request->send(404, "text/html",
                  "<h3>404 - Page Not Found</h3>"
                  "<p><a href=\"/\">Back to PhotoFrame</a></p>");
  });

  Serial.println("Starting web server...");
  server.begin();
  Serial.println("Web server started.");
}
