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
  draw();
}

void DisplayManager::loop() {
  if (!ready_) return;
  unsigned long now = millis();
  if (lastDrawMs_ != 0 && (now - lastDrawMs_) < intervalMs_) return;
  lastDrawMs_ = now;
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

  const uint16_t bg  = theme_ ? (uint16_t)0xFFFF : (uint16_t)0x0000;
  const uint16_t fg  = theme_ ? (uint16_t)0x0000 : (uint16_t)0xFFFF;
  const uint16_t dim = d->color565(130, 140, 150);
  const uint16_t acc = d->color565(56, 189, 248);

  const float t   = sensors.valueOf("temp");
  const float h   = sensors.valueOf("hum");
  const float p   = sensors.valueOf("press");
  float w         = sensors.valueOf("vev_wind");
  if (isnan(w)) w = sensors.valueOf("wind");
  const float pm  = sensors.valueOf("c3_pm25");
  const float aqi = sensors.valueOf("c3_aqi");
  const float tout = sensors.valueOf("c3_temp");

  time_t now = time(nullptr);
  char hhmm[8] = "--:--";
  if (now > 1600000000) {
    struct tm tmv;
    localtime_r(&now, &tmv);
    strftime(hhmm, sizeof(hhmm), "%H:%M", &tmv);
  }

  String foot = String("WiFi ") + (WiFi.status() == WL_CONNECTED ? "OK" : "--");
  foot += "  MQTT ";
  foot += (mqtt.connected() ? "OK" : "--");
  if (!isnan(tout)) foot += "  zewn. " + num(tout, 1) + " C";

  unsigned long t0 = millis();
  d->startWrite();
  d->fillScreen(bg);

  // --- Nagłówek (wspólny) ---
  d->setFont(&lgfx::fonts::Font2);
  d->setTextColor(dim, bg);
  d->setTextDatum(lgfx::textdatum_t::top_left);
  d->drawString("Stacja Pogody", 8, 6);
  d->setTextDatum(lgfx::textdatum_t::top_right);
  d->drawString(hhmm, right, 6);
  d->drawFastHLine(8, 26, W - 16, dim);

  if (W >= H) {
    // ---------------- UKŁAD POZIOMY (np. panel zamontowany w poziomie) ----------------
    const int colX = W * 55 / 100;          // granica kolumn
    d->drawFastVLine(colX, 32, H - 60, dim);

    // Lewa kolumna: duża temperatura + wilgotność i wiatr pod nią
    d->setTextDatum(lgfx::textdatum_t::top_left);
    d->setTextColor(fg, bg);
    d->setFont(&lgfx::fonts::Font6);
    d->drawString(num(t, 1).c_str(), 12, 34);
    d->setFont(&lgfx::fonts::Font2);
    d->setTextColor(acc, bg);
    d->drawString("stopnie C", 12, 88);

    auto leftRow = [&](int y, const char* label, const String& value) {
      d->setFont(&lgfx::fonts::Font2);
      d->setTextColor(dim, bg);
      d->setTextDatum(lgfx::textdatum_t::top_left);
      d->drawString(label, 12, y);
      d->setFont(&lgfx::fonts::Font4);
      d->setTextColor(fg, bg);
      d->drawString(value.c_str(), 12, y + 18);
    };
    leftRow(116, "Wilgotnosc", num(h, 0) + " %");
    leftRow(166, "Wiatr",      num(w, 1) + " km/h");

    // Prawa kolumna: ciśnienie, pył i AQI z C3
    auto rightRow = [&](int y, const char* label, const String& value, uint16_t color) {
      d->setFont(&lgfx::fonts::Font2);
      d->setTextColor(dim, bg);
      d->setTextDatum(lgfx::textdatum_t::top_left);
      d->drawString(label, colX + 10, y);
      d->setFont(&lgfx::fonts::Font4);
      d->setTextColor(color, bg);
      d->setTextDatum(lgfx::textdatum_t::top_right);
      d->drawString(value.c_str(), right, y + 16);
    };
    rightRow(40,  "Cisnienie",  num(p, 1) + " hPa",   fg);
    rightRow(86,  "PM2.5 (C3)", num(pm, 0) + " ug/m3", acc);
    rightRow(132, "AQI (C3)",   num(aqi, 0),           acc);
  } else {
    // ---------------- UKŁAD PIONOWY (240x320) ----------------
    const int cx = W / 2;

    d->setTextDatum(lgfx::textdatum_t::top_center);
    d->setTextColor(fg, bg);
    d->setFont(&lgfx::fonts::Font6);
    d->drawString(num(t, 1).c_str(), cx, 40);
    d->setFont(&lgfx::fonts::Font2);
    d->setTextColor(acc, bg);
    d->drawString("stopnie C", cx, 92);
    d->drawFastHLine(8, 112, W - 16, dim);

    const int rowTop  = 140;
    const int rowStep = (H - rowTop - 36) / 4;
    auto row = [&](int idx, const char* label, const String& value, uint16_t color) {
      const int y = rowTop + idx * rowStep;
      d->setFont(&lgfx::fonts::Font2);
      d->setTextColor(dim, bg);
      d->setTextDatum(lgfx::textdatum_t::middle_left);
      d->drawString(label, 12, y);
      d->setFont(&lgfx::fonts::Font4);
      d->setTextColor(color, bg);
      d->setTextDatum(lgfx::textdatum_t::middle_right);
      d->drawString(value.c_str(), right, y);
    };
    row(0, "Wilgotnosc",  num(h, 0) + " %",        fg);
    row(1, "Cisnienie",   num(p, 1) + " hPa",      fg);
    row(2, "Wiatr",       num(w, 1) + " km/h",     fg);
    row(3, "PM2.5 (C3)",  num(pm, 0) + " ug/m3",   acc);
    row(4, "AQI (C3)",    num(aqi, 0),             acc);
  }

  // --- Stopka (wspólna) ---
  d->setFont(&lgfx::fonts::Font2);
  d->setTextColor(dim, bg);
  d->setTextDatum(lgfx::textdatum_t::bottom_center);
  d->drawString(foot.c_str(), W / 2, H - 4);
  d->endWrite();

  // Jedno wypchnięcie gotowej klatki na panel (bez migotania).
  if (sp) sp->pushSprite(0, 0);

  lastDrawTook_ = millis() - t0;
  if (draws_ < 2 || lastDrawTook_ > 250) {
    LOG_I("Wyświetlacz TFT: odswiezanie %lu ms", lastDrawTook_);
  }
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

  if (iv < 2) iv = 2;
  if (iv > 300) iv = 300;
  rot &= 3;
  if (ic < 0 || ic > 2) ic = 0;
  offrot &= 7;
  if (offx < -64) offx = -64;
  if (offx > 64) offx = 64;
  if (offy < -64) offy = -64;
  if (offy > 64) offy = 64;

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
  config.save();

  LOG_I("Wyświetlacz TFT: zapis ustawień (on=%d, %u s, obrót %d, motyw %d, inwersja %d, IC %d, offrot %d, offx %d, offy %d, BGR %d)",
        (int)en, (unsigned)iv, rot, (int)theme, (int)inv, ic, offrot, offx, offy, (int)bgr);
  applyConfig();
  return true;
}

#else  // STACJA_ROLE_MASTER - na węźle wyświetlacz nie jest obsługiwany

DisplayManager display;

bool DisplayManager::enabled() { return false; }
void DisplayManager::setEnabled(bool) {}
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
