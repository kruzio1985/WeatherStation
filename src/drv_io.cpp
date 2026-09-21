/* =============================================================================
 * Stacja Pogody - stacja meteorologiczna na ESP32-S3 WROOM-1 (N16R8)
 * Copyright (c) 2026 kruzio1985 - https://github.com/kruzio1985
 * Licencja: PolyForm Noncommercial License 1.0.0 (plik LICENSE)
 * Użytek niekomercyjny. Kontakt: kruzio1985@users.noreply.github.com
 * =============================================================================
 */
#include "drv_io.h"
#include "i2c_util.h"
#include "syslog.h"
#include "pinmap.h"
#include "config.h"
#include <stdio.h>
#include <math.h>

// =============================================================
//  Czujniki na pojedynczych liniach GPIO + ekspandery I2C.
//  Opis modułu, kanałów i kluczy kalibracji - patrz drv_io.h.
//
//  Wszystkie odczyty robimy ręcznie (bit-bang), bez bibliotek
//  zewnętrznych. Sekcje czasowe (DHT, HX711) są krótkie: DHT to
//  ~5 ms z wyłączonymi przerwaniami, HX711 ~50 us na konwersję.
// =============================================================

// ---------- stałe czasowe i zakresy ----------
#define DHT_START_LONG_MS   18      // sygnał startowy DHT11 (min. 18 ms)
#define DHT_START_SHORT_MS  1       // sygnał startowy DHT22 / AM2302 / AM2320
#define DHT_INTERVAL_11_MS  2000    // DHT11: max 1 odczyt na 2 s
#define DHT_INTERVAL_22_MS  1000    // DHT22: max 1 odczyt na 1 s
#define DHT_BIT_THRESHOLD   45      // > 45 us impulsu wysokiego = bit 1

#define HX_INTERVAL_MS      100     // HX711: max 10 odczytów na sekundę
#define HX_READY_TIMEOUT_MS 400     // czekanie na pierwsze dane po starcie
#define HX_SAT_MAX          0x7FFFFF    // przetwornik nasycony (tensometr odłączony)
#define HX_SAT_MIN          (-0x800000)

#define US_INTERVAL_MS      250     // ultradźwięki: max 4 pomiary na sekundę
#define US_MIN_CM           2.0f    // zakres poprawnych pomiarów modułu
#define US_MAX_CM           400.0f
#define US_TIMEOUT_US       20000   // pulseIn z limitem 20 ms (blokowanie odczytu)
#define US_CM_PER_US        (1.0f / 58.31f)   // prędkość dźwięku ~343 m/s

#define EXP_ADDR_MIN        0x20    // MCP23017: adresy 0x20..0x27
#define EXP_ADDR_MAX        0x27

// ---------- rejestry MCP23017 (BANK = 0) ----------
#define MCP_IODIRA          0x00
#define MCP_IODIRB          0x01
#define MCP_GPINTENA        0x04
#define MCP_GPINTENB        0x05
#define MCP_INTCONA         0x08
#define MCP_INTCONB         0x09
#define MCP_IOCON           0x0A
#define MCP_GPPUA           0x0C
#define MCP_GPPUB           0x0D
#define MCP_GPIOA           0x12

// ---------- klucze kalibracji (pamięć ustawień, sekcja "extra") ----------
#define KEY_HX_G_CNT        "hx_g_cnt"      // gramy na jednostkę zliczeń
#define KEY_US_EMPTY        "us_empty_cm"   // odległość dla 0 % poziomu
#define KEY_US_FULL         "us_full_cm"    // odległość dla 100 % poziomu

