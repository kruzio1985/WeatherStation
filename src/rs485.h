/* =============================================================================
 * Stacja Pogody - stacja meteorologiczna na ESP32-S3 WROOM-1 (N16R8)
 * Copyright (c) 2026 kruzio1985 - https://github.com/kruzio1985
 * Licencja: Stacja Pogody Non-Commercial License 1.0 (plik LICENSE)
 * Użytek niekomercyjny. Kontakt: kruzio1985@users.noreply.github.com
 * =============================================================================
 */
#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

// =============================================================
//  Magistrala RS485 - drugi ESP (węzeł z zewnętrznymi czujnikami).
//
//  Ta stacja ("master") odpytywuje drugi ESP ("węzeł") po skrętce i pokazuje
//  jego czujniki na pulpicie, pierścieniu LED, w plikach CSV na karcie SD
//  oraz w Home Assistant. Węzeł nie łączy się z siecią Wi-Fi - jego dane
//  idą wyłącznie przewodem, a wszystkie ustawienia węzła (piny, kalibracja,
//  zegar, restart) zmienia się z zakładki "ESP i magistrala RS485" na masterze.
//
//  Ramka (little endian):
//    AA 55 | ver | dst | src | typ | seq(2) | len(2) | payload(JSON, len) | crc16(2)
//    * ver   = 0x01
//    * dst   = adres odbiorcy (1..247)
//    * src   = adres nadawcy
//    * typ   = 0x01 zapytanie, 0x81 odpowiedź
//    * seq   = numer kolejny zapytania (odpowiedź go powtarza)
//    * len   = długość JSON-a (max RS485_MAX_PAYLOAD)
//    * crc16 = CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF) z ver..payload
//
//  Zapytanie:  {"c":"sync"}
//  Odpowiedź:  {"ok":true,"err":"","data":{ ... }}
//
//  UART: UART0. Na tej płytce konsola idzie przez natywne USB
//  (ARDUINO_USB_CDC_ON_BOOT=1), więc UART0 jest wolny; UART1 zajmuje GPS,
//  a UART2 - czujnik pyłu PMS5003.
// =============================================================

#define RS485_MAX_PAYLOAD    8192   // maks. długość JSON-a w jednej ramce
#define RS485_FRAME_OVERHEAD 12     // nagłówek + CRC
#define RS485_BUF_SIZE       (RS485_MAX_PAYLOAD + RS485_FRAME_OVERHEAD)
#define RS485_PROTO_VER      0x01

// Lista kanałów węzła bywa większa niż jedna ramka (przy komplecie czujników
// to ~30 kB JSON-a), dlatego "chan" i "vals" odpowiadają stronami: pytanie
// dostaje {"o":<od czego>,"n":<ile>}, a odpowiedź "total"/"next"/"done".
#define RS485_CHAN_PAGE      12   // kanałów (z metadanymi) w jednej stronie
#define RS485_VAL_PAGE       48   // samych wartości w jednej stronie
#define RS485_MAX_PAGES      24   // bezpiecznik: tyle stron maksymalnie pobieramy
#define RS485_META_PAGES_SYNC 4   // metadane: tyle stron na jedną synchronizację
                                  // (tyle wystarcza, by nie blokować pętli na dłużej)

// Limit czasu na duże ramki ("sync", "chan"): przy 19200 b/s strona metadanych
// leci ~4 s, a węzeł musi jeszcze zbudować odpowiedź. Zwykłe 400 ms wystarcza
// na "ping"/"vals", ale dużą ramkę ucinało w połowie ("odpowiedź węzła za długa").
#define RS485_BIG_TIMEOUT_MS  3000

// Ile węzłów master obsługuje jednocześnie. Każdy węzeł to jedno małe ESP
// (np. ESP32-C3 SuperMini) zbierające własne czujniki; master odpytuje je po
// kolei na tej samej parze przewodów A-A / B-B. Protokół dopuszcza adresy
// 1..247, ale przy 8 węzłach pełny obieg trwa już kilkadziesiąt sekund
// (jeden węzeł = kilka ramek), dlatego lista jest ograniczona.
#define RS485_MAX_NODES      8
// Najwyższy adres sprawdzany przez skan magistrali ("szukaj węzłów").
#define RS485_SCAN_MAX_ADDR  32

