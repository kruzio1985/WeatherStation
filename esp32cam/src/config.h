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
 * Przykład komendy:  SET SSID haslo http://192.168.1.100
 * (url bez "/capture" - endpoint jest doklejany automatycznie)
 *
 * UWAGA: domyślnie WSZYSTKIE pola są puste - publiczna kopia projektu nie
 * zawiera danych konkretnej instalacji. Po pierwszym uruchomieniu kamera
 * wystawia punkt dostępowy "StacjaKam" (http://192.168.4.1) i tam wpisujesz
 * własne Wi-Fi i adres mastera; trafiają do NVS kamery, nie do kodu.
 */
#pragma once

#define CAM_WIFI_SSID  ""
#define CAM_WIFI_PASS  ""
#define CAM_MASTER_URL "http://192.168.1.100"

// Hasło aktualizacji OTA kamery (ArduinoOTA, port 3232). Zmień je!
#define CAM_OTA_PASS   "stacja-cam-ota"

// Hasło punktu dostępowego (tryb AP, gdy kamera nie połączy się z Wi-Fi).
// AP: SSID "StacjaKam", adres http://192.168.4.1 - pełna strona konfiguracji.
#define CAM_AP_PASS    "stacja-cam"

// Rozdzielczość zdjęć: FRAMESIZE_QVGA / _VGA / _SVGA / _XGA / _UXGA
#define CAM_FRAME_SIZE FRAMESIZE_VGA
#define CAM_JPEG_QUALITY 10       // 0..63, mniej = lepsza jakość
