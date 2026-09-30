$tool="$env:LOCALAPPDATA\Arduino15\packages\esp32\tools\mklittlefs\3.0.0-gnu\mklittlefs.exe"
$siz="0x160000"          # Größe aus deiner partitions.csv
$root=$PSScriptRoot
$out=Join-Path $root "littlefs.bin"
$src=Join-Path $root "data"
& $tool -c $src -b 4096 -p 256 -s $siz $out