# .\LittleFS-OTA.ps1 -Ip 192.168.4.1         # AP/Portal
# .\LittleFS-OTA.ps1 -Ip 192.168.1.50 -Port 3232
# .\LittleFS-OTA.ps1 -Ip 192.168.1.50 -Password "meinOTApass"

param(
    [Parameter(Mandatory = $true)] [string]$Ip,       # Ziel-IP des ESP
    [int]$Port = 3232,                                # ArduinoOTA-Port
    [string]$Password = ""                            # OTA-Passwort, falls gesetzt
)

$ErrorActionPreference = "Stop"

# Pfade / Konstanten
$root   = $PSScriptRoot
$src    = Join-Path $root "data"
$out    = Join-Path $root "littlefs.bin"
$siz    = "0x60000"      # Größe aus partitions.csv
$tool   = "$env:LOCALAPPDATA\Arduino15\packages\esp32\tools\mklittlefs\4.0.2-db0513a\mklittlefs.exe"

# espota.py dynamisch aus aktueller esp32-HW-Version auflösen
$hwRoot = Join-Path $env:LOCALAPPDATA "Arduino15\packages\esp32\hardware\esp32"
$hwVer  = Get-ChildItem $hwRoot -Directory | Sort-Object Name -Descending | Select-Object -First 1
if (-not $hwVer) { throw "Keine ESP32-Hardwareversion unter $hwRoot gefunden." }
$espota = Join-Path $hwVer.FullName "tools\espota.py"
if (-not (Test-Path $espota)) { throw "espota.py nicht gefunden: $espota" }

# Checks
if (-not (Test-Path $src)) { throw "data-Verzeichnis fehlt: $src" }
if (-not (Get-ChildItem -File -Recurse $src)) { throw "data ist leer: $src" }
if (-not (Test-Path $tool)) { throw "mklittlefs fehlt: $tool" }

# LittleFS-Image bauen
& $tool -c $src -b 4096 -p 256 -s $siz $out

# OTA-Upload des LittleFS-Images
$authArg = @()
if ($Password.Length) { $authArg += @("--auth", $Password) }

python $espota --spiffs -i $Ip -p $Port @authArg -f $out