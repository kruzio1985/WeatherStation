/* =============================================================================
 * Stacja Pogody - stacja meteorologiczna na ESP32-S3 WROOM-1 (N16R8)
 * Copyright (c) 2026 kruzio1985 - https://github.com/kruzio1985
 * Licencja: Stacja Pogody Non-Commercial License 1.0 (plik LICENSE)
 * Użytek niekomercyjny. Kontakt: kruzio1985@users.noreply.github.com
 * =============================================================================
 */
// Status i diagnostyka jako JSON. Dokładnie ten sam tekst pokazuje strona www
// (zakładki "Pulpit" i "Diagnostyka") i odsyła węzeł RS485 na polecenie
// "status" / "diag" - dlatego kod jest wspólny dla mastera i dla węzła bez
// sieci (STACJA_HEADLESS w platformio.ini).
#include "api_json.h"

#include <LittleFS.h>
#include <time.h>
#include <esp_heap_caps.h>
#include <esp_ota_ops.h>

#include "config.h"
#include "board.h"
#include "pinmap.h"
#include "sensors.h"
#include "sd_card.h"
#include "led_ring.h"
#include "syslog.h"
#include "nvs_store.h"
#include "app_info.h"
#include "aqi.h"
#if !STACJA_HEADLESS
#include <WiFi.h>
#include "mqtt_client.h"
#include "weather_services.h"
#endif


