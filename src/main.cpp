/* =============================================================================
 * Stacja Pogody - stacja meteorologiczna na ESP32-S3 WROOM-1 (N16R8)
 * Copyright (c) 2026 kruzio1985 - https://github.com/kruzio1985
 * Licencja: Stacja Pogody Non-Commercial License 1.0 (plik LICENSE)
 * Użytek niekomercyjny. Kontakt: kruzio1985@users.noreply.github.com
 * =============================================================================
 */
#include <Arduino.h>
#include <LittleFS.h>
#include <esp_heap_caps.h>
#include <time.h>

#include "pins.h"
#include "config.h"
#include "board.h"
#include "pinmap.h"
#include "sensors.h"
#include "sd_card.h"
#include "camera.h"
#include "logger.h"
#include "analysis.h"
#include "alerts.h"
#include "openmeteo.h"
#include "syslog.h"
#include "led_ring.h"
#include "flashfix.h"
#include "gps.h"
#include "astro.h"
#include "forecast.h"
#include "lightning.h"
#include "drv_rtc.h"
#include "rs485.h"
#include "recovery_ap.h"
#include "retention.h"
#if !STACJA_HEADLESS
// Firmware węzła bez sieci (STACJA_HEADLESS) nie zawiera Wi-Fi, serwera www,
// MQTT, serwisów pogodowych ani OTA - dane z jego czujników czyta master po
// RS485, a nowy firmware wgrywa się kablem USB.
#include <WiFi.h>
#include "mqtt_client.h"
#include "weather_services.h"
#include "web_server.h"
#include "ota.h"
#endif

#if !STACJA_HEADLESS
static WiFiClient wifiClient;
#endif
static unsigned long lastSensorRead = 0;
static unsigned long lastLog = 0;
static unsigned long lastLed = 0;
static unsigned long lastAstro = 0;
static unsigned long lastForecast = 0;

// Piny pobrane raz, po wczytaniu mapy pinów (patrz pinmap.cpp)
static int ledPin = -1;
static int buttonPin = -1;

// Tryb węzła RS485 (moduł na magistrali). Węzeł pracuje wyłącznie po przewodzie:
// nie łączy się z routerem, nie wysyła nic do MQTT ani do serwisów
// pogodowych - jego czujniki czyta master. Ustawia się to w zakładce
// "ESP i magistrala RS485" (rs485_enabled + rola "węzeł").
static bool asNode = false;

#if !STACJA_HEADLESS
static wl_status_t lastWifiStatus = WL_IDLE_STATUS;
static IPAddress apAddress(192, 168, 4, 1);
static unsigned long staConnectedAt = 0;
static bool apTurnedOff = false;

// Zwraca true i zapisuje wynik, gdy tekst jest poprawnym adresem IPv4
static bool parseIp(const String& s, IPAddress& out) {
  String t = s;
  t.trim();
  if (t.length() == 0) return false;
  return out.fromString(t);
}

