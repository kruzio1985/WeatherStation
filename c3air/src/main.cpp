/* =============================================================================
 * Stacja Pogody - węzeł jakości powietrza / pogody na ESP32-C3 SuperMini
 * Copyright (c) 2026 kruzio1985 - https://github.com/kruzio1985
 * Licencja: PolyForm Noncommercial License 1.0.0 (plik LICENSE)
 * Użytek niekomercyjny. Kontakt: kruzio1985@users.noreply.github.com
 * =============================================================================
 * Samodzielny moduł na ESP32-C3:
 *   - AHT20  (I2C 0x38)      temperatura + wilgotność
 *   - BMP280 (I2C 0x76/0x77) ciśnienie
 *   - PMS7003 (UART0 9600)   PM1.0 / PM2.5 / PM10
 *
 * Wi-Fi STA ze statycznym IP 192.168.1.147 (konfigurowalny) + AP awaryjny.
 * Panel www: pulpit, kalibracja (offset/współczynniki), ustawienia
 * (Wi-Fi + MQTT), OTA, logi. MQTT -> Home Assistant z auto-discovery oraz
 * /api/status w formacie importowanym przez stację pogody (typ "aq").
 * ========================================================================== */
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <ArduinoOTA.h>
#include <Update.h>
#include <esp_task_wdt.h>
#include <esp_system.h>
#include <esp_ota_ops.h>
#include <math.h>
#include <stdarg.h>
#include <time.h>

// ============================ Piny ============================
#define PIN_SDA     8
#define PIN_SCL     9
#define PIN_PMS_RX  20
#define PIN_PMS_TX  21

// ============================ Konfiguracja (NVS) ============================
Preferences P;
String  C_name;
String  C_ssid, C_pass, C_apPass;
String  C_ip, C_gw, C_mask;
bool    C_static;
String  C_mqttHost, C_mqttUser, C_mqttPass, C_mqttPrefix;
int     C_mqttPort;
// Kalibracja (offset sumowany, współczynnik mnożony)
float   K_tempOff, K_humOff, K_pressOff;
float   K_pm1F, K_pm25F, K_pm10F;
// Położenie geograficzne (ręcznie albo z mastera) + ciśnienie odniesienia
double  C_lat = 0, C_lon = 0;
float   C_p0 = 1013.25f;     // ciśnienie na poziomie morza do wysokości
String  C_masterUrl;         // opcjonalnie: adres mastera do pobrania GPS

void loadCfg() {
  P.begin("stacja-c3", false);
  C_name      = P.getString("name", "");
  C_ssid      = P.getString("ssid", "");
  C_pass      = P.getString("pass", "");
  C_apPass    = P.getString("ap", "12345678");
  C_ip        = P.getString("ip", "192.168.1.147");
  C_gw        = P.getString("gw", "192.168.1.1");
  C_mask      = P.getString("mask", "255.255.255.0");
  C_static    = P.getBool("static", true);
  C_mqttHost  = P.getString("mqh", "");
  C_mqttPort  = P.getInt("mqp", 1883);
  C_mqttUser  = P.getString("mqu", "");
  C_mqttPass  = P.getString("mqw", "");
  C_mqttPrefix= P.getString("mqx", "stacja_c3");
  K_tempOff   = P.getFloat("kt", 0.0f);
  K_humOff    = P.getFloat("kh", 0.0f);
  K_pressOff  = P.getFloat("kp", 0.0f);
  K_pm1F      = P.getFloat("kpm1", 1.0f);
  K_pm25F     = P.getFloat("kpm25", 1.0f);
  K_pm10F     = P.getFloat("kpm10", 1.0f);
  C_lat       = P.getDouble("lat", 0.0);
  C_lon       = P.getDouble("lon", 0.0);
  C_p0        = P.getFloat("p0", 1013.25f);
  C_masterUrl = P.getString("murl", "");
  P.end();
  if (C_mqttPort < 1 || C_mqttPort > 65535) C_mqttPort = 1883;
  if (C_mqttPrefix.length() == 0) C_mqttPrefix = "stacja_c3";
  if (!isfinite(K_pm1F) || K_pm1F <= 0) K_pm1F = 1.0f;
  if (!isfinite(K_pm25F) || K_pm25F <= 0) K_pm25F = 1.0f;
  if (!isfinite(K_pm10F) || K_pm10F <= 0) K_pm10F = 1.0f;
}

void saveCfg() {
  P.begin("stacja-c3", false);
  P.putString("name", C_name);
  P.putString("ssid", C_ssid); P.putString("pass", C_pass); P.putString("ap", C_apPass);
  P.putString("ip", C_ip); P.putString("gw", C_gw); P.putString("mask", C_mask);
  P.putBool("static", C_static);
  P.putString("mqh", C_mqttHost); P.putInt("mqp", C_mqttPort);
  P.putString("mqu", C_mqttUser); P.putString("mqw", C_mqttPass); P.putString("mqx", C_mqttPrefix);
  P.putFloat("kt", K_tempOff); P.putFloat("kh", K_humOff); P.putFloat("kp", K_pressOff);
  P.putFloat("kpm1", K_pm1F); P.putFloat("kpm25", K_pm25F); P.putFloat("kpm10", K_pm10F);
  P.putDouble("lat", C_lat); P.putDouble("lon", C_lon);
  P.putFloat("p0", C_p0); P.putString("murl", C_masterUrl);
  P.end();
}

// ============================ Licznik uruchomień ============================
static uint32_t bootCount() {
  Preferences P;
  P.begin("stacja-c3", false);
  uint32_t n = P.getUInt("boots", 0) + 1;
  P.putUInt("boots", n);
  P.end();
  return n;
}

// ============================ Log ============================
// RAM: pierścień 64 wpisów (podgląd na żywo). Trwały: plik /logs.txt na
// LittleFS, spłukiwany okresowo (co 5 min) i przy starcie - dzięki temu po
// awarii/resetcie widać, co się działo. Stary wpis kasowany, gdy plik
// przekroczy ~12 KB (kilka dni błędów), żeby nie zajmować pamięci.
#define LOG_N 64
static char g_log[LOG_N][160];
static int  g_logN = 0;
static void logE(const char* fmt, ...) {
  va_list ap; va_start(ap, fmt);
  char tmp[160]; vsnprintf(tmp, sizeof(tmp), fmt, ap); va_end(ap);
  snprintf(g_log[g_logN % LOG_N], 160, "[%lus] %s", (unsigned long)(millis() / 1000), tmp);
  g_logN++;
  Serial.println(g_log[(g_logN - 1) % LOG_N]);
}

// --- Trwały log (LittleFS) ---
static const char* LOG_FILE = "/logs.txt";
static int  g_logFlushed = 0;   // ile wpisów RAM już trafiło do FS
static bool g_fsOk = false;

static void fsLogAppend(const String& line) {
  File f = LittleFS.open(LOG_FILE, "a");
  if (!f) return;
  f.print(line); f.print("\n");
  f.close();
}

static void fsLogTrim() {
  File f = LittleFS.open(LOG_FILE, "r");
  if (!f) return;
  size_t sz = f.size();
  if (sz <= 12288) { f.close(); return; }
  String all = f.readString();
  f.close();
  if (all.length() <= 12288) return;
  // Zachowaj tylko drugą (najnowszą) połowę, od początku najbliższej linii.
  int keepFrom = all.length() / 2;
  while (keepFrom > 0 && all[keepFrom - 1] != '\n') keepFrom--;
  String keep = all.substring(keepFrom);
  LittleFS.remove(LOG_FILE);
  File w = LittleFS.open(LOG_FILE, "w");
  if (w) { w.print(keep); w.close(); }
}

static void flushLog() {
  if (!g_fsOk) return;
  for (int i = g_logFlushed; i < g_logN; i++) fsLogAppend(String(g_log[i % LOG_N]));
  g_logFlushed = g_logN;
  fsLogTrim();
}

