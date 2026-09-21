/* =============================================================================
 * Stacja Pogody - stacja meteorologiczna na ESP32-S3 WROOM-1 (N16R8)
 * Copyright (c) 2026 kruzio1985 - https://github.com/kruzio1985
 * Licencja: Stacja Pogody Non-Commercial License 1.0 (plik LICENSE)
 * Użytek niekomercyjny. Kontakt: kruzio1985@users.noreply.github.com
 * =============================================================================
 */
#include "ota.h"
#include "app_info.h"
#include "syslog.h"
#include "config.h"
#include "flashfix.h"
#include "sd_card.h"
#include <ArduinoOTA.h>
#include <Update.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <esp_ota_ops.h>
#include <esp_image_format.h>
#include <esp_partition.h>
#include <mbedtls/sha256.h>
#include <stdlib.h>
#include <string.h>

OtaManager ota;
volatile bool g_otaInProgress = false;

#define OTA_PORT 3232

// Aktualizacja przez www jest chroniona własnym nagłówkiem z hasłem.
// Własny nagłówek (a nie Basic auth) dodatkowo utrudnia atak CSRF -
// przeglądarka nie wyśle go w żądaniu z obcej strony bez zgody serwera (CORS).
static const char* OTA_HEADER = "X-OTA-Key";

// Katalog na karcie SD, w którym trzymamy binarki firmware wszystkich płytek
// (master S3, ESP32-C3 SuperMini / NodeMCU C3, ESP32-C6). Pliki wgrywa się
// ze strony www (zakładka "Aktualizacja" -> "Firmware na karcie SD"), a na PC
// pozostają w dist\ (tools/build_release.ps1).
#define OTA_SD_DIR "/fw"

// ------------------------------------------------------------
//  Stan uploadu przez www
// ------------------------------------------------------------
static volatile bool s_rebootPending = false;
static unsigned long s_rebootAt = 0;

static bool   s_upOk = false;
static int    s_upStatus = 200;
static size_t s_upBytes = 0;
static String s_upError;
static int    s_upCommand = U_FLASH;   // U_FLASH albo U_SPIFFS - potrzebne przy finalizacji
static bool   s_netFs = false;         // czy aktualizacja przez sieć dotyczy systemu plików

// Pilnowanie wgrywania z www. Gdy przeglądarka zamknie kartę albo zerwie się Wi-Fi,
// serwer asynchroniczny zamyka gniazdo bez zdarzenia końcowego: Update zostaje otwarty,
// a g_otaInProgress na stałe blokuje pętlę główną (czujniki, przycisk BOOT, restart z www).
// Dlatego każdy odebrany fragment odświeża czas, a cisza dłuższa niż OTA_UP_IDLE_MS
// i rozłączenie klienta przerywają zapis.
// Zmienne współdzielone z zadaniem serwera (zapis robi ono, czyta pętla główna) muszą być
// volatile - inaczej pętla potrafi odczytać nieaktualny znacznik czasu i przerwać wgranie
// w trakcie normalnej transmisji (objawiało się jako "cisza 0 ms" w dzienniku).
static volatile bool          s_upActive = false;
static volatile unsigned long s_upLastMs = 0;
static volatile unsigned long s_upIdleSince = 0;  // potwierdzenie ciszy drugim pomiarem
static String        s_upAbort;        // powód ostatniego przerwania (diagnostyka)
#define OTA_UP_IDLE_MS 20000UL
#define OTA_UP_IDLE_CONFIRM_MS 500UL

// Przywrócenie systemu plików po przerwanym wgraniu. Bez formatowania - uszkodzenie
// montowania nie może kasować zawartości partycji z interfejsem www.
static void remountLittleFs(const char* ctx) {
  if (!LittleFS.begin()) {
    LOG_E("%s: nie można zamontować LittleFS (wymagane ponowne wgranie systemu plików)", ctx);
  }
}

// Zwolnienie zawieszonego zapisu: przerwane połączenie albo resztka po nieudanej próbie.
static void abortUpload(const char* why) {
  if (!Update.isRunning() && !s_upActive) return;

  Update.abort();
  if (s_upCommand == U_SPIFFS) remountLittleFs("Aktualizacja przerwana");

  s_upActive = false;
  s_upOk = false;
  s_upStatus = 500;
  s_upAbort = why;
  if (s_upError.length() == 0) s_upError = why;
  g_otaInProgress = false;
  LOG_W("Aktualizacja przerwana: %s (odebrano %u B, teraz %lu ms, ostatnie dane %lu ms)",
        why, (unsigned)s_upBytes, (unsigned long)millis(), (unsigned long)s_upLastMs);
}

static bool authorized(AsyncWebServerRequest* r) {
  String pass = config.otaPassword();
  if (pass.length() == 0) return true;  // brak hasła w konfiguracji = OTA otwarte
  if (!r->hasHeader(OTA_HEADER)) return false;
  const AsyncWebHeader* h = r->getHeader(OTA_HEADER);
  return h && h->value() == pass;
}

static void scheduleReboot(unsigned long delayMs) {
  s_rebootAt = millis() + delayMs;
  s_rebootPending = true;
}

