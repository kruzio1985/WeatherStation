/* =============================================================================
 * Stacja Pogody - stacja meteorologiczna na ESP32-S3 WROOM-1 (N16R8)
 * Copyright (c) 2026 kruzio1985 - https://github.com/kruzio1985
 * Licencja: PolyForm Noncommercial License 1.0.0 (plik LICENSE)
 * Użytek niekomercyjny. Kontakt: kruzio1985@users.noreply.github.com
 * =============================================================================
 */
#include "config.h"
#include <LittleFS.h>

#include "nvs_store.h"
#include "syslog.h"

// Ustawienia trzymamy przede wszystkim w partycji NVS - nie kasuje jej ani
// aktualizacja programu (OTA), ani wgranie plików www do LittleFS.
// Pliki /config.json i /state.json zostają jako kopia zapasowa i jako źródło
// danych przy pierwszym starcie po aktualizacji ze starszej wersji.

ConfigManager config;

// Żądania restartu i przywrócenia ustawień fabrycznych. Ustawia je strona www
// (web_server.cpp) oraz polecenie "action" z magistrali RS485; wykonuje main.cpp.
volatile bool g_rebootRequested = false;
volatile bool g_factoryResetRequested = false;

// Znacznik roli, z jaką zbudowano firmware (0 = brak zapisanego znacznika,
// czyli urządzenie po raz pierwszy widzi tę wersję .bin).
#define ROLE_BUILD_MASTER 1
#define ROLE_BUILD_NODE   2

// ArduinoJson v7: to<T>() najpierw ZAWSZE wyczyszcza element, na którym zostało
// wywołane - a JsonDocument::to<T>() czysci caly dokument (JsonDocument.hpp:
// clear() + to<T>()). Dlatego do dokladania czegokolwiek do istniejacych
// ustawien uzywamy as<T>() i tworzymy obiekt/tablice tylko wtedy, gdy jeszcze
// nie istnieje - inaczej jedno to<T>() kasuje wszystkie wczesniejsze wpisy.
// Uwaga: parametr musi pozostać proxy (MemberProxy/ElementProxy) przekazanym
// przez referencję - MemberProxy jest niekopiowalny, a JsonVariant utworzony
// z proxy przez getData() traci zdolność tworzenia brakującego klucza
// (to<T>() dostaje wtedy null i nic nie zapisuje). Dzięki szablonowi
// as<T>()/to<T>() wołane są na proxy, które przez getOrCreateData() potrafi
// dodać brakujący element.
template <typename TRef>
static JsonObject objIn(TRef&& v) {
  JsonObject o = v.template as<JsonObject>();
  return o.isNull() ? v.template to<JsonObject>() : o;
}
template <typename TRef>
static JsonArray arrIn(TRef&& v) {
  JsonArray a = v.template as<JsonArray>();
  return a.isNull() ? v.template to<JsonArray>() : a;
}
static JsonObject rootObj(JsonDocument& d) {
  JsonObject o = d.as<JsonObject>();
  return o.isNull() ? d.to<JsonObject>() : o;
}

void ConfigManager::lock() {
  if (mutex_) xSemaphoreTakeRecursive(mutex_, portMAX_DELAY);
}
void ConfigManager::unlock() {
  if (mutex_) xSemaphoreGiveRecursive(mutex_);
}

