# V.90 Phase-2 interoperability fix (v5)

This build corrects the first real-hardware V.90 Phase-2 trace.

Changes:

- INFO0d now includes the required preceding arbitrary-phase modulation point before bit 0.
- Corrected INFO0d bit 19: zero disallows V.34 3429-symbol/s transmission; one allows it.
- Added a complete 49-bit INFO0a receiver with arbitrary sample-timing search and V.34 CRC validation.
- INFO0a acquisition coherently detects the 2400-Hz DPSK carrier while rejecting its 1800-Hz guard tone.
- Replaced RTP-block-to-block Tone-A reversal detection with a sliding coherent phase detector, so a reversal may occur anywhere inside a media packet.
- Removed the non-standard 8-second first-Tone-A-reversal fallback in explicit V.90 mode. ITU-T V.90 recovery requires the digital modem to continue Tone B until the reversal is detected.
- Added regression tests for guard-tone INFO0a acquisition, arbitrary INFO start offset, arbitrary Tone-A reversal offset, CRC, and INFO0d framing.

This does not claim full V.90 data mode. After Phase-2 ranging succeeds, L1/L2 probing, INFO1 exchange, V.34 upstream training, V.90 Phase 3/4, and rate selection still need completion.
