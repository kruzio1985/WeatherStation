/* =============================================================================
 * Stacja Pogody - stacja meteorologiczna na ESP32-S3 WROOM-1 (N16R8)
 * Copyright (c) 2026 kruzio1985 - https://github.com/kruzio1985
 * Licencja: Stacja Pogody Non-Commercial License 1.0 (plik LICENSE)
 * Użytek niekomercyjny. Kontakt: kruzio1985@users.noreply.github.com
 * =============================================================================
 */
#include "weather_services.h"
#include "sensors.h"
#include "mqtt_client.h"
#include "syslog.h"
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <mbedtls/md5.h>
#include <math.h>
#include <time.h>

WeatherServices weatherServices;

// Kolejność musi być zgodna z SVC_NAMES w config.h
enum { SVC_WU = 0, SVC_PWS, SVC_WINDY, SVC_OWM, SVC_TS, SVC_CUSTOM,
       SVC_AWEKAS, SVC_WCLOUD, SVC_CWOP };

// -------------------------------------------------------------
//  Zbieranie pomiarów do wysłania
// -------------------------------------------------------------
struct WxData {
  float temp = NAN, hum = NAN, press = NAN, wind = NAN, winddir = NAN, gust = NAN;
  float rain = NAN, light = NAN, pm1 = NAN, pm25 = NAN, pm10 = NAN;
  float co2 = NAN, eco2 = NAN, tvoc = NAN;
  String compass;
  String tempSource;   // ID czujnika temperatury użytego jako temperatura zewnętrzna
};

static bool usable(const Channel& c, bool externalOnly) {
  if (!c.enabled || !c.detected || !c.measured || isnan(c.value)) return false;
  if (externalOnly && c.zone != "out") return false;
  return true;
}

static float findVal(const std::vector<Channel>& ch, const char* id, bool externalOnly) {
  for (const auto& c : ch) {
    if (c.id == id) return usable(c, externalOnly) ? c.value : NAN;
  }
  return NAN;
}

// Temperatura dla serwisów pogodowych: użytkownik może wskazać konkretny
// czujnik (services.temp_ch). Bez wskazania bierzemy pierwszy czujnik
// spełniający filtr strefy, a preferujemy zewnętrzne DS18B20.
static float pickTemp(const std::vector<Channel>& ch, bool externalOnly, String& sourceOut) {
  String want = config.svcTop("temp_ch");
  if (want.length()) {
    for (const auto& c : ch) {
      if (c.id == want && usable(c, false)) { sourceOut = c.id; return c.value; }
    }
    // wskazany czujnik nie ma danych - schodzimy do wyboru automatycznego
  }
  // 1) zewnętrzne DS18B20 (najczęściej to prawdziwa temperatura na dworze)
  for (const auto& c : ch) {
    if (c.id.startsWith("ds_") && usable(c, externalOnly)) { sourceOut = c.id; return c.value; }
  }
  // 2) czujnik temperatury na płytce
  for (const auto& c : ch) {
    if (c.id == "temp" && usable(c, externalOnly)) { sourceOut = c.id; return c.value; }
  }
  return NAN;
}

static WxData collect(const std::vector<Channel>& ch) {
  const bool ext = config.svcExternalOnly();
  WxData d;
  d.temp    = pickTemp(ch, ext, d.tempSource);
  d.hum     = findVal(ch, "hum", ext);
  d.press   = findVal(ch, "press", ext);
  d.wind    = findVal(ch, "wind", ext);
  d.gust    = d.wind;                  // brak osobnego czujnika porywów
  d.winddir = findVal(ch, "vane", ext);
  d.rain    = findVal(ch, "rain", ext);
  d.light   = findVal(ch, "light", ext);
  d.pm1     = findVal(ch, "pm1", ext);
  d.pm25    = findVal(ch, "pm25", ext);
  d.pm10    = findVal(ch, "pm10", ext);
  d.co2     = findVal(ch, "co2", ext);
  d.eco2    = findVal(ch, "eco2", ext);
  d.tvoc    = findVal(ch, "tvoc", ext);
  d.compass = sensors.windCompass();
  return d;
}

// -------------------------------------------------------------
//  Konwersje jednostek - serwisy pogodowe używają systemu imperialnego
// -------------------------------------------------------------
static float cToF(float c)     { return c * 9.0f / 5.0f + 32.0f; }
static float kmhToMph(float k) { return k * 0.621371f; }
static float kmhToMs(float k)  { return k / 3.6f; }
static float hpaToInHg(float h){ return h * 0.029530f; }
static float mmToIn(float mm)  { return mm * 0.0393701f; }

