/* =============================================================================
 * Stacja Pogody - stacja meteorologiczna na ESP32-S3 WROOM-1 (N16R8)
 * Copyright (c) 2026 kruzio1985 - https://github.com/kruzio1985
 * Licencja: PolyForm Noncommercial License 1.0.0 (plik LICENSE)
 * Użytek niekomercyjny. Kontakt: kruzio1985@users.noreply.github.com
 * =============================================================================
 *
 * Awaryjny punkt dostępowy węzła bez sieci (STACJA_HEADLESS=1).
 *
 * Węzeł C3 normalnie pracuje wyłącznie po magistrali RS485 i nie ma ani
 * Wi-Fi, ani strony www. Gdy po wgraniu firmware'u nie może dogadać się
 * z masterem (zły baud, adres lub piny, albo master wyłączony), uruchamia
 * własny punkt dostępowy z minimalną stroną konfiguracji magistrali
 * (baud, adres, piny RX/TX/DE). Po zapisaniu ustawień węzeł restartuje się
 * i wraca do normalnej pracy po przewodzie.
 *
 * Aktywacja tylko przez flagę -DSTACJA_NODE_RECOVERY_AP=1 w platformio.ini,
 * żeby dało się nadal budować wersję w pełni "chudą" (bez Wi-Fi).
 * =============================================================================
 */
#pragma once
#include <Arduino.h>

class WebServer;   // pełna definicja tylko w recovery_ap.cpp (gdy AP włączony)

class RecoveryAp {
 public:
  void begin();              // przygotowanie - wołać po rs485.begin()
  void loop();               // wołać w każdej pętli: monitor + obsługa HTTP
  bool active() const { return active_; }

 private:
  void start();              // uruchom softAP + serwer
  void stop();               // wyłącz AP i zwolnij serwer
  void handleRoot();
  void handleSave();
  void handleStop();

  WebServer* server_ = nullptr;
  bool active_ = false;
  bool stopped_ = false;     // AP celowo wyłączony - nie startuj ponownie
  unsigned long bootMs_ = 0;
};

extern RecoveryAp recoveryAp;
