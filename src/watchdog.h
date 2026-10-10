/* =============================================================================
 * Stacja Pogody - stacja meteorologiczna na ESP32-S3 WROOM-1 (N16R8)
 * Copyright (c) 2026 kruzio1985 - https://github.com/kruzio1985
 * Licencja: PolyForm Noncommercial License 1.0.0 (plik LICENSE)
 * Użytek niekomercyjny. Kontakt: kruzio1985@users.noreply.github.com
 * =============================================================================
 */
#pragma once
#include <Arduino.h>

// Dwa mechanizmy chroniące stację przed wielodniową pracą i twardym
// zawieszeniem:
//  1) RESTART DOBOWY - raz na dobę, w oknie minut 0-9 wybranej godziny,
//     czyści fragmentację sterty i stan stosu Wi-Fi. Ustawiany na stronie
//     www (checkbox + lista godzin), zapis w NVS (sekcja "extra").
//  2) IMPULS ŻYCIA (~1 Hz) na wolnym GPIO - do zewnętrznego modułu
//     watchdog z przekaźnikiem, który odetnie zasilanie, gdy impulsy
//     znikną (twarde zawieszenie, przy którym program nie zrestartuje
//     się sam).
class WatchdogManager {
public:
  void begin();
  void heartbeat();          // impuls życia - wołać na POCZĄTKU każdego obiegu pętli
  void maybeDailyRestart();  // kontrola restartu dobowego - wołać w normalnym obiegu

  // Ustawienia dla /api/watchdog i /api/status
  bool    autoRestartEnabled();
  uint8_t autoRestartHour();        // 0..23 (domyślnie 4)
  String  autoRestartInfo();        // opis stanu dla /api/status
  bool    heartbeatEnabled();       // włączone ORAZ pin przypisany
  int     heartbeatPin();
  bool    heartbeatActive();        // pin pulsuje (do diagnostyki)

  String  json();                   // GET /api/watchdog
  bool    applyJson(const char* json, size_t len);   // POST /api/watchdog

private:
  void maybeRestartNow();

  unsigned long lastToggle_ = 0;
  unsigned long lastDailyCheck_ = 0;
  int  pin_ = -1;
  bool heartbeatOn_ = false;      // zapamiętany stan przełącznika (bez czytania NVS co obieg)
  bool level_ = false;
  bool pulsing_ = false;
  bool restartDoneThisBoot_ = false;   // strażnik: maks. jeden restart na uruchomienie
};

extern WatchdogManager watchdog;
