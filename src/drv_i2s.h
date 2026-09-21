/* =============================================================================
 * Stacja Pogody - stacja meteorologiczna na ESP32-S3 WROOM-1 (N16R8)
 * Copyright (c) 2026 kruzio1985 - https://github.com/kruzio1985
 * Licencja: PolyForm Noncommercial License 1.0.0 (plik LICENSE)
 * Użytek niekomercyjny. Kontakt: kruzio1985@users.noreply.github.com
 * =============================================================================
 */
#pragma once
#include "drv_mod.h"

// =============================================================
//  Audio / I²S: mikrofon pomiarowy (RX) + wzmacniacz (TX).
//  Opis modułu, konfiguracji i kanałów - patrz drv_i2s.cpp.
//
//  Kanały (moduł publikuje wartości zmierzone, w dBFS):
//    * "snd_level" - poziom RMS wygładzony (dBFS),
//    * "snd_peak"  - szczyt bloku z wolnym opadaniem (dBFS),
//    * "snd_leq"   - średnia energetyczna z okna czasowego (LEQ, dBFS).
//
//  Piny (mapa pinów, grupa "Audio / I²S"): "i2s_bclk", "i2s_ws",
//  "i2s_din" (mikrofon) oraz "i2s_dout" (wzmacniacz MAX98357A).
//  Gdy którykolwiek z nich jest wyłączony (-1), sterownik I²S nie
//  jest w ogóle instalowany i żaden kanał nic nie publikuje.
//
//  Współczynniki z sekcji "extra" pamięci ustawień (tylko odczyt):
//    * "snd_offset_db" - korekta poziomu [dB] (czułość mikrofonu),
//    * "snd_leq_s"     - długość okna LEQ [s] (domyślnie 10 s).
// =============================================================

// Sterownik modułowy (rejestracja w src/drv_table.cpp po stronie projektu)
extern const DrvModule drvI2s;