// ------------------------------------------------------------
//  Weryfikacja i aktywacja obrazu
// ------------------------------------------------------------
// Tło problemu (dowody pomiarowe: docs/OTA.md):
// Rdzeń Arduino (IDF 4.4) przy aktywacji nowego programu sprawdza dodatkowo skrót SHA-256
// dopisany na końcu pliku firmware.bin. Odczyt flasha na tej płytce zwracał jednak błędne dane,
// gdy żądanie nie mieściło się w jednym bloku 32-bajtowym, a skrót obrazu IDF czyta właśnie tak
// (32 bajty spod adresu "start + długość obrazu", dla naszego firmware 0x10000 + 1538224 =
// 0x187B30, czyli 16 mod 32). Porównanie skrótu wypadało więc źle dla KAŻDEGO obrazu i
// Update.end() kończyło się błędem "Could Not Activate The Firmware", choć plik był zapisany
// w całości i był poprawny.
//
// Sam błąd odczytu naprawia flashfix.cpp (patrz src/flashfix.cpp) - po tej poprawce aktywacja
// przechodzi zwykłą drogą. Funkcje poniżej robią dwie rzeczy:
//   * sprawdzają obraz po swojemu (długość, własne SHA-256, odczyt surowy vs poprawiony) -
//     to dane dla endpointu /api/ota/diag,
//   * jeśli rdzeń odrzuci obraz TYLKO z powodu skrótu, a nasze SHA-256 zgadza się z tym,
//     co leży w flashu (czyli obraz jest kompletny), wyłączają wpis hash_appended
//     w nagłówku i próbują ponownie.
//
// Zabezpieczenie na wypadek zaniku zasilania: otadata wskazuje w tym momencie jeszcze na stary
// program, a loader (bootloader_utility_load_boot_image) przy nieudanej weryfikacji partycji
// uruchamia poprzednią - czyli najgorszym skutkiem jest start na starej wersji, a nie brak
// działania stacji.

// Wynik ostatniej aktywacji przechowywany w pamięci RTC, żeby dało się go odczytać
// także PO restarcie (dziennik systemowy obejmuje tylko bieżące uruchomienie).
#define OTA_REC_MAGIC 0x4F544131UL   // "OTA1"

enum OtaActSource : uint8_t {
  OTA_SRC_UNKNOWN = 0,
  OTA_SRC_UPLOAD  = 1,   // wgrywanie przez stronę www
  OTA_SRC_NET     = 2,   // ArduinoOTA / espota
  OTA_SRC_API     = 3,   // POST /api/ota/activate
};

typedef struct {
  uint32_t magic;
  uint32_t sum;
  uint32_t err;         // esp_err_t z esp_ota_set_boot_partition (0 = OK)
  uint32_t verifyErr;   // esp_err_t z esp_image_verify
  uint32_t span;        // długość obrazu bez dopisanego skrótu
  uint8_t  shaMatch;
  uint8_t  hashAppended;
  uint8_t  patched;
  uint8_t  source;
} OtaActRecord;

static RTC_NOINIT_ATTR OtaActRecord s_otaRec;

static uint32_t otaRecSum(const OtaActRecord& r0) {
  OtaActRecord r = r0;
  r.sum = 0;
  const uint8_t* p = (const uint8_t*)&r;
  uint32_t s = 0;
  for (size_t i = 0; i < sizeof(r); i++) s += p[i];
  return s;
}

static void otaRecSave(uint32_t err, uint32_t verifyErr, uint32_t span,
                       bool shaMatch, bool hashAppended, bool patched, uint8_t source) {
  OtaActRecord r;
  memset(&r, 0, sizeof(r));
  r.magic = OTA_REC_MAGIC;
  r.err = err;
  r.verifyErr = verifyErr;
  r.span = span;
  r.shaMatch = shaMatch ? 1 : 0;
  r.hashAppended = hashAppended ? 1 : 0;
  r.patched = patched ? 1 : 0;
  r.source = source;
  r.sum = otaRecSum(r);
  s_otaRec = r;
}

static bool otaRecLoad(OtaActRecord* out) {
  if (s_otaRec.magic != OTA_REC_MAGIC || s_otaRec.sum != otaRecSum(s_otaRec)) return false;
  *out = s_otaRec;
  return true;
}

struct OtaSlotReport {
  bool     headerOk;
  bool     hashAppended;
  bool     verifyOk;
  bool     shaCalculated;
  bool     shaMatch;
  bool     rawReadOk;
  bool     rawMatch;
  uint32_t verifyErr;
  uint32_t verifyLen;
  uint32_t span;
  uint8_t  header[24];
  uint8_t  digestCalc[32];
  uint8_t  digestFlash[32];
  uint8_t  digestRaw[32];
};

static void otaHex(const uint8_t* data, size_t len, char* out) {
  static const char* digits = "0123456789abcdef";
  for (size_t i = 0; i < len; i++) {
    out[i * 2] = digits[data[i] >> 4];
    out[i * 2 + 1] = digits[data[i] & 15];
  }
  out[len * 2] = 0;
}

// Długość obrazu bez dopisanego skrótu, liczona wg reguł loadera (IDF esp_image_format.c):
// nagłówek (24 B) + nagłówki i dane kolejnych segmentów + suma kontrolna dopełniona zerami
// do pełnych 16 bajtów.
static bool imageSpan(const esp_partition_t* part, uint32_t* span,
                      uint8_t* header, bool* hashAppended) {
  uint32_t hdr[6];
  if (esp_partition_read(part, 0, hdr, sizeof(hdr)) != ESP_OK) return false;

  const uint8_t* h = (const uint8_t*)hdr;
  if (header) memcpy(header, h, 24);
  if (h[0] != ESP_IMAGE_HEADER_MAGIC) return false;
  if (h[1] == 0 || h[1] > ESP_IMAGE_MAX_SEGMENTS) return false;
  if (hashAppended) *hashAppended = (h[23] != 0);

  uint32_t off = 24;
  for (uint8_t seg = 0; seg < h[1]; seg++) {
    uint32_t sh[2];
    if (esp_partition_read(part, off, sh, sizeof(sh)) != ESP_OK) return false;
    const uint32_t addr = sh[0];
    const uint32_t len = sh[1];
    if ((addr & 0xFFFF0000) == 0 || (addr & 0xFFFF0000) == 0xFFFF0000) return false;
    if ((len & 3) != 0 || len > 16u * 1024u * 1024u) return false;
    off += 8 + len;
    if (off > part->size) return false;
  }

  off = (off + 1 + 15) & ~(uint32_t)15;
  if (off + 32 > part->size) return false;

  *span = off;
  return true;
}

