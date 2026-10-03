/* =============================================================================
 * Stacja Pogody - stacja meteorologiczna na ESP32-S3 WROOM-1 (N16R8)
 * Copyright (c) 2026 kruzio1985 - https://github.com/kruzio1985
 * Licencja: PolyForm Noncommercial License 1.0.0 (plik LICENSE)
 * Użytek niekomercyjny. Kontakt: kruzio1985@users.noreply.github.com
 * =============================================================================
 */
#include "camera.h"
#include "sd_card.h"
#include "sensors.h"
#include "syslog.h"
#include <ArduinoJson.h>
#include <time.h>
#include <string.h>
#include <math.h>
#include <algorithm>

#if STACJA_HAS_CAMERA
#include "esp_camera.h"
#include "img_converters.h"
#include <esp_heap_caps.h>
#include <WiFiClient.h>
#include <HTTPClient.h>
#endif

CameraManager camera;

// Poniżej tej temperatury otoczenia biały kadr może być śniegiem. Powyżej -
// to chmury, mgła albo prześwietlone niebo (śnieg nie pada przy +25 °C).
static const float CAM_SNOW_MAX_TEMP = 2.0f;

// Piny kamery w sekcji "pins" ustawień. Wartość -1 = nieużywane / nieustawione.
static const char* const CAM_PIN_KEYS[] = {
  "cam_xclk", "cam_pclk", "cam_vsync", "cam_href",
  "cam_d0", "cam_d1", "cam_d2", "cam_d3",
  "cam_d4", "cam_d5", "cam_d6", "cam_d7",
  "cam_sda", "cam_scl", "cam_pwdn", "cam_reset"
};

// Wymagane do startu piny magistrali równoległej (bez nich sterownik
// nie ma prawa działać i inicjalizacja skończyłaby się crash-em).
static const char* const CAM_REQUIRED_PINS[] = {
  "cam_xclk", "cam_pclk", "cam_vsync", "cam_href",
  "cam_d0", "cam_d1", "cam_d2", "cam_d3",
  "cam_d4", "cam_d5", "cam_d6", "cam_d7"
};

// Maksymalny rozmiar zdjęcia pobieranego z kamery zewnętrznej. Bufor idzie do
// PSRAM, ale 2 MB to bezpieczny limit (zdjęcie 1600x1200 to ~250 kB).
#define CAM_REMOTE_MAX_BYTES (2UL * 1024UL * 1024UL)

// Okno, w którym identyczny JPEG jest uznawany za duplikat (patrz saveAndAnalyze).
#define CAM_DUP_WINDOW_MS 15000UL

bool CameraManager::configured() const {
#if !STACJA_HAS_CAMERA
  return false;
#else
  for (const char* k : CAM_REQUIRED_PINS) {
    if (config.pinOverride(k, -1) < 0) return false;
  }
  return true;
#endif
}

#if STACJA_HAS_CAMERA
static String cameraErrName(esp_err_t err) {
  switch (err) {
    case ESP_ERR_CAMERA_NOT_DETECTED:   return "nie wykryto kamery na magistrali I2C";
    case ESP_ERR_CAMERA_FAILED_TO_SET_FRAME_SIZE: return "nie udało się ustawić rozdzielczości";
    case ESP_ERR_CAMERA_FAILED_TO_SET_OUT_FORMAT: return "nie udało się ustawić formatu JPEG";
    case ESP_ERR_CAMERA_NOT_SUPPORTED:  return "czujnik nieobsługiwany";
    case ESP_ERR_NO_MEM:                return "brak pamięci (PSRAM)";
    default: {
      static char buf[48];
      snprintf(buf, sizeof(buf), "esp_camera_init błąd 0x%X", (int)err);
      return buf;
    }
  }
}

// Odczytywanie rozmiaru JPEG z nagłówka (marker SOF). Wystarczy do
// wyliczenia rozmiaru miniatury RGB565 zwracanej przez jpg2rgb565().
static bool parseJpegSize(const uint8_t* d, size_t len, uint16_t& w, uint16_t& h) {
  if (!d || len < 4 || d[0] != 0xFF || d[1] != 0xD8) return false;
  size_t i = 2;
  while (i + 4 <= len) {
    if (d[i] != 0xFF) { i++; continue; }
    while (i < len && d[i] == 0xFF) i++;   // wypełnienie 0xFF przed markerem
    if (i >= len) return false;
    uint8_t marker = d[i++];
    // Markery bez pola długości (SOI, EOI, TEM, RSTn).
    if (marker == 0xD8 || marker == 0xD9 || marker == 0x01 ||
        (marker >= 0xD0 && marker <= 0xD7)) continue;
    if (i + 2 > len) return false;
    uint16_t segLen = (d[i] << 8) | d[i + 1];
    // SOF0..SOF15 z wyjątkiem DHT(0xC4)/JPG(0xC8)/DAC(0xCC):
    // segment = len(2) + precyzja(1) + wysokość(2) + szerokość(2) + ...
    if ((marker >= 0xC0 && marker <= 0xCF) &&
        marker != 0xC4 && marker != 0xC8 && marker != 0xCC) {
      if (i + 7 > len) return false;
      h = (d[i + 3] << 8) | d[i + 4];
      w = (d[i + 5] << 8) | d[i + 6];
      return w > 0 && h > 0;
    }
    i += segLen;   // segLen zawiera już 2 bajty długości
  }
  return false;
}
#endif

