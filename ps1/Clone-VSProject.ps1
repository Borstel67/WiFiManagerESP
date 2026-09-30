# Clone-VSProject.ps1
# .\Clone-VSProject.ps1 -NewName ComfoAirMQTTESP -CopyVmHeader -PatchSolution
param(
    [Parameter(Mandatory = $true)] [string]$NewName,
    [string]$SourceName = "WiFiManagerESP",
    [string]$Root       = (Get-Location).Path,
    [switch]$CopyVmHeader,      # kopiert __vm\.WiFiManagerESP.vsarduino.h
    [switch]$PatchSolution      # ersetzt Namen/Pfade in erster gefundenen .sln
)

function Copy-And-Replace($src, $dst, $old, $new) {
    if (-not (Test-Path $src)) { throw "Quelle fehlt: $src" }
    (Get-Content $src -Raw) -replace [regex]::Escape($old), $new | Set-Content $dst
    Write-Host "Erstellt: $dst"
}

# Dateien definieren
$projSrc     = Join-Path $Root "$SourceName.vcxproj"
$projDst     = Join-Path $Root "$NewName.vcxproj"
$filtersSrc  = Join-Path $Root "$SourceName.vcxproj.filters"
$filtersDst  = Join-Path $Root "$NewName.vcxproj.filters"
$vmSrc       = Join-Path $Root "__vm\.$SourceName.vsarduino.h"
$vmDst       = Join-Path $Root "__vm\.$NewName.vsarduino.h"

# Kopieren + Inhalte ersetzen
Copy-And-Replace $projSrc    $projDst    $SourceName $NewName
Copy-And-Replace $filtersSrc $filtersDst $SourceName $NewName

if ($CopyVmHeader -and (Test-Path $vmSrc)) {
    Copy-And-Replace $vmSrc $vmDst $SourceName $NewName
}

if ($PatchSolution) {
    $sln = Get-ChildItem $Root -Filter *.sln | Select-Object -First 1
    if ($sln) {
        $slnDst = $sln.FullName  # in-place
        (Get-Content $slnDst -Raw) -replace [regex]::Escape($SourceName), $NewName |
            Set-Content $slnDst
        Write-Host "Solution aktualisiert: $slnDst"
    } else {
        Write-Warning "Keine .sln gefunden."
    }
}

Write-Host "Fertig. Neues Projekt: $projDst"