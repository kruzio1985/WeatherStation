/* =============================================================================
 * Stacja Pogody - stacja meteorologiczna na ESP32-S3 WROOM-1 (N16R8)
 * Copyright (c) 2026 kruzio1985 - https://github.com/kruzio1985
 * Licencja: Stacja Pogody Non-Commercial License 1.0 (plik LICENSE)
 * Użytek niekomercyjny. Kontakt: kruzio1985@users.noreply.github.com
 * =============================================================================
 */
#include "sensors.h"
#include "config.h"
#include "pinmap.h"
#include "syslog.h"
#include <Wire.h>
#include <Adafruit_BME280.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <math.h>
#include <new>
#include <time.h>

// PMS5003 (pył) siedzi na UART2. ESP32-C3 ma tylko dwa UART-y (Serial0 +
// Serial1), więc na tej płytce czujnik pyłu jest niedostępny - kod musi się
// wtedy skompilować bez odwołań do Serial2.
#if SOC_UART_NUM > 2
  #define PMS_UART_AVAILABLE 1
#else
  #define PMS_UART_AVAILABLE 0
#endif


SensorManager sensors;

volatile unsigned long SensorManager::rainPulses_ = 0;
volatile unsigned long SensorManager::anemPulses_ = 0;

// Stałe sprzętowe (dopasuj do swoich czujników)
#define RAIN_MM_PER_TIP     0.3f     // mm opadu na jeden impuls deszczomierza
#define ANEM_KMH_PER_HZ     2.4f     // km/h na impuls/sekundę (typowy anemometr)
#define ANEM_WINDOW_MS      5000     // okno pomiaru wiatru
#define VANE_MAX_SPAN       120      // maks. rozrzut próbek ADC wiatrowskazu
#define VANE_FLOAT_DELTA    1500     // różnica wskazań przy pull-down/pull-up = wolny pin

// Adresy I2C (zapasowe, gdyby adres zmienił się po podłączeniu)
#define BH1750_ADDR         0x23
#define BH1750_ADDR_ALT     0x5C
#define SCD4X_ADDR          0x62     // Sensirion SCD40 / SCD41
#define SGP30_ADDR          0x58     // Sensirion SGP30 / SGP40

// Nazwy kierunków wiatru (16 stron świata). Kolejność od północy, co 22,5°.
static const char* const COMPASS_16[16] = {
  "N", "NNE", "NE", "ENE", "E", "ESE", "SE", "SSE",
  "S", "SSW", "SW", "WSW", "W", "WNW", "NW", "NNW"
};

// Suma kontrolna CRC-8 dla czujników Sensirion (poly 0x31, init 0xFF).
// Bez niej nie odróżnimy prawdziwej odpowiedzi czujnika od śmieci na I2C.
static uint8_t crc8Sensirion(const uint8_t* data, size_t len) {
  uint8_t crc = 0xFF;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (uint8_t bit = 0; bit < 8; bit++) {
      crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
    }
  }
  return crc;
}

// Odczyt N bajtów z I2C z krótkim oczekiwaniem na dane
static bool i2cRead(uint8_t addr, uint8_t* buf, size_t len) {
  if (Wire.requestFrom((int)addr, (int)len) != (int)len) return false;
  for (size_t i = 0; i < len; i++) {
    unsigned long t = millis();
    while (!Wire.available() && millis() - t < 20) delay(1);
    if (!Wire.available()) return false;
    buf[i] = Wire.read();
  }
  return true;
}

static void i2cWrite16(uint8_t addr, uint16_t cmd) {
  Wire.beginTransmission(addr);
  Wire.write((uint8_t)(cmd >> 8));
  Wire.write((uint8_t)(cmd & 0xFF));
  Wire.endTransmission();
}

void IRAM_ATTR SensorManager::rainIsr()  { rainPulses_++; }
void IRAM_ATTR SensorManager::anemIsr()  { anemPulses_++; }

Channel* SensorManager::ch(const String& id) {
  for (auto& c : channels_) if (c.id == id) return &c;
  return nullptr;
}

void SensorManager::setPresent(const String& id, bool present) {
  Channel* c = ch(id);
  if (c) c->present = present;
}

void SensorManager::setDetected(const String& id, bool detected) {
  Channel* c = ch(id);
  if (c) c->detected = detected;
}