void ConfigManager::ensureDefaults() {
  JsonObject o = rootObj(doc_);

  auto s = [&](const char* k, const char* v) { if (!o[k].is<const char*>()) o[k] = v; };
  s("wifi_ssid", "");
  s("wifi_pass", "");
  s("ap_ssid", "StacjaPogody");
  s("ap_pass", "12345678");
  s("ap_ip", "192.168.4.1");
  s("hostname", "stacja-pogody");
  s("ota_password", "stacja-ota");
  s("device_name", "Stacja Pogody");
  s("location", "Dom");
  s("mqtt_host", "");
  s("mqtt_user", "");
  s("mqtt_pass", "");
  s("mqtt_prefix", "stacja_pogody");
  s("ntp_server", "pool.ntp.org");
  s("tz_string", "CET-1CEST,M3.5.0,M10.5.0/3");
  s("company_name", "");
  s("company_url", "");

  // Statyczny adres IP stacji (puste = DHCP)
  s("wifi_ip", "");
  s("wifi_gw", "");
  s("wifi_mask", "255.255.255.0");
  s("wifi_dns", "");

  if (!o["wifi_static"].is<bool>()) o["wifi_static"] = false;
  if (!o["ap_hidden"].is<bool>()) o["ap_hidden"] = false;
  if (!o["ap_always_on"].is<bool>()) o["ap_always_on"] = true;
  if (!o["mqtt_port"].is<uint16_t>()) o["mqtt_port"] = 1883;
  if (!o["log_interval_s"].is<uint32_t>()) o["log_interval_s"] = 60;
  // Retencja plików na karcie SD (0 = wyłączone)
  if (!o["sd_retention_days"].is<uint16_t>()) o["sd_retention_days"] = 0;
  if (!o["sd_retention_photos_days"].is<uint16_t>()) o["sd_retention_photos_days"] = 0;
  if (!o["aq_type"].is<uint8_t>()) o["aq_type"] = 0;   // 0 = wykryj sam

  // Kamera (opcjonalna). Domyślnie wyłączona - włącza się dopiero, gdy
  // fizycznie podłączona (i skonfigurowane piny w sekcji "pins").
  if (!o["cam_enabled"].is<bool>()) o["cam_enabled"] = false;
  if (!o["cam_interval_min"].is<uint16_t>()) o["cam_interval_min"] = 15;
  if (!o["cam_resolution"].is<uint8_t>()) o["cam_resolution"] = 8;  // 8 = VGA (640x480)
  if (!o["cam_quality"].is<uint8_t>()) o["cam_quality"] = 12;
  if (!o["cam_model"].is<uint8_t>()) o["cam_model"] = 0;            // 0 = wykryj sam
  // Limity miejsca na zdjęcia (0 = bez limitu). Po przekroczeniu najstarsze
  // zdjęcia są kasowane razem z ich plikami analizy (.json).
  if (!o["cam_max_photos"].is<uint16_t>()) o["cam_max_photos"] = 0;
  if (!o["cam_max_mb"].is<uint16_t>()) o["cam_max_mb"] = 0;
  // Adres zewnętrznej kamery (ESP32-CAM). Puste = kamera lokalna na pinach.
  // Po ustawieniu master tylko odbiera zdjęcia (HTTP POST /api/camera/upload).
  if (!o["cam_remote_url"].is<const char*>()) o["cam_remote_url"] = "";

  // Serwisy zewnętrzne (udostępnianie pomiarów poza MQTT/Home Assistant)
  {
    JsonObject svc = objIn(o["services"]);
    if (!svc["enabled"].is<bool>()) svc["enabled"] = false;
    if (!svc["interval"].is<uint16_t>()) svc["interval"] = 300;   // sekundy
    if (!svc["external_only"].is<bool>()) svc["external_only"] = false;
    // Kanał, z którego bierzemy temperaturę dla serwisów pogodowych
    // (puste = automatycznie: najpierw czujniki zewnętrzne)
    if (!svc["temp_ch"].is<const char*>()) svc["temp_ch"] = "";
    for (size_t i = 0; i < SVC_COUNT; i++) {
      JsonObject s = objIn(svc[SVC_NAMES[i]]);
      if (!s["enabled"].is<bool>()) s["enabled"] = false;
      if (!s["id"].is<const char*>()) s["id"] = "";
      if (!s["key"].is<const char*>()) s["key"] = "";
      if (!s["url"].is<const char*>()) s["url"] = "";
    }
  }

  // Pierścień LED RGB (WS2812B / RGBIC)
  if (!o["rgb_enabled"].is<bool>()) o["rgb_enabled"] = true;
  if (!o["rgb_brightness"].is<uint8_t>()) o["rgb_brightness"] = 60;
  if (!o["rgb_effects"].is<bool>()) o["rgb_effects"] = true;
  if (!o["rgb_off_at_night"].is<bool>()) o["rgb_off_at_night"] = false;
  if (!o["rgb_count"].is<uint16_t>()) o["rgb_count"] = 36;
  if (!o["rgb_order"].is<uint8_t>()) o["rgb_order"] = 0;      // 0=GRB 1=RGB 2=BRG
  if (!o["rgb_max_ma"].is<uint16_t>()) o["rgb_max_ma"] = 900; // limit prądu pierścienia

  // Kolory stanów pogody - tablica 10 wartości 0xRRGGBB (kolejność wg WeatherState)
  if (!o["rgb_colors"].is<JsonArray>()) {
    JsonArray cols = o["rgb_colors"].to<JsonArray>();
    for (size_t i = 0; i < WEATHER_STATE_COUNT; i++) cols.add((uint32_t)WEATHER_COLOR_DEFAULTS[i]);
  }
  // Własne nazwy stanów pogody - tablica 10 tekstów, puste = nazwa domyślna
  if (!o["rgb_names"].is<JsonArray>()) {
    JsonArray names = o["rgb_names"].to<JsonArray>();
    for (size_t i = 0; i < WEATHER_STATE_COUNT; i++) names.add("");
  }
  if (!o["channels"].is<JsonObject>()) o["channels"].to<JsonObject>();
  if (!o["pins"].is<JsonObject>()) o["pins"].to<JsonObject>();

  // Magistrala RS485 - drugi ESP (węzeł z zewnętrznymi czujnikami).
  // Wartości domyślne zależą od tego, którą wersję firmware wgrano:
  //   bin 1 (master): magistrala włączona, adres 1, drugi ESP = 0 (wykryj sam)
  //   bin 2 (węzeł) : magistrala włączona, rola węzeł, adres = STACJA_NODE_ADDR
  // Dzięki temu urządzenia widzą się od razu po wgraniu, bez ręcznej
  // konfiguracji. Gotowe binarki "z adresem" (flagi -DSTACJA_NODE_ADDR=N)
  // ustawiają inny adres fabryczny, więc kilka świeżych modułów można od razu
  // podłączyć bez kolizji. Rola i adres z magazynu ustawień (gdy już są) mają
  // pierwszeństwo - patrz applyBuildRole().
  if (!o["rs485_enabled"].is<bool>()) o["rs485_enabled"] = true;
  if (!o["rs485_master"].is<bool>()) o["rs485_master"] = (STACJA_ROLE_MASTER != 0);
  if (!o["rs485_addr"].is<uint8_t>()) o["rs485_addr"] = STACJA_ROLE_MASTER ? 1 : STACJA_NODE_ADDR;
  if (!o["rs485_peer"].is<uint8_t>()) o["rs485_peer"] = STACJA_ROLE_MASTER ? 0 : STACJA_NODE_ADDR;
  // Lista adresów węzłów odpytywanych przez mastera, np. "2,3,4" (pusta =
  // wykryj sam). Jeden uniwersalny plik .bin węzła (adres 2) oraz gotowe
  // binarki -addrN obsługują wszystkie moduły - adres można też zmienić z www
  // (master nadaje go po RS485).
  if (!o["rs485_nodes"].is<const char*>()) o["rs485_nodes"] = "";
  if (!o["rs485_baud"].is<uint32_t>()) o["rs485_baud"] = 19200;
  if (!o["rs485_poll_s"].is<uint16_t>()) o["rs485_poll_s"] = 30;
  if (!o["rs485_timeout_ms"].is<uint16_t>()) o["rs485_timeout_ms"] = 400;
  if (!o["rs485_retries"].is<uint8_t>()) o["rs485_retries"] = 2;
  if (!o["rs485_log"].is<bool>()) o["rs485_log"] = true;
  if (!o["rs485_mqtt"].is<bool>()) o["rs485_mqtt"] = true;
  if (!o["rs485_slave_ap"].is<bool>()) o["rs485_slave_ap"] = true;
  // true = użytkownik zmienił prędkość w awaryjnym AP węzła headless;
  // wtedy ta wartość ma pierwszeństwo przed STACJA_NODE_BAUD z kompilacji.
  if (!o["rs485_baud_manual"].is<bool>()) o["rs485_baud_manual"] = false;
}

