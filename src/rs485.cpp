/* =============================================================================
 * Stacja Pogody - stacja meteorologiczna na ESP32-S3 WROOM-1 (N16R8)
 * Copyright (c) 2026 kruzio1985 - https://github.com/kruzio1985
 * Licencja: Stacja Pogody Non-Commercial License 1.0 (plik LICENSE)
 * Użytek niekomercyjny. Kontakt: kruzio1985@users.noreply.github.com
 * =============================================================================
 */
#include "rs485.h"

#if !STACJA_HEADLESS
#include <WiFi.h>
#endif
#include <time.h>
#include <sys/time.h>
#include <vector>
#include <esp_heap_caps.h>
#include <driver/gpio.h>

#include "config.h"
#include "board.h"
#include "gps.h"
#include "pinmap.h"
#include "pins.h"
#include "sensors.h"
#include "syslog.h"
#include "sd_card.h"
#include "drv_rtc.h"
// Status i diagnostyka JSON (apiStatusJson / apiDiagnosticsJson) - te same,
// które normalnie pokazuje strona www. Węzeł bez sieci nie ma web_server.h,
// więc funkcje są w osobnym pliku api_json.cpp.
#include "api_json.h"

Rs485Bus rs485;

// Czas oczekiwania na odpowiedź na "ping" przy wykrywaniu adresu węzła
// i przy skanie magistrali z zakładki "ESP #2".
#define RS485_PING_TIMEOUT_MS   200
// Wykrywanie adresów idzie "po kilka na raz", żeby skan nie blokował strony
// www (każde zapytanie bez odpowiedzi to czekanie). Po pełnym obiegu bez
// trafienia przerwa rośnie, żeby nie hałasować na magistrali.
#define RS485_DISCOVER_STEP     2
#define RS485_DISCOVER_GAP_MS   1200UL
#define RS485_DISCOVER_MAX_GAP_MS 12000UL
// Minimalna przerwa między odpytywaniem węzłów, gdy jest ich kilka.
#define RS485_MIN_POLL_GAP_MS   2000UL
// Jak często najwyżej informować w logu, że na magistrali nie ma węzła.
#define RS485_NOPEER_LOG_MS     600000UL
// Górny limit czasu całego przeglądu pinów ("scout") - w tym czasie strona www
// czeka na odpowiedź, więc przegląd nie może blokować obsługi zbyt długo.
#define RS485_SCOUT_BUDGET_MS   3000UL

// Zapytanie bieżącej wymiany idzie do: adres z trwającego skanu, adres
// wybranego węzła (reqAddr_) albo - gdy nie ma listy - adres z ustawień.
static bool staleOk(uint32_t lastOkMs, uint16_t pollS) {
  if (!lastOkMs) return false;
  uint32_t window = (uint32_t)pollS * 2000UL + 5000UL;
  return (millis() - lastOkMs) <= window;
}

// MAC w jednym formacie (AA:BB:CC:DD:EE:FF) niezależnie od tego, czy przyszedł
// z WiFi, czy z eFuse. Porównania robimy bez wielkości liter i separatorów.
static String normMac(const String& m) {
  String o;
  o.reserve(12);
  for (size_t i = 0; i < m.length(); i++) {
    char c = m[i];
    if (c == ':' || c == '-' || c == ' ') continue;
    o += (char)toupper((unsigned char)c);
  }
  return o;
}

// Każdy bajt z linii - także taki, który nie układa się w ramkę. Pozwala
// odróżnić "przewód nie łączy" (rawRx stoi) od "bajty są, ale nie ma z nich
// ramki" (zły baud, odwrócony sygnał, inny nadawca na linii).
static inline void rawByte(Rs485Stats& s, uint8_t b) {
  s.rawRx++;
  s.lastRawMs = millis();
  if (s.sniffN < (uint8_t)sizeof(s.sniff)) s.sniff[s.sniffN++] = b;
}

// Pierwsze odebrane bajty w postaci hex ("AA 55 01 ...") - do logu i na stronę.
static String sniffHex(const Rs485Stats& s) {
  String o;
  for (uint8_t i = 0; i < s.sniffN; i++) {
    char h[4];
    snprintf(h, sizeof(h), "%02X", s.sniff[i]);
    if (i) o += ' ';
    o += h;
  }
  return o;
}

#if STACJA_HEADLESS
// ---------------------------------------------------------------------------
//  Diagnostyka węzła bez sieci
//
//  Węzeł nie ma strony www, MQTT ani konsoli sieciowej - stan magistrali
//  widać wyłącznie w logu na USB. Bez tego nie da się odróżnić trzech
//  najczęstszych przyczyn braku łączności: master nie pyta, przewody RX/TX
//  są proste zamiast skrzyżowane, albo pytania dochodzą, a odpowiedź nie.
// ---------------------------------------------------------------------------
// Poziom linii mierzymy wyłącznie podciągiem wewnętrznym. pinMode() było tu
// błędem: przejmowało pad odbiornikowi UART0 (RX magistrali), więc po pierwszym
// takim pomiarze węzeł przestawał odbierać cokolwiek - aż do restartu. Zmiana
// podciągu (gpio_set_pull_mode) nie rusza ani kierunku pinu, ani przypisania
// do UART-u, więc port szeregowy pracuje dalej.
static int nodeLineLevel(int8_t pin, int* driven) {
  if (pin < 0) return -1;
  gpio_set_pull_mode((gpio_num_t)pin, GPIO_PULLUP_ONLY);
  delayMicroseconds(300);
  int hi = 0;
  for (int i = 0; i < 100; i++) {
    hi += digitalRead(pin) ? 1 : 0;
    delayMicroseconds(20);
  }
  if (driven) {
    // Podciąg w dół: linia sterowana przez drugą stronę (nadajnik mastera)
    // zostaje wysoko, a wolny przewód opadnie od razu. To odróżnia "przewód
    // nie łączy" od "ktoś tu nadaje".
    gpio_set_pull_mode((gpio_num_t)pin, GPIO_PULLDOWN_ONLY);
    delayMicroseconds(300);
    int lo = 0;
    for (int i = 0; i < 100; i++) {
      lo += digitalRead(pin) ? 0 : 1;
      delayMicroseconds(20);
    }
    *driven = (lo < 80) ? 1 : 0;
  }
  gpio_set_pull_mode((gpio_num_t)pin, GPIO_FLOATING);
  return hi;
}

static void nodeBusDiag(uint8_t addr, const Rs485Stats& st, int8_t rxPin, int8_t txPin) {
  static uint32_t lastMs = 0;
  static uint32_t lastRx = 0, lastTx = 0, lastCrc = 0, lastFrame = 0, lastRaw = 0;
  static uint8_t  quietLogged = 0;

  const bool moved = (st.rx != lastRx || st.tx != lastTx ||
                      st.errCrc != lastCrc || st.errFrame != lastFrame ||
                      st.rawRx != lastRaw);
  uint32_t now = millis();
  if (lastMs != 0) {
    uint32_t waited = now - lastMs;
    if (waited < (moved ? 10000UL : 30000UL)) return;
  }
  lastMs = now;

  if (!moved) {
    // Cisza: nikt nie pyta albo przewody nie łączą.
    if (st.rx == 0 && st.rawRx == 0 && quietLogged < 5) {
      quietLogged++;
      int driven = -1;
      int lvl = nodeLineLevel(rxPin, &driven);
      LOG_W("RS485: węzeł %u - cisza na RX %d (0 bajtów), poziom linii RX: %d/100 (0 = linia w GND, 100 = wolna), ktoś nią steruje: %d",
            (unsigned)addr, (int)rxPin, lvl, driven);
      LOG_W("RS485: sprawdź, czy RX %d idzie do TX mastera, a TX %d do jego RX (+ GND)",
            (int)rxPin, (int)txPin);
    }
    return;
  }

  lastRx = st.rx; lastTx = st.tx; lastCrc = st.errCrc; lastFrame = st.errFrame; lastRaw = st.rawRx;
  quietLogged = 0;

  if (st.rawRx != 0 && st.rx == 0) {
    // Bajty dochodzą, ale żadna ramka z nich nie wychodzi - przewody są
    // sprawne, problem jest w sygnale (baud, odwrócenie) albo w tym, że na
    // linii nadaje ktoś inny. Dlatego pokazujemy pierwsze bajty.
    LOG_W("RS485: węzeł %u - bajty na RX %d: %lu, ale 0 poprawnych ramek (nagłówki %lu, CRC %lu) - pierwsze bajty: %s",
          (unsigned)addr, (int)rxPin, (unsigned long)st.rawRx,
          (unsigned long)st.errFrame, (unsigned long)st.errCrc, sniffHex(st).c_str());
    return;
  }

  LOG_I("RS485: węzeł %u - ramki odebrane %lu, odpowiedzi %lu, błędne CRC %lu, złe ramki %lu, surowe bajty %lu",
        (unsigned)addr, (unsigned long)st.rx, (unsigned long)st.tx,
        (unsigned long)st.errCrc, (unsigned long)st.errFrame, (unsigned long)st.rawRx);
}
#endif

// ---------------------------------------------------------------
//  Narzędzia pomocnicze
// ---------------------------------------------------------------

// Tekst bezpieczny dla JSON-a (cudzysłowy i znaki sterujące)
static String jsonEsc(const String& in) {
  String out;
  out.reserve(in.length() + 8);
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    if (c == '"' || c == '\\') { out += '\\'; out += c; }
    else if (c == '\n') out += "\\n";
    else if (c == '\r') out += "\\r";
    else if (c == '\t') out += "\\t";
    else if ((uint8_t)c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", (unsigned)(uint8_t)c); out += b; }
    else out += c;
  }
  return out;
}

// Odpowiedź węzła: {"ok":bool,"err":"...","data":{...}}
static String respJson(bool ok, const String& err, const String& data = "") {
  String s = "{\"ok\":";
  s += ok ? "true" : "false";
  s += ",\"err\":\"";
  s += jsonEsc(err);
  s += "\"";
  if (data.length()) { s += ",\"data\":"; s += data; }
  s += "}";
  return s;
}

// Wartość tekstowa z JSON-a (pusta, gdy klucza nie ma)
static String sv(JsonVariantConst v) {
  if (v.isNull()) return String();
  return v.as<String>();
}

const char* Rs485Bus::roleName() const {
  if (!cfgEnabled_) return "wyłączona";
  return cfgMaster_ ? "master" : "węzeł";
}

