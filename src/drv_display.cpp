/* =============================================================================
 * Stacja Pogody - stacja meteorologiczna na ESP32-S3 WROOM-1 (N16R8)
 * Copyright (c) 2026 kruzio1985 - https://github.com/kruzio1985
 * Licencja: PolyForm Noncommercial License 1.0.0 (plik LICENSE)
 * Użytek niekomercyjny. Kontakt: kruzio1985@users.noreply.github.com
 * =============================================================================
 */
#include "drv_display.h"
// config.h definiuje domyślną rolę płytki (STACJA_ROLE_MASTER) - musi być
// włączony PRZED sprawdzeniem makra, inaczej nieokreślone makro = 0.
#include "config.h"

#if STACJA_ROLE_MASTER

#include "camera.h"
#include "extdev.h"
#include "mqtt_client.h"
#include "pinmap.h"
#include "sd_card.h"
#include "sensors.h"
#include "syslog.h"
#include <ArduinoJson.h>
#include <WiFi.h>
#include <math.h>
#include <time.h>

#define LGFX_USE_V1
#include <LovyanGFX.hpp>

DisplayManager display;

namespace {

// 20 MHz - bezpieczne dla ILI9341 i kilkunastocentymetrowych przewodów.
// Podniesienie do 40 MHz skraca pełny redraw, ale wymaga krótkiego okablowania.
constexpr uint32_t TFT_SPI_HZ = 20000000;

// Panel jest szablonem, bo moduły 2,4" 240x320 trafiają się z różnymi
// kontrolerami (GMT024-10 bywa na ILI9341 albo ST7789). Zły kontroler objawia
// się lustrem, przesunięciem i paskiem szumu - dlatego układ wybiera się
// na stronie www (extra "disp_ic"), bez przebudowy firmware.
template <typename PanelT>
class LGFX_SPI : public lgfx::LGFX_Device {
  PanelT        panel_;
  lgfx::Bus_SPI bus_;
public:
  LGFX_SPI(int sck, int mosi, int cs, int dc, int rst, bool invert, bool bgr,
           uint8_t offrot, int offx, int offy, uint16_t w, uint16_t h) {
    auto b = bus_.config();
    b.spi_host    = SPI3_HOST;      // osobny kontroler niż SPI2 (Arduino SPI = karta SD)
    b.spi_mode    = 0;
    b.freq_write  = TFT_SPI_HZ;
    b.freq_read   = 8000000;
    b.spi_3wire   = false;
    b.use_lock    = true;
    b.dma_channel = 1;
    b.pin_sclk    = sck;
    b.pin_mosi    = mosi;
    b.pin_miso    = -1;             // moduł GMT024-10 nie wyprowadza MISO
    b.pin_dc      = dc;
    bus_.config(b);
    panel_.setBus(&bus_);

    auto p = panel_.config();
    p.pin_cs         = cs;
    p.pin_rst        = rst;         // -1 = brak linii resetu (LovyanGFX to obsługuje)
    p.pin_busy       = -1;
    p.panel_width    = w;
    p.panel_height   = h;
    p.offset_x       = offx;
    p.offset_y       = offy;
    p.offset_rotation = offrot;     // 4-7 = warianty lustrzane (gdy obraz jest odwrócony)
    p.readable       = false;       // zapis jednokierunkowy
    p.invert         = invert;
    p.rgb_order      = bgr;
    panel_.config(p);
    setPanel(&panel_);
  }
};

// Liczba z przecinkiem (jak na stronie www) albo "---", gdy brak odczytu.
String num(float v, uint8_t dec) {
  if (isnan(v)) return "---";
  char b[16];
  dtostrf(v, 0, dec, b);
  String s(b);
  s.replace('.', ',');
  return s;
}

// Przygaszenie koloru RGB565 (mnożnik 0..1). Moduł GMT024-10 nie wyprowadza
// pinu podświetlenia, więc "tryb nocny" ściemnia barwy, a nie samo światło.
uint16_t dim565(uint16_t c, float k) {
  if (k >= 0.999f) return c;
  uint16_t r = (uint16_t)(((c >> 11) & 0x1F) * k + 0.5f);
  uint16_t g = (uint16_t)(((c >>  5) & 0x3F) * k + 0.5f);
  uint16_t b = (uint16_t)(( c        & 0x1F) * k + 0.5f);
  return (uint16_t)((r << 11) | (g << 5) | b);
}

// 0xRRGGBB -> RGB565 (kolory zapisane tak, jak w legendzie na stronie www).
uint16_t hex565(lgfx::LovyanGFX* d, uint32_t hex) {
  return d->color565((uint8_t)((hex >> 16) & 0xFF), (uint8_t)((hex >> 8) & 0xFF),
                     (uint8_t)(hex & 0xFF));
}

// Kolor wartości temperatury: poniżej +10 °C zimne błękity (im zimniej, tym
// ciemniej - przy minusie granat), powyżej od jasnożółtego przez pomarańczowy
// do czerwonego. Wartości dobrane tak, żeby na 2-3 m różnica była oczywista.
uint16_t tempColor565(lgfx::LovyanGFX* d, float c, float nk) {
  float r, g, b;
  if (c <= -15.0f)      { r =  10; g =  35; b = 115; }
  else if (c <   0.0f)  { const float k = (c + 15.0f) / 15.0f; r =  10 + k *  55; g =  35 + k * 105; b = 115 + k * 115; }
  else if (c <  10.0f)  { const float k = c / 10.0f;           r =  65 + k *  45; g = 140 + k *  60; b = 230 + k *  25; }
  else if (c <  20.0f)  { const float k = (c - 10.0f) / 10.0f; r = 235 + k *  20; g = 225 - k *  25; b = 120 - k *  60; }
  else if (c <  30.0f)  { const float k = (c - 20.0f) / 10.0f; r = 255;           g = 200 - k *  95; b =  60 - k *  30; }
  else                  { float k = (c - 30.0f) / 10.0f; if (k > 1) k = 1; r = 255 - k *  25; g = 105 - k *  50; b =  30 - k *  20; }
  return dim565(d->color565((uint8_t)r, (uint8_t)g, (uint8_t)b), nk);
}

// Progi barw jakości powietrza - dokładnie te same wartości hex, co pasek IAQ
// i legenda na stronie www (data/index.html, .aqi-track).
static const uint32_t AQ_BAND_HEX[6] = { 0x00C853, 0xFFD600, 0xFF9100, 0xE53935, 0x8E24AA, 0x7B1F1F };
static const float    AQ_BAND_STOP[7] = { 0, 50, 100, 150, 200, 300, 500 };

// Czcionka wg numeru z ustawień "disp_font" (0 = auto - wybiera setBigFont).
void setFontById(lgfx::LovyanGFX* d, uint8_t id) {
  switch (id) {
    case 1:  d->setFont(&lgfx::fonts::Font4); break;
    case 2:  d->setFont(&lgfx::fonts::Font6); break;
    case 3:  d->setFont(&lgfx::fonts::Font8); break;
    case 4:  d->setFont(&lgfx::fonts::Font7); break;   // 7-segment (tylko cyfry)
    case 5:  d->setFont(&lgfx::fonts::Font2); break;
    default: d->setFont(&lgfx::fonts::Font2); break;
  }
}

// Czcionka o zadanej wysokości w pikselach (16/26/48/75). 0 = auto (nie zmienia).
void setFontByPx(lgfx::LovyanGFX* d, int px) {
  switch (px) {
    case 16: d->setFont(&lgfx::fonts::Font2); break;
    case 26: d->setFont(&lgfx::fonts::Font4); break;
    case 48: d->setFont(&lgfx::fonts::Font6); break;
    case 75: d->setFont(&lgfx::fonts::Font8); break;
    default: break;
  }
}

// "#RRGGBB" (albo samo "RRGGBB") -> 0xRRGGBB; pusty napis = 0 (kolor domyślny).
uint32_t hexFromString(const String& s) {
  if (!s.length()) return 0;
  const char* p = s.c_str();
  if (*p == '#') p++;
  return (uint32_t)strtoul(p, nullptr, 16) & 0xFFFFFFu;
}

String hexString(uint32_t v) {
  char b[9];
  snprintf(b, sizeof(b), "#%06X", (unsigned)(v & 0xFFFFFFu));
  return String(b);
}

// Kolor elementu z ustawień: 0 = kolor domyślny, inaczej z przygaszeniem
// nocnym (nk), tak samo jak reszta ekranu.
uint16_t uiColor(lgfx::LovyanGFX* d, uint32_t hex, uint16_t def, float nk) {
  if (!hex) return def;
  return dim565(hex565(d, hex), nk);
}

// Szerokość napisu z polskimi znakami (definicja niżej - potrzebna już przy
// dobieraniu rozmiaru czcionki).
static int plWidth(lgfx::LovyanGFX* d, const String& s);

// Największa czcionka, w której tekst zmieści się na szerokość maxW.
void setBigFont(lgfx::LovyanGFX* d, const String& text, int maxW, uint8_t fontMode) {
  if (fontMode != 0) {
    // Font7 to czcionka 7-segmentowa: ma cyfry, kropkę i minus, ale NIE MA
    // przecinka, więc "17,2" wyszłoby jako "17 2". Przy przecinku bierzemy Font8.
    if (fontMode == 4 && text.indexOf(',') >= 0) {
      setFontById(d, 3);
      if (plWidth(d, text) > maxW) setBigFont(d, text, maxW, 0);   // nie mieści się? dobierz mniejszą
      return;
    }
    setFontById(d, fontMode);
    return;
  }
  static const uint8_t kTry[] = { 3, 2, 1, 5 };   // Font8 -> Font6 -> Font4 -> Font2
  for (uint8_t i = 0; i < sizeof(kTry); i++) {
    setFontById(d, kTry[i]);
    if (plWidth(d, text) <= maxW) return;
  }
}

// =============================================================
//  Polskie znaki na wyświetlaczu
//
//  Czcionki wbudowane w LovyanGFX (Font2/4/6/8, a także DejaVu z tej paczki -
//  zakres tylko 0x20-0x7E) nie mają liter ą/ć/ę/ł/ń/ó/ś/ź/ż, więc na ekranie
//  wychodziły puste miejsca ("Wilgotno  (AHT30)"). Pełna czcionka Unicode
//  (IPA/efont) to dziesiątki megabajtów źródła, dlatego rysujemy literę bazową
//  i dokładamy do niej znak diakrytyczny. Ta sama droga obsługuje °, µ, ², ³.
// =============================================================
enum : uint8_t { PL_NONE = 0, PL_ACUTE, PL_DOT, PL_OGONEK, PL_STROKE, PL_DEG, PL_MICRO, PL_SUP2, PL_SUP3 };

struct PlChar { uint16_t cp; char base; uint8_t mark; };

static const PlChar kPlTable[] = {
  {0x0104, 'A', PL_OGONEK}, {0x0105, 'a', PL_OGONEK},   // Ą ą
  {0x0106, 'C', PL_ACUTE},  {0x0107, 'c', PL_ACUTE},    // Ć ć
  {0x0118, 'E', PL_OGONEK}, {0x0119, 'e', PL_OGONEK},   // Ę ę
  {0x0141, 'L', PL_STROKE}, {0x0142, 'l', PL_STROKE},   // Ł ł
  {0x0143, 'N', PL_ACUTE},  {0x0144, 'n', PL_ACUTE},    // Ń ń
  {0x00D3, 'O', PL_ACUTE},  {0x00F3, 'o', PL_ACUTE},    // Ó ó
  {0x015A, 'S', PL_ACUTE},  {0x015B, 's', PL_ACUTE},    // Ś ś
  {0x0179, 'Z', PL_ACUTE},  {0x017A, 'z', PL_ACUTE},    // Ź ź
  {0x017B, 'Z', PL_DOT},    {0x017C, 'z', PL_DOT},      // Ż ż
  {0x00B0, ' ', PL_DEG},                                // °
  {0x00B5, 'u', PL_MICRO},                              // µ
  {0x00B2, ' ', PL_SUP2},                               // ²
  {0x00B3, ' ', PL_SUP3},                               // ³
};

// Kolejny znak UTF-8 (1-3 bajty; dłuższych nie używamy). Zawsze przesuwa "i".
static uint16_t plUtf8Next(const String& s, size_t& i) {
  const uint8_t c0 = (uint8_t)s[i];
  if (c0 < 0x80) { i++; return c0; }
  if ((c0 & 0xE0) == 0xC0 && i + 1 < s.length()) {
    const uint16_t cp = ((uint16_t)(c0 & 0x1F) << 6) | ((uint8_t)s[i + 1] & 0x3F);
    i += 2;
    return cp;
  }
  if ((c0 & 0xF0) == 0xE0 && i + 2 < s.length()) {
    const uint16_t cp = ((uint16_t)(c0 & 0x0F) << 12) | (((uint8_t)s[i + 1] & 0x3F) << 6) |
                        ((uint8_t)s[i + 2] & 0x3F);
    i += 3;
    return cp;
  }
  i++;
  return '?';
}

// Rysuje (albo tylko mierzy - rysujW = false) jeden znak i zwraca jego
// szerokość. "x/y" to lewy górny róg pola tekstowego.
static int plGlyph(lgfx::LovyanGFX* d, int x, int y, uint16_t cp,
                   uint16_t fg, uint16_t bg, bool draw) {
  char base = 0;
  uint8_t mark = PL_NONE;
  if (cp < 0x80) {
    base = (char)cp;
  } else {
    for (uint8_t i = 0; i < sizeof(kPlTable) / sizeof(kPlTable[0]); i++) {
      if (kPlTable[i].cp == cp) { base = kPlTable[i].base; mark = kPlTable[i].mark; break; }
    }
    if (!base) base = '?';
  }

  const int h = d->fontHeight();
  d->setTextDatum(lgfx::textdatum_t::top_left);

  // Znaki bez litery bazowej (stopień, indeksy górne): własny kształt.
  if (mark == PL_DEG || mark == PL_SUP2 || mark == PL_SUP3) {
    const int w = d->textWidth("o");
    if (mark == PL_DEG) {
      int r = h / 14;
      if (r < 1) r = 1;
      if (r > 4) r = 4;
      if (draw) d->fillCircle(x + w / 2, y + h / 5, r, fg);
      return w;
    }
    const char* digit = (mark == PL_SUP2) ? "2" : "3";
    const int wd = d->textWidth(digit);
    if (draw) {
      d->setTextColor(fg, bg);
      d->drawString(digit, x, y - h * 28 / 100);   // indeks górny
    }
    return wd;
  }

  const int w = d->textWidth(String(base).c_str());
  if (draw) {
    d->setTextColor(fg, bg);
    d->drawString(String(base).c_str(), x, y);
  }
  if (!draw || mark == PL_NONE) return w;

  // Znak diakrytyczny nad literą albo pod nią.
  const int lx  = x;
  const int ly  = y;
  const int lw  = w;
  const int lh  = h;
  switch (mark) {
    case PL_ACUTE:                      // ć ń ó ś ź - kreska nad literą
      d->drawLine(lx + lw * 62 / 100, ly + lh * 16 / 100,
                  lx + lw * 88 / 100, ly + lh * 3 / 100, fg);
      d->drawLine(lx + lw * 62 / 100, ly + lh * 17 / 100,
                  lx + lw * 88 / 100, ly + lh * 4 / 100, fg);
      break;
    case PL_DOT: {                      // ż - kropka nad literą
      int s = lh / 9;
      if (s < 2) s = 2;
      d->fillRect(lx + lw * 52 / 100, ly + lh * 4 / 100, s, s, fg);
      break;
    }
    case PL_OGONEK:                     // ą ę - ogonek pod literą
      d->drawLine(lx + lw * 60 / 100, ly + lh * 78 / 100,
                  lx + lw * 66 / 100, ly + lh * 93 / 100, fg);
      d->drawLine(lx + lw * 66 / 100, ly + lh * 93 / 100,
                  lx + lw * 88 / 100, ly + lh * 90 / 100, fg);
      break;
    case PL_STROKE:                     // ł - kreska przez literę
      d->drawLine(lx + lw * 5 / 100, ly + lh * 66 / 100,
                  lx + lw * 95 / 100, ly + lh * 32 / 100, fg);
      break;
    case PL_MICRO:                      // µ - "u" z ogonkiem po lewej
      d->drawLine(lx, ly + lh * 72 / 100, lx, ly + lh * 92 / 100, fg);
      d->drawLine(lx, ly + lh * 92 / 100, lx + lw * 25 / 100, ly + lh * 78 / 100, fg);
      break;
    default:
      break;
  }
  return w;
}

// Szerokość napisu z polskimi znakami.
static int plWidth(lgfx::LovyanGFX* d, const String& s) {
  int w = 0;
  for (size_t i = 0; i < s.length(); ) {
    w += plGlyph(d, 0, 0, plUtf8Next(s, i), 0, 0, false);
  }
  return w;
}

// Napis z polskimi znakami z wyrównaniem jak w drawString (datum).
static void plDraw(lgfx::LovyanGFX* d, int x, int y, const String& s,
                   uint16_t fg, uint16_t bg, lgfx::textdatum_t datum) {
  const int w = plWidth(d, s);
  const int h = d->fontHeight();
  int x0 = x, y0 = y;
  switch (datum) {
    case lgfx::textdatum_t::top_center:    x0 = x - w / 2;            break;
    case lgfx::textdatum_t::top_right:     x0 = x - w;                break;
    case lgfx::textdatum_t::middle_left:   y0 = y - h / 2;            break;
    case lgfx::textdatum_t::middle_center: x0 = x - w / 2; y0 = y - h / 2; break;
    case lgfx::textdatum_t::middle_right:  x0 = x - w;     y0 = y - h / 2; break;
    case lgfx::textdatum_t::bottom_left:   y0 = y - h;                break;
    case lgfx::textdatum_t::bottom_center: x0 = x - w / 2; y0 = y - h; break;
    case lgfx::textdatum_t::bottom_right:  x0 = x - w;     y0 = y - h; break;
    default: break;                                                   // top_left
  }
  for (size_t i = 0; i < s.length(); ) {
    x0 += plGlyph(d, x0, y0, plUtf8Next(s, i), fg, bg, true);
  }
}

static void plDraw(lgfx::LovyanGFX* d, int x, int y, const char* s,
                   uint16_t fg, uint16_t bg, lgfx::textdatum_t datum) {
  plDraw(d, x, y, String(s), fg, bg, datum);
}

// Ścieżki grafik pogody (klucze w konfiguracji) - kolejność jak DISP_IC_*.
const char* const kIconKeys[DISP_IC_COUNT] = {
  "disp_i_sun", "disp_i_cloud", "disp_i_rain", "disp_i_snow", "disp_i_fog", "disp_i_night"
};
const char* const kIconNames[DISP_IC_COUNT] = {
  "sun", "cloud", "rain", "snow", "fog", "night"
};

const char* weatherStateName(uint8_t st) {
  switch (st) {
    case DISP_IC_SUN:   return "Słonecznie";
    case DISP_IC_CLOUD: return "Zachmurzenie";
    case DISP_IC_RAIN:  return "Deszcz";
    case DISP_IC_SNOW:  return "Śnieg";
    case DISP_IC_FOG:   return "Mgła";
    default:            return "Noc";
  }
}

// Grafika zastępcza rysowana na ekranie - używana, gdy na karcie SD nie ma
// pliku BMP (albo jest nieczytelny). Dzięki temu tryb "pogoda" działa zawsze.
void drawIconVector(lgfx::LovyanGFX* d, uint8_t state, int x, int y, int s, uint16_t bg) {
  const uint16_t sun   = d->color565(250, 200, 40);
  const uint16_t cloud = d->color565(170, 180, 190);
  const uint16_t rain  = d->color565(70, 150, 240);
  const uint16_t snow  = d->color565(235, 245, 255);
  const uint16_t fog   = d->color565(140, 150, 160);
  const uint16_t moon  = d->color565(230, 230, 170);
  const int cx = x + s / 2;
  const int cy = y + s / 2;
  const int r  = s / 4;

  auto clouds = [&](int dy) {
    d->fillCircle(cx - r, cy + dy, r * 8 / 10, cloud);
    d->fillCircle(cx + r / 3, cy + dy - r / 3, r, cloud);
    d->fillRect(cx - r * 8 / 5, cy + dy, r * 16 / 10, r * 8 / 10, cloud);
  };

  switch (state) {
    case DISP_IC_SUN:
      d->fillCircle(cx, cy, r, sun);
      for (int i = 0; i < 8; i++) {
        const float a = i * (float)PI / 4.0f;
        d->drawLine(cx + (int)(cosf(a) * (r + 3)), cy + (int)(sinf(a) * (r + 3)),
                    cx + (int)(cosf(a) * (r + r / 2)), cy + (int)(sinf(a) * (r + r / 2)), sun);
      }
      break;
    case DISP_IC_CLOUD:
      clouds(0);
      break;
    case DISP_IC_RAIN:
      clouds(-r / 3);
      for (int i = -1; i <= 1; i++)
        d->drawLine(cx + i * r, cy + r, cx + i * r - r / 3, cy + r + r, rain);
      break;
    case DISP_IC_SNOW:
      clouds(-r / 3);
      for (int i = -1; i <= 1; i++) {
        const int sx = cx + i * r, sy = cy + r + r / 2;
        d->drawLine(sx - 5, sy, sx + 5, sy, snow);
        d->drawLine(sx, sy - 5, sx, sy + 5, snow);
      }
      break;
    case DISP_IC_FOG:
      for (int i = 0; i < 4; i++)
        d->drawFastHLine(cx - s / 3, y + s / 5 + i * (s / 12), s * 2 / 3, fog);
      break;
    default:                                  // noc: sierp księżyca
      d->fillCircle(cx, cy, r, moon);
      d->fillCircle(cx + r / 2, cy - r / 3, r, bg);
      d->drawPixel(cx - r, cy - r, moon);
      d->drawPixel(cx + r / 2, cy + r, moon);
      d->drawPixel(cx - r / 2, cy + r / 2, moon);
      break;
  }
}

}  // namespace

