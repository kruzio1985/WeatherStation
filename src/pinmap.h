/* =============================================================================
 * Stacja Pogody - stacja meteorologiczna na ESP32-S3 WROOM-1 (N16R8)
 * Copyright (c) 2026 kruzio1985 - https://github.com/kruzio1985
 * Licencja: Stacja Pogody Non-Commercial License 1.0 (plik LICENSE)
 * Użytek niekomercyjny. Kontakt: kruzio1985@users.noreply.github.com
 * =============================================================================
 */
#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

// ------------------------------------------------------------
//  Mapa pinów zmieniana z przeglądarki (zakładka "Piny").
//
//  Wartości w include/pins.h są tylko wartościami DOMYŚLNYMI.
//  Zmiany zapisują się w pamięci trwałej (NVS + kopia /config.json)
//  w sekcji "pins" i obowiązują po restarcie stacji (moduły czytają
//  przypisania w begin()).
// ------------------------------------------------------------

struct PinRole {
  const char* key;          // klucz w konfiguracji ("i2c_sda")
  const char* label;        // nazwa funkcji na stronie
  const char* group;        // grupa (I2C, karta SD, ...)
  const char* desc;         // co podłączyć / na co uważać
  int8_t def;               // wartość domyślna z include/pins.h
  bool analog;              // czy wymaga wejścia analogowego (ADC1)
};

struct PinCheck {
  bool ok = true;           // false = nie wolno zapisać
  bool warning = false;     // true = zapis możliwy, ale ryzykowny
  String message;
};

class PinMap {
public:
  void begin();                                  // wczytaj przypisania z konfiguracji
  int count() const;
  const PinRole* role(size_t i) const;
  const PinRole* find(const char* key) const;
  int pin(const char* key) const;                // -1 = wyłączone / brak funkcji

  bool set(const char* key, int gpio, String& err);      // pojedyncze przypisanie
  void resetToDefaults();                                // tylko w pamięci
  bool save();                                           // zapis ustawień (NVS + kopia)

  PinCheck check(const char* key, int gpio) const;       // weryfikacja bez zapisu
  String problems() const;                               // lista konfliktów ("" = brak)

  String toJson() const;                                 // GET /api/pins
  bool applyJson(const char* json, size_t len, String& err);  // POST /api/pins

private:
  int8_t values_[64] = {0};
  bool loaded_ = false;

  int indexOf(const char* key) const;
  PinCheck checkIndex(int idx, int gpio, const int8_t* values) const;
  void conflictsInto(JsonArray arr, const int8_t* values) const;
  String conflictsText(const int8_t* values) const;
  void writeConfig();
};

extern PinMap pinMap;

// ------------------------------------------------------------
//  Dostęp do tabeli funkcji pinów (używane przez katalog płytek
//  w src/board.cpp, żeby policzyć domyślne piny dla każdego szablonu).
// ------------------------------------------------------------
size_t pinMapRoleCount();
const char* pinMapRoleKey(size_t i);
int pinMapRoleDef(size_t i);
