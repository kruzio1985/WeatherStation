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

// Szerokość napisu z polskimi znakami (definicja niżej - potrzebna już przy
// dobieraniu rozmiaru czcionki).
static int plWidth(lgfx::LovyanGFX* d, const String& s);

// Największa czcionka, w której tekst zmieści się na szerokość maxW.
void setBigFont(lgfx::LovyanGFX* d, const String& text, int maxW, uint8_t fontMode) {
  if (fontMode != 0) {
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

  switch (mode_) {
    case 1:  drawBig(d, W, H, bg, fg, dim, acc, hhmm, true);   break;  // cykl wskazań
    case 2:  drawBig(d, W, H, bg, fg, dim, acc, hhmm, false);  break;  // jedna wartość
    case 3:  drawWeather(d, W, H, bg, fg, dim, acc, hhmm);     break;  // pogoda + grafika
    default: drawDashboard(d, W, H, bg, fg, dim, acc, hhmm);   break;  // pulpit
  }

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
  const float tout = sensors.valueOf("c3_temp");

  String foot = String("WiFi ") + (WiFi.status() == WL_CONNECTED ? "OK" : "--");
  foot += "  MQTT ";
  foot += (mqtt.connected() ? "OK" : "--");
  if (!isnan(tout)) foot += "  zewn. " + num(tout, 1) + " C";

  // --- Nagłówek (wspólny) ---
  d->setFont(&lgfx::fonts::Font2);
  d->setTextColor(dim, bg);
  plDraw(d, 8, 6, "Stacja Pogody", dim, bg, lgfx::textdatum_t::top_left);
  plDraw(d, right, 6, hhmm, dim, bg, lgfx::textdatum_t::top_right);
  d->drawFastHLine(8, 26, W - 16, dim);

  if (W >= H) {
    // ---------------- UKŁAD POZIOMY (np. panel zamontowany w poziomie) ----------------
    const int colX = W * 55 / 100;          // granica kolumn
    d->drawFastVLine(colX, 32, H - 60, dim);

    // Lewa kolumna: duża temperatura + wilgotność i wiatr pod nią
    d->setTextColor(fg, bg);
    d->setFont(&lgfx::fonts::Font6);
    plDraw(d, 12, 34, num(t, 1), fg, bg, lgfx::textdatum_t::top_left);
    d->setFont(&lgfx::fonts::Font2);
    plDraw(d, 12, 88, "°C", acc, bg, lgfx::textdatum_t::top_left);

    auto leftRow = [&](int y, const char* label, const String& value) {
      d->setFont(&lgfx::fonts::Font2);
      plDraw(d, 12, y, label, dim, bg, lgfx::textdatum_t::top_left);
      d->setFont(&lgfx::fonts::Font4);
      plDraw(d, 12, y + 18, value, fg, bg, lgfx::textdatum_t::top_left);
    };
    leftRow(116, "Wilgotność", num(h, 0) + " %");
    leftRow(166, "Wiatr",      num(w, 1) + " km/h");

    // Prawa kolumna: ciśnienie, pył i AQI z C3
    auto rightRow = [&](int y, const char* label, const String& value, uint16_t color) {
      d->setFont(&lgfx::fonts::Font2);
      plDraw(d, colX + 10, y, label, dim, bg, lgfx::textdatum_t::top_left);
      d->setFont(&lgfx::fonts::Font4);
      plDraw(d, right, y + 16, value, color, bg, lgfx::textdatum_t::top_right);
    };
    rightRow(40,  "Ciśnienie",   num(p, 1) + " hPa",   fg);
    rightRow(86,  "PM2.5 (C3)",  num(pm, 0) + " µg/m³", acc);
    rightRow(132, "AQI (C3)",    num(aqi, 0),           acc);
  } else {
    // ---------------- UKŁAD PIONOWY (240x320) ----------------
    const int cx = W / 2;

    d->setFont(&lgfx::fonts::Font6);
    plDraw(d, cx, 40, num(t, 1), fg, bg, lgfx::textdatum_t::top_center);
    d->setFont(&lgfx::fonts::Font2);
    plDraw(d, cx, 92, "°C", acc, bg, lgfx::textdatum_t::top_center);
    d->drawFastHLine(8, 112, W - 16, dim);

    const int rowTop  = 140;
    const int rowStep = (H - rowTop - 36) / 4;
    auto row = [&](int idx, const char* label, const String& value, uint16_t color) {
      const int y = rowTop + idx * rowStep;
      d->setFont(&lgfx::fonts::Font2);
      plDraw(d, 12, y, label, dim, bg, lgfx::textdatum_t::middle_left);
      d->setFont(&lgfx::fonts::Font4);
      plDraw(d, right, y, value, color, bg, lgfx::textdatum_t::middle_right);
    };
    row(0, "Wilgotność",  num(h, 0) + " %",        fg);
    row(1, "Ciśnienie",   num(p, 1) + " hPa",      fg);
    row(2, "Wiatr",       num(w, 1) + " km/h",     fg);
    row(3, "PM2.5 (C3)",  num(pm, 0) + " µg/m³",   acc);
    row(4, "AQI (C3)",    num(aqi, 0),             acc);
  }

  // --- Stopka (wspólna) ---
  d->setFont(&lgfx::fonts::Font2);
  plDraw(d, W / 2, H - 4, foot, dim, bg, lgfx::textdatum_t::bottom_center);
}

// ---------------- Widoki 1 i 2: duże wskazania ----------------
// Tryb 1 pokazuje kolejne kanały z "disp_ch" (zmiana co "disp_step_s"),
// tryb 2 - pierwszy kanał na stałe. Nazwa kanału i jednostka biorą się
// z definicji kanału, więc nic nie trzeba wpisywać ręcznie.
void DisplayManager::drawBig(void* dv, int W, int H, uint16_t bg, uint16_t fg, uint16_t dim,
                             uint16_t acc, const char* hhmm, bool cycle) {
  lgfx::LovyanGFX* d = static_cast<lgfx::LovyanGFX*>(dv);
  const uint8_t n = channelCount();

  d->setFont(&lgfx::fonts::Font2);
  d->setTextColor(dim, bg);
  d->setTextDatum(lgfx::textdatum_t::top_left);
  d->drawString(hhmm, 8, 6);
  if (cycle && n > 1) {
    d->setTextDatum(lgfx::textdatum_t::top_right);
    d->drawString(String((unsigned)(idx_ % n) + 1) + "/" + String(n), W - 8, 6);
  }
  d->drawFastHLine(8, 26, W - 16, dim);

  if (n == 0) {
    d->setTextDatum(lgfx::textdatum_t::middle_center);
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
  const String val = num(sensors.valueOf(id), dec);

  // Nazwa kanału
  d->setFont(&lgfx::fonts::Font4);
  plDraw(d, W / 2, H * 28 / 100, name, acc, bg, lgfx::textdatum_t::middle_center);

  // Wartość - największa czcionka, jaka się zmieści
  setBigFont(d, val, W - 20, fontMode_);
  const int valY = H * 55 / 100;
  plDraw(d, W / 2, valY, val, fg, bg, lgfx::textdatum_t::middle_center);

  // Jednostka pod wartością
  if (unit.length()) {
    d->setFont(&lgfx::fonts::Font4);
    plDraw(d, W / 2, valY + d->fontHeight() * 3 / 4 + 22, unit, acc, bg,
           lgfx::textdatum_t::middle_center);
  }

  // Kropki pokazujące pozycję w cyklu
  if (cycle && n > 1) {
    const int gap = 10;
    const int x0 = W / 2 - (n * gap) / 2;
    for (uint8_t i = 0; i < n && i < 16; i++) {
      const int x = x0 + i * gap;
      if (i == idx_ % n) d->fillCircle(x, H - 12, 3, acc);
      else               d->drawCircle(x, H - 12, 3, dim);
    }
  }
}

// ---------------- Widok 3: pogoda (grafika + wartości) ----------------
void DisplayManager::drawWeather(void* dv, int W, int H, uint16_t bg, uint16_t fg, uint16_t dim,
                                 uint16_t acc, const char* hhmm) {
  lgfx::LovyanGFX* d = static_cast<lgfx::LovyanGFX*>(dv);
  const uint8_t st = weatherState();
  const bool wide = (W >= H);
  const int  side = wide ? (H * 45 / 100) : (W * 55 / 100);

  int ix = icX_;
  int iy = icY_;
  if (!wide) {                       // w pionie grafika idzie na środek u góry
    ix = (W - side) / 2;
    iy = 40;
  }

  d->setFont(&lgfx::fonts::Font2);
  plDraw(d, 8, 6, hhmm, dim, bg, lgfx::textdatum_t::top_left);
  plDraw(d, W - 8, 6, weatherStateName(st), dim, bg, lgfx::textdatum_t::top_right);
  d->drawFastHLine(8, 26, W - 16, dim);

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
  const int tx = wide ? (ix + side + 14) : 12;
  for (uint8_t i = 0; i < n && i < 4; i++) {
    const String id = channelAt(i);
    String name, unit;
    uint8_t dec = 1;
    if (!sensors.channelMeta(id, name, unit, dec)) continue;
    d->setFont(&lgfx::fonts::Font2);
    plDraw(d, tx, ty, name, dim, bg, lgfx::textdatum_t::top_left);
    d->setFont(&lgfx::fonts::Font4);
    plDraw(d, W - 8, ty, num(sensors.valueOf(id), dec), acc, bg,
           lgfx::textdatum_t::top_right);
    if (unit.length()) {
      d->setFont(&lgfx::fonts::Font2);
      plDraw(d, tx, ty + 22, unit, dim, bg, lgfx::textdatum_t::top_left);
    }
    ty += 40;
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