bool CameraManager::begin() {
  enabled_ = config.camEnabled();
  intervalMin_ = config.camIntervalMin();

#if !STACJA_HAS_CAMERA
  lastError_ = "brak wsparcia sprzętowego (ten wariant nie obsługuje kamery)";
  LOG_I("Kamera: niedostępna (STACJA_HAS_CAMERA=0)");
  return false;
#else
  if (!enabled_) {
    lastError_ = "";
    LOG_I("Kamera: wyłączona w ustawieniach");
    return true;
  }

  String remoteUrl = config.camRemoteUrl();
  remoteUrl.trim();
  if (remoteUrl.length() > 0) {
    // Tryb zewnętrzny: zdjęcia robi i wysyła zewnętrzna kamera (ESP32-CAM)
    // przez /api/camera/upload. Lokalny sterownik nie jest uruchamiany, więc
    // piny magistrali równoległej nie są wymagane.
    lastError_ = "";
    // Pierwsza pogoda z kamery ma pojawić się szybko po starcie, a nie po
    // pełnym interwale (inaczej po każdym restarcie kafelek długo jest pusty).
    lastCaptureMs_ = millis() - (uint32_t)intervalMin_ * 60000UL + 45000UL;
    LOG_I("Kamera: tryb zewnętrzny (%s)", remoteUrl.c_str());
    return true;
  }

  return start();
#endif
}

bool CameraManager::start() {
#if !STACJA_HAS_CAMERA
  lastError_ = "brak wsparcia sprzętowego (ten wariant nie obsługuje kamery)";
  return false;
#else
  if (present_) return true;
  if (!configured()) {
    lastError_ = "nie skonfigurowano pinów kamery (Ustawienia -> Kamera)";
    LOG_W("Kamera: brak pinów - konfiguracja w sekcji \"pins\"");
    return false;
  }
  if (!psramFound()) {
    lastError_ = "brak PSRAM (wymagana dla buforów kamery)";
    LOG_E("Kamera: brak PSRAM");
    return false;
  }

  camera_config_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.pin_pwdn  = config.pinOverride("cam_pwdn", -1);
  cfg.pin_reset = config.pinOverride("cam_reset", -1);
  cfg.pin_xclk  = config.pinOverride("cam_xclk", -1);
  cfg.pin_sccb_sda = config.pinOverride("cam_sda", -1);
  cfg.pin_sccb_scl = config.pinOverride("cam_scl", -1);
  cfg.pin_d7 = config.pinOverride("cam_d7", -1);
  cfg.pin_d6 = config.pinOverride("cam_d6", -1);
  cfg.pin_d5 = config.pinOverride("cam_d5", -1);
  cfg.pin_d4 = config.pinOverride("cam_d4", -1);
  cfg.pin_d3 = config.pinOverride("cam_d3", -1);
  cfg.pin_d2 = config.pinOverride("cam_d2", -1);
  cfg.pin_d1 = config.pinOverride("cam_d1", -1);
  cfg.pin_d0 = config.pinOverride("cam_d0", -1);
  cfg.pin_vsync = config.pinOverride("cam_vsync", -1);
  cfg.pin_href  = config.pinOverride("cam_href", -1);
  cfg.pin_pclk  = config.pinOverride("cam_pclk", -1);

  // 16 MHz włącza EDMA na ESP32-S3 (szybszy, stabilniejszy transfer).
  cfg.xclk_freq_hz = 16000000;
  cfg.ledc_timer   = LEDC_TIMER_0;
  cfg.ledc_channel = LEDC_CHANNEL_0;
  cfg.pixel_format = PIXFORMAT_JPEG;
  cfg.frame_size   = (framesize_t)config.camResolution();
  cfg.jpeg_quality = config.camQuality();
  cfg.fb_count     = 2;
  cfg.fb_location  = CAMERA_FB_IN_PSRAM;
  cfg.grab_mode    = CAMERA_GRAB_LATEST;
  cfg.sccb_i2c_port = 0;

  esp_err_t err = esp_camera_init(&cfg);
  if (err != ESP_OK) {
    lastError_ = cameraErrName(err);
    LOG_E("Kamera: inicjalizacja nieudana - %s", lastError_.c_str());
    return false;
  }

  present_ = true;
  modelName_ = "";
  sensor_t* s = esp_camera_sensor_get();
  if (s) {
    camera_sensor_info_t* info = esp_camera_sensor_get_info(&s->id);
    if (info && info->name) modelName_ = info->name;
  }
  lastError_ = "";
  // Pierwsze zdjęcie ~45 s po starcie, potem co pełny interwał.
  lastCaptureMs_ = millis() - (uint32_t)intervalMin_ * 60000UL + 45000UL;
  LOG_I("Kamera: gotowa (%s), rozdzielczość %d, jakość %d",
        modelName_.length() ? modelName_.c_str() : "nieznana",
        (int)config.camResolution(), (int)config.camQuality());
  return true;
#endif
}

void CameraManager::stop() {
#if STACJA_HAS_CAMERA
  if (present_) {
    esp_camera_deinit();
    present_ = false;
    LOG_I("Kamera: sterownik zwolniony");
  }
#endif
}

void CameraManager::applyConfig() {
  enabled_ = config.camEnabled();
  intervalMin_ = config.camIntervalMin();

#if STACJA_HAS_CAMERA
  String url = config.camRemoteUrl();
  url.trim();
  if (url.length() > 0) {
    // Tryb zewnętrzny: master nie dotyka lokalnego sterownika kamery.
    if (present_) stop();
    return;
  }
  if (enabled_ && !present_) {
    start();
  } else if (enabled_ && present_) {
    // Zmiana rozdzielczości / jakości działa od razu, bez re-init.
    sensor_t* s = esp_camera_sensor_get();
    if (s) {
      if (s->set_framesize) s->set_framesize(s, (framesize_t)config.camResolution());
      if (s->set_quality)   s->set_quality(s, (int)config.camQuality());
    }
  } else if (!enabled_ && present_) {
    stop();
  }
#endif
}