bool SensorManager::begin() {
  mutex_ = xSemaphoreCreateMutex();

  // Kanały wewnętrzne (BME280)
  Channel t; t.id="temp"; t.name="Temperatura"; t.unit="°C"; t.decimals=1;
  t.zone="in";
  t.haClass="temperature"; t.haUnit="°C"; t.haIcon="mdi:thermometer";
  channels_.push_back(t);

  Channel h; h.id="hum"; h.name="Wilgotność"; h.unit="%"; h.decimals=1;
  h.zone="in";
  h.haClass="humidity"; h.haUnit="%"; h.haIcon="mdi:water-percent";
  channels_.push_back(h);

  Channel p; p.id="press"; p.name="Ciśnienie"; p.unit="hPa"; p.decimals=1;
  p.zone="in";
  p.haClass="pressure"; p.haUnit="hPa"; p.haIcon="mdi:gauge";
  channels_.push_back(p);

  // Natężenie światła (BH1750)
  Channel l; l.id="light"; l.name="Natężenie światła"; l.unit="lx"; l.decimals=0;
  l.zone="out";
  l.haClass="illuminance"; l.haUnit="lx"; l.haIcon="mdi:white-balance-sunny";
  channels_.push_back(l);

  // Czujniki zewnętrzne DS18B20 (do 8)
  for (int i = 0; i < 8; i++) {
    Channel d; d.id = "ds_" + String(i); d.name = "Zewn. DS18B20 #" + String(i + 1);
    d.unit = "°C"; d.decimals = 1; d.zone = "out";
    d.haClass = "temperature"; d.haUnit = "°C"; d.haIcon="mdi:thermometer";
    d.enabled = false;
    channels_.push_back(d);
  }

  // Deszcz
  Channel r; r.id="rain"; r.name="Opad (dziś)"; r.unit="mm"; r.decimals=1;
  r.zone="out";
  r.haClass="precipitation"; r.haUnit="mm"; r.haIcon="mdi:weather-rainy";
  channels_.push_back(r);

  // Wiatr
  Channel w; w.id="wind"; w.name="Wiatr"; w.unit="km/h"; w.decimals=1;
  w.zone="out";
  w.haClass="wind_speed"; w.haUnit="m/s"; w.haFactor = 1.0f / 3.6f; w.haIcon="mdi:weather-windy";
  channels_.push_back(w);

  Channel v; v.id="vane"; v.name="Kierunek wiatru"; v.unit="°"; v.decimals=0;
  v.zone="out";
  v.haClass=""; v.haUnit="°"; v.haIcon="mdi:compass";
  channels_.push_back(v);

  // Jakość powietrza na zewnątrz (PMS5003)
  Channel q1; q1.id="pm1"; q1.name="PM 1.0"; q1.unit="µg/m³"; q1.decimals=0;
  q1.zone="out";
  q1.haClass="pm1"; q1.haUnit="µg/m³"; q1.haIcon="mdi:air-filter";
  channels_.push_back(q1);
  Channel q2; q2.id="pm25"; q2.name="PM 2.5"; q2.unit="µg/m³"; q2.decimals=0;
  q2.zone="out";
  q2.haClass="pm25"; q2.haUnit="µg/m³"; q2.haIcon="mdi:air-filter";
  channels_.push_back(q2);
  Channel q3; q3.id="pm10"; q3.name="PM 10"; q3.unit="µg/m³"; q3.decimals=0;
  q3.zone="out";
  q3.haClass="pm10"; q3.haUnit="µg/m³"; q3.haIcon="mdi:air-filter";
  channels_.push_back(q3);

  // Jakość powietrza wewnątrz (SCD40/41 - CO2, SGP30/40 - eCO2 i TVOC)
  Channel c1; c1.id="co2"; c1.name="CO₂ (wewnątrz)"; c1.unit="ppm"; c1.decimals=0;
  c1.zone="in";
  c1.haClass="carbon_dioxide"; c1.haUnit="ppm"; c1.haIcon="mdi:molecule-co2";
  channels_.push_back(c1);
  Channel c2; c2.id="eco2"; c2.name="eCO₂ (wewnątrz)"; c2.unit="ppm"; c2.decimals=0;
  c2.zone="in";
  c2.haClass="carbon_dioxide"; c2.haUnit="ppm"; c2.haIcon="mdi:molecule-co2";
  channels_.push_back(c2);
  Channel c3; c3.id="tvoc"; c3.name="TVOC (wewnątrz)"; c3.unit="ppb"; c3.decimals=0;
  c3.zone="in";
  c3.haClass="volatile_organic_compounds_parts"; c3.haUnit="ppb";
  c3.haIcon="mdi:chemical-weapon";
  channels_.push_back(c3);

  // Dodatkowe czujniki (SHT4x, BMP581, VEML7700, LTR-390UV, MMC5983MA,
  // AS5600, ADS1115, SEN5x/SPS30, gleba, pyranometr) - rejestracja kanałów
  // przed zastosowaniem konfiguracji, żeby ustawienia z NVS też je objęły.
  registerExtraChannels();

  // Zastosuj konfigurację kanałów (włącz/wyłącz, offset, nazwa, strefa)
  applyChannelConfig();

  // --- Inicjalizacja sprzętu (każdy czujnik opcjonalny) ---

  // BME280 na I2C
  const int sda = pinMap.pin("i2c_sda");
  const int scl = pinMap.pin("i2c_scl");
  if (sda >= 0 && scl >= 0) {
    Wire.begin(sda, scl);
    Adafruit_BME280* b = new Adafruit_BME280();
    if (b->begin(0x76, &Wire) || b->begin(0x77, &Wire)) {
      bme_ = b;
      bmeOk_ = true;
      setPresent("temp", true);
      setPresent("hum", true);
      setPresent("press", true);
      setDetected("temp", true);
      setDetected("hum", true);
      setDetected("press", true);
    } else {
      delete b;
      LOG_W("BME280 nie odpowiedział na I2C (0x76/0x77) - brak temperatury, wilgotności i ciśnienia");
    }
  }

  // BH1750 (surowy I2C, bo biblioteki bywają zawodne na S3)
  Wire.beginTransmission(BH1750_ADDR);
  if (Wire.endTransmission() == 0) {
    Wire.beginTransmission(BH1750_ADDR);
    Wire.write(0x10); // tryb ciągły, rozdzielczość 1 lx
    Wire.endTransmission();
    bhOk_ = true;
    setPresent("light", true);
    setDetected("light", true);
  }

  // DS18B20 na OneWire
  const int owPin = pinMap.pin("onewire");
  if (owPin >= 0) {
    OneWire* ow = new OneWire(owPin);
    oneWire_ = ow;
    DallasTemperature* d = new DallasTemperature(ow);
    d->begin();
    dallas_ = d;
    dallasOk_ = true;
    requestDsDiscovery();
  }

  // Deszczomierz, anemometr i wiatrowskaz - tu tylko przypisujemy piny.
  // Czy czujnik naprawdę jest podłączony, poznajemy dopiero po pierwszym
  // realnym impulsie lub sensownym odczycie ADC (patrz detected).
  const int rainPin = pinMap.pin("rain");
  const int anemPin = pinMap.pin("anem");
  const int vanePin = pinMap.pin("vane");
  if (rainPin >= 0) {
    pinMode(rainPin, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(rainPin), rainIsr, FALLING);
    setPresent("rain", true);
  }
  if (anemPin >= 0) {
    pinMode(anemPin, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(anemPin), anemIsr, FALLING);
    setPresent("wind", true);
  }
  if (vanePin >= 0) {
    pinMode(vanePin, INPUT);
    setPresent("vane", true);
  }

  // PMS5003 na UART2 - kanały pm1/pm25/pm10 pojawią się dopiero, gdy czujnik
  // przyśle poprawną ramkę (sam pin RX nie znaczy, że czujnik jest podłączony).
  const int pmsRx = pinMap.pin("pms_rx");
  const int pmsTx = pinMap.pin("pms_tx");
#if PMS_UART_AVAILABLE
  if (pmsRx >= 0) {
    Serial2.begin(9600, SERIAL_8N1, pmsRx, pmsTx);
    pmsStarted_ = true;
  }
#else
  if (pmsRx >= 0) {
    LOG_W("PMS5003: ten chip ma tylko 2 UART-y - czujnik pyłu pominięty");
  }
#endif

  // Jakość powietrza wewnątrz (I2C): SCD40/41 (CO2) oraz SGP30/40 (eCO2+TVOC).
  // Domyślnie wykrywamy sami - wystarczy podłączyć czujnik do magistrali I2C,
  // bez zmiany ustawień. Wymuszenie typu: ustawienie "aq_type" na stronie www.
  {
    const uint8_t want = config.aqType();   // 0 = wykryj sam, 3 = wyłączone
    if (want != 3) {
      if (want == 0 || want == 1) initScd4x();
      if (want == 0 || want == 2) initSgp30();
    }
    // Sam fakt odpowiedzi na I2C nie znaczy, że przyszła już próbka - dlatego
    // kanał jest tylko "skonfigurowany" (present); detected ustawia pierwszy
    // sensowny pomiar w readAq(). Dzięki temu na stronie widać "czeka na dane"
    // (SCD4x potrzebuje ~5 s, SGP30/40 ~15 s na pierwszy poprawny odczyt).
    if (scdOk_) {
      setPresent("co2", true);
      aqName_ = "SCD40/SCD41";
    }
    if (sgpOk_) {
      setPresent("eco2", true);
      setPresent("tvoc", true);
      aqName_ += (aqName_.length() ? " + " : "") + String("SGP30/SGP40");
    }
    if (aqName_.length()) LOG_I("Jakość powietrza wewnątrz: %s", aqName_.c_str());
  }

  computeRain();

  // Dodatkowe czujniki I2C - wykrywanie robimy po starcie magistrali
  // (BME280/BH1750 kończą konfigurację Wire powyżej).
  beginExtra();

  {
    unsigned present = 0, detected = 0;
    for (auto& c : channels_) { if (c.present) present++; if (c.detected) detected++; }
    LOG_I("Czujniki: BME280 %s, BH1750 %s, OneWire %s, PMS5003 %s, deszcz %s, wiatr %s, kierunek %s",
          bmeOk_ ? "OK" : "brak", bhOk_ ? "OK" : "brak", dallasOk_ ? "OK" : "brak",
          pmsOk_ ? "OK" : "brak", rainPin >= 0 ? "pin" : "brak",
          anemPin >= 0 ? "pin" : "brak", vanePin >= 0 ? "pin" : "brak");
    LOG_I("Kanały: %u skonfigurowanych, %u potwierdzonych sprzętowo", present, detected);
  }
  return true;
}
std::vector<Channel> SensorManager::snapshot() {
  std::vector<Channel> out;
  if (mutex_) xSemaphoreTake(mutex_, portMAX_DELAY);
  // Kopia całej listy kanałów (z nazwami i opisami HA) potrzebuje sporego,
  // ciągłego bloku pamięci. Na płytce bez PSRAM potrafi jej zabraknąć - wtedy
  // zwracamy pustą listę zamiast wywalać stację przez std::bad_alloc (=abort).
  try {
    out = channels_;
  } catch (const std::bad_alloc&) {
    out.clear();
    LOG_W("Brak pamięci na kopię listy kanałów (%u) - pominięto",
          (unsigned)channels_.size());
  } catch (...) {
    out.clear();
    LOG_W("Błąd kopiowania listy kanałów (%u) - pominięto",
          (unsigned)channels_.size());
  }
  if (mutex_) xSemaphoreGive(mutex_);
  return out;
}

