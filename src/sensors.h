/* =============================================================================
 * Stacja Pogody - stacja meteorologiczna na ESP32-S3 WROOM-1 (N16R8)
 * Copyright (c) 2026 kruzio1985 - https://github.com/kruzio1985
 * Licencja: Stacja Pogody Non-Commercial License 1.0 (plik LICENSE)
 * Użytek niekomercyjny. Kontakt: kruzio1985@users.noreply.github.com
 * =============================================================================
 */
#pragma once
#include <Arduino.h>
#include <vector>
#include "pins.h"

// =============================================================
//  Pojedynczy kanał pomiarowy (jeden wskaźnik na stronie / w HA)
// =============================================================
struct Channel {
  String id;          // unikalny identyfikator (temp, hum, ds_0, ...)
  String name;        // nazwa wyświetlana (PL)
  String unit;        // jednostka na stronie ("°C", "%", "hPa", ...)
  // strefa czujnika: "in" = wewnątrz, "out" = na zewnątrz. Strona www dzieli
  // pulpit na te dwie sekcje, a serwisy pogodowe (WU/Windy/OWM) mogą wysyłać
  // wyłącznie pomiary z czujników zewnętrznych.
  String zone = "out";
  float value = NAN;  // bieżąca wartość surowa (po kalibracji)
  bool enabled = true;
  // present = kanał jest w ogóle skonfigurowany (pin przypisany / sterownik wystartował)
  bool present = false;
  // detected = czujnik potwierdzony sprzętowo: odpowiedział na magistrali
  // (I2C/OneWire) albo przyszedł z niego realny pomiar (impuls, ramka, odczyt ADC).
  // Bez tego rozróżnienia strona www pokazywała jako "podłączone" czujniki,
  // których fizycznie nie ma (np. deszczomierz/anemometr po samym przypisaniu pinu).
  bool detected = false;
  // true = z czujnika przyszła już prawdziwa próbka (używane przez pierścień
  // LED, żeby nie pokazywał pogody na podstawie samych zer z niepodłączonych
  // czujników)
  bool measured = false;
  int decimals = 1;

  // true = kanał pochodzi z innego ESP (magistrala RS485, id "x2_...", "x3_...").
  // Strona www pokazuje takie kanały w osobnej sekcji pulpitu.
  bool remote = false;
  // Adres węzła RS485, z którego pochodzi kanał (0 = kanał lokalny).
  uint8_t remoteAddr = 0;

  // Mapowanie do Home Assistant (auto-discovery)
  String haClass;     // np. temperature, humidity, pressure, wind_speed
  String haUnit;      // jednostka w HA, np. "°C", "%", "hPa"
  float haFactor = 1.0f; // mnożnik przed wysłaniem do HA (np. km/h -> m/s)
  String haIcon;      // mdi:...
};

class SensorManager {
public:
  bool begin();

  // Zbiera dane ze wszystkich podłączonych czujników (wołane co N sekund)
  void readAll();

  // Ponowne zastosowanie ustawień kanałów (włącz/wyłącz, nazwa, strefa)
  // po zapisie z zakładki Kalibracja - bez restartu stacji.
  void applyChannelConfig();

  // Snapshot kanałów (bezpieczny wątkowo - kopiuje dane pod mutexem)
  std::vector<Channel> snapshot();

  // Strona listy własnych kanałów (bez zdalnych) na potrzeby RS485 - lista jest
  // zbyt długa, by zmieścić ją w jednej ramce. cnt == 0 => bez limitu.
  // Zwraca łączną liczbę własnych kanałów.
  uint32_t localPage(uint32_t off, uint32_t cnt, std::vector<Channel>& out);

  // Bieżąca wartość pojedynczego kanału (NAN = brak danych / kanał wyłączony)
  float valueOf(const String& id);

  // Wartość pierwszego kanału danego typu (klasa HA, np. "temperature") w danej
  // strefie ("in"/"out"). Odczyt idzie wprost po liście kanałów, bez jej
  // kopiowania - na płytce bez PSRAM kopia całej listy nie mieści się w heapie.
  float valueByHaClass(const char* haClass, const char* zone);

  // Wyszukiwanie nowych czujników DS18B20 - musi być wołane z loop()
  // (biblioteka Dallas nie lubi współbieżnego dostępu)
  void serviceDiscovery();

  // --- Sterowniki ---
  void readBME280();
  void readBH1750();
  void readDallas();
  void readPMS5003();
  void readVane();
  void readWind();
  void computeRain();
  void readAq();               // SCD40/41 (CO2) i SGP30 (eCO2 + TVOC)

  // Kierunek wiatru jako strona świata ("N", "SW", ...) - pusta wartość,
  // gdy czujnik nie podał jeszcze realnego pomiaru
  String windCompass();

  // Nazwa wykrytego czujnika jakości powietrza ("" gdy żaden)
  String aqName();