bool DisplayManager::enabled() {
  return config.extraF("disp_on", 0.0f) >= 0.5f;
}

void DisplayManager::setEnabled(bool on) {
  config.setExtraF("disp_on", on ? 1.0f : 0.0f);
  config.save();
  applyConfig();
}

uint32_t DisplayManager::intervalS() {
  float s = config.extraF("disp_int", 10.0f);
  if (s < 2.0f) s = 2.0f;
  if (s > 300.0f) s = 300.0f;
  return (uint32_t)(s + 0.5f);
}

void DisplayManager::setIntervalS(uint32_t seconds) {
  if (seconds < 2) seconds = 2;
  if (seconds > 300) seconds = 300;
  config.setExtraF("disp_int", (float)seconds);
  config.save();
  applyConfig();
}

// Grafiki z karty zostaną wczytane od nowa przy najbliższym odświeżeniu
// (np. po wgraniu pliku BMP przez /api/sd/upload). Bez reinicjalizacji panelu.
void DisplayManager::reloadIcons() {
  freeIcons();
}

// Kontrola polskich znaków bez patrzenia na ekran: ten sam napis rysujemy
// z diakrytykami i bez nich, po czym liczymy zapalone piksele. Jeśli wersja
// z diakrytykami ma ich więcej, znaki faktycznie się dokładają.
String DisplayManager::plTest() {
  JsonDocument o;
  if (!gfx_) {
    o["error"] = "wyświetlacz nieaktywny";
    String s; serializeJson(o, s); return s;
  }
  lgfx::LGFX_Sprite sp(static_cast<lgfx::LGFX_Device*>(gfx_));
  sp.setPsram(true);
  sp.setColorDepth(16);

  const int W = 320, H = 40;
  auto countPixels = [&](uint8_t font, const char* txt) -> int {
    if (!sp.createSprite(W, H)) return -1;
    sp.fillScreen(0x0000);
    setFontById(&sp, font);
    plDraw(&sp, 2, 2, txt, 0xFFFF, 0x0000, lgfx::textdatum_t::top_left);
    int n = 0;
    for (int y = 0; y < H; y++)
      for (int x = 0; x < W; x++)
        if (sp.readPixel(x, y) != 0) n++;
    sp.deleteSprite();
    return n;
  };

  const int plain  = countPixels(1, "Wilgotnosc");
  const int accent = countPixels(1, "Wilgotność");
  const int plain2 = countPixels(1, "Slonecznie Mgla O");
  const int acc2   = countPixels(1, "Słonecznie Mgła Ó");
  o["font"]         = 1;
  o["plain"]        = plain;
  o["accent"]       = accent;
  o["extra_px"]     = accent - plain;
  o["plain2"]       = plain2;
  o["accent2"]      = acc2;
  o["extra_px2"]    = acc2 - plain2;
  o["ok"]           = (accent > plain && acc2 > plain2);
  String s;
  serializeJson(o, s);
  return s;
}

