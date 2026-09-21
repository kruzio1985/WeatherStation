/* =============================================================================
 * Stacja Pogody - stacja meteorologiczna na ESP32-S3 WROOM-1 (N16R8)
 * Copyright (c) 2026 kruzio1985 - https://github.com/kruzio1985
 * Licencja: PolyForm Noncommercial License 1.0.0 (plik LICENSE)
 * Użytek niekomercyjny. Kontakt: kruzio1985@users.noreply.github.com
 * =============================================================================
 */
#include "openmeteo.h"
#include "config.h"
#include "syslog.h"
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <math.h>
#include <string.h>

OpenMeteoService openMeteo;

namespace {

const char* wxText(int code) {
  switch (code) {
    case 0:  return "Bezchmurnie";
    case 1:  return "Przeważnie słonecznie";
    case 2:  return "Częściowe zachmurzenie";
    case 3:  return "Pochmurno";
    case 45:
    case 48: return "Mgła";
    case 51:
    case 53:
    case 55: return "Mżawka";
    case 56:
    case 57: return "Mżawka marznąca";
    case 61: return "Słaby deszcz";
    case 63: return "Deszcz";
    case 65: return "Intensywny deszcz";
    case 66:
    case 67: return "Deszcz marznący";
    case 71: return "Słaby śnieg";
    case 73: return "Śnieg";
    case 75: return "Intensywny śnieg";
    case 77: return "Ziarna śniegu";
    case 80: return "Przelotne opady";
    case 81: return "Przelotny deszcz";
    case 82: return "Ulewny deszcz";
    case 85:
    case 86: return "Przelotny śnieg";
    case 95: return "Burza";
    case 96:
    case 99: return "Burza z gradem";
    default: return "";
  }
}

// Skopiuj tekst do bufora o podanej pojemności (zawsze zakończony '\0').
void copyStr(char* dst, size_t cap, const char* src) {
  if (!src) { dst[0] = 0; return; }
  strncpy(dst, src, cap - 1);
  dst[cap - 1] = 0;
}

// ISO 8601 "YYYY-MM-DDTHH:MM" -> sama godzina "HH:MM" (od pozycji po 'T').
void copyHhmm(char* dst, size_t cap, const char* src) {
  if (!src) { dst[0] = 0; return; }
  const char* t = strchr(src, 'T');
  if (t) src = t + 1;
  copyStr(dst, cap, src);
}

float arrFloat(JsonArrayConst a, size_t i) {
  if (!a || i >= a.size()) return NAN;
  JsonVariantConst v = a[i];
  if (v.isNull()) return NAN;
  if (v.is<float>() || v.is<int>()) return v.as<float>();
  return NAN;
}

} // namespace

bool OpenMeteoService::enabled() {
  return config.extraF("om_on", 1.0f) >= 0.5f;
}

void OpenMeteoService::setEnabled(bool on) {
  config.setExtraF("om_on", on ? 1.0f : 0.0f);
}

uint32_t OpenMeteoService::intervalMin() {
  float m = config.extraF("om_int", 60.0f);
  if (m < 10) m = 10;
  if (m > 720) m = 720;
  return (uint32_t)m;
}

void OpenMeteoService::setIntervalMin(uint32_t minutes) {
  if (minutes < 10) minutes = 10;
  if (minutes > 720) minutes = 720;
  config.setExtraF("om_int", (float)minutes);
}

bool OpenMeteoService::applyJson(const char* json, size_t len) {
  JsonDocument nd;
  DeserializationError err = deserializeJson(nd, json, len);
  if (err) {
    LOG_W("Open-Meteo: błędny JSON ustawień (%s)", err.c_str());
    return false;
  }
  if (nd["enabled"].is<bool>()) setEnabled(nd["enabled"].as<bool>());
  if (nd["interval_min"].is<float>() || nd["interval_min"].is<int>()) {
    setIntervalMin((uint32_t)(nd["interval_min"] | 60));
  }
  bool saved = config.save();
  LOG_I("Open-Meteo: zapis ustawień (enabled=%d, interval=%u min, save=%d)",
        (int)enabled(), (unsigned)intervalMin(), (int)saved);
  return saved;
}

