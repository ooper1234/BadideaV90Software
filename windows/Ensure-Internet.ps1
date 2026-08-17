param(
    [string]$PrivateAlias = 'v92isp',
    [string]$PrivatePrefix = '192.168.137.0/24',
    [string]$PrivateIP = '192.168.137.1'
)
$ErrorActionPreference = 'Stop'

function Test-Admin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    $p = New-Object Security.Principal.WindowsPrincipal($id)
    return $p.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}
if (-not (Test-Admin)) { throw 'Run this script as Administrator.' }

$private = Get-NetAdapter -Name $PrivateAlias -ErrorAction SilentlyContinue
if (-not $private) { throw "Private adapter '$PrivateAlias' was not found." }

# Keep private PPP adapter address deterministic
$wanted = Get-NetIPAddress -InterfaceAlias $PrivateAlias -AddressFamily IPv4 -ErrorAction SilentlyContinue |
    Where-Object { $_.IPAddress -eq $PrivateIP } | Select-Object -First 1
if (-not $wanted) {
    Get-NetIPAddress -InterfaceAlias $PrivateAlias -AddressFamily IPv4 -ErrorAction SilentlyContinue |
        Where-Object { $_.PrefixOrigin -ne 'WellKnown' } |
        Remove-NetIPAddress -Confirm:$false -ErrorAction SilentlyContinue
    $prefixLen = [int](($PrivatePrefix -split '/')[1])
    New-NetIPAddress -InterfaceAlias $PrivateAlias -IPAddress $PrivateIP -PrefixLength $prefixLen -AddressFamily IPv4 -ErrorAction SilentlyContinue | Out-Null
}

# Ensure ICS is DISABLED so Windows Firewall does not block incoming RTP audio on Ethernet
try {
    $mgr = New-Object -ComObject HNetCfg.HNetShare
    foreach ($conn in @($mgr.EnumEveryConnection)) {
        try {
            $cfg = $mgr.INetSharingConfigurationForINetConnection.Invoke($conn)
            if ($cfg.SharingEnabled) { $cfg.DisableSharing() }
        } catch {}
    }
} catch {}

Write-Host '[NAT] Internet sharing ready via built-in WinDivert NAT (ICS disabled for clean RTP audio)' -ForegroundColor Green
exit 0

Write-Host "[ICS] public : $publicAlias"
Write-Host "[ICS] private: $PrivateAlias"

function Ensure-PrivateIp {
    # Keep the PPP gateway address deterministic even when ICS changes adapter
    # configuration behind our back.
    $wanted = Get-NetIPAddress -InterfaceAlias $PrivateAlias -AddressFamily IPv4 -ErrorAction SilentlyContinue |
        Where-Object { $_.IPAddress -eq $PrivateIP } | Select-Object -First 1
    if (-not $wanted) {
        Get-NetIPAddress -InterfaceAlias $PrivateAlias -AddressFamily IPv4 -ErrorAction SilentlyContinue |
            Where-Object { $_.PrefixOrigin -ne 'WellKnown' } |
            Remove-NetIPAddress -Confirm:$false -ErrorAction SilentlyContinue
        $prefixLen = [int](($PrivatePrefix -split '/')[1])
        New-NetIPAddress -InterfaceAlias $PrivateAlias -IPAddress $PrivateIP -PrefixLength $prefixLen -AddressFamily IPv4 -ErrorAction Stop | Out-Null
    }
}