bool DisplayManager::pinsOk() {
  return sck_ >= 0 && mosi_ >= 0 && cs_ >= 0 && dc_ >= 0;
}

const char* DisplayManager::icName() const {
  switch (ic_) {
    case 1:  return "ST7789";
    case 2:  return "ST7796";
    default: return "ILI9341";
  }
}

uint16_t DisplayManager::width() const  { return ic_ == 2 ? 320 : 240; }
uint16_t DisplayManager::height() const { return ic_ == 2 ? 480 : 320; }

void DisplayManager::release() {
  freeIcons();
  if (sprite_) {
    delete static_cast<lgfx::LGFX_Sprite*>(sprite_);
    sprite_ = nullptr;
  }
  if (gfx_) {
    // Rodzaj panelu jest zaszyty w typie obiektu, więc usuwamy po tym typie,
    // jakim obiekt faktycznie utworzyliśmy (gfxIc_), a nie po bieżącym ic_.
    switch (gfxIc_) {
      case 1:  delete static_cast<LGFX_SPI<lgfx::Panel_ST7789>*>(gfx_);  break;
      case 2:  delete static_cast<LGFX_SPI<lgfx::Panel_ST7796>*>(gfx_);  break;
      default: delete static_cast<LGFX_SPI<lgfx::Panel_ILI9341>*>(gfx_); break;
    }
    gfx_ = nullptr;
  }
  ready_ = false;
}

void DisplayManager::begin() {
  applyConfig();
}

void DisplayManager::applyConfig() {
  sck_  = pinMap.pin("tft_sck");
  mosi_ = pinMap.pin("tft_mosi");
  cs_   = pinMap.pin("tft_cs");
  dc_   = pinMap.pin("tft_dc");
  rst_  = pinMap.pin("tft_rst");

  rot_    = (uint8_t)(((int)config.extraF("disp_rot", 0.0f)) & 3);
  theme_  = config.extraF("disp_theme", 0.0f) >= 0.5f;
  invert_ = config.extraF("disp_inv", 0.0f) >= 0.5f;

  // --- Widok: tryb, kanały, czcionka, noc, grafiki pogody ---
  int md = (int)(config.extraF("disp_mode", 0.0f) + 0.5f);
  if (md < 0 || md > 3) md = 0;
  mode_ = (uint8_t)md;
  int fm = (int)(config.extraF("disp_font", 0.0f) + 0.5f);
  if (fm < 0 || fm > 5) fm = 0;
  fontMode_ = (uint8_t)fm;
  idx_ = 0;
  float st = config.extraF("disp_step_s", 5.0f);
  if (st < 2.0f) st = 2.0f;
  if (st > 120.0f) st = 120.0f;
  stepMs_ = (uint32_t)(st * 1000.0f + 0.5f);
  chList_ = config.extraS("disp_ch", "temp,hum,press,pm25,pm10");
  nightOn_ = config.extraF("disp_night", 0.0f) >= 0.5f;
  int nf = (int)(config.extraF("disp_nf", 21.0f) + 0.5f);
  int nt = (int)(config.extraF("disp_nt", 7.0f) + 0.5f);
  if (nf < 0) nf = 0;
  if (nf > 23) nf = 23;
  if (nt < 0) nt = 0;
  if (nt > 23) nt = 23;
  nightFrom_ = (uint8_t)nf;
  nightTo_   = (uint8_t)nt;
  icX_ = (int16_t)(config.extraF("disp_ic_x", 8.0f) + 0.5f);
  icY_ = (int16_t)(config.extraF("disp_ic_y", 56.0f) + 0.5f);
  for (uint8_t i = 0; i < DISP_IC_COUNT; i++) {
    icPath_[i] = config.extraS(kIconKeys[i], "");
  }

  // Wygląd i pozycje elementów ekranu (zakładka www "Wyświetlacz TFT").
  // 0 / pusty kolor = wartość domyślna, więc stare konfiguracje wyglądają
  // dokładnie tak, jak dotąd.
  struct UiLoad { const char* p; UiCfg* u; };
  const UiLoad uis[] = { { "top", &uiTop_ }, { "lbl", &uiLbl_ }, { "val", &uiVal_ },
                         { "unit", &uiUnit_ }, { "info", &uiInfo_ }, { "wx", &uiWx_ } };
  for (uint8_t i = 0; i < sizeof(uis) / sizeof(uis[0]); i++) {
    const String k = String("disp_ui_") + uis[i].p + "_";
    UiCfg& u = *uis[i].u;
    u.size = (int)(config.extraF((k + "size").c_str(), 0.0f) + 0.5f);
    u.dx   = (int)(config.extraF((k + "dx").c_str(), 0.0f));
    u.dy   = (int)(config.extraF((k + "dy").c_str(), 0.0f));
    u.on   = config.extraF((k + "on").c_str(), 1.0f) >= 0.5f;
    u.col  = hexFromString(config.extraS((k + "col").c_str(), ""));
  }
  uiTopCam_ = config.extraF("disp_ui_top_cam", 1.0f) >= 0.5f;
  uiTopOut_ = config.extraF("disp_ui_top_out", 1.0f) >= 0.5f;
  uiTopIn_  = config.extraF("disp_ui_top_in", 1.0f) >= 0.5f;
  uiBarOn_  = config.extraF("disp_ui_bar_on", 1.0f) >= 0.5f;
  uiBarH_   = (int)(config.extraF("disp_ui_bar_h", 0.0f) + 0.5f);
  uiBarCol_ = hexFromString(config.extraS("disp_ui_bar_col", ""));
  uiWxIcon_ = (int)(config.extraF("disp_ui_wx_icon", 0.0f) + 0.5f);

  int ic = (int)(config.extraF("disp_ic", 0.0f) + 0.5f);
  if (ic < 0 || ic > 2) ic = 0;
  ic_ = (uint8_t)ic;
  int offrot = (int)(config.extraF("disp_offrot", 0.0f) + 0.5f);
  offrot_ = (uint8_t)(offrot & 7);
  offx_ = (int)(config.extraF("disp_offx", 0.0f) + 0.5f);
  offy_ = (int)(config.extraF("disp_offy", 0.0f) + 0.5f);
  bgr_  = config.extraF("disp_bgr", 0.0f) >= 0.5f;

  intervalMs_ = intervalS() * 1000UL;

  release();

  if (!enabled()) {
    LOG_I("Wyświetlacz TFT: wyłączony w ustawieniach (extra disp_on = 0)");
    return;
  }
  if (!pinsOk()) {
    LOG_W("Wyświetlacz TFT: piny nieustawione (SCK/MOSI/CS/DC) - pomijam");
    return;
  }

  switch (ic_) {
    case 1:
      gfx_ = new LGFX_SPI<lgfx::Panel_ST7789>(sck_, mosi_, cs_, dc_, rst_, invert_, bgr_,
                                              offrot_, offx_, offy_, 240, 320);
      break;
    case 2:
      gfx_ = new LGFX_SPI<lgfx::Panel_ST7796>(sck_, mosi_, cs_, dc_, rst_, invert_, bgr_,
                                              offrot_, offx_, offy_, 320, 480);
      break;
    default:
      gfx_ = new LGFX_SPI<lgfx::Panel_ILI9341>(sck_, mosi_, cs_, dc_, rst_, invert_, bgr_,
                                               offrot_, offx_, offy_, 240, 320);
      break;
  }
  gfxIc_ = ic_;   // typ obiektu - potrzebny w release()

  lgfx::LGFX_Device* g = static_cast<lgfx::LGFX_Device*>(gfx_);
  g->init();
  g->setRotation(rot_);
  ready_ = true;
  lastDrawMs_ = 0;
  draws_ = 0;

  LOG_I("Wyświetlacz TFT: gotowy (%s %ux%u, SPI3, SCK=%d MOSI=%d CS=%d DC=%d RST=%d, obrót %u st., offrot %u, offx %d, offy %d, BGR %d)",
        icName(), width(), height(), sck_, mosi_, cs_, dc_, rst_,
        (unsigned)(rot_ * 90), (unsigned)offrot_, offx_, offy_, (int)bgr_);
  LOG_I("Wyświetlacz TFT: widok %u (0 pulpit, 1 cykl, 2 jedna wartość, 3 pogoda), kanały: %s, czcionka %u, cykl %lu s, noc %d (%u-%u)",
        (unsigned)mode_, chList_.c_str(), (unsigned)fontMode_,
        (unsigned long)(stepMs_ / 1000), (int)nightOn_, (unsigned)nightFrom_, (unsigned)nightTo_);
  draw();
}

void DisplayManager::loop() {
  if (!ready_) return;
  unsigned long now = millis();
  // W trybie cyklu wskazania zmieniają się co "disp_step_s", więc odświeżanie
  // musi być szybsze niż "disp_int" - inaczej lista by stała w miejscu.
  const uint32_t period = (mode_ == 1 && channelCount() > 1) ? stepMs_ : intervalMs_;
  if (lastDrawMs_ != 0 && (now - lastDrawMs_) < period) return;
  lastDrawMs_ = now;
  // W cyklu przechodzimy do następnego kanału (pierwszy kanał pokazuje draw()
  // wywołane z applyConfig, więc tutaj przesuwamy od razu dalej).
  if (mode_ == 1) {
    const uint8_t n = channelCount();
    if (n > 1) idx_ = (uint8_t)((idx_ + 1) % n);
  }
  draw();
  draws_++;
}

// =============================================================
//  Pasek stanu urządzeń (góra) + pasek jakości powietrza (dół)
//
//  Stan "działa / nie działa" bierzemy WYŁĄCZNIE z tego, co urządzenia same
//  zgłaszają - nigdzie nie ma odpytywania wpisanego na sztywno adresu IP:
//   * węzły powietrza (ExtDevice typu "outaq" = na zewnątrz, np. stacja C3
//     .147, oraz "aq" = w środku, np. .141) w każdym
//     /api/status przysyłają swój czas pracy i stan MQTT, a master zapisuje
//     chwilę odebrania tej odpowiedzi (extdev: ok, lastMs, peerMs),
//   * kamera (moduł ESP32-CAM) przysyła zdjęcie na /api/camera/upload, więc
//     wystarczy pamiętać chwilę ostatnio odebranego zdjęcia (camera.lastIngestMs).
// =============================================================
enum : uint8_t { ST_OFF = 0, ST_OK = 1, ST_BAD = 2 };   // brak / działa / nie odpowiada