void CameraManager::loop() {
#if STACJA_HAS_CAMERA
  // Zdjęcie zlecone z www ma pierwszeństwo - działa też przy timelapsie
  // ustawionym na 0 (tylko zdjęcia ręczne). Wykonujemy je tutaj, a nie w
  // wątku serwera www: pobranie zdjęcia z kamery trwa kilka sekund.
  if (pendingCapture_) {
    pendingCapture_ = false;
    if (!enabled_) {
      lastError_ = "kamera jest wyłączona w ustawieniach";
      return;
    }
    String url = config.camRemoteUrl();
    url.trim();
    bool ok = (url.length() > 0) ? triggerRemote() : capture();
    if (!ok) LOG_W("Kamera: zdjęcie na żądanie nieudane - %s", lastError_.c_str());
    return;
  }

  if (!enabled_) return;
  if (intervalMin_ == 0) return;   // tylko zdjęcia ręczne

  String url = config.camRemoteUrl();
  url.trim();
  bool remote = (url.length() > 0);
  if (!remote && !present_) return;   // kamera lokalna nie działa

  uint32_t now = millis();
  if (now - lastCaptureMs_ >= (uint32_t)intervalMin_ * 60000UL) {
    bool ok = remote ? triggerRemote() : capture();
    // Nie zapętlamy się co tick - kolejna próba za pełny interwał.
    lastCaptureMs_ = now;
    if (!ok) LOG_W("Kamera: zdjęcie timelapse nieudane - %s", lastError_.c_str());
  }
#endif
}

bool CameraManager::requestCapture() {
  if (!enabled_) {
    lastError_ = "kamera jest wyłączona w ustawieniach";
    return false;
  }
  pendingCapture_ = true;
  return true;
}

bool CameraManager::capture() {
#if !STACJA_HAS_CAMERA
  lastError_ = "brak wsparcia sprzętowego (ten wariant nie obsługuje kamery)";
  return false;
#else
  if (!present_) {
    lastError_ = "kamera nie jest zainicjalizowana";
    return false;
  }
  if (capturing_) {
    lastError_ = "trwa już wykonywanie zdjęcia";
    return false;
  }

  capturing_ = true;
  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) {
    capturing_ = false;
    lastError_ = "timeout pobrania klatki (sprawdź piny XCLK/PCLK)";
    LOG_E("Kamera: esp_camera_fb_get() = NULL");
    return false;
  }

  String path;
  bool ok = saveAndAnalyze(fb->buf, fb->len, path);
  esp_camera_fb_return(fb);
  capturing_ = false;
  return ok;
#endif
}

// Odbiór zdjęcia wysłanego przez zewnętrzną kamerę (ESP32-CAM) przez
// HTTP POST na /api/camera/upload. Funkcja zapisuje JPEG na SD, analizuje
// go i pilnuje limitów - dokładnie tak samo jak zdjęcie lokalne.
bool CameraManager::ingestJpeg(const uint8_t* data, size_t len) {
#if !STACJA_HAS_CAMERA
  lastError_ = "brak wsparcia sprzętowego (ten wariant nie obsługuje kamery)";
  return false;
#else
  if (!data || len < 4) {
    lastError_ = "puste dane zdjęcia";
    return false;
  }
  if (data[0] != 0xFF || data[1] != 0xD8) {
    lastError_ = "to nie jest plik JPEG (brak nagłówka SOI)";
    return false;
  }
  if (capturing_) {
    lastError_ = "trwa już zapis innego zdjęcia";
    return false;
  }

  capturing_ = true;
  String path;
  bool ok = saveAndAnalyze(data, len, path);
  capturing_ = false;
  return ok;
#endif
}

// Zdjęcie z zewnętrznej kamery. Adres w config cam_remote_url wskazuje
// endpoint kamery (np. http://192.168.1.142/capture). Master najpierw próbuje
// pobrać gotowy JPEG i sam go analizuje oraz zapisuje - dzięki temu działa
// zwykła kamera ESP32-CAM serwująca /capture, bez żadnego programu po jej
// stronie. Gdy odpowiedź nie jest zdjęciem (adres tylko wyzwala ujęcie),
// kończymy połączenie od razu, a zdjęcie wraca później przez
// /api/camera/upload - jak dotychczas.
bool CameraManager::triggerRemote() {
#if !STACJA_HAS_CAMERA
  return false;
#else
  const int r = fetchRemoteJpeg();
  if (r > 0) return true;         // JPEG odebrany, zapisany i przeanalizowany
  if (r == 0) {                   // tryb wyzwalania - zdjęcie wróci POST-em
    lastError_ = "";
    LOG_I("Kamera: wyzwolono ujęcie (zdjęcie wróci przez /api/camera/upload)");
    return true;
  }
  return false;                   // fetchRemoteJpeg wpisał już powód do lastError_
#endif
}

