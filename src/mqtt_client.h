/* =============================================================================
 * Stacja Pogody - stacja meteorologiczna na ESP32-S3 WROOM-1 (N16R8)
 * Copyright (c) 2026 kruzio1985 - https://github.com/kruzio1985
 * Licencja: Stacja Pogody Non-Commercial License 1.0 (plik LICENSE)
 * Użytek niekomercyjny. Kontakt: kruzio1985@users.noreply.github.com
 * =============================================================================
 */
#pragma once
#include <Arduino.h>
#include <WiFiClient.h>
#include <PubSubClient.h>
#include "sensors.h"

class MqttManager {
public:
  void begin(WiFiClient& client);
  void loop();
  bool connected();
  bool connectNow();
  void publishReadings(const std::vector<Channel>& channels);
  void publishStatus(const char* status);
  // Publikacja dowolnego tematu (używana m.in. przez raport serwisów pogodowych)
  bool publishTopic(const String& topic, const String& payload);
  bool setLastWill();
  bool isConfigured();

  // Ponowne wczytanie ustawień MQTT z konfiguracji + jedno próbne połączenie
  // (przycisk "Testuj połączenie" na stronie www). Bez restartu stacji.
  bool reconnectWithConfig(String& info);
  String serverDescription();

private:
  void announceDiscovery(const Channel& c);
  void announceCompassDiscovery();
  String stateTopic(const Channel& c);
  String discoveryTopic(const Channel& c);

  WiFiClient* wc_ = nullptr;
  PubSubClient* mqtt_ = nullptr;
  unsigned long lastReconnect_ = 0;
  unsigned long lastAnnounce_ = 0;
  bool configured_ = false;
};

extern MqttManager mqtt;
