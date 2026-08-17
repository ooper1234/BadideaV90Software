$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
# Always run the canonical executable produced from the CURRENT source tree.
# Historical *-fixed.exe files are intentionally ignored: they are snapshots
# from earlier debugging rounds and can silently mask newer DSP fixes.
$exe = Join-Path $root 'dist\v92isp-windows.exe'
$config = Join-Path $root 'config\v92isp-windows.env'

function Test-Admin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    $p = New-Object Security.Principal.WindowsPrincipal($id)
    return $p.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}
if (-not (Test-Admin)) {
    Start-Process powershell.exe -Verb RunAs -ArgumentList '-NoExit','-NoProfile','-ExecutionPolicy','Bypass','-File',("`"$PSCommandPath`"")
    exit
}
if (-not (Test-Path $exe)) { throw "Missing $exe. Run windows\Build-Windows.ps1 first." }
if (-not (Test-Path (Join-Path $root 'dist\wintun.dll'))) { throw 'dist\wintun.dll missing. Run windows\Build-Windows.ps1.' }
if (-not (Test-Path (Join-Path $root 'dist\WinDivert.dll'))) { throw 'dist\WinDivert.dll missing. Run windows\Build-Windows.ps1.' }
if (-not (Test-Path (Join-Path $root 'dist\WinDivert64.sys'))) { throw 'dist\WinDivert64.sys missing. Run windows\Build-Windows.ps1.' }

# Avoid a confusing bind failure if another softphone owns the actual ports
# selected by Setup-Windows.ps1 (which may have chosen alternates).
function Get-ConfigPort([string]$key, [int]$default) {
    $line = Get-Content $config | Where-Object { $_ -match ('^' + [regex]::Escape($key) + '=') } | Select-Object -First 1
    if (-not $line) { return $default }
    $v = 0
    if ([int]::TryParse($line.Substring($key.Length + 1).Trim(), [ref]$v)) { return $v }
    return $default
}
$sipPort = Get-ConfigPort 'SIP_PORT' 5060
$rtpPort = Get-ConfigPort 'RTP_PORT' 40000
$busy = Get-NetUDPEndpoint -ErrorAction SilentlyContinue | Where-Object { $_.LocalPort -in $sipPort,$rtpPort }
foreach ($ep in $busy) {
    $proc = Get-Process -Id $ep.OwningProcess -ErrorAction SilentlyContinue
    if ($proc -and $proc.Path -ne $exe) {
        throw "UDP port $($ep.LocalPort) is already used by $($proc.ProcessName) (PID $($ep.OwningProcess)). Close the conflicting SIP/audio app first."
    }
}

Set-Location $root
Write-Host "[START] Executable: $exe"
$exeInfo = Get-Item -LiteralPath $exe
Write-Host ("[START] Executable modified: " + $exeInfo.LastWriteTime.ToString("yyyy-MM-dd HH:mm:ss"))
$logDir = Join-Path $root 'logs'
if (-not (Test-Path $logDir)) { New-Item -ItemType Directory -Path $logDir -Force | Out-Null }
$logFile = Join-Path $logDir 'v92isp-live.log'
Write-Host "[LOG] Streaming server log to $logFile"
& $exe --config $config --debug 2>&1 | Tee-Object -FilePath $logFile
