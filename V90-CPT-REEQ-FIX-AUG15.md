# V.90 Phase-4 CPt adaptive re-equalizer fix — 2026-08-15

Observed hardware symptom:

- Phase 3 TRN/Ja decodes and validates.
- Phase 4 reports `fixed CPt header=seen` with roughly 0.30–0.35 rad QPSK decision error.
- CPt CRC never validates and the call retrains.

Offline replay of `dialup_2026-08-15_19-36-55.wav` recovers a CRC-valid 290-bit short-fill CPt with S=5, K=12, ld=1. The frame is therefore present on the line; the failure is receive DSP, not missing CPt.

Root cause: the Phase-4 receiver previously froze the seven-tap equalizer learned during caller TRN. Phase 4 is a different receive condition because downstream Ri is now active and the analogue hybrid/VoIP echo changes the effective channel. A slightly stale TRN equalizer can still expose the CPt sync/header while making enough systematic QPSK errors to invalidate every CRC.

Fix:

- Keep the retained TRN equalizer as the first decode path.
- Add a CPt-only adaptive QPSK re-equalizer initialized from those retained taps.
- Use constant-modulus passes followed by fourth-power phase estimation and normalized decision-directed LMS refinement.
- Continue to require the complete CPt framing and V.34 CRC; adaptive equalization does not relax protocol validation.
- Add a regression where the Phase-4 echo path differs from the Phase-3 training path. The old frozen-equalizer code reproduces `header seen / ~0.38 rad / no CRC`; the repaired decoder validates the 290-bit CPt.

The diagnostic line now includes `adaptive CPt re-equalizer=enabled` so a live log confirms that this build is running.