// ---------------------------------------------------------------
//  Start / zatrzymanie magistrali
// ---------------------------------------------------------------
void Rs485Bus::begin() {
  reload();

  if (mtx_ == nullptr) mtx_ = xSemaphoreCreateMutex();

  if (txBuf_ == nullptr) {
    txBuf_ = (uint8_t*)heap_caps_malloc(RS485_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (txBuf_ == nullptr) txBuf_ = (uint8_t*)malloc(RS485_BUF_SIZE);
  }
  if (rxBuf_ == nullptr) {
    rxBuf_ = (uint8_t*)heap_caps_malloc(RS485_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (rxBuf_ == nullptr) rxBuf_ = (uint8_t*)malloc(RS485_BUF_SIZE);
  }
  if (txBuf_ == nullptr || rxBuf_ == nullptr) {
    LOG_E("RS485: brak pamięci na bufory ramek (%u B) - magistrala nieaktywna "
          "(wolne %u kB, największy blok %u kB)",
          (unsigned)RS485_BUF_SIZE, (unsigned)(ESP.getFreeHeap() / 1024),
          (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024));
    stopPort();
    return;
  }

  if (cfgEnabled_) {
    LOG_I("RS485: %s, adres %u, węzłów %d, %u b/s, piny RX %d TX %d DE %d",
          cfgMaster_ ? "master" : "węzeł", (unsigned)cfgAddr_, nodeCnt_,
          (unsigned)cfgBaud_, (int)pinRx_, (int)pinTx_, (int)pinDe_);
    if (cfgMaster_ && nodeCnt_ > 0) {
      String list;
      for (int i = 0; i < nodeCnt_; i++) {
        if (i) list += ", ";
        list += String((unsigned)nodes_[i].addr);
      }
      LOG_I("RS485: odpytywane adresy: %s", list.c_str());
    } else if (cfgMaster_) {
      LOG_I("RS485: lista węzłów pusta - master szuka sam");
    }
    if (cfgMaster_) nextPollMs_ = 0;
  }
}

void Rs485Bus::reload() {
  bool     wasEnabled = cfgEnabled_;
  bool     wasStarted = started_;
  uint32_t oldBaud = cfgBaud_;
  int8_t   oldRx = pinRx_, oldTx = pinTx_, oldDe = pinDe_;

  cfgEnabled_   = config.rs485Enabled();
  cfgMaster_    = config.rs485Master();
  cfgAddr_      = config.rs485Addr();
#if STACJA_HEADLESS
  // Firmware węzła bez sieci ma zawsze pracować jako węzeł i zawsze odpowiadać
  // na magistrali - nie ma strony www, więc nie ma jak tego włączyć ręcznie,
  // a po wyczyszczeniu NVS master w ogóle by go nie zobaczył. Adres 1 należy do
  // mastera, więc węzeł nigdy go nie dostaje (0 też nie jest poprawny).
  cfgEnabled_ = true;
  cfgMaster_  = false;
  if (cfgAddr_ < 2) cfgAddr_ = STACJA_NODE_ADDR;
#endif
  cfgPeer_      = config.rs485Peer();
#if STACJA_HEADLESS
  // Prędkość: domyślnie wpisana w firmware (STACJA_NODE_BAUD), żeby świeży
  // moduł od razu działał na tej samej prędkości co master. Jeśli użytkownik
  // zmienił ją w awaryjnym AP węzła (rs485_baud_manual), to ta wartość ma
  // pierwszeństwo - inaczej zmiana z AP nie miałaby skutku.
  cfgBaud_      = config.rs485BaudManual() ? config.rs485Baud() : STACJA_NODE_BAUD;
#else
  cfgBaud_      = config.rs485Baud();
#endif
  cfgPollS_     = config.rs485PollS();
  cfgTimeoutMs_ = config.rs485TimeoutMs();
  cfgRetries_   = config.rs485Retries();
  pinRx_ = (int8_t)pinMap.pin("rs485_rx");
  pinTx_ = (int8_t)pinMap.pin("rs485_tx");
  pinDe_ = (int8_t)pinMap.pin("rs485_de");
  nodesLoad();   // lista odpytywanych węzłów z ustawień

  if (!cfgEnabled_) {
    stopPort();
    sensors.remoteClear();   // kanały węzłów nie mogą zostać na pulpicie
    if (wasEnabled) LOG_I("RS485: magistrala wyłączona");
    return;
  }

  // Węzeł nie odbija cudzych kanałów, więc po zmianie roli czyścimy lustra.
  if (!cfgMaster_) sensors.remoteClear();

  // Port trzeba otworzyć od nowa po zmianie prędkości lub pinów
  if (started_ && (cfgBaud_ != oldBaud || pinRx_ != oldRx || pinTx_ != oldTx || pinDe_ != oldDe)) {
    stopPort();
  }
  if (wasStarted && !started_ && cfgBaud_ != oldBaud) {
    LOG_I("RS485: zmiana prędkości na %u b/s", (unsigned)cfgBaud_);
  }
  startPort();
  nextPollMs_ = 0;   // po zmianie ustawień odpyujemy węzeł od razu
  // Zmiana adresu węzła (albo wpisanie 0 = wykryj sam) - szukamy od nowa.
  failStreak_ = 0;
  nextDiscoverMs_ = 0;
}

void Rs485Bus::startPort() {
  if (started_) return;
  if (pinRx_ < 0 || pinTx_ < 0) {
    LOG_W("RS485: brak pinów RX/TX w mapie pinów - magistrala nieaktywna");
    return;
  }

  // DE (kierunek nadawania) - przy module z automatycznym kierunkiem pin = -1
  if (pinDe_ >= 0) {
    pinMode(pinDe_, OUTPUT);
    digitalWrite(pinDe_, LOW);   // nasłuch
  }

  // Logi tej stacji idą przez natywne USB (ARDUINO_USB_CDC_ON_BOOT=1), więc
  // poniższe wywołanie zostawia konsolę na USB i odłącza teksty jądra od
  // UART0 - inaczej komunikaty trafiłyby na magistralę RS485 jako śmieci.
  Serial.setDebugOutput(true);

  // Domyślny bufor odbiornika UART ma tylko 256 B, a ramka z danymi węzła
  // może mieć kilka kB - bez powiększenia bufora dane by się gubiły.
  Serial0.setRxBufferSize(RS485_MAX_PAYLOAD + 512);
  Serial0.setDebugOutput(false);
  Serial0.begin(cfgBaud_, SERIAL_8N1, pinRx_, pinTx_);
  started_ = true;
}

void Rs485Bus::stopPort() {
  if (!started_) return;
  Serial0.end();
  started_ = false;
}

// ---------------------------------------------------------------
//  Ramki
// ---------------------------------------------------------------
uint16_t Rs485Bus::crc16(const uint8_t* p, size_t n) {
  uint16_t crc = 0xFFFF;
  while (n--) {
    crc ^= (uint16_t)(*p++) << 8;
    for (uint8_t i = 0; i < 8; i++) {
      crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
  }
  return crc;
}

size_t Rs485Bus::buildFrame(uint8_t dst, uint8_t type, uint16_t seq, const String& payload) {
  if (txBuf_ == nullptr) return 0;
  size_t flen = payload.length();
  if (flen > RS485_MAX_PAYLOAD) return 0;

  uint8_t* b = txBuf_;
  b[0] = 0xAA;
  b[1] = 0x55;
  b[2] = RS485_PROTO_VER;
  b[3] = dst;
  b[4] = cfgAddr_;
  b[5] = type;
  b[6] = (uint8_t)(seq & 0xFF);
  b[7] = (uint8_t)(seq >> 8);
  b[8] = (uint8_t)(flen & 0xFF);
  b[9] = (uint8_t)(flen >> 8);
  if (flen) memcpy(b + 10, payload.c_str(), flen);

  uint16_t crc = crc16(b + 2, 8 + flen);
  b[10 + flen] = (uint8_t)(crc & 0xFF);
  b[11 + flen] = (uint8_t)(crc >> 8);
  return 12 + flen;
}

void Rs485Bus::frameFeed(Parser& p, uint8_t* buf, uint8_t b, Resp& out, Rs485Stats* stats) {
  Rs485Stats& S = stats ? *stats : st_;
  rawByte(S, b);
  out.ok = false;

  if (p.pos == 0) {
    if (b != 0xAA) return;
    buf[0] = b;
    p.pos = 1;
    return;
  }
  if (p.pos == 1) {
    if (b != 0x55) {
      p.pos = 0;
      if (b == 0xAA) { buf[0] = b; p.pos = 1; }   // ta ramka zaczyna się tutaj
      return;
    }
    buf[1] = b;
    p.pos = 2;
    return;
  }

  if (p.pos >= RS485_BUF_SIZE) {   // zabezpieczenie - nigdy nie powinniśmy tu trafić
    S.errFrame++;
    p.pos = 0;
    return;
  }

  buf[p.pos++] = b;

  if (p.pos == 10) {
    p.ver  = buf[2];
    p.dst  = buf[3];
    p.src  = buf[4];
    p.type = buf[5];
    p.seq  = (uint16_t)buf[6] | ((uint16_t)buf[7] << 8);
    p.flen = (uint16_t)buf[8] | ((uint16_t)buf[9] << 8);
    if (p.ver != RS485_PROTO_VER || p.flen > RS485_MAX_PAYLOAD) {
      S.errFrame++;
      p.pos = 0;
      return;
    }
  }

  if (p.pos >= 12 && p.pos == (uint16_t)(10 + p.flen + 2)) {
    uint16_t rx = (uint16_t)buf[10 + p.flen] | ((uint16_t)buf[11 + p.flen] << 8);
    uint16_t calc = crc16(buf + 2, (size_t)8 + p.flen);
    p.pos = 0;
    if (rx != calc) {
      S.errCrc++;
      out.err = "błędne CRC ramki";
      return;
    }
    S.rx++;
    out.ok = true;
    out.src = p.src;
    out.dst = p.dst;
    out.type = p.type;
    out.seq = p.seq;
    out.payload = "";
    if (p.flen) out.payload.concat((const char*)(buf + 10), p.flen);
  }
}

void Rs485Bus::writeTx(const uint8_t* p, size_t n) {
  if (!started_ || n == 0) return;
  if (pinDe_ >= 0) digitalWrite(pinDe_, HIGH);
  Serial0.write(p, n);
  Serial0.flush();
  if (pinDe_ >= 0) {
    // flush() opróżnia kolejkę nadajnika, ale ostatni znak siedzi jeszcze
    // w rejestrze przesuwnym - zanim zwolnimy DE, trzeba go wysłać.
    uint32_t oneChar = 10000000UL / (cfgBaud_ ? cfgBaud_ : 9600);
    delayMicroseconds(oneChar + 20);
    digitalWrite(pinDe_, LOW);
  }
}

bool Rs485Bus::readResponse(uint32_t timeoutMs, uint16_t wantSeq, Resp& out, String& err,
                            Rs485Stats* stats) {
  Rs485Stats& S = stats ? *stats : st_;
  Resp    r;
  Parser  p;                       // własny parser - nie miesza się z węzłowym
  uint32_t first = timeoutMs ? timeoutMs : cfgTimeoutMs_;
  uint32_t t0 = millis();
  uint32_t deadline = t0 + first;
  // Limit czasu na całą ramkę musi zależeć od prędkości: przy 19200 b/s największa
  // ramka (nagłówek 12 B + payload do RS485_MAX_PAYLOAD) leci ~4,3 s. Sztywny limit
  // first*3+400 = 1,6 s ucinał poprawną, ale dużą odpowiedź w połowie i zgłaszał
  // "odpowiedź węzła za długa" (rx=0, ok=0) mimo braku błędów CRC na węźle.
  uint32_t frameMs = (uint32_t)(((uint64_t)(RS485_MAX_PAYLOAD + 12) * 10000UL) /
                                (cfgBaud_ ? cfgBaud_ : 9600)) + 100;
  uint32_t hardLimit = t0 + first + frameMs;
  uint32_t lastByte = t0;
  bool gotAny = false;

  while (true) {
    while (Serial0.available()) {
      uint8_t b = (uint8_t)Serial0.read();
      gotAny = true;
      lastByte = millis();
      frameFeed(p, rxBuf_, b, r, stats);
      if (!r.ok) continue;
      if (r.type == 0x81 && r.seq == wantSeq && r.dst == cfgAddr_) {
        out = r;
        out.ok = true;
        S.latencyMs = millis() - t0;
        return true;
      }
      r.ok = false;   // ramka do kogoś innego - ignorujemy
    }

    uint32_t now = millis();
    if (!gotAny) {
      if ((int32_t)(now - deadline) >= 0) {
        err = "brak odpowiedzi węzła";
        S.errTimeout++;
        S.lastErrMs = now;
        S.lastError = err;
        return false;
      }
    } else if ((int32_t)(now - lastByte) >= 80) {
      // Cisza na magistrali = odpowiedź urwana (węzeł nie dokończył ramki)
      err = "przerwana odpowiedź węzła";
      S.errTimeout++;
      S.lastErrMs = now;
      S.lastError = err;
      return false;
    }
    if ((int32_t)(now - hardLimit) >= 0) {
      err = "odpowiedź węzła za długa";
      S.errTimeout++;
      S.lastErrMs = now;
      S.lastError = err;
      return false;
    }
    delay(1);   // oddajemy czas innym zadaniom (Wi-Fi, strona www)
  }
}

bool Rs485Bus::request(const String& cmd, const String& argsJson, JsonDocument& out, String& err,
                       uint32_t timeoutMs) {
  return requestTo(0, cmd, argsJson, out, err, timeoutMs);
}

bool Rs485Bus::requestTo(uint8_t dst, const String& cmd, const String& argsJson, JsonDocument& out,
                         String& err, uint32_t timeoutMs) {
  err = "";
  if (!cfgEnabled_ || !started_) { err = "magistrala RS485 jest wyłączona"; return false; }
  if (!cfgMaster_) { err = "ta stacja pracuje jako węzeł"; return false; }
  if (mtx_ == nullptr) { err = "brak blokady magistrali"; return false; }

  // Adres odbiorcy: wymuszony przez wywołującego (strona www pyta wybrany
  // węzeł), adres trwającego skanu, węzeł odpytywany teraz, a na końcu adres
  // z ustawień (instalacje z jednym węzłem).
  if (dst == 0) dst = scanAddr_ ? scanAddr_ : (reqAddr_ ? reqAddr_ : cfgPeer_);

  // Statystyki liczymy temu węzłowi, do którego naprawdę pytamy - zapytanie
  // z www skierowane do innego modułu nie może zafałszować liczników.
  int tgt = nodeIndexByAddr(dst);
  Rs485Stats* st = (tgt >= 0 && tgt != loadedIdx_) ? &nodes_[tgt].st : &st_;

  if (xSemaphoreTake(mtx_, pdMS_TO_TICKS(2500)) != pdTRUE) { err = "magistrala zajęta"; return false; }

  String pl = "{\"c\":\"";
  pl += jsonEsc(cmd);
  pl += "\"";
  if (argsJson.length()) { pl += ",\"a\":"; pl += argsJson; }
  pl += "}";

  // Bez łączności nie powtarzamy prób - inaczej loop() blokuje się na
  // kilka sekund, gdy drugi ESP jest wyłączony.
  uint8_t tries = (dst && nodeOnline(dst)) ? (uint8_t)(cfgRetries_ + 1) : 1;

  bool got = false;
  Resp r;
  for (uint8_t t = 0; t < tries && !got; t++) {
    if (t) { st->retries++; delay(20); }
    // Stare resztki odrzucamy, ale nadal liczymy jako surowe bajty - inaczej
    // to, co przyszło między wymianami, zniknęłoby bez żadnego śladu.
    while (Serial0.available()) {
      uint8_t stale = (uint8_t)Serial0.read();
      rawByte(*st, stale);
    }
    seq_++;
    size_t n = buildFrame(dst, 0x01, seq_, pl);
    if (n == 0) { err = "zapytanie za duże (limit " + String(RS485_MAX_PAYLOAD) + " B)"; break; }
    writeTx(txBuf_, n);
    st->tx++;
    if (readResponse(timeoutMs, seq_, r, err, st)) got = true;
  }

  xSemaphoreGive(mtx_);
  if (!got) return false;

  // Pełna odpowiedź {"ok":..,"err":..,"data":{..}} trafia do dokumentu
  // wywołującego - dzięki temu dane są już sparsowane.
  DeserializationError e = deserializeJson(out, r.payload);
  if (e) { err = String("błędna odpowiedź: ") + e.c_str(); return false; }

  st->ok++;
  st->lastOkMs = millis();
  st->lastError = "";
  // Świeżość wpisu w liście węzłów czyta się z nodes_[i].lastOkMs, a nie ze
  // statystyk zestawu roboczego - trzeba je trzymać razem.
  if (tgt >= 0) nodes_[tgt].lastOkMs = st->lastOkMs;

  bool ok = out["ok"] | false;
  if (!ok) {
    err = sv(out["err"]);
    if (!err.length()) err = "węzeł zgłosił błąd";
    return false;
  }
  return true;
}

bool Rs485Bus::online() const {
  // Gdy master zna listę węzłów, "online" znaczy "choć jeden odpowiada".
  if (nodeCnt_ > 0) {
    for (int i = 0; i < nodeCnt_; i++) {
      if (nodeFresh(nodes_[i])) return true;
    }
    return false;
  }
  return staleOk(st_.lastOkMs, cfgPollS_);
}

bool Rs485Bus::nodeFresh(const Rs485Node& nd) const {
  uint32_t t = (nd.addr == activeAddr_ && st_.lastOkMs) ? st_.lastOkMs : nd.lastOkMs;
  return staleOk(t, cfgPollS_);
}

bool Rs485Bus::nodeOnline(uint8_t addr) const {
  int i = nodeIndexByAddr(addr);
  return i >= 0 ? nodeFresh(nodes_[i]) : false;
}

uint32_t Rs485Bus::cacheAgeS() const {
  if (lastSyncMs_ == 0) return 0;
  return (millis() - lastSyncMs_) / 1000;
}

void Rs485Bus::pollNow() {
  nextPollMs_ = 0;
  LOG_I("RS485: odpytywanie węzła na żądanie");
}

// ---------------------------------------------------------------
//  Master
// ---------------------------------------------------------------
void Rs485Bus::loop() {
  if (!cfgEnabled_ || !started_) return;

  if (!cfgMaster_) {
    handleSlave();
#if STACJA_HEADLESS
    nodeBusDiag(cfgAddr_, st_, pinRx_, pinTx_);
#endif
    return;
  }

  uint32_t now = millis();
  if (nextPollMs_ == 0) nextPollMs_ = now;
  if ((int32_t)(now - nextPollMs_) >= 0) {
    // Przy kilku węzłach dzielimy okno odpytywania na równe odstępy, żeby
    // każdy moduł doczekał się swojego pytania.
    uint32_t gap = (uint32_t)cfgPollS_ * 1000UL / (nodeCnt_ > 0 ? (uint32_t)nodeCnt_ : 1UL);
    if (gap < RS485_MIN_POLL_GAP_MS) gap = RS485_MIN_POLL_GAP_MS;
    if (scanAddr_ || !nodeCnt_) gap = 1000;   // skan / brak listy: nie czekamy pół minuty
    nextPollMs_ = now + gap;
    masterSync();
  }
}

// Wybiera węzeł, który najdłużej czekał na odpytywanie (kolejka okrężna).
int Rs485Bus::pickNode() {
  int best = -1;
  uint32_t bestT = 0;
  for (int i = 0; i < nodeCnt_; i++) {
    // Wpis odpytywany teraz ma świeższy znacznik w zestawie roboczym.
    uint32_t t = (i == loadedIdx_ && lastSyncMs_) ? lastSyncMs_ : nodes_[i].lastSyncMs;
    if (best < 0 || (int32_t)(t - bestT) < 0) { best = i; bestT = t; }
  }
  return best;
}

// Skanowanie adresów w tle. Bez tego master nie zauważy nowego modułu, dopóki
// ktoś nie dopisze go ręcznie na stronie www.
void Rs485Bus::discoverPass(uint32_t now) {
  bool need = (nodeCnt_ == 0) || (cfgPeer_ == 0) || (failStreak_ >= 2);
  if (!need) return;
  if ((int32_t)(now - nextDiscoverMs_) < 0) return;

  for (uint8_t n = 0; n < RS485_DISCOVER_STEP; n++) {
    uint8_t a = discoverNext_++;
    if (a > RS485_SCAN_MAX_ADDR) {
      discoverNext_ = 1;
      if (discoverMiss_ < 32) discoverMiss_++;
      break;
    }
    if (!nodesValidAddr(a) || a == cfgAddr_ || nodeIndexByAddr(a) >= 0) continue;
    if (pingAddr(a, RS485_PING_TIMEOUT_MS)) {
      adoptPeer(a);
      discoverMiss_ = 0;
      discoverNext_ = 1;
      nextDiscoverMs_ = 0;
      lastNoPeerLogMs_ = 0;
      failStreak_ = 0;
      return;
    }
  }

  // Gdy na magistrali nikogo nie ma, nie ma sensu pytać o kolejne adresy co
  // pół minuty - odstęp rośnie z każdym przebiegiem bez odpowiedzi.
  uint32_t gap = RS485_DISCOVER_GAP_MS * (uint32_t)(discoverMiss_ + 1);
  if (gap > RS485_DISCOVER_MAX_GAP_MS) gap = RS485_DISCOVER_MAX_GAP_MS;
  nextDiscoverMs_ = now + gap;
  if (nodeCnt_ == 0 && (lastNoPeerLogMs_ == 0 ||
                        (uint32_t)(now - lastNoPeerLogMs_) > RS485_NOPEER_LOG_MS)) {
    lastNoPeerLogMs_ = now;
    LOG_W("RS485: nie widzę żadnego węzła (adresy 2-%u) - sprawdź przewody A-A, B-B, GND",
          (unsigned)RS485_SCAN_MAX_ADDR);
  }
}

void Rs485Bus::masterSync() {
  if (scanAddr_) return;   // trwa skanowanie magistrali

  discoverPass(millis());

  int idx = pickNode();
  if (idx < 0) {
    // Lista pusta - nikt jeszcze nie odpowiedział; discoverPass() szuka dalej.
    lastSyncMs_ = millis();
    return;
  }
  syncNode(idx);
}

// Odpytywanie jednego węzła: zestaw roboczy (peer_, st_, ...) przestawiamy na
// jego wpis, a po wymianie zapisujemy z powrotem.
void Rs485Bus::syncNode(int idx) {
  nodeSelect(idx);
  uint8_t addr = activeAddr_;
  if (!addr) return;

  bool wasOnline = staleOk(st_.lastOkMs, cfgPollS_);
  sensors.remoteBegin(addr);   // bez łączności kanały tego węzła pokazują "—"

  PsramAllocator alloc;
  JsonDocument d(&alloc);
  String err;
  bool ok = request("sync", "", d, err, RS485_BIG_TIMEOUT_MS);

  if (ok && d["data"].is<JsonObject>()) {
    JsonObject data = d["data"].as<JsonObject>();
    applyPeerStatus(data);
    if (data["chans"].is<JsonArray>()) applyPeerChannels(data["chans"].as<JsonArray>());

    // Kanałów własnych jest już ponad setkę, więc "sync" wiezie tylko początek
    // listy. Resztę metadanych dociągamy stronami polecenia "chan", a wartości
    // odświeżamy krótkimi ramkami "vals". Bez tego na pulpicie mastera byłaby
    // tylko część czujników węzła.
    uint32_t total = data["channels"] | 0UL;
    uint32_t sent  = data["sent"] | 0UL;
    if (sent == 0 && data["chans"].is<JsonArray>()) sent = data["chans"].size();
    uint32_t known = sent;
    if (peerMetaTotal_ == total && peerMetaFrom_ > known) known = peerMetaFrom_;
    peerMetaTotal_ = total;
    peerMetaFrom_ = known;

    if (known < total || !peerValsOk_) {
      fetchPeerMeta(known < total ? known : 0);
    } else if (total) {
      fetchPeerValues();
    }
    if (data["trunc"] | false) {
      LOG_W("RS485: węzeł %u ma %u kanałów - %u w pierwszej ramce, resztę dociągam na raty",
            (unsigned)addr, (unsigned)total, (unsigned)sent);
    }
    // Węzeł bez Wi-Fi i bez RTC nie ma skąd wziąć czasu - przekazujemy go,
    // dopóki zegar węzła nie jest ustawiony.
    if (peer_.epoch < 1000000000L) pushTimeToPeer();
  }

  if (ok && !wasOnline) {
    LOG_I("RS485: węzeł %u online (%s, %u kanałów, %u potwierdzonych)", (unsigned)addr,
          peer_.name.c_str(), (unsigned)peer_.channels, (unsigned)peer_.detected);
  } else if (!ok && wasOnline) {
    LOG_W("RS485: brak łączności z węzłem %u (%s)", (unsigned)addr, err.c_str());
  } else if (!ok && st_.lastOkMs == 0 && cfgPollS_ > 0 && nodes_[idx].failStreak == 0) {
    // Zimny start bez węzła - jedno ostrzeżenie na węzeł, żeby nie zaśmiecać logów
    LOG_W("RS485: węzeł %u nie odpowiada (%s) - sprawdź przewody i adres",
          (unsigned)addr, err.c_str());
  }

  // Po dwóch nieudanych próbach discoverPass() przeszuka adresy jeszcze raz:
  // węzeł mógł zmienić adres albo właśnie został wymieniony.
  if (ok) {
    failStreak_ = 0;
  } else if (failStreak_ < 200) {
    failStreak_++;
  }
  if (!ok && failStreak_ >= 2) nextDiscoverMs_ = 0;
  lastSyncMs_ = millis();
  nodeSave(idx);
}

// Pojedyncze zapytanie "ping" do wskazanego adresu. scanAddr_ sprawia, że
// zwykła synchronizacja czeka, aż skan/dopasowanie adresu się skończy.
bool Rs485Bus::pingAddr(uint8_t addr, uint32_t timeoutMs) {
  scanAddr_ = addr;
  PsramAllocator alloc;
  JsonDocument d(&alloc);
  String err;
  bool ok = request("ping", "", d, err, timeoutMs);
  scanAddr_ = 0;
  return ok;
}

void Rs485Bus::adoptPeer(uint8_t addr) {
  if (!nodesValidAddr(addr) || addr == cfgAddr_) return;

  int idx = nodeIndexByAddr(addr);
  if (idx < 0) {
    // Wpis z ustawień (domyślnie adres 2), który nigdy nie odpowiedział,
    // oddajemy wykrytemu modułowi - inaczej po pierwszym uruchomieniu lista
    // zapełniałaby się adresami widmami.
    if (nodeCnt_ == 1 && nodes_[0].addr == cfgPeer_ && nodes_[0].lastOkMs == 0 &&
        nodes_[0].failStreak >= 2) {
      if (loadedIdx_ == 0) {
        peer_ = Rs485PeerInfo();
        st_ = Rs485Stats();
        peerMetaFrom_ = peerMetaTotal_ = 0;
        peerValsOk_ = true;
        loadedIdx_ = -1;
        activeAddr_ = reqAddr_ = 0;
      }
      nodes_[0] = Rs485Node();
      nodes_[0].addr = addr;
      idx = 0;
    } else {
      if (nodeCnt_ >= RS485_MAX_NODES) {
        LOG_W("RS485: wykryto węzeł %u, ale lista jest pełna (%d)", (unsigned)addr,
              (int)RS485_MAX_NODES);
        return;
      }
      nodes_[nodeCnt_] = Rs485Node();
      nodes_[nodeCnt_].addr = addr;
      idx = nodeCnt_++;
    }
  }

  if (cfgPeer_ == 0) cfgPeer_ = addr;   // starsze pole "adres drugiego ESP"
  nodesSave();
  failStreak_ = 0;
  nextDiscoverMs_ = 0;
  discoverMiss_ = 0;
  lastNoPeerLogMs_ = 0;
  if (loadedIdx_ < 0) nodeLoad(idx);
  pollNow();
  LOG_I("RS485: wykryto węzeł %u (wpis %d, węzłów: %d)", (unsigned)addr, idx + 1, nodeCnt_);
}

// ---------------------------------------------------------------
//  Lista węzłów (kilka modułów na jednej magistrali)
// ---------------------------------------------------------------
bool Rs485Bus::nodesValidAddr(uint8_t addr) { return addr >= 2 && addr <= 247; }

// Ten sam format MAC dla mastera (Wi-Fi) i dla węzła pracującego bez Wi-Fi
// (MAC z eFuse) - dzięki temu porównanie adresu sprzętowego w "assignAddr"
// działa po obu stronach magistrali.
String Rs485Bus::deviceMac() {
#if !STACJA_HEADLESS
  String m = WiFi.macAddress();
  if (m.length() == 17 && m != "00:00:00:00:00:00") return m;
#endif
  uint64_t mac = ESP.getEfuseMac();
  char buf[18];
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", (unsigned)((mac >> 40) & 0xFF),
           (unsigned)((mac >> 32) & 0xFF), (unsigned)((mac >> 24) & 0xFF),
           (unsigned)((mac >> 16) & 0xFF), (unsigned)((mac >> 8) & 0xFF), (unsigned)(mac & 0xFF));
  return String(buf);
}

const Rs485Node& Rs485Bus::node(int idx) const {
  if (idx < 0 || idx >= nodeCnt_) idx = 0;
  return nodes_[idx];
}

int Rs485Bus::nodeIndexByAddr(uint8_t addr) const {
  if (addr == 0) return -1;
  for (int i = 0; i < nodeCnt_; i++) {
    if (nodes_[i].addr == addr) return i;
  }
  return -1;
}

// Lista adresów z ustawień ("3,4,5") -> wpisy węzłów. Kolejność już znanych
// węzłów zostaje (wraz ze statystykami), żeby po zapisie ustawień nic nie
// "przeskakiwało" na stronie www.
void Rs485Bus::nodesLoad() {
  if (!cfgMaster_) {   // na węźle ta lista nie ma sensu
    nodeCnt_ = 0;
    loadedIdx_ = -1;
    activeAddr_ = reqAddr_ = 0;
    return;
  }

  nodeSave(loadedIdx_);   // zestaw roboczy do wpisu, zanim przestawimy listę

  uint8_t list[RS485_MAX_NODES];
  uint8_t n = 0;
  String src = config.rs485Nodes();
  int i = 0;
  while (i < (int)src.length() && n < RS485_MAX_NODES) {
    while (i < (int)src.length() && !isDigit(src[i])) i++;
    if (i >= (int)src.length()) break;
    long v = 0;
    while (i < (int)src.length() && isDigit(src[i])) v = v * 10 + (src[i++] - '0');
    uint8_t a = (uint8_t)v;
    if (!nodesValidAddr(a) || a == cfgAddr_) continue;
    bool dup = false;
    for (uint8_t k = 0; k < n; k++) {
      if (list[k] == a) { dup = true; break; }
    }
    if (!dup) list[n++] = a;
  }
  if (n == 0 && nodesValidAddr(cfgPeer_)) list[n++] = cfgPeer_;

  for (uint8_t i2 = 0; i2 < n; i2++) {
    if (nodes_[i2].addr != list[i2]) {
      int j = nodeIndexByAddr(list[i2]);
      if (j > (int)i2) {
        Rs485Node tmp = nodes_[j];
        nodes_[j] = nodes_[i2];
        nodes_[i2] = tmp;
      } else {
        nodes_[i2] = Rs485Node();   // wpis nieznanego węzła
      }
    }
    nodes_[i2].addr = list[i2];
  }
  for (uint8_t i2 = n; i2 < RS485_MAX_NODES; i2++) nodes_[i2] = Rs485Node();
  nodeCnt_ = n;

  int keep = nodeIndexByAddr(activeAddr_);
  if (keep >= 0) {
    loadedIdx_ = keep;   // dane robocze nadal opisują ten sam adres
  } else if (n > 0) {
    nodeLoad(0);
  } else {
    peer_ = Rs485PeerInfo();
    st_ = Rs485Stats();
    peerMetaFrom_ = peerMetaTotal_ = 0;
    peerValsOk_ = true;
    failStreak_ = 0;
    lastSyncMs_ = 0;
    loadedIdx_ = -1;
    activeAddr_ = reqAddr_ = 0;
  }
}

bool Rs485Bus::nodesSave() {
  String s;
  for (int i = 0; i < nodeCnt_; i++) {
    if (i) s += ",";
    s += String((unsigned)nodes_[i].addr);
  }
  String js = "{\"rs485_nodes\":\"" + s + "\"}";
  bool ok = config.applyJson(js.c_str(), js.length());
  if (!ok) LOG_W("RS485: nie udało się zapisać listy węzłów (%s)", s.c_str());
  return ok;
}

void Rs485Bus::nodeLoad(int idx) {
  if (idx < 0 || idx >= nodeCnt_) return;
  loadedIdx_ = idx;
  activeAddr_ = reqAddr_ = nodes_[idx].addr;
  peer_ = nodes_[idx].info;
  st_ = nodes_[idx].st;
  st_.lastOkMs = nodes_[idx].lastOkMs;
  peerMetaFrom_ = nodes_[idx].metaFrom;
  peerMetaTotal_ = nodes_[idx].metaTotal;
  peerValsOk_ = nodes_[idx].valsOk;
  failStreak_ = nodes_[idx].failStreak;
  lastSyncMs_ = nodes_[idx].lastSyncMs;
}

void Rs485Bus::nodeSave(int idx) {
  // Zapisujemy tylko wtedy, gdy zestaw roboczy naprawdę należy do tego wpisu,
  // inaczej nadpisalibyśmy dane jednego węzła danymi innego (strona www może
  // w tym czasie pytać wybrany moduł przez requestTo()).
  if (idx < 0 || idx >= nodeCnt_ || loadedIdx_ != idx) return;
  if (nodes_[idx].addr != activeAddr_) return;
  nodes_[idx].info = peer_;
  nodes_[idx].st = st_;
  nodes_[idx].lastOkMs = st_.lastOkMs;
  nodes_[idx].lastSyncMs = lastSyncMs_;
  nodes_[idx].metaFrom = peerMetaFrom_;
  nodes_[idx].metaTotal = peerMetaTotal_;
  nodes_[idx].valsOk = peerValsOk_;
  nodes_[idx].failStreak = failStreak_;
}

int Rs485Bus::nodeSelect(int idx) {
  if (idx < 0) idx = nodeIndexByAddr(activeAddr_);
  if (idx < 0 && nodeCnt_ > 0) idx = 0;
  if (idx < 0 || idx >= nodeCnt_) return -1;
  if (loadedIdx_ >= 0 && loadedIdx_ != idx) nodeSave(loadedIdx_);
  if (loadedIdx_ != idx) nodeLoad(idx);
  return idx;
}

bool Rs485Bus::addNode(uint8_t addr, String& err) {
  err = "";
  if (!cfgMaster_) { err = "lista węzłów istnieje tylko w masterze"; return false; }
  if (!nodesValidAddr(addr)) { err = "adres musi być z zakresu 2..247"; return false; }
  if (addr == cfgAddr_) { err = "ten adres należy do mastera"; return false; }
  if (nodeIndexByAddr(addr) >= 0) { err = "taki węzeł już jest na liście"; return false; }
  if (nodeCnt_ >= RS485_MAX_NODES) {
    err = "limit " + String((unsigned)RS485_MAX_NODES) + " węzłów na magistrali";
    return false;
  }
  nodes_[nodeCnt_] = Rs485Node();
  nodes_[nodeCnt_].addr = addr;
  nodeCnt_++;
  if (cfgPeer_ == 0) cfgPeer_ = addr;
  nodesSave();
  if (loadedIdx_ < 0) nodeLoad(nodeCnt_ - 1);
  pollNow();
  LOG_I("RS485: dodano węzeł %u (węzłów: %d)", (unsigned)addr, nodeCnt_);
  return true;
}

bool Rs485Bus::removeNode(uint8_t addr, String& err) {
  err = "";
  if (!cfgMaster_) { err = "lista węzłów istnieje tylko w masterze"; return false; }
  int idx = nodeIndexByAddr(addr);
  if (idx < 0) { err = "nie ma takiego węzła na liście"; return false; }

  sensors.remoteClear(addr);   // kanały usuniętego węzła nie mogą zostać na pulpicie

  bool wasWorking = (loadedIdx_ == idx);
  if (wasWorking) nodeSave(idx);   // statystyki wpisu zostają w historii do końca
  for (int i = idx; i < nodeCnt_ - 1; i++) nodes_[i] = nodes_[i + 1];
  nodes_[--nodeCnt_] = Rs485Node();

  if (wasWorking) {
    peer_ = Rs485PeerInfo();
    st_ = Rs485Stats();
    peerMetaFrom_ = peerMetaTotal_ = 0;
    peerValsOk_ = true;
    failStreak_ = 0;
    lastSyncMs_ = 0;
    loadedIdx_ = -1;
    activeAddr_ = reqAddr_ = 0;
    if (nodeCnt_ > 0) nodeLoad(0);
  } else if (loadedIdx_ > idx) {
    loadedIdx_--;
  }
  if (cfgPeer_ == addr) cfgPeer_ = nodeCnt_ ? nodes_[0].addr : 0;
  nodesSave();
  pollNow();
  LOG_I("RS485: usunięto węzeł %u (zostało %d)", (unsigned)addr, nodeCnt_);
  return true;
}

// Nadanie nowego adresu węzłowi (węzeł zapisuje go w swojej pamięci i restartuje
// się). Adres trafia też do listy mastera, żeby od razu był odpytywany.
bool Rs485Bus::assignAddr(uint8_t curAddr, const String& mac, uint8_t newAddr, String& err) {
  err = "";
  if (!cfgMaster_) { err = "adres węzła zmienia tylko master"; return false; }
  if (!nodesValidAddr(curAddr) || curAddr == cfgAddr_) {
    err = "podaj aktualny adres węzła (2..247)";
    return false;
  }
  if (!nodesValidAddr(newAddr)) { err = "nowy adres musi być z zakresu 2..247"; return false; }
  if (newAddr == cfgAddr_) { err = "ten adres należy do mastera"; return false; }
  int clash = nodeIndexByAddr(newAddr);
  if (clash >= 0 && nodes_[clash].addr != curAddr) {
    err = "adres " + String((unsigned)newAddr) + " jest już na liście węzłów";
    return false;
  }

  String args = "{\"addr\":" + String((unsigned)newAddr);
  if (mac.length()) args += ",\"mac\":\"" + jsonEsc(mac) + "\"";
  args += "}";

  PsramAllocator alloc;
  JsonDocument d(&alloc);
  // Węzeł zapisuje nowy adres w NVS i restartuje się, więc na odpowiedź dajemy
  // trzy razy więcej czasu niż normalnie.
  if (!requestTo(curAddr, "addr", args, d, err, (uint32_t)cfgTimeoutMs_ * 3)) return false;

  sensors.remoteClear(curAddr);   // kanały wrócą pod nowym adresem

  int idx = nodeIndexByAddr(curAddr);
  bool wasWorking = (idx >= 0 && loadedIdx_ == idx);
  if (wasWorking) nodeSave(idx);
  if (idx < 0) {
    if (nodeCnt_ >= RS485_MAX_NODES) {
      err = "limit " + String((unsigned)RS485_MAX_NODES) + " węzłów na magistrali";
      return false;
    }
    nodes_[nodeCnt_] = Rs485Node();
    nodes_[nodeCnt_].addr = curAddr;
    idx = nodeCnt_++;
  }
  uint8_t old = nodes_[idx].addr;
  // Nowy adres = nowy węzeł: MAC, liczniki i metadane zbieramy od zera.
  nodes_[idx] = Rs485Node();
  nodes_[idx].addr = newAddr;
  if (wasWorking) {
    activeAddr_ = reqAddr_ = newAddr;
    peer_ = Rs485PeerInfo();
    st_ = Rs485Stats();
    peerMetaFrom_ = peerMetaTotal_ = 0;
    peerValsOk_ = true;
    failStreak_ = 0;
    sensors.remoteBegin(newAddr);
  }
  if (loadedIdx_ < 0) nodeLoad(idx);
  nodesSave();
  failStreak_ = 0;
  nextDiscoverMs_ = 0;
  pollNow();
  LOG_I("RS485: węzeł %u ma teraz adres %u", (unsigned)old, (unsigned)newAddr);
  return true;
}

void Rs485Bus::pushTimeToPeer() {
  time_t now = time(nullptr);
  if (now < 1000000000L) return;   // nasza stacja też nie ma jeszcze czasu z NTP
  PsramAllocator alloc;
  JsonDocument d(&alloc);
  String err;
  String args = "{\"epoch\":" + String((uint32_t)now) + "}";
  if (request("time", args, d, err, cfgTimeoutMs_)) {
    LOG_I("RS485: czas przekazany do węzła (%lu)", (unsigned long)now);
  } else {
    LOG_W("RS485: nie udało się przekazać czasu do węzła (%s)", err.c_str());
  }
}

void Rs485Bus::applyPeerStatus(JsonObject d) {
  peer_.valid      = true;
  peer_.name       = sv(d["name"]);
  peer_.host       = sv(d["host"]);
  peer_.fw         = sv(d["fw"]);
  peer_.build      = sv(d["build"]);
  peer_.mac        = sv(d["mac"]);
  peer_.ip         = sv(d["ip"]);
  peer_.role       = sv(d["role"]);
  peer_.addr       = (uint8_t)(d["addr"] | 0);
  peer_.uptimeS    = (uint32_t)(d["uptime_s"] | 0UL);
  peer_.epoch      = (time_t)(uint32_t)(d["epoch"] | 0UL);
  peer_.rssi       = (int32_t)(d["rssi"] | 0);
  peer_.heap       = (uint32_t)(d["heap"] | 0UL);
  peer_.psram      = (uint32_t)(d["psram"] | 0UL);
  peer_.bootCount  = (uint32_t)(d["boot"] | 0UL);
  peer_.resetReason = sv(d["reset"]);
  peer_.sd         = d["sd"] | false;
  peer_.errors     = (uint32_t)(d["errors"] | 0UL);
  peer_.warns      = (uint32_t)(d["warns"] | 0UL);
  peer_.channels   = (uint32_t)(d["channels"] | 0UL);
  peer_.detected   = (uint32_t)(d["detected"] | 0UL);
  peer_.board      = sv(d["board"]);
  peer_.chip       = sv(d["chip_name"]);
  peer_.flashMb    = (uint32_t)(d["flash_mb"] | 0UL);
  peer_.psramMb    = (uint32_t)(d["psram_mb"] | 0UL);
}

void Rs485Bus::applyPeerChannels(JsonArray arr) {
  for (size_t i = 0; i < arr.size(); i++) {
    JsonObject c = arr[i].as<JsonObject>();
    if (c.isNull()) continue;
    String id = sv(c["id"]);
    if (!id.length() || SensorManager::isRemoteId(id)) continue;   // nie odbijamy własnych luster
    float v = NAN;
    if (!c["v"].isNull()) v = c["v"] | 0.0f;
    sensors.remotePublish(activeAddr_, id, sv(c["n"]), sv(c["u"]), sv(c["z"]), v,
                          c["det"] | false, c["meas"] | false,
                          (int)(c["dec"] | 1), sv(c["hc"]), sv(c["hu"]), sv(c["hi"]));
  }
}

// Same wartości kanałów węzła (odpowiedź na "vals"): [id, wartość, wykryty,
// zmierzony]. Kanał musi już istnieć z metadanych - inaczej pomijamy wpis.
void Rs485Bus::applyPeerValues(JsonArray arr) {
  for (size_t i = 0; i < arr.size(); i++) {
    JsonArray e = arr[i].as<JsonArray>();
    if (e.isNull() || e.size() < 3) continue;
    String id = sv(e[0]);
    if (!id.length() || SensorManager::isRemoteId(id)) continue;
    float v = NAN;
    if (!e[1].isNull()) v = e[1] | 0.0f;
    sensors.remoteValue(activeAddr_, id, v, e[2] | false, e.size() > 3 ? (e[3] | false) : true);
  }
}

// Ile kanałów zmieści się w jednej stronie. Strona musi zmieścić się w jednej
// ramce RS485, a readResponse() po pierwszym bajcie czeka na całą ramkę do
// hardLimit (zależnego od prędkości), więc stronę możemy rozmiarować pełnym
// budżetem ramki zamiast ściskać do czasu transmisji w timeout/2. Przy małych
// stronach lista 179 kanałów wymagałaby ~45 wymian i nie mieściła się w
// RS485_MAX_PAGES (24), przez co www dostawało tylko 96 kanałów.
uint32_t Rs485Bus::pageChans(uint32_t bytesPerChan) const {
  // Strona to najwyżej połowa ramki: przy 19200 b/s pełna ramka (8192 B) leci
  // ~4,3 s i bywała ucinana w połowie, więc celujemy w ~2 s na stronę. To daje
  // też krótsze blokady magistrali i www nie czeka w nieskończoność.
  const uint32_t budget = RS485_MAX_PAYLOAD / 2;
  if (!bytesPerChan) bytesPerChan = 200;
  uint32_t n = budget / bytesPerChan;
  if (n < 4)  n = 4;
  if (n > 64) n = 64;
  return n;
}

// Węzeł, który nie czyta argumentów "o"/"n" (starszy firmware), odsyła za
// każdym razem pierwszą stronę - wtedy lista kanałów urywa się w ciszy, więc
// zostawiamy po sobie ślad w logu (rzadko, żeby nie zalać syslogu).
static void warnNoPaging(const char* what, uint32_t off, uint32_t total) {
  static uint32_t lastMs = 0;
  uint32_t now = millis();
  if (!off || (uint32_t)(now - lastMs) < 60000) return;
  lastMs = now;
  LOG_W("RS485: węzeł nie stronicuje %s (zatrzymanie na %u z %u)", what,
        (unsigned)off, (unsigned)total);
}

// Dociąga metadane kanałów węzła od numeru "from" do końca listy. Każda strona
// niesie też aktualne wartości, więc po udanym dociągnięciu mamy komplet danych.
void Rs485Bus::fetchPeerMeta(uint32_t from) {
  const uint32_t page = pageChans(200);
  uint32_t off = from;
  uint32_t guard = 0;

  for (uint8_t i = 0; i < RS485_META_PAGES_SYNC; i++) {
    String args = String("{\"o\":") + String(off) + ",\"n\":" + String(page) + "}";
    PsramAllocator alloc;
    JsonDocument r(&alloc);
    String err;
    if (!request("chan", args, r, err, RS485_BIG_TIMEOUT_MS)) {
      LOG_W("RS485: nie udało się dociągnąć listy kanałów węzła (%s)", err.c_str());
      return;
    }
    JsonObject data = r["data"].as<JsonObject>();
    if (data.isNull()) return;
    if (data["chans"].is<JsonArray>()) applyPeerChannels(data["chans"].as<JsonArray>());

    uint32_t total = data["total"] | 0UL;
    uint32_t next  = data["next"] | 0UL;
    if (total) peerMetaTotal_ = total;
    if (next <= off) {                 // węzeł nie posunął się - nie zapętlajmy się
      warnNoPaging("listy kanałów", off, peerMetaTotal_);
      break;
    }
    off = next;
    peerMetaFrom_ = off;
    guard += (uint32_t)(data["channels"] | 0UL);
    if (data["done"] | false) break;
    if (guard > 4096) break;           // bezpiecznik na wypadek dziwnej odpowiedzi
  }

  if (peerMetaFrom_ >= peerMetaTotal_ || peerMetaTotal_ == 0) {
    LOG_I("RS485: pełna lista kanałów węzła (%u) wciągnięta", (unsigned)peerMetaTotal_);
  }
}

// Świeże wartości wszystkich kanałów węzła - krótkie ramki "vals" (starszy
// węzeł ich nie zna; wtedy wracamy do odpytywania pełnych stron).
bool Rs485Bus::fetchPeerValues() {
  const uint32_t page = pageChans(32);
  uint32_t off = 0;
  bool any = false;

  for (uint8_t i = 0; i < RS485_MAX_PAGES; i++) {
    String args = String("{\"o\":") + String(off) + ",\"n\":" + String(page) + "}";
    PsramAllocator alloc;
    JsonDocument r(&alloc);
    String err;
    if (!request("vals", args, r, err, cfgTimeoutMs_)) {
      if (err.indexOf("nieznane") >= 0) {
        LOG_W("RS485: węzeł nie zna polecenia 'vals' - odpytywanie po staremu");
        peerValsOk_ = false;
      } else {
        LOG_W("RS485: brak wartości kanałów węzła (%s)", err.c_str());
      }
      return any;
    }
    JsonObject data = r["data"].as<JsonObject>();
    if (data.isNull()) return any;
    if (data["v"].is<JsonArray>()) {
      applyPeerValues(data["v"].as<JsonArray>());
      any = true;
    }
    uint32_t next = data["next"] | 0UL;
    if (next <= off) {
      warnNoPaging("wartości kanałów", off, data["total"] | 0UL);
      break;
    }
    off = next;
    if (data["done"] | false) break;
  }
  return any;
}

// Pełna lista kanałów węzła dla zakładki "ESP (RS485)" w www. Lista jest tak
// długa, że węzeł nie zmieści jej w jednej ramce, więc sklejamy ją ze stron
// "chan". idx < 0 => węzeł odpytywany teraz (albo pierwszy z listy).
bool Rs485Bus::peerChannelsJson(int idx, JsonDocument& out, String& err) {
  if (!cfgEnabled_ || !started_ || !cfgMaster_) { err = "magistrala nieaktywna"; return false; }

  uint8_t peer = 0;
  if (idx >= 0 && idx < nodeCnt_) {
    peer = nodes_[idx].addr;
  } else {
    peer = activeAddr_ ? activeAddr_ : (nodeCnt_ ? nodes_[0].addr : cfgPeer_);
  }
  if (!peer) { err = "brak węzłów na magistrali"; return false; }

  // Listy kanałów węzła bywają bardzo długie (np. 179), a sklejanie ich "na żywo"
  // ze stron "chan" blokowało zadanie serwera www na kilkanaście sekund, co
  // kończyło się resetem (watchdog zadania). Pełna lista jest i tak utrzymywana
  // w tle przez fetchPeerMeta(), więc serwujemy ją natychmiast z pamięci
  // SensorManager - bez blokowania magistrali i bez zrywania połączenia.
  std::vector<Channel> all = sensors.snapshot();
  JsonArray chans = out["chans"].to<JsonArray>();
  uint32_t got = 0;

  for (const Channel& c : all) {
    if (!c.remote || c.remoteAddr != peer) continue;

    // Identyfikator w formacie węzła (bez prefiksu "x2_"), bo po nim rozpoznaje
    // go np. polecenie chan_set. Nadpisania kalibracji trzymamy po pełnym "x2_".
    String id = c.id;
    if (SensorManager::isRemoteId(id)) {
      int sep = id.indexOf('_');
      if (sep > 0) id = id.substring(sep + 1);
    }

    JsonObject o = chans.add<JsonObject>();
    o["id"] = id;
    o["n"]  = c.name;
    o["u"]  = c.unit;
    o["z"]  = c.zone;
    o["dec"] = c.decimals;
    if (isnan(c.value)) o["v"] = nullptr; else o["v"] = c.value;
    o["det"] = c.detected;
    o["meas"] = c.measured;
    o["en"] = c.enabled;
    o["off"] = config.channelCfg(c.id).offset;
    if (c.haClass.length()) o["hc"] = c.haClass;
    if (c.haUnit.length())  o["hu"] = c.haUnit;
    if (c.haIcon.length())  o["hi"] = c.haIcon;
    got++;
  }

  out["channels"] = got;
  out["total"]    = got;
  out["peer"]     = peer;
  out["node"]     = idx;
  out["online"]   = nodeOnline(peer);
  err = "";
  return true;
}

int Rs485Bus::scan(uint8_t* found, int maxFound) {
  int n = 0;
  if (!cfgEnabled_ || !started_ || !cfgMaster_) return 0;

  // Każdy znaleziony adres od razu trafia na listę węzłów, więc nie trzeba go
  // dopisywać ręcznie - master odpyta go w następnej kolejce.
  for (uint8_t a = 2; a <= RS485_SCAN_MAX_ADDR && n < maxFound; a++) {
    if (a == cfgAddr_) continue;
    if (pingAddr(a, RS485_PING_TIMEOUT_MS)) {
      found[n++] = a;
      adoptPeer(a);
    }
  }

  discoverMiss_ = 0;
  discoverNext_ = 1;
  nextDiscoverMs_ = 0;
  if (n) {
    String list;
    for (int i = 0; i < n; i++) { if (i) list += ", "; list += String(found[i]); }
    LOG_I("RS485: znalezione adresy na magistrali: %s", list.c_str());
    failStreak_ = 0;
    lastNoPeerLogMs_ = 0;
    pollNow();
  } else {
    LOG_W("RS485: nie znaleziono żadnego urządzenia na magistrali (adresy 2-%u)",
          (unsigned)RS485_SCAN_MAX_ADDR);
  }
  return n;
}

// ---------------------------------------------------------------
//  Węzeł
// ---------------------------------------------------------------
void Rs485Bus::handleSlave() {
  if (!Serial0.available()) return;
  if (mtx_ == nullptr || xSemaphoreTake(mtx_, 0) != pdTRUE) return;

  Resp r;
  while (Serial0.available()) {
    uint8_t b = (uint8_t)Serial0.read();
    frameFeed(parser_, rxBuf_, b, r);
    if (!r.ok) continue;
    if (r.type == 0x01 && (r.dst == 0 || r.dst == cfgAddr_)) {
#if STACJA_HEADLESS
      // Każde pytanie mastera widać w konsoli USB (max 1 na sekundę) - to
      // jedyny sposób sprawdzenia przewodów bez strony www na węźle.
      static uint32_t lastAskMs = 0;
      uint32_t askNow = millis();
      if (lastAskMs == 0 || (uint32_t)(askNow - lastAskMs) >= 1000) {
        lastAskMs = askNow;
        LOG_I("RS485: pytanie od adresu %u (%u B): %.60s",
              (unsigned)r.src, (unsigned)r.payload.length(), r.payload.c_str());
      }
#endif
      bool ok = true;
      String err;
      String resp = slaveHandle(r.payload, r.src, ok, err);
      if (resp.length() == 0) {
        // Pusta odpowiedź = ramka nie była przeznaczona dla tego modułu
        // (np. zmiana adresu innego węzła) - milczymy, żeby nie zajmować
        // magistrali.
        r.ok = false;
        continue;
      }
      size_t n = buildFrame(r.src, 0x81, r.seq, resp);
      if (n == 0) {
        n = buildFrame(r.src, 0x81, r.seq,
                       respJson(false, "odpowiedź za duża (limit 8192 B)"));
        LOG_W("RS485: odpowiedź na '%s' nie zmieściła się w ramce", r.payload.substring(0, 24).c_str());
      }
      if (n) {
        writeTx(txBuf_, n);
        st_.tx++;
        st_.ok++;
        st_.lastOkMs = millis();
      } else {
        st_.errFrame++;
      }
      if (pendingAddr_) {
        // Nowy adres zapisujemy po odpowiedzi - od tej chwili węzeł odpowiada
        // (i jest odpytywany) pod nowym adresem.
        uint8_t na = pendingAddr_;
        pendingAddr_ = 0;
        String js = "{\"rs485_addr\":" + String((unsigned)na) + "}";
        if (config.applyJson(js.c_str(), js.length())) {
          reload();
          nextPollMs_ = 0;
          LOG_I("RS485: nowy adres węzła %u zapisany", (unsigned)na);
        } else {
          LOG_W("RS485: nie udało się zapisać nowego adresu %u", (unsigned)na);
        }
      }
    }
    r.ok = false;
  }
  xSemaphoreGive(mtx_);
}

String Rs485Bus::slaveHandle(const String& payload, uint8_t src, bool& ok, String& err) {
  ok = true;
  err = "";

  JsonDocument req;
  if (deserializeJson(req, payload)) {
    ok = false;
    err = "błędny JSON zapytania";
    return respJson(false, err);
  }

  String cmd = sv(req["c"]);
  JsonVariantConst a = req["a"];
  // Sekcja "a" bywa obiektem albo tablicą (chan_set), a na wariancie "const"
  // testy is<JsonObject>()/is<JsonArray>() nie przechodzą (typy mutowalne),
  // przez co argumenty były po cichu odrzucane. Dlatego konwertujemy raz do
  // widoku const - pusty (isNull) oznacza, że "a" nie jest obiektem.
  JsonObjectConst ao = a.as<JsonObjectConst>();
  if (!cmd.length()) {
    ok = false;
    err = "brak nazwy polecenia";
    return respJson(false, err);
  }

  // --- identyfikacja ---
  if (cmd == "ping" || cmd == "hello") {
    return respJson(true, "", selfIdentityJson());
  }
  if (cmd == "sync") {
    return respJson(true, "", selfStatusJson());
  }
  if (cmd == "status") {
    return respJson(true, "", apiStatusJson());
  }
  if (cmd == "diag") {
    return respJson(true, "", apiDiagnosticsJson());
  }
  if (cmd == "chan") {
    // Strona listy kanałów: {"o":<od którego>,"n":<ile>} - bez argumentów
    // zwracamy pierwszą stronę (starszy master dostawał tu całą listę, więc
    // nadal działa, tylko przycina się do rozmiaru ramki).
    uint32_t off = 0, cnt = 0;
    if (!ao.isNull()) {
      off = ao["o"] | 0UL;
      cnt = ao["n"] | 0UL;
    }
    return respJson(true, "", selfChannelsJson(off, cnt));
  }
  if (cmd == "vals") {
    // Same wartości (id, wartość, wykryty, zmierzony) - krótka ramka, więc
    // można je odświeżać co synchronizację bez przesyłania metadanych.
    uint32_t off = 0, cnt = 0;
    if (!ao.isNull()) {
      off = ao["o"] | 0UL;
      cnt = ao["n"] | 0UL;
    }
    return respJson(true, "", selfValuesJson(off, cnt));
  }
  if (cmd == "cfg") {
    return respJson(true, "", configJson());
  }
  if (cmd == "pins") {
    return respJson(true, "", pinsCompactJson());
  }

  // --- zmiana adresu węzła (master nadaje nowy adres po RS485) ---
  if (cmd == "addr") {
    if (ao.isNull()) { ok = false; err = "brak sekcji 'a'"; return respJson(false, err); }
    String want = normMac(sv(ao["mac"]));
    if (want.length() && want != normMac(deviceMac())) {
      // Ramka adresowana sprzętowo trafia do wszystkich modułów na magistrali,
      // więc obcy węzeł nie odpowiada - master próbuje dalej.
      return "";
    }
    int na = ao["addr"] | 0;
    if (!nodesValidAddr((uint8_t)na)) {
      ok = false;
      err = "nowy adres musi być z zakresu 2..247";
      return respJson(false, err);
    }
    if ((uint8_t)na == cfgAddr_) {
      ok = false;
      err = "węzeł już ma ten adres";
      return respJson(false, err);
    }
    uint8_t old = cfgAddr_;
    // Adres zapisujemy dopiero po wysłaniu odpowiedzi (handleSlave) - węzeł
    // musi najpierw potwierdzić zmianę, a potem przestawia się na nowy adres.
    pendingAddr_ = (uint8_t)na;
    LOG_W("RS485: master zmienia adres węzła %u -> %u", (unsigned)old, (unsigned)na);
    return respJson(true, "", "{\"addr\":" + String((unsigned)na) + ",\"old\":" +
                                  String((unsigned)old) + "}");
  }

  // --- ustawienia pinów (cała tabela, klucz = rola) ---
  if (cmd == "pins_set") {
    if (ao.isNull()) { ok = false; err = "brak sekcji 'a'"; return respJson(false, err); }
    int changed = 0;
    String problems;
    for (JsonPairConst kv : ao) {
      const char* key = kv.key().c_str();
      if (pinMap.find(key) == nullptr) continue;
      int gpio = kv.value() | 0;
      String e;
      if (!pinMap.set(key, gpio, e)) {
        problems += String(key) + ": " + e + "; ";
        continue;
      }
      changed++;
    }
    bool saved = pinMap.save();
    String data = "{\"changed\":" + String(changed) + ",\"saved\":" +
                  String(saved ? "true" : "false") +
                  ",\"restart\":true,\"problems\":\"" + jsonEsc(problems) + "\"}";
    LOG_I("RS485: master ustawił %d przypisań pinów", changed);
    return respJson(saved, saved ? "" : "nie udało się zapisać pinów", data);
  }
  if (cmd == "pins_reset") {
    pinMap.resetToDefaults();
    bool saved = pinMap.save();
    LOG_I("RS485: master przywrócił domyślne piny");
    return respJson(saved, saved ? "" : "nie udało się zapisać pinów", "{\"restart\":true}");
  }

  // --- ustawienia globalne (tylko podane klucze, reszta bez zmian) ---
  if (cmd == "cfg_set") {
    if (ao.isNull()) { ok = false; err = "brak sekcji 'a'"; return respJson(false, err); }
    if (ao["pins"].is<JsonObjectConst>() || ao["channels"].is<JsonObjectConst>() ||
        ao["extra"].is<JsonObjectConst>()) {
      ok = false;
      err = "sekcje pins/channels/extra ustawia się poleceniami pins_set / chan_set";
      return respJson(false, err);
    }
    String js;
    serializeJson(ao, js);
    if (!config.applyJson(js.c_str(), js.length())) {
      ok = false;
      err = "nie udało się zapisać ustawień";
      return respJson(false, err);
    }
    LOG_I("RS485: master zmienił ustawienia węzła (%u B)", (unsigned)js.length());
    return respJson(true, "", "{\"restart\":true}");
  }

  // --- kalibracja / nazwy kanałów ---
  if (cmd == "chan_set") {
    JsonArrayConst arr = a.as<JsonArrayConst>();
    if (arr.isNull()) { ok = false; err = "brak listy kanałów w 'a'"; return respJson(false, err); }
    int changed = 0;
    for (size_t i = 0; i < arr.size(); i++) {
      JsonObjectConst c = arr[i].as<JsonObjectConst>();
      if (c.isNull()) continue;
      String id = sv(c["id"]);
      if (!id.length()) continue;
      ChannelConfig cc = config.channelCfg(id);
      if (!c["en"].isNull())   cc.enabled = c["en"] | true;
      if (!c["off"].isNull())  cc.offset  = c["off"] | 0.0f;
      if (!c["n"].isNull())    cc.name    = sv(c["n"]);
      if (!c["z"].isNull())    cc.zone    = sv(c["z"]);
      config.setChannelCfg(id, cc);
      changed++;
    }
    bool saved = config.save();
    sensors.applyChannelConfig();
    LOG_I("RS485: master zmienił kalibrację %d kanałów", changed);
    String data = "{\"changed\":" + String(changed) + ",\"saved\":" +
                  String(saved ? "true" : "false") + "}";
    return respJson(saved, saved ? "" : "nie udało się zapisać kalibracji", data);
  }

  // --- logi ---
  if (cmd == "log") {
    PsramAllocator alloc;
    JsonDocument d(&alloc);
    JsonObject o = d.to<JsonObject>();
    uint32_t n = a["n"] | 3000UL;
    logTailInto(o, n);
    String s;
    serializeJson(d, s);
    return respJson(true, "", s);
  }
  if (cmd == "log_clear") {
    syslog.clear();
    LOG_I("RS485: master wyczyścił log węzła");
    return respJson(true, "", "{\"cleared\":true}");
  }

  // --- zegar ---
  if (cmd == "time") {
    time_t epoch = (time_t)(uint32_t)(a["epoch"] | 0UL);
    if (epoch < 1000000000) { ok = false; err = "nieprawidłowy czas"; return respJson(false, err); }
    struct timeval tv;
    tv.tv_sec = epoch;
    tv.tv_usec = 0;
    settimeofday(&tv, nullptr);
    bool rtcOk = rtc.setTimeFromEpoch(epoch);
    LOG_I("RS485: czas ustawiony przez master (RTC: %s)", rtcOk ? "tak" : "nie");
    String data = "{\"epoch\":" + String((uint32_t)epoch) + ",\"rtc\":" +
                  String(rtcOk ? "true" : "false") + "}";
    return respJson(true, "", data);
  }

  // --- akcje ---
  if (cmd == "action") {
    String what = sv(a["do"]);
    if (what == "reboot") {
      LOG_W("RS485: master zlecił restart węzła");
      g_rebootRequested = true;
      return respJson(true, "", "{\"reboot\":true}");
    }
    if (what == "discover") {
      sensors.requestDsDiscovery();
      LOG_I("RS485: master zlecił wyszukiwanie czujników DS18B20");
      return respJson(true, "", "{\"discover\":true}");
    }
    ok = false;
    err = "nieznana akcja '" + what + "'";
    return respJson(false, err);
  }

  ok = false;
  err = "nieznane polecenie '" + cmd + "'";
  return respJson(false, err);
}

// ---------------------------------------------------------------
//  Odpowiedzi węzła
// ---------------------------------------------------------------
void Rs485Bus::identityInto(JsonObject o) {
  o["name"]  = config.deviceName();
  o["host"]  = config.hostname();
  o["fw"]    = FW_VERSION_FULL;
  o["build"] = String(__DATE__) + " " + String(__TIME__);
  String ip = "-";
  bool ap = false;
#if !STACJA_HEADLESS
  o["mac"] = deviceMac();
  ap = (WiFi.getMode() & WIFI_MODE_AP) != 0;
  if (WiFi.status() == WL_CONNECTED) ip = WiFi.localIP().toString();
  else if (ap) ip = WiFi.softAPIP().toString();
#else
  // Węzeł bez sieci: MAC z eFuse (ten sam format, co WiFi.macAddress()),
  // brak adresu IP i brak punktu dostępowego.
  o["mac"] = deviceMac();
  o["net"] = false;   // master wie, że ten węzeł nie ma sieci
#endif
  o["ip"]   = ip;
  o["ap"]   = ap;
  o["role"] = cfgMaster_ ? "master" : "wezel";
  o["addr"] = cfgAddr_;
  o["proto"] = RS485_PROTO_VER;

  // Płytka węzła - master sprawdza, czy wybrany szablon pinów się zgadza.
  const BoardProfile& bp = boards::current();
  o["board"]       = bp.id;
  o["board_short"] = bp.shortName;
  o["chip"]        = boards::chipId();
  o["chip_name"]   = boards::chipName();
  o["flash_mb"]    = (uint32_t)(ESP.getFlashChipSize() / (1024UL * 1024UL));
  o["psram_mb"]    = (uint32_t)(ESP.getPsramSize() / (1024UL * 1024UL));
}

  // Szacunkowy rozmiar kanału w JSON-ie - pozwala przerwać dopisywanie jeszcze
  // przed serializacją (budowanie 30 kB tekstu i przycinanie go po jednym
  // kanale byłoby bardzo kosztowne).
  static uint32_t chanJsonEstimate(const Channel& c) {
    return 160 + c.id.length() + c.name.length() + c.unit.length() + c.zone.length()
           + c.haClass.length() + c.haUnit.length() + c.haIcon.length();
  }

// Wspólny zapis jednego kanału do JSON-a magistrali (używany i przez pełną
// listę w "sync", i przez strony "chan").
static void channelInto(JsonObject o, const Channel& c) {
  o["id"] = c.id;
  o["n"]  = c.name;
  o["u"]  = c.unit;
  o["z"]  = c.zone;
  o["dec"] = c.decimals;
  if (isnan(c.value)) o["v"] = nullptr; else o["v"] = c.value;
  o["det"] = c.detected;
  o["meas"] = c.measured;
  o["en"] = c.enabled;
  o["off"] = config.channelCfg(c.id).offset;
  if (c.haClass.length()) o["hc"] = c.haClass;
  if (c.haUnit.length())  o["hu"] = c.haUnit;
  if (c.haIcon.length())  o["hi"] = c.haIcon;
}

String Rs485Bus::selfIdentityJson() {
  PsramAllocator alloc;
  JsonDocument d(&alloc);
  identityInto(d.to<JsonObject>());
  String s;
  serializeJson(d, s);
  return s;
}

String Rs485Bus::selfChannelsJson(uint32_t off, uint32_t cnt) {
  PsramAllocator alloc;
  JsonDocument d(&alloc);
  JsonObject o = d.to<JsonObject>();
  JsonArray arr = o["chans"].to<JsonArray>();

  std::vector<Channel> page;
  uint32_t total = sensors.localPage(off, cnt ? cnt : RS485_CHAN_PAGE, page);

  // Strona musi zmieścić się w jednej ramce - dlatego kończymy dopisywanie,
  // gdy oszacowany rozmiar dojdzie do budżetu (długie nazwy użytkownika).
  const uint32_t budget = RS485_MAX_PAYLOAD - 160;
  uint32_t est = 0;
  for (const Channel& c : page) {
    uint32_t e = chanJsonEstimate(c);
    if (arr.size() && est + e > budget) break;
    est += e;
    channelInto(arr.add<JsonObject>(), c);
  }

  uint32_t added = (uint32_t)arr.size();
  o["channels"] = added;
  o["total"] = total;
  o["off"] = off;
  o["next"] = off + added;
  o["done"] = (off + added) >= total;

  String s;
  serializeJson(d, s);
  while (s.length() > RS485_MAX_PAYLOAD - 128 && arr.size() > 0) {
    arr.remove(arr.size() - 1);
    added = (uint32_t)arr.size();
    o["channels"] = added;
    o["next"] = off + added;
    o["done"] = (off + added) >= total;
    serializeJson(d, s);
  }
  return s;
}

String Rs485Bus::selfValuesJson(uint32_t off, uint32_t cnt) {
  PsramAllocator alloc;
  JsonDocument d(&alloc);
  JsonObject o = d.to<JsonObject>();
  JsonArray arr = o["v"].to<JsonArray>();

  std::vector<Channel> page;
  uint32_t total = sensors.localPage(off, cnt ? cnt : RS485_VAL_PAGE, page);
  const uint32_t budget = RS485_MAX_PAYLOAD - 160;
  uint32_t est = 0;
  for (const Channel& c : page) {
    uint32_t e = c.id.length() + 28;
    if (arr.size() && est + e > budget) break;
    est += e;
    JsonArray e2 = arr.add<JsonArray>();
    e2.add(c.id);
    if (isnan(c.value)) e2.add(nullptr); else e2.add(c.value);
    e2.add(c.detected);
    e2.add(c.measured);
  }

  uint32_t added = (uint32_t)arr.size();
  o["channels"] = added;
  o["total"] = total;
  o["off"] = off;
  o["next"] = off + added;
  o["done"] = (off + added) >= total;

  String s;
  serializeJson(d, s);
  while (s.length() > RS485_MAX_PAYLOAD - 128 && arr.size() > 0) {
    arr.remove(arr.size() - 1);
    added = (uint32_t)arr.size();
    o["channels"] = added;
    o["next"] = off + added;
    o["done"] = (off + added) >= total;
    serializeJson(d, s);
  }
  return s;
}

String Rs485Bus::selfStatusJson() {
  PsramAllocator alloc;
  JsonDocument d(&alloc);
  JsonObject o = d.to<JsonObject>();

  identityInto(o);
  o["uptime_s"] = (uint32_t)(millis() / 1000);
  time_t now = time(nullptr);
  o["epoch"] = (uint32_t)now;
  o["time_set"] = now > 1000000000;
#if STACJA_HEADLESS
  o["rssi"] = 0;   // węzeł bez sieci nie ma sygnału Wi-Fi do raportowania
#else
  o["rssi"] = WiFi.RSSI();
#endif
  o["heap"] = (uint32_t)ESP.getFreeHeap();
  o["psram"] = (uint32_t)ESP.getFreePsram();
  o["boot"] = syslog.bootCount();
  o["reset"] = syslog.resetReason();
  o["sd"] = sdCard.mounted();
  o["errors"] = syslog.errors();
  o["warns"] = syslog.warns();

  // Kanałów własnych jest już ponad setkę, więc do jednej ramki "sync" wchodzi
  // tylko ich początek. Resztę master dociąga stronami polecenia "chan"
  // (patrz fetchPeerMeta), a wartości - krótkimi ramkami "vals".
  JsonArray arr = o["chans"].to<JsonArray>();
  std::vector<Channel> all;
  uint32_t total = sensors.localPage(0, 0, all);   // 0 = wszystkie własne kanały

  uint32_t det = 0;
  for (const Channel& c : all) if (c.detected) det++;

  const uint32_t budget = RS485_MAX_PAYLOAD - 256;
  uint32_t est = 0;
  for (const Channel& c : all) {
    uint32_t e = chanJsonEstimate(c);
    if (arr.size() && est + e > budget) break;
    est += e;
    channelInto(arr.add<JsonObject>(), c);
  }

  String s;
  serializeJson(d, s);

  bool trunc = false;
  while (s.length() > RS485_MAX_PAYLOAD - 96 && arr.size() > 0) {
    arr.remove(arr.size() - 1);
    trunc = true;
    serializeJson(d, s);
  }

  o["channels"] = total;          // ile kanałów ma węzeł (dla pulpitu mastera)
  o["sent"] = arr.size();         // ile zmieściło się w tej ramce
  o["detected"] = det;
  o["trunc"] = trunc;
  serializeJson(d, s);
  return s;
}

String Rs485Bus::pinsCompactJson() {
  PsramAllocator alloc;
  JsonDocument d(&alloc);
  JsonObject o = d.to<JsonObject>();
  JsonObject a = o["a"].to<JsonObject>();
  int count = pinMap.count();
  for (int i = 0; i < count; i++) {
    const PinRole* r = pinMap.role(i);
    if (!r) continue;
    a[r->key] = pinMap.pin(r->key);
  }
  String pr = pinMap.problems();
  o["pr"] = pr;
  o["ok"] = pr.length() == 0;
  String s;
  serializeJson(d, s);
  return s;
}

String Rs485Bus::configJson() {
  return config.toJsonString();
}

void Rs485Bus::logTailInto(JsonObject o, uint32_t n) {
  if (n < 512) n = 512;
  if (n > 8000) n = 8000;
  size_t len = syslog.snapshot();
  const char* data = syslog.snapshotData();
  if (data == nullptr || len == 0) {
    o["log"] = "";
    o["bytes"] = 0;
    o["trunc"] = false;
    return;
  }
  bool trunc = false;
  if (len > n) { data += (len - n); len = n; trunc = true; }
  o["log"] = String(data, len);
  o["bytes"] = (uint32_t)len;
  o["trunc"] = trunc;
  o["buffer"] = (uint32_t)syslog.capacity();
}

// ---------------------------------------------------------------
//  Stan dla strony www (zakładka "ESP i magistrala RS485")
// ---------------------------------------------------------------
// Jeden węzeł listy dla strony www. Statystyki i identyfikację bierzemy
// z zestawu roboczego, gdy to węzeł właśnie odpytywany (są najświeższe),
// a w pozostałych przypadkach - z zapisanego wpisu listy.
void Rs485Bus::nodeJsonInto(int idx, JsonObject o) {
  if (idx < 0 || idx >= nodeCnt_) return;
  const Rs485Node& nd = nodes_[idx];
  bool active = (idx == loadedIdx_) || (loadedIdx_ < 0 && nd.addr == activeAddr_);
  const Rs485Stats& s = active ? st_ : nd.st;
  const Rs485PeerInfo& inf = active ? peer_ : nd.info;

  o["i"] = idx;
  o["addr"] = nd.addr;
  o["label"] = String("ESP #") + String((unsigned)idx + 2);
  o["active"] = active;
  o["online"] = nodeFresh(nd);
  o["detected"] = inf.valid || nd.lastOkMs != 0;
  o["ok_s"] = s.lastOkMs ? (millis() - s.lastOkMs) / 1000 : 0;
  o["sync_s"] = nd.lastSyncMs ? (millis() - nd.lastSyncMs) / 1000 : 0;
  o["mirrored"] = sensors.remoteCount(nd.addr);
  o["meta_known"] = active ? peerMetaFrom_ : nd.metaFrom;
  o["meta_total"] = active ? peerMetaTotal_ : nd.metaTotal;
  o["vals_ok"] = active ? peerValsOk_ : nd.valsOk;
  o["fail_streak"] = active ? failStreak_ : nd.failStreak;

  if (inf.valid) {
    o["name"] = inf.name;
    o["host"] = inf.host;
    o["fw"] = inf.fw;
    o["build"] = inf.build;
    o["mac"] = inf.mac;
    o["ip"] = inf.ip;
    o["role"] = inf.role;
    o["board"] = inf.board;
    o["chip"] = inf.chip;
    o["flash_mb"] = inf.flashMb;
    o["psram_mb"] = inf.psramMb;
    o["uptime_s"] = inf.uptimeS;
    o["rssi"] = inf.rssi;
    o["heap"] = inf.heap;
    o["psram"] = inf.psram;
    o["boot"] = inf.bootCount;
    o["reset"] = inf.resetReason;
    o["sd"] = inf.sd;
    o["errors"] = inf.errors;
    o["warns"] = inf.warns;
    o["channels"] = inf.channels;
    o["detected_ch"] = inf.detected;   // kanały potwierdzone sprzętowo
  }

  JsonObject t = o["st"].to<JsonObject>();
  t["tx"] = s.tx;
  t["rx"] = s.rx;
  t["ok"] = s.ok;
  t["err_crc"] = s.errCrc;
  t["err_frame"] = s.errFrame;
  t["err_timeout"] = s.errTimeout;
  t["retries"] = s.retries;
  t["latency_ms"] = s.latencyMs;
  t["err"] = s.lastError;
  t["raw_rx"] = s.rawRx;
  t["sniff"] = sniffHex(s);
}

String Rs485Bus::stateJson() {
  PsramAllocator alloc;
  JsonDocument d(&alloc);
  JsonObject o = d.to<JsonObject>();

  o["enabled"]  = cfgEnabled_;
  o["role"]     = cfgMaster_ ? "master" : "wezel";
  o["master"]   = cfgMaster_;
  o["build_role"] = FW_ROLE_NAME;
  o["fw"]       = FW_VERSION_FULL;
  o["peer_auto"] = (nodeCnt_ == 0);
  o["nodes_auto"] = (nodeCnt_ == 0);
  o["addr"]     = cfgAddr_;
  o["peer"]     = cfgPeer_;
  o["active_addr"] = activeAddr_;
  o["node_count"] = nodeCnt_;
  o["max_nodes"] = RS485_MAX_NODES;
  o["scan_max_addr"] = RS485_SCAN_MAX_ADDR;
  o["baud"]     = cfgBaud_;
  o["poll_s"]   = cfgPollS_;
  o["timeout_ms"] = cfgTimeoutMs_;
  o["retries"]  = cfgRetries_;
  o["log"]      = config.rs485Log();
  o["mqtt"]     = config.rs485Mqtt();
  o["slave_ap"] = config.rs485SlaveAp();
  o["proto"]    = RS485_PROTO_VER;
  o["max_payload"] = RS485_MAX_PAYLOAD;
  o["rx"] = (int)pinRx_;
  o["tx"] = (int)pinTx_;
  o["de"] = (int)pinDe_;
  o["started"]  = started_;
  o["online"]   = online();
  o["last_sync_s"] = cacheAgeS();
  o["last_ok_s"]  = st_.lastOkMs ? (millis() - st_.lastOkMs) / 1000 : 0;
  o["remote_channels"] = sensors.remoteCount();
  o["selftest"] = selftestInfo_;

  JsonObject s = o["st"].to<JsonObject>();
  s["tx"] = st_.tx;
  s["rx"] = st_.rx;
  s["ok"] = st_.ok;
  s["err_crc"] = st_.errCrc;
  s["err_frame"] = st_.errFrame;
  s["err_timeout"] = st_.errTimeout;
  s["retries"] = st_.retries;
  s["latency_ms"] = st_.latencyMs;
  s["err"] = st_.lastError;
  s["raw_rx"] = st_.rawRx;
  s["raw_s"] = st_.lastRawMs ? (uint32_t)((millis() - st_.lastRawMs) / 1000) : 0;
  s["sniff"] = sniffHex(st_);

  // Uwaga: o["peer"] to adres węzła (liczba) - szczegóły idą do "node",
  // inaczej konwersja do obiektu skasowałaby adres z formularza.
  JsonObject p = o["node"].to<JsonObject>();
  p["valid"] = peer_.valid;
  if (peer_.valid) {
    p["name"] = peer_.name;
    p["host"] = peer_.host;
    p["fw"] = peer_.fw;
    p["build"] = peer_.build;
    p["mac"] = peer_.mac;
    p["ip"] = peer_.ip;
    p["role"] = peer_.role;
    p["addr"] = peer_.addr;
    p["uptime_s"] = peer_.uptimeS;
    p["epoch"] = (uint32_t)peer_.epoch;
    p["rssi"] = peer_.rssi;
    p["heap"] = peer_.heap;
    p["psram"] = peer_.psram;
    p["boot"] = peer_.bootCount;
    p["reset"] = peer_.resetReason;
    p["sd"] = peer_.sd;
    p["errors"] = peer_.errors;
    p["warns"] = peer_.warns;
    p["channels"] = peer_.channels;
    p["detected"] = peer_.detected;
    p["board"] = peer_.board;
    p["chip"] = peer_.chip;
    p["flash_mb"] = peer_.flashMb;
    p["psram_mb"] = peer_.psramMb;
  }

  // Lista węzłów magistrali - strona www buduje z niej podmenu "ESP #n".
  JsonArray ns = o["nodes"].to<JsonArray>();
  if (cfgMaster_) {
    for (int i = 0; i < nodeCnt_; i++) nodeJsonInto(i, ns.add<JsonObject>());
  }
  // Adresy, które nie odpowiadają, ale master próbuje je wykryć w tle.
  o["discovering"] = cfgMaster_ && (nodeCnt_ == 0 || cfgPeer_ == 0 || failStreak_ >= 2);

  String out;
  serializeJson(d, out);
  return out;
}

// ---------------------------------------------------------------
//  Test protokołu bez magistrali
// ---------------------------------------------------------------
bool Rs485Bus::selftest(String& info) {
  if (txBuf_ == nullptr || rxBuf_ == nullptr) {
    info = "Brak buforów ramek - test niemożliwy";
    selftestInfo_ = info;
    return false;
  }
  if (mtx_ == nullptr || xSemaphoreTake(mtx_, pdMS_TO_TICKS(2000)) != pdTRUE) {
    info = "Test niedostępny - magistrala zajęta";
    selftestInfo_ = info;
    return false;
  }

  // Test nie ma prawa nabijać statystyk magistrali
  uint32_t saveRx = st_.rx, saveCrc = st_.errCrc, saveFrame = st_.errFrame;
  uint32_t saveRaw = st_.rawRx, saveRawMs = st_.lastRawMs;
  uint8_t  saveSniffN = st_.sniffN;
  uint8_t  saveSniff[sizeof(st_.sniff)];
  memcpy(saveSniff, st_.sniff, sizeof(saveSniff));

  String log;
  bool ok = true;
  const uint16_t testSeq = 0x4C51;

  // 1. Ramka zapytania zbudowana na tej stacji
  String reqPayload = "{\"c\":\"ping\"}";
  size_t reqLen = buildFrame(cfgMaster_ ? cfgPeer_ : 1, 0x01, testSeq, reqPayload);
  if (reqLen == 0) { ok = false; log += "1. błąd budowy ramki zapytania\r\n"; }
  else {
    Parser p;
    Resp r;
    for (size_t i = 0; i < reqLen; i++) { frameFeed(p, rxBuf_, txBuf_[i], r); if (r.ok) break; }
    if (!r.ok) { ok = false; log += "1. stacja nie rozpoznała własnej ramki zapytania\r\n"; }
    else if (r.type != 0x01 || r.seq != testSeq) { ok = false; log += "1. błędny nagłówek ramki zapytania\r\n"; }
    else log += "1. ramka zapytania: " + String((unsigned)reqLen) + " B, nagłówek i CRC OK\r\n";
  }

  // 2. Ta sama treść trafia do obsługi jak z magistrali i wraca odpowiedzią
  uint8_t respType = 0;
  size_t respLen = 0;
  if (ok) {
    bool cmdOk = true;
    String err;
    String resp = slaveHandle(reqPayload, 7, cmdOk, err);
    respLen = buildFrame(7, 0x81, testSeq, resp);
    if (respLen == 0) { ok = false; log += "2. odpowiedź nie zmieściła się w ramce\r\n"; }
    else {
      Parser p;
      Resp r;
      for (size_t i = 0; i < respLen; i++) { frameFeed(p, rxBuf_, txBuf_[i], r); if (r.ok) break; }
      if (!r.ok) { ok = false; log += "2. odpowiedź nie przeszła kontroli CRC\r\n"; }
      else {
        respType = r.type;
        JsonDocument d;
        if (deserializeJson(d, r.payload)) { ok = false; log += "2. odpowiedź to niepoprawny JSON\r\n"; }
        else if (respType != 0x81) { ok = false; log += "2. błędny typ ramki odpowiedzi\r\n"; }
        else if (!(d["ok"] | false)) { ok = false; log += "2. węzeł odrzucił polecenie ping\r\n"; }
        else {
          log += "2. odpowiedź węzła: " + String((unsigned)respLen) + " B, JSON poprawny ("
                 + sv(d["data"]["name"]) + ")\r\n";
        }
      }
    }
  }

  // 3. Uszkodzona ramka musi zostać odrzucona
  if (ok) {
    txBuf_[respLen / 2] ^= 0x20;
    Parser p;
    Resp r;
    for (size_t i = 0; i < respLen; i++) { frameFeed(p, rxBuf_, txBuf_[i], r); if (r.ok) break; }
    if (r.ok) { ok = false; log += "3. uszkodzona ramka NIE została odrzucona\r\n"; }
    else log += "3. uszkodzona ramka odrzucona (CRC wykryło błąd)\r\n";
  }

  st_.rx = saveRx;
  st_.errCrc = saveCrc;
  st_.errFrame = saveFrame;
  st_.rawRx = saveRaw;
  st_.lastRawMs = saveRawMs;
  st_.sniffN = saveSniffN;
  memcpy(st_.sniff, saveSniff, sizeof(saveSniff));
  xSemaphoreGive(mtx_);

  info = String(ok ? "Test protokołu: OK" : "Test protokołu: BŁĄD") + "\r\n" + log;
  info += cfgEnabled_ && started_
            ? String("Magistrala: aktywna (") + (cfgMaster_ ? "master" : "węzeł") + ")\r\n"
            : String("Magistrala: nieaktywna - test nie używa portu szeregowego\r\n");
  info += String("Limit ramki: ") + String(RS485_MAX_PAYLOAD) + " B, protokół v"
          + String(RS485_PROTO_VER) + ", CRC-16/CCITT";
  selftestInfo_ = info;
  return ok;
}

// ---------------------------------------------------------------
//  Diagnostyka przewodu: "czy mój nadajnik w ogóle nadaje"
// ---------------------------------------------------------------
// Piny, po których warto szukać przewodu na masterze: całe złącze magistrali
// (38/39) plus piny sąsiadujące, które najłatwiej pomylić przy podłączaniu.
// Pomijamy linie, których ruszać nie wolno: 19/20 (natywne USB konsoli),
// 26-37 (flash i PSRAM), 43/44 (konsola szeregowa).
static const uint8_t DIAG_PINS[] = {
  38, 39, 40, 41, 42, 45, 46, 47, 48,
  0, 1, 2, 3, 4, 5, 6, 8, 9, 14, 15, 16, 17, 18, 21
};

static int diagParsePins(const String& csv, uint8_t* out, int maxN) {
  int n = 0, i = 0;
  while (i < (int)csv.length()) {
    while (i < (int)csv.length() && !isDigit(csv[i])) i++;
    if (i >= (int)csv.length()) break;
    int v = 0;
    while (i < (int)csv.length() && isDigit(csv[i])) { v = v * 10 + (csv[i] - '0'); i++; }
    if (n < maxN) out[n++] = (uint8_t)v;
  }
  return n;
}

static String diagHex(const uint8_t* p, size_t n) {
  String s;
  for (size_t i = 0; i < n; i++) {
    char b[4];
    if (i) s += ' ';
    snprintf(b, sizeof(b), "%02X", p[i]);
    s += b;
  }
  return s;
}

// Czy zebrane bajty to odpowiedź węzła na naszą ramkę (nagłówek + nasz seq)?
static bool diagIsReply(const uint8_t* p, size_t n, uint16_t seq) {
  if (n < 10) return false;
  if (p[0] != 0xAA || p[1] != 0x55) return false;
  if (p[2] != RS485_PROTO_VER || p[5] != 0x81) return false;
  if (seq == 0) return true;   // nasłuch bierny: liczy się dowolna ramka odpowiedzi
  return ((uint16_t)p[6] | ((uint16_t)p[7] << 8)) == seq;
}

// Poziom jałowy pinu: 100 = linia wolna (podciągnięta w górę), 0 = coś trzyma
// ją w GND. Podciąg wewnętrzny (~45 kΩ) przegrywa z każdym zwarciem do masy,
// dlatego wynik 0 jednoznacznie wskazuje na zwarcie/obcy driver w stanie LOW.
// Podciąg ustawiamy bezpośrednio (gpio_set_pull_mode), a nie pinMode()-em:
// pinMode() na pinie magistrali odbiera pad nadajnikowi UART0 (albo pinowi DE),
// przez co po diagnostyce nadajnik mastera milczał do restartu stacji.
static int diagIdleLevel(uint8_t pin) {
  gpio_set_pull_mode((gpio_num_t)pin, GPIO_PULLUP_ONLY);
  delayMicroseconds(300);
  int hi = 0;
  for (int k = 0; k < 100; k++) {
    if (digitalRead(pin)) hi++;
    delayMicroseconds(10);
  }
  gpio_set_pull_mode((gpio_num_t)pin, GPIO_FLOATING);
  return hi;
}

// Sprawdzenie nadajnika mastera "na żywo": wysyłamy porcje bajtów 0xAA i w
// trakcie nadawania próbkujemy poziom tego samego pinu. Sprawny nadajnik daje
// kilkadziesiąt procent próbek w LOW (0xAA to naprzemienne bity), a pad tylko
// podciągnięty - 0%. Nie używamy tu writeTx(), bo jego flush() czeka na koniec
// transmisji, więc w momencie pomiaru linia byłaby już wolna.
static int diagTxDrive(uint8_t pin) {
  uint8_t chunk[8];
  memset(chunk, 0xAA, sizeof(chunk));
  int lo = 0, tot = 0;
  for (int rep = 0; rep < 4; rep++) {
    Serial0.write(chunk, sizeof(chunk));
    uint32_t t0 = micros();
    while ((uint32_t)(micros() - t0) < 60) {
      if (!gpio_get_level((gpio_num_t)pin)) lo++;
      tot++;
    }
  }
  Serial0.flush();
  return tot ? (int)((100L * (long)lo) / (long)tot) : -1;
}

// Piny zajęte przez inne układy (I2C, karta SD, GPS, DHT, AS3935, RTC...)
// zostawiamy w spokoju: diagnostyka nie może zostawić ich jako zwykłe GPIO, bo
// po powrocie na stronę www zniknęłyby czujniki. Wyjątkiem są piny samej
// magistrali (RX/TX) - te są potrzebne, a port wraca na nie po teście.
static bool diagPinBusy(uint8_t p) {
  if (p == 19 || p == 20) return true;   // natywne USB konsoli
  for (int i = 0; i < pinMap.count(); i++) {
    const PinRole* r = pinMap.role((size_t)i);
    if (!r || !r->key) continue;
    if (pinMap.pin(r->key) != (int)p) continue;
    if (!strcmp(r->key, "rs485_rx") || !strcmp(r->key, "rs485_tx") ||
        !strcmp(r->key, "rs485_de")) continue;
    return true;
  }
  return false;
}

// Restart samego portu magistrali (bez restartu stacji). Zamyka i otwiera UART0
// od nowa, więc nadajnik, odbiornik i pin DE wracają na swoje miejsca - to
// jedyny pewny sposób oddania padów, które zabrała diagnostyka przewodu.
bool Rs485Bus::restartPort() {
  if (mtx_ == nullptr || xSemaphoreTake(mtx_, pdMS_TO_TICKS(2500)) != pdTRUE) return false;
  bool ok = false;
  if (cfgEnabled_ && pinRx_ >= 0 && pinTx_ >= 0) {
    stopPort();
    startPort();
    ok = started_;
    // Po restarcie portu od razu próbujemy się dogadać z węzłem.
    nextPollMs_     = 0;
    nextDiscoverMs_ = 0;
    failStreak_     = 0;
  }
  xSemaphoreGive(mtx_);
  return ok;
}

bool Rs485Bus::sniffTx(int pin, uint32_t ms, JsonDocument& out, String& err) {
  err = "";
  if (!cfgMaster_) { err = "Diagnostyka tylko na masterze"; return false; }
  if (txBuf_ == nullptr) { err = "Brak bufora ramek"; return false; }
  if (!started_) { err = "Magistrala nieaktywna - najpierw włącz magistralę"; return false; }
  if (pin < 0) pin = pinTx_;
  if (pin < 0 || pin > 48 || pin == 19 || pin == 20) { err = "Nieprawidłowy pin nasłuchu"; return false; }
  if (mtx_ == nullptr || xSemaphoreTake(mtx_, pdMS_TO_TICKS(2500)) != pdTRUE) {
    err = "Magistrala zajęta - spróbuj ponownie";
    return false;
  }

  if (ms < 20) ms = 50;
  if (ms > 5000) ms = 5000;

  uint8_t dst = scanAddr_ ? scanAddr_ : (reqAddr_ ? reqAddr_ : cfgPeer_);
  if (dst == 0) dst = 1;

  uint16_t seq = ++seq_;
  size_t n = buildFrame(dst, 0x01, seq, "{\"c\":\"ping\"}");

  // Najpierw mierzymy stan linii, zanim przejmiemy pad na UART1: poziom 0 przy
  // włączonym podciągu = ktoś (albo zwarcie) trzyma ten przewód w GND.
  out["idle"] = diagIdleLevel((uint8_t)pin);
  // Poziom pinu DE (w połączeniu 3-przewodowym ma być 0 - nadajnik jest wtedy
  // na stałe włączony, a pomiar podciągiem na pinie wyjściowym zawsze dawał 0).
  if (pinDe_ >= 0) out["de_level"] = digitalRead(pinDe_) ? 1 : 0;

  // Nasłuch własnego pinu TX przez UART1 przestawiłby pad nadajnika (wtedy ramka
  // testowa i tak nie wyszłaby na przewód), dlatego dla tego pinu sprawdzamy
  // nadajnik na żywo - to jedyny wiarygodny dowód, że dane idą w przewód.
  if (pin == (int)pinTx_) {
    out["self_tx"] = true;
    out["pin"]     = pin;
    out["drive"]   = diagTxDrive((uint8_t)pin);
    xSemaphoreGive(mtx_);
    return true;
  }

  // Diagnostyka używa UART1, który normalnie obsługuje GPS - na czas testu
  // zamykamy port odbiornika i otwieramy go z powrotem po zakończeniu.
  bool gpsWas = gps.enabled();
  if (gpsWas) gps.end();

  // Pin RX mastera to linia nadawcza węzła: gdybyśmy po nim nadali ramkę
  // testową, nasz sygnał zderzyłby się z odpowiedzią węzła (i psuł ramki
  // odbierane przez UART0) - dlatego ten pin tylko nasłuchujemy.
  bool listenOnly = (pin == (int)pinRx_);

  // UART1 nasłuchuje tego pinu (TX = -1) i jednocześnie nadajemy po nim ramkę
  // testową - w ten sposób sprawdzamy przewód/pin, którego nie używa UART0.
  Serial1.begin(cfgBaud_, SERIAL_8N1, pin, -1);
  delay(2);
  while (Serial1.available()) Serial1.read();

  if (!listenOnly && n) writeTx(txBuf_, n);

  uint8_t got[160];
  size_t gn = 0;
  uint32_t t0 = millis();
  while ((uint32_t)(millis() - t0) < ms) {
    while (Serial1.available() && gn < sizeof(got)) got[gn++] = (uint8_t)Serial1.read();
    delay(1);
  }
  Serial1.end();
  if (gpsWas) gps.begin();
  // UART1 zdjął przypisanie tego pinu z UART0, a pomiar poziomu zmienił
  // podciąg - dlatego otwieramy port od nowa. Bez tego nadajnik (lub pin DE)
  // zostawał odłączony do restartu stacji.
  stopPort();
  startPort();

  out["pin"]   = pin;
  out["sent"]  = listenOnly ? 0u : (unsigned)n;
  out["bytes"] = (unsigned)gn;
  if (gn) out["hex"] = diagHex(got, gn < 40 ? gn : 40);
  if (listenOnly) {
    out["listen"] = true;   // tylko nasłuch linii węzła
    // W czasie testu trzymamy magistralę, więc węzeł nie ma na co odpowiadać i
    // linia bywa cicha. Że ten pin naprawdę odbiera, pokazują liczniki bieżącej
    // wymiany: raw_rx to wszystkie bajty odebrane tym pinem od startu stacji.
    out["link_raw_rx"] = st_.rawRx;
    out["link_rx"]     = st_.rx;
    out["link_ok"]     = st_.ok;
  }
  out["reply"] = diagIsReply(got, gn, listenOnly ? 0 : seq);
  xSemaphoreGive(mtx_);
  return true;
}

bool Rs485Bus::scoutWire(const String& pinsCsv, uint32_t ms, JsonDocument& out, String& err) {
  err = "";
  if (!cfgMaster_) { err = "Diagnostyka tylko na masterze"; return false; }
  if (txBuf_ == nullptr) { err = "Brak bufora ramek"; return false; }
  if (pinRx_ < 0) { err = "Master nie ma ustawionego pinu RX magistrali"; return false; }
  if (mtx_ == nullptr || xSemaphoreTake(mtx_, pdMS_TO_TICKS(2500)) != pdTRUE) {
    err = "Magistrala zajęta - spróbuj ponownie";
    return false;
  }

  uint8_t list[32];
  int ln = diagParsePins(pinsCsv, list, 32);
  if (ln == 0) {
    // Bez podanej listy przeglądamy piny złącza magistrali i piny wolne;
    // pomijamy te zajęte przez inne układy oraz pin kierunku nadawania (DE).
    int n = 0;
    for (size_t i = 0; i < sizeof(DIAG_PINS) / sizeof(DIAG_PINS[0]) && n < 32; i++) {
      uint8_t p = DIAG_PINS[i];
      if (diagPinBusy(p)) continue;
      if (pinDe_ >= 0 && p == (uint8_t)pinDe_) continue;
      list[n++] = p;
    }
    ln = n;
  }
  if (ln == 0) {
    err = "Brak pinów do sprawdzenia (wszystkie zajęte przez inne układy)";
    xSemaphoreGive(mtx_);
    return false;
  }
  if (ms < 30) ms = 60;
  if (ms > 800) ms = 800;
  // Cały przegląd musi zmieścić się w kilku sekundach: strona www czeka w tym
  // czasie na tę odpowiedź, a długie blokowanie urywało połączenie z
  // przeglądarką ("scout" wisiał bez odpowiedzi).
  if ((uint32_t)ms * (uint32_t)ln > RS485_SCOUT_BUDGET_MS) {
    uint32_t per = RS485_SCOUT_BUDGET_MS / (uint32_t)ln;
    ms = per < 30 ? 30 : per;
  }

  uint8_t dst = scanAddr_ ? scanAddr_ : (reqAddr_ ? reqAddr_ : cfgPeer_);
  if (dst == 0) dst = 1;

  // Przewód jest jeden: RX mastera musi zostać zwolniony, zanim nadamy po
  // pinach-kandydatach. Po teście port wraca do pracy w niezmienionym stanie.
  bool wasStarted = started_;
  if (wasStarted) stopPort();
  // Po zamknięciu portu pad nadajnika zostaje w nieznanym stanie - zwalniamy go,
  // żeby pomiar poziomu jałowego nie zależał od resztek po UART0.
  pinMode(pinTx_, INPUT);
  if (pinDe_ >= 0) pinMode(pinDe_, INPUT);   // przy połączeniu 3-przewodowym DE nie bierze udziału

  // UART1 normalnie obsługuje GPS - wracamy do niego po zakończeniu testu.
  bool gpsWas = gps.enabled();
  if (gpsWas) gps.end();

  out["ms"]     = ms;
  out["rx_pin"] = (int)pinRx_;
  JsonArray arr = out["pins"].to<JsonArray>();
  int foundPin = -1;

  for (int i = 0; i < ln; i++) {
    uint8_t p = list[i];
    uint16_t seq = ++seq_;
    size_t n = buildFrame(dst, 0x01, seq, "{\"c\":\"ping\"}");

    JsonObject o = arr.add<JsonObject>();
    o["pin"] = p;
    // 19/20 to na S3 natywne USB konsoli: nadawanie po nich psuje port szeregowy,
    // a pomiar poziomu nic nie wnosi (pad obsługuje PHY USB, nie magistralę).
    if (p == 19 || p == 20) { o["usb"] = true; o["idle"] = 100; o["sent"] = 0; o["bytes"] = 0; o["reply"] = false; continue; }
    o["idle"] = diagIdleLevel(p);

    // Na pinie RX mastera tylko mierzymy poziom - tam nigdy nie nadajemy.
    if (p == pinRx_) { o["listen"] = true; o["sent"] = 0; o["bytes"] = 0; o["reply"] = false; continue; }

    Serial1.begin(cfgBaud_, SERIAL_8N1, pinRx_, p);
    delay(2);
    while (Serial1.available()) Serial1.read();
    if (n) { Serial1.write(txBuf_, n); Serial1.flush(); }

    uint8_t got[160];
    size_t gn = 0;
    uint32_t t0 = millis();
    while ((uint32_t)(millis() - t0) < ms) {
      while (Serial1.available() && gn < sizeof(got)) got[gn++] = (uint8_t)Serial1.read();
      delay(1);
    }
    Serial1.end();

    o["sent"]  = (unsigned)n;
    o["bytes"] = (unsigned)gn;
    if (gn) o["hex"] = diagHex(got, gn < 40 ? gn : 40);
    bool reply = diagIsReply(got, gn, seq);
    o["reply"] = reply;
    if (reply && foundPin < 0) foundPin = (int)p;
    // Wolne piny zostają wolne, a piny magistrali przywraca startPort() niżej.
    pinMode(p, INPUT);
  }

  if (wasStarted) startPort();
  if (gpsWas) gps.begin();
  nextPollMs_ = 0;   // po diagnostyce wracamy do normalnego odpytywania
  xSemaphoreGive(mtx_);

  out["found"] = foundPin;   // -1 = na żadnym z pinów nie było odpowiedzi
  return true;
}
