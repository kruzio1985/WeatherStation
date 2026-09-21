/* =============================================================================
 * Stacja Pogody - stacja meteorologiczna na ESP32-S3 WROOM-1 (N16R8)
 * Copyright (c) 2026 kruzio1985 - https://github.com/kruzio1985
 * Licencja: PolyForm Noncommercial License 1.0.0 (plik LICENSE)
 * Użytek niekomercyjny. Kontakt: kruzio1985@users.noreply.github.com
 * =============================================================================
 */
#pragma once
#include <Arduino.h>

// =============================================================
//  Rozmiar programu - bez weryfikowania sumy SHA-256 obrazu.
//
//  ESP.getSketchSize()/getFreeSketchSpace() z rdzenia Arduino liczą
//  SHA-256 całego obrazu aplikacji przy KAŻDYM wywołaniu: to ok. 90 ms
//  i - gdy odczyt flash zwróci choć jeden zły bajt - komunikat
//  "esp_image: Image hash failed - image is corrupt" w logu, mimo że
//  obraz działa. Tutaj rozmiar liczymy z nagłówków obrazu (ok. 150 B
//  odczytu), więc trwa to mikrosekundy i nic nie loguje.
// =============================================================

uint32_t appImageSize();            // rozmiar obrazu firmware (bajty)
uint32_t appImagePartitionSize();   // rozmiar partycji programu (bajty)
uint32_t appImageFreeBytes();       // wolne miejsce w partycji programu
void     appImageInfoRefresh();     // policz ponownie (np. po aktualizacji)
