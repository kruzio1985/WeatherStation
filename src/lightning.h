/* =============================================================================
 * Stacja Pogody - stacja meteorologiczna na ESP32-S3 WROOM-1 (N16R8)
 * Copyright (c) 2026 kruzio1985 - https://github.com/kruzio1985
 * Licencja: PolyForm Noncommercial License 1.0.0 (plik LICENSE)
 * Użytek niekomercyjny. Kontakt: kruzio1985@users.noreply.github.com
 * =============================================================================
 */
#pragma once
#include <Arduino.h>

// =============================================================
//  Detektor wyładowań atmosferycznych AS3935 (Franklin).
//
//  Sterownik jest napisany bez biblioteki zewnętrznej - bezpośrednio
//  na rejestrach po I2C (adres 0x03 lub 0x02). Wyładowania zgłaszane
//  są linią IRQ (domyślnie GPIO 14) i/lub odpytywaniem rejestru 0x03.
//
//  Dane o wyładowaniach trafiają do modułu prognozy
//  (forecast.addLightning), który liczy ryzyko burzy.
// =============================================================

#define AS3935_I2C_ADDR_A   0x03
#define AS3935_I2C_ADDR_B   0x02

// Rejestry (wartości potwierdzone w dokumentacji AMS/SparkFun)
#define AS3935_REG_AFE      0x00      // bity [5:1] AFE_GB (wzrost), bit0 PWD
#define AS3935_REG_PCF      0x01      // [3:0] WDTH, [6:4] NF_LEV, bit0 PWD
#define AS3935_REG_MASK     0x02      // [3:0] SPRej, [5:4] MIN_LIG_STR, [6] CLEAR_STAT, [5]? MASK_DIST
#define AS3935_REG_STAT     0x03      // [3:0] źródło przerwania, [7:6] podział LCO
#define AS3935_REG_ENERGY   0x04      // 20-bitowa energia: 0x04 LSB, 0x05, 0x06 [4:0] MSB
#define AS3935_REG_DISTANCE 0x07      // [5:0] odległość w km, 0x3F = poza zasięgiem
#define AS3935_REG_CAP      0x08      // [3:0] trymer pojemności (kalibracja anteny)
#define AS3935_REG_CALIB_TR 0x3A      // kalibracja TRCO
#define AS3935_REG_CALIB_SR 0x3B      // kalibracja SRCO
#define AS3935_REG_PRESET   0x3C      // zapis 0x96 = przywrócenie ustawień fabrycznych
#define AS3935_REG_CALIB_RC 0x3D      // zapis 0x96 = kalibracja RCO

#define AS3935_INT_NONE       0x00
#define AS3935_INT_NOISE      0x01
#define AS3935_INT_DISTURBER  0x04
#define AS3935_INT_LIGHTNING  0x08

#define AS3935_AFE_INDOOR   0x12      // AFE_GB dla pracy w pomieszczeniu
#define AS3935_AFE_OUTDOOR  0x0E      // AFE_GB dla pracy na zewnątrz

struct LightningInfo {
  bool enabled = false;
  bool present = false;       // czujnik odpowiada po I2C
  bool indoor = false;        // tryb pracy (indoor/outdoor)
  bool irqLine = false;       // czy używamy linii IRQ (GPIO)
  unsigned long strikes = 0;          // łącznie wykrytych wyładowań
  unsigned long disturbers = 0;       // łącznie zakłóceń (disturber)
  unsigned long noiseEvents = 0;      // łącznie zdarzeń szumu
  unsigned long lastStrikeMs = 0;     // millis() ostatniego wyładowania
  float lastDistanceKm = NAN;         // odległość ostatniego wyładowania
  float lastEnergy = NAN;             // energia ostatniego wyładowania
  float closestKm = NAN;              // najbliższe wyładowanie od startu
  int8_t address = -1;                // wykryty adres I2C
  uint8_t wdth = 2;
  uint8_t nfLev = 2;
  uint8_t spikeRej = 2;
  uint8_t minStrike = 0;              // 0 = najmniejsza czułość progu (MIN_LIG_STR)
  uint8_t maskDist = 1;               // 1 = maskowanie odległości (tylko burze blisko)
  uint8_t tuningCap = 0;
  unsigned long i2cErrors = 0;
};

class LightningService {
public:
  // Otwiera czujnik, jeśli pin I2C jest dostępny i czujnik odpowiada.
  // Ustawienia (tryb indoor/outdoor, czułości) bierze z konfiguracji.
  bool begin();
  void loop();

  bool enabled() const { return info_.enabled; }
  bool present() const { return info_.present; }
  const LightningInfo& info() const { return info_; }

  String json() const;

  // Zmiana ustawień z www - zapisywane w konfiguracji (klucze extra.*)
  void setIndoor(bool indoor);
  void setSensitivity(uint8_t nfLev, uint8_t wdth, uint8_t spikeRej, uint8_t minStrike);
  void setMaskDist(bool mask);
  void setTuningCap(int cap);              // -1 = automatycznie (autoTune)
  void calibrate();          // CALIB_RCO + automatyczny dobór trymera anteny
  void resetDefaults();      // PRESET_DEFAULT (0x96)
  void clearStatistics();
  float measureLcoKHz();     // pomiar częstotliwości rezonatora (kHz, NAN = brak)
  float autoTune();          // dobór trymera na 500 kHz, zwraca wybraną częstotliwość

private:
  LightningInfo info_;
  int8_t irqPin_ = -1;
  uint8_t addr_ = 0;
  unsigned long lastPollMs_ = 0;
  bool configLoaded_ = false;

  bool writeReg(uint8_t reg, uint8_t value);
  bool readReg(uint8_t reg, uint8_t& value);
  bool readRegs(uint8_t reg, uint8_t* buf, size_t len);
  void applyConfig();
  void handleInterrupt();
  void registerStrike(float distanceKm, float energy);
  uint8_t detectAddress();
};

extern LightningService lightning;
