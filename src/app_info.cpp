/* =============================================================================
 * Stacja Pogody - stacja meteorologiczna na ESP32-S3 WROOM-1 (N16R8)
 * Copyright (c) 2026 kruzio1985 - https://github.com/kruzio1985
 * Licencja: PolyForm Noncommercial License 1.0.0 (plik LICENSE)
 * Użytek niekomercyjny. Kontakt: kruzio1985@users.noreply.github.com
 * =============================================================================
 */
// Rozmiar obrazu aplikacji bez weryfikacji sumy kontrolnej.
//
// ESP.getSketchSize() woła esp_image_verify(), które liczy SHA-256 całego obrazu
// (~1,3 MB) i - gdy obraz nie przejdzie weryfikacji - wypisuje w logu
// "Image hash failed - image is corrupt". Robi to przy KAŻDYM wywołaniu i nie da
// się tego wyciszyć (w IDF 4.4 komunikat nie ma parametru "silent").
//
// Tutaj liczymy długość obrazu sami, dokładnie tak samo jak robi to IDF w
// esp_image_format.c: nagłówek + nagłówki segmentów + dopełnienie do 16 B
// (bajt sumy kontrolnej) + dopisany skrót SHA-256. Czytamy przy tym ok. 150 B.
// Wynik liczymy raz i trzymamy w pamięci.

#include "app_info.h"
#include "syslog.h"

#include <string.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_image_format.h>

static uint32_t s_image  = 0;
static uint32_t s_part   = 0;
static bool     s_valid  = false;
static bool     s_failed = false;

void appImageInfoRefresh() {
  s_valid  = false;
  s_failed = false;

  const esp_partition_t* run = esp_ota_get_running_partition();
  if (!run) {
    s_failed = true;
    LOG_W("Rozmiar programu: nie znaleziono partycji uruchomieniowej");
    return;
  }
  s_part = run->size;

  esp_image_header_t hdr;
  if (esp_partition_read(run, 0, &hdr, sizeof(hdr)) != ESP_OK ||
      hdr.magic != ESP_IMAGE_HEADER_MAGIC ||
      hdr.segment_count == 0 || hdr.segment_count > ESP_IMAGE_MAX_SEGMENTS) {
    s_failed = true;
    LOG_W("Rozmiar programu: nie udało się odczytać nagłówka obrazu (magic 0x%02X, segmentów %u)",
          (unsigned)hdr.magic, (unsigned)hdr.segment_count);
    return;
  }

  uint32_t len = sizeof(esp_image_header_t);
  for (uint8_t i = 0; i < hdr.segment_count; i++) {
    esp_image_segment_header_t sh;
    if (esp_partition_read(run, len, &sh, sizeof(sh)) != ESP_OK) {
      s_failed = true;
      LOG_W("Rozmiar programu: błąd czytania nagłówka segmentu %u", (unsigned)i);
      return;
    }
    len += sizeof(sh) + sh.data_len;
    if (len > run->size) {
      s_failed = true;
      LOG_W("Rozmiar programu: obraz dłuższy niż partycja (%u B)", (unsigned)len);
      return;
    }
  }

  // Bajt sumy kontrolnej + dopełnienie do 16 B, potem dopisany skrót SHA-256
  len = (len + 1 + 15) & ~15u;
  if (hdr.hash_appended) len += ESP_IMAGE_HASH_LEN;
  if (len > run->size) {
    s_failed = true;
    LOG_W("Rozmiar programu: obraz dłuższy niż partycja (%u B)", (unsigned)len);
    return;
  }

  s_image = len;
  s_valid = true;
  LOG_I("Obraz programu: %u B z %u B partycji %s", (unsigned)s_image,
        (unsigned)s_part, run->label);
}

static void ensureInfo() {
  if (s_valid || s_failed) return;
  appImageInfoRefresh();
}

uint32_t appImageSize() {
  ensureInfo();
  return s_image;
}

uint32_t appImagePartitionSize() {
  ensureInfo();
  return s_part;
}

uint32_t appImageFreeBytes() {
  ensureInfo();
  if (!s_valid || s_part <= s_image) return 0;
  return s_part - s_image;
}
