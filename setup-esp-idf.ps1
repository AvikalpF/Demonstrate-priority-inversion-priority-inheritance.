////*  Run the command from E:\SHM_ESP32.
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass
.\setup-esp-idf.ps1
*////


$ErrorActionPreference = "Stop"

$idfPath = "C:\esp\v6.1\esp-idf"
$projectPath = "E:\SHM_ESP32"

if (-not (Test-Path "$idfPath\export.ps1")) {
    throw "ESP-IDF export.ps1 not found at $idfPath"
}

Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass
Set-Location $idfPath

. "$idfPath\export.ps1"

if (-not $env:IDF_PATH) {
    throw "ESP-IDF environment initialization failed."
}

Set-Location $projectPath
idf.py --version
Write-Host "ESP-IDF environment initialized."
Write-Host "Project: $projectPath"