// ---------- tabela kanałów modułu ----------
const ChanDef chansIo[] = {
    // DHT11 / DHT22 / AM2302 / AM2320 - pomiar na zewnątrz
    { "dht_t",   "Temperatura (DHT)",             "°C", 1, "out", "temperature", "°C", "mdi:thermometer"       },
    { "dht_h",   "Wilgotność (DHT)",              "%",  1, "out", "humidity",    "%",  "mdi:water-percent"     },
    { "dht_hi",  "Odczuwalna (DHT)",              "°C", 1, "out", "",            "°C", "mdi:thermometer-lines" },
    // HX711 - tensometr (waga deszczomierza wagowego, gleba, sila)
    { "hx_raw",  "Tensometr (zliczenia)",         "",   0, "out", "",            "",   "mdi:scale"             },
    { "hx_w",    "Waga tensometru",               "kg", 3, "out", "weight",      "kg", "mdi:weight-kilogram"   },
    // HC-SR04 / JSN-SR04T - odległość i poziom (woda, śnieg)
    { "us_dist", "Odległość (ultradźwięki)",      "cm", 1, "out", "distance",    "cm", "mdi:ruler"             },
    { "us_lvl",  "Poziom (ultradźwięki)",         "%",  0, "out", "",            "%",  "mdi:waves"             },
    // MCP23017 / PCF857x - stan ośmiu wejść
    { "mcp_in0", "Wejście ekspandera #1",         "",   0, "in",  "",            "",   "mdi:electric-switch"   },
    { "mcp_in1", "Wejście ekspandera #2",         "",   0, "in",  "",            "",   "mdi:electric-switch"   },
    { "mcp_in2", "Wejście ekspandera #3",         "",   0, "in",  "",            "",   "mdi:electric-switch"   },
    { "mcp_in3", "Wejście ekspandera #4",         "",   0, "in",  "",            "",   "mdi:electric-switch"   },
    { "mcp_in4", "Wejście ekspandera #5",         "",   0, "in",  "",            "",   "mdi:electric-switch"   },
    { "mcp_in5", "Wejście ekspandera #6",         "",   0, "in",  "",            "",   "mdi:electric-switch"   },
    { "mcp_in6", "Wejście ekspandera #7",         "",   0, "in",  "",            "",   "mdi:electric-switch"   },
    { "mcp_in7", "Wejście ekspandera #8",         "",   0, "in",  "",            "",   "mdi:electric-switch"   },
    // Liczniki zmian stanu (cykl odczytu stacji to 5 s - impulsy krótsze
    // od cyklu zliczamy tylko wtedy, gdy ekspander zgłosi je linią INT)
    { "mcp_evt0", "Licznik zdarzeń ekspandera #1", "",  0, "in",  "",            "",   "mdi:counter"           },
    { "mcp_evt1", "Licznik zdarzeń ekspandera #2", "",  0, "in",  "",            "",   "mdi:counter"           },
    { "mcp_evt2", "Licznik zdarzeń ekspandera #3", "",  0, "in",  "",            "",   "mdi:counter"           },
    { "mcp_evt3", "Licznik zdarzeń ekspandera #4", "",  0, "in",  "",            "",   "mdi:counter"           },
    { "mcp_evt4", "Licznik zdarzeń ekspandera #5", "",  0, "in",  "",            "",   "mdi:counter"           },
    { "mcp_evt5", "Licznik zdarzeń ekspandera #6", "",  0, "in",  "",            "",   "mdi:counter"           },
    { "mcp_evt6", "Licznik zdarzeń ekspandera #7", "",  0, "in",  "",            "",   "mdi:counter"           },
    { "mcp_evt7", "Licznik zdarzeń ekspandera #8", "",  0, "in",  "",            "",   "mdi:counter"           },
};
const uint16_t N = (uint16_t)(sizeof(chansIo) / sizeof(chansIo[0]));

// =============================================================
//  Pomocniki wspólne
// =============================================================

// Czekanie na zadany stan linii z limitem czasu (bez delay() - mikrosekundy)
static inline bool waitLevel(int pin, uint8_t level, uint16_t timeoutUs) {
  const uint32_t start = micros();
  while (digitalRead(pin) != level) {
    if ((uint32_t)(micros() - start) > timeoutUs) return false;
  }
  return true;
}

// Odczuwalna temperatura (heat index, wzór Rothfusa - liczony w °F).
// Poniżej 26,7 °C odczuwalna jest praktycznie równa temperaturze suchej,
// więc nie publikujemy liczby różniącej się tylko szumem pomiaru.
static float heatIndex(float t, float rh) {
  if (isnan(t) || isnan(rh)) return NAN;
  if (t < 26.7f || rh < 40.0f) return t;
  const float T = t * 9.0f / 5.0f + 32.0f;
  const float hi = -42.379f + 2.04901523f * T + 10.14333127f * rh
                   - 0.22475541f * T * rh - 6.83783e-3f * T * T
                   - 5.481717e-2f * rh * rh + 1.22874e-3f * T * T * rh
                   + 8.5282e-4f * T * rh * rh - 1.99e-6f * T * T * rh * rh;
  return (hi - 32.0f) * 5.0f / 9.0f;
}

