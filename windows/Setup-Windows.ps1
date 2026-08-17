$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path

function Test-Admin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    $p = New-Object Security.Principal.WindowsPrincipal($id)
    return $p.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}
if (-not (Test-Admin)) {
    Write-Host '[SETUP] Re-launching as Administrator...'
    Start-Process powershell.exe -Verb RunAs -Wait -ArgumentList '-NoProfile','-ExecutionPolicy','Bypass','-File',("`"$PSCommandPath`"")
    exit $LASTEXITCODE
}

$config = Join-Path $root 'config\v92isp-windows.env'
if (-not (Test-Path $config)) { throw "Missing $config" }

function Get-ConfigValue([string]$key, [string]$default) {
    $line = Get-Content $config | Where-Object { $_ -match ('^' + [regex]::Escape($key) + '=') } | Select-Object -First 1
    if (-not $line) { return $default }
    return $line.Substring($key.Length + 1)
}
function Set-ConfigValue([string]$key, [string]$value) {
    $c = Get-Content $config -Raw
    if ($c -match ('(?m)^' + [regex]::Escape($key) + '=')) {
        $c = [regex]::Replace($c, ('(?m)^' + [regex]::Escape($key) + '=.*$'), "$key=$value")
    } else { $c += "`r`n$key=$value`r`n" }
    Set-Content -Path $config -Value $c -Encoding ASCII
}
function Test-UdpBind([string]$ip, [int]$port) {
    $u = $null
    try {
        $u = New-Object System.Net.Sockets.UdpClient
        $u.ExclusiveAddressUse = $true
        $u.Client.Bind([System.Net.IPEndPoint]::new([System.Net.IPAddress]::Parse($ip), $port))
        return $true
    } catch { return $false }
    finally { if ($u) { $u.Close() } }
}
function Find-UdpPort([string]$ip, [int[]]$candidates) {
    foreach ($p in $candidates) { if (Test-UdpBind $ip $p) { return $p } }
    throw "None of these UDP ports can be bound on ${ip}: $($candidates -join ', ')"
}

# Stop an old v92isp instance so it cannot hold our ports during an upgrade.
Get-Process -Name 'v92isp-windows' -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 300

# Determine the Windows LAN address used to reach the SIP PBX.
$server = ((Get-ConfigValue 'SIP_SERVER' '192.168.2.35:5060') -split ':')[0]
$udp = New-Object System.Net.Sockets.UdpClient
$udp.Connect($server,5060)
$lanIp = ([System.Net.IPEndPoint]$udp.Client.LocalEndPoint).Address.IPAddressToString
$udp.Close()
Set-ConfigValue 'BIND_IP' $lanIp
Set-ConfigValue 'ADVERTISE_IP' $lanIp

# Avoid WSAEACCES/WSAEADDRINUSE from a reserved or occupied port. SIP REGISTER
# advertises our chosen Contact port, and SDP advertises the chosen RTP port,
# so neither has to be hard-coded to 5060/40000 locally.
$sipPreferred = [int](Get-ConfigValue 'SIP_PORT' '5060')
$rtpPreferred = [int](Get-ConfigValue 'RTP_PORT' '40000')
$sipPort = Find-UdpPort $lanIp @($sipPreferred,5062,5070,5080,5090,5160)
$rtpPort = Find-UdpPort $lanIp @($rtpPreferred,40002,40004,41000,42000,43000)
Set-ConfigValue 'SIP_PORT' ([string]$sipPort)
Set-ConfigValue 'RTP_PORT' ([string]$rtpPort)
Write-Host "[OK] Windows SIP/RTP address: $lanIp"
Write-Host "[OK] SIP UDP port: $sipPort"
Write-Host "[OK] RTP UDP port: $rtpPort"

Get-NetFirewallRule -DisplayName 'v92isp SIP UDP' -ErrorAction SilentlyContinue | Remove-NetFirewallRule
Get-NetFirewallRule -DisplayName 'v92isp RTP UDP' -ErrorAction SilentlyContinue | Remove-NetFirewallRule
New-NetFirewallRule -DisplayName 'v92isp SIP UDP' -Direction Inbound -Action Allow -Protocol UDP -LocalPort $sipPort -Profile Any | Out-Null
New-NetFirewallRule -DisplayName 'v92isp RTP UDP' -Direction Inbound -Action Allow -Protocol UDP -LocalPort $rtpPort -Profile Any | Out-Null

Get-NetFirewallRule -DisplayName 'v92isp app UDP' -ErrorAction SilentlyContinue | Remove-NetFirewallRule
$exePath = Join-Path $root 'dist\v92isp-windows.exe'
if (Test-Path $exePath) {
    New-NetFirewallRule -DisplayName 'v92isp app UDP' -Direction Inbound -Action Allow -Program $exePath -Protocol UDP -Profile Any -EdgeTraversalPolicy Allow | Out-Null
}

Get-NetFirewallRule -DisplayName 'v92isp block private LAN' -ErrorAction SilentlyContinue | Remove-NetFirewallRule

Write-Host '[OK] Firewall/config ready. Internet forwarding will prefer built-in WinDivert NAT, with WinNAT/ICS as fallback.' -ForegroundColor Green
