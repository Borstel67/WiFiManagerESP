# Jüngstes ELF nehmen
$elf = Get-ChildItem $env:TEMP -Recurse -Filter *.elf |
       Sort-Object LastWriteTime -Descending |
       Select-Object -First 1

# addr2line suchen (passt für ESP32 / ESP32-S3)
$addr = Get-ChildItem "$env:LOCALAPPDATA\Arduino15\packages\esp32\tools" -Recurse -Filter "*addr2line*.exe" |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1

$addr.FullName
$elf.FullName

# Backtrace auflösen (Pfad in Anführungszeichen!)
& "$($addr.FullName)" -pfiaC -e "$($elf.FullName)" 0x4008d32c 0x4008d2f1 0x40093735 0x4008dca2 0x4011c062 0x40108262 0x4011c82b 0x4011c889 0x401071aa 0x400e25ec 0x400e1e41 0x4019741d 0x400ede49 0x400edfc5 0x400d3e7f 0x400d6c06 0x400d3641 0x400f5684 0x4008e141