// Rozpoznanie wariantu i rozkodowanie 5 bajtów odpowiedzi DHT.
// DHT11 przesyła wilgotność i temperaturę jako liczby całkowite (bajty 1
// i 3 są zerowe), DHT22/AM2302/AM2320 jako 16-bitowe wartości 0,1.
static DhtKind decodeDht(const uint8_t* d, float& tempC, float& rh) {
  if ((uint8_t)(d[0] + d[1] + d[2] + d[3]) != d[4]) return DHT_NONE;

  // DHT11: bajt 3 to tylko znak temperatury (0x00 / 0x80)
  if (d[1] == 0 && (d[3] == 0x00 || d[3] == 0x80) && d[0] <= 100 && d[2] <= 80) {
    rh = (float)d[0];
    tempC = (d[3] == 0x80) ? -(float)d[2] : (float)d[2];
    return DHT_11;
  }

  // DHT22 / AM2302 / AM2320 (starsze AM2320 odpowiada jak DHT22)
  const float rh22 = (float)(((uint16_t)d[0] << 8) | d[1]) * 0.1f;
  const float t22 = (float)(int16_t)(((uint16_t)d[2] << 8) | d[3]) * 0.1f;
  if (rh22 <= 100.0f && t22 >= -40.0f && t22 <= 80.0f) {
    rh = rh22;
    tempC = t22;
    return DHT_22;
  }
  return DHT_NONE;
}

// =============================================================
//  DHT11 / DHT22 / AM2302 / AM2320 - protokół jednoprzewodowy
// =============================================================

// Odbiór odpowiedzi czujnika - przerwania wyłączone, bo cała transmisja
// (przerwa ~80 us i 40 bitów po 50..90 us) musi zmieścić się w ~5 ms.
static bool dhtReadResponse(int pin, uint8_t* data) {
  if (!waitLevel(pin, LOW, 200)) return false;
  if (!waitLevel(pin, HIGH, 200)) return false;
  if (!waitLevel(pin, LOW, 200)) return false;

  const uint32_t deadline = micros() + 5000;
  for (uint8_t i = 0; i < 5; i++) {
    uint8_t v = 0;
    for (uint8_t bit = 0; bit < 8; bit++) {
      if (!waitLevel(pin, HIGH, 90)) return false;      // koniec przerwy ~50 us
      const uint32_t t0 = micros();
      if (!waitLevel(pin, LOW, 90)) return false;       // długość impulsu = wartość bitu
      v = (uint8_t)((v << 1) | ((uint32_t)(micros() - t0) > DHT_BIT_THRESHOLD ? 1u : 0u));
    }
    data[i] = v;
    if ((int32_t)(micros() - deadline) > 0) return false;
  }
  return true;
}

bool DhtGpio::readRaw(uint8_t* data, uint8_t startMs) {
  // Sygnał startowy: linia nisko. delay() (a nie delayMicroseconds) oddaje
  // w tym czasie procesor - 18 ms to za długo na pętlę zajętego czekania.
  pinMode(pin_, OUTPUT);
  digitalWrite(pin_, LOW);
  delay(startMs);
  digitalWrite(pin_, HIGH);
  pinMode(pin_, INPUT_PULLUP);

  noInterrupts();
  const bool ok = dhtReadResponse(pin_, data);
  interrupts();
  return ok;
}

bool DhtGpio::begin(int pin) {
  pin_ = pin;
  kind_ = DHT_NONE;
  intervalMs_ = DHT_INTERVAL_11_MS;
  pinMode(pin_, INPUT_PULLUP);
  delay(5);

  // Najpierw sygnał startowy DHT11 (18 ms), potem krótki (1 ms) - DHT22
  // i AM2302 wolą krótki impuls, więc druga próba rozstrzyga typ układu.
  uint8_t d[5];
  for (uint8_t attempt = 0; attempt < 2 && kind_ == DHT_NONE; attempt++) {
    float t = 0, h = 0;
    if (readRaw(d, attempt == 0 ? DHT_START_LONG_MS : DHT_START_SHORT_MS)) {
      kind_ = decodeDht(d, t, h);
      if (kind_ == DHT_11) intervalMs_ = DHT_INTERVAL_11_MS;
      else if (kind_ == DHT_22) intervalMs_ = DHT_INTERVAL_22_MS;
    }
    if (kind_ == DHT_NONE) delay(20);
  }
  lastMs_ = millis();
  return kind_ != DHT_NONE;
}

