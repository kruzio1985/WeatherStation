/* =============================================================================
 * Stacja Pogody - dekoder ramek BLE czujników temperatury / wilgotności
 * Copyright (c) 2026 kruzio1985 - https://github.com/kruzio1985
 * Licencja: PolyForm Noncommercial License 1.0.0 (plik LICENSE)
 * Użytek niekomercyjny. Kontakt: kruzio1985@users.noreply.github.com
 * =============================================================================
 * Wspólny, czysty dekoder (bez typów Arduino) używany przez:
 *   - src/drv_ble.cpp          (węzeł RS485 ESP32-C3)
 *   - blegw/src/main.cpp       (samodzielna bramka BLE przez Wi-Fi)
 *
 * Obsługiwane formaty:
 *   - Xiaomi MiBeacon (LYWSD03MMC i podobne): szyfrowane (AES-CCM, bind key)
 *     i nieszyfrowane
 *   - ATC / pvvx (custom firmware na LYWSD03MMC): 4 warianty
 *   - Govee H5074/H5051, H5072/H5075, H5101/H5102, H5179
 *   - BTHome v2 (nieszyfrowane)
 *
 * Każda funkcja dostaje surową sekcję reklamy (manufacturer data lub service
 * data) i wypełnia BleSample. mac/rssi uzupełnia wołający.
 * ========================================================================== */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <math.h>

// mbedtls (AES-CCM) jest dostępny na ESP32/ESP32-C3 (Arduino / ESP-IDF).
// Na hoście (testy) definiujemy BLE_NO_MBEDTLS, żeby skompilować bez niego.
#if !defined(BLE_NO_MBEDTLS) && (defined(ARDUINO) || defined(ESP_PLATFORM) || defined(IDF_VER))
  #include "mbedtls/ccm.h"
  #define BLE_HAVE_MBEDTLS 1
#else
  #define BLE_HAVE_MBEDTLS 0
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Wynik dekodowania jednej ramki
typedef struct {
  uint8_t  mac[6];        // adres czujnika (z reklamy)
  bool     valid;         // true = udało się zdekodować T/H
  float    temp;          // °C (NAN = brak)
  float    hum;           // %  (NAN = brak)
  int      batt;          // %  (-1 = brak)
  int8_t   rssi;          // dBm
  char     type[24];      // nazwa formatu/urządzenia
} BleSample;

void bleSampleInit(BleSample* s);

// AES-128-CCM (auth_decrypt), tag 4 bajty. Zwraca false przy błędzie.
bool bleAesCcmDecrypt(const uint8_t* key, const uint8_t* nonce, size_t nonceLen,
                      const uint8_t* aad, size_t aadLen,
                      const uint8_t* cipher, size_t cipherLen,
                      const uint8_t* tag, size_t tagLen,
                      uint8_t* out);

// --- Xiaomi MiBeacon -----------------------------------------------
// sd = service data (za 2-bajtowym UUID 0xFE95), key = bind key 16 B (NULL
// dla ramek nieszyfrowanych).
bool bleDecodeXiaomi(const uint8_t* sd, uint16_t len, const uint8_t* mac,
                     const uint8_t key[16], BleSample* out);

// --- ATC / pvvx ----------------------------------------------------
// md = manufacturer data (razem z 2-bajtowym company ID 0x181A).
bool bleDecodeAtc(const uint8_t* md, uint16_t len, const uint8_t* mac,
                  const uint8_t key[16], BleSample* out);

// --- Govee ---------------------------------------------------------
// md = manufacturer data (razem z 2-bajtowym company ID).
bool bleDecodeGovee(const uint8_t* md, uint16_t len, BleSample* out);

// --- BTHome v2 (nieszyfrowane) -------------------------------------
// sd = service data BTHome (za UUID 0xFCD2 albo typ 0xD2).
bool bleDecodeBthome(const uint8_t* sd, uint16_t len, BleSample* out);

#ifdef __cplusplus
}
#endif

/* ============================ implementacja ============================ */

static inline int16_t bleBe16(const uint8_t* b) {
  return (int16_t)(((uint16_t)b[0] << 8) | b[1]);
}
static inline int16_t bleLe16(const uint8_t* b) {
  return (int16_t)(((uint16_t)b[1] << 8) | b[0]);
}
static inline uint16_t bleLeU16(const uint8_t* b) {
  return (uint16_t)(((uint16_t)b[1] << 8) | b[0]);
}

