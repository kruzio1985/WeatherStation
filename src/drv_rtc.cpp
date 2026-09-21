/* =============================================================================
 * Stacja Pogody - stacja meteorologiczna na ESP32-S3 WROOM-1 (N16R8)
 * Copyright (c) 2026 kruzio1985 - https://github.com/kruzio1985
 * Licencja: PolyForm Noncommercial License 1.0.0 (plik LICENSE)
 * Użytek niekomercyjny. Kontakt: kruzio1985@users.noreply.github.com
 * =============================================================================
 */
#include "drv_rtc.h"

#include <math.h>
#include <sys/time.h>
#include <time.h>
#include <esp_sntp.h>

#include "config.h"
#include "pins.h"
#include "pinmap.h"
#include "syslog.h"

RtcDriver rtc;

// Wire1 istnieje na układach z dwoma kontrolerami I2C (np. ESP32-S3).
// Arduino core 2 udostępnia Wire1 także na układach jednokontrolerowych
// (jako alias wspólnej magistrali), natomiast core 3 już nie - dlatego
// decyzję podejmujemy osobno dla każdej wersji rdzenia.
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  #if defined(SOC_I2C_NUM) && SOC_I2C_NUM > 1
    #define STACJA_HAS_WIRE1 1
  #else
    #define STACJA_HAS_WIRE1 0
  #endif
#else
  #define STACJA_HAS_WIRE1 1
#endif

// Czas uznajemy za ustawiony dopiero od 2020 roku - chroni przed zapisaniem
// do układu RTC śmieci z "pustego" zegara (wszystkie rejestry 0) i przed
// ustawieniem z niego zegara systemowego.
static const time_t RTC_MIN_EPOCH = 1577836800L;   // 2020-01-01 00:00:00 UTC