// Rozmiar kropki stanu rośnie razem z czcionką opisu (12 px przy 26 px tekstu),
// żeby kolor było widać z 2-3 m także przy większych znacznikach.
int stateDotR(uint8_t st, int fontH) {
  int r = fontH / 5;
  if (r < 4) r = 4;
  if (r > 10) r = 10;
  return (st == ST_OFF) ? (r > 5 ? r - 2 : 4) : r;
}

// Kamera: wyłączona w ustawieniach = ST_OFF (szara), brak zdjęcia w oknie
// 3 interwałów albo zgłoszony błąd = ST_BAD (czerwona).
uint8_t camState() {
  if (!camera.enabled()) return ST_OFF;
  const unsigned long last = camera.lastIngestMs();
  const uint32_t ivMin = config.camIntervalMin();
  // Okno oceny: 3 interwały zdjęć (min. 10 min), plus minutę zapasu na transmisję.
  const unsigned long window = (ivMin > 0 ? (unsigned long)ivMin * 180000UL
                                          : 10UL * 60000UL) + 60000UL;
  if (last && (millis() - last) <= window) return ST_OK;   // zdjęcie dotarło niedawno
  if (camera.error().length()) return ST_BAD;              // sterownik zgłasza problem
  if (camera.remoteUrl().length() > 0) {
    // Kamera zewnętrzna: jedynym dowodem, że działa, jest przysłane zdjęcie.
    if (last) return ST_BAD;
    return millis() < 300000UL ? ST_OFF : ST_BAD;          // pierwsze 5 min - jeszcze nie oceniamy
  }
  return camera.present() ? ST_OK : ST_BAD;                // kamera podłączona do mastera
}

// Węzeł powietrza danego typu: brak urządzenia w konfiguracji = ST_OFF,
// brak odpowiedzi albo odpowiedź starsza niż 3 interwały = ST_BAD. Typ
// ("aq" = powietrze w środku, "outaq" = powietrze na zewnątrz) i adres bierzemy
// z konfiguracji urządzenia - nic nie jest wpisane na sztywno.
uint8_t airStateByType(const char* type) {
  const unsigned long now = millis();
  for (int i = 0; i < extdev.count(); i++) {
    const ExtDevice* e = extdev.at(i);
    if (!e || e->type != type) continue;
    if (!e->ok || !e->lastMs) return ST_BAD;
    const unsigned long iv = (unsigned long)(e->interval > 0 ? e->interval : 30) * 1000UL;
    if (now - e->lastMs > iv * 3UL) return ST_BAD;         // milczy od trzech odczytów
    if (e->peerMs && now - e->peerMs > iv * 3UL) return ST_BAD;
    return ST_OK;
  }
  return ST_OFF;
}

// Węzeł powietrza na zewnątrz (np. stacja C3) i w środku - dwa osobne znaczniki.
uint8_t airNodeState() { return airStateByType("outaq"); }
uint8_t airInState()   { return airStateByType("aq"); }

// Kolor stanu (przygaszany w trybie nocnym razem z resztą ekranu).
uint16_t stateColor565(lgfx::LovyanGFX* d, uint8_t st, float nk, uint16_t dim) {
  if (st == ST_OK)  return dim565(d->color565( 70, 210,  95), nk);
  if (st == ST_BAD) return dim565(d->color565(235,  70,  55), nk);
  return dim;
}

// Szerokość znacznika: opis + kropka stanu NA KOŃCU opisu (nie przed nim).
int chipWidth(lgfx::LovyanGFX* d, const char* label, uint8_t st, uint8_t fontId, int fontH) {
  setFontById(d, fontId);
  return 10 + (int)d->textWidth(label) + 8 + stateDotR(st, fontH) * 2;
}

// Znacznik stanu: zaokrąglona ramka + opis wersalikami + kropka w kolorze stanu
// na końcu tekstu. txtCol = 0 znaczy "domyślny" (opis przygaszony, a przy braku
// łączności czerwony). Zwraca szerokość znacznika.
int chipDraw(lgfx::LovyanGFX* d, int x, int y, int h, const char* label, uint8_t st,
             uint16_t bg, uint16_t dim, float nk, uint8_t fontId, uint16_t txtCol) {
  setFontById(d, fontId);
  const int tw = (int)d->textWidth(label);
  const int fh = d->fontHeight();
  const int r  = stateDotR(st, fh);
  const int w  = chipWidth(d, label, st, fontId, fh);
  d->drawRoundRect(x, y, w, h, 6, dim);
  d->setTextDatum(lgfx::textdatum_t::middle_left);
  d->setTextColor(st == ST_BAD ? stateColor565(d, st, nk, dim) : (txtCol ? txtCol : dim), bg);
  d->drawString(label, x + 10, y + h / 2);
  d->fillCircle(x + 10 + tw + 8 + r, y + h / 2, r, stateColor565(d, st, nk, dim));
  d->setTextColor(dim, bg);
  return w;
}

// Górny pasek: znaczniki stanu węzłów (kamera, powietrze na zewnątrz, powietrze
// w środku). Co pokazać, jakim rozmiarem, gdzie i w jakim kolorze ustawia się
// w www - zakładka "Wyświetlacz TFT", sekcja "Wygląd i pozycje".
void DisplayManager::dispTopStatus(void* dv, int W, uint16_t bg, uint16_t fg, uint16_t dim, float nk) {
  lgfx::LovyanGFX* d = static_cast<lgfx::LovyanGFX*>(dv);
  if (!uiTop_.on) return;

  struct Chip { uint8_t st; const char* full; const char* shrt; };
  Chip c[3];
  uint8_t cnt = 0;
  if (uiTopCam_) c[cnt++] = { camState(),     "KAMERA", "KAM" };
  if (uiTopOut_) c[cnt++] = { airNodeState(), "C3",     "C3"  };
  if (uiTopIn_)  c[cnt++] = { airInState(),   "WEW",    "WEW" };
  if (!cnt) return;

  uint8_t fid = 1;                                  // Font4 = 26 px
  if (uiTop_.size == 16)      fid = 5;              // Font2
  else if (uiTop_.size == 48) fid = 2;              // Font6
  else if (uiTop_.size == 75) fid = 3;              // Font8

  // Gdy znaczniki nie mieszczą się na szerokość, najpierw skracamy opisy,
  // a dopiero potem zmniejszamy czcionkę.
  auto need = [&](uint8_t f, bool shrt) {
    setFontById(d, f);
    const int fh = d->fontHeight();
    int n = 6 + uiTop_.dx;
    for (uint8_t i = 0; i < cnt; i++) n += chipWidth(d, shrt ? c[i].shrt : c[i].full, c[i].st, f, fh) + 6;
    return n;
  };
  bool shrt = false;
  if (need(fid, false) > W - 4) {
    shrt = true;
    if (need(fid, true) > W - 4) fid = 5;
  }
  setFontById(d, fid);
  const int h = d->fontHeight() + 8;
  const int y = 4 + uiTop_.dy;
  const uint16_t txtCol = uiColor(d, uiTop_.col, 0, nk);
  int x = 6 + uiTop_.dx;
  for (uint8_t i = 0; i < cnt; i++) {
    x += chipDraw(d, x, y, h, shrt ? c[i].shrt : c[i].full, c[i].st, bg, dim, nk, fid, txtCol) + 6;
  }
  d->drawFastHLine(8, y + h + 4, W - 16, txtCol ? txtCol : dim);
}

// Dolny wiersz informacyjny (tuż nad paskiem jakości): zegar po lewej, licznik
// cyklu po prawej, a na pulpicie na środku skrót stanu sieci.
void DisplayManager::dispBottomInfo(void* dv, int W, int H, uint16_t bg, uint16_t dim,
                    const char* hhmm, const char* counter, const char* center) {
  lgfx::LovyanGFX* d = static_cast<lgfx::LovyanGFX*>(dv);
  if (!uiInfo_.on) return;
  uint8_t fid = 1;
  if (uiInfo_.size == 16)      fid = 5;
  else if (uiInfo_.size == 48) fid = 2;
  else if (uiInfo_.size == 75) fid = 3;
  setFontById(d, fid);
  const int y = H - 26 + uiInfo_.dy;
  const uint16_t col = uiColor(d, uiInfo_.col, dim, nightK_);
  plDraw(d, 8 + uiInfo_.dx, y, hhmm, col, bg, lgfx::textdatum_t::middle_left);
  const int cw = (int)d->textWidth(hhmm);
  int rw = 0;
  if (counter && counter[0]) {
    rw = (int)d->textWidth(counter);
    plDraw(d, W - 8 + uiInfo_.dx, y, counter, col, bg, lgfx::textdatum_t::middle_right);
  }
  // Środek tylko wtedy, gdy naprawdę zostaje na niego miejsce (i zostaje mały:
  // to informacja dodatkowa).
  if (center && center[0]) {
    d->setFont(&lgfx::fonts::Font2);
    const int avail = W - 16 - cw - rw - 24;
    if ((int)d->textWidth(center) <= avail) {
      plDraw(d, W / 2 + uiInfo_.dx, y, center, col, bg, lgfx::textdatum_t::middle_center);
    }
  }
}

// PM2.5 -> AQI (progi EPA, te same co legenda na stronie) - tylko na wypadek,
// gdy węzeł nie poda gotowego AQI.
float pm2Aqi(float pm) {
  static const float bp[7] = { 0.0f, 12.1f, 35.5f, 55.5f, 150.5f, 250.5f, 500.5f };
  static const float ai[7] = { 0.0f,  50.0f, 100.0f, 150.0f, 200.0f, 300.0f, 500.0f };
  if (pm <= 0) return 0;
  for (int i = 1; i < 7; i++) {
    if (pm <= bp[i]) {
      return ai[i - 1] + (pm - bp[i - 1]) * (ai[i] - ai[i - 1]) / (bp[i] - bp[i - 1]);
    }
  }
  return 500.0f;
}

// AQI do paska: najpierw gotowe AQI z węzła C3, potem jego PM2.5, na końcu AQI
// liczone z czujników mastera.
float airAqi() {
  const float a = sensors.valueOf("c3_aqi");
  if (!isnan(a)) return a;
  const float pm = sensors.valueOf("c3_pm25");
  if (!isnan(pm)) return pm2Aqi(pm);
  return sensors.valueOf("aqi");
}

// Dolny pasek: te same progi i kolory, co legenda jakości powietrza na stronie
// www (0-50 zielony ... 300+ bordowy), rozciągnięte na całą szerokość ekranu.
// Słupek pokazuje, gdzie jest aktualny pomiar.
void DisplayManager::dispBottomBar(void* dv, int W, int H, uint16_t bg, uint16_t fg, float nk) {
  lgfx::LovyanGFX* d = static_cast<lgfx::LovyanGFX*>(dv);
  if (!uiBarOn_) return;
  int bh = uiBarH_ > 0 ? uiBarH_ : 12;      // wysokość pasa z ustawień www
  if (bh < 4)  bh = 4;
  if (bh > 48) bh = 48;
  const int y  = H - bh;
  for (int i = 0; i < 6; i++) {
    int x0 = (int)((float)W * AQ_BAND_STOP[i] / AQ_BAND_STOP[6]);
    int x1 = (int)((float)W * AQ_BAND_STOP[i + 1] / AQ_BAND_STOP[6]);
    if (x0 < 0) x0 = 0;
    if (x1 > W) x1 = W;
    if (x1 > x0) d->fillRect(x0, y, x1 - x0, bh, dim565(hex565(d, AQ_BAND_HEX[i]), nk));
  }
  const float aqi = airAqi();
  if (isnan(aqi)) return;                   // brak pomiaru - zostaje sam pas legendy
  float k = aqi / AQ_BAND_STOP[6];
  if (k < 0) k = 0;
  if (k > 1) k = 1;
  const int mx = (int)(k * (float)(W - 1));
  const int bx = (mx > 3) ? mx - 3 : 0;
  const uint16_t mark = uiColor(d, uiBarCol_, fg, nk);   // kolor znacznika z ustawień
  d->fillRect(bx, y - 6, 7, bh + 6, mark);   // słupek znacznika
  d->drawRect(bx, y - 6, 7, bh + 6, bg);     // obwódka w kolorze tła (czytelna na każdym pasie)
}

