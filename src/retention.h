/* =============================================================================
 * Stacja Pogody - stacja meteorologiczna na ESP32-S3 WROOM-1 (N16R8)
 * Copyright (c) 2026 kruzio1985 - https://github.com/kruzio1985
 * Licencja: PolyForm Noncommercial License 1.0.0 (plik LICENSE)
 * Użytek niekomercyjny. Kontakt: kruzio1985@users.noreply.github.com
 * =============================================================================
 */
// Retencja plików na karcie SD. Kasuje logi CSV i zdjęcia starsze niż ustawiona
// liczba dni (0 = wyłączone), żeby karta nie zapchała się z czasem.
#pragma once
#include <Arduino.h>

class SdRetention {
public:
  // Wołane cyklicznie z loop() mastera. Samo pilnuje interwału (co godzinę).
  void loop();

private:
  void runOnce();
  unsigned long lastRun_ = 0;
};

extern SdRetention retention;
