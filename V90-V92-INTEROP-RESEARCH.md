# V.90/V.92/V.42 interoperability research and fix

This change was derived from the supplied PAP2/PJSIP hardware trace and the
normative ITU-T recommendations, then checked with Windows-native protocol
regressions.

## Root cause in the supplied V.90 trace

The trace reached a CRC-valid INFO1a, but reported that the analogue modem had
selected V.34. The CRC was valid because both the received frame and its CRC
were untouched; the bug was how multi-bit integer fields were interpreted.

V.90 and V.92 transmit integer fields marked LSB:MSB least-significant bit
first. The previous code read bits 37:39 as a conventional MSB-first number.
The wire pattern for selector 6 is `0,1,1`; the old parser therefore read it as
3 and falsely logged V.34. INFO0d and INFO1d power/rate fields had the same
encoding error and are now LSB-first too.

## V.90 recovery behavior

- A correct INFO0a is acknowledged by repeating INFO0d with bit 28 set.
- Tone A received before a correct INFO0a causes INFO0d to be repeated.
- A missed second Tone-A reversal enters silence, reacquires Tone A and retries
  the Tone-B ranging exchange. The implementation allows two bounded retries.
- A missed second-exchange reversal applies the specified forced Tone-B
  reversal recovery before L1/L2.
- A V.34 selection no longer starts V.22 in the middle of the established
  V.90/V.34 startup. That waveform switch was not a valid fallback.

## Root cause after client TRN/Ja succeeded

The later hardware trace shows the analogue modem locking its TRN, sending a
valid Ja, receiving the downstream response, and only then requesting a
Tone-A retrain. The downstream sequence in memory was correct, but the PJSIP
frontend decoded PCMU into signed 16-bit samples before the conference bridge
encoded it again. That round trip preserves 255 of 256 PCMU codewords. PCMU
negative zero (`0x7F`) decodes to integer zero and was re-encoded as positive
zero (`0xFF`). V.90 8.4.4 explicitly distinguishes `+0` and `-0` throughout
Sd/S-bar, so the hardware received a malformed first Phase-3 training signal.

The bridge now represents `0x7F` as linear sample -1 for re-encoding. PJSIP's
fast encoder table normally maps that sample to `0x7E`, so the Windows build
also applies a pinned one-entry patch that maps it back to `0x7F`. This is
still silence acoustically but recreates the required negative-zero PCMU
codeword. The frontend verifies the actual linked PJSIP result before opening
SIP. After a complete Ja descriptor, a 400-ms guard is used (inside the
permitted 500-ms window),
and TRN1d is extended from the 2040T minimum to 4800T. This supplies 600 ms
of downstream PCM receiver training while remaining well inside the analogue
modem's 4500-ms post-Ja Jd deadline in 9.3.2.7.

## V.92 Quick Connect

During ANSam the digital answer modem now runs the QC1a detector and the normal
V.8 CM receiver concurrently, as required by 9.2.4.1/V.92. A caller with no
matching saved profile sends CM; that same in-progress V.8 context sends JM and
continues to CJ. The implementation does not wait five seconds, discard CM, and
restart ANSam. The QC1a parser also validates both repeated V.8-formatted data
frames before accepting the saved UQTS/LAPM parameters.

ANSpcm generation now converts the Recommendation's 14-bit linear values to
the 16-bit domain expected by the local G.711 encoder. The previous generator
was 12 dB too quiet. The Windows protocol suite compares the entire 301-symbol
−12 dBm0 sequence against Table 8/V.92, as well as checking the 3612-symbol
phase reversal.

The old state labelled the 75-ms post-TONEq guard as "short Phase 2 reached".
The live path now performs short Phase 2 itself:

1. Send INFO0d with the V.92-capable and short-Phase-2 bits.
2. Confirm the corresponding bits in CRC-valid INFO0a.
3. Send Tone B for at least 50 ms after Tone A is present.
4. Reverse Tone B, hold it for 10 ms, then stop.
5. Detect the client Tone-A reversal.
6. Receive CRC-valid INFO1a directly; short Phase 2 omits L1/L2 and INFO1d.

If the peer does not confirm short Phase 2, or its short exchange times out,
the state machine retries using full Phase 2. Absence of TONEq for two seconds
after QCA1d returns to full V.8 startup.

