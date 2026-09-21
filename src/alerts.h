/* =============================================================================
 * Stacja Pogody - stacja meteorologiczna na ESP32-S3 WROOM-1 (N16R8)
 * Copyright (c) 2026 kruzio1985 - https://github.com/kruzio1985
 * Licencja: PolyForm Noncommercial License 1.0.0 (plik LICENSE)
 * Użytek niekomercyjny. Kontakt: kruzio1985@users.noreply.github.com
 * =============================================================================
 */
// Alerty pogodowe: progi dla wybranych kanałów. Gdy wartość wyjdzie poza
// [min, max], stacja publikuje alert do Home Assistant (MQTT binary_sensor)
// i dopisuje wpis do logu systemowego. Progi konfiguruje się na stronie www
// (zakładka "Alerty") i trzyma w sekcji "extra" ustawień.
#pragma once
#include <Arduino.h>

// Liczba kanałów objętych alertami (lista w alerts.cpp).
#define ALERT_METRIC_COUNT 11

class AlertManager {
public:
  void begin();
  // Wołane cyklicznie z loop() - sprawdza progi i publikuje zmiany.
  void loop();

  // Stan + konfiguracja w JSON (dla /api/alerts).
  String json();
  // Zapis progów z formularza www. Zwraca true, gdy zapis się powiódł.
  bool applyJson(const char* json, size_t len);

  bool enabled();
  void setEnabled(bool on);

private:
  struct State {
    bool active = false;
    unsigned long activeAt = 0;   // moment wejścia w stan alarmu
    unsigned long lastNotify = 0; // ostatnia publikacja/przypomnienie
  };

  State states_[ALERT_METRIC_COUNT];
  unsigned long lastCheck_ = 0;
  unsigned long lastAnnounce_ = 0;
};

extern AlertManager alerts;
