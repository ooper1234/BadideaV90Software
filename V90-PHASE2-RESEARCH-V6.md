# V.90 Phase-2 research/interoperability fix (v6)

This build corrects two start-up mistakes discovered from a real hardware-modem trace and a closer read of ITU-T V.90.

1. **V.8 PSTN access category** — the server is the V.90 digital modem, so its V.8 JM now advertises `V8_PSTN_ACCESS_DCE_ON_DIGITAL` together with digital PCM-modem availability. The remote CM's PSTN-access and PCM-availability fields are preserved before SpanDSP rewrites the V.8 result into the answer JM.
2. **One 75 ms guard, not two** — SpanDSP's answer-side V.8 state machine already performs the post-CJ 75 ms silence before reporting successful V.8 negotiation. The old wrapper added a second 75 ms silence. v6 begins INFO0d immediately at the Phase-2 boundary.
3. **Declared/transmitted Phase-2 level match** — INFO0d declares -12 dBm0 nominal power. v6 generates INFO0d and Tone B at the corresponding linear sine level rather than the old arbitrary 6500 peak.
4. **Hardware diagnostics** — during V.90 Phase 2 the console periodically reports RMS and coherent 1200/1800/2400-Hz receive levels. This distinguishes no analogue Phase-2 response from an INFO0a decoder miss or a Tone-A detector miss.

Expected successful early trace:

```
V.90 selected by V.8; post-CJ 75 ms guard already complete; remote CM pstn=0x... pcm=0x...; starting INFO0d immediately...
V.90: transmitting standards-shaped INFO0d ...
V.90: CRC-valid analogue INFO0a received ...
V.90: analogue Tone A detected ...
V.90: FIRST Tone A phase reversal detected ...
```

If V.8 reports V.90 but the caller's CM does not advertise analogue PCM-modem availability, v6 falls back to V.22bis and prints the exact remote V.8 values instead of entering an invalid V.90 pair.

If Phase 2 still does not lock, use the `V.90 RX spectrum` lines:
- strong **1800 + 2400 Hz** shortly after the boundary: analogue INFO0a exists; debug INFO0a timing/CRC demodulation;
- strong **2400 Hz** later: Tone A exists; debug reversal detection;
- neither 1800 nor 2400: the analogue modem is not entering/sustaining V.90 Phase 2, so inspect the V.8 pair fields and outgoing INFO0d/media path.

Full V.90 Phase 2 line probing (L1/L2), INFO1, V.34 upstream training, and Phases 3/4 are still unfinished. This build does not claim a 56K data connection.
