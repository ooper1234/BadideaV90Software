# V.90 Phase-4 TRN2d fix (2026-08-15)

This build fixes the failure seen after a CRC-valid 426-bit CPt, where the
analogue modem immediately sent a Tone-A retrain request as TRN2d began.

Changes:

1. CPt bit 128 handling: the PCM mapper now uses the digital-modem transmitter
   constellation family beginning at CPt bit 136.  The optional second family
   describes the corresponding constellation at the codec D/A output and must
   not be used as the digital transmitter's mapper input.
2. Spectral-shaping filter: corrected the Q1.6 recurrence to
   y=x-b1*x1+a1*y1 and v=y-b2*y1+a2*v1.
3. Look-ahead: each trellis depth now uses the p' value belonging to that
   shaping frame instead of reusing the current frame's value for every depth.
4. TRN2d startup: compensates for ld shaping-frame latency so the first
   transmitted TRN2d data frame is generated from the first D scrambled ones,
   rather than leaking zero-history mapper output onto the line.
5. Added regression tests for separate transmitter/codec CPt families and the
   first-frame magnitude rule with ld=1.
6. Phase-4 log now prints S, K, ld and a1/a2/b1/b2 after a valid CPt.

Native Linux build/tests in the repair environment: 4/4 CTest targets passed.
For Windows, run START-V90-TEST.cmd (or START-V92ISP-WINDOWS.cmd); the included
PowerShell build path rebuilds dist\\v92isp-windows.exe from the corrected
sources before starting it.