// Własne SHA-256 obrazu - dokładnie ten sam materiał, który liczy IDF.
static bool imageSha(const esp_partition_t* part, uint32_t span, uint8_t* out) {
  uint8_t* buf = (uint8_t*)malloc(4096);
  if (!buf) return false;

  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  bool ok = (mbedtls_sha256_starts_ret(&ctx, 0) == 0);

  for (uint32_t off = 0; ok && off < span; off += 4096) {
    uint32_t n = span - off;
    if (n > 4096) n = 4096;
    ok = (esp_partition_read(part, off, buf, n) == ESP_OK) &&
         (mbedtls_sha256_update_ret(&ctx, buf, n) == 0);
  }
  if (ok) ok = (mbedtls_sha256_finish_ret(&ctx, out) == 0);

  mbedtls_sha256_free(&ctx);
  free(buf);
  return ok;
}

// Kontrola samego liczenia skrótu na znanym wzorcu: gdyby zawiodło, wynikom naszych
// skrótów nie można ufać - stąd osobne pole w diagnostyce.
static const char* shaSelfTest() {
  static const char* expected =
      "c8f5d0341d54d951a71b136e6e2afcb14d11ed8489a7ae126a8fee0df6ecf193";
  static uint8_t pattern[4096];
  static bool patternReady = false;
  if (!patternReady) {
    for (size_t i = 0; i < sizeof(pattern); i++) pattern[i] = (uint8_t)i;
    patternReady = true;
  }

  uint8_t out[32];
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  bool ok = (mbedtls_sha256_starts_ret(&ctx, 0) == 0);
  for (size_t off = 0; ok && off < sizeof(pattern); off += 1024) {
    ok = (mbedtls_sha256_update_ret(&ctx, pattern + off, 1024) == 0);
  }
  if (ok) ok = (mbedtls_sha256_finish_ret(&ctx, out) == 0);
  mbedtls_sha256_free(&ctx);
  if (!ok) return "error";

  char hex[65];
  otaHex(out, 32, hex);
  return (strcmp(hex, expected) == 0) ? "ok" : "fail";
}

static void otaSlotReport(const esp_partition_t* part, OtaSlotReport& rep) {
  memset(&rep, 0, sizeof(rep));
  if (!part) return;

  rep.headerOk = imageSpan(part, &rep.span, rep.header, &rep.hashAppended);
  rep.shaCalculated = rep.headerOk && imageSha(part, rep.span, rep.digestCalc);

  esp_partition_pos_t pos;
  pos.offset = part->address;
  pos.size = part->size;
  esp_image_metadata_t meta;
  memset(&meta, 0, sizeof(meta));
  esp_err_t verr = esp_image_verify(ESP_IMAGE_VERIFY_SILENT, &pos, &meta);
  rep.verifyErr = (uint32_t)verr;
  rep.verifyOk = (verr == ESP_OK);
  rep.verifyLen = meta.image_len;

  if (!rep.headerOk) return;

  const bool readOk = (esp_partition_read(part, rep.span, rep.digestFlash, 32) == ESP_OK);

  // Ten sam odczyt, ale oryginalnym (niepoprawionym) sterownikiem - pokazuje, czy różnica
  // bierze się z odczytu flasha, czy z samego liczenia skrótu.
  flashReadFixSuspend();
  rep.rawReadOk = (esp_partition_read(part, rep.span, rep.digestRaw, 32) == ESP_OK);
  flashReadFixResume();

  rep.shaMatch = rep.shaCalculated && readOk &&
                 memcmp(rep.digestCalc, rep.digestFlash, 32) == 0;
  rep.rawMatch = rep.rawReadOk && readOk &&
                 memcmp(rep.digestRaw, rep.digestFlash, 32) == 0;
}

// Wpis hash_appended w nagłówku obrazu (bajt 23) wyłącza sprawdzanie dopisanego SHA-256;
// zostaje sama suma kontrolna obrazu. Piszemy wyłącznie w partycji zapasowej, nigdy
// w programie, z którego stacja właśnie wystartowała.
static bool otaPatchWithoutHash(const esp_partition_t* part) {
  uint8_t* head = (uint8_t*)malloc(4096);
  if (!head) {
    LOG_E("Aktywacja: brak pamięci na poprawkę nagłówka obrazu");
    return false;
  }

  bool ok = false;
  if (esp_partition_read(part, 0, head, 4096) == ESP_OK &&
      head[0] == ESP_IMAGE_HEADER_MAGIC && head[23] != 0) {
    head[23] = 0;
    // Flash zapisuje się całymi sektorami, stąd odczyt, wymazanie i ponowny
    // zapis pierwszego sektora obrazu (ok. 10 ms).
    ok = (esp_partition_erase_range(part, 0, 4096) == ESP_OK) &&
         (esp_partition_write(part, 0, head, 4096) == ESP_OK);
  }
  free(head);
  return ok;
}