static String fsLogRead() {
  if (!g_fsOk) return "";
  File f = LittleFS.open(LOG_FILE, "r");
  if (!f) return "";
  String s = f.readString();
  f.close();
  return s;
}

// ============================ Stan czujników ============================
static float s_pm1 = NAN, s_pm25 = NAN, s_pm10 = NAN;
static float s_temp = NAN, s_hum = NAN, s_press = NAN;
static unsigned long s_pmsLastMs = 0;
static uint32_t g_boots = 0;

// --- Parametry obliczane z pomiarów ---
static float s_dew = NAN;      // punkt rosy °C
static float s_absh = NAN;     // wilgotność bezwzględna g/m³
static float s_heat = NAN;     // odczuwalna (heat index) °C
static float s_alt = NAN;      // wysokość n.p.m. m
static int   s_trend = 0;      // trend ciśnienia: -1 spada, 0 stabilny, +1 rośnie
static float s_trend3h = NAN;  // zmiana ciśnienia w 3 h [hPa]
static float s_aqi = NAN;      // EPA AQI (max PM2.5 / PM10)
static int   s_aqiCat = 0;     // 0..5
static float s_pm25ratio = NAN;// PM2.5 / PM10
static float s_pm25_1m = NAN, s_pm25_15m = NAN, s_pm25_60m = NAN;

// Historia ciśnienia (trend 3 h, próbka co 5 min) i PM2.5 (średnie kroczące).
static const int  PRESS_N = 36;          // 36 * 5 min = 3 h
static float      g_press[PRESS_N];
static uint32_t   g_pressT[PRESS_N];
static int        g_pressN = 0;
static const int  PM_N = 128;            // próbka co ~3 s => ~6 min historii
static float      g_pm25[PM_N];
static uint32_t   g_pm25T[PM_N];
static int        g_pm25N = 0;

// ============================ Parametry obliczane ============================
static void computeDerived() {
  // Punkt rosy (Magnus)
  if (!isnan(s_temp) && !isnan(s_hum)) {
    float a = 17.625f, b = 243.04f;
    float g = logf(s_hum / 100.0f) + a * s_temp / (b + s_temp);
    s_dew = b * g / (a - g);
    // Wilgotność bezwzględna [g/m³]
    float svp = 6.112f * expf((17.67f * s_temp) / (s_temp + 243.5f));
    s_absh = svp * s_hum * 2.1674f / (273.15f + s_temp);
  } else {
    s_dew = NAN; s_absh = NAN;
  }
  // Heat index (tylko przy T >= 27 °C; poniżej = temperatura)
  if (!isnan(s_temp) && !isnan(s_hum) && s_temp >= 27.0f) {
    float T = s_temp, RH = s_hum;
    float T2 = T * T, RH2 = RH * RH;
    s_heat = -8.784695f + 1.61139411f * T + 2.338549f * RH - 0.14611605f * T * RH
           - 1.2308094e-2f * T2 - 1.6424828e-2f * RH2 + 2.211732e-3f * T2 * RH
           + 7.2546e-4f * T * RH2 - 3.582e-6f * T2 * RH2;
  } else {
    s_heat = NAN;
  }
  // Wysokość n.p.m. z ciśnienia (wzór barometryczny, p0 = ciśnienie odniesienia)
  if (!isnan(s_press)) {
    float ratio = s_press / C_p0;
    s_alt = 44330.0f * (1.0f - powf(ratio, 1.0f / 5.255f));
  } else s_alt = NAN;

  // Trend ciśnienia: zmiana w ostatnich 3 h
  s_trend3h = NAN;
  if (g_pressN > 0) {
    uint32_t now = millis();
    float oldest = NAN;
    for (int i = 0; i < g_pressN; i++) {
      int idx = (g_pressN - 1 - i + PRESS_N) % PRESS_N;
      if (now - g_pressT[idx] >= 3UL * 3600UL * 1000UL) { oldest = g_press[idx]; break; }
      if (i == g_pressN - 1) oldest = g_press[idx];   // za mało czasu - najstarsza próbka
    }
    if (!isnan(oldest) && !isnan(s_press)) {
      s_trend3h = s_press - oldest;
      if (s_trend3h > 1.0f) s_trend = 1;
      else if (s_trend3h < -1.0f) s_trend = -1;
      else s_trend = 0;
    }
  }

  // Stosunek PM2.5 / PM10
  if (!isnan(s_pm25) && !isnan(s_pm10) && s_pm10 > 0) s_pm25ratio = s_pm25 / s_pm10;
  else s_pm25ratio = NAN;

  // EPA AQI (maksimum z PM2.5 i PM10, 24 h breakpointy)
  s_aqi = NAN; s_aqiCat = 0;
  if (!isnan(s_pm25) || !isnan(s_pm10)) {
    auto aqiPm25 = [](float c) {
      const float bp[7] = {0, 12.0f, 35.4f, 55.4f, 150.4f, 250.4f, 350.4f};
      const float ah[7] = {0, 50, 100, 150, 200, 300, 400};
      for (int i = 1; i < 7; i++) if (c <= bp[i]) {
        if (c <= bp[i-1]) return ah[i-1];
        return ah[i-1] + (ah[i]-ah[i-1]) * (c - bp[i-1]) / (bp[i]-bp[i-1]);
      }
      return 500.0f;
    };
    auto aqiPm10 = [](float c) {
      const float bp[7] = {0, 54, 154, 254, 354, 424, 504};
      const float ah[7] = {0, 50, 100, 150, 200, 300, 400};
      for (int i = 1; i < 7; i++) if (c <= bp[i]) {
        if (c <= bp[i-1]) return ah[i-1];
        return ah[i-1] + (ah[i]-ah[i-1]) * (c - bp[i-1]) / (bp[i]-bp[i-1]);
      }
      return 500.0f;
    };
    float a = NAN;
    if (!isnan(s_pm25)) a = aqiPm25(s_pm25);
    if (!isnan(s_pm10)) { float b = aqiPm10(s_pm10); if (isnan(a) || b > a) a = b; }
    if (!isnan(a)) {
      s_aqi = a;
      if (a <= 50) s_aqiCat = 0; else if (a <= 100) s_aqiCat = 1;
      else if (a <= 150) s_aqiCat = 2; else if (a <= 200) s_aqiCat = 3;
      else if (a <= 300) s_aqiCat = 4; else s_aqiCat = 5;
    }
  }
}

// Kategoria AQI -> nazwa i kolor (EPA).
static const char* aqiCatName(int c) {
  switch (c) {
    case 0: return "Dobra"; case 1: return "Umiarkowana";
    case 2: return "Dostateczna (wrażliwe)"; case 3: return "Zła";
    case 4: return "Bardzo zła"; default: return "Niebezpieczna";
  }
}
static const char* aqiCatColor(int c) {
  switch (c) {
    case 0: return "#00e400"; case 1: return "#ffff00"; case 2: return "#ff7e00";
    case 3: return "#ff0000"; case 4: return "#8f3f97"; default: return "#7e0023";
  }
}

static bool ahtOk = false;

// ============================ BMP280 (I2C 0x76/0x77) ============================
// Surowy sterownik według wzorów kompensacji z noty Bosch (BMP280).
// Adafruit_BME280 w tej wersji obsługuje tylko BME280 (ID 0x60), a na płytce
// siedzi BMP280 (ID 0x58) - stąd własny, niezależny od biblioteki odczyt.
static uint8_t  bmpAddr = 0;
static uint16_t bmp_T1; static int16_t bmp_T2, bmp_T3;
static uint16_t bmp_P1; static int16_t bmp_P2, bmp_P3, bmp_P4, bmp_P5, bmp_P6, bmp_P7, bmp_P8, bmp_P9;
static int32_t  bmp_tfine = 0;

