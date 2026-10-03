/* =============================================================================
 * Stacja Pogody - stacja meteorologiczna na ESP32-S3 WROOM-1 (N16R8)
 * Copyright (c) 2026 kruzio1985 - https://github.com/kruzio1985
 * Licencja: PolyForm Noncommercial License 1.0.0 (plik LICENSE)
 * Użytek niekomercyjny. Kontakt: kruzio1985@users.noreply.github.com
 * =============================================================================
 */
#pragma once
#include <Arduino.h>
#include "config.h"

// =============================================================
//  Kamera (opcjonalna) - dwa tryby:
//
//  1) ZEWNĘTRZNA (domyślny kierunek): osobny moduł ESP32-CAM robi
//     zdjęcia. Master pobiera gotowy JPEG z adresu cam_remote_url
//     (np. http://192.168.1.142/capture) i sam go analizuje oraz
//     zapisuje na karcie SD; jeśli adres tylko wyzwala ujęcie, zdjęcie
//     wraca przez HTTP POST na /api/camera/upload.
//
//  2) LOKALNA: kamera podłączona do pinów magistrali równoległej
//     ESP32-S3 z PSRAM (np. OV2640 / OV3660 / OV5640 / GC0308).
//
//  Zdjęcia JPEG trafiają na kartę SD do katalogu /photos/YYYY-MM/,
//  skąd są serwowane na stronie www (galeria) i można je usuwać /
//  pobierać jak zwykłe pliki karty. Obok każdego .jpg zapisywany
//  jest mały plik .json z wynikiem analizy (jasność, luksy, dzień/
//  noc, RGB, ruch, zachmurzenie). Limity cam_max_photos/cam_max_mb
//  kasują najstarsze zdjęcia razem z ich plikami analizy.
//
//  Wynik analizy jest też wystawiany jako kanały stacji (cam_weather,
//  cam_phase, cam_cloud, cam_rain, cam_snow, cam_fog, cam_lux), więc
//  "co widać na niebie" trafia na pulpit, do dziennika i do Home
//  Assistant razem z pozostałymi pomiarami.
//
//  Ustawienie cam_rotate (0/90/180/270, 90 = w prawo) obraca obraz przy
//  podglądzie na stronie i przy analizie - dzięki temu region nieba jest
//  zawsze na górze kadru, niezależnie od tego, jak wisi kamera.
//
//  Warianty ESP32-C3 (STACJA_HAS_CAMERA=0) kompilują się poprawnie,
//  ale manager zgłasza "brak wsparcia sprzętowego" - żaden kod kamery
//  nie jest wtedy dołączany.
// =============================================================

// Wynik analizy pojedynczego zdjęcia. Analiza jest liczona z miniatury
// RGB565 dekodowanej z JPEG-a (skala 1/8), więc jest tania i mieści się
// nawet bez PSRAM. Zachmurzenie i luksy to wartości szacunkowe.
struct CamAnalysis {
  bool     valid = false;
  uint16_t w = 0, h = 0;       // rozmiar oryginalnego zdjęcia (piksele)
  uint32_t ts = 0;             // czas wykonania zdjęcia (unix)
  float    brightness = 0;     // średnia luminancja 0..255
  float    lux = 0;            // szacunkowe natężenie światła [lx]
  float    meanR = 0, meanG = 0, meanB = 0;   // średnie składowe 0..255
  float    motion = 0;         // zmiana vs poprzednia klatka 0..100 %
  float    cloudCover = -1;    // szacunkowe zachmurzenie 0..100 % (-1 = brak danych)
  String   phase;              // "day" / "dawn/dusk" / "night"
  // Szacunkowa klasyfikacja pogody z pojedynczego zdjęcia (wartości -1 = brak
  // danych). To heurystyka z jasności/kolorów/kontrastu, a nie pomiar - przy
  // jednym zdjęciu co 15 minut jest orientacyjna.
  float    snowPct = -1;       // pokrywa / intensywność śniegu 0..100 %
  float    rainPct = -1;       // intensywność opadu deszczu 0..100 %
  float    fogPct  = -1;       // intensywność mgły 0..100 %
  String   weather;            // "clear"/"cloudy"/"fog"/"rain"/"snow"/"night"
};

// Siatka próbkowania analizy - stała niezależnie od rozdzielczości zdjęcia,
// żeby bufor detekcji ruchu miał znany rozmiar.
#define ANA_GRID_W 64
#define ANA_GRID_H 48

