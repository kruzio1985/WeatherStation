/* =============================================================================
 * Stacja Pogody - zewnętrzna kamera ESP32-CAM (AI-Thinker)
 * Copyright (c) 2026 kruzio1985 - https://github.com/kruzio1985
 * Licencja: PolyForm Noncommercial License 1.0.0 (plik LICENSE)
 * Użytek niekomercyjny. Kontakt: kruzio1985@users.noreply.github.com
 * =============================================================================
 *
 * Domyślne ustawienia kamery. Możesz je zmienić w kodzie poniżej ALBO - po
 * wgraniu firmware - na stronie www samej kamery (http://<ip-kamery>/) lub
 * komendą szeregową (monitor 115200): SET ssid hasło url
 *
 * Przykład komendy:  SET SGC mojehaslo http://192.168.1.143
 * (url bez "/capture" - endpoint jest doklejany automatycznie)
 */
#pragma once

#define CAM_WIFI_SSID  "SGC"
#define CAM_WIFI_PASS  ""
#define CAM_MASTER_URL "http://192.168.1.143"

// Hasło aktualizacji OTA kamery (ArduinoOTA, port 3232). Zmień je!
#define CAM_OTA_PASS   "stacja-cam-ota"

// Hasło punktu dostępowego (tryb AP, gdy kamera nie połączy się z Wi-Fi).
// AP: SSID "StacjaKam", adres http://192.168.4.1 - pełna strona konfiguracji.
#define CAM_AP_PASS    "stacja-cam"

// Rozdzielczość zdjęć: FRAMESIZE_QVGA / _VGA / _SVGA / _XGA / _UXGA
#define CAM_FRAME_SIZE FRAMESIZE_VGA
#define CAM_JPEG_QUALITY 10       // 0..63, mniej = lepsza jakość
