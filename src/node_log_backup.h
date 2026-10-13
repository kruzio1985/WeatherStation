/* =============================================================================
 * Stacja Pogody - dzienna kopia logów węzłów na kartę SD
 * Copyright (c) 2026 kruzio1985 - https://github.com/kruzio1985
 * Licencja: PolyForm Noncommercial License 1.0.0 (plik LICENSE)
 * =============================================================================
 * Węzły (ESP32 z ExtDev typu "aq"/"outaq", np. stacja powietrza 192.168.1.147)
 * wystawiają surowy log pod <baza>/log.txt. Raz na dobę stacja pobiera te logi
 * i dopisuje je na kartę SD do /logi/<nazwa>-<RRRR-MM-DD>.txt.
 *
 * Kopie robi raz na dobę po godzinie BACKUP_HOUR (czas lokalny), a gdy plik z
 * dzisiejszą datą już istnieje (np. po restarcie stacji), nie duplikuje go.
 * ========================================================================== */
#pragma once
#include <Arduino.h>

class NodeLogBackup {
public:
  void loop();                       // wołać w pętli; sam pilnuje raz-na-dobę
  String status() const { return status_; }
  bool   enabled() const { return enabled_; }

private:
  void   runBackup();
  bool   enabled_ = true;
  String status_ = "jeszcze nie było kopii";
  unsigned long lastCheck_ = 0;
};

extern NodeLogBackup nodeLogs;
