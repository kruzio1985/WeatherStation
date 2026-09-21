/* =============================================================================
 * Stacja Pogody - stacja meteorologiczna na ESP32-S3 WROOM-1 (N16R8)
 * Copyright (c) 2026 kruzio1985 - https://github.com/kruzio1985
 * Licencja: PolyForm Noncommercial License 1.0.0 (plik LICENSE)
 * Użytek niekomercyjny. Kontakt: kruzio1985@users.noreply.github.com
 * =============================================================================
 */

// =============================================================
//  Moduł sterowników TERMOPARY / RTD / ADC na SPI.
//  Opis układów, pinów, kanałów i kluczy kalibracji - patrz drv_tc.h.
//
//  SPI robimy ręcznie (bit-bang), bez <SPI.h> i bez bibliotek
//  zewnętrznych, więc magistrala może siedzieć na dowolnych GPIO.
//
//    * MAX6675 / MAX31855 - zegar w spoczynku wysoko, dane próbkowane
//      zaraz po zboczu opadającym,
//    * MCP3008 / MCP3208  - zegar w spoczynku nisko, dane próbkowane
//      po zboczu narastającym.
//
//  Wykrywanie jest konserwatywne: czujniki nie są podłączone, więc wolimy
//  nie zgłosić układu niż zgłosić fałszywy. Log ostrzegawczy pojawia się
//  co najwyżej raz na serię błędów, nigdy w każdej iteracji.
//
//  Wartości publikujemy surowe (offsetów nie liczymy - robi to
//  SensorManager), a przy nieudanym odczycie czyścimy kanał.
// =============================================================
#include "drv_tc.h"
#include "config.h"
#include "pinmap.h"
#include "syslog.h"
#include <math.h>

// =============================================================
//  Kanały modułu (id unikalne w całym projekcie)
// =============================================================
static const ChanDef CHANS[] = {
  { "tc_max6675",  "Temperatura termopary (MAX6675)",       "°C", 2, "out", "temperature", "°C", "mdi:thermometer-lines" },
  { "tc_31855_t",  "Temperatura termopary (MAX31855)",      "°C", 2, "out", "temperature", "°C", "mdi:thermometer-lines" },
  { "tc_31855_cj", "Temperatura zimnych końców (MAX31855)", "°C", 2, "in",  "temperature", "°C", "mdi:thermometer"       },
  { "rtd_temp",    "Temperatura RTD (MAX31865)",            "°C", 2, "out", "temperature", "°C", "mdi:thermometer"       },
  { "rtd_res",     "Rezystancja RTD (MAX31865)",            "Ω",  2, "out", "",            "",   "mdi:resistor"          },
  { "adc_mcp_ch0", "Napięcie ADC ch0 (MCP3x08)",            "V",  3, "in",  "voltage",     "V",  "mdi:sine-wave"         },
  { "adc_mcp_ch1", "Napięcie ADC ch1 (MCP3x08)",            "V",  3, "in",  "voltage",     "V",  "mdi:sine-wave"         },
  { "adc_mcp_ch2", "Napięcie ADC ch2 (MCP3x08)",            "V",  3, "in",  "voltage",     "V",  "mdi:sine-wave"         },
  { "adc_mcp_ch3", "Napięcie ADC ch3 (MCP3x08)",            "V",  3, "in",  "voltage",     "V",  "mdi:sine-wave"         },
  { "adc_mcp_ch4", "Napięcie ADC ch4 (MCP3x08)",            "V",  3, "in",  "voltage",     "V",  "mdi:sine-wave"         },
  { "adc_mcp_ch5", "Napięcie ADC ch5 (MCP3x08)",            "V",  3, "in",  "voltage",     "V",  "mdi:sine-wave"         },
  { "adc_mcp_ch6", "Napięcie ADC ch6 (MCP3x08)",            "V",  3, "in",  "voltage",     "V",  "mdi:sine-wave"         },
  { "adc_mcp_ch7", "Napięcie ADC ch7 (MCP3x08)",            "V",  3, "in",  "voltage",     "V",  "mdi:sine-wave"         }
};

static const uint16_t NCHANS = sizeof(CHANS) / sizeof(CHANS[0]);

// Pierwszy kanał ADC w tabeli powyżej (kolejne to ch0..ch7)
#define CH_ADC0          5