bool DhtGpio::due(uint32_t now) {
  if ((uint32_t)(now - lastMs_) < intervalMs_) return false;
  lastMs_ = now;
  return true;
}

bool DhtGpio::read(float& tempC, float& rh) {
  if (pin_ < 0) return false;
  uint8_t d[5];
  const uint8_t startMs = (kind_ == DHT_11) ? DHT_START_LONG_MS : DHT_START_SHORT_MS;
  if (!readRaw(d, startMs)) return false;
  float t = 0, h = 0;
  if (decodeDht(d, t, h) == DHT_NONE) return false;
  tempC = t;
  rh = h;
  return true;
}

const char* DhtGpio::kindName() const {
  return (kind_ == DHT_11) ? "DHT11" : "DHT22/AM2302/AM2320";
}

// =============================================================
//  HX711 - 24-bitowy przetwornik tensometru
// =============================================================

bool Hx711::begin(int dtPin, int sckPin) {
  dt_ = dtPin;
  sck_ = sckPin;
  ok_ = false;

  // SCK w stanie niskim budzi przetwornik (wysoki > 60 us to tryb power-down)
  pinMode(sck_, OUTPUT);
  digitalWrite(sck_, LOW);
  pinMode(dt_, INPUT);

  // Wykrywanie: linia DT ma być wysoka (dane niegotowe), a po konwersji
  // choć jeden z dwóch odczytów musi być różny od zera i od nasycenia.
  for (uint8_t i = 0; i < 2 && !ok_; i++) {
    const uint32_t t0 = millis();
    while (!ready() && (uint32_t)(millis() - t0) < HX_READY_TIMEOUT_MS) delay(1);
    int32_t v = 0;
    if (read(v) && v != 0 && v != HX_SAT_MAX && v != HX_SAT_MIN) ok_ = true;
  }
  return ok_;
}

bool Hx711::ready() const {
  return dt_ >= 0 && digitalRead(dt_) == LOW;
}

bool Hx711::read(int32_t& raw) {
  if (dt_ < 0 || sck_ < 0) return false;

  // 24 bity danych + 25. impuls wybierający kanał A i wzmocnienie 128.
  // Przerwania wyłączone na ~50 us, żeby nie przesunąć zboczy zegara.
  uint32_t value = 0;
  noInterrupts();
  for (uint8_t i = 0; i < 24; i++) {
    digitalWrite(sck_, HIGH);
    delayMicroseconds(1);
    value = (value << 1) | (uint32_t)(digitalRead(dt_) ? 1u : 0u);
    digitalWrite(sck_, LOW);
    delayMicroseconds(1);
  }
  digitalWrite(sck_, HIGH);
  delayMicroseconds(1);
  digitalWrite(sck_, LOW);
  interrupts();

  raw = (int32_t)(value << 8) >> 8;   // rozszerzenie znaku z 24 bitów
  return true;
}

// =============================================================
//  HC-SR04 / JSN-SR04T / A02YYUW - ultradźwięki TRIG/ECHO
//  (moduły A02YYUW w wersji z liniami TRIG/ECHO, nie UART)
// =============================================================

bool UltraSonic::begin(int trigPin, int echoPin) {
  trig_ = trigPin;
  echo_ = echoPin;
  ok_ = false;

  pinMode(trig_, OUTPUT);
  digitalWrite(trig_, LOW);
  pinMode(echo_, INPUT);

  for (uint8_t i = 0; i < 3 && !ok_; i++) {
    float cm = 0;
    if (read(cm) && cm >= US_MIN_CM && cm <= US_MAX_CM) ok_ = true;
    if (!ok_) delay(60);   // JSN-SR04T potrzebuje ~60 ms na wygaszenie echa
  }
  return ok_;
}

