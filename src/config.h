/* =============================================================================
 * Stacja Pogody - stacja meteorologiczna na ESP32-S3 WROOM-1 (N16R8)
 * Copyright (c) 2026 kruzio1985 - https://github.com/kruzio1985
 * Licencja: PolyForm Noncommercial License 1.0.0 (plik LICENSE)
 * Użytek niekomercyjny. Kontakt: kruzio1985@users.noreply.github.com
 * =============================================================================
 */
#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

#define FW_VERSION "1.0.0"

// -----------------------------------------------------------------------------
//  Rola urządzenia wybrana przy kompilacji (bin 1 = master, bin 2 = węzeł).
//  Ustawia ją platformio.ini w środowiskach esp32s3-master / esp32s3-node.
//  Bez definicji firmware zachowuje się jak dotychczas, czyli master.
// -----------------------------------------------------------------------------
#ifndef STACJA_ROLE_MASTER
#define STACJA_ROLE_MASTER 1
#endif

#if STACJA_ROLE_MASTER
#define FW_ROLE_NAME "master"
#else
#define FW_ROLE_NAME "node"
#endif

// -----------------------------------------------------------------------------
//  Węzeł pracujący BEZ SIECI (STACJA_HEADLESS=1, ustawia platformio.ini dla
//  esp32c3-node). Taki firmware ma wszystkie sterowniki czujników, ale nie ma
//  strony www, punktu dostępowego, Wi-Fi, MQTT, serwisów pogodowych i OTA:
//     - zbiera dane ze swoich czujników i czeka na pytania mastera po RS485,
//     - master pokazuje te dane na pulpicie, zapisuje na karcie SD i wysyła
//       dalej (MQTT/Home Assistant/serwisy pogodowe),
//     - ustawienia (piny, adres, kalibracja) przychodzą z mastera magistralą,
//     - nowy firmware wgrywa się kablem USB, bo bez sieci nie ma OTA.
//  Dzięki temu na płytce bez PSRAM (ESP32-C3 SuperMini) zostaje dużo wolnego
//  RAM-u i flasha na sterowniki.
// -----------------------------------------------------------------------------
//  UWAGA dla kodu: makro jest ZAWSZE zdefiniowane (0 albo 1), więc sprawdzać
//  je wyłącznie przez "#if STACJA_HEADLESS" / "#if !STACJA_HEADLESS".
//  "#ifdef STACJA_HEADLESS" było prawdziwe także w masterze (bo tutaj makro
//  dostaje wartość 0), przez co każda kompilacja wychodziła jako węzeł bez sieci.
#ifndef STACJA_HEADLESS
#define STACJA_HEADLESS 0
#endif

// -----------------------------------------------------------------------------
//  Kamera (opcjonalna). Makro jest ZAWSZE zdefiniowane (0 albo 1) i ustawiane
//  w platformio.ini flagą -DSTACJA_HAS_CAMERA=1 tylko dla wariantów ESP32-S3
//  (ESP32-C3 nie ma sprzętowej obsługi kamery w esp32-camera). Cały kod kamery
//  należy sprawdzać przez "#if STACJA_HAS_CAMERA" / "#if !STACJA_HAS_CAMERA".
// -----------------------------------------------------------------------------
#ifndef STACJA_HAS_CAMERA
#define STACJA_HAS_CAMERA 0
#endif

// -----------------------------------------------------------------------------
//  Fabryczny adres węzła (STACJA_NODE_ADDR). Domyślny adres magistrali, jaki
//  dostaje węzeł przy PIERWSZYM uruchomieniu (pusta NVS, brak zapisanych
//  ustawień). Gotowe binarki "z adresem" (dist\*-node-*-addrN.bin) ustawiają
//  tę wartość flagą -DSTACJA_NODE_ADDR=N, dzięki czemu kilka świeżych modułów
//  może być od razu podłączonych na magistrali bez kolizji (każdy na innym
//  adresie). Adres nadal można zmienić z www mastera (polecenie "assign" po
//  magistrali) - zapisany w NVS ma pierwszeństwo przed tą wartością domyślną.
// -----------------------------------------------------------------------------
#ifndef STACJA_NODE_ADDR
#define STACJA_NODE_ADDR 2
#endif
#define STACJA_STR1(x) #x
#define STACJA_STR(x)  STACJA_STR1(x)
#define STACJA_NODE_ADDR_STR STACJA_STR(STACJA_NODE_ADDR)