// =============================================================
//  Stałe czasowe, zakresy i rejestry
// =============================================================
#define TC_READ_MS       250      // MAX6675 potrzebuje 220 ms na konwersję
#define TC_DET_FRAMES    3        // tyle ramek musi przejść test wykrywania
#define MCP_CFG_START    0x18     // start (bit4) + SGL/DIFF = 1 (wejście niesymetryczne)
#define MCP_CHANNELS     8

#define RTD_REG_CONFIG   0x00
#define RTD_REG_RTD_MSB  0x01
#define RTD_REG_RTD_LSB  0x02
#define RTD_CFG_BASE     0xC0     // VBIAS + tryb automatycznej konwersji
#define RTD_CFG_3WIRE    0x10
#define RTD_CFG_F50      0x01     // 1 = filtr 50 Hz, 0 = 60 Hz
#define RTD_MASK_CFG     0xDD     // bity konfiguracji bez D1 (kasowanie błędu - sam się zeruje)
#define RTD_CODE_MAX     32768.0f // 15 bitów danych RTD
#define RTD_RMAX_RATIO   4.0f     // górna granica sensownej rezystancji (R0 * 4)

// Termopary: litery i zakresy temperatur (ITS-90)
#define TC_TYPE_COUNT    7
static const char  TC_TYPES[TC_TYPE_COUNT] = { 'K', 'J', 'N', 'T', 'S', 'R', 'E' };
static const float TC_TMIN[TC_TYPE_COUNT]  = { -270.0f, -210.0f, -270.0f, -270.0f, -50.0f, -50.0f, -270.0f };
static const float TC_TMAX[TC_TYPE_COUNT]  = { 1372.0f, 1200.0f, 1300.0f,  400.0f, 1768.0f, 1768.0f, 1000.0f };

// Rodzaj wykrytej termopary
#define TC_KIND_NONE     0
#define TC_KIND_6675     1
#define TC_KIND_31855    2

// =============================================================
//  Stan modułu, piny i konfiguracja
// =============================================================
static DrvSink s_sink;

static int s_sck = -1, s_mosi = -1, s_miso = -1, s_tcCs = -1, s_rtdCs = -1, s_adcCs = -1;

static bool    s_tcOk  = false;
static uint8_t s_tcKind = TC_KIND_NONE;
static bool    s_adcOk = false;
static uint8_t s_adcBits = 10;          // 10 = MCP3008, 12 = MCP3208
static float   s_adcFull = 1023.0f;     // pełna skala przetwornika
static bool    s_rtdOk = false;
static bool    s_rtdMode1 = false;      // druga konwencja próbkowania MAX31865

static char    s_tcType = 'K';
static float   s_tcMin = -270.0f, s_tcMax = 1372.0f;
static float   s_rref = 430.0f, s_r0 = 100.0f;
static uint8_t s_rtdWires = 2, s_rtdFilter = 50;
static float   s_vref = 3.3f;

static uint32_t s_lastMs = 0;
static uint8_t  s_tcFail = 0, s_adcFail = 0, s_rtdFail = 0;

// ---------- pomocniki liczbowe ----------
// Rozszerzenie znaku dla pól 12- i 14-bitowych (temperatury Maximów)
static int32_t sgn12(int32_t v) { return (v & 0x0800) ? (v - 0x1000) : v; }
static int32_t sgn14(int32_t v) { return (v & 0x2000) ? (v - 0x4000) : v; }

// =============================================================
//  Konfiguracja (tylko odczyt - nic nie zapisujemy do NVS)
// =============================================================
// tc_type: albo litera termopary (kod ASCII), albo numer 0..6 na liście
static void cfgLoadType() {
  const float v = config.extraF("tc_type", (float)TC_TYPES[0]);
  uint8_t idx = 0;
  if (v >= 0.0f && v < 32.0f) {
    idx = (uint8_t)(v + 0.5f);
  } else {
    for (uint8_t i = 0; i < TC_TYPE_COUNT; i++) {
      if ((int)(v + 0.5f) == (int)TC_TYPES[i]) { idx = i; break; }
    }
  }
  if (idx >= TC_TYPE_COUNT) idx = 0;
  s_tcType = TC_TYPES[idx];
  s_tcMin = TC_TMIN[idx];
  s_tcMax = TC_TMAX[idx];
}