// Aktywacja obrazu. Dopisany SHA-256 jest sprawdzany dwa razy: przez aplikację
// (esp_ota_set_boot_partition) i przez loader przy starcie. Ten drugi odczyt omija naszą
// poprawkę, więc gdy skrót leży pod adresem nie wyrównanym do 32 B (długość obrazu 16 mod 32),
// kompletny obraz można stracić przy starcie - wtedy, po własnej kontroli SHA-256, wyłączamy
// w nagłówku sprawdzanie dopisanego skrótu (suma kontrolna obrazu zostaje sprawdzana dalej).
static bool otaActivate(const esp_partition_t* part, uint8_t source, String* detail) {
  if (!part) {
    if (detail) *detail = "brak partycji zapasowej";
    return false;
  }

  uint32_t span = 0;
  uint8_t header[24];
  bool hashAppended = false;
  const bool spanKnown = imageSpan(part, &span, header, &hashAppended);

  OtaSlotReport rep;
  bool repKnown = false;
  bool patched = false;

  if (spanKnown && hashAppended && (span % 32) != 0) {
    otaSlotReport(part, rep);
    repKnown = true;
    if (rep.shaMatch && otaPatchWithoutHash(part)) {
      patched = true;
      LOG_W("Aktywacja: skrót obrazu (%u B) leży pod adresem 16 mod 32 - wyłączam dopisany "
            "SHA-256, żeby loader nie odrzucił kompletnego obrazu przy starcie",
            (unsigned)span);
    } else if (!rep.shaMatch) {
      LOG_W("Aktywacja: skrót pod adresem nie wyrównanym, ale obraz nie zgadza się ze skrótem "
            "- nagłówek zostaje bez zmian");
    }
  }

  esp_err_t err = esp_ota_set_boot_partition(part);
  if (err == ESP_OK) {
    otaRecSave(0, 0, span, repKnown && rep.shaMatch, hashAppended, patched, source);
    LOG_I("Aktywacja: obraz w %s sprawdzony i ustawiony jako startowy%s", part->label,
          patched ? " (bez dopisanego SHA-256)" : "");
    return true;
  }

  if (!repKnown) {
    otaSlotReport(part, rep);
    repKnown = true;
  }
  LOG_W("Aktywacja: %s odrzucona - %s (%d); obraz %u B, skrót %s, dopisany skrót %s",
         part->label, esp_err_to_name(err), (int)err, (unsigned)rep.span,
         rep.shaCalculated ? (rep.shaMatch ? "zgodny" : "niezgodny") : "niepoliczony",
         rep.hashAppended ? "jest" : "brak");

  if (err == ESP_ERR_OTA_VALIDATE_FAILED && rep.headerOk && rep.hashAppended && rep.shaMatch) {
    LOG_W("Aktywacja: obraz kompletny - wyłączam dopisany SHA-256 i próbuję ponownie");
    if (otaPatchWithoutHash(part)) {
      patched = true;
      esp_err_t err2 = esp_ota_set_boot_partition(part);
      if (err2 == ESP_OK) {
        otaRecSave(0, rep.verifyErr, rep.span, true, true, true, source);
        LOG_I("Aktywacja: obraz w %s ustawiony jako startowy (bez dopisanego SHA-256)",
              part->label);
        return true;
      }
      LOG_E("Aktywacja: druga próba nieudana - %s (%d)", esp_err_to_name(err2), (int)err2);
      err = err2;
    }
  }

  if (strcmp(shaSelfTest(), "ok") != 0) {
    LOG_E("Aktywacja: test SHA-256 nie przeszedł (%s) - sprawdź /api/ota/diag", shaSelfTest());
  }

  otaRecSave((uint32_t)err, rep.verifyErr, rep.span, rep.shaMatch, rep.hashAppended, patched,
             source);
  if (detail) *detail = String(esp_err_to_name(err)) + " (" + String((int)err) + ")";
  LOG_E("Aktywacja: nie udało się ustawić obrazu z %s jako startowego - zostaje poprzednia wersja",
        part->label);
  return false;
}

// Udana aktualizacja nie przechodzi przez otaActivate (Update.end sam sprawdza obraz i ustawia
// partycję startową), więc wynik zapisujemy osobno - dzięki temu diagnostyka pokazuje stan po
// każdej próbie, także po tej udanej.
static void otaRecSaveSuccess(uint8_t source) {
  const esp_partition_t* part = esp_ota_get_next_update_partition(nullptr);
  if (!part) return;

  OtaSlotReport rep;
  otaSlotReport(part, rep);
  otaRecSave(0, 0, rep.span, rep.shaMatch, rep.hashAppended, false, source);
  LOG_I("Aktualizacja: obraz w %s zapisany i sprawdzony (%u B)", part->label, (unsigned)rep.span);
}

