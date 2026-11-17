/* =============================================================================
 * Stacja Pogody - ustawienia radiowe Wi-Fi wspólne dla wszystkich płytek
 * Copyright (c) 2026 kruzio1985 - https://github.com/kruzio1985
 * Licencja: PolyForm Noncommercial License 1.0.0 (plik LICENSE)
 * Użytek niekomercyjny. Kontakt: kruzio1985@users.noreply.github.com
 * =============================================================================
 * WYMAGANIE PROJEKTU: każda płytka nadaje z MAKSYMALNĄ mocą i nie usypia
 * modemu Wi-Fi.
 *
 * Dlaczego to nie jest domyślne:
 *  - Arduino domyślnie włącza oszczędzanie energii (modem sleep) w trybie STA.
 *    Objaw: opóźnienia pingów 100-450 ms, skoki do sekund i gubienie pakietów
 *    (na węźle C3 zmierzono 65 % strat przy 90-450 ms), przez co odpytywanie
 *    po HTTP i MQTT potrafi się nie wyrobić w limitach czasu.
 *  - Domyślna moc bywa niższa od maksymalnej. Ten projekt już raz na tym
 *    poległ (sniffer nadawał 8.5 dBm przy stacji 19.5 dBm - patrz
 *    BLEDY-I-NAPRAWY.md, punkty 12 i 14).
 *
 * Wywoływać po WiFi.mode(...) i po udanym połączeniu (sterownik potrafi
 * przywrócić swoje wartości przy reassoc). W trybie AP działa tak samo.
 * ========================================================================== */
#pragma once
#include <WiFi.h>

// Maksymalna moc nadawania dostępna w API (wifi_power_t): 19,5 dBm.
// Wyżej nie da się wejść - to sufit sterownika, nie ograniczenie projektu.
#define WIFI_TX_POWER_MAX  WIFI_POWER_19_5dBm

inline void wifiApplyMaxPower() {
  WiFi.setSleep(false);                   // WIFI_PS_NONE - brak usypiania modemu
  WiFi.setTxPower(WIFI_TX_POWER_MAX);     // pełna moc nadawania
}

// Pełna moc nadawania + wybór trybu oszczędzania energii modemu.
// Uwaga: przy powerSave=true sterownik włącza WIFI_PS_MIN_MODEM - mniejszy
// pobór (~0,2-0,3 W), ale pingi rosną do 100-400 ms i mogą ginąć pakiety.
// Dlatego domyślnie wyłączone; włączane ustawieniem "wifi_ps" w configu.
// dbm: moc nadawania; 19.5 = maksimum. Przy RSSI > -65 dBm 13-15 dBm wystarcza.
// Obniżenie mocy ogranicza tylko kierunek stacja -> router (odbiór bez zmian).
inline void wifiApplyPower(bool powerSave, float dbm = 19.5f) {
  int q = (int)(dbm * 4.0f + 0.5f);          // wifi_power_t = dBm * 4
  if (q < 8 * 4) q = 8 * 4;
  if (q > 19 * 4 + 2) q = 19 * 4 + 2;        // 19.5 dBm = maksimum sterownika
  WiFi.setTxPower((wifi_power_t)q);
  WiFi.setSleep(powerSave);
}
