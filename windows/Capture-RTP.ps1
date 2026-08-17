param(
    [string]$PeerIP = '192.168.2.38',
    [int]$RtpPort = 40000
)
$ErrorActionPreference = 'Stop'
$id = [Security.Principal.WindowsIdentity]::GetCurrent()
$pr = New-Object Security.Principal.WindowsPrincipal($id)
if (-not $pr.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Run this PowerShell as Administrator.'
}
Write-Host "[PKTMON] Capturing UDP involving $PeerIP and port $RtpPort. Dial now. Ctrl+C to stop." -ForegroundColor Cyan
pktmon stop 2>$null | Out-Null
pktmon filter remove | Out-Null
pktmon filter add v92isp-rtp -i $PeerIP -p $RtpPort -t UDP | Out-Null
pktmon start -c --comp nics --pkt-size 0 --log-mode real-time
