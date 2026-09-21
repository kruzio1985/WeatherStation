/* =============================================================================
 * Stacja Pogody - stacja meteorologiczna na ESP32-S3 WROOM-1 (N16R8)
 * Copyright (c) 2026 kruzio1985 - https://github.com/kruzio1985
 * Licencja: PolyForm Noncommercial License 1.0.0 (plik LICENSE)
 * Użytek niekomercyjny. Kontakt: kruzio1985@users.noreply.github.com
 * =============================================================================
 */

// =============================================================
//  Moduł sterowników AUDIO / I²S (mikrofon + wzmacniacz).
//
//  Mikrofon (RX, port I2S_NUM_0):
//    * INMP441, ICS-43434, SPH0645 i układy z nimi zgodne - jeden
//      przewód danych DIN, 24 bity w 32-bitowej ramce (dane w górnych
//      bitach, dlatego przesuwamy >> 8 i normalizujemy do ±1,0).
//    * Te mikrofony nie mają rejestru identyfikacji, więc nie da się
//      ich rozpoznać po ID. Potwierdzeniem sprzętu jest niezerowy
//      odczyt z DMA - każdy prawdziwy mikrofon I²S szumi choćby
//      własnym szumem (same zera = nic nie jest taktowane).
//
//  Wzmacniacz (TX, port I2S_NUM_1 - tylko ESP32-S3, C3 ma jeden port):
//    * MAX98357A. Sam wzmacniacz nie ma kanałów pomiarowych, więc nic
//      nie publikuje - ma się zainstalować bez błędu i milczeć.
//      W begin() nie odtwarzamy żadnego dźwięku (żadnego beepu), a bufor
//      DMA od razu wypełniamy zerami (i2s_zero_dma_buffer), dodatkowo
//      tx_desc_auto_clear - żeby przy starcie nie poszedł szum.
//
//  Uwaga o wspólnych liniach zegara: BCLK i WS są jedne dla mikrofonu
//  i wzmacniacza (tak jest w mapie pinów), a o tym, który port je
//  wyprowadza na piny, decyduje kolejność instalacji (wygrywa ostatni).
//  Dlatego najpierw instalujemy TX, a na końcu RX - wtedy linie zegara
//  należą do portu mikrofonu i jego próbki są taktowane zegarem tego
//  samego portu (poprawna faza próbkowania). Wzmacniacz dostaje ten sam
//  zegar (16 kHz, 32-bitowe sloty) i same zera danych, czyli ciszę.
//
//  read() chodzi w pętli głównej stacji (odczyt czujników co 5 s) i nie
//  może blokować: czytamy z timeoutem 0 i drenujemy najwyżej kilka
//  bloków DMA (bez delay()). Poziom jest wygładzany w dziedzinie energii,
//  szczyt ma wolne opadanie, a LEQ to średnia energetyczna z okna
//  (domyślnie 10 s) - zgodnie z definicją LEQ.
//
//  Sterownik publikuje wartości zmierzone, kalibrację (offset) robi
//  SensorManager. Współczynniki z zakładki Kalibracja (sekcja "extra")
//  tylko czytamy - nic nie zapisujemy do NVS:
//    * "snd_offset_db" - korekta poziomu (czułość mikrofonu) [dB],
//    * "snd_leq_s"     - długość okna średniej LEQ [s].
//
//  Piny (mapa pinów, grupa "Audio / I²S"): "i2s_bclk", "i2s_ws",
//  "i2s_din", "i2s_dout". Gdy którykolwiek jest wyłączony (-1),
//  sterownik I²S w ogóle nie jest instalowany i nic nie publikujemy.
// =============================================================
#include "drv_i2s.h"
#include "pinmap.h"
#include "config.h"
#include "syslog.h"
#include <driver/i2s.h>
#include <soc/soc_caps.h>
#include <math.h>