  // --- Dodatkowe czujniki (sterowniki opisane w drv_i2c.* / drv_pm.*) ---
  void registerExtraChannels();   // rejestracja kanałów (przed applyChannelConfig)
  bool beginExtra();              // wykrywanie sprzętu na magistrali I2C
  void readExtra();               // odczyt dodatkowych czujników
  String extraAqName();           // nazwa czujnika pyłu wewnątrz ("" gdy brak)

  // --- Mostek dla sterowników modułowych (drv_*.cpp, kontrakt w drv_mod.h) ---
  // Moduły nie znają SensorManagera - zgłaszają tylko identyfikatory kanałów,
  // a te trzy metody robią z nich pełnoprawne kanały (obecność, wykrycie,
  // wartość z korektą kalibracji, publikacja do MQTT/HA/CSV).
  void drvFound(const char* id);              // sprzęt potwierdzony (bez pomiaru)
  void drvPublish(const char* id, float v);   // potwierdzony pomiar
  void drvClear(const char* id);              // odczyt nieudany - czyści wartość

  // --- Kanały zdalne z innych ESP (magistrala RS485) ---
  // Każdy kanał węzła o adresie N jest odbijany jako kanał "xN_<id>"
  // (np. x2_temp, x3_wind), więc pojawia się na pulpicie, w wykresach, w CSV
  // i w Home Assistant. Adres 0 w tych metodach znaczy "wszystkie węzły".
  // remoteBegin()   - przed odczytem: czyści wartości (brak łącza => "—")
  // remotePublish() - zapisuje kanał z odebranej odpowiedzi węzła
  // remoteClear()   - usuwa kanały zdalne (magistrala wyłączona / węzeł usunięty)
  static bool isRemoteId(const String& id);   // "x2_temp" / "x12_ds_3" => true
  static uint8_t remoteAddrOf(const String& id);   // "x12_ds_3" => 12, inaczej 0
  void remoteBegin(uint8_t addr = 0);
  void remotePublish(uint8_t addr, const String& id, const String& name, const String& unit,
                     const String& zone, float v, bool detected, bool measured,
                     int decimals, const String& haClass, const String& haUnit,
                     const String& haIcon);
  void remoteClear(uint8_t addr = 0);
  // Aktualizuje samą wartość już znanego kanału zdalnego (odpowiedź na "vals").
  // Zwraca false, gdy kanał nie istnieje (np. metadane jeszcze nie dotarły).
  bool remoteValue(uint8_t addr, const String& id, float v, bool detected, bool measured);
  int  remoteCount(uint8_t addr = 0);

  // --- Przerwania (statyczne, bo ISR) ---
  static void IRAM_ATTR rainIsr();
  static void IRAM_ATTR anemIsr();

  void requestDsDiscovery() { dsDiscoverRequested_ = true; }
  int  dsSensorCount()       { return dsCount_; }
  bool pmsDetected()         { return pmsOk_; }

private:
  Channel* ch(const String& id);
  void setPresent(const String& id, bool present);
  void setDetected(const String& id, bool detected);
  bool initScd4x();            // SCD40/SCD41 na I2C (CO2)
  bool initSgp30();            // SGP30/SGP40 na I2C (eCO2 + TVOC)
  // valid=false czyści wartość (NAN) i nie oznacza kanału jako zmierzonego.
  // Mutex musi być już zajęty przez wołającego.
  void putValue(const String& id, float v, bool valid = true);

  // Dodatkowe czujniki: zapis potwierdzonego pomiaru (present + detected +
  // wartość z korektą kalibracji) oraz wyczyszczenie nieaktualnej wartości.
  void publishExtra(const String& id, float v);
  void clearExtra(const String& id);

  std::vector<Channel> channels_;
  SemaphoreHandle_t mutex_ = nullptr;

  // BME280 (I2C, opcjonalny)
  void* bme_ = nullptr;
  bool bmeOk_ = false;

  // BH1750 (I2C, surowy odczyt - opcjonalny)
  bool bhOk_ = false;

  // DS18B20 (OneWire, obsługa wielu czujników)
  void* oneWire_ = nullptr;
  void* dallas_ = nullptr;
  bool dallasOk_ = false;
  int dsCount_ = 0;
  volatile bool dsDiscoverRequested_ = false;

  // PMS5003 (UART2, opcjonalny)
  bool pmsStarted_ = false;   // UART otwarty
  bool pmsOk_ = false;        // odebrano poprawną ramkę (czujnik naprawdę jest)

  // Deszcz / wiatr
  static volatile unsigned long rainPulses_;
  static volatile unsigned long anemPulses_;
  unsigned long lastAnemRead_ = 0;
  unsigned long lastAnemPulses_ = 0;
  bool anemSeen_ = false;   // wiatromierz dał już choć jeden impuls
  float windKmh_ = 0.0f;

  // Jakość powietrza wewnętrznego (I2C, opcjonalne)
  bool scdOk_ = false;              // SCD40/SCD41 odpowiedział (CO2)
  unsigned long scdStartMs_ = 0;
  bool sgpOk_ = false;              // SGP30/SGP40 odpowiedział (eCO2 + TVOC)
  unsigned long sgpStartMs_ = 0;
  String aqName_;
};

extern SensorManager sensors;
