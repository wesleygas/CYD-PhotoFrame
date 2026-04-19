# PhotoFrame Backlog

## Pre-Step (gating)

- [x] **Touchscreen hardware probe**: touch IC works on the user's CYD board. Raw axes are inverted relative to `tft.setRotation(3)`; the probe now mirrors X and Y so the green dot lands under the finger. Same mapping needs to be applied wherever touch input is consumed in the main firmware.

## Quick wins (high impact, low effort)

- [x] Persist slideshow speed (`X`) across reboots via NVS / `Preferences`
- [x] Accept `.jpeg` extension alongside `.jpg` (case-insensitive) in file scan and `loadImage`
- [x] Re-display IP / QR code on demand -- long-press the screen to show the IP/QR overlay, tap to dismiss
- [x] Shorten boot delays (vanity 10s -> 2s; removed 10s instructional screen; removed 10s+5s QR countdown)
- [x] Fix WiFi failure dead-loop -- now falls back to offline SD-only slideshow after captive portal timeout
- [x] Remove audio subsystem entirely
  - [x] Drop `ESP8266Audio` from `lib_deps` in [platformio.ini](platformio.ini)
  - [x] Remove `AudioGeneratorWAV`, `AudioOutputI2S`, `playWAVTask`, `playMusic`, SdFat <-> Arduino-SD swap
  - [x] Remove `/play-music` route + home-page link
  - [x] Remove post-upload auto-play of `music.wav`
- [x] README cleanup: removed "OTA not yet implemented" strikethrough; scrubbed all audio / `.wav` / `.mp3` / Audacity references

## On-device UX (high impact, moderate effort)

- [x] Differentiate "no SD card" vs "no images found" on screen
- [x] Show QR code even when no image is found (status screen always renders QR when an IP is available)
- [x] On-screen error for unsupported / failed-to-decode image, including the offending filename
- [x] PWM backlight + night-mode auto-dim
  - [x] Drive backlight via LEDC PWM (channel 7 on TFT_BL = GPIO 21)
  - [x] Configurable day brightness, night brightness, and night start/end hours via `/display`
  - [x] Time source: NTP (UTC) after WiFi connects; user adjusts hours to match local timezone
  - [x] Persisted in NVS

## Web interface

- [ ] Auto-convert / resize images on upload via website (target `.jpg` 320x240)
- [x] Server-side upload validation: rejects anything whose name is not `.jpg`/`.jpeg` before touching the SD, returns a 400 with the reason
- [ ] Server-side size / resolution validation on upload (decode header up-front and reject oversize)
- [x] `/status` page: SD free space, image count, WiFi signal, IP, firmware build, uptime, current image, settings
- [x] Delete page: keep listing all files, prominent red warning + JS confirm dialog; non-image files are visually marked

## Touch-dependent (probe passed, mapping calibrated)

- [x] Tap right third -> next, left third -> previous
- [x] Tap centre -> pause / resume (auto-advance is gated on `slideshowPaused` and a `PAUSE` badge is drawn while paused)
- [x] Long-press -> show IP / QR overlay (any tap dismisses)
- [x] `/status` reports the current playback state (`running` / `paused` / `info screen`)
- [ ] Surface the same pause toggle on the web UI so the frame can be paused remotely

## Reliability

- [x] On-screen feedback if SPIFFS mount fails (currently Serial-only)
- [x] Removed duplicate WebSocket registration
- [x] **`/status` page no longer crashes**: SD-card calls (`sectorCount`, `freeClusterCount`) were running on the AsyncTCP task and racing the slideshow thread on the SPI bus. Now wrapped in `xSpiMutex`, same treatment applied to `/delete` and `/delete_files`.
- [x] **Upload no longer races the slideshow**: chunked upload handler stops the slideshow and grabs `xSpiMutex` at `index == 0`, releases on `final` (or on rejection).
- [x] **Bad / oversized JPEGs surface an on-screen error** instead of a silent black screen. We check `getWidth/getHeight` (cap 1280x800) and the return value of `jpeg.decode()`.
- [ ] Profile actual JPEG memory ceiling on the device and tighten the cap if needed.

## Nice to have (higher effort)

