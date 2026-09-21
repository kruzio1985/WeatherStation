/* =============================================================================
 * Stacja Pogody - stacja meteorologiczna na ESP32-S3 WROOM-1 (N16R8)
 * Copyright (c) 2026 kruzio1985 - https://github.com/kruzio1985
 * Licencja: Stacja Pogody Non-Commercial License 1.0 (plik LICENSE)
 * Użytek niekomercyjny. Kontakt: kruzio1985@users.noreply.github.com
 * =============================================================================
 */

// =============================================================
//  Moduł sterowników CZUJNIKÓW ANALOGOWYCH (ADC ESP32 / ADS1115).
//
//  Czujniki analogowe nie siedzą na żadnej magistrali poza opcjonalnym
//  ADS1115, więc cały moduł czyta wyłącznie przez analogReadMilliVolts()
//  (z kalibracją ADC z rdzenia Arduino), a gdy użytkownik ustawi klucz
//  "*_ads_ch" - przez ADS1115.
//
//  Obsługiwane czujniki:
//    * MQ-2 / MQ-3 / MQ-4 / MQ-5 / MQ-6 / MQ-7 / MQ-8 / MQ-9 / MQ-131 /
//      MQ-135 / MQ-136 / MQ-137 / MiCS-4514 / MiCS-6814 / TGS2600
//      na dwóch niezależnych wejściach: gas_adc (1) i gas_adc2 (2)
//    * ACS712 / ACS758   - prąd i moc (acs_adc)
//    * mokrość liścia ALBO oblodzenie (leaf_adc, wybór kluczem leaf_mode)
//    * PAR / fotodioda S2-131 (par_adc)
//
//  Zasada nadrzędna: kanał publikuje wartość TYLKO wtedy, gdy sprzęt
//  naprawdę odpowiedział. Pin < 0, nieudany odczyt przetwornika albo
//  napięcie poza sensownym zakresem to sink.clear(id). W czasie rozbiegu
//  grzejnika MQ sprzęt odpowiada, ale wartości jeszcze nie ma - wtedy
//  sink.found(id) bez publikacji.
//
//  Moduł jest WYŁĄCZNIE CZYTAJĄCY, jeżeli chodzi o konfigurację: nie
//  zapisuje nic do NVS (brak setExtraF/save), nie rusza Wi-Fi, OTA ani
//  magistrali I2C (Wire.begin() należy do stacji).
//
//  read() jest wołane z pętli głównej co kilka sekund - obsługujemy JEDNO
//  wejście na wywołanie (karuzela), żeby jeden obieg pętli stacji nigdy
//  nie zablokował się na kilku odczytach ADC naraz.
// =============================================================
#include "drv_gas.h"
#include "config.h"
#include "syslog.h"
#include "pinmap.h"
#include "drv_i2c.h"
#include <math.h>
#include <string.h>

// =============================================================
//  Klucze kalibracji (pamięć ustawień, sekcja "extra")
//  Moduł tylko je czyta - nic tu nie zapisujemy.
// =============================================================
#define KEY_G1_MODEL   "gas1_model"        // 0..15 - model czujnika na wejściu 1
#define KEY_G1_R0      "gas1_r0"           // kΩ - opór czujnika w czystym powietrzu
#define KEY_G1_RL      "gas1_rl"           // kΩ - opornik obciążający modułu (10 kΩ)
#define KEY_G1_ADS     "gas1_ads_ch"       // 0..3 - kanał ADS1115, -1 = ADC ESP32
#define KEY_G2_MODEL   "gas2_model"
#define KEY_G2_R0      "gas2_r0"
#define KEY_G2_RL      "gas2_rl"
#define KEY_G2_ADS     "gas2_ads_ch"
#define KEY_ACS_MODEL  "acs_model"         // 1..7 - typ ACS712 / ACS758
#define KEY_ACS_ZERO   "acs_zero_v"        // V przy 0 A (domyślnie Vcc/2 = 1,65 V)
#define KEY_ACS_VOLTS  "acs_volts"         // V instalacji (domyślnie 230)
#define KEY_ACS_ADS    "acs_ads_ch"
#define KEY_LEAF_MODE  "leaf_mode"         // 0 = mokrość liścia, 1 = oblodzenie
#define KEY_LEAF_DRY   "leaf_dry_v"        // V dla czujnika suchego (0 %)
#define KEY_LEAF_WET   "leaf_wet_v"        // V dla czujnika mokrego (100 %)
#define KEY_LEAF_ADS   "leaf_ads_ch"
#define KEY_PAR_MV     "par_mv_per_umol"   // mV na µmol/m²/s (kalibracja PAR)
#define KEY_PAR_ADS    "par_ads_ch"

// ---------- stałe czasowe i zakresy ----------
#define GAS_VCC_V       3.3f        // napięcie zasilania dzielnika modułu MQ (V)
#define GAS_RL_DEF_K    10.0f       // typowy opornik obciążający modułu MQ (kΩ)
#define GAS_WARMUP_MS   60000UL     // rozbieg grzejnika MQ/MiCS - 60 s bez wartości
#define GAS_VOUT_MIN_V  0.01f       // poniżej = zwarcie do GND / brak dzielnika
#define GAS_VOUT_MAX_V  3.28f       // powyżej = wejście wisi w powietrzu
#define GAS_PPM_MAX     1.0e6f      // bezpiecznik krzywej potęgowej (ppm)
#define ACS_AVG_N       8           // długość średniej ruchomej prądu
#define ACS_SAMPLES     4           // odczyty ADC na jedną próbkę prądu
#define ACS_MODEL_MIN   1           // indeksy w ACS_MODELS[]
#define ACS_MODEL_MAX   7
#define ADS_CH_MAX      3           // ADS1115 ma 4 kanały (0..3)

