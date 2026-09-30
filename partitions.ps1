# 1) Tool finden
$gen = Get-ChildItem -Path "$env:LOCALAPPDATA\Arduino15\packages\esp32" `
         -Filter gen_esp32part.py -Recurse -ErrorAction SilentlyContinue |
       Select-Object -First 1 -ExpandProperty FullName
if (-not $gen) { throw "gen_esp32part.py nicht gefunden (ESP32 Boards-Paket installieren)" }

# 2) partitions.bin erzeugen
python "$gen" partitions.csv partitions.bin

# 3) Flashen (Partitionstabelle liegt bei 0x8000)
$esptool = "$env:LOCALAPPDATA\Arduino15\packages\esp32\tools\esptool_py\5.2.0\esptool.exe"
& $esptool --chip esp32 --port COM4 --baud 115200 write_flash 0x8000 partitions.bin