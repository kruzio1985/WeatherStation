/* =============================================================================
 * Stacja Pogody - stacja meteorologiczna na ESP32-S3 WROOM-1 (N16R8)
 * Copyright (c) 2026 kruzio1985 - https://github.com/kruzio1985
 * Licencja: PolyForm Noncommercial License 1.0.0 (plik LICENSE)
 * Użytek niekomercyjny. Kontakt: kruzio1985@users.noreply.github.com
 * =============================================================================
 */
#pragma once
#include <Arduino.h>

// =============================================================
//  Pierścień LED RGB (WS2812B / RGBIC, np. 36 diod, 48 mm)
//  Zasilanie 5V + GND z płytki, dane z jednego pinu GPIO.
//
//  Pierścień pokazuje stan pogody kolorem, a jasność można ustawić
//  ręcznie na stronie www (zakładka "Led RGB") - ustawienie jest
//  zapisywane w NVS (przetrwa aktualizację) i przywracane po restarcie.
// =============================================================

enum WeatherState : uint8_t {
  WS_OFF = 0,   // wyłączony
  WS_GOOD,      // ładnie, ciepło, bez opadów - zielony
  WS_MEDIUM,    // pogoda średnia, zachmurzenie - żółty
  WS_WIND,      // silny wiatr - bursztynowy
  WS_RAIN,      // deszcz - jasnoniebieski
  WS_STORM,     // burza - ciemnoniebieski
  WS_SNOW,      // śnieg / mróz (temperatura ujemna) - biały
  WS_FOG,       // mgła, bardzo duża wilgotność - szary
  WS_HEAT,      // upał - pomarańczowy
  WS_ERROR      // brak danych z czujników - fioletowy
};

class LedRing {
public:
  bool begin();                          // inicjalizacja wg config + pinMap
  void loop();                           // animacja + wyliczanie stanu pogody

  void applyConfig();                    // wczytaj jasność/liczbę diod/kolejność
  void reinit();                         // ponowne utworzenie paska (zmiana pinu/liczby diod)

  bool  enabled() const   { return enabled_; }
  uint8_t brightness() const { return brightness_; }
  void setEnabled(bool on);
  void setBrightness(uint8_t percent);   // 0..100

  WeatherState state() const { return state_; }
  String       stateName() const;        // nazwa stanu (własna albo domyślna)
  uint32_t     stateColor() const;       // 0xRRGGBB koloru stanu
  uint8_t      ledCount() const { return count_; }
  uint16_t     estimatedMilliAmps() const { return lastMa_; }   // z ostatniej klatki
  uint16_t     maxMilliAmps() const;     // najgorszy przypadek (wszystko na biało)

  // Odczyt ustawień do strony diagnostycznej
  bool         isReady() const { return ready_; }
  int          pin() const { return pin_; }
  bool         effects() const { return effects_; }
  bool         offAtNight() const { return offAtNight_; }
  uint16_t     maxMa() const { return maxMa_; }
  const char*  orderName() const;        // "GRB" / "RGB" / "BRG"
  const char*  stateReason() const { return lastReason_.c_str(); }

  // Wymuszenie stanu (podgląd kolorów z www); kPreviewNone = wróć do czujników
  void setPreview(int state);

  String toJson();                       // stan dla strony www + legenda kolorów

  static constexpr int kPreviewNone  = -1;
  static constexpr int kPreviewCycle = 200;   // przełącz wszystkie kolory po kolei

private:
  void evaluateWeather();                // stan pogody z aktualnych pomiarów
  void render(uint32_t now);
  void fill(uint32_t rgb);
  void setPixel(uint16_t i, uint32_t rgb, uint8_t scale);
  void scaleDown(uint32_t& r, uint32_t& g, uint32_t& b, uint8_t pct);
  uint8_t effPct(uint32_t now) const;    // jasność po uwzględnieniu nocy

  bool  enabled_ = true;
  bool  effects_ = true;
  bool  offAtNight_ = false;
  bool  ready_ = false;                  // sterownik (Adafruit_NeoPixel) działa
  uint8_t brightness_ = 60;
  uint8_t count_ = 36;
  uint8_t order_ = 0;                    // 0=GRB, 1=RGB, 2=BRG
  uint16_t maxMa_ = 900;
  int     pin_ = -1;
  int     preview_ = -1;

  WeatherState state_ = WS_ERROR;
  unsigned long lastEval_ = 0;
  unsigned long lastRender_ = 0;
  unsigned long previewUntil_ = 0;
  unsigned long initMs_ = 0;
  uint16_t lastMa_ = 0;
  String lastReason_;
};

extern LedRing ledRing;