// =============================================================
//  Tabela modeli MQ / MiCS / TGS.
//
//  Publikowana krzywa z noty katalogowej ma postać   ppm = a · (Rs/R0)^b,
//  gdzie R0 to opór czujnika w CZYSTYM POWIETRZU. Nasz moduł liczy
//  najpierw stosunek r = Rs/R0 (R0 z klucza "gasN_r0" albo wartość typowa
//  z tabeli), a w czystym powietrzu nota podaje r = airRatio. Żeby wynik
//  był sensowny również bez wpisanej kalibracji, stałą krzywej
//  przesuwamy:   A = a · airRatio^b   i wtedy   ppm = A · r^b.
//  Przykład (MQ-7): nota daje dla czystego powietrza Rs/R0 ~ 10, więc
//  z krzywej 99,042 · (Rs/R0)^-1,518 wychodzi ~3 ppm CO - tyle, ile
//  naprawdę jest w powietrzu, a nie 99 ppm.
//
//  Współczynniki a, b to przybliżenia krzywych log-log z not
//  katalogowych (Hanwei/Winsen MQ, SGX MiCS, Figaro TGS); nie są
//  wzorcowane - po wpisaniu własnego R0 (klucz "gasN_r0") wynik jest
//  powtarzalny i wystarczający do progów ostrzegawczych.
//
//  outScale: 1,0 = wynik w ppm, 1000,0 = wynik w ppb (MQ-131 mierzy ozon
//  w zakresie ppb, a kanał "gas_o3" ma jednostkę ppb).
// =============================================================
struct GasModel {
  const char* name;       // nazwa do logu
  const char* chan;       // kanał publikowany przez ten model
  float a;                // stała krzywej z noty (ppm dla Rs/R0 = 1)
  float b;                // wykładnik (ujemny = więcej gazu -> mniej Rs)
  float airRatio;         // typowe Rs/R0 w czystym powietrzu
  float r0Def;            // typowy R0 w czystym powietrzu (kΩ)
  float outScale;         // 1,0 = ppm, 1000,0 = ppm -> ppb
};

static const GasModel MODELS[] = {
    // idx  model            kanał            a         b        czyste   R0 typ   skala
    /*  0 */ { "ogólny",     "gas",           50.0f,   -2.0f,    3.0f,   10.0f,    1.0f },
    /*  1 */ { "MQ-2",       "gas_lpg",      574.25f,  -2.222f,  9.83f,  20.0f,    1.0f },  // LPG/dym; czyste ~9,8
    /*  2 */ { "MQ-3",       "gas_alcohol",    0.3934f, -1.504f, 60.0f,  60.0f,    1.0f },  // alkohol; czyste ~60
    /*  3 */ { "MQ-4",       "gas_ch4",     1012.7f,   -2.786f,  4.4f,   30.0f,    1.0f },  // metan; czyste ~4,4
    /*  4 */ { "MQ-5",       "gas_lpg",       80.897f,  -2.431f,  6.5f,   20.0f,    1.0f },  // LPG/CH4; czyste ~6,5
    /*  5 */ { "MQ-6",       "gas_lpg",     1000.5f,   -2.186f, 10.0f,   20.0f,    1.0f },  // LPG; czyste ~10
    /*  6 */ { "MQ-7",       "gas_co",        99.042f,  -1.518f, 10.0f,   10.0f,    1.0f },  // CO; czyste ~10 (nota 10..27)
    /*  7 */ { "MQ-8",       "gas",          976.97f,   -0.688f, 30.0f,   10.0f,    1.0f },  // wodór; odczyt orientacyjny
    /*  8 */ { "MQ-9",       "gas",         1000.5f,   -2.186f,  9.0f,   10.0f,    1.0f },  // CO/palne; czyste ~9
    /*  9 */ { "MQ-131",     "gas_o3",        23.943f,  -1.11f,  15.0f,   50.0f, 1000.0f },  // ozon; nota w ppb
    /* 10 */ { "MQ-135",     "gas_nh3",      110.47f,   -2.862f,  3.6f,   10.0f,    1.0f },  // jakość powietrza/NH3
    /* 11 */ { "MQ-136",     "gas_h2s",       36.737f,  -3.536f, 20.0f,   10.0f,    1.0f },  // siarkowodór
    /* 12 */ { "MQ-137",     "gas_nh3",       56.082f,  -3.023f,  3.6f,   10.0f,    1.0f },  // amoniak
    /* 13 */ { "MiCS-4514",  "gas_co",         1.0f,    -2.95f,   1.0f,  200.00f,   1.0f },  // kanał RED (CO), R0 100..1500 kΩ
    /* 14 */ { "MiCS-6814",  "gas_no2",       0.01f,     4.64f,   1.0f,   10.0f,    1.0f },  // kanał OX (NO2), Rs rośnie z NO2
    /* 15 */ { "TGS2600",     "gas",         100.0f,    -1.5f,    3.0f,   10.0f,    1.0f },  // powietrze; odczyt względny
};
#define MODEL_COUNT     ((uint8_t)(sizeof(MODELS) / sizeof(MODELS[0])))
#define MODEL_DEF       0           // 0 = model ogólny (nieznany)

