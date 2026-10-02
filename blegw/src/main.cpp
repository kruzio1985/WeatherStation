/* =============================================================================
 * Stacja Pogody - bramka BLE / węzeł ESP32-C3 (pełny panel www)
 * Copyright (c) 2026 kruzio1985 - https://github.com/kruzio1985
 * Licencja: PolyForm Noncommercial License 1.0.0 (plik LICENSE)
 * Użytek niekomercyjny. Kontakt: kruzio1985@users.noreply.github.com
 * =============================================================================
 * 1) Multi-dekoder BLE: Xiaomi/ATC/Govee/BTHome/Inkbird/ThermoPro/Moat/Jaalee
 *    + wysyłanie komend GATT do urządzeń (które nie są zablokowane).
 * 2) Panel www z menu po lewej: Pulpit, BLE, Piny, LED, Ustawienia, Logi.
 * 3) Wi-Fi: STA ze statycznym IP (domyślnie 192.168.1.148) + AP "StacjaBLE"
 *    z hasłem 12345678 (gdy brak sieci).
 * 4) Sterowniki: NeoPixel (pasek LED), DHT11/22, DS18B20 (1-Wire), BME280 (I2C).
 * 5) Log błędów (ring buffer) + powód resetu + watchdog (10 s).
 * ========================================================================== */
#include <Arduino.h>
#include <NimBLEDevice.h>
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <Adafruit_NeoPixel.h>
#include <Adafruit_BME280.h>
#include <DHT.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <esp_task_wdt.h>
#include <esp_system.h>
#include <ArduinoOTA.h>
#include <Update.h>
#include <math.h>
#include <stdarg.h>
#include "ble_decode.h"

// ============================ KONFIGURACJA (NVS) ============================
Preferences P;
String C_ssid, C_pass, C_url, C_apPass;
String C_ip, C_gw, C_mask;
bool   C_static = true;
String C_names, C_transport;
String C_name;   // nazwa urządzenia (pusta = generyczna z MAC)
String C_keys;   // "MAC=32znakowyKluczHex" - bind key Xiaomi (MiBeacon v5)
int    C_ledPin = -1, C_ledCount = 8;
int    C_ledBright = 50; uint32_t C_ledColor = 0xFFFFFF; int C_ledFx = 0;
int    C_dhtPin = -1, C_dhtType = 22;
int    C_owPin = -1;
int    C_sda = -1, C_scl = -1;
int    C_outPin = -1; bool C_outState = false;

void loadCfg() {
  P.begin("stacja-ble", false);
  C_ssid   = P.getString("ssid", "");
  C_pass   = P.getString("pass", "");
  C_url    = P.getString("url", "http://192.168.1.143/api/remote/ble");
  C_apPass = P.getString("ap", "12345678");
  C_ip     = P.getString("ip", "192.168.1.148");
  C_gw     = P.getString("gw", "192.168.1.1");
  C_mask   = P.getString("mask", "255.255.255.0");
  C_static = P.getBool("static", true);
  C_names  = P.getString("names", "");
  C_name   = P.getString("name", "");
  C_keys   = P.getString("keys", "");
  C_transport = P.getString("transport", "wifi");
  C_ledPin = P.getInt("ledpin", -1); C_ledCount = P.getInt("ledcnt", 8);
  C_ledBright = P.getInt("ledbri", 50); C_ledColor = P.getUInt("ledcol", 0xFFFFFF); C_ledFx = P.getInt("ledfx", 0);
  C_dhtPin = P.getInt("dhtpin", -1); C_dhtType = P.getInt("dhttype", 22);
  C_owPin  = P.getInt("owpin", -1);
  C_sda    = P.getInt("sda", -1); C_scl = P.getInt("scl", -1);
  C_outPin = P.getInt("outpin", -1);
  P.end();
  if (C_transport != "wifi" && C_transport != "rs485") C_transport = "wifi";
  if (C_ledCount < 1) C_ledCount = 1; if (C_ledCount > 300) C_ledCount = 300;
}

void saveCfg() {
  P.begin("stacja-ble", false);
  P.putString("ssid", C_ssid); P.putString("pass", C_pass); P.putString("url", C_url);
  P.putString("ap", C_apPass); P.putString("ip", C_ip); P.putString("gw", C_gw); P.putString("mask", C_mask);
  P.putBool("static", C_static); P.putString("names", C_names); P.putString("transport", C_transport);
  P.putString("name", C_name); P.putString("keys", C_keys);
  P.putInt("ledpin", C_ledPin); P.putInt("ledcnt", C_ledCount);
  P.putInt("ledbri", C_ledBright); P.putUInt("ledcol", C_ledColor); P.putInt("ledfx", C_ledFx);
  P.putInt("dhtpin", C_dhtPin); P.putInt("dhttype", C_dhtType);
  P.putInt("owpin", C_owPin); P.putInt("sda", C_sda); P.putInt("scl", C_scl);
  P.putInt("outpin", C_outPin);
  P.end();
}

// ============================ LOG BŁĘDÓW ============================
#define LOG_N 64
static char g_log[LOG_N][160];
static int g_logN = 0;
static void logE(const char* fmt, ...) {
  va_list ap; va_start(ap, fmt);
  char tmp[160]; vsnprintf(tmp, sizeof(tmp), fmt, ap); va_end(ap);
  snprintf(g_log[g_logN % LOG_N], 160, "[%lus] %s", (unsigned long)(millis() / 1000), tmp);
  g_logN++;
  Serial.println(g_log[(g_logN - 1) % LOG_N]);
}