uint32_t SensorManager::localPage(uint32_t off, uint32_t cnt, std::vector<Channel>& out) {
  out.clear();
  uint32_t total = 0;
  if (mutex_) xSemaphoreTake(mutex_, portMAX_DELAY);
  try {
    for (auto& c : channels_) {
      if (c.remote) continue;
      if (total >= off && (!cnt || out.size() < cnt)) out.push_back(c);
      total++;
    }
  } catch (const std::bad_alloc&) {
    LOG_W("Brak pamięci na stronę kanałów RS485");
  }
  if (mutex_) xSemaphoreGive(mutex_);
  return total;
}

// Ustawienia z zakładki Kalibracja (nazwa, włączenie, strefa) są trzymane w NVS.
// Po zapisie stosujemy je od razu, żeby nie trzeba było restartować stacji.
void SensorManager::applyChannelConfig() {
  if (mutex_) xSemaphoreTake(mutex_, portMAX_DELAY);
  for (auto& c : channels_) {
    ChannelConfig cc = config.channelCfg(c.id);
    c.enabled = cc.enabled;
    if (cc.name.length()) c.name = cc.name;
    if (cc.zone == "in" || cc.zone == "out") c.zone = cc.zone;
  }
  if (mutex_) xSemaphoreGive(mutex_);
}

