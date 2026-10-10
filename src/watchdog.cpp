/* =============================================================================
 * Stacja Pogody - stacja meteorologiczna na ESP32-S3 WROOM-1 (N16R8)
 * Copyright (c) 2026 kruzio1985 - https://github.com/kruzio1985
 * Licencja: PolyForm Noncommercial License 1.0.0 (plik LICENSE)
 * Użytek niekomercyjny. Kontakt: kruzio1985@users.noreply.github.com
 * =============================================================================
 */
#include "watchdog.h"
#include "config.h"
#include "pinmap.h"
#include "syslog.h"
#include <ArduinoJson.h>
#include <time.h>
#if !STACJA_HEADLESS
#include "ota.h"
#endif

WatchdogManager watchdog;

// Klucze w sekcji "extra" ustawień (NVS + kopia /config.json).
static const char* K_AUTO_RESTART      = "auto_restart";        // 0/1
static const char* K_AUTO_RESTART_HOUR = "auto_restart_hour";   // 0..23
static const char* K_WD_HEARTBEAT      = "wd_heartbeat";        // 0/1

void WatchdogManager::begin() {
  pin_ = pinMap.pin("wd_heartbeat");
  heartbeatOn_ = config.extraF(K_WD_HEARTBEAT, 0.0f) >= 0.5f;
  level_ = false;
  pulsing_ = false;
  lastToggle_ = millis();
  if (heartbeatEnabled()) {
    pinMode(pin_, OUTPUT);
    digitalWrite(pin_, level_ ? HIGH : LOW);
    LOG_I("Watchdog: impuls życia na GPIO %d (1 Hz)", pin_);
  } else if (pin_ >= 0) {
    LOG_I("Watchdog: pin impulsu życia GPIO %d, ale wyłączony w ustawieniach", pin_);
  }
}

bool WatchdogManager::autoRestartEnabled() {
  return config.extraF(K_AUTO_RESTART, 0.0f) >= 0.5f;
}

uint8_t WatchdogManager::autoRestartHour() {
  float h = config.extraF(K_AUTO_RESTART_HOUR, 4.0f);
  if (h < 0.0f) h = 0.0f;
  if (h > 23.0f) h = 23.0f;
  return (uint8_t)h;
}

bool WatchdogManager::heartbeatEnabled() {
  return pin_ >= 0 && heartbeatOn_;
}

int WatchdogManager::heartbeatPin() {
  return pin_;
}

bool WatchdogManager::heartbeatActive() {
  return pulsing_;
}

String WatchdogManager::autoRestartInfo() {
  if (!autoRestartEnabled()) return "wyłączony";
  time_t now = time(nullptr);
  if (now < 1000000000) return "czeka na synchronizację czasu";
  if (millis() < 15UL * 60000UL) return "czeka na stabilny start (15 min)";
  char b[48];
  snprintf(b, sizeof(b), "zaplanowany na %02u:00 (okno 00-09 min)", (unsigned)autoRestartHour());
  return String(b);
}

void WatchdogManager::heartbeat() {
  if (!heartbeatEnabled()) { pulsing_ = false; return; }
  unsigned long now = millis();
  // Wystarczy pilnować odstępu - nie używamy czasu bezwzględnego, żeby po
  // długiej operacji blokującej (np. zapis SD) impuls nadal wychodził równo.
  if (now - lastToggle_ >= 500UL) {
    lastToggle_ = now;
    level_ = !level_;
    digitalWrite(pin_, level_ ? HIGH : LOW);
    pulsing_ = true;
  }
}

void WatchdogManager::maybeDailyRestart() {
  if (!autoRestartEnabled()) return;
  if (restartDoneThisBoot_) return;

  // Kontrola co ~5 s - restart i tak ma okno 10 minut, nie ma potrzeby
  // sprawdzać przy każdym obiegu pętli.
  unsigned long nowMs = millis();
  if (nowMs - lastDailyCheck_ < 5000UL) return;
  lastDailyCheck_ = nowMs;

  // Strażnik przed pętlą restartów: okno minutowe to 0-9, a po restarcie
  // urządzenie musi najpierw popracować 15 minut - dopiero wtedy może znów
  // trafić w okno (i to dopiero o tej samej godzinie następnego dnia).
  if (nowMs < 15UL * 60000UL) return;

  time_t now = time(nullptr);
  if (now < 1000000000) return;   // brak czasu (NTP / RS485)

#if !STACJA_HEADLESS
  if (g_otaInProgress || ota.running()) return;   // nie przerywać aktualizacji
#endif

  struct tm tmv;
  localtime_r(&now, &tmv);
  if (tmv.tm_hour != (int)autoRestartHour()) return;
  if (tmv.tm_min > 9) return;

  maybeRestartNow();
}

void WatchdogManager::maybeRestartNow() {
  restartDoneThisBoot_ = true;
  // Zanim zrestartujemy: zapisz liczniki deszczu i inne dane z NVS, żeby
  // restart dobowy nie zerował historii (analogicznie do afcSave w snifferze).
  config.saveState();
  LOG_I("Watchdog: restart dobowy o %s (zapis stanu OK) - restartuję",
        autoRestartInfo().c_str());
  delay(200);
  ESP.restart();
}

String WatchdogManager::json() {
  JsonDocument d;
  d["auto_restart"] = autoRestartEnabled();
  d["auto_restart_hour"] = (uint8_t)autoRestartHour();
  d["auto_restart_info"] = autoRestartInfo();
  d["wd_heartbeat"] = heartbeatEnabled();
  d["wd_heartbeat_pin"] = pin_;
  d["wd_heartbeat_active"] = heartbeatActive();
  String s;
  serializeJson(d, s);
  return s;
}

bool WatchdogManager::applyJson(const char* json, size_t len) {
  JsonDocument d;
  if (deserializeJson(d, json, len)) return false;

  if (d["auto_restart"].is<bool>() || d["auto_restart"].is<float>() ||
      d["auto_restart"].is<int>()) {
    config.setExtraF(K_AUTO_RESTART, d["auto_restart"].as<float>() >= 0.5f ? 1.0f : 0.0f);
  }
  if (d["auto_restart_hour"].is<float>() || d["auto_restart_hour"].is<int>()) {
    float h = d["auto_restart_hour"].as<float>();
    if (h < 0.0f) h = 0.0f;
    if (h > 23.0f) h = 23.0f;
    config.setExtraF(K_AUTO_RESTART_HOUR, h);
  }
  if (d["wd_heartbeat"].is<bool>() || d["wd_heartbeat"].is<float>() ||
      d["wd_heartbeat"].is<int>()) {
    config.setExtraF(K_WD_HEARTBEAT, d["wd_heartbeat"].as<float>() >= 0.5f ? 1.0f : 0.0f);
  }

  if (!config.save()) return false;

  // Impuls życia ma ruszyć od razu po zapisie (bez restartu stacji).
  heartbeatOn_ = config.extraF(K_WD_HEARTBEAT, 0.0f) >= 0.5f;
  if (heartbeatEnabled()) {
    pinMode(pin_, OUTPUT);
    digitalWrite(pin_, level_ ? HIGH : LOW);
  }
  return true;
}
