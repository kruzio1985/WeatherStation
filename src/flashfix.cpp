/* =============================================================================
 * Stacja Pogody - stacja meteorologiczna na ESP32-S3 WROOM-1 (N16R8)
 * Copyright (c) 2026 kruzio1985 - https://github.com/kruzio1985
 * Licencja: PolyForm Noncommercial License 1.0.0 (plik LICENSE)
 * Użytek niekomercyjny. Kontakt: kruzio1985@users.noreply.github.com
 * =============================================================================
 */
// Obejście błędu odczytu flash w ESP32-S3 (16 MB flash + 8 MB PSRAM octal, Arduino core 2.0.17 / IDF 4.4).
//
// Objaw: przy odczycie z wyłączonym cache (np. przez LittleFS, NVS, weryfikację OTA) dane są
// poprawne tylko wtedy, gdy całe żądanie mieści się w jednym bloku 32-bajtowym wyrównanym do
// 32 bajtów. Zmierzone na tej płytce dwa objawy tego samego błędu sterownika:
//   * żądanie >= 64 bajtów zwracało 32 prawdziwe bajty, a górną połowę każdej porcji 64-bajtowej
//     powtarzało z dolnej (bajty 32..63 błędne) - psuło to littlefs ("Corrupted dir pair"),
//   * żądanie 32-bajtowe zaczynające się w połowie bloku (adres % 32 == 16) zwracało błędne
//     bajty 16..31.
// Drugi przypadek psuł weryfikację obrazu OTA: IDF czyta dopisany skrót SHA-256 obrazu jednym
// odczytem 32 bajtów spod adresu "start + długość obrazu" (dla naszego firmware:
// 0x10000 + 1538224 = 0x187B30, czyli 16 mod 32), więc porównanie skrótu wypadało źle dla
// KAŻDEGO obrazu i Update.end() kończyło się błędem aktywacji ("Could Not Activate The Firmware").
//
// Rozwiązanie: każde żądanie realizujemy wyrównanymi porcjami 32-bajtowymi - odczyt zawsze
// startuje na granicy 32 bajtów i ma dokładnie 32 bajty, a bajty spoza żądanego zakresu
// odrzucamy. Odczyt nigdy nie wychodzi poza flash, bo wyrównanie 32-bajtowych bloków pokrywa
// się z końcem partycji.

#include "flashfix.h"

#include <string.h>

#include <esp_flash.h>
#include "hal/spi_flash_types.h"

#if defined(__has_include)
#  if __has_include("esp_attr.h")
#    include "esp_attr.h"
#  endif
#endif
#ifndef IRAM_ATTR
#  define IRAM_ATTR __attribute__((section(".iram1")))
#endif

#define FLASHFIX_CHUNK 32
#define FLASHFIX_MASK (FLASHFIX_CHUNK - 1)

// Funkcja z podmienionej tablicy wykonuje się przy wyłączonym cache, więc:
// - sama musi leżeć w IRAM,
// - wszystko, czego dotyka (tablica sterownika i wskaźnik na oryginalny odczyt), musi leżeć
//   w DRAM. Zwykłe zmienne statyczne bez inicjalizatora trafiają do .bss, czyli do DRAM.
static spi_flash_host_driver_t s_table;
static const spi_flash_host_driver_t* s_origTable;
static esp_err_t (*s_origRead)(spi_flash_host_inst_t* host, void* buffer, uint32_t address, uint32_t readLen);
static bool s_active;

static IRAM_ATTR esp_err_t flashfixRead(spi_flash_host_inst_t* host, void* buffer, uint32_t address, uint32_t readLen) {
  if (readLen == 0) return ESP_OK;

  uint8_t* dst = (uint8_t*)buffer;

  // Żądanie wyrównane do 32 bajtów i o długości wielokrotności 32 bajtów - porcje pokrywają się
  // z żądaniem, więc piszemy wprost do bufora odbiorcy.
  if ((address & FLASHFIX_MASK) == 0 && (readLen & FLASHFIX_MASK) == 0) {
    while (readLen > 0) {
      uint32_t n = readLen > FLASHFIX_CHUNK ? FLASHFIX_CHUNK : readLen;
      esp_err_t err = s_origRead(host, dst, address, n);
      if (err != ESP_OK) return err;
      dst += n;
      address += n;
      readLen -= n;
    }
    return ESP_OK;
  }

  // Żądanie zaczyna się w środku bloku lub nie kończy się na granicy - czytamy całe wyrównane
  // bloki 32-bajtowe i kopiujemy z nich tylko potrzebny zakres.
  uint8_t tmp[FLASHFIX_CHUNK];
  const uint32_t end = address + readLen;
  uint32_t block = address & ~(uint32_t)FLASHFIX_MASK;

  while (block < end) {
    esp_err_t err = s_origRead(host, tmp, block, FLASHFIX_CHUNK);
    if (err != ESP_OK) return err;

    const uint32_t from = address > block ? address : block;
    const uint32_t to = end < block + FLASHFIX_CHUNK ? end : block + FLASHFIX_CHUNK;
    memcpy(dst + (from - address), tmp + (from - block), to - from);

    block += FLASHFIX_CHUNK;
  }
  return ESP_OK;
}

bool installFlashReadFix(void) {
  if (s_active) return true;

  esp_flash_t* chip = esp_flash_default_chip;
  if (chip == NULL || chip->host == NULL || chip->host->driver == NULL) return false;

  const spi_flash_host_driver_t* orig = chip->host->driver;
  if (orig->read == NULL || orig->read == flashfixRead) return false;

  memcpy(&s_table, orig, sizeof(s_table));
  s_origRead = orig->read;
  s_origTable = orig;
  s_table.read = flashfixRead;
  chip->host->driver = &s_table;
  s_active = true;
  return true;
}

// Diagnostyka (endpoint /api/ota/diag): porównanie odczytu surowego z poprawionym.
// Surowego odczytu nie wolno wykonywać bezpośrednio - trzeba go zrobić przez
// esp_partition_read()/esp_flash_read(), bo tylko one wyłączają cache wokół odczytu,
// a bez tego błąd sterownika nie występuje.
void flashReadFixSuspend(void) {
  if (!s_active) return;
  esp_flash_t* chip = esp_flash_default_chip;
  if (chip != NULL && chip->host != NULL) chip->host->driver = s_origTable;
}

void flashReadFixResume(void) {
  if (!s_active) return;
  esp_flash_t* chip = esp_flash_default_chip;
  if (chip != NULL && chip->host != NULL) chip->host->driver = &s_table;
}

bool flashReadFixActive(void) {
  return s_active;
}

uint32_t flashReadFixChunk(void) {
  return FLASHFIX_CHUNK;
}

const void* flashReadFixTable(void) {
  return &s_table;
}
