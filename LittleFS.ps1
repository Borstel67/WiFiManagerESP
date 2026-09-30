# .\LittleFS.ps1 -Flash -Port COM4

param(
   [switch]$Flash,
   [string]$Port = "COM4"
)

$tool    = "$env:LOCALAPPDATA\Arduino15\packages\esp32\tools\mklittlefs\4.0.2-db0513a\mklittlefs.exe"
$esptool = "$env:LOCALAPPDATA\Arduino15\packages\esp32\tools\esptool_py\5.3.1\esptool.exe"
$siz     = "0x60000"   # Größe aus partitions.csv
$offset  = "0x3A0000"  # LittleFS-Offset aus partitions.csv
$root    = $PSScriptRoot
$out     = Join-Path $root "littlefs.bin"
$src     = Join-Path $root "data"

if (-not (Test-Path $src)) { throw "data-Verzeichnis fehlt: $src" }
if (-not (Get-ChildItem -File -Recurse $src)) { throw "data ist leer: $src" }

& $tool -c $src -b 4096 -p 256 -s $siz $out

& $esptool --chip esp32 --port $Port --baud 921600 write_flash $offset $out

 if ($Flash) {
    & $esptool --chip esp32 --port $Port --baud 921600 `
        write_flash $offset $out
}