// Pojedyncza wartość kanału - NAN gdy kanał nie istnieje, jest wyłączony,
// czujnik nie jest wykryty albo nie przysłał jeszcze żadnej prawdziwej próbki
// (używane m.in. przez pierścień LED RGB).
// Gdy tej stacji brakuje danego czujnika (albo nie ma z niego próbki),
// sięgamy po pomiar z węzłów podłączonych po RS485 - dzięki temu pierścień LED
// i prognoza na masterze działają na czujnikach zewnętrznych podłączonych do
// dowolnego węzła (kanał zdalny ma id "x<adres>_<id>", np. "x3_temp").
float SensorManager::valueOf(const String& id) {
  float v = NAN;         // kanał lokalny
  float rv = NAN;        // kanał zdalny (x<adres>_<id>)
  String suffix = "_" + id;
  if (mutex_) xSemaphoreTake(mutex_, portMAX_DELAY);
  for (auto& c : channels_) {
    if (c.id == id) {
      if (c.enabled && c.present && c.measured) v = c.value;
    } else if (isnan(rv) && c.remote && c.enabled && c.measured &&
               c.id.endsWith(suffix)) {
      rv = c.value;
    }
  }
  if (mutex_) xSemaphoreGive(mutex_);
  return isnan(v) ? rv : v;
}

