/* =============================================================================
 * Stacja Pogody - stacja meteorologiczna na ESP32-S3 WROOM-1 (N16R8)
 * Copyright (c) 2026 kruzio1985 - https://github.com/kruzio1985
 * Licencja: PolyForm Noncommercial License 1.0.0 (plik LICENSE)
 * Użytek niekomercyjny. Kontakt: kruzio1985@users.noreply.github.com
 * =============================================================================
 */
#include "sd_card.h"
#include "pins.h"
#include "pinmap.h"
#include "syslog.h"

#if defined(SOC_SDMMC_HOST_SUPPORTED) || defined(CONFIG_SOC_SDMMC_HOST_SUPPORTED)
#include "driver/sdmmc_host.h"
#include "driver/sdmmc_types.h"
#include "sdmmc_cmd.h"
#include "diskio_impl.h"
#include "diskio_sdmmc.h"
#endif
// FatFS jest potrzebny niezależnie od interfejsu - formatowanie karty
// po SPI też korzysta z f_mkfs (płytki bez SDMMC, np. ESP32-C3).
#include "ff.h"
#include "sd_diskio.h"

SdCardManager sdCard;

bool SdCardManager::begin() {
  mutex_ = xSemaphoreCreateMutex();
  return remount();
}

// Zwalnia poprzedni montaż (jeśli był) - bez tego druga próba SD.begin()
// nie ma sensu, bo obiekt FS trzyma stary sterownik dysku.
void SdCardManager::unmount() {
  if (!mounted_) return;
#if SD_MMC_AVAILABLE
  if (sdmmc_) SD_MMC.end();
  else
#endif
  SD.end();
  mounted_ = false;
}

bool SdCardManager::remount() {
  unmount();

  const int clk = pinMap.pin("sdmmc_clk");
  const int cmd = pinMap.pin("sdmmc_cmd");
  const int d0  = pinMap.pin("sdmmc_d0");
  const int cs  = pinMap.pin("sd_cs");
  const bool sdmmcOn = (clk >= 0 && cmd >= 0 && d0 >= 0);
  const bool spiOn = (cs >= 0);

  // 1. SDMMC 1-bit - tylko gdy wszystkie trzy piny są ustawione.
  if (sdmmcOn && mountSdmmc()) {
    mounted_ = true;
    sdmmc_ = true;
    err_ = "";
    info_ = "SDMMC 1-bit (CLK " + String(clk) + ", CMD " + String(cmd) + ", D0 " + String(d0) + ")";
    LOG_I("Karta SD wykryta (SDMMC 1-bit): %.1f GB", totalBytes() / 1073741824.0);
    return true;
  }
  if (sdmmcOn && spiOn) {
    LOG_W("SD SDMMC nie odpowiedział (CLK %d, CMD %d, D0 %d) - sprawdzam SPI", clk, cmd, d0);
  }

  // 2. SPI - piny SCK/MOSI/MISO z mapy pinów (SD.begin() sam ich nie zna).
  if (spiOn && mountSpi()) {
    mounted_ = true;
    sdmmc_ = false;
    err_ = "";
    info_ = "SPI (CS " + String(cs) + ", SCK " + String(pinMap.pin("sd_sck")) + ", MOSI " +
            String(pinMap.pin("sd_mosi")) + ", MISO " + String(pinMap.pin("sd_miso")) + ")";
    LOG_I("Karta SD wykryta (SPI): CS %d, SCK %d, MOSI %d, MISO %d - %.1f GB",
          cs, pinMap.pin("sd_sck"), pinMap.pin("sd_mosi"), pinMap.pin("sd_miso"),
          totalBytes() / 1073741824.0);
    return true;
  }

  if (!sdmmcOn && !spiOn) {
    err_ = "Karta SD wyłączona w mapie pinów (ustaw CS dla SPI albo CLK/CMD/D0 dla SDMMC)";
  } else if (spiOn) {
    err_ = "Nie wykryto karty SD na SPI (CS " + String(cs) + ", SCK " + String(pinMap.pin("sd_sck")) +
           ", MOSI " + String(pinMap.pin("sd_mosi")) + ", MISO " + String(pinMap.pin("sd_miso")) +
           ") - sprawdź piny i zasilanie modułu";
  } else {
    err_ = "Nie wykryto karty SD na SDMMC (sprawdź przypisanie pinów na stronie www)";
  }
  info_ = spiOn ? ("SPI, brak karty") : "SDMMC, brak karty";
  LOG_W("Karta SD niepodłączona - %s", err_.c_str());
  return false;
}

String SdCardManager::interfaceInfo() {
  return info_;
}