// Identyfikacja i stan drugiego ESP (z odpowiedzi na "sync" / "hello")
struct Rs485PeerInfo {
  bool     valid = false;
  String   name;          // nazwa urządzenia (device_name)
  String   host;          // nazwa sieciowa
  String   fw;            // wersja firmware
  String   build;         // data kompilacji
  String   mac;
  String   ip;
  String   role;          // "master" / "węzeł"
  uint8_t  addr = 0;
  uint32_t uptimeS = 0;
  time_t   epoch = 0;
  int32_t  rssi = 0;
  uint32_t heap = 0;
  uint32_t psram = 0;
  uint32_t bootCount = 0;
  String   resetReason;
  bool     sd = false;
  uint32_t errors = 0;
  uint32_t warns = 0;
  uint32_t channels = 0;   // kanały zgłoszone przez węzeł
  uint32_t detected = 0;   // w tym potwierdzone sprzętowo
  String   board;          // wykryta płytka węzła ("s3n16r8", "c3mini", ...)
  String   chip;           // "ESP32-S3" / "ESP32-C3" / "ESP32-C6"
  uint32_t flashMb = 0;
  uint32_t psramMb = 0;
};

struct Rs485Stats {
  uint32_t tx = 0;          // wysłane ramki
  uint32_t rx = 0;          // odebrane poprawne ramki
  uint32_t ok = 0;          // udane wymiany (zapytanie -> odpowiedź)
  uint32_t errCrc = 0;      // błędne CRC
  uint32_t errFrame = 0;    // uszkodzona ramka (nagłówek / długość / wersja)
  uint32_t errTimeout = 0;  // brak odpowiedzi
  // Surowe bajty z linii - liczone dla KAŻDEGO bajtu, także takiego, który nie
  // układa się w ramkę. Bez tego "cisza na linii" i "śmieci na linii" wyglądają
  // identycznie, bo rx/errCrc/errFrame liczą się dopiero po nagłówku AA 55.
  uint32_t rawRx = 0;
  uint32_t lastRawMs = 0;   // millis() ostatniego bajtu (0 = nic nie przyszło)
  uint8_t  sniff[8] = {};   // pierwsze odebrane bajty (podgląd na stronie)
  uint8_t  sniffN = 0;
  uint32_t retries = 0;     // ponowienia (nieudane próby)
  uint32_t latencyMs = 0;   // czas ostatniej wymiany
  uint32_t lastOkMs = 0;    // millis() ostatniej udanej wymiany (0 = nigdy)
  uint32_t lastErrMs = 0;
  String   lastError;
};

// Stan jednego węzła na magistrali. Master trzyma listę takich wpisów
// (adresy z ustawienia "rs485_nodes") i odpytywany jest zawsze ten, który
// najdłużej czekał - dzięki temu każdy węzeł dostaje swoje okno w pętli.
struct Rs485Node {
  uint8_t  addr = 0;
  Rs485PeerInfo info;          // identyfikacja i stan z ostatniej odpowiedzi
  Rs485Stats    st;            // statystyki wymian z tym węzłem
  uint32_t lastSyncMs = 0;     // millis() ostatniej próby odpytywania
  uint32_t lastOkMs = 0;       // millis() ostatniej udanej wymiany (0 = nigdy)
  uint32_t metaFrom = 0;       // ile kanałów tego węzła już znamy
  uint32_t metaTotal = 0;      // ile kanałów węzeł w ogóle ma
  bool     valsOk = true;      // węzeł zna polecenie "vals"
  uint16_t failStreak = 0;     // ile odpytań z rzędu się nie udało
};

class Rs485Bus {
 public:
  void begin();      // start magistrali wg ustawień (wołane z setup())
  void loop();       // master: odpytywanie; węzeł: obsługa ramek
  void reload();     // ponowne wczytanie ustawień (piny, prędkość, rola)

  bool enabled() const { return cfgEnabled_; }
  bool isMaster() const { return cfgMaster_; }
  bool started() const { return started_; }
  const char* roleName() const;

