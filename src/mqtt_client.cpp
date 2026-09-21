/* =============================================================================
 * Stacja Pogody - stacja meteorologiczna na ESP32-S3 WROOM-1 (N16R8)
 * Copyright (c) 2026 kruzio1985 - https://github.com/kruzio1985
 * Licencja: PolyForm Noncommercial License 1.0.0 (plik LICENSE)
 * Użytek niekomercyjny. Kontakt: kruzio1985@users.noreply.github.com
 * =============================================================================
 */
#include "mqtt_client.h"
#include "config.h"
#include "sensors.h"
#include "syslog.h"
#include <ArduinoJson.h>

MqttManager mqtt;

bool MqttManager::isConfigured() {
  return config.mqttHost().length() > 0;
}

void MqttManager::begin(WiFiClient& client) {
  wc_ = &client;
  mqtt_ = new PubSubClient(client);
  configured_ = isConfigured();
  if (configured_) {
    mqtt_->setServer(config.mqttHost().c_str(), config.mqttPort());
    mqtt_->setBufferSize(1024);
    LOG_I("MQTT: skonfigurowano %s:%u, prefiks '%s'", config.mqttHost().c_str(),
          (unsigned)config.mqttPort(), config.mqttPrefix().c_str());
  } else {
    LOG_W("MQTT: brak adresu brokera - wysyłanie do Home Assistant wyłączone");
  }
}

String MqttManager::stateTopic(const Channel& c) {
  return config.mqttPrefix() + "/sensor/" + c.id + "/state";
}

String MqttManager::discoveryTopic(const Channel& c) {
  return "homeassistant/sensor/" + config.mqttPrefix() + "_" + c.id + "/config";
}

bool MqttManager::publishTopic(const String& topic, const String& payload) {
  if (!connected()) return false;
  return mqtt_->publish(topic.c_str(), payload.c_str(), true);
}

bool MqttManager::setLastWill() {
  // Unikalny identyfikator klienta - dwie stacje z tą samą nazwą nie będą
  // się wzajemnie rozłączać na brokerze.
  String clientId = "stacja-" + String((uint32_t)ESP.getEfuseMac(), HEX);
  String t = config.mqttPrefix() + "/status";
  return mqtt_->connect(clientId.c_str(),
                        config.mqttUser().c_str(), config.mqttPass().c_str(),
                        t.c_str(), 0, true, "offline");
}

String MqttManager::serverDescription() {
  return config.mqttHost() + ":" + String((unsigned)config.mqttPort());
}

// Podpowiedź po kodzie błędu PubSubClient - najczęstsze pomyłki przy
// wpisywaniu danych z Home Assistant.
static const char* mqttStateHint(int st) {
  switch (st) {
    case MQTT_CONNECTION_TIMEOUT:       return "brak odpowiedzi - sprawdź adres IP i port brokera";
    case MQTT_CONNECTION_LOST:          return "połączenie zerwane - sprawdź sieć Wi-Fi";
    case MQTT_CONNECT_FAILED:           return "serwer nieosiągalny - sprawdź adres IP i port";
    case MQTT_CONNECT_BAD_PROTOCOL:     return "broker nie obsługuje tego protokołu";
    case MQTT_CONNECT_BAD_CLIENT_ID:    return "broker odrzucił identyfikator klienta";
    case MQTT_CONNECT_UNAVAILABLE:      return "broker niedostępny - spróbuj ponownie";
    case MQTT_CONNECT_BAD_CREDENTIALS:  return "zły użytkownik lub hasło/token";
    case MQTT_CONNECT_UNAUTHORIZED:     return "brak autoryzacji - zezwól użytkownikowi na MQTT";
    default:                            return "nieznany błąd";
  }
}

bool MqttManager::reconnectWithConfig(String& info) {
  if (!wc_) { info = "Nie zainicjowano klienta sieciowego"; return false; }
  if (!mqtt_) mqtt_ = new PubSubClient(*wc_);

  mqtt_->disconnect();
  configured_ = isConfigured();
  if (!configured_) {
    info = "Nie podano adresu brokera - wpisz IP Home Assistant i port 1883";
    return false;
  }

  mqtt_->setServer(config.mqttHost().c_str(), config.mqttPort());
  mqtt_->setBufferSize(1024);

  unsigned long t0 = millis();
  bool ok = setLastWill();
  unsigned long ms = millis() - t0;

  if (ok) {
    LOG_I("MQTT: test połączenia OK (%s, %u ms)", serverDescription().c_str(), (unsigned)ms);
    publishStatus("online");
    lastAnnounce_ = 0;   // discovery ogłoszone przy najbliższej publikacji
    info = "Połączono z " + serverDescription() + " (" + String((unsigned)ms) + " ms)";
  } else {
    int st = (int)mqtt_->state();
    info = String("Brak połączenia z ") + serverDescription() + " - " +
           mqttStateHint(st) + " [kod " + st + ", " + String((unsigned)ms) + " ms]";
    LOG_W("MQTT: test połączenia nieudany - %s", info.c_str());
  }
  return ok;
}