bool SdCardManager::mountSdmmc() {
#if defined(SOC_SDMMC_HOST_SUPPORTED) || defined(CONFIG_SOC_SDMMC_HOST_SUPPORTED)
  SD_MMC.setPins(pinMap.pin("sdmmc_clk"), pinMap.pin("sdmmc_cmd"), pinMap.pin("sdmmc_d0"));
  if (!SD_MMC.begin("/sdcard", true, false, SDMMC_FREQ_DEFAULT)) return false;
  return true;
#else
  return false;
#endif
}

// SPI: kolejność argumentów SPI.begin to (SCK, MISO, MOSI, SS).
// Bez tego wywołania SD.begin() wywołuje wewnętrznie SPI.begin() bez
// argumentów i karta ląduje na pinach domyślnych dla danego chipu.
bool SdCardManager::mountSpi() {
  const int cs = pinMap.pin("sd_cs");
  const int sck = pinMap.pin("sd_sck");
  const int mosi = pinMap.pin("sd_mosi");
  const int miso = pinMap.pin("sd_miso");
  if (sck >= 0 && mosi >= 0 && miso >= 0) SPI.begin(sck, miso, mosi, cs);
  if (!SD.begin(cs, SPI, SD_SPI_FREQ)) return false;
  return true;
}

bool SdCardManager::mounted() {
  return mounted_;
}

bool SdCardManager::listDir(const String& dir, std::vector<SdFileInfo>& out) {
  if (!mounted_) return false;
  if (mutex_) xSemaphoreTake(mutex_, portMAX_DELAY);

  const String base = dir.isEmpty() ? "/" : dir;
  File root;
#if SD_MMC_AVAILABLE
  if (sdmmc_) root = SD_MMC.open(base);
  else
#endif
  root = SD.open(base);

  bool ok = (bool)root;
  if (ok) {
    File f = root.openNextFile();
    while (f) {
      SdFileInfo info;
      const String name = String(f.name());
      // f.name() zwraca samą nazwę (bez katalogu), a pobieranie i usuwanie
      // z www dostają pełną ścieżkę - dlatego sklejamy ją tutaj.
      info.path = name.startsWith("/") ? name
                                       : (base.endsWith("/") ? base + name : base + "/" + name);
      info.size = f.size();
      info.mtime = f.getLastWrite();
      info.isDir = f.isDirectory();
      out.push_back(info);
      f = root.openNextFile();
    }
  }
  root.close();
  if (mutex_) xSemaphoreGive(mutex_);
  return ok;
}

bool SdCardManager::readFile(const String& path, String& out, size_t maxLen) {
  if (!mounted_) return false;
  if (mutex_) xSemaphoreTake(mutex_, portMAX_DELAY);
  File f = openRead(path);
  if (!f) { if (mutex_) xSemaphoreGive(mutex_); return false; }

  out = "";
  out.reserve(min((size_t)f.size(), maxLen));
  while (f.available() && out.length() < maxLen) {
    out += (char)f.read();
  }
  f.close();
  if (mutex_) xSemaphoreGive(mutex_);
  return true;
}

bool SdCardManager::appendFile(const String& path, const String& line) {
  if (!mounted_) return false;
  if (mutex_) xSemaphoreTake(mutex_, portMAX_DELAY);
  // SD.open() nie tworzy katalogów, więc pierwszy zapis do /logs na świeżej
  // (albo sformatowanej) karcie kończył się cicho błędem.
  const int slash = path.lastIndexOf('/');
  if (slash > 0) mkdirsRaw(path.substring(0, slash));
  File f;
#if SD_MMC_AVAILABLE
  if (sdmmc_) f = SD_MMC.open(path, FILE_APPEND);
  else
#endif
  f = SD.open(path, FILE_APPEND);
  if (!f) { if (mutex_) xSemaphoreGive(mutex_); return false; }
  bool ok = f.print(line);
  f.close();
  if (mutex_) xSemaphoreGive(mutex_);
  return ok;
}

bool SdCardManager::deleteFile(const String& path) {
  if (!mounted_) return false;
  if (mutex_) xSemaphoreTake(mutex_, portMAX_DELAY);
  bool ok;
#if SD_MMC_AVAILABLE
  if (sdmmc_) ok = SD_MMC.remove(path);
  else
#endif
  ok = SD.remove(path);
  if (mutex_) xSemaphoreGive(mutex_);
  return ok;
}

bool SdCardManager::exists(const String& path) {
  if (!mounted_) return false;
  if (mutex_) xSemaphoreTake(mutex_, portMAX_DELAY);
  bool ok;
#if SD_MMC_AVAILABLE
  if (sdmmc_) ok = SD_MMC.exists(path);
  else
#endif
  ok = SD.exists(path);
  if (mutex_) xSemaphoreGive(mutex_);
  return ok;
}