bool ConfigManager::begin() {
  mutex_ = xSemaphoreCreateRecursiveMutex();
  lock();
  ensureDefaults();

  // 1) Partycja NVS - najważniejsze źródło (przetrwa aktualizację firmware).
  String json = nvsStoreRead(NVS_NS, NVS_KEY_CFG);
  const char* src = "NVS";

  // 2) Kopia na LittleFS: źródło danych przy pierwszej aktualizacji ze
  //    starszej wersji (albo gdy NVS zostanie skasowana).
  File f = LittleFS.open(CONFIG_FILE, "r");
  if (f) {
    size_t sz = f.size();
    String s;
    s.reserve(sz);
    while (f.available()) s += (char)f.read();
    f.close();
    if (json.length() == 0 && s.length()) {
      json = s;
      src = "LittleFS (migracja ustawień)";
    }
  }

  if (json.length()) {
    JsonDocument fd;
    DeserializationError err = deserializeJson(fd, json);
    if (!err) {
      JsonObject fo = fd.as<JsonObject>();
      for (JsonPair kv : fo) doc_[kv.key()] = kv.value();
      LOG_I("Ustawienia z %s (%u B)", src, (unsigned)json.length());
    } else {
      LOG_E("Ustawienia: uszkodzony JSON z %s - zostają wartości domyślne", src);
    }
  } else {
    LOG_I("Ustawienia: brak zapisanej konfiguracji - zapisuję wartości domyślne");
  }
  unlock();

  // Dopasowanie ustawień magistrali do wgranej wersji .bin (bin 1 / bin 2).
  applyBuildRole();

  loadState();
  save();
  lock();
  const size_t keys = doc_.size();
  unlock();
  LOG_I("Ustawienia: gotowe (%u kluczy)", (unsigned)keys);
  return true;
}

// Rola magistrali wybrana przy kompilacji trafia do ustawień. Dzięki temu gdy
// wgrasz drugą wersję firmware (master <-> węzeł), blok magistrali RS485
// dostaje wartości dla nowej roli i oba urządzenia widzą się od razu, bez
// ręcznej konfiguracji. Pozostałe ustawienia (Wi-Fi, MQTT, kalibracja, piny)
// zostają nietknięte.
void ConfigManager::applyBuildRole() {
  const int want = STACJA_ROLE_MASTER ? ROLE_BUILD_MASTER : ROLE_BUILD_NODE;
  lock();
  const int have = doc_["role_build"] | 0;
  if (have == want) {
    unlock();
    return;
  }

  JsonObject o = rootObj(doc_);   // uwaga: doc_.to<JsonObject>() skasowalby caly dokument
  o["rs485_enabled"] = true;
  o["rs485_master"]  = (STACJA_ROLE_MASTER != 0);
  o["rs485_addr"]    = STACJA_ROLE_MASTER ? 1 : STACJA_NODE_ADDR;
  o["rs485_peer"]    = STACJA_ROLE_MASTER ? 0 : STACJA_NODE_ADDR;   // master: adres węzła wykrywany sam
  o["role_build"]    = want;
  unlock();
  save();

  LOG_I("Ustawienia: firmware %s - magistrala RS485 jako %s (adres %u, drugi ESP: %s)",
        FW_ROLE_NAME, STACJA_ROLE_MASTER ? "master" : "wezel",
        (unsigned)(STACJA_ROLE_MASTER ? 1 : STACJA_NODE_ADDR),
        STACJA_ROLE_MASTER ? "wykrywany automatycznie" : STACJA_NODE_ADDR_STR);
}