// Pobranie gotowego JPEG-a z adresu kamery. Zwraca 1 = odebrano zdjęcie,
// 0 = połączenie i odpowiedź OK, ale treść nie jest zdjęciem (kamera tylko
// wyzwala ujęcie), -1 = błąd (opis w lastError_).
int CameraManager::fetchRemoteJpeg() {
#if !STACJA_HAS_CAMERA
  return -1;
#else
  String url = config.camRemoteUrl();
  url.trim();
  if (url.length() == 0) { lastError_ = "brak adresu kamery zewnętrznej"; return -1; }
  if (!url.startsWith("http://")) {
    lastError_ = "adres kamery musi zaczynać się od http://";
    LOG_W("Kamera: pobranie - zły adres %s", url.c_str());
    return -1;
  }

  // Parsowanie "http://host[:port]/sciezka".
  String rest = url.substring(7);
  String host = rest;
  String path = "/";
  int slash = rest.indexOf('/');
  if (slash >= 0) { path = rest.substring(slash); host = rest.substring(0, slash); }

  uint16_t port = 80;
  int colon = host.indexOf(':');
  if (colon >= 0) {
    port = (uint16_t)host.substring(colon + 1).toInt();
    host = host.substring(0, colon);
  }
  if (host.length() == 0) { lastError_ = "nieprawidłowy adres kamery"; return -1; }

  WiFiClient client;
  IPAddress ip;
  bool connected = ip.fromString(host)
      ? client.connect(ip, port, 1200)
      : client.connect(host.c_str(), port, 1200);
  if (!connected) {
    lastError_ = "nie udało się połączyć z kamerą (" + host + ")";
    LOG_W("Kamera: pobranie - brak połączenia z %s", host.c_str());
    return -1;
  }
  client.setTimeout(600);   // jedno czytanie czeka najwyżej 0,6 s

  client.print("GET " + path + " HTTP/1.1\r\n");
  client.print("Host: " + host + "\r\n");
  client.print("User-Agent: stacja-pogody\r\n");
  client.print("Accept: image/jpeg\r\n");
  client.print("Connection: close\r\n\r\n");
  client.flush();

  // Odpowiedź czytamy sami (a nie przez HTTPClient), bo timeout każdej
  // operacji musi być ograniczony. Całe pobranie (połączenie + nagłówki +
  // zdjęcie) musi zmieścić się w limicie watchdoga zadania (5 s), dlatego
  // connect 1,2 s, odczyt 0,6 s na operację i twardy deadline 2,5 s. Kamera,
  // która przyjmuje połączenie i milczy (zawieszony ESP32-CAM), odcina się
  // wtedy po ~2 s, zamiast blokować pętlę do resetu stacji.
  const unsigned long deadline = millis() + 2500UL;
  String status = client.readStringUntil('\n');
  int code = 0;
  if (status.length() == 0 || sscanf(status.c_str(), "HTTP/%*d.%*d %d", &code) != 1) {
    client.stop();
    lastError_ = "kamera nie odpowiedziała po HTTP";
    LOG_W("Kamera: pobranie - zła odpowiedź: '%s'", status.c_str());
    return -1;
  }

  long contentLen = -1;
  while (client.connected() || client.available()) {
    String line = client.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) break;                 // koniec nagłówków
    String low = line; low.toLowerCase();
    if (low.startsWith("content-length:")) contentLen = low.substring(15).toInt();
    if (millis() > deadline) break;
  }

  if (code != 200) {
    client.stop();
    lastError_ = "kamera zwróciła HTTP " + String(code);
    LOG_W("Kamera: pobranie - HTTP %d", code);
    return -1;
  }

  // Znacznik JPEG (FF D8) - jeśli treść nim nie jest, kamera tylko wyzwala
  // ujęcie i wysyła zdjęcie osobno (POST /api/camera/upload), więc zamykamy
  // połączenie bez odbierania całej odpowiedzi.
  uint8_t head[2] = {0, 0};
  int got = client.readBytes(head, 2);
  if (got < 2 || head[0] != 0xFF || head[1] != 0xD8) {
    client.stop();
    return 0;
  }

  if (contentLen > CAM_REMOTE_MAX_BYTES) {
    client.stop();
    lastError_ = "zdjęcie z kamery za duże (" + String(contentLen) + " B)";
    LOG_W("Kamera: %s", lastError_.c_str());
    return -1;
  }

  // Bufor w PSRAM (zdjęcie 1600x1200 potrafi mieć ~250 kB).
  size_t cap = (contentLen > 2) ? (size_t)contentLen : 512UL * 1024UL;
  if (cap > CAM_REMOTE_MAX_BYTES) cap = CAM_REMOTE_MAX_BYTES;
  uint8_t* buf = (uint8_t*)heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
  bool inPsram = (buf != nullptr);
  if (!buf) buf = (uint8_t*)malloc(cap);
  if (!buf) {
    client.stop();
    lastError_ = "brak pamięci na zdjęcie (" + String((unsigned)cap) + " B)";
    LOG_E("Kamera: %s", lastError_.c_str());
    return -1;
  }

  size_t len = 2;
  buf[0] = head[0]; buf[1] = head[1];
  while (len < cap && millis() < deadline) {
    if (!client.available()) {
      if (!client.connected()) break;
      delay(2);
      continue;
    }
    int n = client.read(buf + len, cap - len);
    if (n <= 0) break;
    len += (size_t)n;
  }
  client.stop();

  // Zapisujemy wyłącznie kompletne zdjęcie. Gdy odczyt urywa się na deadline
  // (albo kamera zerwie połączenie w połowie), na karcie zostałby nieotwieralny
  // plik, więc lepiej zgłosić błąd i spróbować przy następnym zdjęciu.
  const bool complete = (contentLen > 2)
      ? (len == (size_t)contentLen)
      : (len > 4 && buf[len - 2] == 0xFF && buf[len - 1] == 0xD9);
  bool ok = (len > 64) && complete;
  if (ok) {
    // Zapis + analiza (tą samą drogą, co zdjęcie lokalne i upload z modułu).
    // Flaga blokuje równoczesny zapis, gdy kamera w tej samej chwili przyśle
    // zdjęcie POST-em na /api/camera/upload (to samo ujęcie).
    String path2;
    capturing_ = true;
    ok = saveAndAnalyze(buf, len, path2);
    capturing_ = false;
    if (ok) LOG_I("Kamera: pobrano z kamery %u B", (unsigned)len);
  } else if (len > 64) {
    lastError_ = "odebrano niepełne zdjęcie z kamery";
    LOG_W("Kamera: pobranie - niepełne zdjęcie (%u z %ld B)",
          (unsigned)len, contentLen);
  } else {
    lastError_ = "nie udało się odebrać zdjęcia z kamery";
    LOG_W("Kamera: pobranie - odebrano tylko %u B", (unsigned)len);
  }

  if (inPsram) heap_caps_free(buf);
  else         free(buf);
  return ok ? 1 : -1;
