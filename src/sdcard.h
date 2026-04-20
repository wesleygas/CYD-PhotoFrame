#pragma once
#include "globals.h"

bool   isImageFile(const char* name);
uint16_t scanImages();
void   loadImage(uint16_t logicalIndex);
void   decodeJpeg(const char* name);
bool   checkAndMountSDCard();
void   refreshSdSpaceCache();
void   stopSlideshow();
void   restartSlideshow();

// JPEG decoder callbacks
int    JPEGDraw(JPEGDRAW* pDraw);
void*  myOpen(const char* filename, int32_t* size);
void   myClose(void* handle);
int32_t myRead(JPEGFILE* handle, uint8_t* buffer, int32_t length);
int32_t mySeek(JPEGFILE* handle, int32_t position);

// SPIFFS JPEG callbacks (for vanity splash)
void*   spiffsOpen(const char* filename, int32_t* size);
void    spiffsClose(void* handle);
int32_t spiffsRead(JPEGFILE* handle, uint8_t* buffer, int32_t length);
int32_t spiffsSeek(JPEGFILE* handle, int32_t position);
