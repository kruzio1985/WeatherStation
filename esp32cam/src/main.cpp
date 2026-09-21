/* =============================================================================
 * Stacja Pogody - zewnętrzna kamera ESP32-CAM (AI-Thinker)
 * Copyright (c) 2026 kruzio1985 - https://github.com/kruzio1985
 * Licencja: Stacja Pogody Non-Commercial License 1.0 (plik LICENSE)
 * Użytek niekomercyjny. Kontakt: kruzio1985@users.noreply.github.com
 * =============================================================================
 *
 * Moduł robi zdjęcia JPEG (OV2640), wystawia własną stronę / podgląd i po
 * każdym zdjęciu wysyła je HTTP POST do stacji-mastera pod adres:
 *     <CAM_MASTER_URL>/api/camera/upload
 * Master zapisuje zdjęcie na karcie SD, analizuje je (jasność, luksy, chmury,
 * ruch) i pilnuje limitów galerii.
 *
 * Ustawienia można zmienić bez ponownej kompilacji:
 *   - komenda szeregowa (monitor 115200):  SET ssid haslo url
 *   - strona www kamery: http://<ip-kamery>/
 */
#define CAMERA_MODEL_AI_THINKER
#include "esp_camera.h"
#include "camera_pins.h"
#include "config.h"
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <ArduinoOTA.h>
#include <esp_task_wdt.h>

WebServer server(80);
Preferences prefs;

static String g_ssid = CAM_WIFI_SSID;
static String g_pass = CAM_WIFI_PASS;
static String g_url  = CAM_MASTER_URL;
static String g_err  = "";
static uint32_t g_photos = 0;

