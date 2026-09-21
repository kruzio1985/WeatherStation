/* =============================================================================
 * Stacja Pogody - stacja meteorologiczna na ESP32-S3 WROOM-1 (N16R8)
 * Copyright (c) 2026 kruzio1985 - https://github.com/kruzio1985
 * Licencja: Stacja Pogody Non-Commercial License 1.0 (plik LICENSE)
 * Użytek niekomercyjny. Kontakt: kruzio1985@users.noreply.github.com
 * =============================================================================
 */
// Raporty pogodowe: min / średnia / max dla kluczowych wskaźników, liczone
// bezpośrednio z plików CSV na karcie SD. Wspiera grupowanie po dniu,
// tygodniu lub miesiącu. Używane przez zakładkę "Raporty" na stronie www.
#pragma once
#include <Arduino.h>

class ReportService {
public:
  // period: "day" | "week" | "month"
  String json(const String& period);
  String csv(const String& period);
};

extern ReportService reports;
