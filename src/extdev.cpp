/* =============================================================================
 * Stacja Pogody - urządzenia zewnętrzne odpytywane przez HTTP (implementacja)
 * Copyright (c) 2026 kruzio1985 - https://github.com/kruzio1985
 * Licencja: PolyForm Noncommercial License 1.0.0 (plik LICENSE)
 * Użytek niekomercyjny. Kontakt: kruzio1985@users.noreply.github.com
 * ========================================================================== */
#include "extdev.h"
#include "sensors.h"
#include "nvs_store.h"
#include "syslog.h"
#include <ArduinoJson.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <math.h>

static const char* EV_NS = "extdev";
static const char* EV_KEY = "list";

ExtDevManager extdev;

// Prosty HTTP GET zwracający treść (max 8 KB). Krótkie timeouty, żeby
// NIE przekroczyć limitu 5 s watchdoga zadania (connect + read < 5 s).
static bool httpGetJson(const String& url, String& body) {
  HTTPClient h;
  h.setConnectTimeout(1500);   // max 1,5 s na połączenie
  h.setTimeout(2000);          // max 2 s na odpowiedź
  if (!h.begin(url)) return false;
  const int code = h.GET();
  if (code != HTTP_CODE_OK) {
    h.end();
    return false;
  }
  body = h.getString();
  h.end();
  if (body.length() > 8192) body = body.substring(0, 8192);
  return body.length() > 0;
}

void ExtDevManager::begin() { load(); LOG_I("ExtDev: zaladowano %d urzadzen", (int)devs_.size()); }

void ExtDevManager::load() {
  String s = nvsStoreRead(EV_NS, EV_KEY);
  devs_.clear();
  if (!s.length()) return;
  JsonDocument doc;
  if (deserializeJson(doc, s)) return;
  JsonArray arr = doc.as<JsonArray>();
  for (JsonVariant v : arr) {
    if (!v.is<JsonObject>()) continue;
    JsonObjectConst o = v.as<JsonObjectConst>();
    ExtDevice d;
    d.name = o["name"] | "";
    d.url  = o["url"] | "";
    d.type = o["type"] | "vevor";
    d.interval = o["interval"] | 20;
    if (d.interval < 5) d.interval = 5;
    if (d.url.length()) devs_.push_back(d);
  }
}

void ExtDevManager::save() {
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (auto& d : devs_) {
    JsonObject o = arr.add<JsonObject>();
    o["name"] = d.name;
    o["url"] = d.url;
    o["type"] = d.type;
    o["interval"] = d.interval;
  }
  String s;
  serializeJson(doc, s);
  nvsStoreWrite(EV_NS, EV_KEY, s);
}

bool ExtDevManager::add(const String& name, const String& url, const String& type, int interval) {
  String u = url; u.trim();
  if (!u.startsWith("http://") && !u.startsWith("https://")) return false;
  String t = (type == "ble") ? "ble" : ((type == "aq") ? "aq" : "vevor");
  int iv = interval;
  if (iv < 5) iv = 20;
  for (auto& d : devs_) {
    if (d.url == u) {          // aktualizacja istniejącego
      d.name = name.length() ? name : d.name;
      d.type = t;
      d.interval = iv;
      save();
      return true;
    }
  }
  ExtDevice d;
  d.name = name.length() ? name : u;
  d.url = u;
  d.type = t;
  d.interval = iv;
  devs_.push_back(d);
  save();
  return true;
}

bool ExtDevManager::remove(int i) {
  if (i < 0 || i >= (int)devs_.size()) return false;
  devs_.erase(devs_.begin() + i);
  save();
  return true;
}

