/* =============================================================================
 * Stacja Pogody - stacja meteorologiczna na ESP32-S3 WROOM-1 (N16R8)
 * Copyright (c) 2026 kruzio1985 - https://github.com/kruzio1985
 * Licencja: PolyForm Noncommercial License 1.0.0 (plik LICENSE)
 * Użytek niekomercyjny. Kontakt: kruzio1985@users.noreply.github.com
 * =============================================================================
 */
#include "nvs_store.h"

#include <Preferences.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <esp_partition.h>

#include "syslog.h"

namespace {
const size_t CHUNK    = 3500;  // mniej niż limit wpisu w NVS (~4000 B)
const int    MAX_CHUNK = 16;   // zabezpieczenie pętli (do ~56 kB)

void chunkKey(char* out, size_t n, const char* key, int idx) {
  snprintf(out, n, "%s_%d", key, idx);
}
}  // namespace

bool nvsStoreWrite(const char* ns, const char* key, const String& data) {
  Preferences p;
  if (!p.begin(ns, false)) {
    LOG_E("NVS: nie mogę otworzyć przestrzeni '%s'", ns);
    return false;
  }

  char kCnt[16], kOk[16];
  snprintf(kCnt, sizeof(kCnt), "%s_c", key);
  snprintf(kOk, sizeof(kOk), "%s_ok", key);

  // Znacznik poprawności kasujemy przed zapisem - przerwanie (reset, zanik
  // zasilania) w trakcie zapisu nie zostawi częściowo nadpisanych ustawień.
  p.putUInt(kOk, 0);

  int chunks = (int)((data.length() + CHUNK - 1) / CHUNK);
  if (chunks < 1) chunks = 1;

  bool ok = true;
  for (int i = 0; i < chunks; i++) {
    char k[16];
    chunkKey(k, sizeof(k), key, i);
    String part = data.substring(i * CHUNK, i * CHUNK + CHUNK);
    if (p.putString(k, part) != part.length()) ok = false;
  }

  if (ok) {
    if (p.putInt(kCnt, chunks) != sizeof(int32_t)) ok = false;
    if (ok && p.putUInt(kOk, 1) != sizeof(uint32_t)) ok = false;
  }
  if (!ok) {
    // Zostawiamy niepoprawną wersję (znacznik 0) - odczyt wróci do kopii.
    p.putUInt(kOk, 0);
    LOG_E("NVS: zapis '%s' nieudany (brak miejsca? %u B)", key, (unsigned)data.length());
  }
  p.end();
  return ok;
}

String nvsStoreRead(const char* ns, const char* key) {
  Preferences p;
  if (!p.begin(ns, true)) return String();

  char kCnt[16], kOk[16];
  snprintf(kCnt, sizeof(kCnt), "%s_c", key);
  snprintf(kOk, sizeof(kOk), "%s_ok", key);

  if (p.getUInt(kOk, 0) != 1) { p.end(); return String(); }

  int chunks = p.getInt(kCnt, 0);
  if (chunks < 1 || chunks > MAX_CHUNK) { p.end(); return String(); }

  String out;
  out.reserve((size_t)chunks * CHUNK);
  for (int i = 0; i < chunks; i++) {
    char k[16];
    chunkKey(k, sizeof(k), key, i);
    String part = p.getString(k, "");
    // Brak środkowego kawałka oznacza uszkodzone dane - nie zwracamy resztek.
    if (part.length() == 0 && i < chunks - 1) { p.end(); return String(); }
    out += part;
  }
  p.end();
  return out;
}

size_t nvsStoreSize(const char* ns, const char* key) {
  return nvsStoreRead(ns, key).length();
}

void nvsStoreErase(const char* ns, const char* key) {
  Preferences p;
  if (!p.begin(ns, false)) return;

  char kCnt[16], kOk[16];
  snprintf(kCnt, sizeof(kCnt), "%s_c", key);
  snprintf(kOk, sizeof(kOk), "%s_ok", key);

  for (int i = 0; i < MAX_CHUNK; i++) {
    char k[16];
    chunkKey(k, sizeof(k), key, i);
    if (p.isKey(k)) p.remove(k);
  }
  p.remove(kCnt);
  p.remove(kOk);
  p.end();
}

NvsPartitionInfo nvsPartitionInfo() {
  NvsPartitionInfo info;

  const esp_partition_t* part = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, nullptr);
  if (part) {
    info.sizeBytes = part->size;
    info.ok = true;
  }

  nvs_stats_t st;
  if (nvs_get_stats(nullptr, &st) == ESP_OK) {
    info.ok = true;
    info.totalEntries = st.total_entries;
    info.usedEntries  = st.used_entries;
    info.freeEntries  = st.free_entries;
    info.namespaces   = st.namespace_count;
  }
  return info;
}