bool UltraSonic::read(float& cm) {
  if (trig_ < 0 || echo_ < 0) return false;

  digitalWrite(trig_, LOW);
  delayMicroseconds(3);
  digitalWrite(trig_, HIGH);
  delayMicroseconds(10);   // 10 us wyzwolenia
  digitalWrite(trig_, LOW);

  // Limit 20 ms oznacza pomiar do ~340 cm; dalsze cele przekraczają
  // dopuszczalny czas blokowania odczytu (400 cm to echo 23 ms).
  const unsigned long us = pulseIn(echo_, HIGH, US_TIMEOUT_US);
  if (us == 0) return false;

  cm = (float)us * US_CM_PER_US;
  return true;
}

// =============================================================
//  MCP23017 / PCF8574 / PCF8575 - ekspandery wejść na I2C
// =============================================================

// Flaga zgłaszana przez linię INT ekspandera (krótka sekcja ISR)
static volatile bool s_expInt = false;

static void IRAM_ATTR isrExpander() {
  s_expInt = true;
}

bool IoExpander::begin(int intPin) {
  intPin_ = intPin;
  kind_ = IOEXP_NONE;
  addr_ = 0;

  for (uint8_t a = EXP_ADDR_MIN; a <= EXP_ADDR_MAX; a++) {
    if (!i2cuPresent(a)) continue;

    // MCP23017 ma rejestry - dwa różne bajty zapisane do IODIRA/IODIRB
    // muszą wrócić przy odczycie. PCF857x nie ma rejestrów, więc ten test
    // zawsze wypadnie fałszywie.
    bool isMcp = false;
    if (i2cuWriteReg(a, MCP_IODIRA, 0xAA) && i2cuWriteReg(a, MCP_IODIRB, 0x55)) {
      uint8_t b[2] = { 0, 0 };
      if (i2cuReadReg(a, MCP_IODIRA, b, 2) && b[0] == 0xAA && b[1] == 0x55) isMcp = true;
    }

    if (isMcp) {
      kind_ = IOEXP_MCP23017;
    } else {
      // PCF8574 (8 bitów) i PCF8575 (16 bitów): zapis 0xFF ustawia linie
      // jako wejścia (wysoki = wejście z podciąganiem).
      Wire.beginTransmission(a);
      Wire.write(0xFF);
      Wire.write(0xFF);
      if (Wire.endTransmission() != 0) continue;
      kind_ = IOEXP_PCF857X;
    }
    addr_ = a;
    break;
  }

  if (kind_ == IOEXP_MCP23017) {
    i2cuWriteReg(addr_, MCP_IOCON, 0x00);      // BANK=0, sekwencyjny odczyt
    i2cuWriteReg(addr_, MCP_IODIRA, 0xFF);     // wszystkie linie wejściami
    i2cuWriteReg(addr_, MCP_IODIRB, 0xFF);
    i2cuWriteReg(addr_, MCP_GPPUA, 0xFF);      // podciąganie - wolne piny nie wiszą
    i2cuWriteReg(addr_, MCP_GPPUB, 0xFF);
    i2cuWriteReg(addr_, MCP_INTCONA, 0x00);    // przerwanie przy każdej zmianie
    i2cuWriteReg(addr_, MCP_INTCONB, 0x00);
    i2cuWriteReg(addr_, MCP_GPINTENA, 0xFF);   // zgłaszamy tylko port A (8 wejść)
    i2cuWriteReg(addr_, MCP_GPINTENB, 0x00);
    uint8_t dummy[2];
    i2cuReadReg(addr_, MCP_GPIOA, dummy, 2);   // odczyt kasuje zgłoszenie z konfiguracji
  }

  // Linia INT jest wspólna dla wszystkich zdarzeń - zbieramy samą informację
  // "coś się zmieniło", a stan wejść i tak czytamy z układu.
  if (kind_ != IOEXP_NONE && intPin_ >= 0) {
    pinMode(intPin_, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(intPin_), isrExpander, FALLING);
  }
  return kind_ != IOEXP_NONE;
}

