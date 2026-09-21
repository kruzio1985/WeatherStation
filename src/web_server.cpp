/* =============================================================================
 * Stacja Pogody - stacja meteorologiczna na ESP32-S3 WROOM-1 (N16R8)
 * Copyright (c) 2026 kruzio1985 - https://github.com/kruzio1985
 * Licencja: PolyForm Noncommercial License 1.0.0 (plik LICENSE)
 * Użytek niekomercyjny. Kontakt: kruzio1985@users.noreply.github.com
 * =============================================================================
 */
#include "web_server.h"
#include "config.h"
#include "board.h"
#include "pinmap.h"
#include "sensors.h"
#include "mqtt_client.h"
#include "weather_services.h"
#include "sd_card.h"
#include "camera.h"
#include "logger.h"
#include "analysis.h"
#include "alerts.h"
#include "report.h"
#include "openmeteo.h"
#include "ota.h"
#include "syslog.h"
#include "led_ring.h"
#include "nvs_store.h"
#include "gps.h"
#include "astro.h"
#include "forecast.h"
#include "lightning.h"
#include "app_info.h"
#include "drv_rtc.h"
#include "rs485.h"
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <time.h>
#include <math.h>
#include <vector>
#include <esp_ota_ops.h>
#include <esp_heap_caps.h>
#include <esp_wifi.h>
#include <esp_err.h>
#include <algorithm>

AsyncWebServer server(80);
DNSServer dnsServer;
WebServerManager web;

// ------------------------------------------------------------
//  Skanowanie sieci Wi-Fi (zakładka Sieć)
//  Skan działa w osobnym zadaniu FreeRTOS, a zapytanie HTTP tylko go
//  uruchamia i odczytuje wynik - serwer www nie jest blokowany na kilka
//  sekund.
//
//  WYNIKI ODBIERAMY PRZEZ API ARDUINO (WiFi.scanNetworks), a nie przez
//  esp_wifi_scan_start()/esp_wifi_scan_get_ap_records(). W rdzeniu
//  Arduino-ESP32 obsługa zdarzenia WIFI_EVENT_SCAN_DONE
//  (WiFiScanClass::_scanDone) sama pobiera i czyści wyniki sterownika,
//  więc własne wywołanie esp_wifi_scan_get_ap_num() dostawało już pusty
//  zestaw - skan zwracał 0 sieci mimo widocznych AP. Ścieżka Arduino jest
//  spójna z rdzeniem, dlatego tylko ona daje prawidłową listę sieci.
// ------------------------------------------------------------
static volatile bool s_scanRunning = false;   // trwa skan (dla strony www)
static volatile bool s_scanBusy = false;      // zadanie skanujące jeszcze żyje
static unsigned long s_scanStarted = 0;
static String s_scanError;
static String s_scanNetworks;                 // gotowa tablica JSON z sieciami
static int    s_scanCount = 0;
static int    s_scanTries = 0;                // ile przebiegów skanu wykonał sterownik

static const int SCAN_AP_CAP = 64;            // maksymalna liczba AP w jednym skanie

// Jedno znalezione AP - tylko pola potrzebne stronie www.
struct ScanAp {
  String  ssid;
  int     rssi;
  int     channel;
  bool    secure;
  uint8_t bssid[6];
};

// Jeden przebieg skanu. Zwraca liczbę sieci albo -1 przy błędzie.
// Przy wyniku <= 0 tablica out zostaje nietknięta, dzięki czemu nieudana
// próba nie kasuje poprzedniego, udanego wyniku.
static int scanPass(ScanAp* out, int cap, String& err) {
  WiFi.enableSTA(true);   // sam tryb AP nie potrafi skanować

  int n = WiFi.scanNetworks(false /* synchronicznie */, true /* także ukryte SSID */);
  if (n == WIFI_SCAN_RUNNING) { err = "skan jest już w toku"; return -1; }
  if (n == WIFI_SCAN_FAILED)  { err = "sterownik Wi-Fi nie wykonał skanu"; return -1; }
  if (n < 0)                  { err = "nieznany błąd skanu (" + String(n) + ")"; return -1; }

  int cnt = 0;
  for (int i = 0; i < n && cnt < cap; i++) {
    String ssid = WiFi.SSID(i);
    if (ssid.length() == 0) continue;
    ScanAp& a = out[cnt];
    a.ssid    = ssid;
    a.rssi    = WiFi.RSSI(i);
    a.channel = WiFi.channel(i);
    a.secure  = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
    const uint8_t* b = WiFi.BSSID(i);
    if (b) memcpy(a.bssid, b, 6); else memset(a.bssid, 0, 6);
    cnt++;
  }
  WiFi.scanDelete();   // zwalnia pamięć wyników trzymaną przez rdzeń
  err = "";
  return cnt;
}

// Właściwy skan - blokujące wywołanie sterownika w osobnym zadaniu.
static void wifiScanTask(void*) {
  static ScanAp aps[SCAN_AP_CAP];

  // Interfejs STA potrzebuje chwili po włączeniu, a pierwszy skan potrafi
  // zwrócić pustą listę mimo widocznych sieci - dlatego powtarzamy go.
  vTaskDelay(pdMS_TO_TICKS(400));

  String err;
  int got = scanPass(aps, SCAN_AP_CAP, err);
  if (err.length()) LOG_W("Skan Wi-Fi: przebieg 1 -> błąd: %s", err.c_str());
  else              LOG_I("Skan Wi-Fi: przebieg 1 -> %d sieci", got);

  int tries = 1;
  while (got <= 0 && tries < 3) {
    vTaskDelay(pdMS_TO_TICKS(700));
    tries++;
    String e2;
    got = scanPass(aps, SCAN_AP_CAP, e2);
    if (e2.length()) LOG_W("Skan Wi-Fi: przebieg %d -> błąd: %s", tries, e2.c_str());
    else             LOG_I("Skan Wi-Fi: przebieg %d -> %d sieci", tries, got);
    if (e2.length()) err = e2;
  }
  s_scanTries = tries;

  if (got <= 0 && err.length()) {
    s_scanError = err;
    s_scanNetworks = "";
    s_scanCount = 0;
    LOG_E("Skan Wi-Fi nie powiódł się: %s", err.c_str());
  } else {
    // sortowanie od najsilniejszego sygnału
    for (int i = 1; i < got; i++) {
      ScanAp key = aps[i];
      int j = i - 1;
      while (j >= 0 && aps[j].rssi < key.rssi) { aps[j + 1] = aps[j]; j--; }
      aps[j + 1] = key;
    }

    PsramAllocator alloc;
    JsonDocument d(&alloc);
    JsonArray arr = d.to<JsonArray>();
    for (int i = 0; i < got; i++) {
      // ta sama nazwa sieci (kilka AP) - zostawiamy najsilniejszy
      bool dup = false;
      for (JsonObject o : arr) {
        if (o["ssid"].as<String>() == aps[i].ssid) { dup = true; break; }
      }
      if (dup) continue;
      JsonObject o = arr.add<JsonObject>();
      o["ssid"] = aps[i].ssid;
      o["rssi"] = aps[i].rssi;
      o["channel"] = aps[i].channel;
      o["secure"] = aps[i].secure;
      char mac[20];
      snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
               aps[i].bssid[0], aps[i].bssid[1], aps[i].bssid[2],
               aps[i].bssid[3], aps[i].bssid[4], aps[i].bssid[5]);
      o["bssid"] = mac;
    }
    serializeJson(arr, s_scanNetworks);
    s_scanCount = (int)arr.size();
    s_scanError = "";
    LOG_I("Skan Wi-Fi: znaleziono %d sieci (widocznych AP: %d, przebiegi: %d)",
          s_scanCount, got, tries);
  }

  s_scanRunning = false;
  s_scanBusy = false;
  vTaskDelete(nullptr);
}

// ------------------------------------------------------------
//  Odbiór treści żądań POST
//
//  ESPAsyncWebServer 3.x wstawia surowe body do parametru "plain" TYLKO
//  wtedy, gdy żądanie wygląda na formularz (Content-Type:
//  application/x-www-form-urlencoded). Przy "application/json" treść
//  trafia wyłącznie do odbiornika podanego w server.on(...), a bez niego
//  jest po cichu odrzucana - arg("plain") jest wtedy pusty i zapis
//  ustawień kończy się błędem. Dlatego każda trasa POST czytająca treść
//  żądania rejestrowana jest przez onPost().
// ------------------------------------------------------------
static String s_body;
static AsyncWebServerRequest* s_bodyReq = nullptr;
static const size_t MAX_BODY = 24576;

static void bodySink(AsyncWebServerRequest* r, uint8_t* data, size_t len, size_t index, size_t total) {
  if (index == 0) { s_body = ""; s_bodyReq = r; }
  if (r != s_bodyReq) return;                        // inne żądanie w toku
  if (s_body.length() + len > MAX_BODY) return;      // ochrona przed przepełnieniem
  s_body.concat((const char*)data, len);
}

