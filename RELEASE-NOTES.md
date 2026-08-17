# Release notes: WSL-ready Internet server

This release is configured for:

- SIP proxy/registrar: `192.168.2.40:5060`
- SIP extension: `101`
- SIP password: empty
- WSL2 mirrored networking on Windows 11 22H2+
- PCMU/G.711u RTP, 8 kHz, 20 ms packets
- stable software mode: V.8 -> V.22/V.22bis -> V.42/transparent -> PPP -> NAT
- built-in V.21 300-bit/s fallback

Automated tests cover the core modem helpers, V.92 Quick Connect state
progression, PPP binary transport, SIP INVITE/RTP/BYE, SIP Digest REGISTER with
an empty password, and a synthetic V.21 modem sending PPP bytes through real
UDP RTP into the live server and through the PPP PTY bridge.

Full V.90/V.92 56K data transfer is not included yet. `MODE=v92` is an
experimental handshake path only.

## August 15 CPt hardware follow-up

- Added an independent raw-RRC CP/CPt decode candidate alongside the retained
  TRN equalizer and the adaptive re-equalizer. The 21:31 hardware capture
  contains CRC-valid 426-bit CPt frames, but a stale retained TRN equalizer can
  poison both existing branches because the adaptive branch starts from the
  same seven taps. The raw candidate bypasses that stale inverse and still
  requires the complete Table-14 framing and V.34 CRC before acceptance.
- Tied the reported CPt decision error to candidates that actually produce a
  plausible CP/CPt header. The former global minimum could report ~0.08 rad
  from one blind-equalizer solution while `header=seen` came from a different
  branch, making the diagnostic misleading.
- Added six-phase cancellation of the locally transmitted period-6 Ri echo
  before CPt demodulation, matching the offline decode that reduced hardware
  QPSK error from 0.335 to 0.165 rad and recovered the 426-bit CPt CRC.
- Added a guarded compatibility path for the captured modem's CRC-valid
  426-bit short-fill CPt (`290+d`). It is accepted only after three aligned,
  bit-identical repetitions independently pass the strict header and CRC;
  standard Table-14 `292+d` framing remains the primary path.
- Prevented a recognized CPt header from being reclassified as a 50-ms Tone-A
  retrain merely because its valid QPSK waveform has strong 2400-Hz energy.
  The bounded Phase-4 CPt timeout remains responsible for genuine recovery.
- Added repeated-CPt majority combining for burst-damaged G.711 paths. The
  receiver requires at least three aligned copies and still requires the
  combined variable-length CPt to pass every fixed field and the full V.34 CRC.
- Matched the DIL-completion S threshold to the validated post-Jd S threshold
  (`0.55`). The hardware line delivered valid S at `0.58-0.60`, below the old
  DIL-only `0.62` cutoff. Long 64/80/96T windows remain mandatory, so SCR is
  still rejected.
- Corrected Table 14 CPt discriminator bit 19 to `0`; `1` identifies ordinary
  CP. The receiver and synthetic generator previously shared the reversed
  value, hiding the error in tests while rejecting every hardware CPt header.
- Prevented SCR transmitted by the caller during DIL from matching a short
  32T S-detector window and ending DIL early. The post-DIL gate now requires a
  coherent 96T/112T/128T region of the specified 128T completion S burst.
- Corrected the Phase-4 sequence polarity: initial Ri is ordinary R (`+++---`)
  at UINFO. Only after receiving CPt does the digital modem send 24T Ri-bar
  (`---+++`). Starting with Ri-bar left the hardware caller transmitting SCR,
  which explained the good QPSK quality with no recognizable CPt header.
- Preserved the seven-tap upstream line equalizer trained on the caller's TRN
  and now reuse it for Phase-4 CPt. The former CPt path discarded that channel
  estimate and attempted to decode the longest CRC-protected Phase-4 message
  from raw, unequalized symbol samples.
- CPt validation remains strict: complete variable-length framing, fixed/start
  fields, V.34 descrambling, and the full CRC must pass. Unsupported but valid
  CPt parameters are now reported as a transport incompatibility instead of a
  misleading "no CPt" timeout.
- Added once-per-second hardware diagnostics for retained-equalizer state,
  fixed-header detection, and mean QPSK decision error.
- Added regressions through a splitter-style echo for both the compact 292-bit
  CPt and the maximum 1788-bit layout with six separate codec constellation
  collections.

## August 15 Jd/S hardware-stall follow-up

- Tone-A retrain recognition now evaluates one phase-continuous 50-ms window
  across RTP packets. Three independent 20-ms TRN blocks can no longer be
  added together and mislabeled as one held Tone A.
- Extended downstream TRN1d from 2400T to 4800T. V.90 requires at least 2040T;
  the extra PCM receiver-training margin remains safely inside the client's
  4500-ms post-Ja Jd deadline.