static uint8_t bmpReg8(uint8_t reg) {
  Wire.beginTransmission(bmpAddr); Wire.write(reg); Wire.endTransmission();
  Wire.requestFrom(bmpAddr, 1);
  return Wire.available() ? (uint8_t)Wire.read() : 0;
}
static uint16_t bmpReg16(uint8_t reg) {
  Wire.beginTransmission(bmpAddr); Wire.write(reg); Wire.endTransmission();
  Wire.requestFrom(bmpAddr, 2);
  if (Wire.available() < 2) return 0;
  return (uint16_t)Wire.read() | ((uint16_t)Wire.read() << 8);
}
static bool bmpBegin(uint8_t addr) {
  bmpAddr = addr;
  uint8_t id = bmpReg8(0xD0);
  if (id != 0x58) return false;   // BMP280
  bmp_T1 = bmpReg16(0x88); bmp_T2 = (int16_t)bmpReg16(0x8A); bmp_T3 = (int16_t)bmpReg16(0x8C);
  bmp_P1 = bmpReg16(0x8E); bmp_P2 = (int16_t)bmpReg16(0x90); bmp_P3 = (int16_t)bmpReg16(0x92);
  bmp_P4 = (int16_t)bmpReg16(0x94); bmp_P5 = (int16_t)bmpReg16(0x96); bmp_P6 = (int16_t)bmpReg16(0x98);
  bmp_P7 = (int16_t)bmpReg16(0x9A); bmp_P8 = (int16_t)bmpReg16(0x9C); bmp_P9 = (int16_t)bmpReg16(0x9E);
  // Tryb normalny, nadpróbkowanie T x1, P x1, filtr wyłączony.
  Wire.beginTransmission(bmpAddr); Wire.write(0xF4); Wire.write(0x27); Wire.endTransmission();
  Wire.beginTransmission(bmpAddr); Wire.write(0xF5); Wire.write(0x00); Wire.endTransmission();
  return true;
}

// Zwraca ciśnienie w Pa. Najpierw trzeba odczytać temperaturę (bmpTemp).
static float bmpPressure() {
  Wire.beginTransmission(bmpAddr); Wire.write(0xF7); Wire.endTransmission();
  Wire.requestFrom(bmpAddr, 3);
  if (Wire.available() < 3) return NAN;
  int32_t adc_P = ((int32_t)Wire.read() << 12) | ((int32_t)Wire.read() << 4) | ((int32_t)Wire.read() >> 4);

  int64_t var1 = (int64_t)bmp_tfine - 128000;
  int64_t var2 = var1 * var1 * (int64_t)bmp_P6;
  var2 += (var1 * (int64_t)bmp_P5) << 17;
  var2 += ((int64_t)bmp_P4) << 35;
  var1 = ((var1 * var1 * (int64_t)bmp_P3) >> 8) + ((var1 * (int64_t)bmp_P2) << 12);
  var1 = ((((int64_t)1) << 47) + var1) * (int64_t)bmp_P1 >> 33;
  if (var1 == 0) return NAN;

  int64_t p = 1048576 - adc_P;
  p = (((p << 31) - var2) * 3125) / var1;
  var1 = ((int64_t)bmp_P9 * (p >> 13) * (p >> 13)) >> 25;
  var2 = ((int64_t)bmp_P8 * p) >> 19;
  p = ((p + var1 + var2) >> 8) + ((int64_t)bmp_P7 << 4);
  return (float)p / 256.0f;
}

// Zwraca temperaturę w °C i ustawia bmp_tfine (potrzebny do ciśnienia).
static float bmpTemp() {
  Wire.beginTransmission(bmpAddr); Wire.write(0xFA); Wire.endTransmission();
  Wire.requestFrom(bmpAddr, 3);
  if (Wire.available() < 3) return NAN;
  int32_t adc_T = ((int32_t)Wire.read() << 12) | ((int32_t)Wire.read() << 4) | ((int32_t)Wire.read() >> 4);

  int32_t var1 = (((adc_T >> 3) - ((int32_t)bmp_T1 << 1)) * (int32_t)bmp_T2) >> 11;
  int32_t var2 = (((((adc_T >> 4) - (int32_t)bmp_T1) * ((adc_T >> 4) - (int32_t)bmp_T1)) >> 12) * (int32_t)bmp_T3) >> 14;
  bmp_tfine = var1 + var2;
  return (float)((bmp_tfine * 5 + 128) >> 8) / 100.0f;
}

static String devName() {
  if (C_name.length()) return C_name;
  String m = WiFi.macAddress();
  String suf = m.substring(m.length() - 8);
  suf.replace(":", "");
  return "stacja-c3-" + suf;
}

// Przyjazny opis powodu resetu (diagnostyka / log).
static String resetReasonStr() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:   return "włączenie zasilania";
    case ESP_RST_SW:        return "restart programowy";
    case ESP_RST_PANIC:     return "panic (wyjątek)";
    case ESP_RST_INT_WDT:   return "watchdog przerwań (IWDT)";
    case ESP_RST_TASK_WDT:  return "watchdog zadania (TWDT)";
    case ESP_RST_WDT:       return "watchdog (inny)";
    case ESP_RST_DEEPSLEEP: return "wybudzenie z deep sleep";
    case ESP_RST_BROWNOUT:  return "spadek napięcia (brownout)";
    case ESP_RST_SDIO:      return "reset SDIO";
    default:                return "nieznany (" + String((int)esp_reset_reason()) + ")";
  }
}

// ============================ AHT20 (I2C 0x38) ============================
static bool ahtBegin() {
  Wire.beginTransmission(0x38);
  Wire.write(0xBE);                 // inicjalizacja
  if (Wire.endTransmission() != 0) return false;
  delay(10);
  Wire.beginTransmission(0x38);
  Wire.write(0x71);                 // odczyt rejestru statusu
  Wire.endTransmission();
  Wire.requestFrom(0x38, 1);
  uint8_t st = Wire.available() ? (uint8_t)Wire.read() : 0;
  return (st & 0x18) == 0x18;       // bit 3 = skalibrowany
}

static bool ahtRead(float& t, float& h) {
  Wire.beginTransmission(0x38);
  Wire.write(0xAC); Wire.write(0x33); Wire.write(0x00);   // trigger pomiaru
  if (Wire.endTransmission() != 0) return false;
  delay(80);
  Wire.requestFrom(0x38, 7);
  if (Wire.available() < 7) return false;
  uint8_t d[7];
  for (int i = 0; i < 7; i++) d[i] = (uint8_t)Wire.read();
  if (d[0] & 0x80) return false;    // zajęty
  uint32_t hum = ((uint32_t)d[1] << 12) | ((uint32_t)d[2] << 4) | (d[3] >> 4);
  uint32_t tmp = (((uint32_t)(d[3] & 0x0F)) << 16) | ((uint32_t)d[4] << 8) | d[5];
  h = (float)hum * 100.0f / 1048576.0f;
  t = (float)tmp * 200.0f / 1048576.0f - 50.0f;
  return true;
}

// ============================ PMS7003 (UART0) ============================
static void pmsParse() {
  while (Serial0.available()) {
    if (Serial0.read() != 0x42) continue;
    if (!Serial0.available()) return;
    if (Serial0.read() != 0x4D) continue;

    unsigned long timeout = millis() + 500;
    while (Serial0.available() < 2 && millis() < timeout) delay(1);
    if (Serial0.available() < 2) return;
    uint16_t len = ((uint16_t)Serial0.read() << 8) | Serial0.read();
    if (len < 28 || len > 64) continue;

    uint8_t buf[64];
    timeout = millis() + 500;
    size_t got = 0;
    while (got < len && millis() < timeout) {
      if (Serial0.available()) buf[got++] = (uint8_t)Serial0.read();
    }
    if (got < len) return;

    uint16_t sum = 0x42 + 0x4D + (len >> 8) + (len & 0xFF);
    for (int i = 0; i < len - 2; i++) sum += buf[i];
    uint16_t rxc = ((uint16_t)buf[len - 2] << 8) | buf[len - 1];
    if (sum != rxc) continue;

    auto u16 = [&](int i) { return ((uint16_t)buf[i] << 8) | buf[i + 1]; };
    // buf to dane PO 2-bajtowej długości ramki. Układ PMS7003:
    //   u16(0)=PM1.0 CF1, u16(2)=PM2.5 CF1, u16(4)=PM10 CF1,
    //   u16(6)=PM1.0 atmosferyczne, u16(8)=PM2.5 atmosferyczne,
    //   u16(10)=PM10 atmosferyczne. Używamy pomiaru ATMOSFERYCZNEGO.
    s_pm1  = u16(6)  * K_pm1F;
    s_pm25 = u16(8)  * K_pm25F;
    s_pm10 = u16(10) * K_pm10F;
    s_pmsLastMs = millis();
    return;   // jedna ramka na wywołanie
  }
}