static void cfgLoad() {
  cfgLoadType();

  s_r0 = config.extraF("rtd_r0", 100.0f);
  if (!(s_r0 > 10.0f && s_r0 < 10000.0f)) s_r0 = 100.0f;

  // PT1000 ma w typowych modułach rezystor odniesienia 4,3 kΩ
  const float rdef = (s_r0 >= 500.0f) ? 4300.0f : 430.0f;
  s_rref = config.extraF("rtd_rref", rdef);
  if (!(s_rref > 10.0f && s_rref < 100000.0f)) s_rref = rdef;

  const int w = (int)(config.extraF("rtd_wires", 2.0f) + 0.5f);
  s_rtdWires = (w == 3) ? 3 : ((w == 4) ? 4 : 2);

  const int f = (int)(config.extraF("rtd_filter", 50.0f) + 0.5f);
  s_rtdFilter = (f == 60) ? 60 : 50;

  s_vref = config.extraF("adc_spi_vref", 3.3f);
  if (!(s_vref > 0.5f && s_vref < 5.5f)) s_vref = 3.3f;
}

// =============================================================
//  SPI programowe - termopary (MAX6675 / MAX31855)
//  Zegar w spoczynku wysoko, dane ważne zaraz po zboczu opadającym.
// =============================================================
static uint32_t tcReadFrame(uint8_t bits) {
  uint32_t v = 0;
  digitalWrite(s_sck, HIGH);
  digitalWrite(s_tcCs, LOW);
  delayMicroseconds(1);
  for (uint8_t i = 0; i < bits; i++) {
    digitalWrite(s_sck, LOW);
    delayMicroseconds(1);
    v = (v << 1) | (uint32_t)(digitalRead(s_miso) ? 1u : 0u);
    digitalWrite(s_sck, HIGH);
    delayMicroseconds(1);
  }
  digitalWrite(s_tcCs, HIGH);
  delayMicroseconds(1);
  return v;
}

// =============================================================
//  SPI programowe - MCP3008 / MCP3208
//  Zegar w spoczynku nisko, dane ważne po zboczu narastającym.
//  Zawsze zegarujemy 12 bitów danych (MCP3008 dosyła dwa bity
//  śmieciowe na końcu), żeby dało się rozróżnić oba układy.
// =============================================================
static void mcpRead(uint8_t ch, uint16_t* val, bool* nullBit) {
  const uint8_t cfg = (uint8_t)(MCP_CFG_START | (ch & 0x07));
  uint16_t v = 0;

  digitalWrite(s_sck, LOW);
  digitalWrite(s_mosi, LOW);
  digitalWrite(s_adcCs, LOW);
  delayMicroseconds(1);

  // 5 bitów konfiguracji: start, SGL/DIFF, D2, D1, D0
  for (int8_t i = 4; i >= 0; i--) {
    digitalWrite(s_mosi, ((cfg >> i) & 1) ? HIGH : LOW);
    delayMicroseconds(1);
    digitalWrite(s_sck, HIGH);
    delayMicroseconds(1);
    digitalWrite(s_sck, LOW);
    delayMicroseconds(1);
  }
  digitalWrite(s_mosi, LOW);

  // bit NULL - zawsze 0, to nasza kontrola struktury ramki
  digitalWrite(s_sck, HIGH);
  delayMicroseconds(1);
  const bool nb = digitalRead(s_miso) ? true : false;
  digitalWrite(s_sck, LOW);
  delayMicroseconds(1);

  for (uint8_t i = 0; i < 12; i++) {
    digitalWrite(s_sck, HIGH);
    delayMicroseconds(1);
    v = (uint16_t)((v << 1) | (digitalRead(s_miso) ? 1u : 0u));
    digitalWrite(s_sck, LOW);
    delayMicroseconds(1);
  }

  digitalWrite(s_adcCs, HIGH);
  delayMicroseconds(1);

  *val = v;
  *nullBit = nb;
}

