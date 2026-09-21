/* =============================================================================
 * Stacja Pogody - stacja meteorologiczna na ESP32-S3 WROOM-1 (N16R8)
 * Copyright (c) 2026 kruzio1985 - https://github.com/kruzio1985
 * Licencja: Stacja Pogody Non-Commercial License 1.0 (plik LICENSE)
 * Użytek niekomercyjny. Kontakt: kruzio1985@users.noreply.github.com
 * =============================================================================
 */
#include "astro.h"
#include "config.h"
#include "gps.h"
#include "syslog.h"
#include <math.h>
#include <string.h>

AstroService astro;

static const double PI_D = 3.14159265358979323846;
static const double DEG = PI_D / 180.0;

static inline double norm360(double d) {
  d = fmod(d, 360.0);
  if (d < 0) d += 360.0;
  return d;
}
static inline double norm24h(double minutes) {
  minutes = fmod(minutes, 1440.0);
  if (minutes < 0) minutes += 1440.0;
  return minutes;
}
static inline double rad(double d) { return d * DEG; }
static inline double deg(double r) { return r / DEG; }

static double julianDay(time_t t) { return (double)t / 86400.0 + 2440587.5; }

int AstroService::localTzOffsetMin() {
  time_t now = time(nullptr);
  struct tm lt, gt;
  localtime_r(&now, &lt);
  gmtime_r(&now, &gt);
  return (int)((mktime(&lt) - mktime(&gt)) / 60);
}

// ------------------------------------------------------------
//  Słońce: parametry orbity i równanie czasu (algorytm NOAA)
// ------------------------------------------------------------
struct SunTerms {
  double decl;     // deklinacja (°)
  double eotMin;   // równanie czasu (minuty)
  double lonEcl;   // długość ekliptyczna (°)
};

static SunTerms sunTerms(double jd) {
  double T = (jd - 2451545.0) / 36525.0;
  double L0 = norm360(280.46646 + T * (36000.76983 + T * 0.0003032));
  double M  = norm360(357.52911 + T * (35999.05029 - 0.0001537 * T));
  double e  = 0.016708634 - T * (0.000042037 + 0.0000001267 * T);
  double C  = sin(rad(M)) * (1.914602 - T * (0.004817 + 0.000014 * T))
            + sin(rad(2 * M)) * (0.019993 - 0.000101 * T)
            + sin(rad(3 * M)) * 0.000289;
  double trueLong = L0 + C;
  double eps0 = 23.0 + (26.0 + (21.448 - T * (46.815 + T * (0.00059 - T * 0.001813))) / 60.0) / 60.0;
  double eps  = eps0 + 0.00256 * cos(rad(125.04 - 1934.136 * T));
  double lambda = trueLong - 0.00569 - 0.00478 * sin(rad(125.04 - 1934.136 * T));

  double y = tan(rad(eps / 2.0));
  y *= y;
  double eot = 4.0 * deg(y * sin(rad(2 * L0)) - 2 * e * sin(rad(M))
                         + 4 * e * y * sin(rad(M)) * cos(rad(2 * L0))
                         - 0.5 * y * y * sin(rad(4 * L0))
                         - 1.25 * e * e * sin(rad(2 * M)));

  SunTerms t;
  t.decl   = deg(asin(sin(rad(eps)) * sin(rad(lambda))));
  t.eotMin = eot;
  t.lonEcl = lambda;
  return t;
}

