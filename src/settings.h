#pragma once
#include "globals.h"

void   loadSettings();
void   saveSettings();
void   initBacklight();
void   applyBacklight();
String makePosixTz(int offset);
String tzLabel(int offset);
void   buildShuffleOrder();
void   initTimeNTP();