// ------------------------------------------------------------
//  ArduinoOTA - aktualizacja przez sieć
//  (espota, `pio run -e esp32s3-ota -t upload`)
// ------------------------------------------------------------
void OtaManager::begin() {
  String host = config.hostname();
  host.trim();
  host.toLowerCase();
  host.replace(" ", "-");
  if (host.length() == 0) host = "stacja-pogody";

  ArduinoOTA.setHostname(host.c_str());
  ArduinoOTA.setPort(OTA_PORT);
  ArduinoOTA.setTimeout(30000);

  String pass = config.otaPassword();
  if (pass.length()) ArduinoOTA.setPassword(pass.c_str());

  ArduinoOTA.onStart([]() {
    bool fs = (ArduinoOTA.getCommand() == U_SPIFFS);
    s_netFs = fs;
    ota.running(true);
    g_otaInProgress = true;
    // Przy aktualizacji systemu plików LittleFS musi być odmontowany -
    // Update pisze bezpośrednio w partycję, z pominięciem warstwy FS.
    if (fs) LittleFS.end();
    LOG_I("Aktualizacja przez sieć (%s) - start", fs ? "system plików" : "firmware");
  });

  ArduinoOTA.onEnd([]() {
    ota.running(false);
    g_otaInProgress = false;
    if (!Update.hasError() && !s_netFs) otaRecSaveSuccess(OTA_SRC_NET);
    LOG_I("Aktualizacja przez sieć zakończona - restart");
  });

  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    static unsigned int lastPct = 101;
    unsigned int pct = total ? (progress * 100U) / total : 0;
    if (pct != lastPct) {
      lastPct = pct;
      if (pct % 25U == 0) LOG_I("Aktualizacja przez sieć: %u%%", pct);
    }
  });

  ArduinoOTA.onError([](ota_error_t err) {
    ota.running(false);
    g_otaInProgress = false;
    const char* what = "nieznany";
    switch (err) {
      case OTA_AUTH_ERROR:    what = "błędne hasło"; break;
      case OTA_BEGIN_ERROR:   what = "brak miejsca / błąd startu"; break;
      case OTA_CONNECT_ERROR: what = "błąd połączenia"; break;
      case OTA_RECEIVE_ERROR: what = "przerwany odbiór"; break;
      case OTA_END_ERROR:     what = "błąd finalizacji"; break;
      default:                break;
    }
    LOG_E("Aktualizacja przez sieć nieudana (%u): %s", (unsigned)err, what);

    // ArduinoOTA kończy pracę przez Update.end(), które sprawdza dopisany skrót SHA-256.
    // Obraz jest już zapisany w całości, więc jeśli zawiodła tylko ta weryfikacja,
    // próbujemy aktywować go sami (patrz otaActivate).
    if (err == OTA_END_ERROR && !s_netFs) {
      String detail;
      if (otaActivate(esp_ota_get_next_update_partition(nullptr), OTA_SRC_NET, &detail)) {
        LOG_I("Aktualizacja przez sieć: obraz aktywowany - restart");
        scheduleReboot(1000);
      } else {
        LOG_E("Aktualizacja przez sieć: obraz nieaktywny (%s)", detail.c_str());
      }
    }
  });

  ArduinoOTA.begin();

  LOG_I("ArduinoOTA gotowe: %s.local:%u", host.c_str(), (unsigned)OTA_PORT);
  if (pass.length()) LOG_I("Hasło OTA ustawione");
  else               LOG_W("Brak hasła OTA - ustaw je w zakładce Aktualizacja");
}

void OtaManager::loop() {
  ArduinoOTA.handle();

  // Wgrywanie z www bez nowych danych = zerwane połączenie (serwer asynchroniczny
  // nie zgłasza tego zdarzeniem końcowym, patrz abortUpload).
  // Ciszę potwierdzamy drugim pomiarem po OTA_UP_IDLE_CONFIRM_MS - pojedynczy,
  // nieaktualny odczyt znacznika nie może przerywać poprawnego wgrania.
  if (s_upActive && (millis() - s_upLastMs) > OTA_UP_IDLE_MS) {
    if (s_upIdleSince == 0) {
      s_upIdleSince = millis();
    } else if ((millis() - s_upIdleSince) > OTA_UP_IDLE_CONFIRM_MS) {
      char why[96];
      snprintf(why, sizeof(why), "brak danych z przeglądarki przez %lu s",
               (unsigned long)(OTA_UP_IDLE_MS / 1000UL));
      LOG_W("Aktualizacja: cisza %lu ms (teraz %lu ms, ostatnie dane %lu ms)",
            (unsigned long)(millis() - s_upLastMs), (unsigned long)millis(),
            (unsigned long)s_upLastMs);
      s_upIdleSince = 0;
      abortUpload(why);
    }
  } else {
    s_upIdleSince = 0;
  }

  if (s_rebootPending && (long)(millis() - s_rebootAt) >= 0) {
    s_rebootPending = false;
    LOG_I("Restart po aktualizacji");
    ESP.restart();
  }
}

// ------------------------------------------------------------
//  Aktualizacja przez stronę www
// ------------------------------------------------------------
static String otaInfoJson() {
  PsramAllocator alloc;
  JsonDocument d(&alloc);

  const esp_partition_t* run = esp_ota_get_running_partition();
  const esp_partition_t* next = esp_ota_get_next_update_partition(nullptr);

  d["fw"] = FW_VERSION_FULL;
  d["hostname"] = config.hostname();
  d["ota_port"] = OTA_PORT;
  d["password_set"] = config.otaPassword().length() > 0;
  d["running_partition"] = run ? run->label : "?";
  d["next_partition"] = next ? next->label : "?";
  d["sketch_size"] = appImageSize();
  d["sketch_free"] = appImageFreeBytes();
  d["sketch_part"] = appImagePartitionSize();
  d["fs_total"] = (uint32_t)LittleFS.totalBytes();
  d["fs_used"] = (uint32_t)LittleFS.usedBytes();
  d["flash_read_fix"] = flashReadFixActive();
  d["flash_read_fix_chunk"] = flashReadFixChunk();
  d["upload_running"] = Update.isRunning();
  d["upload_active"] = s_upActive;
  if (Update.isRunning() && s_upActive) {
    d["upload_bytes"] = (uint32_t)s_upBytes;
    d["upload_idle_ms"] = (uint32_t)(millis() - s_upLastMs);
    d["upload_timeout_ms"] = (uint32_t)OTA_UP_IDLE_MS;
  }
  if (s_upAbort.length()) d["upload_aborted"] = s_upAbort;

  OtaActRecord rec;
  if (otaRecLoad(&rec)) {
    d["last_ota_err"] = rec.err;
    d["last_ota_err_name"] = esp_err_to_name((esp_err_t)rec.err);
    d["last_ota_patched"] = rec.patched != 0;
    d["last_ota_source"] = rec.source;
  }

  String s;
  serializeJson(d, s);
  return s;
}

