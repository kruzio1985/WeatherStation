/* =============================================================================
 * Stacja Pogody - stacja meteorologiczna na ESP32-S3 WROOM-1 (N16R8)
 * Copyright (c) 2026 kruzio1985 - https://github.com/kruzio1985
 * Licencja: PolyForm Noncommercial License 1.0.0 (plik LICENSE)
 * Użytek niekomercyjny. Kontakt: kruzio1985@users.noreply.github.com
 * =============================================================================
 */
#include "loop_stats.h"
#include "syslog.h"
#include <string.h>

static const char* const STAGE_NAMES[LOOP_STAGE_COUNT] = {
  "www/MQTT/OTA",
  "obsługa czujników",
  "odczyt czujników",
  "astronomia i prognoza",
  "analiza, alerty, retencja",
  "zapis logu na SD",
  "magistrala RS485",
  "pierścień LED",
  "kamera",
  "urządzenia zewnętrzne",
  "wyświetlacz TFT",
};

// Czas każdego etapu w bieżącym obiegu (etap może wystąpić więcej niż raz,
// dlatego sumujemy) oraz maksimum od uruchomienia.
static uint32_t s_stageLast[LOOP_STAGE_COUNT];
static uint32_t s_stageMax[LOOP_STAGE_COUNT];
static unsigned long s_iterStart = 0;
static uint32_t s_iterLast = 0;
static uint32_t s_iterMax = 0;

static uint8_t worstStage(const uint32_t* times) {
  uint8_t worst = 0;
  for (uint8_t i = 1; i < LOOP_STAGE_COUNT; i++) {
    if (times[i] > times[worst]) worst = i;
  }
  return worst;
}

void loopStatsBeginIteration() {
  s_iterStart = millis();
  for (uint8_t i = 0; i < LOOP_STAGE_COUNT; i++) s_stageLast[i] = 0;
}

void loopStatsRecord(LoopStage stage, uint32_t ms) {
  if (stage >= LOOP_STAGE_COUNT) return;
  s_stageLast[stage] += ms;
  if (s_stageLast[stage] > s_stageMax[stage]) s_stageMax[stage] = s_stageLast[stage];
}

// ------------------------------------------------------------
//  Ślad przed resetem (pamięć RTC przeżywa restart z watchdoga)
//
//  Reset "watchdog zadania" nie zostawia w logu miejsca, w którym stacja
//  stanęła, a bufor logów jest kasowany przy starcie. Dlatego ostatni etap
//  obiegu (i opcjonalnie miejsce w obsłudze żądania www) zapisujemy w pamięci
//  RTC - po restarcie widać je w pierwszej linii logu.
// ------------------------------------------------------------
RTC_NOINIT_ATTR static uint32_t s_rtcMagic;
RTC_NOINIT_ATTR static uint32_t s_rtcStage;
RTC_NOINIT_ATTR static uint32_t s_rtcMs;
RTC_NOINIT_ATTR static char     s_rtcWhere[40];
RTC_NOINIT_ATTR static uint32_t s_rtcWhereMs;
static const uint32_t RTC_MAGIC = 0x53505254u;   // "SPRT"

void crashTraceMark(uint8_t stage, const char* where) {
  s_rtcMagic = RTC_MAGIC;
  if (where) {
    strncpy(s_rtcWhere, where, sizeof(s_rtcWhere) - 1);
    s_rtcWhere[sizeof(s_rtcWhere) - 1] = 0;
    s_rtcWhereMs = millis();
    return;
  }
  s_rtcStage = stage;
  s_rtcMs    = millis();
}

void crashTraceBootLog() {
  if (s_rtcMagic != RTC_MAGIC) return;
  // Tylko po restarcie, którego nikt nie zamawiał - zwykłe restarty (OTA,
  // restart dobowy, przycisk) mają swoje linie w logu i nie są awarią.
  const esp_reset_reason_t rr = esp_reset_reason();
  const bool unexpected = (rr == ESP_RST_TASK_WDT || rr == ESP_RST_INT_WDT ||
                           rr == ESP_RST_WDT || rr == ESP_RST_PANIC);
  if (unexpected) {
    LOG_W("Przed resetem: etap %s (millis %lu), ostatnie żądanie: %s (millis %lu)",
          loopStatsStageName((LoopStage)(s_rtcStage < LOOP_STAGE_COUNT ? s_rtcStage
                                                                       : LOOP_WWW)),
          (unsigned long)s_rtcMs, s_rtcWhere[0] ? s_rtcWhere : "-",
          (unsigned long)s_rtcWhereMs);
  }
  s_rtcMagic = 0;
}

void loopStatsEndIteration() {
  const uint32_t total = (uint32_t)(millis() - s_iterStart);
  s_iterLast = total;
  if (total > s_iterMax) s_iterMax = total;

  if (total < LOOP_SLOW_MS) return;
  const uint8_t worst = worstStage(s_stageLast);
  LOG_W("Pętla główna: obieg %lu ms, najdłuższy etap: %s %lu ms",
        (unsigned long)total, STAGE_NAMES[worst], (unsigned long)s_stageLast[worst]);
}

const char* loopStatsStageName(LoopStage stage) {
  return (stage < LOOP_STAGE_COUNT) ? STAGE_NAMES[stage] : "?";
}

const char* loopStatsWorstStage() { return STAGE_NAMES[worstStage(s_stageMax)]; }

uint32_t loopStatsWorstStageMs() { return s_stageMax[worstStage(s_stageMax)]; }

uint32_t loopStatsWorstIterationMs() { return s_iterMax; }

uint32_t loopStatsLastIterationMs() { return s_iterLast; }
