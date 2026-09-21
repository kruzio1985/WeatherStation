/* =============================================================================
 * Stacja Pogody - stacja meteorologiczna na ESP32-S3 WROOM-1 (N16R8)
 * Copyright (c) 2026 kruzio1985 - https://github.com/kruzio1985
 * Licencja: PolyForm Noncommercial License 1.0.0 (plik LICENSE)
 * Użytek niekomercyjny. Kontakt: kruzio1985@users.noreply.github.com
 * =============================================================================
 */
#include "lightning.h"
#include "config.h"
#include "pinmap.h"
#include "forecast.h"
#include "syslog.h"
#include "i2c_util.h"
#include <Wire.h>
#include <math.h>

// Pomiar LCO (trymer anteny AS3935) korzysta ze sprzętowego licznika impulsów
// PCNT. Na płytkach, dla których ten sterownik nie jest dołączony do
// prekompilowanego SDK (np. ESP32-C3 w rdzeniu 2.0.17), impulsy liczymy
// przerwaniem GPIO - wynik jest ten sam, tylko mniej dokładny.
#ifndef STACJA_LIGHTNING_NO_PCNT
#include <driver/pcnt.h>
#define LIGHTNING_PCNT_AVAILABLE 1
#else
#define LIGHTNING_PCNT_AVAILABLE 0
#endif

LightningService lightning;

#if !LIGHTNING_PCNT_AVAILABLE
// Licznik impulsów LCO bez sterownika PCNT (patrz measureLcoKHz).
static volatile uint32_t sLcoCount = 0;
static void IRAM_ATTR lcoIsr() { sLcoCount++; }
#endif

// --- klucze ustawień w konfiguracji (obiekt "extra") ---
#define K_INDOOR   "as3935_indoor"
#define K_WDTH     "as3935_wdth"
#define K_NFLEV    "as3935_nflev"
#define K_SPIKE    "as3935_spike"
#define K_MINSTR   "as3935_minstr"
#define K_MASKDIST "as3935_maskdist"
#define K_TUNCAP   "as3935_tuncap"

static const uint8_t AFE_MASK   = 0x3E;   // rej. 0x00: AFE_GB [5:1]
static const uint8_t PWD_BIT    = 0x01;   // rej. 0x00/0x01: power down
static const uint8_t NF_MASK    = 0x70;   // rej. 0x01: NF_LEV [6:4]
static const uint8_t WDTH_MASK  = 0x0F;   // rej. 0x01: WDTH [3:0]
static const uint8_t STRIKE_MASK = 0x30;  // rej. 0x02: MIN_LIG_STR [5:4]
static const uint8_t SREJ_MASK  = 0x0F;   // rej. 0x02: SPIKE_REJ [3:0]
static const uint8_t CLSTAT_BIT = 0x40;   // rej. 0x02: CLEAR_STAT
static const uint8_t MASKDIST_BIT = 0x20;// rej. 0x03: MASK_DIST
static const uint8_t INT_MASK   = 0x0F;   // rej. 0x03: źródło przerwania
static const uint8_t LCO_DIV_MASK = 0xC0; // rej. 0x03: LCO_FDIV [7:6]
static const uint8_t DISP_LCO_BIT = 0x80; // rej. 0x08: LCO na pin IRQ
static const uint8_t TUNCAP_MASK  = 0x0F; // rej. 0x08: trymer [3:0]

static const uint8_t DIRECT_CMD = 0x96;   // komenda bezpośrednia (preset/kalibracja)

bool LightningService::writeReg(uint8_t reg, uint8_t value) {
  if (!addr_) return false;
  if (!i2cuWriteReg(addr_, reg, value)) {
    info_.i2cErrors++;
    return false;
  }
  return true;
}

bool LightningService::readReg(uint8_t reg, uint8_t& value) {
  if (!addr_) return false;
  if (!i2cuReadReg(addr_, reg, &value, 1)) {
    info_.i2cErrors++;
    return false;
  }
  return true;
}