  // ----------------------------------------------------------
  //  Master
  // ----------------------------------------------------------
  // Wymiana z węzłem. "argsJson" to gotowa treść sekcji "a" ("" = brak),
  // "out" dostaje całą odpowiedź {"ok":..,"err":..,"data":{..}} już
  // sparsowaną (dane w out["data"]), "err" treść błędu ("" = sukces).
  bool request(const String& cmd, const String& argsJson, JsonDocument& out, String& err,
               uint32_t timeoutMs = 0);
  // To samo, ale z wymuszonym adresem odbiorcy (dst == 0 => adres bieżącej
  // wymiany). Używane przez stronę www, żeby wymiana z wybranym węzłem nie
  // przestawiała stanu aktualnie odpytywanego modułu.
  bool requestTo(uint8_t dst, const String& cmd, const String& argsJson, JsonDocument& out,
                 String& err, uint32_t timeoutMs = 0);
  bool online() const;
  void pollNow();
  int  scan(uint8_t* found, int maxFound);   // szuka węzłów pod adresami 1..32
  bool selftest(String& info);               // test protokołu bez magistrali

  // ----------------------------------------------------------
  //  Diagnostyka przewodu (tylko master)
  // ----------------------------------------------------------
  // Nasłuch linii magistrali. Dla pinu nadajnika mastera mierzy poziom jałowy
  // (idle) i nadaje "na żywo": w trakcie wysyłania bajtów 0xAA próbkuje ten pin,
  // więc wynik "drive" (procent próbek w LOW) pokazuje, czy pad nadajnika
  // naprawdę wypuszcza dane na przewód. Zdrowa linia daje idle 100 i drive
  // kilkadziesiąt %, drive 0 % oznacza odłączony pad/port, a idle 0 - że coś
  // zewnętrznego trzyma przewód w GND.
  // Dla innego pinu UART1 nasłuchuje go i wysyła ramkę testową, raportując
  // liczbę odebranych bajtów, ich treść (hex) i czy przyszła odpowiedź węzła.
  bool sniffTx(int pin, uint32_t ms, JsonDocument& out, String& err);
  // Szukanie przewodu do węzła: master nadaje po kolei po podanych pinach
  // (domyślnie lista pinów wolnych - piny zajęte przez inne układy i pin DE są
  // pomijane) i słucha odpowiedzi na swoim RX. Dla każdego pinu raportuje
  // poziom jałowy, liczbę bajtów i treść odpowiedzi. Cały przegląd trwa
  // najwyżej RS485_SCOUT_BUDGET_MS, żeby nie blokować strony www.
  bool scoutWire(const String& pinsCsv, uint32_t ms, JsonDocument& out, String& err);
  // Restart samego portu magistrali (bez restartu stacji): zamyka i otwiera
  // UART0 od nowa, więc nadajnik, odbiornik i pin DE wracają na swoje miejsca.
  bool restartPort();

  // ----------------------------------------------------------
  //  Węzły RS485 (kilka modułów na jednej magistrali)
  // ----------------------------------------------------------
  int  nodeCount() const { return nodeCnt_; }          // 0 = wykrywanie automatyczne
  const Rs485Node& node(int idx) const;                // idx < nodeCount()
  int  nodeIndexByAddr(uint8_t addr) const;
  uint8_t activeAddr() const { return activeAddr_; }   // węzeł odpytywany teraz
  // Dodanie / usunięcie węzła z listy (zapis w ustawieniach, bez restartu).
  bool addNode(uint8_t addr, String& err);
  bool removeNode(uint8_t addr, String& err);
  // Zmiana adresu węzła: master nadaje nowy adres po RS485, węzeł zapisuje go
  // w swojej pamięci i restartuje się. "mac" (opcjonalny) wskazuje konkretne
  // urządzenie, gdy pod jednym adresem odpowiada kilka świeżych modułów.
  bool assignAddr(uint8_t curAddr, const String& mac, uint8_t newAddr, String& err);

  // Pełna lista kanałów węzła złożona ze stron ("chan") - używane przez
  // /api/node?c=chan, bo jedna ramka nie zmieści wszystkich kanałów.
  // idx < 0 => węzeł odpytywany teraz (albo pierwszy z listy).
  bool peerChannelsJson(JsonDocument& out, String& err) { return peerChannelsJson(-1, out, err); }
  bool peerChannelsJson(int idx, JsonDocument& out, String& err);