float SensorManager::valueByHaClass(const char* haClass, const char* zone) {
  if (!haClass || !zone) return NAN;
  float out = NAN;
  if (mutex_) xSemaphoreTake(mutex_, portMAX_DELAY);
  for (auto& c : channels_) {
    if (!c.enabled || !c.measured) continue;
    if (!(c.zone == zone) || !(c.haClass == haClass)) continue;
    if (isnan(c.value) || isinf(c.value)) continue;
    out = c.value;
    break;
  }
  if (mutex_) xSemaphoreGive(mutex_);
  return out;
}

// Zapisuje wartość kanału i oznacza go jako realnie zmierzony.
void SensorManager::putValue(const String& id, float v, bool valid) {
  Channel* c = ch(id);
  if (!c) return;
  c->value = valid ? v : NAN;
  if (valid) c->measured = true;
}

void SensorManager::readAll() {
  readBME280();
  readBH1750();
  readDallas();
  readPMS5003();
  readVane();
  computeRain();
  readWind();
  readAq();
  readExtra();
}

// -------------------------------------------------------------
//  Jakość powietrza wewnątrz: SCD40/SCD41 (CO2) i SGP30/SGP40 (eCO2+TVOC)
// -------------------------------------------------------------

// SCD40/SCD41: rozpoznajemy po numerze seryjnym (komenda 0x3682) - sama
// odpowiedź adresu nie wystarczy, bo taki sam numer ma wiele innych układów.
bool SensorManager::initScd4x() {
  i2cWrite16(SCD4X_ADDR, 0x3682);
  delay(5);
  uint8_t buf[9] = {0};
  if (!i2cRead(SCD4X_ADDR, buf, sizeof(buf))) return false;
  if (crc8Sensirion(buf, 2) != buf[2]) return false;
  uint16_t serial = (uint16_t)((buf[0] << 8) | buf[1]);
  if (serial == 0 || serial == 0xFFFF) return false;

  i2cWrite16(SCD4X_ADDR, 0x21B1);       // start_periodic_measurement
  delay(20);
  scdOk_ = true;
  scdStartMs_ = millis();
  LOG_I("SCD4x: wykryty (nr seryjny %u), pomiar ciągły włączony", (unsigned)serial);
  return true;
}

// SGP30/SGP40: komenda init_air_quality (0x2003) - gdy układ odpowie ACK,
// mamy pewność, że coś siedzi pod adresem 0x58 i przyjmuje komendy.
bool SensorManager::initSgp30() {
  Wire.beginTransmission(SGP30_ADDR);
  Wire.write((uint8_t)0x20);
  Wire.write((uint8_t)0x03);
  if (Wire.endTransmission() != 0) return false;
  delay(10);
  sgpOk_ = true;
  sgpStartMs_ = millis();
  LOG_I("SGP30/SGP40: wykryty, rozgrzewanie ok. 15 s");
  return true;
}