// =============================================================
//  SPI programowe - MAX31865 (RTD)
//  Adres z bitem A7 = 1 to zapis, sam adres to odczyt.
//  Część modułów ma odwróconą fazę zegara, więc próbkowanie robimy
//  raz po zboczu narastającym (s_rtdMode1 = false), raz opadającym.
// =============================================================
static uint8_t rtdClockByte(uint8_t out) {
  uint8_t in = 0;
  for (int8_t i = 7; i >= 0; i--) {
    digitalWrite(s_sck, s_rtdMode1 ? HIGH : LOW);
    digitalWrite(s_mosi, ((out >> i) & 1) ? HIGH : LOW);
    delayMicroseconds(1);
    digitalWrite(s_sck, HIGH);
    if (!s_rtdMode1) {
      in = (uint8_t)((in << 1) | (digitalRead(s_miso) ? 1u : 0u));
      delayMicroseconds(1);
    }
    if (s_rtdMode1) {
      digitalWrite(s_sck, LOW);
      in = (uint8_t)((in << 1) | (digitalRead(s_miso) ? 1u : 0u));
      delayMicroseconds(1);
    }
  }
  return in;
}

static void rtdBeginTransfer() {
  digitalWrite(s_sck, s_rtdMode1 ? HIGH : LOW);
  digitalWrite(s_mosi, LOW);
  digitalWrite(s_rtdCs, LOW);
  delayMicroseconds(2);
}

static void rtdEndTransfer() {
  digitalWrite(s_rtdCs, HIGH);
  delayMicroseconds(2);
}

static void rtdWriteReg(uint8_t reg, uint8_t val) {
  rtdBeginTransfer();
  rtdClockByte((uint8_t)(0x80 | (reg & 0x7F)));
  rtdClockByte(val);
  rtdEndTransfer();
}

static uint8_t rtdReadReg(uint8_t reg) {
  rtdBeginTransfer();
  rtdClockByte((uint8_t)(reg & 0x7F));
  const uint8_t v = rtdClockByte(0x00);
  rtdEndTransfer();
  return v;
}

// Odczyt dwóch kolejnych rejestrów (bufor z auto-inkrementacją adresu)
static void rtdReadPair(uint8_t reg, uint8_t* b0, uint8_t* b1) {
  rtdBeginTransfer();
  rtdClockByte((uint8_t)(reg & 0x7F));
  *b0 = rtdClockByte(0x00);
  *b1 = rtdClockByte(0x00);
  rtdEndTransfer();
}

// =============================================================
//  Wykrywanie termopar (MAX6675 / MAX31855)
// =============================================================
// Ramka MAX6675: bit15 = 0 (dummy), bit2 = 1 -> termopara rozwarta,
// bit1 = 0 (identyfikacja układu). Po 16 bitach SO MAX6675 milczy albo
// powtarza ramkę, dlatego druga połowa nie jest sprawdzana wprost -
// wykluczamy tylko ramkę, która wygląda jak prawdziwy MAX31855.
static bool tcFrame6675(uint32_t w) {
  const uint16_t h = (uint16_t)(w >> 16);
  if (h == 0x0000u || h == 0xFFFFu) return false;   // linia MISO zwarta do masy / zasilania
  if (h & 0x8006u) return false;                    // D15 (dummy), D2 (obwód rozwarty), D1 (ID)
  return true;
}

// Ramka MAX31855: bit17 i bit16 (błąd) oraz bity 3..0 muszą być zerami,
// temperatura zimnych końców musi być sensowna, a temperatura termopary
// musi mieścić się w zakresie ustawionym przez tc_type. Wymagamy też, żeby
// obie połówki ramki się różniły - dzięki temu MAX6675 powtarzający swoją
// ramkę nie zostanie wzięty za MAX31855.
static bool tcFrame31855(uint32_t w) {
  if (w & 0x0003000Fu) return false;
  const uint16_t h = (uint16_t)(w >> 16);
  const uint16_t l = (uint16_t)(w & 0xFFFFu);
  if (h == l) return false;
  if (h == 0x0000u) return false;                   // linia MISO zwarta do masy
  const float cj = (float)sgn12((int32_t)((w >> 4) & 0x0FFFu)) * 0.0625f;
  if (cj < -50.0f || cj > 130.0f) return false;
  const float tc = (float)sgn14((int32_t)((w >> 18) & 0x3FFFu)) * 0.25f;
  if (tc < s_tcMin - 20.0f || tc > s_tcMax + 20.0f) return false;
  return true;
}

