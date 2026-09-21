/* =============================================================================
 * Stacja Pogody - stacja meteorologiczna na ESP32-S3 WROOM-1 (N16R8)
 * Copyright (c) 2026 kruzio1985 - https://github.com/kruzio1985
 * Licencja: Stacja Pogody Non-Commercial License 1.0 (plik LICENSE)
 * Użytek niekomercyjny. Kontakt: kruzio1985@users.noreply.github.com
 * =============================================================================
 */

// =============================================================
//  Moduł sterowników TERMOPARY / RTD / ADC na SPI.
//
//  SPI tylko programowe (bit-bang) - bez <SPI.h> i bez bibliotek
//  zewnętrznych, więc magistrala może siedzieć na dowolnych GPIO.
//
//  Obsługiwane układy:
//    * MAX6675  - termopara K, ramka 16 bit (0,25 °C, bit2 = obwód rozwarty)
//    * MAX31855 - termopara K/J/N/T/S/R/E, ramka 32 bit
//                 (14 bit termopara + 12 bit zimne końce + bity błędu)
//    * MAX31865 - RTD PT100/PT1000, rejestr 16 bit + bit błędu,
//                 konfiguracja 2/3/4 przewody i filtr 50/60 Hz
//    * MCP3008  - 8 kanałów, ADC SAR 10 bit
//    * MCP3208  - 8 kanałów, ADC SAR 12 bit
//
//  Piny (mapa pinów, wartość < 0 = funkcja wyłączona):
//    tc_sck / tc_mosi / tc_miso / tc_cs
//        - MAX6675 i MAX31855 (MOSI nieużywane, ale magistrala wspólna)
//        - MCP3008 / MCP3208 (pełne cztery linie, ten sam CS co termopara)
//    rtd_cs
//        - MAX31865 (dzieli SCK/MOSI/MISO z termoparą, ma własny CS)
//  Brak któregokolwiek wymaganego pinu = układ pomijany, bez logu błędu.
//
//  Kanały:
//    tc_max6675        [°C]  temperatura termopary (MAX6675)
//    tc_31855_t        [°C]  temperatura termopary (MAX31855)
//    tc_31855_cj       [°C]  temperatura zimnych końców (MAX31855)
//    rtd_temp          [°C]  temperatura RTD (MAX31865)
//    rtd_res           [Ω]   rezystancja RTD (MAX31865)
//    adc_mcp_ch0..ch7  [V]   napięcia na wejściach MCP3008 / MCP3208
//
//  Kalibracja (tylko odczyt z config.extraF, bez zapisu NVS/EEPROM):
//    tc_type      - litera termopary: 'K','J','N','T','S','R','E' (dom. 'K')
//    rtd_rref     - rezystancja odniesienia MAX31865 w omach (430 / 4300)
//    rtd_r0       - rezystancja RTD w 0 °C (100 dla PT100, 1000 dla PT1000)
//    rtd_wires    - liczba przewodów: 2 / 3 / 4 (dom. 2)
//    rtd_filter   - filtr przeciwzakłóceniowy: 50 / 60 Hz (dom. 50)
//    adc_spi_vref - napięcie odniesienia MCP3x08 w woltach (dom. 3,3)
//
//  Rejestrację robi wywołujący: extern w src/drv_table.cpp + wpis
//  w tabeli MODULES[].
// =============================================================
#pragma once
#include "drv_mod.h"

extern const DrvModule drvTc;
