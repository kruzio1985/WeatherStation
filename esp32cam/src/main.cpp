/* =============================================================================
 * Stacja Pogody - zewnętrzna kamera ESP32-CAM (AI-Thinker)
 * Copyright (c) 2026 kruzio1985 - https://github.com/kruzio1985
 * Licencja: PolyForm Noncommercial License 1.0.0 (plik LICENSE)
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

WebServer server(80);
Preferences prefs;

static String g_ssid = CAM_WIFI_SSID;
static String g_pass = CAM_WIFI_PASS;
static String g_url  = CAM_MASTER_URL;
static String g_err  = "";
static uint32_t g_photos = 0;

// --- Sieć: DHCP / stały adres IP ---
static bool    g_static_ip = false;
static String  g_ip   = "192.168.1.200";
static String  g_gw   = "192.168.1.1";
static String  g_mask = "255.255.255.0";
static String  g_dns  = "1.1.1.1";
static String  g_ap_pass = CAM_AP_PASS;
static bool    g_ap_active = false;

// --- Ustawienia obrazu (OV2640) ---
static int  g_framesize  = CAM_FRAME_SIZE;    // framesize_t 0..13
static int  g_quality    = CAM_JPEG_QUALITY;  // 0..63 (mniej = lepsza)
static int  g_brightness = 0;                 // -2..2
static int  g_contrast   = 0;                 // -2..2
static int  g_saturation = 0;                 // -2..2
static int  g_sharpness  = 0;                 // -2..2
static int  g_denoise    = 0;                 // -2..2
static bool g_awb        = true;              // auto balans bieli
static int  g_wb_mode    = 0;                 // 0 auto, 1 słonecznie, 2 pochmurno, 3 biuro, 4 dom
static bool g_aec        = true;              // auto ekspozycja
static int  g_ae_level   = 0;                 // -2..2
static bool g_agc        = true;              // auto wzmocnienie
static int  g_agc_gain   = 0;                 // 0..30
static bool g_hmirror    = false;
static bool g_vflip      = false;
static int  g_special    = 0;                 // efekt specjalny 0..6
static bool g_lenc       = true;              // korekcja obiektywu
static bool g_raw_gma    = true;              // gamma
static bool g_flash_led  = false;             // LED doświetlająca: błysk przy zdjęciu

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
  cfg.pin_sccb_sda = SIOD_GPIO_NUM;
  cfg.pin_sccb_scl = SIOC_GPIO_NUM;
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
//  Zapis / odczyt ustawień (NVS) i zastosowanie ustawień obrazu
// ---------------------------------------------------------------------------
static int clampInt(int v, int lo, int hi) {
  if (v < lo) v = lo;
  if (v > hi) v = hi;
  return v;
}

static String htmlEsc(const String& s) {
  String r;
  r.reserve(s.length() + 16);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    switch (c) {
      case '&': r += "&amp;"; break;
      case '<': r += "&lt;"; break;
      case '>': r += "&gt;"; break;
      case '"': r += "&quot;"; break;
      case '\'': r += "&#39;"; break;
      default: r += c;
    }
  }
  return r;
}

static String optSel(int cur, int val, const char* label) {
  String s = "<option value=\"" + String(val) + "\"";
  if (val == cur) s += " selected";
  s += ">" + String(label) + "</option>";
  return s;
}

static String chk(bool v) { return v ? " checked" : ""; }

static String levSelect(const char* name, int cur) {
  static const char* labels[5] = {"-2", "-1", "0", "+1", "+2"};
  String s = "<select name='" + String(name) + "'>";
  for (int i = -2; i <= 2; i++) {
    s += "<option value=\"" + String(i) + "\"";
    if (cur == i) s += " selected";
    s += ">" + String(labels[i + 2]) + "</option>";
  }
  s += "</select>";
  return s;
}

static void loadConfig() {
  g_ssid = prefs.getString("ssid", CAM_WIFI_SSID);
  g_pass = prefs.getString("pass", CAM_WIFI_PASS);
  g_url  = prefs.getString("url",  CAM_MASTER_URL);

  g_static_ip = prefs.getBool("stip", false);
  g_ip   = prefs.getString("ip",   "192.168.1.200");
  g_gw   = prefs.getString("gw",   "192.168.1.1");
  g_mask = prefs.getString("mask", "255.255.255.0");
  g_dns  = prefs.getString("dns",  "1.1.1.1");
  g_ap_pass = prefs.getString("appass", CAM_AP_PASS);

  g_framesize  = clampInt(prefs.getInt("fsize", CAM_FRAME_SIZE), 0, 13);
  g_quality    = clampInt(prefs.getInt("qual", CAM_JPEG_QUALITY), 0, 63);
  g_brightness = clampInt(prefs.getInt("brt", 0), -2, 2);
  g_contrast   = clampInt(prefs.getInt("ctr", 0), -2, 2);
  g_saturation = clampInt(prefs.getInt("sat", 0), -2, 2);
  g_sharpness  = clampInt(prefs.getInt("shp", 0), -2, 2);
  g_denoise    = clampInt(prefs.getInt("den", 0), -2, 2);
  g_awb      = prefs.getBool("awb", true);
  g_wb_mode  = clampInt(prefs.getInt("wbm", 0), 0, 4);
  g_aec      = prefs.getBool("aec", true);
  g_ae_level = clampInt(prefs.getInt("ael", 0), -2, 2);
  g_agc      = prefs.getBool("agc", true);
  g_agc_gain = clampInt(prefs.getInt("agg", 0), 0, 30);
  g_hmirror  = prefs.getBool("hmir", false);
  g_vflip    = prefs.getBool("vflip", false);
  g_special  = clampInt(prefs.getInt("fx", 0), 0, 6);
  g_lenc     = prefs.getBool("lenc", true);
  g_raw_gma  = prefs.getBool("gma", true);
  g_flash_led = prefs.getBool("flash", false);
}

static void saveConfig() {
  prefs.putString("ssid", g_ssid);
  prefs.putString("pass", g_pass);
  prefs.putString("url", g_url);
  prefs.putBool("stip", g_static_ip);
  prefs.putString("ip", g_ip);
  prefs.putString("gw", g_gw);
  prefs.putString("mask", g_mask);
  prefs.putString("dns", g_dns);
  prefs.putString("appass", g_ap_pass);
  prefs.putInt("fsize", g_framesize);
  prefs.putInt("qual", g_quality);
  prefs.putInt("brt", g_brightness);
  prefs.putInt("ctr", g_contrast);
  prefs.putInt("sat", g_saturation);
  prefs.putInt("shp", g_sharpness);
  prefs.putInt("den", g_denoise);
  prefs.putBool("awb", g_awb);
  prefs.putInt("wbm", g_wb_mode);
  prefs.putBool("aec", g_aec);
  prefs.putInt("ael", g_ae_level);
  prefs.putBool("agc", g_agc);
  prefs.putInt("agg", g_agc_gain);
  prefs.putBool("hmir", g_hmirror);
  prefs.putBool("vflip", g_vflip);
  prefs.putInt("fx", g_special);
  prefs.putBool("lenc", g_lenc);
  prefs.putBool("gma", g_raw_gma);
  prefs.putBool("flash", g_flash_led);
}

static void applySensorSettings() {
  sensor_t* s = esp_camera_sensor_get();
  if (!s) return;
  if (s->set_framesize)    s->set_framesize(s, (framesize_t)g_framesize);
  if (s->set_quality)      s->set_quality(s, g_quality);
  if (s->set_brightness)   s->set_brightness(s, g_brightness);
  if (s->set_contrast)     s->set_contrast(s, g_contrast);
  if (s->set_saturation)   s->set_saturation(s, g_saturation);
  if (s->set_sharpness)    s->set_sharpness(s, g_sharpness);
  if (s->set_denoise)      s->set_denoise(s, g_denoise);
  if (s->set_whitebal)     s->set_whitebal(s, g_awb);
  if (s->set_wb_mode)      s->set_wb_mode(s, g_wb_mode);
  if (s->set_exposure_ctrl) s->set_exposure_ctrl(s, g_aec);
  if (s->set_ae_level)     s->set_ae_level(s, g_ae_level);
  if (s->set_gain_ctrl)    s->set_gain_ctrl(s, g_agc);
  if (s->set_agc_gain)     s->set_agc_gain(s, g_agc_gain);
  if (s->set_hmirror)      s->set_hmirror(s, g_hmirror);
  if (s->set_vflip)        s->set_vflip(s, g_vflip);
  if (s->set_special_effect) s->set_special_effect(s, g_special);
  if (s->set_lenc)         s->set_lenc(s, g_lenc);
  if (s->set_raw_gma)      s->set_raw_gma(s, g_raw_gma);
}

static void reconnectWifi() {
  WiFi.disconnect(true);
  delay(300);
  if (g_ap_active) {
    WiFi.softAPdisconnect(true);
    g_ap_active = false;
  }
  WiFi.mode(WIFI_STA);
  if (g_static_ip) {
    IPAddress ip, gw, mask, dns;
    if (ip.fromString(g_ip) && gw.fromString(g_gw) && mask.fromString(g_mask)) {
      if (dns.fromString(g_dns)) WiFi.config(ip, gw, mask, dns);
      else WiFi.config(ip, gw, mask);
    }
  }
  WiFi.begin(g_ssid.c_str(), g_pass.c_str());
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
  if (g_flash_led) {
    digitalWrite(FLASH_LED_GPIO, HIGH);
    delay(180);  // błysk tylko na czas zdjęcia, nie świeci cały czas
  }
  camera_fb_t* fb = esp_camera_fb_get();
  if (g_flash_led) digitalWrite(FLASH_LED_GPIO, LOW);
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
  h.reserve(7000);
  h += "<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>";
  h += "<title>Kamera Stacja Pogody</title><style>";
  h += "body{font-family:system-ui,sans-serif;max-width:720px;margin:16px auto;padding:0 14px;background:#111;color:#eee}";
  h += "h2{color:#8cf}fieldset{border:1px solid #333;border-radius:8px;margin:12px 0;padding:10px 14px}";
  h += "legend{color:#8cf;font-weight:600;padding:0 6px}";
  h += "label{display:flex;justify-content:space-between;align-items:center;margin:7px 0;gap:10px}";
  h += "input,select{background:#1d1d1d;color:#eee;border:1px solid #444;border-radius:5px;padding:5px 8px;max-width:62%;box-sizing:border-box}";
  h += "input[type=checkbox]{width:18px;height:18px;max-width:none}";
  h += "button{background:#0a84ff;color:#fff;border:0;border-radius:6px;padding:10px 18px;font-size:15px;cursor:pointer}";
  h += ".row{display:flex;gap:8px;flex-wrap:wrap}.row label{flex:1;min-width:150px}";
  h += "a{text-decoration:none}.hint{color:#999;font-size:12px;margin:2px 0}";
  h += "</style></head><body>";
  h += "<h2>📷 Kamera ESP32-CAM</h2>";

  // --- Status ---
  h += "<fieldset><legend>Status</legend><p style='margin:4px 0'>";
  if (g_ap_active) {
    h += "<b>Tryb AP (konfiguracja)</b> — SSID <b>StacjaKam</b>, IP: " + WiFi.softAPIP().toString() + "<br>";
  }
  if (WiFi.status() == WL_CONNECTED) {
    h += "Wi-Fi: połączono • IP: " + WiFi.localIP().toString();
  } else {
    h += "Wi-Fi: <span style='color:#f77'>brak połączenia (status " + String(WiFi.status()) + ")</span>";
  }
  h += "<br>Master: " + htmlEsc(g_url) + " • zdjęć: " + String(g_photos);
  h += "<br>Kamera: " + String(g_err.length() ? htmlEsc(g_err) : "OK") + "</p></fieldset>";

  h += "<form method='post' action='/config'>";

  // --- Wi-Fi ---
  h += "<fieldset><legend>Wi-Fi</legend>";
  h += "<label>SSID: <input name='ssid' value='" + htmlEsc(g_ssid) + "'></label>";
  h += "<label>Hasło: <input name='pass' type='password' value='" + htmlEsc(g_pass) + "'></label>";
  h += "<label>Adres mastera: <input name='url' value='" + htmlEsc(g_url) + "' placeholder='http://192.168.1.143'></label>";
  h += "<label>Hasło AP (tryb awaryjny): <input name='appass' value='" + htmlEsc(g_ap_pass) + "'></label>";
  h += "<p class='hint'>Gdy kamera nie połączy się z Wi-Fi, uruchomi punkt dostępowy <b>StacjaKam</b> (192.168.4.1) z podanym hasłem.</p>";
  h += "</fieldset>";

  // --- Adres IP ---
  h += "<fieldset><legend>Adres IP</legend>";
  h += "<label><span>Tryb:</span><span>";
  h += "<label style='display:inline;margin:0'><input type='radio' name='ipmode' value='0'" + String(g_static_ip ? "" : " checked") + "> DHCP (auto)</label> ";
  h += "<label style='display:inline;margin:0'><input type='radio' name='ipmode' value='1'" + String(g_static_ip ? " checked" : "") + "> Stały IP</label>";
  h += "</span></label>";
  h += "<div class='row'>";
  h += "<label>IP: <input name='ip' value='" + htmlEsc(g_ip) + "'></label>";
  h += "<label>Brama: <input name='gw' value='" + htmlEsc(g_gw) + "'></label>";
  h += "<label>Maska: <input name='mask' value='" + htmlEsc(g_mask) + "'></label>";
  h += "<label>DNS: <input name='dns' value='" + htmlEsc(g_dns) + "'></label>";
  h += "</div></fieldset>";

  // --- Obraz ---
  h += "<fieldset><legend>Obraz</legend>";
  h += "<label>Rozdzielczość: <select name='fsize'>";
  h += optSel(g_framesize, 4, "240x240");
  h += optSel(g_framesize, 5, "QVGA 320x240");
  h += optSel(g_framesize, 8, "VGA 640x480");
  h += optSel(g_framesize, 9, "SVGA 800x600");
  h += optSel(g_framesize, 10, "XGA 1024x768");
  h += optSel(g_framesize, 11, "HD 1280x720");
  h += optSel(g_framesize, 12, "SXGA 1280x1024");
  h += optSel(g_framesize, 13, "UXGA 1600x1200");
  h += "</select></label>";
  h += "<label>Jakość JPEG (0-63, mniej=lepsza): <input name='qual' type='number' min='0' max='63' value='" + String(g_quality) + "'></label>";
  h += "<label>Jasność: " + levSelect("brt", g_brightness) + "</label>";
  h += "<label>Kontrast: " + levSelect("ctr", g_contrast) + "</label>";
  h += "<label>Nasycenie: " + levSelect("sat", g_saturation) + "</label>";
  h += "<label>Ostrość: " + levSelect("shp", g_sharpness) + "</label>";
  h += "<label>Redukcja szumu: " + levSelect("den", g_denoise) + "</label>";
  h += "</fieldset>";

  // --- Ekspozycja i balans bieli ---
  h += "<fieldset><legend>Ekspozycja i balans bieli</legend>";
  h += "<label><span>Auto balans bieli (AWB)</span><input type='checkbox' name='awb'" + chk(g_awb) + "></label>";
  h += "<label>Tryb balansu bieli: <select name='wbm'>";
  h += optSel(g_wb_mode, 0, "Auto");
  h += optSel(g_wb_mode, 1, "Słonecznie");
  h += optSel(g_wb_mode, 2, "Pochmurno");
  h += optSel(g_wb_mode, 3, "Biuro (świetlówka)");
  h += optSel(g_wb_mode, 4, "Dom (żarówka)");
  h += "</select></label>";
  h += "<label><span>Auto ekspozycja (AEC)</span><input type='checkbox' name='aec'" + chk(g_aec) + "></label>";
  h += "<label>Poziom ekspozycji: " + levSelect("ael", g_ae_level) + "</label>";
  h += "<label><span>Auto wzmocnienie (AGC)</span><input type='checkbox' name='agc'" + chk(g_agc) + "></label>";
  h += "<label>Wzmocnienie (0-30): <input name='agg' type='number' min='0' max='30' value='" + String(g_agc_gain) + "'></label>";
  h += "</fieldset>";

  // --- Kadr, efekty, LED ---
  h += "<fieldset><legend>Kadr, efekty i LED</legend>";
  h += "<label><span>Odbicie poziome (mirror)</span><input type='checkbox' name='hmir'" + chk(g_hmirror) + "></label>";
  h += "<label><span>Obrót pionowy (flip)</span><input type='checkbox' name='vflip'" + chk(g_vflip) + "></label>";
  h += "<label>Efekt: <select name='fx'>";
  h += optSel(g_special, 0, "Brak");
  h += optSel(g_special, 1, "Negatyw");
  h += optSel(g_special, 2, "Czarno-biały");
  h += optSel(g_special, 3, "Czerwony");
  h += optSel(g_special, 4, "Zielony");
  h += optSel(g_special, 5, "Niebieski");
  h += optSel(g_special, 6, "Sepia");
  h += "</select></label>";
  h += "<label><span>Korekcja obiektywu (lenc)</span><input type='checkbox' name='lenc'" + chk(g_lenc) + "></label>";
  h += "<label><span>Gamma (raw gma)</span><input type='checkbox' name='gma'" + chk(g_raw_gma) + "></label>";
  h += "<label><span>LED doświetlająca (błysk przy zdjęciu)</span><input type='checkbox' name='flash'" + chk(g_flash_led) + "></label>";
  h += "<p class='hint'>LED zapala się tylko na chwilę robienia zdjęcia (jak lampa błyskowa), nie świeci cały czas.</p>";
  h += "</fieldset>";

  h += "<p style='display:flex;gap:10px;flex-wrap:wrap'>";
  h += "<button type='submit'>💾 Zapisz ustawienia</button>";
  h += "<a href='/capture'><button type='button'>📸 Zrób zdjęcie</button></a>";
  h += "<a href='/status'><button type='button'>JSON</button></a>";
  h += "</p></form>";
  h += "</body></html>";
  server.send(200, "text/html", h);
}

static void handleStatus() {
  String s;
  s.reserve(320);
  s += "{\"ip\":\"" + WiFi.localIP().toString() + "\","
       "\"ap\":" + String(g_ap_active ? "true" : "false") + ","
       "\"ap_ip\":\"" + (g_ap_active ? WiFi.softAPIP().toString() : String("")) + "\","
       "\"static_ip\":" + String(g_static_ip ? "true" : "false") + ","
       "\"ssid\":\"" + g_ssid + "\","
       "\"master\":\"" + g_url + "\","
       "\"photos\":" + String(g_photos) + ","
       "\"error\":\"" + g_err + "\"}";
  server.send(200, "application/json", s);
}

static void handleConfig() {
  if (server.hasArg("ssid"))   { g_ssid = server.arg("ssid"); g_ssid.trim(); }
  if (server.hasArg("pass"))   { g_pass = server.arg("pass"); g_pass.trim(); }
  if (server.hasArg("url"))    { g_url  = server.arg("url");  g_url.trim(); }
  if (server.hasArg("appass")) { g_ap_pass = server.arg("appass"); g_ap_pass.trim(); }

  g_static_ip = server.hasArg("ipmode") && server.arg("ipmode").toInt() == 1;
  if (server.hasArg("ip"))   { g_ip = server.arg("ip"); g_ip.trim(); }
  if (server.hasArg("gw"))   { g_gw = server.arg("gw"); g_gw.trim(); }
  if (server.hasArg("mask")) { g_mask = server.arg("mask"); g_mask.trim(); }
  if (server.hasArg("dns"))  { g_dns = server.arg("dns"); g_dns.trim(); }

  if (server.hasArg("fsize")) g_framesize = clampInt(server.arg("fsize").toInt(), 0, 13);
  if (server.hasArg("qual"))  g_quality   = clampInt(server.arg("qual").toInt(), 0, 63);
  if (server.hasArg("brt"))   g_brightness = clampInt(server.arg("brt").toInt(), -2, 2);
  if (server.hasArg("ctr"))   g_contrast   = clampInt(server.arg("ctr").toInt(), -2, 2);
  if (server.hasArg("sat"))   g_saturation = clampInt(server.arg("sat").toInt(), -2, 2);
  if (server.hasArg("shp"))   g_sharpness  = clampInt(server.arg("shp").toInt(), -2, 2);
  if (server.hasArg("den"))   g_denoise    = clampInt(server.arg("den").toInt(), -2, 2);

  g_awb = server.hasArg("awb");
  if (server.hasArg("wbm")) g_wb_mode = clampInt(server.arg("wbm").toInt(), 0, 4);
  g_aec = server.hasArg("aec");
  if (server.hasArg("ael")) g_ae_level = clampInt(server.arg("ael").toInt(), -2, 2);
  g_agc = server.hasArg("agc");
  if (server.hasArg("agg")) g_agc_gain = clampInt(server.arg("agg").toInt(), 0, 30);

  g_hmirror = server.hasArg("hmir");
  g_vflip   = server.hasArg("vflip");
  if (server.hasArg("fx"))   g_special = clampInt(server.arg("fx").toInt(), 0, 6);
  g_lenc    = server.hasArg("lenc");
  g_raw_gma = server.hasArg("gma");
  g_flash_led = server.hasArg("flash");

  saveConfig();
  applySensorSettings();

  server.send(200, "text/html",
    "<meta charset='utf-8'><p>Zapisano. Ponowne łączenie…</p>"
    "<script>setTimeout(()=>location='/',3000)</script>");
  reconnectWifi();
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

  pinMode(FLASH_LED_GPIO, OUTPUT);
  digitalWrite(FLASH_LED_GPIO, LOW);

  prefs.begin("stacja-cam", false);
  loadConfig();

  if (g_ssid.length() == 0 || g_pass.length() == 0) {
    Serial.println("[KAM] BRAK KONFIGURACJI WIFI. Podaj przez monitor (115200):");
    Serial.println("      SET ssid haslo url");
  }

  // Najpierw Wi-Fi (musi być przed server.begin() - lwIP wymaga
  // zainicjalizowanego interfejsu sieciowego).
  WiFi.mode(WIFI_STA);
  if (g_static_ip) {
    IPAddress ip, gw, mask, dns;
    if (ip.fromString(g_ip) && gw.fromString(g_gw) && mask.fromString(g_mask)) {
      if (dns.fromString(g_dns)) WiFi.config(ip, gw, mask, dns);
      else WiFi.config(ip, gw, mask);
      Serial.printf("[KAM] Stały IP: %s (brama %s)\n", g_ip.c_str(), g_gw.c_str());
    }
  }
  WiFi.begin(g_ssid.c_str(), g_pass.c_str());

  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
    delay(200);
  }

  if (WiFi.status() != WL_CONNECTED) {
    // Tryb awaryjny: punkt dostępowy z pełną stroną konfiguracji
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP("StacjaKam", g_ap_pass.c_str());
    g_ap_active = true;
    Serial.printf("[KAM] Tryb AP: StacjaKam / %s (IP %s)\n",
                  g_ap_pass.c_str(), WiFi.softAPIP().toString().c_str());
  }

  if (!initCamera()) {
    Serial.printf("[KAM] BŁĄD: %s\n", g_err.c_str());
  } else {
    Serial.println("[KAM] Kamera gotowa");
    applySensorSettings();
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
      Serial.printf("[KAM] Wi-Fi rozłączone (status %d)%s\n", WiFi.status(),
                    g_ap_active ? " — tryb AP 192.168.4.1" : "");
    }
  }

  while (Serial.available()) {
    String line = Serial.readStringUntil('\n');
    handleSerialLine(line);
  }

  server.handleClient();
  ArduinoOTA.handle();
}
