/* =============================================================================
 * Stacja Pogody - stacja meteorologiczna na ESP32-S3 WROOM-1 (N16R8)
 * Copyright (c) 2026 kruzio1985 - https://github.com/kruzio1985
 * Licencja: Stacja Pogody Non-Commercial License 1.0 (plik LICENSE)
 * Użytek niekomercyjny. Kontakt: kruzio1985@users.noreply.github.com
 * =============================================================================
 */
#pragma once
#include <Arduino.h>

// Status i diagnostyka w formie JSON:
//  - strona www pokazuje je w zakładkach "Pulpit" i "Diagnostyka",
//  - węzeł RS485 odsyła je masterowi na polecenie "status" / "diag".
//
// Definicje są w api_json.cpp, a nie w web_server.cpp, bo firmware węzła
// pracującego bez sieci (STACJA_HEADLESS - patrz platformio.ini) nie zawiera
// serwera www, a master i tak odpytuje węzeł o te dane po magistrali.
String apiStatusJson();
String apiDiagnosticsJson();