bool OpenMeteoService::fetch() {
  double lat = config.latitude();
  double lon = config.longitude();
  if (fabs(lat) < 0.001 && fabs(lon) < 0.001) {
    error_ = "Brak współrzędnych stacji - ustaw szerokość i długość geograficzną";
    LOG_W("Open-Meteo: %s", error_.c_str());
    return false;
  }

  char url[512];
  snprintf(url, sizeof(url),
           "https://api.open-meteo.com/v1/forecast"
           "?latitude=%.5f&longitude=%.5f&timezone=auto&forecast_days=%d"
           "&daily=temperature_2m_max,temperature_2m_min,precipitation_sum,"
           "precipitation_probability_max,windspeed_10m_max,weathercode,sunrise,sunset"
           "&current=temperature_2m,relative_humidity_2m,apparent_temperature,"
           "is_day,weathercode,windspeed_10m",
           lat, lon, OM_DAYS);

  WiFiClientSecure secure;
  secure.setInsecure();
  HTTPClient http;
  if (!http.begin(secure, url)) {
    error_ = "Nie udało się otworzyć połączenia z api.open-meteo.com";
    LOG_W("Open-Meteo: %s", error_.c_str());
    return false;
  }

  http.setConnectTimeout(8000);
  http.setTimeout(12000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setUserAgent("StacjaPogody/" FW_VERSION);

  int code = http.GET();
  if (code != 200) {
    error_ = String("HTTP ") + String(code) + " (" + http.errorToString(code) + ")";
    LOG_W("Open-Meteo: błąd pobierania: %s", error_.c_str());
    http.end();
    return false;
  }

  String payload = http.getString();
  http.end();

  PsramAllocator alloc;
  JsonDocument d(&alloc);
  DeserializationError err = deserializeJson(d, payload);
  if (err) {
    error_ = String("Błędna odpowiedź serwisu: ") + err.c_str();
    LOG_W("Open-Meteo: %s", error_.c_str());
    return false;
  }

  JsonObjectConst daily = d["daily"];
  if (!daily) {
    error_ = "Brak sekcji daily w odpowiedzi serwisu";
    LOG_W("Open-Meteo: %s", error_.c_str());
    return false;
  }

  JsonArrayConst time = daily["time"].as<JsonArrayConst>();
  JsonArrayConst tmax = daily["temperature_2m_max"].as<JsonArrayConst>();
  JsonArrayConst tmin = daily["temperature_2m_min"].as<JsonArrayConst>();
  JsonArrayConst precip = daily["precipitation_sum"].as<JsonArrayConst>();
  JsonArrayConst precipProb = daily["precipitation_probability_max"].as<JsonArrayConst>();
  JsonArrayConst wind = daily["windspeed_10m_max"].as<JsonArrayConst>();
  JsonArrayConst codeArr = daily["weathercode"].as<JsonArrayConst>();
  JsonArrayConst sunrise = daily["sunrise"].as<JsonArrayConst>();
  JsonArrayConst sunset = daily["sunset"].as<JsonArrayConst>();

  if (!time || time.size() == 0) {
    error_ = "Pusta prognoza w odpowiedzi serwisu";
    LOG_W("Open-Meteo: %s", error_.c_str());
    return false;
  }

  OmDay tmp[OM_DAYS];
  int n = 0;
  size_t cnt = time.size();
  if (cnt > OM_DAYS) cnt = OM_DAYS;
  for (size_t i = 0; i < cnt; i++) {
    OmDay& day = tmp[n];
    copyStr(day.date, sizeof(day.date), time[i].as<const char*>());
    day.tmin = arrFloat(tmin, i);
    day.tmax = arrFloat(tmax, i);
    day.precip = arrFloat(precip, i);
    day.precipProb = arrFloat(precipProb, i);
    day.wind = arrFloat(wind, i);
    day.code = (codeArr && i < codeArr.size() && !codeArr[i].isNull()) ? codeArr[i].as<int>() : -1;
    if (sunrise && i < sunrise.size()) copyHhmm(day.sunrise, sizeof(day.sunrise), sunrise[i].as<const char*>());
    else day.sunrise[0] = 0;
    if (sunset && i < sunset.size()) copyHhmm(day.sunset, sizeof(day.sunset), sunset[i].as<const char*>());
    else day.sunset[0] = 0;
    n++;
  }

  OmCurrent cur;
  JsonObjectConst c = d["current"];
  if (c) {
    cur.valid = true;
    cur.temp = c["temperature_2m"].isNull() ? NAN : c["temperature_2m"].as<float>();
    cur.hum  = c["relative_humidity_2m"].isNull() ? NAN : c["relative_humidity_2m"].as<float>();
    cur.app  = c["apparent_temperature"].isNull() ? NAN : c["apparent_temperature"].as<float>();
    cur.wind = c["windspeed_10m"].isNull() ? NAN : c["windspeed_10m"].as<float>();
    cur.code = c["weathercode"].isNull() ? -1 : c["weathercode"].as<int>();
    cur.isDay = c["is_day"].isNull() ? true : (c["is_day"].as<int>() != 0);
  }

  if (mutex_) xSemaphoreTake(mutex_, portMAX_DELAY);
  for (int i = 0; i < n; i++) days_[i] = tmp[i];
  dayCount_ = n;
  current_ = cur;
  valid_ = true;
  fetchedAtMs_ = millis();
  error_ = "";
  if (mutex_) xSemaphoreGive(mutex_);

  LOG_I("Open-Meteo: pobrano prognozę na %d dni (%.2f, %.2f)", n, lat, lon);
  return true;
}

void OpenMeteoService::run() {
  unsigned long lastFetch = 0;
  for (;;) {
    const bool forced = requestNow_;
    if (forced) requestNow_ = false;

    const unsigned long intervalMs = intervalMin() * 60UL * 1000UL;
    const bool due = (lastFetch == 0) || (millis() - lastFetch >= intervalMs);

    if ((forced || due) && WiFi.status() == WL_CONNECTED) {
      if (forced || enabled()) {
        if (fetch()) lastFetch = millis();
      }
    } else if (forced && WiFi.status() != WL_CONNECTED) {
      if (mutex_) xSemaphoreTake(mutex_, portMAX_DELAY);
      error_ = "Brak połączenia z Wi-Fi";
      if (mutex_) xSemaphoreGive(mutex_);
      LOG_W("Open-Meteo: %s", error_.c_str());
    }

    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

void OpenMeteoService::taskEntry(void* arg) {
  ((OpenMeteoService*)arg)->run();
  vTaskDelete(nullptr);
}

void OpenMeteoService::begin() {
  mutex_ = xSemaphoreCreateMutex();
  xTaskCreatePinnedToCore(taskEntry, "openmeteo", 12288, this, 1, nullptr, 0);
  LOG_I("Open-Meteo: zadanie wystartowało (interwał %u min)", (unsigned)intervalMin());
}

String OpenMeteoService::json() {
  PsramAllocator alloc;
  JsonDocument d(&alloc);

  d["enabled"] = enabled();
  d["interval_min"] = intervalMin();
  d["wifi"] = (WiFi.status() == WL_CONNECTED);
  d["lat"] = config.latitude();
  d["lon"] = config.longitude();

  if (mutex_) xSemaphoreTake(mutex_, portMAX_DELAY);
  d["valid"] = valid_;
  d["count"] = dayCount_;
  d["updated_ms"] = fetchedAtMs_ ? (unsigned long)(millis() - fetchedAtMs_) : 0UL;
  if (error_.length()) d["error"] = error_; else d["error"] = nullptr;

  JsonObject cur = d["current"].to<JsonObject>();
  cur["valid"] = current_.valid;
  if (current_.valid) {
    cur["temp"] = current_.temp;
    cur["hum"] = current_.hum;
    cur["app"] = current_.app;
    cur["wind"] = current_.wind;
    cur["code"] = current_.code;
    cur["is_day"] = current_.isDay;
    cur["text"] = wxText(current_.code);
  }

  JsonArray arr = d["days"].to<JsonArray>();
  for (int i = 0; i < dayCount_; i++) {
    JsonObject o = arr.add<JsonObject>();
    o["date"] = days_[i].date;
    o["tmin"] = days_[i].tmin;
    o["tmax"] = days_[i].tmax;
    o["precip"] = days_[i].precip;
    o["precip_prob"] = days_[i].precipProb;
    o["wind"] = days_[i].wind;
    o["code"] = days_[i].code;
    o["text"] = wxText(days_[i].code);
    if (days_[i].sunrise[0]) o["sunrise"] = days_[i].sunrise; else o["sunrise"] = nullptr;
    if (days_[i].sunset[0])  o["sunset"] = days_[i].sunset;  else o["sunset"] = nullptr;
  }
  if (mutex_) xSemaphoreGive(mutex_);

  String s;
  serializeJson(d, s);
  return s;
}