static void onPost(const char* uri, ArRequestHandlerFunction fn) {
  server.on(uri, HTTP_POST, fn, nullptr, bodySink);
}

// ------------------------------------------------------------
//  Odbiór binarnych zdjęć JPEG (POST /api/camera/upload)
//
//  Zdjęcia są większe niż limity JSON-a, więc mają własny bufor w PSRAM
//  i własny odbiornik (content-type image/jpeg lub application/octet-stream).
// ------------------------------------------------------------
static const size_t MAX_IMG_BODY = 512 * 1024;     // max 512 KB na jedno zdjęcie
static uint8_t*  s_imgBuf = nullptr;
static size_t    s_imgLen = 0;
static size_t    s_imgCap = 0;
static AsyncWebServerRequest* s_imgReq = nullptr;
static bool      s_imgTooBig = false;

static void imgBodySink(AsyncWebServerRequest* r, uint8_t* data, size_t len,
                        size_t index, size_t total) {
  if (index == 0) {
    s_imgLen = 0;
    s_imgTooBig = false;
    s_imgReq = r;
    size_t want = (total > 0) ? total : MAX_IMG_BODY;
    if (want > MAX_IMG_BODY) { s_imgTooBig = true; return; }
    if (s_imgCap < want) {
      if (s_imgBuf) { heap_caps_free(s_imgBuf); s_imgBuf = nullptr; s_imgCap = 0; }
      s_imgBuf = (uint8_t*)heap_caps_malloc(want, MALLOC_CAP_SPIRAM);
      if (!s_imgBuf) s_imgBuf = (uint8_t*)malloc(want);
      if (!s_imgBuf) { s_imgTooBig = true; return; }
      s_imgCap = want;
    }
  }
  if (r != s_imgReq || s_imgTooBig) return;
  if (s_imgLen + len > s_imgCap) { s_imgTooBig = true; return; }
  memcpy(s_imgBuf + s_imgLen, data, len);
  s_imgLen += len;
}

// ------------------------------------------------------------
//  Pomocnicze
// ------------------------------------------------------------
static String jsonError(const char* msg) {
  JsonDocument d;
  d["error"] = msg;
  String s;
  serializeJson(d, s);
  return s;
}

// Zamienia "#RRGGBB" / "RRGGBB" na liczbę 0xRRGGBB. Zwraca false, gdy tekst
// nie jest poprawnym kolorem (wtedy wołający pomija daną wartość).
static bool parseHexColor(const char* s, uint32_t& out) {
  if (!s) return false;
  while (*s == ' ' || *s == '#') s++;
  uint32_t v = 0;
  int n = 0;
  for (; *s; s++) {
    int d;
    if (*s >= '0' && *s <= '9')      d = *s - '0';
    else if (*s >= 'a' && *s <= 'f') d = *s - 'a' + 10;
    else if (*s >= 'A' && *s <= 'F') d = *s - 'A' + 10;
    else return false;
    v = (v << 4) | (uint32_t)d;
    if (++n > 6) return false;
  }
  if (n != 6) return false;
  out = v;
  return true;
}

static String jsonEscape(const String& in) {
  String out;
  out.reserve(in.length() + 8);
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    if (c == '"' || c == '\\')      { out += '\\'; out += c; }
    else if (c == '\n')             out += "\\n";
    else if (c == '\r')             out += "\\r";
    else if (c == '\t')             out += "\\t";
    else if ((uint8_t)c < 0x20)     { char b[8]; snprintf(b, sizeof(b), "\\u%04X", (unsigned)(uint8_t)c); out += b; }
    else                            out += c;
  }
  return out;
}

static String bodyOf(AsyncWebServerRequest* r) {
  // 1) treść odebrana przez bodySink (application/json i inne typy)
  if (r == s_bodyReq && s_body.length()) return s_body;

  // 2) formularz lub tekst wysłany w parametrze "plain"
  String b = r->arg("plain");
  if (b.length() == 0 && r->params()) {
    for (size_t i = 0; i < r->params(); i++) {
      const AsyncWebParameter* p = r->getParam(i);
      if (p->isPost()) {
        if (b.length()) b += "&";
        b += p->name() + "=" + p->value();
      }
    }
  }
  return b;
}

