# V.90 Phase-4 retrain gate fix — 2026-08-15

Observed on `dialup_2026-08-15_20-56-10.wav`: after a CRC-valid 290-bit CPt and entry into TRN2d/MP, the analogue modem initiates a retrain (brief silence followed by sustained 2400-Hz Tone A). The server remained in `V90Phase4SendMP`.

The defect was in `LiveModem::receive_pcm()`: the sticky, header-only `cpt_header_seen` observation was treated as an accepted phase transition in `V90Phase4SendTRN2d` and `V90Phase4SendMP`. That cleared the 50-ms Tone-A history on every receive packet even when no CRC-valid ordinary CP had been decoded, masking a genuine retrain indefinitely.

Fix: retain the header guard only while waiting for the initial CPt. Once CPt has validated, only a CRC-valid `cpt_detected_new` suppresses retrain handling. Header-only CP/SCR evidence no longer blocks the coherent 50-ms Tone-A detector. Added Phase-4 CP-wait diagnostics showing header state, decision error, and that the retrain detector is armed.

This patch intentionally does not change the TRN2d/MP mapper. The caller's reason for rejecting the downstream Phase-4 waveform is still a separate issue to diagnose after the state machine responds correctly to retrain.