static void detectTc() {
  if (s_sck < 0 || s_miso < 0 || s_tcCs < 0) return;   // brak pinów - układ pomijany bez logu

  pinMode(s_sck, OUTPUT);
  digitalWrite(s_sck, HIGH);
  pinMode(s_tcCs, OUTPUT);
  digitalWrite(s_tcCs, HIGH);
  if (s_mosi >= 0) {
    pinMode(s_mosi, OUTPUT);
    digitalWrite(s_mosi, LOW);
  }
  pinMode(s_miso, INPUT);
  delayMicroseconds(10);

  uint32_t f[TC_DET_FRAMES];
  for (uint8_t i = 0; i < TC_DET_FRAMES; i++) {
    f[i] = tcReadFrame(32);
    delayMicroseconds(2);
  }

  bool f31855 = true, f6675 = true;
  for (uint8_t i = 0; i < TC_DET_FRAMES; i++) {
    if (!tcFrame31855(f[i])) f31855 = false;
    if (!tcFrame6675(f[i]))  f6675 = false;
  }
  // MAX6675 zgłaszamy tylko wtedy, gdy żadna ramka nie wygląda jak MAX31855
  if (!f31855) {
    for (uint8_t i = 0; i < TC_DET_FRAMES; i++) {
      if (tcFrame31855(f[i])) f6675 = false;
    }
  }

  if (f31855) {
    s_tcKind = TC_KIND_31855;
  } else if (f6675) {
    s_tcKind = TC_KIND_6675;
  } else {
    return;
  }

  s_tcOk = true;
  s_tcFail = 0;
  if (s_tcKind == TC_KIND_6675) {
    s_sink.found("tc_max6675");
    LOG_I("Termopara: MAX6675 (typ K) na CS=GPIO%d", s_tcCs);
  } else {
    s_sink.found("tc_31855_t");
    s_sink.found("tc_31855_cj");
    LOG_I("Termopara: MAX31855 (typ %c) na CS=GPIO%d - termopara i zimne końce", s_tcType, s_tcCs);
  }
}

// =============================================================
//  Wykrywanie ADC (MCP3008 / MCP3208)
// =============================================================
static void detectAdc() {
  if (s_sck < 0 || s_mosi < 0 || s_miso < 0 || s_adcCs < 0) return;  // brak pinów - pomijamy bez logu

  pinMode(s_sck, OUTPUT);
  digitalWrite(s_sck, LOW);
  pinMode(s_mosi, OUTPUT);
  digitalWrite(s_mosi, LOW);
  pinMode(s_adcCs, OUTPUT);
  digitalWrite(s_adcCs, HIGH);
  pinMode(s_miso, INPUT);
  delayMicroseconds(10);

  uint16_t v[4] = { 0, 0, 0, 0 };
  for (uint8_t i = 0; i < 4; i++) {
    bool nb = true;
    mcpRead(i, &v[i], &nb);
    if (nb) return;                                     // bit NULL = 1 -> nie ma układu
    if (v[i] == 0x0000u || v[i] == 0x0FFFu) return;     // wartość ekstremalna -> linia zwarta
    delayMicroseconds(2);
  }
  if (v[0] == v[1]) return;                             // stały odczyt na dwóch kanałach -> brak układu

  // MCP3008 (10 bit) po dziesięciu bitach danych dosyła dwa zera;
  // MCP3208 (12 bit) ma na tych pozycjach młodsze bity wyniku.
  uint8_t lsbZero = 0;
  for (uint8_t i = 0; i < 4; i++) {
    if ((v[i] & 0x0003u) == 0) lsbZero++;
  }
  if (lsbZero == 4) {
    s_adcBits = 10;
    s_adcFull = 1023.0f;
  } else {
    s_adcBits = 12;
    s_adcFull = 4095.0f;
  }

  s_adcOk = true;
  s_adcFail = 0;
  for (uint8_t i = 0; i < MCP_CHANNELS; i++) s_sink.found(CHANS[CH_ADC0 + i].id);
  LOG_I("ADC SPI: %s na CS=GPIO%d - %u kanałów, VREF=%.2f V",
        (s_adcBits == 10) ? "MCP3008 (10 bit)" : "MCP3208 (12 bit)",
        s_adcCs, (unsigned)MCP_CHANNELS, s_vref);
}