void WebServerManager::registerRoutes() {
  // --- Status ---
  server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest* r) {
    r->send(200, "application/json", apiStatusJson());
  });

  // --- Konfiguracja ---
  server.on("/api/config", HTTP_GET, [](AsyncWebServerRequest* r) {
    r->send(200, "application/json", config.toJsonString());
  });

  onPost("/api/config", [](AsyncWebServerRequest* r) {
    String b = bodyOf(r);
    if (b.length() == 0) {
      LOG_E("Ustawienia: puste żądanie (brak treści POST)");
      r->send(400, "application/json", jsonError("Puste żądanie - brak danych do zapisu"));
      return;
    }
    if (config.applyJson(b.c_str(), b.length())) {
      LOG_I("Ustawienia zapisane ze strony www (%u B)", (unsigned)b.length());
      // Ustawienia pierścienia LED działają od razu, bez restartu
      ledRing.applyConfig();
      camera.applyConfig();
      // Po zmianie współrzędnych (lub innych ustawień) przelicz od razu
      // Słońce/Księżyc, żeby zakładka Astronomia nie czekała na pętlę 60 s.
      astro.update(config.latitude(), config.longitude(), time(nullptr),
                   AstroService::localTzOffsetMin());
      r->send(200, "application/json", "{\"ok\":true}");
      return;
    }
    // Rozróżniamy błąd danych od błędu zapisu, żeby komunikat na stronie
    // wskazywał prawdziwą przyczynę.
    JsonDocument probe;
    bool badJson = deserializeJson(probe, b) != DeserializationError::Ok;
    if (badJson) {
      LOG_E("Nie udało się zapisać ustawień - błędny JSON (%u B)", (unsigned)b.length());
      r->send(400, "application/json", jsonError("Błędny JSON"));
    } else {
      LOG_E("Nie udało się zapisać ustawień - pamięć NVS/LittleFS (%u B)", (unsigned)b.length());
      r->send(500, "application/json",
             jsonError("Nie udało się zapisać w pamięci trwałej (NVS) - sprawdź Diagnostykę"));
    }
  });

  // --- Kalibracja / konfiguracja kanałów ---
  onPost("/api/calibrate", [](AsyncWebServerRequest* r) {
    String b = bodyOf(r);
    if (config.applyChannelOverrides(b.c_str(), b.length())) {
      sensors.applyChannelConfig();
      r->send(200, "application/json", "{\"ok\":true}");
    } else {
      r->send(400, "application/json", jsonError("Błędny JSON"));
    }
  });

  // --- Lista czujników ---
  server.on("/api/sensors", HTTP_GET, [](AsyncWebServerRequest* r) {
    PsramAllocator alloc;
    JsonDocument d(&alloc);
    JsonArray arr = d["sensors"].to<JsonArray>();
    for (auto& c : sensors.snapshot()) {
      JsonObject co = arr.add<JsonObject>();
      co["id"] = c.id;
      co["name"] = c.name;
      co["present"] = c.present;
      co["detected"] = c.detected;
      co["measured"] = c.measured;
      co["enabled"] = c.enabled;
      co["unit"] = c.unit;
      co["zone"] = c.zone;
      co["remote"] = c.remote;   // kanał z drugiego ESP (po RS485)
      co["remote_addr"] = c.remote ? c.remoteAddr : 0;   // adres węzła magistrali
    }
    d["ds_count"] = sensors.dsSensorCount();
    d["pms_detected"] = sensors.pmsDetected();
    d["aq_name"] = sensors.aqName();
    d["compass"] = sensors.windCompass();
    String s;
    serializeJson(d, s);
    r->send(200, "application/json", s);
  });

  // --- Skanowanie DS18B20 ---
  server.on("/api/sensors/discover", HTTP_POST, [](AsyncWebServerRequest* r) {
    sensors.requestDsDiscovery();
    r->send(200, "application/json", "{\"ok\":true}");
  });

  // --- GPS ---
  server.on("/api/gps", HTTP_GET, [](AsyncWebServerRequest* r) {
    r->send(200, "application/json", gps.json());
  });

  // --- Słońce i Księżyc ---
  server.on("/api/astro", HTTP_GET, [](AsyncWebServerRequest* r) {
    r->send(200, "application/json", astro.json());
  });

  // --- Prognoza lokalna ---
  // UWAGA - kolejność jest istotna: ESPAsyncWebServer uznaje handler
  // zarejestrowany dla "/api/forecast" za pasujący także do "/api/forecast/...",
  // więc trasy podrzędne muszą być zarejestrowane PRZED trasą nadrzędną.
  // Inaczej "/api/forecast/analysis" i "/api/forecast/history" zwracałyby
  // zwykłą prognozę (pierwszy pasujący handler wygrywa).

  // Szczegółowa analiza (lista czynników wpływających na prognozę)
  server.on("/api/forecast/analysis", HTTP_GET, [](AsyncWebServerRequest* r) {
    r->send(200, "application/json", forecast.analysisJson());
  });

  // Historia ciśnienia/temperatury/wilgotności z 24 h (do wykresu na www)
  server.on("/api/forecast/history", HTTP_GET, [](AsyncWebServerRequest* r) {
    String j = "{\"count\":" + String((unsigned)forecast.historyCount()) + ",\"t\":[";
    size_t n = forecast.historyCount();
    for (size_t i = 0; i < n; i++) {
      if (i) j += ",";
      j += String((long)forecast.historyTime(i));
    }
    j += "],\"press\":[";
    for (size_t i = 0; i < n; i++) {
      if (i) j += ",";
      float v = forecast.historyPressure(i);
      j += isnan(v) ? String("null") : String(v, 2);
    }
    j += "],\"temp\":[";
    for (size_t i = 0; i < n; i++) {
      if (i) j += ",";
      float v = forecast.historyTemp(i);
      j += isnan(v) ? String("null") : String(v, 2);
    }
    j += "],\"hum\":[";
    for (size_t i = 0; i < n; i++) {
      if (i) j += ",";
      float v = forecast.historyHum(i);
      j += isnan(v) ? String("null") : String(v, 1);
    }
    j += "]}";
    r->send(200, "application/json", j);
  });

  server.on("/api/forecast", HTTP_GET, [](AsyncWebServerRequest* r) {
    r->send(200, "application/json", forecast.json());
  });

  // --- Detektor wyładowań AS3935 ---
  // Trasy podrzędne przed "/api/lightning" - patrz komentarz przy prognozie.

  // Kalibracja anteny AS3935 (pomiar LCO ~500 kHz, dobór trymera)
  onPost("/api/lightning/calibrate", [](AsyncWebServerRequest* r) {
    if (!lightning.present()) {
      r->send(409, "application/json", jsonError("Czujnik AS3935 nie odpowiada"));
      return;
    }
    float khz = lightning.autoTune();
    String j = "{\"ok\":";
    j += isnan(khz) ? "false" : "true";
    j += ",\"lco_khz\":";
    j += isnan(khz) ? String("null") : String(khz, 1);
    j += ",\"tuning_cap\":" + String(lightning.info().tuningCap) + "}";
    r->send(200, "application/json", j);
  });

  onPost("/api/lightning/reset", [](AsyncWebServerRequest* r) {
    lightning.resetDefaults();
    r->send(200, "application/json", lightning.json());
  });

  onPost("/api/lightning/clear", [](AsyncWebServerRequest* r) {
    lightning.clearStatistics();
    r->send(200, "application/json", lightning.json());
  });

  server.on("/api/lightning", HTTP_GET, [](AsyncWebServerRequest* r) {
    r->send(200, "application/json", lightning.json());
  });

  onPost("/api/lightning", [](AsyncWebServerRequest* r) {
    String b = bodyOf(r);
    PsramAllocator alloc;
    JsonDocument d(&alloc);
    if (deserializeJson(d, b) != DeserializationError::Ok) {
      r->send(400, "application/json", jsonError("Błędny JSON"));
      return;
    }
    if (d["indoor"].is<bool>()) lightning.setIndoor(d["indoor"].as<bool>());
    if (d["mask_dist"].is<bool>()) lightning.setMaskDist(d["mask_dist"].as<bool>());
    if (d["wdth"].is<int>() || d["nf_lev"].is<int>() || d["spike_rej"].is<int>() ||
        d["min_strikes"].is<int>()) {
      const LightningInfo& li = lightning.info();
      lightning.setSensitivity(
        d["nf_lev"].is<int>() ? (uint8_t)d["nf_lev"].as<int>() : li.nfLev,
        d["wdth"].is<int>() ? (uint8_t)d["wdth"].as<int>() : li.wdth,
        d["spike_rej"].is<int>() ? (uint8_t)d["spike_rej"].as<int>() : li.spikeRej,
        d["min_strikes"].is<int>() ? (uint8_t)d["min_strikes"].as<int>() : li.minStrike);
    }
    if (d["tuning_cap"].is<int>()) lightning.setTuningCap(d["tuning_cap"].as<int>());
    r->send(200, "application/json", lightning.json());
  });

  // --- Karta SD ---
  server.on("/api/sd/list", HTTP_GET, [](AsyncWebServerRequest* r) {
    std::vector<SdFileInfo> files;
    String dir = "/logs";
    if (r->hasArg("dir")) dir = r->arg("dir");
    if (!sdCard.mounted()) {
      r->send(200, "application/json", "{\"mounted\":false,\"files\":[]}");
      return;
    }
    sdCard.listDir(dir, files);

    PsramAllocator alloc;
    JsonDocument d(&alloc);
    d["mounted"] = true;
    d["dir"] = dir;
    JsonArray arr = d["files"].to<JsonArray>();
    for (auto& f : files) {
      JsonObject fo = arr.add<JsonObject>();
      fo["path"] = f.path;
      fo["size"] = f.size;
      fo["mtime"] = (uint32_t)f.mtime;
    }
    String s;
    serializeJson(d, s);
    r->send(200, "application/json", s);
  });

  server.on("/api/sd/download", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (!r->hasArg("path")) { r->send(400, "application/json", jsonError("Brak path")); return; }
    String path = r->arg("path");
    if (!sdCard.exists(path)) { r->send(404, "application/json", jsonError("Nie ma takiego pliku")); return; }

    String ct = mimeType(path);
#if SD_MMC_AVAILABLE
    if (sdCard.usingSdmmc()) r->send(SD_MMC, path, ct, true);
    else
#endif
    r->send(SD, path, ct, true);
  });

  onPost("/api/sd/delete", [](AsyncWebServerRequest* r) {
    String b = bodyOf(r);
    JsonDocument d;
    deserializeJson(d, b);
    String path = d["path"] | "";
    if (path.length() == 0) { r->send(400, "application/json", jsonError("Brak path")); return; }
    bool ok = sdCard.deleteFile(path);
    r->send(ok ? 200 : 500, "application/json", ok ? "{\"ok\":true}" : jsonError("Nie udało się usunąć"));
  });

  server.on("/api/sd/format", HTTP_POST, [](AsyncWebServerRequest* r) {
    bool ok = sdCard.format();
    if (ok) logger.begin();
    r->send(ok ? 200 : 500, "application/json", ok ? "{\"ok\":true}" : jsonError("Formatowanie nieudane"));
  });

  // Ponowne wykrycie karty bez restartu stacji: zwalnia poprzedni montaż
  // i próbuje od nowa (SDMMC, potem SPI) - przydaje się, gdy kartę włożono
  // już po starcie albo po poprawieniu pinów na stronie www.
  server.on("/api/sd/remount", HTTP_POST, [](AsyncWebServerRequest* r) {
    bool ok = sdCard.remount();
    if (ok) logger.begin();
    JsonDocument d;
    d["ok"] = ok;
    d["mounted"] = sdCard.mounted();
    d["mode"] = sdCard.usingSdmmc() ? "sdmmc" : "spi";
    d["interface"] = sdCard.interfaceInfo();
    if (ok) {
      d["total"] = sdCard.totalBytes();
      d["used"] = sdCard.usedBytes();
    } else {
      d["error"] = sdCard.error();
    }
    String s;
    serializeJson(d, s);
    r->send(ok ? 200 : 500, "application/json", s);
  });

  // --- Kamera (opcjonalna) ---
  server.on("/api/camera", HTTP_GET, [](AsyncWebServerRequest* r) {
    r->send(200, "application/json", camera.toJson());
  });

  onPost("/api/camera/capture", [](AsyncWebServerRequest* r) {
    // Przy zewnętrznej kamerze (cam_remote_url) "zrób zdjęcie" to żądanie
    // HTTP GET do modułu - zdjęcie wróci samo przez /api/camera/upload.
    String url = camera.remoteUrl();
    url.trim();
    bool remote = (url.length() > 0);
    bool ok = remote ? camera.triggerRemote() : camera.capture();
    PsramAllocator alloc;
    JsonDocument d(&alloc);
    d["ok"] = ok;
    d["remote"] = remote;
    if (ok) d["path"] = camera.lastFile();
    else    d["error"] = camera.error();
    String s;
    serializeJson(d, s);
    r->send(ok ? 200 : 500, "application/json", s);
  });

  // Odbiór zdjęcia wysłanego przez zewnętrzny moduł ESP32-CAM (JPEG w body).
  server.on("/api/camera/upload", HTTP_POST,
    [](AsyncWebServerRequest* r) {
      PsramAllocator alloc;
      JsonDocument d(&alloc);
      bool ok = false;
      String err = "brak danych zdjęcia";
      if (r == s_imgReq && !s_imgTooBig && s_imgLen >= 4) {
        ok = camera.ingestJpeg(s_imgBuf, s_imgLen);
        if (!ok) err = camera.error();
      } else if (s_imgTooBig) {
        err = "zdjęcie przekracza maksymalny rozmiar (512 KB)";
      }
      d["ok"] = ok;
      if (ok) d["path"] = camera.lastFile();
      else    d["error"] = err;
      String s;
      serializeJson(d, s);
      r->send(ok ? 200 : 413, "application/json", s);
    }, nullptr, imgBodySink);

  // Piny kamery (sekcja "pins" ustawień). Zarządzane osobno od zakładki
  // "Piny", bo to 16 dedykowanych linii magistrali równoległej kamery.
  onPost("/api/camera/pins", [](AsyncWebServerRequest* r) {
    String b = bodyOf(r);
    JsonDocument d;
    if (deserializeJson(d, b) != DeserializationError::Ok) {
      r->send(400, "application/json", jsonError("Błędny JSON"));
      return;
    }
    static const char* const KEYS[] = {
      "cam_xclk", "cam_pclk", "cam_vsync", "cam_href",
      "cam_d0", "cam_d1", "cam_d2", "cam_d3",
      "cam_d4", "cam_d5", "cam_d6", "cam_d7",
      "cam_sda", "cam_scl", "cam_pwdn", "cam_reset"
    };
    for (const char* k : KEYS) {
      JsonVariant v = d[k];
      if (v.is<int>()) config.setPinOverride(k, v.as<int>());
    }
    config.save();
    camera.applyConfig();
    r->send(200, "application/json", camera.toJson());
  });

  // --- Galeria zdjęć: rekurencyjnie /photos (katalogi miesięczne) ---
  server.on("/api/photos", HTTP_GET, [](AsyncWebServerRequest* r) {
    PsramAllocator alloc;
    JsonDocument d(&alloc);
    d["mounted"] = sdCard.mounted();
    JsonArray arr = d["files"].to<JsonArray>();

    struct PhotoItem { String path; size_t size; time_t mtime; };
    std::vector<PhotoItem> items;

    if (sdCard.mounted()) {
      std::vector<SdFileInfo> base;
      sdCard.listDir("/photos", base);
      for (auto& e : base) {
        if (e.path.endsWith(".jpg") || e.path.endsWith(".jpeg")) {
          items.push_back({e.path, e.size, e.mtime});
        } else {
          // Katalog miesięczny: /photos/RRRR-MM/
          std::vector<SdFileInfo> sub;
          sdCard.listDir(e.path, sub);
          for (auto& f : sub) {
            if (f.path.endsWith(".jpg") || f.path.endsWith(".jpeg"))
              items.push_back({f.path, f.size, f.mtime});
          }
        }
      }
      std::sort(items.begin(), items.end(),
                [](const PhotoItem& a, const PhotoItem& b) { return a.mtime > b.mtime; });
    }

    for (auto& it : items) {
      JsonObject fo = arr.add<JsonObject>();
      fo["path"] = it.path;
      fo["size"] = it.size;
      fo["mtime"] = (uint32_t)it.mtime;
    }
    String s;
    serializeJson(d, s);
    r->send(200, "application/json", s);
  });

  // --- Wykresy / logi ---
  server.on("/api/logs", HTTP_GET, [](AsyncWebServerRequest* r) {
    String period = r->hasArg("period") ? r->arg("period") : "day";
    String metric = r->hasArg("metric") ? r->arg("metric") : "temp";

    std::vector<SeriesPoint> series;
    String err;
    if (!logger.getSeries(period, metric, series, err)) {
      r->send(200, "application/json", jsonError(err.c_str()));
      return;
    }

    PsramAllocator alloc;
    JsonDocument d(&alloc);
    d["period"] = period;
    d["metric"] = metric;
    JsonArray arr = d["points"].to<JsonArray>();
    for (auto& p : series) {
      JsonObject po = arr.add<JsonObject>();
      po["t"] = p.ts;
      po["avg"] = p.avg;
      po["min"] = p.min;
      po["max"] = p.max;
      po["n"] = p.count;
    }
    String s;
    serializeJson(d, s);
    r->send(200, "application/json", s);
  });

  // --- Róża wiatrów (kierunek + prędkość) - liczona na żądanie z CSV. ---
  server.on("/api/windrose", HTTP_GET, [](AsyncWebServerRequest* r) {
    String period = r->hasArg("period") ? r->arg("period") : "week";
    if (period != "day" && period != "week" && period != "month" && period != "year")
      period = "week";
    r->send(200, "application/json", analysis.windRoseJson(period));
  });

  // --- Indeks jakości powietrza (CAQI) z PM2.5 / PM10 + CO2 wewnątrz ---
  server.on("/api/aqi", HTTP_GET, [](AsyncWebServerRequest* r) {
    r->send(200, "application/json", analysis.aqiJson());
  });

  // --- Analiza pogodowa (zakładka "Rekordy"): ekstrema, statystyki dnia,
  //     trend ciśnienia. Liczone z CSV na karcie SD + pamięci podręcznej.
  server.on("/api/analysis", HTTP_GET, [](AsyncWebServerRequest* r) {
    r->send(200, "application/json", analysis.json());
  });

  // --- Raporty min/śr/max (zakładka "Raporty") ---
  // Trasa podrzędna "/api/reports/export" PRZED "/api/reports" (patrz
  // komentarz o kolejności przy prognozie).
  server.on("/api/reports/export", HTTP_GET, [](AsyncWebServerRequest* r) {
    String period = r->hasArg("period") ? r->arg("period") : "day";
    if (period != "day" && period != "week" && period != "month") period = "day";
    String csv = reports.csv(period);
    if (csv.length() == 0) {
      r->send(404, "application/json", jsonError("Brak danych dla tego okresu"));
      return;
    }
    AsyncWebServerResponse* resp = r->beginResponse(
        200, "text/csv; charset=utf-8", csv);
    resp->addHeader("Content-Disposition",
        "attachment; filename=\"raport-" + period + ".csv\"");
    r->send(resp);
  });
  server.on("/api/reports", HTTP_GET, [](AsyncWebServerRequest* r) {
    String period = r->hasArg("period") ? r->arg("period") : "day";
    if (period != "day" && period != "week" && period != "month") period = "day";
    r->send(200, "application/json", reports.json(period));
  });

  // --- Alerty / progi (zakładka "Alerty") ---
  server.on("/api/alerts", HTTP_GET, [](AsyncWebServerRequest* r) {
    r->send(200, "application/json", alerts.json());
  });
  onPost("/api/alerts", [](AsyncWebServerRequest* r) {
    String b = bodyOf(r);
    if (b.length() == 0) {
      r->send(400, "application/json", jsonError("Brak danych"));
      return;
    }
    if (!alerts.applyJson(b.c_str(), b.length())) {
      r->send(400, "application/json", jsonError("Nieprawidłowy JSON alertów"));
      return;
    }
    LOG_I("Progi alertów zapisane ze strony www (%u B)", (unsigned)b.length());
    r->send(200, "application/json", "{\"ok\":true}");
  });

  // --- Prognoza Open-Meteo (7 dni, zakładka "Prognoza") ---
  // Trasa podrzędna przed "/api/openmeteo" (patrz komentarz przy prognozie).
  onPost("/api/openmeteo/refresh", [](AsyncWebServerRequest* r) {
    openMeteo.requestNow();
    r->send(200, "application/json", "{\"ok\":true}");
  });
  server.on("/api/openmeteo", HTTP_GET, [](AsyncWebServerRequest* r) {
    r->send(200, "application/json", openMeteo.json());
  });
  onPost("/api/openmeteo", [](AsyncWebServerRequest* r) {
    String b = bodyOf(r);
    if (b.length() == 0) {
      r->send(400, "application/json", jsonError("Brak danych"));
      return;
    }
    if (!openMeteo.applyJson(b.c_str(), b.length())) {
      r->send(400, "application/json", jsonError("Nieprawidłowy JSON prognozy"));
      return;
    }
    LOG_I("Ustawienia Open-Meteo zapisane ze strony www (%u B)", (unsigned)b.length());
    r->send(200, "application/json", "{\"ok\":true}");
  });

  // --- Logi systemowe (zakładka "Logi") ---
  // Uwaga: inna nazwa niż /api/logs, bo tamten endpoint zwraca serie wykresów z CSV.
  server.on("/api/syslog", HTTP_GET, [](AsyncWebServerRequest* r) {
    size_t n = syslog.snapshot();
    if (n == 0) {
      r->send(200, "text/plain; charset=utf-8", "Brak logów.\n");
      return;
    }
    AsyncWebServerResponse* resp =
        r->beginResponse(200, "text/plain; charset=utf-8",
                         (const uint8_t*)syslog.snapshotData(), n);
    resp->addHeader("Cache-Control", "no-store");
    r->send(resp);
  });

  server.on("/api/syslog", HTTP_DELETE, [](AsyncWebServerRequest* r) {
    syslog.clear();
    LOG_I("Bufor logów wyczyszczony ze strony www");
    r->send(200, "application/json", "{\"ok\":true}");
  });

  // --- Diagnostyka (zakładka "Diagnostyka") ---
  server.on("/api/diagnostics", HTTP_GET, [](AsyncWebServerRequest* r) {
    r->send(200, "application/json", apiDiagnosticsJson());
  });

  // --- Kopia ustawień / odtworzenie ---
  server.on("/api/backup", HTTP_GET, [](AsyncWebServerRequest* r) {
    AsyncWebServerResponse* resp =
        r->beginResponse(200, "application/json", config.toJsonString());
    resp->addHeader("Content-Disposition",
                    "attachment; filename=\"stacja-pogody-ustawienia.json\"");
    resp->addHeader("Cache-Control", "no-store");
    r->send(resp);
  });

  onPost("/api/backup/restore", [](AsyncWebServerRequest* r) {
    String b = bodyOf(r);
    if (b.length() == 0) {
      r->send(400, "application/json", jsonError("Brak danych"));
      return;
    }
    if (!config.applyJson(b.c_str(), b.length())) {
      r->send(400, "application/json", jsonError("Nieprawidłowy JSON ustawień"));
      return;
    }
    ledRing.applyConfig();
    LOG_I("Ustawienia odtworzone z kopii (%u B)", (unsigned)b.length());
    r->send(200, "application/json", "{\"ok\":true}");
  });

  // --- Dane publiczne (dla strony firmowej) ---
  server.on("/api/public", HTTP_GET, [](AsyncWebServerRequest* r) {
    PsramAllocator alloc;
    JsonDocument d(&alloc);
    d["company"] = config.companyName();
    d["url"] = config.companyUrl();
    d["location"] = config.location();
    for (auto& c : sensors.snapshot()) {
      if (c.id == "temp" || c.id == "hum" || c.id == "press" ||
          c.id == "pm25" || c.id == "wind") {
        if (isnan(c.value)) {
          d[c.id] = nullptr;
        } else {
          d[c.id] = c.value;
        }
        d[c.id + "_unit"] = c.unit;
      }
    }
    String s;
    serializeJson(d, s);
    r->send(200, "application/json", s);
  });

  // --- Skanowanie sieci Wi-Fi (zakładka Sieć) ---
  onPost("/api/wifi/scan", [](AsyncWebServerRequest* r) {
    if (s_scanBusy) {
      r->send(200, "application/json", "{\"scanning\":true}");
      return;
    }

    // Skan wymaga włączonego interfejsu STA - sam tryb AP go nie ma.
    if (!WiFi.enableSTA(true)) {
      LOG_W("Nie udało się włączyć interfejsu STA przed skanowaniem Wi-Fi");
    }

    s_scanError = "";
    s_scanNetworks = "";
    s_scanCount = 0;
    s_scanTries = 0;
    s_scanRunning = true;
    s_scanBusy = true;
    s_scanStarted = millis();

    if (xTaskCreatePinnedToCore(wifiScanTask, "wifi_scan", 8192, nullptr, 1, nullptr, tskNO_AFFINITY) != pdPASS) {
      s_scanRunning = false;
      s_scanBusy = false;
      s_scanError = "brak pamięci na uruchomienie zadania skanującego";
      LOG_E("%s", s_scanError.c_str());
      r->send(500, "application/json", jsonError(s_scanError.c_str()));
      return;
    }
    LOG_I("Skan Wi-Fi uruchomiony (tryb %s)", WiFi.getMode() == WIFI_MODE_APSTA ? "AP+STA" : "inny");
    r->send(200, "application/json", "{\"scanning\":true}");
  });

  server.on("/api/wifi/scan", HTTP_GET, [](AsyncWebServerRequest* r) {
    wifi_mode_t m = WiFi.getMode();
    const char* modeName = (m == WIFI_MODE_APSTA) ? "AP+STA"
                         : (m == WIFI_MODE_STA)   ? "STA"
                         : (m == WIFI_MODE_AP)    ? "AP" : "wyłączone";

    String resp = "{\"scanning\":";
    resp += s_scanRunning ? "true" : "false";
    resp += ",\"mode\":\"";
    resp += modeName;
    resp += "\",\"sta_enabled\":";
    resp += (WiFi.getMode() & WIFI_MODE_STA) ? "true" : "false";

    if (s_scanRunning) {
      resp += ",\"status\":\"scanning\"";
    } else if (s_scanError.length()) {
      resp += ",\"status\":\"failed\",\"error\":\"";
      resp += jsonEscape(s_scanError);
      resp += "\"";
    } else {
      resp += ",\"status\":\"done\",\"count\":";
      resp += String(s_scanCount);
      resp += ",\"tries\":";
      resp += String(s_scanTries);
      resp += ",\"networks\":";
      resp += (s_scanNetworks.length() ? s_scanNetworks : String("[]"));
    }
    resp += "}";
    r->send(200, "application/json", resp);
  });

  // --- Mapa pinów (zakładka "Piny") ---
  // "/api/pins/reset" przed "/api/pins" - trasa nadrzędna przechwytuje
  // wszystkie żądania "/api/pins/..." (patrz komentarz przy prognozie).
  server.on("/api/pins/reset", HTTP_POST, [](AsyncWebServerRequest* r) {
    pinMap.resetToDefaults();
    pinMap.save();
    LOG_I("Mapa pinów przywrócona do domyślnych z www - wymagany restart");
    r->send(200, "application/json", pinMap.toJson());
  });

  server.on("/api/pins", HTTP_GET, [](AsyncWebServerRequest* r) {
    r->send(200, "application/json", pinMap.toJson());
  });

  // --- Katalog płytek (szablony pinów dla drugiego ESP) ---
  // Gotowe mapy pinów dla ESP32-S3 N16R8 / N8R8 / N8R4 oraz ESP32-C3 SuperMini,
  // żeby nie wpisywać numerów GPIO ręcznie. "current" = wykryta płytka.
  server.on("/api/boards", HTTP_GET, [](AsyncWebServerRequest* r) {
    r->send(200, "application/json", boards::catalogJson());
  });

  onPost("/api/pins", [](AsyncWebServerRequest* r) {
    String body = bodyOf(r);
    String err;
    if (!pinMap.applyJson(body.c_str(), body.length(), err)) {
      LOG_W("Zmiana pinów odrzucona: %s", err.c_str());
      JsonDocument d;
      d["ok"] = false;
      d["error"] = err;
      String s;
      serializeJson(d, s);
      r->send(400, "application/json", s);
      return;
    }
    LOG_I("Mapa pinów zapisana z www - wymagany restart");
    r->send(200, "application/json", pinMap.toJson());
  });

  // --- Pierścień LED RGB (status pogody) ---
  server.on("/api/ring", HTTP_GET, [](AsyncWebServerRequest* r) {
    r->send(200, "application/json", ledRing.toJson());
  });

  // Sterowanie może przychodzić osobno od zapisu ustawień (suwak jasności,
  // test kolorów), dlatego każdy z kluczy jest opcjonalny.
  onPost("/api/ring", [](AsyncWebServerRequest* r) {
    String body = bodyOf(r);
    if (body.length() == 0) {
      r->send(400, "application/json", jsonError("Brak danych"));
      return;
    }

    JsonDocument d;
    if (deserializeJson(d, body) != DeserializationError::Ok) {
      r->send(400, "application/json", jsonError("Błędny JSON"));
      return;
    }

    JsonDocument cfg;

    // Własne kolory stanów pogody: tablica 10 wartości "#RRGGBB" albo liczb
    // 0xRRGGBB (kolejność jak w WeatherState). Zapis do NVS razem z resztą.
    if (d["colors"].is<JsonArray>()) {
      JsonArray cols = cfg["rgb_colors"].to<JsonArray>();
      uint8_t i = 0;
      for (JsonVariant v : d["colors"].as<JsonArray>()) {
        if (i >= WEATHER_STATE_COUNT) break;
        uint32_t rgb = 0;
        if (v.is<const char*>()) {
          if (!parseHexColor(v.as<const char*>(), rgb)) { i++; continue; }
        } else if (v.is<uint32_t>()) {
          rgb = v.as<uint32_t>();
        } else {
          i++; continue;
        }
        cols.add(rgb & 0xFFFFFFu);
        i++;
      }
      if (cols.size() == 0) cfg.remove("rgb_colors");
    }
    if (d["reset_colors"].is<bool>() && d["reset_colors"].as<bool>()) {
      JsonArray cols = cfg["rgb_colors"].to<JsonArray>();
      for (size_t i = 0; i < WEATHER_STATE_COUNT; i++) cols.add((uint32_t)WEATHER_COLOR_DEFAULTS[i]);
    }

    // Własne nazwy stanów pogody: tablica 10 tekstów (kolejność jak w
    // WeatherState). Pusty tekst = nazwa domyślna, więc można przywrócić
    // domyślną nazwę pojedynczego stanu, wpisując puste pole.
    if (d["names"].is<JsonArray>()) {
      JsonArray names = cfg["rgb_names"].to<JsonArray>();
      for (JsonVariant v : d["names"].as<JsonArray>()) {
        if (names.size() >= WEATHER_STATE_COUNT) break;
        const char* txt = v.is<const char*>() ? v.as<const char*>() : "";
        names.add(txt ? txt : "");
      }
      if (names.size() == 0) cfg.remove("rgb_names");
    }
    if (d["reset_names"].is<bool>() && d["reset_names"].as<bool>()) {
      JsonArray names = cfg["rgb_names"].to<JsonArray>();
      for (size_t i = 0; i < WEATHER_STATE_COUNT; i++) names.add("");
    }

    // Podgląd kolorów: numer stanu albo kPreviewCycle (pokazuj wszystkie po kolei)
    if (d["preview"].is<int>()) ledRing.setPreview(d["preview"].as<int>());

    // Zapisujemy w pamięci trwałej (NVS + kopia na LittleFS), żeby ustawienie
    // przetrwało restart i aktualizację firmware
    if (d["enabled"].is<bool>())    cfg["rgb_enabled"] = d["enabled"].as<bool>();
    if (d["brightness"].is<int>())  cfg["rgb_brightness"] = d["brightness"].as<int>();
    if (d["effects"].is<bool>())    cfg["rgb_effects"] = d["effects"].as<bool>();
    if (d["off_at_night"].is<bool>()) cfg["rgb_off_at_night"] = d["off_at_night"].as<bool>();
    if (d["count"].is<int>())       cfg["rgb_count"] = d["count"].as<int>();
    if (d["order"].is<int>())       cfg["rgb_order"] = d["order"].as<int>();
    if (d["max_ma"].is<int>())      cfg["rgb_max_ma"] = d["max_ma"].as<int>();

    if (cfg.size() > 0) {
      String s;
      serializeJson(cfg, s);
      if (!config.applyJson(s.c_str(), s.length())) {
        LOG_W("Nie udało się zapisać ustawień pierścienia LED");
      }
    }

    ledRing.applyConfig();
    LOG_I("Pierścień LED: %s, jasność %u%%, stan %s",
          ledRing.enabled() ? "włączony" : "wyłączony",
          (unsigned)ledRing.brightness(), ledRing.stateName().c_str());

    r->send(200, "application/json", ledRing.toJson());
  });

  // --- Zegar RTC (DS3231 / DS1307 / PCF8563) ---
  // Odczyt stanu: wykryty układ, adres, magistrala, czas RTC i systemowy,
  // różnica, flaga podtrzymania oraz ustawienia z NVS.
  server.on("/api/rtc", HTTP_GET, [](AsyncWebServerRequest* r) {
    JsonDocument d;
    JsonObject o = d.to<JsonObject>();
    o["ok"] = true;
    rtc.toJson(o);
    String s;
    serializeJson(d, s);
    r->send(200, "application/json", s);
  });

  // Zapis ustawień i polecenia jednorazowe. Każdy klucz jest opcjonalny,
  // więc strona może wysłać samo polecenie (np. tylko ustawienie czasu).
  // Odpowiedź zawsze zawiera pełny stan układu, a pole "ok" mówi, czy
  // polecenie się powiodło - strona tłumaczy to na swój komunikat.
  onPost("/api/rtc", [](AsyncWebServerRequest* r) {
    String body = bodyOf(r);
    if (body.length() == 0) {
      r->send(400, "application/json", jsonError("Brak danych"));
      return;
    }

    JsonDocument d;
    if (deserializeJson(d, body) != DeserializationError::Ok) {
      r->send(400, "application/json", jsonError("Błędny JSON"));
      return;
    }

    JsonDocument cfg;
    if (d["enabled"].is<bool>())  cfg["rtc_enabled"]  = d["enabled"].as<bool>();
    if (d["ntp_sync"].is<bool>()) cfg["rtc_ntp_sync"] = d["ntp_sync"].as<bool>();
    if (d["type"].is<int>())      cfg["rtc_type"]     = d["type"].as<int>();
    if (d["bus"].is<int>())       cfg["rtc_bus"]      = d["bus"].as<int>();
    if (d["sda"].is<int>())       cfg["rtc_sda"]      = d["sda"].as<int>();
    if (d["scl"].is<int>())       cfg["rtc_scl"]      = d["scl"].as<int>();

    if (cfg.size() > 0) {
      String s;
      serializeJson(cfg, s);
      if (!config.applyJson(s.c_str(), s.length())) {
        LOG_W("Nie udało się zapisać ustawień RTC (%u B)", (unsigned)s.length());
      }
      // Nowy typ układu, magistrala albo piny obowiązują od razu, bez restartu
      rtc.reload();
    }

    bool ok = true;
    const char* action = "";
    if (d["set_epoch"].is<long>()) {
      action = "set_epoch";
      ok = rtc.setTimeFromEpoch((time_t)d["set_epoch"].as<long>()) && rtc.syncSystemFromRtc();
    } else if (d["set_time"].is<const char*>()) {
      action = "set_time";
      ok = rtc.setTimeFromString(String(d["set_time"].as<const char*>())) && rtc.syncSystemFromRtc();
    } else if (d["sync_to_sys"].is<bool>() && d["sync_to_sys"].as<bool>()) {
      action = "sync_to_sys";
      ok = rtc.syncSystemFromRtc();
    } else if (d["sync_to_rtc"].is<bool>() && d["sync_to_rtc"].as<bool>()) {
      action = "sync_to_rtc";
      ok = rtc.syncRtcFromSystem();
    } else if (d["clear_battery"].is<bool>() && d["clear_battery"].as<bool>()) {
      action = "clear_battery";
      ok = rtc.clearBatteryFlag();
    } else if (d["ntp_now"].is<bool>() && d["ntp_now"].as<bool>()) {
      action = "ntp_now";
      rtcStartNtp();
    } else if (d["detect"].is<bool>() && d["detect"].as<bool>()) {
      action = "detect";
      rtc.reload();
      ok = rtc.present();
    }

    JsonDocument out;
    JsonObject o = out.to<JsonObject>();
    o["ok"] = ok;
    o["action"] = action;
    rtc.toJson(o);
    String s;
    serializeJson(out, s);
    r->send(200, "application/json", s);
  });

  // --- Test połączenia z brokerem MQTT (Home Assistant) ---
  // Opcjonalnie przyjmuje nowe ustawienia w treści żądania, żeby można było
  // sprawdzić adres/użytkownika przed restartem stacji.
  onPost("/api/mqtt/test", [](AsyncWebServerRequest* r) {
    String body = bodyOf(r);
    if (body.length() > 2) config.applyJson(body.c_str(), body.length());

    String info;
    bool ok = mqtt.reconnectWithConfig(info);

    JsonDocument d;
    d["ok"] = ok;
    d["info"] = info;
    d["server"] = mqtt.serverDescription();
    d["prefix"] = config.mqttPrefix();
    d["topic_example"] = config.mqttPrefix() + "/sensor/temp/state";
    d["discovery_example"] = "homeassistant/sensor/" + config.mqttPrefix() + "_temp/config";
    d["user_set"] = config.mqttUser().length() > 0;
    String s;
    serializeJson(d, s);
    r->send(200, "application/json", s);
  });

  // --- Serwisy zewnętrzne (Windy, Weather Underground, ThingSpeak, ...) ---
  // Wysyłka próbna - działa też dla serwisów wyłączonych, żeby można było
  // sprawdzić wpisane dane dostępowe bez czekania na interwał.
  // Trasa podrzędna przed "/api/services" (patrz komentarz przy prognozie).
  onPost("/api/services/test", [](AsyncWebServerRequest* r) {
    weatherServices.requestNow();
    r->send(200, "application/json", "{\"ok\":true}");
  });

  // Odczyt ustawień i stanu ostatniej wysyłki w jednym miejscu - strona
  // rysuje z tego tabelę i formularz.
  server.on("/api/services", HTTP_GET, [](AsyncWebServerRequest* r) {
    r->send(200, "application/json", weatherServices.statusJson());
  });

  onPost("/api/services", [](AsyncWebServerRequest* r) {
    String b = bodyOf(r);
    if (b.length() == 0) {
      r->send(400, "application/json", jsonError("Puste żądanie - brak danych do zapisu"));
      return;
    }
    if (config.applyServicesJson(b.c_str(), b.length())) {
      LOG_I("Serwisy pogodowe: ustawienia zapisane ze strony www");
      r->send(200, "application/json", "{\"ok\":true}");
    } else {
      r->send(400, "application/json", jsonError("Błędny JSON"));
    }
  });

  // ============================================================
  //  Węzły po RS485 - zakładka "ESP i magistrala RS485"
  //
  //  Wszystkie ustawienia TEGO urządzenia (master) obsługują pozostałe
  //  trasy. Tutaj jest wyłącznie to, co dotyczy magistrali i węzłów:
  //   * /api/rs485       - stan magistrali, lista węzłów i ustawienia roli/wymiany
  //   * /api/rs485/action- polecenia jednorazowe (odpytywanie, skan, test, add/del/assign)
  //   * /api/node        - przezroczysty dostęp do węzła (?addr= lub ?i=)
  //  Trasa /api/rs485/action MUSI być zarejestrowana przed /api/rs485:
  //  ESPAsyncWebServer dopasowuje URI prefiksowo, więc "/api/rs485" obsłużyłby
  //  także "/api/rs485/action" (ta sama kolejność obowiązuje w pozostałych
  //  trasach, np. /api/forecast/analysis przed /api/forecast).
  // ============================================================

  // Polecenia jednorazowe. "a" można podać w treści żądania albo w adresie
  // (GET) - dzięki temu test protokołu da się uruchomić z przeglądarki.
  auto rs485Action = [](AsyncWebServerRequest* r) {
    // Treść żądania czytamy raz: zawiera nazwę polecenia ("a") i jego argumenty
    // (np. adres węzła przy dodawaniu, usuwaniu i zmianie adresu).
    JsonDocument req;
    {
      String body = bodyOf(r);
      if (body.length() && deserializeJson(req, body) != DeserializationError::Ok) {
        r->send(400, "application/json", jsonError("Błędny JSON"));
        return;
      }
    }
    String what = r->arg("a");
    if (!what.length()) what = String((const char*)(req["a"] | ""));
    if (!what.length()) {
      r->send(400, "application/json", jsonError("Brak nazwy polecenia (a)"));
      return;
    }

    // Adres węzła można podać w treści żądania albo w adresie (GET ?addr=N),
    // żeby polecenie dało się wywołać także z paska przeglądarki.
    int addrArg = -1, newArg = -1;
    if (req["addr"].is<int>()) addrArg = req["addr"].as<int>();
    else if (r->hasArg("addr")) addrArg = r->arg("addr").toInt();
    if (req["new"].is<int>()) newArg = req["new"].as<int>();
    else if (r->hasArg("new")) newArg = r->arg("new").toInt();
    String macArg = String((const char*)(req["mac"] | ""));
    if (!macArg.length() && r->hasArg("mac")) macArg = r->arg("mac");

    JsonDocument out;
    out["a"] = what;
    bool listChanged = false;

    if (what == "poll") {
      rs485.pollNow();
      out["ok"] = true;
    } else if (what == "scan") {
      uint8_t found[8];
      int n = rs485.scan(found, 8);
      JsonArray arr = out["found"].to<JsonArray>();
      for (int i = 0; i < n; i++) arr.add(found[i]);
      out["ok"] = true;
      listChanged = true;
    } else if (what == "add") {
      String err;
      if (addrArg < 0) { out["ok"] = false; out["err"] = "Brak adresu węzła (addr)"; }
      else if (rs485.addNode((uint8_t)addrArg, err)) { out["ok"] = true; listChanged = true; }
      else { out["ok"] = false; out["err"] = err; }
    } else if (what == "del") {
      String err;
      if (addrArg < 0) { out["ok"] = false; out["err"] = "Brak adresu węzła (addr)"; }
      else if (rs485.removeNode((uint8_t)addrArg, err)) { out["ok"] = true; listChanged = true; }
      else { out["ok"] = false; out["err"] = err; }
    } else if (what == "assign") {
      // Zmiana adresu węzła z www: master nadaje nowy adres po RS485,
      // "mac" (opcjonalny) wskazuje konkretny moduł.
      String err;
      if (addrArg < 0 || newArg < 0) {
        out["ok"] = false;
        out["err"] = "Brak adresu węzła (addr) lub nowego adresu (new)";
      } else if (rs485.assignAddr((uint8_t)addrArg, macArg, (uint8_t)newArg, err)) {
        out["ok"] = true;
        listChanged = true;
      } else {
        out["ok"] = false;
        out["err"] = err;
      }
    } else if (what == "sniff" || what == "scout") {
      // Diagnostyka przewodu magistrali: "sniff" nasłuchuje własnego pinu TX,
      // "scout" nadaje po kolei po podanych pinach i szuka odpowiedzi węzła.
      int pinArg = -1;
      if (req["pin"].is<int>()) pinArg = req["pin"].as<int>();
      else if (r->hasArg("pin")) pinArg = r->arg("pin").toInt();
      uint32_t msArg = 0;
      if (req["ms"].is<unsigned>()) msArg = req["ms"].as<unsigned>();
      else if (r->hasArg("ms")) msArg = (uint32_t)r->arg("ms").toInt();
      String pinsArg = String((const char*)(req["pins"] | ""));
      if (!pinsArg.length() && r->hasArg("pins")) pinsArg = r->arg("pins");

      PsramAllocator alloc;
      JsonDocument diag(&alloc);
      String err;
      bool ok = (what == "sniff") ? rs485.sniffTx(pinArg, msArg, diag, err)
                                  : rs485.scoutWire(pinsArg, msArg, diag, err);
      out["ok"] = ok;
      if (ok) out["diag"] = diag;
      else out["err"] = err;
    } else if (what == "restart") {
      // Restart portu magistrali bez restartu stacji: oddaje nadajnikowi i
      // odbiornikowi ich pady (przydatne po diagnostyce przewodu) i wznawia
      // odpytywanie węzłów od razu.
      bool ok = rs485.restartPort();
      out["ok"] = ok;
      if (!ok) out["err"] = "Magistrala wyłączona albo zajęta";
      listChanged = true;
    } else if (what == "selftest") {
      String info;
      out["ok"] = rs485.selftest(info);
      out["info"] = info;
    } else {
      // Polecenia przekazywane do drugiego ESP
      String cmd, args;
      if (what == "time_sync") {
        cmd = "time";
        args = "{\"epoch\":" + String((uint32_t)time(nullptr)) + "}";
      } else if (what == "reboot_peer") {
        cmd = "action";
        args = "{\"do\":\"reboot\"}";
      } else if (what == "discover_peer") {
        cmd = "action";
        args = "{\"do\":\"discover\"}";
      } else if (what == "clear_peer_log") {
        cmd = "log_clear";
      } else if (what == "pins_reset") {
        cmd = "pins_reset";
      } else if (what == "refresh") {
        cmd = "sync";
      } else {
        r->send(400, "application/json", jsonError("Nieznane polecenie"));
        return;
      }

      PsramAllocator alloc;
      JsonDocument peer(&alloc);
      String err;
      // Polecenie może iść do wybranego węzła z listy (reszta ruchu omija
      // ten moduł, więc nie może zmienić stanu odpytywanego teraz).
      uint8_t dst = addrArg > 0 ? (uint8_t)constrain(addrArg, 1, 247) : 0;
      if (rs485.requestTo(dst, cmd, args, peer, err, 2500)) {
        out["ok"] = true;
        out["node"] = peer["data"];
      } else {
        out["ok"] = false;
        out["err"] = err;
      }
    }

    if (listChanged) {
      // Lista węzłów zmieniła się - strona dostaje nowy stan w tej samej
      // odpowiedzi (bez dodatkowego zapytania).
      String st = rs485.stateJson();
      out["state"] = serialized(st);
    }

    String s;
    serializeJson(out, s);
    r->send(200, "application/json", s);
  };
  server.on("/api/rs485/action", HTTP_GET, rs485Action);
  onPost("/api/rs485/action", rs485Action);

  server.on("/api/rs485", HTTP_GET, [](AsyncWebServerRequest* r) {
    r->send(200, "application/json", rs485.stateJson());
  });

  onPost("/api/rs485", [](AsyncWebServerRequest* r) {
    String body = bodyOf(r);
    if (body.length() == 0) {
      r->send(400, "application/json", jsonError("Brak danych"));
      return;
    }
    JsonDocument d;
    if (deserializeJson(d, body) != DeserializationError::Ok) {
      r->send(400, "application/json", jsonError("Błędny JSON"));
      return;
    }

    // Zmiana roli (master <-> węzeł) obowiązuje dopiero po restarcie,
    // bo od niej zależy cały start stacji (Wi-Fi, MQTT, OTA).
    bool restart = false;
    if (d["master"].is<bool>() && d["master"].as<bool>() != config.rs485Master()) restart = true;

    JsonDocument cfg;
    if (d["enabled"].is<bool>()) cfg["rs485_enabled"] = d["enabled"].as<bool>();
    if (d["master"].is<bool>())  cfg["rs485_master"]  = d["master"].as<bool>();
    if (d["addr"].is<int>())     cfg["rs485_addr"]    = constrain(d["addr"].as<int>(), 1, 247);
    if (d["peer"].is<int>())     cfg["rs485_peer"]    = constrain(d["peer"].as<int>(), 0, 247);
    if (d["baud"].is<int>()) {
      int b = constrain(d["baud"].as<int>(), 1200, 921600);
      cfg["rs485_baud"] = (uint32_t)b;
    }
    if (d["poll_s"].is<int>())     cfg["rs485_poll_s"]     = constrain(d["poll_s"].as<int>(), 2, 3600);
    if (d["timeout_ms"].is<int>()) cfg["rs485_timeout_ms"] = constrain(d["timeout_ms"].as<int>(), 50, 5000);
    if (d["retries"].is<int>())    cfg["rs485_retries"]    = constrain(d["retries"].as<int>(), 0, 5);
    if (d["log"].is<bool>())       cfg["rs485_log"]        = d["log"].as<bool>();
    if (d["mqtt"].is<bool>())      cfg["rs485_mqtt"]       = d["mqtt"].as<bool>();
    if (d["slave_ap"].is<bool>())  cfg["rs485_slave_ap"]   = d["slave_ap"].as<bool>();

    if (cfg.size() > 0) {
      String s;
      serializeJson(cfg, s);
      if (!config.applyJson(s.c_str(), s.length())) {
        LOG_W("RS485: nie udało się zapisać ustawień (%u B)", (unsigned)s.length());
      }
      rs485.reload();
      LOG_I("RS485: ustawienia magistrali zmienione ze strony www");
    }

    JsonDocument out;
    out["ok"] = true;
    out["restart"] = restart;
    // Stan musi żyć do serializacji - serialized() trzyma wskaźnik, a nie kopię.
    String stateJson = rs485.stateJson();
    out["state"] = serialized(stateJson);
    String s;
    serializeJson(out, s);
    r->send(200, "application/json", s);
  });

  // Przezroczysty dostęp do drugiego ESP: /api/node?c=<polecenie>.
  // Wybór modułu: ?addr=N (adres na magistrali) albo ?i=N (numer węzła na
  // liście, 0 = pierwszy). Bez wyboru wymiana idzie z węzłem odpytywanym
  // teraz - tak jak dotąd przy jednym module.
  // Treść żądania (albo parametr "a") to argumenty polecenia.
  // Zapisy (pins_set, cfg_set, chan_set, action, time) przyjmujemy tylko
  // metodą POST - GET zostaje dla poleceń czytających.
  auto nodeProxy = [](AsyncWebServerRequest* r) {
    if (!rs485.enabled() || !rs485.started()) {
      r->send(400, "application/json", jsonError("Magistrala RS485 jest wyłączona"));
      return;
    }
    if (!rs485.isMaster()) {
      r->send(400, "application/json", jsonError("Ta stacja pracuje jako węzeł"));
      return;
    }
    String cmd = r->arg("c");
    if (!cmd.length()) {
      r->send(400, "application/json", jsonError("Brak parametru c (polecenie)"));
      return;
    }
    static const char* readOnly[] = {"ping", "hello", "sync", "status", "diag", "chan",
                                     "cfg", "pins", "log"};
    if (r->method() != HTTP_POST) {
      bool allowed = false;
      for (const char* c : readOnly) if (cmd == c) allowed = true;
      if (!allowed) {
        r->send(400, "application/json", jsonError("To polecenie wymaga metody POST"));
        return;
      }
    }

    String args = bodyOf(r);
    if (args.length() == 0) args = r->arg("a");
    if (args.length() && args[0] != '{' && args[0] != '[') {
      r->send(400, "application/json", jsonError("Argumenty muszą być obiektem JSON"));
      return;
    }

    // Wybrany moduł: jawny adres ma pierwszeństwo, potem numer na liście.
    uint8_t dst = 0;
    if (r->hasArg("addr")) {
      int a = r->arg("addr").toInt();
      if (a > 0 && a <= 247) dst = (uint8_t)a;
    } else if (r->hasArg("i")) {
      int i = r->arg("i").toInt();
      if (i >= 0 && i < rs485.nodeCount()) dst = rs485.node(i).addr;
      else if (i > 0) dst = (uint8_t)constrain(i, 1, 247);   // i jako adres
    }

    PsramAllocator alloc;
    JsonDocument out(&alloc);
    String err;
    bool ok;
    if (cmd == "chan" && args.length() == 0) {
      // Lista kanałów węzła nie mieści się w jednej ramce RS485 - sklejamy ją
      // ze stron, zachowując dla www ten sam kształt odpowiedzi co wcześniej.
      // Kanały dociągamy tylko dla znanego węzła z listy (inaczej trzeba by
      // podać, o który moduł chodzi) - nieznany adres nie może po cichu
      // zwrócić danych innego węzła.
      int idx = -1;
      if (dst) {
        idx = rs485.nodeIndexByAddr(dst);
        if (idx < 0) {
          r->send(400, "application/json",
                  jsonError("Nieznany adres węzła - najpierw dodaj go do listy"));
          return;
        }
      }
      ok = rs485.peerChannelsJson(idx, out, err);
    } else {
      ok = rs485.requestTo(dst, cmd, args, out, err, 3000);
    }
    if (!ok && out.isNull()) {   // brak odpowiedzi / błąd transmisji
      out["ok"] = false;
      out["err"] = err;
    }
    out["cmd"] = cmd;
    if (dst) out["node_addr"] = dst;
    String s;
    serializeJson(out, s);
    r->send(200, "application/json", s);
  };
  server.on("/api/node", HTTP_GET, nodeProxy);
  onPost("/api/node", nodeProxy);

  // --- Aktualizacja firmware / systemu plików przez www ---
  registerOtaRoutes(server);

  // --- Sterowanie ---
  server.on("/api/reboot", HTTP_POST, [](AsyncWebServerRequest* r) {
    r->send(200, "application/json", "{\"ok\":true}");
    g_rebootRequested = true;
  });

  server.on("/api/factoryreset", HTTP_POST, [](AsyncWebServerRequest* r) {
    r->send(200, "application/json", "{\"ok\":true}");
    g_factoryResetRequested = true;
  });

  // --- Pliki statyczne (SPA) i portal captive ---
  // no-cache: po uploadfs przeglądarka musi zweryfikować (ETag/Last-Modified) index.html i i18n.js,
  // inaczej trzyma stare tłumaczenia i nie widać nowych zakładek.
  server.serveStatic("/", LittleFS, "/", "no-cache, must-revalidate").setDefaultFile("index.html");

  server.onNotFound([](AsyncWebServerRequest* r) {
    if (r->method() == HTTP_GET && r->url().indexOf('.') < 0) {
      r->redirect("/");
    } else {
      r->send(404, "text/plain", "404");
    }
  });
}

