/* =============================================================================
 * Stacja Pogody - stacja meteorologiczna na ESP32-S3 WROOM-1 (N16R8)
 * Copyright (c) 2026 kruzio1985 - https://github.com/kruzio1985
 * Licencja: PolyForm Noncommercial License 1.0.0 (plik LICENSE)
 * Użytek niekomercyjny. Kontakt: kruzio1985@users.noreply.github.com
 * =============================================================================
 */
#pragma once
#include <Arduino.h>
#include <Wire.h>
#include <ArduinoJson.h>

// =============================================================
//  Zegar czasu rzeczywistego (RTC) na magistrali I2C.
//
//  Obsługiwane układy (wszystkie podają czas w BCD):
//    - DS3231 (0x68) - termometr + flaga OSF, zalecany dla tej stacji,
//    - DS1307 (0x68) - bez termometru, bit CH zatrzymania zegara,
//    - PCF8563 (0x51) - flaga VL (niskie napięcie podtrzymania).
//
//  W układzie zapisujemy czas LOKALNY (razem ze strefą ustawioną w stacji).
//  Dzięki temu odczyt i zapis są zwykłym mktime()/localtime_r(), a zmiana
//  czasu letni/zimowy jest dopisywana automatycznie, bo po synchronizacji
//  NTP/GPS czas systemowy trafia do RTC (patrz loop()).
//
//  RTC jest tylko DODATKIEM do czasu z sieci: gdy stacja straci Wi-Fi,
//  po restarcie od razu ma poprawną godzinę i nie zaczyna od 1970 roku.
// =============================================================

enum RtcChip : uint8_t {
  RTC_NONE    = 0,   // nic nie odpowiedziało na magistrali
  RTC_DS3231  = 1,   // DS3231 / DS3231SN
  RTC_DS1307  = 2,   // DS1307
  RTC_PCF8563 = 3    // PCF8563 / PCF8563T
};

class RtcDriver {
 public:
  void begin();                       // wykrycie układu + korekta zegara systemowego
  void loop();                        // okresowa synchronizacja RTC <-> czas systemowy
  void reload();                      // po zmianie ustawień (typ/magistrala/włączony)

  bool    present() const { return chip_ != RTC_NONE; }
  bool    enabled() const { return enabled_; }
  uint8_t chip() const { return chip_; }
  uint8_t address() const { return addr_; }
  String  chipName() const;

  bool  readTime(struct tm& out);           // czas lokalny z RTC
  bool  writeTime(const struct tm& t);      // zapis czasu lokalnego do RTC
  bool  batteryLost();                      // utrata podtrzymania (OSF / VL / CH)
  bool  clearBatteryFlag();
  bool  hasTemperature() const { return chip_ == RTC_DS3231; }
  float temperature();                      // DS3231: termometr układu (NAN gdy brak)

  bool  syncSystemFromRtc();                // RTC -> zegar systemowy
  bool  syncRtcFromSystem();                // zegar systemowy -> RTC
  int   driftSeconds();                     // RTC minus system (sekundy)
  bool  setTimeFromEpoch(time_t epoch);     // epoch -> RTC (czas lokalny)
  bool  setTimeFromString(const String& s); // "RRRR-MM-DD GG:MM(:SS)" lub ISO
  bool  ntpSynced() const;

  void  toJson(JsonObject o);

 private:
  uint8_t       chip_ = RTC_NONE;
  uint8_t       addr_ = 0;
  bool          enabled_ = false;
  bool          bus2Init_ = false;
  unsigned long lastSyncMs_ = 0;

  void     detect();
  TwoWire* busPtr();
  bool     probe(uint8_t a);
  bool     readRegs(uint8_t reg, uint8_t* buf, size_t len);
  bool     writeRegs(uint8_t reg, const uint8_t* buf, size_t len);
  bool     readRaw(struct tm& t, bool& battLost);
  bool     writeRaw(const struct tm& t);
};

extern RtcDriver rtc;

// Uzbrojenie SNTP wg ustawień stacji (strefa + serwer NTP). Jedno miejsce
// dla startu i dla ręcznej synchronizacji z zakładki "Zegar RTC".
void rtcStartNtp();
