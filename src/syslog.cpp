/* =============================================================================
 * Stacja Pogody - stacja meteorologiczna na ESP32-S3 WROOM-1 (N16R8)
 * Copyright (c) 2026 kruzio1985 - https://github.com/kruzio1985
 * Licencja: Stacja Pogody Non-Commercial License 1.0 (plik LICENSE)
 * Użytek niekomercyjny. Kontakt: kruzio1985@users.noreply.github.com
 * =============================================================================
 */
#include "syslog.h"
#include "config.h"
#include <esp_log.h>
#include <esp_system.h>
#include <esp_heap_caps.h>
#include <Preferences.h>

SysLog syslog;

// ------------------------------------------------------------
//  Powód ostatniego restartu (widoczny też w zakładce Diagnostyka)
// ------------------------------------------------------------
static const char* resetReasonText(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:  return "włączenie zasilania";
    case ESP_RST_EXT:      return "reset zewnętrzny";
    case ESP_RST_SW:       return "restart programowy";
    case ESP_RST_PANIC:    return "wyjątek / panic";
    case ESP_RST_INT_WDT:  return "watchdog przerwań";
    case ESP_RST_TASK_WDT: return "watchdog zadania";
    case ESP_RST_WDT:      return "watchdog";
    case ESP_RST_DEEPSLEEP:return "wybudzenie z deep sleep";
    case ESP_RST_BROWNOUT: return "spadek napięcia (brownout)";
    case ESP_RST_SDIO:     return "reset SDIO";
    default:               return "nieznany";
  }
}

char* SysLog::allocPsi(size_t n) {
  char* p = nullptr;
  if (psramFound()) p = (char*)heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!p) p = (char*)malloc(n);
  return p;
}

// Bufor logów to dwa bloki (bufor + kopia dla HTTP). Na płytce z PSRAM leżą
// one w PSRAM i mogą być duże, ale na ESP32-C3 (brak PSRAM) 2 x 48 kB zjadało
// prawie 1/3 DRAM-u - brakowało go potem dla Wi-Fi, serwera www, buforów
// magistrali RS485 i kopii listy kanałów (stacja restartowała się w pętli).
static const size_t SYSLOG_RAM_MAX = 16 * 1024;   // limit bez PSRAM
static const size_t SYSLOG_RAM_MIN = 4 * 1024;    // mniejszego nie ma sensu

void SysLog::begin(size_t capacity) {
  if (mtx_) return;                       // begin() wywołane drugi raz
  mtx_ = xSemaphoreCreateMutex();

  size_t want = capacity;
  if (!psramFound() && want > SYSLOG_RAM_MAX) want = SYSLOG_RAM_MAX;

  // Gdy brakuje pamięci, bufor jest zmniejszany (a nie wyłączany) - logi
  // startowe są zbyt cenne, żeby z nich rezygnować.
  for (cap_ = want; cap_ >= SYSLOG_RAM_MIN; cap_ /= 2) {
    buf_ = allocPsi(cap_);
    snap_ = allocPsi(cap_);
    if (buf_ && snap_) break;
    free(buf_); free(snap_);
    buf_ = snap_ = nullptr;
  }
  if (!buf_ || !snap_) {
    // Bez pamięci logi trafiają tylko na port szeregowy.
    free(buf_); free(snap_);
    buf_ = snap_ = nullptr;
    cap_ = 0;
    return;
  }
  buf_[0] = '\0';
  snap_[0] = '\0';

  // Licznik uruchomień - pozwala odróżnić "restart po aktualizacji"
  // od przypadkowych restartów w pętli.
  Preferences prefs;
  if (prefs.begin("stacja", false)) {
    bootCount_ = prefs.getUInt("boots", 0) + 1;
    prefs.putUInt("boots", bootCount_);
    prefs.end();
  }

  snprintf(resetReason_, sizeof(resetReason_), "%s", resetReasonText(esp_reset_reason()));

  // Przechwycenie logów ESP-IDF (poziom E jest wkompilowany w biblioteki,
  // dlatego błędy sterowników Wi-Fi/SD/systemu plików też trafią na stronę).
  esp_log_level_set("*", ESP_LOG_INFO);
  esp_log_set_vprintf(&SysLog::vprintfHook);

  Serial.printf("[syslog] Bufor %u kB w %s\n", (unsigned)(cap_ / 1024),
                psramFound() ? "PSRAM" : "RAM (brak PSRAM!)");
  line('B', "boot", "=== start Stacja Pogody %s, powód resetu: %s, uruchomienie #%u ===",
       FW_VERSION, resetReason_, (unsigned)bootCount_);
}