function New-IcsManager { return (New-Object -ComObject HNetCfg.HNetShare) }
function Get-IcsConnection([object]$mgr, [string]$name) {
    foreach ($conn in @($mgr.EnumEveryConnection)) {
        $props = $mgr.NetConnectionProps.Invoke($conn)
        if ($props.Name -eq $name) { return $conn }
    }
    return $null
}
function Get-IcsState([string]$name) {
    $mgr = New-IcsManager
    $conn = Get-IcsConnection $mgr $name
    if (-not $conn) { return [pscustomobject]@{ Found=$false; Enabled=$false; Type=-1 } }
    $cfg = $mgr.INetSharingConfigurationForINetConnection.Invoke($conn)
    $typ = -1
    try { if ($cfg.SharingEnabled) { $typ = [int]$cfg.SharingConnectionType } } catch {}
    return [pscustomobject]@{ Found=$true; Enabled=[bool]$cfg.SharingEnabled; Type=$typ }
}
function Disable-Ics([string]$name) {
    try {
        $mgr = New-IcsManager
        $conn = Get-IcsConnection $mgr $name
        if ($conn) {
            $cfg = $mgr.INetSharingConfigurationForINetConnection.Invoke($conn)
            if ($cfg.SharingEnabled) { $cfg.DisableSharing() }
        }
    } catch {}
}
function Enable-IcsSide([string]$name, [int]$mode, [string]$label) {
    for ($attempt=1; $attempt -le 3; $attempt++) {
        # Re-create COM objects every attempt. Restarting SharedAccess invalidates
        # old HNetCfg connection/configuration objects on some Windows 11 builds.
        $mgr = New-IcsManager
        $conn = Get-IcsConnection $mgr $name
        if (-not $conn) { Write-Host "[ICS] cannot enumerate $label '$name'" -ForegroundColor Yellow; return $false }
        $cfg = $mgr.INetSharingConfigurationForINetConnection.Invoke($conn)
        try {
            if ($cfg.SharingEnabled -and [int]$cfg.SharingConnectionType -eq $mode) { return $true }
        } catch {}
        try { $cfg.EnableSharing($mode) }
        catch { Write-Host "[ICS] $label EnableSharing attempt $attempt returned: $($_.Exception.Message)" -ForegroundColor Yellow }
        Start-Sleep -Milliseconds 700
        $st = Get-IcsState $name
        if ($st.Enabled -and $st.Type -eq $mode) {
            Write-Host "[ICS] $label sharing enabled." -ForegroundColor Green
            return $true
        }
        if ($attempt -lt 3) {
            Write-Host '[ICS] refreshing SharedAccess + COM state before retry...' -ForegroundColor Yellow
            Restart-Service SharedAccess -Force -ErrorAction SilentlyContinue
            Start-Sleep -Milliseconds 1200
        }
    }
    return $false
}

Set-Service SharedAccess -StartupType Manual -ErrorAction SilentlyContinue
Start-Service SharedAccess -ErrorAction SilentlyContinue

# A pre-assigned static address can make HNetCfg reject a Wintun adapter as the
# private/home side. Let ICS claim the adapter first, then restore our known PPP
# gateway address after sharing is configured.
Get-NetIPAddress -InterfaceAlias $PrivateAlias -AddressFamily IPv4 -ErrorAction SilentlyContinue |
    Where-Object { $_.PrefixOrigin -ne 'WellKnown' } |
    Remove-NetIPAddress -Confirm:$false -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 300

$ok = $false
try {
    Disable-Ics $publicAlias
    Disable-Ics $PrivateAlias
    Restart-Service SharedAccess -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 900

    $pubOk = Enable-IcsSide $publicAlias 0 'public'
    $privOk = $false
    if ($pubOk) { $privOk = Enable-IcsSide $PrivateAlias 1 'private' }
    $ok = $pubOk -and $privOk

    # If this Windows build dislikes public-first ordering, clear the pair and
    # retry private-first with brand-new COM objects.
    if (-not $ok) {
        Write-Host '[ICS] first ordering failed; retrying private-first...' -ForegroundColor Yellow
        Disable-Ics $publicAlias
        Disable-Ics $PrivateAlias
        Restart-Service SharedAccess -Force -ErrorAction SilentlyContinue
        Start-Sleep -Milliseconds 900
        $privOk = Enable-IcsSide $PrivateAlias 1 'private'
        $pubOk = $false
        if ($privOk) { $pubOk = Enable-IcsSide $publicAlias 0 'public' }
        $ok = $pubOk -and $privOk
    }
} finally {
    # Whether ICS worked or not, the v92isp adapter must keep the local PPP
    # gateway so the application can use WinDivert NAT as a fallback.
    Ensure-PrivateIp
}

if (-not $ok) {
    throw 'ICS could not establish both public and private sharing; application will use its built-in WinDivert NAT fallback.'
}

$pubState = Get-IcsState $publicAlias
$privState = Get-IcsState $PrivateAlias
if (-not $pubState.Enabled -or $pubState.Type -ne 0 -or -not $privState.Enabled -or $privState.Type -ne 1) {
    throw 'ICS state verification failed after configuration; application will use its built-in NAT fallback.'
}
Write-Host '[ICS] Internet Connection Sharing is enabled and verified.' -ForegroundColor Green
exit 0