static inline void bleSetType(BleSample* s, const char* t) {
  if (!s) return;
  strncpy(s->type, t, sizeof(s->type) - 1);
  s->type[sizeof(s->type) - 1] = 0;
}

#ifdef __cplusplus
void bleSampleInit(BleSample* s) {
#else
void bleSampleInit(BleSample* s) {
#endif
  if (!s) return;
  memset(s, 0, sizeof(*s));
  s->temp = NAN; s->hum = NAN; s->batt = -1; s->rssi = 0; s->valid = false;
}

bool bleAesCcmDecrypt(const uint8_t* key, const uint8_t* nonce, size_t nonceLen,
                      const uint8_t* aad, size_t aadLen,
                      const uint8_t* cipher, size_t cipherLen,
                      const uint8_t* tag, size_t tagLen,
                      uint8_t* out) {
#if BLE_HAVE_MBEDTLS
  if (!key || !nonce || !cipher || !out) return false;
  mbedtls_ccm_context ctx;
  mbedtls_ccm_init(&ctx);
  int rc = mbedtls_ccm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key, 128);
  if (rc == 0) {
    rc = mbedtls_ccm_auth_decrypt(&ctx, cipherLen, nonce, nonceLen,
                                  aad, aadLen, cipher, out, tag, tagLen);
  }
  mbedtls_ccm_free(&ctx);
  return rc == 0;
#else
  (void)key; (void)nonce; (void)nonceLen; (void)aad; (void)aadLen;
  (void)cipher; (void)cipherLen; (void)tag; (void)tagLen; (void)out;
  return false;
#endif
}

bool bleDecodeXiaomi(const uint8_t* sd, uint16_t len, const uint8_t* mac,
                     const uint8_t key[16], BleSample* out) {
  if (!sd || !out || len < 9) return false;
  const uint16_t frctrl = (uint16_t)(sd[0] | (sd[1] << 8));
  const uint8_t  version = (uint8_t)(frctrl >> 12);
  const bool encrypted  = ((frctrl >> 3) & 1) != 0;
  const bool macIncl    = ((frctrl >> 4) & 1) != 0;
  const bool capIncl    = ((frctrl >> 5) & 1) != 0;
  if (version < 2) return false;

  // ID urządzenia (0x055B = LYWSD03MMC, 0x0347 = CGG1, 0x0387 = MHO-C401,
  // 0x0576 = CGD1, 0x16e4 = LYWSD02MMC) - akceptujemy każdy i parsujemy obiekty.
  // const uint16_t devId = (uint16_t)(sd[2] | (sd[3] << 8));

  uint16_t off = 5;                       // za frame ctrl + product id + counter
  if (macIncl) { off += 6; if (len < off) return false; }
  if (capIncl) { off += 1; if (len < off) return false; }

  const uint8_t* payload;
  uint16_t plen;
  uint8_t decBuf[64];

  if (encrypted) {
    if (!key || len < off + 7) return false;
    // nonce = mac[::-1] + productId(2 LE) + counter(1) + seq(3, jawnie przed MAC)
    uint8_t nonce[12];
    for (int i = 0; i < 6; i++) nonce[i] = mac[5 - i];
    nonce[6] = sd[2]; nonce[7] = sd[3]; nonce[8] = sd[4];
    nonce[9] = sd[len - 7]; nonce[10] = sd[len - 6]; nonce[11] = sd[len - 5];
    const uint8_t aad[1] = { 0x11 };
    const uint16_t cLen = (uint16_t)(len - off - 7);
    if (cLen > sizeof(decBuf)) return false;
    if (!bleAesCcmDecrypt(key, nonce, sizeof(nonce), aad, 1,
                          sd + off, cLen, sd + len - 4, 4, decBuf)) return false;
    payload = decBuf;
    plen = cLen;
  } else {
    payload = sd + off;
    plen = (uint16_t)(len - off);
  }

  float temp = NAN, hum = NAN; int batt = -1; bool got = false;
  uint16_t o = 0;
  while (o + 3 <= plen) {
    const uint16_t code = (uint16_t)(payload[o] | (payload[o + 1] << 8));
    const uint8_t  olen = payload[o + 2];
    if (o + 3 + olen > plen) break;
    const uint8_t* d = payload + o + 3;
    if (code == 0x1004 && olen == 2) { temp = bleLe16(d) / 10.0f; got = true; }
    else if (code == 0x1006 && olen == 2) { hum = bleLeU16(d) / 10.0f; got = true; }
    else if (code == 0x100D && olen == 4) { temp = bleLe16(d) / 10.0f; hum = bleLeU16(d + 2) / 10.0f; got = true; }
    else if (code == 0x100A && olen == 1) { batt = d[0]; }
    o += 3 + olen;
  }
  if (!got) return false;
  out->temp = temp; out->hum = hum; out->batt = batt; out->valid = true;
  bleSetType(out, encrypted ? "Xiaomi (MiBeacon)" : "Xiaomi");
  return true;
}