// ------------------------------------------------------------
//  Diagnostyka OTA (/api/ota/diag)
// ------------------------------------------------------------
static void otaSlotJson(JsonObject o, const esp_partition_t* part, bool withDigests) {
  if (!part) {
    o["label"] = "?";
    return;
  }

  OtaSlotReport rep;
  otaSlotReport(part, rep);

  char hex[65];
  o["label"] = part->label;
  o["offset"] = part->address;
  o["size"] = part->size;
  o["header_ok"] = rep.headerOk;
  o["hash_appended"] = rep.hashAppended;
  o["image_span"] = rep.span;
  o["verify_ok"] = rep.verifyOk;
  o["verify_err"] = rep.verifyErr;
  o["verify_err_name"] = esp_err_to_name((esp_err_t)rep.verifyErr);
  o["verify_len"] = rep.verifyLen;
  o["sha_calculated"] = rep.shaCalculated;
  o["sha_match"] = rep.shaMatch;
  o["sha_raw_read_ok"] = rep.rawReadOk;
  o["sha_raw_match"] = rep.rawMatch;

  if (withDigests) {
    otaHex(rep.digestCalc, 32, hex);  o["sha_calc"] = hex;
    otaHex(rep.digestFlash, 32, hex); o["sha_flash"] = hex;
    otaHex(rep.digestRaw, 32, hex);   o["sha_flash_raw"] = hex;
    otaHex(rep.header, 24, hex);      o["header_hex"] = hex;
  }
}

static String otaDiagJson() {
  PsramAllocator alloc;
  JsonDocument d(&alloc);

  const esp_partition_t* boot = esp_ota_get_boot_partition();

  d["fw"] = FW_VERSION_FULL;
  d["flash_read_fix"] = flashReadFixActive();
  d["flash_read_fix_chunk"] = flashReadFixChunk();
  d["flash_read_fix_table"] = (uint32_t)(uintptr_t)flashReadFixTable();
  d["sha_self_test"] = shaSelfTest();
  d["boot_partition"] = boot ? boot->label : "?";

  // Surowy i poprawiony odczyt tego samego fragmentu - jeśli się różnią, obraz jest
  // kompletny, a winny jest odczyt flasha (dowód dla naprawy w flashfix.cpp).
  otaSlotJson(d["running"].to<JsonObject>(), esp_ota_get_running_partition(), true);
  otaSlotJson(d["next"].to<JsonObject>(), esp_ota_get_next_update_partition(nullptr), true);

  OtaActRecord rec;
  if (otaRecLoad(&rec)) {
    JsonObject l = d["last_activation"].to<JsonObject>();
    l["err"] = rec.err;
    l["err_name"] = esp_err_to_name((esp_err_t)rec.err);
    l["verify_err"] = rec.verifyErr;
    l["verify_err_name"] = esp_err_to_name((esp_err_t)rec.verifyErr);
    l["image_span"] = rec.span;
    l["sha_match"] = rec.shaMatch != 0;
    l["hash_appended"] = rec.hashAppended != 0;
    l["patched"] = rec.patched != 0;
    l["source"] = rec.source;
  }

  String s;
  serializeJson(d, s);
  return s;
}

static void sendJson(AsyncWebServerRequest* r, int code, const String& body) {
  AsyncWebServerResponse* resp = r->beginResponse(code, "application/json", body);
  resp->addHeader("Connection", "close");
  r->send(resp);
}

// ---- Obsługa binarek firmware na karcie SD (katalog /fw) ----------------
// Endpointy /api/ota/sd/* tylko przechowują i serwują pliki .bin na karcie
// SD. NIE flashują urządzenia - wgranie na płytkę nadal przez OTA (U_FLASH)
// albo esptool na PC. Dzięki temu wszystkie obrazy są pod ręką w jednym
// miejscu i można je pobrać / wgrać na inne moduły bez dostępu do PC.

// Sanitacja nazwy pliku: bez ścieżek, dozwolone litery/cyfry/kropki/spacje/
// myślniki/podkreślenia, wymagane rozszerzenie .bin i max 64 znaki.
static String otaSdName(const String& raw, bool* ok) {
  *ok = false;
  String n = raw;
  const int s = n.lastIndexOf('/');
  if (s >= 0) n = n.substring(s + 1);
  const int b = n.lastIndexOf('\\');
  if (b >= 0) n = n.substring(b + 1);
  n.trim();
  if (n.length() == 0 || n.length() > 64) return String();
  if (!n.endsWith(".bin")) return String();
  for (size_t i = 0; i < n.length(); i++) {
    char c = n[i];
    bool okc = isalnum((unsigned char)c) || c == '.' || c == '-' || c == '_' || c == ' ';
    if (!okc) return String();
  }
  *ok = true;
  return n;
}

// GET /api/ota/sd/list -> lista plików .bin w katalogu /fw.
static void otaSdList(AsyncWebServerRequest* r) {
  PsramAllocator alloc;
  JsonDocument d(&alloc);
  JsonArray arr = d["files"].to<JsonArray>();
  std::vector<SdFileInfo> files;
  sdCard.listDir(OTA_SD_DIR, files);
  for (const SdFileInfo& fi : files) {
    if (!fi.path.endsWith(".bin")) continue;
    const int slash = fi.path.lastIndexOf('/');
    String name = (slash >= 0) ? fi.path.substring(slash + 1) : fi.path;
    JsonObject o = arr.add<JsonObject>();
    o["name"] = name;
    o["path"] = fi.path;
    o["size"] = fi.size;
    o["mtime"] = (uint32_t)fi.mtime;
  }
  d["dir"] = OTA_SD_DIR;
  d["mounted"] = sdCard.mounted();
  String out;
  serializeJson(d, out);
  sendJson(r, 200, out);
}