class CameraManager {
public:
  bool begin();              // inicjalizacja wg config (tylko gdy włączona)
  void loop();               // timelapse: zdjęcie co cam_interval_min minut
  void applyConfig();        // po zapisaniu ustawień z www (bez restartu)

  bool capture();            // zdjęcie lokalne (kamera podłączona do pinów)
  bool ingestJpeg(const uint8_t* data, size_t len);  // odbiór zdjęcia z ESP32-CAM
  bool triggerRemote();      // zdjęcie z kamery zewnętrznej: pobierz JPEG albo wyzwól ujęcie
  // Zlecenie zdjęcia z kamery zewnętrznej do wykonania w pętli głównej.
  // Pobieranie zdjęcia trwa kilka sekund, więc nie może się odbywać w wątku
  // serwera www (przekroczenie watchdoga zadania restartowało stację).
  bool requestCapture();
  bool start();              // próba (re)inicjalizacji sterownika kamery
  void stop();               // zwolnij sterownik (gdy wyłączono kamerę)

  bool enabled() const { return enabled_; }
  bool present() const { return present_; }     // sterownik zainicjalizowany
  bool configured() const;                      // wymagane piny ustawione

  String error() const { return lastError_; }
  String modelName() const { return modelName_; }
  String lastFile() const { return lastFile_; }
  unsigned long lastCaptureMs() const { return lastCaptureMs_; }
  uint16_t intervalMin() const { return intervalMin_; }

  const CamAnalysis& lastAnalysis() const { return lastAnalysis_; }
  uint16_t maxPhotos() const { return config.camMaxPhotos(); }
  uint16_t maxMb() const { return config.camMaxMb(); }
  String remoteUrl() const { return config.camRemoteUrl(); }
  uint16_t rotate() const { return config.camRotate(); }

  String toJson() const;                        // status dla /api/camera

private:
  // Zapisuje JPEG na SD, analizuje go, zapisuje plik .json obok i pilnuje limitów.
  bool saveAndAnalyze(const uint8_t* jpg, size_t len, String& outPath);
  // Dekoduje miniaturę JPEG (RGB565, skala 1/8) i liczy analizę.
  bool analyzeJpeg(const uint8_t* jpg, size_t len);
  bool analyzeRgb565(const uint8_t* buf, uint16_t w, uint16_t h);
  // Pobiera gotowy JPEG z adresu kamery zewnętrznej (http://.../capture).
  // Zwraca: 1 = odebrano zdjęcie, 0 = połączenie działa, ale to nie JPEG
  // (kamera w trybie wyzwalania), -1 = błąd.
  int  fetchRemoteJpeg();
  // Wystawia wynik analizy jako kanały stacji (pulpit, dziennik, MQTT/HA).
  void publishAnalysis();
  void writeAnalysisJson(const String& jpgPath);
  void enforceQuota();                          // kasowanie najstarszych zdjęć ponad limit
  void deletePhotoWithMeta(const String& jpgPath);

  bool        enabled_ = false;
  bool        present_ = false;
  bool        capturing_ = false;
  bool        pendingCapture_ = false;   // zdjęcie zlecone z www - do wykonania w pętli głównej
  uint16_t    intervalMin_ = 15;
  unsigned long lastCaptureMs_ = 0;
  String      lastFile_;
  String      lastError_;

  // Odcisk ostatnio zapisanego zdjęcia. Ten sam JPEG potrafi dotrzeć dwiema
  // drogami naraz (kamera wysyła go POST-em na /api/camera/upload i zwraca w
  // odpowiedzi na GET /capture), a wtedy na karcie powstawały dwa identyczne
  // pliki. Duplikat w krótkim odstępie jest pomijany.
  uint32_t    lastJpegHash_ = 0;
  uint32_t    lastJpegLen_ = 0;
  unsigned long lastJpegMs_ = 0;
  String      modelName_;

  // Wynik analizy ostatniego zdjęcia + siatka luminancji poprzedniej klatki
  // (do detekcji ruchu między kolejnymi zdjęciami).
  CamAnalysis lastAnalysis_;
  uint8_t     prevLum_[ANA_GRID_W * ANA_GRID_H];
  bool        havePrevLum_ = false;
};

extern CameraManager camera;