// =============================================================
//  Tabela czułości ACS712 / ACS758 (klucz "acs_model").
//  Układ zasilamy z 3,3 V, wtedy zero prądu to 1,65 V (Vcc/2) i cały
//  zakres pomiaru mieści się w ADC. Przy zasilaniu 5 V trzeba dzielnika,
//  bo ACS712 daje wtedy 2,5 V w spoczynku.
// =============================================================
struct AcsModel {
  const char* name;
  float mvPerA;      // czułość z noty katalogowej (mV/A)
};

static const AcsModel ACS_MODELS[] = {
    /* 1 */ { "ACS712-5A",    185.0f },
    /* 2 */ { "ACS712-20A",   100.0f },
    /* 3 */ { "ACS712-30A",    66.0f },
    /* 4 */ { "ACS758-50B",    40.0f },
    /* 5 */ { "ACS758-100B",   20.0f },
    /* 6 */ { "ACS758-150B",   13.3f },
    /* 7 */ { "ACS758-200B",   10.0f },
};

// ---------- tabela kanałów modułu ----------
const ChanDef chansGas[] = {
    // MQ / MiCS - jeden czujnik = jeden konkretny gaz
    { "gas_co",      "Czujnik CO (MQ-7 / MiCS)",         "ppm",       1, "out", "carbon_monoxide", "ppm",        "mdi:molecule-co"          },
    { "gas_lpg",     "Czujnik LPG (MQ-2 / MQ-5 / MQ-6)", "ppm",       1, "out", "",                "ppm",        "mdi:gas-cylinder"         },
    { "gas_ch4",     "Czujnik metanu (MQ-4)",            "ppm",       1, "out", "",                "ppm",        "mdi:gas-cylinder"         },
    { "gas_nh3",     "Czujnik amoniaku (MQ-135 / MQ-137)", "ppm",     1, "out", "",                "ppm",        "mdi:molecule"             },
    { "gas_o3",      "Czujnik ozonu (MQ-131)",           "ppb",       0, "out", "",                "ppb",        "mdi:molecule"             },
    { "gas_h2s",     "Czujnik siarkowodoru (MQ-136)",    "ppm",       1, "out", "",                "ppm",        "mdi:molecule"             },
    { "gas_no2",     "Czujnik NO2 (MiCS-6814 / MiCS-4514)", "ppm",    2, "out", "nitrogen_dioxide", "ppm",       "mdi:molecule"             },
    { "gas_alcohol", "Czujnik alkoholu (MQ-3)",          "ppm",       0, "out", "",                "ppm",        "mdi:molecule"             },
    // Odczyt ogólny - gdy model czujnika nie jest jeszcze wybrany
    { "gas",         "Czujnik gazów - odczyt ogólny",    "ppm",       1, "out", "",                "ppm",        "mdi:molecule"             },
    // ACS712 / ACS758 - prąd i moc
    { "acs_a",       "Prąd (ACS712 / ACS758)",           "A",         2, "out", "current",         "A",          "mdi:current-ac"           },
    { "acs_w",       "Moc (ACS712 / ACS758)",            "W",         1, "out", "power",           "W",          "mdi:flash"                },
    // Sonda na listku: mokrość albo oblodzenie (jedno wejście, dwa kanały)
    { "leaf",        "Mokrość liścia",                   "%",         0, "out", "humidity",        "%",          "mdi:leaf"                 },
    { "ice",         "Oblodzenie",                       "%",         0, "out", "",                "%",          "mdi:snowflake-alert"      },
    // PAR / fotodioda
    { "par",         "PAR - światło do fotosyntezy",     "µmol/m²/s", 0, "out", "",                "µmol/m²/s",  "mdi:white-balance-sunny"  },
};
const uint16_t NGAS = (uint16_t)(sizeof(chansGas) / sizeof(chansGas[0]));

// =============================================================
//  Stan modułu (statyczny, bez alokacji po begin())
// =============================================================
static DrvSink  s_sink;
static bool     s_started = false;
static uint32_t s_startMs = 0;

// Jeden czujnik MQ/MiCS na wejściu analogowym
struct GasIn {
  uint8_t model;      // indeks w MODELS[]
  int8_t  pin;        // GPIO albo -1
  int8_t  adsCh;      // kanał ADS1115 (0..3) albo -1
  float   r0;         // kΩ - opór czujnika w czystym powietrzu
  float   rl;         // kΩ - opornik obciążający modułu
  bool    enabled;    // wejście ma jakiekolwiek źródło odczytu
  bool    owner;      // czy to wejście obsługuje kanał ogólny "gas"
  bool    extra;      // czy dodatkowo publikować kanał ogólny "gas"
  uint8_t fails;      // kolejne nieudane odczyty
};
static GasIn s_gas[2];

static int8_t  s_acsPin = -1;
static int8_t  s_acsAds = -1;
static uint8_t s_acsModel = 1;      // 1..7 (indeks w ACS_MODELS[] to model-1)
static float   s_acsZero = GAS_VCC_V / 2.0f;
static float   s_acsVolts = 230.0f;
static bool    s_acsEnabled = false;
static uint8_t s_acsFails = 0;
static uint8_t s_acsRingIdx = 0;
static uint8_t s_acsRingCnt = 0;
static float   s_acsRing[ACS_AVG_N];