// ============================ BLE ============================
// Generyczna, unikalna nazwa urządzenia: własna z ustawień, a domyślnie
// "blegw-XXXXXX" (6 ostatnich cyfr MAC) - nie koliduje z innymi modułami.
static String devName() {
  if (C_name.length()) return C_name;
  String m = WiFi.macAddress();
  String suf = m.substring(m.length() - 8);   // ":DD:EE:FF"
  suf.replace(":", "");
  return "blegw-" + suf;
}

#define MAX_SENSORS 16
struct Sensor {
  char mac[13], name[24], type[24];
  float temp = NAN, hum = NAN;
  int batt = -1, rssi = 0;
  unsigned long lastMs = 0;
  char raw[96];
};
Sensor g_sensors[MAX_SENSORS];
int g_sensorCount = 0;

static void macStr(const uint8_t* m, char* out) {
  snprintf(out, 13, "%02x%02x%02x%02x%02x%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
}
static void hexStr(const uint8_t* d, size_t n, char* out, size_t cap) {
  size_t o = 0;
  for (size_t i = 0; i < n && o + 3 < cap; i++) { snprintf(out + o, 3, "%02x", d[i]); o += 2; }
  out[o] = 0;
}
static const char* sensorName(const char* mac) {
  static char n[24];
  String all = C_names; int pos = 0;
  while (pos >= 0) {
    int next = all.indexOf(',', pos);
    String item = (next < 0) ? all.substring(pos) : all.substring(pos, next);
    item.trim();
    int eq = item.indexOf('=');
    if (eq > 0 && item.substring(0, eq).equalsIgnoreCase(mac)) {
      strncpy(n, item.substring(eq + 1).c_str(), sizeof(n) - 1); n[sizeof(n) - 1] = 0;
      return n;
    }
    if (next < 0) break;
    pos = next + 1;
  }
  return nullptr;
}
static Sensor* findSensor(const char* mac) {
  for (int i = 0; i < g_sensorCount; i++) if (strcmp(g_sensors[i].mac, mac) == 0) return &g_sensors[i];
  return nullptr;
}

// Bind key Xiaomi (MiBeacon v5) z konfiguracji "MAC=32znakowyKluczHex".
static bool sensorKey(const char* mac, uint8_t key[16]) {
  String all = C_keys; int pos = 0;
  while (pos >= 0) {
    int next = all.indexOf(',', pos);
    String item = (next < 0) ? all.substring(pos) : all.substring(pos, next);
    item.trim();
    int eq = item.indexOf('=');
    if (eq > 0 && item.substring(0, eq).equalsIgnoreCase(mac)) {
      String hex = item.substring(eq + 1); hex.trim();
      if (hex.length() >= 32) {
        for (int i = 0; i < 16; i++)
          key[i] = (uint8_t)strtol(hex.substring(i * 2, i * 2 + 2).c_str(), nullptr, 16);
        return true;
      }
    }
    if (next < 0) break;
    pos = next + 1;
  }
  return false;
}

// Rozpoznanie typu po nazwie rozgłoszeniowej (dla nowych modeli bez dekodera).
static const char* typeByName(const char* name) {
  if (!name || !name[0]) return nullptr;
  if (strncmp(name, "Govee", 5) == 0 || strncmp(name, "GVH", 3) == 0) return "Govee";
  if (strncmp(name, "LYWSD", 5) == 0) return "Xiaomi";
  if (strncmp(name, "Mi_", 3) == 0) return "Xiaomi";
  if (strncmp(name, "ibst", 4) == 0 || strncmp(name, "IBS-TH", 6) == 0) return "Inkbird";
  if (strncmp(name, "TP35", 4) == 0) return "ThermoPro";
  if (strncmp(name, "JHT", 3) == 0) return "Jaalee";
  return nullptr;
}

static void upsert(const char* mac, const char* advName, BleSample* s, bool ok, const char* raw) {
  Sensor* p = findSensor(mac);
  if (!p) {
    if (g_sensorCount >= MAX_SENSORS) return;
    p = &g_sensors[g_sensorCount++];
    memset(p, 0, sizeof(*p));
    p->temp = NAN; p->hum = NAN; p->batt = -1;   // po memset pola są zerowane
    strncpy(p->mac, mac, 12);
  }
  p->lastMs = millis();
  p->rssi = s->rssi;
  if (raw) strncpy(p->raw, raw, sizeof(p->raw) - 1);

  // nazwa: najpierw z konfiguracji (MAC=Nazwa), potem rozgłoszeniowa
  const char* nm = sensorName(mac);
  if (nm) strncpy(p->name, nm, sizeof(p->name) - 1);
  else if (advName && advName[0]) strncpy(p->name, advName, sizeof(p->name) - 1);

  if (ok) {
    strncpy(p->type, s->type, sizeof(p->type) - 1);
    p->temp = s->temp; p->hum = s->hum; p->batt = s->batt;
  } else {
    // bez dekodera: pokaż typ z nazwy (diagnostyka, ale wiadomo co to jest)
    const char* tn = typeByName(advName);
    if (tn) strncpy(p->type, tn, sizeof(p->type) - 1);
  }
}

// Parsuje struktury AD z payloadu reklamy / scan response.
static void parseAds(const uint8_t* data, size_t len,
                     const uint8_t** mfg, uint16_t* mfgLen,
                     const uint8_t** svc, uint16_t* svcLen, uint16_t* svcUuid,
                     const uint8_t** bth, uint16_t* bthLen,
                     char* localName, size_t nameCap) {
  size_t i = 0;
  while (i + 2 <= len) {
    uint8_t alen = data[i], type = data[i + 1];
    if (alen < 1 || i + 1 + alen > len) break;
    const uint8_t* val = data + i + 2; uint8_t vlen = alen - 1;
    if (type == 0xFF) { *mfg = val; *mfgLen = vlen; }
    else if (type == 0x16 && vlen >= 2) { *svcUuid = val[0] | (val[1] << 8); *svc = val + 2; *svcLen = vlen - 2; }
    else if (type == 0xD2) { *bth = val; *bthLen = vlen; }
    else if ((type == 0x09 || type == 0x08) && localName[0] == 0) {
      size_t c = vlen < nameCap - 1 ? vlen : nameCap - 1;
      memcpy(localName, val, c); localName[c] = 0;
    }
    i += alen + 1;
  }
}

class ScanCB : public NimBLEAdvertisedDeviceCallbacks {
  void onResult(NimBLEAdvertisedDevice* d) override {
    uint8_t mac[6];
    memcpy(mac, d->getAddress().getNative(), 6);
    const uint8_t* mfg = nullptr; uint16_t mfgLen = 0;
    const uint8_t* svc = nullptr; uint16_t svcLen = 0; uint16_t svcUuid = 0;
    const uint8_t* bth = nullptr; uint16_t bthLen = 0;
    char localName[32] = {0};

    parseAds(d->getPayload(), d->getPayloadLength(),
             &mfg, &mfgLen, &svc, &svcLen, &svcUuid, &bth, &bthLen,
             localName, sizeof(localName));
    // Przy aktywnym skanowaniu scan response przychodzi jako osobne wywołanie
    // callbacka (ten sam MAC) - upsert() scala dane po MAC.

    BleSample s; bleSampleInit(&s); memcpy(s.mac, mac, 6); s.rssi = d->getRSSI();
    char macs[13]; macStr(mac, macs);
    bool ok = false;
    if (svcUuid == 0xFE95 && svcLen) {
      uint8_t key[16]; bool hk = sensorKey(macs, key);
      ok = bleDecodeXiaomi(svc, svcLen, mac, hk ? key : nullptr, &s);
    }
    if (!ok && svcUuid == 0xFCD2 && svcLen) ok = bleDecodeBthome(svc, svcLen, &s);
    if (!ok && bth) ok = bleDecodeBthome(bth, bthLen, &s);
    if (!ok && mfg) {
      ok = bleDecodeAtc(mfg, mfgLen, mac, nullptr, &s)
        || bleDecodeGovee(mfg, mfgLen, &s)
        || bleDecodeMoat(mfg, mfgLen, &s)
        || bleDecodeJaalee(mfg, mfgLen, &s);
      if (!ok && localName[0])
        ok = bleDecodeInkbird(localName, mfg, mfgLen, &s)
          || bleDecodeThermopro(localName, mfg, mfgLen, &s);
    }
    char raw[96] = {0}; hexStr(d->getPayload(), d->getPayloadLength(), raw, sizeof(raw));
    upsert(macs, localName, &s, ok, raw);
  }
};

void startBle() {
  NimBLEDevice::init(devName().c_str());
  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setActiveScan(true);   // aktywny: odbieramy też scan response (tam często T/H)
  scan->setInterval(160); scan->setWindow(160);
  scan->setAdvertisedDeviceCallbacks(new ScanCB(), false);
  scan->start(0, nullptr, true);
}

// Wysłanie komendy GATT do urządzenia (dla odblokowanych urządzeń).
static String bleSendCommand(const String& mac, const String& svc, const String& chr, const String& hex) {
  NimBLEUUID su(svc.c_str()), cu(chr.c_str());
  uint8_t buf[64]; size_t n = 0;
  for (size_t i = 0; i + 1 < hex.length() && n < sizeof(buf); i += 2) {
    buf[n++] = (uint8_t)strtol(hex.substring(i, i + 2).c_str(), nullptr, 16);
  }
  if (!n) return "pusty payload";
  NimBLEClient* cl = NimBLEDevice::createClient();
  cl->setConnectionParams(12, 12, 0, 3000);
  if (!cl->connect(NimBLEAddress(mac.c_str()), true)) { NimBLEDevice::deleteClient(cl); return "brak połączenia"; }
  NimBLERemoteService* rs = cl->getService(su);
  if (!rs) { cl->disconnect(); NimBLEDevice::deleteClient(cl); return "brak usługi"; }
  NimBLERemoteCharacteristic* rc = rs->getCharacteristic(cu);
  if (!rc) { cl->disconnect(); NimBLEDevice::deleteClient(cl); return "brak charakterystyki"; }
  bool ok = rc->writeValue(buf, n, false);
  cl->disconnect(); NimBLEDevice::deleteClient(cl);
  return ok ? "OK" : "błąd zapisu";
}

// ============================ CZUJNIKI ============================
Adafruit_NeoPixel* strip = nullptr;
DHT* dht = nullptr;
OneWire* ow = nullptr;
DallasTemperature* dallas = nullptr;
Adafruit_BME280 bme;

float s_temp = NAN, s_hum = NAN, s_press = NAN;
unsigned long s_lastRead = 0;

void ledApply() {
  if (!strip || C_ledPin < 0) return;
  if (C_ledFx == 0) {
    strip->fill(strip->Color((C_ledColor >> 16) & 0xFF, (C_ledColor >> 8) & 0xFF, C_ledColor & 0xFF));
  } else if (C_ledFx == 1) {   // tęcza
    static uint16_t hue = 0;
    for (int i = 0; i < strip->numPixels(); i++)
      strip->setPixelColor(i, strip->ColorHSV((hue + i * 65536 / strip->numPixels()) & 0xFFFF));
    hue += 256;
  } else if (C_ledFx == 2) {   // oddychanie
    static unsigned long t0 = millis();
    float v = (sinf((millis() - t0) / 500.0f) + 1) / 2;
    uint8_t r = (C_ledColor >> 16) & 0xFF, g = (C_ledColor >> 8) & 0xFF, b = C_ledColor & 0xFF;
    strip->fill(strip->Color((uint8_t)(r * v), (uint8_t)(g * v), (uint8_t)(b * v)));
  }
  strip->setBrightness(C_ledBright);
  strip->show();
}

void sensorsInit() {
  if (C_ledPin >= 0) {
    strip = new Adafruit_NeoPixel(C_ledCount, C_ledPin, NEO_GRB + NEO_KHZ800);
    strip->begin(); ledApply();
  }
  if (C_dhtPin >= 0) dht = new DHT(C_dhtPin, C_dhtType);
  if (C_owPin >= 0) {
    ow = new OneWire(C_owPin);
    dallas = new DallasTemperature(ow);
    dallas->begin();
  }
  if (C_sda >= 0 && C_scl >= 0) {
    Wire.begin(C_sda, C_scl);
    if (!bme.begin(0x76, &Wire)) bme.begin(0x77, &Wire);
  }
  if (C_outPin >= 0) { pinMode(C_outPin, OUTPUT); digitalWrite(C_outPin, C_outState ? HIGH : LOW); }
}

void sensorsRead() {
  unsigned long now = millis();
  if (now - s_lastRead < 3000) return;
  s_lastRead = now;
  s_temp = s_hum = s_press = NAN;
  if (dht) {
    float h = dht->readHumidity(), t = dht->readTemperature();
    if (!isnan(h) && !isnan(t)) { s_hum = h; s_temp = t; }
  }
  if (dallas) {
    dallas->requestTemperatures();
    float t = dallas->getTempCByIndex(0);
    if (t > -100) s_temp = t;
  }
  if (C_sda >= 0 && C_scl >= 0) {
    float p = bme.readPressure() / 100.0f;
    if (!isnan(p)) { s_press = p; if (isnan(s_temp)) s_temp = bme.readTemperature(); }
  }
}

// ============================ WYŚLIKA DO MASTERA ============================
unsigned long g_lastSend = 0;

static String sensorsJson() {
  JsonDocument doc;
  JsonArray arr = doc["sensors"].to<JsonArray>();
  for (int i = 0; i < g_sensorCount; i++) {
    Sensor& s = g_sensors[i];
    if (!s.type[0] || s.raw[0]) continue;
    if (millis() - s.lastMs > 120000) continue;
    JsonObject o = arr.add<JsonObject>();
    o["mac"] = s.mac;
    if (s.name[0]) o["name"] = s.name;
    if (!isnan(s.temp)) o["temp"] = s.temp;
    if (!isnan(s.hum)) o["hum"] = s.hum;
    if (s.batt >= 0) o["batt"] = s.batt;
    o["rssi"] = s.rssi;
  }
  String body; serializeJson(doc, body); return body;
}
void sendOut() {
  if (!g_sensorCount) return;
  String body = sensorsJson();
  if (body.indexOf("\"mac\"") < 0) return;
  if (C_transport == "rs485") { Serial1.println(body); return; }
  HTTPClient h; h.setConnectTimeout(3000); h.setTimeout(4000);
  if (h.begin(C_url)) { h.addHeader("Content-Type", "application/json"); h.POST(body); h.end(); }
}

// ============================ WWW ============================
WebServer web(80);

static String esc(String s) {
  s.replace("&", "&amp;"); s.replace("<", "&lt;"); s.replace(">", "&gt;"); s.replace("\"", "&quot;");
  return s;
}

static String shell(const String& title, const String& active, const String& body) {
  String m = "<div class='menu'>"
    "<a class='" + String(active == "dash" ? "on" : "") + "' href='/'>🏠 Pulpit</a>"
    "<a class='" + String(active == "ble" ? "on" : "") + "' href='/ble'>📡 BLE</a>"
    "<a class='" + String(active == "pins" ? "on" : "") + "' href='/pins'>🔌 Piny</a>"
    "<a class='" + String(active == "led" ? "on" : "") + "' href='/led'>💡 LED</a>"
    "<a class='" + String(active == "setup" ? "on" : "") + "' href='/setup'>⚙️ Ustawienia</a>"
    "<a class='" + String(active == "ota" ? "on" : "") + "' href='/ota'>⬆️ OTA</a>"
    "<a class='" + String(active == "log" ? "on" : "") + "' href='/log'>📜 Logi</a>"
    "</div>";
  return "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>" + title + "</title><style>"
    "*{box-sizing:border-box}body{font-family:system-ui,sans-serif;background:#111;color:#eee;margin:0}"
    ".menu{position:fixed;left:0;top:0;bottom:0;width:190px;background:#16161f;padding:14px 0;overflow:auto}"
    ".menu a{display:block;color:#ccc;text-decoration:none;padding:10px 16px;border-left:3px solid transparent}"
    ".menu a.on,.menu a:hover{background:#1f1f2e;color:#0cf;border-left:3px solid #0cf}"
    ".main{margin-left:190px;padding:16px}"
    "h2{color:#0cf}table{width:100%;border-collapse:collapse;margin-top:10px}"
    "th,td{border-bottom:1px solid #333;padding:6px;text-align:left;font-size:.85em}"
    "th{color:#0cf}.ok{color:#4c4}.raw{font-family:monospace;font-size:.7em;color:#9ad0ff;word-break:break-all}"
    "input,select,textarea{width:100%;max-width:340px;padding:7px;margin:4px 0;background:#1a1a2e;color:#eee;border:1px solid #333;border-radius:4px}"
    "button{background:#0cf;color:#000;border:0;padding:8px 16px;border-radius:4px;font-weight:bold;cursor:pointer;margin:2px}"
    ".card{background:#16161f;padding:14px;border-radius:8px;margin-bottom:12px}"
    ".tile{display:inline-block;background:#16161f;border-radius:8px;padding:14px;margin:6px;min-width:120px;text-align:center}"
    ".tile .v{font-size:1.5em;color:#0cf}.tile .l{color:#888;font-size:.8em}"
    "label{display:block;margin-top:8px;color:#bbb;font-size:.85em}"
    "</style></head><body>" + m + "<div class='main'><h2>" + title + "</h2>" + body + "</div></body></html>";
}

void handleDash() {
  String b = "<div class='card'><b>Lokalne czujniki</b><br>";
  if (isnan(s_temp) && isnan(s_press)) b += "<span class='ok'>brak podłączonych czujników (przypisz piny w zakładce Piny)</span>";
  if (!isnan(s_temp)) b += "<div class='tile'><div class='v'>" + String(s_temp, 1) + "°C</div><div class='l'>Temperatura</div></div>";
  if (!isnan(s_hum)) b += "<div class='tile'><div class='v'>" + String(s_hum, 1) + "%</div><div class='l'>Wilgotność</div></div>";
  if (!isnan(s_press)) b += "<div class='tile'><div class='v'>" + String(s_press, 1) + " hPa</div><div class='l'>Ciśnienie</div></div>";
  b += "</div>";
  b += "<div class='card'><b>BLE: " + String(g_sensorCount) + " urządzeń</b><br>";
  b += "<table><tr><th>MAC</th><th>Nazwa</th><th>Typ</th><th>T</th><th>RH</th><th>Bat</th><th>RSSI</th><th>Wiek</th></tr>";
  for (int i = 0; i < g_sensorCount; i++) {
    Sensor& s = g_sensors[i];
    unsigned age = (millis() - s.lastMs) / 1000;
    b += "<tr><td class='raw'>" + String(s.mac) + "</td><td>" + esc(s.name) + "</td><td>" + esc(s.type) +
         "</td><td>" + (isnan(s.temp) ? "—" : String(s.temp, 1)) + "</td><td>" + (isnan(s.hum) ? "—" : String(s.hum, 1)) +
         "</td><td>" + (s.batt < 0 ? "—" : String(s.batt)) + "</td><td>" + String(s.rssi) + "</td><td>" + String(age) + "s</td></tr>";
    if (s.raw[0]) b += "<tr><td colspan='8' class='raw'>ramka: " + String(s.raw) + "</td></tr>";
  }
  b += "</table></div>";
  b += "<div class='card'>Wi-Fi: " + String(WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "AP " + WiFi.softAPIP().toString()) +
       " · IP konfigurowane: " + esc(C_ip) + (C_static ? " (statyczne)" : " (DHCP)") + "</div>";
  web.send(200, "text/html", shell("Pulpit", "dash", b));
}

void handleBle() {
  String b = "<div class='card'><b>Wyślij komendę GATT</b><form method='post' action='/blesend'>"
             "MAC<br><input name='mac' placeholder='a4c138123456'>"
             "<br>Service UUID<br><input name='svc' placeholder='0000180f-0000-1000-8000-00805f9b34fb'>"
             "<br>Char UUID<br><input name='chr' placeholder='00002a19-0000-1000-8000-00805f9b34fb'>"
             "<br>Dane (hex)<br><input name='hex' placeholder='01'>"
             "<br><button type='submit'>Wyślij</button></form>"
             "<div class='ok'>Uwaga: działa tylko na urządzeniach odblokowanych (bez szyfrowania/pary).</div></div>";
  b += "<div class='card'><b>Widoczne urządzenia</b><br><a href='/ble' onclick='setTimeout(()=>location.reload(),100)'>Odśwież</a>"
       "<table><tr><th>MAC</th><th>Nazwa</th><th>Typ</th><th>T</th><th>RH</th><th>Bat</th><th>RSSI</th></tr>";
  for (int i = 0; i < g_sensorCount; i++) {
    Sensor& s = g_sensors[i];
    b += "<tr><td class='raw'>" + String(s.mac) + "</td><td>" + esc(s.name) + "</td><td>" + esc(s.type) +
         "</td><td>" + (isnan(s.temp) ? "—" : String(s.temp, 1)) + "</td><td>" + (isnan(s.hum) ? "—" : String(s.hum, 1)) +
         "</td><td>" + (s.batt < 0 ? "—" : String(s.batt)) + "</td><td>" + String(s.rssi) + "</td></tr>";
    if (s.raw[0]) b += "<tr><td colspan='7' class='raw'>ramka: " + String(s.raw) + "</td></tr>";
  }
  b += "</table></div>";
  web.send(200, "text/html", shell("Bluetooth", "ble", b));
}

void handleBleSend() {
  String mac = web.arg("mac"), svc = web.arg("svc"), chr = web.arg("chr"), hex = web.arg("hex");
  String r = bleSendCommand(mac, svc, chr, hex);
  logE("BLE komenda %s -> %s", mac.c_str(), r.c_str());
  web.send(200, "text/html", shell("Bluetooth", "ble",
    "<div class='card'>Wynik: <b class='ok'>" + esc(r) + "</b><br><a href='/ble'>Wróć</a></div>"));
}

struct PinDef { int8_t gpio; const char* label; bool reserved; };
static const PinDef PINS[] = {
  {0, "GPIO0 (ADC1)", false}, {1, "GPIO1 (ADC1)", false}, {2, "GPIO2 (ADC1)", false},
  {3, "GPIO3 (ADC1)", false}, {4, "GPIO4 (ADC1)", false}, {5, "GPIO5 (ADC2)", false},
  {6, "GPIO6", false}, {7, "GPIO7", false}, {8, "GPIO8 (LED wbudowana)", false},
  {9, "GPIO9 (BOOT)", true}, {10, "GPIO10", false},
  {20, "GPIO20 (RX)", false}, {21, "GPIO21 (TX)", false}
};
#define NPINS (sizeof(PINS) / sizeof(PINS[0]))

static const char* pinFn(int8_t g) {
  if (g == C_ledPin) return "NeoPixel (LED)";
  if (g == C_dhtPin) return "DHT11/22";
  if (g == C_owPin) return "DS18B20 (1-Wire)";
  if (g == C_sda) return "I2C SDA";
  if (g == C_scl) return "I2C SCL";
  if (g == C_outPin) return "Wyjście cyfrowe";
  if (g == 20) return "RS485 RX (UART1)";
  if (g == 21) return "RS485 TX (UART1)";
  return "wolny";
}

void handlePins() {
  String b = "<div class='card'><b>Przypisz funkcje do pinów</b>"
             "<p class='ok'>Zmiana działa po zapisie i restarcie. GPIO9 (BOOT) i 20/21 (RS485) są zarezerwowane.</p>"
             "<form method='post' action='/pinsave'><table><tr><th>Pin</th><th>Funkcja</th></tr>";
  for (unsigned i = 0; i < NPINS; i++) {
    const PinDef& p = PINS[i];
    b += "<tr><td>" + String(p.label) + "</td><td>";
    if (p.reserved) b += "<span class='ok'>" + String(pinFn(p.gpio)) + " (rezerw.)</span>";
    else {
      b += "<select name='p" + String(p.gpio) + "'>"
           "<option value=''>wolny</option>"
           "<option value='led'" + (C_ledPin == p.gpio ? " selected" : "") + ">NeoPixel (LED)</option>"
           "<option value='dht'" + (C_dhtPin == p.gpio ? " selected" : "") + ">DHT11/22</option>"
           "<option value='ow'" + (C_owPin == p.gpio ? " selected" : "") + ">DS18B20 (1-Wire)</option>"
           "<option value='sda'" + (C_sda == p.gpio ? " selected" : "") + ">I2C SDA</option>"
           "<option value='scl'" + (C_scl == p.gpio ? " selected" : "") + ">I2C SCL</option>"
           "<option value='out'" + (C_outPin == p.gpio ? " selected" : "") + ">Wyjście cyfrowe</option>"
           "</select>";
    }
    b += "</td></tr>";
  }
  b += "</table><button type='submit'>Zapisz i restart</button></form></div>";
  web.send(200, "text/html", shell("Piny", "pins", b));
}

void handlePinSave() {
  C_ledPin = C_dhtPin = C_owPin = C_sda = C_scl = C_outPin = -1;
  for (unsigned i = 0; i < NPINS; i++) {
    const PinDef& p = PINS[i];
    if (p.reserved) continue;
    String v = web.arg("p" + String(p.gpio));
    if (v == "led") C_ledPin = p.gpio;
    else if (v == "dht") C_dhtPin = p.gpio;
    else if (v == "ow") C_owPin = p.gpio;
    else if (v == "sda") C_sda = p.gpio;
    else if (v == "scl") C_scl = p.gpio;
    else if (v == "out") C_outPin = p.gpio;
  }
  saveCfg();
  web.send(200, "text/html", shell("Piny", "pins", "<p class='ok'>Zapisano - restart...</p>"));
  delay(300); ESP.restart();
}

void handleLed() {
  String b = "<div class='card'><b>Pasek LED</b>";
  if (C_ledPin < 0) b += "<p>Przypisz pin NeoPixel w zakładce <a href='/pins'>Piny</a>.</p></div>";
  else {
    b += "<form method='post' action='/ledsave'>"
         "<label>Liczba diod</label><input type='number' name='cnt' value='" + String(C_ledCount) + "'>"
         "<label>Jasność (0-255)</label><input type='number' name='bri' value='" + String(C_ledBright) + "'>"
         "<label>Kolor (hex)</label><input name='col' value='#" + String(C_ledColor, HEX) + "'>"
         "<label>Efekt</label><select name='fx'>"
         "<option value='0'" + (C_ledFx == 0 ? " selected" : "") + ">stały kolor</option>"
         "<option value='1'" + (C_ledFx == 1 ? " selected" : "") + ">tęcza</option>"
         "<option value='2'" + (C_ledFx == 2 ? " selected" : "") + ">oddychanie</option>"
         "</select><br><button type='submit'>Zapisz</button></form>";
    if (C_outPin >= 0) {
      b += "<form method='post' action='/outsave'>"
           "<label>Wyjście cyfrowe GPIO" + String(C_outPin) + "</label>"
           "<select name='st'><option value='0'" + (!C_outState ? " selected" : "") + ">OFF</option>"
           "<option value='1'" + (C_outState ? " selected" : "") + ">ON</option></select>"
           "<br><button type='submit'>Ustaw</button></form>";
    }
  }
  b += "</div>";
  web.send(200, "text/html", shell("LED", "led", b));
}

void handleLedSave() {
  C_ledCount = web.arg("cnt").toInt(); C_ledBright = web.arg("bri").toInt();
  C_ledFx = web.arg("fx").toInt();
  String col = web.arg("col"); col.replace("#", "");
  if (col.length() == 6) C_ledColor = (uint32_t)strtol(col.c_str(), nullptr, 16);
  if (C_ledCount < 1) C_ledCount = 1; if (C_ledCount > 300) C_ledCount = 300;
  saveCfg();
  if (strip) { delete strip; strip = nullptr; }
  if (C_ledPin >= 0) { strip = new Adafruit_NeoPixel(C_ledCount, C_ledPin, NEO_GRB + NEO_KHZ800); strip->begin(); }
  ledApply();
  web.sendHeader("Location", "/led"); web.send(302, "text/plain", "");
}

void handleOutSave() {
  C_outState = web.arg("st") == "1";
  if (C_outPin >= 0) digitalWrite(C_outPin, C_outState ? HIGH : LOW);
  web.sendHeader("Location", "/led"); web.send(302, "text/plain", "");
}

void handleSetup() {
  String b = "<div class='card'><b>Konfiguracja</b><form method='post' action='/save'>"
             "<label>Transport</label><select name='transport'>"
             "<option value='wifi'" + String(C_transport == "wifi" ? " selected" : "") + ">Wi-Fi (HTTP do mastera)</option>"
             "<option value='rs485'" + String(C_transport == "rs485" ? " selected" : "") + ">RS485 (UART1)</option>"
             "</select>"
             "<label>Nazwa urządzenia (pusta = generyczna z MAC)</label><input name='name' value='" + esc(C_name) + "' placeholder='blegw-AABBCC'>"
             "<label>Wi-Fi SSID</label><input name='ssid' value='" + esc(C_ssid) + "'>"
             "<label>Hasło Wi-Fi</label><input name='pass' type='password' value='" + esc(C_pass) + "'>"
             "<label>Adres mastera</label><input name='url' value='" + esc(C_url) + "'>"
             "<label><input type='checkbox' name='static'" + (C_static ? " checked" : "") + "> Stały adres IP</label>"
             "<label>IP</label><input name='ip' value='" + esc(C_ip) + "'>"
             "<label>Brama</label><input name='gw' value='" + esc(C_gw) + "'>"
             "<label>Maska</label><input name='mask' value='" + esc(C_mask) + "'>"
             "<label>Hasło punktu AP</label><input name='ap' value='" + esc(C_apPass) + "'>"
             "<label>Nazwy czujników (MAC=Nazwa, MAC2=Nazwa2)</label><textarea name='names' rows='2'>" + esc(C_names) + "</textarea>"
             "<label>Klucze Xiaomi (MAC=32znakowyKlucz, MAC2=Klucz)</label><textarea name='keys' rows='2'>" + esc(C_keys) + "</textarea>"
             "<br><button type='submit'>Zapisz i restart</button></form></div>"
             "<div class='card'><b>Skan sieci Wi-Fi</b> <button onclick='scanWifi()'>Skanuj</button>"
             "<div id='wlist' class='raw'>Kliknij Skanuj, zeby zobaczyc sieci w zasiegu.</div></div>"
             "<script>function scanWifi(){document.getElementById('wlist').innerHTML='skanowanie...';"
             "fetch('/wifi/scan').then(r=>r.json()).then(j=>{"
             "if(!j.n) {document.getElementById('wlist').innerHTML='<b>Nie wykryto zadnej sieci</b>';return;}"
             "let h=''; for(const n of j.net){h+='<div style=\"margin:2px 0\"><b>'+n.ssid+'</b> · '+n.rssi+' dBm · '+n.enc+'</div>';}"
             "document.getElementById('wlist').innerHTML=h;}).catch(e=>{document.getElementById('wlist').innerHTML='błąd skanowania';});}"
             "</script>";
  web.send(200, "text/html", shell("Ustawienia", "setup", b));
}

void handleSave() {
  C_ssid = web.arg("ssid"); C_pass = web.arg("pass"); C_url = web.arg("url");
  C_ip = web.arg("ip"); C_gw = web.arg("gw"); C_mask = web.arg("mask");
  C_apPass = web.arg("ap"); C_names = web.arg("names");
  C_name = web.arg("name"); C_keys = web.arg("keys");
  C_transport = web.arg("transport");
  C_static = web.hasArg("static");
  if (C_transport != "wifi" && C_transport != "rs485") C_transport = "wifi";
  if (C_url.length() == 0) C_url = "http://192.168.1.143/api/remote/ble";
  if (C_apPass.length() == 0) C_apPass = "12345678";
  saveCfg();
  web.send(200, "text/html", shell("Ustawienia", "setup", "<p class='ok'>Zapisano - restart...</p>"));
  delay(300); ESP.restart();
}

void handleLog() {
  String b = "<div class='card'><b>Log błędów (ostatnie " + String(g_logN < LOG_N ? g_logN : LOG_N) + ")</b>"
             "<br><span class='ok'>Powód ostatniego resetu: " + String(esp_reset_reason()) + "</span>"
             "<table><tr><th>Czas</th><th>Wiadomość</th></tr>";
  int start = g_logN > LOG_N ? g_logN - LOG_N : 0;
  for (int i = start; i < g_logN; i++)
    b += "<tr><td class='raw'>" + String(g_log[i % LOG_N]) + "</td></tr>";
  b += "</table></div>";
  web.send(200, "text/html", shell("Logi", "log", b));
}

void handleOtaPage() {
  String b = "<div class='card'><b>Aktualizacja firmware (OTA)</b>"
             "<p>Wgraj plik <code>firmware.bin</code> zbudowany przez PlatformIO.</p>"
             "<form method='post' action='/update' enctype='multipart/form-data'>"
             "<input type='file' name='firmware'><br><button type='submit'>Wgraj</button></form>"
             "<p class='ok'>Możesz też wgrywać z komputera: <code>pio run -t upload --upload-port &lt;ip&gt;</code> (protokół espota).</p></div>";
  web.send(200, "text/html", shell("Aktualizacja", "ota", b));
}

void handleUpdate() {
  HTTPUpload& u = web.upload();
  if (u.status == UPLOAD_FILE_START) {
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) { logE("OTA: begin fail"); return; }
    logE("OTA: start");
  } else if (u.status == UPLOAD_FILE_WRITE) {
    if (Update.write(u.buf, u.currentSize) != u.currentSize) logE("OTA: write fail");
  } else if (u.status == UPLOAD_FILE_END) {
    if (Update.end(true)) { logE("OTA: ok - restart"); web.send(200, "text/html", shell("Aktualizacja", "ota", "<p class='ok'>OK - restart...</p>")); delay(300); ESP.restart(); }
    else { logE("OTA: end fail %s", Update.errorString()); web.send(200, "text/html", shell("Aktualizacja", "ota", "<p>Błąd: " + String(Update.errorString()) + "</p>")); }
  }
}

