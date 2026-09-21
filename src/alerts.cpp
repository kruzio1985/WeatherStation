/* =============================================================================
 * Stacja Pogody - stacja meteorologiczna na ESP32-S3 WROOM-1 (N16R8)
 * Copyright (c) 2026 kruzio1985 - https://github.com/kruzio1985
 * Licencja: PolyForm Noncommercial License 1.0.0 (plik LICENSE)
 * Użytek niekomercyjny. Kontakt: kruzio1985@users.noreply.github.com
 * =============================================================================
 */
#include "alerts.h"
#include "config.h"
#include "mqtt_client.h"
#include "sensors.h"
#include "syslog.h"
#include <ArduinoJson.h>
#include <time.h>

AlertManager alerts;

namespace {

struct AlertMetric {
  const char* id;
  const char* name;
  const char* unit;
};

// Lista kanałów objętych alertami - tyle samo pozycji co ALERT_METRIC_COUNT.
const AlertMetric ALERT_METRICS[ALERT_METRIC_COUNT] = {
  { "temp",  "Temperatura",        "°C"     },
  { "hum",   "Wilgotność",         "%"      },
  { "press", "Ciśnienie",          "hPa"    },
  { "wind",  "Wiatr",              "km/h"   },
  { "rain",  "Opad dobowy",        "mm"     },
  { "pm25",  "PM2.5",              "µg/m³"  },
  { "pm10",  "PM10",               "µg/m³"  },
  { "co2",   "CO₂",                "ppm"    },
  { "uv",    "Indeks UV",          "UVI"    },
  { "soil",  "Wilgotność gleby",   "%"      },
  { "light", "Natężenie światła",  "lx"     },
};

String minKey(const char* id) { return String("alrt_") + id + "_min"; }
String maxKey(const char* id) { return String("alrt_") + id + "_max"; }

bool minSet(const char* id) { return config.hasExtra(minKey(id).c_str()); }
bool maxSet(const char* id) { return config.hasExtra(maxKey(id).c_str()); }
float minVal(const char* id) { return config.extraF(minKey(id).c_str(), 0.0f); }
float maxVal(const char* id) { return config.extraF(maxKey(id).c_str(), 0.0f); }

String alertTopic(const char* id) {
  return config.mqttPrefix() + "/alert/" + id + "/state";
}

String alertDiscoveryTopic(const char* id) {
  return "homeassistant/binary_sensor/" + config.mqttPrefix() + "_alert_" + id + "/config";
}

void publishAlert(const char* id, bool on) {
  String payload = on ? "ON" : "OFF";
  if (mqtt.publishTopic(alertTopic(id), payload)) {
    LOG_I("Alert %s: %s (temat %s)", id, payload.c_str(), alertTopic(id).c_str());
  }
}

void announceAlertDiscovery(const char* id, const char* name) {
  JsonDocument d;
  d["name"] = config.deviceName() + " Alert: " + String(name);
  d["uniq_id"] = config.mqttPrefix() + "_alert_" + id;
  d["stat_t"] = alertTopic(id);
  d["avty_t"] = config.mqttPrefix() + "/status";
  d["ic"] = "mdi:alert";
  d["payload_on"] = "ON";
  d["payload_off"] = "OFF";

  JsonObject dev = d["dev"].to<JsonObject>();
  dev["name"] = config.deviceName();
  dev["mdl"] = "Stacja Pogody ESP32-S3";
  dev["sw"] = FW_VERSION_FULL;
  dev["mf"] = config.companyName().length() ? config.companyName() : "DIY";

  String payload;
  serializeJson(d, payload);
  mqtt.publishTopic(alertDiscoveryTopic(id), payload);
}

} // namespace

void AlertManager::begin() {
  lastCheck_ = 0;
  lastAnnounce_ = 0;
  for (int i = 0; i < ALERT_METRIC_COUNT; i++) states_[i] = State();
}

bool AlertManager::enabled() {
  return config.extraF("alrt_on", 0.0f) >= 0.5f;
}

void AlertManager::setEnabled(bool on) {
  config.setExtraF("alrt_on", on ? 1.0f : 0.0f);
}