static int8_t  s_leafPin = -1;
static int8_t  s_leafAds = -1;
static uint8_t s_leafMode = 0;      // 0 = mokrość liścia, 1 = oblodzenie
static float   s_leafDry = 2.8f;
static float   s_leafWet = 0.4f;
static bool    s_leafEnabled = false;
static uint8_t s_leafFails = 0;

static int8_t  s_parPin = -1;
static int8_t  s_parAds = -1;
static float   s_parMvPerUmol = 1.0f;
static bool    s_parEnabled = false;
static uint8_t s_parFails = 0;

// Własna instancja ADS1115 - instancja z sensors_extra.cpp jest widoczna
// tylko w tym pliku, więc nie da się jej użyć. begin() tylko sprawdza
// obecność układu; Wire.begin() zostaje tam, gdzie był (stacja).
static Ads1115 s_ads;
static bool    s_adsOk = false;

static uint8_t s_rr = 0;            // karuzela: które wejście czytać w tym obiegu

// =============================================================
//  Pomocniki wspólne
// =============================================================
static float clampf(float v, float lo, float hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

// Tłumienie wejścia ustawiamy punktowo (11 dB = zakres do ~3,1 V).
// ADC_11db to stała typu enum rdzenia, a nie makro, więc zamiast #ifdef
// sprawdzamy dostępność rdzenia Arduino; gdyby stałej nie było, zostaje
// domyślne 11 dB ustawiane przez rdzeń.
static void setupAdcPin(int8_t pin, int8_t adsCh) {
  if (pin < 0 || adsCh >= 0) return;      // odczyt idzie z ADS1115
#if defined(ESP_ARDUINO_VERSION_MAJOR) || defined(ADC_11db)
  analogSetPinAttenuation((uint8_t)pin, ADC_11db);
#else
  (void)pin;
#endif
}

// Jeden odczyt napięcia wejścia analogowego (ADC ESP32 albo ADS1115).
// false = wejście nieprzypisane albo odczyt się nie udał.
static bool readVolts(int8_t pin, int8_t adsCh, float& out) {
  if (adsCh >= 0) {
    if (!s_adsOk) return false;
    float v = 0.0f;
    if (!s_ads.readChannel((uint8_t)adsCh, v)) return false;
    if (isnan(v) || v < 0.0f) return false;
    out = v;
    return true;
  }
  if (pin < 0) return false;
  const float mv = (float)analogReadMilliVolts((uint8_t)pin);
  if (isnan(mv)) return false;
  out = mv / 1000.0f;
  return true;
}

// Średnia z kilku szybkich odczytów - surowy ADC jest szumiący.
// Przy ADS1115 bierzemy jedną próbkę, bo każdy odczyt to ~10 ms.
static bool readVoltsAvg(int8_t pin, int8_t adsCh, uint8_t samples, float& out) {
  float sum = 0.0f;
  uint8_t n = 0;
  for (uint8_t i = 0; i < samples; i++) {
    float v = 0.0f;
    if (readVolts(pin, adsCh, v)) {
      sum += v;
      n++;
    }
  }
  if (n == 0) return false;
  out = sum / (float)n;
  return true;
}

// Rozstrzygnięcie źródła odczytu dla wejścia, które ma ustawiony klucz
// "*_ads_ch": gdy ADS1115 nie odpowiedział, wracamy na ADC ESP32 (o ile
// pin jest przypisany), a gdy nie ma już nic - wejście jest pomijane.
static int8_t resolveAds(int8_t pin, int8_t adsCh, int8_t& outPin, const char* tag) {
  outPin = pin;
  if (adsCh < 0) return -1;
  if (s_adsOk) return adsCh;
  outPin = pin;
  if (pin >= 0) {
    LOG_W("Gazy: %s - brak ADS1115, przechodzę na ADC ESP32 (GPIO %d)", tag, (int)pin);
    return -1;
  }
  LOG_W("Gazy: %s - jest tylko kanał ADS1115, a układ nie odpowiada - wejście pominięte", tag);
  return -2;
}

// ------------------------------------------------------------
//  Gazy palne i toksyczne (MQ / MiCS) - konwersja Rs na ppm.
//
//  Moduł MQ to dzielnik: wyjście Vout, opornik obciążający RL,
//  opór czujnika liczony z napięcia zasilania Vcc = 3,3 V:
//      Rs = RL · (Vcc - Vout) / Vout
//  Dalej stosunek r = Rs / R0 (R0 to opór w czystym powietrzu)
//  i krzywa z noty:   ppm = a · (Rs/R0)^b,  przesunięta o stosunek
//  czystego powietrza:  ppm = a · k^b · r^b,  k = airRatio.
// ------------------------------------------------------------
static void stepGas(GasIn& g) {
  const GasModel& m = MODELS[g.model];
  const char* id = m.chan;

  // Kanał ogólny należy do jednego wejścia - inaczej oba nadpisywałyby
  // sobie nawzajem "gas" co drugi obieg pętli.
  if (strcmp(id, "gas") == 0 && !g.owner) return;

  float v = 0.0f;
  if (!readVolts(g.pin, g.adsCh, v) || v <= GAS_VOUT_MIN_V || v >= GAS_VOUT_MAX_V) {
    if (g.fails < 3 && ++g.fails == 3) {
      LOG_W("Gazy: %s - brak poprawnego napięcia na wejściu - kanał wstrzymany", m.name);
    }
    s_sink.clear(id);
    return;
  }
  g.fails = 0;

  // Rozbieg grzejnika MQ/MiCS: sprzęt odpowiada, ale wartość jest
  // jeszcze bez sensu (grzejnik musi się wygrzać).
  if ((uint32_t)(millis() - s_startMs) < GAS_WARMUP_MS) {
    s_sink.found(id);
    return;
  }

  const float rs = g.rl * (GAS_VCC_V - v) / v;                 // kΩ
  const float r = (g.r0 > 0.001f) ? (rs / g.r0) : 0.0f;        // Rs / R0
  const float A = m.a * powf(m.airRatio, m.b);
  float ppm = A * powf(r, m.b);
  if (isnan(ppm) || ppm < 0.0f) {
    s_sink.clear(id);
    return;
  }
  if (ppm > GAS_PPM_MAX) ppm = GAS_PPM_MAX;                    // zabezpieczenie krzywej
  s_sink.publish(id, ppm * m.outScale);

  // Wejście #1 dokłada jeszcze odczyt ogólny (kanał "gas"), żeby przed
  // wybraniem modelu w kluczu gas1_model też było co pokazać.
  if (g.extra) {
    const GasModel& gm = MODELS[MODEL_DEF];
    float gen = gm.a * powf(gm.airRatio, gm.b) * powf(r, gm.b);
    if (isnan(gen) || gen < 0.0f) {
      s_sink.clear("gas");
    } else {
      if (gen > GAS_PPM_MAX) gen = GAS_PPM_MAX;
      s_sink.publish("gas", gen);
    }
  }
}

// ------------------------------------------------------------
//  ACS712 / ACS758 - prąd i moc.
//      I = (Vout - zero) / czułość
//  gdzie "zero" to napięcie przy 0 A (klucz acs_zero_v, domyślnie
//  Vcc/2 = 1,65 V przy zasilaniu 3,3 V), a czułość bierzemy z tabeli
//  ACS_MODELS[]. Prąd może być ujemny (przepływ w drugą stronę) -
//  nie obcinamy go. Moc = prąd · napięcie sieci (klucz acs_volts).
// ------------------------------------------------------------
static void stepAcs(void) {
  const uint8_t samples = (s_acsAds >= 0) ? 1 : ACS_SAMPLES;
  float v = 0.0f;
  if (!readVoltsAvg(s_acsPin, s_acsAds, samples, v)) {
    if (s_acsFails < 3 && ++s_acsFails == 3) {
      LOG_W("ACS: brak odczytu napięcia na wejściu - kanały prądu i mocy wstrzymane");
    }
    s_sink.clear("acs_a");
    s_sink.clear("acs_w");
    return;
  }
  s_acsFails = 0;

  // Średnia ruchoma z ostatnich próbek - surowy ADC jest szumiący.
  s_acsRing[s_acsRingIdx] = v;
  s_acsRingIdx = (uint8_t)((s_acsRingIdx + 1) % ACS_AVG_N);
  if (s_acsRingCnt < ACS_AVG_N) s_acsRingCnt++;
  float sum = 0.0f;
  for (uint8_t i = 0; i < s_acsRingCnt; i++) sum += s_acsRing[i];
  const float vAvg = sum / (float)s_acsRingCnt;

  const float mvPerA = ACS_MODELS[s_acsModel - ACS_MODEL_MIN].mvPerA;
  if (mvPerA <= 0.0f) {
    s_sink.clear("acs_a");
    s_sink.clear("acs_w");
    return;
  }
  const float amps = (vAvg - s_acsZero) / (mvPerA / 1000.0f);
  if (isnan(amps)) {
    s_sink.clear("acs_a");
    s_sink.clear("acs_w");
    return;
  }
  s_sink.publish("acs_a", amps);
  s_sink.publish("acs_w", amps * s_acsVolts);
}

// ------------------------------------------------------------
//  Mokrość liścia / oblodzenie - jedno wejście leaf_adc, wybór kluczem
//  "leaf_mode" (0 = mokrość liścia, 1 = oblodzenie). Tylko wybrany kanał
//  publikuje wartość, drugi jest zerowany.
//
//  Kalibracja (klucze leaf_dry_v / leaf_wet_v):
//    leaf_dry_v = napięcie czujnika SUCHego  (0 % mokrości, domyślnie 2,8 V)
//    leaf_wet_v = napięcie czujnika MOKREgo  (100 % mokrości, domyślnie 0,4 V)
//  Na płytkach rezystancyjnych woda ZWIĘKSZA przewodność, więc napięcie
//  dzielnika spada - dlatego mokry liść ma niższe napięcie.
//
//  Oblodzenie używa tych samych dwóch napięć, ale z odwróconą
//  interpretacją: 100 % oblodzenia odpowiada napięciu SUCHego czujnika,
//  a 0 % - napięciu MOKREgo (lód na powierzchni działa jak izolator).
//  Jeżeli Twój czujnik przewodzi mocniej pod lodem, zamień w konfiguracji
//  wartości leaf_dry_v i leaf_wet_v miejscami.
// ------------------------------------------------------------
static void stepLeaf(void) {
  const bool iceMode = (s_leafMode == 1);
  const char* onId  = iceMode ? "ice" : "leaf";
  const char* offId = iceMode ? "leaf" : "ice";
  s_sink.clear(offId);

  float v = 0.0f;
  if (!readVolts(s_leafPin, s_leafAds, v)) {
    if (s_leafFails < 3 && ++s_leafFails == 3) {
      LOG_W("Liść: brak odczytu napięcia na wejściu - kanał wstrzymany");
    }
    s_sink.clear(onId);
    return;
  }
  s_leafFails = 0;

  const float span = s_leafDry - s_leafWet;
  if (fabsf(span) < 0.05f) {          // brak sensownej kalibracji
    s_sink.clear(onId);
    return;
  }

  float pct = (s_leafDry - v) / span * 100.0f;    // sucho = 0 %, mokro = 100 %
  if (iceMode) pct = 100.0f - pct;                // odwrócona interpretacja
  pct = clampf(pct, 0.0f, 100.0f);
  if (isnan(pct)) {
    s_sink.clear(onId);
    return;
  }
  s_sink.publish(onId, pct);
}

// ------------------------------------------------------------
//  PAR / fotodioda S2-131.
//      wartość = mV / par_mv_per_umol
//  Nota S2-131 podaje ~1 mV na µmol/m²/s przy obciążeniu ~100 kΩ, dlatego
//  domyślnie 1,0 daje wynik wprost w µmol/m²/s. Wynik nie może być ujemny.
// ------------------------------------------------------------
static void stepPar(void) {
  float v = 0.0f;
  if (!readVolts(s_parPin, s_parAds, v)) {
    if (s_parFails < 3 && ++s_parFails == 3) {
      LOG_W("PAR: brak odczytu napięcia na wejściu - kanał wstrzymany");
    }
    s_sink.clear("par");
    return;
  }
  s_parFails = 0;

  const float mvPerUmol = (s_parMvPerUmol > 0.001f) ? s_parMvPerUmol : 1.0f;
  const float umol = (v * 1000.0f) / mvPerUmol;
  if (isnan(umol) || umol < 0.0f) {
    s_sink.clear("par");
    return;
  }
  s_sink.publish("par", umol);
}

// =============================================================
//  begin() - przypisania pinów, klucze kalibracji, pierwszy odczyt
// =============================================================
static void beginGas(const DrvSink& sink) {
  s_sink = sink;
  s_startMs = millis();
  s_started = true;

  // Rozdzielczość ADC ustawiamy raz dla całego modułu.
  analogReadResolution(12);

  // ---------- przypisania z mapy pinów ----------
  s_gas[0].pin = (int8_t)pinMap.pin("gas_adc");
  s_gas[1].pin = (int8_t)pinMap.pin("gas_adc2");
  s_acsPin  = (int8_t)pinMap.pin("acs_adc");
  s_leafPin = (int8_t)pinMap.pin("leaf_adc");
  s_parPin  = (int8_t)pinMap.pin("par_adc");

  // ---------- modele i kalibracja gazów ----------
  static const char* const modelKeys[2] = { KEY_G1_MODEL, KEY_G2_MODEL };
  static const char* const r0Keys[2]    = { KEY_G1_R0,    KEY_G2_R0    };
  static const char* const rlKeys[2]    = { KEY_G1_RL,    KEY_G2_RL    };
  static const char* const adsKeys[2]   = { KEY_G1_ADS,   KEY_G2_ADS   };

  for (uint8_t i = 0; i < 2; i++) {
    GasIn& g = s_gas[i];
    const int model = (int)(config.extraF(modelKeys[i], (float)MODEL_DEF) + 0.5f);
    g.model = (model >= 0 && model < (int)MODEL_COUNT) ? (uint8_t)model : MODEL_DEF;
    if (model != (int)g.model) {
      LOG_W("Gazy: \"%s\" = %d poza zakresem 0..%u - używam modelu ogólnego",
            modelKeys[i], model, (unsigned)(MODEL_COUNT - 1));
    }

    const GasModel& m = MODELS[g.model];
    g.adsCh = -1;
    if (config.hasExtra(adsKeys[i])) {
      const int ch = (int)(config.extraF(adsKeys[i], -1.0f) + 0.5f);
      if (ch >= 0 && ch <= ADS_CH_MAX) g.adsCh = (int8_t)ch;
    }

    const bool r0Set = config.hasExtra(r0Keys[i]);
    g.r0 = r0Set ? config.extraF(r0Keys[i], m.r0Def) : m.r0Def;
    if (g.r0 <= 0.001f) g.r0 = m.r0Def;
    g.rl = config.extraF(rlKeys[i], GAS_RL_DEF_K);
    if (g.rl <= 0.001f) g.rl = GAS_RL_DEF_K;

    // Ostrzeżenie tylko raz na wejście - brak kalibracji nie blokuje
    // publikacji, wynik jest po prostu przybliżony.
    if (!r0Set) {
      LOG_W("Gazy: \"%s\" bez kalibracji R0, używam wartości domyślnej %.1f kΩ (%s - wynik przybliżony)",
            r0Keys[i], g.r0, m.name);
    }
  }

  // ---------- ACS712 / ACS758 ----------
  s_acsAds = -1;
  if (config.hasExtra(KEY_ACS_ADS)) {
    const int ch = (int)(config.extraF(KEY_ACS_ADS, -1.0f) + 0.5f);
    if (ch >= 0 && ch <= ADS_CH_MAX) s_acsAds = (int8_t)ch;
  }
  const int acsModel = (int)(config.extraF(KEY_ACS_MODEL, (float)ACS_MODEL_MIN) + 0.5f);
  s_acsModel = (acsModel >= ACS_MODEL_MIN && acsModel <= ACS_MODEL_MAX) ? (uint8_t)acsModel : ACS_MODEL_MIN;
  if (acsModel != (int)s_acsModel) {
    LOG_W("ACS: \"%s\" = %d poza zakresem %d..%d - używam %s",
          KEY_ACS_MODEL, acsModel, ACS_MODEL_MIN, ACS_MODEL_MAX,
          ACS_MODELS[s_acsModel - ACS_MODEL_MIN].name);
  }
  s_acsZero  = config.extraF(KEY_ACS_ZERO, GAS_VCC_V / 2.0f);
  s_acsVolts = config.extraF(KEY_ACS_VOLTS, 230.0f);
  if (s_acsVolts < 0.0f) s_acsVolts = 0.0f;

  // ---------- liść / oblodzenie ----------
  s_leafAds = -1;
  if (config.hasExtra(KEY_LEAF_ADS)) {
    const int ch = (int)(config.extraF(KEY_LEAF_ADS, -1.0f) + 0.5f);
    if (ch >= 0 && ch <= ADS_CH_MAX) s_leafAds = (int8_t)ch;
  }
  s_leafMode = (config.extraF(KEY_LEAF_MODE, 0.0f) >= 0.5f) ? 1 : 0;
  s_leafDry  = config.extraF(KEY_LEAF_DRY, 2.8f);
  s_leafWet  = config.extraF(KEY_LEAF_WET, 0.4f);

  // ---------- PAR ----------
  s_parAds = -1;
  if (config.hasExtra(KEY_PAR_ADS)) {
    const int ch = (int)(config.extraF(KEY_PAR_ADS, -1.0f) + 0.5f);
    if (ch >= 0 && ch <= ADS_CH_MAX) s_parAds = (int8_t)ch;
  }
  s_parMvPerUmol = config.extraF(KEY_PAR_MV, 1.0f);

  // ---------- ADS1115 (tylko gdy któryś klucz *_ads_ch go wybrał) ----------
  const bool adsNeeded = (s_gas[0].adsCh >= 0) || (s_gas[1].adsCh >= 0) ||
                         (s_acsAds >= 0) || (s_leafAds >= 0) || (s_parAds >= 0);
  if (adsNeeded) {
    if (pinMap.pin("i2c_sda") < 0 || pinMap.pin("i2c_scl") < 0) {
      LOG_W("Gazy: klucze *_ads_ch ustawione, ale magistrala I2C nieprzypisana - odczyt z ADS1115 niemożliwy");
    } else if (s_ads.begin()) {     // tylko rozpoznanie układu, Wire.begin() robi stacja
      s_adsOk = true;
      LOG_I("Gazy: ADS1115 wykryty pod adresem 0x%02X - wejścia analogowe przez ADS1115",
            s_ads.addr());
    } else {
      LOG_W("Gazy: brak ADS1115 pod adresami 0x48..0x4B - zostaje ADC ESP32");
    }
  }

  // ---------- rozstrzygnięcie źródła odczytu ----------
  for (uint8_t i = 0; i < 2; i++) {
    GasIn& g = s_gas[i];
    int8_t pin = g.pin;
    const int8_t ch = resolveAds(g.pin, g.adsCh, pin, i == 0 ? "gaz #1" : "gaz #2");
    g.pin = pin;
    g.adsCh = (ch == -2) ? -1 : ch;
    g.enabled = (ch != -2) && (g.pin >= 0 || g.adsCh >= 0);
  }
  {
    int8_t pin = s_acsPin;
    const int8_t ch = resolveAds(s_acsPin, s_acsAds, pin, "ACS");
    s_acsPin = pin;
    s_acsAds = (ch == -2) ? -1 : ch;
    s_acsEnabled = (ch != -2) && (s_acsPin >= 0 || s_acsAds >= 0);
  }
  {
    int8_t pin = s_leafPin;
    const int8_t ch = resolveAds(s_leafPin, s_leafAds, pin, "liść/oblodzenie");
    s_leafPin = pin;
    s_leafAds = (ch == -2) ? -1 : ch;
    s_leafEnabled = (ch != -2) && (s_leafPin >= 0 || s_leafAds >= 0);
  }
  {
    int8_t pin = s_parPin;
    const int8_t ch = resolveAds(s_parPin, s_parAds, pin, "PAR");
    s_parPin = pin;
    s_parAds = (ch == -2) ? -1 : ch;
    s_parEnabled = (ch != -2) && (s_parPin >= 0 || s_parAds >= 0);
  }

  // Kanał ogólny "gas" obsługuje wejście #1; wejście #2 przejmuje go tylko
  // wtedy, gdy pierwszego wejścia w ogóle nie ma (dwa wejścia nie mogą
  // publikować tego samego kanału - nadpisywałyby się nawzajem).
  // Wejście #1 dokłada odczyt ogólny również przy konkretnym modelu, żeby
  // przed wybraniem modelu (gas1_model) też było coś widać na pulpicie.
  s_gas[0].owner = true;
  s_gas[1].owner = !s_gas[0].enabled;
  s_gas[0].extra = (strcmp(MODELS[s_gas[0].model].chan, "gas") != 0);
  s_gas[1].extra = false;

  // ---------- tłumienie wejść ADC ESP32 ----------
  for (uint8_t i = 0; i < 2; i++) setupAdcPin(s_gas[i].pin, s_gas[i].adsCh);
  setupAdcPin(s_acsPin, s_acsAds);
  setupAdcPin(s_leafPin, s_leafAds);
  setupAdcPin(s_parPin, s_parAds);

  // ---------- log: jedno zdanie na skonfigurowane wejście ----------
  for (uint8_t i = 0; i < 2; i++) {
    GasIn& g = s_gas[i];
    const GasModel& m = MODELS[g.model];
    // Wejście z modelem bez konkretnego gazu, ale nie będące właścicielem
    // kanału ogólnego, nie publikuje niczego (patrz stepGas).
    const char* chan = (!g.owner && strcmp(m.chan, "gas") == 0) ? "gas (pominięty)" : m.chan;
    if (!g.enabled) {
      LOG_I("Gazy: wejście #%u nieprzypisane - czujnik pominięty", (unsigned)(i + 1));
    } else if (g.adsCh >= 0) {
      LOG_I("Gazy: %s na ADS1115 kanał %d, R0=%.1f kΩ, RL=%.0f kΩ (kanał %s)",
            m.name, (int)g.adsCh, g.r0, g.rl, chan);
    } else {
      LOG_I("Gazy: %s na GPIO %d, R0=%.1f kΩ, RL=%.0f kΩ (kanał %s)",
            m.name, (int)g.pin, g.r0, g.rl, chan);
    }
  }
  if (s_acsEnabled) {
    if (s_acsAds >= 0) {
      LOG_I("ACS: %s na ADS1115 kanał %d, zero %.2f V, sieć %.0f V (kanały acs_a, acs_w)",
            ACS_MODELS[s_acsModel - ACS_MODEL_MIN].name, (int)s_acsAds, s_acsZero, s_acsVolts);
    } else {
      LOG_I("ACS: %s na GPIO %d, zero %.2f V, sieć %.0f V (kanały acs_a, acs_w)",
            ACS_MODELS[s_acsModel - ACS_MODEL_MIN].name, (int)s_acsPin, s_acsZero, s_acsVolts);
    }
  } else {
    LOG_I("ACS: wejście acs_adc nieprzypisane - prąd i moc pominięte");
  }
  if (s_leafEnabled) {
    if (s_leafAds >= 0) {
      LOG_I("Liść/oblodzenie: tryb %s na ADS1115 kanał %d, kalibracja %.2f V (sucho) -> %.2f V (mokro), kanał %s",
            s_leafMode == 1 ? "oblodzenie" : "mokrość liścia", (int)s_leafAds,
            s_leafDry, s_leafWet, s_leafMode == 1 ? "ice" : "leaf");
    } else {
      LOG_I("Liść/oblodzenie: tryb %s na GPIO %d, kalibracja %.2f V (sucho) -> %.2f V (mokro), kanał %s",
            s_leafMode == 1 ? "oblodzenie" : "mokrość liścia", (int)s_leafPin,
            s_leafDry, s_leafWet, s_leafMode == 1 ? "ice" : "leaf");
    }
  } else {
    LOG_I("Liść/oblodzenie: wejście leaf_adc nieprzypisane - kanały pominięte");
  }
  if (s_parEnabled) {
    if (s_parAds >= 0) {
      LOG_I("PAR: ADS1115 kanał %d, kalibracja %.3f mV na µmol/m²/s",
            (int)s_parAds, s_parMvPerUmol);
    } else {
      LOG_I("PAR: GPIO %d, kalibracja %.3f mV na µmol/m²/s",
            (int)s_parPin, s_parMvPerUmol);
    }
  } else {
    LOG_I("PAR: wejście par_adc nieprzypisane - kanał pominięty");
  }

  // ---------- pierwszy odczyt od razu po starcie ----------
  // Kanały "cyfrowe" (ACS, liść, PAR) mają wartość natychmiast; grzejniki
  // MQ zgłoszą tylko obecność, bo pierwsze 60 s to rozbieg grzejnika.
  if (s_parEnabled) stepPar();
  if (s_leafEnabled) stepLeaf();
  if (s_acsEnabled) stepAcs();
  if (s_gas[0].enabled) stepGas(s_gas[0]);
  if (s_gas[1].enabled) stepGas(s_gas[1]);
}

// =============================================================
//  read() - jedno wejście na wywołanie (karuzela)
// =============================================================
static void readGas(void) {
  if (!s_started) return;

  for (uint8_t n = 0; n < 5; n++) {
    const uint8_t slot = s_rr;
    s_rr = (uint8_t)((s_rr + 1) % 5);
    switch (slot) {
      case 0: if (s_gas[0].enabled) { stepGas(s_gas[0]); return; } break;
      case 1: if (s_gas[1].enabled) { stepGas(s_gas[1]); return; } break;
      case 2: if (s_acsEnabled)     { stepAcs();         return; } break;
      case 3: if (s_leafEnabled)    { stepLeaf();        return; } break;
      case 4: if (s_parEnabled)     { stepPar();         return; } break;
      default: break;
    }
  }
}

// =============================================================
//  Rejestracja modułu
// =============================================================
const DrvModule drvGas = { "Analogowe (MQ/MiCS, ACS712/758, liść, PAR)", chansGas, NGAS, beginGas, readGas };