void AstroService::computeSun(double lat, double lonEast, time_t utc, int tzMin, SunInfo& out) {
  out = SunInfo();

  // Południe lokalne bieżącej doby (dla wschodu/zachodu liczymy w południe,
  // potem poprawiamy deklinację o godzinę wschodu/zachodu).
  long long localSec = (long long)utc + (long long)tzMin * 60;
  long long dayStartLocal = localSec - (localSec % 86400LL);
  if (localSec < 0 && localSec % 86400LL != 0) dayStartLocal -= 86400LL;
  time_t utcNoon = (time_t)(dayStartLocal + 43200LL - (long long)tzMin * 60);

  SunTerms noon = sunTerms(julianDay(utcNoon));

  double cosH = (cos(rad(90.8333)) - sin(rad(lat)) * sin(rad(noon.decl)))
              / (cos(rad(lat)) * cos(rad(noon.decl)));
  double cosH6 = (cos(rad(96.0)) - sin(rad(lat)) * sin(rad(noon.decl)))
               / (cos(rad(lat)) * cos(rad(noon.decl)));

  out.declinationDeg = (float)noon.decl;
  out.maxElevationDeg = (float)(90.0 - fabs(lat - noon.decl));
  if (out.maxElevationDeg > 90.0f) out.maxElevationDeg = (float)(180.0 - out.maxElevationDeg);

  double noonMinLocal = norm24h(720.0 + 4.0 * lonEast - noon.eotMin + tzMin);
  out.solarNoonMin = (float)noonMinLocal;

  if (cosH > 1.0) {
    out.polarNight = true;
    out.dayLengthH = 0.0f;
  } else if (cosH < -1.0) {
    out.polarDay = true;
    out.dayLengthH = 24.0f;
  } else {
    double H = deg(acos(cosH));
    // poprawka: deklinacja w chwili wschodu/zachodu
    double riseUtcMin = 720.0 + 4.0 * lonEast - 4.0 * H - noon.eotMin;
    double setUtcMin  = 720.0 + 4.0 * lonEast + 4.0 * H - noon.eotMin;
    for (int i = 0; i < 2; i++) {
      time_t riseUtc = (time_t)(dayStartLocal - (long long)tzMin * 60 + (long long)(riseUtcMin * 60.0));
      SunTerms rs = sunTerms(julianDay(riseUtc));
      double cH = (cos(rad(90.8333)) - sin(rad(lat)) * sin(rad(rs.decl)))
                / (cos(rad(lat)) * cos(rad(rs.decl)));
      if (cH < -1.0 || cH > 1.0) break;
      double hh = deg(acos(cH));
      riseUtcMin = 720.0 + 4.0 * lonEast - 4.0 * hh - rs.eotMin;
      setUtcMin  = 720.0 + 4.0 * lonEast + 4.0 * hh - rs.eotMin;
    }
    out.sunriseMin = (float)norm24h(riseUtcMin + tzMin);
    out.sunsetMin  = (float)norm24h(setUtcMin + tzMin);
    out.dayLengthH = (float)(8.0 * H / 60.0);
  }

  if (cosH6 > 1.0) {
    out.civilDawnMin = NAN;
    out.civilDuskMin = NAN;
  } else if (cosH6 < -1.0) {
    out.civilDawnMin = 0.0f;
    out.civilDuskMin = 1440.0f;
  } else {
    double H6 = deg(acos(cosH6));
    out.civilDawnMin = (float)norm24h(720.0 + 4.0 * lonEast - 4.0 * H6 - noon.eotMin + tzMin);
    out.civilDuskMin = (float)norm24h(720.0 + 4.0 * lonEast + 4.0 * H6 - noon.eotMin + tzMin);
  }

  // Bieżąca pozycja Słońca
  SunTerms now = sunTerms(julianDay(utc));
  double utcMinutes = (double)(utc % 86400) / 60.0;
  double tst = utcMinutes + now.eotMin + 4.0 * lonEast;
  double ha = rad(tst / 4.0 - 180.0);
  double sinAlt = sin(rad(lat)) * sin(rad(now.decl)) + cos(rad(lat)) * cos(rad(now.decl)) * cos(ha);
  if (sinAlt > 1.0) sinAlt = 1.0;
  if (sinAlt < -1.0) sinAlt = -1.0;
  out.elevationDeg = (float)deg(asin(sinAlt));
  double az = deg(atan2(sin(ha), cos(ha) * sin(rad(lat)) - tan(rad(now.decl)) * cos(rad(lat)))) + 180.0;
  out.azimuthDeg = (float)norm360(az);
  out.valid = true;
}

