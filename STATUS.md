# v92isp status — V.90/V.92 DSP milestone

## Working / integrated

- Native Windows PJSIP 2.17 REGISTER / INVITE / ACK / BYE / SDP / RTP.
- PCMU/G.711 mu-law, 8000 Hz, 20 ms modem audio.
- Two-way PJSUA2 media bridge.
- Stable V.8 -> V.22/V.22bis 1200/2400 path with V.42 LAPM, XID-gated
  V.42bis receive decompression and transparent-async compatibility fallback.
- Correct answer-side 44-byte XID/F encoding, including the mandatory HDLC
  optional-functions value, before the caller advances to SABME/UA.
- V.21 300-bps fallback.
- Userspace PPP: framing/FCS, LCP, optional PAP, IPCP, IPv4.
- Windows client disconnect: Terminate-Ack flush followed by SIP BYE/carrier drop.
- Wintun + single-peer WinDivert IPv4 NAT with WinNAT/ICS backup.
- Inbound IPv4 fragmentation to the configured PPP MRU.

## New real V.90 laboratory path

- Explicit `MODE=v90` V.8 digital-modem capability negotiation.
- V.90 Phase-2 75 ms silence, INFO0d DBPSK/1200-Hz transmission and CRC.
- Analogue 2400-Hz INFO/Tone-A observation and phase-reversal detection.
- Digital 1200-Hz Tone-B ranging response with 40 ms reversal timing.
- V.90 PCM six-sample mapping algebra with round-trip tests.
- Complete Phase-3 Ja/DIL receive and both post-Jd S/S-bar transitions.
- CRC-valid variable-length CPt receive, Ri-bar, and CPt-parameterized final
  TRN2d generation.
- Standards-directed bounded recovery for missing INFO1a: listen silently for
  Tone A or INFOMARKSa, then cleanly retrain or repeat INFO1d.
- Clean stop on a V.34 selection or exhausted Phase-2 retries; the code no
  longer emits incompatible V.22 training in the middle of a V.90 handshake.

## V.92 laboratory path

- QC1a / QCA1d.
- QTS / QTS-bar.
- ANSpcm and TONEq.
- Real short Phase 2: V.92 capability INFO0, >=50-ms Tone B, B reversal and
  10-ms hold, client Tone-A reversal, CRC-valid INFO1a.

## Not complete yet

- full live V.34 PHY/startup (required for V.90 upstream);
- V.90 Phase 4: MP/CP -> MP'/CP' acknowledgement is implemented for live testing; Ed/B1d and the final live PCM/V.34 data-mode handoff remain
  mode;
- V.92 PCM upstream to 48 kbit/s and complete Quick Connect data resumption;
- production V.32/V.32bis PHY;
- multi-call/multi-line daemon.

**Do not describe this release as full 56K yet.** The real Internet-capable
path remains V.22/V.22bis until the high-speed startup/data-pump work above is
finished.

## v6 hardware-interoperability diagnostics

The explicit V.90 test path now validates the V.8 analogue/digital PCM pairing, starts INFO0d immediately after SpanDSP's existing post-CJ 75-ms guard, and prints receive spectrum telemetry for INFO0a/Tone-A diagnosis. The stable V.22/PPP/NAT path is unchanged.