bool IoExpander::readInputs(uint8_t& bits) {
  if (kind_ == IOEXP_MCP23017) {
    uint8_t b[2] = { 0, 0 };
    if (!i2cuReadReg(addr_, MCP_GPIOA, b, 2)) return false;   // odczyt portu A kasuje INT
    bits = b[0];
    return true;
  }
  if (kind_ == IOEXP_PCF857X) {
    uint8_t b[1] = { 0 };
    if (!i2cuRead(addr_, b, 1)) return false;
    bits = b[0];
    return true;
  }
  return false;
}

bool IoExpander::tookInterrupt() {
  if (!s_expInt) return false;
  s_expInt = false;
  return true;
}

const char* IoExpander::kindName() const {
  return (kind_ == IOEXP_MCP23017) ? "MCP23017" : "PCF857x";
}

// =============================================================
//  Moduł sterownika (szkielet dla SensorManagera)
// =============================================================

static DrvSink s_sink;
static bool s_started = false;

// Każdy czujnik niezależny - brak sprzętu = brak kanałów, bez błędu
static DhtGpio s_dht;
static Hx711 s_hx;
static UltraSonic s_us;
static IoExpander s_exp;

// Liczniki nieudanych odczytów (3 z rzędu = kanały wstrzymane)
static uint8_t s_dhtFail = 0;
static uint8_t s_hxFail = 0;
static uint8_t s_usFail = 0;
static uint8_t s_expFail = 0;
static bool s_hxWarned = false;    // ostrzeżenie o braku kalibracji wagi
static bool s_usWarned = false;    // ostrzeżenie o braku kalibracji poziomu
static uint32_t s_hxLastMs = 0;
static uint32_t s_usLastMs = 0;

// Stan ekspandera: ostatnio widoczne wejścia i liczniki zmian
static uint8_t s_expBits = 0;
static bool s_expFirst = true;
static uint32_t s_expEvt[8] = { 0 };

static void beginIo(const DrvSink& sink) {
  s_sink = sink;
  s_started = true;

  // ---------- DHT11 / DHT22 / AM2302 / AM2320 ----------
  const int dhtPin = pinMap.pin("dht");
  if (dhtPin >= 0) {
    if (s_dht.begin(dhtPin)) {
      s_sink.found("dht_t");
      s_sink.found("dht_h");
      s_sink.found("dht_hi");
      LOG_I("DHT: wykryty %s na GPIO%d (temperatura, wilgotność, odczuwalna)",
            s_dht.kindName(), dhtPin);
    } else {
      LOG_W("DHT: brak odpowiedzi na GPIO%d - sprawdź podciąganie 4,7 kΩ, zasilanie i GND", dhtPin);
    }
  } else {
    LOG_I("DHT: pin danych nieprzypisany - czujnik pominięty");
  }

  // ---------- HX711 ----------
  const int hxDt = pinMap.pin("hx711_dt");
  const int hxSck = pinMap.pin("hx711_sck");
  if (hxDt >= 0 && hxSck >= 0) {
    if (s_hx.begin(hxDt, hxSck)) {
      s_hxLastMs = millis();
      s_sink.found("hx_raw");
      if (config.hasExtra(KEY_HX_G_CNT)) s_sink.found("hx_w");
      LOG_I("HX711: wykryty na DT=GPIO%d SCK=GPIO%d (zliczenia%s)",
            hxDt, hxSck, config.hasExtra(KEY_HX_G_CNT) ? " i waga" : "; waga czeka na kalibrację");
    } else {
      LOG_W("HX711: brak danych na DT=GPIO%d / SCK=GPIO%d - tensometr pominięty", hxDt, hxSck);
    }
  } else {
    LOG_I("HX711: piny DT/SCK nieprzypisane - tensometr pominięty");
  }

  // ---------- HC-SR04 / JSN-SR04T / A02YYUW ----------
  const int usTrig = pinMap.pin("us_trig");
  const int usEcho = pinMap.pin("us_echo");
  if (usTrig >= 0 && usEcho >= 0) {
    if (s_us.begin(usTrig, usEcho)) {
      s_usLastMs = millis();
      s_sink.found("us_dist");
      const bool usCal = config.hasExtra(KEY_US_EMPTY) && config.hasExtra(KEY_US_FULL);
      if (usCal) s_sink.found("us_lvl");
      LOG_I("Ultradźwięki: wykryte na TRIG=GPIO%d ECHO=GPIO%d (odległość%s)",
            usTrig, usEcho, usCal ? " i poziom" : "; poziom czeka na kalibrację");
    } else {
      LOG_W("Ultradźwięki: brak poprawnego pomiaru (%.0f..%.0f cm) na TRIG=GPIO%d ECHO=GPIO%d",
            US_MIN_CM, US_MAX_CM, usTrig, usEcho);
    }
  } else {
    LOG_I("Ultradźwięki: piny TRIG/ECHO nieprzypisane - czujnik pominięty");
  }

  // ---------- Ekspandery wejść (MCP23017 / PCF8574 / PCF8575) ----------
  const int expInt = pinMap.pin("exp_int");
  if (pinMap.pin("i2c_sda") < 0 || pinMap.pin("i2c_scl") < 0) {
    LOG_I("Ekspander I2C: magistrala nieprzypisana - ekspandery pominięte");
  } else if (s_exp.begin(expInt)) {
    for (uint8_t i = 0; i < 8; i++) {
      char id[16];
      snprintf(id, sizeof(id), "mcp_in%u", (unsigned)i);
      s_sink.found(id);
      snprintf(id, sizeof(id), "mcp_evt%u", (unsigned)i);
      s_sink.found(id);
    }
    LOG_I("Ekspander %s: wykryty pod adresem 0x%02X, 8 wejść%s",
          s_exp.kindName(), s_exp.addr(),
          expInt >= 0 ? " + linia INT" : " (bez linii INT - tylko odczyt cykliczny)");
  } else {
    LOG_W("Ekspander I2C: brak układu pod adresem 0x%02X..0x%02X - wejścia pominięte",
          EXP_ADDR_MIN, EXP_ADDR_MAX);
  }
}

