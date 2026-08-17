# Windows troubleshooting

## Close MicroSIP first

Only one process can normally own the SIP/RTP UDP ports. `Start-V92ISP.ps1` checks UDP 5060 and 40000 and tells you which process is using them.

## Registration

Normal startup:

```text
[SIP] REGISTER -> 192.168.2.40:5060
[SIP] registered as 101 at 192.168.2.40:5060
```

A 401 challenge before successful registration is normal SIP Digest behavior.

## RTP

During a call you should see inbound RTP diagnostics such as:

```text
[RTP] inbound 50 pkt/s, -18 dBFS, state V8Negotiating
```

If Windows still reports zero inbound RTP even though MicroSIP works, verify that MicroSIP is closed and that v92isp is advertising the same LAN IPv4 address. Inspect `config\v92isp-windows.env`.

## Wintun

Startup should show:

```text
[TUN] v92isp adapter ready at 10.77.0.1/24
[NAT] WinNAT ready for 10.77.0.0/24
```

If `wintun.dll` is missing, re-run:

```powershell
powershell -ExecutionPolicy Bypass -File .\windows\Build-Windows.ps1
```

Run PowerShell as Administrator because creating the Wintun adapter and WinNAT requires elevated networking privileges.

## PPP

After modem training succeeds:

```text
[PPP] modem data connected; starting userspace PPP
[PPP] LCP ...
[PPP] IPCP ...
[PPP] ... [NETWORK]
```

The dial-up client receives `10.77.0.2`; the ISP side is `10.77.0.1`.

## Internet

Administrator PowerShell:

```powershell
Get-NetNat -Name v92isp
Get-NetIPAddress | Where-Object IPAddress -eq '10.77.0.1'
Get-NetRoute -DestinationPrefix '10.77.0.0/24'
```

If the NAT does not exist:

```powershell
New-NetNat -Name v92isp -InternalIPInterfaceAddressPrefix '10.77.0.0/24'
```

## Modem/ATA settings

Use PCMU/G.711u only. Disable silence suppression/VAD and echo processing. Speech codecs and transcoding can break modem training.

## 56K

`MODE=v92` is not a full 56K data mode. The complete V.90/V.92 PCM data pump and V.34 upstream receiver still need implementation.
