$ErrorActionPreference = 'Stop'
$localPython = (Get-Command python).Source
$extraSite = & $localPython -c "import sysconfig; print(sysconfig.get_path('purelib'))"
$compat = Join-Path $PSScriptRoot '.pio\python-compat'
New-Item -ItemType Directory -Force -Path $compat | Out-Null
@"
import sys
from pathlib import Path

fallback = str(Path(r"$extraSite"))
if fallback not in sys.path:
    sys.path.append(fallback)
"@ | Set-Content -Encoding utf8 (Join-Path $compat 'sitecustomize.py')
$previous = $env:PYTHONPATH
try {
    $env:PYTHONPATH = if ($previous) { "$compat;$previous" } else { $compat }
    & pio run --project-dir $PSScriptRoot
    if ($LASTEXITCODE) { exit $LASTEXITCODE }
} finally {
    $env:PYTHONPATH = $previous
}
