#pragma once
#include <ESPAsyncWebServer.h>

void simExpiryGetController(AsyncWebServerRequest* request);
void simExpiryPostController(AsyncWebServerRequest* request, uint8_t* data, size_t len,
                             size_t index, size_t total);
void simExpiryDeleteController(AsyncWebServerRequest* request);
