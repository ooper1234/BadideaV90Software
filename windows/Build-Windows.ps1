$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path

function To-MsysPath([string]$p) {
    $p = [IO.Path]::GetFullPath($p)
    if ($p -match '^([A-Za-z]):\\(.*)$') {
        return '/' + $Matches[1].ToLower() + '/' + ($Matches[2] -replace '\\','/')
    }
    throw "Cannot convert Windows path to MSYS2 path: $p"
}

$bash = 'C:\msys64\usr\bin\bash.exe'
if (-not (Test-Path $bash)) {
    Write-Host '[SETUP] MSYS2 not found. Installing it with winget...'
    $winget = Get-Command winget.exe -ErrorAction SilentlyContinue
    if (-not $winget) { throw 'winget is required for automatic MSYS2 installation. Install MSYS2 manually to C:\msys64 and run this script again.' }
    & winget install --id MSYS2.MSYS2 -e --silent --accept-package-agreements --accept-source-agreements
    if ($LASTEXITCODE -ne 0 -and -not (Test-Path $bash)) { throw "MSYS2 install failed (winget exit $LASTEXITCODE)" }
}


# Fetch WinDivert 2.2.2-A before CMake so the Windows build can compile the
# built-in user-mode NAT fallback. This is used only when WinNAT/ICS cannot be
# enabled (which occurs on some current Windows 11 builds/adapters).
$wdRoot = Join-Path $root 'third_party\windivert'
$wdHeader = Join-Path $wdRoot 'include\windivert.h'
$wdDllStaged = Join-Path $wdRoot 'x64\WinDivert.dll'
$wdSysStaged = Join-Path $wdRoot 'x64\WinDivert64.sys'
if (-not (Test-Path $wdHeader) -or -not (Test-Path $wdDllStaged) -or -not (Test-Path $wdSysStaged)) {
    Write-Host '[SETUP] Downloading official WinDivert 2.2.2-A NAT runtime...'
    $wdZip = Join-Path $env:TEMP 'WinDivert-2.2.2-A.zip'
    $wdTmp = Join-Path $env:TEMP 'v92isp-windivert'
    Invoke-WebRequest -UseBasicParsing 'https://www.reqrypt.org/download/WinDivert-2.2.2-A.zip' -OutFile $wdZip
    Remove-Item $wdTmp -Recurse -Force -ErrorAction SilentlyContinue
    Expand-Archive $wdZip -DestinationPath $wdTmp -Force
    $hdr = Get-ChildItem $wdTmp -Recurse -Filter windivert.h | Select-Object -First 1
    $dll = Get-ChildItem $wdTmp -Recurse -Filter WinDivert.dll | Where-Object { $_.FullName -match '[\\/]x64[\\/]' } | Select-Object -First 1
    $sys = Get-ChildItem $wdTmp -Recurse -Filter WinDivert64.sys | Where-Object { $_.FullName -match '[\\/]x64[\\/]' } | Select-Object -First 1
    if (-not $hdr -or -not $dll -or -not $sys) { throw 'Official WinDivert archive did not contain the expected x64 runtime/header files.' }
    New-Item -ItemType Directory -Force (Join-Path $wdRoot 'include') | Out-Null
    New-Item -ItemType Directory -Force (Join-Path $wdRoot 'x64') | Out-Null
    Copy-Item $hdr.FullName $wdHeader -Force
    Copy-Item $dll.FullName $wdDllStaged -Force
    Copy-Item $sys.FullName $wdSysStaged -Force
}

# The official pre-built driver is signed. Re-check it even when the cache was
# already present. Reject an actually unsigned/tampered file; certificate-chain
# lookup failures are warnings so an offline machine is not blocked needlessly.
$sig = Get-AuthenticodeSignature $wdSysStaged
if ($sig.Status -eq 'Valid') {
    Write-Host '[SETUP] WinDivert signed driver verified.' -ForegroundColor Green
} elseif ($sig.Status -eq 'NotSigned' -or $sig.Status -eq 'HashMismatch') {
    throw "WinDivert64.sys signature validation failed: $($sig.Status) $($sig.StatusMessage)"
} else {
    Write-Host "[SETUP] WinDivert signature status could not be fully verified: $($sig.Status) $($sig.StatusMessage)" -ForegroundColor Yellow
}