void handleWifiScan() {
  int n = WiFi.scanNetworks();
  JsonDocument doc;
  doc["n"] = n;
  JsonArray arr = doc["net"].to<JsonArray>();
  for (int i = 0; i < n && i < 30; i++) {
    JsonObject o = arr.add<JsonObject>();
    o["ssid"] = WiFi.SSID(i);
    o["rssi"] = WiFi.RSSI(i);
    o["enc"] = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN) ? "otwarta" : "zabezpieczona";
  }
  String out; serializeJson(doc, out);
  web.send(200, "application/json", out);
}

void handleJson() {
  JsonDocument doc;
  doc["ip"] = WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
  if (!isnan(s_temp)) doc["temp"] = s_temp;
  if (!isnan(s_hum)) doc["hum"] = s_hum;
  if (!isnan(s_press)) doc["press"] = s_press;
  JsonArray arr = doc["sensors"].to<JsonArray>();
  for (int i = 0; i < g_sensorCount; i++) {
    Sensor& s = g_sensors[i];
    JsonObject o = arr.add<JsonObject>();
    o["mac"] = s.mac;
    if (s.name[0]) o["name"] = s.name;
    if (!isnan(s.temp)) o["temp"] = s.temp;
    if (!isnan(s.hum)) o["hum"] = s.hum;
    if (s.batt >= 0) o["batt"] = s.batt;
    o["rssi"] = s.rssi; o["type"] = s.type;
  }
  String out; serializeJson(doc, out);
  web.send(200, "application/json", out);
}