bool LightningService::readRegs(uint8_t reg, uint8_t* buf, size_t len) {
  if (!addr_) return false;
  if (!i2cuReadReg(addr_, reg, buf, len)) {
    info_.i2cErrors++;
    return false;
  }
  return true;
}

// Zapis tylko wybranych bitów rejestru (bez psucia pozostałych ustawień)
static bool modifyReg(uint8_t addr, uint8_t reg, uint8_t mask, uint8_t value) {
  uint8_t cur = 0;
  if (!i2cuReadReg(addr, reg, &cur, 1)) return false;
  cur = (uint8_t)((cur & ~mask) | (value & mask));
  return i2cuWriteReg(addr, reg, cur);
}

uint8_t LightningService::detectAddress() {
  const uint8_t addrs[2] = { AS3935_I2C_ADDR_A, AS3935_I2C_ADDR_B };
  for (uint8_t a : addrs) {
    if (!i2cuPresent(a)) continue;
    // Rejestr 0x00 po resecie ma AFE_GB = 0x12 lub 0x0E, a rejestr 0x07
    // odległość <= 0x3F - to wystarcza, żeby odróżnić AS3935 od innych
    // układów odpowiadających pod tym adresem.
    uint8_t r0 = 0, r7 = 0;
    if (!i2cuReadReg(a, AS3935_REG_AFE, &r0, 1)) continue;
    if (!i2cuReadReg(a, AS3935_REG_DISTANCE, &r7, 1)) continue;
    if ((r7 & 0xC0) != 0) continue;                // bity 7:6 muszą być zerami
    uint8_t afe = (uint8_t)((r0 & AFE_MASK) >> 1);
    if (afe == AS3935_AFE_INDOOR || afe == AS3935_AFE_OUTDOOR || afe == 0) return a;
  }
  return 0;
}

bool LightningService::begin() {
  irqPin_ = pinMap.pin("as3935_irq");
  info_.irqLine = irqPin_ >= 0;
  if (irqPin_ >= 0) pinMode(irqPin_, INPUT);

  addr_ = detectAddress();
  if (!addr_) {
    info_.enabled = false;
    info_.present = false;
    LOG_I("AS3935: nie wykryto czujnika wyładowań (I2C)");
    return false;
  }

  info_.address = (int8_t)addr_;
  info_.present = true;
  info_.enabled = true;

  if (!writeReg(AS3935_REG_PRESET, DIRECT_CMD)) {   // przywrócenie ustawień fabrycznych
    info_.present = false;
    info_.enabled = false;
    LOG_W("AS3935: brak odpowiedzi po I2C (adres 0x%02X)", addr_);
    return false;
  }
  delay(3);
  writeReg(AS3935_REG_CALIB_RC, DIRECT_CMD);        // kalibracja RCO
  delay(3);

  applyConfig();

  LOG_I("AS3935: wykryty pod adresem 0x%02X, tryb %s, IRQ %s", addr_,
        info_.indoor ? "wewnętrzny" : "zewnętrzny",
        info_.irqLine ? "podłączone" : "brak pinu");
  return true;
}