bool SdCardManager::existsRaw(const String& path) {
#if SD_MMC_AVAILABLE
  if (sdmmc_) return SD_MMC.exists(path);
#endif
  return SD.exists(path);
}

bool SdCardManager::mkdirRaw(const String& path) {
#if SD_MMC_AVAILABLE
  if (sdmmc_) return SD_MMC.mkdir(path);
#endif
  return SD.mkdir(path);
}

// Tworzy katalog wraz z katalogami nadrzędnymi ("/logs", "/logs/2026/01").
bool SdCardManager::mkdirsRaw(const String& dir) {
  if (dir.isEmpty() || dir == "/") return true;
  if (existsRaw(dir)) return true;
  int pos = 0;
  while (true) {
    const int next = dir.indexOf('/', pos + 1);
    const String part = (next < 0) ? dir : dir.substring(0, next);
    if (part.length() > 1 && !existsRaw(part) && !mkdirRaw(part)) {
      LOG_W("Nie udało się utworzyć katalogu %s na karcie SD", part.c_str());
      return false;
    }
    if (next < 0) break;
    pos = next;
  }
  return true;
}

bool SdCardManager::mkdirs(const String& dir) {
  if (!mounted_) return false;
  if (mutex_) xSemaphoreTake(mutex_, portMAX_DELAY);
  const bool ok = mkdirsRaw(dir);
  if (mutex_) xSemaphoreGive(mutex_);
  return ok;
}

bool SdCardManager::format() {
  if (!mounted_) return false;
  if (mutex_) xSemaphoreTake(mutex_, portMAX_DELAY);

  LOG_W("Formatowanie karty SD (%s) - wszystkie dane zostaną usunięte",
        sdmmc_ ? "SDMMC" : "SPI");
  bool ok = sdmmc_ ? formatSdmmc() : formatSpi();
  if (ok) LOG_I("Karta SD sformatowana (FAT32), wolne %.1f GB", totalBytes() / 1073741824.0);
  else    LOG_E("Formatowanie karty SD nieudane");

  if (mutex_) xSemaphoreGive(mutex_);
  return ok;
}

// Formatowanie karty SDMMC (1-bit).
// SD_MMC nie ma publicznego .format(), więc wykonujemy to "ręcznie" na
// warstwie FatFS: demontaż -> niska inicjalizacja hosta/karty ->
// rejestracja sterownika dysku -> f_mkfs -> sprzątanie -> ponowny montaż.
bool SdCardManager::formatSdmmc() {
#if defined(SOC_SDMMC_HOST_SUPPORTED) || defined(CONFIG_SOC_SDMMC_HOST_SUPPORTED)
  bool ok = false;
  bool hostInited = false;
  BYTE pdrv = 0xFF;
  sdmmc_card_t* card = nullptr;

  sdmmc_host_t host = SDMMC_HOST_DEFAULT();
  host.flags = SDMMC_HOST_FLAG_1BIT;
  host.slot = SDMMC_HOST_SLOT_1;
  host.max_freq_khz = SDMMC_FREQ_DEFAULT;

  sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
  slot.clk = (gpio_num_t)digitalPinToGPIONumber(pinMap.pin("sdmmc_clk"));
  slot.cmd = (gpio_num_t)digitalPinToGPIONumber(pinMap.pin("sdmmc_cmd"));
  slot.d0  = (gpio_num_t)digitalPinToGPIONumber(pinMap.pin("sdmmc_d0"));
  slot.width = 1;

  // Pełny demontaż aktualnego systemu plików.
  SD_MMC.end();
  mounted_ = false;

  if (sdmmc_host_init() != ESP_OK) goto cleanup;
  hostInited = true;

  if (sdmmc_host_init_slot(host.slot, &slot) != ESP_OK) goto cleanup;

  card = (sdmmc_card_t*)malloc(sizeof(sdmmc_card_t));
  if (!card) goto cleanup;

  if (sdmmc_card_init(&host, card) != ESP_OK) goto cleanup;

  if (ff_diskio_get_drive(&pdrv) != ESP_OK || pdrv == 0xFF) goto cleanup;
  ff_diskio_register_sdmmc(pdrv, card);

  {
    char drv[3] = {(char)('0' + pdrv), ':', 0};
    BYTE* work = (BYTE*)malloc(FF_MAX_SS);
    if (work) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
      // FatFS R0.15 (Arduino core 3): f_mkfs(path, opt, work, len); NULL = domyślne opcje.
      FRESULT fr = f_mkfs(drv, nullptr, work, FF_MAX_SS);
#else
      FRESULT fr = f_mkfs(drv, FM_ANY, 0, work, FF_MAX_SS);
#endif
      free(work);
      ok = (fr == FR_OK);
    }
    ff_diskio_unregister(pdrv);
  }

