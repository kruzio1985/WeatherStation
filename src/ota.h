/* =============================================================================
 * Stacja Pogody - stacja meteorologiczna na ESP32-S3 WROOM-1 (N16R8)
 * Copyright (c) 2026 kruzio1985 - https://github.com/kruzio1985
 * Licencja: PolyForm Noncommercial License 1.0.0 (plik LICENSE)
 * Użytek niekomercyjny. Kontakt: kruzio1985@users.noreply.github.com
 * =============================================================================
 */
#pragma once
#include <Arduino.h>
#include <ESPAsyncWebServer.h>

// Ustawiane na czas aktualizacji - main.cpp wstrzymuje wtedy odczyt czujników
// i zapis logów, żeby nie mieszać zapisu do flasha z innymi operacjami.
extern volatile bool g_otaInProgress;

class OtaManager {
public:
  // ArduinoOTA (aktualizacja przez sieć, np. `pio run -e esp32s3-ota -t upload`)
  void begin();
  void loop();

  bool running() const { return running_; }
  void running(bool v) { running_ = v; }

private:
  bool running_ = false;
};

// Rejestruje endpointy www: /api/ota/firmware, /api/ota/filesystem, /api/ota/info,
// /api/ota/diag (GET, diagnostyka obrazu) i /api/ota/activate (POST, ręczna aktywacja).
void registerOtaRoutes(AsyncWebServer& server);

extern OtaManager ota;
