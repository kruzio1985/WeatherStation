/* =============================================================================
 * Stacja Pogody - stacja meteorologiczna na ESP32-S3 WROOM-1 (N16R8)
 * Copyright (c) 2026 kruzio1985 - https://github.com/kruzio1985
 * Licencja: Stacja Pogody Non-Commercial License 1.0 (plik LICENSE)
 * Użytek niekomercyjny. Kontakt: kruzio1985@users.noreply.github.com
 * =============================================================================
 */
#pragma once
#include <Arduino.h>

// =============================================================
//  Wspólny interfejs modułu sterowników.
//
//  Każdy plik drv_*.cpp wystawia dokładnie jedną strukturę DrvModule
//  (patrz np. drv_ths.cpp, drv_light.cpp) i nic poza tym. Dzięki temu
//  dopisanie obsługi kolejnych czujników nie wymaga zmian w
//  sensors_extra.cpp - wystarczy dodać nowy moduł do tabeli MODULES
//  w src/drv_table.cpp.
//
//  Zasada wykrywania sprzętu (taka sama jak w resztę stacji):
//  kanał pokazuje dane tylko wtedy, gdy sterownik potwierdził układ
//  na magistrali (WHO AM I / odpowiedź na komendę + CRC) i odczyt
//  się udał. Publikacja robi się przez sink - on ustawia flagi
//  "present"/"detected" i zapisuje wartość do kanału.
//
//  Kod jest jeden dla obu wersji firmware (bin 1 MASTER i bin 2 WĘZEŁ),
//  więc każdy sterownik dopisany tutaj działa na obu ESP.
// =============================================================

// Jedna pozycja pomiarowa czujnika (kanał: pulpit, wykresy, CSV, MQTT/HA)
struct ChanDef {
  const char* id;       // identyfikator kanału (unikalny, bez spacji)
  const char* name;     // nazwa na www (PL, tłumaczenie w i18n.js)
  const char* unit;     // jednostka ("" gdy bezwymiarowe)
  uint8_t     dec;      // miejsca po przecinku na pulpicie
  const char* zone;     // "in" (wewnątrz) / "out" (na zewnątrz)
  const char* haClass;  // klasa encji HA ("" gdy brak)
  const char* haUnit;   // jednostka encji HA
  const char* haIcon;   // ikona mdi:...
};

// Kanał publikacji wstrzykiwany z sensors_extra.cpp
struct DrvSink {
  void (*found)(const char* id);              // sprzęt wykryty (bez wartości)
  void (*publish)(const char* id, float v);   // wartość + potwierdzenie obecności
  void (*clear)(const char* id);              // brak odczytu / czujnik zniknął
};

struct DrvModule {
  const char* name;                    // nazwa modułu do logu
  const ChanDef* chans;                // tabela kanałów (może być nullptr)
  uint16_t chanCount;
  void (*begin)(const DrvSink& sink);  // wykrywanie sprzętu, zapamiętanie sink
  void (*read)(void);                  // odczyt w cyklu (bez delay > 20 ms)
};

// Zakres adresów pomocniczy dla sterowników I2C
#define DRV_I2C_ADDR_MIN 0x08
#define DRV_I2C_ADDR_MAX 0x77