// ---------- konfiguracja toru audio ----------
#define SND_RATE_HZ         16000   // częstotliwość próbkowania (mikrofon i wzmacniacz)
#define SND_CHUNK_FRAMES    256     // ramek w jednym bloku czytanym z DMA (16 ms)
#define SND_CHUNKS          4       // maksymalna liczba bloków na jedno read()
#define SND_DMA_COUNT       8       // liczba buforów DMA (8 * 16 ms = 128 ms)
#define SND_DATA_SHIFT      8       // 24 bity danych w górnych bitach 32-bitowego słowa
#define SND_FULL_SCALE      8388608.0f  // 2^23 - pełna skala po przesunięciu (0 dBFS)
#define SND_DB_FLOOR        (-120.0f)   // podłoga dBFS (logarytm z zera nie istnieje)
#define SND_SMOOTH_MS       1000.0f // stała czasu wygładzania poziomu
#define SND_SMOOTH_MAX      0.6f    // maks. udział nowej próbki na cykl (stacja czyta co 5 s)
#define SND_PEAK_FALL_DB_S  3.0f    // opadanie szczytu (dB na sekundę)
#define SND_LEQ_WINDOW_S    10.0f   // domyślne okno średniej LEQ [s]
#define SND_LEQ_MAX         16      // rozmiar bufora okna LEQ (próbek)
#define SND_PROBE_MS        200     // próba potwierdzenia mikrofonu w begin()
#define SND_PROBE_WAIT_MS   20      // pojedyncze oczekiwanie na dane podczas próby
#define SND_NO_DATA_MS      500     // brak próbek dłużej niż tyle = kanały puste

// ---------- klucze kalibracji (pamięć ustawień, sekcja "extra") ----------
#define KEY_SND_OFFSET      "snd_offset_db"  // korekta poziomu [dB]
#define KEY_SND_LEQ_S       "snd_leq_s"      // długość okna LEQ [s]

// ---------- tabela kanałów modułu ----------
const ChanDef chansI2s[] = {
    // Mikrofon I²S - pomiar wewnątrz (poziom w dBFS: 0 dB = pełna skala)
    { "snd_level", "Poziom dźwięku (mikrofon)", "dB", 1, "in", "sound_pressure", "dB", "mdi:volume-high"     },
    { "snd_peak",  "Szczyt dźwięku (mikrofon)", "dB", 1, "in", "sound_pressure", "dB", "mdi:waveform"        },
    { "snd_leq",   "Średni poziom (LEQ)",       "dB", 1, "in", "sound_pressure", "dB", "mdi:chart-bell-curve" },
};
const uint16_t N = (uint16_t)(sizeof(chansI2s) / sizeof(chansI2s[0]));

// =============================================================
//  Stan modułu
// =============================================================
static DrvSink s_sink = { nullptr, nullptr, nullptr };
static bool s_rxOk = false;        // port RX zainstalowany (piny mikrofonu ustawione)
static bool s_micOk = false;       // mikrofon potwierdzony niezerowymi próbkami
static bool s_txOk = false;        // wzmacniacz zainstalowany (tryb ciszy)
static bool s_cleared = true;      // kanały już puste (nie wołamy clear bez potrzeby)
static uint32_t s_lastMs = 0;      // czas poprzedniego przeliczenia (wygładzanie)
static uint32_t s_lastDataMs = 0;  // czas ostatnich danych z mikrofonu
static float s_rms2 = 0.0f;        // wygładzony średniokwadrat (dziedzina energii)
static float s_peakDb = SND_DB_FLOOR;

// Okno średniej LEQ - bufor statyczny (bez alokacji na stercie)
struct SndLeqSample { uint32_t ms; float db; };
static SndLeqSample s_leq[SND_LEQ_MAX];
static uint8_t s_leqHead = 0;
static uint8_t s_leqCount = 0;

// Bufor bloku DMA - statyczny (1 kB), używany tylko w read() i w próbie startowej
static int32_t s_block[SND_CHUNK_FRAMES];

// =============================================================
//  Pomocniki
// =============================================================

static inline float dbFromLinear(float v) {
  // 20 * log10(v / pełna skala) z podłogą - dla zera logarytm nie istnieje
  if (v <= 0.0f) return SND_DB_FLOOR;
  const float db = 20.0f * log10f(v / SND_FULL_SCALE);
  return db < SND_DB_FLOOR ? SND_DB_FLOOR : db;
}

// Korekta z pamięci ustawień (tylko odczyt, niezapisująca nic do NVS)
static inline float sndOffsetDb() {
  return config.extraF(KEY_SND_OFFSET, 0.0f);
}

// Długość okna LEQ [ms] z sensownym ograniczeniem (1..60 s)
static uint32_t leqWindowMs() {
  float s = config.extraF(KEY_SND_LEQ_S, SND_LEQ_WINDOW_S);
  if (s < 1.0f) s = 1.0f;
  if (s > 60.0f) s = 60.0f;
  return (uint32_t)(s * 1000.0f);
}

