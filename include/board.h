/* =============================================================================
 * Stacja Pogody - stacja meteorologiczna na ESP32-S3 WROOM-1 (N16R8)
 * Copyright (c) 2026 kruzio1985 - https://github.com/kruzio1985
 * Licencja: Stacja Pogody Non-Commercial License 1.0 (plik LICENSE)
 * Użytek niekomercyjny. Kontakt: kruzio1985@users.noreply.github.com
 * =============================================================================
 */
#pragma once
#include <Arduino.h>

// =============================================================
//  Katalog płytek (board profiles)
//
//  Jedno firmware obsługuje kilka płytek. Dla każdej płytki jest
//  gotowy szablon pinów, więc przy konfiguracji drugiego ESP
//  (węzła po RS485) nie trzeba wpisywać numerów GPIO ręcznie -
//  wystarczy wybrać wersję płytki na stronie www.
//
//  Płytka, na której firmware faktycznie pracuje, jest wykrywana
//  w locie (chip + rozmiar flash + PSRAM); ta informacja jedzie
//  po RS485 do mastera, żeby dało się porównać wybrany szablon
//  z rzeczywistą płytką węzła.
// =============================================================

struct BoardPinDef {
  const char* key;   // nazwa funkcji pinu (jak w zakładce "Piny")
  int8_t      gpio;  // -1 = funkcja wyłączona na tej płytce
};

struct BoardProfile {
  const char* id;         // "s3n16r8"
  const char* label;      // pełna nazwa do wyboru na stronie
  const char* shortName;  // krótka nazwa ("S3 N16R8")
  const char* chip;       // "esp32s3" / "esp32c3" / "esp32c6"
  uint8_t     flashMb;
  const char* ram;        // opis PSRAM
  const char* env;        // środowisko PlatformIO
  const char* note;       // krótka uwaga (co warto wiedzieć o pinach)
  bool        buildable;  // czy publikujemy gotowy plik firmware
  const BoardPinDef* pins;  // nadpisania domyślnych pinów (nullptr = pins.h)
  uint8_t     pinCount;
};

// -------------------------------------------------------------
//  Fizyczny układ pinów na płytce (zakładka "Piny")
//
//  Kolejność taka, jak na laminacie: najpierw cała lewa kolumna
//  złącza (od góry do dołu), potem cała prawa. Dzięki temu lista
//  na stronie zgadza się z płytką, a przy każdym pinie można
//  wybrać funkcję i od razu widzieć jej opis.
// -------------------------------------------------------------
struct BoardPad {
  uint8_t     col;    // 1 = lewa kolumna (J1), 2 = prawa (J3)
  int8_t      gpio;   // -1 = zasilanie / masa / RST (pin bez GPIO)
  const char* label;  // napis na płytce: "3V3", "G", "6", "TX", "EN"
  const char* note;   // krótka uwaga (nullptr = uwaga z gpioNote())
};

struct BoardLayout {
  const char* kind;   // "s3-devkitc1"
  const char* title;  // "ESP32-S3-DevKitC-1"
  const BoardPad* pads;
  uint16_t    padCount;
};

namespace boards {

size_t count();
const BoardProfile& at(size_t i);
const BoardProfile* byId(const char* id);

// Płytka, na której działa to firmware (wykrywana automatycznie).
const BoardProfile& current();
bool isCurrent(const char* id);
const char* chipName();          // "ESP32-S3" / "ESP32-C3" / "ESP32-C6"
const char* chipId();            // "esp32s3" / "esp32c3" / "esp32c6"

// Domyślny pin funkcji: najpierw szablon płytki, potem wartość z pins.h.
int defaultPin(const char* key, int fallback);
int defaultPinOf(const BoardProfile& p, const char* key, int fallback);

// Kontrola pinów zależna od chipu (nieistniejące / flash-PSRAM / strapping).
const char* reservedReason(int gpio);
const char* warningReason(const char* key, int gpio);
const char* gpioNote(int gpio);
bool pinExists(int gpio);
int  maxPin();
bool analogOk(int gpio);          // czy pin należy do ADC1 tego chipu
const char* analogHint();         // "GPIO 1-10 (ADC1)"
const char* adc2Note();           // ostrzeżenie o ADC2 (albo "" gdy brak ADC2)

// Fizyczny układ pinów płytki, na której działa firmware.
// nullptr = brak potwierdzonego układu (wtedy strona pokazuje listę GPIO).
const BoardLayout* layout();
const BoardLayout* layoutForChip(const char* chipId);

// JSON-y do strony www.
String defsJson(const BoardProfile& p);        // {"i2c_sda":8,...}
String catalogJson();                          // katalog + wykryta płytka + pliki
String currentJson();                          // tylko wykryta płytka

}  // namespace boards
