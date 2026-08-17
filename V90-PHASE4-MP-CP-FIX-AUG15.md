# V.90 Phase-4 MP/CP fix — 2026-08-15

## Failure observed on hardware

The analogue modem successfully completes Phase 3 and sends a CRC-valid CPt. The server decodes:

- CPt length: 290 bits (short-fill interoperability form)
- S = 5
- K = 12
- ld = 1
- a1 = 64, a2 = 0, b1 = 0, b2 = 0
- resulting Phase-4 training rate: 22,666 bit/s

The previous build then sent 24T Ri-bar and 2400T TRN2d, but the `V90Phase4SendTRN2d` state deliberately stopped there. No MP sequence was ever emitted.

In the 2026-08-15 20:20:41 recording, the caller begins a real retrain about two seconds after TRN2d starts: the signal drops to the retrain guard silence and then a stable Tone A appears. This matches ITU-T V.90 section 9.4.1.3: MP must begin within 2000 ms of the beginning of TRN2d.

## Exact code defect

Old `src/live_modem.cpp` behavior:

```cpp
case LiveState::V90Phase4SendTRN2d:
    // ... MP/CP acknowledgement is the next Phase-4 milestone ...
    break;
```

Therefore a standards-compliant caller waited for MP, did not receive it, and retrained. This is not a CPt, equalizer, CRC, or TRN2d-constellation failure.

## Changes in this build

### 1. Type-0 MP framing and CRC

`build_v90_mp0_bits()` constructs a V.90 Table-16 Type-0 MP with:

- 17-one frame sync
- correct start/reserved fields
- maximum upstream rate field
- upstream capability mask
- acknowledge bit for MP/MP'
- V.34 information CRC
- zero fill to the next complete six-PCM-symbol data-frame boundary

### 2. Continuous TRN2d -> MP mapper/scrambler

`V90Phase4DigitalTx` keeps one GPC scrambler and one PCM mapper alive across TRN2d, MP and MP'. This matters for `ld > 0`: the spectral-shaper look-ahead must not be reset at each protocol sequence boundary.

For the captured CPt (`S=5`, `K=12`, `ld=1`), D = 17 bits per six PCM symbols. A Type-0 MP pads to 102 bits, i.e. six mapping frames / 36 PCM symbols.

### 3. Ordinary CP / CP' receive path

The trained Phase-3 QPSK receiver can now be re-armed for V.90 ordinary CP/CP' (Table-14 discriminator bit 19 = 1). It retains full framing and V.34 CRC validation.

- CP with acknowledge=0 causes the server to finish the current MP and switch to MP'.
- CP' with acknowledge=1 is recognized and logged.

### 4. Phase-4 state machine

After CPt the live path now sends:

`24T Ri-bar -> 2400T TRN2d -> MP -> MP -> ...`

After valid CP:

`... current MP -> MP' -> MP' -> ...`

The decoder remains armed for CP/CP' while MP is transmitted. Valid CP framing also has priority over the 2400-Hz Tone-A retrain detector so CP energy is not mistaken for a retrain.

## Test status

Fresh Linux build and CTest:

- `v92_protocol_tests` — PASS
- `v92_tests` — PASS
- `v92_register_selftest` — PASS
- `v92_userspace_ppp_selftest` — PASS

4/4 tests pass.

## Important remaining Phase-4 work

This is deliberately not described as a complete V.90 data connection yet. The patch fixes the observed missing-MP timeout and implements the MP/CP -> MP'/CP' handshake far enough to observe the next real modem transition.

Still missing after a validated CP'/MP' exchange:

- Ed transmit sequence
- B1d generation at the negotiated downstream data rate
- E/B1 receive completion where required
- the complete live V.34 upstream data-mode receive path
- final data-mode/PPP handoff at V.90 rates

The next hardware log should therefore be used to validate CP/CP'. Once CP' is reached, Ed/B1d is the next implementation milestone.
