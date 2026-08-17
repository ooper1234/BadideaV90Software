# Public/free dial-up service on Windows

The software can be free/open-source; telephone/SIP carrier charges are separate.

## Authentication

Set in `config\v92isp-windows.env`:

```text
REQUIRE_PAP=1
```

Then edit `config\pap-secrets.txt` using one tab-separated account per line:

```text
username<TAB>password
```

## Network isolation

`Setup-Windows.ps1` creates a Windows Firewall block rule for traffic sourced from `10.77.0.0/24` toward RFC1918 private ranges. Keep that rule enabled before accepting untrusted callers.

## One call per process

The current server handles one modem/SIP call at a time. A public multi-line service needs per-dialog modem/PPP sessions, separate client addresses, and a call/session manager.

## Speed status

The stable implemented Windows path currently reaches V.22bis 2400 / V.22 1200 and V.21 fallback. Do not advertise it as a functioning 56K ISP until the V.90/V.92 high-speed DSP is complete.