bool ConfigManager::save() {
  lock();
  String s;
  serializeJson(doc_, s);
  // Ostatnia linia obrony: niekompletny dokument (np. przypadkowo wyczyszczony
  // przez blad w kodzie) nie trafia do NVS, bo skasowalby cale ustawienia na
  // stale. ensureDefaults() zawsze ustawia ap_ssid, a pelny zestaw ustawien ma
  // kilkadziesiat kluczy - dokument bez nich jest podejrzany.
  const size_t keys = doc_.size();
  const char* ap = doc_["ap_ssid"] | "";
  const bool sane = keys >= 20 && ap && *ap;
  unlock();

  if (!sane) {
    LOG_E("Ustawienia: dokument niekompletny (%u kluczy, %u B) - zapisu do NVS NIE wykonuje",
          (unsigned)keys, (unsigned)s.length());
    return false;
  }

  bool okNvs = nvsStoreWrite(NVS_NS, NVS_KEY_CFG, s);

  // Kopia zapasowa - awaria kopii nie przerywa zapisu ustawień.
  File f = LittleFS.open(CONFIG_FILE, "w");
  bool okFs = false;
  if (f) {
    okFs = f.print(s) > 0;
    f.close();
  }
  if (!okFs) LOG_W("Ustawienia: kopia /config.json na LittleFS nieudana");

  return okNvs || okFs;
}

void ConfigManager::factoryReset() {
  nvsStoreErase(NVS_NS, NVS_KEY_CFG);
  nvsStoreErase(NVS_NS, NVS_KEY_STA);
  LittleFS.remove(CONFIG_FILE);
  LittleFS.remove(STATE_FILE);
  LOG_W("Ustawienia: przywrócono wartości fabryczne (NVS + LittleFS)");
}

String ConfigManager::toJsonString() {
  lock();
  String s;
  serializeJson(doc_, s);
  unlock();
  return s;
}

bool ConfigManager::applyJson(const char* json, size_t len) {
  JsonDocument nd;
  DeserializationError err = deserializeJson(nd, json, len);
  if (err) return false;

  lock();
  JsonObject no = nd.as<JsonObject>();
  for (JsonPair kv : no) doc_[kv.key()] = kv.value();
  unlock();
  return save();
}

bool ConfigManager::applyChannelOverrides(const char* json, size_t len) {
  JsonDocument nd;
  DeserializationError err = deserializeJson(nd, json, len);
  if (err) return false;

  lock();
  JsonObject ch = nd["channels"].as<JsonObject>();
  if (ch) {
    JsonObject dst = objIn(doc_["channels"]);
    for (JsonPair kv : ch) dst[kv.key()] = kv.value();
  }
  // Wartości kalibracji dodatkowych czujników analogowych (gleba, pyranometr)
  // oraz deklinacja magnetyczna dla kompasu.
  JsonObject extra = nd["extra"].as<JsonObject>();
  if (extra) {
    JsonObject dst = objIn(doc_["extra"]);
    for (JsonPair kv : extra) dst[kv.key()] = kv.value();
  }
  unlock();
  return save();
}

String ConfigManager::getString(const char* key) {
  lock();
  String v = doc_[key].as<String>();
  unlock();
  return v;
}

String ConfigManager::wifiSsid()   { return getString("wifi_ssid"); }
String ConfigManager::wifiPass()   { return getString("wifi_pass"); }
String ConfigManager::apSsid()     { return getString("ap_ssid"); }
String ConfigManager::apPass()     { return getString("ap_pass"); }
String ConfigManager::deviceName() { return getString("device_name"); }
String ConfigManager::location()   { return getString("location"); }
String ConfigManager::mqttHost()   { return getString("mqtt_host"); }
String ConfigManager::mqttUser()   { return getString("mqtt_user"); }
String ConfigManager::mqttPass()   { return getString("mqtt_pass"); }
String ConfigManager::mqttPrefix() { return getString("mqtt_prefix"); }
String ConfigManager::ntpServer()  { return getString("ntp_server"); }
String ConfigManager::tzString()   { return getString("tz_string"); }
String ConfigManager::companyName(){ return getString("company_name"); }
String ConfigManager::companyUrl() { return getString("company_url"); }
String ConfigManager::wifiIp()    { return getString("wifi_ip"); }
String ConfigManager::wifiGw()    { return getString("wifi_gw"); }
String ConfigManager::wifiMask()  { return getString("wifi_mask"); }
String ConfigManager::wifiDns()   { return getString("wifi_dns"); }
String ConfigManager::apIp()      { return getString("ap_ip"); }
String ConfigManager::hostname()  { return getString("hostname"); }
String ConfigManager::otaPassword(){ return getString("ota_password"); }

bool ConfigManager::wifiStatic() {
  lock();
  bool v = doc_["wifi_static"] | false;
  unlock();
  return v;
}

bool ConfigManager::apHidden() {
  lock();
  bool v = doc_["ap_hidden"] | false;
  unlock();
  return v;
}