void ExtDevManager::pollOne(ExtDevice& d) {
  String body;
  if (!httpGetJson(d.url, body)) {
    d.ok = false;
    d.lastErr = "brak odpowiedzi";
    return;
  }
  JsonDocument doc;
  if (deserializeJson(doc, body)) {
    d.ok = false;
    d.lastErr = "błędny JSON";
    return;
  }

  if (d.type == "ble") {
    int n = 0;
    JsonVariant arr = doc["sensors"];
    if (arr.is<JsonArray>()) {
      for (JsonVariant v : arr.as<JsonArray>()) {
        if (!v.is<JsonObject>()) continue;
        JsonObjectConst o = v.as<JsonObjectConst>();
        auto getf = [&](const char* key) -> float {
          JsonVariantConst x = o[key];
          if (x.is<float>()) return x.as<float>();
          if (x.is<int>()) return (float)x.as<int>();
          if (x.is<unsigned int>()) return (float)x.as<unsigned int>();
          return NAN;
        };
        const String mac  = o["mac"] | "";
        const String name = o["name"] | "";
        if (!mac.length()) continue;
        sensors.ingestBleSensor(mac, name, getf("temp"), getf("hum"),
                                (int)getf("batt"), (int)getf("rssi"));
        n++;
      }
    }
    d.ok = true;
    d.lastErr = n ? String(n) + " czujników" : "brak czujników";
    return;
  }

  if (d.type == "aq") {
    // Stacja jakości powietrza: {"channels":[{"id":"pm25","value":22,"valid":true}, ...]}
    // Bierzemy WSZYSTKIE kanały liczbowe, jakie stacja poda - które z nich
    // trafią do pomiarów wewnętrznych, decyduje tabela AIR_IMPORT
    // (sensors_extra.cpp). Dzięki temu nowy czujnik po stronie stacji
    // powietrza nie wymaga zmian w tym miejscu.
    // Wartości bez flagi "valid" traktujemy jako poprawne (format bywa różny).
    AirStationData aq;
    int n = 0;
    JsonVariant chs = doc["channels"];
    if (chs.is<JsonArray>()) {
      for (JsonVariant v : chs.as<JsonArray>()) {
        if (!v.is<JsonObject>()) continue;
        JsonObjectConst o = v.as<JsonObjectConst>();
        const String id = o["id"] | "";
        JsonVariantConst okv = o["valid"];
        if (okv.is<bool>() && !okv.as<bool>()) continue;
        JsonVariantConst x = o["value"];
        float val = NAN;
        if (x.is<float>()) val = x.as<float>();
        else if (x.is<int>()) val = (float)x.as<int>();
        else if (x.is<unsigned int>()) val = (float)x.as<unsigned int>();
        if (isnan(val)) continue;

        aq.add(id.c_str(), val);
        n++;
      }
    }
    if (n) sensors.ingestAirStation(aq);
    d.ok = n > 0;
    d.lastErr = n ? String(n) + " odczytów" : "brak rozpoznanych kanałów";
    return;
  }

  // domyślnie "vevor"
  JsonVariant weather = doc["weather"];
  JsonObjectConst w;
  if (weather.is<JsonObject>()) w = weather.as<JsonObjectConst>();
  else w = doc.as<JsonObjectConst>();
  auto getf = [&](const char* key) -> float {
    JsonVariantConst v = w[key];
    if (v.is<float>()) return v.as<float>();
    if (v.is<int>())   return (float)v.as<int>();
    if (v.is<unsigned int>()) return (float)v.as<unsigned int>();
    return NAN;
  };
  float uvi = getf("uvi");
  if (isnan(uvi)) uvi = getf("uv");
  sensors.ingestVevor(getf("temperature_C"), getf("humidity"),
                      getf("wind_avg_m_s"), getf("wind_max_m_s"),
                      getf("wind_dir_deg"), getf("rain_mm"), uvi, getf("light_lux"));
  d.ok = true;
  d.lastErr = "";
}

void ExtDevManager::loop() {
  if (!devs_.size()) return;
  const unsigned long now = millis();
  for (auto& d : devs_) {
    // Backoff: po 3 kolejnych błędach odpytuj martwe urządzenie co 5 min,
    // żeby nie wisieć w pętli i nie męczyć watchdoga zadania.
    unsigned long gap = (d.failCount >= 3) ? 300000UL : (unsigned long)d.interval * 1000UL;
    if (now - d.lastMs < gap) continue;
    LOG_I("ExtDev: poll %s", d.url.c_str());
    d.lastMs = now;
    pollOne(d);
    if (d.ok) d.failCount = 0;
    else if (d.failCount < 10) d.failCount++;
    LOG_I("Urządzenie %s (%s): %s", d.name.c_str(), d.url.c_str(),
          d.ok ? "OK" : d.lastErr.c_str());
  }
}
