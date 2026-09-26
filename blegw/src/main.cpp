/* =============================================================================
 * Stacja Pogody - bramka BLE (ESP32-C3)
 * Copyright (c) 2026 kruzio1985 - https://github.com/kruzio1985
 * Licencja: PolyForm Noncommercial License 1.0.0 (plik LICENSE)
 * Użytek niekomercyjny. Kontakt: kruzio1985@users.noreply.github.com
 * =============================================================================
 * Skanuje czujniki BLE (Xiaomi, ATC, Govee, BTHome, Inkbird, ThermoPro, Moat,
 * Jaalee), dekoduje je (include/ble_decode.h) i wysyła do mastera przez HTTP
 * POST /api/remote/ble. Nieznane czujniki (np. SETTI SS302) są pokazywane jako
 * surowa ramka hex na stronie /.
 *
 * Konfiguracja: strona /setup (Wi-Fi, adres mastera, statyczny IP, nazwy
 * czujników). Przy braku Wi-Fi uruchamia się punkt dostępowy StacjaBLE
 * (192.168.4.1) z tą samą stroną.
 * ========================================================================== */
#include <Arduino.h>
#include <NimBLEDevice.h>
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <math.h>
#include "ble_decode.h"

// --------------------------- konfiguracja ---------------------------
Preferences g_prefs;
String g_ssid, g_pass, g_url;
String g_ip, g_gw, g_mask;
String g_names;   // "MAC=Nazwa" linie
bool g_apMode = false;

void loadCfg() {
  g_prefs.begin("stacja-ble", false);
  g_ssid  = g_prefs.getString("ssid", "");
  g_pass  = g_prefs.getString("pass", "");
  g_url   = g_prefs.getString("url", "http://192.168.1.143/api/remote/ble");
  g_ip    = g_prefs.getString("ip", "");
  g_gw    = g_prefs.getString("gw", "");
  g_mask  = g_prefs.getString("mask", "");
  g_names = g_prefs.getString("names", "");
  g_prefs.end();
}

void saveCfg() {
  g_prefs.begin("stacja-ble", false);
  g_prefs.putString("ssid", g_ssid);
  g_prefs.putString("pass", g_pass);
  g_prefs.putString("url", g_url);
  g_prefs.putString("ip", g_ip);
  g_prefs.putString("gw", g_gw);
  g_prefs.putString("mask", g_mask);
  g_prefs.putString("names", g_names);
  g_prefs.end();
}

// --------------------------- skaner BLE ---------------------------
#define MAX_SENSORS 16
struct Sensor {
  char mac[13];
  char name[24];
  char type[24];
  float temp = NAN, hum = NAN;
  int batt = -1, rssi = 0;
  unsigned long lastMs = 0;
  char raw[96];       // hex ostatniej ramki (diagnostyka)
};
Sensor g_sensors[MAX_SENSORS];
int g_sensorCount = 0;

static void macStr(const uint8_t* m, char* out) {
  snprintf(out, 13, "%02x%02x%02x%02x%02x%02x",
           m[0], m[1], m[2], m[3], m[4], m[5]);
}

static void hexStr(const uint8_t* d, size_t n, char* out, size_t cap) {
  size_t o = 0;
  for (size_t i = 0; i < n && o + 3 < cap; i++) {
    snprintf(out + o, 3, "%02x", d[i]);
    o += 2;
  }
  out[o] = 0;
}

static const char* sensorName(const char* mac) {
  static char n[24];
  // "MAC=Nazwa" wiersze rozdzielone przecinkami
  String all = g_names;
  int pos = 0;
  while (pos >= 0) {
    int next = all.indexOf(',', pos);
    String item = (next < 0) ? all.substring(pos) : all.substring(pos, next);
    item.trim();
    int eq = item.indexOf('=');
    if (eq > 0 && item.substring(0, eq).equalsIgnoreCase(mac)) {
      strncpy(n, item.substring(eq + 1).c_str(), sizeof(n) - 1);
      n[sizeof(n) - 1] = 0;
      return n;
    }
    if (next < 0) break;
    pos = next + 1;
  }
  return nullptr;
}

