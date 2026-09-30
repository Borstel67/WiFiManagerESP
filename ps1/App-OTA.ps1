# .\App-OTA.ps1 -Ip 192.168.1.50 -Bin .\build\firmware.bin
# .\App-OTA.ps1 -Ip 192.168.1.50 -Bin .\build\firmware.bin -Password "meinOTApass"
param(
    [Parameter(Mandatory = $true)] [string]$Ip,          # Ziel-IP des ESP
    [Parameter(Mandatory = $true)] [string]$Bin,         # Pfad zur Firmware-.bin
    [int]$Port = 3232,                                   # ArduinoOTA-Port
    [string]$Password = ""                               # OTA-Passwort, falls gesetzt
)

$ErrorActionPreference = "Stop"

# espota.py dynamisch aus aktueller esp32-HW-Version auflösen
$hwRoot = Join-Path $env:LOCALAPPDATA "Arduino15\packages\esp32\hardware\esp32"
$hwVer  = Get-ChildItem $hwRoot -Directory | Sort-Object Name -Descending | Select-Object -First 1
if (-not $hwVer) { throw "Keine ESP32-Hardwareversion unter $hwRoot gefunden." }
$espota = Join-Path $hwVer.FullName "tools\espota.py"
if (-not (Test-Path $espota)) { throw "espota.py nicht gefunden: $espota" }

if (-not (Test-Path $Bin)) { throw "Firmware-Bin nicht gefunden: $Bin" }

# OTA-Upload der Anwendung
$authArg = @()
if ($Password.Length) { $authArg += @("--auth", $Password) }

python $espota -i $Ip -p $Port @authArg -f $Bin