bool ConfigManager::apAlwaysOn() {
  lock();
  bool v = doc_["ap_always_on"] | true;
  unlock();
  return v;
}

uint16_t ConfigManager::mqttPort() {
  lock();
  uint16_t v = doc_["mqtt_port"] | 1883;
  unlock();
  return v;
}

uint32_t ConfigManager::logIntervalS() {
  lock();
  uint32_t v = doc_["log_interval_s"] | 60;
  unlock();
  return v;
}

uint16_t ConfigManager::sdRetentionDays() {
  lock();
  uint16_t v = doc_["sd_retention_days"] | 0;
  unlock();
  return v;
}

uint16_t ConfigManager::sdRetentionPhotosDays() {
  lock();
  uint16_t v = doc_["sd_retention_photos_days"] | 0;
  unlock();
  return v;
}

// --- Pierścień LED RGB ---
bool ConfigManager::rgbEnabled() {
  lock();
  bool v = doc_["rgb_enabled"] | true;
  unlock();
  return v;
}

uint8_t ConfigManager::rgbBrightness() {
  lock();
  int v = doc_["rgb_brightness"] | 60;
  unlock();
  if (v < 0)   v = 0;
  if (v > 100) v = 100;
  return (uint8_t)v;
}

bool ConfigManager::rgbEffects() {
  lock();
  bool v = doc_["rgb_effects"] | true;
  unlock();
  return v;
}

bool ConfigManager::rgbOffAtNight() {
  lock();
  bool v = doc_["rgb_off_at_night"] | false;
  unlock();
  return v;
}

uint16_t ConfigManager::rgbCount() {
  lock();
  int v = doc_["rgb_count"] | 36;
  unlock();
  if (v < 1)   v = 1;
  if (v > 150) v = 150;
  return (uint16_t)v;
}

uint8_t ConfigManager::rgbOrder() {
  lock();
  int v = doc_["rgb_order"] | 0;
  unlock();
  if (v < 0 || v > 2) v = 0;
  return (uint8_t)v;
}

uint16_t ConfigManager::rgbMaxMa() {
  lock();
  int v = doc_["rgb_max_ma"] | 900;
  unlock();
  if (v < 100)  v = 100;
  if (v > 5000) v = 5000;
  return (uint16_t)v;
}

// Kolor stanu pogody. Gdy w NVS nie ma jeszcze tablicy kolorów (albo wpis jest
// uszkodzony), wracamy do wartości domyślnej z WEATHER_COLOR_DEFAULTS.
uint32_t ConfigManager::rgbColor(uint8_t state) {
  if (state >= WEATHER_STATE_COUNT) state = 0;
  lock();
  JsonArray cols = doc_["rgb_colors"].as<JsonArray>();
  uint32_t v = WEATHER_COLOR_DEFAULTS[state];
  if (cols && state < cols.size() && cols[state].is<uint32_t>()) {
    v = cols[state].as<uint32_t>() & 0xFFFFFFu;
  }
  unlock();
  return v;
}

void ConfigManager::setRgbColor(uint8_t state, uint32_t rgb) {
  if (state >= WEATHER_STATE_COUNT) return;
  lock();
  JsonArray cols = arrIn(doc_["rgb_colors"]);
  // Braki uzupelniamy kolorami domyslnymi - ustawienie jednego koloru nie moze
  // kasowac pozostalych stanow pogody.
  while (cols.size() < WEATHER_STATE_COUNT) cols.add((uint32_t)WEATHER_COLOR_DEFAULTS[cols.size()]);
  cols[state] = rgb & 0xFFFFFFu;
  unlock();
}

void ConfigManager::resetRgbColors() {
  lock();
  JsonArray cols = doc_["rgb_colors"].to<JsonArray>();
  cols.clear();
  for (size_t i = 0; i < WEATHER_STATE_COUNT; i++) cols.add((uint32_t)WEATHER_COLOR_DEFAULTS[i]);
  unlock();
}

// Porządkowanie własnej nazwy stanu: bez znaków sterujących, bez spacji na
// końcach i nie dłuższa niż RGB_NAME_MAX znaków (licząc znaki, nie bajty, żeby
// nie uciąć polskiej litery w połowie).
static void rgbNameSanitize(String& v) {
  String out;
  out.reserve(v.length());
  for (size_t i = 0; i < v.length(); i++) {
    char c = v[i];
    if ((uint8_t)c < 0x20 || (uint8_t)c == 0x7F) c = ' ';
    out += c;
  }
  out.trim();

  size_t chars = 0;
  for (size_t i = 0; i < out.length(); i++) {
    if (((uint8_t)out[i] & 0xC0) == 0x80) continue;   // kontynuacja znaku UTF-8
    if (chars >= RGB_NAME_MAX) {
      out = out.substring(0, i);
      break;
    }
    chars++;
  }
  v = out;
}