// POST /api/ota/sd/upload -> zapisuje plik .bin na kartę SD (/fw/<nazwa>).
// Nagłówek: X-OTA-Key (hasło OTA, jak przy wgrywaniu firmware).
// Wzorzec jak przy /api/ota/firmware: ciało zbierane w handlerze uploadu,
// odpowiedź JSON wysyła dopiero handler wyniku (otaSdResult).
static String s_sdName;
static bool   s_sdOk     = false;
static bool   s_sdActive = false;
static size_t s_sdBytes  = 0;
static int    s_sdStatus = 200;
static String s_sdError;

static void otaSdUpload(AsyncWebServerRequest* r, const String& filename, size_t index, uint8_t* data, size_t len, bool final) {
  if (index == 0) {
    s_sdName   = "";
    s_sdOk     = false;
    s_sdActive = false;
    s_sdBytes  = 0;
    s_sdStatus = 200;
    s_sdError  = "";
    if (!authorized(r)) {
      s_sdStatus = 401;
      s_sdError  = "Brak lub błędne hasło OTA (nagłówek " + String(OTA_HEADER) + ")";
      return;
    }
    bool ok = false;
    String n = otaSdName(filename, &ok);
    if (!ok) {
      s_sdStatus = 400;
      s_sdError  = "Błędna nazwa pliku (wymagane .bin)";
      return;
    }
    if (!sdCard.mounted()) {
      s_sdStatus = 503;
      s_sdError  = "Karta SD niedostępna";
      return;
    }
    String path = String(OTA_SD_DIR) + "/" + n;
    sdCard.deleteFile(path);            // nadpisz, zamiast doklejać
    s_sdName   = n;
    s_sdActive = true;
  }
  if (s_sdStatus != 200) return;
  if (len) {
    String path = String(OTA_SD_DIR) + "/" + s_sdName;
    if (!sdCard.writeBlock(path, data, len)) {
      s_sdStatus = 500;
      s_sdError  = "Błąd zapisu na kartę SD";
      s_sdActive = false;
      return;
    }
    s_sdBytes += len;
  }
  if (final) {
    s_sdOk     = (s_sdStatus == 200);
    s_sdActive = false;
  }
}

static void otaSdResult(AsyncWebServerRequest* r) {
  PsramAllocator alloc;
  JsonDocument d(&alloc);
  d["ok"]    = s_sdOk;
  d["name"]  = s_sdName;
  d["bytes"] = (uint32_t)s_sdBytes;
  if (s_sdError.length()) d["error"] = s_sdError;
  String s;
  serializeJson(d, s);
  sendJson(r, s_sdStatus, s);
}

// Partycja, do której trafi wgrywany plik (firmware = druga partycja OTA,
// system plików = partycja LittleFS).
static const esp_partition_t* targetPartition(int command) {
  if (command == U_SPIFFS) {
    return esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                    ESP_PARTITION_SUBTYPE_DATA_SPIFFS, nullptr);
  }
  return esp_ota_get_next_update_partition(nullptr);
}

static void startUpload(AsyncWebServerRequest* r, int command, const String& filename, size_t size) {
  // Resztka po przerwanej próbie: bez tego każde kolejne wgranie kończy się
  // odpowiedzią 409 "Inna aktualizacja już trwa" aż do restartu stacji.
  if (Update.isRunning() && !s_upActive) abortUpload("zwieszony zapis z poprzedniej próby");

  s_upOk = false;
  s_upBytes = 0;
  s_upError = "";
  s_upAbort = "";   // bez tego diagnostyka pokazuje powód z poprzedniej, udanej/przerwanej próby
  s_upIdleSince = 0;
  s_upStatus = 200;
  s_upCommand = command;

  if (!authorized(r)) {
    s_upStatus = 401;
    s_upError = "Brak lub błędne hasło OTA (nagłówek " + String(OTA_HEADER) + ")";
    return;
  }
  if (Update.isRunning()) {
    s_upStatus = 409;
    s_upError = "Inna aktualizacja już trwa";
    return;
  }

  // Bez znajomości rozmiaru IDF kasuje całą partycję docelową na starcie, co
  // blokuje stos sieciowy na kilkadziesiąt sekund. Rozmiar treści z nagłówka
  // Content-Length wystarcza, żeby skasować tylko potrzebny zakres.
  // Uwaga: treść formularza bywa większa od partycji (nagłówki multipart), a
  // UpdateClass::begin() odrzuca rozmiar większy od partycji - przycinamy.
  if (size == 0) size = UPDATE_SIZE_UNKNOWN;
  if (size != UPDATE_SIZE_UNKNOWN) {
    const esp_partition_t* part = targetPartition(command);
    if (part && size > part->size) {
      LOG_W("Aktualizacja: treść %u B > partycja %s (%u B) - przycięto do rozmiaru partycji",
            (unsigned)size, part->label, (unsigned)part->size);
      size = part->size;
    }
  }

  g_otaInProgress = true;

  if (command == U_SPIFFS) LittleFS.end();

  const uint32_t t0 = millis();
  if (!Update.begin(size, command)) {
    s_upStatus = 500;
    s_upError = "Nie można rozpocząć: " + String(Update.errorString());
    LOG_E("Aktualizacja: nie można rozpocząć zapisu - %s", Update.errorString());
    if (command == U_SPIFFS) remountLittleFs("Aktualizacja");
    g_otaInProgress = false;
    return;
  }

  s_upActive = true;
  s_upLastMs = millis();

  LOG_I("Aktualizacja przez www (%s): %s, %u B miejsca, przygotowanie %lu ms",
        command == U_FLASH ? "firmware" : "system plików",
        filename.c_str(), (unsigned)Update.size(), (unsigned long)(millis() - t0));
}