// ------------------------------------------------------------
//  Księżyc: uproszczony model Meeusa
// ------------------------------------------------------------
struct MoonPos {
  double ra, dec, alt, az, distKm, lonEclDeg, latEclDeg;
};

static MoonPos moonPosition(double jd, double lat, double lonEast) {
  double d = jd - 2451545.0;
  double L  = norm360(218.316 + 13.176396 * d);
  double M  = norm360(357.529 + 0.98560028 * d);
  double Mp = norm360(134.963 + 13.064993 * d);
  double F  = norm360(93.272 + 13.229350 * d);

  double lonEcl = norm360(L + 6.289 * sin(rad(Mp)));
  double latEcl = 5.128 * sin(rad(F));
  double dist   = 385001.0 - 20905.0 * cos(rad(Mp));

  double eps = rad(23.4397);
  double le = rad(lonEcl), be = rad(latEcl);
  double ra = atan2(sin(le) * cos(eps) - tan(be) * sin(eps), cos(le));
  double dec = asin(sin(be) * cos(eps) + cos(be) * sin(eps) * sin(le));

  double lst = rad(norm360(280.16 + 360.985623 * d + lonEast));
  double H = lst - ra;

  MoonPos p;
  p.ra = ra; p.dec = dec; p.distKm = dist;
  p.lonEclDeg = lonEcl; p.latEclDeg = latEcl;
  p.alt = asin(sin(rad(lat)) * sin(dec) + cos(rad(lat)) * cos(dec) * cos(H));
  p.az  = norm360(deg(atan2(sin(H), cos(H) * sin(rad(lat)) - tan(dec) * cos(rad(lat)))) + 180.0);
  return p;
}

static const char* phaseKeyFor(double phase) {
  if (phase < 0.02 || phase >= 0.98) return "new";
  if (phase < 0.23) return "waxing_crescent";
  if (phase < 0.27) return "first_quarter";
  if (phase < 0.48) return "waxing_gibbous";
  if (phase < 0.52) return "full";
  if (phase < 0.73) return "waning_gibbous";
  if (phase < 0.77) return "last_quarter";
  return "waning_crescent";
}

void AstroService::computeMoon(double lat, double lonEast, time_t utc, int tzMin, MoonInfo& out) {
  out = MoonInfo();

  double jd = julianDay(utc);
  MoonPos p = moonPosition(jd, lat, lonEast);
  SunTerms sun = sunTerms(jd);

  out.altitudeDeg = (float)deg(p.alt);
  out.azimuthDeg  = (float)p.az;
  out.distanceKm  = (float)p.distKm;

  double elong = deg(acos(cos(rad(p.lonEclDeg - sun.lonEcl)) * cos(rad(p.latEclDeg))));
  double phaseAngle = 180.0 - elong;
  out.illumination = (float)((1.0 + cos(rad(phaseAngle))) / 2.0);
  out.phase = (float)(norm360(p.lonEclDeg - sun.lonEcl) / 360.0);
  out.ageDays = (float)(out.phase * 29.530588853);
  out.phaseKey = phaseKeyFor(out.phase);

  // Wschód/zachód: przeszukanie doby lokalnej co 5 minut i przecięcie
  // wysokości 0,125° (refrakcja + paralaksa - wystarczające przybliżenie).
  long long localSec = (long long)utc + (long long)tzMin * 60;
  long long dayStartLocal = localSec - (localSec % 86400LL);
  if (localSec < 0 && localSec % 86400LL != 0) dayStartLocal -= 86400LL;
  time_t baseUtc = (time_t)(dayStartLocal - (long long)tzMin * 60);

  const double limit = 0.125;
  double prev = deg(moonPosition(julianDay(baseUtc), lat, lonEast).alt);
  bool riseFound = false, setFound = false;
  for (int m = 5; m <= 1440; m += 5) {
    double h = deg(moonPosition(julianDay(baseUtc + m * 60), lat, lonEast).alt);
    if (!riseFound && prev < limit && h >= limit) {
      double frac = (limit - prev) / (h - prev);
      out.riseMin = (float)(m - 5 + frac * 5.0);
      riseFound = true;
    } else if (!setFound && prev >= limit && h < limit) {
      double frac = (prev - limit) / (prev - h);
      out.setMin = (float)(m - 5 + frac * 5.0);
      setFound = true;
    }
    prev = h;
  }
  out.valid = true;
}