static Sensor* findSensor(const char* mac) {
  for (int i = 0; i < g_sensorCount; i++)
    if (strcmp(g_sensors[i].mac, mac) == 0) return &g_sensors[i];
  return nullptr;
}

static void upsert(const char* mac, BleSample* s, bool ok, const char* raw) {
  Sensor* p = findSensor(mac);
  if (!p) {
    if (g_sensorCount >= MAX_SENSORS) return;
    p = &g_sensors[g_sensorCount++];
    memset(p, 0, sizeof(*p));
    strncpy(p->mac, mac, 12);
  }
  p->lastMs = millis();
  p->rssi = s->rssi;
  if (raw) strncpy(p->raw, raw, sizeof(p->raw) - 1);
  if (ok) {
    strncpy(p->type, s->type, sizeof(p->type) - 1);
    p->temp = s->temp;
    p->hum = s->hum;
    p->batt = s->batt;
    const char* nm = sensorName(mac);
    if (nm) strncpy(p->name, nm, sizeof(p->name) - 1);
  }
}

class ScanCallbacks : public NimBLEAdvertisedDeviceCallbacks {
  void onResult(NimBLEAdvertisedDevice* d) override {
    const uint8_t* data = d->getPayload();
    size_t len = d->getPayloadLength();
    uint8_t mac[6];
    memcpy(mac, d->getAddress().getNative(), 6);

    const uint8_t* mfg = nullptr; uint16_t mfgLen = 0;
    const uint8_t* svc = nullptr; uint16_t svcLen = 0; uint16_t svcUuid = 0;
    const uint8_t* bth = nullptr; uint16_t bthLen = 0;
    char localName[32] = {0};

    size_t i = 0;
    while (i + 2 <= len) {
      uint8_t alen = data[i], type = data[i + 1];
      if (alen < 1 || i + 1 + alen > len) break;
      const uint8_t* val = data + i + 2;
      uint8_t vlen = alen - 1;
      if (type == 0xFF) { mfg = val; mfgLen = vlen; }
      else if (type == 0x16 && vlen >= 2) { svcUuid = val[0] | (val[1] << 8); svc = val + 2; svcLen = vlen - 2; }
      else if (type == 0xD2) { bth = val; bthLen = vlen; }
      else if (type == 0x09 || type == 0x08) { size_t c = vlen < 31 ? vlen : 31; memcpy(localName, val, c); localName[c] = 0; }
      i += alen + 1;
    }

    BleSample s;
    bleSampleInit(&s);
    memcpy(s.mac, mac, 6);
    s.rssi = d->getRSSI();

    bool ok = false;
    if (svcUuid == 0xFE95 && svcLen) ok = bleDecodeXiaomi(svc, svcLen, mac, nullptr, &s);
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

    char macs[13];
    macStr(mac, macs);
    char raw[96] = {0};
    hexStr(data, len, raw, sizeof(raw));
    upsert(macs, &s, ok, ok ? nullptr : raw);
  }
};

void startBle() {
  NimBLEDevice::init("StacjaBLE");
  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setActiveScan(false);            // pasywny - mniej prądu, łapie reklamy
  scan->setInterval(160);
  scan->setWindow(160);
  scan->setAdvertisedDeviceCallbacks(new ScanCallbacks(), false);
  scan->start(0, nullptr, true);         // skan ciągły
}

// --------------------------- WWW ---------------------------
WebServer web(80);

static String htmlEscape(String s) {
  s.replace("&", "&amp;"); s.replace("<", "&lt;"); s.replace(">", "&gt;"); s.replace("\"", "&quot;");
  return s;
}

