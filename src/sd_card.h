/* =============================================================================
 * Stacja Pogody - stacja meteorologiczna na ESP32-S3 WROOM-1 (N16R8)
 * Copyright (c) 2026 kruzio1985 - https://github.com/kruzio1985
 * Licencja: PolyForm Noncommercial License 1.0.0 (plik LICENSE)
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

// Zegar SPI karty SD. Każde żądanie do karty ma narzut niezależny od rozmiaru
// danych, więc taktowanie przekłada się wprost na czas blokowania pętli głównej
// i obsługi www: przy 4 MHz pojedynczy wpis katalogu (nazwa długa = kilka
// sektorów) kosztował ~37 ms, więc skan katalogu z kilkuset zdjęciami trwał
// dziesiątki sekund i kończył się resetem "watchdog zadania". Startujemy z
// najwyższego taktowania i schodzimy niżej, gdy karta nie odpowie.
#define SD_SPI_FREQ_FAST 20000000
#define SD_SPI_FREQ      4000000

// Numer wolumenu FatFS karty ("0:"). ESP-IDF montuje pierwszy wolumen jako 0,
// a na tej płytce karta jest jedynym nośnikiem na FatFS.
#define SD_FATFS_DRIVE "0:"

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
  // Skan katalogu z ograniczeniem. maxEntries > 0 ucina liczbę wpisów:
  // przy keepLast = true zostają ostatnie wpisy z katalogu (FAT dopisuje nowe
  // na końcu, więc są to najnowsze pliki), a ext zawęża wynik do jednego
  // rozszerzenia (np. ".jpg"). Bez tego katalog ze setkami zdjęć blokuje
  // obsługę www na dziesiątki sekund.
  bool listDir(const String& dir, std::vector<SdFileInfo>& out,
               size_t maxEntries, bool keepLast = false, const char* ext = nullptr);
  // Skan katalogu BEZ metadanych: podaje tylko ścieżki (i to, czy wpis jest
  // katalogiem). Na FATFS każde pobranie rozmiaru/czasu wpisu to osobne
  // wyszukanie w katalogu, więc listDir() przy n wpisach kosztuje O(n^2) -
  // katalog ze 300 zdjęciami zajmował 17 s. Gdy rozmiar i czas nie są
  // potrzebne (albo czas da się odczytać z nazwy pliku), używamy tego wariantu.
  bool listDirNames(const String& dir, std::vector<String>& names,
                    size_t maxEntries = 0, bool keepLast = false,
                    const char* ext = nullptr);
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
  // Szybki odczyt katalogu (FatFS f_readdir); false = użyj ścieżki przez File API.
  bool listDirFat(const String& dir, std::vector<SdFileInfo>& out,
                  size_t maxEntries, bool keepLast, const char* ext);
  void unmount();
  // Warianty bez blokady muteksa - wołane tam, gdzie muteks jest już zajęty.
  bool existsRaw(const String& path);
  bool mkdirRaw(const String& path);
  bool mkdirsRaw(const String& dir);

  bool mounted_ = false;
  bool sdmmc_ = false;
  uint32_t spiFreq_ = SD_SPI_FREQ;   // taktowanie, na którym udało się zamontować
  String err_;
  // Diagnostyka ostatniej próby: co było włączone i na jakich pinach.
  String info_;
  SemaphoreHandle_t mutex_ = nullptr;
};

extern SdCardManager sdCard;

// Czas zapisu zaszyty w nazwie pliku: ".../RRRR-MM-DD_GGMMSS.jpg" (tak nazywają
// się zdjęcia kamery). Zwraca 0, gdy nazwa nie ma takiego wzorca. Dzięki temu
// sortowanie i kasowanie po wieku nie wymaga pytania karty o metadane każdego
// wpisu (patrz listDirNames).
time_t sdFileNameTime(const String& path);
