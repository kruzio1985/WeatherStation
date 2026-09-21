/* =============================================================================
 * Stacja Pogody - stacja meteorologiczna na ESP32-S3 WROOM-1 (N16R8)
 * Copyright (c) 2026 kruzio1985 - https://github.com/kruzio1985
 * Licencja: PolyForm Noncommercial License 1.0.0 (plik LICENSE)
 * Użytek niekomercyjny. Kontakt: kruzio1985@users.noreply.github.com
 * =============================================================================
 * Tabela sterowników modułowych - implementacja.
 * ========================================================================== */
#include "drv_table.h"
#include "sensors.h"
#include "syslog.h"

// --- Sterowniki modułowe (każdy w swoim pliku drv_*.cpp) ---
// Atrybut weak sprawia, że projekt zbuduje się także wtedy, gdy danego pliku
// drv_*.cpp jeszcze nie ma w projekcie (albo jest wyłączony na mniejszej
// płytce) - brak symbolu daje wskaźnik 0, pomijany w pętlach poniżej.
extern const DrvModule drvThs   __attribute__((weak));  // drv_ths.cpp   - temp/wilg./ciśnienie
extern const DrvModule drvLight __attribute__((weak));  // drv_light.cpp - światło, UV, prądy
extern const DrvModule drvAir   __attribute__((weak));  // drv_air.cpp   - jakość powietrza
extern const DrvModule drvIo    __attribute__((weak));  // drv_io.cpp    - DHT, HX711, ultradźwięki, ekspandery
extern const DrvModule drvTc    __attribute__((weak));  // drv_tc.cpp    - termopary i RTD (SPI)
extern const DrvModule drvMotion __attribute__((weak)); // drv_motion.cpp- IMU, kompasy, ToF, czujniki ruchu
extern const DrvModule drvI2s   __attribute__((weak));  // drv_i2s.cpp   - mikrofon i wzmacniacz I²S
extern const DrvModule drvGas   __attribute__((weak));  // drv_gas.cpp   - gazy MQ/MiCS, prąd ACS, liść, PAR (ADC)

static const DrvModule* MODULES[] = {
  &drvThs,
  &drvLight,
  &drvAir,
  &drvIo,
  &drvTc,
  &drvMotion,
  &drvI2s,
  &drvGas
};
static const uint16_t MODULE_SLOTS = sizeof(MODULES) / sizeof(MODULES[0]);

// -------------------------------------------------------------
//  Mostek: moduły zgłaszają identyfikatory kanałów, SensorManager
//  robi z nich pełne kanały (pulpit, wykresy, CSV, MQTT/HA).
// -------------------------------------------------------------
static void sinkFound(const char* id) { sensors.drvFound(id); }
static void sinkPublish(const char* id, float v) { sensors.drvPublish(id, v); }
static void sinkClear(const char* id) { sensors.drvClear(id); }

static const DrvSink SINK = { sinkFound, sinkPublish, sinkClear };

static inline const DrvModule* modAt(uint16_t i) {
  if (i >= MODULE_SLOTS) return nullptr;
  const DrvModule* p = MODULES[i];
  return (p && p->name) ? p : nullptr;
}

uint16_t drvModuleCount() {
  uint16_t n = 0;
  for (uint16_t i = 0; i < MODULE_SLOTS; i++) if (modAt(i)) n++;
  return n;
}

const DrvModule* drvModuleAt(uint16_t i) {
  uint16_t n = 0;
  for (uint16_t s = 0; s < MODULE_SLOTS; s++) {
    const DrvModule* p = modAt(s);
    if (!p) continue;
    if (n == i) return p;
    n++;
  }
  return nullptr;
}

void drvBeginAll() {
  uint16_t n = 0;
  for (uint16_t i = 0; i < MODULE_SLOTS; i++) {
    const DrvModule* m = modAt(i);
    if (!m || !m->begin) continue;
    m->begin(SINK);
    n++;
  }
  LOG_I("Sterowniki modułowe: %u zestawów aktywnych", (unsigned)n);
}

void drvReadAll() {
  for (uint16_t i = 0; i < MODULE_SLOTS; i++) {
    const DrvModule* m = modAt(i);
    if (m && m->read) m->read();
  }
}