  String   stateJson();        // GET /api/rs485
  uint32_t cacheAgeS() const;  // ile sekund od ostatniej udanej synchronizacji
  const Rs485PeerInfo& peer() const { return peer_; }
  const Rs485Stats& stats() const { return st_; }

  // ----------------------------------------------------------
  //  Węzeł (drugi ESP)
  // ----------------------------------------------------------
  void handleSlave();          // odbiór ramek i odpowiedzi (wołane z loop())

 private:
  // ---- master: wykrywanie adresu węzła ----
  // Pojedyncze zapytanie "ping" do podanego adresu (bez zmiany stanu magistrali).
  bool pingAddr(uint8_t addr, uint32_t timeoutMs);
  // Sprawdza w tle kilka kolejnych adresów (bez blokowania strony www na długo).
  void discoverPass(uint32_t now);
  // Zapisuje wykryty adres węzła w ustawieniach (żeby przetrwał restart).
  void adoptPeer(uint8_t addr);
  // Przekazuje węzłowi czas, gdy ten jeszcze nie ma ustawionego zegara.
  void pushTimeToPeer();

  // ---- master: obsługa listy węzłów ----
  void nodesLoad();                    // lista adresów z ustawień -> nodes_
  bool nodesSave();                    // lista nodes_ -> ustawienia (NVS)
  void nodeLoad(int idx);              // stan węzła -> roboczy zestaw (peer_, st_, ...)
  void nodeSave(int idx);              // roboczy zestaw -> stan węzła
  int  nodeSelect(int idx);            // ustawia węzeł roboczy (-1 = aktywny/pierwszy)
  int  pickNode();                     // węzeł, który najdłużej czekał na odpytywanie
  void syncNode(int idx);              // odpytywanie jednego węzła (jak masterSync dla jednego)
  void nodeJsonInto(int idx, JsonObject o);  // jeden węzeł do JSON-a dla strony www
  bool nodeOnline(uint8_t addr) const; // czy węzeł odpowiadał niedawno
  bool nodeFresh(const Rs485Node& nd) const;
  static bool nodesValidAddr(uint8_t addr);
  static String deviceMac();           // MAC tej płytki (ten sam format wszędzie)

  // ---- stan konfiguracji (kopiowany przy begin/reload) ----
  bool     cfgEnabled_ = false;
  bool     cfgMaster_ = true;
  uint8_t  cfgAddr_ = 1;
  uint8_t  cfgPeer_ = 2;
  uint32_t cfgBaud_ = 19200;
  uint16_t cfgPollS_ = 30;
  uint16_t cfgTimeoutMs_ = 400;
  uint8_t  cfgRetries_ = 2;
  int8_t   pinRx_ = -1, pinTx_ = -1, pinDe_ = -1;
  bool     started_ = false;

  // ---- bufory (PSRAM) ----
  uint8_t* txBuf_ = nullptr;   // budowana ramka wychodząca
  uint8_t* rxBuf_ = nullptr;   // ramka w odbiorze
  uint16_t seq_ = 0;
  SemaphoreHandle_t mtx_ = nullptr;

  // ---- master ----
  uint32_t nextPollMs_ = 0;
  uint8_t  scanAddr_ = 0;    // != 0: trwa skanowanie (wstrzymuje synchronizację)
  uint16_t failStreak_ = 0;  // ile synchronizacji z rzędu się nie udało
  uint32_t nextDiscoverMs_ = 0;   // cooldown wykrywania adresu węzła
  uint32_t lastNoPeerLogMs_ = 0;  // ograniczenie komunikatu "brak węzła"
  Rs485PeerInfo peer_;
  Rs485Stats    st_;
  uint32_t lastSyncMs_ = 0;  // millis() ostatniego odpytywania węzła
  uint32_t peerMetaFrom_ = 0;   // ile kanałów węzła już znamy (metadane)
  uint32_t peerMetaTotal_ = 0;  // ile kanałów węzeł w ogóle ma
  bool     peerValsOk_ = true;  // węzeł odpowiada na "vals" (starszy = nie)