// Punkt rosy (wzór Magnusa) - WU i PWSWeather oczekują dewptf
static float dewPointC(float t, float rh) {
  if (isnan(t) || isnan(rh) || rh <= 0.0f || rh > 100.0f) return NAN;
  const float a = 17.62f, b = 243.12f;
  float g = logf(rh / 100.0f) + (a * t) / (b + t);
  return (b * g) / (a - g);
}

// Liczba jako tekst z podaną liczbą miejsc po przecinku. Rzutowanie na
// (double, unsigned int) jest konieczne, bo String ma kilka przeciążonych
// konstruktorów liczbowych i wywołanie String(float, uint8_t) jest niejednoznaczne.
static String fmtVal(float v, uint8_t dec) {
  if (isnan(v)) return String("");
  return String((double)v, (unsigned int)dec);
}

static void addF(String& s, const char* name, float v, uint8_t dec = 2) {
  if (isnan(v)) return;
  s += '&';
  s += name;
  s += '=';
  s += fmtVal(v, dec);
}

// Dane logowania w adresie muszą być zakodowane (np. hasło z "&").
static String urlEncode(const String& in) {
  static const char* hex = "0123456789ABCDEF";
  String out;
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += c;
    } else {
      out += '%';
      out += hex[(c >> 4) & 0x0F];
      out += hex[c & 0x0F];
    }
  }
  return out;
}

// Adres bez danych logowania - do pokazania na stronie i w logach
static String sanitizeUrl(const String& url) {
  int q = url.indexOf('?');
  return q < 0 ? url : url.substring(0, q) + "?...";
}

// MD5 jako hex (mbedTLS jest już w linkerze przez WiFiClientSecure).
// AWEKAS wymaga hasła w postaci MD5, nie w czystej postaci.
static String md5hex(const String& in) {
  unsigned char out[16];
  // API jednoprzebiegowe (mbedtls_md5_ret) - wrapper mbedtls_md5_starts/update
  // jest w tym SDK oznaczone jako przestarzałe i nie jest linkowane.
  mbedtls_md5_ret((const unsigned char*)in.c_str(), in.length(), out);

  static const char* hex = "0123456789abcdef";
  String r;
  r.reserve(32);
  for (int i = 0; i < 16; i++) {
    r += hex[(out[i] >> 4) & 0x0F];
    r += hex[out[i] & 0x0F];
  }
  return r;
}

// Liczba całkowita bez "-nan" dla pól numerycznych (puste, gdy brak pomiaru)
static String fmtInt(float v) {
  if (isnan(v)) return String("");
  return String((long)lroundf(v));
}

// Pozycja w formacie APRS: ddmm.hhN / dddmm.hhE (stopnie, minuty, setne)
static String aprsLat(double lat) {
  char hemi = lat >= 0 ? 'N' : 'S';
  double a = fabs(lat);
  int deg = (int)a;
  double min = (a - deg) * 60.0;
  int mm = (int)min;
  int hh = (int)lroundf((min - mm) * 100.0);
  if (hh >= 100) { mm++; hh -= 100; }
  char b[16];
  snprintf(b, sizeof(b), "%02d%02d.%02d%c", deg, mm, hh, hemi);
  return String(b);
}

static String aprsLon(double lon) {
  char hemi = lon >= 0 ? 'E' : 'W';
  double a = fabs(lon);
  int deg = (int)a;
  double min = (a - deg) * 60.0;
  int mm = (int)min;
  int hh = (int)lroundf((min - mm) * 100.0);
  if (hh >= 100) { mm++; hh -= 100; }
  char b[17];
  snprintf(b, sizeof(b), "%03d%02d.%02d%c", deg, mm, hh, hemi);
  return String(b);
}

// -------------------------------------------------------------
//  Budowanie żądań dla poszczególnych serwisów
// -------------------------------------------------------------
struct Request {
  String url;
  String body;   // niepusty = POST z JSON-em (OpenWeather)
};