static void writeChunk(uint8_t* data, size_t len) {
  if (s_upError.length() || !Update.isRunning()) return;
  if (Update.write(data, len) != len) {
    const String why = "błąd zapisu: " + String(Update.errorString());
    abortUpload(why.c_str());
    return;
  }
  const size_t before = s_upBytes;
  const unsigned long gap = millis() - s_upLastMs;
  s_upBytes += len;
  s_upLastMs = millis();
  if (gap > 2000) {
    LOG_W("Aktualizacja: przerwa %lu ms przed %u B", gap, (unsigned)s_upBytes);
  }
  if (before / 131072 != s_upBytes / 131072) {
    LOG_I("Aktualizacja: %u B odebrane", (unsigned)s_upBytes);
  }
}

static void finishUpload() {
  s_upActive = false;
  LOG_I("Aktualizacja: koniec strumienia (%u B, status %d)", (unsigned)s_upBytes, s_upStatus);

  if (s_upError.length()) {
    Update.abort();
    g_otaInProgress = false;
    return;
  }

  bool ok = Update.end(true);
  if (!ok && s_upCommand == U_FLASH) {
    // Plik zapisany w całości - jeśli zawiodła tylko weryfikacja dopisanego skrótu,
    // aktywujemy obraz sami (patrz otaActivate).
    LOG_W("Aktualizacja: aktywacja nieudana (%s)", Update.errorString());
    String detail;
    if (otaActivate(esp_ota_get_next_update_partition(nullptr), OTA_SRC_UPLOAD, &detail)) {
      ok = true;
      LOG_I("Aktualizacja: obraz aktywowany");
    } else {
      s_upError = "Nie udało się aktywować obrazu: " + detail;
    }
  } else if (ok && s_upCommand == U_FLASH) {
    otaRecSaveSuccess(OTA_SRC_UPLOAD);
  }

  if (ok) {
    s_upOk = true;
    LOG_I("Aktualizacja zapisana (%u B) - restart", (unsigned)s_upBytes);
  } else {
    s_upStatus = 500;
    if (s_upError.length() == 0) s_upError = "Błąd zakończenia: " + String(Update.errorString());
    LOG_E("Aktualizacja: błąd finalizacji - %s", s_upError.c_str());
  }
  g_otaInProgress = false;
}

static ArUploadHandlerFunction makeUploadHandler(int command) {
  return [command](AsyncWebServerRequest* r, String filename, size_t index,
                   uint8_t* data, size_t len, bool final) {
    if (index == 0) {
      // Rozłączenie klienta w trakcie wysyłania: natychmiast zwalniamy zapis,
      // inaczej stacja zostaje zablokowana (szczegóły w opisie abortUpload).
      r->onDisconnect([]() {
        if (s_upActive) abortUpload("przerwane połączenie z przeglądarką");
      });
      startUpload(r, command, filename, r->contentLength());
    }
    if (s_upStatus != 200) return;
    if (len) writeChunk(data, len);
    if (final) finishUpload();
  };
}

static ArRequestHandlerFunction makeResultHandler() {
  return [](AsyncWebServerRequest* r) {
    JsonDocument d;
    d["ok"] = s_upOk;
    d["bytes"] = (uint32_t)s_upBytes;
    if (s_upError.length()) d["error"] = s_upError;
    String s;
    serializeJson(d, s);

    sendJson(r, s_upStatus, s);
    // Restart dopiero po wysłaniu odpowiedzi - inaczej przeglądarka
    // zgłosi zerwane połączenie mimo udanej aktualizacji.
    if (s_upOk) scheduleReboot(1500);
  };
}

void registerOtaRoutes(AsyncWebServer& server) {
  server.on("/api/ota/info", HTTP_GET, [](AsyncWebServerRequest* r) {
    r->send(200, "application/json", otaInfoJson());
  });

  // Pełna diagnostyka obrazu: długość, skrót policzony, skrót z flasha i ten sam
  // odczyt oryginalnym sterownikiem. Bez hasła - to tylko odczyty.
  server.on("/api/ota/diag", HTTP_GET, [](AsyncWebServerRequest* r) {
    r->send(200, "application/json", otaDiagJson());
  });

  // Ręczna aktywacja obrazu z partycji zapasowej (np. po sprawdzeniu /api/ota/diag).
  // Nie restartuje stacji - mówi tylko, czy obraz da się ustawić jako startowy.
  server.on("/api/ota/activate", HTTP_POST, [](AsyncWebServerRequest* r) {
    JsonDocument d;
    if (!authorized(r)) {
      d["ok"] = false;
      d["error"] = "Brak lub błędne hasło OTA (nagłówek " + String(OTA_HEADER) + ")";
    } else if (Update.isRunning()) {
      d["ok"] = false;
      d["error"] = "Trwa wgrywanie - poczekaj";
    } else {
      const esp_partition_t* next = esp_ota_get_next_update_partition(nullptr);
      String detail;
      bool ok = otaActivate(next, OTA_SRC_API, &detail);
      d["ok"] = ok;
      d["partition"] = next ? next->label : "?";
      if (ok) d["reboot_required"] = true;
      else    d["error"] = detail;
    }
    String s;
    serializeJson(d, s);
    sendJson(r, 200, s);
  });

  // Przechowywanie binarek firmware na karcie SD (/fw). Tylko pliki .bin,
  // bez flashowania - wgranie na płytkę nadal przez /api/ota/firmware.
  server.on("/api/ota/sd/list", HTTP_GET, otaSdList);
  server.on("/api/ota/sd/upload", HTTP_POST, otaSdResult, otaSdUpload);
  // Usuwanie binarek z karty SD: ponownie używamy /api/sd/delete ({"path"}),
  // tak samo jak reszta plików na karcie.

  server.on("/api/ota/firmware", HTTP_POST, makeResultHandler(), makeUploadHandler(U_FLASH));
  server.on("/api/ota/filesystem", HTTP_POST, makeResultHandler(), makeUploadHandler(U_SPIFFS));
}
