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