void LightningService::applyConfig() {
  if (!addr_) return;

  info_.indoor = config.extraF(K_INDOOR, 0.0f) != 0.0f;
  info_.wdth = (uint8_t)constrain((int)config.extraF(K_WDTH, 2.0f), 0, 15);
  info_.nfLev = (uint8_t)constrain((int)config.extraF(K_NFLEV, 2.0f), 0, 7);
  info_.spikeRej = (uint8_t)constrain((int)config.extraF(K_SPIKE, 2.0f), 0, 15);
  info_.minStrike = (uint8_t)constrain((int)config.extraF(K_MINSTR, 0.0f), 0, 3);
  info_.maskDist = config.extraF(K_MASKDIST, 1.0f) != 0.0f ? 1 : 0;

  modifyReg(addr_, AS3935_REG_AFE, (uint8_t)(AFE_MASK | PWD_BIT),   // wyjście z power-down
            (uint8_t)((info_.indoor ? AS3935_AFE_INDOOR : AS3935_AFE_OUTDOOR) << 1));

  uint8_t pcf = (uint8_t)((info_.nfLev << 4) | (info_.wdth & WDTH_MASK));
  modifyReg(addr_, AS3935_REG_PCF, (uint8_t)(NF_MASK | WDTH_MASK), pcf);

  modifyReg(addr_, AS3935_REG_MASK, (uint8_t)(STRIKE_MASK | SREJ_MASK),
            (uint8_t)((info_.minStrike << 4) | (info_.spikeRej & SREJ_MASK)));

  modifyReg(addr_, AS3935_REG_STAT, (uint8_t)(LCO_DIV_MASK | MASKDIST_BIT),
            (uint8_t)((2 << 6) | (info_.maskDist ? MASKDIST_BIT : 0)));

  if (config.hasExtra(K_TUNCAP)) {
    int cap = (int)config.extraF(K_TUNCAP, 0.0f);
    if (cap >= 0 && cap <= 15) {
      modifyReg(addr_, AS3935_REG_CAP, TUNCAP_MASK, (uint8_t)cap);
      info_.tuningCap = (uint8_t)cap;
    }
  }

  uint8_t cap = 0;
  if (readReg(AS3935_REG_CAP, cap)) info_.tuningCap = (uint8_t)(cap & TUNCAP_MASK);

  // statystyki od zera
  modifyReg(addr_, AS3935_REG_MASK, CLSTAT_BIT, CLSTAT_BIT);
  info_.strikes = info_.disturbers = info_.noiseEvents = 0;
  info_.closestKm = NAN;
}

void LightningService::loop() {
  if (!info_.present || !addr_) return;
  unsigned long now = millis();

  bool pending = false;
  if (irqPin_ >= 0) pending = digitalRead(irqPin_) == HIGH;
  if (pending || (now - lastPollMs_ >= 500)) {
    lastPollMs_ = now;
    handleInterrupt();
  }
}

void LightningService::handleInterrupt() {
  uint8_t stat = 0;
  if (!readReg(AS3935_REG_STAT, stat)) return;

  switch (stat & INT_MASK) {
    case AS3935_INT_LIGHTNING: {
      uint8_t e[3] = {0, 0, 0};
      uint8_t dist = 0;
      float energy = NAN, km = NAN;
      if (readRegs(AS3935_REG_ENERGY, e, 3)) {
        unsigned long raw = ((unsigned long)(e[2] & 0x1F) << 16) |
                            ((unsigned long)e[1] << 8) | (unsigned long)e[0];
        energy = (float)raw;
      }
      if (readReg(AS3935_REG_DISTANCE, dist)) {
        dist &= 0x3F;
        if (dist < 0x3F) km = (float)dist;
      }
      registerStrike(km, energy);
      break;
    }
    case AS3935_INT_DISTURBER:
      info_.disturbers++;
      break;
    case AS3935_INT_NOISE:
      info_.noiseEvents++;
      break;
    default:
      break;
  }
}

void LightningService::registerStrike(float distanceKm, float energy) {
  info_.strikes++;
  info_.lastStrikeMs = millis();
  info_.lastDistanceKm = distanceKm;
  info_.lastEnergy = energy;
  if (!isnan(distanceKm) && (isnan(info_.closestKm) || distanceKm < info_.closestKm)) {
    info_.closestKm = distanceKm;
  }
  forecast.addLightning(isnan(distanceKm) ? -1.0f : distanceKm, energy);

  static unsigned long lastLogMs = 0;
  if (info_.lastStrikeMs - lastLogMs > 5000UL) {
    lastLogMs = info_.lastStrikeMs;
    if (isnan(distanceKm)) {
      LOG_I("AS3935: wyładowanie poza zasięgiem pomiaru odległości");
    } else {
      LOG_I("AS3935: wyładowanie w odległości %.0f km (energia %.0f)", distanceKm,
            isnan(energy) ? -1.0f : energy);
    }
  }
}

