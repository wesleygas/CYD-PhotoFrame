#pragma once
#include "globals.h"

String webProcessor(const String& var);
void   sendHtml(AsyncWebServerRequest* req, const char* path,
                AwsTemplateProcessor processor = nullptr);
String buildDeleteFileList();
String buildStatusRows();
void   handleFileUpload(AsyncWebServerRequest* request);
void   onWebSocketEvent(AsyncWebSocket* server, AsyncWebSocketClient* client,
                        AwsEventType type, void* arg, uint8_t* data, size_t len);
void   setupWebServer();