## V.42 and V.42bis

V.42 is LAPM error correction; V.42bis is the data-compression recommendation.
The V.8/V.22 path now requests V.42, waits for LAPM data state before starting
PPP, and retains transparent asynchronous fallback for non-V.42 callers.

For a direct V.22/V.22bis call where V.8 did not select a protocol, the V.42
answerer applies the normative T400 detection timer. If no ODP or protocol
start is detected within the default 750 ms after carrier establishment, the
trained carrier is retained and captured caller bits are replayed into
V.14-style transparent asynchronous mode. When ODP is recognized, T400 is
stopped and LAPM establishment continues without this fallback timer.

SpanDSP may report `LAPM_IDLE` or enter `LAPM_ESTABLISH` without ever reaching
`LAPM_DATA`. The former no longer disables T400. The latter arms a separate
ten-second establishment watchdog; V.42 clause 7.9 permits fallback to
non-error-correcting V.14 operation after error-control establishment fails,
so the implementation retains the trained V.22bis carrier instead of hanging.

The hardware bytes `11 91 11 91 ...` are exactly ODP: ASCII DC1 with
alternating parity. They are not transparent user bytes. SpanDSP 0.0.6 only
reports a state change after its answer detection pattern has been sent, so an
application-side 750-ms timer can race a valid exchange. A separate raw-bit
observer now requires four alternating DC1 characters with the specified
8..16 intervening mark bits and disarms T400 immediately on that evidence.
The hardware follow-up reached this ODP path but did not reach LAPM before the
old five-second watchdog. Source inspection found that SpanDSP initializes its
V.42 timer rate to 28 800 bit/s regardless of the actual carrier, so T400/T401
wall time was wrong on a 2400-bit/s V.22bis connection. The wrapper now starts
V.42 at physical carrier-up and sets the control timer rate to the real data
rate. SpanDSP sends the required ten EC ADPs; after those, the wrapper follows
Appendix III.1's robustness guidance and continues EC ADP until it sees an
incoming HDLC flag, without consuming a queued LAPM response. If establishment
still fails, transparent receive is reset and requires a mark-idle guard before
delivering bytes, preventing ODP/HDLC residue such as `7E E0 01` from being
misreported as PPP.

SpanDSP's V.42 answerer advertises V.42bis P0=1, P1=512 and P2=6. A parallel
HDLC observer validates the caller's XID private parameter group. V.42bis
receive decompression activates only after compatible parameters are actually
seen and LAPM is connected. With P0=1, compression is in the negotiation
initiator-to-responder direction; server-to-client LAPM data remains
uncompressed, exactly matching the advertised answer-side parameters.

The next hardware trace reached ADP acceptance and incoming HDLC flags but
still timed out before `LAPM_DATA`. This isolates the failure after V.42
capability detection and before PPP: the answerer must receive the caller's
XID and/or SABME and return XID/UA. SpanDSP labels its V.42 code unfinished;
depending on the build, its internal HDLC receiver can miss that first real
control frame even though an identical parallel receiver sees the flags. At
the first post-ADP flag, the parallel CRC-validating receiver now becomes the
single owner of LAPM RX and feeds every complete frame to `lapm_receive` once.
It never duplicates frames into both state machines. Logs expose XID, SABME,
bad-FCS counts, and the no-complete-frame case so another timeout cannot be
mistaken for PPP failure.

## Verification and remaining boundary

`v92_protocol_tests` checks:

- the exact LSB-first selector/power bit patterns that caused the real failure;
- V.90/V.92 INFO CRC and decode round trips;
- the complete V.92 short-Phase-2 state path;
- transition into the bounded client-tone retry state;
- all 256 PCMU codewords across the project bridge's linear re-encode boundary;
- exact G.711 Sd/S-bar-d symbols, the GPC scrambler startup, 4800T live TRN1d,
  CRC-valid Jd and the live transition out of post-INFO1a silence;
- exact positive and negative V.42 ODP patterns in addition to an end-to-end
  2400-bit/s V.22bis carrier with no ODP, T400 transparent
  fallback, carrier preservation, and asynchronous byte recovery;
- an end-to-end V.22bis ODP/ADP exchange through two real SpanDSP data pumps,
  proving the answerer stays out of transparent mode and reaches LAPM data;