#endif
}

// Wspólny zapis zdjęcia JPEG na karcie SD + analiza + limity. Ścieżka:
// /photos/RRRR-MM/RRRR-MM-DD_HHMMSS.jpg (katalogi miesięczne, żeby nie
// przekroczyć limitu plików w jednym katalogu FAT32 przy długim timelapse).
bool CameraManager::saveAndAnalyze(const uint8_t* jpg, size_t len, String& outPath) {
#if !STACJA_HAS_CAMERA
  return false;
#else
  if (!sdCard.mounted()) {
    lastError_ = "brak karty SD (zdjęcia zapisywane są na karcie)";
    return false;
  }

  // Pominięcie duplikatu: ten sam JPEG, który już zapisaliśmy chwilę temu.
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < len; i++) { h ^= jpg[i]; h *= 16777619u; }
  unsigned long nowMs = millis();
  if (h == lastJpegHash_ && (uint32_t)len == lastJpegLen_ &&
      (nowMs - lastJpegMs_) < CAM_DUP_WINDOW_MS) {
    LOG_I("Kamera: pominięto duplikat zdjęcia (%s)", lastFile_.c_str());
    outPath = lastFile_;
    lastError_ = "";
    return true;
  }
  lastJpegHash_ = h;
  lastJpegLen_ = (uint32_t)len;
  lastJpegMs_ = nowMs;

  time_t now = time(nullptr);
  struct tm t;
  localtime_r(&now, &t);
  char dir[32];
  snprintf(dir, sizeof(dir), "/photos/%04d-%02d", t.tm_year + 1900, t.tm_mon + 1);
  char path[64];
  snprintf(path, sizeof(path), "/photos/%04d-%02d/%04d-%02d-%02d_%02d%02d%02d.jpg",
           t.tm_year + 1900, t.tm_mon + 1,
           t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
           t.tm_hour, t.tm_min, t.tm_sec);
  sdCard.mkdirs(dir);

  if (!sdCard.writeBlock(path, jpg, len)) {
    lastError_ = "nie udało się zapisać zdjęcia na karcie SD";
    LOG_E("Kamera: zapis %s nieudany", path);
    return false;
  }

  lastFile_ = path;
  lastCaptureMs_ = millis();
  lastError_ = "";
  LOG_I("Kamera: zapisano %s (%u B)", path, (unsigned)len);

  if (!analyzeJpeg(jpg, len)) {
    LOG_W("Kamera: analiza zdjęcia nieudana");
  } else {
    publishAnalysis();
  }
  writeAnalysisJson(path);
  enforceQuota();
  outPath = path;
  return true;
#endif
}

// Dekoduje miniaturę JPEG (RGB565, skala 1/8) i liczy analizę. Rozmiar
// miniatury = szerokość/8 x wysokość/8 (tjpgd), z marginesem bufora na
// ewentualne zaokrąglenia.
bool CameraManager::analyzeJpeg(const uint8_t* jpg, size_t len) {
#if !STACJA_HAS_CAMERA
  (void)jpg; (void)len;
  return false;
#else
  CamAnalysis none;
  lastAnalysis_ = none;

  uint16_t w = 0, h = 0;
  if (!parseJpegSize(jpg, len, w, h)) return false;

  uint16_t mw = w >> 3; if (mw < 1) mw = 1;
  uint16_t mh = h >> 3; if (mh < 1) mh = 1;
  size_t outLen = (size_t)(mw + 2) * (mh + 2) * 2;
  if (outLen > 1536UL * 1024UL) {
    LOG_W("Kamera: zdjęcie zbyt duże do analizy (%ux%u)", w, h);
    return false;
  }

  uint8_t* out = (uint8_t*)heap_caps_malloc(outLen, MALLOC_CAP_SPIRAM);
  bool inPsram = (out != nullptr);
  if (!out) out = (uint8_t*)malloc(outLen);
  if (!out) {
    LOG_E("Kamera: brak pamięci na miniaturę (%u B)", (unsigned)outLen);
    return false;
  }

  bool ok = jpg2rgb565(jpg, len, out, JPG_SCALE_8X);
  if (ok) {
    analyzeRgb565(out, mw, mh);
    lastAnalysis_.w = w;   // w analizie trzymamy oryginalny rozmiar zdjęcia
    lastAnalysis_.h = h;
  }
  if (inPsram) heap_caps_free(out);
  else         free(out);
  return ok;
#endif
}