// Nazwa stanu pogody. Pusty tekst w ustawieniach oznacza nazwę domyślną
// z WEATHER_STATE_NAMES - dzięki temu nadal da się je tłumaczyć na stronie
// (słownik zna teksty domyślne), a własna nazwa jest pokazywana dosłownie.
String ConfigManager::rgbName(uint8_t state) {
  if (state >= WEATHER_STATE_COUNT) state = 0;
  String v;
  lock();
  JsonArray names = doc_["rgb_names"].as<JsonArray>();
  if (names && state < names.size()) v = names[state].as<const char*>();
  unlock();

  rgbNameSanitize(v);
  if (v.length() == 0) return String(WEATHER_STATE_NAMES[state]);
  return v;
}

bool ConfigManager::rgbNameCustom(uint8_t state) {
  return rgbName(state) != String(WEATHER_STATE_NAMES[state < WEATHER_STATE_COUNT ? state : 0]);
}

void ConfigManager::setRgbName(uint8_t state, const String& name) {
  if (state >= WEATHER_STATE_COUNT) return;
  String v = name;
  rgbNameSanitize(v);
  lock();
  JsonArray names = arrIn(doc_["rgb_names"]);
  while (names.size() < WEATHER_STATE_COUNT) names.add("");   // puste = nazwa domyslna
  names[state] = v;
  unlock();
}

void ConfigManager::resetRgbNames() {
  lock();
  JsonArray names = doc_["rgb_names"].to<JsonArray>();
  names.clear();
  for (size_t i = 0; i < WEATHER_STATE_COUNT; i++) names.add("");
  unlock();
}

ChannelConfig ConfigManager::channelCfg(const String& id) {
  ChannelConfig c;
  // Cztery wejścia ADS1115 domyślnie nie są włączone - zwykle wykorzystuje
  // się jedno, więc reszta nie zaśmiecałaby pulpitu. Włącza się je w zakładce
  // Kalibracja.
  if (id.startsWith("ads_")) c.enabled = false;
  lock();
  JsonObject ch = doc_["channels"].as<JsonObject>();
  JsonVariant v = ch[id];
  if (v.is<JsonObject>()) {
    JsonObject o = v.as<JsonObject>();
    if (o["enabled"].is<bool>()) c.enabled = o["enabled"].as<bool>();
    if (o["offset"].is<float>()) c.offset = o["offset"].as<float>();
    if (o["name"].is<const char*>()) c.name = o["name"].as<const char*>();
    if (o["zone"].is<const char*>()) c.zone = o["zone"].as<const char*>();
  }
  unlock();
  return c;
}

// --- Piny: nadpisania wartości domyślnych z include/pins.h ---
int ConfigManager::pinOverride(const char* key, int def) {
  lock();
  JsonObject pins = doc_["pins"].as<JsonObject>();
  JsonVariant v = pins[key];
  int out = v.is<int>() ? v.as<int>() : def;
  unlock();
  return out;
}

void ConfigManager::setPinOverride(const char* key, int gpio) {
  lock();
  JsonObject pins = objIn(doc_["pins"]);
  pins[key] = gpio;
  unlock();
}

void ConfigManager::removePinOverride(const char* key) {
  lock();
  JsonObject pins = doc_["pins"].as<JsonObject>();
  if (pins) pins.remove(key);
  unlock();
}

void ConfigManager::setChannelCfg(const String& id, const ChannelConfig& c) {
  lock();
  JsonObject ch = objIn(doc_["channels"]);
  JsonObject o = objIn(ch[id]);
  o["enabled"] = c.enabled;
  o["offset"] = c.offset;
  // Pusty tekst usuwa wpis (tak jak dotychczas), ale pozostale kanaly zostaja.
  if (c.name.length()) o["name"] = c.name; else o.remove("name");
  if (c.zone.length()) o["zone"] = c.zone; else o.remove("zone");
  unlock();
}

// --- Czujnik jakości powietrza wewnętrznego ---
uint8_t ConfigManager::aqType() {
  lock();
  uint8_t v = doc_["aq_type"] | (uint8_t)0;
  unlock();
  return v;
}

// --- Kamera (opcjonalna) ---
bool ConfigManager::camEnabled() {
  lock();
  bool v = doc_["cam_enabled"] | false;
  unlock();
  return v;
}

uint16_t ConfigManager::camIntervalMin() {
  lock();
  int v = doc_["cam_interval_min"] | 15;
  unlock();
  if (v < 0)  v = 0;
  if (v > 1440) v = 1440;   // maks. 1 doba
  return (uint16_t)v;
}

uint8_t ConfigManager::camResolution() {
  lock();
  int v = doc_["cam_resolution"] | 8;
  unlock();
  if (v < 0)  v = 0;
  if (v > 13) v = 13;       // 0..13 (96x96 .. UXGA 1600x1200)
  return (uint8_t)v;
}

uint8_t ConfigManager::camQuality() {
  lock();
  int v = doc_["cam_quality"] | 12;
  unlock();
  if (v < 0)  v = 0;
  if (v > 63) v = 63;
  return (uint8_t)v;
}

uint8_t ConfigManager::camModel() {
  lock();
  int v = doc_["cam_model"] | 0;
  unlock();
  if (v < 0) v = 0;
  if (v > 4) v = 4;
  return (uint8_t)v;
}

uint16_t ConfigManager::camMaxPhotos() {
  lock();
  int v = doc_["cam_max_photos"] | 0;
  unlock();
  if (v < 0)      v = 0;
  if (v > 50000)  v = 50000;
  return (uint16_t)v;
}