// ============================ MQTT ============================
static WiFiClient   wifiClient;
static PubSubClient mqtt(wifiClient);
static unsigned long g_lastMqtt = 0, g_lastDisc = 0;

static String stateTopic(const char* id) { return C_mqttPrefix + "/sensor/" + id + "/state"; }
static String discTopic(const char* id)  { return "homeassistant/sensor/" + C_mqttPrefix + "_" + id + "/config"; }

static void mqttPublishDiscovery(const char* id, const char* name, const char* cls,
                                 const char* unit, const char* icon) {
  JsonDocument d;
  d["name"] = devName() + " " + name;
  d["unique_id"] = C_mqttPrefix + "_" + id;
  d["state_topic"] = stateTopic(id);
  d["availability_topic"] = C_mqttPrefix + "/status";
  if (cls && cls[0]) d["device_class"] = cls;
  if (unit && unit[0]) d["unit_of_measurement"] = unit;
  if (icon && icon[0]) d["icon"] = icon;
  d["expire_after"] = 600;
  JsonObject dev = d["device"].to<JsonObject>();
  dev["name"] = devName();
  JsonArray ids = dev["identifiers"].to<JsonArray>();
  ids.add(devName());
  dev["model"] = "Stacja C3 (powietrze)";
  dev["manufacturer"] = "kruzio1985";
  String out; serializeJson(d, out);
  mqtt.publish(discTopic(id).c_str(), out.c_str(), true);
}

static void mqttPublishState(const char* id, float v) {
  char b[32];
  dtostrf(v, 0, 2, b);
  mqtt.publish(stateTopic(id).c_str(), b, true);
}

static void mqttAnnounceAll() {
  mqttPublishDiscovery("pm1", "PM1.0", "pm1", "µg/m³", "mdi:air-filter");
  mqttPublishDiscovery("pm25", "PM2.5", "pm25", "µg/m³", "mdi:air-filter");
  mqttPublishDiscovery("pm10", "PM10", "pm10", "µg/m³", "mdi:air-filter");
  mqttPublishDiscovery("temp", "Temperatura", "temperature", "°C", "mdi:thermometer");
  mqttPublishDiscovery("hum", "Wilgotność", "humidity", "%", "mdi:water-percent");
  mqttPublishDiscovery("press", "Ciśnienie", "atmospheric_pressure", "hPa", "mdi:gauge");
  mqttPublishDiscovery("dew", "Punkt rosy", "temperature", "°C", "mdi:water-thermometer");
  mqttPublishDiscovery("heat", "Odczuwalna", "temperature", "°C", "mdi:thermometer-lines");
  mqttPublishDiscovery("absh", "Wilgotność bezwzględna", "humidity", "g/m³", "mdi:water");
  mqttPublishDiscovery("alt", "Wysokość n.p.m.", "", "m", "mdi:image-filter-hdr");
  mqttPublishDiscovery("aqi", "AQI EPA", "aqi", "", "mdi:air-filter");
  mqttPublishDiscovery("pm25_60m", "PM2.5 60 min", "pm25", "µg/m³", "mdi:air-filter");
}

static void mqttEnsure() {
  if (C_mqttHost.length() == 0) return;
  if (!mqtt.connected()) {
    static unsigned long lastTry = 0;
    if (millis() - lastTry < 10000) return;
    lastTry = millis();
    String id = "c3air-" + String((uint32_t)ESP.getEfuseMac(), HEX);
    String t = C_mqttPrefix + "/status";
    if (mqtt.connect(id.c_str(), C_mqttUser.c_str(), C_mqttPass.c_str(),
                     t.c_str(), 0, true, "offline")) {
      logE("MQTT: połączono z %s", C_mqttHost.c_str());
      mqtt.publish(t.c_str(), "online", true);
      g_lastDisc = 0;   // ogłoś discovery od razu
    } else {
      logE("MQTT: brak połączenia (kod %d)", mqtt.state());
    }
    return;
  }
  mqtt.loop();

  unsigned long now = millis();
  if (g_lastDisc == 0 || now - g_lastDisc > 300000) {
    g_lastDisc = now;
    mqttAnnounceAll();
  }
  if (now - g_lastMqtt >= 30000) {
    g_lastMqtt = now;
    if (!isnan(s_pm1))  mqttPublishState("pm1", s_pm1);
    if (!isnan(s_pm25)) mqttPublishState("pm25", s_pm25);
    if (!isnan(s_pm10)) mqttPublishState("pm10", s_pm10);
    if (!isnan(s_temp)) mqttPublishState("temp", s_temp);
    if (!isnan(s_hum))  mqttPublishState("hum", s_hum);
    if (!isnan(s_press)) mqttPublishState("press", s_press);
    if (!isnan(s_dew))  mqttPublishState("dew", s_dew);
    if (!isnan(s_heat)) mqttPublishState("heat", s_heat);
    if (!isnan(s_absh)) mqttPublishState("absh", s_absh);
    if (!isnan(s_alt))  mqttPublishState("alt", s_alt);
    if (!isnan(s_aqi))  mqttPublishState("aqi", s_aqi);
    if (!isnan(s_pm25_60m)) mqttPublishState("pm25_60m", s_pm25_60m);
  }
}

// ============================ WWW ============================
WebServer web(80);

static String esc(const String& s) {
  String r = s;
  r.replace("&", "&amp;"); r.replace("<", "&lt;"); r.replace(">", "&gt;"); r.replace("\"", "&quot;");
  return r;
}

static String shell(const String& title, const String& active, const String& body) {
  String m = "<div class='menu'>"
    "<a class='" + String(active == "dash" ? "on" : "") + "' href='/'>🏠 Pulpit</a>"
    "<a class='" + String(active == "calib" ? "on" : "") + "' href='/calib'>🎚️ Kalibracja</a>"
    "<a class='" + String(active == "setup" ? "on" : "") + "' href='/setup'>⚙️ Ustawienia</a>"
    "<a class='" + String(active == "ota" ? "on" : "") + "' href='/ota'>⬆️ OTA</a>"
    "<a class='" + String(active == "diag" ? "on" : "") + "' href='/diag'>🩺 Diagnostyka</a>"
    "<a class='" + String(active == "log" ? "on" : "") + "' href='/log'>📜 Logi</a>"
    "</div>";
  return "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>" + title + "</title><style>"
    "*{box-sizing:border-box}body{font-family:system-ui,sans-serif;background:#111;color:#eee;margin:0}"
    ".menu{position:fixed;left:0;top:0;bottom:0;width:190px;background:#16161f;padding:14px 0;overflow:auto}"
    ".menu a{display:block;color:#ccc;text-decoration:none;padding:10px 16px;border-left:3px solid transparent}"
    ".menu a.on,.menu a:hover{background:#1f1f2e;color:#0cf;border-left:3px solid #0cf}"
    ".main{margin-left:190px;padding:16px}"
    "h2{color:#0cf}table{width:100%;border-collapse:collapse;margin-top:10px}"
    "th,td{border-bottom:1px solid #333;padding:6px;text-align:left;font-size:.85em}"
    "th{color:#0cf}.ok{color:#4c4}.err{color:#e44}.raw{font-family:monospace;font-size:.75em;color:#9ad0ff;word-break:break-all}"
    "input,select{width:100%;max-width:340px;padding:7px;margin:4px 0;background:#1a1a2e;color:#eee;border:1px solid #333;border-radius:4px}"
    "button{background:#0cf;color:#000;border:0;padding:8px 16px;border-radius:4px;font-weight:bold;cursor:pointer;margin:2px}"
    ".card{background:#16161f;padding:14px;border-radius:8px;margin-bottom:12px}"
    ".tile{display:inline-block;background:#16161f;border-radius:8px;padding:14px;margin:6px;min-width:130px;text-align:center;vertical-align:top}"
    ".tile .v{font-size:1.6em;color:#0cf}.tile .l{color:#888;font-size:.8em}"
    "label{display:block;margin-top:8px;color:#bbb;font-size:.85em}"
    ".chk{display:flex;align-items:center;gap:6px}.chk input{width:auto;max-width:none}"
    "</style></head><body>" + m + "<div class='main'><h2>" + title + "</h2>" + body + "</div></body></html>";
}

