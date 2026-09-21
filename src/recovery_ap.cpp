/* =============================================================================
 * Stacja Pogody - stacja meteorologiczna na ESP32-S3 WROOM-1 (N16R8)
 * Copyright (c) 2026 kruzio1985 - https://github.com/kruzio1985
 * Licencja: Stacja Pogody Non-Commercial License 1.0 (plik LICENSE)
 * Użytek niekomercyjny. Kontakt: kruzio1985@users.noreply.github.com
 * =============================================================================
 */
#include "recovery_ap.h"
#include "config.h"
#include "pinmap.h"
#include "rs485.h"
#include "syslog.h"

#if STACJA_HEADLESS && STACJA_NODE_RECOVERY_AP
#include <WiFi.h>
#include <WebServer.h>

#define RECOVERY_AP_PASS "12345678"

// Po jakim czasie bez poprawnej ramki od mastera włącza się AP. Minimum 60 s
// i zawsze co najmniej interwał odpytywania + 20 s (żeby AP nie migotał,
// gdy master odpytuje rzadko).
static uint32_t waitMs() {
  uint32_t w = 1000UL * ((uint32_t)config.rs485PollS() + 20UL);
  return w < 60000UL ? 60000UL : w;
}

static String optionHtml(uint32_t value, uint32_t current) {
  String s = "<option value='";
  s += value;
  s += "'";
  if (value == current) s += " selected";
  s += ">";
  s += value;
  s += " b/s</option>";
  return s;
}

static String pageHtml() {
  uint32_t baud = config.rs485BaudManual() ? config.rs485Baud() : (uint32_t)STACJA_NODE_BAUD;
  uint8_t  addr = config.rs485Addr();
  int rx = pinMap.pin("rs485_rx");
  int tx = pinMap.pin("rs485_tx");
  int de = pinMap.pin("rs485_de");
  const Rs485Stats& st = rs485.stats();

  String h;
  h.reserve(2400);
  h += "<!DOCTYPE html><html lang='pl'><head><meta charset='utf-8'>";
  h += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  h += "<title>Stacja Pogody - konfiguracja węzła</title>";
  h += "<style>body{font-family:system-ui,sans-serif;max-width:560px;margin:20px auto;padding:0 14px;background:#111;color:#eee}"
       "h1{font-size:1.3em}.card{background:#1d1d1f;border-radius:12px;padding:16px;margin:12px 0}"
       "label{display:block;margin:10px 0 4px}input,select{width:100%;box-sizing:border-box;padding:9px;border-radius:8px;"
       "border:1px solid #444;background:#26262a;color:#eee;font-size:1em}"
       ".btn{display:inline-block;background:#0a84ff;color:#fff;border:0;border-radius:8px;padding:11px 16px;margin-top:14px;"
       "font-size:1em;text-decoration:none}.muted{color:#9aa;font-size:.9em}.ok{color:#4cd964}</style></head><body>";
  h += "<h1>Stacja Pogody - awaryjna konfiguracja węzła</h1>";
  h += "<div class='card'><p class='muted'>Firmware " FW_VERSION_FULL ", adres " + String((unsigned)addr) + "</p>";
  h += "<p class='muted'>Kontakt z masterem: ";
  if (st.lastOkMs) {
    h += "<span class='ok'>jest</span> (ostatnia ramka " + String((millis() - st.lastOkMs) / 1000) + " s temu, poprawnych " + String(st.ok) + ")";
  } else {
    h += "brak";
  }
  h += " | surowe bajty RX: " + String(st.rawRx) + "</p></div>";

  h += "<form action='/save' method='get'><div class='card'>";
  h += "<label>Adres węzła na magistrali (2-247)</label>";
  h += "<input type='number' name='addr' min='2' max='247' value='" + String((unsigned)addr) + "'>";
  h += "<label>Prędkość magistrali</label><select name='baud'>";
  h += optionHtml(9600, baud);
  h += optionHtml(19200, baud);
  h += optionHtml(38400, baud);
  h += optionHtml(57600, baud);
  h += optionHtml(115200, baud);
  h += "</select>";
  h += "<label>Pin RX (odbiór od mastera)</label><input type='number' name='rx' min='0' max='30' value='" + String(rx) + "'>";
  h += "<label>Pin TX (nadawanie do mastera)</label><input type='number' name='tx' min='0' max='30' value='" + String(tx) + "'>";
  h += "<label>Pin DE/RE (kierunek; -1 = połączenie bezpośrednie TX-RX-GND)</label><input type='number' name='de' min='-1' max='30' value='" + String(de) + "'>";
  h += "<p class='muted'>Po zapisaniu węzeł zapisze ustawienia w NVS i uruchomi się ponownie.</p>";
  h += "<button class='btn' type='submit'>Zapisz i uruchom ponownie</button>";
  h += "</div></form>";

  h += "<div class='card'>";
  h += "<a class='btn' href='/stop'>Wyłącz AP i pracuj dalej</a>";
  h += " <a class='btn' href='/save?baud=" + String(baud) + "&addr=" + String((unsigned)addr) +
       "&rx=" + String(rx) + "&tx=" + String(tx) + "&de=" + String(de) +
       "'>Uruchom ponownie bez zmian</a>";
  h += "</div></body></html>";
  return h;
}

