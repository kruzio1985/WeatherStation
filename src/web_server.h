/* =============================================================================
 * Stacja Pogody - stacja meteorologiczna na ESP32-S3 WROOM-1 (N16R8)
 * Copyright (c) 2026 kruzio1985 - https://github.com/kruzio1985
 * Licencja: Stacja Pogody Non-Commercial License 1.0 (plik LICENSE)
 * Użytek niekomercyjny. Kontakt: kruzio1985@users.noreply.github.com
 * =============================================================================
 */
#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <DNSServer.h>

// Te same JSON-y, których używa strona www (zakładki "Pulpit" i
// "Diagnostyka"). Węzeł RS485 wysyła je na żądanie "status" / "diag".
#include "api_json.h"

class WebServerManager {
public:
  void begin(const IPAddress& apIP);
  void loop();

private:
  void registerRoutes();
  static String mimeType(const String& path);
};

extern WebServerManager web;
extern AsyncWebServer server;
extern DNSServer dnsServer;