// -----------------------------------------------------------------------------
//  Prędkość magistrali RS485 dla WĘZŁA BEZ SIECI (STACJA_HEADLESS=1).
//  Węzeł headless nie ma strony www, więc prędkości nie da się u niego
//  zmienić ręcznie. Żeby kilka modułów z czystą NVS startowało od razu na
//  tej samej prędkości co master, prędkość jest wpisywana przy kompilacji
//  (flaga -DSTACJA_NODE_BAUD=N w platformio.ini) i ma pierwszeństwo przed
//  wartością zapisaną w NVS. Master ustawia u siebie tę samą wartość w www
//  (Ustawienia -> RS485 -> Prędkość). Domyślnie 19200 b/s - na linii TTL
//  bez transceivera stabilniejsze niż 115200, a ramka ~2 kB schodzi ~1 s.
// -----------------------------------------------------------------------------
#ifndef STACJA_NODE_BAUD
#define STACJA_NODE_BAUD 19200
#endif

// -----------------------------------------------------------------------------
//  Awaryjny punkt dostępowy na WĘŹLE BEZ SIECI (STACJA_HEADLESS=1).
//  Gdy 1, węzeł C3 po starcie czeka na poprawną ramkę od mastera; jeśli jej
//  nie ma (zły baud/adres/piny albo master wyłączony), uruchamia własny
//  punkt dostępowy z minimalną stroną konfiguracji RS485 (baud, adres, RX/TX/DE).
//  Normalna praca węzła pozostaje "chuda" - AP startuje tylko awaryjnie.
//  Ustawiane w platformio.ini (flagą -DSTACJA_NODE_RECOVERY_AP=1).
// -----------------------------------------------------------------------------
#ifndef STACJA_NODE_RECOVERY_AP
#define STACJA_NODE_RECOVERY_AP 0
#endif

// Flagi ustawiane przez stronę www i przez magistralę RS485, obsługiwane
// w main.cpp. Definicje są w config.cpp, bo w firmware węzła bez sieci nie ma
// pliku web_server.cpp, w którym były wcześniej.
extern volatile bool g_rebootRequested;
extern volatile bool g_factoryResetRequested;

// Wersja firmware razem z rolą - pokazuje, czy urządzenie ma wgraną wersję
// master (bin 1), czy węzła (bin 2): na stronie www, w Home Assistant
// i w zakładce "ESP #2" na drugim urządzeniu.
#define FW_VERSION_FULL FW_VERSION " (" FW_ROLE_NAME ")"

// Pliki na LittleFS: podstawowe miejsce ustawień to partycja NVS, pliki są
// kopią zapasową i źródłem danych przy migracji ze starszych wersji.
#define CONFIG_FILE "/config.json"
#define STATE_FILE  "/state.json"

// Alokator PSRAM dla ArduinoJson - duże bufory JSON (wykresy, listy plików)
// trafiają do 8 MB PSRAM, dzięki czemu UI działa płynnie.
struct PsramAllocator : ArduinoJson::Allocator {
  void* allocate(size_t size) override {
    if (psramFound()) return heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    return malloc(size);
  }
  void deallocate(void* ptr) override {
    if (!ptr) return;
    heap_caps_free(ptr);
  }
  void* reallocate(void* ptr, size_t newSize) override {
    if (!ptr) return allocate(newSize);
    if (psramFound()) return heap_caps_realloc(ptr, newSize, MALLOC_CAP_SPIRAM);
    return realloc(ptr, newSize);
  }
};

// Ustawienia pojedynczego kanału pomiarowego (kalibracja itp.)
struct ChannelConfig {
  bool enabled = true;
  float offset = 0.0f;
  String name;   // pusta = nazwa domyślna
  String zone;   // "in" (wewnątrz) / "out" (na zewnątrz); pusta = domyślna kanału
};

// Serwisy pogodowe, do których stacja może wysyłać pomiary jednocześnie
// (obok MQTT/Home Assistant). Kolejność jest używana też na stronie www.
#define SVC_COUNT 9
static const char* const SVC_NAMES[SVC_COUNT] = {
  "wu", "pws", "windy", "owm", "thingspeak", "custom", "awekas", "wcloud", "cwop"
};

// Nazwy wyświetlane na stronie i w diagnostyce (kolejność jak SVC_NAMES)
static const char* const SVC_LABELS[SVC_COUNT] = {
  "Weather Underground", "PWSWeather", "Windy", "OpenWeather", "ThingSpeak", "Własny adres",
  "AWEKAS", "WeatherCloud", "CWOP / APRS"
};

// Stan przetrwania restartu (licznik deszczu itp.)
struct StationState {
  unsigned long rainTipsTotal = 0;   // całkowita liczba impulsów deszczomierza
  unsigned long rainStartTips = 0;   // impulsy na początku bieżącej doby
  int rainDay = -1;                  // unikalny klucz doby (rok*1000 + dzień roku)
};

// Kolory stanów pogody dla pierścienia LED RGB (0xRRGGBB).
// Kolejność musi być zgodna z enum WeatherState w led_ring.h.
// Wartości domyślne można zmienić na stronie www (zakładka "Led RGB") -
// trafiają wtedy do NVS i przetrwają aktualizację firmware.
#define WEATHER_STATE_COUNT 10

