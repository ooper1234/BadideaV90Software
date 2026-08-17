# Linksys PAP2 direct-modem lab settings

The goal is to make the PAP2/ATA path a nearly transparent **FXS <-> G.711 mu-law RTP** bridge. v92isp can receive calls directly on the LAN or register to a local SIP proxy/PBX. The checked-in WSL config registers extension `101` to `192.168.2.40:5060` with an empty password.

## Network topology

```text
Windows dial-up PC
  -> hardware 56K modem
  -> RJ11
  -> PAP2 PHONE port
  -> Ethernet LAN
  -> Linux PC running v92isp-live
```

Example Linux address used below: `192.168.1.50`.

## PAP2 Line settings

Use the advanced/admin voice page for the PHONE port being tested.

- Line Enable: **Yes**
- Proxy: **192.168.1.50** (replace with your Linux PC)
- Register: **No**
- Use Outbound Proxy: **No**
- SIP Transport: **UDP**
- Preferred Codec: **G711u**
- Use Pref Codec Only: **Yes** if the firmware exposes that option
- Silence Supp Enable / VAD: **No**
- Echo Supp Enable: **No**
- Echo Canc Enable: preferably **No for this modem lab**
- FAX passthrough codec: **G711u** if present
- Do not enable G.729/G.723 or another compressed speech codec

The PAP2 local SIP port can stay at its normal value. The Linux server listens on UDP 5060 by default.

A simple dial plan such as `(x.)` is enough when the Linux PC is the configured proxy. Dial any test number, e.g. `555`.

## Why these settings matter

V.90/V.92 depends on the relation between exact 8-kHz PCM codewords and the analogue voltage presented to the modem. Speech processing, silence suppression, codec transcoding, sample-rate conversion, and aggressive adaptive jitter processing can destroy that relation and force fallback.

For the first test keep the PAP2 and Linux PC on the same wired LAN.

## Windows modem side

Create an ordinary dial-up connection using the hardware modem attached to the PAP2. The telephone number can be any number accepted by the PAP2 dial plan, for example `555`.

For the current end-to-end Internet test use the server's `--mode v21`. The expected negotiated line rate is only **300 bit/s**; this mode exists to prove the entire chain (real modem -> RTP -> software modem -> PPP -> NAT -> Internet) before the V.90/V.92 data pump is complete.

For live V.92 handshake experiments run `--mode v92`. That path currently reaches the V.92 Short Phase 2 boundary but does not yet carry PPP data at 56K.

## Faster currently-usable mode: V.22bis 2400

If the project was built with SpanDSP, try the 2400-bit/s path before the 56K work is complete:

```bash
./build/v92isp-live --features
# must contain: spandsp-v22=yes

sudo ./scripts/run-v22bis-internet.sh 192.168.1.50
```

For a public/PAP-authenticated test:

```bash
sudo ./scripts/add-user.sh retro 'choose-a-password'
sudo ./scripts/run-public-v22bis.sh 192.168.1.50
```

V.42/LAPM is terminated in the V.22/V.22bis path. When V.8 negotiates LAPM,
the server skips the older V.42 capability probe and enters LAPM establishment
directly. For forced/non-V.8 V.22bis calls it runs the legacy V.42 probe and
falls back to transparent 8-N-1 data if the caller does not use LAPM. V.42bis
receive decompression is activated only after the V.42 XID exchange advertises
compatible P0/P1/P2 values; plain LAPM payloads are never guessed to be
compressed.

V.90/V.92 remains experimental: Quick Connect startup is implemented, but full V.90/V.92 data transfer is not yet present.

## Your proxy-based WSL setup

With the included config, v92isp itself registers as SIP extension `101`:

```text
registrar: 192.168.2.40:5060
user:      101
password:  <empty>
codec:     PCMU/G711u only
```

Configure the device/extension that places the call so the PBX routes the dialed
number to extension `101`. v92isp answers the resulting INVITE and learns the RTP
peer from SDP/symmetric RTP.
