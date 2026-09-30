# 0) Pfade relativ zum Skriptordner auflösen
$csvPath = Join-Path $PSScriptRoot "..\partitions.csv"
$csvPath = (Resolve-Path $csvPath).Path
$binPath = Join-Path $PSScriptRoot "partitions.bin"

if (-not (Test-Path $csvPath)) {
    throw "partitions.csv nicht gefunden: $csvPath"
}

# 1) Tool finden
$gen = Get-ChildItem -Path "$env:LOCALAPPDATA\Arduino15\packages\esp32" `
         -Filter gen_esp32part.py -Recurse -ErrorAction SilentlyContinue |
       Select-Object -First 1 -ExpandProperty FullName
if (-not $gen) { throw "gen_esp32part.py nicht gefunden (ESP32 Boards-Paket installieren)" }

# 2) partitions.bin erzeugen
python "$gen" "$csvPath" "$binPath"

# 3) Flashen (Partitionstabelle liegt bei 0x8000)
$esptool = "$env:LOCALAPPDATA\Arduino15\packages\esp32\tools\esptool_py\5.2.0\esptool.exe"
& $esptool --chip esp32 --port COM9 --baud 115200 --filename $binPath write-flash