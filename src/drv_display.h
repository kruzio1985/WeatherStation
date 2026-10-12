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
//  Wyświetlacz TFT SPI (GMT024-10, kontroler ILI9341, 240x320).
//
//  Piny pochodzą z mapy pinów (zakładka "Piny" na stronie www):
//  tft_sck, tft_mosi, tft_cs, tft_dc, tft_rst. Zmiana pinów na stronie
//  nie wymaga przebudowy firmware - wystarczy zapisać i zrestartować.
//
//  Moduł ma WŁASNĄ magistralę SPI (SPI3), więc nie koliduje z kartą SD
//  ani z magistralą I2C (czujniki, PCA9548A).
//
//  Domyślnie wyłączony: gdy wyświetlacz jest wyłączony w ustawieniach
//  (extra "disp_on" = 1 włącza) albo którykolwiek pin jest -1, moduł nie
//  robi zupełnie nic - zero kosztu w pętli głównej.
//
//  Ustawienia trzymane w sekcji "extra" konfiguracji:
//    disp_on   (1/0)  - wyświetlacz włączony (domyślnie 0)
//    disp_int  (s)    - odstęp między odświeżeniami, 2..300 (domyślnie 10)
//    disp_rot  (0..3) - obrót ekranu (0/90/180/270 stopni)
//    disp_ic   (0..2) - kontroler: 0 = ILI9341, 1 = ST7789, 2 = ST7796
//    disp_offrot (0..7) - przesunięcie mapowania obrotu (4-7 = lustro)
//    disp_offx/disp_offy (px) - przesunięcie okna adresowego
//    disp_bgr  (0/1)  - kolejność składowych BGR zamiast RGB
//    disp_theme(0/1)  - 0 = ciemny, 1 = jasny
//    disp_inv  (0/1)  - odwrócenie kolorów (gdy moduł wyświetla negatyw)
// =============================================================

class DisplayManager {
public:
  void begin();                 // inicjalizacja wg mapy pinów i ustawień
  void loop();                  // odświeżanie z odstępem "disp_int"
  void applyConfig();           // ponowna inicjalizacja po zmianie pinów/ustawień

  bool enabled();
  void setEnabled(bool on);
  uint32_t intervalS();
  void setIntervalS(uint32_t seconds);

  String json();                                   // GET /api/display
  bool applyJson(const char* json, size_t len);    // POST /api/display

private:
  void draw();
  void release();
  bool pinsOk();
  const char* icName() const;
  uint16_t width() const;
  uint16_t height() const;

  void*    gfx_ = nullptr;      // LGFX_SPI<Panel...>* (typ ukryty przed nagłówkiem)
  void*    sprite_ = nullptr;   // LGFX_Sprite* - bufor klatki w PSRAM (brak migotania)
  uint8_t  gfxIc_ = 0;          // kontroler, dla którego utworzono gfx_
  bool     ready_ = false;      // sterownik zainicjalizowany
  int8_t   sck_ = -1, mosi_ = -1, cs_ = -1, dc_ = -1, rst_ = -1;
  uint8_t  ic_ = 0;             // 0 = ILI9341, 1 = ST7789 (GMT024-10), 2 = ST7796
  uint8_t  rot_ = 0;
  uint8_t  offrot_ = 0;         // 4-7 = warianty lustrzane (gdy obraz jest odwrócony)
  int16_t  offx_ = 0, offy_ = 0;
  bool     bgr_ = false;        // zamiana kolejności składowych BGR<->RGB
  bool     theme_ = false;      // false = ciemny
  bool     invert_ = false;
  uint32_t intervalMs_ = 10000;
  unsigned long lastDrawMs_ = 0;    // znacznik czasu ostatniego odświeżenia
  unsigned long lastDrawTook_ = 0;  // ile ms trwało rysowanie
  uint32_t draws_ = 0;
};

extern DisplayManager display;