void LightningService::setIndoor(bool indoor) {
  config.setExtraF(K_INDOOR, indoor ? 1.0f : 0.0f);
  config.save();
  applyConfig();
}

void LightningService::setSensitivity(uint8_t nfLev, uint8_t wdth, uint8_t spikeRej, uint8_t minStrike) {
  config.setExtraF(K_NFLEV, (float)constrain((int)nfLev, 0, 7));
  config.setExtraF(K_WDTH, (float)constrain((int)wdth, 0, 15));
  config.setExtraF(K_SPIKE, (float)constrain((int)spikeRej, 0, 15));
  config.setExtraF(K_MINSTR, (float)constrain((int)minStrike, 0, 3));
  config.save();
  applyConfig();
}

void LightningService::setMaskDist(bool mask) {
  config.setExtraF(K_MASKDIST, mask ? 1.0f : 0.0f);
  config.save();
  applyConfig();
}

// Pomiar częstotliwości LCO (rezonator anteny) sprzętowym licznikiem impulsów.
// Wartość docelowa to 500 kHz ±3,5% - przy odchyleniu trzeba zmienić trymer.
float LightningService::measureLcoKHz() {
  if (!addr_ || irqPin_ < 0) return NAN;

  modifyReg(addr_, AS3935_REG_CAP, DISP_LCO_BIT, DISP_LCO_BIT);  // LCO na pin IRQ
  delay(5);

#if LIGHTNING_PCNT_AVAILABLE
  pcnt_config_t cfg = {};
  cfg.pulse_gpio_num = irqPin_;
  cfg.ctrl_gpio_num = PCNT_PIN_NOT_USED;
  cfg.channel = PCNT_CHANNEL_0;
  cfg.unit = PCNT_UNIT_0;
  cfg.pos_mode = PCNT_COUNT_INC;      // zbocze narastające
  cfg.neg_mode = PCNT_COUNT_DIS;
  cfg.lctrl_mode = PCNT_MODE_KEEP;
  cfg.hctrl_mode = PCNT_MODE_KEEP;
  cfg.counter_h_lim = 30000;
  cfg.counter_l_lim = 0;

  if (pcnt_unit_config(&cfg) != ESP_OK) {
    modifyReg(addr_, AS3935_REG_CAP, DISP_LCO_BIT, 0);
    return NAN;
  }
  pcnt_counter_clear(PCNT_UNIT_0);
  pcnt_counter_resume(PCNT_UNIT_0);
  delay(20);                          // okno 20 ms
  int16_t count = 0;
  pcnt_counter_pause(PCNT_UNIT_0);
  pcnt_get_counter_value(PCNT_UNIT_0, &count);
  pcnt_counter_clear(PCNT_UNIT_0);
#else
  sLcoCount = 0;
  const int irqNum = digitalPinToInterrupt(irqPin_);
  attachInterrupt(irqNum, lcoIsr, RISING);
  delay(20);                          // okno 20 ms
  detachInterrupt(irqNum);
  int16_t count = (int16_t)(sLcoCount > 32000UL ? 32000UL : sLcoCount);
#endif

  modifyReg(addr_, AS3935_REG_CAP, DISP_LCO_BIT, 0);
  delay(2);
  // przywrócenie pinu IRQ do pracy jako wejście przerwania
  pinMode(irqPin_, INPUT);

  if (count <= 0) return NAN;
  float khz = (float)count * 50.0f;   // 1/0,02 s = 50
  if (khz < 100.0f || khz > 900.0f) return NAN;
  return khz;
}

