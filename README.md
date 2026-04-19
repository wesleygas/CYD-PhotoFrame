# PhotoFrame

A photo frame project powered by the ESP32 microcontroller with a built-in 320x240 TFT display. Ideal for showcasing slideshows of your favourite images.

---

## Easiest Way to Install

The quickest way to get PhotoFrame running is to use the **Web Installer** here:
[**CYD-PhotoFrame Web Installer**](https://grey-lancaster.github.io/CYD-PhotoFrame/)

This method lets you flash the firmware directly from your browser — no additional tools required.
If you prefer, you can follow the **PlatformIO instructions below** to build and upload from source.

---

## Features

- **TFT Display**: Displays photos at 320x240 resolution.
- **Slideshow**: Automatically cycles through `.jpg` / `.jpeg` images stored on the SD card.
- **Touch Controls**: Tap left / right to change image, tap centre to pause, long-press for the IP / QR overlay.
- **WiFi Configuration**: Connect to WiFi using the built-in WiFiManager captive portal.
- **Web Interface**: Upload, delete, configure, and view a synchronized slideshow from any browser on your network.
- **Status Page**: At a glance, see SD usage, image count, WiFi info, IP, firmware build, uptime, and playback state.
- **Display Settings**: Configurable backlight brightness with optional NTP-based night mode (auto-dim during set hours).
- **Persistent Settings**: Slideshow speed, brightness, and night mode are saved across reboots.
- **OTA Updates**: Update both the firmware **and** the web UI / vanity image wirelessly via [ElegantOTA](https://docs.elegantota.pro/) at `/update` — pick `Firmware` for `firmware.bin`, or `Filesystem` for `spiffs.bin`.
- **QR Code Helper**: Boot screen and error screens show a QR code to the device's web UI so phones can connect with one scan.
- **Compact Design**: Built for the "Cheap Yellow Display" (CYD) board, a cost-effective ESP32 + display under $20.

---

## Board Description

This project is designed for the **Cheap Yellow Display (CYD)** ESP32 board, featuring:

- 320x240 TFT display
- SD card slot
- GPIO pins
- Resistive touchscreen, used for in-frame controls (tap zones + long-press). A diagnostic probe build is also available — see [Touchscreen Probe](#touchscreen-probe).

You can purchase this board on:

- [Amazon](https://amzn.to/3UVQwrV) (under $20) *(right-click to open in a new tab)*
- [AliExpress](#) (under $13, 2-week shipping)

---

## Setup Instructions (PlatformIO Method)

PhotoFrame ships in **two pieces**:

- The **firmware** (`firmware.bin`) -- the C++ program.
- The **filesystem image** (`spiffs.bin`) -- the web UI templates (`data/web/*.html`, `data/web/style.css`), the boot splash (`data/vanity.jpg`), and `data/favicon.ico`.

You upload them separately. After the first install, day-to-day updates can be done over-the-air for either side via `/update`.

1. **First-time install (USB)**:

   ```bash
   # Pick the env that matches your CYD variant: cyd, cyd2usb, or cyd2b.
   pio run -e cyd2usb -t upload      # firmware
   pio run -e cyd2usb -t uploadfs    # filesystem (HTML / CSS / vanity.jpg)
   ```

   Skip `uploadfs` only if you don't care about the boot splash AND have already uploaded the web UI on a previous run -- without the filesystem image, every page returns a "Web UI not uploaded" message that points you back to this step.

2. **Optional: customise the splash**:
   - Drop a 320x240 JPEG named `vanity.jpg` into the `data/` folder before running `pio run -t uploadfs`.
   - Web UI templates live under `data/web/` and can be edited in any text editor; rerun `uploadfs` to apply.

3. **WiFi Configuration**:
   - On first boot (or if no saved network is reachable), the device opens an AP called `ESP32_AP`.
   - Connect to it with your phone or PC and visit `http://192.168.4.1` to pick your home network.
   - The captive portal times out after 3 minutes; if it does, the device will continue running offline from the SD card.

4. **Start the Frame**:
   - Once connected, the frame will:
     - Briefly show your `vanity.jpg` (or a black screen).
     - Display the device IP, mDNS name, and a QR code to the web UI.
     - Begin the slideshow from the SD card.

---

## Web Interface

Access the web interface at `http://photoframe.local` or `http://<device-IP>` (e.g., `http://192.168.1.x`) to:

- Upload images (`/upload_file`)
- Delete files from the SD card (`/delete`) — every file on the card is listed; a clear warning is shown.
- Adjust slideshow speed (`/speed`) — saved to NVS and persists across reboots.
- Configure display brightness and night-mode auto-dim (`/display`).
- View a live, WebSocket-synchronized slideshow page (`/slideshow`).
- See diagnostics on the status page (`/status`).
- Update the firmware over the air (`/update`, ElegantOTA).

---

## Preparing Images

- Resize images to `320x240` pixels for best results.
- Save as `.jpg` or `.jpeg` (case-insensitive). PNG / other formats are not supported on the device.
- Larger images may still display but will use more decode time.

---

## Touch Controls

When the slideshow is showing an image:

| Gesture | Action |
| --- | --- |
| Tap left third | Previous image |
| Tap right third | Next image |
| Tap centre | Pause / resume (a `PAUSE` badge appears in the top-right while paused) |
| Long-press (>= 0.8 s) | Show IP / QR / mDNS overlay |
| Tap any status overlay | Dismiss back to the slideshow |

The hardware `BOOT` button still advances to the next image.

---

## Touchscreen Probe

The CYD's touchscreen IC varies between revisions. The diagnostic build streams raw `X / Y / Z` samples to Serial and to the screen, which is handy for re-calibrating if your panel is mounted differently:

```bash
pio run -e touch_probe_cyd       -t upload   # default CYD
pio run -e touch_probe_cyd2usb   -t upload   # ST7789-based CYD
pio run -e touch_probe_cyd2b     -t upload   # CYD2B variant
```

Tap the screen and watch:

- A green dot appears where you touched.
- The bottom of the screen and the serial monitor show raw `X / Y / Z` values.

If nothing happens on any variant, the bitbang touch driver may not match your board and an alternative library (e.g., `XPT2046_Touchscreen` over SPI) would be needed.

---

## Advanced Features

- **mDNS**: Access your device via `photoframe.local` without needing the IP address.
- **OTA Updates**: Push firmware **or** filesystem images via ElegantOTA at `/update`. Build artefacts land in `.pio/build/<env>/firmware.bin` and `.pio/build/<env>/spiffs.bin` (use `pio run -e <env> -t buildfs` to produce the SPIFFS image).
- **Customization**: Modify the vanity screen (`data/vanity.jpg`) or the web UI templates under `data/web/` for personalized branding, then rerun `pio run -t uploadfs` (USB) or push the new `spiffs.bin` via `/update` in `Filesystem` mode.
- **Persistence**: Slideshow speed, brightness, night-mode settings are saved in NVS via the ESP32 `Preferences` API.

---

## Project Files and Notes

- **README**: Basic instructions.
- **Wiki**: Detailed guides, including:
  - Using the `.bin` file
  - Uploading files to SPIFFS
  - Preparing images
  - Creating a 3D printed case
- **.bin File**: Precompiled firmware for easy flashing.

---

## Contributions

Feel free to contribute! Fork the repo, make changes, and submit a pull request. Ideas and bug reports are also welcome.

---

## License

This project is licensed under the [MIT License](LICENSE).

---

## About

Developed by **Grey Lancaster**. Thanks to the open-source community for inspiration and support.