static String pageShell(String body) {
  String h = "<!DOCTYPE html><html><head><meta charset='utf-8'>"
             "<meta name='viewport' content='width=device-width,initial-scale=1'>"
             "<title>Bramka BLE</title><style>"
             "body{font-family:system-ui,sans-serif;background:#111;color:#eee;padding:16px}"
             "a{color:#0cf;text-decoration:none;margin-right:12px}"
             "table{width:100%;border-collapse:collapse;margin-top:12px}"
             "th,td{border-bottom:1px solid #333;padding:6px;text-align:left;font-size:.85em}"
             "th{color:#0cf}.ok{color:#4c4}.raw{font-family:monospace;font-size:.7em;color:#9ad0ff;word-break:break-all}"
             "input,textarea{width:100%;max-width:420px;padding:6px;margin:4px 0;background:#1a1a2e;color:#eee;border:1px solid #333;border-radius:4px}"
             "button{background:#0cf;color:#000;border:0;padding:8px 18px;border-radius:4px;font-weight:bold;cursor:pointer}"
             ".card{background:#16161f;padding:12px;border-radius:8px;margin-bottom:12px}"
             "</style></head><body><h2>📡 Bramka BLE</h2>"
             "<a href='/'>Czujniki</a><a href='/setup'>Ustawienia</a><a href='/json'>JSON</a>"
             + body + "</body></html>";
  return h;
}

void handleRoot() {
  String b = "<table><tr><th>MAC</th><th>Nazwa</th><th>Typ</th><th>T</th><th>RH</th><th>Bat</th><th>RSSI</th><th>Wiek</th></tr>";
  for (int i = 0; i < g_sensorCount; i++) {
    Sensor& s = g_sensors[i];
    unsigned age = (millis() - s.lastMs) / 1000;
    String t = isnan(s.temp) ? "—" : String(s.temp, 1);
    String h = isnan(s.hum) ? "—" : String(s.hum, 1);
    String bt = s.batt < 0 ? "—" : String(s.batt);
    b += "<tr><td class='raw'>" + String(s.mac) + "</td><td>" + htmlEscape(s.name) +
         "</td><td>" + htmlEscape(s.type) + "</td><td>" + t + "</td><td>" + h +
         "</td><td>" + bt + "</td><td>" + String(s.rssi) + "</td><td>" + String(age) + "s</td></tr>";
    if (s.raw[0]) b += "<tr><td colspan='8' class='raw'>ramka: " + String(s.raw) + "</td></tr>";
  }
  if (!g_sensorCount) b += "<tr><td colspan='8'>Brak czujników BLE w zasięgu</td></tr>";
  b += "</table>";
  web.send(200, "text/html", pageShell(b));
}

void handleSetup() {
  String b = "<div class='card'><h3>Konfiguracja</h3><form method='post' action='/save'>"
             "Wi-Fi SSID<br><input name='ssid' value='" + htmlEscape(g_ssid) + "'>"
             "<br>Hasło<br><input name='pass' type='password' value='" + htmlEscape(g_pass) + "'>"
             "<br>Adres mastera<br><input name='url' value='" + htmlEscape(g_url) + "'>"
             "<br><small>Statyczny IP (puste = DHCP)</small><br>"
             "<input name='ip' value='" + htmlEscape(g_ip) + "' placeholder='IP'>"
             "<input name='gw' value='" + htmlEscape(g_gw) + "' placeholder='Brama'>"
             "<input name='mask' value='" + htmlEscape(g_mask) + "' placeholder='Maska'>"
             "<br><small>Nazwy czujników: MAC=Nazwa, MAC2=Nazwa2</small><br>"
             "<textarea name='names' rows='3'>" + htmlEscape(g_names) + "</textarea>"
             "<br><button type='submit'>Zapisz i restart</button></form></div>";
  web.send(200, "text/html", pageShell(b));
}