// ============================ Wi-Fi ============================
void setupWifi() {
  WiFi.mode(WIFI_STA);
  if (C_transport == "rs485") { Serial1.begin(115200, SERIAL_8N1, 20, 21); Serial.println("[BLE] transport RS485"); }
  if (C_static) {
    IPAddress ip, gw, mk;
    if (ip.fromString(C_ip) && gw.fromString(C_gw) && mk.fromString(C_mask)) WiFi.config(ip, gw, mk);
  }
  if (C_ssid.length()) {
    WiFi.begin(C_ssid.c_str(), C_pass.c_str());
    for (int i = 0; i < 30 && WiFi.status() != WL_CONNECTED; i++) delay(500);
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[BLE] IP: %s\n", WiFi.localIP().toString().c_str());
    return;
  }
  WiFi.mode(WIFI_AP);
  WiFi.softAP(devName().c_str(), C_apPass.c_str());
  Serial.printf("[BLE] AP %s: %s\n", devName().c_str(), WiFi.softAPIP().toString().c_str());
}

// ============================ SETUP / LOOP ============================
void setup() {
  Serial.begin(115200);
  delay(200);
  loadCfg();
  logE("start, reset reason %d", (int)esp_reset_reason());
  setupWifi();
  startBle();
  sensorsInit();

  web.on("/", handleDash);
  web.on("/ble", handleBle);
  web.on("/blesend", handleBleSend);
  web.on("/pins", handlePins);
  web.on("/pinsave", handlePinSave);
  web.on("/led", handleLed);
  web.on("/ledsave", handleLedSave);
  web.on("/outsave", handleOutSave);
  web.on("/setup", handleSetup);
  web.on("/save", handleSave);
  web.on("/log", handleLog);
  web.on("/wifi/scan", handleWifiScan);
  web.on("/ota", handleOtaPage);
  web.on("/update", HTTP_POST, []() { web.send(200, "text/plain", "done"); }, handleUpdate);
  web.on("/json", handleJson);
  web.begin();

  // OTA (ArduinoOTA + wgranie z przeglądarki)
  ArduinoOTA.setHostname(devName().c_str());
  ArduinoOTA.onStart([]() { logE("OTA(Arduino): start"); });
  ArduinoOTA.onEnd([]() { logE("OTA(Arduino): ok - restart"); });
  ArduinoOTA.onError([](ota_error_t e) { logE("OTA(Arduino): blad %d", (int)e); });
  ArduinoOTA.begin();

  esp_task_wdt_init(10, true);   // watchdog 10 s
  esp_task_wdt_add(NULL);
  Serial.println("[BLE] gotowe");
}

void loop() {
  web.handleClient();
  ArduinoOTA.handle();
  sensorsRead();
  if (strip && C_ledFx != 0 && C_ledPin >= 0) ledApply();   // animacje
  unsigned long now = millis();
  if (now - g_lastSend >= 30000) { g_lastSend = now; sendOut(); }
  esp_task_wdt_reset();
  delay(2);
}