// Dopisanie próbki poziomu do okna LEQ (pierścień, bez dynamicznej pamięci)
static void leqAdd(uint32_t now, float db) {
  if (isnan(db)) return;
  s_leq[s_leqHead].ms = now;
  s_leq[s_leqHead].db = db;
  s_leqHead = (uint8_t)((s_leqHead + 1) % SND_LEQ_MAX);
  if (s_leqCount < SND_LEQ_MAX) s_leqCount++;
}

// LEQ = 10 * log10(średnia z 10^(L/10)) po próbkach mieszczących się w oknie
static float leqValue(uint32_t now, uint32_t windowMs) {
  float energy = 0.0f;
  uint8_t used = 0;
  for (uint8_t i = 0; i < s_leqCount; i++) {
    const uint8_t idx = (uint8_t)((s_leqHead + SND_LEQ_MAX - s_leqCount + i) % SND_LEQ_MAX);
    if ((uint32_t)(now - s_leq[idx].ms) > windowMs) continue;   // poza oknem
    energy += powf(10.0f, s_leq[idx].db * 0.1f);
    used++;
  }
  if (!used) return NAN;
  const float mean = energy / (float)used;
  if (mean <= 0.0f) return SND_DB_FLOOR;
  const float db = 10.0f * log10f(mean);
  return db < SND_DB_FLOOR ? SND_DB_FLOOR : db;
}

static void foundMic() {
  if (!s_sink.found) return;
  s_sink.found("snd_level");
  s_sink.found("snd_peak");
  s_sink.found("snd_leq");
}

static void clearMic() {
  if (s_cleared) return;
  if (s_sink.clear) {
    s_sink.clear("snd_level");
    s_sink.clear("snd_peak");
    s_sink.clear("snd_leq");
  }
  s_cleared = true;
}

// =============================================================
//  Start - instalacja I²S i wykrywanie mikrofonu
// =============================================================

// Mikrofon: instalujemy RX na I2S_NUM_0 i próbujemy potwierdzić dane.
// Zwraca true, gdy driver stoi (dane mogą pojawić się później).
static bool beginMic(int bclk, int ws, int din) {
  const i2s_config_t cfg = {
    (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),  // tylko RX - trybów nie mieszamy
    SND_RATE_HZ,
    I2S_BITS_PER_SAMPLE_32BIT,                    // mikrofony I²S: 24 bity w ramce 32-bit
    I2S_CHANNEL_FMT_ONLY_LEFT,                    // mono (INMP441 z nóżką L/R na GND)
    I2S_COMM_FORMAT_STAND_I2S,
    0,                                            // intr_alloc_flags
    SND_DMA_COUNT,
    SND_CHUNK_FRAMES,
    false,                                        // use_apll
    false,                                        // tx_desc_auto_clear (RX)
    0,                                            // fixed_mclk
    I2S_MCLK_MULTIPLE_256,
  };
  const i2s_pin_config_t pins = {
    I2S_PIN_NO_CHANGE,   // mck
    bclk,
    ws,
    I2S_PIN_NO_CHANGE,   // data out (nadajemy na drugim porcie)
    din,
  };
  if (i2s_driver_install(I2S_NUM_0, &cfg, 0, nullptr) != ESP_OK) {
    LOG_W("Mikrofon I²S: nie udało się zainstalować RX (BCLK %d, WS %d, DIN %d)", bclk, ws, din);
    return false;
  }
  if (i2s_set_pin(I2S_NUM_0, &pins) != ESP_OK) {
    i2s_driver_uninstall(I2S_NUM_0);
    LOG_W("Mikrofon I²S: nie udało się ustawić pinów (BCLK %d, WS %d, DIN %d)", bclk, ws, din);
    return false;
  }

  // Próba potwierdzenia: przez ~200 ms zbieramy bloki i sprawdzamy, czy są
  // niezerowe próbki. Same zera = linia danych nic nie nadaje.
  const uint32_t start = millis();
  uint32_t gotFrames = 0;
  while (!s_micOk && (uint32_t)(millis() - start) < SND_PROBE_MS) {
    size_t got = 0;
    if (i2s_read(I2S_NUM_0, s_block, sizeof(s_block), &got, pdMS_TO_TICKS(SND_PROBE_WAIT_MS)) != ESP_OK) continue;
    if (got == 0) continue;
    const uint32_t samples = (uint32_t)(got / sizeof(int32_t));
    gotFrames += samples;
    for (uint32_t i = 0; i < samples; i++) {
      if ((s_block[i] >> SND_DATA_SHIFT) != 0) { s_micOk = true; break; }
    }
  }
  if (s_micOk) {
    LOG_I("Mikrofon I²S: potwierdzony (BCLK %d, WS %d, DIN %d, 16 kHz/32 bit) - poziom dźwięku, szczyt i LEQ",
          bclk, ws, din);
    foundMic();
  } else {
    LOG_W("Mikrofon I²S: brak próbek z INMP441/ICS-43434/SPH0645 (odebrano %u ramek, same zera) - kanały pozostaną puste",
          (unsigned)gotFrames);
  }
  return true;
}

