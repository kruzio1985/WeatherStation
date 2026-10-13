/* =============================================================================
 * Stacja Pogody - dzienna kopia logów węzłów na kartę SD
 * Copyright (c) 2026 kruzio1985 - https://github.com/kruzio1985
 * Licencja: PolyForm Noncommercial License 1.0.0 (plik LICENSE)
 * ========================================================================== */
#include "node_log_backup.h"

#include <HTTPClient.h>
#include <esp_task_wdt.h>
#include <time.h>

#include "extdev.h"
#include "sd_card.h"
#include "syslog.h"

NodeLogBackup nodeLogs;

// Godzina (czas lokalny), po której wolno zrobić dobową kopię.
static const int BACKUP_HOUR = 3;
static const char* LOG_DIR = "/logi";

static String dayStr(const struct tm& t) {
  char b[12];
  strftime(b, sizeof(b), "%Y-%m-%d", &t);
  return String(b);
}

static String stampStr(const struct tm& t) {
  char b[24];
  strftime(b, sizeof(b), "%Y-%m-%d %H:%M:%S", &t);
  return String(b);
}

// Nazwa urządzenia trafia do nazwy pliku, więc zostawiamy tylko bezpieczne znaki.
static String safeName(String s) {
  for (size_t i = 0; i < s.length(); i++) {
    const char c = s[i];
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
    if (!ok) s[i] = '_';
  }
  if (s.length() == 0) s = "node";
  return s;
}

// "http://192.168.1.147/api/status" -> "http://192.168.1.147"
static String baseUrl(String url) {
  url.trim();
  if (url.startsWith("http://")) {
    const int slash = url.indexOf('/', 7);
    if (slash > 0) url = url.substring(0, slash);
  }
  while (url.endsWith("/")) url.remove(url.length() - 1);
  return url;
}

void NodeLogBackup::runBackup() {
  if (!sdCard.mounted()) {
    status_ = "karta SD niezamontowana";
    LOG_W("Kopia logów: %s", status_.c_str());
    return;
  }

  const time_t nowT = time(nullptr);
  struct tm tmv;
  localtime_r(&nowT, &tmv);
  const String day = dayStr(tmv);
  const String stamp = stampStr(tmv);

  int done = 0, failed = 0, skipped = 0;
  size_t bytes = 0;
  String details;

  for (int i = 0; i < extdev.count(); i++) {
    const ExtDevice* d = extdev.at(i);
    if (!d) continue;
    if (!(d->type == "aq" || d->type == "outaq")) continue;   // tylko węzły z /log.txt
    esp_task_wdt_reset();

    const String url = baseUrl(d->url) + "/log.txt";
    const String path = String(LOG_DIR) + "/" + safeName(d->name) + "-" + day + ".txt";
    if (sdCard.exists(path)) { skipped++; continue; }         // dzisiejsza kopia już jest

    HTTPClient h;
    h.setConnectTimeout(3000);
    h.setTimeout(6000);
    if (!h.begin(url)) {
      failed++;
      details += " " + d->name + ":brak-połączenia";
      continue;
    }
    const int code = h.GET();
    String body;
    if (code == 200) body = h.getString();
    h.end();
    esp_task_wdt_reset();

    if (code == 404) {                                    // urządzenie nie ma /log.txt
      skipped++;
      details += " " + d->name + ":brak-logu";
      continue;
    }
    if (code != 200 || body.length() == 0) {
      failed++;
      details += " " + d->name + ":HTTP" + String(code);
      continue;
    }

    const String head = "=== " + d->name + " (" + url + ") kopia " + stamp + " ===\n";
    const String text = head + body;
    sdCard.mkdirs(LOG_DIR);
    if (sdCard.writeBlock(path, (const uint8_t*)text.c_str(), text.length())) {
      done++;
      bytes += body.length();
      LOG_I("Kopia logów: %s -> %s (%u B)", d->name.c_str(), path.c_str(), (unsigned)body.length());
    } else {
      failed++;
      details += " " + d->name + ":zapis";
    }
    esp_task_wdt_reset();
  }

  status_ = String(done) + " kopii, " + String(skipped) + " już było, " +
            String(failed) + " błędów, " + String((unsigned)bytes) + " B" + details;
  if (done || failed) LOG_I("Kopia logów węzłów: %s", status_.c_str());
}

void NodeLogBackup::loop() {
  if (!enabled_) return;
  const unsigned long now = millis();
  if (now - lastCheck_ < 60000UL) return;   // sprawdzamy raz na minutę
  lastCheck_ = now;

  const time_t t = time(nullptr);
  if (t < 1700000000) return;               // brak sensownego czasu (SNTP jeszcze nie zsynchronizował)

  struct tm tmv;
  localtime_r(&t, &tmv);
  if (tmv.tm_hour < BACKUP_HOUR) return;    // kopię robimy po 3:00

  runBackup();
}