- The post-Jd S detector now searches coherent 32T/48T/64T regions within the
  specified 128T S burst, tolerating a short splitter/line echo at either edge.
- Added live S-correlation diagnostics and bounded Jd, DIL, and CPt recovery.
  A missing transition now performs a full retrain instead of appearing stuck.

## August 15 final-TRN receive fix

- The trained Phase-3 V.34 observer now examines each block before the generic
  2400-Hz retrain detector. A caller's post-Jd S signal can no longer be
  discarded as a false 50-ms Tone-A request on the block where S validates.
- Added repeated 4-point CPt receive with GPA descrambling, variable-length
  constellation parsing, and V.34 CRC validation.
- A valid CPt now produces the specified 24T Ri-bar followed by 2400T of real
  CPt-parameterized TRN2d. The mapper uses the caller's K/S, look-ahead,
  spectral shaping, and codec constellation masks.
- Added end-to-end regressions for the hardware failure sequence through S,
  Jd-bar, DIL, Ri, CPt, and final TRN2d.

## August 15 XID/SABME and INFO1a recovery follow-up

- Fixed the answer-side V.42 XID frame produced after a hardware caller's
  CRC-valid XID command. SpanDSP 0.0.6 omitted a four-byte pointer advance,
  overwrote the mandatory HDLC optional-functions value, and returned F=0 for
  a P=1 command. The wrapper now replaces the queued frame with an exact,
  independently tested 44-byte XID/F response before HDLC transmission.
- The hardware trace's three valid XID commands are now treated as proof that
  the receive path is good and the caller is retrying our rejected response;
  transparent async is no longer expected to rescue this error-correcting
  negotiation before SABME.
- INFO1d now projects 9600 bit/s only for V.34 symbol-rate/carrier combinations
  advertised by the caller's CRC-valid INFO0a. The previous all-rates template
  contradicted peers that omitted 2743 or 2800 symbols/s.
- Replaced the post-INFO1d blind Tone-B retry with V.90 recovery: remain silent
  and condition the receiver for Tone A or INFOMARKSa. Tone A starts a clean
  70-ms-silence retrain; INFOMARKSa repeats INFO1d. Both paths are bounded.
- A retrain now clears the stale second-ranging flag. It can no longer jump
  from the first Tone-A reversal directly to local L1/L2 and INFO1d.
- Added exact XID octet, INFO0a/INFO1d capability, INFOMARKSa/Tone-A
  discrimination, two-data-pump LAPM, Windows, and WSL regressions.

## August 15 hardware follow-up: LAPM control and long-TRN clock tracking

- After answer-side ADP, the independent CRC-validating HDLC receiver now
  becomes the sole LAPM receive path and passes XID, SABME, supervisory and
  information frames exactly once into SpanDSP. This works around the
  unfinished library path that recognized ODP and flags but sometimes never
  answered the caller's first XID/SABME. Runtime events now distinguish XID,
  SABME, bad-FCS frames, and a peer that sends flags but no complete frame.
- V.90 Phase-3 receive now estimates the analogue modem's independent symbol
  clock from consecutive known-TRN windows. A legal 100-ppm error accumulates
  by more than one symbol during a long TRN; the old fixed `8000/rate` sampler
  could therefore lock at 0.92 correlation and still reject every later Ja.
- Phase-3 retrain detection now requires a strongly coherent unmodulated
  2400-Hz Tone A. Incidental energy from wideband TRN/Ja no longer accumulates
  as a false 50-ms retrain request.
- Added regressions for a 12,000-symbol TRN at 100-ppm clock error, false Tone-A
  rejection, and the full ODP/ADP/HDLC/LAPM exchange through two V.22bis DSPs.
- A real V.34 fallback remains outside this patch: the installed SpanDSP has no
  V.34 API, and switching to V.22 after INFO1a selects V.34 is not a valid
  in-call fallback.

## PJSIP V.90 negative-zero and receiver-timing fix

- Fixed the signed-linear bridge and PJSIP encoder collapsing PCMU `0x7F`
  (negative zero) into another codeword. V.90 Sd/S-bar requires both zero
  codewords as distinct training symbols; the old bridge therefore corrupted
  the first downstream Phase-3 signal even though its in-memory sequence was
  right.
- Added a reproducible PJSIP 2.17 table patch, an actual-library startup guard,
  and a bit-exact 256-codeword bridge regression.
- After validating client Ja, the server now uses a standards-allowed 400-ms
  receiver guard before Sd/S-bar and sends 2400T TRN1d instead of stopping at
  the 2040T minimum.
- The supplied trace proves client TRN/Ja receive is working; the retrain begins
  after the downstream response, which is consistent with the corrupted
  negative-zero Sd/S-bar reaching the hardware modem.