// Wzmacniacz MAX98357A: sam nie ma kanałów pomiarowych - instalacja TX
// w trybie ciszy (zera w DMA). Nic nie odtwarzamy, więc nie ma beepu.
static void beginAmp(int bclk, int ws, int dout) {
#if SOC_I2S_NUM > 1
  const i2s_config_t cfg = {
    (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),  // tylko TX - trybów nie mieszamy
    SND_RATE_HZ,
    I2S_BITS_PER_SAMPLE_32BIT,                    // wspólny zegar z RX (te same sloty)
    I2S_CHANNEL_FMT_ONLY_LEFT,                    // mono (MAX98357A bez SD_MODE w stereo)
    I2S_COMM_FORMAT_STAND_I2S,
    0,                                            // intr_alloc_flags
    SND_DMA_COUNT,
    SND_CHUNK_FRAMES,
    false,                                        // use_apll
    true,                                         // tx_desc_auto_clear - brak danych = zera
    0,                                            // fixed_mclk
    I2S_MCLK_MULTIPLE_256,
  };
  const i2s_pin_config_t pins = {
    I2S_PIN_NO_CHANGE,   // mck
    bclk,
    ws,
    dout,
    I2S_PIN_NO_CHANGE,   // data in (danych słuchamy na drugim porcie)
  };
  if (i2s_driver_install(I2S_NUM_1, &cfg, 0, nullptr) != ESP_OK) {
    LOG_W("Wzmacniacz MAX98357A: nie udało się zainstalować TX (BCLK %d, WS %d, DOUT %d)", bclk, ws, dout);
    return;
  }
  if (i2s_set_pin(I2S_NUM_1, &pins) != ESP_OK) {
    i2s_driver_uninstall(I2S_NUM_1);
    LOG_W("Wzmacniacz MAX98357A: nie udało się ustawić pinów (BCLK %d, WS %d, DOUT %d)", bclk, ws, dout);
    return;
  }
  i2s_zero_dma_buffer(I2S_NUM_1);   // zera zamiast śmieci z DMA = cisza na starcie
  s_txOk = true;
  LOG_I("Wzmacniacz MAX98357A: gotowy (BCLK %d, WS %d, DOUT %d), cisza - nic nie odtwarzamy", bclk, ws, dout);
#else
  // ESP32-C3 ma jeden kontroler I²S - nie da się rozdzielić RX i TX na dwa
  // porty, a mieszanie trybów w jednym porcie jest tu zabronione.
  (void)bclk; (void)ws; (void)dout;
  LOG_W("Wzmacniacz MAX98357A: pominięty (ten układ ma tylko jeden kontroler I²S)");
#endif
}

static void beginI2s(const DrvSink& sink) {
  s_sink = sink;

  const int bclk = pinMap.pin("i2s_bclk");
  const int ws   = pinMap.pin("i2s_ws");
  const int din  = pinMap.pin("i2s_din");
  const int dout = pinMap.pin("i2s_dout");

  // Wykrywanie sprzętu ogranicza się do mapy pinów: którykolwiek pin
  // wyłączony = nie instalujemy sterownika I²S i nic nie publikujemy
  // (mikrofon i wzmacniacz dzielą linie zegara, więc tor jest jeden).
  if (bclk < 0 || ws < 0 || din < 0 || dout < 0) {
    LOG_I("I²S: pominięte (piny wyłączone - BCLK %d, WS %d, DIN %d, DOUT %d)", bclk, ws, din, dout);
    return;
  }
  if (bclk == ws || din == dout || din == bclk || din == ws || dout == bclk || dout == ws) {
    LOG_W("I²S: pominięte (ten sam GPIO przypisany do kilku linii - BCLK %d, WS %d, DIN %d, DOUT %d)",
          bclk, ws, din, dout);
    return;
  }

  // Kolejność jest istotna - patrz komentarz o wspólnych liniach zegara
  // na początku pliku: TX instalujemy pierwszy, RX ostatni.
  beginAmp(bclk, ws, dout);
  if (!beginMic(bclk, ws, din)) {
    if (s_txOk) {          // mikrofon nie wszedł - nie zostawiamy samego TX
#if SOC_I2S_NUM > 1
      i2s_driver_uninstall(I2S_NUM_1);
#endif
      s_txOk = false;
    }
    return;
  }
  s_rxOk = true;
  s_lastDataMs = millis();
}

