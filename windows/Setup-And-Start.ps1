$ErrorActionPreference = 'Stop'
& (Join-Path $PSScriptRoot 'Build-Windows.ps1')
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& (Join-Path $PSScriptRoot 'Setup-Windows.ps1')
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& (Join-Path $PSScriptRoot 'Start-V92ISP.ps1')