void setupWiFi() {
  // Węzeł RS485 nie łączy się z routerem, a własny punkt dostępowy włącza
  // tylko wtedy, gdy tak ustawiono (wygodne przy pierwszej konfiguracji
  // węzła i przy podglądzie jego strony). Master działa jak dotychczas.
  if (asNode && !config.rs485SlaveAp()) {
    WiFi.mode(WIFI_OFF);
    LOG_I("Węzeł RS485: Wi-Fi wyłączone (sterowanie tylko przewodem RS485)");
    return;
  }

  WiFi.mode(asNode ? WIFI_AP : WIFI_AP_STA);
  WiFi.setSleep(false);   // stabilniejszy MQTT i szybszy interfejs www

  String host = config.hostname();
  host.trim();
  if (host.length() == 0) host = "stacja-pogody";
  WiFi.setHostname(host.c_str());

  // --- Punkt dostępowy (AP) - konfiguracja PRZED softAP ---
  if (!parseIp(config.apIp(), apAddress)) apAddress = IPAddress(192, 168, 4, 1);
  WiFi.softAPConfig(apAddress, apAddress, IPAddress(255, 255, 255, 0));

  String apSsid = config.apSsid();
  if (apSsid.length() == 0) apSsid = "StacjaPogody";
  String apPass = config.apPass();
  if (apPass.length() < 8) apPass = "12345678";
  WiFi.softAP(apSsid.c_str(), apPass.c_str(), 1, config.apHidden());

  if (asNode) {
    LOG_I("Węzeł RS485: punkt dostępowy %s (IP %s) - tylko do konfiguracji",
          apSsid.c_str(), apAddress.toString().c_str());
    return;
  }

  // --- Połączenie z routerem (STA) ---
  String ssid = config.wifiSsid();
  if (ssid.length() == 0) return;

  if (config.wifiStatic()) {
    IPAddress ip;
    if (parseIp(config.wifiIp(), ip)) {
      IPAddress gw, mask, dns;
      if (!parseIp(config.wifiGw(), gw))
        gw = IPAddress(ip[0], ip[1], ip[2], 1);
      if (!parseIp(config.wifiMask(), mask))
        mask = IPAddress(255, 255, 255, 0);
      if (parseIp(config.wifiDns(), dns)) WiFi.config(ip, gw, mask, dns);
      else                               WiFi.config(ip, gw, mask);
      LOG_I("Statyczny adres IP: %s, brama %s", ip.toString().c_str(), gw.toString().c_str());
    } else {
      LOG_W("Błędny statyczny adres IP '%s' - używam DHCP", config.wifiIp().c_str());
    }
  }

  WiFi.begin(ssid.c_str(), config.wifiPass().c_str());
}

// Wyłącza AP po ustalonym połączeniu z routerem, jeśli tak ustawiono.
// Dzięki opóźnieniu można jeszcze wrócić na 192.168.4.1 i cofnąć zmianę.
void manageAp(unsigned long now) {
  if (asNode) return;        // węzeł nie łączy się z routerem - AP zostaje włączony
  if (apTurnedOff) return;
  if (WiFi.status() != WL_CONNECTED) {
    staConnectedAt = 0;
    return;
  }
  if (config.apAlwaysOn()) return;

  if (staConnectedAt == 0) staConnectedAt = now ? now : 1;
  if (now - staConnectedAt > 60000UL) {
    WiFi.softAPdisconnect(true);
    apTurnedOff = true;
    LOG_I("Punkt dostępowy wyłączony (stacja połączona z routerem)");
  }
}

// Logowanie zmian stanu Wi-Fi (raz na zdarzenie, nie w każdej pętli)
void logWifiChanges() {
  wl_status_t st = WiFi.status();
  if (st == lastWifiStatus) return;
  lastWifiStatus = st;

  switch (st) {
    case WL_CONNECTED:
      LOG_I("Połączono z Wi-Fi '%s', IP %s, RSSI %d dBm",
            config.wifiSsid().c_str(), WiFi.localIP().toString().c_str(), WiFi.RSSI());
      break;
    case WL_NO_SSID_AVAIL:
      LOG_W("Nie znaleziono sieci '%s'", config.wifiSsid().c_str());
      break;
    case WL_CONNECT_FAILED:
      LOG_E("Błąd łączenia z siecią '%s'", config.wifiSsid().c_str());
      break;
    case WL_DISCONNECTED:
      LOG_W("Rozłączono z siecią '%s' - ponawiam", config.wifiSsid().c_str());
      break;
    default:
      break;
  }
}
#endif  // !STACJA_HEADLESS

void setupTime() {
  // Strefa czasowa i serwer NTP - wspólne z ręczną synchronizacją
  // w zakładce "Zegar RTC" (patrz drv_rtc.cpp).
#if STACJA_HEADLESS
  // Węzeł bez sieci nie ma jak zapytać o czas przez internet - dokładny czas
  // przysyła master poleceniem "time" po RS485. Tutaj ustawiamy tylko strefę
  // czasową, żeby znaczniki czasu w logach były lokalne.
  setenv("TZ", config.tzString().c_str(), 1);
  tzset();
  LOG_I("Zegar: strefa %s, dokładny czas przyśle master po RS485", config.tzString().c_str());
#else
  rtcStartNtp();
#endif
}