// =============================================================
//  Wykrywanie RTD (MAX31865)
// =============================================================
// MAX31865 nie ma rejestru identyfikacji, więc zapisujemy konfigurację
// i sprawdzamy, czy odczyt wraca taki sam. Dwie różne wartości dowodzą,
// że rejestr naprawdę daje się zapisać (odrzucamy 0x00 i 0xFF - linia
// zwarta do masy albo do zasilania).
static bool rtdDetectWith(bool mode1) {
  s_rtdMode1 = mode1;

  const uint8_t cfg = (uint8_t)(RTD_CFG_BASE |
                                ((s_rtdWires == 3) ? RTD_CFG_3WIRE : 0x00) |
                                ((s_rtdFilter == 50) ? RTD_CFG_F50 : 0x00));
  const uint8_t alt = (uint8_t)(cfg ^ RTD_CFG_F50);

  rtdWriteReg(RTD_REG_CONFIG, cfg);
  delayMicroseconds(10);
  const uint8_t a = rtdReadReg(RTD_REG_CONFIG);
  if (a == 0x00u || a == 0xFFu) return false;
  if ((a & RTD_MASK_CFG) != (cfg & RTD_MASK_CFG)) return false;

  rtdWriteReg(RTD_REG_CONFIG, alt);
  delayMicroseconds(10);
  const uint8_t b = rtdReadReg(RTD_REG_CONFIG);
  if (b == 0x00u || b == 0xFFu) return false;
  if ((b & RTD_MASK_CFG) != (alt & RTD_MASK_CFG)) return false;

  // zostawiamy tryb automatycznej konwersji z filtrem z konfiguracji
  rtdWriteReg(RTD_REG_CONFIG, cfg);
  delayMicroseconds(10);
  const uint8_t c = rtdReadReg(RTD_REG_CONFIG);
  if ((c & RTD_MASK_CFG) != (cfg & RTD_MASK_CFG)) return false;

  return true;
}

static void detectRtd() {
  if (s_rtdCs < 0 || s_sck < 0 || s_mosi < 0 || s_miso < 0) return;  // brak pinów - pomijamy bez logu

  pinMode(s_sck, OUTPUT);
  digitalWrite(s_sck, LOW);
  pinMode(s_mosi, OUTPUT);
  digitalWrite(s_mosi, LOW);
  pinMode(s_rtdCs, OUTPUT);
  digitalWrite(s_rtdCs, HIGH);
  pinMode(s_miso, INPUT);
  delayMicroseconds(10);

  bool ok = rtdDetectWith(false);
  if (!ok) ok = rtdDetectWith(true);
  if (!ok) {
    s_rtdMode1 = false;
    return;
  }

  s_rtdOk = true;
  s_rtdFail = 0;
  s_sink.found("rtd_temp");
  s_sink.found("rtd_res");
  LOG_I("RTD: MAX31865 na CS=GPIO%d - %u przewody, R0=%.1f Ω, RREF=%.1f Ω, filtr %u Hz",
        s_rtdCs, (unsigned)s_rtdWires, s_r0, s_rref, (unsigned)s_rtdFilter);
}

// =============================================================
//  Odczyty
// =============================================================
static void tcFail() {
  if (s_tcFail < 3 && ++s_tcFail == 3) {
    LOG_W("Termopara: brak poprawnego odczytu (3 próby) - kanały wstrzymane");
  }
  if (s_tcKind == TC_KIND_6675) {
    s_sink.clear("tc_max6675");
  } else {
    s_sink.clear("tc_31855_t");
    s_sink.clear("tc_31855_cj");
  }
}

static void readTc() {
  if (s_tcKind == TC_KIND_6675) {
    const uint32_t w = tcReadFrame(16);
    const uint16_t h = (uint16_t)w;
    if (h & 0x0004u) {                       // D2 = 1 -> obwód termopary rozwarty
      tcFail();
      return;
    }
    s_tcFail = 0;
    s_sink.publish("tc_max6675", (float)((h >> 3) & 0x0FFFu) * 0.25f);
    return;
  }

  const uint32_t w = tcReadFrame(32);
  if (w & 0x0003000Fu) {                     // błąd układu (m.in. rozwarta termopara)
    tcFail();
    return;
  }
  const float tc = (float)sgn14((int32_t)((w >> 18) & 0x3FFFu)) * 0.25f;
  const float cj = (float)sgn12((int32_t)((w >> 4) & 0x0FFFu)) * 0.0625f;
  if (tc < s_tcMin - 20.0f || tc > s_tcMax + 20.0f || cj < -60.0f || cj > 135.0f) {
    tcFail();
    return;
  }
  s_tcFail = 0;
  s_sink.publish("tc_31855_t", tc);
  s_sink.publish("tc_31855_cj", cj);
}

