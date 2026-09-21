# ============================================================================
#  Stacja Pogody - gotowe pliki do wgrania (katalog dist\)
#
#  Buduje firmware w wersjach roli i sklada je w jeden plik do wgrania:
#
#    StacjaPogody-<wersja>-master-16mb.bin       ESP #1 (pulpit) - 16 MB
#    StacjaPogody-<wersja>-node-8mb-quad.bin     Wezel (8 MB, PSRAM quad - N8R4 / N8R2)
#    StacjaPogody-<wersja>-node-c3-4mb.bin       Wezel (ESP32-C3 SuperMini)
#    StacjaPogody-<wersja>-node-c3-4mb-addrN.bin Wezel C3 z adresem fabrycznym N
#                                                (gotowe dla modulow 3..8 na magistrali)
#    StacjaPogody-<wersja>-node-c3-v3-4mb.bin    Wezel C3 (rdzen Arduino 3.x, ESP-IDF 5.x)
#    StacjaPogody-<wersja>-node-c6-4mb.bin       Wezel ESP32-C6 (rdzen Arduino 3.x, ESP-IDF 5.x)
#    StacjaPogody-<wersja>-node-c6-4mb-addrN.bin Wezel C6 z adresem fabrycznym N
#                                                (gotowe dla modulow 3..8 na magistrali)
#    StacjaPogody-<wersja>-<wariant>-ota.bin     sam firmware (OTA / strona www)
#    StacjaPogody-<wersja>-www-16mb.bin         strona www + binarki wezla (LittleFS, 16 MB)
#    StacjaPogody-<wersja>-www-8mb.bin          strona www bez binarek wezla (LittleFS, 8 MB)
#    StacjaPogody-<wersja>-cam-4mb.bin          Kamera ESP32-CAM (AI-Thinker, 4 MB)
#    StacjaPogody-<wersja>-cam-4mb-ota.bin      sam firmware kamery (OTA, port 3232)
#
#  Trzy binarki wystarczaja na cala instalacje: master + dwa typy wezla po RS485
#  (dokladnie te dwa sa publikowane na stronie www, zakladka "ESP i magistrala RS485").
#  Wezel na 16 MB (N16R8) i na 8 MB z PSRAM octal (N8R8) nie ma gotowego pliku -
#  buduje sie go recznie: pio run -e esp32s3-node / -e esp32s3-node-8mb.
#
#  Pliki bez "-ota" wgrywa sie od adresu 0x0 (bootloader + tablica partycji
#  + firmware). Kasuja one ustawienia zapisane w NVS - nadaja sie na nowa
#  plytke. Do aktualizacji dzialajacej stacji sluzy plik "-ota.bin".
#  Przyklad:
#    esptool --chip esp32s3 --port COM9 write_flash 0x0 StacjaPogody-1.0.0-master-16mb.bin
#    esptool --chip esp32c3 --port COM9 write_flash 0x0 StacjaPogody-1.0.0-node-c3-4mb.bin
#
#  Uzycie:
#    powershell -ExecutionPolicy Bypass -File tools\build_release.ps1
#    ... -SkipBuild   (tylko zebranie tego, co juz lezy w .pio\build)
#    ... -SkipWww     (bez przebudowy obrazu LittleFS)
#    ... -PublishNode  (kopiuje TYLKO obrazki wezla C3 do data\node, zeby
#                       stacja mogla je wydawac przez /api/boards i strone www;
#                       dzieje sie to przed budowaniem obrazow LittleFS.
#                       Pozostale wezly (S3, C6, warianty z adresem) instaluje
#                       sie recznie z plikow w dist\. Stare binarki w data\node
#                       sa czyszczone, wiec obraz www jest zawsze aktualny.)
#
#  Uwaga: obraz -www-8mb.bin nie zawiera katalogu data\node - binarki wezla
#  (4,6 MB) nie mieszcza sie na partycji LittleFS plytki 8 MB (1,88 MB).
#  Wydaje je tylko master (obraz -www-16mb.bin) przez /api/boards. Dlatego
#  srodowiska wezla buduja system plikow bez data\node (tools/stage_node_fs.py
#  w extra_scripts), a ten skrypt sklada obraz 8 MB tym samym srodowiskiem.
# ============================================================================
[CmdletBinding()]
param(
  [switch]$SkipBuild,
  [switch]$SkipWww,
  [switch]$PublishNode
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

# --- wersja firmware z src\config.h ----------------------------------------
$cfgText = Get-Content -Raw (Join-Path $root 'src\config.h')
if ($cfgText -notmatch '#define\s+FW_VERSION\s+"([^"]+)"') {
  throw 'Nie znalazlem #define FW_VERSION w src\config.h'
}
$ver = $Matches[1]
Write-Host "Stacja Pogody $ver - budowanie plikow do wgrania" -ForegroundColor Cyan

$dist = Join-Path $root 'dist'
New-Item -ItemType Directory -Force -Path $dist | Out-Null
Get-ChildItem $dist -Filter '*.bin' -ErrorAction SilentlyContinue | Remove-Item -Force

$targets = @(
  [pscustomobject]@{ Env = 'esp32s3-master';        Suffix = 'master-16mb';   Flash = '16MB'; Chip = 'esp32s3'; Board = '';        Core3 = $false; Note = 'ESP #1 - pulpit, Wi-Fi, MQTT, OTA (16 MB, PSRAM octal)' }
  [pscustomobject]@{ Env = 'esp32s3-node-8mb-quad'; Suffix = 'node-8mb-quad'; Flash = '8MB';  Chip = 'esp32s3'; Board = 's3n8r4';  Core3 = $false; Note = 'Wezel - dowolny adres na magistrali (8 MB, PSRAM quad, N8R2 / N8R4)' }
  [pscustomobject]@{ Env = 'esp32c3-node';          Suffix = 'node-c3-4mb';   Flash = '4MB';  Chip = 'esp32c3'; Board = 'c3mini';  Core3 = $false; Note = 'Wezel - dowolny adres na magistrali (ESP32-C3 SuperMini, 4 MB, bez PSRAM)' }
)

# Adresowane binarki węzła C3: gotowe pliki dla kolejnych modułów 3..8.
# Uniwersalny esp32c3-node to adres 2 (w $targets). Każde środowisko ma
# wkompilowany inny STACJA_NODE_ADDR, więc świeże moduły nie kolidują.
$addrs = 3..8
$addrEnvs = @($addrs | ForEach-Object { 'esp32c3-node-addr' + $_ })

# Środowiska na Arduino core 3.x (pioarduino) budujemy w krótszym katalogu
# roboczym - inaczej ścieżki w esp32-arduino-libs (Matter) przekraczają limit
# Windows MAX_PATH (260 znaków). Core 2 zostaje w domyślnym .platformio.
$core3Dir = 'C:\Users\kruse\pio'

$targetsCore3 = @(
  [pscustomobject]@{ Env = 'esp32c3-node-v3'; Suffix = 'node-c3-v3-4mb'; Flash = '4MB'; Chip = 'esp32c3'; Board = 'c3mini-v3'; Core3 = $true; Note = 'Wezel - ESP32-C3 SuperMini na Arduino core 3.x (4 MB, bez PSRAM)' }
  [pscustomobject]@{ Env = 'esp32c6-node';    Suffix = 'node-c6-4mb';     Flash = '4MB'; Chip = 'esp32c6'; Board = 'c6mini';     Core3 = $true; Note = 'Wezel - ESP32-C6 na Arduino core 3.x (4 MB, wiecej GPIO)' }
)

$addrEnvsCore3 = @($addrs | ForEach-Object { 'esp32c6-node-addr' + $_ })

# --- kompilacja -------------------------------------------------------------
if (-not $SkipBuild) {
  # Grupa 1: rdzen Arduino 2.0.17 (oficjalna platforma espressif32),
  # domyslny katalog roboczy .platformio.
  $core2Envs = @(($targets | ForEach-Object { $_.Env }) + $addrEnvs)
  Write-Host ("Kompilacja (core 2): " + ($core2Envs -join ', '))
  $pioArgs = @('run')
  foreach ($e in $core2Envs) { $pioArgs += @('-e', $e) }
  & pio @pioArgs
  if ($LASTEXITCODE -ne 0) { throw "Kompilacja core 2 nie powiodla sie (kod $LASTEXITCODE)" }

  # Grupa 2: rdzen Arduino 3.x (pioarduino) - wymaga PlatformIO Core >= 6.2.0
  # i krotszego katalogu roboczego (Windows MAX_PATH).
  $core3Envs = @(($targetsCore3 | ForEach-Object { $_.Env }) + $addrEnvsCore3)
  Write-Host ("Kompilacja (core 3): " + ($core3Envs -join ', '))
  $env:PLATFORMIO_CORE_DIR = $core3Dir
  $pioArgs = @('run')
  foreach ($e in $core3Envs) { $pioArgs += @('-e', $e) }
  & pio @pioArgs
  $core3Exit = $LASTEXITCODE
  Remove-Item Env:\PLATFORMIO_CORE_DIR -ErrorAction SilentlyContinue
  if ($core3Exit -ne 0) { throw "Kompilacja core 3 nie powiodla sie (kod $core3Exit)" }

  # Grupa 3: zewnetrzna kamera ESP32-CAM (osobny projekt w esp32cam\).
  Write-Host "Kompilacja (kamera ESP32-CAM)"
  Push-Location (Join-Path $root 'esp32cam')
  & pio run
  $camExit = $LASTEXITCODE
  Pop-Location
  if ($camExit -ne 0) { throw "Kompilacja kamery nie powiodla sie (kod $camExit)" }
}

# --- sklejanie plikow -------------------------------------------------------
# esptool jest modulem Pythona w wirtualnym srodowisku PlatformIO (penv).
# Core 3 ma wlasne penv w krotszym katalogu roboczym.
function Get-EsptoolPython([bool]$core3) {
  $py = if ($core3) { Join-Path $core3Dir 'penv\Scripts\python.exe' }
        else        { Join-Path $env:USERPROFILE '.platformio\penv\Scripts\python.exe' }
  if (-not (Test-Path $py)) { $py = 'python' }
  return $py
}

$made = @()
foreach ($t in ($targets + $targetsCore3)) {
  $py = Get-EsptoolPython $t.Core3
  $build = Join-Path $root (".pio\build\" + $t.Env)
  $boot = Join-Path $build 'bootloader.bin'
  $part = Join-Path $build 'partitions.bin'
  $app  = Join-Path $build 'firmware.bin'
  foreach ($f in @($boot, $part, $app)) {
    if (-not (Test-Path $f)) { throw "Brak $f - uruchom skrypt bez -SkipBuild" }
  }

  # Jeden plik od 0x0: bootloader + tablica partycji + firmware.
  $merged = Join-Path $dist ("StacjaPogody-$ver-" + $t.Suffix + '.bin')
  & $py -m esptool --chip $t.Chip merge_bin --flash_mode dio --flash_freq 80m --flash_size $t.Flash `
      -o $merged 0x0 $boot 0x8000 $part 0x10000 $app | Out-Null
  if ($LASTEXITCODE -ne 0) { throw ("merge_bin nie powiodl sie dla " + $t.Env) }

  # Sam firmware - do wgrania przez OTA (zakladka "Aktualizacja") lub espota.
  Copy-Item $app (Join-Path $dist ("StacjaPogody-$ver-" + $t.Suffix + '-ota.bin')) -Force
  $made += $t
}

# --- zewnetrzna kamera ESP32-CAM (4 MB) --------------------------------------
# Kamerę budujemy w osobnym projekcie (esp32cam\), więc sklejamy jej pliki
# recznie. AI-Thinker ESP32-CAM: flash DIO 40 MHz, 4 MB.
$camBuild = Join-Path $root 'esp32cam\.pio\build\esp32cam'
$camBoot = Join-Path $camBuild 'bootloader.bin'
$camPart = Join-Path $camBuild 'partitions.bin'
$camApp  = Join-Path $camBuild 'firmware.bin'
foreach ($f in @($camBoot, $camPart, $camApp)) {
  if (-not (Test-Path $f)) { throw "Brak $f - zbuduj kamere: pio run w katalogu esp32cam\" }
}
$py = Get-EsptoolPython $false
$camMerged = Join-Path $dist ("StacjaPogody-$ver-cam-4mb.bin")
& $py -m esptool --chip esp32 merge_bin --flash_mode dio --flash_freq 40m --flash_size 4MB `
    -o $camMerged 0x0 $camBoot 0x8000 $camPart 0x10000 $camApp | Out-Null
if ($LASTEXITCODE -ne 0) { throw "merge_bin nie powiodl sie dla kamery" }
Copy-Item $camApp (Join-Path $dist ("StacjaPogody-$ver-cam-4mb-ota.bin")) -Force

# --- adresowane binarki węzła C3 (moduły 3..8) ------------------------------
# Każda binarka to ten sam firmware węzła z innym STACJA_NODE_ADDR. Wgrywa się
# je od 0x0 (bootloader + partycje + firmware) na świeży moduł ESP32-C3.
$madeAddrs = @()
$py = Get-EsptoolPython $false
foreach ($n in $addrs) {
  $env = 'esp32c3-node-addr' + $n
  $build = Join-Path $root ('.pio\build\' + $env)
  $boot = Join-Path $build 'bootloader.bin'
  $part = Join-Path $build 'partitions.bin'
  $app  = Join-Path $build 'firmware.bin'
  foreach ($f in @($boot, $part, $app)) {
    if (-not (Test-Path $f)) { throw "Brak $f - uruchom skrypt bez -SkipBuild" }
  }
  $merged = Join-Path $dist ("StacjaPogody-$ver-node-c3-4mb-addr" + $n + '.bin')
  & $py -m esptool --chip esp32c3 merge_bin --flash_mode dio --flash_freq 80m --flash_size 4MB `
      -o $merged 0x0 $boot 0x8000 $part 0x10000 $app | Out-Null
  if ($LASTEXITCODE -ne 0) { throw ("merge_bin nie powiodl sie dla " + $env) }
  $madeAddrs += $n
}

# --- adresowane binarki węzła C6 (moduły 3..8) ------------------------------
# Ta sama zasada co dla C3, ale budowane na Arduino core 3.x (pioarduino).
$madeAddrsC6 = @()
$py = Get-EsptoolPython $true
foreach ($n in $addrs) {
  $env = 'esp32c6-node-addr' + $n
  $build = Join-Path $root ('.pio\build\' + $env)
  $boot = Join-Path $build 'bootloader.bin'
  $part = Join-Path $build 'partitions.bin'
  $app  = Join-Path $build 'firmware.bin'
  foreach ($f in @($boot, $part, $app)) {
    if (-not (Test-Path $f)) { throw "Brak $f - uruchom skrypt bez -SkipBuild" }
  }
  $merged = Join-Path $dist ("StacjaPogody-$ver-node-c6-4mb-addr" + $n + '.bin')
  & $py -m esptool --chip esp32c6 merge_bin --flash_mode dio --flash_freq 80m --flash_size 4MB `
      -o $merged 0x0 $boot 0x8000 $part 0x10000 $app | Out-Null
  if ($LASTEXITCODE -ne 0) { throw ("merge_bin nie powiodl sie dla " + $env) }
  $madeAddrsC6 += $n
}

# --- obrazki wezla dla strony www (opcjonalnie) -----------------------------
# Stacja wydaje je przez /api/boards i linkuje z zakladki "ESP i magistrala RS485",
# dzieki czemu druga plytke C3 mozna wgrac bez szukania pliku w repozytorium.
# Publikujemy TYLKO wezel C3 SuperMini. Pozostale wezly (S3, C6, warianty
# z adresem) instaluje sie recznie z plikow w dist\.
$nodeDir = Join-Path $root 'data\node'
if ($PublishNode) {
  New-Item -ItemType Directory -Force -Path $nodeDir | Out-Null

  # Wezly C3 nie maja Wi-Fi ani OTA, wiec kopiujemy tylko pelny plik od 0x0
  # (bootloader + partycje + firmware) - uniwersalny c3mini (core 2) oraz
  # c3mini-v3 (core 3).
  foreach ($t in $made) {
    if ($t.Board -notin @('c3mini', 'c3mini-v3')) { continue }
    Copy-Item (Join-Path $dist ("StacjaPogody-$ver-" + $t.Suffix + '.bin')) `
              (Join-Path $nodeDir ($t.Board + '.bin')) -Force
  }
}

# Zawsze usuwamy stare binarki (wszystko poza aktualnymi plikami C3), zeby
# obraz www byl zawsze aktualny i bez starych plikow - niezaleznie od flagi
# -PublishNode (ta flaga jedynie doklada swieze pliki C3).
if (Test-Path $nodeDir) {
  Get-ChildItem $nodeDir -Filter '*.bin' | ForEach-Object {
    if ($_.Name -notmatch '^c3mini(-v3)?\.bin$') {
      Remove-Item $_.FullName -Force
    }
  }
}

if ($PublishNode) {
  $sum = (Get-ChildItem $nodeDir -Filter '*.bin' | Measure-Object -Property Length -Sum).Sum
  Write-Host ("Obrazki wezla C3 w data\node ({0:N1} MB) - pozostale wezly instaluj recznie z dist\" -f ($sum / 1MB)) -ForegroundColor Cyan
}

# --- strony www (LittleFS) --------------------------------------------------
# Obraz zalezy od rozmiaru partycji systemu plikow, wiec dla plytek 8 MB
# (1,88 MB) trzeba go zbudowac osobno. Katalogu data\node nie ma w obrazie
# wezla - binarki wezla (4,6 MB) sie tam nie mieszcza, a wydaje je tylko
# master (przez /api/boards). Srodowiska wezla same podmieniaja katalog danych
# (tools/stage_node_fs.py, patrz extra_scripts w platformio.ini), a majster
# pakuje pelne data\ (z binarkami) - dlatego obraz 16 MB ma swieze pliki
# zaraz po -PublishNode.
$mkfs = Join-Path $env:USERPROFILE '.platformio\packages\tool-mklittlefs\mklittlefs.exe'
if (-not (Test-Path $mkfs)) { throw "Brak mklittlefs w $mkfs" }

# Rozmiar partycji systemu plikow z tablicy partycji (wiersz "spiffs").
function Get-FsSize([string]$csv) {
  foreach ($line in Get-Content $csv) {
    if ($line -match '^\s*\S+\s*,\s*data\s*,\s*(spiffs|littlefs)\s*,\s*\S+\s*,\s*(0x[0-9a-fA-F]+)') {
      return [Convert]::ToInt64($Matches[2], 16)
    }
  }
  throw "Nie znalazlem partycji LittleFS w $csv"
}

$fsJobs = @(
  [pscustomobject]@{ Env = 'esp32s3-master';        Partitions = 'partitions.csv';     WithNode = $true },
  [pscustomobject]@{ Env = 'esp32s3-node-8mb-quad'; Partitions = 'partitions-8mb.csv'; WithNode = $false }
)

$fsImgs = @()
if (-not $SkipWww) {
  foreach ($j in $fsJobs) {
    Write-Host ("Budowanie obrazu LittleFS (strona www) - " + $j.Env)
    & pio run -e $j.Env -t buildfs
    if ($LASTEXITCODE -ne 0) { throw ("Nie udalo sie zbudowac obrazu LittleFS (" + $j.Env + ")") }

    $target = Join-Path $root ('.pio\build\' + $j.Env + '\littlefs.bin')
    $size = Get-FsSize (Join-Path $root $j.Partitions)
    if (-not (Test-Path $target)) { throw ("Brak obrazu LittleFS " + $target) }
    if ((Get-Item $target).Length -ne $size) {
      throw ("Obraz LittleFS " + $j.Env + " nie ma rozmiaru partycji (" + $size + " B)")
    }

    # PlatformIO konczy z kodem 0 takze wtedy, gdy pliki sie nie zmiescily
    # ("lfs_write error(-28)") i obraz wychodzi z plikami 0 B - sprawdzamy go.
    $listing = & $mkfs -l -s $size -p 256 -b 4096 $target
    if (-not ($listing -match '/index\.html')) { throw ("Obraz " + $j.Env + " nie zawiera /index.html") }
    if ($listing -match '(?m)^0\t') { throw ("Obraz " + $j.Env + " zawiera plik o rozmiarze 0 B") }
    if ($j.WithNode) {
      if (-not ($listing -match '/node/')) { throw ("Obraz " + $j.Env + " nie zawiera binarek wezla z data\node") }
    } elseif ($listing -match '/node/') {
      throw ("Obraz " + $j.Env + " zawiera binarki wezla - nie mieszcza sie na plytce 8 MB")
    }
    $fsImgs += $target
  }
} else {
  $fsImgs = @(
    (Join-Path $root '.pio\build\esp32s3-master\littlefs.bin'),
    (Join-Path $root '.pio\build\esp32s3-node-8mb-quad\littlefs.bin')
  )
}

$fsOut = @{ 'esp32s3-master' = 'www-16mb'; 'esp32s3-node-8mb-quad' = 'www-8mb' }
$found = 0
foreach ($f in $fsImgs) {
  if (-not (Test-Path $f)) { continue }
  $envName = Split-Path -Leaf (Split-Path -Parent $f)
  $name = "StacjaPogody-$ver-" + $fsOut[$envName] + '.bin'
  Copy-Item $f (Join-Path $dist $name) -Force
  $found++
}
if ($found -eq 0) {
  Write-Host 'UWAGA: nie ma obrazow LittleFS - uruchom bez -SkipWww' -ForegroundColor Yellow
}

# --- podsumowanie ----------------------------------------------------------
$rows = Get-ChildItem $dist -Filter '*.bin' | Sort-Object Name | ForEach-Object {
  [pscustomobject]@{
    Plik    = $_.Name
    Rozmiar = ('{0:N0} kB' -f ($_.Length / 1KB))
    MD5     = (Get-FileHash $_.FullName -Algorithm MD5).Hash.ToLower()
  }
}
$rows | ForEach-Object { '{0}  {1}' -f $_.MD5, $_.Plik } |
  Set-Content -Path (Join-Path $dist 'SUMY-MD5.txt') -Encoding ASCII

$rows | Format-Table -AutoSize
Write-Host 'Ktory plik wybrac:' -ForegroundColor Cyan
foreach ($t in $made) {
  Write-Host ("  StacjaPogody-$ver-" + $t.Suffix + ".bin  -> " + $t.Note)
}
foreach ($n in $madeAddrs) {
  Write-Host ("  StacjaPogody-$ver-node-c3-4mb-addr" + $n + ".bin -> Wezel C3 z adresem fabrycznym " + $n + " (kolejny modul na magistrali)")
}
foreach ($n in $madeAddrsC6) {
  Write-Host ("  StacjaPogody-$ver-node-c6-4mb-addr" + $n + ".bin -> Wezel C6 z adresem fabrycznym " + $n + " (kolejny modul na magistrali)")
}
Write-Host "  StacjaPogody-$ver-cam-4mb.bin -> Kamera ESP32-CAM (AI-Thinker, 4 MB)"
Write-Host "  StacjaPogody-$ver-cam-4mb-ota.bin -> sam firmware kamery (OTA, port 3232)"
Write-Host 'Pliki "*.bin" od 0x0 wgrywamy tylko na nowa plytke albo po czyszczeniu' -ForegroundColor Yellow
Write-Host '  pamieci - kasuja ustawienia (Wi-Fi, MQTT, kalibracje)! Do aktualizacji' -ForegroundColor Yellow
Write-Host '  dzialajacej stacji uzyj pliku "-ota.bin" (strona www -> Aktualizacja).' -ForegroundColor Yellow
Write-Host 'Wgrywanie od 0x0 (port COM dobierz wedlug menedzera urzadzen):'
Write-Host "  esptool --chip esp32s3 --port COM9 write_flash 0x0 StacjaPogody-$ver-master-16mb.bin" -ForegroundColor DarkGray
Write-Host "  esptool --chip esp32c3 --port COM9 write_flash 0x0 StacjaPogody-$ver-node-c3-4mb.bin" -ForegroundColor DarkGray
Write-Host "  esptool --chip esp32c3 --port COM10 write_flash 0x0 StacjaPogody-$ver-node-c3-4mb-addr3.bin" -ForegroundColor DarkGray
Write-Host "  esptool --chip esp32 --port COM11 write_flash 0x0 StacjaPogody-$ver-cam-4mb.bin" -ForegroundColor DarkGray
Write-Host "  Pliki '-ota.bin' wgrywa sie ze strony www (zakladka Aktualizacja) - bez kabla."
Write-Host "  Strona www (0x610000): StacjaPogody-$ver-www-16mb.bin / -www-8mb.bin"
Write-Host "Pliki gotowe w: $dist" -ForegroundColor Green