void SensorManager::readAq() {
  if (!scdOk_ && !sgpOk_) return;

  ChannelConfig cco2 = config.channelCfg("co2");
  ChannelConfig ceco = config.channelCfg("eco2");
  ChannelConfig ctv  = config.channelCfg("tvoc");

  // --- SCD40/41: CO2 w ppm ---
  if (scdOk_) {
    i2cWrite16(SCD4X_ADDR, 0xEC05);     // read_measurement
    delay(5);
    uint8_t buf[9] = {0};
    bool ok = i2cRead(SCD4X_ADDR, buf, sizeof(buf)) &&
              crc8Sensirion(buf, 2) == buf[2];
    uint16_t co2 = ok ? (uint16_t)((buf[0] << 8) | buf[1]) : 0;
    // 0 ppm to wartość "jeszcze nie zmierzone" (czujnik potrzebuje ~5 s)
    if (ok && co2 > 0) {
      if (mutex_) xSemaphoreTake(mutex_, portMAX_DELAY);
      putValue("co2", (float)co2 + cco2.offset);
      setDetected("co2", true);
      if (mutex_) xSemaphoreGive(mutex_);
    } else if (millis() - scdStartMs_ > 30000) {
      // brak poprawnych danych przez 30 s - pokazujemy to na stronie
      if (mutex_) xSemaphoreTake(mutex_, portMAX_DELAY);
      putValue("co2", NAN, false);
      if (mutex_) xSemaphoreGive(mutex_);
    }
  }

  // --- SGP30/40: eCO2 w ppm i TVOC w ppb ---
  if (sgpOk_) {
    i2cWrite16(SGP30_ADDR, 0x2008);     // measure_air_quality
    delay(12);
    uint8_t buf[6] = {0};
    bool ok = i2cRead(SGP30_ADDR, buf, sizeof(buf)) &&
              crc8Sensirion(buf, 2) == buf[2] &&
              crc8Sensirion(buf + 3, 2) == buf[5];
    // Pierwsze ~15 s po starcie czujnik zwraca stałe 400 ppm / 0 ppb -
    // to nie pomiar, więc czekamy, aż pojawią się realne wartości.
    if (ok && (millis() - sgpStartMs_ > 15000) &&
        !(buf[0] == 0 && buf[1] == 0 && buf[3] == 0 && buf[4] == 0) &&
        !(buf[0] == 0x01 && buf[1] == 0x90 && buf[3] == 0 && buf[4] == 0)) {
      float eco2 = (float)(((uint16_t)buf[0] << 8) | buf[1]);
      float tvoc = (float)(((uint16_t)buf[3] << 8) | buf[4]);
      if (mutex_) xSemaphoreTake(mutex_, portMAX_DELAY);
      putValue("eco2", eco2 + ceco.offset);
      putValue("tvoc", tvoc + ctv.offset);
      setDetected("eco2", true);
      setDetected("tvoc", true);
      if (mutex_) xSemaphoreGive(mutex_);
    }
  }
}

String SensorManager::aqName() { return aqName_; }

// Kierunek wiatru jako strona świata (16 kierunków) - używany na stronie www,
// w Home Assistant i w serwisach pogodowych, bo same stopnie są nieczytelne.
String SensorManager::windCompass() {
  float deg = valueOf("vane");
  if (isnan(deg)) return "";
  int idx = (int)((deg + 11.25f) / 22.5f) % 16;
  if (idx < 0) idx += 16;
  return String(COMPASS_16[idx]);
}

void SensorManager::readBME280() {
  if (!bmeOk_ || !bme_) return;
  Adafruit_BME280* b = (Adafruit_BME280*)bme_;
  float t = b->readTemperature();
  float h = b->readHumidity();
  float p = b->readPressure() / 100.0f;

  ChannelConfig ct = config.channelCfg("temp");
  ChannelConfig chh = config.channelCfg("hum");
  ChannelConfig cp = config.channelCfg("press");

  if (mutex_) xSemaphoreTake(mutex_, portMAX_DELAY);
  putValue("temp",  t + ct.offset,  !isnan(t));
  putValue("hum",   h + chh.offset, !isnan(h));
  putValue("press", p + cp.offset,  !isnan(p));
  if (mutex_) xSemaphoreGive(mutex_);
}

void SensorManager::readBH1750() {
  if (!bhOk_) return;
  Wire.beginTransmission(BH1750_ADDR);
  Wire.requestFrom((int)BH1750_ADDR, 2);
  if (Wire.available() == 2) {
    uint16_t raw = ((uint16_t)Wire.read() << 8) | Wire.read();
    float lux = raw / 1.2f;
    ChannelConfig c = config.channelCfg("light");
    if (mutex_) xSemaphoreTake(mutex_, portMAX_DELAY);
    putValue("light", lux + c.offset);
    if (mutex_) xSemaphoreGive(mutex_);
  }
}

void SensorManager::serviceDiscovery() {
  if (!dsDiscoverRequested_ || !dallasOk_ || !dallas_) return;
  dsDiscoverRequested_ = false;

  DallasTemperature* d = (DallasTemperature*)dallas_;
  d->begin(); // ponowny skan magistrali
  int n = d->getDeviceCount();
  if (n > 8) n = 8;
  dsCount_ = n;
  LOG_I("Magistrala OneWire: wykryto %d czujników DS18B20", n);

  // Włącz kanały ds_0..ds_{n-1}, wyłącz pozostałe
  if (mutex_) xSemaphoreTake(mutex_, portMAX_DELAY);
  for (int i = 0; i < 8; i++) {
    Channel* c = ch("ds_" + String(i));
    if (!c) continue;
    c->present = (i < n);
    if (i < n && n > 0 && i == 0) c->enabled = true; // pierwszy domyślnie włączony
    if (i >= n) c->enabled = false;
  }
  if (mutex_) xSemaphoreGive(mutex_);
}