// Analiza miniatury RGB565: jasność, luksy, średnie RGB, ruch (różnica
// siatki luminancji vs poprzednia klatka), zachmurzenie i faza dnia/nocy.
bool CameraManager::analyzeRgb565(const uint8_t* buf, uint16_t w, uint16_t h) {
  CamAnalysis a;
  if (!buf || w < 4 || h < 4) { lastAnalysis_ = a; return false; }

  a.w = w; a.h = h; a.ts = time(nullptr);

  const int gw = ANA_GRID_W, gh = ANA_GRID_H;
  int stepX = w / gw; if (stepX < 1) stepX = 1;
  int stepY = h / gh; if (stepY < 1) stepY = 1;
  const int n = gw * gh;

  // Region nieba to górne 45% kadru PO obrocie o cam_rotate. Przy obrazie
  // obróconym o 90/270 stopni góra kadru to odpowiednio lewa/prawa krawędź
  // zdjęcia z czujnika, więc zakres liczymy raz, a nie w każdej próbce.
  const int rot = config.camRotate();
  const int limitW = gw * 45 / 100;
  const int limitH = gh * 45 / 100;
  auto inSkyRegion = [&](int gx, int gy) -> bool {
    switch (rot) {
      case 90:  return gx < limitW;          // obrót w prawo: góra = lewa krawędź
      case 180: return gy >= gh - limitH;    // góra = dolna krawędź
      case 270: return gx >= gw - limitW;    // góra = prawa krawędź
      default:  return gy < limitH;          // bez obrotu: góra kadru
    }
  };

  uint8_t* cur = (uint8_t*)malloc(n);
  if (!cur) { lastAnalysis_ = a; return false; }

  const uint16_t* px = (const uint16_t*)buf;
  float sumLum = 0, sumR = 0, sumG = 0, sumB = 0;
  float motion = 0;
  uint32_t cloud = 0, sky = 0, whiteCnt = 0, grayCnt = 0;

  for (int gy = 0; gy < gh; gy++) {
    int y = gy * stepY; if (y >= (int)h) y = h - 1;
    for (int gx = 0; gx < gw; gx++) {
      int x = gx * stepX; if (x >= (int)w) x = w - 1;
      uint16_t p = px[y * w + x];
      int r5 = (p >> 11) & 0x1F, g6 = (p >> 5) & 0x3F, b5 = p & 0x1F;
      int r = (r5 << 3) | (r5 >> 2);
      int g = (g6 << 2) | (g6 >> 4);
      int b = (b5 << 3) | (b5 >> 2);
      int lum = (299 * r + 587 * g + 114 * b) / 1000;

      int idx = gy * gw + gx;
      cur[idx] = (uint8_t)lum;
      sumLum += lum; sumR += r; sumG += g; sumB += b;

      int mx = r > g ? r : g; if (b > mx) mx = b;
      int mn = r < g ? r : g; if (b < mn) mn = b;
      int sat = mx - mn;
      // Biel śniegu i gęstych chmur jest neutralna (r ≈ g ≈ b). Sam warunek
      // "jasne i mało nasycone" łapał też blade, błękitne niebo (np. RGB
      // 157,172,201 ma nasycenie 44, czyli tuż pod progiem), więc przy słońcu
      // wychodził z tego śnieg. Dlatego biel nie może być wyraźnie niebieska.
      if (lum > 160 && sat < 45 && (b - r) <= 18) whiteCnt++;
      if (sat < 30) grayCnt++;                 // niskie nasycenie (mgła / deszcz)

      if (havePrevLum_) {
        int d = (int)cur[idx] - (int)prevLum_[idx];
        if (d < 0) d = -d;
        motion += d;
      }

      // Chmury / niebo liczymy w górnych 45% kadru (kamera najczęściej
      // patrzy w niebo; przy kadrze poziomym wynik jest przybliżony).
      if (inSkyRegion(gx, gy)) {
        if (lum > 170 && r > (b * 3) / 4 && g > 80) cloud++;
        else if (b > (r * 13) / 10 && lum < 180) sky++;
      }
    }
  }

  a.brightness = sumLum / n;
  a.meanR = sumR / n; a.meanG = sumG / n; a.meanB = sumB / n;
  float bn = a.brightness / 255.0f;
  a.lux = bn * bn * 120000.0f;
  a.motion = havePrevLum_ ? (motion / n) * 100.0f / 255.0f : 0;
  if (cloud + sky > 0) a.cloudCover = (float)cloud * 100.0f / (float)(cloud + sky);
  else a.cloudCover = -1;
  if (a.brightness >= 70)      a.phase = "day";
  else if (a.brightness >= 25) a.phase = "dawn/dusk";
  else                         a.phase = "night";

  // Kontrast = odchylenie standardowe luminancji siatki (mgła go mocno obniża).
  float contrast = 0;
  for (int i = 0; i < n; i++) {
    float d = (float)cur[i] - a.brightness;
    contrast += d * d;
  }
  contrast = sqrtf(contrast / (float)n);

  float whitePct = (float)whiteCnt * 100.0f / (float)n;
  float grayPct  = (float)grayCnt * 100.0f / (float)n;

  // Orientacyjna klasyfikacja pogody z pojedynczego zdjęcia. To heurystyka
  // (jasność + nasycenie + kontrast + zachmurzenie), nie pomiar - traktować
  // jako wskaźnik do porównywania kolejnych zdjęć, nie jako dokładny opad.
  //
  // Śnieg wymaga dodatkowo mrozu: biały kadr przy +25 stopniach to jasne, grube
  // chmury albo mgła, a nie śnieg. Bierzemy najniższą temperaturę z czujników
  // zewnętrznych (płytka stacji stoi w obudowie i potrafi pokazywać ciepło, gdy
  // na dworze jest mróz), a brak jakiegokolwiek pomiaru nie blokuje wykrycia.
  const float ambient = sensors.coldestOutdoorTemp();
  const bool snowPossible = isnan(ambient) || ambient <= CAM_SNOW_MAX_TEMP;

  a.snowPct = a.rainPct = a.fogPct = -1;
  if (a.brightness < 40) {
    a.weather = "night";
  } else if (whitePct >= 30.0f && a.brightness >= 110.0f && snowPossible) {
    a.weather = "snow";
    a.snowPct = constrain(whitePct * 1.6f - 16.0f, 10.0f, 100.0f);
  } else if (contrast < 26.0f && a.brightness >= 55.0f &&
             a.brightness <= 160.0f && grayPct >= 55.0f) {
    a.weather = "fog";
    a.fogPct = constrain((grayPct - 50.0f) * 3.0f, 5.0f, 100.0f);
  } else if (a.cloudCover >= 40.0f && grayPct >= 45.0f && a.brightness < 125.0f) {
    a.weather = "rain";
    a.rainPct = constrain((a.cloudCover - 35.0f) * 1.8f, 5.0f, 100.0f);
  } else if (whitePct >= 30.0f && a.brightness >= 110.0f) {
    // Ta sama biała scena, ale bez mrozu - jasne, zlane chmury.
    a.weather = "cloudy";
    if (!isnan(ambient) && ambient > CAM_SNOW_MAX_TEMP) {
      static uint32_t lastSnowReject = 0;
      uint32_t now = millis();
      if (now - lastSnowReject > 60000UL) {
        lastSnowReject = now;
        LOG_I("Kamera: biały kadr, ale %.1f °C - chmury, nie śnieg", ambient);
      }
    }
  } else if (a.cloudCover > 55.0f) {
    a.weather = "cloudy";
  } else {
    a.weather = "clear";
  }

  a.valid = true;

  memcpy(prevLum_, cur, n);
  havePrevLum_ = true;
  free(cur);
  lastAnalysis_ = a;
  return true;
}