void DisplayManager::draw() {
  if (!gfx_) return;
  lgfx::LGFX_Device* g = static_cast<lgfx::LGFX_Device*>(gfx_);
  // Wymiary LOGICZNE po setRotation() - dla panelu zamontowanego "w poziomie"
  // (rotate 1/3) są to 320x240 i układ musi się do tego dopasować, inaczej
  // zostaje czarny pas z boku ekranu.
  const int W = (int)g->width();
  const int H = (int)g->height();
  const int right = W - 8;

  // Bufor klatki w PSRAM: rysujemy całość poza ekranem i wypychamy jednym
  // zapisem, dzięki czemu nie ma migotania (wcześniej było czyszczenie
  // całego ekranu i rysowanie od zera - każde odświeżenie "mrugało").
  lgfx::LGFX_Sprite* sp = static_cast<lgfx::LGFX_Sprite*>(sprite_);
  if (sp && ((int)sp->width() != W || (int)sp->height() != H)) {
    delete sp;
    sp = nullptr;
    sprite_ = nullptr;
  }
  if (!sp) {
    sp = new lgfx::LGFX_Sprite(g);
    sp->setPsram(true);
    sp->setColorDepth(16);
    if (!sp->createSprite(W, H)) {
      delete sp;                    // brak PSRAM? rysujemy wprost na panel
      sp = nullptr;
    }
    sprite_ = sp;
  }
  lgfx::LovyanGFX* d = sp ? static_cast<lgfx::LovyanGFX*>(sp)
                          : static_cast<lgfx::LovyanGFX*>(g);

  // "Noc" (disp_night): moduł GMT024-10 nie ma wyprowadzonego pinu
  // podświetlenia, więc przygaszenie robimy na kolorach - wymuszamy ciemne
  // tło i stonowane barwy (patrz komentarz w nagłówku).
  nightNow_ = isNight();
  const bool light = theme_ && !nightNow_;
  const float nk = nightNow_ ? 0.38f : 1.0f;
  nightK_ = nk;                                   // kolorów używają też widoki
  const uint16_t bg  = light ? (uint16_t)0xFFFF : (uint16_t)0x0000;
  const uint16_t fg  = light ? (uint16_t)0x0000 : dim565(0xFFFF, nk);
  const uint16_t dim = dim565(d->color565(130, 140, 150), nk);
  const uint16_t acc = dim565(d->color565(56, 189, 248), nightNow_ ? 0.45f : 1.0f);

  // Godzina jest wspólna dla wszystkich widoków.
  time_t now = time(nullptr);
  char hhmm[8] = "--:--";
  if (now > 1600000000) {
    struct tm tmv;
    localtime_r(&now, &tmv);
    strftime(hhmm, sizeof(hhmm), "%H:%M", &tmv);
  }

  unsigned long t0 = millis();
  d->startWrite();
  d->fillScreen(bg);

  // Górny pasek stanu (kamera + węzeł powietrza na zewnątrz + węzeł powietrza
  // w środku) jest wspólny dla wszystkich widoków - stale widoczny i duży,
  // żeby z 2-3 m było widać, czy urządzenia żyją. Zegar i licznik cyklu są
  // przeniesione na dół, nad pasek jakości powietrza (patrz dispBottomInfo).
  String cyc;
  if (mode_ == 1) {
    const uint8_t n = channelCount();
    if (n > 1) cyc = String((unsigned)(idx_ % n) + 1) + "/" + String(n);
  }
  dispTopStatus(d, W, bg, fg, dim, nk);

  switch (mode_) {
    case 1:  drawBig(d, W, H, bg, fg, dim, acc, hhmm, true);   break;  // cykl wskazań
    case 2:  drawBig(d, W, H, bg, fg, dim, acc, hhmm, false);  break;  // jedna wartość
    case 3:  drawWeather(d, W, H, bg, fg, dim, acc, hhmm);     break;  // pogoda + grafika
    default: drawDashboard(d, W, H, bg, fg, dim, acc, hhmm);   break;  // pulpit
  }

  // Dolny wiersz: zegar (lewo), stan sieci na pulpicie (środek), licznik cyklu
  // (prawo) - tuż nad paskiem jakości powietrza.
  String net;
  if (mode_ == 0) {
    net = String("WiFi ") + (WiFi.status() == WL_CONNECTED ? "OK" : "--") + "  MQTT " +
          (mqtt.connected() ? "OK" : "--");
  }
  dispBottomInfo(d, W, H, bg, dim, hhmm, cyc.length() ? cyc.c_str() : nullptr,
                 net.length() ? net.c_str() : nullptr);

  // Dolny pasek jakości powietrza - kolory i progi jak legenda na stronie www.
  dispBottomBar(d, W, H, bg, fg, nk);

  d->endWrite();

  // Jedno wypchnięcie gotowej klatki na panel (bez migotania).
  if (sp) sp->pushSprite(0, 0);

  lastDrawTook_ = millis() - t0;
  if (draws_ < 2 || lastDrawTook_ > 250) {
    LOG_I("Wyświetlacz TFT: odswiezanie %lu ms", lastDrawTook_);
  }
}

// ---------------- Widok 0: pulpit (dotychczasowy układ) ----------------
// Układ zależy od wymiarów LOGICZNYCH po obrocie (nie od modelu panelu):
// 320x240 = panel zamontowany w poziomie, 240x320 = w pionie.
void DisplayManager::drawDashboard(void* dv, int W, int H, uint16_t bg, uint16_t fg, uint16_t dim,
                                   uint16_t acc, const char* hhmm) {
  lgfx::LovyanGFX* d = static_cast<lgfx::LovyanGFX*>(dv);
  const int right = W - 8;

  const float t   = sensors.valueOf("temp");
  const float h   = sensors.valueOf("hum");
  const float p   = sensors.valueOf("press");
  float w         = sensors.valueOf("vev_wind");
  if (isnan(w)) w = sensors.valueOf("wind");
  const float pm  = sensors.valueOf("c3_pm25");
  const float aqi = sensors.valueOf("c3_aqi");

  // --- Treść widoku. Wspólny nagłówek (stan urządzeń) rysuje draw(), a skrót
  //     stanu sieci trafia do dolnego wiersza (patrz dispBottomInfo). ---

  if (W >= H) {
    // ---------------- UKŁAD POZIOMY (np. panel zamontowany w poziomie) ----------------
    const int colX = W * 55 / 100;          // granica kolumn
    d->drawFastVLine(colX, 34, H - 48, dim);

    // Lewa kolumna: duża temperatura (kolor wg skali: błękit = zimno, żółty =
    // ciepło, czerwień = upał) + wilgotność i wiatr pod nią
    d->setTextColor(fg, bg);
    if (uiWx_.size) setFontByPx(d, uiWx_.size);
    else            d->setFont(&lgfx::fonts::Font6);
    plDraw(d, 12 + uiWx_.dx, 34 + uiWx_.dy, num(t, 1),
           uiColor(d, uiWx_.col, isnan(t) ? fg : tempColor565(d, t, nightK_), nightK_),
           bg, lgfx::textdatum_t::top_left);
    d->setFont(&lgfx::fonts::Font2);
    plDraw(d, 12, 88, "°C", acc, bg, lgfx::textdatum_t::top_left);

    auto leftRow = [&](int y, const char* label, const String& value) {
      const int yy = y + uiWx_.dy;
      d->setFont(&lgfx::fonts::Font2);
      plDraw(d, 12 + uiWx_.dx, yy, label, dim, bg, lgfx::textdatum_t::top_left);
      if (uiWx_.size) setFontByPx(d, uiWx_.size);
      else            d->setFont(&lgfx::fonts::Font4);
      plDraw(d, 12 + uiWx_.dx, yy + 18, value, uiColor(d, uiWx_.col, fg, nightK_), bg,
             lgfx::textdatum_t::top_left);
    };
    leftRow(110, "Wilgotność", num(h, 0) + " %");
    leftRow(156, "Wiatr",      num(w, 1) + " km/h");

    // Prawa kolumna: ciśnienie, pył i AQI z C3
    auto rightRow = [&](int y, const char* label, const String& value, uint16_t color) {
      const int yy = y + uiWx_.dy;
      d->setFont(&lgfx::fonts::Font2);
      plDraw(d, colX + 10 + uiWx_.dx, yy, label, dim, bg, lgfx::textdatum_t::top_left);
      if (uiWx_.size) setFontByPx(d, uiWx_.size);
      else            d->setFont(&lgfx::fonts::Font4);
      plDraw(d, right + uiWx_.dx, yy + 16, value, uiColor(d, uiWx_.col, color, nightK_), bg,
             lgfx::textdatum_t::top_right);
    };
    rightRow(40,  "Ciśnienie",   num(p, 1) + " hPa",   fg);
    rightRow(86,  "PM2.5 (C3)",  num(pm, 0) + " µg/m³", acc);
    rightRow(132, "AQI (C3)",    num(aqi, 0),           acc);
  } else {
    // ---------------- UKŁAD PIONOWY (240x320) ----------------
    const int cx = W / 2;

    if (uiWx_.size) setFontByPx(d, uiWx_.size);
    else            d->setFont(&lgfx::fonts::Font6);
    plDraw(d, cx + uiWx_.dx, 40 + uiWx_.dy, num(t, 1),
           uiColor(d, uiWx_.col, isnan(t) ? fg : tempColor565(d, t, nightK_), nightK_),
           bg, lgfx::textdatum_t::top_center);
    d->setFont(&lgfx::fonts::Font2);
    plDraw(d, cx, 92, "°C", acc, bg, lgfx::textdatum_t::top_center);
    d->drawFastHLine(8, 112, W - 16, dim);

    const int rowTop  = 140;
    const int rowStep = (H - rowTop - 46) / 4;
    auto row = [&](int idx, const char* label, const String& value, uint16_t color) {
      const int y = rowTop + idx * rowStep + uiWx_.dy;
      d->setFont(&lgfx::fonts::Font2);
      plDraw(d, 12 + uiWx_.dx, y, label, dim, bg, lgfx::textdatum_t::middle_left);
      if (uiWx_.size) setFontByPx(d, uiWx_.size);
      else            d->setFont(&lgfx::fonts::Font4);
      plDraw(d, right + uiWx_.dx, y, value, uiColor(d, uiWx_.col, color, nightK_), bg,
             lgfx::textdatum_t::middle_right);
    };
    row(0, "Wilgotność",  num(h, 0) + " %",        fg);
    row(1, "Ciśnienie",   num(p, 1) + " hPa",      fg);
    row(2, "Wiatr",       num(w, 1) + " km/h",     fg);
    row(3, "PM2.5 (C3)",  num(pm, 0) + " µg/m³",   acc);
    row(4, "AQI (C3)",    num(aqi, 0),             acc);
  }
}

