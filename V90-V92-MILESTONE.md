# V.90 / V.92 high-speed DSP milestone

This release starts the **real high-speed modem path** without claiming that a
56K data session is complete.

## MODE=v90

The server acts as the digitally connected V.90 modem over the existing
PCMU/G.711 media path.

Implemented in this milestone:

- V.8 advertises digital V.90 capability only in explicit `MODE=v90`.
- V.22 remains a real fallback and the stable `MODE=auto` behavior is unchanged.
- 75 ms V.90 Phase-2 transition silence.
- INFO0d builder with V.34 CRC (`x^16 + x^12 + x^5 + 1`).
- INFO0d transmission at 600 bit/s binary DPSK on the 1200-Hz digital-modem
  INFO carrier.
- 2400-Hz analogue INFO/Tone-A observation.
- Tone-A phase-reversal detection.
- 1200-Hz Tone-B generation, including the 40 ms delayed phase reversal and
  10 ms post-reversal hold used by V.90 ranging.
- Detection of the second Tone-A reversal and an explicit Phase-2/ranging
  milestone event.
- Both L1/L2 exchanges and CRC-valid INFO1d/INFO1a handling.
- ITU numeric INFO fields serialized least-significant bit first.
- Bounded recovery for a missed client Tone-A reversal and INFO1a.
- Post-INFO1a digital Phase-3 transmitter: exact 384T Sd, 48T S-bar-d,
  4800T GPC-scrambled TRN1d and repeating CRC-valid Jd frames using the UINFO,
  symbol rate and MD duration selected by the hardware modem.
- V.90 PCM algebra core mapping data bits into six G.711 PCM amplitudes. The
  unit test uses a 40 kbit/s error-free laboratory mapping configuration.
- Complete Ja/DIL receive, post-Jd S/S-bar transitions, and Phase-4 Ri.
- Variable-length CRC-valid CPt receive followed by 24T Ri-bar and 2400T
  CPt-parameterized final TRN2d.

Still required before a real V.90 `CONNECT 56K` is allowed:

- live V.34 upstream data receiver after start-up;
- completion of Phase 4 after TRN2d (MP/CP acknowledgement, Ed/B1d and rate
  negotiation);
- wire the later data-mode CP parameters into the PCM mapper;
- error-control/data path interoperability at the selected high-speed rate.

Until those pieces are completed, the code **never reports V.90 data connected**.
If INFO1a selects V.34 or retry limits are exhausted, the high-speed handshake
stops cleanly. Starting V.22 in the middle of that exchange is not a compatible
fallback; a new call is required to negotiate a lower modulation.

## MODE=v92

The existing V.92 Quick Connect implementation remains an explicit laboratory
path. It has real QC1a/QCA1d, QTS/QTS-bar and ANSpcm/TONEq, followed by the
V.92 short Phase-2 INFO0/Tone-A/Tone-B/INFO1a sequence. It does **not** yet have the V.92 PCM-upstream data
pump, Modem-on-Hold data path, or a complete reconnect-to-data sequence.

## Launchers

- `START-V90-TEST.cmd` — select `MODE=v90`, build/start the normal Windows server.
- `START-V92-TEST.cmd` — select `MODE=v92`.
- `START-STABLE-AUTO.cmd` — return to the proven V.22bis/V.22/V.21 path.