uint16_t ConfigManager::camMaxMb() {
  lock();
  int v = doc_["cam_max_mb"] | 0;
  unlock();
  if (v < 0)      v = 0;
  if (v > 65535)  v = 65535;
  return (uint16_t)v;
}

String ConfigManager::camRemoteUrl() {
  return getString("cam_remote_url");
}

// --- Położenie geograficzne i GPS ---
double ConfigManager::latitude() {
  lock();
  double v = doc_["latitude"] | 0.0;
  unlock();
  return v;
}

double ConfigManager::longitude() {
  lock();
  double v = doc_["longitude"] | 0.0;
  unlock();
  return v;
}

// Prędkość UART odbiornika GPS; dozwolone tylko typowe wartości, żeby
// literówka w ustawieniach nie zablokowała portu.
uint32_t ConfigManager::gpsBaud() {
  lock();
  uint32_t v = doc_["gps_baud"] | (uint32_t)9600;
  unlock();
  if (v != 4800 && v != 9600 && v != 19200 && v != 38400 && v != 57600 && v != 115200) v = 9600;
  return v;
}

bool ConfigManager::gpsTimeSync() {
  lock();
  bool v = doc_["gps_time_sync"] | true;
  unlock();
  return v;
}

bool ConfigManager::gpsEnabled() {
  lock();
  bool v = doc_["gps_enabled"] | true;
  unlock();
  return v;
}

// --- Zegar czasu rzeczywistego (RTC) ---
bool ConfigManager::rtcEnabled() {
  lock();
  bool v = doc_["rtc_enabled"] | true;
  unlock();
  return v;
}

bool ConfigManager::rtcNtpSync() {
  lock();
  bool v = doc_["rtc_ntp_sync"] | true;
  unlock();
  return v;
}

uint8_t ConfigManager::rtcBus() {
  lock();
  uint8_t v = doc_["rtc_bus"] | (uint8_t)0;
  unlock();
  if (v > 1) v = 0;
  return v;
}

// Piny magistrali osobnej (I2C2/Wire1) - używane tylko przy rtc_bus = 1.
// Sprawdzenie zakresu chroni przed literówką, która zablokowałaby magistralę.
int ConfigManager::rtcSda() {
  lock();
  int v = doc_["rtc_sda"] | 47;
  unlock();
  if (v < 0 || v > 48) v = 47;
  return v;
}

int ConfigManager::rtcScl() {
  lock();
  int v = doc_["rtc_scl"] | 48;
  unlock();
  if (v < 0 || v > 48) v = 48;
  return v;
}

uint8_t ConfigManager::rtcType() {
  lock();
  uint8_t v = doc_["rtc_type"] | (uint8_t)0;
  unlock();
  if (v > 3) v = 0;
  return v;
}

// --- Magistrala RS485: drugi ESP (węzeł z zewnętrznymi czujnikami) ---
bool ConfigManager::rs485Enabled() {
  lock();
  bool v = doc_["rs485_enabled"] | false;
  unlock();
  return v;
}

// true = ta stacja odpytywuje węzeł (master), false = ta stacja jest węzłem.
bool ConfigManager::rs485Master() {
  lock();
  bool v = doc_["rs485_master"] | true;
  unlock();
  return v;
}

uint8_t ConfigManager::rs485Addr() {
  lock();
  int v = doc_["rs485_addr"] | 1;
  unlock();
  if (v < 1 || v > 247) v = 1;
  return (uint8_t)v;
}

uint8_t ConfigManager::rs485Peer() {
  lock();
  // 0 = master sam wykrywa adres drugiego ESP (skan adresów 1..32)
  int v = doc_["rs485_peer"] | 0;
  unlock();
  if (v < 0 || v > 247) v = 0;
  return (uint8_t)v;
}

// Lista adresów węzłów RS485 (np. "2,3,4"). Pusta = master wykrywa węzły sam.
String ConfigManager::rs485Nodes() {
  lock();
  String v = doc_["rs485_nodes"] | "";
  unlock();
  return v;
}

// Dozwolone prędkości: typowe dla RS485 (moduły USB-RS485 i konwertery).
uint32_t ConfigManager::rs485Baud() {
  lock();
  uint32_t v = doc_["rs485_baud"] | (uint32_t)19200;
  unlock();
  if (v != 9600 && v != 19200 && v != 38400 && v != 57600 && v != 115200) v = 19200;
  return v;
}

uint16_t ConfigManager::rs485PollS() {
  lock();
  uint16_t v = doc_["rs485_poll_s"] | (uint16_t)30;
  unlock();
  if (v < 2) v = 2;      // 2 s to najkrótszy sensowny odstęp (ramka ~2 kB)
  if (v > 3600) v = 3600;
  return v;
}

uint16_t ConfigManager::rs485TimeoutMs() {
  lock();
  uint16_t v = doc_["rs485_timeout_ms"] | (uint16_t)400;
  unlock();
  if (v < 50) v = 50;
  if (v > 5000) v = 5000;
  return v;
}

uint8_t ConfigManager::rs485Retries() {
  lock();
  uint8_t v = doc_["rs485_retries"] | (uint8_t)2;
  unlock();
  if (v > 5) v = 5;
  return v;
}

