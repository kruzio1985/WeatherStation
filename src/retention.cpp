/* =============================================================================
 * Stacja Pogody - stacja meteorologiczna na ESP32-S3 WROOM-1 (N16R8)
 * Copyright (c) 2026 kruzio1985 - https://github.com/kruzio1985
 * Licencja: PolyForm Noncommercial License 1.0.0 (plik LICENSE)
 * Użytek niekomercyjny. Kontakt: kruzio1985@users.noreply.github.com
 * =============================================================================
 */
#include "retention.h"
#include "config.h"
#include "sd_card.h"
#include "syslog.h"
#include <time.h>
#include <vector>

SdRetention retention;

static bool olderThan(time_t mtime, uint16_t days) {
  if (days == 0 || mtime <= 0) return false;
  time_t now = time(nullptr);
  if (now < 1000000000) return false;   // brak synchronizacji czasu - nie kasujemy
  return (now - mtime) > (time_t)days * 86400;
}

// Kasuje zdjęcie oraz plik .json z analizą zapisany obok niego.
static void deletePhotoWithMeta(const String& jpgPath, int& deleted) {
  if (!sdCard.deleteFile(jpgPath)) return;
  deleted++;
  String meta = jpgPath;
  meta.replace(".jpg", ".json");
  meta.replace(".jpeg", ".json");
  sdCard.deleteFile(meta);
}

void SdRetention::runOnce() {
  if (!sdCard.mounted()) return;

  const uint16_t logDays = config.sdRetentionDays();
  const uint16_t photoDays = config.sdRetentionPhotosDays();
  int deleted = 0;

  // Logi CSV w /logs (szerokie YYYY-MM.csv i długie extra-YYYY-MM.csv)
  if (logDays > 0) {
    std::vector<SdFileInfo> files;
    sdCard.listDir("/logs", files);
    for (auto& f : files) {
      if (!f.path.endsWith(".csv")) continue;
      if (olderThan(f.mtime, logDays)) {
        if (sdCard.deleteFile(f.path)) deleted++;
      }
    }
  }

  // Zdjęcia w /photos/RRRR-MM/RRRR-MM-DD_HHMMSS.jpg
  if (photoDays > 0) {
    // Bez pobierania metadanych każdego wpisu (patrz listDirNames): katalog ze
    // setkami zdjęć zajmowałby dziesiątki sekund i blokował pętlę główną.
    // Czas zapisu zdjęcia jest w jego nazwie.
    std::vector<String> base;
    sdCard.listDirNames("/photos", base);
    for (auto& p : base) {
      if (p.endsWith(".jpg") || p.endsWith(".jpeg")) {
        if (olderThan(sdFileNameTime(p), photoDays)) deletePhotoWithMeta(p, deleted);
      } else {
        // Katalog miesięczny
        std::vector<String> sub;
        sdCard.listDirNames(p, sub, 0, false, ".jpg");
        for (auto& f : sub) {
          if (olderThan(sdFileNameTime(f), photoDays)) deletePhotoWithMeta(f, deleted);
        }
      }
    }
  }

  if (deleted > 0) {
    LOG_I("Retencja SD: usunięto %d starych plików (logi %u dni, zdjęcia %u dni)",
          deleted, (unsigned)logDays, (unsigned)photoDays);
  }
}

void SdRetention::loop() {
  unsigned long now = millis();
  if (lastRun_ == 0) lastRun_ = now;   // start liczenia od pierwszego wywołania
  if (now - lastRun_ >= 3600000UL) {   // potem co godzinę
    lastRun_ = now;
    runOnce();
  }
}