static String tile(float v, const String& unit, const String& label) {
  if (isnan(v)) return "<div class='tile'><div class='v'>—</div><div class='l'>" + label + "</div></div>";
  return "<div class='tile'><div class='v'>" + String(v, 1) + " " + unit + "</div><div class='l'>" + label + "</div></div>";
}
static String tileC(float v, const String& unit, const String& label, const char* color) {
  if (isnan(v)) return "<div class='tile'><div class='v'>—</div><div class='l'>" + label + "</div></div>";
  return "<div class='tile' style='border:1px solid " + String(color) + "'><div class='v' style='color:" + String(color) + "'>" +
         String(v, 1) + " " + unit + "</div><div class='l'>" + label + "</div></div>";
}

static const char* pmColor(float c) {
  if (isnan(c)) return "#888";
  if (c <= 10) return "#00e400";      // dobra
  if (c <= 15) return "#76ff03";      // bardzo niska
  if (c <= 25) return "#ffd600";      // umiarkowana
  if (c <= 35) return "#ff7e00";      // dostateczna
  if (c <= 55) return "#ff0000";      // zła
  if (c <= 150) return "#8f3f97";     // bardzo zła
  return "#7e0023";                   // niebezpieczna
}

static void handleDash() {
  // --- AQI ---
  String b = "<div class='card'><b>Jakość powietrza (EPA AQI)</b><br>";
  if (isnan(s_aqi)) {
    b += "<span class='err'>brak pomiaru PM</span>";
  } else {
    b += "<div class='tile' style='min-width:200px'><div class='v' style='font-size:2.4em;color:" +
         String(aqiCatColor(s_aqiCat)) + "'>" + String((int)(s_aqi + 0.5f)) + "</div>" +
         "<div class='l' style='color:" + String(aqiCatColor(s_aqiCat)) + "'>" + String(aqiCatName(s_aqiCat)) + "</div></div>";
    b += "<div style='margin:8px 0'><table><tr>";
    for (int c = 0; c < 6; c++) b += "<td style='text-align:center;font-size:.7em;color:" + String(aqiCatColor(c)) + "'>" +
        String(aqiCatName(c)) + "<br><div style='background:" + String(aqiCatColor(c)) + ";height:10px;width:44px;margin:2px auto'></div></td>";
    b += "</tr></table></div>";
  }
  b += "</div>";

  // --- Pył ---
  b += "<div class='card'><b>Pył zawieszony (PMS7003, pomiar atmosferyczny)</b><br>";
  b += tileC(s_pm1,  "µg/m³", "PM1.0",  pmColor(s_pm1));
  b += tileC(s_pm25, "µg/m³", "PM2.5",  pmColor(s_pm25));
  b += tileC(s_pm10, "µg/m³", "PM10",   pmColor(s_pm10));
  b += tile(s_pm25_1m, "µg/m³", "PM2.5 · 1 min");
  b += tile(s_pm25_15m, "µg/m³", "PM2.5 · 15 min");
  b += tile(s_pm25_60m, "µg/m³", "PM2.5 · 60 min");
  b += tile(s_pm25ratio, "", "PM2.5 / PM10");
  b += "</div>";

  // --- Ostrzeżenia ---
  String warn = "";
  if (!isnan(s_hum) && s_hum > 80.0f)
    warn += "<div class='err'>⚠️ Wilgotność &gt; 80% — odczyt PM mniej wiarygodny (kropelki wody zawyżają PM2.5/PM10).</div>";
  if (!isnan(s_aqi) && s_aqi > 100.0f)
    warn += "<div class='err'>⚠️ Wysoki AQI (" + String((int)(s_aqi + 0.5f)) + ") — " + String(aqiCatName(s_aqiCat)) + ".</div>";
  if (!isnan(s_temp) && s_temp < -10.0f)
    warn += "<div class='err'>⚠️ Niska temperatura — poniżej zakresu pełnej dokładności PMS7003 (-10…+60 °C).</div>";
  if (warn.length()) b += "<div class='card'>" + warn + "</div>";

  // --- Pogoda ---
  b += "<div class='card'><b>Pogoda</b><br>";
  b += tile(s_temp, "°C", "Temperatura");
  b += tile(s_hum, "%", "Wilgotność");
  b += tile(s_press, "hPa", "Ciśnienie");
  b += tile(s_dew, "°C", "Punkt rosy");
  b += tile(s_absh, "g/m³", "Wilg. bezwzględna");
  b += tile(s_heat, "°C", "Odczuwalna (heat index)");
  b += tile(s_alt, "m", "Wysokość n.p.m.");
  String trend = (s_trend == 1) ? "<span class='ok'>▲ rośnie</span>" : (s_trend == -1 ? "<span class='err'>▼ spada</span>" : "— stabilne");
  b += "<div class='tile'><div class='v'>" + (isnan(s_trend3h) ? "—" : String(s_trend3h, 1) + " hPa") + "</div><div class='l'>Ciśnienie 3 h</div><div class='l'>" + trend + "</div></div>";
  b += "</div>";

  // --- Stan ---
  String ip = WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "AP " + WiFi.softAPIP().toString();
  b += "<div class='card'>Wi-Fi: " + ip + " · IP ustawione: " + esc(C_ip) + (C_static ? " (statyczne)" : " (DHCP)") + "<br>";
  b += "MQTT: " + (C_mqttHost.length() ? (mqtt.connected() ? "<span class='ok'>połączony</span> " : "<span class='err'>rozłączony</span> ") + esc(C_mqttHost) : "<span class='err'>nie skonfigurowano</span>") + "<br>";
  b += "PMS7003: " + String(millis() - s_pmsLastMs < 60000 && !isnan(s_pm25) ? "<span class='ok'>odbiór OK</span>" : "<span class='err'>brak ramek</span>");
  if (C_lat != 0 || C_lon != 0) b += " · Położenie: " + String(C_lat, 4) + ", " + String(C_lon, 4);
  b += "</div>";

  web.send(200, "text/html", shell("Pulpit", "dash", b));
}

static void handleCalib() {
  String b = "<div class='card'><b>Kalibracja czujników</b>"
    "<p class='raw'>Offset jest dodawany do odczytu, współczynnik mnoży odczyt (1.0 = bez zmian).</p>"
    "<form method='post' action='/calsave'>"
    "<label>Offset temperatury [°C]</label><input name='kt' type='number' step='0.1' value='" + String(K_tempOff, 2) + "'>"
    "<label>Offset wilgotności [%]</label><input name='kh' type='number' step='0.1' value='" + String(K_humOff, 2) + "'>"
    "<label>Offset ciśnienia [hPa]</label><input name='kp' type='number' step='0.1' value='" + String(K_pressOff, 2) + "'>"
    "<label>Współczynnik PM1.0</label><input name='kpm1' type='number' step='0.01' value='" + String(K_pm1F, 3) + "'>"
    "<label>Współczynnik PM2.5</label><input name='kpm25' type='number' step='0.01' value='" + String(K_pm25F, 3) + "'>"
    "<label>Współczynnik PM10</label><input name='kpm10' type='number' step='0.01' value='" + String(K_pm10F, 3) + "'>"
    "<br><button type='submit'>Zapisz</button></form></div>";
  web.send(200, "text/html", shell("Kalibracja", "calib", b));
}