// Weather Underground i PWSWeather używają tego samego zestawu parametrów
static void buildWuLike(WxData& d, const String& id, const String& key,
                        bool pws, Request& r) {
  r.url = pws ? "https://www.pwsweather.com/pwsupdate/pwsupdate.php"
              : "http://rtupdate.wunderground.com/weatherstation/updateweatherstation.php";
  r.url += "?ID=" + urlEncode(id);
  r.url += "&PASSWORD=" + urlEncode(key);
  r.url += "&dateutc=now&action=updateraw&softwaretype=StacjaPogody-" FW_VERSION;

  float dew = dewPointC(d.temp, d.hum);
  addF(r.url, "tempf", isnan(d.temp) ? NAN : cToF(d.temp), 1);
  addF(r.url, "humidity", d.hum, 0);
  addF(r.url, "dewptf", isnan(dew) ? NAN : cToF(dew), 1);
  addF(r.url, "winddir", d.winddir, 0);
  addF(r.url, "windspeedmph", isnan(d.wind) ? NAN : kmhToMph(d.wind), 1);
  addF(r.url, "windgustmph", isnan(d.gust) ? NAN : kmhToMph(d.gust), 1);
  addF(r.url, "dailyrainin", isnan(d.rain) ? NAN : mmToIn(d.rain), 2);
  addF(r.url, "baromin", isnan(d.press) ? NAN : hpaToInHg(d.press), 3);
  addF(r.url, "solarradiation", d.light, 0);
}

// Windy PWS API v2 (jednostki metryczne: °C, m/s, Pa, mm)
static void buildWindy(WxData& d, const String& key, Request& r) {
  r.url = "https://stations.windy.com/api/v2/observation/update?id=" + urlEncode(key);
  float dew = dewPointC(d.temp, d.hum);
  addF(r.url, "temp", d.temp, 1);
  addF(r.url, "dewpoint", dew, 1);
  addF(r.url, "humidity", d.hum, 0);
  addF(r.url, "wind", isnan(d.wind) ? NAN : kmhToMs(d.wind), 1);
  addF(r.url, "gust", isnan(d.gust) ? NAN : kmhToMs(d.gust), 1);
  addF(r.url, "winddir", d.winddir, 0);
  addF(r.url, "pressure", isnan(d.press) ? NAN : d.press * 100.0f, 0);
  addF(r.url, "precip", d.rain, 1);
  addF(r.url, "uv", NAN, 0);
}

// OpenWeather Weather Stations API 3.0 - POST z tablicą JSON
static void buildOpenWeather(WxData& d, const String& id, const String& key, Request& r) {
  r.url = "https://api.openweathermap.org/data/3.0/measurements?appid=" + urlEncode(key);

  JsonDocument doc;
  JsonObject o = doc.add<JsonObject>();
  o["station_id"] = id;
  o["dt"] = (uint32_t)time(nullptr);

  if (!isnan(d.temp)) o["temperature"] = d.temp;
  if (!isnan(d.hum)) o["humidity"] = (int)lroundf(d.hum);
  if (!isnan(d.press)) o["pressure"] = d.press * 100.0f;
  if (!isnan(d.wind)) o["wind_speed"] = kmhToMs(d.wind);
  if (!isnan(d.winddir)) o["wind_deg"] = (int)lroundf(d.winddir);
  if (!isnan(d.gust)) o["wind_gust"] = kmhToMs(d.gust);
  serializeJson(doc, r.body);
}

// ThingSpeak - 8 pól (kolejność ustalona, żeby wykresy na ThingSpeak były czytelne)
static void buildThingSpeak(WxData& d, const String& key, Request& r) {
  r.url = "https://api.thingspeak.com/update?api_key=" + urlEncode(key);
  addF(r.url, "field1", d.temp, 1);      // temperatura
  addF(r.url, "field2", d.hum, 1);       // wilgotność
  addF(r.url, "field3", d.press, 1);     // ciśnienie
  addF(r.url, "field4", d.wind, 1);      // wiatr km/h
  addF(r.url, "field5", d.winddir, 0);   // kierunek
  addF(r.url, "field6", d.rain, 1);      // opad
  addF(r.url, "field7", d.pm25, 1);      // PM2.5
  addF(r.url, "field8", d.co2, 0);       // CO2
}

