# V.90 Phase-2 continuation (v7)

This milestone starts from the first Phase-2 ranging exchange already proven on a real analogue modem (CRC-valid INFO0a, Tone A, first and second phase reversals).

Added in v7:

- Receive the remote V.34 L1/L2 line probe after the first RTT/ranging exchange.
- Detect the L1 -> L2 power transition and report measured probe levels.
- Run the second Tone A / Tone B phase-reversal exchange.
- Transmit the digital modem's V.34 L1 for 160 ms followed by L2.
- Build/transmit a CRC-valid 109-bit V.90 INFO1d frame.
- Receive and CRC-check the analogue modem's 70-bit INFO1a frame.
- Report whether INFO1a requests V.90 Phase 3 or V.34 fallback.
- Preserve the stable V.22/V.22bis -> PPP -> NAT fallback path.

This is still not a 56K data connection. A real `V90Phase2Complete` result proves Phase 2 and hands the implementation to the remaining V.90 Phase-3/Phase-4 DSP.