static void handleCalSave() {
  K_tempOff = web.arg("kt").toFloat();
  K_humOff  = web.arg("kh").toFloat();
  K_pressOff = web.arg("kp").toFloat();
  K_pm1F    = web.arg("kpm1").toFloat();
  K_pm25F   = web.arg("kpm25").toFloat();
  K_pm10F   = web.arg("kpm10").toFloat();
  if (!isfinite(K_pm1F) || K_pm1F <= 0) K_pm1F = 1.0f;
  if (!isfinite(K_pm25F) || K_pm25F <= 0) K_pm25F = 1.0f;
  if (!isfinite(K_pm10F) || K_pm10F <= 0) K_pm10F = 1.0f;
  saveCfg();
  web.send(200, "text/html", shell("Kalibracja", "calib", "<p class='ok'>Zapisano.</p><script>setTimeout(()=>location='/calib',1200)</script>"));
}

static void handleSetup() {
  String b = "<div class='card'><b>Konfiguracja</b><form method='post' action='/save'>"
    "<label>Nazwa urządzenia (pusta = z MAC)</label><input name='name' value='" + esc(C_name) + "'>"
    "<label>Wi-Fi SSID</label><input name='ssid' value='" + esc(C_ssid) + "'>"
    "<label>Hasło Wi-Fi</label><input name='pass' type='password' value='" + esc(C_pass) + "'>"
    "<label class='chk'><input type='checkbox' name='static'" + (C_static ? " checked" : "") + "> Stały adres IP</label>"
    "<label>IP</label><input name='ip' value='" + esc(C_ip) + "'>"
    "<label>Brama</label><input name='gw' value='" + esc(C_gw) + "'>"
    "<label>Maska</label><input name='mask' value='" + esc(C_mask) + "'>"
    "<label>Hasło punktu AP</label><input name='ap' value='" + esc(C_apPass) + "'>"
    "<br><b>Położenie geograficzne</b> (do obliczeń; opcjonalnie adres mastera do pobrania GPS)"
    "<label>Szerokość geograficzna (lat)</label><input name='lat' type='number' step='0.0001' value='" + String(C_lat, 4) + "'>"
    "<label>Długość geograficzna (lon)</label><input name='lon' type='number' step='0.0001' value='" + String(C_lon, 4) + "'>"
    "<label>Adres mastera (do GPS, np. http://192.168.1.143)</label><input name='murl' value='" + esc(C_masterUrl) + "'>"
    "<label>Ciśnienie odniesienia (poziom morza) [hPa]</label><input name='p0' type='number' step='0.1' value='" + String(C_p0, 1) + "'>"
    "<br><b>MQTT (Home Assistant)</b>"
    "<label>Adres brokera (IP)</label><input name='mqh' value='" + esc(C_mqttHost) + "' placeholder='192.168.1.14'>"
    "<label>Port</label><input name='mqp' value='" + String(C_mqttPort) + "'>"
    "<label>Użytkownik</label><input name='mqu' value='" + esc(C_mqttUser) + "'>"
    "<label>Hasło / token</label><input name='mqw' type='password' value='" + esc(C_mqttPass) + "'>"
    "<label>Prefiks tematów</label><input name='mqx' value='" + esc(C_mqttPrefix) + "'>"
    "<br><button type='submit'>Zapisz i restart</button></form></div>"
    "<div class='card'><b>Skan sieci</b> <button onclick='scanWifi()'>Skanuj</button><div id='wlist' class='raw'></div></div>"
    "<script>function scanWifi(){document.getElementById('wlist').innerHTML='skanowanie...';"
    "fetch('/wifi/scan').then(r=>r.json()).then(j=>{"
    "let h=''; for(const n of j.net){h+='<div style=\"margin:2px 0\"><b>'+n.ssid+'</b> · '+n.rssi+' dBm · '+n.enc+'</div>';}"
    "document.getElementById('wlist').innerHTML=h||'brak sieci';})}</script>";
  web.send(200, "text/html", shell("Ustawienia", "setup", b));
}

static void handleSave() {
  C_name   = web.arg("name");
  C_ssid   = web.arg("ssid"); C_pass = web.arg("pass"); C_apPass = web.arg("ap");
  C_ip     = web.arg("ip"); C_gw = web.arg("gw"); C_mask = web.arg("mask");
  C_static = web.hasArg("static");
  C_mqttHost = web.arg("mqh"); C_mqttPort = web.arg("mqp").toInt();
  C_mqttUser = web.arg("mqu"); C_mqttPass = web.arg("mqw"); C_mqttPrefix = web.arg("mqx");
  C_lat = web.arg("lat").toDouble();
  C_lon = web.arg("lon").toDouble();
  C_p0 = web.arg("p0").toFloat();
  C_masterUrl = web.arg("murl");
  if (C_mqttPort < 1 || C_mqttPort > 65535) C_mqttPort = 1883;
  if (C_mqttPrefix.length() == 0) C_mqttPrefix = "stacja_c3";
  if (C_apPass.length() == 0) C_apPass = "12345678";
  if (!isfinite(C_p0) || C_p0 < 800.0f || C_p0 > 1200.0f) C_p0 = 1013.25f;
  saveCfg();
  web.send(200, "text/html", shell("Ustawienia", "setup", "<p class='ok'>Zapisano - restart...</p>"));
  delay(300); ESP.restart();
}

static void handleOtaPage() {
  String b = "<div class='card'><b>Aktualizacja (OTA)</b>"
    "<p>Wgraj plik <code>firmware.bin</code> zbudowany przez PlatformIO.</p>"
    "<form method='post' action='/update' enctype='multipart/form-data'>"
    "<input type='file' name='firmware'><br><button type='submit'>Wgraj</button></form>"
    "<p class='ok'>Komputerowo: <code>pio run -e esp32c3-air -t upload --upload-port 192.168.1.147</code></p></div>";
  web.send(200, "text/html", shell("Aktualizacja", "ota", b));
}

static void handleUpdate() {
  HTTPUpload& u = web.upload();
  if (u.status == UPLOAD_FILE_START) {
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) logE("OTA: begin fail");
  } else if (u.status == UPLOAD_FILE_WRITE) {
    // Podczas wolnego zapisu przez Wi-Fi watchdog zadania nie może przerwać OTA.
    esp_task_wdt_reset();
    if (Update.write(u.buf, u.currentSize) != u.currentSize) logE("OTA: write fail");
  } else if (u.status == UPLOAD_FILE_END) {
    if (Update.end(true)) {
      logE("OTA: ok - restart");
      web.send(200, "text/html", shell("Aktualizacja", "ota", "<p class='ok'>OK - restart...</p>"));
      delay(300); ESP.restart();
    } else {
      logE("OTA: end fail %s", Update.errorString());
      web.send(200, "text/html", shell("Aktualizacja", "ota", "<p>Błąd: " + String(Update.errorString()) + "</p>"));
    }
  }
}