void handleSave() {
  g_ssid = web.arg("ssid");
  g_pass = web.arg("pass");
  g_url  = web.arg("url");
  g_ip   = web.arg("ip");
  g_gw   = web.arg("gw");
  g_mask = web.arg("mask");
  g_names = web.arg("names");
  if (g_url.length() == 0) g_url = "http://192.168.1.143/api/remote/ble";
  saveCfg();
  web.send(200, "text/html", pageShell("<p class='ok'>Zapisano - restart...</p>"));
  delay(300);
  ESP.restart();
}

void handleJson() {
  JsonDocument doc;
  JsonArray arr = doc["sensors"].to<JsonArray>();
  for (int i = 0; i < g_sensorCount; i++) {
    Sensor& s = g_sensors[i];
    JsonObject o = arr.add<JsonObject>();
    o["mac"] = s.mac;
    if (s.name[0]) o["name"] = s.name;
    if (!isnan(s.temp)) o["temp"] = s.temp;
    if (!isnan(s.hum)) o["hum"] = s.hum;
    if (s.batt >= 0) o["batt"] = s.batt;
    o["rssi"] = s.rssi;
    o["type"] = s.type;
  }
  String out;
  serializeJson(doc, out);
  web.send(200, "application/json", out);
}

// --------------------------- wysyłka do mastera ---------------------------
unsigned long g_lastSend = 0;

void sendToMaster() {
  if (!g_url.length() || !g_sensorCount) return;
  JsonDocument doc;
  JsonArray arr = doc["sensors"].to<JsonArray>();
  int n = 0;
  for (int i = 0; i < g_sensorCount; i++) {
    Sensor& s = g_sensors[i];
    if (!s.type[0] || s.raw[0]) continue;   // tylko zdekodowane
    if (millis() - s.lastMs > 120000) continue;  // nieaktualne
    JsonObject o = arr.add<JsonObject>();
    o["mac"] = s.mac;
    if (s.name[0]) o["name"] = s.name;
    if (!isnan(s.temp)) o["temp"] = s.temp;
    if (!isnan(s.hum)) o["hum"] = s.hum;
    if (s.batt >= 0) o["batt"] = s.batt;
    o["rssi"] = s.rssi;
    n++;
  }
  if (!n) return;
  String body;
  serializeJson(doc, body);
  HTTPClient h;
  h.setConnectTimeout(3000);
  h.setTimeout(4000);
  if (h.begin(g_url)) {
    h.addHeader("Content-Type", "application/json");
    int code = h.POST(body);
    if (code != 200) Serial.printf("[BLE] master %s -> HTTP %d\n", g_url.c_str(), code);
    h.end();
  }
}

// --------------------------- WiFi ---------------------------
void setupWifi() {
  WiFi.mode(WIFI_STA);
  if (g_ip.length() && g_gw.length() && g_mask.length()) {
    IPAddress ip, gw, mk;
    if (ip.fromString(g_ip) && gw.fromString(g_gw) && mk.fromString(g_mask))
      WiFi.config(ip, gw, mk);
  }
  if (g_ssid.length()) {
    WiFi.begin(g_ssid.c_str(), g_pass.c_str());
    for (int i = 0; i < 30 && WiFi.status() != WL_CONNECTED; i++) delay(500);
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[BLE] IP: %s\n", WiFi.localIP().toString().c_str());
    return;
  }
  // AP awaryjny
  g_apMode = true;
  WiFi.mode(WIFI_AP);
  WiFi.softAP("StacjaBLE", "");
  Serial.printf("[BLE] AP: %s\n", WiFi.softAPIP().toString().c_str());
}

void setup() {
  Serial.begin(115200);
  delay(200);
  loadCfg();
  setupWifi();
  startBle();

  web.on("/", handleRoot);
  web.on("/setup", handleSetup);
  web.on("/save", handleSave);
  web.on("/json", handleJson);
  web.begin();
  Serial.println("[BLE] gotowe");
}

void loop() {
  web.handleClient();
  unsigned long now = millis();
  if (now - g_lastSend >= 30000) {
    g_lastSend = now;
    sendToMaster();
  }
  delay(2);
}
