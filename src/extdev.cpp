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
#include <math.h>

static const char* EV_NS = "extdev";
static const char* EV_KEY = "list";

ExtDevManager extdev;

// Prosty HTTP GET zwracający treść (max 8 KB).
//
// UWAGA (naprawa 12.10): wcześniej użyty HTTPClient potrafił ZAWIESIĆ PĘTLĘ
// GŁÓWNĄ bez końca. Objaw: węzeł C3 odpowiadał na ping, ale nie na HTTP;
// log kończył się na "ExtDev: poll http://192.168.1.147/api/status" i stacja
// przestawała cokolwiek mierzyć (czujniki zamrożone, ekran nie odświeżał się,
// OTA nie odpowiadało) - bo ESP.restart() z /api/reboot też czeka na pętlę.
// Limity HTTPClient (connect/read) nie zadziałały w tym przypadku.
//
// Dlatego łączymy się własnym gniazdem i nie ma tu ANI JEDNEGO blokującego
// oczekiwania: czytamy tylko to, co już przyszło (available()), a po przekroczonym
// budżecie czasu zamykamy gniazdo. Czas jednego odczytu jest więc zawsze
// ograniczony z góry, niezależnie od stanu drugiej strony.
static bool httpGetJson(const String& url, String& body) {
  const uint32_t CONNECT_MS = 1200;   // max na nawiązanie połączenia TCP
  const uint32_t BUDGET_MS  = 1800;   // max na całość (nagłówki + treść)
  const size_t   MAX_BYTES  = 9000;   // 8 KB treści + nagłówki

  if (!url.startsWith("http://")) {
    // https:// wymagało TLS - tego po surowym gnieździe nie zrobimy.
    return false;
  }
  String rest = url.substring(7);            // bez "http://"
  int slash = rest.indexOf('/');
  String hostPort = (slash < 0) ? rest : rest.substring(0, slash);
  String path     = (slash < 0) ? "/" : rest.substring(slash);

  String host = hostPort;
  uint16_t port = 80;
  int colon = hostPort.indexOf(':');
  if (colon > 0) {
    host = hostPort.substring(0, colon);
    int p = hostPort.substring(colon + 1).toInt();
    if (p > 0 && p < 65536) port = (uint16_t)p;
  }

  IPAddress ip;
  if (!ip.fromString(host)) {                // nazwa zamiast adresu - zapytaj DNS
    if (!WiFi.hostByName(host.c_str(), ip)) return false;
  }

  WiFiClient c;
  c.setTimeout(0);                           // zero = żadne czytanie nie czeka
  if (!c.connect(ip, port, CONNECT_MS)) return false;

  c.printf("GET %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n", path.c_str(), host.c_str());

  String raw;
  raw.reserve(1024);
  const uint32_t t0 = millis();
  while (millis() - t0 < BUDGET_MS) {
    bool got = false;
    while (c.available()) {
      const int ch = c.read();
      if (ch < 0) break;
      raw += (char)ch;
      got = true;
      if (raw.length() >= MAX_BYTES) break;
    }
    if (raw.length() >= MAX_BYTES) break;
    if (!got && !c.connected()) break;       // serwer zamknął połączenie
    if (!got) delay(2);                      // nic nie ma - oddaj czas innym zadaniom
  }
  c.stop();

  const int sep = raw.indexOf("\r\n\r\n");
  if (sep < 0) return false;
  if (raw.indexOf(" 200 ") < 0 && !raw.startsWith("HTTP/1.1 200") && !raw.startsWith("HTTP/1.0 200")) {
    return false;
  }
  body = raw.substring(sep + 4);
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
  String t = (type == "ble") ? "ble"
           : ((type == "aq") ? "aq"
           : ((type == "outaq") ? "outaq" : "vevor"));
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

  if (d.type == "aq" || d.type == "outaq") {
    // Stacja jakości powietrza: {"channels":[{"id":"pm25","value":22,"valid":true}, ...]}
    // "aq"  -> kanały WEWNĘTRZNE (AIR_IMPORT, strefa "in", np. stacja .141)
    // "outaq" -> kanały ZEWNĘTRZNE (OUTC3_IMPORT, strefa "out", prefiks c3_)
    // Bierzemy WSZYSTKIE kanały liczbowe, jakie stacja poda - które z nich
    // trafią do pomiarów, decydują tabele importu (sensors_extra.cpp).
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
    if (n) {
      if (d.type == "outaq") sensors.ingestOutdoorAir(aq);
      else sensors.ingestAirStation(aq);
    }
    // Urządzenie podaje WŁASNY czas pracy i stan MQTT - to jest dowód życia
    // (sama odpowiedź HTTP mogłaby przyjść z resztek, a licznik czasu pracy
    // rośnie tylko na działającym urządzeniu). Zapisujemy to razem z chwilą
    // odbioru, żeby wyświetlacz mógł pokazać stan bez sprawdzania IP.
    JsonVariant up = doc["uptime_s"];
    if (!up.isNull()) {
      d.peerUptimeS = up.as<uint32_t>();
      d.peerMs = millis();
    }
    JsonVariant mq = doc["mqtt"];
    if (mq.is<bool>()) d.peerMqtt = mq.as<bool>();
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
    float f = NAN;
    if (v.is<float>()) f = v.as<float>();
    else if (v.is<int>()) f = (float)v.as<int>();
    else if (v.is<unsigned int>()) f = (float)v.as<unsigned int>();
    // NaN/Inf nie mogą iść dalej (do ingestVevor, MQTT ani na SD).
    return isfinite(f) ? f : NAN;
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
    // Backoff: chwilowa przerwa (np. zgubiony pakiet do sniffera) nie może
    // odbierać danych na długo, ale urządzenie, które naprawdę nie odpowiada,
    // musi być odpytywane coraz rzadziej - inaczej każde nieudane połączenie
    // kosztuje czas pętli głównej. Progi: 3 błędy -> 1 min, 6 -> 5 min,
    // 10 -> 30 min (wcześniej 5 min, co przy martwym węźle C3 dawało
    // niekończące się serie nieudanych prób).
    unsigned long gap = (d.failCount >= 10) ? 1800000UL
                      : (d.failCount >= 6)  ? 300000UL
                      : (d.failCount >= 3)  ? 60000UL
                      : (unsigned long)d.interval * 1000UL;
    if (now - d.lastMs < gap) continue;
    LOG_I("ExtDev: poll %s", d.url.c_str());
    const unsigned long t0 = millis();
    d.lastMs = now;
    pollOne(d);
    if (d.ok) d.failCount = 0;
    else if (d.failCount < 10) d.failCount++;
    LOG_I("Urządzenie %s (%s): %s (%lu ms)", d.name.c_str(), d.url.c_str(),
          d.ok ? "OK" : d.lastErr.c_str(), (unsigned long)(millis() - t0));
    // Po 10 nieudanych próbach odpytywanie tego urządzenia jest zawieszone na
    // pół godziny - zgłaszamy to raz, żeby nie zaśmiecać logu.
    if (d.failCount == 10) {
      LOG_W("ExtDev: %s nie odpowiada - odpytywanie zawieszone na 30 min", d.name.c_str());
    }

    // Budżet czasu na jedno wywołanie: przy kilku martwych urządzeniach
    // sumaryczny czas mógłby wydłużyć obieg pętli. Przerywamy po ~4 s -
    // reszta urządzeń zostaje odpyta w kolejnych obiegach.
    if (millis() - now > 4000UL) break;
  }
}
