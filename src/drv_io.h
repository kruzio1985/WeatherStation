/* =============================================================================
 * Stacja Pogody - stacja meteorologiczna na ESP32-S3 WROOM-1 (N16R8)
 * Copyright (c) 2026 kruzio1985 - https://github.com/kruzio1985
 * Licencja: Stacja Pogody Non-Commercial License 1.0 (plik LICENSE)
 * Użytek niekomercyjny. Kontakt: kruzio1985@users.noreply.github.com
 * =============================================================================
 */
#pragma once
#include <Arduino.h>
#include "drv_mod.h"

// =============================================================
//  Czujniki na pojedynczych liniach GPIO oraz ekspandery I2C.
//
//  Wszystko napisane ręcznie (bit-bang, bez DHT.h/HX711.h i bez
//  innych bibliotek zewnętrznych), żeby nie zajmować flasha:
//    * DHT11 / DHT22 / AM2302 / AM2320 - jeden przewód danych,
//      protokół DHT (start 18 ms / 1 ms, 40 bitów, suma kontrolna),
//    * HX711 - 24-bitowy przetwornik tensometru (waga deszczomierza
//      wagowego, tensjometr gleby, siła) na liniach DT/SCK,
//    * HC-SR04 / JSN-SR04T / A02YYUW - ultradźwięki TRIG/ECHO
//      (odległość, poziom wody/śniegu; moduły zgodne z HC-SR04),
//    * MCP23017 oraz PCF8574 / PCF8575 - ekspandery wejść na wspólnej
//      magistrali I2C z linią INT; publikowany jest wyłącznie stan
//      ośmiu wejść i licznik zmian (nie duplikujemy cudzych kanałów).
//
//  Każdy czujnik działa niezależnie: brak przypisania pinu (rola -1
//  w mapie pinów) albo brak odpowiedzi sprzętu = kanały zostają
//  "brak", bez błędu. Przy starcie potwierdzamy sprzęt (odpowiedź na
//  sygnał startowy DHT, niezerowe zliczenia HX711, poprawny pomiar
//  2..400 cm, ACK I2C ekspandera), więc kanał "wykryty" znaczy
//  naprawdę podłączony czujnik, a nie samo przypisanie pinu.
//
//  Kalibrację kanału (offset) robi SensorManager - ten moduł
//  publikuje wartości surowe. Współczynniki skalujące, których nie da
//  się wyrazić offsetem, siedzą w pamięci ustawień (zakładka
//  Kalibracja, sekcja "extra"):
//    * "hx_g_cnt"    - gramy na jednostkę zliczeń HX711 (kanał hx_w),
//    * "us_empty_cm" - odległość dla 0 % poziomu (kanał us_lvl),
//    * "us_full_cm"  - odległość dla 100 % poziomu (kanał us_lvl).
//  Bez nich publikowane są tylko wartości surowe (hx_raw, us_dist),
//  a kanały przeliczone (hx_w, us_lvl) czekają na kalibrację - dzięki
//  temu na pulpicie nie ma liczb o zmyślonej skali.
//
//  Piny (mapa pinów, grupa "Czujniki cyfrowe"): "dht", "hx711_dt",
//  "hx711_sck", "us_trig", "us_echo", "exp_int"; ekspandery siedzą na
//  wspólnej magistrali "i2c_sda"/"i2c_scl". Deszczomierz "rain"
//  i anemometr "anem" mają własną obsługę - tutaj ich nie ruszamy.
// =============================================================

// ---------- DHT11 / DHT22 / AM2302 / AM2320 (jeden przewód) ----------
enum DhtKind : uint8_t {
  DHT_NONE = 0,
  DHT_11,       // całkowite % i °C, 2 s odstępu
  DHT_22,       // 0,1 % i 0,1 °C, 1 s odstępu (AM2302, AM2320)
};

class DhtGpio {
 public:
  bool begin(int pin);                     // wykrycie: poprawna odpowiedź na start
  bool read(float& tempC, float& rh);      // jeden odczyt protokołu DHT (~5 ms)
  bool due(uint32_t now);                  // czy minął minimalny odstęp (zajmuje termin)
  bool ok() const { return pin_ >= 0 && kind_ != DHT_NONE; }
  DhtKind kind() const { return kind_; }
  const char* kindName() const;
 private:
  bool readRaw(uint8_t* data, uint8_t startMs);   // 5 bajtów: RH, T, suma kontrolna

  int pin_ = -1;
  DhtKind kind_ = DHT_NONE;
  uint32_t lastMs_ = 0;
  uint32_t intervalMs_ = 2000;
};

// ---------- HX711 (24-bitowy przetwornik tensometru) ----------
class Hx711 {
 public:
  bool begin(int dtPin, int sckPin);       // wykrycie: 2 odczyty, choć jeden niezerowy
  bool ready() const;                      // DT w stanie niskim = dane gotowe
  bool read(int32_t& raw);                 // 24 bity + impuls wzmocnienia 128
  bool ok() const { return ok_; }
 private:
  int dt_ = -1;
  int sck_ = -1;
  bool ok_ = false;
};

// ---------- HC-SR04 / JSN-SR04T / A02YYUW (TRIG/ECHO) ----------
class UltraSonic {
 public:
  bool begin(int trigPin, int echoPin);    // wykrycie: pomiar w zakresie 2..400 cm
  bool read(float& cm);                    // jeden pomiar (pulseIn z limitem czasu)
  bool ok() const { return ok_; }
 private:
  int trig_ = -1;
  int echo_ = -1;
  bool ok_ = false;
};

// ---------- Ekspandery wejść na I2C ----------
enum IoExpKind : uint8_t {
  IOEXP_NONE = 0,
  IOEXP_MCP23017,   // 16 wejść z rejestrami (używamy pierwszych ośmiu)
  IOEXP_PCF857X,    // PCF8574 / PCF8575 - port bez rejestrów
};

class IoExpander {
 public:
  bool begin(int intPin);                  // szuka 0x20..0x27, ustawia wejścia + INT
  bool readInputs(uint8_t& bits);          // stan ośmiu wejść (P0..P7)
  bool tookInterrupt();                    // czy linia INT zgłosiła zmianę (i skasuj flagę)
  bool ok() const { return kind_ != IOEXP_NONE; }
  IoExpKind kind() const { return kind_; }
  const char* kindName() const;
  uint8_t addr() const { return addr_; }
 private:
  uint8_t addr_ = 0;
  IoExpKind kind_ = IOEXP_NONE;
  int intPin_ = -1;
};

// Sterownik modułowy (rejestracja w src/drv_table.cpp po stronie projektu)
extern const DrvModule drvIo;
