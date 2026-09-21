/* =============================================================================
 * Stacja Pogody - stacja meteorologiczna na ESP32-S3 WROOM-1 (N16R8)
 * Copyright (c) 2026 kruzio1985 - https://github.com/kruzio1985
 * Licencja: PolyForm Noncommercial License 1.0.0 (plik LICENSE)
 * Użytek niekomercyjny. Kontakt: kruzio1985@users.noreply.github.com
 * =============================================================================
 */
#include "led_ring.h"
#include "config.h"
#include "pinmap.h"
#include "sensors.h"
#include "syslog.h"
#include <Adafruit_NeoPixel.h>
#include <math.h>
#include <time.h>

LedRing ledRing;

// ------------------------------------------------------------
//  Kolory stanów pogody
//  Wartości domyślne i nazwy stanów są w config.h, a same kolory można
//  zmienić na stronie www (zakładka "Led RGB") - wtedy czytamy je z NVS.
// ------------------------------------------------------------
static inline uint32_t stateColorAt(uint8_t state) {
  return config.rgbColor(state);
}

static Adafruit_NeoPixel* s_strip = nullptr;
static uint8_t* s_buf = nullptr;    // 3 bajty na diodę (R,G,B)
static uint8_t  s_bufCount = 0;

static inline uint32_t rgbOf(uint8_t r, uint8_t g, uint8_t b) {
  return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

static bool isNight() {
  time_t now = time(nullptr);
  if (now < 1000000000) return false;   // brak synchronizacji NTP
  struct tm tmv;
  localtime_r(&now, &tmv);
  return tmv.tm_hour >= 22 || tmv.tm_hour < 6;
}

// ------------------------------------------------------------
//  Inicjalizacja
// ------------------------------------------------------------
bool LedRing::begin() {
  applyConfig();
  if (pin_ < 0) {
    LOG_W("Pierścień LED RGB wyłączony (pin -1)");
    return false;
  }
  if (!ready_) {
    LOG_E("Nie udało się utworzyć pierścienia LED RGB (brak pamięci)");
    enabled_ = false;
    return false;
  }
  initMs_ = millis();
  LOG_I("Pierścień LED RGB: %u diod na GPIO %d (kolejność %s, jasność %u%%)",
        (unsigned)count_, pin_, orderName(), (unsigned)brightness_);
  evaluateWeather();
  render(millis());
  return true;
}

void LedRing::reinit() {
  if (s_strip) { s_strip->clear(); s_strip->show(); delete s_strip; s_strip = nullptr; }
  if (s_buf)   { free(s_buf); s_buf = nullptr; s_bufCount = 0; }
  ready_ = false;
  if (pin_ < 0 || count_ == 0) return;

  neoPixelType t = NEO_GRB + NEO_KHZ800;
  if (order_ == 1) t = NEO_RGB + NEO_KHZ800;
  if (order_ == 2) t = NEO_BRG + NEO_KHZ800;

  s_strip = new Adafruit_NeoPixel(count_, pin_, t);
  s_buf = (uint8_t*)malloc((size_t)count_ * 3);
  if (!s_strip || !s_buf) {
    LOG_E("Brak pamięci na sterownik pierścienia LED RGB");
    if (s_strip) { delete s_strip; s_strip = nullptr; }
    if (s_buf)   { free(s_buf); s_buf = nullptr; }
    enabled_ = false;
    return;
  }
  s_bufCount = count_;
  s_strip->begin();
  s_strip->clear();
  s_strip->show();
  ready_ = true;
}

void LedRing::applyConfig() {
  bool  newEnabled   = config.rgbEnabled();
  uint8_t newBright  = config.rgbBrightness();
  bool  newEffects   = config.rgbEffects();
  bool  newOffNight  = config.rgbOffAtNight();
  uint8_t newCount   = (uint8_t)constrain((int)config.rgbCount(), 1, 150);
  uint8_t newOrder   = (uint8_t)constrain((int)config.rgbOrder(), 0, 2);
  uint16_t newMaxMa  = (uint16_t)constrain((int)config.rgbMaxMa(), 100, 5000);
  int   newPin       = pinMap.pin("rgb");

  bool needInit = (newPin != pin_) || (newCount != count_) || (newOrder != order_) || !s_strip;

  enabled_    = newEnabled;
  brightness_ = newBright;
  effects_    = newEffects;
  offAtNight_ = newOffNight;
  count_      = newCount;
  order_      = newOrder;
  maxMa_      = newMaxMa;
  pin_        = newPin;

  if (needInit) {
    reinit();
    initMs_ = millis();
  }
  render(millis());
}

void LedRing::setEnabled(bool on) {
  enabled_ = on;
  render(millis());
}

void LedRing::setBrightness(uint8_t percent) {
  brightness_ = percent > 100 ? 100 : percent;
  render(millis());
}

void LedRing::setPreview(int state) {
  if (state < 0) { preview_ = kPreviewNone; }
  else           { preview_ = state; previewUntil_ = millis() + 20000; }
  render(millis());
}

// ------------------------------------------------------------
//  Stan pogody z aktualnych pomiarów
// ------------------------------------------------------------
void LedRing::evaluateWeather() {
  float temp  = sensors.valueOf("temp");
  float hum   = sensors.valueOf("hum");
  float press = sensors.valueOf("press");
  float light = sensors.valueOf("light");
  float rain  = sensors.valueOf("rain");
  float wind  = sensors.valueOf("wind");

  bool anyData = !isnan(temp) || !isnan(hum) || !isnan(press) ||
                 !isnan(light) || !isnan(rain) || !isnan(wind);

  WeatherState st;
  String reason;

  if (!anyData) {
    st = WS_ERROR;
    reason = "brak danych z czujników";
  } else if ((!isnan(wind) && wind >= 45) ||
             (!isnan(rain) && rain >= 8) ||
             (!isnan(rain) && !isnan(wind) && rain > 0.5f && wind >= 30)) {
    st = WS_STORM;
    reason = "burza - silny wiatr/ulewa";
  } else if (!isnan(rain) && rain > 0.2f) {
    if (!isnan(temp) && temp <= 0.5f) { st = WS_SNOW;  reason = "opad przy mrozie - śnieg"; }
    else                              { st = WS_RAIN;  reason = "deszcz"; }
  } else if (!isnan(temp) && temp <= 0.0f) {
    st = WS_SNOW;   reason = "temperatura ujemna";
  } else if (!isnan(temp) && temp >= 30.0f) {
    st = WS_HEAT;   reason = "upał";
  } else if (!isnan(hum) && hum >= 95.0f) {
    st = WS_FOG;    reason = "wilgotność ~100% - mgła";
  } else if (!isnan(wind) && wind >= 25.0f) {
    st = WS_WIND;   reason = "silny wiatr";
  } else if (!isnan(light) && light < 2000.0f) {
    st = WS_MEDIUM; reason = "zachmurzenie (mało światła)";
  } else if (!isnan(hum) && hum >= 80.0f) {
    st = WS_MEDIUM; reason = "duża wilgotność";
  } else {
    st = WS_GOOD;   reason = "ładna pogoda";
  }

  if (st != state_ || reason != lastReason_) {
    LOG_I("Pierścień LED RGB: %s (%s) T=%.1f H=%.0f opad=%.1f wiatr=%.1f",
          config.rgbName((uint8_t)st).c_str(),
          reason.c_str(),
          isnan(temp) ? 0.0f : temp, isnan(hum) ? 0.0f : hum,
          isnan(rain) ? 0.0f : rain, isnan(wind) ? 0.0f : wind);
    state_ = st;
    lastReason_ = reason;
  }
}

// ------------------------------------------------------------
//  Rysowanie
// ------------------------------------------------------------
void LedRing::fill(uint32_t rgb) {
  if (!s_strip || !s_buf) return;
  uint8_t r = (rgb >> 16) & 0xFF, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
  for (uint16_t i = 0; i < count_; i++) s_strip->setPixelColor(i, s_strip->Color(r, g, b));
  s_strip->show();
}

uint8_t LedRing::effPct(uint32_t now) const {
  if (!enabled_) return 0;
  uint32_t pct = brightness_;
  if (offAtNight_ && isNight()) pct = (pct * 5) / 100;   // nocą tylko delikatna poświata
  if (pct > 100) pct = 100;
  return (uint8_t)pct;
}

void LedRing::render(uint32_t now) {
  if (!s_strip || !s_buf || !s_bufCount) return;

  // Podgląd kolorów z www ma pierwszeństwo nad stanem z czujników
  WeatherState shown = state_;
  if (preview_ != kPreviewNone) {
    if ((int32_t)(now - previewUntil_) >= 0) preview_ = kPreviewNone;
    else if (preview_ == kPreviewCycle) {
      size_t idx = ((now - previewUntil_ + 20000) / 1500) % WEATHER_STATE_COUNT;
      shown = (WeatherState)idx;
    } else {
      shown = (WeatherState)constrain(preview_, 0, (int)WEATHER_STATE_COUNT - 1);
    }
  }

  uint8_t pct = effPct(now);
  if (pct == 0 || shown == WS_OFF) { fill(0x000000); lastMa_ = 0; return; }

  uint32_t col = stateColorAt((uint8_t)shown);
  uint8_t cr = (col >> 16) & 0xFF, cg = (col >> 8) & 0xFF, cb = col & 0xFF;
  uint32_t t = now - initMs_;

  // Wypełnienie bufora zależnie od stanu (efekty można wyłączyć w www)
  memset(s_buf, 0, (size_t)count_ * 3);
  auto put = [&](int i, uint8_t k) {
    i = ((i % count_) + count_) % count_;
    uint8_t* p = s_buf + (size_t)i * 3;
    uint8_t r = (uint8_t)((uint16_t)cr * k / 255);
    uint8_t g = (uint8_t)((uint16_t)cg * k / 255);
    uint8_t b = (uint8_t)((uint16_t)cb * k / 255);
    if (r < p[0]) r = p[0];
    if (g < p[1]) g = p[1];
    if (b < p[2]) b = p[2];
    p[0] = r; p[1] = g; p[2] = b;
  };
  auto allAt = [&](uint8_t k) { for (uint16_t i = 0; i < count_; i++) put((int)i, k); };

  const uint8_t full = 255;
  if (!effects_) {
    allAt(full);
  } else if (shown == WS_STORM) {
    // Burza: ciemnoniebieskie tło + błyski (podwójne uderzenie pioruna)
    uint32_t cyc = t % 4000;
    bool flash = (cyc < 70) || (cyc > 130 && cyc < 200);
    if (flash) {
      uint8_t r = 255, g = 255, b = 255;
      for (uint16_t i = 0; i < count_; i++) {
        uint8_t* p = s_buf + (size_t)i * 3;
        uint8_t k = (uint8_t)random(210, 256);
        p[0] = (uint8_t)((uint16_t)r * k / 255);
        p[1] = (uint8_t)((uint16_t)g * k / 255);
        p[2] = (uint8_t)((uint16_t)b * k / 255);
      }
    } else {
      uint8_t k = (uint8_t)(150 + 60 * sinf(t / 700.0f));
      allAt(k);
    }
  } else if (shown == WS_RAIN) {
    // Deszcz: kropla biegnąca po pierścieniu, reszta przygaszona
    uint8_t dim = 70;
    allAt(dim);
    int head = (int)((t / 90) % count_);
    for (int i = 1; i <= 3; i++) put(head - i + 1, (uint8_t)(255 - (i - 1) * 70));
  } else if (shown == WS_SNOW) {
    // Śnieg: dwie białe płatki wędrujące w przeciwnych kierunkach
    allAt(45);
    int a = (int)((t / 700) % count_);
    int b = (int)(count_ - ((t / 1100) % count_));
    put(a, full); put(a - 1, 160); put(b, full); put(b + 1, 160);
  } else if (shown == WS_WIND) {
    // Wiatr: dwa komety wirujące dookoła
    allAt(30);
    int head = (int)((t / 70) % count_);
    for (int i = 0; i < 5; i++) put(head - i, (uint8_t)(255 - i * 45));
    int head2 = head + count_ / 2;
    for (int i = 0; i < 5; i++) put(head2 - i, (uint8_t)(255 - i * 45));
  } else if (shown == WS_HEAT) {
    // Upał: wyraźne pulsowanie
    uint8_t k = (uint8_t)(170 + 85 * sinf(t / 500.0f));
    allAt(k);
  } else if (shown == WS_ERROR) {
    // Brak danych: stałe, przygaszone fioletowe światło (bez mrugania)
    allAt(60);
  } else {
    // Ładna pogoda / zachmurzenie / mgła - spokojne oddychanie
    float amp = (shown == WS_GOOD) ? 45.0f : 30.0f;
    float base = (shown == WS_FOG) ? 170.0f : 210.0f;
    uint8_t k = (uint8_t)constrain(base + amp * sinf(t / 1400.0f), 0.0f, 255.0f);
    allAt(k);
  }

  // Ograniczenie prądu: 1 dioda na pełnej bieli ~60 mA (20 mA na kanał).
  // Sumujemy jasność wszystkich kanałów i w razie potrzeby przygaszamy całość.
  uint32_t sum = 0;
  for (uint16_t i = 0; i < count_; i++) {
    const uint8_t* p = s_buf + (size_t)i * 3;
    sum += (uint32_t)p[0] + p[1] + p[2];
  }
  uint32_t estMa = (sum * 20u) / 255u;
  uint8_t limitPct = 100;
  if (estMa > maxMa_ && estMa > 0) limitPct = (uint8_t)constrain((int)((uint32_t)maxMa_ * 100u / estMa), 1, 100);
  lastMa_ = (uint16_t)((estMa * pct * limitPct) / 10000u);

  for (uint16_t i = 0; i < count_; i++) {
    uint8_t* p = s_buf + (size_t)i * 3;
    uint8_t r = (uint8_t)((uint32_t)p[0] * pct * limitPct / 10000u);
    uint8_t g = (uint8_t)((uint32_t)p[1] * pct * limitPct / 10000u);
    uint8_t b = (uint8_t)((uint32_t)p[2] * pct * limitPct / 10000u);
    s_strip->setPixelColor(i, s_strip->Color(r, g, b));
  }
  s_strip->show();
}

void LedRing::loop() {
  if (!s_strip) return;
  uint32_t now = millis();
  if (preview_ == kPreviewNone && now - lastEval_ > 2000) {
    lastEval_ = now;
    evaluateWeather();
  }
  // 25 klatek na sekundę wystarczy, a nie blokuje Wi-Fi (przerwania w show())
  if (now - lastRender_ >= 40) {
    lastRender_ = now;
    render(now);
  }
}

// ------------------------------------------------------------
//  Opis stanu dla strony www
// ------------------------------------------------------------
String LedRing::stateName() const {
  size_t i = (size_t)constrain((int)state_, 0, (int)WEATHER_STATE_COUNT - 1);
  return config.rgbName((uint8_t)i);
}

uint32_t LedRing::stateColor() const {
  return stateColorAt((uint8_t)constrain((int)state_, 0, (int)WEATHER_STATE_COUNT - 1));
}

const char* LedRing::orderName() const {
  return order_ == 1 ? "RGB" : (order_ == 2 ? "BRG" : "GRB");
}

// Najgorszy przypadek: wszystkie diody na pełnej bieli (~60 mA na diodę).
uint16_t LedRing::maxMilliAmps() const {
  uint32_t ma = (uint32_t)count_ * 60u * brightness_ / 100u;
  return (uint16_t)(ma > 65535u ? 65535u : ma);
}

String LedRing::toJson() {
  PsramAllocator alloc;
  JsonDocument d(&alloc);
  d["enabled"] = enabled_;
  d["brightness"] = brightness_;
  d["effects"] = effects_;
  d["off_at_night"] = offAtNight_;
  d["count"] = count_;
  d["order"] = order_;
  d["max_ma"] = maxMa_;
  d["pin"] = pin_;
  d["state"] = (uint8_t)state_;
  d["state_name"] = stateName();
  d["reason"] = lastReason_;
  char hex[8];
  snprintf(hex, sizeof(hex), "#%06X", (unsigned)stateColor());
  d["color"] = hex;
  d["power_ma"] = (unsigned)lastMa_;
  d["power_max_ma"] = (unsigned)maxMilliAmps();
  d["preview"] = preview_;
  if (!s_strip) d["error"] = "sterownik nieaktywny (sprawdź pin i liczbę diod)";
  JsonArray legend = d["legend"].to<JsonArray>();
  for (size_t i = 0; i < WEATHER_STATE_COUNT; i++) {
    JsonObject o = legend.add<JsonObject>();
    o["name"] = config.rgbName((uint8_t)i);           // własna nazwa (jeśli jest)
    o["default_name"] = WEATHER_STATE_NAMES[i];       // nazwa domyślna - do przywracania
    o["custom"] = config.rgbNameCustom((uint8_t)i);
    snprintf(hex, sizeof(hex), "#%06X", (unsigned)stateColorAt((uint8_t)i));
    o["color"] = hex;
    o["rgb"] = stateColorAt((uint8_t)i);
    o["default"] = WEATHER_COLOR_DEFAULTS[i];
  }
  String s;
  serializeJson(d, s);
  return s;
}