static const uint32_t WEATHER_COLOR_DEFAULTS[WEATHER_STATE_COUNT] = {
  0x000000,  // Wyłączony
  0x00C853,  // Ładna pogoda      - zielony
  0xFFD400,  // Pogoda średnia    - żółty (zachmurzenie)
  0xFFB300,  // Silny wiatr       - bursztynowy
  0x00A2FF,  // Deszcz            - jasnoniebieski
  0x1020A0,  // Burza             - ciemnoniebieski
  0xEAF2FF,  // Śnieg / mróz      - biały (temperatura ujemna)
  0x9AA5B1,  // Mgła              - szary
  0xFF5A00,  // Upał              - pomarańczowy
  0x7C3AED   // Brak danych       - fioletowy
};

static const char* const WEATHER_STATE_NAMES[WEATHER_STATE_COUNT] = {
  "Wyłączony", "Ładna pogoda", "Pogoda średnia", "Silny wiatr", "Deszcz",
  "Burza", "Śnieg / mróz", "Mgła", "Upał", "Brak danych"
};

// Maksymalna długość własnej nazwy stanu pogody (pierścień LED).
#define RGB_NAME_MAX 28

class ConfigManager {
public:
  bool begin();
  bool save();
  void factoryReset();

  // Dopasowuje ustawienia magistrali RS485 do roli wgranej wersji firmware
  // (bin 1 = master, bin 2 = węzeł). Wołane raz, na starcie z begin().
  void applyBuildRole();

  String toJsonString();
  bool applyJson(const char* json, size_t len);
  bool applyChannelOverrides(const char* json, size_t len);

  String wifiSsid();
  String wifiPass();
  String apSsid();
  String apPass();
  String deviceName();
  String location();
  String mqttHost();
  uint16_t mqttPort();
  String mqttUser();
  String mqttPass();
  String mqttPrefix();
  String ntpServer();
  String tzString();
  uint32_t logIntervalS();
  String companyName();
  String companyUrl();

  // Retencja plików na karcie SD (0 = wyłączone, pliki trzymane bez limitu).
  // sd_retention_days  - po ilu dniach kasować logi CSV (/logs)
  // sd_retention_photos_days - po ilu dniach kasować zdjęcia kamery (/photos)
  uint16_t sdRetentionDays();
  uint16_t sdRetentionPhotosDays();

  // Sieć - stacja (STA)
  bool   wifiStatic();
  String wifiIp();
  String wifiGw();
  String wifiMask();
  String wifiDns();

  // Sieć - punkt dostępowy (AP)
  String apIp();
  bool   apHidden();
  bool   apAlwaysOn();

  // Urządzenie / aktualizacje
  String hostname();
  String otaPassword();

  // Pierścień LED RGB (WS2812B / RGBIC)
  bool    rgbEnabled();
  uint8_t rgbBrightness();
  bool    rgbEffects();
  bool    rgbOffAtNight();
  uint16_t rgbCount();
  uint8_t rgbOrder();
  uint16_t rgbMaxMa();
  uint32_t rgbColor(uint8_t state);              // kolor stanu pogody (z NVS)
  void     setRgbColor(uint8_t state, uint32_t rgb);
  void     resetRgbColors();                     // przywróć kolory domyślne
  String   rgbName(uint8_t state);               // nazwa stanu (własna lub domyślna)
  bool     rgbNameCustom(uint8_t state);         // true = nazwa wpisana przez użytkownika
  void     setRgbName(uint8_t state, const String& name);
  void     resetRgbNames();                      // przywróć nazwy domyślne

  ChannelConfig channelCfg(const String& id);
  void setChannelCfg(const String& id, const ChannelConfig& c);

  // Kamera (opcjonalna, ESP32-S3 z PSRAM; patrz STACJA_HAS_CAMERA).
  // Piny kamery są trzymane w sekcji "pins" (klucze cam_xclk, cam_pclk,
  // cam_vsync, cam_href, cam_d0..cam_d7, cam_sda, cam_scl, cam_pwdn, cam_reset)
  // i odczytywane przez istniejące pinOverride()/setPinOverride().
  bool     camEnabled();
  uint16_t camIntervalMin();   // co ile minut zdjęcie timelapse (0 = tylko ręcznie)
  uint8_t  camResolution();    // indeks framesize_t (0..13, domyślnie 8 = VGA 640x480)
  uint8_t  camQuality();       // jakość JPEG 0..63 (niżej = lepsza, domyślnie 12)
  uint8_t  camModel();         // 0 = wykryj sam, 1 = OV2640, 2 = OV3660, 3 = OV5640, 4 = GC0308
  uint16_t camMaxPhotos();     // limit liczby zdjęć na karcie (0 = bez limitu)
  uint16_t camMaxMb();         // limit łącznego rozmiaru zdjęć w MB (0 = bez limitu)
  String   camRemoteUrl();     // adres zewnętrznej kamery (ESP32-CAM), np. http://192.168.1.150/capture