// Własny adres: szablon ze znacznikami {temp}, {hum}, ... - pozwala wysłać dane
// na własny serwer (np. strona firmowa z danymi z czujników).
static void buildCustom(WxData& d, const String& tpl, const String& id, const String& key, Request& r) {
  auto val = [](float v, uint8_t dec) -> String { return fmtVal(v, dec); };
  const float tempF = isnan(d.temp) ? NAN : cToF(d.temp);

  struct Tok { const char* name; String value; };
  Tok toks[] = {
    {"temp", val(d.temp, 2)}, {"tempf", val(tempF, 2)},
    {"hum", val(d.hum, 1)}, {"press", val(d.press, 2)},
    {"wind", val(d.wind, 2)}, {"winddir", val(d.winddir, 0)},
    {"rain", val(d.rain, 2)}, {"light", val(d.light, 0)},
    {"pm1", val(d.pm1, 1)}, {"pm25", val(d.pm25, 1)}, {"pm10", val(d.pm10, 1)},
    {"co2", val(d.co2, 0)}, {"eco2", val(d.eco2, 0)}, {"tvoc", val(d.tvoc, 0)},
    {"compass", d.compass}, {"id", id}, {"key", key},
    {"ts", String((uint32_t)time(nullptr))},
  };

  r.url = tpl;
  for (const Tok& t : toks) {
    String ph = String("{") + t.name + "}";
    r.url.replace(ph, t.value);
  }
}

// WeatherCloud: https://api.weathercloud.net/v01/set?wid=...&key=...
// Wartości są liczbami całkowitymi (temperatura i ciśnienie ×10, wiatr m/s ×10).
static void buildWeatherCloud(WxData& d, const String& id, const String& key, Request& r) {
  auto x10 = [](float v) -> String { return isnan(v) ? String("") : String((long)lroundf(v * 10.0f)); };

  r.url = "https://api.weathercloud.net/v01/set";
  r.url += "?wid=" + urlEncode(id);
  r.url += "&key=" + urlEncode(key);
  if (!isnan(d.temp))  r.url += "&temp=" + x10(d.temp);
  if (!isnan(d.hum))   r.url += "&hum=" + fmtInt(d.hum);
  if (!isnan(d.press)) r.url += "&bar=" + x10(d.press);
  if (!isnan(d.wind))  r.url += "&wspd=" + x10(kmhToMs(d.wind));
  if (!isnan(d.gust))  r.url += "&wspdhi=" + x10(kmhToMs(d.gust));
  if (!isnan(d.winddir)) r.url += "&wdir=" + fmtInt(d.winddir);
  if (!isnan(d.rain))  r.url += "&rain=" + x10(d.rain);
  if (!isnan(d.light)) r.url += "&solarrad=" + fmtInt(d.light);

  // data UTC + identyfikator oprogramowania
  time_t t = time(nullptr);
  struct tm tmUtc;
  gmtime_r(&t, &tmUtc);
  char dt[16];
  snprintf(dt, sizeof(dt), "%04d%02d%02d", tmUtc.tm_year + 1900, tmUtc.tm_mon + 1, tmUtc.tm_mday);
  r.url += "&date=";
  r.url += dt;
  r.url += "&software=StacjaPogody";
}

// AWEKAS: https://data.awekas.at/eingabe_pruefung.php?output=json&val=...
// val = pola rozdzielone średnikami; hasło jako MD5. Brakujące pola zostają puste.
static void buildAwekas(WxData& d, const String& id, const String& key, Request& r) {
  r.url = "https://data.awekas.at/eingabe_pruefung.php?output=json&val=";

  String val;
  val.reserve(160);
  val += urlEncode(id);   // ID stacji
  val += ";";
  val += md5hex(key);     // hasło jako MD5
  val += ";";

  // data i czas lokalny (dd.MM.yyyy;HH:mm)
  time_t t = time(nullptr);
  struct tm tmL;
  localtime_r(&t, &tmL);
  char dt[32];
  snprintf(dt, sizeof(dt), "%02d.%02d.%04d;%02d:%02d",
           tmL.tm_mday, tmL.tm_mon + 1, tmL.tm_year + 1900,
           tmL.tm_hour, tmL.tm_min);
  val += dt;
  val += ";";

  // temp;hum;press;rain;wind;winddir
  auto f0 = [](float v) -> String { return isnan(v) ? String("") : String((long)lroundf(v)); };
  val += f0(d.temp);  val += ";";
  val += f0(d.hum);   val += ";";
  val += f0(d.press); val += ";";
  val += fmtVal(d.rain, 1); val += ";";        // opad (dziś) w mm
  val += fmtVal(kmhToMs(d.wind), 1); val += ";"; // wiatr m/s (AWEKAS przyjmuje m/s)
  val += f0(d.winddir); val += ";";

  // AWEKAS przyjmuje zredukowany zestaw pól - puste pola po prostu pomijamy.
  r.url += val;
}

