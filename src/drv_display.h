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
//
//  Widok (zakładka "Wyświetlacz TFT"):
//    disp_mode (0..3) - 0 = pulpit, 1 = cykl dużych wskazań, 2 = jedna wartość,
//                       3 = pogoda (grafika + wartości)
//    disp_ch          - lista identyfikatorów kanałów, np. "temp,hum,press,pm25"
//                       (tekst; używana przez tryby 1, 2 i 3)
//    disp_step_s (s)  - co ile sekund zmienia się wskazanie w trybie cyklu
//    disp_font (0..5) - rozmiar cyfr: 0 = auto, 1 = Font4, 2 = Font6,
//                       3 = Font8, 4 = Font7 (7-segment), 5 = Font2 (małe)
//    disp_night(0/1)  - nocne przygaszenie kolorów wg godzin
//    disp_nf / disp_nt (godz.) - od/do której trwa "noc" (domyślnie 21 -> 7)
//    disp_ic_x / disp_ic_y (px) - położenie grafiki pogody
//    disp_i_sun/cloud/rain/snow/fog/night - ścieżka BMP na karcie SD
//                       (np. "/tft/slonce.bmp"; brak pliku = grafika rysowana)
//
//  Uwaga: moduł GMT024-10 nie wyprowadza pinu podświetlenia, więc "tryb nocny"
//  ściemnia KOLORY (tło czarne + stonowane barwy), a nie samo podświetlenie.
// =============================================================

// Grafiki pogody - kolejność kluczy "disp_i_*" i rysowanych zamienników.
enum : uint8_t {
  DISP_IC_SUN = 0, DISP_IC_CLOUD, DISP_IC_RAIN, DISP_IC_SNOW, DISP_IC_FOG, DISP_IC_NIGHT,
  DISP_IC_COUNT
};

class DisplayManager {
public:
  void begin();                 // inicjalizacja wg mapy pinów i ustawień
  void loop();                  // odświeżanie z odstępem "disp_int"
  void applyConfig();           // ponowna inicjalizacja po zmianie pinów/ustawień

  bool enabled();
  void setEnabled(bool on);
  uint32_t intervalS();
  void setIntervalS(uint32_t seconds);

  // Pozwala wczytać grafiki z karty od nowa (np. po wgraniu pliku BMP przez
  // /api/sd/upload) - bez ponownej inicjalizacji panelu.
  void reloadIcons();
  // Test polskich znaków: rysuje napis z diakrytykami i bez nich do bufora
  // i liczy zapalone piksele (więcej = znaki naprawdę się rysują).
  String plTest();

  String json();                                   // GET /api/display
  bool applyJson(const char* json, size_t len);    // POST /api/display

private:
  void draw();
  // Układy widoków - wszystkie dostają gotową paletę i wymiary LOGICZNE,
  // dzięki czemu działają i w poziomie (320x240), i w pionie (240x320).
  void drawDashboard(void* d, int W, int H, uint16_t bg, uint16_t fg, uint16_t dim, uint16_t acc, const char* hhmm);
  void drawBig(void* d, int W, int H, uint16_t bg, uint16_t fg, uint16_t dim, uint16_t acc, const char* hhmm, bool cycle);
  void drawWeather(void* d, int W, int H, uint16_t bg, uint16_t fg, uint16_t dim, uint16_t acc, const char* hhmm);
  void release();
  bool pinsOk();
  const char* icName() const;
  uint16_t width() const;
  uint16_t height() const;

  // Tryb 3: który obrazek pokazać - na podstawie opadu/mgły/zachmurzenia
  // i pory dnia (kolejność: deszcz, śnieg, mgła, chmury, noc, słońce).
  uint8_t weatherState();
  bool isNight();
  // Grafika z karty SD: ładuje BMP raz, trzyma w PSRAM; false = rysowana zastępczo.
  void* iconSprite(uint8_t state);
  void freeIcons();
  // Liczba kanałów w "disp_ch" (rozdzielone przecinkami).
  uint8_t channelCount();
  String channelAt(uint8_t idx);

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

  // --- Widok / tryby ---
  uint8_t  mode_ = 0;               // 0 = pulpit, 1 = cykl, 2 = jedna wartość, 3 = pogoda
  uint8_t  fontMode_ = 0;           // rozmiar cyfr dużych wskazań (0 = auto)
  uint32_t stepMs_ = 5000;          // co ile zmienia się wskazanie w cyklu
  String   chList_;                 // kanały dla trybów 1/2/3
  uint8_t  idx_ = 0;                // bieżący kanał w cyklu
  // --- Noc (brak pinu podświetlenia: ściemniamy kolory) ---
  bool     nightOn_ = false;        // przygaszanie włączone w ustawieniach
  uint8_t  nightFrom_ = 21, nightTo_ = 7;
  bool     nightNow_ = false;       // wyliczone dla bieżącej klatki
  float    nightK_ = 1.0f;          // mnożnik przygaszenia kolorów (tryb nocny)
  // --- Grafiki pogody z karty SD ---
  int16_t  icX_ = 8, icY_ = 56;
  String   icPath_[DISP_IC_COUNT];
  void*    icSprite_[DISP_IC_COUNT] = {nullptr};
  bool     icTried_[DISP_IC_COUNT] = {false};
};

extern DisplayManager display;