#if !STACJA_HEADLESS
// Suma kontrolna FNV-1a pliku z LittleFS - pozwala sprawdzić, czy pliki www
// wgrały się poprawnie, bez łączenia się z siecią stacji.
static uint32_t fileFnv1a(const char* path, size_t& size) {
  size = 0;
  File f = LittleFS.open(path, "r");
  if (!f) return 0;

  uint32_t hash = 2166136261u;
  uint8_t buf[512];
  for (;;) {
    size_t n = f.read(buf, sizeof(buf));
    if (n == 0) break;
    for (size_t i = 0; i < n; i++) {
      hash ^= buf[i];
      hash *= 16777619u;
    }
    size += n;
  }
  f.close();
  return hash;
}
#endif

void setup() {
  Serial.begin(115200);

  // Natywne USB-CDC "pojawia się" dopiero po wyliczeniu przez hosta,
  // więc chwilę czekamy - inaczej banner startowy przepada. Nigdy nie
  // blokujemy na stałe: bez podłączonego PC stacja ma wystartować sama.
  for (uint8_t i = 0; i < 30 && !Serial; i++) delay(50);

  // Obejście błędu odczytu flash musi być gotowe przed pierwszym użyciem
  // LittleFS/NVS (patrz flashfix.cpp).
  if (installFlashReadFix()) {
    LOG_I("Obejście odczytu flash aktywne (porcje %u B)", (unsigned)flashReadFixChunk());
  } else {
    LOG_E("Nie udało się włączyć obejścia odczytu flash - LittleFS może nie wystartować");
  }

  // Bufor logów jako pierwszy - żeby uchwycić komunikaty startowe
  syslog.begin();

  // System plików
  if (!LittleFS.begin(true)) {
    LOG_E("LittleFS nie wystartował - strona www i ustawienia niedostępne");
  } else {
    LOG_I("LittleFS: zajęte %u z %u kB", (unsigned)(LittleFS.usedBytes() / 1024),
          (unsigned)(LittleFS.totalBytes() / 1024));

#if !STACJA_HEADLESS
    if (LittleFS.exists("/index.html")) {
      size_t wwwSize = 0;
      uint32_t wwwHash = fileFnv1a("/index.html", wwwSize);
      LOG_I("Strona www: index.html %u B, suma kontrolna %08X", (unsigned)wwwSize, (unsigned)wwwHash);
    } else {
      LOG_W("Strona www: brak pliku /index.html - wgraj ją poleceniem 'pio run -t uploadfs'");
    }
#else
    // Węzeł bez sieci nie ma strony www - system plików trzyma tylko
    // ustawienia (kopię /config.json).
    LOG_I("Węzeł bez sieci: bez strony www, LittleFS tylko na ustawienia");
#endif
  }

  config.begin();
  pinMap.begin();     // przypisania pinów z ustawień (musi być przed sensors/sdCard)

#if defined(CONFIG_IDF_TARGET_ESP32S3) && !STACJA_HEADLESS
  // Wygaszenie wbudowanej diody RGB WS2812. Gdy linia danych diody "wisi
  // w powietrzu", potrafi ona świecić na biało. Na płytkach ESP32-S3 bywa na
  // GPIO 48 (oryginał DevKitC-1) albo na klonach na 21/47 - trzymamy te
  // wolne piny nisko, żeby dioda była ciemna. GPIO 38/39 pomijamy (RS485).
  // Uwaga: 47/48 to też domyślne piny OSOBNEJ magistrali RTC (I2C2) - do
  // osobnego RTC wybierz w www inne piny, bo te zostają ustawione na LOW.
  static const int onboardLedPins[] = {48, 21, 47};
  for (int p : onboardLedPins) {
    pinMode(p, OUTPUT);
    digitalWrite(p, LOW);
  }
  LOG_I("Wbudowana dioda RGB: piny 48/21/47 trzymane nisko");
#endif

  // Rola tego urządzenia na magistrali RS485. Ustalamy ją PRZED startem
  // sieci, bo od niej zależy, czy stacja łączy się z routerem i MQTT.
#if STACJA_HEADLESS
  // Firmware węzła bez sieci jest zawsze węzłem - nie zawiera strony www,
  // Wi-Fi ani MQTT, więc nie ma jak być masterem. Adres bierzemy z ustawień
  // (domyślnie 2), a prędkość jest stała w buildzie (STACJA_NODE_BAUD = 19200),
  // żeby węzeł działał od razu po wgraniu bin, bez konfiguracji magistralą.
  asNode = true;
  LOG_I("Tryb węzła RS485 (bez sieci): adres %u, %u b/s - czujniki tego ESP czyta master",
        (unsigned)config.rs485Addr(), (unsigned)STACJA_NODE_BAUD);
#else
  asNode = config.rs485Enabled() && !config.rs485Master();
  if (asNode) {
    LOG_I("Tryb węzła RS485: adres %u, %u b/s - czujniki tego ESP czyta master",
          (unsigned)config.rs485Addr(), (unsigned)config.rs485Baud());
  }
#endif

  // LED i przycisk - dopiero po wczytaniu mapy pinów
  ledPin = pinMap.pin("led");
  buttonPin = pinMap.pin("button");
  if (ledPin >= 0) pinMode(ledPin, OUTPUT);
  if (buttonPin >= 0) pinMode(buttonPin, INPUT_PULLUP);

#if !STACJA_HEADLESS
  setupWiFi();
#endif
  setupTime();

  sensors.begin();
  rtc.begin();     // zegar RTC na I2C - po sensors.begin() (wspólna magistrala)
  sdCard.begin();
  logger.begin();
  camera.begin();   // kamera (opcjonalna) - po karcie SD (zapis zdjęć)
  analysis.begin();
#if !STACJA_HEADLESS
  if (!asNode) {
    // Sieciowe usługi tylko na masterze - węzeł nie ma połączenia z internetem
    mqtt.begin(wifiClient);
    weatherServices.begin();
    alerts.begin();
    openMeteo.begin();
  }
#endif
  gps.begin();
  forecast.begin();
  lightning.begin();
#if !STACJA_HEADLESS
  web.begin(apAddress);
  ota.begin();
#endif

  // Pierścień LED RGB - po pinMap i config (jasność/stan z ustawień)
  ledRing.begin();

  // Magistrala RS485 - po pinMap (piny RX/TX/DE) i config (rola, prędkość)
  rs485.begin();

#if STACJA_HEADLESS
  // Awaryjny punkt dostępowy węzła bez sieci - uruchomi się tylko wtedy,
  // gdy węzeł nie zobaczy poprawnej ramki od mastera (patrz recovery_ap.cpp).
  recoveryAp.begin();
#endif

#if !STACJA_HEADLESS
  LOG_I("Punkt dostępu: %s (IP %s)%s", config.apSsid().c_str(),
        apAddress.toString().c_str(), config.apHidden() ? " [ukryty]" : "");
#endif
  LOG_I("PSRAM: %u kB (wolne %u kB)", (unsigned)(ESP.getPsramSize() / 1024),
        (unsigned)(ESP.getFreePsram() / 1024));
  LOG_I("RAM: wolne %u kB, największy blok %u kB, minimalny zapas %u kB",
        (unsigned)(ESP.getFreeHeap() / 1024),
        (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024),
        (unsigned)(ESP.getMinFreeHeap() / 1024));
  LOG_I("Płytka: %s - %s, flash %u MB, PSRAM %u MB", boards::current().label,
        boards::chipName(),
        (unsigned)(ESP.getFlashChipSize() / (1024UL * 1024UL)),
        (unsigned)(ESP.getPsramSize() / (1024UL * 1024UL)));
#if !STACJA_HEADLESS
  LOG_I("Gotowe - strona www: http://%s/", apAddress.toString().c_str());
#else
  LOG_I("Gotowe - węzeł RS485 adres %u: czekam na pytania mastera", (unsigned)config.rs485Addr());
#endif

  // Pierwszy odczyt od razu
  sensors.readAll();
  forecast.loop();          // pierwsza próbka historii (ciśnienie/temperatura)
  forecast.update(time(nullptr), AstroService::localTzOffsetMin());
  astro.update(config.latitude(), config.longitude(), time(nullptr),
               AstroService::localTzOffsetMin());
#if !STACJA_HEADLESS
  if (!asNode) mqtt.publishReadings(sensors.snapshot());
  lastWifiStatus = WiFi.status();
#endif
}