- a V.42bis compression/decompression round trip using the shipped SpanDSP.

The Windows-only regression additionally checks all 256 values against the
installed PJSIP table, and the frontend refuses startup unless its critical
negative-zero result is exactly `0x7F`.

## Post-INFO1a pseudo-random training

The first downstream waveform after the analogue modem's Ja window is not user
data and must not be random PCM. V.90 defines exact training sequences. The
implementation now uses UINFO, upstream symbol rate and MD length from the
CRC-valid INFO1a and emits:

1. Sd for 384 PCM symbols and S-bar-d for 48 symbols using Ucodes 16+UINFO and 0;
2. TRN1d for 2400 symbols (the standard requires at least 2040T) using Ucode
   UINFO, with sign bits produced by binary ones through the V.34 call-modem
   polynomial `1 + x^-18 + x^-23`;
3. repeating 72-symbol Jd capability frames with the scrambler state continued
   from TRN1d and a CRC over the specified information field.

The labelled successful recordings under `Documents\Dialup Recordings\successful`
were calls to 2600.network and were used as a hardware timing cross-check.
That service documents a G.711/ulaw SIP/T1 path terminating on a Patton remote
access server, making it a useful real digital-modem reference rather than
another build of this software. In the supplied annotated capture,
the calling modem's Phase-3 TRN runs for roughly 2.4--2.5 seconds before the
answering side begins S/PP/TRN. The failing trace found Ja at TRN symbol 5934
at 3200 symbols/s (about 1.85 seconds), then previously waited only 100 ms.
Receiving a minimum 240-bit descriptor takes another 75 ms and the new 400-ms
guard places downstream startup at about 2.33 seconds, closely matching the
successful exchange without relying on a fixed TRN timer.

The old implementation gated this start with inbound energy plus an elapsed
time estimate. That is insufficient: it can run ahead of a slow client and
cannot recognize a retrain request. The live gate is now receive-driven. A
27-tap root-raised-cosine receiver searches fractional timing and carrier
offset, correlates with the known caller GPA-scrambled TRN sequence, and trains
a seven-tap complex equalizer on the mandatory first 512 TRN symbols. It then
differentially decodes and GPA-descrambles the following symbols. Sd is queued
only after the complete variable-length Ja DIL descriptor validates. The
receiver derives its length from N, LSP and LTP, checks every specified
start/reserved/padding field, and verifies the V.34 CRC. For N=0 the descriptor
is still 240 bits; accepting only its first 52 bits was premature.
Because GPA is self-synchronizing after 23 received bits and Ja is repeated,
the receiver also scans the continuous post-TRN stream for later descriptors;
it no longer depends on guessing the exact first-Ja boundary or initial
descrambler register.

The next hardware call exposed a second Table 12 length rule. Every Ja DIL
descriptor has one fill bit after its CRC, but a second fill bit is present
only when necessary to make the complete descriptor contain an even number of
bits. The parser always required two. N=0 happens to need both and made the
synthetic test pass; a common N=1 descriptor is already exactly 256 bits, so
the parser consumed the following repeated Ja frame's first sync `1` as an
invalid second fill. Descriptor sizing now applies the conditional fill rule,
with independent N=0/240-bit and N=1/256-bit waveform regressions.

The August 15 hardware trace then exposed a CRC-coverage defect hidden by the
synthetic generator: V.34 10.1.2.3.2 says the CRC input contains information
bits but excludes frame-sync bits, every start bit, and fill bits. The old Ja
receiver included the many internal start bits, so it saw the real client's
clean TRN and descriptor header but rejected its standards-valid CRC. The Jd
transmitter used the same incorrect convention and would have made the client
reject the server response. Ja receive, Jd generation, Jd verification, and
the receiver and Jd path now use the specified coverage. The N=0 Ja regression
uses the independently derived fixed CRC vector `0xEF8A`, while the Jd test
computes its information field independently, so matching encoder/checker
mistakes cannot hide this again.

