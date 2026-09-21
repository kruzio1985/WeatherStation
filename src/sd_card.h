/* =============================================================================
 * Stacja Pogody - stacja meteorologiczna na ESP32-S3 WROOM-1 (N16R8)
 * Copyright (c) 2026 kruzio1985 - https://github.com/kruzio1985
 * Licencja: Stacja Pogody Non-Commercial License 1.0 (plik LICENSE)
 * Użytek niekomercyjny. Kontakt: kruzio1985@users.noreply.github.com
 * =============================================================================
 */
#pragma once
#include <Arduino.h>
#include <FS.h>
#include <SD.h>
#include <SD_MMC.h>
#include <vector>

// ESP32-C3 (i inne chipy bez hosta SDMMC) nie mają interfejsu SDMMC,
// więc karta SD działa tam wyłącznie po SPI. Makro pilnuje, żeby kod
// nie odwoływał się do obiektu SD_MMC na takich płytkach.
#if defined(SOC_SDMMC_HOST_SUPPORTED) || defined(CONFIG_SOC_SDMMC_HOST_SUPPORTED)
#define SD_MMC_AVAILABLE 1
#else
#define SD_MMC_AVAILABLE 0
#endif

// Zegar SPI karty SD. 4 MHz jest bezpieczne dla dłuższych przewodów;
// po sprawdzeniu można podnieść (np. 20000000) w jednym miejscu.
#define SD_SPI_FREQ 4000000

struct SdFileInfo {
  String path;
  size_t size;
  time_t mtime;
  bool isDir;
};

class SdCardManager {
public:
  bool begin();
  // Ponowna próba montażu karty bez restartu stacji (zakładka "Karta SD"
  // na stronie www -> "Wykryj kartę ponownie"). Zwalnia poprzedni montaż
  // i próbuje od nowa: SDMMC (jeśli piny ustawione), potem SPI.
  bool remount();
  bool mounted();
  bool usingSdmmc() { return sdmmc_; }
  // Opis interfejsu użytego (albo próbowanego) przy ostatnim montażu.
  String interfaceInfo();

  bool listDir(const String& dir, std::vector<SdFileInfo>& out);
  // Tworzy katalog (i katalogi nadrzędne) na karcie.
  bool mkdirs(const String& dir);
  bool readFile(const String& path, String& out, size_t maxLen);
  bool appendFile(const String& path, const String& line);
  bool deleteFile(const String& path);
  bool exists(const String& path);
  bool format();
  uint64_t totalBytes();
  uint64_t usedBytes();

  // Otwiera strumień do pobierania pliku (web). Zwraca uchwyt, który
  // trzeba zamknąć przez caller (f.close()).
  File openRead(const String& path);

  // Dopisuje blok bajtów do pliku (FILE_APPEND), tworząc w razie potrzeby
  // katalogi nadrzędne. Bezpieczne do wołania z uploadu www (blokada muteksa
  // wewnątrz), np. przy zapisywaniu binarek firmware w /fw.
  bool writeBlock(const String& path, const uint8_t* data, size_t len);

  String error() { return err_; }

private:
  bool mountSdmmc();
  bool mountSpi();
  bool formatSdmmc();
  bool formatSpi();
  void unmount();
  // Warianty bez blokady muteksa - wołane tam, gdzie muteks jest już zajęty.
  bool existsRaw(const String& path);
  bool mkdirRaw(const String& path);
  bool mkdirsRaw(const String& dir);

  bool mounted_ = false;
  bool sdmmc_ = false;
  String err_;
  // Diagnostyka ostatniej próby: co było włączone i na jakich pinach.
  String info_;
  SemaphoreHandle_t mutex_ = nullptr;
};

extern SdCardManager sdCard;