## V.90 client-TRN/Ja receive and retrain fix

- Removed the elapsed-time/energy gate that could start downstream Phase 3
  before the analogue modem finished its pseudo-random training.
- Added a 27-tap root-raised-cosine V.34 upstream observer for the selected
  3000, 3200, or 3429 symbol/s low carrier.
- Correlates against the caller GPA-scrambled TRN sequence, searches carrier
  offset and fractional symbol timing, and trains a seven-tap complex LMS
  equalizer on the client's first 512 TRN symbols.
- Differentially decodes and GPA-descrambles the following stream. Downstream
  Sd is released only after the Ja DIL frame sync and fixed start/reserved
  fields validate; unrelated tones or random energy leave the ISP silent.
- Detects a client Tone-A retrain request held for at least 50 ms in every
  Phase-3 transmit/receive state, stops the current PCM sequence, sends 70 ms
  silence, then answers with Tone B and re-enters the ranging exchange.
- Added shaped caller-waveform regressions with carrier offset, MD, false
  energy, Ja gating, and the Tone-A retrain response.
- Made Ja acquisition use GPA's 23-bit self-synchronization and scan repeated
  descriptors, so a long TRN or lost first Ja boundary does not force retrain.

## V.22bis V.42 detection fallback fix

- The answerer now applies the V.42 T400 default of 750 ms after the V.22bis
  carrier is trained.
- If no Originator Detection Pattern or protocol-start flags appear, it keeps
  the trained 2400-bit/s carrier, replays captured caller bits, and enters
  transparent asynchronous PPP instead of remaining at "waiting for
  V.42/LAPM" and eventually discarding the carrier.
- `LAPM_IDLE` no longer accidentally cancels T400. A valid
  `LAPM_ESTABLISH` transition cancels T400 but arms a separate ten-second
  establishment watchdog; if SABME/UA never reaches data state, the modem
  keeps the trained carrier and falls back to transparent V.14 operation.
- Starts the V.42 control layer at physical carrier-up and corrects SpanDSP's
  hardcoded 28.8-kbit/s timer rate to the actual 2400-bit/s carrier rate.
- After SpanDSP's minimum ten EC answerer patterns, continues ADP until an
  originator HDLC flag arrives, as recommended by V.42 Appendix III.1. A
  transparent fallback now reacquires mark-idle before exposing bytes, rather
  than passing ODP/LAPM residue such as `7E E0 01` to PPP.

## Hardware timeout follow-up: complete Ja and V.42 ODP

- The repeated caller bytes `11 91 11 91` are the V.42 Originator Detection
  Pattern (DC1 with alternating parity), not PPP or random payload. Added an
  independent detector requiring four alternating DC1 characters with 8..16
  mark bits between them. T400 is now disarmed as soon as ODP is valid while
  SpanDSP continues transmitting ADP and establishing LAPM.
- Ja validation now parses the descriptor's variable N/LSP/LTP-derived length,
  every required start/reserved/padding bit, and its V.34 CRC. The earlier
  52-bit-prefix gate could interrupt a descriptor that is at least 240 bits
  even when N=0, causing the client to request Tone-A retraining.
- After the complete descriptor, downstream Phase 3 waits 400 ms (within the
  V.90 allowance of up to 500 ms), then sends Sd/S-bar. Logs now report the
  received descriptor length and requested DIL segment count N.
- This receive-driven timing was cross-checked against the labelled successful
  V.90/V.92 recordings: their caller TRN-to-answerer-start interval is about
  2.4--2.5 seconds, versus about 2.33 seconds for the corrected failing trace.
- Fixed the Ja and Jd V.34 CRC coverage to exclude frame sync, start bits, and
  fill bits as required by V.34 10.1.2.3.2. The previous synthetic sender and
  receiver agreed with each other but rejected a real modem's Ja; the outgoing
  Jd was likewise invalid to a standards-compliant client. The Ja regression
  now uses the independently derived fixed N=0 CRC vector `0xEF8A`, and the Jd
  test independently builds its information field, preventing that class of
  self-consistent test failure.
- Fixed Ja's conditional trailing fill rule from Table 12/V.90. N=0 requires
  the optional second fill and is 240 bits; N=1 is already even at 256 bits and
  has only the mandatory fill. The previous parser consumed the next repeated
  Ja sync bit and rejected standards-valid nonzero-DIL descriptors.

## Standards-backed V.90/V.92/V.42 interoperability fix

- Fixed V.34/V.90/V.92 numeric INFO fields to use their required LSB-first
  on-wire order. This fixes the hardware trace where V.90 selector 6 was
  incorrectly decoded as V.34 selector 3.
- Added INFO0 acknowledgement/repetition and two bounded client Tone-A
  ranging retries.
