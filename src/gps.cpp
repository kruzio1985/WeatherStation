/* =============================================================================
 * Stacja Pogody - stacja meteorologiczna na ESP32-S3 WROOM-1 (N16R8)
 * Copyright (c) 2026 kruzio1985 - https://github.com/kruzio1985
 * Licencja: Stacja Pogody Non-Commercial License 1.0 (plik LICENSE)
 * Użytek niekomercyjny. Kontakt: kruzio1985@users.noreply.github.com
 * =============================================================================
 */
#include "gps.h"
#include "pinmap.h"
#include "config.h"
#include "syslog.h"
#include <time.h>
#include <sys/time.h>
#include <string.h>
#include <math.h>

// Obsługiwane odbiorniki (każdy wysyła NMEA 0183 po UART, 9600 Bd domyślnie):
//   u-blox NEO-6M, NEO-7M, NEO-8M, NEO-M8N, NEO-M8Q, NEO-M9N, NEO-M9V,
//   ATGM336H (AT6558), L76K, L86, GT-U7 - te same ramki $GPGGA/$GPRMC/$GPGSV.

GpsService gps;

// UART1 - UART0 zostaje na konsolę/USB, UART2 obsługuje PMS5003.
static HardwareSerial GpsSerial(1);

// ------------------------------------------------------------
//  Suma kontrolna NMEA: XOR znaków między '$' i '*'
// ------------------------------------------------------------
static bool nmeaChecksumOk(const char* s) {
  const char* star = strrchr(s, '*');
  if (!star || star - s < 6) return false;
  uint8_t sum = 0;
  for (const char* p = s + 1; p < star; p++) sum ^= (uint8_t)*p;
  uint8_t want = (uint8_t)strtoul(star + 1, nullptr, 16);
  return sum == want;
}

// ------------------------------------------------------------
//  Pola ramki NMEA -> tablica wskaźników (modyfikuje bufor)
// ------------------------------------------------------------
static int splitFields(char* s, char** out, int maxFields) {
  int n = 0;
  if (n < maxFields) out[n++] = s;
  for (char* p = s; *p; p++) {
    if (*p == ',') {
      *p = '\0';
      if (n < maxFields) out[n++] = p + 1;
    } else if (*p == '*') {
      *p = '\0';
      break;
    }
  }
  return n;
}

static bool fieldEmpty(const char* v) { return !v || !*v; }

bool GpsService::begin() {
  rxPin_ = (int8_t)pinMap.pin("gps_rx");
  txPin_ = (int8_t)pinMap.pin("gps_tx");
  baud_  = config.gpsBaud();

  if (rxPin_ < 0) {
    enabled_ = false;
    LOG_I("GPS: wyłączony (brak przypisanego pinu RX)");
    return false;
  }

  GpsSerial.begin(baud_, SERIAL_8N1, rxPin_, txPin_ >= 0 ? txPin_ : -1);
  enabled_ = true;
  LOG_I("GPS: UART1 %u bps, RX=%d, TX=%d", (unsigned)baud_, rxPin_, txPin_);
  return true;
}

void GpsService::end() {
  if (!enabled_) return;
  GpsSerial.end();
  enabled_ = false;
  len_ = 0;
  LOG_I("GPS: port zamknięty (diagnostyka magistrali)");
}

void GpsService::loop() {
  if (!enabled_) return;

  int budget = 512;   // ograniczamy pracę na jedno wywołanie
  while (budget-- > 0 && GpsSerial.available()) feed((char)GpsSerial.read());
}

void GpsService::feed(char c) {
  if (c == '\n' || c == '\r') {
    if (len_ > 6) {
      buf_[len_] = '\0';
      parseSentence(buf_);
    }
    len_ = 0;
    return;
  }
  if (c == '$') len_ = 0;             // nowa ramka - porzucamy resztki
  if (len_ < sizeof(buf_) - 1) buf_[len_++] = c;
}

bool GpsService::parseCoord(const char* v, char hemi, double& out) const {
  if (fieldEmpty(v)) return false;
  double raw = atof(v);
  int deg = (int)(raw / 100.0);
  double min = raw - deg * 100.0;
  double dec = deg + min / 60.0;
  if (hemi == 'S' || hemi == 'W') dec = -dec;
  out = dec;
  return true;
}

bool GpsService::parseTimeUtc(const char* hhmmss) {
  if (fieldEmpty(hhmmss) || strlen(hhmmss) < 6) return false;
  int h = (hhmmss[0] - '0') * 10 + (hhmmss[1] - '0');
  int m = (hhmmss[2] - '0') * 10 + (hhmmss[3] - '0');
  int s = (hhmmss[4] - '0') * 10 + (hhmmss[5] - '0');
  if (h < 0 || h > 23 || m < 0 || m > 59 || s < 0 || s > 60) return false;
  fix_.hour = h; fix_.minute = m; fix_.second = s;
  return true;
}