// Automatyczny dobór trymera: szukamy pojemności, przy której LCO jest
// najbliżej 500 kHz. Wynik zapisywany jest w konfiguracji.
float LightningService::autoTune() {
  if (!addr_ || irqPin_ < 0) return NAN;

  float bestKhz = NAN;
  int bestCap = -1;
  bool any = false;

  for (int cap = 0; cap <= 15; cap++) {
    modifyReg(addr_, AS3935_REG_CAP, TUNCAP_MASK, (uint8_t)cap);
    delay(3);
    float f = measureLcoKHz();
    if (!isnan(f)) {
      any = true;
      if (bestCap < 0 || fabsf(f - 500.0f) < fabsf(bestKhz - 500.0f)) {
        bestKhz = f;
        bestCap = cap;
      }
      if (fabsf(f - 500.0f) <= 5.0f) break;   // wystarczająco blisko
    }
    yield();
  }

  if (bestCap >= 0) {
    modifyReg(addr_, AS3935_REG_CAP, TUNCAP_MASK, (uint8_t)bestCap);
    info_.tuningCap = (uint8_t)bestCap;
    config.setExtraF(K_TUNCAP, (float)bestCap);
    config.save();
    LOG_I("AS3935: kalibracja anteny - trymer %d, LCO %.1f kHz", bestCap, bestKhz);
  }
  return any ? bestKhz : NAN;
}

void LightningService::calibrate() {
  if (!addr_) return;
  writeReg(AS3935_REG_CALIB_RC, DIRECT_CMD);   // kalibracja RCO
  delay(3);
  autoTune();
}

void LightningService::resetDefaults() {
  if (!addr_) return;
  writeReg(AS3935_REG_PRESET, DIRECT_CMD);
  delay(3);
  writeReg(AS3935_REG_CALIB_RC, DIRECT_CMD);
  delay(3);
  applyConfig();
}

void LightningService::setTuningCap(int cap) {
  if (!addr_) return;
  if (cap < 0) {
    autoTune();
    return;
  }
  cap = constrain(cap, 0, 15);
  modifyReg(addr_, AS3935_REG_CAP, TUNCAP_MASK, (uint8_t)cap);
  info_.tuningCap = (uint8_t)cap;
  config.setExtraF(K_TUNCAP, (float)cap);
  config.save();
}

void LightningService::clearStatistics() {
  if (!addr_) return;
  modifyReg(addr_, AS3935_REG_MASK, CLSTAT_BIT, CLSTAT_BIT);
  info_.strikes = info_.disturbers = info_.noiseEvents = 0;
  info_.closestKm = NAN;
}

String LightningService::json() const {
  String j = "{";
  j += "\"enabled\":";
  j += info_.enabled ? "true" : "false";
  j += ",\"present\":";
  j += info_.present ? "true" : "false";
  j += ",\"address\":";
  j += String((int)info_.address);
  j += ",\"indoor\":";
  j += info_.indoor ? "true" : "false";
  j += ",\"irq_line\":";
  j += info_.irqLine ? "true" : "false";
  j += ",\"mask_dist\":";
  j += info_.maskDist ? "true" : "false";
  j += ",\"wdth\":" + String(info_.wdth);
  j += ",\"nf_lev\":" + String(info_.nfLev);
  j += ",\"spike_rej\":" + String(info_.spikeRej);
  j += ",\"min_strikes\":" + String(info_.minStrike);
  j += ",\"tuning_cap\":" + String(info_.tuningCap);
  j += ",\"strikes\":" + String(info_.strikes);
  j += ",\"disturbers\":" + String(info_.disturbers);
  j += ",\"noise\":" + String(info_.noiseEvents);
  j += ",\"i2c_errors\":" + String(info_.i2cErrors);
  j += ",\"last_distance_km\":";
  if (isnan(info_.lastDistanceKm)) j += "null"; else j += String(info_.lastDistanceKm, 0);
  j += ",\"last_energy\":";
  if (isnan(info_.lastEnergy)) j += "null"; else j += String(info_.lastEnergy, 0);
  j += ",\"closest_km\":";
  if (isnan(info_.closestKm)) j += "null"; else j += String(info_.closestKm, 0);
  unsigned long ageS = 999;
  if (info_.lastStrikeMs) {
    unsigned long s = (millis() - info_.lastStrikeMs) / 1000UL;
    ageS = s > 998 ? 998 : s;
  }
  j += ",\"last_strike_age_s\":" + String(ageS);
  j += "}";
  return j;
}