// =============================================================
//  Odczyt - drenaż bufora DMA i poziom w dBFS
// =============================================================
static void readI2s() {
  if (!s_rxOk) return;

  const uint32_t now = millis();
  uint32_t frames = 0;
  float sum2 = 0.0f;
  int32_t peak = 0;

  // Najwyżej kilka bloków po timeout 0 - read() nie może blokować pętli
  for (uint8_t c = 0; c < SND_CHUNKS; c++) {
    size_t got = 0;
    if (i2s_read(I2S_NUM_0, s_block, sizeof(s_block), &got, 0) != ESP_OK) break;
    if (got == 0) break;
    const uint32_t samples = (uint32_t)(got / sizeof(int32_t));
    for (uint32_t i = 0; i < samples; i++) {
      const int32_t v = s_block[i] >> SND_DATA_SHIFT;   // 24 bity w górnych bitach słowa
      sum2 += (float)v * (float)v;
      const int32_t a = v < 0 ? -v : v;
      if (a > peak) peak = a;
    }
    frames += samples;
  }

  // Brak danych (błąd odczytu / zerowy transfer / same zera) - kanały puste.
  // Chwila zwłoki chroni przed miganiem wartości, gdy stacja czyta rzadziej
  // niż DMA nadąża zbierać próbki.
  if (frames == 0 || peak == 0) {
    if ((uint32_t)(now - s_lastDataMs) > SND_NO_DATA_MS) clearMic();
    return;
  }

  if (!s_micOk) {                     // sprzęt potwierdzony dopiero teraz
    s_micOk = true;
    LOG_I("Mikrofon I²S: potwierdzony odczytem (pierwsze niezerowe próbki)");
    foundMic();
  }
  s_lastDataMs = now;

  const float levelDb = dbFromLinear(sqrtf(sum2 / (float)frames));
  const float blockPeakDb = dbFromLinear((float)peak);

  // Wygładzanie poziomu w dziedzinie energii (średniokwadrat), z ograniczeniem
  // udziału nowej próbki - przy odczycie co 5 s stała czasu nie ma znaczenia.
  const float raw2 = sum2 / (float)frames;
  const uint32_t dt = now - s_lastMs;
  if (s_lastMs == 0) {
    s_rms2 = raw2;                 // pierwszy odczyt - start od zmierzonej wartości
    s_peakDb = blockPeakDb;        // ...i od razu prawdziwy szczyt (bez zjazdu z podłogi)
  } else {
    float alpha = 1.0f - expf(-(float)dt / SND_SMOOTH_MS);
    if (alpha > SND_SMOOTH_MAX) alpha = SND_SMOOTH_MAX;
    if (alpha < 0.0f) alpha = 0.0f;
    s_rms2 += (raw2 - s_rms2) * alpha;

    // Szczyt: podnosi się od razu, opada powoli (kilka cykli odczytu)
    const float dtS = (float)dt * 0.001f;
    s_peakDb -= SND_PEAK_FALL_DB_S * dtS;
    if (s_peakDb < SND_DB_FLOOR) s_peakDb = SND_DB_FLOOR;
    if (blockPeakDb > s_peakDb) s_peakDb = blockPeakDb;
  }
  s_lastMs = now;
  const float smoothDb = dbFromLinear(sqrtf(s_rms2));

  const float offset = sndOffsetDb();
  const float leqDb = leqValue(now, leqWindowMs());

  if (s_sink.publish) {
    s_sink.publish("snd_level", smoothDb + offset);
    s_sink.publish("snd_peak", s_peakDb + offset);
    if (!isnan(leqDb)) s_sink.publish("snd_leq", leqDb + offset);
  }
  s_cleared = false;

  // Próbkę do okna LEQ dodajemy po publikacji - LEQ obejmuje też bieżący odczyt
  leqAdd(now, smoothDb);
}

// ---------- Rejestracja modułu ----------
const DrvModule drvI2s = { "Audio / I²S (mikrofon, wzmacniacz)", chansI2s, N, beginI2s, readI2s };