void AlertManager::loop() {
  unsigned long now = millis();
  if (now - lastCheck_ < 10000UL) return;
  lastCheck_ = now;

  bool on = enabled();

  // Gdy alerty wyłączone, zdejmij wszystkie aktywne stany (raz).
  if (!on) {
    for (int i = 0; i < ALERT_METRIC_COUNT; i++) {
      if (states_[i].active) {
        states_[i].active = false;
        publishAlert(ALERT_METRICS[i].id, false);
      }
    }
    return;
  }

  unsigned long cooldown = (unsigned long)config.extraF("alrt_cool", 3600.0f);
  if (cooldown < 60) cooldown = 60;

  // Odśwież discovery co 5 min (dla skonfigurowanych progów).
  bool announce = (now - lastAnnounce_ > 300000UL);
  if (announce) lastAnnounce_ = now;

  for (int i = 0; i < ALERT_METRIC_COUNT; i++) {
    const char* id = ALERT_METRICS[i].id;
    if (!minSet(id) && !maxSet(id)) continue;

    float v = sensors.valueOf(id);
    State& st = states_[i];
    if (isnan(v)) continue; // brak danych - zostaw stan bez zmian

    float mn = minVal(id);
    float mx = maxVal(id);
    bool hasMin = minSet(id);
    bool hasMax = maxSet(id);

    // Martwa strefa 0.5% progu (min. 0.01) zapobiega migotaniu alarmu,
    // gdy pomiar oscyluje dokładnie wokół wartości progowej.
    auto eps = [](float th) {
      float e = fabsf(th) * 0.005f;
      return e < 0.01f ? 0.01f : e;
    };

    bool cond = false;
    if (hasMin && v < mn - eps(mn)) cond = true;
    if (hasMax && v > mx + eps(mx)) cond = true;

    if (cond && !st.active) {
      st.active = true;
      st.activeAt = now;
      st.lastNotify = now;
      publishAlert(id, true);
      LOG_W("Alert %s: %.2f %s poza progiem [%s, %s]",
            ALERT_METRICS[i].name, v, ALERT_METRICS[i].unit,
            hasMin ? String(mn, 2).c_str() : "-∞",
            hasMax ? String(mx, 2).c_str() : "+∞");
    } else if (cond && st.active) {
      // Przypomnienie co okres cooldown, dopóki stan trwa.
      if (now - st.lastNotify >= cooldown * 1000UL) {
        st.lastNotify = now;
        publishAlert(id, true);
        LOG_W("Alert %s: nadal poza progiem (%.2f %s)",
              ALERT_METRICS[i].name, v, ALERT_METRICS[i].unit);
      }
    } else if (!cond && st.active) {
      // Powrót do zakresu dopiero po wyjściu poza martwą strefę.
      bool clear = true;
      if (hasMin && v < mn + eps(mn)) clear = false;
      if (hasMax && v > mx - eps(mx)) clear = false;
      if (clear) {
        st.active = false;
        publishAlert(id, false);
        LOG_I("Alert %s: powrót do normy (%.2f %s)",
              ALERT_METRICS[i].name, v, ALERT_METRICS[i].unit);
      }
    }

    if (announce && (hasMin || hasMax)) announceAlertDiscovery(id, ALERT_METRICS[i].name);
  }
}

String AlertManager::json() {
  JsonDocument d;
  d["enabled"] = enabled();
  d["cooldown_s"] = (unsigned long)config.extraF("alrt_cool", 3600.0f);

  JsonArray arr = d["metrics"].to<JsonArray>();
  for (int i = 0; i < ALERT_METRIC_COUNT; i++) {
    const char* id = ALERT_METRICS[i].id;
    JsonObject m = arr.add<JsonObject>();
    m["id"] = id;
    m["name"] = ALERT_METRICS[i].name;
    m["unit"] = ALERT_METRICS[i].unit;
    if (minSet(id)) m["min"] = minVal(id); else m["min"] = nullptr;
    if (maxSet(id)) m["max"] = maxVal(id); else m["max"] = nullptr;
    m["active"] = states_[i].active;
    float v = sensors.valueOf(id);
    if (isnan(v)) m["value"] = nullptr; else m["value"] = v;
  }

  String out;
  serializeJson(d, out);
  return out;
}

bool AlertManager::applyJson(const char* json, size_t len) {
  JsonDocument nd;
  DeserializationError err = deserializeJson(nd, json, len);
  if (err) {
    LOG_W("Alerty: błędny JSON ustawień (%s)", err.c_str());
    return false;
  }

  if (nd["enabled"].is<bool>()) {
    setEnabled(nd["enabled"].as<bool>());
  }
  if (nd["cooldown_s"].is<float>() || nd["cooldown_s"].is<int>()) {
    float c = nd["cooldown_s"] | 3600.0f;
    if (c < 60) c = 60;
    config.setExtraF("alrt_cool", c);
  }

  JsonArrayConst arr = nd["metrics"].as<JsonArrayConst>();
  if (arr) {
    for (JsonObjectConst m : arr) {
      const char* id = m["id"] | "";
      if (!*id) continue;
      // Sprawdź, czy id jest na liście alertów (ochrona przed śmieciem w NVS).
      bool known = false;
      for (int i = 0; i < ALERT_METRIC_COUNT; i++) {
        if (String(ALERT_METRICS[i].id) == id) { known = true; break; }
      }
      if (!known) continue;

      if (m["min"].isNull()) config.removeExtra(minKey(id).c_str());
      else if (m["min"].is<float>() || m["min"].is<int>()) config.setExtraF(minKey(id).c_str(), m["min"] | 0.0f);

      if (m["max"].isNull()) config.removeExtra(maxKey(id).c_str());
      else if (m["max"].is<float>() || m["max"].is<int>()) config.setExtraF(maxKey(id).c_str(), m["max"] | 0.0f);
    }
  }

  bool saved = config.save();
  LOG_I("Alerty: zapis progów (enabled=%d, cool=%u s, save=%d)",
        (int)enabled(), (unsigned)(unsigned long)config.extraF("alrt_cool", 3600.0f), (int)saved);
  return saved;
}
