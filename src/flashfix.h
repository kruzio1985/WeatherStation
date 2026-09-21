/* =============================================================================
 * Stacja Pogody - stacja meteorologiczna na ESP32-S3 WROOM-1 (N16R8)
 * Copyright (c) 2026 kruzio1985 - https://github.com/kruzio1985
 * Licencja: Stacja Pogody Non-Commercial License 1.0 (plik LICENSE)
 * Użytek niekomercyjny. Kontakt: kruzio1985@users.noreply.github.com
 * =============================================================================
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Obejście błędu odczytu flash w ESP32-S3 - szczegóły w flashfix.cpp.
// Wywołać raz, jak najwcześniej w setup(), przed pierwszym użyciem LittleFS.
bool installFlashReadFix(void);

// Czy obejście jest zainstalowane.
bool flashReadFixActive(void);

// Rozmiar porcji odczytu używanej przez obejście (w bajtach).
uint32_t flashReadFixChunk(void);

// Adres podmienionej tablicy sterownika (kontrola umiejscowienia w DRAM).
const void* flashReadFixTable(void);

// Diagnostyka: na czas odczytu przywraca oryginalny (błędny) odczyt sterownika, żeby
// porównać go z odczytem poprawionym. Surowy odczyt robić przez esp_partition_read()
// albo esp_flash_read(). Wywołania zawsze w parze suspend/resume.
void flashReadFixSuspend(void);
void flashReadFixResume(void);

#ifdef __cplusplus
}
#endif