void RecoveryAp::begin() {
  bootMs_ = millis();
  LOG_I("Awaryjny AP: włączy się, gdy przez %u s nie będzie poprawnej ramki od mastera",
        (unsigned)(waitMs() / 1000));
}

void RecoveryAp::loop() {
  if (active_) {
    server_->handleClient();
    return;
  }
  if (stopped_) return;

  uint32_t contact = rs485.stats().lastOkMs;
  if (contact == 0) contact = bootMs_;
  if (millis() - contact >= waitMs()) start();
}

void RecoveryAp::start() {
  if (active_) return;

  uint8_t addr = config.rs485Addr();
  String ssid = String("StacjaNode-") + String((unsigned)addr);
  String pass = config.apPass();
  if (pass.length() < 8) pass = RECOVERY_AP_PASS;

  LOG_I("Awaryjny AP: brak łączności z masterem - uruchamiam '%s' (http://192.168.4.1/)", ssid.c_str());
  WiFi.mode(WIFI_AP);
  WiFi.setSleep(false);
  IPAddress ap(192, 168, 4, 1);
  WiFi.softAPConfig(ap, ap, IPAddress(255, 255, 255, 0));
  if (!WiFi.softAP(ssid.c_str(), pass.c_str(), 1, false)) {
    LOG_E("Awaryjny AP: nie udało się uruchomić softAP");
    return;
  }

  server_ = new WebServer(80);
  server_->on("/", HTTP_GET, [this]() { handleRoot(); });
  server_->on("/save", HTTP_GET, [this]() { handleSave(); });
  server_->on("/stop", HTTP_GET, [this]() { handleStop(); });
  server_->begin();
  active_ = true;
  LOG_I("Awaryjny AP: gotowy - SSID '%s', hasło '%s'", ssid.c_str(), pass.c_str());
}

void RecoveryAp::stop() {
  if (server_) {
    server_->stop();
    delete server_;
    server_ = nullptr;
  }
  active_ = false;
  stopped_ = true;
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  LOG_I("Awaryjny AP wyłączony - węzeł wraca do normalnej pracy po RS485");
}

void RecoveryAp::handleRoot() {
  server_->send(200, "text/html; charset=utf-8", pageHtml());
}

void RecoveryAp::handleSave() {
  long baud = server_->arg("baud").toInt();
  long addr = server_->arg("addr").toInt();
  long rx = server_->arg("rx").toInt();
  long tx = server_->arg("tx").toInt();
  long de = server_->arg("de").toInt();

  if (baud != 9600 && baud != 19200 && baud != 38400 && baud != 57600 && baud != 115200) {
    server_->send(400, "text/plain; charset=utf-8",
                  "Błędna prędkość. Dozwolone: 9600, 19200, 38400, 57600, 115200.");
    return;
  }
  if (addr < 2 || addr > 247) {
    server_->send(400, "text/plain; charset=utf-8", "Adres węzła musi być z zakresu 2-247.");
    return;
  }

  String e;
  if (!pinMap.set("rs485_rx", (int)rx, e) || !pinMap.set("rs485_tx", (int)tx, e) ||
      !pinMap.set("rs485_de", (int)de, e)) {
    server_->send(400, "text/plain; charset=utf-8", "Błąd pinów: " + e);
    return;
  }
  pinMap.save();

  String js = "{\"rs485_baud\":" + String(baud) +
              ",\"rs485_baud_manual\":true,\"rs485_addr\":" + String(addr) + "}";
  if (!config.applyJson(js.c_str(), js.length())) {
    server_->send(500, "text/plain; charset=utf-8", "Nie udało się zapisać ustawień w NVS.");
    return;
  }

  server_->send(200, "text/html; charset=utf-8",
                "<!DOCTYPE html><html lang='pl'><head><meta charset='utf-8'>"
                "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                "<title>Zapisano</title><style>body{font-family:system-ui;background:#111;color:#eee;"
                "text-align:center;padding-top:80px}</style></head><body>"
                "<h1>Zapisano ustawienia</h1><p>Adres " + String(addr) + ", " + String(baud) +
                " b/s, RX " + String(rx) + ", TX " + String(tx) + ", DE " + String(de) +
                ".<br>Węzeł uruchamia się ponownie...</p></body></html>");

  delay(500);
  ESP.restart();
}

void RecoveryAp::handleStop() {
  server_->send(200, "text/html; charset=utf-8",
                "<!DOCTYPE html><html lang='pl'><head><meta charset='utf-8'>"
                "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                "<title>AP wyłączony</title><style>body{font-family:system-ui;background:#111;"
                "color:#eee;text-align:center;padding-top:80px}</style></head><body>"
                "<h1>Punkt dostępowy wyłączony</h1><p>Węzeł pracuje dalej po magistrali RS485.</p>"
                "</body></html>");
  stop();
}

#else
// Awaryjny AP wyłączony (master albo węzeł bez flagi STACJA_NODE_RECOVERY_AP).
void RecoveryAp::begin() {}
void RecoveryAp::loop() {}
void RecoveryAp::start() {}
void RecoveryAp::stop() {}
void RecoveryAp::handleRoot() {}
void RecoveryAp::handleSave() {}
void RecoveryAp::handleStop() {}
#endif

RecoveryAp recoveryAp;