bool GpsService::parseDateDmy(const char* ddmmyy) {
  if (fieldEmpty(ddmmyy) || strlen(ddmmyy) < 6) return false;
  int d = (ddmmyy[0] - '0') * 10 + (ddmmyy[1] - '0');
  int mo = (ddmmyy[2] - '0') * 10 + (ddmmyy[3] - '0');
  int y = (ddmmyy[4] - '0') * 10 + (ddmmyy[5] - '0');
  if (d < 1 || d > 31 || mo < 1 || mo > 12) return false;
  fix_.day = d; fix_.month = mo; fix_.year = 2000 + y;
  return true;
}

void GpsService::parseSentence(char* s) {
  if (s[0] != '$') return;
  if (!nmeaChecksumOk(s)) {
    fix_.errors++;
    return;
  }
  fix_.sentences++;
  fix_.lastSentenceMs = millis();

  char* f[24] = {0};
  int n = splitFields(s + 1, f, 24);   // pomijamy '$'
  if (n < 2) return;

  const char* type = f[0];
  size_t tl = strlen(type);
  if (tl < 3) return;
  const char* kind = type + tl - 3;    // GGA / RMC / GSA / GSV / ZDA

  if (strcmp(kind, "GGA") == 0 && n >= 10) {
    if (parseTimeUtc(f[1])) {
      double lat = 0, lon = 0;
      bool okLat = parseCoord(f[2], f[3] && *f[3] ? *f[3] : 'N', lat);
      bool okLon = parseCoord(f[4], f[5] && *f[5] ? *f[5] : 'E', lon);
      fix_.fixQuality = fieldEmpty(f[6]) ? 0 : atoi(f[6]);
      fix_.sats = fieldEmpty(f[7]) ? 0 : atoi(f[7]);
      fix_.hdop = fieldEmpty(f[8]) ? NAN : atof(f[8]);
      fix_.altitudeM = fieldEmpty(f[9]) ? NAN : atof(f[9]);
      if (n >= 12 && !fieldEmpty(f[11])) fix_.geoidM = atof(f[11]);
      if (okLat && okLon && fix_.fixQuality > 0) {
        fix_.lat = lat;
        fix_.lon = lon;
        fix_.valid = true;
        fix_.lastFixMs = millis();
      }
      if (fix_.year > 2000) fix_.timeValid = true;
      maybeSyncClock();
    }
  } else if (strcmp(kind, "RMC") == 0 && n >= 10) {
    bool active = f[2] && (*f[2] == 'A' || *f[2] == 'D');
    parseTimeUtc(f[1]);
    parseDateDmy(f[9]);
    double lat = 0, lon = 0;
    bool okLat = parseCoord(f[3], f[4] && *f[4] ? *f[4] : 'N', lat);
    bool okLon = parseCoord(f[5], f[6] && *f[6] ? *f[6] : 'E', lon);
    if (!fieldEmpty(f[7])) fix_.speedKmh = atof(f[7]) * 1.852f;   // węzły -> km/h
    if (!fieldEmpty(f[8])) fix_.courseDeg = atof(f[8]);
    if (active && okLat && okLon) {
      fix_.lat = lat;
      fix_.lon = lon;
      fix_.valid = true;
      fix_.lastFixMs = millis();
    }
    if (fix_.year > 2000 && fix_.timeValid) fix_.timeValid = true;
    maybeSyncClock();
  } else if (strcmp(kind, "GSA") == 0 && n >= 3) {
    if (!fieldEmpty(f[2])) fix_.fixType = atoi(f[2]);
    if (n >= 17 && !fieldEmpty(f[16])) fix_.hdop = atof(f[16]);
  } else if (strcmp(kind, "GSV") == 0 && n >= 4) {
    // pole 3 = liczba satelitów w zasięgu
    if (!fieldEmpty(f[3])) {
      int v = atoi(f[3]);
      if (v >= 0 && v <= 64) fix_.satsView = v;
    }
  } else if (strcmp(kind, "ZDA") == 0 && n >= 5) {
    parseTimeUtc(f[1]);
    if (!fieldEmpty(f[2])) fix_.day = atoi(f[2]);
    if (!fieldEmpty(f[3])) fix_.month = atoi(f[3]);
    if (!fieldEmpty(f[4])) fix_.year = atoi(f[4]);
    if (fix_.year > 2000) fix_.timeValid = true;
    maybeSyncClock();
  }
}