static void handleDiag() {
  String b = "<div class='card'><b>System</b><table>"
    "<tr><th>Czas pracy</th><td>" + String(millis() / 1000) + " s</td></tr>"
    "<tr><th>Uruchomienia</th><td>" + String(g_boots) + "</td></tr>"
    "<tr><th>Powód ostatniego resetu</th><td>" + resetReasonStr() + "</td></tr>"
    "<tr><th>Wolna pamięć (heap)</th><td>" + String(ESP.getFreeHeap()) + " B</td></tr>"
    "<tr><th>Najmniejszy zapas heap</th><td>" + String(ESP.getMinFreeHeap()) + " B</td></tr>"
    "<tr><th>Zegar CPU</th><td>" + String(ESP.getCpuFreqMHz()) + " MHz</td></tr>"
    "<tr><th>Układ</th><td>" + String(ESP.getChipModel()) + " rev " + String(ESP.getChipRevision()) + "</td></tr>"
    "</table></div>";

  b += "<div class='card'><b>Watchdog zadania</b><table>"
    "<tr><th>Timeout</th><td>15 s (reset przy zawieszeniu pętli)</td></tr>"
    "<tr><th>Stan</th><td><span class='ok'>aktywny</span></td></tr>"
    "</table></div>";

  b += "<div class='card'><b>Wi-Fi</b><table>"
    "<tr><th>SSID</th><td>" + esc(C_ssid) + "</td></tr>"
    "<tr><th>Tryb</th><td>" + String(WiFi.status() == WL_CONNECTED ? "STA" : "AP") + "</td></tr>"
    "<tr><th>IP</th><td>" + (WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : WiFi.softAPIP().toString()) + "</td></tr>"
    "<tr><th>RSSI</th><td>" + String(WiFi.RSSI()) + " dBm</td></tr>"
    "</table></div>";

  b += "<div class='card'><b>Czujniki</b><table>"
    "<tr><th>AHT20</th><td>" + String(ahtOk ? "<span class='ok'>OK</span>" : "<span class='err'>brak</span>") + "</td></tr>"
    "<tr><th>BMP280</th><td>" + String(bmpAddr ? "<span class='ok'>OK (0x" + String(bmpAddr, HEX) + ")</span>" : "<span class='err'>brak</span>") + "</td></tr>"
    "<tr><th>PMS7003</th><td>" + String(!isnan(s_pm25) ? "<span class='ok'>ramka " + String((millis() - s_pmsLastMs) / 1000) + " s temu</span>" : "<span class='err'>brak ramek</span>") + "</td></tr>"
    "</table></div>";

  b += "<div class='card'><b>MQTT</b><table>"
    "<tr><th>Broker</th><td>" + (C_mqttHost.length() ? esc(C_mqttHost) + ":" + String(C_mqttPort) : "<span class='err'>nie skonfigurowano</span>") + "</td></tr>"
    "<tr><th>Stan</th><td>" + String(mqtt.connected() ? "<span class='ok'>połączony</span>" : "<span class='err'>rozłączony</span>") + "</td></tr>"
    "<tr><th>Prefiks</th><td>" + esc(C_mqttPrefix) + "</td></tr>"
    "</table></div>";

  web.send(200, "text/html", shell("Diagnostyka", "diag", b));
}

static void handleLog() {
  String b = "<div class='card'><b>Log (RAM, bieżące uruchomienie)</b>"
    "<br><span class='ok'>Powód resetu: " + resetReasonStr() + " · uruchomień: " + String(g_boots) + "</span>"
    "<table><tr><th>Czas</th><th>Wiadomość</th></tr>";
  int start = g_logN > LOG_N ? g_logN - LOG_N : 0;
  for (int i = start; i < g_logN; i++)
    b += "<tr><td class='raw'>" + String(g_log[i % LOG_N]) + "</td></tr>";
  b += "</table></div>";

  String fs = fsLogRead();
  b += "<div class='card'><b>Log trwały (zapisane na flash, ostatnie wpisy)</b>";
  if (!g_fsOk) {
    b += "<p class='err'>Brak systemu plików - log trwały niedostępny.</p>";
  } else if (fs.length() == 0) {
    b += "<p>Brak wpisów.</p>";
  } else {
    // pokaż najnowsze na dole (odwróć kolejność)
    int lines = 0;
    String cur;
    for (int i = fs.length() - 1; i >= 0 && lines < 60; i--) {
      cur = fs[i] + cur;
      if (fs[i] == '\n' || i == 0) {
        cur.trim();
        if (cur.length()) { b += "<div class='raw'>" + esc(cur) + "</div>"; lines++; }
        cur = "";
      }
    }
  }
  b += "</div>";
  web.send(200, "text/html", shell("Logi", "log", b));
}

static void handleWifiScan() {
  int n = WiFi.scanNetworks();
  JsonDocument doc;
  doc["n"] = n;
  JsonArray arr = doc["net"].to<JsonArray>();
  for (int i = 0; i < n && i < 30; i++) {
    JsonObject o = arr.add<JsonObject>();
    o["ssid"] = WiFi.SSID(i);
    o["rssi"] = WiFi.RSSI(i);
    o["enc"] = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN) ? "otwarta" : "zabezpieczona";
  }
  String out; serializeJson(doc, out);
  web.send(200, "application/json", out);
}

// /api/status - format importowany przez stację pogody (urządzenie "aq").
static void handleApiStatus() {
  JsonDocument doc;
  doc["fw"] = "c3air";
  doc["uptime_s"] = (uint32_t)(millis() / 1000);
  doc["mqtt"] = mqtt.connected();
  JsonArray arr = doc["channels"].to<JsonArray>();
  auto add = [&](const char* id, const char* name, const char* unit, float v) {
    JsonObject o = arr.add<JsonObject>();
    o["id"] = id; o["name"] = name; o["unit"] = unit;
    if (isnan(v)) { o["value"] = nullptr; o["valid"] = false; }
    else { o["value"] = v; o["valid"] = true; }
  };
  add("pm1",  "PM1.0",  "ug/m3", s_pm1);
  add("pm25", "PM2.5",  "ug/m3", s_pm25);
  add("pm10", "PM10",   "ug/m3", s_pm10);
  add("temp", "Temperatura", "degC", s_temp);
  add("hum",  "Wilgotnosc",  "%",    s_hum);
  add("press", "Cisnienie",  "hPa",  s_press);
  // parametry obliczane
  add("dew",   "Punkt rosy", "degC", s_dew);
  add("absh",  "Wilgotnosc bezwzgledna", "g/m3", s_absh);
  add("heat",  "Odczuwalna", "degC", s_heat);
  add("alt",   "Wysokosc n.p.m.", "m", s_alt);
  add("aqi",   "AQI EPA", "", s_aqi);
  add("pm25_1m",  "PM2.5 1 min",  "ug/m3", s_pm25_1m);
  add("pm25_15m", "PM2.5 15 min", "ug/m3", s_pm25_15m);
  add("pm25_60m", "PM2.5 60 min", "ug/m3", s_pm25_60m);
  String out; serializeJson(doc, out);
  web.send(200, "application/json", out);
}

// ============================ Wi-Fi ============================
static void setupWifi() {
  WiFi.mode(WIFI_STA);
  if (C_static) {
    IPAddress ip, gw, mk;
    if (ip.fromString(C_ip) && gw.fromString(C_gw) && mk.fromString(C_mask))
      WiFi.config(ip, gw, mk);
  }
  if (C_ssid.length()) {
    WiFi.begin(C_ssid.c_str(), C_pass.c_str());
    for (int i = 0; i < 30 && WiFi.status() != WL_CONNECTED; i++) delay(500);
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[c3air] IP: %s\n", WiFi.localIP().toString().c_str());
    return;
  }
  WiFi.mode(WIFI_AP);
  WiFi.softAP(devName().c_str(), C_apPass.c_str());
  Serial.printf("[c3air] AP %s: %s\n", devName().c_str(), WiFi.softAPIP().toString().c_str());
}

// ============================ GPS z mastera ============================
static unsigned long g_lastGps = 0;
static void fetchMasterGps() {
  if (C_masterUrl.length() == 0) return;
  if (millis() - g_lastGps < 15UL * 60000UL) return;
  g_lastGps = millis();
  HTTPClient h;
  h.setConnectTimeout(3000); h.setTimeout(4000);
  String url = C_masterUrl;
  if (!url.endsWith("/")) url += "/";
  url += "api/config";
  if (h.begin(url)) {
    int code = h.GET();
    if (code == 200) {
      JsonDocument d;
      if (!deserializeJson(d, h.getString())) {
        double lat = d["latitude"] | 0.0;
        double lon = d["longitude"] | 0.0;
        if (lat != 0.0 || lon != 0.0) { C_lat = lat; C_lon = lon; }
      }
    }
    h.end();
  }
}

