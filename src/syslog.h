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
//  Bufor logów systemowych - widoczny na stronie www (zakładka "Logi").
//
//  Bufor leży w PSRAM i jest jednym ciągłym obszarem pamięci: gdy się
//  zapełni, starsza połowa jest odrzucana, a reszta przesuwana na początek.
//  Dzięki temu nie ma "zawijania" i wysłanie logów przez HTTP nie wymaga
//  sklejania dwóch fragmentów.
//
//  capacity to wartość docelowa: na płytce bez PSRAM (ESP32-C3) jest ona
//  ograniczana, a przy braku pamięci bufor jest jeszcze zmniejszany.
// =============================================================
class SysLog {
public:
  void begin(size_t capacity = 48 * 1024);
  void clear();

  // Dopisuje wpis w formacie: "E (12345) tag: treść"
  void line(char level, const char* tag, const char* fmt, ...) __attribute__((format(printf, 4, 5)));

  bool   ready() const { return buf_ != nullptr; }
  size_t size() const { return len_; }
  size_t capacity() const { return cap_; }
  const char* data() const { return buf_; }

  uint32_t errors() const { return errors_; }
  uint32_t warns()  const { return warns_; }

  // Kopia logów do wewnętrznego bufora - dzięki temu w trakcie wysyłania
  // przez HTTP nikt nie dopisuje do przesyłanych danych.
  size_t snapshot();
  const char* snapshotData() const { return snap_; }

  uint32_t bootCount() const { return bootCount_; }
  const char* resetReason() const { return resetReason_; }

private:
  static int vprintfHook(const char* fmt, va_list args);
  void append(const char* s, size_t n);
  static char* allocPsi(size_t n);

  char* buf_ = nullptr;
  char* snap_ = nullptr;
  size_t cap_ = 0;
  size_t len_ = 0;
  volatile uint32_t errors_ = 0;
  volatile uint32_t warns_ = 0;
  SemaphoreHandle_t mtx_ = nullptr;
  uint32_t bootCount_ = 0;
  char resetReason_[48] = "?";
};

extern SysLog syslog;

// Wygodne makra - używane w całym firmware.
#define LOG_I(...) syslog.line('I', "stacja", __VA_ARGS__)
#define LOG_W(...) syslog.line('W', "stacja", __VA_ARGS__)
#define LOG_E(...) syslog.line('E', "stacja", __VA_ARGS__)