- [ ] Shuffle / random playback mode
- [ ] Image thumbnails in the web file-management UI
- [ ] Ship a self-contained `convert.html` on the SD card root: drag-and-drop image, downscale + re-encode to 320x240 `.jpg` in the browser, save back to SD. Handy when copying photos directly to the card without the device's web UI.
- [ ] Show the device's current wall-clock time on `/status` (and ideally on `/display`) so the user can confirm NTP is working and that night-mode hours line up with reality. Add a timezone offset / POSIX `TZ` field to `/display` and persist it in NVS, then feed it to `setenv("TZ", ...)` + `tzset()` so the on-device clock and the night-mode schedule both follow local time instead of raw UTC.

-----

## Code organisation

- [x] **Extract every web page out of `1-Slideshow.cpp` and serve from SPIFFS.** Each route used to inline a `R"rawliteral(...)"` HTML blob with its own copy of the same ~50-line stylesheet -- 22 of them. Now `data/web/*.html` plus a single `data/web/style.css` are loaded via `request->send(SPIFFS, ...)` with a shared `webProcessor` for `%X% / %DAY_BR% / %NIGHT_*%` etc. and per-route lambdas for the dynamic chunks (`%FILE_LIST%`, `%STATUS_ROWS%`, `%DELETE_RESULT%`, `%REJECT_REASON%`). Result: `1-Slideshow.cpp` is down from 2044 to ~1410 lines, every route handler is a few lines, and the web UI can be edited with normal HTML/CSS tooling.
  - Two-step upload now: `pio run -t upload` for firmware, `pio run -t uploadfs` for `data/`.
  - OTA still works for both: ElegantOTA's `/update` page has a `Mode` toggle (Firmware / Filesystem). The README has the full workflow.
  - `sendHtml()` falls back to a clear "Web UI not uploaded" error if someone flashes only the firmware -- avoids a confusing blank page.

## Resolved follow-up feedback (2026-04-19)

- [x] **Red and blue swapped on `cyd2usb`** -- not a full inversion (that flipped *every* color, e.g. green dots became magenta). Fixed by setting an explicit `-DTFT_RGB_ORDER=TFT_BGR` (and keeping `TFT_INVERSION_OFF`) on `env:cyd2usb` and `env:touch_probe_cyd2usb`.
- [x] **Status / error message hard to spot** -- `showStatusScreen()` now uses a full-width red title banner and a yellow-bordered panel for the message body (auto-sized to text size 2 when it fits). The message is the largest, brightest thing on the screen.
- [x] **Uploading a video file crashed the ESP** -- now rejected at the first chunk before the SD is touched.
- [x] **Big JPEG produced a black screen with no feedback** -- size / decode failures now show a status screen with the filename and dimensions.
- [x] **`/status` page panic** (`xQueueGenericSend` assert in `spiEndTransaction`) -- caused by SD/SPI access from the AsyncTCP task without the SPI mutex; fixed.
- [x] **Touch axes inverted in probe** -- `mapTouch()` mirrors X and Y to match `tft.setRotation(3)`.
- [x] **Image-error screen looped on dismissal** (regression introduced with the touch UX): dismissing called `loadImage(currentIndex)` which re-decoded the same bad file, and gating auto-advance on `!inInfoScreen` also disabled the auto-skip-past-bad-image behavior. Added an `errorScreenAdvancesIndex` flag, set by the three `decodeJpeg` failure paths and reset by every other `showStatusScreen` call. Both manual tap-dismissal and the slideshow timer skip to the next image when the flag is set; long-press info overlays still wait for a tap.
- [x] **Tap left/right advanced two images at once** (touch-UX regression): `loop()` captured `now = millis()` *before* calling `handleTouchInput()`. The touch handler then ran `loadImage()` (a few hundred ms) and bumped `timer` to the new `millis()`. The auto-advance check that followed used the stale `now`, so `now - timer` underflowed `uint32_t` to a huge value, immediately tripping the timer-elapsed branch and advancing again. Pause hid the bug because the auto-advance branch was skipped entirely. Fixed by re-reading `now` after `handleTouchInput()`.
- [x] **`/status` was slow and blocked the whole device** (multiple seconds, slideshow froze, other web requests queued). Cause: `sd.vol()->freeClusterCount()` walks the entire FAT (megabytes of SPI reads on a 32 GB card), and we were calling it on every request while holding `xSpiMutex` from the AsyncTCP task -- which serialises *all* async web work onto a single task, so every other route stalled too. Fix: cache `cachedCardBytes` and `cachedFreeBytes` in RAM, refreshed once at boot (before WiFi) and after every successful upload / delete via a new `refreshSdSpaceCache()` helper. `buildStatusRows` is now a pure in-memory render with zero SD I/O and zero mutex acquisitions -- `/status` returns in microseconds.