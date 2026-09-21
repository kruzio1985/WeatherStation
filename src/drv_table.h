/* =============================================================================
 * Stacja Pogody - stacja meteorologiczna na ESP32-S3 WROOM-1 (N16R8)
 * Copyright (c) 2026 kruzio1985 - https://github.com/kruzio1985
 * Licencja: Stacja Pogody Non-Commercial License 1.0 (plik LICENSE)
 * Użytek niekomercyjny. Kontakt: kruzio1985@users.noreply.github.com
 * =============================================================================
 * Tabela sterowników modułowych.
 *
 * Każdy plik drv_*.cpp opisuje jeden zestaw czujników na tej samej magistrali
 * (patrz drv_mod.h) i wystawia dokładnie jedną stałą DrvModule, np.
 * "extern const DrvModule drvThs;". Tutaj te moduły są wypisane raz, a kod
 * stacji (sensors_extra.cpp) nie musi wiedzieć, ile ich jest.
 *
 * Dodanie nowego zestawu czujników to dwie linijki: nowy plik drv_*.cpp oraz
 * wpis w tablicy w drv_table.cpp. Kod jest wspólny dla obu ESP (master i węzeł
 * RS485 buduje się z tych samych źródeł), więc każdy nowy sterownik od razu
 * działa na obu płytkach.
 * ========================================================================== */
#pragma once
#include "drv_mod.h"

// Liczba modułów i dostęp do nich (indeks poza zakresem zwraca nullptr)
uint16_t drvModuleCount();
const DrvModule* drvModuleAt(uint16_t i);

// Wykrywanie sprzętu i pierwsze odczyty (wołane z SensorManager::beginExtra)
void drvBeginAll();
// Odczyt wszystkich modułów (wołane z SensorManager::readExtra)
void drvReadAll();