bool bleDecodeAtc(const uint8_t* md, uint16_t len, const uint8_t* mac,
                  const uint8_t key[16], BleSample* out) {
  if (!md || !out || len < 2) return false;
  if (md[0] != 0x1A || md[1] != 0x18) return false;   // company ID 0x181A (ATC)

  if (len == 19) {                                    // custom, bez szyfrowania
    out->temp = bleLe16(md + 10) / 100.0f;
    out->hum  = bleLeU16(md + 12) / 100.0f;
    out->batt = md[16];
    out->valid = true;
    bleSetType(out, "ATC (custom)");
    return true;
  }
  if (len == 17) {                                    // ATC1441, bez szyfrowania
    out->temp = bleBe16(md + 10) / 10.0f;
    out->hum  = md[12];
    out->batt = md[13];
    out->valid = true;
    bleSetType(out, "ATC (atc1441)");
    return true;
  }
  if (key && (len == 15 || len == 12)) {              // szyfrowane
    uint8_t nonce[13];
    for (int i = 0; i < 6; i++) nonce[i] = mac[5 - i];
    memcpy(nonce + 6, md, 5);                          // 5 pierwszych bajtów mfg
    const uint8_t aad[1] = { 0x11 };
    const uint16_t cLen = (uint16_t)(len - 5 - 4);
    uint8_t dec[16];
    if (!bleAesCcmDecrypt(key, nonce, sizeof(nonce), aad, 1,
                          md + 5, cLen, md + len - 4, 4, dec)) return false;
    if (len == 15) {                                   // custom encrypted
      out->temp = bleLe16(dec) / 100.0f;
      out->hum  = bleLeU16(dec + 2) / 100.0f;
      out->batt = dec[4];
    } else {                                           // atc1441 encrypted
      out->temp = dec[0] / 2.0f - 40.0f;
      out->hum  = dec[1] / 2.0f;
      out->batt = dec[2] & 0x7F;
    }
    if (out->batt > 100) out->batt = 100;
    out->valid = true;
    bleSetType(out, "ATC");
    return true;
  }
  return false;
}

static inline float bleGoveeTemp24(uint32_t packet) {
  if (packet & 0x800000UL) return (float)(packet ^ 0x800000UL) / -10000.0f;
  return (float)packet / 10000.0f;
}

bool bleDecodeGovee(const uint8_t* md, uint16_t len, BleSample* out) {
  if (!md || !out || len < 3) return false;
  const uint16_t cid = (uint16_t)(md[0] | (md[1] << 8));

  if (cid == 0xEC88) {
    if (len == 9) {                                    // H5074 / H5051
      out->temp = bleLe16(md + 3) / 100.0f;
      out->hum  = bleLeU16(md + 5) / 100.0f;
      out->batt = md[7];
      out->valid = true;
      bleSetType(out, "Govee H5074/H5051");
      return true;
    }
    if (len == 8) {                                    // H5072 / H5075 (24-bit)
      uint32_t packet = (uint32_t)md[3] | ((uint32_t)md[4] << 8) | ((uint32_t)md[5] << 16);
      out->temp = bleGoveeTemp24(packet);
      out->hum  = (packet % 1000) / 10.0f;
      out->batt = md[6];
      out->valid = true;
      bleSetType(out, "Govee H5072/H5075");
      return true;
    }
    if (len == 11) {                                   // H5179
      out->temp = bleLe16(md + 6) / 100.0f;
      out->hum  = bleLeU16(md + 8) / 100.0f;
      out->batt = md[10];
      out->valid = true;
      bleSetType(out, "Govee H5179");
      return true;
    }
  } else if (cid == 0x0100 && len == 8) {              // H5101 / H5102
    uint32_t packet = (uint32_t)md[4] | ((uint32_t)md[5] << 8) | ((uint32_t)md[6] << 16);
    out->temp = bleGoveeTemp24(packet);
    out->hum  = (packet % 1000) / 10.0f;
    out->batt = md[7];
    out->valid = true;
    bleSetType(out, "Govee H5101/H5102");
    return true;
  }
  return false;
}

