$ErrorActionPreference = 'SilentlyContinue'
Get-NetFirewallRule -DisplayName 'v92isp SIP UDP' | Remove-NetFirewallRule
Get-NetFirewallRule -DisplayName 'v92isp RTP UDP' | Remove-NetFirewallRule
Get-NetFirewallRule -DisplayName 'v92isp app UDP' | Remove-NetFirewallRule
Get-NetFirewallRule -DisplayName 'v92isp block private LAN' | Remove-NetFirewallRule
Get-NetNat -Name 'v92isp' | Remove-NetNat -Confirm:$false
Write-Host '[OK] v92isp firewall/WinNAT rules removed. The reusable Wintun adapter may remain installed; it is harmless when v92isp is stopped.'