bool MqttManager::connectNow() {
  if (!configured_ || !mqtt_) return false;
  if (mqtt_->connected()) return true;

  setLastWill();
  if (!mqtt_->connected()) {
    // Ponawiamy co 10 s, ale komunikat w logach tylko raz na minutę.
    static unsigned long lastWarn = 0;
    if (millis() - lastWarn > 60000UL) {
      lastWarn = millis();
      LOG_W("MQTT: brak połączenia z %s:%u (kod %d) - ponawiam",
            config.mqttHost().c_str(), (unsigned)config.mqttPort(), (int)mqtt_->state());
    }
    return false;
  }

  LOG_I("MQTT: połączono z %s:%u - ogłaszam czujniki w Home Assistant",
        config.mqttHost().c_str(), (unsigned)config.mqttPort());
  publishStatus("online");
  lastAnnounce_ = 0; // wymuś ponowne ogłoszenie discovery
  return true;
}

bool MqttManager::connected() {
  return mqtt_ && mqtt_->connected();
}

void MqttManager::loop() {
  if (!configured_ || !mqtt_) return;
  if (mqtt_->connected()) {
    mqtt_->loop();
    return;
  }
  unsigned long now = millis();
  if (now - lastReconnect_ > 10000) {
    lastReconnect_ = now;
    connectNow();
  }
}

void MqttManager::announceDiscovery(const Channel& c) {
  // Tylko kanały z potwierdzonym sprzętowo czujnikiem (detected) trafiają do HA,
  // żeby w Home Assistant nie pojawiały się "widma" niepodłączonych czujników.
  if (!c.detected || !c.enabled) return;

  JsonDocument d;
  d["name"] = config.deviceName() + " " + c.name;
  d["uniq_id"] = config.mqttPrefix() + "_" + c.id;
  d["stat_t"] = stateTopic(c);
  d["avty_t"] = config.mqttPrefix() + "/status";
  if (c.haClass.length()) d["dev_cla"] = c.haClass;
  if (c.haUnit.length()) d["unit_of_meas"] = c.haUnit;
  if (c.haIcon.length()) d["ic"] = c.haIcon;
  d["exp_aft"] = 600;

  JsonObject dev = d["dev"].to<JsonObject>();
  dev["name"] = config.deviceName();
  dev["mdl"] = "Stacja Pogody ESP32-S3";
  dev["sw"] = FW_VERSION_FULL;
  dev["mf"] = config.companyName().length() ? config.companyName() : "DIY";

  String payload;
  serializeJson(d, payload);
  mqtt_->publish(discoveryTopic(c).c_str(), payload.c_str(), true);
}

void MqttManager::publishReadings(const std::vector<Channel>& channels) {
  if (!connected()) return;

  unsigned long now = millis();
  bool announce = (now - lastAnnounce_ > 300000); // co 5 min
  if (announce) lastAnnounce_ = now;

  for (const auto& c : channels) {
    if (!c.enabled || !c.detected || isnan(c.value)) continue;

    if (announce) announceDiscovery(c);

    float v = c.value * c.haFactor;
    char buf[32];
    dtostrf(v, 0, 3, buf);
    mqtt_->publish(stateTopic(c).c_str(), buf, true);
  }

  // Kierunek wiatru jako strona świata (N/NE/E...). Osobny temat tekstowy,
  // bo w Home Assistant wartość liczbowa w stopniach jest nieczytelna.
  String dir = sensors.windCompass();
  if (dir.length()) {
    if (announce) announceCompassDiscovery();
    String t = config.mqttPrefix() + "/vane_dir/state";
    mqtt_->publish(t.c_str(), dir.c_str(), true);
  }

  publishStatus("online");
}

void MqttManager::announceCompassDiscovery() {
  JsonDocument d;
  d["name"] = config.deviceName() + " Kierunek wiatru (N/E/S/W)";
  d["uniq_id"] = config.mqttPrefix() + "_vane_dir";
  d["stat_t"] = config.mqttPrefix() + "/vane_dir/state";
  d["avty_t"] = config.mqttPrefix() + "/status";
  d["ic"] = "mdi:compass";
  d["exp_aft"] = 600;

  JsonObject dev = d["dev"].to<JsonObject>();
  dev["name"] = config.deviceName();
  dev["mdl"] = "Stacja Pogody ESP32-S3";
  dev["sw"] = FW_VERSION_FULL;
  dev["mf"] = config.companyName().length() ? config.companyName() : "DIY";

  String payload;
  serializeJson(d, payload);
  String topic = "homeassistant/sensor/" + config.mqttPrefix() + "_vane_dir/config";
  mqtt_->publish(topic.c_str(), payload.c_str(), true);
}

void MqttManager::publishStatus(const char* status) {
  if (!connected()) return;
  String t = config.mqttPrefix() + "/status";
  mqtt_->publish(t.c_str(), status, true);
}