void loop() {
#if !STACJA_HEADLESS
  web.loop();
  if (!asNode) mqtt.loop();
  ota.loop();
#endif

  unsigned long now = millis();
#if !STACJA_HEADLESS
  manageAp(now);
  if (!asNode) logWifiChanges();
#endif

  // W trakcie aktualizacji nie dotykamy czujników ani karty SD -
  // zapis do flasha musi być jedyną operacją na pamięci.
#if !STACJA_HEADLESS
  if (g_otaInProgress) {
    delay(10);
    return;
  }
#endif

  sensors.serviceDiscovery();
  gps.loop();
  lightning.loop();
  rtc.loop();

  // Odczyt czujników co 5 s
  if (now - lastSensorRead >= 5000) {
    lastSensorRead = now;
    sensors.readAll();
#if !STACJA_HEADLESS
    if (!asNode) mqtt.publishReadings(sensors.snapshot());
#endif
  }

  // Astronomia (Słońce/Księżyc) co 60 s - pozycja z GPS, a bez fixa z ustawień
  if (now - lastAstro >= 60000UL) {
    lastAstro = now;
    astro.update(config.latitude(), config.longitude(), time(nullptr),
                 AstroService::localTzOffsetMin());
  }

  // Prognoza lokalna: próbka historii (co 5 min) + przeliczenie co 30 s
  forecast.loop();
  if (now - lastForecast >= 30000UL) {
    lastForecast = now;
    forecast.update(time(nullptr), AstroService::localTzOffsetMin());
  }

  // Dosiewanie rekordów z CSV (jeden plik na obieg pętli, aż do skutku)
#if !STACJA_HEADLESS
  if (!asNode) {
    analysis.loop();
    alerts.loop();
    retention.loop();
  }
#endif

  // Zapis logu na SD co log_interval_s
  if (now - lastLog >= (unsigned long)config.logIntervalS() * 1000UL) {
    lastLog = now;
    std::vector<Channel> snap = sensors.snapshot();
    logger.logNow(snap);
#if !STACJA_HEADLESS
    if (!asNode) analysis.noteSamples(snap);
#endif
  }

  // Miganie LED (1 Hz gdy działa)
  if (ledPin >= 0 && now - lastLed >= 500) {
    lastLed = now;
    digitalWrite(ledPin, !digitalRead(ledPin));
  }

  // Magistrala RS485: master odpytuje węzeł, węzeł odpowiada.
  // Przed pierścieniem LED - żeby kolory pogody brały też dane z drugiego ESP.
  rs485.loop();

#if STACJA_HEADLESS
  recoveryAp.loop();
#endif

  // Pierścień LED RGB: kolor stanu pogody + animacja
  ledRing.loop();
  camera.loop();    // timelapse: zdjęcie co cam_interval_min (gdy kamera włączona)
  // Długie przytrzymanie przycisku BOOT (3 s) = przywrócenie fabrycznych
  if (buttonPin >= 0 && digitalRead(buttonPin) == LOW) {
    unsigned long press = millis();
    while (digitalRead(buttonPin) == LOW && millis() - press < 3000) delay(10);
    if (millis() - press >= 3000) {
      LOG_W("Przycisk BOOT przytrzymany 3 s - reset do ustawień fabrycznych");
      g_factoryResetRequested = true;
    }
  }

  if (g_factoryResetRequested) {
    g_factoryResetRequested = false;
    config.factoryReset();
    delay(200);
    ESP.restart();
  }

  if (g_rebootRequested) {
    g_rebootRequested = false;
    LOG_I("Restart na żądanie ze strony www");
    delay(200);
    ESP.restart();
  }

  delay(10);
}