The later trace locked the beginning of client TRN at 0.91--0.92 correlation
but never validated Ja. The lock quality rules out a missing random-training
receiver; the remaining timing assumption was wrong. The analogue modem owns
an independent V.34 symbol clock. At the permitted 100-ppm error, a 12,000T
TRN shifts by 1.2 symbol intervals relative to a fixed `8000/rate` sampler.
The receiver now tests clock hypotheses against consecutive known GPA-TRN
windows and uses the longest correlated prefix to resample/equalize Ja. A
12,000T/100-ppm regression proves full descriptor and CRC recovery. Separately,
Phase-3 Tone-A recognition now requires narrowband coherence. More
importantly, the trained V.34 observer consumes each receive block before the
Tone-A fallback sees it. This ordering prevents the strong 2400-Hz component
of a valid post-Jd S sequence from winning the 50-ms retrain race on the same
block where S becomes decodable.

During any Phase-3 state, a narrowband 2400-Hz Tone A held for at least 50 ms
is treated as the client's retrain request. The hold test is a single coherent
400-sample observation spanning RTP packets; consecutive packet-local spectral
hits no longer count as one continuous tone. The digital modem stops the
current PCM sequence, transmits 70 ms of silence, then Tone B and returns to
the ranging exchange, following V.90 9.3.1 and 9.5.1.2.

The post-Jd receive gate now remains active and detects the V.34 S signal by
its alternating +90/-90-degree differential phase. It searches coherent 32T,
48T and 64T sub-windows inside the specified 128T S burst so a short splitter
echo at the burst boundary cannot reject the whole signal; an unmodulated Tone
A does not satisfy that alternating pattern. On S, the transmitter completes the current Jd,
sends 12T Jd-bar, and emits the exact DIL described by Ja (including N>0,
SP/TP, H, REF, and per-segment training Ucodes). A later S/S-bar completes the
current DIL segment and advances to Phase 4 Ri.

V.90 9.3.1.5 bounds the Jd wait from the beginning of TRN1d, while 9.3.2.10
bounds the later DIL-completion S. The live state machine now reports its best
S correlation while waiting and performs a clean full retrain if Jd, DIL, or
CPt does not complete; none of these states can repeat forever.

V.90 9.4.1.1-9.4.1.2 requires the digital modem to keep Ri active until it
receives CPt, then send 24T Ri-bar followed by at least 2040T TRN2d. The live
receiver now continues with the trained upstream carrier/symbol clock,
differentially decodes and GPA-descrambles repeated 4-point CPt, derives the
variable length from its constellation indices, excludes every start/fill bit
from the V.34 CRC calculation, and accepts only a complete valid sequence. The
six selected codec constellations, K/S, look-ahead, and signed Q1.6 spectral
filter coefficients feed a freshly initialized PCM mapper. The server sends
24T Ri-bar and 2400T of actual GPC-scrambled TRN2d; it no longer substitutes
unparameterized random PCM.

The August 15 hardware trace then proved that Jd, Jd-bar, all 119 requested DIL
segments, and the subsequent S/S-bar were completing, while CPt alone timed
out. Code-path comparison found that Ja used the seven-tap equalizer learned
from the caller's known TRN but CPt rebuilt raw baseband symbols and discarded
that line estimate. CPt now resamples at the retained caller clock and applies
the same complex gain and seven-tap equalizer before differential decisions.
Regression coverage applies one consistent delayed echo to TRN and CPt and
checks both the 292-bit minimum sequence and the 1788-bit maximum sequence.
No CRC or framing tolerance was weakened.

The next trace showed a retained equalizer and good QPSK decision quality but no
fixed CPt header. Visual inspection of V.90 8.6.4 and 9.4.1 (where text
extraction loses the overbar) resolved the sequence: initial Ri is ordinary R
`+++---` at UINFO and repeats for at least 192T. Only after receiving CPt does
the digital modem transmit 24T of Ri-bar `---+++`, followed by TRN2d. The state
machine had sent Ri-bar from the start, leaving the analogue modem in SCR. All
initial and repeated Ri blocks now use ordinary R; the post-CPt block remains
Ri-bar. TRN2d remains 2400T (750 ms at 3200 symbols/s), above the 2040T minimum
in V.90 9.4.1.2.

The 17:45 trace reached this same CPt receiver with a stable 0.28-radian
decision error but still never found its fixed header. Direct inspection of
Table 14 found a deterministic discriminator error: bit 19 is `0` for CPt and
`1` for ordinary CP, while both the receiver and its synthetic test generator
used the reverse. The receiver and generator now use the specified polarity,
and a negative regression verifies that an ordinary CP frame is not accepted
as CPt.