// ---------------- Widoki 1 i 2: duże wskazania ----------------
// Krótkie opisy dla dużego widoku. Nazwy kanałów z węzłów bywają długie i z
// obcymi znakami (np. "Umidità (sonda presenza)"), a na 320 px taki napis
// zjechałby do 16 px, czyli był nieczytelny z 2-3 m. Dla znanych kanałów mamy
// więc własne, krótkie napisy - a nieznane skracamy i czścimy z "?".
struct ChLabel { const char* id; const char* label; };
static const ChLabel kChLabel[] = {
  { "temp", "TEMPERATURA" },      { "temp2", "TEMP. 2" },        { "hum", "WILGOTNOŚĆ" },
  { "press", "CIŚNIENIE" },       { "light", "ŚWIATŁO" },        { "rain", "OPAD" },
  { "wind", "WIATR" },            { "vane", "KIERUNEK" },        { "co2", "CO2" },
  { "aqi", "AQI" },               { "pm1", "PM1" },              { "pm25", "PM2,5" },
  { "pm10", "PM10" },             { "tvoc", "TVOC" },            { "eco2", "eCO2" },
  { "uv", "UV" },                 { "soil", "GLEBA" },           { "solar_wm2", "NASŁONECZ." },
  { "vev_temp", "TEMP. VEVOR" },  { "vev_hum", "WILG. VEVOR" },  { "vev_wind", "WIATR" },
  { "vev_gust", "PORYW" },        { "vev_vane", "KIERUNEK" },    { "vev_uv", "UV" },
  { "vev_light", "ŚWIATŁO" },     { "vev_rain_now", "OPAD TERAZ" },
  { "vev_rain_day", "OPAD DZIŚ" },{ "vev_rain_total", "OPAD SUM." },
  { "c3_temp", "TEMP. ZEWN." },   { "c3_hum", "WILG. ZEWN." },   { "c3_press", "CIŚN. ZEWN." },
  { "c3_dew", "PUNKT ROSY" },     { "c3_heat", "ODCZUWALNA" },   { "c3_alt", "WYSOKOŚĆ" },
  { "c3_aqi", "AQI ZEWN." },      { "c3_pm1", "PM1 ZEWN." },     { "c3_pm25", "PM2,5 ZEWN." },
  { "c3_pm10", "PM10 ZEWN." },    { "c3_pm25_60m", "PM2,5 (1 h)" },
  { "in_air_t", "TEMP. WEWN." },  { "in_air_h", "WILG. WEWN." }, { "in_air_t24", "TEMP. 24 h" },
  { "in_air_h24", "WILG. 24 h" }, { "in_air_dp", "PUNKT ROSY" }, { "in_air_t_ext", "TEMP. SONDY" },
  { "in_air_h_ext", "WILG. SONDY" }, { "in_rh_corr", "WILG. POPR." },
  { "in_pm1", "PM1 WEWN." },      { "in_pm25", "PM2,5 WEWN." },  { "in_pm4", "PM4 WEWN." },
  { "in_pm10", "PM10 WEWN." },    { "in_iaq", "IAQ WEWN." },     { "in_tvoc_lvl", "TVOC WEWN." },
  { "in_motion", "RUCH" },        { "in_mics_v", "MiCS" },       { "in_mq7_v", "MQ-7" },
  { "pw_bat_soc", "BATERIA" },    { "pw_ina219_v", "NAPIĘCIE" }, { "pw_ina219_i", "PRĄD" },
  { "pw_ina219_p", "MOC" },       { "cam_lux", "LUX (KAMERA)" }, { "cam_weather", "POGODA (KAMERA)" },
};

// Znaki, których nie ma w czcionkach ekranu, zamieniamy na literę bazową
// (np. włoskie "à" -> "a"), a zupełnie obce pomijamy. Bez tego plGlyph rysuje
// dla każdego takiego znaku znak zapytania - na ekranie wychodziły "artefakty".
static char latinBase(uint16_t cp) {
  switch (cp) {
    case 0x00C0: case 0x00C1: case 0x00C2: case 0x00C3: case 0x00C4: case 0x00C5: return 'A';
    case 0x00E0: case 0x00E1: case 0x00E2: case 0x00E3: case 0x00E4: case 0x00E5: return 'a';
    case 0x00C7: return 'C';   case 0x00E7: return 'c';
    case 0x00C8: case 0x00C9: case 0x00CA: case 0x00CB: return 'E';
    case 0x00E8: case 0x00E9: case 0x00EA: case 0x00EB: return 'e';
    case 0x00CC: case 0x00CD: case 0x00CE: case 0x00CF: return 'I';
    case 0x00EC: case 0x00ED: case 0x00EE: case 0x00EF: return 'i';
    case 0x00D1: return 'N';   case 0x00F1: return 'n';
    case 0x00D2: case 0x00D4: case 0x00D5: case 0x00D6: case 0x00D8: return 'O';
    case 0x00F2: case 0x00F4: case 0x00F5: case 0x00F6: case 0x00F8: return 'o';
    case 0x00D9: case 0x00DA: case 0x00DB: case 0x00DC: return 'U';
    case 0x00F9: case 0x00FA: case 0x00FB: case 0x00FC: return 'u';
    case 0x00DD: return 'Y';   case 0x00FD: case 0x00FF: return 'y';
    case 0x00DF: return 's';
    default: return 0;
  }
}

// Napis bezpieczny dla czcionek ekranu: polskie znaki zostają (plDraw rysuje do
// nich diakrytyki), obce diakrytyki sprowadzamy do liter bazowych, a resztę
// (symbole, emoji) pomijamy.
static String plSafe(const String& in) {
  String out;
  out.reserve(in.length());
  for (size_t i = 0; i < in.length(); ) {
    const size_t start = i;
    const uint16_t cp = plUtf8Next(in, i);
    if (cp < 0x80) { out += (char)cp; continue; }
    bool polish = false;
    for (uint8_t k = 0; k < sizeof(kPlTable) / sizeof(kPlTable[0]); k++) {
      if (kPlTable[k].cp == cp) { polish = true; break; }
    }
    if (polish) { out += in.substring(start, i); continue; }
    const char base = latinBase(cp);
    if (base) out += base;
  }
  return out;
}

// Opis kanału do dużego wskazania: krótki napis z tabeli, a dla nieznanego
// kanału - oczyszczona i skrócona nazwa z definicji czujnika.
static String channelLabel(const String& id, const String& name) {
  for (uint8_t i = 0; i < sizeof(kChLabel) / sizeof(kChLabel[0]); i++) {
    if (id == kChLabel[i].id) return String(kChLabel[i].label);
  }
  if (id.startsWith("ds_")) return String("TEMP. ZEWN. ") + id.substring(3);
  if (id.startsWith("ths_")) return String("CZUJNIK ") + id.substring(4);
  if (id.startsWith("ads_")) return String("ADC ") + id.substring(4);
  if (id.startsWith("mot_")) return String("RUCH ") + id.substring(4);
  String s = plSafe(name);
  s.toUpperCase();
  if (s.length() > 20) s = s.substring(0, 20);
  return s;
}

// Tryb 1 pokazuje kolejne kanały z "disp_ch" (zmiana co "disp_step_s"),
// tryb 2 - pierwszy kanał na stałe. Nazwa kanału i jednostka biorą się
// z definicji kanału, więc nic nie trzeba wpisywać ręcznie.
void DisplayManager::drawBig(void* dv, int W, int H, uint16_t bg, uint16_t fg, uint16_t dim,
                             uint16_t acc, const char* hhmm, bool cycle) {
  lgfx::LovyanGFX* d = static_cast<lgfx::LovyanGFX*>(dv);
  const uint8_t n = channelCount();

  // Nagłówek (stan urządzeń + zegar) rysuje draw() - wspólny dla wszystkich
  // widoków, a licznik cyklu jest na górnym pasku. Tutaj od razu treść.
  if (n == 0) {
    d->setFont(&lgfx::fonts::Font4);
    plDraw(d, W / 2, H / 2, "Brak kanałów w disp_ch", dim, bg,
           lgfx::textdatum_t::middle_center);
    return;
  }

  const String id = channelAt(cycle ? (uint8_t)(idx_ % n) : 0);
  String name, unit;
  uint8_t dec = 1;
  if (!sensors.channelMeta(id, name, unit, dec)) {
    name = id;
    unit = "";
  }
  const float raw = sensors.valueOf(id);
  const String val = num(raw, dec);

  // Kolor wartości: temperatura wg skali (poniżej 10 C błękity - im zimniej, tym
  // ciemniej, przy minusie granat; powyżej żółty -> pomarańcz -> czerwień).
  // Pozostałe kanały zostają jasne jak dotąd, brak odczytu = kolor neutralny.
  const bool tempCh = (id.indexOf("temp") >= 0) ||
                      (unit.indexOf("C") >= 0 && unit.indexOf("%") < 0);
  const uint16_t valCol = (!isnan(raw) && tempCh) ? tempColor565(d, raw, nightK_) : fg;

  // Opis TEGO, co pokazują cyfry. Rozmiar, przesunięcie i kolor biorą się
  // z ustawień www (zakładka TFT -> "Wygląd i pozycje"), domyślnie: Font6 (48 px),
  // a gdy się nie mieści - Font4 (26 px) ze skróceniem napisu. Poniżej Font4 nie
  // schodzimy, bo z 2-3 m byłoby to nieczytelne.
  String label = channelLabel(id, name);
  const int lblMaxW = W - 24 - abs(uiLbl_.dx);
  if (uiLbl_.size) {
    setFontByPx(d, uiLbl_.size);
  } else {
    d->setFont(&lgfx::fonts::Font6);
    if (plWidth(d, label) > lblMaxW) d->setFont(&lgfx::fonts::Font4);
  }
  while (label.length() > 3 && plWidth(d, label) > lblMaxW) label.remove(label.length() - 1);
  plDraw(d, W / 2 + uiLbl_.dx, H * 30 / 100 + uiLbl_.dy, label,
         uiColor(d, uiLbl_.col, acc, nightK_), bg, lgfx::textdatum_t::middle_center);

  // Jednostka: rozmiar i kolor z ustawień (domyślnie 26 px, kolor akcentu).
  const String unitSafe = plSafe(unit);
  if (uiUnit_.size) setFontByPx(d, uiUnit_.size);
  else              d->setFont(&lgfx::fonts::Font4);
  const int unitW = unitSafe.length() ? plWidth(d, unitSafe) : 0;
  const int gapW  = unitW ? 10 : 0;
  const int maxV  = W - 16 - unitW - gapW;

  // Rozmiar cyfr: z ustawień; bez ustawienia - największa czcionka, w której
  // liczba mieści się razem z jednostką (Font8 -> Font6 -> Font4).
  static const uint8_t kTry[] = { 3, 2, 1 };
  if (uiVal_.size) {
    setFontByPx(d, uiVal_.size);
    if (plWidth(d, val) > maxV) {
      for (uint8_t i = 0; i < sizeof(kTry); i++) {
        setFontById(d, kTry[i]);
        if (plWidth(d, val) <= maxV) break;
      }
    }
  } else if (fontMode_ != 0) {
    setBigFont(d, val, maxV, fontMode_);            // czcionka wymuszona w ustawieniach
  } else {
    for (uint8_t i = 0; i < sizeof(kTry); i++) {
      setFontById(d, kTry[i]);
      if (plWidth(d, val) <= maxV) break;
    }
  }
  const int vw   = plWidth(d, val);
  const int x0   = (W - (vw + unitW + gapW)) / 2 + uiVal_.dx;   // środek + przesunięcie
  const int valY = H * 62 / 100 + uiVal_.dy;
  plDraw(d, x0, valY, val, uiColor(d, uiVal_.col, valCol, nightK_), bg,
         lgfx::textdatum_t::middle_left);
  if (unitW) {
    if (uiUnit_.size) setFontByPx(d, uiUnit_.size);
    else              d->setFont(&lgfx::fonts::Font4);
    plDraw(d, x0 + vw + gapW, valY, unitSafe, uiColor(d, uiUnit_.col, acc, nightK_), bg,
           lgfx::textdatum_t::middle_left);
  }
}