// CWOP / APRS: nie HTTP, lecz surowe połączenie TCP z cwop.aprs.net:14580.
// Buduje tylko pakiet pogodowy (login i wysyłka w sendCwop).
static String buildCwopPacket(WxData& d, const String& id) {
  // login: user ID pass hasło vers StacjaPogody-x.y
  // pakiet: ID>APRS,TCPIP*:@{ddHHMM}z{lat}/{lon}_{bearing}/{windMPH}mph g{gustMPH}
  //         t{tempF} r{rain0.01in} p{rain24h0.01in} P{rainToday0.01in}
  //         h{hum} b{bar0.1mb} L{solar} e{software}
  time_t t = time(nullptr);
  struct tm tmUtc;
  gmtime_r(&t, &tmUtc);
  char ts[16];
  snprintf(ts, sizeof(ts), "%02d%02d%02d", tmUtc.tm_mday, tmUtc.tm_hour, tmUtc.tm_min);

  String p;
  p.reserve(160);
  p += id;
  p += ">APRS,TCPIP*:@";
  p += ts;
  p += "z";
  p += aprsLat(config.latitude());
  p += "/";
  p += aprsLon(config.longitude());
  p += "_";
  if (!isnan(d.winddir)) p += String((int)lroundf(d.winddir)) + "/"; else p += ".../";
  if (!isnan(d.wind))    p += String((int)lroundf(kmhToMph(d.wind))) + "mph"; else p += "...";
  p += "g";
  if (!isnan(d.gust))    p += String((int)lroundf(kmhToMph(d.gust))); else p += "...";
  p += "t";
  if (!isnan(d.temp))    p += String((int)lroundf(cToF(d.temp))); else p += "...";
  p += "r";
  if (!isnan(d.rain))    p += String((int)lroundf(mmToIn(d.rain) * 100.0f)); else p += "...";
  p += "p...P...h";
  if (!isnan(d.hum)) {
    int h = (int)lroundf(d.hum);
    if (h >= 100) h = 0; else if (h <= 0) h = 1;
    p += String(h);
  } else {
    p += "..";
  }
  p += "b";
  if (!isnan(d.press)) p += String((int)lroundf(d.press * 10.0f)); else p += ".....";
  if (!isnan(d.light)) {
    long s = (long)lroundf(d.light);
    if (s < 1000) { p += "L"; p += String(s); }
    else { p += "l"; p += String(s - 1000); }
  }
  p += "eStacjaPogody";
  return p;
}