bool bleDecodeBthome(const uint8_t* sd, uint16_t len, BleSample* out) {
  if (!sd || !out || len < 2) return false;
  const bool enc = (sd[0] & 1) != 0;
  const bool macIncl = (sd[0] & 2) != 0;
  if (enc) return false;                               // szyfrowane pomijamy
  uint16_t o = 1;
  if (macIncl) o += 6;
  if (o >= len) return false;

  float temp = NAN, hum = NAN; int batt = -1; bool got = false;
  while (o + 1 < len) {
    const uint8_t id = sd[o];
    if (id == 0x01) { if (o + 2 <= len) { batt = sd[o + 1]; o += 2; } else break; }
    else if (id == 0x02) { if (o + 3 <= len) { temp = bleLe16(sd + o + 1) / 100.0f; o += 3; got = true; } else break; }
    else if (id == 0x03) { if (o + 3 <= len) { hum = bleLeU16(sd + o + 1) / 100.0f; o += 3; got = true; } else break; }
    else break;                                        // nieznany obiekt - stop
  }
  if (!got) return false;
  out->temp = temp; out->hum = hum; out->batt = batt; out->valid = true;
  bleSetType(out, "BTHome");
  return true;
}

// --- Inkbird ---------------------------------------------------------
// Dispatch po nazwie lokalnej: "sps" = IBS-TH (T+H), "tps" = IBS-TH2/P01B (sama T).
bool bleDecodeInkbird(const char* name, const uint8_t* md, uint16_t len, BleSample* out) {
  if (!name || !md || !out || len != 11) return false;
  const bool sps = strstr(name, "sps") != nullptr;
  const bool tps = strstr(name, "tps") != nullptr;
  if (!sps && !tps) return false;
  out->temp = bleLe16(md + 2) / 100.0f;
  if (sps) out->hum = bleLeU16(md + 4) / 100.0f;
  out->batt = md[8];
  out->valid = true;
  bleSetType(out, sps ? "Inkbird IBS-TH" : "Inkbird IBS-TH2/P01B");
  return true;
}

// --- ThermoPro -------------------------------------------------------
// Dispatch po nazwie lokalnej "TP357" / "TP359".
bool bleDecodeThermopro(const char* name, const uint8_t* d, uint16_t len, BleSample* out) {
  if (!name || !d || !out || len < 6) return false;
  if (!strstr(name, "TP357") && !strstr(name, "TP359")) return false;
  out->temp = bleLe16(d + 3) / 10.0f;
  out->hum  = d[5];
  out->valid = true;
  bleSetType(out, "ThermoPro TP357/TP359");
  return true;
}

// --- Moat (M1 / S2) --------------------------------------------------
bool bleDecodeMoat(const uint8_t* md, uint16_t len, BleSample* out) {
  if (!md || !out || len != 22) return false;
  const uint16_t devId = (uint16_t)(md[2] | (md[3] << 8));
  if (devId != 0x1000) return false;
  const uint16_t tr = bleLeU16(md + 14);
  const uint16_t hr = bleLeU16(md + 16);
  const uint16_t volt = bleLeU16(md + 18);
  out->temp = -46.85f + 175.72f * tr / 65536.0f;
  out->hum  = -6.0f + 125.0f * hr / 65536.0f;
  int batt;
  if (volt >= 3000) batt = 100;
  else if (volt >= 2900) batt = (int)(42 + (volt - 2900) * 0.58f);
  else if (volt >= 2740) batt = (int)(18 + (volt - 2740) * 0.15f);
  else if (volt >= 2440) batt = (int)(6 + (volt - 2440) * 0.04f);
  else if (volt >= 2100) batt = (int)((volt - 2100) * (6.0f / 340.0f));
  else batt = 0;
  out->batt = batt;
  out->valid = true;
  bleSetType(out, "Moat S2");
  return true;
}

// --- Jaalee JHT ------------------------------------------------------
bool bleDecodeJaalee(const uint8_t* md, uint16_t len, BleSample* out) {
  if (!md || !out || len != 15) return false;
  out->batt = md[4];
  const uint16_t tr = (uint16_t)((md[11] << 8) | md[12]);   // BE
  const uint16_t hr = (uint16_t)((md[13] << 8) | md[14]);   // BE
  out->temp = 0.0026821682f * tr - 46.873f;
  out->hum  = 0.0019213762f * hr - 6.332f;
  out->valid = true;
  bleSetType(out, "Jaalee JHT");
  return true;
}