static inline uint8_t bcd2bin(uint8_t v) { return (uint8_t)((v >> 4) * 10 + (v & 0x0F)); }
static inline uint8_t bin2bcd(uint8_t v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

static String timeStr(time_t t) {
  struct tm x;
  localtime_r(&t, &x);
  char b[24];
  strftime(b, sizeof(b), "%Y-%m-%d %H:%M:%S", &x);
  return String(b);
}

static String tmStr(const struct tm& t) {
  char b[24];
  strftime(b, sizeof(b), "%Y-%m-%d %H:%M:%S", &t);
  return String(b);
}

void rtcStartNtp() {
  String tz = config.tzString();
  String ntp = config.ntpServer();
  if (tz.length() == 0) tz = "CET-1CEST,M3.5.0,M10.5.0/3";
  if (ntp.length() == 0) ntp = "pool.ntp.org";
  configTzTime(tz.c_str(), ntp.c_str(), "time.nist.gov");
}

// ------------------------------------------------------------- magistrala
TwoWire* RtcDriver::busPtr() {
#if STACJA_HAS_WIRE1
  return config.rtcBus() == 1 ? &Wire1 : &Wire;
#else
  return &Wire;
#endif
}

// Piny magistrali wspólnej bierzemy z mapy pinów (można je zmienić w zakładce
// "Piny"), a gdy mapa ich nie zna - z wartości domyślnych w pins.h.
static int sharedI2cPin(const char* key, int def) {
  const int p = pinMap.pin(key);
  return p >= 0 ? p : def;
}

bool RtcDriver::probe(uint8_t a) {
  TwoWire* w = busPtr();
  w->beginTransmission(a);
  return w->endTransmission() == 0;
}

bool RtcDriver::readRegs(uint8_t reg, uint8_t* buf, size_t len) {
  if (addr_ == 0 || len == 0) return false;
  TwoWire* w = busPtr();
  w->beginTransmission(addr_);
  w->write(reg);
  if (w->endTransmission(false) != 0) return false;
  if (w->requestFrom((int)addr_, (int)len) != (int)len) return false;

  for (size_t i = 0; i < len; i++) {
    unsigned long start = millis();
    while (!w->available() && millis() - start < 20) delay(1);
    if (!w->available()) return false;
    buf[i] = (uint8_t)w->read();
  }
  return true;
}

bool RtcDriver::writeRegs(uint8_t reg, const uint8_t* buf, size_t len) {
  if (addr_ == 0 || len == 0) return false;
  TwoWire* w = busPtr();
  w->beginTransmission(addr_);
  w->write(reg);
  for (size_t i = 0; i < len; i++) w->write(buf[i]);
  return w->endTransmission() == 0;
}

// ------------------------------------------------------------- wykrywanie
void RtcDriver::detect() {
  chip_ = RTC_NONE;
  addr_ = 0;

  const uint8_t want = config.rtcType();   // 0 = automat, 1 = DS3231, 2 = DS1307, 3 = PCF8563

  if (want == RTC_PCF8563) {
    if (probe(0x51)) { chip_ = RTC_PCF8563; addr_ = 0x51; }
    return;
  }
  if (want == RTC_DS3231 || want == RTC_DS1307) {
    if (probe(0x68)) { chip_ = want; addr_ = 0x68; }
    return;
  }

  // Automat: najpierw adres 0x68 (DS3231/DS1307), potem 0x51 (PCF8563).
  // DS3231 i DS1307 mają identyczny układ rejestrów czasu, dlatego przy
  // automatycznym wykrywaniu przyjmujemy DS3231 (termometr i flaga OSF są
  // tylko dodatkiem) - typ można wymusić w ustawieniach.
  if (probe(0x68)) { chip_ = RTC_DS3231; addr_ = 0x68; return; }
  if (probe(0x51)) { chip_ = RTC_PCF8563; addr_ = 0x51; return; }
}

String RtcDriver::chipName() const {
  switch (chip_) {
    case RTC_DS3231:  return "DS3231";
    case RTC_DS1307:  return "DS1307";
    case RTC_PCF8563: return "PCF8563";
    default:          return "";
  }
}

// ------------------------------------------------------------- start pracy
void RtcDriver::begin() {
  enabled_ = config.rtcEnabled();
  lastSyncMs_ = millis();

  if (config.rtcBus() == 1 && !bus2Init_) {
#if STACJA_HAS_WIRE1
    Wire1.begin(config.rtcSda(), config.rtcScl(), 100000);
#else
    Wire.begin(config.rtcSda(), config.rtcScl(), 100000);
#endif
    bus2Init_ = true;
  }

  if (!enabled_) {
    chip_ = RTC_NONE;
    addr_ = 0;
    LOG_I("RTC: wyłączony w ustawieniach (magistrala %u)", (unsigned)config.rtcBus());
    return;
  }

  detect();

  if (!present()) {
    LOG_W("RTC: brak układu na magistrali %u (0x68 DS3231/DS1307, 0x51 PCF8563)",
          (unsigned)config.rtcBus());
    return;
  }

  LOG_I("RTC: %s na adresie 0x%02X, magistrala %u (SDA %d / SCL %d)",
        chipName().c_str(), (unsigned)addr_, (unsigned)config.rtcBus(),
        config.rtcBus() == 1 ? config.rtcSda() : sharedI2cPin("i2c_sda", PIN_SDA),
        config.rtcBus() == 1 ? config.rtcScl() : sharedI2cPin("i2c_scl", PIN_SCL));

  const bool lost = batteryLost();
  const time_t now = time(nullptr);

  if (now >= RTC_MIN_EPOCH) {
    // Mamy już poprawny czas (NTP albo GPS z poprzedniego uruchomienia) -
    // to on jest wzorcem, więc dopisujemy go do RTC i czyścimy flagę.
    if (syncRtcFromSystem()) {
      clearBatteryFlag();
      LOG_I("RTC: zapisany czas systemowy %s", timeStr(time(nullptr)).c_str());
    }
  } else if (!lost && syncSystemFromRtc()) {
    LOG_I("RTC: ustawiłem zegar systemowy z układu RTC (%s)", timeStr(time(nullptr)).c_str());
  } else if (lost) {
    LOG_W("RTC: brak podtrzymania (flagi układu 0x%02X) - ustaw czas z NTP/GPS", (unsigned)addr_);
  }
}

void RtcDriver::reload() {
  chip_ = RTC_NONE;
  addr_ = 0;
  begin();
}

// ------------------------------------------------------------- odczyt czasu
bool RtcDriver::readTime(struct tm& out) {
  bool lost = false;
  return readRaw(out, lost);
}

bool RtcDriver::readRaw(struct tm& t, bool& battLost) {
  battLost = false;
  memset(&t, 0, sizeof(t));
  if (!present()) return false;

  uint8_t b[7];
  int year = 0;

  if (chip_ == RTC_PCF8563) {
    if (!readRegs(0x02, b, 7)) return false;
    battLost  = (b[0] & 0x80) != 0;              // VL - napięcie podtrzymania za niskie
    t.tm_sec  = bcd2bin(b[0] & 0x7F);
    t.tm_min  = bcd2bin(b[1] & 0x7F);
    t.tm_hour = bcd2bin(b[2] & 0x3F);
    t.tm_mday = bcd2bin(b[3] & 0x3F);
    uint8_t dw = bcd2bin(b[4] & 0x07);
    t.tm_wday = dw <= 6 ? dw : 0;
    t.tm_mon  = bcd2bin(b[5] & 0x1F) - 1;
    uint8_t yy = bcd2bin(b[6]);
    year = (b[5] & 0x80) ? 2000 + yy : 1900 + yy;   // bit 7 = wiek
  } else {
    if (!readRegs(0x00, b, 7)) return false;
    battLost = (chip_ == RTC_DS1307) && ((b[0] & 0x80) != 0);   // CH = zegar zatrzymany
    t.tm_sec = bcd2bin(b[0] & 0x7F);
    t.tm_min = bcd2bin(b[1] & 0x7F);

    const bool mode12 = (b[2] & 0x40) != 0;
    t.tm_hour = bcd2bin(mode12 ? (uint8_t)(b[2] & 0x1F) : (uint8_t)(b[2] & 0x3F));
    if (mode12) {
      if ((b[2] & 0x20) && t.tm_hour < 12) t.tm_hour += 12;      // PM
      if (!(b[2] & 0x20) && t.tm_hour == 12) t.tm_hour = 0;      // 12 AM
    }

    uint8_t dw = bcd2bin(b[3] & 0x07);
    t.tm_wday = dw ? (int)dw - 1 : 0;                            // 1 = niedziela
    t.tm_mday = bcd2bin(b[4] & 0x3F);
    t.tm_mon  = bcd2bin(b[5] & 0x1F) - 1;
    uint8_t yy = bcd2bin(b[6]);
    year = (b[5] & 0x80) ? 2000 + yy : (yy >= 70 ? 1900 + yy : 2000 + yy);
  }

  // Normalizacja wieku: inne programy ustawiające RTC często zostawiają bit
  // wieku wyzerowany, a stacja i tak pracuje w latach 2020..2099.
  if (year < 2020) year += 100;
  if (year > 2099) year -= 100;
  t.tm_year = year - 1900;
  t.tm_isdst = -1;

  const bool sane = t.tm_sec >= 0 && t.tm_sec < 60 &&
                    t.tm_min >= 0 && t.tm_min < 60 &&
                    t.tm_hour >= 0 && t.tm_hour < 24 &&
                    t.tm_mday >= 1 && t.tm_mday <= 31 &&
                    t.tm_mon >= 0 && t.tm_mon <= 11 &&
                    t.tm_year >= 100 && t.tm_year <= 199;
  return sane;
}

// ------------------------------------------------------------- zapis czasu
bool RtcDriver::writeTime(const struct tm& t) { return writeRaw(t); }

bool RtcDriver::writeRaw(const struct tm& t) {
  if (!present()) return false;
  if (t.tm_year < 100 || t.tm_year > 199) return false;   // obsługujemy 2000..2099
  if (t.tm_mon < 0 || t.tm_mon > 11 || t.tm_mday < 1 || t.tm_mday > 31) return false;
  if (t.tm_hour < 0 || t.tm_hour > 23 || t.tm_min < 0 || t.tm_min > 59 ||
      t.tm_sec < 0 || t.tm_sec > 59) {
    return false;
  }

  uint8_t b[7];
  if (chip_ == RTC_PCF8563) {
    b[0] = (uint8_t)(bin2bcd(t.tm_sec) & 0x7F);        // bit 7 = VL, zapis czasu go czyści
    b[1] = bin2bcd(t.tm_min);
    b[2] = bin2bcd(t.tm_hour);
    b[3] = bin2bcd(t.tm_mday);
    b[4] = (uint8_t)(t.tm_wday & 0x07);
    b[5] = (uint8_t)(bin2bcd(t.tm_mon + 1) | 0x80);    // bit 7 = wiek 20xx
    b[6] = bin2bcd(t.tm_year % 100);
    uint8_t ctrl[2] = {0x00, 0x00};                    // STOP = 0, zegar pracuje
    writeRegs(0x00, ctrl, 2);
    return writeRegs(0x02, b, 7);
  }

  b[0] = (uint8_t)(bin2bcd(t.tm_sec) & 0x7F);          // bit 7 = CH, 0 = zegar pracuje
  b[1] = bin2bcd(t.tm_min);
  b[2] = bin2bcd(t.tm_hour);                           // bit 6 = 0, tryb 24 h
  uint8_t dow = (uint8_t)(t.tm_wday + 1);              // 1 = niedziela
  if (dow < 1 || dow > 7) dow = 1;
  b[3] = dow;
  b[4] = bin2bcd(t.tm_mday);
  b[5] = (uint8_t)(bin2bcd(t.tm_mon + 1) | 0x80);      // bit 7 = wiek 20xx
  b[6] = bin2bcd(t.tm_year % 100);
  return writeRegs(0x00, b, 7);
}

// ------------------------------------------------------------- flagi układu
bool RtcDriver::batteryLost() {
  if (!present()) return false;

  struct tm t;
  bool lost = false;
  readRaw(t, lost);          // DS1307: CH, PCF8563: VL
  if (chip_ == RTC_DS3231) {
    uint8_t st = 0;
    if (readRegs(0x0F, &st, 1)) {
      lost = lost || ((st & 0x80) != 0);   // OSF - oscylator się zatrzymał
    }
  }
  return lost;
}

bool RtcDriver::clearBatteryFlag() {
  if (!present()) return false;

  if (chip_ == RTC_DS3231) {
    uint8_t st = 0xFF;
    if (!readRegs(0x0F, &st, 1)) return false;
    st = (uint8_t)(st & ~0x80);
    return writeRegs(0x0F, &st, 1);
  }

  // DS1307 (bit CH) i PCF8563 (bit VL) czyszczą flagę zapisem czasu.
  const time_t now = time(nullptr);
  struct tm t;
  if (now >= RTC_MIN_EPOCH) localtime_r(&now, &t);
  else if (!readTime(t)) return false;
  return writeRaw(t);
}

float RtcDriver::temperature() {
  if (chip_ != RTC_DS3231) return NAN;
  uint8_t b[2];
  if (!readRegs(0x11, b, 2)) return NAN;
  float temp = (float)((int8_t)b[0]) + (float)(b[1] >> 6) * 0.25f;
  if (temp < -45.0f || temp > 110.0f) return NAN;
  return temp;
}

// ------------------------------------------------------------- czas systemowy
int RtcDriver::driftSeconds() {
  struct tm r;
  if (!readTime(r)) return 0;
  const time_t rt = mktime(&r);
  const time_t st = time(nullptr);
  if (rt < RTC_MIN_EPOCH) return 0;
  return (int)((long)rt - (long)st);
}

bool RtcDriver::ntpSynced() const {
  if (time(nullptr) < RTC_MIN_EPOCH) return false;
  return sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED;
}

bool RtcDriver::syncSystemFromRtc() {
  struct tm r;
  if (!readTime(r)) return false;
  const time_t e = mktime(&r);
  if (e < RTC_MIN_EPOCH) return false;

  struct timeval tv;
  tv.tv_sec = e;
  tv.tv_usec = 0;
  settimeofday(&tv, nullptr);
  return true;
}

bool RtcDriver::syncRtcFromSystem() {
  const time_t now = time(nullptr);
  if (now < RTC_MIN_EPOCH) return false;
  struct tm t;
  localtime_r(&now, &t);
  return writeRaw(t);
}

bool RtcDriver::setTimeFromEpoch(time_t epoch) {
  if (epoch < RTC_MIN_EPOCH) return false;
  struct tm t;
  localtime_r(&epoch, &t);
  return writeTime(t);
}

bool RtcDriver::setTimeFromString(const String& s) {
  String txt = s;
  txt.trim();
  txt.replace('T', ' ');

  int y = 0, mo = 0, d = 0, h = 0, mi = 0, se = 0;
  if (sscanf(txt.c_str(), "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &se) < 5) return false;

  struct tm t;
  memset(&t, 0, sizeof(t));
  t.tm_year = y - 1900;
  t.tm_mon  = mo - 1;
  t.tm_mday = d;
  t.tm_hour = h;
  t.tm_min  = mi;
  t.tm_sec  = se;
  t.tm_isdst = -1;

  const time_t e = mktime(&t);      // walidacja: mktime normalizuje błędne daty
  if (e < RTC_MIN_EPOCH) return false;

  struct tm chk;
  localtime_r(&e, &chk);
  if (chk.tm_year != t.tm_year || chk.tm_mon != t.tm_mon || chk.tm_mday != t.tm_mday) return false;

  return writeRaw(chk);
}

// ------------------------------------------------------------- podgląd
void RtcDriver::loop() {
  if (!present() || !enabled_ || !config.rtcNtpSync()) return;

  const unsigned long now = millis();
  if (now - lastSyncMs_ < 60000UL) return;
  lastSyncMs_ = now;

  // Wzorcem jest czas systemowy (NTP, GPS albo ustawiony z RTC przy starcie).
  // Gdy oba zegary się rozjadą (albo zmieni się czas letni/zimowy), RTC
  // dostaje aktualną godzinę - wtedy po restarcie bez sieci jest poprawna.
  if (time(nullptr) < RTC_MIN_EPOCH) return;

  if (batteryLost() && clearBatteryFlag()) {
    LOG_I("RTC: wyczyszczona flaga braku podtrzymania (0x%02X)", (unsigned)addr_);
  }

  const int drift = driftSeconds();
  if (drift > 2 || drift < -2) {
    if (syncRtcFromSystem()) {
      LOG_I("RTC: poprawiony czas układu (różnica %d s)", drift);
    }
  }
}

void RtcDriver::toJson(JsonObject o) {
  const time_t sys = time(nullptr);

  o["enabled"]   = config.rtcEnabled();
  o["ntp_sync"]  = config.rtcNtpSync();
  o["type"]      = config.rtcType();
  o["bus"]       = config.rtcBus();
  o["sda"]       = config.rtcBus() == 1 ? config.rtcSda() : sharedI2cPin("i2c_sda", PIN_SDA);
  o["scl"]       = config.rtcBus() == 1 ? config.rtcScl() : sharedI2cPin("i2c_scl", PIN_SCL);
  o["present"]   = present();
  o["chip"]      = chip_;
  o["chip_name"] = chipName();
  o["addr"]      = addr_;
  o["ntp_synced"] = ntpSynced();
  o["sys_ok"]    = sys >= RTC_MIN_EPOCH;
  o["sys_epoch"] = (long)sys;
  o["sys_time"]  = sys >= RTC_MIN_EPOCH ? timeStr(sys) : String("");

  struct tm r;
  if (readTime(r)) {
    const time_t rt = mktime(&r);
    o["time_ok"]   = true;
    o["rtc_epoch"] = (long)rt;
    o["rtc_time"]  = tmStr(r);
    o["drift_s"]   = (long)((long)rt - (long)sys);
  } else {
    o["time_ok"]   = false;
    o["rtc_epoch"] = 0;
    o["rtc_time"]  = "";
    o["drift_s"]   = 0;
  }

  o["battery_lost"] = batteryLost();
  const float tp = temperature();
  o["temp_ok"] = !isnan(tp);
  o["temp_c"]  = isnan(tp) ? 0.0f : tp;
}