// -------------------------------------------------------------
//  Wysyłka po HTTP/HTTPS
// -------------------------------------------------------------
bool WeatherServices::sendOne(uint8_t idx, const String& url, const String& body, Result& res) {
  res.url = sanitizeUrl(url);

  WiFiClientSecure secure;
  WiFiClient plain;
  HTTPClient http;

  const bool https = url.startsWith("https://");
  bool begun = false;
  if (https) {
    // Serwisy pogodowe nie są krytyczne dla bezpieczeństwa stacji, a pinowanie
    // certyfikatów wymagałoby ich aktualizacji przy każdej zmianie klucza w
    // serwisie - dlatego akceptujemy certyfikat bez weryfikacji łańcucha.
    secure.setInsecure();
    begun = http.begin(secure, url);
  } else {
    begun = http.begin(plain, url);
  }
  if (!begun) {
    res.code = 0;
    res.info = "nie udało się otworzyć połączenia";
    res.ok = false;
    return false;
  }

  http.setConnectTimeout(8000);
  http.setTimeout(10000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setUserAgent("StacjaPogody/" FW_VERSION);

  int code = body.length() ? http.POST(body) : http.GET();
  res.code = code;

  if (code > 0) {
    String payload = http.getString();
    payload.trim();
    if (payload.length() > 180) payload = payload.substring(0, 180) + "…";

    // Serwisy zwracają różne potwierdzenia - bierzemy pod uwagę kod HTTP,
    // a dodatkowo odrzucamy znane komunikaty o błędzie w treści odpowiedzi.
    bool bodyBad = payload.indexOf("INVALID") >= 0 ||
                   payload.indexOf("FAILED") >= 0 ||
                   payload.indexOf("not found") >= 0 ||
                   payload.indexOf("Bad Request") >= 0 ||
                   payload.indexOf("Unauthorized") >= 0 ||
                   payload.indexOf("401") == 0 || payload.indexOf("403") == 0;
    res.ok = (code >= 200 && code < 300) && !bodyBad;
    res.info = payload.length() ? payload : String("HTTP ") + String(code);
  } else {
    res.ok = false;
    res.info = String("brak odpowiedzi (") + http.errorToString(code) + ")";
  }

  http.end();
  res.skipped = false;
  res.at = millis();
  (void)idx;
  return res.ok;
}

// CWOP/APRS - surowe TCP (nie HTTP). Logujemy się i wysyłamy jeden pakiet
// pogodowy; serwer odpowiada tekstowo, więc wystarczy, że połączenie
// zadziała i dostaniemy cokolwiek w odpowiedzi.
bool WeatherServices::sendCwop(uint8_t idx, const String& id, const String& pass,
                               const String& packet, Result& res) {
  res.url = "cwop.aprs.net:14580";
  (void)idx;

  WiFiClient c;
  if (!c.connect("cwop.aprs.net", 14580, 10000)) {
    res.code = 0;
    res.info = "nie udało się połączyć z cwop.aprs.net:14580";
    res.ok = false;
    res.skipped = false;
    res.at = millis();
    return false;
  }

  String login = "user " + id + " pass " + pass + " vers StacjaPogody-" FW_VERSION "\r\n";
  c.print(login);
  c.print(packet);
  c.print("\r\n");

  // Czekamy chwilę na odpowiedź serwera (do 5 s), po czym zamykamy.
  unsigned long start = millis();
  String reply;
  while (c.connected() && millis() - start < 5000) {
    if (c.available()) {
      char b = (char)c.read();
      if (reply.length() < 160) reply += b;
    } else {
      delay(10);
    }
  }
  c.stop();

  res.code = 200;
  res.info = reply.length() ? reply : "wysłano (brak odpowiedzi)";
  res.ok = true;
  res.skipped = false;
  res.at = millis();
  return true;
}

void WeatherServices::sendAll(bool forced) {
  const std::vector<Channel> ch = sensors.snapshot();
  WxData data = collect(ch);

  if (isnan(data.temp) && isnan(data.hum) && isnan(data.press) && isnan(data.wind) &&
      isnan(data.rain) && isnan(data.pm25)) {
    LOG_W("Serwisy pogodowe: brak jakichkolwiek potwierdzonych pomiarów - pomijam wysyłkę");
  }

  const bool master = config.svcSectionEnabled() || forced;

  for (uint8_t i = 0; i < SVC_COUNT; i++) {
    const char* name = SVC_NAMES[i];
    Result res;
    res.code = 0;

    String id  = config.svcStr(name, "id");
    String key = config.svcStr(name, "key");
    String url = config.svcStr(name, "url");

    // W trybie "wyślij teraz" (test ze strony) wysyłamy także serwisy
    // wyłączone, o ile mają wpisane dane dostępowe - po to jest ten przycisk.
    const bool active = master && (forced || config.svcEnabled(name));

    Request req;
    String cwopPacket;
    bool ready = false;
    bool tcp = false;   // CWOP idzie po TCP, nie HTTP - nie wołamy sendOne()
    if (active) {
      switch (i) {
        case SVC_WU:  if (id.length() && key.length()) { buildWuLike(data, id, key, false, req); ready = true; } break;
        case SVC_PWS: if (id.length() && key.length()) { buildWuLike(data, id, key, true, req);  ready = true; } break;
        case SVC_WINDY: if (key.length()) { buildWindy(data, key, req); ready = true; } break;
        case SVC_OWM: if (id.length() && key.length()) { buildOpenWeather(data, id, key, req); ready = true; } break;
        case SVC_TS:  if (key.length()) { buildThingSpeak(data, key, req); ready = true; } break;
        case SVC_CUSTOM: if (url.length()) { buildCustom(data, url, id, key, req); ready = true; } break;
        case SVC_AWEKAS: if (id.length() && key.length()) { buildAwekas(data, id, key, req); ready = true; } break;
        case SVC_WCLOUD: if (id.length() && key.length()) { buildWeatherCloud(data, id, key, req); ready = true; } break;
        case SVC_CWOP: if (id.length() && key.length()) { cwopPacket = buildCwopPacket(data, id); tcp = true; ready = true; } break;
      }
    }

    if (!ready) {
      res.skipped = true;
      res.ok = false;
      res.info = active ? "brak danych dostępowych (ID / klucz / adres)"
                        : (master ? "wyłączony" : "sekcja wyłączona");
    } else {
      LOG_I("Serwisy pogodowe: wysyłam do %s", name);
      if (tcp) sendCwop(i, id, key, cwopPacket, res);
      else     sendOne(i, req.url, req.body, res);
      if (res.ok) {
        LOG_I("Serwisy pogodowe: %s - OK (HTTP %d)", name, res.code);
      } else {
        LOG_W("Serwisy pogodowe: %s - błąd: %s", name, res.info.c_str());
      }
    }

    if (mutex_) xSemaphoreTake(mutex_, portMAX_DELAY);
    results_[i] = res;
    if (mutex_) xSemaphoreGive(mutex_);
  }

  lastSend_ = millis();

  // Wynik wysyłki publikujemy też w MQTT - wygodne w Home Assistant
  // i przy diagnostyce bez wchodzenia na stronę stacji.
  String rep = statusJson();
  mqtt.publishTopic(config.mqttPrefix() + "/services/state", rep);
}

void WeatherServices::run() {
  for (;;) {
    const bool forced = requestNow_;
    if (forced) requestNow_ = false;

    const bool due = (lastSend_ == 0) ||
                     (millis() - lastSend_ >= (unsigned long)config.svcInterval() * 1000UL);

    if (forced || due) {
      if (WiFi.status() == WL_CONNECTED) {
        sendAll(forced);
      } else if (forced) {
        LOG_W("Serwisy pogodowe: brak połączenia z Wi-Fi - nie wysłano danych");
        if (mutex_) xSemaphoreTake(mutex_, portMAX_DELAY);
        for (uint8_t i = 0; i < SVC_COUNT; i++) {
          results_[i].skipped = false;
          results_[i].ok = false;
          results_[i].code = 0;
          results_[i].info = "brak połączenia z Wi-Fi";
          results_[i].at = millis();
        }
        if (mutex_) xSemaphoreGive(mutex_);
      }
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

void WeatherServices::taskEntry(void* arg) {
  ((WeatherServices*)arg)->run();
  vTaskDelete(nullptr);
}

void WeatherServices::begin() {
  mutex_ = xSemaphoreCreateMutex();
  // Osobne zadanie z dużym stosem: połączenia HTTPS (mbedTLS) potrzebują
  // kilku kilobajtów stosu, czego nie ma w głównej pętli Arduino.
  xTaskCreatePinnedToCore(taskEntry, "wx_services", 12288, this, 1, nullptr, 0);
  LOG_I("Serwisy pogodowe: zadanie wystartowało (interwał %u s)", (unsigned)config.svcInterval());
}

WeatherServices::Snapshot WeatherServices::lastResult(uint8_t idx) {
  Snapshot s;
  if (idx >= SVC_COUNT) return s;
  if (mutex_) xSemaphoreTake(mutex_, portMAX_DELAY);
  s.ok = results_[idx].ok;
  s.skipped = results_[idx].skipped;
  s.code = results_[idx].code;
  s.info = results_[idx].info;
  s.at = results_[idx].at;
  if (mutex_) xSemaphoreGive(mutex_);
  return s;
}

String WeatherServices::statusJson() {
  PsramAllocator alloc;
  JsonDocument d(&alloc);

  d["enabled"] = config.svcSectionEnabled();
  d["interval"] = config.svcInterval();
  d["external_only"] = config.svcExternalOnly();
  d["temp_ch"] = config.svcTop("temp_ch");
  d["services"] = config.servicesJson();
  d["wifi"] = (WiFi.status() == WL_CONNECTED);

  JsonObject st = d["status"].to<JsonObject>();
  if (mutex_) xSemaphoreTake(mutex_, portMAX_DELAY);
  for (uint8_t i = 0; i < SVC_COUNT; i++) {
    JsonObject o = st[SVC_NAMES[i]].to<JsonObject>();
    o["ok"] = results_[i].ok;
    o["skipped"] = results_[i].skipped;
    o["code"] = results_[i].code;
    o["info"] = results_[i].info;
    o["url"] = results_[i].url;
    o["age_s"] = results_[i].at ? (millis() - results_[i].at) / 1000 : -1;
  }
  if (mutex_) xSemaphoreGive(mutex_);

  String s;
  serializeJson(d, s);
  return s;
}