void SensorManager::readDallas() {
  if (!dallasOk_ || !dallas_) return;
  DallasTemperature* d = (DallasTemperature*)dallas_;
  d->requestTemperatures();

  int n = d->getDeviceCount();
  if (n > 8) n = 8;
  dsCount_ = n;

  for (int i = 0; i < n; i++) {
    float t = d->getTempCByIndex(i);
    String id = "ds_" + String(i);
    ChannelConfig c = config.channelCfg(id);
    if (mutex_) xSemaphoreTake(mutex_, portMAX_DELAY);
    Channel* cc = ch(id);
    if (cc) {
      cc->present = true;
      const bool ok = !(t == DEVICE_DISCONNECTED_C || t < -55.0f);
      if (ok) cc->detected = true;
      putValue(id, t + c.offset, ok);
    }
    if (mutex_) xSemaphoreGive(mutex_);
  }
}

void SensorManager::readPMS5003() {
#if !PMS_UART_AVAILABLE
  return;   // ten chip ma tylko 2 UART-y - brak obsługi PMS5003
#else
  if (!pmsStarted_ || pinMap.pin("pms_rx") < 0) return;

  // Czekamy na nagłówek 0x42 0x4D
  if (!Serial2.available()) return;
  if (Serial2.read() != 0x42) return;
  if (!Serial2.available()) return;
  if (Serial2.read() != 0x4D) return;

  // Długość ramki (2 bajty big-endian)
  unsigned long timeout = millis() + 500;
  while (Serial2.available() < 2 && millis() < timeout) delay(1);
  if (Serial2.available() < 2) return;
  uint16_t len = ((uint16_t)Serial2.read() << 8) | Serial2.read();
  if (len < 28) return;

  uint8_t buf[28];
  timeout = millis() + 500;
  size_t got = 0;
  while (got < 28 && millis() < timeout) {
    if (Serial2.available()) buf[got++] = Serial2.read();
  }
  if (got < 28) return;

  // Suma kontrolna
  uint16_t sum = 0x42 + 0x4D + (len >> 8) + (len & 0xFF);
  for (int i = 0; i < 26; i++) sum += buf[i];
  uint16_t rxc = ((uint16_t)buf[26] << 8) | buf[27];
  if (sum != rxc) return;

  auto u16 = [&](int i) { return ((uint16_t)buf[i] << 8) | buf[i + 1]; };
  float pm1  = u16(6);   // PM1.0 atmosferyczne
  float pm25 = u16(8);   // PM2.5 atmosferyczne
  float pm10 = u16(10);  // PM10 atmosferyczne

  if (mutex_) xSemaphoreTake(mutex_, portMAX_DELAY);
  if (!pmsOk_) {
    // Pierwsza poprawna ramka = czujnik jakości powietrza naprawdę jest podłączony
    pmsOk_ = true;
    setPresent("pm1", true);  setDetected("pm1", true);
    setPresent("pm25", true); setDetected("pm25", true);
    setPresent("pm10", true); setDetected("pm10", true);
    LOG_I("PMS5003: odebrano poprawną ramkę - czujnik jakości powietrza działa");
  }
  putValue("pm1",  pm1);
  putValue("pm25", pm25);
  putValue("pm10", pm10);
  if (mutex_) xSemaphoreGive(mutex_);
#endif  // PMS_UART_AVAILABLE
}

// Sprawdza, czy wejście analogowe jest "wiszące" (nic nie jest podłączone).
// Wolny pin ADC nie ma żadnej polaryzacji, więc wewnętrzny rezystor podciągający
// (~45 kΩ) wychyla go prawie do pełnej skali. Prawdziwy czujnik (dzielnik
// rezystorowy o impedancji rzędu kilku kΩ) zmienia się tylko nieznacznie.
static bool analogInputFloating(int pin) {
  pinMode(pin, INPUT_PULLDOWN);
  delay(2);
  const int lo = analogRead(pin);
  pinMode(pin, INPUT_PULLUP);
  delay(2);
  const int hi = analogRead(pin);
  pinMode(pin, INPUT);           // przywrócenie stanu wysokiej impedancji
  delay(2);
  return (hi - lo) > VANE_FLOAT_DELTA;
}