// Wystawia wynik analizy zdjęcia jako kanały stacji. Dzięki temu informacja
// "co widać na niebie" (pogoda, faza dnia, zachmurzenie, intensywność deszczu,
// śniegu i mgły, natężenie światła) trafia na pulpit, do dziennika CSV, do
// alertów i do Home Assistant razem z pozostałymi pomiarami - a nie tylko do
// zakładki Kamera. Kod pogody: 0 = noc, 1 = bezchmurnie, 2 = pochmurno,
// 3 = deszcz, 4 = śnieg, 5 = mgła. Faza dnia: 0 = noc, 1 = świt/zmierzch,
// 2 = dzień. Kanały bez danych w danym zdjęciu są czyszczone, żeby nie
// pokazywały starej wartości (np. deszczu sprzed godziny).
void CameraManager::publishAnalysis() {
  const CamAnalysis& a = lastAnalysis_;
  if (!a.valid) return;

  int code = -1;
  if      (a.weather == "night")  code = 0;
  else if (a.weather == "clear")  code = 1;
  else if (a.weather == "cloudy") code = 2;
  else if (a.weather == "rain")   code = 3;
  else if (a.weather == "snow")   code = 4;
  else if (a.weather == "fog")    code = 5;
  if (code >= 0) sensors.publishExtra("cam_weather", (float)code);
  else           sensors.clearExtra("cam_weather");

  if (a.phase == "day")            sensors.publishExtra("cam_phase", 2);
  else if (a.phase == "dawn/dusk") sensors.publishExtra("cam_phase", 1);
  else if (a.phase == "night")     sensors.publishExtra("cam_phase", 0);
  else                             sensors.clearExtra("cam_phase");

  if (a.lux >= 0)        sensors.publishExtra("cam_lux",   a.lux);
  // Nocne zdjęcie jest czarne niezależnie od zachmurzenia, więc szacunek
  // zachmurzenia nie ma wtedy żadnej wartości - czyścimy kanał.
  if (a.cloudCover >= 0 && a.phase != "night") sensors.publishExtra("cam_cloud", a.cloudCover);
  else                                        sensors.clearExtra("cam_cloud");

  if (a.rainPct >= 0) sensors.publishExtra("cam_rain", a.rainPct);
  else                sensors.clearExtra("cam_rain");
  if (a.snowPct >= 0) sensors.publishExtra("cam_snow", a.snowPct);
  else                sensors.clearExtra("cam_snow");
  if (a.fogPct >= 0)  sensors.publishExtra("cam_fog", a.fogPct);
  else                sensors.clearExtra("cam_fog");
}

// Zapisuje obok zdjęcia plik .json z wynikiem analizy (do podglądu na www).
void CameraManager::writeAnalysisJson(const String& jpgPath) {
  String meta = jpgPath;
  meta.replace(".jpg", ".json");
  JsonDocument d;
  const CamAnalysis& a = lastAnalysis_;
  d["valid"]       = a.valid;
  d["w"]           = a.w;
  d["h"]           = a.h;
  d["ts"]          = (uint32_t)a.ts;
  d["brightness"]  = a.brightness;
  d["lux"]         = a.lux;
  d["mean_r"]      = a.meanR;
  d["mean_g"]      = a.meanG;
  d["mean_b"]      = a.meanB;
  d["motion_pct"]  = a.motion;
  d["cloud_pct"]   = a.cloudCover;
  d["phase"]       = a.phase;
  d["weather"]     = a.weather;
  d["snow_pct"]    = a.snowPct;
  d["rain_pct"]    = a.rainPct;
  d["fog_pct"]     = a.fogPct;

  String s;
  serializeJson(d, s);
  if (!sdCard.writeBlock(meta, (const uint8_t*)s.c_str(), s.length())) {
    LOG_W("Kamera: zapis analizy %s nieudany", meta.c_str());
  }
}