// ------------------------------------------------------------
//  Serwis: skąd bierzemy położenie
// ------------------------------------------------------------
void AstroService::update(double latCfg, double lonCfg, time_t utcNow, int tzMin) {
  double lat = latCfg, lon = lonCfg;
  fromGps_ = false;

  if (gps.hasFix() && fabs(gps.raw().lat) <= 90.0 && fabs(gps.raw().lon) <= 180.0
      && (gps.raw().lat != 0.0 || gps.raw().lon != 0.0)) {
    lat = gps.raw().lat;
    lon = gps.raw().lon;
    fromGps_ = true;
  }

  lat_ = lat;
  lon_ = lon;

  bool okLat = fabs(lat) > 0.0001 && fabs(lat) <= 90.0;
  bool okLon = fabs(lon) > 0.0001 && fabs(lon) <= 180.0;
  if (!okLat || !okLon) {
    valid_ = false;
    sun_ = SunInfo();
    moon_ = MoonInfo();
    return;
  }

  computeSun(lat, lon, utcNow, tzMin, sun_);
  computeMoon(lat, lon, utcNow, tzMin, moon_);
  valid_ = true;
}

static void addMin(String& j, const char* key, float v, int dec) {
  j += ",\"";
  j += key;
  j += "\":";
  if (isnan(v)) j += "null"; else j += String(v, dec);
}

String AstroService::json() const {
  String j = "{\"valid\":";
  j += valid_ ? "true" : "false";
  j += ",\"source\":\"";
  j += fromGps_ ? "gps" : "manual";
  j += "\",\"lat\":" + String(lat_, 5) + ",\"lon\":" + String(lon_, 5);

  j += ",\"sun\":{";
  j += "\"valid\":";
  j += sun_.valid ? "true" : "false";
  j += ",\"polar_day\":";
  j += sun_.polarDay ? "true" : "false";
  j += ",\"polar_night\":";
  j += sun_.polarNight ? "true" : "false";
  addMin(j, "elevation", sun_.elevationDeg, 1);
  addMin(j, "azimuth", sun_.azimuthDeg, 1);
  addMin(j, "declination", sun_.declinationDeg, 2);
  addMin(j, "max_elevation", sun_.maxElevationDeg, 1);
  addMin(j, "solar_noon", sun_.solarNoonMin, 1);
  addMin(j, "sunrise", sun_.sunriseMin, 1);
  addMin(j, "sunset", sun_.sunsetMin, 1);
  addMin(j, "civil_dawn", sun_.civilDawnMin, 1);
  addMin(j, "civil_dusk", sun_.civilDuskMin, 1);
  addMin(j, "day_length_h", sun_.dayLengthH, 2);
  j += "}";

  j += ",\"moon\":{";
  j += "\"valid\":";
  j += moon_.valid ? "true" : "false";
  j += ",\"phase\":" + String(moon_.phase, 4);
  j += ",\"illumination\":" + String(moon_.illumination, 4);
  j += ",\"age_days\":" + String(moon_.ageDays, 2);
  j += ",\"phase_key\":\"" + String(moon_.phaseKey) + "\"";
  addMin(j, "altitude", moon_.altitudeDeg, 1);
  addMin(j, "azimuth", moon_.azimuthDeg, 1);
  addMin(j, "distance_km", moon_.distanceKm, 0);
  addMin(j, "rise", moon_.riseMin, 1);
  addMin(j, "set", moon_.setMin, 1);
  j += "}}";
  return j;
}