The 18:02 trace did not reach CPt: both attempts timed out while waiting for
the second S/S-bar that terminates DIL. On this same line the validated post-Jd
S bursts measured only 0.58 and 0.60, while the DIL-only threshold was 0.62.
The DIL gate now uses the same 0.55 threshold but retains its much longer
64/80/96T coherence requirement; this admits the real burst without restoring
the short-window SCR false positive.

The 18:08 trace then reached Phase 4 consistently and showed a stable
0.30-radian QPSK decision error, but no single CPt copy passed its fixed fields.
Because CPt is repeated with identical descrambled contents, the receiver now
majority-combines at least three aligned legal-length copies. Acceptance still
requires the combined sequence to pass the exact variable-length layout and
the complete V.34 CRC; no CRC tolerance was introduced.

The 18:13 trace confirmed that near-sync/majority recovery recognizes the CPt
header. It also exposed an independent state-machine collision: CPt's valid
2400-Hz energy satisfied the generic 50-ms Tone-A retrain detector before
enough repeats accumulated. Once a CPt header is present, Phase 4 now owns
those samples and the bounded CPt timeout handles any genuine failure.

Offline decoding of the 18:19 WAV recovered 13 identical four-point CPt
records at a 426-bit interval with a matching 0xF245 V.34 CRC. The caller uses
`d=136` but emits only the first fill zero, whereas strict Table 14 framing is
`292+d=428` bits. A guarded `290+d` compatibility path now requires three
aligned, bit-identical, independently header- and CRC-valid copies. The normal
three-fill path remains unchanged.

That offline result also depended on cancelling the locally transmitted
period-6 Ri returned by the analogue hybrid: mean QPSK error fell from 0.335 to
0.165 rad. The live CPt receiver now estimates and subtracts the six residue-
class means from each recent window before retained-equalizer demodulation.

The following trace still contained no CPt and described the preceding random
training as too short. V.90 9.3.2.9 permits the analogue modem to send SCR
during DIL, while 9.3.2.10 requires a later 128T S plus 16T S-bar to terminate
DIL. The general post-Jd detector accepted 32T sub-windows for damaged-line
compatibility, allowing a chance SCR correlation to move the ISP to Ri while
the caller remained in Phase 3. The DIL-specific gate now requires a coherent
96T, 112T, or 128T region; the short-window detector is retained only for the
earlier Jd response.

This is not yet a full V.90/V.92 data connection. The later Phase-4 MP/CP
acknowledgement, Ed/B1d transmitter, live V.34 upstream data receiver,
negotiated data-mode PCM mapping, and high-speed data pump are still required
before the program may truthfully report a 56K CONNECT.

## V.34 fallback feasibility

When INFO1a selects V.34 after this combined V.90/V.34 Phase 2, V.90 directs
the analogue modem to V.34 11.3.1.2 as the answer modem. The server must
therefore become a real V.34 calling modem: receive answer S/S-bar, PP, TRN and
J; transmit its own S/S-bar, PP, TRN and J; then complete Phase 4 TRN/MP/MP'
/E/B1 and the negotiated data pump. It cannot validly jump to a V.22 waveform
inside that call.

The installed SpanDSP exposes V.22bis and V.42 but no V.34 API. Fabrice
Bellard's GPL linmodem was also reviewed because it contains V.34 modulation
research; its own change log says V.34 negotiation is unfinished, its public
test is half-duplex with fixed parameters and no echo canceller, and key Phase
3 transitions are disabled in the source. It is not a safe drop-in fallback.
Until a complete V.34 Phase 3/4 implementation is integrated, this project
continues to stop truthfully on V.34 selection rather than claim a connection
or emit an incompatible fallback waveform.

Normative references:

- ITU-T V.90 (09/1998): https://www.itu.int/rec/T-REC-V.90-199809-I
- ITU-T V.92 (11/2000): https://www.itu.int/rec/T-REC-V.92-200011-I
- ITU-T V.42 (03/2002): https://www.itu.int/rec/T-REC-V.42-200203-I
- ITU-T V.42bis (01/1990): https://www.itu.int/rec/T-REC-V.42bis-199001-I/en