bool ConfigManager::rs485Log() {
  lock();
  bool v = doc_["rs485_log"] | true;
  unlock();
  return v;
}

bool ConfigManager::rs485Mqtt() {
  lock();
  bool v = doc_["rs485_mqtt"] | true;
  unlock();
  return v;
}

bool ConfigManager::rs485SlaveAp() {
  lock();
  bool v = doc_["rs485_slave_ap"] | true;
  unlock();
  return v;
}

bool ConfigManager::rs485BaudManual() {
  lock();
  bool v = doc_["rs485_baud_manual"] | false;
  unlock();
  return v;
}

// --- Kalibracja czujników analogowych (sekcja "extra") ---
float ConfigManager::extraF(const char* key, float def) {
  if (!key || !*key) return def;
  lock();
  JsonVariant v = doc_["extra"][key];
  float out = v.is<float>() ? v.as<float>() : def;
  unlock();
  return out;
}

void ConfigManager::setExtraF(const char* key, float value) {
  if (!key || !*key) return;
  lock();
  JsonObject ex = objIn(doc_["extra"]);
  ex[key] = value;
  unlock();
}

void ConfigManager::removeExtra(const char* key) {
  if (!key || !*key) return;
  lock();
  if (doc_["extra"].is<JsonObject>()) doc_["extra"].remove(key);
  unlock();
}

bool ConfigManager::hasExtra(const char* key) {
  if (!key || !*key) return false;
  lock();
  bool out = !doc_["extra"][key].isNull();
  unlock();
  return out;
}

// --- Serwisy zewnętrzne ---

// Scala podane pola z sekcją "services" (nie kasuje pozostałych), dzięki czemu
// strona może zapisać samo jedno pole formularza bez wysyłania całości.
static void mergeServiceKeys(JsonObject dst, JsonObjectConst src) {
  for (JsonPairConst kv : src) {
    if (kv.value().is<JsonObjectConst>()) {
      JsonObject sub = dst[kv.key()].is<JsonObject>() ? dst[kv.key()].as<JsonObject>()
                                                      : dst[kv.key()].to<JsonObject>();
      mergeServiceKeys(sub, kv.value().as<JsonObjectConst>());
    } else {
      dst[kv.key()].set(kv.value());
    }
  }
}

String ConfigManager::servicesJson() {
  lock();
  String s;
  serializeJson(doc_["services"], s);
  unlock();
  return s;
}

bool ConfigManager::applyServicesJson(const char* json, size_t len) {
  JsonDocument nd;
  if (deserializeJson(nd, json, len)) return false;
  JsonObjectConst src = nd.as<JsonObjectConst>();
  if (!src) return false;

  lock();
  JsonObject dst = objIn(doc_["services"]);
  mergeServiceKeys(dst, src);
  unlock();
  return save();
}

uint16_t ConfigManager::svcInterval() {
  lock();
  uint16_t v = doc_["services"]["interval"] | (uint16_t)300;
  unlock();
  if (v < 30) v = 30;           // sensowne minimum - nie zalewamy serwisów
  return v;
}

bool ConfigManager::svcExternalOnly() {
  lock();
  bool v = doc_["services"]["external_only"] | false;
  unlock();
  return v;
}

bool ConfigManager::svcSectionEnabled() {
  lock();
  bool v = doc_["services"]["enabled"] | false;
  unlock();
  return v;
}

bool ConfigManager::svcEnabled(const char* svc) {
  lock();
  bool v = doc_["services"][svc]["enabled"] | false;
  unlock();
  return v;
}

String ConfigManager::svcStr(const char* svc, const char* key) {
  lock();
  String v = doc_["services"][svc][key] | "";
  unlock();
  return v;
}

String ConfigManager::svcTop(const char* key) {
  lock();
  String v = doc_["services"][key] | "";
  unlock();
  return v;
}

bool ConfigManager::loadState() {
  String s = nvsStoreRead(NVS_NS, NVS_KEY_STA);
  if (s.length() == 0) {
    File f = LittleFS.open(STATE_FILE, "r");
    if (!f) return false;
    while (f.available()) s += (char)f.read();
    f.close();
    if (s.length() == 0) return false;
  }

  JsonDocument d;
  if (deserializeJson(d, s)) return false;
  state.rainTipsTotal = d["rainTipsTotal"] | 0;
  state.rainStartTips = d["rainStartTips"] | 0;
  state.rainDay       = d["rainDay"] | -1;
  return true;
}

bool ConfigManager::saveState() {
  JsonDocument d;
  d["rainTipsTotal"] = state.rainTipsTotal;
  d["rainStartTips"] = state.rainStartTips;
  d["rainDay"]       = state.rainDay;
  String s;
  serializeJson(d, s);

  // Licznik deszczu zmienia się przy każdym wychyleniu czujnika, dlatego
  // trafia tylko do NVS (mniejsze zużycie flasha niż zapis pliku).
  bool ok = nvsStoreWrite(NVS_NS, NVS_KEY_STA, s);
  if (!ok) LOG_W("NVS: nie udało się zapisać licznika deszczu");
  return ok;
}