void SysLog::clear() {
  if (!buf_) return;
  xSemaphoreTake(mtx_, portMAX_DELAY);
  len_ = 0;
  buf_[0] = '\0';
  errors_ = 0;
  warns_ = 0;
  xSemaphoreGive(mtx_);
}

void SysLog::append(const char* s, size_t n) {
  if (!buf_ || !n) return;

  xSemaphoreTake(mtx_, portMAX_DELAY);

  // Wpis dłuższy niż cały bufor - zapisujemy tylko jego koniec.
  if (n >= cap_ - 1) {
    s += n - (cap_ - 1);
    n = cap_ - 1;
  }

  if (len_ + n > cap_ - 1) {
    // Odrzucamy starszą połowę; początek wycinamy na granicy linii,
    // żeby nie zostawić urwanego wpisu.
    size_t from = len_ / 2;
    while (from < len_ && buf_[from] != '\n') from++;
    if (from < len_) from++;
    size_t keep = len_ - from;
    if (keep) memmove(buf_, buf_ + from, keep);
    len_ = keep;
    if (len_ + n > cap_ - 1) len_ = 0;
  }

  memcpy(buf_ + len_, s, n);
  len_ += n;
  buf_[len_] = '\0';

  xSemaphoreGive(mtx_);
}

void SysLog::line(char level, const char* tag, const char* fmt, ...) {
  char msg[224];
  va_list ap;
  va_start(ap, fmt);
  int m = vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);
  if (m < 0) m = 0;
  size_t mlen = ((size_t)m < sizeof(msg) - 1) ? (size_t)m : sizeof(msg) - 1;

  char out[272];
  int h = snprintf(out, sizeof(out), "%c (%lu) %s: ", level, (unsigned long)millis(), tag);
  size_t len = (h > 0) ? (size_t)h : 0;
  if (len > sizeof(out) - 4) len = sizeof(out) - 4;

  size_t room = sizeof(out) - 2 - len;          // -2: ewentualne "\n" i NUL
  if (mlen > room) mlen = room;
  memcpy(out + len, msg, mlen);
  len += mlen;
  if (len == 0 || out[len - 1] != '\n') out[len++] = '\n';

  if (level == 'E') errors_++;
  if (level == 'W') warns_++;

  append(out, len);

  // Ten sam tekst na port szeregowy (podgląd w monitorze).
  Serial.write((const uint8_t*)out, len);
}

size_t SysLog::snapshot() {
  if (!buf_ || !snap_) return 0;
  xSemaphoreTake(mtx_, portMAX_DELAY);
  memcpy(snap_, buf_, len_ + 1);
  size_t n = len_;
  xSemaphoreGive(mtx_);
  return n;
}

// ------------------------------------------------------------
//  Hak ESP-IDF: logi z bibliotek systemowych lądują w tym samym buforze
//  i nadal są wypisywane na port szeregowy.
// ------------------------------------------------------------
int SysLog::vprintfHook(const char* fmt, va_list args) {
  static volatile bool reentry = false;
  if (reentry) return 0;               // zabezpieczenie przed rekurencją
  reentry = true;

  char tmp[200];
  int n = vsnprintf(tmp, sizeof(tmp), fmt, args);
  size_t len = (n > 0) ? ((size_t)n < sizeof(tmp) - 1 ? (size_t)n : sizeof(tmp) - 1) : 0;

  if (len) {
    syslog.append(tmp, len);
    Serial.write((const uint8_t*)tmp, len);
  }

  reentry = false;
  return (int)len;
}
