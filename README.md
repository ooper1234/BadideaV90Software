# v92isp — native Windows dial-up ISP over SIP

This tree is the consolidated native-Windows build for the current lab setup:

```text
hardware modem -> PAP2/ATA -> SIP + PCMU -> PJSIP -> V.8/V.22(bis)
              -> V.42 LAPM / V.42bis -> userspace PPP -> IPv4 NAT -> Internet
```

## Current working path

- PJSIP 2.17 SIP/SDP/RTP frontend with PCMU/8000 only.
- Two-way modem audio through the PJSUA2 media bridge.
- Stable `MODE=auto`: V.8 -> V.22bis/V.22 -> negotiated V.42 LAPM and
  V.42bis receive decompression, with transparent-async fallback for a peer
  that does not support V.42. Direct detection uses the V.42 T400 default
  (750 ms) and preserves the trained carrier during fallback.
- Userspace PPP: LCP, optional PAP, IPCP, IPv4, async escaping and FCS.
- PPP addresses: server `192.168.137.1`, caller `192.168.137.2`.
- Incoming Internet IPv4 is fragmented to the configured slow-link PPP MRU.
- Internet forwarding prefers a built-in single-peer WinDivert NAT. WinNAT/ICS is only a fallback.
- A client LCP Terminate-Request is ACKed, then the SIP call is hung up after a short flush so Windows RAS loses carrier and finishes Disconnect.
- ESP32 MiniPBX-incompatible PJSIP UDP keep-alives are disabled; REGISTER refreshes remain enabled.

## Run it

Open **Command Prompt or PowerShell as Administrator** in the extracted folder and run:

```bat
START-V92ISP-WINDOWS.cmd
```

or:

```powershell
powershell -ExecutionPolicy Bypass -File .\windows\Setup-And-Start.ps1
```

The first run installs/builds the required Windows toolchain pieces, builds PJSIP 2.17, downloads the official Wintun runtime, downloads the official WinDivert 2.2.2-A runtime, builds `dist\v92isp-windows.exe`, configures the firewall, and starts the server.

## Current config

`config\v92isp-windows.env` is preconfigured for:

```text
SIP server : 192.168.2.40:5060
SIP user   : 101
SIP pass   : <empty>
codec      : PCMU / G.711 mu-law / 8000 Hz
PPP server : 192.168.137.1
PPP caller : 192.168.137.2
PPP MTU    : 296
DNS        : 1.1.1.1, 8.8.8.8
```

PAP is optional by default (`REQUIRE_PAP=0`). `config\pap-secrets.txt` contains the lab account `cooper / dialup` for use if PAP is enabled later.

## Expected successful call

Look for this progression:

```text
[RTP] FIRST inbound caller PCM frame received through PJSIP
[MODEM] ... CONNECT ... V.42 LAPM ... data link ready
[PPP-TX] FIRST bytes queued to caller modem
[PPP-RX] FIRST decoded bytes from caller modem
[PPP] LCP open; starting IPCP
[PPP] PPP IPv4 is UP: 192.168.137.2 <-> 192.168.137.1
[NAT-TX] FIRST PPP IPv4 packet forwarded to Internet
[NAT-RX] FIRST Internet reply returned to PPP peer
```

On Windows 7 Disconnect, the server should then show:

```text
[PPP] peer requested PPP termination
[PPP] client requested disconnect; Terminate-Ack queued, dropping modem carrier after flush
[CALL] client PPP disconnect complete; sending SIP BYE to drop carrier
```

## Modem/ATA settings

Keep the voice path transparent:

```text
Preferred codec:       G711u / PCMU
Use preferred only:    Yes
Silence suppression:   No
VAD:                   No
Echo suppression:      No
Echo cancellation:     No, if the ATA permits it
```

## V.90 / V.92 test modes

The high-speed DSP work now has explicit, truthful laboratory modes:

```text
START-V90-TEST.cmd   V.8 digital V.90 -> real Phase-2 INFO0d/Tone-B/ranging
START-V92-TEST.cmd   V.92 Quick Connect handshake laboratory path
START-STABLE-AUTO.cmd return to the proven Internet-capable V.22/V.21 path
```

`START-V92-TEST.cmd` changes the configuration to `MODE=v92` and launches the
already-built PJSIP server directly, so it is the normal launcher for repeated
Quick Connect calls. Stop an older server first with Ctrl+C. Quick Connect is a
reconnect procedure: the calling modem must have retained parameters from a
previous successful V.92 connection to the same server. On a first call (or
after the modem clears its saved parameters), the caller sends ordinary CM and
the server continues that same live V.8/V.90 exchange. A `QC1a timeout` is not a
normal first-call result: it means neither a valid QC1a nor a decodable CM was
received while ANSam was active.

`MODE=v90` now emits and receives the complete Phase-2 signal sequence through
CRC-valid INFO1a, including bounded client-tone recovery. After an analogue
upstream is selected, it listens to and equalizes the client's V.34 TRN random
training, validates the complete variable-length CRC-protected Ja descriptor,
and only then sends the defined downstream Phase-3 training: a 400-ms receiver
guard, 384T Sd, 48T S-bar-d,
4800T GPC-scrambled TRN1d (above the 2040T minimum and still inside the
caller's Jd timer), then repeating CRC-valid
Jd frames. It keeps the trained V.34 receiver active, detects the caller's
post-Jd S signal, completes Jd with 12T Jd-bar, transmits the exact Ja-requested
DIL (including N>0 descriptors), detects the later S/S-bar termination, and
enters Phase 4 with Ri. In Phase 4 it keeps the trained V.34 receiver active,
validates the caller's complete variable-length CPt (including V.34 CRC),
sends 24T Ri-bar, and generates 2400T final TRN2d from the exact CPt training
constellations, K/S, look-ahead and spectral-shaping parameters. The bridge
maps PCMU negative zero to a distinct signed-linear
sample and the reproducible Windows build patches PJSIP's encoder table so
Sd/S-bar remains bit-exact. Startup refuses an unpatched PJSIP build. It also
answers a client Phase-3 Tone-A
retry with the required silence/Tone-B retrain exchange. These streams are
defined training data, not arbitrary noise or user payload. It **does not
report a high-speed CONNECT yet** because the MP/CP acknowledgement, Ed/B1d
portion of Phase 4, plus the live V.34 upstream data pump and negotiated data
mode, remain unfinished. `MODE=v92`
performs Quick Connect Phase 1 and the real short Phase 2 through INFO1a; PCM
upstream and complete data resumption are not finished. See
`V90-V92-INTEROP-RESEARCH.md` and `V90-V92-MILESTONE.md`.

Long client TRN is resampled using an estimated analogue-modem symbol clock
rather than assuming an exact `8000/symbol-rate` ratio. Phase-3 Tone-A retry
detection now measures one phase-continuous 400-sample/50-ms window across RTP
packet boundaries, so three unrelated TRN blocks cannot cause a false retrain.
Post-Jd S detection searches shorter coherent sub-windows to tolerate splitter
echo, and Jd/DIL/Ri waits have standards-derived bounded retrain recovery rather
than hanging forever. In the V.22bis path, post-ADP HDLC frames are received through
one CRC-validating path and XID/SABME progress is reported before PPP starts.

## Tests

The regression suite covers the portable modem/PPP core. Windows also builds
and runs `v92_protocol_tests`, which checks the wire bit order, V.92 short
Phase 2, retry transition, all 256 bridge PCMU re-encode values, exact Phase-3
PCM training sequences, V.42 T400 fallback and V.42bis codec. A Windows-only
test checks all 256 values against the actual linked PJSIP encoder, and the
frontend repeats the critical negative-zero check at startup:

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Windows-only PJSIP/WinDivert runtime behavior must be validated on the actual Windows host.

## License

v92isp is GPL-2.0-or-later. See `LICENSE` and `THIRD_PARTY.md`.