void SensorManager::readVane() {
  const int vanePin = pinMap.pin("vane");
  if (vanePin < 0) return;

  // Zapobiegnięcie fałszywemu wykryciu czujnika na wolnym pinie ADC — bez tego
  // wiatrowskaz "działa" jeszcze przed zlutowaniem czujników.
  if (analogInputFloating(vanePin)) {
    if (mutex_) xSemaphoreTake(mutex_, portMAX_DELAY);
    setDetected("vane", false);
    putValue("vane", NAN, false);
    if (mutex_) xSemaphoreGive(mutex_);
    return;
  }

  // Wiatrowskaz to dzielnik rezystorowy - stabilne próbki potwierdzają odczyt.
  int rawMin = 4095, rawMax = 0;
  long rawSum = 0;
  for (int i = 0; i < 8; i++) {
    int s = analogRead(vanePin);
    if (s < rawMin) rawMin = s;
    if (s > rawMax) rawMax = s;
    rawSum += s;
    delayMicroseconds(300);
  }
  const int raw = (int)(rawSum / 8);

  // Odczyt sensowny, gdy próbki są stabilne i nie leżą na skraju zakresu.
  const bool ok = (rawMax - rawMin) <= VANE_MAX_SPAN && (raw > 30 && raw < 4060);

  // Tabela typowego wiatrowskazu rezystorowego (8 kierunków).
  // Progi dopasuj po zlutowaniu - to jest orientacyjny szablon.
  struct { int adc; float deg; } table[] = {
    {4095, 0.0f}, {3000, 45.0f}, {1800, 90.0f}, {900, 135.0f},
    {600, 180.0f}, {300, 225.0f}, {150, 270.0f}, {0, 315.0f}
  };
  float deg = 0.0f;
  for (auto& e : table) {
    if (raw >= e.adc) { deg = e.deg; break; }
  }
  if (mutex_) xSemaphoreTake(mutex_, portMAX_DELAY);
  setDetected("vane", ok);
  putValue("vane", deg, ok);
  if (mutex_) xSemaphoreGive(mutex_);
}

void SensorManager::computeRain() {
  unsigned long pulses;
  noInterrupts();
  pulses = rainPulses_;
  rainPulses_ = 0;
  interrupts();

  if (pulses == 0) return;
  config.state.rainTipsTotal += pulses;

  // Wykryj zmianę doby (rok*1000 + dzień roku)
  time_t now = time(nullptr);
  struct tm tmv;
  localtime_r(&now, &tmv);
  int dayKey = (tmv.tm_year + 1900) * 1000 + tmv.tm_yday;

  if (config.state.rainDay != dayKey) {
    config.state.rainDay = dayKey;
    config.state.rainStartTips = config.state.rainTipsTotal - pulses;
  }

  float mm = (config.state.rainTipsTotal - config.state.rainStartTips) * RAIN_MM_PER_TIP;
  if (mutex_) xSemaphoreTake(mutex_, portMAX_DELAY);
  putValue("rain", mm);   // wywołane tylko po realnym impulsie deszczomierza
  setDetected("rain", true); // pierwszy impuls = deszczomierz naprawdę jest podłączony
  if (mutex_) xSemaphoreGive(mutex_);

  config.saveState();
}

void SensorManager::readWind() {
  const int anemPin = pinMap.pin("anem");
  if (anemPin < 0) return;
  unsigned long now = millis();
  unsigned long dt = now - lastAnemRead_;
  if (dt < ANEM_WINDOW_MS) return;

  unsigned long pulses;
  noInterrupts();
  pulses = anemPulses_;
  interrupts();

  float hz = (float)(pulses - lastAnemPulses_) * 1000.0f / (float)dt;
  // Wiatromierz bez jednego impulsu to zwykle niepodłączony czujnik - dopiero
  // po pierwszym impulsie wiemy, że 0 km/h oznacza realny bezwietrzny pomiar.
  if (pulses != lastAnemPulses_) anemSeen_ = true;
  lastAnemPulses_ = pulses;
  lastAnemRead_ = now;

  windKmh_ = hz * ANEM_KMH_PER_HZ;
  if (mutex_) xSemaphoreTake(mutex_, portMAX_DELAY);
  if (anemSeen_) setDetected("wind", true);
  putValue("wind", windKmh_, anemSeen_);
  if (mutex_) xSemaphoreGive(mutex_);
}