static void rtdFail() {
  if (s_rtdFail < 3 && ++s_rtdFail == 3) {
    LOG_W("RTD: brak poprawnego odczytu (3 próby) - kanały wstrzymane");
  }
  s_sink.clear("rtd_temp");
  s_sink.clear("rtd_res");
}

static void readRtd() {
  uint8_t hi = 0, lo = 0;
  rtdReadPair(RTD_REG_RTD_MSB, &hi, &lo);
  const uint16_t raw = (uint16_t)(((uint16_t)hi << 8) | lo);
  if (raw & 0x0001u) {                       // bit błędu: obwód RTD rozwarty / zwarcie
    rtdFail();
    return;
  }

  const float r = (float)(raw >> 1) / RTD_CODE_MAX * s_rref;
  if (!(r > 0.05f * s_r0 && r < RTD_RMAX_RATIO * s_r0)) {
    rtdFail();
    return;
  }

  float t;
  if (r >= s_r0) {
    // Callendar-Van Dusen dla temperatur dodatnich (odwrócenie równania)
    const float a = 3.9083e-3f;
    const float b = -5.775e-7f;
    const float disc = a * a - 4.0f * b * (1.0f - r / s_r0);
    if (!(disc >= 0.0f)) {
      rtdFail();
      return;
    }
    t = (-a + sqrtf(disc)) / (2.0f * b);
  } else {
    // wielomian ITS-90 dla temperatur ujemnych (x = 100 * R/R0)
    const float x = 100.0f * r / s_r0;
    t = -242.02f + x * (2.2228f + x * (2.5859e-3f +
        x * (-4.8260e-6f + x * (-2.8183e-8f + x * 1.5243e-10f))));
  }

  if (t < -220.0f || t > 1000.0f) {
    rtdFail();
    return;
  }

  s_rtdFail = 0;
  s_sink.publish("rtd_res", r);
  s_sink.publish("rtd_temp", t);
}

static void readAdc() {
  uint16_t raw[MCP_CHANNELS];
  for (uint8_t ch = 0; ch < MCP_CHANNELS; ch++) {
    bool nb = true;
    mcpRead(ch, &raw[ch], &nb);
    if (nb) {
      if (s_adcFail < 3 && ++s_adcFail == 3) {
        LOG_W("ADC SPI: błędna ramka (3 próby) - kanały wstrzymane");
      }
      for (uint8_t i = 0; i < MCP_CHANNELS; i++) s_sink.clear(CHANS[CH_ADC0 + i].id);
      return;
    }
  }
  s_adcFail = 0;

  for (uint8_t ch = 0; ch < MCP_CHANNELS; ch++) {
    // MCP3008 dosyła dwa bity na końcu ramki, MCP3208 ma tam młodsze bity
    const uint16_t code = (s_adcBits == 10) ? (uint16_t)(raw[ch] >> 2)
                                           : (uint16_t)(raw[ch] & 0x0FFFu);
    s_sink.publish(CHANS[CH_ADC0 + ch].id, (float)code * s_vref / s_adcFull);
  }
}

// =============================================================
//  Interfejs modułu
// =============================================================
static void beginImpl(const DrvSink& s) {
  s_sink = s;
  cfgLoad();

  s_sck = pinMap.pin("tc_sck");
  s_mosi = pinMap.pin("tc_mosi");
  s_miso = pinMap.pin("tc_miso");
  s_tcCs = pinMap.pin("tc_cs");
  s_rtdCs = pinMap.pin("rtd_cs");
  s_adcCs = pinMap.pin("adc_cs");

  detectTc();      // MAX6675 / MAX31855 (linia tc_cs)
  detectAdc();     // MCP3008 / MCP3208 (własna linia adc_cs)
  detectRtd();     // MAX31865 (własna linia rtd_cs)

  s_lastMs = millis();
}

static void readImpl() {
  // MAX6675 potrzebuje 220 ms na konwersję - jeden wspólny okres odczytu
  const uint32_t now = millis();
  if ((uint32_t)(now - s_lastMs) < TC_READ_MS) return;
  s_lastMs = now;

  if (s_tcOk)  readTc();
  if (s_rtdOk) readRtd();
  if (s_adcOk) readAdc();
}

const DrvModule drvTc = { "Termopary/RTD/ADC", CHANS, NCHANS, beginImpl, readImpl };
