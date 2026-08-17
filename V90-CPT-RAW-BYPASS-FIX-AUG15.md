# V.90 Phase-4 CPt raw-equalizer bypass fix — 2026-08-15

## Hardware symptom

The 21:31 capture reached `V90Phase4WaitCPt`. The live receiver repeatedly
reported a CPt header and eventually a very low nominal QPSK decision error
(~0.08 rad), but never validated CPt and timed out into a retrain.

Offline replay of the Phase-4 section through the same framing/CRC code found
CRC-valid **426-bit short-fill CPt** frames with `S=5`, `K=12`, `ld=1`, and
`a1=64`. Thus the caller was not sending SCR and the CPt framing/CRC parser was
not the failing layer.

## Exact code defect

`V90Phase3AnalogueRx::try_detect_cpt()` had only two symbol candidates:

1. the retained seven-tap equalizer trained on Phase-3 TRN; and
2. an adaptive CPt equalizer **seeded from those same retained taps**.

A Phase-4 channel change can therefore make both branches fail together. The
adaptive branch may converge to a low-error constant-modulus QPSK solution
that is not the transmitted differential bit stream, so its low average phase
error is not proof of a valid CPt.

## Fix

A third candidate now demodulates the directly sampled RRC output without the
stale TRN inverse. Differential QPSK needs phase transitions, not amplitude
normalization, so this branch is valid as an independent fallback. It cannot
advance the state machine unless the full CP/CPt fixed fields, variable-length
layout, descrambling, and V.34 CRC all pass.

The CPt diagnostic was also corrected so the displayed decision error is only
updated by a candidate that actually produced a plausible CP/CPt header.

## Validation

- Offline replay of the 21:31 capture recovers CRC-valid 426-bit CPt through
  the raw path.
- A deliberately poisoned retained-equalizer replay still recovers the same
  426-bit frame through the new independent branch.
- Portable test suite: 4/4 tests pass.
