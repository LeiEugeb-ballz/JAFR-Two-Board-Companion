param(
    [switch]$Flash,
    [string]$Port = "COM18"
)

$ErrorActionPreference = "Stop"
$projectRoot = $PSScriptRoot
$idfExport = "C:\Espressif\frameworks\esp-idf\export.ps1"
$buildDirectory = "build_s31_release_j"
$stageDrive = "J:"

if (-not (Test-Path -LiteralPath $idfExport)) {
    throw "ESP-IDF export script not found at $idfExport"
}

$substLines = @(& subst)
$stageMapping = $substLines | Where-Object { $_ -match '^J:\\:' }
if ($stageMapping) {
    if ($stageMapping -notmatch [regex]::Escape($projectRoot)) {
        throw "$stageDrive is already mapped to a different path"
    }
} else {
    & subst $stageDrive $projectRoot
    if ($LASTEXITCODE -ne 0) {
        throw "Could not map $stageDrive to the project"
    }
}

Push-Location "$stageDrive\"
try {
    . $idfExport
    $idfArgs = @("--preview", "--no-configdep", "-B", $buildDirectory)
    if ($Flash) {
        $idfArgs += @("-p", $Port, "flash")
    } else {
        $idfArgs += "build"
    }
    & idf.py @idfArgs
    if ($LASTEXITCODE -ne 0) {
        throw "ESP-IDF failed with exit code $LASTEXITCODE"
    }
} finally {
    Pop-Location
}