  // Czujnik jakości powietrza wewnętrznego (SCD4x / SGP30)
  // 0 = wykryj automatycznie, 1 = SCD40/SCD41 (CO2), 2 = SGP30/SGP40 (eCO2+TVOC),
  // 3 = wyłączony
  uint8_t aqType();

  // Położenie geograficzne stacji (używane, gdy nie ma poprawnego fixu GPS)
  // oraz ustawienia odbiornika GPS.
  double   latitude();
  double   longitude();
  uint32_t gpsBaud();
  bool     gpsTimeSync();
  bool     gpsEnabled();

  // Zegar czasu rzeczywistego RTC (DS3231; obsługiwane też DS1307 i PCF8563).
  // Magistrala: 0 = wspólna z czujnikami (piny PIN_SDA/PIN_SCL lub z mapy pinów),
  //             1 = osobna (I2C2/Wire1) na pinach rtc_sda/rtc_scl.
  // Typ: 0 = wykryj automatycznie, 1 = DS3231, 2 = DS1307, 3 = PCF8563.
  bool    rtcEnabled();
  bool    rtcNtpSync();       // dopisywać czas z NTP/GPS do RTC
  uint8_t rtcBus();
  int     rtcSda();
  int     rtcScl();
  uint8_t rtcType();

  // --- Magistrala RS485: drugi ESP (węzeł z zewnętrznymi czujnikami) ---
  // Ta stacja może być MASTEREM (odpytuje węzeł i pokazuje jego dane na
  // pulpicie, pierścieniu LED, w CSV i w Home Assistant) albo WĘZŁEM
  // (sama odpowiada na pytania, nie łączy się z siecią Wi-Fi).
  bool     rs485Enabled();
  bool     rs485Master();
  uint8_t  rs485Addr();       // adres tej stacji na magistrali (1 = master)
  uint8_t  rs485Peer();       // adres drugiego ESP (gdy ta stacja jest masterem);
                              // 0 = wykryj adres węzła automatycznie
  // Lista adresów węzłów (np. "2,3,4") odpytywanych po kolei przez mastera.
  // Pusta lista = wykrywanie węzłów automatycznie (tak jak "wykryj sam").
  String   rs485Nodes();
  uint32_t rs485Baud();
  uint16_t rs485PollS();      // co ile sekund odpytywać węzeł
  uint16_t rs485TimeoutMs();  // czas oczekiwania na odpowiedź
  uint8_t  rs485Retries();    // liczba ponowień
  bool     rs485Log();        // zapisywać kanały węzła do CSV na karcie SD
  bool     rs485Mqtt();       // publikować kanały węzła w Home Assistant
  bool     rs485SlaveAp();    // węzeł udostępnia też punkt dostępowy (konfiguracja awaryjna)
  bool     rs485BaudManual(); // headless: true = prędkość ustawiona w awaryjnym AP (ma pierwszeństwo)

  // --- Kalibracja czujników analogowych (sekcja "extra") ---
  // Wartości trzymane są w sekcji "extra" ustawień, więc nie trzeba
  // rozszerzać struktury konfiguracji o kolejne pola.
  float extraF(const char* key, float def);
  void  setExtraF(const char* key, float value);
  void  removeExtra(const char* key);
  bool  hasExtra(const char* key);

  // Serwisy zewnętrzne (Weather Underground, PWSWeather, Windy, OpenWeather,
  // ThingSpeak, własny adres). Cała sekcja jest jednym obiektem JSON, żeby
  // strona www mogła zarządzać nią w jednym miejscu.
  String servicesJson();
  bool   applyServicesJson(const char* json, size_t len);
  uint16_t svcInterval();
  bool   svcExternalOnly();
  bool   svcSectionEnabled();
  bool   svcEnabled(const char* svc);
  String svcStr(const char* svc, const char* key);
  String svcTop(const char* key);            // services.<key> (np. temp_ch)

  StationState state;
  bool loadState();
  bool saveState();

  // Piny (sekcja "pins" w ustawieniach); zapisywane są tylko wartości inne niż domyślne.
  int  pinOverride(const char* key, int def);
  void setPinOverride(const char* key, int gpio);
  void removePinOverride(const char* key);

private:
  JsonDocument doc_;
  SemaphoreHandle_t mutex_ = nullptr;

  void lock();
  void unlock();
  void ensureDefaults();
  String getString(const char* key);
};

extern ConfigManager config;