// ============================ Odczyt ============================
static unsigned long s_lastRead = 0;
static unsigned long s_lastPressSample = 0;

static void sensorsRead() {
  unsigned long now = millis();
  if (now - s_lastRead < 3000) return;
  s_lastRead = now;

  float t = NAN, h = NAN;
  if (ahtOk && ahtRead(t, h)) {
    s_temp = t + K_tempOff;
    s_hum  = h + K_humOff;
  }
  if (bmpAddr) {
    // najpierw temperatura (ustawia bmp_tfine), potem ciśnienie
    float bt = bmpTemp();
    float p = bmpPressure();
    if (!isnan(p) && p > 20000.0f && p < 130000.0f) s_press = p / 100.0f + K_pressOff;
    if (isnan(s_temp) && !isnan(bt)) s_temp = bt + K_tempOff;
  }
  if (millis() - s_pmsLastMs > 120000) {
    // ramki dawno nie było - nie pokazuj nieaktualnego PM
    s_pm1 = s_pm25 = s_pm10 = NAN;
  }

  // Próbka ciśnienia co 5 min (historia do trendu 3 h)
  if (!isnan(s_press) && now - s_lastPressSample >= 5UL * 60000UL) {
    s_lastPressSample = now;
    g_press[g_pressN % PRESS_N] = s_press;
    g_pressT[g_pressN % PRESS_N] = now;
    g_pressN++;
  }
  // Próbka PM2.5 (historia do średnich kroczących)
  if (!isnan(s_pm25)) {
    g_pm25[g_pm25N % PM_N] = s_pm25;
    g_pm25T[g_pm25N % PM_N] = now;
    g_pm25N++;
  }

  // Średnie kroczące PM2.5: 1 / 15 / 60 min
  s_pm25_1m = s_pm25_15m = s_pm25_60m = NAN;
  if (g_pm25N > 0) {
    auto avgSince = [&](uint32_t ageMs) {
      float sum = 0; int n = 0;
      for (int i = g_pm25N - 1; i >= 0 && n < PM_N; i--) {
        int idx = i % PM_N;
        if (now - g_pm25T[idx] <= ageMs) { sum += g_pm25[idx]; n++; }
        else break;
      }
      return n ? sum / n : NAN;
    };
    s_pm25_1m  = avgSince(60UL * 1000UL);
    s_pm25_15m = avgSince(15UL * 60000UL);
    s_pm25_60m = avgSince(60UL * 60000UL);
  }

  computeDerived();
}

// ============================ Setup / loop ============================
void setup() {
  Serial.begin(115200);
  delay(200);
  loadCfg();
  g_boots = bootCount();
  logE("start #%u, reset: %s", (unsigned)g_boots, resetReasonStr().c_str());

  // Trwały log (LittleFS) - zamontuj i od razu zapisz wpis startowy,
  // żeby po awarii było widać powód poprzedniego resetu.
  g_fsOk = LittleFS.begin(true);
  flushLog();

  // Watchdog zadania: 15 s. Gdy pętla główna się zawiesi, ESP zrestartuje się
  // sam (a powód zobaczymy w Diagnostyce jako "watchdog zadania").
  esp_task_wdt_init(15, true);
  esp_task_wdt_add(NULL);

  Wire.begin(PIN_SDA, PIN_SCL);
  // Skan magistrali I2C - żeby od razu zobaczyć, co realnie jest podłączone.
  {
    String found = "";
    for (int addr = 0x08; addr < 0x78; addr++) {
      Wire.beginTransmission(addr);
      if (Wire.endTransmission() == 0) found += " 0x" + String(addr, HEX);
    }
    logE("I2C:%s", found.length() ? found.c_str() : " (puste)");
  }
  ahtOk = ahtBegin();
  logE("AHT20: %s", ahtOk ? "OK" : "brak");
  // Ciśnienie: BMP280 na 0x76 albo 0x77 (na płytce jest na 0x77).
  if (bmpBegin(0x77) || bmpBegin(0x76)) logE("BMP280: OK (0x%02X)", bmpAddr);
  else logE("BMP280: brak");

  Serial0.begin(9600, SERIAL_8N1, PIN_PMS_RX, PIN_PMS_TX);
  Serial0.setRxBufferSize(256);

  setupWifi();

  if (C_mqttHost.length()) {
    mqtt.setServer(C_mqttHost.c_str(), C_mqttPort);
    mqtt.setBufferSize(1024);
  }

  web.on("/", handleDash);
  web.on("/calib", handleCalib);
  web.on("/calsave", handleCalSave);
  web.on("/setup", handleSetup);
  web.on("/save", handleSave);
  web.on("/ota", handleOtaPage);
  web.on("/update", HTTP_POST, []() { web.send(200, "text/plain", "done"); }, handleUpdate);
  web.on("/log", handleLog);
  web.on("/diag", handleDiag);
  web.on("/wifi/scan", handleWifiScan);
  web.on("/api/status", handleApiStatus);
  web.begin();

  ArduinoOTA.setHostname(devName().c_str());
  ArduinoOTA.onStart([]() { logE("OTA: start"); esp_task_wdt_reset(); });
  // ArduinoOTA blokuje pętlę na czas przesyłania (handle() nie wraca), więc
  // watchdog zadania karmimy w callbacku postępu - inaczej 15 s przerwy
  // w pętli uruchomi reset w środku OTA.
  ArduinoOTA.onProgress([](unsigned int p, unsigned int t) {
    (void)p; (void)t; esp_task_wdt_reset();
  });
  ArduinoOTA.onEnd([]() { logE("OTA: ok - restart"); });
  ArduinoOTA.onError([](ota_error_t e) { logE("OTA: blad %d", (int)e); esp_task_wdt_reset(); });
  ArduinoOTA.begin();

  // Oznacz uruchomioną aplikację jako sprawną - jeśli poprzedni OTA nie
  // zdążył, anuluje rollback; jeśli był uszkodzony, bootloader sam wrócił
  // do sprawnej partycji.
  esp_ota_mark_app_valid_cancel_rollback();

  logE("gotowe (OTA dual-bank, watchdog 15 s)");
}

void loop() {
  ArduinoOTA.handle();
  web.handleClient();
  esp_task_wdt_reset();

  // Konfiguracja po USB (do pierwszego uruchomienia):
  //   wifi <SSID> <HASŁO>   -> zapisz Wi-Fi i restart
  static String serialBuf;
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      serialBuf.trim();
      if (serialBuf.startsWith("wifi ")) {
        int sp = serialBuf.indexOf(' ', 5);
        if (sp > 0) {
          C_ssid = serialBuf.substring(5, sp);
          C_pass = serialBuf.substring(sp + 1);
          saveCfg();
          logE("serial: wifi=%s zapisano - restart", C_ssid.c_str());
          delay(200); ESP.restart();
        }
      } else if (serialBuf == "scan") {
        String found = "";
        for (int addr = 0x08; addr < 0x78; addr++) {
          Wire.beginTransmission(addr);
          if (Wire.endTransmission() == 0) found += " 0x" + String(addr, HEX);
        }
        logE("I2C:%s", found.length() ? found.c_str() : " (puste)");
      }
      serialBuf = "";
    } else {
      serialBuf += c;
      if (serialBuf.length() > 160) serialBuf = "";
    }
  }

  pmsParse();
  sensorsRead();
  fetchMasterGps();
  mqttEnsure();

  // Spłucz log z RAM do trwałej pamięci co 5 min (minimalne zużycie flasha).
  static unsigned long lastLogFlush = 0;
  if (millis() - lastLogFlush >= 300000UL) { lastLogFlush = millis(); flushLog(); }

  static unsigned long lastWdt = 0;
  if (millis() - lastWdt >= 1000) { lastWdt = millis(); esp_task_wdt_reset(); }
}