String WebServerManager::mimeType(const String& path) {
  if (path.endsWith(".csv")) return "text/csv";
  if (path.endsWith(".html")) return "text/html";
  if (path.endsWith(".js")) return "application/javascript";
  if (path.endsWith(".css")) return "text/css";
  if (path.endsWith(".json")) return "application/json";
  if (path.endsWith(".png")) return "image/png";
  if (path.endsWith(".jpg") || path.endsWith(".jpeg")) return "image/jpeg";
  return "application/octet-stream";
}

void WebServerManager::begin(const IPAddress& apIP) {
  // Portal captive: wszystkie DNS-y -> IP stacji
  dnsServer.start(53, "*", apIP);

  registerRoutes();
  server.begin();
  LOG_I("Serwer www wystartował (port 80), portal: http://%s/", apIP.toString().c_str());
}

void WebServerManager::loop() {
  dnsServer.processNextRequest();

  // Skan Wi-Fi działa w osobnym zadaniu - gdy trwa zbyt długo, przestajemy
  // kazać stronie czekać (wynik i tak pojawi się po zakończeniu skanu).
  if (s_scanRunning && millis() - s_scanStarted > 30000UL) {
    s_scanRunning = false;
    if (s_scanError.length() == 0) {
      s_scanError = "przekroczono czas skanowania (30 s)";
      LOG_W("Skan Wi-Fi trwa dłużej niż 30 s - zgłaszam przekroczenie czasu");
    }
  }
}
