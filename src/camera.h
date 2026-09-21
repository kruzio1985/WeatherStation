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
//     zdjęcia i wysyła je HTTP POST na /api/camera/upload. Master
//     tylko odbiera JPEG-a, analizuje go i zapisuje na karcie SD.
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
  bool triggerRemote();      // żądanie natychmiastowego zdjęcia (HTTP GET do kamery)
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

  String toJson() const;                        // status dla /api/camera

private:
  // Zapisuje JPEG na SD, analizuje go, zapisuje plik .json obok i pilnuje limitów.
  bool saveAndAnalyze(const uint8_t* jpg, size_t len, String& outPath);
  // Dekoduje miniaturę JPEG (RGB565, skala 1/8) i liczy analizę.
  bool analyzeJpeg(const uint8_t* jpg, size_t len);
  bool analyzeRgb565(const uint8_t* buf, uint16_t w, uint16_t h);
  void writeAnalysisJson(const String& jpgPath);
  void enforceQuota();                          // kasowanie najstarszych zdjęć ponad limit
  void deletePhotoWithMeta(const String& jpgPath);

  bool        enabled_ = false;
  bool        present_ = false;
  bool        capturing_ = false;
  uint16_t    intervalMin_ = 15;
  unsigned long lastCaptureMs_ = 0;
  String      lastFile_;
  String      lastError_;
  String      modelName_;

  // Wynik analizy ostatniego zdjęcia + siatka luminancji poprzedniej klatki
  // (do detekcji ruchu między kolejnymi zdjęciami).
  CamAnalysis lastAnalysis_;
  uint8_t     prevLum_[ANA_GRID_W * ANA_GRID_H];
  bool        havePrevLum_ = false;
};

extern CameraManager camera;
