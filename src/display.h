#pragma once
#include "globals.h"

void displayQRCodeAt(const String& ip, int startX, int startY, int blockSize);
void showStatusScreen(const char* title, const char* message, bool showQR);