// ---------------- Widok 3: pogoda (grafika + wartości) ----------------
void DisplayManager::drawWeather(void* dv, int W, int H, uint16_t bg, uint16_t fg, uint16_t dim,
                                 uint16_t acc, const char* hhmm) {
  lgfx::LovyanGFX* d = static_cast<lgfx::LovyanGFX*>(dv);
  const uint8_t st = weatherState();
  const bool wide = (W >= H);
  int side = wide ? (H * 45 / 100) : (W * 55 / 100);
  if (uiWxIcon_) {                                  // rozmiar grafiki z ustawień (%)
    side = side * uiWxIcon_ / 100;
    const int lim = wide ? (H - 40) : (W - 20);
    if (side < 40) side = 40;
    if (side > lim) side = lim;
  }

  int ix = icX_;
  int iy = icY_;
  if (!wide) {                       // w pionie grafika idzie na środek u góry
    ix = (W - side) / 2;
    iy = 40;
  }

  // Nagłówek (stan urządzeń + zegar) rysuje draw() - wspólny dla wszystkich
  // widoków. Nazwa stanu pogody jest w widoku pionowym pod grafiką, a w
  // poziomym widać ją z grafiki obok wartości.

  void* isp = iconSprite(st);
  if (isp) {
    static_cast<lgfx::LGFX_Sprite*>(isp)->pushSprite(d, ix, iy);
  } else {
    drawIconVector(d, st, ix, iy, side, bg);   // brak pliku na karcie = rysujemy
  }

  const uint8_t n = channelCount();
  int ty = wide ? 44 : (iy + side + 8);
  if (wide && n == 0) {
    // Bez wybranych kanałów: sama grafika + nazwa stanu na środku
    d->setFont(&lgfx::fonts::Font4);
    plDraw(d, ix + side + 14, iy + side / 2, weatherStateName(st), fg, bg,
           lgfx::textdatum_t::middle_left);
    return;
  }
  if (!wide) {
    d->setFont(&lgfx::fonts::Font4);
    plDraw(d, W / 2, ty, weatherStateName(st), fg, bg, lgfx::textdatum_t::top_center);
    ty += 34;
  }
  const int tx = (wide ? (ix + side + 14) : 12) + uiWx_.dx;
  for (uint8_t i = 0; i < n && i < 4; i++) {
    const String id = channelAt(i);
    String name, unit;
    uint8_t dec = 1;
    if (!sensors.channelMeta(id, name, unit, dec)) continue;
    // Opis jak w dużym widoku: krótki napis bez znaków, których nie ma czcionka
    // (obce nazwy z węzłów rysowały się jako "?").
    d->setFont(&lgfx::fonts::Font2);
    plDraw(d, tx, ty + uiWx_.dy, channelLabel(id, name), dim, bg, lgfx::textdatum_t::top_left);
    // Temperatura wg skali kolorów, pozostałe wartości w kolorze akcentu.
    const float raw = sensors.valueOf(id);
    const bool tempCh = (id.indexOf("temp") >= 0) ||
                        (unit.indexOf("C") >= 0 && unit.indexOf("%") < 0);
    const uint16_t vc = (!isnan(raw) && tempCh) ? tempColor565(d, raw, nightK_) : acc;
    if (uiWx_.size) setFontByPx(d, uiWx_.size);
    else            d->setFont(&lgfx::fonts::Font4);
    plDraw(d, W - 8 + uiWx_.dx, ty + uiWx_.dy, num(raw, dec),
           uiColor(d, uiWx_.col, vc, nightK_), bg, lgfx::textdatum_t::top_right);
    if (unit.length()) {
      d->setFont(&lgfx::fonts::Font2);
      plDraw(d, tx, ty + 22 + uiWx_.dy, plSafe(unit), dim, bg, lgfx::textdatum_t::top_left);
    }
    ty += 38;
  }
}

// ---------------- Tryby: noc i stan pogody ----------------

// Noc wg zegara RTC (disp_nf -> disp_nt, przedział może przechodzić północ).
bool DisplayManager::isNight() {
  if (!nightOn_) return false;
  time_t now = time(nullptr);
  if (now < 1600000000) return false;          // brak czasu - nie udajemy nocy
  struct tm tmv;
  localtime_r(&now, &tmv);
  const int h = tmv.tm_hour;
  if (nightFrom_ == nightTo_) return false;
  if (nightFrom_ < nightTo_) return h >= nightFrom_ && h < nightTo_;
  return h >= nightFrom_ || h < nightTo_;
}

// Kolejność rozpoznawania: deszcz, śnieg, mgła, chmury, noc, słońce.
// Źródła: opad z deszczomierza, analiza obrazu z kamery (cam_*), wilgotność,
// wiatr i temperatura - brak kanału po prostu pomija jego warunek.
uint8_t DisplayManager::weatherState() {
  const float rain     = sensors.valueOf("rain");
  const float camRain  = sensors.valueOf("cam_rain");
  const float camSnow  = sensors.valueOf("cam_snow");
  const float camFog   = sensors.valueOf("cam_fog");
  const float camCloud = sensors.valueOf("cam_cloud");
  const float t        = sensors.valueOf("temp");
  const float h        = sensors.valueOf("hum");
  const float w        = sensors.valueOf("wind");

  if ((!isnan(rain) && rain > 0.2f) || (!isnan(camRain) && camRain >= 50.0f)) return DISP_IC_RAIN;
  if ((!isnan(camSnow) && camSnow >= 50.0f) ||
      (!isnan(t) && t <= 0.5f && !isnan(h) && h >= 90.0f))                    return DISP_IC_SNOW;
  if ((!isnan(camFog) && camFog >= 50.0f) ||
      (!isnan(h) && h >= 97.0f && !isnan(w) && w < 8.0f))                     return DISP_IC_FOG;
  if (!isnan(camCloud) && camCloud >= 60.0f)                                  return DISP_IC_CLOUD;
  if (isNight())                                                              return DISP_IC_NIGHT;
  return DISP_IC_SUN;
}

// ---------------- Grafiki pogody z karty SD ----------------
// BMP (np. 100x100) ładujemy RAZ do bufora w PSRAM - dzięki temu w pętli nie
// ma już żadnego odczytu z karty. Nieczytelny/nieobecny plik = grafika rysowana.
void* DisplayManager::iconSprite(uint8_t state) {
  if (state >= DISP_IC_COUNT) return nullptr;
  if (icSprite_[state]) return icSprite_[state];
  if (icTried_[state]) return nullptr;
  icTried_[state] = true;

  const String& p = icPath_[state];
  if (!p.length()) return nullptr;                      // brak wpisanej ścieżki
  if (!sdCard.mounted() || !sdCard.exists(p)) {
    LOG_W("Wyświetlacz TFT: brak grafiki %s na karcie - rysuję zastępczą", p.c_str());
    return nullptr;
  }

  File f = sdCard.openRead(p);
  if (!f) return nullptr;
  uint8_t hdr[26];
  const int got = f.read(hdr, sizeof(hdr));
  f.close();
  if (got != (int)sizeof(hdr) || hdr[0] != 'B' || hdr[1] != 'M') {
    LOG_W("Wyświetlacz TFT: %s to nie BMP (wymagany format BMP 24-bit)", p.c_str());
    return nullptr;
  }
  int32_t iw = (int32_t)(hdr[18] | (hdr[19] << 8) | (hdr[20] << 16) | ((uint32_t)hdr[21] << 24));
  int32_t ih = (int32_t)(hdr[22] | (hdr[23] << 8) | (hdr[24] << 16) | ((uint32_t)hdr[25] << 24));
  if (ih < 0) ih = -ih;                                 // BMP zapisany od góry
  if (iw <= 0 || ih <= 0 || iw > 320 || ih > 320) {
    LOG_W("Wyświetlacz TFT: grafika %s ma nietypowy rozmiar %ldx%ld", p.c_str(), (long)iw, (long)ih);
    return nullptr;
  }

  lgfx::LGFX_Device* g = static_cast<lgfx::LGFX_Device*>(gfx_);
  lgfx::LGFX_Sprite* sp = new lgfx::LGFX_Sprite(g);
  sp->setPsram(true);
  sp->setColorDepth(16);
  if (!sp->createSprite(iw, ih) || !sp->drawBmpFile(sdCard.fs(), p.c_str())) {
    delete sp;
    LOG_W("Wyświetlacz TFT: nie udało się wczytać grafiki %s", p.c_str());
    return nullptr;
  }
  icSprite_[state] = sp;
  LOG_I("Wyświetlacz TFT: grafika %s (%ldx%ld px) wczytana z karty SD", p.c_str(), (long)iw, (long)ih);
  return sp;
}

void DisplayManager::freeIcons() {
  for (uint8_t i = 0; i < DISP_IC_COUNT; i++) {
    if (icSprite_[i]) {
      delete static_cast<lgfx::LGFX_Sprite*>(icSprite_[i]);
      icSprite_[i] = nullptr;
    }
    icTried_[i] = false;       // kolejna próba po zmianie ustawień/restarcie
  }
}

// "disp_ch" to lista identyfikatorów rozdzielonych przecinkami.
uint8_t DisplayManager::channelCount() {
  uint8_t n = 0;
  for (int from = 0; from <= (int)chList_.length(); ) {
    const int comma = chList_.indexOf(',', from);
    String part = (comma < 0) ? chList_.substring(from) : chList_.substring(from, comma);
    part.trim();
    if (part.length()) n++;
    if (comma < 0) break;
    from = comma + 1;
  }
  return n;
}

String DisplayManager::channelAt(uint8_t idx) {
  uint8_t n = 0;
  for (int from = 0; from <= (int)chList_.length(); ) {
    const int comma = chList_.indexOf(',', from);
    String part = (comma < 0) ? chList_.substring(from) : chList_.substring(from, comma);
    part.trim();
    if (part.length()) {
      if (n == idx) return part;
      n++;
    }
    if (comma < 0) break;
    from = comma + 1;
  }
  return String();
}

String DisplayManager::json() {
  JsonDocument d;
  d["enabled"]    = enabled();
  d["active"]     = ready_;
  d["interval_s"] = intervalS();
  d["rotate"]     = (unsigned)rot_;
  d["theme"]      = theme_ ? 1 : 0;
  d["invert"]     = invert_ ? 1 : 0;
  d["ic"]         = (unsigned)ic_;
  d["ic_name"]    = icName();
  d["offrot"]     = (unsigned)offrot_;
  d["offx"]       = offx_;
  d["offy"]       = offy_;
  d["bgr"]        = bgr_ ? 1 : 0;
  d["controller"] = icName();
  d["width"]      = width();
  d["height"]     = height();
  // Widok: tryb, kanały i stan wyliczony (noc, pogoda) - żeby dało się to
  // sprawdzić bez patrzenia na ekran.
  d["mode"]        = (unsigned)mode_;
  d["channels"]    = chList_;
  d["channel_now"] = channelAt(idx_);
  d["step_s"]      = (uint32_t)(stepMs_ / 1000);
  d["font"]        = (unsigned)fontMode_;
  d["night"]       = nightOn_ ? 1 : 0;
  d["night_from"]  = (unsigned)nightFrom_;
  d["night_to"]    = (unsigned)nightTo_;
  d["night_now"]   = nightNow_ ? 1 : 0;
  d["ic_x"]        = icX_;
  d["ic_y"]        = icY_;
  d["weather"]     = weatherStateName(weatherState());
  JsonObject ip = d["icon_paths"].to<JsonObject>();
  JsonObject io = d["icon_ok"].to<JsonObject>();
  for (uint8_t i = 0; i < DISP_IC_COUNT; i++) {
    ip[kIconNames[i]] = icPath_[i];
    io[kIconNames[i]] = (icSprite_[i] != nullptr);
  }
  if (ready_ && gfx_) {
    // Rzeczywisty rozmiar logiczny (po obrocie) - to on trafia na ekran.
    lgfx::LGFX_Device* g = static_cast<lgfx::LGFX_Device*>(gfx_);
    d["logic_w"] = (int)g->width();
    d["logic_h"] = (int)g->height();
    d["framebuf"] = (sprite_ != nullptr);
  }
  d["pins_ok"]    = pinsOk();
  d["last_draw_ms"] = lastDrawTook_;
  d["draws"]      = draws_;
  JsonObject p = d["pins"].to<JsonObject>();
  p["sck"]  = pinMap.pin("tft_sck");
  p["mosi"] = pinMap.pin("tft_mosi");
  p["cs"]   = pinMap.pin("tft_cs");
  p["dc"]   = pinMap.pin("tft_dc");
  p["rst"]  = pinMap.pin("tft_rst");

  // Wygląd i pozycje elementów - te same klucze przyjmuje POST /api/display
  // (sekcja "Wygląd i pozycje" w zakładce TFT).
  JsonObject u = d["ui"].to<JsonObject>();
  struct UiOut { const char* p; const UiCfg* u; };
  const UiOut outs[] = { { "top", &uiTop_ }, { "lbl", &uiLbl_ }, { "val", &uiVal_ },
                         { "unit", &uiUnit_ }, { "info", &uiInfo_ }, { "wx", &uiWx_ } };
  for (uint8_t i = 0; i < sizeof(outs) / sizeof(outs[0]); i++) {
    JsonObject o = u[outs[i].p].to<JsonObject>();
    const UiCfg& c = *outs[i].u;
    o["on"]    = c.on;
    o["size"]  = c.size;
    o["dx"]    = c.dx;
    o["dy"]    = c.dy;
    o["color"] = c.col ? hexString(c.col) : String("");
  }
  u["top_cam"]   = uiTopCam_;
  u["top_out"]   = uiTopOut_;
  u["top_in"]    = uiTopIn_;
  u["bar_on"]    = uiBarOn_;
  u["bar_h"]     = uiBarH_;
  u["bar_color"] = uiBarCol_ ? hexString(uiBarCol_) : String("");
  u["wx_icon"]   = uiWxIcon_;

  String s;
  serializeJson(d, s);
  return s;
}