cleanup:
  if (hostInited) sdmmc_host_deinit();
  if (card) free(card);

  delay(100);

  // Ponowny montaż (karta ma już świeży system plików).
  if (mountSdmmc()) {
    mounted_ = true;
    sdmmc_ = true;
    err_ = "";
    info_ = "SDMMC 1-bit (CLK " + String(pinMap.pin("sdmmc_clk")) + ", CMD " +
            String(pinMap.pin("sdmmc_cmd")) + ", D0 " + String(pinMap.pin("sdmmc_d0")) + ")";
    return ok;
  }

  mounted_ = false;
  err_ = "Sformatowano kartę, ale ponowny montaż się nie powiódł";
  return false;
#else
  (void)sdmmc_;
  return false;
#endif
}

// Formatowanie karty SD w trybie SPI (awaryjnym).
// Wykorzystuje wewnętrzny sterownik dysku biblioteki SD (sdcard_init),
// po czym wymusza f_mkfs i montuje kartę normalnie przez SD.begin().
bool SdCardManager::formatSpi() {
  SD.end();
  mounted_ = false;

  const int cs = pinMap.pin("sd_cs");
  const int sck = pinMap.pin("sd_sck");
  const int mosi = pinMap.pin("sd_mosi");
  const int miso = pinMap.pin("sd_miso");
  if (sck >= 0 && mosi >= 0 && miso >= 0) SPI.begin(sck, miso, mosi, cs);
  else SPI.begin();

  uint8_t pdrv = sdcard_init(cs, &SPI, SD_SPI_FREQ);
  if (pdrv == 0xFF) {
    err_ = "Nie udało się zainicjować karty SD (SPI) do formatowania";
    return false;
  }

  char drv[3] = {(char)('0' + pdrv), ':', 0};
  bool ok = false;
  BYTE* work = (BYTE*)malloc(FF_MAX_SS);
  if (work) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
    // FatFS R0.15 (Arduino core 3): f_mkfs(path, opt, work, len); NULL = domyślne opcje.
    FRESULT fr = f_mkfs(drv, nullptr, work, FF_MAX_SS);
#else
    FRESULT fr = f_mkfs(drv, FM_ANY, 0, work, FF_MAX_SS);
#endif
    free(work);
    ok = (fr == FR_OK);
  }

  sdcard_uninit(pdrv);

  delay(100);

  // Normalny montaż; SD.begin() odtworzy wewnętrzny stan obiektu SD.
  if (mountSpi()) {
    mounted_ = true;
    sdmmc_ = false;
    err_ = "";
    info_ = "SPI (CS " + String(cs) + ", SCK " + String(sck) + ", MOSI " + String(mosi) +
            ", MISO " + String(miso) + ")";
    return ok;
  }

  err_ = "Sformatowano kartę, ale ponowny montaż (SPI) się nie powiódł";
  return false;
}

uint64_t SdCardManager::totalBytes() {
  if (!mounted_) return 0;
  if (mutex_) xSemaphoreTake(mutex_, portMAX_DELAY);
#if SD_MMC_AVAILABLE
  uint64_t v = sdmmc_ ? SD_MMC.totalBytes() : SD.totalBytes();
#else
  uint64_t v = SD.totalBytes();
#endif
  if (mutex_) xSemaphoreGive(mutex_);
  return v;
}

uint64_t SdCardManager::usedBytes() {
  if (!mounted_) return 0;
  if (mutex_) xSemaphoreTake(mutex_, portMAX_DELAY);
#if SD_MMC_AVAILABLE
  uint64_t v = sdmmc_ ? SD_MMC.usedBytes() : SD.usedBytes();
#else
  uint64_t v = SD.usedBytes();
#endif
  if (mutex_) xSemaphoreGive(mutex_);
  return v;
}

File SdCardManager::openRead(const String& path) {
#if SD_MMC_AVAILABLE
  if (sdmmc_) return SD_MMC.open(path, FILE_READ);
#endif
  return SD.open(path, FILE_READ);
}

bool SdCardManager::writeBlock(const String& path, const uint8_t* data, size_t len) {
  if (!mounted_) return false;
  if (mutex_) xSemaphoreTake(mutex_, portMAX_DELAY);
  const int slash = path.lastIndexOf('/');
  if (slash > 0) mkdirsRaw(path.substring(0, slash));
  File f;
#if SD_MMC_AVAILABLE
  if (sdmmc_) f = SD_MMC.open(path, FILE_APPEND);
  else
#endif
  f = SD.open(path, FILE_APPEND);
  if (!f) { if (mutex_) xSemaphoreGive(mutex_); return false; }
  size_t w = f.write(data, len);
  f.close();
  if (mutex_) xSemaphoreGive(mutex_);
  return w == len;
}
