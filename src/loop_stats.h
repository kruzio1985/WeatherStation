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
//  Pomiar czasu etapów pętli głównej.
//
//  Obieg pętli, który trwa kilka sekund, jest groźny dla całej stacji:
//  w tym czasie nie idzie impuls 1 Hz dla zewnętrznego watchdoga, nie
//  odpowiada strona www i spóźniają się odpytywania magistrali. Zamiast
//  zgadywać, co blokuje pętlę, mierzymy każdy etap i przy wolnym obiegu
//  logujemy od razu ten najdłuższy (zakładka "Diagnostyka" pokazuje
//  maksimum z całego czasu pracy).
// =============================================================

enum LoopStage : uint8_t {
  LOOP_WWW = 0,        // web + MQTT + OTA
  LOOP_SENSORS,        // wykrywanie i obsługa czujników (Tuya, GPS, RTC, błyskawice)
  LOOP_SENSORS_READ,   // odczyt wszystkich czujników + BH1750
  LOOP_ASTRO,          // astronomia i prognoza lokalna
  LOOP_JOBS,           // analiza, alerty, retencja
  LOOP_LOG,            // zapis wiersza na kartę SD
  LOOP_RS485,          // magistrala RS485
  LOOP_RING,           // pierścień LED RGB
  LOOP_CAMERA,         // zdjęcie/timelapse
  LOOP_EXTDEV,         // urządzenia zewnętrzne po HTTP
  LOOP_DISPLAY,        // wyświetlacz TFT (render do sprite'a + wypchnięcie)
  LOOP_STAGE_COUNT
};

// Obieg dłuższy niż to (ms) trafia do logu razem z najdłuższym etapem.
#define LOOP_SLOW_MS 2000UL

void loopStatsBeginIteration();
void loopStatsRecord(LoopStage stage, uint32_t ms);
void loopStatsEndIteration();

// Ostatni etap obiegu (i miejsce w obsłudze żądania) w pamięci RTC - po
// restarcie z watchdoga pierwsza linia logu mówi, gdzie stacja stanęła.
void crashTraceMark(uint8_t stage, const char* where);
void crashTraceBootLog();

const char* loopStatsStageName(LoopStage stage);
const char* loopStatsWorstStage();
uint32_t    loopStatsWorstStageMs();
uint32_t    loopStatsWorstIterationMs();
uint32_t    loopStatsLastIterationMs();

// Mierzy czas życia obiektu - jeden etap obiegu pętli. Przy okazji znaczy
// etap w pamięci RTC (ślad dla diagnozy restartów z watchdoga).
struct LoopStageTimer {
  LoopStageTimer(LoopStage s) : stage_(s), t0_(millis()) { crashTraceMark((uint8_t)s, nullptr); }
  ~LoopStageTimer() { loopStatsRecord(stage_, (uint32_t)(millis() - t0_)); }

  LoopStage stage_;
  unsigned long t0_;
};