void GpsService::maybeSyncClock() {
  if (clockSynced_ || !fix_.timeValid || fix_.year < 2020) return;
  if (!config.gpsTimeSync()) return;
  // Czekamy na co najmniej 30 s ruchu, żeby mieć pewność, że data jest ustalona
  if (millis() < 30000UL) return;
  if (millis() - lastSyncTryMs_ < 5000UL && lastSyncTryMs_ != 0) return;
  lastSyncTryMs_ = millis();

  // Korekta tylko gdy zegar naprawdę odstaje (> 60 s) - nie ruszamy NTP.
  time_t sys = time(nullptr);
  time_t g = utcEpoch();
  if (g == 0) return;
  long diff = (long)(g - sys);
  if (diff < 0) diff = -diff;
  if (diff < 60) { clockSynced_ = true; return; }

  struct timeval tv = { .tv_sec = g, .tv_usec = 0 };
  settimeofday(&tv, nullptr);
  clockSynced_ = true;
  LOG_I("GPS: zegar ustawiony z GPS (%02d.%02d.%04d %02d:%02d:%02d UTC)",
        fix_.day, fix_.month, fix_.year, fix_.hour, fix_.minute, fix_.second);
}

time_t GpsService::utcEpoch() const {
  if (!fix_.timeValid || fix_.year < 2020) return 0;
  // Dni od 1.1.1970 dla daty gregoriańskiej (algorytm Hinnanta) - odpowiednik
  // timegm(), którego w ESP-IDF/newlib nie ma.
  int y = fix_.year;
  int m = fix_.month;
  int d = fix_.day;
  y -= m <= 2;
  long era = (y >= 0 ? y : y - 399) / 400;
  unsigned yoe = (unsigned)(y - era * 400);
  unsigned doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
  unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  long days = era * 146097L + (long)doe - 719468L;

  long long secs = (long long)days * 86400LL + fix_.hour * 3600LL + fix_.minute * 60LL + fix_.second;
  return secs > 0 ? (time_t)secs : 0;
}

bool GpsService::hasFix() const {
  if (!enabled_ || !fix_.valid) return false;
  return (millis() - fix_.lastFixMs) < 10000UL;
}

unsigned long GpsService::ageS() const {
  if (!enabled_ || fix_.lastSentenceMs == 0) return 999;
  unsigned long s = (millis() - fix_.lastSentenceMs) / 1000UL;
  return s > 998 ? 998 : s;
}

String GpsService::json() const {
  String j = "{";
  j += "\"enabled\":";
  j += enabled_ ? "true" : "false";
  j += ",\"baud\":" + String((unsigned long)baud_);
  j += ",\"present\":";
  j += present() ? "true" : "false";
  j += ",\"fix\":";
  j += hasFix() ? "true" : "false";
  j += ",\"age\":" + String(ageS());
  j += ",\"sentences\":" + String(fix_.sentences);
  j += ",\"errors\":" + String(fix_.errors);
  j += ",\"sats\":" + String(fix_.sats);
  j += ",\"sats_view\":" + String(fix_.satsView);
  j += ",\"fix_quality\":" + String(fix_.fixQuality);
  j += ",\"fix_type\":" + String(fix_.fixType);
  if (hasFix()) {
    j += ",\"lat\":" + String(fix_.lat, 6);
    j += ",\"lon\":" + String(fix_.lon, 6);
  } else {
    j += ",\"lat\":null,\"lon\":null";
  }
  if (isfinite(fix_.altitudeM))  j += ",\"alt\":"  + String(fix_.altitudeM, 1);  else j += ",\"alt\":null";
  if (isfinite(fix_.hdop))       j += ",\"hdop\":" + String(fix_.hdop, 1);       else j += ",\"hdop\":null";
  if (isfinite(fix_.speedKmh))   j += ",\"speed\":" + String(fix_.speedKmh, 1);  else j += ",\"speed\":null";
  if (isfinite(fix_.courseDeg))  j += ",\"course\":" + String(fix_.courseDeg, 0); else j += ",\"course\":null";
  j += ",\"time_valid\":";
  j += fix_.timeValid ? "true" : "false";
  if (fix_.timeValid) {
    char t[32];
    snprintf(t, sizeof(t), "%04d-%02d-%02d %02d:%02d:%02d",
             fix_.year, fix_.month, fix_.day, fix_.hour, fix_.minute, fix_.second);
    j += ",\"utc\":\"" + String(t) + "\"";
  }
  j += "}";
  return j;
}
