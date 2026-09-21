/* =============================================================================
 * Stacja Pogody - stacja meteorologiczna na ESP32-S3 WROOM-1 (N16R8)
 * Copyright (c) 2026 kruzio1985 - https://github.com/kruzio1985
 * Licencja: Stacja Pogody Non-Commercial License 1.0 (plik LICENSE)
 * Użytek niekomercyjny. Kontakt: kruzio1985@users.noreply.github.com
 * =============================================================================
 */
#pragma once
#include <Arduino.h>
#include "config.h"

// =============================================================
//  Wysyłka pomiarów do zewnętrznych serwisów pogodowych
//
//  Stacja może jednocześnie publikować dane w MQTT (Home Assistant)
//  i wysyłać je do serwisów zewnętrznych:
//    - Weather Underground (PWS),
//    - PWSWeather,
//    - Windy (PWS API v2),
//    - OpenWeather (Weather Stations API 3.0),
//    - ThingSpeak,
//    - dowolny własny adres z szablonem parametrów.
//
//  Wysyłka działa w osobnym zadaniu (task) z własnym stosem 12 kB, bo
//  połączenia HTTPS wymagają dużo pamięci stosu - nie chcemy ryzykować
//  przepełnienia stosu głównej pętli stacji.
// =============================================================
class WeatherServices {
public:
  void begin();

  // Wysyłka natychmiastowa (przycisk "Wyślij teraz" na stronie www)
  void requestNow() { requestNow_ = true; }

  // Stan ostatniej wysyłki dla strony www i diagnostyki (JSON)
  String statusJson();

  // Wynik ostatniej wysyłki do jednego serwisu (używane w Diagnostyce)
  struct Snapshot {
    bool ok = false;
    bool skipped = true;
    int  code = 0;
    String info;
    unsigned long at = 0;
  };
  Snapshot lastResult(uint8_t idx);

private:
  struct Result {
    bool ok = false;
    bool skipped = true;      // serwis nieskonfigurowany / wyłączony
    int  code = 0;
    String info;
    String url;               // bez danych logowania (podgląd na stronie)
    unsigned long at = 0;     // millis() ostatniej próby
  };

  static void taskEntry(void* arg);
  void run();
  void sendAll(bool forced);
  bool sendOne(uint8_t idx, const String& url, const String& body, Result& res);
  bool sendCwop(uint8_t idx, const String& id, const String& pass,
                const String& packet, Result& res);

  SemaphoreHandle_t mutex_ = nullptr;
  Result results_[SVC_COUNT];
  volatile bool requestNow_ = false;
  unsigned long lastSend_ = 0;
};

extern WeatherServices weatherServices;