- Completed V.92 Quick Connect short Phase 2 through CRC-valid INFO1a, with
  the specified no-TONEq and short-to-full-Phase-2 recovery paths.
- Enabled V.42 LAPM in the stable V.22/V.22bis negotiation.
- Added XID-gated V.42bis receive decompression using the answer-side
  P0=1/P1=512/P2=6 parameters advertised by SpanDSP.
- Added a Windows-native protocol regression executable and CTest entry.
- Kept the Phase-3/4 and high-speed data-pump boundary explicit: the program
  still does not claim a complete V.90/V.92 data connection.

## WSL SpanDSP 0.0.6 compatibility fix
- Fixed V.8 for Ubuntu/Debian SpanDSP 0.0.6 flat `v8_parms_t` API.
- Removed dependency on private V.42 structure fields.
- Keeps public V.42/LAPM callbacks and V.22/V.22bis DSP.

## Windows Wintun export hotfix
- Fixed case-sensitive dynamic lookup of the official `WintunGetAdapterLUID` export.
- Previous builds incorrectly requested `WintunGetAdapterLuid`, causing startup to fail even with the correct official amd64 DLL.

## MinGW PJSIP link fix

- Fixed final Windows PJSIP link failure caused by combining PJSIP pkg-config `-lstdc++` with `-static-libstdc++`.
- Keeps static C++ runtime redistribution while avoiding mixed static/dynamic libstdc++ duplicate symbols.

## Consolidated complete Windows fix

- Native PJSIP path retained and PJSIP UDP keep-alive disabled for the ESP32 MiniPBX.
- Stable auto mode now uses V.22/V.22bis transparent async bytes into PPP instead of blocking on incomplete LAPM interop.
- Windows RAS disconnect now receives LCP Terminate-Ack and then an intentional SIP BYE/carrier drop.
- Internet forwarding now prefers a built-in WinDivert single-peer IPv4 NAT, avoiding the observed ICS private-side COM failure. WinNAT/ICS remains a fallback.
- Inbound Internet packets are fragmented to the configured PPP MRU before serial transmission.
- Added NAT first-packet diagnostics (`NAT-TX` / `NAT-RX`).

## Complete v3 - WinDivert/MinGW header compatibility

- Fixed the WinDivert NAT source failing to compile under MSYS2/MinGW because WinDivert 2.2.x defines `__in`/`__out` SAL macros that collide with libstdc++ internals.
- C++ standard headers are now parsed before `windivert.h`, and the WinDivert annotation macros are undefined immediately after its declarations.
- Retains complete v2 pacman-lock recovery, PJSIP RTP, raw V.22 PPP, client disconnect carrier drop, PPP fragmentation, and WinDivert NAT fallback.

## V.90/V.92 high-speed DSP milestone (v4)

- Added explicit `MODE=v90` digital-modem V.8 negotiation.
- Added standards-shaped V.90 INFO0d generation, V.34 CRC, 600-bps DBPSK and
  1200-Hz digital INFO carrier.
- Added 2400-Hz analogue Tone-A observation, phase-reversal ranging and
  1200-Hz Tone-B response timing.
- Added the first V.90 PCM mapping algebra core and round-trip tests.
- Kept stable auto mode on the proven V.22bis/V.22/V.21 data path.
- Added separate V.90, V.92 and stable-auto Windows launchers.
- Full V.90/V.92 data mode remains intentionally unreported until V.34
  upstream plus the remaining V.90/V.92 startup/training phases are complete.

## V.90 Phase-2 hardware interoperability fix (v5)

- Fixed required pre-INFO modulation reference point.
- Added CRC-valid analogue INFO0a decoding with arbitrary sample timing and 1800-Hz guard-tone tolerance.
- Corrected V.90 INFO0d bit 19 semantics for 3429-symbol/s support.
- Replaced packet-boundary-dependent Tone-A reversal detection with a sliding coherent detector.
- Explicit V.90 mode now follows the standard recovery rule and continues Tone B while waiting for the first Tone-A reversal rather than falling back after an invented 8-second timeout.
- Added hardware-trace-focused V.90 regression coverage; stable V.22/PPP mode is unchanged.

## V.90 Phase-2 V.8/timing research fix (v6)

- Advertise the V.90 server as `DCE_ON_DIGITAL` in the V.8 PSTN-access category and preserve the caller CM's PCM/PSTN categories for validation and logging.
- Reject a nominal V.90 selection when the caller did not advertise analogue PCM-modem availability; fall back truthfully instead.
- Remove the duplicate 75-ms Phase-2 guard: SpanDSP already performs the post-CJ guard before reporting V.8 success.
- Generate INFO0d/Tone B at the -12 dBm0 nominal level declared in INFO0d.
- Add live 1200/1800/2400-Hz Phase-2 spectrum diagnostics to separate no-response, INFO0a-demod, and Tone-A detection failures.