// Zbiera rekurencyjnie ścieżki plików .jpg spod /photos (zdjęcia są w
// podkatalogach /photos/YYYY-MM, więc listDir jednego poziomu nie wystarczy).
static void collectPhotoFiles(const String& dir, std::vector<SdFileInfo>& out) {
  // Katalogów miesięcznych jest kilka, ale w każdym mogą leżeć setki plików,
  // a wpis katalogu na SPI kosztuje kilkadziesiąt milisekund. Bierzemy więc
  // tylko ostatnie wpisy każdego katalogu - najstarszych zdjęć i tak nie ma
  // po co liczyć, bo kasujemy zawsze od najstarszych.
  std::vector<SdFileInfo> entries;
  if (!sdCard.listDir(dir, entries, 400, true, nullptr)) return;
  for (const auto& e : entries) {
    if (e.isDir) collectPhotoFiles(e.path, out);
    else if (e.path.endsWith(".jpg")) out.push_back(e);
  }
}

// Kasowanie najstarszych zdjęć (razem z plikami .json analizy), dopóki
// liczba plików i łączny rozmiar mieszczą się w limitach config.
void CameraManager::enforceQuota() {
  uint16_t maxPhotos = config.camMaxPhotos();
  uint16_t maxMb = config.camMaxMb();
  if (maxPhotos == 0 && maxMb == 0) return;

  const uint64_t maxBytes = (uint64_t)maxMb * 1024ULL * 1024ULL;
  // Szybka ścieżka: skanowanie katalogu zdjęć jest drogie, a gdy cała karta
  // zajmuje mniej niż limit rozmiaru zdjęć, limit na pewno nie został
  // przekroczony. Bez tego każde zdjęcie z timelapse blokowało pętlę główną na
  // dziesiątki sekund (setki wpisów katalogu po ~37 ms).
  if (maxPhotos == 0 && maxMb > 0 && sdCard.usedBytes() < maxBytes) return;

  std::vector<SdFileInfo> files;
  collectPhotoFiles("/photos", files);
  if (files.empty()) return;

  std::sort(files.begin(), files.end(),
            [](const SdFileInfo& a, const SdFileInfo& b) { return a.mtime < b.mtime; });

  uint64_t totalBytes = 0;
  uint32_t count = 0;
  for (const auto& f : files) { totalBytes += f.size; count++; }

  for (const auto& f : files) {
    bool overCount = (maxPhotos > 0 && count > maxPhotos);
    bool overBytes = (maxMb > 0 && totalBytes > maxBytes);
    if (!overCount && !overBytes) break;
    deletePhotoWithMeta(f.path);
    totalBytes -= f.size;
    count--;
  }
}

// Usuwa zdjęcie oraz ewentualny plik analizy .json obok niego.
void CameraManager::deletePhotoWithMeta(const String& jpgPath) {
  sdCard.deleteFile(jpgPath);
  String meta = jpgPath;
  meta.replace(".jpg", ".json");
  sdCard.deleteFile(meta);
  LOG_I("Kamera: limit - usunięto %s", jpgPath.c_str());
}

String CameraManager::toJson() const {
  JsonDocument d;
  String remoteUrl = config.camRemoteUrl();
  remoteUrl.trim();
  const char* mode = "off";
  if (enabled_) mode = (remoteUrl.length() > 0) ? "external" : "local";

  d["supported"]  = (STACJA_HAS_CAMERA != 0);
  d["enabled"]    = enabled_;
  d["present"]    = present_;
  d["mode"]       = mode;
  d["configured"] = (remoteUrl.length() > 0) ? true : configured();
  d["interval_min"] = intervalMin_;
  d["resolution"] = config.camResolution();
  d["quality"]    = config.camQuality();
  d["model"]      = modelName_;
  d["last_file"]  = lastFile_;
  d["last_capture_ms"] = (uint32_t)lastCaptureMs_;
  d["error"]      = lastError_;
  d["cam_max_photos"] = config.camMaxPhotos();
  d["cam_max_mb"]     = config.camMaxMb();
  d["cam_remote_url"] = config.camRemoteUrl();
  d["cam_rotate"]     = config.camRotate();

  // Wynik analizy ostatniego zdjęcia (jasność, luksy, RGB, ruch, chmury).
  JsonObject ana = d["last_analysis"].to<JsonObject>();
  const CamAnalysis& a = lastAnalysis_;
  ana["valid"]      = a.valid;
  ana["w"]          = a.w;
  ana["h"]          = a.h;
  ana["ts"]         = (uint32_t)a.ts;
  ana["brightness"] = a.brightness;
  ana["lux"]        = a.lux;
  ana["mean_r"]     = a.meanR;
  ana["mean_g"]     = a.meanG;
  ana["mean_b"]     = a.meanB;
  ana["motion_pct"] = a.motion;
  ana["cloud_pct"]  = a.cloudCover;
  ana["phase"]      = a.phase;
  ana["weather"]    = a.weather;
  ana["snow_pct"]   = a.snowPct;
  ana["rain_pct"]   = a.rainPct;
  ana["fog_pct"]    = a.fogPct;

  // Piny kamery (sekcja "pins") - do podglądu i edycji w zakładce Kamera.
  JsonObject pins = d["pins"].to<JsonObject>();
  for (const char* k : CAM_PIN_KEYS) pins[k] = config.pinOverride(k, -1);

  String s;
  serializeJson(d, s);
  return s;
}
