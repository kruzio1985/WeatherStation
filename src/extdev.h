/* =============================================================================
 * Stacja Pogody - urządzenia zewnętrzne odpytywane przez HTTP
 * Copyright (c) 2026 kruzio1985 - https://github.com/kruzio1985
 * Licencja: PolyForm Noncommercial License 1.0.0 (plik LICENSE)
 * Użytek niekomercyjny. Kontakt: kruzio1985@users.noreply.github.com
 * =============================================================================
 * Stacja co N sekund odpytuje skonfigurowane adresy (np. sniffer VEVOR
 * 868 MHz, bramkę BLE, dodatkową stację) i wpuszcza ich dane do kanałów.
 * Typ urządzenia mówi, jak zinterpretować JSON pod adresem:
 *   "vevor" - {"weather":{temperature_C, humidity, wind_avg_m_s, ...}}
 *   "ble"   - {"sensors":[{mac,name,temp,hum,batt,rssi}, ...]}
 * ========================================================================== */
#pragma once
#include <Arduino.h>
#include <vector>

struct ExtDevice {
  String name;
  String url;
  String type;            // "vevor" / "ble"
  int    interval = 20;   // sekundy między odczytami
  bool   ok = false;      // ostatni odczyt udany
  unsigned long lastMs = 0;   // millis() ostatniego odczytu
  String lastErr;
  int    failCount = 0;   // kolejne nieudane odczyty (do backoffu)
};

class ExtDevManager {
public:
  void begin();           // ładuje listę z NVS
  void loop();            // odpytuje urządzenia wg harmonogramu (z loop)

  int  count() const { return (int)devs_.size(); }
  const ExtDevice* at(int i) const {
    return (i >= 0 && i < (int)devs_.size()) ? &devs_[i] : nullptr;
  }
  bool add(const String& name, const String& url, const String& type, int interval);
  bool remove(int i);

private:
  std::vector<ExtDevice> devs_;
  void load();
  void save();
  void pollOne(ExtDevice& d);
};

extern ExtDevManager extdev;
