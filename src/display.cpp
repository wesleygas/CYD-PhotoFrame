#include "display.h"

// Word-wrap a block of text at (x,y) within maxWidth. Returns the y below the last line.
static int drawWrapped(const char* text, int x, int y, int maxWidth,
                       int textSize, int maxLines, uint16_t color) {
  tft.setTextColor(color);
  tft.setTextSize(textSize);
  int charW    = 6 * textSize;
  int lineH    = 8 * textSize + 2;
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

void displayQRCodeAt(const String& ip, int startX, int startY, int blockSize) {
  String url = "http://" + ip;
  QRCode qrcode;
  uint8_t qrcodeData[qrcode_getBufferSize(4)];
  qrcode_initText(&qrcode, qrcodeData, 4, ECC_MEDIUM, url.c_str());
  int qrSize = qrcode.size;
  tft.fillRect(startX - 2, startY - 2, qrSize * blockSize + 4, qrSize * blockSize + 4, TFT_WHITE);
  for (int y = 0; y < qrSize; y++) {
    for (int x = 0; x < qrSize; x++) {
      tft.fillRect(startX + x * blockSize, startY + y * blockSize, blockSize, blockSize,
                   qrcode_getModule(&qrcode, x, y) ? TFT_BLACK : TFT_WHITE);
    }
  }
}

// Unified info / error screen.
// Layout (320×240, rotation 3):
//   left column: red title banner, yellow-bordered message panel, IP info in cyan/green
//   right column: QR code when showQR && IP is known
void showStatusScreen(const char* title, const char* message, bool showQR) {
  inInfoScreen = true;
  errorScreenAdvancesIndex = false;
  tft.fillScreen(TFT_BLACK);
  bool haveIp = currentIp.length() > 0;
  bool drawQr = showQR && haveIp;

  // Full-width red title banner
  const int bannerH = 26;
  tft.fillRect(0, 0, 320, bannerH, TFT_RED);
  tft.setTextColor(TFT_WHITE, TFT_RED);
  tft.setTextSize(2);
  tft.setCursor(8, 6);
  tft.print(title);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);

  // Yellow-bordered message panel
  const int panelY = bannerH + 6;
  const int panelH = 110;
  const int qrSize = 99;
  const int qrX    = 215;
  const int qrY    = panelY + 2;
  const int panelX = 4;
  const int panelW = drawQr ? (qrX - panelX - 8) : (320 - panelX * 2);

  tft.drawRect(panelX,     panelY,     panelW,     panelH,     TFT_YELLOW);
  tft.drawRect(panelX + 1, panelY + 1, panelW - 2, panelH - 2, TFT_YELLOW);

  const int padX  = panelX + 8;
  const int padY  = panelY + 8;
  const int textW = panelW - 16;

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

  // IP / mDNS strip at the bottom
  int infoY = panelY + panelH + 8;
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

  if (drawQr) displayQRCodeAt(currentIp, qrX, qrY, 3);

  Serial.printf("[STATUS] %s -- %s\n", title, message);
}