// ------------------------------------------------------------
//  Budowanie statusu
// ------------------------------------------------------------
static String datetimeString() {
  time_t now = time(nullptr);
  if (now < 1000000000) return "brak synchronizacji NTP";
  struct tm tmv;
  localtime_r(&now, &tmv);
  char ts[32];
  snprintf(ts, sizeof(ts), "%04d-%02d-%02d %02d:%02d:%02d",
           tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
           tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
  return String(ts);
}

// Adres MAC z eFuse - dokładnie ten sam format (AA:BB:CC:DD:EE:FF), jaki daje
// WiFi.macAddress(). Węzeł bez sieci nie ma Wi-Fi, ale MAC nadal jest potrzebny
// (identyfikacja płytki na magistrali RS485 i w diagnostyce).
static String macString() {
  uint64_t mac = ESP.getEfuseMac();
  char b[18];
  snprintf(b, sizeof(b), "%02X:%02X:%02X:%02X:%02X:%02X",
           (unsigned)((mac >> 40) & 0xFF), (unsigned)((mac >> 32) & 0xFF),
           (unsigned)((mac >> 24) & 0xFF), (unsigned)((mac >> 16) & 0xFF),
           (unsigned)((mac >> 8) & 0xFF), (unsigned)(mac & 0xFF));
  return String(b);
}
 
// Serializacja JSON do Stringa z pominięciem przyrostowego String::concat,
// który przy dużych dokumentach (status ~8 KB) potrafił uszkadzać bajty w PSRAM.
// Dokument jest mierzony, serializowany do jednego bufora, a dopiero gotowy
// bufor kopiowany jedną operacją do Stringa.
static String jsonToString(JsonDocument& d) {
 size_t n = measureJson(d);
 if (n == 0) return String("{}");

 bool inPsram = false;
 char* buf = nullptr;
 if (psramFound()) {
   buf = (char*)heap_caps_malloc(n + 1, MALLOC_CAP_SPIRAM);
   inPsram = (buf != nullptr);
 }
 if (!buf) buf = (char*)malloc(n + 1);
 if (!buf) return String("{}");

 size_t w = serializeJson(d, buf, n + 1);
 String out;
 out.reserve(w + 1);
 out.concat(buf, w);
 if (inPsram) heap_caps_free(buf); else free(buf);
 return out;
}

static String buildStatus() {
  PsramAllocator alloc;
  JsonDocument d(&alloc);
  JsonObject o = d.to<JsonObject>();

  o["fw"] = FW_VERSION_FULL;
  o["build"] = String(__DATE__) + " " + String(__TIME__);
  o["board"] = ESP.getChipModel();
  o["flash_mb"] = (uint32_t)(ESP.getFlashChipSize() / (1024UL * 1024UL));
  o["psram_mb"] = (uint32_t)(ESP.getPsramSize() / (1024UL * 1024UL));
  o["board_id"] = boards::current().id;
  o["board_label"] = boards::current().label;
  o["board_short"] = boards::current().shortName;
  o["chip"] = boards::chipId();
  o["chip_name"] = boards::chipName();
  o["device"] = config.deviceName();
  o["location"] = config.location();
  o["uptime_s"] = (uint32_t)(millis() / 1000);

  time_t now = time(nullptr);
  o["time_synced"] = now >= 1000000000;
  o["datetime"] = datetimeString();

  // Wi-Fi
#if !STACJA_HEADLESS
  JsonObject w = o["wifi"].to<JsonObject>();
  w["ssid"] = config.wifiSsid();
  w["connected"] = WiFi.status() == WL_CONNECTED;
  w["rssi"] = WiFi.RSSI();
  w["ip"] = WiFi.localIP().toString();
  w["static"] = config.wifiStatic();
  w["static_ip"] = config.wifiIp();
  w["gateway"] = WiFi.gatewayIP().toString();
  w["mask"] = WiFi.subnetMask().toString();
  w["dns"] = WiFi.dnsIP().toString();
  w["hostname"] = config.hostname();
  w["ap_ssid"] = config.apSsid();
  w["ap_ip"] = WiFi.softAPIP().toString();
  w["ap_clients"] = WiFi.softAPgetStationNum();
  w["ap_hidden"] = config.apHidden();
  w["ap_always_on"] = config.apAlwaysOn();

  // MQTT
  JsonObject m = o["mqtt"].to<JsonObject>();
  m["configured"] = mqtt.isConfigured();
  m["connected"] = mqtt.connected();
  m["server"] = mqtt.serverDescription();
  m["prefix"] = config.mqttPrefix();
  m["user"] = config.mqttUser();
#else
  // Węzeł bez sieci: pola muszą istnieć (strona www mastera czyta je bez
  // zabezpieczeń), ale zawsze oznaczają "brak". Dane z tego węzła wysyła dalej
  // master - przez MQTT i serwisy pogodowe.
  JsonObject w = o["wifi"].to<JsonObject>();
  w["ssid"] = "";
  w["connected"] = false;
  w["ip"] = "-";
  w["hostname"] = config.hostname();
  w["note"] = "węzeł bez sieci - dane wysyła master";
  JsonObject m = o["mqtt"].to<JsonObject>();
  m["configured"] = false;
  m["connected"] = false;
  m["server"] = "węzeł bez sieci (MQTT po stronie mastera)";
  m["prefix"] = config.mqttPrefix();
  m["user"] = config.mqttUser();
#endif

  // Czas
  o["tz"] = config.tzString();
  o["ntp"] = config.ntpServer();

  // Karta SD
  JsonObject sd = o["sd"].to<JsonObject>();
  sd["mounted"] = sdCard.mounted();
  sd["total"] = sdCard.totalBytes();
  sd["used"] = sdCard.usedBytes();
  sd["mode"] = sdCard.usingSdmmc() ? "sdmmc" : "spi";
  // Opis interfejsu z ostatniej próby montażu (działa też bez karty) i piny,
  // na których stacja próbuje kartę - dzięki temu zakładka "Karta SD" pokazuje,
  // czy problemem jest karta, czy przypisanie pinów.
  sd["interface"] = sdCard.interfaceInfo();
  JsonObject sp = sd["pins"].to<JsonObject>();
  sp["cs"] = pinMap.pin("sd_cs");
  sp["sck"] = pinMap.pin("sd_sck");
  sp["mosi"] = pinMap.pin("sd_mosi");
  sp["miso"] = pinMap.pin("sd_miso");
  sp["mmc_clk"] = pinMap.pin("sdmmc_clk");
  sp["mmc_cmd"] = pinMap.pin("sdmmc_cmd");
  sp["mmc_d0"] = pinMap.pin("sdmmc_d0");
  if (!sdCard.mounted()) sd["error"] = sdCard.error();

  // Pamięć
  JsonObject mem = o["mem"].to<JsonObject>();
  mem["free_heap"] = ESP.getFreeHeap();
  mem["psram"] = ESP.getPsramSize();
  mem["free_psram"] = ESP.getFreePsram();

  // Kanały
  JsonArray arr = o["channels"].to<JsonArray>();
  for (auto& c : sensors.snapshot()) {
    JsonObject co = arr.add<JsonObject>();
    co["id"] = c.id;
    co["name"] = c.name;
    co["unit"] = c.unit;
    co["zone"] = c.zone;
    co["enabled"] = c.enabled;
    co["present"] = c.present;
    co["detected"] = c.detected;
    co["measured"] = c.measured;
    co["decimals"] = c.decimals;
    co["remote"] = c.remote;   // kanał z drugiego ESP (po RS485)
    co["remote_addr"] = c.remote ? c.remoteAddr : 0;   // adres węzła magistrali
    if (isnan(c.value)) co["value"] = nullptr;
    else co["value"] = c.value;
  }

  // Kierunek wiatru jako róża wiatrów (N, NE, E, ...) - obok stopni
  o["compass"] = sensors.windCompass();
  // Nazwa wykrytego czujnika jakości powietrza (SCD4x / SGP30 / PMS5003)
  o["aq_name"] = sensors.aqName();

  // Indeks jakości powietrza (CAQI) - lekki skrót do kafelka na pulpicie.
  // Pełne statystyki dnia są w /api/aqi (skanowanie CSV na żądanie).
  {
    AqiResult ar = aqiFromPm(sensors.valueOf("pm25"), sensors.valueOf("pm10"));
    JsonObject aq = o["aqi"].to<JsonObject>();
    aq["available"] = ar.available;
    aq["index"] = ar.index;
    aq["category"] = ar.category;
    aq["color"] = ar.color;
    if (isnan(ar.pm25)) aq["pm25"] = nullptr; else aq["pm25"] = ar.pm25;
    if (isnan(ar.pm10)) aq["pm10"] = nullptr; else aq["pm10"] = ar.pm10;
  }

  // Pierścień LED - skrót dla nagłówka strony i pulpitu
  JsonObject rg = o["ring"].to<JsonObject>();
  rg["enabled"] = ledRing.enabled();
  rg["brightness"] = ledRing.brightness();
  rg["state"] = (int)ledRing.state();
  rg["state_name"] = ledRing.stateName();
  rg["color"] = ledRing.stateColor();
  rg["count"] = ledRing.ledCount();
  rg["power_ma"] = ledRing.estimatedMilliAmps();

  return jsonToString(d);
}

// ------------------------------------------------------------
//  Diagnostyka (zakładka "Diagnostyka")
//  Wynik jest listą kart - strona www rysuje je bez zmian.
// ------------------------------------------------------------
static String fmtBytes(uint64_t b) {
  char t[32];
  if (b >= 1073741824ULL)     snprintf(t, sizeof(t), "%.2f GB", b / 1073741824.0);
  else if (b >= 1048576ULL)   snprintf(t, sizeof(t), "%.2f MB", b / 1048576.0);
  else if (b >= 1024ULL)      snprintf(t, sizeof(t), "%.1f kB", b / 1024.0);
  else                        snprintf(t, sizeof(t), "%llu B", (unsigned long long)b);
  return String(t);
}

static String fmtUptime(uint32_t sec) {
  uint32_t d = sec / 86400, h = (sec % 86400) / 3600, m = (sec % 3600) / 60, s = sec % 60;
  char t[40];
  if (d)      snprintf(t, sizeof(t), "%ud %uh %um", (unsigned)d, (unsigned)h, (unsigned)m);
  else if (h) snprintf(t, sizeof(t), "%uh %um %us", (unsigned)h, (unsigned)m, (unsigned)s);
  else        snprintf(t, sizeof(t), "%um %us", (unsigned)m, (unsigned)s);
  return String(t);
}

static JsonArray diagCard(JsonDocument& d, const char* title) {
  JsonObject c = d["cards"].add<JsonObject>();
  c["title"] = title;
  return c["rows"].to<JsonArray>();
}

// tone: "ok" | "warn" | "bad" | nullptr (bez koloru)
static void diagRow(JsonArray& rows, const char* k, const String& v, const char* tone = nullptr) {
  JsonObject r = rows.add<JsonObject>();
  r["k"] = k;
  r["v"] = v;
  if (tone) r["t"] = tone;
}

static String buildDiagnostics() {
  PsramAllocator alloc;
  JsonDocument d(&alloc);

  d["uptime_s"] = (uint32_t)(millis() / 1000);
  d["reset_reason"] = syslog.resetReason();
  d["boot_count"] = syslog.bootCount();
  d["log_errors"] = syslog.errors();
  d["log_warns"] = syslog.warns();
  d["log_size"] = (uint32_t)syslog.size();

  float cpu = temperatureRead();
  bool cpuOk = !isnan(cpu);
  if (cpuOk) d["cpu_temp_c"] = cpu;
  else       d["cpu_temp_c"] = nullptr;

  // --- System ---
  JsonArray ss = diagCard(d, "System");
  diagRow(ss, "Wersja firmware", FW_VERSION_FULL);
  diagRow(ss, "Kompilacja", String(__DATE__) + " " + String(__TIME__));
  diagRow(ss, "Czas działania", fmtUptime(millis() / 1000));
  diagRow(ss, "Powód resetu", syslog.resetReason(),
          strcmp(syslog.resetReason(), "włączenie zasilania") == 0 ? "ok" : "warn");
  diagRow(ss, "Liczba uruchomień", String((unsigned)syslog.bootCount()));
  diagRow(ss, "Układ", String(ESP.getChipModel()) + " r" + String((unsigned)ESP.getChipRevision()));
  diagRow(ss, "Rdzenie", String((unsigned)ESP.getChipCores()) + " x " + String((unsigned)ESP.getCpuFreqMHz()) + " MHz");
  diagRow(ss, "Temperatura układu", cpuOk ? String(cpu, 1) + " °C" : String("—"),
          !cpuOk ? nullptr : (cpu > 75 ? "bad" : (cpu > 60 ? "warn" : "ok")));
  diagRow(ss, "Czas lokalny", String(datetimeString()));

  // --- Pamięć ---
  JsonArray mm = diagCard(d, "Pamięć");
  diagRow(mm, "Wolny RAM", fmtBytes(ESP.getFreeHeap()),
          ESP.getFreeHeap() < 50000 ? "warn" : "ok");
  diagRow(mm, "Najmniejszy zapas RAM", fmtBytes(ESP.getMinFreeHeap()),
          ESP.getMinFreeHeap() < 30000 ? "warn" : "ok");
  diagRow(mm, "Największy wolny blok", fmtBytes(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
  if (psramFound()) {
    uint32_t freeP = ESP.getFreePsram(), totP = ESP.getPsramSize();
    diagRow(mm, "PSRAM (wolny / razem)", fmtBytes(freeP) + " / " + fmtBytes(totP), "ok");
    diagRow(mm, "Tryb PSRAM", String("octal (OPI), ") + String(ESP.getPsramSize() / 1048576) + " MB");
  } else {
    diagRow(mm, "PSRAM", "BRAK - sprawdź -DBOARD_HAS_PSRAM", "bad");
  }
  diagRow(mm, "Rozmiar programu", fmtBytes(appImageSize()));
  diagRow(mm, "Wolne miejsce na program", fmtBytes(appImageFreeBytes()));
  diagRow(mm, "Bufor logów", fmtBytes(syslog.capacity()));

  // --- Pamięć masowa ---
  JsonArray ff = diagCard(d, "Pamięć masowa");
  diagRow(ff, "LittleFS (www)",
          fmtBytes(LittleFS.usedBytes()) + " / " + fmtBytes(LittleFS.totalBytes()), "ok");
  diagRow(ff, "Karta SD",
          sdCard.mounted() ? (String("OK (") + (sdCard.usingSdmmc() ? "SDMMC" : "SPI") + ")") : String("brak"),
          sdCard.mounted() ? "ok" : "warn");
  if (sdCard.mounted()) {
    diagRow(ff, "SD zajęte / razem", fmtBytes(sdCard.usedBytes()) + " / " + fmtBytes(sdCard.totalBytes()));
    diagRow(ff, "Interfejs SD", sdCard.usingSdmmc() ? "SDMMC 1-bit" : "SPI");
  } else {
    diagRow(ff, "Powód braku SD", sdCard.error());
    diagRow(ff, "Interfejs SD (próba)", sdCard.interfaceInfo());
  }
  diagRow(ff, "Piny SD",
          "SPI: CS " + String(pinMap.pin("sd_cs")) + ", SCK " + String(pinMap.pin("sd_sck")) +
              ", MOSI " + String(pinMap.pin("sd_mosi")) + ", MISO " + String(pinMap.pin("sd_miso")));
  diagRow(ff, "Zapis logów CSV",
          sdCard.mounted() ? ("co " + String((unsigned)config.logIntervalS()) + " s na kartę SD")
                           : String("wyłączony (brak karty)"),
          sdCard.mounted() ? "ok" : nullptr);

  NvsPartitionInfo nvs = nvsPartitionInfo();
  if (nvs.ok) {
    diagRow(ff, "Ustawienia w NVS",
            fmtBytes(nvs.sizeBytes) + ", wpisy wolne " + String((unsigned)nvs.freeEntries) +
                " / " + String((unsigned)nvs.totalEntries),
            nvs.freeEntries < 20 ? "bad" : (nvs.freeEntries < 60 ? "warn" : "ok"));
    diagRow(ff, "Ustawienia (rozmiar / kopia)",
            fmtBytes(nvsStoreSize(NVS_NS, NVS_KEY_CFG)) + " w NVS" +
                (LittleFS.exists(CONFIG_FILE) ? String(" + /config.json") : String(" (brak kopii)")));
    diagRow(ff, "Ustawienia przetrwają aktualizację",
            "tak - OTA i wgranie www nie kasują partycji NVS", "ok");
  } else {
    diagRow(ff, "Ustawienia w NVS", "partycja NVS niedostępna!", "bad");
  }

  // --- Sieć ---
#if STACJA_HEADLESS
  JsonArray nn = diagCard(d, "Sieć");
  diagRow(nn, "Tryb", "węzeł RS485 - zbiera dane z czujników", "ok");
  diagRow(nn, "Adres MAC", macString());
  diagRow(nn, "Nazwa", config.hostname());
  diagRow(nn, "Wi-Fi / strona www / MQTT", "brak - dane wysyła master po RS485");
  diagRow(nn, "Zasilanie i magistrala", "5 V z mastera, RS485 " + String(STACJA_NODE_BAUD) + " b/s");
#else
  bool sta = WiFi.status() == WL_CONNECTED;
  int rssi = WiFi.RSSI();
  JsonArray nn = diagCard(d, "Sieć");
  diagRow(nn, "Tryb", sta ? "STA + AP" : "tylko AP", sta ? "ok" : "warn");
  diagRow(nn, "SSID", config.wifiSsid().length() ? config.wifiSsid() : String("—"));
  diagRow(nn, "Adresacja", config.wifiStatic() ? "statyczna" : "DHCP");
  diagRow(nn, "Adres IP", sta ? WiFi.localIP().toString() : String("—"));
  diagRow(nn, "Brama", sta ? WiFi.gatewayIP().toString() : String("—"));
  diagRow(nn, "Maska", sta ? WiFi.subnetMask().toString() : String("—"));
  diagRow(nn, "DNS", sta ? WiFi.dnsIP().toString() : String("—"));
  diagRow(nn, "Sygnał", sta ? String(rssi) + " dBm" : String("—"),
          !sta ? nullptr : (rssi < -80 ? "bad" : (rssi < -70 ? "warn" : "ok")));
  diagRow(nn, "Kanał", sta ? String((unsigned)WiFi.channel()) : String("—"));
  diagRow(nn, "Adres MAC", WiFi.macAddress());
  diagRow(nn, "Nazwa sieciowa", config.hostname() + ".local");
  diagRow(nn, "Punkt dostępu", config.apSsid() + " @ " + WiFi.softAPIP().toString());
  diagRow(nn, "Klienci AP", String((unsigned)WiFi.softAPgetStationNum()));
#endif

  // --- MQTT ---
#if STACJA_HEADLESS
  // Węzeł bez sieci wysyła dane tylko po RS485 - MQTT i Home Assistant
  // obsługuje master, dlatego tutaj widać wyłącznie informację o tym.
  JsonArray mq = diagCard(d, "MQTT / Home Assistant");
  diagRow(mq, "Wysyłka", "po stronie mastera (ten węzeł nie ma Wi-Fi)", nullptr);
  diagRow(mq, "Prefiks tematów", config.mqttPrefix());
#else
  JsonArray mq = diagCard(d, "MQTT / Home Assistant");
  if (mqtt.isConfigured()) {
    diagRow(mq, "Serwer", config.mqttHost() + ":" + String((unsigned)config.mqttPort()));
  } else {
    diagRow(mq, "Serwer", "nie skonfigurowano", "warn");
  }
  diagRow(mq, "Stan", mqtt.connected() ? "połączony" : "rozłączony",
          mqtt.connected() ? "ok" : (mqtt.isConfigured() ? "bad" : "warn"));
  diagRow(mq, "Prefiks tematów", config.mqttPrefix());
  diagRow(mq, "Discovery HA", mqtt.connected() ? "wysłane" : "czeka", mqtt.connected() ? "ok" : nullptr);
#endif

  // --- Serwisy pogodowe (wysyłka danych poza dom, obok MQTT) ---
#if STACJA_HEADLESS
  JsonArray ws = diagCard(d, "Serwisy pogodowe");
  diagRow(ws, "Wysyłka", "po stronie mastera (ten węzeł nie ma Wi-Fi)", nullptr);
  diagRow(ws, "Interwał", String((unsigned)config.svcInterval()) + " s");
#else
  JsonArray ws = diagCard(d, "Serwisy pogodowe");
  diagRow(ws, "Udostępnianie danych", config.svcSectionEnabled() ? "włączone" : "wyłączone",
          config.svcSectionEnabled() ? "ok" : nullptr);
  diagRow(ws, "Interwał wysyłki", String((unsigned)config.svcInterval()) + " s");
  diagRow(ws, "Zakres danych", config.svcExternalOnly() ? "tylko czujniki zewnętrzne"
                                                        : "wszystkie czujniki");
  {
    String tch = config.svcTop("temp_ch");
    diagRow(ws, "Temperatura do wysyłki", tch.length() ? tch : String("automatycznie (zewnętrzna)"));
  }
  for (uint8_t i = 0; i < SVC_COUNT; i++) {
    WeatherServices::Snapshot s = weatherServices.lastResult(i);
    String info;
    const char* tone = nullptr;
    if (s.skipped) {
      info = config.svcEnabled(SVC_NAMES[i]) ? "brak danych dostępowych" : "wyłączony";
      if (config.svcEnabled(SVC_NAMES[i])) tone = "warn";
    } else if (s.ok) {
      info = String("OK (HTTP ") + String(s.code) + "), " +
             (s.at ? fmtUptime((millis() - s.at) / 1000) + " temu" : String("teraz"));
      tone = "ok";
    } else {
      info = s.info.length() ? s.info : String("błąd wysyłki");
      tone = "bad";
    }
    diagRow(ws, SVC_LABELS[i], info, tone);
  }
#endif

  // --- Aktualizacje ---
  const esp_partition_t* run = esp_ota_get_running_partition();
  const esp_partition_t* nxt = esp_ota_get_next_update_partition(nullptr);
  JsonArray oo = diagCard(d, "Aktualizacje (OTA)");
  diagRow(oo, "Aktywna partycja", run ? String(run->label) : String("?"), "ok");
  diagRow(oo, "Następna aktualizacja", nxt ? String(nxt->label) : String("?"), "ok");
  diagRow(oo, "Rozmiar slotu", nxt ? fmtBytes(nxt->size) : String("?"));
#if STACJA_HEADLESS
  diagRow(oo, "Aktualizacja", "kablem USB (ten węzeł nie ma Wi-Fi / OTA)", "ok");
#else
  diagRow(oo, "Aktualizacja przez przeglądarkę", "zakładka Aktualizacja", "ok");
  diagRow(oo, "ArduinoOTA", config.hostname() + ".local : 3232");
  diagRow(oo, "Hasło OTA", config.otaPassword().length() ? "ustawione" : "BRAK",
          config.otaPassword().length() ? "ok" : "warn");
#endif

  // --- Czujniki ---
  auto chans = sensors.snapshot();
  unsigned enabled = 0, present = 0, detected = 0, inCnt = 0, outCnt = 0;
  for (auto& c : chans) {
    if (c.enabled) enabled++;
    if (c.present) present++;
    if (c.detected) detected++;
    if (c.zone == "in") inCnt++; else outCnt++;
  }
  JsonArray cc = diagCard(d, "Czujniki");
  diagRow(cc, "Kanały", String((unsigned)chans.size()) + " (aktywne " + String(enabled) + ")");
  diagRow(cc, "Wykryte", String(detected) + " / " + String((unsigned)chans.size()),
          detected ? "ok" : "warn");
  diagRow(cc, "Skonfigurowane", String(present) + " / " + String((unsigned)chans.size()) +
              (present > detected ? String(" (") + String(present - detected) + " czeka na dane)" : String()),
          present > detected ? "warn" : nullptr);
  diagRow(cc, "DS18B20 na magistrali", String((unsigned)sensors.dsSensorCount()),
          sensors.dsSensorCount() ? "ok" : "warn");
  diagRow(cc, "Pyłomierz (PMS5003)",
          pinMap.pin("pms_rx") < 0 ? String("pin nieprzypisany")
                                   : (sensors.pmsDetected() ? String("wykryty")
                                                            : String("czeka na ramkę")),
          pinMap.pin("pms_rx") < 0 ? nullptr : (sensors.pmsDetected() ? "ok" : "warn"));
  // Czujniki jakości powietrza na I2C (SCD4x - CO2, SGP30 - eCO2/TVOC)
  diagRow(cc, "Jakość powietrza (I2C)",
          sensors.aqName().length() ? sensors.aqName() : String("nie wykryto (SCD4x / SGP30)"),
          sensors.aqName().length() ? "ok" : "warn");
  String extraAq = sensors.extraAqName();
  diagRow(cc, "Pyłomierz wewnętrzny (SEN5x / SPS30)",
          extraAq.length() ? extraAq : String("nie wykryto (SEN5x / SPS30)"), extraAq.length() ? "ok" : "warn");
  diagRow(cc, "Strefy czujników",
          String("wewnętrzne ") + String(inCnt) + ", zewnętrzne " + String(outCnt));
  diagRow(cc, "Kamera", "planowana", nullptr);

  // --- Pierścień LED RGB ---
  JsonArray rr = diagCard(d, "Pierścień LED RGB (status pogody)");
  diagRow(rr, "Sterownik", ledRing.isReady() ? "aktywny" : "nieaktywny",
          ledRing.isReady() ? "ok" : "bad");
  diagRow(rr, "Pin danych", ledRing.pin() >= 0 ? String("GPIO ") + String(ledRing.pin())
                                              : String("brak"), 
          ledRing.pin() >= 0 ? nullptr : "warn");
  diagRow(rr, "Liczba diod", String((unsigned)ledRing.ledCount()));
  diagRow(rr, "Kolejność kolorów", ledRing.orderName());
  diagRow(rr, "Włączony", ledRing.enabled() ? "tak" : "nie",
          ledRing.enabled() ? "ok" : nullptr);
  diagRow(rr, "Jasność", String((unsigned)ledRing.brightness()) + " %");
  diagRow(rr, "Stan pogody", ledRing.stateName() + " (" + ledRing.stateReason() + ")");
  diagRow(rr, "Pobór prądu (teraz)", String((unsigned)ledRing.estimatedMilliAmps()) + " mA");
  diagRow(rr, "Pobór maks. (biel)", String((unsigned)ledRing.maxMilliAmps()) + " mA",
          ledRing.maxMilliAmps() > ledRing.maxMa() ? "warn" : "ok");
  diagRow(rr, "Limit prądu z ustawień", String((unsigned)ledRing.maxMa()) + " mA");
  diagRow(rr, "Efekty / wygaszanie w nocy",
          String(ledRing.effects() ? "efekty" : "stały kolor") + " / " +
          String(ledRing.offAtNight() ? "noc 5%" : "bez wygaszania"));

  return jsonToString(d);
}

// Wersje "na wynos" dla magistrali RS485 - węzeł odsyła masterowi dokładnie
// ten sam JSON, który normalnie trafia do przeglądarki.
String apiStatusJson()      { return buildStatus(); }
String apiDiagnosticsJson() { return buildDiagnostics(); }