# Recover cleanly from an MSYS2/pacman database lock left by an interrupted
# previous build. Never remove the lock while pacman.exe is actually running.
$pacmanLock = 'C:\msys64\var\lib\pacman\db.lck'
if (Test-Path $pacmanLock) {
    $pacmanProc = Get-Process pacman -ErrorAction SilentlyContinue
    if ($pacmanProc) {
        Write-Host '[MSYS2] Another pacman process is active; waiting for it to finish...'
        $deadline = (Get-Date).AddSeconds(60)
        do {
            Start-Sleep -Seconds 2
            $pacmanProc = Get-Process pacman -ErrorAction SilentlyContinue
        } while ($pacmanProc -and (Get-Date) -lt $deadline)
    }
    if (Test-Path $pacmanLock) {
        if (Get-Process pacman -ErrorAction SilentlyContinue) {
            throw 'pacman.exe is still running and owns the MSYS2 database lock. Close the other MSYS2/package-manager window and retry.'
        }
        Write-Host '[MSYS2] Removing stale pacman database lock.' -ForegroundColor Yellow
        Remove-Item -LiteralPath $pacmanLock -Force
    }
}

$msysRoot = To-MsysPath $root
Write-Host "[BUILD] $root"
$oldMs = $env:MSYSTEM
$oldChere = $env:CHERE_INVOKING
$env:MSYSTEM = 'UCRT64'
$env:CHERE_INVOKING = '1'
try {
    & $bash -lc "cd '$msysRoot'; bash windows/build-msys2.sh"
} finally {
    $env:MSYSTEM = $oldMs
    $env:CHERE_INVOKING = $oldChere
}
if ($LASTEXITCODE -ne 0) { throw "Windows build failed (exit $LASTEXITCODE)" }

# Fetch the official signed Wintun 0.14.1 DLL. The upstream page publishes
# SHA256 07c256...ef51 for this archive.
$dist = Join-Path $root 'dist'
New-Item -ItemType Directory -Force $dist | Out-Null
$targetWdDll = Join-Path $dist 'WinDivert.dll'
$targetWdSys = Join-Path $dist 'WinDivert64.sys'
if (-not (Test-Path $targetWdDll)) {
    Copy-Item $wdDllStaged $targetWdDll -Force
} else {
    try { Copy-Item $wdDllStaged $targetWdDll -Force } catch {}
}
if (-not (Test-Path $targetWdSys)) {
    Copy-Item $wdSysStaged $targetWdSys -Force
} else {
    try { Copy-Item $wdSysStaged $targetWdSys -Force } catch {}
}
$wintunDll = Join-Path $dist 'wintun.dll'
if (-not (Test-Path $wintunDll)) {
    Write-Host '[SETUP] Downloading official Wintun 0.14.1...'
    $zip = Join-Path $env:TEMP 'wintun-0.14.1.zip'
    $dir = Join-Path $env:TEMP 'v92isp-wintun'
    Invoke-WebRequest -UseBasicParsing 'https://www.wintun.net/builds/wintun-0.14.1.zip' -OutFile $zip
    $hash = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLower()
    if ($hash -ne '07c256185d6ee3652e09fa55c0b673e2624b565e02c4b9091c79ca7d2f24ef51') {
        throw "Wintun SHA256 mismatch: $hash"
    }
    Remove-Item $dir -Recurse -Force -ErrorAction SilentlyContinue
    Expand-Archive $zip -DestinationPath $dir -Force
    $dll = Get-ChildItem $dir -Recurse -Filter wintun.dll | Where-Object { $_.FullName -match 'amd64' } | Select-Object -First 1
    if (-not $dll) { throw 'amd64 wintun.dll not found in official archive' }
    Copy-Item $dll.FullName $wintunDll -Force
}

Write-Host '[OK] Native Windows build is ready.' -ForegroundColor Green
Write-Host "     $dist\v92isp-windows.exe"