  // ---- master: lista węzłów ----
  // peer_/st_/peerMetaFrom_/peerMetaTotal_/peerValsOk_ opisują TYLKO węzeł
  // wskazany przez activeAddr_ (jeden wspólny zestaw roboczy). Przed
  // odpytywaniem węzła dane wchodzą z nodes_[i] (nodeLoad), a po wymianie
  // wracają (nodeSave) - dzięki temu reszta programu nadal widzi "jednego
  // węzła" i nie trzeba zmieniać interfejsu pulpitu.
  Rs485Node nodes_[RS485_MAX_NODES];
  int      nodeCnt_ = 0;        // ile wpisów w nodes_ (0 = lista pusta)
  int      loadedIdx_ = -1;     // który wpis nodes_ jest w zestawie roboczym
  uint8_t  activeAddr_ = 0;     // adres węzła w zestawie roboczym (0 = brak)
  uint8_t  reqAddr_ = 0;        // adres bieżącej wymiany (0 = adres z "primary")
  uint8_t  pendingAddr_ = 0;    // węzeł: nowy adres do zapisania po odpowiedzi
  uint8_t  discoverNext_ = 1;   // kolejny adres do sprawdzenia w tle
  uint16_t discoverMiss_ = 0;   // pełne obiegi skanu bez trafienia

  // ---- odbiór ----
  // Parser ramek jest prostą maszyną stanu - te same bajty obsługują węzeł
  // (odbiór zapytań) i master (odbiór odpowiedzi), a także test protokołu.
  struct Parser {
    uint16_t pos = 0;    // ile bajtów bieżącej ramki już odebrano
    uint16_t flen = 0;   // długość payloadu z nagłówka
    uint8_t  ver = 0, dst = 0, src = 0, type = 0;
    uint16_t seq = 0;
  };
  struct Resp {
    bool     ok = false;   // true = poprawna ramka (CRC + wersja + długość)
    String   err;
    uint8_t  src = 0, dst = 0, type = 0;
    uint16_t seq = 0;
    String   payload;
  };
  Parser parser_;

  // ---- test protokołu (bez magistrali) ----
  String selftestInfo_;

  void startPort();
  void stopPort();
  void writeTx(const uint8_t* p, size_t n);
  static uint16_t crc16(const uint8_t* p, size_t n);
  size_t buildFrame(uint8_t dst, uint8_t type, uint16_t seq, const String& payload);
  // Karmienie parsera pojedynczymi bajtami; gotowa ramka trafia do out.
  // "stats" = liczniki węzła, z którym trwa wymiana (nullptr => zestaw roboczy).
  void frameFeed(Parser& p, uint8_t* buf, uint8_t b, Resp& out, Rs485Stats* stats = nullptr);
  bool readResponse(uint32_t timeoutMs, uint16_t wantSeq, Resp& out, String& err,
                    Rs485Stats* stats = nullptr);
  void masterSync();
  void applyPeerStatus(JsonObject d);
  void applyPeerChannels(JsonArray arr);
  void applyPeerValues(JsonArray arr);
  // Ile kanałów zmieści się w jednej ramce przy obecnej prędkości i limicie
  // czasu - strona liczona tak, żeby odpowiedź zdążyła dojść przed timeoutem.
  uint32_t pageChans(uint32_t bytesPerChan) const;
  // Dociąga brakujące metadane kanałów węzła (gdy "sync" przyciął listę).
  void fetchPeerMeta(uint32_t from);
  // Świeże wartości wszystkich kanałów węzła (osobne, krótkie ramki).
  bool fetchPeerValues();

  // ---- obsługa zapytań na węźle ----
  String slaveHandle(const String& payload, uint8_t src, bool& ok, String& err);

  void identityInto(JsonObject o);    // nazwa, IP, rola, adres, wersja
  String selfIdentityJson();   // {"name":...,"addr":...} - wspólne dla hello/ping
  String selfStatusJson();     // stan węzła + kanały (odpowiedź na "sync")
  String selfChannelsJson(uint32_t off, uint32_t cnt);   // strona kanałów ("chan")
  String selfValuesJson(uint32_t off, uint32_t cnt);     // strona wartości ("vals")
  String pinsCompactJson();    // przypisania pinów (same klucze + numery GPIO)
  String configJson();         // pełne ustawienia węzła
  void   logTailInto(JsonObject o, uint32_t n);
};

extern Rs485Bus rs485;