static void readIo() {
  if (!s_started) return;

  // ---------- DHT: DHT11 max 1 odczyt / 2 s, DHT22 / 1 s ----------
  if (s_dht.ok() && s_dht.due(millis())) {
    float t = NAN, rh = NAN;
    if (s_dht.read(t, rh)) {
      s_dhtFail = 0;
      s_sink.publish("dht_t", t);
      s_sink.publish("dht_h", rh);
      const float hi = heatIndex(t, rh);
      if (!isnan(hi)) s_sink.publish("dht_hi", hi);
    } else {
      if (s_dhtFail < 3 && ++s_dhtFail == 3) {
        LOG_W("DHT: brak poprawnego odczytu (3 próby) - kanały wstrzymane");
      }
      s_sink.clear("dht_t");
      s_sink.clear("dht_h");
      s_sink.clear("dht_hi");
    }
  }

  // ---------- HX711: max 10 odczytów / s i tylko gdy dane gotowe ----------
  const uint32_t now = millis();
  if (s_hx.ok() && (uint32_t)(now - s_hxLastMs) >= HX_INTERVAL_MS) {
    if (s_hx.ready()) {
      s_hxLastMs = now;
      int32_t raw = 0;
      if (s_hx.read(raw)) {
        s_hxFail = 0;
        s_sink.publish("hx_raw", (float)raw);
        // Waga: przelicznik w gramach na jednostkę zliczeń (klucz "hx_g_cnt")
        if (config.hasExtra(KEY_HX_G_CNT)) {
          const float gPerCnt = config.extraF(KEY_HX_G_CNT, 0.0f);
          if (gPerCnt != 0.0f) {
            s_sink.publish("hx_w", (float)raw * gPerCnt / 1000.0f);
          } else if (!s_hxWarned) {
            s_hxWarned = true;
            LOG_W("HX711: \"%s\" wynosi 0 - kanał wagi nieaktywny", KEY_HX_G_CNT);
          }
        } else if (!s_hxWarned) {
          s_hxWarned = true;
          LOG_I("HX711: ustaw \"%s\" (gramy na jednostkę zliczeń), aby włączyć kanał wagi", KEY_HX_G_CNT);
        }
      } else if (s_hxFail < 3 && ++s_hxFail == 3) {
        LOG_W("HX711: brak odczytu (3 próby) - kanały wstrzymane");
        s_sink.clear("hx_raw");
        s_sink.clear("hx_w");
      }
    }
  }

  // ---------- Ultradźwięki: max 4 pomiary / s ----------
  if (s_us.ok() && (uint32_t)(now - s_usLastMs) >= US_INTERVAL_MS) {
    s_usLastMs = now;
    float cm = 0;
    if (s_us.read(cm) && cm >= US_MIN_CM && cm <= US_MAX_CM) {
      s_usFail = 0;
      s_sink.publish("us_dist", cm);
      // Poziom: odległość "pusto" i "pełno" z kalibracji (klucze us_*_cm)
      if (config.hasExtra(KEY_US_EMPTY) && config.hasExtra(KEY_US_FULL)) {
        const float empty = config.extraF(KEY_US_EMPTY, 0.0f);
        const float full = config.extraF(KEY_US_FULL, 0.0f);
        const float span = empty - full;
        if (fabsf(span) >= 1.0f) {
          float lvl = (empty - cm) / span * 100.0f;
          if (lvl < 0.0f) lvl = 0.0f;
          if (lvl > 100.0f) lvl = 100.0f;
          s_sink.publish("us_lvl", lvl);
        }
      } else if (!s_usWarned) {
        s_usWarned = true;
        LOG_I("Ultradźwięki: ustaw \"%s\" i \"%s\", aby włączyć kanał poziomu",
              KEY_US_EMPTY, KEY_US_FULL);
      }
    } else {
      if (s_usFail < 3 && ++s_usFail == 3) {
        LOG_W("Ultradźwięki: brak poprawnego pomiaru (3 próby) - kanały wstrzymane");
      }
      s_sink.clear("us_dist");
      s_sink.clear("us_lvl");
    }
  }

  // ---------- Ekspander: stan wejść i liczniki zmian ----------
  if (s_exp.ok()) {
    uint8_t bits = 0;
    if (s_exp.readInputs(bits)) {
      s_expFail = 0;
      const bool intHit = s_exp.tookInterrupt();
      bool changed = false;
      if (s_expFirst) {
        s_expBits = bits;      // pierwszy odczyt to stan odniesienia
        s_expFirst = false;
      } else if (bits != s_expBits) {
        changed = true;
        const uint8_t prev = s_expBits;
        for (uint8_t i = 0; i < 8; i++) {
          const uint8_t mask = (uint8_t)(1u << i);
          if ((bits & mask) != (prev & mask)) s_expEvt[i]++;
        }
        s_expBits = bits;
        LOG_I("Ekspander %s 0x%02X: zmiana wejść (0x%02X -> 0x%02X)%s",
              s_exp.kindName(), s_exp.addr(), prev, bits, intHit ? " [INT]" : "");
      }
      if (intHit && !changed) {
        LOG_I("Ekspander 0x%02X: impuls na linii INT bez trwałej zmiany stanu", s_exp.addr());
      }

      // Publikujemy stan i liczniki zawsze - to jedyne dane tego modułu
      char id[16];
      for (uint8_t i = 0; i < 8; i++) {
        const float bit = (bits & (uint8_t)(1u << i)) ? 1.0f : 0.0f;
        snprintf(id, sizeof(id), "mcp_in%u", (unsigned)i);
        s_sink.publish(id, bit);
        snprintf(id, sizeof(id), "mcp_evt%u", (unsigned)i);
        s_sink.publish(id, (float)s_expEvt[i]);
      }
    } else if (s_expFail < 3 && ++s_expFail == 3) {
      LOG_W("Ekspander 0x%02X: brak odczytu (3 próby) - wejścia wstrzymane", s_exp.addr());
      char id[16];
      for (uint8_t i = 0; i < 8; i++) {
        snprintf(id, sizeof(id), "mcp_in%u", (unsigned)i);
        s_sink.clear(id);
        snprintf(id, sizeof(id), "mcp_evt%u", (unsigned)i);
        s_sink.clear(id);
      }
    }
  }
}

// =============================================================
//  Rejestracja modułu (dopisanie do tabeli w src/drv_table.cpp
//  robi autor projektu - symbole w tabeli są słabe, więc build
//  działa także bez tego pliku)
// =============================================================
const DrvModule drvIo = { "GPIO (DHT, HX711, ultradźwięki, ekspandery)", chansIo, N, beginIo, readIo };
