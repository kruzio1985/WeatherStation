/* =============================================================================
 * Stacja Pogody - stacja meteorologiczna na ESP32-S3 WROOM-1 (N16R8)
 * Copyright (c) 2026 kruzio1985 - https://github.com/kruzio1985
 * Licencja: PolyForm Noncommercial License 1.0.0 (plik LICENSE)
 * Użytek niekomercyjny. Kontakt: kruzio1985@users.noreply.github.com
 * =============================================================================
 */
#pragma once
#include <Arduino.h>

// Duże teksty (JSON) w partycji NVS.
//
// Partycja NVS nie jest ruszana ani przez aktualizację programu (OTA), ani
// przez wgranie nowych plików www do LittleFS. Trzymamy w niej ustawienia
// (Wi-Fi, piny, kalibracja, MQTT), żeby nie przepadały po aktualizacji.
//
// Wpis w NVS ma limit ok. 4000 B, dlatego dane dzielimy na kawałki po 3500 B
// (klucze "<key>_0", "<key>_1", ... + licznik i znacznik poprawności).

// Przestrzeń nazw i klucze ustawień stacji (wspólne dla config.cpp i www).
#define NVS_NS        "stacja"
#define NVS_KEY_CFG   "cfg"   // pełna konfiguracja (JSON)
#define NVS_KEY_STA   "sta"   // licznik deszczu i stan dzienny (JSON)

bool   nvsStoreWrite(const char* ns, const char* key, const String& data);
String nvsStoreRead(const char* ns, const char* key);   // "" = brak danych
void   nvsStoreErase(const char* ns, const char* key);
size_t nvsStoreSize(const char* ns, const char* key);

struct NvsPartitionInfo {
  bool   ok = false;
  size_t sizeBytes = 0;      // rozmiar partycji NVS
  size_t totalEntries = 0;   // wszystkie wpisy
  size_t usedEntries = 0;    // zajęte wpisy
  size_t freeEntries = 0;    // wolne wpisy
  size_t namespaces = 0;     // liczba przestrzeni nazw
};

NvsPartitionInfo nvsPartitionInfo();