// ---------------------------------------------------------------------------
//  Kamera
// ---------------------------------------------------------------------------
static bool initCamera() {
  camera_config_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.ledc_channel = LEDC_CHANNEL_0;
  cfg.ledc_timer   = LEDC_TIMER_0;
  cfg.pin_d0  = Y2_GPIO_NUM;
  cfg.pin_d1  = Y3_GPIO_NUM;
  cfg.pin_d2  = Y4_GPIO_NUM;
  cfg.pin_d3  = Y5_GPIO_NUM;
  cfg.pin_d4  = Y6_GPIO_NUM;
  cfg.pin_d5  = Y7_GPIO_NUM;
  cfg.pin_d6  = Y8_GPIO_NUM;
  cfg.pin_d7  = Y9_GPIO_NUM;
  cfg.pin_xclk  = XCLK_GPIO_NUM;
  cfg.pin_pclk  = PCLK_GPIO_NUM;
  cfg.pin_vsync = VSYNC_GPIO_NUM;
  cfg.pin_href  = HREF_GPIO_NUM;
  cfg.pin_sscb_sda = SIOD_GPIO_NUM;
  cfg.pin_sscb_scl = SIOC_GPIO_NUM;
  cfg.pin_pwdn  = PWDN_GPIO_NUM;
  cfg.pin_reset = RESET_GPIO_NUM;
  cfg.xclk_freq_hz = 20000000;
  cfg.pixel_format = PIXFORMAT_JPEG;
  cfg.frame_size   = CAM_FRAME_SIZE;
  cfg.jpeg_quality = CAM_JPEG_QUALITY;
  cfg.fb_count     = 2;
  cfg.grab_mode    = CAMERA_GRAB_LATEST;

  esp_err_t e = esp_camera_init(&cfg);
  if (e != ESP_OK) {
    g_err = "esp_camera_init: 0x" + String(e, HEX);
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
//  Wysyłka zdjęcia do mastera
// ---------------------------------------------------------------------------
static bool postToMaster(const uint8_t* jpg, size_t len) {
  if (!jpg || len == 0) return false;
  if (g_url.length() == 0) return false;

  String endpoint = g_url;
  if (!endpoint.endsWith("/")) endpoint += "/";
  endpoint += "api/camera/upload";

  WiFiClient client;
  HTTPClient http;
  http.setConnectTimeout(6000);
  http.setTimeout(12000);
  if (!http.begin(client, endpoint)) return false;
  http.addHeader("Content-Type", "image/jpeg");
  int code = http.POST((uint8_t*)jpg, len);
  http.end();
  return (code == 200);
}

// ---------------------------------------------------------------------------
//  Obsługa HTTP
// ---------------------------------------------------------------------------
static void sendJpegNow() {
  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) {
    server.send(500, "text/plain", "brak klatki");
    return;
  }
  g_photos++;
  // Najpierw do mastera (zapis na SD + analiza), potem do przeglądarki.
  if (!postToMaster(fb->buf, fb->len)) {
    Serial.println("[KAM] nie udało się wysłać zdjęcia do mastera");
  }
  server.setContentLength(fb->len);
  server.send(200, "image/jpeg", "");
  server.sendContent((const char*)fb->buf, fb->len);
  esp_camera_fb_return(fb);
}

static void handleRoot() {
  String h;
  h.reserve(1024);
  h += "<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>";
  h += "<title>Kamera Stacja Pogody</title></head><body style='font-family:system-ui;max-width:640px;margin:24px auto;padding:0 16px'>";
  h += "<h2>Kamera ESP32-CAM</h2>";
  h += "<p>Status: " + String(g_err.length() ? g_err : "OK") + " • zdjęć: " + String(g_photos) + "</p>";
  h += "<p><a href='/capture'><button>Zrób zdjęcie</button></a> "
       "<a href='/status'><button>JSON</button></a></p>";
  h += "<form method='post' action='/config'>";
  h += "<p>Wi-Fi SSID: <input name='ssid' value='" + g_ssid + "'></p>";
  h += "<p>Wi-Fi hasło: <input name='pass' type='password' value='" + g_pass + "'></p>";
  h += "<p>Adres mastera: <input name='url' value='" + g_url + "' placeholder='http://192.168.1.143'></p>";
  h += "<p><button type='submit'>Zapisz i połącz ponownie</button></p></form>";
  h += "</body></html>";
  server.send(200, "text/html", h);
}

static void handleStatus() {
  String s;
  s.reserve(256);
  s += "{\"ip\":\"" + WiFi.localIP().toString() + "\","
       "\"ssid\":\"" + g_ssid + "\","
       "\"master\":\"" + g_url + "\","
       "\"photos\":" + String(g_photos) + ","
       "\"error\":\"" + g_err + "\"}";
  server.send(200, "application/json", s);
}

static void handleConfig() {
  if (server.hasArg("ssid")) g_ssid = server.arg("ssid");
  if (server.hasArg("pass")) g_pass = server.arg("pass");
  if (server.hasArg("url"))  g_url  = server.arg("url");
  g_ssid.trim(); g_pass.trim(); g_url.trim();

  prefs.putString("ssid", g_ssid);
  prefs.putString("pass", g_pass);
  prefs.putString("url", g_url);

  server.send(200, "text/html",
    "<meta charset='utf-8'><p>Zapisano. Ponowne łączenie…</p>"
    "<script>setTimeout(()=>location='/',3000)</script>");
  WiFi.disconnect();
  delay(500);
  WiFi.begin(g_ssid.c_str(), g_pass.c_str());
}

// ---------------------------------------------------------------------------
//  Konfiguracja szeregowa:  SET ssid haslo url
// ---------------------------------------------------------------------------
static void handleSerialLine(const String& line) {
  String s = line;
  s.trim();
  if (!s.startsWith("SET ") && !s.startsWith("set ")) return;
  s = s.substring(4);
  s.trim();
  int sp1 = s.indexOf(' ');
  int sp2 = sp1 >= 0 ? s.indexOf(' ', sp1 + 1) : -1;
  String ssid, pass, url;
  if (sp2 >= 0) {
    ssid = s.substring(0, sp1);
    pass = s.substring(sp1 + 1, sp2);
    url  = s.substring(sp2 + 1);
  } else if (sp1 >= 0) {
    ssid = s.substring(0, sp1);
    pass = s.substring(sp1 + 1);
  } else {
    ssid = s;
  }
  ssid.trim(); pass.trim(); url.trim();
  if (ssid.length()) g_ssid = ssid;
  if (pass.length()) g_pass = pass;
  if (url.length())  g_url  = url;
  prefs.putString("ssid", g_ssid);
  prefs.putString("pass", g_pass);
  prefs.putString("url", g_url);
  Serial.printf("[KAM] Zapisano: ssid=%s master=%s\n", g_ssid.c_str(), g_url.c_str());
  WiFi.disconnect();
  delay(500);
  WiFi.begin(g_ssid.c_str(), g_pass.c_str());
}

// ---------------------------------------------------------------------------
//  OTA (aktualizacja bez kabla, port 3232)
// ---------------------------------------------------------------------------
static void initOTA() {
  ArduinoOTA.setHostname("stacja-cam");
  ArduinoOTA.setPassword(CAM_OTA_PASS);
  ArduinoOTA.onStart([]() { Serial.println("[OTA] Start"); });
  ArduinoOTA.onEnd([]() { Serial.println("\n[OTA] Koniec"); });
  ArduinoOTA.onProgress([](unsigned int p, unsigned int t) {
    if (p % 25 == 0) Serial.printf("[OTA] %u%%\n", p / (t / 100));
  });
  ArduinoOTA.onError([](ota_error_t e) {
    Serial.printf("[OTA] Błąd %u\n", e);
  });
  ArduinoOTA.begin();
  Serial.println("[OTA] Gotowe (port 3232)");
}

// ---------------------------------------------------------------------------
//  setup / loop
// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  Serial.println("\n[KAM] Start kamery Stacja Pogody (ESP32-CAM)");

  prefs.begin("stacja-cam", false);
  g_ssid = prefs.getString("ssid", CAM_WIFI_SSID);
  g_pass = prefs.getString("pass", CAM_WIFI_PASS);
  g_url  = prefs.getString("url",  CAM_MASTER_URL);

  if (g_ssid.length() == 0 || g_pass.length() == 0) {
    Serial.println("[KAM] BRAK KONFIGURACJI WIFI. Podaj przez monitor (115200):");
    Serial.println("      SET ssid haslo url");
  }

  WiFi.mode(WIFI_STA);
  WiFi.begin(g_ssid.c_str(), g_pass.c_str());

  if (!initCamera()) {
    Serial.printf("[KAM] BŁĄD: %s\n", g_err.c_str());
  } else {
    Serial.println("[KAM] Kamera gotowa");
  }

  server.on("/", handleRoot);
  server.on("/status", handleStatus);
  server.on("/config", HTTP_POST, handleConfig);
  server.on("/capture", sendJpegNow);
  server.begin();
  Serial.println("[KAM] Serwer HTTP na porcie 80");
  initOTA();
}

void loop() {
  static uint32_t lastLog = 0;
  uint32_t now = millis();

  if (now - lastLog >= 15000) {
    lastLog = now;
    if (WiFi.status() == WL_CONNECTED) {
      Serial.printf("[KAM] IP %s • master %s • zdjęć %u\n",
                    WiFi.localIP().toString().c_str(), g_url.c_str(), g_photos);
    } else {
      Serial.printf("[KAM] Wi-Fi rozłączone (status %d)…\n", WiFi.status());
    }
  }

  while (Serial.available()) {
    String line = Serial.readStringUntil('\n');
    handleSerialLine(line);
  }

  server.handleClient();
  ArduinoOTA.handle();
  esp_task_wdt_reset();
}