bool DisplayManager::applyJson(const char* json, size_t len) {
  JsonDocument d;
  if (deserializeJson(d, json, len)) return false;

  const bool en     = d["enabled"].isNull()    ? enabled()                : d["enabled"].as<bool>();
  uint32_t   iv     = d["interval_s"].isNull() ? intervalS()              : d["interval_s"].as<uint32_t>();
  int        rot    = d["rotate"].isNull()     ? (int)rot_                : d["rotate"].as<int>();
  const bool theme  = d["theme"].isNull()      ? theme_                   : (d["theme"].as<int>() >= 1);
  const bool inv    = d["invert"].isNull()     ? invert_                  : d["invert"].as<bool>();
  int        ic     = d["ic"].isNull()         ? (int)ic_                 : d["ic"].as<int>();
  int        offrot = d["offrot"].isNull()     ? (int)offrot_             : d["offrot"].as<int>();
  int        offx   = d["offx"].isNull()       ? (int)offx_               : d["offx"].as<int>();
  int        offy   = d["offy"].isNull()       ? (int)offy_               : d["offy"].as<int>();
  const bool bgr    = d["bgr"].isNull()        ? bgr_                     : d["bgr"].as<bool>();

  // Widok: tryb, kanały, czcionka, noc, grafiki pogody
  int        md     = d["mode"].isNull()       ? (int)mode_               : d["mode"].as<int>();
  int        fm     = d["font"].isNull()       ? (int)fontMode_           : d["font"].as<int>();
  uint32_t   stepS  = d["step_s"].isNull()     ? (uint32_t)(stepMs_ / 1000)
                                               : d["step_s"].as<uint32_t>();
  const String chans = d["channels"].isNull()  ? chList_                  : d["channels"].as<String>();
  const bool night  = d["night"].isNull()      ? nightOn_                 : (d["night"].as<int>() >= 1);
  int        nf     = d["night_from"].isNull() ? (int)nightFrom_          : d["night_from"].as<int>();
  int        nt     = d["night_to"].isNull()   ? (int)nightTo_            : d["night_to"].as<int>();
  int        icx    = d["ic_x"].isNull()       ? (int)icX_                : d["ic_x"].as<int>();
  int        icy    = d["ic_y"].isNull()       ? (int)icY_                : d["ic_y"].as<int>();
  String     icons[DISP_IC_COUNT];
  for (uint8_t i = 0; i < DISP_IC_COUNT; i++) {
    icons[i] = d["icon_paths"][kIconNames[i]].isNull() ? icPath_[i]
                                                       : d["icon_paths"][kIconNames[i]].as<String>();
  }

  if (iv < 2) iv = 2;
  if (iv > 300) iv = 300;
  rot &= 3;
  if (ic < 0 || ic > 2) ic = 0;
  offrot &= 7;
  if (offx < -64) offx = -64;
  if (offx > 64) offx = 64;
  if (offy < -64) offy = -64;
  if (offy > 64) offy = 64;

  if (md < 0) md = 0;
  if (md > 3) md = 3;
  if (fm < 0) fm = 0;
  if (fm > 5) fm = 5;
  if (stepS < 2) stepS = 2;
  if (stepS > 120) stepS = 120;
  if (nf < 0) nf = 0;
  if (nf > 23) nf = 23;
  if (nt < 0) nt = 0;
  if (nt > 23) nt = 23;
  if (icx < -32) icx = -32;
  if (icx > 320) icx = 320;
  if (icy < -32) icy = -32;
  if (icy > 320) icy = 320;

  config.setExtraF("disp_on", en ? 1.0f : 0.0f);
  config.setExtraF("disp_int", (float)iv);
  config.setExtraF("disp_rot", (float)rot);
  config.setExtraF("disp_theme", theme ? 1.0f : 0.0f);
  config.setExtraF("disp_inv", inv ? 1.0f : 0.0f);
  config.setExtraF("disp_ic", (float)ic);
  config.setExtraF("disp_offrot", (float)offrot);
  config.setExtraF("disp_offx", (float)offx);
  config.setExtraF("disp_offy", (float)offy);
  config.setExtraF("disp_bgr", bgr ? 1.0f : 0.0f);
  config.setExtraF("disp_mode", (float)md);
  config.setExtraF("disp_font", (float)fm);
  config.setExtraF("disp_step_s", (float)stepS);
  config.setExtraS("disp_ch", chans);
  config.setExtraF("disp_night", night ? 1.0f : 0.0f);
  config.setExtraF("disp_nf", (float)nf);
  config.setExtraF("disp_nt", (float)nt);
  config.setExtraF("disp_ic_x", (float)icx);
  config.setExtraF("disp_ic_y", (float)icy);
  for (uint8_t i = 0; i < DISP_IC_COUNT; i++) config.setExtraS(kIconKeys[i], icons[i]);

  // --- Wygląd i pozycje elementów (sekcja "Wygląd i pozycje" w www) ---
  // Każdy element: on (pokazywać), size (wysokość czcionki: 16/26/48/75, 0 = auto),
  // dx/dy (przesunięcie w pikselach), color ("#RRGGBB", puste = kolor domyślny).
  JsonObjectConst uj = d["ui"];
  if (!uj.isNull()) {
    struct UiRef { const char* p; UiCfg* u; };
    const UiRef refs[] = { { "top", &uiTop_ }, { "lbl", &uiLbl_ }, { "val", &uiVal_ },
                           { "unit", &uiUnit_ }, { "info", &uiInfo_ }, { "wx", &uiWx_ } };
    for (uint8_t i = 0; i < sizeof(refs) / sizeof(refs[0]); i++) {
      JsonObjectConst o = uj[refs[i].p];
      if (o.isNull()) continue;
      UiCfg& u = *refs[i].u;
      if (!o["on"].isNull())    u.on   = o["on"].as<int>() >= 1;
      if (!o["size"].isNull())  u.size = o["size"].as<int>();
      if (!o["dx"].isNull())    u.dx   = o["dx"].as<int>();
      if (!o["dy"].isNull())    u.dy   = o["dy"].as<int>();
      if (!o["color"].isNull()) u.col  = hexFromString(o["color"].as<String>());
      // Zakresy - żeby po wpisaniu przypadkowej liczby nic nie zniknęło z ekranu.
      if (u.size != 16 && u.size != 26 && u.size != 48 && u.size != 75) u.size = 0;
      if (u.dx < -200) u.dx = -200;
      if (u.dx >  200) u.dx =  200;
      if (u.dy < -200) u.dy = -200;
      if (u.dy >  200) u.dy =  200;
    }
    if (!uj["top_cam"].isNull()) uiTopCam_ = uj["top_cam"].as<int>() >= 1;
    if (!uj["top_out"].isNull()) uiTopOut_ = uj["top_out"].as<int>() >= 1;
    if (!uj["top_in"].isNull())  uiTopIn_  = uj["top_in"].as<int>() >= 1;
    if (!uj["bar_on"].isNull())  uiBarOn_  = uj["bar_on"].as<int>() >= 1;
    if (!uj["bar_h"].isNull()) {
      uiBarH_ = uj["bar_h"].as<int>();
      if (uiBarH_ < 0)  uiBarH_ = 0;
      if (uiBarH_ > 48) uiBarH_ = 48;
    }
    if (!uj["bar_color"].isNull()) uiBarCol_ = hexFromString(uj["bar_color"].as<String>());
    if (!uj["wx_icon"].isNull()) {
      uiWxIcon_ = uj["wx_icon"].as<int>();
      if (uiWxIcon_ < 0)   uiWxIcon_ = 0;
      if (uiWxIcon_ > 300) uiWxIcon_ = 300;
    }
    for (uint8_t i = 0; i < sizeof(refs) / sizeof(refs[0]); i++) {
      const String k = String("disp_ui_") + refs[i].p + "_";
      const UiCfg& u = *refs[i].u;
      config.setExtraF((k + "size").c_str(), (float)u.size);
      config.setExtraF((k + "dx").c_str(),   (float)u.dx);
      config.setExtraF((k + "dy").c_str(),   (float)u.dy);
      config.setExtraF((k + "on").c_str(),   u.on ? 1.0f : 0.0f);
      config.setExtraS((k + "col").c_str(),  u.col ? hexString(u.col) : String(""));
    }
    config.setExtraF("disp_ui_top_cam", uiTopCam_ ? 1.0f : 0.0f);
    config.setExtraF("disp_ui_top_out", uiTopOut_ ? 1.0f : 0.0f);
    config.setExtraF("disp_ui_top_in",  uiTopIn_  ? 1.0f : 0.0f);
    config.setExtraF("disp_ui_bar_on",  uiBarOn_  ? 1.0f : 0.0f);
    config.setExtraF("disp_ui_bar_h",   (float)uiBarH_);
    config.setExtraS("disp_ui_bar_col", uiBarCol_ ? hexString(uiBarCol_) : String(""));
    config.setExtraF("disp_ui_wx_icon", (float)uiWxIcon_);
  }

  config.save();

  LOG_I("Wyświetlacz TFT: zapis ustawień (on=%d, %u s, obrót %d, motyw %d, inwersja %d, IC %d, offrot %d, offx %d, offy %d, BGR %d)",
        (int)en, (unsigned)iv, rot, (int)theme, (int)inv, ic, offrot, offx, offy, (int)bgr);
  LOG_I("Wyświetlacz TFT: zapis widoku (tryb %d, czcionka %d, cykl %u s, kanały \"%s\", noc %d %d-%d)",
        md, fm, (unsigned)stepS, chans.c_str(), (int)night, nf, nt);
  applyConfig();
  return true;
}

#else  // STACJA_ROLE_MASTER - na węźle wyświetlacz nie jest obsługiwany

DisplayManager display;

bool DisplayManager::enabled() { return false; }
void DisplayManager::setEnabled(bool) {}
void DisplayManager::reloadIcons() {}
String DisplayManager::plTest() { return String("{\"error\":\"wyświetlacz nieobsługiwany\"}"); }
uint32_t DisplayManager::intervalS() { return 10; }
void DisplayManager::setIntervalS(uint32_t) {}
bool DisplayManager::pinsOk() { return false; }
void DisplayManager::release() {}
void DisplayManager::begin() {}
void DisplayManager::applyConfig() {}
void DisplayManager::loop() {}
void DisplayManager::draw() {}
String DisplayManager::json() { return String("{\"enabled\":false}"); }
bool DisplayManager::applyJson(const char*, size_t) { return false; }

#endif
